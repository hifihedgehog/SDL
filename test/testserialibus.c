/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_ibus_proto.c, the FlySky
   i-BUS of hifihedgehog/SDL#33 Part 5. The vectors are constructed from the
   frame layout. Test numbers follow the ticket. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_ibus_proto.h"

static uint8_t frame1[32], frame_ch1_min[32], frame_ch1_max[32], frame_ch14_min[32], frame_bad[32], frame_masked[32];
static uint8_t frame_skipped[35];

/* A frame with every channel given and its checksum computed */
static void Build(uint8_t *frame, const uint16_t *channels)
{
    uint16_t checksum = 0xFFFF;
    int i;

    frame[0] = 0x20;
    frame[1] = 0x40;
    for (i = 0; i < 14; ++i) {
        frame[2 + 2 * i] = (uint8_t)(channels[i] & 0xFF);
        frame[3 + 2 * i] = (uint8_t)(channels[i] >> 8);
    }
    for (i = 0; i < 30; ++i) {
        checksum = (uint16_t)(checksum - frame[i]);
    }
    frame[30] = (uint8_t)(checksum & 0xFF);
    frame[31] = (uint8_t)(checksum >> 8);
}

static void BuildVectors(void)
{
    uint16_t channels[14];
    int i;

    for (i = 0; i < 14; ++i) {
        channels[i] = 1500;
    }
    Build(frame1, channels);
    channels[0] = 1000;
    Build(frame_ch1_min, channels);
    channels[0] = 2000;
    Build(frame_ch1_max, channels);
    channels[0] = 1500;
    channels[13] = 1000;
    Build(frame_ch14_min, channels);
    channels[13] = 1500;
    channels[0] = 0xF5DC; /* 1500 below the high nibble */
    Build(frame_masked, channels);
    memcpy(frame_bad, frame1, 32);
    frame_bad[31] = 0xF4;
    frame_skipped[0] = 0x00;
    frame_skipped[1] = 0xFF;
    frame_skipped[2] = 0x13;
    memcpy(frame_skipped + 3, frame1, 32);
}

static void BringUp(Harness *h)
{
    H_Feed(h, frame1, 32);
}

