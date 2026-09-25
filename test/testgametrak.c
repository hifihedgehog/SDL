/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_gametrak_proto.c, the
   Gametrak of hifihedgehog/SDL#33 Part 7. Tests 3 to 7 and 12 replay
   GameTrak-Liberation's 10-second recording, whose report 1 is the unlock
   answer. The report numbers are the recording's. */

#include "../src/joystick/hidapi/SDL_hidapi_gametrak_proto.h"
#include "testgametrak_capture.h"

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

#define MAX_WRITES 16

typedef struct TestUnit
{
    int writes;
    uint8_t data[MAX_WRITES][8];
    size_t length[MAX_WRITES];
    int after_report[MAX_WRITES]; /* The recording's report number the write followed, or 0 */
    int current_report;
} TestUnit;

static bool TestWrite(void *userdata, const uint8_t *data, size_t length)
{
    TestUnit *unit = (TestUnit *)userdata;

    CHECK(length <= 8);
    if (unit->writes < MAX_WRITES && length <= 8) {
        memcpy(unit->data[unit->writes], data, length);
        unit->length[unit->writes] = length;
        unit->after_report[unit->writes] = unit->current_report;
    }
    ++unit->writes;
    return true;
}

static SDL_GametrakSink Sink(TestUnit *unit)
{
    SDL_GametrakSink sink;

    sink.userdata = unit;
    sink.write = TestWrite;
    return sink;
}

static bool Handle(SDL_GametrakSession *session, const uint8_t *report, size_t length, const SDL_GametrakSink *sink,
                   SDL_GametrakReport *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    result = SDL_Gametrak_HandleReport(session, 0, copy, length, sink, out);
    free(copy);
    return result;
}

static int16_t Scale(int value)
{
    return (int16_t)(value * 65535 / 2047 - 32768);
}

static bool Written(const TestUnit *unit, int index, const uint8_t *data, size_t length)
{
    return index < unit->writes && unit->length[index] == length && memcmp(unit->data[index], data, length) == 0;
}

static void TestKey(void)
{
    static const uint8_t expected[12] = { 0x8D, 0x35, 0xDD, 0x15, 0xDB, 0xD9, 0xE3, 0x3B, 0x9B, 0x55, 0x1B, 0x0B };
    uint32_t key = SDL_GAMETRAK_INITIAL_KEY;
    int i;

    for (i = 0; i < 12; ++i) {
        key = SDL_Gametrak_NextKey(key);
        CHECK((key & 0xFF) == expected[i]);
        CHECK(key <= 0xFFFFFF);
    }
}

static void TestUnlock(void)
{
    static const uint8_t unlock[8] = { 0x47, 0x61, 0x6D, 0x65, 0x74, 0x72, 0x61, 0x6B };
    static const uint8_t start[2] = { 0x45, 0x23 };
    TestUnit unit;
    SDL_GametrakSink sink = Sink(&unit);
    SDL_GametrakSession session;
    SDL_GametrakReport report;
    uint64_t t;

    /* 1: "Gametrak", then 45 23 when the answer arrives */
    memset(&unit, 0, sizeof(unit));
    SDL_Gametrak_Open(&session, 500, &sink);
    CHECK(unit.writes == 1 && Written(&unit, 0, unlock, 8));
    CHECK(!Handle(&session, gametrak_capture[0], 16, &sink, &report));
    CHECK(unit.writes == 2 && Written(&unit, 1, start, 2));
    SDL_Gametrak_Update(&session, 600, &sink);
    CHECK(unit.writes == 2);

    /* 1: or when 10 ms pass without it */
    memset(&unit, 0, sizeof(unit));
    SDL_Gametrak_Open(&session, 500, &sink);
    for (t = 500; t < 510; ++t) {
        SDL_Gametrak_Update(&session, t, &sink);
    }
    CHECK(unit.writes == 1);
    SDL_Gametrak_Update(&session, 510, &sink);
    CHECK(unit.writes == 2 && Written(&unit, 1, start, 2));
    SDL_Gametrak_Update(&session, 511, &sink);
    CHECK(!Handle(&session, gametrak_capture[0], 16, &sink, &report));
    CHECK(unit.writes == 2);

    /* 11: a reconnect unlocks again with the key back at 0x23, and the count restarts */
    memset(&unit, 0, sizeof(unit));
    SDL_Gametrak_Open(&session, 0, &sink);
    CHECK(session.key == 0x23 && session.sensor_reports == 0 && Written(&unit, 0, unlock, 8));
}

