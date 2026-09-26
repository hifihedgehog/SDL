/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/windows/SDL_icade_proto.c, the iCade letter
   protocol of hifihedgehog/SDL#33 Part 16. Test numbers follow the part's
   replay tests. Tests 5, 7 and 8 need device handles and run in
   test/testicade.c against the driver. The expected make codes are built
   here from iCade-iOS's letter strings and Microsoft's scan code table, not
   copied from the module. */

#include "../src/joystick/windows/SDL_icade_proto.h"

#include <stdint.h>
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

#define BREAK SDL_ICADE_KEY_BREAK
#define E0    SDL_ICADE_KEY_E0
#define E1    SDL_ICADE_KEY_E1

/* The Scan 1 make code of each letter, a to z, from Microsoft's table
   ("Keyboard Input Overview", HID usages 0x04 to 0x1D) */
static const uint16_t letter_scan[26] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
};

/* iCade-iOS iCadeReaderView.m:25-26 */
static const char on_states[] = "wdxayhujikol";
static const char off_states[] = "eczqtrfnmpgv";

static uint16_t Press(int control)
{
    return letter_scan[on_states[control] - 'a'];
}

static uint16_t Release(int control)
{
    return letter_scan[off_states[control] - 'a'];
}

/* Applies one record to a state copied into an exact-size heap object, so
   a write past it reaches the sanitizer */
static bool Apply(uint16_t *state, uint16_t make_code, uint16_t flags)
{
    uint16_t *copy = (uint16_t *)malloc(sizeof(*copy));
    bool changed;

    *copy = *state;
    changed = SDL_ICade_ApplyKey(copy, make_code, flags);
    *state = *copy;
    free(copy);
    return changed;
}

/* The part's table, row by row: control, press letter, release letter */
static void TestTable(void)
{
    static const struct
    {
        uint16_t bit;
        char press;
        char release;
        uint16_t press_scan;
        uint16_t release_scan;
    } rows[SDL_ICADE_CONTROLS] = {
        { SDL_ICADE_UP, 'w', 'e', 0x11, 0x12 },
        { SDL_ICADE_RIGHT, 'd', 'c', 0x20, 0x2E },
        { SDL_ICADE_DOWN, 'x', 'z', 0x2D, 0x2C },
        { SDL_ICADE_LEFT, 'a', 'q', 0x1E, 0x10 },
        { SDL_ICADE_BUTTON_A, 'y', 't', 0x15, 0x14 },
        { SDL_ICADE_BUTTON_B, 'h', 'r', 0x23, 0x13 },
        { SDL_ICADE_BUTTON_C, 'u', 'f', 0x16, 0x21 },
        { SDL_ICADE_BUTTON_D, 'j', 'n', 0x24, 0x31 },
        { SDL_ICADE_BUTTON_E, 'i', 'm', 0x17, 0x32 },
        { SDL_ICADE_BUTTON_F, 'k', 'p', 0x25, 0x19 },
        { SDL_ICADE_BUTTON_G, 'o', 'g', 0x18, 0x22 },
        { SDL_ICADE_BUTTON_H, 'l', 'v', 0x26, 0x2F },
    };
    int i;

    for (i = 0; i < SDL_ICADE_CONTROLS; ++i) {
        CHECK(rows[i].bit == (uint16_t)(1u << i));
        CHECK(on_states[i] == rows[i].press);
        CHECK(off_states[i] == rows[i].release);
        CHECK(Press(i) == rows[i].press_scan);
        CHECK(Release(i) == rows[i].release_scan);
    }
    CHECK(SDL_ICADE_VENDOR == 0x15E4);
    CHECK(SDL_ICADE_PRODUCT == 0x0132);
    CHECK(SDL_ICADE_BUTTONS == 8);
    CHECK(SDL_ICADE_KEY_BREAK == 1 && SDL_ICADE_KEY_E0 == 2 && SDL_ICADE_KEY_E1 == 4);
    CHECK(SDL_ICADE_OVERRUN == 0xFF);
    CHECK(SDL_ICADE_HAT_UP == 0x01 && SDL_ICADE_HAT_RIGHT == 0x02 && SDL_ICADE_HAT_DOWN == 0x04 && SDL_ICADE_HAT_LEFT == 0x08);
}

