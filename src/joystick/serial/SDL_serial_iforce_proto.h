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

/* I-Force wheels and joysticks on RS-232, hifihedgehog/SDL#33 Part 8.
 * 38400 baud 8N1. The query sequence of SDL_iforce_proto.h in serial frames,
 * each reply awaited for 1000 ms from the moment its query left. The M and
 * P replies pick the layout, and input decodes once the sequence is over.
 * A device that leaves O unanswered 20 times gets no joystick, as in Linux.
 * Effects are whole I-Force commands, sent in frames. */

#ifndef SDL_serial_iforce_proto_h_
#define SDL_serial_iforce_proto_h_

#include "SDL_serial_proto.h"
#include "../SDL_iforce_proto.h"

typedef struct SDL_IForceSerialState
{
    SDL_SerialBase base;
    SDL_IForceSerialParser parser;
    SDL_IForceQueries queries;
    uint8_t write_tag;  /* The last tag given out */
    uint8_t query_tag;  /* The query write whose reply is awaited */
    bool waiting;       /* A query is out */
    bool timer;         /* Its reply window runs */
    uint64_t deadline;
    const SDL_IForceModel *model; /* Once the sequence ended with O answered */
    SDL_IForceState input;
} SDL_IForceSerialState;

extern const SDL_SerialModule SDL_SerialIForceModule;

#endif /* SDL_serial_iforce_proto_h_ */
