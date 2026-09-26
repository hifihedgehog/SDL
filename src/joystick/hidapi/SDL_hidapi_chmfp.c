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
#include "SDL_hidapi_chmfp_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_CHMFP

/* The CH Products Multi-Function Panel, 068E:00F0 (hifihedgehog/SDL#33
 * Part 14): one vendor-class interface with one 8-byte interrupt IN
 * endpoint, read through libusb once WinUSB is bound. The panel needs no
 * command and the host never writes to it. The protocol lives in
 * SDL_hidapi_chmfp_proto.c, where the offline tests run it. */

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_CHMFPState state;
    bool post_pending; /* The joystick opened and has not had the state yet */
} SDL_DriverCHMFP_Context;

static void HIDAPI_DriverCHMFP_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_CHMFP, callback, userdata);
}

static void HIDAPI_DriverCHMFP_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_CHMFP, callback, userdata);
}

static bool HIDAPI_DriverCHMFP_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_CHMFP, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverCHMFP_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    /* Only the vendor-class interface the libusb backend reports */
    return vendor_id == USB_VENDOR_CH_PRODUCTS && product_id == USB_PRODUCT_CH_PRODUCTS_MFP &&
           interface_number == 0 && interface_class == 0xFF;
}

static bool HIDAPI_DriverCHMFP_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverCHMFP_Context *ctx = (SDL_DriverCHMFP_Context *)SDL_calloc(1, sizeof(*ctx));

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;
    SDL_CHMFP_ResetState(&ctx->state);

    HIDAPI_SetDeviceName(device, "CH Products Multi-Function Panel");
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;
    return HIDAPI_JoystickConnected(device, NULL);
}

static void HIDAPI_DriverCHMFP_Post(SDL_DriverCHMFP_Context *ctx, SDL_Joystick *joystick)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    int i;

    for (i = 0; i < SDL_CHMFP_BUTTONS; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, SDL_CHMFP_IsPressed(&ctx->state, i));
    }
}

static bool HIDAPI_DriverCHMFP_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverCHMFP_Context *ctx = (SDL_DriverCHMFP_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }
    if (joystick && ctx->post_pending) {
        HIDAPI_DriverCHMFP_Post(ctx, joystick);
        ctx->post_pending = false;
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        if (SDL_CHMFP_HandleReport(&ctx->state, data, (size_t)size) == SDL_CHMFP_REPORT_ACCEPTED && joystick) {
            HIDAPI_DriverCHMFP_Post(ctx, joystick);
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

static bool HIDAPI_DriverCHMFP_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverCHMFP_Context *ctx = (SDL_DriverCHMFP_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->nbuttons = SDL_CHMFP_BUTTONS;
    // The buttons are allocated after this returns, so the next update sends the state
    ctx->post_pending = true;
    return true;
}

static int HIDAPI_DriverCHMFP_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverCHMFP_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverCHMFP_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverCHMFP_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverCHMFP_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverCHMFP_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    // The panel's LED shows its mode, and the host cannot drive it
    return SDL_Unsupported();
}

static bool HIDAPI_DriverCHMFP_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverCHMFP_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverCHMFP_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverCHMFP_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverCHMFP = {
    SDL_HINT_JOYSTICK_HIDAPI_CHMFP,
    true,
    HIDAPI_DriverCHMFP_RegisterHints,
    HIDAPI_DriverCHMFP_UnregisterHints,
    HIDAPI_DriverCHMFP_IsEnabled,
    HIDAPI_DriverCHMFP_IsSupportedDevice,
    HIDAPI_DriverCHMFP_InitDevice,
    HIDAPI_DriverCHMFP_GetDevicePlayerIndex,
    HIDAPI_DriverCHMFP_SetDevicePlayerIndex,
    HIDAPI_DriverCHMFP_UpdateDevice,
    HIDAPI_DriverCHMFP_OpenJoystick,
    HIDAPI_DriverCHMFP_RumbleJoystick,
    HIDAPI_DriverCHMFP_RumbleJoystickTriggers,
    HIDAPI_DriverCHMFP_GetJoystickCapabilities,
    HIDAPI_DriverCHMFP_SetJoystickLED,
    HIDAPI_DriverCHMFP_SendJoystickEffect,
    HIDAPI_DriverCHMFP_SetJoystickSensorsEnabled,
    HIDAPI_DriverCHMFP_CloseJoystick,
    HIDAPI_DriverCHMFP_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_CHMFP

#endif // SDL_JOYSTICK_HIDAPI
