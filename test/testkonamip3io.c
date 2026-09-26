/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_konami_p3io_proto.c, the
   Konami P3IO of hifihedgehog/SDL#33 Part 14, and for the board's vendor
   rule in src/hidapi/SDL_hidapi_vendorusb.c. Test numbers follow the part.
   The frames and reports are constructed from bemanitools and OpenITG. The
   version reply's payload, G32 2.2.6, is the one OpenITG's source records
   from a board. */

#include "../src/joystick/hidapi/SDL_hidapi_konami_p3io_proto.h"
#include "../src/hidapi/SDL_hidapi_vendorusb.h"
#include "../src/joystick/usb_ids.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(condition)                                                      \
    do {                                                                      \
        ++checks;                                                             \
        if (!(condition)) {                                                   \
            ++failures;                                                       \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
        }                                                                     \
    } while (0)

#define ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))

/* Feeds bytes from an exact-size heap copy, so a read past the length
   reaches the sanitizer */
static void Receive(SDL_P3IOSession *session, const uint8_t *data, size_t length, uint64_t now)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);

    if (length) {
        memcpy(copy, data, length);
    }
    SDL_P3IO_SessionReceive(session, copy, length, now);
    free(copy);
}

static bool Feed(SDL_P3IOInput *input, const uint8_t *data, size_t length, SDL_P3IOState *state)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, data, length);
    }
    result = SDL_P3IO_InputFeed(input, copy, length, state);
    free(copy);
    return result;
}

static size_t Poll(SDL_P3IOSession *session, uint64_t now, uint8_t *out)
{
    return SDL_P3IO_SessionPoll(session, now, out, SDL_P3IO_REQUEST_SIZE);
}

/* A frame as the board would send it, built by hand: AA, then each body
   byte, escaped when it is AA or FF */
static size_t Frame(const uint8_t *body, size_t body_length, uint8_t *out)
{
    size_t i, length = 0;

    out[length++] = 0xAA;
    for (i = 0; i < body_length; ++i) {
        if (body[i] == 0xAA || body[i] == 0xFF) {
            out[length++] = 0xFF;
            out[length++] = (uint8_t)(body[i] ^ 0xFF);
        } else {
            out[length++] = body[i];
        }
    }
    return length;
}

static bool SameBytes(const uint8_t *a, size_t a_length, const uint8_t *b, size_t b_length)
{
    return a_length == b_length && memcmp(a, b, a_length) == 0;
}

/* INIT and SET WATCHDOG 00 answered, as a board answers them */
static const uint8_t init_request[4] = { 0xAA, 0x02, 0x00, 0x2F };
static const uint8_t init_reply[5] = { 0xAA, 0x03, 0x00, 0x2F, 0x00 };
static const uint8_t watchdog_request[5] = { 0xAA, 0x03, 0x01, 0x05, 0x00 };
static const uint8_t watchdog_reply[5] = { 0xAA, 0x03, 0x01, 0x05, 0x00 };
static const uint8_t version_payload[7] = { 0x47, 0x33, 0x32, 0x00, 0x02, 0x02, 0x06 };

/* Brings a session up at t: INIT at t, its reply at t + 1, SET WATCHDOG at
   t + 1 and its reply at t + 2 */
static void BringUp(SDL_P3IOSession *session, uint64_t t)
{
    uint8_t frame[SDL_P3IO_REQUEST_SIZE];
    size_t length;

    SDL_P3IO_SessionInit(session, t);
    length = Poll(session, t, frame);
    CHECK(SameBytes(frame, length, init_request, sizeof(init_request)));
    Receive(session, init_reply, sizeof(init_reply), t + 1);
    length = Poll(session, t + 1, frame);
    CHECK(SameBytes(frame, length, watchdog_request, sizeof(watchdog_request)));
    Receive(session, watchdog_reply, sizeof(watchdog_reply), t + 2);
    CHECK(SDL_P3IO_SessionUp(session));
}

/* The version reply for a sequence, as a board sends it */
static size_t VersionReply(uint8_t sequence, uint8_t *out)
{
    uint8_t body[10] = { 0x09, 0x00, 0x01, 0x47, 0x33, 0x32, 0x00, 0x02, 0x02, 0x06 };

    body[1] = sequence;
    return Frame(body, sizeof(body), out);
}

