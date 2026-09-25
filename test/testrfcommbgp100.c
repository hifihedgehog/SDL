/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/windows/SDL_rfcomm_bgp100_proto.c, the
   Chainpus BGP100 and the 2011 Phonejoy of hifihedgehog/SDL#33 Part 11.
   Test numbers follow the part's two sections. The pairs are built from the
   part's tables, since no capture exists. */

#include "testrfcommharness.h"

static RHarness *BGP100(void)
{
    return R_Up(&SDL_RFCOMMBGP100Module, "GAMEPAD");
}

static RHarness *Phonejoy(void)
{
    return R_Up(&SDL_RFCOMMPhonejoyModule, "Phonejoy");
}

static void Send(RHarness *r, const char *bytes, size_t length)
{
    R_Receive(r, (const uint8_t *)bytes, length);
}

#define BIT(button) (1u << (button))

static void TestModules(void)
{
    RHarness *r = R_Create(&SDL_RFCOMMBGP100Module, "GAMEPAD");

    /* Channel 1 and nothing sent */
    CHECK(!SDL_RFCOMMBGP100Module.has_service && !SDL_RFCOMMPhonejoyModule.has_service);
    CHECK(SDL_RFCOMMBGP100Module.family == SDL_RFCOMM_FAMILY_BGP100 && SDL_RFCOMMPhonejoyModule.family == SDL_RFCOMM_FAMILY_PHONEJOY);
    CHECK(R_IsConnect(R_Next(r), SDL_RFCOMM_CHANNEL, 0));
    R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    CHECK(R_Next(r) == NULL);
    R_Advance(r, 60000);
    CHECK(R_Next(r) == NULL && r->link.phase == SDL_RFCOMM_UP);
    R_Destroy(r);
}

/* BGP100 1 */
static void TestPairs(void)
{
    RHarness *r = BGP100();
    const SDL_SerialSnapshot *last;

    Send(r, "\xB6\x49\xF6\x09", 4);
    CHECK(r->nsnapshots == 2);
    CHECK(r->snapshots[0].present && SDL_Serial_GetButton(&r->snapshots[0].controls, SDL_BGP100_BUTTON_A));
    CHECK(R_Buttons(r) == 0);
    last = R_Last(r);
    CHECK(strcmp(last->identity.name, "Chainpus BGP100") == 0 && last->identity.type == SDL_SERIAL_TYPE_GAMEPAD);
    CHECK(last->identity.naxes == 0 && last->identity.nbuttons == SDL_BGP100_BUTTONS && last->identity.nhats == 0);
    CHECK(last->identity.has_mapping);
    CHECK(last->identity.mapping.a.target == SDL_BGP100_BUTTON_A && last->identity.mapping.b.target == SDL_BGP100_BUTTON_B);
    CHECK(last->identity.mapping.x.target == SDL_BGP100_BUTTON_D && last->identity.mapping.y.target == SDL_BGP100_BUTTON_C);
    CHECK(last->identity.mapping.leftshoulder.target == SDL_BGP100_BUTTON_L && last->identity.mapping.rightshoulder.target == SDL_BGP100_BUTTON_R);
    CHECK(last->identity.mapping.start.target == SDL_BGP100_BUTTON_START && last->identity.mapping.back.kind == SDL_SERIAL_MAP_NONE);
    CHECK(last->identity.mapping.dpup.target == SDL_BGP100_BUTTON_UP && last->identity.mapping.dpdown.target == SDL_BGP100_BUTTON_DOWN);
    CHECK(last->identity.mapping.dpleft.target == SDL_BGP100_BUTTON_LEFT && last->identity.mapping.dpright.target == SDL_BGP100_BUTTON_RIGHT);
    CHECK(last->identity.mapping.leftx.kind == SDL_SERIAL_MAP_NONE);
    R_Destroy(r);

    r = BGP100();
    Send(r, "\xB6\x49", 2);
    CHECK(R_Buttons(r) == BIT(SDL_BGP100_BUTTON_A));
    R_Advance(r, 60000);
    CHECK(R_Buttons(r) == BIT(SDL_BGP100_BUTTON_A));
    R_Destroy(r);

    /* 2 */
    r = BGP100();
    Send(r, "\xBA\x45\xBB\x44", 4);
    CHECK(R_Buttons(r) == (BIT(SDL_BGP100_BUTTON_UP) | BIT(SDL_BGP100_BUTTON_LEFT)));
    Send(r, "\xFA\x05\xFB\x04", 4);
    CHECK(R_Buttons(r) == 0 && r->nsnapshots == 4);
    R_Destroy(r);
}

