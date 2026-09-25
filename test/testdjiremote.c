/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/dji/SDL_dji_remote_proto.c, the DJI remotes
   of hifihedgehog/SDL#33 Part 6 on the serial port and the USB bulk
   interface. Each module runs in the serial engine behind a scripted port.
   The vectors are constructed from the layouts the part gives. Test numbers
   follow the part's sections. */

#include "testserialharness.h"
#include "../src/joystick/dji/SDL_dji_remote_proto.h"

#define REST 1024

static const uint8_t simulator[] = { 0x55, 0x0E, 0x04, 0x66, 0x0A, 0x06, 0xEB, 0x34, 0x40, 0x06, 0x24, 0x01, 0xD9, 0xEC };
static const uint8_t poll01[] = { 0x55, 0x0D, 0x04, 0x33, 0x0A, 0x06, 0xEB, 0x34, 0x40, 0x06, 0x01, 0x74, 0x24 };
static const uint8_t pair[] = {
    0x55, 0x0D, 0x04, 0x33, 0x0A, 0x06, 0xEB, 0x34, 0x40, 0x06, 0x01, 0x74, 0x24,
    0x55, 0x0D, 0x04, 0x33, 0x0A, 0x06, 0xEB, 0x34, 0x40, 0x06, 0x27, 0x40, 0x60
};
static const uint8_t mavic_ping[] = { 0x55, 0x0D, 0x04, 0x33, 0x0A, 0x0E, 0x03, 0x00, 0x40, 0x06, 0x01, 0xF4, 0x4A };
static const uint8_t phantom3_pings[] = {
    0x55, 0x0D, 0x04, 0x33, 0x0A, 0x03, 0x04, 0x00, 0x40, 0x00, 0x0E, 0xD0, 0xE3,
    0x55, 0x0D, 0x04, 0x33, 0x0A, 0x0E, 0x05, 0x00, 0x40, 0x06, 0x27, 0x58, 0x35
};
static const uint8_t phantom2_init[] = {
    0x55, 0xAA, 0x55, 0xAA, 0x1E, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x80, 0x00, 0x04, 0x04, 0x74,
    0x94, 0x35, 0x00, 0xD8, 0xC0, 0x41, 0x00, 0x30, 0xF6, 0x08, 0x00, 0x00, 0xF6, 0x69, 0x9C, 0x01, 0xE8
};
static const uint8_t phantom2_ping[] = {
    0x55, 0xAA, 0x55, 0xAA, 0x1E, 0x00, 0x01, 0x00, 0x00, 0x1C, 0x02, 0x00, 0x80, 0x00, 0x06, 0x01, 0x28,
    0x97, 0xAE, 0x03, 0x28, 0x36, 0xA4, 0x03, 0x28, 0x36, 0xA4, 0x03, 0xAB, 0xA7, 0x30, 0x00, 0x03, 0x53
};
static const uint8_t f5_seq1[] = { 0x55, 0x0D, 0x04, 0x33, 0x0A, 0x06, 0x01, 0x00, 0x40, 0x06, 0xF5, 0x8F, 0xCC };
/* RM330 test 1, the first payload of dji-rc-joystick's capture */
static const uint8_t f5_reply[] = {
    0x55, 0x1A, 0x04, 0xB1, 0x06, 0x0A, 0x01, 0x00, 0x80, 0x06, 0xF5, 0x00, 0xF7, 0x07, 0x17, 0x08,
    0x37, 0x08, 0xEA, 0x07, 0xA3, 0x08, 0x3E, 0x08, 0x8C, 0xDE
};
/* RC-N1 test 1 */
static const uint8_t rcn1_reply[] = {
    0x55, 0x26, 0x04, 0xD1, 0x06, 0x0A, 0xEB, 0x34, 0x80, 0x06, 0x01, 0x00, 0x00, 0x94, 0x06, 0x00, 0x00, 0x04, 0x00,
    0x6C, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7A, 0x28
};

static SDL_DJIRemoteState *State(Harness *h)
{
    return (SDL_DJIRemoteState *)h->state;
}

static size_t Frame(uint8_t *out, uint8_t sender, uint8_t receiver, uint16_t sequence, uint8_t type, uint8_t set, uint8_t id,
                    const uint8_t *payload, size_t length)
{
    return SDL_DJI_BuildFrame(out, SDL_DJI_MAX_FRAME, sender, receiver, sequence, type, set, id, payload, length);
}

/* The 38-byte 06/01 reply: values at bytes 13, 16, 19, 22 and 25 */
static size_t Channels38(uint8_t *out, uint16_t rh, uint16_t rv, uint16_t lv, uint16_t lh, uint16_t dial)
{
    const uint16_t values[5] = { rh, rv, lv, lh, dial };
    uint8_t payload[25];
    int i;

    memset(payload, 0, sizeof(payload));
    for (i = 0; i < 5; ++i) {
        payload[2 + 3 * i] = (uint8_t)(values[i] & 0xFF);
        payload[3 + 3 * i] = (uint8_t)(values[i] >> 8);
    }
    return Frame(out, 0x06, 0x0A, 0x34EB, 0x80, 0x06, 0x01, payload, sizeof(payload));
}

/* The 32-byte 06/01 reply: six values from byte 12 */
static size_t Channels32(uint8_t *out, const uint16_t *values)
{
    uint8_t payload[19];
    int i;

    memset(payload, 0, sizeof(payload));
    for (i = 0; i < 6; ++i) {
        payload[1 + 3 * i] = (uint8_t)(values[i] & 0xFF);
        payload[2 + 3 * i] = (uint8_t)(values[i] >> 8);
        payload[3 + 3 * i] = (uint8_t)(i + 1);
    }
    return Frame(out, 0x06, 0x0A, 0x34EB, 0x80, 0x06, 0x01, payload, sizeof(payload));
}

/* A 58-byte 06/27 reply from the given sender, with frame bytes 16 to 29 */
static size_t Status58(uint8_t *out, uint8_t sender, uint16_t sequence, const uint8_t *bytes16to29)
{
    uint8_t payload[45];

    memset(payload, 0, sizeof(payload));
    if (bytes16to29) {
        memcpy(payload + 5, bytes16to29, 14);
    }
    return Frame(out, sender, 0x0A, sequence, 0x80, 0x06, 0x27, payload, sizeof(payload));
}

/* A Phantom 3 06/27 reply: the four sticks, the dial, the counter, the mode and the buttons */
static size_t Phantom3(uint8_t *out, int16_t rv, int16_t rh, int16_t lv, int16_t lh, int16_t dial, uint8_t wheel, uint8_t mode, uint8_t buttons)
{
    uint8_t b[14];

    memset(b, 0, sizeof(b));
    b[0] = (uint8_t)((uint16_t)rv & 0xFF);
    b[1] = (uint8_t)((uint16_t)rv >> 8);
    b[2] = (uint8_t)((uint16_t)rh & 0xFF);
    b[3] = (uint8_t)((uint16_t)rh >> 8);
    b[4] = (uint8_t)((uint16_t)lv & 0xFF);
    b[5] = (uint8_t)((uint16_t)lv >> 8);
    b[6] = (uint8_t)((uint16_t)lh & 0xFF);
    b[7] = (uint8_t)((uint16_t)lh >> 8);
    b[8] = (uint8_t)((uint16_t)dial & 0xFF);
    b[9] = (uint8_t)((uint16_t)dial >> 8);
    b[10] = wheel;
    b[12] = mode;
    b[13] = buttons;
    return Status58(out, 0x0E, 5, b);
}

/* A Phantom 2 read: 55, zeros, and the seven values at 31, 35, 39, 43, 47, 51, 55 */
static void Phantom2(uint8_t *out, int16_t rh, int16_t rv, int16_t lv, int16_t lh, int16_t left, int16_t right, int16_t dial)
{
    const int16_t values[7] = { rh, rv, lv, lh, left, right, dial };
    int i;

    memset(out, 0, SDL_DJI_PHANTOM2_REPLY_LENGTH);
    out[0] = 0x55;
    for (i = 0; i < 7; ++i) {
        out[31 + 4 * i] = (uint8_t)((uint16_t)values[i] & 0xFF);
        out[32 + 4 * i] = (uint8_t)((uint16_t)values[i] >> 8);
    }
}

