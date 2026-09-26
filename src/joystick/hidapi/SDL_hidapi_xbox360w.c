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
#include "SDL_hidapi_xbox360acc_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_XBOX360

// Define this if you want to log all packets from the controller
// #define DEBUG_XBOX_PROTOCOL

SDL_COMPILE_TIME_ASSERT(xbox360acc_hat_up, SDL_XBOX360ACC_HAT_UP == SDL_HAT_UP);
SDL_COMPILE_TIME_ASSERT(xbox360acc_hat_right, SDL_XBOX360ACC_HAT_RIGHT == SDL_HAT_RIGHT);
SDL_COMPILE_TIME_ASSERT(xbox360acc_hat_down, SDL_XBOX360ACC_HAT_DOWN == SDL_HAT_DOWN);
SDL_COMPILE_TIME_ASSERT(xbox360acc_hat_left, SDL_XBOX360ACC_HAT_LEFT == SDL_HAT_LEFT);

/* Each receiver slot carries a pad or an Xbox 360 uDraw GameTablet, and a pad
 * can carry a chatpad (hifihedgehog/SDL#33 Part 15). The slot's kind, the
 * chatpad's commands and keys and the tablet's pen live in
 * SDL_hidapi_xbox360acc_proto.c, where the offline tests run them. With the
 * uDraw hint on, a slot's joystick connects when the link control packet
 * gives its subtype, or on its first pad state packet. */
typedef struct
{
    SDL_HIDAPI_Device *device;
    int player_index;
    bool player_lights;
    SDL_xinput_capabilities capabilities;
    Uint8 last_state[USB_PACKET_LENGTH];
    SDL_Xbox360AccSlot slot;
    SDL_JoystickID main_id;    // The slot's pad or tablet
    Uint8 main_kind;           // What main_id is, SDL_XBOX360ACC_SLOT_*
    bool tablet_post_pending;  // The tablet opened and has not had the state yet
    SDL_JoystickID chatpad_id;
    bool chatpad_post_pending; // The chatpad opened and has not had the state yet
    Uint64 chatpad_sent;       // The buttons the chatpad joystick has
} SDL_DriverXbox360W_Context;

static bool IsMainJoystick(SDL_DriverXbox360W_Context *ctx, SDL_Joystick *joystick, Uint8 kind)
{
    return ctx->main_id && joystick && joystick->instance_id == ctx->main_id && ctx->main_kind == kind;
}

static bool IsChatpadJoystick(SDL_DriverXbox360W_Context *ctx, SDL_Joystick *joystick)
{
    return ctx->chatpad_id && joystick && joystick->instance_id == ctx->chatpad_id;
}

static void HIDAPI_DriverXbox360W_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX, callback, userdata);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360, callback, userdata);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_WIRELESS, callback, userdata);
}

static void HIDAPI_DriverXbox360W_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX, callback, userdata);
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360, callback, userdata);
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_WIRELESS, callback, userdata);
}

static bool HIDAPI_DriverXbox360W_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_WIRELESS,
                              SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT))));
}

static bool HIDAPI_DriverXbox360W_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    const int XB360W_IFACE_PROTOCOL = 129; // Wireless

    if ((vendor_id == USB_VENDOR_MICROSOFT) && (product_id == USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER)) {
        // This is the BigButton wireless receiver, which talks a different protocol
        return false;
    }
    if ((vendor_id == USB_VENDOR_MICROSOFT && (product_id == USB_PRODUCT_XBOX360_WIRELESS_RECEIVER_THIRDPARTY2 || product_id == USB_PRODUCT_XBOX360_WIRELESS_RECEIVER_THIRDPARTY1 || product_id == USB_PRODUCT_XBOX360_WIRELESS_RECEIVER) && interface_protocol == 0) ||
        (type == SDL_GAMEPAD_TYPE_XBOX360 && interface_protocol == XB360W_IFACE_PROTOCOL)) {
        return true;
    }
    return false;
}

