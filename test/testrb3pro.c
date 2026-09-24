/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/SDL_rb3pro_proto.c, the Rock
   Band 3 Pro instruments of hifihedgehog/SDL#33 Part 3. "PS3 n" and "X360 n"
   follow the part's two lists of replay tests. Every report is fed as an
   exact-size heap copy, so AddressSanitizer catches a read past its length,
   and truncated inside a buffer of stale bytes. */

#include "../src/joystick/SDL_rb3pro_proto.h"

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

#define MS(x) ((uint64_t)(x) * 1000)

/* ------------------------------------------------------------------------ */
/* Helpers */

/* RPCS3's emulated adapter at rest, keyboard mode, after it writes the empty
   key mask over bytes 5 to 7 */
static const uint8_t keyboard_rest[27] = {
    0x00, 0x00, 0x08, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02
};

/* RPCS3's emulated adapter at rest, guitar mode */
static const uint8_t guitar_rest[27] = {
    0x00, 0x00, 0x08, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x40, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static bool Decode(int variant, const uint8_t *report, size_t length, SDL_RB3ProOutput *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    memset(out, 0xA5, sizeof(*out));
    result = SDL_RB3Pro_DecodeReport(variant, copy, length, out);
    free(copy);
    return result;
}

static bool Untouched(const SDL_RB3ProOutput *out)
{
    const uint8_t *bytes = (const uint8_t *)out;
    size_t i;

    for (i = 0; i < sizeof(*out); ++i) {
        if (bytes[i] != 0xA5) {
            return false;
        }
    }
    return true;
}

static bool Down(const SDL_RB3ProOutput *o, int button)
{
    return ((o->buttons >> button) & 1) != 0;
}

static int CountDown(const SDL_RB3ProOutput *o)
{
    int count = 0, i;
    for (i = 0; i < 64; ++i) {
        count += Down(o, i) ? 1 : 0;
    }
    return count;
}

static int16_t Scale7(int value)
{
    if (value > 127) {
        value = 127;
    }
    return (int16_t)((value * 32767) / 127);
}

static int16_t Scale5(int value)
{
    return (int16_t)((value * 32767) / 31);
}

/* Field by field: padding is not compared */
static bool Same(const SDL_RB3ProOutput *a, const SDL_RB3ProOutput *b)
{
    int i;

    if (a->buttons != b->buttons || a->button_mask != b->button_mask ||
        a->axis_mask != b->axis_mask || a->hat != b->hat) {
        return false;
    }
    for (i = 0; i < SDL_RB3PRO_MAX_AXES; ++i) {
        if (((a->axis_mask >> i) & 1) && a->axes[i] != b->axes[i]) {
            return false;
        }
    }
    return true;
}

static uint64_t AllButtons(int count)
{
    return (count >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << count) - 1);
}

static const uint8_t clockwise[8] = {
    SDL_RB3PRO_HAT_UP,
    SDL_RB3PRO_HAT_UP | SDL_RB3PRO_HAT_RIGHT,
    SDL_RB3PRO_HAT_RIGHT,
    SDL_RB3PRO_HAT_DOWN | SDL_RB3PRO_HAT_RIGHT,
    SDL_RB3PRO_HAT_DOWN,
    SDL_RB3PRO_HAT_DOWN | SDL_RB3PRO_HAT_LEFT,
    SDL_RB3PRO_HAT_LEFT,
    SDL_RB3PRO_HAT_UP | SDL_RB3PRO_HAT_LEFT
};

/* The XInput state a PS3 report corresponds to: the gamepad part through
   wButtons and PS3 bytes 5 to 20 in the fields and trailing bytes */
static SDL_RB3ProXInputState XInputFromReport(const uint8_t *report)
{
    SDL_RB3ProXInputState s;
    memset(&s, 0, sizeof(s));
    if (report[0] & 0x01) {
        s.buttons |= 0x4000; /* square, X */
    }
    if (report[0] & 0x02) {
        s.buttons |= 0x1000; /* cross, A */
    }
    if (report[0] & 0x04) {
        s.buttons |= 0x2000; /* circle, B */
    }
    if (report[0] & 0x08) {
        s.buttons |= 0x8000; /* triangle, Y */
    }
    if (report[1] & 0x01) {
        s.buttons |= 0x0020; /* select, back */
    }
    if (report[1] & 0x02) {
        s.buttons |= 0x0010; /* start */
    }
    if (report[1] & 0x10) {
        s.buttons |= 0x0400; /* PS, guide */
    }
    if (report[2] < 8) {
        const uint8_t hat = clockwise[report[2]];
        if (hat & SDL_RB3PRO_HAT_UP) {
            s.buttons |= 0x0001;
        }
        if (hat & SDL_RB3PRO_HAT_DOWN) {
            s.buttons |= 0x0002;
        }
        if (hat & SDL_RB3PRO_HAT_LEFT) {
            s.buttons |= 0x0004;
        }
        if (hat & SDL_RB3PRO_HAT_RIGHT) {
            s.buttons |= 0x0008;
        }
    }
    s.left_trigger = report[5];
    s.right_trigger = report[6];
    s.thumb_lx = (int16_t)(uint16_t)(report[7] | (report[8] << 8));
    s.thumb_ly = (int16_t)(uint16_t)(report[9] | (report[10] << 8));
    s.thumb_rx = (int16_t)(uint16_t)(report[11] | (report[12] << 8));
    s.thumb_ry = (int16_t)(uint16_t)(report[13] | (report[14] << 8));
    s.has_trailing = true;
    memcpy(s.trailing, &report[15], 6);
    return s;
}

static bool DecodeX(int variant, const SDL_RB3ProXInputState *s, SDL_RB3ProOutput *out)
{
    SDL_RB3ProXInputState *copy = (SDL_RB3ProXInputState *)malloc(sizeof(*copy));
    bool result;

    memcpy(copy, s, sizeof(*copy));
    memset(out, 0xA5, sizeof(*out));
    result = SDL_RB3Pro_DecodeXInput(variant, copy, out);
    free(copy);
    return result;
}

