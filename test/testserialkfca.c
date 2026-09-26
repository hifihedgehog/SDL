/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_kfca_proto.c, Konami's
   KFCA board of hifihedgehog/SDL#33 Part 14. The vectors are the ticket's,
   constructed from bemanitools' aciodrv, sdvxio-kfca and ACIO headers, and
   the version reply carries arcade-docs' KFCA entry. Test numbers follow the
   ticket's RS-232 tests. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_kfca_proto.h"

/* The ticket's bytes, test 1. They carry the sequences 2, 3 and 4, where one
   node's bring-up takes 1 to 3, so the module sends the same frames with 4,
   5 and 6. */
static const uint8_t ticket_watchdog[9] = { 0xAA, 0x01, 0x01, 0x20, 0x02, 0x02, 0x17, 0x70, 0xAD };
static const uint8_t ticket_amp[11] = { 0xAA, 0x01, 0x01, 0x28, 0x03, 0x04, 0x00, 0x00, 0x00, 0x00, 0x31 };
static const uint8_t ticket_poll_header[6] = { 0xAA, 0x01, 0x01, 0x13, 0x04, 0x18 };

/* The bus's bring-up, from the framing tests */
static const uint8_t aa[1] = { 0xAA };
static const uint8_t enumerate_request[8] = { 0xAA, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x03 };
static const uint8_t enumerate_reply[8] = { 0xAA, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x04 };
static const uint8_t version_request[7] = { 0xAA, 0x01, 0x00, 0x02, 0x02, 0x00, 0x05 };
static const uint8_t start_request[7] = { 0xAA, 0x01, 0x00, 0x03, 0x03, 0x00, 0x07 };
static const uint8_t start_reply[8] = { 0xAA, 0x81, 0x00, 0x03, 0x03, 0x01, 0x00, 0x88 };

/* The module's */
static const uint8_t watchdog_request[9] = { 0xAA, 0x01, 0x01, 0x20, 0x04, 0x02, 0x17, 0x70, 0xAF };
static const uint8_t amp_request[11] = { 0xAA, 0x01, 0x01, 0x28, 0x05, 0x04, 0x00, 0x00, 0x00, 0x00, 0x33 };
static uint8_t version_reply[51]; /* arcade-docs' KFCA 1.0.5 of Aug 30 2011 */
static uint8_t watchdog_reply[8];
static uint8_t amp_reply[11];
static uint8_t poll_request[31];  /* Sequence 6: AA 01 01 13 06 18, 24 bytes of 00, 33 */
static uint8_t idle_reply[23];

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

/* The low byte of the sum of a frame's body before its checksum */
static uint8_t Sum(const uint8_t *body, size_t length)
{
    uint8_t sum = 0;
    size_t i;

    for (i = 0; i < length; ++i) {
        sum = (uint8_t)(sum + body[i]);
    }
    return sum;
}

/* A version payload */
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

static SDL_KFCAState *State(Harness *h)
{
    return (SDL_KFCAState *)h->state;
}

static void BuildVectors(void)
{
    static const uint8_t zeros[SDL_KFCA_OUTPUT] = { 0 };
    uint8_t payload[SDL_ACIO_VERSION_LENGTH];

    Version(payload, SDL_ACIO_TYPE_KFCA, "KFCA", 1, 0, 5, "Aug 30 2011", "13:23:47");
    CHECK(Encode(0x81, SDL_ACIO_CMD_GET_VERSION, 2, payload, SDL_ACIO_VERSION_LENGTH, version_reply) == sizeof(version_reply));
    payload[0] = 0x00;
    CHECK(Encode(0x81, SDL_KFCA_CMD_WATCHDOG, 4, payload, 1, watchdog_reply) == sizeof(watchdog_reply) && watchdog_reply[7] == 0xA7);
    /* bemanitools' KFCA emulator answers the amplifier with the four bytes */
    CHECK(Encode(0x81, SDL_KFCA_CMD_AMP, 5, zeros, 4, amp_reply) == sizeof(amp_reply));
    CHECK(Encode(0x01, SDL_KFCA_CMD_POLL, 6, zeros, SDL_KFCA_OUTPUT, poll_request) == sizeof(poll_request) && poll_request[30] == 0x33);
    CHECK(Encode(0x81, SDL_KFCA_CMD_POLL, 6, zeros, SDL_KFCA_INPUT, idle_reply) == sizeof(idle_reply));
}

/* A poll reply with the given payload */
static size_t Reply(const uint8_t *payload, uint8_t *out)
{
    return Encode(0x81, SDL_KFCA_CMD_POLL, 6, payload, SDL_KFCA_INPUT, out);
}

static void FeedReply(Harness *h, const uint8_t *payload)
{
    uint8_t frame[64];

    H_Feed(h, frame, Reply(payload, frame));
}