static bool SetSlotLED(SDL_hid_device *dev, Uint8 slot, bool on)
{
    const bool blink = false;
    Uint8 mode = on ? ((blink ? 0x02 : 0x06) + slot) : 0;
    Uint8 led_packet[] = { 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    led_packet[3] = 0x40 + (mode % 0x0e);
    if (SDL_hid_write(dev, led_packet, sizeof(led_packet)) != sizeof(led_packet)) {
        return false;
    }
    return true;
}

static void UpdateSlotLED(SDL_DriverXbox360W_Context *ctx)
{
    if (ctx->player_lights && ctx->player_index >= 0) {
        SetSlotLED(ctx->device->dev, (ctx->player_index % 4), true);
    } else if (ctx->player_lights && ctx->main_kind == SDL_XBOX360ACC_SLOT_TABLET) {
        // A tablet has no player index, so it lights the quadrant of its
        // slot, the even interfaces 0 to 6
        int slot = (ctx->device->interface_number > 0) ? (ctx->device->interface_number / 2) : 0;
        SetSlotLED(ctx->device->dev, (Uint8)(slot % 4), true);
    } else {
        SetSlotLED(ctx->device->dev, 0, false);
    }
}

static void SDLCALL SDL_PlayerLEDHintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    SDL_DriverXbox360W_Context *ctx = (SDL_DriverXbox360W_Context *)userdata;
    bool player_lights = SDL_GetStringBoolean(hint, true);

    if (player_lights != ctx->player_lights) {
        ctx->player_lights = player_lights;

        UpdateSlotLED(ctx);
        HIDAPI_UpdateDeviceProperties(ctx->device);
    }
}

static void UpdatePowerLevel(SDL_Joystick *joystick, Uint8 level)
{
    int percent = (int)SDL_roundf((level / 255.0f) * 100.0f);
    SDL_SendJoystickPowerInfo(joystick, SDL_POWERSTATE_ON_BATTERY, percent);
}

// The chatpad and a tablet are named apart from the pad, which the core asks
// for by instance
static const char *HIDAPI_DriverXbox360W_GetJoystickName(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    const SDL_DriverXbox360W_Context *ctx = (const SDL_DriverXbox360W_Context *)device->context;

    if (!ctx || !instance_id) {
        return NULL;
    }
    if (instance_id == ctx->chatpad_id) {
        return SDL_XBOX360ACC_CHATPAD_NAME;
    }
    if (instance_id == ctx->main_id && ctx->main_kind == SDL_XBOX360ACC_SLOT_TABLET) {
        return SDL_XBOX360ACC_UDRAW_NAME;
    }
    return NULL;
}

// Their GUIDs: the device's, with the CRC of their own name and the byte 15
// that keeps them off the gamepad mappings
static bool HIDAPI_DriverXbox360W_GetJoystickGUID(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, SDL_GUID *guid)
{
    const SDL_DriverXbox360W_Context *ctx = (const SDL_DriverXbox360W_Context *)device->context;
    const char *name = HIDAPI_DriverXbox360W_GetJoystickName(device, instance_id);

    // A name means a context and the chatpad's or the tablet's ID
    if (!name) {
        return false;
    }
    *guid = device->guid;
    SDL_SetJoystickGUIDCRC(guid, SDL_crc16(0, name, SDL_strlen(name)));
    guid->data[15] = (instance_id == ctx->chatpad_id) ? SDL_XBOX360ACC_GUID_CHATPAD : SDL_XBOX360ACC_GUID_UDRAW;
    return true;
}

static bool HIDAPI_DriverXbox360W_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverXbox360W_Context *ctx;

    // Requests controller presence information from the wireless dongle
    const Uint8 init_packet[] = { 0x08, 0x00, 0x0F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    HIDAPI_SetDeviceName(device, "Xbox 360 Wireless Controller");

    ctx = (SDL_DriverXbox360W_Context *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return false;
    }
    ctx->device = device;
    {
        const bool receiver = SDL_Xbox360Acc_ReceiverSupported(device->vendor_id, device->product_id);

        SDL_Xbox360Acc_SlotInit(&ctx->slot,
            receiver && SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_CHATPAD,
                                           SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360,
                                                              SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX,
                                                                                 SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT)))),
            receiver && SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_UDRAW, HIDAPI_DriverXbox360W_IsEnabled()));
    }

    device->context = ctx;
    if (ctx->slot.chatpad_enabled || ctx->slot.udraw_enabled) {
        device->GetJoystickName = HIDAPI_DriverXbox360W_GetJoystickName;
        device->GetJoystickGUID = HIDAPI_DriverXbox360W_GetJoystickGUID;
    }

    if (SDL_hid_write(device->dev, init_packet, sizeof(init_packet)) != sizeof(init_packet)) {
        SDL_SetError("Couldn't write init packet");
        return false;
    }

    device->type = SDL_GAMEPAD_TYPE_XBOX360;

    return true;
}

