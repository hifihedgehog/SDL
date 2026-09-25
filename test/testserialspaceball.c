/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_spaceball_proto.c, the
   serial Spaceball of hifihedgehog/SDL#33 Part 5. The vectors are built from
   the start-up of inputattach and spacenavd and the decoders of Linux, VRPN,
   spacenavd and libsball. Test numbers follow the ticket. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_spaceball_proto.h"

static SDL_SpaceballState *State(Harness *h)
{
    return (SDL_SpaceballState *)h->state;
}

/* A reply line and its 0D */
static void Line(Harness *h, uint64_t t, const char *text)
{
    H_Advance(h, t);
    H_FeedText(h, text);
    H_Feed(h, (const uint8_t *)"\r", 1);
}

static void Bytes(Harness *h, uint64_t t, const uint8_t *data, size_t length)
{
    H_FeedAt(h, t, data, length);
}

static const uint8_t xon_line[2] = { 0x11, 0x0D };
static const uint8_t reset_command[8] = { 0x0D, 0x40, 0x52, 0x45, 0x53, 0x45, 0x54, 0x0D };

/* Test 1's start-up from the time of the open: banner, firmware 2.63, the
   3003C reply and the three setup replies */
static void BringUp3003C(Harness *h)
{
    const uint64_t t = h->now;

    Bytes(h, t + 10, xon_line, 2);
    Line(h, t + 20, "@1 Spaceball alive and well after a poweron reset.");
    Line(h, t + 30, "@2 Firmware version 2.63 created on 11-Jan-1994.");
    Line(h, t + 40, "Hm3003C");
    Line(h, t + 50, "P");
    Line(h, t + 60, "F");
    Line(h, t + 70, "M");
}

static void TestStartupBanner(void)
{
    Harness *h = H_Create(&SDL_SerialSpaceballModule);

    /* 1 */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_NextCall(h) == NULL);
    Bytes(h, 100, xon_line, 2);
    Line(h, 150, "@1 Spaceball alive");
    CHECK(H_NextCall(h) == NULL);
    Line(h, 200, "@2 Firmware version 2.63");
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x68\x6D\x0D", 3, 200));
    CHECK(State(h)->firmware_major == 2 && State(h)->firmware_minor == 63);
    Line(h, 300, "Hm3003C");
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x50\x40\x41\x40\x41\x0D", 6, 300));
    Line(h, 350, "P@A@A");
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x46\x54\x40\x0D", 4, 350));
    Line(h, 400, "FT@");
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x4D\x53\x53\x0D", 4, 400));
    CHECK(h->npresence == 0);
    Line(h, 450, "MSS");
    CHECK(h->npresence == 1 && h->presence[0] == 1);
    CHECK(strcmp(h->identity[0].name, "Spaceball 3003C") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN && h->identity[0].naxes == 6 && h->identity[0].nbuttons == 12);
    CHECK(h->identity[0].nhats == 0 && h->identity[0].nballs == 0);
    CHECK(H_NextCall(h) == NULL);

    /* No keep-alive and no silence limit: a minute of quiet changes nothing */
    H_Advance(h, 60450);
    CHECK(H_NextCall(h) == NULL && h->presence[0] == 1);
    H_Destroy(h);

    /* 0A bytes in start-up lines are skipped, and junk before the 11 byte is ignored */
    h = H_Create(&SDL_SerialSpaceballModule);
    H_Start(h);
    H_SkipCalls(h);
    Line(h, 10, "noise");
    Bytes(h, 20, (const uint8_t *)"\x0A\x41\x11\x0D\x0A", 5);
    Line(h, 30, "\n@1 Spaceball alive");
    Line(h, 40, "\n@2 Firmware version 2.02");
    CHECK(H_IsWriteText(H_NextCall(h), "hm\r", 40));
    CHECK(State(h)->firmware_major == 2 && State(h)->firmware_minor == 2);
    H_Destroy(h);
}

