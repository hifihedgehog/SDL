/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_usio_proto.c, the Namco
   USIO of hifihedgehog/SDL#33 Part 14. Test numbers follow the part. No
   capture of a real board exists, so every block is constructed from the
   layouts RPCS3 (rpcs3/Emu/Io/usio.cpp), TaikoZucchini (input/taiko_frame.c,
   hooks/bpreader_hook.c) and ITAIKO-firmware (src/usb/device/vendor/
   usio_driver.c) serve to the games. Replies arrive the way the libusb
   backend hands them over: one read per bulk packet of up to 64 bytes, each
   from an exact-size heap copy so a read past its length reaches the
   sanitizer. */

#include "../src/joystick/hidapi/SDL_hidapi_usio_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;
static int commands;

#define CHECK(condition)                                                      \
    do {                                                                      \
        ++checks;                                                             \
        if (!(condition)) {                                                   \
            ++failures;                                                       \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
        }                                                                     \
    } while (0)

#define MS 1000000ULL
#define PACKET 64

#define TAIKO_LENGTH  0x60
#define TEKKEN_LENGTH 0x180
#define IDENT_LENGTH  0x180

typedef struct TestSink
{
    bool connected;
    int connects;
    int disconnects;
    int controls_calls;
    int player_calls[SDL_USIO_MAX_PLAYERS];
    SDL_USIOControls last[SDL_USIO_MAX_PLAYERS];
    int logs;
    SDL_USIOLayout layout;
} TestSink;

static void TestConnect(void *userdata)
{
    TestSink *sink = (TestSink *)userdata;

    CHECK(!sink->connected);
    sink->connected = true;
    ++sink->connects;
}

static void TestDisconnect(void *userdata)
{
    TestSink *sink = (TestSink *)userdata;

    CHECK(sink->connected);
    sink->connected = false;
    ++sink->disconnects;
}

static void TestControls(void *userdata, int player, const SDL_USIOControls *controls)
{
    TestSink *sink = (TestSink *)userdata;

    /* Controls come only for a player of the layout, while connected */
    CHECK(sink->connected);
    CHECK(player >= 0 && player < SDL_USIO_PlayerCount(sink->layout));
    CHECK(controls != NULL);
    if (player >= 0 && player < SDL_USIO_MAX_PLAYERS && controls) {
        sink->last[player] = *controls;
        ++sink->player_calls[player];
    }
    ++sink->controls_calls;
}

static void TestLog(void *userdata, const char *text)
{
    TestSink *sink = (TestSink *)userdata;

    CHECK(text != NULL && text[0] != '\0');
    ++sink->logs;
}

static SDL_USIOSink Sink(TestSink *sink, SDL_USIOLayout layout)
{
    SDL_USIOSink result;

    memset(sink, 0, sizeof(*sink));
    sink->layout = layout;
    result.userdata = sink;
    result.connect = TestConnect;
    result.disconnect = TestDisconnect;
    result.controls = TestControls;
    result.log = TestLog;
    return result;
}

static void Put16(uint8_t *block, size_t offset, uint16_t value)
{
    block[offset] = (uint8_t)(value & 0xFF);
    block[offset + 1] = (uint8_t)(value >> 8);
}

static void Put64(uint8_t *block, size_t offset, uint64_t value)
{
    int i;

    for (i = 0; i < 8; ++i) {
        block[offset + (size_t)i] = (uint8_t)(value >> (8 * i));
    }
}

static bool SameControls(const SDL_USIOControls *a, const SDL_USIOControls *b)
{
    int i;

    for (i = 0; i < SDL_USIO_MAX_AXES; ++i) {
        if (a->axes[i] != b->axes[i]) {
            return false;
        }
    }
    return a->buttons == b->buttons && a->hat == b->hat;
}

static bool AtRest(const SDL_USIOControls *controls)
{
    SDL_USIOControls rest;

    memset(&rest, 0, sizeof(rest));
    return SameControls(controls, &rest);
}

static const SDL_USIOControls *Controls(const SDL_USIOState *state, int player)
{
    static SDL_USIOControls none;
    const SDL_USIOControls *controls = SDL_USIO_GetControls(state, player);

    CHECK(controls != NULL);
    return controls ? controls : &none;
}

/* The identification block: the fields "NBGI.", "USIO01", "Ver1.00" and
   "JPN,Multipurpose with PPG." joined by the byte 3B (usio.cpp:746-754),
   then zeros. Only the first five bytes carry meaning for the module. */
static void IdentBlock(uint8_t block[IDENT_LENGTH])
{
    static const char *fields[] = { "NBGI.", "USIO01", "Ver1.00", "JPN,Multipurpose with PPG." };
    size_t i, used = 0;

    memset(block, 0, IDENT_LENGTH);
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        const size_t length = strlen(fields[i]);

        if (i > 0) {
            block[used++] = 0x3B;
        }
        memcpy(block + used, fields[i], length);
        used += length;
    }
}

/* Takes the next command, checks it is the read of this register and
   length, and completes its write */
static bool Expect(SDL_USIOState *state, uint64_t now, uint16_t reg, uint16_t length, const SDL_USIOSink *sink)
{
    uint8_t command[SDL_USIO_COMMAND_SIZE];

    memset(command, 0xEE, sizeof(command));
    if (!SDL_USIO_NextCommand(state, now, command)) {
        ++checks;
        ++failures;
        printf("FAILED: no command at %u ms for register %04X\n", (unsigned int)(now / MS), reg);
        return false;
    }
    ++commands;
    /* Never a write, an init or another channel: a read of channel 0 */
    CHECK(command[0] == 0x10);
    CHECK(command[1] == 0xE0);
    CHECK(command[2] == (uint8_t)(reg & 0xFF) && command[3] == (uint8_t)(reg >> 8));
    CHECK(command[4] == (uint8_t)(length & 0xFF) && command[5] == (uint8_t)(length >> 8));
    SDL_USIO_CommandDone(state, SDL_USIO_COMMAND_SIZE, now, sink);
    return true;
}

static void FeedExact(SDL_USIOState *state, const uint8_t *data, size_t length, uint64_t now, const SDL_USIOSink *sink)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);

    if (!copy) {
        ++failures;
        printf("FAILED: test buffer\n");
        return;
    }
    if (length) {
        memcpy(copy, data, length);
    }
    SDL_USIO_Feed(state, copy, length, now, sink);
    free(copy);
}

/* One read per packet, as the libusb backend reads a bulk endpoint */
static void FeedPackets(SDL_USIOState *state, const uint8_t *data, size_t length, uint64_t now, const SDL_USIOSink *sink)
{
    size_t offset;

    for (offset = 0; offset < length; offset += PACKET) {
        const size_t size = (length - offset > PACKET) ? PACKET : length - offset;
        FeedExact(state, data + offset, size, now, sink);
    }
}

static uint16_t BlockRegister(SDL_USIOLayout layout)
{
    return (layout == SDL_USIO_LAYOUT_TEKKEN) ? 0x1000 : 0x1080;
}

static uint16_t BlockLength(SDL_USIOLayout layout)
{
    return (layout == SDL_USIO_LAYOUT_TEKKEN) ? TEKKEN_LENGTH : TAIKO_LENGTH;
}

/* Starts a board and answers its identification read */
static void Start(SDL_USIOState *state, SDL_USIOLayout layout, bool require_ident, TestSink *test, SDL_USIOSink *sink, uint64_t now)
{
    uint8_t ident[IDENT_LENGTH];

    *sink = Sink(test, layout);
    SDL_USIO_Init(state, layout, require_ident);
    IdentBlock(ident);
    if (Expect(state, now, 0x1800, IDENT_LENGTH, sink)) {
        FeedPackets(state, ident, IDENT_LENGTH, now, sink);
    }
    CHECK(test->connects == 1 && test->connected);
    CHECK(state->phase == SDL_USIO_PHASE_POLL);
}

/* One input read answered with a whole block */
static void Poll(SDL_USIOState *state, const uint8_t *block, uint64_t now, const SDL_USIOSink *sink)
{
    if (Expect(state, now, BlockRegister(state->layout), BlockLength(state->layout), sink)) {
        FeedPackets(state, block, BlockLength(state->layout), now, sink);
    }
    CHECK(!state->waiting);
}

