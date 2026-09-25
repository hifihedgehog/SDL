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

#include "SDL_hidapi_nia_proto.h"

#include <string.h>

bool SDL_NIA_IsManufacturer(const char *manufacturer)
{
    return manufacturer && strcmp(manufacturer, SDL_NIA_MANUFACTURER) == 0;
}

bool SDL_NIA_DecodeReport(const uint8_t *report, size_t length, SDL_NIAReport *out)
{
    int i;

    if (!report || !out || length != SDL_NIA_REPORT_LENGTH) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->raw_count = report[54];
    out->count = report[54];
    if (out->count > SDL_NIA_MAX_SAMPLES) {
        out->count = SDL_NIA_MAX_SAMPLES;
    }
    for (i = 0; i < out->count; ++i) {
        const uint8_t *sample = &report[3 * i];
        const int32_t value = (int32_t)sample[0] | ((int32_t)sample[1] << 8) | ((int32_t)sample[2] << 16);
        out->samples[i] = value - 0x800000;
    }
    out->counter = (uint16_t)(report[52] | (report[53] << 8));
    return true;
}

int16_t SDL_NIA_SampleAxis(int32_t offset)
{
    int32_t value = offset / 256;

    if (value < -32768) {
        value = -32768;
    } else if (value > 32767) {
        value = 32767;
    }
    return (int16_t)value;
}

void SDL_NIA_Init(SDL_NIAState *state)
{
    memset(state, 0, sizeof(*state));
}

bool SDL_NIA_HandleReport(SDL_NIAState *state, const uint8_t *report, size_t length, SDL_NIAReport *out)
{
    if (!state || !SDL_NIA_DecodeReport(report, length, out)) {
        return false;
    }
    state->gap = state->have_counter && out->counter != (uint16_t)(state->counter + out->raw_count);
    state->counter = out->counter;
    state->have_counter = true;
    if (out->count > 0) {
        state->axis = SDL_NIA_SampleAxis(out->samples[out->count - 1]);
    }
    return true;
}
