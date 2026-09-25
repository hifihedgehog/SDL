/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_vrinsight_proto.c, the
   VRinsight panels of hifihedgehog/SDL#33 Part 5. Vectors 1 and 2 are the
   SerialFP2 capture. The others are constructed from the cited sources.
   Test numbers follow the ticket. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_vrinsight_proto.h"

static SDL_VRinsightState *State(Harness *h)
{
    return (SDL_VRinsightState *)h->state;
}

static const uint8_t connect[16] = { 0x43, 0x4D, 0x44, 0x52, 0x53, 0x54, 0x00, 0x00, 0x43, 0x4D, 0x44, 0x43, 0x4F, 0x4E, 0x00, 0x00 };
static const uint8_t con_reply[8] = { 0x43, 0x4D, 0x44, 0x43, 0x4F, 0x4E, 0x00, 0x88 };
static const uint8_t fun[8] = { 0x43, 0x4D, 0x44, 0x46, 0x55, 0x4E, 0x00, 0x00 };
static const uint8_t cdu2[8] = { 0x43, 0x4D, 0x44, 0x43, 0x44, 0x55, 0x32, 0x00 };
static const uint8_t ver[8] = { 0x43, 0x4D, 0x44, 0x56, 0x45, 0x52, 0x00, 0x00 };
static const uint8_t version[8] = { 0x43, 0x4D, 0x44, 0x31, 0x2E, 0x30, 0x30, 0x30 };
static const uint8_t fmer[8] = { 0x43, 0x4D, 0x44, 0x46, 0x4D, 0x45, 0x52, 0x00 };
static const uint8_t mcp2a[8] = { 0x43, 0x4D, 0x44, 0x4D, 0x43, 0x50, 0x32, 0x41 };
static const uint8_t mcp2b[8] = { 0x43, 0x4D, 0x44, 0x4D, 0x43, 0x50, 0x32, 0x42 };
static const uint8_t mpanl[8] = { 0x43, 0x4D, 0x44, 0x4D, 0x50, 0x61, 0x6E, 0x6C };
static const uint8_t keepalive[8] = { 0x43, 0x4D, 0x44, 0x43, 0x4F, 0x4E, 0x00, 0x00 };
static const uint8_t key_a[8] = { 0x4B, 0x45, 0x59, 0x41, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t lskl1[8] = { 0x4C, 0x53, 0x4B, 0x4C, 0x31, 0x00, 0x00, 0x00 };
static const uint8_t fun42[8] = { 0x46, 0x55, 0x4E, 0x34, 0x32, 0x00, 0x00, 0x00 };
static const uint8_t althld[8] = { 0x41, 0x4C, 0x54, 0x48, 0x4C, 0x44, 0x00, 0x00 };
static const uint8_t hdg_up[8] = { 0x48, 0x44, 0x47, 0x31, 0x32, 0x33, 0x2B, 0x00 };
static const uint8_t hdg_down[8] = { 0x48, 0x44, 0x47, 0x31, 0x32, 0x33, 0x00, 0x00 };
static const uint8_t spd_up[8] = { 0x53, 0x50, 0x44, 0x32, 0x35, 0x30, 0x2B, 0x00 };
static const uint8_t bar_boost[8] = { 0x42, 0x41, 0x52, 0x2B, 0x2B, 0x00, 0x00, 0x00 };
static const uint8_t adfsel1[8] = { 0x41, 0x44, 0x46, 0x53, 0x45, 0x4C, 0x31, 0x00 };
static const uint8_t adfsel2[8] = { 0x41, 0x44, 0x46, 0x53, 0x45, 0x4C, 0x32, 0x00 };
static const uint8_t com_value[8] = { 0x43, 0x4F, 0x4D, 0x73, 0x31, 0x32, 0x33, 0x34 };
static const uint8_t spd_value[8] = { 0x53, 0x50, 0x44, 0x32, 0x35, 0x30, 0x00, 0x00 };
static const uint8_t stray_key_a[9] = { 0x00, 0x4B, 0x45, 0x59, 0x41, 0x00, 0x00, 0x00, 0x00 };

/* A message from text, padded with 00 */
static void Message(const char *text, uint8_t *out)
{
    const size_t n = strlen(text);

    memset(out, 0, 8);
    memcpy(out, text, (n > 8) ? 8 : n);
}

static void BringUpWith(Harness *h, const uint8_t *panel)
{
    const uint64_t t = h->now;

    H_FeedAt(h, t + 10, con_reply, 8);
    H_FeedAt(h, t + 20, panel, 8);
    H_FeedAt(h, t + 30, version, 8);
}

static void BringUpCdu(Harness *h)
{
    BringUpWith(h, cdu2);
}

static void BringUpCombo(Harness *h)
{
    BringUpWith(h, fmer);
}

static Harness *Present(const uint8_t *panel)
{
    Harness *h = H_Create(&SDL_SerialVRinsightModule);

    H_Start(h);
    BringUpWith(h, panel);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialVRinsightModule);

    /* 1 */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), connect, 16, 0));
    H_FeedAt(h, 100, con_reply, 8);
    CHECK(H_IsWrite(H_NextCall(h), fun, 8, 100));
    CHECK(h->npresence == 0);
    H_FeedAt(h, 200, cdu2, 8);
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "VRinsight CDU II") == 0);
    CHECK(h->identity[0].nbuttons == 70 && h->identity[0].naxes == 0 && h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN);
    CHECK(H_IsWrite(H_NextCall(h), ver, 8, 200));

    /* 2 */
    {
        const int mark = h->npublished;
        H_FeedAt(h, 300, version, 8);
        CHECK(h->npublished == mark && strcmp(State(h)->version, "1.000") == 0);
    }
    H_Destroy(h);

    /* 3 */
    h = H_Create(&SDL_SerialVRinsightModule);
    H_Start(h);
    BringUpCombo(h);
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "VRinsight MCP Combo") == 0 && h->identity[0].nbuttons == 72);
    H_Destroy(h);
    {
        const uint8_t *others[3];
        int i;

        others[0] = mcp2a;
        others[1] = mcp2b;
        others[2] = mpanl;
        for (i = 0; i < 3; ++i) {
            h = H_Create(&SDL_SerialVRinsightModule);
            H_Start(h);
            H_FeedAt(h, 10, con_reply, 8);
            H_SkipCalls(h);
            H_FeedAt(h, 20, others[i], 8);
            CHECK(h->npresence == 0 && State(h)->step == SDL_VRINSIGHT_STEP_UNSUPPORTED && h->nlogs == 1);
            CHECK(H_IsWrite(H_NextCall(h), ver, 8, 20));
            /* Its messages press nothing and it gets no keep-alive */
            H_FeedAt(h, 30, key_a, 8);
            H_Advance(h, 120000);
            CHECK(h->npresence == 0 && h->npublished == 0 && H_NextCall(h) == NULL);
            H_Destroy(h);
        }
        CHECK(SDL_VRinsight_Panel(mcp2a) == SDL_VRINSIGHT_PANEL_COMBO2_AIRBUS);
        CHECK(SDL_VRinsight_Panel(mcp2b) == SDL_VRINSIGHT_PANEL_COMBO2_BOEING);
        CHECK(SDL_VRinsight_Panel(mpanl) == SDL_VRINSIGHT_PANEL_MPANEL);
        CHECK(SDL_VRinsight_Panel(key_a) == SDL_VRINSIGHT_PANEL_NONE);
    }

    /* No answer within 3 s restarts, from the write */
    h = H_Create(&SDL_SerialVRinsightModule);
    H_Start(h);
    H_SkipCalls(h);
    H_Advance(h, 2999);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 3000);
    CHECK(H_IsWrite(H_NextCall(h), connect, 16, 3000));
    H_FeedAt(h, 3100, con_reply, 8);
    CHECK(H_IsWrite(H_NextCall(h), fun, 8, 3100));
    H_Advance(h, 6100);
    CHECK(H_IsWrite(H_NextCall(h), connect, 16, 6100) && h->npresence == 0);
    H_Destroy(h);

    /* Buttons before identification press nothing, and a repeated CMDCON is not a name */
    h = H_Create(&SDL_SerialVRinsightModule);
    H_Start(h);
    H_FeedAt(h, 10, key_a, 8);
    H_FeedAt(h, 20, con_reply, 8);
    H_FeedAt(h, 30, con_reply, 8);
    CHECK(h->npresence == 0 && State(h)->step == SDL_VRINSIGHT_STEP_FUNCTION);
    H_FeedAt(h, 40, cdu2, 8);
    CHECK(h->presence[0] == 1);
    H_Destroy(h);
}

