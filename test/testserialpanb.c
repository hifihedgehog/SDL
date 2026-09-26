/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_panb_proto.c, Konami's
   PANB panel of Nostalgia, hifihedgehog/SDL#33 Part 14. The vectors are the
   ticket's, constructed from bemanitools' aciodrv, aciodrv-proc, aciotest
   and ACIO headers. Test numbers follow the ticket's RS-232 tests. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_panb_proto.h"

/* The ticket's bytes, test 2. The start carries the sequence 2, where the
   bring-up of the panel's four nodes takes 1 to 9, so the module sends the
   same frame with 10. */
static const uint8_t ticket_start[8] = { 0xAA, 0x01, 0x01, 0x15, 0x02, 0x01, 0x04, 0x1E };
static const uint8_t ticket_key1[16] = { 0x05, 0x07, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t ticket_key28[16] = { 0x05, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F };

/* The bus's bring-up of four nodes */
static const uint8_t aa[1] = { 0xAA };
static const uint8_t enumerate_request[8] = { 0xAA, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x03 };
static const uint8_t enumerate_reply[8] = { 0xAA, 0x00, 0x00, 0x01, 0x01, 0x01, 0x04, 0x07 };

/* The module's */
static const uint8_t start_request[8] = { 0xAA, 0x01, 0x01, 0x15, 0x0A, 0x01, 0x04, 0x26 };
static uint8_t version_replies[4][51];
static uint8_t start_replies[4][8];
static uint8_t idle_frame[23];

/* An encoder written apart from the modules', from aciodrv/device.c */
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

static uint8_t Sum(const uint8_t *body, size_t length)
{
    uint8_t sum = 0;
    size_t i;

    for (i = 0; i < length; ++i) {
        sum = (uint8_t)(sum + body[i]);
    }
    return sum;
}

static void Version(uint8_t *payload, uint32_t type, const char *product)
{
    memset(payload, 0, SDL_ACIO_VERSION_LENGTH);
    payload[0] = (uint8_t)(type >> 24);
    payload[1] = (uint8_t)(type >> 16);
    payload[2] = (uint8_t)(type >> 8);
    payload[3] = (uint8_t)type;
    payload[5] = 1;
    memcpy(payload + 8, product, strlen(product));
}

static SDL_PANBState *State(Harness *h)
{
    return (SDL_PANBState *)h->state;
}

static void BuildVectors(void)
{
    static const uint8_t zeros[SDL_PANB_INPUT] = { 0 };
    uint8_t payload[SDL_ACIO_VERSION_LENGTH];
    int i;

    Version(payload, SDL_ACIO_TYPE_PANB, "PANB");
    for (i = 0; i < 4; ++i) {
        CHECK(Encode((uint8_t)(0x81 + i), SDL_ACIO_CMD_GET_VERSION, (uint8_t)(2 + i), payload, SDL_ACIO_VERSION_LENGTH, version_replies[i]) == 51);
        CHECK(Encode((uint8_t)(0x81 + i), SDL_ACIO_CMD_START, (uint8_t)(6 + i), zeros, 1, start_replies[i]) == 8);
    }
    CHECK(Encode(0x81, SDL_PANB_CMD_STREAM, 10, zeros, SDL_PANB_INPUT, idle_frame) == sizeof(idle_frame));
}

/* A streamed frame with the given payload */
static size_t Frame(const uint8_t *payload, uint8_t *out)
{
    return Encode(0x81, SDL_PANB_CMD_STREAM, 10, payload, SDL_PANB_INPUT, out);
}

static void FeedFrame(Harness *h, const uint8_t *payload)
{
    uint8_t frame[64];

    H_Feed(h, frame, Frame(payload, frame));
}

/* From an open or a restart: the reset and the bring-up of the panel's four
   nodes, up to the start */
static void BringUpBus(Harness *h)
{
    int i;

    H_Advance(h, h->now + 2650);
    H_Feed(h, aa, 1);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    for (i = 0; i < 4; ++i) {
        H_Feed(h, version_replies[i], sizeof(version_replies[i]));
    }
    for (i = 0; i < 4; ++i) {
        H_Feed(h, start_replies[i], sizeof(start_replies[i]));
    }
}

/* The start and the first frame */
static void BringUp(Harness *h)
{
    BringUpBus(h);
    H_Feed(h, idle_frame, sizeof(idle_frame));
}

static Harness *Present(void)
{
    Harness *h = H_Create(&SDL_SerialPANBModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1 && State(h)->step == SDL_PANB_STEP_STREAM);
    H_SkipCalls(h);
    return h;
}

static bool ExpectRestart(Harness *h, uint64_t t)
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

/* Test 2: the ticket's frames, byte for byte, and the module's */
static void TestTicketFrames(void)
{
    const uint8_t nodes = SDL_PANB_NODES;
    uint8_t out[SDL_ACIO_MAX_FRAME];
    SDL_SerialControls controls;
    int i;

    CHECK(Sum(ticket_start + 1, 6) == 0x1E && Sum(start_request + 1, 6) == 0x26);
    CHECK(SDL_ACIO_EncodeFrame(0x01, SDL_PANB_CMD_START, 2, &nodes, 1, out, sizeof(out)) == 8 && memcmp(out, ticket_start, 8) == 0);
    CHECK(SDL_ACIO_EncodeFrame(0x01, SDL_PANB_CMD_START, 10, &nodes, 1, out, sizeof(out)) == 8 && memcmp(out, start_request, 8) == 0);
    CHECK(memcmp(start_request, ticket_start, 4) == 0 && memcmp(start_request + 5, ticket_start + 5, 2) == 0);
    /* The count is bemanitools' four nodes of seven keys */
    CHECK(SDL_PANB_NODES == 4 && SDL_PANB_KEYS == 28 && SDL_PANB_INPUT == 16 && SDL_PANB_KEYS == 7 * SDL_PANB_NODES);
    /* The streamed frame is 0110 (acio/panb.h), and the start 0115 */
    CHECK(SDL_PANB_CMD_STREAM == 0x0110 && SDL_PANB_CMD_START == 0x0115);
    CHECK(idle_frame[0] == 0xAA && idle_frame[2] == 0x01 && idle_frame[3] == 0x10 && idle_frame[5] == 0x10 && idle_frame[22] == 0xAC);
    /* 2: 05 07 F0 00 ... presses key 1 only, byte 15 = 0F key 28 only */
    SDL_PANB_Decode(ticket_key1, &controls);
    for (i = 0; i < SDL_SERIAL_MAX_BUTTONS; ++i) {
        CHECK(SDL_Serial_GetButton(&controls, i) == (i == 0));
    }
    SDL_PANB_Decode(ticket_key28, &controls);
    for (i = 0; i < SDL_SERIAL_MAX_BUTTONS; ++i) {
        CHECK(SDL_Serial_GetButton(&controls, i) == (i == 27));
    }
    /* The gap is three reply times of the bus */
    CHECK(SDL_PANB_GAP_MS == 600);
}

/* Test 2 through the port, from the open */
static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialPANBModule);
    int i;

    /* The line: 57600 8N1 first, DTR and RTS on, then the reset */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 57600, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(ExpectRestart(h, 0));
    CHECK(State(h)->acio.bus.rate == 57600 && State(h)->acio.bus.alternate_rate == 115200);
    H_Advance(h, 2650);
    H_SkipCalls(h);
    H_Feed(h, aa, 1);
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, sizeof(enumerate_request), 2650));
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    for (i = 0; i < 4; ++i) {
        H_Feed(h, version_replies[i], sizeof(version_replies[i]));
    }
    for (i = 0; i < 4; ++i) {
        CHECK(State(h)->step == SDL_PANB_STEP_BUS && !State(h)->acio.base.close_sequence);
        H_Feed(h, start_replies[i], sizeof(start_replies[i]));
    }
    /* The start to the first node, sequence 10, with nothing to answer it */
    h->cursor = h->ncalls - 1;
    CHECK(H_IsWrite(H_NextCall(h), start_request, sizeof(start_request), 2650) && H_NextCall(h) == NULL);
    CHECK(State(h)->step == SDL_PANB_STEP_STREAM && State(h)->node == 1 && State(h)->deadline == 2650 + SDL_PANB_GAP_MS);
    CHECK(State(h)->acio.base.close_sequence && !State(h)->acio.bus.pending && h->npresence == 0);
    /* 2: the ticket's frame presents the joystick with key 1 pressed */
    H_Advance(h, 2700);
    FeedFrame(h, ticket_key1);
    CHECK(h->presence[0] == 1 && h->npresence == 1 && h->npublished == 1);
    CHECK(strcmp(h->identity[0].name, "Konami Nostalgia Panel") == 0 && h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN);
    CHECK(h->identity[0].naxes == 0 && h->identity[0].nbuttons == 28 && h->identity[0].nhats == 0 && h->identity[0].nballs == 0);
    CHECK(!h->identity[0].has_mapping && H_OnlyButton(h, 0, 0));
    CHECK(State(h)->deadline == 2700 + SDL_PANB_GAP_MS);
    FeedFrame(h, ticket_key28);
    CHECK(H_OnlyButton(h, 0, 27) && h->npublished == 2);
    /* Frames ask for nothing */
    CHECK(H_NextCall(h) == NULL);
    CHECK(strcmp(SDL_SerialPANBModule.token, "panb") == 0 && SDL_SerialPANBModule.state_size == sizeof(SDL_PANBState));
    CHECK(!SDL_SerialPANBModule.rumble && SDL_SerialPANBModule.effect_max == 0);
    H_Destroy(h);
}

