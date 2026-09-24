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
#include "../SDL_ghl_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_GHL

/* The Guitar Hero Live dongles for PS3 and Wii U (12BA:074B) and for PS4
 * (1430:07BB). Each wants a keep-alive every 8 s, or the strum bar cuts out
 * held frets. It goes out as an output report on the control pipe, the
 * SET_REPORT every source sends. The protocol lives in
 * SDL_ghl_proto.c, where the offline tests run it. This file only
 * moves bytes between it and SDL. The Xbox One dongle is read by the
 * GameInput backend and the GIP driver.
 */

SDL_COMPILE_TIME_ASSERT(ghl_hat_up, SDL_GHL_HAT_UP == SDL_HAT_UP);
SDL_COMPILE_TIME_ASSERT(ghl_hat_right, SDL_GHL_HAT_RIGHT == SDL_HAT_RIGHT);
SDL_COMPILE_TIME_ASSERT(ghl_hat_down, SDL_GHL_HAT_DOWN == SDL_HAT_DOWN);
SDL_COMPILE_TIME_ASSERT(ghl_hat_left, SDL_GHL_HAT_LEFT == SDL_HAT_LEFT);
SDL_COMPILE_TIME_ASSERT(ghl_button_south, SDL_GHL_BUTTON_SOUTH == SDL_GAMEPAD_BUTTON_SOUTH);
SDL_COMPILE_TIME_ASSERT(ghl_button_east, SDL_GHL_BUTTON_EAST == SDL_GAMEPAD_BUTTON_EAST);
SDL_COMPILE_TIME_ASSERT(ghl_button_west, SDL_GHL_BUTTON_WEST == SDL_GAMEPAD_BUTTON_WEST);
SDL_COMPILE_TIME_ASSERT(ghl_button_north, SDL_GHL_BUTTON_NORTH == SDL_GAMEPAD_BUTTON_NORTH);
SDL_COMPILE_TIME_ASSERT(ghl_button_back, SDL_GHL_BUTTON_BACK == SDL_GAMEPAD_BUTTON_BACK);
SDL_COMPILE_TIME_ASSERT(ghl_button_guide, SDL_GHL_BUTTON_GUIDE == SDL_GAMEPAD_BUTTON_GUIDE);
SDL_COMPILE_TIME_ASSERT(ghl_button_start, SDL_GHL_BUTTON_START == SDL_GAMEPAD_BUTTON_START);
SDL_COMPILE_TIME_ASSERT(ghl_button_left_stick, SDL_GHL_BUTTON_LEFT_STICK == SDL_GAMEPAD_BUTTON_LEFT_STICK);
SDL_COMPILE_TIME_ASSERT(ghl_button_right_stick, SDL_GHL_BUTTON_RIGHT_STICK == SDL_GAMEPAD_BUTTON_RIGHT_STICK);
SDL_COMPILE_TIME_ASSERT(ghl_button_left_shoulder, SDL_GHL_BUTTON_LEFT_SHOULDER == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
SDL_COMPILE_TIME_ASSERT(ghl_button_right_shoulder, SDL_GHL_BUTTON_RIGHT_SHOULDER == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
SDL_COMPILE_TIME_ASSERT(ghl_axis_leftx, SDL_GHL_AXIS_LEFTX == SDL_GAMEPAD_AXIS_LEFTX);
SDL_COMPILE_TIME_ASSERT(ghl_axis_lefty, SDL_GHL_AXIS_LEFTY == SDL_GAMEPAD_AXIS_LEFTY);
SDL_COMPILE_TIME_ASSERT(ghl_axis_rightx, SDL_GHL_AXIS_RIGHTX == SDL_GAMEPAD_AXIS_RIGHTX);
SDL_COMPILE_TIME_ASSERT(ghl_axis_righty, SDL_GHL_AXIS_RIGHTY == SDL_GAMEPAD_AXIS_RIGHTY);
SDL_COMPILE_TIME_ASSERT(ghl_axis_left_trigger, SDL_GHL_AXIS_LEFT_TRIGGER == SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
SDL_COMPILE_TIME_ASSERT(ghl_axis_right_trigger, SDL_GHL_AXIS_RIGHT_TRIGGER == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

typedef struct
{
    SDL_HIDAPI_Device *device;
    int dongle;
    SDL_GHLKeepAlive keepalive;
    bool guitar_connected; /* PS4: byte 25 of the last report */
} SDL_DriverGHL_Context;

static void HIDAPI_DriverGHL_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_GHL, callback, userdata);
}

static void HIDAPI_DriverGHL_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_GHL, callback, userdata);
}

static bool HIDAPI_DriverGHL_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_GHL,
                              SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverGHL_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    const int dongle = SDL_GHL_GetDongle(vendor_id, product_id);

    return dongle == SDL_GHL_DONGLE_PS3 || dongle == SDL_GHL_DONGLE_PS4;
}

/* Sends the keep-alive when it is due. A failed send stays due, so the next
   update tries again. */
static void HIDAPI_DriverGHL_SendKeepAlive(SDL_DriverGHL_Context *ctx)
{
    const Uint64 now = SDL_GetTicks();
    Uint8 buffer[SDL_GHL_HID_KEEPALIVE_LENGTH];
    size_t length;
    bool sent;

    if (!SDL_GHL_KeepAliveDue(&ctx->keepalive, now)) {
        return;
    }
    length = SDL_GHL_BuildHIDKeepAlive(ctx->dongle, buffer);
    sent = (length > 0 && SDL_hid_send_output_report(ctx->device->dev, buffer, length) >= 0);
    SDL_GHL_KeepAliveSent(&ctx->keepalive, now, sent);
}

