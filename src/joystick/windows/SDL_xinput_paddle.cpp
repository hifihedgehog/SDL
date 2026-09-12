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

#ifndef SDL_XINPUT_PADDLE_BROKER_TESTING
#include "SDL_internal.h"
#endif

#if defined(SDL_JOYSTICK_XINPUT_PADDLES) || defined(SDL_XINPUT_PADDLE_BROKER_TESTING)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <process.h>
#include <roapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "SDL_xinput_paddle_decode.h"
#include "SDL_xinput_paddle_gatt.h"
#include "SDL_xinput_paddle_identity.h"
#include "SDL_xinput_paddle_runtime.h"
#include "SDL_xinput_paddle_service.h"
#include "SDL_xinput_paddle_wgi.h"
#include "SDL_xinput_paddle_trace.h"
#include "../usb_ids.h"

#ifndef SDL_XINPUT_PADDLE_BROKER_TESTING
extern "C" {
#include "SDL_windowsjoystick_c.h"
#include "../../core/windows/SDL_xinput.h"
#include "../SDL_joystick_c.h"
#include "../SDL_gamepad_c.h"
#include "SDL_xinput_paddle_c.h"
}
#endif

namespace sdl_paddles {
namespace broker_detail {

constexpr size_t RouteLimit = 16;
constexpr size_t EventLimit = 1024;
constexpr size_t TraceLimit = 512;
constexpr size_t PathLimit = 1024;
constexpr std::uint64_t IdentityPeriodMs = 1000;
constexpr std::uint64_t AcquisitionTimeoutMs = 10000;
constexpr std::uint64_t EnableAdmissionRetryMs = 50;

bool Eligible(std::uint16_t vendor, std::uint16_t product) noexcept
{
    return vendor == USB_VENDOR_MICROSOFT &&
           (product == USB_PRODUCT_XBOX_ONE_ELITE_SERIES_1 ||
            product == USB_PRODUCT_XBOX_ONE_ELITE_SERIES_2 ||
            product == USB_PRODUCT_XBOX_ONE_ELITE_SERIES_2_BLUETOOTH ||
            product == USB_PRODUCT_XBOX_ONE_ELITE_SERIES_2_BLE);
}

bool EligiblePhysical(std::uint16_t vendor, std::uint16_t product,
                      const PhysicalIdentity &identity) noexcept
{
    if (IsGipTransport(identity.transport)) {
        return IsGipPaddleHardware(identity.vendor, identity.product);
    }
    return identity.transport == PaddleTransport::Bluetooth && Eligible(vendor, product);
}

std::uint64_t QpcNow() noexcept
{
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) && value.QuadPart > 0 ?
           static_cast<std::uint64_t>(value.QuadPart) : 0;
}

// Divide before multiplying. Frequencies are bounded to UINT32_MAX at startup.
std::uint64_t ScaleClamped(std::uint64_t value, std::uint64_t numerator,
                          std::uint64_t denominator, std::uint64_t ceiling) noexcept
{
    if (!denominator || !numerator) return 0;
    const auto whole = value / denominator;
    if (whole > ceiling / numerator) return ceiling;
    const auto result = whole * numerator;
    const auto fraction = (value % denominator) * numerator / denominator;
    return fraction > ceiling - result ? ceiling : result + fraction;
}

std::uint64_t HistoryQpc(std::uint64_t anchor, std::uint64_t newestUs,
                         std::uint64_t sampleUs, std::uint64_t frequency) noexcept
{
    // SharedReader copies GameInputReading::GetTimestamp's microseconds.
    // Its epoch is private. Only deltas within this view's owned batch are used.
    const auto age = newestUs >= sampleUs ? newestUs - sampleUs : 0;
    return anchor - ScaleClamped(age, frequency, 1000000, anchor);
}

struct Edge {
    std::uint64_t qpc = 0;
    std::uint8_t mask = 0;
    bool release = false;
    std::uint64_t traceId = 0;
};

enum class TraceStage { Raw, Decoded, Queued, Drained };
struct TraceRecord {
    TraceStage stage = TraceStage::Raw;
    size_t route = 0;
    std::uint64_t id = 0, attachment = 0, source = 0, view = 0, nativeId = 0;
    std::uint64_t openxGeneration = 0;
    std::uint32_t instance = 0, user = 0, index = 0, channels = 0;
    std::uint64_t qpc = 0, generation = 0, sequence = 0, sourceTime = 0;
    std::uint32_t report = 0, flags = 0;
    size_t size = 0, copied = 0;
    std::uint8_t rawMask = 0, mask = 0, profile = 0;
    bool gatt = false, bootstrap = false, gap = false;
    std::array<std::uint8_t, 64> bytes{};
};

class EventQueue {
    std::array<Edge, EventLimit> values_{};
    size_t begin_ = 0, size_ = 0;
    // Overflow drops the incomplete interval and supplies release + snapshot
    // outside the event budget. Later transitions remain ordered in values_.
    Edge snapshot_{};
    unsigned resync_ = 0;
    std::uint64_t lastQpc_ = 0;
public:
    void Clear() noexcept { begin_ = size_ = resync_ = 0; lastQpc_ = 0; }
    bool Push(Edge value) noexcept
    {
        value.mask &= 15;
        value.qpc = std::max(value.qpc, lastQpc_);
        lastQpc_ = value.qpc;
        if (size_ == EventLimit) {
            begin_ = size_ = 0;
            snapshot_ = value;
            snapshot_.release = false;
            resync_ = 2;
            return false;
        }
        values_[(begin_ + size_) % EventLimit] = value;
        ++size_;
        return true;
    }
    bool Pop(Edge &value) noexcept
    {
        if (resync_) {
            value = snapshot_;
            if (resync_ == 2) { value.mask = 0; value.release = true; }
            --resync_;
            return true;
        }
        if (!size_) return false;
        value = values_[begin_];
        begin_ = (begin_ + 1) % EventLimit;
        --size_;
        return true;
    }
};

struct Diagnostics {
    std::uint64_t normal = 0, separate = 0, gatt = 0;
    std::uint64_t malformed = 0, gaps = 0, overflows = 0, states = 0;
    std::uint32_t enableStatus = 0;
    GipInputState input;
    HRESULT commandError = S_OK;
    bool inputValid = false, commandDispatched = false, admissionExpired = false;
    bool failed = false, dataAvailable = false;
    char reason[160]{};
};

void SetReason(Diagnostics &diagnostics, const char *reason) noexcept
{
    size_t n = 0;
    if (reason) {
        while (n + 1 < sizeof(diagnostics.reason) && reason[n]) ++n;
        std::memcpy(diagnostics.reason, reason, n);
    }
    diagnostics.reason[n] = '\0';
}

struct Attachment {
    std::uint64_t token = 0, generation = 0;
    std::uint32_t instanceId = 0, index = 0, count = 0;
    std::uint8_t user = 0;
    bool trace = false;
    std::wstring path;
    PhysicalIdentity physical;
};

struct Lock {
    SRWLOCK &value;
    explicit Lock(SRWLOCK &lock) noexcept : value(lock) { AcquireSRWLockExclusive(&value); }
    ~Lock() { ReleaseSRWLockExclusive(&value); }
};

// This object has no SDL object pointers, allocators, callbacks, or destructors
// registered for process exit. Only copies cross its short queue lock.
class Shared {
    struct Slot {
        std::uint64_t token = 0;
        std::shared_ptr<const Attachment> wanted;
        EventQueue queue;
        Diagnostics diagnostics;
    };
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::array<Slot, RouteLimit> slots_{};
    std::atomic<std::uint64_t> serial_{0};
    std::array<TraceRecord, TraceLimit> trace_{};
    size_t traceBegin_ = 0, traceCount_ = 0;
    std::uint64_t traceSerial_ = 0, traceLost_ = 0;
    void RecordLocked(const TraceRecord &record) noexcept
    {
        if (traceCount_ == TraceLimit) {
            traceBegin_ = (traceBegin_ + 1) % TraceLimit;
            --traceCount_;
            ++traceLost_;
        }
        trace_[(traceBegin_ + traceCount_) % TraceLimit] = record;
        ++traceCount_;
    }
    void StageLocked(TraceStage stage, size_t index, std::uint64_t token, const Edge &edge) noexcept
    {
        if (!edge.traceId) return;
        TraceRecord record;
        record.stage = stage; record.route = index; record.attachment = token;
        record.id = edge.traceId; record.qpc = edge.qpc; record.mask = edge.mask;
        record.gap = edge.release;
        RecordLocked(record);
    }
public:
    HANDLE wake = nullptr;
    std::uint64_t frequency = 0;

