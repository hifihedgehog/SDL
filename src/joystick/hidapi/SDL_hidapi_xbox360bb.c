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
#include "SDL_hidapi_rumble.h"
#include "SDL_hidapi_xbox360.h"
#include "SDL_hidapi_xbox360bb_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_XBOX360

// Define this if you want to log all packets from the controller
// #define DEBUG_XBOX_PROTOCOL

#define MAX_CONTROLLERS 4

enum
{
    SDL_GAMEPAD_BUTTON_XBOX360_BIG_BUTTON = SDL_GAMEPAD_BUTTON_START + 1,
    SDL_GAMEPAD_NUM_XBOX360BB_BUTTONS,
};

/* The packet handling lives in SDL_hidapi_xbox360bb_proto.c, where the offline
 * tests run it. Its button bits are these joystick buttons, and its hat bits
 * are SDL's. */
SDL_COMPILE_TIME_ASSERT(xbox360bb_pads, SDL_XBOX360BB_PADS == MAX_CONTROLLERS);
SDL_COMPILE_TIME_ASSERT(xbox360bb_buttons, SDL_XBOX360BB_BUTTON_COUNT == SDL_GAMEPAD_NUM_XBOX360BB_BUTTONS);
SDL_COMPILE_TIME_ASSERT(xbox360bb_a, SDL_XBOX360BB_BUTTON_A == (1 << SDL_GAMEPAD_BUTTON_SOUTH));
SDL_COMPILE_TIME_ASSERT(xbox360bb_b, SDL_XBOX360BB_BUTTON_B == (1 << SDL_GAMEPAD_BUTTON_EAST));
SDL_COMPILE_TIME_ASSERT(xbox360bb_x, SDL_XBOX360BB_BUTTON_X == (1 << SDL_GAMEPAD_BUTTON_WEST));
SDL_COMPILE_TIME_ASSERT(xbox360bb_y, SDL_XBOX360BB_BUTTON_Y == (1 << SDL_GAMEPAD_BUTTON_NORTH));
SDL_COMPILE_TIME_ASSERT(xbox360bb_back, SDL_XBOX360BB_BUTTON_BACK == (1 << SDL_GAMEPAD_BUTTON_BACK));
SDL_COMPILE_TIME_ASSERT(xbox360bb_guide, SDL_XBOX360BB_BUTTON_GUIDE == (1 << SDL_GAMEPAD_BUTTON_GUIDE));
SDL_COMPILE_TIME_ASSERT(xbox360bb_start, SDL_XBOX360BB_BUTTON_START == (1 << SDL_GAMEPAD_BUTTON_START));
SDL_COMPILE_TIME_ASSERT(xbox360bb_big, SDL_XBOX360BB_BUTTON_BIG == (1 << SDL_GAMEPAD_BUTTON_XBOX360_BIG_BUTTON));
SDL_COMPILE_TIME_ASSERT(xbox360bb_hat_up, SDL_XBOX360BB_HAT_UP == SDL_HAT_UP);
SDL_COMPILE_TIME_ASSERT(xbox360bb_hat_right, SDL_XBOX360BB_HAT_RIGHT == SDL_HAT_RIGHT);
SDL_COMPILE_TIME_ASSERT(xbox360bb_hat_down, SDL_XBOX360BB_HAT_DOWN == SDL_HAT_DOWN);
SDL_COMPILE_TIME_ASSERT(xbox360bb_hat_left, SDL_XBOX360BB_HAT_LEFT == SDL_HAT_LEFT);

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_JoystickID joysticks[MAX_CONTROLLERS];
    SDL_Xbox360BBState state;
    Uint64 timestamp; /* Of the event being sent */
} SDL_DriverXbox360BB_Context;

static void HIDAPI_DriverXBOX360BB_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX, callback, userdata);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360, callback, userdata);
}

static void HIDAPI_DriverXBOX360BB_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX, callback, userdata);
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360, callback, userdata);
}

static bool HIDAPI_DriverXBOX360BB_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT)));
}

static bool HIDAPI_DriverXBOX360BB_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return (vendor_id == USB_VENDOR_MICROSOFT) && (product_id == USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER);
}

/* The sink the packet handling reports through. */
static void Xbox360BB_Connect(void *userdata, int pad)
{
    SDL_DriverXbox360BB_Context *ctx = (SDL_DriverXbox360BB_Context *)userdata;

    HIDAPI_JoystickConnected(ctx->device, &ctx->joysticks[pad]);
}

static void *Xbox360BB_Joystick(void *userdata, int pad)
{
    SDL_DriverXbox360BB_Context *ctx = (SDL_DriverXbox360BB_Context *)userdata;

    /* NULL for a pad the application has not opened */
    return SDL_GetJoystickFromID(ctx->joysticks[pad]);
}

