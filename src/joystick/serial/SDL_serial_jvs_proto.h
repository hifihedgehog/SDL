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

/* JVS arcade I/O boards on a USB-RS485 adapter, protocol token "jvs". 115200
 * baud 8N1 with RTS high while sending. The module is the bus master: it
 * resets the boards twice, assigns addresses until one goes unanswered,
 * identifies each board and then polls them in turn. Frames are E0, the
 * node, a length, the data and a checksum, with E0 and D0 escaped as D0 and
 * the value less one. The facts are from jvsio, MAME's jvs devices,
 * JoypadOS and Dolphin (GPL, facts only). No code from Dolphin is copied.
 */

#ifndef SDL_serial_jvs_proto_h_
#define SDL_serial_jvs_proto_h_

#include "SDL_serial_proto.h"

#define SDL_JVS_RATE          115200
#define SDL_JVS_SYNC          0xE0
#define SDL_JVS_MARK          0xD0
#define SDL_JVS_BROADCAST     0xFF
#define SDL_JVS_HOST          0x00
#define SDL_JVS_MAX_BOARDS    31
#define SDL_JVS_MAX_PLAYERS   4
#define SDL_JVS_RESET_MS      500  /* Between the two resets */
#define SDL_JVS_REPLY_MS      100
#define SDL_JVS_ERROR_MS      500  /* After an error, before the next reset */
#define SDL_JVS_SCAN_MS       1000 /* After a scan that found no board */
#define SDL_JVS_MAX_FRAME     257  /* Node, length and up to 255 bytes */
#define SDL_JVS_TEXT_LENGTH   100
#define SDL_JVS_MAX_OUTPUT    8
#define SDL_JVS_COIN_PULSES   16   /* At most per slot and poll */

/* Joystick buttons of every player */
#define SDL_JVS_BUTTON_START   10
#define SDL_JVS_BUTTON_SERVICE 11
#define SDL_JVS_BUTTON_COIN    12
#define SDL_JVS_BUTTON_TEST    13 /* Player 1 only */

typedef enum SDL_JVSStep
{
    SDL_JVS_STEP_RESET1,   /* First reset written, 500 ms */
    SDL_JVS_STEP_RESET2,   /* Second reset being written */
    SDL_JVS_STEP_ADDRESS,  /* Set address written, its answer */
    SDL_JVS_STEP_IDENTIFY, /* 10 to 14 to one board */
    SDL_JVS_STEP_POLL,
    SDL_JVS_STEP_ERROR,    /* 500 ms, then the resets */
    SDL_JVS_STEP_SCAN      /* No board: 1000 ms, then the resets */
} SDL_JVSStep;

typedef struct SDL_JVSBoard
{
    char ident[SDL_JVS_TEXT_LENGTH];
    uint8_t command_revision;
    uint8_t jvs_revision;
    uint8_t communication_revision;
    uint8_t players;  /* Feature 01 */
    uint8_t switches;
    uint8_t slots;    /* Feature 02 */
    uint8_t analog;   /* Feature 03 */
    uint8_t analog_bits;
    uint8_t rotary;   /* Feature 04 */
    uint8_t outputs;  /* Feature 12, general-purpose output slots */
    uint8_t first_player;   /* Joystick of its first player */
    uint8_t mapped_players; /* Its players that got a joystick */
    uint16_t coins[SDL_JVS_MAX_PLAYERS];
    bool coin_baseline[SDL_JVS_MAX_PLAYERS];
    bool output_pending;
    uint8_t output_length;
    uint8_t output[SDL_JVS_MAX_OUTPUT];
    bool output_sent; /* The current poll carries command 32 */
} SDL_JVSBoard;

typedef struct SDL_JVSState
{
    SDL_SerialBase base;
    SDL_JVSStep step;
    bool timer;
    uint64_t deadline;
    uint8_t action_seq;
    uint8_t waiting_seq; /* The request whose completion starts the reply timer */
    bool awaiting;       /* A request waits for its answer */
    int address;         /* The address being assigned */
    int boards;
    int board;           /* The board being identified or polled, from 0 */
    uint8_t command;     /* The identify command being asked */
    SDL_JVSBoard board_info[SDL_JVS_MAX_BOARDS];
    int players;         /* Joysticks presented */
    int poll_board_of_player1;

    bool rx_in;
    bool rx_escape;
    size_t rx_length;
    uint8_t rx[SDL_JVS_MAX_FRAME];
    int errors;
} SDL_JVSState;

extern const SDL_SerialModule SDL_SerialJVSModule;

/* A whole frame for a node, escaped. Returns its length, 0 when it does not fit. */
extern size_t SDL_JVS_BuildFrame(uint8_t node, const uint8_t *data, size_t length, uint8_t *out, size_t out_size);

#endif /* SDL_serial_jvs_proto_h_ */
