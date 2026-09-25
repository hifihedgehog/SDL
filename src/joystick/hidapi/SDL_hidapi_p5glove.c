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
#include "SDL_hidapi_p5glove_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_P5GLOVE

/* The Essential Reality P5 glove, 0D7F:0100, on its native collection,
 * vendor page 0x8C usage 1. Report 1 packs five finger bends of 6 bits,
 * four buttons and four tracked LED slots, most significant bit first. The
 * driver reports the bends and the buttons. It reads and writes no feature
 * report, so mouse mode and every glove setting stay as they are. The
 * module also decodes the LED slots and the position replies for a later
 * position step. The protocol lives in SDL_hidapi_p5glove_proto.c, where
 * the offline tests run it. */

static void HIDAPI_DriverP5Glove_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_P5GLOVE, callback, userdata);
}

static void HIDAPI_DriverP5Glove_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_P5GLOVE, callback, userdata);
}

static bool HIDAPI_DriverP5Glove_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_P5GLOVE, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverP5Glove_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return vendor_id == USB_VENDOR_P5 && product_id == USB_PRODUCT_P5_GLOVE;
}

static bool HIDAPI_DriverP5Glove_InitDevice(SDL_HIDAPI_Device *device)
{
    HIDAPI_SetDeviceName(device, "P5 Glove");
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;
    return HIDAPI_JoystickConnected(device, NULL);
}

static bool HIDAPI_DriverP5Glove_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        SDL_P5GloveState state;

        if (joystick && SDL_P5Glove_DecodeReport(data, (size_t)size, &state)) {
            const Uint64 timestamp = SDL_GetTicksNS();
            Uint8 i;

            for (i = 0; i < SDL_P5GLOVE_FINGERS; ++i) {
                SDL_SendJoystickAxis(timestamp, joystick, i, SDL_P5Glove_FingerAxis(state.fingers[i]));
            }
            for (i = 0; i < SDL_P5GLOVE_BUTTONS; ++i) {
                SDL_SendJoystickButton(timestamp, joystick, i, (state.buttons & (1u << i)) != 0);
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

static bool HIDAPI_DriverP5Glove_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_AssertJoysticksLocked();

    joystick->naxes = SDL_P5GLOVE_FINGERS;
    joystick->nbuttons = SDL_P5GLOVE_BUTTONS;
    return true;
}

static int HIDAPI_DriverP5Glove_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverP5Glove_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverP5Glove_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverP5Glove_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverP5Glove_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverP5Glove_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverP5Glove_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverP5Glove_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverP5Glove_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverP5Glove_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverP5Glove = {
    SDL_HINT_JOYSTICK_HIDAPI_P5GLOVE,
    true,
    HIDAPI_DriverP5Glove_RegisterHints,
    HIDAPI_DriverP5Glove_UnregisterHints,
    HIDAPI_DriverP5Glove_IsEnabled,
    HIDAPI_DriverP5Glove_IsSupportedDevice,
    HIDAPI_DriverP5Glove_InitDevice,
    HIDAPI_DriverP5Glove_GetDevicePlayerIndex,
    HIDAPI_DriverP5Glove_SetDevicePlayerIndex,
    HIDAPI_DriverP5Glove_UpdateDevice,
    HIDAPI_DriverP5Glove_OpenJoystick,
    HIDAPI_DriverP5Glove_RumbleJoystick,
    HIDAPI_DriverP5Glove_RumbleJoystickTriggers,
    HIDAPI_DriverP5Glove_GetJoystickCapabilities,
    HIDAPI_DriverP5Glove_SetJoystickLED,
    HIDAPI_DriverP5Glove_SendJoystickEffect,
    HIDAPI_DriverP5Glove_SetJoystickSensorsEnabled,
    HIDAPI_DriverP5Glove_CloseJoystick,
    HIDAPI_DriverP5Glove_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_P5GLOVE

#endif // SDL_JOYSTICK_HIDAPI
