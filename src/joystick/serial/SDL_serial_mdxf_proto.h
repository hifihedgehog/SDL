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

/* Konami's MDXF, the stage of DDR A cabinets, protocol token "mdxf",
 * hifihedgehog/SDL#33 Part 14. Each side of the stage is a node on the ACIO
 * bus of SDL_konami_acio_proto.h, on an RS-232 port at 115200 baud, the rate
 * p4io-mdxfdrv opens it at. The facts are from bemanitools (Unlicense) and
 * p4io-mdxfdrv (GPL-3.0, facts only). No code from p4io-mdxfdrv is copied.
 *
 * After the bus comes up, the module takes the nodes of type 09070000 in
 * address order, the first for player 1 and the second for player 2, as
 * p4io-mdxfdrv reads node 1 for player 1. It polls them in turn, 0110 with
 * no payload, answered with 3 bytes, and each reply sends the next poll.
 * The board also has an automatic mode that sends frames without pause,
 * which overran p4io-mdxfdrv's reader, so the module never starts it. Each
 * player's joystick appears with its node's first reply, and three failed
 * polls of one node in a row start the bus over.
 *
 * Byte 0 holds the four down sensors in its low nibble and the four up
 * sensors in its high nibble, byte 1 right and left the same way. Byte 2 is
 * not known. An arrow is pressed when any of its sensors is. The arrows are
 * buttons, so a jump can hold two opposite arrows at once, and the gamepad
 * mapping puts them on the D-pad.
 */

#ifndef SDL_serial_mdxf_proto_h_
#define SDL_serial_mdxf_proto_h_

#include "SDL_konami_acio_proto.h"

#define SDL_MDXF_RATE      115200
#define SDL_MDXF_CMD_POLL  0x0110
#define SDL_MDXF_INPUT     3
#define SDL_MDXF_PLAYERS   2
#define SDL_MDXF_FAILURES  3 /* Failed polls of one node in a row that start the bus over */

/* Buttons of each player's joystick */
#define SDL_MDXF_BUTTON_UP    0
#define SDL_MDXF_BUTTON_DOWN  1
#define SDL_MDXF_BUTTON_LEFT  2
#define SDL_MDXF_BUTTON_RIGHT 3
#define SDL_MDXF_BUTTONS      4

typedef enum SDL_MDXFStep
{
    SDL_MDXF_STEP_BUS,  /* The bus is coming up */
    SDL_MDXF_STEP_IDLE, /* No MDXF node */
    SDL_MDXF_STEP_POLL
} SDL_MDXFStep;

typedef struct SDL_MDXFState
{
    SDL_ACIOState acio;
    SDL_MDXFStep step;
    int players;                     /* MDXF nodes taken, one per player */
    uint8_t nodes[SDL_MDXF_PLAYERS]; /* Their addresses, player 1 first */
    int player;                      /* The player whose node is polled */
    int failures[SDL_MDXF_PLAYERS];  /* Polls failed in a row, per node */
} SDL_MDXFState;

extern const SDL_SerialModule SDL_SerialMDXFModule;

/* Controls from a 3-byte poll reply */
extern void SDL_MDXF_Decode(const uint8_t *payload, SDL_SerialControls *controls);

#endif /* SDL_serial_mdxf_proto_h_ */