static void TestCdu(void)
{
    Harness *h = Present(cdu2);
    uint8_t m[8];
    int i;

    /* 4 */
    H_FeedAt(h, 1000, key_a, 8);
    CHECK(H_OnlyButton(h, 0, 22));
    H_Advance(h, 1099);
    CHECK(H_Button(h, 0, 22));
    H_Advance(h, 1100);
    CHECK(H_NoButtons(h, 0));
    H_Feed(h, lskl1, 8);
    CHECK(H_OnlyButton(h, 0, 0));
    H_Feed(h, fun42, 8);
    CHECK(H_Button(h, 0, 69));

    /* 5 */
    H_Advance(h, 2000);
    {
        const int mark = h->npublished;
        H_FeedAt(h, 2000, key_a, 8);
        CHECK(h->npublished == mark + 1 && SDL_Serial_GetButton(&h->published[mark].controls, 22));
        H_FeedAt(h, 2050, key_a, 8);
        CHECK(h->npublished == mark + 3);
        CHECK(!SDL_Serial_GetButton(&h->published[mark + 1].controls, 22));
        CHECK(SDL_Serial_GetButton(&h->published[mark + 2].controls, 22));
        H_Advance(h, 2149);
        CHECK(H_Button(h, 0, 22));
        H_Advance(h, 2150);
        CHECK(!H_Button(h, 0, 22) && h->npublished == mark + 4);
    }

    /* Every key name */
    {
        static const char *const names[70] = {
            "LSKL1", "LSKL2", "LSKL3", "LSKL4", "LSKL5", "LSKL6", "LSKR1", "LSKR2", "LSKR3", "LSKR4", "LSKR5", "LSKR6",
            "KEY0", "KEY1", "KEY2", "KEY3", "KEY4", "KEY5", "KEY6", "KEY7", "KEY8", "KEY9",
            "KEYA", "KEYB", "KEYC", "KEYD", "KEYE", "KEYF", "KEYG", "KEYH", "KEYI", "KEYJ", "KEYK", "KEYL", "KEYM",
            "KEYN", "KEYO", "KEYP", "KEYQ", "KEYR", "KEYS", "KEYT", "KEYU", "KEYV", "KEYW", "KEYX", "KEYY", "KEYZ",
            "KEY+", "KEY.", "KEY/", "KEYSP", "KEYCLR", "KEYDEL",
            "FUN11", "FUN12", "FUN13", "FUN14", "FUN15", "FUN16", "FUN21", "FUN22", "FUN23", "FUN24", "FUN25", "FUN26",
            "FUN31", "FUN32", "FUN41", "FUN42"
        };
        for (i = 0; i < 70; ++i) {
            Message(names[i], m);
            CHECK(SDL_VRinsight_CduButton(m) == i);
        }
        Message("KEY", m);
        CHECK(SDL_VRinsight_CduButton(m) == -1);
        Message("KEYAB", m);
        CHECK(SDL_VRinsight_CduButton(m) == -1);
        Message("FUN17", m);
        CHECK(SDL_VRinsight_CduButton(m) == -1);
    }

    /* 8 */
    H_Advance(h, 3000);
    H_Feed(h, stray_key_a, sizeof(stray_key_a));
    CHECK(H_OnlyButton(h, 0, 22) && State(h)->dropped == 1);
    H_Destroy(h);
}