    std::uint64_t NewToken() noexcept
    {
        auto old = serial_.load(std::memory_order_relaxed);
        while (old != UINT64_MAX) {
            if (serial_.compare_exchange_weak(old, old + 1, std::memory_order_relaxed)) return old + 1;
        }
        return 0; // Never wrap a lifetime token.
    }
    void Signal() noexcept { if (wake) SetEvent(wake); }
    size_t Attach(std::shared_ptr<const Attachment> attachment) noexcept
    {
        size_t free = RouteLimit;
        {
            Lock lock(lock_);
            if (!attachment || !attachment->token) return RouteLimit;
            for (size_t i = 0; i < RouteLimit; ++i) {
                const auto &slot = slots_[i];
                if (!slot.token && free == RouteLimit) free = i;
                if (slot.wanted && (slot.wanted->user == attachment->user ||
                    SamePhysicalIdentity(slot.wanted->physical, attachment->physical))) return RouteLimit;
            }
            if (free != RouteLimit) {
                auto &slot = slots_[free];
                slot.token = attachment->token;
                slot.wanted = std::move(attachment);
                slot.queue.Clear();
                slot.diagnostics = {};
            }
        }
        Signal();
        return free;
    }
    std::array<std::shared_ptr<const Attachment>, RouteLimit> Wanted() noexcept
    {
        std::array<std::shared_ptr<const Attachment>, RouteLimit> result{};
        Lock lock(lock_);
        for (size_t i = 0; i < RouteLimit; ++i) result[i] = slots_[i].wanted;
        return result;
    }
    bool Active(size_t index, std::uint64_t token) noexcept
    {
        Lock lock(lock_);
        return index < RouteLimit && slots_[index].token == token && slots_[index].wanted != nullptr;
    }
    void Detach(size_t index, std::uint64_t token) noexcept
    {
        {
            Lock lock(lock_);
            if (index >= RouteLimit || slots_[index].token != token) return;
            slots_[index].wanted.reset();
            slots_[index].token = 0;
            slots_[index].queue.Clear();
        }
        Signal();
    }
    void Fail(size_t index, std::uint64_t token, const char *reason, std::uint64_t qpc) noexcept
    {
        {
            Lock lock(lock_);
            if (index >= RouteLimit || slots_[index].token != token) return;
            auto &slot = slots_[index];
            if (!slot.wanted) return;
            slot.wanted.reset();
            slot.queue.Clear();
            slot.queue.Push({qpc, 0, true});
            slot.diagnostics.failed = true;
            slot.diagnostics.dataAvailable = false;
            SetReason(slot.diagnostics, reason);
        }
        Signal();
    }
    void Quit(std::uint64_t qpc) noexcept
    {
        {
            Lock lock(lock_);
            for (auto &slot : slots_) {
                slot.wanted.reset();
                slot.queue.Clear();
                slot.queue.Push({qpc, 0, true});
                slot.token = 0;
            }
        }
        Signal();
    }
    void Publish(size_t index, std::uint64_t token, Edge value) noexcept
    {
        Lock lock(lock_);
        auto &slot = slots_[index];
        if (slot.token != token || !slot.wanted) return;
        if (!slot.queue.Push(value)) ++slot.diagnostics.overflows;
        StageLocked(TraceStage::Queued, index, token, value);
    }
    std::uint64_t CaptureRaw(size_t index, std::uint64_t token, TraceRecord record) noexcept
    {
        Lock lock(lock_);
        if (index >= RouteLimit || slots_[index].token != token || !slots_[index].wanted || traceSerial_ == UINT64_MAX) return 0;
        record.route = index; record.attachment = token; record.id = ++traceSerial_;
        const auto &attachment = *slots_[index].wanted;
        record.instance = attachment.instanceId; record.user = attachment.user;
        record.openxGeneration = attachment.generation;
        record.index = attachment.index; record.channels = attachment.count;
        RecordLocked(record);
        return record.id;
    }
    void CaptureDecoded(size_t index, std::uint64_t token, std::uint64_t id,
                        const SDL_XInputPaddleResult &decoded, std::uint64_t qpc) noexcept
    {
        if (!id) return;
        Lock lock(lock_);
        if (index >= RouteLimit || slots_[index].token != token || !slots_[index].wanted) return;
        TraceRecord record;
        record.stage = TraceStage::Decoded; record.route = index; record.attachment = token; record.id = id;
        record.qpc = qpc; record.flags = decoded.flags; record.rawMask = decoded.sample.raw_mask;
        record.mask = decoded.sample.physical_mask; record.profile = decoded.sample.profile;
        RecordLocked(record);
    }
    size_t DrainTrace(TraceRecord *records, size_t capacity, std::uint64_t &lost) noexcept
    {
        Lock lock(lock_);
        size_t count = 0;
        while (count < capacity && traceCount_) {
            records[count++] = trace_[traceBegin_];
            traceBegin_ = (traceBegin_ + 1) % TraceLimit;
            --traceCount_;
        }
        lost = traceLost_;
        traceLost_ = 0;
        return count;
    }
    void Report(size_t index, std::uint64_t token, Diagnostics diagnostics) noexcept
    {
        Lock lock(lock_);
        auto &slot = slots_[index];
        if (slot.token != token || !slot.wanted) return;
        diagnostics.overflows = slot.diagnostics.overflows;
        slot.diagnostics = diagnostics;
    }
    size_t Drain(size_t index, std::uint64_t token, Edge *values, size_t capacity,
                 Diagnostics &diagnostics, bool ready) noexcept
    {
        Lock lock(lock_);
        if (index >= RouteLimit || slots_[index].token != token) return 0;
        auto &slot = slots_[index];
        diagnostics = slot.diagnostics;
        size_t count = 0;
        if (ready) while (count < capacity && slot.queue.Pop(values[count])) {
            StageLocked(TraceStage::Drained, index, token, values[count]);
            ++count;
        }
        return count;
    }
};

// SDL-side route lifetime. Kept separate from owner failure, whose queued
// release must remain drainable until SDL consumes it.
struct SDLRoute {
    size_t route = RouteLimit;
    bool retired = false;
    bool RetireRoute(Shared *broker, std::uint64_t token) noexcept
    {
        if (retired) return false;
        retired = true;
        if (broker) broker->Detach(route, token);
        return true;
    }
};

// Open only. Update must continue to issue one nonblocking query per tick.
template<class Query, class Yield>
DWORD QueryAtOpen(Query &&query, Yield &&yield) noexcept
{
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const DWORD status = query();
        if (status != ERROR_BUSY) return status;
        if (attempt + 1 < 8) yield();
    }
    return ERROR_BUSY;
}