static void Xbox360BB_Controls(void *userdata, void *handle, Uint8 buttons, Uint8 hat)
{
    SDL_DriverXbox360BB_Context *ctx = (SDL_DriverXbox360BB_Context *)userdata;
    SDL_Joystick *joystick = (SDL_Joystick *)handle;
    Uint8 i;

    SDL_SendJoystickHat(ctx->timestamp, joystick, 0, hat);
    for (i = 0; i < SDL_GAMEPAD_NUM_XBOX360BB_BUTTONS; ++i) {
        SDL_SendJoystickButton(ctx->timestamp, joystick, i, (buttons & (1 << i)) != 0);
    }
}

static SDL_Xbox360BBSink HIDAPI_DriverXBOX360BB_Sink(SDL_DriverXbox360BB_Context *ctx)
{
    SDL_Xbox360BBSink sink;

    sink.userdata = ctx;
    sink.connect = Xbox360BB_Connect;
    sink.joystick = Xbox360BB_Joystick;
    sink.controls = Xbox360BB_Controls;
    return sink;
}

static bool HIDAPI_DriverXBOX360BB_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverXbox360BB_Context *ctx;
    SDL_Xbox360BBSink sink;

    HIDAPI_SetDeviceName(device, "Xbox 360 Big Button Controller");

    ctx = (SDL_DriverXbox360BB_Context *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return false;
    }
    ctx->device = device;

    device->context = ctx;

    /* The receiver sends nothing for a pad at rest, so all four connect */
    sink = HIDAPI_DriverXBOX360BB_Sink(ctx);
    SDL_Xbox360BB_Attach(&ctx->state, &sink);

    return true;
}

static int HIDAPI_DriverXBOX360BB_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverXBOX360BB_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverXBOX360BB_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverXbox360BB_Context *ctx = (SDL_DriverXbox360BB_Context *)device->context;
    Uint8 i;

    SDL_AssertJoysticksLocked();

    for (i = 0; i < MAX_CONTROLLERS; ++i) {
        if (joystick->instance_id == ctx->joysticks[i]) {
            joystick->nbuttons = SDL_GAMEPAD_NUM_XBOX360BB_BUTTONS;
            joystick->nhats = 1;
            joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
            return true;
        }
    }
    return false; // Should never get here!
}

static bool HIDAPI_DriverXBOX360BB_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverXBOX360BB_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverXBOX360BB_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverXBOX360BB_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverXBOX360BB_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverXBOX360BB_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverXBOX360BB_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverXbox360BB_Context *ctx = (SDL_DriverXbox360BB_Context *)device->context;
    SDL_Xbox360BBSink sink = HIDAPI_DriverXBOX360BB_Sink(ctx);
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
#ifdef DEBUG_XBOX_PROTOCOL
        HIDAPI_DumpPacket("Xbox 360 BigButton packet: size = %d", data, size);
#endif
        /* Only 5-byte packets for pads 0 to 3 apply. A pad the application
         * has not opened gets no events. */
        ctx->timestamp = SDL_GetTicksNS();
        SDL_Xbox360BB_HandlePacket(&ctx->state, data, (size_t)size, ctx->timestamp, &sink);
    }

    // The receiver only sends packets when a button is down, handle our own release logic
    // FIXME: Even when a button is continuously pressed, we get _huge_
    // delays between packets. This is the smallest number I could get
    // without erroneous button releases. Yeesh.
    // -flibit
    // The number is SDL_XBOX360BB_RELEASE_NS, 120 ms, in SDL_hidapi_xbox360bb_proto.h.
    ctx->timestamp = SDL_GetTicksNS();
    SDL_Xbox360BB_Expire(&ctx->state, ctx->timestamp, &sink);

    return (size >= 0);
}

static void HIDAPI_DriverXBOX360BB_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverXBOX360BB_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverXbox360BB = {
    "SDL_JOYSTICK_HIDAPI_XBOX_360_BIGBUTTON",
    true,
    HIDAPI_DriverXBOX360BB_RegisterHints,
    HIDAPI_DriverXBOX360BB_UnregisterHints,
    HIDAPI_DriverXBOX360BB_IsEnabled,
    HIDAPI_DriverXBOX360BB_IsSupportedDevice,
    HIDAPI_DriverXBOX360BB_InitDevice,
    HIDAPI_DriverXBOX360BB_GetDevicePlayerIndex,
    HIDAPI_DriverXBOX360BB_SetDevicePlayerIndex,
    HIDAPI_DriverXBOX360BB_UpdateDevice,
    HIDAPI_DriverXBOX360BB_OpenJoystick,
    HIDAPI_DriverXBOX360BB_RumbleJoystick,
    HIDAPI_DriverXBOX360BB_RumbleJoystickTriggers,
    HIDAPI_DriverXBOX360BB_GetJoystickCapabilities,
    HIDAPI_DriverXBOX360BB_SetJoystickLED,
    HIDAPI_DriverXBOX360BB_SendJoystickEffect,
    HIDAPI_DriverXBOX360BB_SetJoystickSensorsEnabled,
    HIDAPI_DriverXBOX360BB_CloseJoystick,
    HIDAPI_DriverXBOX360BB_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_XBOX360

#endif // SDL_JOYSTICK_HIDAPI
