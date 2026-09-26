/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_konami_acio_proto.c, the Konami
   ACIO bus of hifihedgehog/SDL#33 Part 14. The vectors are the ticket's,
   constructed from bemanitools' aciodrv/device.c, and the version reply
   carries arcade-docs' BI2A entry. Test numbers follow the ticket's framing
   tests. A probe module runs the bus inside the real engine. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_konami_acio_proto.h"

/* The ticket's bytes */
static const uint8_t aa[1] = { 0xAA };
static const uint8_t enumerate_request[8] = { 0xAA, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x03 };
static const uint8_t enumerate_reply[8] = { 0xAA, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x04 };
static const uint8_t version_request[7] = { 0xAA, 0x01, 0x00, 0x02, 0x02, 0x00, 0x05 };
static const uint8_t version_reply[51] = {
    0xAA, 0x81, 0x00, 0x02, 0x02, 0x2C, 0x0D, 0x06, 0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x42, 0x49,
    0x32, 0x41, 0x4E, 0x6F, 0x76, 0x20, 0x32, 0x37, 0x20, 0x32, 0x30, 0x31, 0x37, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x31, 0x34, 0x3A, 0x34, 0x38, 0x3A, 0x35, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x18
};
static const uint8_t start_request[7] = { 0xAA, 0x01, 0x00, 0x03, 0x03, 0x00, 0x07 };
static const uint8_t start_reply[8] = { 0xAA, 0x81, 0x00, 0x03, 0x03, 0x01, 0x00, 0x88 };
static const uint8_t escape_aa[9] = { 0xAA, 0x01, 0x01, 0x00, 0x04, 0x01, 0xFF, 0x55, 0xB1 };
static const uint8_t escape_ff[9] = { 0xAA, 0x01, 0x01, 0x00, 0x04, 0x01, 0xFF, 0x00, 0x06 };
static const uint8_t escape_sum_aa[9] = { 0xAA, 0x01, 0x01, 0x00, 0x04, 0x01, 0xA3, 0xFF, 0x55 };
static const uint8_t escape_sum_ff[9] = { 0xAA, 0x01, 0x01, 0x00, 0x04, 0x01, 0xF8, 0xFF, 0x00 };

/* The probe module's request after Ready: 0110 to node 1 */
#define PROBE_COMMAND 0x0110
#define PROBE_RATE    57600

/* Test knobs, which survive the module's Reset */
static bool probe_present; /* Ready presents sub-device 0 */
static bool probe_auto;    /* Ready and every reply send the next request */
static bool probe_alternate; /* Reset names 115200 as the alternate rate */
static bool probe_close;     /* Ready asks for the reset on close */

typedef struct ProbeState
{
    SDL_ACIOState acio;
    int readies;
    int replies; /* Every Reply call */
    int failed;  /* Replies of NULL */
    int frames;  /* Frames that answered no exchange */
    int downs;
    uint8_t last_address;
    uint16_t last_command;
    uint8_t last_sequence;
    bool last_broadcast;
    size_t last_length;
    uint8_t last_payload[SDL_ACIO_MAX_PAYLOAD];
    bool timer;
    uint64_t timer_at;
    int ticks;
} ProbeState;

static ProbeState *State(Harness *h)
{
    return (ProbeState *)h->state;
}

static SDL_ACIOBus *Bus(Harness *h)
{
    return &State(h)->acio.bus;
}

static void Probe_Record(ProbeState *s, const SDL_ACIOFrame *frame)
{
    s->last_address = frame->address;
    s->last_command = frame->command;
    s->last_sequence = frame->sequence;
    s->last_broadcast = frame->broadcast;
    s->last_length = frame->length;
    memcpy(s->last_payload, frame->payload, frame->length);
}

static void Probe_Request(ProbeState *s)
{
    CHECK(SDL_ACIO_Request(s, 1, PROBE_COMMAND, NULL, 0, 1, 16));
}

static void Probe_Ready(void *state, uint64_t now)
{
    ProbeState *s = (ProbeState *)state;

    (void)now;
    ++s->readies;
    if (probe_close) {
        SDL_ACIO_ResetOnClose(s, true);
    }
    if (probe_present) {
        SDL_SerialIdentity identity;

        SDL_Serial_SetIdentity(&identity, "ACIO probe", SDL_SERIAL_TYPE_UNKNOWN, 1, 1, 0, 0);
        SDL_Serial_Present(&s->acio.base, 0, &identity);
    }
    if (probe_auto) {
        Probe_Request(s);
    }
}

static void Probe_Reply(void *state, const SDL_ACIOFrame *reply, uint64_t now)
{
    ProbeState *s = (ProbeState *)state;

    (void)now;
    ++s->replies;
    if (!reply) {
        ++s->failed;
    } else {
        Probe_Record(s, reply);
        if (probe_present) {
            SDL_SerialControls controls;

            memset(&controls, 0, sizeof(controls));
            controls.axes[0] = (int16_t)reply->payload[0];
            SDL_Serial_Commit(&s->acio.base, 0, &controls);
        }
    }
    if (probe_auto) {
        Probe_Request(s);
    }
}

static void Probe_Frame(void *state, const SDL_ACIOFrame *frame, uint64_t now)
{
    ProbeState *s = (ProbeState *)state;

    (void)now;
    ++s->frames;
    Probe_Record(s, frame);
}

static void Probe_Down(void *state, uint64_t now)
{
    ProbeState *s = (ProbeState *)state;

    (void)now;
    ++s->downs;
    SDL_Serial_AbsentAll(&s->acio.base);
}

static bool Probe_GetDeadline(void *state, uint64_t *deadline)
{
    ProbeState *s = (ProbeState *)state;

    if (!s->timer) {
        return false;
    }
    *deadline = s->timer_at;
    return true;
}

static void Probe_Tick(void *state, uint64_t now)
{
    ProbeState *s = (ProbeState *)state;

    if (s->timer && now >= s->timer_at) {
        s->timer = false;
        ++s->ticks;
    }
}

static const SDL_ACIOHandler probe_handler = {
    Probe_Ready, Probe_Reply, Probe_Frame, Probe_Down, Probe_GetDeadline, Probe_Tick
};

/* The same bus without the optional handlers */
static const SDL_ACIOHandler bare_handler = {
    Probe_Ready, Probe_Reply, NULL, NULL, NULL, NULL
};

static void Probe_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_ACIO_Reset(state, sizeof(ProbeState), sink, &probe_handler, PROBE_RATE, now);
    if (probe_alternate) {
        SDL_ACIO_SetAlternateRate(state, 115200);
    }
}

static void Bare_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_ACIO_Reset(state, sizeof(ProbeState), sink, &bare_handler, PROBE_RATE, now);
}

static void Probe_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

static const SDL_SerialModule probe_module = {
    "acioprobe", sizeof(ProbeState), false, 0, 0,
    Probe_Reset, SDL_ACIO_Feed, SDL_ACIO_Tick, SDL_Serial_NextAction, SDL_ACIO_ActionDone, Probe_Output, SDL_ACIO_GetDeadline, SDL_Serial_GetSnapshot
};

static const SDL_SerialModule bare_module = {
    "aciobare", sizeof(ProbeState), false, 0, 0,
    Bare_Reset, SDL_ACIO_Feed, SDL_ACIO_Tick, SDL_Serial_NextAction, SDL_ACIO_ActionDone, Probe_Output, SDL_ACIO_GetDeadline, SDL_Serial_GetSnapshot
};

/* An encoder written apart from the module's, from aciodrv/device.c */
static size_t Encode(uint8_t address, uint16_t command, uint8_t sequence, const uint8_t *payload, size_t length, uint8_t *out)
{
    uint8_t body[300];
    size_t n = 0, o = 0, i;
    uint8_t sum = 0;

    body[n++] = address;
    body[n++] = (uint8_t)(command >> 8);
    body[n++] = (uint8_t)(command & 0xFF);
    body[n++] = sequence;
    body[n++] = (uint8_t)length;
    if (length) {
        memcpy(body + n, payload, length);
    }
    n += length;
    for (i = 0; i < n; ++i) {
        sum = (uint8_t)(sum + body[i]);
    }
    body[n++] = sum;
    out[o++] = 0xAA;
    for (i = 0; i < n; ++i) {
        if (body[i] == 0xAA || body[i] == 0xFF) {
            out[o++] = 0xFF;
            out[o++] = (uint8_t)~body[i];
        } else {
            out[o++] = body[i];
        }
    }
    return o;
}

/* A version payload */
static void Version(uint8_t *payload, uint32_t type, const char *product, const char *date, const char *time)
{
    memset(payload, 0, SDL_ACIO_VERSION_LENGTH);
    payload[0] = (uint8_t)(type >> 24);
    payload[1] = (uint8_t)(type >> 16);
    payload[2] = (uint8_t)(type >> 8);
    payload[3] = (uint8_t)type;
    payload[5] = 1;
    payload[6] = 2;
    payload[7] = 1;
    memcpy(payload + 8, product, strlen(product));
    memcpy(payload + 12, date, strlen(date));
    memcpy(payload + 28, time, strlen(time));
}

/* Feeds one encoded frame */
static void FeedFrame(Harness *h, uint8_t address, uint16_t command, uint8_t sequence, const uint8_t *payload, size_t length)
{
    uint8_t frame[SDL_ACIO_MAX_FRAME];

    H_Feed(h, frame, Encode(address, command, sequence, payload, length, frame));
}

/* Runs the engine at the current time, after a test called the module */
static void Pump(Harness *h)
{
    H_Advance(h, h->now);
}

/* The 525 zero bytes and the break set, at time t */
static bool ExpectZeros(Harness *h, uint64_t t)
{
    static const uint8_t zeros[64] = { 0 };
    int i;

    for (i = 0; i < 8; ++i) {
        if (!H_IsWrite(H_NextCall(h), zeros, 64, t)) {
            return false;
        }
    }
    return H_IsWrite(H_NextCall(h), zeros, 13, t) && H_IsEscape(H_NextCall(h), SDL_SERIAL_SETBREAK, t);
}

/* The ticket's single BI2A node */
static void BringUp(Harness *h)
{
    const uint64_t t = h->now;

    H_Advance(h, t + 2650);
    H_Feed(h, aa, 1);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply, sizeof(version_reply));
    H_Feed(h, start_reply, sizeof(start_reply));
}

/* The bring-up replies in order, and a harness waiting for number step */
static const uint8_t *const bringup_replies[3] = { enumerate_reply, version_reply, start_reply };
static const size_t bringup_lengths[3] = { sizeof(enumerate_reply), sizeof(version_reply), sizeof(start_reply) };

