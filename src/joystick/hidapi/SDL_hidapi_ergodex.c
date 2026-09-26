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
#include "SDL_hidapi_ergodex_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_ERGODEX

/* The Ergodex DX1, 1603:0002 (hifihedgehog/SDL#33 Part 14). Its vendor
 * interface, 1, is read through libusb once WinUSB is bound to it, and its
 * keyboard interface, 0, stays with the system. The pad forgets its keys at
 * power-off, so every connection programs them again: the start-up goes out
 * one output per update, at least 10 ms apart on SDL's clock, from
 * UpdateDevice, which runs for every device whether or not a joystick is
 * open. The protocol lives in SDL_hidapi_ergodex_proto.c, where the offline
 * tests run it. */

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_ErgodexState state;
    bool post_pending; /* The joystick opened and has not had the state yet */
} SDL_DriverErgodex_Context;

static void HIDAPI_DriverErgodex_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_ERGODEX, callback, userdata);
}

static void HIDAPI_DriverErgodex_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_ERGODEX, callback, userdata);
}

static bool HIDAPI_DriverErgodex_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_ERGODEX, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverErgodex_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    /* Only the vendor interface, never the keyboard on interface 0 */
    return vendor_id == USB_VENDOR_ERGODEX && product_id == USB_PRODUCT_ERGODEX_DX1 &&
           interface_number == 1 && interface_class == 0xFF;
}

static bool HIDAPI_DriverErgodex_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverErgodex_Context *ctx = (SDL_DriverErgodex_Context *)SDL_calloc(1, sizeof(*ctx));

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;
    SDL_Ergodex_Init(&ctx->state, SDL_GetTicksNS());

    HIDAPI_SetDeviceName(device, "Ergodex DX1");
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;
    return HIDAPI_JoystickConnected(device, NULL);
}

static void HIDAPI_DriverErgodex_Post(SDL_DriverErgodex_Context *ctx, SDL_Joystick *joystick)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    int i;

    for (i = 0; i < SDL_ERGODEX_BUTTONS; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, ((ctx->state.buttons >> i) & 1) != 0);
    }
}

static bool HIDAPI_DriverErgodex_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverErgodex_Context *ctx = (SDL_DriverErgodex_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    Uint8 output[SDL_ERGODEX_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }
    if (joystick && ctx->post_pending) {
        HIDAPI_DriverErgodex_Post(ctx, joystick);
        ctx->post_pending = false;
    }

    /* The pad sends messages nobody asked for, so every read is taken on
       its own and dispatched on its type */
    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        const int packet = SDL_Ergodex_HandlePacket(&ctx->state, data, (size_t)size);

        if (packet == SDL_ERGODEX_PACKET_DEVICE) {
            char serial[17];

            // The USB descriptor has no serial string, so this tells two pads apart
            SDL_Ergodex_FormatSerial(ctx->state.serial, serial);
            HIDAPI_SetDeviceSerial(device, serial);
        } else if ((packet == SDL_ERGODEX_PACKET_STATUS || packet == SDL_ERGODEX_PACKET_KEYS) && joystick) {
            HIDAPI_DriverErgodex_Post(ctx, joystick);
        }
    }

    if (size < 0) {
        // Read error, device is disconnected
        if (device->num_joysticks > 0) {
            HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        }
        return false;
    }

    /* At most one start-up output per update. The module holds the next one
       back until 10 ms after the last write completed. */
    if (SDL_Ergodex_NextOutput(&ctx->state, SDL_GetTicksNS(), output)) {
        const int written = SDL_hid_write(device->dev, output, sizeof(output));
        SDL_Ergodex_OutputDone(&ctx->state, written, SDL_GetTicksNS());
    }
    return true;
}

static bool HIDAPI_DriverErgodex_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverErgodex_Context *ctx = (SDL_DriverErgodex_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->nbuttons = SDL_ERGODEX_BUTTONS;
    // The buttons are allocated after this returns, so the next update sends the state
    ctx->post_pending = true;
    return true;
}

static int HIDAPI_DriverErgodex_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverErgodex_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverErgodex_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverErgodex_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverErgodex_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverErgodex_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    // The two sources disagree on what the LED bits light
    return SDL_Unsupported();
}

static bool HIDAPI_DriverErgodex_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverErgodex_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverErgodex_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverErgodex_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverErgodex = {
    SDL_HINT_JOYSTICK_HIDAPI_ERGODEX,
    true,
    HIDAPI_DriverErgodex_RegisterHints,
    HIDAPI_DriverErgodex_UnregisterHints,
    HIDAPI_DriverErgodex_IsEnabled,
    HIDAPI_DriverErgodex_IsSupportedDevice,
    HIDAPI_DriverErgodex_InitDevice,
    HIDAPI_DriverErgodex_GetDevicePlayerIndex,
    HIDAPI_DriverErgodex_SetDevicePlayerIndex,
    HIDAPI_DriverErgodex_UpdateDevice,
    HIDAPI_DriverErgodex_OpenJoystick,
    HIDAPI_DriverErgodex_RumbleJoystick,
    HIDAPI_DriverErgodex_RumbleJoystickTriggers,
    HIDAPI_DriverErgodex_GetJoystickCapabilities,
    HIDAPI_DriverErgodex_SetJoystickLED,
    HIDAPI_DriverErgodex_SendJoystickEffect,
    HIDAPI_DriverErgodex_SetJoystickSensorsEnabled,
    HIDAPI_DriverErgodex_CloseJoystick,
    HIDAPI_DriverErgodex_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_ERGODEX

#endif // SDL_JOYSTICK_HIDAPI
