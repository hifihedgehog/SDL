/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/SDL_ghl_proto.c and the
   GameInput raw type table, the Guitar Hero Live guitars of
   hifihedgehog/SDL#33 Part 3. The numbers follow the part's replay tests.
   Every report is fed as an exact-size heap copy, so AddressSanitizer
   catches a read past its length, and truncated inside a buffer of stale
   bytes. */

#include "../src/joystick/SDL_ghl_proto.h"
#include "../src/joystick/gdk/SDL_gameinput_rawtype.h"

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

#define AXIS_MIN (-32768)
#define AXIS_MAX 32767
#define ALL_BUTTONS 0x07FF

/* ------------------------------------------------------------------------ */
/* Helpers */

/* Test 1's rest frame, from RPCS3's emulated guitar */
static const uint8_t frame_a_rest[27] = {
    0x00, 0x00, 0x0F, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x02
};

static void FrameBRest(uint8_t report[64])
{
    memset(report, 0, 64);
    report[0] = 0x01;
    report[1] = 0x80;
    report[2] = 0x80;
    report[3] = 0x80;
    report[4] = 0x80;
    report[5] = 0x08;
    report[19] = 0x80;
    report[21] = 0x00;
    report[22] = 0x01;
    report[25] = 0x01;
}

static const SDL_GHLOutput sentinel = { 0xABCD, 0x1234, { 1, 2, 3, 4, 5, 6 }, 0x5A };

static bool DecodeA(const uint8_t *report, size_t length, SDL_GHLOutput *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    *out = sentinel;
    result = SDL_GHL_DecodeFrameA(copy, length, out);
    free(copy);
    return result;
}

static bool DecodeB(const uint8_t *report, size_t length, SDL_GHLOutput *out, bool *connected)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    *out = sentinel;
    result = SDL_GHL_DecodeFrameB(copy, length, out, connected);
    free(copy);
    return result;
}

static bool DecodeXbox(uint8_t message, const uint8_t *payload, size_t length, SDL_GHLOutput *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, payload, length);
    }
    *out = sentinel;
    result = SDL_GHL_DecodeXboxMessage(message, copy, length, out);
    free(copy);
    return result;
}

/* Field by field: struct assignment need not copy padding */
static bool Same(const SDL_GHLOutput *a, const SDL_GHLOutput *b)
{
    int i;

    if (a->buttons != b->buttons || a->button_mask != b->button_mask || a->hat != b->hat) {
        return false;
    }
    for (i = 0; i < SDL_GHL_NUM_AXES; ++i) {
        if (a->axes[i] != b->axes[i]) {
            return false;
        }
    }
    return true;
}

static bool IsSentinel(const SDL_GHLOutput *o)
{
    return Same(o, &sentinel);
}

static bool Down(const SDL_GHLOutput *o, int button)
{
    return ((o->buttons >> button) & 1) != 0;
}

/* Exactly one button down, and it is this one */
static bool Only(const SDL_GHLOutput *o, int button)
{
    return o->buttons == (uint16_t)(1u << button);
}

static int16_t Scale(uint8_t value)
{
    return (int16_t)(value * 257 - 32768);
}

static const uint8_t clockwise[8] = {
    SDL_GHL_HAT_UP,
    SDL_GHL_HAT_UP | SDL_GHL_HAT_RIGHT,
    SDL_GHL_HAT_RIGHT,
    SDL_GHL_HAT_DOWN | SDL_GHL_HAT_RIGHT,
    SDL_GHL_HAT_DOWN,
    SDL_GHL_HAT_DOWN | SDL_GHL_HAT_LEFT,
    SDL_GHL_HAT_LEFT,
    SDL_GHL_HAT_UP | SDL_GHL_HAT_LEFT
};

static void CheckRestA(const SDL_GHLOutput *o, uint16_t mask)
{
    CHECK(o->buttons == 0);
    CHECK(o->button_mask == mask);
    CHECK(o->hat == SDL_GHL_HAT_CENTERED);
    CHECK(o->axes[SDL_GHL_AXIS_LEFTX] == 0);
    CHECK(o->axes[SDL_GHL_AXIS_LEFTY] == 128);
    CHECK(o->axes[SDL_GHL_AXIS_RIGHTY] == 128);
    CHECK(o->axes[SDL_GHL_AXIS_RIGHTX] == 128);
    CHECK(o->axes[SDL_GHL_AXIS_LEFT_TRIGGER] == AXIS_MIN);
    CHECK(o->axes[SDL_GHL_AXIS_RIGHT_TRIGGER] == AXIS_MIN);
}

/* ------------------------------------------------------------------------ */
/* Identity */

