/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
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
#include "SDL_xinput_paddle_service.h"
#include "SDL_xinput_paddle_nt.h"

#include <bcrypt.h>
#include <sddl.h>
#include <intrin.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace sdl_paddles {
namespace service_detail {
using SectionId = std::array<std::uint8_t, 32>;
constexpr std::size_t MaxView = 64 * 1024 * 1024;
constexpr std::size_t MaxTotal = 256 * 1024 * 1024;
constexpr std::size_t MaxViews = 128;
constexpr unsigned ReceiveBudget = 1024;
constexpr std::size_t MessageBytes = 0x170;

bool Fail(std::string& error, const char* reason)
{
    error = reason;
    return false;
}
void Require(bool okay, const char* reason)
{
    if (!okay) throw std::runtime_error(reason);
}
void CheckNt(LONG status, const char* operation)
{
    if (status != nt::Success) throw std::runtime_error(std::string(operation) + ": status " + std::to_string(static_cast<ULONG>(status)));
}
void CheckWin(BOOL okay, const char* operation)
{
    if (!okay) throw std::runtime_error(std::string(operation) + ": error " + std::to_string(GetLastError()));
}
void CleanupFailure(std::uint32_t& failures) noexcept
{
    if (failures != UINT32_MAX) ++failures;
}

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() noexcept { Reset(); }
    void Reset() noexcept
    {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(value, nullptr));
    }
};
struct LocalMemory {
    void* value = nullptr;
    ~LocalMemory() noexcept { if (value) LocalFree(value); }
};

struct Api {
    nt::ConnectPort connect = nullptr;
    nt::DisconnectPort disconnect = nullptr;
    nt::QueryInformation query = nullptr;
    nt::DeleteSectionView unmap = nullptr;
    nt::SendWaitReceivePort sendReceive = nullptr;
    nt::CancelMessage cancel = nullptr;
    nt::InitializeMessageAttribute initialize = nullptr;
    nt::GetMessageAttribute attribute = nullptr;
    decltype(&SetEvent) signal = &SetEvent;
    decltype(&DuplicateHandle) duplicate = &DuplicateHandle;

    bool Complete() const noexcept
    {
        return connect && disconnect && query && unmap && sendReceive && cancel && initialize && attribute;
    }
    template<class T> void Bind(T& target, HMODULE module, const char* name)
    {
        const FARPROC address = GetProcAddress(module, name);
        static_assert(sizeof(target) == sizeof(address));
        std::memcpy(&target, &address, sizeof(target));
    }
    bool Load()
    {
        wchar_t directory[MAX_PATH];
        const UINT count = GetSystemDirectoryW(directory, MAX_PATH);
        if (!count || count >= MAX_PATH) return false;
        const std::wstring path = std::wstring(directory, count) + L"\\ntdll.dll";
        const HMODULE module = GetModuleHandleW(path.c_str());
        if (!module) return false;
        Bind(connect, module, "NtAlpcConnectPort");
        Bind(disconnect, module, "NtAlpcDisconnectPort");
        Bind(query, module, "NtAlpcQueryInformation");
        Bind(unmap, module, "NtAlpcDeleteSectionView");
        Bind(sendReceive, module, "NtAlpcSendWaitReceivePort");
        Bind(cancel, module, "NtAlpcCancelMessage");
        Bind(initialize, module, "AlpcInitializeMessageAttribute");
        Bind(attribute, module, "AlpcGetMessageAttribute");
        return Complete();
    }
};

