/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_nimbus_proto.c, the
   SteelSeries Nimbus of hifihedgehog/SDL#33 Part 7, built from
   MFIGamepadFeeder's mapping with 0x01 standing in for the unrecorded report
   ID. Stick values follow the fork's two's complement reading. */

#include "../src/joystick/hidapi/SDL_hidapi_nimbus_proto.h"

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

static bool Decode(const uint8_t *report, size_t length, bool has_id, SDL_NimbusState *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    memset(out, 0xA5, sizeof(*out));
    result = SDL_Nimbus_DecodeReport(copy, length, has_id, out);
    free(copy);
    return result;
}

static const uint8_t test1[18] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x7F, 0x80, 0x00, 0x00
};

static void TestControls(bool has_id)
{
    const int skip = has_id ? 0 : 1; /* Without a report ID the payload starts at byte 1 of test1 */
    const size_t length = has_id ? 18 : 17;
    SDL_NimbusState s;
    uint8_t r[18];
    static const struct
    {
        int byte;
        uint8_t hat;
        uint8_t button;
    } controls[] = {
        { 1, 0x01, 0 }, { 2, 0x02, 0 }, { 3, 0x04, 0 }, { 4, 0x08, 0 },
        { 5, 0, 0x01 }, { 6, 0, 0x02 }, { 7, 0, 0x04 }, { 8, 0, 0x08 }, { 9, 0, 0x10 }, { 10, 0, 0x20 },
        { 13, 0, 0x40 },
    };
    size_t i;

    /* 1 */
    CHECK(Decode(test1 + skip, length, has_id, &s));
    CHECK(s.buttons == 0x01 && s.hat == 0);
    CHECK(s.axes[4] == 128 && s.axes[5] == -32768);
    CHECK(s.axes[0] == 32767 && s.axes[1] == 32767 && s.axes[2] == 0 && s.axes[3] == 0);

    /* 2: each control byte alone at 0x01, then released */
    for (i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        memset(r, 0, sizeof(r));
        r[0] = 0x01;
        r[controls[i].byte] = 0x01;
        CHECK(Decode(r + skip, length, has_id, &s));
        CHECK(s.hat == controls[i].hat && s.buttons == controls[i].button);
        r[controls[i].byte] = 0x00;
        CHECK(Decode(r + skip, length, has_id, &s) && s.hat == 0 && s.buttons == 0);
    }
    /* Opposite D-pad directions cancel, adjacent ones combine */
    memset(r, 0, sizeof(r));
    r[0] = 0x01;
    r[1] = 0x40;
    r[3] = 0x40;
    CHECK(Decode(r + skip, length, has_id, &s) && s.hat == 0);
    r[3] = 0x00;
    r[2] = 0x40;
    CHECK(Decode(r + skip, length, has_id, &s) && s.hat == (0x01 | 0x02));
}

static void TestSticks(void)
{
    SDL_NimbusState s;
    uint8_t r[18];

    /* 3 */
    CHECK(SDL_Nimbus_StickAxis(0x00) == 0 && SDL_Nimbus_StickAxis(0x7F) == 32767);
    CHECK(SDL_Nimbus_StickAxis(0x80) == -32768 && SDL_Nimbus_StickAxis(0xFF) == -256);
    memset(r, 0, sizeof(r));
    r[0] = 0x01;
    r[14] = 0x80;
    r[15] = 0x80;
    r[16] = 0xFF;
    r[17] = 0x7F;
    CHECK(Decode(r, 18, true, &s));
    CHECK(s.axes[0] == -32768 && s.axes[1] == 32767 && s.axes[2] == -256 && s.axes[3] == -32767);
}

static void TestFraming(void)
{
    SDL_NimbusState s;
    uint8_t stale[32];
    size_t length;

    /* 4: every prefix changes nothing, with 0xFF past the received length, and 19 or more bytes are ignored */
    for (length = 0; length < 18; ++length) {
        CHECK(!Decode(test1, length, true, &s));
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, test1, length);
        CHECK(!SDL_Nimbus_DecodeReport(stale, length, true, &s));
    }
    memcpy(stale, test1, 18);
    CHECK(!Decode(stale, 19, true, &s) && !Decode(stale, 20, true, &s));
    /* Without a report ID: 17 bytes, and nothing else */
    for (length = 0; length < 17; ++length) {
        CHECK(!Decode(test1 + 1, length, false, &s));
    }
    CHECK(Decode(test1 + 1, 17, false, &s) && s.buttons == 0x01);
    CHECK(!Decode(test1, 18, false, &s));
    CHECK(!SDL_Nimbus_DecodeReport(NULL, 18, true, &s));
    CHECK(!SDL_Nimbus_DecodeReport(test1, 18, true, NULL));
}

int main(void)
{
    TestControls(true);
    TestControls(false);
    TestSticks();
    TestFraming();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
