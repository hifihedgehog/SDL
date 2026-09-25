/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/windows/SDL_rfcomm_moga_proto.c, the PowerA
   MOGA in Mode A of hifihedgehog/SDL#33 Part 11. Test numbers follow the
   part's MOGA section. The frames are built from the part's layout, since no
   capture exists. */

#include "testrfcommharness.h"

static const char analog_name[] = "MOGA Pro Power";
static const char digital_name[] = "BD&A Pocket";

/* A report with its checksum, 12 or 14 bytes */
static size_t Report(uint8_t *out, uint8_t length, uint8_t code, uint8_t buttons, uint8_t pad, uint8_t lx, uint8_t ly,
                     uint8_t rx, uint8_t ry, uint8_t lt, uint8_t rt)
{
    uint8_t checksum = 0;
    size_t i;

    memset(out, 0, 14);
    out[0] = 0x7A;
    out[1] = length;
    out[2] = code;
    out[3] = 0x01;
    out[4] = buttons;
    out[5] = pad;
    out[6] = lx;
    out[7] = ly;
    out[8] = rx;
    out[9] = ry;
    if (length == 14) {
        out[10] = lt;
        out[11] = rt;
    }
    out[length - 2] = 0x10;
    for (i = 0; i < (size_t)length - 1; ++i) {
        checksum ^= out[i];
    }
    out[length - 1] = checksum;
    return length;
}

static bool IsCommand(const R_Action *a, const uint8_t *bytes, uint64_t time)
{
    return R_IsSend(a, bytes, SDL_MOGA_COMMAND_LENGTH, time);
}

/* The helpers */
static void TestHelpers(void)
{
    uint8_t command[SDL_MOGA_COMMAND_LENGTH];

    SDL_MOGA_BuildCommand(0x43, 0x01, command);
    CHECK(memcmp(command, "\x5A\x05\x43\x01\x1D", 5) == 0);
    SDL_MOGA_BuildCommand(0x45, 0x01, command);
    CHECK(memcmp(command, "\x5A\x05\x45\x01\x1B", 5) == 0);
    SDL_MOGA_BuildCommand(0x43, 0x03, command);
    CHECK(memcmp(command, "\x5A\x05\x43\x03\x1F", 5) == 0);

    CHECK(SDL_MOGA_IsAnalogName("MOGA 2") && SDL_MOGA_IsAnalogName("Moga Pro 2") && SDL_MOGA_IsAnalogName("moga pro"));
    CHECK(!SDL_MOGA_IsAnalogName("BD&A") && !SDL_MOGA_IsAnalogName("BDA Pocket") && !SDL_MOGA_IsAnalogName("bd&a 1"));

    CHECK(SDL_MOGA_IdForPlayerIndex(0) == 1 && SDL_MOGA_IdForPlayerIndex(1) == 2);
    CHECK(SDL_MOGA_IdForPlayerIndex(2) == 3 && SDL_MOGA_IdForPlayerIndex(3) == 4);
    CHECK(SDL_MOGA_IdForPlayerIndex(-1) == 5 && SDL_MOGA_IdForPlayerIndex(4) == 5 && SDL_MOGA_IdForPlayerIndex(100) == 5);

    /* The module connects by the Serial Port UUID first */
    CHECK(SDL_RFCOMMMogaModule.has_service && SDL_RFCOMMMogaModule.family == SDL_RFCOMM_FAMILY_MOGA);
    CHECK(memcmp(SDL_RFCOMMMogaModule.service, "\x00\x00\x11\x01\x00\x00\x10\x00\x80\x00\x00\x80\x5F\x9B\x34\xFB", 16) == 0);
}