/* The 06/F5 reply with four values */
static size_t TestStick(uint8_t *out, uint16_t sequence, uint16_t rh, uint16_t rv, uint16_t lv, uint16_t lh)
{
    const uint16_t values[4] = { rh, rv, lv, lh };
    uint8_t payload[13];
    int i;

    memset(payload, 0, sizeof(payload));
    for (i = 0; i < 4; ++i) {
        payload[1 + 2 * i] = (uint8_t)(values[i] & 0xFF);
        payload[2 + 2 * i] = (uint8_t)(values[i] >> 8);
    }
    return Frame(out, 0x06, 0x0A, sequence, 0x80, 0x06, 0xF5, payload, sizeof(payload));
}

static size_t Model(uint8_t *out, uint8_t id, const char *model)
{
    uint8_t payload[66];

    memset(payload, 0, sizeof(payload));
    memcpy(payload, model, strlen(model));
    return Frame(out, 0x0D, 0x2A, 0x10, 0x40, 0x00, id, payload, sizeof(payload));
}

static bool Axes(Harness *h, int lx, int ly, int rx, int ry, int dial)
{
    return H_Axis(h, 0, SDL_DJI_AXIS_LEFT_X) == lx && H_Axis(h, 0, SDL_DJI_AXIS_LEFT_Y) == ly &&
           H_Axis(h, 0, SDL_DJI_AXIS_RIGHT_X) == rx && H_Axis(h, 0, SDL_DJI_AXIS_RIGHT_Y) == ry &&
           H_Axis(h, 0, SDL_DJI_AXIS_DIAL) == dial;
}

static bool MappingOf(const SDL_SerialMapInput *input, uint8_t kind, uint8_t target)
{
    return input->kind == kind && input->target == target;
}

static void CheckSharedMapping(const SDL_SerialIdentity *identity)
{
    CHECK(identity->type == SDL_SERIAL_TYPE_GAMEPAD && identity->has_mapping);
    CHECK(MappingOf(&identity->mapping.leftx, SDL_SERIAL_MAP_AXIS, 0));
    CHECK(MappingOf(&identity->mapping.lefty, SDL_SERIAL_MAP_AXIS, 1));
    CHECK(MappingOf(&identity->mapping.rightx, SDL_SERIAL_MAP_AXIS, 2));
    CHECK(MappingOf(&identity->mapping.righty, SDL_SERIAL_MAP_AXIS, 3));
    CHECK(MappingOf(&identity->mapping.y, SDL_SERIAL_MAP_AXIS_POSITIVE, 4));
    CHECK(MappingOf(&identity->mapping.b, SDL_SERIAL_MAP_AXIS_NEGATIVE, 4));
}

static void Feed(Harness *h, const uint8_t *data, size_t length)
{
    H_Feed(h, data, length);
}

/* RC-N1 */

static void BringUpRCN1(Harness *h)
{
    uint8_t reply[64];
    const size_t length = Channels38(reply, REST, REST, REST, REST, REST);

    H_Advance(h, h->now + SDL_DJI_SIMULATOR_SETTLE_MS);
    Feed(h, reply, length);
}

static Harness *PresentRCN1(void)
{
    Harness *h = H_Create(&SDL_DJIRemoteRCN1Module);

    H_Start(h);
    BringUpRCN1(h);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

/* RC-N1 test 5 and the silence rule */
static void TestRCN1Startup(void)
{
    Harness *h = H_Create(&SDL_DJIRemoteRCN1Module);
    uint8_t reply[64];
    const size_t length = Channels38(reply, REST, REST, REST, REST, REST);
    uint64_t t;

    /* 5 */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), simulator, sizeof(simulator), 0));
    /* Input during the 50 ms after 06/24 is dropped */
    H_Advance(h, 10);
    Feed(h, reply, length);
    CHECK(h->presence[0] == 0);
    H_Advance(h, 49);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 50);
    CHECK(H_IsWrite(H_NextCall(h), pair, sizeof(pair), 50));
    CHECK(H_NextCall(h) == NULL && h->presence[0] == 0);

    /* A reply triggers the next poll at once */
    H_Advance(h, 60);
    Feed(h, reply, length);
    CHECK(h->presence[0] == 1);
    CHECK(strcmp(h->identity[0].name, "DJI RC-N1") == 0);
    CHECK(h->identity[0].naxes == 5 && h->identity[0].nbuttons == 19 && h->identity[0].nhats == 0);
    CheckSharedMapping(&h->identity[0]);
    CHECK(h->identity[0].mapping.a.kind == SDL_SERIAL_MAP_NONE && h->identity[0].mapping.guide.kind == SDL_SERIAL_MAP_NONE);
    CHECK(H_IsWrite(H_NextCall(h), pair, sizeof(pair), 60));
    CHECK(H_NextCall(h) == NULL);

    /* 25 ms without a frame: exactly one resend, and the timer restarts */
    H_Advance(h, 84);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 85);
    CHECK(H_IsWrite(H_NextCall(h), pair, sizeof(pair), 85));
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 109);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 110);
    CHECK(H_IsWrite(H_NextCall(h), pair, sizeof(pair), 110));

    /* 2000 ms without a stick report: the remote goes and start-up begins again */
    H_Advance(h, 2059);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    H_Advance(h, 2060);
    CHECK(h->presence[0] == 0);
    CHECK(H_IsWrite(H_NextCall(h), simulator, sizeof(simulator), 2060));
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 2110);
    CHECK(H_IsWrite(H_NextCall(h), pair, sizeof(pair), 2110));
    Feed(h, reply, length);
    CHECK(h->presence[0] == 2);

    /* Resends keep the remote polled for as long as nothing answers */
    for (t = 2135; t < 4000; t += 25) {
        H_Advance(h, t);
    }
    CHECK(h->presence[0] == 2);
    H_Destroy(h);
}