static void TestCapture(void)
{
    static const uint8_t keepalives[7][2] = {
        { 0x46, 0x8D }, { 0x46, 0x35 }, { 0x46, 0xDD }, { 0x46, 0x15 }, { 0x46, 0xDB }, { 0x46, 0xD9 }, { 0x46, 0xE3 },
    };
    TestUnit unit;
    SDL_GametrakSink sink = Sink(&unit);
    SDL_GametrakSession session;
    SDL_GametrakReport report;
    int n, sensor = 0, desyncs = 0, i;

    memset(&unit, 0, sizeof(unit));
    SDL_Gametrak_Open(&session, 0, &sink);
    for (n = 1; n <= GAMETRAK_CAPTURE_REPORTS; ++n) {
        unit.current_report = n;
        memset(&report, 0, sizeof(report));
        if (Handle(&session, gametrak_capture[n - 1], 16, &sink, &report)) {
            ++sensor;
            desyncs += session.desync ? 1 : 0;
        }
        switch (n) {
        case 1:
            /* 3: the unlock answer is dropped */
            CHECK(session.sensor_reports == 0);
            break;
        case 2:
            /* 4 */
            CHECK(report.words[0] == 1944 && report.words[1] == 1715 && report.words[2] == 2028);
            CHECK(report.words[3] == 232 && report.words[4] == 249 && report.words[5] == 2028);
            CHECK(report.key_bits == (0x8D & 0x3F) && report.buttons == 0 && report.hat == 0);
            CHECK(report.axes[0] == Scale(1944) && report.axes[5] == Scale(2028));
            break;
        case 3:
            /* 5 */
            CHECK(report.words[0] == 1965 && report.words[1] == 1715 && report.words[2] == 2028);
            CHECK(report.words[3] == 234 && report.words[4] == 251 && report.words[5] == 2028);
            break;
        case 101:
            /* 6 */
            CHECK(report.words[0] == 1125 && report.words[1] == 567 && report.words[2] == 1956);
            CHECK(report.words[3] == 1342 && report.words[4] == 401 && report.words[5] == 2025);
            CHECK(report.key_bits == (0x8D & 0x3F));
            break;
        case 102:
            CHECK(report.words[0] == 1134 && report.words[1] == 567 && report.words[2] == 1952);
            CHECK(report.words[3] == 1342 && report.words[4] == 377 && report.words[5] == 2026);
            CHECK(report.key_bits == (0x35 & 0x3F));
            break;
        case 202:
            /* 7 */
            CHECK(report.words[0] == 1757 && report.words[1] == 1147 && report.words[2] == 1535);
            CHECK(report.words[3] == 1964 && report.words[4] == 336 && report.words[5] == 1721);
            CHECK(report.key_bits == (0xDD & 0x3F));
            break;
        default:
            break;
        }
    }

    /* 12: seven keep-alives, right after reports 101 to 701, and no desync */
    CHECK(sensor == 799 && desyncs == 0);
    CHECK(unit.writes == 2 + 7);
    for (i = 0; i < 7; ++i) {
        CHECK(Written(&unit, 2 + i, keepalives[i], 2));
        CHECK(unit.after_report[2 + i] == 101 + 100 * i);
    }
    /* 2: the first four keep-alives follow sensor reports 100, 200, 300 and 400 */
    CHECK(unit.after_report[2] - 1 == 100 && unit.after_report[5] - 1 == 400);
}

