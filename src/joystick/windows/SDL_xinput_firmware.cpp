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
#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_XINPUT

#include "../SDL_sysjoystick.h"
#include "../../core/windows/SDL_windows.h"
#include "../../core/windows/SDL_xinput.h"
#include "../../core/windows/SDL_gameinput.h"
#include "SDL_xinputjoystick_hwdata.h"
#include "SDL_xinput_firmware_c.h"
#ifdef SDL_JOYSTICK_XINPUT_SLOT_RECORD
#include "SDL_xinput_slot_record.h"
#endif

/* PadForge fork: XInput has no firmware query, so the XInput driver reads an
   open pad's version from GameInput's device information
   (hifihedgehog/SDL#44). A GameInput device callback keeps a list of every
   controller's device path, device instance, device container and firmware
   version. A slot's device is the one whose path is the path OpenXInput
   gives for the slot, ignoring case, or whose path names the same device
   instance, or else the only listed device in the slot's device container.
   Not FindDeviceFromPlatformString, which returned 0x80070490 for these paths
   in the hifihedgehog/SDL#28 tests. GameInput supplies the number only: no
   joystick, input or device ownership comes from it.

   When GameInput has no version for the slot, the slot's device metadata can
   have one: a Bluetooth controller's PnP ID product version, or a wired
   controller's USB release number (SDL_xinput_slot_record.h). That covers a
   machine without a GameInput runtime that offers the v4 interface, and a
   pad GameInput has not listed yet. GameInput's version replaces it once
   GameInput lists the pad.

   GameInput 4.0.302 makes RegisterDeviceCallback and UnregisterCallback wait
   for whatever callback it is running, any client's. The XInput driver
   starts and quits under the joystick lock, so a callback waiting on that
   lock would hold either call forever, as SDL's GameInput driver's callback
   could until hifihedgehog/SDL#45. So a thread of this module's own
   registers the callback, the callback takes no lock but the list's, and
   nothing unregisters: the registration, its device list and the GameInput
   reference last for the process, whatever another callback waits on. This
   fork serves PadForge, which loads SDL for the life of its process. */

#if defined(HAVE_GAMEINPUT_H) && (GAMEINPUT_API_VERSION >= 4) && \
    !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)

// The paddle route's limit for a slot's path (SDL_xinput_paddle.cpp, PathLimit)
#define FIRMWARE_PATH_LIMIT 1024
// Above MAX_DEVICE_ID_LEN, 200 characters (cfgmgr32.h)
#define FIRMWARE_INSTANCE_LIMIT 256

typedef struct XInputFirmwareDevice
{
    IGameInputDevice *device; // Held, and the key for its removal
    WCHAR *path;              // GetPnpPath, in UTF-16
    WCHAR instance[FIRMWARE_INSTANCE_LIMIT]; // Empty when unknown
    GUID container;           // GetContainerId
    bool has_container;       // False for the empty and the system container
    Uint16 version;           // (major << 8) | minor
} XInputFirmwareDevice;

enum
{
    FIRMWARE_UNREGISTERED,
    FIRMWARE_REGISTERING,
    FIRMWARE_REGISTERED
};

// Each lasts for the process once set
static IGameInput *s_firmware_gameinput;
static SDL_AtomicInt s_firmware_registration;
// Guards the list. The device callback takes no other lock.
static SDL_Mutex *s_firmware_lock;
static XInputFirmwareDevice *s_firmware_devices;
static int s_firmware_device_count;
// Moves on every change to the list
static SDL_AtomicInt s_firmware_generation;

/* Ignoring case, as the paddle route compares paths and instance IDs
   (SDL_xinput_paddle_identity.cpp, EqualText). Device instance IDs, and the
   interface paths made from them, are printable ASCII, where SDL_wcscasecmp
   and its CompareStringOrdinal agree. */
static bool EqualText(const WCHAR *a, const WCHAR *b)
{
    return a && b && a[0] && b[0] && SDL_wcscasecmp(a, b) == 0;
}

