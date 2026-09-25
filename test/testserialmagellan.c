/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_magellan_proto.c, the
   serial Magellan and SpaceMouse of hifihedgehog/SDL#33 Part 5. The vectors
   are constructed from the X.org start-up and the cited decoders. Test
   numbers follow the ticket. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_magellan_proto.h"

static SDL_MagellanState *State(Harness *h)
{
    return (SDL_MagellanState *)h->state;
}

static void Line(Harness *h, uint64_t t, const char *text)
{
    H_Advance(h, t);
    H_FeedText(h, text);
    H_Feed(h, (const uint8_t *)"\r", 1);
}

static const char version_text[] = "vMAGELLAN  Version 5.49  by LOGITECH INC. 10/22/96";

/* The start-up from the time of the open, every echo right away */
static void BringUp(Harness *h)
{
    const uint64_t t = h->now;

    H_Advance(h, t + 1000);
    Line(h, t + 1010, "m0");
    Line(h, t + 1020, "z");
    Line(h, t + 1030, "z");
    Line(h, t + 1040, "q00");
    Line(h, t + 1050, "pAA");
    Line(h, t + 1060, "nH");
    Line(h, t + 1070, "m3");
    Line(h, t + 1080, version_text);
}

static Harness *PresentMagellan(void)
{
    Harness *h = H_Create(&SDL_SerialMagellanModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialMagellanModule);

    /* 1 */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 2, 0));
    H_Advance(h, 999);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1000);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x0D\x0D\x6D\x30\x0D", 5, 1000));
    H_Advance(h, 1199);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1200);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x7A\x0D", 2, 1200));
    H_Advance(h, 1400);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x7A\x0D", 2, 1400));
    H_Advance(h, 1600);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x71\x30\x30\x0D", 4, 1600));
    H_FeedAt(h, 1650, (const uint8_t *)"\x71\x30\x30\x0D", 4);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x70\x41\x41\x0D", 4, 1650));
    H_FeedAt(h, 1700, (const uint8_t *)"\x70\x41\x41\x0D", 4);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x6E\x48\x0D", 3, 1700));
    H_FeedAt(h, 1750, (const uint8_t *)"\x6E\x48\x0D", 3);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x6D\x33\x0D", 3, 1750));
    H_FeedAt(h, 1800, (const uint8_t *)"\x6D\x33\x0D", 3);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x76\x51\x0D", 3, 1800));
    CHECK(h->npresence == 0);
    Line(h, 1850, version_text);
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Magellan SpaceMouse") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN && h->identity[0].naxes == 6 && h->identity[0].nbuttons == 12);
    CHECK(H_NextCall(h) == NULL);
    /* No keep-alive */
    H_Advance(h, 61850);
    CHECK(H_NextCall(h) == NULL && h->presence[0] == 1);
    H_Destroy(h);

    /* A line in steps 2 and 3 moves on at once */
    h = H_Create(&SDL_SerialMagellanModule);
    H_Start(h);
    H_SkipCalls(h);
    H_Advance(h, 1000);
    CHECK(H_IsWriteText(H_NextCall(h), "\r\rm0\r", 1000));
    Line(h, 1050, "m0");
    CHECK(H_IsWriteText(H_NextCall(h), "z\r", 1050));
    Line(h, 1060, "z");
    CHECK(H_IsWriteText(H_NextCall(h), "z\r", 1060));
    Line(h, 1070, "z");
    CHECK(H_IsWriteText(H_NextCall(h), "q00\r", 1070));
    /* A line that is not the echo does not count, a longer one neither */
    Line(h, 1080, "q01");
    Line(h, 1085, "q00x");
    CHECK(H_NextCall(h) == NULL);
    Line(h, 1090, "q00");
    CHECK(H_IsWriteText(H_NextCall(h), "pAA\r", 1090));
    H_Destroy(h);
}

static void TestStartupFailure(void)
{
    Harness *h = H_Create(&SDL_SerialMagellanModule);

    /* 2 */
    H_Start(h);
    H_SkipCalls(h);
    H_Advance(h, 1600);
    H_SkipCalls(h);
    H_Advance(h, 2599);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 2600);
    H_Advance(h, 3599);
    CHECK(H_NextCall(h) == NULL && h->npresence == 0);
    H_Advance(h, 3600);
    CHECK(H_IsWriteText(H_NextCall(h), "\r\rm0\r", 3600));
    H_Destroy(h);

    /* No version line within 1000 ms */
    h = H_Create(&SDL_SerialMagellanModule);
    H_Start(h);
    H_Advance(h, 1000);
    Line(h, 1010, "m0");
    Line(h, 1020, "z");
    Line(h, 1030, "z");
    Line(h, 1040, "q00");
    Line(h, 1050, "pAA");
    Line(h, 1060, "nH");
    Line(h, 1070, "m3");
    H_SkipCalls(h);
    Line(h, 1080, "x not a version");
    H_Advance(h, 2069);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 2070);
    H_Advance(h, 3070);
    CHECK(H_IsWriteText(H_NextCall(h), "\r\rm0\r", 3070) && h->npresence == 0);
    H_Destroy(h);

    /* The timer starts when the write is done */
    h = H_Create(&SDL_SerialMagellanModule);
    h->pend_writes = true;
    H_Start(h);
    H_Advance(h, 1000);
    H_Advance(h, 1500);
    H_CompleteWrite(h, true);
    H_Advance(h, 1699);
    CHECK(State(h)->step == SDL_MAGELLAN_STEP_MODE_OFF);
    H_Advance(h, 1700);
    CHECK(State(h)->step == SDL_MAGELLAN_STEP_ZERO1);
    H_Destroy(h);
}