/* 1 */
static void TestCommands(void)
{
    static const uint8_t taiko[6] = { 0x10, 0xE0, 0x80, 0x10, 0x60, 0x00 };
    static const uint8_t tekken[6] = { 0x10, 0xE0, 0x00, 0x10, 0x80, 0x01 };
    static const uint8_t ident[6] = { 0x10, 0xE0, 0x00, 0x18, 0x80, 0x01 };
    static const uint8_t write7400[6] = { 0x90, 0x80, 0x00, 0x74, 0x02, 0x00 };
    uint8_t command[6];
    uint8_t written[6];
    int high, low;

    CHECK(SDL_USIO_EncodeCommand(SDL_USIO_READ, 0, 0x1080, 0x60, command) && memcmp(command, taiko, 6) == 0);
    CHECK(SDL_USIO_EncodeCommand(SDL_USIO_READ, 0, 0x1000, 0x180, command) && memcmp(command, tekken, 6) == 0);
    CHECK(SDL_USIO_EncodeCommand(SDL_USIO_READ, 0, 0x1800, 0x180, command) && memcmp(command, ident, 6) == 0);

    /* The write rule gives 90 80 00 74 02 00 for 2 bytes to 7400, and the
       encoder refuses any write, leaving the buffer alone */
    CHECK(SDL_USIO_CheckByte(0x7400) == 0x80);
    written[0] = SDL_USIO_WRITE;
    written[1] = SDL_USIO_CheckByte(0x7400);
    written[2] = 0x00;
    written[3] = 0x74;
    written[4] = 0x02;
    written[5] = 0x00;
    CHECK(memcmp(written, write7400, 6) == 0);
    memset(command, 0xEE, sizeof(command));
    CHECK(!SDL_USIO_EncodeCommand(SDL_USIO_WRITE, 0, 0x7400, 2, command));
    CHECK(command[0] == 0xEE && command[5] == 0xEE);
    CHECK(!SDL_USIO_EncodeCommand(SDL_USIO_WRITE, 0, 0x1080, 0x60, command));
    /* Nor an init, nor a byte 0 the board reads as a read with other bits */
    CHECK(!SDL_USIO_EncodeCommand(SDL_USIO_INIT, 0, 0x0008, 0, command));
    CHECK(!SDL_USIO_EncodeCommand(SDL_USIO_INIT, 0, 0x0008, 2, command));
    CHECK(!SDL_USIO_EncodeCommand(0x30, 0, 0x1080, 0x60, command));
    CHECK(!SDL_USIO_EncodeCommand(0x00, 0, 0x1080, 0x60, command));
    CHECK(command[0] == 0xEE);
    /* Channel 1 is firmware and channels 2 and up backup SRAM (usio.cpp:693-709) */
    CHECK(!SDL_USIO_EncodeCommand(SDL_USIO_READ, 1, 0x1080, 0x60, command));
    CHECK(!SDL_USIO_EncodeCommand(SDL_USIO_READ, 2, 0x0000, 0x40, command));
    CHECK(!SDL_USIO_EncodeCommand(SDL_USIO_READ, 15, 0x1080, 0x60, command));
    CHECK(command[0] == 0xEE);
    /* Lengths from 1 to 0x180 */
    CHECK(!SDL_USIO_EncodeCommand(SDL_USIO_READ, 0, 0x1080, 0, command));
    CHECK(!SDL_USIO_EncodeCommand(SDL_USIO_READ, 0, 0x1080, 0x181, command));
    CHECK(!SDL_USIO_EncodeCommand(SDL_USIO_READ, 0, 0x1080, 0xFFFF, command));
    CHECK(SDL_USIO_EncodeCommand(SDL_USIO_READ, 0, 0x1080, 1, command) && command[4] == 0x01 && command[5] == 0x00);
    CHECK(SDL_USIO_EncodeCommand(SDL_USIO_READ, 0, 0xABCD, 0x180, command));
    CHECK(command[1] == 0x50 && command[2] == 0xCD && command[3] == 0xAB);
    CHECK(!SDL_USIO_EncodeCommand(SDL_USIO_READ, 0, 0x1080, 0x60, NULL));

    /* Byte 1 is (NOT high byte) AND F0, whatever the low byte */
    for (high = 0; high < 256; ++high) {
        for (low = 0; low < 256; low += 85) {
            const uint16_t reg = (uint16_t)((high << 8) | low);
            CHECK(SDL_USIO_CheckByte(reg) == (uint8_t)((0xFF - high) & 0xF0));
        }
    }

    /* The module's own sequence: the identification block once, then the
       layout's block, every one a read of channel 0 */
    {
        SDL_USIOState state;
        TestSink test;
        SDL_USIOSink sink;
        uint8_t block[TEKKEN_LENGTH];
        int i;

        memset(block, 0, sizeof(block));
        Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
        for (i = 0; i < 3; ++i) {
            memset(command, 0, sizeof(command));
            CHECK(SDL_USIO_NextCommand(&state, 0, command) && memcmp(command, taiko, 6) == 0);
            ++commands;
            /* One read at a time */
            CHECK(!SDL_USIO_NextCommand(&state, 0, command));
            SDL_USIO_CommandDone(&state, 6, 0, &sink);
            FeedPackets(&state, block, TAIKO_LENGTH, 0, &sink);
        }
        Start(&state, SDL_USIO_LAYOUT_TEKKEN, false, &test, &sink, 0);
        for (i = 0; i < 3; ++i) {
            memset(command, 0, sizeof(command));
            CHECK(SDL_USIO_NextCommand(&state, 0, command) && memcmp(command, tekken, 6) == 0);
            ++commands;
            CHECK(!SDL_USIO_NextCommand(&state, 0, command));
            SDL_USIO_CommandDone(&state, 6, 0, &sink);
            FeedPackets(&state, block, TEKKEN_LENGTH, 0, &sink);
        }
        /* The first command of a board is the identification read */
        SDL_USIO_Init(&state, SDL_USIO_LAYOUT_TEKKEN, false);
        CHECK(SDL_USIO_NextCommand(&state, 0, command) && memcmp(command, ident, 6) == 0);
        ++commands;
        SDL_USIO_Init(&state, SDL_USIO_LAYOUT_TAIKO, true);
        CHECK(SDL_USIO_NextCommand(&state, 0, command) && memcmp(command, ident, 6) == 0);
        ++commands;
    }
}

/* 2 */
static void TestTaikoIdle(void)
{
    SDL_USIOState state;
    TestSink test;
    SDL_USIOSink sink;
    uint8_t block[TAIKO_LENGTH];

    memset(block, 0, sizeof(block));
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    CHECK(AtRest(Controls(&state, 0)) && AtRest(Controls(&state, 1)));
    Poll(&state, block, 0, &sink);
    Poll(&state, block, 0, &sink);
    CHECK(AtRest(Controls(&state, 0)) && AtRest(Controls(&state, 1)));
    /* Nothing changed, so nothing was sent */
    CHECK(test.controls_calls == 0);
}

/* Polls a baseline idle block, then this one, and returns what it decoded */
static void TaikoOnce(const uint8_t *block, SDL_USIOControls out[2], int *calls)
{
    SDL_USIOState state;
    TestSink test;
    SDL_USIOSink sink;
    uint8_t idle[TAIKO_LENGTH];

    memset(idle, 0, sizeof(idle));
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Poll(&state, idle, 0, &sink);
    Poll(&state, block, 0, &sink);
    out[0] = *Controls(&state, 0);
    out[1] = *Controls(&state, 1);
    *calls = test.controls_calls;
    /* The sink saw what the state holds */
    if (test.player_calls[0]) {
        CHECK(SameControls(&test.last[0], &out[0]));
    }
    if (test.player_calls[1]) {
        CHECK(SameControls(&test.last[1], &out[1]));
    }
    /* Back to idle releases everything */
    Poll(&state, idle, 0, &sink);
    CHECK(AtRest(Controls(&state, 0)) && AtRest(Controls(&state, 1)));
}

