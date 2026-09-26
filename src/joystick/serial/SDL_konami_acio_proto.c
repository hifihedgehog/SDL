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

#include "SDL_konami_acio_proto.h"

#include <string.h>

size_t SDL_ACIO_EncodeFrame(uint8_t address, uint16_t command, uint8_t sequence, const uint8_t *payload, size_t length, uint8_t *out, size_t size)
{
    uint8_t body[SDL_ACIO_MAX_BODY];
    size_t n = 0, o = 0, i;
    uint8_t sum = 0;

    if (address == SDL_ACIO_BROADCAST || length > SDL_ACIO_MAX_PAYLOAD || (length && !payload) || !out || size == 0) {
        return 0;
    }
    body[n++] = address;
    body[n++] = (uint8_t)(command >> 8);
    body[n++] = (uint8_t)command;
    body[n++] = sequence;
    body[n++] = (uint8_t)length;
    if (length) {
        memcpy(body + n, payload, length);
        n += length;
    }
    for (i = 0; i < n; ++i) {
        sum = (uint8_t)(sum + body[i]);
    }
    body[n++] = sum;

    out[o++] = SDL_ACIO_SOF;
    for (i = 0; i < n; ++i) {
        if (body[i] == SDL_ACIO_SOF || body[i] == SDL_ACIO_ESCAPE) {
            if (o + 2 > size) {
                return 0;
            }
            out[o++] = SDL_ACIO_ESCAPE;
            out[o++] = (uint8_t)~body[i];
        } else {
            if (o + 1 > size) {
                return 0;
            }
            out[o++] = body[i];
        }
    }
    return o;
}

void SDL_ACIO_InitDecoder(SDL_ACIODecoder *decoder)
{
    memset(decoder, 0, sizeof(*decoder));
}

/* The body length the frame needs, 0 until its length byte has come */
static size_t ACIO_BodyLength(const SDL_ACIODecoder *decoder)
{
    if (decoder->body[0] == SDL_ACIO_BROADCAST) {
        return (decoder->length >= 2) ? 2 + (size_t)decoder->body[1] + 1 : 0;
    }
    return (decoder->length >= SDL_ACIO_HEADER) ? SDL_ACIO_HEADER + (size_t)decoder->body[4] + 1 : 0;
}

bool SDL_ACIO_DecodeByte(SDL_ACIODecoder *decoder, uint8_t byte, SDL_ACIOFrame *frame)
{
    const uint8_t *body = decoder->body;
    size_t need, i;
    uint8_t sum = 0;

    if (byte == SDL_ACIO_SOF) {
        decoder->in_frame = true;
        decoder->escape = false;
        decoder->length = 0;
        return false;
    }
    if (!decoder->in_frame) {
        return false;
    }
    if (decoder->escape) {
        decoder->escape = false;
        byte = (uint8_t)~byte;
    } else if (byte == SDL_ACIO_ESCAPE) {
        decoder->escape = true;
        return false;
    }
    decoder->body[decoder->length++] = byte;
    need = ACIO_BodyLength(decoder);
    if (need == 0 || decoder->length < need) {
        return false;
    }
    decoder->in_frame = false;
    for (i = 0; i + 1 < need; ++i) {
        sum = (uint8_t)(sum + body[i]);
    }
    if (sum != body[need - 1]) {
        ++decoder->bad_checksums;
        return false;
    }
    frame->address = body[0];
    frame->broadcast = (body[0] == SDL_ACIO_BROADCAST);
    if (frame->broadcast) {
        frame->command = 0;
        frame->sequence = 0;
        frame->length = body[1];
        frame->payload = body + 2;
    } else {
        frame->command = (uint16_t)((body[1] << 8) | body[2]);
        frame->sequence = body[3];
        frame->length = body[4];
        frame->payload = body + SDL_ACIO_HEADER;
    }
    return true;
}

/* Printable ASCII up to the first 00 */
static void ACIO_Text(char *out, const uint8_t *in, size_t size)
{
    size_t i;

    for (i = 0; i < size && in[i]; ++i) {
        out[i] = (in[i] >= 0x20 && in[i] <= 0x7E) ? (char)in[i] : '?';
    }
    out[i] = '\0';
}