/* From an open or a restart: the reset and the bring-up of one KFCA node */
static void BringUpBus(Harness *h)
{
    H_Advance(h, h->now + 2650);
    H_Feed(h, aa, 1);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply, sizeof(version_reply));
    H_Feed(h, start_reply, sizeof(start_reply));
}

/* The bring-up, the watchdog, the amplifier and the first poll's idle reply */
static void BringUp(Harness *h)
{
    BringUpBus(h);
    H_Feed(h, watchdog_reply, sizeof(watchdog_reply));
    H_Feed(h, amp_reply, sizeof(amp_reply));
    H_Feed(h, idle_reply, sizeof(idle_reply));
}

static Harness *Present(void)
{
    Harness *h = H_Create(&SDL_SerialKFCAModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1 && State(h)->step == SDL_KFCA_STEP_POLL);
    H_SkipCalls(h);
    return h;
}

/* The zeros and the break of a restart at time t */
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

/* Test 1: the ticket's frames, byte for byte, and the module's */
static void TestTicketFrames(void)
{
    static const uint8_t watchdog[2] = { 0x17, 0x70 };
    static const uint8_t zeros[SDL_KFCA_OUTPUT] = { 0 };
    uint8_t out[SDL_ACIO_MAX_FRAME], mine[SDL_ACIO_MAX_FRAME];
    size_t n;

    /* The checksums hold */
    CHECK(Sum(ticket_watchdog + 1, 7) == 0xAD && Sum(ticket_amp + 1, 9) == 0x31);
    CHECK(Sum(watchdog_request + 1, 7) == 0xAF && Sum(amp_request + 1, 9) == 0x33);
    /* The bus's encoder and this file's give the ticket's bytes */
    n = SDL_ACIO_EncodeFrame(0x01, SDL_KFCA_CMD_WATCHDOG, 2, watchdog, 2, out, sizeof(out));
    CHECK(n == sizeof(ticket_watchdog) && memcmp(out, ticket_watchdog, n) == 0);
    CHECK(Encode(0x01, SDL_KFCA_CMD_WATCHDOG, 2, watchdog, 2, mine) == n && memcmp(mine, out, n) == 0);
    n = SDL_ACIO_EncodeFrame(0x01, SDL_KFCA_CMD_AMP, 3, zeros, SDL_KFCA_AMP_LENGTH, out, sizeof(out));
    CHECK(n == sizeof(ticket_amp) && memcmp(out, ticket_amp, n) == 0);
    n = SDL_ACIO_EncodeFrame(0x01, SDL_KFCA_CMD_POLL, 4, zeros, SDL_KFCA_OUTPUT, out, sizeof(out));
    CHECK(n == 31 && memcmp(out, ticket_poll_header, sizeof(ticket_poll_header)) == 0);
    CHECK(Sum(out + 1, 29) == out[30] && out[30] == 0x31);
    /* The module's frames differ from the ticket's in the sequence and the
       checksum only */
    CHECK(memcmp(watchdog_request, ticket_watchdog, 4) == 0 && memcmp(watchdog_request + 5, ticket_watchdog + 5, 3) == 0);
    CHECK(memcmp(amp_request, ticket_amp, 4) == 0 && memcmp(amp_request + 5, ticket_amp + 5, 5) == 0);
    CHECK(memcmp(poll_request, ticket_poll_header, 4) == 0 && poll_request[5] == ticket_poll_header[5]);
    CHECK(SDL_ACIO_EncodeFrame(0x01, SDL_KFCA_CMD_WATCHDOG, 4, watchdog, 2, out, sizeof(out)) == 9 && memcmp(out, watchdog_request, 9) == 0);
    CHECK(SDL_ACIO_EncodeFrame(0x01, SDL_KFCA_CMD_AMP, 5, zeros, 4, out, sizeof(out)) == 11 && memcmp(out, amp_request, 11) == 0);
    /* The watchdog word is 6000 */
    CHECK(SDL_KFCA_WATCHDOG == 6000 && (SDL_KFCA_WATCHDOG >> 8) == 0x17 && (SDL_KFCA_WATCHDOG & 0xFF) == 0x70);
}

