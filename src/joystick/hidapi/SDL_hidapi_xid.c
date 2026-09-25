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
#include "SDL_hidapi_xid_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_XID

#include "../../misc/SDL_libusb.h"

// Define this if you want to log all packets from the controller
// #define DEBUG_XID_PROTOCOL

/* The protocol lives in SDL_hidapi_xid_proto.c, where the offline tests run
 * it. Its joystick types, hat bits, buttons 0-10 and axes 0-5 are SDL's. */
SDL_COMPILE_TIME_ASSERT(xid_type_unknown, SDL_XID_JOYSTICK_UNKNOWN == SDL_JOYSTICK_TYPE_UNKNOWN);
SDL_COMPILE_TIME_ASSERT(xid_type_gamepad, SDL_XID_JOYSTICK_GAMEPAD == SDL_JOYSTICK_TYPE_GAMEPAD);
SDL_COMPILE_TIME_ASSERT(xid_type_wheel, SDL_XID_JOYSTICK_WHEEL == SDL_JOYSTICK_TYPE_WHEEL);
SDL_COMPILE_TIME_ASSERT(xid_type_arcade_stick, SDL_XID_JOYSTICK_ARCADE_STICK == SDL_JOYSTICK_TYPE_ARCADE_STICK);
SDL_COMPILE_TIME_ASSERT(xid_type_flight_stick, SDL_XID_JOYSTICK_FLIGHT_STICK == SDL_JOYSTICK_TYPE_FLIGHT_STICK);
SDL_COMPILE_TIME_ASSERT(xid_type_dance_pad, SDL_XID_JOYSTICK_DANCE_PAD == SDL_JOYSTICK_TYPE_DANCE_PAD);
SDL_COMPILE_TIME_ASSERT(xid_hat_up, SDL_XID_HAT_UP == SDL_HAT_UP);
SDL_COMPILE_TIME_ASSERT(xid_hat_right, SDL_XID_HAT_RIGHT == SDL_HAT_RIGHT);
SDL_COMPILE_TIME_ASSERT(xid_hat_down, SDL_XID_HAT_DOWN == SDL_HAT_DOWN);
SDL_COMPILE_TIME_ASSERT(xid_hat_left, SDL_XID_HAT_LEFT == SDL_HAT_LEFT);
SDL_COMPILE_TIME_ASSERT(xid_south, SDL_GAMEPAD_BUTTON_SOUTH == 0);
SDL_COMPILE_TIME_ASSERT(xid_east, SDL_GAMEPAD_BUTTON_EAST == 1);
SDL_COMPILE_TIME_ASSERT(xid_west, SDL_GAMEPAD_BUTTON_WEST == 2);
SDL_COMPILE_TIME_ASSERT(xid_north, SDL_GAMEPAD_BUTTON_NORTH == 3);
SDL_COMPILE_TIME_ASSERT(xid_back, SDL_GAMEPAD_BUTTON_BACK == 4);
SDL_COMPILE_TIME_ASSERT(xid_guide, SDL_GAMEPAD_BUTTON_GUIDE == 5);
SDL_COMPILE_TIME_ASSERT(xid_start, SDL_GAMEPAD_BUTTON_START == 6);
SDL_COMPILE_TIME_ASSERT(xid_left_stick, SDL_GAMEPAD_BUTTON_LEFT_STICK == 7);
SDL_COMPILE_TIME_ASSERT(xid_right_stick, SDL_GAMEPAD_BUTTON_RIGHT_STICK == 8);
SDL_COMPILE_TIME_ASSERT(xid_left_shoulder, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER == 9);
SDL_COMPILE_TIME_ASSERT(xid_right_shoulder, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER == 10);
SDL_COMPILE_TIME_ASSERT(xid_extra, SDL_XID_BUTTON_EXTRA == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER + 1);
SDL_COMPILE_TIME_ASSERT(xid_leftx, SDL_GAMEPAD_AXIS_LEFTX == 0);
SDL_COMPILE_TIME_ASSERT(xid_lefty, SDL_GAMEPAD_AXIS_LEFTY == 1);
SDL_COMPILE_TIME_ASSERT(xid_rightx, SDL_GAMEPAD_AXIS_RIGHTX == 2);
SDL_COMPILE_TIME_ASSERT(xid_righty, SDL_GAMEPAD_AXIS_RIGHTY == 3);
SDL_COMPILE_TIME_ASSERT(xid_left_trigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER == 4);
SDL_COMPILE_TIME_ASSERT(xid_right_trigger, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER == 5);
SDL_COMPILE_TIME_ASSERT(xid_read_length, SDL_XID_READ_LENGTH == USB_PACKET_LENGTH);
SDL_COMPILE_TIME_ASSERT(xid_gamepad_buttons, SDL_XID_DANCE_BUTTONS <= 16);
SDL_COMPILE_TIME_ASSERT(xid_sb_buttons, SDL_XID_SB_BUTTONS <= 64);

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_JoystickID joystick_id; // 0 once a read error has disconnected it
    SDL_XIDSession session;
    // Set only while InitDevice sends its control requests
    SDL_LibUSBContext *libusb;
    libusb_device_handle *handle;
} SDL_DriverXID_Context;

