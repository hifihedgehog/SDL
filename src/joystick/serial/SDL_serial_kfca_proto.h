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

/* Konami's KFCA board, the SOUND VOLTEX I/O before the BIO2, protocol token
 * "kfca", hifihedgehog/SDL#33 Part 14. The board is a node on the ACIO bus
 * of SDL_konami_acio_proto.h, on an RS-232 port at 57600 baud, the rate
 * bemanitools says real boards expect. The facts are from bemanitools
 * (Unlicense) and arcade-docs (WTFPL).
 *
 * After the bus comes up, the module takes the node of type 09060000, the
 * highest if several are, as bemanitools' sdvxio-kfca takes the last KFCA.
 * It arms the node's watchdog with 0120 and the word 6000, which no source
 * gives a unit, sets the amplifier with 0128 and four bytes of 00, then
 * polls: 0113 with 24 bytes of 00, lamps off, answered with 16 bytes. Each
 * reply sends the next poll. The joystick appears with the first reply, and
 * three failed polls in a row start the bus over.
 *
 * The reply holds big-endian words. Bytes 0-1 and 2-3 carry knobs L and R
 * in bits 15-6, and byte 1 also the system inputs. Byte 9 holds BT-C, BT-B,
 * BT-A, START, recorder and headphone in bits 0 to 5, byte 11 FX-R, FX-L
 * and BT-D in bits 3 to 5. Inputs are active high.
 *
 * The joystick is the BIO2's SOUND VOLTEX joystick (SDL_serial_bio2_proto.h),
 * so both boards of the game map alike. The KFCA has no EX buttons, and
 * those two stay released.
 */

#ifndef SDL_serial_kfca_proto_h_
#define SDL_serial_kfca_proto_h_

#include "SDL_serial_bio2_proto.h"

#define SDL_KFCA_RATE          57600
#define SDL_KFCA_CMD_POLL      0x0113
#define SDL_KFCA_CMD_WATCHDOG  0x0120
#define SDL_KFCA_CMD_AMP       0x0128
#define SDL_KFCA_WATCHDOG      6000 /* A big-endian word */
#define SDL_KFCA_AMP_LENGTH    4
#define SDL_KFCA_OUTPUT        24   /* A 32-bit lamp word and 20 PWM bytes */
#define SDL_KFCA_INPUT         16
#define SDL_KFCA_FAILURES      3    /* Failed polls in a row that start the bus over */
#define SDL_KFCA_NAME          "Konami SOUND VOLTEX"

/* Byte 1, the system inputs. bemanitools' ACIO header has Test at 10 and
 * Service at 20, and only its test tool's printout reads them. Its SOUND
 * VOLTEX API and its configuration tool name bit 5 Test and bit 4 Service,
 * and its KFCA emulator hands those bits to the game as they are. The
 * module takes the API's order until a capture settles it. */
#define SDL_KFCA_SYS_COIN      0x04
#define SDL_KFCA_SYS_SERVICE   0x10
#define SDL_KFCA_SYS_TEST      0x20

typedef enum SDL_KFCAStep
{
    SDL_KFCA_STEP_BUS,      /* The bus is coming up */
    SDL_KFCA_STEP_IDLE,     /* No KFCA node */
    SDL_KFCA_STEP_WATCHDOG, /* 0120 sent */
    SDL_KFCA_STEP_AMP,      /* 0128 sent */
    SDL_KFCA_STEP_POLL
} SDL_KFCAStep;

typedef struct SDL_KFCAState
{
    SDL_ACIOState acio;
    SDL_KFCAStep step;
    uint8_t node; /* The KFCA node's address */
    int failures; /* Polls failed in a row */
} SDL_KFCAState;

extern const SDL_SerialModule SDL_SerialKFCAModule;

/* Controls from a 16-byte poll reply, on the BIO2's SOUND VOLTEX layout */
extern void SDL_KFCA_Decode(const uint8_t *payload, SDL_SerialControls *controls);

#endif /* SDL_serial_kfca_proto_h_ */
