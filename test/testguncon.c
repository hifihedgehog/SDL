/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_guncon_proto.c, the Namco
   light guns of hifihedgehog/SDL#33 Part 9. Test numbers follow the part.
   The GunCon 2 reports are constructed from the bit tables of beardypig's
   driver and PCSX2. The GunCon 3 checksum test replays the three hardware
   packets printed in beardypig's blog. GunCon 3 test 3, the decryption,
   waits on the table question and does not run. */

#include "../src/joystick/hidapi/SDL_hidapi_guncon_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(condition)                                                      \
    do {                                                                      \
        ++checks;                                                             \
        if (!(condition)) {                                                   \
            ++failures;                                                       \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
        }                                                                     \
    } while (0)

#define MAX_WRITES 8

typedef struct TestUnit
{
    int writes;
    uint8_t data[MAX_WRITES][16];
    size_t length[MAX_WRITES];
} TestUnit;

static bool TestWrite(void *userdata, const uint8_t *data, size_t length)
{
    TestUnit *unit = (TestUnit *)userdata;

    CHECK(length <= 16);
    if (unit->writes < MAX_WRITES && length <= 16) {
        memcpy(unit->data[unit->writes], data, length);
        unit->length[unit->writes] = length;
    }
    ++unit->writes;
    return true;
}

static SDL_GunConSink Sink(TestUnit *unit)
{
    SDL_GunConSink sink;

    memset(unit, 0, sizeof(*unit));
    sink.userdata = unit;
    sink.write = TestWrite;
    return sink;
}

/* The control transfer the libusb backend's hid_write sends for a device
   with no OUT endpoint (src/hidapi/libusb/hid.c, hid_write): a leading
   report number 0 is stripped, and SET_REPORT goes to the interface with
   the report type output and that number in wValue. */
static void HidWriteSetup(const uint8_t *data, size_t length, uint8_t interface_number, uint8_t setup[8],
                          const uint8_t **payload, size_t *payload_length)
{
    const uint8_t report_number = data[0];

    if (report_number == 0) {
        ++data;
        --length;
    }
    setup[0] = 0x21; /* Class, interface, host to device */
    setup[1] = 0x09; /* SET_REPORT */
    setup[2] = report_number;
    setup[3] = 0x02; /* Output */
    setup[4] = interface_number;
    setup[5] = 0x00;
    setup[6] = (uint8_t)(length & 0xFF);
    setup[7] = (uint8_t)(length >> 8);
    *payload = data;
    *payload_length = length;
}

/* Decodes from an exact-size heap copy, so a read past the length reaches
   the sanitizer */
static bool Decode2(const uint8_t *report, size_t length, SDL_GunCon2State *state)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    result = SDL_GunCon2_Decode(copy, length, state);
    free(copy);
    return result;
}

static bool Idle2(const SDL_GunCon2State *state)
{
    return state->buttons == 0 && state->hat == 0 && state->axes[0] == 0 && state->axes[1] == 0;
}

