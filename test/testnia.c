/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_nia_proto.c, the OCZ NIA
   of hifihedgehog/SDL#33 Part 7. Tests 1 to 3 are the three reports
   drwho.virtadpt.net quotes from the OCZ forum. Tests 6 and 9 use the report
   endings and samples the part quotes from the nia4linux Windows logs. */

#include "../src/joystick/hidapi/SDL_hidapi_nia_proto.h"

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

/* A report from its sample bytes, filled up with the unused triplet 00 12 7A, then the tail */
static void Build(uint8_t report[55], const uint8_t *samples, int sample_bytes, const uint8_t tail[7])
{
    int i;

    for (i = 0; i < 48; i += 3) {
        report[i] = 0x00;
        report[i + 1] = 0x12;
        report[i + 2] = 0x7A;
    }
    memcpy(report, samples, (size_t)sample_bytes);
    memcpy(&report[48], tail, 7);
}

static bool Handle(SDL_NIAState *state, const uint8_t *report, size_t length, SDL_NIAReport *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    result = SDL_NIA_HandleReport(state, copy, length, out);
    free(copy);
    return result;
}

static void TestCaptures(void)
{
    static const uint8_t samples1[24] = {
        0xA2, 0xFA, 0x7F, 0xBA, 0xFA, 0x7F, 0x92, 0xFA, 0x7F, 0xC2, 0xFA, 0x7F,
        0x72, 0xFA, 0x7F, 0xDB, 0xFA, 0x7F, 0x73, 0xFA, 0x7F, 0xDB, 0xFA, 0x7F
    };
    static const uint8_t tail1[7] = { 0x38, 0xBD, 0xFA, 0xFF, 0x51, 0x54, 0x08 };
    static const int32_t expect1[8] = { -1374, -1350, -1390, -1342, -1422, -1317, -1421, -1317 };
    static const uint8_t samples2[12] = { 0x73, 0xFA, 0x7F, 0xDB, 0xFA, 0x7F, 0x73, 0xFA, 0x7F, 0xDC, 0xFA, 0x7F };
    static const uint8_t tail2[7] = { 0x38, 0xBD, 0xFA, 0xFF, 0x55, 0x54, 0x04 };
    static const int32_t expect2[4] = { -1421, -1317, -1421, -1316 };
    static const uint8_t samples3[24] = {
        0x74, 0xFA, 0x7F, 0xC4, 0xFA, 0x7F, 0x94, 0xFA, 0x7F, 0xBC, 0xFA, 0x7F,
        0xA5, 0xFA, 0x7F, 0xA5, 0xFA, 0x7F, 0xBD, 0xFA, 0x7F, 0x95, 0xFA, 0x7F
    };
    static const uint8_t tail3[7] = { 0x38, 0xBD, 0xFA, 0xFF, 0x5D, 0x54, 0x08 };
    static const int32_t expect3[8] = { -1420, -1340, -1388, -1348, -1371, -1371, -1347, -1387 };
    SDL_NIAState state;
    SDL_NIAReport report;
    uint8_t r[55];
    int i;

    SDL_NIA_Init(&state);
    CHECK(state.axis == 0 && !state.have_counter);

    /* 1 */
    Build(r, samples1, 24, tail1);
    CHECK(Handle(&state, r, 55, &report));
    CHECK(report.count == 8 && report.counter == 0x5451);
    for (i = 0; i < 8; ++i) {
        CHECK(report.samples[i] == expect1[i]);
    }
    CHECK(state.axis == -5 && !state.gap);

    /* 2: the counter advanced by 4 */
    Build(r, samples2, 12, tail2);
    CHECK(Handle(&state, r, 55, &report));
    CHECK(report.count == 4 && report.counter == 0x5455);
    for (i = 0; i < 4; ++i) {
        CHECK(report.samples[i] == expect2[i]);
    }
    CHECK(state.axis == -5 && !state.gap);

    /* 3: the counter advanced by 8 */
    Build(r, samples3, 24, tail3);
    CHECK(Handle(&state, r, 55, &report));
    CHECK(report.count == 8 && report.counter == 0x545D);
    for (i = 0; i < 8; ++i) {
        CHECK(report.samples[i] == expect3[i]);
    }
    CHECK(state.axis == -5 && !state.gap);

    /* 4: a count of 0 yields no samples and leaves the axis, 17 yields 16, and triplets past the count are never read */
    Build(r, samples1, 24, tail1);
    r[54] = 0;
    r[0] = 0xFF;
    r[1] = 0xFF;
    r[2] = 0xFF;
    CHECK(Handle(&state, r, 55, &report) && report.count == 0 && state.axis == -5);
    Build(r, samples1, 24, tail1);
    r[54] = 17;
    CHECK(Handle(&state, r, 55, &report) && report.count == 16);
    CHECK(report.samples[15] == 0x7A1200 - 0x800000);
    Build(r, samples1, 24, tail1);
    r[24] = 0xFF;
    r[25] = 0xFF;
    r[26] = 0xFF; /* sample 9, past the count of 8 */
    CHECK(Handle(&state, r, 55, &report) && report.count == 8 && report.samples[7] == -1317 && report.samples[8] == 0);
}

