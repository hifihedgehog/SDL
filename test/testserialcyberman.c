/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_cyberman_proto.c, the
   Logitech CyberMan of hifihedgehog/SDL#33 Part 5. The vectors are built
   from pages 5, 12, 16 and 19-23 of the CyberMan 3D SWIFT Supplement 1.0
   and its errata. Test numbers follow the ticket. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_cyberman_proto.h"

static SDL_CyberManState *State(Harness *h)
{
    return (SDL_CyberManState *)h->state;
}

/* Version 1.50, X and Y absolute with 8 bits, the rest absolute, centering, 2 bits */
static const uint8_t static_report[12] = { 0xA0, 0x01, 0x32, 0x48, 0x48, 0x62, 0x62, 0x62, 0x62, 0x00, 0x00, 0x00 };

/* From a reset or an open at time t: RTS low at t, high at t + 200, M3,
   the switch at t + 300, the new rate at t + 320, the static report */
static void BringUp(Harness *h)
{
    const uint64_t t = h->now;

    H_FeedAt(h, t + 260, (const uint8_t *)"M", 1);
    H_FeedAt(h, t + 300, (const uint8_t *)"3", 1);
    H_Advance(h, t + 320);
    H_FeedAt(h, t + 400, static_report, sizeof(static_report));
}

static Harness *PresentCyberMan(void)
{
    Harness *h = H_Create(&SDL_SerialCyberManModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialCyberManModule);

    /* 1 */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 1200, 7, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETDTR, 0));
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRRTS, 0));
    H_Advance(h, 199);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 200);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETDTR, 200));
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETRTS, 200));
    H_FeedAt(h, 260, (const uint8_t *)"\x4D", 1);
    CHECK(H_NextCall(h) == NULL);
    H_FeedAt(h, 300, (const uint8_t *)"\x33", 1);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x2A\x53", 2, 300));
    CHECK(H_IsCall(H_NextCall(h), 'D', 300));
    H_Advance(h, 319);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 320);
    CHECK(H_IsPlainLine(H_NextCall(h), 4800, 8, SDL_SERIAL_NOPARITY, 1, 320));
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x21\x53", 2, 320));
    CHECK(h->npresence == 0);
    /* The static report is 12 bytes: 11 present nothing */
    H_FeedAt(h, 400, static_report, 11);
    CHECK(h->npresence == 0);
    H_Feed(h, static_report + 11, 1);
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Logitech CyberMan") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN && h->identity[0].naxes == 6 && h->identity[0].nbuttons == 3);
    CHECK(State(h)->version_major == 1 && State(h)->version_minor == 50);
    CHECK(SDL_SerialCyberManModule.rumble);
    CHECK(H_NextCall(h) == NULL);
    /* Reports arrive on change only, so no silence limit */
    H_Advance(h, 60400);
    CHECK(h->presence[0] == 1 && H_NextCall(h) == NULL);
    H_Destroy(h);

    /* The drain is timed from its completion, and the new rate waits 20 ms after it */
    h = H_Create(&SDL_SerialCyberManModule);
    h->pend_writes = true;
    H_Start(h);
    H_Advance(h, 200);
    H_FeedAt(h, 260, (const uint8_t *)"M3", 2);
    H_SkipCalls(h);
    H_Advance(h, 500);
    CHECK(H_NextCall(h) == NULL);
    H_CompleteWrite(h, true);
    CHECK(H_IsCall(H_NextCall(h), 'D', 500));
    H_Advance(h, 519);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 520);
    CHECK(H_IsPlainLine(H_NextCall(h), 4800, 8, SDL_SERIAL_NOPARITY, 1, 520));
    H_Destroy(h);
}