/* RC-N1 tests 1 to 4 */
static void TestRCN1Reports(void)
{
    Harness *h = PresentRCN1();
    uint8_t frame[128];
    uint8_t bytes[14];
    size_t length;
    int list[4];

    /* 1 */
    Feed(h, rcn1_reply, sizeof(rcn1_reply));
    CHECK(Axes(h, 0, 32767, 32767, 0, 0));
    CHECK(Channels38(frame, 1684, REST, 364, REST, REST) == sizeof(rcn1_reply) && memcmp(frame, rcn1_reply, sizeof(rcn1_reply)) == 0);

    /* 2 */
    Feed(h, frame, Channels38(frame, 364, REST, REST, REST, REST));
    CHECK(Axes(h, 0, 0, -32767, 0, 0));
    Feed(h, frame, Channels38(frame, REST, 364, REST, REST, REST));
    CHECK(Axes(h, 0, 0, 0, 32767, 0));
    Feed(h, frame, Channels38(frame, REST, 1684, REST, REST, REST));
    CHECK(Axes(h, 0, 0, 0, -32767, 0));
    Feed(h, frame, Channels38(frame, REST, REST, 364, REST, REST));
    CHECK(Axes(h, 0, 32767, 0, 0, 0));
    Feed(h, frame, Channels38(frame, REST, REST, 1684, REST, REST));
    CHECK(Axes(h, 0, -32767, 0, 0, 0));
    Feed(h, frame, Channels38(frame, REST, REST, REST, 364, REST));
    CHECK(Axes(h, -32767, 0, 0, 0, 0));
    Feed(h, frame, Channels38(frame, REST, REST, REST, 1684, REST));
    CHECK(Axes(h, 32767, 0, 0, 0, 0));
    Feed(h, frame, Channels38(frame, REST, REST, REST, REST, 364));
    CHECK(Axes(h, 0, 0, 0, 0, -32767));
    Feed(h, frame, Channels38(frame, REST, REST, REST, REST, 1684));
    CHECK(Axes(h, 0, 0, 0, 0, 32767));
    /* Past the range clamps */
    Feed(h, frame, Channels38(frame, 0, 2000, REST, REST, REST));
    CHECK(Axes(h, 0, 0, -32767, -32767, 0));
    Feed(h, frame, Channels38(frame, REST, REST, REST, REST, REST));
    CHECK(Axes(h, 0, 0, 0, 0, 0));

    /* 3: id 27 instead of 01, or a bad CRC16 */
    {
        const int mark = h->npublished;
        uint8_t payload[25];

        memcpy(payload, rcn1_reply + 11, sizeof(payload));
        length = Frame(frame, 0x06, 0x0A, 0x34EB, 0x80, 0x06, 0x27, payload, sizeof(payload));
        Feed(h, frame, length);
        memcpy(frame, rcn1_reply, sizeof(rcn1_reply));
        frame[37] ^= 0x01;
        Feed(h, frame, sizeof(rcn1_reply));
        CHECK(h->npublished == mark && Axes(h, 0, 0, 0, 0, 0));
    }

    /* 4: byte 28 10 and byte 29 80, CRC16 7E E0 */
    memset(bytes, 0, sizeof(bytes));
    bytes[12] = 0x10;
    bytes[13] = 0x80;
    length = Status58(frame, 0x06, 0x34EB, bytes);
    CHECK(length == 58 && frame[56] == 0x7E && frame[57] == 0xE0);
    Feed(h, frame, length);
    list[0] = SDL_DJI_BUTTON_MODE + 1;
    list[1] = SDL_DJI_BUTTON_RCN1_BIT7;
    list[2] = -1;
    CHECK(H_OnlyButtons(h, 0, list));
    /* Byte 28 00 still reports the button, and the switch sits at the end */
    bytes[12] = 0x00;
    Feed(h, frame, Status58(frame, 0x06, 0x34EB, bytes));
    list[0] = SDL_DJI_BUTTON_MODE;
    CHECK(H_OnlyButtons(h, 0, list));
    /* Bit 5 of byte 28 is the other end, whatever else is set */
    bytes[12] = 0x30;
    bytes[13] = 0x00;
    Feed(h, frame, Status58(frame, 0x06, 0x34EB, bytes));
    CHECK(H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE + 2));
    /* Byte 27 set with byte 28 00 is the middle */
    bytes[11] = 0x01;
    bytes[12] = 0x00;
    Feed(h, frame, Status58(frame, 0x06, 0x34EB, bytes));
    CHECK(H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE + 1));
    bytes[11] = 0x00;
    /* The status bits one at a time */
    bytes[12] = 0x10;
    bytes[13] = 0x02;
    Feed(h, frame, Status58(frame, 0x06, 0x34EB, bytes));
    list[0] = SDL_DJI_BUTTON_MODE + 1;
    list[1] = SDL_DJI_BUTTON_RCN1_BIT1;
    CHECK(H_OnlyButtons(h, 0, list));
    bytes[13] = 0x04;
    Feed(h, frame, Status58(frame, 0x06, 0x34EB, bytes));
    list[1] = SDL_DJI_BUTTON_RCN1_BIT2;
    CHECK(H_OnlyButtons(h, 0, list));
    bytes[13] = 0x60;
    Feed(h, frame, Status58(frame, 0x06, 0x34EB, bytes));
    list[1] = SDL_DJI_BUTTON_RCN1_BITS56;
    CHECK(H_OnlyButtons(h, 0, list));
    /* Bit 5 or bit 6 alone is not the pair */
    bytes[13] = 0x20;
    Feed(h, frame, Status58(frame, 0x06, 0x34EB, bytes));
    CHECK(H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE + 1));
    bytes[13] = 0x40;
    Feed(h, frame, Status58(frame, 0x06, 0x34EB, bytes));
    CHECK(H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE + 1));
    /* A 57-byte 06/27 changes nothing */
    {
        const int mark = h->npublished;
        uint8_t payload[44];

        memset(payload, 0, sizeof(payload));
        payload[18] = 0x80;
        Feed(h, frame, Frame(frame, 0x06, 0x0A, 0x34EB, 0x80, 0x06, 0x27, payload, sizeof(payload)));
        CHECK(h->npublished == mark);
    }
    H_Destroy(h);
}

/* The status reply before presence waits for the first stick report */
static void TestRCN1StatusFirst(void)
{
    Harness *h = H_Create(&SDL_DJIRemoteRCN1Module);
    uint8_t frame[128];
    uint8_t bytes[14];

    H_Start(h);
    H_Advance(h, 50);
    memset(bytes, 0, sizeof(bytes));
    bytes[12] = 0x20;
    Feed(h, frame, Status58(frame, 0x06, 0x34EB, bytes));
    CHECK(h->presence[0] == 0 && h->npublished == 0);
    Feed(h, rcn1_reply, sizeof(rcn1_reply));
    CHECK(h->presence[0] == 1);
    CHECK(H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE + 2) && Axes(h, 0, 32767, 32767, 0, 0));
    H_Destroy(h);
}

/* RC-N1 test 7 and battery B */
static void TestRCN1BatteryB(void)
{
    Harness *h = PresentRCN1();
    uint8_t reply[64];

    Feed(h, rcn1_reply, sizeof(rcn1_reply));
    H_Advance(h, 100);
    H_SkipCalls(h);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 100));
    H_Advance(h, 1100);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 1100));
    CHECK(H_IsWrite(H_NextCall(h), simulator, sizeof(simulator), 1100));
    H_Advance(h, 1150);
    CHECK(H_IsWrite(H_NextCall(h), pair, sizeof(pair), 1150));
    CHECK(h->presence[0] == 0);
    Feed(h, reply, Channels38(reply, REST, REST, REST, REST, REST));
    CHECK(h->presence[0] == 2 && Axes(h, 0, 0, 0, 0, 0) && H_NoButtons(h, 0));
    H_Destroy(h);
}

static void TestRCN1BatteryA(void)
{
    Harness *h = PresentRCN1();
    static uint8_t right[64], status[64], wrong[64], bad[64];
    uint8_t bytes[14];
    H_Vector vectors[3], rest;
    size_t length;

    length = Channels38(right, 1684, REST, REST, REST, REST);
    H_SetVector(&vectors[0], "1 test vector", rcn1_reply, sizeof(rcn1_reply), 1, 0);
    H_SetVector(&vectors[1], "2 right gimbal right", right, length, 1, 0);
    memset(bytes, 0, sizeof(bytes));
    bytes[12] = 0x10;
    bytes[13] = 0x80;
    H_SetVector(&vectors[2], "4 status", status, Status58(status, 0x06, 0x34EB, bytes), 1, 0);
    H_BatteryA(h, &vectors[0], BringUpRCN1);
    H_BatteryA(h, &vectors[1], BringUpRCN1);
    H_BatteryA(h, &vectors[2], BringUpRCN1);

    H_SetVector(&rest, "right", right, length, 1, 0);
    {
        uint8_t payload[25];

        memcpy(payload, rcn1_reply + 11, sizeof(payload));
        H_BatteryA6(h, "3 id 27", wrong, Frame(wrong, 0x06, 0x0A, 0x34EB, 0x80, 0x06, 0x27, payload, sizeof(payload)), &rest);
    }
    memcpy(bad, rcn1_reply, sizeof(rcn1_reply));
    bad[36] ^= 0x40;
    H_BatteryA6(h, "3 bad CRC16", bad, sizeof(rcn1_reply), &rest);
    H_Destroy(h);
}

