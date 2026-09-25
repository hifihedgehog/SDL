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

#include "SDL_hidapi_rcadapter_proto.h"

#include <string.h>

static int16_t RCAdapter_Axis(uint8_t value)
{
    return (int16_t)((int)value * 257 - 32768);
}

void SDL_RCAdapter_Init(SDL_RCAdapterState *state)
{
    memset(state, 0, sizeof(*state));
}

bool SDL_RCAdapter_HandleReport(SDL_RCAdapterState *state, const uint8_t *report, size_t length)
{
    if (!state || !report || length != SDL_RCADAPTER_REPORT_LENGTH) {
        return false;
    }
    state->axes[0] = RCAdapter_Axis(report[0]);
    state->axes[1] = RCAdapter_Axis(report[2]);
    state->axes[2] = RCAdapter_Axis(report[3]);
    state->axes[3] = RCAdapter_Axis(report[4]);
    state->axes[4] = RCAdapter_Axis(report[5]);
    state->axes[5] = RCAdapter_Axis(report[6]);
    if (state->next_is_channel_7) {
        state->axes[6] = RCAdapter_Axis(report[7]);
    } else {
        state->axes[7] = RCAdapter_Axis(report[7]);
    }
    state->next_is_channel_7 = !state->next_is_channel_7;
    return true;
}
