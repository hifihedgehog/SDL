/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL's GameInput joystick driver against a scripted GameInput, for
   hifihedgehog/SDL#45. SDL_QuitJoysticks holds the joystick lock across each
   driver's quit, and GameInput 4.0.302 makes UnregisterCallback wait for a
   running callback, as the hifihedgehog/SDL#44 bench measured. The driver's
   device callback therefore queues each event under a lock of its own, and
   the driver's detect applies the queue under the joystick lock.

   The driver starts, detects and quits under the joystick lock, as
   SDL_InitJoysticks, SDL_UpdateJoysticks and SDL_QuitJoysticks call it. SDL's
   joystick subsystem never starts, so no other driver runs and no device
   opens. The scripted devices are the raw-report instruments PadForge's
   build lists by default: GameInput's gamepads stay off and raw reports on.

   1. A quit while a device callback runs: the quit returns, and the callback
      finishes inside UnregisterCallback.
   2. Callbacks delivered while another thread holds the joystick lock finish
      without it.
   3. Devices connected before the start, connected and removed while the
      driver runs, removed and connected again before a detect, connected and
      removed before a detect, and gone without a callback: each detect adds
      and removes the same joysticks as the driver did before the change.
   4. A registration failure at the start: no hang, and GameInput released.
   5. Devices reported during a quit, or reported and never detected, are
      dropped with their references.
   6. A flood of callbacks while detects run.

   --hotplug runs only the scenarios the earlier driver also completes (3, 4,
   the second half of 5, and 6), so the same checks run against both. */

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"

#include <stdio.h>

#include "joystick/SDL_sysjoystick.h"
#include "core/windows/SDL_windows.h"
#include "core/windows/SDL_gameinput.h"

/* The names through which the driver would start or quit GameInput, change
   SDL's joystick list, or ask the other drivers */
extern "C" {
void Fake_JoystickAdded(SDL_JoystickID instance_id);
void Fake_JoystickRemoved(SDL_JoystickID instance_id);
bool Fake_HandledByAnotherDriver(struct SDL_JoystickDriver *driver, Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name);
}
bool Fake_InitGameInput(IGameInput **ppGameInput);
void Fake_QuitGameInput(void);

#define SDL_InitGameInput Fake_InitGameInput
#define SDL_QuitGameInput Fake_QuitGameInput
#define SDL_PrivateJoystickAdded Fake_JoystickAdded
#define SDL_PrivateJoystickRemoved Fake_JoystickRemoved
#define SDL_JoystickHandledByAnotherDriver Fake_HandledByAnotherDriver

#include "../src/joystick/gdk/SDL_gameinputjoystick.cpp"

static int gi_checks;
static int gi_failures;
static const char *gi_scenario = "";

#define CHECK(condition, ...)                                        \
    do {                                                             \
        ++gi_checks;                                                 \
        if (!(condition)) {                                          \
            ++gi_failures;                                           \
            printf("FAILED line %d (%s): ", __LINE__, gi_scenario);  \
            printf(__VA_ARGS__);                                     \
            printf("\n");                                            \
        }                                                            \
    } while (0)

namespace {

constexpr int FakeMaxDevices = 8;
constexpr int FakeMaxEvents = 64;
constexpr GameInputCallbackToken FakeToken = 0x45;
constexpr Sint32 GateMilliseconds = 30000;

SDL_AtomicInt s_unexpected;

void Unexpected()
{
    SDL_AddAtomicInt(&s_unexpected, 1);
}

// Set on the thread that runs a callback, for as long as it runs
thread_local bool t_dispatching = false;

SDL_Semaphore *s_entered;  // A held callback has started
SDL_Semaphore *s_release;  // Lets a held callback go on
SDL_AtomicInt s_held;      // A callback waits on s_release
SDL_AtomicInt s_release_timed_out;
SDL_AtomicInt s_callback_running;
SDL_AtomicInt s_unregister_saw_running;

class FakeDevice final : public IGameInputDevice
{
public:
    LONG refs = 1;
    LONG connected = 0;
    Uint16 vendor = 0;
    Uint16 product = 0;
    char name[64] = {};
    char path[128] = {};
    Uint8 id = 0;
    bool hold = false; // The next callback for this device waits inside IsConnected

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