/* Not the empty GUID, and not the container Windows gives every internal
   device, as the paddle route qualifies one (SDL_xinput_paddle_identity.cpp,
   ValidContainer) */
static bool ValidContainer(const GUID *container)
{
    static const GUID empty = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };
    static const GUID system = { 0, 0, 0, { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };

    return SDL_memcmp(container, &empty, sizeof(*container)) != 0 &&
           SDL_memcmp(container, &system, sizeof(*container)) != 0;
}

static void AddFirmwareDevice(IGameInputDevice *device)
{
    char path[FIRMWARE_PATH_LIMIT];
    uint32_t bytes;
    GameInputVersion version;
    XInputFirmwareDevice entry;
    XInputFirmwareDevice *devices;
    int i;

    SDL_zero(entry);
    bytes = device->GetPnpPath(sizeof(path), path);
    if (bytes < 2 || bytes > sizeof(path) || path[bytes - 1] != '\0' || SDL_strlen(path) != bytes - 1) {
        return;
    }
    entry.path = WIN_UTF8ToStringW(path);
    if (!entry.path) {
        return;
    }
    // The encoding the GIP driver gives its version (SDL_hidapi_gip.c, GIP_GetFirmwareVersion)
    SDL_zero(version);
    device->GetFirmwareVersion(&version);
    entry.version = (Uint16)((version.major << 8) | version.minor);
    (void)WIN_GetDeviceInterfaceInstanceId(entry.path, entry.instance, SDL_arraysize(entry.instance));
    device->GetContainerId(&entry.container);
    entry.has_container = ValidContainer(&entry.container);

    SDL_LockMutex(s_firmware_lock);
    for (i = 0; i < s_firmware_device_count; ++i) {
        if (s_firmware_devices[i].device == device) {
            break;
        }
    }
    if (i == s_firmware_device_count) {
        devices = (XInputFirmwareDevice *)SDL_realloc(s_firmware_devices, (size_t)(s_firmware_device_count + 1) * sizeof(*devices));
        if (devices) {
            device->AddRef();
            entry.device = device;
            devices[s_firmware_device_count++] = entry;
            s_firmware_devices = devices;
            entry.path = NULL; // The list owns it now
            SDL_AddAtomicInt(&s_firmware_generation, 1);
        }
    }
    SDL_UnlockMutex(s_firmware_lock);

    SDL_free(entry.path);
}

static void RemoveFirmwareDevice(IGameInputDevice *device)
{
    IGameInputDevice *removed = NULL;
    WCHAR *path = NULL;
    int i;

    SDL_LockMutex(s_firmware_lock);
    for (i = 0; i < s_firmware_device_count; ++i) {
        if (s_firmware_devices[i].device == device) {
            removed = s_firmware_devices[i].device;
            path = s_firmware_devices[i].path;
            --s_firmware_device_count;
            SDL_memmove(&s_firmware_devices[i], &s_firmware_devices[i + 1], (size_t)(s_firmware_device_count - i) * sizeof(*s_firmware_devices));
            SDL_AddAtomicInt(&s_firmware_generation, 1);
            break;
        }
    }
    SDL_UnlockMutex(s_firmware_lock);

    if (removed) {
        removed->Release();
    }
    SDL_free(path);
}

/* GameInput runs its callbacks one at a time on its own thread, and lists the
   devices already connected there after RegisterDeviceCallback returns. This
   one takes only the list's lock, never the joystick lock. Connection is read
   as SDL's GameInput driver reads it (SDL_gameinputjoystick.cpp,
   GAMEINPUT_InternalJoystickDeviceCallback). */
static void CALLBACK XInputFirmwareDeviceCallback(GameInputCallbackToken callbackToken, void *context, GameInputDeviceEvent deviceEvent, IGameInputDevice *device)
{
    (void)callbackToken;
    (void)context;
    (void)deviceEvent;

    if (!device) {
        return;
    }
    if (device->IsConnected()) {
        AddFirmwareDevice(device);
    } else {
        RemoveFirmwareDevice(device);
    }
}

