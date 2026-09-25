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
#include "SDL_hidapi_gamevoice_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_GAMEVOICE

/* The Microsoft SideWinder Game Voice, 045E:003B: eight push buttons on a
 * telephony headset collection that Windows gives no game controller. The
 * report is one byte, a bit per button. The protocol lives in
 * SDL_hidapi_gamevoice_proto.c, where the offline tests run it. */

static void HIDAPI_DriverGameVoice_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_GAMEVOICE, callback, userdata);
}

static void HIDAPI_DriverGameVoice_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_GAMEVOICE, callback, userdata);
}

static bool HIDAPI_DriverGameVoice_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_GAMEVOICE, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverGameVoice_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return vendor_id == USB_VENDOR_MICROSOFT && product_id == USB_PRODUCT_MICROSOFT_SIDEWINDER_GAME_VOICE;
}

static bool HIDAPI_DriverGameVoice_InitDevice(SDL_HIDAPI_Device *device)
{
    HIDAPI_SetDeviceName(device, "SideWinder Game Voice");
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;
    return HIDAPI_JoystickConnected(device, NULL);
}

static bool HIDAPI_DriverGameVoice_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        Uint8 buttons;

        if (joystick && SDL_GameVoice_DecodeReport(data, (size_t)size, &buttons)) {
            const Uint64 timestamp = SDL_GetTicksNS();
            Uint8 i;

            for (i = 0; i < SDL_GAMEVOICE_BUTTONS; ++i) {
                SDL_SendJoystickButton(timestamp, joystick, i, (buttons & (1u << i)) != 0);
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

static bool HIDAPI_DriverGameVoice_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_AssertJoysticksLocked();

    joystick->nbuttons = SDL_GAMEVOICE_BUTTONS;
    return true;
}

static int HIDAPI_DriverGameVoice_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverGameVoice_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverGameVoice_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverGameVoice_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverGameVoice_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverGameVoice_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverGameVoice_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverGameVoice_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverGameVoice_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverGameVoice_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverGameVoice = {
    SDL_HINT_JOYSTICK_HIDAPI_GAMEVOICE,
    true,
    HIDAPI_DriverGameVoice_RegisterHints,
    HIDAPI_DriverGameVoice_UnregisterHints,
    HIDAPI_DriverGameVoice_IsEnabled,
    HIDAPI_DriverGameVoice_IsSupportedDevice,
    HIDAPI_DriverGameVoice_InitDevice,
    HIDAPI_DriverGameVoice_GetDevicePlayerIndex,
    HIDAPI_DriverGameVoice_SetDevicePlayerIndex,
    HIDAPI_DriverGameVoice_UpdateDevice,
    HIDAPI_DriverGameVoice_OpenJoystick,
    HIDAPI_DriverGameVoice_RumbleJoystick,
    HIDAPI_DriverGameVoice_RumbleJoystickTriggers,
    HIDAPI_DriverGameVoice_GetJoystickCapabilities,
    HIDAPI_DriverGameVoice_SetJoystickLED,
    HIDAPI_DriverGameVoice_SendJoystickEffect,
    HIDAPI_DriverGameVoice_SetJoystickSensorsEnabled,
    HIDAPI_DriverGameVoice_CloseJoystick,
    HIDAPI_DriverGameVoice_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_GAMEVOICE

#endif // SDL_JOYSTICK_HIDAPI