/* 3 */
static void TestTaikoControls(void)
{
    static const struct
    {
        uint8_t byte0, byte1;
        uint16_t buttons;
    } panel[] = {
        { 0x80, 0x00, 1u << 8 }, /* Test */
        { 0x00, 0x02, 1u << 4 }, /* Enter */
        { 0x00, 0x10, 1u << 6 }, /* Down */
        { 0x00, 0x20, 1u << 5 }, /* Up */
        { 0x00, 0x40, 1u << 7 }, /* Service */
    };
    static const struct
    {
        uint16_t value;
        int16_t axis;
        bool hit;
    } sensor[] = {
        { 0x1800, 0x0C00, true },  /* RPCS3's hit */
        { 0xFFFF, 0x7FFF, true },  /* TaikoZucchini's and ITAIKO's */
        { 0x0C00, 0x0600, true },  /* The threshold */
        { 0x0BFF, 0x05FF, false }, /* One below */
        { 0x0001, 0x0000, false },
        { 0x0002, 0x0001, false },
        { 0x8000, 0x4000, true },
    };
    uint8_t block[TAIKO_LENGTH];
    SDL_USIOControls out[2];
    int calls;
    size_t i, s;
    int offset;

    for (i = 0; i < sizeof(panel) / sizeof(panel[0]); ++i) {
        memset(block, 0, sizeof(block));
        block[0] = panel[i].byte0;
        block[1] = panel[i].byte1;
        TaikoOnce(block, out, &calls);
        CHECK(out[0].buttons == panel[i].buttons && out[0].hat == 0);
        CHECK(out[0].axes[0] == 0 && out[0].axes[1] == 0 && out[0].axes[2] == 0 && out[0].axes[3] == 0);
        CHECK(AtRest(&out[1]));
        CHECK(calls == 1);
    }

    /* Bytes 32 to 39 are player 1's pads, 40 to 47 player 2's, in the order
       side left, center left, center right, side right */
    for (offset = 32; offset <= 46; offset += 2) {
        const int player = (offset - 32) / 8;
        const int pad = ((offset - 32) % 8) / 2;

        for (s = 0; s < sizeof(sensor) / sizeof(sensor[0]); ++s) {
            SDL_USIOControls expected;

            memset(block, 0, sizeof(block));
            Put16(block, (size_t)offset, sensor[s].value);
            TaikoOnce(block, out, &calls);
            memset(&expected, 0, sizeof(expected));
            expected.axes[pad] = sensor[s].axis;
            if (sensor[s].hit) {
                expected.buttons = (uint16_t)(1u << pad);
            }
            CHECK(SameControls(&out[player], &expected));
            CHECK(AtRest(&out[1 - player]));
        }
    }

    /* Every panel word, read against the part's table: 0080 Test, 0200
       Enter, 1000 Down, 2000 Up, 4000 Service, and no other bit */
    {
        SDL_USIOState state;
        TestSink test;
        SDL_USIOSink sink;
        int word, mismatches = 0;

        Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
        for (word = 0; word < 0x10000; ++word) {
            uint16_t expected = 0;

            memset(block, 0, sizeof(block));
            block[0] = (uint8_t)(word & 0xFF);
            block[1] = (uint8_t)(word >> 8);
            if (word & 0x0080) {
                expected = (uint16_t)(expected | (1u << 8));
            }
            if (word & 0x0200) {
                expected = (uint16_t)(expected | (1u << 4));
            }
            if (word & 0x1000) {
                expected = (uint16_t)(expected | (1u << 6));
            }
            if (word & 0x2000) {
                expected = (uint16_t)(expected | (1u << 5));
            }
            if (word & 0x4000) {
                expected = (uint16_t)(expected | (1u << 7));
            }
            Poll(&state, block, 0, &sink);
            if (Controls(&state, 0)->buttons != expected || !AtRest(Controls(&state, 1))) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
    }

    /* The bytes between the fields carry nothing */
    memset(block, 0xFF, sizeof(block));
    block[0] = block[1] = 0;
    memset(&block[16], 0, 2);
    memset(&block[32], 0, 16);
    TaikoOnce(block, out, &calls);
    CHECK(AtRest(&out[0]) && AtRest(&out[1]) && calls == 0);
}

/* 4 */
static void TestTaikoCoin(void)
{
    SDL_USIOState state;
    TestSink test;
    SDL_USIOSink sink;
    uint8_t block[TAIKO_LENGTH];
    const uint16_t coin = 1u << 9;
    int i, presses;

    /* 0000 then 0001 presses Coin for one poll */
    memset(block, 0, sizeof(block));
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == 0);
    Put16(block, 16, 0x0001);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == coin);
    CHECK(test.player_calls[0] == 1 && test.last[0].buttons == coin);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == 0);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == 0 && test.player_calls[0] == 2);
    CHECK(AtRest(Controls(&state, 1)));

    /* FFFF to 0000 is one coin */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Put16(block, 16, 0xFFFF);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == 0);
    Put16(block, 16, 0x0000);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == coin);
    Poll(&state, block, 0, &sink);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == 0 && test.player_calls[0] == 2);

    /* The first block after the start sets the baseline and presses nothing,
       whatever the count */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Put16(block, 16, 0x1234);
    Poll(&state, block, 0, &sink);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == 0 && test.controls_calls == 0);
    Put16(block, 16, 0x1235);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == coin);

    /* Three coins in one poll are three presses, each followed by a
       released poll */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Put16(block, 16, 0x0010);
    Poll(&state, block, 0, &sink);
    Put16(block, 16, 0x0013);
    presses = 0;
    for (i = 0; i < 10; ++i) {
        const bool pressed = (Controls(&state, 0)->buttons & coin) != 0;

        Poll(&state, block, 0, &sink);
        if ((Controls(&state, 0)->buttons & coin) != 0) {
            CHECK(!pressed);
            CHECK(i % 2 == 0);
            ++presses;
        }
    }
    CHECK(presses == 3);

    /* A coin that comes while the button is down waits one released poll */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Put16(block, 16, 0x0000);
    Poll(&state, block, 0, &sink);
    Put16(block, 16, 0x0001);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == coin);
    Put16(block, 16, 0x0002);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == 0);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == coin);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == 0);

    /* A counter that went back counts nothing and sets a new baseline */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Put16(block, 16, 0x0010);
    Poll(&state, block, 0, &sink);
    Put16(block, 16, 0x0005);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == 0);
    Put16(block, 16, 0x0006);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == coin);

    /* 7FFF up is the most that counts as coins, 16 of them at most. 8000 up
       went back. */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Put16(block, 16, 0x0000);
    Poll(&state, block, 0, &sink);
    Put16(block, 16, 0x7FFF);
    presses = 0;
    for (i = 0; i < 40; ++i) {
        Poll(&state, block, 0, &sink);
        if (Controls(&state, 0)->buttons & coin) {
            ++presses;
        }
    }
    CHECK(presses == SDL_USIO_MAX_PENDING_COINS);
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Put16(block, 16, 0x0000);
    Poll(&state, block, 0, &sink);
    Put16(block, 16, 0x8000);
    for (i = 0; i < 4; ++i) {
        Poll(&state, block, 0, &sink);
        CHECK(Controls(&state, 0)->buttons == 0);
    }
    Put16(block, 16, 0x8001);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == coin);

    /* 17 coins at once are 16 presses */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Put16(block, 16, 0x0000);
    Poll(&state, block, 0, &sink);
    Put16(block, 16, 0x0011);
    presses = 0;
    for (i = 0; i < 40; ++i) {
        Poll(&state, block, 0, &sink);
        if (Controls(&state, 0)->buttons & coin) {
            ++presses;
        }
    }
    CHECK(presses == 16);
    /* 16 at once are all pressed */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Put16(block, 16, 0x0000);
    Poll(&state, block, 0, &sink);
    Put16(block, 16, 0x0010);
    presses = 0;
    for (i = 0; i < 40; ++i) {
        Poll(&state, block, 0, &sink);
        if (Controls(&state, 0)->buttons & coin) {
            ++presses;
        }
    }
    CHECK(presses == 16);

    /* Coin rides beside other buttons, and player 2 has none */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    memset(block, 0, sizeof(block));
    Poll(&state, block, 0, &sink);
    block[1] = 0x02;
    Put16(block, 16, 0x0001);
    Put16(block, 40, 0xFFFF);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == (coin | (1u << 4)));
    CHECK(Controls(&state, 1)->buttons == 1u && Controls(&state, 1)->axes[0] == 0x7FFF);
}

/* The part's table for one pair's 64-bit word: the player it belongs to (0
   first, 1 second) and the control, a button index or a hat bit */
typedef struct TekkenBit
{
    int bit;
    int half;
    int button; /* -1 for a hat bit */
    uint8_t hat;
} TekkenBit;

static const TekkenBit tekken_bits[] = {
    { 7, 0, 6, 0 },   /* Test */
    { 14, 0, 7, 0 },  /* Service */
    { 16, 0, 1, 0 },  /* Button 2 */
    { 17, 0, 0, 0 },  /* Button 1 */
    { 18, 0, -1, SDL_USIO_HAT_RIGHT },
    { 19, 0, -1, SDL_USIO_HAT_LEFT },
    { 20, 0, -1, SDL_USIO_HAT_DOWN },
    { 21, 0, -1, SDL_USIO_HAT_UP },
    { 23, 0, 5, 0 },  /* Enter */
    { 29, 0, 3, 0 },  /* Button 4 */
    { 30, 0, 2, 0 },  /* Button 3 */
    { 31, 0, 4, 0 },  /* Button 5 */
    { 40, 1, 1, 0 },
    { 41, 1, 0, 0 },
    { 42, 1, -1, SDL_USIO_HAT_RIGHT },
    { 43, 1, -1, SDL_USIO_HAT_LEFT },
    { 44, 1, -1, SDL_USIO_HAT_DOWN },
    { 45, 1, -1, SDL_USIO_HAT_UP },
    { 47, 1, 5, 0 },
    { 53, 1, 3, 0 },
    { 54, 1, 2, 0 },
    { 55, 1, 4, 0 },
};