static Harness *AtBringUp(int step)
{
    Harness *h;
    int i;

    probe_present = false;
    probe_auto = false;
    h = H_Create(&probe_module);
    H_Start(h);
    H_Advance(h, 2650);
    H_Feed(h, aa, 1);
    for (i = 0; i < step; ++i) {
        H_Feed(h, bringup_replies[i], bringup_lengths[i]);
    }
    H_SkipCalls(h);
    return h;
}

/* The reply of number step was taken: the next request went out, or Ready came */
static bool Advanced(Harness *h, int step)
{
    if (step == 0) {
        return H_IsWrite(H_NextCall(h), version_request, sizeof(version_request), 2650);
    }
    if (step == 1) {
        return H_IsWrite(H_NextCall(h), start_request, sizeof(start_request), 2650);
    }
    return State(h)->readies == 1 && Bus(h)->step == SDL_ACIO_STEP_READY;
}

static Harness *ReadyBus(const SDL_SerialModule *module, bool present, bool automatic)
{
    Harness *h;

    probe_present = present;
    probe_auto = automatic;
    h = H_Create(module);
    H_Start(h);
    BringUp(h);
    CHECK(Bus(h)->step == SDL_ACIO_STEP_READY && State(h)->readies == 1);
    H_SkipCalls(h);
    return h;
}

/* Test 1 to 4: the encoder */
static void TestEncode(void)
{
    uint8_t out[SDL_ACIO_MAX_FRAME + 8];
    uint8_t payload[SDL_ACIO_MAX_PAYLOAD + 1];
    uint8_t expected[SDL_ACIO_MAX_FRAME + 8];
    size_t n, size;
    const uint8_t zero = 0x00;
    const uint8_t count = 0x01;
    const uint8_t status = 0x00;
    uint8_t one;

    /* 1 */
    CHECK(SDL_ACIO_EncodeFrame(0x00, SDL_ACIO_CMD_ENUMERATE, 1, &zero, 1, out, sizeof(out)) == 8 && memcmp(out, enumerate_request, 8) == 0);
    CHECK(SDL_ACIO_EncodeFrame(0x00, SDL_ACIO_CMD_ENUMERATE, 1, &count, 1, out, sizeof(out)) == 8 && memcmp(out, enumerate_reply, 8) == 0);
    /* 2 */
    CHECK(SDL_ACIO_EncodeFrame(0x01, SDL_ACIO_CMD_GET_VERSION, 2, NULL, 0, out, sizeof(out)) == 7 && memcmp(out, version_request, 7) == 0);
    CHECK(SDL_ACIO_EncodeFrame(0x81, SDL_ACIO_CMD_GET_VERSION, 2, version_reply + 6, 44, out, sizeof(out)) == 51 && memcmp(out, version_reply, 51) == 0);
    /* 3 */
    CHECK(SDL_ACIO_EncodeFrame(0x01, SDL_ACIO_CMD_START, 3, NULL, 0, out, sizeof(out)) == 7 && memcmp(out, start_request, 7) == 0);
    CHECK(SDL_ACIO_EncodeFrame(0x81, SDL_ACIO_CMD_START, 3, &status, 1, out, sizeof(out)) == 8 && memcmp(out, start_reply, 8) == 0);
    /* 4: payload and checksum escapes */
    one = 0xAA;
    CHECK(SDL_ACIO_EncodeFrame(0x01, 0x0100, 4, &one, 1, out, sizeof(out)) == 9 && memcmp(out, escape_aa, 9) == 0);
    one = 0xFF;
    CHECK(SDL_ACIO_EncodeFrame(0x01, 0x0100, 4, &one, 1, out, sizeof(out)) == 9 && memcmp(out, escape_ff, 9) == 0);
    one = 0xA3;
    CHECK(SDL_ACIO_EncodeFrame(0x01, 0x0100, 4, &one, 1, out, sizeof(out)) == 9 && memcmp(out, escape_sum_aa, 9) == 0);
    one = 0xF8;
    CHECK(SDL_ACIO_EncodeFrame(0x01, 0x0100, 4, &one, 1, out, sizeof(out)) == 9 && memcmp(out, escape_sum_ff, 9) == 0);
    /* Header bytes escape too: address, both command bytes, sequence and length */
    one = 0x00;
    n = SDL_ACIO_EncodeFrame(0x7F, 0xAAFF, 0xAA, &one, 1, out, sizeof(out));
    CHECK(n == Encode(0x7F, 0xAAFF, 0xAA, &one, 1, expected) && memcmp(out, expected, n) == 0);
    CHECK(n == 11 && out[2] == 0xFF && out[3] == 0x55 && out[4] == 0xFF && out[5] == 0x00 && out[6] == 0xFF && out[7] == 0x55);
    memset(payload, 0xAA, sizeof(payload));
    n = SDL_ACIO_EncodeFrame(0x81, 0x0152, 5, payload, 0xAA, out, sizeof(out));
    CHECK(n == Encode(0x81, 0x0152, 5, payload, 0xAA, expected) && memcmp(out, expected, n) == 0);
    /* The longest frame fits SDL_ACIO_MAX_FRAME: 255 bytes of FF */
    memset(payload, 0xFF, sizeof(payload));
    n = SDL_ACIO_EncodeFrame(0xFF, 0xFFFF, 0xFF, payload, 255, out, sizeof(out));
    CHECK(n == Encode(0xFF, 0xFFFF, 0xFF, payload, 255, expected) && memcmp(out, expected, n) == 0);
    CHECK(n <= SDL_ACIO_MAX_FRAME && n > 500);
    /* Refused: 256 bytes, a missing payload, no buffer, the broadcast address */
    CHECK(SDL_ACIO_EncodeFrame(0x01, 0x0100, 4, payload, 256, out, sizeof(out)) == 0);
    CHECK(SDL_ACIO_EncodeFrame(0x01, 0x0100, 4, NULL, 1, out, sizeof(out)) == 0);
    CHECK(SDL_ACIO_EncodeFrame(0x01, 0x0100, 4, NULL, 0, NULL, sizeof(out)) == 0);
    CHECK(SDL_ACIO_EncodeFrame(SDL_ACIO_BROADCAST, 0x0100, 4, NULL, 0, out, sizeof(out)) == 0);
    CHECK(SDL_ACIO_EncodeFrame(0xF0, 0x0100, 4, NULL, 0, out, sizeof(out)) == 7);
    /* Every size short of the frame fails without writing past it */
    for (size = 0; size < 9; ++size) {
        memset(out, 0x5A, sizeof(out));
        one = 0xA3;
        CHECK(SDL_ACIO_EncodeFrame(0x01, 0x0100, 4, &one, 1, out, size) == 0);
        CHECK(out[size] == 0x5A);
    }
    CHECK(SDL_ACIO_EncodeFrame(0x01, 0x0100, 4, &one, 1, out, 9) == 9);
    for (size = 0; size < 9; ++size) {
        one = 0xAA;
        CHECK(SDL_ACIO_EncodeFrame(0x01, 0x0100, 4, &one, 1, out, size) == 0);
    }
    CHECK(SDL_ACIO_EncodeFrame(0x01, 0x0100, 4, &one, 1, out, 9) == 9);
}

/* Decodes a whole buffer. Returns the number of frames, the last in frame. */
static int DecodeAll(SDL_ACIODecoder *decoder, const uint8_t *data, size_t length, SDL_ACIOFrame *frame)
{
    int frames = 0;
    size_t i;

    for (i = 0; i < length; ++i) {
        if (SDL_ACIO_DecodeByte(decoder, data[i], frame)) {
            ++frames;
        }
    }
    return frames;
}

/* A decoded frame against its body */
static bool IsFrame(const SDL_ACIOFrame *frame, uint8_t address, uint16_t command, uint8_t sequence, const uint8_t *payload, size_t length)
{
    return !frame->broadcast && frame->address == address && frame->command == command && frame->sequence == sequence &&
           frame->length == length && (length == 0 || memcmp(frame->payload, payload, length) == 0);
}

