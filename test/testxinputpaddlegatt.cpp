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

// Compile this translation unit alone. Including the implementation exercises
// its private machine, adapter result path, and production notification delegate
// without adding test exports or calling Bluetooth factories.
#define SDL_XINPUT_PADDLE_GATT_TESTING
#include "../src/joystick/windows/SDL_xinput_paddle_gatt.cpp"
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <thread>

using namespace sdl_paddles;
using namespace sdl_paddles::gatt_detail;
using namespace winrt::Windows::Storage::Streams;

#define CHECK(condition) do { if (!(condition)) { throw std::runtime_error(#condition); } } while (0)

namespace {
const winrt::guid Target{0x12345678, 0x1234, 0x5678, {0x91, 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78}};
const winrt::guid Other{0x12345679, 0x1234, 0x5678, {0x91, 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78}};
constexpr std::uint64_t TargetAddress = 0x123456789abc;
static Endpoint EP(std::wstring id, GUID container, bool resolved = true, std::uint64_t address = TargetAddress)
{
    return {std::move(id), container, resolved, address, address != 0};
}

struct Stats {
    std::vector<Phase> begins;
    unsigned results = 0, closes = 0, cancels = 0, revokes = 0, releases = 0;
    bool pending = false;
    bool connected = true;
    bool throwStatus = false, throwClose = false, throwRevoke = false;
    bool throwSubscribe = false;
    Phase throwBegin = Phase::Done;
    Phase throwResults = Phase::Done;
    bool emptyServices = false, emptyCharacteristics = false;
    bool subscribed = false;
    AsyncStatus status = AsyncStatus::Started;
    HRESULT operationError = HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    Cccd original = Cccd::None, current = Cccd::Notify;
    std::weak_ptr<Gate> gate;

    unsigned Count(Phase phase) const
    {
        return static_cast<unsigned>(std::count(begins.begin(), begins.end(), phase));
    }
};

struct FakeAdapter final : Adapter {
    explicit FakeAdapter(std::shared_ptr<Stats> stats) : stats(std::move(stats)) {}
    std::shared_ptr<Stats> stats;

    void Begin(Phase phase, Cccd original) override
    {
        CHECK(!stats->pending);
        stats->begins.push_back(phase);
        if (phase == stats->throwBegin) { throw winrt::hresult_error(E_ACCESSDENIED); }
        if (phase == Phase::WriteRestore) { CHECK(original == stats->original); }
        stats->pending = true;
        stats->status = AsyncStatus::Started;
    }
    AsyncStatus Status() override
    {
        CHECK(stats->pending);
        if (stats->throwStatus) { throw winrt::hresult_error(E_UNEXPECTED); }
        return stats->status;
    }
    HRESULT OperationError() override { return stats->operationError; }
    PhaseResult Results(Phase phase) override
    {
        CHECK(stats->pending && stats->status == AsyncStatus::Completed);
        ++stats->results;
        if (phase == stats->throwResults) { throw winrt::hresult_error(stats->operationError); }
        if (phase == Phase::ReadOriginal) { return {stats->original}; }
        if (phase == Phase::ReadRestore) { return {stats->current}; }
        return {Cccd::None, (phase == Phase::Services && stats->emptyServices) ||
            (phase == Phase::Characteristics && stats->emptyCharacteristics)};
    }
    void Cancel() override { CHECK(stats->pending); ++stats->cancels; }
    void CloseOperation() override
    {
        CHECK(stats->pending && stats->status != AsyncStatus::Started);
        ++stats->closes;
        stats->pending = false;
        if (std::exchange(stats->throwClose, false)) { throw winrt::hresult_error(E_FAIL); }
    }
    void Subscribe(std::shared_ptr<Gate> const& gate) override
    {
        stats->gate = gate;
        if (stats->throwSubscribe) { throw winrt::hresult_error(E_ACCESSDENIED); }
        stats->subscribed = true;
    }
    void Revoke() override
    {
        ++stats->revokes;
        if (auto gate = stats->gate.lock()) { CHECK(!gate->active.load()); }
        if (stats->throwRevoke) { throw winrt::hresult_error(E_FAIL); }
        stats->subscribed = false;
    }
    bool Connected() override { return stats->connected; }
    void Release() override { CHECK(!stats->pending); ++stats->releases; }
    std::uint64_t Address() const noexcept override { return 0x112233445566; }
};

struct Fixture {
    std::shared_ptr<Stats> stats = std::make_shared<Stats>();
    std::shared_ptr<Gate> gate = std::make_shared<Gate>(77, AcquirePermit());
    Machine machine{std::make_unique<FakeAdapter>(stats), gate};
    std::uint64_t now = 100;