static int HIDAPI_DriverXbox360W_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverXbox360W_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
    SDL_DriverXbox360W_Context *ctx = (SDL_DriverXbox360W_Context *)device->context;

    if (!ctx || instance_id != ctx->main_id) {
        return;
    }

    ctx->player_index = player_index;

    UpdateSlotLED(ctx);
}

static bool HIDAPI_DriverXbox360W_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverXbox360W_Context *ctx = (SDL_DriverXbox360W_Context *)device->context;

    SDL_AssertJoysticksLocked();

    if (IsChatpadJoystick(ctx, joystick)) {
        joystick->nbuttons = SDL_XBOX360ACC_CHATPAD_BUTTONS;
        joystick->naxes = 0;
        joystick->nhats = 0;
        joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
        // The buttons are allocated after this returns, so the next update sends them
        ctx->chatpad_post_pending = true;
        return true;
    }
    if (IsMainJoystick(ctx, joystick, SDL_XBOX360ACC_SLOT_TABLET)) {
        ctx->player_index = SDL_GetJoystickPlayerIndex(joystick);
        ctx->player_lights = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_PLAYER_LED, true);
        UpdateSlotLED(ctx);

        SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_PLAYER_LED,
                            SDL_PlayerLEDHintChanged, ctx);

        joystick->nbuttons = SDL_XBOX360ACC_TABLET_BUTTONS;
        joystick->naxes = SDL_XBOX360ACC_TABLET_AXES;
        joystick->nhats = 1;
        joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
        // The axes are allocated after this returns, so the next update sends them
        ctx->tablet_post_pending = true;
        return true;
    }
    if (!IsMainJoystick(ctx, joystick, SDL_XBOX360ACC_SLOT_PAD)) {
        // A pad, tablet or chatpad that the update before this open removed
        return false;
    }

    SDL_zeroa(ctx->last_state);

    // Initialize player index (needed for setting LEDs)
    ctx->player_index = SDL_GetJoystickPlayerIndex(joystick);
    ctx->player_lights = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_PLAYER_LED, true);
    UpdateSlotLED(ctx);

    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_PLAYER_LED,
                        SDL_PlayerLEDHintChanged, ctx);

    // Initialize the joystick capabilities
    joystick->nbuttons = 11;
    joystick->naxes = SDL_GAMEPAD_AXIS_COUNT;
    joystick->nhats = 1;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
    ctx->capabilities.type = 1;
    ctx->capabilities.flags = FLAG_WIRELESS;
    ctx->capabilities.subType = SDL_JOYSTICK_TYPE_GAMEPAD;
    ctx->capabilities.gamepad.wButtons = 0xFFFF;
    ctx->capabilities.gamepad.bLeftTrigger = 0xFF;
    ctx->capabilities.gamepad.bRightTrigger = 0xFF;
    ctx->capabilities.gamepad.sThumbLX = 0xFFC0;
    ctx->capabilities.gamepad.sThumbLY = 0xFFC0;
    ctx->capabilities.gamepad.sThumbRX = 0xFFC0;
    ctx->capabilities.gamepad.sThumbRY = 0xFFC0;
    ctx->capabilities.vibration.wLeftMotorSpeed = 0xFFFF;
    ctx->capabilities.vibration.wRightMotorSpeed = 0xFFFF;

    return true;
}

static bool HIDAPI_DriverXbox360W_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    SDL_DriverXbox360W_Context *ctx = (SDL_DriverXbox360W_Context *)device->context;
    Uint8 rumble_packet[] = { 0x00, 0x01, 0x0f, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    if (IsChatpadJoystick(ctx, joystick) || IsMainJoystick(ctx, joystick, SDL_XBOX360ACC_SLOT_TABLET)) {
        return SDL_Unsupported();
    }

    rumble_packet[5] = (low_frequency_rumble >> 8);
    rumble_packet[6] = (high_frequency_rumble >> 8);

    if (SDL_HIDAPI_SendRumble(device, rumble_packet, sizeof(rumble_packet)) != sizeof(rumble_packet)) {
        return SDL_SetError("Couldn't send rumble packet");
    }
    return true;
}

