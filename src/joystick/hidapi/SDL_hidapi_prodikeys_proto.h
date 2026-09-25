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

/* The Creative Prodikeys PC-MIDI, 041E:2801, interface 1. Pure C99: no SDL
 * runtime and no I/O.
 *
 * Output report 6, 01 C1, starts the music keys. Report 3 carries note and
 * velocity pairs, a note byte below 0x81 a press and 0x81 and above a
 * release. Report 4 carries a 24-bit mask of extra keys. Report 1 stays with
 * Windows. The facts are from Linux hid-prodikeys and the linux-input and
 * alsa-devel threads. No code is copied.
 */

#ifndef SDL_hidapi_prodikeys_proto_h_
#define SDL_hidapi_prodikeys_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_PRODIKEYS_INTERFACE     1
#define SDL_PRODIKEYS_NOTE_BUTTONS  128 /* MIDI notes 0-127 at octave 0 */
#define SDL_PRODIKEYS_MASK_BUTTONS  24  /* Report 4's mask bits 0-23 */
#define SDL_PRODIKEYS_BUTTONS       (SDL_PRODIKEYS_NOTE_BUTTONS + SDL_PRODIKEYS_MASK_BUTTONS)
#define SDL_PRODIKEYS_START_LENGTH  3

typedef struct SDL_ProdikeysState
{
    uint8_t buttons[(SDL_PRODIKEYS_BUTTONS + 7) / 8];
    int16_t velocity; /* Axis 0: the velocity of the latest press */
} SDL_ProdikeysState;

/* Every key up, the velocity axis at 0 velocity */
extern void SDL_Prodikeys_Init(SDL_ProdikeysState *state);

/* The output report that starts the music keys: 06 01 C1 */
extern void SDL_Prodikeys_BuildStart(uint8_t out[SDL_PRODIKEYS_START_LENGTH]);

/* Whether a collection's input reports, one bit per report ID, include the
 * key reports 3 or 4. The driver opens only that collection of interface 1. */
extern bool SDL_Prodikeys_CarriesKeys(const uint8_t input_report_ids[32]);

/* Applies one report. Returns true for report 3 or a complete report 4. */
extern bool SDL_Prodikeys_HandleReport(SDL_ProdikeysState *state, const uint8_t *report, size_t length);

extern bool SDL_Prodikeys_IsButtonDown(const SDL_ProdikeysState *state, int button);

#endif /* SDL_hidapi_prodikeys_proto_h_ */
