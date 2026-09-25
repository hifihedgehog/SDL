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

#include "SDL_hidapi_prodikeys_proto.h"

#include <string.h>

#define PRODIKEYS_REPORT_NOTES 0x03
#define PRODIKEYS_REPORT_MASK  0x04

static void Prodikeys_SetButton(SDL_ProdikeysState *state, int button, bool down)
{
    if (down) {
        state->buttons[button >> 3] |= (uint8_t)(1 << (button & 7));
    } else {
        state->buttons[button >> 3] &= (uint8_t)~(1 << (button & 7));
    }
}

void SDL_Prodikeys_Init(SDL_ProdikeysState *state)
{
    memset(state, 0, sizeof(*state));
    state->velocity = -32768;
}

void SDL_Prodikeys_BuildStart(uint8_t out[SDL_PRODIKEYS_START_LENGTH])
{
    /* Output report 6: its two fields 0x01 and the state 0xC1, 8 bits each */
    out[0] = 0x06;
    out[1] = 0x01;
    out[2] = 0xC1;
}

bool SDL_Prodikeys_CarriesKeys(const uint8_t input_report_ids[32])
{
    return input_report_ids &&
           ((input_report_ids[0] & (1 << PRODIKEYS_REPORT_NOTES)) || (input_report_ids[0] & (1 << PRODIKEYS_REPORT_MASK)));
}

bool SDL_Prodikeys_HandleReport(SDL_ProdikeysState *state, const uint8_t *report, size_t length)
{
    size_t i;

    if (!state || !report || length == 0) {
        return false;
    }
    if (report[0] == PRODIKEYS_REPORT_NOTES) {
        /* Note and velocity pairs. Middle C at octave 0 is 0x54 pressed and 0x94 released. */
        for (i = 1; i + 1 < length; i += 2) {
            const uint8_t note = report[i];
            const uint8_t velocity = report[i + 1];
            if (note < 0x81) {
                const int midi = note - 0x54 + 60;
                if (midi >= 0 && midi < SDL_PRODIKEYS_NOTE_BUTTONS) {
                    Prodikeys_SetButton(state, midi, true);
                    state->velocity = (int16_t)((int)(velocity ? velocity : 1) * 257 - 32768);
                }
            } else {
                const int midi = note - 0x94 + 60;
                if (midi >= 0 && midi < SDL_PRODIKEYS_NOTE_BUTTONS) {
                    Prodikeys_SetButton(state, midi, false);
                }
            }
        }
        return true;
    }
    if (report[0] == PRODIKEYS_REPORT_MASK) {
        /* A 24-bit mask, byte 1 most significant. Windows pads a shorter report
           to the collection's longest, so bytes past 3 are not checked. */
        uint32_t mask;
        int bit;

        if (length < 4) {
            return false;
        }
        mask = ((uint32_t)report[1] << 16) | ((uint32_t)report[2] << 8) | report[3];
        for (bit = 0; bit < SDL_PRODIKEYS_MASK_BUTTONS; ++bit) {
            Prodikeys_SetButton(state, SDL_PRODIKEYS_NOTE_BUTTONS + bit, (mask >> bit) & 1);
        }
        return true;
    }
    /* Report 1 and anything else stay out of the joystick */
    return false;
}

bool SDL_Prodikeys_IsButtonDown(const SDL_ProdikeysState *state, int button)
{
    if (!state || button < 0 || button >= SDL_PRODIKEYS_BUTTONS) {
        return false;
    }
    return (state->buttons[button >> 3] >> (button & 7)) & 1;
}