/* Every value of every key alone */
static void TestDecode(void)
{
    uint8_t payload[SDL_PANB_INPUT];
    SDL_SerialControls controls;
    int key, value, byte, i;

    for (key = 0; key < SDL_PANB_KEYS; ++key) {
        for (value = 0; value < 16; ++value) {
            memset(payload, 0, sizeof(payload));
            payload[2 + key / 2] = (uint8_t)((key % 2) ? value : (value << 4));
            SDL_PANB_Decode(payload, &controls);
            for (i = 0; i < SDL_SERIAL_MAX_BUTTONS; ++i) {
                CHECK(SDL_Serial_GetButton(&controls, i) == (i == key && value != 0));
            }
            for (i = 0; i < SDL_SERIAL_MAX_AXES; ++i) {
                CHECK(controls.axes[i] == 0);
            }
        }
    }
    /* Bytes 0 and 1 press nothing */
    for (byte = 0; byte < 2; ++byte) {
        for (value = 0; value < 256; ++value) {
            memset(payload, 0, sizeof(payload));
            payload[byte] = (uint8_t)value;
            SDL_PANB_Decode(payload, &controls);
            for (i = 0; i < SDL_SERIAL_MAX_BUTTONS; ++i) {
                CHECK(!SDL_Serial_GetButton(&controls, i));
            }
        }
    }
    /* Every key at once, the weakest value */
    memset(payload, 0x11, sizeof(payload));
    SDL_PANB_Decode(payload, &controls);
    for (i = 0; i < SDL_SERIAL_MAX_BUTTONS; ++i) {
        CHECK(SDL_Serial_GetButton(&controls, i) == (i < SDL_PANB_KEYS));
    }
}