/* Test 1 through the port, from the open */
static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialKFCAModule);
    uint8_t expected[64];
    int i;

    /* The line: 57600 8N1, DTR and RTS on, then the reset */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 57600, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(ExpectRestart(h, 0));
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
    CHECK(strcmp(h->last_log, "ACIO node 1: KFCA, type 09060000, version 1.0.5, Aug 30 2011 13:23:47") == 0);
    CHECK(State(h)->step == SDL_KFCA_STEP_BUS && State(h)->node == 0);
    /* The watchdog, sequence 4 */
    H_Feed(h, start_reply, sizeof(start_reply));
    CHECK(H_IsWrite(H_NextCall(h), watchdog_request, sizeof(watchdog_request), 2650));
    CHECK(State(h)->step == SDL_KFCA_STEP_WATCHDOG && State(h)->node == 1 && H_NextCall(h) == NULL);
    /* The amplifier, sequence 5 */
    H_Feed(h, watchdog_reply, sizeof(watchdog_reply));
    CHECK(H_IsWrite(H_NextCall(h), amp_request, sizeof(amp_request), 2650) && State(h)->step == SDL_KFCA_STEP_AMP);
    /* The first poll, sequence 6, and no joystick before its reply */
    H_Feed(h, amp_reply, sizeof(amp_reply));
    CHECK(H_IsWrite(H_NextCall(h), poll_request, sizeof(poll_request), 2650) && State(h)->step == SDL_KFCA_STEP_POLL);
    CHECK(h->npresence == 0 && h->npublished == 0);
    H_Feed(h, idle_reply, sizeof(idle_reply));
    CHECK(h->presence[0] == 1 && h->npresence == 1 && h->npublished == 1);
    /* The BIO2's SOUND VOLTEX joystick */
    CHECK(strcmp(h->identity[0].name, "Konami SOUND VOLTEX") == 0 && h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN);
    CHECK(h->identity[0].naxes == SDL_BIO2_SDVX_AXES && h->identity[0].nbuttons == SDL_BIO2_SDVX_BUTTONS);
    CHECK(h->identity[0].naxes == 2 && h->identity[0].nbuttons == 14 && h->identity[0].nhats == 0 && h->identity[0].nballs == 0);
    CHECK(!h->identity[0].has_mapping);
    CHECK(H_Axis(h, 0, 0) == -32768 && H_Axis(h, 0, 1) == -32768 && H_NoButtons(h, 0));
    for (i = 2; i < SDL_SERIAL_MAX_AXES; ++i) {
        CHECK(H_Axis(h, 0, i) == 0);
    }
    /* The next poll goes out at once, sequence 7 */
    {
        static const uint8_t zeros[SDL_KFCA_OUTPUT] = { 0 };
        const size_t n = Encode(0x01, SDL_KFCA_CMD_POLL, 7, zeros, SDL_KFCA_OUTPUT, expected);

        CHECK(H_IsWrite(H_NextCall(h), expected, n, 2650) && H_NextCall(h) == NULL);
    }
    CHECK(strcmp(SDL_SerialKFCAModule.token, "kfca") == 0 && SDL_SerialKFCAModule.state_size == sizeof(SDL_KFCAState));
    CHECK(!SDL_SerialKFCAModule.rumble && SDL_SerialKFCAModule.effect_min == 0 && SDL_SerialKFCAModule.effect_max == 0);
    H_Destroy(h);
}