/* Every PS3 vector also decodes identically through XInput (X360 3) */
static int equivalence_vectors;

static void CheckEquivalent(int variant, const uint8_t *report)
{
    SDL_RB3ProOutput a, b;
    SDL_RB3ProXInputState s = XInputFromReport(report);

    CHECK(Decode(variant, report, 27, &a));
    CHECK(DecodeX(variant, &s, &b));
    CHECK(Same(&a, &b));
    ++equivalence_vectors;
}

/* ------------------------------------------------------------------------ */
/* Identity and layout */

static void TestIdentity(void)
{
    static const struct
    {
        uint16_t vendor, product;
        int variant;
        bool ps3;
        const char *name;
    } expected[] = {
        { 0x12BA, 0x2330, SDL_RB3PRO_KEYBOARD, true, "Rock Band 3 Keyboard" },
        { 0x12BA, 0x2338, SDL_RB3PRO_KEYBOARD, true, "Rock Band 3 MIDI Pro Adapter (Keyboard)" },
        { 0x12BA, 0x2430, SDL_RB3PRO_GUITAR, true, "Rock Band 3 Pro Guitar" },
        { 0x12BA, 0x2438, SDL_RB3PRO_GUITAR, true, "Rock Band 3 MIDI Pro Adapter (Guitar)" },
        { 0x12BA, 0x2530, SDL_RB3PRO_GUITAR, true, "Rock Band 3 Pro Guitar" },
        { 0x12BA, 0x2538, SDL_RB3PRO_GUITAR, true, "Rock Band 3 MIDI Pro Adapter (Guitar)" },
        { 0x1BAD, 0x3330, SDL_RB3PRO_KEYBOARD, false, "Rock Band 3 Keyboard" },
        { 0x1BAD, 0x3338, SDL_RB3PRO_KEYBOARD, false, "Rock Band 3 MIDI Pro Adapter (Keyboard)" },
        { 0x1BAD, 0x3430, SDL_RB3PRO_GUITAR, false, "Rock Band 3 Pro Guitar" },
        { 0x1BAD, 0x3438, SDL_RB3PRO_GUITAR, false, "Rock Band 3 MIDI Pro Adapter (Guitar)" },
        { 0x1BAD, 0x3530, SDL_RB3PRO_GUITAR, false, "Rock Band 3 Pro Guitar" },
        { 0x1BAD, 0x3538, SDL_RB3PRO_GUITAR, false, "Rock Band 3 MIDI Pro Adapter (Guitar)" },
    };
    SDL_RB3ProDevice device;
    int nbuttons, naxes, nhats;
    size_t i;

    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        memset(&device, 0, sizeof(device));
        CHECK(SDL_RB3Pro_GetDevice(expected[i].vendor, expected[i].product, &device));
        CHECK(device.variant == expected[i].variant);
        CHECK(device.ps3 == expected[i].ps3);
        CHECK(device.name && strcmp(device.name, expected[i].name) == 0);
    }
    CHECK(SDL_RB3Pro_GetDevice(0x12BA, 0x2330, NULL));
    /* The drums and the older guitars are not Pro instruments */
    CHECK(!SDL_RB3Pro_GetDevice(0x12BA, 0x0200, &device));
    CHECK(!SDL_RB3Pro_GetDevice(0x12BA, 0x0218, &device));
    CHECK(!SDL_RB3Pro_GetDevice(0x1BAD, 0x3138, &device));
    CHECK(!SDL_RB3Pro_GetDevice(0x1BAD, 0x3010, &device));
    /* The Xbox 360 models go through XInput, not this table */
    CHECK(!SDL_RB3Pro_GetDevice(0x1BAD, 0x1330, &device));
    CHECK(!SDL_RB3Pro_GetDevice(0x1BAD, 0x1430, &device));
    /* The vendors swapped */
    CHECK(!SDL_RB3Pro_GetDevice(0x1BAD, 0x2330, &device));
    CHECK(!SDL_RB3Pro_GetDevice(0x12BA, 0x3330, &device));

    CHECK(SDL_RB3Pro_GetLayout(SDL_RB3PRO_KEYBOARD, &nbuttons, &naxes, &nhats));
    CHECK(nbuttons == 39 && naxes == 7 && nhats == 1);
    CHECK(SDL_RB3Pro_GetLayout(SDL_RB3PRO_GUITAR, &nbuttons, &naxes, &nhats));
    CHECK(nbuttons == 19 && naxes == 15 && nhats == 1);
    CHECK(!SDL_RB3Pro_GetLayout(SDL_RB3PRO_NONE, &nbuttons, &naxes, &nhats));
    CHECK(SDL_RB3Pro_GetLayout(SDL_RB3PRO_GUITAR, NULL, NULL, NULL));

    /* X360 4: only subtypes 15 and 25 are Pro instruments */
    CHECK(SDL_RB3Pro_VariantForXInputSubtype(0x0F) == SDL_RB3PRO_KEYBOARD);
    CHECK(SDL_RB3Pro_VariantForXInputSubtype(0x19) == SDL_RB3PRO_GUITAR);
    {
        static const uint8_t others[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0B, 0x10, 0x13, 0xFF };
        for (i = 0; i < sizeof(others); ++i) {
            CHECK(SDL_RB3Pro_VariantForXInputSubtype(others[i]) == SDL_RB3PRO_NONE);
        }
    }
    CHECK(strcmp(SDL_RB3Pro_XInputName(SDL_RB3PRO_KEYBOARD), "Rock Band 3 Keyboard (Xbox 360)") == 0);
    CHECK(strcmp(SDL_RB3Pro_XInputName(SDL_RB3PRO_GUITAR), "Rock Band 3 Pro Guitar (Xbox 360)") == 0);
    CHECK(SDL_RB3Pro_XInputName(SDL_RB3PRO_NONE) == NULL);

    CHECK(strcmp(SDL_RB3PRO_MAPPING, "a:b0,b:b1,back:b4,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b5,start:b6,x:b2,y:b3,") == 0);
}