/* 1: press make then its key-up holds the control, release make then its
   key-up releases it, and nothing else changes, from every other state of
   the rest */
static void Test1(void)
{
    static const uint16_t others[] = { 0x0000, 0x0FFF, 0x0AAA, 0x0555 };
    int i, j;

    for (i = 0; i < SDL_ICADE_CONTROLS; ++i) {
        const uint16_t bit = (uint16_t)(1u << i);

        for (j = 0; j < (int)(sizeof(others) / sizeof(others[0])); ++j) {
            const uint16_t rest = (uint16_t)(others[j] & ~bit);
            uint16_t state = rest;

            CHECK(Apply(&state, Press(i), 0));
            CHECK(state == (rest | bit));
            CHECK(!Apply(&state, Press(i), BREAK));
            CHECK(state == (rest | bit));
            CHECK(Apply(&state, Release(i), 0));
            CHECK(state == rest);
            CHECK(!Apply(&state, Release(i), BREAK));
            CHECK(state == rest);
        }
    }
}

/* 2: up and right held give the hat up-right, releasing up leaves right */
static void Test2(void)
{
    uint16_t state = 0;

    CHECK(Apply(&state, 0x11, 0));
    CHECK(!Apply(&state, 0x11, BREAK));
    CHECK(Apply(&state, 0x20, 0));
    CHECK(!Apply(&state, 0x20, BREAK));
    CHECK(state == (SDL_ICADE_UP | SDL_ICADE_RIGHT));
    CHECK(SDL_ICade_GetHat(state) == (SDL_ICADE_HAT_UP | SDL_ICADE_HAT_RIGHT));
    CHECK(Apply(&state, 0x12, 0));
    CHECK(!Apply(&state, 0x12, BREAK));
    CHECK(state == SDL_ICADE_RIGHT);
    CHECK(SDL_ICade_GetHat(state) == SDL_ICADE_HAT_RIGHT);
}

/* 3: a press letter alone holds, a repeat keeps it, the release letter
   releases and a repeat of it changes nothing */
static void Test3(void)
{
    uint16_t state = 0;

    CHECK(Apply(&state, 0x15, 0));
    CHECK(state == SDL_ICADE_BUTTON_A && SDL_ICade_GetButton(state, 0));
    CHECK(!Apply(&state, 0x15, 0));
    CHECK(state == SDL_ICADE_BUTTON_A);
    CHECK(Apply(&state, 0x14, 0));
    CHECK(state == 0 && !SDL_ICade_GetButton(state, 0));
    CHECK(!Apply(&state, 0x14, 0));
    CHECK(state == 0);
}

/* 4: B, S, the digit row, an E0 letter and the overrun code change nothing */
static void Test4(void)
{
    static const uint16_t starts[] = { 0x0000, 0x0FFF, 0x0123 };
    int i, j;

    for (i = 0; i < 3; ++i) {
        uint16_t state = starts[i];

        CHECK(!Apply(&state, 0x30, 0) && state == starts[i]);
        CHECK(!Apply(&state, 0x1F, 0) && state == starts[i]);
        for (j = 0x02; j <= 0x0B; ++j) {
            CHECK(!Apply(&state, (uint16_t)j, 0) && state == starts[i]);
        }
        CHECK(!Apply(&state, 0x11, E0) && state == starts[i]);
        CHECK(!Apply(&state, 0x12, E0) && state == starts[i]);
        CHECK(!Apply(&state, 0xFF, 0) && state == starts[i]);
    }
    /* The digit row really is 02 to 0B */
    CHECK(letter_scan['b' - 'a'] == 0x30 && letter_scan['s' - 'a'] == 0x1F);
}

/* 6: the make code decides, not the layout, so Q's code 10, which a French
   AZERTY layout types as A, still releases stick left, and A's code 1E,
   typed as Q there, presses it */