/* 1: start-up */
static void TestStartup(void)
{
    RHarness *r = R_Create(&SDL_RFCOMMMogaModule, analog_name);

    CHECK(R_IsConnect(R_Next(r), 0, 0));
    CHECK(R_Next(r) == NULL);
    R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x43\x01\x1D", 0));
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x45\x01\x1B", 0));
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x46\x01\x18", 0));
    CHECK(R_Next(r) == NULL);
    /* No joystick before a report */
    CHECK(r->nsnapshots == 0);
    R_Destroy(r);

    r = R_Create(&SDL_RFCOMMMogaModule, digital_name);
    R_Skip(r);
    R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x43\x01\x1D", 0));
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x41\x01\x1F", 0));
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x44\x01\x1A", 0));
    CHECK(R_Next(r) == NULL);
    R_Destroy(r);

    /* Uppercase or not, "HID" anywhere is Mode B and no MOGA of this path */
    CHECK(SDL_RFCOMM_MatchName("MOGA Pro HID") == SDL_RFCOMM_FAMILY_NONE);
    CHECK(SDL_RFCOMM_MatchName("Moga 2 hid") == SDL_RFCOMM_FAMILY_NONE);
}

/* 2, 3 and 4: the part's frames, and the joystick they make */
static void TestFrames(void)
{
    RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);
    const SDL_SerialSnapshot *last;

    R_Receive(r, (const uint8_t *)"\x7A\x0E\x66\x01\x00\x00\x00\x00\x00\x00\x00\x00\x10\x03", 14);
    CHECK(r->nsnapshots == 1 && R_Present(r));
    last = R_Last(r);
    CHECK(strcmp(last->identity.name, analog_name) == 0 && last->identity.type == SDL_SERIAL_TYPE_GAMEPAD);
    CHECK(last->identity.naxes == SDL_MOGA_AXES && last->identity.nbuttons == SDL_MOGA_BUTTONS && last->identity.nhats == 0);
    CHECK(R_Buttons(r) == 0);
    CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_X) == 0 && R_Axis(r, SDL_MOGA_AXIS_LEFT_Y) == 0);
    CHECK(R_Axis(r, SDL_MOGA_AXIS_RIGHT_X) == 0 && R_Axis(r, SDL_MOGA_AXIS_RIGHT_Y) == 0);
    /* The triggers rest at the bottom from the first snapshot on */
    CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_TRIGGER) == -32768 && R_Axis(r, SDL_MOGA_AXIS_RIGHT_TRIGGER) == -32768);

    /* The mapping */
    CHECK(last->identity.has_mapping);
    CHECK(last->identity.mapping.a.kind == SDL_SERIAL_MAP_BUTTON && last->identity.mapping.a.target == SDL_MOGA_BUTTON_A);
    CHECK(last->identity.mapping.b.target == SDL_MOGA_BUTTON_B && last->identity.mapping.x.target == SDL_MOGA_BUTTON_X);
    CHECK(last->identity.mapping.y.target == SDL_MOGA_BUTTON_Y && last->identity.mapping.back.target == SDL_MOGA_BUTTON_SELECT);
    CHECK(last->identity.mapping.start.target == SDL_MOGA_BUTTON_START && last->identity.mapping.leftstick.target == SDL_MOGA_BUTTON_L3);
    CHECK(last->identity.mapping.rightstick.target == SDL_MOGA_BUTTON_R3 && last->identity.mapping.leftshoulder.target == SDL_MOGA_BUTTON_L1);
    CHECK(last->identity.mapping.rightshoulder.target == SDL_MOGA_BUTTON_R1 && last->identity.mapping.dpup.target == SDL_MOGA_BUTTON_UP);
    CHECK(last->identity.mapping.dpdown.target == SDL_MOGA_BUTTON_DOWN && last->identity.mapping.dpleft.target == SDL_MOGA_BUTTON_LEFT);
    CHECK(last->identity.mapping.dpright.target == SDL_MOGA_BUTTON_RIGHT && last->identity.mapping.dpright.kind == SDL_SERIAL_MAP_BUTTON);
    CHECK(last->identity.mapping.leftx.kind == SDL_SERIAL_MAP_AXIS && last->identity.mapping.leftx.target == SDL_MOGA_AXIS_LEFT_X);
    CHECK(last->identity.mapping.lefty.target == SDL_MOGA_AXIS_LEFT_Y && last->identity.mapping.rightx.target == SDL_MOGA_AXIS_RIGHT_X);
    CHECK(last->identity.mapping.righty.target == SDL_MOGA_AXIS_RIGHT_Y);
    CHECK(last->identity.mapping.lefttrigger.kind == SDL_SERIAL_MAP_AXIS && last->identity.mapping.lefttrigger.target == SDL_MOGA_AXIS_LEFT_TRIGGER);
    CHECK(last->identity.mapping.righttrigger.target == SDL_MOGA_AXIS_RIGHT_TRIGGER);
    CHECK(last->identity.mapping.guide.kind == SDL_SERIAL_MAP_NONE && last->identity.mapping.misc1.kind == SDL_SERIAL_MAP_NONE);

    /* 3 */
    R_Receive(r, (const uint8_t *)"\x7A\x0E\x66\x01\x04\x00\x7F\x00\x00\x00\x00\xFF\x10\x87", 14);
    CHECK(r->nsnapshots == 2 && R_Buttons(r) == (1u << SDL_MOGA_BUTTON_A));
    CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_X) == 32767 && R_Axis(r, SDL_MOGA_AXIS_LEFT_Y) == 0);
    CHECK(R_Axis(r, SDL_MOGA_AXIS_RIGHT_TRIGGER) == 32767 && R_Axis(r, SDL_MOGA_AXIS_LEFT_TRIGGER) == -32768);
    R_Destroy(r);

    /* 4: a 12-byte report on a digital pad */
    r = R_Up(&SDL_RFCOMMMogaModule, digital_name);
    R_Receive(r, (const uint8_t *)"\x7A\x0C\x64\x01\x00\x11\x00\x80\x00\x00\x10\x92", 12);
    CHECK(R_Present(r) && strcmp(R_Last(r)->identity.name, "PowerA MOGA") == 0);
    CHECK(R_Buttons(r) == (1u << SDL_MOGA_BUTTON_UP));
    CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_TRIGGER) == 32767 && R_Axis(r, SDL_MOGA_AXIS_RIGHT_TRIGGER) == -32768);
    /* Raw -128 is full down, which SDL reads as positive */
    CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_Y) == 32767 && R_Axis(r, SDL_MOGA_AXIS_LEFT_X) == 0);
    R_Destroy(r);

    /* The same 12-byte report reaches an analog-name pad the same way, since
       the length decides the layout */
    r = R_Up(&SDL_RFCOMMMogaModule, analog_name);
    R_Receive(r, (const uint8_t *)"\x7A\x0C\x64\x01\x00\x11\x00\x80\x00\x00\x10\x92", 12);
    CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_TRIGGER) == 32767 && R_Buttons(r) == (1u << SDL_MOGA_BUTTON_UP));
    /* In a 14-byte report the analog value rules and the pressed bit is not read */
    {
        uint8_t report[14];

        Report(report, 14, 0x66, 0x00, 0x30, 0, 0, 0, 0, 0x00, 0x80);
        R_Receive(r, report, 14);
        CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_TRIGGER) == -32768);
        CHECK(R_Axis(r, SDL_MOGA_AXIS_RIGHT_TRIGGER) == (int16_t)(0x80 * 257 - 32768));
        CHECK(R_Buttons(r) == 0);
    }
    R_Destroy(r);
}

