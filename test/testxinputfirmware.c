/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL's XInput driver and its firmware lookup against scripted XInput
   slots and a scripted GameInput, for hifihedgehog/SDL#44. XInput has no
   firmware query, so the driver finds the slot's device in GameInput's
   device list, by the path OpenXInput gives for the slot or by the device
   instance both paths belong to, and reports GameInput's version as
   (major << 8) | minor.

   GameInput 4.0.302 made RegisterDeviceCallback and UnregisterCallback wait
   for any running callback on the bench, and the XInput driver starts and
   quits under the joystick lock, which a callback can wait on, as SDL's
   GameInput driver's did before hifihedgehog/SDL#45. So the lookup
   registers from a thread of its own and never unregisters, and its device
   list lasts for the process. The scenarios run in that order: GameInput
   missing, then refusing the callback, then present while another client's
   callback holds GameInput's dispatch.

   The resolver behind SDL_XINPUT_ReadSlotRecord gives the slot's device
   container, which matches the only listed device in it. A slot GameInput
   gives no version for reports 0: its metadata's product version is no
   firmware version (hifihedgehog/SDL#46). The joystick carries GameInput's
   whole version as the string property SDL.joystick.xinput.firmware.

   1. A slot whose path GameInput lists with 5.17.3202.0 reports 0x0511.
   2. A path that differs only in case, and a different path naming the
      same device instance, match. Neither matching reports 0.
   3. A device GameInput lists after the open reaches the open joystick at
      the next update. Without a change to the list, no update looks again.
      A device listed before a quit is found after a restart.
   4. GameInput missing, or its registration failing: every version 0, and
      the driver lists and opens the same joysticks as with GameInput.
   5. OpenXInput's identity busy, missing, or naming a shared interface.
   6. A different pad taking the slot after a disconnection, a device
      leaving GameInput, version 0.0, and the encoding of 10.1.
   7. The driver starting and quitting while another client's callback
      waits on the joystick lock inside GameInput's dispatch, callbacks
      delivered while that lock is held, and a flood of them during updates
      and across a quit.
   8. The string property: "5.17.3202.0" beside 0x0511, none for a slot
      GameInput does not list though its metadata has a record, set by a late
      listing, cleared by a disconnection and set again after it, none for
      0.0.0.0, "0.0.12.3" with version 0, a part above 255, and none once a
      pad GameInput does not list takes the slot.
   9. The only listed device in the slot's container, two in it, the slot's
      path over its container, and the empty and system containers. */

#define SDL_MAIN_HANDLED
#include "testxinputfirmware.h"

#include "joystick/SDL_sysjoystick.h"
#include "core/windows/SDL_windows.h"
#include "core/windows/SDL_xinput.h"

/* The names through which the driver would load XInput, start device
   notifications, change SDL's joystick list, or ask the other drivers */
extern bool Fake_LoadXInputDLL(void);
extern void Fake_UnloadXInputDLL(void);
extern void Fake_InitDeviceNotification(void);
extern void Fake_QuitDeviceNotification(void);
extern Uint64 Fake_GetLastDeviceNotification(void);
extern void Fake_JoystickAdded(SDL_JoystickID instance_id);
extern void Fake_JoystickRemoved(SDL_JoystickID instance_id);
extern bool Fake_HandledByAnotherDriver(struct SDL_JoystickDriver *driver, Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name);

#define WIN_LoadXInputDLL Fake_LoadXInputDLL
#define WIN_UnloadXInputDLL Fake_UnloadXInputDLL
#define WIN_InitDeviceNotification Fake_InitDeviceNotification
#define WIN_QuitDeviceNotification Fake_QuitDeviceNotification
#define WIN_GetLastDeviceNotification Fake_GetLastDeviceNotification
#define SDL_PrivateJoystickAdded Fake_JoystickAdded
#define SDL_PrivateJoystickRemoved Fake_JoystickRemoved
#define SDL_JoystickHandledByAnotherDriver Fake_HandledByAnotherDriver

#include "../src/joystick/windows/SDL_xinputjoystick.c"

int xf_checks;
int xf_failures;
const char *xf_scenario = "";

static const char *XUSB_A = "\\\\?\\USB#VID_045E&PID_0B12&IG_00#7&1a2b3c4d&0&0000#{ec87f1e3-c13b-4100-b5f7-8b84d54260cb}";
static const char *XUSB_A_UPPER = "\\\\?\\USB#VID_045E&PID_0B12&IG_00#7&1A2B3C4D&0&0000#{EC87F1E3-C13B-4100-B5F7-8B84D54260CB}";
static const char *HID_A = "\\\\?\\HID#VID_045E&PID_0B12&IG_00#8&2b3c4d5e&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
static const char *INSTANCE_A = "USB\\VID_045E&PID_0B12&IG_00\\7&1A2B3C4D&0&0000";
static const char *XUSB_B = "\\\\?\\USB#VID_045E&PID_0B00&IG_00#7&5e6f7a8b&0&0000#{ec87f1e3-c13b-4100-b5f7-8b84d54260cb}";
static const char *INSTANCE_B = "USB\\VID_045E&PID_0B00&IG_00\\7&5E6F7A8B&0&0000";
static const char *XUSB_C = "\\\\?\\USB#VID_045E&PID_0B13&IG_00#7&9c0d1e2f&0&0000#{ec87f1e3-c13b-4100-b5f7-8b84d54260cb}";
/* As GameInput lists a HIDMaestro virtual pad on the bench */
static const char *HIDMAESTRO = "\\\\?\\HID#HIDMAESTRO_VID_045E_PID_0B13&IG_00#1&d3dd7a1f&0&0000#{ec87f1e3-c13b-4100-b5f7-8b84d54260cb}";
/* A Bluetooth pad's XInput interface, in the form the bench lists one */
static const char *BLE_A = "\\\\?\\HID#{00001812-0000-1000-8000-00805f9b34fb}&Dev&VID_045e&PID_0b13&REV_0523&000000000001&IG_00#b&1a2b3c4&1&0000#{ec87f1e3-c13b-4100-b5f7-8b84d54260cb}";
/* Paths of the controller in other interfaces, with no device instance scripted */
static const char *GIP_A = "\\\\?\\USB#VID_045E&PID_0B12#UNIT_A#{020bc73c-0dca-4ee3-96d5-ab006adae9e9}";
static const char *GIP_B = "\\\\?\\USB#VID_045E&PID_0B12#UNIT_B#{020bc73c-0dca-4ee3-96d5-ab006adae9e9}";

typedef struct FakeSlot
{
    bool connected;
    Uint16 product;
    WCHAR path[256];
    Uint64 generation;
    DWORD interface_count;
    int busy; /* Identity calls that answer ERROR_BUSY first */
    DWORD packet;
} FakeSlot;

static FakeSlot slots[XUSER_MAX_COUNT];
static Uint64 next_generation = 1;
static bool identity_exported;
static int identity_queries;
static Uint64 notification = 1;
static int joysticks_added;
static int joysticks_removed;

static void Widen(const char *text, WCHAR *out, size_t capacity)
{
    size_t i;

    for (i = 0; text[i] && i + 1 < capacity; ++i) {
        out[i] = (WCHAR)(unsigned char)text[i];
    }
    out[i] = L'\0';
}

static DWORD WINAPI Fake_XInputGetState(DWORD user, XINPUT_STATE *state)
{
    if (user >= XUSER_MAX_COUNT || !slots[user].connected) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    SDL_zerop(state);
    state->dwPacketNumber = ++slots[user].packet;
    return ERROR_SUCCESS;
}

static DWORD WINAPI Fake_XInputSetState(DWORD user, XINPUT_VIBRATION *vibration)
{
    (void)vibration;
    return (user < XUSER_MAX_COUNT && slots[user].connected) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

static DWORD WINAPI Fake_XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES *capabilities)
{
    (void)flags;
    if (user >= XUSER_MAX_COUNT || !slots[user].connected) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    SDL_zerop(capabilities);
    capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
    capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    return ERROR_SUCCESS;
}

static DWORD WINAPI Fake_XInputGetCapabilitiesEx(DWORD reserved, DWORD user, DWORD flags, SDL_XINPUT_CAPABILITIES_EX *capabilities)
{
    (void)reserved;
    (void)flags;
    if (user >= XUSER_MAX_COUNT || !slots[user].connected) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    SDL_zerop(capabilities);
    capabilities->Capabilities.Type = XINPUT_DEVTYPE_GAMEPAD;
    capabilities->Capabilities.SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    capabilities->VendorId = 0x045E;
    capabilities->ProductId = slots[user].product;
    capabilities->ProductVersion = 0x0408;
    return ERROR_SUCCESS;
}

/* As OpenXInput answers (OpenXinput.cpp, OpenXInputGetDeviceIdentityV1) */
static DWORD WINAPI Fake_XInputGetDeviceIdentity(DWORD user, SDL_XINPUT_DEVICE_IDENTITY_V1 *identity, WCHAR *path, DWORD capacity)
{
    size_t length;

    ++identity_queries;
    if (!identity || identity->cbSize != sizeof(*identity) || user >= XUSER_MAX_COUNT || (!path && capacity)) {
        return ERROR_INVALID_PARAMETER;
    }
    identity->requiredCch = 0;
    identity->generation = 0;
    identity->interfaceIndex = 0;
    identity->interfaceCount = 0;
    if (path && capacity) {
        path[0] = L'\0';
    }
    if (slots[user].busy > 0) {
        --slots[user].busy;
        return ERROR_BUSY;
    }
    if (!slots[user].connected) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    length = SDL_wcslen(slots[user].path);
    identity->requiredCch = (DWORD)(length + 1);
    if (capacity < identity->requiredCch) {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    SDL_memcpy(path, slots[user].path, (length + 1) * sizeof(WCHAR));
    identity->generation = slots[user].generation;
    identity->interfaceIndex = 0;
    identity->interfaceCount = slots[user].interface_count;
    return ERROR_SUCCESS;
}

bool Fake_LoadXInputDLL(void)
{
    SDL_XInputGetState = Fake_XInputGetState;
    SDL_XInputSetState = Fake_XInputSetState;
    SDL_XInputGetCapabilities = Fake_XInputGetCapabilities;
    SDL_XInputGetCapabilitiesEx = Fake_XInputGetCapabilitiesEx;
    SDL_XInputGetBatteryInformation = NULL;
    SDL_XInputGetSystemButtons = NULL;
    SDL_XInputGetDeviceIdentity = identity_exported ? Fake_XInputGetDeviceIdentity : NULL;
    SDL_XInputGetStateExtended = NULL;
    return true;
}

void Fake_UnloadXInputDLL(void)
{
    SDL_XInputGetState = NULL;
    SDL_XInputSetState = NULL;
    SDL_XInputGetCapabilities = NULL;
    SDL_XInputGetCapabilitiesEx = NULL;
    SDL_XInputGetDeviceIdentity = NULL;
}

void Fake_InitDeviceNotification(void)
{
}

void Fake_QuitDeviceNotification(void)
{
}

Uint64 Fake_GetLastDeviceNotification(void)
{
    return notification;
}

void Fake_JoystickAdded(SDL_JoystickID instance_id)
{
    (void)instance_id;
    ++joysticks_added;
}

void Fake_JoystickRemoved(SDL_JoystickID instance_id)
{
    (void)instance_id;
    ++joysticks_removed;
}

bool Fake_HandledByAnotherDriver(struct SDL_JoystickDriver *driver, Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    (void)driver;
    (void)vendor_id;
    (void)product_id;
    (void)version;
    (void)name;
    return false;
}

static void Begin(const char *name, bool gameinput, bool register_fails)
{
    xf_scenario = name;
    SDL_zeroa(slots);
    identity_exported = true;
    identity_queries = 0;
    joysticks_added = 0;
    joysticks_removed = 0;
    FakeGI_Reset(gameinput, register_fails);
}

static void ScriptSlot(int user, const char *path, Uint16 product)
{
    slots[user].connected = true;
    slots[user].product = product;
    Widen(path, slots[user].path, SDL_arraysize(slots[user].path));
    slots[user].generation = next_generation++;
    slots[user].interface_count = 1;
}

static bool Start(void)
{
    bool result;

    SDL_LockJoysticks();
    result = SDL_XINPUT_JoystickDriver.Init();
    SDL_UnlockJoysticks();
    return result && SDL_XINPUT_Enabled();
}

static void Stop(void)
{
    SDL_LockJoysticks();
    SDL_XINPUT_JoystickDriver.Quit();
    SDL_UnlockJoysticks();
}

static SDL_Joystick *Open(int device_index)
{
    SDL_Joystick *joystick = (SDL_Joystick *)SDL_calloc(1, sizeof(*joystick));
    bool opened;

    if (!joystick) {
        return NULL;
    }
    joystick->instance_id = SDL_XINPUT_JoystickDriver.GetDeviceInstanceID(device_index);
    joystick->guid = SDL_XINPUT_JoystickDriver.GetDeviceGUID(device_index);
    joystick->attached = true;
    joystick->ref_count = 1;
    SDL_SetObjectValid(joystick, SDL_OBJECT_TYPE_JOYSTICK, true);

    SDL_LockJoysticks();
    opened = SDL_XINPUT_JoystickDriver.Open(joystick, device_index);
    SDL_UnlockJoysticks();
    if (opened) {
        // The arrays SDL_OpenJoystick allocates after the driver's open
        joystick->axes = (SDL_JoystickAxisInfo *)SDL_calloc((size_t)joystick->naxes, sizeof(*joystick->axes));
        joystick->hats = (Uint8 *)SDL_calloc((size_t)joystick->nhats, sizeof(*joystick->hats));
        joystick->buttons = (bool *)SDL_calloc((size_t)joystick->nbuttons, sizeof(*joystick->buttons));
        if (joystick->axes && joystick->hats && joystick->buttons) {
            return joystick;
        }
        SDL_LockJoysticks();
        SDL_XINPUT_JoystickDriver.Close(joystick);
        SDL_UnlockJoysticks();
    }
    SDL_free(joystick->axes);
    SDL_free(joystick->hats);
    SDL_free(joystick->buttons);
    if (joystick->props) {
        SDL_DestroyProperties(joystick->props);
    }
    SDL_SetObjectValid(joystick, SDL_OBJECT_TYPE_JOYSTICK, false);
    SDL_free(joystick);
    return NULL;
}

static void Update(SDL_Joystick *joystick)
{
    SDL_LockJoysticks();
    SDL_XINPUT_JoystickDriver.Update(joystick);
    SDL_UnlockJoysticks();
}

static void Close(SDL_Joystick *joystick)
{
    if (!joystick) {
        return;
    }
    SDL_LockJoysticks();
    SDL_XINPUT_JoystickDriver.Close(joystick);
    SDL_UnlockJoysticks();
    if (joystick->props) {
        SDL_DestroyProperties(joystick->props);
    }
    SDL_free(joystick->axes);
    SDL_free(joystick->hats);
    SDL_free(joystick->buttons);
    SDL_SetObjectValid(joystick, SDL_OBJECT_TYPE_JOYSTICK, false);
    SDL_free(joystick);
}

static Uint16 Reported(SDL_Joystick *joystick)
{
    return joystick ? SDL_GetJoystickFirmwareVersion(joystick) : 0xFFFF;
}

/* GameInput's whole version as the joystick carries it, or "" without one */
static const char *Whole(SDL_Joystick *joystick)
{
    return joystick ? SDL_GetStringProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_XINPUT_FIRMWARE_STRING, "") : "?";
}

/* Disconnects the scenario's devices, after which the list holds nothing of
   them. Nothing is ever unregistered, GameInput is never quit, and no other
   way of finding a device is used. */
static void Finish(int devices)
{
    int i;

    for (i = 0; i < devices; ++i) {
        FakeGI_Disconnect(i);
    }
    FakeGI_Deliver();
    for (i = 0; i < devices; ++i) {
        CHECK(FakeGI_References(i) == 0, "device %d kept %ld references", i, FakeGI_References(i));
    }
    CHECK(FakeGI_Quits() == 0, "GameInput was quit %d times", FakeGI_Quits());
    CHECK(FakeGI_Unregistrations() == 0, "%d unregistrations", FakeGI_Unregistrations());
    CHECK(FakeGI_PlatformStringLookups() == 0, "FindDeviceFromPlatformString was called %d times", FakeGI_PlatformStringLookups());
    CHECK(FakeGI_Unexpected() == 0, "%d calls the lookup has no use for", FakeGI_Unexpected());
}

typedef struct Listing
{
    int count;
    char names[XUSER_MAX_COUNT][64];
    char paths[XUSER_MAX_COUNT][64];
    SDL_GUID guids[XUSER_MAX_COUNT];
} Listing;

static void ReadListing(Listing *listing)
{
    int i;

    SDL_zerop(listing);
    SDL_LockJoysticks();
    listing->count = SDL_XINPUT_JoystickDriver.GetCount();
    for (i = 0; i < listing->count && i < XUSER_MAX_COUNT; ++i) {
        SDL_strlcpy(listing->names[i], SDL_XINPUT_JoystickDriver.GetDeviceName(i), sizeof(listing->names[i]));
        SDL_strlcpy(listing->paths[i], SDL_XINPUT_JoystickDriver.GetDevicePath(i), sizeof(listing->paths[i]));
        listing->guids[i] = SDL_XINPUT_JoystickDriver.GetDeviceGUID(i);
    }
    SDL_UnlockJoysticks();
}

/* The two pads in slots 0 and 1, GameInput listing them and a HIDMaestro
   virtual pad. With busy_start, another client's callback holds GameInput's
   dispatch, waiting on the joystick lock, while the driver starts under it. */
static void RunListing(const char *name, bool gameinput, bool register_fails, bool busy_start,
                       Listing *listing, Uint16 *versions, int *added)
{
    SDL_Joystick *joysticks[2];
    bool started;
    int i;

    Begin(name, gameinput, register_fails);
    ScriptSlot(0, XUSB_A, 0x0B12);
    ScriptSlot(1, XUSB_B, 0x0B00);
    (void)FakeGI_AddDevice(XUSB_A, 5, 17, 3202, 0);
    (void)FakeGI_AddDevice(XUSB_B, 5, 21, 3307, 0);
    (void)FakeGI_AddDevice(HIDMAESTRO, 0, 0, 0, 0);
    if (busy_start) {
        SDL_LockJoysticks();
        FakeGI_StartForeign();
        started = Start();
        CHECK(!FakeGI_Registered(), "the callback was registered while GameInput's dispatch was held");
        SDL_UnlockJoysticks();
        FakeGI_WaitForeign();
    } else {
        started = Start();
    }
    CHECK(started, "the driver did not start");
    if (gameinput && !register_fails) {
        CHECK(FakeGI_WaitRegistered(5000), "the device callback was never registered");
    } else if (gameinput) {
        CHECK(FakeGI_WaitRegistrations(1, 5000), "the registration was never attempted");
    }
    CHECK(FakeGI_WaitModuleSettled(5000), "the registering thread never finished");
    FakeGI_Deliver();
    ReadListing(listing);
    for (i = 0; i < 2; ++i) {
        joysticks[i] = Open(i);
        CHECK(joysticks[i] != NULL, "joystick %d did not open: %s", i, SDL_GetError());
        Update(joysticks[i]);
        versions[i] = Reported(joysticks[i]);
    }
    *added = joysticks_added;
    for (i = 0; i < 2; ++i) {
        Close(joysticks[i]);
    }
    Stop();
    Finish(3);
}

/* First in the run: the registration that succeeds here lasts for the process */
static void TestWithoutGameInput(void)
{
    Listing with, missing, refused;
    Uint16 versions[2];
    int added_with, added_missing, added_refused;
    int i;

    RunListing("GameInput missing", false, false, false, &missing, versions, &added_missing);
    CHECK(versions[0] == 0 && versions[1] == 0, "GameInput missing reported 0x%04x and 0x%04x", versions[0], versions[1]);
    CHECK(FakeGI_Registrations() == 0, "a device callback was registered without GameInput");

    RunListing("GameInput refusing the callback", true, true, false, &refused, versions, &added_refused);
    CHECK(versions[0] == 0 && versions[1] == 0, "a refused callback reported 0x%04x and 0x%04x", versions[0], versions[1]);
    CHECK(FakeGI_Inits() == 1, "GameInput was started %d times", FakeGI_Inits());

    RunListing("GameInput present, its dispatch held at the start", true, false, true, &with, versions, &added_with);
    CHECK(versions[0] == 0x0511 && versions[1] == 0x0515, "GameInput present reported 0x%04x and 0x%04x", versions[0], versions[1]);
    CHECK(FakeGI_Inits() == 0, "GameInput was started again, %d times", FakeGI_Inits());
    CHECK(FakeGI_Registrations() == 1, "%d registrations", FakeGI_Registrations());

    xf_scenario = "the same joysticks with and without GameInput";
    CHECK(with.count == 2 && missing.count == 2 && refused.count == 2, "the driver listed %d, %d and %d joysticks", with.count, missing.count, refused.count);
    CHECK(added_with == 2 && added_missing == 2 && added_refused == 2, "%d, %d and %d joysticks were added", added_with, added_missing, added_refused);
    for (i = 0; i < 2; ++i) {
        CHECK(SDL_strcmp(with.names[i], missing.names[i]) == 0 && SDL_strcmp(with.names[i], refused.names[i]) == 0,
              "joystick %d was named %s, %s and %s", i, with.names[i], missing.names[i], refused.names[i]);
        CHECK(SDL_strcmp(with.paths[i], missing.paths[i]) == 0 && SDL_strcmp(with.paths[i], refused.paths[i]) == 0,
              "joystick %d had the paths %s, %s and %s", i, with.paths[i], missing.paths[i], refused.paths[i]);
        CHECK(SDL_memcmp(&with.guids[i], &missing.guids[i], sizeof(SDL_GUID)) == 0 &&
              SDL_memcmp(&with.guids[i], &refused.guids[i], sizeof(SDL_GUID)) == 0, "joystick %d had different GUIDs", i);
    }
}

static void TestMatch(const char *name, const char *listed_path, bool same_instance)
{
    SDL_Joystick *joystick;
    int device;

    Begin(name, true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    device = FakeGI_AddDevice(listed_path, 5, 17, 3202, 0);
    if (same_instance) {
        FakeGI_SetInstance(listed_path, INSTANCE_A);
        FakeGI_SetInstance(XUSB_A, INSTANCE_A);
    }
    CHECK(Start(), "the driver did not start");
    CHECK(FakeGI_Registered() && FakeGI_Registrations() == 0, "the registration did not carry over: %d more", FakeGI_Registrations());
    FakeGI_Deliver();
    CHECK(FakeGI_References(device) == 1, "the list holds %ld references", FakeGI_References(device));
    joystick = Open(0);
    CHECK(joystick != NULL, "the open failed: %s", SDL_GetError());
    CHECK(Reported(joystick) == 0x0511, "the open reported 0x%04x", Reported(joystick));
    CHECK(SDL_strcmp(Whole(joystick), "5.17.3202.0") == 0, "the open published \"%s\"", Whole(joystick));
    Update(joystick);
    CHECK(Reported(joystick) == 0x0511, "the update reported 0x%04x", Reported(joystick));
    CHECK(SDL_strcmp(Whole(joystick), "5.17.3202.0") == 0, "the update published \"%s\"", Whole(joystick));
    Close(joystick);
    Stop();
    Finish(1);
}

/* The registration does not enumerate again after a restart, so the list
   must outlive the quit */
static void TestRestart(void)
{
    SDL_Joystick *joystick;

    Begin("a device listed before a quit and a restart", true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    (void)FakeGI_AddDevice(XUSB_A, 5, 17, 3202, 0);
    CHECK(Start(), "the driver did not start");
    FakeGI_Deliver();
    Stop();
    CHECK(Start(), "the driver did not start again");
    joystick = Open(0);
    CHECK(Reported(joystick) == 0x0511, "the open after the restart reported 0x%04x", Reported(joystick));
    Close(joystick);
    Stop();
    Finish(1);
}

static void TestNoMatch(void)
{
    SDL_Joystick *joystick;
    int queries;
    int i;

    Begin("no match", true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    (void)FakeGI_AddDevice(XUSB_B, 5, 17, 3202, 0);
    FakeGI_SetInstance(XUSB_A, INSTANCE_A);
    FakeGI_SetInstance(XUSB_B, INSTANCE_B);
    CHECK(Start(), "the driver did not start");
    FakeGI_Deliver();
    joystick = Open(0);
    CHECK(joystick != NULL, "the open failed: %s", SDL_GetError());
    CHECK(Reported(joystick) == 0, "the open reported 0x%04x", Reported(joystick));
    queries = identity_queries;
    for (i = 0; i < 3; ++i) {
        Update(joystick);
    }
    CHECK(identity_queries == queries, "updates with no change to the list asked %d more times", identity_queries - queries);
    CHECK(Reported(joystick) == 0, "the updates reported 0x%04x", Reported(joystick));
    Close(joystick);
    Stop();
    Finish(1);
}

static void TestLate(void)
{
    SDL_Joystick *joystick;
    int queries;

    Begin("listed after the open", true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    CHECK(Start(), "the driver did not start");
    joystick = Open(0);
    CHECK(joystick != NULL, "the open failed: %s", SDL_GetError());
    CHECK(Reported(joystick) == 0, "the open reported 0x%04x", Reported(joystick));
    Update(joystick);
    CHECK(Reported(joystick) == 0, "an update before the listing reported 0x%04x", Reported(joystick));

    (void)FakeGI_AddDevice(XUSB_A, 5, 17, 3202, 0);
    FakeGI_Deliver();
    CHECK(Reported(joystick) == 0, "the callback set 0x%04x without the joystick lock", Reported(joystick));
    Update(joystick);
    CHECK(Reported(joystick) == 0x0511, "the update after the listing reported 0x%04x", Reported(joystick));
    queries = identity_queries;
    (void)FakeGI_AddDevice(HIDMAESTRO, 0, 0, 0, 0);
    FakeGI_Deliver();
    Update(joystick);
    CHECK(identity_queries == queries, "a found version was looked up again");
    Close(joystick);
    Stop();
    Finish(2);
}

static void TestZeroAndEncoding(void)
{
    SDL_Joystick *first;
    SDL_Joystick *second;
    int queries;

    Begin("version 0.0, and 10.1", true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    ScriptSlot(1, XUSB_B, 0x0B00);
    (void)FakeGI_AddDevice(XUSB_A, 0, 0, 0, 0);
    (void)FakeGI_AddDevice(XUSB_B, 10, 1, 7, 2);
    CHECK(Start(), "the driver did not start");
    FakeGI_Deliver();
    /* The driver lists slot 0 first */
    first = Open(0);
    second = Open(1);
    CHECK(first && second, "the opens failed: %s", SDL_GetError());
    CHECK(Reported(first) == 0, "0.0 reported 0x%04x", Reported(first));
    CHECK(Whole(first)[0] == '\0', "0.0.0.0 published \"%s\"", Whole(first));
    CHECK(Reported(second) == 0x0A01, "10.1 reported 0x%04x", Reported(second));
    CHECK(SDL_strcmp(Whole(second), "10.1.7.2") == 0, "10.1.7.2 published \"%s\"", Whole(second));
    queries = identity_queries;
    (void)FakeGI_AddDevice(XUSB_C, 5, 21, 0, 0);
    FakeGI_Deliver();
    Update(first);
    CHECK(identity_queries == queries, "a device found with 0.0 was looked up again");
    Close(first);
    Close(second);
    Stop();
    Finish(3);
}

static void TestIdentity(void)
{
    SDL_Joystick *joystick;

    Begin("identity busy at the open", true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    slots[0].busy = 1;
    (void)FakeGI_AddDevice(XUSB_A, 5, 17, 3202, 0);
    CHECK(Start(), "the driver did not start");
    FakeGI_Deliver();
    joystick = Open(0);
    CHECK(Reported(joystick) == 0, "a busy identity reported 0x%04x", Reported(joystick));
    Update(joystick);
    CHECK(Reported(joystick) == 0x0511, "the next update reported 0x%04x", Reported(joystick));
    CHECK(identity_queries == 2, "the identity was asked %d times", identity_queries);
    Close(joystick);
    Stop();
    Finish(1);

    Begin("no identity export", true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    identity_exported = false;
    (void)FakeGI_AddDevice(XUSB_A, 5, 17, 3202, 0);
    CHECK(Start(), "the driver did not start");
    FakeGI_Deliver();
    /* The one lookup is the listed device's own */
    CHECK(FakeGI_InstanceLookups() == 1, "%d instance lookups for one listed device", FakeGI_InstanceLookups());
    joystick = Open(0);
    Update(joystick);
    CHECK(Reported(joystick) == 0, "no identity export reported 0x%04x", Reported(joystick));
    CHECK(FakeGI_InstanceLookups() == 1, "%d instance lookups after the open without an identity", FakeGI_InstanceLookups());
    Close(joystick);
    Stop();
    Finish(1);

    Begin("a slot path shared by two interfaces", true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    slots[0].interface_count = 2;
    (void)FakeGI_AddDevice(XUSB_A, 5, 17, 3202, 0);
    CHECK(Start(), "the driver did not start");
    FakeGI_Deliver();
    joystick = Open(0);
    Update(joystick);
    CHECK(Reported(joystick) == 0, "a shared interface reported 0x%04x", Reported(joystick));
    Close(joystick);
    Stop();
    Finish(1);
}

static void TestSlotChanges(void)
{
    SDL_Joystick *joystick;
    int a;

    Begin("another pad in the slot after a disconnection", true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    a = FakeGI_AddDevice(XUSB_A, 5, 17, 3202, 0);
    (void)FakeGI_AddDevice(XUSB_B, 5, 21, 3307, 0);
    CHECK(Start(), "the driver did not start");
    FakeGI_Deliver();
    joystick = Open(0);
    CHECK(Reported(joystick) == 0x0511, "the open reported 0x%04x", Reported(joystick));
    slots[0].connected = false;
    Update(joystick);
    ScriptSlot(0, XUSB_B, 0x0B12);
    Update(joystick);
    CHECK(Reported(joystick) == 0x0515, "the new pad reported 0x%04x", Reported(joystick));
    slots[0].connected = false;
    Update(joystick);
    ScriptSlot(0, XUSB_C, 0x0B12);
    Update(joystick);
    CHECK(Reported(joystick) == 0, "an unlisted pad reported 0x%04x", Reported(joystick));

    xf_scenario = "a device leaving GameInput";
    slots[0].connected = false;
    Update(joystick);
    ScriptSlot(0, XUSB_A, 0x0B12);
    Update(joystick);
    CHECK(Reported(joystick) == 0x0511, "the first pad back reported 0x%04x", Reported(joystick));
    FakeGI_Disconnect(a);
    FakeGI_Deliver();
    CHECK(FakeGI_References(a) == 0, "a removed device kept %ld references", FakeGI_References(a));
    Update(joystick);
    CHECK(Reported(joystick) == 0x0511, "the removal changed the version to 0x%04x", Reported(joystick));
    Close(joystick);
    Stop();
    Finish(2);
}

static void TestThreads(void)
{
    SDL_Joystick *joystick;
    int a;
    int i;

    Begin("callbacks while the joystick lock is held", true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    a = FakeGI_AddDevice(XUSB_A, 5, 17, 3202, 0);
    CHECK(Start(), "the driver did not start");
    /* The callback must finish without the joystick lock, or this waits forever */
    SDL_LockJoysticks();
    FakeGI_Deliver();
    SDL_UnlockJoysticks();
    joystick = Open(0);
    CHECK(Reported(joystick) == 0x0511, "the open reported 0x%04x", Reported(joystick));
    Close(joystick);

    xf_scenario = "a flood of callbacks during updates";
    ScriptSlot(1, XUSB_B, 0x0B00);
    notification++;
    SDL_LockJoysticks();
    SDL_XINPUT_JoystickDriver.Detect();
    SDL_UnlockJoysticks();
    joystick = Open(1);
    CHECK(Reported(joystick) == 0, "the open reported 0x%04x", Reported(joystick));
    FakeGI_StartFlood(a, 2000);
    for (i = 0; i < 500; ++i) {
        Update(joystick);
    }
    FakeGI_WaitFlood();
    CHECK(Reported(joystick) == 0, "the flood of another device reported 0x%04x", Reported(joystick));
    CHECK(FakeGI_References(a) == 1, "the flooded device holds %ld references", FakeGI_References(a));
    (void)FakeGI_AddDevice(XUSB_B, 5, 21, 3307, 0);
    FakeGI_Deliver();
    Update(joystick);
    CHECK(Reported(joystick) == 0x0515, "the update after the flood reported 0x%04x", Reported(joystick));
    Close(joystick);

    xf_scenario = "a quit during a flood of callbacks";
    FakeGI_StartFlood(a, 2000);
    Stop();
    FakeGI_WaitFlood();
    CHECK(FakeGI_References(a) == 1, "the flooded device holds %ld references after the quit", FakeGI_References(a));

    xf_scenario = "a quit while another client's callback waits on the joystick lock";
    CHECK(Start(), "the driver did not start again");
    SDL_LockJoysticks();
    FakeGI_StartForeign();
    SDL_XINPUT_JoystickDriver.Quit();
    SDL_UnlockJoysticks();
    FakeGI_WaitForeign();
    Finish(2);
}

/* First in the run, before any GameInput starts: the lookup keeps the first
   GameInput it gets for the process */
static void TestRecordWithoutGameInput(void)
{
    SDL_Joystick *joystick;

    Begin("a Bluetooth pad's metadata without GameInput", false, false);
    ScriptSlot(0, BLE_A, 0x0B13);
    FakeGI_SetRecord(BLE_A, 1);
    CHECK(Start(), "the driver did not start");
    joystick = Open(0);
    CHECK(joystick != NULL, "the open failed: %s", SDL_GetError());
    CHECK(Reported(joystick) == 0, "a slot without GameInput reported 0x%04x", Reported(joystick));
    CHECK(Whole(joystick)[0] == '\0', "a slot without GameInput published \"%s\"", Whole(joystick));
    CHECK(FakeGI_RecordReads() == 0, "the record was read %d times without GameInput", FakeGI_RecordReads());
    Close(joystick);
    Stop();
    CHECK(FakeGI_Inits() == 0, "GameInput was started %d times", FakeGI_Inits());
    Finish(0);
}

static void TestFirmwareString(void)
{
    SDL_Joystick *first;
    SDL_Joystick *second;

    Begin("a slot GameInput does not list, with a record", true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    FakeGI_SetRecord(XUSB_A, 1);
    (void)FakeGI_AddDevice(XUSB_B, 5, 21, 3307, 0);
    CHECK(Start(), "the driver did not start");
    FakeGI_Deliver();
    first = Open(0);
    CHECK(first != NULL, "the open failed: %s", SDL_GetError());
    CHECK(Reported(first) == 0, "an unlisted slot reported 0x%04x", Reported(first));
    CHECK(Whole(first)[0] == '\0', "an unlisted slot published \"%s\"", Whole(first));
    CHECK(FakeGI_RecordReads() == 1, "the record was read %d times", FakeGI_RecordReads());

    xf_scenario = "a late listing";
    (void)FakeGI_AddDevice(XUSB_A, 5, 17, 3202, 0);
    FakeGI_Deliver();
    Update(first);
    CHECK(Reported(first) == 0x0511, "the late listing reported 0x%04x", Reported(first));
    CHECK(SDL_strcmp(Whole(first), "5.17.3202.0") == 0, "the late listing published \"%s\"", Whole(first));

    xf_scenario = "a disconnection, then the slot's pad back";
    slots[0].connected = false;
    Update(first);
    CHECK(Whole(first)[0] == '\0', "the disconnection left \"%s\"", Whole(first));
    ScriptSlot(0, XUSB_A, 0x0B12);
    Update(first);
    CHECK(Reported(first) == 0x0511, "the next lookup reported 0x%04x", Reported(first));
    CHECK(SDL_strcmp(Whole(first), "5.17.3202.0") == 0, "the next lookup published \"%s\"", Whole(first));

    xf_scenario = "a pad GameInput does not list taking the slot";
    slots[0].connected = false;
    Update(first);
    ScriptSlot(0, XUSB_C, 0x0B12);
    Update(first);
    CHECK(Reported(first) == 0, "the unlisted pad reported 0x%04x", Reported(first));
    CHECK(Whole(first)[0] == '\0', "the unlisted pad published \"%s\"", Whole(first));
    Close(first);
    Stop();
    Finish(2);

    /* The string keeps parts the 16-bit value cannot */
    Begin("parts the 16-bit value cannot hold", true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    ScriptSlot(1, XUSB_B, 0x0B00);
    (void)FakeGI_AddDevice(XUSB_A, 0, 0, 12, 3);
    (void)FakeGI_AddDevice(XUSB_B, 5, 300, 1, 0);
    CHECK(Start(), "the driver did not start");
    FakeGI_Deliver();
    first = Open(0);
    second = Open(1);
    CHECK(first && second, "the opens failed: %s", SDL_GetError());
    CHECK(Reported(first) == 0, "0.0.12.3 reported 0x%04x", Reported(first));
    CHECK(SDL_strcmp(Whole(first), "0.0.12.3") == 0, "0.0.12.3 published \"%s\"", Whole(first));
    CHECK(Reported(second) == 0, "a minor part of 300 reported 0x%04x", Reported(second));
    CHECK(SDL_strcmp(Whole(second), "5.300.1.0") == 0, "5.300.1.0 published \"%s\"", Whole(second));
    Close(first);
    Close(second);
    Stop();
    Finish(2);
}

/* The slot's record names a container, and GameInput lists two devices at
   other paths, or the second at the slot's path */
static void ContainerCase(const char *name, bool has_record, int record, int first, int second, bool second_at_slot_path, Uint16 expected, const char *whole)
{
    SDL_Joystick *joystick;
    int device;

    Begin(name, true, false);
    ScriptSlot(0, XUSB_A, 0x0B12);
    if (has_record) {
        FakeGI_SetRecord(XUSB_A, record);
    }
    device = FakeGI_AddDevice(GIP_A, 5, 17, 3202, 0);
    FakeGI_SetContainer(device, first);
    device = FakeGI_AddDevice(second_at_slot_path ? XUSB_A : GIP_B, 5, 21, 3307, 0);
    FakeGI_SetContainer(device, second);
    CHECK(Start(), "the driver did not start");
    FakeGI_Deliver();
    joystick = Open(0);
    CHECK(joystick != NULL, "the open failed: %s", SDL_GetError());
    CHECK(Reported(joystick) == expected, "the open reported 0x%04x, expected 0x%04x", Reported(joystick), expected);
    CHECK(SDL_strcmp(Whole(joystick), whole) == 0, "the open published \"%s\", expected \"%s\"", Whole(joystick), whole);
    Close(joystick);
    Stop();
    Finish(2);
}

static void TestContainer(void)
{
    ContainerCase("the only listed device in the slot's container", true, 7, 7, 8, false, 0x0511, "5.17.3202.0");
    ContainerCase("two listed devices in the slot's container", true, 7, 7, 7, false, 0, "");
    ContainerCase("the slot's path over its container", true, 7, 7, 9, true, 0x0515, "5.21.3307.0");
    ContainerCase("no record for the slot", false, 0, 7, 8, false, 0, "");
    ContainerCase("the system container", true, -1, -1, 8, false, 0, "");
    ContainerCase("the empty container", true, 0, 0, 8, false, 0, "");
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    TestRecordWithoutGameInput();
    TestWithoutGameInput();
    TestMatch("the slot's path", XUSB_A, false);
    TestMatch("the slot's path in other case", XUSB_A_UPPER, false);
    TestMatch("another path naming the same device instance", HID_A, true);
    TestRestart();
    TestNoMatch();
    TestLate();
    TestZeroAndEncoding();
    TestIdentity();
    TestSlotChanges();
    TestThreads();
    TestFirmwareString();
    TestContainer();

    SDL_Quit();
    if (xf_failures) {
        printf("testxinputfirmware FAILED: %d of %d checks\n", xf_failures, xf_checks);
        return 1;
    }
    printf("testxinputfirmware PASSED: %d checks, 0 failures\n", xf_checks);
    return 0;
}