static void Test6(void)
{
    uint16_t state = SDL_ICADE_LEFT | SDL_ICADE_BUTTON_C;

    CHECK(Apply(&state, 0x10, 0));
    CHECK(state == SDL_ICADE_BUTTON_C);
    CHECK(Apply(&state, 0x1E, 0));
    CHECK(state == (SDL_ICADE_LEFT | SDL_ICADE_BUTTON_C));
}

/* Every make code, and every flag word on a press and a release letter */
static void TestExhaustive(void)
{
    int code, flags, i, bad_codes = 0, bad_flags = 0;

    for (code = 0; code <= 0xFFFF; ++code) {
        int control = -1;
        bool press = false;
        uint16_t zero = 0, full = 0x0FFF;
        bool changed_zero, changed_full;

        for (i = 0; i < SDL_ICADE_CONTROLS; ++i) {
            if (code == Press(i)) {
                control = i;
                press = true;
            } else if (code == Release(i)) {
                control = i;
            }
        }
        changed_zero = SDL_ICade_ApplyKey(&zero, (uint16_t)code, 0);
        changed_full = SDL_ICade_ApplyKey(&full, (uint16_t)code, 0);
        if (control < 0) {
            if (changed_zero || changed_full || zero != 0 || full != 0x0FFF) {
                ++bad_codes;
            }
        } else if (press) {
            if (!changed_zero || changed_full || zero != (uint16_t)(1u << control) || full != 0x0FFF) {
                ++bad_codes;
            }
        } else {
            if (changed_zero || !changed_full || zero != 0 || full != (uint16_t)(0x0FFF & ~(1u << control))) {
                ++bad_codes;
            }
        }
    }
    CHECK(bad_codes == 0);

    for (flags = 0; flags <= 0xFFFF; ++flags) {
        const bool acts = (flags & (BREAK | E0 | E1)) == 0;

        for (i = 0; i < SDL_ICADE_CONTROLS; ++i) {
            uint16_t state = 0;
            bool changed = SDL_ICade_ApplyKey(&state, Press(i), (uint16_t)flags);

            if (changed != acts || state != (acts ? (uint16_t)(1u << i) : 0)) {
                ++bad_flags;
            }
            state = 0x0FFF;
            changed = SDL_ICade_ApplyKey(&state, Release(i), (uint16_t)flags);
            if (changed != acts || state != (acts ? (uint16_t)(0x0FFF & ~(1u << i)) : 0x0FFF)) {
                ++bad_flags;
            }
        }
    }
    CHECK(bad_flags == 0);
    CHECK(!SDL_ICade_ApplyKey(NULL, 0x11, 0));
}

/* Records split across any number of reads decode as one stream, since the
   decoder keeps only the state */
static void TestStream(void)
{
    static const uint16_t codes[] = { 0x11, 0x20, 0x15, 0x30, 0x12, 0x23, 0x14, 0xFF, 0x2D, 0x13, 0x2C, 0x2E };
    static const uint16_t flags[] = { 0, 0, 0, 0, BREAK, 0, 0, 0, 0, 0, E0, 0 };
    const int count = (int)(sizeof(codes) / sizeof(codes[0]));
    uint16_t expected = 0;
    int split, i;

    for (i = 0; i < count; ++i) {
        (void)SDL_ICade_ApplyKey(&expected, codes[i], flags[i]);
    }
    /* W, D, Y, then B and E's key-up change nothing, H, T releases A, FF,
       X, R releases B, Z with E0 changes nothing, C releases right */
    CHECK(expected == (SDL_ICADE_UP | SDL_ICADE_DOWN));
    CHECK(SDL_ICade_GetHat(expected) == SDL_ICADE_HAT_CENTERED);
    for (split = 0; split <= count; ++split) {
        uint16_t first = 0, second;

        for (i = 0; i < split; ++i) {
            (void)Apply(&first, codes[i], flags[i]);
        }
        second = first;
        for (; i < count; ++i) {
            (void)Apply(&second, codes[i], flags[i]);
        }
        CHECK(second == expected);
    }
}

