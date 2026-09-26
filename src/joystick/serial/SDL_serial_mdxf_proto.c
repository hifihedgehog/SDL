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

#include "SDL_serial_mdxf_proto.h"

#include <string.h>

void SDL_MDXF_Decode(const uint8_t *payload, SDL_SerialControls *controls)
{
    memset(controls, 0, sizeof(*controls));
    SDL_Serial_SetButton(controls, SDL_MDXF_BUTTON_UP, (payload[0] & 0xF0) != 0);
    SDL_Serial_SetButton(controls, SDL_MDXF_BUTTON_DOWN, (payload[0] & 0x0F) != 0);
    SDL_Serial_SetButton(controls, SDL_MDXF_BUTTON_LEFT, (payload[1] & 0xF0) != 0);
    SDL_Serial_SetButton(controls, SDL_MDXF_BUTTON_RIGHT, (payload[1] & 0x0F) != 0);
}

static void MDXF_Poll(SDL_MDXFState *s, uint64_t now)
{
    s->step = SDL_MDXF_STEP_POLL;
    if (!SDL_ACIO_Request(s, s->nodes[s->player], SDL_MDXF_CMD_POLL, NULL, 0, SDL_MDXF_INPUT, SDL_MDXF_INPUT)) {
        SDL_ACIO_Restart(s, now);
    }
}

/* A node's first reply presents its player's joystick with its controls */
static void MDXF_Commit(SDL_MDXFState *s, int player, const uint8_t *payload)
{
    SDL_SerialControls controls;
    SDL_SerialIdentity identity;

    SDL_MDXF_Decode(payload, &controls);
    if (SDL_Serial_IsPresent(&s->acio.base, player)) {
        SDL_Serial_Commit(&s->acio.base, player, &controls);
        return;
    }
    SDL_Serial_SetIdentity(&identity, (player == 0) ? "Konami DDR Stage P1" : "Konami DDR Stage P2", SDL_SERIAL_TYPE_DANCE_PAD,
                           0, SDL_MDXF_BUTTONS, 0, 0);
    identity.has_mapping = true;
    identity.mapping.dpup = SDL_Serial_MapButton(SDL_MDXF_BUTTON_UP);
    identity.mapping.dpdown = SDL_Serial_MapButton(SDL_MDXF_BUTTON_DOWN);
    identity.mapping.dpleft = SDL_Serial_MapButton(SDL_MDXF_BUTTON_LEFT);
    identity.mapping.dpright = SDL_Serial_MapButton(SDL_MDXF_BUTTON_RIGHT);
    SDL_Serial_PresentWith(&s->acio.base, player, &identity, &controls);
}

static void MDXF_Ready(void *state, uint64_t now)
{
    SDL_MDXFState *s = (SDL_MDXFState *)state;
    int address = SDL_ACIO_NextType(state, SDL_ACIO_TYPE_MDXF, 0);

    s->players = 0;
    while (address != 0 && s->players < SDL_MDXF_PLAYERS) {
        s->nodes[s->players++] = (uint8_t)address;
        address = SDL_ACIO_NextType(state, SDL_ACIO_TYPE_MDXF, address);
    }
    if (s->players == 0) {
        s->step = SDL_MDXF_STEP_IDLE;
        SDL_Serial_Log(&s->acio.base, "No MDXF node on the bus, so no joystick opens");
        return;
    }
    if (address != 0) {
        SDL_Serial_Log(&s->acio.base, "More than two MDXF nodes on the bus: the first two are players 1 and 2");
    }
    s->player = 0;
    MDXF_Poll(s, now);
}

static void MDXF_Reply(void *state, const SDL_ACIOFrame *reply, uint64_t now)
{
    SDL_MDXFState *s = (SDL_MDXFState *)state;

    /* Every exchange is a poll */
    if (!reply) {
        if (++s->failures[s->player] >= SDL_MDXF_FAILURES) {
            SDL_Serial_Log(&s->acio.base, "An MDXF node failed three polls, so the bus starts over");
            SDL_ACIO_Restart(state, now);
            return;
        }
    } else {
        s->failures[s->player] = 0;
        MDXF_Commit(s, s->player, reply->payload);
    }
    s->player = (s->player + 1) % s->players;
    MDXF_Poll(s, now);
}

static void MDXF_Down(void *state, uint64_t now)
{
    SDL_MDXFState *s = (SDL_MDXFState *)state;

    (void)now;
    SDL_Serial_AbsentAll(&s->acio.base);
    s->step = SDL_MDXF_STEP_BUS;
    s->players = 0;
    s->player = 0;
    memset(s->nodes, 0, sizeof(s->nodes));
    memset(s->failures, 0, sizeof(s->failures));
}

static const SDL_ACIOHandler mdxf_handler = {
    MDXF_Ready,
    MDXF_Reply,
    NULL,
    MDXF_Down,
    NULL,
    NULL
};

static void MDXF_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_ACIO_Reset(state, sizeof(SDL_MDXFState), sink, &mdxf_handler, SDL_MDXF_RATE, now);
}

/* Lamps stay off: no output request is taken */
static void MDXF_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

const SDL_SerialModule SDL_SerialMDXFModule = {
    "mdxf",
    sizeof(SDL_MDXFState),
    false,
    0,
    0,
    MDXF_Reset,
    SDL_ACIO_Feed,
    SDL_ACIO_Tick,
    SDL_Serial_NextAction,
    SDL_ACIO_ActionDone,
    MDXF_Output,
    SDL_ACIO_GetDeadline,
    SDL_Serial_GetSnapshot
};
