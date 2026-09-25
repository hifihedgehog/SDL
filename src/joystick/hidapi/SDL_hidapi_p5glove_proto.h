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

/* The P5 Glove, 0D7F:0100. Pure C99: no SDL runtime and no I/O.
 *
 * Input report 1 is 24 bytes with the ID, bits counted from bit 7 of byte 0,
 * most significant first: five 6-bit fingers, four buttons and four
 * tracking slots of an LED index and three signed 10-bit values. The
 * feature parsers are for a later position step, which the driver does not
 * run. The facts are from libp5glove, the maker's USB Packet Format
 * document and the Scratchpad wiki. No code from them is copied.
 */

#ifndef SDL_hidapi_p5glove_proto_h_
#define SDL_hidapi_p5glove_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_P5GLOVE_REPORT_ID     0x01
#define SDL_P5GLOVE_REPORT_LENGTH 24
#define SDL_P5GLOVE_FINGERS       5 /* Index, middle, ring, pinky, thumb */
#define SDL_P5GLOVE_BUTTONS       4 /* A, B, C, then the unnamed bit */
#define SDL_P5GLOVE_SLOTS         4
#define SDL_P5GLOVE_LEDS          10
#define SDL_P5GLOVE_EMPTY_SLOT    0xFF

typedef struct SDL_P5GloveSlot
{
    uint8_t led;       /* 0-7, or SDL_P5GLOVE_EMPTY_SLOT */
    int16_t values[3]; /* Signed 10-bit, in report order */
} SDL_P5GloveSlot;

typedef struct SDL_P5GloveState
{
    uint8_t fingers[SDL_P5GLOVE_FINGERS]; /* 0-63 */
    uint8_t buttons;                      /* Bit 0 A, 1 B, 2 C, 3 the fourth */
    SDL_P5GloveSlot slots[SDL_P5GLOVE_SLOTS];
} SDL_P5GloveState;

/* Input report 1, exactly 24 bytes with the ID. Other IDs, reports 3 and 8
 * among them, are not read. */
extern bool SDL_P5Glove_DecodeReport(const uint8_t *report, size_t length, SDL_P5GloveState *out);

/* A finger, 0-63, as an axis: 0 is -32768 and 63 is 32767 */
extern int16_t SDL_P5Glove_FingerAxis(uint8_t finger);

typedef struct SDL_P5GloveLEDPositions
{
    uint8_t glove_type;
    int16_t positions[SDL_P5GLOVE_LEDS][3]; /* Hundredths of an inch, the first axis negated */
} SDL_P5GloveLEDPositions;

/* Feature 12: the glove type and the LED positions. Needs 62 bytes. */
extern bool SDL_P5Glove_ParseLEDPositions(const uint8_t *reply, size_t length, SDL_P5GloveLEDPositions *out);

typedef struct SDL_P5GloveTanCompensation
{
    int8_t angles[4];   /* 1/32 degree: head 1 vertical and horizontal, head 2 vertical and horizontal */
    uint8_t separation; /* Tenths of an inch */
} SDL_P5GloveTanCompensation;

/* Feature 6. Needs 6 bytes. */
extern bool SDL_P5Glove_ParseTanCompensation(const uint8_t *reply, size_t length, SDL_P5GloveTanCompensation *out);

/* Feature 5: 05 01 is mouse mode on and 05 FF off */
extern bool SDL_P5Glove_ParseMouseMode(const uint8_t *reply, size_t length, bool *mouse_on);

#endif /* SDL_hidapi_p5glove_proto_h_ */