using Sid = std::array<std::uint8_t, SECURITY_MAX_SID_SIZE>;
Sid WellKnown(WELL_KNOWN_SID_TYPE type)
{
    Sid sid{};
    DWORD size = static_cast<DWORD>(sid.size());
    CheckWin(CreateWellKnownSid(type, nullptr, sid.data(), &size), "create SID");
    return sid;
}
Sid TokenSid(HANDLE token, TOKEN_INFORMATION_CLASS kind)
{
    DWORD count = 0;
    GetTokenInformation(token, kind, nullptr, 0, &count);
    Require(count >= sizeof(void*) && count <= 4096, "token information size");
    std::vector<std::uint8_t> storage(count);
    CheckWin(GetTokenInformation(token, kind, storage.data(), count, &count), "token information");
    PSID pointer = nullptr;
    std::memcpy(&pointer, storage.data(), sizeof(pointer));
    if (!pointer) return WellKnown(WinNullSid);
    const auto at = reinterpret_cast<std::uintptr_t>(pointer);
    const auto base = reinterpret_cast<std::uintptr_t>(storage.data());
    Require(at >= base && at - base <= storage.size() && storage.size() - (at - base) >= 8, "token SID bounds");
    const auto length = 8u + 4u * static_cast<const std::uint8_t*>(pointer)[1];
    Require(length <= storage.size() - (at - base) && length <= SECURITY_MAX_SID_SIZE && IsValidSid(pointer), "token SID format");
    Sid result{};
    std::memcpy(result.data(), pointer, length);
    return result;
}
std::wstring SidText(Sid& sid)
{
    LPWSTR text = nullptr;
    CheckWin(ConvertSidToStringSidW(sid.data(), &text), "SID text");
    LocalMemory memory{text};
    return std::wstring(text);
}
struct Security {
    LocalMemory storage;
    explicit Security(const std::wstring& text)
    {
        CheckWin(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &storage.value, nullptr), "security descriptor");
    }
    SECURITY_ATTRIBUTES Attributes() const { return {sizeof(SECURITY_ATTRIBUTES), storage.value, FALSE}; }
};
struct Namespace {
    HANDLE value = nullptr, boundary = nullptr;
    Handle clientSignal, serverSignal;
    std::array<std::uint8_t, 0x98> attributes{};
    ~Namespace() noexcept { Reset(); }
    bool DestroyName() noexcept
    {
        return !value || ClosePrivateNamespace(std::exchange(value, nullptr), PRIVATE_NAMESPACE_FLAG_DESTROY);
    }
    void Reset() noexcept
    {
        DestroyName();
        clientSignal.Reset();
        serverSignal.Reset();
        if (boundary) DeleteBoundaryDescriptor(std::exchange(boundary, nullptr));
    }
    void Create()
    {
        // sipc-prototype/transport.cpp:68-96 and the matched native namespace.
        Handle token;
        CheckWin(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value), "process token");
        auto app = TokenSid(token.value, TokenAppContainerSid);
        auto user = TokenSid(token.value, TokenUser);
        static_assert(SECURITY_MAX_SID_SIZE == 68);
        attributes.fill(0);
        CheckNt(BCryptGenRandom(nullptr, attributes.data(), 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG), "namespace random bytes");
        std::memcpy(attributes.data() + 0x10, app.data(), app.size());
        std::memcpy(attributes.data() + 0x54, user.data(), user.size());
        std::wstring alias = L"SIPC_";
        constexpr wchar_t digits[] = L"0123456789ABCDEF";
        for (std::size_t i = 0; i < 16; ++i) {
            alias += digits[attributes[i] >> 4];
            alias += digits[attributes[i] & 15];
        }
        boundary = CreateBoundaryDescriptorW(alias.c_str(), 0);
        CheckWin(boundary != nullptr, "namespace boundary");
        auto world = WellKnown(WinWorldSid);
        CheckWin(AddSIDToBoundaryDescriptor(&boundary, world.data()), "boundary SID");
        if (!IsWellKnownSid(app.data(), WinNullSid)) CheckWin(AddSIDToBoundaryDescriptor(&boundary, app.data()), "app boundary SID");
        Security security(L"S:(ML;;NW;;;LW)D:(A;;GA;;;WD)(A;;GA;;;" + SidText(app) + L")(A;;GA;;;" + SidText(user) + L")");
        auto sa = security.Attributes();
        value = CreatePrivateNamespaceW(&sa, boundary, alias.c_str());
        CheckWin(value != nullptr, "private namespace");
        Security eventSecurity(L"D:(A;;GA;;;WD)");
        auto eventSa = eventSecurity.Attributes();
        clientSignal.value = CreateEventW(&eventSa, FALSE, FALSE, (alias + L"\\ClientSignal").c_str());
        CheckWin(clientSignal.value != nullptr, "client event");
        serverSignal.value = CreateEventW(&eventSa, FALSE, FALSE, (alias + L"\\ServerSignal").c_str());
        CheckWin(serverSignal.value != nullptr, "server event");
    }
};

