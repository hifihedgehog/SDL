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

/* The CH Products Multi-Function Panel, 068E:00F0, hifihedgehog/SDL#33
 * Part 14. Pure C99: no SDL runtime and no I/O, so every decision here runs
 * in the offline tests exactly as it runs in the library.
 *
 * The panel's only interface is vendor class with one 8-byte interrupt IN
 * endpoint, and the host never writes to it. A report carries one bit per
 * key, the green and red buttons, and the mode the panel's LED shows. In red
 * mode the same keys count as a second bank of 50, so every report sets both
 * banks.
 *
 * The facts are restated from chmfp 0.6.0 (GPL-3.0), which reads the panel
 * through libusb on Linux. No code from it is copied.
 */

#ifndef SDL_hidapi_chmfp_proto_h_
#define SDL_hidapi_chmfp_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_CHMFP_REPORT_LENGTH 8
#define SDL_CHMFP_KEYS          50

/* The joystick: buttons 0 to 49 are keys 1 to 50 in green mode, 50 to 99
 * the same keys in red mode, then the green and red buttons. */
#define SDL_CHMFP_BUTTONS      102
#define SDL_CHMFP_BUTTON_GREEN 100
#define SDL_CHMFP_BUTTON_RED   101

/* chmfp drops a report that changes more than this many buttons at once,
 * the chord limit CH gives for the panel. */
#define SDL_CHMFP_MAX_CHANGES 6

typedef struct SDL_CHMFPState
{
    uint8_t buttons[(SDL_CHMFP_BUTTONS + 7) / 8]; /* Bit i % 8 of byte i / 8 is button i */
} SDL_CHMFPState;

typedef enum SDL_CHMFPResult
{
    SDL_CHMFP_REPORT_BAD_LENGTH, /* Not 8 bytes: dropped */
    SDL_CHMFP_REPORT_BAD_BITS,   /* A bit of byte 7 above bit 2 is set: dropped */
    SDL_CHMFP_REPORT_CHORD,      /* More than 6 buttons changed: dropped */
    SDL_CHMFP_REPORT_ACCEPTED
} SDL_CHMFPResult;

/* Nothing pressed, as before the first report of a connection */
extern void SDL_CHMFP_ResetState(SDL_CHMFPState *state);

/* Decodes one 8-byte report into both banks and the two buttons, ignoring
 * the chord limit. Returns false and leaves out unchanged when the length is
 * not 8 or a bit of byte 7 above bit 2 is set. Reads nothing past length. */
extern bool SDL_CHMFP_Decode(const uint8_t *report, size_t length, SDL_CHMFPState *out);

/* How many of the 102 buttons differ between two states. A press and a
 * release count alike, and a mode change counts each held key twice: once
 * released in one bank and once pressed in the other. */
extern int SDL_CHMFP_CountChanges(const SDL_CHMFPState *a, const SDL_CHMFPState *b);

/* Applies one read. The state changes only when the report is accepted, and
 * the chord limit is counted against the last accepted state. Returns an
 * SDL_CHMFPResult. */
extern int SDL_CHMFP_HandleReport(SDL_CHMFPState *state, const uint8_t *report, size_t length);

extern bool SDL_CHMFP_IsPressed(const SDL_CHMFPState *state, int button);

#endif /* SDL_hidapi_chmfp_proto_h_ */