/* Every payload bit alone, every knob position, and test 1's values */
static void TestDecode(void)
{
    uint8_t payload[SDL_KFCA_INPUT];
    SDL_SerialControls controls, idle;
    int byte, bit, value;

    CHECK(SDL_KFCA_SYS_COIN == 0x04 && SDL_KFCA_SYS_SERVICE == 0x10 && SDL_KFCA_SYS_TEST == 0x20);
    memset(payload, 0, sizeof(payload));
    SDL_KFCA_Decode(payload, &idle);
    CHECK(idle.axes[0] == -32768 && idle.axes[1] == -32768 && idle.axes[2] == 0);
    for (byte = 0; byte < SDL_KFCA_INPUT; ++byte) {
        for (bit = 0; bit < 8; ++bit) {
            SDL_SerialControls want = idle;
            int expected = -1;

            memset(payload, 0, sizeof(payload));
            payload[byte] = (uint8_t)(1 << bit);
            SDL_KFCA_Decode(payload, &controls);
            if (byte == 9 && bit <= 5) {
                static const int buttons[6] = {
                    SDL_BIO2_SDVX_BUTTON_BT_C, SDL_BIO2_SDVX_BUTTON_BT_B, SDL_BIO2_SDVX_BUTTON_BT_A,
                    SDL_BIO2_SDVX_BUTTON_START, SDL_BIO2_SDVX_BUTTON_RECORDER, SDL_BIO2_SDVX_BUTTON_HEADPHONE
                };
                expected = buttons[bit];
            } else if (byte == 11 && bit >= 3 && bit <= 5) {
                static const int buttons[3] = { SDL_BIO2_SDVX_BUTTON_FX_R, SDL_BIO2_SDVX_BUTTON_FX_L, SDL_BIO2_SDVX_BUTTON_BT_D };
                expected = buttons[bit - 3];
            } else if (byte == 1 && bit == 2) {
                expected = SDL_BIO2_SDVX_BUTTON_COIN;
            } else if (byte == 1 && bit == 4) {
                expected = SDL_BIO2_SDVX_BUTTON_SERVICE;
            } else if (byte == 1 && bit == 5) {
                expected = SDL_BIO2_SDVX_BUTTON_TEST;
            }
            if (expected >= 0) {
                SDL_Serial_SetButton(&want, expected, true);
            } else if (byte == 0 || byte == 2) {
                /* Knob bits 9 to 2 */
                want.axes[byte / 2] = (int16_t)((1 << (bit + 2)) * 64 - 32768);
            } else if ((byte == 1 || byte == 3) && bit >= 6) {
                /* Knob bits 1 and 0 */
                want.axes[byte / 2] = (int16_t)((1 << (bit - 6)) * 64 - 32768);
            }
            CHECK(SDL_Serial_ControlsEqual(&controls, &want));
        }
    }
    /* Every knob position, the low six bits ignored */
    for (value = 0; value < 65536; value += 7) {
        memset(payload, 0, sizeof(payload));
        payload[0] = (uint8_t)(value >> 8);
        payload[1] = (uint8_t)value;
        payload[2] = (uint8_t)((65535 - value) >> 8);
        payload[3] = (uint8_t)(65535 - value);
        SDL_KFCA_Decode(payload, &controls);
        CHECK(controls.axes[SDL_BIO2_SDVX_AXIS_KNOB_L] == (value >> 6) * 64 - 32768);
        CHECK(controls.axes[SDL_BIO2_SDVX_AXIS_KNOB_R] == ((65535 - value) >> 6) * 64 - 32768);
    }
    /* 1: FF C4 is knob L at 1023 and Coin */
    memset(payload, 0, sizeof(payload));
    payload[0] = 0xFF;
    payload[1] = 0xC4;
    SDL_KFCA_Decode(payload, &controls);
    CHECK(controls.axes[SDL_BIO2_SDVX_AXIS_KNOB_L] == 1023 * 64 - 32768 && controls.axes[SDL_BIO2_SDVX_AXIS_KNOB_L] == 32704);
    CHECK(SDL_Serial_GetButton(&controls, SDL_BIO2_SDVX_BUTTON_COIN) && !SDL_Serial_GetButton(&controls, SDL_BIO2_SDVX_BUTTON_TEST));
    CHECK(!SDL_Serial_GetButton(&controls, SDL_BIO2_SDVX_BUTTON_SERVICE));
    /* Byte 9 = 04 is BT-A, byte 11 = 20 BT-D */
    memset(payload, 0, sizeof(payload));
    payload[9] = 0x04;
    SDL_KFCA_Decode(payload, &controls);
    CHECK(SDL_Serial_GetButton(&controls, SDL_BIO2_SDVX_BUTTON_BT_A) && SDL_Serial_GetButton(&controls, 0));
    memset(payload, 0, sizeof(payload));
    payload[11] = 0x20;
    SDL_KFCA_Decode(payload, &controls);
    CHECK(SDL_Serial_GetButton(&controls, SDL_BIO2_SDVX_BUTTON_BT_D) && SDL_Serial_GetButton(&controls, 3));
    /* Everything at once: every button but the EX pair, which the KFCA lacks */
    memset(payload, 0xFF, sizeof(payload));
    SDL_KFCA_Decode(payload, &controls);
    for (bit = 0; bit < SDL_BIO2_SDVX_BUTTONS; ++bit) {
        CHECK(SDL_Serial_GetButton(&controls, bit) == (bit != SDL_BIO2_SDVX_BUTTON_EX1 && bit != SDL_BIO2_SDVX_BUTTON_EX2));
    }
    CHECK(!SDL_Serial_GetButton(&controls, SDL_BIO2_SDVX_BUTTONS) && controls.axes[0] == 32704 && controls.axes[1] == 32704);
}