template<class T> T Read(const std::uint8_t* bytes, std::size_t offset)
{
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}
using QueryMemory = decltype(&VirtualQuery);
bool Writable(void* address, std::size_t length, QueryMemory query = &VirtualQuery)
{
    const auto base = reinterpret_cast<std::uintptr_t>(address);
    if (!base || !length || length > UINTPTR_MAX - base) return false;
    const auto end = base + length;
    for (auto at = base; at < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (query(reinterpret_cast<void*>(at), &info, sizeof(info)) != sizeof(info)) return false;
        if (info.AllocationBase != address || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) ||
            ((info.Protect & 0xFF) != PAGE_READWRITE && (info.Protect & 0xFF) != PAGE_WRITECOPY)) return false;
        const auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (region > at || info.RegionSize > UINTPTR_MAX - region || region + info.RegionSize <= at) return false;
        at = (std::min)(end, region + info.RegionSize);
    }
    return true;
}
bool Trailer(void* address, std::size_t size, std::size_t& logical, SectionId& id)
{
    logical = 0;
    id.fill(0);
    // Validate every page and the allocation before the first trailer read.
    if ((reinterpret_cast<std::uintptr_t>(address) & 0xFFF) || size < 0x28 || size >= MaxView || !Writable(address, size)) return false;
    const auto offset = (size - 0x28) & ~std::size_t(7);
    std::array<std::uint8_t, 0x28> copy{};
    std::memcpy(copy.data(), static_cast<std::uint8_t*>(address) + offset, copy.size());
    const auto length = Read<std::uint64_t>(copy.data(), 0);
    if (length > offset || length < 0x24) return false;
    std::memcpy(id.data(), copy.data() + 8, id.size());
    if (std::all_of(id.begin(), id.end(), [](auto byte) { return byte == 0; })) return false;
    logical = static_cast<std::size_t>(length);
    return true;
}

struct Identity {
    std::uint64_t nativeId = 0;
    std::size_t offset = 0;
    std::array<std::uint8_t, 0x24> header{};
    std::array<std::uint8_t, 0x330> block{};
    bool operator==(const Identity&) const = default;
};
bool CaptureIdentity(void* address, std::size_t logical, Identity& output)
{
    // Matched private GIP builder: P=B+u32(B+4), size exactly 0x330,
    // native ID at P+8, provider at P+0x1A0. No other private format is admitted.
    output = {};
    if (logical < 0x24 || !Writable(address, logical)) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    Identity copy;
    std::memcpy(copy.header.data(), bytes, copy.header.size());
    const auto total = Read<std::uint32_t>(copy.header.data(), 0);
    if (total < 0x24 || total > logical) return false;
    std::array<std::size_t, 5> starts{};
    constexpr std::size_t offsets[] = {4, 8, 0xC, 0x10, 0x18};
    for (std::size_t i = 0; i < starts.size(); ++i) {
        starts[i] = Read<std::uint32_t>(copy.header.data(), offsets[i]);
        if (!starts[i]) continue;
        if (starts[i] < 0x24 || starts[i] > total || total - starts[i] < 4) return false;
        for (std::size_t j = 0; j < i; ++j) if (starts[i] == starts[j]) return false;
    }
    copy.offset = starts[0];
    if (!copy.offset) return false;
    std::size_t end = total;
    for (auto start : starts) if (start > copy.offset) end = (std::min)(end, start);
    if (copy.block.size() > end - copy.offset) return false;
    std::memcpy(copy.block.data(), bytes + copy.offset, copy.block.size());
    if (Read<std::uint32_t>(copy.block.data(), 0) != copy.block.size()) return false;
    copy.nativeId = Read<std::uint64_t>(copy.block.data(), 8);
    const auto character = [&](std::size_t i) { return Read<std::uint16_t>(copy.block.data(), 0x1A0 + i * 2); };
    if (character(0) != 'G' || character(1) != 'I' || character(2) != 'P' || character(3) != ':' || character(20) != 0) return false;
    std::uint64_t parsed = 0;
    for (std::size_t i = 4; i < 20; ++i) {
        const auto ch = character(i);
        unsigned digit;
        if (ch >= '0' && ch <= '9') digit = ch - '0';
        else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
        else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
        else return false;
        parsed = (parsed << 4) | digit;
    }
    if (!parsed || parsed != copy.nativeId || std::memcmp(copy.header.data(), bytes, copy.header.size()) ||
        std::memcmp(copy.block.data(), bytes + copy.offset, copy.block.size())) return false;
    output = copy;
    return true;
}
bool Eligible(const DeviceInfo& info)
{
    return info.vendor == 0x045E && (info.product == 0x0B00 || info.product == 0x0B05 || info.product == 0x02E3 || info.product == 0x0B22);
}
bool Budget(std::size_t size, std::size_t total, std::size_t replaced, std::size_t count, bool replacement)
{
    return size < MaxView && replaced <= total && total - replaced < MaxTotal &&
        size < MaxTotal - (total - replaced) && (replacement || count < MaxViews);
}

