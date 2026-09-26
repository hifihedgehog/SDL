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
#include "SDL_hidapi_tacx_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_TACX

/* Tacx trainer head units (hifihedgehog/SDL#33 Part 14): the T1904 and the
 * T1932. Their interface 0 is read through libusb once WinUSB is bound, with
 * the transfer type its endpoint descriptors give. The head unit asks the
 * brake only after a frame from the host, so the version request and then a
 * stop frame every 100 ms go out from UpdateDevice, which runs for every
 * device whether or not a joystick is open. The joystick appears with the
 * first data reply and goes 1000 ms after the last one. The driver never sets
 * a resistance. The protocol and its timing live in SDL_hidapi_tacx_proto.c,
 * where the offline tests run them. */

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_TacxState state;
    bool post_pending; /* The joystick opened and has not had the state yet */
} SDL_DriverTacx_Context;

static void HIDAPI_DriverTacx_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_TACX, callback, userdata);
}

static void HIDAPI_DriverTacx_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_TACX, callback, userdata);
}

static bool HIDAPI_DriverTacx_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_TACX, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverTacx_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    /* Interface 0, whatever class it declares, since no source records it */
    return interface_number == 0 && SDL_Tacx_Identify(vendor_id, product_id) != SDL_TACX_NONE;
}

static bool HIDAPI_DriverTacx_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverTacx_Context *ctx = (SDL_DriverTacx_Context *)SDL_calloc(1, sizeof(*ctx));

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;
    SDL_Tacx_Init(&ctx->state, SDL_GetTicksNS());

    HIDAPI_SetDeviceName(device, SDL_Tacx_Name(SDL_Tacx_Identify(device->vendor_id, device->product_id)));
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;

    /* The joystick connects with the first data reply. A head unit whose
       brake does not answer sends none. */
    return true;
}

static void HIDAPI_DriverTacx_Post(SDL_DriverTacx_Context *ctx, SDL_Joystick *joystick)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    SDL_TacxControls controls;
    Uint8 i;

    SDL_Tacx_GetControls(&ctx->state.data, &controls);
    for (i = 0; i < SDL_TACX_AXES; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, i, controls.axes[i]);
    }
    for (i = 0; i < SDL_TACX_BUTTONS; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, i, (controls.buttons & (1u << i)) != 0);
    }
}

/* Turns what the module changed into SDL joystick calls */
static void HIDAPI_DriverTacx_Apply(SDL_DriverTacx_Context *ctx, int changed)
{
    SDL_HIDAPI_Device *device = ctx->device;

    if (changed & SDL_TACX_CHANGED_BRAKE) {
        const SDL_TacxBrake *brake = &ctx->state.brake;

        if (brake->magnetic) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "%s: brake firmware 0x%.8" SDL_PRIx32 ", serial 0, a magnetic brake",
                         device->name, brake->firmware);
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "%s: brake firmware 0x%.8" SDL_PRIx32 ", serial %" SDL_PRIu32 ", T19%.2u motor brake of 20%.2u, unit %" SDL_PRIu32,
                         device->name, brake->firmware, brake->serial, (unsigned int)brake->type, (unsigned int)brake->year, brake->number);
        }
    }
    if (changed & SDL_TACX_CHANGED_PRESENT) {
        if (ctx->state.present) {
            if (device->num_joysticks == 0) {
                HIDAPI_JoystickConnected(device, NULL);
            }
        } else if (device->num_joysticks > 0) {
            HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        }
    }
    if ((changed & SDL_TACX_CHANGED_CONTROLS) && device->num_joysticks > 0) {
        SDL_Joystick *joystick = SDL_GetJoystickFromID(device->joysticks[0]);

        if (joystick) {
            HIDAPI_DriverTacx_Post(ctx, joystick);
        }
    }
}

static bool HIDAPI_DriverTacx_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverTacx_Context *ctx = (SDL_DriverTacx_Context *)device->context;
    Uint8 data[SDL_TACX_READ_SIZE];
    Uint8 frame[SDL_TACX_FRAME_LENGTH];
    size_t length;
    int size;

    if (ctx->post_pending && device->num_joysticks > 0) {
        SDL_Joystick *joystick = SDL_GetJoystickFromID(device->joysticks[0]);

        if (joystick) {
            HIDAPI_DriverTacx_Post(ctx, joystick);
            ctx->post_pending = false;
        }
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
#ifdef DEBUG_TACX_PROTOCOL
        HIDAPI_DumpPacket("Tacx reply: size = %d", data, size);
#endif
        HIDAPI_DriverTacx_Apply(ctx, SDL_Tacx_HandleReply(&ctx->state, data, (size_t)size, SDL_GetTicksNS()));
    }

    if (size < 0) {
        // Read error, device is disconnected
        if (device->num_joysticks > 0) {
            HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        }
        return false;
    }

    HIDAPI_DriverTacx_Apply(ctx, SDL_Tacx_Tick(&ctx->state, SDL_GetTicksNS()));

    /* At most one frame per update, the next one due 100 ms after this
       write completes */
    if (SDL_Tacx_NextFrame(&ctx->state, SDL_GetTicksNS(), frame, &length)) {
        const int written = SDL_hid_write(device->dev, frame, length);
        SDL_Tacx_FrameDone(&ctx->state, written, SDL_GetTicksNS());
    }
    return true;
}

static bool HIDAPI_DriverTacx_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverTacx_Context *ctx = (SDL_DriverTacx_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->naxes = SDL_TACX_AXES;
    joystick->nbuttons = SDL_TACX_BUTTONS;
    // The axes are allocated after this returns, so the next update sends the state
    ctx->post_pending = true;
    return true;
}

static int HIDAPI_DriverTacx_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverTacx_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverTacx_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverTacx_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverTacx_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverTacx_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverTacx_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    // Resistance control belongs to the application's trainer code, not the joystick
    return SDL_Unsupported();
}

static bool HIDAPI_DriverTacx_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverTacx_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverTacx_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverTacx = {
    SDL_HINT_JOYSTICK_HIDAPI_TACX,
    true,
    HIDAPI_DriverTacx_RegisterHints,
    HIDAPI_DriverTacx_UnregisterHints,
    HIDAPI_DriverTacx_IsEnabled,
    HIDAPI_DriverTacx_IsSupportedDevice,
    HIDAPI_DriverTacx_InitDevice,
    HIDAPI_DriverTacx_GetDevicePlayerIndex,
    HIDAPI_DriverTacx_SetDevicePlayerIndex,
    HIDAPI_DriverTacx_UpdateDevice,
    HIDAPI_DriverTacx_OpenJoystick,
    HIDAPI_DriverTacx_RumbleJoystick,
    HIDAPI_DriverTacx_RumbleJoystickTriggers,
    HIDAPI_DriverTacx_GetJoystickCapabilities,
    HIDAPI_DriverTacx_SetJoystickLED,
    HIDAPI_DriverTacx_SendJoystickEffect,
    HIDAPI_DriverTacx_SetJoystickSensorsEnabled,
    HIDAPI_DriverTacx_CloseJoystick,
    HIDAPI_DriverTacx_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_TACX

#endif // SDL_JOYSTICK_HIDAPI
