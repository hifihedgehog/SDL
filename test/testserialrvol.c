/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_rvol_proto.c, Konami's
   RVOL board of MUSECA, hifihedgehog/SDL#33 Part 14. The vectors are the
   ticket's, constructed from bemanitools' aciodrv, aciotest and ACIO
   headers, and the version reply carries arcade-docs' RVOL entry. Test
   numbers follow the ticket's RS-232 tests. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_rvol_proto.h"

/* The ticket's bytes, test 3. They carry the sequences 2 and 3, where one
   node's bring-up takes 1 to 3, so the module sends the same frames with 4
   and 5. */
static const uint8_t ticket_expand[8] = { 0xAA, 0x01, 0x01, 0x14, 0x02, 0x01, 0xC0, 0xD9 };
static const uint8_t ticket_poll_header[6] = { 0xAA, 0x01, 0x01, 0x12, 0x03, 0x20 };

/* The bus's bring-up, from the framing tests */
static const uint8_t aa[1] = { 0xAA };
static const uint8_t enumerate_request[8] = { 0xAA, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x03 };
static const uint8_t enumerate_reply[8] = { 0xAA, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x04 };
static const uint8_t version_request[7] = { 0xAA, 0x01, 0x00, 0x02, 0x02, 0x00, 0x05 };
static const uint8_t start_request[7] = { 0xAA, 0x01, 0x00, 0x03, 0x03, 0x00, 0x07 };
static const uint8_t start_reply[8] = { 0xAA, 0x81, 0x00, 0x03, 0x03, 0x01, 0x00, 0x88 };

/* The module's */
static const uint8_t expand_request[8] = { 0xAA, 0x01, 0x01, 0x14, 0x04, 0x01, 0xC0, 0xDB };
static uint8_t version_reply[51]; /* arcade-docs' RVOL, KFCA 1.6.1 of Oct 21 2015 */
static uint8_t expand_reply[8];
static uint8_t poll_request[39];  /* Sequence 5: AA 01 01 12 05 20, 32 bytes of 00, 39 */
static uint8_t idle_reply[30];    /* Byte 1 = 08: the pedal is up */
static uint8_t idle_payload[SDL_RVOL_INPUT];

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

static void Version(uint8_t *payload, uint32_t type, const char *product, uint8_t major, uint8_t minor, uint8_t revision, const char *date, const char *time)
{
    memset(payload, 0, SDL_ACIO_VERSION_LENGTH);
    payload[0] = (uint8_t)(type >> 24);
    payload[1] = (uint8_t)(type >> 16);
    payload[2] = (uint8_t)(type >> 8);
    payload[3] = (uint8_t)type;
    payload[5] = major;
    payload[6] = minor;
    payload[7] = revision;
    memcpy(payload + 8, product, strlen(product));
    memcpy(payload + 12, date, strlen(date));
    memcpy(payload + 28, time, strlen(time));
}

static SDL_RVOLState *State(Harness *h)
{
    return (SDL_RVOLState *)h->state;
}

static void BuildVectors(void)
{
    static const uint8_t zeros[SDL_RVOL_OUTPUT] = { 0 };
    uint8_t payload[SDL_ACIO_VERSION_LENGTH];

    Version(payload, SDL_ACIO_TYPE_RVOL, "KFCA", 1, 6, 1, "Oct 21 2015", "16:39:52");
    CHECK(Encode(0x81, SDL_ACIO_CMD_GET_VERSION, 2, payload, SDL_ACIO_VERSION_LENGTH, version_reply) == sizeof(version_reply));
    payload[0] = 0x00;
    CHECK(Encode(0x81, SDL_RVOL_CMD_EXPAND, 4, payload, 1, expand_reply) == sizeof(expand_reply));
    CHECK(Encode(0x01, SDL_RVOL_CMD_POLL, 5, zeros, SDL_RVOL_OUTPUT, poll_request) == sizeof(poll_request) && poll_request[38] == 0x39);
    memset(idle_payload, 0, sizeof(idle_payload));
    idle_payload[1] = 0x08;
    CHECK(Encode(0x81, SDL_RVOL_CMD_POLL, 5, idle_payload, SDL_RVOL_INPUT, idle_reply) == sizeof(idle_reply));
}