/* Test 1 to 5: the decoder */
static void TestDecode(void)
{
    SDL_ACIODecoder decoder;
    SDL_ACIOFrame frame;
    SDL_ACIONodeVersion version;
    const uint8_t *vectors[6];
    size_t lengths[6], k, i;
    uint8_t buffer[128];
    int v;

    /* 1 to 3 */
    SDL_ACIO_InitDecoder(&decoder);
    CHECK(DecodeAll(&decoder, enumerate_reply, sizeof(enumerate_reply), &frame) == 1);
    CHECK(frame.address == 0x00 && frame.command == SDL_ACIO_CMD_ENUMERATE && frame.sequence == 1 && frame.length == 1 && frame.payload[0] == 1);
    CHECK(DecodeAll(&decoder, version_reply, sizeof(version_reply), &frame) == 1);
    CHECK(frame.address == 0x81 && frame.command == SDL_ACIO_CMD_GET_VERSION && frame.sequence == 2 && frame.length == 44);
    CHECK(SDL_ACIO_DecodeVersion(frame.payload, frame.length, &version));
    CHECK(version.type == SDL_ACIO_TYPE_BI2A && strcmp(version.product, "BI2A") == 0);
    CHECK(version.flag == 0 && version.major == 1 && version.minor == 2 && version.revision == 1);
    CHECK(strcmp(version.date, "Nov 27 2017") == 0 && strcmp(version.time, "14:48:52") == 0);
    CHECK(DecodeAll(&decoder, start_reply, sizeof(start_reply), &frame) == 1 && frame.command == SDL_ACIO_CMD_START && frame.payload[0] == 0);
    CHECK(DecodeAll(&decoder, enumerate_request, sizeof(enumerate_request), &frame) == 1 && frame.length == 1 && frame.payload[0] == 0);
    CHECK(DecodeAll(&decoder, version_request, sizeof(version_request), &frame) == 1 && frame.length == 0 && frame.address == 1);
    CHECK(decoder.bad_checksums == 0);

    /* 4: the original bodies come back */
    {
        static const uint8_t bodies[4] = { 0xAA, 0xFF, 0xA3, 0xF8 };

        vectors[0] = escape_aa;
        vectors[1] = escape_ff;
        vectors[2] = escape_sum_aa;
        vectors[3] = escape_sum_ff;
        for (v = 0; v < 4; ++v) {
            CHECK(DecodeAll(&decoder, vectors[v], 9, &frame) == 1 && IsFrame(&frame, 0x01, 0x0100, 4, &bodies[v], 1));
        }
    }
    /* Escaped header bytes */
    {
        const uint8_t zero = 0x00;
        const size_t n = Encode(0x7F, 0xAAFF, 0xAA, &zero, 1, buffer);

        CHECK(DecodeAll(&decoder, buffer, n, &frame) == 1 && IsFrame(&frame, 0x7F, 0xAAFF, 0xAA, &zero, 1));
    }

    /* 5: any number of AA bytes before a frame */
    for (k = 1; k <= 12; ++k) {
        memset(buffer, 0xAA, k);
        memcpy(buffer + k, start_reply, sizeof(start_reply));
        SDL_ACIO_InitDecoder(&decoder);
        CHECK(DecodeAll(&decoder, buffer, k + sizeof(start_reply), &frame) == 1 && frame.address == 0x81 && frame.command == SDL_ACIO_CMD_START);
    }
    /* Bytes outside a frame are ignored, FF too */
    {
        static const uint8_t junk[5] = { 0x00, 0xFF, 0x55, 0x81, 0x03 };

        SDL_ACIO_InitDecoder(&decoder);
        CHECK(DecodeAll(&decoder, junk, sizeof(junk), &frame) == 0);
        CHECK(DecodeAll(&decoder, start_reply, sizeof(start_reply), &frame) == 1);
        /* A whole body with no AA before it is no frame */
        SDL_ACIO_InitDecoder(&decoder);
        CHECK(DecodeAll(&decoder, enumerate_reply + 1, sizeof(enumerate_reply) - 1, &frame) == 0);
        /* Nor are bytes after a frame, before the next AA: the frame ends once */
        CHECK(DecodeAll(&decoder, start_reply, sizeof(start_reply), &frame) == 1);
        CHECK(DecodeAll(&decoder, enumerate_reply + 1, sizeof(enumerate_reply) - 1, &frame) == 0);
        CHECK(DecodeAll(&decoder, start_reply, sizeof(start_reply), &frame) == 1 && frame.command == SDL_ACIO_CMD_START);
    }
    /* A wrong checksum */
    memcpy(buffer, start_reply, sizeof(start_reply));
    buffer[7] = 0x89;
    SDL_ACIO_InitDecoder(&decoder);
    CHECK(DecodeAll(&decoder, buffer, sizeof(start_reply), &frame) == 0 && decoder.bad_checksums == 1);
    CHECK(DecodeAll(&decoder, start_reply, sizeof(start_reply), &frame) == 1);
    /* Every body byte off by one fails the checksum */
    for (i = 1; i < sizeof(version_reply) - 1; ++i) {
        memcpy(buffer, version_reply, sizeof(version_reply));
        if (i == 5) {
            continue; /* The length byte changes where the frame ends */
        }
        buffer[i] = (uint8_t)(buffer[i] + 1);
        if (buffer[i] == 0xAA || buffer[i] == 0xFF) {
            continue;
        }
        SDL_ACIO_InitDecoder(&decoder);
        CHECK(DecodeAll(&decoder, buffer, sizeof(version_reply), &frame) == 0);
    }

    /* Every truncation, the ones that end on a lone FF included, gives
       nothing, and the whole frame after it decodes */
    vectors[0] = version_reply;
    lengths[0] = sizeof(version_reply);
    vectors[1] = escape_aa;
    lengths[1] = 9;
    vectors[2] = escape_ff;
    lengths[2] = 9;
    vectors[3] = escape_sum_aa;
    lengths[3] = 9;
    vectors[4] = escape_sum_ff;
    lengths[4] = 9;
    vectors[5] = start_reply;
    lengths[5] = sizeof(start_reply);
    for (v = 0; v < 6; ++v) {
        for (k = 0; k < lengths[v]; ++k) {
            SDL_ACIO_InitDecoder(&decoder);
            CHECK(DecodeAll(&decoder, vectors[v], k, &frame) == 0);
            CHECK(DecodeAll(&decoder, vectors[v], lengths[v], &frame) == 1);
            CHECK(frame.command == vectors[v][2] * 256 + vectors[v][3] || v == 0);
        }
    }
    /* A frame split after its FF decodes */
    for (v = 1; v < 5; ++v) {
        const size_t split = (v <= 2) ? 7 : 8;

        SDL_ACIO_InitDecoder(&decoder);
        CHECK(vectors[v][split - 1] == 0xFF);
        CHECK(DecodeAll(&decoder, vectors[v], split, &frame) == 0 && decoder.escape);
        CHECK(DecodeAll(&decoder, vectors[v] + split, lengths[v] - split, &frame) == 1 && frame.length == 1);
    }
    /* After FF, AA starts a new frame, and FF FF carries 00 */
    SDL_ACIO_InitDecoder(&decoder);
    CHECK(DecodeAll(&decoder, escape_aa, 7, &frame) == 0);
    CHECK(DecodeAll(&decoder, start_reply, sizeof(start_reply), &frame) == 1 && frame.command == SDL_ACIO_CMD_START);
    {
        static const uint8_t ff_ff[9] = { 0xAA, 0x01, 0x01, 0x00, 0x04, 0x01, 0xFF, 0xFF, 0x07 };
        const uint8_t zero = 0x00;

        CHECK(DecodeAll(&decoder, ff_ff, sizeof(ff_ff), &frame) == 1 && IsFrame(&frame, 0x01, 0x0100, 4, &zero, 1));
    }
    /* A broadcast: address, length, payload, checksum */
    {
        static const uint8_t broadcast[6] = { 0xAA, 0x70, 0x02, 0x11, 0x22, 0xA5 };
        static const uint8_t empty[4] = { 0xAA, 0x70, 0x00, 0x70 };

        CHECK(DecodeAll(&decoder, broadcast, sizeof(broadcast), &frame) == 1);
        CHECK(frame.broadcast && frame.address == 0x70 && frame.length == 2 && frame.payload[0] == 0x11 && frame.payload[1] == 0x22);
        CHECK(frame.command == 0 && frame.sequence == 0);
        CHECK(DecodeAll(&decoder, empty, sizeof(empty), &frame) == 1 && frame.broadcast && frame.length == 0);
        CHECK(DecodeAll(&decoder, broadcast, 5, &frame) == 0);
        CHECK(DecodeAll(&decoder, broadcast, 6, &frame) == 1);
    }
    /* The longest frames stay inside the body */
    {
        uint8_t payload[255], big[SDL_ACIO_MAX_FRAME];
        size_t n;

        memset(payload, 0xAA, sizeof(payload));
        n = Encode(0x81, 0x0110, 7, payload, sizeof(payload), big);
        CHECK(DecodeAll(&decoder, big, n, &frame) == 1 && IsFrame(&frame, 0x81, 0x0110, 7, payload, 255));
        big[1] = 0x70;
        big[2] = 0xFF;
        big[3] = 0x00; /* A broadcast of 255 bytes */
        CHECK(DecodeAll(&decoder, big, 4, &frame) == 0 && decoder.length == 2);
        for (i = 0; i < 255; ++i) {
            CHECK(!SDL_ACIO_DecodeByte(&decoder, 0x01, &frame));
        }
        CHECK(decoder.length == 257);
        /* 70 + FF + 255 x 01 = 26E */
        CHECK(SDL_ACIO_DecodeByte(&decoder, 0x6E, &frame) && frame.broadcast && frame.length == 255);
    }
}

static void TestVersion(void)
{
    uint8_t payload[SDL_ACIO_VERSION_LENGTH + 1];
    SDL_ACIONodeVersion version;

    Version(payload, SDL_ACIO_TYPE_KFCA, "KFCA", "Aug 30 2011", "13:23:47");
    payload[4] = 0x5A;
    payload[5] = 0x10;
    payload[6] = 0x20;
    payload[7] = 0x30;
    CHECK(SDL_ACIO_DecodeVersion(payload, SDL_ACIO_VERSION_LENGTH, &version));
    CHECK(version.type == 0x09060000u && version.flag == 0x5A && version.major == 0x10 && version.minor == 0x20 && version.revision == 0x30);
    CHECK(strcmp(version.product, "KFCA") == 0 && strcmp(version.date, "Aug 30 2011") == 0 && strcmp(version.time, "13:23:47") == 0);
    /* Every type byte in its place */
    payload[0] = 0x12;
    payload[1] = 0x34;
    payload[2] = 0x56;
    payload[3] = 0x78;
    CHECK(SDL_ACIO_DecodeVersion(payload, SDL_ACIO_VERSION_LENGTH, &version) && version.type == 0x12345678u);
    /* 16 characters with no 00, and a byte out of range */
    memset(payload + 12, 'D', 16);
    memset(payload + 28, 'T', 16);
    payload[8] = 0x7F;
    payload[9] = 0x1F;
    payload[10] = 0x20;
    payload[11] = 0x7E;
    CHECK(SDL_ACIO_DecodeVersion(payload, SDL_ACIO_VERSION_LENGTH, &version));
    CHECK(strcmp(version.product, "?? ~") == 0);
    CHECK(strlen(version.date) == 16 && version.date[15] == 'D' && strlen(version.time) == 16 && version.time[0] == 'T');
    /* A product that stops early */
    payload[9] = 0x00;
    CHECK(SDL_ACIO_DecodeVersion(payload, SDL_ACIO_VERSION_LENGTH, &version) && strcmp(version.product, "?") == 0);
    CHECK(!SDL_ACIO_DecodeVersion(payload, SDL_ACIO_VERSION_LENGTH - 1, &version));
    CHECK(!SDL_ACIO_DecodeVersion(payload, SDL_ACIO_VERSION_LENGTH + 1, &version));
    CHECK(!SDL_ACIO_DecodeVersion(NULL, SDL_ACIO_VERSION_LENGTH, &version));
    CHECK(!SDL_ACIO_DecodeVersion(payload, SDL_ACIO_VERSION_LENGTH, NULL));
}