static void HIDAPI_DriverXboxOriginal_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX, callback, userdata);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_ORIGINAL, callback, userdata);
}

static void HIDAPI_DriverXboxOriginal_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX, callback, userdata);
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_ORIGINAL, callback, userdata);
}

static bool HIDAPI_DriverXboxOriginal_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX_ORIGINAL,
                              SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT)));
}

static bool HIDAPI_DriverXboxOriginal_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return SDL_XID_IsSupportedInterface(interface_class, interface_subclass, interface_protocol) &&
           !SDL_XID_IsSteelBattalionID(vendor_id, product_id);
}

static void HIDAPI_DriverSteelBattalion_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX, callback, userdata);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_ORIGINAL, callback, userdata);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_STEEL_BATTALION, callback, userdata);
}

static void HIDAPI_DriverSteelBattalion_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX, callback, userdata);
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_ORIGINAL, callback, userdata);
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_STEEL_BATTALION, callback, userdata);
}

static bool HIDAPI_DriverSteelBattalion_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_STEEL_BATTALION, HIDAPI_DriverXboxOriginal_IsEnabled());
}

static bool HIDAPI_DriverSteelBattalion_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return SDL_XID_IsSupportedInterface(interface_class, interface_subclass, interface_protocol) &&
           SDL_XID_IsSteelBattalionID(vendor_id, product_id);
}

/* The sink the session reports through. */
static int XID_ControlIn(void *userdata, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                         uint8_t *data, uint16_t length, unsigned int timeout_ms)
{
    SDL_DriverXID_Context *ctx = (SDL_DriverXID_Context *)userdata;

    if (!ctx->libusb || !ctx->handle) {
        return -1;
    }
    return ctx->libusb->control_transfer(ctx->handle, request_type, request, value, index, data, length, timeout_ms);
}

static int XID_Read(void *userdata, uint8_t *data, size_t length)
{
    SDL_DriverXID_Context *ctx = (SDL_DriverXID_Context *)userdata;
    int size = SDL_hid_read_timeout(ctx->device->dev, data, length, 0);

#ifdef DEBUG_XID_PROTOCOL
    if (size > 0) {
        HIDAPI_DumpPacket("XID packet: size = %d", data, size);
    }
#endif
    return size;
}

static bool XID_Write(void *userdata, const uint8_t *data, size_t length)
{
    SDL_DriverXID_Context *ctx = (SDL_DriverXID_Context *)userdata;

    // The libusb backend writes it to the interrupt OUT endpoint with its leading 0x00
    return SDL_HIDAPI_SendRumble(ctx->device, data, (int)length) == (int)length;
}

static void *XID_Joystick(void *userdata)
{
    SDL_DriverXID_Context *ctx = (SDL_DriverXID_Context *)userdata;

    // NULL until the application opens the joystick
    return ctx->joystick_id ? SDL_GetJoystickFromID(ctx->joystick_id) : NULL;
}