static size_t Reply(const uint8_t *payload, uint8_t *out)
{
    return Encode(0x81, SDL_RVOL_CMD_POLL, 5, payload, SDL_RVOL_INPUT, out);
}

static void FeedReply(Harness *h, const uint8_t *payload)
{
    uint8_t frame[80];

    H_Feed(h, frame, Reply(payload, frame));
}

/* From an open or a restart: the reset and the bring-up of one RVOL node */
static void BringUpBus(Harness *h)
{
    H_Advance(h, h->now + 2650);
    H_Feed(h, aa, 1);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply, sizeof(version_reply));
    H_Feed(h, start_reply, sizeof(start_reply));
}

static void BringUp(Harness *h)
{
    BringUpBus(h);
    H_Feed(h, expand_reply, sizeof(expand_reply));
    H_Feed(h, idle_reply, sizeof(idle_reply));
}

static Harness *Present(void)
{
    Harness *h = H_Create(&SDL_SerialRVOLModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1 && State(h)->step == SDL_RVOL_STEP_POLL);
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

/* Test 3: the ticket's frames, byte for byte, and the module's */
static void TestTicketFrames(void)
{
    static const uint8_t expand = 0xC0;
    static const uint8_t zeros[SDL_RVOL_OUTPUT] = { 0 };
    uint8_t out[SDL_ACIO_MAX_FRAME];
    size_t n;

    CHECK(Sum(ticket_expand + 1, 6) == 0xD9 && Sum(expand_request + 1, 6) == 0xDB);
    /* bemanitools' mode 1: (1 | 2 x 1) << 6 */
    CHECK(SDL_RVOL_EXPAND == ((1 | (2 * 1)) << 6));
    n = SDL_ACIO_EncodeFrame(0x01, SDL_RVOL_CMD_EXPAND, 2, &expand, 1, out, sizeof(out));
    CHECK(n == sizeof(ticket_expand) && memcmp(out, ticket_expand, n) == 0);
    n = SDL_ACIO_EncodeFrame(0x01, SDL_RVOL_CMD_POLL, 3, zeros, SDL_RVOL_OUTPUT, out, sizeof(out));
    CHECK(n == 39 && memcmp(out, ticket_poll_header, sizeof(ticket_poll_header)) == 0 && out[38] == 0x37);
    CHECK(memcmp(expand_request, ticket_expand, 4) == 0 && memcmp(expand_request + 5, ticket_expand + 5, 2) == 0);
    CHECK(memcmp(poll_request, ticket_poll_header, 4) == 0 && poll_request[5] == ticket_poll_header[5]);
    CHECK(SDL_ACIO_EncodeFrame(0x01, SDL_RVOL_CMD_EXPAND, 4, &expand, 1, out, sizeof(out)) == 8 && memcmp(out, expand_request, 8) == 0);
}

/* Test 3 through the port, from the open */
static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialRVOLModule);
    uint8_t expected[80];
    int i;

    /* The line: 57600 8N1 first, DTR and RTS on, then the reset */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 57600, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(ExpectRestart(h, 0));
    CHECK(State(h)->acio.bus.rate == 57600 && State(h)->acio.bus.alternate_rate == 115200);
    H_Advance(h, 1450);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 1450));
    H_Advance(h, 2650);
    CHECK(H_IsWrite(H_NextCall(h), aa, 1, 2650));
    H_Feed(h, aa, 1);
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, sizeof(enumerate_request), 2650));
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    CHECK(H_IsWrite(H_NextCall(h), version_request, sizeof(version_request), 2650));
    H_Feed(h, version_reply, sizeof(version_reply));
    CHECK(H_IsWrite(H_NextCall(h), start_request, sizeof(start_request), 2650));
    CHECK(strcmp(h->last_log, "ACIO node 1: KFCA, type 09060001, version 1.6.1, Oct 21 2015 16:39:52") == 0);
    /* The expand mode, sequence 4 */
    H_Feed(h, start_reply, sizeof(start_reply));
    CHECK(H_IsWrite(H_NextCall(h), expand_request, sizeof(expand_request), 2650));
    CHECK(State(h)->step == SDL_RVOL_STEP_EXPAND && State(h)->node == 1 && H_NextCall(h) == NULL);
    /* The first poll, sequence 5, and no joystick before its reply */
    H_Feed(h, expand_reply, sizeof(expand_reply));
    CHECK(H_IsWrite(H_NextCall(h), poll_request, sizeof(poll_request), 2650) && State(h)->step == SDL_RVOL_STEP_POLL);
    CHECK(h->npresence == 0);
    /* 3: byte 1 = 08, the pedal released */
    H_Feed(h, idle_reply, sizeof(idle_reply));
    CHECK(h->presence[0] == 1 && h->npresence == 1 && h->npublished == 1);
    CHECK(strcmp(h->identity[0].name, "Konami MUSECA") == 0 && h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN);
    CHECK(h->identity[0].naxes == 5 && h->identity[0].nbuttons == 6 && h->identity[0].nhats == 0 && h->identity[0].nballs == 0);
    CHECK(!h->identity[0].has_mapping && H_NoButtons(h, 0));
    for (i = 0; i < 5; ++i) {
        CHECK(H_Axis(h, 0, i) == -32768);
    }
    CHECK(H_Axis(h, 0, 5) == 0);
    /* The next poll goes out at once, sequence 6 */
    {
        static const uint8_t zeros[SDL_RVOL_OUTPUT] = { 0 };
        const size_t n = Encode(0x01, SDL_RVOL_CMD_POLL, 6, zeros, SDL_RVOL_OUTPUT, expected);

        CHECK(H_IsWrite(H_NextCall(h), expected, n, 2650) && H_NextCall(h) == NULL);
    }
    CHECK(strcmp(SDL_SerialRVOLModule.token, "rvol") == 0 && SDL_SerialRVOLModule.state_size == sizeof(SDL_RVOLState));
    CHECK(!SDL_SerialRVOLModule.rumble && SDL_SerialRVOLModule.effect_max == 0);
    H_Destroy(h);
}

