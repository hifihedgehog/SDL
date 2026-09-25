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

#include "SDL_serial_iforce_proto.h"

#include <stdio.h>
#include <string.h>

static uint8_t IForce_NextTag(SDL_IForceSerialState *s)
{
    if (++s->write_tag == 0) {
        s->write_tag = 1;
    }
    return s->write_tag;
}

static void IForce_Commit(SDL_IForceSerialState *s)
{
    SDL_IForceIdentity identity;
    SDL_SerialControls controls;
    int i;

    SDL_IForce_GetIdentity(s->model, &identity);
    memset(&controls, 0, sizeof(controls));
    for (i = 0; i < identity.naxes; ++i) {
        controls.axes[i] = s->input.axes[i];
    }
    for (i = 0; i < identity.nbuttons; ++i) {
        SDL_Serial_SetButton(&controls, i, (s->input.buttons & (1u << i)) != 0);
    }
    for (i = 0; i < identity.nhats; ++i) {
        controls.hats[i] = s->input.hats[i];
    }
    SDL_Serial_Commit(&s->base, 0, &controls);
}

/* The sequence is over. With O answered, M and P pick the layout. */
static void IForce_Identify(SDL_IForceSerialState *s)
{
    const SDL_IForceQueries *q = &s->queries;
    SDL_IForceIdentity identity;
    SDL_SerialIdentity serial;
    char name[SDL_SERIAL_NAME_LENGTH];

    if (!q->open) {
        SDL_Serial_Log(&s->base, "no answer to O after 20 tries");
        return;
    }
    s->model = SDL_IForce_FindModel(q->vendor_id, q->product_id, false);
    if (s->model) {
        (void)snprintf(name, sizeof(name), "%s", s->model->name);
    } else {
        s->model = SDL_IForce_UnknownModel();
        (void)snprintf(name, sizeof(name), "%s [%04x:%04x]", s->model->name, q->vendor_id, q->product_id);
    }
    SDL_IForce_GetIdentity(s->model, &identity);
    SDL_Serial_SetIdentity(&serial, name, identity.wheel ? SDL_SERIAL_TYPE_WHEEL : SDL_SERIAL_TYPE_FLIGHT_STICK,
                           identity.naxes, identity.nbuttons, identity.nhats, 0);
    SDL_Serial_Present(&s->base, 0, &serial);
    SDL_IForce_ResetState(s->model, &s->input);
    IForce_Commit(s);
}

/* Asks the next letter, or ends the sequence */
static void IForce_Query(SDL_IForceSerialState *s, uint64_t now)
{
    const uint8_t letter = SDL_IForce_NextQuery(&s->queries);
    uint8_t frame[8];
    size_t length;

    s->waiting = false;
    s->timer = false;
    if (!letter) {
        IForce_Identify(s);
        return;
    }
    length = SDL_IForce_BuildSerialFrame(frame, sizeof(frame), SDL_IFORCE_CMD_QUERY, &letter, 1);
    s->query_tag = IForce_NextTag(s);
    s->waiting = true;
    if (!SDL_Serial_QueueWrite(&s->base, s->query_tag, frame, length)) {
        /* A full queue counts as a query that went unanswered */
        s->query_tag = 0;
        s->timer = true;
        s->deadline = now + SDL_IFORCE_QUERY_TIMEOUT_MS;
    }
}

typedef struct IForce_FrameContext
{
    SDL_IForceSerialState *state;
    uint64_t now;
} IForce_FrameContext;

static void IForce_OnFrame(void *userdata, uint8_t type, const uint8_t *data, size_t length)
{
    const IForce_FrameContext *context = (const IForce_FrameContext *)userdata;
    SDL_IForceSerialState *s = context->state;

    if (type == SDL_IFORCE_PACKET_QUERY) {
        /* Any FF frame ends the wait, and a wrong letter fails the query */
        if (s->waiting) {
            SDL_IForce_QueryResult(&s->queries, data, length);
            IForce_Query(s, context->now);
        }
        return;
    }
    /* Input before identification is dropped, as Linux drops it */
    if (!s->model) {
        return;
    }
    if (SDL_IForce_DecodePacket(s->model, type, data, length, &s->input, NULL) != SDL_IFORCE_PACKET_IGNORED) {
        IForce_Commit(s);
    }
}

static void IForce_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_IForceSerialState *s = (SDL_IForceSerialState *)state;
    const SDL_SerialLine line = SDL_Serial_Line(SDL_IFORCE_SERIAL_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);
    const uint8_t write_tag = s->write_tag;

    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    /* Tags keep counting, so a done from before the reset matches nothing */
    s->write_tag = write_tag;
    SDL_IForce_InitSerialParser(&s->parser);
    SDL_IForce_InitQueries(&s->queries);
    SDL_Serial_QueueLine(&s->base, 0, &line);
    IForce_Query(s, now);
}

static void IForce_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    IForce_FrameContext context;

    context.state = (SDL_IForceSerialState *)state;
    context.now = now;
    SDL_IForce_FeedSerial(&context.state->parser, data, length, IForce_OnFrame, &context);
}

static void IForce_Tick(void *state, uint64_t now)
{
    SDL_IForceSerialState *s = (SDL_IForceSerialState *)state;

    if (s->waiting && s->timer && now >= s->deadline) {
        SDL_IForce_QueryResult(&s->queries, NULL, 0);
        IForce_Query(s, now);
    }
}

static void IForce_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_IForceSerialState *s = (SDL_IForceSerialState *)state;
    uint8_t tag;

    /* A failed write counts as sent. Its reply never comes, and the window closes. */
    (void)success;
    if (!SDL_Serial_FinishAction(&s->base, &tag) || tag == 0) {
        return;
    }
    if (s->waiting && tag == s->query_tag) {
        s->timer = true;
        s->deadline = now + SDL_IFORCE_QUERY_TIMEOUT_MS;
    }
}

/* An effect is one whole command, sent in a frame */
static void IForce_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    SDL_IForceSerialState *s = (SDL_IForceSerialState *)state;
    uint8_t frame[4 + SDL_IFORCE_MAX_LENGTH];
    size_t length;

    (void)now;
    if (request->kind != SDL_SERIAL_OUTPUT_EFFECT || !s->model ||
        !SDL_IForce_IsCommand(request->data, request->length)) {
        return;
    }
    length = SDL_IForce_BuildSerialFrame(frame, sizeof(frame), request->data[0], request->data + 1, request->length - 1);
    if (length) {
        SDL_Serial_QueueWrite(&s->base, IForce_NextTag(s), frame, length);
    }
}

static bool IForce_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_IForceSerialState *s = (SDL_IForceSerialState *)state;

    if (s->waiting && s->timer) {
        *deadline = s->deadline;
        return true;
    }
    return false;
}

const SDL_SerialModule SDL_SerialIForceModule = {
    "iforce",
    sizeof(SDL_IForceSerialState),
    false,
    2,
    SDL_IFORCE_MAX_COMMAND,
    IForce_Reset,
    IForce_Feed,
    IForce_Tick,
    SDL_Serial_NextAction,
    IForce_ActionDone,
    IForce_Output,
    IForce_GetDeadline,
    SDL_Serial_GetSnapshot
};