    void Pump() { machine.Pump(++now); }
    void Complete(AsyncStatus status = AsyncStatus::Completed)
    {
        CHECK(stats->pending);
        stats->status = status;
        Pump();
    }
    void Reach(Phase phase)
    {
        for (unsigned i = 0; i < 60; ++i) {
            if (machine.phase == phase && (stats->pending || phase == Phase::Streaming)) { return; }
            CHECK(!machine.retired);
            if (stats->pending) { Complete(); } else { Pump(); }
        }
        throw std::runtime_error("phase not reached");
    }
    void Retire() { machine.Retire(++now); }
    void Finish()
    {
        for (unsigned i = 0; !machine.finished && i < 30; ++i) {
            if (stats->pending) { Complete(); } else { Pump(); }
        }
        CHECK(machine.finished && !stats->pending && stats->releases == 1);
    }
};

std::array<std::uint8_t, 17> Payload(unsigned value)
{
    std::array<std::uint8_t, 17> bytes{};
    bytes[0] = static_cast<std::uint8_t>(value);
    bytes[1] = static_cast<std::uint8_t>(value >> 8);
    bytes[14] = static_cast<std::uint8_t>(value & 15);
    bytes[16] = 0xe7;
    return bytes;
}

struct EventArgs : winrt::implements<EventArgs, IGattValueChangedEventArgs> {
    explicit EventArgs(IBuffer buffer) : buffer(std::move(buffer)) {}
    IBuffer buffer;
    std::atomic<unsigned> accesses{0};
    bool fail = false;
    IBuffer CharacteristicValue()
    {
        ++accesses;
        if (fail) { throw winrt::hresult_error(E_FAIL); }
        return buffer;
    }
    DateTime Timestamp() const { return {}; }
};

winrt::com_ptr<EventArgs> Args(unsigned size, unsigned value = 9)
{
    Buffer buffer(size);
    buffer.Length(size);
    auto access = buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();
    std::uint8_t* bytes = nullptr;
    winrt::check_hresult(access->Buffer(&bytes));
    if (size) { std::memset(bytes, 0xa5, size); }
    if (size == 17) {
        auto payload = Payload(value);
        std::memcpy(bytes, payload.data(), 17);
    }
    return winrt::make_self<EventArgs>(buffer);
}

void Invoke(decltype(NotificationHandler({})) const& handler, winrt::com_ptr<EventArgs> const& args)
{
    handler(nullptr, args.as<GattValueChangedEventArgs>());
}

template<class T>
struct ManualOperation : winrt::implements<ManualOperation<T>, IAsyncOperation<T>, IAsyncInfo> {
    explicit ManualOperation(T result) : result(std::move(result)) {}
    T result;
    AsyncStatus status = AsyncStatus::Started;
    unsigned results = 0, cancels = 0, closes = 0;
    AsyncOperationCompletedHandler<T> completed{nullptr};
    std::uint32_t Id() const { return 1; }
    AsyncStatus Status() const { return status; }
    winrt::hresult ErrorCode() const { return E_FAIL; }
    void Cancel() { ++cancels; }
    void Close() { CHECK(status != AsyncStatus::Started); ++closes; }
    void Completed(AsyncOperationCompletedHandler<T> const& value) { completed = value; }
    AsyncOperationCompletedHandler<T> Completed() const { return completed; }
    T GetResults() { CHECK(status == AsyncStatus::Completed); ++results; return result; }
};

struct ServiceResult : winrt::implements<ServiceResult, IGattDeviceServicesResult> {
    GattCommunicationStatus status = GattCommunicationStatus::Success;
    GattCommunicationStatus Status() const { return status; }
    IReference<std::uint8_t> ProtocolError() const { return nullptr; }
    winrt::Windows::Foundation::Collections::IVectorView<GattDeviceService> Services() const
    {
        return winrt::single_threaded_vector<GattDeviceService>().GetView();
    }
};

struct CharacteristicResult : winrt::implements<CharacteristicResult, IGattCharacteristicsResult> {
    GattCommunicationStatus Status() const { return GattCommunicationStatus::Success; }
    IReference<std::uint8_t> ProtocolError() const { return nullptr; }
    winrt::Windows::Foundation::Collections::IVectorView<GattCharacteristic> Characteristics() const
    {
        return winrt::single_threaded_vector<GattCharacteristic>().GetView();
    }
};

void ExpectIdentity(Code code, std::vector<Endpoint> const& endpoints, GUID const& container = Target)
{
    try { SelectEndpoint(endpoints, BthDevice{L"node", container, TargetAddress}); }
    catch (GattPaddleError const& error) { CHECK(error.code == code); return; }
    throw std::runtime_error("identity accepted");
}

} // namespace

