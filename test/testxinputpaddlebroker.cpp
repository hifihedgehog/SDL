/* Offline tests of the production broker with owned, injected sources. */
#define SDL_XINPUT_PADDLE_BROKER_TESTING
#include "../src/joystick/windows/SDL_xinput_paddle.cpp"
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <stdexcept>
#include <thread>

using namespace sdl_paddles;
using namespace sdl_paddles::broker_detail;

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) throw std::runtime_error(#condition); } while (0)

namespace {
struct Injection {
    bool runtime = true, server = true, connect = true, pump = true, select = true;
    bool throwDevices = false, gattStart = true, gattPump = true;
    unsigned constructs = 0, connects = 0, disconnects = 0, selects = 0, deselects = 0;
    unsigned resolves = 0, qualifications = 0, submits = 0, gattStarts = 0, gattRetires = 0;
    unsigned rejectedSubmissions = 0, acceptedSubmissions = 0;
    unsigned observations = 0;
    bool queryInput = true, pollInput = true;
    std::uint64_t nextRequest = 10000, lastRequest = 0;
    std::map<std::uint64_t, GipInputState> input;
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> accepted;
    std::map<std::uint64_t, GipEnableResult> replies;
    std::function<void()> inSubmit;
    unsigned orphanPumps = 0, orphans = 0;
    std::uint64_t lastSource = 0;
    GipEnableStatus result = GipEnableStatus::Pending;
    std::map<std::wstring, PhysicalIdentity> identities;
    std::vector<ServiceDevice> catalog;
    std::map<std::uint64_t, std::vector<ServicePacket>> bootstrap;
    std::deque<std::vector<ServicePacket>> batches;
    std::deque<std::vector<GattPaddlePacket>> gattBatches;
    std::vector<std::uint64_t> retired;
    std::vector<std::string> calls;
    std::function<void()> inSelect;
} injection;

struct FakeService {
    FakeService() { ++injection.constructs; }
    bool Connect(std::string &error) {
        injection.calls.emplace_back("connect");
        ++injection.connects;
        if (!injection.connect) error = "Injected connection failure.";
        return injection.connect;
    }
    std::uint32_t ServerPid() const noexcept { return 42; }
    std::vector<ServiceDevice> Devices() const {
        if (injection.throwDevices) throw std::bad_alloc();
        return injection.catalog;
    }
    bool Select(std::uint64_t native, std::uint64_t, bool enabled,
                std::vector<ServicePacket> &packets, std::string &error) {
        packets.clear();
        if (!enabled) { ++injection.deselects; return true; }
        ++injection.selects;
        injection.calls.emplace_back("select");
        if (injection.inSelect) injection.inSelect();
        if (!injection.select) { error = "Injected selection failure."; return false; }
        packets = injection.bootstrap[native];
        return true;
    }
    bool Pump(std::vector<ServicePacket> &packets, std::string &error) {
        packets.clear();
        if (!injection.pump) { error = "Injected empty failure batch."; return false; }
        if (!injection.batches.empty()) {
            packets = std::move(injection.batches.front());
            injection.batches.pop_front();
        }
        return true;
    }
    void Disconnect() noexcept { ++injection.disconnects; }
};

struct FakeGatt {
    std::uint64_t token;
    bool retired = false;
    FakeGatt(GUID, std::uint64_t generation) noexcept : token(generation) { injection.lastSource = generation; }
    ~FakeGatt() { if (retired) ++injection.orphans; }
    bool Start(GattPaddleError &) noexcept { ++injection.gattStarts; return injection.gattStart; }
    bool Pump(std::vector<GattPaddlePacket> &packets, GattPaddleError &) {
        packets.clear();
        if (!injection.gattBatches.empty()) {
            packets = std::move(injection.gattBatches.front());
            injection.gattBatches.pop_front();
            for (auto &packet : packets) if (!packet.attachmentGeneration) packet.attachmentGeneration = token;
        }
        return injection.gattPump;
    }
    void Retire() noexcept { retired = true; ++injection.gattRetires; }
};

struct FakePlatform {
    using Service = FakeService;
    using Gatt = FakeGatt;
    static bool Resolve(const wchar_t *path, std::uint32_t index, std::uint32_t count,
                        PhysicalIdentity &identity, std::string &error) {
        ++injection.resolves;
        const auto found = injection.identities.find(path);
        if (index != 0 || count != 1 || found == injection.identities.end()) {
            error = "Injected identity failure.";
            return false;
        }
        identity = found->second;
        return true;
    }
    static bool RuntimeAvailable(std::string &error) {
        injection.calls.emplace_back("runtime");
        if (!injection.runtime) error = "Injected runtime rejection.";
        return injection.runtime;
    }
    static bool ValidateServer(std::uint32_t pid, std::string &error) {
        ++injection.qualifications;
        injection.calls.emplace_back("server");
        if (!injection.server) error = "Injected peer rejection.";
        return pid == 42 && injection.server;
    }
    static std::uint64_t PublicationQpc(std::uint64_t now) noexcept { return now; }
    static bool InputState(std::uint64_t native, GipInputState &state) {
        ++injection.observations;
        state = injection.input[native];
        return injection.queryInput;
    }
    static std::uint64_t Submit(std::uint64_t native, std::uint64_t token,
                               std::uint64_t provider, std::uint64_t epoch) {
        ++injection.submits;
        injection.lastSource = token;
        if (injection.inSubmit) injection.inSubmit();
        const auto &state = injection.input[native];
        if (!state.Ready() || state.busy || state.provider != provider || state.epoch != epoch) return 0;
        if (injection.rejectedSubmissions) { --injection.rejectedSubmissions; return 0; }
        ++injection.acceptedSubmissions;
        injection.lastRequest = ++injection.nextRequest;
        injection.accepted[injection.lastRequest] = {provider, epoch};
        return injection.lastRequest;
    }
    static bool Poll(std::uint64_t token, GipEnableResult &result) noexcept {
        if (!injection.pollInput) return false;
        const auto found = injection.accepted.find(token);
        if (found == injection.accepted.end()) return false;
        const auto reply = injection.replies.find(token);
        if (reply != injection.replies.end()) result = reply->second;
        else {
            result.status = injection.result;
            result.hresult = injection.result == GipEnableStatus::Failed ? E_FAIL : S_OK;
            result.provider = found->second.first; result.epoch = found->second.second;
            result.dispatched = injection.result == GipEnableStatus::Success || injection.result == GipEnableStatus::Failed || injection.result == GipEnableStatus::TimedOut;
        }
        return true;
    }
    static void RetireEnable(std::uint64_t token) noexcept { injection.retired.push_back(token); }
    static unsigned PumpRetired(GattPaddleError &) noexcept { ++injection.orphanPumps; return injection.orphans; }
};

struct Fixture {
    std::unique_ptr<Shared> shared = std::make_unique<Shared>();
    Owner<FakePlatform> owner{*shared};
    Fixture() { injection = {}; shared->frequency = 10000000; }
    std::shared_ptr<Attachment> Attach(unsigned n, PaddleTransport transport = PaddleTransport::UsbGip) {
        auto a = std::make_shared<Attachment>();
        a->token = shared->NewToken();
        a->generation = 500 + n;
        a->instanceId = n;
        a->user = static_cast<std::uint8_t>(n);
        a->count = 1;
        a->path = L"interface" + std::to_wstring(n);
        a->physical.transport = transport;
        a->physical.container.Data1 = 100 + n;
        a->physical.nativeId = transport == PaddleTransport::UsbGip ? 1000 + n : 0;
        if (transport == PaddleTransport::UsbGip) { a->physical.vendor = 0x045e; a->physical.product = 0x0b00; }
        a->physical.instance = L"HID\\VID_045E&PID_0B00&IG_00\\" + std::to_wstring(n);
        injection.identities[a->path] = a->physical;
        CHECK(shared->Attach(a) == n);
        return a;
    }
    void Catalog(const Attachment &a, std::uint64_t view = 9) {
        ServiceDevice device;
        device.nativeId = a.physical.nativeId;
        device.viewGeneration = view;
        device.vendor = 0x045e;
        device.product = 0x0b00;
        injection.catalog.push_back(device);
    }
    void Ready(const Attachment &a, std::uint64_t epoch = 1, std::uint64_t provider = 0) {
        auto &state = injection.input[a.physical.nativeId];
        state = {};
        state.provider = provider ? provider : a.physical.nativeId;
        state.epoch = epoch;
        state.resumed = state.normalSeen = true;
        state.resumeTime = epoch * 100;
        state.normalTime = epoch * 100 + 10;
    }
    std::vector<Edge> Drain(const Attachment &a, bool ready = true, Diagnostics *diagnostics = nullptr) {
        std::array<Edge, EventLimit + 2> edges{};
        Diagnostics d;
        const auto count = shared->Drain(a.user, a.token, edges.data(), edges.size(), d, ready);
        if (diagnostics) *diagnostics = d;
        return {edges.begin(), edges.begin() + count};
    }
};

ServicePacket Packet(const Attachment &a, std::uint64_t generation, std::uint8_t mask,
                     bool bootstrap = false, std::uint32_t id = 0x0c, size_t size = 17, std::uint64_t view = 9)
{
    ServicePacket packet;
    packet.nativeId = a.physical.nativeId;
    packet.viewGeneration = view;
    packet.hasReading = true;
    packet.bootstrap = bootstrap;
    packet.reading.generation = generation;
    packet.reading.timestamp = generation * 100;
    packet.reading.reportId = id;
    packet.reading.bytes.resize(size);
    if (size > 14) packet.reading.bytes[14] = mask;
    if (size == 46) packet.reading.bytes[18] = mask;
    if (size == 29) packet.reading.bytes[28] = mask;
    return packet;
}

void QueueOrder()
{
    EventQueue queue;
    for (unsigned i = 0; i < EventLimit; ++i) CHECK(queue.Push({i, static_cast<std::uint8_t>(i % 16), false}));
    Edge value;
    for (unsigned i = 0; i < EventLimit; ++i) {
        CHECK(queue.Pop(value));
        CHECK(value.qpc == i && value.mask == i % 16 && !value.release);
    }
    CHECK(!queue.Pop(value));
    CHECK(queue.Push({1, 3, false}));
    CHECK(queue.Pop(value) && value.qpc == EventLimit - 1);
}

void Overflow()
{
    EventQueue queue;
    for (unsigned i = 0; i < EventLimit; ++i) queue.Push({i, 1, false});
    CHECK(!queue.Push({2000, 15, false}));
    CHECK(queue.Push({2001, 0, false}));
    Edge value;
    CHECK(queue.Pop(value) && value.release && value.mask == 0);
    CHECK(queue.Pop(value) && !value.release && value.mask == 15 && value.qpc == 2000);
    CHECK(queue.Pop(value) && value.mask == 0 && value.qpc == 2001);
    CHECK(!queue.Pop(value));
    queue.Clear();
    for (unsigned i = 0; i < 3 * EventLimit; ++i) queue.Push({i, static_cast<std::uint8_t>(i % 16), false});
    CHECK(queue.Pop(value) && value.release);
}

void TokensAndStartupDrain()
{
    Fixture f;
    auto a = f.Attach(0);
    f.shared->Publish(0, a->token, {20, 1, false});
    f.shared->Publish(0, a->token, {21, 0, false});
    CHECK(f.Drain(*a, false).empty());
    auto values = f.Drain(*a);
    CHECK(values.size() == 2 && values[0].mask == 1 && values[1].mask == 0);
    f.shared->Detach(0, a->token);
    auto b = f.Attach(0);
    CHECK(b->token != a->token);
    f.shared->Publish(0, a->token, {22, 15, false});
    f.shared->Fail(0, a->token, "Old failure", 23);
    CHECK(f.shared->Active(0, b->token));
    CHECK(f.Drain(*b).empty());
    f.shared->Publish(0, b->token, {24, 4, false});
    CHECK(f.Drain(*a).empty());
    CHECK(f.Drain(*b)[0].mask == 4);
}

void BoundAndDuplicate()
{
    Fixture f;
    std::vector<std::shared_ptr<Attachment>> attachments;
    for (unsigned i = 0; i < RouteLimit; ++i) attachments.push_back(f.Attach(i));
    auto extra = std::make_shared<Attachment>(*attachments[0]);
    extra->token = f.shared->NewToken();
    CHECK(f.shared->Attach(extra) == RouteLimit);
    f.shared->Detach(5, attachments[5]->token);
    extra->user = 5;
    CHECK(f.shared->Attach(extra) == RouteLimit); // Same physical device on another slot.
}

void HeldRetiredContexts()
{
    Fixture f;
    auto prototype = f.Attach(0);
    f.shared->Detach(0, prototype->token);
    std::vector<SDLRoute> held;
    std::vector<std::shared_ptr<Attachment>> attachments;
    for (size_t i = 0; i < RouteLimit * 3; ++i) {
        auto attachment = std::make_shared<Attachment>(*prototype);
        attachment->token = f.shared->NewToken();
        SDLRoute context;
        context.route = f.shared->Attach(attachment);
        CHECK(context.route < RouteLimit);
        for (size_t old = 0; old < held.size(); ++old) {
            CHECK(!held[old].RetireRoute(f.shared.get(), attachments[old]->token));
            f.shared->Publish(held[old].route, attachments[old]->token, {1, 15, false});
        }
        CHECK(f.shared->Active(context.route, attachment->token));
        CHECK(context.RetireRoute(f.shared.get(), attachment->token));
        CHECK(context.retired && !f.shared->Active(context.route, attachment->token));
        held.push_back(context);
        attachments.push_back(std::move(attachment));
    }
    CHECK(held.size() > RouteLimit);
}

void OpenOnlyBusyRetry()
{
    for (unsigned busy = 0; busy <= 8; ++busy) {
        unsigned queries = 0, yields = 0;
        const auto status = QueryAtOpen([&] { return queries++ < busy ? ERROR_BUSY : ERROR_SUCCESS; }, [&] { ++yields; });
        CHECK(queries == std::min(busy + 1, 8u));
        CHECK(yields == std::min(busy, 7u));
        CHECK(status == static_cast<DWORD>(busy == 8 ? ERROR_BUSY : ERROR_SUCCESS));
    }
    unsigned queries = 0, yields = 0;
    CHECK(QueryAtOpen([&] { ++queries; return ERROR_NOT_SUPPORTED; }, [&] { ++yields; }) == ERROR_NOT_SUPPORTED);
    CHECK(queries == 1 && yields == 0);
    queries = yields = 0;
    CHECK(QueryAtOpen([&] { return ++queries == 1 ? ERROR_BUSY : ERROR_DEVICE_NOT_CONNECTED; }, [&] { ++yields; }) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(queries == 2 && yields == 1);
}

void BootstrapAndLiveEdges()
{
    Fixture f;
    auto a = f.Attach(0);
    DataRoute data;
    data.Start(PaddleTransport::UsbGip, f.shared->NewToken(), 9);
    data.Service(*f.shared, 0, *a, {Packet(*a, 1, 1, true), Packet(*a, 2, 0, true), Packet(*a, 3, 8, true)}, 10000);
    auto values = f.Drain(*a);
    CHECK(values.size() == 1 && values[0].mask == 8);
    data.Service(*f.shared, 0, *a, {Packet(*a, 4, 0), Packet(*a, 5, 1), Packet(*a, 6, 0)}, 20000);
    values = f.Drain(*a);
    CHECK(values.size() == 3 && values[0].mask == 0 && values[1].mask == 1 && values[2].mask == 0);
    CHECK(values[0].qpc == 18000 && values[1].qpc == 19000 && values[2].qpc == 20000);
    data.Service(*f.shared, 0, *a, {Packet(*a, 5, 15), Packet(*a, 7, 15, false, 0x0c, 17, 88)}, 30000);
    CHECK(f.Drain(*a).empty());
}

void GapPreservesSeparateMode()
{
    Fixture f;
    auto a = f.Attach(0);
    DataRoute data;
    data.Start(PaddleTransport::UsbGip, f.shared->NewToken(), 9);
    data.Service(*f.shared, 0, *a, {Packet(*a, 1, 8)}, 10000);
    f.Drain(*a);
    ServicePacket gap;
    gap.nativeId = a->physical.nativeId;
    gap.viewGeneration = 9;
    gap.gap = true;
    data.Service(*f.shared, 0, *a, {gap, Packet(*a, 2, 0, false, 0x20, 34), Packet(*a, 3, 4)}, 20000);
    const auto values = f.Drain(*a);
    CHECK(values.size() == 2 && values[0].release && values[0].mask == 0 && values[1].mask == 4);
    CHECK(data.decoder.have_separate && data.diagnostics.normal == 1 && data.diagnostics.separate == 2);
    CHECK(values[0].qpc == 19000 && values[1].qpc == 20000);
    data.Service(*f.shared, 0, *a, {gap}, 30000);
    Diagnostics diagnostics;
    CHECK(f.Drain(*a, true, &diagnostics)[0].release && !diagnostics.dataAvailable);
}

void KnownLayoutsAndCandidate()
{
    Fixture f;
    auto a = f.Attach(0);
    DataRoute data;
    data.Start(PaddleTransport::UsbGip, f.shared->NewToken(), 9);
    data.Service(*f.shared, 0, *a, {Packet(*a, 1, 15, false, 0x20, 46)}, 10000);
    CHECK(data.candidate46 && data.diagnostics.normal == 1 && data.diagnostics.separate == 0);
    const auto inBand = f.Drain(*a);
    CHECK(inBand.size() == 1 && inBand[0].mask == 15);
    CHECK(data.decoder.paddles.delivery == SDL_XINPUT_PADDLE_DELIVERY_IN_BAND && !data.decoder.have_separate);
    data.Service(*f.shared, 0, *a, {Packet(*a, 2, 2, false, 0x20, 29), Packet(*a, 3, 4, false, 0x20, 34), Packet(*a, 4, 8, false, 0x20, 47)}, 20000);
    const auto values = f.Drain(*a);
    CHECK(values.size() == 3 && values[0].mask == 1 && values[1].mask == 4 && values[2].mask == 8);
    data.Service(*f.shared, 0, *a, {Packet(*a, 5, 1, false, 0x0c, 16)}, 30000);
    CHECK(f.Drain(*a).empty() && data.diagnostics.malformed == 1);
}

void Legacy46ThenSeparate()
{
    Fixture f;
    auto a = f.Attach(0);
    DataRoute data;
    data.Start(PaddleTransport::UsbGip, f.shared->NewToken(), 9);
    auto first = Packet(*a, 1, 1, false, 0x20, 46);
    first.reading.bytes[19] = 3;
    data.Service(*f.shared, 0, *a, {first}, 10000);
    auto values = f.Drain(*a);
    CHECK(values.size() == 1 && values[0].mask == 1);
    CHECK(data.decoder.paddles.profile == 3 && data.diagnostics.separate == 0);
    data.Service(*f.shared, 0, *a, {Packet(*a, 2, 3, false, 0x20, 46), Packet(*a, 3, 0, false, 0x20, 46)}, 20000);
    values = f.Drain(*a);
    CHECK(values.size() == 2 && values[0].mask == 3 && values[1].mask == 0);
    CHECK(data.diagnostics.states == 3 && data.diagnostics.normal == 3 && data.diagnostics.separate == 0);
    auto ordinary = Packet(*a, 5, 0, false, 0x20, 46);
    ordinary.reading.bytes[0] = 0x10;
    data.Service(*f.shared, 0, *a, {Packet(*a, 4, 8), ordinary}, 30000);
    values = f.Drain(*a);
    CHECK(values.size() == 1 && values[0].mask == 8);
    CHECK(data.decoder.normal[0] == 0x10 && data.decoder.paddles.physical_mask == 8);
    CHECK(data.decoder.paddles.delivery == SDL_XINPUT_PADDLE_DELIVERY_SEPARATE && data.decoder.have_separate);
    ServicePacket gap;
    gap.nativeId = a->physical.nativeId;
    gap.viewGeneration = 9;
    gap.gap = true;
    data.Service(*f.shared, 0, *a, {gap, Packet(*a, 6, 15, false, 0x20, 46)}, 40000);
    values = f.Drain(*a);
    CHECK(values.size() == 1 && values[0].release && values[0].mask == 0);
    CHECK(data.decoder.have_separate && !data.diagnostics.dataAvailable);
    data.Service(*f.shared, 0, *a, {Packet(*a, 7, 4)}, 50000);
    CHECK(f.Drain(*a)[0].mask == 4 && data.diagnostics.dataAvailable);
}

void GattMarkers()
{
    Fixture f;
    auto a = f.Attach(0, PaddleTransport::Bluetooth);
    DataRoute data;
    data.Start(PaddleTransport::Bluetooth, f.shared->NewToken());
    GattPaddlePacket packet;
    packet.attachmentGeneration = data.sourceToken;
    packet.bytes[14] = 15;
    data.Gatt(*f.shared, 0, *a, {packet}, 10000);
    CHECK(f.Drain(*a).empty() && data.diagnostics.gatt == 0);
    packet.hasPayload = true;
    packet.receivedQpc = 9000;
    data.Gatt(*f.shared, 0, *a, {packet}, 10000);
    CHECK(f.Drain(*a)[0].mask == 15);
    packet.gap = true;
    packet.bytes[14] = 1;
    data.Gatt(*f.shared, 0, *a, {packet}, 20000);
    auto values = f.Drain(*a);
    CHECK(values.size() == 2 && values[0].release && values[1].mask == 1);
    packet.attachmentGeneration = data.sourceToken + 1;
    data.Gatt(*f.shared, 0, *a, {packet}, 30000);
    CHECK(f.Drain(*a).empty());
    packet.attachmentGeneration = data.sourceToken;
    packet.hasPayload = false;
    packet.retired = true;
    data.Gatt(*f.shared, 0, *a, {packet}, 40000);
    CHECK(!f.shared->Active(0, a->token));
    values = f.Drain(*a);
    CHECK(values.size() == 1 && values[0].release && values[0].mask == 0);
}

void OwnerSingleClientAndEnable()
{
    Fixture f;
    auto a = f.Attach(0);
    auto b = f.Attach(1);
    f.Catalog(*a);
    f.Catalog(*b, 10);
    f.Ready(*a);
    injection.bootstrap[a->physical.nativeId] = {Packet(*a, 1, 0, true, 0x20, 46)};
    injection.bootstrap[b->physical.nativeId] = {Packet(*b, 1, 2, true, 0x20, 34, 10)};
    CHECK(f.owner.Step(0, 10000) == 2);
    CHECK(injection.constructs == 1 && injection.connects == 1 && injection.selects == 2);
    CHECK(injection.submits == 1 && injection.qualifications == 4);
    CHECK(injection.calls[0] == "runtime" && injection.calls[1] == "connect" && injection.calls[2] == "server");
    const auto initial = f.Drain(*a);
    CHECK(initial.size() == 1 && initial[0].mask == 0 && f.Drain(*b)[0].mask == 2);
    injection.result = GipEnableStatus::Success;
    f.owner.Step(1, 20000);
    CHECK(injection.submits == 1 && f.Drain(*a).empty());
    injection.batches.push_back({Packet(*a, 2, 1), Packet(*a, 3, 0)});
    f.owner.Step(2, 30000);
    CHECK(f.Drain(*a).size() == 2);
    f.shared->Detach(0, a->token);
    f.owner.Step(3, 40000);
    CHECK(injection.disconnects == 0 && injection.deselects == 1);
    f.shared->Detach(1, b->token);
    CHECK(f.owner.Step(4, 50000) == INFINITE);
    CHECK(injection.disconnects == 1);
    auto c = f.Attach(0);
    f.owner.Step(5, 60000);
    CHECK(injection.constructs == 1 && injection.connects == 2);
    CHECK(c->token != a->token);
}

void QualificationFailures()
{
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture f;
        auto a = f.Attach(0);
        f.Catalog(*a);
        if (mode == 0) injection.runtime = false;
        if (mode == 1) injection.server = false;
        if (mode == 2) injection.connect = false;
        if (mode == 3) injection.catalog.push_back(injection.catalog[0]);
        CHECK(f.owner.Step(0, 10000) == INFINITE);
        CHECK(!f.shared->Active(0, a->token) && injection.selects == 0 && injection.submits == 0);
        CHECK(f.Drain(*a)[0].mask == 0);
        if (mode == 0) CHECK(injection.connects == 0);
    }
}

void WgiAdmissionRetry()
{
    for (const auto terminal : {GipEnableStatus::Success, GipEnableStatus::Failed,
         GipEnableStatus::TimedOut, GipEnableStatus::Retired, GipEnableStatus::Exhausted}) {
        Fixture f;
        auto a = f.Attach(0);
        f.Catalog(*a);
        injection.bootstrap[a->physical.nativeId] = {Packet(*a, 1, 0, true, 0x20, 46)};
        f.Ready(*a);
        injection.rejectedSubmissions = 1;
        f.owner.Step(0, 10000);
        CHECK(injection.submits == 1 && injection.acceptedSubmissions == 0);
        const auto source = injection.lastSource;
        Diagnostics before;
        f.Drain(*a, true, &before);
        CHECK(before.normal == 1 && before.separate == 0 && before.enableStatus == 2);
        const auto qualifications = injection.qualifications;
        const auto resolves = injection.resolves;
        f.owner.Step(49, 20000);
        CHECK(injection.submits == 1 && injection.qualifications == qualifications && injection.resolves == resolves);
        f.owner.Step(50, 30000);
        CHECK(injection.submits == 2 && injection.acceptedSubmissions == 1 && injection.lastSource == source);
        CHECK(injection.qualifications == qualifications + 1 && injection.resolves == resolves + 1);
        injection.result = terminal;
        // These cases represent a consumed native attempt. Pre-dispatch
        // queue expiry/exhaustion is covered separately below.
        injection.replies[injection.lastRequest] = {terminal, terminal == GipEnableStatus::Failed ? E_FAIL : S_OK,
            a->physical.nativeId, 1, true};
        f.owner.Step(51, 40000);
        f.owner.Step(AcquisitionTimeoutMs + 1, 50000);
        Diagnostics after;
        CHECK(f.Drain(*a, true, &after).empty());
        CHECK(f.shared->Active(0, a->token) && injection.submits == 2 && injection.acceptedSubmissions == 1);
        CHECK(after.normal == before.normal && after.separate == 0 && after.states == before.states);
        CHECK(after.enableStatus == 3 + static_cast<std::uint32_t>(terminal));
    }
}

void QueueExpiryBeforeDispatchCanReadmit()
{
    for (const auto status : {GipEnableStatus::TimedOut, GipEnableStatus::Exhausted}) {
        Fixture f;
        auto a = f.Attach(0);
        f.Catalog(*a); f.Ready(*a);
        f.owner.Step(0, 10000);
        const auto first = injection.lastRequest;
        const auto source = injection.lastSource;
        injection.replies[first] = {status, HRESULT_FROM_WIN32(ERROR_TIMEOUT), a->physical.nativeId, 1, false};
        f.owner.Step(1, 20000);
        CHECK(injection.acceptedSubmissions == 1); // Fifty-ms pacing still applies.
        f.owner.Step(50, 30000);
        CHECK(injection.acceptedSubmissions == 1);
        f.owner.Step(51, 30001);
        CHECK(injection.acceptedSubmissions == 2 && injection.lastRequest != first && injection.lastSource == source);
        injection.replies[injection.lastRequest] = {GipEnableStatus::TimedOut, HRESULT_FROM_WIN32(ERROR_TIMEOUT), a->physical.nativeId, 1, true};
        f.owner.Step(52, 40000);
        f.owner.Step(100000, 50000);
        CHECK(injection.acceptedSubmissions == 2); // Actual native timeout never retries.
    }
    Fixture f;
    auto a = f.Attach(0);
    f.Catalog(*a); f.Ready(*a);
    f.owner.Step(0, 10000);
    injection.replies[injection.lastRequest] = {GipEnableStatus::TimedOut, HRESULT_FROM_WIN32(ERROR_TIMEOUT), a->physical.nativeId, 1, false};
    f.owner.Step(20000, 20000); // No intermediate poll. Queued waiting cannot consume the admission budget.
    CHECK(injection.acceptedSubmissions == 1);
    f.owner.Step(20050, 30000);
    CHECK(injection.acceptedSubmissions == 2);
}

void QueueReadmissionPreservesBudget()
{
    Fixture f;
    auto a = f.Attach(0);
    f.Catalog(*a); f.Ready(*a);
    injection.rejectedSubmissions = 1;
    f.owner.Step(0, 10000);
    f.owner.Step(AcquisitionTimeoutMs - 50, 20000);
    CHECK(injection.acceptedSubmissions == 1);
    injection.replies[injection.lastRequest] = {GipEnableStatus::TimedOut, HRESULT_FROM_WIN32(ERROR_TIMEOUT), a->physical.nativeId, 1, false};
    f.owner.Step(100000, 30000);
    f.owner.Step(100050, 40000);
    Diagnostics d;
    f.Drain(*a, true, &d);
    CHECK(d.admissionExpired && injection.acceptedSubmissions == 1 && f.shared->Active(0, a->token));
}

void WgiAdmissionDeadline()
{
    Fixture f;
    auto a = f.Attach(0);
    f.Catalog(*a);
    injection.bootstrap[a->physical.nativeId] = {Packet(*a, 1, 4, true, 0x20, 46)};
    f.Ready(*a);
    injection.rejectedSubmissions = UINT32_MAX;
    f.owner.Step(0, 10000);
    CHECK(f.Drain(*a)[0].mask == 4);
    f.owner.Step(50, 20000);
    CHECK(injection.submits == 2 && injection.acceptedSubmissions == 0);
    CHECK(f.owner.Step(AcquisitionTimeoutMs, 30000) == 2);
    Diagnostics diagnostics;
    CHECK(f.Drain(*a, true, &diagnostics).empty());
    CHECK(diagnostics.admissionExpired && diagnostics.dataAvailable && !diagnostics.failed);
    CHECK(f.shared->Active(0, a->token) && injection.submits == 2 && injection.acceptedSubmissions == 0);
    f.owner.Step(AcquisitionTimeoutMs + 1000, 40000);
    CHECK(injection.submits == 2);
}

void WgiLateNormalAdmission()
{
    Fixture f;
    auto a = f.Attach(0);
    f.Catalog(*a);
    f.owner.Step(0, 10000);
    CHECK(injection.submits == 0 && f.shared->Active(0, a->token));
    injection.rejectedSubmissions = 1;
    injection.batches.push_back({Packet(*a, 1, 4, false, 0x20, 46)});
    const auto firstNormal = AcquisitionTimeoutMs + 1000;
    f.Ready(*a);
    f.owner.Step(firstNormal, 20000);
    CHECK(f.shared->Active(0, a->token) && injection.submits == 1 && injection.acceptedSubmissions == 0);
    CHECK(f.Drain(*a)[0].mask == 4);
    f.owner.Step(firstNormal + 50, 30000);
    CHECK(f.shared->Active(0, a->token) && injection.submits == 2 && injection.acceptedSubmissions == 1);
}

void ObserverStartsBeforeServiceInput()
{
    Fixture f;
    auto a = f.Attach(0);
    f.Catalog(*a);
    f.owner.Step(0, 10000);
    CHECK(injection.selects == 1 && injection.observations == 1 && injection.submits == 0);
    f.owner.Step(AcquisitionTimeoutMs * 3, 20000);
    Diagnostics d;
    f.Drain(*a, true, &d);
    CHECK(f.shared->Active(0, a->token) && !d.admissionExpired && d.normal == 0);
    f.Ready(*a);
    injection.input[a->physical.nativeId].resumeTime = 0; // Initial epoch seeded by own normal input.
    injection.input[a->physical.nativeId].normalTime = UINT64_C(987654321000);
    f.owner.Step(AcquisitionTimeoutMs * 3 + 1, 30000);
    f.Drain(*a, true, &d);
    CHECK(injection.submits == 1 && injection.acceptedSubmissions == 1 && d.normal == 0);
    CHECK(d.inputValid && d.input.Ready() && d.input.resumeTime == 0 && d.input.normalTime == UINT64_C(987654321000));
}

void ServiceInputDoesNotEstablishReadiness()
{
    Fixture f;
    auto a = f.Attach(0);
    f.Catalog(*a);
    injection.bootstrap[a->physical.nativeId] = {Packet(*a, 1, 8, true, 0x20, 46)};
    f.Ready(*a);
    injection.input[a->physical.nativeId].normalSeen = false;
    f.owner.Step(0, 10000);
    CHECK(f.Drain(*a)[0].mask == 8 && injection.submits == 0);
    injection.batches.push_back({Packet(*a, 2, 4)});
    f.owner.Step(AcquisitionTimeoutMs * 3, 20000);
    CHECK(f.Drain(*a)[0].mask == 4 && injection.submits == 0 && f.shared->Active(0, a->token));
    f.Ready(*a);
    f.owner.Step(AcquisitionTimeoutMs * 3 + 1, 30000);
    CHECK(injection.acceptedSubmissions == 1);
}

void EpochChangesPreserveServiceState()
{
    Fixture f;
    auto a = f.Attach(0);
    f.Catalog(*a); f.Ready(*a);
    f.owner.Step(0, 10000);
    const auto source = injection.lastSource;
    injection.result = GipEnableStatus::Success;
    f.owner.Step(1, 20000);
    injection.input[a->physical.nativeId].resumed = false;
    injection.input[a->physical.nativeId].normalSeen = false;
    injection.batches.push_back({Packet(*a, 1, 15)});
    f.owner.Step(2, 30000);
    CHECK(f.Drain(*a)[0].mask == 15 && injection.acceptedSubmissions == 1);
    injection.input[a->physical.nativeId] = {};
    f.owner.Step(3, 40000);
    f.Ready(*a); // A temporary missing snapshot must not rearm the same pair.
    f.owner.Step(4, 50000);
    CHECK(injection.acceptedSubmissions == 1);
    f.Ready(*a, 2);
    injection.result = GipEnableStatus::Pending;
    f.owner.Step(5, 60000);
    CHECK(injection.acceptedSubmissions == 2 && injection.lastSource == source);
    Diagnostics d;
    CHECK(f.Drain(*a, true, &d).empty() && d.dataAvailable && d.states == 1 && d.input.epoch == 2);
    injection.result = GipEnableStatus::Failed;
    f.owner.Step(6, 70000);
    f.owner.Step(100000, 80000);
    f.Drain(*a, true, &d);
    CHECK(injection.acceptedSubmissions == 2 && d.commandError == E_FAIL && d.commandDispatched && d.dataAvailable);
    f.Ready(*a, 1, 90000); // New monotonic provider serial, independently numbered epoch.
    f.owner.Step(100001, 90000);
    CHECK(injection.acceptedSubmissions == 3 && injection.lastSource == source);
    CHECK(injection.retired.empty()); // Epoch changes do not retire the service attachment.
}

void BusyAndContentionPauseAdmission()
{
    Fixture f;
    auto a = f.Attach(0);
    f.Catalog(*a); f.Ready(*a);
    injection.rejectedSubmissions = UINT32_MAX;
    f.owner.Step(0, 10000);
    f.owner.Step(50, 20000);
    CHECK(injection.submits == 2);
    injection.queryInput = false;
    f.owner.Step(20000, 30000);
    Diagnostics d;
    f.Drain(*a, true, &d);
    CHECK(!d.inputValid && !d.admissionExpired && injection.submits == 2);
    injection.queryInput = true;
    injection.input[a->physical.nativeId].busy = true;
    f.owner.Step(40000, 40000);
    f.owner.Step(80000, 50000);
    f.Drain(*a, true, &d);
    CHECK(d.input.busy && !d.admissionExpired && injection.submits == 2);
    injection.input[a->physical.nativeId].busy = false;
    injection.rejectedSubmissions = 0;
    f.owner.Step(80001, 60000);
    CHECK(injection.submits == 3 && injection.acceptedSubmissions == 1);
}

void BusyOldEpochAndStaleCompletion()
{
    Fixture f;
    auto a = f.Attach(0);
    f.Catalog(*a); f.Ready(*a);
    f.owner.Step(0, 10000);
    const auto source = injection.lastSource;
    f.Ready(*a, 2);
    injection.input[a->physical.nativeId].busy = true;
    f.owner.Step(1, 20000);
    f.owner.Step(AcquisitionTimeoutMs * 10, 30000);
    CHECK(injection.acceptedSubmissions == 1);
    injection.input[a->physical.nativeId].busy = false;
    f.owner.Step(AcquisitionTimeoutMs * 10 + 1, 40000);
    CHECK(injection.acceptedSubmissions == 2 && injection.lastSource == source);
    injection.replies[injection.lastRequest] = {GipEnableStatus::Failed, E_FAIL, a->physical.nativeId, 1, true};
    f.owner.Step(AcquisitionTimeoutMs * 10 + 2, 50000);
    Diagnostics d;
    f.Drain(*a, true, &d);
    CHECK(d.input.epoch == 2 && d.commandError == S_OK && !d.commandDispatched && !d.admissionExpired);
    f.owner.Step(AcquisitionTimeoutMs * 20, 60000);
    CHECK(injection.acceptedSubmissions == 2);
}

void ReadinessRaceAtAdmission()
{
    Fixture f;
    auto a = f.Attach(0);
    f.Catalog(*a); f.Ready(*a);
    injection.inSubmit = [&] { injection.input[a->physical.nativeId].resumed = false; };
    f.owner.Step(0, 10000);
    CHECK(injection.submits == 1 && injection.acceptedSubmissions == 0);
    f.owner.Step(AcquisitionTimeoutMs * 3, 20000);
    CHECK(injection.submits == 1 && f.shared->Active(0, a->token));
    injection.inSubmit = {};
    f.Ready(*a, 2);
    f.owner.Step(AcquisitionTimeoutMs * 3 + 1, 30000);
    CHECK(injection.submits == 2 && injection.acceptedSubmissions == 1);
}

void TracePreservesOwnedPayloadAndStages()
{
    Fixture f;
    auto a = f.Attach(0);
    a->trace = true;
    DataRoute data;
    data.Start(PaddleTransport::UsbGip, f.shared->NewToken(), 9);
    auto packet = Packet(*a, 51, 15);
    packet.reading.rawSequence = 777;
    const std::array<std::uint8_t, 17> captured{0x10,0,0,0,0,0,0x7b,0xfe,0xae,0xfd,0x5f,0xff,0x5a,0xfc,0x0f,0,0};
    packet.reading.bytes.assign(captured.begin(), captured.end());
    data.Service(*f.shared, 0, *a, {packet}, 10000);
    packet.reading.bytes.assign(17, 0);
    const auto edges = f.Drain(*a);
    CHECK(edges.size() == 1 && edges[0].mask == 15 && edges[0].traceId != 0);
    std::array<TraceRecord, 8> records{};
    std::uint64_t lost = 0;
    CHECK(f.shared->DrainTrace(records.data(), records.size(), lost) == 4 && lost == 0);
    CHECK(records[0].stage == TraceStage::Raw && records[0].generation == 51 && records[0].sequence == 777);
    CHECK(records[0].size == 17 && records[0].copied == 17 && std::memcmp(records[0].bytes.data(), captured.data(), 17) == 0);
    CHECK(records[0].source == data.sourceToken && records[0].nativeId == a->physical.nativeId && records[0].view == 9);
    CHECK(records[1].stage == TraceStage::Decoded && records[1].mask == 15 && (records[1].flags & SDL_XINPUT_PADDLE_STATE_READY));
    CHECK(records[2].stage == TraceStage::Queued && records[3].stage == TraceStage::Drained);
    for (const auto &record : {records[0], records[1], records[2], records[3]}) CHECK(record.id == edges[0].traceId);
}

void ViewAndIdentityReplacement()
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        Fixture f;
        auto a = f.Attach(0);
        f.Catalog(*a);
        injection.bootstrap[a->physical.nativeId] = {Packet(*a, 1, 4, true)};
        f.owner.Step(0, 10000);
        CHECK(f.Drain(*a)[0].mask == 4);
        injection.batches.push_back({Packet(*a, 2, 15)});
        if (mode == 0) injection.catalog[0].viewGeneration = 10;
        if (mode == 1) injection.identities[a->path].container.Data1++;
        if (mode == 2) injection.identities[a->path].instance += L"new";
        f.owner.Step(1001, 20000);
        const auto values = f.Drain(*a);
        CHECK(values.size() == 1 && values[0].mask == 0 && values[0].release);
        CHECK(!f.shared->Active(0, a->token));
    }
}