struct View {
    Api& api;
    std::uint32_t& failures;
    Handle port;
    void* base = nullptr;
    std::size_t size = 0, logical = 0;
    ServiceDevice device;
    Identity identity;
    bool catalog = false, selected = false;
    std::unique_ptr<SharedReader> reader;
    View(Api& functions, std::uint32_t& errors) : api(functions), failures(errors) {}
    void Retire() noexcept
    {
        if (reader) { reader->Retire(); reader.reset(); }
        selected = false;
    }
    ~View() noexcept
    {
        Retire();
        if (base && api.unmap(port.value, 0, std::exchange(base, nullptr)) != nt::Success) CleanupFailure(failures);
    }
};

struct Message {
    alignas(nt::PortMessage) std::array<std::uint8_t, MessageBytes> bytes{};
    alignas(nt::ViewAttribute) std::array<std::uint8_t, 0x48> attributeBytes{};
    nt::PortMessage* Header() { return reinterpret_cast<nt::PortMessage*>(bytes.data()); }
    nt::MessageAttributes* Attributes() { return reinterpret_cast<nt::MessageAttributes*>(attributeBytes.data()); }
    bool Initialize(Api& api) noexcept
    {
        SIZE_T needed = 0;
        Header()->totalLength = sizeof(nt::PortMessage);
        return api.initialize(nt::View | nt::Context, Attributes(), attributeBytes.size(), &needed) == nt::Success && needed <= attributeBytes.size();
    }
};
struct Incoming {
    Api& api;
    HANDLE port;
    std::uint32_t& failures;
    Message& message;
    nt::ViewAttribute view{};
    nt::ContextAttribute context{};
    bool hasView = false, hasContext = false, continuation = false;
    Incoming(Api& functions, HANDLE handle, std::uint32_t& errors, Message& received)
        : api(functions), port(handle), failures(errors), message(received)
    {
        const auto valid = message.Attributes()->valid;
        if (valid & nt::View) {
            if (auto* p = api.attribute(message.Attributes(), nt::View)) { std::memcpy(&view, p, sizeof(view)); hasView = true; }
            else CleanupFailure(failures);
        }
        if (valid & nt::Context) {
            if (auto* p = api.attribute(message.Attributes(), nt::Context)) { std::memcpy(&context, p, sizeof(context)); hasContext = true; }
        }
        continuation = (message.Header()->type & nt::Continuation) != 0;
    }
    ~Incoming() noexcept
    {
        // Each receive record owns its mapping until adoption. Keep the original
        // context for cancellation even if the accept attempt changed the header.
        if (view.base && api.unmap(port, 0, std::exchange(view.base, nullptr)) != nt::Success) CleanupFailure(failures);
        if (continuation) {
            if (!hasContext || api.cancel(port, 0, &context) != nt::Success) CleanupFailure(failures);
        }
    }
    void Accept()
    {
        Require(hasView && view.base, "missing view attribute");
        // phnt ALPC_VIEWFLG_AUTO_RELEASE is incompatible with retaining a view
        // beyond this message. The matched SIPC sender uses view flags zero
        // (GameInput.dll RVA 0x2BC10). Reject this lifetime before completing it.
        Require((view.flags & nt::ViewAutoRelease) == 0, "automatic view release is unsupported");
        message.Header()->type &= static_cast<USHORT>(~nt::Continuation);
        LARGE_INTEGER zero{};
        CheckNt(api.sendReceive(port, 0, message.Header(), message.Attributes(), nullptr, nullptr, nullptr, &zero), "accept view");
        continuation = false;
        // GameInput.dll RVAs 0x2995E, 0x299C7: return-message precedes validation.
        Require(message.Header()->dataLength == 8 && Read<std::uint64_t>(message.bytes.data(), sizeof(nt::PortMessage)) == 0, "unsupported view envelope");
    }
    void* Adopt() noexcept
    {
        message.Attributes()->valid &= ~nt::View;
        return std::exchange(view.base, nullptr);
    }
};
} // namespace service_detail

