/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_ergodex_proto.c, the
 * Ergodex DX1 of hifihedgehog/SDL#33 Part 14. Test numbers follow the part.
 * The packets are hardware captures printed in dx1-studio's
 * docs/hardware_audit.md and ergodex-dx1-linux's docs/PROTOCOL.md, 16 bytes
 * with trailing zeros, plus constructed cases. The start-up runs on an
 * injected clock. No device, no SDL runtime. Every packet is also fed from an
 * exact-size heap copy, so AddressSanitizer catches a read past the length
 * the module was given. */

#include "../src/joystick/hidapi/SDL_hidapi_ergodex_proto.h"
#include "../src/hidapi/SDL_hidapi_vendorusb.h"
#include "../src/joystick/usb_ids.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        ++checks;                                                                     \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);               \
        }                                                                             \
    } while (0)

#define MS (1000000ULL)
#define BIT(button) (1ULL << (button))

/* A 16-byte packet from its leading bytes, the rest zero */
typedef struct Packet
{
    uint8_t bytes[SDL_ERGODEX_PACKET_LENGTH];
} Packet;

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static Packet Make(const char *hex)
{
    Packet p;
    size_t n = 0;

    memset(&p, 0, sizeof(p));
    while (*hex) {
        int high, low;

        if (*hex == ' ') {
            ++hex;
            continue;
        }
        high = HexDigit(hex[0]);
        low = hex[1] ? HexDigit(hex[1]) : -1;
        if (high < 0 || low < 0 || n >= sizeof(p.bytes)) {
            ++failures;
            printf("FAIL: bad packet text %s\n", hex);
            break;
        }
        p.bytes[n++] = (uint8_t)((high << 4) | low);
        hex += 2;
    }
    return p;
}

/* Applies a packet from an exact-size heap copy */
static int Feed(SDL_ErgodexState *state, const uint8_t *data, size_t length)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    int result;

    if (!copy) {
        ++failures;
        printf("FAIL: out of memory\n");
        return -1;
    }
    if (length) {
        memcpy(copy, data, length);
    }
    result = SDL_Ergodex_HandlePacket(state, copy, length);
    free(copy);
    return result;
}

static int FeedHex(SDL_ErgodexState *state, const char *hex)
{
    const Packet p = Make(hex);

    return Feed(state, p.bytes, sizeof(p.bytes));
}