static void TestStartupReset(void)
{
    Harness *h = H_Create(&SDL_SerialSpaceballModule);

    /* 2 */
    H_Start(h);
    H_SkipCalls(h);
    H_Advance(h, 3999);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 4000);
    CHECK(H_IsWrite(H_NextCall(h), reset_command, 8, 4000));
    H_Advance(h, 6999);
    CHECK(H_NextCall(h) == NULL && h->npresence == 0);
    H_Advance(h, 7000);
    CHECK(H_IsWrite(H_NextCall(h), reset_command, 8, 7000));
    /* A line without "@1" is no answer */
    Line(h, 7050, "Hm3003C");
    CHECK(State(h)->step == SDL_SPACEBALL_STEP_RESET && H_NextCall(h) == NULL);

    /* An answer holding "@1" anywhere, as spacenavd searches for it, then the
       firmware line. An unknown H reply with firmware 2.15 is a 1003/2003. */
    Line(h, 7100, "xx@1 Spaceball alive");
    Line(h, 7150, "@2 Firmware version 2.15");
    CHECK(H_IsWriteText(H_NextCall(h), "hm\r", 7150));
    Line(h, 7200, "Hm2003");
    CHECK(H_IsWriteText(H_NextCall(h), "P@A@A\r", 7200));
    Line(h, 7210, "P");
    Line(h, 7220, "F");
    Line(h, 7230, "M");
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Spaceball 1003/2003") == 0);
    H_Destroy(h);

    /* A first @ line that is not the banner goes to step 2 at once */
    h = H_Create(&SDL_SerialSpaceballModule);
    H_Start(h);
    H_SkipCalls(h);
    Bytes(h, 10, xon_line, 2);
    Line(h, 20, "@2 Firmware version 2.63");
    CHECK(H_IsWrite(H_NextCall(h), reset_command, 8, 20));
    H_Destroy(h);

    /* An 11 byte with no banner line within 1000 ms also goes to step 2 */
    h = H_Create(&SDL_SerialSpaceballModule);
    H_Start(h);
    H_SkipCalls(h);
    Bytes(h, 10, xon_line, 2);
    H_Advance(h, 1009);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1010);
    CHECK(H_IsWrite(H_NextCall(h), reset_command, 8, 1010));
    H_Destroy(h);
}