/* 5 */
static void TestTekken(void)
{
    static const struct
    {
        size_t offset;
        uint8_t value;
        int player;
        uint16_t buttons;
        uint8_t hat;
    } single[] = {
        { 0x102, 0x02, 0, 1u << 0, 0 },           /* P1 button 1 */
        { 0x105, 0x02, 1, 1u << 0, 0 },           /* P2 button 1 */
        { 0x082, 0x02, 2, 1u << 0, 0 },           /* P3 button 1 */
        { 0x085, 0x02, 3, 1u << 0, 0 },           /* P4 button 1 */
        { 0x103, 0x80, 0, 1u << 4, 0 },           /* P1 button 5 */
        { 0x100, 0x80, 0, 1u << 6, 0 },           /* Test */
        { 0x101, 0x40, 0, 1u << 7, 0 },           /* Service */
        { 0x102, 0x20, 0, 0, SDL_USIO_HAT_UP },   /* P1 up */
        { 0x080, 0x80, 2, 1u << 6, 0 },           /* P3 Test */
        { 0x081, 0x40, 2, 1u << 7, 0 },           /* P3 Service */
        { 0x085, 0x80, 3, 1u << 5, 0 },           /* P4 Enter, bit 47 */
        { 0x086, 0x80, 3, 1u << 4, 0 },           /* P4 button 5, bit 55 */
        { 0x107, 0x80, 1, 0, 0 },                 /* Bit 63 is no control */
        { 0x087, 0x80, 3, 0, 0 },
    };
    SDL_USIOState state;
    TestSink test;
    SDL_USIOSink sink;
    uint8_t block[TEKKEN_LENGTH];
    size_t i;
    int pair, bit, player;

    Start(&state, SDL_USIO_LAYOUT_TEKKEN, false, &test, &sink, 0);
    memset(block, 0, sizeof(block));
    Poll(&state, block, 0, &sink);
    for (player = 0; player < 4; ++player) {
        CHECK(AtRest(Controls(&state, player)));
    }
    CHECK(test.controls_calls == 0);

    for (i = 0; i < sizeof(single) / sizeof(single[0]); ++i) {
        memset(block, 0, sizeof(block));
        block[single[i].offset] = single[i].value;
        Poll(&state, block, 0, &sink);
        for (player = 0; player < 4; ++player) {
            const SDL_USIOControls *controls = Controls(&state, player);

            if (player == single[i].player) {
                CHECK(controls->buttons == single[i].buttons && controls->hat == single[i].hat);
            } else {
                CHECK(controls->buttons == 0 && controls->hat == 0);
            }
            CHECK(controls->axes[0] == 0 && controls->axes[3] == 0);
        }
    }

    /* Every bit of both words alone, read against the part's table */
    for (pair = 0; pair < 2; ++pair) {
        for (bit = 0; bit < 64; ++bit) {
            int expected_player = -1, expected_button = -1;
            uint8_t expected_hat = 0;

            for (i = 0; i < sizeof(tekken_bits) / sizeof(tekken_bits[0]); ++i) {
                if (tekken_bits[i].bit == bit) {
                    expected_player = pair * 2 + tekken_bits[i].half;
                    expected_button = tekken_bits[i].button;
                    expected_hat = tekken_bits[i].hat;
                }
            }
            memset(block, 0, sizeof(block));
            Put64(block, pair ? 0x080 : 0x100, (uint64_t)1 << bit);
            Poll(&state, block, 0, &sink);
            for (player = 0; player < 4; ++player) {
                const SDL_USIOControls *controls = Controls(&state, player);
                uint16_t buttons = 0;
                uint8_t hat = 0;

                if (player == expected_player) {
                    if (expected_button >= 0) {
                        buttons = (uint16_t)(1u << expected_button);
                    }
                    hat = expected_hat;
                }
                if (controls->buttons != buttons || controls->hat != hat) {
                    printf("  pair %d bit %d player %d: %04X %X\n", pair, bit, player, controls->buttons, controls->hat);
                }
                CHECK(controls->buttons == buttons && controls->hat == hat);
            }
        }
    }

    /* Opposite directions cancel, each axis on its own */
    memset(block, 0, sizeof(block));
    Put64(block, 0x100, ((uint64_t)1 << 21) | ((uint64_t)1 << 20));
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->hat == 0);
    Put64(block, 0x100, ((uint64_t)1 << 19) | ((uint64_t)1 << 18));
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->hat == 0);
    Put64(block, 0x100, ((uint64_t)1 << 21) | ((uint64_t)1 << 19));
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->hat == (SDL_USIO_HAT_UP | SDL_USIO_HAT_LEFT));
    Put64(block, 0x100, ((uint64_t)1 << 20) | ((uint64_t)1 << 18) | ((uint64_t)1 << 19));
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->hat == SDL_USIO_HAT_DOWN);
    Put64(block, 0x080, ((uint64_t)1 << 44) | ((uint64_t)1 << 42));
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 3)->hat == (SDL_USIO_HAT_DOWN | SDL_USIO_HAT_RIGHT));
    CHECK(Controls(&state, 0)->hat == SDL_USIO_HAT_DOWN);

    /* Everything outside the two words and the two counters carries
       nothing, bytes 0 to 2 included, which repeat player 1 and hold the DIP
       switches */
    memset(block, 0xFF, sizeof(block));
    memset(&block[0x080], 0, 8);
    memset(&block[0x090], 0, 2);
    memset(&block[0x100], 0, 8);
    memset(&block[0x110], 0, 2);
    Poll(&state, block, 0, &sink);
    for (player = 0; player < 4; ++player) {
        CHECK(AtRest(Controls(&state, player)));
    }
    /* A full word presses everything its players have */
    memset(block, 0, sizeof(block));
    Put64(block, 0x100, ~(uint64_t)0);
    Put64(block, 0x080, ~(uint64_t)0);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == 0x00FF && Controls(&state, 0)->hat == 0);
    CHECK(Controls(&state, 1)->buttons == 0x003F && Controls(&state, 1)->hat == 0);
    CHECK(Controls(&state, 2)->buttons == 0x00FF && Controls(&state, 3)->buttons == 0x003F);

    /* The pairs' coin counters: players 1 and 3 */
    Start(&state, SDL_USIO_LAYOUT_TEKKEN, false, &test, &sink, 0);
    memset(block, 0, sizeof(block));
    Put16(block, 0x110, 0x0100);
    Put16(block, 0x090, 0x0200);
    Poll(&state, block, 0, &sink);
    CHECK(test.controls_calls == 0);
    Put16(block, 0x110, 0x0101);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == (1u << 8) && Controls(&state, 2)->buttons == 0);
    CHECK(Controls(&state, 1)->buttons == 0 && Controls(&state, 3)->buttons == 0);
    Put16(block, 0x090, 0x0201);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == 0 && Controls(&state, 2)->buttons == (1u << 8));
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 2)->buttons == 0);
    /* FFFF to 0000 on player 3's counter */
    Put16(block, 0x090, 0xFFFF);
    Poll(&state, block, 0, &sink);
    Poll(&state, block, 0, &sink);
    Put16(block, 0x090, 0x0000);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 2)->buttons == (1u << 8));
}

/* Builds a state whose controls are not at rest, polls it, and returns it */
static void Pressed(SDL_USIOState *state, SDL_USIOLayout layout, TestSink *test, SDL_USIOSink *sink, uint64_t now)
{
    uint8_t block[TEKKEN_LENGTH];

    memset(block, 0, sizeof(block));
    Start(state, layout, false, test, sink, now);
    if (layout == SDL_USIO_LAYOUT_TEKKEN) {
        Put64(block, 0x100, ((uint64_t)1 << 17) | ((uint64_t)1 << 21) | ((uint64_t)1 << 47));
        Put64(block, 0x080, ((uint64_t)1 << 31) | ((uint64_t)1 << 40));
    } else {
        block[1] = 0x42; /* Enter and Service */
        Put16(block, 32, 0x2000);
        Put16(block, 46, 0x0100);
    }
    Poll(state, block, now, sink);
    CHECK(!AtRest(Controls(state, 0)));
}

/* A block that would change every player: player 1's panel and every pad
   of the Taiko block, every control of the Tekken words */
static void Changing(SDL_USIOLayout layout, uint8_t *block)
{
    memset(block, 0, TEKKEN_LENGTH);
    if (layout == SDL_USIO_LAYOUT_TEKKEN) {
        Put64(block, 0x100, ~(uint64_t)0);
        Put64(block, 0x080, ~(uint64_t)0);
    } else {
        memset(block, 0xFF, TAIKO_LENGTH);
    }
}

/* 6: every truncation of every block, with stale pressed bits in the
   packet buffer past the received length, changes nothing and keeps the
   last state. The read is then abandoned at its timeout and read again. */
static void TestTruncation(void)
{
    static const SDL_USIOLayout layouts[] = { SDL_USIO_LAYOUT_TAIKO, SDL_USIO_LAYOUT_TEKKEN };
    size_t l;

    for (l = 0; l < sizeof(layouts) / sizeof(layouts[0]); ++l) {
        const SDL_USIOLayout layout = layouts[l];
        const size_t length = BlockLength(layout);
        uint8_t block[TEKKEN_LENGTH];
        uint8_t command[SDL_USIO_COMMAND_SIZE];
        size_t n;

        Changing(layout, block);
        for (n = 0; n <= length; ++n) {
            SDL_USIOState state;
            TestSink test;
            SDL_USIOSink sink;
            SDL_USIOControls before[SDL_USIO_MAX_PLAYERS];
            const uint64_t t0 = 1000 * MS;
            size_t offset;
            int player, calls;

            Pressed(&state, layout, &test, &sink, 0);
            for (player = 0; player < SDL_USIO_PlayerCount(layout); ++player) {
                before[player] = *Controls(&state, player);
            }
            calls = test.controls_calls;

            CHECK(Expect(&state, t0, BlockRegister(layout), (uint16_t)length, &sink));
            /* The first n bytes, in packets, each read into a 64-byte buffer
               whose bytes past the read hold pressed bits */
            for (offset = 0; offset < n; offset += PACKET) {
                const size_t size = (n - offset > PACKET) ? PACKET : n - offset;
                uint8_t stale[PACKET];

                memset(stale, 0xFF, sizeof(stale));
                memcpy(stale, block + offset, size);
                SDL_USIO_Feed(&state, stale, size, t0, &sink);
                FeedExact(&state, block + offset, 0, t0, &sink);
            }
            if (n == length) {
                /* The whole block applies */
                CHECK(!state.waiting && test.controls_calls > calls);
                CHECK(Controls(&state, 0)->buttons != before[0].buttons);
                continue;
            }
            CHECK(state.waiting && state.received == n);
            CHECK(test.controls_calls == calls);
            for (player = 0; player < SDL_USIO_PlayerCount(layout); ++player) {
                CHECK(SameControls(Controls(&state, player), &before[player]));
            }
            /* No second read goes out, and the timeout keeps the state */
            CHECK(!SDL_USIO_NextCommand(&state, t0, command));
            SDL_USIO_Tick(&state, t0 + SDL_USIO_REPLY_TIMEOUT_NS - 1, &sink);
            CHECK(state.waiting);
            SDL_USIO_Tick(&state, t0 + SDL_USIO_REPLY_TIMEOUT_NS, &sink);
            CHECK(!state.waiting && test.controls_calls == calls && test.disconnects == 0);
            for (player = 0; player < SDL_USIO_PlayerCount(layout); ++player) {
                CHECK(SameControls(Controls(&state, player), &before[player]));
            }
            /* The read goes out again after the quiet wait, and a whole
               block then applies */
            CHECK(Expect(&state, t0 + SDL_USIO_REPLY_TIMEOUT_NS + SDL_USIO_QUIET_NS, BlockRegister(layout), (uint16_t)length, &sink));
            FeedPackets(&state, block, length, t0 + SDL_USIO_REPLY_TIMEOUT_NS + SDL_USIO_QUIET_NS, &sink);
            CHECK(test.controls_calls > calls);
            CHECK(Controls(&state, 0)->buttons != before[0].buttons);
        }
    }

    /* The identification block: nothing connects until it is whole */
    {
        uint8_t ident[IDENT_LENGTH];
        size_t n;

        IdentBlock(ident);
        for (n = 0; n < IDENT_LENGTH; ++n) {
            SDL_USIOState state;
            TestSink test;
            SDL_USIOSink sink = Sink(&test, SDL_USIO_LAYOUT_TAIKO);
            size_t offset;

            SDL_USIO_Init(&state, SDL_USIO_LAYOUT_TAIKO, true);
            CHECK(Expect(&state, 0, 0x1800, IDENT_LENGTH, &sink));
            for (offset = 0; offset < n; offset += PACKET) {
                const size_t size = (n - offset > PACKET) ? PACKET : n - offset;
                uint8_t stale[PACKET];

                memset(stale, 0x4E, sizeof(stale));
                memcpy(stale, ident + offset, size);
                SDL_USIO_Feed(&state, stale, size, 0, &sink);
            }
            CHECK(test.connects == 0 && state.phase == SDL_USIO_PHASE_IDENTIFY);
            SDL_USIO_Tick(&state, SDL_USIO_REPLY_TIMEOUT_NS, &sink);
            CHECK(!state.waiting && test.connects == 0);
            CHECK(Expect(&state, SDL_USIO_REPLY_TIMEOUT_NS + SDL_USIO_QUIET_NS, 0x1800, IDENT_LENGTH, &sink));
            FeedPackets(&state, ident, IDENT_LENGTH, SDL_USIO_REPLY_TIMEOUT_NS + SDL_USIO_QUIET_NS, &sink);
            CHECK(test.connects == 1 && state.phase == SDL_USIO_PHASE_POLL);
        }
    }
}