/* Mavic Mini remote: miniDjiController's ping to 0E, no 06/24 */
static void TestMavicMini(void)
{
    Harness *h = H_Create(&SDL_DJIRemoteMavicMiniModule);
    uint8_t reply[64];
    size_t length;

    H_Start(h);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), mavic_ping, sizeof(mavic_ping), 0));
    H_Advance(h, 24);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 25);
    CHECK(H_IsWrite(H_NextCall(h), mavic_ping, sizeof(mavic_ping), 25));

    /* A reply from 0E */
    length = Channels38(reply, REST, REST, REST, 1684, REST);
    reply[4] = 0x0E;
    reply[36] = 0;
    reply[37] = 0;
    {
        const uint16_t crc = SDL_DJI_CRC16(reply, 36);

        reply[36] = (uint8_t)(crc & 0xFF);
        reply[37] = (uint8_t)(crc >> 8);
    }
    H_Advance(h, 30);
    Feed(h, reply, length);
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "DJI Mavic Mini Remote") == 0);
    CHECK(h->identity[0].naxes == 5 && h->identity[0].nbuttons == 0);
    CheckSharedMapping(&h->identity[0]);
    CHECK(Axes(h, 32767, 0, 0, 0, 0));
    CHECK(H_IsWrite(H_NextCall(h), mavic_ping, sizeof(mavic_ping), 30));

    /* A 06/27 status does not press anything */
    {
        uint8_t bytes[14], frame[64];
        const int mark = h->npublished;

        memset(bytes, 0, sizeof(bytes));
        bytes[13] = 0xFF;
        Feed(h, frame, Status58(frame, 0x0E, 3, bytes));
        CHECK(h->npublished == mark);
    }

    /* Silence: the remote goes, and the ping goes on */
    H_Advance(h, 2029);
    CHECK(h->presence[0] == 1);
    H_Advance(h, 2030);
    CHECK(h->presence[0] == 0);
    H_SkipCalls(h);
    H_Advance(h, 2055);
    CHECK(H_IsWrite(H_NextCall(h), mavic_ping, sizeof(mavic_ping), 2055));
    H_Destroy(h);
}

/* Phantom 3 */

static void BringUpPhantom3(Harness *h)
{
    uint8_t reply[64];

    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 0, 0));
}

static Harness *PresentPhantom3(void)
{
    Harness *h = H_Create(&SDL_DJIRemotePhantom3Module);

    H_Start(h);
    BringUpPhantom3(h);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

/* Phantom 3 tests 1, 2, 5 and 7 */
static void TestPhantom3Startup(void)
{
    Harness *h = H_Create(&SDL_DJIRemotePhantom3Module);
    uint8_t reply[64];
    size_t length;
    int list[4];
    uint64_t t;

    /* 1: the two pings, no init frame, then every 10 ms */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), phantom3_pings, sizeof(phantom3_pings), 0));
    for (t = 10; t <= 50; t += 10) {
        H_Advance(h, t - 1);
        CHECK(H_NextCall(h) == NULL);
        H_Advance(h, t);
        CHECK(H_IsWrite(H_NextCall(h), phantom3_pings, sizeof(phantom3_pings), t));
    }

    /* 2 */
    length = Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 0x10, 0x82);
    CHECK(length == 58 && memcmp(reply, "\x55\x3A\x04\x70\x0E\x0A\x05\x00\x80\x06\x27", 11) == 0);
    CHECK(reply[16] == 0x34 && reply[17] == 0x08 && reply[56] == 0x65 && reply[57] == 0xE2);
    Feed(h, reply, length);
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "DJI Phantom 3 Remote") == 0);
    CHECK(h->identity[0].naxes == 5 && h->identity[0].nbuttons == 21);
    CheckSharedMapping(&h->identity[0]);
    CHECK(MappingOf(&h->identity[0].mapping.a, SDL_SERIAL_MAP_BUTTON, SDL_DJI_BUTTON_SHUTTER));
    CHECK(MappingOf(&h->identity[0].mapping.x, SDL_SERIAL_MAP_BUTTON, SDL_DJI_BUTTON_RECORD));
    CHECK(MappingOf(&h->identity[0].mapping.leftshoulder, SDL_SERIAL_MAP_BUTTON, SDL_DJI_BUTTON_C1));
    CHECK(MappingOf(&h->identity[0].mapping.rightshoulder, SDL_SERIAL_MAP_BUTTON, SDL_DJI_BUTTON_C2));
    CHECK(MappingOf(&h->identity[0].mapping.guide, SDL_SERIAL_MAP_BUTTON, SDL_DJI_BUTTON_HOME));
    CHECK(h->identity[0].mapping.back.kind == SDL_SERIAL_MAP_NONE);
    CHECK(Axes(h, 0, 0, 0, 0, 0));
    list[0] = SDL_DJI_BUTTON_HOME;
    list[1] = SDL_DJI_BUTTON_C1;
    list[2] = SDL_DJI_BUTTON_MODE + 1;
    list[3] = -1;
    CHECK(H_OnlyButtons(h, 0, list));

    /* 5: shorter replies and other ids change nothing, and the pings go on */
    {
        const int mark = h->npublished;
        uint8_t payload[45], frame[64];

        memcpy(payload, reply + 11, sizeof(payload));
        payload[18] = 0x04;
        Feed(h, frame, Frame(frame, 0x0E, 0x0A, 5, 0x80, 0x06, 0x27, payload, 44));
        Feed(h, frame, Frame(frame, 0x0E, 0x0A, 5, 0x80, 0x06, 0x28, payload, 45));
        CHECK(h->npublished == mark);
    }
    H_SkipCalls(h);
    H_Advance(h, 60);
    CHECK(H_IsWrite(H_NextCall(h), phantom3_pings, sizeof(phantom3_pings), 60));

    /* 7: the port goes and returns, and the loop resumes at once */
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 60));
    H_Advance(h, 1060);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 1060));
    CHECK(H_IsWrite(H_NextCall(h), phantom3_pings, sizeof(phantom3_pings), 1060));
    H_Advance(h, 1070);
    CHECK(H_IsWrite(H_NextCall(h), phantom3_pings, sizeof(phantom3_pings), 1070));
    BringUpPhantom3(h);
    CHECK(h->presence[0] == 2 && H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE));

    /* Silence clears presence and the loop goes on */
    H_Advance(h, 3069);
    CHECK(h->presence[0] == 2);
    H_Advance(h, 3070);
    CHECK(h->presence[0] == 0);
    H_SkipCalls(h);
    H_Advance(h, 3080);
    CHECK(H_IsWrite(H_NextCall(h), phantom3_pings, sizeof(phantom3_pings), 3080));
    H_Destroy(h);
}

/* Phantom 3 tests 3, 4 and 6 */
static void TestPhantom3Controls(void)
{
    Harness *h = PresentPhantom3();
    static const int buttons[8] = { -1, SDL_DJI_BUTTON_C1, SDL_DJI_BUTTON_C2, SDL_DJI_BUTTON_DIAL_PRESS, SDL_DJI_BUTTON_PLAYBACK,
                                    SDL_DJI_BUTTON_SHUTTER, SDL_DJI_BUTTON_RECORD, SDL_DJI_BUTTON_HOME };
    uint8_t reply[64];
    int list[3];
    int bit;

    /* 3 */
    for (bit = 1; bit < 8; ++bit) {
        Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 0, (uint8_t)(1 << bit)));
        list[0] = buttons[bit];
        list[1] = SDL_DJI_BUTTON_MODE;
        list[2] = -1;
        CHECK(H_OnlyButtons(h, 0, list));
    }
    /* Bit 0 is not a button */
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 0, 0x01));
    CHECK(H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 16, 0));
    CHECK(H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE + 1));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 31, 0));
    CHECK(H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE + 1));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 32, 0));
    CHECK(H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE + 2));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 15, 0));
    CHECK(H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE));
    /* A negative mode byte keeps the last position */
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 32, 0));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 0x90, 0));
    CHECK(H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE + 2));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 0, 0));

    /* 4: the counter up by one, then down by one */
    H_Advance(h, 1000);
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 5, 0, 0));
    list[0] = SDL_DJI_BUTTON_MODE;
    list[1] = SDL_DJI_BUTTON_DIAL_UP;
    list[2] = -1;
    CHECK(H_OnlyButtons(h, 0, list));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 5, 0, 0));
    H_Advance(h, 1099);
    CHECK(H_Button(h, 0, SDL_DJI_BUTTON_DIAL_UP));
    H_Advance(h, 1100);
    CHECK(!H_Button(h, 0, SDL_DJI_BUTTON_DIAL_UP));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 5, 0, 0));
    CHECK(!H_Button(h, 0, SDL_DJI_BUTTON_DIAL_UP) && !H_Button(h, 0, SDL_DJI_BUTTON_DIAL_DOWN));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 4, 0, 0));
    CHECK(H_Button(h, 0, SDL_DJI_BUTTON_DIAL_DOWN) && !H_Button(h, 0, SDL_DJI_BUTTON_DIAL_UP));
    H_Advance(h, 1200);
    CHECK(!H_Button(h, 0, SDL_DJI_BUTTON_DIAL_DOWN));
    /* The counter wraps: 127 to -128 is a step up */
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0x7F, 0, 0));
    H_Advance(h, 1400);
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0x80, 0, 0));
    CHECK(H_Button(h, 0, SDL_DJI_BUTTON_DIAL_UP) && !H_Button(h, 0, SDL_DJI_BUTTON_DIAL_DOWN));
    H_Advance(h, 1600);

    /* 6 */
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 750, 0, 0x80, 0, 0));
    CHECK(Axes(h, -32767, 0, 0, 0, 0));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 3450, 0, 0x80, 0, 0));
    CHECK(Axes(h, 32767, 0, 0, 0, 0));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 750, 2100, 0, 0x80, 0, 0));
    CHECK(Axes(h, 0, 32767, 0, 0, 0));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 3450, 2100, 0, 0x80, 0, 0));
    CHECK(Axes(h, 0, -32767, 0, 0, 0));
    Feed(h, reply, Phantom3(reply, 2100, 750, 2100, 2100, 0, 0x80, 0, 0));
    CHECK(Axes(h, 0, 0, -32767, 0, 0));
    Feed(h, reply, Phantom3(reply, 2100, 3450, 2100, 2100, 0, 0x80, 0, 0));
    CHECK(Axes(h, 0, 0, 32767, 0, 0));
    Feed(h, reply, Phantom3(reply, 750, 2100, 2100, 2100, 0, 0x80, 0, 0));
    CHECK(Axes(h, 0, 0, 0, 32767, 0));
    Feed(h, reply, Phantom3(reply, 3450, 2100, 2100, 2100, 0, 0x80, 0, 0));
    CHECK(Axes(h, 0, 0, 0, -32767, 0));
    /* Half of 1350 either way reads half scale, rounded toward 0 */
    Feed(h, reply, Phantom3(reply, 2775, 2775, 2775, 2775, 0, 0x80, 0, 0));
    CHECK(Axes(h, 16383, -16383, 16383, -16383, 0));
    Feed(h, reply, Phantom3(reply, 1425, 1425, 1425, 1425, 0, 0x80, 0, 0));
    CHECK(Axes(h, -16383, 16383, -16383, 16383, 0));
    /* The dial passes its raw value through, clamped at -32767 */
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 1234, 0x80, 0, 0));
    CHECK(Axes(h, 0, 0, 0, 0, 1234));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, -32768, 0x80, 0, 0));
    CHECK(Axes(h, 0, 0, 0, 0, -32767));
    H_Destroy(h);
}

