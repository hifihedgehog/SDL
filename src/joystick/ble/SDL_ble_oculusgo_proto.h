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

/* The Oculus Go controller: accelerometer, touchpad and buttons on one
 * characteristic, gyroscope samples on another, streaming only to a bonded
 * host. See hifihedgehog/SDL#33 Part 12. */

#ifndef SDL_ble_oculusgo_proto_h_
#define SDL_ble_oculusgo_proto_h_

#include "SDL_ble_proto.h"

/* Characteristic indices */
#define SDL_OCULUSGO_FACE 0
#define SDL_OCULUSGO_BEEF 1

#define SDL_OCULUSGO_REPORT_SIZE    20
#define SDL_OCULUSGO_SILENCE_MS     3000 /* A silence, main.swift:647 */
#define SDL_OCULUSGO_RESUBSCRIBE_MS 1000 /* Requests while it lasts, the check's period, main.swift:646 */

typedef struct SDL_OculusGoState
{
    SDL_BLEBase base;
    bool started;
    bool silent;       /* Released for a silence, no 8126FACE value since */
    uint64_t deadline; /* 3 s after the last value, then 1 s after each request */
} SDL_OculusGoState;

extern const SDL_BLEModule SDL_BLEOculusGoModule;
extern const SDL_BLEFamily SDL_BLEOculusGoFamily;

#endif /* SDL_ble_oculusgo_proto_h_ */