static void TestNames(void)
{
    static const char spaceball[] = "vSPACEBALL 5000 Version 6.60";
    static const char cadman[] = "vCadMan Version 3.00";
    static const char other[] = "vSOMETHING";

    CHECK(strcmp(SDL_Magellan_NameFromVersion((const uint8_t *)version_text, strlen(version_text)), "Magellan SpaceMouse") == 0);
    CHECK(strcmp(SDL_Magellan_NameFromVersion((const uint8_t *)spaceball, strlen(spaceball)), "Spaceball 5000") == 0);
    CHECK(strcmp(SDL_Magellan_NameFromVersion((const uint8_t *)cadman, strlen(cadman)), "CadMan") == 0);
    CHECK(strcmp(SDL_Magellan_NameFromVersion((const uint8_t *)other, strlen(other)), "Magellan SpaceMouse") == 0);
}

/* A d line of six groups, the first given, the rest at rest */
static size_t BuildD(uint8_t *out, const char *first)
{
    size_t n = 0;
    int i;

    out[n++] = 'd';
    memcpy(out + n, first, 4);
    n += 4;
    for (i = 1; i < 6; ++i) {
        memcpy(out + n, "H000", 4);
        n += 4;
    }
    out[n++] = 0x0D;
    return n;
}

static uint8_t d_rest[26], d_plus1[26], d_minus1[26], d_max[26], d_min[26], d_invalid[26];
static const uint8_t k_key1[5] = { 0x6B, 0x41, 0x30, 0x30, 0x0D };
static const uint8_t k_key8[5] = { 0x6B, 0x30, 0x48, 0x30, 0x0D };
static const uint8_t k_star[5] = { 0x6B, 0x30, 0x30, 0x41, 0x0D };
static const uint8_t k_bit9[5] = { 0x6B, 0x30, 0x30, 0x42, 0x0D };
static uint8_t d_short[25];
static const uint8_t k_short[4] = { 0x6B, 0x41, 0x30, 0x0D };

static void BuildVectors(void)
{
    CHECK(BuildD(d_rest, "H000") == 26);
    BuildD(d_plus1, "H00A");
    BuildD(d_minus1, "G???");
    BuildD(d_max, "????");
    BuildD(d_min, "0000");
    BuildD(d_invalid, "H001");
    memcpy(d_short, d_rest, 24);
    d_short[24] = 0x0D;
}

static void TestDecode(void)
{
    Harness *h = PresentMagellan();
    int16_t axes[6];

    /* 3 */
    CHECK(d_rest[0] == 0x64 && d_rest[1] == 0x48 && d_rest[2] == 0x30 && d_rest[25] == 0x0D);
    H_Feed(h, d_plus1, 26);
    H_Feed(h, d_rest, 26);
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 5) == 0);

    /* 4 */
    H_Feed(h, d_plus1, 26);
    CHECK(H_Axis(h, 0, 0) == 1 && H_Axis(h, 0, 1) == 0);
    H_Feed(h, d_minus1, 26);
    CHECK(H_Axis(h, 0, 0) == -1);
    H_Feed(h, d_max, 26);
    CHECK(H_Axis(h, 0, 0) == 32767);
    H_Feed(h, d_min, 26);
    CHECK(H_Axis(h, 0, 0) == -32768);

    /* Each group in its place */
    {
        uint8_t line[26];
        int group;
        for (group = 0; group < 6; ++group) {
            memcpy(line, d_rest, 26);
            memcpy(line + 1 + 4 * group, "H00A", 4);
            CHECK(SDL_Magellan_DecodeAxes(line, 25, axes));
            CHECK(axes[group] == 1 && axes[(group + 1) % 6] == 0);
        }
    }

    /* 5 */
    {
        const int mark = h->npublished;
        H_Feed(h, d_invalid, 26);
        CHECK(h->npublished == mark && H_Axis(h, 0, 0) == -32768);
        CHECK(!SDL_Magellan_DecodeAxes(d_invalid, 25, axes));
    }

    /* 6 */
    H_Feed(h, k_key1, 5);
    CHECK(H_OnlyButton(h, 0, 0));
    H_Feed(h, k_key8, 5);
    CHECK(H_OnlyButton(h, 0, 7));
    H_Feed(h, k_star, 5);
    CHECK(H_OnlyButton(h, 0, 8));
    H_Feed(h, k_bit9, 5);
    CHECK(H_OnlyButton(h, 0, 9));
    {
        static const uint8_t k_all[5] = { 0x6B, 0x3F, 0x3F, 0x3F, 0x0D };
        static const int all[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1 };
        H_Feed(h, k_all, 5);
        CHECK(H_OnlyButtons(h, 0, all));
    }

    /* 8 */
    {
        const int mark = h->npublished;
        H_Feed(h, d_short, sizeof(d_short));
        H_Feed(h, k_short, sizeof(k_short));
        CHECK(h->npublished == mark);
    }

    /* Error lines are logged */
    H_Feed(h, (const uint8_t *)"\x65\x01\x0D\x65\x02\x0D", 6);
    CHECK(State(h)->errors == 2 && h->nlogs == 2 && strcmp(h->last_log, "Magellan reported a framing error") == 0);
    H_Destroy(h);
}

