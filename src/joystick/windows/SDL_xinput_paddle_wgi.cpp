/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "SDL_xinput_paddle_wgi.h"
#include "SDL_xinput_paddle_trace.h"
#include "SDL_xinput_paddle_decode.h"
#include <windows.gaming.input.h>
#include <windows.gaming.input.custom.h>
#include <windows.gaming.input.preview.h>
#include <roapi.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <process.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>

static_assert(sizeof(void*) == 8, "The qualified WGI contract is x64 only");

namespace sdl_paddles {
namespace {
namespace custom = ABI::Windows::Gaming::Input::Custom;
namespace input = ABI::Windows::Gaming::Input;
namespace preview = ABI::Windows::Gaming::Input::Preview;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
using Status = GipEnableStatus;
constexpr unsigned CatalogLimit = 16, RequestLimit = 20, QueueLimit = 16, WorkerLimit = 4;
constexpr std::uint64_t DeadlineMs = 10000;
constexpr HRESULT TimeoutError = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
constexpr HRESULT RetiredError = HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
constexpr HRESULT FullError = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);

// Qualified aggregation prefix, also exercised by the existing WGI inner.
MIDL_INTERFACE("06e58977-7684-4dc5-bad1-cda52a4aa06d") ControllerInitialize : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Initialize(IInspectable*, custom::IGameControllerProvider*) = 0;
};

struct Guard {
    SRWLOCK& lock;
    explicit Guard(SRWLOCK& value) noexcept : lock(value) { AcquireSRWLockExclusive(&lock); }
    ~Guard() { ReleaseSRWLockExclusive(&lock); }
};

bool ParseProviderId(const wchar_t* text, unsigned size, std::uint64_t& value) noexcept
{
    value = 0;
    if (!text || size != 20 || text[0] != L'G' || text[1] != L'I' || text[2] != L'P' || text[3] != L':') return false;
    for (unsigned i = 4; i < size; ++i) {
        unsigned digit;
        if (text[i] >= L'0' && text[i] <= L'9') digit = text[i] - L'0';
        else if (text[i] >= L'A' && text[i] <= L'F') digit = text[i] - L'A' + 10;
        else if (text[i] >= L'a' && text[i] <= L'f') digit = text[i] - L'a' + 10;
        else { value = 0; return false; }
        value = (value << 4) | digit;
    }
    return value != 0;
}

bool Eligible(USHORT vendor, USHORT product) noexcept
{
    return vendor == 0x045e && (product == 0x02e3 || product == 0x0b00 || product == 0x0b05 || product == 0x0b22);
}

struct Ticket { unsigned index = CatalogLimit; std::uint64_t serial = 0; };
struct CatalogCell {
    std::uint64_t serial = 0, nativeId = 0;
    std::uint64_t inputEpoch = 0, lifecycleTime = 0, resumeTime = 0, normalTime = 0, splitTime = 0;
    unsigned borrowers = 0;
    bool occupied = false, reserved = false, innerAlive = false;
    bool active = false, closing = false, releasing = false;
    bool lifecycleSeen = false, resumed = false, normalSeen = false, splitSeen = false, everReady = false;
    // Attempt 1 needs this provider's own normal frame in the current epoch.
    // A re-send needs the provider to have been ready once and its epoch kept.
    bool Admits(std::uint64_t attempt) const noexcept {
        return attempt == 1 ? resumed && normalSeen : everReady;
    }
    custom::IGipGameControllerProvider* provider = nullptr; // Owned, released off the lock.
    IUnknown* identity = nullptr; // Owned controlling identity, not the inner.
};
struct Request {
    std::uint64_t token = 0, nativeId = 0, generation = 0, serial = 0, submitted = 0;
    std::uint64_t epoch = 0, attempt = 0;
    GipEnableResult result{Status::Pending, E_PENDING};
    bool working = false;
    bool dispatched = false;
    std::array<BYTE, 2> request{0x07, 0x00};
    std::array<BYTE, 64> response{};
};
struct Job {
    unsigned request = RequestLimit;
    Ticket catalog;
    std::uint64_t token = 0;
    custom::IGipGameControllerProvider* provider = nullptr; // Protected by catalog lease.
    std::uint64_t epoch = 0;
};

