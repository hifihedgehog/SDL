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

#include "SDL_hidapi_p5glove_proto.h"

#include <string.h>

/* count bits starting at bit position first, most significant bit first */
static uint32_t P5Glove_Bits(const uint8_t *data, unsigned first, unsigned count)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < count; ++i) {
        const unsigned bit = first + i;
        value = (value << 1) | (uint32_t)((data[bit >> 3] >> (7 - (bit & 7))) & 1);
    }
    return value;
}

static int16_t P5Glove_Signed10(uint32_t value)
{
    return (int16_t)((value & 0x200) ? (int)value - 0x400 : (int)value);
}

bool SDL_P5Glove_DecodeReport(const uint8_t *report, size_t length, SDL_P5GloveState *out)
{
    static const unsigned finger_bits[SDL_P5GLOVE_FINGERS] = { 8, 14, 20, 26, 32 };
    int i, j;

    if (!report || !out || length != SDL_P5GLOVE_REPORT_LENGTH || report[0] != SDL_P5GLOVE_REPORT_ID) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    for (i = 0; i < SDL_P5GLOVE_FINGERS; ++i) {
        out->fingers[i] = (uint8_t)P5Glove_Bits(report, finger_bits[i], 6);
    }
    out->buttons = (uint8_t)P5Glove_Bits(report, 40, 4);
    for (i = 0; i < SDL_P5GLOVE_SLOTS; ++i) {
        const uint32_t led = P5Glove_Bits(report, 44 + 4 * (unsigned)i, 4);
        out->slots[i].led = (led & 0x8) ? SDL_P5GLOVE_EMPTY_SLOT : (uint8_t)led;
        for (j = 0; j < 3; ++j) {
            out->slots[i].values[j] = P5Glove_Signed10(P5Glove_Bits(report, 60 + 30 * (unsigned)i + 10 * (unsigned)j, 10));
        }
    }
    return true;
}

int16_t SDL_P5Glove_FingerAxis(uint8_t finger)
{
    if (finger > 63) {
        finger = 63;
    }
    return (int16_t)((int)finger * 65535 / 63 - 32768);
}

bool SDL_P5Glove_ParseLEDPositions(const uint8_t *reply, size_t length, SDL_P5GloveLEDPositions *out)
{
    int led, axis;

    if (!reply || !out || length < 2 + 6 * SDL_P5GLOVE_LEDS || reply[0] != 0x0C) {
        return false;
    }
    out->glove_type = reply[1];
    for (led = 0; led < SDL_P5GLOVE_LEDS; ++led) {
        for (axis = 0; axis < 3; ++axis) {
            const uint8_t *p = &reply[2 + 6 * led + 2 * axis];
            int value = (int16_t)(uint16_t)((p[0] << 8) | p[1]);
            if (axis == 0) {
                value = -value;
            }
            out->positions[led][axis] = (int16_t)value;
        }
    }
    return true;
}

bool SDL_P5Glove_ParseTanCompensation(const uint8_t *reply, size_t length, SDL_P5GloveTanCompensation *out)
{
    int i;

    if (!reply || !out || length < 6 || reply[0] != 0x06) {
        return false;
    }
    for (i = 0; i < 4; ++i) {
        out->angles[i] = (int8_t)reply[1 + i];
    }
    out->separation = reply[5];
    return true;
}

bool SDL_P5Glove_ParseMouseMode(const uint8_t *reply, size_t length, bool *mouse_on)
{
    if (!reply || !mouse_on || length < 2 || reply[0] != 0x05) {
        return false;
    }
    *mouse_on = (reply[1] == 0x01);
    return true;
}