bool SDL_ACIO_DecodeVersion(const uint8_t *payload, size_t length, SDL_ACIONodeVersion *version)
{
    if (!payload || !version || length != SDL_ACIO_VERSION_LENGTH) {
        return false;
    }
    memset(version, 0, sizeof(*version));
    version->type = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) | ((uint32_t)payload[2] << 8) | payload[3];
    version->flag = payload[4];
    version->major = payload[5];
    version->minor = payload[6];
    version->revision = payload[7];
    ACIO_Text(version->product, payload + 8, 4);
    ACIO_Text(version->date, payload + 12, 16);
    ACIO_Text(version->time, payload + 28, 16);
    return true;
}

static SDL_SerialBase *ACIO_Base(void *state)
{
    return &((SDL_ACIOState *)state)->base;
}

static SDL_ACIOBus *ACIO_Bus(void *state)
{
    return &((SDL_ACIOState *)state)->bus;
}

static const SDL_ACIOBus *ACIO_ConstBus(const void *state)
{
    return &((const SDL_ACIOState *)state)->bus;
}

static void ACIO_Append(char *line, size_t size, size_t *used, const char *text)
{
    while (*text && *used + 1 < size) {
        line[(*used)++] = *text++;
    }
    line[*used] = '\0';
}

/* A number in base 10 or 16, at least digits long */
static void ACIO_AppendNumber(char *line, size_t size, size_t *used, uint32_t value, uint32_t base, int digits)
{
    static const char symbols[] = "0123456789ABCDEF";
    char text[16];
    int n = 0;

    do {
        text[n++] = symbols[value % base];
        value /= base;
    } while (value || n < digits);
    while (n > 0 && *used + 1 < size) {
        line[(*used)++] = text[--n];
    }
    line[*used] = '\0';
}

static void ACIO_LogVersion(void *state, int address, const SDL_ACIONodeVersion *version)
{
    char line[96];
    size_t used = 0;

    line[0] = '\0';
    ACIO_Append(line, sizeof(line), &used, "ACIO node ");
    ACIO_AppendNumber(line, sizeof(line), &used, (uint32_t)address, 10, 1);
    ACIO_Append(line, sizeof(line), &used, ": ");
    ACIO_Append(line, sizeof(line), &used, version->product);
    ACIO_Append(line, sizeof(line), &used, ", type ");
    ACIO_AppendNumber(line, sizeof(line), &used, version->type, 16, 8);
    ACIO_Append(line, sizeof(line), &used, ", version ");
    ACIO_AppendNumber(line, sizeof(line), &used, version->major, 10, 1);
    ACIO_Append(line, sizeof(line), &used, ".");
    ACIO_AppendNumber(line, sizeof(line), &used, version->minor, 10, 1);
    ACIO_Append(line, sizeof(line), &used, ".");
    ACIO_AppendNumber(line, sizeof(line), &used, version->revision, 10, 1);
    ACIO_Append(line, sizeof(line), &used, ", ");
    ACIO_Append(line, sizeof(line), &used, version->date);
    ACIO_Append(line, sizeof(line), &used, " ");
    ACIO_Append(line, sizeof(line), &used, version->time);
    SDL_Serial_Log(ACIO_Base(state), line);
}

static uint8_t ACIO_NextTag(SDL_ACIOBus *bus)
{
    if (++bus->action_tag == 0) {
        bus->action_tag = 1;
    }
    return bus->action_tag;
}

static void ACIO_SetTimer(SDL_ACIOBus *bus, uint64_t at)
{
    bus->deadline = at;
    bus->timer = true;
}

/* Queues bytes as writes of at most SDL_SERIAL_MAX_WRITE. With track, the
 * last write gets the tag whose completion the bus awaits. False, queuing
 * nothing, when the queue has no room for every write. */
static bool ACIO_Write(void *state, const uint8_t *data, size_t length, bool track)
{
    SDL_SerialBase *base = ACIO_Base(state);
    SDL_ACIOBus *bus = ACIO_Bus(state);
    const size_t writes = (length + SDL_SERIAL_MAX_WRITE - 1) / SDL_SERIAL_MAX_WRITE;
    size_t offset = 0;

    if (length == 0 || writes > (size_t)(SDL_SERIAL_MAX_ACTIONS - base->action_count)) {
        return false;
    }
    while (offset < length) {
        const size_t chunk = (length - offset < SDL_SERIAL_MAX_WRITE) ? length - offset : SDL_SERIAL_MAX_WRITE;
        uint8_t tag = 0;

        if (track && offset + chunk == length) {
            tag = ACIO_NextTag(bus);
            bus->waiting_tag = tag;
        }
        (void)SDL_Serial_QueueWrite(base, tag, data + offset, chunk);
        offset += chunk;
    }
    return true;
}

