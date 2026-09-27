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
 * writes go out as bulk transfers. A bulk write waits up to 1000 ms while
 * the OUT endpoint refuses data, and UpdateDevice runs on the thread that
 * pumps events with the joystick lock held, so the writes go out on a
 * thread of the device's own, one at a time, as DJI-RC-Emulator polls from
 * its own thread. A write's completion reaches the module at a later
 * update, with the time the transfer ended. The joystick appears with the
 * first stick report and goes after 1000 ms without one. The protocol and
 * its timing live in SDL_dji_remote_proto.c, where the offline tests run
 * them. */

#define DJI_MAX_ACTIONS 16

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_DJIRemoteState state;
    SDL_SerialIdentity identity; /* Of the joystick the device has */
    bool send_snapshot;          /* A joystick opened since the last update */
    bool writing;                /* A write went to the thread, and the module has not had its completion */

    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_Condition *wake;

    /* Under lock */
    bool stop;
    bool queued;    /* The write below waits for the thread */
    bool done;      /* The thread finished a write, with the result below */
    bool success;
    Uint64 done_at; /* When the transfer returned, in SDL_GetTicks() */
    size_t length;
    Uint8 data[SDL_SERIAL_MAX_WRITE];
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

/* Sends each write handed over, until FreeDevice stops it. The lock is
   never held during a transfer. */
static int SDLCALL HIDAPI_DriverDJIRemote_WriteThread(void *data)
{
    SDL_DriverDJIRemote_Context *ctx = (SDL_DriverDJIRemote_Context *)data;
    Uint8 frame[SDL_SERIAL_MAX_WRITE];

    SDL_LockMutex(ctx->lock);
    while (!ctx->stop) {
        size_t length;
        bool success;
        Uint64 done_at;

        if (!ctx->queued) {
            SDL_WaitCondition(ctx->wake, ctx->lock);
            continue;
        }
        length = ctx->length;
        SDL_memcpy(frame, ctx->data, length);
        ctx->queued = false;
        SDL_UnlockMutex(ctx->lock);

        success = (SDL_hid_write(ctx->device->dev, frame, length) == (int)length);
        done_at = SDL_GetTicks();

        SDL_LockMutex(ctx->lock);
        ctx->success = success;
        ctx->done_at = done_at;
        ctx->done = true;
    }
    SDL_UnlockMutex(ctx->lock);
    return 0;
}

/* Runs the module's actions in order, one at a time, as the serial engine
   does. The line action has nothing to do on USB. A write goes to the
   thread, and the next action waits until the module has its completion. */
static void HIDAPI_DriverDJIRemote_RunActions(SDL_DriverDJIRemote_Context *ctx)
{
    int count = 0;

    for (;;) {
        SDL_SerialAction action;

        if (ctx->writing) {
            bool done, success;
            Uint64 done_at;

            SDL_LockMutex(ctx->lock);
            done = ctx->done;
            success = ctx->success;
            done_at = ctx->done_at;
            ctx->done = false;
            SDL_UnlockMutex(ctx->lock);
            if (!done) {
                return;
            }
            ctx->writing = false;
            SDL_DJIRemoteBulkModule.ActionDone(&ctx->state, success, done_at);
        }
        if (count++ >= DJI_MAX_ACTIONS || !SDL_DJIRemoteBulkModule.NextAction(&ctx->state, &action)) {
            return;
        }
        if (action.kind != SDL_SERIAL_ACTION_WRITE) {
            SDL_DJIRemoteBulkModule.ActionDone(&ctx->state, true, SDL_GetTicks());
            continue;
        }
        SDL_LockMutex(ctx->lock);
        SDL_memcpy(ctx->data, action.data, action.length);
        ctx->length = action.length;
        ctx->queued = true;
        SDL_SignalCondition(ctx->wake);
        SDL_UnlockMutex(ctx->lock);
        ctx->writing = true;
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

    ctx->lock = SDL_CreateMutex();
    ctx->wake = SDL_CreateCondition();
    if (!ctx->lock || !ctx->wake) {
        return false;
    }
    ctx->thread = SDL_CreateThread(HIDAPI_DriverDJIRemote_WriteThread, "SDL DJI remote writes", ctx);
    if (!ctx->thread) {
        return false;
    }

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
    Uint8 data[SDL_DJI_BULK_READ_SIZE];
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

    /* A write that finished since the last update reaches the module before
       the replies read now */
    HIDAPI_DriverDJIRemote_RunActions(ctx);

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
    SDL_DriverDJIRemote_Context *ctx = (SDL_DriverDJIRemote_Context *)device->context;

    if (!ctx) {
        return;
    }
    /* The backend closes the device after this returns, so a write still
       running finishes first, which takes up to its 1000 ms timeout */
    if (ctx->thread) {
        SDL_LockMutex(ctx->lock);
        ctx->stop = true;
        SDL_BroadcastCondition(ctx->wake);
        SDL_UnlockMutex(ctx->lock);
        SDL_WaitThread(ctx->thread, NULL);
        ctx->thread = NULL;
    }
    if (ctx->wake) {
        SDL_DestroyCondition(ctx->wake);
        ctx->wake = NULL;
    }
    if (ctx->lock) {
        SDL_DestroyMutex(ctx->lock);
        ctx->lock = NULL;
    }
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
