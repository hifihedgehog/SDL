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

/* Logitech CyberMan, protocol token "cyberman". After a reset, RTS dropped
 * for at least 100 ms while DTR is high, the device answers "M3" as a
 * 3-button serial mouse at 1200 baud 7N1. The host writes "*S" to enter
 * SWIFT mode, waits for it to leave, switches to 4800 baud 8N1 and asks for
 * the static status report with "!S". SWIFT reports start with a byte with
 * bit 7 set whose bits 6-5 give the type. "!T" drives the tactile motor. The
 * facts are from the CyberMan 3D SWIFT Supplement 1.0 (Logitech, 1994) and
 * its errata.
 */

#ifndef SDL_serial_cyberman_proto_h_
#define SDL_serial_cyberman_proto_h_

#include "SDL_serial_proto.h"

#define SDL_CYBERMAN_MOUSE_RATE    1200
#define SDL_CYBERMAN_SWIFT_RATE    4800
#define SDL_CYBERMAN_RTS_LOW_MS    200
#define SDL_CYBERMAN_M_MS          110 /* After RTS rises */
#define SDL_CYBERMAN_3_MS          100 /* After the M */
#define SDL_CYBERMAN_SETTLE_MS     20  /* After the drain, before the new rate */
#define SDL_CYBERMAN_STATIC_MS     500
#define SDL_CYBERMAN_RETRY_MS      1000
#define SDL_CYBERMAN_AXES          6 /* X, Y, Z, pitch, roll, yaw */
#define SDL_CYBERMAN_BUTTONS       3 /* L, M, R */
#define SDL_CYBERMAN_MAX_REPORT    12

typedef enum SDL_CyberManStep
{
    SDL_CYBERMAN_STEP_RTS_LOW,  /* RTS dropped, 200 ms */
    SDL_CYBERMAN_STEP_RTS_HIGH, /* RTS being raised */
    SDL_CYBERMAN_STEP_WAIT_M,
    SDL_CYBERMAN_STEP_WAIT_3,
    SDL_CYBERMAN_STEP_SWITCH,   /* *S written, then a drain */
    SDL_CYBERMAN_STEP_SETTLE,   /* 20 ms after the drain */
    SDL_CYBERMAN_STEP_RATE,     /* 4800 8N1 being set, then !S written */
    SDL_CYBERMAN_STEP_STATIC,   /* The static report */
    SDL_CYBERMAN_STEP_RETRY,
    SDL_CYBERMAN_STEP_PRESENT
} SDL_CyberManStep;

typedef struct SDL_CyberManState
{
    SDL_SerialBase base;
    SDL_CyberManStep step;
    bool deadline_set;
    uint64_t deadline;
    uint8_t action_seq;
    uint8_t waiting_seq;
    uint8_t report[SDL_CYBERMAN_MAX_REPORT];
    int report_length;
    int report_expected; /* 0 while no report is open */
    int version_major;
    int version_minor;
    bool power_connected;
    bool power_too_high;
} SDL_CyberManState;

extern const SDL_SerialModule SDL_SerialCyberManModule;

/* A 3D report: X and Y from -128 to 127, the 2-bit fields from -2 to 1, and
 * L, M, R in bits 0, 1, 2 */
extern bool SDL_CyberMan_Decode3D(const uint8_t *report, size_t length, int *axes, uint8_t *buttons);
/* The five bytes of "!T" for a rumble request */
extern void SDL_CyberMan_TactileCommand(uint16_t low_frequency_rumble, uint16_t high_frequency_rumble, uint8_t *command);

#endif /* SDL_serial_cyberman_proto_h_ */