static void TestIdentity(void)
{
    CHECK(SDL_GHL_GetDongle(0x12BA, 0x074B) == SDL_GHL_DONGLE_PS3);
    CHECK(SDL_GHL_GetDongle(0x1430, 0x07BB) == SDL_GHL_DONGLE_PS4);
    CHECK(SDL_GHL_GetDongle(0x1430, 0x079B) == SDL_GHL_DONGLE_XBOXONE);
    /* The Xbox 360 dongle needs only a driver binding and is not here */
    CHECK(SDL_GHL_GetDongle(0x1430, 0x070B) == SDL_GHL_DONGLE_NONE);
    /* The PDP Jaguar guitar that the old key named */
    CHECK(SDL_GHL_GetDongle(0x1430, 0x0170) == SDL_GHL_DONGLE_NONE);
    CHECK(SDL_GHL_GetDongle(0x0E6F, 0x0170) == SDL_GHL_DONGLE_NONE);
    CHECK(SDL_GHL_GetDongle(0x12BA, 0x07BB) == SDL_GHL_DONGLE_NONE);
    CHECK(SDL_GHL_GetDongle(0x1430, 0x074B) == SDL_GHL_DONGLE_NONE);

    CHECK(strcmp(SDL_GHL_DongleName(SDL_GHL_DONGLE_PS3), "Guitar Hero Live Guitar (PS3/Wii U)") == 0);
    CHECK(strcmp(SDL_GHL_DongleName(SDL_GHL_DONGLE_PS4), "Guitar Hero Live Guitar (PS4)") == 0);
    CHECK(strcmp(SDL_GHL_DongleName(SDL_GHL_DONGLE_XBOXONE), "Guitar Hero Live Guitar (Xbox One)") == 0);
    CHECK(SDL_GHL_DongleName(SDL_GHL_DONGLE_NONE) == NULL);

    /* The constants the driver and GameInput use */
    CHECK(USB_VENDOR_RED_OCTANE == 0x1430);
    CHECK(USB_PRODUCT_RED_OCTANE_XB1_GUITAR_HERO_LIVE_GUITAR == 0x079B);
    CHECK(USB_PRODUCT_RED_OCTANE_PS4_GHLIVE_DONGLE == 0x07BB);
    CHECK(USB_VENDOR_SCEA == 0x12BA && USB_PRODUCT_SCEA_PS3WIIU_GHLIVE == 0x074B);
    CHECK(USB_PRODUCT_PDP_XB1_JAGUAR_GUITAR == 0x0170);
}

/* ------------------------------------------------------------------------ */
/* Frame A, tests 1 to 6 */