/* Every payload bit changed alone from the pedal-up idle, and every spinner
   position */
static void TestDecode(void)
{
    uint8_t payload[SDL_RVOL_INPUT];
    SDL_SerialControls controls, idle;
    int byte, bit, value, i;

    SDL_RVOL_Decode(idle_payload, &idle);
    for (i = 0; i < 5; ++i) {
        CHECK(idle.axes[i] == -32768);
    }
    CHECK(idle.axes[5] == 0 && !SDL_Serial_GetButton(&idle, SDL_RVOL_BUTTON_PEDAL));
    for (byte = 0; byte < SDL_RVOL_INPUT; ++byte) {
        for (bit = 0; bit < 8; ++bit) {
            SDL_SerialControls want = idle;
            int expected = -1;

            memcpy(payload, idle_payload, sizeof(payload));
            payload[byte] ^= (uint8_t)(1 << bit);
            SDL_RVOL_Decode(payload, &controls);
            if (byte == 9 && bit == 3) {
                expected = SDL_RVOL_BUTTON_SPINNER1;
            } else if (byte == 9 && bit == 0) {
                expected = SDL_RVOL_BUTTON_SPINNER1 + 1;
            } else if (byte == 11 && bit == 3) {
                expected = SDL_RVOL_BUTTON_SPINNER1 + 2;
            } else if (byte == 1 && bit == 2) {
                expected = SDL_RVOL_BUTTON_SPINNER1 + 3;
            } else if (byte == 1 && bit == 4) {
                expected = SDL_RVOL_BUTTON_SPINNER1 + 4;
            } else if (byte == 1 && bit == 3) {
                /* The pedal bit cleared: pressed */
                expected = SDL_RVOL_BUTTON_PEDAL;
            }
            if (expected >= 0) {
                SDL_Serial_SetButton(&want, expected, true);
            } else if (byte >= 16 && byte <= 20) {
                want.axes[SDL_RVOL_AXIS_SPINNER1 + byte - 16] = (int16_t)((1 << bit) * 256 - 32768);
            }
            CHECK(SDL_Serial_ControlsEqual(&controls, &want));
        }
    }
    /* Every spinner position */
    for (value = 0; value < 256; ++value) {
        memcpy(payload, idle_payload, sizeof(payload));
        for (i = 0; i < 5; ++i) {
            payload[16 + i] = (uint8_t)(value ^ (i * 0x33));
        }
        SDL_RVOL_Decode(payload, &controls);
        for (i = 0; i < 5; ++i) {
            CHECK(controls.axes[SDL_RVOL_AXIS_SPINNER1 + i] == (value ^ (i * 0x33)) * 256 - 32768);
        }
    }
    /* 3: bytes 16 to 20 at 00, then FF */
    memcpy(payload, idle_payload, sizeof(payload));
    SDL_RVOL_Decode(payload, &controls);
    for (i = 0; i < 5; ++i) {
        CHECK(controls.axes[i] == -32768);
    }
    memset(payload + 16, 0xFF, 5);
    SDL_RVOL_Decode(payload, &controls);
    for (i = 0; i < 5; ++i) {
        CHECK(controls.axes[i] == 32512);
    }
    /* A zero byte 1 is the pedal down */
    memset(payload, 0, sizeof(payload));
    SDL_RVOL_Decode(payload, &controls);
    CHECK(SDL_Serial_GetButton(&controls, SDL_RVOL_BUTTON_PEDAL));
    for (i = 0; i < SDL_RVOL_BUTTON_PEDAL; ++i) {
        CHECK(!SDL_Serial_GetButton(&controls, i));
    }
    /* Everything at once: every press, the pedal up */
    memset(payload, 0xFF, sizeof(payload));
    SDL_RVOL_Decode(payload, &controls);
    for (i = 0; i < SDL_RVOL_BUTTONS; ++i) {
        CHECK(SDL_Serial_GetButton(&controls, i) == (i != SDL_RVOL_BUTTON_PEDAL));
    }
    CHECK(!SDL_Serial_GetButton(&controls, SDL_RVOL_BUTTONS) && controls.axes[4] == 32512 && controls.axes[5] == 0);
}