/* 5: each bit alone, and the stick values */
static void TestBits(void)
{
    static const int byte4[8] = {
        SDL_MOGA_BUTTON_Y, SDL_MOGA_BUTTON_B, SDL_MOGA_BUTTON_A, SDL_MOGA_BUTTON_X,
        SDL_MOGA_BUTTON_START, SDL_MOGA_BUTTON_SELECT, SDL_MOGA_BUTTON_L1, SDL_MOGA_BUTTON_R1
    };
    static const int byte5[8] = {
        SDL_MOGA_BUTTON_UP, SDL_MOGA_BUTTON_DOWN, SDL_MOGA_BUTTON_LEFT, SDL_MOGA_BUTTON_RIGHT,
        -1, -1, SDL_MOGA_BUTTON_L3, SDL_MOGA_BUTTON_R3
    };
    static const struct
    {
        uint8_t raw;
        int16_t x, y;
    } sticks[] = {
        { 0x7F, 32767, -32768 }, { 0x00, 0, 0 }, { 0x81, -32512, 32511 }, { 0x80, -32768, 32767 },
        { 0x01, 258, -258 }, { 0xFF, -256, 255 }, { 0x40, 16512, -16513 }, { 0xC0, -16384, 16383 },
    };
    uint8_t report[14];
    int bit;
    size_t i;

    for (bit = 0; bit < 8; ++bit) {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);

        Report(report, 14, 0x66, (uint8_t)(1 << bit), 0, 0, 0, 0, 0, 0, 0);
        R_Receive(r, report, 14);
        CHECK(R_Buttons(r) == (1u << byte4[bit]));
        CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_TRIGGER) == -32768 && R_Axis(r, SDL_MOGA_AXIS_RIGHT_TRIGGER) == -32768);
        R_Destroy(r);
    }
    for (bit = 0; bit < 8; ++bit) {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, digital_name);

        Report(report, 12, 0x64, 0, (uint8_t)(1 << bit), 0, 0, 0, 0, 0, 0);
        R_Receive(r, report, 12);
        if (byte5[bit] >= 0) {
            CHECK(R_Buttons(r) == (1u << byte5[bit]));
            CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_TRIGGER) == -32768 && R_Axis(r, SDL_MOGA_AXIS_RIGHT_TRIGGER) == -32768);
        } else {
            /* L2 and R2 pressed, as triggers only */
            CHECK(R_Buttons(r) == 0);
            CHECK(R_Axis(r, (bit == 4) ? SDL_MOGA_AXIS_LEFT_TRIGGER : SDL_MOGA_AXIS_RIGHT_TRIGGER) == 32767);
            CHECK(R_Axis(r, (bit == 4) ? SDL_MOGA_AXIS_RIGHT_TRIGGER : SDL_MOGA_AXIS_LEFT_TRIGGER) == -32768);
        }
        R_Destroy(r);
    }
    for (i = 0; i < sizeof(sticks) / sizeof(sticks[0]); ++i) {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);

        Report(report, 14, 0x66, 0, 0, sticks[i].raw, sticks[i].raw, sticks[i].raw, sticks[i].raw, 0, 0);
        R_Receive(r, report, 14);
        CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_X) == sticks[i].x && R_Axis(r, SDL_MOGA_AXIS_RIGHT_X) == sticks[i].x);
        CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_Y) == sticks[i].y && R_Axis(r, SDL_MOGA_AXIS_RIGHT_Y) == sticks[i].y);
        R_Destroy(r);
    }
    /* Each stick byte reaches its own axis */
    {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);

        Report(report, 14, 0x66, 0, 0, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60);
        R_Receive(r, report, 14);
        CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_X) == SDL_RFCOMM_AxisFromS8(0x10));
        CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_Y) == SDL_RFCOMM_AxisFromS8Negated(0x20));
        CHECK(R_Axis(r, SDL_MOGA_AXIS_RIGHT_X) == SDL_RFCOMM_AxisFromS8(0x30));
        CHECK(R_Axis(r, SDL_MOGA_AXIS_RIGHT_Y) == SDL_RFCOMM_AxisFromS8Negated(0x40));
        CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_TRIGGER) == SDL_RFCOMM_TriggerFromU8(0x50));
        CHECK(R_Axis(r, SDL_MOGA_AXIS_RIGHT_TRIGGER) == SDL_RFCOMM_TriggerFromU8(0x60));
        R_Destroy(r);
    }
    /* Each trigger byte value */
    for (i = 0; i < 256; ++i) {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);

        Report(report, 14, 0x66, 0, 0, 0, 0, 0, 0, (uint8_t)i, (uint8_t)(255 - i));
        R_Receive(r, report, 14);
        CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_TRIGGER) == (int16_t)((int)i * 257 - 32768));
        CHECK(R_Axis(r, SDL_MOGA_AXIS_RIGHT_TRIGGER) == (int16_t)((255 - (int)i) * 257 - 32768));
        R_Destroy(r);
    }
}