/* Builds and queues a frame with the next sequence */
static bool ACIO_SendFrame(void *state, uint8_t address, uint16_t command, const uint8_t *payload, size_t length, bool track)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);
    uint8_t frame[SDL_ACIO_MAX_FRAME];
    const size_t n = SDL_ACIO_EncodeFrame(address, command, bus->sequence, payload, length, frame, sizeof(frame));

    if (n == 0 || !ACIO_Write(state, frame, n, track)) {
        return false;
    }
    ++bus->sequence;
    return true;
}

static bool ACIO_Exchange(void *state, uint8_t address, uint16_t command, const uint8_t *payload, size_t length, size_t min_reply, size_t max_reply)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);

    if (!ACIO_SendFrame(state, address, command, payload, length, true)) {
        return false;
    }
    bus->pending = true;
    bus->timer = false;
    bus->address = address;
    bus->command = command;
    bus->min_reply = min_reply;
    bus->max_reply = max_reply;
    return true;
}

/* A completion the request still owes finds no pending exchange */
static void ACIO_EndExchange(SDL_ACIOBus *bus)
{
    bus->pending = false;
    bus->timer = false;
}

/* 525 bytes of 00, then the break */
static void ACIO_StartReset(void *state)
{
    static const uint8_t zeros[SDL_ACIO_RESET_ZEROS] = { 0 };
    SDL_ACIOBus *bus = ACIO_Bus(state);

    /* The bus was just cleared, so the decoder is empty and the queue holds
       at most the line, which leaves room for the 9 writes */
    bus->step = SDL_ACIO_STEP_ZEROS;
    bus->sequence = 1;
    (void)ACIO_Write(state, zeros, sizeof(zeros), true);
}

/* 8N1 with DTR and RTS on, at rate */
static void ACIO_QueueLine(void *state, uint32_t rate)
{
    const SDL_SerialLine line = SDL_Serial_Line(rate, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);

    (void)SDL_Serial_QueueLine(ACIO_Base(state), 0, &line);
}

static void ACIO_LogRate(void *state, uint32_t tried, uint32_t next)
{
    char line[96];
    size_t used = 0;

    line[0] = '\0';
    ACIO_Append(line, sizeof(line), &used, "ACIO bus: nothing came up at ");
    ACIO_AppendNumber(line, sizeof(line), &used, tried, 10, 1);
    ACIO_Append(line, sizeof(line), &used, " baud, so the bus tries ");
    ACIO_AppendNumber(line, sizeof(line), &used, next, 10, 1);
    SDL_Serial_Log(ACIO_Base(state), line);
}

static void ACIO_StartOver(void *state, uint64_t now)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);
    const SDL_ACIOHandler *handler = bus->handler;
    const uint8_t action_tag = bus->action_tag;
    const uint32_t restarts = bus->restarts;
    const bool up = (bus->step == SDL_ACIO_STEP_READY);
    const bool closing = bus->closing;
    uint32_t rate = bus->rate;
    uint32_t alternate_rate = bus->alternate_rate;

    SDL_Serial_ClearActions(ACIO_Base(state));
    memset(bus, 0, sizeof(*bus));
    bus->handler = handler;
    bus->action_tag = action_tag;
    bus->restarts = restarts + 1;
    bus->closing = closing;
    if (!up && !closing && alternate_rate) {
        /* Nothing came up at this rate, so the next reset runs at the other */
        const uint32_t tried = rate;

        rate = alternate_rate;
        alternate_rate = tried;
        ACIO_LogRate(state, tried, rate);
        ACIO_QueueLine(state, rate);
    }
    bus->rate = rate;
    bus->alternate_rate = alternate_rate;
    if (handler->Down) {
        handler->Down(state, now);
    }
    ACIO_StartReset(state);
}

/* The port is to close: the zeros and the break go out once more, and the
   port closes when the break ends */