/* The hat for every combination of the four stick bits, with buttons held */
static void TestHat(void)
{
    static const uint8_t expected[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x00, 0x06, 0x02,
        0x08, 0x09, 0x00, 0x01, 0x0C, 0x08, 0x04, 0x00
    };
    int stick;

    for (stick = 0; stick < 16; ++stick) {
        CHECK(SDL_ICade_GetHat((uint16_t)stick) == expected[stick]);
        CHECK(SDL_ICade_GetHat((uint16_t)(stick | 0x0FF0)) == expected[stick]);
    }
}

static void TestButtons(void)
{
    int button, other;

    for (button = 0; button < SDL_ICADE_BUTTONS; ++button) {
        const uint16_t state = (uint16_t)(SDL_ICADE_BUTTON_A << button);

        for (other = 0; other < SDL_ICADE_BUTTONS; ++other) {
            CHECK(SDL_ICade_GetButton(state, other) == (other == button));
            CHECK(SDL_ICade_GetButton((uint16_t)(0x0FFF & ~state), other) == (other != button));
        }
    }
    CHECK(!SDL_ICade_GetButton(0x000F, 0));
    CHECK(!SDL_ICade_GetButton(0xFFFF, -1));
    CHECK(!SDL_ICade_GetButton(0xFFFF, 8));
    CHECK(!SDL_ICade_GetButton(0xFFFF, 12));
    CHECK(!SDL_ICade_GetButton(0xFFFF, -2147483647 - 1));
}

/* The cabinet: bottom row B, D, F, H on south, east, right trigger, left
   trigger, top row A, C, E, G on west, north, right shoulder, left
   shoulder. 9: a pad takes iCade-iOS's table, A back, B left shoulder, C
   start, D right shoulder, E north, F south, G east, H west. */
static void TestMapping(void)
{
    enum { A, B, C, D, E, F, G, H };
    static const int cabinet[SDL_ICADE_SLOT_COUNT] = { B, D, A, C, -1, -1, G, E, H, F };
    static const int pad[SDL_ICADE_SLOT_COUNT] = { F, G, H, E, A, C, B, D, -1, -1 };
    int slot;

    CHECK(SDL_ICADE_SLOT_SOUTH == 0 && SDL_ICADE_SLOT_EAST == 1 && SDL_ICADE_SLOT_WEST == 2 && SDL_ICADE_SLOT_NORTH == 3);
    CHECK(SDL_ICADE_SLOT_BACK == 4 && SDL_ICADE_SLOT_START == 5);
    CHECK(SDL_ICADE_SLOT_LEFT_SHOULDER == 6 && SDL_ICADE_SLOT_RIGHT_SHOULDER == 7);
    CHECK(SDL_ICADE_SLOT_LEFT_TRIGGER == 8 && SDL_ICADE_SLOT_RIGHT_TRIGGER == 9 && SDL_ICADE_SLOT_COUNT == 10);
    for (slot = 0; slot < SDL_ICADE_SLOT_COUNT; ++slot) {
        CHECK(SDL_ICade_GetSlotButton(SDL_ICADE_LAYOUT_CABINET, (SDL_ICadeSlot)slot) == cabinet[slot]);
        CHECK(SDL_ICade_GetSlotButton(SDL_ICADE_LAYOUT_PAD, (SDL_ICadeSlot)slot) == pad[slot]);
        CHECK(SDL_ICade_GetSlotButton(SDL_ICADE_LAYOUT_NONE, (SDL_ICadeSlot)slot) == -1);
        CHECK(SDL_ICade_GetSlotButton((SDL_ICadeLayout)3, (SDL_ICadeSlot)slot) == -1);
    }
    CHECK(SDL_ICade_GetSlotButton(SDL_ICADE_LAYOUT_CABINET, (SDL_ICadeSlot)-1) == -1);
    CHECK(SDL_ICade_GetSlotButton(SDL_ICADE_LAYOUT_PAD, SDL_ICADE_SLOT_COUNT) == -1);
    CHECK(SDL_ICade_GetSlotButton(SDL_ICADE_LAYOUT_CABINET, (SDL_ICadeSlot)100) == -1);

    /* 9 at the decoder: A's press presses back, F's press presses south */
    {
        uint16_t state = 0;

        CHECK(Apply(&state, 0x15, 0));
        CHECK(SDL_ICade_GetButton(state, SDL_ICade_GetSlotButton(SDL_ICADE_LAYOUT_PAD, SDL_ICADE_SLOT_BACK)));
        CHECK(!SDL_ICade_GetButton(state, SDL_ICade_GetSlotButton(SDL_ICADE_LAYOUT_PAD, SDL_ICADE_SLOT_SOUTH)));
        CHECK(Apply(&state, 0x25, 0));
        CHECK(SDL_ICade_GetButton(state, SDL_ICade_GetSlotButton(SDL_ICADE_LAYOUT_PAD, SDL_ICADE_SLOT_SOUTH)));
    }
}