/* The stream: any address, other frames, and the gap */
static void TestStream(void)
{
    static const uint8_t addresses[6] = { 0x81, 0x01, 0x84, 0x80, 0x00, 0x7F };
    uint8_t payload[SDL_PANB_INPUT + 1], frame[64];
    Harness *h = Present();
    uint64_t t;
    int i;

    /* bemanitools checks neither address nor sequence */
    for (i = 0; i < 6; ++i) {
        memset(payload, 0, sizeof(payload));
        payload[2 + i] = 0x30;
        H_Feed(h, frame, Encode(addresses[i], SDL_PANB_CMD_STREAM, (uint8_t)(i * 50), payload, SDL_PANB_INPUT, frame));
        CHECK(H_OnlyButton(h, 0, 2 * i));
    }
    /* Other commands and lengths change nothing and leave the gap running */
    H_Advance(h, 2800);
    memset(payload, 0x77, sizeof(payload));
    H_Feed(h, frame, Encode(0x81, SDL_PANB_CMD_STREAM + 1, 10, payload, SDL_PANB_INPUT, frame));
    H_Feed(h, frame, Encode(0x81, SDL_PANB_CMD_START, 10, payload, SDL_PANB_INPUT, frame));
    H_Feed(h, frame, Encode(0x81, SDL_PANB_CMD_STREAM, 10, payload, SDL_PANB_INPUT - 1, frame));
    H_Feed(h, frame, Encode(0x81, SDL_PANB_CMD_STREAM, 10, payload, SDL_PANB_INPUT + 1, frame));
    {
        static const uint8_t broadcast[6] = { 0xAA, 0x70, 0x02, 0x11, 0x22, 0xA5 };

        H_Feed(h, broadcast, sizeof(broadcast));
    }
    CHECK(H_OnlyButton(h, 0, 10) && State(h)->deadline == 2650 + SDL_PANB_GAP_MS && h->npublished == 7);
    /* Frames every 500 ms keep the stream for 10 seconds */
    for (t = 3000; t <= 13000; t += 500) {
        H_Advance(h, t);
        FeedFrame(h, ticket_key1);
    }
    CHECK(State(h)->acio.bus.restarts == 0 && h->presence[0] == 1 && H_NextCall(h) == NULL);
    /* 599 ms with no frame keeps it too */
    H_Advance(h, 13000 + SDL_PANB_GAP_MS - 1);
    CHECK(State(h)->acio.bus.restarts == 0 && h->presence[0] == 1 && H_NextCall(h) == NULL);
    /* 600 ms: the bus starts over, the joystick goes, and the panel comes
       back with a new start */
    H_Advance(h, 13000 + SDL_PANB_GAP_MS);
    CHECK(State(h)->acio.bus.restarts == 1 && h->presence[0] == 0 && State(h)->step == SDL_PANB_STEP_BUS && State(h)->node == 0);
    CHECK(ExpectRestart(h, 13600) && strcmp(h->last_log, "The PANB stream stopped, so the bus starts over") == 0);
    CHECK(State(h)->acio.bus.rate == 57600);
    BringUpBus(h);
    h->cursor = h->ncalls - 1;
    CHECK(H_IsWrite(H_NextCall(h), start_request, sizeof(start_request), 16250));
    FeedFrame(h, ticket_key28);
    CHECK(h->presence[0] == 2 && H_OnlyButton(h, 0, 27));
    H_Destroy(h);

    /* A start that brings no frame at all starts the bus over too */
    h = H_Create(&SDL_SerialPANBModule);
    H_Start(h);
    BringUpBus(h);
    H_SkipCalls(h);
    H_Advance(h, 2650 + SDL_PANB_GAP_MS - 1);
    CHECK(State(h)->acio.bus.restarts == 0);
    H_Advance(h, 2650 + SDL_PANB_GAP_MS);
    CHECK(State(h)->acio.bus.restarts == 1 && ExpectRestart(h, 3250) && h->npresence == 0);
    H_Destroy(h);
}