/* A negative mode byte in the first report after start names no position */
static void TestPhantom3FirstMode(void)
{
    Harness *h = H_Create(&SDL_DJIRemotePhantom3Module);
    uint8_t reply[64];

    H_Start(h);
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 0x90, 0));
    CHECK(h->presence[0] == 1 && H_NoButtons(h, 0));
    Feed(h, reply, Phantom3(reply, 2100, 2100, 2100, 2100, 0, 0, 0x10, 0));
    CHECK(H_OnlyButton(h, 0, SDL_DJI_BUTTON_MODE + 1));
    H_Destroy(h);
}

static void TestPhantom3BatteryA(void)
{
    Harness *h = PresentPhantom3();
    static uint8_t first[64], second[64], shorter[64];
    H_Vector vectors[2], valid;
    uint8_t payload[45];

    H_SetVector(&vectors[0], "2 test vector", first, Phantom3(first, 2100, 2100, 2100, 2100, 0, 0, 0x10, 0x82), 1, 0);
    H_SetVector(&vectors[1], "6 left stick left", second, Phantom3(second, 2100, 2100, 2100, 750, 0, 0, 0, 0), 1, 0);
    H_BatteryA(h, &vectors[0], BringUpPhantom3);
    H_BatteryA(h, &vectors[1], BringUpPhantom3);
    H_SetVector(&valid, "valid", first, 58, 1, 0);
    memcpy(payload, first + 11, sizeof(payload));
    H_BatteryA6(h, "5 short", shorter, Frame(shorter, 0x0E, 0x0A, 5, 0x80, 0x06, 0x27, payload, 44), &valid);
    H_Destroy(h);
}

/* Phantom 2 */

static void BringUpPhantom2(Harness *h)
{
    uint8_t read[SDL_DJI_PHANTOM2_REPLY_LENGTH];

    H_Advance(h, h->now + 2005);
    Phantom2(read, 0, 0, 0, 0, 0, 0, 0);
    Feed(h, read, sizeof(read));
    H_Advance(h, h->now + 10);
}

/* Phantom 2 tests 1 and 4, and the silence rule */
static void TestPhantom2Startup(void)
{
    Harness *h = H_Create(&SDL_DJIRemotePhantom2Module);
    uint8_t read[SDL_DJI_PHANTOM2_REPLY_LENGTH];
    uint64_t t;

    /* 1 */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), phantom2_init, sizeof(phantom2_init), 0));
    H_Advance(h, 1999);
    CHECK(H_NextCall(h) == NULL);
    for (t = 2000; t <= 2050; t += 10) {
        H_Advance(h, t);
        CHECK(H_IsWrite(H_NextCall(h), phantom2_ping, sizeof(phantom2_ping), t));
        CHECK(H_NextCall(h) == NULL);
    }
    /* A reply fed within a window decodes at the next pass */
    Phantom2(read, 1000, 0, 0, 0, 0, 0, 0);
    H_Advance(h, 2053);
    Feed(h, read, sizeof(read));
    CHECK(h->presence[0] == 0);
    H_Advance(h, 2060);
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "DJI Phantom 2 Remote") == 0);
    CHECK(h->identity[0].naxes == 5 && h->identity[0].nbuttons == 15);
    CheckSharedMapping(&h->identity[0]);
    CHECK(Axes(h, 0, 0, 32767, 0, 0));
    {
        /* Both levers at 0: CL on the left, ATTI on the right */
        int list[3];

        list[0] = SDL_DJI_BUTTON_MODE + 1;
        list[1] = SDL_DJI_BUTTON_LEVER + 1;
        list[2] = -1;
        CHECK(H_OnlyButtons(h, 0, list));
    }

    /* 4: the port goes, then the init frame once more and 2000 ms */
    H_SkipCalls(h);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 2060));
    H_Advance(h, 3060);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 3060));
    CHECK(H_IsWrite(H_NextCall(h), phantom2_init, sizeof(phantom2_init), 3060));
    H_Advance(h, 5059);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 5060);
    CHECK(H_IsWrite(H_NextCall(h), phantom2_ping, sizeof(phantom2_ping), 5060));

    /* 2000 ms without a reply: the init frame again */
    H_SkipCalls(h);
    H_Advance(h, 7059);
    H_SkipCalls(h);
    H_Advance(h, 7060);
    CHECK(H_IsWrite(H_NextCall(h), phantom2_init, sizeof(phantom2_init), 7060));
    H_Destroy(h);
}

