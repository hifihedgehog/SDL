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

/* Kettler ergometers with an RS-232 port, protocol token "kettler". 9600
 * baud 8N1. Commands and answers are ASCII lines ending in 0D 0A. The host
 * sends eight set-up commands 150 ms apart, then ST every 2000 ms, whose
 * answer is 8 fields separated by 09. The facts are from KettlerBLE (facts
 * only), kettlerUSB2BLE and Joni Kahara's protocol notes (facts only).
 */

#ifndef SDL_serial_kettler_proto_h_
#define SDL_serial_kettler_proto_h_

#include "SDL_serial_proto.h"

#define SDL_KETTLER_RATE         9600
#define SDL_KETTLER_COMMAND_MS   150
#define SDL_KETTLER_POLL_MS      2000
#define SDL_KETTLER_SILENCE_MS   5000
#define SDL_KETTLER_RECONNECT_MS 3000
#define SDL_KETTLER_LINE_LENGTH  127
#define SDL_KETTLER_COMMANDS     8
#define SDL_KETTLER_FIELDS       8
#define SDL_KETTLER_AXES         5 /* Cadence, power on the brake, speed, heart rate, target power */

typedef enum SDL_KettlerStep
{
    SDL_KETTLER_STEP_SETUP,   /* VE, ID, VE, KI, CA, RS, CM, SP1 */
    SDL_KETTLER_STEP_RUNNING, /* ST every 2000 ms */
    SDL_KETTLER_STEP_WAIT     /* 3000 ms after the silence, then the set-up again */
} SDL_KettlerStep;

typedef struct SDL_KettlerState
{
    SDL_SerialBase base;
    SDL_KettlerStep step;
    int command;       /* The next set-up command */
    uint64_t next_at;  /* The next command or poll */
    uint64_t last_line;
    char line[SDL_KETTLER_LINE_LENGTH + 1];
    int line_length;
    bool line_overflow;
    bool power_pending;
    uint16_t power;
} SDL_KettlerState;

extern const SDL_SerialModule SDL_SerialKettlerModule;

/* An ST answer without its line end: the 8 values, the time field as 0 */
extern bool SDL_Kettler_ParseStatus(const char *line, int *fields);

#endif /* SDL_serial_kettler_proto_h_ */
