/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/dji/SDL_dji_duml.c, the DUML v1 codec of
 * hifihedgehog/SDL#33 Part 6: the two CRCs, the frame builder, the frame
 * check and the stream parser with and without the TCP envelope. The
 * vectors are the frames the part quotes. */

#include "testserialharness.h"
#include "../src/joystick/dji/SDL_dji_duml.h"

#define MAX_FRAMES 64
#define SDL_arraysize_test(array) (sizeof(array) / sizeof((array)[0]))

typedef struct Collected
{
    int count;
    size_t lengths[MAX_FRAMES];
    uint8_t frames[MAX_FRAMES][SDL_DJI_MAX_FRAME];
    SDL_DJIFrame fields[MAX_FRAMES];
} Collected;

static void Collect(void *userdata, const SDL_DJIFrame *frame)
{
    Collected *c = (Collected *)userdata;

    if (c->count < MAX_FRAMES) {
        memcpy(c->frames[c->count], frame->data, frame->length);
        c->lengths[c->count] = frame->length;
        c->fields[c->count] = *frame;
        ++c->count;
    }
}

/* Feeds from an exact-size heap copy, so a read past the count reaches the sanitizer */
static void Feed(SDL_DJIParser *parser, const uint8_t *data, size_t length, Collected *c)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);

    if (length) {
        memcpy(copy, data, length);
    }
    SDL_DJIParser_Feed(parser, copy, length, Collect, c);
    free(copy);
}

/* Feeds a buffer whose bytes past the count are 64 bytes of 00, then 64 of FF */
static void FeedPadded(SDL_DJIParser *parser, const uint8_t *data, size_t length, Collected *c)
{
    uint8_t *buffer = (uint8_t *)malloc(length + 128);

    memcpy(buffer, data, length);
    memset(buffer + length, 0x00, 64);
    memset(buffer + length + 64, 0xFF, 64);
    SDL_DJIParser_Feed(parser, buffer, length, Collect, c);
    free(buffer);
}

static Collected *NewCollected(void)
{
    return (Collected *)calloc(1, sizeof(Collected));
}

/* Writes the CRC16 of a frame's other bytes into its last two */
static void Reseal(uint8_t *frame, size_t length)
{
    const uint16_t crc = SDL_DJI_CRC16(frame, length - 2);

    frame[length - 2] = (uint8_t)(crc & 0xFF);
    frame[length - 1] = (uint8_t)(crc >> 8);
}

/* The frames of codec test 1: four of miniDjiController's pings and the
 * poll of DJI_RC-N1_SIMULATOR_FLY_DCL */
static const uint8_t request1[] = { 0x55, 0x0D, 0x04, 0x33, 0x0A, 0x0E, 0x02, 0x00, 0x40, 0x06, 0x27, 0x84, 0x05 };
static const uint8_t request2[] = { 0x55, 0x0D, 0x04, 0x33, 0x0A, 0x0E, 0x03, 0x00, 0x40, 0x06, 0x01, 0xF4, 0x4A };
static const uint8_t request3[] = { 0x55, 0x0D, 0x04, 0x33, 0x0A, 0x0E, 0x04, 0x00, 0x40, 0x06, 0x27, 0x1C, 0x3E };
static const uint8_t request4[] = { 0x55, 0x0D, 0x04, 0x33, 0x0A, 0x0E, 0x05, 0x00, 0x40, 0x06, 0x01, 0x6C, 0x71 };
static const uint8_t request5[] = { 0x55, 0x0D, 0x04, 0x33, 0x0A, 0x06, 0xEB, 0x34, 0x40, 0x06, 0x01, 0x74, 0x24 };

/* Codec test 2: two 06/01 replies and a pushed status of an RC Motion 3 */
static const uint8_t reply1[] = { 0x55, 0x17, 0x04, 0x38, 0x06, 0x0A, 0xF6, 0x34, 0x80, 0x06, 0x01, 0x00, 0x94, 0x06, 0x01, 0x80, 0x00, 0x02, 0x80, 0x00, 0x03, 0x08, 0x42 };
static const uint8_t reply2[] = { 0x55, 0x17, 0x04, 0x38, 0x06, 0x0A, 0xF6, 0x34, 0x80, 0x06, 0x01, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x02, 0x80, 0x00, 0x03, 0x63, 0x48 };
static const uint8_t status1[] = { 0x55, 0x13, 0x04, 0x03, 0x06, 0x0A, 0xD3, 0x0F, 0x00, 0x06, 0x1E, 0x28, 0x0A, 0x00, 0x00, 0x64, 0x01, 0xAD, 0xEF };