/* 1 */
static void TestGunCon2Open(void)
{
    static const uint8_t mode[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    static const uint8_t setup_expected[8] = { 0x21, 0x09, 0x00, 0x02, 0x00, 0x00, 0x06, 0x00 };
    static const uint8_t payload_expected[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    TestUnit unit;
    SDL_GunConSink sink = Sink(&unit);
    uint8_t request[SDL_GUNCON2_MODE_LENGTH];
    uint8_t setup[8];
    const uint8_t *payload;
    size_t payload_length;

    SDL_GunCon2_ModeRequest(request);
    CHECK(memcmp(request, mode, sizeof(mode)) == 0);

    /* One write per open, the 7-byte buffer */
    CHECK(SDL_GunCon2_Open(&sink));
    CHECK(unit.writes == 1 && unit.length[0] == 7 && memcmp(unit.data[0], mode, 7) == 0);
    HidWriteSetup(unit.data[0], unit.length[0], 0, setup, &payload, &payload_length);
    CHECK(memcmp(setup, setup_expected, 8) == 0);
    CHECK(payload_length == 6 && memcmp(payload, payload_expected, 6) == 0);

    /* A reconnect sends it again, exactly once */
    CHECK(SDL_GunCon2_Open(&sink));
    CHECK(unit.writes == 2 && unit.length[1] == 7 && memcmp(unit.data[1], mode, 7) == 0);

    /* No sink, no write */
    CHECK(!SDL_GunCon2_Open(NULL));
}

/* 2, 3 and 6 */
static void TestGunCon2Controls(void)
{
    static const struct
    {
        uint8_t byte0, byte1;
        uint8_t buttons;
        uint8_t hat;
    } single[] = {
        { 0xFD, 0xFF, 1u << 3, 0 },                      /* C */
        { 0xFB, 0xFF, 1u << 2, 0 },                      /* B */
        { 0xF7, 0xFF, 1u << 1, 0 },                      /* A */
        { 0xEF, 0xFF, 0, SDL_GUNCON_HAT_UP },
        { 0xDF, 0xFF, 0, SDL_GUNCON_HAT_RIGHT },
        { 0xBF, 0xFF, 0, SDL_GUNCON_HAT_DOWN },
        { 0x7F, 0xFF, 0, SDL_GUNCON_HAT_LEFT },
        { 0xFF, 0xDF, 1u << 0, 0 },                      /* Trigger */
        { 0xFF, 0xBF, 1u << 5, 0 },                      /* Select */
        { 0xFF, 0x7F, 1u << 4, 0 },                      /* Start */
    };
    SDL_GunCon2State state;
    uint8_t report[6];
    size_t i;

    SDL_GunCon2_ResetState(&state);
    CHECK(Idle2(&state));
    CHECK(Decode2((const uint8_t *)"\xFF\xFF\x00\x00\x00\x00", 6, &state) && Idle2(&state));

    for (i = 0; i < sizeof(single) / sizeof(single[0]); ++i) {
        memcpy(report, "\xFF\xFF\x00\x00\x00\x00", 6);
        report[0] = single[i].byte0;
        report[1] = single[i].byte1;
        SDL_GunCon2_ResetState(&state);
        CHECK(Decode2(report, 6, &state));
        CHECK(state.buttons == single[i].buttons && state.hat == single[i].hat);
        CHECK(state.axes[0] == 0 && state.axes[1] == 0);
    }

    /* 6: the unused bits press nothing */
    CHECK(Decode2((const uint8_t *)"\xFE\xE0\x00\x00\x00\x00", 6, &state) && Idle2(&state));
}

/* An independent reading of the part's table, for every button word */
static void TestGunCon2AllWords(void)
{
    static const struct
    {
        int byte;
        int bit;
        int button; /* -1 for a hat direction */
        uint8_t hat;
    } table[] = {
        { 0, 1, 3, 0 }, { 0, 2, 2, 0 }, { 0, 3, 1, 0 },
        { 0, 4, -1, SDL_GUNCON_HAT_UP }, { 0, 5, -1, SDL_GUNCON_HAT_RIGHT },
        { 0, 6, -1, SDL_GUNCON_HAT_DOWN }, { 0, 7, -1, SDL_GUNCON_HAT_LEFT },
        { 1, 5, 0, 0 }, { 1, 6, 5, 0 }, { 1, 7, 4, 0 },
    };
    SDL_GunCon2State state;
    uint8_t report[6] = { 0, 0, 0x34, 0x12, 0x56, 0x00 };
    int word, mismatches = 0;

    for (word = 0; word < 0x10000; ++word) {
        uint8_t buttons = 0, held = 0, hat = 0;
        size_t k;

        report[0] = (uint8_t)(word & 0xFF);
        report[1] = (uint8_t)(word >> 8);
        for (k = 0; k < sizeof(table) / sizeof(table[0]); ++k) {
            if (!(report[table[k].byte] & (1u << table[k].bit))) {
                if (table[k].button >= 0) {
                    buttons |= (uint8_t)(1u << table[k].button);
                } else {
                    held |= table[k].hat;
                }
            }
        }
        if ((held & (SDL_GUNCON_HAT_UP | SDL_GUNCON_HAT_DOWN)) != (SDL_GUNCON_HAT_UP | SDL_GUNCON_HAT_DOWN)) {
            hat |= held & (SDL_GUNCON_HAT_UP | SDL_GUNCON_HAT_DOWN);
        }
        if ((held & (SDL_GUNCON_HAT_LEFT | SDL_GUNCON_HAT_RIGHT)) != (SDL_GUNCON_HAT_LEFT | SDL_GUNCON_HAT_RIGHT)) {
            hat |= held & (SDL_GUNCON_HAT_LEFT | SDL_GUNCON_HAT_RIGHT);
        }
        if (!SDL_GunCon2_Decode(report, 6, &state) || state.buttons != buttons || state.hat != hat ||
            state.axes[0] != 0x1234 || state.axes[1] != 0x0056) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

/* 4 and 5 */
static void TestGunCon2HatAndAim(void)
{
    SDL_GunCon2State state;

    CHECK(Decode2((const uint8_t *)"\xCF\xFF\x00\x00\x00\x00", 6, &state));
    CHECK(state.hat == (SDL_GUNCON_HAT_UP | SDL_GUNCON_HAT_RIGHT) && state.buttons == 0);
    CHECK(Decode2((const uint8_t *)"\xAF\xFF\x00\x00\x00\x00", 6, &state));
    CHECK(state.hat == 0 && state.buttons == 0);
    /* Left and right together cancel as well, and each axis on its own */
    CHECK(Decode2((const uint8_t *)"\x5F\xFF\x00\x00\x00\x00", 6, &state) && state.hat == 0);
    CHECK(Decode2((const uint8_t *)"\x0F\xFF\x00\x00\x00\x00", 6, &state) && state.hat == 0);
    CHECK(Decode2((const uint8_t *)"\x4F\xFF\x00\x00\x00\x00", 6, &state) && state.hat == SDL_GUNCON_HAT_UP);

    CHECK(Decode2((const uint8_t *)"\xFF\xFF\xAF\x01\x78\x00", 6, &state));
    CHECK(state.axes[0] == 431 && state.axes[1] == 120 && state.buttons == 0 && state.hat == 0);
    CHECK(Decode2((const uint8_t *)"\xFF\xFF\xFF\xFF\xFF\xFF", 6, &state));
    CHECK(state.axes[0] == 32767 && state.axes[1] == 32767);
    /* The clamp starts above 32767, and byte 5 counts */
    CHECK(Decode2((const uint8_t *)"\xFF\xFF\xFF\x7F\x00\x80", 6, &state));
    CHECK(state.axes[0] == 32767 && state.axes[1] == 32767);
    CHECK(Decode2((const uint8_t *)"\xFF\xFF\xFE\x7F\x00\x01", 6, &state));
    CHECK(state.axes[0] == 32766 && state.axes[1] == 256);
}

/* 7 */
static void TestGunCon2Lengths(void)
{
    static const uint8_t report[8] = { 0xF7, 0xDF, 0x2C, 0x01, 0x64, 0x00, 0x00, 0x00 };
    SDL_GunCon2State state, before;
    uint8_t stale[64];
    size_t n;

    /* Every read shorter than 6 bytes, with stale bytes past it */
    for (n = 0; n < 6; ++n) {
        SDL_GunCon2_ResetState(&state);
        CHECK(Decode2((const uint8_t *)"\x00\x00\xFF\xFF\xFF\xFF", 6, &state));
        before = state;
        memset(stale, 0x00, sizeof(stale));
        memcpy(stale, report, n);
        CHECK(!SDL_GunCon2_Decode(stale, n, &state));
        CHECK(memcmp(&state, &before, sizeof(state)) == 0);
        CHECK(!Decode2(report, n, &state));
        CHECK(memcmp(&state, &before, sizeof(state)) == 0);
    }
    CHECK(!SDL_GunCon2_Decode(NULL, 6, &state) && memcmp(&state, &before, sizeof(state)) == 0);

    /* 7 and 8 bytes decode their first 6 */
    for (n = 6; n <= 8; ++n) {
        SDL_GunCon2_ResetState(&state);
        memcpy(stale, report, sizeof(report));
        stale[6] = 0x00;
        stale[7] = 0x00;
        CHECK(Decode2(stale, n, &state));
        CHECK(state.buttons == ((1u << 0) | (1u << 1)) && state.hat == 0);
        CHECK(state.axes[0] == 300 && state.axes[1] == 100);
    }
}

/* The three packets beardypig's blog printed from a GunCon 3 */
static const uint8_t capture[3][15] = {
    { 0x56, 0xC8, 0x30, 0x71, 0x97, 0x4B, 0x3F, 0xF4, 0x27, 0x46, 0xF1, 0x40, 0x98, 0xEF, 0xF4 },
    { 0xBE, 0xB4, 0xCC, 0x4C, 0x53, 0x9B, 0xD8, 0xEE, 0x5B, 0x72, 0xFC, 0x7D, 0x6C, 0xE7, 0xF3 },
    { 0x56, 0xC7, 0x30, 0x71, 0x97, 0x4B, 0x3F, 0xF4, 0x27, 0x46, 0xF1, 0x40, 0x7D, 0x29, 0xF4 },
};

/* 1 */
static void TestGunCon3Open(void)
{
    static const uint8_t key[8] = { 0x01, 0x12, 0x6F, 0x32, 0x24, 0x60, 0x17, 0x21 };
    SDL_GunCon3Session session;
    TestUnit unit;
    SDL_GunConSink sink = Sink(&unit);
    uint8_t setup[8];
    const uint8_t *payload;
    size_t payload_length;

    CHECK(memcmp(SDL_GunCon3_Key, key, 8) == 0);
    session.failures = 5;
    CHECK(SDL_GunCon3_Open(&session, &sink));
    CHECK(unit.writes == 1 && unit.length[0] == 8 && memcmp(unit.data[0], key, 8) == 0);
    CHECK(session.failures == 0);
    /* The first byte, 01, is data: hid_write strips only a report number of 0 */
    HidWriteSetup(unit.data[0], unit.length[0], 0, setup, &payload, &payload_length);
    CHECK(payload_length == 8 && payload[0] == 0x01);
    /* A reconnect writes it again */
    CHECK(SDL_GunCon3_Open(&session, &sink));
    CHECK(unit.writes == 2 && memcmp(unit.data[1], key, 8) == 0);
}

/* 2 */
static void TestGunCon3Checksum(void)
{
    uint8_t key22[8];
    uint8_t packet[15];
    int p, byte, bit;

    memcpy(key22, SDL_GunCon3_Key, 8);
    key22[7] = 0x22;
    for (p = 0; p < 3; ++p) {
        int flipped_fail = 0, flipped = 0;

        CHECK(SDL_GunCon3_Checksum(capture[p], 0x21) == capture[p][13]);
        CHECK(SDL_GunCon3_ChecksumValid(capture[p], 15, SDL_GunCon3_Key));
        CHECK(!SDL_GunCon3_ChecksumValid(capture[p], 15, key22));
        for (byte = 0; byte < 14; ++byte) {
            for (bit = 0; bit < 8; ++bit) {
                memcpy(packet, capture[p], 15);
                packet[byte] ^= (uint8_t)(1u << bit);
                ++flipped;
                flipped_fail += !SDL_GunCon3_ChecksumValid(packet, 15, SDL_GunCon3_Key);
            }
        }
        CHECK(flipped == 112 && flipped_fail == 112);
        for (bit = 0; bit < 8; ++bit) {
            memcpy(packet, capture[p], 15);
            packet[14] ^= (uint8_t)(1u << bit);
            CHECK(SDL_GunCon3_ChecksumValid(packet, 15, SDL_GunCon3_Key));
        }
    }
    /* Each step of the sum, one at a time */
    {
        uint8_t zero[13];

        memset(zero, 0, sizeof(zero));
        CHECK(SDL_GunCon3_Checksum(zero, 0x5A) == 0x5A);
        zero[0] = 1;
        CHECK(SDL_GunCon3_Checksum(zero, 0x10) == 0x11);
        zero[0] = 0;
        zero[1] = 1;
        CHECK(SDL_GunCon3_Checksum(zero, 0x10) == 0x0F);
        zero[1] = 0;
        zero[2] = 0x20;
        CHECK(SDL_GunCon3_Checksum(zero, 0x10) == 0xF0);
        zero[2] = 0;
        zero[3] = 0x0F;
        CHECK(SDL_GunCon3_Checksum(zero, 0x10) == 0x1F);
        zero[3] = 0;
        zero[4] = 0x01;
        zero[5] = 0x02;
        CHECK(SDL_GunCon3_Checksum(zero, 0x10) == 0x13);
        zero[4] = 0;
        zero[5] = 0;
        zero[6] = 0x01;
        zero[7] = 0x03;
        CHECK(SDL_GunCon3_Checksum(zero, 0x10) == 0x12);
        zero[6] = 0;
        zero[7] = 0;
        zero[8] = 0x01;
        zero[9] = 0x02;
        zero[10] = 0x04;
        zero[11] = 0x08;
        CHECK(SDL_GunCon3_Checksum(zero, 0x10) == 0x07);
        zero[8] = 0;
        zero[9] = 0;
        zero[10] = 0;
        zero[11] = 0;
        zero[12] = 0xFF;
        CHECK(SDL_GunCon3_Checksum(zero, 0x10) == 0xEF);
    }
}

/* 4 */
static void TestGunCon3Layout(void)
{
    static const struct
    {
        int byte;
        uint8_t mask;
        int button;
    } bits[] = {
        { 1, 0x20, 0 }, { 0, 0x04, 1 }, { 0, 0x02, 2 }, { 2, 0x80, 3 }, { 1, 0x04, 4 }, { 1, 0x02, 5 },
        { 2, 0x40, 6 }, { 1, 0x80, 7 }, { 0, 0x08, 8 }, { 1, 0x08, 9 }, { 1, 0x10, 10 },
    };
    SDL_GunCon3State state;
    uint8_t plain[13];
    size_t i;
    int byte, bit;

    /* Rest: sticks near 7F, aim 0 */
    memset(plain, 0, sizeof(plain));
    plain[9] = plain[10] = plain[11] = plain[12] = 0x7F;
    SDL_GunCon3_DecodePlain(plain, &state);
    CHECK(state.buttons == 0);
    CHECK(state.axes[0] == 0 && state.axes[1] == 0 && state.axes[2] == 0);
    CHECK(state.axes[3] == -129 && state.axes[4] == -129 && state.axes[5] == -129 && state.axes[6] == -129);

    /* Each button bit alone */
    for (i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        memset(plain, 0, sizeof(plain));
        plain[bits[i].byte] = bits[i].mask;
        SDL_GunCon3_DecodePlain(plain, &state);
        CHECK(state.buttons == (1u << bits[i].button));
    }
    /* No other bit of bytes 0 to 2 presses anything */
    for (byte = 0; byte < 3; ++byte) {
        for (bit = 0; bit < 8; ++bit) {
            bool mapped = false;

            for (i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
                mapped = mapped || (bits[i].byte == byte && bits[i].mask == (1u << bit));
            }
            if (!mapped) {
                memset(plain, 0, sizeof(plain));
                plain[byte] = (uint8_t)(1u << bit);
                SDL_GunCon3_DecodePlain(plain, &state);
                CHECK(state.buttons == 0);
            }
        }
    }

    /* Aim */
    memset(plain, 0, sizeof(plain));
    plain[3] = 0xFF;
    plain[4] = 0x7F;
    plain[5] = 0xFF;
    plain[6] = 0x7F;
    plain[7] = 0x34;
    plain[8] = 0x92;
    SDL_GunCon3_DecodePlain(plain, &state);
    CHECK(state.axes[0] == 32767 && state.axes[1] == -32767 && state.axes[2] == -28108); /* 0x9234 */
    plain[3] = 0x00;
    plain[4] = 0x80;
    plain[5] = 0x00;
    plain[6] = 0x80;
    SDL_GunCon3_DecodePlain(plain, &state);
    CHECK(state.axes[0] == -32768 && state.axes[1] == 32767);
    plain[5] = 0x01;
    plain[6] = 0x00;
    SDL_GunCon3_DecodePlain(plain, &state);
    CHECK(state.axes[1] == -1);

    /* Sticks: A in bytes 11 and 12, B in 9 and 10 */
    {
        static const uint8_t raw[4] = { 0x00, 0x7F, 0x80, 0xFF };
        static const int16_t scaled[4] = { -32768, -129, 128, 32767 };

        for (i = 0; i < 4; ++i) {
            memset(plain, 0, sizeof(plain));
            plain[11] = raw[i];
            SDL_GunCon3_DecodePlain(plain, &state);
            CHECK(state.axes[3] == scaled[i] && state.axes[4] == -32768 && state.axes[5] == -32768 && state.axes[6] == -32768);
            memset(plain, 0, sizeof(plain));
            plain[12] = raw[i];
            SDL_GunCon3_DecodePlain(plain, &state);
            CHECK(state.axes[4] == scaled[i] && state.axes[3] == -32768);
            memset(plain, 0, sizeof(plain));
            plain[9] = raw[i];
            SDL_GunCon3_DecodePlain(plain, &state);
            CHECK(state.axes[5] == scaled[i] && state.axes[6] == -32768);
            memset(plain, 0, sizeof(plain));
            plain[10] = raw[i];
            SDL_GunCon3_DecodePlain(plain, &state);
            CHECK(state.axes[6] == scaled[i] && state.axes[5] == -32768);
        }
    }
}

/* 5 and 6 */
static void TestGunCon3Session(void)
{
    SDL_GunCon3Session session;
    TestUnit unit;
    SDL_GunConSink sink = Sink(&unit);
    uint8_t bad[15], stale[32];
    size_t n;

    CHECK(SDL_GunCon3_Open(&session, &sink) && unit.writes == 1);
    /* 6: no report and no clock, so nothing more goes out */
    CHECK(unit.writes == 1 && session.failures == 0);

    /* Reads shorter than 15 bytes change nothing, stale bytes or not */
    for (n = 0; n < 15; ++n) {
        memset(stale, 0, sizeof(stale));
        memcpy(stale, capture[0], 15);
        CHECK(SDL_GunCon3_HandleReport(&session, stale, n, &sink) == SDL_GUNCON3_REPORT_SHORT);
        CHECK(session.failures == 0 && unit.writes == 1);
    }
    CHECK(!SDL_GunCon3_ChecksumValid(capture[0], 14, SDL_GunCon3_Key));
    CHECK(SDL_GunCon3_HandleReport(&session, NULL, 15, &sink) == SDL_GUNCON3_REPORT_SHORT);

    /* One failure, then a good packet: no key */
    memcpy(bad, capture[0], 15);
    bad[13] ^= 0x01;
    CHECK(SDL_GunCon3_HandleReport(&session, bad, 15, &sink) == SDL_GUNCON3_REPORT_FAILED);
    CHECK(session.failures == 1 && unit.writes == 1);
    CHECK(SDL_GunCon3_HandleReport(&session, capture[1], 15, &sink) == SDL_GUNCON3_REPORT_VALID);
    CHECK(session.failures == 0 && unit.writes == 1);
    CHECK(SDL_GunCon3_HandleReport(&session, bad, 15, &sink) == SDL_GUNCON3_REPORT_FAILED);
    CHECK(SDL_GunCon3_HandleReport(&session, capture[2], 15, &sink) == SDL_GUNCON3_REPORT_VALID);
    CHECK(unit.writes == 1);

    /* Two in a row: the key again, and the count starts over */
    CHECK(SDL_GunCon3_HandleReport(&session, bad, 15, &sink) == SDL_GUNCON3_REPORT_FAILED);
    CHECK(SDL_GunCon3_HandleReport(&session, bad, 15, &sink) == SDL_GUNCON3_REPORT_FAILED);
    CHECK(unit.writes == 2 && memcmp(unit.data[1], SDL_GunCon3_Key, 8) == 0 && session.failures == 0);
    CHECK(SDL_GunCon3_HandleReport(&session, bad, 15, &sink) == SDL_GUNCON3_REPORT_FAILED);
    CHECK(unit.writes == 2 && session.failures == 1);
    CHECK(SDL_GunCon3_HandleReport(&session, bad, 15, &sink) == SDL_GUNCON3_REPORT_FAILED);
    CHECK(unit.writes == 3 && session.failures == 0);

    /* A 16-byte read with a good first 15 is valid */
    memset(stale, 0xAA, sizeof(stale));
    memcpy(stale, capture[1], 15);
    CHECK(SDL_GunCon3_HandleReport(&session, stale, 16, &sink) == SDL_GUNCON3_REPORT_VALID);
}

int main(void)
{
    TestGunCon2Open();
    TestGunCon2Controls();
    TestGunCon2AllWords();
    TestGunCon2HatAndAim();
    TestGunCon2Lengths();
    TestGunCon3Open();
    TestGunCon3Checksum();
    TestGunCon3Layout();
    TestGunCon3Session();

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
