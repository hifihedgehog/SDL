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

/* The Ergodex DX1. See SDL_hidapi_ergodex_proto.h. */

#include "SDL_hidapi_ergodex_proto.h"

#include <string.h>

/* Request types, host to pad */
#define ERGODEX_REQUEST_SET     0x02
#define ERGODEX_REQUEST_PROGRAM 0x03
#define ERGODEX_REQUEST_DEVICE  0x0A

/* Reply types, pad to host. Going out, 02 is SET and 03 is PROGRAM. */
#define ERGODEX_REPLY_STATUS 0x01
#define ERGODEX_REPLY_MACRO  0x02
#define ERGODEX_REPLY_TEST   0x03
#define ERGODEX_REPLY_DEVICE 0x0A

/* SET byte 2 is the mode: 00 normal, 01 test. Mode 06 stops the pad from
 * sensing any key, and nothing here sends it. */
#define ERGODEX_MODE_NORMAL 0x00
#define ERGODEX_MODE_TEST   0x01

/* PROGRAM carries five slots of key ID, type and value. Type 3 is a macro
 * key, whose value the pad does not use. */
#define ERGODEX_KEYS_PER_PROGRAM 5
#define ERGODEX_TYPE_MACRO       3

/* STATUS byte 4, active low: a bit is 1 while its button is up */
#define ERGODEX_TOP_UP   0x01
#define ERGODEX_LOWER_UP 0x02

/* MACRO and TEST list up to six held key IDs in bytes 3 to 8, 00 for none */
#define ERGODEX_FIRST_KEY_SLOT 3
#define ERGODEX_KEY_SLOTS      6

#define ERGODEX_KEY_BITS    ((1ULL << SDL_ERGODEX_KEYS) - 1)
#define ERGODEX_BUTTON_BITS ((1ULL << SDL_ERGODEX_BUTTON_TOP) | (1ULL << SDL_ERGODEX_BUTTON_LOWER))

/* A SET packet: byte 3 is the LEDs, byte 5 is 01 while the keys are
 * disabled, and bytes 6 and 7 are 01 in every packet known to work. */
static void Ergodex_Set(uint8_t *out, uint8_t mode, bool disabled)
{
    out[0] = ERGODEX_REQUEST_SET;
    out[2] = mode;
    out[3] = 0x01;
    out[5] = disabled ? 0x01 : 0x00;
    out[6] = 0x01;
    out[7] = 0x01;
}

void SDL_Ergodex_Init(SDL_ErgodexState *state, uint64_t now)
{
    if (state) {
        memset(state, 0, sizeof(*state));
        state->next_at = now;
    }
}

bool SDL_Ergodex_StartupOutput(int step, uint8_t *out)
{
    if (!out || step < 0 || step >= SDL_ERGODEX_STARTUP_OUTPUTS) {
        return false;
    }
    memset(out, 0, SDL_ERGODEX_PACKET_LENGTH);

    if (step == 0) {
        /* DEVICE, answered with the serial and firmware version */
        out[0] = ERGODEX_REQUEST_DEVICE;
    } else if (step == 1) {
        /* DISABLE before programming */
        Ergodex_Set(out, ERGODEX_MODE_NORMAL, true);
    } else if (step == SDL_ERGODEX_STARTUP_OUTPUTS - 1) {
        /* ENABLE in test mode */
        Ergodex_Set(out, ERGODEX_MODE_TEST, false);
    } else {
        /* Keys 1 to 50 as macro keys, five per packet in key order */
        const int first = (step - 2) * ERGODEX_KEYS_PER_PROGRAM + 1;
        int slot;

        out[0] = ERGODEX_REQUEST_PROGRAM;
        for (slot = 0; slot < ERGODEX_KEYS_PER_PROGRAM; ++slot) {
            out[1 + slot * 3] = (uint8_t)(first + slot);
            out[2 + slot * 3] = ERGODEX_TYPE_MACRO;
        }
    }
    return true;
}

bool SDL_Ergodex_NextOutput(SDL_ErgodexState *state, uint64_t now, uint8_t *out)
{
    if (!state || !out || state->in_flight || state->stopped || now < state->next_at) {
        return false;
    }
    if (!SDL_Ergodex_StartupOutput(state->step, out)) {
        return false; /* All went out */
    }
    state->in_flight = true;
    return true;
}

void SDL_Ergodex_OutputDone(SDL_ErgodexState *state, int written, uint64_t now)
{
    if (!state || !state->in_flight) {
        return;
    }
    state->in_flight = false;
    if (written == SDL_ERGODEX_PACKET_LENGTH) {
        ++state->step;
        state->retried = false;
        state->next_at = now + SDL_ERGODEX_PACE_NS;
    } else if (!state->retried) {
        state->retried = true;
        state->next_at = now + SDL_ERGODEX_RETRY_NS;
    } else {
        state->stopped = true;
    }
}

int SDL_Ergodex_HandlePacket(SDL_ErgodexState *state, const uint8_t *data, size_t length)
{
    if (!state || !data || length != SDL_ERGODEX_PACKET_LENGTH) {
        return SDL_ERGODEX_PACKET_IGNORED;
    }

    switch (data[0]) {
    case ERGODEX_REPLY_STATUS:
    {
        uint64_t buttons = state->buttons & ~ERGODEX_BUTTON_BITS;

        if (!(data[4] & ERGODEX_TOP_UP)) {
            buttons |= 1ULL << SDL_ERGODEX_BUTTON_TOP;
        }
        if (!(data[4] & ERGODEX_LOWER_UP)) {
            buttons |= 1ULL << SDL_ERGODEX_BUTTON_LOWER;
        }
        state->buttons = buttons;
        return SDL_ERGODEX_PACKET_STATUS;
    }

    case ERGODEX_REPLY_MACRO:
    case ERGODEX_REPLY_TEST:
    {
        uint64_t keys = 0;
        int slot;

        /* Each packet lists every key held at that moment, so a key it
           leaves out is released. 00 and IDs above 50 hold nothing. */
        for (slot = 0; slot < ERGODEX_KEY_SLOTS; ++slot) {
            const uint8_t id = data[ERGODEX_FIRST_KEY_SLOT + slot];

            if (id >= 1 && id <= SDL_ERGODEX_KEYS) {
                keys |= 1ULL << (id - 1);
            }
        }
        state->buttons = (state->buttons & ~ERGODEX_KEY_BITS) | keys;
        return SDL_ERGODEX_PACKET_KEYS;
    }

    case ERGODEX_REPLY_DEVICE:
    {
        uint64_t serial = 0;
        int i;

        /* Bytes 1 to 8, little endian, then the major and minor version */
        for (i = 8; i >= 1; --i) {
            serial = (serial << 8) | data[i];
        }
        state->serial = serial;
        state->version[0] = data[9];
        state->version[1] = data[10];
        state->have_device = true;
        return SDL_ERGODEX_PACKET_DEVICE;
    }

    default:
        return SDL_ERGODEX_PACKET_IGNORED;
    }
}

void SDL_Ergodex_FormatSerial(uint64_t serial, char *out)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    if (!out) {
        return;
    }
    for (i = 15; i >= 0; --i) {
        out[i] = digits[serial & 0x0F];
        serial >>= 4;
    }
    out[16] = '\0';
}