// The decoder is owner-only. A gap discards cached values but preserves the
// learned split-report mode until the view is retired. Later 0x20 reports can
// therefore never manufacture a release after an actual 0x0C, including gaps.
struct DataRoute {
    SDL_XInputPaddleState decoder{};
    SDL_XInputPaddleResult lastResult{};
    Diagnostics diagnostics;
    std::uint64_t sourceToken = 0, view = 0;
    std::uint64_t lastReading = 0;
    bool candidate46 = false, haveState = false;
    std::uint8_t latest = 0;

    void Start(PaddleTransport transport, std::uint64_t token, std::uint64_t generation = 0) noexcept
    {
        *this = {};
        sourceToken = token;
        view = generation;
        SDL_XInputPaddleReset(&decoder, transport == PaddleTransport::Bluetooth ?
            SDL_XINPUT_PADDLE_VENDOR_GATT : SDL_XINPUT_PADDLE_SERVICE_GIP, token);
    }
    void Gap() noexcept
    {
        ++diagnostics.gaps;
        const auto separate = decoder.have_separate;
        SDL_XInputPaddleReset(&decoder, decoder.transport, sourceToken);
        decoder.have_separate = separate;
        latest = 0;
        haveState = false;
        diagnostics.dataAvailable = false;
    }
    bool Decode(std::uint32_t id, const std::uint8_t *bytes, size_t size) noexcept
    {
        if (decoder.transport == SDL_XINPUT_PADDLE_VENDOR_GATT) ++diagnostics.gatt;
        else if (id == 0x20) ++diagnostics.normal;
        else if (id == 0x0C) ++diagnostics.separate;
        const auto result = SDL_XInputPaddleDecode(&decoder, decoder.transport, sourceToken, id, bytes, size);
        lastResult = result;
        if (!result.flags) { ++diagnostics.malformed; return false; }
        if (id == 0x20 && size == 46 && (result.flags & SDL_XINPUT_PADDLE_NORMAL_READY)) candidate46 = true;
        // Preserve every known in-band layout, including legacy 46-byte input.
        // The decoder stops their publication after an actual separate 0x0C.
        // An in-band sample never increments the separate receipt counter.
        if (!(result.flags & SDL_XINPUT_PADDLE_STATE_READY)) return false;
        latest = result.sample.physical_mask;
        haveState = true;
        diagnostics.dataAvailable = true;
        ++diagnostics.states;
        return true;
    }
    void Service(Shared &shared, size_t index, const Attachment &attachment,
                 const std::vector<ServicePacket> &packets, std::uint64_t now) noexcept
    {
        std::uint64_t newest = 0, oldest = UINT64_MAX;
        for (const auto &packet : packets) {
            if (packet.nativeId == attachment.physical.nativeId && packet.viewGeneration == view && packet.hasReading) {
                newest = std::max(newest, packet.reading.timestamp);
                oldest = std::min(oldest, packet.reading.timestamp);
            }
        }
        bool bootstrap = false, bootstrapState = false;
        std::uint64_t bootstrapTrace = 0;
        for (const auto &packet : packets) {
            if (packet.nativeId != attachment.physical.nativeId || packet.viewGeneration != view) continue;
            if (packet.retired) {
                shared.Report(index, attachment.token, diagnostics);
                shared.Fail(index, attachment.token, packet.reason.c_str(), now);
                return;
            }
            if (packet.bootstrap) bootstrap = true;
            // Select supplies one bootstrap batch. Commit it before any later
            // live packet if an injected or future source mixes the two.
            if (!packet.bootstrap && bootstrap) {
                if (bootstrapState) shared.Publish(index, attachment.token, {now, latest, false, bootstrapTrace});
                bootstrap = bootstrapState = false;
            }
            const auto timestamp = packet.hasReading ?
                HistoryQpc(now, newest, packet.reading.timestamp, shared.frequency) :
                (oldest != UINT64_MAX ? HistoryQpc(now, newest, oldest, shared.frequency) : now);
            if (packet.gap) {
                Gap();
                bootstrapState = false;
                if (!packet.bootstrap) shared.Publish(index, attachment.token, {timestamp, 0, true});
            }
            if (!packet.hasReading) continue;
            std::uint64_t traceId = 0;
            if (attachment.trace) {
                // Own the original payload before calling the decoder, even
                // when its report ID or length will later be rejected.
                TraceRecord raw;
                raw.source = sourceToken; raw.view = view; raw.nativeId = packet.nativeId;
                raw.generation = packet.reading.generation; raw.sequence = packet.reading.rawSequence;
                raw.sourceTime = packet.reading.timestamp; raw.qpc = timestamp; raw.report = packet.reading.reportId;
                raw.bootstrap = packet.bootstrap; raw.gap = packet.gap;
                raw.size = packet.reading.bytes.size(); raw.copied = std::min(raw.size, raw.bytes.size());
                if (raw.copied) std::memcpy(raw.bytes.data(), packet.reading.bytes.data(), raw.copied);
                traceId = shared.CaptureRaw(index, attachment.token, raw);
            }
            // Reader generations, not packet numbers, define order. Nonraw
            // readings can leave holes, so only duplicates/backward IDs reject.
            if (!packet.reading.generation || packet.reading.generation <= lastReading) continue;
            lastReading = packet.reading.generation;
            const bool ready = Decode(packet.reading.reportId, packet.reading.bytes.data(), packet.reading.bytes.size());
            shared.CaptureDecoded(index, attachment.token, traceId, lastResult, timestamp);
            if (ready) {
                if (packet.bootstrap) { bootstrapState = true; bootstrapTrace = traceId; }
                else shared.Publish(index, attachment.token, {timestamp, latest, false, traceId});
            }
        }
        if (bootstrap && bootstrapState) shared.Publish(index, attachment.token, {now, latest, false, bootstrapTrace});
        shared.Report(index, attachment.token, diagnostics);
    }
    void Gatt(Shared &shared, size_t index, const Attachment &attachment,
              const std::vector<GattPaddlePacket> &packets, std::uint64_t now) noexcept
    {
        for (const auto &packet : packets) {
            if (packet.attachmentGeneration != sourceToken) continue;
            if (packet.retired) {
                shared.Report(index, attachment.token, diagnostics);
                shared.Fail(index, attachment.token, "GATT source retired.", now);
                return;
            }
            const auto timestamp = packet.receivedQpc > 0 ?
                std::min(now, static_cast<std::uint64_t>(packet.receivedQpc)) : now;
            if (packet.gap) {
                Gap();
                shared.Publish(index, attachment.token, {timestamp, 0, true});
            }
            if (packet.hasPayload) {
                std::uint64_t traceId = 0;
                if (attachment.trace) {
                    TraceRecord raw;
                    raw.gatt = true; raw.source = sourceToken; raw.qpc = timestamp;
                    raw.sourceTime = static_cast<std::uint64_t>(std::max<std::int64_t>(packet.receivedQpc, 0));
                    raw.gap = packet.gap; raw.size = raw.copied = packet.bytes.size();
                    std::memcpy(raw.bytes.data(), packet.bytes.data(), raw.copied);
                    traceId = shared.CaptureRaw(index, attachment.token, raw);
                }
                const bool ready = Decode(0, packet.bytes.data(), packet.bytes.size());
                shared.CaptureDecoded(index, attachment.token, traceId, lastResult, timestamp);
                if (ready) shared.Publish(index, attachment.token, {timestamp, latest, false, traceId});
            }
        }
        shared.Report(index, attachment.token, diagnostics);
    }
};

