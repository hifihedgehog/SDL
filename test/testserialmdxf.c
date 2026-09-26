/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_mdxf_proto.c, Konami's
   MDXF stage of DDR A, hifihedgehog/SDL#33 Part 14. The vectors are the
   ticket's, constructed from bemanitools' ACIO headers and p4io-mdxfdrv
   (GPL-3.0, facts only). Test numbers follow the ticket's RS-232 tests. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_mdxf_proto.h"

/* The ticket's bytes, test 4. The polls carry the sequences 2 and 3, where
   the bring-up of two nodes takes 1 to 5, so the module polls with 6 and 7.
   The bus matches a reply by address and command, so the ticket's reply is
   fed as it is. */
static const uint8_t ticket_poll1[7] = { 0xAA, 0x01, 0x01, 0x10, 0x02, 0x00, 0x14 };
static const uint8_t ticket_poll2[7] = { 0xAA, 0x02, 0x01, 0x10, 0x03, 0x00, 0x16 };
static const uint8_t ticket_reply_up[10] = { 0xAA, 0x81, 0x01, 0x10, 0x02, 0x03, 0x10, 0x00, 0x00, 0xA7 };

/* The bus's bring-up of two nodes */
static const uint8_t aa[1] = { 0xAA };
static const uint8_t enumerate_request[8] = { 0xAA, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x03 };
static const uint8_t enumerate_reply[8] = { 0xAA, 0x00, 0x00, 0x01, 0x01, 0x01, 0x02, 0x05 };

/* The module's */
static const uint8_t poll1_request[7] = { 0xAA, 0x01, 0x01, 0x10, 0x06, 0x00, 0x18 };
static const uint8_t poll2_request[7] = { 0xAA, 0x02, 0x01, 0x10, 0x07, 0x00, 0x1A };
static uint8_t version_reply1[51];
static uint8_t version_reply2[51];
static uint8_t start_reply1[8];
static uint8_t start_reply2[8];
static uint8_t idle1[10];
static uint8_t idle2[10];

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

static SDL_MDXFState *State(Harness *h)
{
    return (SDL_MDXFState *)h->state;
}

static void BuildVectors(void)
{
    static const uint8_t zeros[SDL_MDXF_INPUT] = { 0 };
    uint8_t payload[SDL_ACIO_VERSION_LENGTH];

    Version(payload, SDL_ACIO_TYPE_MDXF, "MDXF");
    CHECK(Encode(0x81, SDL_ACIO_CMD_GET_VERSION, 2, payload, SDL_ACIO_VERSION_LENGTH, version_reply1) == sizeof(version_reply1));
    CHECK(Encode(0x82, SDL_ACIO_CMD_GET_VERSION, 3, payload, SDL_ACIO_VERSION_LENGTH, version_reply2) == sizeof(version_reply2));
    CHECK(Encode(0x81, SDL_ACIO_CMD_START, 4, zeros, 1, start_reply1) == sizeof(start_reply1));
    CHECK(Encode(0x82, SDL_ACIO_CMD_START, 5, zeros, 1, start_reply2) == sizeof(start_reply2));
    CHECK(Encode(0x81, SDL_MDXF_CMD_POLL, 6, zeros, SDL_MDXF_INPUT, idle1) == sizeof(idle1));
    CHECK(Encode(0x82, SDL_MDXF_CMD_POLL, 7, zeros, SDL_MDXF_INPUT, idle2) == sizeof(idle2));
}

/* A poll reply of player 1 or 2 */
static size_t Reply(int player, const uint8_t *payload, uint8_t *out)
{
    return Encode((uint8_t)(0x81 + player), SDL_MDXF_CMD_POLL, (uint8_t)(6 + player), payload, SDL_MDXF_INPUT, out);
}

static void FeedReply(Harness *h, int player, const uint8_t *payload)
{
    uint8_t frame[32];

    H_Feed(h, frame, Reply(player, payload, frame));
}

/* From an open or a restart: the reset and the bring-up of two MDXF nodes */
static void BringUpBus(Harness *h)
{
    H_Advance(h, h->now + 2650);
    H_Feed(h, aa, 1);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply1, sizeof(version_reply1));
    H_Feed(h, version_reply2, sizeof(version_reply2));
    H_Feed(h, start_reply1, sizeof(start_reply1));
    H_Feed(h, start_reply2, sizeof(start_reply2));
}