    // The device information the driver reads to list a device
    IFACEMETHODIMP_(uint16_t) GetVendorId() override { return vendor; }
    IFACEMETHODIMP_(uint16_t) GetProductId() override { return product; }
    IFACEMETHODIMP_(GameInputKind) GetSupportedInput() override { return GameInputKindRawDeviceReport; }
    IFACEMETHODIMP_(uint32_t) GetDisplayName(uint32_t bufferByteCount, char *buffer) override
    {
        const uint32_t bytes = (uint32_t)SDL_strlen(name) + 1;

        if (buffer && bufferByteCount >= bytes) {
            SDL_memcpy(buffer, name, bytes);
        }
        return bytes;
    }
    IFACEMETHODIMP_(void) GetDeviceId(APP_LOCAL_DEVICE_ID *deviceId) override
    {
        SDL_zerop(deviceId);
        deviceId->value[0] = id;
    }
    IFACEMETHODIMP_(uint32_t) GetPnpPath(uint32_t bufferByteCount, char *buffer) override
    {
        const uint32_t bytes = (uint32_t)SDL_strlen(path) + 1;

        if (buffer && bufferByteCount >= bytes) {
            SDL_memcpy(buffer, path, bytes);
        }
        return bytes;
    }
    IFACEMETHODIMP_(uint32_t) GetForceFeedbackMotorCount() override { return 0; }
    IFACEMETHODIMP_(bool) IsConnected() override
    {
        if (t_dispatching && hold) {
            hold = false;
            SDL_SetAtomicInt(&s_held, 1);
            SDL_SignalSemaphore(s_entered);
            if (!SDL_WaitSemaphoreTimeout(s_release, GateMilliseconds)) {
                SDL_SetAtomicInt(&s_release_timed_out, 1);
            }
            SDL_SetAtomicInt(&s_held, 0);
        }
        return InterlockedCompareExchange(&connected, 0, 0) != 0;
    }

