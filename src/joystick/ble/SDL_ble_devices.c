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

#include "SDL_ble_devices.h"
#include "SDL_ble_pokeball_proto.h"
#include "SDL_ble_daydream_proto.h"
#include "SDL_ble_gearvr_proto.h"
#include "SDL_ble_oculusgo_proto.h"
#include "SDL_ble_ghlios_proto.h"
#include "SDL_ble_zwift_proto.h"
#include "SDL_ble_myo_proto.h"

const SDL_BLEFamily *const SDL_BLE_Families[SDL_BLE_FAMILY_COUNT] = {
    &SDL_BLEPokeballFamily,
    &SDL_BLEDaydreamFamily,
    &SDL_BLEGearVRFamily,
    &SDL_BLEOculusGoFamily,
    &SDL_BLEGHLiOSFamily,
    &SDL_BLEZwiftFamily,
    &SDL_BLEMyoFamily
};