/* Both players present, player 1's poll out */
static void BringUp(Harness *h)
{
    BringUpBus(h);
    H_Feed(h, idle1, sizeof(idle1));
    H_Feed(h, idle2, sizeof(idle2));
}

/* Both players present, player 2's poll out */
static void BringUpP2(Harness *h)
{
    BringUp(h);
    H_Feed(h, idle1, sizeof(idle1));
}

static Harness *Present(void)
{
    Harness *h = H_Create(&SDL_SerialMDXFModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1 && h->presence[1] == 1 && State(h)->step == SDL_MDXF_STEP_POLL && State(h)->player == 0);
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

/* The mapping puts the arrows on the D-pad and nothing else anywhere */
static bool IsStageIdentity(const SDL_SerialIdentity *identity, const char *name)
{
    const SDL_SerialGamepadMap *m = &identity->mapping;
    const SDL_SerialMapInput *others[18];
    int i, n = 0;

    others[n++] = &m->a;
    others[n++] = &m->b;
    others[n++] = &m->x;
    others[n++] = &m->y;
    others[n++] = &m->back;
    others[n++] = &m->guide;
    others[n++] = &m->start;
    others[n++] = &m->leftstick;
    others[n++] = &m->rightstick;
    others[n++] = &m->leftshoulder;
    others[n++] = &m->rightshoulder;
    others[n++] = &m->misc1;
    others[n++] = &m->leftx;
    others[n++] = &m->lefty;
    others[n++] = &m->rightx;
    others[n++] = &m->righty;
    others[n++] = &m->lefttrigger;
    others[n++] = &m->righttrigger;
    for (i = 0; i < n; ++i) {
        if (others[i]->kind != SDL_SERIAL_MAP_NONE) {
            return false;
        }
    }
    return strcmp(identity->name, name) == 0 && identity->type == SDL_SERIAL_TYPE_DANCE_PAD && identity->type == 5 &&
           identity->naxes == 0 && identity->nbuttons == 4 && identity->nhats == 0 && identity->nballs == 0 && identity->has_mapping &&
           m->dpup.kind == SDL_SERIAL_MAP_BUTTON && m->dpup.target == SDL_MDXF_BUTTON_UP &&
           m->dpdown.kind == SDL_SERIAL_MAP_BUTTON && m->dpdown.target == SDL_MDXF_BUTTON_DOWN &&
           m->dpleft.kind == SDL_SERIAL_MAP_BUTTON && m->dpleft.target == SDL_MDXF_BUTTON_LEFT &&
           m->dpright.kind == SDL_SERIAL_MAP_BUTTON && m->dpright.target == SDL_MDXF_BUTTON_RIGHT;
}

/* Test 4: the ticket's frames, byte for byte, and the module's */
static void TestTicketFrames(void)
{
    uint8_t out[32];
    SDL_ACIODecoder decoder;
    SDL_ACIOFrame frame;
    size_t i;
    int frames = 0;

    CHECK(Sum(ticket_poll1 + 1, 5) == 0x14 && Sum(ticket_poll2 + 1, 5) == 0x16 && Sum(ticket_reply_up + 1, 8) == 0xA7);
    CHECK(Sum(poll1_request + 1, 5) == 0x18 && Sum(poll2_request + 1, 5) == 0x1A);
    CHECK(SDL_ACIO_EncodeFrame(0x01, SDL_MDXF_CMD_POLL, 2, NULL, 0, out, sizeof(out)) == 7 && memcmp(out, ticket_poll1, 7) == 0);
    CHECK(SDL_ACIO_EncodeFrame(0x02, SDL_MDXF_CMD_POLL, 3, NULL, 0, out, sizeof(out)) == 7 && memcmp(out, ticket_poll2, 7) == 0);
    CHECK(SDL_ACIO_EncodeFrame(0x01, SDL_MDXF_CMD_POLL, 6, NULL, 0, out, sizeof(out)) == 7 && memcmp(out, poll1_request, 7) == 0);
    CHECK(SDL_ACIO_EncodeFrame(0x02, SDL_MDXF_CMD_POLL, 7, NULL, 0, out, sizeof(out)) == 7 && memcmp(out, poll2_request, 7) == 0);
    CHECK(memcmp(poll1_request, ticket_poll1, 4) == 0 && poll1_request[5] == 0x00 && memcmp(poll2_request, ticket_poll2, 4) == 0);
    /* The reply decodes to node 1, 0110, a 3-byte payload with an up sensor */
    SDL_ACIO_InitDecoder(&decoder);
    for (i = 0; i < sizeof(ticket_reply_up); ++i) {
        if (SDL_ACIO_DecodeByte(&decoder, ticket_reply_up[i], &frame)) {
            ++frames;
        }
    }
    CHECK(frames == 1 && frame.address == 0x81 && frame.command == SDL_MDXF_CMD_POLL && frame.length == 3 && frame.payload[0] == 0x10);
}

/* Test 4 through the port, from the open */
static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialMDXFModule);
    uint8_t expected[16];

    /* The line: 115200 8N1, DTR and RTS on, then the reset */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(ExpectRestart(h, 0));
    CHECK(State(h)->acio.bus.alternate_rate == 0);
    H_Advance(h, 2650);
    H_SkipCalls(h);
    H_Feed(h, aa, 1);
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, sizeof(enumerate_request), 2650));
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply1, sizeof(version_reply1));
    H_Feed(h, version_reply2, sizeof(version_reply2));
    H_Feed(h, start_reply1, sizeof(start_reply1));
    CHECK(State(h)->step == SDL_MDXF_STEP_BUS);
    H_Feed(h, start_reply2, sizeof(start_reply2));
    /* Player 1's node first, sequence 6 */
    h->cursor = h->ncalls - 1;
    CHECK(H_IsWrite(H_NextCall(h), poll1_request, sizeof(poll1_request), 2650) && H_NextCall(h) == NULL);
    CHECK(State(h)->step == SDL_MDXF_STEP_POLL && State(h)->players == 2 && State(h)->nodes[0] == 1 && State(h)->nodes[1] == 2);
    CHECK(h->npresence == 0);
    /* 4: the ticket's reply, as it is, presses player 1's up */
    H_Feed(h, ticket_reply_up, sizeof(ticket_reply_up));
    CHECK(h->presence[0] == 1 && h->presence[1] == 0 && h->npublished == 1);
    CHECK(IsStageIdentity(&h->identity[0], "Konami DDR Stage P1"));
    CHECK(H_OnlyButton(h, 0, SDL_MDXF_BUTTON_UP));
    /* Then player 2's node, sequence 7 */
    CHECK(H_IsWrite(H_NextCall(h), poll2_request, sizeof(poll2_request), 2650) && H_NextCall(h) == NULL);
    H_Feed(h, idle2, sizeof(idle2));
    CHECK(h->presence[1] == 1 && IsStageIdentity(&h->identity[1], "Konami DDR Stage P2") && H_NoButtons(h, 1));
    /* And player 1's again, sequence 8 */
    CHECK(H_IsWrite(H_NextCall(h), expected, Encode(0x01, SDL_MDXF_CMD_POLL, 8, NULL, 0, expected), 2650));
    CHECK(strcmp(SDL_SerialMDXFModule.token, "mdxf") == 0 && SDL_SerialMDXFModule.state_size == sizeof(SDL_MDXFState));
    CHECK(!SDL_SerialMDXFModule.rumble && SDL_SerialMDXFModule.effect_max == 0);
    H_Destroy(h);
}

