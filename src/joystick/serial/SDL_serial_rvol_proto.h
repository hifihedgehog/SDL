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

/* Konami's RVOL, the KFCA variant of MUSECA, protocol token "rvol",
 * hifihedgehog/SDL#33 Part 14. The board is a node on the ACIO bus of
 * SDL_konami_acio_proto.h, on an RS-232 port. Its product code reads KFCA
 * and its type 09060001. The facts are from bemanitools (Unlicense) and
 * arcade-docs (WTFPL).
 *
 * No source records the board's rate. The module tries 57600, the KFCA's,
 * and after each bring-up that fails the bus switches between it and
 * 115200, the BIO2's and the MDXF's.
 *
 * After the bus comes up, the module takes the node of type 09060001, the
 * highest if several are. It sets the expand mode with 0114 and C0, which is
 * bemanitools' mode 1, then polls: 0112 with 32 bytes of 00, lamps off,
 * answered with 23 bytes. Each reply sends the next poll. The joystick
 * appears with the first reply, and three failed polls in a row start the
 * bus over.
 *
 * Bytes 16 to 20 are spinners 1 to 5, one byte each. Byte 9 bit 3 is
 * spinner 1's press and bit 0 spinner 2's, byte 11 bit 3 spinner 3's, and
 * byte 1 bits 2 and 4 spinners 4 and 5. The presses are active high. Byte 1
 * bit 3 is the pedal, active low.
 */

#ifndef SDL_serial_rvol_proto_h_
#define SDL_serial_rvol_proto_h_

#include "SDL_konami_acio_proto.h"

#define SDL_RVOL_RATE           57600  /* Tried first */
#define SDL_RVOL_ALTERNATE_RATE 115200
#define SDL_RVOL_CMD_POLL       0x0112
#define SDL_RVOL_CMD_EXPAND     0x0114
#define SDL_RVOL_EXPAND         0xC0   /* Mode 1, as (1 | 2) << 6 */
#define SDL_RVOL_OUTPUT         32
#define SDL_RVOL_INPUT          23
#define SDL_RVOL_FAILURES       3      /* Failed polls in a row that start the bus over */
#define SDL_RVOL_NAME           "Konami MUSECA"

/* Axes: spinners 1 to 5 are axes 0 to 4, position x 256 - 32768 */
#define SDL_RVOL_AXIS_SPINNER1  0
#define SDL_RVOL_AXES           5
/* Buttons: spinner 1 to 5 presses are buttons 0 to 4 */
#define SDL_RVOL_BUTTON_SPINNER1 0
#define SDL_RVOL_BUTTON_PEDAL    5
#define SDL_RVOL_BUTTONS         6

typedef enum SDL_RVOLStep
{
    SDL_RVOL_STEP_BUS,    /* The bus is coming up */
    SDL_RVOL_STEP_IDLE,   /* No RVOL node */
    SDL_RVOL_STEP_EXPAND, /* 0114 sent */
    SDL_RVOL_STEP_POLL
} SDL_RVOLStep;

typedef struct SDL_RVOLState
{
    SDL_ACIOState acio;
    SDL_RVOLStep step;
    uint8_t node; /* The RVOL node's address */
    int failures; /* Polls failed in a row */
} SDL_RVOLState;

extern const SDL_SerialModule SDL_SerialRVOLModule;

/* Controls from a 23-byte poll reply */
extern void SDL_RVOL_Decode(const uint8_t *payload, SDL_SerialControls *controls);

#endif /* SDL_serial_rvol_proto_h_ */
