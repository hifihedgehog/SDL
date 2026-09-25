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
#include "../SDL_iforce_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_IFORCE

#include "../../misc/SDL_libusb.h"

/* I-Force wheels and joysticks on USB (hifihedgehog/SDL#33 Part 8):
 * Immersion's packets on the interrupt endpoints of interface 0, read
 * through libusb once WinUSB is bound. The descriptor's VID:PID picks the
 * layout, so input needs no query. The queries O, M, P, B, N, C, E, O and V
 * are vendor control requests of up to 1000 ms each, with O tried up to 20
 * times, so they run on their own thread. Their effect count and memory end
 * become joystick properties, which the I-Force haptic driver reads. The
 * protocol lives in SDL_iforce_proto.c, where the offline tests run it. */
SDL_COMPILE_TIME_ASSERT(iforce_hat_up, SDL_IFORCE_HAT_UP == SDL_HAT_UP);
SDL_COMPILE_TIME_ASSERT(iforce_hat_right, SDL_IFORCE_HAT_RIGHT == SDL_HAT_RIGHT);
SDL_COMPILE_TIME_ASSERT(iforce_hat_down, SDL_IFORCE_HAT_DOWN == SDL_HAT_DOWN);
SDL_COMPILE_TIME_ASSERT(iforce_hat_left, SDL_IFORCE_HAT_LEFT == SDL_HAT_LEFT);

typedef struct
{
    SDL_HIDAPI_Device *device;
    const SDL_IForceModel *model;
    SDL_IForceIdentity identity;
    SDL_IForceState state;
    bool post_pending; /* The joystick opened and has not had the state yet */
    bool published;    /* The joystick has the query results */

    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_Condition *settled;
    SDL_AtomicInt stop;
    SDL_LibUSBContext *libusb;
    libusb_device_handle *handle;

    /* Under lock */
    SDL_IForceQueries queries;
    bool unanswered; /* A query went unanswered */
    bool finished;   /* The sequence is over, or never started */
} SDL_DriverIForce_Context;

static void HIDAPI_DriverIForce_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_IFORCE, callback, userdata);
}

static void HIDAPI_DriverIForce_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_IFORCE, callback, userdata);
}

static bool HIDAPI_DriverIForce_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_IFORCE, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverIForce_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return interface_number == 0 && SDL_IForce_FindModel(vendor_id, product_id, true) != NULL;
}

/* Asks each query in turn until the sequence ends or the device goes */
static int SDLCALL HIDAPI_DriverIForce_QueryThread(void *data)
{
    SDL_DriverIForce_Context *ctx = (SDL_DriverIForce_Context *)data;

    for (;;) {
        SDL_IForceControlSetup setup;
        Uint8 reply[SDL_IFORCE_MAX_LENGTH];
        Uint8 letter;
        int result;

        SDL_LockMutex(ctx->lock);
        letter = SDL_IForce_NextQuery(&ctx->queries);
        SDL_UnlockMutex(ctx->lock);
        if (!letter || SDL_GetAtomicInt(&ctx->stop)) {
            break;
        }
        SDL_IForce_QuerySetup(letter, &setup);
        result = ctx->libusb->control_transfer(ctx->handle, setup.request_type, setup.request, setup.value, setup.index,
                                               reply, setup.length, SDL_IFORCE_QUERY_TIMEOUT_MS);
        SDL_LockMutex(ctx->lock);
        SDL_IForce_QueryResult(&ctx->queries, (result > 0) ? reply : NULL, (result > 0) ? (size_t)result : 0);
        if (result <= 0 || reply[0] != letter) {
            ctx->unanswered = true;
        }
        SDL_BroadcastCondition(ctx->settled);
        SDL_UnlockMutex(ctx->lock);
    }

    SDL_LockMutex(ctx->lock);
    ctx->finished = true;
    SDL_BroadcastCondition(ctx->settled);
    SDL_UnlockMutex(ctx->lock);
    return 0;
}

