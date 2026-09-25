/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_stinger_proto.c, the Gravis
   Stinger of hifihedgehog/SDL#33 Part 5. The start-up bytes are
   inputattach's, the packet layout Linux's. Test numbers follow the
   ticket. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_stinger_proto.h"

static const uint8_t enable[5] = { 0x20, 0x45, 0x35, 0x45, 0x35 };
static const uint8_t reply[16] = { 0x0D, 0x0A, 0x30, 0x36, 0x30, 0x30, 0x35, 0x32, 0x30, 0x30, 0x35, 0x38, 0x43, 0x32, 0x37, 0x32 };

static SDL_StingerState *State(Harness *h)
{
    return (SDL_StingerState *)h->state;
}

/* From the open: the drain, the enable command, the answer 10 ms apart */
static void BringUp(Harness *h)
{
    const uint64_t t = h->now;
    int i;

    H_Advance(h, t + 100);
    for (i = 0; i < 16; ++i) {
        H_FeedAt(h, t + 110 + (uint64_t)i * 10, &reply[i], 1);
    }
}

static Harness *PresentPad(void)
{
    Harness *h = H_Create(&SDL_SerialStingerModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialStingerModule);
    const SDL_SerialGamepadMap *map;
    int i;

    /* 1 */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 1200, 8, SDL_SERIAL_NOPARITY, 1, 0));
    H_Advance(h, 99);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 100);
    CHECK(H_IsWrite(H_NextCall(h), enable, 5, 100));
    for (i = 0; i < 15; ++i) {
        H_FeedAt(h, 110 + (uint64_t)i * 10, &reply[i], 1);
    }
    CHECK(h->npresence == 0);
    H_FeedAt(h, 260, &reply[15], 1);
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Gravis Stinger") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_GAMEPAD && h->identity[0].naxes == 2 && h->identity[0].nbuttons == 10);
    /* The mapping: A South, B East, X West, Y North, C and Z shoulders, TL and TR triggers */
    map = &h->identity[0].mapping;
    CHECK(h->identity[0].has_mapping);
    CHECK(map->a.kind == SDL_SERIAL_MAP_BUTTON && map->a.target == 0);
    CHECK(map->b.kind == SDL_SERIAL_MAP_BUTTON && map->b.target == 1);
    CHECK(map->rightshoulder.kind == SDL_SERIAL_MAP_BUTTON && map->rightshoulder.target == 2);
    CHECK(map->x.kind == SDL_SERIAL_MAP_BUTTON && map->x.target == 3);
    CHECK(map->y.kind == SDL_SERIAL_MAP_BUTTON && map->y.target == 4);
    CHECK(map->leftshoulder.kind == SDL_SERIAL_MAP_BUTTON && map->leftshoulder.target == 5);
    CHECK(map->lefttrigger.kind == SDL_SERIAL_MAP_BUTTON && map->lefttrigger.target == 6);
    CHECK(map->righttrigger.kind == SDL_SERIAL_MAP_BUTTON && map->righttrigger.target == 7);
    CHECK(map->back.kind == SDL_SERIAL_MAP_BUTTON && map->back.target == 8);
    CHECK(map->start.kind == SDL_SERIAL_MAP_BUTTON && map->start.target == 9);
    CHECK(map->leftx.kind == SDL_SERIAL_MAP_AXIS && map->leftx.target == 0);
    CHECK(map->lefty.kind == SDL_SERIAL_MAP_AXIS && map->lefty.target == 1);
    CHECK(map->guide.kind == SDL_SERIAL_MAP_NONE && map->dpup.kind == SDL_SERIAL_MAP_NONE && map->rightx.kind == SDL_SERIAL_MAP_NONE);
    CHECK(H_NextCall(h) == NULL);
    /* No keep-alive, no silence limit */
    H_Advance(h, 60260);
    CHECK(h->presence[0] == 1 && H_NextCall(h) == NULL);
    H_Destroy(h);

    /* The seventh byte changed: nothing presented, retry at +1000 ms */
    h = H_Create(&SDL_SerialStingerModule);
    H_Start(h);
    H_Advance(h, 100);
    H_SkipCalls(h);
    for (i = 0; i < 6; ++i) {
        H_FeedAt(h, 110 + (uint64_t)i * 10, &reply[i], 1);
    }
    {
        const uint8_t wrong = (uint8_t)(reply[6] ^ 0x01);
        H_FeedAt(h, 170, &wrong, 1);
    }
    for (i = 7; i < 16; ++i) {
        H_FeedAt(h, 110 + (uint64_t)i * 10, &reply[i], 1);
    }
    CHECK(h->npresence == 0);
    H_Advance(h, 1169);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1170);
    CHECK(H_IsWrite(H_NextCall(h), enable, 5, 1170));
    H_Destroy(h);

    /* A 201 ms gap before the ninth byte: nothing presented */
    h = H_Create(&SDL_SerialStingerModule);
    H_Start(h);
    H_Advance(h, 100);
    H_SkipCalls(h);
    for (i = 0; i < 8; ++i) {
        H_FeedAt(h, 110 + (uint64_t)i * 10, &reply[i], 1);
    }
    H_Advance(h, 379);
    CHECK(State(h)->step == SDL_STINGER_STEP_REPLY);
    for (i = 8; i < 16; ++i) {
        H_FeedAt(h, 381 + (uint64_t)(i - 8) * 10, &reply[i], 1);
    }
    CHECK(h->npresence == 0 && State(h)->step == SDL_STINGER_STEP_DRAIN);
    /* The retry waits for 100 ms of silence after the stray bytes, and 1000 ms after the failure */
    H_Advance(h, 1379);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1380);
    CHECK(H_IsWrite(H_NextCall(h), enable, 5, 1380));
    H_Destroy(h);

    /* No answer to the enable command */
    h = H_Create(&SDL_SerialStingerModule);
    H_Start(h);
    H_Advance(h, 299);
    CHECK(State(h)->step == SDL_STINGER_STEP_REPLY);
    H_Advance(h, 300);
    CHECK(State(h)->step == SDL_STINGER_STEP_DRAIN);
    H_Destroy(h);

    /* Bytes waiting at the open extend the drain */
    h = H_Create(&SDL_SerialStingerModule);
    H_Start(h);
    H_SkipCalls(h);
    H_FeedAt(h, 50, reply, 3);
    H_FeedAt(h, 140, reply, 3);
    H_Advance(h, 239);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 240);
    CHECK(H_IsWrite(H_NextCall(h), enable, 5, 240));
    H_Destroy(h);
}