enum class InputKind { Resumed, Suspended, Message, Key };

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE changed = CONDITION_VARIABLE_INIT;
    std::array<CatalogCell, CatalogLimit> catalog{};
    std::array<Request, RequestLimit> requests{};
    std::uint64_t nextToken = 0, nextSerial = 0;
    HRESULT registrationError = E_PENDING;
    bool registrationDone = false;

    CatalogCell* Cell(Ticket t) noexcept {
        if (t.index >= catalog.size()) return nullptr;
        auto& c = catalog[t.index];
        return c.occupied && c.serial == t.serial ? &c : nullptr;
    }
    void InvalidateInput(CatalogCell& cell) noexcept {
        for (auto& r : requests) {
            if (r.token && r.serial == cell.serial &&
                (r.epoch != cell.inputEpoch || !cell.Admits(r.attempt))) r.result = {Status::Retired, RetiredError};
        }
    }
    void Input(Ticket ticket, InputKind kind, UINT64 timestamp, BYTE messageClass = 0,
               BYTE id = 0, BYTE sequence = 0, UINT32 size = 0, const BYTE* bytes = nullptr) noexcept {
        Guard guard(lock);
        auto* c = Cell(ticket);
        if (!c) return;
        trace::Record record;
        record.nativeId = c->nativeId;
        record.provider = c->serial;
        record.epoch = c->inputEpoch;
        record.sourceTime = timestamp;
        record.report = id;
        record.messageClass = messageClass;
        record.sequence = sequence;
        if (kind == InputKind::Message || kind == InputKind::Key) {
            // Retain the callback's owned bytes before interpreting a report.
            record.kind = kind == InputKind::Key ? trace::Kind::WgiKey : trace::Kind::WgiMessage;
            if (kind == InputKind::Key) record.mask = sequence;
            trace::Raw(record, bytes, size);
        }
        if (c->closing) return;
        if (kind == InputKind::Resumed || kind == InputKind::Suspended) {
            record.kind = kind == InputKind::Resumed ? trace::Kind::WgiResume : trace::Kind::WgiSuspend;
            if ((c->lifecycleSeen && timestamp < c->lifecycleTime) ||
                (c->lifecycleSeen && timestamp == c->lifecycleTime && c->resumed == (kind == InputKind::Resumed))) {
                record.status = S_FALSE;
                trace::Push(record);
                return;
            }
            c->lifecycleSeen = true;
            c->lifecycleTime = timestamp;
            c->resumed = kind == InputKind::Resumed;
            c->normalSeen = c->splitSeen = false;
            c->normalTime = c->splitTime = 0;
            if (c->resumed) {
                if (c->inputEpoch == UINT64_MAX) c->resumed = false;
                else ++c->inputEpoch;
                c->resumeTime = timestamp;
                // Re-send permission belongs to one epoch. A suspend keeps it.
                c->everReady = false;
            }
            InvalidateInput(*c);
            record.epoch = c->inputEpoch;
            trace::Push(record);
        } else if (kind == InputKind::Message && bytes) {
            const bool normal = messageClass == custom::GipMessageClass_LowLatency && id == 0 &&
                                SDL_XInputPaddleNormalSizeValid(size);
            // The proven initializer requires this provider's own normal frame.
            // It also covers initial delivery when no resume callback preceded it.
            if (normal && !c->lifecycleSeen && !c->inputEpoch) {
                c->inputEpoch = 1;
                c->resumed = true;
                c->resumeTime = timestamp;
            }
            // The native dispatcher serializes these callbacks. It can sample
            // a message timestamp before synthesizing the preceding resume,
            // so callback order defines readiness, not timestamp comparison.
            if (c->resumed) {
                if (normal) {
                    c->normalSeen = true;
                    c->everReady = true;
                    c->normalTime = (std::max)(c->normalTime, timestamp);
                }
                if (messageClass == custom::GipMessageClass_Command && id == 0x0c && size == 17) {
                    c->splitSeen = true;
                    c->splitTime = (std::max)(c->splitTime, timestamp);
                }
            }
        }
        WakeAllConditionVariable(&changed);
    }
    bool InputState(std::uint64_t nativeId, GipInputState& result) noexcept {
        result = {};
        if (!nativeId || !TryAcquireSRWLockExclusive(&lock)) return false;
        Ticket ticket;
        const auto matches = Matches(nativeId, ticket);
        if (matches == 1) {
            const auto& c = catalog[ticket.index];
            result.provider = c.serial;
            result.epoch = c.inputEpoch;
            result.resumeTime = c.resumeTime;
            result.normalTime = c.normalTime;
            result.splitTime = c.splitTime;
            result.resumed = c.resumed;
            result.normalSeen = c.normalSeen;
            result.splitSeen = c.splitSeen;
            result.everReady = c.everReady;
            result.busy = c.borrowers != 0;
        } else if (matches > 1) result.error = HRESULT_FROM_WIN32(ERROR_DUP_NAME);
        else if (registrationDone && FAILED(registrationError)) result.error = registrationError;
        ReleaseSRWLockExclusive(&lock);
        return true;
    }
    void Expire(std::uint64_t now) noexcept {
        for (auto& r : requests) {
            if (r.token && r.result.status == Status::Pending && now - r.submitted >= DeadlineMs)
                r.result = {Status::TimedOut, TimeoutError};
        }
    }
    void Close(CatalogCell& c) noexcept {
        if (c.closing) return;
        c.active = false;
        c.closing = true;
        for (auto& r : requests) {
            if (r.token && r.nativeId == c.nativeId && (r.serial == c.serial || !r.serial))
                r.result = {Status::Retired, RetiredError};
        }
    }
    Ticket Reserve() noexcept {
        Guard guard(lock);
        if (nextSerial == UINT64_MAX) return {};
        for (unsigned i = 0; i < catalog.size(); ++i) {
            if (!catalog[i].occupied) {
                catalog[i] = {};
                catalog[i].occupied = catalog[i].reserved = true;
                catalog[i].serial = ++nextSerial;
                return {i, nextSerial};
            }
        }
        return {};
    }
    void CancelReserve(Ticket t) noexcept {
        Guard guard(lock);
        if (auto* c = Cell(t); c && c->reserved) *c = {};
    }
    bool Install(Ticket t, std::uint64_t nativeId, custom::IGipGameControllerProvider* provider) noexcept {
        Guard guard(lock);
        auto* c = Cell(t);
        if (!c || !c->reserved) return false;
        c->nativeId = nativeId;
        c->provider = provider;
        c->innerAlive = true;
        c->reserved = false;
        return true;
    }
    bool BindOuter(Ticket t, IUnknown* identity) noexcept {
        if (!identity) return false;
        Guard guard(lock);
        auto* c = Cell(t);
        if (!c || c->closing || c->identity) return false;
        c->identity = identity;
        return true;
    }
    void Added(IUnknown* identity) noexcept {
        if (!identity) return;
        Guard guard(lock);
        for (auto& c : catalog) if (c.occupied && !c.closing && c.identity == identity) c.active = true;
        WakeAllConditionVariable(&changed);
    }
    void Removed(IUnknown* identity) noexcept {
        if (!identity) return;
        Guard guard(lock);
        for (auto& c : catalog) if (c.occupied && c.identity == identity) Close(c);
        WakeAllConditionVariable(&changed);
    }
    void InnerGone(Ticket t) noexcept {
        Guard guard(lock);
        if (auto* c = Cell(t)) { c->innerAlive = false; Close(*c); }
        WakeAllConditionVariable(&changed);
    }
    bool CleanupOne() noexcept {
        Ticket t;
        IUnknown* identity = nullptr;
        custom::IGipGameControllerProvider* provider = nullptr;
        {
            Guard guard(lock);
            for (unsigned i = 0; i < catalog.size(); ++i) {
                auto& c = catalog[i];
                if (c.occupied && c.closing && !c.reserved && !c.releasing && !c.borrowers &&
                    (c.identity || c.provider || !c.innerAlive)) {
                    c.releasing = true;
                    t = {i, c.serial};
                    identity = c.identity; c.identity = nullptr;
                    provider = c.provider; c.provider = nullptr;
                    break;
                }
            }
        }
        if (!t.serial) return false;
        if (identity) identity->Release();
        if (provider) provider->Release();
        {
            Guard guard(lock);
            if (auto* c = Cell(t)) {
                c->releasing = false;
                if (!c->innerAlive) *c = {};
            }
        }
        return true;
    }
    unsigned Matches(std::uint64_t id, Ticket& ticket) noexcept {
        unsigned count = 0;
        for (unsigned i = 0; i < catalog.size(); ++i) {
            const auto& c = catalog[i];
            if (c.occupied && c.active && !c.closing && c.nativeId == id) { ticket = {i,c.serial}; ++count; }
        }
        return count;
    }
    std::uint64_t Submit(std::uint64_t id, std::uint64_t generation, std::uint64_t providerEpoch,
                         std::uint64_t inputEpoch, std::uint64_t attempt, std::uint64_t now, bool exhausted) noexcept {
        if (!id || !generation || !providerEpoch || !inputEpoch || !attempt || !TryAcquireSRWLockExclusive(&lock)) return 0;
        Expire(now);
        unsigned queued = 0;
        Request* available = nullptr;
        Request* retry = nullptr;
        for (auto& r : requests) {
            if (r.token && r.nativeId == id && r.generation == generation && r.serial == providerEpoch &&
                r.epoch == inputEpoch && r.attempt == attempt) {
                if (!r.working && !r.dispatched &&
                    (r.result.status == Status::TimedOut || r.result.status == Status::Exhausted)) {
                    retry = &r; // No native attempt occurred in this input epoch.
                } else {
                    const auto token = r.token; ReleaseSRWLockExclusive(&lock); return token;
                }
            }
            if (r.token && r.result.status == Status::Pending && !r.working) ++queued;
            if (!r.working && (!r.token || r.result.status != Status::Pending) &&
                (!available || r.token < available->token)) available = &r;
        }
        if (retry) available = retry;
        if (!available || queued >= QueueLimit || nextToken == UINT64_MAX) { ReleaseSRWLockExclusive(&lock); return 0; }
        Ticket ticket;
        const unsigned matches = Matches(id, ticket);
        const auto* current = matches == 1 ? Cell(ticket) : nullptr;
        if (!current || current->serial != providerEpoch || current->inputEpoch != inputEpoch ||
            !current->Admits(attempt) || current->borrowers) {
            ReleaseSRWLockExclusive(&lock);
            return 0;
        }
        *available = {};
        available->token = ++nextToken;
        available->nativeId = id;
        available->generation = generation;
        available->epoch = inputEpoch;
        available->attempt = attempt;
        available->serial = providerEpoch;
        available->submitted = now;
        available->response.fill(0xff);
        if (exhausted) available->result = {Status::Exhausted, FullError};
        else if (registrationDone && FAILED(registrationError)) available->result = {Status::Failed, registrationError};
        const auto token = available->token;
        trace::Record record;
        record.kind = trace::Kind::CommandQueued;
        record.nativeId = id; record.attachment = generation; record.provider = providerEpoch;
        record.epoch = inputEpoch; record.command = token; record.sequence = attempt;
        record.messageClass = custom::GipMessageClass_StandardLatency; record.report = 0x0d;
        trace::Push(record);
        ReleaseSRWLockExclusive(&lock);
        WakeAllConditionVariable(&changed);
        return token;
    }
    bool Poll(std::uint64_t token, GipEnableResult& result, std::uint64_t now) noexcept {
        if (!token || !TryAcquireSRWLockExclusive(&lock)) return false;
        Expire(now);
        bool found = false;
        for (auto& r : requests) if (r.token == token) {
            result = r.result; result.provider = r.serial; result.epoch = r.epoch; result.attempt = r.attempt;
            result.dispatched = r.dispatched;
            found = true; break;
        }
        ReleaseSRWLockExclusive(&lock);
        return found;
    }
    void Retire(std::uint64_t generation) noexcept {
        if (!generation) return;
        Guard guard(lock);
        for (auto& r : requests) if (r.token && r.generation == generation) r.result = {Status::Retired, RetiredError};
        WakeAllConditionVariable(&changed);
    }
    Job Claim(std::uint64_t now) noexcept {
        Guard guard(lock);
        Expire(now);
        Job job;
        for (unsigned i = 0; i < requests.size(); ++i) {
            auto& r = requests[i];
            if (!r.token || r.working || r.result.status != Status::Pending) continue;
            Ticket t;
            const unsigned matches = Matches(r.nativeId,t);
            if (matches > 1) { r.result = {Status::Failed,HRESULT_FROM_WIN32(ERROR_DUP_NAME)}; continue; }
            if (!matches) continue;
            const auto& c = catalog[t.index];
            if (r.serial != t.serial || r.epoch != c.inputEpoch || !c.Admits(r.attempt)) {
                r.result = {Status::Retired,RetiredError}; continue;
            }
            if (c.borrowers) continue;
            if (job.token && job.token < r.token) continue;
            job = {i,t,r.token,catalog[t.index].provider,r.epoch};
        }
        if (job.token) {
            auto& r = requests[job.request];
            r.serial = job.catalog.serial;
            r.working = true;
            ++catalog[job.catalog.index].borrowers;
        }
        return job;
    }
    bool Authorize(const Job& job, std::uint64_t now) noexcept {
        Guard guard(lock);
        Expire(now);
        auto& r = requests[job.request];
        const auto* c = Cell(job.catalog);
        Ticket current;
        const bool allowed = r.token == job.token && r.working && !r.dispatched && r.result.status == Status::Pending &&
               c && c->active && !c->closing && c->Admits(r.attempt) && c->inputEpoch == job.epoch &&
               Matches(r.nativeId,current) == 1 && current.serial == job.catalog.serial;
        if (allowed) {
            r.dispatched = true;
            trace::Record record;
            record.kind = trace::Kind::CommandDispatch;
            record.nativeId = r.nativeId; record.attachment = r.generation; record.provider = r.serial;
            record.epoch = r.epoch; record.command = r.token; record.sequence = r.attempt; record.sourceTime = c->normalTime;
            record.messageClass = custom::GipMessageClass_StandardLatency; record.report = 0x0d;
            trace::Raw(record, r.request.data(), r.request.size());
        }
        return allowed;
    }
    void Complete(const Job& job, HRESULT hr, std::uint64_t now) noexcept {
        Guard guard(lock);
        Expire(now);
        auto& r = requests[job.request];
        auto* c = Cell(job.catalog);
        if (r.token == job.token) {
            Ticket current;
            if (r.result.status == Status::Pending) {
                const bool valid = c && c->active && !c->closing && (r.attempt == 1 ? c->resumed : c->everReady) &&
                    c->inputEpoch == job.epoch && Matches(r.nativeId,current) == 1 && current.serial == job.catalog.serial;
                r.result = valid ? GipEnableResult{SUCCEEDED(hr) ? Status::Success : Status::Failed,hr} : GipEnableResult{Status::Retired,RetiredError};
            }
            r.working = false;
            trace::Record record;
            record.kind = trace::Kind::CommandComplete;
            record.nativeId = r.nativeId; record.attachment = r.generation; record.provider = r.serial;
            record.epoch = r.epoch; record.command = r.token; record.sequence = r.attempt; record.status = hr;
            record.mask = r.dispatched ? 1 : 0;
            trace::Raw(record, r.response.data(), r.response.size());
        }
        if (c && c->borrowers) --c->borrowers;
        WakeAllConditionVariable(&changed);
    }
};

