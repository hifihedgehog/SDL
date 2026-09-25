/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/dji/SDL_dji_tcp_proto.c, the DJI screen
   remotes over TCP of hifihedgehog/SDL#33 Part 6: the links, keepalives,
   acknowledgements, liveness, the joystick and the host hint. The session
   runs on an injected clock and every action it asks for is logged. Test
   numbers follow the part's TCP section. */

#include "testserialharness.h"
#include "../src/joystick/dji/SDL_dji_tcp_proto.h"

#define T_MAX_LOG       4096
#define T_MAX_SNAPSHOTS 1024

typedef struct T_Action
{
    SDL_DJITCPAction action;
    uint64_t time;
} T_Action;

typedef struct TCPHarness
{
    SDL_DJITCPState state;
    uint64_t now;
    T_Action log[T_MAX_LOG];
    int nlog;
    int cursor;
    SDL_SerialSnapshot snapshots[T_MAX_SNAPSHOTS];
    int nsnapshots;
} TCPHarness;

static void T_Changed(void *userdata, int sub, const SDL_SerialSnapshot *snapshot)
{
    TCPHarness *t = (TCPHarness *)userdata;

    CHECK(sub == 0);
    if (t->nsnapshots < T_MAX_SNAPSHOTS) {
        t->snapshots[t->nsnapshots++] = *snapshot;
    }
}

static void T_Pump(TCPHarness *t)
{
    SDL_DJITCPAction action;

    while (SDL_DJITCP_NextAction(&t->state, &action)) {
        if (t->nlog < T_MAX_LOG) {
            t->log[t->nlog].action = action;
            t->log[t->nlog].time = t->now;
            ++t->nlog;
        }
    }
}

/* Runs the session at every deadline up to and including to, then at to */
static void T_Advance(TCPHarness *t, uint64_t to)
{
    int guard = 0;

    for (;;) {
        uint64_t deadline;

        if (!SDL_DJITCP_GetDeadline(&t->state, &deadline) || deadline > to) {
            break;
        }
        if (deadline > t->now) {
            t->now = deadline;
        }
        SDL_DJITCP_Tick(&t->state, t->now);
        T_Pump(t);
        if (++guard > 100000) {
            H_Fail("deadline loop");
            break;
        }
    }
    if (to > t->now) {
        t->now = to;
    }
    SDL_DJITCP_Tick(&t->state, t->now);
    T_Pump(t);
}

static TCPHarness *T_Create(void)
{
    TCPHarness *t = (TCPHarness *)calloc(1, sizeof(TCPHarness));
    SDL_SerialSink sink;

    sink.userdata = t;
    sink.changed = T_Changed;
    sink.log = NULL;
    SDL_DJITCP_Init(&t->state, &sink, 0);
    /* The driver's thread runs the session at once, which connects link 0 */
    T_Advance(t, 0);
    return t;
}

static void T_Connected(TCPHarness *t, int link, bool success)
{
    SDL_DJITCP_Connected(&t->state, link, success, t->now);
    T_Pump(t);
}

static void T_Receive(TCPHarness *t, int link, const uint8_t *data, size_t length)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);

    memcpy(copy, data, length);
    SDL_DJITCP_Received(&t->state, link, copy, length, t->now);
    free(copy);
    T_Pump(t);
}

static void T_Lost(TCPHarness *t, int link)
{
    SDL_DJITCP_Lost(&t->state, link, t->now);
    T_Pump(t);
}

static const T_Action *T_Next(TCPHarness *t)
{
    if (t->cursor >= t->nlog) {
        return NULL;
    }
    return &t->log[t->cursor++];
}

static void T_Skip(TCPHarness *t)
{
    t->cursor = t->nlog;
}

static bool T_Is(const T_Action *a, SDL_DJITCPActionKind kind, int link, uint64_t time)
{
    return a && a->action.kind == kind && a->action.link == link && a->time == time;
}

static bool T_IsSend(const T_Action *a, int link, const uint8_t *data, size_t length, uint64_t time)
{
    return T_Is(a, SDL_DJI_TCP_SEND, link, time) && a->action.length == length && memcmp(a->action.data, data, length) == 0;
}