static bool Same(const SDL_ErgodexState *a, const SDL_ErgodexState *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* 1 */
static void TestStatus(void)
{
    SDL_ErgodexState state;
    int value;

    SDL_Ergodex_Init(&state, 0);
    /* hardware_audit.md:75, :74 and :84 */
    CHECK(FeedHex(&state, "01 04 00 01 03 00") == SDL_ERGODEX_PACKET_STATUS && state.buttons == 0);
    CHECK(FeedHex(&state, "01 04 00 01 02 00") == SDL_ERGODEX_PACKET_STATUS && state.buttons == BIT(SDL_ERGODEX_BUTTON_TOP));
    CHECK(FeedHex(&state, "01 04 00 01 01 00") == SDL_ERGODEX_PACKET_STATUS && state.buttons == BIT(SDL_ERGODEX_BUTTON_LOWER));
    CHECK(FeedHex(&state, "01 04 00 01 00 00") == SDL_ERGODEX_PACKET_STATUS &&
          state.buttons == (BIT(SDL_ERGODEX_BUTTON_TOP) | BIT(SDL_ERGODEX_BUTTON_LOWER)));
    CHECK(FeedHex(&state, "01 04 00 01 03 00") == SDL_ERGODEX_PACKET_STATUS && state.buttons == 0);

    /* Every value of byte 4: bits 0 and 1 active low, the rest ignored, and
     * the held keys untouched */
    for (value = 0; value < 256; ++value) {
        Packet p = Make("01 04 00 01 00 00");
        uint64_t expected = BIT(4) | BIT(49);

        SDL_Ergodex_Init(&state, 0);
        CHECK(FeedHex(&state, "03 01 01 05 32") == SDL_ERGODEX_PACKET_KEYS);
        p.bytes[4] = (uint8_t)value;
        if (!(value & 0x01)) {
            expected |= BIT(SDL_ERGODEX_BUTTON_TOP);
        }
        if (!(value & 0x02)) {
            expected |= BIT(SDL_ERGODEX_BUTTON_LOWER);
        }
        CHECK(Feed(&state, p.bytes, sizeof(p.bytes)) == SDL_ERGODEX_PACKET_STATUS);
        CHECK(state.buttons == expected);
    }

    /* The mode echo, the LEDs, the key count and the rest do not matter */
    SDL_Ergodex_Init(&state, 0);
    CHECK(FeedHex(&state, "01 FF FF FF 02 FF FF FF FF FF FF FF FF FF FF FF") == SDL_ERGODEX_PACKET_STATUS);
    CHECK(state.buttons == BIT(SDL_ERGODEX_BUTTON_TOP) && !state.have_device);
}

/* 1 and 2: the button and LED probe of 19:30, hardware_audit.md:106-141,
 * in order */
static void TestProbeCapture(void)
{
    static const struct
    {
        const char *hex;
        int type;
        uint64_t buttons;
    } steps[] = {
        { "01 04 00 01 02 00", SDL_ERGODEX_PACKET_STATUS, BIT(SDL_ERGODEX_BUTTON_TOP) },   /* top-short */
        { "01 04 00 01 03 00", SDL_ERGODEX_PACKET_STATUS, 0 },
        { "01 04 00 01 02 00", SDL_ERGODEX_PACKET_STATUS, BIT(SDL_ERGODEX_BUTTON_TOP) },   /* top-hold */
        { "01 04 00 01 03 00", SDL_ERGODEX_PACKET_STATUS, 0 },                             /* top-release */
        { "01 04 00 01 01 00", SDL_ERGODEX_PACKET_STATUS, BIT(SDL_ERGODEX_BUTTON_LOWER) }, /* lower-short */
        { "01 04 00 01 03 00", SDL_ERGODEX_PACKET_STATUS, 0 },
        { "01 04 00 01 01 00", SDL_ERGODEX_PACKET_STATUS, BIT(SDL_ERGODEX_BUTTON_LOWER) }, /* lower-hold */
        { "01 04 00 01 03 00", SDL_ERGODEX_PACKET_STATUS, 0 },                             /* lower-release */
        { "02 01 01 01 00 00", SDL_ERGODEX_PACKET_KEYS, BIT(0) },                          /* key-control */
        { "02 01 01 00 00 00", SDL_ERGODEX_PACKET_KEYS, 0 },
        { "01 04 00 01 01 00", SDL_ERGODEX_PACKET_STATUS, BIT(SDL_ERGODEX_BUTTON_LOWER) }, /* lower-with-key */
        { "02 01 01 01 00 00", SDL_ERGODEX_PACKET_KEYS, BIT(SDL_ERGODEX_BUTTON_LOWER) | BIT(0) },
        { "01 04 00 01 03 00", SDL_ERGODEX_PACKET_STATUS, BIT(0) },
        { "02 01 01 00 00 00", SDL_ERGODEX_PACKET_KEYS, 0 },
        { "01 04 00 01 02 00", SDL_ERGODEX_PACKET_STATUS, BIT(SDL_ERGODEX_BUTTON_TOP) },   /* top-short-2 */
        { "01 04 00 01 03 00", SDL_ERGODEX_PACKET_STATUS, 0 },
        { "01 04 00 01 01 00", SDL_ERGODEX_PACKET_STATUS, BIT(SDL_ERGODEX_BUTTON_LOWER) }, /* lower-short-2 */
        { "01 04 00 01 03 00", SDL_ERGODEX_PACKET_STATUS, 0 },
    };
    SDL_ErgodexState state;
    size_t i;

    SDL_Ergodex_Init(&state, 0);
    for (i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        CHECK(FeedHex(&state, steps[i].hex) == steps[i].type);
        CHECK(state.buttons == steps[i].buttons);
    }
}

/* 2 and 3 */
static void TestKeys(void)
{
    SDL_ErgodexState state;

    /* MACRO, hardware_audit.md:126-127 */
    SDL_Ergodex_Init(&state, 0);
    CHECK(FeedHex(&state, "02 01 01 01 00 00 00 00 00 00 00 00 00 00 00 00") == SDL_ERGODEX_PACKET_KEYS);
    CHECK(state.buttons == BIT(0));
    CHECK(FeedHex(&state, "02 01 01 00 00 00 00 00 00 00 00 00 00 00 00 00") == SDL_ERGODEX_PACKET_KEYS);
    CHECK(state.buttons == 0);

    /* TEST, PROTOCOL.md:159-162 */
    CHECK(FeedHex(&state, "03 01 01 15 00") == SDL_ERGODEX_PACKET_KEYS && state.buttons == BIT(20));
    CHECK(FeedHex(&state, "03 01 01 00 00") == SDL_ERGODEX_PACKET_KEYS && state.buttons == 0);
    CHECK(FeedHex(&state, "03 01 01 0D 00") == SDL_ERGODEX_PACKET_KEYS && state.buttons == BIT(12));

    /* The two types decode alike, whatever bytes 1 and 2 and 9 to 15 hold */
    SDL_Ergodex_Init(&state, 0);
    CHECK(FeedHex(&state, "02 00 00 0D 00 00 00 00 00 FF FF FF FF FF FF FF") == SDL_ERGODEX_PACKET_KEYS && state.buttons == BIT(12));
    CHECK(FeedHex(&state, "03 7F 06 0D 00 00 00 00 00 FF FF FF FF FF FF FF") == SDL_ERGODEX_PACKET_KEYS && state.buttons == BIT(12));
    CHECK(FeedHex(&state, "02 01 01 15 00") == SDL_ERGODEX_PACKET_KEYS && state.buttons == BIT(20));
    CHECK(FeedHex(&state, "03 01 01 00 00") == SDL_ERGODEX_PACKET_KEYS && state.buttons == 0);
}

/* 4 */
static void TestDevice(void)
{
    SDL_ErgodexState state;
    char serial[17];

    SDL_Ergodex_Init(&state, 0);
    CHECK(FeedHex(&state, "01 04 00 01 02 00") == SDL_ERGODEX_PACKET_STATUS);
    /* PROTOCOL.md:193-196 */
    CHECK(FeedHex(&state, "0A 05 28 95 01 31 97 07 39 01 01 00 00 00 00 00") == SDL_ERGODEX_PACKET_DEVICE);
    CHECK(state.have_device && state.serial == 0x3907973101952805ULL);
    CHECK(state.version[0] == 1 && state.version[1] == 1);
    CHECK(state.buttons == BIT(SDL_ERGODEX_BUTTON_TOP));
    SDL_Ergodex_FormatSerial(state.serial, serial);
    CHECK(strcmp(serial, "3907973101952805") == 0);

    /* Each serial byte lands in its own place, and the version bytes are 9 and 10 */
    {
        Packet p = Make("0A 01 02 03 04 05 06 07 08 09 0A FF FF FF FF FF");

        CHECK(Feed(&state, p.bytes, sizeof(p.bytes)) == SDL_ERGODEX_PACKET_DEVICE);
        CHECK(state.serial == 0x0807060504030201ULL && state.version[0] == 9 && state.version[1] == 10);
        SDL_Ergodex_FormatSerial(state.serial, serial);
        CHECK(strcmp(serial, "0807060504030201") == 0);
    }
    SDL_Ergodex_FormatSerial(0, serial);
    CHECK(strcmp(serial, "0000000000000000") == 0);
    SDL_Ergodex_FormatSerial(0xFEDCBA9876543210ULL, serial);
    CHECK(strcmp(serial, "fedcba9876543210") == 0);
    SDL_Ergodex_FormatSerial(1, NULL);
}

/* 5 */
static void TestSnapshots(void)
{
    SDL_ErgodexState state;
    int id, slot;

    SDL_Ergodex_Init(&state, 0);
    CHECK(FeedHex(&state, "03 01 01 01 02 03 04 05 06") == SDL_ERGODEX_PACKET_KEYS);
    CHECK(state.buttons == (BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5)));
    CHECK(FeedHex(&state, "03 01 01 02 00") == SDL_ERGODEX_PACKET_KEYS && state.buttons == BIT(1));

    /* 00 and IDs above 50 hold nothing, in any slot */
    CHECK(FeedHex(&state, "03 01 01 00 33 FF 80 00 00") == SDL_ERGODEX_PACKET_KEYS && state.buttons == 0);
    CHECK(FeedHex(&state, "03 01 01 32 33 00 00 00 00") == SDL_ERGODEX_PACKET_KEYS && state.buttons == BIT(49));

    /* Every ID in every slot */
    for (slot = 0; slot < 6; ++slot) {
        for (id = 0; id < 256; ++id) {
            Packet p = Make("03 01 01");

            p.bytes[3 + slot] = (uint8_t)id;
            SDL_Ergodex_Init(&state, 0);
            CHECK(FeedHex(&state, "01 04 00 01 01 00") == SDL_ERGODEX_PACKET_STATUS);
            CHECK(Feed(&state, p.bytes, sizeof(p.bytes)) == SDL_ERGODEX_PACKET_KEYS);
            if (id >= 1 && id <= 50) {
                CHECK(state.buttons == (BIT(id - 1) | BIT(SDL_ERGODEX_BUTTON_LOWER)));
            } else {
                CHECK(state.buttons == BIT(SDL_ERGODEX_BUTTON_LOWER));
            }
        }
    }

    /* Byte 9 is past the six slots */
    SDL_Ergodex_Init(&state, 0);
    CHECK(FeedHex(&state, "03 01 01 00 00 00 00 00 00 07") == SDL_ERGODEX_PACKET_KEYS && state.buttons == 0);

    /* A key report leaves the pad buttons alone */
    SDL_Ergodex_Init(&state, 0);
    CHECK(FeedHex(&state, "01 04 00 01 00 00") == SDL_ERGODEX_PACKET_STATUS);
    CHECK(FeedHex(&state, "02 01 01 01 00") == SDL_ERGODEX_PACKET_KEYS);
    CHECK(FeedHex(&state, "02 01 01 00 00") == SDL_ERGODEX_PACKET_KEYS);
    CHECK(state.buttons == (BIT(SDL_ERGODEX_BUTTON_TOP) | BIT(SDL_ERGODEX_BUTTON_LOWER)));

    /* A repeated ID is one key */
    CHECK(FeedHex(&state, "03 01 01 07 07 07 00") == SDL_ERGODEX_PACKET_KEYS);
    CHECK(state.buttons == (BIT(6) | BIT(SDL_ERGODEX_BUTTON_TOP) | BIT(SDL_ERGODEX_BUTTON_LOWER)));
}

