/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_speedforce_proto.c, the
   Logitech Speed Force Wireless of hifihedgehog/SDL#33 Part 7. The numbers
   are the part's. No raw capture exists, so the packets are built from the
   WiiBrew descriptor and the evtest rest values. Every report is fed as an
   exact-size heap copy, and truncated inside a buffer of 0xFF bytes. */

#include "../src/joystick/hidapi/SDL_hidapi_speedforce_proto.h"

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

#define MAX_WRITES 8

typedef struct TestDongle
{
    bool feature_ok;
    int random_calls;
    uint8_t random_values[4];
    int writes;
    char kind[MAX_WRITES]; /* 'f' feature, 'o' output */
    uint8_t data[MAX_WRITES][16];
    size_t length[MAX_WRITES];
} TestDongle;

static void Record(TestDongle *dongle, char kind, const uint8_t *data, size_t length)
{
    CHECK(dongle->writes < MAX_WRITES && length <= 16);
    if (dongle->writes < MAX_WRITES && length <= 16) {
        dongle->kind[dongle->writes] = kind;
        memcpy(dongle->data[dongle->writes], data, length);
        dongle->length[dongle->writes] = length;
    }
    ++dongle->writes;
}

static bool TestFeature(void *userdata, const uint8_t *data, size_t length)
{
    TestDongle *dongle = (TestDongle *)userdata;

    Record(dongle, 'f', data, length);
    return dongle->feature_ok;
}

static bool TestOutput(void *userdata, const uint8_t *data, size_t length)
{
    Record((TestDongle *)userdata, 'o', data, length);
    return true;
}

static uint8_t TestRandom(void *userdata)
{
    TestDongle *dongle = (TestDongle *)userdata;
    uint8_t value = dongle->random_values[dongle->random_calls % 4];

    ++dongle->random_calls;
    return value;
}

static SDL_SpeedForceSink Sink(TestDongle *dongle)
{
    SDL_SpeedForceSink sink;

    sink.userdata = dongle;
    sink.feature = TestFeature;
    sink.output = TestOutput;
    sink.random = TestRandom;
    return sink;
}

static bool Decode(const uint8_t *report, size_t length, SDL_SpeedForceState *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    memset(out, 0xA5, sizeof(*out));
    result = SDL_SpeedForce_DecodeReport(copy, length, out);
    free(copy);
    return result;
}