/* The parser's log */

static int log_calls;
static char log_entry[256];
static char log_reason[128];

static void Log(void *userdata, const char *entry, size_t length, const char *reason)
{
    int *calls = (int *)userdata;

    ++*calls;
    ++log_calls;
    if (length >= sizeof(log_entry)) {
        length = sizeof(log_entry) - 1;
    }
    memcpy(log_entry, entry, length);
    log_entry[length] = '\0';
    snprintf(log_reason, sizeof(log_reason), "%s", reason);
}

/* Parses from an exact-size heap copy without a terminator, so a read past
   the length reaches the sanitizer */
static int Parse(const char *text, size_t length, SDL_ICadeDeviceList *list, int *rejects)
{
    char *copy = (char *)malloc(length ? length : 1);
    int count;

    if (length) {
        memcpy(copy, text, length);
    }
    *rejects = 0;
    count = SDL_ICade_ParseDevices(copy, length, list, Log, rejects);
    free(copy);
    return count;
}

static int ParseText(const char *text, SDL_ICadeDeviceList *list, int *rejects)
{
    return Parse(text, strlen(text), list, rejects);
}

static bool Pairs(const SDL_ICadeDeviceList *list, const uint32_t *pairs, int count)
{
    int i;

    if (list->count != count) {
        return false;
    }
    for (i = 0; i < count; ++i) {
        if (list->pairs[i] != pairs[i]) {
            return false;
        }
    }
    return true;
}

static void TestParseValid(void)
{
    SDL_ICadeDeviceList list;
    int rejects;

    CHECK(SDL_ICade_ParseDevices(NULL, 10, &list, Log, &rejects) == 0 && list.count == 0);
    CHECK(SDL_ICade_ParseDevices("0x1/0x2", 7, NULL, NULL, NULL) == 0);
    CHECK(ParseText("", &list, &rejects) == 0 && rejects == 0);
    CHECK(ParseText("0x2dc8/0x9021", &list, &rejects) == 1 && rejects == 0);
    CHECK(list.pairs[0] == 0x2DC89021u);
    {
        static const uint32_t expected[] = { 0x2DC89021u, 0x10381412u };

        CHECK(ParseText(" 0x2DC8 / 0X9021 , 0x1038/0x1412 ", &list, &rejects) == 2 && rejects == 0);
        CHECK(Pairs(&list, expected, 2));
        CHECK(ParseText("\t0x2dc8\t/\t0x9021\t,0x1038/0x1412", &list, &rejects) == 2 && rejects == 0);
        CHECK(Pairs(&list, expected, 2));
    }
    {
        static const uint32_t expected[] = { 0x00010002u, 0x00030004u };

        CHECK(ParseText(",0x1/0x2,,0x3/0x4,", &list, &rejects) == 2 && rejects == 0);
        CHECK(Pairs(&list, expected, 2));
        CHECK(ParseText(" , ,\t, 0x1/0x2 , , 0x3/0x4 , ", &list, &rejects) == 2 && rejects == 0);
        CHECK(Pairs(&list, expected, 2));
    }
    CHECK(ParseText("0XABCD/0xabcd", &list, &rejects) == 1 && list.pairs[0] == 0xABCDABCDu);
    CHECK(ParseText("0xAbCd/0XaBcD", &list, &rejects) == 1 && list.pairs[0] == 0xABCDABCDu);
    CHECK(ParseText("0x0001/0x0000", &list, &rejects) == 1 && list.pairs[0] == 0x00010000u && rejects == 0);
    CHECK(ParseText("0xFFFF/0xFFFF", &list, &rejects) == 1 && list.pairs[0] == 0xFFFFFFFFu);
    /* A repeated pair is kept once, however it is written */
    CHECK(ParseText("0x1/0x2,0x1/0x2, 0X0001/0x0002", &list, &rejects) == 1 && rejects == 0);
    CHECK(list.pairs[0] == 0x00010002u);
}