class Inner final : public ControllerInitialize {
    template<class Interface> struct Sink : Interface {
        Inner& owner;
        explicit Sink(Inner& value) noexcept : owner(value) {}
        IInspectable* Outer() const noexcept { return owner.outer.load(std::memory_order_acquire); }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) noexcept override { return Outer()->QueryInterface(id,value); }
        ULONG STDMETHODCALLTYPE AddRef() noexcept override { return Outer()->AddRef(); }
        ULONG STDMETHODCALLTYPE Release() noexcept override { return Outer()->Release(); }
        HRESULT STDMETHODCALLTYPE GetIids(ULONG* n, IID** ids) noexcept override { return Outer()->GetIids(n,ids); }
        HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* value) noexcept override { return Outer()->GetRuntimeClassName(value); }
        HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* value) noexcept override { return Outer()->GetTrustLevel(value); }
    };
    struct InputSink final : Sink<custom::IGameControllerInputSink> {
        using Sink::Sink;
        HRESULT STDMETHODCALLTYPE OnInputResumed(UINT64 timestamp) noexcept override { return owner.Observe(InputKind::Resumed, timestamp); }
        HRESULT STDMETHODCALLTYPE OnInputSuspended(UINT64 timestamp) noexcept override { return owner.Observe(InputKind::Suspended, timestamp); }
    } inputSink{*this};
    struct GipSink final : Sink<custom::IGipGameControllerInputSink> {
        using Sink::Sink;
        HRESULT STDMETHODCALLTYPE OnKeyReceived(UINT64 timestamp, BYTE key, boolean pressed) noexcept override {
            return owner.Observe(InputKind::Key, timestamp, 0, key, pressed ? 1 : 0);
        }
        HRESULT STDMETHODCALLTYPE OnMessageReceived(UINT64 timestamp, custom::GipMessageClass messageClass,
            BYTE id, BYTE sequence, UINT32 size, BYTE* bytes) noexcept override {
            return owner.Observe(InputKind::Message, timestamp, static_cast<BYTE>(messageClass), id, sequence, size, bytes);
        }
    } gipSink{*this};
    State& state;
    Ticket ticket;
    std::atomic<ULONG> refs{1};
    std::atomic<IInspectable*> outer{nullptr};
    std::atomic<bool> initialized{false};
    HRESULT Observe(InputKind kind, UINT64 timestamp, BYTE messageClass = 0, BYTE id = 0,
                    BYTE sequence = 0, UINT32 size = 0, const BYTE* bytes = nullptr) noexcept {
        struct CallHold {
            IInspectable* value;
            explicit CallHold(IInspectable* object) noexcept : value(object) { value->AddRef(); }
            ~CallHold() { value->Release(); }
        } hold(outer.load(std::memory_order_acquire));
        // The service remains the sole USB publisher. This callback supplies
        // provider-local readiness and a passive, owned diagnostic copy.
        state.Input(ticket, kind, timestamp, messageClass, id, sequence, size, bytes);
        return size && !bytes ? E_POINTER : S_OK;
    }
