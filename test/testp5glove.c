/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_p5glove_proto.c, the P5
   Glove of hifihedgehog/SDL#33 Part 7. Tests 1 and 2 are built from
   libp5glove's layout. Tests 5 to 7 are the feature replies of the vendor
   SDK's Windows capture in libp5glove's doc/usb-dump.txt. */

#include "../src/joystick/hidapi/SDL_hidapi_p5glove_proto.h"

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

static const uint8_t test1[24] = {
    0x01, 0x29, 0x47, 0xA8, 0xC8, 0x53, 0xFF, 0xF1, 0x93, 0x9C, 0x7F, 0xC0,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* count bits of value at bit position first, most significant bit first, as libp5glove reads them */
static void PutBits(uint8_t *data, int first, int count, uint32_t value)
{
    int i;

    for (i = 0; i < count; ++i) {
        const int bit = first + i;
        const int set = (int)((value >> (count - 1 - i)) & 1);
        data[bit >> 3] = (uint8_t)((data[bit >> 3] & ~(0x80 >> (bit & 7))) | (set ? (0x80 >> (bit & 7)) : 0));
    }
}

static bool Decode(const uint8_t *report, size_t length, SDL_P5GloveState *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    memset(out, 0xA5, sizeof(*out));
    result = SDL_P5Glove_DecodeReport(copy, length, out);
    free(copy);
    return result;
}

static void TestReport(void)
{
    static const uint8_t finger_heads[5][5] = {
        { 0xFC, 0x00, 0x00, 0x00, 0x00 },
        { 0x03, 0xF0, 0x00, 0x00, 0x00 },
        { 0x00, 0x0F, 0xC0, 0x00, 0x00 },
        { 0x00, 0x00, 0x3F, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0xFC, 0x00 },
    };
    SDL_P5GloveState s;
    uint8_t r[24];
    int i, j;

    /* 1 */
    CHECK(Decode(test1, 24, &s));
    CHECK(s.fingers[0] == 10 && s.fingers[1] == 20 && s.fingers[2] == 30 && s.fingers[3] == 40 && s.fingers[4] == 50);
    CHECK(s.buttons == ((1 << 0) | (1 << 2))); /* A and C */
    CHECK(s.slots[0].led == 3);
    CHECK(s.slots[0].values[0] == 100 && s.slots[0].values[1] == -100 && s.slots[0].values[2] == 511);
    for (i = 1; i < 4; ++i) {
        CHECK(s.slots[i].led == SDL_P5GLOVE_EMPTY_SLOT);
    }

    /* 2: each finger alone at 63, then each button bit alone */
    for (i = 0; i < 5; ++i) {
        memset(r, 0, sizeof(r));
        r[0] = 0x01;
        memcpy(&r[1], finger_heads[i], 4);
        r[5] = 0x0F;
        r[6] = 0xFF;
        r[7] = 0xF0;
        CHECK(Decode(r, 24, &s));
        for (j = 0; j < 5; ++j) {
            CHECK(s.fingers[j] == (j == i ? 63 : 0));
            CHECK(SDL_P5Glove_FingerAxis(s.fingers[j]) == (j == i ? 32767 : -32768));
        }
        CHECK(s.buttons == 0);
        for (j = 0; j < 4; ++j) {
            CHECK(s.slots[j].led == SDL_P5GLOVE_EMPTY_SLOT);
        }
    }
    for (i = 0; i < 4; ++i) {
        memset(r, 0, sizeof(r));
        r[0] = 0x01;
        r[5] = (uint8_t)((0x10 << i) | 0x0F);
        r[6] = 0xFF;
        r[7] = 0xF0;
        CHECK(Decode(r, 24, &s) && s.buttons == (1 << i));
    }

    /* Every field through an independent bit writer: slot i at bit 60 + 30i, three values each */
    {
        static const int values[4][3] = { { -512, 511, -1 }, { 0, 1, -2 }, { 256, -256, 100 }, { -512, 511, -300 } };
        memset(r, 0, sizeof(r));
        r[0] = 0x01;
        for (i = 0; i < 5; ++i) {
            PutBits(r, 8 + 6 * i, 6, (uint32_t)(i * 13 + 1));
        }
        PutBits(r, 40, 4, 0x9);
        for (i = 0; i < 4; ++i) {
            PutBits(r, 44 + 4 * i, 4, (uint32_t)(i == 2 ? 0x8 + 5 : i * 2 + 1));
            for (j = 0; j < 3; ++j) {
                PutBits(r, 60 + 30 * i + 10 * j, 10, (uint32_t)(values[i][j] & 0x3FF));
            }
        }
        CHECK(Decode(r, 24, &s));
        for (i = 0; i < 5; ++i) {
            CHECK(s.fingers[i] == i * 13 + 1);
        }
        CHECK(s.buttons == 0x9);
        for (i = 0; i < 4; ++i) {
            CHECK(s.slots[i].led == (i == 2 ? SDL_P5GLOVE_EMPTY_SLOT : i * 2 + 1));
            for (j = 0; j < 3; ++j) {
                CHECK(s.slots[i].values[j] == values[i][j]);
            }
        }
        /* Bits 180-191 carry nothing */
        r[22] |= 0x0F;
        r[23] = 0xFF;
        {
            SDL_P5GloveState t;
            CHECK(Decode(r, 24, &t) && memcmp(&s, &t, sizeof(s)) == 0);
        }
    }
}

static void TestFraming(void)
{
    SDL_P5GloveState s;
    uint8_t r[24], stale[32];
    size_t length;

    /* 3: other report IDs, 3 and 8 among them */
    memcpy(r, test1, 24);
    r[0] = 0x03;
    CHECK(!Decode(r, 24, &s));
    r[0] = 0x08;
    CHECK(!Decode(r, 24, &s));
    r[0] = 0x00;
    CHECK(!Decode(r, 24, &s));

    /* 4: every prefix changes nothing, with 0xFF past the received length */
    for (length = 0; length < 24; ++length) {
        CHECK(!Decode(test1, length, &s));
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, test1, length);
        CHECK(!SDL_P5Glove_DecodeReport(stale, length, &s));
    }
    memcpy(stale, test1, 24);
    CHECK(!Decode(stale, 25, &s));
    CHECK(!SDL_P5Glove_DecodeReport(NULL, 24, &s));
    CHECK(!SDL_P5Glove_DecodeReport(test1, 24, NULL));
}