static void XID_PostGamepad(void *userdata, void *handle, const SDL_XIDGamepadState *state)
{
    SDL_DriverXID_Context *ctx = (SDL_DriverXID_Context *)userdata;
    SDL_Joystick *joystick = (SDL_Joystick *)handle;
    const Uint64 timestamp = SDL_GetTicksNS();
    int i;

    for (i = 0; i < ctx->session.identity.nbuttons; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, (state->buttons & (1u << i)) != 0);
    }
    if (ctx->session.identity.nhats) {
        SDL_SendJoystickHat(timestamp, joystick, 0, state->hat);
    }
    for (i = 0; i < SDL_XID_GAMEPAD_AXES; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, state->axes[i]);
    }
}

static void XID_PostSteelBattalion(void *userdata, void *handle, const SDL_XIDSteelBattalionState *state)
{
    SDL_Joystick *joystick = (SDL_Joystick *)handle;
    const Uint64 timestamp = SDL_GetTicksNS();
    int i;

    (void)userdata;
    for (i = 0; i < SDL_XID_SB_BUTTONS; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, (state->buttons & ((Uint64)1 << i)) != 0);
    }
    for (i = 0; i < SDL_XID_SB_AXES; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, state->axes[i]);
    }
}

static SDL_XIDSink HIDAPI_DriverXID_Sink(SDL_DriverXID_Context *ctx)
{
    SDL_XIDSink sink;

    sink.userdata = ctx;
    sink.control_in = XID_ControlIn;
    sink.read = XID_Read;
    sink.write = XID_Write;
    sink.joystick = XID_Joystick;
    sink.gamepad = XID_PostGamepad;
    sink.steel_battalion = XID_PostSteelBattalion;
    return sink;
}

static bool HIDAPI_DriverXID_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverXID_Context *ctx;
    SDL_XIDSink sink;
    bool opened;

    ctx = (SDL_DriverXID_Context *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return false;
    }
    ctx->device = device;

    device->context = ctx;

    /* One GET_DESCRIPTOR picks the decoder. Without an answer the ID tables
     * decide. The gamepad family then reads its current report. These are
     * the only control requests, so libusb is held just for them. */
    if (SDL_InitLibUSB(&ctx->libusb)) {
        ctx->handle = (libusb_device_handle *)SDL_GetPointerProperty(SDL_hid_get_properties(device->dev), SDL_PROP_HIDAPI_LIBUSB_DEVICE_HANDLE_POINTER, NULL);
    } else {
        ctx->libusb = NULL;
    }
    sink = HIDAPI_DriverXID_Sink(ctx);
    opened = SDL_XID_Open(&ctx->session, device->vendor_id, device->product_id, (uint8_t)device->interface_number,
                          device->product_string, &sink);
    if (ctx->libusb) {
        SDL_QuitLibUSB();
    }
    ctx->libusb = NULL;
    ctx->handle = NULL;
    if (!opened) {
        return SDL_SetError("XID device type not supported");
    }

    HIDAPI_SetDeviceName(device, ctx->session.identity.name);
    device->joystick_type = (SDL_JoystickType)ctx->session.identity.joystick_type;
    device->guid.data[15] = ctx->session.identity.guid_byte;

    return HIDAPI_JoystickConnected(device, &ctx->joystick_id);
}

static int HIDAPI_DriverXID_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverXID_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverXID_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverXID_Context *ctx = (SDL_DriverXID_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->naxes = ctx->session.identity.naxes;
    joystick->nbuttons = ctx->session.identity.nbuttons;
    joystick->nhats = ctx->session.identity.nhats;

    // The axes are allocated after this returns, so the next update sends the state
    SDL_XID_Start(&ctx->session);
    return true;
}

static bool HIDAPI_DriverXID_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    SDL_DriverXID_Context *ctx = (SDL_DriverXID_Context *)device->context;
    SDL_XIDSink sink = HIDAPI_DriverXID_Sink(ctx);

    switch (SDL_XID_Rumble(&ctx->session, low_frequency_rumble, high_frequency_rumble, &sink)) {
    case SDL_XID_OUTPUT_SENT:
        return true;
    case SDL_XID_OUTPUT_UNSUPPORTED:
        return SDL_Unsupported();
    default:
        return SDL_SetError("Couldn't send rumble packet");
    }
}

static bool HIDAPI_DriverXID_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverXID_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverXID_Context *ctx = (SDL_DriverXID_Context *)device->context;

    return (ctx->session.identity.kind == SDL_XID_KIND_GAMEPAD) ? SDL_JOYSTICK_CAP_RUMBLE : 0;
}

