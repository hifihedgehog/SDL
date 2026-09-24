/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_ps3ext_proto.c: the uDraw
   GameTablet, the Top Shot light guns and the Tony Hawk skateboards of
   hifihedgehog/SDL#33 Part 3. The numbers follow the part's replay tests for
   each device. Every report is fed as an exact-size heap copy, so
   AddressSanitizer catches a read past its length, and truncated inside a
   buffer of stale bytes. */

#include "../src/joystick/hidapi/SDL_hidapi_ps3ext_proto.h"

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

/* ------------------------------------------------------------------------ */
/* Helpers */

static bool Decode(int variant, const uint8_t *report, size_t length, SDL_PS3ExtOutput *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    memset(out, 0xA5, sizeof(*out));
    result = SDL_PS3Ext_Decode(variant, copy, length, out);
    free(copy);
    return result;
}

static bool Untouched(const SDL_PS3ExtOutput *out)
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

static bool Down(const SDL_PS3ExtOutput *o, int button)
{
    return ((o->buttons >> button) & 1) != 0;
}

static bool Only(const SDL_PS3ExtOutput *o, int button)
{
    return o->buttons == (1u << button);
}

static bool Posted(const SDL_PS3ExtOutput *o, int axis)
{
    return ((o->axis_mask >> axis) & 1) != 0;
}

static bool IsData(const SDL_PS3ExtOutput *o, int axis)
{
    return ((o->data_axis_mask >> axis) & 1) != 0;
}

/* Field by field: padding is not compared */
static bool Same(const SDL_PS3ExtOutput *a, const SDL_PS3ExtOutput *b)
{
    int i;

    if (a->buttons != b->buttons || a->button_mask != b->button_mask ||
        a->axis_mask != b->axis_mask || a->data_axis_mask != b->data_axis_mask ||
        a->hat != b->hat || a->has_accel != b->has_accel) {
        return false;
    }
    for (i = 0; i < SDL_PS3EXT_MAX_AXES; ++i) {
        if (((a->axis_mask >> i) & 1) && a->axes[i] != b->axes[i]) {
            return false;
        }
    }
    if (a->has_accel) {
        for (i = 0; i < 3; ++i) {
            if (a->accel[i] != b->accel[i]) {
                return false;
            }
        }
    }
    return true;
}

static const uint8_t clockwise[8] = {
    SDL_PS3EXT_HAT_UP,
    SDL_PS3EXT_HAT_UP | SDL_PS3EXT_HAT_RIGHT,
    SDL_PS3EXT_HAT_RIGHT,
    SDL_PS3EXT_HAT_DOWN | SDL_PS3EXT_HAT_RIGHT,
    SDL_PS3EXT_HAT_DOWN,
    SDL_PS3EXT_HAT_DOWN | SDL_PS3EXT_HAT_LEFT,
    SDL_PS3EXT_HAT_LEFT,
    SDL_PS3EXT_HAT_UP | SDL_PS3EXT_HAT_LEFT
};

/* The common buttons: byte 0 square, cross, circle, triangle, byte 1 select,
   start, PS */
static void CheckCommonButtons(int variant, const uint8_t *base)
{
    static const struct
    {
        int byte;
        uint8_t value;
        int button;
    } common[] = {
        { 0, 0x01, SDL_PS3EXT_BUTTON_WEST },
        { 0, 0x02, SDL_PS3EXT_BUTTON_SOUTH },
        { 0, 0x04, SDL_PS3EXT_BUTTON_EAST },
        { 0, 0x08, SDL_PS3EXT_BUTTON_NORTH },
        { 1, 0x01, SDL_PS3EXT_BUTTON_BACK },
        { 1, 0x02, SDL_PS3EXT_BUTTON_START },
        { 1, 0x10, SDL_PS3EXT_BUTTON_GUIDE },
    };
    uint8_t report[27];
    SDL_PS3ExtOutput out;
    int i;

    for (i = 0; i < (int)(sizeof(common) / sizeof(common[0])); ++i) {
        memcpy(report, base, sizeof(report));
        report[common[i].byte] = common[i].value;
        CHECK(Decode(variant, report, 27, &out));
        CHECK(Only(&out, common[i].button));
    }
    /* Every d-pad value */
    for (i = 0; i < 256; ++i) {
        memcpy(report, base, sizeof(report));
        report[2] = (uint8_t)i;
        CHECK(Decode(variant, report, 27, &out));
        CHECK(out.hat == (i < 8 ? clockwise[i] : SDL_PS3EXT_HAT_CENTERED));
        CHECK(out.buttons == 0);
    }
}

/* Every truncation from 0 to 26 bytes changes nothing, as an exact-size copy
   and inside a buffer of stale bytes */