/* Test 1's replies through the port */
static void TestReplies(void)
{
    Harness *h = Present();
    uint8_t payload[SDL_KFCA_INPUT], frame[64];
    size_t n;
    int i;

    memset(payload, 0, sizeof(payload));
    payload[0] = 0xFF;
    payload[1] = 0xC4;
    FeedReply(h, payload);
    CHECK(H_Axis(h, 0, SDL_BIO2_SDVX_AXIS_KNOB_L) == 32704 && H_OnlyButton(h, 0, SDL_BIO2_SDVX_BUTTON_COIN));
    memset(payload, 0, sizeof(payload));
    payload[9] = 0x04;
    FeedReply(h, payload);
    CHECK(H_OnlyButton(h, 0, SDL_BIO2_SDVX_BUTTON_BT_A) && H_Axis(h, 0, SDL_BIO2_SDVX_AXIS_KNOB_L) == -32768);
    memset(payload, 0, sizeof(payload));
    payload[11] = 0x20;
    FeedReply(h, payload);
    CHECK(H_OnlyButton(h, 0, SDL_BIO2_SDVX_BUTTON_BT_D));
    /* Test and Service */
    memset(payload, 0, sizeof(payload));
    payload[1] = 0x20;
    FeedReply(h, payload);
    CHECK(H_OnlyButton(h, 0, SDL_BIO2_SDVX_BUTTON_TEST));
    payload[1] = 0x10;
    FeedReply(h, payload);
    CHECK(H_OnlyButton(h, 0, SDL_BIO2_SDVX_BUTTON_SERVICE));
    /* Knob R with its bytes escaped */
    memset(payload, 0, sizeof(payload));
    payload[2] = 0xAA;
    payload[3] = 0xFF;
    n = Reply(payload, frame);
    CHECK(n == sizeof(idle_reply) + 2 && frame[8] == 0xFF && frame[9] == 0x55 && frame[10] == 0xFF && frame[11] == 0x00);
    H_Feed(h, frame, n);
    CHECK(H_Axis(h, 0, SDL_BIO2_SDVX_AXIS_KNOB_R) == (0xAAFF >> 6) * 64 - 32768 && H_NoButtons(h, 0));
    /* Every mapped bit alone through the port */
    for (i = 0; i < 6; ++i) {
        static const int buttons[6] = {
            SDL_BIO2_SDVX_BUTTON_BT_C, SDL_BIO2_SDVX_BUTTON_BT_B, SDL_BIO2_SDVX_BUTTON_BT_A,
            SDL_BIO2_SDVX_BUTTON_START, SDL_BIO2_SDVX_BUTTON_RECORDER, SDL_BIO2_SDVX_BUTTON_HEADPHONE
        };

        memset(payload, 0, sizeof(payload));
        payload[9] = (uint8_t)(1 << i);
        FeedReply(h, payload);
        CHECK(H_OnlyButton(h, 0, buttons[i]));
    }
    for (i = 3; i <= 5; ++i) {
        static const int buttons[3] = { SDL_BIO2_SDVX_BUTTON_FX_R, SDL_BIO2_SDVX_BUTTON_FX_L, SDL_BIO2_SDVX_BUTTON_BT_D };

        memset(payload, 0, sizeof(payload));
        payload[11] = (uint8_t)(1 << i);
        FeedReply(h, payload);
        CHECK(H_OnlyButton(h, 0, buttons[i - 3]));
    }
    CHECK(State(h)->failures == 0 && h->presence[0] == 1 && h->npresence == 1);
    H_Destroy(h);
}

/* Every truncation of every reply, with stale bytes past the count, sends
   nothing, and every split of it decodes as one read does */
static void TestTruncations(void)
{
    const uint8_t *replies[3];
    size_t lengths[3];
    const uint8_t *nexts[3];
    size_t next_lengths[3];
    uint8_t second_poll[64];
    int r;

    {
        static const uint8_t zeros[SDL_KFCA_OUTPUT] = { 0 };

        next_lengths[2] = Encode(0x01, SDL_KFCA_CMD_POLL, 7, zeros, SDL_KFCA_OUTPUT, second_poll);
    }
    replies[0] = watchdog_reply;
    lengths[0] = sizeof(watchdog_reply);
    nexts[0] = amp_request;
    next_lengths[0] = sizeof(amp_request);
    replies[1] = amp_reply;
    lengths[1] = sizeof(amp_reply);
    nexts[1] = poll_request;
    next_lengths[1] = sizeof(poll_request);
    replies[2] = idle_reply;
    lengths[2] = sizeof(idle_reply);
    nexts[2] = second_poll;
    for (r = 0; r < 3; ++r) {
        size_t k;

        for (k = 0; k <= lengths[r]; ++k) {
            int split;

            for (split = 0; split < 2; ++split) {
                Harness *h = H_Create(&SDL_SerialKFCAModule);
                int i;

                H_Start(h);
                BringUpBus(h);
                for (i = 0; i < r; ++i) {
                    H_Feed(h, replies[i], lengths[i]);
                }
                H_SkipCalls(h);
                if (split == 0) {
                    H_FeedPadded(h, replies[r], k);
                    if (k < lengths[r]) {
                        CHECK(H_NextCall(h) == NULL && h->npresence == 0);
                        H_Feed(h, replies[r], lengths[r]);
                    }
                } else if (k > 0 && k < lengths[r]) {
                    H_Feed(h, replies[r], k);
                    H_Feed(h, replies[r] + k, lengths[r] - k);
                } else {
                    size_t b;

                    for (b = 0; b < lengths[r]; ++b) {
                        H_Feed(h, replies[r] + b, 1);
                    }
                }
                CHECK(H_IsWrite(H_NextCall(h), nexts[r], next_lengths[r], 2650) && H_NextCall(h) == NULL);
                CHECK(h->presence[0] == ((r == 2) ? 1u : 0u));
                H_Destroy(h);
            }
        }
    }
}

