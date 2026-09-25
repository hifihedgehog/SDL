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

/* SpaceTec Spaceball serial models, protocol token "spaceball". 9600 baud
 * 8N1. The start-up follows inputattach: the power-up banner, or a reset
 * command when there is none, then the firmware line, the model query and
 * the setup commands. Packets run from a type byte to 0D, with '^' escaping
 * 0D, 11, 13 and '^' itself. The facts are from inputattach and the Linux
 * spaceball driver (GPL, facts only), spacenavd (GPL, facts only), VRPN and
 * libsball. No code from them is copied.
 */

#ifndef SDL_serial_spaceball_proto_h_
#define SDL_serial_spaceball_proto_h_

#include "SDL_serial_proto.h"

#define SDL_SPACEBALL_RATE          9600
#define SDL_SPACEBALL_LINE_LENGTH   128 /* Longer packets and lines are discarded */
#define SDL_SPACEBALL_BANNER_MS     4000
#define SDL_SPACEBALL_RESET_MS      2000 /* The reply window after @RESET */
#define SDL_SPACEBALL_REPLY_MS      1000 /* Each line of a reply */
#define SDL_SPACEBALL_REPLY_LINES   8
#define SDL_SPACEBALL_RETRY_MS      1000
#define SDL_SPACEBALL_AXES          6
#define SDL_SPACEBALL_BUTTONS       12

typedef enum SDL_SpaceballStep
{
    SDL_SPACEBALL_STEP_BANNER,    /* A line holding the power-up 11 byte */
    SDL_SPACEBALL_STEP_ALIVE,     /* The first @ line, "@1 Spaceball alive" */
    SDL_SPACEBALL_STEP_RESET,     /* @RESET written, a line holding "@1" */
    SDL_SPACEBALL_STEP_FIRMWARE,  /* The next @ line */
    SDL_SPACEBALL_STEP_MODEL,     /* hm written, an H line */
    SDL_SPACEBALL_STEP_FLX_NAME,  /* " written, a line starting "1 Spaceball 4000 FLX */
    SDL_SPACEBALL_STEP_FLX_HAND,  /* The next " line, " L " for a Lefty */
    SDL_SPACEBALL_STEP_FLX_THIRD, /* The third " line */
    SDL_SPACEBALL_STEP_FLX_YS,    /* YS written, a Y line */
    SDL_SPACEBALL_STEP_FLX_M,     /* M written, an M line */
    SDL_SPACEBALL_STEP_P,         /* P@A@A written, a P line */
    SDL_SPACEBALL_STEP_F,         /* FT@ written, an F line */
    SDL_SPACEBALL_STEP_M,         /* MSS written, an M line */
    SDL_SPACEBALL_STEP_RETRY,     /* 1000 ms, then @RESET */
    SDL_SPACEBALL_STEP_PRESENT
} SDL_SpaceballStep;

typedef enum SDL_SpaceballModel
{
    SDL_SPACEBALL_MODEL_UNKNOWN,
    SDL_SPACEBALL_MODEL_1003_2003, /* By firmware */
    SDL_SPACEBALL_MODEL_2003B,
    SDL_SPACEBALL_MODEL_2003C,
    SDL_SPACEBALL_MODEL_3003C,
    SDL_SPACEBALL_MODEL_3003,      /* By firmware: a 3003 or a 3003C */
    SDL_SPACEBALL_MODEL_4000FLX,
    SDL_SPACEBALL_MODEL_4000FLX_LEFTY
} SDL_SpaceballModel;

typedef struct SDL_SpaceballState
{
    SDL_SerialBase base;
    SDL_SpaceballStep step;
    bool deadline_set;
    uint64_t deadline;
    int lines;           /* Lines seen in the current wait */
    uint8_t write_seq;   /* Tag of the last command written */
    uint8_t waiting_seq; /* The command whose completion starts the reply timer, 0 when none */

    uint8_t line[SDL_SPACEBALL_LINE_LENGTH];
    size_t line_length;
    bool line_overflow;

    uint8_t packet[SDL_SPACEBALL_LINE_LENGTH];
    size_t packet_length;
    bool packet_overflow;
    bool escape;

    int firmware_major; /* -1 until a firmware line gives it */
    int firmware_minor;
    SDL_SpaceballModel model;
    bool seen_dot;     /* After a . packet, K packets are ignored */
    bool right_handed; /* Bit 5 of byte 1 of the last . packet */
    int errors;        /* E and ? packets */
} SDL_SpaceballState;

extern const SDL_SerialModule SDL_SerialSpaceballModule;

/* The model an hm reply names, else the one the firmware version names */
extern SDL_SpaceballModel SDL_Spaceball_Classify(const char *reply, int firmware_major, int firmware_minor);
extern const char *SDL_Spaceball_ModelName(SDL_SpaceballModel model);

#endif /* SDL_serial_spaceball_proto_h_ */
