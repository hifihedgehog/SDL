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

/* The CH Products Multi-Function Panel. See SDL_hidapi_chmfp_proto.h. */

#include "SDL_hidapi_chmfp_proto.h"

#include <string.h>

/* Byte 7. Bits 0 and 1 are the green and red buttons and bit 2 is red mode.
 * chmfp drops a report with any higher bit set. */
#define CHMFP_GREEN_BUTTON 0x01
#define CHMFP_RED_BUTTON   0x02
#define CHMFP_RED_MODE     0x04
#define CHMFP_RESERVED     0xF8

static void CHMFP_Press(SDL_CHMFPState *state, int button)
{
    state->buttons[button / 8] = (uint8_t)(state->buttons[button / 8] | (1u << (button % 8)));
}

void SDL_CHMFP_ResetState(SDL_CHMFPState *state)
{
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

bool SDL_CHMFP_Decode(const uint8_t *report, size_t length, SDL_CHMFPState *out)
{
    SDL_CHMFPState decoded;
    int bank, key;

    if (!report || !out || length != SDL_CHMFP_REPORT_LENGTH) {
        return false;
    }
    if (report[7] & CHMFP_RESERVED) {
        return false;
    }
    memset(&decoded, 0, sizeof(decoded));

    /* Key k is bit (k - 1) % 8 of byte (k - 1) / 8. Bits 2 to 7 of byte 6
     * belong to no key. */
    bank = (report[7] & CHMFP_RED_MODE) ? SDL_CHMFP_KEYS : 0;
    for (key = 0; key < SDL_CHMFP_KEYS; ++key) {
        if (report[key / 8] & (1u << (key % 8))) {
            CHMFP_Press(&decoded, bank + key);
        }
    }
    if (report[7] & CHMFP_GREEN_BUTTON) {
        CHMFP_Press(&decoded, SDL_CHMFP_BUTTON_GREEN);
    }
    if (report[7] & CHMFP_RED_BUTTON) {
        CHMFP_Press(&decoded, SDL_CHMFP_BUTTON_RED);
    }
    *out = decoded;
    return true;
}

int SDL_CHMFP_CountChanges(const SDL_CHMFPState *a, const SDL_CHMFPState *b)
{
    int changes = 0;
    size_t i;

    if (!a || !b) {
        return 0;
    }
    for (i = 0; i < sizeof(a->buttons); ++i) {
        uint8_t differ = (uint8_t)(a->buttons[i] ^ b->buttons[i]);

        while (differ) {
            differ = (uint8_t)(differ & (differ - 1));
            ++changes;
        }
    }
    return changes;
}

int SDL_CHMFP_HandleReport(SDL_CHMFPState *state, const uint8_t *report, size_t length)
{
    SDL_CHMFPState decoded;

    if (!state || !report || length != SDL_CHMFP_REPORT_LENGTH) {
        return SDL_CHMFP_REPORT_BAD_LENGTH;
    }
    if (!SDL_CHMFP_Decode(report, length, &decoded)) {
        return SDL_CHMFP_REPORT_BAD_BITS;
    }
    /* chmfp counts every button whose state differs from the last report it
     * accepted, both banks and both buttons, presses and releases alike. */
    if (SDL_CHMFP_CountChanges(state, &decoded) > SDL_CHMFP_MAX_CHANGES) {
        return SDL_CHMFP_REPORT_CHORD;
    }
    *state = decoded;
    return SDL_CHMFP_REPORT_ACCEPTED;
}

bool SDL_CHMFP_IsPressed(const SDL_CHMFPState *state, int button)
{
    if (!state || button < 0 || button >= SDL_CHMFP_BUTTONS) {
        return false;
    }
    return (state->buttons[button / 8] & (1u << (button % 8))) != 0;
}