/* Types the pad does not send change nothing */
static void TestIgnoredTypes(void)
{
    int type;

    for (type = 0; type < 256; ++type) {
        SDL_ErgodexState state, before;
        Packet p = Make("00 05 28 95 01 31 97 07 39 01 01 00 00 00 00 00");

        if (type == 0x01 || type == 0x02 || type == 0x03 || type == 0x0A) {
            continue;
        }
        SDL_Ergodex_Init(&state, 0);
        CHECK(FeedHex(&state, "03 01 01 03 00") == SDL_ERGODEX_PACKET_KEYS);
        before = state;
        p.bytes[0] = (uint8_t)type;
        CHECK(Feed(&state, p.bytes, sizeof(p.bytes)) == SDL_ERGODEX_PACKET_IGNORED);
        CHECK(Same(&state, &before));
    }
}

/* The part's literal start-up packets */
static bool IsOutput(const uint8_t *out, const char *hex)
{
    const Packet p = Make(hex);

    return memcmp(out, p.bytes, sizeof(p.bytes)) == 0;
}

/* Takes the next output at now and completes it with written */
static bool Step(SDL_ErgodexState *state, uint64_t now, int written, uint8_t out[16])
{
    if (!SDL_Ergodex_NextOutput(state, now, out)) {
        return false;
    }
    SDL_Ergodex_OutputDone(state, written, now);
    return true;
}

