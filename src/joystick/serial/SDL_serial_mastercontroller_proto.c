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

#include "SDL_serial_mastercontroller_proto.h"

#include <string.h>

static void MasterController_Commit(SDL_MasterControllerState *s)
{
    SDL_SerialControls controls;
    SDL_TrainIdentity identity;
    int i;

    SDL_Train_GetIdentity(SDL_TRAIN_MASTER, &identity);
    if (!SDL_Serial_IsPresent(&s->base, 0)) {
        SDL_SerialIdentity serial;

        SDL_Serial_SetIdentity(&serial, SDL_Train_Name(SDL_TRAIN_MASTER), SDL_SERIAL_TYPE_GAMEPAD,
                               identity.naxes, identity.nbuttons, 0, 0);
        /* B South, C East, A West and S North, the lever on the left stick's
           Y and the reverser on the right stick's */
        serial.has_mapping = true;
        serial.mapping.a = SDL_Serial_MapButton(0);
        serial.mapping.b = SDL_Serial_MapButton(1);
        serial.mapping.x = SDL_Serial_MapButton(2);
        serial.mapping.y = SDL_Serial_MapButton(3);
        serial.mapping.lefty = SDL_Serial_MapAxis(1);
        serial.mapping.righty = SDL_Serial_MapAxis(3);
        SDL_Serial_Present(&s->base, 0, &serial);
    }
    memset(&controls, 0, sizeof(controls));
    for (i = 0; i < identity.naxes; ++i) {
        controls.axes[i] = s->input.axes[i];
    }
    for (i = 0; i < identity.nbuttons; ++i) {
        SDL_Serial_SetButton(&controls, i, (s->input.buttons & (1u << i)) != 0);
    }
    SDL_Serial_Commit(&s->base, 0, &controls);
}

static void MasterController_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_MasterControllerState *s = (SDL_MasterControllerState *)state;
    const SDL_SerialLine line = SDL_Serial_Line(SDL_MASTERCONTROLLER_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, false, false);
    static const uint8_t start = 0x00;

    (void)now;
    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    SDL_Train_InitLine(&s->parser);
    SDL_Train_ResetState(SDL_TRAIN_MASTER, &s->input);
    SDL_Serial_QueueLine(&s->base, 0, &line);
    SDL_Serial_QueueWrite(&s->base, 0, &start, 1);
}

static void MasterController_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_MasterControllerState *s = (SDL_MasterControllerState *)state;

    (void)now;
    if (SDL_Train_FeedLine(&s->parser, data, length, &s->input)) {
        MasterController_Commit(s);
    }
}

static void MasterController_Tick(void *state, uint64_t now)
{
    (void)state;
    (void)now;
}

static void MasterController_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_MasterControllerState *s = (SDL_MasterControllerState *)state;
    uint8_t tag;

    (void)success;
    (void)now;
    (void)SDL_Serial_FinishAction(&s->base, &tag);
}

static void MasterController_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

static bool MasterController_GetDeadline(void *state, uint64_t *deadline)
{
    (void)state;
    (void)deadline;
    return false;
}

const SDL_SerialModule SDL_SerialMasterControllerModule = {
    "mastercontroller",
    sizeof(SDL_MasterControllerState),
    false,
    0,
    0,
    MasterController_Reset,
    MasterController_Feed,
    MasterController_Tick,
    SDL_Serial_NextAction,
    MasterController_ActionDone,
    MasterController_Output,
    MasterController_GetDeadline,
    SDL_Serial_GetSnapshot
};
