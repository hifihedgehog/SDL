/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_prodikeys_proto.c, the
   Creative Prodikeys PC-MIDI of hifihedgehog/SDL#33 Part 7. Test 6 uses the
   report 4 of the January 2008 debug log. The rest are built from Linux
   hid-prodikeys. */

#include "../src/joystick/hidapi/SDL_hidapi_prodikeys_proto.h"

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

static bool Handle(SDL_ProdikeysState *state, const uint8_t *report, size_t length)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    result = SDL_Prodikeys_HandleReport(state, copy, length);
    free(copy);
    return result;
}

static int CountDown(const SDL_ProdikeysState *state)
{
    int b, count = 0;

    for (b = 0; b < SDL_PRODIKEYS_BUTTONS; ++b) {
        count += SDL_Prodikeys_IsButtonDown(state, b) ? 1 : 0;
    }
    return count;
}

static void TestStart(void)
{
    uint8_t out[3];
    uint8_t ids[32];

    /* 1: the start report */
    SDL_Prodikeys_BuildStart(out);
    CHECK(out[0] == 0x06 && out[1] == 0x01 && out[2] == 0xC1);

    /* The collection that declares input report 3 or 4 carries the keys */
    memset(ids, 0, sizeof(ids));
    CHECK(!SDL_Prodikeys_CarriesKeys(ids));
    ids[0] = 1 << 1;
    CHECK(!SDL_Prodikeys_CarriesKeys(ids));
    ids[0] = 1 << 3;
    CHECK(SDL_Prodikeys_CarriesKeys(ids));
    ids[0] = 1 << 4;
    CHECK(SDL_Prodikeys_CarriesKeys(ids));
    ids[0] = 1 << 6;
    CHECK(!SDL_Prodikeys_CarriesKeys(ids));
    CHECK(!SDL_Prodikeys_CarriesKeys(NULL));
}

static void TestNotes(void)
{
    SDL_ProdikeysState state;

    /* 2 */
    SDL_Prodikeys_Init(&state);
    CHECK(CountDown(&state) == 0 && state.velocity == -32768);
    CHECK(Handle(&state, (const uint8_t *)"\x03\x54\x40", 3));
    CHECK(SDL_Prodikeys_IsButtonDown(&state, 60) && CountDown(&state) == 1 && state.velocity == 0x40 * 257 - 32768);
    CHECK(Handle(&state, (const uint8_t *)"\x03\x94\x00", 3));
    CHECK(CountDown(&state) == 0 && state.velocity == 0x40 * 257 - 32768);

    /* 3: velocity 0 counts as 1 */
    CHECK(Handle(&state, (const uint8_t *)"\x03\x54\x00", 3));
    CHECK(SDL_Prodikeys_IsButtonDown(&state, 60) && state.velocity == 1 * 257 - 32768);
    CHECK(Handle(&state, (const uint8_t *)"\x03\x54\xFF", 3) && state.velocity == 32767);

    /* 4: two pairs, and an odd byte ignored */
    SDL_Prodikeys_Init(&state);
    CHECK(Handle(&state, (const uint8_t *)"\x03\x54\x40\x58\x50", 5));
    CHECK(SDL_Prodikeys_IsButtonDown(&state, 60) && SDL_Prodikeys_IsButtonDown(&state, 64) && CountDown(&state) == 2);
    CHECK(state.velocity == 0x50 * 257 - 32768);
    SDL_Prodikeys_Init(&state);
    CHECK(Handle(&state, (const uint8_t *)"\x03\x54\x40\x58", 4));
    CHECK(SDL_Prodikeys_IsButtonDown(&state, 60) && CountDown(&state) == 1);

    /* 5: no pairs, and a note below 0 */
    SDL_Prodikeys_Init(&state);
    CHECK(Handle(&state, (const uint8_t *)"\x03", 1) && CountDown(&state) == 0);
    CHECK(Handle(&state, (const uint8_t *)"\x03\x10\x40", 3) && CountDown(&state) == 0 && state.velocity == -32768);
    /* Every note byte lands inside 0-127 or nowhere */
    {
        int note;
        uint8_t r[3];
        for (note = 0; note < 256; ++note) {
            SDL_Prodikeys_Init(&state);
            r[0] = 0x03;
            r[1] = (uint8_t)note;
            r[2] = 0x40;
            CHECK(Handle(&state, r, 3));
            if (note < 0x81) {
                const int midi = note - 0x54 + 60;
                CHECK(CountDown(&state) == ((midi >= 0 && midi <= 127) ? 1 : 0));
                if (midi >= 0 && midi <= 127) {
                    CHECK(SDL_Prodikeys_IsButtonDown(&state, midi));
                }
            } else {
                CHECK(CountDown(&state) == 0);
            }
        }
    }
}