// Platform supplies the source APIs. Offline tests instantiate this same owner
// with injected sources, including Connect, Select, retirement, and cleanup.
template<class Platform> class Owner {
    using Service = typename Platform::Service;
    using Gatt = typename Platform::Gatt;
    struct Route {
        std::shared_ptr<const Attachment> attachment;
        DataRoute data;
        std::unique_ptr<Gatt> gatt;
        std::uint64_t nextIdentity = 0, deadline = 0, enableToken = 0;
        std::uint64_t enableProvider = 0, enableEpoch = 0, nextEnableAdmission = 0;
        std::uint64_t admissionRemaining = AcquisitionTimeoutMs, lastAdmissionCheck = 0;
        bool selected = false, enableAttempted = false, admissionEligible = false;
    };
    Shared &shared_;
    std::array<Route, RouteLimit> routes_{};
    // Retained through idle, disconnect, and SDL reinitialization. A second
    // Client object would replace this PID's service connection.
    std::unique_ptr<Service> client_;
    bool connected_ = false;
    bool failed_ = false;

    bool Active(size_t i) noexcept
    {
        return routes_[i].attachment && shared_.Active(i, routes_[i].attachment->token);
    }
    void Fail(size_t i, const char *reason, std::uint64_t now) noexcept
    {
        if (routes_[i].attachment) shared_.Fail(i, routes_[i].attachment->token, reason, now);
    }
    void FailGip(const char *reason, std::uint64_t now) noexcept
    {
        for (size_t i = 0; i < RouteLimit; ++i) {
            if (routes_[i].attachment && IsGipTransport(routes_[i].attachment->physical.transport)) Fail(i, reason, now);
        }
    }
    bool Revalidate(size_t i, std::uint64_t ms, std::uint64_t now)
    {
        auto &route = routes_[i];
        if (!Active(i)) return false;
        std::string error;
        PhysicalIdentity current;
        const auto &a = *route.attachment;
        if (!Platform::Resolve(a.path.c_str(), a.index, a.count, current, error) ||
            !SamePhysicalIdentity(a.physical, current)) {
            Fail(i, error.empty() ? "Physical identity changed." : error.c_str(), now);
            return false;
        }
        route.nextIdentity = ms + IdentityPeriodMs;
        return Active(i);
    }
    void ObserveEnable(size_t i, std::uint64_t ms, std::uint64_t now)
    {
        auto &route = routes_[i];
        // xone publishes Series 1 paddles from ordinary reports. The 07 00
        // extra-data command is a Series 2 initializer in xone and xpad.
        if (route.attachment->physical.product == USB_PRODUCT_XBOX_ONE_ELITE_SERIES_1) return;
        auto &d = route.data.diagnostics;
        GipInputState input;
        d.inputValid = Platform::InputState(route.attachment->physical.nativeId, input);
        if (!d.inputValid) {
            route.admissionEligible = false;
            route.lastAdmissionCheck = ms;
            return;
        }
        d.input = input; // Preserve the provider's callback clocks without conversion.
        if (input.provider && input.epoch &&
            (input.provider != route.enableProvider || input.epoch != route.enableEpoch)) {
            route.enableProvider = input.provider;
            route.enableEpoch = input.epoch;
            route.enableToken = 0;
            route.enableAttempted = route.admissionEligible = false;
            route.admissionRemaining = AcquisitionTimeoutMs;
            route.nextEnableAdmission = 0;
            d.enableStatus = 0; d.commandError = S_OK;
            d.commandDispatched = d.admissionExpired = false;
        }
        if (route.enableToken) {
            GipEnableResult result;
            if (Platform::Poll(route.enableToken, result)) {
                if (result.provider == route.enableProvider && result.epoch == route.enableEpoch) {
                    d.commandError = result.hresult;
                    d.commandDispatched = result.dispatched;
                    if (result.status != GipEnableStatus::Pending) {
                        d.enableStatus = 3 + static_cast<std::uint32_t>(result.status);
                        route.enableToken = 0;
                        // Queue expiry/exhaustion before dispatch did not issue
                        // a native command. Retain the budget and pacing when
                        // reopening admission for this same input epoch.
                        if (!result.dispatched && (result.status == GipEnableStatus::TimedOut ||
                                                   result.status == GipEnableStatus::Exhausted)) {
                            route.enableAttempted = false;
                            route.nextEnableAdmission = std::max(route.nextEnableAdmission, ms + EnableAdmissionRetryMs);
                        }
                    }
                } else {
                    // A stale completion cannot change the current pair's state.
                    // The accepted-attempt latch still prevents a second request.
                    route.enableToken = 0;
                }
            }
        }
        const bool eligible = input.Ready() && !input.busy &&
            input.provider == route.enableProvider && input.epoch == route.enableEpoch;
        if (!eligible || route.enableAttempted || d.admissionExpired) {
            route.admissionEligible = false;
            route.lastAdmissionCheck = ms;
            return;
        }
        if (route.admissionEligible && ms >= route.lastAdmissionCheck) {
            const auto elapsed = ms - route.lastAdmissionCheck;
            route.admissionRemaining -= std::min(route.admissionRemaining, elapsed);
        }
        route.admissionEligible = true;
        route.lastAdmissionCheck = ms;
        if (!route.admissionRemaining) {
            // Admission failure does not invalidate independently received data.
            d.admissionExpired = true;
            d.commandError = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            return;
        }
        if (ms < route.nextEnableAdmission) return;
        route.nextEnableAdmission = ms + EnableAdmissionRetryMs;
        if (!Revalidate(i, ms, now)) return;
        std::string error;
        if (!Platform::ValidateServer(client_->ServerPid(), error)) {
            FailGip(error.c_str(), now);
            return;
        }
        if (!Active(i)) return;
        // WGI checks the expected pair again at admission and dispatch.
        route.enableToken = Platform::Submit(route.attachment->physical.nativeId,
            route.data.sourceToken, route.enableProvider, route.enableEpoch);
        route.enableAttempted = (route.enableToken != 0);
        d.enableStatus = route.enableToken ? 1 : 2;
        if (route.enableAttempted) {
            // Waiting on an admitted request is not queue-admission time.
            route.admissionEligible = false;
            d.commandError = S_OK;
            d.commandDispatched = false;
        }
    }
    void TraceAttachment(const Route &route, trace::Kind kind) noexcept
    {
        if (!route.attachment || !route.data.sourceToken) return;
        trace::Record record;
        record.kind = kind; record.nativeId = route.attachment->physical.nativeId;
        record.attachment = route.data.sourceToken; record.view = route.data.view;
        record.instance = route.attachment->instanceId;
        record.provider = route.enableProvider; record.epoch = route.enableEpoch;
        trace::Push(record);
    }
    void Drop(Route &route) noexcept
    {
        if (!route.attachment) return;
        TraceAttachment(route, trace::Kind::Retired);
        if (route.data.sourceToken) Platform::RetireEnable(route.data.sourceToken);
        if (route.gatt) {
            route.gatt->Retire();
            // The GATT class transfers unfinished cleanup to its bounded orphan
            // slots. PumpRetired below uses this same persistent owner thread.
            route.gatt.reset();
        }
        if (route.selected && connected_) {
            try {
                std::vector<ServicePacket> ignored;
                std::string error;
                client_->Select(route.attachment->physical.nativeId, route.data.view, false, ignored, error);
            } catch (...) { /* Last-route Disconnect also retires every reader. */ }
        }
        route = {};
    }
    void Reconcile(std::uint64_t ms, std::uint64_t now)
    {
        const auto wanted = shared_.Wanted();
        for (size_t i = 0; i < RouteLimit; ++i) {
            auto &route = routes_[i];
            if (route.attachment != wanted[i]) {
                Drop(route);
                route.attachment = wanted[i];
                route.deadline = ms + AcquisitionTimeoutMs;
            }
            if (!Active(i)) continue;
            if (failed_) { Fail(i, "The broker owner could not start.", now); continue; }
            if (ms >= route.nextIdentity && !Revalidate(i, ms, now)) continue;
            if (route.attachment->physical.transport == PaddleTransport::Bluetooth && !route.gatt) {
                const auto token = shared_.NewToken();
                if (!token) { Fail(i, "Source tokens exhausted.", now); continue; }
                route.data.Start(PaddleTransport::Bluetooth, token);
                TraceAttachment(route, trace::Kind::Attachment);
                route.gatt = std::make_unique<Gatt>(route.attachment->physical.container, token);
                GattPaddleError error;
                if (!route.gatt->Start(error)) Fail(i, "GATT acquisition could not start.", now);
            }
        }
    }
    void Gip(std::uint64_t ms, std::uint64_t now)
    {
        bool any = false;
        for (size_t i = 0; i < RouteLimit; ++i) {
            if (Active(i) && IsGipTransport(routes_[i].attachment->physical.transport)) any = true;
        }
        if (!any) {
            if (connected_) { client_->Disconnect(); connected_ = false; }
            return;
        }
        std::string error;
        if (!connected_) {
            if (!Platform::RuntimeAvailable(error)) { FailGip(error.c_str(), now); return; }
            if (!client_) client_ = std::make_unique<Service>();
            if (!client_->Connect(error)) { FailGip(error.c_str(), now); return; }
            connected_ = true;
            if (!Platform::ValidateServer(client_->ServerPid(), error)) {
                FailGip(error.c_str(), now);
                client_->Disconnect();
                connected_ = false;
                return;
            }
        }
        std::vector<ServicePacket> packets;
        if (!client_->Pump(packets, error)) {
            // False retires all routes even if an allocation failure prevented
            // the service from emitting its individual retirement markers.
            FailGip(error.c_str(), now);
            client_->Disconnect();
            connected_ = false;
            return;
        }
        now = Platform::PublicationQpc(now);
        const auto devices = client_->Devices();
        for (size_t i = 0; i < RouteLimit; ++i) {
            auto &route = routes_[i];
            if (!Active(i) || !IsGipTransport(route.attachment->physical.transport)) continue;
            const ServiceDevice *match = nullptr;
            unsigned matches = 0;
            for (const auto &device : devices) {
                if (device.nativeId == route.attachment->physical.nativeId) { match = &device; ++matches; }
            }
            if (route.selected) {
                if (matches != 1 || !match->viewGeneration || match->viewGeneration != route.data.view ||
                    match->vendor != route.attachment->physical.vendor || match->product != route.attachment->physical.product) {
                    Fail(i, "Service view changed or became ambiguous.", now);
                    continue;
                }
                route.data.Service(shared_, i, *route.attachment, packets, now);
            } else {
                if (matches > 1) { Fail(i, "Service native identity is ambiguous.", now); continue; }
                if (!match) {
                    if (ms >= route.deadline) Fail(i, "No matching service view arrived.", now);
                    continue;
                }
                if (!match->viewGeneration || !IsGipPaddleHardware(match->vendor, match->product) ||
                    match->vendor != route.attachment->physical.vendor || match->product != route.attachment->physical.product) {
                    Fail(i, "Service catalog entry is not qualified.", now);
                    continue;
                }
                if (!Revalidate(i, ms, now)) continue;
                if (!Platform::ValidateServer(client_->ServerPid(), error)) { FailGip(error.c_str(), now); break; }
                if (!Active(i)) continue;
                const auto token = shared_.NewToken();
                if (!token) { Fail(i, "Source tokens exhausted.", now); continue; }
                std::vector<ServicePacket> bootstrap;
                if (!client_->Select(match->nativeId, match->viewGeneration, true, bootstrap, error)) {
                    if (error.rfind("retry:", 0) == 0 && ms < route.deadline) continue;
                    Fail(i, error.c_str(), now);
                    continue;
                }
                route.selected = true;
                route.data.Start(route.attachment->physical.transport, token, match->viewGeneration);
                TraceAttachment(route, trace::Kind::Attachment);
                route.data.Service(shared_, i, *route.attachment, bootstrap, Platform::PublicationQpc(now));
            }
            if (!Active(i)) continue;
            // Passive observation begins as soon as exact service selection has
            // succeeded. Service traffic is not the WGI readiness predicate.
            ObserveEnable(i, ms, now);
            shared_.Report(i, route.attachment->token, route.data.diagnostics);
        }
    }
public:
    explicit Owner(Shared &shared) noexcept : shared_(shared) {}
    void StartupFailed() noexcept { failed_ = true; }
    DWORD Step(std::uint64_t ms, std::uint64_t now) noexcept
    {
        try {
            Reconcile(ms, now);
            Gip(ms, now);
            for (size_t i = 0; i < RouteLimit; ++i) {
                auto &route = routes_[i];
                if (!Active(i) || !route.gatt) continue;
                std::vector<GattPaddlePacket> packets;
                GattPaddleError error;
                const bool ok = route.gatt->Pump(packets, error);
                route.data.Gatt(shared_, i, *route.attachment, packets, Platform::PublicationQpc(now));
                if (!ok) Fail(i, "GATT acquisition failed or retired.", now);
            }
        } catch (...) {
            for (size_t i = 0; i < RouteLimit; ++i) Fail(i, "Broker allocation or source operation failed.", now);
        }
        // Cleanup runs even after an exception, SDL Quit, or a failed source.
        bool active = false;
        for (size_t i = 0; i < RouteLimit; ++i) {
            if (!Active(i)) Drop(routes_[i]);
            else active = true;
        }
        bool gip = false;
        for (const auto &route : routes_) {
            if (route.attachment && IsGipTransport(route.attachment->physical.transport)) gip = true;
        }
        if (!gip && connected_) { client_->Disconnect(); connected_ = false; }
        // An unsuccessful initial RoInitialize has no GATT objects to pump.
        if (failed_) return INFINITE;
        GattPaddleError cleanup;
        const auto pending = Platform::PumpRetired(cleanup);
        if (active) return 2;
        if (pending || cleanup.code == GattPaddleErrorCode::WrongApartment ||
            cleanup.code == GattPaddleErrorCode::WrongThread) return 10;
        return INFINITE;
    }
};