/* 6: frames that change nothing */
static void TestRejects(void)
{
    static const uint8_t full14[] = { 0x7A, 0x0E, 0x66, 0x01, 0x04, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x10, 0x87 };
    static const uint8_t full12[] = { 0x7A, 0x0C, 0x64, 0x01, 0x00, 0x11, 0x00, 0x80, 0x00, 0x00, 0x10, 0x92 };
    uint8_t bad[16];
    size_t i, bit;

    /* Every one-bit change of the checksum */
    for (bit = 0; bit < 8; ++bit) {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);

        memcpy(bad, full14, 14);
        bad[13] ^= (uint8_t)(1 << bit);
        R_Receive(r, bad, 14);
        CHECK(r->nsnapshots == 0);
        R_Destroy(r);
    }
    /* A length of 13, and every other length but 12 and 14 */
    for (i = 0; i < 256; ++i) {
        RHarness *r;

        if (i == 12 || i == 14) {
            continue;
        }
        r = R_Up(&SDL_RFCOMMMogaModule, analog_name);
        memcpy(bad, full14, 14);
        bad[1] = (uint8_t)i;
        R_Receive(r, bad, 14);
        CHECK(r->nsnapshots == 0);
        R_Destroy(r);
    }
    /* A missing 7A */
    {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);

        R_Receive(r, full14 + 1, 13);
        CHECK(r->nsnapshots == 0);
        memcpy(bad, full14, 14);
        bad[0] = 0x7B;
        R_Receive(r, bad, 14);
        CHECK(r->nsnapshots == 0);
        R_Destroy(r);
    }
    /* Every proper prefix */
    for (i = 0; i < 14; ++i) {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);

        R_Receive(r, full14, i);
        CHECK(r->nsnapshots == 0);
        R_Destroy(r);
    }
    for (i = 0; i < 12; ++i) {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, digital_name);

        R_Receive(r, full12, i);
        CHECK(r->nsnapshots == 0);
        R_Destroy(r);
    }
    /* After noise and a broken frame the next whole frame still decodes */
    {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);
        uint8_t stream[64];
        size_t length = 0;

        memcpy(stream + length, "\x00\x7A\x7A\x0D\x66", 5);
        length += 5;
        memcpy(stream + length, full14, 14);
        length += 14;
        R_Receive(r, stream, length);
        CHECK(r->nsnapshots == 1 && R_Buttons(r) == (1u << SDL_MOGA_BUTTON_A));
        R_Destroy(r);
    }
    /* The frame after one with a broken checksum decodes */
    {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);
        uint8_t stream[32];

        memcpy(stream, full14, 14);
        stream[13] ^= 0x01;
        memcpy(stream + 14, full12, 12);
        R_Receive(r, stream, 26);
        CHECK(r->nsnapshots == 1 && R_Buttons(r) == (1u << SDL_MOGA_BUTTON_UP));
        R_Destroy(r);
    }
    /* A frame that holds together but starts with another byte is noise */
    {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);
        uint8_t report[14];
        uint8_t checksum = 0;

        Report(report, 14, 0x66, 0x04, 0, 0, 0, 0, 0, 0, 0);
        report[0] = 0x55;
        for (i = 0; i < 13; ++i) {
            checksum ^= report[i];
        }
        report[13] = checksum;
        R_Receive(r, report, 14);
        CHECK(r->nsnapshots == 0);
        R_Destroy(r);
    }
    /* A 13-byte frame with a checksum that holds is still noise, and the
       frame inside it decodes */
    {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);
        uint8_t stream[16];
        uint8_t fake = 0x7A ^ 0x0D, sum = 0;

        /* The real frame at offset 2, its left trigger chosen so that the
           first 13 bytes also pass a checksum */
        Report(stream + 2, 14, 0x66, 0x02, 0, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00);
        for (i = 0; i < 10; ++i) {
            fake ^= stream[2 + i];
        }
        stream[0] = 0x7A;
        stream[1] = 0x0D;
        stream[12] = fake;
        for (i = 0; i < 13; ++i) {
            sum ^= stream[2 + i];
        }
        stream[15] = sum;
        R_Receive(r, stream, 16);
        CHECK(r->nsnapshots == 1 && R_Buttons(r) == (1u << SDL_MOGA_BUTTON_B));
        CHECK(R_Axis(r, SDL_MOGA_AXIS_LEFT_TRIGGER) == SDL_RFCOMM_TriggerFromU8(fake));
        R_Destroy(r);
    }
    /* The id in a report is not checked */
    {
        RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);
        uint8_t report[14];

        Report(report, 14, 0x66, 0x02, 0, 0, 0, 0, 0, 0, 0);
        report[3] = 0x04;
        report[13] ^= 0x01 ^ 0x04;
        R_Receive(r, report, 14);
        CHECK(R_Buttons(r) == (1u << SDL_MOGA_BUTTON_B));
        R_Destroy(r);
    }
}