void EmptyFailureAndAllocation()
{
    for (unsigned mode = 0; mode < 2; ++mode) {
        Fixture f;
        auto a = f.Attach(0);
        f.Catalog(*a);
        f.owner.Step(0, 10000);
        f.shared->Publish(0, a->token, {10000, 15, false});
        if (mode == 0) injection.pump = false;
        else injection.throwDevices = true;
        CHECK(f.owner.Step(1, 20000) == INFINITE);
        const auto values = f.Drain(*a);
        CHECK(values.size() == 1 && values[0].mask == 0 && values[0].release);
        CHECK(injection.disconnects == 1);
    }
}

void RetireDuringSelect()
{
    Fixture f;
    auto a = f.Attach(0);
    f.Catalog(*a);
    injection.bootstrap[a->physical.nativeId] = {Packet(*a, 1, 15, true, 0x20, 46), Packet(*a, 2, 15, true)};
    injection.inSelect = [&] { f.shared->Detach(0, a->token); };
    CHECK(f.owner.Step(0, 10000) == INFINITE);
    CHECK(f.Drain(*a).empty() && injection.submits == 0);
    CHECK(injection.deselects == 1 && injection.disconnects == 1);
}

void QuitAndGattCleanup()
{
    Fixture f;
    auto a = f.Attach(0, PaddleTransport::Bluetooth);
    CHECK(f.owner.Step(0, 10000) == 2);
    CHECK(injection.gattStarts == 1 && injection.connects == 0);
    GattPaddlePacket packet;
    packet.hasPayload = true;
    packet.bytes[14] = 2;
    injection.gattBatches.push_back({packet});
    f.owner.Step(1, 20000);
    CHECK(f.Drain(*a)[0].mask == 2);
    const auto source = injection.lastSource;
    f.shared->Quit(30000);
    CHECK(f.owner.Step(2, 30000) == 10);
    CHECK(injection.gattRetires == 1 && injection.orphans == 1 && injection.orphanPumps >= 3);
    CHECK(f.Drain(*a).empty());
    injection.orphans = 0;
    CHECK(f.owner.Step(3, 40000) == INFINITE);
    auto b = f.Attach(0, PaddleTransport::Bluetooth);
    f.owner.Step(4, 50000);
    CHECK(injection.lastSource != source && b->token != a->token);
}

