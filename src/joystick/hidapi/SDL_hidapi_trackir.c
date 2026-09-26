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
#include "SDL_hidapi_trackir_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_TRACKIR

/* The NaturalPoint TrackIR 2, 131D:0150, and TrackIR 3, 131D:0155
 * (hifihedgehog/SDL#33 Part 14): one vendor interface, read through libusb
 * once WinUSB is bound. Commands go out on bulk OUT 0x02 through hid_write,
 * and the vendor rule reads bulk IN 0x82 16384 bytes at a time, as
 * linuxtrack does. The camera starts on a timed command sequence, which runs
 * in UpdateDevice, since SDL calls that for every device, and stops on
 * another in FreeDevice. The protocol lives in SDL_hidapi_trackir_proto.c,
 * where the offline tests run it. */

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_TrackIRState state;
    bool post_pending; /* The joystick opened and has not had the state yet */
    bool gone;         /* A read failed, so nothing more can be sent */
    /* One whole transfer. hid_read cuts a report to the buffer it is given. */
    Uint8 data[SDL_TRACKIR_READ_SIZE];
} SDL_DriverTrackIR_Context;

static void HIDAPI_DriverTrackIR_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_TRACKIR, callback, userdata);
}

static void HIDAPI_DriverTrackIR_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_TRACKIR, callback, userdata);
}

static bool HIDAPI_DriverTrackIR_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_TRACKIR, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverTrackIR_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return interface_number == 0 && SDL_TrackIR_Identify(vendor_id, product_id) != SDL_TRACKIR_NONE;
}

static bool HIDAPI_DriverTrackIR_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverTrackIR_Context *ctx = (SDL_DriverTrackIR_Context *)SDL_calloc(1, sizeof(*ctx));
    SDL_TrackIRModel model;

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;
    model = SDL_TrackIR_Identify(device->vendor_id, device->product_id);
    SDL_TrackIR_Init(&ctx->state, model, SDL_GetTicks());

    HIDAPI_SetDeviceName(device, SDL_TrackIR_Name(model));
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;

    /* The first command goes out on the first update */
    return HIDAPI_JoystickConnected(device, NULL);
}

static int HIDAPI_DriverTrackIR_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverTrackIR_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static void HIDAPI_DriverTrackIR_Post(SDL_DriverTrackIR_Context *ctx, SDL_Joystick *joystick)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    Uint8 i;

    for (i = 0; i < SDL_TRACKIR_AXES; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, i, ctx->state.axes[i]);
    }
    SDL_SendJoystickButton(timestamp, joystick, 0, ctx->state.in_view);
}

/* Sends every command that is due, each after the one before it completes */
static void HIDAPI_DriverTrackIR_SendCommands(SDL_DriverTrackIR_Context *ctx)
{
    SDL_TrackIRCommand command;

    while (SDL_TrackIR_NextCommand(&ctx->state, SDL_GetTicks(), &command)) {
        const int written = SDL_hid_write(ctx->device->dev, command.data, command.length);

        SDL_TrackIR_CommandDone(&ctx->state, written == (int)command.length, SDL_GetTicks());
    }
}

static bool HIDAPI_DriverTrackIR_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverTrackIR_Context *ctx = (SDL_DriverTrackIR_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }
    if (joystick && ctx->post_pending) {
        HIDAPI_DriverTrackIR_Post(ctx, joystick);
        ctx->post_pending = false;
    }

    while ((size = SDL_hid_read_timeout(device->dev, ctx->data, sizeof(ctx->data), 0)) > 0) {
        if (SDL_TrackIR_Feed(&ctx->state, ctx->data, (size_t)size, SDL_GetTicks()) && joystick) {
            HIDAPI_DriverTrackIR_Post(ctx, joystick);
        }
    }

    if (size < 0) {
        // Read error, device is disconnected
        ctx->gone = true;
        if (device->num_joysticks > 0) {
            HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        }
        return false;
    }

    /* The reads come first, so an answer that arrived is seen before a
       timer that would give up on it */
    HIDAPI_DriverTrackIR_SendCommands(ctx);
    return true;
}

static bool HIDAPI_DriverTrackIR_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverTrackIR_Context *ctx = (SDL_DriverTrackIR_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->naxes = SDL_TRACKIR_AXES;
    joystick->nbuttons = SDL_TRACKIR_BUTTONS;
    // The axes are allocated after this returns, so the next update sends the state
    ctx->post_pending = true;
    return true;
}

static bool HIDAPI_DriverTrackIR_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverTrackIR_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverTrackIR_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverTrackIR_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverTrackIR_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverTrackIR_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverTrackIR_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverTrackIR_FreeDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverTrackIR_Context *ctx = (SDL_DriverTrackIR_Context *)device->context;
    Uint64 now, deadline;

    if (!ctx || ctx->gone || !device->dev) {
        return;
    }

    /* The stop sequence linuxtrack sends when it closes the camera, which
       turns the infrared lights off. It waits out its delays here, 401 ms on
       the TrackIR 2 and 109 ms on the TrackIR 3, and ends at the first
       failed write. */
    SDL_TrackIR_BeginClose(&ctx->state, SDL_GetTicks());
    for (;;) {
        HIDAPI_DriverTrackIR_SendCommands(ctx);
        if (SDL_TrackIR_IsClosed(&ctx->state) || !SDL_TrackIR_GetDeadline(&ctx->state, &deadline)) {
            break;
        }
        now = SDL_GetTicks();
        if (deadline > now) {
            SDL_Delay((Uint32)(deadline - now));
        }
    }
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverTrackIR = {
    SDL_HINT_JOYSTICK_HIDAPI_TRACKIR,
    true,
    HIDAPI_DriverTrackIR_RegisterHints,
    HIDAPI_DriverTrackIR_UnregisterHints,
    HIDAPI_DriverTrackIR_IsEnabled,
    HIDAPI_DriverTrackIR_IsSupportedDevice,
    HIDAPI_DriverTrackIR_InitDevice,
    HIDAPI_DriverTrackIR_GetDevicePlayerIndex,
    HIDAPI_DriverTrackIR_SetDevicePlayerIndex,
    HIDAPI_DriverTrackIR_UpdateDevice,
    HIDAPI_DriverTrackIR_OpenJoystick,
    HIDAPI_DriverTrackIR_RumbleJoystick,
    HIDAPI_DriverTrackIR_RumbleJoystickTriggers,
    HIDAPI_DriverTrackIR_GetJoystickCapabilities,
    HIDAPI_DriverTrackIR_SetJoystickLED,
    HIDAPI_DriverTrackIR_SendJoystickEffect,
    HIDAPI_DriverTrackIR_SetJoystickSensorsEnabled,
    HIDAPI_DriverTrackIR_CloseJoystick,
    HIDAPI_DriverTrackIR_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_TRACKIR

#endif // SDL_JOYSTICK_HIDAPI
