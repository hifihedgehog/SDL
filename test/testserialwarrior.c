/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_warrior_proto.c, the
   Logitech WingMan Warrior of hifihedgehog/SDL#33 Part 5. The handshake is
   inputattach's, the packets Linux's. Test numbers follow the ticket. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_warrior_proto.h"

static SDL_WarriorState *State(Harness *h)
{
    return (SDL_WarriorState *)h->state;
}

static void BringUp(Harness *h)
{
    const uint64_t t = h->now;

    H_Advance(h, t + 100);
    H_FeedAt(h, t + 110, (const uint8_t *)"*", 1);
    H_FeedAt(h, t + 120, (const uint8_t *)"S", 1);
}

static Harness *PresentStick(void)
{
    Harness *h = H_Create(&SDL_SerialWarriorModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialWarriorModule);

    /* 1 */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 1200, 7, SDL_SERIAL_NOPARITY, 2, 0));
    H_Advance(h, 99);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 100);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x2A", 1, 100));
    H_FeedAt(h, 150, (const uint8_t *)"\x2A", 1);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x53", 1, 150));
    CHECK(h->npresence == 0);
    H_FeedAt(h, 200, (const uint8_t *)"\x53", 1);
    CHECK(H_IsPlainLine(H_NextCall(h), 4800, 8, SDL_SERIAL_NOPARITY, 1, 200));
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Logitech WingMan Warrior") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_FLIGHT_STICK && h->identity[0].naxes == 3 && h->identity[0].nbuttons == 4);
    CHECK(h->identity[0].nhats == 1 && h->identity[0].nballs == 1 && !h->identity[0].has_mapping);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 60200);
    CHECK(h->presence[0] == 1 && H_NextCall(h) == NULL);
    H_Destroy(h);

    /* No echo within 1000 ms */
    h = H_Create(&SDL_SerialWarriorModule);
    H_Start(h);
    H_Advance(h, 100);
    H_SkipCalls(h);
    H_Advance(h, 1099);
    CHECK(State(h)->step == SDL_WARRIOR_STEP_STAR);
    H_Advance(h, 1100);
    CHECK(State(h)->step == SDL_WARRIOR_STEP_DRAIN);
    H_Advance(h, 2099);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 2100);
    CHECK(H_IsWriteText(H_NextCall(h), "*", 2100) && h->npresence == 0);
    H_Destroy(h);

    /* The echo 2B */
    h = H_Create(&SDL_SerialWarriorModule);
    H_Start(h);
    H_Advance(h, 100);
    H_SkipCalls(h);
    H_FeedAt(h, 150, (const uint8_t *)"\x2B", 1);
    CHECK(h->npresence == 0 && State(h)->step == SDL_WARRIOR_STEP_DRAIN);
    H_Advance(h, 1149);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1150);
    CHECK(H_IsWriteText(H_NextCall(h), "*", 1150));
    /* A wrong second echo too */
    H_FeedAt(h, 1160, (const uint8_t *)"*", 1);
    CHECK(H_IsWriteText(H_NextCall(h), "S", 1160));
    H_FeedAt(h, 1170, (const uint8_t *)"s", 1);
    CHECK(State(h)->step == SDL_WARRIOR_STEP_DRAIN && h->npresence == 0);
    H_Destroy(h);

    /* Presence waits for the new rate: a port that refuses 4800 baud never
       presents the stick */
    h = H_Create(&SDL_SerialWarriorModule);
    h->fail_rate = 4800;
    H_Start(h);
    H_Advance(h, 100);
    H_FeedAt(h, 150, (const uint8_t *)"*", 1);
    H_FeedAt(h, 200, (const uint8_t *)"S", 1);
    CHECK(h->npresence == 0 && h->engine.losses == 1);
    H_Destroy(h);
}

