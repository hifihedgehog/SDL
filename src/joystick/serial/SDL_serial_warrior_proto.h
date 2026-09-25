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

/* Logitech WingMan Warrior, protocol token "warrior". Logitech's SWIFT
 * serial protocol: at 1200 baud 7N2 the host writes '*' and 'S', each
 * echoed, then both sides run at 4800 baud 8N1. A byte with bit 7 set
 * starts a packet and bits 4-6 give its type and length. The facts are from
 * inputattach and Linux's warrior driver (GPL, facts only). No code from
 * them is copied.
 */

#ifndef SDL_serial_warrior_proto_h_
#define SDL_serial_warrior_proto_h_

#include "SDL_serial_proto.h"

#define SDL_WARRIOR_HANDSHAKE_RATE 1200
#define SDL_WARRIOR_RATE           4800
#define SDL_WARRIOR_DRAIN_MS       100
#define SDL_WARRIOR_ECHO_MS        1000
#define SDL_WARRIOR_RETRY_MS       1000
#define SDL_WARRIOR_MAX_LENGTH     12
#define SDL_WARRIOR_AXES           3 /* X, Y, throttle */
#define SDL_WARRIOR_BUTTONS        4 /* Trigger, thumb, top, top 2 */

typedef enum SDL_WarriorStep
{
    SDL_WARRIOR_STEP_DRAIN,   /* Until 100 ms of silence, and not before not_before */
    SDL_WARRIOR_STEP_STAR,    /* '*' written, its echo */
    SDL_WARRIOR_STEP_S,       /* 'S' written, its echo */
    SDL_WARRIOR_STEP_RATE,    /* 4800 8N1 being set */
    SDL_WARRIOR_STEP_PRESENT
} SDL_WarriorStep;

typedef struct SDL_WarriorState
{
    SDL_SerialBase base;
    SDL_WarriorStep step;
    uint64_t not_before;
    uint64_t last_byte;
    bool echo_timer;
    uint64_t echo_deadline;
    uint8_t action_seq;
    uint8_t waiting_seq;
    uint8_t packet[SDL_WARRIOR_MAX_LENGTH];
    int packet_length;
    int packet_expected; /* 0 while no packet is open */
} SDL_WarriorState;

extern const SDL_SerialModule SDL_SerialWarriorModule;

/* The length of a packet from its first byte, 0 for types that carry none */
extern int SDL_Warrior_PacketLength(uint8_t first);

#endif /* SDL_serial_warrior_proto_h_ */
