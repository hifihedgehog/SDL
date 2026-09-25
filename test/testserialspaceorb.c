/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_spaceorb_proto.c, the
   SpaceOrb 360 of hifihedgehog/SDL#33 Part 5. The vectors are constructed
   from the X.org packing and the Linux framing. Test numbers follow the
   ticket. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_spaceorb_proto.h"

static SDL_SpaceOrbState *State(Harness *h)
{
    return (SDL_SpaceOrbState *)h->state;
}

static const uint8_t v_rest[12] = { 0x44, 0x80, 0xD3, 0xF0, 0xE1, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5, 0xA1 };
static const uint8_t v_x_plus1[12] = { 0x44, 0x80, 0xD3, 0xE0, 0xE1, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5, 0xB1 };
static const uint8_t v_x_minus1[12] = { 0x44, 0x80, 0xAC, 0x80, 0xE1, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5, 0xAE };
static const uint8_t v_x_max[12] = { 0x44, 0x80, 0xEC, 0x80, 0xE1, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5, 0xEE };
static const uint8_t v_x_min[12] = { 0x44, 0x80, 0x93, 0xF0, 0xE1, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5, 0xE1 };
static const uint8_t v_y_plus1[12] = { 0x44, 0x80, 0xD3, 0xF0, 0xE3, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5, 0xA3 };
static const uint8_t v_z_plus1[12] = { 0x44, 0x80, 0xD3, 0xF0, 0xE1, 0xE3, 0xC5, 0xD7, 0xE1, 0xF2, 0xE5, 0x81 };
static const uint8_t v_z_plus4[12] = { 0x44, 0x80, 0xD3, 0xF0, 0xE1, 0xE2, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5, 0xA0 };
static const uint8_t v_rx_plus1[12] = { 0x44, 0x80, 0xD3, 0xF0, 0xE1, 0xE3, 0xE5, 0xD3, 0xE1, 0xF2, 0xE5, 0xA5 };
static const uint8_t v_ry_plus1[12] = { 0x44, 0x80, 0xD3, 0xF0, 0xE1, 0xE3, 0xE5, 0xD7, 0xE1, 0xB2, 0xE5, 0xE1 };
static const uint8_t v_ry_plus2[12] = { 0x44, 0x80, 0xD3, 0xF0, 0xE1, 0xE3, 0xE5, 0xD7, 0xE0, 0xF2, 0xE5, 0xA0 };
static const uint8_t v_rz_plus1[12] = { 0x44, 0x80, 0xD3, 0xF0, 0xE1, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xED, 0xA9 };
static const uint8_t v_y16[12] = { 0x44, 0x80, 0xD3, 0xF0, 0xC1, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5, 0x81 };
static const uint8_t v_rx16[12] = { 0x44, 0x80, 0xD3, 0xF0, 0xE1, 0xE3, 0xE5, 0x97, 0xE1, 0xF2, 0xE5, 0xE1 };
static const uint8_t v_button0[12] = { 0x44, 0x81, 0xD3, 0xF0, 0xE1, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5, 0xA0 };
static const uint8_t v_button6[12] = { 0x44, 0xC0, 0xD3, 0xF0, 0xE1, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5, 0xE1 };
static const uint8_t v_buttons_all[12] = { 0x44, 0xFF, 0xD3, 0xF0, 0xE1, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5, 0xDE };
static const uint8_t k_button0[5] = { 0x4B, 0x80, 0x81, 0x80, 0xCA };
static const uint8_t k_button6[5] = { 0x4B, 0x80, 0xC0, 0x80, 0x8B };
static const uint8_t bad_xor[12] = { 0x44, 0x80, 0xD3, 0xF0, 0xE1, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5, 0xA2 };
static const uint8_t short_d[11] = { 0x44, 0x80, 0xD3, 0xF0, 0xE1, 0xE3, 0xE5, 0xD7, 0xE1, 0xF2, 0xE5 };
/* Six bytes whose XOR is 0, with a first five that do not check */
static const uint8_t long_k[6] = { 0x4B, 0x80, 0x81, 0x80, 0xCB, 0x81 };

