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
#include "SDL_hidapi_guncon_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_GUNCON

/* The Namco GunCon 2, 0B9A:016A, and the EMS LCD TopGun that shares its ID
 * (hifihedgehog/SDL#33 Part 9): one vendor-class interface, read through
 * libusb once WinUSB is bound. The mode request goes out once as a
 * SET_REPORT through hid_write, and the reports carry the raw beam position,
 * which the application calibrates to its screen. The GunCon 3 is not
 * claimed: its reports are encrypted with a table no zlib-compatible source
 * carries. The protocol lives in SDL_hidapi_guncon_proto.c, where the
 * offline tests run it. */

SDL_COMPILE_TIME_ASSERT(guncon_hat_up, SDL_GUNCON_HAT_UP == SDL_HAT_UP);
SDL_COMPILE_TIME_ASSERT(guncon_hat_right, SDL_GUNCON_HAT_RIGHT == SDL_HAT_RIGHT);
SDL_COMPILE_TIME_ASSERT(guncon_hat_down, SDL_GUNCON_HAT_DOWN == SDL_HAT_DOWN);
SDL_COMPILE_TIME_ASSERT(guncon_hat_left, SDL_GUNCON_HAT_LEFT == SDL_HAT_LEFT);

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_GunCon2State state;
    bool post_pending; /* The joystick opened and has not had the state yet */
} SDL_DriverGunCon_Context;

static void HIDAPI_DriverGunCon_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_GUNCON, callback, userdata);
}

static void HIDAPI_DriverGunCon_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_GUNCON, callback, userdata);
}

static bool HIDAPI_DriverGunCon_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_GUNCON, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverGunCon_Write(void *userdata, const uint8_t *data, size_t length)
{
    SDL_DriverGunCon_Context *ctx = (SDL_DriverGunCon_Context *)userdata;

    return SDL_hid_write(ctx->device->dev, data, length) >= 0;
}

static bool HIDAPI_DriverGunCon_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    /* Only the vendor-class interface the libusb backend reports */
    return vendor_id == USB_VENDOR_NAMCO && product_id == USB_PRODUCT_NAMCO_GUNCON2 &&
           interface_number == 0 && interface_class == 0xFF;
}

static bool HIDAPI_DriverGunCon_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverGunCon_Context *ctx = (SDL_DriverGunCon_Context *)SDL_calloc(1, sizeof(*ctx));
    SDL_GunConSink sink;

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;
    SDL_GunCon2_ResetState(&ctx->state);

    HIDAPI_SetDeviceName(device, "Namco GunCon 2");
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;

    /* The mode request goes out before this driver reads a report, as the
       tools that read the gun send it. A failed request is not retried: the
       TopGun's own driver reads with no request at all. */
    sink.userdata = ctx;
    sink.write = HIDAPI_DriverGunCon_Write;
    SDL_GunCon2_Open(&sink);
    return HIDAPI_JoystickConnected(device, NULL);
}

static void HIDAPI_DriverGunCon_Post(SDL_DriverGunCon_Context *ctx, SDL_Joystick *joystick)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    Uint8 i;

    for (i = 0; i < SDL_GUNCON2_AXES; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, i, ctx->state.axes[i]);
    }
    for (i = 0; i < SDL_GUNCON2_BUTTONS; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, i, (ctx->state.buttons & (1u << i)) != 0);
    }
    SDL_SendJoystickHat(timestamp, joystick, 0, ctx->state.hat);
}

static bool HIDAPI_DriverGunCon_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverGunCon_Context *ctx = (SDL_DriverGunCon_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }
    if (joystick && ctx->post_pending) {
        HIDAPI_DriverGunCon_Post(ctx, joystick);
        ctx->post_pending = false;
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        if (SDL_GunCon2_Decode(data, (size_t)size, &ctx->state) && joystick) {
            HIDAPI_DriverGunCon_Post(ctx, joystick);
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

static bool HIDAPI_DriverGunCon_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverGunCon_Context *ctx = (SDL_DriverGunCon_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->naxes = SDL_GUNCON2_AXES;
    joystick->nbuttons = SDL_GUNCON2_BUTTONS;
    joystick->nhats = 1;
    // The axes are allocated after this returns, so the next update sends the state
    ctx->post_pending = true;
    return true;
}

static int HIDAPI_DriverGunCon_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverGunCon_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverGunCon_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverGunCon_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverGunCon_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverGunCon_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverGunCon_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverGunCon_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverGunCon_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverGunCon_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverGunCon = {
    SDL_HINT_JOYSTICK_HIDAPI_GUNCON,
    true,
    HIDAPI_DriverGunCon_RegisterHints,
    HIDAPI_DriverGunCon_UnregisterHints,
    HIDAPI_DriverGunCon_IsEnabled,
    HIDAPI_DriverGunCon_IsSupportedDevice,
    HIDAPI_DriverGunCon_InitDevice,
    HIDAPI_DriverGunCon_GetDevicePlayerIndex,
    HIDAPI_DriverGunCon_SetDevicePlayerIndex,
    HIDAPI_DriverGunCon_UpdateDevice,
    HIDAPI_DriverGunCon_OpenJoystick,
    HIDAPI_DriverGunCon_RumbleJoystick,
    HIDAPI_DriverGunCon_RumbleJoystickTriggers,
    HIDAPI_DriverGunCon_GetJoystickCapabilities,
    HIDAPI_DriverGunCon_SetJoystickLED,
    HIDAPI_DriverGunCon_SendJoystickEffect,
    HIDAPI_DriverGunCon_SetJoystickSensorsEnabled,
    HIDAPI_DriverGunCon_CloseJoystick,
    HIDAPI_DriverGunCon_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_GUNCON

#endif // SDL_JOYSTICK_HIDAPI
