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
#include "SDL_hidapi_gametrak_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_GAMETRAK

/* The In2Games Gametrak for PlayStation, 14B7:0982: two tethers, each with
 * an X and Y angle and a pulled length, a hat and 12 buttons in a 16-byte
 * report without a report ID. The unit stays silent until the host writes
 * "Gametrak" and then 45 and a key byte, and it wants 46 and the next key
 * byte after every 100 reports. hid.dll cannot send those writes, so on
 * Windows the Gametrak is read through libusb once WinUSB is bound (see
 * docs/README-vendor-usb.md). The protocol lives in
 * SDL_hidapi_gametrak_proto.c, where the offline tests run it. */

SDL_COMPILE_TIME_ASSERT(gametrak_hat_up, SDL_HAT_UP == 0x01);
SDL_COMPILE_TIME_ASSERT(gametrak_hat_right, SDL_HAT_RIGHT == 0x02);
SDL_COMPILE_TIME_ASSERT(gametrak_hat_down, SDL_HAT_DOWN == 0x04);
SDL_COMPILE_TIME_ASSERT(gametrak_hat_left, SDL_HAT_LEFT == 0x08);

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_GametrakSession session;
} SDL_DriverGametrak_Context;

static void HIDAPI_DriverGametrak_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_GAMETRAK, callback, userdata);
}

static void HIDAPI_DriverGametrak_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_GAMETRAK, callback, userdata);
}

static bool HIDAPI_DriverGametrak_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_GAMETRAK, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverGametrak_Write(void *userdata, const uint8_t *data, size_t length)
{
    SDL_DriverGametrak_Context *ctx = (SDL_DriverGametrak_Context *)userdata;

    return SDL_hid_write(ctx->device->dev, data, length) >= 0;
}

static bool HIDAPI_DriverGametrak_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return vendor_id == USB_VENDOR_IN2GAMES && product_id == USB_PRODUCT_IN2GAMES_GAMETRAK;
}

static bool HIDAPI_DriverGametrak_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverGametrak_Context *ctx = (SDL_DriverGametrak_Context *)SDL_calloc(1, sizeof(*ctx));
    SDL_GametrakSink sink;

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;

    HIDAPI_SetDeviceName(device, "Gametrak");
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;

    sink.userdata = ctx;
    sink.write = HIDAPI_DriverGametrak_Write;
    SDL_Gametrak_Open(&ctx->session, SDL_GetTicks(), &sink);
    return HIDAPI_JoystickConnected(device, NULL);
}

static bool HIDAPI_DriverGametrak_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverGametrak_Context *ctx = (SDL_DriverGametrak_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    SDL_GametrakSink sink;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    sink.userdata = ctx;
    sink.write = HIDAPI_DriverGametrak_Write;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        SDL_GametrakReport report;

        if (SDL_Gametrak_HandleReport(&ctx->session, SDL_GetTicks(), data, (size_t)size, &sink, &report) && joystick) {
            const Uint64 timestamp = SDL_GetTicksNS();
            Uint8 i;

            for (i = 0; i < SDL_GAMETRAK_AXES; ++i) {
                SDL_SendJoystickAxis(timestamp, joystick, i, report.axes[i]);
            }
            for (i = 0; i < SDL_GAMETRAK_BUTTONS; ++i) {
                SDL_SendJoystickButton(timestamp, joystick, i, (report.buttons & (1u << i)) != 0);
            }
            SDL_SendJoystickHat(timestamp, joystick, 0, report.hat);
        }
    }

    if (size < 0) {
        // Read error, device is disconnected
        if (device->num_joysticks > 0) {
            HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        }
        return false;
    }

    // The key write goes out 10 ms after "Gametrak" when no answer has come
    SDL_Gametrak_Update(&ctx->session, SDL_GetTicks(), &sink);
    return true;
}

static bool HIDAPI_DriverGametrak_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_AssertJoysticksLocked();

    joystick->naxes = SDL_GAMETRAK_AXES;
    joystick->nbuttons = SDL_GAMETRAK_BUTTONS;
    joystick->nhats = 1;
    return true;
}

static int HIDAPI_DriverGametrak_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverGametrak_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverGametrak_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverGametrak_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverGametrak_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverGametrak_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverGametrak_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverGametrak_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverGametrak_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverGametrak_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverGametrak = {
    SDL_HINT_JOYSTICK_HIDAPI_GAMETRAK,
    true,
    HIDAPI_DriverGametrak_RegisterHints,
    HIDAPI_DriverGametrak_UnregisterHints,
    HIDAPI_DriverGametrak_IsEnabled,
    HIDAPI_DriverGametrak_IsSupportedDevice,
    HIDAPI_DriverGametrak_InitDevice,
    HIDAPI_DriverGametrak_GetDevicePlayerIndex,
    HIDAPI_DriverGametrak_SetDevicePlayerIndex,
    HIDAPI_DriverGametrak_UpdateDevice,
    HIDAPI_DriverGametrak_OpenJoystick,
    HIDAPI_DriverGametrak_RumbleJoystick,
    HIDAPI_DriverGametrak_RumbleJoystickTriggers,
    HIDAPI_DriverGametrak_GetJoystickCapabilities,
    HIDAPI_DriverGametrak_SetJoystickLED,
    HIDAPI_DriverGametrak_SendJoystickEffect,
    HIDAPI_DriverGametrak_SetJoystickSensorsEnabled,
    HIDAPI_DriverGametrak_CloseJoystick,
    HIDAPI_DriverGametrak_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_GAMETRAK

#endif // SDL_JOYSTICK_HIDAPI