/* The slot's device path from OpenXInput, checked as the paddle route checks
   it (SDL_xinput_paddle.cpp, QuerySlot): a nonzero generation, one interface
   for the slot, and a terminated path that fits */
static DWORD QuerySlotPath(Uint8 userid, WCHAR *path, DWORD capacity)
{
    SDL_XINPUT_DEVICE_IDENTITY_V1 identity;
    DWORD status;
    DWORD length;
    DWORD i;

    SDL_zero(identity);
    identity.cbSize = sizeof(identity);
    status = SDL_XInputGetDeviceIdentity(userid, &identity, path, capacity);
    if (status != ERROR_SUCCESS) {
        return status;
    }
    length = identity.requiredCch;
    if (!identity.generation || identity.interfaceIndex != 0 || identity.interfaceCount != 1 ||
        length < 2 || length > capacity || path[length - 1] != L'\0') {
        return ERROR_INVALID_DATA;
    }
    for (i = 0; i + 1 < length; ++i) {
        if (path[i] == L'\0') {
            return ERROR_INVALID_DATA;
        }
    }
    return ERROR_SUCCESS;
}

/* The slot's device in GameInput's list: the one with the slot's path,
   ignoring case, or with the slot's device instance, or else the only one in
   the slot's device container. Two in one container match neither. */
static bool FindFirmwareDevice(const WCHAR *path, const GUID *container, Uint16 *version)
{
    WCHAR instance[FIRMWARE_INSTANCE_LIMIT];
    const bool have_instance = WIN_GetDeviceInterfaceInstanceId(path, instance, SDL_arraysize(instance));
    Uint16 container_version = 0;
    int in_container = 0;
    bool found = false;
    int i;

    SDL_LockMutex(s_firmware_lock);
    for (i = 0; i < s_firmware_device_count; ++i) {
        const XInputFirmwareDevice *device = &s_firmware_devices[i];

        if (EqualText(device->path, path) || (have_instance && EqualText(device->instance, instance))) {
            found = true;
            *version = device->version;
            break;
        }
        if (container && device->has_container && SDL_memcmp(&device->container, container, sizeof(*container)) == 0) {
            ++in_container;
            container_version = device->version;
        }
    }
    SDL_UnlockMutex(s_firmware_lock);

    if (!found && in_container == 1) {
        found = true;
        *version = container_version;
    }
    return found;
}

static void LookUpFirmwareVersion(SDL_Joystick *joystick)
{
    struct joystick_hwdata *hwdata = joystick->hwdata;
    WCHAR path[FIRMWARE_PATH_LIMIT];
    GUID container;
    bool have_container = false;
    Uint16 record = 0;  // From the slot's device metadata
    Uint16 version = 0; // From GameInput
    bool found = false;
    int generation;
    DWORD status;

    SDL_AssertJoysticksLocked();

    if (!SDL_XInputGetDeviceIdentity) {
        // Microsoft's xinput1_4.dll names no device for a slot: the version stays 0
        hwdata->firmware_settled = true;
        return;
    }

    // Read before the lookup, so a device listed meanwhile brings another
    generation = SDL_GetAtomicInt(&s_firmware_generation);

    status = QuerySlotPath(hwdata->userid, path, SDL_arraysize(path));
    if (status == ERROR_BUSY) {
        // OpenXInput's lock was held. The next update asks again.
        return;
    }
    SDL_zero(container);
    if (status == ERROR_SUCCESS) {
#ifdef SDL_JOYSTICK_XINPUT_SLOT_RECORD
        SDL_XInputSlotRecord slot;

        if (SDL_XINPUT_ReadSlotRecord(path, &slot)) {
            container = slot.container;
            have_container = ValidContainer(&container);
            record = slot.firmware;
        }
#endif
        if (s_firmware_gameinput) {
            found = FindFirmwareDevice(path, have_container ? &container : NULL, &version);
        }
    }

    // GameInput's version, or the metadata's while GameInput has none
    joystick->firmware_version = version ? version : record;
    if (found || !s_firmware_gameinput) {
        hwdata->firmware_settled = true;
    } else {
        hwdata->firmware_generation = generation;
    }
}

