/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_zhenhua_proto.c, the Zhen
   Hua RC transmitters of hifihedgehog/SDL#33 Part 5. The vectors are built
   from Linux's zhenhua driver and inputattach's check. Test numbers follow
   the ticket. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_zhenhua_proto.h"

static const uint8_t v1[10] = { 0xEF, 0xBE, 0xBE, 0xBE, 0xBE, 0xEF, 0xBE, 0xBE, 0xBE, 0xBE };
static const uint8_t v2[5] = { 0xEF, 0x4C, 0x13, 0xBE, 0xBE };
static const uint8_t v4[7] = { 0x00, 0x12, 0xEF, 0xBE, 0xBE, 0xBE, 0xBE };
static const uint8_t v4_moved[7] = { 0x00, 0x12, 0xEF, 0x4C, 0xBE, 0xBE, 0xBE };
static const uint8_t v5[8] = { 0xEF, 0xBE, 0xBE, 0xEF, 0xBE, 0xBE, 0xBE, 0xBE };
static const uint8_t v5_moved[8] = { 0xEF, 0x4C, 0xBE, 0xEF, 0x13, 0xBE, 0xBE, 0xBE };
static const uint8_t v6[5] = { 0xEF, 0x00, 0xBE, 0xBE, 0xBE };
static uint8_t v3[8][5];

static void BuildVectors(void)
{
    int channel;

    for (channel = 0; channel < 4; ++channel) {
        memcpy(v3[channel * 2], v2, 5);
        memcpy(v3[channel * 2 + 1], v2, 5);
        memset(v3[channel * 2] + 1, 0xBE, 4);
        memset(v3[channel * 2 + 1] + 1, 0xBE, 4);
        v3[channel * 2][1 + channel] = 0x4C;     /* 50 */
        v3[channel * 2 + 1][1 + channel] = 0x13; /* 200 */
    }
}

static void BringUp(Harness *h)
{
    H_Feed(h, v1, sizeof(v1));
}

static Harness *PresentTransmitter(void)
{
    Harness *h = H_Create(&SDL_SerialZhenHuaModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

static void TestReverse(void)
{
    CHECK(SDL_ZhenHua_Reverse(0xEF) == 0xF7);
    CHECK(SDL_ZhenHua_Reverse(0xBE) == 125);
    CHECK(SDL_ZhenHua_Reverse(0x4C) == 50);
    CHECK(SDL_ZhenHua_Reverse(0x13) == 200);
    CHECK(SDL_ZhenHua_Reverse(0x01) == 0x80 && SDL_ZhenHua_Reverse(0x80) == 0x01);
}

static void TestDetection(void)
{
    Harness *h = H_Create(&SDL_SerialZhenHuaModule);
    int i;

    /* 1 */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 19200, 8, SDL_SERIAL_NOPARITY, 1, 0));
    for (i = 0; i < 9; ++i) {
        H_FeedAt(h, 10 + (uint64_t)i, &v1[i], 1);
    }
    CHECK(h->npresence == 0);
    H_FeedAt(h, 19, &v1[9], 1);
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Zhen Hua RC transmitter") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN && h->identity[0].naxes == 4 && h->identity[0].nbuttons == 0);
    for (i = 0; i < 4; ++i) {
        CHECK(H_Axis(h, 0, i) == 0);
    }
    /* Receive only */
    CHECK(H_NextCall(h) == NULL);
    H_Destroy(h);

    /* A byte 500 ms late starts the detection over */
    h = H_Create(&SDL_SerialZhenHuaModule);
    H_Start(h);
    H_FeedAt(h, 10, v1, 7);
    H_FeedAt(h, 510, v1 + 7, 3);
    CHECK(h->npresence == 0);
    H_FeedAt(h, 520, v1, 10);
    CHECK(h->presence[0] == 1);
    H_Destroy(h);

    /* The second frame must start right after the first */
    h = H_Create(&SDL_SerialZhenHuaModule);
    H_Start(h);
    H_Feed(h, v1, 5);
    H_Feed(h, v1 + 1, 1);
    H_Feed(h, v1 + 5, 4);
    CHECK(h->npresence == 0);
    H_Feed(h, v1 + 9, 1);
    CHECK(h->npresence == 0);
    H_Feed(h, v1, 10);
    CHECK(h->presence[0] == 1);
    H_Destroy(h);

    /* Bytes before the first EF are ignored */
    h = H_Create(&SDL_SerialZhenHuaModule);
    H_Start(h);
    H_Feed(h, (const uint8_t *)"\x01\x02\x03\x04\x05\x06\x07", 7);
    H_Feed(h, v1, 10);
    CHECK(h->presence[0] == 1);
    H_Destroy(h);
}