/* Codec test 7 */
static const uint8_t simulator[] = { 0x55, 0x0E, 0x04, 0x66, 0x0A, 0x06, 0xEB, 0x34, 0x40, 0x06, 0x24, 0x01, 0xD9, 0xEC };
static const uint8_t test_stick[] = { 0x55, 0x0D, 0x04, 0x33, 0x0A, 0x06, 0x01, 0x00, 0x40, 0x06, 0xF5, 0x8F, 0xCC };
static const uint8_t response[] = { 0x55, 0x0D, 0x04, 0x33, 0x0A, 0x06, 0x10, 0x00, 0x80, 0x00, 0x81, 0x62, 0x18 };

/* The DJI RC keepalive over TCP, sequence 1 */
static const uint8_t keepalive[] = { 0x55, 0xCC, 0x30, 0x75, 0x0D, 0x00, 0x00, 0x00, 0x55, 0x0D, 0x04, 0x33, 0x02, 0x06, 0x01, 0x00, 0x00, 0x00, 0x01, 0x6E, 0xF1 };

typedef struct Known
{
    const char *label;
    const uint8_t *data;
    size_t length;
} Known;

static const Known known[] = {
    { "request 1", request1, sizeof(request1) },
    { "request 2", request2, sizeof(request2) },
    { "request 3", request3, sizeof(request3) },
    { "request 4", request4, sizeof(request4) },
    { "request 5", request5, sizeof(request5) },
    { "reply 1", reply1, sizeof(reply1) },
    { "reply 2", reply2, sizeof(reply2) },
    { "status", status1, sizeof(status1) },
    { "06/24", simulator, sizeof(simulator) },
    { "06/F5", test_stick, sizeof(test_stick) },
    { "00/81 response", response, sizeof(response) },
};

static void TestTables(void)
{
    /* The table forms start 00 5E BC E2 61 3F DD 83 and 0000 1189 2312 329B */
    static const uint8_t t8[] = { 0x00, 0x5E, 0xBC, 0xE2, 0x61, 0x3F, 0xDD, 0x83 };
    static const uint16_t t16[] = { 0x0000, 0x1189, 0x2312, 0x329B };
    int i;

    for (i = 0; i < 8; ++i) {
        const uint8_t byte = (uint8_t)(i ^ 0x77);

        CHECK(SDL_DJI_CRC8(&byte, 1) == t8[i]);
    }
    for (i = 0; i < 4; ++i) {
        const uint8_t byte = (uint8_t)(i ^ 0x92);

        CHECK((uint16_t)(SDL_DJI_CRC16(&byte, 1) ^ 0x0036) == t16[i]);
    }
    CHECK(SDL_DJI_CRC8(NULL, 0) == 0x77);
    CHECK(SDL_DJI_CRC16(NULL, 0) == 0x3692);
}

/* Codec tests 1, 2 and 7 */
static void TestKnownFrames(void)
{
    size_t i;

    for (i = 0; i < SDL_arraysize_test(known); ++i) {
        const Known *k = &known[i];
        SDL_DJIFrame frame;
        uint8_t rebuilt[SDL_DJI_MAX_FRAME];
        size_t length;

        CHECK(SDL_DJI_CheckFrame(k->data, k->length, &frame));
        CHECK(frame.length == k->length && frame.data == k->data);
        CHECK(frame.payload_length == k->length - 13);
        length = SDL_DJI_BuildFrame(rebuilt, sizeof(rebuilt), frame.sender, frame.receiver, frame.sequence, frame.type,
                                    frame.set, frame.id, frame.payload, frame.payload_length);
        CHECK(length == k->length && memcmp(rebuilt, k->data, length) == 0);
        if (length != k->length || memcmp(rebuilt, k->data, k->length) != 0) {
            printf("  frame %s\n", k->label);
        }
    }
}