/* Test 3's replies through the port */
static void TestReplies(void)
{
    Harness *h = Present();
    uint8_t payload[SDL_RVOL_INPUT];
    int i;

    /* Bytes 16 to 20 at FF */
    memcpy(payload, idle_payload, sizeof(payload));
    memset(payload + 16, 0xFF, 5);
    FeedReply(h, payload);
    for (i = 0; i < 5; ++i) {
        CHECK(H_Axis(h, 0, i) == 32512);
    }
    CHECK(H_NoButtons(h, 0));
    /* The pedal down, then up */
    memcpy(payload, idle_payload, sizeof(payload));
    payload[1] = 0x00;
    FeedReply(h, payload);
    CHECK(H_OnlyButton(h, 0, SDL_RVOL_BUTTON_PEDAL) && H_Axis(h, 0, 0) == -32768);
    payload[1] = 0x08;
    FeedReply(h, payload);
    CHECK(H_NoButtons(h, 0));
    /* Each spinner press */
    {
        static const int bytes[5] = { 9, 9, 11, 1, 1 };
        static const uint8_t masks[5] = { 0x08, 0x01, 0x08, 0x04, 0x10 };

        for (i = 0; i < 5; ++i) {
            memcpy(payload, idle_payload, sizeof(payload));
            payload[bytes[i]] |= masks[i];
            FeedReply(h, payload);
            CHECK(H_OnlyButton(h, 0, SDL_RVOL_BUTTON_SPINNER1 + i));
        }
    }
    /* Every spinner position through the port, AA and FF escaped */
    for (i = 0; i < 256; ++i) {
        memcpy(payload, idle_payload, sizeof(payload));
        payload[16] = (uint8_t)i;
        payload[20] = (uint8_t)(255 - i);
        FeedReply(h, payload);
        CHECK(H_Axis(h, 0, 0) == i * 256 - 32768 && H_Axis(h, 0, 4) == (255 - i) * 256 - 32768);
    }
    CHECK(State(h)->failures == 0 && h->presence[0] == 1 && h->npresence == 1);
    H_Destroy(h);
}