static bool HIDAPI_DriverXbox360W_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverXbox360W_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverXbox360W_Context *ctx = (SDL_DriverXbox360W_Context *)device->context;
    Uint32 result = SDL_JOYSTICK_CAP_RUMBLE;

    if (IsChatpadJoystick(ctx, joystick)) {
        return 0;
    }
    if (IsMainJoystick(ctx, joystick, SDL_XBOX360ACC_SLOT_TABLET)) {
        result = 0;
    }
    if (ctx->player_lights) {
        result |= SDL_JOYSTICK_CAP_PLAYER_LED;
    }
    return result;
}

static bool HIDAPI_DriverXbox360W_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

// Sends the queued chatpad commands, 12 bytes each on the slot's OUT endpoint
static void HIDAPI_DriverXbox360W_SendCommands(SDL_HIDAPI_Device *device, SDL_DriverXbox360W_Context *ctx)
{
    Uint8 packet[SDL_XBOX360ACC_COMMAND_SIZE];

    while (SDL_Xbox360Acc_SlotTakeCommand(&ctx->slot, packet)) {
        SDL_hid_write(device->dev, packet, sizeof(packet));
    }
}

/* The chatpad's lamps: one byte, 00 to 03 to put out Shift, Green, Orange
   and People, 08 to 0B to light them, 04 to put out the backlight and 0C to
   light it. A lit lamp is sent again after each keep-alive. */
static bool HIDAPI_DriverXbox360W_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    SDL_DriverXbox360W_Context *ctx = (SDL_DriverXbox360W_Context *)device->context;

    if (IsChatpadJoystick(ctx, joystick)) {
        if (size <= 0 || !SDL_Xbox360Acc_SlotLamp(&ctx->slot, (const Uint8 *)data, (size_t)size)) {
            return SDL_SetError("The Xbox 360 chatpad takes one lamp byte, 00 to 04 or 08 to 0C");
        }
        HIDAPI_DriverXbox360W_SendCommands(device, ctx);
        return true;
    }
    return SDL_Unsupported();
}

static bool HIDAPI_DriverXbox360W_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverXbox360W_HandleStatePacket(SDL_Joystick *joystick, SDL_hid_device *dev, SDL_DriverXbox360W_Context *ctx, Uint8 *data, int size)
{
    Sint16 axis;
    const bool invert_y_axes = true;
    Uint64 timestamp = SDL_GetTicksNS();

    if (ctx->last_state[2] != data[2]) {
        Uint8 hat = 0;

        if (data[2] & 0x01) {
            hat |= SDL_HAT_UP;
        }
        if (data[2] & 0x02) {
            hat |= SDL_HAT_DOWN;
        }
        if (data[2] & 0x04) {
            hat |= SDL_HAT_LEFT;
        }
        if (data[2] & 0x08) {
            hat |= SDL_HAT_RIGHT;
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);

        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data[2] & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data[2] & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data[2] & 0x40) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, ((data[2] & 0x80) != 0));
    }

    if (ctx->last_state[3] != data[3]) {
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[3] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data[3] & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[3] & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[3] & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[3] & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[3] & 0x40) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[3] & 0x80) != 0));
    }

    axis = ((int)data[4] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis);
    axis = ((int)data[5] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis);
    axis = SDL_Swap16LE(*(Sint16 *)(&data[6]));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, axis);
    axis = SDL_Swap16LE(*(Sint16 *)(&data[8]));
    if (invert_y_axes) {
        axis = ~axis;
    }
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, axis);
    axis = SDL_Swap16LE(*(Sint16 *)(&data[10]));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis);
    axis = SDL_Swap16LE(*(Sint16 *)(&data[12]));
    if (invert_y_axes) {
        axis = ~axis;
    }
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, axis);

    SDL_memcpy(ctx->last_state, data, SDL_min(size, sizeof(ctx->last_state)));
}