/* BGP100 3, 4 and 6: what changes nothing */
static void TestRejects(void)
{
    RHarness *r = BGP100();
    int first, second;

    Send(r, "\x00\xB6\x49", 3);
    CHECK(r->nsnapshots == 1 && R_Buttons(r) == BIT(SDL_BGP100_BUTTON_A));
    R_Destroy(r);

    r = BGP100();
    Send(r, "\xB6\x4A", 2);
    Send(r, "\x36\xC9", 2);
    CHECK(r->nsnapshots == 0);
    /* The Phonejoy's stick frames are noise to the BGP100 */
    Send(r, "\xFF\x12\xFF\xFF\x11\x00", 6);
    CHECK(r->nsnapshots == 0);
    /* 4: keys 1, 3 and F */
    Send(r, "\xB1\x4E\xB3\x4C\xBF\x40", 6);
    CHECK(r->nsnapshots == 0);
    /* The keys outside 4 to E, and every high nibble but B and F, for every
       key: nothing is a pair */
    for (first = 0x80; first <= 0xFF; ++first) {
        const int key = first & 0x0F, event = first >> 4;
        uint8_t pair[2];

        if ((event == 0xB || event == 0xF) && key >= 4 && key <= 0xE) {
            continue;
        }
        pair[0] = (uint8_t)first;
        pair[1] = (uint8_t)(0xFF - first);
        R_Receive(r, pair, 2);
    }
    CHECK(r->nsnapshots == 0);
    R_Destroy(r);

    /* Every pair of bytes: only the 22 listed pairs act */
    {
        static SDL_BGP100State state;
        int accepted = 0;

        for (first = 0; first < 256; ++first) {
            for (second = 0; second < 256; ++second) {
                uint8_t pair[2];

                pair[0] = (uint8_t)first;
                pair[1] = (uint8_t)second;
                if (R_CountSnapshots(&SDL_RFCOMMBGP100Module, &state, pair, 2)) {
                    ++accepted;
                    CHECK(first + second == 0xFF && ((first >> 4) == 0xB || (first >> 4) == 0xF));
                    CHECK((first & 0x0F) >= 4 && (first & 0x0F) <= 0xE);
                }
            }
        }
        CHECK(accepted == 22);
    }

    /* 6: no joystick before the first valid pair, and a lost link with keys
       held releases them */
    r = BGP100();
    Send(r, "\x12\x34\x56", 3);
    CHECK(r->nsnapshots == 0);
    Send(r, "\xB6\x49\xB5\x4A\xBE\x41", 6);
    CHECK(R_Buttons(r) == (BIT(SDL_BGP100_BUTTON_A) | BIT(SDL_BGP100_BUTTON_B) | BIT(SDL_BGP100_BUTTON_D)));
    R_Lost(r);
    CHECK(!R_Present(r) && R_Buttons(r) == 0);
    R_Destroy(r);
}