/* Test 6: the reset on the injected clock */
static void TestReset(void)
{
    Harness *h;
    int i;

    probe_present = false;
    probe_auto = false;
    h = H_Create(&probe_module);
    H_Start(h);
    CHECK(H_ExpectOpened(h, PROBE_RATE, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(ExpectZeros(h, 0));
    CHECK(H_NextCall(h) == NULL && Bus(h)->step == SDL_ACIO_STEP_BREAK);
    /* Bytes during the reset are dropped, an AA and a whole reply too */
    H_Advance(h, 700);
    H_Feed(h, aa, 1);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Advance(h, 1449);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1450);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 1450));
    CHECK(Bus(h)->step == SDL_ACIO_STEP_SETTLE && h->engine.breaking == false);
    H_Feed(h, aa, 1);
    H_Advance(h, 2649);
    H_Feed(h, aa, 1);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 2650);
    CHECK(H_IsWrite(H_NextCall(h), aa, 1, 2650) && Bus(h)->step == SDL_ACIO_STEP_PROBE);
    H_Advance(h, 2659);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 2660);
    CHECK(H_IsWrite(H_NextCall(h), aa, 1, 2660));
    /* Bytes other than AA do not answer a probe */
    {
        static const uint8_t other[4] = { 0x00, 0x55, 0xFF, 0x81 };
        H_Feed(h, other, sizeof(other));
        CHECK(H_NextCall(h) == NULL && Bus(h)->step == SDL_ACIO_STEP_PROBE);
    }
    /* An AA does, and what came with it is dropped */
    H_Advance(h, 2665);
    {
        uint8_t data[1 + sizeof(enumerate_reply)];

        data[0] = 0xAA;
        memcpy(data + 1, enumerate_reply, sizeof(enumerate_reply));
        H_Feed(h, data, sizeof(data));
    }
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, sizeof(enumerate_request), 2665));
    CHECK(H_NextCall(h) == NULL && Bus(h)->step == SDL_ACIO_STEP_ENUMERATE && Bus(h)->probes == 2);
    /* The probe timer stopped, and the reply is due 200 ms after the request */
    H_Advance(h, 2864);
    CHECK(H_NextCall(h) == NULL && Bus(h)->step == SDL_ACIO_STEP_ENUMERATE);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    CHECK(H_IsWrite(H_NextCall(h), version_request, sizeof(version_request), 2864));
    H_Destroy(h);

    /* 500 probes with no answer, then the bus starts over */
    h = H_Create(&probe_module);
    H_Start(h);
    H_SkipCalls(h);
    H_Advance(h, 2650 + 499 * SDL_ACIO_PROBE_MS);
    for (i = 0; i < SDL_ACIO_PROBES; ++i) {
        const H_Call *call = H_NextCall(h);

        if (i == 0) {
            CHECK(H_IsEscape(call, SDL_SERIAL_CLRBREAK, 1450));
            call = H_NextCall(h);
        }
        CHECK(H_IsWrite(call, aa, 1, 2650 + (uint64_t)i * SDL_ACIO_PROBE_MS));
    }
    CHECK(H_NextCall(h) == NULL && Bus(h)->probes == SDL_ACIO_PROBES && Bus(h)->restarts == 0);
    H_Advance(h, 2650 + 500 * SDL_ACIO_PROBE_MS - 1);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 2650 + 500 * SDL_ACIO_PROBE_MS);
    CHECK(ExpectZeros(h, 7650) && Bus(h)->restarts == 1 && Bus(h)->probes == 0 && State(h)->downs == 1);
    CHECK(strcmp(h->last_log, "ACIO bus: no answer to the reset, so the bus starts over") == 0);
    H_Destroy(h);

    /* An AA before the probe's write finishes: enumeration waits for it */
    h = H_Create(&probe_module);
    H_Start(h);
    H_Advance(h, 2649);
    H_SkipCalls(h);
    h->pend_writes = true;
    H_Advance(h, 2650);
    CHECK(H_IsWrite(H_NextCall(h), aa, 1, 2650));
    H_Feed(h, aa, 1);
    CHECK(H_NextCall(h) == NULL && Bus(h)->step == SDL_ACIO_STEP_ENUMERATE);
    H_Advance(h, 2652);
    H_CompleteWrite(h, true);
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, sizeof(enumerate_request), 2652));
    /* The probe's completion started nothing: the reply timer starts at the
       request's */
    H_Advance(h, 2700);
    H_CompleteWrite(h, true);
    H_Advance(h, 2899);
    CHECK(Bus(h)->step == SDL_ACIO_STEP_ENUMERATE && Bus(h)->restarts == 0);
    H_Advance(h, 2900);
    CHECK(Bus(h)->restarts == 1 && Bus(h)->step == SDL_ACIO_STEP_ZEROS);
    H_Destroy(h);

    /* Half a frame during the settle time is dropped: its end after the
       probe's answer completes nothing */
    h = H_Create(&probe_module);
    H_Start(h);
    H_Advance(h, 2000);
    H_Feed(h, enumerate_reply, 6);
    H_Advance(h, 2650);
    H_Feed(h, aa, 1);
    CHECK(Bus(h)->step == SDL_ACIO_STEP_ENUMERATE);
    H_Feed(h, enumerate_reply + 6, 2);
    CHECK(Bus(h)->step == SDL_ACIO_STEP_ENUMERATE && Bus(h)->pending && Bus(h)->decoder.bad_checksums == 0);
    H_Destroy(h);

    /* An AA after the probe's write finished: the enumeration stops the
       probe timer even while its own write is out */
    h = H_Create(&probe_module);
    H_Start(h);
    H_Advance(h, 2650);
    CHECK(Bus(h)->timer && Bus(h)->deadline == 2660);
    h->pend_writes = true;
    H_Advance(h, 2655);
    H_Feed(h, aa, 1);
    CHECK(Bus(h)->step == SDL_ACIO_STEP_ENUMERATE && !Bus(h)->timer);
    H_Advance(h, 2700);
    CHECK(Bus(h)->step == SDL_ACIO_STEP_ENUMERATE && Bus(h)->restarts == 0 && Bus(h)->probes == 1);
    H_CompleteWrite(h, true);
    CHECK(Bus(h)->timer && Bus(h)->deadline == 2700 + SDL_ACIO_REPLY_MS);
    h->pend_writes = false;
    H_Destroy(h);
}

/* Test 1 to 3 as one bring-up */
static void TestBringUp(void)
{
    Harness *h;
    const SDL_ACIONodeVersion *node;
    uint8_t payload[SDL_ACIO_VERSION_LENGTH];
    int i;

    probe_present = false;
    probe_auto = false;
    h = H_Create(&probe_module);
    H_Start(h);
    H_Advance(h, 2650);
    H_SkipCalls(h);
    H_Feed(h, aa, 1);
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, 8, 2650));
    CHECK(SDL_ACIO_GetNodeCount(h->state) == 0);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    CHECK(H_IsWrite(H_NextCall(h), version_request, 7, 2650) && Bus(h)->nodes == 1);
    CHECK(SDL_ACIO_GetNodeCount(h->state) == 0 && SDL_ACIO_GetNode(h->state, 1) == NULL);
    H_Feed(h, version_reply, sizeof(version_reply));
    CHECK(H_IsWrite(H_NextCall(h), start_request, 7, 2650));
    CHECK(strcmp(h->last_log, "ACIO node 1: BI2A, type 0D060000, version 1.2.1, Nov 27 2017 14:48:52") == 0);
    CHECK(State(h)->readies == 0 && Bus(h)->step == SDL_ACIO_STEP_START);
    H_Feed(h, start_reply, sizeof(start_reply));
    CHECK(H_NextCall(h) == NULL && State(h)->readies == 1 && Bus(h)->step == SDL_ACIO_STEP_READY);
    CHECK(SDL_ACIO_GetNodeCount(h->state) == 1 && Bus(h)->sequence == 4);
    node = SDL_ACIO_GetNode(h->state, 1);
    CHECK(node && node->type == SDL_ACIO_TYPE_BI2A && strcmp(node->product, "BI2A") == 0 && node->major == 1 && node->minor == 2 && node->revision == 1);
    CHECK(SDL_ACIO_GetNode(h->state, 0) == NULL && SDL_ACIO_GetNode(h->state, 2) == NULL);
    CHECK(SDL_ACIO_FindProduct(h->state, "BI2A") == 1 && SDL_ACIO_FindProduct(h->state, "KFCA") == 0);
    CHECK(SDL_ACIO_FindProduct(h->state, "BI2") == 0 && SDL_ACIO_FindProduct(h->state, NULL) == 0);
    CHECK(h->npresence == 0 && State(h)->downs == 0);
    H_Destroy(h);

    /* Every truncation of the bring-up replies, with stale bytes past the
       count, sends nothing, and the whole reply after it goes on. Every
       split of them, one byte at a time too, decodes as one read does. */
    {
        int step;

        for (step = 0; step < 3; ++step) {
            const uint8_t *reply = bringup_replies[step];
            const size_t length = bringup_lengths[step];
            Harness *t;
            size_t k;

            for (k = 0; k < length; ++k) {
                t = AtBringUp(step);
                H_FeedPadded(t, reply, k);
                CHECK(H_NextCall(t) == NULL && Bus(t)->pending && State(t)->readies == 0);
                H_Feed(t, reply, length);
                CHECK(Advanced(t, step));
                H_Destroy(t);
            }
            for (k = 1; k < length; ++k) {
                t = AtBringUp(step);
                H_Feed(t, reply, k);
                H_Feed(t, reply + k, length - k);
                CHECK(Advanced(t, step));
                H_Destroy(t);
            }
            t = AtBringUp(step);
            for (k = 0; k < length; ++k) {
                H_Feed(t, reply + k, 1);
            }
            CHECK(Advanced(t, step));
            H_Destroy(t);
        }
    }

    /* An enumeration reply from 80, and replies without bit 7, are taken */
    h = H_Create(&probe_module);
    H_Start(h);
    H_Advance(h, 2650);
    H_Feed(h, aa, 1);
    payload[0] = 1;
    FeedFrame(h, 0x80, SDL_ACIO_CMD_ENUMERATE, 1, payload, 1);
    CHECK(Bus(h)->step == SDL_ACIO_STEP_VERSION);
    Version(payload, SDL_ACIO_TYPE_BI2A, "BI2A", "", "");
    FeedFrame(h, 0x01, SDL_ACIO_CMD_GET_VERSION, 2, payload, SDL_ACIO_VERSION_LENGTH);
    CHECK(Bus(h)->step == SDL_ACIO_STEP_START);
    FeedFrame(h, 0x01, SDL_ACIO_CMD_START, 3, NULL, 0);
    CHECK(Bus(h)->step == SDL_ACIO_STEP_READY);
    H_Destroy(h);

    /* 16 nodes: every version, then every start, in address order */
    {
        static const char *const products[16] = {
            "BI2A", "KFCA", "BI2A", "ICCA", "KFCA", "PANB", "MDXF", "MDXF",
            "ICCA", "BI2A", "KFCA", "KFCA", "PANB", "RVOL", "BI2A", "ICCA"
        };
        uint8_t expected[16];

        h = H_Create(&probe_module);
        H_Start(h);
        H_Advance(h, 2650);
        H_SkipCalls(h);
        H_Feed(h, aa, 1);
        CHECK(H_NextCall(h) != NULL);
        payload[0] = 16;
        FeedFrame(h, 0x00, SDL_ACIO_CMD_ENUMERATE, 1, payload, 1);
        for (i = 0; i < 16; ++i) {
            CHECK(H_IsWrite(H_NextCall(h), expected, Encode((uint8_t)(i + 1), SDL_ACIO_CMD_GET_VERSION, (uint8_t)(2 + i), NULL, 0, expected), 2650));
            /* A reply from another node is not this one's */
            Version(payload, SDL_ACIO_TYPE_BI2A, products[i], "", "");
            FeedFrame(h, (uint8_t)(0x81 + ((i + 1) % 16)), SDL_ACIO_CMD_GET_VERSION, 0, payload, SDL_ACIO_VERSION_LENGTH);
            CHECK(Bus(h)->node == i && Bus(h)->restarts == 0);
            FeedFrame(h, (uint8_t)(0x81 + i), SDL_ACIO_CMD_GET_VERSION, (uint8_t)(2 + i), payload, SDL_ACIO_VERSION_LENGTH);
        }
        for (i = 0; i < 16; ++i) {
            CHECK(H_IsWrite(H_NextCall(h), expected, Encode((uint8_t)(i + 1), SDL_ACIO_CMD_START, (uint8_t)(18 + i), NULL, 0, expected), 2650));
            CHECK(State(h)->readies == 0);
            FeedFrame(h, (uint8_t)(0x81 + i), SDL_ACIO_CMD_START, (uint8_t)(18 + i), NULL, 0);
        }
        CHECK(State(h)->readies == 1 && SDL_ACIO_GetNodeCount(h->state) == 16);
        /* The highest match, as bemanitools' iidxio takes the last */
        CHECK(SDL_ACIO_FindProduct(h->state, "BI2A") == 15 && SDL_ACIO_FindProduct(h->state, "RVOL") == 14);
        CHECK(SDL_ACIO_FindProduct(h->state, "ICCA") == 16 && SDL_ACIO_FindProduct(h->state, "PANB") == 13);
        CHECK(strcmp(SDL_ACIO_GetNode(h->state, 7)->product, "MDXF") == 0 && strcmp(SDL_ACIO_GetNode(h->state, 16)->product, "ICCA") == 0);
        H_Destroy(h);
    }
}