using namespace service_detail;
struct Client::Impl {
    Api api;
    Handle port;
    Namespace names;
    std::map<SectionId, std::unique_ptr<View>> views;
    std::size_t total = 0;
    std::uint64_t nextGeneration = 0;
    std::uint32_t serverPid = 0, failures = 0;
    ServiceQueryDiagnostics queryDiagnostics;
    ServiceViewDiagnostics viewDiagnostics;
    bool connected = false;

    void Retire(View& view, std::vector<ServicePacket>& packets, const char* reason)
    {
        if (view.selected) {
            ServicePacket packet;
            packet.nativeId = view.device.nativeId;
            packet.viewGeneration = view.device.viewGeneration;
            packet.retired = true;
            packet.reason = reason;
            packets.push_back(std::move(packet));
        }
        view.Retire();
    }
    void RetireAll(std::vector<ServicePacket>& packets, const char* reason)
    {
        for (auto& item : views) Retire(*item.second, packets, reason);
    }
    bool IdentityMatches(View& view)
    {
        Identity current;
        return CaptureIdentity(view.base, view.logical, current) && current == view.identity;
    }
    void ReceiveView(Incoming& incoming, std::vector<ServicePacket>& packets)
    {
        incoming.Accept();
        std::size_t logical = 0;
        SectionId id{};
        Require(Trailer(incoming.view.base, incoming.view.size, logical, id), "invalid view memory or trailer");
        const auto old = views.find(id);
        const auto replaced = old == views.end() ? 0 : old->second->size;
        Require(Budget(incoming.view.size, total, replaced, views.size(), old != views.end()), "view budget exceeded");
        Require(nextGeneration != UINT64_MAX, "view generation exhausted");
        auto view = std::make_unique<View>(api, failures);
        CheckWin(api.duplicate(GetCurrentProcess(), port.value, GetCurrentProcess(), &view->port.value, 0, FALSE, DUPLICATE_SAME_ACCESS), "duplicate view port");
        view->size = incoming.view.size;
        view->logical = logical;
        view->base = incoming.Adopt();
        view->device.sectionId = id;
        view->device.viewGeneration = ++nextGeneration;
        std::string error;
        if (CaptureIdentity(view->base, view->logical, view->identity)) {
            auto metadata = SharedReader::Create(view->base, view->logical, error);
            if (metadata && Eligible(metadata->Info())) {
                view->device.nativeId = view->identity.nativeId;
                view->device.vendor = metadata->Info().vendor;
                view->device.product = metadata->Info().product;
                view->catalog = true;
            }
            // Creation validates immutable metadata. It never polls or pins.
            if (metadata) metadata->Retire();
        }
        if (old != views.end()) {
            Retire(*old->second, packets, "section replaced");
            total -= old->second->size;
            views.erase(old);
        }
        const auto size = view->size;
        views.emplace(id, std::move(view));
        total += size;
        for (auto& a : views) if (a.second->selected) {
            for (const auto& b : views) if (a.first != b.first && b.second->catalog &&
                a.second->device.nativeId == b.second->device.nativeId) {
                Retire(*a.second, packets, "ambiguous native identity");
                break;
            }
        }
    }
    void Receive(std::vector<ServicePacket>& packets)
    {
        for (unsigned n = 0; n < ReceiveBudget; ++n) {
            Message message;
            Require(message.Initialize(api), "ALPC attribute initialization");
            SIZE_T length = message.bytes.size();
            LARGE_INTEGER zero{};
            const auto status = api.sendReceive(port.value, 0, nullptr, nullptr, message.Header(), &length, message.Attributes(), &zero);
            // Matched SipcPort::GetPortEvent, RVAs 0x29AEF-0x29B0A, admits both.
            if (status == nt::Timeout || status == nt::Unsuccessful) return;
            CheckNt(status, "receive");
            Incoming incoming(api, port.value, failures, message);
            if (incoming.hasView) {
                ++viewDiagnostics.received;
                viewDiagnostics.lastBytes = incoming.view.size;
                viewDiagnostics.lastFlags = incoming.view.flags;
                viewDiagnostics.flagsOr |= incoming.view.flags;
            }
            // Coalesce ownership before any path can reject the message. Even
            // a failed accept or malformed envelope must leave one unmap owner.
            for (const auto& item : views) if (item.second->base == incoming.view.base) {
                incoming.Adopt();
                throw std::runtime_error("duplicate mapping base");
            }
            const auto* header = message.Header();
            Require(length <= message.bytes.size() && length >= sizeof(nt::PortMessage) &&
                header->totalLength >= sizeof(nt::PortMessage) && header->totalLength <= length &&
                header->dataLength <= header->totalLength - sizeof(nt::PortMessage), "ALPC message bounds");
            const auto type = header->type & 0xFF;
            // Native GetPortEvent: 0x296D9 sign-extends Type, 0x29A66-0x29A99
            // maps lost replies/closed ports to disconnect, and 0x297FD rejects
            // unsupported types. This client never accepts inbound connections.
            Require((header->type & nt::KernelMessage) == 0, "unsupported kernel-mode ALPC message");
            Require(type != nt::LostReply && type != nt::PortClosed && type != nt::ClientDied, "service peer disconnected");
            // The native type-12 branch receives again (0x298BA-0x2992A).
            // Keep that behavior inside the same bounded receive loop.
            if (type == 12) {
                incoming.continuation = false;
                continue;
            }
            Require(type == nt::Datagram, "unsupported ALPC message type");
            if (type == nt::Datagram && (message.Attributes()->valid & nt::View)) ReceiveView(incoming, packets);
            else if (type == nt::Datagram && header->dataLength == 32) {
                SectionId id{};
                std::memcpy(id.data(), message.bytes.data() + sizeof(nt::PortMessage), id.size());
                if (auto view = views.find(id); view != views.end()) {
                    Retire(*view->second, packets, "section removed");
                    view->second->catalog = false;
                }
            }
        }
        throw std::runtime_error("immediate receive budget exceeded");
    }
    void Append(View& view, std::vector<RawReading>& readings, const std::string& error,
                bool bootstrap, std::vector<ServicePacket>& packets)
    {
        if (!error.empty()) {
            ServicePacket gap;
            gap.nativeId = view.device.nativeId;
            gap.viewGeneration = view.device.viewGeneration;
            gap.gap = true;
            gap.bootstrap = bootstrap;
            gap.reason = error;
            packets.push_back(std::move(gap));
        }
        for (std::size_t i = 0; i < readings.size(); ++i) {
            if (bootstrap && std::any_of(readings.begin() + i + 1, readings.end(), [&](const auto& later) {
                return later.reportId == readings[i].reportId;
            })) continue;
            ServicePacket packet;
            packet.nativeId = view.device.nativeId;
            packet.viewGeneration = view.device.viewGeneration;
            packet.hasReading = true;
            packet.bootstrap = bootstrap;
            packet.reading = std::move(readings[i]);
            packets.push_back(std::move(packet));
        }
    }
    void Disconnect() noexcept
    {
        for (auto& item : views) item.second->Retire();
        if (port.value) {
            if (api.disconnect(port.value, 0) != nt::Success) CleanupFailure(failures);
            // AlpcPort::Disconnect (0x288B4-0x2897E) disconnects before draining.
            // Adopted views retain duplicate port handles for later deletion.
            for (unsigned n = 0; n < ReceiveBudget; ++n) {
                Message message;
                if (!message.Initialize(api)) { CleanupFailure(failures); break; }
                SIZE_T length = message.bytes.size();
                LARGE_INTEGER zero{};
                const auto status = api.sendReceive(port.value, 0, nullptr, nullptr, message.Header(), &length, message.Attributes(), &zero);
                if (status != nt::Success) break;
                Incoming incoming(api, port.value, failures, message);
                for (const auto& item : views) if (item.second->base == incoming.view.base) {
                    incoming.Adopt(); // The existing view already owns this base.
                    break;
                }
                if (n + 1 == ReceiveBudget) CleanupFailure(failures);
            }
        }
        views.clear();
        total = 0;
        port.Reset();
        names.Reset();
        connected = false;
        serverPid = 0;
    }
    bool Abort(std::vector<ServicePacket>& packets, std::string& error, const char* reason)
    {
        try { RetireAll(packets, reason); } catch (...) { /* False also retires every broker route. */ }
        Disconnect();
        return Fail(error, reason);
    }
};

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() { Disconnect(); }
bool Client::Connect(std::string& error)
{
    error.clear();
    if (impl_->connected) return Fail(error, "client already connected");
    impl_->queryDiagnostics = {};
    impl_->viewDiagnostics = {};
    try {
#ifdef SDL_XINPUT_PADDLE_SERVICE_TESTING
        // Offline tests must inject every native operation before Connect.
        if (!impl_->api.Complete()) return Fail(error, "native ALPC API unavailable");
#else
        if (!impl_->api.Load()) return Fail(error, "native ALPC API unavailable");
#endif
        impl_->names.Create();
        DWORD session = 0;
        CheckWin(ProcessIdToSessionId(GetCurrentProcessId(), &session), "process session");
        const std::wstring path = L"\\Sessions\\" + std::to_wstring(session) + L"\\BaseNamedObjects\\SIPC_{85AE2C03-8BBC-4701-B189-CCD20316B884}";
        UNICODE_STRING name{static_cast<USHORT>(path.size() * sizeof(wchar_t)), static_cast<USHORT>((path.size() + 1) * sizeof(wchar_t)), const_cast<PWSTR>(path.c_str())};
        Message message;
        message.Header()->dataLength = 0xC8;
        message.Header()->totalLength = 0xF0;
        auto* payload = message.bytes.data() + sizeof(nt::PortMessage);
        payload[0] = 2;
        payload[2] = 5;
        std::memcpy(payload + 0x30, impl_->names.attributes.data(), impl_->names.attributes.size());
        nt::PortAttributes attributes{};
        // Matched native constants and prototype: connect header 0xF0, port
        // message maximum 0x48, local receive storage 0x170 are distinct fields.
        attributes.flags = 0x00860000;
        attributes.securityQos = {sizeof(SECURITY_QUALITY_OF_SERVICE), SecurityAnonymous, 1, 1};
        attributes.maxMessageLength = 0x48;
        attributes.maxPoolUsage = 0x1000;
        attributes.dupObjectTypes = 0x80;
        auto serverSid = WellKnown(WinLocalSystemSid);
        SIZE_T length = message.bytes.size();
        LARGE_INTEGER timeout{};
        timeout.QuadPart = -30000000; // The prototype's bounded connection attempt.
        const auto status = impl_->api.connect(&impl_->port.value, &name, nullptr, &attributes, 0x20000,
            serverSid.data(), message.Header(), &length, nullptr, nullptr, &timeout);
        if (!impl_->names.DestroyName()) CleanupFailure(impl_->failures);
        CheckNt(status, "connect service");
        Require(impl_->port.value && length <= message.bytes.size() && message.Header()->dataLength == 4 &&
            message.Header()->totalLength >= sizeof(nt::PortMessage) + 4 && message.Header()->totalLength <= length, "connection reply bounds");
        Require(Read<std::uint32_t>(payload, 0) <= MaxViews, "initial view count exceeded");
        nt::ServerSessionInformation server{};
        ULONG returned = 0;
        const auto queryStatus = impl_->api.query(impl_->port.value, 12, &server, sizeof(server), &returned);
        impl_->queryDiagnostics = {true, queryStatus, returned, server.sessionId, server.processId};
        CheckNt(queryStatus, "server process query");
        // phnt class 12 and matched ntoskrnl AlpcpPortQueryServerSessionInfo:
        // this is the peer process's session, independent of the endpoint name.
        // The kernel writes ReturnLength=8 (RVA 0x923791) on this success path.
        // Session is peer metadata, not transport admission policy. The caller
        // performs runtime peer qualification before Select can activate input.
        Require(returned == sizeof(server) && server.processId != 0, "server process identity");
        impl_->serverPid = server.processId;
        impl_->connected = true;
        std::vector<ServicePacket> metadataEvents;
        impl_->Receive(metadataEvents);
        return true;
    } catch (const std::exception& e) {
        impl_->Disconnect();
        return Fail(error, e.what());
    }
}
std::uint32_t Client::ServerPid() const noexcept { return impl_->serverPid; }
ServiceQueryDiagnostics Client::ServerQuery() const noexcept { return impl_->queryDiagnostics; }
ServiceViewDiagnostics Client::ViewDiagnostics() const noexcept { return impl_->viewDiagnostics; }
std::uint32_t Client::CleanupFailures() const noexcept { return impl_->failures; }
std::vector<ServiceDevice> Client::Devices() const
{
    std::vector<ServiceDevice> result;
    for (const auto& item : impl_->views) if (item.second->catalog) result.push_back(item.second->device);
    return result;
}
bool Client::Select(std::uint64_t nativeId, std::uint64_t viewGeneration, bool enabled,
                    std::vector<ServicePacket>& packets, std::string& error)
{
    packets.clear();
    error.clear();
    if (!impl_->connected) return Fail(error, "client is disconnected");
    try {
        View* selected = nullptr;
        unsigned matches = 0;
        for (auto& item : impl_->views) if (item.second->catalog && item.second->device.nativeId == nativeId) {
            ++matches;
            if (item.second->device.viewGeneration == viewGeneration) selected = item.second.get();
        }
        if (!nativeId || !selected || matches != 1) return Fail(error, "selection is stale or ambiguous");
        auto& view = *selected;
        if (!enabled) {
            impl_->Retire(view, packets, "selection disabled");
            return true;
        }
        if (!impl_->IdentityMatches(view)) {
            impl_->Retire(view, packets, "private identity changed");
            view.catalog = false;
            return Fail(error, "private identity changed");
        }
        if (view.selected) return true;
        std::string pollError;
        view.reader = SharedReader::Create(view.base, view.logical, pollError);
        if (!view.reader) { view.catalog = false; return Fail(error, pollError.c_str()); }
        std::vector<RawReading> readings;
        if (!view.reader->Poll(readings, pollError)) {
            if (!pollError.starts_with("retry:")) view.catalog = false;
            view.Retire();
            return Fail(error, pollError.c_str());
        }
        // Build owned bootstrap before activation. It contains no action replay.
        impl_->Append(view, readings, pollError, true, packets);
        if (!impl_->IdentityMatches(view)) {
            packets.clear();
            view.Retire();
            view.catalog = false;
            return Fail(error, "private identity changed during bootstrap");
        }
        std::uint32_t previous = 0;
        bool changed = false;
        if (!view.reader->UseDefaultInputPolicy(previous, changed, error)) {
            packets.clear();
            view.Retire();
            view.catalog = false;
            return false;
        }
        // SharedReader writes only this client's policy and issues its mfence.
        // Signal even if it was already zero after a prior selection.
        _mm_mfence();
        CheckWin(impl_->api.signal(impl_->names.clientSignal.value), "publish client policy");
        view.selected = true;
        return true;
    } catch (const std::exception& e) {
        packets.clear();
        return impl_->Abort(packets, error, e.what());
    }
}
bool Client::Pump(std::vector<ServicePacket>& packets, std::string& error)
{
    packets.clear();
    error.clear();
    if (!impl_->connected) return Fail(error, "client is disconnected");
    try {
        packets.reserve(MaxViews);
        impl_->Receive(packets);
        for (auto& item : impl_->views) {
            auto& view = *item.second;
            if (!view.selected) continue;
            if (!impl_->IdentityMatches(view)) {
                impl_->Retire(view, packets, "private identity changed");
                view.catalog = false;
                continue;
            }
            std::vector<RawReading> readings;
            std::string pollError;
            if (!view.reader->Poll(readings, pollError)) {
                if (pollError.starts_with("retry:")) continue;
                impl_->Retire(view, packets, pollError.c_str());
                view.catalog = false;
                continue;
            }
            // Recheck after Poll too, before releasing any sample to the broker.
            if (!impl_->IdentityMatches(view)) {
                impl_->Retire(view, packets, "private identity changed during poll");
                view.catalog = false;
                continue;
            }
            impl_->Append(view, readings, pollError, false, packets);
        }
        return true;
    } catch (const std::exception& e) {
        return impl_->Abort(packets, error, e.what());
    }
}
void Client::Disconnect() noexcept { impl_->Disconnect(); }
} // namespace sdl_paddles