/* 1, 2 and 3: the request frames, byte for byte */
static void TestEncode(void)
{
    static const uint8_t version[4] = { 0xAA, 0x02, 0x00, 0x01 };
    static const uint8_t init[4] = { 0xAA, 0x02, 0x01, 0x2F };
    static const uint8_t watchdog[5] = { 0xAA, 0x03, 0x02, 0x05, 0x00 };
    static const uint8_t outputs_payload[5] = { 0xFF, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t outputs[10] = { 0xAA, 0x07, 0x00, 0x24, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t aa_payload[1] = { 0xAA };
    static const uint8_t aa[6] = { 0xAA, 0x03, 0x04, 0x05, 0xFF, 0x55 };
    static const uint8_t escaped_header[8] = { 0xAA, 0x02, 0xFF, 0x55, 0xFF, 0x00 };
    const uint8_t off = SDL_P3IO_WATCHDOG_OFF;
    uint8_t out[600], payload[256];
    size_t length, size, i;

    /* 1: GET VERSION, sequence 0 */
    length = SDL_P3IO_EncodeRequest(0, SDL_P3IO_CMD_GET_VERSION, NULL, 0, out, sizeof(out));
    CHECK(SameBytes(out, length, version, sizeof(version)));
    /* 2: INIT, sequence 1, and SET WATCHDOG 00, sequence 2 */
    length = SDL_P3IO_EncodeRequest(1, SDL_P3IO_CMD_INIT, NULL, 0, out, sizeof(out));
    CHECK(SameBytes(out, length, init, sizeof(init)));
    length = SDL_P3IO_EncodeRequest(2, SDL_P3IO_CMD_SET_WATCHDOG, &off, 1, out, sizeof(out));
    CHECK(SameBytes(out, length, watchdog, sizeof(watchdog)));
    /* 3: payload FF goes out as FF 00, here the FF that leads SET OUTPUTS */
    length = SDL_P3IO_EncodeRequest(0, 0x24, outputs_payload, sizeof(outputs_payload), out, sizeof(out));
    CHECK(SameBytes(out, length, outputs, sizeof(outputs)));
    /* and AA as FF 55 */
    length = SDL_P3IO_EncodeRequest(4, 0x05, aa_payload, 1, out, sizeof(out));
    CHECK(SameBytes(out, length, aa, sizeof(aa)));
    /* The sequence and the command are body bytes too */
    length = SDL_P3IO_EncodeRequest(0xAA, 0xFF, NULL, 0, out, sizeof(out));
    CHECK(SameBytes(out, length, escaped_header, 6));

    /* Every buffer short of the frame fails, and the exact size fits */
    for (size = 0; size < sizeof(outputs); ++size) {
        memset(out, 0x11, sizeof(out));
        CHECK(SDL_P3IO_EncodeRequest(0, 0x24, outputs_payload, sizeof(outputs_payload), out, size) == 0);
    }
    CHECK(SDL_P3IO_EncodeRequest(0, 0x24, outputs_payload, sizeof(outputs_payload), out, sizeof(outputs)) == sizeof(outputs));
    for (size = 0; size < sizeof(aa); ++size) {
        CHECK(SDL_P3IO_EncodeRequest(4, 0x05, aa_payload, 1, out, size) == 0);
    }
    CHECK(SDL_P3IO_EncodeRequest(4, 0x05, aa_payload, 1, out, sizeof(aa)) == sizeof(aa));

    /* The longest body: 253 payload bytes make length FF, escaped */
    for (i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)i;
    }
    length = SDL_P3IO_EncodeRequest(7, 0x3A, payload, 253, out, sizeof(out));
    CHECK(length > 3 && out[0] == 0xAA && out[1] == 0xFF && out[2] == 0x00 && out[3] == 0x07 && out[4] == 0x3A);
    CHECK(SDL_P3IO_EncodeRequest(7, 0x3A, payload, 254, out, sizeof(out)) == 0);

    /* No buffer, or a payload length without a payload */
    CHECK(SDL_P3IO_EncodeRequest(0, 0x01, NULL, 0, NULL, 16) == 0);
    CHECK(SDL_P3IO_EncodeRequest(0, 0x01, NULL, 1, out, sizeof(out)) == 0);
}

/* Feeds a whole stream to a fresh parser and counts the frames it completes */
static int ParseAll(SDL_P3IOParser *parser, const uint8_t *data, size_t length)
{
    int frames = 0;
    size_t i;

    for (i = 0; i < length; ++i) {
        if (SDL_P3IO_ParseByte(parser, data[i])) {
            ++frames;
        }
    }
    return frames;
}

/* 1, 2 and 3: the reply frames through the parser alone */
static void TestParser(void)
{
    static const uint8_t version[11] = { 0xAA, 0x09, 0x00, 0x01, 0x47, 0x33, 0x32, 0x00, 0x02, 0x02, 0x06 };
    static const uint8_t init[5] = { 0xAA, 0x03, 0x01, 0x2F, 0x00 };
    static const uint8_t watchdog[5] = { 0xAA, 0x03, 0x02, 0x05, 0x00 };
    static const uint8_t escaped[13] = { 0xAA, 0x09, 0x00, 0x01, 0xFF, 0x55, 0xFF, 0x00, 0x32, 0x00, 0x02, 0x02, 0x06 };
    static const uint8_t escaped_body[10] = { 0x09, 0x00, 0x01, 0xAA, 0xFF, 0x32, 0x00, 0x02, 0x02, 0x06 };
    SDL_P3IOParser parser;
    uint8_t body[256], stream[600];
    size_t length, i;

    SDL_P3IO_ResetParser(&parser);
    CHECK(ParseAll(&parser, version, sizeof(version)) == 1);
    CHECK(parser.count == 10 && memcmp(parser.body, version + 1, 10) == 0);
    CHECK(!parser.in_frame);
    SDL_P3IO_ResetParser(&parser);
    CHECK(ParseAll(&parser, init, sizeof(init)) == 1 && parser.count == 4 && memcmp(parser.body, init + 1, 4) == 0);
    SDL_P3IO_ResetParser(&parser);
    CHECK(ParseAll(&parser, watchdog, sizeof(watchdog)) == 1 && memcmp(parser.body, watchdog + 1, 4) == 0);

    /* Escaped payload bytes decode */
    SDL_P3IO_ResetParser(&parser);
    CHECK(ParseAll(&parser, escaped, sizeof(escaped)) == 1);
    CHECK(parser.count == 10 && memcmp(parser.body, escaped_body, 10) == 0);

    /* The frame completes on its last byte and no other */
    SDL_P3IO_ResetParser(&parser);
    for (i = 0; i < sizeof(version); ++i) {
        CHECK(SDL_P3IO_ParseByte(&parser, version[i]) == (i == sizeof(version) - 1));
    }
    /* Bytes after a frame wait for the next AA */
    CHECK(ParseAll(&parser, version + 1, sizeof(version) - 1) == 0);
    CHECK(ParseAll(&parser, version, sizeof(version)) == 1);

    /* Junk and any number of AA bytes before a frame */
    SDL_P3IO_ResetParser(&parser);
    CHECK(ParseAll(&parser, (const uint8_t *)"\x00\x12\x55\x03\x2F", 5) == 0);
    CHECK(ParseAll(&parser, (const uint8_t *)"\xAA\xAA\xAA", 3) == 0);
    CHECK(ParseAll(&parser, init + 1, sizeof(init) - 1) == 1 && memcmp(parser.body, init + 1, 4) == 0);

    /* An AA inside a frame starts the frame over */
    SDL_P3IO_ResetParser(&parser);
    CHECK(ParseAll(&parser, version, 6) == 0);
    CHECK(ParseAll(&parser, init, sizeof(init)) == 1 && parser.count == 4 && memcmp(parser.body, init + 1, 4) == 0);

    /* FF FF drops the frame, and the next AA starts another */
    SDL_P3IO_ResetParser(&parser);
    CHECK(ParseAll(&parser, (const uint8_t *)"\xAA\x03\x01\xFF\xFF\x00\x00\x00", 8) == 0);
    CHECK(!parser.in_frame);
    CHECK(ParseAll(&parser, init, sizeof(init)) == 1);

    /* Length 0 or 1 leaves no room for the sequence and the command */
    SDL_P3IO_ResetParser(&parser);
    CHECK(ParseAll(&parser, (const uint8_t *)"\xAA\x00\x00\x2F\x00", 5) == 0);
    CHECK(ParseAll(&parser, (const uint8_t *)"\xAA\x01\x00\x2F\x00", 5) == 0);
    /* Length 2 is the shortest frame: sequence and command */
    CHECK(ParseAll(&parser, (const uint8_t *)"\xAA\x02\x05\x01", 4) == 1 && parser.count == 3);

    /* A length byte of AA, escaped, and the longest frame, length FF */
    for (i = 0; i < sizeof(body); ++i) {
        body[i] = (uint8_t)(i * 7);
    }
    body[0] = 0xAA;
    length = Frame(body, 0xAB, stream);
    SDL_P3IO_ResetParser(&parser);
    CHECK(stream[1] == 0xFF && stream[2] == 0x55);
    CHECK(ParseAll(&parser, stream, length) == 1 && parser.count == 0xAB && memcmp(parser.body, body, 0xAB) == 0);
    body[0] = 0xFF;
    length = Frame(body, 256, stream);
    SDL_P3IO_ResetParser(&parser);
    CHECK(ParseAll(&parser, stream, length) == 1 && parser.count == 256 && memcmp(parser.body, body, 256) == 0);
    /* The last byte of the longest frame is the one that completes it */
    SDL_P3IO_ResetParser(&parser);
    CHECK(ParseAll(&parser, stream, length - 1) == 0 && parser.count == 255);

    /* A lone FF at the end completes nothing, and the complement finishes it */
    SDL_P3IO_ResetParser(&parser);
    CHECK(ParseAll(&parser, escaped, 5) == 0 && parser.escape);
    CHECK(ParseAll(&parser, escaped + 5, sizeof(escaped) - 5) == 1 && memcmp(parser.body, escaped_body, 10) == 0);
    /* An AA after a lone FF starts a clean frame */
    SDL_P3IO_ResetParser(&parser);
    CHECK(ParseAll(&parser, escaped, 5) == 0 && parser.escape);
    CHECK(ParseAll(&parser, init, sizeof(init)) == 1 && memcmp(parser.body, init + 1, 4) == 0);
}

/* 2: start-up, INIT then SET WATCHDOG 00, one request at a time */
static void TestStartup(void)
{
    SDL_P3IOSession session;
    uint8_t frame[SDL_P3IO_REQUEST_SIZE];
    size_t length;

    SDL_P3IO_SessionInit(&session, 100);
    CHECK(!SDL_P3IO_SessionUp(&session) && !SDL_P3IO_SessionAwaiting(&session));
    CHECK(SDL_P3IO_SessionDeadline(&session) == 100);
    CHECK(Poll(&session, 99, frame) == 0);
    length = Poll(&session, 100, frame);
    CHECK(SameBytes(frame, length, init_request, sizeof(init_request)));
    CHECK(SDL_P3IO_SessionAwaiting(&session) && SDL_P3IO_SessionDeadline(&session) == 100 + SDL_P3IO_REPLY_TIMEOUT_MS);
    /* Nothing more goes out while the reply is awaited */
    CHECK(Poll(&session, 100, frame) == 0 && Poll(&session, 150, frame) == 0);

    Receive(&session, init_reply, sizeof(init_reply), 160);
    CHECK(!SDL_P3IO_SessionUp(&session) && !SDL_P3IO_SessionAwaiting(&session));
    CHECK(session.phase == SDL_P3IO_PHASE_WATCHDOG && SDL_P3IO_SessionDeadline(&session) == 160);
    /* SET WATCHDOG 00 goes out at once */
    length = Poll(&session, 160, frame);
    CHECK(SameBytes(frame, length, watchdog_request, sizeof(watchdog_request)));
    CHECK(Poll(&session, 161, frame) == 0);

    Receive(&session, watchdog_reply, sizeof(watchdog_reply), 170);
    CHECK(SDL_P3IO_SessionUp(&session) && session.ups == 1 && !SDL_P3IO_SessionAwaiting(&session));
    CHECK(SDL_P3IO_SessionDeadline(&session) == 170 + SDL_P3IO_KEEPALIVE_MS);

    /* The state byte of the watchdog reply is not checked, as bemanitools
       does not check it */
    SDL_P3IO_SessionInit(&session, 0);
    Poll(&session, 0, frame);
    Receive(&session, init_reply, sizeof(init_reply), 0);
    Poll(&session, 0, frame);
    Receive(&session, (const uint8_t *)"\xAA\x03\x01\x05\x01", 5, 0);
    CHECK(SDL_P3IO_SessionUp(&session));

    /* A buffer too small for the request sends nothing and keeps the sequence */
    SDL_P3IO_SessionInit(&session, 0);
    CHECK(SDL_P3IO_SessionPoll(&session, 0, frame, 3) == 0);
    CHECK(!SDL_P3IO_SessionAwaiting(&session) && session.sequence == 0);
    CHECK(SDL_P3IO_SessionPoll(&session, 0, frame, 4) == 4 && memcmp(frame, init_request, 4) == 0);
    CHECK(SDL_P3IO_SessionPoll(NULL, 0, frame, sizeof(frame)) == 0);
    CHECK(!SDL_P3IO_SessionUp(NULL) && !SDL_P3IO_SessionAwaiting(NULL) && SDL_P3IO_SessionDeadline(NULL) == 0);
}

/* 6 with literal times, which pins each timer at 1000 ms */
static void TestTimeline(void)
{
    SDL_P3IOSession session;
    uint8_t frame[SDL_P3IO_REQUEST_SIZE];

    SDL_P3IO_SessionInit(&session, 0);
    CHECK(Poll(&session, 0, frame) == 4 && memcmp(frame, init_request, 4) == 0);
    Receive(&session, init_reply, sizeof(init_reply), 10);
    CHECK(Poll(&session, 10, frame) == 5 && memcmp(frame, watchdog_request, 5) == 0);
    Receive(&session, watchdog_reply, sizeof(watchdog_reply), 20);
    CHECK(SDL_P3IO_SessionDeadline(&session) == 1020);
    CHECK(Poll(&session, 1019, frame) == 0);
    CHECK(Poll(&session, 1020, frame) == 4 && frame[2] == 0x02 && frame[3] == 0x01);
    CHECK(SDL_P3IO_SessionDeadline(&session) == 2020);
    CHECK(Poll(&session, 2019, frame) == 0 && SDL_P3IO_SessionUp(&session));
    CHECK(Poll(&session, 2020, frame) == 0 && !SDL_P3IO_SessionUp(&session));
    CHECK(SDL_P3IO_SessionDeadline(&session) == 3020);
    CHECK(Poll(&session, 3019, frame) == 0);
    CHECK(Poll(&session, 3020, frame) == 4 && memcmp(frame, init_request, 4) == 0);
}

/* 1, 2 and 6: GET VERSION on schedule, the sequence from 2 through 15 and
   back to 0, and the part's version reply for sequence 0 */
static void TestKeepAlive(void)
{
    SDL_P3IOSession session;
    uint8_t frame[SDL_P3IO_REQUEST_SIZE], reply[32];
    const uint8_t version_request[4] = { 0xAA, 0x02, 0x00, 0x01 };
    const uint8_t version_reply[11] = { 0xAA, 0x09, 0x00, 0x01, 0x47, 0x33, 0x32, 0x00, 0x02, 0x02, 0x06 };
    uint64_t t = 1000;
    size_t length;
    int k;

    BringUp(&session, t);
    t += 2; /* The watchdog reply */
    for (k = 0; k < 18; ++k) {
        const uint8_t sequence = (uint8_t)((2 + k) & 0x0F);
        uint8_t expected[4] = { 0xAA, 0x02, 0x00, 0x01 };

        expected[2] = sequence;
        CHECK(Poll(&session, t + SDL_P3IO_KEEPALIVE_MS - 1, frame) == 0);
        length = Poll(&session, t + SDL_P3IO_KEEPALIVE_MS, frame);
        CHECK(SameBytes(frame, length, expected, sizeof(expected)));
        t += SDL_P3IO_KEEPALIVE_MS;
        CHECK(SDL_P3IO_SessionDeadline(&session) == t + SDL_P3IO_REPLY_TIMEOUT_MS);
        if (sequence == 0) {
            /* 1: the part's frames, byte for byte */
            CHECK(SameBytes(frame, length, version_request, sizeof(version_request)));
            memset(session.version, 0, sizeof(session.version));
            Receive(&session, version_reply, sizeof(version_reply), t + 7);
            CHECK(memcmp(session.version, version_payload, sizeof(version_payload)) == 0);
            t += 7;
        } else {
            length = VersionReply(sequence, reply);
            Receive(&session, reply, length, t + 3);
            t += 3;
        }
        /* The next one is due a keep-alive after the reply */
        CHECK(!SDL_P3IO_SessionAwaiting(&session) && SDL_P3IO_SessionUp(&session));
        CHECK(SDL_P3IO_SessionDeadline(&session) == t + SDL_P3IO_KEEPALIVE_MS);
    }
    CHECK(session.ups == 1);

    /* 6: a reply that never comes ends the session after the timeout */
    length = Poll(&session, t + SDL_P3IO_KEEPALIVE_MS, frame);
    CHECK(length == 4 && frame[3] == SDL_P3IO_CMD_GET_VERSION);
    t += SDL_P3IO_KEEPALIVE_MS;
    CHECK(Poll(&session, t + SDL_P3IO_REPLY_TIMEOUT_MS - 1, frame) == 0);
    CHECK(SDL_P3IO_SessionUp(&session) && SDL_P3IO_SessionAwaiting(&session));
    CHECK(Poll(&session, t + SDL_P3IO_REPLY_TIMEOUT_MS, frame) == 0);
    CHECK(!SDL_P3IO_SessionUp(&session) && !SDL_P3IO_SessionAwaiting(&session));
    CHECK(session.phase == SDL_P3IO_PHASE_LOST);
    t += SDL_P3IO_REPLY_TIMEOUT_MS;
    CHECK(SDL_P3IO_SessionDeadline(&session) == t + SDL_P3IO_RETRY_MS);
    /* A reply after the timeout changes nothing */
    length = VersionReply((uint8_t)((2 + 18) & 0x0F), reply);
    Receive(&session, reply, length, t + 1);
    CHECK(session.phase == SDL_P3IO_PHASE_LOST && SDL_P3IO_SessionDeadline(&session) == t + SDL_P3IO_RETRY_MS);

    /* 7: reconnect, INIT again with sequence 0 */
    CHECK(Poll(&session, t + SDL_P3IO_RETRY_MS - 1, frame) == 0);
    length = Poll(&session, t + SDL_P3IO_RETRY_MS, frame);
    CHECK(SameBytes(frame, length, init_request, sizeof(init_request)));
    t += SDL_P3IO_RETRY_MS;
    Receive(&session, init_reply, sizeof(init_reply), t + 1);
    length = Poll(&session, t + 1, frame);
    CHECK(SameBytes(frame, length, watchdog_request, sizeof(watchdog_request)));
    Receive(&session, watchdog_reply, sizeof(watchdog_reply), t + 2);
    CHECK(SDL_P3IO_SessionUp(&session) && session.ups == 2);
    length = Poll(&session, t + 2 + SDL_P3IO_KEEPALIVE_MS, frame);
    CHECK(length == 4 && frame[2] == 0x02 && frame[3] == SDL_P3IO_CMD_GET_VERSION);

    /* INIT and SET WATCHDOG that go unanswered time out the same way */
    SDL_P3IO_SessionInit(&session, 0);
    CHECK(Poll(&session, 0, frame) == 4);
    CHECK(Poll(&session, SDL_P3IO_REPLY_TIMEOUT_MS - 1, frame) == 0 && session.phase == SDL_P3IO_PHASE_INIT);
    CHECK(Poll(&session, SDL_P3IO_REPLY_TIMEOUT_MS, frame) == 0 && session.phase == SDL_P3IO_PHASE_LOST);
    SDL_P3IO_SessionInit(&session, 0);
    Poll(&session, 0, frame);
    Receive(&session, init_reply, sizeof(init_reply), 10);
    CHECK(Poll(&session, 10, frame) == 5);
    CHECK(Poll(&session, 10 + SDL_P3IO_REPLY_TIMEOUT_MS - 1, frame) == 0 && session.phase == SDL_P3IO_PHASE_WATCHDOG);
    CHECK(Poll(&session, 10 + SDL_P3IO_REPLY_TIMEOUT_MS, frame) == 0 && session.phase == SDL_P3IO_PHASE_LOST);
    CHECK(SDL_P3IO_SessionDeadline(&session) == 10 + SDL_P3IO_REPLY_TIMEOUT_MS + SDL_P3IO_RETRY_MS);

    /* The bound the timing comment states: a command at least every 2
       seconds while up, and INIT under 3 seconds after the last command */
    CHECK(SDL_P3IO_KEEPALIVE_MS + SDL_P3IO_REPLY_TIMEOUT_MS <= 2000);
    CHECK(SDL_P3IO_KEEPALIVE_MS + SDL_P3IO_REPLY_TIMEOUT_MS + SDL_P3IO_RETRY_MS <= 3000);
}

/* 3 and 7: every truncation and every split of each reply */
typedef enum
{
    STEP_INIT,
    STEP_WATCHDOG,
    STEP_VERSION
} Step;

/* A session with the request of the step out, sent at t */
static void Prepare(SDL_P3IOSession *session, Step step, uint64_t t, uint8_t *reply, size_t *reply_length)
{
    uint8_t frame[SDL_P3IO_REQUEST_SIZE];

    switch (step) {
    case STEP_INIT:
        SDL_P3IO_SessionInit(session, t);
        Poll(session, t, frame);
        memcpy(reply, init_reply, sizeof(init_reply));
        *reply_length = sizeof(init_reply);
        break;
    case STEP_WATCHDOG:
        SDL_P3IO_SessionInit(session, t);
        Poll(session, t, frame);
        Receive(session, init_reply, sizeof(init_reply), t);
        Poll(session, t, frame);
        memcpy(reply, watchdog_reply, sizeof(watchdog_reply));
        *reply_length = sizeof(watchdog_reply);
        break;
    default:
        BringUp(session, t - 2 - SDL_P3IO_KEEPALIVE_MS);
        Poll(session, t, frame);
        *reply_length = VersionReply(2, reply);
        break;
    }
    CHECK(SDL_P3IO_SessionAwaiting(session));
}

static bool Accepted(const SDL_P3IOSession *session, Step step)
{
    if (SDL_P3IO_SessionAwaiting(session)) {
        return false;
    }
    switch (step) {
    case STEP_INIT:
        return session->phase == SDL_P3IO_PHASE_WATCHDOG;
    case STEP_WATCHDOG:
        return session->phase == SDL_P3IO_PHASE_UP && session->ups == 1;
    default:
        return session->phase == SDL_P3IO_PHASE_UP && memcmp(session->version, version_payload, 7) == 0;
    }
}

static void TestTruncationAndSplits(void)
{
    static const Step steps[3] = { STEP_INIT, STEP_WATCHDOG, STEP_VERSION };
    SDL_P3IOSession session;
    uint8_t reply[64], stale[64];
    size_t reply_length, n, i, j;
    int s;

    for (s = 0; s < 3; ++s) {
        const Step step = steps[s];
        const uint64_t t = 5000;

        /* Every truncation, with the rest of the reply stale past the length */
        Prepare(&session, step, t, reply, &reply_length);
        for (n = 0; n < reply_length; ++n) {
            Prepare(&session, step, t, reply, &reply_length);
            if (step == STEP_VERSION) {
                memset(session.version, 0, sizeof(session.version));
            }
            memset(stale, 0x5A, sizeof(stale));
            memcpy(stale, reply, reply_length);
            SDL_P3IO_SessionReceive(&session, stale, n, t + 1);
            CHECK(SDL_P3IO_SessionAwaiting(&session) && !Accepted(&session, step));
            Receive(&session, reply, n, t + 1);
            CHECK(SDL_P3IO_SessionAwaiting(&session));
            /* The truncated reply then times out */
            CHECK(Poll(&session, t + SDL_P3IO_REPLY_TIMEOUT_MS, stale) == 0 && session.phase == SDL_P3IO_PHASE_LOST);
        }
        /* A truncated reply followed by the whole one: the AA starts over */
        for (n = 0; n < reply_length; ++n) {
            Prepare(&session, step, t, reply, &reply_length);
            Receive(&session, reply, n, t + 1);
            Receive(&session, reply, reply_length, t + 2);
            CHECK(Accepted(&session, step));
        }
        /* Every split in two, and every split in three */
        for (i = 0; i <= reply_length; ++i) {
            Prepare(&session, step, t, reply, &reply_length);
            Receive(&session, reply, i, t + 1);
            CHECK(i == reply_length || SDL_P3IO_SessionAwaiting(&session));
            Receive(&session, reply + i, reply_length - i, t + 2);
            CHECK(Accepted(&session, step));
            for (j = i; j <= reply_length; ++j) {
                Prepare(&session, step, t, reply, &reply_length);
                Receive(&session, reply, i, t + 1);
                Receive(&session, reply + i, j - i, t + 1);
                Receive(&session, reply + j, reply_length - j, t + 1);
                CHECK(Accepted(&session, step));
            }
        }
        /* One byte per read */
        Prepare(&session, step, t, reply, &reply_length);
        for (i = 0; i < reply_length; ++i) {
            CHECK(!Accepted(&session, step));
            Receive(&session, reply + i, 1, t + 1);
        }
        CHECK(Accepted(&session, step));
        /* The same reply again, a duplicate, changes nothing */
        {
            const SDL_P3IOSession before = session;

            Receive(&session, reply, reply_length, t + 3);
            CHECK(session.phase == before.phase && session.due_at == before.due_at && session.ups == before.ups);
            CHECK(session.awaiting == before.awaiting && session.sequence == before.sequence);
        }
    }

    /* 3: the version reply split as AA 09 00 and the rest decodes once */
    Prepare(&session, STEP_VERSION, 7000, reply, &reply_length);
    session.awaited_sequence = 0x00;
    Receive(&session, (const uint8_t *)"\xAA\x09\x00", 3, 7001);
    CHECK(SDL_P3IO_SessionAwaiting(&session));
    Receive(&session, (const uint8_t *)"\x01\x47\x33\x32\x00\x02\x02\x06", 8, 7002);
    CHECK(Accepted(&session, STEP_VERSION) && SDL_P3IO_SessionDeadline(&session) == 7002 + SDL_P3IO_KEEPALIVE_MS);
}

/* 7: a wrong sequence or command, stale frames, and replies that fail */
static void TestWrongReplies(void)
{
    SDL_P3IOSession session;
    uint8_t reply[64], frame[SDL_P3IO_REQUEST_SIZE];
    size_t reply_length;

    /* A wrong sequence or command changes nothing, and the right reply
       still counts */
    Prepare(&session, STEP_INIT, 100, reply, &reply_length);
    Receive(&session, (const uint8_t *)"\xAA\x03\x01\x2F\x00", 5, 101);
    CHECK(SDL_P3IO_SessionAwaiting(&session) && session.phase == SDL_P3IO_PHASE_INIT);
    Receive(&session, (const uint8_t *)"\xAA\x03\x00\x2E\x00", 5, 101);
    Receive(&session, (const uint8_t *)"\xAA\x03\x10\x2F\x00", 5, 101);
    Receive(&session, (const uint8_t *)"\xAA\x03\x00\x05\x00", 5, 101);
    CHECK(SDL_P3IO_SessionAwaiting(&session) && session.phase == SDL_P3IO_PHASE_INIT);
    CHECK(SDL_P3IO_SessionDeadline(&session) == 100 + SDL_P3IO_REPLY_TIMEOUT_MS);
    Receive(&session, init_reply, sizeof(init_reply), 102);
    CHECK(Accepted(&session, STEP_INIT));

    /* Stale bytes and a stale frame ahead of the reply in one read */
    Prepare(&session, STEP_WATCHDOG, 200, reply, &reply_length);
    Receive(&session, (const uint8_t *)"\x00\x55\xAA\x09\x05\x01\x47\x33\x32\x00\x02\x02\x06\xAA\x03\x01\x05\x00", 18, 201);
    CHECK(Accepted(&session, STEP_WATCHDOG));

    /* An escape that completes nothing drops the frame */
    Prepare(&session, STEP_INIT, 300, reply, &reply_length);
    Receive(&session, (const uint8_t *)"\xAA\x03\x00\xFF\xFF\x2F\x00", 7, 301);
    CHECK(SDL_P3IO_SessionAwaiting(&session));
    Receive(&session, init_reply, sizeof(init_reply), 301);
    CHECK(Accepted(&session, STEP_INIT));

    /* Bytes that came before the request are not its reply */
    SDL_P3IO_SessionInit(&session, 400);
    Receive(&session, (const uint8_t *)"\xAA\x03\x00", 3, 399);
    Poll(&session, 400, frame);
    Receive(&session, (const uint8_t *)"\x2F\x00", 2, 401);
    CHECK(SDL_P3IO_SessionAwaiting(&session) && session.phase == SDL_P3IO_PHASE_INIT);
    SDL_P3IO_SessionInit(&session, 400);
    Receive(&session, (const uint8_t *)"\xAA", 1, 399);
    Poll(&session, 400, frame);
    Receive(&session, (const uint8_t *)"\x03\x00\x2F\x00", 4, 401);
    CHECK(SDL_P3IO_SessionAwaiting(&session) && session.phase == SDL_P3IO_PHASE_INIT);

    /* No request out: a frame changes nothing */
    BringUp(&session, 500);
    Receive(&session, (const uint8_t *)"\xAA\x03\x02\x01\x00", 5, 510);
    reply_length = VersionReply(2, reply);
    Receive(&session, reply, reply_length, 510);
    CHECK(SDL_P3IO_SessionUp(&session) && !SDL_P3IO_SessionAwaiting(&session));
    CHECK(SDL_P3IO_SessionDeadline(&session) == 502 + SDL_P3IO_KEEPALIVE_MS);
    CHECK(session.sequence == 2);

    /* An INIT status other than 00, or a counted reply of the wrong length,
       ends the session, and INIT goes out again a pause later */
    Prepare(&session, STEP_INIT, 600, reply, &reply_length);
    Receive(&session, (const uint8_t *)"\xAA\x03\x00\x2F\x01", 5, 601);
    CHECK(session.phase == SDL_P3IO_PHASE_LOST && SDL_P3IO_SessionDeadline(&session) == 601 + SDL_P3IO_RETRY_MS);
    CHECK(Poll(&session, 600 + SDL_P3IO_RETRY_MS, frame) == 0);
    CHECK(Poll(&session, 601 + SDL_P3IO_RETRY_MS, frame) == 4 && memcmp(frame, init_request, 4) == 0);
    Prepare(&session, STEP_INIT, 600, reply, &reply_length);
    Receive(&session, (const uint8_t *)"\xAA\x02\x00\x2F", 4, 601);
    CHECK(session.phase == SDL_P3IO_PHASE_LOST);
    Prepare(&session, STEP_INIT, 600, reply, &reply_length);
    Receive(&session, (const uint8_t *)"\xAA\x04\x00\x2F\x00\x00", 6, 601);
    CHECK(session.phase == SDL_P3IO_PHASE_LOST);
    Prepare(&session, STEP_WATCHDOG, 600, reply, &reply_length);
    Receive(&session, (const uint8_t *)"\xAA\x02\x01\x05", 4, 601);
    CHECK(session.phase == SDL_P3IO_PHASE_LOST && session.ups == 0);
    Prepare(&session, STEP_WATCHDOG, 600, reply, &reply_length);
    Receive(&session, (const uint8_t *)"\xAA\x04\x01\x05\x00\x00", 6, 601);
    CHECK(session.phase == SDL_P3IO_PHASE_LOST && session.ups == 0);
    Prepare(&session, STEP_VERSION, 2000, reply, &reply_length);
    Receive(&session, (const uint8_t *)"\xAA\x08\x02\x01\x47\x33\x32\x00\x02\x02", 10, 2001);
    CHECK(session.phase == SDL_P3IO_PHASE_LOST);
    Prepare(&session, STEP_VERSION, 2000, reply, &reply_length);
    Receive(&session, (const uint8_t *)"\xAA\x0A\x02\x01\x47\x33\x32\x00\x02\x02\x06\x00", 12, 2001);
    CHECK(session.phase == SDL_P3IO_PHASE_LOST);

    /* A failed transfer ends the session at once */
    BringUp(&session, 800);
    Poll(&session, 802 + SDL_P3IO_KEEPALIVE_MS, frame);
    SDL_P3IO_SessionFailed(&session, 1900);
    CHECK(session.phase == SDL_P3IO_PHASE_LOST && !SDL_P3IO_SessionAwaiting(&session));
    CHECK(SDL_P3IO_SessionDeadline(&session) == 1900 + SDL_P3IO_RETRY_MS);
    CHECK(Poll(&session, 1900 + SDL_P3IO_RETRY_MS - 1, frame) == 0);
    CHECK(Poll(&session, 1900 + SDL_P3IO_RETRY_MS, frame) == 4 && memcmp(frame, init_request, 4) == 0);
    SDL_P3IO_SessionFailed(NULL, 0);

    /* No data, or no session, changes nothing */
    Prepare(&session, STEP_INIT, 900, reply, &reply_length);
    SDL_P3IO_SessionReceive(&session, NULL, 5, 901);
    SDL_P3IO_SessionReceive(NULL, init_reply, sizeof(init_reply), 901);
    CHECK(SDL_P3IO_SessionAwaiting(&session) && session.phase == SDL_P3IO_PHASE_INIT);
}

/* The part's table, read on its own: which control each byte-and-bit
   presses, as player (0 or 1) and joystick button */
typedef struct
{
    int byte;
    int bit;
    int player;
    int button;
} Bit;

static const Bit table[] = {
    { 1, 0, 0, 0 }, { 1, 1, 0, 1 }, { 1, 2, 0, 2 }, { 1, 3, 0, 3 }, { 1, 4, 0, 4 }, { 1, 6, 0, 5 }, { 1, 7, 0, 6 },
    { 2, 0, 1, 0 }, { 2, 1, 1, 1 }, { 2, 2, 1, 2 }, { 2, 3, 1, 3 }, { 2, 4, 1, 4 }, { 2, 6, 1, 5 }, { 2, 7, 1, 6 },
    { 3, 0, 0, 7 }, { 3, 1, 0, 8 }, { 3, 2, 1, 7 }, { 3, 3, 1, 8 },
    { 3, 4, 0, 9 },  /* Test, bemanitools' bit */
    { 3, 5, 0, 11 }, /* Coin */
    { 3, 6, 0, 10 }, /* Service, bemanitools' bit */
};

static void Expected(const uint8_t *report, uint16_t buttons[2])
{
    size_t k;

    buttons[0] = buttons[1] = 0;
    for (k = 0; k < ARRAYSIZE(table); ++k) {
        if (!(report[table[k].byte] & (1u << table[k].bit))) {
            buttons[table[k].player] |= (uint16_t)(1u << table[k].button);
        }
    }
}

/* An input with every HD menu bit armed, as after an idle report */
static void Armed(SDL_P3IOInput *input, SDL_P3IOState *state)
{
    static const uint8_t idle[12] = { 0x80, 0xFF, 0xFF, 0xFF };

    SDL_P3IO_InputInit(input, state);
    CHECK(Feed(input, idle, sizeof(idle), state));
}

/* 4 */
static void TestReport(void)
{
    SDL_P3IOInput input;
    SDL_P3IOState state;
    uint8_t report[12];
    uint16_t expected[2];
    size_t k;
    int byte, player_byte, mismatches;

    /* Idle: nothing pressed */
    Armed(&input, &state);
    CHECK(state.buttons[0] == 0 && state.buttons[1] == 0);
    /* 80 FD FF FF: P1 up, OpenITG's mask FD */
    memcpy(report, "\x80\xFD\xFF\xFF\x00\x00\x00\x00\x00\x00\x00\x00", 12);
    CHECK(Feed(&input, report, 12, &state));
    CHECK(state.buttons[0] == (1u << SDL_P3IO_BUTTON_UP) && state.buttons[1] == 0);

    /* Each mapped bit alone presses one control */
    for (k = 0; k < ARRAYSIZE(table); ++k) {
        Armed(&input, &state);
        memcpy(report, "\x80\xFF\xFF\xFF\x00\x00\x00\x00\x00\x00\x00\x00", 12);
        report[table[k].byte] &= (uint8_t)~(1u << table[k].bit);
        CHECK(Feed(&input, report, 12, &state));
        CHECK(state.buttons[table[k].player] == (1u << table[k].button));
        CHECK(state.buttons[1 - table[k].player] == 0);
    }
    /* Bit 5 of bytes 1 and 2 and bit 7 of byte 3 press nothing */
    memcpy(report, "\x80\xDF\xDF\x7F\x00\x00\x00\x00\x00\x00\x00\x00", 12);
    Armed(&input, &state);
    CHECK(Feed(&input, report, 12, &state) && state.buttons[0] == 0 && state.buttons[1] == 0);
    /* Nor do bytes 4 to 11 */
    memcpy(report, "\x80\xFF\xFF\xFF\x00\x00\x00\x00\x00\x00\x00\x00", 12);
    for (byte = 4; byte < 12; ++byte) {
        report[byte] = 0xA5;
    }
    CHECK(Feed(&input, report, 12, &state) && state.buttons[0] == 0 && state.buttons[1] == 0);

    /* OpenITG's masks name the same controls, except that its Test is our
       Service and its Service our Test */
    {
        static const struct
        {
            int byte;
            uint8_t mask;
            int player;
            int button;
        } openitg[] = {
            { 1, 0xFD, 0, SDL_P3IO_BUTTON_UP }, { 1, 0xFB, 0, SDL_P3IO_BUTTON_DOWN },
            { 1, 0xF7, 0, SDL_P3IO_BUTTON_LEFT }, { 1, 0xEF, 0, SDL_P3IO_BUTTON_RIGHT },
            { 1, 0xBF, 0, SDL_P3IO_BUTTON_MENU_LEFT }, { 1, 0xFE, 0, SDL_P3IO_BUTTON_START },
            { 1, 0x7F, 0, SDL_P3IO_BUTTON_MENU_RIGHT }, { 2, 0xFD, 1, SDL_P3IO_BUTTON_UP },
            { 3, 0xFE, 0, SDL_P3IO_BUTTON_MENU_UP }, { 3, 0xFB, 1, SDL_P3IO_BUTTON_MENU_UP },
            { 3, 0xFD, 0, SDL_P3IO_BUTTON_MENU_DOWN }, { 3, 0xF7, 1, SDL_P3IO_BUTTON_MENU_DOWN },
            { 3, 0xBF, 0, SDL_P3IO_BUTTON_SERVICE }, { 3, 0xEF, 0, SDL_P3IO_BUTTON_TEST },
            { 3, 0xDF, 0, SDL_P3IO_BUTTON_COIN },
        };

        for (k = 0; k < ARRAYSIZE(openitg); ++k) {
            Armed(&input, &state);
            memcpy(report, "\x80\xFF\xFF\xFF\x00\x00\x00\x00\x00\x00\x00\x00", 12);
            report[openitg[k].byte] = openitg[k].mask;
            CHECK(Feed(&input, report, 12, &state));
            CHECK(state.buttons[openitg[k].player] == (1u << openitg[k].button));
        }
    }

    /* Every value of bytes 1 and 3, and of bytes 2 and 3, against the table */
    for (player_byte = 1; player_byte <= 2; ++player_byte) {
        int word;

        mismatches = 0;
        Armed(&input, &state);
        memcpy(report, "\x80\xFF\xFF\xFF\x00\x00\x00\x00\x00\x00\x00\x00", 12);
        for (word = 0; word < 0x10000; ++word) {
            report[player_byte] = (uint8_t)(word & 0xFF);
            report[3] = (uint8_t)(word >> 8);
            Expected(report, expected);
            if (!SDL_P3IO_InputFeed(&input, report, 12, &state) ||
                state.buttons[0] != expected[0] || state.buttons[1] != expected[1]) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
    }

    /* The mapping names only buttons both joysticks have */
    CHECK(strcmp(SDL_P3IO_MAPPING, "a:b8,dpdown:b2,dpleft:b3,dpright:b4,dpup:b1,leftshoulder:b5,rightshoulder:b6,start:b0,y:b7,") == 0);
    CHECK(SDL_P3IO_BUTTON_MENU_DOWN < SDL_P3IO_P2_BUTTONS && SDL_P3IO_BUTTON_COIN < SDL_P3IO_P1_BUTTONS);
    CHECK(SDL_P3IO_BUTTON_TEST >= SDL_P3IO_P2_BUTTONS && SDL_P3IO_BUTTON_SERVICE >= SDL_P3IO_P2_BUTTONS);
}

/* The HD menu bits press only once they have read released */
static void TestMenuGate(void)
{
    SDL_P3IOInput input;
    SDL_P3IOState state;
    uint8_t report[12];
    const uint16_t p1_up = 1u << SDL_P3IO_BUTTON_MENU_UP, p1_down = 1u << SDL_P3IO_BUTTON_MENU_DOWN;

    memcpy(report, "\x80\xFF\xFF\xF0\x00\x00\x00\x00\x00\x00\x00\x00", 12);
    SDL_P3IO_InputInit(&input, &state);
    /* Held from the first report: nothing, however long */
    CHECK(Feed(&input, report, 12, &state) && state.buttons[0] == 0 && state.buttons[1] == 0);
    CHECK(Feed(&input, report, 12, &state) && state.buttons[0] == 0 && state.buttons[1] == 0);
    /* Released, then pressed */
    report[3] = 0xFF;
    CHECK(Feed(&input, report, 12, &state) && state.buttons[0] == 0 && state.buttons[1] == 0);
    CHECK(input.armed == SDL_P3IO_OPERATOR_HD_MENU);
    report[3] = 0xFE;
    CHECK(Feed(&input, report, 12, &state) && state.buttons[0] == p1_up && state.buttons[1] == 0);
    report[3] = 0xF0;
    CHECK(Feed(&input, report, 12, &state));
    CHECK(state.buttons[0] == (p1_up | p1_down) && state.buttons[1] == (p1_up | p1_down));

    /* Each bit arms on its own */
    SDL_P3IO_InputInit(&input, &state);
    report[3] = 0xFA; /* P1 up and P2 up held, the downs released */
    CHECK(Feed(&input, report, 12, &state) && state.buttons[0] == 0 && state.buttons[1] == 0);
    CHECK(input.armed == (SDL_P3IO_OPERATOR_P1_MENU_DOWN | SDL_P3IO_OPERATOR_P2_MENU_DOWN));
    report[3] = 0xF0;
    CHECK(Feed(&input, report, 12, &state) && state.buttons[0] == p1_down && state.buttons[1] == p1_down);

    /* Test, Service and Coin are never held back */
    SDL_P3IO_InputInit(&input, &state);
    report[3] = 0x8F;
    CHECK(Feed(&input, report, 12, &state));
    CHECK(state.buttons[0] == ((1u << SDL_P3IO_BUTTON_TEST) | (1u << SDL_P3IO_BUTTON_SERVICE) | (1u << SDL_P3IO_BUTTON_COIN)));
    CHECK(state.buttons[1] == 0);

    /* The same holds for reports that come in three transfers */
    SDL_P3IO_InputInit(&input, &state);
    CHECK(!Feed(&input, (const uint8_t *)"\x80\xFF\xFF\xF0", 4, &state));
    CHECK(!Feed(&input, (const uint8_t *)"\x00\x00\x00\x00", 4, &state));
    CHECK(Feed(&input, (const uint8_t *)"\x00\x00\x00\x00", 4, &state) && state.buttons[0] == 0);
    CHECK(!Feed(&input, (const uint8_t *)"\x80\xFF\xFF\xFF", 4, &state));
    CHECK(!Feed(&input, (const uint8_t *)"\x00\x00\x00\x00", 4, &state));
    CHECK(Feed(&input, (const uint8_t *)"\x00\x00\x00\x00", 4, &state) && input.armed == SDL_P3IO_OPERATOR_HD_MENU);

    /* A new input starts disarmed, with nothing gathered or pressed */
    CHECK(Feed(&input, (const uint8_t *)"\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 12, &state));
    CHECK(state.buttons[0] != 0 && state.buttons[1] != 0);
    CHECK(!Feed(&input, (const uint8_t *)"\x80\x00\x00\x00", 4, &state) && input.filled == 4);
    SDL_P3IO_InputInit(&input, &state);
    CHECK(input.armed == 0 && input.filled == 0 && state.buttons[0] == 0 && state.buttons[1] == 0);
}

/* 5 and 7: reports in three transfers, lengths, truncations */
static void TestTransfers(void)
{
    static const uint8_t whole[12] = { 0x80, 0xFD, 0xBF, 0xEF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    SDL_P3IOInput input;
    SDL_P3IOState state, whole_state, before;
    uint8_t stale[32];
    size_t n;

    Armed(&input, &state);
    CHECK(Feed(&input, whole, 12, &whole_state));
    CHECK(whole_state.buttons[0] == ((1u << SDL_P3IO_BUTTON_UP) | (1u << SDL_P3IO_BUTTON_TEST)));
    CHECK(whole_state.buttons[1] == (1u << SDL_P3IO_BUTTON_MENU_LEFT));

    /* The same report as three 4-byte transfers */
    Armed(&input, &state);
    CHECK(!Feed(&input, whole, 4, &state) && input.filled == 4);
    CHECK(!Feed(&input, whole + 4, 4, &state) && input.filled == 8);
    CHECK(Feed(&input, whole + 8, 4, &state) && input.filled == 0);
    CHECK(memcmp(&state, &whole_state, sizeof(state)) == 0);

    /* Before the first 80, a 4-byte transfer is dropped */
    Armed(&input, &state);
    CHECK(!Feed(&input, whole + 4, 4, &state) && input.filled == 0);
    CHECK(!Feed(&input, whole + 8, 4, &state) && input.filled == 0);
    CHECK(!Feed(&input, (const uint8_t *)"\x7F\xFD\xBF\xEF", 4, &state) && input.filled == 0);
    CHECK(state.buttons[0] == 0 && state.buttons[1] == 0);
    CHECK(!Feed(&input, whole, 4, &state));
    CHECK(!Feed(&input, whole + 4, 4, &state));
    CHECK(Feed(&input, whole + 8, 4, &state) && memcmp(&state, &whole_state, sizeof(state)) == 0);

    /* An 80 in the middle starts the report over */
    Armed(&input, &state);
    CHECK(!Feed(&input, (const uint8_t *)"\x80\x00\x00\xFF", 4, &state));
    CHECK(!Feed(&input, whole + 4, 4, &state));
    CHECK(!Feed(&input, whole, 4, &state) && input.filled == 4);
    CHECK(!Feed(&input, whole + 4, 4, &state));
    CHECK(Feed(&input, whole + 8, 4, &state) && memcmp(&state, &whole_state, sizeof(state)) == 0);

    /* A whole report in the middle of a gather drops the gather */
    Armed(&input, &state);
    CHECK(!Feed(&input, (const uint8_t *)"\x80\x00\x00\xFF", 4, &state));
    CHECK(Feed(&input, whole, 12, &state) && input.filled == 0);
    CHECK(!Feed(&input, whole + 8, 4, &state) && memcmp(&state, &whole_state, sizeof(state)) == 0);

    /* A 16-byte transfer decodes its first 12 */
    Armed(&input, &state);
    memset(stale, 0x00, sizeof(stale));
    memcpy(stale, whole, 12);
    stale[12] = stale[13] = stale[14] = stale[15] = 0x80;
    CHECK(Feed(&input, stale, 16, &state) && memcmp(&state, &whole_state, sizeof(state)) == 0);
    /* A report that does not start with 80 is dropped, and so is a gather */
    Armed(&input, &state);
    CHECK(!Feed(&input, whole, 4, &state) && input.filled == 4);
    memcpy(stale, whole, 12);
    stale[0] = 0x00;
    CHECK(!Feed(&input, stale, 12, &state) && input.filled == 0 && state.buttons[0] == 0);

    /* Every truncation of the report, with stale bytes past it, decodes
       nothing and changes no state */
    for (n = 0; n < 12; ++n) {
        Armed(&input, &state);
        before = state;
        memset(stale, 0x00, sizeof(stale));
        memcpy(stale, whole, 12);
        CHECK(!SDL_P3IO_InputFeed(&input, stale, n, &state));
        CHECK(memcmp(&state, &before, sizeof(state)) == 0);
        CHECK(input.filled == ((n == 4) ? 4u : 0u));
        CHECK(!Feed(&input, whole, n, &state) && memcmp(&state, &before, sizeof(state)) == 0);
    }
    /* A transfer of another length drops what was gathered, and an empty
       one keeps it */
    memset(stale, 0x00, sizeof(stale));
    memcpy(stale, whole, 12);
    for (n = 1; n < 12; ++n) {
        if (n == 4) {
            continue;
        }
        Armed(&input, &state);
        CHECK(!Feed(&input, whole, 4, &state) && input.filled == 4);
        CHECK(!Feed(&input, stale + 4, n, &state) && input.filled == 0);
    }
    Armed(&input, &state);
    CHECK(!Feed(&input, whole, 4, &state));
    CHECK(!Feed(&input, whole + 4, 0, &state) && input.filled == 4);
    CHECK(!SDL_P3IO_InputFeed(&input, NULL, 4, &state) && input.filled == 4);
    CHECK(!SDL_P3IO_InputFeed(NULL, whole, 12, &state) && !SDL_P3IO_InputFeed(&input, whole, 12, NULL));
    CHECK(!Feed(&input, whole + 4, 4, &state) && input.filled == 8);
    CHECK(Feed(&input, whole + 8, 4, &state) && memcmp(&state, &whole_state, sizeof(state)) == 0);

    /* Every truncation of each 4-byte transfer, with stale bytes past it */
    for (n = 0; n < 4; ++n) {
        Armed(&input, &state);
        before = state;
        memcpy(stale, whole, 12);
        CHECK(!SDL_P3IO_InputFeed(&input, stale, n, &state) && input.filled == 0);
        CHECK(!Feed(&input, whole, 4, &state));
        CHECK(!SDL_P3IO_InputFeed(&input, stale + 4, n, &state) && input.filled == (n ? 0u : 4u));
        CHECK(memcmp(&state, &before, sizeof(state)) == 0);
    }
}

/* The command endpoints, wherever the board puts them */
static void TestCommandEndpoints(void)
{
    static const SDL_P3IOEndpoint one[3] = {
        { 0, 0x81, 0x02 }, { 0, 0x02, 0x02 }, { 0, 0x83, 0x03 },
    };
    static const SDL_P3IOEndpoint two[3] = {
        { 0, 0x83, 0x03 }, { 1, 0x02, 0x02 }, { 1, 0x81, 0x02 },
    };
    static const SDL_P3IOEndpoint split[3] = {
        { 2, 0x81, 0x02 }, { 0, 0x83, 0x03 }, { 1, 0x02, 0x02 },
    };
    static const SDL_P3IOEndpoint interrupt[2] = {
        { 0, 0x02, 0x03 }, { 0, 0x81, 0x03 },
    };
    static const SDL_P3IOEndpoint missing_in[2] = {
        { 0, 0x02, 0x02 }, { 0, 0x83, 0x03 },
    };
    static const SDL_P3IOEndpoint missing_out[2] = {
        { 0, 0x81, 0x02 }, { 0, 0x83, 0x03 },
    };
    static const SDL_P3IOEndpoint wrong_type[4] = {
        { 0, 0x02, 0x00 }, { 0, 0x81, 0x01 }, { 3, 0x02, 0x02 }, { 4, 0x81, 0x02 },
    };
    static const SDL_P3IOEndpoint first[4] = {
        { 5, 0x02, 0x02 }, { 6, 0x81, 0x03 }, { 7, 0x02, 0x03 }, { 8, 0x81, 0x02 },
    };
    SDL_P3IOCommandEndpoints found;

    memset(&found, 0x77, sizeof(found));
    CHECK(SDL_P3IO_FindCommandEndpoints(one, 3, &found));
    CHECK(found.out_interface == 0 && found.in_interface == 0 && !found.out_interrupt && !found.in_interrupt);
    CHECK(SDL_P3IO_FindCommandEndpoints(two, 3, &found));
    CHECK(found.out_interface == 1 && found.in_interface == 1 && !found.out_interrupt && !found.in_interrupt);
    CHECK(SDL_P3IO_FindCommandEndpoints(split, 3, &found));
    CHECK(found.out_interface == 1 && found.in_interface == 2);
    CHECK(SDL_P3IO_FindCommandEndpoints(interrupt, 2, &found) && found.out_interrupt && found.in_interrupt);
    /* A control or isochronous endpoint at either address is passed over */
    CHECK(SDL_P3IO_FindCommandEndpoints(wrong_type, 4, &found));
    CHECK(found.out_interface == 3 && found.in_interface == 4 && !found.out_interrupt && !found.in_interrupt);
    CHECK(!SDL_P3IO_FindCommandEndpoints(wrong_type, 2, &found));
    /* The first of each counts */
    CHECK(SDL_P3IO_FindCommandEndpoints(first, 4, &found));
    CHECK(found.out_interface == 5 && found.in_interface == 6 && !found.out_interrupt && found.in_interrupt);

    /* Either one missing fails and leaves the result alone */
    memset(&found, 0x77, sizeof(found));
    CHECK(!SDL_P3IO_FindCommandEndpoints(missing_in, 2, &found));
    CHECK(!SDL_P3IO_FindCommandEndpoints(missing_out, 2, &found));
    CHECK(!SDL_P3IO_FindCommandEndpoints(one, 0, &found));
    CHECK(found.out_interface == 0x77 && found.in_interface == 0x77);
    CHECK(!SDL_P3IO_FindCommandEndpoints(NULL, 3, &found));
    CHECK(!SDL_P3IO_FindCommandEndpoints(one, 3, NULL));
}

/* One interface's descriptors as a configuration carries them */
static size_t Interface(uint8_t *out, uint8_t number, const uint8_t (*endpoints)[3], size_t count)
{
    size_t length = 0, i;

    out[length++] = 9;
    out[length++] = 0x04;
    out[length++] = number;
    out[length++] = 0; /* Alternate */
    out[length++] = (uint8_t)count;
    out[length++] = 0xFF;
    out[length++] = 0x00;
    out[length++] = 0x00;
    out[length++] = 0;
    for (i = 0; i < count; ++i) {
        out[length++] = 7;
        out[length++] = 0x05;
        out[length++] = endpoints[i][0];
        out[length++] = endpoints[i][1];
        out[length++] = endpoints[i][2];
        out[length++] = 0;
        out[length++] = 1;
    }
    return length;
}

/* The vendor rule: interface 0 on Windows only, interrupt IN 0x83 */
static void TestVendorRule(void)
{
    static const uint8_t board[3][3] = { { 0x81, 0x02, 64 }, { 0x02, 0x02, 64 }, { 0x83, 0x03, 16 } };
    static const uint8_t first[3][3] = { { 0x83, 0x03, 16 }, { 0x81, 0x02, 64 }, { 0x02, 0x02, 64 } };
    static const uint8_t no_input[2][3] = { { 0x81, 0x02, 64 }, { 0x02, 0x02, 64 } };
    static const SDL_VendorUSBPlatform platforms[3] = {
        SDL_VENDORUSB_PLATFORM_OTHER, SDL_VENDORUSB_PLATFORM_WINDOWS, SDL_VENDORUSB_PLATFORM_MACOS
    };
    const SDL_VendorUSBRule *rule;
    SDL_VendorUSBSelection selection;
    SDL_VendorUSBRouting routing;
    uint8_t descriptors[128];
    size_t length;
    int p;

    CHECK(USB_VENDOR_KONAMI == 0x1ccf && USB_PRODUCT_KONAMI_P3IO == 0x8008);
    rule = SDL_VendorUSB_FindRule(USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P3IO, 0, 0xFF, 0x00, 0x00);
    CHECK(rule != NULL);
    if (!rule) {
        return;
    }
    CHECK(rule->flags == SDL_VENDORUSB_WINDOWS_ONLY && rule->alternate == 0 && rule->in_endpoint == SDL_P3IO_INPUT_IN);
    CHECK(rule->out_endpoint == 0 && rule->in_size == 0 && rule->out_size == 0);
    /* Interface 0 whatever its class, and no other interface */
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P3IO, 0, 0x03, 0x01, 0x02) == rule);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P3IO, 1, 0xFF, 0x00, 0x00) == NULL);
    CHECK(SDL_VendorUSB_IsVendorDevice(USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P3IO));
    /* The chimera ID OpenITG also lists is not the board */
    CHECK(SDL_VendorUSB_FindRule(0x0000, 0x5731, 0, 0xFF, 0x00, 0x00) == NULL);
    CHECK(!SDL_VendorUSB_IsVendorDevice(0x0000, 0x5731));
    CHECK(!SDL_VendorUSB_IsVendorDevice(USB_VENDOR_KONAMI, 0x8009));

    for (p = 0; p < 3; ++p) {
        const bool windows = (platforms[p] == SDL_VENDORUSB_PLATFORM_WINDOWS);

        /* Windows only, like every rule of Part 14. Only the interface's
           rule takes the board to libusb. */
        CHECK(SDL_VendorUSB_RuleApplies(rule, platforms[p]) == windows);
        CHECK(!SDL_VendorUSB_RequiresLibUSB(platforms[p], USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P3IO, false, false));
        CHECK(SDL_VendorUSB_IsCandidate(platforms[p], USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P3IO, 0, 0xFF, 0, 0, false) == windows);
        CHECK(!SDL_VendorUSB_IsCandidate(platforms[p], USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P3IO, 1, 0xFF, 0, 0, false));
        /* On Windows libusb keeps it and the platform backend leaves it.
           Elsewhere libusb leaves it, as before the part. */
        memset(&routing, 0, sizeof(routing));
        routing.platform = platforms[p];
        routing.vendor = USB_VENDOR_KONAMI;
        routing.product = USB_PRODUCT_KONAMI_P3IO;
        routing.gamecube = true;
        routing.libusb = true;
        routing.vendor_interface = SDL_VendorUSB_RuleApplies(rule, platforms[p]);
        routing.whitelist = true;
        CHECK(SDL_VendorUSB_Ignore(&routing) == !windows);
        routing.libusb = false;
        routing.vendor_interface = false;
        CHECK(SDL_VendorUSB_Ignore(&routing) == windows);
    }

    /* The rule takes 0x83, not bulk IN 0x81, wherever 0x81 sits */
    length = Interface(descriptors, 0, board, 3);
    CHECK(SDL_VendorUSB_SelectEndpoints(rule, 0, descriptors, length, &selection));
    CHECK(selection.in.address == 0x83 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT);
    CHECK(selection.in.max_packet_size == 16 && selection.in.read_size == 16);
    CHECK(selection.out.address == 0x02 && selection.alternate == 0);
    length = Interface(descriptors, 0, first, 3);
    CHECK(SDL_VendorUSB_SelectEndpoints(rule, 0, descriptors, length, &selection) && selection.in.address == 0x83);
    /* Without 0x83 on interface 0, nothing is selected */
    length = Interface(descriptors, 0, no_input, 2);
    CHECK(!SDL_VendorUSB_SelectEndpoints(rule, 0, descriptors, length, &selection));
    length = Interface(descriptors, 1, board, 3);
    CHECK(!SDL_VendorUSB_SelectEndpoints(rule, 0, descriptors, length, &selection));
}

int main(void)
{
    TestEncode();
    TestParser();
    TestStartup();
    TestTimeline();
    TestKeepAlive();
    TestTruncationAndSplits();
    TestWrongReplies();
    TestReport();
    TestMenuGate();
    TestTransfers();
    TestCommandEndpoints();
    TestVendorRule();

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
