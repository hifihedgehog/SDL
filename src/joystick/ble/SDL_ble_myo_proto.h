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

/* Thalmic Labs' Myo armband: classified hand poses and orientation on
 * vendor services. See hifihedgehog/SDL#33 Part 12. */

#ifndef SDL_ble_myo_proto_h_
#define SDL_ble_myo_proto_h_

#include "SDL_ble_proto.h"

/* Characteristic indices */
#define SDL_MYO_IMU        0
#define SDL_MYO_CLASSIFIER 1
#define SDL_MYO_MOTION     2
#define SDL_MYO_COMMAND    3
#define SDL_MYO_BATTERY    4

#define SDL_MYO_IMU_SIZE        20
#define SDL_MYO_CLASSIFIER_SIZE 3

/* Buttons, one per pose */
#define SDL_MYO_BUTTON_FIST       0
#define SDL_MYO_BUTTON_WAVE_IN    1
#define SDL_MYO_BUTTON_WAVE_OUT   2
#define SDL_MYO_BUTTON_SPREAD     3
#define SDL_MYO_BUTTON_DOUBLE_TAP 4

/* Axes */
#define SDL_MYO_AXIS_ROLL  0
#define SDL_MYO_AXIS_PITCH 1
#define SDL_MYO_AXIS_YAW   2

typedef struct SDL_MyoState
{
    SDL_BLEBase base;
    bool started;
} SDL_MyoState;

extern const SDL_BLEModule SDL_BLEMyoModule;
extern const SDL_BLEFamily SDL_BLEMyoFamily;

#endif /* SDL_ble_myo_proto_h_ */