static const uint8_t b_trigger[4] = { 0x90, 0x00, 0x00, 0x01 };
static const uint8_t b_thumb[4] = { 0x90, 0x00, 0x00, 0x02 };
static const uint8_t b_top[4] = { 0x90, 0x00, 0x00, 0x04 };
static const uint8_t b_top2[4] = { 0x90, 0x00, 0x00, 0x08 };
static const uint8_t s_rest[3] = { 0xB0, 0x00, 0x00 };
static const uint8_t s_mixed[3] = { 0xB0, 0x10, 0x20 };
static const uint8_t s_x_max[3] = { 0xB8, 0x00, 0x00 };
static const uint8_t s_x_min[3] = { 0xB4, 0x00, 0x7F };
static const uint8_t s_y_max[3] = { 0xB1, 0x7F, 0x00 };
static const uint8_t s_y_min[3] = { 0xB2, 0x00, 0x00 };
static const uint8_t t_left[4] = { 0xD0, 0x00, 0x00, 0x01 };
static const uint8_t t_right[4] = { 0xD0, 0x00, 0x00, 0x02 };
static const uint8_t t_up[4] = { 0xD0, 0x00, 0x00, 0x04 };
static const uint8_t t_down[4] = { 0xD0, 0x00, 0x00, 0x08 };
static const uint8_t t_throttle[4] = { 0xD1, 0x7F, 0x00, 0x00 };
static const uint8_t t_dial_plus5[4] = { 0xD0, 0x00, 0x05, 0x00 };
static const uint8_t t_dial_minus256[4] = { 0xD8, 0x00, 0x00, 0x00 };
static const uint8_t type2[12] = { 0xA0, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F };
static const uint8_t type4[4] = { 0xC0, 0x7F, 0x7F, 0x7F };
static const uint8_t cut[6] = { 0xB0, 0x10, 0x90, 0x00, 0x00, 0x01 };

static void TestPackets(void)
{
    Harness *h = PresentStick();

    /* 2 */
    H_Feed(h, b_trigger, 4);
    CHECK(H_OnlyButton(h, 0, 0));
    H_Feed(h, b_thumb, 4);
    CHECK(H_OnlyButton(h, 0, 1));
    H_Feed(h, b_top, 4);
    CHECK(H_OnlyButton(h, 0, 2));
    H_Feed(h, b_top2, 4);
    CHECK(H_OnlyButton(h, 0, 3));

    /* 3 */
    H_Feed(h, s_mixed, 3);
    CHECK(H_Axis(h, 0, 1) == SDL_Serial_ScaleSigned(16, -256, 255) && H_Axis(h, 0, 0) == SDL_Serial_ScaleSigned(-32, -255, 256));
    CHECK(H_Axis(h, 0, 1) == 2055 && H_Axis(h, 0, 0) == -4112);
    H_Feed(h, s_rest, 3);
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 1) == 0);
    H_Feed(h, s_x_max, 3);
    CHECK(H_Axis(h, 0, 0) == 32767);
    H_Feed(h, s_x_min, 3);
    CHECK(H_Axis(h, 0, 0) == -32768);
    H_Feed(h, s_y_max, 3);
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 1) == 32767);
    H_Feed(h, s_y_min, 3);
    CHECK(H_Axis(h, 0, 1) == -32768);

    /* 4 */
    H_Feed(h, t_left, 4);
    CHECK(H_Snapshot(h, 0)->controls.hats[0] == SDL_SERIAL_HAT_LEFT);
    H_Feed(h, t_right, 4);
    CHECK(H_Snapshot(h, 0)->controls.hats[0] == SDL_SERIAL_HAT_RIGHT);
    H_Feed(h, t_up, 4);
    CHECK(H_Snapshot(h, 0)->controls.hats[0] == SDL_SERIAL_HAT_UP);
    H_Feed(h, t_down, 4);
    CHECK(H_Snapshot(h, 0)->controls.hats[0] == SDL_SERIAL_HAT_DOWN);
    H_Feed(h, t_throttle, 4);
    CHECK(H_Axis(h, 0, 2) == 32767 && H_Snapshot(h, 0)->controls.hats[0] == 0);
    H_Feed(h, t_dial_plus5, 4);
    CHECK(H_Last(h)->controls.ball[0] == 5 && H_Last(h)->controls.ball[1] == 0 && H_Axis(h, 0, 2) == 0);
    H_Feed(h, t_dial_minus256, 4);
    CHECK(H_Last(h)->controls.ball[0] == -256);

    /* 5 */
    {
        const int mark = h->npublished;
        H_Feed(h, type2, sizeof(type2));
        H_Feed(h, type4, sizeof(type4));
        CHECK(h->npublished == mark);
    }

    /* 6: a start byte inside a packet */
    H_Feed(h, b_top2, 4);
    H_Feed(h, s_mixed, 3);
    H_Feed(h, cut, sizeof(cut));
    CHECK(H_OnlyButton(h, 0, 0) && H_Axis(h, 0, 1) == 2055 && H_Axis(h, 0, 0) == -4112);

    /* Types 0, 6 and 7 carry nothing, and the bytes after them are ignored */
    {
        static const uint8_t empty[6] = { 0x80, 0x10, 0xE0, 0x10, 0xF0, 0x10 };
        const int mark = h->npublished;
        H_Feed(h, empty, sizeof(empty));
        CHECK(h->npublished == mark);
    }
    CHECK(SDL_Warrior_PacketLength(0x90) == 4 && SDL_Warrior_PacketLength(0xA0) == 12 && SDL_Warrior_PacketLength(0xB0) == 3);
    CHECK(SDL_Warrior_PacketLength(0xC0) == 4 && SDL_Warrior_PacketLength(0xD0) == 4 && SDL_Warrior_PacketLength(0x80) == 0);
    CHECK(SDL_Warrior_PacketLength(0xE0) == 0 && SDL_Warrior_PacketLength(0xF0) == 0);
    H_Destroy(h);
}