public:
    Inner(State& value, Ticket cell) noexcept : state(value), ticket(cell) {}
    ~Inner() { state.InnerGone(ticket); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IInspectable) || id == __uuidof(ControllerInitialize)) {
            *value = static_cast<ControllerInitialize*>(this); AddRef(); return S_OK;
        }
        IInspectable* sink = nullptr;
        if (id == __uuidof(custom::IGameControllerInputSink)) sink = &inputSink;
        else if (id == __uuidof(custom::IGipGameControllerInputSink)) sink = &gipSink;
        if (!sink) return E_NOINTERFACE;
        if (!initialized.load(std::memory_order_acquire)) return E_UNEXPECTED;
        sink->AddRef(); *value = sink; return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return refs.fetch_add(1,std::memory_order_relaxed)+1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG left = refs.fetch_sub(1,std::memory_order_acq_rel)-1;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** ids) noexcept override {
        if (count) *count = 0;
        if (ids) *ids = nullptr;
        if (!count || !ids) return E_POINTER;
        const IID supported[] = {__uuidof(ControllerInitialize),__uuidof(custom::IGameControllerInputSink),__uuidof(custom::IGipGameControllerInputSink)};
        *ids = static_cast<IID*>(CoTaskMemAlloc(sizeof(supported)));
        if (!*ids) return E_OUTOFMEMORY;
        std::memcpy(*ids,supported,sizeof(supported)); *count = 3; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* value) noexcept override { if (!value) return E_POINTER; *value = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* value) noexcept override { if (!value) return E_POINTER; *value = BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(IInspectable* value, custom::IGameControllerProvider*) noexcept override {
        if (!value) return E_POINTER;
        ComPtr<IUnknown> identity;
        HRESULT hr = value->QueryInterface(IID_PPV_ARGS(&identity));
        if (FAILED(hr)) return hr;
        if (!identity) return E_UNEXPECTED;
        IInspectable* expected = nullptr;
        if (!outer.compare_exchange_strong(expected,value)) return E_UNEXPECTED;
        if (!state.BindOuter(ticket,identity.Get())) return E_UNEXPECTED;
        identity.Detach();
        initialized.store(true,std::memory_order_release);
        return S_OK;
    }
};