static void TestFrameA(void)
{
    static const int byte0_buttons[6] = {
        SDL_GHL_BUTTON_WEST, SDL_GHL_BUTTON_SOUTH, SDL_GHL_BUTTON_EAST,
        SDL_GHL_BUTTON_NORTH, SDL_GHL_BUTTON_LEFT_SHOULDER, SDL_GHL_BUTTON_RIGHT_SHOULDER
    };
    uint8_t report[27];
    SDL_GHLOutput out, rest;
    int i;

    /* 1. Rest */
    CHECK(DecodeA(frame_a_rest, sizeof(frame_a_rest), &rest));
    CheckRestA(&rest, ALL_BUTTONS);

    /* 2. Byte 0, the frets */
    for (i = 0; i < 6; ++i) {
        memcpy(report, frame_a_rest, sizeof(report));
        report[0] = (uint8_t)(1u << i);
        CHECK(DecodeA(report, sizeof(report), &out));
        CHECK(Only(&out, byte0_buttons[i]));
        CHECK(out.hat == SDL_GHL_HAT_CENTERED);
    }
    for (i = 6; i < 8; ++i) {
        memcpy(report, frame_a_rest, sizeof(report));
        report[0] = (uint8_t)(1u << i);
        CHECK(DecodeA(report, sizeof(report), &out));
        CHECK(Same(&out, &rest));
    }

    /* 3. Byte 1 */
    {
        static const uint8_t values[4] = { 0x01, 0x02, 0x04, 0x10 };
        static const int buttons[4] = {
            SDL_GHL_BUTTON_BACK, SDL_GHL_BUTTON_START, SDL_GHL_BUTTON_LEFT_STICK, SDL_GHL_BUTTON_GUIDE
        };
        static const uint8_t nothing[4] = { 0x08, 0x20, 0x40, 0x80 };
        for (i = 0; i < 4; ++i) {
            memcpy(report, frame_a_rest, sizeof(report));
            report[1] = values[i];
            CHECK(DecodeA(report, sizeof(report), &out));
            CHECK(Only(&out, buttons[i]));
        }
        for (i = 0; i < 4; ++i) {
            memcpy(report, frame_a_rest, sizeof(report));
            report[1] = nothing[i];
            CHECK(DecodeA(report, sizeof(report), &out));
            CHECK(Same(&out, &rest));
        }
    }
    /* The right stick button is never pressed but always posted */
    memcpy(report, frame_a_rest, sizeof(report));
    report[0] = 0xFF;
    report[1] = 0xFF;
    CHECK(DecodeA(report, sizeof(report), &out));
    CHECK(out.buttons == (ALL_BUTTONS & ~(1u << SDL_GHL_BUTTON_RIGHT_STICK)));
    CHECK(!Down(&out, SDL_GHL_BUTTON_RIGHT_STICK));

    /* 4. Byte 2, the d-pad */
    for (i = 0; i < 16; ++i) {
        memcpy(report, frame_a_rest, sizeof(report));
        report[2] = (uint8_t)i;
        CHECK(DecodeA(report, sizeof(report), &out));
        CHECK(out.hat == (i < 8 ? clockwise[i] : SDL_GHL_HAT_CENTERED));
        CHECK(out.buttons == 0);
    }
    /* Clockwise, not RPCS3's comment labels: 2 is right and 6 is left */
    report[2] = 2;
    CHECK(DecodeA(report, sizeof(report), &out) && out.hat == SDL_GHL_HAT_RIGHT);
    report[2] = 6;
    CHECK(DecodeA(report, sizeof(report), &out) && out.hat == SDL_GHL_HAT_LEFT);

    /* 5. Strum */
    memcpy(report, frame_a_rest, sizeof(report));
    report[4] = 0x00;
    CHECK(DecodeA(report, sizeof(report), &out));
    CHECK(out.hat == SDL_GHL_HAT_UP && out.axes[SDL_GHL_AXIS_LEFTY] == AXIS_MIN);
    report[4] = 0xFF;
    CHECK(DecodeA(report, sizeof(report), &out));
    CHECK(out.hat == SDL_GHL_HAT_DOWN && out.axes[SDL_GHL_AXIS_LEFTY] == AXIS_MAX);
    report[4] = 0x7F;
    CHECK(DecodeA(report, sizeof(report), &out));
    CHECK(out.hat == SDL_GHL_HAT_CENTERED && out.axes[SDL_GHL_AXIS_LEFTY] == -129);
    report[4] = 0x81;
    CHECK(DecodeA(report, sizeof(report), &out));
    CHECK(out.hat == SDL_GHL_HAT_CENTERED && out.axes[SDL_GHL_AXIS_LEFTY] == 385);
    CHECK(out.buttons == 0);
    /* Up and down only at 0x00 and 0xFF */
    for (i = 0; i < 256; ++i) {
        memcpy(report, frame_a_rest, sizeof(report));
        report[4] = (uint8_t)i;
        CHECK(DecodeA(report, sizeof(report), &out));
        CHECK(out.hat == (i == 0x00 ? SDL_GHL_HAT_UP : i == 0xFF ? SDL_GHL_HAT_DOWN : SDL_GHL_HAT_CENTERED));
        CHECK(out.axes[SDL_GHL_AXIS_LEFTY] == Scale((uint8_t)i));
    }
    /* The d-pad and the strum bar combine */
    report[2] = 6;
    report[4] = 0x00;
    CHECK(DecodeA(report, sizeof(report), &out));
    CHECK(out.hat == (SDL_GHL_HAT_LEFT | SDL_GHL_HAT_UP));
    report[2] = 0;
    report[4] = 0xFF;
    CHECK(DecodeA(report, sizeof(report), &out));
    CHECK(out.hat == (SDL_GHL_HAT_UP | SDL_GHL_HAT_DOWN));

    /* 6. Whammy, tilt, and byte 5 */
    memcpy(report, frame_a_rest, sizeof(report));
    report[6] = 0x80;
    CHECK(DecodeA(report, sizeof(report), &out) && out.axes[SDL_GHL_AXIS_RIGHTY] == 128);
    report[6] = 0xFF;
    CHECK(DecodeA(report, sizeof(report), &out) && out.axes[SDL_GHL_AXIS_RIGHTY] == AXIS_MAX);
    memcpy(report, frame_a_rest, sizeof(report));
    report[19] = 0x00;
    CHECK(DecodeA(report, sizeof(report), &out) && out.axes[SDL_GHL_AXIS_RIGHTX] == AXIS_MIN);
    report[19] = 0x80;
    CHECK(DecodeA(report, sizeof(report), &out) && out.axes[SDL_GHL_AXIS_RIGHTX] == 128);
    report[19] = 0xFF;
    CHECK(DecodeA(report, sizeof(report), &out) && out.axes[SDL_GHL_AXIS_RIGHTX] == AXIS_MAX);
    for (i = 0; i < 256; ++i) {
        memcpy(report, frame_a_rest, sizeof(report));
        report[19] = (uint8_t)i;
        report[6] = (uint8_t)(255 - i);
        CHECK(DecodeA(report, sizeof(report), &out));
        CHECK(out.axes[SDL_GHL_AXIS_RIGHTX] == Scale((uint8_t)i));
        CHECK(out.axes[SDL_GHL_AXIS_RIGHTY] == Scale((uint8_t)(255 - i)));
        CHECK(out.axes[SDL_GHL_AXIS_LEFTY] == 128);
    }
    for (i = 0; i < 256; ++i) {
        memcpy(report, frame_a_rest, sizeof(report));
        report[5] = (uint8_t)i;
        CHECK(DecodeA(report, sizeof(report), &out));
        CHECK(Same(&out, &rest));
    }
    /* The unused bytes change nothing either: 3, 7 to 18, 20 to 26 */
    {
        static const int unused[] = { 3, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 21, 22, 23, 24, 25, 26 };
        for (i = 0; i < (int)(sizeof(unused) / sizeof(unused[0])); ++i) {
            memcpy(report, frame_a_rest, sizeof(report));
            report[unused[i]] ^= 0xFF;
            CHECK(DecodeA(report, sizeof(report), &out));
            CHECK(Same(&out, &rest));
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Frame B, tests 7 and 8 */

static void TestFrameB(void)
{
    uint8_t report[64];
    SDL_GHLOutput out, rest;
    bool connected = false;
    int i;

    /* 7. Rest connects and holds nothing. Byte 25 = 0 disconnects. */
    FrameBRest(report);
    CHECK(DecodeB(report, sizeof(report), &rest, &connected));
    CHECK(connected);
    CheckRestA(&rest, ALL_BUTTONS);
    report[25] = 0x00;
    connected = true;
    CHECK(DecodeB(report, sizeof(report), &out, &connected));
    CHECK(!connected);
    CHECK(Same(&out, &rest));

    /* 8. One bit at a time */
    {
        static const uint8_t byte5[4] = { 0x18, 0x28, 0x48, 0x88 };
        static const int byte5_buttons[4] = {
            SDL_GHL_BUTTON_WEST, SDL_GHL_BUTTON_SOUTH, SDL_GHL_BUTTON_EAST, SDL_GHL_BUTTON_NORTH
        };
        static const uint8_t byte6[5] = { 0x01, 0x02, 0x20, 0x40, 0x80 };
        static const int byte6_buttons[5] = {
            SDL_GHL_BUTTON_LEFT_SHOULDER, SDL_GHL_BUTTON_RIGHT_SHOULDER, SDL_GHL_BUTTON_START,
            SDL_GHL_BUTTON_LEFT_STICK, SDL_GHL_BUTTON_BACK
        };
        static const uint8_t byte6_nothing[3] = { 0x04, 0x08, 0x10 };

        for (i = 0; i < 4; ++i) {
            FrameBRest(report);
            report[5] = byte5[i];
            CHECK(DecodeB(report, sizeof(report), &out, &connected));
            CHECK(Only(&out, byte5_buttons[i]));
            CHECK(out.hat == SDL_GHL_HAT_CENTERED);
        }
        for (i = 0; i < 5; ++i) {
            FrameBRest(report);
            report[6] = byte6[i];
            CHECK(DecodeB(report, sizeof(report), &out, &connected));
            CHECK(Only(&out, byte6_buttons[i]));
        }
        /* L2, R2 and share are no guitar control */
        for (i = 0; i < 3; ++i) {
            FrameBRest(report);
            report[6] = byte6_nothing[i];
            CHECK(DecodeB(report, sizeof(report), &out, &connected));
            CHECK(Same(&out, &rest));
        }
        FrameBRest(report);
        report[7] = 0x01;
        CHECK(DecodeB(report, sizeof(report), &out, &connected));
        CHECK(Only(&out, SDL_GHL_BUTTON_GUIDE));
        /* The touchpad click is no guitar control */
        report[7] = 0x02;
        CHECK(DecodeB(report, sizeof(report), &out, &connected));
        CHECK(Same(&out, &rest));
    }
    /* The d-pad: 0 to 7 clockwise, 8 and above neutral */
    for (i = 0; i < 16; ++i) {
        FrameBRest(report);
        report[5] = (uint8_t)i;
        CHECK(DecodeB(report, sizeof(report), &out, &connected));
        CHECK(out.hat == (i < 8 ? clockwise[i] : SDL_GHL_HAT_CENTERED));
        CHECK(out.buttons == 0);
    }
    /* Byte 3 is the tilt and byte 4 the whammy, byte 2 the strum bar */
    for (i = 0; i < 256; ++i) {
        FrameBRest(report);
        report[3] = (uint8_t)i;
        CHECK(DecodeB(report, sizeof(report), &out, &connected));
        CHECK(out.axes[SDL_GHL_AXIS_RIGHTX] == Scale((uint8_t)i));
        CHECK(out.axes[SDL_GHL_AXIS_RIGHTY] == 128);
        FrameBRest(report);
        report[4] = (uint8_t)i;
        CHECK(DecodeB(report, sizeof(report), &out, &connected));
        CHECK(out.axes[SDL_GHL_AXIS_RIGHTY] == Scale((uint8_t)i));
        CHECK(out.axes[SDL_GHL_AXIS_RIGHTX] == 128);
    }
    for (i = 0; i < 256; ++i) {
        FrameBRest(report);
        report[2] = (uint8_t)i;
        CHECK(DecodeB(report, sizeof(report), &out, &connected));
        CHECK(out.hat == (i == 0x00 ? SDL_GHL_HAT_UP : i == 0xFF ? SDL_GHL_HAT_DOWN : SDL_GHL_HAT_CENTERED));
        CHECK(out.axes[SDL_GHL_AXIS_LEFTY] == Scale((uint8_t)i));
    }
    FrameBRest(report);
    report[2] = 0x00;
    CHECK(DecodeB(report, sizeof(report), &out, &connected));
    CHECK(out.hat == SDL_GHL_HAT_UP && out.axes[SDL_GHL_AXIS_LEFTY] == AXIS_MIN);
    report[2] = 0xFF;
    CHECK(DecodeB(report, sizeof(report), &out, &connected));
    CHECK(out.hat == SDL_GHL_HAT_DOWN && out.axes[SDL_GHL_AXIS_LEFTY] == AXIS_MAX);
    /* Byte 1 and the copies of the tilt change nothing */
    FrameBRest(report);
    report[1] = 0x00;
    report[19] = 0x00;
    report[21] = 0x00;
    report[22] = 0x02;
    CHECK(DecodeB(report, sizeof(report), &out, &connected));
    CHECK(Same(&out, &rest));

    /* A report with another ID changes nothing */
    FrameBRest(report);
    report[0] = 0x02;
    report[5] = 0x18;
    connected = false;
    CHECK(!DecodeB(report, sizeof(report), &out, &connected));
    CHECK(IsSentinel(&out));
    CHECK(!connected);

    /* The structure's 67 bytes and a longer read are accepted */
    {
        uint8_t longer[80];
        FrameBRest(longer);
        memset(longer + 64, 0xEE, sizeof(longer) - 64);
        CHECK(DecodeB(longer, 67, &out, &connected));
        CHECK(Same(&out, &rest));
        CHECK(DecodeB(longer, sizeof(longer), &out, &connected));
        CHECK(Same(&out, &rest));
    }
}

/* ------------------------------------------------------------------------ */
/* Xbox One, test 9 */

static void TestXbox(void)
{
    uint8_t payload[40];
    SDL_GHLOutput out, rest;
    int i, message;

    CHECK(DecodeXbox(SDL_GHL_XBOX_MESSAGE_GUITAR, frame_a_rest, sizeof(frame_a_rest), &rest));
    CheckRestA(&rest, ALL_BUTTONS & ~(1u << SDL_GHL_BUTTON_GUIDE));

    /* The GIP guide message carries the d-pad center, not this report */
    memcpy(payload, frame_a_rest, 27);
    payload[1] = 0x10;
    CHECK(DecodeXbox(SDL_GHL_XBOX_MESSAGE_GUITAR, payload, 27, &out));
    CHECK(Same(&out, &rest));
    CHECK(!Down(&out, SDL_GHL_BUTTON_GUIDE));

    /* Every other control decodes as frame A */
    {
        SDL_GHLOutput a;
        memcpy(payload, frame_a_rest, 27);
        payload[0] = 0x3F;
        payload[1] = 0x07;
        payload[2] = 0x03;
        payload[4] = 0xFF;
        payload[6] = 0xC0;
        payload[19] = 0x20;
        CHECK(DecodeXbox(SDL_GHL_XBOX_MESSAGE_GUITAR, payload, 27, &out));
        CHECK(DecodeA(payload, 27, &a));
        CHECK(a.button_mask == ALL_BUTTONS);
        a.button_mask = (uint16_t)(a.button_mask & ~(1u << SDL_GHL_BUTTON_GUIDE));
        CHECK(Same(&out, &a));
        CHECK(out.buttons == ((1u << SDL_GHL_BUTTON_WEST) | (1u << SDL_GHL_BUTTON_SOUTH) |
                              (1u << SDL_GHL_BUTTON_EAST) | (1u << SDL_GHL_BUTTON_NORTH) |
                              (1u << SDL_GHL_BUTTON_LEFT_SHOULDER) | (1u << SDL_GHL_BUTTON_RIGHT_SHOULDER) |
                              (1u << SDL_GHL_BUTTON_BACK) | (1u << SDL_GHL_BUTTON_START) |
                              (1u << SDL_GHL_BUTTON_LEFT_STICK)));
        CHECK(out.hat == (SDL_GHL_HAT_DOWN | SDL_GHL_HAT_RIGHT));
        CHECK(out.axes[SDL_GHL_AXIS_LEFTY] == AXIS_MAX);
        CHECK(out.axes[SDL_GHL_AXIS_RIGHTY] == Scale(0xC0));
        CHECK(out.axes[SDL_GHL_AXIS_RIGHTX] == Scale(0x20));
    }

    /* 0x20 with any content changes nothing, nor does any other message */
    for (i = 0; i < 256; ++i) {
        memset(payload, i, sizeof(payload));
        CHECK(!DecodeXbox(SDL_GHL_XBOX_MESSAGE_NAVIGATION, payload, 14, &out));
        CHECK(IsSentinel(&out));
    }
    for (message = 0; message < 256; ++message) {
        if (message == SDL_GHL_XBOX_MESSAGE_GUITAR) {
            continue;
        }
        CHECK(!DecodeXbox((uint8_t)message, frame_a_rest, 27, &out));
        CHECK(IsSentinel(&out));
    }

    /* A 32-byte payload decodes its first 27 and ignores the rest */
    memcpy(payload, frame_a_rest, 27);
    for (i = 27; i < 32; ++i) {
        payload[i] = 0xFF;
    }
    CHECK(DecodeXbox(SDL_GHL_XBOX_MESSAGE_GUITAR, payload, 32, &out));
    CHECK(Same(&out, &rest));
    CHECK(!DecodeXbox(SDL_GHL_XBOX_MESSAGE_GUITAR, payload, 33, &out));
    CHECK(IsSentinel(&out));
}

/* ------------------------------------------------------------------------ */
/* Truncations, test 10 */

static void TestTruncations(void)
{
    uint8_t stale[64];
    uint8_t report[64];
    SDL_GHLOutput out;
    bool connected;
    size_t length;

    for (length = 0; length < SDL_GHL_FRAME_A_LENGTH; ++length) {
        CHECK(!DecodeA(frame_a_rest, length, &out));
        CHECK(IsSentinel(&out));
        CHECK(!DecodeXbox(SDL_GHL_XBOX_MESSAGE_GUITAR, frame_a_rest, length, &out));
        CHECK(IsSentinel(&out));

        /* The same read into a buffer that still holds a previous report */
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, frame_a_rest, length);
        out = sentinel;
        CHECK(!SDL_GHL_DecodeFrameA(stale, length, &out));
        CHECK(IsSentinel(&out));
    }
    /* 26 bytes read where byte 26 held a previous value */
    memcpy(stale, frame_a_rest, 27);
    stale[26] = 0x55;
    out = sentinel;
    CHECK(!SDL_GHL_DecodeFrameA(stale, 26, &out));
    CHECK(IsSentinel(&out));
    /* Past the metadata's 32 bytes */
    for (length = SDL_GHL_FRAME_A_MAX_LENGTH + 1; length <= sizeof(stale); ++length) {
        memset(stale, 0, sizeof(stale));
        memcpy(stale, frame_a_rest, 27);
        out = sentinel;
        CHECK(!SDL_GHL_DecodeFrameA(stale, length, &out));
        CHECK(IsSentinel(&out));
    }

    FrameBRest(report);
    report[5] = 0x18;
    for (length = 0; length < SDL_GHL_FRAME_B_LENGTH; ++length) {
        connected = false;
        CHECK(!DecodeB(report, length, &out, &connected));
        CHECK(IsSentinel(&out));
        CHECK(!connected);

        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, report, length);
        out = sentinel;
        CHECK(!SDL_GHL_DecodeFrameB(stale, length, &out, &connected));
        CHECK(IsSentinel(&out));
    }

    /* Null arguments */
    out = sentinel;
    CHECK(!SDL_GHL_DecodeFrameA(NULL, 27, &out));
    CHECK(!SDL_GHL_DecodeFrameA(frame_a_rest, 27, NULL));
    CHECK(!SDL_GHL_DecodeFrameB(NULL, 64, &out, &connected));
    FrameBRest(report);
    CHECK(!SDL_GHL_DecodeFrameB(report, 64, NULL, &connected));
    CHECK(!SDL_GHL_DecodeFrameB(report, 64, &out, NULL));
    CHECK(IsSentinel(&out));
    CHECK(!SDL_GHL_DecodeXboxMessage(SDL_GHL_XBOX_MESSAGE_GUITAR, NULL, 27, &out));
    CHECK(IsSentinel(&out));
}

/* ------------------------------------------------------------------------ */
/* Keep-alive, tests 11 and 13 */

typedef struct
{
    SDL_GHLKeepAlive keepalive;
    int dongle;
    int sends;
    int attempts;
    uint64_t last_send;
    bool fail;
    uint8_t last[SDL_GHL_HID_KEEPALIVE_LENGTH];
    size_t last_length;
} SimDriver;

/* What a driver does on each update: send one when due, through
   hid_send_output_report on HID and as raw output report 0x22 on Xbox One. */
static void SimUpdate(SimDriver *sim, uint64_t now)
{
    if (!SDL_GHL_KeepAliveDue(&sim->keepalive, now)) {
        return;
    }
    ++sim->attempts;
    if (sim->dongle == SDL_GHL_DONGLE_XBOXONE) {
        sim->last_length = SDL_GHL_BuildXboxKeepAlive(sim->last);
    } else {
        sim->last_length = SDL_GHL_BuildHIDKeepAlive(sim->dongle, sim->last);
    }
    if (!sim->fail) {
        ++sim->sends;
        sim->last_send = now;
    }
    SDL_GHL_KeepAliveSent(&sim->keepalive, now, !sim->fail);
}

static void SimOpen(SimDriver *sim, int dongle, uint64_t now)
{
    memset(sim, 0, sizeof(*sim));
    sim->dongle = dongle;
    SDL_GHL_KeepAliveStart(&sim->keepalive, now);
    SimUpdate(sim, now);
}

static void TestKeepAlive(void)
{
    static const uint8_t ps3[9] = { 0x00, 0x02, 0x08, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t ps4[9] = { 0x30, 0x02, 0x08, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t xbox[8] = { 0x02, 0x08, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t buffer[SDL_GHL_HID_KEEPALIVE_LENGTH];
    SimDriver sim;
    int dongle;
    uint64_t t;

    /* The bytes */
    CHECK(SDL_GHL_BuildHIDKeepAlive(SDL_GHL_DONGLE_PS3, buffer) == 9 && memcmp(buffer, ps3, 9) == 0);
    CHECK(SDL_GHL_BuildHIDKeepAlive(SDL_GHL_DONGLE_PS4, buffer) == 9 && memcmp(buffer, ps4, 9) == 0);
    CHECK(SDL_GHL_BuildHIDKeepAlive(SDL_GHL_DONGLE_XBOXONE, buffer) == 0);
    CHECK(SDL_GHL_BuildHIDKeepAlive(SDL_GHL_DONGLE_NONE, buffer) == 0);
    CHECK(SDL_GHL_BuildXboxKeepAlive(buffer) == 8 && memcmp(buffer, xbox, 8) == 0);
    CHECK(SDL_GHL_XBOX_MESSAGE_OUTPUT == 0x22);
    CHECK(SDL_GHL_BuildHIDKeepAlive(SDL_GHL_DONGLE_PS3, NULL) == 0);
    CHECK(SDL_GHL_BuildXboxKeepAlive(NULL) == 0);

    /* 11. The schedule, per dongle */
    for (dongle = SDL_GHL_DONGLE_PS3; dongle <= SDL_GHL_DONGLE_XBOXONE; ++dongle) {
        SimOpen(&sim, dongle, 0);
        CHECK(sim.sends == 1 && sim.last_send == 0);
        if (dongle == SDL_GHL_DONGLE_PS3) {
            CHECK(sim.last_length == 9 && memcmp(sim.last, ps3, 9) == 0);
        } else if (dongle == SDL_GHL_DONGLE_PS4) {
            CHECK(sim.last_length == 9 && memcmp(sim.last, ps4, 9) == 0);
        } else {
            CHECK(sim.last_length == 8 && memcmp(sim.last, xbox, 8) == 0);
        }
        for (t = 1; t < 8000; t += 7) {
            SimUpdate(&sim, t);
        }
        SimUpdate(&sim, 7999);
        CHECK(sim.sends == 1);
        SimUpdate(&sim, 8000);
        CHECK(sim.sends == 2 && sim.last_send == 8000);
        for (t = 8001; t < 16000; t += 13) {
            SimUpdate(&sim, t);
        }
        CHECK(sim.sends == 2);
        SimUpdate(&sim, 16000);
        CHECK(sim.sends == 3 && sim.last_send == 16000);
        /* A clock jump sends exactly one */
        SimUpdate(&sim, 60000);
        SimUpdate(&sim, 60000);
        SimUpdate(&sim, 60001);
        CHECK(sim.sends == 4 && sim.last_send == 60000);
        CHECK(sim.keepalive.next_due_ms == 68000);
        SimUpdate(&sim, 67999);
        CHECK(sim.sends == 4);
        SimUpdate(&sim, 68000);
        CHECK(sim.sends == 5);

        /* A failed send is retried on the next update */
        sim.fail = true;
        SimUpdate(&sim, 76000);
        CHECK(sim.sends == 5 && sim.attempts == 6);
        SimUpdate(&sim, 76001);
        CHECK(sim.attempts == 7);
        sim.fail = false;
        SimUpdate(&sim, 76002);
        CHECK(sim.sends == 6 && sim.last_send == 76002);
        CHECK(sim.keepalive.next_due_ms == 84002);
        SimUpdate(&sim, 84001);
        CHECK(sim.sends == 6);
        SimUpdate(&sim, 84002);
        CHECK(sim.sends == 7);

        /* 13. Closing stops it, and reopening restarts at the reopen time */
        SDL_GHL_KeepAliveStop(&sim.keepalive);
        SimUpdate(&sim, 200000);
        CHECK(sim.sends == 7);
        CHECK(!SDL_GHL_KeepAliveDue(&sim.keepalive, 1000000));
        SimOpen(&sim, dongle, 300000);
        CHECK(sim.sends == 1 && sim.last_send == 300000);
        SimUpdate(&sim, 307999);
        CHECK(sim.sends == 1);
        SimUpdate(&sim, 308000);
        CHECK(sim.sends == 2);
    }

    /* A schedule never started is never due */
    {
        SDL_GHLKeepAlive idle;
        memset(&idle, 0, sizeof(idle));
        CHECK(!SDL_GHL_KeepAliveDue(&idle, 0));
        CHECK(!SDL_GHL_KeepAliveDue(&idle, 1000000));
    }
}

/* ------------------------------------------------------------------------ */
/* GameInput raw types, test 12 */

static void TestGameInputRawType(void)
{
    CHECK(SDL_GameInputRawTypeForDevice(0x1430, 0x079B) == SDL_GAMEINPUT_RAWTYPE_GUITAR_HERO_LIVE_GUITAR);
    CHECK(SDL_GameInputRawTypeForDevice(0x1430, 0x0170) == SDL_GAMEINPUT_RAWTYPE_NONE);
    CHECK(SDL_GameInputRawTypeForDevice(0x0E6F, 0x0170) == SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_GUITAR);

    /* The rest of the table is unchanged */
    CHECK(SDL_GameInputRawTypeForDevice(0x0E6F, 0x0248) == SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_GUITAR);
    CHECK(SDL_GameInputRawTypeForDevice(0x0E6F, 0x0171) == SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_DRUM_KIT);
    CHECK(SDL_GameInputRawTypeForDevice(USB_VENDOR_MADCATZ, USB_PRODUCT_MADCATZ_XB1_STRATOCASTER_GUITAR) == SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_GUITAR);
    CHECK(SDL_GameInputRawTypeForDevice(USB_VENDOR_MADCATZ, USB_PRODUCT_MADCATZ_XB1_DRUM_KIT) == SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_DRUM_KIT);
    CHECK(SDL_GameInputRawTypeForDevice(USB_VENDOR_MADCATZ, USB_PRODUCT_MADCATZ_XB1_LEGACY_ADAPTER) == SDL_GAMEINPUT_RAWTYPE_LEGACY_ADAPTER);
    CHECK(SDL_GameInputRawTypeForDevice(USB_VENDOR_CRKD, USB_PRODUCT_RED_OCTANE_XB1_STAGE_TOUR_GUITAR) == SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_GUITAR);
    CHECK(SDL_GameInputRawTypeForDevice(USB_VENDOR_RED_OCTANE_GAMES, USB_PRODUCT_RED_OCTANE_XB1_STAGE_TOUR_GUITAR) == SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_GUITAR);
    CHECK(SDL_GameInputRawTypeForDevice(USB_VENDOR_RED_OCTANE_GAMES, USB_PRODUCT_RED_OCTANE_XB1_STAGE_TOUR_DRUMS) == SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_DRUM_KIT);

    /* Nothing else is a raw type */
    CHECK(SDL_GameInputRawTypeForDevice(0x1430, 0x07BB) == SDL_GAMEINPUT_RAWTYPE_NONE);
    CHECK(SDL_GameInputRawTypeForDevice(0x1430, 0x070B) == SDL_GAMEINPUT_RAWTYPE_NONE);
    CHECK(SDL_GameInputRawTypeForDevice(0x12BA, 0x074B) == SDL_GAMEINPUT_RAWTYPE_NONE);
    CHECK(SDL_GameInputRawTypeForDevice(0x045E, 0x02EA) == SDL_GAMEINPUT_RAWTYPE_NONE);
    CHECK(SDL_GameInputRawTypeForDevice(0x0000, 0x0000) == SDL_GAMEINPUT_RAWTYPE_NONE);
}

int main(void)
{
    TestIdentity();
    TestFrameA();
    TestFrameB();
    TestXbox();
    TestTruncations();
    TestKeepAlive();
    TestGameInputRawType();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