static Harness *PresentReceiver(void)
{
    Harness *h = H_Create(&SDL_SerialIBusModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

static void TestFrames(void)
{
    Harness *h = H_Create(&SDL_SerialIBusModule);
    uint16_t channels[14];
    int i;

    /* The ticket's bytes */
    {
        static const uint8_t ticket[4] = { 0x51, 0xF3, 0x47, 0xF3 };
        CHECK(frame1[0] == 0x20 && frame1[1] == 0x40 && frame1[2] == 0xDC && frame1[3] == 0x05);
        CHECK(frame1[30] == ticket[0] && frame1[31] == ticket[1]);
        CHECK(frame_ch1_min[2] == 0xE8 && frame_ch1_min[3] == 0x03 && frame_ch1_min[30] == ticket[2] && frame_ch1_min[31] == ticket[3]);
        CHECK(frame_ch1_max[2] == 0xD0 && frame_ch1_max[3] == 0x07 && frame_ch1_max[30] == 0x5B && frame_ch1_max[31] == 0xF3);
        CHECK(frame_ch14_min[28] == 0xE8 && frame_ch14_min[29] == 0x03 && frame_ch14_min[30] == 0x47 && frame_ch14_min[31] == 0xF3);
    }

    /* 1 */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 0));
    H_Feed(h, frame1, 31);
    CHECK(h->npresence == 0);
    H_Feed(h, frame1 + 31, 1);
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "FlySky FS-iA6B") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN && h->identity[0].naxes == 14 && h->identity[0].nbuttons == 0);
    for (i = 0; i < 14; ++i) {
        CHECK(H_Axis(h, 0, i) == 0);
    }
    CHECK(H_NextCall(h) == NULL);

    /* 2 */
    H_Feed(h, frame_ch1_min, 32);
    CHECK(H_Axis(h, 0, 0) == -32768 && H_Axis(h, 0, 1) == 0);
    H_Feed(h, frame_ch1_max, 32);
    CHECK(H_Axis(h, 0, 0) == 32767);
    H_Feed(h, frame_ch14_min, 32);
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 13) == -32768);

    /* 3 */
    H_Feed(h, frame_ch1_min, 32);
    {
        const int mark = h->npublished;
        H_Feed(h, frame_bad, 32);
        CHECK(h->npublished == mark && H_Axis(h, 0, 0) == -32768);
    }
    H_Feed(h, frame1, 32);
    CHECK(H_Axis(h, 0, 0) == 0);

    /* 4 */
    H_Feed(h, frame_ch1_max, 32);
    H_Feed(h, frame_skipped, sizeof(frame_skipped));
    CHECK(H_Axis(h, 0, 0) == 0);

    /* Channels are masked to 12 bits */
    CHECK(SDL_IBus_DecodeFrame(frame_masked, 32, channels) && channels[0] == 1500);
    H_Feed(h, frame_ch1_max, 32);
    H_Feed(h, frame_masked, 32);
    CHECK(H_Axis(h, 0, 0) == 0);

    /* A frame that began inside a frame cut short is found at once */
    H_Feed(h, frame_ch1_max, 32);
    H_Feed(h, frame1, 4);
    H_Feed(h, frame_ch1_min, 32);
    CHECK(H_Axis(h, 0, 0) == -32768);

    /* Clamped outside 1000 to 2000 */
    {
        uint16_t low[14], high[14];
        uint8_t f[32];
        for (i = 0; i < 14; ++i) {
            low[i] = 1500;
            high[i] = 1500;
        }
        low[3] = 900;
        high[3] = 2100;
        Build(f, low);
        H_Feed(h, f, 32);
        CHECK(H_Axis(h, 0, 3) == -32768);
        Build(f, high);
        H_Feed(h, f, 32);
        CHECK(H_Axis(h, 0, 3) == 32767);
    }
    CHECK(!SDL_IBus_DecodeFrame(frame_bad, 32, channels));
    CHECK(!SDL_IBus_DecodeFrame(frame1, 31, channels));

    /* A frame with a command other than 40 is no frame, however its checksum reads */
    {
        uint8_t f[32];
        uint16_t checksum = 0xFFFF;
        const int mark = h->npublished;

        memcpy(f, frame_ch1_min, 32);
        f[1] = 0x41;
        for (i = 0; i < 30; ++i) {
            checksum = (uint16_t)(checksum - f[i]);
        }
        f[30] = (uint8_t)(checksum & 0xFF);
        f[31] = (uint8_t)(checksum >> 8);
        CHECK(!SDL_IBus_DecodeFrame(f, 32, channels));
        H_Feed(h, f, 32);
        CHECK(h->npublished == mark);
    }
    H_Destroy(h);
}

static void TestSilence(void)
{
    Harness *h = PresentReceiver();

    /* 5 */
    H_FeedAt(h, 100, frame_ch1_min, 32);
    H_Advance(h, 599);
    CHECK(h->presence[0] == 1);
    /* A bad frame does not count */
    H_Feed(h, frame_bad, 32);
    H_Advance(h, 600);
    CHECK(h->presence[0] == 0);
    H_FeedAt(h, 900, frame1, 32);
    CHECK(h->presence[0] == 2 && H_Axis(h, 0, 0) == 0);
    H_Destroy(h);
}

static void TestBatteryB(void)
{
    Harness *h = PresentReceiver();

    H_Feed(h, frame_ch1_min, 32);
    H_Advance(h, 100);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 100));
    H_Advance(h, 1100);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 1100));
    CHECK(h->presence[0] == 0);
    H_Feed(h, frame1, 32);
    CHECK(h->presence[0] == 2);
    CHECK(H_NextCall(h) == NULL);
    H_Destroy(h);
}

static void TestBatteryA(void)
{
    Harness *h = PresentReceiver();
    H_Vector vectors[8];
    int n = 0, i;

    H_SetVector(&vectors[n++], "1 rest", frame1, 32, 0, 0);
    H_SetVector(&vectors[n++], "2 ch1 1000", frame_ch1_min, 32, 1, 0);
    H_SetVector(&vectors[n++], "2 ch1 2000", frame_ch1_max, 32, 1, 0);
    H_SetVector(&vectors[n++], "2 ch14 1000", frame_ch14_min, 32, 1, 0);
    H_SetVector(&vectors[n++], "4 leading bytes", frame_skipped, sizeof(frame_skipped), 0, 0);
    for (i = 0; i < n; ++i) {
        H_BatteryA(h, &vectors[i], BringUp);
    }
    H_BatteryA6(h, "3 bad checksum", frame_bad, 32, &vectors[1]);
    H_Destroy(h);
}

int main(void)
{
    BuildVectors();
    TestFrames();
    TestSilence();
    TestBatteryB();
    TestBatteryA();
    return H_Finish();
}