static const uint8_t p_rest[4] = { 0x00, 0x00, 0x00, 0x00 };
static const uint8_t p_a[4] = { 0x20, 0x00, 0x00, 0x00 };
static const uint8_t p_b[4] = { 0x10, 0x00, 0x00, 0x00 };
static const uint8_t p_c[4] = { 0x08, 0x00, 0x00, 0x00 };
static const uint8_t p_x[4] = { 0x04, 0x00, 0x00, 0x00 };
static const uint8_t p_y[4] = { 0x00, 0x00, 0x00, 0x20 };
static const uint8_t p_z[4] = { 0x00, 0x00, 0x00, 0x10 };
static const uint8_t p_tl[4] = { 0x00, 0x00, 0x00, 0x08 };
static const uint8_t p_tr[4] = { 0x00, 0x00, 0x00, 0x04 };
static const uint8_t p_select[4] = { 0x00, 0x00, 0x00, 0x02 };
static const uint8_t p_start[4] = { 0x00, 0x00, 0x00, 0x01 };
static const uint8_t p_x_min[4] = { 0x01, 0x00, 0x00, 0x00 };
static const uint8_t p_x_max[4] = { 0x00, 0x3F, 0x00, 0x00 };
static const uint8_t p_y_max[4] = { 0x02, 0x00, 0x00, 0x00 };
static const uint8_t p_y_min[4] = { 0x00, 0x00, 0x3F, 0x00 };
static const uint8_t p_high[4] = { 0xC0, 0xC0, 0xC0, 0xC0 };

