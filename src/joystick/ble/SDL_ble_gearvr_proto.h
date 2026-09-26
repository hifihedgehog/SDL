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

/* Samsung's Gear VR controller: touchpad, buttons and three IMU samples per
 * 60-byte notification on the "Oculus Threemote" service, streaming only
 * after a host writes the start commands. See hifihedgehog/SDL#33 Part 12. */

#ifndef SDL_ble_gearvr_proto_h_
#define SDL_ble_gearvr_proto_h_

#include "SDL_ble_proto.h"

/* Characteristic indices */
#define SDL_GEARVR_DATA    0
#define SDL_GEARVR_COMMAND 1

#define SDL_GEARVR_PACKET_SIZE   60
#define SDL_GEARVR_ACK_WAIT_MS   4000  /* gearvr.py:158 */
#define SDL_GEARVR_KEEPALIVE_MS  10000
#define SDL_GEARVR_SILENCE_MS    3000  /* A stream stopped: about nine times the longest gap in the captures */
#define SDL_GEARVR_RESTARTS      2     /* Restarts in a row without a packet before the module gives up */

typedef enum SDL_GearVRPhase
{
    SDL_GEARVR_IDLE,
    SDL_GEARVR_WAIT_ACK, /* 08 00 written, its echo awaited */
    SDL_GEARVR_STREAMING /* 01 00 written, and the silence timer runs */
} SDL_GearVRPhase;

typedef struct SDL_GearVRState
{
    SDL_BLEBase base;
    uint8_t phase; /* SDL_GearVRPhase */
    uint64_t deadline;   /* The end of the echo's wait, or the next keep-alive */
    uint64_t silence;    /* No packet since 01 00 or the last packet by then is a stopped stream */
    uint8_t restarts;    /* Restarts since the last packet after 01 00 */
    bool have_time;
    uint32_t last_time;  /* The device's microsecond counter */
    uint64_t sensor_us;  /* Accumulated across its wraps */
} SDL_GearVRState;

extern const SDL_BLEModule SDL_BLEGearVRModule;
extern const SDL_BLEFamily SDL_BLEGearVRFamily;

#endif /* SDL_ble_gearvr_proto_h_ */