/* ------------------------------------------------------------------------ */
/* The enable, PS3 1 and 2 */

static void TestEnable(void)
{
    static const uint8_t rows[5][9] = {
        { 0x00, 0xE9, 0x00, 0x89, 0x1B, 0x00, 0x00, 0x00, 0x02 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x89, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x00, 0xE9, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    };
    uint8_t single[41];
    uint8_t row[9];
    uint8_t report[27];
    SDL_RB3ProEnable enable;
    SDL_RB3ProDevice device;
    int i;

    /* The single transfer: report ID 0 and the 40 bytes Linux, Dolphin and
       RPCS3 match on */
    SDL_RB3Pro_BuildEnable(single);
    CHECK(single[0] == 0x00);
    for (i = 0; i < 5; ++i) {
        CHECK(memcmp(&single[1 + i * 8], &rows[i][1], 8) == 0);
    }
    CHECK(single[3] == 0x89);

    /* PS3 1: open sends five rows, 1 and 2 back to back, then a 1473 us
       pause after each of rows 2 to 5 */
    CHECK(SDL_RB3Pro_GetDevice(0x12BA, 0x2330, &device) && device.ps3);
    SDL_RB3ProEnable_Init(&enable, device.ps3, true);
    CHECK(!SDL_RB3ProEnable_NextRow(&enable, 0, row));
    CHECK(SDL_RB3ProEnable_Open(&enable, 0));
    CHECK(SDL_RB3ProEnable_NextRow(&enable, 0, row) && memcmp(row, rows[0], 9) == 0);
    CHECK(SDL_RB3ProEnable_NextRow(&enable, 0, row) && memcmp(row, rows[1], 9) == 0);
    CHECK(!SDL_RB3ProEnable_NextRow(&enable, 0, row));
    CHECK(!SDL_RB3ProEnable_NextRow(&enable, 1472, row));
    CHECK(SDL_RB3ProEnable_NextRow(&enable, 1473, row) && memcmp(row, rows[2], 9) == 0);
    CHECK(!SDL_RB3ProEnable_NextRow(&enable, 2945, row));
    CHECK(SDL_RB3ProEnable_NextRow(&enable, 2946, row) && memcmp(row, rows[3], 9) == 0);
    CHECK(!SDL_RB3ProEnable_NextRow(&enable, 4418, row));
    CHECK(SDL_RB3ProEnable_NextRow(&enable, 4419, row) && memcmp(row, rows[4], 9) == 0);
    for (i = 0; i < 100; ++i) {
        CHECK(!SDL_RB3ProEnable_NextRow(&enable, 4419 + (uint64_t)i * 1000, row));
    }
    /* A late update sends a row at once, and the pause runs from it */
    SDL_RB3ProEnable_Init(&enable, true, true);
    CHECK(SDL_RB3ProEnable_Open(&enable, 0));
    CHECK(SDL_RB3ProEnable_NextRow(&enable, 5000, row) && memcmp(row, rows[0], 9) == 0);
    CHECK(SDL_RB3ProEnable_NextRow(&enable, 5000, row) && memcmp(row, rows[1], 9) == 0);
    CHECK(!SDL_RB3ProEnable_NextRow(&enable, 6472, row));
    CHECK(SDL_RB3ProEnable_NextRow(&enable, 20000, row) && memcmp(row, rows[2], 9) == 0);
    CHECK(!SDL_RB3ProEnable_NextRow(&enable, 21472, row));
    CHECK(SDL_RB3ProEnable_NextRow(&enable, 21473, row) && memcmp(row, rows[3], 9) == 0);

    /* Unsplit: a start, and no rows */
    SDL_RB3ProEnable_Init(&enable, true, false);
    CHECK(SDL_RB3ProEnable_Open(&enable, 0));
    CHECK(!SDL_RB3ProEnable_NextRow(&enable, 0, row));
    CHECK(!SDL_RB3ProEnable_NextRow(&enable, 1000000, row));

    /* Every PS3 ID is armed and every Wii ID sends nothing, ever */
    {
        static const uint16_t ps3[6] = { 0x2330, 0x2338, 0x2430, 0x2438, 0x2530, 0x2538 };
        static const uint16_t wii[6] = { 0x3330, 0x3338, 0x3430, 0x3438, 0x3530, 0x3538 };
        for (i = 0; i < 6; ++i) {
            CHECK(SDL_RB3Pro_GetDevice(0x12BA, ps3[i], &device) && device.ps3);
            CHECK(SDL_RB3Pro_GetDevice(0x1BAD, wii[i], &device) && !device.ps3);
            SDL_RB3ProEnable_Init(&enable, device.ps3, true);
            CHECK(!SDL_RB3ProEnable_Open(&enable, 0));
            memcpy(report, keyboard_rest, sizeof(report));
            report[24] = 0x02;
            CHECK(!SDL_RB3ProEnable_OnReport(&enable, report, 27, MS(1000)));
            CHECK(!SDL_RB3ProEnable_OnReport(&enable, report, 27, MS(100000)));
            CHECK(!SDL_RB3ProEnable_NextRow(&enable, MS(100000), row));
        }
    }

    /* PS3 2: Linux's rule, strictly after the 8 s mark */
    SDL_RB3ProEnable_Init(&enable, true, true);
    CHECK(SDL_RB3ProEnable_Open(&enable, 0));
    while (SDL_RB3ProEnable_NextRow(&enable, MS(10), row)) {
    }
    memcpy(report, keyboard_rest, sizeof(report));
    report[24] = 0x02;
    CHECK(!SDL_RB3ProEnable_OnReport(&enable, report, 27, MS(1000)));
    CHECK(!SDL_RB3ProEnable_OnReport(&enable, report, 27, MS(8000)));
    CHECK(SDL_RB3ProEnable_OnReport(&enable, report, 27, MS(8001)));
    {
        int sent = 0;
        uint64_t t = MS(8001);
        while (sent < 5 && t < MS(9000)) {
            if (SDL_RB3ProEnable_NextRow(&enable, t, row)) {
                CHECK(memcmp(row, rows[sent], 9) == 0);
                ++sent;
            } else {
                t += 100;
            }
        }
        CHECK(sent == 5);
        CHECK(!SDL_RB3ProEnable_NextRow(&enable, MS(10000), row));
    }
    /* A full report stops it */
    report[24] = 0xE0;
    CHECK(!SDL_RB3ProEnable_OnReport(&enable, report, 27, MS(20000)));
    CHECK(!SDL_RB3ProEnable_OnReport(&enable, report, 27, MS(30000)));
    /* A navigation report 30 s on sends them again at once */
    report[24] = 0x02;
    CHECK(SDL_RB3ProEnable_OnReport(&enable, report, 27, MS(38001)));
    CHECK(SDL_RB3ProEnable_NextRow(&enable, MS(38001), row) && memcmp(row, rows[0], 9) == 0);
    /* Reports shorter than 25 bytes never trigger it */
    SDL_RB3ProEnable_Init(&enable, true, true);
    CHECK(SDL_RB3ProEnable_Open(&enable, 0));
    for (i = 0; i <= 24; ++i) {
        CHECK(!SDL_RB3ProEnable_OnReport(&enable, report, (size_t)i, MS(100000)));
    }
    CHECK(SDL_RB3ProEnable_OnReport(&enable, report, 25, MS(100000)));
    /* The guitar's rest reads 0x00 in byte 24 and triggers nothing */
    SDL_RB3ProEnable_Init(&enable, true, true);
    CHECK(SDL_RB3ProEnable_Open(&enable, 0));
    CHECK(!SDL_RB3ProEnable_OnReport(&enable, guitar_rest, 27, MS(100000)));
    CHECK(!SDL_RB3ProEnable_OnReport(&enable, NULL, 27, MS(100000)));
}

/* ------------------------------------------------------------------------ */
/* The keyboard, PS3 3 to 5 */

static void TestKeyboard(void)
{
    uint8_t report[27];
    SDL_RB3ProOutput out, rest;
    int key, i;

    CHECK(Decode(SDL_RB3PRO_KEYBOARD, keyboard_rest, 27, &rest));
    CHECK(rest.buttons == 0);
    CHECK(rest.button_mask == AllButtons(SDL_RB3PRO_KEYBOARD_BUTTONS));
    CHECK(rest.axis_mask == 0x7F);
    CHECK(rest.hat == SDL_RB3PRO_HAT_CENTERED);
    for (i = 0; i < 7; ++i) {
        CHECK(rest.axes[i] == 0);
    }
    CheckEquivalent(SDL_RB3PRO_KEYBOARD, keyboard_rest);

    /* PS3 3: the named keys */
    {
        static const struct
        {
            int byte;
            uint8_t value;
            int button;
        } named[] = {
            { 5, 0x80, 13 }, /* C1 */
            { 5, 0x01, 20 }, /* G1 */
            { 6, 0x80, 21 }, /* Ab1 */
            { 7, 0x80, 29 }, /* E2 */
            { 7, 0x01, 36 }, /* B2 */
            { 8, 0x80, 37 }, /* C3 */
        };
        for (i = 0; i < (int)(sizeof(named) / sizeof(named[0])); ++i) {
            memcpy(report, keyboard_rest, sizeof(report));
            report[named[i].byte] = named[i].value;
            CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
            CHECK(CountDown(&out) == 1 && Down(&out, named[i].button));
            CHECK(out.axes[SDL_RB3PRO_KEYBOARD_FIRST_VELOCITY] == 0);
            CheckEquivalent(SDL_RB3PRO_KEYBOARD, report);
        }
    }
    /* All 25 single-bit vectors give exactly one key, in order */
    for (key = 0; key < 25; ++key) {
        memcpy(report, keyboard_rest, sizeof(report));
        report[5 + key / 8] = (uint8_t)(0x80 >> (key % 8));
        CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
        CHECK(CountDown(&out) == 1);
        CHECK(Down(&out, SDL_RB3PRO_KEYBOARD_FIRST_KEY + key));
        CheckEquivalent(SDL_RB3PRO_KEYBOARD, report);
    }

    /* PS3 4: byte 8 holds C3 and slot 1, bytes 9 to 12 slots 2 to 5 */
    memcpy(report, keyboard_rest, sizeof(report));
    report[8] = 0xFF;
    CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
    CHECK(Down(&out, 37) && CountDown(&out) == 1);
    CHECK(out.axes[0] == 32767);
    CheckEquivalent(SDL_RB3PRO_KEYBOARD, report);
    for (i = 0; i < 4; ++i) {
        memcpy(report, keyboard_rest, sizeof(report));
        report[9 + i] = 0x7F;
        CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
        CHECK(out.axes[1 + i] == 32767);
        CHECK(CountDown(&out) == 0);
        report[9 + i] = 0x80;
        CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
        CHECK(Same(&out, &rest));
        CheckEquivalent(SDL_RB3PRO_KEYBOARD, report);
    }
    /* Every velocity value, every slot */
    for (i = 0; i < 128; ++i) {
        int slot;
        for (slot = 0; slot < 5; ++slot) {
            memcpy(report, keyboard_rest, sizeof(report));
            report[8 + slot] = (uint8_t)i;
            CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
            CHECK(out.axes[slot] == Scale7(i));
        }
    }

    /* PS3 5: overdrive, the pedal port and the touch strip */
    memcpy(report, keyboard_rest, sizeof(report));
    report[13] = 0x80;
    CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
    CHECK(Down(&out, SDL_RB3PRO_KEYBOARD_OVERDRIVE) && CountDown(&out) == 1);
    CheckEquivalent(SDL_RB3PRO_KEYBOARD, report);
    report[13] = 0x7F;
    CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
    CHECK(Same(&out, &rest));
    memcpy(report, keyboard_rest, sizeof(report));
    report[14] = 0x80;
    CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
    CHECK(Down(&out, SDL_RB3PRO_KEYBOARD_PEDAL) && CountDown(&out) == 1);
    CHECK(out.axes[SDL_RB3PRO_KEYBOARD_PEDAL_AXIS] == 0);
    report[14] = 0x7F;
    CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
    CHECK(!Down(&out, SDL_RB3PRO_KEYBOARD_PEDAL));
    CHECK(out.axes[SDL_RB3PRO_KEYBOARD_PEDAL_AXIS] == 32767);
    CheckEquivalent(SDL_RB3PRO_KEYBOARD, report);
    memcpy(report, keyboard_rest, sizeof(report));
    report[15] = 0x7F;
    CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
    CHECK(out.axes[SDL_RB3PRO_KEYBOARD_TOUCH_STRIP] == 32767);
    report[15] = 0xC0;
    CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
    CHECK(out.axes[SDL_RB3PRO_KEYBOARD_TOUCH_STRIP] == Scale7(0x40));
    CheckEquivalent(SDL_RB3PRO_KEYBOARD, report);
    memcpy(report, keyboard_rest, sizeof(report));
    report[20] = 0x01;
    CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
    CHECK(Down(&out, SDL_RB3PRO_KEYBOARD_PEDAL_CONNECTED) && CountDown(&out) == 1);
    CheckEquivalent(SDL_RB3PRO_KEYBOARD, report);
    /* RPCS3's rest value there, 0x02, is not the flag */
    CHECK(Decode(SDL_RB3PRO_KEYBOARD, keyboard_rest, 27, &out));
    CHECK(!Down(&out, SDL_RB3PRO_KEYBOARD_PEDAL_CONNECTED));

    /* Bytes the keyboard does not use change nothing */
    {
        static const int unused[] = { 3, 4, 16, 17, 18, 19, 21, 22, 23, 24, 25, 26 };
        for (i = 0; i < (int)(sizeof(unused) / sizeof(unused[0])); ++i) {
            memcpy(report, keyboard_rest, sizeof(report));
            report[unused[i]] ^= 0xFE;
            CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
            CHECK(Same(&out, &rest));
        }
        memcpy(report, keyboard_rest, sizeof(report));
        report[13] = 0x7F;
        report[20] = 0xFE;
        CHECK(Decode(SDL_RB3PRO_KEYBOARD, report, 27, &out));
        CHECK(Same(&out, &rest));
    }
}

/* ------------------------------------------------------------------------ */
/* The Pro guitar, PS3 6 and 7 */

static void CheckFrets(const SDL_RB3ProOutput *o, const int frets[6])
{
    int i;
    for (i = 0; i < 6; ++i) {
        CHECK(o->axes[SDL_RB3PRO_GUITAR_FIRST_FRET + i] == Scale5(frets[i]));
    }
}

/* The encoding PlasticBand and RPCS3 agree on, written independently */
static void EncodeFrets(uint8_t *report, const int frets[6])
{
    const unsigned word1 = (unsigned)frets[0] | ((unsigned)frets[1] << 5) | ((unsigned)frets[2] << 10);
    const unsigned word2 = (unsigned)frets[3] | ((unsigned)frets[4] << 5) | ((unsigned)frets[5] << 10);
    report[5] = (uint8_t)(word1 & 0xFF);
    report[6] = (uint8_t)((report[6] & 0x80) | ((word1 >> 8) & 0x7F));
    report[7] = (uint8_t)(word2 & 0xFF);
    report[8] = (uint8_t)((report[8] & 0x80) | ((word2 >> 8) & 0x7F));
}

static void TestGuitar(void)
{
    uint8_t report[27];
    SDL_RB3ProOutput out, rest;
    int frets[6];
    int i, s;

    CHECK(Decode(SDL_RB3PRO_GUITAR, guitar_rest, 27, &rest));
    CHECK(rest.buttons == 0);
    CHECK(rest.button_mask == AllButtons(SDL_RB3PRO_GUITAR_BUTTONS));
    CHECK(rest.axis_mask == 0x7FFF);
    for (i = 0; i < 12; ++i) {
        CHECK(rest.axes[i] == 0);
    }
    CHECK(rest.axes[SDL_RB3PRO_GUITAR_TILT] == Scale7(0x40));
    CHECK(rest.axes[SDL_RB3PRO_GUITAR_MICROPHONE] == Scale7(0x40));
    CHECK(rest.axes[SDL_RB3PRO_GUITAR_LIGHT] == Scale7(0x40));
    CheckEquivalent(SDL_RB3PRO_GUITAR, guitar_rest);

    /* PS3 6: the split fields */
    memset(frets, 0, sizeof(frets));
    memcpy(report, guitar_rest, sizeof(report));
    report[5] = 0xE0;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
    frets[1] = 7;
    CheckFrets(&out, frets);
    memcpy(report, guitar_rest, sizeof(report));
    report[6] = 0x03;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
    frets[1] = 24;
    CheckFrets(&out, frets);
    CheckEquivalent(SDL_RB3PRO_GUITAR, report);
    memset(frets, 0, sizeof(frets));
    memcpy(report, guitar_rest, sizeof(report));
    report[5] = 0x1F;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
    frets[0] = 31;
    CheckFrets(&out, frets);
    memset(frets, 0, sizeof(frets));
    memcpy(report, guitar_rest, sizeof(report));
    report[8] = 0x7C;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
    frets[5] = 31;
    CheckFrets(&out, frets);
    CHECK(!Down(&out, SDL_RB3PRO_GUITAR_SOLO));
    memcpy(report, guitar_rest, sizeof(report));
    report[8] = 0x80;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
    CHECK(Down(&out, SDL_RB3PRO_GUITAR_SOLO) && CountDown(&out) == 1);
    memset(frets, 0, sizeof(frets));
    CheckFrets(&out, frets);
    CheckEquivalent(SDL_RB3PRO_GUITAR, report);
    /* Every fret value on every string, through an independent encoder */
    for (s = 0; s < 6; ++s) {
        for (i = 0; i < 32; ++i) {
            memset(frets, 0, sizeof(frets));
            frets[s] = i;
            memcpy(report, guitar_rest, sizeof(report));
            EncodeFrets(report, frets);
            CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
            CheckFrets(&out, frets);
            CHECK(CountDown(&out) == 0);
        }
    }
    /* All six at once, different values */
    for (i = 0; i < 6; ++i) {
        frets[i] = 3 + i * 5;
    }
    memcpy(report, guitar_rest, sizeof(report));
    EncodeFrets(report, frets);
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
    CheckFrets(&out, frets);
    CheckEquivalent(SDL_RB3PRO_GUITAR, report);

    /* The color flags and the velocities */
    for (i = 0; i < 5; ++i) {
        memcpy(report, guitar_rest, sizeof(report));
        report[9 + i] = 0x80;
        CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
        CHECK(Down(&out, SDL_RB3PRO_GUITAR_FIRST_COLOR + i) && CountDown(&out) == 1);
        CHECK(out.axes[SDL_RB3PRO_GUITAR_FIRST_VELOCITY + i] == 0);
        CheckEquivalent(SDL_RB3PRO_GUITAR, report);
        report[9 + i] = 0x7F;
        CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
        CHECK(CountDown(&out) == 0);
        CHECK(out.axes[SDL_RB3PRO_GUITAR_FIRST_VELOCITY + i] == 32767);
    }
    /* High E has a velocity and no flag */
    memcpy(report, guitar_rest, sizeof(report));
    report[14] = 0xFF;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
    CHECK(CountDown(&out) == 0);
    CHECK(out.axes[SDL_RB3PRO_GUITAR_FIRST_VELOCITY + 5] == 32767);
    CheckEquivalent(SDL_RB3PRO_GUITAR, report);

    /* PS3 7: the tilt, the sensors and the pedal port */
    memcpy(report, guitar_rest, sizeof(report));
    report[17] = 0x40;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out) && out.axes[SDL_RB3PRO_GUITAR_TILT] == Scale7(0x40));
    report[17] = 0x7F;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out) && out.axes[SDL_RB3PRO_GUITAR_TILT] == 32767);
    CHECK(out.axes[SDL_RB3PRO_GUITAR_MICROPHONE] == Scale7(0x40));
    report[15] = 0x10;
    report[16] = 0x70;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
    CHECK(out.axes[SDL_RB3PRO_GUITAR_MICROPHONE] == Scale7(0x10));
    CHECK(out.axes[SDL_RB3PRO_GUITAR_LIGHT] == Scale7(0x70));
    CheckEquivalent(SDL_RB3PRO_GUITAR, report);
    memcpy(report, guitar_rest, sizeof(report));
    report[18] = 0x80;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
    CHECK(Down(&out, SDL_RB3PRO_GUITAR_PEDAL) && CountDown(&out) == 1);
    report[18] = 0x7F;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
    CHECK(Same(&out, &rest));
    memcpy(report, guitar_rest, sizeof(report));
    report[20] = 0x01;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
    CHECK(Down(&out, SDL_RB3PRO_GUITAR_PEDAL_CONNECTED) && CountDown(&out) == 1);
    CheckEquivalent(SDL_RB3PRO_GUITAR, report);
    /* A byte above 127 saturates */
    memcpy(report, guitar_rest, sizeof(report));
    report[17] = 0xFF;
    CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out) && out.axes[SDL_RB3PRO_GUITAR_TILT] == 32767);
    /* Unused bytes change nothing */
    {
        static const int unused[] = { 3, 4, 19, 21, 22, 23, 24, 25, 26 };
        for (i = 0; i < (int)(sizeof(unused) / sizeof(unused[0])); ++i) {
            memcpy(report, guitar_rest, sizeof(report));
            report[unused[i]] ^= 0xFF;
            CHECK(Decode(SDL_RB3PRO_GUITAR, report, 27, &out));
            CHECK(Same(&out, &rest));
        }
    }
}