static void CheckRaw(const uint8_t *packet, int x, int y, int z, int rx, int ry, int rz, uint8_t buttons)
{
    uint8_t stored[12], b = 0xA5;
    int axes[6], i;

    for (i = 0; i < 12; ++i) {
        stored[i] = (uint8_t)(packet[i] & 0x7F);
    }
    CHECK(SDL_SpaceOrb_DecodeD(stored, 12, axes, &b));
    CHECK(axes[0] == x && axes[1] == y && axes[2] == z && axes[3] == rx && axes[4] == ry && axes[5] == rz);
    CHECK(b == buttons);
}

/* After the open: the drain, then the rest packet */
static void BringUp(Harness *h)
{
    H_Advance(h, h->now + SDL_SPACEORB_DRAIN_MS);
    H_Feed(h, v_rest, sizeof(v_rest));
}

static Harness *PresentOrb(void)
{
    Harness *h = H_Create(&SDL_SerialSpaceOrbModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1);
    return h;
}

static void TestDecode(void)
{
    /* 1 to 5, from the packing alone */
    CheckRaw(v_rest, 0, 0, 0, 0, 0, 0, 0);
    CheckRaw(v_x_plus1, 1, 0, 0, 0, 0, 0, 0);
    CheckRaw(v_x_minus1, -1, 0, 0, 0, 0, 0, 0);
    CheckRaw(v_x_max, 511, 0, 0, 0, 0, 0, 0);
    CheckRaw(v_x_min, -512, 0, 0, 0, 0, 0, 0);
    CheckRaw(v_y_plus1, 0, 1, 0, 0, 0, 0, 0);
    CheckRaw(v_z_plus1, 0, 0, 1, 0, 0, 0, 0);
    CheckRaw(v_z_plus4, 0, 0, 4, 0, 0, 0, 0);
    CheckRaw(v_rx_plus1, 0, 0, 0, 1, 0, 0, 0);
    CheckRaw(v_ry_plus1, 0, 0, 0, 0, 1, 0, 0);
    CheckRaw(v_ry_plus2, 0, 0, 0, 0, 2, 0, 0);
    CheckRaw(v_rz_plus1, 0, 0, 0, 0, 0, 1, 0);
    /* 4: each bit is used once, so neither Z nor RY moves */
    CheckRaw(v_y16, 0, 16, 0, 0, 0, 0, 0);
    CheckRaw(v_rx16, 0, 0, 0, 16, 0, 0, 0);
    CheckRaw(v_button0, 0, 0, 0, 0, 0, 0, 0x01);
    CheckRaw(v_button6, 0, 0, 0, 0, 0, 0, 0x40);
    CheckRaw(v_buttons_all, 0, 0, 0, 0, 0, 0, 0x7F);
    {
        int axes[6];
        uint8_t b;
        uint8_t stored[12] = { 0x44 };
        CHECK(!SDL_SpaceOrb_DecodeD(stored, 11, axes, &b));
        stored[0] = 0x4B;
        CHECK(!SDL_SpaceOrb_DecodeD(stored, 12, axes, &b));
        CHECK(!SDL_SpaceOrb_DecodeD(NULL, 12, axes, &b));
    }
}