static void TestParseMalformed(void)
{
    static const struct
    {
        const char *text;
        const char *reason;
    } cases[] = {
        { "0x1-0x2", "no '/' between vendor and product" },
        { "0x1", "no '/' between vendor and product" },
        { "1234/0x5678", "vendor is not 0x and one to four hex digits" },
        { "0x/0x1", "vendor is not 0x and one to four hex digits" },
        { "0x12345/0x1", "vendor is not 0x and one to four hex digits" },
        { "0xg1/0x1", "vendor is not 0x and one to four hex digits" },
        { "x1/0x2", "vendor is not 0x and one to four hex digits" },
        { "00x1/0x2", "vendor is not 0x and one to four hex digits" },
        { "1x12/0x3", "vendor is not 0x and one to four hex digits" },
        { "0x12/1X34", "product is not 0x and one to four hex digits" },
        { "0x1 2/0x3", "vendor is not 0x and one to four hex digits" },
        { "/0x3", "vendor is not 0x and one to four hex digits" },
        { "0x1/5678", "product is not 0x and one to four hex digits" },
        { "0x1/0x", "product is not 0x and one to four hex digits" },
        { "0x1/", "product is not 0x and one to four hex digits" },
        { "0x1/0x2/0x3", "product is not 0x and one to four hex digits" },
        { "0x1/0x2 junk", "product is not 0x and one to four hex digits" },
        { "0x1/0x2;0x3/0x4", "product is not 0x and one to four hex digits" },
        { "0x1/0x2\n", "product is not 0x and one to four hex digits" },
        { "0x1/0x10000", "product is not 0x and one to four hex digits" },
        { "0x1/-0x2", "product is not 0x and one to four hex digits" },
        { "0x0/0x1", "vendor 0x0000 names no device" },
        { "0x0000/0x0000", "vendor 0x0000 names no device" },
    };
    SDL_ICadeDeviceList list;
    int rejects;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        log_calls = 0;
        CHECK(ParseText(cases[i].text, &list, &rejects) == 0);
        CHECK(rejects == 1 && log_calls == 1);
        CHECK(strcmp(log_reason, cases[i].reason) == 0);
        if (strcmp(log_reason, cases[i].reason) != 0) {
            printf("  case \"%s\": reason \"%s\"\n", cases[i].text, log_reason);
        }
    }

    /* The log gets the entry without the spaces around it */
    CHECK(ParseText("0x1/0x2,  bad entry\t, 0x3/0x4", &list, &rejects) == 2 && rejects == 1);
    CHECK(strcmp(log_entry, "bad entry") == 0);
    {
        static const uint32_t expected[] = { 0x00010002u, 0x00030004u };

        CHECK(Pairs(&list, expected, 2));
    }
    /* A NULL log is allowed */
    CHECK(SDL_ICade_ParseDevices("bad,0x1/0x2", 11, &list, NULL, NULL) == 1 && list.pairs[0] == 0x00010002u);
}

static void TestParseLimit(void)
{
    char text[64 * 16];
    SDL_ICadeDeviceList list;
    size_t used = 0;
    int rejects, i;

    for (i = 0; i < SDL_ICADE_MAX_PAIRS + 8; ++i) {
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s0x%x/0x%x", i ? "," : "", 0x100 + i, 0x200 + i);
    }
    log_calls = 0;
    CHECK(ParseText(text, &list, &rejects) == SDL_ICADE_MAX_PAIRS);
    CHECK(rejects == 8 && strcmp(log_reason, "too many pairs") == 0);
    for (i = 0; i < SDL_ICADE_MAX_PAIRS; ++i) {
        CHECK(list.pairs[i] == (((uint32_t)(0x100 + i) << 16) | (uint32_t)(0x200 + i)));
    }
    /* A repeat of a kept pair past the limit is still no error */
    used = strlen(text);
    snprintf(text + used, sizeof(text) - used, ",0x100/0x200");
    CHECK(ParseText(text, &list, &rejects) == SDL_ICADE_MAX_PAIRS && rejects == 8);
}