/* Joins a thread that has finished and lets go of libusb */
static void HIDAPI_DriverIForce_EndQueries(SDL_DriverIForce_Context *ctx, bool wait)
{
    bool finished;

    if (!ctx->thread) {
        return;
    }
    SDL_LockMutex(ctx->lock);
    finished = ctx->finished;
    SDL_UnlockMutex(ctx->lock);
    if (!finished && !wait) {
        return;
    }
    SDL_SetAtomicInt(&ctx->stop, 1);
    SDL_WaitThread(ctx->thread, NULL);
    ctx->thread = NULL;
    if (ctx->libusb) {
        SDL_QuitLibUSB();
        ctx->libusb = NULL;
        ctx->handle = NULL;
    }
}

static bool HIDAPI_DriverIForce_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverIForce_Context *ctx = (SDL_DriverIForce_Context *)SDL_calloc(1, sizeof(*ctx));
    libusb_device_handle *handle;

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;
    ctx->model = SDL_IForce_FindModel(device->vendor_id, device->product_id, true);
    if (!ctx->model) {
        return SDL_SetError("Not an I-Force device");
    }
    /* Only the libusb backend hands over the endpoint's packets unchanged.
       A device a platform HID backend opened stays with the other joystick
       backends. */
    handle = (libusb_device_handle *)SDL_GetPointerProperty(SDL_hid_get_properties(device->dev), SDL_PROP_HIDAPI_LIBUSB_DEVICE_HANDLE_POINTER, NULL);
    if (!handle) {
        return SDL_SetError("I-Force devices are read through libusb");
    }
    SDL_IForce_GetIdentity(ctx->model, &ctx->identity);
    SDL_IForce_ResetState(ctx->model, &ctx->state);
    SDL_IForce_InitQueries(&ctx->queries);
    ctx->finished = true;

    HIDAPI_SetDeviceName(device, ctx->model->name);
    device->joystick_type = ctx->identity.wheel ? SDL_JOYSTICK_TYPE_WHEEL : SDL_JOYSTICK_TYPE_FLIGHT_STICK;

    ctx->lock = SDL_CreateMutex();
    ctx->settled = SDL_CreateCondition();
    if (ctx->lock && ctx->settled && SDL_InitLibUSB(&ctx->libusb)) {
        ctx->handle = handle;
        ctx->finished = false;
        ctx->thread = SDL_CreateThread(HIDAPI_DriverIForce_QueryThread, "SDL I-Force queries", ctx);
        if (!ctx->thread) {
            ctx->finished = true;
            SDL_QuitLibUSB();
            ctx->libusb = NULL;
            ctx->handle = NULL;
        }
    }
    /* Without the queries the device keeps its input and has no force feedback */
    return HIDAPI_JoystickConnected(device, NULL);
}

static void HIDAPI_DriverIForce_Post(SDL_DriverIForce_Context *ctx, SDL_Joystick *joystick)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    int i;

    for (i = 0; i < ctx->identity.naxes; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, ctx->state.axes[i]);
    }
    for (i = 0; i < ctx->identity.nbuttons; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, (ctx->state.buttons & (1u << i)) != 0);
    }
    for (i = 0; i < ctx->identity.nhats; ++i) {
        SDL_SendJoystickHat(timestamp, joystick, (Uint8)i, ctx->state.hats[i]);
    }
}

/* The joystick learns N and B once the sequence is over */
static void HIDAPI_DriverIForce_Publish(SDL_DriverIForce_Context *ctx, SDL_Joystick *joystick)
{
    SDL_PropertiesID props;
    SDL_IForceQueries queries;
    bool finished;

    if (ctx->published || !joystick) {
        return;
    }
    SDL_LockMutex(ctx->lock);
    finished = ctx->finished;
    queries = ctx->queries;
    SDL_UnlockMutex(ctx->lock);
    if (!finished) {
        return;
    }
    ctx->published = true;
    if (!queries.open || queries.effects <= 0) {
        return;
    }
    props = SDL_GetJoystickProperties(joystick);
    SDL_SetNumberProperty(props, SDL_IFORCE_PROP_EFFECTS_NUMBER, queries.effects);
    SDL_SetNumberProperty(props, SDL_IFORCE_PROP_MEMORY_NUMBER, queries.memory_end);
}

