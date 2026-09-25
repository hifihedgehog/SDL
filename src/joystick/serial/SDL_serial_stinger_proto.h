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

/* Gravis Stinger, protocol token "stinger". 1200 baud 8N1. After a drain,
 * the host writes " E5E5" and the pad answers with 16 fixed bytes. Then it
 * sends 4-byte packets with no marker, counted from the end of the answer.
 * The facts are from inputattach and Linux's stinger driver (GPL, facts
 * only). No code from them is copied.
 */

#ifndef SDL_serial_stinger_proto_h_
#define SDL_serial_stinger_proto_h_

#include "SDL_serial_proto.h"

#define SDL_STINGER_RATE         1200
#define SDL_STINGER_DRAIN_MS     100 /* Silence that ends a drain */
#define SDL_STINGER_BYTE_MS      200 /* Each byte of the answer */
#define SDL_STINGER_RETRY_MS     1000
#define SDL_STINGER_REPLY_LENGTH 16
#define SDL_STINGER_AXES         2
#define SDL_STINGER_BUTTONS      10 /* A, B, C, X, Y, Z, TL, TR, Select, Start */

typedef enum SDL_StingerStep
{
    SDL_STINGER_STEP_DRAIN, /* Until 100 ms of silence, and not before not_before */
    SDL_STINGER_STEP_REPLY, /* " E5E5" written, the answer byte by byte */
    SDL_STINGER_STEP_PRESENT
} SDL_StingerStep;

typedef struct SDL_StingerState
{
    SDL_SerialBase base;
    SDL_StingerStep step;
    uint64_t not_before;
    uint64_t last_byte;
    bool reply_timer; /* The write finished and the answer is timed */
    uint64_t reply_deadline;
    uint8_t write_seq;
    uint8_t waiting_seq;
    int reply_index;
    uint8_t packet[4];
    int packet_length;
} SDL_StingerState;

extern const SDL_SerialModule SDL_SerialStingerModule;

/* A 4-byte packet: X from -64 to 63, Y from -63 to 64, and the buttons in
 * the order above */
extern void SDL_Stinger_DecodePacket(const uint8_t *packet, int *x, int *y, uint16_t *buttons);

#endif /* SDL_serial_stinger_proto_h_ */