/* Test 5 */
static void TestBatteryA(void)
{
    uint8_t payload[SDL_KFCA_INPUT + 1];
    uint8_t knob[64], bt_a[64], bt_d[64], escaped[64], sum_ff[64], idle_again[64];
    uint8_t bad_sum[64], bad_command[64], short_reply[64], long_reply[64], cut_ff[64];
    size_t n_knob, n_bt_a, n_bt_d, n_escaped, n_sum_ff, n_idle, n_bad_sum, n_bad_command, n_short, n_long, n_cut_ff;
    H_Vector vectors[8];
    Harness *h;
    int i, count = 0;

    memset(payload, 0, sizeof(payload));
    payload[0] = 0xFF;
    payload[1] = 0xC4;
    n_knob = Reply(payload, knob);
    memset(payload, 0, sizeof(payload));
    payload[9] = 0x04;
    n_bt_a = Reply(payload, bt_a);
    memset(payload, 0, sizeof(payload));
    payload[11] = 0x20;
    n_bt_d = Reply(payload, bt_d);
    memset(payload, 0, sizeof(payload));
    payload[2] = 0xAA;
    payload[3] = 0xFF;
    n_escaped = Reply(payload, escaped);
    /* 81 + 01 + 13 + 06 + 10 = AB, and 54 more is FF: the checksum travels escaped */
    memset(payload, 0, sizeof(payload));
    payload[9] = 0x14;
    payload[11] = 0x20;
    payload[1] = 0x20;
    n_sum_ff = Reply(payload, sum_ff);
    CHECK(sum_ff[n_sum_ff - 2] == 0xFF && sum_ff[n_sum_ff - 1] == 0x00);
    memset(payload, 0, sizeof(payload));
    n_idle = Reply(payload, idle_again);
    payload[9] = 0x04;
    n_bad_sum = Reply(payload, bad_sum);
    bad_sum[n_bad_sum - 1] ^= 0x01;
    n_bad_command = Encode(0x81, SDL_KFCA_CMD_POLL + 1, 6, payload, SDL_KFCA_INPUT, bad_command);
    n_short = Encode(0x81, SDL_KFCA_CMD_POLL, 6, payload, SDL_KFCA_INPUT - 1, short_reply);
    n_long = Encode(0x81, SDL_KFCA_CMD_POLL, 6, payload, SDL_KFCA_INPUT + 1, long_reply);
    memset(payload, 0, sizeof(payload));
    payload[0] = 0xFF;
    Reply(payload, cut_ff);
    CHECK(cut_ff[6] == 0xFF);
    n_cut_ff = 7;

    h = Present();
    H_SetVector(&vectors[count++], "1 knob L and Coin", knob, n_knob, 1, 0);
    H_SetVector(&vectors[count++], "1 BT-A", bt_a, n_bt_a, 1, 0);
    H_SetVector(&vectors[count++], "1 BT-D", bt_d, n_bt_d, 1, 0);
    H_SetVector(&vectors[count++], "knob R escaped", escaped, n_escaped, 1, 0);
    H_SetVector(&vectors[count++], "checksum FF escaped", sum_ff, n_sum_ff, 1, 0);
    H_SetVector(&vectors[count++], "idle again", idle_again, n_idle, 0, 0);
    for (i = 0; i < count; ++i) {
        H_BatteryA(h, &vectors[i], BringUp);
    }
    H_BatteryA6(h, "wrong checksum", bad_sum, n_bad_sum, &vectors[1]);
    H_BatteryA6(h, "command 0114", bad_command, n_bad_command, &vectors[1]);
    H_BatteryA6(h, "15 bytes", short_reply, n_short, &vectors[1]);
    H_BatteryA6(h, "17 bytes", long_reply, n_long, &vectors[1]);
    H_BatteryA6(h, "cut after FF", cut_ff, n_cut_ff, &vectors[0]);
    for (i = 1; i < (int)n_knob; ++i) {
        H_BatteryA6(h, "every truncation", knob, (size_t)i, &vectors[0]);
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
        /* The RVOL's product code reads KFCA too */
        Version(payload, types[i], (types[i] == SDL_ACIO_TYPE_ICCA) ? "ICCA" : "KFCA", 1, 0, 5, "", "");
        H_Feed(h, frame, Encode((uint8_t)(0x81 + i), SDL_ACIO_CMD_GET_VERSION, seq++, payload, SDL_ACIO_VERSION_LENGTH, frame));
    }
    for (i = 0; i < count; ++i) {
        payload[0] = 0x00;
        H_Feed(h, frame, Encode((uint8_t)(0x81 + i), SDL_ACIO_CMD_START, seq++, payload, 1, frame));
    }
}