static void TestStartupFailures(void)
{
    Harness *h = H_Create(&SDL_SerialCyberManModule);

    /* 2 */
    H_Start(h);
    H_Advance(h, 200);
    H_SkipCalls(h);
    H_Advance(h, 309);
    CHECK(State(h)->step == SDL_CYBERMAN_STEP_WAIT_M);
    H_Advance(h, 310);
    CHECK(State(h)->step == SDL_CYBERMAN_STEP_RETRY && h->npresence == 0);
    H_Advance(h, 1309);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1310);
    CHECK(H_IsPlainLine(H_NextCall(h), 1200, 7, SDL_SERIAL_NOPARITY, 1, 1310));
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETDTR, 1310));
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRRTS, 1310));
    H_Advance(h, 1510);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETDTR, 1510));
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETRTS, 1510));
    H_Destroy(h);

    /* Bytes other than M while waiting for it are ignored. A 3 later than 100 ms fails. */
    h = H_Create(&SDL_SerialCyberManModule);
    H_Start(h);
    H_Advance(h, 200);
    H_FeedAt(h, 210, (const uint8_t *)"\x00\x7F", 2);
    H_FeedAt(h, 250, (const uint8_t *)"M", 1);
    CHECK(State(h)->step == SDL_CYBERMAN_STEP_WAIT_3);
    H_Advance(h, 350);
    CHECK(State(h)->step == SDL_CYBERMAN_STEP_RETRY);
    H_Destroy(h);

    /* A byte other than 3 after the M: a mouse that is not a CyberMan */
    h = H_Create(&SDL_SerialCyberManModule);
    H_Start(h);
    H_Advance(h, 200);
    H_FeedAt(h, 250, (const uint8_t *)"MZ", 2);
    CHECK(State(h)->step == SDL_CYBERMAN_STEP_RETRY);
    H_Destroy(h);

    /* No static report within 500 ms of !S, written at 280 */
    h = H_Create(&SDL_SerialCyberManModule);
    H_Start(h);
    H_Advance(h, 200);
    H_FeedAt(h, 260, (const uint8_t *)"M3", 2);
    H_Advance(h, 779);
    CHECK(State(h)->step == SDL_CYBERMAN_STEP_STATIC);
    H_Advance(h, 780);
    CHECK(State(h)->step == SDL_CYBERMAN_STEP_RETRY && h->npresence == 0);
    /* The retry goes back to 1200 7N1 before the reset pulse */
    H_SkipCalls(h);
    H_Advance(h, 1780);
    CHECK(H_IsPlainLine(H_NextCall(h), 1200, 7, SDL_SERIAL_NOPARITY, 1, 1780));
    H_Destroy(h);

    /* A 3D report before the static report does not present the device */
    h = H_Create(&SDL_SerialCyberManModule);
    H_Start(h);
    H_Advance(h, 200);
    H_FeedAt(h, 260, (const uint8_t *)"M3", 2);
    H_FeedAt(h, 330, (const uint8_t *)"\x80\x00\x00\x00\x00", 5);
    CHECK(h->npresence == 0 && State(h)->step == SDL_CYBERMAN_STEP_STATIC);
    H_Destroy(h);
}