static void TestBond(void)
{
    static const uint8_t af[9] = { 0x00, 0xAF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t b2[9] = { 0x00, 0xB2, 0x12, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t f5[8] = { 0x00, 0xF5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    TestDongle dongle;
    SDL_SpeedForceSink sink = Sink(&dongle);
    SDL_SpeedForceBond bond;
    uint64_t t;

    /* 1: AF at 0 ms, nothing before 40 ms, then B2 with the random bytes and F5 */
    memset(&dongle, 0, sizeof(dongle));
    dongle.feature_ok = true;
    dongle.random_values[0] = 0x12;
    dongle.random_values[1] = 0x34;
    SDL_SpeedForce_StartBond(&bond, 1000, &sink);
    CHECK(dongle.writes == 1 && dongle.kind[0] == 'f' && dongle.length[0] == 9 && memcmp(dongle.data[0], af, 9) == 0);
    CHECK(dongle.random_calls == 0);
    for (t = 1000; t < 1040; ++t) {
        CHECK(!SDL_SpeedForce_UpdateBond(&bond, t, &sink));
    }
    CHECK(dongle.writes == 1);
    CHECK(SDL_SpeedForce_UpdateBond(&bond, 1040, &sink));
    CHECK(dongle.writes == 3);
    CHECK(dongle.kind[1] == 'f' && dongle.length[1] == 9 && memcmp(dongle.data[1], b2, 9) == 0);
    CHECK(dongle.kind[2] == 'o' && dongle.length[2] == 8 && memcmp(dongle.data[2], f5, 8) == 0);
    /* Nothing more, ever */
    for (t = 1041; t < 5000; t += 7) {
        CHECK(SDL_SpeedForce_UpdateBond(&bond, t, &sink));
    }
    CHECK(dongle.writes == 3 && dongle.random_calls == 2);

    /* 2: a failed AF sends no B2 and waits for nothing, and F5 still follows */
    memset(&dongle, 0, sizeof(dongle));
    dongle.feature_ok = false;
    SDL_SpeedForce_StartBond(&bond, 0, &sink);
    CHECK(dongle.writes == 2);
    CHECK(dongle.kind[0] == 'f' && memcmp(dongle.data[0], af, 9) == 0);
    CHECK(dongle.kind[1] == 'o' && memcmp(dongle.data[1], f5, 8) == 0);
    CHECK(SDL_SpeedForce_UpdateBond(&bond, 0, &sink));
    CHECK(SDL_SpeedForce_UpdateBond(&bond, 40, &sink));
    CHECK(dongle.writes == 2 && dongle.random_calls == 0);

    /* 9: a reconnect bonds again from the start */
    memset(&dongle, 0, sizeof(dongle));
    dongle.feature_ok = true;
    dongle.random_values[0] = 0x56;
    dongle.random_values[1] = 0x78;
    SDL_SpeedForce_StartBond(&bond, 7, &sink);
    CHECK(!SDL_SpeedForce_UpdateBond(&bond, 46, &sink));
    CHECK(SDL_SpeedForce_UpdateBond(&bond, 47, &sink));
    CHECK(dongle.writes == 3 && dongle.data[1][2] == 0x56 && dongle.data[1][3] == 0x78);
}

static void TestInput(void)
{
    SDL_SpeedForceState s, rest;
    uint8_t r[5];
    static const uint8_t button_bytes[11][2] = {
        { 1, 0x12 }, { 1, 0x22 }, { 1, 0x42 }, { 1, 0x82 },
        { 2, 0x01 }, { 2, 0x02 }, { 2, 0x04 }, { 2, 0x08 }, { 2, 0x10 }, { 2, 0x20 }, { 2, 0x40 },
    };
    int i;

    /* 3: at rest */
    r[0] = 0x08;
    r[1] = 0x02;
    r[2] = 0x00;
    r[3] = 0xFF;
    r[4] = 0xFF;
    CHECK(Decode(r, 5, &s));
    CHECK(s.axes[0] == 544 && s.axes[1] == 32767 && s.axes[2] == 32767 && s.buttons == 0);
    SDL_SpeedForce_RestState(&rest);
    CHECK(memcmp(&rest, &s, sizeof(s)) == 0);

    /* 4: each button alone */
    for (i = 0; i < 11; ++i) {
        r[0] = 0x08;
        r[1] = 0x02;
        r[2] = 0x00;
        r[button_bytes[i][0]] = button_bytes[i][1];
        CHECK(Decode(r, 5, &s) && s.buttons == (1u << i) && s.axes[0] == 544);
    }

    /* 5: the vendor bits 10, 11 and 23 change nothing */
    r[0] = 0x08;
    r[1] = 0x0E;
    r[2] = 0x80;
    CHECK(Decode(r, 5, &s) && memcmp(&s, &rest, sizeof(s)) == 0);

    /* 6: the ends of the wheel and pedals */
    memset(r, 0, 5);
    CHECK(Decode(r, 5, &s) && s.axes[0] == -32768 && s.axes[1] == -32768 && s.axes[2] == -32768);
    r[0] = 0xFF;
    r[1] = 0x03;
    r[3] = 0xFF;
    r[4] = 0xFF;
    CHECK(Decode(r, 5, &s) && s.axes[0] == 32767 && s.buttons == 0);

    /* 11: each pedal alone */
    r[0] = 0x08;
    r[1] = 0x02;
    r[2] = 0x00;
    r[3] = 0x00;
    r[4] = 0xFF;
    CHECK(Decode(r, 5, &s) && s.axes[1] == -32768 && s.axes[2] == 32767 && s.axes[0] == 544 && s.buttons == 0);
    r[3] = 0xFF;
    r[4] = 0x00;
    CHECK(Decode(r, 5, &s) && s.axes[1] == 32767 && s.axes[2] == -32768 && s.axes[0] == 544 && s.buttons == 0);

    /* Every wheel value maps monotonically onto the full range */
    for (i = 1; i < 1024; ++i) {
        SDL_SpeedForceState a, b;
        r[0] = (uint8_t)((i - 1) & 0xFF);
        r[1] = (uint8_t)((i - 1) >> 8);
        CHECK(Decode(r, 5, &a));
        r[0] = (uint8_t)(i & 0xFF);
        r[1] = (uint8_t)(i >> 8);
        CHECK(Decode(r, 5, &b));
        CHECK(b.axes[0] > a.axes[0] && a.buttons == 0 && b.buttons == 0);
    }
}

static void TestFraming(void)
{
    static const uint8_t packets[2][5] = {
        { 0x08, 0x02, 0x00, 0xFF, 0xFF },
        { 0x08, 0x0E, 0x80, 0xFF, 0xFF },
    };
    SDL_SpeedForceState s;
    uint8_t stale[16], six[6];
    size_t length;
    int p;

    /* 7: every prefix of tests 3 and 5 changes nothing, with 0xFF past the received length */
    for (p = 0; p < 2; ++p) {
        for (length = 0; length < 5; ++length) {
            CHECK(!Decode(packets[p], length, &s));
            memset(stale, 0xFF, sizeof(stale));
            memcpy(stale, packets[p], length);
            CHECK(!SDL_SpeedForce_DecodeReport(stale, length, &s));
        }
    }
    /* 8: a 6-byte read is ignored */
    memcpy(six, packets[0], 5);
    six[5] = 0x00;
    CHECK(!Decode(six, 6, &s));
    CHECK(!SDL_SpeedForce_DecodeReport(NULL, 5, &s));
    CHECK(!SDL_SpeedForce_DecodeReport(packets[0], 5, NULL));
}

static void TestOutputEncoder(void)
{
    static const uint8_t stop[8] = { 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t plus20[8] = { 0x00, 0x11, 0x08, 0xA0, 0x80, 0x00, 0x00, 0x00 };
    static const uint8_t minus90[8] = { 0x00, 0x11, 0x08, 0x00, 0x80, 0x00, 0x00, 0x00 };
    uint8_t out[8];
    int level;

    /* 10 */
    memset(out, 0xEE, sizeof(out));
    SDL_SpeedForce_BuildConstantForce(0, out);
    CHECK(memcmp(out, stop, 8) == 0);
    SDL_SpeedForce_BuildConstantForce(0x20, out);
    CHECK(memcmp(out, plus20, 8) == 0);
    SDL_SpeedForce_BuildConstantForce(-0x90, out);
    CHECK(memcmp(out, minus90, 8) == 0);
    SDL_SpeedForce_BuildConstantForce(0x7F, out);
    CHECK(out[3] == 0xFF);
    SDL_SpeedForce_BuildConstantForce(0x200, out);
    CHECK(out[3] == 0xFF);
    for (level = -0x80; level <= 0x7F; ++level) {
        if (level == 0) {
            continue;
        }
        SDL_SpeedForce_BuildConstantForce(level, out);
        CHECK(out[0] == 0x00 && out[1] == 0x11 && out[2] == 0x08 && out[3] == (uint8_t)(level + 0x80) && out[4] == 0x80);
        CHECK(out[5] == 0 && out[6] == 0 && out[7] == 0);
    }
}

int main(void)
{
    TestBond();
    TestInput();
    TestFraming();
    TestOutputEncoder();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
