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

/* The iOS model of the Guitar Hero Live guitar: a PS3-style report as
 * notifications on a vendor characteristic. See hifihedgehog/SDL#33 Part 12. */

#ifndef SDL_ble_ghlios_proto_h_
#define SDL_ble_ghlios_proto_h_

#include "SDL_ble_proto.h"

/* Characteristic indices */
#define SDL_GHLIOS_INPUT 0

#define SDL_GHLIOS_REPORT_SIZE 20

typedef struct SDL_GHLiOSState
{
    SDL_BLEBase base;
    bool started; /* Start has run: the next report makes the joystick ready */
} SDL_GHLiOSState;

extern const SDL_BLEModule SDL_BLEGHLiOSModule;
extern const SDL_BLEFamily SDL_BLEGHLiOSFamily;

#endif /* SDL_ble_ghlios_proto_h_ */
