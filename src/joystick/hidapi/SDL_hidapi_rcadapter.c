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
#include "SDL_hidapi_rcadapter_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_RC_ADAPTER

/* The PhoenixRC USB adapter, 1781:0898, which passes a radio-control
 * transmitter's channels as an 8-byte report under a descriptor that gives
 * Windows no usable joystick. Channels 7 and 8 take turns in byte 7. The
 * protocol lives in SDL_hidapi_rcadapter_proto.c, where the offline tests
 * run it. */

typedef struct
{
    SDL_RCAdapterState state;
} SDL_DriverRCAdapter_Context;

static void HIDAPI_DriverRCAdapter_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_RC_ADAPTER, callback, userdata);
}

static void HIDAPI_DriverRCAdapter_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_RC_ADAPTER, callback, userdata);
}

static bool HIDAPI_DriverRCAdapter_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_RC_ADAPTER, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverRCAdapter_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return vendor_id == USB_VENDOR_MULTIPLE_1781 && product_id == USB_PRODUCT_PHOENIXRC_ADAPTER;
}

static bool HIDAPI_DriverRCAdapter_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverRCAdapter_Context *ctx = (SDL_DriverRCAdapter_Context *)SDL_calloc(1, sizeof(*ctx));

    if (!ctx) {
        return false;
    }
    SDL_RCAdapter_Init(&ctx->state);
    device->context = ctx;

    HIDAPI_SetDeviceName(device, "PhoenixRC USB Adapter");
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;
    return HIDAPI_JoystickConnected(device, NULL);
}

static bool HIDAPI_DriverRCAdapter_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverRCAdapter_Context *ctx = (SDL_DriverRCAdapter_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        if (SDL_RCAdapter_HandleReport(&ctx->state, data, (size_t)size) && joystick) {
            const Uint64 timestamp = SDL_GetTicksNS();
            Uint8 i;

            for (i = 0; i < SDL_RCADAPTER_AXES; ++i) {
                SDL_SendJoystickAxis(timestamp, joystick, i, ctx->state.axes[i]);
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

static bool HIDAPI_DriverRCAdapter_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_AssertJoysticksLocked();

    joystick->naxes = SDL_RCADAPTER_AXES;
    return true;
}

static int HIDAPI_DriverRCAdapter_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverRCAdapter_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverRCAdapter_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverRCAdapter_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverRCAdapter_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverRCAdapter_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverRCAdapter_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverRCAdapter_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverRCAdapter_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverRCAdapter_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverRCAdapter = {
    SDL_HINT_JOYSTICK_HIDAPI_RC_ADAPTER,
    true,
    HIDAPI_DriverRCAdapter_RegisterHints,
    HIDAPI_DriverRCAdapter_UnregisterHints,
    HIDAPI_DriverRCAdapter_IsEnabled,
    HIDAPI_DriverRCAdapter_IsSupportedDevice,
    HIDAPI_DriverRCAdapter_InitDevice,
    HIDAPI_DriverRCAdapter_GetDevicePlayerIndex,
    HIDAPI_DriverRCAdapter_SetDevicePlayerIndex,
    HIDAPI_DriverRCAdapter_UpdateDevice,
    HIDAPI_DriverRCAdapter_OpenJoystick,
    HIDAPI_DriverRCAdapter_RumbleJoystick,
    HIDAPI_DriverRCAdapter_RumbleJoystickTriggers,
    HIDAPI_DriverRCAdapter_GetJoystickCapabilities,
    HIDAPI_DriverRCAdapter_SetJoystickLED,
    HIDAPI_DriverRCAdapter_SendJoystickEffect,
    HIDAPI_DriverRCAdapter_SetJoystickSensorsEnabled,
    HIDAPI_DriverRCAdapter_CloseJoystick,
    HIDAPI_DriverRCAdapter_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_RC_ADAPTER

#endif // SDL_JOYSTICK_HIDAPI