static bool HIDAPI_DriverXID_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverXID_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    SDL_DriverXID_Context *ctx = (SDL_DriverXID_Context *)device->context;
    SDL_XIDSink sink = HIDAPI_DriverXID_Sink(ctx);

    // The Steel Battalion lamps: 22 bytes, one brightness nibble per lamp
    switch (SDL_XID_SendEffect(&ctx->session, (const uint8_t *)data, (size > 0) ? (size_t)size : 0, &sink)) {
    case SDL_XID_OUTPUT_SENT:
        return true;
    case SDL_XID_OUTPUT_UNSUPPORTED:
        return SDL_Unsupported();
    case SDL_XID_OUTPUT_INVALID:
        return SDL_SetError("Steel Battalion lamp effects are %d bytes", SDL_XID_SB_LAMP_EFFECT_LENGTH);
    default:
        return SDL_SetError("Couldn't send lamp packet");
    }
}

static bool HIDAPI_DriverXID_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverXID_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverXID_Context *ctx = (SDL_DriverXID_Context *)device->context;
    SDL_XIDSink sink = HIDAPI_DriverXID_Sink(ctx);

    /* The device sends a report only on a change, so silence is not a
     * disconnect. A read error is. */
    if (!SDL_XID_Update(&ctx->session, &sink)) {
        if (ctx->joystick_id) {
            HIDAPI_JoystickDisconnected(device, ctx->joystick_id);
            ctx->joystick_id = 0;
        }
        return false;
    }
    return true;
}

static void HIDAPI_DriverXID_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverXID_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverXboxOriginal = {
    SDL_HINT_JOYSTICK_HIDAPI_XBOX_ORIGINAL,
    true,
    HIDAPI_DriverXboxOriginal_RegisterHints,
    HIDAPI_DriverXboxOriginal_UnregisterHints,
    HIDAPI_DriverXboxOriginal_IsEnabled,
    HIDAPI_DriverXboxOriginal_IsSupportedDevice,
    HIDAPI_DriverXID_InitDevice,
    HIDAPI_DriverXID_GetDevicePlayerIndex,
    HIDAPI_DriverXID_SetDevicePlayerIndex,
    HIDAPI_DriverXID_UpdateDevice,
    HIDAPI_DriverXID_OpenJoystick,
    HIDAPI_DriverXID_RumbleJoystick,
    HIDAPI_DriverXID_RumbleJoystickTriggers,
    HIDAPI_DriverXID_GetJoystickCapabilities,
    HIDAPI_DriverXID_SetJoystickLED,
    HIDAPI_DriverXID_SendJoystickEffect,
    HIDAPI_DriverXID_SetJoystickSensorsEnabled,
    HIDAPI_DriverXID_CloseJoystick,
    HIDAPI_DriverXID_FreeDevice,
};

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverSteelBattalion = {
    SDL_HINT_JOYSTICK_HIDAPI_STEEL_BATTALION,
    true,
    HIDAPI_DriverSteelBattalion_RegisterHints,
    HIDAPI_DriverSteelBattalion_UnregisterHints,
    HIDAPI_DriverSteelBattalion_IsEnabled,
    HIDAPI_DriverSteelBattalion_IsSupportedDevice,
    HIDAPI_DriverXID_InitDevice,
    HIDAPI_DriverXID_GetDevicePlayerIndex,
    HIDAPI_DriverXID_SetDevicePlayerIndex,
    HIDAPI_DriverXID_UpdateDevice,
    HIDAPI_DriverXID_OpenJoystick,
    HIDAPI_DriverXID_RumbleJoystick,
    HIDAPI_DriverXID_RumbleJoystickTriggers,
    HIDAPI_DriverXID_GetJoystickCapabilities,
    HIDAPI_DriverXID_SetJoystickLED,
    HIDAPI_DriverXID_SendJoystickEffect,
    HIDAPI_DriverXID_SetJoystickSensorsEnabled,
    HIDAPI_DriverXID_CloseJoystick,
    HIDAPI_DriverXID_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_XID

#endif // SDL_JOYSTICK_HIDAPI
