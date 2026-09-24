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
#include "SDL_hidapi_intelwireless_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_INTEL_WIRELESS

/* The Intel Wireless Series base station (8086:C013) serves up to eight
 * wireless pads on alternate setting 1 of its interface 0. The shared
 * vendor-USB path in SDL_hidapi_vendorusb.c selects that alternate and its
 * endpoints when the device opens. The protocol lives in
 * SDL_hidapi_intelwireless_proto.c, where the offline tests run it. This file
 * only moves bytes between it and SDL.
 */

SDL_COMPILE_TIME_ASSERT(intel_wireless_hat_up, SDL_INTEL_WIRELESS_HAT_UP == SDL_HAT_UP);
SDL_COMPILE_TIME_ASSERT(intel_wireless_hat_right, SDL_INTEL_WIRELESS_HAT_RIGHT == SDL_HAT_RIGHT);
SDL_COMPILE_TIME_ASSERT(intel_wireless_hat_down, SDL_INTEL_WIRELESS_HAT_DOWN == SDL_HAT_DOWN);
SDL_COMPILE_TIME_ASSERT(intel_wireless_hat_left, SDL_INTEL_WIRELESS_HAT_LEFT == SDL_HAT_LEFT);

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_IntelWirelessState state;
    SDL_JoystickID joysticks[SDL_INTEL_WIRELESS_SLOTS];
    bool refresh[SDL_INTEL_WIRELESS_SLOTS]; /* Opened since the slot connected */
    Uint64 timestamp;                       /* Of the event being sent */
} SDL_DriverIntelWireless_Context;

static void HIDAPI_DriverIntelWireless_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_INTEL_WIRELESS, callback, userdata);
}

static void HIDAPI_DriverIntelWireless_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_INTEL_WIRELESS, callback, userdata);
}

static bool HIDAPI_DriverIntelWireless_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_INTEL_WIRELESS,
                              SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverIntelWireless_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return vendor_id == USB_VENDOR_INTEL && product_id == USB_PRODUCT_INTEL_WIRELESS_SERIES && interface_number == 0;
}

/* The sink the protocol reports through. */
static void IntelWireless_Connect(void *userdata, int slot)
{
    SDL_DriverIntelWireless_Context *ctx = (SDL_DriverIntelWireless_Context *)userdata;

    if (ctx->joysticks[slot] == 0) {
        ctx->refresh[slot] = false;
        HIDAPI_JoystickConnected(ctx->device, &ctx->joysticks[slot]);
    }
}

static void IntelWireless_Disconnect(void *userdata, int slot)
{
    SDL_DriverIntelWireless_Context *ctx = (SDL_DriverIntelWireless_Context *)userdata;

    if (ctx->joysticks[slot] != 0) {
        HIDAPI_JoystickDisconnected(ctx->device, ctx->joysticks[slot]);
        ctx->joysticks[slot] = 0;
        ctx->refresh[slot] = false;
    }
}

static void *IntelWireless_Joystick(void *userdata, int slot)
{
    SDL_DriverIntelWireless_Context *ctx = (SDL_DriverIntelWireless_Context *)userdata;

    if (ctx->joysticks[slot] == 0) {
        return NULL;
    }
    return SDL_GetJoystickFromID(ctx->joysticks[slot]);
}

static void IntelWireless_Controls(void *userdata, void *handle, Uint16 buttons, Uint8 hat)
{
    SDL_DriverIntelWireless_Context *ctx = (SDL_DriverIntelWireless_Context *)userdata;
    SDL_Joystick *joystick = (SDL_Joystick *)handle;
    Uint8 i;

    for (i = 0; i < SDL_INTEL_WIRELESS_JOYSTICK_BUTTONS; ++i) {
        SDL_SendJoystickButton(ctx->timestamp, joystick, i, (buttons & SDL_IntelWireless_ButtonOrder[i]) != 0);
    }
    SDL_SendJoystickHat(ctx->timestamp, joystick, 0, hat);
}

static SDL_IntelWirelessSink HIDAPI_DriverIntelWireless_Sink(SDL_DriverIntelWireless_Context *ctx)
{
    SDL_IntelWirelessSink sink;

    sink.userdata = ctx;
    sink.connect = IntelWireless_Connect;
    sink.disconnect = IntelWireless_Disconnect;
    sink.joystick = IntelWireless_Joystick;
    sink.controls = IntelWireless_Controls;
    return sink;
}