/* Every truncation and split of the expand and poll replies */
static void TestTruncations(void)
{
    static const uint8_t zeros[SDL_RVOL_OUTPUT] = { 0 };
    uint8_t second_poll[80];
    const size_t n_second = Encode(0x01, SDL_RVOL_CMD_POLL, 6, zeros, SDL_RVOL_OUTPUT, second_poll);
    int r;

    for (r = 0; r < 2; ++r) {
        const uint8_t *reply = (r == 0) ? expand_reply : idle_reply;
        const size_t length = (r == 0) ? sizeof(expand_reply) : sizeof(idle_reply);
        const uint8_t *next = (r == 0) ? poll_request : second_poll;
        const size_t next_length = (r == 0) ? sizeof(poll_request) : n_second;
        size_t k;

        for (k = 0; k <= length; ++k) {
            int split;

            for (split = 0; split < 2; ++split) {
                Harness *h = H_Create(&SDL_SerialRVOLModule);

                H_Start(h);
                BringUpBus(h);
                if (r == 1) {
                    H_Feed(h, expand_reply, sizeof(expand_reply));
                }
                H_SkipCalls(h);
                if (split == 0) {
                    H_FeedPadded(h, reply, k);
                    if (k < length) {
                        CHECK(H_NextCall(h) == NULL && h->npresence == 0);
                        H_Feed(h, reply, length);
                    }
                } else if (k > 0 && k < length) {
                    H_Feed(h, reply, k);
                    H_Feed(h, reply + k, length - k);
                } else {
                    size_t b;

                    for (b = 0; b < length; ++b) {
                        H_Feed(h, reply + b, 1);
                    }
                }
                CHECK(H_IsWrite(H_NextCall(h), next, next_length, 2650) && H_NextCall(h) == NULL);
                CHECK(h->presence[0] == (uint32_t)r);
                H_Destroy(h);
            }
        }
    }
}

/* Test 5 */
static void TestBatteryA(void)
{
    uint8_t payload[SDL_RVOL_INPUT + 1];
    uint8_t spin[80], pedal[80], press[80], escaped[80], idle_again[80];
    uint8_t bad_sum[80], bad_command[80], short_reply[80], long_reply[80], cut_ff[80];
    size_t n_spin, n_pedal, n_press, n_escaped, n_idle, n_bad_sum, n_bad_command, n_short, n_long, n_cut_ff;
    H_Vector vectors[8];
    Harness *h;
    int i, count = 0;

    memcpy(payload, idle_payload, SDL_RVOL_INPUT);
    memset(payload + 16, 0x80, 5);
    n_spin = Reply(payload, spin);
    memcpy(payload, idle_payload, SDL_RVOL_INPUT);
    payload[1] = 0x00;
    n_pedal = Reply(payload, pedal);
    memcpy(payload, idle_payload, SDL_RVOL_INPUT);
    payload[9] = 0x09;
    n_press = Reply(payload, press);
    memcpy(payload, idle_payload, SDL_RVOL_INPUT);
    payload[16] = 0xAA;
    payload[17] = 0xFF;
    n_escaped = Reply(payload, escaped);
    n_idle = Reply(idle_payload, idle_again);
    memcpy(payload, idle_payload, SDL_RVOL_INPUT);
    payload[1] = 0x00;
    n_bad_sum = Reply(payload, bad_sum);
    bad_sum[n_bad_sum - 1] ^= 0x01;
    n_bad_command = Encode(0x81, SDL_RVOL_CMD_POLL + 1, 5, payload, SDL_RVOL_INPUT, bad_command);
    n_short = Encode(0x81, SDL_RVOL_CMD_POLL, 5, payload, SDL_RVOL_INPUT - 1, short_reply);
    n_long = Encode(0x81, SDL_RVOL_CMD_POLL, 5, payload, SDL_RVOL_INPUT + 1, long_reply);
    memcpy(payload, idle_payload, SDL_RVOL_INPUT);
    payload[0] = 0xFF;
    Reply(payload, cut_ff);
    CHECK(cut_ff[6] == 0xFF);
    n_cut_ff = 7;

    h = Present();
    H_SetVector(&vectors[count++], "3 spinners at 80", spin, n_spin, 1, 0);
    H_SetVector(&vectors[count++], "pedal down", pedal, n_pedal, 1, 0);
    H_SetVector(&vectors[count++], "spinners 1 and 2 pressed", press, n_press, 1, 0);
    H_SetVector(&vectors[count++], "spinners escaped", escaped, n_escaped, 1, 0);
    H_SetVector(&vectors[count++], "3 pedal up again", idle_again, n_idle, 0, 0);
    for (i = 0; i < count; ++i) {
        H_BatteryA(h, &vectors[i], BringUp);
    }
    H_BatteryA6(h, "wrong checksum", bad_sum, n_bad_sum, &vectors[1]);
    H_BatteryA6(h, "command 0113", bad_command, n_bad_command, &vectors[1]);
    H_BatteryA6(h, "22 bytes", short_reply, n_short, &vectors[1]);
    H_BatteryA6(h, "24 bytes", long_reply, n_long, &vectors[1]);
    H_BatteryA6(h, "cut after FF", cut_ff, n_cut_ff, &vectors[0]);
    for (i = 1; i < (int)n_pedal; ++i) {
        H_BatteryA6(h, "every truncation", pedal, (size_t)i, &vectors[1]);
    }
    CHECK(State(h)->failures == 0);
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
        Version(payload, types[i], "KFCA", 1, 6, 1, "", "");
        H_Feed(h, frame, Encode((uint8_t)(0x81 + i), SDL_ACIO_CMD_GET_VERSION, seq++, payload, SDL_ACIO_VERSION_LENGTH, frame));
    }
    for (i = 0; i < count; ++i) {
        payload[0] = 0x00;
        H_Feed(h, frame, Encode((uint8_t)(0x81 + i), SDL_ACIO_CMD_START, seq++, payload, 1, frame));
    }
}

