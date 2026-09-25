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

/* Pony Canyon's Master Controller and Master Controller II, protocol token
 * "mastercontroller", hifihedgehog/SDL#33 Part 10. 19200 baud 8N1 with no
 * flow control, and DTR and RTS off, as the PC readers open the port. The
 * host writes one 00 byte after opening. Each event is five ASCII
 * characters and CR, decoded by SDL_train_proto.c. The controller
 * sends an event when a control moves, so the joystick appears with its
 * first event.
 */

#ifndef SDL_serial_mastercontroller_proto_h_
#define SDL_serial_mastercontroller_proto_h_

#include "SDL_serial_proto.h"
#include "../SDL_train_proto.h"

#define SDL_MASTERCONTROLLER_RATE 19200

typedef struct SDL_MasterControllerState
{
    SDL_SerialBase base;
    SDL_TrainLineParser parser;
    SDL_TrainState input;
} SDL_MasterControllerState;

extern const SDL_SerialModule SDL_SerialMasterControllerModule;

#endif /* SDL_serial_mastercontroller_proto_h_ */