static void TestStartupFlx(void)
{
    Harness *h = H_Create(&SDL_SerialSpaceballModule);

    /* 3 */
    H_Start(h);
    H_SkipCalls(h);
    Bytes(h, 10, xon_line, 2);
    Line(h, 20, "@1 Spaceball alive");
    Line(h, 30, "@2 Firmware version 2.43");
    CHECK(H_IsWriteText(H_NextCall(h), "hm\r", 30));
    Line(h, 40, "HvFirmware");
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x22\x0D", 2, 40));
    Line(h, 50, "\"1 Spaceball 4000 FLX");
    Bytes(h, 60, (const uint8_t *)"\x22\x32\x20\x4C\x20\x0D", 6);
    CHECK(H_NextCall(h) == NULL);
    Bytes(h, 70, (const uint8_t *)"\x22\x33\x0D", 3);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x59\x53\x0D", 3, 70));
    Line(h, 80, "YS");
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x4D\x0D", 2, 80));
    CHECK(h->npresence == 0);
    Line(h, 90, "M");
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Spaceball 4000 FLX Lefty") == 0);
    H_Destroy(h);

    /* Right-handed, and the model reply in any case */
    h = H_Create(&SDL_SerialSpaceballModule);
    H_Start(h);
    H_SkipCalls(h);
    Bytes(h, 10, xon_line, 2);
    Line(h, 20, "@1 Spaceball alive");
    Line(h, 30, "@2 Firmware version 2.43");
    Line(h, 40, "HVFIRMWARE v2.4.3");
    CHECK(H_IsWriteText(H_NextCall(h), "hm\r", 30));
    CHECK(H_IsWriteText(H_NextCall(h), "\"\r", 40));
    Line(h, 50, "\"1 Spaceball 4000 FLX");
    Line(h, 60, "\"2 R ");
    Line(h, 70, "\"3");
    Line(h, 80, "Y");
    Line(h, 90, "M");
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Spaceball 4000 FLX") == 0);
    H_Destroy(h);

    /* An L that is not " L " is not a Lefty */
    h = H_Create(&SDL_SerialSpaceballModule);
    H_Start(h);
    Bytes(h, 10, xon_line, 2);
    Line(h, 20, "@1 Spaceball alive");
    Line(h, 30, "@2 Firmware version 2.43");
    Line(h, 40, "HvFirmware");
    Line(h, 50, "\"1 Spaceball 4000 FLX");
    Line(h, 60, "\"2 LR");
    Line(h, 70, "\"3");
    Line(h, 80, "Y");
    Line(h, 90, "M");
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Spaceball 4000 FLX") == 0);
    H_Destroy(h);

    /* A " reply naming something else fails, and step 2 follows after 1000 ms */
    h = H_Create(&SDL_SerialSpaceballModule);
    H_Start(h);
    H_SkipCalls(h);
    Bytes(h, 10, xon_line, 2);
    Line(h, 20, "@1 Spaceball alive");
    Line(h, 30, "@2 Firmware version 2.43");
    Line(h, 40, "HvFirmware");
    Line(h, 50, "\"1 Spaceball 5000");
    H_SkipCalls(h);
    H_Advance(h, 1049);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1050);
    CHECK(H_IsWrite(H_NextCall(h), reset_command, 8, 1050));
    CHECK(h->npresence == 0);
    H_Destroy(h);
}

/* Every reply the model table lists */
static void TestClassify(void)
{
    CHECK(SDL_Spaceball_Classify("Hm2003B", -1, -1) == SDL_SPACEBALL_MODEL_2003B);
    CHECK(SDL_Spaceball_Classify("Hm2003C", 2, 63) == SDL_SPACEBALL_MODEL_2003C);
    CHECK(SDL_Spaceball_Classify("Hm3003C", 2, 15) == SDL_SPACEBALL_MODEL_3003C);
    CHECK(SDL_Spaceball_Classify("hVfIrMwArE", -1, -1) == SDL_SPACEBALL_MODEL_4000FLX);
    CHECK(SDL_Spaceball_Classify("Hm2003", 2, 2) == SDL_SPACEBALL_MODEL_1003_2003);
    CHECK(SDL_Spaceball_Classify("Hm2003", 2, 13) == SDL_SPACEBALL_MODEL_1003_2003);
    CHECK(SDL_Spaceball_Classify("H", 2, 15) == SDL_SPACEBALL_MODEL_1003_2003);
    CHECK(SDL_Spaceball_Classify("H", 2, 42) == SDL_SPACEBALL_MODEL_1003_2003);
    CHECK(SDL_Spaceball_Classify("H", 2, 35) == SDL_SPACEBALL_MODEL_3003);
    CHECK(SDL_Spaceball_Classify("H", 2, 62) == SDL_SPACEBALL_MODEL_3003);
    CHECK(SDL_Spaceball_Classify("H", 2, 63) == SDL_SPACEBALL_MODEL_3003);
    CHECK(SDL_Spaceball_Classify("H", 2, 43) == SDL_SPACEBALL_MODEL_4000FLX);
    CHECK(SDL_Spaceball_Classify("H", 2, 45) == SDL_SPACEBALL_MODEL_4000FLX);
    CHECK(SDL_Spaceball_Classify("H", 2, 44) == SDL_SPACEBALL_MODEL_UNKNOWN);
    CHECK(SDL_Spaceball_Classify("H", 3, 63) == SDL_SPACEBALL_MODEL_UNKNOWN);
    CHECK(SDL_Spaceball_Classify("H", -1, -1) == SDL_SPACEBALL_MODEL_UNKNOWN);
    CHECK(strcmp(SDL_Spaceball_ModelName(SDL_SPACEBALL_MODEL_UNKNOWN), "Spaceball") == 0);
    CHECK(strcmp(SDL_Spaceball_ModelName(SDL_SPACEBALL_MODEL_2003B), "Spaceball 2003B") == 0);
    CHECK(strcmp(SDL_Spaceball_ModelName(SDL_SPACEBALL_MODEL_2003C), "Spaceball 2003C") == 0);
    CHECK(strcmp(SDL_Spaceball_ModelName(SDL_SPACEBALL_MODEL_3003), "Spaceball 3003/3003C") == 0);
}

