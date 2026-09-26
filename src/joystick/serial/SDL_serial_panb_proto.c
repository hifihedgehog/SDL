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

#include "SDL_serial_panb_proto.h"

#include <string.h>

void SDL_PANB_Decode(const uint8_t *payload, SDL_SerialControls *controls)
{
    int i;

    memset(controls, 0, sizeof(*controls));
    for (i = 0; i < SDL_PANB_KEYS / 2; ++i) {
        SDL_Serial_SetButton(controls, 2 * i, (payload[2 + i] & 0xF0) != 0);
        SDL_Serial_SetButton(controls, 2 * i + 1, (payload[2 + i] & 0x0F) != 0);
    }
}

/* The first frame presents the joystick with its controls */
static void PANB_Commit(SDL_PANBState *s, const uint8_t *payload)
{
    SDL_SerialControls controls;
    SDL_SerialIdentity identity;

    SDL_PANB_Decode(payload, &controls);
    if (SDL_Serial_IsPresent(&s->acio.base, 0)) {
        SDL_Serial_Commit(&s->acio.base, 0, &controls);
        return;
    }
    SDL_Serial_SetIdentity(&identity, SDL_PANB_NAME, SDL_SERIAL_TYPE_UNKNOWN, 0, SDL_PANB_KEYS, 0, 0);
    SDL_Serial_PresentWith(&s->acio.base, 0, &identity, &controls);
}

static void PANB_Ready(void *state, uint64_t now)
{
    static const uint8_t nodes = SDL_PANB_NODES;
    SDL_PANBState *s = (SDL_PANBState *)state;

    s->node = (uint8_t)SDL_ACIO_NextType(state, SDL_ACIO_TYPE_PANB, 0);
    if (s->node == 0) {
        s->step = SDL_PANB_STEP_IDLE;
        SDL_Serial_Log(&s->acio.base, "No PANB node on the bus, so no joystick opens");
        return;
    }
    if (!SDL_ACIO_Send(state, s->node, SDL_PANB_CMD_START, &nodes, 1)) {
        SDL_ACIO_Restart(state, now);
        return;
    }
    /* From here only a reset stops the stream */
    SDL_ACIO_ResetOnClose(state, true);
    s->step = SDL_PANB_STEP_STREAM;
    s->deadline = now + SDL_PANB_GAP_MS;
}

static void PANB_Frame(void *state, const SDL_ACIOFrame *frame, uint64_t now)
{
    SDL_PANBState *s = (SDL_PANBState *)state;

    if (s->step != SDL_PANB_STEP_STREAM || frame->command != SDL_PANB_CMD_STREAM || frame->length != SDL_PANB_INPUT) {
        return;
    }
    s->deadline = now + SDL_PANB_GAP_MS;
    PANB_Commit(s, frame->payload);
}

static void PANB_Down(void *state, uint64_t now)
{
    SDL_PANBState *s = (SDL_PANBState *)state;

    (void)now;
    SDL_Serial_AbsentAll(&s->acio.base);
    s->step = SDL_PANB_STEP_BUS;
    s->node = 0;
}

static bool PANB_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_PANBState *s = (SDL_PANBState *)state;

    if (s->step != SDL_PANB_STEP_STREAM) {
        return false;
    }
    *deadline = s->deadline;
    return true;
}

static void PANB_Tick(void *state, uint64_t now)
{
    SDL_PANBState *s = (SDL_PANBState *)state;

    if (s->step == SDL_PANB_STEP_STREAM && now >= s->deadline) {
        SDL_Serial_Log(&s->acio.base, "The PANB stream stopped, so the bus starts over");
        SDL_ACIO_Restart(state, now);
    }
}

/* The module starts no exchange, so it takes no reply */
static const SDL_ACIOHandler panb_handler = {
    PANB_Ready,
    NULL,
    PANB_Frame,
    PANB_Down,
    PANB_GetDeadline,
    PANB_Tick
};

static void PANB_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_ACIO_Reset(state, sizeof(SDL_PANBState), sink, &panb_handler, SDL_PANB_RATE, now);
    SDL_ACIO_SetAlternateRate(state, SDL_PANB_ALTERNATE_RATE);
}

/* Lamps stay off: no output request is taken */
static void PANB_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

const SDL_SerialModule SDL_SerialPANBModule = {
    "panb",
    sizeof(SDL_PANBState),
    false,
    0,
    0,
    PANB_Reset,
    SDL_ACIO_Feed,
    SDL_ACIO_Tick,
    SDL_Serial_NextAction,
    SDL_ACIO_ActionDone,
    PANB_Output,
    SDL_ACIO_GetDeadline,
    SDL_Serial_GetSnapshot
};