/* Every bit alone, and jumps */
static void TestDecode(void)
{
    uint8_t payload[SDL_MDXF_INPUT];
    SDL_SerialControls controls, idle;
    int byte, bit, i;

    memset(payload, 0, sizeof(payload));
    SDL_MDXF_Decode(payload, &idle);
    for (i = 0; i < SDL_SERIAL_MAX_AXES; ++i) {
        CHECK(idle.axes[i] == 0);
    }
    for (byte = 0; byte < SDL_MDXF_INPUT; ++byte) {
        for (bit = 0; bit < 8; ++bit) {
            SDL_SerialControls want = idle;

            memset(payload, 0, sizeof(payload));
            payload[byte] = (uint8_t)(1 << bit);
            SDL_MDXF_Decode(payload, &controls);
            if (byte == 0) {
                SDL_Serial_SetButton(&want, (bit < 4) ? SDL_MDXF_BUTTON_DOWN : SDL_MDXF_BUTTON_UP, true);
            } else if (byte == 1) {
                SDL_Serial_SetButton(&want, (bit < 4) ? SDL_MDXF_BUTTON_RIGHT : SDL_MDXF_BUTTON_LEFT, true);
            }
            CHECK(SDL_Serial_ControlsEqual(&controls, &want));
        }
    }
    /* Every value of the two bytes */
    for (i = 0; i < 65536; ++i) {
        memset(payload, 0, sizeof(payload));
        payload[0] = (uint8_t)(i >> 8);
        payload[1] = (uint8_t)i;
        payload[2] = (uint8_t)(i * 7);
        SDL_MDXF_Decode(payload, &controls);
        CHECK(SDL_Serial_GetButton(&controls, SDL_MDXF_BUTTON_UP) == ((payload[0] & 0xF0) != 0));
        CHECK(SDL_Serial_GetButton(&controls, SDL_MDXF_BUTTON_DOWN) == ((payload[0] & 0x0F) != 0));
        CHECK(SDL_Serial_GetButton(&controls, SDL_MDXF_BUTTON_LEFT) == ((payload[1] & 0xF0) != 0));
        CHECK(SDL_Serial_GetButton(&controls, SDL_MDXF_BUTTON_RIGHT) == ((payload[1] & 0x0F) != 0));
        CHECK(!SDL_Serial_GetButton(&controls, SDL_MDXF_BUTTONS));
    }
    /* A jump holds opposite arrows */
    payload[0] = 0x81;
    payload[1] = 0x18;
    payload[2] = 0;
    SDL_MDXF_Decode(payload, &controls);
    for (i = 0; i < SDL_MDXF_BUTTONS; ++i) {
        CHECK(SDL_Serial_GetButton(&controls, i));
    }
}