static const uint8_t r_rest[5] = { 0x80, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t r_x_max[5] = { 0x80, 0x3F, 0x40, 0x00, 0x00 };
static const uint8_t r_x_min[5] = { 0x80, 0x40, 0x00, 0x00, 0x00 };
static const uint8_t r_y_max[5] = { 0x80, 0x00, 0x1F, 0x60, 0x00 };
static const uint8_t r_y_minus1[5] = { 0x80, 0x00, 0x3F, 0x60, 0x00 };
static const uint8_t r_z_plus1[5] = { 0x80, 0x00, 0x00, 0x08, 0x00 };
static const uint8_t r_z_minus2[5] = { 0x80, 0x00, 0x00, 0x10, 0x00 };
static const uint8_t r_pitch_plus1[5] = { 0x80, 0x00, 0x00, 0x02, 0x00 };
static const uint8_t r_pitch_minus2[5] = { 0x80, 0x00, 0x00, 0x04, 0x00 };
static const uint8_t r_roll_plus1[5] = { 0x80, 0x00, 0x00, 0x00, 0x40 };
static const uint8_t r_roll_minus2[5] = { 0x80, 0x00, 0x00, 0x01, 0x00 };
static const uint8_t r_yaw_plus1[5] = { 0x80, 0x00, 0x00, 0x00, 0x10 };
static const uint8_t r_yaw_minus2[5] = { 0x80, 0x00, 0x00, 0x00, 0x20 };
static const uint8_t r_left[5] = { 0x84, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t r_middle[5] = { 0x82, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t r_right[5] = { 0x81, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t r_power[4] = { 0xC1, 0x00, 0x00, 0x00 };
static const uint8_t r_z_minus1[5] = { 0x80, 0x00, 0x00, 0x18, 0x00 };
static const uint8_t r_cut[7] = { 0x80, 0x3F, 0x80, 0x00, 0x00, 0x00, 0x00 };

static void CheckRaw(const uint8_t *r, int x, int y, int z, int pitch, int roll, int yaw, uint8_t buttons)
{
    int axes[6];
    uint8_t b = 0xA5;

    CHECK(SDL_CyberMan_Decode3D(r, 5, axes, &b));
    CHECK(axes[0] == x && axes[1] == y && axes[2] == z && axes[3] == pitch && axes[4] == roll && axes[5] == yaw);
    CHECK(b == buttons);
}

static void TestReports(void)
{
    Harness *h = PresentCyberMan();

    /* 3 */
    CheckRaw(r_rest, 0, 0, 0, 0, 0, 0, 0);
    H_Feed(h, r_x_max, 5);
    H_Feed(h, r_rest, 5);
    CHECK(H_Axis(h, 0, 0) == 0 && H_NoButtons(h, 0));

    /* 4 */
    CheckRaw(r_x_max, 127, 0, 0, 0, 0, 0, 0);
    CheckRaw(r_x_min, -128, 0, 0, 0, 0, 0, 0);
    CheckRaw(r_y_max, 0, 127, 0, 0, 0, 0, 0);
    CheckRaw(r_y_minus1, 0, -1, 0, 0, 0, 0, 0);
    H_Feed(h, r_x_max, 5);
    CHECK(H_Axis(h, 0, 0) == 32767);
    H_Feed(h, r_x_min, 5);
    CHECK(H_Axis(h, 0, 0) == -32768);
    H_Feed(h, r_y_max, 5);
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 1) == 32767);
    H_Feed(h, r_y_minus1, 5);
    CHECK(H_Axis(h, 0, 1) == -256);

    /* 5 */
    CheckRaw(r_z_plus1, 0, 0, 1, 0, 0, 0, 0);
    CheckRaw(r_z_minus2, 0, 0, -2, 0, 0, 0, 0);
    CheckRaw(r_pitch_plus1, 0, 0, 0, 1, 0, 0, 0);
    CheckRaw(r_pitch_minus2, 0, 0, 0, -2, 0, 0, 0);
    CheckRaw(r_roll_plus1, 0, 0, 0, 0, 1, 0, 0);
    CheckRaw(r_roll_minus2, 0, 0, 0, 0, -2, 0, 0);
    CheckRaw(r_yaw_plus1, 0, 0, 0, 0, 0, 1, 0);
    CheckRaw(r_yaw_minus2, 0, 0, 0, 0, 0, -2, 0);
    H_Feed(h, r_z_plus1, 5);
    CHECK(H_Axis(h, 0, 2) == 32767 && H_Axis(h, 0, 1) == 0);
    H_Feed(h, r_z_minus2, 5);
    CHECK(H_Axis(h, 0, 2) == -32768);
    H_Feed(h, r_roll_plus1, 5);
    CHECK(H_Axis(h, 0, 2) == 0 && H_Axis(h, 0, 4) == 32767);
    H_Feed(h, r_yaw_minus2, 5);
    CHECK(H_Axis(h, 0, 4) == 0 && H_Axis(h, 0, 5) == -32768);

    /* 6 */
    CheckRaw(r_left, 0, 0, 0, 0, 0, 0, 0x01);
    CheckRaw(r_middle, 0, 0, 0, 0, 0, 0, 0x02);
    CheckRaw(r_right, 0, 0, 0, 0, 0, 0, 0x04);
    H_Feed(h, r_left, 5);
    CHECK(H_OnlyButton(h, 0, 0));
    H_Feed(h, r_middle, 5);
    CHECK(H_OnlyButton(h, 0, 1));
    H_Feed(h, r_right, 5);
    CHECK(H_OnlyButton(h, 0, 2));

    /* 7 */
    {
        const int mark = h->npublished;
        H_Feed(h, r_power, 4);
        H_Feed(h, static_report, sizeof(static_report));
        CHECK(h->npublished == mark && State(h)->power_connected && !State(h)->power_too_high);
        CHECK(h->npresence == 1);
    }

    /* 8 */
    CheckRaw(r_z_minus1, 0, 0, -1, 0, 0, 0, 0);
    H_Feed(h, r_z_minus1, 5);
    CHECK(H_Axis(h, 0, 2) == -16384);

    /* 9 */
    H_Feed(h, r_rest, 5);
    H_Feed(h, r_cut, sizeof(r_cut));
    CHECK(H_Axis(h, 0, 0) == 0);

    /* External power too high is logged once */
    {
        static const uint8_t high[4] = { 0xC3, 0x00, 0x00, 0x00 };
        const int logs = h->nlogs;
        H_Feed(h, high, 4);
        H_Feed(h, high, 4);
        CHECK(h->nlogs == logs + 1 && State(h)->power_too_high);
    }
    H_Destroy(h);
}

static void TestRumble(void)
{
    Harness *h = PresentCyberMan();
    SDL_SerialOutput request;
    uint8_t command[5];

    /* 10 */
    SDL_CyberMan_TactileCommand(0xFFFF, 0, command);
    CHECK(memcmp(command, "\x21\x54\xFF\x01\xFF", 5) == 0);
    SDL_CyberMan_TactileCommand(0x8000, 0x1234, command);
    CHECK(memcmp(command, "\x21\x54\x80\x01\xFF", 5) == 0);
    SDL_CyberMan_TactileCommand(0, 0, command);
    CHECK(memcmp(command, "\x21\x54\x01\x01\x00", 5) == 0);
    SDL_CyberMan_TactileCommand(0x0001, 0, command);
    CHECK(command[2] == 0x01 && command[4] == 0xFF);

    memset(&request, 0, sizeof(request));
    request.kind = SDL_SERIAL_OUTPUT_RUMBLE;
    request.high_frequency_rumble = 0xFFFF;
    H_Advance(h, 1000);
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(1000));
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x21\x54\xFF\x01\xFF", 5, 1000));

    /* Requests made while a write is pending collapse into the latest */
    h->pend_writes = true;
    request.high_frequency_rumble = 0x8000;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(1000));
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x21\x54\x80\x01\xFF", 5, 1000));
    request.high_frequency_rumble = 0xFFFF;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(1000));
    request.high_frequency_rumble = 0;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(1000));
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1010);
    H_CompleteWrite(h, true);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x21\x54\x01\x01\x00", 5, 1010));
    H_CompleteWrite(h, true);
    CHECK(H_NextCall(h) == NULL);

    /* An effect request is not rumble */
    h->pend_writes = false;
    request.kind = SDL_SERIAL_OUTPUT_EFFECT;
    request.length = 2;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(1010));
    CHECK(H_NextCall(h) == NULL);
    H_Destroy(h);

    /* Nothing is sent before the device is present */
    h = H_Create(&SDL_SerialCyberManModule);
    H_Start(h);
    H_SkipCalls(h);
    memset(&request, 0, sizeof(request));
    request.kind = SDL_SERIAL_OUTPUT_RUMBLE;
    request.low_frequency_rumble = 0xFFFF;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(0));
    CHECK(H_NextCall(h) == NULL);
    H_Destroy(h);
}