/* 6 */
static void TestStartup(void)
{
    SDL_ErgodexState state;
    uint8_t out[16], sent[SDL_ERGODEX_STARTUP_OUTPUTS][16];
    uint64_t now = 5 * MS;
    int i, sets = 0;

    SDL_Ergodex_Init(&state, now);
    for (i = 0; i < SDL_ERGODEX_STARTUP_OUTPUTS; ++i) {
        if (i > 0) {
            /* 10 ms after the last OUT transfer completed, not before */
            CHECK(!SDL_Ergodex_NextOutput(&state, now + SDL_ERGODEX_PACE_NS - 1, out));
            now += SDL_ERGODEX_PACE_NS;
        }
        CHECK(SDL_Ergodex_NextOutput(&state, now, out));
        memcpy(sent[i], out, 16);
        /* Nothing else goes out while one is in flight */
        CHECK(!SDL_Ergodex_NextOutput(&state, now + 1000 * MS, out));
        now += 3 * MS; /* The transfer takes a while */
        SDL_Ergodex_OutputDone(&state, 16, now);
    }
    CHECK(state.step == SDL_ERGODEX_STARTUP_OUTPUTS && !state.stopped && !state.in_flight);
    CHECK(!SDL_Ergodex_NextOutput(&state, now + 1000000 * MS, out));

    CHECK(IsOutput(sent[0], "0A 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"));
    CHECK(IsOutput(sent[1], "02 00 00 01 00 01 01 01 00 00 00 00 00 00 00 00"));
    CHECK(IsOutput(sent[2], "03 01 03 00 02 03 00 03 03 00 04 03 00 05 03 00"));
    CHECK(IsOutput(sent[3], "03 06 03 00 07 03 00 08 03 00 09 03 00 0A 03 00"));
    CHECK(IsOutput(sent[4], "03 0B 03 00 0C 03 00 0D 03 00 0E 03 00 0F 03 00"));
    CHECK(IsOutput(sent[5], "03 10 03 00 11 03 00 12 03 00 13 03 00 14 03 00"));
    CHECK(IsOutput(sent[6], "03 15 03 00 16 03 00 17 03 00 18 03 00 19 03 00"));
    CHECK(IsOutput(sent[7], "03 1A 03 00 1B 03 00 1C 03 00 1D 03 00 1E 03 00"));
    CHECK(IsOutput(sent[8], "03 1F 03 00 20 03 00 21 03 00 22 03 00 23 03 00"));
    CHECK(IsOutput(sent[9], "03 24 03 00 25 03 00 26 03 00 27 03 00 28 03 00"));
    CHECK(IsOutput(sent[10], "03 29 03 00 2A 03 00 2B 03 00 2C 03 00 2D 03 00"));
    CHECK(IsOutput(sent[11], "03 2E 03 00 2F 03 00 30 03 00 31 03 00 32 03 00"));
    CHECK(IsOutput(sent[12], "02 00 01 01 00 00 01 01 00 00 00 00 00 00 00 00"));

    /* Byte 2 of every SET is never 06, every key 1 to 50 is programmed once
     * as type 3 with value 00, and no output starts with 00, the byte
     * hid_write would strip as a report number without the rule's flag */
    {
        int programmed[51];
        int k;

        memset(programmed, 0, sizeof(programmed));
        for (i = 0; i < SDL_ERGODEX_STARTUP_OUTPUTS; ++i) {
            CHECK(sent[i][0] != 0x00);
            if (sent[i][0] == 0x02) {
                ++sets;
                CHECK(sent[i][2] != 0x06);
            }
            if (sent[i][0] == 0x03) {
                for (k = 0; k < 5; ++k) {
                    const int id = sent[i][1 + k * 3];

                    CHECK(id >= 1 && id <= 50 && sent[i][2 + k * 3] == 3 && sent[i][3 + k * 3] == 0);
                    if (id >= 1 && id <= 50) {
                        ++programmed[id];
                    }
                }
            }
        }
        for (k = 1; k <= 50; ++k) {
            CHECK(programmed[k] == 1);
        }
    }
    CHECK(sets == 2);

    /* The table of steps agrees, and nothing lies past it */
    for (i = 0; i < SDL_ERGODEX_STARTUP_OUTPUTS; ++i) {
        CHECK(SDL_Ergodex_StartupOutput(i, out) && memcmp(out, sent[i], 16) == 0);
    }
    memset(out, 0xEE, sizeof(out));
    CHECK(!SDL_Ergodex_StartupOutput(-1, out) && out[0] == 0xEE);
    CHECK(!SDL_Ergodex_StartupOutput(SDL_ERGODEX_STARTUP_OUTPUTS, out) && out[0] == 0xEE);
    CHECK(!SDL_Ergodex_StartupOutput(0, NULL));

    /* The first output is due at once, and not before the time Init took */
    SDL_Ergodex_Init(&state, 100 * MS);
    CHECK(!SDL_Ergodex_NextOutput(&state, 100 * MS - 1, out));
    CHECK(SDL_Ergodex_NextOutput(&state, 100 * MS, out) && IsOutput(out, "0A"));
}