/* The bus started over: with writes that finish at once, the zeros are out
   and the break holds */
static void ExpectStartsOver(Harness *h, const char *log)
{
    CHECK(Bus(h)->restarts == 1 && Bus(h)->step == SDL_ACIO_STEP_BREAK && State(h)->downs == 1);
    CHECK(ExpectZeros(h, h->now));
    CHECK(strcmp(h->last_log, log) == 0);
}

static void TestBringUpFailures(void)
{
    static const char *failed = "ACIO bus: a node failed its start-up, so the bus starts over";
    static const char *count_log = "ACIO bus: no node, or more than 16, so the bus starts over";
    uint8_t payload[SDL_ACIO_VERSION_LENGTH + 1];
    Harness *h;
    int step;

    for (step = 0; step < 3; ++step) {
        /* No reply in 200 ms */
        h = AtBringUp(step);
        H_Advance(h, 2849);
        CHECK(Bus(h)->restarts == 0);
        H_Advance(h, 2850);
        ExpectStartsOver(h, failed);
        H_Destroy(h);

        /* A reply with another command */
        h = AtBringUp(step);
        FeedFrame(h, (step == 0) ? 0x00 : 0x81, 0x0004, 1, payload, 1);
        ExpectStartsOver(h, failed);
        H_Destroy(h);

        /* Frames from elsewhere, and broadcasts, change nothing */
        {
            static const uint8_t broadcast[4] = { 0xAA, 0x70, 0x00, 0x70 };

            h = AtBringUp(step);
            FeedFrame(h, 0x85, 0x0004, 1, payload, 1);
            H_Feed(h, broadcast, sizeof(broadcast));
            CHECK(Bus(h)->restarts == 0 && Bus(h)->pending && State(h)->frames == 0);
            H_Destroy(h);
        }
    }

    /* Enumeration: no node, 17 nodes, a count in 0 or 2 bytes */
    h = AtBringUp(0);
    payload[0] = 0;
    FeedFrame(h, 0x00, SDL_ACIO_CMD_ENUMERATE, 1, payload, 1);
    ExpectStartsOver(h, count_log);
    H_Destroy(h);
    h = AtBringUp(0);
    payload[0] = 17;
    FeedFrame(h, 0x00, SDL_ACIO_CMD_ENUMERATE, 1, payload, 1);
    ExpectStartsOver(h, count_log);
    H_Destroy(h);
    h = AtBringUp(0);
    FeedFrame(h, 0x00, SDL_ACIO_CMD_ENUMERATE, 1, payload, 0);
    ExpectStartsOver(h, failed);
    H_Destroy(h);
    h = AtBringUp(0);
    payload[0] = 1;
    payload[1] = 0;
    FeedFrame(h, 0x00, SDL_ACIO_CMD_ENUMERATE, 1, payload, 2);
    ExpectStartsOver(h, failed);
    H_Destroy(h);

    /* Version: 43 and 45 bytes */
    Version(payload, SDL_ACIO_TYPE_BI2A, "BI2A", "", "");
    h = AtBringUp(1);
    FeedFrame(h, 0x81, SDL_ACIO_CMD_GET_VERSION, 2, payload, SDL_ACIO_VERSION_LENGTH - 1);
    ExpectStartsOver(h, failed);
    H_Destroy(h);
    h = AtBringUp(1);
    FeedFrame(h, 0x81, SDL_ACIO_CMD_GET_VERSION, 2, payload, SDL_ACIO_VERSION_LENGTH + 1);
    ExpectStartsOver(h, failed);
    H_Destroy(h);

    /* Start: any status length */
    {
        static const size_t lengths[3] = { 0, 1, 255 };
        uint8_t status[255];
        int i;

        memset(status, 0x33, sizeof(status));
        for (i = 0; i < 3; ++i) {
            h = AtBringUp(2);
            FeedFrame(h, 0x81, SDL_ACIO_CMD_START, 3, status, lengths[i]);
            CHECK(Bus(h)->step == SDL_ACIO_STEP_READY && State(h)->readies == 1);
            H_Destroy(h);
        }
    }

    /* A request that does not finish leaving fails at once */
    h = AtBringUp(0);
    h->pend_writes = true;
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    CHECK(H_IsWrite(H_NextCall(h), version_request, sizeof(version_request), 2650));
    H_Advance(h, 2700);
    h->pend_writes = false;
    H_CompleteWrite(h, false);
    ExpectStartsOver(h, failed);
    H_Destroy(h);

    /* The bare handler: no Down, and bring-up still starts over */
    probe_present = false;
    h = H_Create(&bare_module);
    H_Start(h);
    H_Advance(h, 2650);
    H_Feed(h, aa, 1);
    H_SkipCalls(h);
    H_Advance(h, 2850);
    CHECK(Bus(h)->restarts == 1 && State(h)->downs == 0 && ExpectZeros(h, 2850));
    H_Destroy(h);
}

