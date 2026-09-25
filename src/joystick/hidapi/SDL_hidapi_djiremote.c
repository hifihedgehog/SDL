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
#include "../dji/SDL_dji_remote_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_DJI_REMOTE

/* The DJI RC (RM330), 2CA3:1023, over its DUML bulk interface, which libusb
 * reads once WinUSB is bound (docs/README-dji-remotes.md). The bulk module
 * in src/joystick/dji runs here: its line action is done at once and its
 * writes go out as bulk transfers. The joystick appears with the first stick
 * report and goes after 1000 ms without one. The protocol and its timing
 * live in SDL_dji_remote_proto.c, where the offline tests run them. */

#define DJI_READ_SIZE   512 /* The bulk endpoint's packet size at high speed */
#define DJI_MAX_ACTIONS 16

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_DJIRemoteState state;
    SDL_SerialIdentity identity; /* Of the joystick the device has */
    bool send_snapshot;          /* A joystick opened since the last update */
} SDL_DriverDJIRemote_Context;

static void HIDAPI_DriverDJIRemote_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_DJI_REMOTE, callback, userdata);
}

static void HIDAPI_DriverDJIRemote_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_DJI_REMOTE, callback, userdata);
}

static bool HIDAPI_DriverDJIRemote_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_DJI_REMOTE, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverDJIRemote_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return vendor_id == USB_VENDOR_DJI && product_id == USB_PRODUCT_DJI_RC_RM330 &&
           interface_class == 0xFF && interface_subclass == 0x43;
}

static void HIDAPI_DriverDJIRemote_SendControls(SDL_Joystick *joystick, const SDL_SerialIdentity *identity, const SDL_SerialControls *controls)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    int i;

    for (i = 0; i < identity->naxes; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, controls->axes[i]);
    }
    for (i = 0; i < identity->nbuttons; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, SDL_Serial_GetButton(controls, i));
    }
}

/* The module's snapshots, in order, while it runs on this thread */
static void HIDAPI_DriverDJIRemote_Changed(void *userdata, int sub, const SDL_SerialSnapshot *snapshot)
{
    SDL_DriverDJIRemote_Context *ctx = (SDL_DriverDJIRemote_Context *)userdata;
    SDL_HIDAPI_Device *device = ctx->device;
    SDL_Joystick *joystick;

    if (sub != 0) {
        return;
    }
    if (!snapshot->present) {
        if (device->num_joysticks > 0) {
            HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        }
        return;
    }
    if (device->num_joysticks == 0) {
        ctx->identity = snapshot->identity;
        HIDAPI_SetDeviceName(device, snapshot->identity.name);
        if (!HIDAPI_JoystickConnected(device, NULL)) {
            return;
        }
    }
    joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    if (joystick) {
        HIDAPI_DriverDJIRemote_SendControls(joystick, &ctx->identity, &snapshot->controls);
    }
}

static void HIDAPI_DriverDJIRemote_Log(void *userdata, const char *text)
{
    (void)userdata;
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "DJI remote: %s", text);
}

/* The line action has nothing to do on USB. Writes are bulk transfers. */
static void HIDAPI_DriverDJIRemote_RunActions(SDL_DriverDJIRemote_Context *ctx)
{
    SDL_SerialAction action;
    int count = 0;

    while (count++ < DJI_MAX_ACTIONS && SDL_DJIRemoteBulkModule.NextAction(&ctx->state, &action)) {
        bool success = true;

        if (action.kind == SDL_SERIAL_ACTION_WRITE) {
            success = (SDL_hid_write(ctx->device->dev, action.data, action.length) == (int)action.length);
        }
        SDL_DJIRemoteBulkModule.ActionDone(&ctx->state, success, SDL_GetTicks());
    }
}

