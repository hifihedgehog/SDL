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
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "SDL_xinput_paddle_gatt.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <robuffer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace sdl_paddles {
namespace gatt_detail {

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using Cccd = GattClientCharacteristicConfigurationDescriptorValue;
using Phase = GattPaddlePhase;
using Code = GattPaddleErrorCode;
constexpr unsigned InstanceLimit = 16;
constexpr std::size_t QueueCapacity = 1024;
constexpr std::uint64_t StepTimeoutMs = 8000;
constexpr std::uint64_t CleanupTimeoutMs = 30000;
constexpr unsigned RestoreReadLimit = 2;
constexpr winrt::guid ServiceUuid{0x00000001, 0x5f60, 0x4c4f, {0x9c, 0x83, 0xa7, 0x95, 0x32, 0x98, 0xd4, 0x0d}};
constexpr winrt::guid CharacteristicUuid{0x00000005, 0x5f60, 0x4c4f, {0x9c, 0x83, 0xa7, 0x95, 0x32, 0x98, 0xd4, 0x0d}};

static bool Equal(GUID const& a, GUID const& b) noexcept
{
    return InlineIsEqualGUID(a, b) != 0;
}

static bool ValidContainer(GUID const& id) noexcept
{
    constexpr GUID system{0, 0, 0, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
    return !Equal(id, GUID{}) && !Equal(id, system);
}

static GattPaddleError Error(Code code, Phase phase, HRESULT hr = E_FAIL) noexcept
{
    return {code, phase, static_cast<std::int32_t>(hr)};
}

static HRESULT ExceptionCode() noexcept
{
    try { throw; }
    catch (winrt::hresult_error const& e) { return e.code().value; }
    catch (std::bad_alloc const&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

// The permit lives as long as the last delegate's gate, even if its client and
// COM objects are gone. A failed revocation cannot admit unbounded new gates.
static std::atomic<unsigned> livePermits{0};
struct Permit {
    ~Permit() { livePermits.fetch_sub(1, std::memory_order_relaxed); }
};

static std::shared_ptr<Permit> AcquirePermit()
{
    unsigned count = livePermits.load(std::memory_order_relaxed);
    do {
        if (count >= InstanceLimit) { return {}; }
    } while (!livePermits.compare_exchange_weak(count, count + 1, std::memory_order_relaxed));
    try { return std::make_shared<Permit>(); }
    catch (...) { livePermits.fetch_sub(1, std::memory_order_relaxed); throw; }
}

struct Gate {
    explicit Gate(std::uint64_t generation, std::shared_ptr<Permit> permit)
        : generation(generation), permit(std::move(permit)) {}

    const std::uint64_t generation;
    const std::shared_ptr<Permit> permit;
    std::atomic<bool> active{false};
    SRWLOCK lock = SRWLOCK_INIT;
    std::array<GattPaddlePacket, QueueCapacity> queue{};
    std::size_t count = 0;
    bool gapPending = false;
    GattPaddlePacket resync{};

    // Called only with the queue lock held. A second unknown report invalidates
    // an earlier resync snapshot but never erases the valid prefix.
    void MarkGap() noexcept
    {
        gapPending = true;
        resync = {};
        resync.attachmentGeneration = generation;
        resync.gap = true;
    }

    void Gap() noexcept
    {
        AcquireSRWLockExclusive(&lock);
        if (active.load(std::memory_order_acquire)) {
            MarkGap();
        }
        ReleaseSRWLockExclusive(&lock);
    }

    void Push(std::uint8_t const* bytes, std::size_t size, std::int64_t qpc) noexcept
    {
        if (!active.load(std::memory_order_acquire)) { return; }
        if (size != 17 || !bytes) { Gap(); return; }
        AcquireSRWLockExclusive(&lock);
        if (!active.load(std::memory_order_acquire)) {
            ReleaseSRWLockExclusive(&lock);
            return;
        }
        if (count == queue.size() && !gapPending) { MarkGap(); }
        auto& packet = gapPending ? resync : queue[count++];
        packet = {};
        packet.attachmentGeneration = generation;
        packet.receivedQpc = qpc;
        packet.hasPayload = true;
        packet.gap = gapPending;
        std::memcpy(packet.bytes.data(), bytes, packet.bytes.size());
        ReleaseSRWLockExclusive(&lock);
    }

    void Drain(std::vector<GattPaddlePacket>& packets)
    {
        // Reserve before taking the callback lock. Allocation failure leaves
        // the queued edges intact, and the owner can retry on its next pump.
        constexpr auto limit = QueueCapacity + 1;
        if (packets.max_size() - packets.size() < limit) { throw std::bad_alloc(); }
        packets.reserve(packets.size() + limit);
        if (!TryAcquireSRWLockExclusive(&lock)) { return; }
        if (active.load(std::memory_order_acquire)) {
            for (std::size_t i = 0; i < count; ++i) { packets.push_back(queue[i]); }
            if (gapPending) { packets.push_back(resync); }
            count = 0;
            gapPending = false;
            resync = {};
        }
        ReleaseSRWLockExclusive(&lock);
    }
};

struct PhaseResult {
    Cccd cccd = Cccd::None;
    bool emptyAttributes = false;
};

// This boundary is shared by production and offline tests. The machine owns
// sequencing and retirement. The adapter owns all WinRT objects and operations.
struct Adapter {
    virtual ~Adapter() = default;
    virtual void Begin(Phase phase, Cccd original) = 0;
    virtual AsyncStatus Status() = 0;
    virtual HRESULT OperationError() = 0;
    virtual PhaseResult Results(Phase phase) = 0;
    virtual void Cancel() = 0;
    virtual void CloseOperation() = 0;
    virtual void Subscribe(std::shared_ptr<Gate> const& gate) = 0;
    virtual void Revoke() = 0;
    virtual bool Connected() = 0;
    virtual void Release() = 0;
    virtual std::uint64_t Address() const noexcept = 0;
};

struct Endpoint {
    std::wstring id;
    GUID container{};
    bool resolved = false;
    std::uint64_t address = 0;
    bool hasAddress = false;
};

struct BthDevice {
    std::wstring id;
    GUID container{};
    std::uint64_t address = 0;
};

static bool SameEndpoint(std::wstring const& a, std::wstring const& b) noexcept
{
    return a.size() == b.size() && a.size() <= 32767 &&
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

static bool ParseBluetoothAddress(std::wstring_view text, std::uint64_t& address) noexcept
{
    if (text.size() != 12 && text.size() != 17) { return false; }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        wchar_t ch = text[i];
        if (text.size() == 17 && i % 3 == 2) {
            if (ch != L':') { return false; }
            continue;
        }
        unsigned digit;
        if (ch >= L'0' && ch <= L'9') { digit = ch - L'0'; }
        else if (ch >= L'a' && ch <= L'f') { digit = ch - L'a' + 10; }
        else if (ch >= L'A' && ch <= L'F') { digit = ch - L'A' + 10; }
        else { return false; }
        value = (value << 4) | digit;
    }
    if (!value) { return false; }
    address = value;
    return true;
}

static BthDevice SelectBthDevice(std::vector<BthDevice> const& devices, GUID const& container)
{
    if (!ValidContainer(container)) { throw Error(Code::InvalidIdentity, Phase::Discover); }
    const BthDevice* selected = nullptr;
    for (auto const& device : devices) {
        if (Equal(device.container, container)) {
            if (selected) { throw Error(Code::AmbiguousIdentity, Phase::Discover); }
            selected = &device;
        }
    }
    if (!selected) { throw Error(Code::IdentityMismatch, Phase::Discover); }
    if (selected->id.empty() || !selected->address) { throw Error(Code::IdentityUnavailable, Phase::Discover); }
    return *selected;
}

static bool SameBthDevice(BthDevice const& a, BthDevice const& b) noexcept
{
    return ValidContainer(a.container) && Equal(a.container, b.container) && a.address &&
        a.address == b.address && SameEndpoint(a.id, b.id);
}

static std::wstring SelectEndpoint(std::vector<Endpoint> const& endpoints, BthDevice const& device)
{
    if (!ValidContainer(device.container) || !device.address) { throw Error(Code::InvalidIdentity, Phase::Discover); }
    const Endpoint* selected = nullptr;
    for (auto const& endpoint : endpoints) {
        if (!endpoint.hasAddress) { throw Error(Code::IdentityUnavailable, Phase::Discover); }
        if (endpoint.address != device.address) { continue; }
        // Count every address-value match before applying the container check.
        // A second LE endpoint is ambiguous even with a different container.
        if (selected) { throw Error(Code::AmbiguousIdentity, Phase::Discover); }
        selected = &endpoint;
    }
    if (!selected) { throw Error(Code::IdentityMismatch, Phase::Discover); }
    if (!selected->resolved || !ValidContainer(selected->container) || selected->id.empty()) {
        throw Error(Code::IdentityUnavailable, Phase::Discover);
    }
    if (!Equal(selected->container, device.container)) { throw Error(Code::IdentityMismatch, Phase::Discover); }
    return selected->id;
}

static BthDevice ResolveBthDevice(GUID const& container)
{
    // SDK bthguid.h DEVPKEY_Bluetooth_DeviceAddress and propkey.h
    // PKEY_DeviceInterface_Bluetooth_DeviceAddress define this STRING key.
    constexpr DEVPROPKEY addressKey{{0x2bd67d8b, 0x8beb, 0x48d5,
        {0x87, 0xe0, 0x6c, 0xda, 0x34, 0x28, 0x04, 0x0a}}, 1};
    ULONG count = 0;
    auto cr = CM_Get_Device_ID_List_SizeW(&count, L"BTHLE", CM_GETIDLIST_FILTER_ENUMERATOR);
    if (cr != CR_SUCCESS || !count || count > 1048576) { throw Error(Code::IdentityUnavailable, Phase::Discover); }
    std::vector<wchar_t> ids(count);
    if (CM_Get_Device_ID_ListW(L"BTHLE", ids.data(), count, CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS) {
        throw Error(Code::IdentityUnavailable, Phase::Discover);
    }
    std::vector<BthDevice> devices;
    unsigned visited = 0;
    for (std::size_t offset = 0; offset < ids.size() && ids[offset];) {
        const auto start = offset;
        while (offset < ids.size() && ids[offset]) { ++offset; }
        if (offset == ids.size() || ++visited > 4096) { throw Error(Code::IdentityUnavailable, Phase::Discover); }
        BthDevice device;
        device.id.assign(ids.data() + start, offset - start);
        ++offset;
        DEVINST node = 0;
        // Include stored roots in the uniqueness check. A duplicate phantom
        // root disables this attempt instead of silently selecting a radio.
        if (CM_Locate_DevNodeW(&node, device.id.data(), CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS) { continue; }
        DEVPROPTYPE type = 0; ULONG size = sizeof(GUID);
        cr = CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_ContainerId, &type,
            reinterpret_cast<PBYTE>(&device.container), &size, 0);
        if (cr != CR_SUCCESS || type != DEVPROP_TYPE_GUID || size != sizeof(GUID) || !Equal(device.container, container)) { continue; }
        std::array<wchar_t, 64> address{};
        size = static_cast<ULONG>(sizeof(address));
        cr = CM_Get_DevNode_PropertyW(node, &addressKey, &type, reinterpret_cast<PBYTE>(address.data()), &size, 0);
        if (cr == CR_SUCCESS && type == DEVPROP_TYPE_STRING && size >= sizeof(wchar_t) && size <= sizeof(address) &&
            size % sizeof(wchar_t) == 0 && address[size / sizeof(wchar_t) - 1] == 0) {
            ParseBluetoothAddress(std::wstring_view(address.data(), size / sizeof(wchar_t) - 1), device.address);
        }
        devices.push_back(std::move(device));
    }
    return SelectBthDevice(devices, container);
}

static Endpoint ReadEndpoint(DeviceInformation const& info)
{
    Endpoint endpoint;
    endpoint.id = info.Id();
    auto properties = info.Properties();
    constexpr wchar_t key[] = L"System.Devices.Aep.ContainerId";
    if (properties.HasKey(key)) {
        auto boxed = properties.Lookup(key);
        auto value = boxed.try_as<IPropertyValue>();
        if (value && value.Type() == PropertyType::Guid) {
            endpoint.container = value.GetGuid();
            endpoint.resolved = ValidContainer(endpoint.container);
        }
    }
    constexpr wchar_t addressKey[] = L"System.Devices.Aep.DeviceAddress";
    if (properties.HasKey(addressKey)) {
        auto address = properties.Lookup(addressKey).try_as<IPropertyValue>();
        if (address && address.Type() == PropertyType::String) {
            endpoint.hasAddress = ParseBluetoothAddress(std::wstring_view(address.GetString()), endpoint.address);
        }
    }
    return endpoint;
}

static auto NotificationHandler(std::shared_ptr<Gate> gate)
{
    return TypedEventHandler<GattCharacteristic, GattValueChangedEventArgs>{
        [gate = std::move(gate)](GattCharacteristic const&, GattValueChangedEventArgs const& args) noexcept {
            if (!gate->active.load(std::memory_order_acquire)) { return; }
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            try {
                // Only access the event's owned buffer. No sender, device,
                // discovery, SDL, or CCCD calls occur in the callback.
                auto buffer = args.CharacteristicValue();
                // The vendor contract defines exactly 17 bytes. Unknown shapes
                // cannot qualify input and make the current state uncertain.
                if (buffer.Length() != 17) { gate->Gap(); return; }
                auto access = buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();
                std::uint8_t* data = nullptr;
                winrt::check_hresult(access->Buffer(&data));
                if (!data) { gate->Gap(); return; }
                gate->Push(data, 17, qpc.QuadPart);
            } catch (...) { gate->Gap(); }
        }};
}

class WindowsAdapter final : public Adapter {
public:
    explicit WindowsAdapter(GUID container) : container_(container) {}

    void Begin(Phase phase, Cccd original) override
    {
        switch (phase) {
        case Phase::Discover:
            bthDevice_ = ResolveBthDevice(container_);
            [[fallthrough]];
        case Phase::Verify:
            if (phase == Phase::Verify && !SameBthDevice(bthDevice_, ResolveBthDevice(container_))) {
                throw Error(Code::IdentityMismatch, phase);
            }
            operation_ = DeviceInformation::FindAllAsync(
                BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected),
                {L"System.Devices.Aep.ContainerId", L"System.Devices.Aep.DeviceAddress"},
                DeviceInformationKind::AssociationEndpoint);
            break;
        case Phase::Open:
            operation_ = BluetoothLEDevice::FromIdAsync(selected_);
            break;
        case Phase::Services:
        case Phase::ServicesUncached:
            operation_ = device_.GetGattServicesForUuidAsync(ServiceUuid,
                phase == Phase::Services ? BluetoothCacheMode::Cached : BluetoothCacheMode::Uncached);
            break;
        case Phase::Characteristics:
        case Phase::CharacteristicsUncached:
            operation_ = service_.GetCharacteristicsForUuidAsync(CharacteristicUuid,
                phase == Phase::Characteristics ? BluetoothCacheMode::Cached : BluetoothCacheMode::Uncached);
            break;
        case Phase::ReadOriginal:
        case Phase::ReadRestore:
            operation_ = characteristic_.ReadClientCharacteristicConfigurationDescriptorAsync();
            break;
        case Phase::WriteNotify:
            operation_ = characteristic_.WriteClientCharacteristicConfigurationDescriptorAsync(Cccd::Notify);
            break;
        case Phase::WriteRestore:
            operation_ = characteristic_.WriteClientCharacteristicConfigurationDescriptorAsync(original);
            break;
        default:
            throw winrt::hresult_error(E_UNEXPECTED);
        }
    }

    AsyncStatus Status() override { return Info().Status(); }
    HRESULT OperationError() override { return Info().ErrorCode().value; }
    void Cancel() override { Info().Cancel(); }
    void CloseOperation() override
    {
        // Terminal status is already proven. Drop our reference even if Close
        // fails so an attempted Notify write can still get its restore check.
        auto info = Info();
        operation_ = std::monostate{};
        info.Close();
    }

    PhaseResult Results(Phase phase) override
    {
        switch (phase) {
        case Phase::Discover:
        case Phase::Verify: {
            auto infos = std::get<IAsyncOperation<DeviceInformationCollection>>(operation_).GetResults();
            if (infos.Size() > 256) { throw Error(Code::IdentityUnavailable, phase); }
            std::vector<Endpoint> endpoints;
            endpoints.reserve(infos.Size());
            for (auto const& info : infos) { endpoints.push_back(ReadEndpoint(info)); }
            if (!SameBthDevice(bthDevice_, ResolveBthDevice(container_))) { throw Error(Code::IdentityMismatch, phase); }
            auto selected = SelectEndpoint(endpoints, bthDevice_);
            if (phase == Phase::Discover) {
                selected_ = std::move(selected);
            } else {
                if (!device_ || !SameEndpoint(selected, selected_) ||
                    !SameEndpoint(std::wstring(device_.DeviceId()), selected_)) {
                    throw Error(Code::IdentityMismatch, phase);
                }
                if (!Connected()) { throw Error(Code::NotConnected, phase); }
                address_ = device_.BluetoothAddress();
                if (address_ != bthDevice_.address) { throw Error(Code::IdentityMismatch, phase); }
            }
            break;
        }
        case Phase::Open:
            device_ = std::get<IAsyncOperation<BluetoothLEDevice>>(operation_).GetResults();
            if (!device_ || !SameEndpoint(std::wstring(device_.DeviceId()), selected_)) {
                throw Error(Code::IdentityMismatch, phase);
            }
            if (!Connected()) { throw Error(Code::NotConnected, phase); }
            if (device_.BluetoothAddress() != bthDevice_.address) { throw Error(Code::IdentityMismatch, phase); }
            break;
        case Phase::Services:
        case Phase::ServicesUncached: {
            auto result = std::get<IAsyncOperation<GattDeviceServicesResult>>(operation_).GetResults();
            Check(result.Status());
            auto services = result.Services();
            if (services.Size() == 0 && phase == Phase::Services) { return {Cccd::None, true}; }
            if (services.Size() != 1) {
                for (auto const& service : services) { service.Close(); }
                throw Error(Code::Unsupported, phase);
            }
            service_ = services.GetAt(0);
            if (service_.Uuid() != ServiceUuid) { throw Error(Code::Unsupported, phase); }
            break;
        }
        case Phase::Characteristics:
        case Phase::CharacteristicsUncached: {
            auto result = std::get<IAsyncOperation<GattCharacteristicsResult>>(operation_).GetResults();
            Check(result.Status());
            if (result.Characteristics().Size() == 0 && phase == Phase::Characteristics) { return {Cccd::None, true}; }
            if (result.Characteristics().Size() != 1) { throw Error(Code::Unsupported, phase); }
            characteristic_ = result.Characteristics().GetAt(0);
            if (characteristic_.Uuid() != CharacteristicUuid ||
                (characteristic_.CharacteristicProperties() & GattCharacteristicProperties::Notify) == GattCharacteristicProperties::None) {
                throw Error(Code::Unsupported, phase);
            }
            break;
        }
        case Phase::ReadOriginal:
        case Phase::ReadRestore: {
            auto result = std::get<IAsyncOperation<GattReadClientCharacteristicConfigurationDescriptorResult>>(operation_).GetResults();
            Check(result.Status());
            return {result.ClientCharacteristicConfigurationDescriptor()};
        }
        case Phase::WriteNotify:
        case Phase::WriteRestore:
            Check(std::get<IAsyncOperation<GattCommunicationStatus>>(operation_).GetResults());
            break;
        default:
            throw winrt::hresult_error(E_UNEXPECTED);
        }
        return {};
    }

    void Subscribe(std::shared_ptr<Gate> const& gate) override
    {
        token_ = characteristic_.ValueChanged(NotificationHandler(gate));
        subscribed_ = true;
    }

    void Revoke() override
    {
        if (subscribed_) {
            characteristic_.ValueChanged(token_);
            subscribed_ = false;
        }
    }

    bool Connected() override { return device_ && device_.ConnectionStatus() == BluetoothConnectionStatus::Connected; }
    std::uint64_t Address() const noexcept override { return address_; }

    void Release() override
    {
        HRESULT failure = S_OK;
        try { Revoke(); } catch (...) { failure = ExceptionCode(); }
        operation_ = std::monostate{};
        characteristic_ = nullptr;
        if (service_) {
            try { service_.Close(); } catch (...) { failure = ExceptionCode(); }
            service_ = nullptr;
        }
        if (device_) {
            try { device_.Close(); } catch (...) { failure = ExceptionCode(); }
            device_ = nullptr;
        }
        if (FAILED(failure)) { throw winrt::hresult_error(failure); }
    }

#ifdef SDL_XINPUT_PADDLE_GATT_TESTING
    template<class T> void InjectOperation(IAsyncOperation<T> operation) { operation_ = std::move(operation); }
#endif

private:
    static void Check(GattCommunicationStatus status)
    {
        if (status != GattCommunicationStatus::Success) {
            const HRESULT hr = status == GattCommunicationStatus::AccessDenied ? E_ACCESSDENIED :
                status == GattCommunicationStatus::Unreachable ? HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) : E_FAIL;
            throw winrt::hresult_error(hr);
        }
    }

    IAsyncInfo Info()
    {
        return std::visit([](auto const& operation) -> IAsyncInfo {
            using T = std::decay_t<decltype(operation)>;
            if constexpr (std::is_same_v<T, std::monostate>) { throw winrt::hresult_error(E_UNEXPECTED); }
            else { return operation.template as<IAsyncInfo>(); }
        }, operation_);
    }

    GUID container_{};
    BthDevice bthDevice_;
    std::wstring selected_;
    std::uint64_t address_ = 0;
    BluetoothLEDevice device_{nullptr};
    GattDeviceService service_{nullptr};
    GattCharacteristic characteristic_{nullptr};
    winrt::event_token token_{};
    bool subscribed_ = false;
    std::variant<std::monostate, IAsyncOperation<DeviceInformationCollection>,
        IAsyncOperation<BluetoothLEDevice>, IAsyncOperation<GattDeviceServicesResult>,
        IAsyncOperation<GattCharacteristicsResult>,
        IAsyncOperation<GattReadClientCharacteristicConfigurationDescriptorResult>,
        IAsyncOperation<GattCommunicationStatus>> operation_;
};

class Machine {
public:
    Machine(std::unique_ptr<Adapter> adapter, std::shared_ptr<Gate> gate)
        : adapter(std::move(adapter)), gate(std::move(gate)) {}

    std::unique_ptr<Adapter> adapter;
    std::shared_ptr<Gate> gate;
    Phase phase = Phase::Idle;
    GattPaddleError error{};
    bool retired = false;
    bool finished = false;

    void RequestRetire() noexcept
    {
        retireRequested_.store(true, std::memory_order_seq_cst);
        gate->active.store(false, std::memory_order_seq_cst);
    }

    void Retire(std::uint64_t now) noexcept
    {
        RequestRetire();
        if (retired) { return; }
        retired = true;
        retirementMarker_ = true;
        cleanupStarted_ = now;
        try { adapter->Revoke(); }
        catch (...) { Record(Error(Code::CleanupFailed, phase, ExceptionCode())); }
        CancelPending();
    }

    void Pump(std::uint64_t now) noexcept
    {
        if (retireRequested_.load(std::memory_order_seq_cst) && !retired) { Retire(now); }
        if (finished) { return; }
        if (retired && now - cleanupStarted_ >= CleanupTimeoutMs) {
            error.cleanupPending = true;
        }
        if (pending_) {
            Poll(now);
            return;
        }
        try {
            if (retired) {
                if (notifyAttempted_ && !restoreChecked_) { Begin(Phase::ReadRestore, now); }
                else if (restoreNeeded_) { Begin(Phase::WriteRestore, now); }
                else { Finish(); }
            } else if (phase == Phase::Idle) {
                Begin(Phase::Discover, now);
            } else if (phase == Phase::Streaming) {
                if (!adapter->Connected()) { Fail(Error(Code::NotConnected, phase), now); }
            } else {
                Begin(next_, now);
            }
        } catch (GattPaddleError const& failure) { Fail(failure, now); }
        catch (...) { Fail(Error(retired ? Code::CleanupFailed : Code::OperationFailed, phase, ExceptionCode()), now); }
    }

    void Drain(std::vector<GattPaddlePacket>& packets)
    {
        if (retirementMarker_) {
            GattPaddlePacket marker{};
            marker.attachmentGeneration = gate->generation;
            marker.gap = marker.retired = true;
            packets.push_back(marker);
            retirementMarker_ = false;
        } else if (!retired && phase == Phase::Streaming) {
            gate->Drain(packets);
        }
    }

private:
    Phase next_ = Phase::Discover;
    Cccd original_ = Cccd::None;
    bool pending_ = false;
    bool canceled_ = false;
    bool notifyAttempted_ = false;
    bool restoreChecked_ = false;
    bool restoreNeeded_ = false;
    bool retirementMarker_ = false;
    std::atomic<bool> retireRequested_{false};
    unsigned restoreReadAttempts_ = 0;
    std::uint64_t started_ = 0;
    std::uint64_t cleanupStarted_ = 0;

    void Record(GattPaddleError failure) noexcept
    {
        if (retired || failure.code == Code::CleanupFailed) {
            if (error.cleanupCode == Code::None) {
                error.cleanupCode = Code::CleanupFailed;
                error.cleanupPhase = failure.phase;
                error.cleanupHresult = failure.hresult;
            }
        } else if (error.code == Code::None) {
            error.code = failure.code;
            error.phase = failure.phase;
            error.hresult = failure.hresult;
        }
    }

    void Fail(GattPaddleError failure, std::uint64_t now) noexcept
    {
        failure.phase = phase;
        if (phase == Phase::ReadRestore) {
            // Retry only after terminal failure. A timeout or unreadable Status
            // leaves the current operation retained, so no retry overlaps it.
            if (!pending_ && restoreReadAttempts_ >= RestoreReadLimit) {
                restoreChecked_ = true;
                Record(Error(Code::CleanupFailed, phase, failure.hresult));
            }
        } else {
            Record(failure);
        }
        if (phase == Phase::WriteRestore) { restoreNeeded_ = false; }
        Retire(now);
    }

    void Begin(Phase target, std::uint64_t now)
    {
        phase = target;
        // An issued write may take effect even when its result is canceled or
        // failed. After terminal status, compare CCCD before any restoration.
        if (target == Phase::ReadRestore) { ++restoreReadAttempts_; }
        adapter->Begin(target, original_);
        if (target == Phase::WriteNotify) { notifyAttempted_ = true; }
        pending_ = true;
        canceled_ = false;
        started_ = now;
    }

    void CancelPending() noexcept
    {
        if (pending_ && !canceled_) {
            canceled_ = true;
            try { adapter->Cancel(); }
            catch (...) { Record(Error(Code::CleanupFailed, phase, ExceptionCode())); }
        }
    }

    void Finish() noexcept
    {
        gate->active.store(false, std::memory_order_release);
        try { adapter->Release(); }
        catch (...) { Record(Error(Code::CleanupFailed, phase, ExceptionCode())); }
        finished = true;
        error.cleanupPending = false;
        phase = Phase::Done;
    }

    void Poll(std::uint64_t now) noexcept
    {
        AsyncStatus status;
        try { status = adapter->Status(); }
        catch (...) {
            // Unknown status is not proof of completion. Retain and poll.
            Fail(Error(Code::OperationFailed, phase, ExceptionCode()), now);
            return;
        }
        if (status == AsyncStatus::Started) {
            if (now - started_ >= StepTimeoutMs) {
                Fail(Error(Code::Timeout, phase, HRESULT_FROM_WIN32(ERROR_TIMEOUT)), now);
                // Cleanup may already have been retired before this operation
                // began. Cancel that operation once when its deadline expires.
                CancelPending();
            }
            return;
        }
        if (status != AsyncStatus::Completed && status != AsyncStatus::Canceled && status != AsyncStatus::Error) {
            Fail(Error(Code::OperationFailed, phase, E_UNEXPECTED), now);
            return;
        }
        pending_ = false;
        bool success = false;
        PhaseResult result{};
        try {
            if (status == AsyncStatus::Completed) {
                result = adapter->Results(phase);
                success = true;
            } else if (!retired || phase == Phase::ReadRestore || phase == Phase::WriteRestore) {
                Fail(Error(retired ? Code::CleanupFailed : Code::OperationFailed, phase,
                    status == AsyncStatus::Canceled ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : adapter->OperationError()), now);
            }
        } catch (GattPaddleError const& failure) { Fail(failure, now); }
        catch (...) { Fail(Error(retired ? Code::CleanupFailed : Code::OperationFailed, phase, ExceptionCode()), now); }
        try { adapter->CloseOperation(); }
        catch (...) {
            const HRESULT hr = ExceptionCode();
            Record(Error(Code::CleanupFailed, phase, hr));
            if (!retired) { Fail(Error(Code::OperationFailed, phase, hr), now); }
            else { Retire(now); }
        }
        if (!success) {
            if (phase == Phase::WriteRestore) { restoreNeeded_ = false; }
            return;
        }
        if (phase == Phase::ReadRestore) {
            restoreChecked_ = true;
            restoreNeeded_ = result.cccd == Cccd::Notify;
            return;
        }
        if (phase == Phase::WriteRestore) {
            restoreNeeded_ = false;
            return;
        }
        if (retired) { return; }
        try {
            switch (phase) {
            case Phase::Discover: next_ = Phase::Open; break;
            case Phase::Open: next_ = Phase::Verify; break;
            case Phase::Verify: next_ = Phase::Services; break;
            case Phase::Services:
                next_ = result.emptyAttributes ? Phase::ServicesUncached : Phase::Characteristics;
                break;
            case Phase::ServicesUncached: next_ = Phase::Characteristics; break;
            case Phase::Characteristics:
                next_ = result.emptyAttributes ? Phase::CharacteristicsUncached : Phase::ReadOriginal;
                break;
            case Phase::CharacteristicsUncached: next_ = Phase::ReadOriginal; break;
            case Phase::ReadOriginal:
                original_ = result.cccd;
                // Preserve a preexisting indication subscription and reject
                // undefined/combined values instead of replacing their policy.
                if (original_ != Cccd::None && original_ != Cccd::Notify) {
                    Fail(Error(Code::Unsupported, phase), now);
                    break;
                }
                gate->active.store(true, std::memory_order_seq_cst);
                // A wrong-thread retirement request must never be undone by
                // activation racing on the owner. The requester also clears it.
                if (retireRequested_.load(std::memory_order_seq_cst)) {
                    Retire(now);
                    break;
                }
                adapter->Subscribe(gate);
                if (original_ == Cccd::Notify) { phase = Phase::Streaming; }
                else { next_ = Phase::WriteNotify; }
                break;
            case Phase::WriteNotify: phase = Phase::Streaming; break;
            default: Fail(Error(Code::OperationFailed, phase, E_UNEXPECTED), now); break;
            }
        } catch (...) { Fail(Error(Code::OperationFailed, phase, ExceptionCode()), now); }
    }
};

static bool OwnerMta(DWORD owner, GattPaddleError& error) noexcept
{
    if (GetCurrentThreadId() != owner) { error = Error(Code::WrongThread, Phase::Idle, RPC_E_WRONG_THREAD); return false; }
    APTTYPE type{};
    APTTYPEQUALIFIER qualifier{};
    HRESULT hr = CoGetApartmentType(&type, &qualifier);
    if (FAILED(hr) || type != APTTYPE_MTA) {
        error = Error(Code::WrongApartment, Phase::Idle, FAILED(hr) ? hr : RPC_E_CHANGED_MODE);
        return false;
    }
    return true;
}

} // namespace gatt_detail

struct GattPaddleClient::Impl {
    DWORD owner;
    gatt_detail::Machine machine;
    Impl(DWORD owner, std::unique_ptr<gatt_detail::Adapter> adapter, std::shared_ptr<gatt_detail::Gate> gate)
        : owner(owner), machine(std::move(adapter), std::move(gate)) {}
#ifdef SDL_XINPUT_PADDLE_GATT_TESTING
    static inline void (*beforePublication)(GattPaddleClient&, Impl&) = nullptr;
#endif

    // Raw slots intentionally have no process-exit destructors. An operation
    // with unknown terminal status must not be released during static teardown.
    static inline SRWLOCK orphanLock = SRWLOCK_INIT;
    static inline std::array<Impl*, gatt_detail::InstanceLimit> orphans{};
    static void Retain(Impl* state) noexcept
    {
        AcquireSRWLockExclusive(&orphanLock);
        for (auto& slot : orphans) {
            if (!slot) { slot = state; ReleaseSRWLockExclusive(&orphanLock); return; }
        }
        ReleaseSRWLockExclusive(&orphanLock);
        // Unreachable under the permit bound. Retaining is safer than deleting
        // state that Windows might still be using if the contract is violated.
    }
};

GattPaddleClient::GattPaddleClient(GUID container, std::uint64_t generation) noexcept
    : container_(container), generation_(generation), ownerThread_(GetCurrentThreadId()) {}

GattPaddleClient::~GattPaddleClient()
{
    Retire();
    published_.store(nullptr, std::memory_order_seq_cst);
    GattPaddleError ignored{};
    if (impl_ && (!gatt_detail::OwnerMta(ownerThread_, ignored) || !impl_->machine.finished)) {
        Impl::Retain(impl_.release());
    }
}

bool GattPaddleClient::Start(GattPaddleError& error) noexcept
{
    using namespace gatt_detail;
    error = {};
    if (!OwnerMta(ownerThread_, error)) { return false; }
    if (attempted_ || retired_.load(std::memory_order_seq_cst)) { error = Error(Code::AlreadyStarted, Phase::Idle); return false; }
    if (!ValidContainer(container_)) { error = Error(Code::InvalidIdentity, Phase::Idle, E_INVALIDARG); return false; }
    try {
        auto permit = AcquirePermit();
        if (!permit) { error = Error(Code::InstanceLimit, Phase::Idle); return false; }
        attempted_ = true;
        auto gate = std::make_shared<Gate>(generation_, std::move(permit));
        auto state = std::make_unique<Impl>(ownerThread_, std::make_unique<WindowsAdapter>(container_), std::move(gate));
#ifdef SDL_XINPUT_PADDLE_GATT_TESTING
        if (Impl::beforePublication) { Impl::beforePublication(*this, *state); }
#endif
        impl_ = std::move(state);
        published_.store(impl_.get(), std::memory_order_seq_cst);
        // A request made before publication saw no state to forward to. This
        // check closes that window before the first phase can perform I/O.
        // Both sides use one sequentially consistent order across the pointer
        // and the latch, so they cannot each miss the other side's store.
        if (retired_.load(std::memory_order_seq_cst)) { impl_->machine.RequestRetire(); }
        impl_->machine.Pump(GetTickCount64());
        error = impl_->machine.error;
        return !impl_->machine.retired;
    } catch (...) {
        error = Error(Code::OutOfMemory, Phase::Idle, ExceptionCode());
        return false;
    }
}

bool GattPaddleClient::Pump(std::vector<GattPaddlePacket>& packets, GattPaddleError& error) noexcept
{
    using namespace gatt_detail;
    error = {};
    if (!OwnerMta(ownerThread_, error)) { return false; }
    if (!impl_) { return false; }
    if (retired_.load(std::memory_order_seq_cst)) { impl_->machine.RequestRetire(); }
    impl_->machine.Pump(GetTickCount64());
    try { impl_->machine.Drain(packets); }
    catch (...) { error = Error(Code::OutOfMemory, impl_->machine.phase, ExceptionCode()); return false; }
    error = impl_->machine.error;
    return !impl_->machine.retired;
}

void GattPaddleClient::Retire() noexcept
{
    retired_.store(true, std::memory_order_seq_cst);
    if (auto* state = published_.load(std::memory_order_seq_cst)) {
        state->machine.RequestRetire();
        GattPaddleError ignored{};
        if (gatt_detail::OwnerMta(ownerThread_, ignored)) { state->machine.Retire(GetTickCount64()); }
    }
}

bool GattPaddleClient::Finished() const noexcept
{
    return GetCurrentThreadId() == ownerThread_ && (!impl_ || impl_->machine.finished);
}
std::uint64_t GattPaddleClient::BluetoothAddress() const noexcept
{
    return GetCurrentThreadId() == ownerThread_ && impl_ ? impl_->machine.adapter->Address() : 0;
}

unsigned GattPaddleClient::PumpRetired(GattPaddleError& error) noexcept
{
    error = {};
    const DWORD owner = GetCurrentThreadId();
    if (!gatt_detail::OwnerMta(owner, error)) { return 0; }
    std::array<Impl*, gatt_detail::InstanceLimit> owned{};
    unsigned count = 0;
    AcquireSRWLockExclusive(&Impl::orphanLock);
    for (auto& slot : Impl::orphans) {
        if (slot && slot->owner == owner) { owned[count++] = std::exchange(slot, nullptr); }
    }
    ReleaseSRWLockExclusive(&Impl::orphanLock);
    unsigned remaining = 0;
    for (unsigned i = 0; i < count; ++i) {
        auto state = owned[i];
        state->machine.Pump(GetTickCount64());
        if (state->machine.error.code != GattPaddleErrorCode::None ||
            state->machine.error.cleanupCode != GattPaddleErrorCode::None || state->machine.error.cleanupPending) {
            error = state->machine.error;
        }
        if (state->machine.finished) { delete state; }
        else { Impl::Retain(state); ++remaining; }
    }
    return remaining;
}

} // namespace sdl_paddles