static void TestCombo(void)
{
    Harness *h = Present(fmer);
    static const struct
    {
        const char *text;
        int button;
    } table[] = {
        { "ALTHLD", 0 }, { "ALTSEL", 1 }, { "ALT123+", 2 }, { "ALT123-", 3 },
        { "HDGHLD", 4 }, { "HDGHDG", 5 }, { "HDG123+", 6 }, { "HDG123-", 7 },
        { "SPDL", 8 }, { "SPDN1", 9 }, { "SPDSPD", 10 }, { "SPDSEL", 11 }, { "SPD250+", 12 }, { "SPD250-", 13 },
        { "VVS+", 14 }, { "VVS-", 15 },
        { "APLMAST+", 16 }, { "APLMAST-", 17 }, { "APLAT+", 18 }, { "APLAT-", 19 }, { "APLFD+", 20 }, { "APLFD-", 21 },
        { "APLCMDA", 22 }, { "APLCMDB", 23 }, { "APLCMDC", 24 }, { "APLCWSA", 25 }, { "APLCWSB", 26 }, { "APLTOGA", 27 },
        { "APLAPP+", 28 }, { "APLLNAV", 29 }, { "APLVNAV", 30 }, { "APLLOC-", 31 },
        { "VVSH", 32 },
        { "NDR-", 33 }, { "NDR+", 34 }, { "NDM+", 35 }, { "NDM-", 36 },
        { "EFIVOR1", 37 }, { "EFIADF1", 38 }, { "EFIVOR2", 39 }, { "EFIADF2", 40 }, { "EFIARPT", 41 }, { "EFIFPV", 42 },
        { "EFIWX", 43 }, { "EFISTA", 44 }, { "EFIWPT", 45 }, { "EFIDATA", 46 }, { "EFIPOS", 47 }, { "EFITERR", 48 },
        { "BAR+", 49 }, { "BAR-", 50 }, { "BARS", 51 }, { "EFIMTR", 52 },
        { "MIN+", 53 }, { "MIN-", 54 }, { "MINS", 55 }, { "NDMS", 56 }, { "NDRS", 57 },
        { "OBS+", 58 }, { "OBS-", 59 },
        { "COMSEL1", 60 }, { "COMSEL2", 61 }, { "COMAUX", 62 }, { "NAVSEL1", 63 }, { "NAVSEL2", 64 }, { "NAVAUX", 65 },
        { "ADFSEL1", 66 }, { "ADFSEL2", 67 }, { "DMESEL1", 68 }, { "DMESEL2", 69 }, { "TRNSEL", 70 }, { "TRNAUX", 71 },
        /* Boosted steps press the same button */
        { "BAR--", 50 }, { "MIN++", 53 }, { "VVS++", 14 },
        /* Values and unknown messages press nothing. All of ADFSEL3 is compared. */
        { "COMs1234", -1 }, { "COMx1234", -1 }, { "COMS1234", -1 }, { "COMX1234", -1 },
        { "NAVs1234", -1 }, { "NAVX1234", -1 }, { "TRNS1234", -1 }, { "TRNX1234", -1 },
        { "SPD250", -1 }, { "ADFSEL3", -1 }, { "XYZ", -1 }, { "EFIXX", -1 }, { "APLXX", -1 }
    };
    uint8_t m[8];
    size_t i;
    int covered[72];

    memset(covered, 0, sizeof(covered));
    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        Message(table[i].text, m);
        CHECK(SDL_VRinsight_ComboButton(m) == table[i].button);
        if (table[i].button >= 0) {
            covered[table[i].button] = 1;
        }
    }
    for (i = 0; i < 72; ++i) {
        CHECK(covered[i]);
    }

    /* 6 */
    H_FeedAt(h, 1000, althld, 8);
    CHECK(H_OnlyButton(h, 0, 0));
    H_FeedAt(h, 1200, hdg_up, 8);
    CHECK(H_OnlyButton(h, 0, 6));
    H_FeedAt(h, 1400, hdg_down, 8);
    CHECK(H_OnlyButton(h, 0, 7));
    H_FeedAt(h, 1600, spd_up, 8);
    CHECK(H_OnlyButton(h, 0, 12));
    H_FeedAt(h, 1800, bar_boost, 8);
    CHECK(H_OnlyButton(h, 0, 49));
    H_FeedAt(h, 2000, adfsel1, 8);
    CHECK(H_OnlyButton(h, 0, 66));
    H_FeedAt(h, 2200, adfsel2, 8);
    CHECK(H_OnlyButton(h, 0, 67));

    /* 7 */
    H_Advance(h, 2400);
    {
        const int mark = h->npublished;
        H_Feed(h, com_value, 8);
        H_Feed(h, spd_value, 8);
        CHECK(h->npublished == mark);
    }
    H_Destroy(h);
}

