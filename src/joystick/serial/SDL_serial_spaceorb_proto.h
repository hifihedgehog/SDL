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

/* SpaceTec SpaceOrb 360 and SpaceBall Avenger, protocol token "spaceorb".
 * 9600 baud 8N1, nothing written. A byte with bit 7 clear starts a packet
 * and later bytes carry 7 bits each. The XOR of a packet's stored bytes is
 * 0. D packets carry six 10-bit values XORed with "SpaceWare" and the
 * buttons, K packets the buttons, E packets error flags and R packets the
 * greeting. The framing is Linux's spaceorb driver's (GPL, facts only), the
 * value packing the X.org spaceorb driver's. No code from them is copied.
 */

#ifndef SDL_serial_spaceorb_proto_h_
#define SDL_serial_spaceorb_proto_h_

#include "SDL_serial_proto.h"

#define SDL_SPACEORB_RATE       9600
#define SDL_SPACEORB_MAX_LENGTH 64
#define SDL_SPACEORB_DRAIN_MS   100 /* Silence that ends the drain after open */
#define SDL_SPACEORB_AXES       6
#define SDL_SPACEORB_BUTTONS    7
#define SDL_SPACEORB_D_LENGTH   12
#define SDL_SPACEORB_K_LENGTH   5
#define SDL_SPACEORB_E_LENGTH   4

typedef struct SDL_SpaceOrbState
{
    SDL_SerialBase base;
    bool draining;
    uint64_t drain_until;
    bool in_packet;
    bool finished; /* The packet reached its type's length */
    bool overflow;
    uint8_t packet[SDL_SPACEORB_MAX_LENGTH]; /* Bit 7 removed */
    size_t length;
    int errors;
    uint8_t last_error; /* Flags of the last E packet */
} SDL_SpaceOrbState;

extern const SDL_SerialModule SDL_SerialSpaceOrbModule;

/* A stored D packet, 12 bytes with bit 7 removed: the six values from -512
 * to 511 in the order X, Y, Z, RX, RY, RZ, and buttons 0 to 6 */
extern bool SDL_SpaceOrb_DecodeD(const uint8_t *stored, size_t length, int *axes, uint8_t *buttons);

#endif /* SDL_serial_spaceorb_proto_h_ */