/* Every truncation of a hint, alone and with stale bytes after it, gives
   what the same prefix gives as a string of its own */
static void TestParseTruncations(void)
{
    static const char *const hints[] = {
        " 0x2dc8/0x9021 , 0x1038/0x1412,bad,0X15E4 / 0x0132 ,, 0x0/0x1 ",
        "0x1/0x2,0x1/0x2,0x3/0x4",
    };
    static const char stale[] = ",0x7777/0x8888,0x9999/0xAAAA";
    size_t h, length;

    for (h = 0; h < sizeof(hints) / sizeof(hints[0]); ++h) {
        const size_t full = strlen(hints[h]);

        for (length = 0; length <= full; ++length) {
            char prefix[128];
            char *buffer = (char *)malloc(length + sizeof(stale));
            SDL_ICadeDeviceList reference, alone, with_stale;
            int rejects_reference, rejects_alone, rejects_stale;

            memcpy(prefix, hints[h], length);
            prefix[length] = '\0';
            rejects_reference = 0;
            (void)SDL_ICade_ParseDevices(prefix, sizeof(prefix), &reference, Log, &rejects_reference);
            (void)Parse(hints[h], length, &alone, &rejects_alone);
            memcpy(buffer, hints[h], length);
            memcpy(buffer + length, stale, sizeof(stale));
            rejects_stale = 0;
            (void)SDL_ICade_ParseDevices(buffer, length, &with_stale, Log, &rejects_stale);
            free(buffer);

            CHECK(memcmp(&reference, &alone, sizeof(reference)) == 0 && rejects_reference == rejects_alone);
            CHECK(memcmp(&reference, &with_stale, sizeof(reference)) == 0 && rejects_reference == rejects_stale);
        }
    }
    /* A 0 byte ends the text before the length does */
    {
        static const char text[] = "0x1/0x2\0,0x3/0x4";
        SDL_ICadeDeviceList list;
        int rejects = 0;

        CHECK(SDL_ICade_ParseDevices(text, sizeof(text), &list, Log, &rejects) == 1 && rejects == 0);
        CHECK(list.pairs[0] == 0x00010002u);
    }
}

static void TestClassify(void)
{
    SDL_ICadeDeviceList list;
    int rejects;

    CHECK(SDL_ICade_Classify(0x15E4, 0x0132, NULL) == SDL_ICADE_LAYOUT_CABINET);
    CHECK(SDL_ICade_Classify(0x0132, 0x15E4, NULL) == SDL_ICADE_LAYOUT_NONE);
    CHECK(SDL_ICade_Classify(0x15E4, 0x0133, NULL) == SDL_ICADE_LAYOUT_NONE);
    CHECK(SDL_ICade_Classify(0x15E5, 0x0132, NULL) == SDL_ICADE_LAYOUT_NONE);
    CHECK(SDL_ICade_Classify(0x0000, 0x0000, NULL) == SDL_ICADE_LAYOUT_NONE);
    CHECK(ParseText("0x2dc8/0x9021, 0x15e4/0x0132", &list, &rejects) == 2);
    CHECK(SDL_ICade_Classify(0x2DC8, 0x9021, &list) == SDL_ICADE_LAYOUT_PAD);
    CHECK(SDL_ICade_Classify(0x15E4, 0x0132, &list) == SDL_ICADE_LAYOUT_CABINET);
    CHECK(SDL_ICade_Classify(0x9021, 0x2DC8, &list) == SDL_ICADE_LAYOUT_NONE);
    CHECK(SDL_ICade_Classify(0x2DC8, 0x9022, &list) == SDL_ICADE_LAYOUT_NONE);
    CHECK(SDL_ICade_Classify(0x2DC9, 0x9021, &list) == SDL_ICADE_LAYOUT_NONE);
    /* A count past the array is read no further than the array */
    list.count = SDL_ICADE_MAX_PAIRS + 1000;
    CHECK(SDL_ICade_Classify(0x1111, 0x2222, &list) == SDL_ICADE_LAYOUT_NONE);
    list.count = 0;
    CHECK(SDL_ICade_Classify(0x2DC8, 0x9021, &list) == SDL_ICADE_LAYOUT_NONE);
}

