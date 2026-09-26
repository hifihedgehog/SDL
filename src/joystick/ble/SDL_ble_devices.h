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

/* The device table of the Bluetooth LE GATT joystick driver, in the order of
 * hifihedgehog/SDL#33 Part 12. Each family's hint follows the same order. */

#ifndef SDL_ble_devices_h_
#define SDL_ble_devices_h_

#include "SDL_ble_proto.h"

typedef enum SDL_BLEFamilyID
{
    SDL_BLE_FAMILY_POKEBALL,
    SDL_BLE_FAMILY_DAYDREAM,
    SDL_BLE_FAMILY_GEARVR,
    SDL_BLE_FAMILY_OCULUSGO,
    SDL_BLE_FAMILY_GHLIOS,
    SDL_BLE_FAMILY_ZWIFT,
    SDL_BLE_FAMILY_MYO,
    SDL_BLE_FAMILY_COUNT
} SDL_BLEFamilyID;

extern const SDL_BLEFamily *const SDL_BLE_Families[SDL_BLE_FAMILY_COUNT];

#endif /* SDL_ble_devices_h_ */