static void ACIO_Close(void *state, uint64_t now)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);

    bus->closing = true;
    SDL_Serial_Log(ACIO_Base(state), "ACIO bus: the port closes, so the bus resets the nodes first");
    if (bus->step < SDL_ACIO_STEP_SETTLE) {
        /* A reset is under way, and its break ends the close */
        return;
    }
    ACIO_StartOver(state, now);
}

/* A bring-up request. One that cannot be queued starts the bus over. */
static void ACIO_Ask(void *state, uint8_t address, uint16_t command, const uint8_t *payload, size_t length, size_t min_reply, size_t max_reply, uint64_t now)
{
    if (!ACIO_Exchange(state, address, command, payload, length, min_reply, max_reply)) {
        ACIO_StartOver(state, now);
    }
}

static void ACIO_Break(void *state, bool on)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);
    const uint8_t tag = ACIO_NextTag(bus);

    bus->step = on ? SDL_ACIO_STEP_BREAK_SET : SDL_ACIO_STEP_BREAK_CLEAR;
    bus->waiting_tag = tag;
    (void)SDL_Serial_QueueBreak(ACIO_Base(state), tag, on);
}

static void ACIO_Probe(void *state)
{
    static const uint8_t probe = SDL_ACIO_SOF;
    SDL_ACIOBus *bus = ACIO_Bus(state);

    ++bus->probes;
    (void)ACIO_Write(state, &probe, 1, true);
}

/* An AA came back: enumeration, with the payload 00 */
static void ACIO_Enumerate(void *state, uint64_t now)
{
    static const uint8_t first = 0x00;

    /* The request stops the probe timer, and its tag replaces the probe's */
    ACIO_Bus(state)->step = SDL_ACIO_STEP_ENUMERATE;
    ACIO_Ask(state, 0x00, SDL_ACIO_CMD_ENUMERATE, &first, 1, 1, 1, now);
}

static void ACIO_AskVersion(void *state, uint64_t now)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);

    ACIO_Ask(state, (uint8_t)(bus->node + 1), SDL_ACIO_CMD_GET_VERSION, NULL, 0, SDL_ACIO_VERSION_LENGTH, SDL_ACIO_VERSION_LENGTH, now);
}

/* The start reply's status is not used, so any payload length is taken */
static void ACIO_AskStart(void *state, uint64_t now)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);

    ACIO_Ask(state, (uint8_t)(bus->node + 1), SDL_ACIO_CMD_START, NULL, 0, 0, SDL_ACIO_MAX_PAYLOAD, now);
}

static void ACIO_Enumerated(void *state, int count, uint64_t now)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);

    if (count == 0 || count > SDL_ACIO_MAX_NODES) {
        SDL_Serial_Log(ACIO_Base(state), "ACIO bus: no node, or more than 16, so the bus starts over");
        ACIO_StartOver(state, now);
        return;
    }
    bus->nodes = count;
    bus->node = 0;
    bus->step = SDL_ACIO_STEP_VERSION;
    ACIO_AskVersion(state, now);
}

static void ACIO_Versioned(void *state, const SDL_ACIOFrame *reply, uint64_t now)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);
    SDL_ACIONodeVersion *version = &bus->versions[bus->node];

    (void)SDL_ACIO_DecodeVersion(reply->payload, reply->length, version);
    ACIO_LogVersion(state, bus->node + 1, version);
    if (++bus->node < bus->nodes) {
        ACIO_AskVersion(state, now);
        return;
    }
    bus->node = 0;
    bus->step = SDL_ACIO_STEP_START;
    ACIO_AskStart(state, now);
}

static void ACIO_Started(void *state, uint64_t now)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);

    if (++bus->node < bus->nodes) {
        ACIO_AskStart(state, now);
        return;
    }
    bus->node = 0;
    bus->step = SDL_ACIO_STEP_READY;
    if (bus->handler->Ready) {
        bus->handler->Ready(state, now);
    }
}

/* The exchange failed. Before Ready the bus starts over. */
static void ACIO_Fail(void *state, uint64_t now)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);

    ACIO_EndExchange(bus);
    if (bus->step == SDL_ACIO_STEP_READY) {
        if (bus->handler->Reply) {
            bus->handler->Reply(state, NULL, now);
        }
        return;
    }
    SDL_Serial_Log(ACIO_Base(state), "ACIO bus: a node failed its start-up, so the bus starts over");
    ACIO_StartOver(state, now);
}