/* A keepalive with this sequence, in its envelope */
static size_t Keepalive(uint8_t *out, uint16_t sequence)
{
    uint8_t frame[16];
    const size_t length = SDL_DJI_BuildFrame(frame, sizeof(frame), 0x02, 0x06, sequence, 0x00, 0x00, 0x01, NULL, 0);

    return SDL_DJI_BuildEnvelope(out, 64, frame, length);
}

static bool T_IsKeepalive(const T_Action *a, int link, uint16_t sequence, uint64_t time)
{
    uint8_t expected[64];
    const size_t length = Keepalive(expected, sequence);

    return T_IsSend(a, link, expected, length, time);
}

/* A frame in its envelope */
static size_t Envelope(uint8_t *out, uint8_t sender, uint8_t receiver, uint16_t sequence, uint8_t type, uint8_t set, uint8_t id,
                       const uint8_t *payload, size_t payload_length)
{
    uint8_t frame[SDL_DJI_MAX_FRAME];
    const size_t length = SDL_DJI_BuildFrame(frame, sizeof(frame), sender, receiver, sequence, type, set, id, payload, payload_length);

    return SDL_DJI_BuildEnvelope(out, SDL_DJI_ENVELOPE_LENGTH + SDL_DJI_MAX_FRAME, frame, length);
}

/* A 06/AE frame with the buttons word, the mode word and six values */
static size_t Controls(uint8_t *out, uint16_t buttons, uint16_t mode, const uint16_t *values)
{
    uint8_t payload[17];
    int i;

    memset(payload, 0, sizeof(payload));
    payload[0] = (uint8_t)(buttons & 0xFF);
    payload[1] = (uint8_t)(buttons >> 8);
    payload[2] = (uint8_t)(mode & 0xFF);
    payload[3] = (uint8_t)(mode >> 8);
    for (i = 0; i < 6; ++i) {
        payload[5 + 2 * i] = (uint8_t)(values[i] & 0xFF);
        payload[6 + 2 * i] = (uint8_t)(values[i] >> 8);
    }
    return Envelope(out, 0x06, 0x02, 1, 0x00, 0x06, 0xAE, payload, sizeof(payload));
}

static size_t RestControls(uint8_t *out)
{
    static const uint16_t rest[6] = { 1024, 1024, 1024, 1024, 1024, 1024 };

    return Controls(out, 0, 1, rest);
}

static size_t Beacon(uint8_t *out, const char *model, uint8_t type)
{
    uint8_t payload[66];

    memset(payload, 0, sizeof(payload));
    memcpy(payload, model, strlen(model));
    return Envelope(out, 0x06, 0x02, 0x10, type, 0x00, 0x81, payload, sizeof(payload));
}

static const SDL_SerialSnapshot *T_Snapshot(TCPHarness *t)
{
    return SDL_Serial_GetSnapshot(&t->state, 0);
}

static bool T_Axes(TCPHarness *t, int lx, int ly, int rx, int ry, int dial, int dial2)
{
    const SDL_SerialControls *c = &T_Snapshot(t)->controls;

    return c->axes[0] == lx && c->axes[1] == ly && c->axes[2] == rx && c->axes[3] == ry && c->axes[4] == dial && c->axes[5] == dial2;
}

static bool T_Buttons(TCPHarness *t, const int *buttons)
{
    SDL_SerialControls expected;
    int i;

    memset(&expected, 0, sizeof(expected));
    for (i = 0; buttons[i] >= 0; ++i) {
        SDL_Serial_SetButton(&expected, buttons[i], true);
    }
    return memcmp(expected.buttons, T_Snapshot(t)->controls.buttons, sizeof(expected.buttons)) == 0;
}

/* A session with link 0 up at time 0 and its first keepalive taken */
static TCPHarness *T_Up(void)
{
    TCPHarness *t = T_Create();

    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 0, 0));
    T_Connected(t, 0, true);
    CHECK(T_IsKeepalive(T_Next(t), 0, 1, 0));
    return t;
}