#ifndef SDL_XINPUT_PADDLE_BROKER_TESTING
struct NativePlatform {
    using Service = Client;
    using Gatt = GattPaddleClient;
    static bool Resolve(const wchar_t *path, std::uint32_t index, std::uint32_t count,
                        PhysicalIdentity &identity, std::string &error)
    { return ResolvePaddleIdentity(path, index, count, identity, error); }
    static bool RuntimeAvailable(std::string &error) { return PaddleUsbRuntimeAvailable(error); }
    static bool ValidateServer(std::uint32_t pid, std::string &error) { return PaddleValidateServer(pid, error); }
    static std::uint64_t PublicationQpc(std::uint64_t) noexcept { return QpcNow(); }
    static bool InputState(std::uint64_t native, GipInputState &state) noexcept { return QueryGipInputState(native, state); }
    static std::uint64_t Submit(std::uint64_t native, std::uint64_t token, std::uint64_t provider, std::uint64_t epoch) noexcept
    { return SubmitGipEnable(native, token, provider, epoch); }
    static bool Poll(std::uint64_t token, GipEnableResult &result) noexcept { return PollGipEnable(token, result); }
    static void RetireEnable(std::uint64_t token) noexcept { RetireGipEnable(token); }
    static unsigned PumpRetired(GattPaddleError &error) noexcept { return GattPaddleClient::PumpRetired(error); }
};