/* 6: a failed or short write goes out again after the retry delay, once */
static void TestStartupFailures(void)
{
    SDL_ErgodexState state;
    uint8_t out[16];
    uint64_t now = 0;
    int written;

    /* One failure, then success: the same packet again, 50 ms later */
    SDL_Ergodex_Init(&state, now);
    CHECK(Step(&state, now, 16, out) && IsOutput(out, "0A"));
    now += SDL_ERGODEX_PACE_NS;
    CHECK(Step(&state, now, -1, out) && IsOutput(out, "02 00 00 01 00 01 01 01"));
    CHECK(state.retried && !state.stopped && state.step == 1);
    CHECK(!SDL_Ergodex_NextOutput(&state, now + SDL_ERGODEX_RETRY_NS - 1, out));
    now += SDL_ERGODEX_RETRY_NS;
    CHECK(Step(&state, now, 16, out) && IsOutput(out, "02 00 00 01 00 01 01 01"));
    CHECK(!state.retried && state.step == 2);
    /* The next output keeps the plain pause */
    CHECK(!SDL_Ergodex_NextOutput(&state, now + SDL_ERGODEX_PACE_NS - 1, out));
    now += SDL_ERGODEX_PACE_NS;
    /* A later failure has its own retry */
    CHECK(Step(&state, now, 0, out) && IsOutput(out, "03 01 03 00 02 03 00 03 03 00 04 03 00 05 03 00"));
    CHECK(state.retried && !state.stopped);
    now += SDL_ERGODEX_RETRY_NS;
    CHECK(Step(&state, now, 16, out) && IsOutput(out, "03 01 03 00 02 03 00 03 03 00 04 03 00 05 03 00"));
    CHECK(state.step == 3 && !state.stopped);

    /* Every short count is a failure, and a second one ends the start-up */
    for (written = -2; written < 16; ++written) {
        SDL_Ergodex_Init(&state, 0);
        CHECK(Step(&state, 0, written, out) && IsOutput(out, "0A"));
        CHECK(state.retried && state.step == 0 && !state.stopped);
        CHECK(Step(&state, SDL_ERGODEX_RETRY_NS, written, out) && IsOutput(out, "0A"));
        CHECK(state.stopped && state.step == 0 && !state.in_flight);
        CHECK(!SDL_Ergodex_NextOutput(&state, 1000000 * MS, out));
    }
    /* A count above 16 is not a whole write either */
    SDL_Ergodex_Init(&state, 0);
    CHECK(Step(&state, 0, 17, out) && state.retried && state.step == 0);

    /* Completing nothing changes nothing */
    SDL_Ergodex_Init(&state, 0);
    {
        SDL_ErgodexState before = state;

        SDL_Ergodex_OutputDone(&state, 16, 7 * MS);
        CHECK(Same(&state, &before));
        SDL_Ergodex_OutputDone(NULL, 16, 0);
        CHECK(!SDL_Ergodex_NextOutput(NULL, 0, out));
        CHECK(!SDL_Ergodex_NextOutput(&state, 0, NULL) && Same(&state, &before));
    }

    /* Key reports keep working after the start-up ended */
    SDL_Ergodex_Init(&state, 0);
    CHECK(Step(&state, 0, -1, out));
    CHECK(Step(&state, SDL_ERGODEX_RETRY_NS, -1, out) && state.stopped);
    CHECK(FeedHex(&state, "03 01 01 2A 00") == SDL_ERGODEX_PACKET_KEYS && state.buttons == BIT(41));
}

/* 7: a STATUS that arrives before the DEVICE reply is handled, and the reply
 * is still read. The start-up does not wait for the reply. */
static void TestStatusBeforeDevice(void)
{
    SDL_ErgodexState state;
    uint8_t out[16];
    uint64_t now = 0;

    SDL_Ergodex_Init(&state, now);
    CHECK(Step(&state, now, 16, out) && IsOutput(out, "0A"));
    CHECK(FeedHex(&state, "01 04 00 01 02 00") == SDL_ERGODEX_PACKET_STATUS);
    CHECK(state.buttons == BIT(SDL_ERGODEX_BUTTON_TOP) && !state.have_device);
    now += SDL_ERGODEX_PACE_NS;
    CHECK(Step(&state, now, 16, out) && IsOutput(out, "02 00 00 01 00 01 01 01"));
    CHECK(FeedHex(&state, "0A 05 28 95 01 31 97 07 39 01 01 00 00 00 00 00") == SDL_ERGODEX_PACKET_DEVICE);
    CHECK(state.have_device && state.serial == 0x3907973101952805ULL);
    CHECK(state.buttons == BIT(SDL_ERGODEX_BUTTON_TOP));
    CHECK(FeedHex(&state, "01 04 00 01 03 00") == SDL_ERGODEX_PACKET_STATUS && state.buttons == 0);
    CHECK(state.have_device && state.serial == 0x3907973101952805ULL);
}

