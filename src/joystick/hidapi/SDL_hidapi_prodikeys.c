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
#include "SDL_hidapi_prodikeys_proto.h"
#include "../../hidapi/SDL_hidapi_collections.h"

#ifdef SDL_JOYSTICK_HIDAPI_PRODIKEYS

/* The Creative Prodikeys PC-MIDI, 041E:2801, a PC keyboard with 37 music
 * keys. The typing keys stay with Windows. Interface 1 carries report 3,
 * note and velocity pairs from the music keys, and report 4, a 24-bit mask
 * of the other keys. Linux's hid-prodikeys sends output report 6, 01 C1,
 * before the music keys report, and the driver sends it at open. Report 1
 * stays with Windows. The joystick has 152 buttons, MIDI notes 0-127 at
 * octave 0 and then the 24 mask bits, and axis 0 is the velocity of the
 * latest press. The protocol lives in SDL_hidapi_prodikeys_proto.c, where
 * the offline tests run it. */

typedef struct
{
    SDL_ProdikeysState state;
    SDL_ProdikeysState posted; /* What the joystick last saw */
    bool post_pending;         /* The joystick opened and has not had the state yet */
} SDL_DriverProdikeys_Context;

static void HIDAPI_DriverProdikeys_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_PRODIKEYS, callback, userdata);
}

static void HIDAPI_DriverProdikeys_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_PRODIKEYS, callback, userdata);
}

static bool HIDAPI_DriverProdikeys_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_PRODIKEYS, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

/* Whether the collection declares input report 3 or 4. Windows makes a
   device of each top-level collection of interface 1. */
static bool HIDAPI_DriverProdikeys_CarriesKeys(SDL_hid_device *dev)
{
    unsigned char descriptor[4096];
    SDL_HIDAPIReportIDs ids;
    const int length = SDL_hid_get_report_descriptor(dev, descriptor, sizeof(descriptor));

    return length > 0 && SDL_HIDAPI_ParseReportIDs(descriptor, (size_t)length, &ids) && SDL_Prodikeys_CarriesKeys(ids.input);
}

static bool HIDAPI_DriverProdikeys_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    if (vendor_id != USB_VENDOR_CREATIVE || product_id != USB_PRODUCT_CREATIVE_PRODIKEYS) {
        return false;
    }
    if (!device) {
        // Without a device the question is only whether the ID could be one
        return true;
    }
    if (interface_number != SDL_PRODIKEYS_INTERFACE) {
        return false;
    }
    // Once the collection is open, its descriptor decides
    return !device->dev || HIDAPI_DriverProdikeys_CarriesKeys(device->dev);
}

static bool HIDAPI_DriverProdikeys_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverProdikeys_Context *ctx = (SDL_DriverProdikeys_Context *)SDL_calloc(1, sizeof(*ctx));
    Uint8 start[SDL_PRODIKEYS_START_LENGTH];

    if (!ctx) {
        return false;
    }
    SDL_Prodikeys_Init(&ctx->state);
    ctx->posted = ctx->state;
    device->context = ctx;

    HIDAPI_SetDeviceName(device, "Creative Prodikeys PC-MIDI");
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;

    SDL_Prodikeys_BuildStart(start);
    if (SDL_hid_write(device->dev, start, sizeof(start)) < 0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Prodikeys: output report 6 not sent: %s", SDL_GetError());
    }
    return HIDAPI_JoystickConnected(device, NULL);
}

static void HIDAPI_DriverProdikeys_Post(SDL_DriverProdikeys_Context *ctx, SDL_Joystick *joystick, bool all)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    int i;

    for (i = 0; i < SDL_PRODIKEYS_BUTTONS; ++i) {
        const bool down = SDL_Prodikeys_IsButtonDown(&ctx->state, i);

        if (all || down != SDL_Prodikeys_IsButtonDown(&ctx->posted, i)) {
            SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, down);
        }
    }
    if (all || ctx->state.velocity != ctx->posted.velocity) {
        SDL_SendJoystickAxis(timestamp, joystick, 0, ctx->state.velocity);
    }
    ctx->posted = ctx->state;
}

static bool HIDAPI_DriverProdikeys_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverProdikeys_Context *ctx = (SDL_DriverProdikeys_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }
    if (joystick && ctx->post_pending) {
        // The velocity rests at -32768, which the joystick can only learn from an event
        HIDAPI_DriverProdikeys_Post(ctx, joystick, true);
        ctx->post_pending = false;
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        if (SDL_Prodikeys_HandleReport(&ctx->state, data, (size_t)size) && joystick) {
            HIDAPI_DriverProdikeys_Post(ctx, joystick, false);
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

static bool HIDAPI_DriverProdikeys_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverProdikeys_Context *ctx = (SDL_DriverProdikeys_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->naxes = 1;
    joystick->nbuttons = SDL_PRODIKEYS_BUTTONS;
    ctx->post_pending = true;
    return true;
}

static int HIDAPI_DriverProdikeys_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverProdikeys_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverProdikeys_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverProdikeys_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverProdikeys_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverProdikeys_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverProdikeys_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverProdikeys_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverProdikeys_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverProdikeys_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverProdikeys = {
    SDL_HINT_JOYSTICK_HIDAPI_PRODIKEYS,
    true,
    HIDAPI_DriverProdikeys_RegisterHints,
    HIDAPI_DriverProdikeys_UnregisterHints,
    HIDAPI_DriverProdikeys_IsEnabled,
    HIDAPI_DriverProdikeys_IsSupportedDevice,
    HIDAPI_DriverProdikeys_InitDevice,
    HIDAPI_DriverProdikeys_GetDevicePlayerIndex,
    HIDAPI_DriverProdikeys_SetDevicePlayerIndex,
    HIDAPI_DriverProdikeys_UpdateDevice,
    HIDAPI_DriverProdikeys_OpenJoystick,
    HIDAPI_DriverProdikeys_RumbleJoystick,
    HIDAPI_DriverProdikeys_RumbleJoystickTriggers,
    HIDAPI_DriverProdikeys_GetJoystickCapabilities,
    HIDAPI_DriverProdikeys_SetJoystickLED,
    HIDAPI_DriverProdikeys_SendJoystickEffect,
    HIDAPI_DriverProdikeys_SetJoystickSensorsEnabled,
    HIDAPI_DriverProdikeys_CloseJoystick,
    HIDAPI_DriverProdikeys_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_PRODIKEYS

#endif // SDL_JOYSTICK_HIDAPI