namespace sdl_paddles {
struct GattPaddleTestAccess {
    static inline std::shared_ptr<Stats> startingStats;
    static bool StartWithEarlyRetire(GattPaddleClient& client, std::shared_ptr<Stats> stats, GattPaddleError& error)
    {
        startingStats = std::move(stats);
        GattPaddleClient::Impl::beforePublication = [](GattPaddleClient& owner, GattPaddleClient::Impl& state) {
            state.machine.adapter = std::make_unique<FakeAdapter>(startingStats);
            startingStats->gate = state.machine.gate;
            std::thread retire([&owner] { owner.Retire(); });
            retire.join();
        };
        bool result = client.Start(error);
        GattPaddleClient::Impl::beforePublication = nullptr;
        startingStats.reset();
        return result;
    }
    static void Install(GattPaddleClient& client, std::shared_ptr<Stats> stats)
    {
        auto permit = AcquirePermit();
        CHECK(permit);
        auto gate = std::make_shared<Gate>(client.generation_, std::move(permit));
        stats->gate = gate;
        client.impl_ = std::make_unique<GattPaddleClient::Impl>(client.ownerThread_,
            std::make_unique<FakeAdapter>(std::move(stats)), std::move(gate));
        client.published_.store(client.impl_.get(), std::memory_order_seq_cst);
        client.attempted_ = true;
    }
};
} // namespace sdl_paddles