/* The ticket's test 1 envelope */
static const uint8_t test1[] = {
    0x55, 0xCC, 0x30, 0x75, 0x1E, 0x00, 0x00, 0x00, 0x55, 0x1E, 0x04, 0x8A, 0x06, 0x02, 0x01, 0x00, 0x00, 0x06, 0xAE,
    0x20, 0x00, 0x05, 0x00, 0x00, 0x94, 0x06, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x9A, 0x13
};

static void TestControls(void)
{
    TCPHarness *t = T_Up();
    uint8_t buffer[128];
    static const uint16_t right[6] = { 1684, 1024, 1024, 1024, 1024, 1024 };
    int list[4];

    /* 1 */
    CHECK(Controls(buffer, 0x0020, 0x0005, right) == sizeof(test1) && memcmp(buffer, test1, sizeof(test1)) == 0);
    t->now = 10;
    T_Receive(t, 0, test1, sizeof(test1));
    CHECK(t->state.live && T_Snapshot(t)->present);
    CHECK(strcmp(T_Snapshot(t)->identity.name, "DJI Remote") == 0);
    CHECK(T_Snapshot(t)->identity.naxes == 6 && T_Snapshot(t)->identity.nbuttons == 12);
    CHECK(T_Axes(t, 0, 0, 32767, 0, 0, 0));
    list[0] = SDL_DJI_BUTTON_HOME;
    list[1] = SDL_DJI_BUTTON_C1;
    list[2] = SDL_DJI_BUTTON_MODE + 1;
    list[3] = -1;
    CHECK(T_Buttons(t, list));
    {
        const SDL_SerialGamepadMap *m = &T_Snapshot(t)->identity.mapping;

        CHECK(T_Snapshot(t)->identity.has_mapping && T_Snapshot(t)->identity.type == SDL_SERIAL_TYPE_GAMEPAD);
        CHECK(m->a.kind == SDL_SERIAL_MAP_BUTTON && m->a.target == SDL_DJI_BUTTON_SHUTTER);
        CHECK(m->x.kind == SDL_SERIAL_MAP_BUTTON && m->x.target == SDL_DJI_BUTTON_RECORD);
        CHECK(m->leftshoulder.target == SDL_DJI_BUTTON_C1 && m->rightshoulder.target == SDL_DJI_BUTTON_C2);
        CHECK(m->guide.kind == SDL_SERIAL_MAP_BUTTON && m->guide.target == SDL_DJI_BUTTON_HOME);
        CHECK(m->y.kind == SDL_SERIAL_MAP_AXIS_POSITIVE && m->y.target == 4);
        CHECK(m->b.kind == SDL_SERIAL_MAP_AXIS_NEGATIVE && m->b.target == 4);
        CHECK(m->leftx.kind == SDL_SERIAL_MAP_AXIS && m->leftx.target == 0 && m->righty.target == 3);
        CHECK(m->back.kind == SDL_SERIAL_MAP_NONE);
    }
    free(t);
}