static void ACIO_Frame(void *state, const SDL_ACIOFrame *frame, uint64_t now)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);

    /* Only the addressed node answers, so frames from elsewhere are not a
       reply */
    if (!bus->pending || frame->broadcast || (frame->address & ~SDL_ACIO_REPLY) != bus->address) {
        if (bus->step == SDL_ACIO_STEP_READY && bus->handler->Frame) {
            bus->handler->Frame(state, frame, now);
        }
        return;
    }
    if (frame->command != bus->command || frame->length < bus->min_reply || frame->length > bus->max_reply) {
        ACIO_Fail(state, now);
        return;
    }
    ACIO_EndExchange(bus);
    switch (bus->step) {
    case SDL_ACIO_STEP_ENUMERATE:
        ACIO_Enumerated(state, frame->payload[0], now);
        break;
    case SDL_ACIO_STEP_VERSION:
        ACIO_Versioned(state, frame, now);
        break;
    case SDL_ACIO_STEP_START:
        ACIO_Started(state, now);
        break;
    default:
        if (bus->handler->Reply) {
            bus->handler->Reply(state, frame, now);
        }
        break;
    }
}

void SDL_ACIO_Reset(void *state, size_t state_size, const SDL_SerialSink *sink, const SDL_ACIOHandler *handler, uint32_t rate, uint64_t now)
{
    SDL_ACIOState *s = (SDL_ACIOState *)state;
    const uint8_t action_tag = s->bus.action_tag;

    (void)now;
    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)state + sizeof(s->base), 0, state_size - sizeof(s->base));
    s->bus.handler = handler;
    s->bus.action_tag = action_tag;
    s->bus.rate = rate;
    ACIO_QueueLine(state, rate);
    ACIO_StartReset(state);
}

void SDL_ACIO_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);
    size_t i;

    for (i = 0; i < length; ++i) {
        SDL_ACIOFrame frame;

        switch (bus->step) {
        case SDL_ACIO_STEP_PROBE:
            if (data[i] == SDL_ACIO_SOF) {
                /* The rest of what came with the AA is dropped, as
                   bemanitools empties its buffer after the AA */
                ACIO_Enumerate(state, now);
                return;
            }
            break;
        case SDL_ACIO_STEP_ENUMERATE:
        case SDL_ACIO_STEP_VERSION:
        case SDL_ACIO_STEP_START:
        case SDL_ACIO_STEP_READY:
            if (SDL_ACIO_DecodeByte(&bus->decoder, data[i], &frame)) {
                ACIO_Frame(state, &frame, now);
            }
            break;
        default:
            /* The reset drops every byte, where bemanitools discards 100
               reads after the break */
            break;
        }
    }
}

void SDL_ACIO_Tick(void *state, uint64_t now)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);

    if (ACIO_Base(state)->closing && !bus->closing) {
        ACIO_Close(state, now);
    }
    SDL_Serial_TickPulses(ACIO_Base(state), now);
    if (bus->timer && now >= bus->deadline) {
        bus->timer = false;
        switch (bus->step) {
        case SDL_ACIO_STEP_BREAK:
            ACIO_Break(state, false);
            break;
        case SDL_ACIO_STEP_SETTLE:
            bus->step = SDL_ACIO_STEP_PROBE;
            ACIO_Probe(state);
            break;
        case SDL_ACIO_STEP_PROBE:
            if (bus->probes >= SDL_ACIO_PROBES) {
                SDL_Serial_Log(ACIO_Base(state), "ACIO bus: no answer to the reset, so the bus starts over");
                ACIO_StartOver(state, now);
            } else {
                ACIO_Probe(state);
            }
            break;
        default:
            if (bus->pending) {
                ACIO_Fail(state, now);
            }
            break;
        }
    }
    if (!bus->closing && bus->handler->Tick) {
        bus->handler->Tick(state, now);
    }
}