#ifdef SDL_XINPUT_PADDLE_WGI_OFFLINE_TEST
HRESULT PublicProviderId(custom::IGameControllerProvider*, HSTRING*) noexcept;
#else
HRESULT PublicProviderId(custom::IGameControllerProvider* provider, HSTRING* value) noexcept
{
    ComPtr<preview::IGameControllerProviderInfoStatics> info;
    HRESULT hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Gaming_Input_Preview_GameControllerProviderInfo).Get(),IID_PPV_ARGS(&info));
    if (FAILED(hr)) return hr;
    return info ? info->GetProviderId(provider,value) : E_UNEXPECTED;
}
#endif

class Factory final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRt>,custom::ICustomGameControllerFactory,Microsoft::WRL::FtmBase> {
    InspectableClass(L"SDL.Private.XInputPaddleFactory",BaseTrust);
    State& state;
    HRESULT Build(Ticket ticket, custom::IGameControllerProvider* provider, IInspectable** value) noexcept {
        USHORT vendor = 0, product = 0;
        HRESULT hr = provider->get_HardwareVendorId(&vendor);
        if (FAILED(hr)) return hr;
        hr = provider->get_HardwareProductId(&product);
        if (FAILED(hr)) return hr;
        if (!Eligible(vendor,product)) return E_NOINTERFACE;
        HString id;
        hr = PublicProviderId(provider,id.GetAddressOf());
        if (FAILED(hr)) return hr;
        UINT32 size = 0;
        const wchar_t* text = id.GetRawBuffer(&size);
        std::uint64_t parsed = 0;
        if (!ParseProviderId(text,size,parsed)) return E_INVALIDARG;
        ComPtr<custom::IGipGameControllerProvider> gip;
        hr = provider->QueryInterface(IID_PPV_ARGS(&gip));
        if (FAILED(hr)) return hr;
        if (!gip) return E_UNEXPECTED;
        auto* inner = new (std::nothrow) Inner(state,ticket);
        if (!inner) return E_OUTOFMEMORY;
        // Matches() compares this parsed public ID with the input owner's ID.
        if (!state.Install(ticket,parsed,gip.Get())) { inner->Release(); return E_UNEXPECTED; }
        gip.Detach();
        *value = inner;
        return S_OK;
    }
public:
    explicit Factory(State& value) noexcept : state(value) {}
    HRESULT STDMETHODCALLTYPE CreateGameController(custom::IGameControllerProvider* provider, IInspectable** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (!provider) return E_INVALIDARG;
        Ticket ticket = state.Reserve();
        if (!ticket.serial) return FullError;
        const HRESULT hr = Build(ticket,provider,value);
        if (FAILED(hr)) state.CancelReserve(ticket);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE OnGameControllerAdded(input::IGameController* controller) noexcept override {
        if (!controller) return E_INVALIDARG;
        ComPtr<IUnknown> identity;
        HRESULT hr = controller->QueryInterface(IID_PPV_ARGS(&identity));
        if (SUCCEEDED(hr)) { if (!identity) return E_UNEXPECTED; state.Added(identity.Get()); }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE OnGameControllerRemoved(input::IGameController* controller) noexcept override {
        if (!controller) return E_INVALIDARG;
        ComPtr<IUnknown> identity;
        HRESULT hr = controller->QueryInterface(IID_PPV_ARGS(&identity));
        if (SUCCEEDED(hr)) { if (!identity) return E_UNEXPECTED; state.Removed(identity.Get()); }
        return hr;
    }
};

template<class Connected, class Send, class Clock>
void Execute(State& state, const Job& job, Connected connected, Send send, Clock now) noexcept
{
    HRESULT hr = E_FAIL;
    try {
        hr = connected(job.provider);
        if (SUCCEEDED(hr)) {
            // This is the dispatch linearization point. Later retirement cannot
            // recall the native call, but it suppresses its completion.
            if (state.Authorize(job,now())) {
                auto& r = state.requests[job.request];
                hr = send(job.provider,r.request,r.response);
            } else hr = RetiredError;
        }
    } catch (...) { hr = E_FAIL; }
    state.Complete(job,hr,now());
}

struct Broker {
    State state;
    struct Worker {
        Broker* owner = nullptr;
        std::atomic<bool> alive{false}, busy{false};
        std::atomic<std::uint64_t> since{0};
    } workers[WorkerLimit];
    std::atomic<bool> registrationClaimed{false};
    custom::ICustomGameControllerFactory* factory = nullptr; // Process-lifetime reference.
    bool Exhausted(std::uint64_t now) noexcept {
        for (auto& w : workers) {
            if (w.alive.load() && (!w.busy.load() || now-w.since.load() < DeadlineMs)) return false;
        }
        return true;
    }
};
std::atomic<Broker*> broker{nullptr};

#ifndef SDL_XINPUT_PADDLE_WGI_OFFLINE_TEST
SRWLOCK startupLock = SRWLOCK_INIT;

void RegisterFactory(Broker& b) noexcept
{
    ComPtr<custom::IGameControllerFactoryManagerStatics> manager;
    HRESULT hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Gaming_Input_Custom_GameControllerFactoryManager).Get(),IID_PPV_ARGS(&manager));
    bool any = false;
    if (SUCCEEDED(hr)) {
        auto factory = Microsoft::WRL::Make<Factory>(b.state);
        if (!factory) hr = E_OUTOFMEMORY;
        else {
            b.factory = factory.Detach();
            constexpr USHORT products[] = {0x02e3,0x0b00,0x0b05,0x0b22};
            for (USHORT product : products) {
                hr = manager->RegisterCustomFactoryForHardwareId(b.factory,0x045e,product);
                if (SUCCEEDED(hr)) any = true;
            }
        }
    }
    Guard guard(b.state.lock);
    b.state.registrationDone = true;
    b.state.registrationError = any ? S_OK : hr;
    if (!any) for (auto& r : b.state.requests)
        if (r.token && r.result.status == Status::Pending) r.result = {Status::Failed,hr};
    WakeAllConditionVariable(&b.state.changed);
}