static void TestFields(void)
{
    SDL_DJIFrame frame;

    CHECK(SDL_DJI_CheckFrame(reply1, sizeof(reply1), &frame));
    CHECK(frame.sender == 0x06 && frame.receiver == 0x0A && frame.sequence == 0x34F6);
    CHECK(frame.type == 0x80 && frame.set == 0x06 && frame.id == 0x01);
    CHECK(frame.payload == reply1 + 11 && frame.payload[0] == 0x00 && frame.payload[1] == 0x94);

    CHECK(SDL_DJI_CheckFrame(simulator, sizeof(simulator), &frame));
    CHECK(frame.payload_length == 1 && frame.payload[0] == 0x01);
}

static void TestBuilder(void)
{
    uint8_t out[SDL_DJI_MAX_FRAME + 16];
    uint8_t payload[SDL_DJI_MAX_FRAME];
    const uint8_t on = 0x01;
    SDL_DJIFrame request;
    size_t length;

    /* Codec test 7 */
    CHECK(SDL_DJI_BuildFrame(out, sizeof(out), 0x0A, 0x06, 0x34EB, 0x40, 0x06, 0x24, &on, 1) == sizeof(simulator));
    CHECK(memcmp(out, simulator, sizeof(simulator)) == 0);
    CHECK(SDL_DJI_BuildFrame(out, sizeof(out), 0x0A, 0x06, 1, 0x40, 0x06, 0xF5, NULL, 0) == sizeof(test_stick));
    CHECK(memcmp(out, test_stick, sizeof(test_stick)) == 0);

    /* The response to a 00/81 beacon of sequence 0010 from 06 to 0A */
    memset(&request, 0, sizeof(request));
    request.sender = 0x06;
    request.receiver = 0x0A;
    request.sequence = 0x0010;
    request.type = 0x40;
    request.set = 0x00;
    request.id = 0x81;
    CHECK(SDL_DJI_BuildResponse(out, sizeof(out), &request) == sizeof(response));
    CHECK(memcmp(out, response, sizeof(response)) == 0);
    CHECK(SDL_DJI_BuildResponse(out, sizeof(out), NULL) == 0);

    /* The largest frame is 1023 bytes, and the length's high bits sit below the version */
    memset(payload, 0x55, sizeof(payload));
    length = SDL_DJI_BuildFrame(out, sizeof(out), 0x0A, 0x06, 7, 0x40, 0x06, 0x01, payload, SDL_DJI_MAX_FRAME - 13);
    CHECK(length == SDL_DJI_MAX_FRAME);
    CHECK(out[1] == 0xFF && out[2] == (0x03 | 0x04));
    CHECK(SDL_DJI_CheckFrame(out, length, NULL));
    CHECK(SDL_DJI_BuildFrame(out, sizeof(out), 0x0A, 0x06, 7, 0x40, 0x06, 0x01, payload, SDL_DJI_MAX_FRAME - 12) == 0);
    CHECK(SDL_DJI_BuildFrame(out, 12, 0x0A, 0x06, 7, 0x40, 0x06, 0x01, NULL, 0) == 0);
    CHECK(SDL_DJI_BuildFrame(out, 13, 0x0A, 0x06, 7, 0x40, 0x06, 0x01, NULL, 0) == 13);
    CHECK(SDL_DJI_BuildFrame(NULL, 13, 0x0A, 0x06, 7, 0x40, 0x06, 0x01, NULL, 0) == 0);
    CHECK(SDL_DJI_BuildFrame(out, sizeof(out), 0x0A, 0x06, 7, 0x40, 0x06, 0x01, NULL, 1) == 0);

    /* Lengths of 200 and 300 put 0 and 1 in the high bits, and the parser
       takes each whole */
    length = SDL_DJI_BuildFrame(out, sizeof(out), 0x0A, 0x06, 7, 0x40, 0x06, 0x01, payload, 200 - 13);
    CHECK(length == 200 && out[1] == 0xC8 && out[2] == 0x04);
    CHECK(SDL_DJI_CheckFrame(out, length, NULL));
    length = SDL_DJI_BuildFrame(out, sizeof(out), 0x0A, 0x06, 7, 0x40, 0x06, 0x01, payload, 300 - 13);
    CHECK(length == 300 && out[1] == 0x2C && out[2] == 0x05);
    CHECK(SDL_DJI_CheckFrame(out, length, NULL));
    {
        SDL_DJIParser parser;
        Collected *c = NewCollected();

        SDL_DJIParser_Init(&parser, false);
        Feed(&parser, out, length, c);
        CHECK(c->count == 1 && c->lengths[0] == 300 && c->fields[0].payload_length == 287 && parser.dropped == 0);
        free(c);
    }

    /* The envelope */
    CHECK(SDL_DJI_BuildFrame(out + 64, sizeof(out) - 64, 0x02, 0x06, 1, 0x00, 0x00, 0x01, NULL, 0) == 13);
    CHECK(SDL_DJI_BuildEnvelope(out, sizeof(out), out + 64, 13) == sizeof(keepalive));
    CHECK(memcmp(out, keepalive, sizeof(keepalive)) == 0);
    CHECK(SDL_DJI_BuildEnvelope(out, 20, keepalive + 8, 13) == 0);
    CHECK(SDL_DJI_BuildEnvelope(out, sizeof(out), keepalive + 8, 12) == 0);
}