static void TestNodes(void)
{
    static const uint8_t expand = SDL_RVOL_EXPAND;
    uint8_t expected[16];
    Harness *h;

    /* A KFCA is not an RVOL, though both read KFCA */
    {
        static const uint32_t types[2] = { SDL_ACIO_TYPE_KFCA, SDL_ACIO_TYPE_ICCA };

        h = H_Create(&SDL_SerialRVOLModule);
        H_Start(h);
        BringUpTypes(h, types, 2);
        CHECK(State(h)->step == SDL_RVOL_STEP_IDLE && State(h)->node == 0);
        CHECK(strcmp(h->last_log, "No RVOL node on the bus, so no joystick opens") == 0);
        H_SkipCalls(h);
        H_Advance(h, 60000);
        H_Feed(h, idle_reply, sizeof(idle_reply));
        CHECK(H_NextCall(h) == NULL && h->npresence == 0);
        H_Destroy(h);
    }
    /* Several: the highest, sequence 8 */
    {
        static const uint32_t types[3] = { SDL_ACIO_TYPE_RVOL, SDL_ACIO_TYPE_KFCA, SDL_ACIO_TYPE_RVOL };

        h = H_Create(&SDL_SerialRVOLModule);
        H_Start(h);
        BringUpTypes(h, types, 3);
        h->cursor = h->ncalls - 1;
        CHECK(H_IsWrite(H_NextCall(h), expected, Encode(0x03, SDL_RVOL_CMD_EXPAND, 8, &expand, 1, expected), 2650));
        CHECK(State(h)->node == 3 && State(h)->step == SDL_RVOL_STEP_EXPAND);
        H_Destroy(h);
    }
}