/* The module's exchanges */
static void TestExchanges(void)
{
    uint8_t frame[SDL_ACIO_MAX_FRAME], payload[SDL_ACIO_MAX_PAYLOAD];
    Harness *h;
    size_t n;
    int i;

    /* Nothing before Ready */
    probe_present = false;
    probe_auto = false;
    h = H_Create(&probe_module);
    H_Start(h);
    CHECK(!SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 0, 1));
    CHECK(!SDL_ACIO_Send(h->state, 1, PROBE_COMMAND, NULL, 0));
    H_Advance(h, 2650);
    H_Feed(h, aa, 1);
    CHECK(!SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 0, 1));
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply, sizeof(version_reply));
    CHECK(!SDL_ACIO_Send(h->state, 1, PROBE_COMMAND, NULL, 0));
    H_Feed(h, start_reply, sizeof(start_reply));
    H_SkipCalls(h);

    /* A request, sequence 4, and its reply */
    CHECK(!SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 2, 1));
    CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
    CHECK(!SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
    Pump(h);
    n = Encode(0x01, PROBE_COMMAND, 4, NULL, 0, frame);
    CHECK(H_IsWrite(H_NextCall(h), frame, n, 2650));
    payload[0] = 0x42;
    FeedFrame(h, 0x81, PROBE_COMMAND, 4, payload, 1);
    CHECK(State(h)->replies == 1 && State(h)->failed == 0 && State(h)->last_payload[0] == 0x42 && State(h)->last_address == 0x81);
    CHECK(!Bus(h)->pending && !Bus(h)->timer);

    /* No reply: NULL 200 ms after the request finished leaving */
    h->pend_writes = true;
    CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
    Pump(h);
    H_Advance(h, 3000);
    CHECK(!Bus(h)->timer);
    H_CompleteWrite(h, true);
    H_Advance(h, 3199);
    CHECK(State(h)->replies == 1);
    H_Advance(h, 3200);
    CHECK(State(h)->replies == 2 && State(h)->failed == 1 && !Bus(h)->pending);
    h->pend_writes = false;

    /* Another command from the node fails the exchange at once. Payload
       lengths out of range fail it too. */
    {
        static const size_t lengths[2] = { 0, 17 };

        CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
        Pump(h);
        payload[0] = 0;
        FeedFrame(h, 0x81, PROBE_COMMAND + 1, 5, payload, 1);
        CHECK(State(h)->failed == 2 && !Bus(h)->pending);
        for (i = 0; i < 2; ++i) {
            CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
            Pump(h);
            memset(payload, 0, sizeof(payload));
            FeedFrame(h, 0x81, PROBE_COMMAND, 6, payload, lengths[i]);
            CHECK(State(h)->failed == 3 + i);
        }
        /* The bounds themselves are taken */
        CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
        Pump(h);
        FeedFrame(h, 0x81, PROBE_COMMAND, 6, payload, 16);
        CHECK(State(h)->failed == 4 && State(h)->last_length == 16);
        CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
        Pump(h);
        FeedFrame(h, 0x81, PROBE_COMMAND, 6, payload, 1);
        CHECK(State(h)->failed == 4 && State(h)->last_length == 1);
    }

    /* Frames from elsewhere and broadcasts answer no exchange: they go to
       Frame and the exchange goes on */
    {
        static const uint8_t broadcast[6] = { 0xAA, 0x70, 0x02, 0x11, 0x22, 0xA5 };
        const int replies = State(h)->replies;

        CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
        Pump(h);
        payload[0] = 0x07;
        FeedFrame(h, 0x82, PROBE_COMMAND, 9, payload, 1);
        CHECK(State(h)->frames == 1 && State(h)->last_address == 0x82 && Bus(h)->pending && State(h)->replies == replies);
        H_Feed(h, broadcast, sizeof(broadcast));
        CHECK(State(h)->frames == 2 && State(h)->last_broadcast && State(h)->last_length == 2 && Bus(h)->pending);
        /* A reply and a stream frame in one read, in order */
        {
            uint8_t both[32];
            size_t m;

            payload[0] = 0x09;
            m = Encode(0x81, PROBE_COMMAND, 9, payload, 1, both);
            payload[0] = 0x0A;
            m += Encode(0x83, 0x0110, 10, payload, 1, both + m);
            H_Feed(h, both, m);
            CHECK(State(h)->replies == replies + 1 && State(h)->frames == 3 && State(h)->last_payload[0] == 0x0A);
        }
        /* With no exchange out, every frame goes to Frame */
        payload[0] = 0x0B;
        FeedFrame(h, 0x81, PROBE_COMMAND, 11, payload, 1);
        CHECK(State(h)->frames == 4 && State(h)->replies == replies + 1);
    }

    /* A reply before the request finished leaving: its completion then
       starts no timer */
    h->pend_writes = true;
    CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
    Pump(h);
    payload[0] = 0x0C;
    FeedFrame(h, 0x81, PROBE_COMMAND, 12, payload, 1);
    CHECK(!Bus(h)->pending && State(h)->last_payload[0] == 0x0C);
    H_CompleteWrite(h, true);
    CHECK(!Bus(h)->timer);
    {
        const int failed = State(h)->failed;

        H_Advance(h, h->now + 1000);
        CHECK(State(h)->failed == failed);
    }
    /* A request that does not finish leaving fails */
    CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
    Pump(h);
    {
        const int failed = State(h)->failed;

        H_CompleteWrite(h, false);
        CHECK(State(h)->failed == failed + 1 && !Bus(h)->pending);
    }
    h->pend_writes = false;
    H_Destroy(h);

    /* Sequence numbers climb per frame, AA and FF escaped, and wrap past 255 to 0 */
    h = ReadyBus(&probe_module, false, false);
    for (i = 4; i < 260; ++i) {
        CHECK(SDL_ACIO_Send(h->state, 2, 0x0115, NULL, 0));
        Pump(h);
        n = Encode(0x02, 0x0115, (uint8_t)i, NULL, 0, frame);
        CHECK(H_IsWrite(H_NextCall(h), frame, n, h->now));
    }
    CHECK(Bus(h)->sequence == 4 && !Bus(h)->pending);
    /* A send answers nothing and waits for nothing */
    payload[0] = 0x0D;
    FeedFrame(h, 0x82, 0x0115, 4, payload, 1);
    CHECK(State(h)->frames == 1 && State(h)->replies == 0);
    /* A send while a reply is due leaves the reply timer alone */
    CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
    Pump(h);
    CHECK(Bus(h)->timer && Bus(h)->deadline == 2650 + SDL_ACIO_REPLY_MS);
    H_Advance(h, 2700);
    CHECK(SDL_ACIO_Send(h->state, 2, 0x0115, NULL, 0));
    Pump(h);
    CHECK(Bus(h)->deadline == 2650 + SDL_ACIO_REPLY_MS);
    H_Advance(h, 2650 + SDL_ACIO_REPLY_MS);
    CHECK(State(h)->failed == 1);
    H_Destroy(h);

    /* A long frame goes out as writes of 64 bytes, and the reply timer
       starts when the last one finishes */
    h = ReadyBus(&probe_module, false, false);
    h->pend_writes = true;
    memset(payload, 0xAA, 100);
    CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, payload, 100, 0, 1));
    Pump(h);
    n = Encode(0x01, PROBE_COMMAND, 4, payload, 100, frame);
    CHECK(n == 1 + 5 + 200 + 1);
    CHECK(H_IsWrite(H_NextCall(h), frame, 64, 2650));
    H_Advance(h, 2651);
    H_CompleteWrite(h, true);
    CHECK(H_IsWrite(H_NextCall(h), frame + 64, 64, 2651) && !Bus(h)->timer);
    H_Advance(h, 2652);
    H_CompleteWrite(h, true);
    CHECK(H_IsWrite(H_NextCall(h), frame + 128, 64, 2652));
    H_Advance(h, 2653);
    H_CompleteWrite(h, true);
    CHECK(H_IsWrite(H_NextCall(h), frame + 192, n - 192, 2653) && !Bus(h)->timer);
    H_Advance(h, 2654);
    H_CompleteWrite(h, true);
    CHECK(Bus(h)->timer && Bus(h)->deadline == 2654 + SDL_ACIO_REPLY_MS);
    h->pend_writes = false;
    H_Destroy(h);

    /* A full queue refuses a frame whole */
    h = ReadyBus(&probe_module, false, false);
    h->pend_writes = true;
    memset(payload, 0xFF, sizeof(payload));
    CHECK(SDL_ACIO_Send(h->state, 1, 0xFFFF, payload, 255));
    Pump(h);
    CHECK(State(h)->acio.base.action_count == 8);
    CHECK(!SDL_ACIO_Send(h->state, 1, 0xFFFF, payload, 255));
    CHECK(State(h)->acio.base.action_count == 8 && Bus(h)->sequence == 5);
    /* 237 bytes of FF make a frame of 481 bytes, 8 writes */
    CHECK(SDL_ACIO_Send(h->state, 1, 0x0115, payload, 237));
    CHECK(State(h)->acio.base.action_count == 16);
    CHECK(!SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 0, 1) && !Bus(h)->pending);
    CHECK(!SDL_ACIO_Send(h->state, 1, PROBE_COMMAND, payload, 256));
    h->pend_writes = false;
    H_Destroy(h);
}

/* Restart, and test 7: reconnect */
static void TestRestart(void)
{
    Harness *h = ReadyBus(&probe_module, true, true);

    CHECK(h->presence[0] == 1 && Bus(h)->pending);
    /* A restart drops the joystick and resets the bus */
    SDL_ACIO_Restart(h->state, h->now);
    Pump(h);
    CHECK(State(h)->downs == 1 && h->presence[0] == 0 && ExpectZeros(h, 2650));
    CHECK(Bus(h)->restarts == 1 && Bus(h)->sequence == 1 && !Bus(h)->pending);
    /* A restart during the reset does nothing */
    SDL_ACIO_Restart(h->state, h->now);
    H_Advance(h, 3000);
    SDL_ACIO_Restart(h->state, h->now);
    H_Advance(h, 4100);
    SDL_ACIO_Restart(h->state, h->now);
    CHECK(State(h)->downs == 1 && Bus(h)->restarts == 1);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 4100) && H_NextCall(h) == NULL);
    /* The bus comes back from its restart with the sequence from 1 */
    H_Advance(h, 5300);
    CHECK(H_IsWrite(H_NextCall(h), aa, 1, 5300));
    H_Feed(h, aa, 1);
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, 8, 5300));
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply, sizeof(version_reply));
    H_Feed(h, start_reply, sizeof(start_reply));
    CHECK(State(h)->readies == 2 && h->presence[0] == 2);
    H_Destroy(h);

    /* A restart while probing starts over */
    probe_present = false;
    probe_auto = false;
    h = H_Create(&probe_module);
    H_Start(h);
    H_Advance(h, 2650);
    H_SkipCalls(h);
    CHECK(Bus(h)->step == SDL_ACIO_STEP_PROBE);
    SDL_ACIO_Restart(h->state, h->now);
    Pump(h);
    CHECK(Bus(h)->restarts == 1 && State(h)->downs == 1 && ExpectZeros(h, 2650));
    H_Destroy(h);

    /* A restart drops the rest of a frame still queued: the zeros go next */
    h = ReadyBus(&probe_module, false, false);
    h->pend_writes = true;
    {
        uint8_t payload[100];
        static const uint8_t zeros[64] = { 0 };

        memset(payload, 0xAA, sizeof(payload));
        CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, payload, sizeof(payload), 0, 1));
        Pump(h);
        CHECK(H_NextCall(h) != NULL && State(h)->acio.base.action_count == 3);
        SDL_ACIO_Restart(h->state, h->now);
        Pump(h);
        CHECK(H_NextCall(h) == NULL && State(h)->acio.base.action_count == 9);
        H_Advance(h, 2651);
        H_CompleteWrite(h, true);
        CHECK(H_IsWrite(H_NextCall(h), zeros, 64, 2651) && Bus(h)->step == SDL_ACIO_STEP_ZEROS);
    }
    h->pend_writes = false;
    H_Destroy(h);

    /* 7: the port closes, the next open repeats the reset and enumeration,
       and the sequence restarts at 1 */
    h = ReadyBus(&probe_module, true, true);
    H_Advance(h, 2700);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 2700));
    H_Advance(h, 3699);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 3700);
    CHECK(H_ExpectOpened(h, PROBE_RATE, 8, SDL_SERIAL_NOPARITY, 1, 3700) && ExpectZeros(h, 3700));
    CHECK(State(h)->readies == 0 && Bus(h)->restarts == 0);
    BringUp(h);
    CHECK(State(h)->readies == 1 && h->presence[0] == 2);
    H_Destroy(h);

    /* A loss during the break ends the break before the port closes */
    probe_present = false;
    probe_auto = false;
    h = H_Create(&probe_module);
    H_Start(h);
    H_SkipCalls(h);
    H_Advance(h, 500);
    CHECK(h->engine.breaking);
    H_LosePort(h);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 500) && H_IsCall(H_NextCall(h), 'C', 500));
    CHECK(!h->engine.breaking);
    H_Advance(h, 1500);
    CHECK(H_ExpectOpened(h, PROBE_RATE, 8, SDL_SERIAL_NOPARITY, 1, 1500) && ExpectZeros(h, 1500));
    H_Destroy(h);
}