/* The replay packets the length tests cut */
static const char *const replay[] = {
    "01 04 00 01 02 00",
    "01 04 00 01 01 00",
    "02 01 01 01 00 00",
    "03 01 01 15 00 00",
    "03 01 01 01 02 03 04 05 06",
    "0A 05 28 95 01 31 97 07 39 01 01 00 00 00 00 00",
};

/* 8: every truncation below 16 bytes and every longer read changes nothing,
 * with stale bytes past the length that would change the state */
static void TestLengths(void)
{
    size_t r, n;

    for (r = 0; r < sizeof(replay) / sizeof(replay[0]); ++r) {
        const Packet p = Make(replay[r]);
        uint8_t stale[64];

        for (n = 0; n < SDL_ERGODEX_PACKET_LENGTH; ++n) {
            SDL_ErgodexState state, before;
            const Packet fill = Make("03 01 01 30 31 32");

            SDL_Ergodex_Init(&state, 0);
            CHECK(FeedHex(&state, "03 01 01 09 00") == SDL_ERGODEX_PACKET_KEYS);
            before = state;
            memcpy(stale, fill.bytes, 16);
            memset(stale + 16, 0x01, sizeof(stale) - 16);
            memcpy(stale, p.bytes, n);
            if (n == 0) {
                stale[0] = 0x0A; /* A DEVICE type in the stale byte */
            }
            CHECK(SDL_Ergodex_HandlePacket(&state, stale, n) == SDL_ERGODEX_PACKET_IGNORED);
            CHECK(Same(&state, &before));
            CHECK(Feed(&state, p.bytes, n) == SDL_ERGODEX_PACKET_IGNORED);
            CHECK(Same(&state, &before));
        }
        for (n = SDL_ERGODEX_PACKET_LENGTH + 1; n <= sizeof(stale); ++n) {
            SDL_ErgodexState state, before;

            SDL_Ergodex_Init(&state, 0);
            before = state;
            memset(stale, 0, sizeof(stale));
            memcpy(stale, p.bytes, 16);
            CHECK(Feed(&state, stale, n) == SDL_ERGODEX_PACKET_IGNORED && Same(&state, &before));
        }
    }
    {
        SDL_ErgodexState state, before;
        const Packet status = Make("01 04 00 01 02");

        SDL_Ergodex_Init(&state, 0);
        before = state;
        CHECK(SDL_Ergodex_HandlePacket(&state, NULL, 16) == SDL_ERGODEX_PACKET_IGNORED && Same(&state, &before));
        CHECK(SDL_Ergodex_HandlePacket(NULL, status.bytes, 16) == SDL_ERGODEX_PACKET_IGNORED);
    }
}

/* A packet split across two reads is two short transfers: neither acts, and
 * the next whole packet decodes */
static void TestSplits(void)
{
    size_t r, cut;

    for (r = 0; r < sizeof(replay) / sizeof(replay[0]); ++r) {
        const Packet p = Make(replay[r]);

        for (cut = 1; cut < SDL_ERGODEX_PACKET_LENGTH; ++cut) {
            SDL_ErgodexState state, before, whole;

            SDL_Ergodex_Init(&state, 0);
            before = state;
            CHECK(Feed(&state, p.bytes, cut) == SDL_ERGODEX_PACKET_IGNORED);
            CHECK(Feed(&state, p.bytes + cut, SDL_ERGODEX_PACKET_LENGTH - cut) == SDL_ERGODEX_PACKET_IGNORED);
            CHECK(Same(&state, &before));
            SDL_Ergodex_Init(&whole, 0);
            CHECK(Feed(&whole, p.bytes, 16) != SDL_ERGODEX_PACKET_IGNORED);
            CHECK(Feed(&state, p.bytes, 16) != SDL_ERGODEX_PACKET_IGNORED && Same(&state, &whole));
        }
    }
}

/* 9: a reconnect forgets everything and runs the whole start-up again, since
 * the pad forgot its keys at power-off */
static void TestReconnect(void)
{
    SDL_ErgodexState state;
    uint8_t out[16];
    uint64_t now = 0;
    int i;

    SDL_Ergodex_Init(&state, now);
    for (i = 0; i < SDL_ERGODEX_STARTUP_OUTPUTS; ++i) {
        CHECK(Step(&state, now, 16, out));
        now += SDL_ERGODEX_PACE_NS;
    }
    CHECK(FeedHex(&state, "0A 05 28 95 01 31 97 07 39 01 01 00 00 00 00 00") == SDL_ERGODEX_PACKET_DEVICE);
    CHECK(FeedHex(&state, "03 01 01 05 06") == SDL_ERGODEX_PACKET_KEYS);
    CHECK(FeedHex(&state, "01 04 00 01 00 00") == SDL_ERGODEX_PACKET_STATUS);
    CHECK(!SDL_Ergodex_NextOutput(&state, now + 1000 * MS, out));

    now += 2000 * MS;
    SDL_Ergodex_Init(&state, now);
    CHECK(state.buttons == 0 && !state.have_device && state.step == 0 && !state.stopped);
    for (i = 0; i < SDL_ERGODEX_STARTUP_OUTPUTS; ++i) {
        uint8_t expected[16];

        CHECK(SDL_Ergodex_StartupOutput(i, expected));
        CHECK(Step(&state, now, 16, out) && memcmp(out, expected, 16) == 0);
        now += SDL_ERGODEX_PACE_NS;
    }
    CHECK(state.step == SDL_ERGODEX_STARTUP_OUTPUTS);
    SDL_Ergodex_Init(NULL, 0);
}

