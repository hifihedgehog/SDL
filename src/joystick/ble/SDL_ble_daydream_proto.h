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

/* Google's Daydream controller: a 20-byte bit-packed record of time,
 * orientation, motion, touchpad and buttons on service FE55. See
 * hifihedgehog/SDL#33 Part 12. */

#ifndef SDL_ble_daydream_proto_h_
#define SDL_ble_daydream_proto_h_

#include "SDL_ble_proto.h"

/* Characteristic indices */
#define SDL_DAYDREAM_POSE    0
#define SDL_DAYDREAM_BATTERY 1

#define SDL_DAYDREAM_REPORT_SIZE 20
#define SDL_DAYDREAM_VENDOR      0x18D1
#define SDL_DAYDREAM_PRODUCT     0x9210

typedef struct SDL_DaydreamState
{
    SDL_BLEBase base;
    bool started;        /* Start has run: the next record makes the joystick ready */
    bool have_time;
    uint16_t last_time;  /* The 9-bit millisecond counter */
    uint64_t sensor_ms;  /* Accumulated across its wraps */
} SDL_DaydreamState;

/* Stream bits, most significant first from bit 7 of byte 0 */
extern uint32_t SDL_Daydream_Bits(const uint8_t *data, int start, int count);
/* A 13-bit two's complement field */
extern int32_t SDL_Daydream_Signed13(const uint8_t *data, int start);

extern const SDL_BLEModule SDL_BLEDaydreamModule;
extern const SDL_BLEFamily SDL_BLEDaydreamFamily;

#endif /* SDL_ble_daydream_proto_h_ */