static bool HIDAPI_DriverIntelWireless_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverIntelWireless_Context *ctx;

    HIDAPI_SetDeviceName(device, "Intel Wireless Series Gamepad");

    ctx = (SDL_DriverIntelWireless_Context *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return false;
    }
    ctx->device = device;
    SDL_IntelWireless_Init(&ctx->state);
    device->context = ctx;

    /* A joystick connects when its slot becomes ready or sends input. A
     * remembered slot alone connects nothing, so a paired pad that is
     * switched off does not appear. */
    return true;
}

static int HIDAPI_DriverIntelWireless_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverIntelWireless_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverIntelWireless_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverIntelWireless_Context *ctx = (SDL_DriverIntelWireless_Context *)device->context;
    SDL_IntelWirelessSink sink = HIDAPI_DriverIntelWireless_Sink(ctx);
    Uint8 data[USB_PACKET_LENGTH];
    Uint8 reply[SDL_INTEL_WIRELESS_REPLY_SIZE];
    int size, slot;

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
#ifdef DEBUG_INTEL_WIRELESS_PROTOCOL
        HIDAPI_DumpPacket("Intel Wireless Series packet: size = %d", data, size);
#endif
        ctx->timestamp = SDL_GetTicksNS();
        SDL_IntelWireless_HandlePacket(&ctx->state, data, (size_t)size, &sink);
    }

    if (size < 0) {
        /* The base station went away */
        SDL_IntelWireless_Detach(&ctx->state, &sink);
        return false;
    }

    /* The pads report changes only, so a joystick opened after its slot
     * connected learns what is already held. */
    for (slot = 0; slot < SDL_INTEL_WIRELESS_SLOTS; ++slot) {
        if (ctx->refresh[slot]) {
            ctx->refresh[slot] = false;
            ctx->timestamp = SDL_GetTicksNS();
            SDL_IntelWireless_Refresh(&ctx->state, slot, &sink);
        }
    }

    /* At most one activation reply per update. A failed or short write is
     * queued again after a delay. */
    if (SDL_IntelWireless_NextReply(&ctx->state, SDL_GetTicksNS(), reply)) {
        const int written = SDL_hid_write(device->dev, reply, sizeof(reply));
        SDL_IntelWireless_ReplyDone(&ctx->state, written, SDL_GetTicksNS());
    }
    return true;
}

static bool HIDAPI_DriverIntelWireless_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverIntelWireless_Context *ctx = (SDL_DriverIntelWireless_Context *)device->context;
    int slot;

    SDL_AssertJoysticksLocked();

    for (slot = 0; slot < SDL_INTEL_WIRELESS_SLOTS; ++slot) {
        if (joystick->instance_id == ctx->joysticks[slot]) {
            joystick->nbuttons = SDL_INTEL_WIRELESS_JOYSTICK_BUTTONS;
            joystick->nhats = 1;
            joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
            ctx->refresh[slot] = true;
            return true;
        }
    }
    return false; // Should never get here!
}

static bool HIDAPI_DriverIntelWireless_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverIntelWireless_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverIntelWireless_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverIntelWireless_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverIntelWireless_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverIntelWireless_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverIntelWireless_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverIntelWireless_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverIntelWireless = {
    SDL_HINT_JOYSTICK_HIDAPI_INTEL_WIRELESS,
    true,
    HIDAPI_DriverIntelWireless_RegisterHints,
    HIDAPI_DriverIntelWireless_UnregisterHints,
    HIDAPI_DriverIntelWireless_IsEnabled,
    HIDAPI_DriverIntelWireless_IsSupportedDevice,
    HIDAPI_DriverIntelWireless_InitDevice,
    HIDAPI_DriverIntelWireless_GetDevicePlayerIndex,
    HIDAPI_DriverIntelWireless_SetDevicePlayerIndex,
    HIDAPI_DriverIntelWireless_UpdateDevice,
    HIDAPI_DriverIntelWireless_OpenJoystick,
    HIDAPI_DriverIntelWireless_RumbleJoystick,
    HIDAPI_DriverIntelWireless_RumbleJoystickTriggers,
    HIDAPI_DriverIntelWireless_GetJoystickCapabilities,
    HIDAPI_DriverIntelWireless_SetJoystickLED,
    HIDAPI_DriverIntelWireless_SendJoystickEffect,
    HIDAPI_DriverIntelWireless_SetJoystickSensorsEnabled,
    HIDAPI_DriverIntelWireless_CloseJoystick,
    HIDAPI_DriverIntelWireless_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_INTEL_WIRELESS

#endif // SDL_JOYSTICK_HIDAPI
