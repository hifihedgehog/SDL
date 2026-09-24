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

#include "SDL_hidapi_xbox360bb_proto.h"

#include <string.h>

void SDL_Xbox360BB_Init(SDL_Xbox360BBState *state)
{
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

void SDL_Xbox360BB_Attach(SDL_Xbox360BBState *state, const SDL_Xbox360BBSink *sink)
{
    int pad;

    if (!state || !sink) {
        return;
    }
    SDL_Xbox360BB_Init(state);
    for (pad = 0; pad < SDL_XBOX360BB_PADS; ++pad) {
        sink->connect(sink->userdata, pad);
    }
}

static void Xbox360BB_Publish(const SDL_Xbox360BBState *state, int pad, const SDL_Xbox360BBSink *sink)
{
    /* A pad the application has not opened has no joystick to send to. */
    void *joystick = sink->joystick(sink->userdata, pad);

    if (joystick) {
        sink->controls(sink->userdata, joystick, state->pads[pad].buttons, state->pads[pad].hat);
    }
}

int SDL_Xbox360BB_HandlePacket(SDL_Xbox360BBState *state, const uint8_t *data, size_t length,
                               uint64_t now, const SDL_Xbox360BBSink *sink)
{
    SDL_Xbox360BBPad *pad;
    uint8_t buttons = 0;
    uint8_t hat = 0;
    int index;

    if (!state || !sink || !data || length != SDL_XBOX360BB_PACKET_SIZE) {
        return -1;
    }
    /* The pad index is the device's own byte. Anything past the fourth pad
     * would index past the arrays. */
    index = data[2];
    if (index >= SDL_XBOX360BB_PADS) {
        return -1;
    }
    pad = &state->pads[index];

    /* Byte 3: D-pad up, down, left, right, then Start and Back. */
    if (data[3] & 0x01) {
        hat |= SDL_XBOX360BB_HAT_UP;
    }
    if (data[3] & 0x02) {
        hat |= SDL_XBOX360BB_HAT_DOWN;
    }
    if (data[3] & 0x04) {
        hat |= SDL_XBOX360BB_HAT_LEFT;
    }
    if (data[3] & 0x08) {
        hat |= SDL_XBOX360BB_HAT_RIGHT;
    }
    if (data[3] & 0x10) {
        buttons |= SDL_XBOX360BB_BUTTON_START;
    }
    if (data[3] & 0x20) {
        buttons |= SDL_XBOX360BB_BUTTON_BACK;
    }
    /* Byte 4: Guide, the Big Button, then A, B, X and Y. */
    if (data[4] & 0x04) {
        buttons |= SDL_XBOX360BB_BUTTON_GUIDE;
    }
    if (data[4] & 0x08) {
        buttons |= SDL_XBOX360BB_BUTTON_BIG;
    }
    if (data[4] & 0x10) {
        buttons |= SDL_XBOX360BB_BUTTON_A;
    }
    if (data[4] & 0x20) {
        buttons |= SDL_XBOX360BB_BUTTON_B;
    }
    if (data[4] & 0x40) {
        buttons |= SDL_XBOX360BB_BUTTON_X;
    }
    if (data[4] & 0x80) {
        buttons |= SDL_XBOX360BB_BUTTON_Y;
    }

    pad->buttons = buttons;
    pad->hat = hat;
    pad->last_packet = now;
    Xbox360BB_Publish(state, index, sink);
    return index;
}

uint8_t SDL_Xbox360BB_Expire(SDL_Xbox360BBState *state, uint64_t now, const SDL_Xbox360BBSink *sink)
{
    uint8_t released = 0;
    int index;

    if (!state || !sink) {
        return 0;
    }
    for (index = 0; index < SDL_XBOX360BB_PADS; ++index) {
        SDL_Xbox360BBPad *pad = &state->pads[index];

        if (!pad->buttons && !pad->hat) {
            continue;
        }
        if (now >= pad->last_packet && now - pad->last_packet >= SDL_XBOX360BB_RELEASE_NS) {
            pad->buttons = 0;
            pad->hat = 0;
            released |= (uint8_t)(1u << index);
            Xbox360BB_Publish(state, index, sink);
        }
    }
    return released;
}