static void TestFeatures(void)
{
    static const uint8_t feature12[62] = {
        0x0C, 0x00, 0xFF, 0x01, 0x00, 0x0C, 0x00, 0xAD, 0xFF, 0x0D, 0xFF, 0xE5, 0xFF, 0xE4, 0xFF, 0x50,
        0x00, 0x1E, 0xFF, 0xCF, 0xFF, 0xDC, 0x00, 0x66, 0xFF, 0xF6, 0x00, 0xA9, 0xFF, 0xEF, 0xFF, 0xA5,
        0x00, 0xCC, 0x00, 0x03, 0x00, 0xD6, 0xFE, 0xEA, 0xFF, 0xC1, 0x00, 0x9B, 0x00, 0xC6, 0xFF, 0xB8,
        0x00, 0xD9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t feature6[6] = { 0x06, 0xED, 0xED, 0xB3, 0x1D, 0x4F };
    static const uint8_t mouse_off[2] = { 0x05, 0xFF };
    static const uint8_t mouse_on[2] = { 0x05, 0x01 };
    SDL_P5GloveLEDPositions leds;
    SDL_P5GloveTanCompensation tan;
    bool on;

    /* 5: glove type 0, LED 0 at raw -255, 12, 173, the first axis negated */
    CHECK(SDL_P5Glove_ParseLEDPositions(feature12, 62, &leds));
    CHECK(leds.glove_type == 0);
    CHECK(leds.positions[0][0] == 255 && leds.positions[0][1] == 12 && leds.positions[0][2] == 173);
    CHECK(leds.positions[1][0] == 243 && leds.positions[1][1] == -27 && leds.positions[1][2] == -28);
    CHECK(!SDL_P5Glove_ParseLEDPositions(feature12, 61, &leds));

    /* 6 */
    CHECK(SDL_P5Glove_ParseTanCompensation(feature6, 6, &tan));
    CHECK(tan.angles[0] == -19 && tan.angles[1] == -19 && tan.angles[2] == -77 && tan.angles[3] == 29);
    CHECK(tan.separation == 79);
    CHECK(!SDL_P5Glove_ParseTanCompensation(feature6, 5, &tan));

    /* 7 */
    CHECK(SDL_P5Glove_ParseMouseMode(mouse_off, 2, &on) && !on);
    CHECK(SDL_P5Glove_ParseMouseMode(mouse_on, 2, &on) && on);
    CHECK(!SDL_P5Glove_ParseMouseMode(mouse_on, 1, &on));
    CHECK(!SDL_P5Glove_ParseMouseMode(feature6, 6, &on));
}

/* 8: the decode keeps no state, so nothing outlives a reconnect: after a
   report with every bit set, one with none holds no button and every
   finger at 0. The module has no output, and the driver writes nothing on
   open. */
static void TestReconnect(void)
{
    uint8_t r[SDL_P5GLOVE_REPORT_LENGTH];
    SDL_P5GloveState state;
    int i;

    memset(r, 0xFF, sizeof(r));
    r[0] = SDL_P5GLOVE_REPORT_ID;
    CHECK(SDL_P5Glove_DecodeReport(r, sizeof(r), &state) && state.buttons == 0x0F && state.fingers[0] == 63);
    memset(r, 0x00, sizeof(r));
    r[0] = SDL_P5GLOVE_REPORT_ID;
    CHECK(SDL_P5Glove_DecodeReport(r, sizeof(r), &state) && state.buttons == 0);
    for (i = 0; i < SDL_P5GLOVE_FINGERS; ++i) {
        CHECK(state.fingers[i] == 0);
    }
}

int main(void)
{
    TestReport();
    TestFraming();
    TestFeatures();
    TestReconnect();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