/* ------------------------------------------------------------------------ */
/* PS3 8 and 9 */

static void TestNavigationAndLengths(void)
{
    uint8_t report[40];
    uint8_t stale[40];
    SDL_RB3ProOutput out, rest;
    int variant, i;
    size_t length;

    for (variant = SDL_RB3PRO_KEYBOARD; variant <= SDL_RB3PRO_GUITAR; ++variant) {
        const uint8_t *base = (variant == SDL_RB3PRO_KEYBOARD) ? keyboard_rest : guitar_rest;
        static const struct
        {
            int byte;
            uint8_t value;
            int button;
        } buttons[] = {
            { 0, 0x01, SDL_RB3PRO_BUTTON_WEST },
            { 0, 0x02, SDL_RB3PRO_BUTTON_SOUTH },
            { 0, 0x04, SDL_RB3PRO_BUTTON_EAST },
            { 0, 0x08, SDL_RB3PRO_BUTTON_NORTH },
            { 1, 0x01, SDL_RB3PRO_BUTTON_BACK },
            { 1, 0x02, SDL_RB3PRO_BUTTON_START },
            { 1, 0x10, SDL_RB3PRO_BUTTON_GUIDE },
        };

        CHECK(Decode(variant, base, 27, &rest));

        /* PS3 9: the face buttons, select, start and PS one at a time */
        for (i = 0; i < (int)(sizeof(buttons) / sizeof(buttons[0])); ++i) {
            memcpy(report, base, 27);
            report[buttons[i].byte] = buttons[i].value;
            CHECK(Decode(variant, report, 27, &out));
            CHECK(Down(&out, buttons[i].button) && CountDown(&out) == 1);
            CheckEquivalent(variant, report);
        }
        /* The bits no instrument uses */
        {
            static const struct
            {
                int byte;
                uint8_t value;
            } nothing[] = { { 0, 0x10 }, { 0, 0x20 }, { 0, 0x40 }, { 0, 0x80 },
                            { 1, 0x04 }, { 1, 0x08 }, { 1, 0x20 }, { 1, 0x40 }, { 1, 0x80 } };
            for (i = 0; i < (int)(sizeof(nothing) / sizeof(nothing[0])); ++i) {
                memcpy(report, base, 27);
                report[nothing[i].byte] = nothing[i].value;
                CHECK(Decode(variant, report, 27, &out));
                CHECK(Same(&out, &rest));
            }
        }
        /* Every d-pad value */
        for (i = 0; i < 256; ++i) {
            memcpy(report, base, 27);
            report[2] = (uint8_t)i;
            CHECK(Decode(variant, report, 27, &out));
            CHECK(out.hat == (i < 8 ? clockwise[i] : SDL_RB3PRO_HAT_CENTERED));
            CHECK(CountDown(&out) == 0);
            if (i <= 8) {
                CheckEquivalent(variant, report);
            }
        }

        /* PS3 8: every truncation changes nothing */
        for (length = 0; length < 27; ++length) {
            CHECK(!Decode(variant, base, length, &out));
            CHECK(Untouched(&out));
            memset(stale, 0xFF, sizeof(stale));
            memcpy(stale, base, length);
            memset(&out, 0xA5, sizeof(out));
            CHECK(!SDL_RB3Pro_DecodeReport(variant, stale, length, &out));
            CHECK(Untouched(&out));
        }
        /* A longer read decodes its first 27 bytes */
        memset(report, 0xFF, sizeof(report));
        memcpy(report, base, 27);
        CHECK(Decode(variant, report, sizeof(report), &out));
        CHECK(Same(&out, &rest));
    }

    /* Arguments */
    CHECK(!Decode(SDL_RB3PRO_NONE, keyboard_rest, 27, &out));
    CHECK(Untouched(&out));
    CHECK(!Decode(3, keyboard_rest, 27, &out));
    CHECK(!SDL_RB3Pro_DecodeReport(SDL_RB3PRO_KEYBOARD, NULL, 27, &out));
    CHECK(!SDL_RB3Pro_DecodeReport(SDL_RB3PRO_KEYBOARD, keyboard_rest, 27, NULL));
}