static void TestKeepAlive(void)
{
    Harness *h = Present(cdu2);

    /* 9: the last message was the version at 30 */
    H_Advance(h, 60029);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 60030);
    CHECK(H_IsWrite(H_NextCall(h), keepalive, 8, 60030));
    H_FeedAt(h, 61000, con_reply, 8);
    CHECK(h->presence[0] == 1 && State(h)->step == SDL_VRINSIGHT_STEP_PRESENT);
    /* A key press also counts as a message */
    H_FeedAt(h, 100000, key_a, 8);
    H_Advance(h, 159999);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 160000);
    CHECK(H_IsWrite(H_NextCall(h), keepalive, 8, 160000));
    /* No answer within 3 s: presence clears and the start-up runs again */
    H_Advance(h, 162999);
    CHECK(h->presence[0] == 1);
    H_Advance(h, 163000);
    CHECK(h->presence[0] == 0);
    CHECK(H_IsWrite(H_NextCall(h), connect, 16, 163000));
    BringUpCdu(h);
    CHECK(h->presence[0] == 2);
    H_Destroy(h);
}

static void TestOutput(void)
{
    Harness *h = Present(fmer);
    SDL_SerialOutput request;
    static const uint8_t speed[8] = { 'S', 'P', 'D', '2', '5', '0', 0, 0 };

    memset(&request, 0, sizeof(request));
    request.kind = SDL_SERIAL_OUTPUT_EFFECT;
    request.length = 8;
    memcpy(request.data, speed, 8);
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    CHECK(H_IsWrite(H_NextCall(h), speed, 8, h->now));
    request.length = 7;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    CHECK(H_NextCall(h) == NULL);
    CHECK(SDL_SerialVRinsightModule.effect_min == 8 && SDL_SerialVRinsightModule.effect_max == 8);
    H_Destroy(h);
}

