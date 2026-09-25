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
#include "SDL_hidapi_speedforce_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_SPEEDFORCE

/* The Logitech Speed Force Wireless for the Wii, 046D:C29C, on its USB
 * receiver. The receiver does not bond with the wheel until the host sends
 * feature AF and, 40 ms later, feature B2 with two random address bytes, as
 * Linux does, and nothing on Windows sends them. Autocentering then goes
 * off. Input is a 5-byte report without a report ID: a 10-bit wheel, 11
 * buttons and two pedal bytes. The protocol lives in
 * SDL_hidapi_speedforce_proto.c, where the offline tests run it. */

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_SpeedForceBond bond;
    SDL_SpeedForceState state;
    bool post_pending; /* The joystick opened and has not had the state yet */
} SDL_DriverSpeedForce_Context;

static void HIDAPI_DriverSpeedForce_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_SPEEDFORCE, callback, userdata);
}

static void HIDAPI_DriverSpeedForce_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_SPEEDFORCE, callback, userdata);
}

static bool HIDAPI_DriverSpeedForce_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_SPEEDFORCE, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverSpeedForce_Feature(void *userdata, const uint8_t *data, size_t length)
{
    SDL_DriverSpeedForce_Context *ctx = (SDL_DriverSpeedForce_Context *)userdata;

    return SDL_hid_send_feature_report(ctx->device->dev, data, length) >= 0;
}

static bool HIDAPI_DriverSpeedForce_Output(void *userdata, const uint8_t *data, size_t length)
{
    SDL_DriverSpeedForce_Context *ctx = (SDL_DriverSpeedForce_Context *)userdata;

    return SDL_hid_write(ctx->device->dev, data, length) >= 0;
}

static uint8_t HIDAPI_DriverSpeedForce_Random(void *userdata)
{
    return (uint8_t)SDL_rand_bits();
}

static void HIDAPI_DriverSpeedForce_GetSink(SDL_DriverSpeedForce_Context *ctx, SDL_SpeedForceSink *sink)
{
    sink->userdata = ctx;
    sink->feature = HIDAPI_DriverSpeedForce_Feature;
    sink->output = HIDAPI_DriverSpeedForce_Output;
    sink->random = HIDAPI_DriverSpeedForce_Random;
}

static bool HIDAPI_DriverSpeedForce_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return vendor_id == USB_VENDOR_LOGITECH && product_id == USB_PRODUCT_LOGITECH_SPEED_FORCE_WIRELESS;
}

static bool HIDAPI_DriverSpeedForce_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverSpeedForce_Context *ctx = (SDL_DriverSpeedForce_Context *)SDL_calloc(1, sizeof(*ctx));
    SDL_SpeedForceSink sink;

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    SDL_SpeedForce_RestState(&ctx->state);
    device->context = ctx;

    HIDAPI_SetDeviceName(device, "Logitech Speed Force Wireless");
    device->joystick_type = SDL_JOYSTICK_TYPE_WHEEL;

    HIDAPI_DriverSpeedForce_GetSink(ctx, &sink);
    SDL_SpeedForce_StartBond(&ctx->bond, SDL_GetTicks(), &sink);
    return HIDAPI_JoystickConnected(device, NULL);
}

static void HIDAPI_DriverSpeedForce_Post(SDL_Joystick *joystick, const SDL_SpeedForceState *state)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    Uint8 i;

    for (i = 0; i < SDL_SPEEDFORCE_AXES; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, i, state->axes[i]);
    }
    for (i = 0; i < SDL_SPEEDFORCE_BUTTONS; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, i, (state->buttons & (1u << i)) != 0);
    }
}

static bool HIDAPI_DriverSpeedForce_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverSpeedForce_Context *ctx = (SDL_DriverSpeedForce_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    SDL_SpeedForceSink sink;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    HIDAPI_DriverSpeedForce_GetSink(ctx, &sink);
    (void)SDL_SpeedForce_UpdateBond(&ctx->bond, SDL_GetTicks(), &sink);

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }
    if (joystick && ctx->post_pending) {
        // The pedals rest at 32767, which the joystick can only learn from an event
        HIDAPI_DriverSpeedForce_Post(joystick, &ctx->state);
        ctx->post_pending = false;
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        if (SDL_SpeedForce_DecodeReport(data, (size_t)size, &ctx->state) && joystick) {
            HIDAPI_DriverSpeedForce_Post(joystick, &ctx->state);
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

static bool HIDAPI_DriverSpeedForce_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverSpeedForce_Context *ctx = (SDL_DriverSpeedForce_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->naxes = SDL_SPEEDFORCE_AXES;
    joystick->nbuttons = SDL_SPEEDFORCE_BUTTONS;
    ctx->post_pending = true;
    return true;
}

static int HIDAPI_DriverSpeedForce_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverSpeedForce_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverSpeedForce_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverSpeedForce_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverSpeedForce_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverSpeedForce_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverSpeedForce_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverSpeedForce_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverSpeedForce_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverSpeedForce_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverSpeedForce = {
    SDL_HINT_JOYSTICK_HIDAPI_SPEEDFORCE,
    true,
    HIDAPI_DriverSpeedForce_RegisterHints,
    HIDAPI_DriverSpeedForce_UnregisterHints,
    HIDAPI_DriverSpeedForce_IsEnabled,
    HIDAPI_DriverSpeedForce_IsSupportedDevice,
    HIDAPI_DriverSpeedForce_InitDevice,
    HIDAPI_DriverSpeedForce_GetDevicePlayerIndex,
    HIDAPI_DriverSpeedForce_SetDevicePlayerIndex,
    HIDAPI_DriverSpeedForce_UpdateDevice,
    HIDAPI_DriverSpeedForce_OpenJoystick,
    HIDAPI_DriverSpeedForce_RumbleJoystick,
    HIDAPI_DriverSpeedForce_RumbleJoystickTriggers,
    HIDAPI_DriverSpeedForce_GetJoystickCapabilities,
    HIDAPI_DriverSpeedForce_SetJoystickLED,
    HIDAPI_DriverSpeedForce_SendJoystickEffect,
    HIDAPI_DriverSpeedForce_SetJoystickSensorsEnabled,
    HIDAPI_DriverSpeedForce_CloseJoystick,
    HIDAPI_DriverSpeedForce_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_SPEEDFORCE

#endif // SDL_JOYSTICK_HIDAPI