/* BGP100 5: each key alone */
static void TestKeys(void)
{
    static const int keys[16] = {
        -1, -1, -1, -1,
        SDL_BGP100_BUTTON_START, SDL_BGP100_BUTTON_B, SDL_BGP100_BUTTON_A, SDL_BGP100_BUTTON_C,
        SDL_BGP100_BUTTON_L, SDL_BGP100_BUTTON_R, SDL_BGP100_BUTTON_UP, SDL_BGP100_BUTTON_LEFT,
        SDL_BGP100_BUTTON_RIGHT, SDL_BGP100_BUTTON_DOWN, SDL_BGP100_BUTTON_D, -1
    };
    int key;

    for (key = 4; key <= 0xE; ++key) {
        RHarness *r = BGP100();
        uint8_t pairs[4];

        pairs[0] = (uint8_t)(0xB0 + key);
        pairs[1] = (uint8_t)(0xFF - pairs[0]);
        pairs[2] = (uint8_t)(0xF0 + key);
        pairs[3] = (uint8_t)(0xFF - pairs[2]);
        R_Receive(r, pairs, 2);
        CHECK(R_Buttons(r) == BIT(keys[key]));
        R_Receive(r, pairs + 2, 2);
        CHECK(R_Buttons(r) == 0);
        R_Destroy(r);
    }
}

/* Phonejoy 1 and 2 */
static void TestPhonejoyKeys(void)
{
    RHarness *r = Phonejoy();
    const SDL_SerialSnapshot *last;

    Send(r, "\xB1\x4D", 2);
    CHECK(R_Present(r) && R_Axis(r, SDL_PHONEJOY_AXIS_L2) == 32767 && R_Axis(r, SDL_PHONEJOY_AXIS_R2) == -32768);
    last = R_Last(r);
    CHECK(strcmp(last->identity.name, "Phonejoy") == 0 && last->identity.naxes == SDL_PHONEJOY_AXES);
    CHECK(last->identity.nbuttons == SDL_PHONEJOY_BUTTONS && last->identity.has_mapping);
    CHECK(last->identity.mapping.a.target == SDL_PHONEJOY_BUTTON_3 && last->identity.mapping.b.target == SDL_PHONEJOY_BUTTON_2);
    CHECK(last->identity.mapping.x.target == SDL_PHONEJOY_BUTTON_4 && last->identity.mapping.y.target == SDL_PHONEJOY_BUTTON_1);
    CHECK(last->identity.mapping.back.target == SDL_PHONEJOY_BUTTON_SELECT && last->identity.mapping.start.target == SDL_PHONEJOY_BUTTON_START);
    CHECK(last->identity.mapping.leftshoulder.target == SDL_PHONEJOY_BUTTON_L1 && last->identity.mapping.rightshoulder.target == SDL_PHONEJOY_BUTTON_R1);
    CHECK(last->identity.mapping.lefttrigger.kind == SDL_SERIAL_MAP_AXIS && last->identity.mapping.lefttrigger.target == SDL_PHONEJOY_AXIS_L2);
    CHECK(last->identity.mapping.righttrigger.target == SDL_PHONEJOY_AXIS_R2);
    CHECK(last->identity.mapping.leftx.target == SDL_PHONEJOY_AXIS_LEFT_X && last->identity.mapping.lefty.target == SDL_PHONEJOY_AXIS_LEFT_Y);
    CHECK(last->identity.mapping.rightx.target == SDL_PHONEJOY_AXIS_RIGHT_X && last->identity.mapping.righty.target == SDL_PHONEJOY_AXIS_RIGHT_Y);
    CHECK(last->identity.mapping.dpup.target == SDL_PHONEJOY_BUTTON_UP && last->identity.mapping.dpdown.target == SDL_PHONEJOY_BUTTON_DOWN);
    CHECK(last->identity.mapping.dpleft.target == SDL_PHONEJOY_BUTTON_LEFT && last->identity.mapping.dpright.target == SDL_PHONEJOY_BUTTON_RIGHT);
    Send(r, "\xF1\x0D", 2);
    CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_L2) == -32768);
    Send(r, "\xB2\x4E", 2);
    CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_R2) == 32767);
    Send(r, "\xF2\x0E", 2);
    CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_R2) == -32768);
    Send(r, "\xB3\x4C", 2);
    CHECK(R_Buttons(r) == BIT(SDL_PHONEJOY_BUTTON_SELECT));
    Send(r, "\xF3\x0C", 2);
    CHECK(R_Buttons(r) == 0);
    R_Destroy(r);

    /* The triggers rest at the bottom from the first snapshot on */
    r = Phonejoy();
    Send(r, "\xB6\x49", 2);
    CHECK(r->nsnapshots == 1 && R_Axis(r, SDL_PHONEJOY_AXIS_L2) == -32768 && R_Axis(r, SDL_PHONEJOY_AXIS_R2) == -32768);
    R_Destroy(r);

    /* 2 */
    r = Phonejoy();
    Send(r, "\xB1\x4E\xB2\x4D", 4);
    CHECK(r->nsnapshots == 0);
    /* The releases of the BGP100 rule too */
    Send(r, "\xF1\x0E\xF2\x0D", 4);
    CHECK(r->nsnapshots == 0);
    R_Destroy(r);
}