static void TestCheckFrame(void)
{
    uint8_t bad[sizeof(reply1)];
    uint8_t buffer[sizeof(reply1) + 1];
    int version;

    CHECK(!SDL_DJI_CheckFrame(reply1, sizeof(reply1) - 1, NULL));
    CHECK(!SDL_DJI_CheckFrame(NULL, 13, NULL));
    memcpy(bad, reply1, sizeof(bad));
    bad[sizeof(bad) - 1] ^= 0x01;
    CHECK(!SDL_DJI_CheckFrame(bad, sizeof(bad), NULL));
    memcpy(bad, reply1, sizeof(bad));
    bad[3] ^= 0x01;
    CHECK(!SDL_DJI_CheckFrame(bad, sizeof(bad), NULL));
    memcpy(bad, reply1, sizeof(bad));
    bad[0] = 0x54;
    CHECK(!SDL_DJI_CheckFrame(bad, sizeof(bad), NULL));

    /* A buffer longer than its frame */
    memcpy(buffer, reply1, sizeof(reply1));
    buffer[sizeof(reply1)] = 0x00;
    CHECK(!SDL_DJI_CheckFrame(buffer, sizeof(reply1) + 1, NULL));
    CHECK(SDL_DJI_CheckFrame(buffer, sizeof(reply1), NULL));

    /* Each header check alone, under a CRC16 that matches */
    memcpy(bad, reply1, sizeof(bad));
    bad[3] ^= 0x01;
    Reseal(bad, sizeof(bad));
    CHECK(!SDL_DJI_CheckFrame(bad, sizeof(bad), NULL));
    for (version = 0; version < 64; ++version) {
        memcpy(bad, reply1, sizeof(bad));
        bad[2] = (uint8_t)((bad[2] & 0x03) | (version << 2));
        bad[3] = SDL_DJI_CRC8(bad, 3);
        Reseal(bad, sizeof(bad));
        CHECK(SDL_DJI_CheckFrame(bad, sizeof(bad), NULL) == (version == 1));
    }
}

/* Codec test 5: every proper prefix waits, the rest completes the frame,
 * and bytes past the count never reach a decoder */
static void TestPrefixes(void)
{
    size_t i, k;

    for (i = 0; i < SDL_arraysize_test(known); ++i) {
        const Known *f = &known[i];

        for (k = 1; k < f->length; ++k) {
            SDL_DJIParser parser;
            Collected *c = NewCollected();

            SDL_DJIParser_Init(&parser, false);
            Feed(&parser, f->data, k, c);
            CHECK(c->count == 0 && parser.length == k);
            Feed(&parser, f->data + k, f->length - k, c);
            CHECK(c->count == 1 && c->lengths[0] == f->length && memcmp(c->frames[0], f->data, f->length) == 0);
            CHECK(parser.length == 0 && parser.dropped == 0);
            free(c);
        }
        {
            SDL_DJIParser parser;
            Collected *c = NewCollected();

            SDL_DJIParser_Init(&parser, false);
            for (k = 0; k < f->length; ++k) {
                Feed(&parser, f->data + k, 1, c);
                CHECK(c->count == (k + 1 == f->length ? 1 : 0));
            }
            SDL_DJIParser_Init(&parser, false);
            c->count = 0;
            FeedPadded(&parser, f->data, f->length, c);
            CHECK(c->count == 1 && c->lengths[0] == f->length);
            free(c);
        }
    }
}