/* Test 4's replies through the port, both players */
static void TestReplies(void)
{
    Harness *h = Present();
    uint8_t payload[SDL_MDXF_INPUT];
    int player, bit;

    /* 00 20 00: player 1's left */
    payload[0] = 0x00;
    payload[1] = 0x20;
    payload[2] = 0x00;
    FeedReply(h, 0, payload);
    CHECK(H_OnlyButton(h, 0, SDL_MDXF_BUTTON_LEFT) && H_NoButtons(h, 1));
    /* Each of the 16 sensor bits alone, for each player in turn */
    for (bit = 0; bit < 16; ++bit) {
        for (player = 1; player <= 2; ++player) {
            const int p = (player == 1) ? 1 : 0; /* Player 2's poll is out now, then player 1's */
            static const int arrows[4] = { SDL_MDXF_BUTTON_DOWN, SDL_MDXF_BUTTON_UP, SDL_MDXF_BUTTON_RIGHT, SDL_MDXF_BUTTON_LEFT };

            memset(payload, 0, sizeof(payload));
            payload[bit / 8] = (uint8_t)(1 << (bit % 8));
            FeedReply(h, p, payload);
            CHECK(H_OnlyButton(h, p, arrows[bit / 4]));
        }
    }
    /* Byte 2 presses nothing */
    for (player = 0; player < 2; ++player) {
        const int p = (player == 0) ? 1 : 0;

        memset(payload, 0, sizeof(payload));
        payload[2] = 0xFF;
        FeedReply(h, p, payload);
        CHECK(H_NoButtons(h, p));
    }
    /* A reply of the wrong player answers nothing: it waits for its node */
    memset(payload, 0, sizeof(payload));
    payload[0] = 0x10;
    FeedReply(h, 0, payload);
    CHECK(H_OnlyButton(h, 1, SDL_MDXF_BUTTON_UP) == false && State(h)->player == 1);
    FeedReply(h, 1, payload);
    CHECK(H_OnlyButton(h, 1, SDL_MDXF_BUTTON_UP) && H_NoButtons(h, 0) && State(h)->player == 0);
    CHECK(State(h)->failures[0] == 0 && State(h)->failures[1] == 0 && h->npresence == 2);
    H_Destroy(h);
}