    // Everything else
    IFACEMETHODIMP_(uint16_t) GetRevisionNumber() override { Unexpected(); return 0; }
    IFACEMETHODIMP_(void) GetUsage(GameInputUsage *usage) override { Unexpected(); SDL_zerop(usage); }
    IFACEMETHODIMP_(void) GetHardwareVersion(GameInputVersion *version) override { Unexpected(); SDL_zerop(version); }
    IFACEMETHODIMP_(void) GetFirmwareVersion(GameInputVersion *version) override { Unexpected(); SDL_zerop(version); }
    IFACEMETHODIMP_(void) GetDeviceRootId(APP_LOCAL_DEVICE_ID *deviceRootId) override { Unexpected(); SDL_zerop(deviceRootId); }
    IFACEMETHODIMP_(GameInputDeviceFamily) GetDeviceFamily() override { Unexpected(); return (GameInputDeviceFamily)0; }
    IFACEMETHODIMP_(GameInputRumbleMotors) GetSupportedRumbleMotors() override { Unexpected(); return (GameInputRumbleMotors)0; }
    IFACEMETHODIMP_(GameInputSystemButtons) GetSupportedSystemButtons() override { Unexpected(); return (GameInputSystemButtons)0; }
    IFACEMETHODIMP_(GameInputLightingGroups) GetSupportedLightingGroups() override { Unexpected(); return (GameInputLightingGroups)0; }
    IFACEMETHODIMP_(uint32_t) GetSupportedLightingGroupSize(GameInputLightingGroups group) override { Unexpected(); return 0; }
    IFACEMETHODIMP_(void) GetContainerId(GUID *containerId) override { Unexpected(); SDL_zerop(containerId); }
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
    IFACEMETHODIMP_(uint64_t) GetCurrentTimestamp() override { return 0; }
    IFACEMETHODIMP_(void) EnableBackgroundInput(bool enable) override {}

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
        Unexpected();
        *device = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP CreateAggregateDevice(GameInputKind inputKind, APP_LOCAL_DEVICE_ID *deviceId) override { Unexpected(); return E_NOTIMPL; }
    IFACEMETHODIMP DisableAggregateDevice(const APP_LOCAL_DEVICE_ID *deviceId) override { Unexpected(); return E_NOTIMPL; }
};

struct FakeEvent
{
    int device;
    bool connect;
};

FakeGameInput s_gameinput;
FakeDevice s_devices[FakeMaxDevices];
int s_device_count;
FakeEvent s_events[FakeMaxEvents];
int s_event_count;
bool s_available;
int s_inits;
int s_quits;
SDL_Mutex *s_dispatch; // Held while a callback runs, as GameInput runs one at a time
SDL_Thread *s_async;
SDL_Semaphore *s_async_done;
SDL_Thread *s_flood;
int s_flood_device;
int s_flood_count;
char s_log[512];

// Runs one callback on the calling thread, as GameInput's dispatch thread does
void Dispatch(int device, bool connect)
{
    SDL_LockMutex(s_dispatch);
    if (SDL_GetAtomicInt(&s_gameinput.registered)) {
        InterlockedExchange(&s_devices[device].connected, connect ? 1 : 0);
        SDL_SetAtomicInt(&s_callback_running, 1);
        t_dispatching = true;
        s_gameinput.callback(FakeToken, s_gameinput.context,
                             connect ? GameInputDeviceEventConnected : GameInputDeviceEventDisconnected,
                             &s_devices[device]);
        t_dispatching = false;
        SDL_SetAtomicInt(&s_callback_running, 0);
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
    SDL_SignalSemaphore(s_async_done);
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
    if (device || !(inputKind & GameInputKindRawDeviceReport) || !callbackFunc || !callbackToken) {
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
    if (SDL_GetAtomicInt(&s_callback_running)) {
        SDL_SetAtomicInt(&s_unregister_saw_running, 1);
    }
    if (SDL_GetAtomicInt(&s_held)) {
        SDL_SignalSemaphore(s_release);
    }
    // Waits for any running callback, as GameInput 4.0.302 did on the bench
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

static char Letter(IGameInputDevice *device)
{
    const ptrdiff_t index = static_cast<FakeDevice *>(device) - s_devices;

    return (index >= 0 && index < FakeMaxDevices) ? (char)('A' + index) : '?';
}

static char LetterOf(SDL_JoystickID instance_id)
{
    int i;

    for (i = 0; i < g_GameInputList.count; ++i) {
        if (g_GameInputList.devices[i] && g_GameInputList.devices[i]->device_instance == instance_id) {
            return Letter(g_GameInputList.devices[i]->device);
        }
    }
    return '?';
}

extern "C" void Fake_JoystickAdded(SDL_JoystickID instance_id)
{
    char entry[4] = { '+', LetterOf(instance_id), ' ', '\0' };

    SDL_strlcat(s_log, entry, sizeof(s_log));
}

extern "C" void Fake_JoystickRemoved(SDL_JoystickID instance_id)
{
    char entry[4] = { '-', LetterOf(instance_id), ' ', '\0' };

    SDL_strlcat(s_log, entry, sizeof(s_log));
}

extern "C" bool Fake_HandledByAnotherDriver(struct SDL_JoystickDriver *driver, Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    return false;
}

static void Begin(const char *name, bool available, bool register_fails)
{
    int i;

    gi_scenario = name;
    if (!s_dispatch) {
        s_dispatch = SDL_CreateMutex();
        s_entered = SDL_CreateSemaphore(0);
        s_release = SDL_CreateSemaphore(0);
        s_async_done = SDL_CreateSemaphore(0);
    }
    s_gameinput.register_fails = register_fails;
    SDL_SetAtomicInt(&s_gameinput.registrations, 0);
    SDL_SetAtomicInt(&s_gameinput.unregistrations, 0);
    SDL_SetAtomicInt(&s_unexpected, 0);
    SDL_SetAtomicInt(&s_release_timed_out, 0);
    SDL_SetAtomicInt(&s_unregister_saw_running, 0);
    for (i = 0; i < FakeMaxDevices; ++i) {
        s_devices[i].refs = 1;
        s_devices[i].connected = 0;
        s_devices[i].hold = false;
    }
    s_device_count = 0;
    s_event_count = 0;
    s_available = available;
    s_inits = 0;
    s_quits = 0;
    s_log[0] = '\0';
}

/* A Mad Catz Stratocaster or a PDP drum kit, both in the raw instrument table
   (SDL_gameinput_rawtype.h). Queues its connection. */
static int AddDevice(bool drums)
{
    FakeDevice *device;
    const int index = s_device_count;

    if (index == FakeMaxDevices) {
        Unexpected();
        return 0;
    }
    device = &s_devices[index];
    device->vendor = drums ? USB_VENDOR_PDP : USB_VENDOR_MADCATZ;
    device->product = drums ? USB_PRODUCT_PDP_XB1_DRUM_KIT : USB_PRODUCT_MADCATZ_XB1_STRATOCASTER_GUITAR;
    SDL_snprintf(device->name, sizeof(device->name), "Instrument %c", 'A' + index);
    SDL_snprintf(device->path, sizeof(device->path), "\\\\?\\GIP#VID_%04X&PID_%04X#%d", device->vendor, device->product, index);
    device->id = (Uint8)(index + 1);
    QueueEvent(index, true);
    return s_device_count++;
}

static void Connect(int device)
{
    QueueEvent(device, true);
}

static void Disconnect(int device)
{
    QueueEvent(device, false);
}

// Runs the queued callbacks on a thread of their own and waits for them
static void Deliver(void)
{
    SDL_Thread *thread = SDL_CreateThread(DeliverThread, "FakeGameInput", NULL);

    if (thread) {
        SDL_WaitThread(thread, NULL);
        SDL_WaitSemaphore(s_async_done);
    } else {
        Unexpected();
    }
}

// Starts the queued callbacks on a thread of their own and returns
static void DeliverAsync(void)
{
    s_async = SDL_CreateThread(DeliverThread, "FakeGameInputAsync", NULL);
    if (!s_async) {
        Unexpected();
    }
}

// False when the callbacks have not finished within the time given
static bool FinishAsync(Sint32 milliseconds)
{
    const bool done = SDL_WaitSemaphoreTimeout(s_async_done, milliseconds);

    if (done && s_async) {
        SDL_WaitThread(s_async, NULL);
        s_async = NULL;
    }
    return done;
}

static bool Start(void)
{
    bool result;

    SDL_LockJoysticks();
    result = SDL_GAMEINPUT_JoystickDriver.Init();
    SDL_UnlockJoysticks();
    return result;
}

static void Detect(void)
{
    SDL_LockJoysticks();
    SDL_GAMEINPUT_JoystickDriver.Detect();
    SDL_UnlockJoysticks();
}

// As SDL_QuitJoysticks calls it, under the joystick lock
static void Stop(void)
{
    SDL_LockJoysticks();
    SDL_GAMEINPUT_JoystickDriver.Quit();
    SDL_UnlockJoysticks();
}

// The joysticks the detect added and removed, then the driver's list
static void Expect(const char *log, const char *listed)
{
    char list[16];
    int count;
    int i;

    SDL_LockJoysticks();
    count = SDL_GAMEINPUT_JoystickDriver.GetCount();
    for (i = 0; i < count && i + 1 < (int)sizeof(list); ++i) {
        list[i] = Letter(g_GameInputList.devices[i]->device);
        CHECK(list[i] != '?' && SDL_strcmp(SDL_GAMEINPUT_JoystickDriver.GetDeviceName(i), s_devices[list[i] - 'A'].name) == 0,
              "joystick %d was named %s", i, SDL_GAMEINPUT_JoystickDriver.GetDeviceName(i));
    }
    list[i] = '\0';
    SDL_UnlockJoysticks();
    CHECK(SDL_strcmp(s_log, log) == 0, "the detect logged \"%s\", expected \"%s\"", s_log, log);
    CHECK(SDL_strcmp(list, listed) == 0, "the driver listed \"%s\", expected \"%s\"", list, listed);
    s_log[0] = '\0';
}

static void Finish(void)
{
    int i;

    for (i = 0; i < s_device_count; ++i) {
        CHECK(s_devices[i].refs == 1, "device %c kept %ld references", 'A' + i, s_devices[i].refs - 1);
    }
    CHECK(!SDL_GetAtomicInt(&s_gameinput.registered), "the callback stayed registered");
    CHECK(s_quits == s_inits, "GameInput was started %d times and quit %d times", s_inits, s_quits);
    CHECK(SDL_GetAtomicInt(&s_unexpected) == 0, "%d calls the driver has no use for", SDL_GetAtomicInt(&s_unexpected));
}

static void TestHotplug(void)
{
    int a, b, c, d;

    Begin("devices connected before the start", true, false);
    a = AddDevice(false);
    b = AddDevice(true);
    CHECK(Start(), "the driver did not start: %s", SDL_GetError());
    CHECK(SDL_GetAtomicInt(&s_gameinput.registrations) == 1, "%d registrations", SDL_GetAtomicInt(&s_gameinput.registrations));
    // GameInput 4.0.302 lists the devices already connected after the registration returns
    Expect("", "");
    Deliver();
    Detect();
    Expect("+A +B ", "AB");

    gi_scenario = "a device connected and one removed while the driver runs";
    c = AddDevice(false);
    Deliver();
    Detect();
    Expect("+C ", "ABC");
    Disconnect(a);
    Deliver();
    Detect();
    Expect("-A ", "BC");
    CHECK(s_devices[a].refs == 1, "the removed device kept %ld references", s_devices[a].refs - 1);

    gi_scenario = "a device removed and connected again before a detect";
    Disconnect(b);
    Connect(b);
    Deliver();
    Detect();
    Expect("", "BC");

    gi_scenario = "a device connected and removed before a detect";
    d = AddDevice(true);
    Disconnect(d);
    Deliver();
    Detect();
    Expect("+D -D ", "BC");

    gi_scenario = "a device gone without a callback";
    InterlockedExchange(&s_devices[c].connected, 0);
    Detect();
    Expect("-C ", "B");

    gi_scenario = "a device connected again after its removal";
    Connect(a);
    Deliver();
    Detect();
    Expect("+A ", "BA");

    Stop();
    Expect("", "");
    Finish();
}

static void TestRegistrationFailure(void)
{
    Begin("a registration failure at the start", true, true);
    (void)AddDevice(false);
    CHECK(!Start(), "the driver started with its callback refused");
    CHECK(s_inits == 1 && s_quits == 1, "GameInput was started %d times and quit %d times", s_inits, s_quits);
    // SDL_QuitJoysticks still calls the quit
    Stop();
    CHECK(s_quits == 1, "the second quit released GameInput again");
    Finish();
    s_event_count = 0;

    Begin("GameInput missing", false, false);
    CHECK(!Start(), "the driver started without GameInput");
    Stop();
    Finish();
}

static void TestQuitDropsQueue(void)
{
    int a;

    Begin("a device reported and never detected", true, false);
    CHECK(Start(), "the driver did not start: %s", SDL_GetError());
    a = AddDevice(false);
    Deliver();
    Stop();
    CHECK(s_devices[a].refs == 1, "the device kept %ld references", s_devices[a].refs - 1);
    Expect("", "");
    Finish();
}

static void TestFlood(void)
{
    int a, b;
    int i;

    Begin("a flood of callbacks while detects run", true, false);
    a = AddDevice(false);
    b = AddDevice(true);
    CHECK(Start(), "the driver did not start: %s", SDL_GetError());
    Deliver();
    Detect();
    Expect("+A +B ", "AB");
    // 2000 events for B, alternating removal and connection, ending connected
    s_flood_device = b;
    s_flood_count = 2000;
    s_flood = SDL_CreateThread(FloodThread, "FakeGameInputFlood", NULL);
    CHECK(s_flood != NULL, "the flood did not start");
    for (i = 0; i < 500; ++i) {
        Detect();
    }
    SDL_WaitThread(s_flood, NULL);
    s_flood = NULL;
    Detect();
    // B may have left and come back during the flood. A never left.
    s_log[0] = '\0';
    Expect("", "AB");
    CHECK(s_devices[a].refs == 2 && s_devices[b].refs == 2, "the listed devices hold %ld and %ld references", s_devices[a].refs - 1, s_devices[b].refs - 1);
    Stop();
    Finish();
}

static void TestQuitDuringCallback(void)
{
    int b;

    Begin("a quit while a device callback runs", true, false);
    (void)AddDevice(false);
    CHECK(Start(), "the driver did not start: %s", SDL_GetError());
    Deliver();
    Detect();
    Expect("+A ", "A");

    // SDL_QuitJoysticks takes the joystick lock before it quits the drivers
    b = AddDevice(true);
    s_devices[b].hold = true;
    SDL_LockJoysticks();
    DeliverAsync();
    CHECK(SDL_WaitSemaphoreTimeout(s_entered, GateMilliseconds), "the callback never started");
    SDL_GAMEINPUT_JoystickDriver.Quit();
    SDL_UnlockJoysticks();
    CHECK(FinishAsync(GateMilliseconds), "the callback never finished");
    CHECK(SDL_GetAtomicInt(&s_unregister_saw_running) == 1, "UnregisterCallback found no callback running");
    CHECK(SDL_GetAtomicInt(&s_release_timed_out) == 0, "the callback went on only after its wait timed out");
    CHECK(SDL_GetAtomicInt(&s_gameinput.unregistrations) == 1, "%d unregistrations", SDL_GetAtomicInt(&s_gameinput.unregistrations));
    CHECK(s_devices[b].refs == 1, "the device reported during the quit kept %ld references", s_devices[b].refs - 1);
    Expect("", "");
    Finish();

    gi_scenario = "a start after a quit during a callback";
    s_device_count = 0;
    s_event_count = 0;
    (void)AddDevice(false);
    CHECK(Start(), "the driver did not start again: %s", SDL_GetError());
    Deliver();
    Detect();
    Expect("+A ", "A");
    Stop();
    Finish();
}

static void TestCallbacksUnderLock(void)
{
    Begin("callbacks while another thread holds the joystick lock", true, false);
    CHECK(Start(), "the driver did not start: %s", SDL_GetError());
    (void)AddDevice(false);
    (void)AddDevice(true);
    SDL_LockJoysticks();
    DeliverAsync();
    CHECK(FinishAsync(GateMilliseconds), "the callbacks waited on the joystick lock");
    SDL_UnlockJoysticks();
    // Waits for them here when they did wait on the lock
    if (s_async) {
        SDL_WaitThread(s_async, NULL);
        s_async = NULL;
        SDL_WaitSemaphore(s_async_done);
    }
    Detect();
    Expect("+A +B ", "AB");
    Stop();
    Finish();
}

int main(int argc, char *argv[])
{
    const bool hotplug_only = (argc > 1 && SDL_strcmp(argv[1], "--hotplug") == 0);

    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    // PadForge's defaults in this build: GameInput's gamepads off, raw reports on
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT_RAW, "1");

    TestHotplug();
    TestRegistrationFailure();
    TestQuitDropsQueue();
    TestFlood();
    if (!hotplug_only) {
        TestQuitDuringCallback();
        TestCallbacksUnderLock();
    }

    SDL_Quit();
    if (gi_failures) {
        printf("testgameinputdriver FAILED: %d of %d checks\n", gi_failures, gi_checks);
        return 1;
    }
    printf("testgameinputdriver PASSED: %d checks, 0 failures\n", gi_checks);
    return 0;
}