/* ------------------------------------------------------------------------ */
/* Xbox 360, X360 1, 2, 5 and 6 */

static void TestXInput(void)
{
    SDL_RB3ProXInputState s;
    SDL_RB3ProOutput out;
    int i;

    /* X360 1: the keyboard */
    memset(&s, 0, sizeof(s));
    s.has_trailing = true;
    s.left_trigger = 0x80;
    CHECK(DecodeX(SDL_RB3PRO_KEYBOARD, &s, &out));
    CHECK(Down(&out, 13) && CountDown(&out) == 1);
    memset(&s, 0, sizeof(s));
    s.has_trailing = true;
    s.thumb_lx = (int16_t)-32768;
    CHECK(DecodeX(SDL_RB3PRO_KEYBOARD, &s, &out));
    CHECK(Down(&out, 37) && CountDown(&out) == 1);
    CHECK(out.axes[0] == 0);
    s.thumb_lx = 0x7F00;
    CHECK(DecodeX(SDL_RB3PRO_KEYBOARD, &s, &out));
    CHECK(CountDown(&out) == 0 && out.axes[0] == 32767);
    memset(&s, 0, sizeof(s));
    s.has_trailing = true;
    s.thumb_ry = 0x0080;
    CHECK(DecodeX(SDL_RB3PRO_KEYBOARD, &s, &out));
    CHECK(Down(&out, SDL_RB3PRO_KEYBOARD_OVERDRIVE) && CountDown(&out) == 1);
    s.thumb_ry = (int16_t)-32768;
    CHECK(DecodeX(SDL_RB3PRO_KEYBOARD, &s, &out));
    CHECK(Down(&out, SDL_RB3PRO_KEYBOARD_PEDAL) && CountDown(&out) == 1);
    s.thumb_ry = 0x7F00;
    CHECK(DecodeX(SDL_RB3PRO_KEYBOARD, &s, &out));
    CHECK(CountDown(&out) == 0 && out.axes[SDL_RB3PRO_KEYBOARD_PEDAL_AXIS] == 32767);

    /* X360 2: the Pro guitar */
    {
        int frets[6];
        memset(frets, 0, sizeof(frets));
        memset(&s, 0, sizeof(s));
        s.has_trailing = true;
        s.left_trigger = 0x1F;
        CHECK(DecodeX(SDL_RB3PRO_GUITAR, &s, &out));
        frets[0] = 31;
        CheckFrets(&out, frets);
        memset(frets, 0, sizeof(frets));
        s.left_trigger = 0xE0;
        s.right_trigger = 0x03;
        CHECK(DecodeX(SDL_RB3PRO_GUITAR, &s, &out));
        frets[1] = 31;
        CheckFrets(&out, frets);
    }
    memset(&s, 0, sizeof(s));
    s.has_trailing = true;
    s.thumb_lx = (int16_t)-32768;
    CHECK(DecodeX(SDL_RB3PRO_GUITAR, &s, &out));
    CHECK(Down(&out, SDL_RB3PRO_GUITAR_SOLO) && CountDown(&out) == 1);
    memset(&s, 0, sizeof(s));
    s.has_trailing = true;
    s.thumb_ly = 0x0080;
    CHECK(DecodeX(SDL_RB3PRO_GUITAR, &s, &out));
    CHECK(Down(&out, SDL_RB3PRO_GUITAR_FIRST_COLOR) && CountDown(&out) == 1);
    s.thumb_ly = (int16_t)-32768;
    CHECK(DecodeX(SDL_RB3PRO_GUITAR, &s, &out));
    CHECK(Down(&out, SDL_RB3PRO_GUITAR_FIRST_COLOR + 1) && CountDown(&out) == 1);

    /* The gamepad part comes from wButtons */
    {
        static const struct
        {
            uint16_t mask;
            int button;
        } map[] = {
            { 0x1000, SDL_RB3PRO_BUTTON_SOUTH }, { 0x2000, SDL_RB3PRO_BUTTON_EAST },
            { 0x4000, SDL_RB3PRO_BUTTON_WEST }, { 0x8000, SDL_RB3PRO_BUTTON_NORTH },
            { 0x0020, SDL_RB3PRO_BUTTON_BACK }, { 0x0010, SDL_RB3PRO_BUTTON_START },
            { 0x0400, SDL_RB3PRO_BUTTON_GUIDE },
        };
        static const uint16_t nothing[] = { 0x0040, 0x0080, 0x0100, 0x0200, 0x0800 };
        for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); ++i) {
            memset(&s, 0, sizeof(s));
            s.buttons = map[i].mask;
            CHECK(DecodeX(SDL_RB3PRO_GUITAR, &s, &out));
            CHECK(Down(&out, map[i].button) && CountDown(&out) == 1);
        }
        for (i = 0; i < (int)(sizeof(nothing) / sizeof(nothing[0])); ++i) {
            memset(&s, 0, sizeof(s));
            s.buttons = nothing[i];
            CHECK(DecodeX(SDL_RB3PRO_KEYBOARD, &s, &out));
            CHECK(CountDown(&out) == 0 && out.hat == SDL_RB3PRO_HAT_CENTERED);
        }
        memset(&s, 0, sizeof(s));
        s.buttons = 0x0001 | 0x0008;
        CHECK(DecodeX(SDL_RB3PRO_KEYBOARD, &s, &out));
        CHECK(out.hat == (SDL_RB3PRO_HAT_UP | SDL_RB3PRO_HAT_RIGHT));
        s.buttons = 0x0002 | 0x0004;
        CHECK(DecodeX(SDL_RB3PRO_KEYBOARD, &s, &out));
        CHECK(out.hat == (SDL_RB3PRO_HAT_DOWN | SDL_RB3PRO_HAT_LEFT));
    }

    /* X360 5: the byte OpenXInput reads as Share is trailing byte 5, whose
       bit 0 is the pedal connection. It never touches button 11. */
    memset(&s, 0, sizeof(s));
    s.has_trailing = true;
    s.trailing[5] = 0x01;
    CHECK(DecodeX(SDL_RB3PRO_KEYBOARD, &s, &out));
    CHECK(Down(&out, SDL_RB3PRO_KEYBOARD_PEDAL_CONNECTED) && CountDown(&out) == 1);
    CHECK(!Down(&out, 11));
    CHECK(DecodeX(SDL_RB3PRO_GUITAR, &s, &out));
    CHECK(Down(&out, SDL_RB3PRO_GUITAR_PEDAL_CONNECTED) && CountDown(&out) == 1);
    CHECK(!Down(&out, 11));

    /* X360 6: without the trailing bytes the touch strip, the tilt, the
       sensors, the guitar's pedal and the pedal connection read absent,
       whatever the buffer holds */
    memset(&s, 0, sizeof(s));
    s.has_trailing = false;
    memset(s.trailing, 0xFF, sizeof(s.trailing));
    CHECK(DecodeX(SDL_RB3PRO_KEYBOARD, &s, &out));
    CHECK(out.axes[SDL_RB3PRO_KEYBOARD_TOUCH_STRIP] == 0);
    CHECK((out.axis_mask >> SDL_RB3PRO_KEYBOARD_TOUCH_STRIP) & 1);
    CHECK(!Down(&out, SDL_RB3PRO_KEYBOARD_PEDAL_CONNECTED));
    CHECK(CountDown(&out) == 0);
    CHECK(DecodeX(SDL_RB3PRO_GUITAR, &s, &out));
    CHECK(out.axes[SDL_RB3PRO_GUITAR_TILT] == 0);
    CHECK(out.axes[SDL_RB3PRO_GUITAR_MICROPHONE] == 0);
    CHECK(out.axes[SDL_RB3PRO_GUITAR_LIGHT] == 0);
    CHECK(!Down(&out, SDL_RB3PRO_GUITAR_PEDAL));
    CHECK(!Down(&out, SDL_RB3PRO_GUITAR_PEDAL_CONNECTED));
    CHECK(CountDown(&out) == 0);
    /* The public bytes still decode */
    s.left_trigger = 0x80;
    s.thumb_lx = 0x7F00;
    CHECK(DecodeX(SDL_RB3PRO_KEYBOARD, &s, &out));
    CHECK(Down(&out, 13) && out.axes[0] == 32767);

    /* Arguments */
    CHECK(!DecodeX(SDL_RB3PRO_NONE, &s, &out));
    CHECK(Untouched(&out));
    CHECK(!SDL_RB3Pro_DecodeXInput(SDL_RB3PRO_KEYBOARD, NULL, &out));
    CHECK(!SDL_RB3Pro_DecodeXInput(SDL_RB3PRO_KEYBOARD, &s, NULL));
}

