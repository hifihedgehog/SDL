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
#include "SDL_hidapi_usio_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_USIO

/* The Namco USIO, 0B9A:0910, and the H050 USJ(C), 0B9A:0900, the I/O board
 * of the System 357 and 369 cabinets (hifihedgehog/SDL#33 Part 14). Its
 * vendor interface 0 is read through libusb once WinUSB is bound. The vendor
 * rule in SDL_hidapi_vendorusb.c names bulk OUT 0x01 and bulk IN 0x82, so
 * hid_write sends each 6-byte command as one bulk transfer, first byte
 * included, and the backend reads the replies a packet at a time. The board
 * sends nothing unasked, so the protocol runs in UpdateDevice whether or not
 * a joystick is open. The protocol, its timing and the two layouts live in
 * SDL_hidapi_usio_proto.c, where the offline tests run them. This file only
 * moves bytes between it and SDL. */

SDL_COMPILE_TIME_ASSERT(usio_hat_up, SDL_USIO_HAT_UP == SDL_HAT_UP);
SDL_COMPILE_TIME_ASSERT(usio_hat_right, SDL_USIO_HAT_RIGHT == SDL_HAT_RIGHT);
SDL_COMPILE_TIME_ASSERT(usio_hat_down, SDL_USIO_HAT_DOWN == SDL_HAT_DOWN);
SDL_COMPILE_TIME_ASSERT(usio_hat_left, SDL_USIO_HAT_LEFT == SDL_HAT_LEFT);
SDL_COMPILE_TIME_ASSERT(usio_arcade_stick, SDL_USIO_JOYSTICK_ARCADE_STICK == SDL_JOYSTICK_TYPE_ARCADE_STICK);
SDL_COMPILE_TIME_ASSERT(usio_drum_kit, SDL_USIO_JOYSTICK_DRUM_KIT == SDL_JOYSTICK_TYPE_DRUM_KIT);

/* The largest bulk packet, at high speed (USB 2.0 section 5.8.3). The rule
 * takes any packet size, and a read shorter than the packet would lose the
 * rest of it. */
#define USIO_READ_SIZE 512

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_USIOState state;
    int players;
    SDL_JoystickID joysticks[SDL_USIO_MAX_PLAYERS];
    bool post_pending[SDL_USIO_MAX_PLAYERS]; /* Opened and not yet sent the state */
} SDL_DriverUSIO_Context;

static void HIDAPI_DriverUSIO_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_USIO, callback, userdata);
}

static void HIDAPI_DriverUSIO_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_USIO, callback, userdata);
}

static bool HIDAPI_DriverUSIO_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_USIO, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverUSIO_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    /* Interface 0 whatever its class. RPCS3 and ITAIKO-firmware give class
       00 (usio.cpp:93-101, usio_driver.c:47-54), TaikoZucchini FF
       (bpreader_hook.c:118-121). */
    return vendor_id == USB_VENDOR_NAMCO &&
           (product_id == USB_PRODUCT_NAMCO_USIO || product_id == USB_PRODUCT_NAMCO_H050_USJC) &&
           interface_number == 0;
}

static int HIDAPI_DriverUSIO_FindPlayer(const SDL_DriverUSIO_Context *ctx, SDL_JoystickID instance_id)
{
    int player;

    if (!ctx || instance_id == 0) {
        return -1;
    }
    for (player = 0; player < ctx->players; ++player) {
        if (ctx->joysticks[player] == instance_id) {
            return player;
        }
    }
    return -1;
}

static void HIDAPI_DriverUSIO_Post(SDL_DriverUSIO_Context *ctx, int player, SDL_Joystick *joystick, const SDL_USIOControls *controls)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    SDL_USIOJoystickInfo info;
    int i;

    if (!controls || !SDL_USIO_GetJoystickInfo(ctx->state.layout, player, &info)) {
        return;
    }
    for (i = 0; i < info.naxes; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, controls->axes[i]);
    }
    for (i = 0; i < info.nbuttons; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, (controls->buttons & (1u << i)) != 0);
    }
    for (i = 0; i < info.nhats; ++i) {
        SDL_SendJoystickHat(timestamp, joystick, (Uint8)i, controls->hat);
    }
}