/* Codec test 6: each failure drops one byte and the next valid frame parses */
static void TestResync(void)
{
    uint8_t buffer[256];
    uint8_t bad[sizeof(reply1)];
    size_t length;
    SDL_DJIParser parser;
    Collected *c = NewCollected();

    /* Garbage before a frame */
    memcpy(buffer, "\x01\x02\xFF\x00", 4);
    memcpy(buffer + 4, reply1, sizeof(reply1));
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, buffer, 4 + sizeof(reply1), c);
    CHECK(c->count == 1 && c->lengths[0] == sizeof(reply1) && parser.dropped == 4);

    /* A lone 55 */
    c->count = 0;
    buffer[0] = 0x55;
    memcpy(buffer + 1, reply2, sizeof(reply2));
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, buffer, 1 + sizeof(reply2), c);
    CHECK(c->count == 1 && memcmp(c->frames[0], reply2, sizeof(reply2)) == 0 && parser.dropped == 1);

    /* A 55 with a bad header CRC8 */
    c->count = 0;
    memcpy(buffer, "\x55\x0D\x04\x34", 4);
    memcpy(buffer + 4, request5, sizeof(request5));
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, buffer, 4 + sizeof(request5), c);
    CHECK(c->count == 1 && memcmp(c->frames[0], request5, sizeof(request5)) == 0 && parser.dropped == 4);

    /* A valid header with a bad CRC16: its 23 bytes go one at a time */
    c->count = 0;
    memcpy(bad, reply1, sizeof(bad));
    bad[sizeof(bad) - 2] ^= 0x80;
    memcpy(buffer, bad, sizeof(bad));
    memcpy(buffer + sizeof(bad), status1, sizeof(status1));
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, buffer, sizeof(bad) + sizeof(status1), c);
    CHECK(c->count == 1 && memcmp(c->frames[0], status1, sizeof(status1)) == 0 && parser.dropped == sizeof(bad));

    /* A length below 13, with a matching header CRC8 */
    c->count = 0;
    buffer[0] = 0x55;
    buffer[1] = 0x0C;
    buffer[2] = 0x04;
    buffer[3] = SDL_DJI_CRC8(buffer, 3);
    memcpy(buffer + 4, request1, sizeof(request1));
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, buffer, 4 + sizeof(request1), c);
    CHECK(c->count == 1 && memcmp(c->frames[0], request1, sizeof(request1)) == 0 && parser.dropped == 4);

    /* Version 2, with a matching header CRC8 */
    c->count = 0;
    buffer[0] = 0x55;
    buffer[1] = 0x0D;
    buffer[2] = 0x08;
    buffer[3] = SDL_DJI_CRC8(buffer, 3);
    memcpy(buffer + 4, request2, sizeof(request2));
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, buffer, 4 + sizeof(request2), c);
    CHECK(c->count == 1 && memcmp(c->frames[0], request2, sizeof(request2)) == 0 && parser.dropped == 4);

    /* A header that fails a check drops at once, even under a length of
       1023: the frame behind it parses in the same read */
    c->count = 0;
    buffer[0] = 0x55;
    buffer[1] = 0xFF;
    buffer[2] = 0x07;
    buffer[3] = (uint8_t)(SDL_DJI_CRC8(buffer, 3) ^ 0x01);
    memcpy(buffer + 4, request5, sizeof(request5));
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, buffer, 4 + sizeof(request5), c);
    CHECK(c->count == 1 && memcmp(c->frames[0], request5, sizeof(request5)) == 0 && parser.dropped == 4);
    c->count = 0;
    buffer[2] = 0x0B;
    buffer[3] = SDL_DJI_CRC8(buffer, 3);
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, buffer, 4 + sizeof(request5), c);
    CHECK(c->count == 1 && memcmp(c->frames[0], request5, sizeof(request5)) == 0 && parser.dropped == 4);

    /* A 12-byte frame with both CRCs matching is not a frame: its bytes
       drop one at a time */
    c->count = 0;
    memcpy(buffer, "\x55\x0C\x04", 3);
    buffer[3] = SDL_DJI_CRC8(buffer, 3);
    memcpy(buffer + 4, "\x0A\x06\x01\x00\x40\x06", 6);
    Reseal(buffer, 12);
    CHECK(!SDL_DJI_CheckFrame(buffer, 12, NULL));
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, buffer, 12, c);
    CHECK(c->count == 0 && parser.dropped == 12 && parser.length == 0);

    /* Nor can one swallow the start of a real frame. The 12 bytes end in
       the real frame's sequence, which is the CRC16 they need. */
    c->count = 0;
    memcpy(buffer + 4, "\x55\x0D\x04\x33\x0A\x06", 6);
    {
        const uint16_t crc = SDL_DJI_CRC16(buffer, 10);

        length = SDL_DJI_BuildFrame(buffer + 4, sizeof(buffer) - 4, 0x0A, 0x06, crc, 0x40, 0x06, 0x01, NULL, 0);
        CHECK(length == 13 && buffer[10] == (crc & 0xFF) && buffer[11] == (crc >> 8));
    }
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, buffer, 4 + length, c);
    CHECK(c->count == 1 && memcmp(c->frames[0], buffer + 4, 13) == 0 && parser.dropped == 4);

    /* Frames back to back in one read, and a frame whose payload holds 55 */
    c->count = 0;
    length = 0;
    memcpy(buffer + length, reply1, sizeof(reply1));
    length += sizeof(reply1);
    memcpy(buffer + length, simulator, sizeof(simulator));
    length += sizeof(simulator);
    memcpy(buffer + length, status1, sizeof(status1));
    length += sizeof(status1);
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, buffer, length, c);
    CHECK(c->count == 3 && parser.dropped == 0);
    CHECK(c->lengths[0] == sizeof(reply1) && c->lengths[1] == sizeof(simulator) && c->lengths[2] == sizeof(status1));

    /* A handler of NULL takes the frames without calling anything */
    SDL_DJIParser_Init(&parser, false);
    SDL_DJIParser_Feed(&parser, buffer, length, NULL, NULL);
    CHECK(parser.length == 0);
    free(c);
}