void StartupAndMissingView()
{
    Fixture f;
    auto a = f.Attach(0);
    CHECK(f.owner.Step(0, 10000) == 2);
    CHECK(f.owner.Step(AcquisitionTimeoutMs, 20000) == INFINITE);
    CHECK(!f.shared->Active(0, a->token));
    auto b = f.Attach(1);
    f.owner.StartupFailed();
    CHECK(f.owner.Step(20000, 30000) == INFINITE);
    CHECK(!f.shared->Active(1, b->token));
}

void PhysicalEligibility()
{
    PhysicalIdentity physical;
    physical.transport = PaddleTransport::UsbGip;
    physical.vendor = 0x045e;
    physical.product = 0x0b00;
    CHECK(EligiblePhysical(0x045e, 0x02ff, physical));
    physical.product = 0x0b13;
    CHECK(!EligiblePhysical(0x045e, 0x02ff, physical));
    CHECK(!EligiblePhysical(0x045e, 0x0b00, physical));
    physical.vendor = 0x1234;
    physical.product = 0x0b00;
    CHECK(!EligiblePhysical(0x045e, 0x02ff, physical));
    physical.transport = PaddleTransport::Bluetooth;
    CHECK(EligiblePhysical(0x045e, 0x0b22, physical));
    CHECK(!EligiblePhysical(0x045e, 0x02ff, physical));
    physical.transport = PaddleTransport::None;
    CHECK(!EligiblePhysical(0x045e, 0x0b22, physical));
}

