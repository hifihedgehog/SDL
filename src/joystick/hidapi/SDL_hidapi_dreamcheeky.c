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

#ifdef SDL_JOYSTICK_HIDAPI

#include "../../SDL_hints_c.h"
#include "../SDL_sysjoystick.h"
#include "SDL_hidapijoystick_c.h"
#include "../../hidapi/SDL_hidapi_c.h"
#include "SDL_hidapi_dreamcheeky_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_DREAMCHEEKY

/* The Dream Cheeky roll-up drum kit, 1941:8021: six pads in an 8-byte
 * report whose eight bytes each carry the pad bits. A pad counts as held
 * when its bit is set in at least 4 of the 8 bytes, VRPN's debouncing. A
 * weather station and a missile launcher share the ID, so the driver runs
 * only when SDL_HINT_JOYSTICK_HIDAPI_DREAMCHEEKY turns it on. The protocol
 * lives in SDL_hidapi_dreamcheeky_proto.c, where the offline tests run it. */

static void HIDAPI_DriverDreamCheeky_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_DREAMCHEEKY, callback, userdata);
}

static void HIDAPI_DriverDreamCheeky_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_DREAMCHEEKY, callback, userdata);
}

static bool HIDAPI_DriverDreamCheeky_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_DREAMCHEEKY, false);
}

static bool HIDAPI_DriverDreamCheeky_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return vendor_id == USB_VENDOR_DREAMCHEEKY && product_id == USB_PRODUCT_DREAMCHEEKY_DRUM_KIT;
}

static bool HIDAPI_DriverDreamCheeky_InitDevice(SDL_HIDAPI_Device *device)
{
    HIDAPI_SetDeviceName(device, "Dream Cheeky Roll-Up Drum Kit");
    device->joystick_type = SDL_JOYSTICK_TYPE_DRUM_KIT;
    return HIDAPI_JoystickConnected(device, NULL);
}

static bool HIDAPI_DriverDreamCheeky_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        Uint8 pads;

        if (joystick && SDL_DreamCheeky_DecodeReport(data, (size_t)size, &pads)) {
            const Uint64 timestamp = SDL_GetTicksNS();
            Uint8 i;

            for (i = 0; i < SDL_DREAMCHEEKY_PADS; ++i) {
                SDL_SendJoystickButton(timestamp, joystick, i, (pads & (1u << i)) != 0);
            }
        }
    }

    if (size < 0) {
        // Read error, device is disconnected
        if (device->num_joysticks > 0) {
            HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        }
        return false;
    }
    return true;
}

static bool HIDAPI_DriverDreamCheeky_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_AssertJoysticksLocked();

    joystick->nbuttons = SDL_DREAMCHEEKY_PADS;
    return true;
}

static int HIDAPI_DriverDreamCheeky_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverDreamCheeky_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverDreamCheeky_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverDreamCheeky_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverDreamCheeky_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverDreamCheeky_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverDreamCheeky_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverDreamCheeky_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverDreamCheeky_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverDreamCheeky_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverDreamCheeky = {
    SDL_HINT_JOYSTICK_HIDAPI_DREAMCHEEKY,
    true,
    HIDAPI_DriverDreamCheeky_RegisterHints,
    HIDAPI_DriverDreamCheeky_UnregisterHints,
    HIDAPI_DriverDreamCheeky_IsEnabled,
    HIDAPI_DriverDreamCheeky_IsSupportedDevice,
    HIDAPI_DriverDreamCheeky_InitDevice,
    HIDAPI_DriverDreamCheeky_GetDevicePlayerIndex,
    HIDAPI_DriverDreamCheeky_SetDevicePlayerIndex,
    HIDAPI_DriverDreamCheeky_UpdateDevice,
    HIDAPI_DriverDreamCheeky_OpenJoystick,
    HIDAPI_DriverDreamCheeky_RumbleJoystick,
    HIDAPI_DriverDreamCheeky_RumbleJoystickTriggers,
    HIDAPI_DriverDreamCheeky_GetJoystickCapabilities,
    HIDAPI_DriverDreamCheeky_SetJoystickLED,
    HIDAPI_DriverDreamCheeky_SendJoystickEffect,
    HIDAPI_DriverDreamCheeky_SetJoystickSensorsEnabled,
    HIDAPI_DriverDreamCheeky_CloseJoystick,
    HIDAPI_DriverDreamCheeky_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_DREAMCHEEKY

#endif // SDL_JOYSTICK_HIDAPI