/* A header claiming 1023 bytes waits for all of them, fails, and the search
 * finds the real frame inside */
static void TestLongestWait(void)
{
    uint8_t *buffer = (uint8_t *)calloc(1, 2048);
    SDL_DJIParser parser;
    Collected *c = NewCollected();
    size_t i;

    buffer[0] = 0x55;
    buffer[1] = 0xFF;
    buffer[2] = 0x07;
    buffer[3] = SDL_DJI_CRC8(buffer, 3);
    memcpy(buffer + 100, reply1, sizeof(reply1));
    SDL_DJIParser_Init(&parser, false);
    for (i = 0; i < 1022; ++i) {
        Feed(&parser, buffer + i, 1, c);
    }
    CHECK(c->count == 0 && parser.length == 1022);
    Feed(&parser, buffer + 1022, 1, c);
    CHECK(c->count == 1 && memcmp(c->frames[0], reply1, sizeof(reply1)) == 0);
    /* Every byte but the frame's is dropped, none of them a 55 */
    CHECK(parser.dropped == 1023 - sizeof(reply1) && parser.length == 0);

    /* One large feed takes the same path in slices */
    c->count = 0;
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, buffer, 2048, c);
    CHECK(c->count == 1 && parser.length == 0);
    free(c);
    free(buffer);
}