/* Every truncation and split of both players' replies */
static void TestTruncations(void)
{
    int p;

    for (p = 0; p < 2; ++p) {
        const uint8_t *reply = (p == 0) ? idle1 : idle2;
        uint8_t next[16];
        const size_t next_length = Encode((uint8_t)(p == 0 ? 0x02 : 0x01), SDL_MDXF_CMD_POLL, (uint8_t)(7 + p), NULL, 0, next);
        size_t k;

        for (k = 0; k <= sizeof(idle1); ++k) {
            int split;

            for (split = 0; split < 2; ++split) {
                Harness *h = H_Create(&SDL_SerialMDXFModule);

                H_Start(h);
                BringUpBus(h);
                if (p == 1) {
                    H_Feed(h, idle1, sizeof(idle1));
                }
                H_SkipCalls(h);
                if (split == 0) {
                    H_FeedPadded(h, reply, k);
                    if (k < sizeof(idle1)) {
                        CHECK(H_NextCall(h) == NULL && h->presence[p] == 0);
                        H_Feed(h, reply, sizeof(idle1));
                    }
                } else if (k > 0 && k < sizeof(idle1)) {
                    H_Feed(h, reply, k);
                    H_Feed(h, reply + k, sizeof(idle1) - k);
                } else {
                    size_t b;

                    for (b = 0; b < sizeof(idle1); ++b) {
                        H_Feed(h, reply + b, 1);
                    }
                }
                CHECK(H_IsWrite(H_NextCall(h), next, next_length, 2650) && H_NextCall(h) == NULL && h->presence[p] == 1);
                H_Destroy(h);
            }
        }
    }
}

/* Test 5 */
static void TestBatteryA(void)
{
    uint8_t payload[SDL_MDXF_INPUT + 1];
    uint8_t up[16], left[16], jump[16], idle_again[16], bad_sum[16], bad_command[16], short_reply[16], long_reply[16], cut_ff[16];
    uint8_t p2_right[16], p2_escaped[16];
    size_t n_up, n_left, n_jump, n_idle, n_bad_sum, n_bad_command, n_short, n_long, n_cut_ff, n_p2_right, n_p2_escaped;
    H_Vector vectors[8];
    Harness *h;
    int i, count = 0;

    memcpy(up, ticket_reply_up, sizeof(ticket_reply_up));
    n_up = sizeof(ticket_reply_up);
    payload[0] = 0x00;
    payload[1] = 0x20;
    payload[2] = 0x00;
    n_left = Reply(0, payload, left);
    payload[0] = 0x11;
    payload[1] = 0x11;
    n_jump = Reply(0, payload, jump);
    memset(payload, 0, sizeof(payload));
    n_idle = Reply(0, payload, idle_again);
    payload[0] = 0x10;
    n_bad_sum = Reply(0, payload, bad_sum);
    bad_sum[n_bad_sum - 1] ^= 0x01;
    n_bad_command = Encode(0x81, 0x0116, 6, payload, SDL_MDXF_INPUT, bad_command);
    n_short = Encode(0x81, SDL_MDXF_CMD_POLL, 6, payload, SDL_MDXF_INPUT - 1, short_reply);
    n_long = Encode(0x81, SDL_MDXF_CMD_POLL, 6, payload, SDL_MDXF_INPUT + 1, long_reply);
    payload[0] = 0xFF;
    Reply(0, payload, cut_ff);
    CHECK(cut_ff[6] == 0xFF);
    n_cut_ff = 7;
    memset(payload, 0, sizeof(payload));
    payload[1] = 0x01;
    n_p2_right = Reply(1, payload, p2_right);
    payload[1] = 0x00;
    payload[0] = 0xAA;
    payload[2] = 0xFF;
    n_p2_escaped = Reply(1, payload, p2_escaped);
    CHECK(n_p2_escaped == n_p2_right + 2);

    h = Present();
    H_SetVector(&vectors[count++], "4 ticket reply, P1 up", up, n_up, 1, 0);
    H_SetVector(&vectors[count++], "4 P1 left", left, n_left, 1, 0);
    H_SetVector(&vectors[count++], "P1 jump", jump, n_jump, 1, 0);
    H_SetVector(&vectors[count++], "P1 idle again", idle_again, n_idle, 0, 0);
    for (i = 0; i < count; ++i) {
        H_BatteryA(h, &vectors[i], BringUp);
    }
    /* A frame the decoder drops leaves player 1's poll out */
    H_BatteryA6(h, "wrong checksum", bad_sum, n_bad_sum, &vectors[0]);
    H_BatteryA6(h, "cut after FF", cut_ff, n_cut_ff, &vectors[0]);
    for (i = 1; i < (int)n_up; ++i) {
        H_BatteryA6(h, "every truncation", up, (size_t)i, &vectors[0]);
    }
    /* A reply of another command or length fails player 1's poll, and
       player 2's is out next */
    H_SetVector(&vectors[4], "P2 right after a failed P1 poll", p2_right, n_p2_right, 1, 0);
    H_BatteryA6(h, "command 0116", bad_command, n_bad_command, &vectors[4]);
    H_BatteryA6(h, "2 bytes", short_reply, n_short, &vectors[4]);
    H_BatteryA6(h, "4 bytes", long_reply, n_long, &vectors[4]);
    H_Destroy(h);

    /* Player 2's replies, with its poll out */
    h = H_Create(&SDL_SerialMDXFModule);
    H_Start(h);
    BringUpP2(h);
    CHECK(State(h)->player == 1);
    H_SetVector(&vectors[0], "P2 right", p2_right, n_p2_right, 1, 0);
    H_SetVector(&vectors[1], "P2 up and down, escaped", p2_escaped, n_p2_escaped, 1, 0);
    for (i = 0; i < 2; ++i) {
        H_BatteryA(h, &vectors[i], BringUpP2);
    }
    /* Player 1's reply now answers nothing */
    H_BatteryA6(h, "player 1's reply", idle_again, n_idle, &vectors[0]);
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
        Version(payload, types[i], (types[i] == SDL_ACIO_TYPE_MDXF) ? "MDXF" : "ICCA");
        H_Feed(h, frame, Encode((uint8_t)(0x81 + i), SDL_ACIO_CMD_GET_VERSION, seq++, payload, SDL_ACIO_VERSION_LENGTH, frame));
    }
    for (i = 0; i < count; ++i) {
        payload[0] = 0x00;
        H_Feed(h, frame, Encode((uint8_t)(0x81 + i), SDL_ACIO_CMD_START, seq++, payload, 1, frame));
    }
}