static void HIDAPI_DriverXbox360W_PostTablet(SDL_Joystick *joystick, const SDL_Xbox360AccTablet *tablet)
{
    Uint64 timestamp = SDL_GetTicksNS();
    int i;

    for (i = 0; i < SDL_XBOX360ACC_TABLET_AXES; ++i) {
        /* Pen, pressure and tilt are data: seed past the anti-jitter gate,
           as the Wii uDraw's pen axes are */
        SDL_SeedJoystickDataAxis(joystick, (Uint8)i, tablet->axes[i]);
        SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, tablet->axes[i]);
    }
    for (i = 0; i < SDL_XBOX360ACC_TABLET_BUTTONS; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, ((tablet->buttons >> i) & 1) != 0);
    }
    SDL_SendJoystickHat(timestamp, joystick, 0, tablet->hat);
}

static void HIDAPI_DriverXbox360W_PostChatpad(SDL_Joystick *joystick, Uint64 buttons)
{
    Uint64 timestamp = SDL_GetTicksNS();
    int i;

    for (i = 0; i < SDL_XBOX360ACC_CHATPAD_BUTTONS; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, ((buttons >> i) & 1) != 0);
    }
}

// Connects and disconnects the joysticks the slot's state calls for, and
// sends the chatpad and a tablet that just opened their state. Each ID and
// the main kind are stored before the joystick is announced, so the name and
// GUID callbacks know them from the first event.
static void HIDAPI_DriverXbox360W_Sync(SDL_HIDAPI_Device *device, SDL_DriverXbox360W_Context *ctx)
{
    // The chatpad goes before the pad it belongs to
    if (ctx->chatpad_id && !ctx->slot.chatpad.present) {
        HIDAPI_JoystickDisconnected(device, ctx->chatpad_id);
        ctx->chatpad_id = 0;
    }
    if (ctx->main_kind != ctx->slot.kind) {
        if (ctx->main_id) {
            HIDAPI_JoystickDisconnected(device, ctx->main_id);
            ctx->main_id = 0;
        }
        ctx->tablet_post_pending = false;
        ctx->main_kind = ctx->slot.kind;
        if (ctx->main_kind != SDL_XBOX360ACC_SLOT_NONE && !HIDAPI_JoystickConnected(device, &ctx->main_id)) {
            // A joystick that failed to connect is tried again on the next packet
            ctx->main_kind = SDL_XBOX360ACC_SLOT_NONE;
        }
    }
    if (!ctx->chatpad_id && ctx->slot.chatpad.present) {
        ctx->chatpad_sent = 0;
        HIDAPI_JoystickConnected(device, &ctx->chatpad_id);
    }

    if (ctx->chatpad_id && (ctx->chatpad_post_pending || ctx->slot.chatpad.buttons != ctx->chatpad_sent)) {
        SDL_Joystick *joystick = SDL_GetJoystickFromID(ctx->chatpad_id);

        if (joystick) {
            HIDAPI_DriverXbox360W_PostChatpad(joystick, ctx->slot.chatpad.buttons);
            ctx->chatpad_sent = ctx->slot.chatpad.buttons;
            ctx->chatpad_post_pending = false;
        }
    }
    if (ctx->tablet_post_pending && ctx->main_id && ctx->main_kind == SDL_XBOX360ACC_SLOT_TABLET) {
        SDL_Joystick *joystick = SDL_GetJoystickFromID(ctx->main_id);

        if (joystick) {
            HIDAPI_DriverXbox360W_PostTablet(joystick, &ctx->slot.tablet);
            ctx->tablet_post_pending = false;
        }
    }
}