static bool HIDAPI_DriverGHL_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverGHL_Context *ctx;

    ctx = (SDL_DriverGHL_Context *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return false;
    }
    ctx->device = device;
    ctx->dongle = SDL_GHL_GetDongle(device->vendor_id, device->product_id);
    device->context = ctx;

    HIDAPI_SetDeviceName(device, SDL_GHL_DongleName(ctx->dongle));
    device->joystick_type = SDL_JOYSTICK_TYPE_GUITAR;
    device->type = (ctx->dongle == SDL_GHL_DONGLE_PS4) ? SDL_GAMEPAD_TYPE_PS4 : SDL_GAMEPAD_TYPE_PS3;

    /* The first keep-alive goes at open, as Santroller and RB4InstrumentMapper
       send it */
    SDL_GHL_KeepAliveStart(&ctx->keepalive, SDL_GetTicks());
    HIDAPI_DriverGHL_SendKeepAlive(ctx);

    if (ctx->dongle == SDL_GHL_DONGLE_PS3) {
        /* No connected flag is documented, so the guitar lives as long as
           the dongle */
        return HIDAPI_JoystickConnected(device, NULL);
    }
    /* The PS4 guitar joins when byte 25 of a report says it is there */
    return true;
}

static int HIDAPI_DriverGHL_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverGHL_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static void HIDAPI_DriverGHL_Post(SDL_Joystick *joystick, const SDL_GHLOutput *output)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    Uint8 i;

    for (i = 0; i < SDL_GHL_NUM_BUTTONS; ++i) {
        if (output->button_mask & (1u << i)) {
            SDL_SendJoystickButton(timestamp, joystick, i, (output->buttons & (1u << i)) != 0);
        }
    }
    for (i = 0; i < SDL_GHL_NUM_AXES; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, i, output->axes[i]);
    }
    SDL_SendJoystickHat(timestamp, joystick, 0, output->hat);
}

static bool HIDAPI_DriverGHL_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverGHL_Context *ctx = (SDL_DriverGHL_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        SDL_GHLOutput output;

#ifdef DEBUG_GHL_PROTOCOL
        HIDAPI_DumpPacket("Guitar Hero Live packet: size = %d", data, size);
#endif
        if (ctx->dongle == SDL_GHL_DONGLE_PS3) {
            if (SDL_GHL_DecodeFrameA(data, (size_t)size, &output) && joystick) {
                HIDAPI_DriverGHL_Post(joystick, &output);
            }
        } else {
            bool connected;

            if (!SDL_GHL_DecodeFrameB(data, (size_t)size, &output, &connected)) {
                continue;
            }
            if (connected != ctx->guitar_connected) {
                ctx->guitar_connected = connected;
                if (connected) {
                    HIDAPI_JoystickConnected(device, NULL);
                } else if (device->num_joysticks > 0) {
                    HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
                }
                joystick = (device->num_joysticks > 0) ? SDL_GetJoystickFromID(device->joysticks[0]) : NULL;
            }
            if (connected && joystick) {
                HIDAPI_DriverGHL_Post(joystick, &output);
            }
        }
    }

    if (size < 0) {
        // Read error, device is disconnected
        if (device->num_joysticks > 0) {
            HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        }
        return false;
    }

    HIDAPI_DriverGHL_SendKeepAlive(ctx);
    return true;
}

static bool HIDAPI_DriverGHL_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_AssertJoysticksLocked();

    joystick->nbuttons = SDL_GHL_NUM_BUTTONS;
    joystick->naxes = SDL_GHL_NUM_AXES;
    joystick->nhats = 1;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
    return true;
}

static bool HIDAPI_DriverGHL_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverGHL_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverGHL_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverGHL_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverGHL_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverGHL_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverGHL_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverGHL_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverGHL = {
    SDL_HINT_JOYSTICK_HIDAPI_GHL,
    true,
    HIDAPI_DriverGHL_RegisterHints,
    HIDAPI_DriverGHL_UnregisterHints,
    HIDAPI_DriverGHL_IsEnabled,
    HIDAPI_DriverGHL_IsSupportedDevice,
    HIDAPI_DriverGHL_InitDevice,
    HIDAPI_DriverGHL_GetDevicePlayerIndex,
    HIDAPI_DriverGHL_SetDevicePlayerIndex,
    HIDAPI_DriverGHL_UpdateDevice,
    HIDAPI_DriverGHL_OpenJoystick,
    HIDAPI_DriverGHL_RumbleJoystick,
    HIDAPI_DriverGHL_RumbleJoystickTriggers,
    HIDAPI_DriverGHL_GetJoystickCapabilities,
    HIDAPI_DriverGHL_SetJoystickLED,
    HIDAPI_DriverGHL_SendJoystickEffect,
    HIDAPI_DriverGHL_SetJoystickSensorsEnabled,
    HIDAPI_DriverGHL_CloseJoystick,
    HIDAPI_DriverGHL_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_GHL

#endif // SDL_JOYSTICK_HIDAPI