/* Every split of a stream of several frames */
static void TestSplits(void)
{
    uint8_t stream[64];
    size_t length = 0;

    length += Report(stream + length, 14, 0x65, 0x10, 0x00, 0x20, 0xE0, 0x00, 0x00, 0x00, 0x00);
    length += Report(stream + length, 14, 0x66, 0x10, 0x01, 0x20, 0xE0, 0x7F, 0x80, 0x40, 0xC0);
    length += Report(stream + length, 12, 0x64, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    R_CheckSplits(&SDL_RFCOMMMogaModule, analog_name, stream, length);
}

/* 7: the quiet-link rule */
static void TestQuiet(void)
{
    RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);
    uint8_t report[14];

    /* Nothing before 2000 ms */
    R_Advance(r, 1999);
    CHECK(R_Next(r) == NULL);
    R_Advance(r, 2000);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x45\x01\x1B", 2000));
    CHECK(R_Next(r) == NULL);
    /* The reply brings the listen command */
    r->now = 2100;
    Report(report, 14, 0x65, 0, 0, 0, 0, 0, 0, 0, 0);
    R_Receive(r, report, 14);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x46\x01\x18", 2100));
    CHECK(R_Next(r) == NULL && R_Present(r));
    /* A listen report brings nothing */
    r->now = 2200;
    Report(report, 14, 0x66, 0, 0, 0, 0, 0, 0, 0, 0);
    R_Receive(r, report, 14);
    CHECK(R_Next(r) == NULL);
    /* The next quiet period counts from the last report */
    R_Advance(r, 4199);
    CHECK(R_Next(r) == NULL);
    R_Advance(r, 4200);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x45\x01\x1B", 4200));
    /* A second 2000 ms of silence ends the link, and the joystick goes */
    R_Advance(r, 6199);
    CHECK(R_Next(r) == NULL && R_Present(r));
    R_Advance(r, 6200);
    CHECK(R_IsClose(R_Next(r), 6200));
    CHECK(!R_Present(r) && r->link.phase == SDL_RFCOMM_WAITING);
    /* It connects again 3000 ms later */
    R_Advance(r, 9199);
    CHECK(R_Next(r) == NULL);
    R_Advance(r, 9200);
    CHECK(R_IsConnect(R_Next(r), 0, 9200));
    R_Destroy(r);

    /* A pad that never answers the start-up is dropped after 4000 ms */
    r = R_Up(&SDL_RFCOMMMogaModule, digital_name);
    R_Advance(r, 2000);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x41\x01\x1F", 2000));
    R_Advance(r, 4000);
    CHECK(R_IsClose(R_Next(r), 4000) && r->nsnapshots == 0);
    R_Destroy(r);

    /* A pad that ignored the listen command after connecting gets it again
       with the next poll's answer */
    r = R_Up(&SDL_RFCOMMMogaModule, digital_name);
    r->now = 50;
    Report(report, 12, 0x61, 0, 0, 0, 0, 0, 0, 0, 0);
    R_Receive(r, report, 12);
    CHECK(R_Next(r) == NULL && R_Present(r));
    R_Advance(r, 2050);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x41\x01\x1F", 2050));
    r->now = 2060;
    R_Receive(r, report, 12);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x44\x01\x1A", 2060));
    R_Destroy(r);
}