static bool HIDAPI_DriverDJIRemote_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverDJIRemote_Context *ctx = (SDL_DriverDJIRemote_Context *)SDL_calloc(1, sizeof(*ctx));
    SDL_SerialSink sink;

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;

    HIDAPI_SetDeviceName(device, "DJI RC (RM330)");
    device->joystick_type = SDL_JOYSTICK_TYPE_GAMEPAD;

    sink.userdata = ctx;
    sink.changed = HIDAPI_DriverDJIRemote_Changed;
    sink.log = HIDAPI_DriverDJIRemote_Log;
    SDL_DJIRemoteBulkModule.Reset(&ctx->state, &sink, SDL_GetTicks());
    HIDAPI_DriverDJIRemote_RunActions(ctx);
    return true;
}

static bool HIDAPI_DriverDJIRemote_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverDJIRemote_Context *ctx = (SDL_DriverDJIRemote_Context *)device->context;
    Uint8 data[DJI_READ_SIZE];
    Uint64 deadline;
    int size;

    if (ctx->send_snapshot) {
        /* The remote's state so far, since reports come only when it
           changes. It goes before anything newer. */
        const SDL_SerialSnapshot *snapshot = SDL_DJIRemoteBulkModule.GetSnapshot(&ctx->state, 0);
        SDL_Joystick *joystick = (device->num_joysticks > 0) ? SDL_GetJoystickFromID(device->joysticks[0]) : NULL;

        ctx->send_snapshot = false;
        if (joystick && snapshot && snapshot->present) {
            HIDAPI_DriverDJIRemote_SendControls(joystick, &ctx->identity, &snapshot->controls);
        }
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        SDL_DJIRemoteBulkModule.Feed(&ctx->state, data, (size_t)size, SDL_GetTicks());
        HIDAPI_DriverDJIRemote_RunActions(ctx);
    }

    if (size < 0) {
        // Read error, device is disconnected
        if (device->num_joysticks > 0) {
            HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        }
        return false;
    }

    if (SDL_DJIRemoteBulkModule.GetDeadline(&ctx->state, &deadline) && SDL_GetTicks() >= deadline) {
        SDL_DJIRemoteBulkModule.Tick(&ctx->state, SDL_GetTicks());
    }
    HIDAPI_DriverDJIRemote_RunActions(ctx);
    return true;
}

static bool HIDAPI_DriverDJIRemote_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverDJIRemote_Context *ctx = (SDL_DriverDJIRemote_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->naxes = ctx->identity.naxes;
    joystick->nbuttons = ctx->identity.nbuttons;
    // SDL allocates the axes and buttons after this returns, so the next update sends the state
    ctx->send_snapshot = true;
    return true;
}

static int HIDAPI_DriverDJIRemote_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverDJIRemote_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverDJIRemote_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverDJIRemote_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverDJIRemote_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverDJIRemote_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverDJIRemote_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverDJIRemote_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverDJIRemote_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverDJIRemote_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverDJIRemote = {
    SDL_HINT_JOYSTICK_HIDAPI_DJI_REMOTE,
    true,
    HIDAPI_DriverDJIRemote_RegisterHints,
    HIDAPI_DriverDJIRemote_UnregisterHints,
    HIDAPI_DriverDJIRemote_IsEnabled,
    HIDAPI_DriverDJIRemote_IsSupportedDevice,
    HIDAPI_DriverDJIRemote_InitDevice,
    HIDAPI_DriverDJIRemote_GetDevicePlayerIndex,
    HIDAPI_DriverDJIRemote_SetDevicePlayerIndex,
    HIDAPI_DriverDJIRemote_UpdateDevice,
    HIDAPI_DriverDJIRemote_OpenJoystick,
    HIDAPI_DriverDJIRemote_RumbleJoystick,
    HIDAPI_DriverDJIRemote_RumbleJoystickTriggers,
    HIDAPI_DriverDJIRemote_GetJoystickCapabilities,
    HIDAPI_DriverDJIRemote_SetJoystickLED,
    HIDAPI_DriverDJIRemote_SendJoystickEffect,
    HIDAPI_DriverDJIRemote_SetJoystickSensorsEnabled,
    HIDAPI_DriverDJIRemote_CloseJoystick,
    HIDAPI_DriverDJIRemote_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_DJI_REMOTE

#endif // SDL_JOYSTICK_HIDAPI