unsigned __stdcall OwnerThread(void *parameter) noexcept
{
    auto &shared = *static_cast<Shared *>(parameter);
    // The apartment and all callback owners persist until process exit.
    // There is no join under SDL's joystick lock and no RoUninitialize race.
    Owner<NativePlatform> owner(shared);
    const auto hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) owner.StartupFailed();
    for (;;) {
        const auto wait = owner.Step(GetTickCount64(), QpcNow());
        WaitForSingleObject(shared.wake, wait);
    }
}

// Only the SDL thread accesses these variables, under its joystick lock.
static Shared *processBroker = nullptr;
static bool attemptedStartup = false;
static const char moduleAnchor = 0;

Shared *GetBroker(bool create) noexcept
{
    if (processBroker || !create || attemptedStartup) return processBroker;
    attemptedStartup = true;
    auto shared = std::unique_ptr<Shared>(new (std::nothrow) Shared);
    if (!shared) return nullptr;
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 || frequency.QuadPart > UINT32_MAX) return nullptr;
    shared->frequency = static_cast<std::uint64_t>(frequency.QuadPart);
    shared->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!shared->wake) return nullptr;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&moduleAnchor), &pinned)) {
        CloseHandle(shared->wake);
        return nullptr;
    }
    // Pinning is permanent even if thread creation fails. No asynchronous work
    // can begin before this succeeds, including GATT delegates and WGI factories.
    const auto thread = _beginthreadex(nullptr, 0, OwnerThread, shared.get(), 0, nullptr);
    if (!thread) { CloseHandle(shared->wake); return nullptr; }
    CloseHandle(reinterpret_cast<HANDLE>(thread));
    processBroker = shared.release();
    return processBroker;
}
#endif

} // namespace broker_detail
} // namespace sdl_paddles

#ifndef SDL_XINPUT_PADDLE_BROKER_TESTING
namespace {
using namespace sdl_paddles;
using namespace sdl_paddles::broker_detail;

struct Context : SDLRoute {
    SDL_Joystick *joystick = nullptr;
    Context *next = nullptr;
    std::shared_ptr<const Attachment> attachment;
    bool startupUpdate = true;
    Uint64 nextDiagnostic = 0, lastTimestamp = 0;
};
Context *contexts = nullptr;

struct Query {
    SDL_XINPUT_DEVICE_IDENTITY_V1 identity{};
    std::array<wchar_t, PathLimit> path{};
};

DWORD QuerySlot(Uint8 user, Query &query) noexcept
{
    query = {};
    query.identity.cbSize = sizeof(query.identity);
    if (!SDL_XInputGetDeviceIdentity) return ERROR_NOT_SUPPORTED;
    const auto status = SDL_XInputGetDeviceIdentity(user, &query.identity, query.path.data(), static_cast<DWORD>(query.path.size()));
    if (status != ERROR_SUCCESS) return status;
    const auto length = query.identity.requiredCch;
    if (!query.identity.generation || query.identity.interfaceIndex != 0 || query.identity.interfaceCount != 1 ||
        length < 2 || length > query.path.size() || query.path[length - 1] != L'\0') return ERROR_INVALID_DATA;
    for (size_t i = 0; i + 1 < length; ++i) if (!query.path[i]) return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

bool Matches(const Attachment &a, const Query &query) noexcept
{
    return a.generation == query.identity.generation && a.index == query.identity.interfaceIndex &&
           a.count == query.identity.interfaceCount && a.path == query.path.data();
}

DWORD QuerySlotAtOpen(SDL_Joystick *joystick, Uint8 user, Query &query, const char *stage)
{
    const auto status = QueryAtOpen([&] { return QuerySlot(user, query); }, [] { Sleep(1); });
    if (status == ERROR_BUSY) {
        char reason[160];
        SDL_snprintf(reason, sizeof(reason), "XInput identity %s stayed busy after 8 Open attempts.", stage);
        SDL_SetStringProperty(SDL_GetJoystickProperties(joystick), "SDL.joystick.xinput.paddle.error", reason);
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "%s", reason);
    }
    return status;
}

void ClearPaddles(Context &context) noexcept
{
    auto *joystick = context.joystick;
    if (joystick->buttons && joystick->nbuttons >= 16) {
        const auto timestamp = SDL_GetTicksNS();
        for (Uint8 button = 12; button < 16; ++button) SDL_SendJoystickButton(timestamp, joystick, button, false);
    }
}

void Retire(Context &context, const char *reason) noexcept
{
    if (!context.RetireRoute(GetBroker(false), context.attachment->token)) return;
    ClearPaddles(context);
    const auto props = SDL_GetJoystickProperties(context.joystick);
    SDL_SetBooleanProperty(props, "SDL.joystick.xinput.paddle.data_available", false);
    SDL_SetStringProperty(props, "SDL.joystick.xinput.paddle.error", reason);
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "XInput paddles instance=%u retired: %s", context.attachment->instanceId, reason);
}

