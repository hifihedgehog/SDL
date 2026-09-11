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

// Include the implementation to exercise its private protocol/ownership seams.
// Every native ALPC function is replaced before Connect. No live port is used.
#define SDL_XINPUT_PADDLE_SERVICE_TESTING
#include "../src/joystick/windows/SDL_xinput_paddle_service.cpp"
#include "../src/joystick/windows/SDL_xinput_paddle_decode.h"
#include <cstdio>
#include <deque>
#include <functional>

namespace sdl_paddles {
struct ServiceTestAccess {
    static void Inject(Client& client, const service_detail::Api& api) { client.impl_->api = api; }
    static std::size_t Views(const Client& client) { return client.impl_->views.size(); }
};
}

namespace {
using namespace sdl_paddles;
using namespace sdl_paddles::service_detail;
unsigned checks = 0, pins = 0;
const char* group = "startup";
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(std::string(group) + ": " + #x + " line " + std::to_string(__LINE__)); } while (false)

// The pool layout follows sipc-prototype/shared-reader-tests.cpp:29-130.
// The independent GIP block follows transport.cpp:118-150 and its native builder.
struct Fixture {
    static constexpr std::size_t Size = 0x6000, Logical = 0x5000;
    static constexpr std::size_t P = 0x40, M = 0x400, I = 0x800, L = I + 0x2C;
    static constexpr std::size_t Latest = I + 0xD8, Entries = I + 0x120;
    static constexpr std::size_t Data = I + 0x2120, Stride = 0x60, Policy = Data + 4 * Stride;
    std::uint8_t* base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    explicit Fixture(std::uint8_t section = 1, std::uint64_t nativeId = 0x1234)
    {
        CHECK(base != nullptr);
        Put<std::uint32_t>(0, Logical);
        Put<std::uint32_t>(4, P);
        Put<std::uint32_t>(8, M);
        Put<std::uint32_t>(0xC, 0x700);
        Put<std::uint32_t>(0x10, I);
        Put<std::uint32_t>(0x18, 0x4000);
        Identity(nativeId);
        Put<std::uint32_t>(M, 0x200);
        Put<std::uint16_t>(M + 4, 0x045E);
        Put<std::uint16_t>(M + 6, 0x0B00);
        Put<std::uint32_t>(M + 0x68, 1);
        Put<std::uint32_t>(M + 0x70, 2);
        Put<std::uint64_t>(M + 0xA0, 0x148);
        Put<std::uint32_t>(M + 0x14C, 0x20);
        Put<std::uint32_t>(M + 0x150, 46);
        Put<std::uint32_t>(M + 0x164, 0x0C);
        Put<std::uint32_t>(M + 0x168, 17);
        Put<std::uint32_t>(I, Policy + 4 - I);
        Put<std::uint32_t>(I + 4, 0x2C);
        Put<std::uint32_t>(I + 8, 0xD8);
        Put<std::uint32_t>(I + 0xC, 0xE0);
        Put<std::uint32_t>(I + 0x10, 0x120);
        Put<std::uint32_t>(I + 0x18, 4);
        Put<std::uint32_t>(I + 0x1C, Stride);
        Put<std::uint32_t>(I + 0x20, 0x2120);
        Put<std::uint32_t>(I + 0x28, Policy - I);
        Put<std::uint32_t>(L, Stride);
        Put<std::uint32_t>(L + 4, 0x28);
        Put<std::uint16_t>(L + 8, 46);
        Put<std::uint32_t>(L + 0x14, 0x30);
        Put<std::uint8_t>(L + 0x84, 0x20);
        Put<std::uint32_t>(Policy, 0x15);
        Put<std::uint64_t>(Size - 0x28, Logical);
        Put<std::uint8_t>(Size - 0x20, section);
    }
    ~Fixture() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    template<class T> void Put(std::size_t offset, T value) { CHECK(offset + sizeof(T) <= Size); std::memcpy(base + offset, &value, sizeof(value)); }
    template<class T> T Get(std::size_t offset) const { T value{}; std::memcpy(&value, base + offset, sizeof(value)); return value; }
    void Identity(std::uint64_t nativeId)
    {
        Put<std::uint32_t>(P, 0x330);
        Put<std::uint64_t>(P + 8, nativeId);
        char16_t provider[] = u"GIP:0000000000000000";
        constexpr char16_t digits[] = u"0123456789ABCDEF";
        for (unsigned i = 0; i < 16; ++i) provider[19 - i] = digits[(nativeId >> (4 * i)) & 15];
        std::memcpy(base + P + 0x1A0, provider, sizeof(provider));
    }
    void Publish(unsigned slot, std::uint64_t generation, int previous = -1, std::uint8_t mask = 1, unsigned descriptor = 1)
    {
        CHECK(slot < 4 && descriptor < 2);
        const auto entry = Entries + slot * 16, reading = Data + slot * Stride;
        std::memset(base + reading, 0, Stride);
        Put<std::uint64_t>(reading, 1000 + generation);
        Put<std::uint32_t>(reading + 0xC, 1);
        Put<std::uint64_t>(reading + 0x20, 2000 + generation);
        Put<std::uint32_t>(reading + 0x28, descriptor);
        Put<std::uint32_t>(reading + 0x2C, descriptor ? 17 : 46);
        Put<std::uint8_t>(reading + 0x30 + 14, mask);
        Put<std::uint64_t>(entry, (generation << 16) | 1);
        std::uint64_t flags = (1ULL << 29) | 1;
        if (previous >= 0) {
            flags |= (1ULL << 30) | (static_cast<std::uint64_t>(previous) << 32);
            const auto old = Entries + static_cast<std::size_t>(previous) * 16 + 8;
            Put<std::uint64_t>(old, Get<std::uint64_t>(old) | (1ULL << 31) | (static_cast<std::uint64_t>(slot) << 48));
        }
        Put<std::uint64_t>(entry + 8, flags);
        Put<std::uint64_t>(Latest, (generation << 16) | slot);
    }
    void ReferencesReturned() const
    {
        for (unsigned i = 0; i < 4; ++i) CHECK((Get<std::uint64_t>(Entries + i * 16) & 0xFFFF) <= 1);
    }
};

struct Event {
    LONG status = nt::Success;
    USHORT type = nt::Datagram | nt::Continuation, length = 8;
    void* base = nullptr;
    std::size_t size = Fixture::Size;
    ULONG flags = 0;
    std::uint64_t platform = 0;
    SectionId removed{};
};
struct Fake;
Fake* active = nullptr;
struct Fake {
    std::deque<Event> events;
    std::map<void*, unsigned> unmaps;
    std::vector<std::string> order;
    unsigned accepts = 0, cancels = 0, signals = 0, receives = 0, disconnects = 0;
    DWORD session = 0, serverPid = 123;
    ULONG returned = 8;
    LONG queryStatus = nt::Success, acceptStatus = nt::Success, unmapStatus = nt::Success;
    bool infinite = false;
    bool failDuplicate = false;
    Handle senderSection;
    std::function<void()> beforeSignal;
    Fake()
    {
        active = this;
        DWORD own = 0;
        CHECK(ProcessIdToSessionId(GetCurrentProcessId(), &own));
        session = own;
        senderSection.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CHECK(senderSection.value);
    }
    void Add(Fixture& fixture) { Event e; e.base = fixture.base; events.push_back(e); }
    static LONG NTAPI Connect(HANDLE* port, const UNICODE_STRING* name, OBJECT_ATTRIBUTES* object,
        nt::PortAttributes* attributes, ULONG flags, PSID sid, nt::PortMessage* message,
        SIZE_T* size, nt::MessageAttributes* out, nt::MessageAttributes* in, LARGE_INTEGER* timeout)
    {
        CHECK(name && name->Length && object == nullptr && flags == 0x20000 && IsWellKnownSid(sid, WinLocalSystemSid));
        CHECK(message->dataLength == 0xC8 && message->totalLength == 0xF0 && *size == 0x170);
        CHECK(attributes->maxMessageLength == 0x48 && attributes->flags == 0x00860000 && attributes->maxPoolUsage == 0x1000);
        CHECK(attributes->dupObjectTypes == 0x80 && out == nullptr && in == nullptr && timeout->QuadPart == -30000000);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(message) + sizeof(*message);
        CHECK(bytes[0] == 2 && bytes[2] == 5);
        for (unsigned n = 0; n < 0x30; ++n) if (n != 0 && n != 2) CHECK(bytes[n] == 0);
        *port = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CHECK(*port != nullptr);
        const ULONG count = static_cast<ULONG>(active->events.size());
        std::memcpy(reinterpret_cast<std::uint8_t*>(message) + sizeof(*message), &count, 4);
        message->dataLength = 4;
        message->totalLength = sizeof(*message) + 4;
        *size = message->totalLength;
        return nt::Success;
    }
    static LONG NTAPI Query(HANDLE port, ULONG kind, void* data, ULONG size, ULONG* returned)
    {
        CHECK(port && kind == 12 && size == 8 && returned);
        auto* result = static_cast<nt::ServerSessionInformation*>(data);
        *result = {active->session, active->serverPid};
        *returned = active->returned;
        return active->queryStatus;
    }
    static LONG NTAPI Initialize(ULONG flags, nt::MessageAttributes* attributes, SIZE_T bytes, SIZE_T* required)
    {
        CHECK(flags == (nt::View | nt::Context) && bytes == 0x48);
        attributes->allocated = flags;
        attributes->valid = 0;
        *required = 0x48;
        return nt::Success;
    }
    static void* NTAPI Attribute(nt::MessageAttributes* attributes, ULONG flag)
    {
        return reinterpret_cast<std::uint8_t*>(attributes) + (flag == nt::View ? 8 : 0x28);
    }
    static LONG NTAPI SendReceive(HANDLE port, ULONG flags, nt::PortMessage* send, nt::MessageAttributes* sendAttributes,
        nt::PortMessage* receive, SIZE_T* bytes, nt::MessageAttributes* attributes, LARGE_INTEGER* timeout)
    {
        CHECK(port && flags == 0 && timeout && timeout->QuadPart == 0);
        if (send) {
            CHECK(!receive && !bytes && !attributes && sendAttributes && !(send->type & nt::Continuation));
            ++active->accepts;
            active->order.push_back("accept");
            return active->acceptStatus;
        }
        CHECK(receive && bytes && *bytes == 0x170 && attributes);
        ++active->receives;
        Event event;
        if (active->events.empty()) {
            if (!active->infinite) return nt::Timeout;
            event.type = nt::Datagram;
            event.length = 0;
        } else {
            event = active->events.front();
            active->events.pop_front();
        }
        if (event.status != nt::Success) return event.status;
        receive->type = event.type;
        receive->dataLength = event.length;
        receive->totalLength = static_cast<USHORT>(sizeof(*receive) + event.length);
        *bytes = receive->totalLength;
        attributes->valid = nt::Context;
        *static_cast<nt::ContextAttribute*>(Attribute(attributes, nt::Context)) = {nullptr, nullptr, 1, 77, 2};
        if (event.base) {
            attributes->valid |= nt::View;
            *static_cast<nt::ViewAttribute*>(Attribute(attributes, nt::View)) = {event.flags, active->senderSection.value, event.base, event.size};
        }
        auto* payload = reinterpret_cast<std::uint8_t*>(receive) + sizeof(*receive);
        if (event.length == 32) std::memcpy(payload, event.removed.data(), 32);
        else if (event.length >= 8 && event.length <= 0x148) std::memcpy(payload, &event.platform, 8);
        return nt::Success;
    }
    static LONG NTAPI Unmap(HANDLE port, ULONG flags, void* base)
    {
        CHECK(port && !flags && base);
        ++active->unmaps[base];
        active->order.push_back("unmap");
        return active->unmapStatus;
    }
    static LONG NTAPI Cancel(HANDLE, ULONG flags, nt::ContextAttribute* context)
    {
        CHECK(!flags && context->messageId == 77);
        ++active->cancels;
        active->order.push_back("cancel");
        return nt::Success;
    }
    static LONG NTAPI Disconnect(HANDLE, ULONG flags)
    {
        CHECK(!flags);
        ++active->disconnects;
        active->order.push_back("disconnect");
        return nt::Success;
    }
    static BOOL WINAPI Signal(HANDLE)
    {
        ++active->signals;
        if (active->beforeSignal) active->beforeSignal();
        return TRUE;
    }
    static BOOL WINAPI Duplicate(HANDLE sourceProcess, HANDLE source, HANDLE targetProcess,
        HANDLE* target, DWORD access, BOOL inherit, DWORD options)
    {
        if (active->failDuplicate) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        return DuplicateHandle(sourceProcess, source, targetProcess, target, access, inherit, options);
    }
    Api Functions() const
    {
        return {Connect, Disconnect, Query, Unmap, SendReceive, Cancel, Initialize, Attribute, Signal, Duplicate};
    }
};

void Connected(Client& client, Fake& fake)
{
    ServiceTestAccess::Inject(client, fake.Functions());
    std::string error;
    CHECK(client.Connect(error));
    CHECK(error.empty());
}
ServiceDevice Device(Client& client, std::uint64_t id = 0x1234)
{
    const auto list = client.Devices();
    const auto found = std::find_if(list.begin(), list.end(), [id](const auto& d) { return d.nativeId == id; });
    CHECK(found != list.end());
    return *found;
}
void Select(Client& client, const ServiceDevice& device, std::vector<ServicePacket>& packets, bool enabled = true)
{
    std::string error;
    CHECK(client.Select(device.nativeId, device.viewGeneration, enabled, packets, error));
    CHECK(error.empty());
}

void IdentityBounds()
{
    Fixture f;
    Identity identity;
    CHECK(CaptureIdentity(f.base, f.Logical, identity) && identity.nativeId == 0x1234);
    for (auto bad : {0u, 0x32Fu, 0x331u, UINT32_MAX}) {
        f.Put<std::uint32_t>(f.P, bad);
        CHECK(!CaptureIdentity(f.base, f.Logical, identity) && identity.nativeId == 0);
    }
    f.Identity(0xabcdef);
    CHECK(CaptureIdentity(f.base, f.Logical, identity));
    f.Put<std::uint16_t>(f.P + 0x1A0 + 19 * 2, u'f');
    CHECK(CaptureIdentity(f.base, f.Logical, identity));
    for (unsigned n = 0; n <= 20; ++n) {
        const auto at = f.P + 0x1A0 + n * 2;
        const auto old = f.Get<std::uint16_t>(at);
        f.Put<std::uint16_t>(at, 0x100);
        CHECK(!CaptureIdentity(f.base, f.Logical, identity));
        f.Put(at, old);
    }
    f.Put<std::uint64_t>(f.P + 8, 1);
    CHECK(!CaptureIdentity(f.base, f.Logical, identity));
    f.Identity(0);
    CHECK(!CaptureIdentity(f.base, f.Logical, identity));
    f.Identity(0x1234);
    for (auto offset : {4u, 8u, 0xCu, 0x10u, 0x18u}) {
        const auto old = f.Get<std::uint32_t>(offset);
        for (auto bad : {1u, 0x20u, 0x100u, static_cast<unsigned>(f.Logical), UINT32_MAX}) {
            f.Put<std::uint32_t>(offset, bad);
            CHECK(!CaptureIdentity(f.base, f.Logical, identity));
        }
        f.Put(offset, old);
    }
    CHECK(!CaptureIdentity(nullptr, f.Logical, identity));
    CHECK(!CaptureIdentity(f.base, 0x23, identity));
    CHECK(!CaptureIdentity(f.base + 8, f.Logical, identity));
    f.Put<std::uint32_t>(0, UINT32_MAX);
    CHECK(!CaptureIdentity(f.base, f.Logical, identity));
}

SIZE_T WINAPI CrossAllocation(const void* address, MEMORY_BASIC_INFORMATION* info, SIZE_T)
{
    const auto base = reinterpret_cast<std::uintptr_t>(address);
    *info = {};
    info->BaseAddress = reinterpret_cast<void*>(base);
    info->AllocationBase = reinterpret_cast<void*>(base);
    info->RegionSize = 4096;
    info->State = MEM_COMMIT;
    info->Protect = PAGE_READWRITE;
    return sizeof(*info);
}
void MemoryAndBudget()
{
    Fixture f;
    std::size_t logical;
    SectionId id;
    CHECK(Trailer(f.base, f.Size, logical, id) && logical == f.Logical && id[0] == 1);
    DWORD old;
    CHECK(VirtualProtect(f.base + 0x5000, 0x1000, PAGE_NOACCESS, &old));
    CHECK(!Trailer(f.base, f.Size, logical, id));
    CHECK(VirtualProtect(f.base + 0x5000, 0x1000, PAGE_READWRITE, &old));
    CHECK(VirtualProtect(f.base, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old));
    Identity identity;
    CHECK(!CaptureIdentity(f.base, f.Logical, identity));
    MEMORY_BASIC_INFORMATION info{};
    CHECK(VirtualQuery(f.base, &info, sizeof(info)) && (info.Protect & PAGE_GUARD));
    CHECK(VirtualProtect(f.base, 0x1000, PAGE_READWRITE, &old));
    CHECK(VirtualProtect(f.base + 0x5000, 0x1000, PAGE_READONLY, &old));
    CHECK(!Trailer(f.base, f.Size, logical, id));
    CHECK(VirtualProtect(f.base + 0x5000, 0x1000, PAGE_READWRITE, &old));
    CHECK(!Writable(reinterpret_cast<void*>(0x10000), 8192, CrossAllocation));
    CHECK(!Writable(reinterpret_cast<void*>(UINTPTR_MAX - 3), 8));
    CHECK(!Trailer(f.base, MaxView, logical, id));
    f.Put<std::uint64_t>(f.Size - 40, UINT64_MAX);
    CHECK(!Trailer(f.base, f.Size, logical, id));
    f.Put<std::uint64_t>(f.Size - 40, f.Logical);
    f.Put<std::uint8_t>(f.Size - 32, 0);
    CHECK(!Trailer(f.base, f.Size, logical, id));
    CHECK(Budget(MaxView - 1, 0, 0, 0, false));
    CHECK(!Budget(MaxView, 0, 0, 0, false));
    CHECK(!Budget(1, MaxTotal - 1, 0, 0, false));
    CHECK(Budget(1, MaxTotal - 1, 2, 128, true));
    CHECK(!Budget(1, 0, 1, 0, false));
    CHECK(!Budget(1, 0, 0, 128, false));
}

void MetadataQuery()
{
    Api native;
    CHECK(native.Load() && native.Complete()); // Resolve system ntdll only. No ALPC call.
    Client absent;
    std::string error;
    CHECK(!absent.Connect(error) && !absent.ServerQuery().attempted);
    for (unsigned variant = 0; variant < 5; ++variant) {
        Fake fake;
        Client client;
        ServiceTestAccess::Inject(client, fake.Functions());
        if (variant == 1) fake.returned = 0;
        if (variant == 2) fake.serverPid = 0;
        if (variant == 3) fake.queryStatus = static_cast<LONG>(0xC0000003UL);
        if (variant == 4) fake.session ^= 1u; // Peer session is metadata, not transport admission policy.
        const bool connected = client.Connect(error);
        if (variant == 4 && !connected) std::printf("PEER_SESSION metadata_rejected error=%s\n", error.c_str());
        CHECK(connected == (variant == 0 || variant == 4));
        const auto query = client.ServerQuery();
        CHECK(query.attempted && query.returnedBytes == fake.returned && query.serverSession == fake.session && query.serverPid == fake.serverPid);
        CHECK(query.status == fake.queryStatus);
        if (variant == 0 || variant == 4) CHECK(client.ServerPid() == fake.serverPid);
        client.Disconnect();
        CHECK(client.ServerPid() == 0 && client.ServerQuery().attempted && fake.disconnects == 1);
        CHECK(fake.signals == 0 && client.CleanupFailures() == 0);
    }
}

void BootstrapAndEdges()
{
    Fixture f;
    f.Publish(0, 1, -1, 0, 0);
    f.Publish(1, 2, 0, 1);
    f.Publish(2, 3, 1, 2);
    const std::vector<std::uint8_t> initial(f.base, f.base + f.Size);
    const auto initialPins = pins;
    Fake fake;
    fake.Add(f);
    Client client;
    Connected(client, fake);
    CHECK(client.ViewDiagnostics().received == 1 && client.ViewDiagnostics().lastBytes == f.Size && client.ViewDiagnostics().flagsOr == 0);
    CHECK(pins == initialPins && std::memcmp(initial.data(), f.base, f.Size) == 0);
    auto device = Device(client);
    CHECK(device.vendor == 0x045E && device.product == 0x0B00);
    auto copied = client.Devices(); copied[0].nativeId = 0;
    CHECK(Device(client).nativeId == device.nativeId);
    std::vector<ServicePacket> packets;
    std::string error;
    CHECK(!client.Select(device.nativeId, device.viewGeneration + 1, true, packets, error));
    CHECK(f.Get<std::uint32_t>(f.Policy) == 0x15 && fake.signals == 0);
    fake.beforeSignal = [&] { CHECK(pins > initialPins && f.Get<std::uint32_t>(f.Policy) == 0); f.ReferencesReturned(); };
    Select(client, device, packets);
    CHECK(packets.size() == 2 && packets[0].bootstrap && packets[1].bootstrap);
    CHECK(packets[0].reading.generation == 1 && packets[1].reading.generation == 3);
    CHECK(packets[0].reading.reportId == 0x20 && packets[1].reading.bytes[14] == 2);
    CHECK(fake.signals == 1);
    Select(client, device, packets);
    CHECK(packets.empty() && fake.signals == 1);
    f.Publish(3, 4, 2, 4);
    CHECK(client.Pump(packets, error) && error.empty());
    CHECK(packets.size() == 1 && packets[0].reading.generation == 4 && !packets[0].bootstrap);
    f.base[f.Data + 3 * f.Stride + 0x30 + 14] = 0;
    CHECK(packets[0].reading.bytes[14] == 4); // Output owns its payload.
    CHECK(client.Pump(packets, error) && packets.empty());
    Select(client, device, packets, false);
    CHECK(packets.size() == 1 && packets[0].retired && f.Get<std::uint32_t>(f.Policy) == 0);
    const auto before = pins;
    CHECK(client.Pump(packets, error) && packets.empty() && pins == before);
    Select(client, device, packets);
    CHECK(packets.size() == 2 && packets[0].bootstrap && packets[1].reading.generation == 4);
    f.ReferencesReturned();
    client.Disconnect();
    CHECK(fake.unmaps[f.base] == 1 && client.CleanupFailures() == 0);
    DWORD handleFlags;
    CHECK(GetHandleInformation(fake.senderSection.value, &handleFlags));
    CHECK(std::find(fake.order.begin(), fake.order.end(), "disconnect") < std::find(fake.order.begin(), fake.order.end(), "unmap"));
}

void ReplacementAndIsolation()
{
    Fixture a(1, 0x1234), b(2, 0x5678), replacement(1, 0x1234);
    a.Publish(0, 1); b.Publish(0, 1);
    Fake fake; fake.Add(a); fake.Add(b);
    Client client; Connected(client, fake);
    const auto old = Device(client), other = Device(client, 0x5678);
    std::vector<ServicePacket> packets; std::string error;
    Select(client, old, packets); Select(client, other, packets);
    fake.Add(replacement); b.Publish(1, 2, 0, 8);
    CHECK(client.Pump(packets, error));
    CHECK(packets.size() == 2 && packets[0].retired && packets[0].viewGeneration == old.viewGeneration);
    CHECK(packets[1].nativeId == other.nativeId && packets[1].hasReading);
    const auto newer = Device(client);
    CHECK(newer.viewGeneration > old.viewGeneration && fake.unmaps[a.base] == 1);
    CHECK(replacement.Get<std::uint32_t>(replacement.Policy) == 0x15);
    CHECK(!client.Select(old.nativeId, old.viewGeneration, true, packets, error));
    Select(client, newer, packets);
    replacement.Identity(0x1111);
    b.Publish(2, 3, 1, 2);
    CHECK(client.Pump(packets, error));
    CHECK(packets.size() == 2 && packets[0].retired && packets[1].nativeId == other.nativeId);
    b.Put<std::uint32_t>(b.M + 0x68, 3); // SharedReader's immutable check must retire only this view.
    CHECK(client.Pump(packets, error) && packets.size() == 1 && packets[0].retired);
    CHECK(ServiceTestAccess::Views(client) == 2 && client.Devices().empty());
    client.Disconnect();
    CHECK(fake.unmaps[a.base] == 1 && fake.unmaps[b.base] == 1 && fake.unmaps[replacement.base] == 1);
}

void GapRemovalPeerLoss()
{
    Fixture a, b(2, 0x5678);
    a.Publish(0, 1); b.Publish(0, 1);
    Fake fake; fake.Add(a); fake.Add(b);
    Client client; Connected(client, fake);
    const auto first = Device(client), second = Device(client, 0x5678);
    std::vector<ServicePacket> packets; std::string error;
    Select(client, first, packets); Select(client, second, packets);
    a.Publish(1, 5, -1, 8);
    CHECK(client.Pump(packets, error) && packets.size() == 2);
    CHECK(packets[0].gap && !packets[0].hasReading && packets[1].reading.generation == 5);
    Event remove; remove.length = 32; remove.removed = first.sectionId; fake.events.push_back(remove);
    CHECK(client.Pump(packets, error) && packets.size() == 1 && packets[0].retired);
    CHECK(fake.unmaps[a.base] == 0 && ServiceTestAccess::Views(client) == 2);
    CHECK(!client.Select(first.nativeId, first.viewGeneration, true, packets, error));
    Event peer; peer.status = nt::PortDisconnected; fake.events.push_back(peer);
    CHECK(!client.Pump(packets, error) && packets.size() == 1 && packets[0].nativeId == second.nativeId && packets[0].retired);
    CHECK(fake.unmaps[a.base] == 1 && fake.unmaps[b.base] == 1);
}

void RejectionOwnership()
{
    for (unsigned variant = 0; variant < 6; ++variant) {
        Fixture f; Fake fake; Event event; event.base = f.base;
        if (variant == 0) event.length = 4; // Native acceptance precedes this shape check.
        if (variant == 1) event.platform = 1;
        if (variant == 2) fake.acceptStatus = static_cast<LONG>(0xC0000022UL);
        if (variant == 3) event.flags = nt::ViewAutoRelease;
        if (variant == 4) f.Put<std::uint64_t>(f.Size - 40, UINT64_MAX);
        if (variant == 5) fake.failDuplicate = true;
        fake.events.push_back(event);
        Client client; ServiceTestAccess::Inject(client, fake.Functions());
        std::string error;
        CHECK(!client.Connect(error) && !error.empty());
        CHECK(fake.unmaps[f.base] == 1);
        CHECK(fake.accepts == (variant == 3 ? 0u : 1u));
        CHECK(fake.cancels == ((variant == 2 || variant == 3) ? 1u : 0u));
        client.Disconnect(); CHECK(fake.unmaps[f.base] == 1);
    }
    for (unsigned variant = 0; variant < 3; ++variant) {
        Fixture f; Fake fake; fake.Add(f);
        Client client; Connected(client, fake);
        std::vector<ServicePacket> packets; std::string error;
        Select(client, Device(client), packets);
        Event duplicate; duplicate.base = f.base;
        if (variant == 1) fake.acceptStatus = static_cast<LONG>(0xC0000022UL);
        if (variant == 2) duplicate.flags = nt::ViewAutoRelease;
        fake.events.push_back(duplicate);
        CHECK(!client.Pump(packets, error) && packets.size() == 1 && packets[0].retired);
        if (fake.unmaps[f.base] != 1) std::printf("DUPLICATE variant=%u unmap_calls=%u expected=1\n", variant, fake.unmaps[f.base]);
        CHECK(fake.unmaps[f.base] == 1 && client.CleanupFailures() == 0);
    }
    {
        Fixture f; Fake fake; fake.Add(f);
        Client client; Connected(client, fake);
        fake.Add(f); // The same ownership rule also applies to the disconnect drain.
        client.Disconnect();
        CHECK(fake.unmaps[f.base] == 1 && client.CleanupFailures() == 0 && fake.cancels == 1);
    }
}

void ControlMessagesRetire()
{
    const USHORT types[] = {4, 5, 6, 1, 2, 7, 8, 9, 10, 11, 0x8003};
    for (const auto type : types) {
        Fixture f; f.Publish(0, 1);
        Fake fake; fake.Add(f);
        Client client; Connected(client, fake);
        std::vector<ServicePacket> packets; std::string error;
        Select(client, Device(client), packets);
        CHECK(!packets.empty() && packets.back().hasReading); // Active same-window control.
        Event event; event.type = static_cast<USHORT>(type | nt::Continuation); event.length = 0;
        fake.events.push_back(event);
        const bool okay = client.Pump(packets, error);
        if (okay) std::printf("CONTROL type=%04x ignored=1 retired=0\n", unsigned(type));
        CHECK(!okay && packets.size() == 1 && packets[0].retired);
        CHECK(fake.cancels == 1 && fake.unmaps[f.base] == 1);
    }
    Fake fake; Client client; Connected(client, fake);
    Event receiveAgain; receiveAgain.type = 12 | nt::Continuation; receiveAgain.length = 0;
    fake.events.push_back(receiveAgain);
    Event benign; benign.type = nt::Datagram | nt::Continuation | 0x4000; benign.length = 0;
    fake.events.push_back(benign);
    std::vector<ServicePacket> packets; std::string error;
    CHECK(client.Pump(packets, error) && packets.empty());
    CHECK(fake.cancels == 1); // Native type 12 receives again without canceling it.
}

void DuplicateIdentityAndEligibility()
{
    Fixture a, duplicate(2, 0x1234), excluded(3, 0x9876);
    excluded.Put<std::uint16_t>(excluded.M + 4, 0x1111);
    Fake fake; fake.Add(a); fake.Add(excluded);
    Client client; Connected(client, fake);
    CHECK(client.Devices().size() == 1 && ServiceTestAccess::Views(client) == 2);
    std::vector<ServicePacket> packets; std::string error;
    const auto device = Device(client);
    Select(client, device, packets);
    fake.Add(duplicate);
    CHECK(client.Pump(packets, error) && packets.size() == 1 && packets[0].retired);
    CHECK(client.Devices().size() == 2);
    CHECK(!client.Select(device.nativeId, device.viewGeneration, true, packets, error));
    CHECK(duplicate.Get<std::uint32_t>(duplicate.Policy) == 0x15 && excluded.Get<std::uint32_t>(excluded.Policy) == 0x15);
}

void ReceiveLimitsAndCleanup()
{
    {
        Fake fake; Client client; Connected(client, fake);
        Event empty; empty.status = nt::Unsuccessful; fake.events.push_back(empty);
        std::vector<ServicePacket> packets; std::string error;
        CHECK(client.Pump(packets, error) && error.empty());
        fake.infinite = true;
        const auto before = fake.receives;
        CHECK(!client.Pump(packets, error));
        CHECK(fake.receives - before == 2 * ReceiveBudget && client.CleanupFailures() == 1);
    }
    {
        Fixture f; Fake fake; fake.Add(f);
        Client client; Connected(client, fake);
        fake.unmapStatus = nt::PortDisconnected;
        client.Disconnect();
        CHECK(client.CleanupFailures() == 1 && fake.unmaps[f.base] == 1);
        client.Disconnect(); CHECK(client.CleanupFailures() == 1 && fake.unmaps[f.base] == 1);
    }
    {
        std::vector<std::unique_ptr<Fixture>> fixtures;
        Fake fake;
        for (unsigned i = 1; i <= 128; ++i) {
            fixtures.push_back(std::make_unique<Fixture>(static_cast<std::uint8_t>(i), i));
            fake.Add(*fixtures.back());
        }
        Client client; Connected(client, fake);
        CHECK(ServiceTestAccess::Views(client) == 128);
        Fixture overflow(129, 129); fake.Add(overflow);
        std::vector<ServicePacket> packets; std::string error;
        CHECK(!client.Pump(packets, error) && error.find("budget") != std::string::npos);
        CHECK(fake.unmaps[overflow.base] == 1);
        for (const auto& f : fixtures) CHECK(fake.unmaps[f->base] == 1);
    }
}

void DecoderLinkage()
{
    SDL_XInputPaddleState state;
    SDL_XInputPaddleReset(&state, SDL_XINPUT_PADDLE_SERVICE_GIP, 1);
    const std::uint8_t data[17] = {0};
    const auto result = SDL_XInputPaddleDecode(&state, SDL_XINPUT_PADDLE_SERVICE_GIP, 1, 0x0C, data, sizeof(data));
    CHECK(result.flags & SDL_XINPUT_PADDLE_SEPARATE_FRAME);
}
} // namespace

namespace sdl_paddles::reader_testing {
void Observe(const char* stage, void*, std::uint64_t)
{
    if (std::strcmp(stage, "pinned") == 0) ++pins;
}
}

int main()
{
    const std::pair<const char*, void(*)()> tests[] = {
        {"private identity bounds", IdentityBounds}, {"memory and view budgets", MemoryAndBudget},
        {"server query metadata", MetadataQuery}, {"bootstrap and owned edges", BootstrapAndEdges},
        {"replacement and reader isolation", ReplacementAndIsolation}, {"gap, removal and peer loss", GapRemovalPeerLoss},
        {"rejection ownership", RejectionOwnership}, {"duplicate identity and catalog filter", DuplicateIdentityAndEligibility},
        {"control messages retire sources", ControlMessagesRetire},
        {"receive bounds and cleanup failures", ReceiveLimitsAndCleanup}, {"C decoder linkage", DecoderLinkage}
    };
    try {
        for (const auto& test : tests) { group = test.first; test.second(); std::printf("PASS %s\n", group); }
        std::printf("PASS: %zu groups, %u checks. No live ALPC calls.\n", std::size(tests), checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