static void TestLayout(void)
{
    CHECK(SDL_ERGODEX_BUTTONS == 52 && SDL_ERGODEX_BUTTON_TOP == 50 && SDL_ERGODEX_BUTTON_LOWER == 51);
    CHECK(SDL_ERGODEX_PACKET_LENGTH == 16 && SDL_ERGODEX_KEYS == 50 && SDL_ERGODEX_STARTUP_OUTPUTS == 13);
    CHECK(SDL_ERGODEX_PACE_NS == 10 * MS && SDL_ERGODEX_RETRY_NS == 50 * MS);
}

/* The pad's two interfaces as ergodex-dx1-linux's docs/HARDWARE.md:34-84
 * records them. Interface 0 carries a HID class descriptor for its 43-byte
 * report descriptor. Its bcdHID is not recorded, and the walk skips the
 * descriptor either way. */
static const uint8_t dx1_interface0[] = {
    9, 4, 0, 0, 1, 3, 0, 1, 0,          /* HID, subclass 0, protocol 1 (keyboard) */
    9, 0x21, 0x10, 0x01, 0x00, 0x01, 0x22, 43, 0,
    7, 5, 0x81, 3, 8, 0, 1,             /* interrupt IN 0x81, 8 bytes */
};
static const uint8_t dx1_interface1[] = {
    9, 4, 1, 0, 2, 0xFF, 0, 1, 0,       /* class FF, subclass 00, protocol 01 */
    7, 5, 0x82, 3, 16, 0, 1,            /* interrupt IN 0x82, 16 bytes */
    7, 5, 0x02, 3, 16, 0, 1,            /* interrupt OUT 0x02, 16 bytes */
};

static bool Select(const SDL_VendorUSBRule *rule, uint8_t interface_number, const uint8_t *bytes, size_t length,
                   SDL_VendorUSBSelection *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool ok;

    if (!copy) {
        ++failures;
        return false;
    }
    if (length) {
        memcpy(copy, bytes, length);
    }
    ok = SDL_VendorUSB_SelectEndpoints(rule, interface_number, copy, length, out);
    free(copy);
    return ok;
}

/* What the libusb enumeration and the platform backend decide for one
 * interface, as should_enumerate_interface in hid.c and HIDAPI_ShouldIgnoreDevice
 * in SDL_hidapi.c compute it */
static bool OnLibUSB(SDL_VendorUSBPlatform platform, uint8_t number, uint8_t cls, uint8_t subclass, uint8_t protocol,
                     bool whitelist)
{
    SDL_VendorUSBRouting r;

    r.platform = platform;
    r.libusb = true;
    r.whitelist = whitelist;
    r.gamecube = true;
    r.vendor = USB_VENDOR_ERGODEX;
    r.product = USB_PRODUCT_ERGODEX_DX1;
    r.xbox = false;
    r.vendor_interface = SDL_VendorUSB_RuleApplies(SDL_VendorUSB_FindRule(r.vendor, r.product, number, cls, subclass, protocol), platform);
    return !SDL_VendorUSB_Ignore(&r) &&
           SDL_VendorUSB_IsCandidate(platform, r.vendor, r.product, number, cls, subclass, protocol, false);
}

static bool PlatformIgnores(SDL_VendorUSBPlatform platform)
{
    SDL_VendorUSBRouting r;

    r.platform = platform;
    r.libusb = false;
    r.whitelist = true;
    r.gamecube = true;
    r.vendor = USB_VENDOR_ERGODEX;
    r.product = USB_PRODUCT_ERGODEX_DX1;
    r.xbox = false;
    r.vendor_interface = false;
    return SDL_VendorUSB_Ignore(&r);
}

