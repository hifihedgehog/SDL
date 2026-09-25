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

/* Magellan and SpaceMouse serial models, protocol token "magellan". 9600
 * baud 8N2. The start-up is the X.org magellan driver's: wait for the reset
 * the open causes, mode off, zero twice, then sensitivity, period, null
 * radius and mode, each echoed, then the version. Lines end at 0D and carry
 * nibbles as characters of "0AB3D56GH9:K<MN?". The facts are from the X.org
 * driver, VRPN, Linux's magellan driver (GPL, facts only) and spacenavd
 * (GPL, facts only). No code from them is copied.
 */

#ifndef SDL_serial_magellan_proto_h_
#define SDL_serial_magellan_proto_h_

#include "SDL_serial_proto.h"

#define SDL_MAGELLAN_RATE        9600
#define SDL_MAGELLAN_LINE_LENGTH 64
#define SDL_MAGELLAN_OPEN_MS     1000 /* The device resets when the port opens */
#define SDL_MAGELLAN_SHORT_MS    200  /* After mode off and each zero */
#define SDL_MAGELLAN_ECHO_MS     1000
#define SDL_MAGELLAN_RETRY_MS    1000
#define SDL_MAGELLAN_AXES        6
#define SDL_MAGELLAN_BUTTONS     12

typedef enum SDL_MagellanStep
{
    SDL_MAGELLAN_STEP_OPEN,     /* 1000 ms after the open */
    SDL_MAGELLAN_STEP_MODE_OFF, /* \r\rm0 written, a line or 200 ms */
    SDL_MAGELLAN_STEP_ZERO1,    /* z written, a line or 200 ms */
    SDL_MAGELLAN_STEP_ZERO2,
    SDL_MAGELLAN_STEP_Q,        /* q00 written, its echo */
    SDL_MAGELLAN_STEP_P,        /* pAA */
    SDL_MAGELLAN_STEP_N,        /* nH */
    SDL_MAGELLAN_STEP_M,        /* m3 */
    SDL_MAGELLAN_STEP_V,        /* vQ written, a v line */
    SDL_MAGELLAN_STEP_RETRY,    /* 1000 ms, then step 2 */
    SDL_MAGELLAN_STEP_PRESENT
} SDL_MagellanStep;

typedef struct SDL_MagellanState
{
    SDL_SerialBase base;
    SDL_MagellanStep step;
    bool deadline_set;
    uint64_t deadline;
    uint8_t write_seq;
    uint8_t waiting_seq; /* The command whose completion starts the timer, 0 when none */
    uint8_t line[SDL_MAGELLAN_LINE_LENGTH];
    size_t line_length;
    bool line_overflow;
    int errors;
    int reinits; /* Start-ups repeated after mouse mode */
} SDL_MagellanState;

extern const SDL_SerialModule SDL_SerialMagellanModule;

/* A d line without its 0D: the type and 24 nibble characters. The values
 * are the nibbles, most significant first, minus 32768. */
extern bool SDL_Magellan_DecodeAxes(const uint8_t *line, size_t length, int16_t *axes);
/* A k line without its 0D: the type and 3 nibble characters */
extern bool SDL_Magellan_DecodeKeys(const uint8_t *line, size_t length, uint16_t *keys);
/* The name the version text gives */
extern const char *SDL_Magellan_NameFromVersion(const uint8_t *line, size_t length);

#endif /* SDL_serial_magellan_proto_h_ */