void Report(Context &context, const Diagnostics &d, Uint64 now)
{
    if (!d.failed && now < context.nextDiagnostic) return;
    context.nextDiagnostic = now + SDL_NS_PER_SECOND;
    const auto props = SDL_GetJoystickProperties(context.joystick);
    SDL_SetNumberProperty(props, "SDL.joystick.xinput.paddle.received_20", static_cast<Sint64>(d.normal));
    SDL_SetNumberProperty(props, "SDL.joystick.xinput.paddle.received_0c", static_cast<Sint64>(d.separate));
    SDL_SetNumberProperty(props, "SDL.joystick.xinput.paddle.received_gatt", static_cast<Sint64>(d.gatt));
    SDL_SetNumberProperty(props, "SDL.joystick.xinput.paddle.gaps", static_cast<Sint64>(d.gaps + d.overflows));
    SDL_SetNumberProperty(props, "SDL.joystick.xinput.paddle.enable_status", d.enableStatus);
    SDL_SetBooleanProperty(props, "SDL.joystick.xinput.paddle.wgi_snapshot_valid", d.inputValid);
    SDL_SetNumberProperty(props, "SDL.joystick.xinput.paddle.wgi_provider", static_cast<Sint64>(d.input.provider));
    SDL_SetNumberProperty(props, "SDL.joystick.xinput.paddle.wgi_epoch", static_cast<Sint64>(d.input.epoch));
    SDL_SetNumberProperty(props, "SDL.joystick.xinput.paddle.wgi_resume_time", static_cast<Sint64>(d.input.resumeTime));
    SDL_SetNumberProperty(props, "SDL.joystick.xinput.paddle.wgi_normal_time", static_cast<Sint64>(d.input.normalTime));
    SDL_SetNumberProperty(props, "SDL.joystick.xinput.paddle.wgi_split_time", static_cast<Sint64>(d.input.splitTime));
    SDL_SetBooleanProperty(props, "SDL.joystick.xinput.paddle.wgi_ready", d.inputValid && d.input.Ready());
    SDL_SetBooleanProperty(props, "SDL.joystick.xinput.paddle.wgi_resumed", d.inputValid && d.input.resumed);
    SDL_SetBooleanProperty(props, "SDL.joystick.xinput.paddle.wgi_normal", d.inputValid && d.input.normalSeen);
    SDL_SetBooleanProperty(props, "SDL.joystick.xinput.paddle.wgi_split", d.inputValid && d.input.splitSeen);
    SDL_SetBooleanProperty(props, "SDL.joystick.xinput.paddle.wgi_busy", d.input.busy);
    SDL_SetNumberProperty(props, "SDL.joystick.xinput.paddle.wgi_error", d.input.error);
    SDL_SetNumberProperty(props, "SDL.joystick.xinput.paddle.command_error", d.commandError);
    SDL_SetBooleanProperty(props, "SDL.joystick.xinput.paddle.command_dispatched", d.commandDispatched);
    SDL_SetBooleanProperty(props, "SDL.joystick.xinput.paddle.admission_expired", d.admissionExpired);
    SDL_SetBooleanProperty(props, "SDL.joystick.xinput.paddle.data_available", d.dataAvailable && !d.failed);
    if (d.failed) SDL_SetStringProperty(props, "SDL.joystick.xinput.paddle.error", d.reason);
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
        "XInput paddles instance=%u received20=%llu received0c=%llu receivedGatt=%llu states=%llu gaps=%llu malformed=%llu enable=%u",
        context.attachment->instanceId, static_cast<unsigned long long>(d.normal),
        static_cast<unsigned long long>(d.separate), static_cast<unsigned long long>(d.gatt),
        static_cast<unsigned long long>(d.states), static_cast<unsigned long long>(d.gaps + d.overflows),
        static_cast<unsigned long long>(d.malformed), d.enableStatus);
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
        "XInput paddle readiness instance=%u valid=%d provider=%llu epoch=%llu resumed=%d normal=%d split=%d busy=%d resume_time=%llu normal_time=%llu split_time=%llu wgi_error=%08x command_error=%08x dispatched=%d admission_expired=%d",
        context.attachment->instanceId, d.inputValid, static_cast<unsigned long long>(d.input.provider), static_cast<unsigned long long>(d.input.epoch),
        d.input.resumed, d.input.normalSeen, d.input.splitSeen, d.input.busy, static_cast<unsigned long long>(d.input.resumeTime),
        static_cast<unsigned long long>(d.input.normalTime), static_cast<unsigned long long>(d.input.splitTime),
        static_cast<unsigned>(d.input.error), static_cast<unsigned>(d.commandError), d.commandDispatched, d.admissionExpired);
}

void LogWgiTrace()
{
    if (!trace::Enabled()) return;
    std::array<trace::Record, 64> records;
    std::uint64_t lost = 0;
    const auto count = trace::Drain(records.data(), records.size(), lost);
    if (lost) SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "PADDLETRACE lost=%llu", static_cast<unsigned long long>(lost));
    for (size_t i = 0; i < count; ++i) {
        const auto &r = records[i];
        constexpr char digits[] = "0123456789abcdef";
        char hex[129];
        for (size_t b = 0; b < r.copied; ++b) { hex[2*b] = digits[r.bytes[b] >> 4]; hex[2*b+1] = digits[r.bytes[b] & 15]; }
        hex[2*r.copied] = '\0';
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
            "PADDLETRACE kind=%s order=%llu event=%llu qpc=%llu native=%016llx attachment=%llu provider=%llu epoch=%llu view=%llu reading=%llu sequence=%llu source_time=%llu command=%llu instance=%u report=%02x class=%u length=%u copied=%u truncated=%d status=%08x mask=%x profile=%u hex=%s",
            trace::Name(r.kind), static_cast<unsigned long long>(r.order), static_cast<unsigned long long>(r.event),
            static_cast<unsigned long long>(r.qpc), static_cast<unsigned long long>(r.nativeId), static_cast<unsigned long long>(r.attachment),
            static_cast<unsigned long long>(r.provider), static_cast<unsigned long long>(r.epoch), static_cast<unsigned long long>(r.view),
            static_cast<unsigned long long>(r.reading), static_cast<unsigned long long>(r.sequence), static_cast<unsigned long long>(r.sourceTime),
            static_cast<unsigned long long>(r.command), r.instance, r.report, r.messageClass, r.length, r.copied, r.truncated,
            static_cast<unsigned>(r.status), r.mask, r.profile, hex);
    }
}
void LogDataTrace(Shared &broker)
{
    std::array<TraceRecord, TraceLimit> records;
    std::uint64_t lost = 0;
    const auto count = broker.DrainTrace(records.data(), records.size(), lost);
    if (lost) SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "PADDLETRACE lost=%llu", static_cast<unsigned long long>(lost));
    for (size_t i = 0; i < count; ++i) {
        const auto &r = records[i];
        if (r.stage == TraceStage::Raw) {
            constexpr char digits[] = "0123456789abcdef";
            char hex[129];
            for (size_t b = 0; b < r.copied; ++b) { hex[2*b] = digits[r.bytes[b] >> 4]; hex[2*b+1] = digits[r.bytes[b] & 15]; }
            hex[2*r.copied] = '\0';
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                "PADDLETRACE raw id=%llu route=%u attachment=%llu source=%llu native=%016llx view=%llu generation=%llu sequence=%llu source_time=%llu clock=%s qpc=%llu report=%02x bytes=%u copied=%u bootstrap=%d gap=%d instance=%u slot=%u openx_generation=%llu channel=%u/%u byte14=%d byte15=%d byte18=%d hex=%s",
                static_cast<unsigned long long>(r.id), static_cast<unsigned>(r.route), static_cast<unsigned long long>(r.attachment),
                static_cast<unsigned long long>(r.source), static_cast<unsigned long long>(r.nativeId), static_cast<unsigned long long>(r.view),
                static_cast<unsigned long long>(r.generation), static_cast<unsigned long long>(r.sequence), static_cast<unsigned long long>(r.sourceTime),
                r.gatt ? "qpc" : "us", static_cast<unsigned long long>(r.qpc), r.report, static_cast<unsigned>(r.size),
                static_cast<unsigned>(r.copied), r.bootstrap, r.gap, r.instance, r.user,
                static_cast<unsigned long long>(r.openxGeneration), r.index, r.channels,
                r.copied > 14 ? r.bytes[14] : -1, r.copied > 15 ? r.bytes[15] : -1, r.copied > 18 ? r.bytes[18] : -1, hex);
        } else {
            const char *stage = r.stage == TraceStage::Decoded ? "decoded" : r.stage == TraceStage::Queued ? "queued" : "drained";
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "PADDLETRACE %s id=%llu route=%u attachment=%llu qpc=%llu flags=%x raw_mask=%x mask=%x profile=%u release=%d",
                stage, static_cast<unsigned long long>(r.id), static_cast<unsigned>(r.route), static_cast<unsigned long long>(r.attachment),
                static_cast<unsigned long long>(r.qpc), r.flags, r.rawMask, r.mask, r.profile, r.gap);
        }
    }
}
} // namespace