/* Phonejoy 3, 4 and 5: the sticks */
static void TestPhonejoySticks(void)
{
    static const struct
    {
        uint8_t direction;
        int axis;
        int16_t value;
    } directions[8] = {
        { 1, SDL_PHONEJOY_AXIS_LEFT_Y, -32768 }, { 2, SDL_PHONEJOY_AXIS_LEFT_Y, 32767 },
        { 3, SDL_PHONEJOY_AXIS_LEFT_X, -32768 }, { 4, SDL_PHONEJOY_AXIS_LEFT_X, 32767 },
        { 5, SDL_PHONEJOY_AXIS_RIGHT_Y, -32768 }, { 6, SDL_PHONEJOY_AXIS_RIGHT_Y, 32767 },
        { 7, SDL_PHONEJOY_AXIS_RIGHT_X, -32768 }, { 8, SDL_PHONEJOY_AXIS_RIGHT_X, 32767 },
    };
    RHarness *r = Phonejoy();
    int i, value;

    Send(r, "\xFF\x12\xFF", 3);
    CHECK(R_Present(r) && R_Axis(r, SDL_PHONEJOY_AXIS_LEFT_X) == 32767);
    Send(r, "\xFF\x12\x7F", 3);
    CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_LEFT_X) == 0);
    Send(r, "\xFF\x11\x00", 3);
    CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_LEFT_Y) == -32768);
    Send(r, "\xFF\x14\x00", 3);
    CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_RIGHT_X) == -32768);
    Send(r, "\xFF\x13\xFE", 3);
    CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_RIGHT_Y) == 32767);
    R_Destroy(r);

    /* Every value: 7F centers, lower is up or left, 127 away is full */
    r = Phonejoy();
    for (value = 0; value < 256; ++value) {
        uint8_t frame[3];
        int position = value - 127;

        frame[0] = 0xFF;
        frame[1] = 0x12;
        frame[2] = (uint8_t)value;
        R_Receive(r, frame, 3);
        if (position > 127) {
            position = 127;
        }
        CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_LEFT_X) == ((position >= 0) ? position * 32767 / 127 : position * 32768 / 127));
    }
    R_Destroy(r);

    /* 4 */
    r = Phonejoy();
    Send(r, "\xA3\x23", 2);
    CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_LEFT_X) == -32768);
    Send(r, "\xE3\x13", 2);
    CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_LEFT_X) == 0);
    Send(r, "\xA8\x28", 2);
    CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_RIGHT_X) == 32767);
    R_Destroy(r);

    /* Each direction alone, and its release */
    for (i = 0; i < 8; ++i) {
        uint8_t pairs[4];
        int axis;

        r = Phonejoy();
        pairs[0] = (uint8_t)(0xA0 + directions[i].direction);
        pairs[1] = (uint8_t)(0x20 + directions[i].direction);
        pairs[2] = (uint8_t)(0xE0 + directions[i].direction);
        pairs[3] = (uint8_t)(0x10 + directions[i].direction);
        R_Receive(r, pairs, 2);
        for (axis = 0; axis < 4; ++axis) {
            CHECK(R_Axis(r, axis) == ((axis == directions[i].axis) ? directions[i].value : 0));
        }
        CHECK(R_Buttons(r) == 0);
        R_Receive(r, pairs + 2, 2);
        CHECK(R_Axis(r, directions[i].axis) == 0);
        R_Destroy(r);
    }

    /* Opposite directions held together center the axis, and the release
       of one leaves the other */
    r = Phonejoy();
    Send(r, "\xA1\x21\xA2\x22", 4);
    CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_LEFT_Y) == 0);
    Send(r, "\xE1\x11", 2);
    CHECK(R_Axis(r, SDL_PHONEJOY_AXIS_LEFT_Y) == 32767);
    R_Destroy(r);

    /* 5 */
    r = Phonejoy();
    Send(r, "\xFF\x15\x80", 3);
    CHECK(r->nsnapshots == 0);
    Send(r, "\xB6\x49", 2);
    CHECK(r->nsnapshots == 1 && R_Buttons(r) == BIT(SDL_PHONEJOY_BUTTON_1));
    R_Destroy(r);
    /* The same in one read, and FF 10 */
    r = Phonejoy();
    Send(r, "\xFF\x15\x80\xB6\x49\xFF\x10\x00", 8);
    CHECK(r->nsnapshots == 1 && R_Buttons(r) == BIT(SDL_PHONEJOY_BUTTON_1) && R_Axis(r, SDL_PHONEJOY_AXIS_LEFT_Y) == 0);
    R_Destroy(r);
    /* Direction pairs of 0 and 9 are not listed */
    r = Phonejoy();
    Send(r, "\xA0\x20\xA9\x29\xE0\x10\xE9\x19", 8);
    CHECK(r->nsnapshots == 0);
    R_Destroy(r);
}