/* A reply that never comes, and eight lines without the one awaited */
static void TestStartupFailures(void)
{
    Harness *h = H_Create(&SDL_SerialSpaceballModule);
    int i;

    H_Start(h);
    H_SkipCalls(h);
    Bytes(h, 10, xon_line, 2);
    Line(h, 20, "@1 Spaceball alive");
    Line(h, 30, "@2 Firmware version 2.63");
    Line(h, 40, "Hm3003C");
    CHECK(H_IsWriteText(H_NextCall(h), "hm\r", 30));
    CHECK(H_IsWriteText(H_NextCall(h), "P@A@A\r", 40));
    /* Seven lines without P, each within 1000 ms, keep the wait alive */
    for (i = 0; i < 7; ++i) {
        Line(h, 900 + (uint64_t)i * 900, "X");
    }
    CHECK(H_NextCall(h) == NULL);
    /* The eighth fails the start-up */
    Line(h, 7200, "X");
    H_Advance(h, 8199);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 8200);
    CHECK(H_IsWrite(H_NextCall(h), reset_command, 8, 8200));

    /* The reset answered, then no firmware line within 1000 ms */
    Line(h, 8300, "@1 Spaceball alive");
    H_Advance(h, 9299);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 9300);
    H_Advance(h, 10300);
    CHECK(H_IsWrite(H_NextCall(h), reset_command, 8, 10300));

    /* A line longer than 128 bytes counts as a line that does not match */
    Line(h, 10400, "@1 Spaceball alive");
    {
        char longline[200];
        memset(longline, '@', sizeof(longline) - 1);
        longline[sizeof(longline) - 1] = '\0';
        Line(h, 10500, longline);
    }
    CHECK(H_NextCall(h) == NULL);
    Line(h, 10600, "@2 Firmware version 2.63");
    CHECK(H_IsWriteText(H_NextCall(h), "hm\r", 10600));
    H_Destroy(h);

    /* The reply timer starts when the write finishes */
    h = H_Create(&SDL_SerialSpaceballModule);
    h->pend_writes = true;
    H_Start(h);
    H_SkipCalls(h);
    Bytes(h, 10, xon_line, 2);
    Line(h, 20, "@1 Spaceball alive");
    Line(h, 30, "@2 Firmware version 2.63");
    CHECK(H_IsWriteText(H_NextCall(h), "hm\r", 30));
    H_Advance(h, 2000);
    H_CompleteWrite(h, true);
    H_Advance(h, 2999);
    CHECK(State(h)->step == SDL_SPACEBALL_STEP_MODEL);
    H_Advance(h, 3000);
    CHECK(State(h)->step == SDL_SPACEBALL_STEP_RETRY);
    H_Destroy(h);

    /* An answer before its write finishes: the next command's timer waits
       for that command's own write, not for the earlier one */
    h = H_Create(&SDL_SerialSpaceballModule);
    h->pend_writes = true;
    H_Start(h);
    H_SkipCalls(h);
    Bytes(h, 10, xon_line, 2);
    Line(h, 20, "@1 Spaceball alive");
    Line(h, 30, "@2 Firmware version 2.63");
    Line(h, 40, "Hm3003C");
    CHECK(State(h)->step == SDL_SPACEBALL_STEP_P);
    H_Advance(h, 100);
    H_CompleteWrite(h, true); /* hm */
    CHECK(H_IsWriteText(&h->calls[h->ncalls - 1], "P@A@A\r", 100));
    H_Advance(h, 1500);
    CHECK(State(h)->step == SDL_SPACEBALL_STEP_P);
    H_CompleteWrite(h, true); /* P@A@A */
    H_Advance(h, 2499);
    CHECK(State(h)->step == SDL_SPACEBALL_STEP_P);
    H_Advance(h, 2500);
    CHECK(State(h)->step == SDL_SPACEBALL_STEP_RETRY);
    H_Destroy(h);
}