static void TestBatteryB(void)
{
    Harness *h = PresentStick();

    H_Feed(h, b_trigger, 4);
    H_Advance(h, 500);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 500));
    H_Advance(h, 1500);
    CHECK(H_ExpectOpened(h, 1200, 7, SDL_SERIAL_NOPARITY, 2, 1500));
    H_Advance(h, 1600);
    CHECK(H_IsWriteText(H_NextCall(h), "*", 1600));
    H_FeedAt(h, 1610, (const uint8_t *)"*", 1);
    CHECK(H_IsWriteText(H_NextCall(h), "S", 1610));
    CHECK(h->presence[0] == 0);
    H_FeedAt(h, 1620, (const uint8_t *)"S", 1);
    CHECK(H_IsPlainLine(H_NextCall(h), 4800, 8, SDL_SERIAL_NOPARITY, 1, 1620));
    CHECK(h->presence[0] == 2 && H_NoButtons(h, 0));
    H_Destroy(h);
}

static void TestBatteryA(void)
{
    Harness *h = PresentStick();
    H_Vector vectors[24];
    int n = 0, i;

#define VECTOR(name, bytes, changes, repeat) H_SetVector(&vectors[n++], name, bytes, sizeof(bytes), changes, repeat)
    VECTOR("2 trigger", b_trigger, 1, 0);
    VECTOR("2 thumb", b_thumb, 1, 0);
    VECTOR("2 top", b_top, 1, 0);
    VECTOR("2 top 2", b_top2, 1, 0);
    VECTOR("3 rest", s_rest, 0, 0);
    VECTOR("3 X -32 Y 16", s_mixed, 1, 0);
    VECTOR("3 X 256", s_x_max, 1, 0);
    VECTOR("3 X -255", s_x_min, 1, 0);
    VECTOR("3 Y 255", s_y_max, 1, 0);
    VECTOR("3 Y -256", s_y_min, 1, 0);
    VECTOR("4 hat left", t_left, 1, 0);
    VECTOR("4 hat right", t_right, 1, 0);
    VECTOR("4 hat up", t_up, 1, 0);
    VECTOR("4 hat down", t_down, 1, 0);
    VECTOR("4 throttle", t_throttle, 1, 0);
    /* The dial is relative, so the same packet again is new motion */
    VECTOR("4 dial +5", t_dial_plus5, 1, 1);
    VECTOR("4 dial -256", t_dial_minus256, 1, 1);
    VECTOR("5 type 2", type2, 0, 0);
    VECTOR("5 type 4", type4, 0, 0);
    VECTOR("6 start byte inside", cut, 1, 0);
#undef VECTOR
    for (i = 0; i < n; ++i) {
        H_BatteryA(h, &vectors[i], BringUp);
    }
    H_Destroy(h);
}

int main(void)
{
    TestStartup();
    TestPackets();
    TestBatteryB();
    TestBatteryA();
    return H_Finish();
}