static void CheckTruncations(int variant, const uint8_t *base)
{
    uint8_t stale[40];
    SDL_PS3ExtOutput out;
    size_t length;

    for (length = 0; length < 27; ++length) {
        CHECK(!Decode(variant, base, length, &out));
        CHECK(Untouched(&out));
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, base, length);
        memset(&out, 0xA5, sizeof(out));
        CHECK(!SDL_PS3Ext_Decode(variant, stale, length, &out));
        CHECK(Untouched(&out));
    }
}

/* ------------------------------------------------------------------------ */
/* Identity */

static void TestIdentity(void)
{
    SDL_PS3ExtDevice device;
    SDL_PS3ExtLayout layout;

    CHECK(SDL_PS3Ext_GetDevice(0x20D6, 0xCB17, &device) && device.variant == SDL_PS3EXT_UDRAW);
    CHECK(strcmp(device.name, "THQ uDraw Game Tablet for PS3") == 0);
    CHECK(SDL_PS3Ext_GetDevice(0x12BA, 0x04A0, &device) && device.variant == SDL_PS3EXT_TOPSHOT_ELITE);
    CHECK(strcmp(device.name, "Top Shot Elite") == 0);
    CHECK(SDL_PS3Ext_GetDevice(0x12BA, 0x04A1, &device) && device.variant == SDL_PS3EXT_TOPSHOT_FEARMASTER);
    CHECK(strcmp(device.name, "Top Shot Fearmaster") == 0);
    CHECK(SDL_PS3Ext_GetDevice(0x12BA, 0x0400, &device) && device.variant == SDL_PS3EXT_TONYHAWK);
    CHECK(strcmp(device.name, "Tony Hawk RIDE Skateboard") == 0);
    CHECK(SDL_PS3Ext_GetDevice(0x1430, 0x0100, &device) && device.variant == SDL_PS3EXT_TONYHAWK);
    CHECK(strcmp(device.name, "Tony Hawk Skateboard") == 0);
    CHECK(SDL_PS3Ext_GetDevice(0x12BA, 0x0400, NULL));
    /* 12BA:0100 is the PS3 Guitar Hero guitar, not a skateboard */
    CHECK(!SDL_PS3Ext_GetDevice(0x12BA, 0x0100, &device));
    CHECK(!SDL_PS3Ext_GetDevice(0x1430, 0x0400, &device));
    CHECK(!SDL_PS3Ext_GetDevice(0x20D6, 0xCB18, &device));
    CHECK(!SDL_PS3Ext_GetDevice(0x12BA, 0x074B, &device));

    CHECK(SDL_PS3Ext_GetLayout(SDL_PS3EXT_UDRAW, &layout));
    CHECK(layout.nbuttons == 14 && layout.naxes == 4 && layout.nhats == 1 && layout.accelerometer);
    CHECK(strcmp(layout.mapping, "a:b0,b:b1,back:b4,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b5,start:b6,x:b2,y:b3,") == 0);
    CHECK(SDL_PS3Ext_GetLayout(SDL_PS3EXT_TOPSHOT_ELITE, &layout));
    CHECK(layout.nbuttons == 16 && layout.naxes == 8 && layout.nhats == 1 && !layout.accelerometer);
    CHECK(strcmp(layout.mapping, "a:b0,b:b1,back:b4,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b5,start:b6,x:b2,y:b3,"
                                 "leftstick:b7,leftx:a0,lefty:a1,rightstick:b8,rightx:a2,righty:a3,") == 0);
    CHECK(SDL_PS3Ext_GetLayout(SDL_PS3EXT_TOPSHOT_FEARMASTER, &layout));
    CHECK(layout.nbuttons == 16 && layout.naxes == 7 && layout.nhats == 1 && !layout.accelerometer);
    CHECK(strcmp(layout.mapping, "a:b0,b:b1,back:b4,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b5,start:b6,x:b2,y:b3,"
                                 "leftstick:b7,leftx:a0,lefty:a1,") == 0);
    CHECK(SDL_PS3Ext_GetLayout(SDL_PS3EXT_TONYHAWK, &layout));
    CHECK(layout.nbuttons == 7 && layout.naxes == 6 && layout.nhats == 1 && !layout.accelerometer);
    CHECK(strcmp(layout.mapping, "a:b0,b:b1,back:b4,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b5,start:b6,x:b2,y:b3,") == 0);
    CHECK(!SDL_PS3Ext_GetLayout(SDL_PS3EXT_NONE, &layout));
    CHECK(!SDL_PS3Ext_GetLayout(SDL_PS3EXT_UDRAW, NULL));
}

/* ------------------------------------------------------------------------ */
/* uDraw, tests 1 to 9 */

/* Test 1's idle report, from brandonw.net and Linux */
static const uint8_t udraw_idle[27] = {
    0x00, 0x00, 0x0F, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60,
    0x00, 0x0F, 0x0F, 0xFF, 0xFF, 0x00, 0x02, 0x00, 0x02, 0xEC, 0x01, 0x00, 0x02
};

static int UDrawX(int x)
{
    return (x * 65535) / 1919 - 32768;
}