unsigned __stdcall WorkerMain(void* arg) noexcept
{
    auto& worker = *static_cast<Broker::Worker*>(arg);
    Broker& b = *worker.owner;
    const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(apartment)) { worker.alive.store(false); return 0; }
    struct Apartment { ~Apartment() { RoUninitialize(); } } apartmentLifetime;
    bool expected = false;
    if (b.registrationClaimed.compare_exchange_strong(expected,true)) RegisterFactory(b);
    worker.busy.store(false);
    for (;;) {
        worker.since.store(GetTickCount64());
        worker.busy.store(true);
        if (b.state.CleanupOne()) { worker.busy.store(false); continue; }
        Job job = b.state.Claim(GetTickCount64());
        if (job.token) {
            Execute(b.state,job,
                [](custom::IGipGameControllerProvider* provider) {
                    ComPtr<custom::IGameControllerProvider> base;
                    HRESULT hr = provider->QueryInterface(IID_PPV_ARGS(&base));
                    if (FAILED(hr)) return hr;
                    boolean connected = false;
                    hr = base->get_IsConnected(&connected);
                    return FAILED(hr) ? hr : connected ? S_OK : HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
                },
                [](custom::IGipGameControllerProvider* provider, auto& request, auto& response) {
                    return provider->SendReceiveMessage(custom::GipMessageClass_StandardLatency,0x0d,
                        static_cast<UINT32>(request.size()),request.data(),static_cast<UINT32>(response.size()),response.data());
                }, [] { return GetTickCount64(); });
        }
        worker.busy.store(false);
        if (!job.token) {
            Guard guard(b.state.lock);
            SleepConditionVariableSRW(&b.state.changed,&b.state.lock,50,0);
        }
    }
}