static void TestFraming(void)
{
    static const uint8_t tail1[7] = { 0x38, 0xBD, 0xFA, 0xFF, 0x51, 0x54, 0x08 };
    SDL_NIAState state, before;
    SDL_NIAReport report;
    uint8_t r[55], stale[64];
    size_t length;

    /* 5: 0 to 54 bytes yield nothing, with 0xFF past the received length */
    Build(r, (const uint8_t *)"\xA2\xFA\x7F", 3, tail1);
    SDL_NIA_Init(&state);
    for (length = 0; length < 55; ++length) {
        before = state;
        CHECK(!Handle(&state, r, length, &report));
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, r, length);
        CHECK(!SDL_NIA_HandleReport(&state, stale, length, &report));
        CHECK(memcmp(&state, &before, sizeof(state)) == 0);
    }
    memcpy(stale, r, 55);
    CHECK(!Handle(&state, stale, 56, &report));
    CHECK(!SDL_NIA_DecodeReport(NULL, 55, &report));
    CHECK(!SDL_NIA_DecodeReport(r, 55, NULL));
    CHECK(!SDL_NIA_HandleReport(NULL, r, 55, &report));
}

static void TestCounter(void)
{
    static const uint8_t tail_a[7] = { 0x38, 0xBD, 0xF7, 0xFF, 0xFF, 0x86, 0x04 };
    static const uint8_t tail_b[7] = { 0x38, 0xBD, 0xF7, 0xFF, 0x03, 0x87, 0x04 };
    static const uint8_t four[12] = { 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80 };
    SDL_NIAState state;
    SDL_NIAReport report;
    uint8_t r[55];

    /* 6: 0x86FF then 0x8703 with a count of 4 is no gap. A counter that does not advance by the count is. */
    SDL_NIA_Init(&state);
    Build(r, four, 12, tail_a);
    CHECK(Handle(&state, r, 55, &report) && !state.gap);
    Build(r, four, 12, tail_b);
    CHECK(Handle(&state, r, 55, &report) && !state.gap && report.counter == 0x8703);
    Build(r, four, 12, tail_b);
    CHECK(Handle(&state, r, 55, &report) && state.gap && report.count == 4 && report.samples[3] == 0);
    /* The counter wraps at 65536 */
    SDL_NIA_Init(&state);
    Build(r, four, 12, tail_a);
    r[52] = 0xFE;
    r[53] = 0xFF;
    CHECK(Handle(&state, r, 55, &report) && !state.gap);
    r[52] = 0x02;
    r[53] = 0x00;
    CHECK(Handle(&state, r, 55, &report) && !state.gap);
    /* A count byte above 16 advances the counter by the byte, though only
       16 samples fit the report */
    SDL_NIA_Init(&state);
    Build(r, four, 12, tail_a);
    r[52] = 0x00;
    r[53] = 0x10;
    r[54] = 20;
    CHECK(Handle(&state, r, 55, &report) && report.count == 16 && report.raw_count == 20 && !state.gap);
    r[52] = 0x14;
    CHECK(Handle(&state, r, 55, &report) && !state.gap);
    r[52] = 0x24;
    CHECK(Handle(&state, r, 55, &report) && state.gap);

    /* 8: a reconnect clears the history and centers the axis */
    SDL_NIA_Init(&state);
    CHECK(!state.have_counter && state.axis == 0 && !state.gap);
    Build(r, four, 12, tail_a);
    CHECK(Handle(&state, r, 55, &report) && !state.gap);
}

static void TestScaling(void)
{
    static const uint8_t samples9[12] = { 0x09, 0x90, 0x80, 0x34, 0x23, 0x80, 0x6B, 0xA7, 0x7F, 0x36, 0x5C, 0x7F };
    static const uint8_t tail4[7] = { 0x38, 0xBD, 0xF7, 0xFF, 0x00, 0x00, 0x04 };
    static const uint8_t tail1[7] = { 0x38, 0xBD, 0xF7, 0xFF, 0x00, 0x00, 0x01 };
    SDL_NIAState state;
    SDL_NIAReport report;
    uint8_t r[55];

    /* 9 */
    SDL_NIA_Init(&state);
    Build(r, samples9, 12, tail4);
    CHECK(Handle(&state, r, 55, &report) && report.count == 4);
    CHECK(report.samples[0] == 36873 && report.samples[1] == 9012 && report.samples[2] == -22677 && report.samples[3] == -41930);
    CHECK(state.axis == -163);
    Build(r, (const uint8_t *)"\x00\x00\x80", 3, tail1);
    CHECK(Handle(&state, r, 55, &report) && state.axis == 0);
    Build(r, (const uint8_t *)"\xFF\xFF\xFF", 3, tail1);
    CHECK(Handle(&state, r, 55, &report) && state.axis == 32767);
    Build(r, (const uint8_t *)"\x00\x00\x00", 3, tail1);
    CHECK(Handle(&state, r, 55, &report) && state.axis == -32768);
    CHECK(SDL_NIA_SampleAxis(8388607) == 32767 && SDL_NIA_SampleAxis(-8388608) == -32768);
    CHECK(SDL_NIA_SampleAxis(255) == 0 && SDL_NIA_SampleAxis(-255) == 0 && SDL_NIA_SampleAxis(-256) == -1);
}

static void TestIdentity(void)
{
    CHECK(SDL_NIA_IsManufacturer("Brain Actuated Technologies"));
    CHECK(!SDL_NIA_IsManufacturer("Brain Actuated Technologies Inc"));
    CHECK(!SDL_NIA_IsManufacturer(""));
    CHECK(!SDL_NIA_IsManufacturer(NULL));
}

int main(void)
{
    TestCaptures();
    TestFraming();
    TestCounter();
    TestScaling();
    TestIdentity();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