static int UDrawY(int y)
{
    return (y * 65535) / 1079 - 32768;
}

static int UDrawPressure(int level)
{
    return (level * 32767) / 142;
}

static void TestUDraw(void)
{
    uint8_t report[40];
    SDL_PS3ExtOutput out, idle;
    int i;

    /* 1. Idle */
    CHECK(Decode(SDL_PS3EXT_UDRAW, udraw_idle, 27, &idle));
    CHECK(idle.buttons == 0);
    CHECK(idle.button_mask == 0x3FFF);
    CHECK(idle.hat == SDL_PS3EXT_HAT_CENTERED);
    CHECK(!Posted(&idle, SDL_PS3EXT_UDRAW_AXIS_X) && !Posted(&idle, SDL_PS3EXT_UDRAW_AXIS_Y));
    CHECK(Posted(&idle, SDL_PS3EXT_UDRAW_AXIS_PRESSURE) && idle.axes[SDL_PS3EXT_UDRAW_AXIS_PRESSURE] == 0);
    CHECK(Posted(&idle, SDL_PS3EXT_UDRAW_AXIS_DISTANCE) && idle.axes[SDL_PS3EXT_UDRAW_AXIS_DISTANCE] == 0);
    CHECK(idle.has_accel);
    CHECK(idle.accel[0] == 0 && idle.accel[1] == 0 && idle.accel[2] == 0x1EC - 0x200);

    /* 2. Pen at the origin */
    memcpy(report, udraw_idle, 27);
    report[11] = 0x40;
    report[13] = 0x74;
    report[15] = 0x00;
    report[16] = 0x00;
    report[17] = 0x00;
    report[18] = 0x00;
    CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out));
    CHECK(Only(&out, SDL_PS3EXT_UDRAW_PEN));
    CHECK(Posted(&out, SDL_PS3EXT_UDRAW_AXIS_X) && out.axes[SDL_PS3EXT_UDRAW_AXIS_X] == AXIS_MIN);
    CHECK(Posted(&out, SDL_PS3EXT_UDRAW_AXIS_Y) && out.axes[SDL_PS3EXT_UDRAW_AXIS_Y] == AXIS_MIN);
    CHECK(out.axes[SDL_PS3EXT_UDRAW_AXIS_PRESSURE] == UDrawPressure(3));
    for (i = 0; i < 4; ++i) {
        CHECK(IsData(&out, i));
    }

    /* 3. Pen at the far corner */
    report[15] = 0x07;
    report[16] = 0x04;
    report[17] = 0x7F;
    report[18] = 0x37;
    CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out));
    CHECK(out.axes[SDL_PS3EXT_UDRAW_AXIS_X] == AXIS_MAX);
    CHECK(out.axes[SDL_PS3EXT_UDRAW_AXIS_Y] == AXIS_MAX);
    /* Every cell and a range of offsets on both axes */
    for (i = 0; i < 8 * 256; i += 7) {
        const int cell = i / 256, offset = i % 256;
        const int x = (cell * 256 + offset > 1919) ? 1919 : cell * 256 + offset;
        report[15] = (uint8_t)cell;
        report[17] = (uint8_t)offset;
        report[16] = (uint8_t)(cell % 5);
        report[18] = (uint8_t)offset;
        CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out));
        CHECK(out.axes[SDL_PS3EXT_UDRAW_AXIS_X] == UDrawX(x));
        {
            const int y = ((cell % 5) * 256 + offset > 1079) ? 1079 : (cell % 5) * 256 + offset;
            CHECK(out.axes[SDL_PS3EXT_UDRAW_AXIS_Y] == UDrawY(y));
        }
    }

    /* 4. Pressure */
    memcpy(report, udraw_idle, 27);
    report[11] = 0x40;
    report[15] = report[16] = 0x01;
    report[17] = report[18] = 0x10;
    report[13] = 0xFF;
    CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out) && out.axes[SDL_PS3EXT_UDRAW_AXIS_PRESSURE] == AXIS_MAX);
    report[13] = 0x71;
    CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out) && out.axes[SDL_PS3EXT_UDRAW_AXIS_PRESSURE] == 0);
    report[13] = 0x60;
    CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out) && out.axes[SDL_PS3EXT_UDRAW_AXIS_PRESSURE] == 0);
    for (i = 0; i < 256; ++i) {
        const int level = (i - 113 < 0) ? 0 : i - 113;
        report[13] = (uint8_t)i;
        CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out));
        CHECK(out.axes[SDL_PS3EXT_UDRAW_AXIS_PRESSURE] == UDrawPressure(level));
    }

    /* 5. A finger, and two */
    memcpy(report, udraw_idle, 27);
    report[11] = 0x80;
    for (i = 0; i < 256; i += 17) {
        report[13] = (uint8_t)i;
        CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out));
        CHECK(Only(&out, SDL_PS3EXT_UDRAW_FINGER));
        CHECK(out.axes[SDL_PS3EXT_UDRAW_AXIS_PRESSURE] == 0);
        CHECK(out.axes[SDL_PS3EXT_UDRAW_AXIS_DISTANCE] == 0);
    }
    memcpy(report, udraw_idle, 27);
    report[11] = 0xC0;
    report[12] = 0x20;
    CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out));
    CHECK(Only(&out, SDL_PS3EXT_UDRAW_TWO_FINGERS));
    CHECK(out.axes[SDL_PS3EXT_UDRAW_AXIS_DISTANCE] == (0x20 * 32767) / 255);
    CHECK(out.axes[SDL_PS3EXT_UDRAW_AXIS_PRESSURE] == 0);
    /* Any touch value other than 0x00, 0x40 and 0x80 is two fingers */
    for (i = 1; i < 256; ++i) {
        if (i == 0x40 || i == 0x80) {
            continue;
        }
        report[11] = (uint8_t)i;
        CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out));
        CHECK(Only(&out, SDL_PS3EXT_UDRAW_TWO_FINGERS));
    }
    /* The distance reads 0 without two fingers */
    memcpy(report, udraw_idle, 27);
    report[12] = 0x55;
    CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out) && Same(&out, &idle));

    /* 6. Cell bytes 0x0F leave the axis alone */
    memcpy(report, udraw_idle, 27);
    report[11] = 0x40;
    report[13] = 0x80;
    CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out));
    CHECK(!Posted(&out, SDL_PS3EXT_UDRAW_AXIS_X) && !Posted(&out, SDL_PS3EXT_UDRAW_AXIS_Y));
    report[15] = 0x03;
    report[17] = 0x21;
    CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out));
    CHECK(Posted(&out, SDL_PS3EXT_UDRAW_AXIS_X) && !Posted(&out, SDL_PS3EXT_UDRAW_AXIS_Y));
    CHECK(out.axes[SDL_PS3EXT_UDRAW_AXIS_X] == UDrawX(3 * 256 + 0x21));
    /* Nothing touching: no position, even with cells filled in */
    memcpy(report, udraw_idle, 27);
    report[15] = 0x02;
    report[16] = 0x02;
    report[17] = 0x00;
    report[18] = 0x00;
    CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out));
    CHECK(!Posted(&out, SDL_PS3EXT_UDRAW_AXIS_X) && !Posted(&out, SDL_PS3EXT_UDRAW_AXIS_Y));

    /* 7 and 8. Buttons and the d-pad */
    CheckCommonButtons(SDL_PS3EXT_UDRAW, udraw_idle);
    {
        static const uint8_t nothing[] = { 0x10, 0x20, 0x40, 0x80 };
        for (i = 0; i < (int)sizeof(nothing); ++i) {
            memcpy(report, udraw_idle, 27);
            report[0] = nothing[i];
            CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out));
            CHECK(Same(&out, &idle));
        }
    }

    /* The accelerometer words */
    memcpy(report, udraw_idle, 27);
    report[19] = 0x15;
    report[20] = 0x02;
    report[21] = 0xE6;
    report[22] = 0x01;
    report[23] = 0x17;
    report[24] = 0x02;
    CHECK(Decode(SDL_PS3EXT_UDRAW, report, 27, &out));
    CHECK(out.accel[0] == 0x15 && out.accel[1] == 0x1E6 - 0x200 && out.accel[2] == 0x17);
    CHECK(out.buttons == 0);

    /* 9. Only 27 bytes, as Linux requires */
    CheckTruncations(SDL_PS3EXT_UDRAW, udraw_idle);
    memset(report, 0, sizeof(report));
    memcpy(report, udraw_idle, 27);
    for (i = 28; i <= 40; ++i) {
        CHECK(!Decode(SDL_PS3EXT_UDRAW, report, (size_t)i, &out));
        CHECK(Untouched(&out));
    }
}