static Harness *PresentBall(void)
{
    Harness *h = H_Create(&SDL_SerialSpaceballModule);

    H_Start(h);
    BringUp3003C(h);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

static const uint8_t v4[16] = { 0x44, 0x00, 0x10, 0x00, 0x64, 0xFF, 0x9C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0D };
static const uint8_t v5[21] = { 0x44, 0x00, 0x10, 0x5E, 0x4D, 0x5E, 0x4D, 0x5E, 0x51, 0x5E, 0x53, 0x5E, 0x5E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D };
static const uint8_t v6[21] = { 0x44, 0x00, 0x10, 0x5E, 0x41, 0x5E, 0x4D, 0x5E, 0x51, 0x5E, 0x53, 0x5E, 0x5E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D };
static const uint8_t k_button1[4] = { 0x4B, 0x00, 0x01, 0x0D };
static const uint8_t k_button7[4] = { 0x4B, 0x04, 0x00, 0x0D };
static const uint8_t k_rezero2003[4] = { 0x4B, 0x08, 0x00, 0x0D };
static const uint8_t k_rezero3003[4] = { 0x4B, 0x20, 0x00, 0x0D };
static const uint8_t k_pick[4] = { 0x4B, 0x10, 0x00, 0x0D };
static const uint8_t k_left[4] = { 0x4B, 0x00, 0x10, 0x0D };
static const uint8_t k_right[4] = { 0x4B, 0x00, 0x20, 0x0D };
static const uint8_t dot_right[4] = { 0x2E, 0x20, 0x01, 0x0D };
static const uint8_t dot_all[4] = { 0x2E, 0x3F, 0xBF, 0x0D };
static const uint8_t dot_none[4] = { 0x2E, 0x00, 0x00, 0x0D };
static const uint8_t short_d[4] = { 0x44, 0x00, 0x10, 0x0D };
static const uint8_t long_k[5] = { 0x4B, 0x00, 0x01, 0x00, 0x0D };

static void TestPackets(void)
{
    Harness *h = PresentBall();
    uint8_t long_packet[130];

    /* 4 */
    H_Feed(h, v4, sizeof(v4));
    CHECK(H_Axis(h, 0, 0) == 100 && H_Axis(h, 0, 1) == -100 && H_Axis(h, 0, 2) == 256);
    CHECK(H_Axis(h, 0, 3) == 0 && H_Axis(h, 0, 4) == 0 && H_Axis(h, 0, 5) == 1);

    /* 5 */
    H_Feed(h, v5, sizeof(v5));
    CHECK(H_Axis(h, 0, 0) == 3341 && H_Axis(h, 0, 1) == 4371 && H_Axis(h, 0, 2) == 24064);
    CHECK(H_Axis(h, 0, 3) == 0 && H_Axis(h, 0, 4) == 0 && H_Axis(h, 0, 5) == 0);

    /* 6: after ^, any other byte stands for itself */
    H_Feed(h, v6, sizeof(v6));
    CHECK(H_Axis(h, 0, 0) == 0x410D && H_Axis(h, 0, 1) == 4371);

    /* Raw 0A, 11 and 13 inside a packet are data */
    {
        static const uint8_t raw[16] = { 0x44, 0x00, 0x10, 0x0A, 0x0A, 0x11, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D };
        H_Feed(h, raw, sizeof(raw));
        CHECK(H_Axis(h, 0, 0) == 0x0A0A && H_Axis(h, 0, 1) == 0x1113 && H_Axis(h, 0, 2) == 0);
    }
    /* A raw 0D after ^ still ends the packet */
    {
        static const uint8_t cut[6] = { 0x44, 0x00, 0x10, 0x5E, 0x0D, 0x0D };
        const int mark = h->npublished;
        H_Feed(h, cut, sizeof(cut));
        CHECK(h->npublished == mark);
        H_Feed(h, v4, sizeof(v4));
        CHECK(H_Axis(h, 0, 0) == 100);
    }

    /* 7 */
    H_Feed(h, k_button1, 4);
    CHECK(H_OnlyButton(h, 0, 0));
    H_Feed(h, k_button7, 4);
    CHECK(H_OnlyButton(h, 0, 6));
    H_Feed(h, k_rezero2003, 4);
    CHECK(H_OnlyButton(h, 0, 7));
    H_Feed(h, k_rezero3003, 4);
    CHECK(H_OnlyButton(h, 0, 7));
    H_Feed(h, k_pick, 4);
    CHECK(H_OnlyButton(h, 0, 8));
    H_Feed(h, k_left, 4);
    CHECK(H_OnlyButton(h, 0, 0));
    H_Feed(h, k_right, 4);
    CHECK(H_OnlyButton(h, 0, 1));
    {
        static const uint8_t k_rest[4] = { 0x4B, 0x00, 0x00, 0x0D };
        static const uint8_t k_many[4] = { 0x4B, 0x07, 0x0E, 0x0D };
        static const int many[] = { 1, 2, 3, 4, 5, 6, -1 };
        H_Feed(h, k_many, 4);
        CHECK(H_OnlyButtons(h, 0, many));
        H_Feed(h, k_rest, 4);
        CHECK(H_NoButtons(h, 0));
    }

    /* 8 */
    H_Feed(h, dot_right, 4);
    CHECK(H_OnlyButton(h, 0, 0) && State(h)->right_handed);
    H_Feed(h, dot_all, 4);
    {
        static const int all[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1 };
        CHECK(H_OnlyButtons(h, 0, all));
    }
    H_Feed(h, dot_none, 4);
    CHECK(H_NoButtons(h, 0) && !State(h)->right_handed);
    {
        const int mark = h->npublished;
        H_Feed(h, k_button1, 4);
        CHECK(h->npublished == mark && H_NoButtons(h, 0));
    }

    /* 9 */
    {
        const int mark = h->npublished;
        H_Feed(h, short_d, sizeof(short_d));
        H_Feed(h, long_k, sizeof(long_k));
        memset(long_packet, 0, sizeof(long_packet));
        long_packet[0] = 0x44;
        long_packet[129] = 0x0D;
        H_Feed(h, long_packet, sizeof(long_packet));
        CHECK(h->npublished == mark);
        /* E and ? packets are logged and change nothing */
        H_FeedText(h, "E1\r?X\r");
        CHECK(h->npublished == mark && State(h)->errors == 2 && h->nlogs == 2);
        /* A reset banner longer than 128 bytes is discarded like any long packet */
        long_packet[0] = '@';
        memset(long_packet + 1, 'x', 128);
        long_packet[129] = 0x0D;
        H_Feed(h, long_packet, sizeof(long_packet));
        CHECK(h->presence[0] == 1 && State(h)->step == SDL_SPACEBALL_STEP_PRESENT);
    }
    H_Destroy(h);
}

static void TestResetBanner(void)
{
    Harness *h = PresentBall();

    /* 10 */
    H_Feed(h, v4, sizeof(v4));
    H_Advance(h, 1000);
    Line(h, 1000, "@1 Spaceball alive");
    CHECK(h->presence[0] == 0);
    CHECK(H_NextCall(h) == NULL);
    Line(h, 1100, "@2 Firmware version 2.63");
    CHECK(H_IsWriteText(H_NextCall(h), "hm\r", 1100));
    Line(h, 1200, "Hm3003C");
    Line(h, 1210, "P");
    Line(h, 1220, "F");
    Line(h, 1230, "M");
    CHECK(h->presence[0] == 2);
    CHECK(H_Axis(h, 0, 0) == 0);
    H_Destroy(h);
}

/* Battery B: port loss, return, and start-up again with a new instance */
static void TestBatteryB(void)
{
    Harness *h = PresentBall();

    H_Advance(h, 500);
    H_Feed(h, v4, sizeof(v4));
    H_LosePort(h);
    CHECK(h->presence[0] == 0);
    CHECK(H_IsCall(H_NextCall(h), 'C', 500));
    H_Advance(h, 1499);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1500);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 1500));
    CHECK(H_NextCall(h) == NULL);
    /* The same start-up, and nothing presented before it ends */
    Bytes(h, 1510, xon_line, 2);
    Line(h, 1520, "@1 Spaceball alive");
    Line(h, 1530, "@2 Firmware version 2.63");
    CHECK(H_IsWriteText(H_NextCall(h), "hm\r", 1530));
    Line(h, 1540, "Hm3003C");
    Line(h, 1550, "P");
    Line(h, 1560, "F");
    CHECK(h->presence[0] == 0);
    Line(h, 1570, "M");
    CHECK(h->presence[0] == 2);
    CHECK(H_Axis(h, 0, 0) == 0);
    /* Without a reply the port stays open and the start-up retries forever */
    H_Destroy(h);
}

