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

#include "SDL_hidapi_nimbus_proto.h"

#include <string.h>

#define NIMBUS_HAT_UP    0x01
#define NIMBUS_HAT_RIGHT 0x02
#define NIMBUS_HAT_DOWN  0x04
#define NIMBUS_HAT_LEFT  0x08

int16_t SDL_Nimbus_StickAxis(uint8_t value)
{
    const int v = (int)(int8_t)value;

    return (int16_t)((v >= 0) ? v * 32767 / 127 : v * 256);
}

static int16_t Nimbus_Negate(int16_t value)
{
    return (int16_t)((value == -32768) ? 32767 : -value);
}

bool SDL_Nimbus_DecodeReport(const uint8_t *report, size_t length, bool has_report_id, SDL_NimbusState *out)
{
    const uint8_t *p;
    int i;

    if (!report || !out || length != (size_t)SDL_NIMBUS_PAYLOAD_LENGTH + (has_report_id ? 1 : 0)) {
        return false;
    }
    /* p[n] is byte n + 1 of the report as MFIGamepadFeeder counts it */
    p = has_report_id ? report + 1 : report;
    memset(out, 0, sizeof(*out));

    /* Bytes 1-4: D-pad up, right, down, left, pressed above 0. Opposite directions cancel. */
    if (p[0] && !p[2]) {
        out->hat |= NIMBUS_HAT_UP;
    }
    if (p[2] && !p[0]) {
        out->hat |= NIMBUS_HAT_DOWN;
    }
    if (p[1] && !p[3]) {
        out->hat |= NIMBUS_HAT_RIGHT;
    }
    if (p[3] && !p[1]) {
        out->hat |= NIMBUS_HAT_LEFT;
    }
    /* Bytes 5-10: A, B, X, Y and the shoulders */
    for (i = 0; i < 6; ++i) {
        if (p[4 + i]) {
            out->buttons |= (uint8_t)(1 << i);
        }
    }
    /* Byte 13: Menu */
    if (p[12]) {
        out->buttons |= (uint8_t)(1 << 6);
    }
    /* Bytes 14-17: sticks, Y negated because SDL's Y runs down */
    out->axes[0] = SDL_Nimbus_StickAxis(p[13]);
    out->axes[1] = Nimbus_Negate(SDL_Nimbus_StickAxis(p[14]));
    out->axes[2] = SDL_Nimbus_StickAxis(p[15]);
    out->axes[3] = Nimbus_Negate(SDL_Nimbus_StickAxis(p[16]));
    /* Bytes 11-12: triggers */
    out->axes[4] = (int16_t)((int)p[10] * 257 - 32768);
    out->axes[5] = (int16_t)((int)p[11] * 257 - 32768);
    return true;
}