void SDL_ACIO_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);
    uint8_t tag;

    if (!SDL_Serial_FinishAction(ACIO_Base(state), &tag) || tag == 0 || tag != bus->waiting_tag) {
        return;
    }
    bus->waiting_tag = 0;
    switch (bus->step) {
    case SDL_ACIO_STEP_ZEROS:
        ACIO_Break(state, true);
        break;
    case SDL_ACIO_STEP_BREAK_SET:
        bus->step = SDL_ACIO_STEP_BREAK;
        ACIO_SetTimer(bus, now + SDL_ACIO_BREAK_MS);
        break;
    case SDL_ACIO_STEP_BREAK_CLEAR:
        if (bus->closing) {
            /* The nodes are reset, and the port may close */
            ACIO_Base(state)->closed = true;
            break;
        }
        bus->step = SDL_ACIO_STEP_SETTLE;
        ACIO_SetTimer(bus, now + SDL_ACIO_SETTLE_MS);
        break;
    case SDL_ACIO_STEP_PROBE:
        ACIO_SetTimer(bus, now + SDL_ACIO_PROBE_MS);
        break;
    default:
        /* A request left, so its reply is due */
        if (!bus->pending) {
            break;
        }
        if (!success) {
            ACIO_Fail(state, now);
            break;
        }
        ACIO_SetTimer(bus, now + SDL_ACIO_REPLY_MS);
        break;
    }
}

bool SDL_ACIO_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);
    bool have = false;
    uint64_t module_deadline;

    if (bus->timer) {
        SDL_Serial_EarlierDeadline(&have, deadline, bus->deadline);
    }
    SDL_Serial_PulseDeadline(ACIO_Base(state), &have, deadline);
    if (!bus->closing && bus->handler->GetDeadline && bus->handler->GetDeadline(state, &module_deadline)) {
        SDL_Serial_EarlierDeadline(&have, deadline, module_deadline);
    }
    return have;
}

bool SDL_ACIO_Request(void *state, uint8_t address, uint16_t command, const uint8_t *payload, size_t length, size_t min_reply, size_t max_reply)
{
    SDL_ACIOBus *bus = ACIO_Bus(state);

    if (bus->step != SDL_ACIO_STEP_READY || bus->pending || min_reply > max_reply) {
        return false;
    }
    return ACIO_Exchange(state, address, command, payload, length, min_reply, max_reply);
}

bool SDL_ACIO_Send(void *state, uint8_t address, uint16_t command, const uint8_t *payload, size_t length)
{
    if (ACIO_Bus(state)->step != SDL_ACIO_STEP_READY) {
        return false;
    }
    return ACIO_SendFrame(state, address, command, payload, length, false);
}

void SDL_ACIO_Restart(void *state, uint64_t now)
{
    if (ACIO_Bus(state)->step < SDL_ACIO_STEP_PROBE) {
        return;
    }
    ACIO_StartOver(state, now);
}

int SDL_ACIO_GetNodeCount(const void *state)
{
    const SDL_ACIOBus *bus = ACIO_ConstBus(state);

    return (bus->step == SDL_ACIO_STEP_READY) ? bus->nodes : 0;
}

const SDL_ACIONodeVersion *SDL_ACIO_GetNode(const void *state, int address)
{
    if (address < 1 || address > SDL_ACIO_GetNodeCount(state)) {
        return NULL;
    }
    return &ACIO_ConstBus(state)->versions[address - 1];
}

int SDL_ACIO_FindProduct(const void *state, const char *product)
{
    const int count = SDL_ACIO_GetNodeCount(state);
    int address, found = 0;

    if (!product) {
        return 0;
    }
    for (address = 1; address <= count; ++address) {
        if (strcmp(ACIO_ConstBus(state)->versions[address - 1].product, product) == 0) {
            found = address;
        }
    }
    return found;
}

int SDL_ACIO_FindType(const void *state, uint32_t type)
{
    const int count = SDL_ACIO_GetNodeCount(state);
    int address, found = 0;

    for (address = 1; address <= count; ++address) {
        if (ACIO_ConstBus(state)->versions[address - 1].type == type) {
            found = address;
        }
    }
    return found;
}

int SDL_ACIO_NextType(const void *state, uint32_t type, int after)
{
    const int count = SDL_ACIO_GetNodeCount(state);
    int address;

    for (address = 1; address <= count; ++address) {
        if (address > after && ACIO_ConstBus(state)->versions[address - 1].type == type) {
            return address;
        }
    }
    return 0;
}

void SDL_ACIO_SetAlternateRate(void *state, uint32_t rate)
{
    ACIO_Bus(state)->alternate_rate = rate;
}

void SDL_ACIO_ResetOnClose(void *state, bool on)
{
    ACIO_Base(state)->close_sequence = on;
}