/* Phonejoy 6: the BGP100 keys */
static void TestPhonejoyBGP100Keys(void)
{
    static const int keys[16] = {
        -1, -1, -1, SDL_PHONEJOY_BUTTON_SELECT,
        SDL_PHONEJOY_BUTTON_START, SDL_PHONEJOY_BUTTON_2, SDL_PHONEJOY_BUTTON_1, SDL_PHONEJOY_BUTTON_4,
        SDL_PHONEJOY_BUTTON_L1, SDL_PHONEJOY_BUTTON_R1, SDL_PHONEJOY_BUTTON_UP, SDL_PHONEJOY_BUTTON_LEFT,
        SDL_PHONEJOY_BUTTON_RIGHT, SDL_PHONEJOY_BUTTON_DOWN, SDL_PHONEJOY_BUTTON_3, -1
    };
    RHarness *r;
    int key;

    for (key = 3; key <= 0xE; ++key) {
        uint8_t pairs[4];

        r = Phonejoy();
        pairs[0] = (uint8_t)(0xB0 + key);
        pairs[1] = (uint8_t)(0xFF - pairs[0]);
        pairs[2] = (uint8_t)(0xF0 + key);
        pairs[3] = (uint8_t)(0xFF - pairs[2]);
        R_Receive(r, pairs, 2);
        CHECK(R_Buttons(r) == BIT(keys[key]));
        R_Receive(r, pairs + 2, 2);
        CHECK(R_Buttons(r) == 0);
        R_Destroy(r);
    }
    /* The BGP100's tests 1 to 3 */
    r = Phonejoy();
    Send(r, "\xB6\x49\xF6\x09", 4);
    CHECK(r->nsnapshots == 2 && R_Buttons(r) == 0);
    Send(r, "\xBA\x45\xBB\x44", 4);
    CHECK(R_Buttons(r) == (BIT(SDL_PHONEJOY_BUTTON_UP) | BIT(SDL_PHONEJOY_BUTTON_LEFT)));
    Send(r, "\xFA\x05\xFB\x04", 4);
    CHECK(R_Buttons(r) == 0);
    Send(r, "\x00\xB6\x49", 3);
    CHECK(R_Buttons(r) == BIT(SDL_PHONEJOY_BUTTON_1));
    Send(r, "\xB6\x4A\x36\xC9\xBF\x40\xB0\x4F", 8);
    CHECK(R_Buttons(r) == BIT(SDL_PHONEJOY_BUTTON_1));
    R_Destroy(r);

    /* The BGP100 ignores B3 4C */
    r = BGP100();
    Send(r, "\xB3\x4C", 2);
    CHECK(r->nsnapshots == 0);
    R_Destroy(r);

    /* Every pair of bytes: exactly the listed ones act, and FF never acts
       without its third byte */
    {
        static SDL_BGP100State state;
        int first, second, accepted = 0;

        for (first = 0; first < 256; ++first) {
            for (second = 0; second < 256; ++second) {
                uint8_t pair[2];

                pair[0] = (uint8_t)first;
                pair[1] = (uint8_t)second;
                if (R_CountSnapshots(&SDL_RFCOMMPhonejoyModule, &state, pair, 2)) {
                    ++accepted;
                    CHECK(first != 0xFF);
                }
            }
        }
        /* Keys 1 to E pressed and released, and eight directions both ways */
        CHECK(accepted == 14 * 2 + 8 * 2);
    }
}