void TimestampBoundaries()
{
    CHECK(HistoryQpc(100000, 2000, 1000, 10000000) == 90000);
    CHECK(HistoryQpc(100000, 1000, 2000, 10000000) == 100000);
    CHECK(HistoryQpc(100000, UINT64_MAX, 0, UINT32_MAX) == 0);
    CHECK(ScaleClamped(15000000, 1000000000, 10000000, UINT64_MAX) == 1500000000);
    CHECK(ScaleClamped(UINT64_MAX, 1000000000, 1, 123) == 123);
    CHECK(ScaleClamped(1, 1, 0, 123) == 0);
}

void ConcurrentQueueAndRetirement()
{
    Fixture f;
    auto a = f.Attach(0);
    std::atomic<bool> finished{false};
    std::thread producer([&] {
        for (unsigned i = 0; i < 20000; ++i) f.shared->Publish(0, a->token, {i, static_cast<std::uint8_t>(i % 16), false});
        finished.store(true, std::memory_order_release);
    });
    std::uint64_t last = 0;
    while (!finished.load(std::memory_order_acquire)) {
        for (const auto &edge : f.Drain(*a)) { CHECK(edge.qpc >= last); last = edge.qpc; }
    }
    producer.join();
    f.shared->Fail(0, a->token, "Retired", 30000);
    f.shared->Publish(0, a->token, {40000, 15, false});
    const auto values = f.Drain(*a);
    CHECK(values.size() == 1 && values[0].mask == 0 && values[0].release);
}
} // namespace