/* 6: each axis, each button mask and each mode alone */
static void TestDecode(void)
{
    static const int offsets[6] = { 5, 7, 9, 11, 13, 15 };
    static const int axes[6] = { SDL_DJI_AXIS_RIGHT_X, SDL_DJI_AXIS_RIGHT_Y, SDL_DJI_AXIS_LEFT_Y, SDL_DJI_AXIS_LEFT_X, SDL_DJI_AXIS_DIAL, SDL_DJI_AXIS_DIAL2 };
    static const uint16_t raws[3] = { 364, 1024, 1684 };
    static const int scaled[3] = { -32767, 0, 32767 };
    static const struct
    {
        uint16_t buttons;
        uint16_t mode;
        int button;
    } masks[6] = {
        { 0x0020, 3, SDL_DJI_BUTTON_HOME },
        { 0x0080, 3, SDL_DJI_BUTTON_SHUTTER_HALF },
        { 0x0040, 3, SDL_DJI_BUTTON_SHUTTER },
        { 0x0100, 3, SDL_DJI_BUTTON_RECORD },
        { 0x0000, 7, SDL_DJI_BUTTON_C1 },
        { 0x0000, 11, SDL_DJI_BUTTON_C2 },
    };
    uint8_t payload[17];
    SDL_SerialControls controls;
    int i, j, k;

    for (i = 0; i < 6; ++i) {
        for (j = 0; j < 3; ++j) {
            memset(payload, 0, sizeof(payload));
            for (k = 0; k < 6; ++k) {
                payload[offsets[k]] = 0x00;
                payload[offsets[k] + 1] = 0x04;
            }
            payload[offsets[i]] = (uint8_t)(raws[j] & 0xFF);
            payload[offsets[i] + 1] = (uint8_t)(raws[j] >> 8);
            memset(&controls, 0, sizeof(controls));
            CHECK(SDL_DJITCP_DecodeControls(payload, sizeof(payload), &controls));
            for (k = 0; k < 6; ++k) {
                /* The vertical axes are negated, as on the other transports */
                const bool vertical = (axes[k] == SDL_DJI_AXIS_LEFT_Y || axes[k] == SDL_DJI_AXIS_RIGHT_Y);
                const int expected = (k == i) ? (vertical ? -scaled[j] : scaled[j]) : 0;

                CHECK(controls.axes[axes[k]] == expected);
            }
        }
    }
    for (i = 0; i < 6; ++i) {
        memset(payload, 0, sizeof(payload));
        payload[0] = (uint8_t)(masks[i].buttons & 0xFF);
        payload[1] = (uint8_t)(masks[i].buttons >> 8);
        payload[2] = (uint8_t)masks[i].mode;
        memset(&controls, 0, sizeof(controls));
        CHECK(SDL_DJITCP_DecodeControls(payload, sizeof(payload), &controls));
        for (k = 0; k < SDL_DJI_TCP_BUTTONS; ++k) {
            CHECK(SDL_Serial_GetButton(&controls, k) == (k == masks[i].button));
        }
    }
    /* The camera at full press sets both of its bits */
    memset(payload, 0, sizeof(payload));
    payload[0] = 0xC0;
    payload[2] = 3;
    memset(&controls, 0, sizeof(controls));
    SDL_DJITCP_DecodeControls(payload, sizeof(payload), &controls);
    CHECK(SDL_Serial_GetButton(&controls, SDL_DJI_BUTTON_SHUTTER) && SDL_Serial_GetButton(&controls, SDL_DJI_BUTTON_SHUTTER_HALF));
    /* Modes 0, 1 and 2 are Sport, Normal and Cinema. 3 names none. */
    for (i = 0; i < 4; ++i) {
        memset(payload, 0, sizeof(payload));
        payload[2] = (uint8_t)i;
        memset(&controls, 0, sizeof(controls));
        SDL_DJITCP_DecodeControls(payload, sizeof(payload), &controls);
        for (k = 0; k < 3; ++k) {
            CHECK(SDL_Serial_GetButton(&controls, SDL_DJI_BUTTON_MODE + k) == (k == i));
        }
    }
    /* A payload shorter than 17 bytes is not decoded */
    CHECK(!SDL_DJITCP_DecodeControls(payload, 16, &controls));
    CHECK(!SDL_DJITCP_DecodeControls(NULL, 17, &controls));
}

/* 2: a partial envelope waits, and of two envelopes in one read only the
 * last controls count */
static void TestBatches(void)
{
    TCPHarness *t = T_Up();
    uint8_t buffer[256];
    static const uint16_t rest[6] = { 1024, 1024, 1024, 1024, 1024, 1024 };
    size_t first, total;
    int list[2];

    first = Controls(buffer, 0x0020, 3, rest);
    total = first + Controls(buffer + first, 0x0100, 3, rest);
    t->now = 5;
    T_Receive(t, 0, buffer, 10);
    CHECK(!t->state.live && t->nsnapshots == 0);
    T_Receive(t, 0, buffer + 10, first - 10);
    CHECK(t->state.live);
    list[0] = SDL_DJI_BUTTON_HOME;
    list[1] = -1;
    CHECK(T_Buttons(t, list));
    {
        const int mark = t->nsnapshots;

        T_Receive(t, 0, buffer, total);
        CHECK(t->nsnapshots == mark + 1);
        list[0] = SDL_DJI_BUTTON_RECORD;
        CHECK(T_Buttons(t, list));
    }
    /* An envelope claiming more than 1023 bytes is passed one byte at a time */
    {
        uint8_t junk[8 + sizeof(test1)];

        memcpy(junk, "\x55\xCC\x30\x75\x00\x04\x00\x00", 8);
        memcpy(junk + 8, test1, sizeof(test1));
        T_Receive(t, 0, junk, sizeof(junk));
        CHECK(T_Axes(t, 0, 0, 32767, 0, 0, 0));
    }
    free(t);
}