/* Phantom 2 tests 2 and 3, and the split rule */
static void TestPhantom2Reads(void)
{
    Harness *h = H_Create(&SDL_DJIRemotePhantom2Module);
    uint8_t read[SDL_DJI_PHANTOM2_REPLY_LENGTH + 1];
    uint64_t t = 2000;
    int mark;
    int list[3];
    size_t k;

    H_Start(h);
    H_Advance(h, t);
    Phantom2(read, 0, 0, 0, 0, -780, 780, 0);
    Feed(h, read, 76);
    t += 10;
    H_Advance(h, t);
    CHECK(h->presence[0] == 1);
    list[0] = SDL_DJI_BUTTON_MODE;
    list[1] = SDL_DJI_BUTTON_LEVER + 2;
    list[2] = -1;
    CHECK(H_OnlyButtons(h, 0, list));

    /* 2: each value alone at -1000, 0 and 1000 */
    {
        static const int16_t values[3] = { -1000, 0, 1000 };
        int i;

        for (i = 0; i < 3; ++i) {
            const int scaled = (values[i] < 0) ? -32767 : (values[i] > 0) ? 32767 : 0;

            Phantom2(read, values[i], 0, 0, 0, -780, 780, 0);
            Feed(h, read, 76);
            H_Advance(h, t += 10);
            CHECK(Axes(h, 0, 0, scaled, 0, 0));
            Phantom2(read, 0, values[i], 0, 0, -780, 780, 0);
            Feed(h, read, 76);
            H_Advance(h, t += 10);
            CHECK(Axes(h, 0, 0, 0, -scaled, 0));
            Phantom2(read, 0, 0, values[i], 0, -780, 780, 0);
            Feed(h, read, 76);
            H_Advance(h, t += 10);
            CHECK(Axes(h, 0, -scaled, 0, 0, 0));
            Phantom2(read, 0, 0, 0, values[i], -780, 780, 0);
            Feed(h, read, 76);
            H_Advance(h, t += 10);
            CHECK(Axes(h, scaled, 0, 0, 0, 0));
            Phantom2(read, 0, 0, 0, 0, -780, 780, values[i]);
            Feed(h, read, 76);
            H_Advance(h, t += 10);
            CHECK(Axes(h, 0, 0, 0, 0, scaled));
        }
    }
    /* The levers: -780, 0 and 780, and any other value keeps the position */
    Phantom2(read, 0, 0, 0, 0, 0, 0, 0);
    Feed(h, read, 76);
    H_Advance(h, t += 10);
    list[0] = SDL_DJI_BUTTON_MODE + 1;
    list[1] = SDL_DJI_BUTTON_LEVER + 1;
    CHECK(H_OnlyButtons(h, 0, list));
    Phantom2(read, 0, 0, 0, 0, 780, -780, 0);
    Feed(h, read, 76);
    H_Advance(h, t += 10);
    list[0] = SDL_DJI_BUTTON_MODE + 2;
    list[1] = SDL_DJI_BUTTON_LEVER;
    CHECK(H_OnlyButtons(h, 0, list));
    Phantom2(read, 0, 0, 0, 0, 100, -5, 0);
    Feed(h, read, 76);
    H_Advance(h, t += 10);
    CHECK(H_OnlyButtons(h, 0, list));

    /* 3: 75 or 77 bytes, or a first byte other than 55 */
    mark = h->npublished;
    Phantom2(read, 1000, 0, 0, 0, 780, -780, 0);
    Feed(h, read, 75);
    H_Advance(h, t += 10);
    read[76] = 0x00;
    Feed(h, read, 77);
    H_Advance(h, t += 10);
    read[0] = 0x54;
    Feed(h, read, 76);
    H_Advance(h, t += 10);
    /* Two reads in one window make 152 bytes */
    read[0] = 0x55;
    Feed(h, read, 76);
    Feed(h, read, 76);
    H_Advance(h, t += 10);
    CHECK(h->npublished == mark);

    /* A reply split at every point within one window decodes once */
    for (k = 1; k < 76; ++k) {
        Phantom2(read, (int16_t)(k % 2 ? 500 : -500), 0, 0, 0, 780, -780, 0);
        mark = h->npublished;
        Feed(h, read, k);
        Feed(h, read + k, 76 - k);
        CHECK(h->npublished == mark);
        H_Advance(h, t += 10);
        CHECK(h->npublished == mark + 1 && H_Axis(h, 0, SDL_DJI_AXIS_RIGHT_X) == (k % 2 ? 16383 : -16383));
    }
    /* The same reply in the next window changes nothing */
    mark = h->npublished;
    Feed(h, read, 76);
    H_Advance(h, t += 10);
    CHECK(h->npublished == mark);
    /* Bytes past the count never count */
    Phantom2(read, 250, 0, 0, 0, 780, -780, 0);
    {
        uint8_t padded[76 + 128];

        memcpy(padded, read, 76);
        memset(padded + 76, 0x55, sizeof(padded) - 76);
        H_FeedPadded(h, padded, 76);
        H_Advance(h, t += 10);
        CHECK(H_Axis(h, 0, SDL_DJI_AXIS_RIGHT_X) == 8191);
    }
    H_Destroy(h);
}

static void TestPhantom2BatteryB(void)
{
    Harness *h = H_Create(&SDL_DJIRemotePhantom2Module);

    H_Start(h);
    BringUpPhantom2(h);
    CHECK(h->presence[0] == 1);
    H_LosePort(h);
    CHECK(h->presence[0] == 0);
    H_Advance(h, h->now + SDL_SERIAL_RETRY_MS);
    BringUpPhantom2(h);
    CHECK(h->presence[0] == 2);
    H_Destroy(h);
}

/* Bulk */

static void BringUpBulk(Harness *h)
{
    uint16_t values[6] = { REST, REST, REST, REST, REST, REST };
    uint8_t reply[64];

    H_Advance(h, h->now + SDL_DJI_BULK_SETTLE_MS);
    Feed(h, reply, Channels32(reply, values));
}

static Harness *PresentBulk(void)
{
    Harness *h = H_Create(&SDL_DJIRemoteBulkModule);

    H_Start(h);
    BringUpBulk(h);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

/* RM330 tests 1 and 4, and the 06/01 path */
static void TestBulkStartup(void)
{
    Harness *h = H_Create(&SDL_DJIRemoteBulkModule);
    uint8_t frame[64];
    uint64_t t;

    /* 4: 06/24, then 06/01 polls */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), simulator, sizeof(simulator), 0));
    H_Advance(h, 99);
    CHECK(H_NextCall(h) == NULL);
    for (t = 100; t < 300; t += 10) {
        H_Advance(h, t);
        CHECK(H_IsWrite(H_NextCall(h), poll01, sizeof(poll01), t));
        CHECK(H_NextCall(h) == NULL);
    }
    /* No 32-byte 06/01 reply for 200 ms: 06/F5, sequence 1 on */
    H_Advance(h, 300);
    CHECK(H_IsWrite(H_NextCall(h), f5_seq1, sizeof(f5_seq1), 300));
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 311);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 312);
    CHECK(H_IsWrite(H_NextCall(h), frame, Frame(frame, 0x0A, 0x06, 2, 0x40, 0x06, 0xF5, NULL, 0), 312));

    /* 1: the capture's first payload */
    H_Advance(h, 315);
    Feed(h, f5_reply, sizeof(f5_reply));
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "DJI RC (RM330)") == 0);
    CHECK(h->identity[0].naxes == 6 && h->identity[0].nbuttons == 0);
    CheckSharedMapping(&h->identity[0]);
    /* Right horizontal 2039, right vertical 2071 and left horizontal 2026 sit in the dead zone */
    CHECK(Axes(h, 0, -1151, 0, 0, 0));
    /* The 06/F5 loop keeps its period: no poll on a reply */
    CHECK(H_NextCall(h) == NULL);
    /* Nor on a late 32-byte 06/01 answer, whose values count */
    {
        uint16_t values[6] = { REST, REST, REST, REST, REST, REST };

        H_Advance(h, 318);
        Feed(h, frame, Channels32(frame, values));
        CHECK(Axes(h, 0, 0, 0, 0, 0) && H_NextCall(h) == NULL);
    }
    H_Advance(h, 324);
    CHECK(H_IsWrite(H_NextCall(h), frame, Frame(frame, 0x0A, 0x06, 3, 0x40, 0x06, 0xF5, NULL, 0), 324));
    /* No 06/24 in the 06/F5 loop while the remote answers */
    for (t = 330; t <= 3300; t += 10) {
        H_Advance(h, t);
        if (t % 100 == 0) {
            Feed(h, f5_reply, sizeof(f5_reply));
        }
    }
    CHECK(h->presence[0] == 1);
    {
        int i, polls = 0;

        for (i = h->cursor; i < h->ncalls; ++i) {
            CHECK(h->calls[i].length == 13 && h->calls[i].data[10] == 0xF5);
            ++polls;
        }
        /* One every 12 ms from 336 to 3300 */
        CHECK(polls == (3300 - 336) / 12 + 1);
    }
    H_Destroy(h);
}

/* The 06/01 path: a 32-byte reply keeps 06/01, the poll goes out at once,
 * and 06/24 goes again every 3000 ms */