static void TestStream(void)
{
    Harness *h = H_Create(&SDL_SerialSpaceOrbModule);
    static const int all[] = { 0, 1, 2, 3, 4, 5, 6, -1 };

    /* 1: the rest packet presents the orb */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 100);
    CHECK(h->npresence == 0);
    H_Feed(h, v_rest, sizeof(v_rest));
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "SpaceTec SpaceOrb 360") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN && h->identity[0].naxes == 6 && h->identity[0].nbuttons == 7);
    CHECK(H_NoButtons(h, 0) && H_Axis(h, 0, 0) == 0);

    /* 2 */
    H_Feed(h, v_x_plus1, 12);
    CHECK(H_Axis(h, 0, 0) == 64); /* 1 x 32767 / 511 */
    H_Feed(h, v_x_minus1, 12);
    CHECK(H_Axis(h, 0, 0) == -64); /* -1 x 32768 / 512 */
    H_Feed(h, v_x_max, 12);
    CHECK(H_Axis(h, 0, 0) == 32767);
    H_Feed(h, v_x_min, 12);
    CHECK(H_Axis(h, 0, 0) == -32768);

    /* 3 */
    H_Feed(h, v_y_plus1, 12);
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 1) == 64);
    H_Feed(h, v_z_plus4, 12);
    CHECK(H_Axis(h, 0, 1) == 0 && H_Axis(h, 0, 2) == 256);
    H_Feed(h, v_rz_plus1, 12);
    CHECK(H_Axis(h, 0, 2) == 0 && H_Axis(h, 0, 5) == 64);

    /* 5 */
    H_Feed(h, v_button0, 12);
    CHECK(H_OnlyButton(h, 0, 0));
    H_Feed(h, v_button6, 12);
    CHECK(H_OnlyButton(h, 0, 6));
    H_Feed(h, v_buttons_all, 12);
    CHECK(H_OnlyButtons(h, 0, all));
    H_Feed(h, k_button0, 5);
    CHECK(H_OnlyButton(h, 0, 0));
    H_Feed(h, k_button6, 5);
    CHECK(H_OnlyButton(h, 0, 6));

    /* 6 */
    {
        const int mark = h->npublished;
        H_Feed(h, bad_xor, 12);
        H_Feed(h, short_d, 11);
        H_Feed(h, long_k, 6);
        CHECK(h->npublished == mark);
        H_Feed(h, v_x_plus1, 12);
        CHECK(H_Axis(h, 0, 0) == 64 && H_NoButtons(h, 0));
    }

    /* E packets log their flags and change nothing */
    {
        static const uint8_t e_packet[4] = { 0x45, 0x81, 0x80, 0xC4 };
        const int mark = h->npublished;
        H_Feed(h, e_packet, 4);
        CHECK(h->npublished == mark && State(h)->errors == 1 && State(h)->last_error == 0x01 && h->nlogs == 1);
    }

    /* Continuation bytes with no start byte before them are ignored */
    {
        static const uint8_t stray[3] = { 0xC4, 0xC4, 0x81 };
        const int mark = h->npublished;
        H_Feed(h, stray, 3);
        CHECK(h->npublished == mark);
    }
    H_Destroy(h);
}

/* An R greeting with a valid check byte presents the orb at the next start byte */
static void TestGreeting(void)
{
    Harness *h = H_Create(&SDL_SerialSpaceOrbModule);
    uint8_t greeting[16];
    const char *text = " SpaceOrb";
    uint8_t check = 'R';
    size_t n = 0, i;

    greeting[n++] = 'R';
    for (i = 0; text[i]; ++i) {
        greeting[n++] = (uint8_t)(text[i] | 0x80);
        check ^= (uint8_t)text[i];
    }
    greeting[n++] = (uint8_t)(check | 0x80);

    H_Start(h);
    H_Advance(h, 100);
    H_Feed(h, greeting, n);
    CHECK(h->npresence == 0);
    H_Feed(h, v_rest, 1);
    CHECK(h->presence[0] == 1);
    H_Destroy(h);

    /* A greeting that does not check presents nothing */
    h = H_Create(&SDL_SerialSpaceOrbModule);
    H_Start(h);
    H_Advance(h, 100);
    greeting[n - 1] ^= 0x01;
    H_Feed(h, greeting, n);
    H_Feed(h, v_rest, 1);
    CHECK(h->npresence == 0);
    /* The D packet that the start byte opened completes and presents */
    H_Feed(h, v_rest + 1, 11);
    CHECK(h->presence[0] == 1);
    H_Destroy(h);
}

static void TestDrain(void)
{
    Harness *h = H_Create(&SDL_SerialSpaceOrbModule);

    /* 7 */
    H_Start(h);
    H_FeedAt(h, 50, v_x_plus1, 12);
    CHECK(h->npresence == 0);
    H_FeedAt(h, 120, v_x_plus1, 5);
    H_Advance(h, 219);
    H_Feed(h, v_x_plus1 + 5, 7);
    CHECK(h->npresence == 0 && State(h)->draining);
    H_Advance(h, 318);
    CHECK(State(h)->draining);
    H_Advance(h, 319);
    CHECK(!State(h)->draining);
    /* The tail of a packet the drain cut does not decode */
    H_Feed(h, v_x_plus1 + 3, 9);
    CHECK(h->npresence == 0);
    H_Feed(h, v_x_plus1, 12);
    CHECK(h->presence[0] == 1 && H_Axis(h, 0, 0) == 64);
    H_Destroy(h);
}