static void TestBatteryB(void)
{
    Harness *h = Present(cdu2);

    H_FeedAt(h, 100, key_a, 8);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 100));
    H_Advance(h, 1100);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 1100));
    CHECK(H_IsWrite(H_NextCall(h), connect, 16, 1100));
    H_FeedAt(h, 1110, con_reply, 8);
    CHECK(h->presence[0] == 0);
    BringUpCdu(h);
    CHECK(h->presence[0] == 2 && H_NoButtons(h, 0));
    H_Destroy(h);
}

static void TestBatteryA(void)
{
    Harness *h = Present(cdu2);
    H_Vector vectors[16];
    int n = 0, i;

    /* Each message is a pulse, so the same message again is a release and a press */
    H_SetVector(&vectors[n++], "4 KEYA", key_a, 8, 1, 2);
    H_SetVector(&vectors[n++], "4 LSKL1", lskl1, 8, 1, 2);
    H_SetVector(&vectors[n++], "4 FUN42", fun42, 8, 1, 2);
    H_SetVector(&vectors[n++], "8 stray byte", stray_key_a, sizeof(stray_key_a), 1, 2);
    H_SetVector(&vectors[n++], "2 version", version, 8, 0, 0);
    for (i = 0; i < n; ++i) {
        H_BatteryA(h, &vectors[i], BringUpCdu);
    }
    H_Destroy(h);

    h = Present(fmer);
    n = 0;
    H_SetVector(&vectors[n++], "6 ALTHLD", althld, 8, 1, 2);
    H_SetVector(&vectors[n++], "6 HDG123+", hdg_up, 8, 1, 2);
    H_SetVector(&vectors[n++], "6 HDG123", hdg_down, 8, 1, 2);
    H_SetVector(&vectors[n++], "6 SPD250+", spd_up, 8, 1, 2);
    H_SetVector(&vectors[n++], "6 BAR++", bar_boost, 8, 1, 2);
    H_SetVector(&vectors[n++], "6 ADFSEL1", adfsel1, 8, 1, 2);
    H_SetVector(&vectors[n++], "6 ADFSEL2", adfsel2, 8, 1, 2);
    H_SetVector(&vectors[n++], "7 COMs1234", com_value, 8, 0, 0);
    H_SetVector(&vectors[n++], "7 SPD250", spd_value, 8, 0, 0);
    for (i = 0; i < n; ++i) {
        H_BatteryA(h, &vectors[i], BringUpCombo);
    }
    H_Destroy(h);
}

int main(void)
{
    TestStartup();
    TestCdu();
    TestCombo();
    TestKeepAlive();
    TestOutput();
    TestBatteryB();
    TestBatteryA();
    return H_Finish();
}