static void TestBatteryA(void)
{
    Harness *h = PresentBall();
    H_Vector vectors[16];
    uint8_t long_packet[130];
    int n = 0, i;

#define VECTOR(name, bytes, changes) H_SetVector(&vectors[n++], name, bytes, sizeof(bytes), changes, 0)
    VECTOR("4 D", v4, 1);
    VECTOR("5 escapes", v5, 1);
    VECTOR("6 unknown escape", v6, 1);
    VECTOR("7 K button 1", k_button1, 1);
    VECTOR("7 K button 7", k_button7, 1);
    VECTOR("7 K 2003 rezero", k_rezero2003, 1);
    VECTOR("7 K 3003 rezero", k_rezero3003, 1);
    VECTOR("7 K pick", k_pick, 1);
    VECTOR("7 K left", k_left, 1);
    VECTOR("7 K right", k_right, 1);
    VECTOR("8 dot right", dot_right, 1);
    VECTOR("8 dot all", dot_all, 1);
    VECTOR("8 dot none", dot_none, 0);
#undef VECTOR
    for (i = 0; i < n; ++i) {
        H_BatteryA(h, &vectors[i], BringUp3003C);
    }
    /* (A6) on the malformed packets of test 9 */
    memset(long_packet, 0, sizeof(long_packet));
    long_packet[0] = 0x44;
    long_packet[129] = 0x0D;
    H_BatteryA6(h, "9 short D", short_d, sizeof(short_d), &vectors[0]);
    H_BatteryA6(h, "9 long K", long_k, sizeof(long_k), &vectors[3]);
    H_BatteryA6(h, "9 129 bytes", long_packet, sizeof(long_packet), &vectors[0]);
    H_Destroy(h);
}

int main(void)
{
    TestStartupBanner();
    TestStartupReset();
    TestStartupFlx();
    TestClassify();
    TestStartupFailures();
    TestPackets();
    TestResetBanner();
    TestBatteryB();
    TestBatteryA();
    return H_Finish();
}