/* 3: the keepalive schedule, and the DJI RC 2 policy */
static void TestKeepalive(void)
{
    TCPHarness *t = T_Up();
    uint8_t buffer[256];
    int i;

    /* After two empty 20 ms reads, that is 40 ms without data */
    T_Advance(t, 39);
    CHECK(T_Next(t) == NULL);
    T_Advance(t, 40);
    CHECK(T_IsKeepalive(T_Next(t), 0, 2, 40));
    T_Advance(t, 80);
    CHECK(T_IsKeepalive(T_Next(t), 0, 3, 80));
    /* None while frames arrive */
    for (i = 90; i <= 400; i += 30) {
        T_Advance(t, (uint64_t)i);
        T_Receive(t, 0, buffer, RestControls(buffer));
    }
    CHECK(T_Next(t) == NULL);
    /* The last frame came at 390 */
    T_Advance(t, 429);
    CHECK(T_Next(t) == NULL);
    T_Advance(t, 430);
    CHECK(T_IsKeepalive(T_Next(t), 0, 4, 430));

    /* rc331: no more keepalives, and links 1 to 4 start 500 ms apart */
    T_Receive(t, 0, buffer, Beacon(buffer, "rc331", 0x00));
    CHECK(t->state.quiet && t->state.running == 5);
    CHECK(T_Next(t) == NULL);
    T_Advance(t, 929);
    CHECK(T_Next(t) == NULL);
    T_Advance(t, 930);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 1, 930));
    T_Connected(t, 1, true);
    CHECK(T_Next(t) == NULL);
    T_Receive(t, 1, buffer, RestControls(buffer));
    T_Advance(t, 1430);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 2, 1430));
    T_Connected(t, 2, true);
    T_Receive(t, 2, buffer, RestControls(buffer));
    T_Advance(t, 1930);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 3, 1930));
    T_Connected(t, 3, true);
    T_Receive(t, 3, buffer, RestControls(buffer));
    T_Advance(t, 2430);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 4, 2430));
    T_Connected(t, 4, true);
    T_Receive(t, 4, buffer, RestControls(buffer));
    CHECK(T_Next(t) == NULL);
    /* The name was not known at the first frame */
    CHECK(strcmp(T_Snapshot(t)->identity.name, "DJI Remote") == 0);
    free(t);
}

/* The model named before the first frame names the joystick */
static void TestNames(void)
{
    TCPHarness *t = T_Up();
    uint8_t buffer[256];

    T_Receive(t, 0, buffer, Beacon(buffer, "rm330", 0x00));
    T_Receive(t, 0, buffer, RestControls(buffer));
    CHECK(strcmp(T_Snapshot(t)->identity.name, "DJI RC (RM330)") == 0);
    CHECK(!t->state.quiet && t->state.running == 1);
    free(t);

    t = T_Up();
    T_Receive(t, 0, buffer, Beacon(buffer, "rc331", 0x00));
    T_Receive(t, 0, buffer, RestControls(buffer));
    CHECK(strcmp(T_Snapshot(t)->identity.name, "DJI RC 2") == 0);
    free(t);
}

