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
#include "SDL_hidapi_nia_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_NIA

/* The OCZ Neural Impulse Actuator, 1234:0000, a headband whose 55-byte
 * report carries up to 16 samples of its 24-bit signal and a sample
 * counter. Hobby devices use the same placeholder ID, so the driver also
 * needs the manufacturer string "Brain Actuated Technologies". Axis 0 is
 * the newest sample. The protocol lives in SDL_hidapi_nia_proto.c, where
 * the offline tests run it. */

typedef struct
{
    SDL_NIAState state;
} SDL_DriverNIA_Context;

static void HIDAPI_DriverNIA_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_NIA, callback, userdata);
}

static void HIDAPI_DriverNIA_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_NIA, callback, userdata);
}

static bool HIDAPI_DriverNIA_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_NIA, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverNIA_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    if (vendor_id != USB_VENDOR_NIA || product_id != USB_PRODUCT_NIA) {
        return false;
    }
    // Without a device the question is only whether the ID could be one
    return !device || SDL_NIA_IsManufacturer(device->manufacturer_string);
}

static bool HIDAPI_DriverNIA_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverNIA_Context *ctx = (SDL_DriverNIA_Context *)SDL_calloc(1, sizeof(*ctx));

    if (!ctx) {
        return false;
    }
    SDL_NIA_Init(&ctx->state);
    device->context = ctx;

    HIDAPI_SetDeviceName(device, "OCZ Neural Impulse Actuator");
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;
    return HIDAPI_JoystickConnected(device, NULL);
}

static bool HIDAPI_DriverNIA_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverNIA_Context *ctx = (SDL_DriverNIA_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        SDL_NIAReport report;

        if (SDL_NIA_HandleReport(&ctx->state, data, (size_t)size, &report) && joystick) {
            SDL_SendJoystickAxis(SDL_GetTicksNS(), joystick, 0, ctx->state.axis);
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

static bool HIDAPI_DriverNIA_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_AssertJoysticksLocked();

    joystick->naxes = 1;
    return true;
}

static int HIDAPI_DriverNIA_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverNIA_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverNIA_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverNIA_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverNIA_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverNIA_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverNIA_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverNIA_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverNIA_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverNIA_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverNIA = {
    SDL_HINT_JOYSTICK_HIDAPI_NIA,
    true,
    HIDAPI_DriverNIA_RegisterHints,
    HIDAPI_DriverNIA_UnregisterHints,
    HIDAPI_DriverNIA_IsEnabled,
    HIDAPI_DriverNIA_IsSupportedDevice,
    HIDAPI_DriverNIA_InitDevice,
    HIDAPI_DriverNIA_GetDevicePlayerIndex,
    HIDAPI_DriverNIA_SetDevicePlayerIndex,
    HIDAPI_DriverNIA_UpdateDevice,
    HIDAPI_DriverNIA_OpenJoystick,
    HIDAPI_DriverNIA_RumbleJoystick,
    HIDAPI_DriverNIA_RumbleJoystickTriggers,
    HIDAPI_DriverNIA_GetJoystickCapabilities,
    HIDAPI_DriverNIA_SetJoystickLED,
    HIDAPI_DriverNIA_SendJoystickEffect,
    HIDAPI_DriverNIA_SetJoystickSensorsEnabled,
    HIDAPI_DriverNIA_CloseJoystick,
    HIDAPI_DriverNIA_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_NIA

#endif // SDL_JOYSTICK_HIDAPI