/* Every truncation and split of a streamed frame */
static void TestTruncations(void)
{
    uint8_t frame[64];
    const size_t length = Frame(ticket_key1, frame);
    size_t k;

    for (k = 0; k <= length; ++k) {
        int split;

        for (split = 0; split < 2; ++split) {
            Harness *h = H_Create(&SDL_SerialPANBModule);

            H_Start(h);
            BringUpBus(h);
            H_SkipCalls(h);
            if (split == 0) {
                H_FeedPadded(h, frame, k);
                if (k < length) {
                    CHECK(h->npresence == 0 && State(h)->deadline == 2650 + SDL_PANB_GAP_MS);
                    H_Feed(h, frame, length);
                }
            } else if (k > 0 && k < length) {
                H_Feed(h, frame, k);
                H_Feed(h, frame + k, length - k);
            } else {
                size_t b;

                for (b = 0; b < length; ++b) {
                    H_Feed(h, frame + b, 1);
                }
            }
            CHECK(h->presence[0] == 1 && H_OnlyButton(h, 0, 0) && H_NextCall(h) == NULL);
            H_Destroy(h);
        }
    }
}

/* Test 5 */
static void TestBatteryA(void)
{
    uint8_t payload[SDL_PANB_INPUT + 1];
    uint8_t key1[64], key28[64], all[64], escaped[64], idle_again[64];
    uint8_t bad_sum[64], bad_command[64], short_frame[64], long_frame[64], cut_ff[64];
    size_t n_key1, n_key28, n_all, n_escaped, n_idle, n_bad_sum, n_bad_command, n_short, n_long, n_cut_ff;
    H_Vector vectors[8];
    Harness *h;
    int i, count = 0;

    n_key1 = Frame(ticket_key1, key1);
    n_key28 = Frame(ticket_key28, key28);
    memset(payload, 0x11, sizeof(payload));
    n_all = Frame(payload, all);
    memset(payload, 0, sizeof(payload));
    payload[2] = 0xAA;
    payload[3] = 0xFF;
    n_escaped = Frame(payload, escaped);
    CHECK(n_escaped == n_key1 + 2);
    memset(payload, 0, sizeof(payload));
    n_idle = Frame(payload, idle_again);
    n_bad_sum = Frame(ticket_key28, bad_sum);
    bad_sum[n_bad_sum - 1] ^= 0x01;
    n_bad_command = Encode(0x81, SDL_PANB_CMD_START, 10, ticket_key28, SDL_PANB_INPUT, bad_command);
    n_short = Encode(0x81, SDL_PANB_CMD_STREAM, 10, ticket_key28, SDL_PANB_INPUT - 1, short_frame);
    memcpy(payload, ticket_key28, SDL_PANB_INPUT);
    payload[SDL_PANB_INPUT] = 0x00;
    n_long = Encode(0x81, SDL_PANB_CMD_STREAM, 10, payload, SDL_PANB_INPUT + 1, long_frame);
    memset(payload, 0, sizeof(payload));
    payload[0] = 0xFF;
    Frame(payload, cut_ff);
    CHECK(cut_ff[6] == 0xFF);
    n_cut_ff = 7;

    h = Present();
    H_SetVector(&vectors[count++], "2 key 1", key1, n_key1, 1, 0);
    H_SetVector(&vectors[count++], "2 key 28", key28, n_key28, 1, 0);
    H_SetVector(&vectors[count++], "every key", all, n_all, 1, 0);
    H_SetVector(&vectors[count++], "keys 1 to 4 escaped", escaped, n_escaped, 1, 0);
    H_SetVector(&vectors[count++], "idle again", idle_again, n_idle, 0, 0);
    for (i = 0; i < count; ++i) {
        H_BatteryA(h, &vectors[i], BringUp);
    }
    H_BatteryA6(h, "wrong checksum", bad_sum, n_bad_sum, &vectors[0]);
    H_BatteryA6(h, "command 0115", bad_command, n_bad_command, &vectors[0]);
    H_BatteryA6(h, "15 bytes", short_frame, n_short, &vectors[0]);
    H_BatteryA6(h, "17 bytes", long_frame, n_long, &vectors[0]);
    H_BatteryA6(h, "cut after FF", cut_ff, n_cut_ff, &vectors[0]);
    for (i = 1; i < (int)n_key28; ++i) {
        H_BatteryA6(h, "every truncation", key28, (size_t)i, &vectors[0]);
    }
    H_Destroy(h);
}

