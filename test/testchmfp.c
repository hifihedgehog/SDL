/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_chmfp_proto.c, the CH
 * Products Multi-Function Panel of hifihedgehog/SDL#33 Part 14. Test numbers
 * follow the part. The reports come from the byte table in chmfp 0.6.0's
 * README (lines 168-199), which its author built by listening to the USB
 * traffic, plus constructed cases. No device, no SDL runtime. Every report
 * is also fed from an exact-size heap copy, so AddressSanitizer catches a
 * read past the length the module was given. */

#include "../src/joystick/hidapi/SDL_hidapi_chmfp_proto.h"
#include "../src/hidapi/SDL_hidapi_vendorusb.h"
#include "../src/joystick/usb_ids.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        ++checks;                                                                     \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);               \
        }                                                                             \
    } while (0)

/* Applies a report from an exact-size heap copy */
static int Handle(SDL_CHMFPState *state, const uint8_t *report, size_t length)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    int result;

    if (!copy) {
        ++failures;
        printf("FAIL: out of memory\n");
        return -1;
    }
    if (length) {
        memcpy(copy, report, length);
    }
    result = SDL_CHMFP_HandleReport(state, copy, length);
    free(copy);
    return result;
}

static int CountPressed(const SDL_CHMFPState *state)
{
    int button, count = 0;

    for (button = 0; button < SDL_CHMFP_BUTTONS; ++button) {
        count += SDL_CHMFP_IsPressed(state, button) ? 1 : 0;
    }
    return count;
}

/* Exactly one button pressed, and it is this one */
static bool OnlyPressed(const SDL_CHMFPState *state, int button)
{
    return SDL_CHMFP_IsPressed(state, button) && CountPressed(state) == 1;
}