static void TestBatteryB(void)
{
    Harness *h = PresentCyberMan();

    H_Feed(h, r_left, 5);
    H_Advance(h, 1000);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 1000));
    H_Advance(h, 2000);
    CHECK(H_ExpectOpened(h, 1200, 7, SDL_SERIAL_NOPARITY, 1, 2000));
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETDTR, 2000));
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRRTS, 2000));
    BringUp(h);
    CHECK(h->presence[0] == 2 && H_NoButtons(h, 0));
    H_Destroy(h);
}

static void TestBatteryA(void)
{
    Harness *h = PresentCyberMan();
    H_Vector vectors[24];
    int n = 0, i;

#define VECTOR(name, bytes, changes) H_SetVector(&vectors[n++], name, bytes, sizeof(bytes), changes, 0)
    VECTOR("3 rest", r_rest, 0);
    VECTOR("4 X 127", r_x_max, 1);
    VECTOR("4 X -128", r_x_min, 1);
    VECTOR("4 Y 127", r_y_max, 1);
    VECTOR("4 Y -1", r_y_minus1, 1);
    VECTOR("5 Z +1", r_z_plus1, 1);
    VECTOR("5 Z -2", r_z_minus2, 1);
    VECTOR("5 pitch +1", r_pitch_plus1, 1);
    VECTOR("5 pitch -2", r_pitch_minus2, 1);
    VECTOR("5 roll +1", r_roll_plus1, 1);
    VECTOR("5 roll -2", r_roll_minus2, 1);
    VECTOR("5 yaw +1", r_yaw_plus1, 1);
    VECTOR("5 yaw -2", r_yaw_minus2, 1);
    VECTOR("6 L", r_left, 1);
    VECTOR("6 M", r_middle, 1);
    VECTOR("6 R", r_right, 1);
    VECTOR("7 power", r_power, 0);
    VECTOR("7 static", static_report, 0);
    VECTOR("8 Z -1", r_z_minus1, 1);
    VECTOR("9 start byte inside", r_cut, 0);
#undef VECTOR
    for (i = 0; i < n; ++i) {
        H_BatteryA(h, &vectors[i], BringUp);
    }
    H_BatteryA6(h, "9 cut report", r_cut, 2, &vectors[1]);
    H_Destroy(h);
}

int main(void)
{
    TestStartup();
    TestStartupFailures();
    TestReports();
    TestRumble();
    TestBatteryB();
    TestBatteryA();
    return H_Finish();
}