/* Bring-up with nodes of the given types, up to Ready */
static void BringUpTypes(Harness *h, const uint32_t *types, int count)
{
    uint8_t payload[SDL_ACIO_VERSION_LENGTH], frame[64];
    uint8_t seq = 1;
    int i;

    H_Advance(h, h->now + 2650);
    H_Feed(h, aa, 1);
    payload[0] = (uint8_t)count;
    H_Feed(h, frame, Encode(0x00, SDL_ACIO_CMD_ENUMERATE, seq++, payload, 1, frame));
    for (i = 0; i < count; ++i) {
        Version(payload, types[i], (types[i] == SDL_ACIO_TYPE_PANB) ? "PANB" : "KFCA");
        H_Feed(h, frame, Encode((uint8_t)(0x81 + i), SDL_ACIO_CMD_GET_VERSION, seq++, payload, SDL_ACIO_VERSION_LENGTH, frame));
    }
    for (i = 0; i < count; ++i) {
        payload[0] = 0x00;
        H_Feed(h, frame, Encode((uint8_t)(0x81 + i), SDL_ACIO_CMD_START, seq++, payload, 1, frame));
    }
}

static void TestNodes(void)
{
    const uint8_t nodes = SDL_PANB_NODES;
    uint8_t expected[16];
    Harness *h;

    /* One node: the start, sequence 4 */
    {
        static const uint32_t types[1] = { SDL_ACIO_TYPE_PANB };

        h = H_Create(&SDL_SerialPANBModule);
        H_Start(h);
        BringUpTypes(h, types, 1);
        h->cursor = h->ncalls - 1;
        CHECK(H_IsWrite(H_NextCall(h), expected, Encode(0x01, SDL_PANB_CMD_START, 4, &nodes, 1, expected), 2650));
        CHECK(expected[7] == 0x20 && State(h)->node == 1);
        H_Destroy(h);
    }
    /* Other nodes first: the lowest PANB, sequence 10 */
    {
        static const uint32_t types[4] = { SDL_ACIO_TYPE_KFCA, SDL_ACIO_TYPE_ICCA, SDL_ACIO_TYPE_PANB, SDL_ACIO_TYPE_PANB };

        h = H_Create(&SDL_SerialPANBModule);
        H_Start(h);
        BringUpTypes(h, types, 4);
        h->cursor = h->ncalls - 1;
        CHECK(H_IsWrite(H_NextCall(h), expected, Encode(0x03, SDL_PANB_CMD_START, 10, &nodes, 1, expected), 2650));
        CHECK(State(h)->node == 3 && State(h)->step == SDL_PANB_STEP_STREAM);
        H_Destroy(h);
    }
    /* No PANB node: no start, frames are ignored, and no reset on close */
    {
        static const uint32_t types[2] = { SDL_ACIO_TYPE_KFCA, SDL_ACIO_TYPE_ICCA };

        h = H_Create(&SDL_SerialPANBModule);
        H_Start(h);
        BringUpTypes(h, types, 2);
        CHECK(State(h)->step == SDL_PANB_STEP_IDLE && State(h)->node == 0 && !State(h)->acio.base.close_sequence);
        CHECK(strcmp(h->last_log, "No PANB node on the bus, so no joystick opens") == 0);
        H_SkipCalls(h);
        FeedFrame(h, ticket_key1);
        H_Advance(h, 60000);
        CHECK(H_NextCall(h) == NULL && h->npresence == 0 && State(h)->acio.bus.restarts == 0);
        CHECK(!SDL_SerialEngine_BeginStop(&h->engine, H_NS(60000)) && H_IsCall(H_NextCall(h), 'C', 60000));
        H_Destroy(h);
    }
}

