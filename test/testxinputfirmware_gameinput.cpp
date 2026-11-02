/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* The scripted GameInput of the XInput firmware test (hifihedgehog/SDL#44),
   and the real SDL_xinput_firmware.cpp, which this file includes after
   naming fakes for SDL_InitGameInput, SDL_QuitGameInput and
   WIN_GetDeviceInterfaceInstanceId. Its callbacks run one at a time on a
   thread of their own, as GameInput documents. RegisterDeviceCallback and
   UnregisterCallback wait for any running callback, as GameInput 4.0.302
   did on the bench. Every call the firmware lookup has no use for counts as
   unexpected: input, output, other device information, unregistering, and
   FindDeviceFromPlatformString. */

#include "testxinputfirmware.h"

#include "core/windows/SDL_windows.h"
#include "core/windows/SDL_gameinput.h"

extern bool Fake_InitGameInput(IGameInput **ppGameInput);
extern void Fake_QuitGameInput(void);
extern "C" bool Fake_GetDeviceInterfaceInstanceId(const WCHAR *path, WCHAR *instance, size_t capacity);

#define SDL_InitGameInput Fake_InitGameInput
#define SDL_QuitGameInput Fake_QuitGameInput
#define WIN_GetDeviceInterfaceInstanceId Fake_GetDeviceInterfaceInstanceId

#include "../src/joystick/windows/SDL_xinput_firmware.cpp"

namespace {

constexpr int FakeMaxDevices = 16;
constexpr int FakeMaxEvents = 64;
constexpr int FakeMaxInstances = 16;
constexpr GameInputCallbackToken FakeToken = 0x44;

SDL_AtomicInt s_unexpected;
SDL_AtomicInt s_instance_lookups;
SDL_AtomicInt s_platform_lookups;

void Unexpected()
{
    SDL_AddAtomicInt(&s_unexpected, 1);
}

class FakeDevice final : public IGameInputDevice
{
public:
    LONG refs = 1;
    LONG connected = 0;
    char path[512] = {};
    GameInputVersion firmware = {};

    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        Unexpected();
        if (ppv) {
            *ppv = nullptr;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    IFACEMETHODIMP_(ULONG) Release() override { return (ULONG)InterlockedDecrement(&refs); }

    // The device information the lookup reads
    IFACEMETHODIMP_(uint32_t) GetPnpPath(uint32_t bufferByteCount, char *buffer) override
    {
        const uint32_t bytes = (uint32_t)SDL_strlen(path) + 1;

        if (buffer && bufferByteCount >= bytes) {
            SDL_memcpy(buffer, path, bytes);
        }
        return bytes;
    }
    IFACEMETHODIMP_(void) GetFirmwareVersion(GameInputVersion *version) override { *version = firmware; }
    IFACEMETHODIMP_(bool) IsConnected() override { return InterlockedCompareExchange(&connected, 0, 0) != 0; }

    // Everything else
    IFACEMETHODIMP_(uint16_t) GetVendorId() override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint16_t) GetProductId() override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint16_t) GetRevisionNumber() override { Unexpected(); return 0; }
    IFACEMETHODIMP_(void) GetUsage(GameInputUsage *usage) override { Unexpected(); SDL_zerop(usage); }
    IFACEMETHODIMP_(void) GetHardwareVersion(GameInputVersion *version) override { Unexpected(); SDL_zerop(version); }
    IFACEMETHODIMP_(void) GetDeviceId(APP_LOCAL_DEVICE_ID *deviceId) override { Unexpected(); SDL_zerop(deviceId); }
    IFACEMETHODIMP_(void) GetDeviceRootId(APP_LOCAL_DEVICE_ID *deviceRootId) override { Unexpected(); SDL_zerop(deviceRootId); }
    IFACEMETHODIMP_(GameInputDeviceFamily) GetDeviceFamily() override { Unexpected(); return (GameInputDeviceFamily)0; }
    IFACEMETHODIMP_(GameInputKind) GetSupportedInput() override { Unexpected(); return GameInputKindUnknown; }
    IFACEMETHODIMP_(GameInputRumbleMotors) GetSupportedRumbleMotors() override { Unexpected(); return (GameInputRumbleMotors)0; }
    IFACEMETHODIMP_(GameInputSystemButtons) GetSupportedSystemButtons() override { Unexpected(); return (GameInputSystemButtons)0; }
    IFACEMETHODIMP_(GameInputLightingGroups) GetSupportedLightingGroups() override { Unexpected(); return (GameInputLightingGroups)0; }
    IFACEMETHODIMP_(uint32_t) GetSupportedLightingGroupSize(GameInputLightingGroups group) override { Unexpected(); return 0; }
    IFACEMETHODIMP_(void) GetContainerId(GUID *containerId) override { Unexpected(); SDL_zerop(containerId); }
    IFACEMETHODIMP_(uint32_t) GetDisplayName(uint32_t bufferByteCount, char *buffer) override { Unexpected(); return 0; }
    IFACEMETHODIMP_(bool) GetXInputDeviceIndex(uint32_t *index) override { Unexpected(); *index = 0; return false; }
    IFACEMETHODIMP_(bool) GetKeyboardInfo(GameInputKeyboardInfo *info) override { Unexpected(); return false; }
    IFACEMETHODIMP_(bool) GetMouseInfo(GameInputMouseInfo *info) override { Unexpected(); return false; }
    IFACEMETHODIMP_(bool) GetSensorsInfo(GameInputSensorsInfo *info) override { Unexpected(); return false; }
    IFACEMETHODIMP_(uint32_t) GetControllerAxisCount() override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint32_t) GetControllerAxisLabels(uint32_t labelArrayCount, GameInputLabel *labelArray) override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint32_t) GetControllerButtonCount() override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint32_t) GetControllerButtonLabels(uint32_t labelArrayCount, GameInputLabel *labelArray) override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint32_t) GetControllerSwitchCount() override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint32_t) GetControllerSwitchInfo(uint32_t infoArrayCount, GameInputControllerSwitchInfo *infoArray) override { Unexpected(); return 0; }
    IFACEMETHODIMP_(bool) GetArcadeStickInfo(GameInputArcadeStickInfo *info) override { Unexpected(); return false; }
    IFACEMETHODIMP_(bool) GetFlightStickInfo(GameInputFlightStickInfo *info) override { Unexpected(); return false; }
    IFACEMETHODIMP_(bool) GetGamepadInfo(GameInputGamepadInfo *info) override { Unexpected(); return false; }
    IFACEMETHODIMP_(bool) GetRacingWheelInfo(GameInputRacingWheelInfo *info) override { Unexpected(); return false; }
    IFACEMETHODIMP_(uint32_t) GetForceFeedbackMotorCount() override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint32_t) GetForceFeedbackMotorInfo(uint32_t infoArrayCount, GameInputForceFeedbackMotorInfo *infoArray) override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint32_t) GetInputReportCount() override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint32_t) GetInputReportInfo(uint32_t infoArrayCount, GameInputRawDeviceReportInfo *infoArray) override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint32_t) GetOutputReportCount() override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint32_t) GetOutputReportInfo(uint32_t infoArrayCount, GameInputRawDeviceReportInfo *infoArray) override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint32_t) GetFeatureReportCount() override { Unexpected(); return 0; }
    IFACEMETHODIMP_(uint32_t) GetFeatureReportInfo(uint32_t infoArrayCount, GameInputRawDeviceReportInfo *infoArray) override { Unexpected(); return 0; }
    IFACEMETHODIMP GetHapticInfo(GameInputHapticInfo *info) override { Unexpected(); return E_NOTIMPL; }
    IFACEMETHODIMP CreateForceFeedbackEffect(uint32_t motorIndex, const GameInputForceFeedbackParams *params, IGameInputForceFeedbackEffect **effect) override
    {
        Unexpected();
        *effect = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP_(bool) IsForceFeedbackMotorPoweredOn(uint32_t motorIndex) override { Unexpected(); return false; }
    IFACEMETHODIMP_(void) SetForceFeedbackMotorGain(uint32_t motorIndex, float masterGain) override { Unexpected(); }
    IFACEMETHODIMP_(void) SetRumbleState(const GameInputRumbleParams *params) override { Unexpected(); }
    IFACEMETHODIMP DirectInputEscape(uint32_t command, const void *bufferIn, uint32_t bufferInSize, void *bufferOut, uint32_t bufferOutSize, uint32_t *bufferOutSizeWritten) override
    {
        Unexpected();
        return E_NOTIMPL;
    }
    IFACEMETHODIMP CreateInputMapper(IGameInputMapper **inputMapper) override
    {
        Unexpected();
        *inputMapper = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetExtraAxisCount(GameInputKind inputKind, uint32_t *extraAxisCount) override { Unexpected(); return E_NOTIMPL; }
    IFACEMETHODIMP GetExtraButtonCount(GameInputKind inputKind, uint32_t *extraButtonCount) override { Unexpected(); return E_NOTIMPL; }
    IFACEMETHODIMP GetExtraAxisIndexes(GameInputKind inputKind, uint32_t extraAxisCount, uint8_t *extraAxisIndexes) override { Unexpected(); return E_NOTIMPL; }
    IFACEMETHODIMP GetExtraButtonIndexes(GameInputKind inputKind, uint32_t extraButtonCount, uint8_t *extraButtonIndexes) override { Unexpected(); return E_NOTIMPL; }
    IFACEMETHODIMP CreateRawDeviceReport(uint32_t reportId, GameInputRawDeviceReportKind reportKind, IGameInputRawDeviceReport **report) override
    {
        Unexpected();
        *report = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetRawDeviceFeature(uint32_t reportId, IGameInputRawDeviceReport **report) override
    {
        Unexpected();
        *report = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP SetRawDeviceFeature(IGameInputRawDeviceReport *report) override { Unexpected(); return E_NOTIMPL; }
    IFACEMETHODIMP SendRawDeviceOutput(IGameInputRawDeviceReport *report) override { Unexpected(); return E_NOTIMPL; }
    IFACEMETHODIMP_(void) SetLightingColor(GameInputColor color, GameInputLightingGroups group, uint32_t groupOrderIndex) override { Unexpected(); }
    IFACEMETHODIMP_(void) ApplyLightingColors() override { Unexpected(); }
    IFACEMETHODIMP_(void) ApplyLightingSystemDefault() override { Unexpected(); }
    IFACEMETHODIMP_(uint32_t) GetTouchSurfaceCount() override { Unexpected(); return 0; }
    IFACEMETHODIMP GetTouchSurfaceInfo(uint32_t surfaceIndex, GameInputTouchSurfaceInfo *info) override { Unexpected(); return E_NOTIMPL; }
};

class FakeGameInput final : public IGameInput
{
public:
    GameInputDeviceCallback callback = nullptr;
    void *context = nullptr;
    SDL_AtomicInt registered;
    bool register_fails = false;
    SDL_AtomicInt registrations;
    SDL_AtomicInt unregistrations;

    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        Unexpected();
        if (ppv) {
            *ppv = nullptr;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { Unexpected(); return 1; }
    IFACEMETHODIMP_(ULONG) Release() override { Unexpected(); return 1; }

    IFACEMETHODIMP RegisterDeviceCallback(IGameInputDevice *device, GameInputKind inputKind, void *callbackContext, GameInputDeviceCallback callbackFunc, GameInputCallbackToken *callbackToken) override;
    IFACEMETHODIMP_(bool) UnregisterCallback(GameInputCallbackToken callbackToken) override;

    IFACEMETHODIMP_(uint64_t) GetCurrentTimestamp() override { Unexpected(); return 0; }
    IFACEMETHODIMP GetCurrentReading(GameInputKind inputKind, IGameInputDevice *device, IGameInputReading **reading) override
    {
        Unexpected();
        *reading = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetNextReading(IGameInputReading *referenceReading, GameInputKind inputKind, IGameInputDevice *device, IGameInputReading **reading) override
    {
        Unexpected();
        *reading = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetPreviousReading(IGameInputReading *referenceReading, GameInputKind inputKind, IGameInputDevice *device, IGameInputReading **reading) override
    {
        Unexpected();
        *reading = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP RegisterReadingCallback(IGameInputDevice *device, GameInputKind inputKind, void *callbackContext, GameInputReadingCallback callbackFunc, GameInputCallbackToken *callbackToken) override
    {
        Unexpected();
        return E_NOTIMPL;
    }
    IFACEMETHODIMP CreateDispatcher(IGameInputDispatcher **dispatcher) override
    {
        Unexpected();
        *dispatcher = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP FindDeviceFromId(const APP_LOCAL_DEVICE_ID *value, IGameInputDevice **device) override
    {
        Unexpected();
        *device = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP FindDeviceFromPlatformString(LPCWSTR value, IGameInputDevice **device) override
    {
        SDL_AddAtomicInt(&s_platform_lookups, 1);
        Unexpected();
        *device = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP_(void) EnableBackgroundInput(bool enable) override { Unexpected(); }
    IFACEMETHODIMP CreateAggregateDevice(GameInputKind inputKind, APP_LOCAL_DEVICE_ID *deviceId) override { Unexpected(); return E_NOTIMPL; }
    IFACEMETHODIMP DisableAggregateDevice(const APP_LOCAL_DEVICE_ID *deviceId) override { Unexpected(); return E_NOTIMPL; }
};

struct FakeEvent
{
    int device;
    bool connect;
};

struct FakeInstance
{
    WCHAR path[512];
    WCHAR instance[256];
};

FakeGameInput s_gameinput;
FakeDevice s_devices[FakeMaxDevices];
int s_device_count;
FakeEvent s_events[FakeMaxEvents];
int s_event_count;
FakeInstance s_instances[FakeMaxInstances];
int s_instance_count;
bool s_available;
int s_inits;
int s_quits;
SDL_Mutex *s_dispatch; // Held while a callback runs, as GameInput runs one at a time
SDL_Thread *s_flood;
int s_flood_device;
int s_flood_count;
SDL_Thread *s_foreign;
SDL_AtomicInt s_foreign_holding;

void Widen(const char *text, WCHAR *out, size_t capacity)
{
    size_t i;

    for (i = 0; text[i] && i + 1 < capacity; ++i) {
        out[i] = (WCHAR)(unsigned char)text[i];
    }
    out[i] = L'\0';
}

void Dispatch(int device, bool connect)
{
    SDL_LockMutex(s_dispatch);
    if (SDL_GetAtomicInt(&s_gameinput.registered)) {
        InterlockedExchange(&s_devices[device].connected, connect ? 1 : 0);
        s_gameinput.callback(FakeToken, s_gameinput.context,
                             connect ? GameInputDeviceEventConnected : GameInputDeviceEventDisconnected,
                             &s_devices[device]);
    }
    SDL_UnlockMutex(s_dispatch);
}

int SDLCALL DeliverThread(void *unused)
{
    int i;

    (void)unused;
    for (i = 0; i < s_event_count; ++i) {
        Dispatch(s_events[i].device, s_events[i].connect);
    }
    s_event_count = 0;
    return 0;
}

int SDLCALL FloodThread(void *unused)
{
    int i;

    (void)unused;
    for (i = 0; i < s_flood_count; ++i) {
        Dispatch(s_flood_device, (i % 2) != 0);
    }
    return 0;
}

// As GameInput runs SDL's GameInput driver's device callback, which takes the joystick lock
int SDLCALL ForeignThread(void *unused)
{
    (void)unused;
    SDL_LockMutex(s_dispatch);
    SDL_SetAtomicInt(&s_foreign_holding, 1);
    SDL_LockJoysticks();
    SDL_UnlockJoysticks();
    SDL_UnlockMutex(s_dispatch);
    return 0;
}

void QueueEvent(int device, bool connect)
{
    if (s_event_count < FakeMaxEvents) {
        s_events[s_event_count].device = device;
        s_events[s_event_count].connect = connect;
        ++s_event_count;
    } else {
        Unexpected();
    }
}

} // namespace

HRESULT STDMETHODCALLTYPE FakeGameInput::RegisterDeviceCallback(IGameInputDevice *device, GameInputKind inputKind, void *callbackContext, GameInputDeviceCallback callbackFunc, GameInputCallbackToken *callbackToken)
{
    HRESULT result = S_OK;

    if (callbackToken) {
        *callbackToken = 0;
    }
    if (device || !(inputKind & GameInputKindController) || !callbackFunc || !callbackToken) {
        Unexpected();
        return E_INVALIDARG;
    }
    // Waits for any running callback
    SDL_LockMutex(s_dispatch);
    if (register_fails || SDL_GetAtomicInt(&registered)) {
        result = E_FAIL;
    } else {
        callback = callbackFunc;
        context = callbackContext;
        *callbackToken = FakeToken;
        SDL_SetAtomicInt(&registered, 1);
    }
    SDL_AddAtomicInt(&registrations, 1);
    SDL_UnlockMutex(s_dispatch);
    return result;
}

bool STDMETHODCALLTYPE FakeGameInput::UnregisterCallback(GameInputCallbackToken callbackToken)
{
    bool result = false;

    SDL_AddAtomicInt(&unregistrations, 1);
    Unexpected();
    // Waits for any running callback
    SDL_LockMutex(s_dispatch);
    if (SDL_GetAtomicInt(&registered) && callbackToken == FakeToken) {
        SDL_SetAtomicInt(&registered, 0);
        callback = nullptr;
        result = true;
    }
    SDL_UnlockMutex(s_dispatch);
    return result;
}

bool Fake_InitGameInput(IGameInput **ppGameInput)
{
    if (!s_available) {
        return SDL_SetError("The scripted GameInput is unavailable");
    }
    ++s_inits;
    *ppGameInput = &s_gameinput;
    return true;
}

void Fake_QuitGameInput(void)
{
    ++s_quits;
}

extern "C" bool Fake_GetDeviceInterfaceInstanceId(const WCHAR *path, WCHAR *instance, size_t capacity)
{
    int i;

    SDL_AddAtomicInt(&s_instance_lookups, 1);
    instance[0] = L'\0';
    for (i = 0; i < s_instance_count; ++i) {
        if (SDL_wcscmp(s_instances[i].path, path) == 0) {
            if (SDL_wcslen(s_instances[i].instance) + 1 > capacity) {
                return false;
            }
            SDL_wcslcpy(instance, s_instances[i].instance, capacity);
            return true;
        }
    }
    return false;
}

extern "C" {

void FakeGI_Reset(bool available, bool register_fails)
{
    int i;

    if (!s_dispatch) {
        s_dispatch = SDL_CreateMutex();
    }
    // A registration lasts until unregistered, as in GameInput
    s_gameinput.register_fails = register_fails;
    SDL_SetAtomicInt(&s_gameinput.registrations, 0);
    SDL_SetAtomicInt(&s_gameinput.unregistrations, 0);
    SDL_SetAtomicInt(&s_unexpected, 0);
    for (i = 0; i < FakeMaxDevices; ++i) {
        if (s_devices[i].refs != 1) {
            // A device the last scenario left listed
            Unexpected();
        }
        s_devices[i].refs = 1;
        s_devices[i].connected = 0;
        s_devices[i].path[0] = '\0';
        SDL_zero(s_devices[i].firmware);
    }
    s_device_count = 0;
    s_event_count = 0;
    s_instance_count = 0;
    s_available = available;
    s_inits = 0;
    s_quits = 0;
    SDL_SetAtomicInt(&s_instance_lookups, 0);
    SDL_SetAtomicInt(&s_platform_lookups, 0);
}

bool FakeGI_WaitRegistered(int milliseconds)
{
    while (!SDL_GetAtomicInt(&s_gameinput.registered) && milliseconds-- > 0) {
        SDL_Delay(1);
    }
    return SDL_GetAtomicInt(&s_gameinput.registered) != 0;
}

bool FakeGI_WaitRegistrations(int count, int milliseconds)
{
    while (SDL_GetAtomicInt(&s_gameinput.registrations) < count && milliseconds-- > 0) {
        SDL_Delay(1);
    }
    return SDL_GetAtomicInt(&s_gameinput.registrations) >= count;
}

bool FakeGI_WaitModuleSettled(int milliseconds)
{
    while (SDL_GetAtomicInt(&s_firmware_registration) == FIRMWARE_REGISTERING && milliseconds-- > 0) {
        SDL_Delay(1);
    }
    return SDL_GetAtomicInt(&s_firmware_registration) != FIRMWARE_REGISTERING;
}

void FakeGI_StartForeign(void)
{
    SDL_SetAtomicInt(&s_foreign_holding, 0);
    s_foreign = SDL_CreateThread(ForeignThread, "FakeGameInputForeign", NULL);
    if (!s_foreign) {
        Unexpected();
        return;
    }
    while (!SDL_GetAtomicInt(&s_foreign_holding)) {
        SDL_Delay(1);
    }
}

void FakeGI_WaitForeign(void)
{
    if (s_foreign) {
        SDL_WaitThread(s_foreign, NULL);
        s_foreign = NULL;
    }
}

int FakeGI_AddDevice(const char *path, Uint16 major, Uint16 minor, Uint16 build, Uint16 revision)
{
    FakeDevice *device;

    if (s_device_count == FakeMaxDevices) {
        Unexpected();
        return 0;
    }
    device = &s_devices[s_device_count];
    SDL_strlcpy(device->path, path, sizeof(device->path));
    device->firmware.major = major;
    device->firmware.minor = minor;
    device->firmware.build = build;
    device->firmware.revision = revision;
    QueueEvent(s_device_count, true);
    return s_device_count++;
}

void FakeGI_Disconnect(int device)
{
    QueueEvent(device, false);
}

void FakeGI_Deliver(void)
{
    SDL_Thread *thread = SDL_CreateThread(DeliverThread, "FakeGameInput", NULL);

    if (thread) {
        SDL_WaitThread(thread, NULL);
    } else {
        Unexpected();
    }
}

void FakeGI_StartFlood(int device, int count)
{
    s_flood_device = device;
    s_flood_count = count;
    s_flood = SDL_CreateThread(FloodThread, "FakeGameInputFlood", NULL);
    if (!s_flood) {
        Unexpected();
    }
}

void FakeGI_WaitFlood(void)
{
    if (s_flood) {
        SDL_WaitThread(s_flood, NULL);
        s_flood = NULL;
    }
}

void FakeGI_SetInstance(const char *path, const char *instance)
{
    if (s_instance_count == FakeMaxInstances) {
        Unexpected();
        return;
    }
    Widen(path, s_instances[s_instance_count].path, SDL_arraysize(s_instances[s_instance_count].path));
    Widen(instance, s_instances[s_instance_count].instance, SDL_arraysize(s_instances[s_instance_count].instance));
    ++s_instance_count;
}

int FakeGI_Inits(void)
{
    return s_inits;
}

int FakeGI_Quits(void)
{
    return s_quits;
}

int FakeGI_Registrations(void)
{
    return SDL_GetAtomicInt(&s_gameinput.registrations);
}

int FakeGI_Unregistrations(void)
{
    return SDL_GetAtomicInt(&s_gameinput.unregistrations);
}

bool FakeGI_Registered(void)
{
    return SDL_GetAtomicInt(&s_gameinput.registered) != 0;
}

long FakeGI_References(int device)
{
    return InterlockedCompareExchange(&s_devices[device].refs, 0, 0) - 1;
}

int FakeGI_Unexpected(void)
{
    return SDL_GetAtomicInt(&s_unexpected);
}

int FakeGI_InstanceLookups(void)
{
    return SDL_GetAtomicInt(&s_instance_lookups);
}

int FakeGI_PlatformStringLookups(void)
{
    return SDL_GetAtomicInt(&s_platform_lookups);
}

} // extern "C"
