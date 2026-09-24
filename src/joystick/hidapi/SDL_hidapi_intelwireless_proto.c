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

#include "SDL_hidapi_intelwireless_proto.h"

#include <string.h>

/* Major types, the low three bits of byte 0. */
#define INTEL_WIRELESS_TYPE_INPUT       1
#define INTEL_WIRELESS_TYPE_INFORMATION 3
#define INTEL_WIRELESS_TYPE_ZERO_STATE  6

/* Minimum lengths. Input reaches byte 12. Information reaches the operation
 * in byte 2, and the device type in byte 3 for the operations that carry
 * one. Zero state reaches the slot in byte 1. */
#define INTEL_WIRELESS_INPUT_MINIMUM       13
#define INTEL_WIRELESS_INFORMATION_MINIMUM 3
#define INTEL_WIRELESS_TYPED_MINIMUM       4
#define INTEL_WIRELESS_ZERO_STATE_MINIMUM  2

/* Information operations. */
#define INTEL_WIRELESS_OP_ACTIVATE    0x01
#define INTEL_WIRELESS_OP_READY       0x04
#define INTEL_WIRELESS_OP_MOUSE_READY 0x05
#define INTEL_WIRELESS_OP_SEES        0x0A
#define INTEL_WIRELESS_OP_INFORM      0x0B
#define INTEL_WIRELESS_OP_REMEMBERS   0x0C

/* Device type in byte 3. */
#define INTEL_WIRELESS_DEVICE_GAMEPAD 2

/* The two digital axes: byte 9 is left and right, byte 10 is up and down. */
#define INTEL_WIRELESS_AXIS_NEGATIVE 0x81
#define INTEL_WIRELESS_AXIS_POSITIVE 0x7F

const uint16_t SDL_IntelWireless_ButtonOrder[SDL_INTEL_WIRELESS_JOYSTICK_BUTTONS] = {
    SDL_INTEL_WIRELESS_BUTTON_A,
    SDL_INTEL_WIRELESS_BUTTON_B,
    SDL_INTEL_WIRELESS_BUTTON_X,
    SDL_INTEL_WIRELESS_BUTTON_Y,
    SDL_INTEL_WIRELESS_BUTTON_MOUSE,
    SDL_INTEL_WIRELESS_BUTTON_SHIFT,
    SDL_INTEL_WIRELESS_BUTTON_START,
    SDL_INTEL_WIRELESS_BUTTON_Z,
    SDL_INTEL_WIRELESS_BUTTON_C,
    SDL_INTEL_WIRELESS_BUTTON_L,
    SDL_INTEL_WIRELESS_BUTTON_R,
};

