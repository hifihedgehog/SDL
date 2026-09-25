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
#include "SDL_hidapi_nimbus_proto.h"
#include "../../hidapi/SDL_hidapi_collections.h"

#ifdef SDL_JOYSTICK_HIDAPI_NIMBUS

/* The SteelSeries Nimbus, a controller made for iPhone, 0111:1420 over
 * Bluetooth, which Windows leaves out of its game controllers. Each report
 * gives one byte per control: the D-pad, A, B, X, Y, the shoulders, the
 * triggers, Menu and the sticks as signed bytes. The report ID comes off
 * only when the collection declares one. The protocol lives in
 * SDL_hidapi_nimbus_proto.c, where the offline tests run it. */

typedef struct
{
    bool report_id_known;
    bool has_report_id;
} SDL_DriverNimbus_Context;

static void HIDAPI_DriverNimbus_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_NIMBUS, callback, userdata);
}

static void HIDAPI_DriverNimbus_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_NIMBUS, callback, userdata);
}

static bool HIDAPI_DriverNimbus_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_NIMBUS, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverNimbus_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return vendor_id == USB_VENDOR_STEELSERIES_BT && product_id == USB_PRODUCT_STEELSERIES_NIMBUS;
}

static bool HIDAPI_DriverNimbus_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverNimbus_Context *ctx = (SDL_DriverNimbus_Context *)SDL_calloc(1, sizeof(*ctx));
    unsigned char descriptor[4096];
    int length;

    if (!ctx) {
        return false;
    }
    device->context = ctx;

    length = SDL_hid_get_report_descriptor(device->dev, descriptor, sizeof(descriptor));
    if (length > 0) {
        SDL_HIDAPIReportIDs ids;

        if (SDL_HIDAPI_ParseReportIDs(descriptor, (size_t)length, &ids)) {
            ctx->has_report_id = ids.uses_report_ids;
            ctx->report_id_known = true;
        }
    }

    HIDAPI_SetDeviceName(device, "SteelSeries Nimbus");
    device->joystick_type = SDL_JOYSTICK_TYPE_GAMEPAD;
    return HIDAPI_JoystickConnected(device, NULL);
}

static bool HIDAPI_DriverNimbus_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverNimbus_Context *ctx = (SDL_DriverNimbus_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        SDL_NimbusState state;
        /* Without the descriptor, the length tells: the platform backends
           keep the report ID byte only when the collection uses one */
        const bool has_report_id = ctx->report_id_known ? ctx->has_report_id : (size == SDL_NIMBUS_PAYLOAD_LENGTH + 1);

        if (joystick && SDL_Nimbus_DecodeReport(data, (size_t)size, has_report_id, &state)) {
            const Uint64 timestamp = SDL_GetTicksNS();
            Uint8 i;

            for (i = 0; i < SDL_NIMBUS_BUTTONS; ++i) {
                SDL_SendJoystickButton(timestamp, joystick, i, (state.buttons & (1u << i)) != 0);
            }
            SDL_SendJoystickHat(timestamp, joystick, 0, state.hat);
            for (i = 0; i < SDL_NIMBUS_AXES; ++i) {
                SDL_SendJoystickAxis(timestamp, joystick, i, state.axes[i]);
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
    return true;
}

static bool HIDAPI_DriverNimbus_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_AssertJoysticksLocked();

    joystick->naxes = SDL_NIMBUS_AXES;
    joystick->nbuttons = SDL_NIMBUS_BUTTONS;
    joystick->nhats = 1;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
    return true;
}

static int HIDAPI_DriverNimbus_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverNimbus_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverNimbus_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverNimbus_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverNimbus_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverNimbus_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverNimbus_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverNimbus_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverNimbus_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverNimbus_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverNimbus = {
    SDL_HINT_JOYSTICK_HIDAPI_NIMBUS,
    true,
    HIDAPI_DriverNimbus_RegisterHints,
    HIDAPI_DriverNimbus_UnregisterHints,
    HIDAPI_DriverNimbus_IsEnabled,
    HIDAPI_DriverNimbus_IsSupportedDevice,
    HIDAPI_DriverNimbus_InitDevice,
    HIDAPI_DriverNimbus_GetDevicePlayerIndex,
    HIDAPI_DriverNimbus_SetDevicePlayerIndex,
    HIDAPI_DriverNimbus_UpdateDevice,
    HIDAPI_DriverNimbus_OpenJoystick,
    HIDAPI_DriverNimbus_RumbleJoystick,
    HIDAPI_DriverNimbus_RumbleJoystickTriggers,
    HIDAPI_DriverNimbus_GetJoystickCapabilities,
    HIDAPI_DriverNimbus_SetJoystickLED,
    HIDAPI_DriverNimbus_SendJoystickEffect,
    HIDAPI_DriverNimbus_SetJoystickSensorsEnabled,
    HIDAPI_DriverNimbus_CloseJoystick,
    HIDAPI_DriverNimbus_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_NIMBUS

#endif // SDL_JOYSTICK_HIDAPI
