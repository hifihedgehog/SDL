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

/* Konami's BIO2 board, its BI2A node on the ACIO bus of
 * SDL_konami_acio_proto.h, hifihedgehog/SDL#33 Part 14. Windows binds its
 * stock serial driver to the board, 1CCF:804C or 1CCF:8040, which gives a
 * COM port and no game input. The board runs at 115200 baud. The facts are
 * from bemanitools (Unlicense).
 *
 * After the bus comes up, the module takes the node whose product code is
 * BI2A, the highest if several are, and sends 0100 with one byte: 2D for
 * beatmania IIDX, 3B for SOUND VOLTEX. SOUND VOLTEX also gets the amplifier
 * command 0128 with four bytes of 00. Then the module polls, and each reply
 * sends the next poll: 0152 with 48 bytes of 00, answered with 46 bytes, for
 * IIDX, and 0113 with 40 bytes of 00, answered with 16, for SOUND VOLTEX.
 * The node does not say which cabinet it serves, so the token decides:
 * bio2iidx, bio2sdvx, or bio2, which brings the bus up, logs the node and
 * opens no joystick. The joystick appears with the first poll's reply. Three
 * failed polls in a row start the bus over.
 *
 * The bit positions follow the declared order of bemanitools' bit-fields,
 * first field in bit 0, which is how its MinGW build lays them out. Inputs
 * are active high.
 */

#ifndef SDL_serial_bio2_proto_h_
#define SDL_serial_bio2_proto_h_

#include "SDL_konami_acio_proto.h"

#define SDL_BIO2_RATE          115200
#define SDL_BIO2_PRODUCT       "BI2A"
#define SDL_BIO2_CMD_INIT      0x0100
#define SDL_BIO2_CMD_AMP       0x0128
#define SDL_BIO2_CMD_POLL_IIDX 0x0152
#define SDL_BIO2_CMD_POLL_SDVX 0x0113
#define SDL_BIO2_INIT_IIDX     0x2D
#define SDL_BIO2_INIT_SDVX     0x3B
#define SDL_BIO2_AMP_LENGTH    4
#define SDL_BIO2_OUTPUT_IIDX   48
#define SDL_BIO2_OUTPUT_SDVX   40
#define SDL_BIO2_INPUT_IIDX    46
#define SDL_BIO2_INPUT_SDVX    16
#define SDL_BIO2_FAILURES      3 /* Failed polls in a row that start the bus over */

/* beatmania IIDX joystick: axes */
#define SDL_BIO2_IIDX_AXIS_TT1     0 /* (position - 128) x 256 */
#define SDL_BIO2_IIDX_AXIS_TT2     1
#define SDL_BIO2_IIDX_AXIS_SLIDER1 2 /* Sliders 1 to 5 are axes 2 to 6, value x 4369 - 32768 */
#define SDL_BIO2_IIDX_AXES         7
/* Buttons */
#define SDL_BIO2_IIDX_BUTTON_P1_KEY1 0 /* P1 keys 1 to 7 are buttons 0 to 6 */
#define SDL_BIO2_IIDX_BUTTON_P2_KEY1 7 /* P2 keys 1 to 7 are buttons 7 to 13 */
#define SDL_BIO2_IIDX_BUTTON_P1_START 14
#define SDL_BIO2_IIDX_BUTTON_P2_START 15
#define SDL_BIO2_IIDX_BUTTON_VEFX     16
#define SDL_BIO2_IIDX_BUTTON_EFFECT   17
#define SDL_BIO2_IIDX_BUTTON_TEST     18
#define SDL_BIO2_IIDX_BUTTON_SERVICE  19
#define SDL_BIO2_IIDX_BUTTON_COIN     20
#define SDL_BIO2_IIDX_BUTTONS         21

/* SOUND VOLTEX joystick: axes, knob position x 64 - 32768 */
#define SDL_BIO2_SDVX_AXIS_KNOB_L 0
#define SDL_BIO2_SDVX_AXIS_KNOB_R 1
#define SDL_BIO2_SDVX_AXES        2
/* Buttons */
#define SDL_BIO2_SDVX_BUTTON_BT_A      0
#define SDL_BIO2_SDVX_BUTTON_BT_B      1
#define SDL_BIO2_SDVX_BUTTON_BT_C      2
#define SDL_BIO2_SDVX_BUTTON_BT_D      3
#define SDL_BIO2_SDVX_BUTTON_FX_L      4
#define SDL_BIO2_SDVX_BUTTON_FX_R      5
#define SDL_BIO2_SDVX_BUTTON_START     6
#define SDL_BIO2_SDVX_BUTTON_EX1       7
#define SDL_BIO2_SDVX_BUTTON_EX2       8
#define SDL_BIO2_SDVX_BUTTON_HEADPHONE 9
#define SDL_BIO2_SDVX_BUTTON_RECORDER  10
#define SDL_BIO2_SDVX_BUTTON_TEST      11
#define SDL_BIO2_SDVX_BUTTON_SERVICE   12
#define SDL_BIO2_SDVX_BUTTON_COIN      13
#define SDL_BIO2_SDVX_BUTTONS          14

typedef enum SDL_BIO2Mode
{
    SDL_BIO2_MODE_NONE,
    SDL_BIO2_MODE_IIDX,
    SDL_BIO2_MODE_SDVX
} SDL_BIO2Mode;

typedef enum SDL_BIO2Step
{
    SDL_BIO2_STEP_BUS,  /* The bus is coming up */
    SDL_BIO2_STEP_IDLE, /* No BI2A node, or no mode */
    SDL_BIO2_STEP_INIT, /* 0100 sent */
    SDL_BIO2_STEP_AMP,  /* 0128 sent */
    SDL_BIO2_STEP_POLL
} SDL_BIO2Step;

typedef struct SDL_BIO2State
{
    SDL_ACIOState acio;
    SDL_BIO2Mode mode;
    SDL_BIO2Step step;
    uint8_t node;  /* The BI2A node's address */
    int failures;  /* Polls failed in a row */
} SDL_BIO2State;

extern const SDL_SerialModule SDL_SerialBIO2Module;     /* Token "bio2": no mode */
extern const SDL_SerialModule SDL_SerialBIO2IIDXModule; /* Token "bio2iidx" */
extern const SDL_SerialModule SDL_SerialBIO2SDVXModule; /* Token "bio2sdvx" */

/* Controls from a 46-byte IIDX poll reply. Bytes 0, 2, 4, 6 and 7 hold the
 * sliders in bits 7-4, byte 1 Coin, Service and Test in bits 1 to 3, byte 9
 * EFFECT, VEFX, P2 START and P1 START in bits 4 to 7, bytes 16 and 17 the
 * turntables, and bit 7 of the even bytes 18 to 30 and 32 to 44 the keys. */
extern void SDL_BIO2_DecodeIIDX(const uint8_t *payload, SDL_SerialControls *controls);

/* Controls from a 16-byte SOUND VOLTEX poll reply. Bytes 0-1 and 2-3 are
 * big-endian words holding the knobs in bits 15-6, and the first also Coin,
 * Service and Test in bits 1 to 3. Byte 9 holds FX-L, BT-D, BT-C, BT-B,
 * BT-A, START, recorder and headphone in bits 0 to 7, byte 10 EX2, EX1 and
 * FX-R in bits 5 to 7. */
extern void SDL_BIO2_DecodeSDVX(const uint8_t *payload, SDL_SerialControls *controls);

#endif /* SDL_serial_bio2_proto_h_ */