void SDL_IntelWireless_Init(SDL_IntelWirelessState *state)
{
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

static void IntelWireless_Forget(SDL_IntelWirelessState *state, int slot, const SDL_IntelWirelessSink *sink)
{
    const bool connected = state->slots[slot].connected;

    memset(&state->slots[slot], 0, sizeof(state->slots[slot]));
    state->pending &= (uint8_t)~(1u << slot);
    if (connected) {
        sink->disconnect(sink->userdata, slot);
    }
}

static void IntelWireless_Connect(SDL_IntelWirelessState *state, int slot, const SDL_IntelWirelessSink *sink)
{
    if (!state->slots[slot].connected) {
        state->slots[slot].connected = true;
        sink->connect(sink->userdata, slot);
    }
}

static void IntelWireless_Publish(const SDL_IntelWirelessState *state, int slot, const SDL_IntelWirelessSink *sink)
{
    /* An unopened slot has no joystick to send to. */
    void *joystick = sink->joystick(sink->userdata, slot);

    if (joystick) {
        sink->controls(sink->userdata, joystick, state->slots[slot].buttons, state->slots[slot].hat);
    }
}

static uint8_t IntelWireless_Hat(uint8_t horizontal, uint8_t vertical)
{
    uint8_t hat = 0;

    if (horizontal == INTEL_WIRELESS_AXIS_NEGATIVE) {
        hat |= SDL_INTEL_WIRELESS_HAT_LEFT;
    } else if (horizontal == INTEL_WIRELESS_AXIS_POSITIVE) {
        hat |= SDL_INTEL_WIRELESS_HAT_RIGHT;
    }
    if (vertical == INTEL_WIRELESS_AXIS_NEGATIVE) {
        hat |= SDL_INTEL_WIRELESS_HAT_UP;
    } else if (vertical == INTEL_WIRELESS_AXIS_POSITIVE) {
        hat |= SDL_INTEL_WIRELESS_HAT_DOWN;
    }
    return hat;
}

void SDL_IntelWireless_HandlePacket(SDL_IntelWirelessState *state, const uint8_t *data, size_t length,
                                    const SDL_IntelWirelessSink *sink)
{
    int slot;

    if (!state || !sink || !data || length == 0 || length > SDL_INTEL_WIRELESS_READ_SIZE) {
        return;
    }

    switch (data[0] & 0x07) {
    case INTEL_WIRELESS_TYPE_INPUT:
        if (length < INTEL_WIRELESS_INPUT_MINIMUM) {
            return;
        }
        slot = data[4] & 0x07;
        if (!state->slots[slot].gamepad) {
            return; /* Input from a slot never reported as a gamepad creates nothing */
        }
        IntelWireless_Connect(state, slot, sink);
        state->slots[slot].buttons = (uint16_t)(data[11] | ((data[12] & 0x07) << 8));
        state->slots[slot].hat = IntelWireless_Hat(data[9], data[10]);
        IntelWireless_Publish(state, slot, sink);
        break;

    case INTEL_WIRELESS_TYPE_INFORMATION:
    {
        uint8_t operation;

        if (length < INTEL_WIRELESS_INFORMATION_MINIMUM) {
            return;
        }
        slot = data[1] & 0x07;
        operation = data[2];
        if (operation == INTEL_WIRELESS_OP_ACTIVATE || operation == INTEL_WIRELESS_OP_SEES ||
            operation == INTEL_WIRELESS_OP_INFORM || operation == INTEL_WIRELESS_OP_REMEMBERS) {
            bool gamepad;

            if (length < INTEL_WIRELESS_TYPED_MINIMUM) {
                return;
            }
            gamepad = (data[3] == INTEL_WIRELESS_DEVICE_GAMEPAD);
            /* A fresh activation starts a new session. Any of the four with
             * another device type replaces what the slot held. */
            if (operation == INTEL_WIRELESS_OP_ACTIVATE || state->slots[slot].gamepad != gamepad) {
                IntelWireless_Forget(state, slot, sink);
            }
            state->slots[slot].gamepad = gamepad;
            if (operation == INTEL_WIRELESS_OP_ACTIVATE && gamepad) {
                state->pending |= (uint8_t)(1u << slot);
            }
        } else if (operation == INTEL_WIRELESS_OP_READY) {
            state->slots[slot].gamepad = true;
            IntelWireless_Connect(state, slot, sink);
        } else if (operation == INTEL_WIRELESS_OP_MOUSE_READY) {
            IntelWireless_Forget(state, slot, sink);
        }
        break;
    }

    case INTEL_WIRELESS_TYPE_ZERO_STATE:
        if (length < INTEL_WIRELESS_ZERO_STATE_MINIMUM) {
            return;
        }
        slot = data[1] & 0x07;
        /* Release everything held, and keep the device type. */
        if (state->slots[slot].connected) {
            state->slots[slot].buttons = 0;
            state->slots[slot].hat = 0;
            IntelWireless_Publish(state, slot, sink);
        }
        break;

    default:
        break;
    }
}

void SDL_IntelWireless_Refresh(const SDL_IntelWirelessState *state, int slot,
                               const SDL_IntelWirelessSink *sink)
{
    if (!state || !sink || slot < 0 || slot >= SDL_INTEL_WIRELESS_SLOTS || !state->slots[slot].connected) {
        return;
    }
    IntelWireless_Publish(state, slot, sink);
}

bool SDL_IntelWireless_NextReply(SDL_IntelWirelessState *state, uint64_t now,
                                 uint8_t reply[SDL_INTEL_WIRELESS_REPLY_SIZE])
{
    int slot;

    if (!state || !reply || state->in_flight || !state->pending || now < state->retry_at) {
        return false;
    }
    for (slot = 0; !(state->pending & (1u << slot)); ++slot) {
    }
    reply[0] = (uint8_t)slot;
    reply[1] = 0x01;
    reply[2] = 0xFF;
    reply[3] = 0x00;
    reply[4] = 0x01;
    reply[5] = 0x63;
    state->pending &= (uint8_t)~(1u << slot);
    state->in_flight = true;
    state->in_flight_slot = (uint8_t)slot;
    return true;
}

void SDL_IntelWireless_ReplyDone(SDL_IntelWirelessState *state, int written, uint64_t now)
{
    if (!state || !state->in_flight) {
        return;
    }
    state->in_flight = false;
    if (written != SDL_INTEL_WIRELESS_REPLY_SIZE && state->slots[state->in_flight_slot].gamepad) {
        state->pending |= (uint8_t)(1u << state->in_flight_slot);
        state->retry_at = now + SDL_INTEL_WIRELESS_RETRY_NS;
    }
}

void SDL_IntelWireless_Detach(SDL_IntelWirelessState *state, const SDL_IntelWirelessSink *sink)
{
    int slot;

    if (!state || !sink) {
        return;
    }
    for (slot = 0; slot < SDL_INTEL_WIRELESS_SLOTS; ++slot) {
        if (state->slots[slot].connected) {
            state->slots[slot].connected = false;
            sink->disconnect(sink->userdata, slot);
        }
    }
    SDL_IntelWireless_Init(state);
}