/* 4: a request asking for an acknowledgement gets one empty response */
static void TestAcknowledge(void)
{
    TCPHarness *t = T_Up();
    uint8_t buffer[256];
    uint8_t expected[64];
    static const uint8_t response[] = { 0x55, 0x0D, 0x04, 0x33, 0x02, 0x06, 0x10, 0x00, 0x80, 0x00, 0x81 };
    uint8_t frame[16];
    size_t length;

    t->now = 7;
    T_Receive(t, 0, buffer, Beacon(buffer, "rm330", 0x40));
    memcpy(frame, response, sizeof(response));
    {
        const uint16_t crc = SDL_DJI_CRC16(frame, 11);

        frame[11] = (uint8_t)(crc & 0xFF);
        frame[12] = (uint8_t)(crc >> 8);
    }
    length = SDL_DJI_BuildEnvelope(expected, sizeof(expected), frame, 13);
    CHECK(T_IsSend(T_Next(t), 0, expected, length, 7));
    CHECK(T_Next(t) == NULL);
    /* The codec test's response, 06 to 0A swapped */
    {
        static const uint8_t codec_response[] = { 0x55, 0x0D, 0x04, 0x33, 0x0A, 0x06, 0x10, 0x00, 0x80, 0x00, 0x81, 0x62, 0x18 };
        uint8_t payload[66];

        memset(payload, 0, sizeof(payload));
        memcpy(payload, "rc221", 5);
        length = Envelope(buffer, 0x06, 0x0A, 0x10, 0x40, 0x00, 0x81, payload, sizeof(payload));
        T_Receive(t, 0, buffer, length);
        length = SDL_DJI_BuildEnvelope(expected, sizeof(expected), codec_response, sizeof(codec_response));
        CHECK(T_IsSend(T_Next(t), 0, expected, length, 7));
    }
    /* No response to a response, or to a request that asks for none */
    T_Receive(t, 0, buffer, Beacon(buffer, "rm330", 0xC0));
    T_Receive(t, 0, buffer, Beacon(buffer, "rm330", 0x00));
    CHECK(T_Next(t) == NULL);
    /* Ack types 1, 2 and 3 all get one */
    T_Receive(t, 0, buffer, Envelope(buffer, 0x06, 0x02, 3, 0x20, 0x18, 0x40, NULL, 0));
    T_Receive(t, 0, buffer, Envelope(buffer, 0x06, 0x02, 4, 0x60, 0x00, 0x77, NULL, 0));
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_SEND, 0, 7) && T_Is(T_Next(t), SDL_DJI_TCP_SEND, 0, 7) && T_Next(t) == NULL);
    free(t);
}

/* 5: liveness and redials */
static void TestLiveness(void)
{
    TCPHarness *t = T_Up();
    uint8_t buffer[256];

    /* A link that carried a frame closes 1000 ms after it and redials at once */
    T_Advance(t, 100);
    T_Skip(t);
    T_Receive(t, 0, buffer, RestControls(buffer));
    CHECK(t->state.live && T_Snapshot(t)->present);
    T_Advance(t, 1099);
    T_Skip(t);
    CHECK(t->state.live);
    T_Advance(t, 1100);
    CHECK(!t->state.live && !T_Snapshot(t)->present);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CLOSE, 0, 1100));
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 0, 1100));
    CHECK(T_Next(t) == NULL);

    /* One that never did: 1000 ms after its connect, then 100 ms before the redial */
    T_Connected(t, 0, true);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_SEND, 0, 1100));
    T_Skip(t);
    T_Advance(t, 2099);
    T_Skip(t);
    T_Advance(t, 2100);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CLOSE, 0, 2100));
    CHECK(T_Next(t) == NULL);
    T_Advance(t, 2199);
    CHECK(T_Next(t) == NULL);
    T_Advance(t, 2200);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 0, 2200));

    /* A connect not done in 100 ms fails, and the redial waits 100 ms */
    T_Advance(t, 2299);
    CHECK(T_Next(t) == NULL);
    T_Advance(t, 2300);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CLOSE, 0, 2300));
    /* A report after the timeout is ignored */
    T_Connected(t, 0, true);
    CHECK(t->state.links[0].phase == SDL_DJI_LINK_WAITING && T_Next(t) == NULL);
    T_Advance(t, 2400);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 0, 2400));
    T_Connected(t, 0, true);
    CHECK(t->state.links[0].phase == SDL_DJI_LINK_UP);
    /* A failed connect waits 100 ms */
    T_Lost(t, 0);
    T_Skip(t);
    T_Advance(t, 2500);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 0, 2500));
    T_Connected(t, 0, false);
    T_Advance(t, 2599);
    CHECK(T_Next(t) == NULL);
    T_Advance(t, 2600);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 0, 2600));
    T_Connected(t, 0, true);
    T_Skip(t);

    /* Loss of a link that carried a frame redials at once */
    T_Receive(t, 0, buffer, RestControls(buffer));
    CHECK(t->state.live);
    T_Advance(t, 2700);
    T_Skip(t);
    T_Lost(t, 0);
    T_Advance(t, 2700);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 0, 2700));
    /* The stream stays live while another frame comes within 1000 ms */
    T_Connected(t, 0, true);
    T_Receive(t, 0, buffer, RestControls(buffer));
    CHECK(t->state.live);
    free(t);
}