/* The sink the module reports through */
static void USIO_Connect(void *userdata)
{
    SDL_DriverUSIO_Context *ctx = (SDL_DriverUSIO_Context *)userdata;
    int player;

    /* Player 1 first, so the joysticks appear in player order */
    for (player = 0; player < ctx->players; ++player) {
        if (ctx->joysticks[player] == 0) {
            ctx->post_pending[player] = false;
            HIDAPI_JoystickConnected(ctx->device, &ctx->joysticks[player]);
        }
    }
}

static void USIO_Disconnect(void *userdata)
{
    SDL_DriverUSIO_Context *ctx = (SDL_DriverUSIO_Context *)userdata;
    int player;

    for (player = 0; player < ctx->players; ++player) {
        if (ctx->joysticks[player] != 0) {
            HIDAPI_JoystickDisconnected(ctx->device, ctx->joysticks[player]);
            ctx->joysticks[player] = 0;
        }
        ctx->post_pending[player] = false;
    }
}

static void USIO_Controls(void *userdata, int player, const SDL_USIOControls *controls)
{
    SDL_DriverUSIO_Context *ctx = (SDL_DriverUSIO_Context *)userdata;
    SDL_Joystick *joystick;

    if (player < 0 || player >= ctx->players || ctx->joysticks[player] == 0) {
        return;
    }
    /* NULL until the application opens the joystick */
    joystick = SDL_GetJoystickFromID(ctx->joysticks[player]);
    if (joystick) {
        HIDAPI_DriverUSIO_Post(ctx, player, joystick, controls);
    }
}

static void USIO_Log(void *userdata, const char *text)
{
    (void)userdata;
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Namco USIO: %s", text);
}

static SDL_USIOSink HIDAPI_DriverUSIO_Sink(SDL_DriverUSIO_Context *ctx)
{
    SDL_USIOSink sink;

    sink.userdata = ctx;
    sink.connect = USIO_Connect;
    sink.disconnect = USIO_Disconnect;
    sink.controls = USIO_Controls;
    sink.log = USIO_Log;
    return sink;
}

/* Each player's joystick carries its own name, "Namco USIO Taiko Drum P1"
   and so on */
static const char *HIDAPI_DriverUSIO_GetJoystickName(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    const SDL_DriverUSIO_Context *ctx = (const SDL_DriverUSIO_Context *)device->context;
    SDL_USIOJoystickInfo info;
    const int player = HIDAPI_DriverUSIO_FindPlayer(ctx, instance_id);

    if (player < 0 || !SDL_USIO_GetJoystickInfo(ctx->state.layout, player, &info)) {
        return NULL;
    }
    return info.name;
}

static bool HIDAPI_DriverUSIO_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverUSIO_Context *ctx = (SDL_DriverUSIO_Context *)SDL_calloc(1, sizeof(*ctx));
    SDL_USIOLayout layout;

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;

    /* The layout is read once, when the board opens. 0900 is used only when
       its identification block names a USIO. */
    layout = SDL_USIO_LayoutFromHint(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_USIO_LAYOUT));
    SDL_USIO_Init(&ctx->state, layout, device->product_id == USB_PRODUCT_NAMCO_H050_USJC);
    ctx->players = SDL_USIO_PlayerCount(layout);

    HIDAPI_SetDeviceName(device, SDL_USIO_DeviceName(layout));
    device->joystick_type = (SDL_JoystickType)SDL_USIO_JoystickType(layout);
    /* The two layouts share the USB ID, and this byte keeps their gamepad
       mappings apart */
    device->guid.data[15] = SDL_USIO_GUIDByte(layout);
    device->GetJoystickName = HIDAPI_DriverUSIO_GetJoystickName;

    /* The joysticks appear once the identification block confirms the
       board. The first command goes out from the first update. */
    return true;
}

static int HIDAPI_DriverUSIO_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    const SDL_DriverUSIO_Context *ctx = (const SDL_DriverUSIO_Context *)device->context;

    return HIDAPI_DriverUSIO_FindPlayer(ctx, instance_id);
}

