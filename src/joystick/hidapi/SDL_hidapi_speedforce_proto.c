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

#include "SDL_hidapi_speedforce_proto.h"

#include <string.h>

#define SPEEDFORCE_WHEEL_REST 520
#define SPEEDFORCE_PEDAL_REST 255

static int16_t SpeedForce_Wheel(int raw)
{
    return (int16_t)(raw * 65535 / 1023 - 32768);
}

static int16_t SpeedForce_Byte(uint8_t value)
{
    return (int16_t)((int)value * 257 - 32768);
}

bool SDL_SpeedForce_DecodeReport(const uint8_t *report, size_t length, SDL_SpeedForceState *out)
{
    uint32_t bits;

    if (!report || !out || length != SDL_SPEEDFORCE_REPORT_LENGTH) {
        return false;
    }
    bits = (uint32_t)report[0] | ((uint32_t)report[1] << 8) | ((uint32_t)report[2] << 16);
    memset(out, 0, sizeof(*out));
    /* Bits 0-9 the wheel, 10-11 vendor, 12-22 buttons 1-11, 23 vendor */
    out->axes[0] = SpeedForce_Wheel((int)(bits & 0x3FF));
    out->buttons = (uint16_t)((bits >> 12) & 0x7FF);
    out->axes[1] = SpeedForce_Byte(report[3]);
    out->axes[2] = SpeedForce_Byte(report[4]);
    return true;
}

void SDL_SpeedForce_RestState(SDL_SpeedForceState *out)
{
    memset(out, 0, sizeof(*out));
    out->axes[0] = SpeedForce_Wheel(SPEEDFORCE_WHEEL_REST);
    out->axes[1] = SpeedForce_Byte(SPEEDFORCE_PEDAL_REST);
    out->axes[2] = SpeedForce_Byte(SPEEDFORCE_PEDAL_REST);
}

void SDL_SpeedForce_BuildConstantForce(int level, uint8_t out[SDL_SPEEDFORCE_OUTPUT_BUFFER])
{
    int value = level + 0x80;

    memset(out, 0, SDL_SPEEDFORCE_OUTPUT_BUFFER);
    if (level == 0) {
        out[1] = 0x13; /* stop */
        return;
    }
    if (value < 0x00) {
        value = 0x00;
    } else if (value > 0xFF) {
        value = 0xFF;
    }
    out[1] = 0x11;
    out[2] = 0x08;
    out[3] = (uint8_t)value;
    out[4] = 0x80;
}

void SDL_SpeedForce_BuildAutocenterOff(uint8_t out[SDL_SPEEDFORCE_OUTPUT_BUFFER])
{
    memset(out, 0, SDL_SPEEDFORCE_OUTPUT_BUFFER);
    out[1] = 0xF5;
}

static void SpeedForce_SendAutocenterOff(SDL_SpeedForceBond *bond, const SDL_SpeedForceSink *sink)
{
    uint8_t report[SDL_SPEEDFORCE_OUTPUT_BUFFER];

    SDL_SpeedForce_BuildAutocenterOff(report);
    sink->output(sink->userdata, report, sizeof(report));
    bond->done = true;
}

void SDL_SpeedForce_StartBond(SDL_SpeedForceBond *bond, uint64_t now_ms, const SDL_SpeedForceSink *sink)
{
    uint8_t report[SDL_SPEEDFORCE_FEATURE_BUFFER];

    memset(bond, 0, sizeof(*bond));
    memset(report, 0, sizeof(report));
    /* AF starts the link. Its byte 2 picks hopping sequence 1. */
    report[1] = 0xAF;
    report[2] = 0x01;
    if (sink->feature(sink->userdata, report, sizeof(report))) {
        bond->b2_pending = true;
        bond->b2_due_ms = now_ms + SDL_SPEEDFORCE_BOND_DELAY_MS;
    } else {
        SpeedForce_SendAutocenterOff(bond, sink);
    }
}

bool SDL_SpeedForce_UpdateBond(SDL_SpeedForceBond *bond, uint64_t now_ms, const SDL_SpeedForceSink *sink)
{
    uint8_t report[SDL_SPEEDFORCE_FEATURE_BUFFER];

    if (bond->done) {
        return true;
    }
    if (!bond->b2_pending || now_ms < bond->b2_due_ms) {
        return false;
    }
    /* B2 sets the address, two random bytes */
    memset(report, 0, sizeof(report));
    report[1] = 0xB2;
    report[2] = sink->random(sink->userdata);
    report[3] = sink->random(sink->userdata);
    bond->b2_pending = false;
    sink->feature(sink->userdata, report, sizeof(report));
    SpeedForce_SendAutocenterOff(bond, sink);
    return true;
}