/* A connect the driver reports lost before it finished waits 100 ms too */
static void TestLostConnect(void)
{
    TCPHarness *t = T_Create();

    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 0, 0));
    T_Advance(t, 30);
    T_Lost(t, 0);
    CHECK(t->state.links[0].phase == SDL_DJI_LINK_WAITING && T_Next(t) == NULL);
    T_Advance(t, 129);
    CHECK(T_Next(t) == NULL);
    T_Advance(t, 130);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 0, 130));
    free(t);
}

/* Links stay open while any link delivers */
static void TestSharedLiveness(void)
{
    TCPHarness *t = T_Up();
    uint8_t buffer[256];

    T_Receive(t, 0, buffer, Beacon(buffer, "rc331", 0x00));
    T_Advance(t, 500);
    CHECK(T_Is(T_Next(t), SDL_DJI_TCP_CONNECT, 1, 500));
    T_Connected(t, 1, true);
    /* Link 0 brings frames. Link 1 stays silent and open. */
    {
        uint64_t when;

        for (when = 600; when <= 3000; when += 100) {
            T_Advance(t, when);
            T_Receive(t, 0, buffer, RestControls(buffer));
        }
    }
    CHECK(t->state.links[1].phase == SDL_DJI_LINK_UP);
    /* Then nothing from anyone: both close 1000 ms after the last frame */
    T_Skip(t);
    T_Advance(t, 4000);
    CHECK(t->state.links[0].phase != SDL_DJI_LINK_UP && t->state.links[1].phase != SDL_DJI_LINK_UP);
    free(t);
}

static void TestBattery(void)
{
    TCPHarness *t = T_Up();
    uint8_t buffer[256];
    static const uint8_t status[] = { 0x28, 0x0A, 0x00, 0x00, 0x64, 0x01 };

    CHECK(t->state.battery == -1);
    T_Receive(t, 0, buffer, Envelope(buffer, 0x06, 0x02, 9, 0x00, 0x06, 0x1E, status, sizeof(status)));
    CHECK(t->state.battery == 100);
    {
        uint8_t low[6];

        memcpy(low, status, sizeof(low));
        low[4] = 37;
        T_Receive(t, 0, buffer, Envelope(buffer, 0x06, 0x02, 9, 0x00, 0x06, 0x1E, low, sizeof(low)));
        CHECK(t->state.battery == 37);
        low[4] = 101;
        T_Receive(t, 0, buffer, Envelope(buffer, 0x06, 0x02, 9, 0x00, 0x06, 0x1E, low, sizeof(low)));
        CHECK(t->state.battery == 37);
        T_Receive(t, 0, buffer, Envelope(buffer, 0x06, 0x02, 9, 0x00, 0x06, 0x1E, low, 4));
        CHECK(t->state.battery == 37);
    }
    free(t);
}

/* Data on a link that is not up is ignored, and reports for links outside
 * the pool change nothing */