static bool HIDAPI_DriverXbox360W_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverXbox360W_Context *ctx = (SDL_DriverXbox360W_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        int packet;

#ifdef DEBUG_XBOX_PROTOCOL
        HIDAPI_DumpPacket("Xbox 360 wireless packet: size = %d", data, size);
#endif
        // The slot's pad or tablet, when the application has it open
        joystick = ctx->main_id ? SDL_GetJoystickFromID(ctx->main_id) : NULL;

        packet = SDL_Xbox360Acc_SlotPacket(&ctx->slot, SDL_GetTicks(), data, (size_t)size);
        if (packet == SDL_XBOX360ACC_PACKET_STATUS) {
#ifdef DEBUG_JOYSTICK
            SDL_Log("Connected = %s", (data[1] & 0x80) ? "TRUE" : "FALSE");
#endif
        } else if (packet == SDL_XBOX360ACC_PACKET_INFO) {
            // Serial number is data[7-13]
#ifdef DEBUG_JOYSTICK
            SDL_Log("Battery status (initial): %d", data[17]);
#endif
            if (joystick) {
                UpdatePowerLevel(joystick, data[17]);
            }
            ctx->capabilities.type = 1;
            ctx->capabilities.subType = data[25] & 0x7f;
            if ((data[25] & 0x80) != 0) {
                ctx->capabilities.flags |= FLAG_FORCE_FEEDBACK;
            }
            switch (data[25] & 0x7f) {
                case 0x01: // XINPUT_DEVSUBTYPE_GAMEPAD
                    device->joystick_type = SDL_JOYSTICK_TYPE_GAMEPAD;
                    break;
                case 0x02: // XINPUT_DEVSUBTYPE_WHEEL
                    device->joystick_type = SDL_JOYSTICK_TYPE_WHEEL;
                    break;
                case 0x03: // XINPUT_DEVSUBTYPE_ARCADE_STICK
                    device->joystick_type = SDL_JOYSTICK_TYPE_ARCADE_STICK;
                    break;
                case 0x04: // XINPUT_DEVSUBTYPE_FLIGHT_STICK
                    device->joystick_type = SDL_JOYSTICK_TYPE_FLIGHT_STICK;
                    break;
                case 0x05: // XINPUT_DEVSUBTYPE_DANCE_PAD
                    device->joystick_type = SDL_JOYSTICK_TYPE_DANCE_PAD;
                    break;
                case 0x06: // XINPUT_DEVSUBTYPE_GUITAR
                case 0x07: // XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE
                case 0x0B: // XINPUT_DEVSUBTYPE_GUITAR_BASS
                    device->joystick_type = SDL_JOYSTICK_TYPE_GUITAR;
                    break;
                case 0x08: // XINPUT_DEVSUBTYPE_DRUM_KIT
                    device->joystick_type = SDL_JOYSTICK_TYPE_DRUM_KIT;
                    break;
                case 0x13: // XINPUT_DEVSUBTYPE_ARCADE_PAD
                    device->joystick_type = SDL_JOYSTICK_TYPE_ARCADE_PAD;
                    break;
            }
            device->guid.data[15] = ctx->capabilities.subType;
            const Uint8 capabilities_packet[] = { 0x00, 0x00, 0x02, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            if (SDL_hid_write(device->dev, capabilities_packet, sizeof(capabilities_packet)) != sizeof(capabilities_packet)) {
                SDL_SetError("Couldn't write capabilities_packet packet");
            }
        } else if (size == 29 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x13) {
#ifdef DEBUG_JOYSTICK
            SDL_Log("Battery status: %d", data[4]);
#endif
            if (joystick) {
                UpdatePowerLevel(joystick, data[4]);
            }
        } else if (data[0] == 0x00 && data[1] == 0x05 && data[5] == 0x12) {
            ctx->capabilities.gamepad.wButtons = LOAD16(data[6], data[7]);
            ctx->capabilities.gamepad.bLeftTrigger = data[8];
            ctx->capabilities.gamepad.bRightTrigger = data[9];
            ctx->capabilities.gamepad.sThumbLX = LOAD16(data[10], data[11]);
            ctx->capabilities.gamepad.sThumbLY = LOAD16(data[12], data[13]);
            ctx->capabilities.gamepad.sThumbRX = LOAD16(data[14], data[15]);
            ctx->capabilities.gamepad.sThumbRY = LOAD16(data[16], data[17]);
            ctx->capabilities.flags |= data[20];
            ctx->capabilities.vibration.wLeftMotorSpeed = data[18] << 8;
            ctx->capabilities.vibration.wRightMotorSpeed = data[19] << 8;
#ifdef DEBUG_XBOX_PROTOCOL
            SDL_Log("Xbox 360 capabilities:");
            SDL_Log("   type: %02x", ctx->capabilities.type);
            SDL_Log("   subType: %02x", ctx->capabilities.subType);
            SDL_Log("   flags: %02x", ctx->capabilities.flags);
            SDL_Log("   wButtons: %02x", ctx->capabilities.gamepad.wButtons);
            SDL_Log("   bLeftTrigger: %02x", ctx->capabilities.gamepad.bLeftTrigger);
            SDL_Log("   bRightTrigger: %02x", ctx->capabilities.gamepad.bRightTrigger);
            SDL_Log("   sThumbLX: %02x", ctx->capabilities.gamepad.sThumbLX);
            SDL_Log("   sThumbLY: %02x", ctx->capabilities.gamepad.sThumbLY);
            SDL_Log("   sThumbRX: %02x", ctx->capabilities.gamepad.sThumbRX);
            SDL_Log("   sThumbRY: %02x", ctx->capabilities.gamepad.sThumbRY);
            SDL_Log("   wLeftMotorSpeed: %02x", ctx->capabilities.vibration.wLeftMotorSpeed);
            SDL_Log("   wRightMotorSpeed: %02x", ctx->capabilities.vibration.wRightMotorSpeed);
#endif
        }

        // The slot's kind and chatpad may have changed
        HIDAPI_DriverXbox360W_Sync(device, ctx);
        joystick = ctx->main_id ? SDL_GetJoystickFromID(ctx->main_id) : NULL;

        if (packet == SDL_XBOX360ACC_PACKET_PAD) {
            if (joystick && ctx->main_kind == SDL_XBOX360ACC_SLOT_PAD) {
                HIDAPI_DriverXbox360W_HandleStatePacket(joystick, device->dev, ctx, data + 4, size - 4);
            }
        } else if (packet == SDL_XBOX360ACC_PACKET_TABLET) {
            if (joystick && ctx->main_kind == SDL_XBOX360ACC_SLOT_TABLET) {
                HIDAPI_DriverXbox360W_PostTablet(joystick, &ctx->slot.tablet);
                ctx->tablet_post_pending = false;
            }
        }
    }

    if (size < 0) {
        // Read error, device is disconnected
        SDL_Xbox360Acc_SlotLost(&ctx->slot);
        HIDAPI_DriverXbox360W_Sync(device, ctx);
    } else {
        SDL_Xbox360Acc_SlotUpdate(&ctx->slot, SDL_GetTicks());
        HIDAPI_DriverXbox360W_SendCommands(device, ctx);
        HIDAPI_DriverXbox360W_Sync(device, ctx);
    }
    return (size >= 0);
}