Broker* GetBroker(bool start) noexcept
{
    Broker* value = broker.load(std::memory_order_acquire);
    if (value || !start) return value;
    if (!TryAcquireSRWLockExclusive(&startupLock)) return nullptr;
    value = broker.load(std::memory_order_acquire);
    if (!value) {
        value = new (std::nothrow) Broker;
        HMODULE pinned = nullptr;
        if (value && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
              reinterpret_cast<LPCWSTR>(&SubmitGipEnable),&pinned)) { delete value; value = nullptr; }
        if (value) {
            for (auto& w : value->workers) {
                w.owner = value; w.since.store(GetTickCount64()); w.busy.store(true); w.alive.store(true);
                const uintptr_t thread = _beginthreadex(nullptr,0,WorkerMain,&w,0,nullptr);
                if (thread) CloseHandle(reinterpret_cast<HANDLE>(thread));
                else w.alive.store(false);
            }
            broker.store(value,std::memory_order_release);
        }
    }
    ReleaseSRWLockExclusive(&startupLock);
    return value;
}
#else
// The offline runner must supply a broker. This build cannot activate WGI,
// register a factory, pin a module, or launch the production workers.
Broker* GetBroker(bool) noexcept { return broker.load(); }
#endif
} // namespace

bool QueryGipInputState(std::uint64_t nativeId, GipInputState& state) noexcept
{
    state = {};
    if (!nativeId) { state.error = E_INVALIDARG; return true; }
    auto* b = GetBroker(true);
    if (!b || !b->state.InputState(nativeId, state)) return false;
    if (b->Exhausted(GetTickCount64())) state.error = FullError;
    return true;
}

std::uint64_t SubmitGipEnable(std::uint64_t nativeId, std::uint64_t generation,
                             std::uint64_t provider, std::uint64_t epoch, std::uint64_t attempt) noexcept
{
    if (!nativeId || !generation || !attempt) return 0;
    auto* b = GetBroker(true);
    const auto now = GetTickCount64();
    return b ? b->state.Submit(nativeId,generation,provider,epoch,attempt,now,b->Exhausted(now)) : 0;
}

bool PollGipEnable(std::uint64_t token, GipEnableResult& result) noexcept
{
    auto* b = GetBroker(false);
    return b && b->state.Poll(token,result,GetTickCount64());
}

void RetireGipEnable(std::uint64_t generation) noexcept
{
    if (auto* b = GetBroker(false)) b->state.Retire(generation);
}
} // namespace sdl_paddles