static void TestStale(void)
{
    TCPHarness *t = T_Create();
    uint8_t buffer[256];

    T_Receive(t, 0, buffer, RestControls(buffer));
    CHECK(!t->state.live);
    T_Connected(t, 3, true);
    T_Lost(t, 4);
    SDL_DJITCP_Received(&t->state, -1, buffer, 1, 0);
    SDL_DJITCP_Received(&t->state, 7, buffer, 1, 0);
    CHECK(t->state.links[3].phase == SDL_DJI_LINK_IDLE);
    free(t);
}

typedef struct HostLog
{
    int count;
    char last[64];
} HostLog;

static void LogHost(void *userdata, const char *entry, size_t length, const char *reason)
{
    HostLog *log = (HostLog *)userdata;

    (void)reason;
    ++log->count;
    if (length >= sizeof(log->last)) {
        length = sizeof(log->last) - 1;
    }
    memcpy(log->last, entry, length);
    log->last[length] = '\0';
}

static void TestHosts(void)
{
    SDL_DJITCPHost hosts[SDL_DJI_TCP_MAX_HOSTS];
    HostLog log;

    memset(&log, 0, sizeof(log));
    CHECK(SDL_DJITCP_ParseHosts("192.168.7.251", hosts, SDL_DJI_TCP_MAX_HOSTS, LogHost, &log) == 1);
    CHECK(hosts[0].address == 0xC0A807FBu && hosts[0].port == 40007 && strcmp(hosts[0].key, "192.168.7.251:40007") == 0);
    CHECK(SDL_DJITCP_ParseHosts(" 127.0.0.1:40008 , 10.0.0.2 ,, ", hosts, SDL_DJI_TCP_MAX_HOSTS, LogHost, &log) == 2);
    CHECK(hosts[0].port == 40008 && hosts[0].address == 0x7F000001u && strcmp(hosts[1].key, "10.0.0.2:40007") == 0);
    CHECK(log.count == 0);
    /* An address named twice keeps one entry */
    CHECK(SDL_DJITCP_ParseHosts("10.0.0.2,10.0.0.2:40007,10.0.0.2:1", hosts, SDL_DJI_TCP_MAX_HOSTS, LogHost, &log) == 2);
    CHECK(log.count == 0);
    /* Entries that cannot be used, one log each */
    CHECK(SDL_DJITCP_ParseHosts("1.2.3,256.1.1.1,01.2.3.4,1.2.3.4:,1.2.3.4:0,1.2.3.4:65536,1.2.3.4:080,a.b.c.d,1.2.3.4.5,1..2.3,1.2.3.4:5x",
                                hosts, SDL_DJI_TCP_MAX_HOSTS, LogHost, &log) == 0);
    CHECK(log.count == 11 && strcmp(log.last, "1.2.3.4:5x") == 0);
    /* The limits: 0 and 255 in an octet, port 65535 */
    CHECK(SDL_DJITCP_ParseHosts("0.0.0.0,255.255.255.255:65535", hosts, SDL_DJI_TCP_MAX_HOSTS, LogHost, &log) == 2);
    CHECK(hosts[1].address == 0xFFFFFFFFu && hosts[1].port == 65535 && strcmp(hosts[1].key, "255.255.255.255:65535") == 0);
    /* Too many hosts */
    log.count = 0;
    CHECK(SDL_DJITCP_ParseHosts("1.0.0.1,1.0.0.2,1.0.0.3", hosts, 2, LogHost, &log) == 2);
    CHECK(log.count == 1 && strcmp(log.last, "1.0.0.3") == 0);
    CHECK(SDL_DJITCP_ParseHosts(NULL, hosts, 2, LogHost, &log) == 0);
    CHECK(SDL_DJITCP_ParseHosts("", hosts, 2, NULL, NULL) == 0);
    CHECK(SDL_DJITCP_ParseHosts("bad", hosts, 2, NULL, NULL) == 0);
}

int main(void)
{
    TestControls();
    TestDecode();
    TestBatches();
    TestKeepalive();
    TestNames();
    TestAcknowledge();
    TestLiveness();
    TestLostConnect();
    TestSharedLiveness();
    TestBattery();
    TestStale();
    TestHosts();
    return H_Finish();
}