static void TestNodes(void)
{
    static const uint8_t watchdog[2] = { 0x17, 0x70 };
    uint8_t expected[16];
    Harness *h;

    /* No KFCA node: an RVOL whose product reads KFCA is not one */
    {
        static const uint32_t types[2] = { SDL_ACIO_TYPE_RVOL, SDL_ACIO_TYPE_ICCA };

        h = H_Create(&SDL_SerialKFCAModule);
        H_Start(h);
        BringUpTypes(h, types, 2);
        CHECK(State(h)->step == SDL_KFCA_STEP_IDLE && State(h)->node == 0);
        CHECK(strcmp(h->last_log, "No KFCA node on the bus, so no joystick opens") == 0);
        H_SkipCalls(h);
        H_Advance(h, 60000);
        H_Feed(h, idle_reply, sizeof(idle_reply));
        CHECK(H_NextCall(h) == NULL && h->npresence == 0);
        H_Destroy(h);
    }
    /* Several: the highest takes the watchdog, sequence 8 */
    {
        static const uint32_t types[3] = { SDL_ACIO_TYPE_KFCA, SDL_ACIO_TYPE_ICCA, SDL_ACIO_TYPE_KFCA };

        h = H_Create(&SDL_SerialKFCAModule);
        H_Start(h);
        BringUpTypes(h, types, 3);
        h->cursor = h->ncalls - 1;
        CHECK(H_IsWrite(H_NextCall(h), expected, Encode(0x03, SDL_KFCA_CMD_WATCHDOG, 8, watchdog, 2, expected), 2650));
        CHECK(State(h)->node == 3 && State(h)->step == SDL_KFCA_STEP_WATCHDOG);
        H_Destroy(h);
    }
    /* A KFCA beside an RVOL */
    {
        static const uint32_t types[2] = { SDL_ACIO_TYPE_KFCA, SDL_ACIO_TYPE_RVOL };

        h = H_Create(&SDL_SerialKFCAModule);
        H_Start(h);
        BringUpTypes(h, types, 2);
        CHECK(State(h)->node == 1 && State(h)->step == SDL_KFCA_STEP_WATCHDOG);
        H_Destroy(h);
    }
}