static void HIDAPI_DriverXbox360W_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverXbox360W_Context *ctx = (SDL_DriverXbox360W_Context *)device->context;

    if (IsChatpadJoystick(ctx, joystick)) {
        return;
    }

    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_PLAYER_LED,
                        SDL_PlayerLEDHintChanged, ctx);
}

static void HIDAPI_DriverXbox360W_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverXbox360W = {
    SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_WIRELESS,
    true,
    HIDAPI_DriverXbox360W_RegisterHints,
    HIDAPI_DriverXbox360W_UnregisterHints,
    HIDAPI_DriverXbox360W_IsEnabled,
    HIDAPI_DriverXbox360W_IsSupportedDevice,
    HIDAPI_DriverXbox360W_InitDevice,
    HIDAPI_DriverXbox360W_GetDevicePlayerIndex,
    HIDAPI_DriverXbox360W_SetDevicePlayerIndex,
    HIDAPI_DriverXbox360W_UpdateDevice,
    HIDAPI_DriverXbox360W_OpenJoystick,
    HIDAPI_DriverXbox360W_RumbleJoystick,
    HIDAPI_DriverXbox360W_RumbleJoystickTriggers,
    HIDAPI_DriverXbox360W_GetJoystickCapabilities,
    HIDAPI_DriverXbox360W_SetJoystickLED,
    HIDAPI_DriverXbox360W_SendJoystickEffect,
    HIDAPI_DriverXbox360W_SetJoystickSensorsEnabled,
    HIDAPI_DriverXbox360W_CloseJoystick,
    HIDAPI_DriverXbox360W_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_XBOX360

#endif // SDL_JOYSTICK_HIDAPI