static void TestDecode(void)
{
    SDL_GametrakReport report;
    uint8_t r[16], stale[32];
    size_t length;
    int j, v;
    static const uint8_t hats[10] = { 0, 0x01, 0x03, 0x02, 0x06, 0x04, 0x0C, 0x08, 0x09, 0 };

    /* 8: the foot switch, the hat, and the last button */
    memcpy(r, gametrak_capture[1], 16);
    r[12] = 0x10;
    CHECK(SDL_Gametrak_DecodeReport(r, 16, &report) && report.buttons == 0x0001 && report.hat == 0);
    for (v = 0; v <= 9; ++v) {
        r[12] = (uint8_t)v;
        CHECK(SDL_Gametrak_DecodeReport(r, 16, &report) && report.hat == hats[v] && report.buttons == 0);
    }
    r[12] = 0x00;
    r[13] = 0x80;
    CHECK(SDL_Gametrak_DecodeReport(r, 16, &report) && report.buttons == (1u << 11));
    for (v = 0; v < 11; ++v) {
        r[12] = (uint8_t)(v < 4 ? (0x10 << v) : 0);
        r[13] = (uint8_t)(v >= 4 ? (1 << (v - 4)) : 0);
        CHECK(SDL_Gametrak_DecodeReport(r, 16, &report) && report.buttons == (1u << v));
    }

    /* 9: every prefix changes nothing, a word above 4095 drops the report, and the scale */
    for (length = 0; length < 16; ++length) {
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, gametrak_capture[1], length);
        CHECK(!SDL_Gametrak_DecodeReport(stale, length, &report));
        {
            uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
            if (length) {
                memcpy(copy, gametrak_capture[1], length);
            }
            CHECK(!SDL_Gametrak_DecodeReport(copy, length, &report));
            free(copy);
        }
    }
    memcpy(stale, gametrak_capture[1], 16);
    stale[16] = 0x00;
    CHECK(!SDL_Gametrak_DecodeReport(stale, 17, &report));
    for (j = 0; j < 6; ++j) {
        memset(r, 0, 16);
        r[2 * j] = 0x00;
        r[2 * j + 1] = 0x10; /* 4096 */
        CHECK(!SDL_Gametrak_DecodeReport(r, 16, &report));
        r[2 * j] = 0xFE;
        r[2 * j + 1] = 0x0F; /* 4094 */
        CHECK(SDL_Gametrak_DecodeReport(r, 16, &report));
        for (v = 0; v < 6; ++v) {
            CHECK(report.axes[v] == (v == j ? 32767 : -32768));
        }
    }
    memset(r, 0, 16);
    r[0] = 0xFE;
    r[1] = 0x07;
    r[2] = 0x00;
    r[3] = 0x08;
    CHECK(SDL_Gametrak_DecodeReport(r, 16, &report) && report.axes[0] == -17 && report.axes[1] == 15);
    CHECK(!SDL_Gametrak_DecodeReport(NULL, 16, &report));
    CHECK(!SDL_Gametrak_DecodeReport(r, 16, NULL));
    CHECK(SDL_Gametrak_IsUnlockAnswer(gametrak_capture[0], 16) && !SDL_Gametrak_IsUnlockAnswer(gametrak_capture[0], 15));
    CHECK(!SDL_Gametrak_IsUnlockAnswer(gametrak_capture[1], 16));
}

static void TestDesync(void)
{
    TestUnit unit;
    SDL_GametrakSink sink = Sink(&unit);
    SDL_GametrakSession session;
    SDL_GametrakReport report;
    uint8_t r[16];

    /* 10: key bits that do not match raise the flag and still decode */
    memset(&unit, 0, sizeof(unit));
    SDL_Gametrak_Open(&session, 0, &sink);
    CHECK(!Handle(&session, gametrak_capture[0], 16, &sink, &report));
    memcpy(r, gametrak_capture[1], 16);
    CHECK(Handle(&session, r, 16, &sink, &report) && !session.desync);
    r[0] ^= 0x01;
    CHECK(Handle(&session, r, 16, &sink, &report) && session.desync);
    CHECK(report.words[0] == 1944);
    r[0] ^= 0x01;
    CHECK(Handle(&session, r, 16, &sink, &report) && !session.desync);
}

int main(void)
{
    TestKey();
    TestUnlock();
    TestCapture();
    TestDecode();
    TestDesync();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