int main()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    unsigned passed = 0, failed = 0;
    auto test = [&](char const* name, std::function<void()> body) {
        try {
            CHECK(livePermits.load() == 0);
            body();
            CHECK(livePermits.load() == 0);
            ++passed;
            std::printf("PASS %s\n", name);
        } catch (std::exception const& error) {
            ++failed; std::printf("FAIL %s: %s\n", name, error.what());
        } catch (winrt::hresult_error const& error) {
            ++failed; std::printf("FAIL %s: HRESULT %08x\n", name, static_cast<unsigned>(error.code().value));
        } catch (...) { ++failed; std::printf("FAIL %s: unexpected exception\n", name); }
    };

    test("exact identity and case-insensitive endpoint equality", [] {
        CHECK(SelectEndpoint({EP(L"endpoint-other", Other, true, TargetAddress + 1), EP(L"Endpoint-A", Target)},
            BthDevice{L"node", Target, TargetAddress}) == L"Endpoint-A");
        CHECK(SameEndpoint(L"Endpoint-A", L"endpoint-a"));
        CHECK(!SameEndpoint(L"Endpoint-A", L"endpoint-b"));
    });
    test("zero wrong missing unresolved and multiple identity rejected", [] {
        const GUID system{0, 0, 0, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
        ExpectIdentity(Code::InvalidIdentity, {}, GUID{});
        ExpectIdentity(Code::InvalidIdentity, {}, system);
        ExpectIdentity(Code::IdentityUnavailable, {EP(L"system", system)});
        ExpectIdentity(Code::IdentityMismatch, {});
        ExpectIdentity(Code::IdentityMismatch, {EP(L"other", Other)});
        ExpectIdentity(Code::IdentityUnavailable, {EP(L"missing", {}, false)});
        ExpectIdentity(Code::IdentityUnavailable, {EP(L"zero", {})});
        ExpectIdentity(Code::IdentityUnavailable, {EP(L"", Target)});
        ExpectIdentity(Code::AmbiguousIdentity, {EP(L"a", Target), EP(L"b", Target)});
        ExpectIdentity(Code::AmbiguousIdentity, {EP(L"a", Target), EP(L"a", Target)});
        ExpectIdentity(Code::AmbiguousIdentity, {EP(L"a", Target), EP(L"b", Other)});
        ExpectIdentity(Code::IdentityUnavailable, {EP(L"a", Target, true, 0)});
    });
    test("complete property address parsing never extracts an ID substring", [] {
        std::uint64_t address = 0;
        CHECK(ParseBluetoothAddress(L"123456789abc", address) && address == TargetAddress);
        CHECK(ParseBluetoothAddress(L"12:34:56:78:9A:BC", address) && address == TargetAddress);
        CHECK(!ParseBluetoothAddress(L"BTHLE\\Dev_123456789abc", address));
        CHECK(!ParseBluetoothAddress(L"12:34:56:78:9a:bc-extra", address));
        CHECK(!ParseBluetoothAddress(L"123456789abg", address));
        CHECK(!ParseBluetoothAddress(L"000000000000", address));
        CHECK(!ParseBluetoothAddress(L"12:3456:78:9abc", address));
    });
    test("unrelated malformed container does not block a different address", [] {
        CHECK(SelectEndpoint({EP(L"unrelated", {}, false, TargetAddress + 1), EP(L"selected", Target)},
            BthDevice{L"node", Target, TargetAddress}) == L"selected");
    });
    test("BTHLE binding rejects duplicate roots and detects changed metadata", [] {
        BthDevice node{L"node", Target, TargetAddress};
        CHECK(SameBthDevice(node, SelectBthDevice({{L"other", Other, TargetAddress + 1}, node}, Target)));
        CHECK(!SameBthDevice(node, BthDevice{L"new-node", Target, TargetAddress}));
        CHECK(!SameBthDevice(node, BthDevice{L"node", Other, TargetAddress}));
        CHECK(!SameBthDevice(node, BthDevice{L"node", Target, TargetAddress + 1}));
        bool rejected = false;
        try { SelectBthDevice({node, {L"node2", Target, TargetAddress}}, Target); }
        catch (GattPaddleError const& e) { rejected = e.code == Code::AmbiguousIdentity; }
        CHECK(rejected);
        rejected = false;
        try { SelectBthDevice({{L"node", Target, 0}}, Target); }
        catch (GattPaddleError const& e) { rejected = e.code == Code::IdentityUnavailable; }
        CHECK(rejected);
    });
    test("actual notification copies all bytes and samples QPC", [] {
        Fixture f;
        f.Reach(Phase::Streaming);
        auto handler = NotificationHandler(f.gate);
        auto args = Args(17);
        LARGE_INTEGER before{}, after{};
        QueryPerformanceCounter(&before);
        Invoke(handler, args);
        QueryPerformanceCounter(&after);
        std::vector<GattPaddlePacket> packets;
        f.machine.Drain(packets);
        CHECK(packets.size() == 1 && packets[0].hasPayload && !packets[0].gap);
        CHECK(packets[0].bytes == Payload(9) && packets[0].attachmentGeneration == 77);
        CHECK(packets[0].receivedQpc >= before.QuadPart && packets[0].receivedQpc <= after.QuadPart);
        f.Retire(); f.Finish();
    });
    test("unknown lengths release without qualifying or replacing prefix", [] {
        for (auto size : {0u, 16u, 18u, 64u}) {
            Fixture f; f.Reach(Phase::Streaming);
            auto handler = NotificationHandler(f.gate);
            Invoke(handler, Args(17));
            Invoke(handler, Args(size));
            std::vector<GattPaddlePacket> packets;
            f.machine.Drain(packets);
            CHECK(packets.size() == 2 && packets[0].hasPayload);
            CHECK(packets[1].gap && !packets[1].hasPayload && !packets[1].retired);
            CHECK((packets[1].receivedQpc == 0 && packets[1].bytes == std::array<std::uint8_t, 17>{}));
            f.Retire(); f.Finish();
        }
    });
    test("overflow preserves 1024 edges then one latest resync snapshot", [] {
        Fixture f; f.Reach(Phase::Streaming);
        for (unsigned i = 0; i < 1100; ++i) {
            auto bytes = Payload(i); f.gate->Push(bytes.data(), bytes.size(), i);
        }
        std::vector<GattPaddlePacket> packets;
        f.machine.Drain(packets);
        CHECK(packets.size() == 1025);
        for (unsigned i = 0; i < 1024; ++i) {
            CHECK(packets[i].bytes == Payload(i) && !packets[i].gap && packets[i].hasPayload);
        }
        CHECK(packets.back().gap && packets.back().hasPayload && packets.back().bytes == Payload(1099));
        packets.clear();
        auto bytes = Payload(1100); f.gate->Push(bytes.data(), bytes.size(), 1100);
        f.machine.Drain(packets);
        CHECK(packets.size() == 1 && !packets[0].gap && packets[0].bytes == bytes);
        f.Retire(); f.Finish();
    });
    test("gap coalesces bounded resync and later unknown invalidates snapshot", [] {
        Fixture f; f.Reach(Phase::Streaming);
        auto bytes = Payload(1); f.gate->Push(bytes.data(), 17, 1);
        f.gate->Gap();
        for (unsigned i = 2; i < 2000; ++i) { bytes = Payload(i); f.gate->Push(bytes.data(), 17, i); }
        CHECK(f.gate->count == 1);
        f.gate->Gap();
        std::vector<GattPaddlePacket> packets;
        f.machine.Drain(packets);
        CHECK(packets.size() == 2 && packets[0].bytes == Payload(1));
        CHECK(packets[1].gap && !packets[1].hasPayload);
        f.Retire(); f.Finish();
    });
    test("callback buffer failure preserves prefix and sends release", [] {
        Fixture f; f.Reach(Phase::Streaming);
        auto handler = NotificationHandler(f.gate);
        Invoke(handler, Args(17));
        auto bad = Args(17); bad->fail = true; Invoke(handler, bad);
        std::vector<GattPaddlePacket> packets; f.machine.Drain(packets);
        CHECK(packets.size() == 2 && packets[0].hasPayload && !packets[1].hasPayload && packets[1].gap);
        f.Retire(); f.Finish();
    });
    test("drain returns with held callback lock and preserves queued edges", [] {
        Fixture f; f.Reach(Phase::Streaming);
        auto bytes = Payload(1); f.gate->Push(bytes.data(), 17, 1);
        std::vector<GattPaddlePacket> packets;
        AcquireSRWLockExclusive(&f.gate->lock);
        f.machine.Drain(packets);
        ReleaseSRWLockExclusive(&f.gate->lock);
        CHECK(packets.empty());
        f.machine.Drain(packets); CHECK(packets.size() == 1);
        f.Retire(); f.Finish();
    });
    test("retire every phase cancels once and waits for terminal", [] {
        for (auto phase : {Phase::Idle, Phase::Discover, Phase::Open, Phase::Verify, Phase::Services,
                Phase::ServicesUncached, Phase::Characteristics, Phase::CharacteristicsUncached,
                Phase::ReadOriginal, Phase::WriteNotify, Phase::Streaming, Phase::ReadRestore, Phase::WriteRestore, Phase::Done}) {
            Fixture f;
            f.stats->emptyServices = f.stats->emptyCharacteristics = true;
            if (phase == Phase::ReadRestore || phase == Phase::WriteRestore || phase == Phase::Done) {
                f.Reach(Phase::Streaming); f.Retire();
                f.Pump();
                if (phase != Phase::ReadRestore) { f.Complete(); f.Pump(); }
                if (phase == Phase::Done) { f.Complete(); f.Pump(); }
            } else if (phase != Phase::Idle) { f.Reach(phase); }
            CHECK(f.machine.phase == phase);
            f.Retire();
            CHECK(!f.gate->active.load());
            auto closes = f.stats->closes;
            if (f.stats->pending) { f.Pump(); CHECK(!f.machine.finished && f.stats->closes == closes); }
            f.Finish();
            auto writes = f.stats->Count(Phase::WriteRestore);
            CHECK(writes == (phase == Phase::WriteNotify || phase == Phase::Streaming ||
                phase == Phase::ReadRestore || phase == Phase::WriteRestore || phase == Phase::Done ? 1u : 0u));
        }
    });
    test("notify completion after retire restores only after terminal", [] {
        Fixture f; f.Reach(Phase::WriteNotify); f.Retire();
        f.now += 60000; f.Pump();
        CHECK(f.machine.error.cleanupPending && !f.machine.finished && f.stats->cancels == 1);
        CHECK(f.stats->Count(Phase::ReadRestore) == 0);
        f.Complete(); f.Finish();
        CHECK(f.stats->Count(Phase::ReadRestore) == 1 && f.stats->Count(Phase::WriteRestore) == 1);
    });
    test("canceled or failed notify still gets comparison after terminal", [] {
        for (auto status : {AsyncStatus::Canceled, AsyncStatus::Error}) {
            Fixture f; f.Reach(Phase::WriteNotify); f.Retire(); f.Complete(status); f.Finish();
            CHECK(f.stats->Count(Phase::ReadRestore) == 1 && f.stats->Count(Phase::WriteRestore) == 1);
        }
    });
    test("unknown status retained beyond cleanup deadline then recoverable", [] {
        for (bool throws : {false, true}) {
            Fixture f; f.Reach(Phase::Open);
            f.stats->throwStatus = throws;
            f.stats->status = static_cast<AsyncStatus>(99);
            f.Pump();
            auto initiating = f.machine.error;
            f.now += 60000; f.Pump();
            CHECK(!f.machine.finished && f.stats->pending && f.stats->cancels == 1 && f.stats->closes == 1);
            CHECK(f.machine.error.code == initiating.code && f.machine.error.cleanupPending);
            f.stats->throwStatus = false; f.Complete(); f.Finish();
            CHECK(!f.machine.error.cleanupPending);
        }
    });
    test("timeout preserves initiating error and emits retirement marker once", [] {
        Fixture f; f.Reach(Phase::Discover);
        f.now += StepTimeoutMs; f.Pump();
        CHECK(f.machine.error.code == Code::Timeout && !f.machine.finished);
        f.now += CleanupTimeoutMs; f.Pump();
        CHECK(f.machine.error.code == Code::Timeout && f.machine.error.cleanupPending);
        std::vector<GattPaddlePacket> packets; f.machine.Drain(packets); f.machine.Drain(packets);
        CHECK(packets.size() == 1 && packets[0].retired && packets[0].gap && !packets[0].hasPayload);
        CHECK(packets[0].attachmentGeneration == 77);
        f.Complete(AsyncStatus::Canceled); f.Finish();
    });
    test("preexisting notify and indicate subscriptions are preserved", [] {
        for (auto original : {Cccd::Notify, Cccd::Indicate, static_cast<Cccd>(3)}) {
            Fixture f; f.stats->original = original; f.Reach(Phase::ReadOriginal); f.Complete();
            if (original == Cccd::Notify) { CHECK(f.machine.phase == Phase::Streaming); }
            else { CHECK(f.machine.retired && f.machine.error.code == Code::Unsupported); }
            f.Retire(); f.Finish();
            CHECK(f.stats->Count(Phase::WriteNotify) == 0 && f.stats->Count(Phase::ReadRestore) == 0);
        }
    });
    test("later CCCD policy change skips restore", [] {
        for (auto value : {Cccd::None, Cccd::Indicate, static_cast<Cccd>(3)}) {
            Fixture f; f.stats->current = value; f.Reach(Phase::Streaming); f.Retire(); f.Finish();
            CHECK(f.stats->Count(Phase::ReadRestore) == 1 && f.stats->Count(Phase::WriteRestore) == 0);
        }
    });
    test("restore read retries once and never writes blindly", [] {
        Fixture f; f.Reach(Phase::Streaming); f.Retire();
        f.stats->throwResults = Phase::ReadRestore;
        f.Finish();
        CHECK(f.stats->Count(Phase::ReadRestore) == 2 && f.stats->Count(Phase::WriteRestore) == 0);
        CHECK(f.machine.error.cleanupCode == Code::CleanupFailed && f.machine.error.cleanupPhase == Phase::ReadRestore);
    });
    test("restore read retry can succeed with changed policy", [] {
        Fixture f; f.Reach(Phase::Streaming); f.Retire(); f.Pump();
        f.Complete(AsyncStatus::Error);
        CHECK(f.machine.error.cleanupCode == Code::None);
        f.stats->current = Cccd::Indicate; f.Finish();
        CHECK(f.stats->Count(Phase::ReadRestore) == 2 && f.stats->Count(Phase::WriteRestore) == 0);
        CHECK(f.machine.error.cleanupCode == Code::None);
    });
    test("restore read timeout retains operation and allows late success", [] {
        Fixture f; f.Reach(Phase::Streaming); f.Retire(); f.Pump();
        f.now += StepTimeoutMs; f.Pump();
        CHECK(f.stats->pending && f.stats->cancels == 1 && f.stats->Count(Phase::ReadRestore) == 1);
        f.Complete(); f.Finish();
        CHECK(f.stats->Count(Phase::WriteRestore) == 1 && f.machine.error.cleanupCode == Code::None);
    });
    test("restore write failure remains explicit with inert callback", [] {
        Fixture f; f.Reach(Phase::Streaming); f.Retire();
        f.stats->throwResults = Phase::WriteRestore; f.Finish();
        CHECK(f.machine.error.cleanupCode == Code::CleanupFailed && f.machine.error.cleanupPhase == Phase::WriteRestore);
        CHECK(!f.gate->active.load() && f.stats->Count(Phase::WriteRestore) == 1);
    });
    test("terminal Close failure does not skip late Notify restore", [] {
        Fixture f; f.Reach(Phase::WriteNotify); f.Retire(); f.stats->throwClose = true;
        f.Complete(); f.Finish();
        CHECK(f.stats->Count(Phase::WriteRestore) == 1 && f.machine.error.cleanupCode == Code::CleanupFailed);
    });
    test("failed subscribe and failed notify initiation keep gate inert", [] {
        for (bool subscribe : {false, true}) {
            Fixture f; f.stats->throwSubscribe = subscribe;
            if (!subscribe) { f.stats->throwBegin = Phase::WriteNotify; }
            f.Reach(Phase::ReadOriginal); f.Complete();
            if (!subscribe) { f.Pump(); }
            CHECK(f.machine.retired && !f.gate->active.load()); f.Finish();
            CHECK(f.stats->Count(Phase::ReadRestore) == 0);
        }
    });
    test("one uncached retry per successful empty attribute result", [] {
        Fixture f; f.stats->emptyServices = f.stats->emptyCharacteristics = true;
        f.Reach(Phase::Streaming); f.Retire(); f.Finish();
        CHECK(f.stats->Count(Phase::Services) == 1 && f.stats->Count(Phase::ServicesUncached) == 1);
        CHECK(f.stats->Count(Phase::Characteristics) == 1 && f.stats->Count(Phase::CharacteristicsUncached) == 1);
    });
    test("actual WinRT operation adapter status cancel terminal results close", [] {
        WindowsAdapter adapter(Target);
        auto operation = winrt::make_self<ManualOperation<GattCommunicationStatus>>(GattCommunicationStatus::Success);
        adapter.InjectOperation(operation.as<IAsyncOperation<GattCommunicationStatus>>());
        CHECK(adapter.Status() == AsyncStatus::Started); adapter.Cancel();
        CHECK(operation->cancels == 1 && operation->results == 0 && operation->closes == 0);
        operation->status = AsyncStatus::Completed;
        CHECK(adapter.Status() == AsyncStatus::Completed); adapter.Results(Phase::WriteNotify); adapter.CloseOperation();
        CHECK(operation->results == 1 && operation->closes == 1 && !operation->completed);
        adapter.Release();
    });
    test("actual adapter rejects unreachable and access-denied GATT results", [] {
        for (auto status : {GattCommunicationStatus::Unreachable, GattCommunicationStatus::AccessDenied, GattCommunicationStatus::ProtocolError}) {
            WindowsAdapter adapter(Target);
            auto operation = winrt::make_self<ManualOperation<GattCommunicationStatus>>(status);
            operation->status = AsyncStatus::Completed;
            adapter.InjectOperation(operation.as<IAsyncOperation<GattCommunicationStatus>>());
            bool rejected = false;
            try { adapter.Results(Phase::WriteNotify); }
            catch (winrt::hresult_error const& error) {
                rejected = true;
                CHECK(error.code().value == (status == GattCommunicationStatus::Unreachable ? HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) :
                    status == GattCommunicationStatus::AccessDenied ? E_ACCESSDENIED : E_FAIL));
            }
            CHECK(rejected); adapter.CloseOperation(); adapter.Release();
        }
    });
    test("actual empty attribute results retry cached and reject uncached", [] {
        for (bool services : {true, false}) {
            WindowsAdapter adapter(Target);
            auto install = [&] {
                if (services) {
                    auto result = winrt::make<ServiceResult>().as<GattDeviceServicesResult>();
                    auto op = winrt::make_self<ManualOperation<GattDeviceServicesResult>>(result);
                    op->status = AsyncStatus::Completed;
                    adapter.InjectOperation(op.as<IAsyncOperation<GattDeviceServicesResult>>());
                } else {
                    auto result = winrt::make<CharacteristicResult>().as<GattCharacteristicsResult>();
                    auto op = winrt::make_self<ManualOperation<GattCharacteristicsResult>>(result);
                    op->status = AsyncStatus::Completed;
                    adapter.InjectOperation(op.as<IAsyncOperation<GattCharacteristicsResult>>());
                }
            };
            install(); CHECK(adapter.Results(services ? Phase::Services : Phase::Characteristics).emptyAttributes);
            adapter.CloseOperation(); install();
            bool rejected = false;
            try { adapter.Results(services ? Phase::ServicesUncached : Phase::CharacteristicsUncached); }
            catch (GattPaddleError const& error) { rejected = true; CHECK(error.code == Code::Unsupported); }
            CHECK(rejected); adapter.CloseOperation(); adapter.Release();
        }
    });
    test("unreachable operation and disconnect retire and retain initiating reason", [] {
        Fixture f; f.Reach(Phase::Services); f.Complete(AsyncStatus::Error); f.Finish();
        CHECK(f.machine.error.code == Code::OperationFailed && f.machine.error.hresult == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED));
        Fixture g; g.Reach(Phase::Streaming); g.stats->connected = false; g.Pump(); g.Finish();
        CHECK(g.machine.error.code == Code::NotConnected);
    });
    test("late delegate survives client state and cannot cross generations", [] {
        decltype(NotificationHandler({})) retained{nullptr};
        std::weak_ptr<Gate> oldGate;
        {
            Fixture f; f.Reach(Phase::Streaming); oldGate = f.gate;
            retained = NotificationHandler(f.gate); f.Retire(); f.Finish();
        }
        CHECK(!oldGate.expired() && livePermits.load() == 1);
        auto args = Args(17); Invoke(retained, args); CHECK(args->accesses.load() == 0);
        auto newGate = std::make_shared<Gate>(88, AcquirePermit()); newGate->active.store(true);
        std::vector<GattPaddlePacket> packets; newGate->Drain(packets); CHECK(packets.empty());
        retained = nullptr; CHECK(oldGate.expired());
    });
    test("concurrent callback retirement preserves lifetime and blocks late reads", [] {
        Fixture f; f.Reach(Phase::Streaming);
        auto handler = NotificationHandler(f.gate); auto args = Args(17);
        std::atomic<bool> started{false};
        std::thread worker([handler, args, &started] {
            started.store(true);
            for (unsigned i = 0; i < 10000; ++i) { Invoke(handler, args); }
        });
        while (!started.load()) { std::this_thread::yield(); }
        f.Retire(); worker.join();
        auto count = args->accesses.load(); Invoke(handler, args); CHECK(args->accesses.load() == count);
        f.Finish();
    });
    test("16 retained callback gates refuse further admission until released", [] {
        std::vector<decltype(NotificationHandler({}))> retained;
        for (unsigned i = 0; i < InstanceLimit; ++i) {
            auto gate = std::make_shared<Gate>(i, AcquirePermit()); retained.push_back(NotificationHandler(gate));
        }
        CHECK(livePermits.load() == InstanceLimit && !AcquirePermit());
        GattPaddleClient client(Target, 999); GattPaddleError error;
        CHECK(!client.Start(error) && error.code == Code::InstanceLimit);
        retained.pop_back(); auto permit = AcquirePermit(); CHECK(permit && livePermits.load() == InstanceLimit);
        permit.reset(); retained.clear();
    });
    test("wrong-thread Retire calls no adapter operations and owner finishes", [] {
        auto stats = std::make_shared<Stats>();
        GattPaddleClient client(Target, 44); GattPaddleTestAccess::Install(client, stats);
        std::vector<GattPaddlePacket> packets; GattPaddleError error;
        CHECK(client.Pump(packets, error) && stats->pending);
        std::thread worker([&] {
            client.Retire();
            CHECK(!client.Finished() && client.BluetoothAddress() == 0);
        }); worker.join();
        CHECK(stats->cancels == 0 && stats->revokes == 0 && !stats->gate.lock()->active.load());
        CHECK(!client.Pump(packets, error)); CHECK(stats->cancels == 1);
        stats->status = AsyncStatus::Canceled; client.Pump(packets, error); client.Pump(packets, error);
        CHECK(client.Finished());
    });
    test("wrong-thread destruction transfers pending cleanup to owner", [] {
        auto stats = std::make_shared<Stats>();
        auto client = std::make_unique<GattPaddleClient>(Target, 55);
        GattPaddleTestAccess::Install(*client, stats);
        std::vector<GattPaddlePacket> packets; GattPaddleError error;
        client->Pump(packets, error);
        std::thread worker([client = std::move(client)]() mutable { client.reset(); }); worker.join();
        CHECK(stats->cancels == 0 && stats->revokes == 0 && livePermits.load() == 1);
        CHECK(GattPaddleClient::PumpRetired(error) == 1 && stats->cancels == 1);
        stats->status = AsyncStatus::Canceled;
        CHECK(GattPaddleClient::PumpRetired(error) == 1);
        CHECK(GattPaddleClient::PumpRetired(error) == 0 && stats->releases == 1);
    });
    test("16 orphan operations remain bounded and return permits after terminal", [] {
        std::vector<std::shared_ptr<Stats>> states;
        for (unsigned i = 0; i < InstanceLimit; ++i) {
            auto stats = std::make_shared<Stats>(); states.push_back(stats);
            GattPaddleClient client(Target, i); GattPaddleTestAccess::Install(client, stats);
            std::vector<GattPaddlePacket> packets; GattPaddleError error; client.Pump(packets, error);
        }
        GattPaddleError error;
        CHECK(livePermits.load() == InstanceLimit && !AcquirePermit());
        CHECK(GattPaddleClient::PumpRetired(error) == InstanceLimit);
        for (auto const& state : states) { CHECK(state->cancels == 1 && state->closes == 0); state->status = AsyncStatus::Canceled; }
        CHECK(GattPaddleClient::PumpRetired(error) == InstanceLimit);
        CHECK(GattPaddleClient::PumpRetired(error) == 0 && livePermits.load() == 0);
    });
    test("constructor and invalid Start perform no I/O or permit admission", [] {
        GattPaddleClient client({}, 1); GattPaddleError error;
        CHECK(client.Finished() && livePermits.load() == 0);
        CHECK(!client.Start(error) && error.code == Code::InvalidIdentity);
        const GUID system{0, 0, 0, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
        GattPaddleClient invalid(system, 2);
        CHECK(!invalid.Start(error) && error.code == Code::InvalidIdentity);
        std::thread worker([&] {
            GattPaddleError local;
            CHECK(!client.Start(local) && local.code == Code::WrongThread);
        }); worker.join();
    });

    test("retire between Start check and publication cannot start acquisition", [] {
        auto stats = std::make_shared<Stats>();
        GattPaddleClient client(Target, 91); GattPaddleError error;
        bool started = GattPaddleTestAccess::StartWithEarlyRetire(client, stats, error);
        bool beganAcquisition = !stats->begins.empty();
        bool alreadyFinished = client.Finished();
        // Drain even on regression, keeping the test independent of orphan
        // cleanup and leaving the failing invariant visible in its own result.
        client.Retire();
        std::vector<GattPaddlePacket> packets;
        if (stats->pending) { stats->status = AsyncStatus::Canceled; }
        for (unsigned i = 0; !client.Finished() && i < 4; ++i) { client.Pump(packets, error); }
        CHECK(client.Finished());
        CHECK(!started && !beganAcquisition && alreadyFinished);
        for (auto const& packet : packets) { CHECK(packet.retired && !packet.hasPayload); }
    });
    test("active Close failure has initiating and cleanup errors", [] {
        Fixture f; f.Reach(Phase::WriteNotify); f.stats->throwClose = true;
        f.Complete(); f.Finish();
        CHECK(f.machine.error.code == Code::OperationFailed);
        CHECK(f.machine.error.phase == Phase::WriteNotify && f.machine.error.hresult == E_FAIL);
        CHECK(f.machine.error.cleanupCode == Code::CleanupFailed && f.machine.error.cleanupHresult == E_FAIL);
        CHECK(f.stats->Count(Phase::WriteRestore) == 1);
    });

    std::printf("RESULT %u passed, %u failed\n", passed, failed);
    winrt::uninit_apartment();
    return failed ? 1 : 0;
}