static void TestMask(void)
{
    SDL_ProdikeysState state;
    int bit;
    uint8_t r[8];

    /* 6: the captured report 4, then its release, and the Fn lock bit */
    SDL_Prodikeys_Init(&state);
    CHECK(Handle(&state, (const uint8_t *)"\x04\x02\x00\x00", 4));
    CHECK(SDL_Prodikeys_IsButtonDown(&state, 145) && CountDown(&state) == 1);
    CHECK(Handle(&state, (const uint8_t *)"\x04\x00\x00\x00", 4) && CountDown(&state) == 0);
    CHECK(Handle(&state, (const uint8_t *)"\x04\x00\x00\x10", 4));
    CHECK(SDL_Prodikeys_IsButtonDown(&state, 132) && CountDown(&state) == 1);
    CHECK(!Handle(&state, (const uint8_t *)"\x04\x00\x00", 3));
    CHECK(SDL_Prodikeys_IsButtonDown(&state, 132));
    /* Each mask bit alone, in a report padded as Windows pads it */
    for (bit = 0; bit < 24; ++bit) {
        const uint32_t mask = 1u << bit;
        memset(r, 0, sizeof(r));
        r[0] = 0x04;
        r[1] = (uint8_t)(mask >> 16);
        r[2] = (uint8_t)(mask >> 8);
        r[3] = (uint8_t)mask;
        CHECK(Handle(&state, r, 8));
        CHECK(SDL_Prodikeys_IsButtonDown(&state, 128 + bit) && CountDown(&state) == 1);
    }
}

static void TestIgnored(void)
{
    SDL_ProdikeysState state, before;
    uint8_t stale[16];
    size_t length;
    int id;

    /* 7: report 1 and other IDs change nothing */
    SDL_Prodikeys_Init(&state);
    CHECK(Handle(&state, (const uint8_t *)"\x03\x54\x40", 3));
    before = state;
    CHECK(!Handle(&state, (const uint8_t *)"\x01\x00\x40\x00", 4));
    CHECK(memcmp(&state, &before, sizeof(state)) == 0);
    for (id = 0; id < 256; ++id) {
        uint8_t r[4];
        if (id == 3 || id == 4) {
            continue;
        }
        r[0] = (uint8_t)id;
        r[1] = 0xFF;
        r[2] = 0xFF;
        r[3] = 0xFF;
        CHECK(!Handle(&state, r, 4));
    }
    CHECK(memcmp(&state, &before, sizeof(state)) == 0);

    /* 7: every prefix of test 4 acts on its complete pairs only, with 0xFF past the received length */
    for (length = 0; length <= 5; ++length) {
        SDL_Prodikeys_Init(&state);
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, "\x03\x54\x40\x58\x50", length);
        (void)SDL_Prodikeys_HandleReport(&state, stale, length);
        CHECK(CountDown(&state) == (length >= 5 ? 2 : length >= 3 ? 1 : 0));
        SDL_Prodikeys_Init(&state);
        (void)Handle(&state, (const uint8_t *)"\x03\x54\x40\x58\x50", length);
        CHECK(CountDown(&state) == (length >= 5 ? 2 : length >= 3 ? 1 : 0));
    }
    CHECK(!SDL_Prodikeys_HandleReport(NULL, stale, 3));
    CHECK(!SDL_Prodikeys_HandleReport(&state, NULL, 3));

    /* 8: a reconnect releases everything */
    SDL_Prodikeys_Init(&state);
    CHECK(CountDown(&state) == 0 && state.velocity == -32768);
    CHECK(!SDL_Prodikeys_IsButtonDown(&state, -1) && !SDL_Prodikeys_IsButtonDown(&state, 152));
    /* Buttons past the last read nothing, whatever lies after the bits */
    memset(&state, 0xFF, sizeof(state));
    CHECK(SDL_Prodikeys_IsButtonDown(&state, 151));
    CHECK(!SDL_Prodikeys_IsButtonDown(&state, 152) && !SDL_Prodikeys_IsButtonDown(&state, 159) && !SDL_Prodikeys_IsButtonDown(&state, -1));
}

int main(void)
{
    TestStart();
    TestNotes();
    TestMask();
    TestIgnored();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