/* The rate no source records: each failed bring-up switches it */
static void TestRates(void)
{
    Harness *h = H_Create(&SDL_SerialRVOLModule);

    H_Start(h);
    H_Advance(h, 2650 + 500 * SDL_ACIO_PROBE_MS - 1);
    H_SkipCalls(h);
    /* 500 unanswered probes at 57600: the line goes to 115200 */
    H_Advance(h, 2650 + 500 * SDL_ACIO_PROBE_MS);
    CHECK(H_IsPlainLine(H_NextCall(h), 115200, 8, SDL_SERIAL_NOPARITY, 1, 7650) && ExpectRestart(h, 7650));
    CHECK(strcmp(h->last_log, "ACIO bus: nothing came up at 57600 baud, so the bus tries 115200") == 0);
    CHECK(State(h)->step == SDL_RVOL_STEP_BUS && State(h)->acio.bus.rate == 115200);
    /* The board answers at 115200 */
    BringUp(h);
    CHECK(h->presence[0] == 1 && State(h)->step == SDL_RVOL_STEP_POLL && h->engine.line.rate == 115200);
    /* Three failed polls start the bus over at the same rate */
    H_SkipCalls(h);
    H_Advance(h, h->now + 3 * SDL_ACIO_REPLY_MS);
    CHECK(h->presence[0] == 0 && State(h)->acio.bus.rate == 115200 && State(h)->acio.bus.restarts == 2);
    CHECK(strcmp(h->last_log, "The RVOL node failed three polls, so the bus starts over") == 0);
    {
        const H_Call *call;
        int lines = 0;

        while ((call = H_NextCall(h)) != NULL) {
            if (call->kind == 'L') {
                ++lines;
            }
        }
        CHECK(lines == 0);
    }
    BringUp(h);
    CHECK(h->presence[0] == 2);
    /* A failed bring-up at 115200 goes back to 57600 */
    H_SkipCalls(h);
    H_Advance(h, h->now + 3 * SDL_ACIO_REPLY_MS);
    H_Advance(h, h->now + 2650);
    H_Feed(h, aa, 1);
    H_SkipCalls(h);
    H_Advance(h, h->now + SDL_ACIO_REPLY_MS);
    CHECK(H_IsPlainLine(H_NextCall(h), 57600, 8, SDL_SERIAL_NOPARITY, 1, h->now) && ExpectRestart(h, h->now));
    CHECK(State(h)->acio.bus.rate == 57600);
    H_Destroy(h);
}

/* Failed commands and polls, and reconnect */
static void TestFailures(void)
{
    uint8_t payload[SDL_ACIO_MAX_PAYLOAD], frame[SDL_ACIO_MAX_FRAME];
    Harness *h;
    int i;

    /* The expand mode with no reply, or another command, starts the bus over */
    for (i = 0; i < 2; ++i) {
        h = H_Create(&SDL_SerialRVOLModule);
        H_Start(h);
        BringUpBus(h);
        CHECK(State(h)->step == SDL_RVOL_STEP_EXPAND);
        H_SkipCalls(h);
        if (i == 0) {
            H_Advance(h, 2650 + SDL_ACIO_REPLY_MS - 1);
            CHECK(State(h)->acio.bus.restarts == 0);
            H_Advance(h, 2650 + SDL_ACIO_REPLY_MS);
        } else {
            H_Feed(h, idle_reply, sizeof(idle_reply));
        }
        CHECK(State(h)->step == SDL_RVOL_STEP_BUS && State(h)->node == 0 && State(h)->acio.bus.restarts == 1);
        CHECK(ExpectRestart(h, h->now) && h->npresence == 0);
        CHECK(strcmp(h->last_log, "The RVOL node did not answer its expand mode, so the bus starts over") == 0);
        H_Destroy(h);
    }
    /* The expand reply may carry any status length */
    {
        static const size_t lengths[4] = { 0, 1, 2, 255 };

        memset(payload, 0x01, sizeof(payload));
        for (i = 0; i < 4; ++i) {
            h = H_Create(&SDL_SerialRVOLModule);
            H_Start(h);
            BringUpBus(h);
            H_Feed(h, frame, Encode(0x81, SDL_RVOL_CMD_EXPAND, 4, payload, lengths[i], frame));
            CHECK(State(h)->step == SDL_RVOL_STEP_POLL && State(h)->acio.bus.restarts == 0);
            H_Destroy(h);
        }
    }
    /* Two failed polls keep the joystick, a reply clears the count, three in
       a row start the bus over */
    h = Present();
    for (i = 1; i <= 2; ++i) {
        static const uint8_t zeros[SDL_RVOL_OUTPUT] = { 0 };
        const uint64_t at = 2650 + SDL_ACIO_REPLY_MS * (uint64_t)i;

        H_Advance(h, at - 1);
        CHECK(State(h)->failures == i - 1 && H_NextCall(h) == NULL);
        H_Advance(h, at);
        CHECK(State(h)->failures == i && h->presence[0] == 1);
        CHECK(H_IsWrite(H_NextCall(h), frame, Encode(0x01, SDL_RVOL_CMD_POLL, (uint8_t)(6 + i), zeros, SDL_RVOL_OUTPUT, frame), at));
    }
    FeedReply(h, idle_payload);
    CHECK(State(h)->failures == 0 && h->presence[0] == 1);
    H_SkipCalls(h);
    H_Advance(h, h->now + 3 * SDL_ACIO_REPLY_MS - 1);
    CHECK(State(h)->failures == 2 && h->presence[0] == 1);
    H_Advance(h, h->now + 1);
    CHECK(h->presence[0] == 0 && State(h)->failures == 0 && State(h)->node == 0 && State(h)->step == SDL_RVOL_STEP_BUS);
    H_Destroy(h);
    /* Replies of another command or length fail a poll */
    h = Present();
    memset(payload, 0, sizeof(payload));
    H_Feed(h, frame, Encode(0x81, SDL_RVOL_CMD_POLL + 1, 5, payload, SDL_RVOL_INPUT, frame));
    H_Feed(h, frame, Encode(0x81, SDL_RVOL_CMD_POLL, 5, payload, SDL_RVOL_INPUT + 1, frame));
    H_Feed(h, frame, Encode(0x81, SDL_RVOL_CMD_POLL, 5, payload, SDL_RVOL_INPUT - 1, frame));
    CHECK(h->presence[0] == 0 && State(h)->acio.bus.restarts == 1);
    H_Destroy(h);
    /* Output requests change nothing, and the port closes at once */
    h = Present();
    {
        SDL_SerialOutput request;

        memset(&request, 0, sizeof(request));
        request.kind = SDL_SERIAL_OUTPUT_RUMBLE;
        SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
        CHECK(H_NextCall(h) == NULL);
    }
    CHECK(!SDL_SerialEngine_BeginStop(&h->engine, H_NS(h->now)) && H_IsCall(H_NextCall(h), 'C', h->now));
    H_Destroy(h);
}