static void TestMouseMode(void)
{
    Harness *h = PresentMagellan();

    /* 7 */
    H_Feed(h, d_plus1, 26);
    H_Advance(h, 5000);
    H_Feed(h, (const uint8_t *)"\x6D\x48\x0D", 3);
    CHECK(H_IsWriteText(H_NextCall(h), "\r\rm0\r", 5000));
    CHECK(h->presence[0] == 1 && h->npresence == 1 && State(h)->reinits == 1);
    CHECK(H_Axis(h, 0, 0) == 1);
    /* Data still decodes while the start-up runs again */
    H_Feed(h, d_max, 26);
    CHECK(H_Axis(h, 0, 0) == 32767);
    Line(h, 5010, "m0");
    Line(h, 5020, "z");
    Line(h, 5030, "z");
    Line(h, 5040, "q00");
    Line(h, 5050, "pAA");
    Line(h, 5060, "nH");
    Line(h, 5070, "m3");
    Line(h, 5080, version_text);
    CHECK(State(h)->step == SDL_MAGELLAN_STEP_PRESENT && h->npresence == 1 && h->presence[0] == 1);
    /* An m line without the mouse bit starts nothing */
    H_SkipCalls(h);
    H_Feed(h, (const uint8_t *)"\x6D\x33\x0D", 3);
    CHECK(H_NextCall(h) == NULL && State(h)->reinits == 1);
    H_Destroy(h);
}

static void TestBatteryB(void)
{
    Harness *h = PresentMagellan();

    H_Feed(h, d_plus1, 26);
    H_Advance(h, 3000);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 3000));
    H_Advance(h, 4000);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 2, 4000));
    H_Advance(h, 4999);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 5000);
    CHECK(H_IsWriteText(H_NextCall(h), "\r\rm0\r", 5000));
    H_Advance(h, 5200);
    H_Advance(h, 5400);
    H_Advance(h, 5600);
    Line(h, 5610, "q00");
    Line(h, 5620, "pAA");
    Line(h, 5630, "nH");
    Line(h, 5640, "m3");
    CHECK(h->presence[0] == 0);
    Line(h, 5650, version_text);
    CHECK(h->presence[0] == 2 && H_Axis(h, 0, 0) == 0);
    H_Destroy(h);
}

static void TestBatteryA(void)
{
    Harness *h = PresentMagellan();
    H_Vector vectors[12];
    int n = 0, i;

    H_SetVector(&vectors[n++], "3 rest", d_rest, 26, 0, 0);
    H_SetVector(&vectors[n++], "4 X +1", d_plus1, 26, 1, 0);
    H_SetVector(&vectors[n++], "4 X -1", d_minus1, 26, 1, 0);
    H_SetVector(&vectors[n++], "4 X max", d_max, 26, 1, 0);
    H_SetVector(&vectors[n++], "4 X min", d_min, 26, 1, 0);
    H_SetVector(&vectors[n++], "6 key 1", k_key1, 5, 1, 0);
    H_SetVector(&vectors[n++], "6 key 8", k_key8, 5, 1, 0);
    H_SetVector(&vectors[n++], "6 key *", k_star, 5, 1, 0);
    H_SetVector(&vectors[n++], "6 bit 9", k_bit9, 5, 1, 0);
    for (i = 0; i < n; ++i) {
        H_BatteryA(h, &vectors[i], BringUp);
    }
    H_BatteryA6(h, "5 invalid character", d_invalid, 26, &vectors[1]);
    H_BatteryA6(h, "8 short d", d_short, sizeof(d_short), &vectors[1]);
    H_BatteryA6(h, "8 short k", k_short, sizeof(k_short), &vectors[5]);
    H_Destroy(h);
}

int main(void)
{
    BuildVectors();
    TestStartup();
    TestStartupFailure();
    TestNames();
    TestDecode();
    TestMouseMode();
    TestBatteryB();
    TestBatteryA();
    return H_Finish();
}