/* 8: the player index. The part reads player index 3 as id 3, but SDL
   numbers players from 0, so id 3 is player index 2. */
static void TestPlayer(void)
{
    RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);

    R_SetPlayer(r, 2);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x43\x03\x1F", 0));
    CHECK(R_Next(r) == NULL);
    /* The same index sends nothing */
    R_SetPlayer(r, 2);
    CHECK(R_Next(r) == NULL);
    R_SetPlayer(r, 3);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x43\x04\x18", 0));
    /* No player lights no LED */
    R_SetPlayer(r, -1);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x43\x05\x19", 0));
    /* Later commands carry the new id */
    R_Advance(r, 2000);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x45\x05\x1F", 2000));
    R_Destroy(r);

    /* A player index set before connecting is the id of the start-up */
    r = R_Create(&SDL_RFCOMMMogaModule, analog_name);
    R_Skip(r);
    R_SetPlayer(r, 1);
    CHECK(R_Next(r) == NULL);
    R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x43\x02\x1E", 0));
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x45\x02\x18", 0));
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x46\x02\x1B", 0));
    R_Destroy(r);
}

/* A lost link releases every control, and the next link starts over */
static void TestLost(void)
{
    RHarness *r = R_Up(&SDL_RFCOMMMogaModule, analog_name);
    uint8_t report[14];

    Report(report, 14, 0x66, 0xFF, 0xFF, 0x7F, 0x7F, 0x7F, 0x7F, 0xFF, 0xFF);
    R_Receive(r, report, 14);
    CHECK(R_Present(r) && R_Buttons(r) != 0);
    r->now = 500;
    R_Lost(r);
    CHECK(!R_Present(r) && R_Buttons(r) == 0 && R_Axis(r, SDL_MOGA_AXIS_LEFT_X) == 0);
    CHECK(R_Next(r) == NULL);
    R_Advance(r, 3500);
    CHECK(R_IsConnect(R_Next(r), 0, 3500));
    R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    CHECK(IsCommand(R_Next(r), (const uint8_t *)"\x5A\x05\x43\x01\x1D", 3500));
    R_Skip(r);
    /* A partial frame from the old link is gone */
    R_Receive(r, report, 7);
    R_Lost(r);
    R_Advance(r, 6500);
    R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    R_Receive(r, report + 7, 7);
    CHECK(!R_Present(r));
    R_Destroy(r);
}

int main(void)
{
    TestHelpers();
    TestStartup();
    TestFrames();
    TestBits();
    TestRejects();
    TestSplits();
    TestQuiet();
    TestPlayer();
    TestLost();
    return H_Finish();
}