/* Battery B and test 5's reconnect */
static void TestBatteryB(void)
{
    Harness *h = Present();
    uint8_t payload[SDL_RVOL_INPUT];

    memcpy(payload, idle_payload, sizeof(payload));
    payload[1] = 0x00;
    FeedReply(h, payload);
    CHECK(H_Button(h, 0, SDL_RVOL_BUTTON_PEDAL));
    H_Advance(h, 2650 + SDL_ACIO_REPLY_MS);
    CHECK(State(h)->failures == 1);
    H_SkipCalls(h);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 2850));
    /* The reopened port starts the module over, at the first rate */
    H_Advance(h, 3850);
    CHECK(H_ExpectOpened(h, 57600, 8, SDL_SERIAL_NOPARITY, 1, 3850) && ExpectRestart(h, 3850));
    CHECK(State(h)->step == SDL_RVOL_STEP_BUS && State(h)->node == 0 && State(h)->failures == 0);
    H_Advance(h, 3850 + 2650);
    H_SkipCalls(h);
    H_Feed(h, aa, 1);
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, sizeof(enumerate_request), 6500));
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply, sizeof(version_reply));
    H_Feed(h, start_reply, sizeof(start_reply));
    h->cursor = h->ncalls - 1;
    CHECK(H_IsWrite(H_NextCall(h), expand_request, sizeof(expand_request), 6500));
    H_Feed(h, expand_reply, sizeof(expand_reply));
    h->cursor = h->ncalls - 1;
    CHECK(H_IsWrite(H_NextCall(h), poll_request, sizeof(poll_request), 6500));
    H_Feed(h, idle_reply, sizeof(idle_reply));
    CHECK(h->presence[0] == 2 && H_NoButtons(h, 0));
    H_Destroy(h);
}

int main(void)
{
    BuildVectors();
    TestTicketFrames();
    TestStartup();
    TestDecode();
    TestReplies();
    TestTruncations();
    TestBatteryA();
    TestNodes();
    TestRates();
    TestFailures();
    TestBatteryB();
    return H_Finish();
}