static void TestFrames(void)
{
    Harness *h = PresentTransmitter();
    int channel;

    /* 2 */
    H_Feed(h, v2, 5);
    CHECK(H_Axis(h, 0, 0) == -32768 && H_Axis(h, 0, 1) == 32767 && H_Axis(h, 0, 2) == 0 && H_Axis(h, 0, 3) == 0);

    /* 3 */
    for (channel = 0; channel < 4; ++channel) {
        int other;

        H_Feed(h, v3[channel * 2], 5);
        CHECK(H_Axis(h, 0, channel) == -32768);
        H_Feed(h, v3[channel * 2 + 1], 5);
        CHECK(H_Axis(h, 0, channel) == 32767);
        for (other = 0; other < 4; ++other) {
            if (other != channel) {
                CHECK(H_Axis(h, 0, other) == 0);
            }
        }
    }

    /* 4 */
    H_Feed(h, v2, 5);
    H_Feed(h, v4, sizeof(v4));
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 1) == 0);
    H_Feed(h, v4_moved, sizeof(v4_moved));
    CHECK(H_Axis(h, 0, 0) == -32768);

    /* 5: only the second frame decodes */
    H_Feed(h, v1, 10);
    H_Feed(h, v5_moved, sizeof(v5_moved));
    CHECK(H_Axis(h, 0, 0) == 32767);

    /* 6 */
    H_Feed(h, v6, 5);
    CHECK(H_Axis(h, 0, 0) == -32768);
    {
        static const uint8_t over[5] = { 0xEF, 0xFF, 0xBE, 0xBE, 0xBE };
        H_Feed(h, over, 5);
        CHECK(H_Axis(h, 0, 0) == 32767);
    }
    H_Destroy(h);
}

static void TestSilence(void)
{
    Harness *h = PresentTransmitter();

    /* 7 */
    H_FeedAt(h, 100, v2, 5);
    H_Advance(h, 599);
    CHECK(h->presence[0] == 1);
    /* Bytes that complete no frame do not count */
    H_Feed(h, v2, 3);
    H_Advance(h, 600);
    CHECK(h->presence[0] == 0);
    H_FeedAt(h, 700, v1, 10);
    CHECK(h->presence[0] == 2 && H_Axis(h, 0, 0) == 0);
    H_Destroy(h);
}

static void TestBatteryB(void)
{
    Harness *h = PresentTransmitter();

    H_Feed(h, v2, 5);
    H_Advance(h, 100);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 100));
    H_Advance(h, 1100);
    CHECK(H_ExpectOpened(h, 19200, 8, SDL_SERIAL_NOPARITY, 1, 1100));
    H_Feed(h, v2, 5);
    CHECK(h->presence[0] == 0);
    H_Feed(h, v1, 10);
    CHECK(h->presence[0] == 2);
    CHECK(H_NextCall(h) == NULL);
    H_Destroy(h);
}

static void TestBatteryA(void)
{
    Harness *h = PresentTransmitter();
    H_Vector vectors[20];
    int n = 0, i;

    H_SetVector(&vectors[n++], "1 two frames", v1, sizeof(v1), 0, 0);
    H_SetVector(&vectors[n++], "2 50 and 200", v2, sizeof(v2), 1, 0);
    for (i = 0; i < 8; ++i) {
        static const char *labels[8] = { "3 ch1 50", "3 ch1 200", "3 ch2 50", "3 ch2 200", "3 ch3 50", "3 ch3 200", "3 ch4 50", "3 ch4 200" };
        H_SetVector(&vectors[n++], labels[i], v3[i], 5, 1, 0);
    }
    H_SetVector(&vectors[n++], "4 leading bytes", v4, sizeof(v4), 0, 0);
    H_SetVector(&vectors[n++], "4 leading bytes, moved", v4_moved, sizeof(v4_moved), 1, 0);
    H_SetVector(&vectors[n++], "5 cut frame", v5, sizeof(v5), 0, 0);
    H_SetVector(&vectors[n++], "5 cut frame, moved", v5_moved, sizeof(v5_moved), 1, 0);
    H_SetVector(&vectors[n++], "6 clamped", v6, sizeof(v6), 1, 0);
    for (i = 0; i < n; ++i) {
        H_BatteryA(h, &vectors[i], BringUp);
    }
    H_Destroy(h);
}

int main(void)
{
    BuildVectors();
    TestReverse();
    TestDetection();
    TestFrames();
    TestSilence();
    TestBatteryB();
    TestBatteryA();
    return H_Finish();
}