// Runs without the joystick lock, so it can wait for any GameInput callback
static int SDLCALL RegisterFirmwareCallback(void *unused)
{
    GameInputCallbackToken token = 0;

    (void)unused;
    if (SUCCEEDED(s_firmware_gameinput->RegisterDeviceCallback(NULL, GameInputKindController, NULL, XInputFirmwareDeviceCallback, &token))) {
        SDL_SetAtomicInt(&s_firmware_registration, FIRMWARE_REGISTERED);
    } else {
        // The next start of the XInput driver tries again
        SDL_SetAtomicInt(&s_firmware_registration, FIRMWARE_UNREGISTERED);
    }
    return 0;
}

void SDL_XINPUT_FirmwareInit(void)
{
    SDL_Thread *thread;

    if (!s_firmware_lock) {
        s_firmware_lock = SDL_CreateMutex();
        if (!s_firmware_lock) {
            return;
        }
    }
    // Under the joystick lock, which guards SDL's GameInput reference count
    if (!s_firmware_gameinput && !SDL_InitGameInput(&s_firmware_gameinput)) {
        // No GameInput runtime with the v4 interface: every version stays 0
        s_firmware_gameinput = NULL;
        return;
    }
    if (SDL_CompareAndSwapAtomicInt(&s_firmware_registration, FIRMWARE_UNREGISTERED, FIRMWARE_REGISTERING)) {
        thread = SDL_CreateThread(RegisterFirmwareCallback, "SDLXInputFirmware", NULL);
        if (thread) {
            SDL_DetachThread(thread);
        } else {
            SDL_SetAtomicInt(&s_firmware_registration, FIRMWARE_UNREGISTERED);
        }
    }
}

void SDL_XINPUT_FirmwareOpen(SDL_Joystick *joystick)
{
    joystick->hwdata->firmware_settled = false;
    joystick->hwdata->firmware_generation = -1;
    LookUpFirmwareVersion(joystick);
}

// A version GameInput lists after the open reaches the open joystick here
void SDL_XINPUT_FirmwareUpdate(SDL_Joystick *joystick)
{
    struct joystick_hwdata *hwdata = joystick->hwdata;

    if (!hwdata->firmware_settled &&
        hwdata->firmware_generation != SDL_GetAtomicInt(&s_firmware_generation)) {
        LookUpFirmwareVersion(joystick);
    }
}

// The slot's device left. Look again once the slot answers, in case another device took it.
void SDL_XINPUT_FirmwareLost(SDL_Joystick *joystick)
{
    joystick->hwdata->firmware_settled = false;
    joystick->hwdata->firmware_generation = -1;
}

/* Nothing to undo. UnregisterCallback here, under the joystick lock, would
   wait on any GameInput callback running in the process, forever on one
   that waits on the joystick lock. The list stays current for the next
   start. */
void SDL_XINPUT_FirmwareQuit(void)
{
}

#else

void SDL_XINPUT_FirmwareInit(void)
{
}

void SDL_XINPUT_FirmwareOpen(SDL_Joystick *joystick)
{
    (void)joystick;
}

void SDL_XINPUT_FirmwareUpdate(SDL_Joystick *joystick)
{
    (void)joystick;
}

void SDL_XINPUT_FirmwareLost(SDL_Joystick *joystick)
{
    (void)joystick;
}

void SDL_XINPUT_FirmwareQuit(void)
{
}

#endif // HAVE_GAMEINPUT_H && GAMEINPUT_API_VERSION >= 4

#endif // SDL_JOYSTICK_XINPUT