/* ------------------------------------------------------------------------ */
/* Top Shot, tests 1 to 7 */

/* Test 1: the mode off */
static const uint8_t topshot_off[27] = {
    0x00, 0x00, 0x0F, 0x7F, 0x7F, 0x7F, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02
};

static int LED(int value)
{
    return (value * 65535) / 1023 - 32768;
}

static void TestTopShot(void)
{
    uint8_t report[40];
    SDL_PS3ExtOutput out, off;
    int variant, i;

    for (variant = SDL_PS3EXT_TOPSHOT_ELITE; variant <= SDL_PS3EXT_TOPSHOT_FEARMASTER; ++variant) {
        const bool elite = (variant == SDL_PS3EXT_TOPSHOT_ELITE);
        const int led = elite ? 4 : 2;

        /* 1. The mode off */
        CHECK(Decode(variant, topshot_off, 27, &off));
        CHECK(off.buttons == 0);
        CHECK(off.button_mask == 0xFFFF);
        CHECK(off.hat == SDL_PS3EXT_HAT_CENTERED);
        CHECK(!Down(&off, SDL_PS3EXT_TOPSHOT_LEFT_SEEN) && !Down(&off, SDL_PS3EXT_TOPSHOT_RIGHT_SEEN));
        CHECK(off.axes[0] == -129 && off.axes[1] == -129);
        CHECK(off.axis_mask == (elite ? 0xFF : 0x7F));
        CHECK(!IsData(&off, 0) && !IsData(&off, 1));
        CHECK(IsData(&off, led) && IsData(&off, led + 3));
        CHECK(!off.has_accel);

        /* 2. The mode on, nothing in view */
        memcpy(report, topshot_off, 27);
        memset(&report[7], 0xFF, 6);
        CHECK(Decode(variant, report, 27, &out));
        for (i = 0; i < 4; ++i) {
            CHECK(out.axes[led + i] == AXIS_MAX);
        }
        CHECK(out.buttons == 0);

        /* 3. Aim */
        memcpy(report, topshot_off, 27);
        report[7] = 0x80;
        report[8] = 0x15;
        report[9] = 0x52;
        report[10] = 0x99;
        report[11] = 0x95;
        report[12] = 0x52;
        CHECK(Decode(variant, report, 27, &out));
        CHECK(out.axes[led + 0] == LED(512));
        CHECK(out.axes[led + 1] == LED(341));
        CHECK(out.axes[led + 2] == LED(614));
        CHECK(out.axes[led + 3] == LED(341));
        CHECK(out.buttons == ((1u << SDL_PS3EXT_TOPSHOT_LEFT_SEEN) | (1u << SDL_PS3EXT_TOPSHOT_RIGHT_SEEN)));
        memcpy(report, topshot_off, 27);
        report[7] = 0x00;
        report[8] = 0xC0;
        CHECK(Decode(variant, report, 27, &out));
        CHECK(out.axes[led + 0] == LED(3));
        /* Every coordinate through an independent packing */
        for (i = 0; i < 1024; ++i) {
            const int x = i, y = 1023 - i;
            memcpy(report, topshot_off, 27);
            report[7] = (uint8_t)(x >> 2);
            report[8] = (uint8_t)(((x & 0x03) << 6) | (y >> 4));
            report[9] = (uint8_t)(((y & 0x0F) << 4) | 0x2);
            report[10] = (uint8_t)(y >> 2);
            report[11] = (uint8_t)(((y & 0x03) << 6) | (x >> 4));
            report[12] = (uint8_t)(((x & 0x0F) << 4) | 0xF);
            CHECK(Decode(variant, report, 27, &out));
            CHECK(out.axes[led + 0] == LED(x) && out.axes[led + 1] == LED(y));
            CHECK(out.axes[led + 2] == LED(y) && out.axes[led + 3] == LED(x));
            CHECK(Down(&out, SDL_PS3EXT_TOPSHOT_LEFT_SEEN) && !Down(&out, SDL_PS3EXT_TOPSHOT_RIGHT_SEEN));
        }

        /* 4. Buttons */
        CheckCommonButtons(variant, topshot_off);
        {
            static const struct
            {
                int byte;
                uint8_t value;
            } extra[] = { { 0, 0x10 }, { 0, 0x20 }, { 1, 0x04 }, { 1, 0x08 } };
            const int elite_buttons[4] = {
                SDL_PS3EXT_TOPSHOT_RELOAD, SDL_PS3EXT_TOPSHOT_TRIGGER,
                SDL_PS3EXT_BUTTON_LEFT_STICK, SDL_PS3EXT_BUTTON_RIGHT_STICK
            };
            const int fearmaster_buttons[4] = {
                -1, SDL_PS3EXT_TOPSHOT_TRIGGER,
                SDL_PS3EXT_BUTTON_LEFT_STICK, SDL_PS3EXT_TOPSHOT_HEART_TOUCH
            };
            for (i = 0; i < 4; ++i) {
                const int expected = elite ? elite_buttons[i] : fearmaster_buttons[i];
                memcpy(report, topshot_off, 27);
                report[extra[i].byte] = extra[i].value;
                CHECK(Decode(variant, report, 27, &out));
                if (expected < 0) {
                    CHECK(Same(&out, &off));
                } else {
                    CHECK(Only(&out, expected));
                }
            }
            memcpy(report, topshot_off, 27);
            report[0] = 0xC0;
            report[1] = 0xE0;
            CHECK(Decode(variant, report, 27, &out));
            CHECK(Same(&out, &off));
        }

        /* 5. Bytes 5 and 6 */
        memcpy(report, topshot_off, 27);
        report[5] = 0xE0;
        report[6] = 0x03;
        CHECK(Decode(variant, report, 27, &out));
        if (elite) {
            CHECK(out.axes[2] == 0xE0 * 257 - 32768);
            CHECK(out.axes[3] == 0x03 * 257 - 32768);
        } else {
            CHECK(out.axes[6] == 992);
            CHECK(IsData(&out, 6));
            CHECK(out.axes[2] == LED(0));
        }
        report[5] = 0xFF;
        report[6] = 0xFF;
        CHECK(Decode(variant, report, 27, &out));
        if (!elite) {
            CHECK(out.axes[6] == AXIS_MAX);
        }
        /* The left stick */
        for (i = 0; i < 256; ++i) {
            memcpy(report, topshot_off, 27);
            report[3] = (uint8_t)i;
            report[4] = (uint8_t)(255 - i);
            CHECK(Decode(variant, report, 27, &out));
            CHECK(out.axes[0] == i * 257 - 32768 && out.axes[1] == (255 - i) * 257 - 32768);
        }

        /* 7. Truncations, and a longer read */
        CheckTruncations(variant, topshot_off);
        memset(report, 0xFF, sizeof(report));
        memcpy(report, topshot_off, 27);
        CHECK(Decode(variant, report, sizeof(report), &out));
        CHECK(Same(&out, &off));
    }
}