static void TestNodes(void)
{
    static const uint8_t zeros[SDL_MDXF_INPUT] = { 0 };
    uint8_t expected[16], frame[16];
    Harness *h;
    int i;

    /* No MDXF node */
    {
        static const uint32_t types[1] = { SDL_ACIO_TYPE_ICCA };

        h = H_Create(&SDL_SerialMDXFModule);
        H_Start(h);
        BringUpTypes(h, types, 1);
        CHECK(State(h)->step == SDL_MDXF_STEP_IDLE && State(h)->players == 0);
        CHECK(strcmp(h->last_log, "No MDXF node on the bus, so no joystick opens") == 0);
        H_SkipCalls(h);
        H_Advance(h, 60000);
        H_Feed(h, idle1, sizeof(idle1));
        CHECK(H_NextCall(h) == NULL && h->npresence == 0);
        H_Destroy(h);
    }
    /* One node: player 1 only, polled every time */
    {
        static const uint32_t types[1] = { SDL_ACIO_TYPE_MDXF };

        h = H_Create(&SDL_SerialMDXFModule);
        H_Start(h);
        BringUpTypes(h, types, 1);
        h->cursor = h->ncalls - 1;
        CHECK(H_IsWrite(H_NextCall(h), expected, Encode(0x01, SDL_MDXF_CMD_POLL, 4, NULL, 0, expected), 2650));
        CHECK(State(h)->players == 1 && State(h)->nodes[0] == 1 && State(h)->nodes[1] == 0);
        for (i = 0; i < 3; ++i) {
            H_Feed(h, frame, Encode(0x81, SDL_MDXF_CMD_POLL, (uint8_t)(4 + i), zeros, SDL_MDXF_INPUT, frame));
            CHECK(H_IsWrite(H_NextCall(h), expected, Encode(0x01, SDL_MDXF_CMD_POLL, (uint8_t)(5 + i), NULL, 0, expected), 2650));
        }
        CHECK(h->presence[0] == 1 && h->presence[1] == 0 && h->npresence == 1);
        H_Destroy(h);
    }
    /* Other nodes first: the MDXF nodes in address order */
    {
        static const uint32_t types[3] = { SDL_ACIO_TYPE_ICCA, SDL_ACIO_TYPE_MDXF, SDL_ACIO_TYPE_MDXF };

        h = H_Create(&SDL_SerialMDXFModule);
        H_Start(h);
        BringUpTypes(h, types, 3);
        h->cursor = h->ncalls - 1;
        CHECK(H_IsWrite(H_NextCall(h), expected, Encode(0x02, SDL_MDXF_CMD_POLL, 8, NULL, 0, expected), 2650));
        CHECK(State(h)->players == 2 && State(h)->nodes[0] == 2 && State(h)->nodes[1] == 3);
        H_Feed(h, frame, Encode(0x82, SDL_MDXF_CMD_POLL, 8, zeros, SDL_MDXF_INPUT, frame));
        CHECK(H_IsWrite(H_NextCall(h), expected, Encode(0x03, SDL_MDXF_CMD_POLL, 9, NULL, 0, expected), 2650));
        CHECK(h->presence[0] == 1 && h->presence[1] == 0);
        H_Destroy(h);
    }
    /* Three: the first two, logged */
    {
        static const uint32_t types[3] = { SDL_ACIO_TYPE_MDXF, SDL_ACIO_TYPE_MDXF, SDL_ACIO_TYPE_MDXF };

        h = H_Create(&SDL_SerialMDXFModule);
        H_Start(h);
        BringUpTypes(h, types, 3);
        CHECK(State(h)->players == 2 && State(h)->nodes[0] == 1 && State(h)->nodes[1] == 2);
        CHECK(strcmp(h->last_log, "More than two MDXF nodes on the bus: the first two are players 1 and 2") == 0);
        H_Destroy(h);
    }
    /* Two: no such log */
    h = Present();
    CHECK(strcmp(h->last_log, "More than two MDXF nodes on the bus: the first two are players 1 and 2") != 0);
    H_Destroy(h);
}