static void HIDAPI_DriverUSIO_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverUSIO_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverUSIO_Context *ctx = (SDL_DriverUSIO_Context *)device->context;
    SDL_USIOSink sink = HIDAPI_DriverUSIO_Sink(ctx);
    Uint8 data[USIO_READ_SIZE];
    Uint8 command[SDL_USIO_COMMAND_SIZE];
    int player, size, empty = 0;

    /* A joystick opened since the last update learns what is held */
    for (player = 0; player < ctx->players; ++player) {
        if (ctx->post_pending[player]) {
            SDL_Joystick *joystick = SDL_GetJoystickFromID(ctx->joysticks[player]);

            ctx->post_pending[player] = false;
            if (joystick) {
                HIDAPI_DriverUSIO_Post(ctx, player, joystick, SDL_USIO_GetControls(&ctx->state, player));
            }
        }
    }

    /* A zero-length transfer is a report of its own and reads as 0 bytes,
       as an empty queue does, so the reads stop at the second 0 in a row */
    for (;;) {
        size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0);
        if (size < 0) {
            break;
        }
        if (size == 0) {
            if (++empty == 2) {
                break;
            }
            continue;
        }
        empty = 0;
#ifdef DEBUG_USIO_PROTOCOL
        HIDAPI_DumpPacket("Namco USIO packet: size = %d", data, size);
#endif
        SDL_USIO_Feed(&ctx->state, data, (size_t)size, SDL_GetTicksNS(), &sink);
    }

    if (size < 0) {
        // Read error, device is disconnected
        SDL_USIO_Detach(&ctx->state, &sink);
        return false;
    }

    /* One read out at a time, the next as soon as the last reply is whole */
    SDL_USIO_Tick(&ctx->state, SDL_GetTicksNS(), &sink);
    if (SDL_USIO_NextCommand(&ctx->state, SDL_GetTicksNS(), command)) {
        const int written = SDL_hid_write(device->dev, command, sizeof(command));

        SDL_USIO_CommandDone(&ctx->state, written, SDL_GetTicksNS(), &sink);
    }
    return true;
}

static bool HIDAPI_DriverUSIO_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverUSIO_Context *ctx = (SDL_DriverUSIO_Context *)device->context;
    SDL_USIOJoystickInfo info;
    int player;

    SDL_AssertJoysticksLocked();

    player = HIDAPI_DriverUSIO_FindPlayer(ctx, joystick->instance_id);
    if (player < 0 || !SDL_USIO_GetJoystickInfo(ctx->state.layout, player, &info)) {
        return false; // Should never get here!
    }
    joystick->naxes = info.naxes;
    joystick->nbuttons = info.nbuttons;
    joystick->nhats = info.nhats;
    // The controls are allocated after this returns, so the next update sends the state
    ctx->post_pending[player] = true;
    return true;
}

static bool HIDAPI_DriverUSIO_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverUSIO_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverUSIO_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverUSIO_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverUSIO_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverUSIO_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverUSIO_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverUSIO_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverUSIO = {
    SDL_HINT_JOYSTICK_HIDAPI_USIO,
    true,
    HIDAPI_DriverUSIO_RegisterHints,
    HIDAPI_DriverUSIO_UnregisterHints,
    HIDAPI_DriverUSIO_IsEnabled,
    HIDAPI_DriverUSIO_IsSupportedDevice,
    HIDAPI_DriverUSIO_InitDevice,
    HIDAPI_DriverUSIO_GetDevicePlayerIndex,
    HIDAPI_DriverUSIO_SetDevicePlayerIndex,
    HIDAPI_DriverUSIO_UpdateDevice,
    HIDAPI_DriverUSIO_OpenJoystick,
    HIDAPI_DriverUSIO_RumbleJoystick,
    HIDAPI_DriverUSIO_RumbleJoystickTriggers,
    HIDAPI_DriverUSIO_GetJoystickCapabilities,
    HIDAPI_DriverUSIO_SetJoystickLED,
    HIDAPI_DriverUSIO_SendJoystickEffect,
    HIDAPI_DriverUSIO_SetJoystickSensorsEnabled,
    HIDAPI_DriverUSIO_CloseJoystick,
    HIDAPI_DriverUSIO_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_USIO

#endif // SDL_JOYSTICK_HIDAPI