/* Twelve bytes with bit 7 set make no packet without a start byte, even
   when with bit 7 removed they would be a valid D packet */
static void TestStrays(void)
{
    Harness *h = H_Create(&SDL_SerialSpaceOrbModule);
    uint8_t stray[12];

    memcpy(stray, v_x_plus1, 12);
    stray[0] = (uint8_t)(stray[0] | 0x80);
    H_Start(h);
    H_Advance(h, 100);
    H_Feed(h, stray, 12);
    CHECK(h->npresence == 0 && h->npublished == 0);
    H_Feed(h, v_x_plus1, 12);
    CHECK(h->presence[0] == 1 && H_Axis(h, 0, 0) == 64);
    H_Destroy(h);
}

static void TestBatteryB(void)
{
    Harness *h = PresentOrb();

    H_Feed(h, v_x_plus1, 12);
    H_SkipCalls(h);
    H_Advance(h, 400);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 400));
    H_Advance(h, 1400);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 1400));
    CHECK(H_NextCall(h) == NULL);
    /* The drain again, then a new instance on the first valid packet */
    H_Feed(h, v_x_plus1, 12);
    CHECK(h->presence[0] == 0);
    H_Advance(h, 1500);
    H_Feed(h, v_x_plus1, 12);
    CHECK(h->presence[0] == 2 && H_Axis(h, 0, 0) == 64);
    /* No keep-alive and no silence limit */
    H_Advance(h, 61500);
    CHECK(h->presence[0] == 2 && H_NextCall(h) == NULL);
    H_Destroy(h);
}

static void TestBatteryA(void)
{
    Harness *h = PresentOrb();
    H_Vector vectors[24];
    int n = 0, i;

#define VECTOR(name, bytes, changes) H_SetVector(&vectors[n++], name, bytes, sizeof(bytes), changes, 0)
    VECTOR("1 rest", v_rest, 0);
    VECTOR("2 X +1", v_x_plus1, 1);
    VECTOR("2 X -1", v_x_minus1, 1);
    VECTOR("2 X 511", v_x_max, 1);
    VECTOR("2 X -512", v_x_min, 1);
    VECTOR("3 Y +1", v_y_plus1, 1);
    VECTOR("3 Z +1", v_z_plus1, 1);
    VECTOR("3 Z +4", v_z_plus4, 1);
    VECTOR("3 RX +1", v_rx_plus1, 1);
    VECTOR("3 RY +1", v_ry_plus1, 1);
    VECTOR("3 RY +2", v_ry_plus2, 1);
    VECTOR("3 RZ +1", v_rz_plus1, 1);
    VECTOR("4 Y +16", v_y16, 1);
    VECTOR("4 RX +16", v_rx16, 1);
    VECTOR("5 button 0", v_button0, 1);
    VECTOR("5 button 6", v_button6, 1);
    VECTOR("5 all buttons", v_buttons_all, 1);
    VECTOR("5 K button 0", k_button0, 1);
    VECTOR("5 K button 6", k_button6, 1);
#undef VECTOR
    for (i = 0; i < n; ++i) {
        H_BatteryA(h, &vectors[i], BringUp);
    }
    H_BatteryA6(h, "6 bad XOR", bad_xor, sizeof(bad_xor), &vectors[1]);
    H_BatteryA6(h, "6 11-byte D", short_d, sizeof(short_d), &vectors[1]);
    H_BatteryA6(h, "6 6-byte K", long_k, sizeof(long_k), &vectors[17]);
    H_Destroy(h);
}

int main(void)
{
    TestDecode();
    TestStream();
    TestGreeting();
    TestDrain();
    TestStrays();
    TestBatteryB();
    TestBatteryA();
    return H_Finish();
}