/* Each node counts its own failed polls */
static void TestFailures(void)
{
    uint8_t payload[SDL_ACIO_MAX_PAYLOAD], frame[SDL_ACIO_MAX_FRAME];
    Harness *h;
    int i;

    /* Player 2's node fails three polls in a row while player 1's answers */
    h = Present();
    for (i = 1; i <= 3; ++i) {
        H_Feed(h, idle1, sizeof(idle1));
        CHECK(State(h)->player == 1 && State(h)->failures[0] == 0);
        H_Advance(h, h->now + SDL_ACIO_REPLY_MS - 1);
        CHECK(State(h)->failures[1] == i - 1 && h->presence[1] == 1);
        H_Advance(h, h->now + 1);
        if (i < 3) {
            CHECK(State(h)->failures[1] == i && State(h)->player == 0 && h->presence[1] == 1);
        }
    }
    CHECK(h->presence[0] == 0 && h->presence[1] == 0 && State(h)->step == SDL_MDXF_STEP_BUS);
    CHECK(State(h)->players == 0 && State(h)->player == 0 && State(h)->nodes[0] == 0 && State(h)->nodes[1] == 0);
    CHECK(State(h)->failures[0] == 0 && State(h)->failures[1] == 0 && State(h)->acio.bus.restarts == 1);
    CHECK(strcmp(h->last_log, "An MDXF node failed three polls, so the bus starts over") == 0);
    /* The bus comes back with new joysticks */
    BringUp(h);
    CHECK(h->presence[0] == 2 && h->presence[1] == 2);
    H_Destroy(h);

    /* Failures of one node alternate with answers: nothing starts over, and
       an answer clears that node's count only */
    h = Present();
    for (i = 0; i < 10; ++i) {
        H_Advance(h, h->now + SDL_ACIO_REPLY_MS);
        CHECK(State(h)->failures[0] == 1 && State(h)->player == 1);
        H_Feed(h, idle2, sizeof(idle2));
        CHECK(State(h)->failures[0] == 1 && State(h)->failures[1] == 0 && State(h)->player == 0);
        H_Feed(h, idle1, sizeof(idle1));
        CHECK(State(h)->failures[0] == 0);
        H_Feed(h, idle2, sizeof(idle2));
    }
    CHECK(State(h)->acio.bus.restarts == 0 && h->presence[0] == 1 && h->presence[1] == 1);
    H_Destroy(h);

    /* Player 1's node fails twice, then answers, then fails twice more */
    h = Present();
    H_Advance(h, h->now + SDL_ACIO_REPLY_MS);
    H_Feed(h, idle2, sizeof(idle2));
    H_Advance(h, h->now + SDL_ACIO_REPLY_MS);
    CHECK(State(h)->failures[0] == 2);
    H_Feed(h, idle2, sizeof(idle2));
    H_Feed(h, idle1, sizeof(idle1));
    CHECK(State(h)->failures[0] == 0);
    H_Feed(h, idle2, sizeof(idle2));
    H_Advance(h, h->now + SDL_ACIO_REPLY_MS);
    H_Feed(h, idle2, sizeof(idle2));
    H_Advance(h, h->now + SDL_ACIO_REPLY_MS);
    CHECK(State(h)->failures[0] == 2 && State(h)->acio.bus.restarts == 0);
    H_Destroy(h);

    /* Replies of another command or length fail a poll */
    h = Present();
    memset(payload, 0, sizeof(payload));
    H_Feed(h, frame, Encode(0x81, 0x0116, 6, payload, SDL_MDXF_INPUT, frame));
    CHECK(State(h)->failures[0] == 1 && State(h)->player == 1);
    H_Feed(h, frame, Encode(0x82, SDL_MDXF_CMD_POLL, 7, payload, SDL_MDXF_INPUT + 1, frame));
    CHECK(State(h)->failures[1] == 1 && State(h)->player == 0);
    H_Feed(h, frame, Encode(0x81, SDL_MDXF_CMD_POLL, 6, payload, SDL_MDXF_INPUT - 1, frame));
    CHECK(State(h)->failures[0] == 2);
    H_Feed(h, frame, Encode(0x82, SDL_MDXF_CMD_POLL, 7, payload, 0, frame));
    CHECK(State(h)->failures[1] == 2);
    H_Feed(h, frame, Encode(0x81, SDL_MDXF_CMD_POLL, 6, payload, SDL_MDXF_INPUT + 1, frame));
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

/* The automatic mode never goes out */
static void TestNoAutoMode(void)
{
    Harness *h = Present();
    int i, writes = 0, autos = 0;

    for (i = 0; i < 50; ++i) {
        H_Feed(h, idle1, sizeof(idle1));
        H_Feed(h, idle2, sizeof(idle2));
    }
    H_Advance(h, h->now + 5000);
    for (i = 0; i < h->ncalls; ++i) {
        const H_Call *call = &h->calls[i];

        if (call->kind == 'W' && call->length >= 4 && call->data[0] == 0xAA) {
            ++writes;
            if (call->data[2] == 0x01 && call->data[3] == 0x16) {
                ++autos;
            }
        }
    }
    CHECK(writes > 100 && autos == 0);
    H_Destroy(h);
}

/* Battery B and test 5's reconnect */
static void TestBatteryB(void)
{
    Harness *h = Present();
    uint8_t payload[SDL_MDXF_INPUT];

    payload[0] = 0x01;
    payload[1] = 0x00;
    payload[2] = 0x00;
    FeedReply(h, 0, payload);
    CHECK(H_Button(h, 0, SDL_MDXF_BUTTON_DOWN));
    H_Advance(h, 2650 + SDL_ACIO_REPLY_MS);
    CHECK(State(h)->failures[1] == 1 && State(h)->player == 0);
    H_SkipCalls(h);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && h->presence[1] == 0 && H_IsCall(H_NextCall(h), 'C', 2850));
    /* The reopened port starts the module over */
    H_Advance(h, 3850);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 3850) && ExpectRestart(h, 3850));
    CHECK(State(h)->step == SDL_MDXF_STEP_BUS && State(h)->players == 0 && State(h)->failures[1] == 0);
    H_Advance(h, 3850 + 2650);
    H_SkipCalls(h);
    H_Feed(h, aa, 1);
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, sizeof(enumerate_request), 6500));
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply1, sizeof(version_reply1));
    H_Feed(h, version_reply2, sizeof(version_reply2));
    H_Feed(h, start_reply1, sizeof(start_reply1));
    H_Feed(h, start_reply2, sizeof(start_reply2));
    h->cursor = h->ncalls - 1;
    CHECK(H_IsWrite(H_NextCall(h), poll1_request, sizeof(poll1_request), 6500));
    H_Feed(h, idle1, sizeof(idle1));
    H_Feed(h, idle2, sizeof(idle2));
    CHECK(h->presence[0] == 2 && h->presence[1] == 2 && H_NoButtons(h, 0));
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
    TestFailures();
    TestNoAutoMode();
    TestBatteryB();
    return H_Finish();
}