/* 6. The request schedule */
typedef struct
{
    SDL_PS3ExtAim aim;
    int outputs;
    int features;
    uint64_t last_time;
    int last_kind;
} SimGun;

static void SimSend(SimGun *sim, int kind, uint64_t now)
{
    if (kind == SDL_PS3EXT_REQUEST_OUTPUT) {
        ++sim->outputs;
    } else if (kind == SDL_PS3EXT_REQUEST_FEATURE) {
        ++sim->features;
    }
    sim->last_time = now;
    sim->last_kind = kind;
    SDL_PS3ExtAim_Sent(&sim->aim, kind, now);
}

/* A report arrives, then the driver checks for a due request */
static void SimReport(SimGun *sim, bool sensors, uint64_t now)
{
    uint8_t report[27];
    int kind;

    memcpy(report, topshot_off, 27);
    if (sensors) {
        memset(&report[7], 0xFF, 6);
    }
    SDL_PS3ExtAim_OnReport(&sim->aim, report, 27, now);
    kind = SDL_PS3ExtAim_Due(&sim->aim, now);
    if (kind != SDL_PS3EXT_REQUEST_NONE) {
        SimSend(sim, kind, now);
    }
}

static void TestTopShotRequest(void)
{
    static const uint8_t request[9] = { 0x00, 0x82, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t buffer[9];
    SimGun sim;
    uint64_t t;

    CHECK(SDL_PS3Ext_BuildTopShotRequest(buffer) == 9 && memcmp(buffer, request, 9) == 0);
    CHECK(SDL_PS3Ext_BuildTopShotRequest(NULL) == 0);

    /* Open at 0 sends the request as an output report */
    memset(&sim, 0, sizeof(sim));
    SimSend(&sim, SDL_PS3ExtAim_Open(&sim.aim, 0), 0);
    CHECK(sim.outputs == 1 && sim.features == 0);
    /* Zero sensor bytes until 1000: the same bytes as a feature report */
    for (t = 10; t < 1000; t += 10) {
        SimReport(&sim, false, t);
    }
    CHECK(sim.outputs == 1 && sim.features == 0);
    SimReport(&sim, false, 1000);
    CHECK(sim.features == 1 && sim.last_time == 1000);
    /* Data after that: nothing more */
    for (t = 1010; t < 5000; t += 10) {
        SimReport(&sim, true, t);
    }
    CHECK(sim.outputs == 1 && sim.features == 1);
    /* Zeros again from 5000: one request at 6000, of the kind that worked */
    for (t = 5000; t < 6000; t += 10) {
        SimReport(&sim, false, t);
    }
    CHECK(sim.outputs == 1 && sim.features == 1);
    SimReport(&sim, false, 6000);
    CHECK(sim.features == 2 && sim.last_time == 6000);
    /* No more than one per 1000 ms after, and the kind alternates while the
       last one brought nothing */
    for (t = 6010; t < 7000; t += 10) {
        SimReport(&sim, false, t);
    }
    CHECK(sim.outputs == 1 && sim.features == 2);
    SimReport(&sim, false, 7000);
    CHECK(sim.outputs == 2 && sim.last_kind == SDL_PS3EXT_REQUEST_OUTPUT);
    for (t = 7010; t < 12000; t += 10) {
        SimReport(&sim, false, t);
    }
    CHECK(sim.outputs + sim.features == 3 + 5);
    CHECK(sim.last_time == 11000);

    /* A gun already in sensor mode: the open request is credited and
       repeated in kind */
    memset(&sim, 0, sizeof(sim));
    SimSend(&sim, SDL_PS3ExtAim_Open(&sim.aim, 0), 0);
    for (t = 10; t < 3000; t += 10) {
        SimReport(&sim, true, t);
    }
    CHECK(sim.outputs == 1 && sim.features == 0);
    for (t = 3000; t <= 4000; t += 10) {
        SimReport(&sim, false, t);
    }
    CHECK(sim.outputs == 2 && sim.features == 0 && sim.last_time == 4000);

    /* Short reports change nothing, and no report at all still retries */
    memset(&sim, 0, sizeof(sim));
    SimSend(&sim, SDL_PS3ExtAim_Open(&sim.aim, 0), 0);
    {
        uint8_t full[27];
        memcpy(full, topshot_off, 27);
        memset(&full[7], 0xFF, 6);
        SDL_PS3ExtAim_OnReport(&sim.aim, full, 26, 500);
        SDL_PS3ExtAim_OnReport(&sim.aim, NULL, 27, 500);
    }
    CHECK(SDL_PS3ExtAim_Due(&sim.aim, 999) == SDL_PS3EXT_REQUEST_NONE);
    CHECK(SDL_PS3ExtAim_Due(&sim.aim, 1000) == SDL_PS3EXT_REQUEST_FEATURE);

    /* The two 1000 ms marks to the millisecond: zeros since 5000, the last
       request at 4500 */
    memset(&sim, 0, sizeof(sim));
    SimSend(&sim, SDL_PS3ExtAim_Open(&sim.aim, 0), 0);
    SimReport(&sim, true, 100);
    SimSend(&sim, SDL_PS3EXT_REQUEST_OUTPUT, 4500);
    SimReport(&sim, true, 4600);
    SimReport(&sim, false, 5000);
    CHECK(SDL_PS3ExtAim_Due(&sim.aim, 5999) == SDL_PS3EXT_REQUEST_NONE);
    CHECK(SDL_PS3ExtAim_Due(&sim.aim, 6000) == SDL_PS3EXT_REQUEST_OUTPUT);
    /* zeros since 5000, the last request at 5600 */
    SimSend(&sim, SDL_PS3EXT_REQUEST_OUTPUT, 5600);
    CHECK(SDL_PS3ExtAim_Due(&sim.aim, 6599) == SDL_PS3EXT_REQUEST_NONE);
    CHECK(SDL_PS3ExtAim_Due(&sim.aim, 6600) == SDL_PS3EXT_REQUEST_FEATURE);

    /* Never opened: never due */
    memset(&sim, 0, sizeof(sim));
    CHECK(SDL_PS3ExtAim_Due(&sim.aim, 100000) == SDL_PS3EXT_REQUEST_NONE);
}

/* ------------------------------------------------------------------------ */
/* Tony Hawk, tests 1 to 9 */

static const uint8_t board_off[27] = {
    0x00, 0x00, 0x0F, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02
};
static const uint8_t board_lost[27] = {
    0x00, 0x00, 0x0F, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static int BoardState(const uint8_t *report, size_t length)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    int result;

    if (length) {
        memcpy(copy, report, length);
    }
    result = SDL_PS3Ext_TonyHawkBoardState(copy, length);
    free(copy);
    return result;
}

static void TestTonyHawk(void)
{
    uint8_t report[40];
    uint8_t live[27];
    SDL_PS3ExtOutput out, rest;
    bool connected = false;
    int i;

    /* 1. The idle states connect nothing, and either removes a live board */
    CHECK(BoardState(board_off, 27) == SDL_PS3EXT_BOARD_IDLE);
    CHECK(BoardState(board_lost, 27) == SDL_PS3EXT_BOARD_IDLE);
    memcpy(live, board_off, 27);
    live[11] = 0x10;
    CHECK(BoardState(live, 27) == SDL_PS3EXT_BOARD_LIVE);
    /* The joystick follows the state */
    {
        const uint8_t *sequence[5] = { board_off, live, board_lost, live, board_off };
        const bool expected[5] = { false, true, false, true, false };
        for (i = 0; i < 5; ++i) {
            const int state = BoardState(sequence[i], 27);
            if (state == SDL_PS3EXT_BOARD_LIVE) {
                connected = true;
            } else if (state == SDL_PS3EXT_BOARD_IDLE) {
                connected = false;
            }
            CHECK(connected == expected[i]);
        }
    }
    /* Any single byte away from an idle state is a live board */
    for (i = 0; i < 27; ++i) {
        memcpy(report, board_off, 27);
        report[i] ^= 0x01;
        CHECK(BoardState(report, 27) == SDL_PS3EXT_BOARD_LIVE);
        memcpy(report, board_lost, 27);
        report[i] ^= 0x01;
        CHECK(BoardState(report, 27) == SDL_PS3EXT_BOARD_LIVE);
    }

    /* 2. Board on */
    CHECK(Decode(SDL_PS3EXT_TONYHAWK, live, 27, &out));
    CHECK(out.buttons == 0 && out.hat == SDL_PS3EXT_HAT_CENTERED);
    CHECK(out.axes[0] == (0x10 * 32767) / 0x38);
    CHECK(out.axes[1] == 0 && out.axes[2] == 0 && out.axes[3] == 0);
    CHECK(out.axis_mask == 0x3F && out.data_axis_mask == 0x3F);
    CHECK(out.button_mask == 0x7F);
    CHECK(!out.has_accel);

    /* 3. The IR sensors one at a time */
    CHECK(Decode(SDL_PS3EXT_TONYHAWK, board_off, 27, &rest));
    for (i = 0; i < 4; ++i) {
        int other;
        memcpy(report, board_off, 27);
        report[11 + i] = 0x38;
        CHECK(Decode(SDL_PS3EXT_TONYHAWK, report, 27, &out));
        for (other = 0; other < 6; ++other) {
            CHECK(out.axes[other] == (other == i ? AXIS_MAX : rest.axes[other]));
        }
        report[11 + i] = 0x00;
        CHECK(Decode(SDL_PS3EXT_TONYHAWK, report, 27, &out));
        CHECK(out.axes[i] == 0);
        /* Past 0x38 reads full scale */
        report[11 + i] = 0x39;
        CHECK(Decode(SDL_PS3EXT_TONYHAWK, report, 27, &out) && out.axes[i] == AXIS_MAX);
    }

    /* 4. The single-axis accelerometers */
    memcpy(report, board_off, 27);
    report[15] = 0x80;
    CHECK(Decode(SDL_PS3EXT_TONYHAWK, report, 27, &out) && out.axes[4] == AXIS_MIN);
    report[15] = 0x7F;
    CHECK(Decode(SDL_PS3EXT_TONYHAWK, report, 27, &out) && out.axes[4] == 32512);
    CHECK(out.axes[5] == 0);
    memcpy(report, board_off, 27);
    report[16] = 0x80;
    CHECK(Decode(SDL_PS3EXT_TONYHAWK, report, 27, &out) && out.axes[5] == AXIS_MIN);
    report[16] = 0x7F;
    CHECK(Decode(SDL_PS3EXT_TONYHAWK, report, 27, &out) && out.axes[5] == 32512);
    CHECK(out.axes[4] == 0);

    /* 5 and 6. Buttons and the d-pad, clockwise with 2 right and 6 left */
    CheckCommonButtons(SDL_PS3EXT_TONYHAWK, board_off);
    memcpy(report, board_off, 27);
    report[2] = 2;
    CHECK(Decode(SDL_PS3EXT_TONYHAWK, report, 27, &out) && out.hat == SDL_PS3EXT_HAT_RIGHT);
    report[2] = 6;
    CHECK(Decode(SDL_PS3EXT_TONYHAWK, report, 27, &out) && out.hat == SDL_PS3EXT_HAT_LEFT);

    /* 7. The encrypted words change nothing */
    for (i = 19; i <= 26; ++i) {
        memcpy(report, live, 27);
        report[i] ^= 0x5A;
        CHECK(Decode(SDL_PS3EXT_TONYHAWK, report, 27, &out));
        {
            SDL_PS3ExtOutput reference;
            CHECK(Decode(SDL_PS3EXT_TONYHAWK, live, 27, &reference));
            CHECK(Same(&out, &reference));
        }
    }

    /* 8. The activation: 8 zero data bytes after report ID 0 */
    {
        uint8_t activation[9];
        memset(activation, 0xEE, sizeof(activation));
        CHECK(SDL_PS3Ext_BuildTonyHawkActivation(activation) == 9);
        for (i = 0; i < 9; ++i) {
            CHECK(activation[i] == 0x00);
        }
        CHECK(SDL_PS3Ext_BuildTonyHawkActivation(NULL) == 0);
    }

    /* 9. Truncations change nothing, and a short read is no state */
    CheckTruncations(SDL_PS3EXT_TONYHAWK, live);
    for (i = 0; i < 27; ++i) {
        CHECK(BoardState(board_off, (size_t)i) == SDL_PS3EXT_BOARD_INVALID);
        CHECK(BoardState(live, (size_t)i) == SDL_PS3EXT_BOARD_INVALID);
    }
    CHECK(SDL_PS3Ext_TonyHawkBoardState(NULL, 27) == SDL_PS3EXT_BOARD_INVALID);
    /* A longer read compares its first 27 bytes */
    memset(report, 0x77, sizeof(report));
    memcpy(report, board_off, 27);
    CHECK(BoardState(report, sizeof(report)) == SDL_PS3EXT_BOARD_IDLE);
}

static void TestArguments(void)
{
    SDL_PS3ExtOutput out;

    CHECK(!Decode(SDL_PS3EXT_NONE, board_off, 27, &out));
    CHECK(Untouched(&out));
    CHECK(!Decode(5, board_off, 27, &out));
    CHECK(!SDL_PS3Ext_Decode(SDL_PS3EXT_TONYHAWK, NULL, 27, &out));
    CHECK(!SDL_PS3Ext_Decode(SDL_PS3EXT_TONYHAWK, board_off, 27, NULL));
}

int main(void)
{
    TestIdentity();
    TestUDraw();
    TestTopShot();
    TestTopShotRequest();
    TestTonyHawk();
    TestArguments();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