extern "C" void SDL_XINPUT_PaddleOpen(SDL_Joystick *joystick, Uint8 user)
{
    SDL_AssertJoysticksLocked();
    if (!joystick || !joystick->hwdata || joystick->hwdata->paddle_context) return;
    trace::SetEnabled(SDL_GetHintBoolean("SDL_JOYSTICK_XINPUT_PADDLE_TRACE", false));
    // A WGI custom factory affects this process's controller discovery and has
    // no unregister API. Read the opt-out before reserving or starting anything.
    if (!SDL_GetHintBoolean("SDL_JOYSTICK_XINPUT_PADDLES", true)) return;
    Uint16 vendor = 0, product = 0;
    // SDL_OpenJoystick assigns joystick->guid after driver->Open. Windows has
    // already copied the enumerated GUID into hwdata at this call site.
    SDL_GetJoystickGUIDInfo(joystick->hwdata->guid, &vendor, &product, nullptr, nullptr);
    // USB XUSB can expose 045E:02FF even for an Elite. It is only a candidate:
    // eligibility is checked against the verified physical USB parent below.
    if (!Eligible(vendor, product) && !(vendor == USB_VENDOR_MICROSOFT && product == 0x02ff)) return;
    try {
        Query query;
        if (QuerySlotAtOpen(joystick, user, query, "initial query") != ERROR_SUCCESS) return;
        auto attachment = std::make_shared<Attachment>();
        attachment->user = user;
        attachment->trace = trace::Enabled();
        attachment->instanceId = joystick->instance_id;
        attachment->generation = query.identity.generation;
        attachment->index = query.identity.interfaceIndex;
        attachment->count = query.identity.interfaceCount;
        attachment->path = query.path.data();
        std::string error;
        if (!ResolvePaddleIdentity(attachment->path.c_str(), attachment->index, attachment->count, attachment->physical, error)) return;
        if (!EligiblePhysical(vendor, product, attachment->physical)) return;
        if (IsGipTransport(attachment->physical.transport) && !PaddleUsbRuntimeAvailable(error)) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "XInput paddles runtime unavailable: %s", error.c_str());
            return;
        }
        // GATT uses the public Windows 10 WinRT contract. No private service or
        // loaded-image inference is needed for this transport.
        if (!IsGipTransport(attachment->physical.transport) && attachment->physical.transport != PaddleTransport::Bluetooth) return;
        Query recheck;
        if (QuerySlotAtOpen(joystick, user, recheck, "recheck") != ERROR_SUCCESS || !Matches(*attachment, recheck)) return;
        auto context = std::make_unique<Context>();
        context->joystick = joystick;
        context->attachment = attachment;
        const auto props = SDL_GetJoystickProperties(joystick);
        if (!props || !SDL_SetNumberProperty(props, SDL_PROP_JOYSTICK_XINPUT_PADDLE_MASK_NUMBER, 15)) return;
        joystick->nbuttons = 16;
        if (auto *broker = GetBroker(true)) {
            attachment->token = broker->NewToken();
            context->route = broker->Attach(attachment);
        }
        context->next = contexts;
        contexts = context.release();
        joystick->hwdata->paddle_context = contexts;
        if (contexts->route == RouteLimit) Retire(*contexts, "No broker route is available.");
    } catch (...) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "XInput paddle Open could not allocate its owned context.");
    }
}

extern "C" void SDL_XINPUT_PaddleUpdate(SDL_Joystick *joystick)
{
    SDL_AssertJoysticksLocked();
    LogWgiTrace();
    if (!joystick || !joystick->hwdata) return;
    auto *context = static_cast<Context *>(joystick->hwdata->paddle_context);
    if (!context || context->retired) return;
    // The driver Update invoked by SDL_OpenJoystick precedes SDL_OpenGamepad's
    // bindings. Retain the complete owned queue for the next update.
    if (context->startupUpdate) { context->startupUpdate = false; return; }
    if (!joystick->buttons || joystick->nbuttons < 16) return;
    Query query;
    const auto status = QuerySlot(context->attachment->user, query);
    if (status == ERROR_BUSY) return;
    if (status != ERROR_SUCCESS || !Matches(*context->attachment, query)) {
        Retire(*context, "The authoritative XInput attachment changed.");
        return;
    }
    auto *broker = GetBroker(false);
    if (!broker) { Retire(*context, "The broker is unavailable."); return; }
    std::array<Edge, EventLimit + 2> values;
    Diagnostics diagnostics;
    const auto count = broker->Drain(context->route, context->attachment->token, values.data(), values.size(), diagnostics, true);
    if (context->attachment->trace && trace::Enabled()) LogDataTrace(*broker);
    const auto now = SDL_GetTicksNS();
    const auto qpc = QpcNow();
    for (size_t i = 0; i < count; ++i) {
        const auto age = qpc > values[i].qpc ? qpc - values[i].qpc : 0;
        auto timestamp = now - ScaleClamped(age, SDL_NS_PER_SECOND, broker->frequency, now);
        timestamp = std::max(timestamp, context->lastTimestamp);
        context->lastTimestamp = timestamp;
        Uint8 before = 0;
        if (values[i].traceId) for (Uint8 bit = 0; bit < 4; ++bit) if (joystick->buttons[12 + bit]) before |= (1u << bit);
        for (Uint8 bit = 0; bit < 4; ++bit) {
            SDL_SendJoystickButton(timestamp, joystick, static_cast<Uint8>(12 + bit), (values[i].mask & (1u << bit)) != 0);
        }
        if (values[i].traceId && trace::Enabled()) {
            Uint8 after = 0;
            for (Uint8 bit = 0; bit < 4; ++bit) if (joystick->buttons[12 + bit]) after |= (1u << bit);
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "PADDLETRACE sdl id=%llu instance=%u wanted=%x before=%x after=%x release=%d down_events=%d up_events=%d timestamp=%llu",
                static_cast<unsigned long long>(values[i].traceId), joystick->instance_id, values[i].mask, before, after,
                values[i].release, SDL_EventEnabled(SDL_EVENT_JOYSTICK_BUTTON_DOWN), SDL_EventEnabled(SDL_EVENT_JOYSTICK_BUTTON_UP), static_cast<unsigned long long>(timestamp));
        }
    }
    Report(*context, diagnostics, now);
    if (diagnostics.failed) Retire(*context, diagnostics.reason);
}

extern "C" void SDL_XINPUT_PaddleRemoved(SDL_JoystickID instanceId)
{
    SDL_AssertJoysticksLocked();
    for (auto *context = contexts; context; context = context->next) {
        if (context->attachment->instanceId == instanceId) Retire(*context, "The XInput device was removed.");
    }
}

extern "C" void SDL_XINPUT_PaddleClose(SDL_Joystick *joystick)
{
    SDL_AssertJoysticksLocked();
    if (!joystick || !joystick->hwdata) return;
    auto *context = static_cast<Context *>(joystick->hwdata->paddle_context);
    if (!context) return;
    joystick->hwdata->paddle_context = nullptr;
    if (auto *broker = GetBroker(false)) broker->Detach(context->route, context->attachment->token);
    ClearPaddles(*context);
    for (auto **link = &contexts; *link; link = &(*link)->next) {
        if (*link == context) { *link = context->next; break; }
    }
    delete context;
}

extern "C" void SDL_XINPUT_PaddleQuit(void)
{
    SDL_AssertJoysticksLocked();
    while (contexts) SDL_XINPUT_PaddleClose(contexts->joystick);
    if (auto *broker = GetBroker(false)) broker->Quit(QpcNow());
}
#endif // !SDL_XINPUT_PADDLE_BROKER_TESTING
#endif // SDL_JOYSTICK_XINPUT_PADDLES