/* Every split of a reply across reads decodes as one read of the whole */
static void TestSplits(void)
{
    static const SDL_USIOLayout layouts[] = { SDL_USIO_LAYOUT_TAIKO, SDL_USIO_LAYOUT_TEKKEN };
    size_t l;

    for (l = 0; l < sizeof(layouts) / sizeof(layouts[0]); ++l) {
        const SDL_USIOLayout layout = layouts[l];
        const size_t length = BlockLength(layout);
        uint8_t block[TEKKEN_LENGTH];
        SDL_USIOControls reference[SDL_USIO_MAX_PLAYERS];
        SDL_USIOState state;
        TestSink test;
        SDL_USIOSink sink;
        size_t a, b, i;
        int player, mismatches = 0;

        memset(block, 0, sizeof(block));
        if (layout == SDL_USIO_LAYOUT_TEKKEN) {
            Put64(block, 0x100, 0x00A5000000A51234ULL);
            Put64(block, 0x080, 0x0082000080210000ULL);
        } else {
            block[0] = 0x80;
            block[1] = 0x22;
            Put16(block, 34, 0x1800);
            Put16(block, 38, 0x0BFF);
            Put16(block, 40, 0xFFFF);
            Put16(block, 44, 0x0C01);
        }
        Start(&state, layout, false, &test, &sink, 0);
        Poll(&state, block, 0, &sink);
        for (player = 0; player < SDL_USIO_PlayerCount(layout); ++player) {
            reference[player] = *Controls(&state, player);
        }
        CHECK(!AtRest(&reference[0]));

        /* Two reads */
        for (a = 0; a <= length; ++a) {
            Start(&state, layout, false, &test, &sink, 0);
            if (Expect(&state, 0, BlockRegister(layout), (uint16_t)length, &sink)) {
                FeedExact(&state, block, a, 0, &sink);
                FeedExact(&state, block + a, length - a, 0, &sink);
            }
            for (player = 0; player < SDL_USIO_PlayerCount(layout); ++player) {
                if (!SameControls(Controls(&state, player), &reference[player])) {
                    ++mismatches;
                }
            }
            if (state.waiting || state.dropped != 0 || state.quiet) {
                ++mismatches;
            }
        }
        /* Three reads, on the Taiko block */
        if (layout == SDL_USIO_LAYOUT_TAIKO) {
            for (a = 0; a <= length; ++a) {
                for (b = a; b <= length; ++b) {
                    Start(&state, layout, false, &test, &sink, 0);
                    if (Expect(&state, 0, BlockRegister(layout), (uint16_t)length, &sink)) {
                        FeedExact(&state, block, a, 0, &sink);
                        FeedExact(&state, block + a, b - a, 0, &sink);
                        FeedExact(&state, block + b, length - b, 0, &sink);
                    }
                    for (player = 0; player < SDL_USIO_PlayerCount(layout); ++player) {
                        if (!SameControls(Controls(&state, player), &reference[player])) {
                            ++mismatches;
                        }
                    }
                    if (state.waiting || state.dropped != 0) {
                        ++mismatches;
                    }
                }
            }
        }
        /* One byte at a time */
        Start(&state, layout, false, &test, &sink, 0);
        if (Expect(&state, 0, BlockRegister(layout), (uint16_t)length, &sink)) {
            for (i = 0; i < length; ++i) {
                FeedExact(&state, block + i, 1, 0, &sink);
                if (i + 1 < length && !state.waiting) {
                    ++mismatches;
                }
            }
        }
        for (player = 0; player < SDL_USIO_PlayerCount(layout); ++player) {
            if (!SameControls(Controls(&state, player), &reference[player])) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
    }

    /* The identification block, split anywhere, connects once */
    {
        uint8_t ident[IDENT_LENGTH];
        size_t a;
        int wrong = 0;

        IdentBlock(ident);
        for (a = 0; a <= IDENT_LENGTH; ++a) {
            SDL_USIOState state;
            TestSink test;
            SDL_USIOSink sink = Sink(&test, SDL_USIO_LAYOUT_TEKKEN);

            SDL_USIO_Init(&state, SDL_USIO_LAYOUT_TEKKEN, true);
            if (Expect(&state, 0, 0x1800, IDENT_LENGTH, &sink)) {
                FeedExact(&state, ident, a, 0, &sink);
                if (a < IDENT_LENGTH && test.connects != 0) {
                    ++wrong;
                }
                FeedExact(&state, ident + a, IDENT_LENGTH - a, 0, &sink);
            }
            if (test.connects != 1 || state.phase != SDL_USIO_PHASE_POLL || state.dropped != 0) {
                ++wrong;
            }
        }
        CHECK(wrong == 0);
    }
}

/* 7 */
static void TestIdentification(void)
{
    uint8_t ident[IDENT_LENGTH];
    uint8_t other[IDENT_LENGTH];
    uint8_t command[SDL_USIO_COMMAND_SIZE];
    int i;

    IdentBlock(ident);
    CHECK(SDL_USIO_IsIdentified(ident, IDENT_LENGTH));
    CHECK(SDL_USIO_IsIdentified(ident, 5));
    CHECK(!SDL_USIO_IsIdentified(ident, 4));
    CHECK(!SDL_USIO_IsIdentified(ident, 0));
    CHECK(!SDL_USIO_IsIdentified(NULL, 5));
    /* Each of the five bytes counts, the next record's "NBGI1" as well */
    for (i = 0; i < 5; ++i) {
        memcpy(other, ident, sizeof(other));
        other[i] = (uint8_t)(other[i] ^ 0x01);
        CHECK(!SDL_USIO_IsIdentified(other, IDENT_LENGTH));
    }
    memcpy(other, ident, sizeof(other));
    other[4] = '1';
    CHECK(!SDL_USIO_IsIdentified(other, IDENT_LENGTH));

    /* 0910: a block that does not begin 4E 42 47 49 2E is logged and the
       board is still used */
    for (i = 0; i < 7; ++i) {
        SDL_USIOState state;
        TestSink test;
        SDL_USIOSink sink = Sink(&test, SDL_USIO_LAYOUT_TAIKO);
        uint8_t block[TAIKO_LENGTH];

        memcpy(other, ident, sizeof(other));
        if (i < 5) {
            other[i] = (uint8_t)(other[i] + 1);
        } else if (i == 5) {
            memset(other, 0, sizeof(other));
        } else {
            memset(other, 0xFF, sizeof(other));
        }
        SDL_USIO_Init(&state, SDL_USIO_LAYOUT_TAIKO, false);
        CHECK(Expect(&state, 0, 0x1800, IDENT_LENGTH, &sink));
        FeedPackets(&state, other, IDENT_LENGTH, 0, &sink);
        CHECK(test.logs >= 1);
        CHECK(test.connects == 1 && state.phase == SDL_USIO_PHASE_POLL);
        memset(block, 0, sizeof(block));
        block[1] = 0x02;
        Poll(&state, block, 0, &sink);
        CHECK(Controls(&state, 0)->buttons == (1u << 4));
    }

    /* 0900: used with "NBGI." only. Without it nothing connects, nothing
       more is sent, and whatever comes is dropped. */
    {
        SDL_USIOState state;
        TestSink test;
        SDL_USIOSink sink = Sink(&test, SDL_USIO_LAYOUT_TEKKEN);
        uint64_t now;

        memcpy(other, ident, sizeof(other));
        other[4] = '1';
        SDL_USIO_Init(&state, SDL_USIO_LAYOUT_TEKKEN, true);
        CHECK(Expect(&state, 0, 0x1800, IDENT_LENGTH, &sink));
        FeedPackets(&state, other, IDENT_LENGTH, 0, &sink);
        CHECK(test.connects == 0 && state.phase == SDL_USIO_PHASE_REJECTED && test.logs >= 2);
        for (now = 0; now < 3600000 * MS; now += 60000 * MS) {
            SDL_USIO_Tick(&state, now, &sink);
            CHECK(!SDL_USIO_NextCommand(&state, now, command));
        }
        FeedPackets(&state, ident, IDENT_LENGTH, now, &sink);
        CHECK(test.connects == 0 && state.phase == SDL_USIO_PHASE_REJECTED && state.dropped == IDENT_LENGTH);
        CHECK(!SDL_USIO_NextCommand(&state, now + 3600000 * MS, command));

        SDL_USIO_Init(&state, SDL_USIO_LAYOUT_TEKKEN, true);
        sink = Sink(&test, SDL_USIO_LAYOUT_TEKKEN);
        CHECK(Expect(&state, 0, 0x1800, IDENT_LENGTH, &sink));
        FeedPackets(&state, ident, IDENT_LENGTH, 0, &sink);
        CHECK(test.connects == 1 && state.phase == SDL_USIO_PHASE_POLL && test.logs == 0);
        CHECK(Expect(&state, 0, 0x1000, TEKKEN_LENGTH, &sink));
    }
}

/* 8 */
static void TestZeroLength(void)
{
    SDL_USIOState state;
    TestSink test;
    SDL_USIOSink sink;
    uint8_t block[TAIKO_LENGTH];
    uint8_t command[SDL_USIO_COMMAND_SIZE];
    const uint8_t filler = 0x55;

    memset(block, 0, sizeof(block));
    block[1] = 0x02;
    Put16(block, 32, 0x1800);

    /* Before the first command, as ITAIKO-firmware sends one
       (usio_driver.c:514-519): nothing changes and nothing waits */
    sink = Sink(&test, SDL_USIO_LAYOUT_TAIKO);
    SDL_USIO_Init(&state, SDL_USIO_LAYOUT_TAIKO, false);
    FeedExact(&state, block, 0, 0, &sink);
    SDL_USIO_Feed(&state, NULL, 0, 0, &sink);
    SDL_USIO_Feed(&state, &filler, 0, 0, &sink);
    CHECK(!state.quiet && state.dropped == 0 && test.logs == 0);
    CHECK(Expect(&state, 0, 0x1800, IDENT_LENGTH, &sink));

    /* Inside a reply */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    CHECK(Expect(&state, 0, 0x1080, TAIKO_LENGTH, &sink));
    FeedExact(&state, block, 10, 0, &sink);
    FeedExact(&state, block, 0, 0, &sink);
    CHECK(state.waiting && state.received == 10);
    FeedExact(&state, block + 10, TAIKO_LENGTH - 10, 0, &sink);
    CHECK(!state.waiting && Controls(&state, 0)->buttons == ((1u << 4) | 1u));

    /* After a reply of whole packets, as ITAIKO-firmware sends one after
       0x180 bytes (usio_driver.c:497-502): no drop, no wait */
    FeedExact(&state, block, 0, 0, &sink);
    CHECK(!state.quiet && state.dropped == 0);
    CHECK(SDL_USIO_NextCommand(&state, 0, command));
    SDL_USIO_CommandDone(&state, 6, 0, &sink);

    /* A zero-length transfer never ends a wait early or late */
    SDL_USIO_Tick(&state, SDL_USIO_REPLY_TIMEOUT_NS, &sink);
    CHECK(state.quiet && state.quiet_until == SDL_USIO_REPLY_TIMEOUT_NS + SDL_USIO_QUIET_NS);
    FeedExact(&state, block, 0, SDL_USIO_REPLY_TIMEOUT_NS + 50 * MS, &sink);
    CHECK(state.quiet_until == SDL_USIO_REPLY_TIMEOUT_NS + SDL_USIO_QUIET_NS);
    CHECK(SDL_USIO_NextCommand(&state, SDL_USIO_REPLY_TIMEOUT_NS + SDL_USIO_QUIET_NS, command));

    /* A NULL buffer with a length changes nothing */
    SDL_USIO_Feed(&state, NULL, 5, 0, &sink);
    CHECK(state.waiting && state.received == 0);
}

/* 9 */
static void TestReconnect(void)
{
    SDL_USIOState state;
    TestSink test;
    SDL_USIOSink sink;
    uint8_t block[TAIKO_LENGTH];
    uint8_t ident[IDENT_LENGTH];
    const uint16_t coin = 1u << 9;
    uint64_t t;
    int i;

    memset(block, 0, sizeof(block));
    IdentBlock(ident);
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Put16(block, 16, 7);
    block[1] = 0x20;
    Poll(&state, block, 0, &sink);
    Put16(block, 16, 8);
    Poll(&state, block, 0, &sink);
    CHECK(Controls(&state, 0)->buttons == (coin | (1u << 5)));

    /* The board is removed: its joysticks disconnect once */
    SDL_USIO_Detach(&state, &sink);
    CHECK(test.disconnects == 1 && !test.connected);
    CHECK(AtRest(Controls(&state, 0)) && !state.baseline && state.phase == SDL_USIO_PHASE_IDENTIFY);
    SDL_USIO_Detach(&state, &sink);
    CHECK(test.disconnects == 1);

    /* The next attach reads 1800 first and takes a new coin baseline */
    CHECK(Expect(&state, 5000 * MS, 0x1800, IDENT_LENGTH, &sink));
    FeedPackets(&state, ident, IDENT_LENGTH, 5000 * MS, &sink);
    CHECK(test.connects == 2 && test.connected);
    Put16(block, 16, 20);
    Poll(&state, block, 5000 * MS, &sink);
    CHECK(Controls(&state, 0)->buttons == (1u << 5));
    Put16(block, 16, 21);
    Poll(&state, block, 5000 * MS, &sink);
    CHECK(Controls(&state, 0)->buttons == (coin | (1u << 5)));

    /* A board removed mid-read starts over as well */
    CHECK(Expect(&state, 6000 * MS, 0x1080, TAIKO_LENGTH, &sink));
    FeedExact(&state, block, 20, 6000 * MS, &sink);
    SDL_USIO_Detach(&state, &sink);
    CHECK(test.disconnects == 2 && !state.waiting && state.received == 0);
    CHECK(Expect(&state, 6000 * MS, 0x1800, IDENT_LENGTH, &sink));
    /* The layout and the 0900 rule stay */
    CHECK(state.layout == SDL_USIO_LAYOUT_TAIKO && !state.require_ident);
    sink = Sink(&test, SDL_USIO_LAYOUT_TEKKEN);
    SDL_USIO_Init(&state, SDL_USIO_LAYOUT_TEKKEN, true);
    CHECK(Expect(&state, 7000 * MS, 0x1800, IDENT_LENGTH, &sink));
    FeedPackets(&state, ident, IDENT_LENGTH, 7000 * MS, &sink);
    SDL_USIO_Detach(&state, &sink);
    CHECK(test.disconnects == 1 && state.layout == SDL_USIO_LAYOUT_TEKKEN && state.require_ident);
    CHECK(Expect(&state, 7000 * MS, 0x1800, IDENT_LENGTH, &sink));
    FeedPackets(&state, ident, IDENT_LENGTH, 7000 * MS, &sink);
    CHECK(Expect(&state, 7000 * MS, 0x1000, TEKKEN_LENGTH, &sink));

    /* A board lost to silence and then removed disconnects once */
    sink = Sink(&test, SDL_USIO_LAYOUT_TAIKO);
    SDL_USIO_Init(&state, SDL_USIO_LAYOUT_TAIKO, false);
    CHECK(Expect(&state, 0, 0x1800, IDENT_LENGTH, &sink));
    FeedPackets(&state, ident, IDENT_LENGTH, 0, &sink);
    t = 0;
    for (i = 0; i < 3; ++i) {
        CHECK(Expect(&state, t, 0x1080, TAIKO_LENGTH, &sink));
        t += SDL_USIO_REPLY_TIMEOUT_NS;
        SDL_USIO_Tick(&state, t, &sink);
        t += SDL_USIO_QUIET_NS;
    }
    CHECK(test.disconnects == 1 && !state.connected);
    SDL_USIO_Detach(&state, &sink);
    CHECK(test.disconnects == 1 && test.connects == 1);
}

/* The reply timeout, the quiet wait and the failure count, on the injected
   clock */
static void TestTiming(void)
{
    SDL_USIOState state;
    TestSink test;
    SDL_USIOSink sink;
    uint8_t block[TAIKO_LENGTH];
    uint8_t command[SDL_USIO_COMMAND_SIZE];
    const uint64_t t0 = 10000 * MS;
    uint64_t t;
    int i;

    memset(block, 0, sizeof(block));

    /* 100 ms for a reply, then 100 ms of quiet before the read goes again */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    CHECK(SDL_USIO_NextCommand(&state, t0, command));
    SDL_USIO_CommandDone(&state, 6, t0, &sink);
    SDL_USIO_Tick(&state, t0 + 99 * MS, &sink);
    CHECK(state.waiting);
    SDL_USIO_Tick(&state, t0 + 100 * MS, &sink);
    CHECK(!state.waiting);
    CHECK(!SDL_USIO_NextCommand(&state, t0 + 199 * MS, command));
    CHECK(SDL_USIO_NextCommand(&state, t0 + 200 * MS, command));

    /* The wait starts when the write is done */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    CHECK(SDL_USIO_NextCommand(&state, t0, command));
    CHECK(state.deadline == t0 + SDL_USIO_REPLY_TIMEOUT_NS);
    SDL_USIO_CommandDone(&state, 6, t0 + 7 * MS, &sink);
    CHECK(state.deadline == t0 + 7 * MS + SDL_USIO_REPLY_TIMEOUT_NS);
    SDL_USIO_Tick(&state, t0 + 7 * MS + SDL_USIO_REPLY_TIMEOUT_NS - 1, &sink);
    CHECK(state.waiting && !SDL_USIO_NextCommand(&state, t0 + 7 * MS + SDL_USIO_REPLY_TIMEOUT_NS - 1, command));
    t = t0 + 7 * MS + SDL_USIO_REPLY_TIMEOUT_NS;
    SDL_USIO_Tick(&state, t, &sink);
    CHECK(!state.waiting && state.quiet && state.failures == 1 && test.logs == 1);
    /* The same read goes out once the quiet wait has passed */
    CHECK(!SDL_USIO_NextCommand(&state, t + SDL_USIO_QUIET_NS - 1, command));
    CHECK(Expect(&state, t + SDL_USIO_QUIET_NS, 0x1080, TAIKO_LENGTH, &sink));
    FeedPackets(&state, block, TAIKO_LENGTH, t + SDL_USIO_QUIET_NS, &sink);
    CHECK(state.failures == 0 && !state.quiet);
    /* A whole reply lets the next read go at once */
    CHECK(Expect(&state, t + SDL_USIO_QUIET_NS, 0x1080, TAIKO_LENGTH, &sink));
    FeedPackets(&state, block, TAIKO_LENGTH, t + SDL_USIO_QUIET_NS, &sink);

    /* The timeout runs only while a read is out */
    SDL_USIO_Tick(&state, t + 3600000 * MS, &sink);
    CHECK(!state.quiet && state.failures == 0 && test.logs == 1);

    /* Bytes no read asked for are dropped and logged, and push the next
       command back until they stop */
    t = t0 + 1000 * MS;
    i = test.logs;
    FeedExact(&state, block, 3, t, &sink);
    CHECK(state.dropped == 3 && state.quiet && test.logs == i + 1);
    FeedExact(&state, block, 5, t + 50 * MS, &sink);
    CHECK(state.dropped == 8 && test.logs == i + 1);
    CHECK(!SDL_USIO_NextCommand(&state, t + SDL_USIO_QUIET_NS, command));
    CHECK(!SDL_USIO_NextCommand(&state, t + 50 * MS + SDL_USIO_QUIET_NS - 1, command));
    CHECK(Expect(&state, t + 50 * MS + SDL_USIO_QUIET_NS, 0x1080, TAIKO_LENGTH, &sink));
    FeedPackets(&state, block, TAIKO_LENGTH, t + 50 * MS + SDL_USIO_QUIET_NS, &sink);
    CHECK(!state.quiet && !state.waiting);

    /* Bytes past a whole reply: the reply is used, the rest dropped */
    t = t0 + 2000 * MS;
    {
        uint8_t longer[TAIKO_LENGTH + 5];

        memset(longer, 0, sizeof(longer));
        longer[1] = 0x02;
        CHECK(Expect(&state, t, 0x1080, TAIKO_LENGTH, &sink));
        FeedExact(&state, longer, 64, t, &sink);
        FeedExact(&state, longer + 64, sizeof(longer) - 64, t, &sink);
        CHECK(Controls(&state, 0)->buttons == (1u << 4));
        CHECK(state.dropped == 8 + 5 && state.quiet);
        CHECK(!SDL_USIO_NextCommand(&state, t + SDL_USIO_QUIET_NS - 1, command));
        CHECK(Expect(&state, t + SDL_USIO_QUIET_NS, 0x1080, TAIKO_LENGTH, &sink));
        FeedPackets(&state, block, TAIKO_LENGTH, t + SDL_USIO_QUIET_NS, &sink);
        CHECK(Controls(&state, 0)->buttons == 0);
    }

    /* A failed or short write abandons the read at once */
    t = t0 + 3000 * MS;
    CHECK(SDL_USIO_NextCommand(&state, t, command));
    SDL_USIO_CommandDone(&state, -1, t, &sink);
    CHECK(!state.waiting && state.quiet && state.quiet_until == t + SDL_USIO_QUIET_NS && state.failures == 1);
    CHECK(!SDL_USIO_NextCommand(&state, t + SDL_USIO_QUIET_NS - 1, command));
    CHECK(SDL_USIO_NextCommand(&state, t + SDL_USIO_QUIET_NS, command));
    SDL_USIO_CommandDone(&state, 5, t + SDL_USIO_QUIET_NS, &sink);
    CHECK(!state.waiting && state.failures == 2 && test.connected);
    CHECK(SDL_USIO_NextCommand(&state, t + 2 * SDL_USIO_QUIET_NS, command));
    SDL_USIO_CommandDone(&state, 6, t + 2 * SDL_USIO_QUIET_NS, &sink);
    FeedPackets(&state, block, TAIKO_LENGTH, t + 2 * SDL_USIO_QUIET_NS, &sink);
    CHECK(state.failures == 0 && test.connected);
    CHECK(SDL_USIO_NextCommand(&state, t + 2 * SDL_USIO_QUIET_NS, command));
    SDL_USIO_CommandDone(&state, 7, t + 2 * SDL_USIO_QUIET_NS, &sink);
    CHECK(!state.waiting && state.failures == 1);
    /* A done with nothing out does nothing */
    SDL_USIO_CommandDone(&state, -1, t + 2 * SDL_USIO_QUIET_NS, &sink);
    CHECK(state.failures == 1);

    /* Two abandoned reads keep the joysticks, and a whole reply clears the
       count. Three in a row disconnect them, and the module starts over
       with the identification block and a new coin baseline. */
    Start(&state, SDL_USIO_LAYOUT_TAIKO, false, &test, &sink, 0);
    Put16(block, 16, 40);
    block[1] = 0x02;
    Poll(&state, block, 0, &sink);
    t = t0;
    for (i = 0; i < 2; ++i) {
        CHECK(Expect(&state, t, 0x1080, TAIKO_LENGTH, &sink));
        t += SDL_USIO_REPLY_TIMEOUT_NS;
        SDL_USIO_Tick(&state, t, &sink);
        t += SDL_USIO_QUIET_NS;
    }
    CHECK(state.failures == 2 && test.connected && Controls(&state, 0)->buttons == (1u << 4));
    Poll(&state, block, t, &sink);
    CHECK(state.failures == 0);
    /* Three coins: one pressed now and two waiting when the board goes */
    Put16(block, 16, 43);
    Poll(&state, block, t, &sink);
    CHECK(Controls(&state, 0)->buttons == ((1u << 4) | (1u << 9)));
    for (i = 0; i < 2; ++i) {
        CHECK(Expect(&state, t, 0x1080, TAIKO_LENGTH, &sink));
        t += SDL_USIO_REPLY_TIMEOUT_NS;
        SDL_USIO_Tick(&state, t, &sink);
        t += SDL_USIO_QUIET_NS;
    }
    CHECK(test.disconnects == 0);
    CHECK(Expect(&state, t, 0x1080, TAIKO_LENGTH, &sink));
    t += SDL_USIO_REPLY_TIMEOUT_NS;
    SDL_USIO_Tick(&state, t, &sink);
    CHECK(test.disconnects == 1 && !test.connected && state.phase == SDL_USIO_PHASE_IDENTIFY && !state.connected);
    CHECK(state.failures == 0 && AtRest(Controls(&state, 0)) && !state.baseline);
    CHECK(!SDL_USIO_NextCommand(&state, t + SDL_USIO_QUIET_NS - 1, command));
    t += SDL_USIO_QUIET_NS;
    {
        uint8_t ident[IDENT_LENGTH];

        IdentBlock(ident);
        CHECK(Expect(&state, t, 0x1800, IDENT_LENGTH, &sink));
        FeedPackets(&state, ident, IDENT_LENGTH, t, &sink);
    }
    CHECK(test.connects == 2 && test.connected);
    Put16(block, 16, 90);
    Poll(&state, block, t, &sink);
    CHECK(Controls(&state, 0)->buttons == (1u << 4));
    /* The coins that waited before the loss never press */
    for (i = 0; i < 4; ++i) {
        Poll(&state, block, t, &sink);
        CHECK(Controls(&state, 0)->buttons == (1u << 4));
    }

    /* A board that never answers its identification: no joystick, reads
       again forever, and one log line */
    sink = Sink(&test, SDL_USIO_LAYOUT_TEKKEN);
    SDL_USIO_Init(&state, SDL_USIO_LAYOUT_TEKKEN, false);
    t = 0;
    for (i = 0; i < 20; ++i) {
        CHECK(Expect(&state, t, 0x1800, IDENT_LENGTH, &sink));
        t += SDL_USIO_REPLY_TIMEOUT_NS;
        SDL_USIO_Tick(&state, t, &sink);
        t += SDL_USIO_QUIET_NS;
    }
    CHECK(test.connects == 0 && test.disconnects == 0 && test.logs == 1 && state.failures == SDL_USIO_MAX_FAILURES);
    /* It answers at last */
    {
        uint8_t ident[IDENT_LENGTH];

        IdentBlock(ident);
        CHECK(Expect(&state, t, 0x1800, IDENT_LENGTH, &sink));
        FeedPackets(&state, ident, IDENT_LENGTH, t, &sink);
        CHECK(test.connects == 1 && state.failures == 0);
    }
    /* A lost board logs its loss once */
    for (i = 0; i < 3; ++i) {
        CHECK(Expect(&state, t, 0x1000, TEKKEN_LENGTH, &sink));
        t += SDL_USIO_REPLY_TIMEOUT_NS;
        SDL_USIO_Tick(&state, t, &sink);
        t += SDL_USIO_QUIET_NS;
    }
    CHECK(test.disconnects == 1 && test.logs == 3);
}

static void TestTables(void)
{
    SDL_USIOJoystickInfo info;
    SDL_USIOState state;
    TestSink test;
    SDL_USIOSink sink = Sink(&test, SDL_USIO_LAYOUT_TAIKO);
    uint8_t command[SDL_USIO_COMMAND_SIZE];
    int player;

    CHECK(SDL_USIO_PlayerCount(SDL_USIO_LAYOUT_TAIKO) == 2 && SDL_USIO_PlayerCount(SDL_USIO_LAYOUT_TEKKEN) == 4);
    CHECK(strcmp(SDL_USIO_DeviceName(SDL_USIO_LAYOUT_TAIKO), "Namco USIO Taiko Drum") == 0);
    CHECK(strcmp(SDL_USIO_DeviceName(SDL_USIO_LAYOUT_TEKKEN), "Namco USIO Tekken") == 0);
    CHECK(SDL_USIO_JoystickType(SDL_USIO_LAYOUT_TAIKO) == 7 && SDL_USIO_JoystickType(SDL_USIO_LAYOUT_TEKKEN) == 3);
    /* GUID byte 15 names the layout in saved mappings, so its values stay */
    CHECK(SDL_USIO_GUIDByte(SDL_USIO_LAYOUT_TAIKO) == 0x01 && SDL_USIO_GUIDByte(SDL_USIO_LAYOUT_TEKKEN) == 0x02);
    /* SDL's hat bits */
    CHECK(SDL_USIO_HAT_UP == 0x01 && SDL_USIO_HAT_RIGHT == 0x02 && SDL_USIO_HAT_DOWN == 0x04 && SDL_USIO_HAT_LEFT == 0x08);

    /* Taiko: two drums, player 1 with the panel and Coin */
    CHECK(SDL_USIO_GetJoystickInfo(SDL_USIO_LAYOUT_TAIKO, 0, &info));
    CHECK(strcmp(info.name, "Namco USIO Taiko Drum P1") == 0 && info.naxes == 4 && info.nbuttons == 10 && info.nhats == 0);
    CHECK(SDL_USIO_GetJoystickInfo(SDL_USIO_LAYOUT_TAIKO, 1, &info));
    CHECK(strcmp(info.name, "Namco USIO Taiko Drum P2") == 0 && info.naxes == 4 && info.nbuttons == 4 && info.nhats == 0);
    CHECK(!SDL_USIO_GetJoystickInfo(SDL_USIO_LAYOUT_TAIKO, 2, &info));
    CHECK(!SDL_USIO_GetJoystickInfo(SDL_USIO_LAYOUT_TAIKO, -1, &info));
    CHECK(!SDL_USIO_GetJoystickInfo(SDL_USIO_LAYOUT_TAIKO, 0, NULL));

    /* Tekken: four sticks, players 1 and 3 with Test, Service and Coin */
    for (player = 0; player < 4; ++player) {
        char name[32] = "Namco USIO Tekken P";

        name[19] = (char)('1' + player);
        name[20] = '\0';
        CHECK(SDL_USIO_GetJoystickInfo(SDL_USIO_LAYOUT_TEKKEN, player, &info));
        CHECK(strcmp(info.name, name) == 0 && info.naxes == 0 && info.nhats == 1);
        CHECK(info.nbuttons == ((player % 2 == 0) ? 9 : 6));
    }
    CHECK(!SDL_USIO_GetJoystickInfo(SDL_USIO_LAYOUT_TEKKEN, 4, &info));

    /* Every button a player's controls can hold is one its joystick has */
    CHECK(SDL_USIO_TAIKO_COIN == 9 && SDL_USIO_TAIKO_SIDE_RIGHT == 3);
    CHECK(SDL_USIO_TEKKEN_COIN == 8 && SDL_USIO_TEKKEN_ENTER == 5);

    /* The mapping: RPCS3's default pad, stick to D-pad, 1 West, 2 North,
       3 South, 4 East, 5 right shoulder, Enter Start */
    CHECK(SDL_USIO_GetMapping(SDL_USIO_GUIDByte(SDL_USIO_LAYOUT_TEKKEN)) != NULL);
    CHECK(strcmp(SDL_USIO_GetMapping(SDL_USIO_GUID_TEKKEN),
                 "a:b2,b:b3,x:b0,y:b1,rightshoulder:b4,start:b5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,") == 0);
    CHECK(SDL_USIO_GetMapping(SDL_USIO_GUIDByte(SDL_USIO_LAYOUT_TAIKO)) == NULL);
    CHECK(SDL_USIO_GetMapping(0x00) == NULL && SDL_USIO_GetMapping(0x03) == NULL && SDL_USIO_GetMapping(0xFF) == NULL);
    /* Every button the mapping names is one every Tekken joystick has */
    {
        const char *mapping = SDL_USIO_TEKKEN_MAPPING;
        const char *p;
        int highest = -1;

        for (p = mapping; *p; ++p) {
            if (*p == ':' && p[1] == 'b') {
                const int button = atoi(p + 2);
                if (button > highest) {
                    highest = button;
                }
            }
        }
        CHECK(highest == 5);
    }

    /* The hint: "tekken" in any case, Taiko otherwise */
    CHECK(SDL_USIO_LayoutFromHint(NULL) == SDL_USIO_LAYOUT_TAIKO);
    CHECK(SDL_USIO_LayoutFromHint("") == SDL_USIO_LAYOUT_TAIKO);
    CHECK(SDL_USIO_LayoutFromHint("taiko") == SDL_USIO_LAYOUT_TAIKO);
    CHECK(SDL_USIO_LayoutFromHint("tekken") == SDL_USIO_LAYOUT_TEKKEN);
    CHECK(SDL_USIO_LayoutFromHint("TEKKEN") == SDL_USIO_LAYOUT_TEKKEN);
    CHECK(SDL_USIO_LayoutFromHint("Tekken") == SDL_USIO_LAYOUT_TEKKEN);
    CHECK(SDL_USIO_LayoutFromHint("tekke") == SDL_USIO_LAYOUT_TAIKO);
    CHECK(SDL_USIO_LayoutFromHint("tekken ") == SDL_USIO_LAYOUT_TAIKO);
    CHECK(SDL_USIO_LayoutFromHint("tekkenx") == SDL_USIO_LAYOUT_TAIKO);
    CHECK(SDL_USIO_LayoutFromHint("xtekken") == SDL_USIO_LAYOUT_TAIKO);
    CHECK(SDL_USIO_LayoutFromHint("1") == SDL_USIO_LAYOUT_TAIKO);

    /* Out-of-range layouts start as Taiko */
    SDL_USIO_Init(&state, (SDL_USIOLayout)7, false);
    CHECK(state.layout == SDL_USIO_LAYOUT_TAIKO && state.phase == SDL_USIO_PHASE_IDENTIFY);

    /* Controls exist for the layout's players only */
    CHECK(SDL_USIO_GetControls(&state, 0) != NULL && SDL_USIO_GetControls(&state, 1) != NULL);
    CHECK(SDL_USIO_GetControls(&state, 2) == NULL && SDL_USIO_GetControls(&state, -1) == NULL);
    CHECK(SDL_USIO_GetControls(NULL, 0) == NULL);
    SDL_USIO_Init(&state, SDL_USIO_LAYOUT_TEKKEN, false);
    CHECK(SDL_USIO_GetControls(&state, 3) != NULL && SDL_USIO_GetControls(&state, 4) == NULL);

    /* NULL arguments change nothing */
    SDL_USIO_Init(NULL, SDL_USIO_LAYOUT_TAIKO, false);
    CHECK(!SDL_USIO_NextCommand(NULL, 0, command));
    CHECK(!SDL_USIO_NextCommand(&state, 0, NULL));
    CHECK(!state.waiting);
    SDL_USIO_CommandDone(NULL, 6, 0, &sink);
    SDL_USIO_Feed(NULL, command, 6, 0, &sink);
    SDL_USIO_Tick(NULL, 0, &sink);
    SDL_USIO_Detach(NULL, &sink);
    CHECK(SDL_USIO_NextCommand(&state, 0, command));
    SDL_USIO_Feed(&state, command, 6, 0, NULL);
    SDL_USIO_Tick(&state, SDL_USIO_REPLY_TIMEOUT_NS, NULL);
    SDL_USIO_CommandDone(&state, -1, 0, NULL);
    SDL_USIO_Detach(&state, NULL);
    CHECK(state.waiting && state.received == 0 && state.failures == 0);
}

int main(void)
{
    TestCommands();
    TestTaikoIdle();
    TestTaikoControls();
    TestTaikoCoin();
    TestTekken();
    TestTruncation();
    TestSplits();
    TestIdentification();
    TestZeroLength();
    TestReconnect();
    TestTiming();
    TestTables();
    printf("%s: %d checks, %d commands, %d failures\n", failures ? "FAILED" : "PASSED", checks, commands, failures);
    return failures ? 1 : 0;
}