static bool Same(const SDL_CHMFPState *a, const SDL_CHMFPState *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* Feeds one report to a fresh state and requires it to be accepted */
static SDL_CHMFPState Fresh(const uint8_t report[8])
{
    SDL_CHMFPState state;

    SDL_CHMFP_ResetState(&state);
    CHECK(Handle(&state, report, 8) == SDL_CHMFP_REPORT_ACCEPTED);
    return state;
}

/* 1 */
static void TestSingleKeys(void)
{
    static const uint8_t key1[8] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t key25[8] = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t key49[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00 };
    static const uint8_t key50[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00 };
    SDL_CHMFPState state;

    state = Fresh(key1);
    CHECK(OnlyPressed(&state, 0));
    state = Fresh(key25);
    CHECK(OnlyPressed(&state, 24));
    state = Fresh(key49);
    CHECK(OnlyPressed(&state, 48));
    state = Fresh(key50);
    CHECK(OnlyPressed(&state, 49));
}

/* 2 */
static void TestPanelButtons(void)
{
    static const uint8_t green[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    static const uint8_t red[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 };
    static const uint8_t both[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 };
    static const uint8_t red_in_red_mode[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06 };
    SDL_CHMFPState state;

    state = Fresh(green);
    CHECK(OnlyPressed(&state, SDL_CHMFP_BUTTON_GREEN));
    state = Fresh(red);
    CHECK(OnlyPressed(&state, SDL_CHMFP_BUTTON_RED));
    state = Fresh(both);
    CHECK(SDL_CHMFP_IsPressed(&state, SDL_CHMFP_BUTTON_GREEN) && SDL_CHMFP_IsPressed(&state, SDL_CHMFP_BUTTON_RED));
    CHECK(CountPressed(&state) == 2);
    /* The buttons do not move with the mode */
    state = Fresh(red_in_red_mode);
    CHECK(OnlyPressed(&state, SDL_CHMFP_BUTTON_RED));
}

/* 3 */
static void TestIdle(void)
{
    static const uint8_t idle_green[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t idle_red[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 };
    SDL_CHMFPState state;

    state = Fresh(idle_green);
    CHECK(CountPressed(&state) == 0);
    state = Fresh(idle_red);
    CHECK(CountPressed(&state) == 0);
}

/* 4 */
static void TestModeChange(void)
{
    static const uint8_t key1_red[8] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 };
    static const uint8_t key1_green[8] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t keys12_green[8] = { 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t keys12_red[8] = { 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 };
    SDL_CHMFPState state, decoded;

    SDL_CHMFP_ResetState(&state);
    CHECK(Handle(&state, key1_red, 8) == SDL_CHMFP_REPORT_ACCEPTED);
    CHECK(OnlyPressed(&state, 50));
    CHECK(Handle(&state, key1_green, 8) == SDL_CHMFP_REPORT_ACCEPTED);
    CHECK(OnlyPressed(&state, 0) && !SDL_CHMFP_IsPressed(&state, 50));

    /* Two held keys: the change releases two buttons and presses two */
    CHECK(Handle(&state, keys12_green, 8) == SDL_CHMFP_REPORT_ACCEPTED && CountPressed(&state) == 2);
    CHECK(SDL_CHMFP_Decode(keys12_red, 8, &decoded) && SDL_CHMFP_CountChanges(&state, &decoded) == 4);
    CHECK(Handle(&state, keys12_red, 8) == SDL_CHMFP_REPORT_ACCEPTED);
    CHECK(SDL_CHMFP_IsPressed(&state, 50) && SDL_CHMFP_IsPressed(&state, 51) && CountPressed(&state) == 2);
}

/* 5 */
static void TestDropped(void)
{
    static const uint8_t reserved[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08 };
    static const uint8_t seven[8] = { 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t six[8] = { 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    SDL_CHMFPState state, idle;
    int k;

    SDL_CHMFP_ResetState(&idle);
    state = idle;
    CHECK(Handle(&state, reserved, 8) == SDL_CHMFP_REPORT_BAD_BITS && Same(&state, &idle));
    CHECK(Handle(&state, seven, 8) == SDL_CHMFP_REPORT_CHORD && Same(&state, &idle));
    CHECK(Handle(&state, six, 8) == SDL_CHMFP_REPORT_ACCEPTED);
    CHECK(CountPressed(&state) == 6);
    for (k = 0; k < 6; ++k) {
        CHECK(SDL_CHMFP_IsPressed(&state, k));
    }
    /* A reserved bit drops a report whatever else it carries */
    {
        uint8_t report[8] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09 };
        SDL_CHMFPState before = state;

        CHECK(Handle(&state, report, 8) == SDL_CHMFP_REPORT_BAD_BITS && Same(&state, &before));
    }
}

/* 6: the rows of the README table, key, byte and value */
static void TestEveryKey(void)
{
    static const uint8_t table[50][3] = {
        { 1, 0, 0x01 }, { 2, 0, 0x02 }, { 3, 0, 0x04 }, { 4, 0, 0x08 }, { 5, 0, 0x10 },
        { 6, 0, 0x20 }, { 7, 0, 0x40 }, { 8, 0, 0x80 }, { 9, 1, 0x01 }, { 10, 1, 0x02 },
        { 11, 1, 0x04 }, { 12, 1, 0x08 }, { 13, 1, 0x10 }, { 14, 1, 0x20 }, { 15, 1, 0x40 },
        { 16, 1, 0x80 }, { 17, 2, 0x01 }, { 18, 2, 0x02 }, { 19, 2, 0x04 }, { 20, 2, 0x08 },
        { 21, 2, 0x10 }, { 22, 2, 0x20 }, { 23, 2, 0x40 }, { 24, 2, 0x80 }, { 25, 3, 0x01 },
        { 26, 3, 0x02 }, { 27, 3, 0x04 }, { 28, 3, 0x08 }, { 29, 3, 0x10 }, { 30, 3, 0x20 },
        { 31, 3, 0x40 }, { 32, 3, 0x80 }, { 33, 4, 0x01 }, { 34, 4, 0x02 }, { 35, 4, 0x04 },
        { 36, 4, 0x08 }, { 37, 4, 0x10 }, { 38, 4, 0x20 }, { 39, 4, 0x40 }, { 40, 4, 0x80 },
        { 41, 5, 0x01 }, { 42, 5, 0x02 }, { 43, 5, 0x04 }, { 44, 5, 0x08 }, { 45, 5, 0x10 },
        { 46, 5, 0x20 }, { 47, 5, 0x40 }, { 48, 5, 0x80 }, { 49, 6, 0x01 }, { 50, 6, 0x02 },
    };
    int row, seen = 0;

    for (row = 0; row < 50; ++row) {
        const int key = table[row][0];
        uint8_t report[8];
        SDL_CHMFPState state;

        /* The table and the part's rule agree: key k is bit (k - 1) % 8 of byte (k - 1) / 8 */
        CHECK(table[row][1] == (key - 1) / 8 && table[row][2] == (1u << ((key - 1) % 8)));

        memset(report, 0, sizeof(report));
        report[table[row][1]] = table[row][2];
        state = Fresh(report);
        CHECK(OnlyPressed(&state, key - 1));
        seen += SDL_CHMFP_IsPressed(&state, key - 1) ? 1 : 0;

        /* The same key in red mode is the second bank */
        report[7] = 0x04;
        state = Fresh(report);
        CHECK(OnlyPressed(&state, 50 + key - 1));
    }
    CHECK(seen == 50);

    /* All 50 keys at once. Decode ignores the chord limit, which would drop
     * this report after an idle one. */
    {
        static const uint8_t all_green[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0x00 };
        SDL_CHMFPState direct;
        int button;

        CHECK(SDL_CHMFP_Decode(all_green, 8, &direct));
        CHECK(CountPressed(&direct) == SDL_CHMFP_KEYS);
        for (button = 0; button < SDL_CHMFP_KEYS; ++button) {
            CHECK(SDL_CHMFP_IsPressed(&direct, button));
        }
    }
}

/* Bits 2 to 7 of byte 6 belong to no key. chmfp reads 50 key bits and does
 * not check them. */
static void TestByte6(void)
{
    int bit;

    for (bit = 2; bit < 8; ++bit) {
        uint8_t report[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        SDL_CHMFPState state;

        report[6] = (uint8_t)(1u << bit);
        state = Fresh(report);
        CHECK(CountPressed(&state) == 0);
        report[6] = (uint8_t)(0x01 | (1u << bit));
        state = Fresh(report);
        CHECK(OnlyPressed(&state, 48));
    }
}

/* Every value of byte 7 against an independent reading of the part's table */
static void TestByte7(void)
{
    int value, dropped = 0;

    for (value = 0; value < 256; ++value) {
        uint8_t report[8] = { 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00 };
        SDL_CHMFPState state, idle;
        int result;

        report[7] = (uint8_t)value;
        SDL_CHMFP_ResetState(&state);
        idle = state;
        result = Handle(&state, report, 8);
        if (value & 0xF8) {
            CHECK(result == SDL_CHMFP_REPORT_BAD_BITS && Same(&state, &idle));
            ++dropped;
        } else {
            const int key21 = (value & 0x04) ? SDL_CHMFP_KEYS + 20 : 20;

            CHECK(result == SDL_CHMFP_REPORT_ACCEPTED);
            CHECK(SDL_CHMFP_IsPressed(&state, key21));
            CHECK(SDL_CHMFP_IsPressed(&state, SDL_CHMFP_BUTTON_GREEN) == ((value & 0x01) != 0));
            CHECK(SDL_CHMFP_IsPressed(&state, SDL_CHMFP_BUTTON_RED) == ((value & 0x02) != 0));
            CHECK(CountPressed(&state) == 1 + ((value & 0x01) ? 1 : 0) + ((value & 0x02) ? 1 : 0));
        }
    }
    CHECK(dropped == 248);
}

/* The chord limit counts what chmfp counts: every one of the 102 buttons
 * whose state differs from the last accepted report, presses and releases
 * alike, both banks and both panel buttons. */
static void TestChordCounting(void)
{
    static const uint8_t idle[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t six[8] = { 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t six_green[8] = { 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    static const uint8_t five_green[8] = { 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    static const uint8_t three[8] = { 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t three_red[8] = { 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 };
    static const uint8_t four[8] = { 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t four_red[8] = { 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 };
    static const uint8_t idle_red[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 };
    SDL_CHMFPState state, before, decoded;

    /* Six presses then six releases: accepted */
    SDL_CHMFP_ResetState(&state);
    CHECK(Handle(&state, six, 8) == SDL_CHMFP_REPORT_ACCEPTED);
    CHECK(Handle(&state, idle, 8) == SDL_CHMFP_REPORT_ACCEPTED && CountPressed(&state) == 0);

    /* The green button counts: five keys and green is six, six keys and green is seven */
    CHECK(Handle(&state, five_green, 8) == SDL_CHMFP_REPORT_ACCEPTED && CountPressed(&state) == 6);
    SDL_CHMFP_ResetState(&state);
    CHECK(Handle(&state, six_green, 8) == SDL_CHMFP_REPORT_CHORD && CountPressed(&state) == 0);

    /* Seven releases at once are dropped too, and the buttons stay held
     * until a report differs from the accepted state by six or fewer */
    CHECK(Handle(&state, six, 8) == SDL_CHMFP_REPORT_ACCEPTED);
    CHECK(Handle(&state, six_green, 8) == SDL_CHMFP_REPORT_ACCEPTED && CountPressed(&state) == 7);
    before = state;
    CHECK(Handle(&state, idle, 8) == SDL_CHMFP_REPORT_CHORD && Same(&state, &before));
    CHECK(Handle(&state, six, 8) == SDL_CHMFP_REPORT_ACCEPTED && CountPressed(&state) == 6);

    /* A mode change counts each held key twice */
    SDL_CHMFP_ResetState(&state);
    CHECK(Handle(&state, three, 8) == SDL_CHMFP_REPORT_ACCEPTED);
    CHECK(SDL_CHMFP_Decode(three_red, 8, &decoded) && SDL_CHMFP_CountChanges(&state, &decoded) == 6);
    CHECK(Handle(&state, three_red, 8) == SDL_CHMFP_REPORT_ACCEPTED);
    CHECK(SDL_CHMFP_IsPressed(&state, 50) && SDL_CHMFP_IsPressed(&state, 51) && SDL_CHMFP_IsPressed(&state, 52));
    CHECK(CountPressed(&state) == 3);
    SDL_CHMFP_ResetState(&state);
    CHECK(Handle(&state, four, 8) == SDL_CHMFP_REPORT_ACCEPTED);
    before = state;
    CHECK(SDL_CHMFP_Decode(four_red, 8, &decoded) && SDL_CHMFP_CountChanges(&state, &decoded) == 8);
    CHECK(Handle(&state, four_red, 8) == SDL_CHMFP_REPORT_CHORD && Same(&state, &before));

    /* The mode bit alone moves nothing when no key is held */
    SDL_CHMFP_ResetState(&state);
    CHECK(Handle(&state, idle_red, 8) == SDL_CHMFP_REPORT_ACCEPTED && CountPressed(&state) == 0);

    /* A dropped report is not the new base: the next report is counted
     * against the last accepted one */
    {
        static const uint8_t seven_a[8] = { 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        static const uint8_t one[8] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

        SDL_CHMFP_ResetState(&state);
        CHECK(Handle(&state, seven_a, 8) == SDL_CHMFP_REPORT_CHORD);
        CHECK(Handle(&state, one, 8) == SDL_CHMFP_REPORT_ACCEPTED && OnlyPressed(&state, 0));
    }

    /* Presses and releases in one report add up */
    {
        static const uint8_t keys123[8] = { 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        static const uint8_t keys4567[8] = { 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        static const uint8_t keys456[8] = { 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

        SDL_CHMFP_ResetState(&state);
        CHECK(Handle(&state, keys123, 8) == SDL_CHMFP_REPORT_ACCEPTED);
        CHECK(Handle(&state, keys4567, 8) == SDL_CHMFP_REPORT_CHORD);
        CHECK(Handle(&state, keys456, 8) == SDL_CHMFP_REPORT_ACCEPTED && CountPressed(&state) == 3);
    }

    /* Every count from 0 to 102 */
    {
        SDL_CHMFPState a, b;
        int n;

        SDL_CHMFP_ResetState(&a);
        CHECK(SDL_CHMFP_CountChanges(&a, &a) == 0);
        for (n = 0; n <= SDL_CHMFP_BUTTONS; ++n) {
            int i;

            SDL_CHMFP_ResetState(&b);
            for (i = 0; i < n; ++i) {
                b.buttons[i / 8] = (uint8_t)(b.buttons[i / 8] | (1u << (i % 8)));
            }
            CHECK(SDL_CHMFP_CountChanges(&a, &b) == n && SDL_CHMFP_CountChanges(&b, &a) == n);
        }
        CHECK(SDL_CHMFP_CountChanges(NULL, &a) == 0 && SDL_CHMFP_CountChanges(&a, NULL) == 0);
    }
}

/* 6: every truncation, and every longer read, changes nothing, with stale
 * bytes past the length that would form a valid report pressing other
 * buttons */
static void TestLengths(void)
{
    static const uint8_t report[8] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    static const uint8_t other[8] = { 0x10, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x06 };
    SDL_CHMFPState state, before, decoded;
    uint8_t stale[64];
    size_t n;

    SDL_CHMFP_ResetState(&state);
    CHECK(Handle(&state, (const uint8_t *)"\x02\x00\x00\x00\x00\x00\x00\x00", 8) == SDL_CHMFP_REPORT_ACCEPTED);
    before = state;
    /* The stale report alone would be accepted from this state */
    {
        SDL_CHMFPState probe = state;

        CHECK(SDL_CHMFP_HandleReport(&probe, other, 8) == SDL_CHMFP_REPORT_ACCEPTED && !Same(&probe, &before));
    }
    for (n = 0; n < 8; ++n) {
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, other, 8);
        memcpy(stale, report, n);
        CHECK(SDL_CHMFP_HandleReport(&state, stale, n) == SDL_CHMFP_REPORT_BAD_LENGTH);
        CHECK(Same(&state, &before));
        CHECK(Handle(&state, report, n) == SDL_CHMFP_REPORT_BAD_LENGTH);
        CHECK(Same(&state, &before));
        memset(&decoded, 0x5A, sizeof(decoded));
        CHECK(!SDL_CHMFP_Decode(stale, n, &decoded));
        CHECK(decoded.buttons[0] == 0x5A);
    }
    /* A transfer longer than 8 bytes cannot come from the 8-byte endpoint.
     * chmfp reads into an 8-byte buffer, so none reaches its decoder. */
    for (n = 9; n <= sizeof(stale); ++n) {
        memset(stale, 0x00, sizeof(stale));
        memcpy(stale, report, 8);
        CHECK(Handle(&state, stale, n) == SDL_CHMFP_REPORT_BAD_LENGTH && Same(&state, &before));
        memset(&decoded, 0x5A, sizeof(decoded));
        CHECK(!SDL_CHMFP_Decode(stale, n, &decoded) && decoded.buttons[0] == 0x5A);
    }
    /* The whole report still works */
    CHECK(Handle(&state, report, 8) == SDL_CHMFP_REPORT_ACCEPTED);
    CHECK(SDL_CHMFP_IsPressed(&state, 0) && SDL_CHMFP_IsPressed(&state, SDL_CHMFP_BUTTON_GREEN) && CountPressed(&state) == 2);
}

/* A report split across two reads is two short transfers: neither acts, and
 * the next whole report decodes */
static void TestSplits(void)
{
    static const uint8_t report[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x06 };
    size_t cut;

    for (cut = 1; cut < 8; ++cut) {
        SDL_CHMFPState state, before;

        SDL_CHMFP_ResetState(&state);
        before = state;
        CHECK(Handle(&state, report, cut) == SDL_CHMFP_REPORT_BAD_LENGTH);
        CHECK(Handle(&state, report + cut, 8 - cut) == SDL_CHMFP_REPORT_BAD_LENGTH);
        CHECK(Same(&state, &before));
        CHECK(Handle(&state, report, 8) == SDL_CHMFP_REPORT_ACCEPTED);
        CHECK(SDL_CHMFP_IsPressed(&state, SDL_CHMFP_KEYS + 47) && SDL_CHMFP_IsPressed(&state, SDL_CHMFP_BUTTON_RED));
        CHECK(CountPressed(&state) == 2);
    }
}

/* 7: a new connection starts with nothing pressed, and its first report is
 * counted against that. SDL releases every button of a removed joystick
 * (SDL_PrivateJoystickRemoved), so the driver keeps no state across a
 * reconnect. */
static void TestReconnect(void)
{
    static const uint8_t held[8] = { 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t seven_held[8] = { 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    SDL_CHMFPState state;

    SDL_CHMFP_ResetState(&state);
    CHECK(Handle(&state, held, 8) == SDL_CHMFP_REPORT_ACCEPTED && CountPressed(&state) == 6);
    SDL_CHMFP_ResetState(&state);
    CHECK(CountPressed(&state) == 0);
    CHECK(Handle(&state, seven_held, 8) == SDL_CHMFP_REPORT_CHORD && CountPressed(&state) == 0);
    CHECK(Handle(&state, held, 8) == SDL_CHMFP_REPORT_ACCEPTED && CountPressed(&state) == 6);
}

static void TestArguments(void)
{
    static const uint8_t report[8] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    SDL_CHMFPState state, before;

    memset(&state, 0xFF, sizeof(state));
    SDL_CHMFP_ResetState(&state);
    CHECK(CountPressed(&state) == 0);
    SDL_CHMFP_ResetState(NULL);
    before = state;
    CHECK(SDL_CHMFP_HandleReport(&state, NULL, 8) == SDL_CHMFP_REPORT_BAD_LENGTH && Same(&state, &before));
    CHECK(SDL_CHMFP_HandleReport(NULL, report, 8) == SDL_CHMFP_REPORT_BAD_LENGTH);
    CHECK(!SDL_CHMFP_Decode(NULL, 8, &state) && Same(&state, &before));
    CHECK(!SDL_CHMFP_Decode(report, 8, NULL));
    CHECK(!SDL_CHMFP_IsPressed(NULL, 0));
    CHECK(Handle(&state, report, 8) == SDL_CHMFP_REPORT_ACCEPTED);
    CHECK(SDL_CHMFP_IsPressed(&state, 0) && !SDL_CHMFP_IsPressed(&state, -1) && !SDL_CHMFP_IsPressed(&state, SDL_CHMFP_BUTTONS));
    /* Out of range is never pressed, even over bits a state never sets */
    {
        SDL_CHMFPState all;

        memset(&all, 0xFF, sizeof(all));
        CHECK(SDL_CHMFP_IsPressed(&all, 0) && SDL_CHMFP_IsPressed(&all, 101));
        CHECK(!SDL_CHMFP_IsPressed(&all, 102) && !SDL_CHMFP_IsPressed(&all, 103) && !SDL_CHMFP_IsPressed(&all, -1));
        CHECK(!SDL_CHMFP_IsPressed(&all, -8) && !SDL_CHMFP_IsPressed(&all, 1000));
    }
    CHECK(SDL_CHMFP_BUTTONS == 102 && SDL_CHMFP_BUTTON_GREEN == 100 && SDL_CHMFP_BUTTON_RED == 101);
    CHECK(SDL_CHMFP_REPORT_LENGTH == 8 && SDL_CHMFP_KEYS == 50 && SDL_CHMFP_MAX_CHANGES == 6);
}

/* The panel's interface as chmfp's README (lines 111-131) prints it, laid
 * out the way hid.c's select_vendor_endpoints lays it out: the interface
 * descriptor, the HID class descriptor the interface carries in its extra
 * bytes, then the endpoint. */
static const uint8_t mfp_interface0[] = {
    9, 4, 0, 0, 1, 0xFF, 0, 0, 4,       /* class FF, one endpoint */
    9, 0x21, 0x00, 0x01, 0x00, 0x01, 0x22, 0x15, 0x00, /* 21-byte report descriptor, unused under class FF */
    7, 5, 0x81, 3, 8, 0, 1,             /* interrupt IN 0x81, 8 bytes, bInterval 1 */
};

static bool Select(const SDL_VendorUSBRule *rule, const uint8_t *bytes, size_t length, SDL_VendorUSBSelection *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool ok;

    if (!copy) {
        ++failures;
        return false;
    }
    if (length) {
        memcpy(copy, bytes, length);
    }
    ok = SDL_VendorUSB_SelectEndpoints(rule, 0, copy, length, out);
    free(copy);
    return ok;
}

static SDL_VendorUSBRouting Route(SDL_VendorUSBPlatform platform, bool libusb)
{
    SDL_VendorUSBRouting r;

    r.platform = platform;
    r.libusb = libusb;
    r.whitelist = true;
    r.gamecube = true;
    r.vendor = USB_VENDOR_CH_PRODUCTS;
    r.product = USB_PRODUCT_CH_PRODUCTS_MFP;
    r.xbox = false;
    /* As should_enumerate_interface in hid.c computes it for interface 0 */
    r.vendor_interface = libusb ? SDL_VendorUSB_RuleApplies(SDL_VendorUSB_FindRule(r.vendor, r.product, 0, 0xFF, 0, 0), platform) : false;
    return r;
}

static void TestVendorRule(void)
{
    const SDL_VendorUSBPlatform platforms[] = {
        SDL_VENDORUSB_PLATFORM_WINDOWS, SDL_VENDORUSB_PLATFORM_OTHER, SDL_VENDORUSB_PLATFORM_MACOS
    };
    const SDL_VendorUSBRule *rule = SDL_VendorUSB_FindRule(USB_VENDOR_CH_PRODUCTS, USB_PRODUCT_CH_PRODUCTS_MFP, 0, 0xFF, 0, 0);
    SDL_VendorUSBSelection selection;
    size_t n, p;

    CHECK(USB_VENDOR_CH_PRODUCTS == 0x068E && USB_PRODUCT_CH_PRODUCTS_MFP == 0x00F0);
    CHECK(rule != NULL);
    CHECK(SDL_VendorUSB_IsVendorDevice(USB_VENDOR_CH_PRODUCTS, USB_PRODUCT_CH_PRODUCTS_MFP));
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_CH_PRODUCTS, USB_PRODUCT_CH_PRODUCTS_MFP, 1, 0xFF, 0, 0) == NULL);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_CH_PRODUCTS, 0x00F1, 0, 0xFF, 0, 0) == NULL);
    if (!rule) {
        return;
    }
    CHECK(!(rule->flags & SDL_VENDORUSB_RAW_OUTPUT) && (rule->flags & SDL_VENDORUSB_WINDOWS_ONLY));

    /* Only the whole interface selects: IN 0x81 of 8 bytes and no OUT */
    for (n = 0; n <= sizeof(mfp_interface0); ++n) {
        bool ok;

        memset(&selection, 0, sizeof(selection));
        ok = Select(rule, mfp_interface0, n, &selection);
        CHECK(ok == (n == sizeof(mfp_interface0)));
        if (ok) {
            CHECK(selection.alternate == 0 && selection.in.address == 0x81);
            CHECK(selection.in.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT);
            CHECK(selection.in.max_packet_size == 8 && selection.in.read_size == 8);
            CHECK(selection.out.address == 0);
        }
    }
    /* Another packet size or address is not the panel the rule describes */
    {
        uint8_t other[sizeof(mfp_interface0)];

        memcpy(other, mfp_interface0, sizeof(other));
        other[22] = 16;
        CHECK(!Select(rule, other, sizeof(other), &selection));
        memcpy(other, mfp_interface0, sizeof(other));
        other[20] = 0x82;
        CHECK(!Select(rule, other, sizeof(other), &selection));
    }

    /* On Windows libusb enumerates the panel, whatever the whitelist, and
     * the platform HID backend leaves it. Like every rule of Part 14 the
     * rule serves Windows only, so elsewhere nothing changes. */
    for (p = 0; p < sizeof(platforms) / sizeof(platforms[0]); ++p) {
        const bool windows = (platforms[p] == SDL_VENDORUSB_PLATFORM_WINDOWS);
        SDL_VendorUSBRouting r = Route(platforms[p], true);

        CHECK(SDL_VendorUSB_IsCandidate(platforms[p], USB_VENDOR_CH_PRODUCTS, USB_PRODUCT_CH_PRODUCTS_MFP, 0, 0xFF, 0, 0, false) == windows);
        CHECK(r.vendor_interface == windows && SDL_VendorUSB_Ignore(&r) == !windows);
        r.whitelist = false;
        CHECK(!SDL_VendorUSB_Ignore(&r));
        r = Route(platforms[p], false);
        CHECK(SDL_VendorUSB_Ignore(&r) == windows);
        CHECK(!SDL_VendorUSB_RequiresLibUSB(platforms[p], USB_VENDOR_CH_PRODUCTS, USB_PRODUCT_CH_PRODUCTS_MFP, false, false));
    }
}

int main(void)
{
    TestSingleKeys();
    TestPanelButtons();
    TestIdle();
    TestModeChange();
    TestDropped();
    TestEveryKey();
    TestByte6();
    TestByte7();
    TestChordCounting();
    TestLengths();
    TestSplits();
    TestReconnect();
    TestArguments();
    TestVendorRule();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