/* The module's timer and pulses run through the bus */
static void TestDeadlines(void)
{
    Harness *h = ReadyBus(&probe_module, true, false);
    uint64_t deadline;

    CHECK(!SDL_ACIO_GetDeadline(h->state, &deadline));
    State(h)->timer = true;
    State(h)->timer_at = 3000;
    CHECK(SDL_ACIO_GetDeadline(h->state, &deadline) && deadline == 3000);
    H_Advance(h, 2999);
    CHECK(State(h)->ticks == 0);
    H_Advance(h, 3000);
    CHECK(State(h)->ticks == 1);
    /* The earliest of the module's, the bus's and a pulse's */
    State(h)->timer = true;
    State(h)->timer_at = 3500;
    CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
    Pump(h);
    CHECK(SDL_ACIO_GetDeadline(h->state, &deadline) && deadline == 3200);
    SDL_Serial_Pulse(&State(h)->acio.base, 0, 0, 3000);
    CHECK(SDL_ACIO_GetDeadline(h->state, &deadline) && deadline == 3100);
    H_Advance(h, 3099);
    CHECK(H_Button(h, 0, 0));
    H_Advance(h, 3100);
    CHECK(!H_Button(h, 0, 0) && State(h)->failed == 0);
    H_Advance(h, 3200);
    CHECK(State(h)->failed == 1);
    CHECK(SDL_ACIO_GetDeadline(h->state, &deadline) && deadline == 3500);
    /* A tick 1 ms before the reply is due leaves the exchange out */
    H_Advance(h, 3500);
    CHECK(SDL_ACIO_Request(h->state, 1, PROBE_COMMAND, NULL, 0, 1, 16));
    Pump(h);
    State(h)->timer = true;
    State(h)->timer_at = 3500 + SDL_ACIO_REPLY_MS - 1;
    H_Advance(h, 3500 + SDL_ACIO_REPLY_MS - 1);
    CHECK(State(h)->ticks == 3 && State(h)->failed == 1 && Bus(h)->pending);
    H_Advance(h, 3500 + SDL_ACIO_REPLY_MS);
    CHECK(State(h)->failed == 2);
    H_Destroy(h);

    /* The bare handler has no timer */
    h = ReadyBus(&bare_module, false, false);
    CHECK(!SDL_ACIO_GetDeadline(h->state, &deadline));
    H_Advance(h, 10000);
    CHECK(Bus(h)->step == SDL_ACIO_STEP_READY);
    /* And no Frame handler: a stray frame is dropped */
    {
        uint8_t payload[1] = { 0x01 };
        FeedFrame(h, 0x81, PROBE_COMMAND, 4, payload, 1);
        CHECK(State(h)->frames == 0 && State(h)->replies == 0);
    }
    H_Destroy(h);
}

/* Test 5 through the engine: truncation, stale bytes, splits and malformed
   replies around a pending exchange */
static void BringUpBattery(Harness *h)
{
    BringUp(h);
}

static void TestBatteryA(void)
{
    uint8_t r5[16], r_aa[16], r_sum_ff[16], r_extra[24], bad_sum[16], bad_command[16], cut_ff[16], r0[16];
    size_t n5, n_aa, n_sum_ff, n_extra, n_bad_sum, n_bad_command, n_cut_ff, n0;
    uint8_t payload[4];
    H_Vector vectors[8];
    Harness *h;
    int i, count = 0;

    payload[0] = 0x05;
    n5 = Encode(0x81, PROBE_COMMAND, 4, payload, 1, r5);
    payload[0] = 0xAA;
    n_aa = Encode(0x81, PROBE_COMMAND, 4, payload, 1, r_aa);
    /* 81 + 01 + 10 + 04 + 01 + 68 = FF */
    payload[0] = 0x68;
    n_sum_ff = Encode(0x81, PROBE_COMMAND, 4, payload, 1, r_sum_ff);
    CHECK(r_sum_ff[n_sum_ff - 2] == 0xFF && r_sum_ff[n_sum_ff - 1] == 0x00);
    memset(r_extra, 0xAA, 3);
    payload[0] = 0x06;
    n_extra = 3 + Encode(0x81, PROBE_COMMAND, 4, payload, 1, r_extra + 3);
    payload[0] = 0x00;
    n0 = Encode(0x81, PROBE_COMMAND, 4, payload, 1, r0);
    payload[0] = 0x05;
    n_bad_sum = Encode(0x81, PROBE_COMMAND, 4, payload, 1, bad_sum);
    bad_sum[n_bad_sum - 1] ^= 0x01;
    n_bad_command = Encode(0x81, PROBE_COMMAND + 1, 4, payload, 1, bad_command);
    payload[0] = 0xFF;
    n_cut_ff = Encode(0x81, PROBE_COMMAND, 4, payload, 1, cut_ff);
    CHECK(cut_ff[6] == 0xFF);
    n_cut_ff = 7; /* Cut after the escape */

    h = ReadyBus(&probe_module, true, true);
    H_SetVector(&vectors[count++], "reply 05", r5, n5, 1, 0);
    H_SetVector(&vectors[count++], "reply AA escaped", r_aa, n_aa, 1, 0);
    H_SetVector(&vectors[count++], "checksum FF escaped", r_sum_ff, n_sum_ff, 1, 0);
    H_SetVector(&vectors[count++], "AA AA AA before a reply", r_extra, n_extra, 1, 0);
    H_SetVector(&vectors[count++], "reply 00", r0, n0, 0, 0);
    for (i = 0; i < count; ++i) {
        H_BatteryA(h, &vectors[i], BringUpBattery);
    }
    CHECK(State(h)->failed == 0);
    /* A6 */
    H_BatteryA6(h, "wrong checksum", bad_sum, n_bad_sum, &vectors[0]);
    H_BatteryA6(h, "another command", bad_command, n_bad_command, &vectors[0]);
    H_BatteryA6(h, "cut after FF", cut_ff, n_cut_ff, &vectors[0]);
    H_BatteryA6(h, "cut after FF, AA reply", cut_ff, n_cut_ff, &vectors[1]);
    for (i = 1; i < (int)n5; ++i) {
        H_BatteryA6(h, "every truncation", r5, (size_t)i, &vectors[0]);
    }
    H_Destroy(h);

    /* The other command failed the exchange and the next request took the
       valid reply */
    h = ReadyBus(&probe_module, true, true);
    H_Feed(h, bad_command, n_bad_command);
    CHECK(State(h)->failed == 1 && Bus(h)->pending);
    H_Feed(h, r5, n5);
    CHECK(H_Axis(h, 0, 0) == 5 && State(h)->failed == 1);
    H_Destroy(h);
}

/* From an open or a restart, up to Ready, with nodes of the given types and
   products */
static void BringUpTypes(Harness *h, const uint32_t *types, const char *const *products, int count)
{
    uint8_t payload[SDL_ACIO_VERSION_LENGTH];
    uint8_t seq = 1;
    int i;

    H_Advance(h, h->now + 2650);
    H_Feed(h, aa, 1);
    payload[0] = (uint8_t)count;
    FeedFrame(h, 0x00, SDL_ACIO_CMD_ENUMERATE, seq++, payload, 1);
    for (i = 0; i < count; ++i) {
        Version(payload, types[i], products[i], "", "");
        FeedFrame(h, (uint8_t)(0x81 + i), SDL_ACIO_CMD_GET_VERSION, seq++, payload, SDL_ACIO_VERSION_LENGTH);
    }
    for (i = 0; i < count; ++i) {
        payload[0] = 0x00;
        FeedFrame(h, (uint8_t)(0x81 + i), SDL_ACIO_CMD_START, seq++, payload, 1);
    }
}

/* Part 14's RS-232 boards: nodes by type. The RVOL's product code reads
   KFCA, so only the type tells it from a KFCA. */
static void TestTypes(void)
{
    static const uint32_t types[5] = { SDL_ACIO_TYPE_KFCA, SDL_ACIO_TYPE_RVOL, SDL_ACIO_TYPE_KFCA, SDL_ACIO_TYPE_MDXF, SDL_ACIO_TYPE_PANB };
    static const char *const products[5] = { "KFCA", "KFCA", "KFCA", "MDXF", "PANB" };
    Harness *h;

    probe_present = false;
    probe_auto = false;
    h = H_Create(&probe_module);
    H_Start(h);
    CHECK(SDL_ACIO_FindType(h->state, SDL_ACIO_TYPE_KFCA) == 0 && SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_KFCA, 0) == 0);
    BringUpTypes(h, types, products, 5);
    CHECK(State(h)->readies == 1 && SDL_ACIO_GetNodeCount(h->state) == 5);
    CHECK(SDL_ACIO_GetNode(h->state, 2)->type == SDL_ACIO_TYPE_RVOL && strcmp(SDL_ACIO_GetNode(h->state, 2)->product, "KFCA") == 0);
    /* The highest of a type */
    CHECK(SDL_ACIO_FindType(h->state, SDL_ACIO_TYPE_KFCA) == 3 && SDL_ACIO_FindType(h->state, SDL_ACIO_TYPE_RVOL) == 2);
    CHECK(SDL_ACIO_FindType(h->state, SDL_ACIO_TYPE_MDXF) == 4 && SDL_ACIO_FindType(h->state, SDL_ACIO_TYPE_PANB) == 5);
    CHECK(SDL_ACIO_FindType(h->state, SDL_ACIO_TYPE_BI2A) == 0 && SDL_ACIO_FindType(h->state, SDL_ACIO_TYPE_ICCA) == 0);
    CHECK(SDL_ACIO_FindProduct(h->state, "KFCA") == 3);
    /* The next of a type, from the lowest */
    CHECK(SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_KFCA, 0) == 1 && SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_KFCA, 1) == 3);
    CHECK(SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_KFCA, 2) == 3 && SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_KFCA, 3) == 0);
    CHECK(SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_KFCA, -7) == 1 && SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_RVOL, 1) == 2);
    CHECK(SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_RVOL, 2) == 0 && SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_PANB, 4) == 5);
    CHECK(SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_PANB, 5) == 0 && SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_PANB, 100) == 0);
    CHECK(SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_MDXF, 0) == 4 && SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_ICCA, 0) == 0);
    /* The sixteenth node is found too */
    H_Destroy(h);
    {
        uint32_t many[16];
        const char *names[16];
        int i;

        for (i = 0; i < 16; ++i) {
            many[i] = SDL_ACIO_TYPE_ICCA;
            names[i] = "ICCA";
        }
        many[15] = SDL_ACIO_TYPE_PANB;
        h = H_Create(&probe_module);
        H_Start(h);
        BringUpTypes(h, many, names, 16);
        CHECK(SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_PANB, 0) == 16 && SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_PANB, 15) == 16);
        CHECK(SDL_ACIO_FindType(h->state, SDL_ACIO_TYPE_PANB) == 16 && SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_PANB, 16) == 0);
        CHECK(SDL_ACIO_FindType(h->state, SDL_ACIO_TYPE_ICCA) == 15 && SDL_ACIO_NextType(h->state, SDL_ACIO_TYPE_ICCA, 14) == 15);
        H_Destroy(h);
    }
}

