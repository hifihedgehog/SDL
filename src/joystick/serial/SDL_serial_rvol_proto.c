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

#include "SDL_serial_rvol_proto.h"

#include <string.h>

void SDL_RVOL_Decode(const uint8_t *payload, SDL_SerialControls *controls)
{
    int i;

    memset(controls, 0, sizeof(*controls));
    for (i = 0; i < SDL_RVOL_AXES; ++i) {
        controls->axes[SDL_RVOL_AXIS_SPINNER1 + i] = (int16_t)(payload[16 + i] * 256 - 32768);
    }
    SDL_Serial_SetButton(controls, SDL_RVOL_BUTTON_SPINNER1, (payload[9] & 0x08) != 0);
    SDL_Serial_SetButton(controls, SDL_RVOL_BUTTON_SPINNER1 + 1, (payload[9] & 0x01) != 0);
    SDL_Serial_SetButton(controls, SDL_RVOL_BUTTON_SPINNER1 + 2, (payload[11] & 0x08) != 0);
    SDL_Serial_SetButton(controls, SDL_RVOL_BUTTON_SPINNER1 + 3, (payload[1] & 0x04) != 0);
    SDL_Serial_SetButton(controls, SDL_RVOL_BUTTON_SPINNER1 + 4, (payload[1] & 0x10) != 0);
    SDL_Serial_SetButton(controls, SDL_RVOL_BUTTON_PEDAL, (payload[1] & 0x08) == 0);
}

/* The output bytes, spinner and title lamps, all off */
static const uint8_t rvol_output[SDL_RVOL_OUTPUT] = { 0 };

static void RVOL_Poll(SDL_RVOLState *s, uint64_t now)
{
    s->step = SDL_RVOL_STEP_POLL;
    if (!SDL_ACIO_Request(s, s->node, SDL_RVOL_CMD_POLL, rvol_output, sizeof(rvol_output), SDL_RVOL_INPUT, SDL_RVOL_INPUT)) {
        SDL_ACIO_Restart(s, now);
    }
}

/* The first reply presents the joystick with its controls */
static void RVOL_Commit(SDL_RVOLState *s, const uint8_t *payload)
{
    SDL_SerialControls controls;
    SDL_SerialIdentity identity;

    SDL_RVOL_Decode(payload, &controls);
    if (SDL_Serial_IsPresent(&s->acio.base, 0)) {
        SDL_Serial_Commit(&s->acio.base, 0, &controls);
        return;
    }
    SDL_Serial_SetIdentity(&identity, SDL_RVOL_NAME, SDL_SERIAL_TYPE_UNKNOWN, SDL_RVOL_AXES, SDL_RVOL_BUTTONS, 0, 0);
    SDL_Serial_PresentWith(&s->acio.base, 0, &identity, &controls);
}

static void RVOL_Ready(void *state, uint64_t now)
{
    static const uint8_t expand = SDL_RVOL_EXPAND;
    SDL_RVOLState *s = (SDL_RVOLState *)state;

    s->node = (uint8_t)SDL_ACIO_FindType(state, SDL_ACIO_TYPE_RVOL);
    s->step = SDL_RVOL_STEP_IDLE;
    if (s->node == 0) {
        SDL_Serial_Log(&s->acio.base, "No RVOL node on the bus, so no joystick opens");
        return;
    }
    /* The reply's status is not used, so any payload length is taken */
    s->step = SDL_RVOL_STEP_EXPAND;
    if (!SDL_ACIO_Request(state, s->node, SDL_RVOL_CMD_EXPAND, &expand, 1, 0, SDL_ACIO_MAX_PAYLOAD)) {
        SDL_ACIO_Restart(state, now);
    }
}

static void RVOL_Reply(void *state, const SDL_ACIOFrame *reply, uint64_t now)
{
    SDL_RVOLState *s = (SDL_RVOLState *)state;

    switch (s->step) {
    case SDL_RVOL_STEP_EXPAND:
        if (!reply) {
            SDL_Serial_Log(&s->acio.base, "The RVOL node did not answer its expand mode, so the bus starts over");
            SDL_ACIO_Restart(state, now);
            break;
        }
        RVOL_Poll(s, now);
        break;
    case SDL_RVOL_STEP_POLL:
        if (!reply) {
            if (++s->failures >= SDL_RVOL_FAILURES) {
                SDL_Serial_Log(&s->acio.base, "The RVOL node failed three polls, so the bus starts over");
                SDL_ACIO_Restart(state, now);
                return;
            }
        } else {
            s->failures = 0;
            RVOL_Commit(s, reply->payload);
        }
        RVOL_Poll(s, now);
        break;
    default:
        break;
    }
}

static void RVOL_Down(void *state, uint64_t now)
{
    SDL_RVOLState *s = (SDL_RVOLState *)state;

    (void)now;
    SDL_Serial_AbsentAll(&s->acio.base);
    s->step = SDL_RVOL_STEP_BUS;
    s->node = 0;
    s->failures = 0;
}

static const SDL_ACIOHandler rvol_handler = {
    RVOL_Ready,
    RVOL_Reply,
    NULL,
    RVOL_Down,
    NULL,
    NULL
};

static void RVOL_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_ACIO_Reset(state, sizeof(SDL_RVOLState), sink, &rvol_handler, SDL_RVOL_RATE, now);
    SDL_ACIO_SetAlternateRate(state, SDL_RVOL_ALTERNATE_RATE);
}

/* Lamps stay off: no output request is taken */
static void RVOL_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

const SDL_SerialModule SDL_SerialRVOLModule = {
    "rvol",
    sizeof(SDL_RVOLState),
    false,
    0,
    0,
    RVOL_Reset,
    SDL_ACIO_Feed,
    SDL_ACIO_Tick,
    SDL_Serial_NextAction,
    SDL_ACIO_ActionDone,
    RVOL_Output,
    SDL_ACIO_GetDeadline,
    SDL_Serial_GetSnapshot
};