static bool HIDAPI_DriverIForce_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverIForce_Context *ctx = (SDL_DriverIForce_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    HIDAPI_DriverIForce_EndQueries(ctx, false);
    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }
    if (joystick && ctx->post_pending) {
        // The pedals and throttle rest at -32768, which the joystick learns only from an event
        HIDAPI_DriverIForce_Post(ctx, joystick);
        ctx->post_pending = false;
    }
    HIDAPI_DriverIForce_Publish(ctx, joystick);

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        if (SDL_IForce_DecodePacket(ctx->model, data[0], data + 1, (size_t)size - 1, &ctx->state, NULL) != SDL_IFORCE_PACKET_IGNORED &&
            joystick) {
            HIDAPI_DriverIForce_Post(ctx, joystick);
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

static bool HIDAPI_DriverIForce_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverIForce_Context *ctx = (SDL_DriverIForce_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->naxes = ctx->identity.naxes;
    joystick->nbuttons = ctx->identity.nbuttons;
    joystick->nhats = ctx->identity.nhats;
    // The axes are allocated after this returns, so the next update sends the state
    ctx->post_pending = true;

    /* An application that opens the haptic device next needs N and B. Wait
       until the sequence is over or a query goes unanswered, which a device
       that answers takes a few milliseconds and a silent one one timeout,
       and never longer than one timeout. A later end publishes from the
       update. */
    ctx->published = false;
    if (ctx->lock) {
        const Uint64 deadline = SDL_GetTicks() + SDL_IFORCE_QUERY_TIMEOUT_MS;

        SDL_LockMutex(ctx->lock);
        while (!ctx->finished && !ctx->unanswered) {
            const Uint64 now = SDL_GetTicks();

            if (now >= deadline) {
                break;
            }
            SDL_WaitConditionTimeout(ctx->settled, ctx->lock, (Sint32)(deadline - now));
        }
        SDL_UnlockMutex(ctx->lock);
    }
    HIDAPI_DriverIForce_Publish(ctx, joystick);
    return true;
}

static int HIDAPI_DriverIForce_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverIForce_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverIForce_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverIForce_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverIForce_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverIForce_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

/* One whole I-Force command, as the haptic driver builds it, on the
   interrupt OUT endpoint */
static bool HIDAPI_DriverIForce_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    if (size <= 0 || !SDL_IForce_IsCommand((const Uint8 *)data, (size_t)size)) {
        return SDL_SetError("An I-Force effect is one whole command");
    }
    if (SDL_hid_write(device->dev, (const unsigned char *)data, (size_t)size) != size) {
        return SDL_SetError("Couldn't send I-Force command");
    }
    return true;
}

static bool HIDAPI_DriverIForce_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverIForce_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverIForce_FreeDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverIForce_Context *ctx = (SDL_DriverIForce_Context *)device->context;

    if (!ctx) {
        return;
    }
    HIDAPI_DriverIForce_EndQueries(ctx, true);
    if (ctx->settled) {
        SDL_DestroyCondition(ctx->settled);
        ctx->settled = NULL;
    }
    if (ctx->lock) {
        SDL_DestroyMutex(ctx->lock);
        ctx->lock = NULL;
    }
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverIForce = {
    SDL_HINT_JOYSTICK_HIDAPI_IFORCE,
    true,
    HIDAPI_DriverIForce_RegisterHints,
    HIDAPI_DriverIForce_UnregisterHints,
    HIDAPI_DriverIForce_IsEnabled,
    HIDAPI_DriverIForce_IsSupportedDevice,
    HIDAPI_DriverIForce_InitDevice,
    HIDAPI_DriverIForce_GetDevicePlayerIndex,
    HIDAPI_DriverIForce_SetDevicePlayerIndex,
    HIDAPI_DriverIForce_UpdateDevice,
    HIDAPI_DriverIForce_OpenJoystick,
    HIDAPI_DriverIForce_RumbleJoystick,
    HIDAPI_DriverIForce_RumbleJoystickTriggers,
    HIDAPI_DriverIForce_GetJoystickCapabilities,
    HIDAPI_DriverIForce_SetJoystickLED,
    HIDAPI_DriverIForce_SendJoystickEffect,
    HIDAPI_DriverIForce_SetJoystickSensorsEnabled,
    HIDAPI_DriverIForce_CloseJoystick,
    HIDAPI_DriverIForce_FreeDevice,
};

#endif /* SDL_JOYSTICK_HIDAPI_IFORCE */

#endif /* SDL_JOYSTICK_HIDAPI */