/* The rate no source records: each failed bring-up switches it */
static void TestRates(void)
{
    Harness *h = H_Create(&SDL_SerialPANBModule);

    H_Start(h);
    H_Advance(h, 2650 + 500 * SDL_ACIO_PROBE_MS - 1);
    H_SkipCalls(h);
    H_Advance(h, 2650 + 500 * SDL_ACIO_PROBE_MS);
    CHECK(H_IsPlainLine(H_NextCall(h), 115200, 8, SDL_SERIAL_NOPARITY, 1, 7650) && ExpectRestart(h, 7650));
    CHECK(strcmp(h->last_log, "ACIO bus: nothing came up at 57600 baud, so the bus tries 115200") == 0);
    /* The panel comes up at 115200 and keeps it when its stream stops */
    BringUp(h);
    CHECK(h->presence[0] == 1 && h->engine.line.rate == 115200);
    H_SkipCalls(h);
    H_Advance(h, h->now + SDL_PANB_GAP_MS);
    CHECK(h->presence[0] == 0 && ExpectRestart(h, h->now) && State(h)->acio.bus.rate == 115200);
    /* A failed bring-up at 115200 goes back to 57600 */
    H_Advance(h, h->now + 2650);
    H_Feed(h, aa, 1);
    H_SkipCalls(h);
    H_Advance(h, h->now + SDL_ACIO_REPLY_MS);
    CHECK(H_IsPlainLine(H_NextCall(h), 57600, 8, SDL_SERIAL_NOPARITY, 1, h->now) && ExpectRestart(h, h->now));
    CHECK(strcmp(h->last_log, "ACIO bus: nothing came up at 115200 baud, so the bus tries 57600") == 0);
    H_Destroy(h);
}