int main()
{
    const struct { const char *name; void (*run)(); } tests[] = {
        {"queue order", QueueOrder}, {"overflow release/resync", Overflow},
        {"tokens and startup drain", TokensAndStartupDrain}, {"route bound and duplicate identity", BoundAndDuplicate},
        {"held retired SDL contexts release routes", HeldRetiredContexts}, {"Open-only bounded busy retry", OpenOnlyBusyRetry},
        {"bootstrap and live edges", BootstrapAndLiveEdges}, {"gap preserves split mode", GapPreservesSeparateMode},
        {"layouts and 46-byte candidate", KnownLayoutsAndCandidate}, {"GATT markers", GattMarkers},
        {"legacy 46-byte edges and separate authority", Legacy46ThenSeparate},
        {"single client and enable sequencing", OwnerSingleClientAndEnable}, {"qualification rejection", QualificationFailures},
        {"WGI admission retry and terminal results", WgiAdmissionRetry}, {"WGI admission deadline", WgiAdmissionDeadline},
        {"queue expiry before dispatch can readmit", QueueExpiryBeforeDispatchCanReadmit},
        {"queue readmission preserves admission budget", QueueReadmissionPreservesBudget},
        {"late normal report starts admission window", WgiLateNormalAdmission},
        {"observer starts before service input", ObserverStartsBeforeServiceInput},
        {"service input cannot establish WGI readiness", ServiceInputDoesNotEstablishReadiness},
        {"epoch changes preserve service state", EpochChangesPreserveServiceState},
        {"busy and contention pause admission", BusyAndContentionPauseAdmission},
        {"busy old epoch and stale completion", BusyOldEpochAndStaleCompletion},
        {"readiness race at admission", ReadinessRaceAtAdmission},
        {"owned raw trace through drain", TracePreservesOwnedPayloadAndStages},
        {"view and identity replacement", ViewAndIdentityReplacement}, {"empty failure and allocation", EmptyFailureAndAllocation},
        {"retire during Select", RetireDuringSelect}, {"Quit and GATT cleanup", QuitAndGattCleanup},
        {"startup and missing view", StartupAndMissingView}, {"physical USB eligibility behind synthetic XInput ID", PhysicalEligibility},
        {"timestamp boundaries", TimestampBoundaries},
        {"concurrent publication and retirement", ConcurrentQueueAndRetirement}
    };
    for (const auto &test : tests) {
        try { test.run(); std::printf("PASS %s\n", test.name); }
        catch (const std::exception &error) { std::printf("FAIL %s: %s\n", test.name, error.what()); return 1; }
    }
    std::printf("PASS %zu groups, %u checks. Injected sources only.\n", std::size(tests), checks);
    return 0;
}
