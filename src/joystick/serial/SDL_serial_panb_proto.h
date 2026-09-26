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

/* Konami's PANB, the key panel of Nostalgia, protocol token "panb",
 * hifihedgehog/SDL#33 Part 14. The panel is four nodes on the ACIO bus of
 * SDL_konami_acio_proto.h, on an RS-232 port, and the first node handles
 * the commands. The facts are from bemanitools (Unlicense) and arcade-docs
 * (WTFPL).
 *
 * No source records the panel's rate. The module tries 57600, the KFCA's,
 * and after each bring-up that fails the bus switches between it and
 * 115200, the BIO2's and the MDXF's.
 *
 * After the bus comes up, the module sends the first node of type 090E0000
 * the start, 0115 with the node count 04, which nothing answers. The panel
 * then streams 0110 frames of 16 bytes without being asked, and stops only
 * when a reset stops it. A panel whose stream goes unread stops sending and
 * then ignores a reset too, until a power cycle, so the port is read
 * continuously while it is open, and the bus resets the nodes before the
 * port closes. bemanitools checks neither the address nor the sequence of a
 * streamed frame, and neither does the module.
 *
 * bemanitools' reader waits for the next frame without a limit. The module
 * allows SDL_PANB_GAP_MS, the silence after which a polled board starts
 * over, three reply times of the bus, since each streamed frame is the
 * panel's poll reply. A longer gap starts the bus over, which resets the
 * panel and sends the start again.
 *
 * Byte 0 of a frame is the sequence of the last command, byte 1 counts the
 * streamed frames, and bytes 2 to 15 hold keys 1 to 28 as nibbles: key
 * 2i+1 in the high nibble of byte 2+i and key 2i+2 in the low nibble. A key
 * is a value from 0 to 15, which bemanitools calls its velocity, and is
 * pressed when not 0. The joystick appears with the first frame.
 */

#ifndef SDL_serial_panb_proto_h_
#define SDL_serial_panb_proto_h_

#include "SDL_konami_acio_proto.h"

#define SDL_PANB_RATE           57600  /* Tried first */
#define SDL_PANB_ALTERNATE_RATE 115200
#define SDL_PANB_CMD_STREAM     0x0110
#define SDL_PANB_CMD_START      0x0115
#define SDL_PANB_NODES          4      /* The start's payload */
#define SDL_PANB_INPUT          16
#define SDL_PANB_KEYS           28     /* Keys 1 to 28 are buttons 0 to 27 */
#define SDL_PANB_GAP_MS         (3 * SDL_ACIO_REPLY_MS)
#define SDL_PANB_NAME           "Konami Nostalgia Panel"

typedef enum SDL_PANBStep
{
    SDL_PANB_STEP_BUS,   /* The bus is coming up */
    SDL_PANB_STEP_IDLE,  /* No PANB node */
    SDL_PANB_STEP_STREAM /* The start is out, frames are due */
} SDL_PANBStep;

typedef struct SDL_PANBState
{
    SDL_ACIOState acio;
    SDL_PANBStep step;
    uint8_t node;      /* The first PANB node's address */
    uint64_t deadline; /* The next frame is due, while streaming */
} SDL_PANBState;

extern const SDL_SerialModule SDL_SerialPANBModule;

/* Controls from a 16-byte streamed frame */
extern void SDL_PANB_Decode(const uint8_t *payload, SDL_SerialControls *controls);

#endif /* SDL_serial_panb_proto_h_ */