static void TestVendorRule(void)
{
    const SDL_VendorUSBPlatform platforms[] = {
        SDL_VENDORUSB_PLATFORM_WINDOWS, SDL_VENDORUSB_PLATFORM_OTHER, SDL_VENDORUSB_PLATFORM_MACOS
    };
    const SDL_VendorUSBRule *rule = SDL_VendorUSB_FindRule(USB_VENDOR_ERGODEX, USB_PRODUCT_ERGODEX_DX1, 1, 0xFF, 0x00, 0x01);
    SDL_VendorUSBSelection selection;
    size_t n, p;

    CHECK(USB_VENDOR_ERGODEX == 0x1603 && USB_PRODUCT_ERGODEX_DX1 == 0x0002);
    CHECK(rule != NULL);
    CHECK(SDL_VendorUSB_IsVendorDevice(USB_VENDOR_ERGODEX, USB_PRODUCT_ERGODEX_DX1));
    /* The rule names interface 1 by number, whatever its class */
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_ERGODEX, USB_PRODUCT_ERGODEX_DX1, 1, 0x03, 0x00, 0x00) == rule);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_ERGODEX, USB_PRODUCT_ERGODEX_DX1, 0, 0x03, 0x00, 0x01) == NULL);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_ERGODEX, USB_PRODUCT_ERGODEX_DX1, 0, 0xFF, 0x00, 0x01) == NULL);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_ERGODEX, 0x0003, 1, 0xFF, 0x00, 0x01) == NULL);
    if (!rule) {
        return;
    }
    /* Byte 0 of every request is data, so hid_write sends it unchanged */
    CHECK((rule->flags & SDL_VENDORUSB_RAW_OUTPUT) && (rule->flags & SDL_VENDORUSB_WINDOWS_ONLY));
    CHECK(!(rule->flags & SDL_VENDORUSB_MATCH_CLASS) && rule->interface_number == 1 && rule->alternate == 0);

    /* Interface 1 as hid.c lays it out: only the whole of it selects */
    for (n = 0; n <= sizeof(dx1_interface1); ++n) {
        bool ok;

        memset(&selection, 0, sizeof(selection));
        ok = Select(rule, 1, dx1_interface1, n, &selection);
        CHECK(ok == (n == sizeof(dx1_interface1)));
        if (ok) {
            CHECK(selection.alternate == 0);
            CHECK(selection.in.address == 0x82 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT);
            CHECK(selection.in.max_packet_size == 16 && selection.in.read_size == 16);
            CHECK(selection.out.address == 0x02 && selection.out.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT);
            CHECK(selection.out.max_packet_size == 16);
        }
    }
    /* In the whole configuration the walk takes interface 1's endpoints and
     * never the keyboard's 0x81 */
    {
        uint8_t both[sizeof(dx1_interface0) + sizeof(dx1_interface1)];

        memcpy(both, dx1_interface0, sizeof(dx1_interface0));
        memcpy(both + sizeof(dx1_interface0), dx1_interface1, sizeof(dx1_interface1));
        memset(&selection, 0, sizeof(selection));
        CHECK(Select(rule, 1, both, sizeof(both), &selection));
        CHECK(selection.in.address == 0x82 && selection.out.address == 0x02);
        CHECK(!Select(rule, 0, both, sizeof(both), &selection));
        CHECK(!Select(rule, 0, dx1_interface0, sizeof(dx1_interface0), &selection));
    }
    /* Other packet sizes, or a missing OUT endpoint, are not the pad the rule describes */
    {
        uint8_t other[sizeof(dx1_interface1)];

        memcpy(other, dx1_interface1, sizeof(other));
        other[13] = 8;
        CHECK(!Select(rule, 1, other, sizeof(other), &selection));
        memcpy(other, dx1_interface1, sizeof(other));
        other[20] = 64;
        CHECK(!Select(rule, 1, other, sizeof(other), &selection));
        CHECK(!Select(rule, 1, dx1_interface1, 16, &selection));
    }

    /* On Windows libusb takes interface 1, whatever the whitelist, and never
     * interface 0, which the system's keyboard driver keeps, and the
     * platform HID backend leaves the pad. Like every rule of Part 14 the
     * rule serves Windows only, so elsewhere nothing changes: the vendor
     * interface stays out, and without the whitelist libusb takes the
     * keyboard as any HID interface. */
    for (p = 0; p < sizeof(platforms) / sizeof(platforms[0]); ++p) {
        const bool windows = (platforms[p] == SDL_VENDORUSB_PLATFORM_WINDOWS);

        CHECK(SDL_VendorUSB_IsCandidate(platforms[p], USB_VENDOR_ERGODEX, USB_PRODUCT_ERGODEX_DX1, 1, 0xFF, 0x00, 0x01, false) == windows);
        CHECK(SDL_VendorUSB_IsCandidate(platforms[p], USB_VENDOR_ERGODEX, USB_PRODUCT_ERGODEX_DX1, 0, 0x03, 0x00, 0x01, false) == !windows);
        CHECK(OnLibUSB(platforms[p], 1, 0xFF, 0x00, 0x01, true) == windows);
        CHECK(OnLibUSB(platforms[p], 1, 0xFF, 0x00, 0x01, false) == windows);
        CHECK(!OnLibUSB(platforms[p], 0, 0x03, 0x00, 0x01, true));
        CHECK(OnLibUSB(platforms[p], 0, 0x03, 0x00, 0x01, false) == !windows);
        CHECK(PlatformIgnores(platforms[p]) == windows);
        CHECK(!SDL_VendorUSB_RequiresLibUSB(platforms[p], USB_VENDOR_ERGODEX, USB_PRODUCT_ERGODEX_DX1, false, false));
    }
}

int main(void)
{
    TestStatus();
    TestProbeCapture();
    TestKeys();
    TestDevice();
    TestSnapshots();
    TestIgnoredTypes();
    TestStartup();
    TestStartupFailures();
    TestStatusBeforeDevice();
    TestLengths();
    TestSplits();
    TestReconnect();
    TestLayout();
    TestVendorRule();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