static void TestBulkChannels(void)
{
    Harness *h = H_Create(&SDL_DJIRemoteBulkModule);
    uint16_t values[6] = { 1684, 364, 364, 1684, 1024, 364 };
    uint8_t reply[64];
    const size_t length = Channels32(reply, values);
    uint64_t t;
    int simulators = 0, f5 = 0;

    H_Start(h);
    H_Advance(h, 150);
    H_SkipCalls(h);
    Feed(h, reply, length);
    CHECK(h->presence[0] == 1);
    CHECK(H_Axis(h, 0, SDL_DJI_AXIS_RIGHT_X) == 32767 && H_Axis(h, 0, SDL_DJI_AXIS_RIGHT_Y) == 32767);
    CHECK(H_Axis(h, 0, SDL_DJI_AXIS_LEFT_Y) == 32767 && H_Axis(h, 0, SDL_DJI_AXIS_LEFT_X) == 32767);
    CHECK(H_Axis(h, 0, SDL_DJI_AXIS_DIAL) == 0 && H_Axis(h, 0, SDL_DJI_AXIS_DIAL2) == -32767);
    CHECK(H_IsWrite(H_NextCall(h), poll01, sizeof(poll01), 150));
    for (t = 160; t <= 6100; t += 10) {
        H_Advance(h, t);
        if (t % 500 == 0) {
            Feed(h, reply, length);
        }
    }
    {
        int polls = 0;

        while (h->cursor < h->ncalls) {
            const H_Call *call = H_NextCall(h);

            if (call->length == sizeof(simulator) && memcmp(call->data, simulator, sizeof(simulator)) == 0) {
                ++simulators;
                CHECK(call->time == 3000 || call->time == 6000);
            }
            if (call->length == 13 && call->data[10] == 0xF5) {
                ++f5;
            }
            if (call->length == sizeof(poll01) && memcmp(call->data, poll01, sizeof(poll01)) == 0) {
                ++polls;
            }
        }
        CHECK(simulators == 2 && f5 == 0);
        /* One poll every 10 ms from 160 on, past 255 writes, and one at
         * each of the 12 replies */
        CHECK(polls == (6100 - 160) / 10 + 1 + 12);
    }
    CHECK(h->presence[0] == 1);

    /* 1000 ms without a stick report: the remote goes and start-up begins again */
    H_Advance(h, 6999);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    H_Advance(h, 7000);
    CHECK(h->presence[0] == 0);
    CHECK(H_IsWrite(H_NextCall(h), simulator, sizeof(simulator), 7000));
    H_Destroy(h);
}

/* A stick reply during the 100 ms after 06/24 counts, and the wait goes on */
static void TestBulkEarlyReply(void)
{
    Harness *h = H_Create(&SDL_DJIRemoteBulkModule);
    uint16_t values[6] = { REST, REST, REST, REST, REST, REST };
    uint8_t reply[64];

    H_Start(h);
    H_Advance(h, 50);
    H_SkipCalls(h);
    Feed(h, reply, Channels32(reply, values));
    CHECK(h->presence[0] == 1 && H_NextCall(h) == NULL);
    H_Advance(h, 99);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 100);
    CHECK(H_IsWrite(H_NextCall(h), poll01, sizeof(poll01), 100));
    H_Advance(h, 110);
    CHECK(H_IsWrite(H_NextCall(h), poll01, sizeof(poll01), 110));
    H_Destroy(h);
}

/* RM330 tests 3, 5 and 6 */
static void TestBulkReports(void)
{
    Harness *h = H_Create(&SDL_DJIRemoteBulkModule);
    uint8_t frame[256], two[128];
    size_t length, first;

    /* 3: the identity frames name the remote */
    H_Start(h);
    H_Advance(h, 100);
    Feed(h, frame, Model(frame, 0x81, "rc331"));
    Feed(h, frame, Model(frame, 0x82, "rm330"));
    Feed(h, f5_reply, sizeof(f5_reply));
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "DJI RC (RM330)") == 0);
    H_Destroy(h);

    h = H_Create(&SDL_DJIRemoteBulkModule);
    H_Start(h);
    H_Advance(h, 100);
    Feed(h, frame, Model(frame, 0x81, "rc331"));
    Feed(h, f5_reply, sizeof(f5_reply));
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "DJI RC 2") == 0);
    H_Destroy(h);

    h = H_Create(&SDL_DJIRemoteBulkModule);
    H_Start(h);
    H_Advance(h, 100);
    Feed(h, frame, Model(frame, 0x81, "rc221"));
    Feed(h, f5_reply, sizeof(f5_reply));
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "DJI RC (RM330)") == 0);

    /* 5: one and a half frames, then the rest */
    first = TestStick(two, 7, 483, 2048, 2048, 2048);
    length = first + TestStick(two + first, 8, 2048, 2048, 2048, 3613);
    {
        const int mark = h->npublished;

        Feed(h, two, first + 13);
        CHECK(h->npublished == mark + 1 && Axes(h, 0, 0, -32767, 0, 0));
        Feed(h, two + first + 13, length - first - 13);
        CHECK(h->npublished == mark + 2 && Axes(h, 32767, 0, 0, 0, 0));
    }

    /* 6 */
    Feed(h, frame, TestStick(frame, 9, 3613, 2048, 2048, 2048));
    CHECK(Axes(h, 0, 0, 32767, 0, 0));
    Feed(h, frame, TestStick(frame, 9, 2048, 483, 2048, 2048));
    CHECK(Axes(h, 0, 0, 0, 32767, 0));
    Feed(h, frame, TestStick(frame, 9, 2048, 3613, 2048, 2048));
    CHECK(Axes(h, 0, 0, 0, -32767, 0));
    Feed(h, frame, TestStick(frame, 9, 2048, 2048, 483, 2048));
    CHECK(Axes(h, 0, 32767, 0, 0, 0));
    Feed(h, frame, TestStick(frame, 9, 2048, 2048, 3613, 2048));
    CHECK(Axes(h, 0, -32767, 0, 0, 0));
    Feed(h, frame, TestStick(frame, 9, 2048, 2048, 2048, 483));
    CHECK(Axes(h, -32767, 0, 0, 0, 0));
    /* The dead zone: 31 counts read 0, 32 do not */
    Feed(h, frame, TestStick(frame, 9, 2048 + 31, 2048 - 31, 2048, 2048));
    CHECK(Axes(h, 0, 0, 0, 0, 0));
    Feed(h, frame, TestStick(frame, 9, 2048 + 32, 2048 - 32, 2048, 2048));
    CHECK(Axes(h, 0, 0, 669, 669, 0));
    Feed(h, frame, TestStick(frame, 9, 2048, 2048, 2048 + 31, 2048 - 31));
    CHECK(Axes(h, 0, 0, 0, 0, 0));
    Feed(h, frame, TestStick(frame, 9, 2048, 2048, 2048 + 32, 2048 - 32));
    CHECK(Axes(h, -669, -669, 0, 0, 0));
    /* A 06/F5 of another length changes nothing */
    {
        const int mark = h->npublished;
        uint8_t payload[14];

        memset(payload, 0, sizeof(payload));
        Feed(h, frame, Frame(frame, 0x06, 0x0A, 9, 0x80, 0x06, 0xF5, payload, 14));
        Feed(h, frame, Frame(frame, 0x06, 0x0A, 9, 0x80, 0x06, 0xF5, payload, 12));
        CHECK(h->npublished == mark);
    }
    H_Destroy(h);
}

/* RM330 test 7 and battery B */
static void TestBulkBatteryB(void)
{
    Harness *h = PresentBulk();

    H_Advance(h, 200);
    H_SkipCalls(h);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 200));
    H_Advance(h, 1200);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 1200));
    CHECK(H_IsWrite(H_NextCall(h), simulator, sizeof(simulator), 1200));
    H_Advance(h, 1300);
    CHECK(H_IsWrite(H_NextCall(h), poll01, sizeof(poll01), 1300));
    CHECK(h->presence[0] == 0);
    Feed(h, f5_reply, sizeof(f5_reply));
    CHECK(h->presence[0] == 2);
    H_Destroy(h);
}