static void TestPackets(void)
{
    Harness *h = PresentPad();
    const uint8_t *buttons[10];
    int i, x, y;
    uint16_t b;

    /* 2 */
    H_Feed(h, p_a, 4);
    H_Feed(h, p_rest, 4);
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 1) == 0 && H_NoButtons(h, 0));

    /* 3 */
    buttons[0] = p_a;
    buttons[1] = p_b;
    buttons[2] = p_c;
    buttons[3] = p_x;
    buttons[4] = p_y;
    buttons[5] = p_z;
    buttons[6] = p_tl;
    buttons[7] = p_tr;
    buttons[8] = p_select;
    buttons[9] = p_start;
    for (i = 0; i < 10; ++i) {
        H_Feed(h, buttons[i], 4);
        CHECK(H_OnlyButton(h, 0, i));
        CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 1) == 0);
    }

    /* 4 */
    H_Feed(h, p_x_min, 4);
    CHECK(H_Axis(h, 0, 0) == -32768 && H_Axis(h, 0, 1) == 0);
    SDL_Stinger_DecodePacket(p_x_min, &x, &y, &b);
    CHECK(x == -64 && y == 0 && b == 0);
    H_Feed(h, p_x_max, 4);
    CHECK(H_Axis(h, 0, 0) == 32767);
    SDL_Stinger_DecodePacket(p_x_max, &x, &y, &b);
    CHECK(x == 63);
    H_Feed(h, p_y_max, 4);
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 1) == 32767);
    SDL_Stinger_DecodePacket(p_y_max, &x, &y, &b);
    CHECK(y == 64);
    H_Feed(h, p_y_min, 4);
    CHECK(H_Axis(h, 0, 1) == -32768);
    SDL_Stinger_DecodePacket(p_y_min, &x, &y, &b);
    CHECK(y == -63);
    /* In between: X magnitude 1 with the sign bit is -63 */
    {
        static const uint8_t p[4] = { 0x01, 0x01, 0x00, 0x00 };
        SDL_Stinger_DecodePacket(p, &x, &y, &b);
        CHECK(x == -63);
    }

    /* 5 */
    H_Feed(h, p_rest, 4);
    {
        const int mark = h->npublished;
        H_Feed(h, p_high, 4);
        CHECK(h->npublished == mark);
    }

    /* A lost byte shifts every later packet, as it does in Linux: 20 00 00
       then 10 00 00 00 read as 20 00 00 10, A and Z */
    H_Feed(h, p_a, 3);
    H_Feed(h, p_b, 4);
    {
        static const int a_and_z[] = { 0, 5, -1 };
        CHECK(H_OnlyButtons(h, 0, a_and_z));
    }
    H_Destroy(h);
}

static void TestBatteryB(void)
{
    Harness *h = PresentPad();

    H_Feed(h, p_a, 4);
    H_Advance(h, 500);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 500));
    H_Advance(h, 1500);
    CHECK(H_ExpectOpened(h, 1200, 8, SDL_SERIAL_NOPARITY, 1, 1500));
    H_Advance(h, 1600);
    CHECK(H_IsWrite(H_NextCall(h), enable, 5, 1600));
    H_Feed(h, reply, 15);
    CHECK(h->presence[0] == 0);
    H_Feed(h, reply + 15, 1);
    CHECK(h->presence[0] == 2 && H_NoButtons(h, 0));
    H_Destroy(h);
}

static void TestBatteryA(void)
{
    Harness *h = PresentPad();
    H_Vector vectors[20];
    int n = 0, i;

#define VECTOR(name, bytes, changes) H_SetVector(&vectors[n++], name, bytes, sizeof(bytes), changes, 0)
    VECTOR("2 rest", p_rest, 0);
    VECTOR("3 A", p_a, 1);
    VECTOR("3 B", p_b, 1);
    VECTOR("3 C", p_c, 1);
    VECTOR("3 X", p_x, 1);
    VECTOR("3 Y", p_y, 1);
    VECTOR("3 Z", p_z, 1);
    VECTOR("3 TL", p_tl, 1);
    VECTOR("3 TR", p_tr, 1);
    VECTOR("3 Select", p_select, 1);
    VECTOR("3 Start", p_start, 1);
    VECTOR("4 X -64", p_x_min, 1);
    VECTOR("4 X 63", p_x_max, 1);
    VECTOR("4 Y 64", p_y_max, 1);
    VECTOR("4 Y -63", p_y_min, 1);
    VECTOR("5 high bits", p_high, 0);
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