/* X360 3 over every bit of the shared block: each single bit of PS3 bytes 5
   to 20, on both variants, decodes the same through both transports */
static void TestEquivalence(void)
{
    uint8_t report[27];
    int variant, byte, bit;

    for (variant = SDL_RB3PRO_KEYBOARD; variant <= SDL_RB3PRO_GUITAR; ++variant) {
        const uint8_t *base = (variant == SDL_RB3PRO_KEYBOARD) ? keyboard_rest : guitar_rest;
        for (byte = 5; byte <= 20; ++byte) {
            for (bit = 0; bit < 8; ++bit) {
                memcpy(report, base, sizeof(report));
                report[byte] ^= (uint8_t)(1u << bit);
                CheckEquivalent(variant, report);
            }
            memcpy(report, base, sizeof(report));
            report[byte] = 0xFF;
            CheckEquivalent(variant, report);
        }
    }
}

int main(void)
{
    TestIdentity();
    TestEnable();
    TestKeyboard();
    TestGuitar();
    TestNavigationAndLengths();
    TestXInput();
    TestEquivalence();

    /* X360 3 ran inside the PS3 groups and over the whole block */
    CHECK(equivalence_vectors >= 2 * 16 * 9);

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures (%d vectors through both transports)\n", checks, equivalence_vectors);
    return 0;
}