static void TestBulkBatteryA(void)
{
    Harness *h = PresentBulk();
    static uint8_t channels[64], stick[64], bad[64];
    uint16_t values[6] = { 1684, 1024, 1024, 1024, 1024, 1024 };
    H_Vector vectors[2], valid;

    H_SetVector(&vectors[0], "06/01 right", channels, Channels32(channels, values), 1, 0);
    H_SetVector(&vectors[1], "1 capture payload", f5_reply, sizeof(f5_reply), 1, 0);
    H_BatteryA(h, &vectors[0], BringUpBulk);
    H_BatteryA(h, &vectors[1], BringUpBulk);
    H_SetVector(&valid, "valid", stick, TestStick(stick, 3, 483, 2048, 2048, 2048), 1, 0);
    memcpy(bad, f5_reply, sizeof(f5_reply));
    bad[3] ^= 0x10;
    H_BatteryA6(h, "bad header", bad, sizeof(f5_reply), &valid);
    H_Destroy(h);
}

/* A remote that comes back after silence starts from rest: no status bit,
 * mode or lever position survives from before */
static void TestRestartClears(void)
{
    Harness *h = PresentRCN1();
    uint8_t frame[128], bytes[14];
    uint8_t read[SDL_DJI_PHANTOM2_REPLY_LENGTH];
    uint64_t start;

    memset(bytes, 0, sizeof(bytes));
    bytes[12] = 0x10;
    bytes[13] = 0x80;
    Feed(h, frame, Status58(frame, 0x06, 0x34EB, bytes));
    Feed(h, rcn1_reply, sizeof(rcn1_reply));
    CHECK(H_Button(h, 0, SDL_DJI_BUTTON_RCN1_BIT7) && H_Button(h, 0, SDL_DJI_BUTTON_MODE + 1));
    start = h->now;
    H_Advance(h, start + SDL_DJI_SERIAL_SILENCE_MS);
    CHECK(h->presence[0] == 0);
    H_Advance(h, start + SDL_DJI_SERIAL_SILENCE_MS + SDL_DJI_SIMULATOR_SETTLE_MS);
    Feed(h, frame, Channels38(frame, REST, REST, REST, REST, REST));
    CHECK(h->presence[0] == 2 && H_NoButtons(h, 0) && Axes(h, 0, 0, 0, 0, 0));
    H_Destroy(h);

    h = H_Create(&SDL_DJIRemotePhantom2Module);
    H_Start(h);
    H_Advance(h, 2000);
    Phantom2(read, 0, 0, 0, 0, 780, -780, 0);
    Feed(h, read, sizeof(read));
    H_Advance(h, 2010);
    CHECK(h->presence[0] == 1 && H_Button(h, 0, SDL_DJI_BUTTON_MODE + 2) && H_Button(h, 0, SDL_DJI_BUTTON_LEVER));
    /* Silence: init at 4010, the loop at 6010 */
    H_Advance(h, 6010);
    CHECK(h->presence[0] == 0);
    /* Levers between positions keep the last one, and there is none now */
    Phantom2(read, 0, 0, 0, 0, 100, -5, 0);
    Feed(h, read, sizeof(read));
    H_Advance(h, 6020);
    CHECK(h->presence[0] == 2 && H_NoButtons(h, 0));
    H_Destroy(h);
}

/* Silence while a write is still pending restarts cleanly */
static void TestPendingWrite(void)
{
    Harness *h = H_Create(&SDL_DJIRemoteRCN1Module);
    uint8_t reply[64];
    const size_t length = Channels38(reply, REST, REST, REST, REST, REST);

    h->pend_writes = true;
    H_Start(h);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), simulator, sizeof(simulator), 0));
    /* The settle waits for the write to finish */
    H_Advance(h, 100);
    CHECK(H_NextCall(h) == NULL);
    H_CompleteWrite(h, true);
    H_Advance(h, 149);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 150);
    CHECK(H_IsWrite(H_NextCall(h), pair, sizeof(pair), 150));
    /* The resend timer starts when the poll leaves. A reply while the poll
       is still on its way sends nothing more. */
    H_Advance(h, 190);
    CHECK(H_NextCall(h) == NULL);
    Feed(h, reply, length);
    CHECK(h->presence[0] == 1 && H_NextCall(h) == NULL);
    H_CompleteWrite(h, true);
    H_Advance(h, 214);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 215);
    CHECK(H_IsWrite(H_NextCall(h), pair, sizeof(pair), 215));
    /* A failed write counts as sent */
    H_CompleteWrite(h, false);
    H_Advance(h, 240);
    CHECK(H_IsWrite(H_NextCall(h), pair, sizeof(pair), 240));
    H_Destroy(h);
}

static void TestHelpers(void)
{
    SDL_DJIFrame frame;
    uint8_t buffer[128];
    char model[SDL_DJI_MODEL_LENGTH];
    uint16_t values[6];
    uint8_t payload[20];

    CHECK(SDL_DJI_ScaleAxis(1024, 1024, 660) == 0);
    CHECK(SDL_DJI_ScaleAxis(1354, 1024, 660) == 16383);
    CHECK(SDL_DJI_ScaleAxis(694, 1024, 660) == -16383);
    CHECK(SDL_DJI_ScaleAxis(65535, 0, 1) == 32767 && SDL_DJI_ScaleAxis(-65535, 0, 1) == -32767);
    CHECK(SDL_DJI_ScaleAxis(5, 0, 0) == 0);

    CHECK(strcmp(SDL_DJI_NameForModel("rm330", "x"), "DJI RC (RM330)") == 0);
    CHECK(strcmp(SDL_DJI_NameForModel("rc331", "x"), "DJI RC 2") == 0);
    CHECK(strcmp(SDL_DJI_NameForModel("rm3300", "x"), "x") == 0);
    CHECK(strcmp(SDL_DJI_NameForModel("", "x"), "x") == 0);

    SDL_DJI_CheckFrame(buffer, Model(buffer, 0x81, "rm330"), &frame);
    CHECK(SDL_DJI_DecodeModel(&frame, model, sizeof(model)) && strcmp(model, "rm330") == 0);
    CHECK(!SDL_DJI_DecodeModel(&frame, model, 5));
    CHECK(SDL_DJI_DecodeModel(&frame, model, 6) && strcmp(model, "rm330") == 0);
    SDL_DJI_CheckFrame(buffer, Model(buffer, 0x80, "rm330"), &frame);
    CHECK(!SDL_DJI_DecodeModel(&frame, model, sizeof(model)));
    SDL_DJI_CheckFrame(buffer, Model(buffer, 0x81, ""), &frame);
    CHECK(!SDL_DJI_DecodeModel(&frame, model, sizeof(model)));
    SDL_DJI_CheckFrame(buffer, Model(buffer, 0x81, "rm\x01" "30"), &frame);
    CHECK(!SDL_DJI_DecodeModel(&frame, model, sizeof(model)));
    /* A model that fills the payload without a 00 */
    memset(payload, 'a', sizeof(payload));
    SDL_DJI_CheckFrame(buffer, Frame(buffer, 0x06, 0x0A, 1, 0x40, 0x00, 0x81, payload, 5), &frame);
    CHECK(SDL_DJI_DecodeModel(&frame, model, sizeof(model)) && strcmp(model, "aaaaa") == 0);

    /* The decoders take only their own sizes */
    SDL_DJI_CheckFrame(buffer, Frame(buffer, 0x06, 0x0A, 1, 0x80, 0x06, 0x01, payload, 20), &frame);
    CHECK(SDL_DJI_DecodeChannels(&frame, values) == 0);
    CHECK(SDL_DJI_DecodeChannels(NULL, values) == 0);
    CHECK(!SDL_DJI_DecodePhantom2(NULL, 76, NULL));
}

int main(void)
{
    TestHelpers();
    TestRCN1Startup();
    TestRCN1Reports();
    TestRCN1StatusFirst();
    TestRCN1BatteryB();
    TestRCN1BatteryA();
    TestMavicMini();
    TestPhantom3Startup();
    TestPhantom3Controls();
    TestPhantom3FirstMode();
    TestPhantom3BatteryA();
    TestPhantom2Startup();
    TestPhantom2Reads();
    TestPhantom2BatteryB();
    TestBulkStartup();
    TestBulkChannels();
    TestBulkEarlyReply();
    TestBulkReports();
    TestBulkBatteryB();
    TestBulkBatteryA();
    TestPendingWrite();
    TestRestartClears();
    return H_Finish();
}