static void TestQueue(void)
{
    SDL_ICadeQueue *queue = (SDL_ICadeQueue *)calloc(1, sizeof(*queue));
    SDL_ICadeEvent event;
    uint64_t next;
    int i;

    CHECK(!SDL_ICade_PopQueue(queue, &event));
    SDL_ICade_PushQueue(queue, 0x0011, 1000);
    SDL_ICade_PushQueue(queue, 0x0013, 2000);
    CHECK(queue->count == 2 && queue->sequence == 2);
    CHECK(SDL_ICade_PopQueue(queue, &event) && event.sequence == 1 && event.state == 0x0011 && event.time_ns == 1000);
    CHECK(SDL_ICade_PopQueue(queue, &event) && event.sequence == 2 && event.state == 0x0013 && event.time_ns == 2000);
    CHECK(!SDL_ICade_PopQueue(queue, &event));

    /* Full: the oldest goes */
    for (i = 0; i < SDL_ICADE_QUEUE_CAPACITY + 5; ++i) {
        SDL_ICade_PushQueue(queue, (uint16_t)i, (uint64_t)i * 10);
    }
    CHECK(queue->count == SDL_ICADE_QUEUE_CAPACITY && queue->dropped == 5);
    /* The two pushes above took sequences 1 and 2, these took 3 onward, and
       the five oldest of them went */
    next = 3 + 5;
    for (i = 0; i < SDL_ICADE_QUEUE_CAPACITY; ++i) {
        CHECK(SDL_ICade_PopQueue(queue, &event) && event.sequence == next && event.state == (uint16_t)(i + 5) &&
              event.time_ns == (uint64_t)(i + 5) * 10);
        ++next;
    }
    CHECK(!SDL_ICade_PopQueue(queue, &event));

    /* Across the wrap, in order */
    for (i = 0; i < 100; ++i) {
        SDL_ICade_PushQueue(queue, (uint16_t)i, 0);
    }
    for (i = 0; i < 90; ++i) {
        CHECK(SDL_ICade_PopQueue(queue, &event) && event.state == (uint16_t)i);
    }
    for (i = 100; i < 200; ++i) {
        SDL_ICade_PushQueue(queue, (uint16_t)i, 0);
    }
    CHECK(queue->count == 110 && queue->dropped == 5);
    for (i = 90; i < 200; ++i) {
        CHECK(SDL_ICade_PopQueue(queue, &event) && event.state == (uint16_t)i);
    }

    /* Clearing empties it and keeps the sequence */
    SDL_ICade_PushQueue(queue, 1, 0);
    next = queue->sequence;
    SDL_ICade_ClearQueue(queue);
    CHECK(queue->count == 0 && queue->dropped == 0 && queue->sequence == next);
    CHECK(!SDL_ICade_PopQueue(queue, &event));
    SDL_ICade_PushQueue(queue, 2, 0);
    CHECK(SDL_ICade_PopQueue(queue, &event) && event.sequence == next + 1 && event.state == 2);

    SDL_ICade_PushQueue(NULL, 1, 0);
    SDL_ICade_ClearQueue(NULL);
    CHECK(!SDL_ICade_PopQueue(NULL, &event));
    CHECK(!SDL_ICade_PopQueue(queue, NULL));
    free(queue);
}

int main(void)
{
    TestTable();
    Test1();
    Test2();
    Test3();
    Test4();
    Test6();
    TestExhaustive();
    TestStream();
    TestHat();
    TestButtons();
    TestMapping();
    TestParseValid();
    TestParseMalformed();
    TestParseLimit();
    TestParseTruncations();
    TestClassify();
    TestQueue();

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