/* The TCP envelope */
static void TestEnvelope(void)
{
    uint8_t buffer[512];
    uint8_t frame[64];
    SDL_DJIParser parser;
    Collected *c = NewCollected();
    size_t length, k;

    SDL_DJIParser_Init(&parser, true);
    Feed(&parser, keepalive, sizeof(keepalive), c);
    CHECK(c->count == 1 && c->lengths[0] == 13 && c->fields[0].sender == 0x02 && c->fields[0].id == 0x01);

    /* Every prefix waits */
    for (k = 1; k < sizeof(keepalive); ++k) {
        c->count = 0;
        SDL_DJIParser_Init(&parser, true);
        Feed(&parser, keepalive, k, c);
        CHECK(c->count == 0 && parser.length == k);
        Feed(&parser, keepalive + k, sizeof(keepalive) - k, c);
        CHECK(c->count == 1);
    }

    /* A length above 1023 is skipped by one byte, and the next envelope parses */
    c->count = 0;
    memcpy(buffer, "\x55\xCC\x30\x75\x00\x04\x00\x00", 8);
    memcpy(buffer + 8, keepalive, sizeof(keepalive));
    SDL_DJIParser_Init(&parser, true);
    Feed(&parser, buffer, 8 + sizeof(keepalive), c);
    CHECK(c->count == 1 && parser.dropped == 8);

    /* A length that does not match the frame's own */
    c->count = 0;
    memcpy(buffer, keepalive, sizeof(keepalive));
    buffer[4] = 0x0E;
    buffer[sizeof(keepalive)] = 0x00;
    memcpy(buffer + sizeof(keepalive) + 1, keepalive, sizeof(keepalive));
    SDL_DJIParser_Init(&parser, true);
    Feed(&parser, buffer, 2 * sizeof(keepalive) + 1, c);
    /* Its 22 bytes drop one at a time */
    CHECK(c->count == 1 && parser.dropped == sizeof(keepalive) + 1);

    /* One byte too many, taken from the next envelope: that one parses */
    c->count = 0;
    memcpy(buffer, keepalive, sizeof(keepalive));
    buffer[4] = 0x0E;
    memcpy(buffer + sizeof(keepalive), keepalive, sizeof(keepalive));
    SDL_DJIParser_Init(&parser, true);
    Feed(&parser, buffer, 2 * sizeof(keepalive), c);
    CHECK(c->count == 1 && parser.dropped == sizeof(keepalive) && parser.length == 0);

    /* Two envelopes in one read both parse */
    c->count = 0;
    length = SDL_DJI_BuildFrame(frame, sizeof(frame), 0x06, 0x02, 9, 0x00, 0x06, 0x1E, status1 + 11, 6);
    CHECK(length == 19);
    memcpy(buffer, keepalive, sizeof(keepalive));
    CHECK(SDL_DJI_BuildEnvelope(buffer + sizeof(keepalive), sizeof(buffer) - sizeof(keepalive), frame, length) == 8 + length);
    SDL_DJIParser_Init(&parser, true);
    Feed(&parser, buffer, sizeof(keepalive) + 8 + length, c);
    CHECK(c->count == 2 && c->fields[1].id == 0x1E && c->fields[1].sequence == 9);

    /* A bare frame is not an envelope */
    c->count = 0;
    SDL_DJIParser_Init(&parser, true);
    Feed(&parser, reply1, sizeof(reply1), c);
    CHECK(c->count == 0);

    /* An envelope around the largest frame fits the parser whole */
    {
        uint8_t *payload = (uint8_t *)calloc(1, SDL_DJI_MAX_FRAME);
        uint8_t *big = (uint8_t *)calloc(1, SDL_DJI_MAX_FRAME);
        uint8_t *wrapped = (uint8_t *)calloc(1, SDL_DJI_ENVELOPE_LENGTH + SDL_DJI_MAX_FRAME);
        size_t big_length, wrapped_length;

        memset(payload, 0x5A, SDL_DJI_MAX_FRAME);
        big_length = SDL_DJI_BuildFrame(big, SDL_DJI_MAX_FRAME, 0x06, 0x02, 5, 0x00, 0x06, 0x1E, payload, SDL_DJI_MAX_FRAME - 13);
        wrapped_length = SDL_DJI_BuildEnvelope(wrapped, SDL_DJI_ENVELOPE_LENGTH + SDL_DJI_MAX_FRAME, big, big_length);
        CHECK(big_length == SDL_DJI_MAX_FRAME && wrapped_length == SDL_DJI_ENVELOPE_LENGTH + SDL_DJI_MAX_FRAME);
        c->count = 0;
        SDL_DJIParser_Init(&parser, true);
        for (k = 0; k < wrapped_length; k += 100) {
            Feed(&parser, wrapped + k, (wrapped_length - k < 100) ? wrapped_length - k : 100, c);
        }
        CHECK(c->count == 1 && c->lengths[0] == SDL_DJI_MAX_FRAME && parser.dropped == 0);
        free(wrapped);
        free(big);
        free(payload);
    }
    free(c);
}

int main(void)
{
    TestTables();
    TestKnownFrames();
    TestFields();
    TestBuilder();
    TestCheckFrame();
    TestPrefixes();
    TestResync();
    TestLongestWait();
    TestEnvelope();
    return H_Finish();
}