/* The streaming hazard: the port resets the panel before it closes */
static void TestClose(void)
{
    Harness *h;

    /* Streaming: the joystick goes, the zeros and the break, then the port
       closes when the break ends, frames dropped meanwhile */
    h = Present();
    H_Advance(h, 2700);
    FeedFrame(h, ticket_key1);
    CHECK(H_OnlyButton(h, 0, 0));
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(2700)));
    CHECK(h->presence[0] == 0 && ExpectRestart(h, 2700) && H_NextCall(h) == NULL);
    CHECK(strcmp(h->last_log, "ACIO bus: the port closes, so the bus resets the nodes first") == 0);
    H_Advance(h, 3000);
    FeedFrame(h, ticket_key28);
    CHECK(h->npresence == 2 && State(h)->step == SDL_PANB_STEP_BUS);
    /* The gap timer waits: 600 ms pass with no restart */
    H_Advance(h, 2700 + SDL_ACIO_BREAK_MS - 1);
    CHECK(H_NextCall(h) == NULL && h->engine.open && State(h)->acio.bus.restarts == 1);
    H_Advance(h, 2700 + SDL_ACIO_BREAK_MS);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 4150) && H_IsCall(H_NextCall(h), 'C', 4150) && H_NextCall(h) == NULL);
    CHECK(!h->engine.open && !SDL_SerialEngine_IsStopping(&h->engine));
    H_Advance(h, 20000);
    CHECK(H_NextCall(h) == NULL && h->engine.opens == 1);
    H_Destroy(h);

    /* After a gap restart the panel may still stream: the close resets too,
       and a reset under way ends it */
    h = Present();
    H_Advance(h, 2650 + SDL_PANB_GAP_MS);
    CHECK(ExpectRestart(h, 3250));
    H_Advance(h, 3300);
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(3300)) && H_NextCall(h) == NULL);
    H_Advance(h, 3250 + SDL_ACIO_BREAK_MS);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 4700) && H_IsCall(H_NextCall(h), 'C', 4700));
    H_Destroy(h);

    /* Before the start nothing streams, and the port closes at once */
    h = H_Create(&SDL_SerialPANBModule);
    H_Start(h);
    H_Advance(h, 2650);
    H_Feed(h, aa, 1);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_SkipCalls(h);
    CHECK(!State(h)->acio.base.close_sequence);
    CHECK(!SDL_SerialEngine_BeginStop(&h->engine, H_NS(2650)) && H_IsCall(H_NextCall(h), 'C', 2650) && H_NextCall(h) == NULL);
    H_Destroy(h);

    /* A loss during the close ends it */
    h = Present();
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(2650)));
    H_Advance(h, 3000);
    H_LosePort(h);
    H_SkipCalls(h);
    CHECK(!h->engine.open && !SDL_SerialEngine_IsStopping(&h->engine));
    H_Advance(h, 10000);
    CHECK(H_NextCall(h) == NULL);
    H_Destroy(h);
}

/* Battery B and test 5's reconnect: a new port has no reset on close until
   its own start */
static void TestBatteryB(void)
{
    Harness *h = Present();

    FeedFrame(h, ticket_key1);
    CHECK(H_Button(h, 0, 0) && State(h)->acio.base.close_sequence);
    H_SkipCalls(h);
    H_Advance(h, 2700);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 2700));
    H_Advance(h, 3700);
    CHECK(H_ExpectOpened(h, 57600, 8, SDL_SERIAL_NOPARITY, 1, 3700) && ExpectRestart(h, 3700));
    CHECK(!State(h)->acio.base.close_sequence && State(h)->step == SDL_PANB_STEP_BUS && State(h)->node == 0);
    BringUpBus(h);
    h->cursor = h->ncalls - 1;
    CHECK(H_IsWrite(H_NextCall(h), start_request, sizeof(start_request), 6350));
    CHECK(State(h)->acio.base.close_sequence);
    H_Feed(h, idle_frame, sizeof(idle_frame));
    CHECK(h->presence[0] == 2 && H_NoButtons(h, 0));
    H_Destroy(h);
}

int main(void)
{
    BuildVectors();
    TestTicketFrames();
    TestStartup();
    TestDecode();
    TestStream();
    TestTruncations();
    TestBatteryA();
    TestNodes();
    TestRates();
    TestClose();
    TestBatteryB();
    return H_Finish();
}