/* A board whose rate no source records: each failed bring-up switches */
static void TestAlternateRate(void)
{
    static const char *const log_up = "ACIO bus: nothing came up at 57600 baud, so the bus tries 115200";
    static const char *const log_down = "ACIO bus: nothing came up at 115200 baud, so the bus tries 57600";
    Harness *h;

    probe_present = false;
    probe_auto = false;
    probe_alternate = true;
    h = H_Create(&probe_module);
    H_Start(h);
    CHECK(H_ExpectOpened(h, PROBE_RATE, 8, SDL_SERIAL_NOPARITY, 1, 0) && ExpectZeros(h, 0));
    CHECK(Bus(h)->rate == 57600 && Bus(h)->alternate_rate == 115200);
    /* 500 probes go unanswered: the line switches before the zeros */
    H_Advance(h, 2650 + 500 * SDL_ACIO_PROBE_MS - 1);
    H_SkipCalls(h);
    H_Advance(h, 2650 + 500 * SDL_ACIO_PROBE_MS);
    CHECK(H_IsPlainLine(H_NextCall(h), 115200, 8, SDL_SERIAL_NOPARITY, 1, 7650) && ExpectZeros(h, 7650));
    CHECK(Bus(h)->rate == 115200 && Bus(h)->alternate_rate == 57600 && Bus(h)->restarts == 1 && State(h)->downs == 1);
    CHECK(strcmp(h->last_log, log_up) == 0 && h->engine.line.rate == 115200);
    /* An answered probe and no enumeration: back to 57600 */
    H_Advance(h, 7650 + 2650);
    H_Feed(h, aa, 1);
    H_SkipCalls(h);
    H_Advance(h, 7650 + 2650 + SDL_ACIO_REPLY_MS);
    CHECK(H_IsPlainLine(H_NextCall(h), 57600, 8, SDL_SERIAL_NOPARITY, 1, 10500) && ExpectZeros(h, 10500));
    CHECK(Bus(h)->rate == 57600 && Bus(h)->restarts == 2 && strcmp(h->last_log, log_down) == 0);
    /* A bring-up that comes up keeps its rate: a restart after Ready sends
       only the zeros */
    BringUp(h);
    CHECK(State(h)->readies == 1 && Bus(h)->step == SDL_ACIO_STEP_READY);
    H_SkipCalls(h);
    SDL_ACIO_Restart(h->state, h->now);
    Pump(h);
    CHECK(ExpectZeros(h, h->now) && Bus(h)->rate == 57600 && Bus(h)->alternate_rate == 115200 && Bus(h)->restarts == 3);
    CHECK(h->engine.line.rate == 57600);
    /* A reopened port starts at the first rate again */
    H_Advance(h, h->now + 3000);
    H_Feed(h, aa, 1);
    H_Advance(h, h->now + SDL_ACIO_REPLY_MS);
    CHECK(Bus(h)->rate == 115200);
    H_LosePort(h);
    H_SkipCalls(h);
    H_Advance(h, h->now + SDL_SERIAL_RETRY_MS);
    CHECK(H_ExpectOpened(h, PROBE_RATE, 8, SDL_SERIAL_NOPARITY, 1, h->now) && Bus(h)->rate == 57600 && Bus(h)->restarts == 0);
    H_Destroy(h);
    probe_alternate = false;

    /* Without an alternate rate the line stays */
    h = H_Create(&probe_module);
    H_Start(h);
    H_SkipCalls(h);
    H_Advance(h, 2650 + 500 * SDL_ACIO_PROBE_MS - 1);
    H_SkipCalls(h);
    H_Advance(h, 2650 + 500 * SDL_ACIO_PROBE_MS);
    CHECK(ExpectZeros(h, 7650) && Bus(h)->rate == 57600 && Bus(h)->alternate_rate == 0);
    H_Destroy(h);
}

/* A streaming node asks for a reset before its port closes */
static void TestCloseReset(void)
{
    static const char *const log_close = "ACIO bus: the port closes, so the bus resets the nodes first";
    uint8_t payload[1] = { 0x42 };
    uint64_t deadline;
    Harness *h;
    int i, logs;

    /* Ready: the reset, then the port closes when the break ends */
    probe_close = true;
    probe_alternate = true;
    h = ReadyBus(&probe_module, true, true);
    CHECK(h->presence[0] == 1 && Bus(h)->pending && State(h)->acio.base.close_sequence);
    State(h)->timer = true;
    State(h)->timer_at = 3000;
    H_Advance(h, 2700);
    logs = h->nlogs;
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(2700)));
    CHECK(ExpectZeros(h, 2700) && H_NextCall(h) == NULL);
    CHECK(h->presence[0] == 0 && State(h)->downs == 1 && Bus(h)->closing && Bus(h)->step == SDL_ACIO_STEP_BREAK);
    CHECK(Bus(h)->rate == 57600 && Bus(h)->restarts == 1 && strcmp(h->last_log, log_close) == 0);
    /* The module's timer waits, frames are dropped, and a restart does nothing */
    CHECK(SDL_ACIO_GetDeadline(h->state, &deadline) && deadline == 2700 + SDL_ACIO_BREAK_MS);
    FeedFrame(h, 0x81, PROBE_COMMAND, 4, payload, 1);
    H_Feed(h, aa, 1);
    SDL_ACIO_Restart(h->state, h->now);
    H_Advance(h, 2700 + SDL_ACIO_BREAK_MS - 1);
    CHECK(H_NextCall(h) == NULL && State(h)->ticks == 0 && State(h)->replies == 0 && State(h)->frames == 0 && h->engine.open);
    CHECK(State(h)->downs == 1 && Bus(h)->restarts == 1);
    H_Advance(h, 2700 + SDL_ACIO_BREAK_MS);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 4150) && H_IsCall(H_NextCall(h), 'C', 4150) && H_NextCall(h) == NULL);
    CHECK(!h->engine.open && State(h)->acio.base.closed && Bus(h)->step == SDL_ACIO_STEP_BREAK_CLEAR && !Bus(h)->timer);
    CHECK(h->nlogs == logs + 1 && State(h)->ticks == 0);
    H_Destroy(h);
    probe_alternate = false;

    /* A reset under way ends the close with its own break */
    h = ReadyBus(&probe_module, true, true);
    SDL_ACIO_Restart(h->state, h->now);
    Pump(h);
    CHECK(ExpectZeros(h, 2650) && State(h)->downs == 1);
    H_Advance(h, 3000);
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(3000)) && H_NextCall(h) == NULL);
    CHECK(State(h)->downs == 1 && Bus(h)->restarts == 1 && Bus(h)->closing);
    H_Advance(h, 2650 + SDL_ACIO_BREAK_MS);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 4100) && H_IsCall(H_NextCall(h), 'C', 4100));
    H_Destroy(h);

    /* So does one whose zeros are still leaving */
    h = ReadyBus(&probe_module, false, false);
    h->pend_writes = true;
    SDL_ACIO_Restart(h->state, h->now);
    Pump(h);
    CHECK(H_NextCall(h) != NULL && Bus(h)->step == SDL_ACIO_STEP_ZEROS);
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(2650)) && H_NextCall(h) == NULL && Bus(h)->restarts == 1);
    for (i = 0; i < 9; ++i) {
        H_CompleteWrite(h, true);
    }
    CHECK(Bus(h)->step == SDL_ACIO_STEP_BREAK && H_NextCall(h) != NULL);
    h->pend_writes = false;
    H_Advance(h, 2650 + SDL_ACIO_BREAK_MS);
    H_SkipCalls(h);
    CHECK(!h->engine.open && State(h)->acio.base.closed);
    H_Destroy(h);

    /* During the settle time the nodes get a fresh reset */
    h = ReadyBus(&probe_module, false, false);
    SDL_ACIO_Restart(h->state, h->now);
    Pump(h);
    H_Advance(h, 2650 + SDL_ACIO_BREAK_MS + 50);
    CHECK(Bus(h)->step == SDL_ACIO_STEP_SETTLE);
    H_SkipCalls(h);
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(4150)));
    CHECK(ExpectZeros(h, 4150) && Bus(h)->restarts == 2 && State(h)->downs == 2);
    H_Advance(h, 4150 + SDL_ACIO_BREAK_MS);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 5600) && H_IsCall(H_NextCall(h), 'C', 5600));
    H_Destroy(h);

    /* So does a bus still probing, and a close is no failed bring-up: the
       rate stays */
    probe_present = false;
    probe_auto = false;
    probe_alternate = true;
    h = H_Create(&probe_module);
    H_Start(h);
    H_Advance(h, 2700);
    SDL_ACIO_ResetOnClose(h->state, true);
    H_SkipCalls(h);
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(2700)) && ExpectZeros(h, 2700));
    CHECK(Bus(h)->rate == 57600 && Bus(h)->alternate_rate == 115200 && strcmp(h->last_log, log_close) == 0);
    H_Advance(h, 2700 + SDL_ACIO_BREAK_MS);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 4150) && H_IsCall(H_NextCall(h), 'C', 4150));
    H_Destroy(h);
    probe_alternate = false;
    probe_close = false;

    /* Without the reset on close, or after it is taken back, the port closes
       at once */
    for (i = 0; i < 2; ++i) {
        h = ReadyBus(&probe_module, true, true);
        if (i == 1) {
            SDL_ACIO_ResetOnClose(h->state, true);
            SDL_ACIO_ResetOnClose(h->state, false);
        }
        CHECK(!State(h)->acio.base.close_sequence);
        CHECK(!SDL_SerialEngine_BeginStop(&h->engine, H_NS(2650)));
        CHECK(H_IsCall(H_NextCall(h), 'C', 2650) && H_NextCall(h) == NULL && State(h)->downs == 0 && !Bus(h)->closing);
        H_Destroy(h);
    }
}

int main(void)
{
    TestEncode();
    TestDecode();
    TestVersion();
    TestReset();
    TestBringUp();
    TestBringUpFailures();
    TestExchanges();
    TestRestart();
    TestDeadlines();
    TestBatteryA();
    TestTypes();
    TestAlternateRate();
    TestCloseReset();
    return H_Finish();
}