/* Failed commands and polls, and reconnect */
static void TestFailures(void)
{
    uint8_t payload[SDL_ACIO_MAX_PAYLOAD], frame[SDL_ACIO_MAX_FRAME];
    Harness *h;
    int i;

    /* The watchdog or the amplifier with no reply, or another command,
       starts the bus over */
    for (i = 0; i < 4; ++i) {
        const bool amp = (i >= 2);

        h = H_Create(&SDL_SerialKFCAModule);
        H_Start(h);
        BringUpBus(h);
        if (amp) {
            H_Feed(h, watchdog_reply, sizeof(watchdog_reply));
        }
        CHECK(State(h)->step == (amp ? SDL_KFCA_STEP_AMP : SDL_KFCA_STEP_WATCHDOG));
        H_SkipCalls(h);
        if ((i % 2) == 0) {
            H_Advance(h, 2650 + SDL_ACIO_REPLY_MS - 1);
            CHECK(State(h)->acio.bus.restarts == 0);
            H_Advance(h, 2650 + SDL_ACIO_REPLY_MS);
        } else {
            H_Feed(h, idle_reply, sizeof(idle_reply));
        }
        CHECK(State(h)->step == SDL_KFCA_STEP_BUS && State(h)->node == 0 && State(h)->acio.bus.restarts == 1);
        CHECK(ExpectRestart(h, h->now) && h->npresence == 0);
        if ((i % 2) == 0) {
            CHECK(strcmp(h->last_log, amp ? "The KFCA node did not answer the amplifier command, so the bus starts over"
                                          : "The KFCA node did not answer its watchdog command, so the bus starts over") == 0);
        }
        H_Destroy(h);
    }

    /* The watchdog and amplifier replies may carry any status length */
    {
        static const size_t lengths[5] = { 0, 1, 2, 4, 255 };

        memset(payload, 0x01, sizeof(payload));
        for (i = 0; i < 5; ++i) {
            h = H_Create(&SDL_SerialKFCAModule);
            H_Start(h);
            BringUpBus(h);
            H_Feed(h, frame, Encode(0x81, SDL_KFCA_CMD_WATCHDOG, 4, payload, lengths[i], frame));
            CHECK(State(h)->step == SDL_KFCA_STEP_AMP);
            H_Feed(h, frame, Encode(0x81, SDL_KFCA_CMD_AMP, 5, payload, lengths[i], frame));
            CHECK(State(h)->step == SDL_KFCA_STEP_POLL && State(h)->acio.bus.restarts == 0);
            H_Destroy(h);
        }
    }

    /* Two failed polls in a row keep the joystick, and each polls again at
       once with the next sequence */
    h = Present();
    for (i = 1; i <= 2; ++i) {
        static const uint8_t zeros[SDL_KFCA_OUTPUT] = { 0 };
        const uint64_t at = 2650 + SDL_ACIO_REPLY_MS * (uint64_t)i;

        H_Advance(h, at - 1);
        CHECK(State(h)->failures == i - 1 && H_NextCall(h) == NULL);
        H_Advance(h, at);
        CHECK(State(h)->failures == i && h->presence[0] == 1);
        CHECK(H_IsWrite(H_NextCall(h), frame, Encode(0x01, SDL_KFCA_CMD_POLL, (uint8_t)(7 + i), zeros, SDL_KFCA_OUTPUT, frame), at));
    }
    memset(payload, 0, SDL_KFCA_INPUT);
    payload[9] = 0x08;
    FeedReply(h, payload);
    CHECK(State(h)->failures == 0 && H_OnlyButton(h, 0, SDL_BIO2_SDVX_BUTTON_START));
    H_Destroy(h);

    /* Three start the bus over and the joystick goes */
    h = Present();
    H_Advance(h, 2650 + 2 * SDL_ACIO_REPLY_MS);
    H_SkipCalls(h);
    H_Advance(h, 2650 + 3 * SDL_ACIO_REPLY_MS - 1);
    CHECK(h->presence[0] == 1 && State(h)->failures == 2);
    H_Advance(h, 2650 + 3 * SDL_ACIO_REPLY_MS);
    CHECK(h->presence[0] == 0 && State(h)->step == SDL_KFCA_STEP_BUS && State(h)->failures == 0 && State(h)->node == 0);
    CHECK(ExpectRestart(h, 3250));
    CHECK(strcmp(h->last_log, "The KFCA node failed three polls, so the bus starts over") == 0);
    /* The bus comes back, the sequence from 1, as a new joystick */
    BringUp(h);
    CHECK(h->presence[0] == 2 && State(h)->acio.bus.restarts == 1 && State(h)->step == SDL_KFCA_STEP_POLL);
    H_Destroy(h);

    /* Replies of another command or length fail a poll too */
    h = Present();
    memset(payload, 0, sizeof(payload));
    H_Feed(h, frame, Encode(0x81, SDL_KFCA_CMD_POLL + 1, 6, payload, SDL_KFCA_INPUT, frame));
    H_Feed(h, frame, Encode(0x81, SDL_KFCA_CMD_POLL, 6, payload, SDL_KFCA_INPUT + 1, frame));
    CHECK(State(h)->failures == 2 && h->presence[0] == 1);
    H_Feed(h, frame, Encode(0x81, SDL_KFCA_CMD_POLL, 6, payload, SDL_KFCA_INPUT - 1, frame));
    CHECK(State(h)->failures == 0 && h->presence[0] == 0 && State(h)->acio.bus.restarts == 1);
    H_Destroy(h);

    /* Output requests change nothing */
    h = Present();
    {
        SDL_SerialOutput request;

        memset(&request, 0, sizeof(request));
        request.kind = SDL_SERIAL_OUTPUT_RUMBLE;
        request.low_frequency_rumble = 0xFFFF;
        SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
        request.kind = SDL_SERIAL_OUTPUT_EFFECT;
        request.length = 1;
        SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
        CHECK(H_NextCall(h) == NULL);
    }
    /* No reset when the port closes: the board does not stream */
    CHECK(!State(h)->acio.base.close_sequence);
    CHECK(!SDL_SerialEngine_BeginStop(&h->engine, H_NS(h->now)) && H_IsCall(H_NextCall(h), 'C', h->now) && H_NextCall(h) == NULL);
    H_Destroy(h);
}

/* Battery B and test 5's reconnect: the port goes and comes back as a new
   joystick, the sequence from 1 */
static void TestBatteryB(void)
{
    Harness *h = Present();
    uint8_t payload[SDL_KFCA_INPUT];

    memset(payload, 0, sizeof(payload));
    payload[9] = 0x08;
    FeedReply(h, payload);
    CHECK(H_Button(h, 0, SDL_BIO2_SDVX_BUTTON_START));
    H_SkipCalls(h);
    H_Advance(h, 2650 + SDL_ACIO_REPLY_MS);
    CHECK(State(h)->failures == 1);
    H_SkipCalls(h);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 2850));
    /* The reopened port starts the module over */
    H_Advance(h, 3850);
    CHECK(H_ExpectOpened(h, 57600, 8, SDL_SERIAL_NOPARITY, 1, 3850) && ExpectRestart(h, 3850));
    CHECK(State(h)->step == SDL_KFCA_STEP_BUS && State(h)->node == 0 && State(h)->failures == 0);
    H_Advance(h, 3850 + 2650);
    H_SkipCalls(h);
    H_Feed(h, aa, 1);
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, sizeof(enumerate_request), 6500));
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply, sizeof(version_reply));
    H_Feed(h, start_reply, sizeof(start_reply));
    h->cursor = h->ncalls - 1;
    CHECK(H_IsWrite(H_NextCall(h), watchdog_request, sizeof(watchdog_request), 6500));
    H_Feed(h, watchdog_reply, sizeof(watchdog_reply));
    H_Feed(h, amp_reply, sizeof(amp_reply));
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
    TestFailures();
    TestBatteryB();
    return H_Finish();
}