/* Phonejoy 7 and every split */
static void TestPhonejoySplits(void)
{
    static const uint8_t stream[] = {
        0xB1, 0x4D, 0xB2, 0x4E, 0xB3, 0x4C, 0xF1, 0x0D, 0xF2, 0x0E, 0xF3, 0x0C,
        0xFF, 0x12, 0xFF, 0xFF, 0x12, 0x7F, 0xFF, 0x11, 0x00, 0xFF, 0x14, 0x00,
        0xA3, 0x23, 0xE3, 0x13, 0xA8, 0x28,
        0xFF, 0x15, 0x80, 0xB6, 0x49
    };
    static const uint8_t bgp100[] = { 0x00, 0xB6, 0x49, 0xBA, 0x45, 0xBB, 0x44, 0xB6, 0x4A, 0xFA, 0x05, 0xFB, 0x04, 0xF6, 0x09 };
    RHarness *r;

    R_CheckSplits(&SDL_RFCOMMPhonejoyModule, "Phonejoy", stream, sizeof(stream));
    R_CheckSplits(&SDL_RFCOMMBGP100Module, "GAMEPAD", bgp100, sizeof(bgp100));

    /* A lone FF 12 or B1 changes nothing until its last byte */
    r = Phonejoy();
    Send(r, "\xFF\x12", 2);
    CHECK(r->nsnapshots == 0);
    Send(r, "\xFF", 1);
    CHECK(r->nsnapshots == 1 && R_Axis(r, SDL_PHONEJOY_AXIS_LEFT_X) == 32767);
    Send(r, "\xB1", 1);
    CHECK(r->nsnapshots == 1);
    Send(r, "\x4D", 1);
    CHECK(r->nsnapshots == 2 && R_Axis(r, SDL_PHONEJOY_AXIS_L2) == 32767);
    /* A lost link releases every control and centers the sticks */
    Send(r, "\xA5\x25\xB6\x49", 4);
    R_Lost(r);
    CHECK(!R_Present(r) && R_Buttons(r) == 0 && R_Axis(r, SDL_PHONEJOY_AXIS_RIGHT_Y) == 0);
    R_Destroy(r);
}

int main(void)
{
    TestModules();
    TestPairs();
    TestRejects();
    TestKeys();
    TestPhonejoyKeys();
    TestPhonejoySticks();
    TestPhonejoyBGP100Keys();
    TestPhonejoySplits();
    return H_Finish();
}
