/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_konami_p4io_proto.c, the
   Konami P4IO of hifihedgehog/SDL#33 Part 14. Test numbers follow the part.
   The requests and replies are constructed from bemanitools (p4io/cmd.h,
   p4iodrv/usb.c, p4iodrv/device.c, p4ioemu/device.c) and arcade-docs, the
   input words from bemanitools' jubeat driver and emulator
   (jbio-p4io/jbio.c, jbhook-util/p4io.c) and p4io-mdxfdrv (src/p4io.h).
   None of them is a capture of a board. The module keeps no clock and no
   bytes from one read to the next, so no test needs either. */

#include "../src/joystick/hidapi/SDL_hidapi_konami_p4io_proto.h"

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

#define ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))

static const int layouts[] = { SDL_KONAMI_P4IO_LAYOUT_RAW, SDL_KONAMI_P4IO_LAYOUT_JUBEAT, SDL_KONAMI_P4IO_LAYOUT_DDR };

/* The GET DEVICE INFO reply of test 1, built from arcade-docs' entry for
   the P4IO firmware: type 0B000000, version 1.1.2, product BMPU, built Jun
   12 2013 19:24:19. The type goes out low byte first here. The test reads
   only product and version, since the type's byte order is not known. */
static void DeviceInfoReply(uint8_t reply[48])
{
    static const uint8_t header[16] = {
        0xAA, 0x01, 0x01, 0x2C, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x01, 0x01, 0x02, 0x42, 0x4D, 0x50, 0x55
    };

    memset(reply, 0, 48);
    memcpy(reply, header, sizeof(header));
    memcpy(&reply[16], "Jun 12 2013", 11);
    memcpy(&reply[32], "19:24:19", 8);
}

/* The reply bemanitools' emulator gives: type 37133713 in host order,
   version 5.7.3, product P4IO, and a 64-byte read whatever the payload */
static void EmulatorReply(uint8_t reply[64], uint8_t sequence)
{
    memset(reply, 0, 64);
    reply[0] = 0xAA;
    reply[1] = 0x01;
    reply[2] = sequence;
    reply[3] = 44;
    reply[4] = 0x13;
    reply[5] = 0x37;
    reply[6] = 0x13;
    reply[7] = 0x37;
    reply[9] = 5;
    reply[10] = 7;
    reply[11] = 3;
    memcpy(&reply[12], "P4IO", 4);
    memcpy(&reply[16], "build_date", 11);
    memcpy(&reply[32], "build_time", 11);
}

/* Runs one call on an exact-size heap copy, so a read past the length
   reaches the sanitizer */
static bool ParseReplyExact(const uint8_t *reply, size_t length, uint8_t sequence,
                            size_t *payload_length, uint8_t *first)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    const uint8_t *payload = NULL;
    size_t declared = 0;
    bool result;

    if (!copy) {
        return false;
    }
    if (length) {
        memcpy(copy, reply, length);
    }
    result = SDL_KonamiP4IO_ParseReply(copy, length, sequence, &payload, &declared);
    if (result) {
        /* The payload lies inside the copy */
        CHECK(payload == copy + SDL_KONAMI_P4IO_HEADER_LENGTH);
        CHECK(SDL_KONAMI_P4IO_HEADER_LENGTH + declared <= length);
        if (first) {
            *first = declared ? payload[0] : 0;
        }
    }
    if (payload_length) {
        *payload_length = declared;
    }
    free(copy);
    return result;
}

static bool DecodeExact(int layout, const uint8_t *report, size_t length, SDL_KonamiP4IOState *state)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (!copy) {
        return false;
    }
    if (length) {
        memcpy(copy, report, length);
    }
    result = SDL_KonamiP4IO_Decode(layout, copy, length, state);
    free(copy);
    return result;
}

static void StartupResultExact(SDL_KonamiP4IOStartup *startup, bool exchanged, const uint8_t *reply, size_t length)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);

    if (!copy) {
        ++failures;
        return;
    }
    if (length) {
        memcpy(copy, reply, length);
    }
    SDL_KonamiP4IO_StartupResult(startup, exchanged, copy, length);
    free(copy);
}

static bool IsZero(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t i;

    for (i = 0; i < length; ++i) {
        if (bytes[i]) {
            return false;
        }
    }
    return true;
}

/* 1: the requests. AA, command, sequence, payload length, payload. */
static void TestRequests(void)
{
    uint8_t out[SDL_KONAMI_P4IO_REQUEST_MAX + 8];
    uint8_t payload[SDL_KONAMI_P4IO_MAX_PAYLOAD + 8];
    size_t n, i;

    CHECK(SDL_KONAMI_P4IO_REQUEST_MAX == 64 && SDL_KONAMI_P4IO_READ_LENGTH == 65);

    memset(out, 0xEE, sizeof(out));
    CHECK(SDL_KonamiP4IO_BuildRequest(SDL_KONAMI_P4IO_CMD_INIT, 0, NULL, 0, out) == 4);
    CHECK(memcmp(out, "\xAA\x00\x00\x00", 4) == 0 && out[4] == 0xEE);
    CHECK(SDL_KonamiP4IO_BuildRequest(SDL_KONAMI_P4IO_CMD_GET_DEVICE_INFO, 1, NULL, 0, out) == 4);
    CHECK(memcmp(out, "\xAA\x01\x01\x00", 4) == 0);
    /* Any command and sequence pass through, among them RESET_WTD 1C */
    CHECK(SDL_KonamiP4IO_BuildRequest(0x1C, 0xFF, NULL, 0, out) == 4);
    CHECK(memcmp(out, "\xAA\x1C\xFF\x00", 4) == 0);

    /* Every payload length up to 60 */
    for (i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(0x80 + i);
    }
    for (n = 0; n <= SDL_KONAMI_P4IO_MAX_PAYLOAD; ++n) {
        memset(out, 0xEE, sizeof(out));
        CHECK(SDL_KonamiP4IO_BuildRequest(0x12, (uint8_t)n, payload, n, out) == 4 + n);
        CHECK(out[0] == 0xAA && out[1] == 0x12 && out[2] == (uint8_t)n && out[3] == (uint8_t)n);
        CHECK(memcmp(&out[4], payload, n) == 0 && out[4 + n] == 0xEE);
    }
    /* A longer payload, or a missing one, is refused and nothing is written */
    for (n = SDL_KONAMI_P4IO_MAX_PAYLOAD + 1; n < sizeof(payload); ++n) {
        memset(out, 0xEE, sizeof(out));
        CHECK(SDL_KonamiP4IO_BuildRequest(0x12, 0, payload, n, out) == 0);
        CHECK(out[0] == 0xEE);
    }
    CHECK(SDL_KonamiP4IO_BuildRequest(0x12, 0, NULL, 1, out) == 0);
    CHECK(SDL_KonamiP4IO_BuildRequest(0x12, 0, NULL, 60, out) == 0);
    CHECK(SDL_KonamiP4IO_BuildRequest(0x12, 0, payload, (size_t)-1, out) == 0);
}

/* 1: the reply header. AA, the request's sequence, and a payload that fits. */
static void TestReplies(void)
{
    uint8_t reply[SDL_KONAMI_P4IO_READ_LENGTH];
    uint8_t info[48];
    size_t length, n;
    const uint8_t *payload;
    uint8_t first;
    int s;

    /* INIT's empty reply, a bare header */
    CHECK(ParseReplyExact((const uint8_t *)"\xAA\x00\x00\x00", 4, 0, &length, NULL) && length == 0);
    /* The same header with another sequence, another start byte, or cut short */
    for (s = 1; s < 256; ++s) {
        CHECK(!ParseReplyExact((const uint8_t *)"\xAA\x00\x00\x00", 4, (uint8_t)s, NULL, NULL));
    }
    CHECK(!ParseReplyExact((const uint8_t *)"\xAB\x00\x00\x00", 4, 0, NULL, NULL));
    CHECK(!ParseReplyExact((const uint8_t *)"\x00\xAA\x00\x00", 4, 0, NULL, NULL));
    for (n = 0; n < 4; ++n) {
        memset(reply, 0, sizeof(reply));
        memcpy(reply, "\xAA\x00\x00\x00", 4);
        CHECK(!SDL_KonamiP4IO_ParseReply(reply, n, 0, NULL, NULL));
        CHECK(!ParseReplyExact(reply, n, 0, NULL, NULL));
    }
    CHECK(!SDL_KonamiP4IO_ParseReply(NULL, 4, 0, NULL, NULL));

    /* The device info reply of test 1 */
    DeviceInfoReply(info);
    CHECK(ParseReplyExact(info, sizeof(info), 1, &length, &first) && length == 44 && first == 0x00);
    CHECK(SDL_KonamiP4IO_ParseReply(info, sizeof(info), 1, &payload, &length) && payload == &info[4] && length == 44);
    /* Only its sequence passes */
    for (s = 0; s < 256; ++s) {
        CHECK(ParseReplyExact(info, sizeof(info), (uint8_t)s, NULL, NULL) == (s == 1));
    }
    /* The command byte is not checked, as bemanitools does not check it */
    for (s = 0; s < 256; ++s) {
        info[1] = (uint8_t)s;
        CHECK(ParseReplyExact(info, sizeof(info), 1, &length, NULL) && length == 44);
    }
    DeviceInfoReply(info);

    /* Every truncation that cuts the payload fails, even with the rest of the
       reply still in the buffer. Bytes past the payload do not count. */
    memset(reply, 0x5A, sizeof(reply));
    memcpy(reply, info, sizeof(info));
    for (n = 0; n <= sizeof(reply); ++n) {
        const bool whole = (n >= sizeof(info));

        CHECK(SDL_KonamiP4IO_ParseReply(reply, n, 1, NULL, &length) == whole);
        CHECK(ParseReplyExact(reply, n, 1, NULL, NULL) == whole);
        if (whole) {
            CHECK(length == 44);
        }
    }

    /* Payload lengths up to 60 fit in 64 bytes, 61 is never a reply */
    for (n = 0; n <= 61; ++n) {
        memset(reply, 0, sizeof(reply));
        reply[0] = 0xAA;
        reply[2] = 7;
        reply[3] = (uint8_t)n;
        CHECK(ParseReplyExact(reply, sizeof(reply), 7, &length, NULL) == (n <= 60));
        if (n <= 60) {
            CHECK(length == n);
            CHECK(ParseReplyExact(reply, 4 + n, 7, &length, NULL) && length == n);
            CHECK(n == 0 || !ParseReplyExact(reply, 4 + n - 1, 7, NULL, NULL));
        }
    }
    reply[3] = 0xFF;
    CHECK(!ParseReplyExact(reply, sizeof(reply), 7, NULL, NULL));

    /* The out parameters are optional */
    CHECK(SDL_KonamiP4IO_ParseReply(info, sizeof(info), 1, NULL, NULL));
}

/* 1: the device info payload */
static void TestDeviceInfo(void)
{
    uint8_t info[48];
    uint8_t emulator[64];
    uint8_t payload[64];
    SDL_KonamiP4IODeviceInfo parsed, before;
    size_t n;

    DeviceInfoReply(info);
    memset(&parsed, 0xEE, sizeof(parsed));
    CHECK(SDL_KonamiP4IO_ParseDeviceInfo(&info[4], 44, &parsed));
    CHECK(strcmp(parsed.product, "BMPU") == 0);
    CHECK(parsed.major == 1 && parsed.minor == 1 && parsed.revision == 2);
    CHECK(strcmp(parsed.date, "Jun 12 2013") == 0 && strcmp(parsed.time, "19:24:19") == 0);
    CHECK(memcmp(parsed.type, "\x00\x00\x00\x0B", 4) == 0);

    EmulatorReply(emulator, 1);
    CHECK(SDL_KonamiP4IO_ParseDeviceInfo(&emulator[4], 44, &parsed));
    CHECK(strcmp(parsed.product, "P4IO") == 0);
    CHECK(parsed.major == 5 && parsed.minor == 7 && parsed.revision == 3);
    CHECK(strcmp(parsed.date, "build_date") == 0 && strcmp(parsed.time, "build_time") == 0);
    CHECK(memcmp(parsed.type, "\x13\x37\x13\x37", 4) == 0);

    /* Exactly 44 bytes. Any other length changes nothing. */
    memset(payload, 0x41, sizeof(payload));
    memcpy(payload, &info[4], 44);
    for (n = 0; n <= sizeof(payload); ++n) {
        memset(&parsed, 0xEE, sizeof(parsed));
        before = parsed;
        if (n == 44) {
            CHECK(SDL_KonamiP4IO_ParseDeviceInfo(payload, n, &parsed));
        } else {
            CHECK(!SDL_KonamiP4IO_ParseDeviceInfo(payload, n, &parsed));
            CHECK(memcmp(&parsed, &before, sizeof(parsed)) == 0);
        }
    }
    CHECK(!SDL_KonamiP4IO_ParseDeviceInfo(NULL, 44, &parsed));
    CHECK(!SDL_KonamiP4IO_ParseDeviceInfo(&info[4], 44, NULL));

    /* Every field sits where cmd.h puts it: type 0-3, pad 4, version 5-7,
       product 8-11, date 12-27, time 28-43 */
    for (n = 0; n < 44; ++n) {
        payload[n] = (uint8_t)('a' + (n % 26));
    }
    payload[4] = 0xFF;
    payload[5] = 9;
    payload[6] = 8;
    payload[7] = 7;
    CHECK(SDL_KonamiP4IO_ParseDeviceInfo(payload, 44, &parsed));
    CHECK(memcmp(parsed.type, "abcd", 4) == 0);
    CHECK(parsed.major == 9 && parsed.minor == 8 && parsed.revision == 7);
    CHECK(strcmp(parsed.product, "ijkl") == 0);
    CHECK(strcmp(parsed.date, "mnopqrstuvwxyzab") == 0);
    CHECK(strcmp(parsed.time, "cdefghijklmnopqr") == 0);
    /* A text ends at its first NUL, and a byte outside printable ASCII
       shows as a question mark */
    payload[10] = 0x00;
    payload[12] = 0x7F;
    payload[13] = 0x1F;
    payload[14] = 0x80;
    payload[15] = 0x20;
    payload[16] = 0x7E;
    payload[28] = 0x00;
    CHECK(SDL_KonamiP4IO_ParseDeviceInfo(payload, 44, &parsed));
    CHECK(strcmp(parsed.product, "ij") == 0);
    CHECK(strncmp(parsed.date, "??? ~", 5) == 0 && strlen(parsed.date) == 16);
    CHECK(parsed.time[0] == '\0');
}

/* 1 and 5: the start-up. INIT with no payload and its outcome ignored, then
   GET DEVICE INFO, the sequence from 0. */
static void TestStartup(void)
{
    SDL_KonamiP4IOStartup startup;
    uint8_t request[SDL_KONAMI_P4IO_REQUEST_MAX];
    uint8_t info[48], padded[SDL_KONAMI_P4IO_READ_LENGTH], emulator[64];
    uint8_t init_padded[64];
    size_t n, i;

    /* Each way INIT can go */
    static const struct
    {
        bool exchanged;
        size_t length;
        const char *bytes;
    } init_outcomes[] = {
        { true, 0, "" },                          /* The zero-byte read bemanitools describes */
        { true, 4, "\xAA\x00\x00\x00" },          /* The bare header */
        { false, 0, "" },                         /* The write or the read failed */
        { true, 4, "\xAA\x00\x05\x00" },          /* Another sequence */
        { true, 2, "\x12\x34" },                  /* Two bytes of anything */
        { true, 5, "\xAA\x00\x00\x3D\x00" },      /* A payload length no reply has */
        { true, 8, "\xAA\x00\x00\x04\x01\x02\x03\x04" },
        { false, 4, "\xAA\x00\x00\x00" },         /* A reply counts for nothing when the exchange failed */
    };

    memset(init_padded, 0, sizeof(init_padded));
    memcpy(init_padded, "\xAA\x00\x00\x00", 4);

    SDL_KonamiP4IO_StartupInit(&startup);
    CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_INIT && startup.sequence == 0 && !startup.identified);
    CHECK(IsZero(&startup.info, sizeof(startup.info)));
    CHECK(SDL_KonamiP4IO_StartupNext(&startup, request) == 4 && memcmp(request, "\xAA\x00\x00\x00", 4) == 0);
    /* Asking again gives the same request */
    CHECK(SDL_KonamiP4IO_StartupNext(&startup, request) == 4 && memcmp(request, "\xAA\x00\x00\x00", 4) == 0);

    /* However INIT went, GET DEVICE INFO follows with sequence 1 */
    for (i = 0; i < ARRAYSIZE(init_outcomes); ++i) {
        SDL_KonamiP4IO_StartupInit(&startup);
        StartupResultExact(&startup, init_outcomes[i].exchanged, (const uint8_t *)init_outcomes[i].bytes,
                           init_outcomes[i].length);
        CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_DEVICE_INFO && startup.sequence == 1 && !startup.identified);
        CHECK(IsZero(&startup.info, sizeof(startup.info)));
        CHECK(SDL_KonamiP4IO_StartupNext(&startup, request) == 4 && memcmp(request, "\xAA\x01\x01\x00", 4) == 0);
    }
    /* A 64-byte INIT reply, as the emulator hands the game every read */
    SDL_KonamiP4IO_StartupInit(&startup);
    StartupResultExact(&startup, true, init_padded, sizeof(init_padded));
    CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_DEVICE_INFO && startup.sequence == 1);
    /* NULL with no bytes is a zero-byte read too */
    SDL_KonamiP4IO_StartupInit(&startup);
    SDL_KonamiP4IO_StartupResult(&startup, true, NULL, 0);
    CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_DEVICE_INFO && startup.sequence == 1);

    /* The device info reply identifies the board, and the start-up is over */
    DeviceInfoReply(info);
    StartupResultExact(&startup, true, info, sizeof(info));
    CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_DONE && startup.sequence == 2 && startup.identified);
    CHECK(strcmp(startup.info.product, "BMPU") == 0);
    CHECK(startup.info.major == 1 && startup.info.minor == 1 && startup.info.revision == 2);
    memset(request, 0xEE, sizeof(request));
    CHECK(SDL_KonamiP4IO_StartupNext(&startup, request) == 0 && request[0] == 0xEE);
    /* Nothing is asked any more, so nothing changes */
    SDL_KonamiP4IO_StartupResult(&startup, false, NULL, 0);
    CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_DONE && startup.sequence == 2 && startup.identified);
    StartupResultExact(&startup, true, info, sizeof(info));
    CHECK(startup.sequence == 2 && strcmp(startup.info.product, "BMPU") == 0);

    /* The reply read with 65 bytes and stale bytes behind it */
    memset(padded, 0x5A, sizeof(padded));
    memcpy(padded, info, sizeof(info));
    for (n = 0; n <= sizeof(padded); ++n) {
        SDL_KonamiP4IO_StartupInit(&startup);
        SDL_KonamiP4IO_StartupResult(&startup, true, NULL, 0);
        SDL_KonamiP4IO_StartupResult(&startup, true, padded, n);
        CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_DONE && startup.sequence == 2);
        /* Every truncation below 48 bytes leaves the board unidentified */
        CHECK(startup.identified == (n >= sizeof(info)));
        if (!startup.identified) {
            CHECK(IsZero(&startup.info, sizeof(startup.info)));
        }
        SDL_KonamiP4IO_StartupInit(&startup);
        SDL_KonamiP4IO_StartupResult(&startup, true, NULL, 0);
        StartupResultExact(&startup, true, padded, n);
        CHECK(startup.identified == (n >= sizeof(info)));
    }

    /* The emulator's 64-byte reply */
    EmulatorReply(emulator, 1);
    SDL_KonamiP4IO_StartupInit(&startup);
    SDL_KonamiP4IO_StartupResult(&startup, true, NULL, 0);
    StartupResultExact(&startup, true, emulator, sizeof(emulator));
    CHECK(startup.identified && strcmp(startup.info.product, "P4IO") == 0 && startup.info.major == 5);

    /* A reply with another sequence is refused, INIT's among them */
    {
        static const uint8_t sequences[] = { 0x00, 0x02, 0x81, 0xFF };

        for (i = 0; i < ARRAYSIZE(sequences); ++i) {
            DeviceInfoReply(info);
            info[2] = sequences[i];
            SDL_KonamiP4IO_StartupInit(&startup);
            SDL_KonamiP4IO_StartupResult(&startup, true, NULL, 0);
            StartupResultExact(&startup, true, info, sizeof(info));
            CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_DONE && !startup.identified);
            CHECK(IsZero(&startup.info, sizeof(startup.info)));
        }
    }
    /* Another start byte, another payload length, or a failed exchange */
    DeviceInfoReply(info);
    info[0] = 0xAB;
    SDL_KonamiP4IO_StartupInit(&startup);
    SDL_KonamiP4IO_StartupResult(&startup, true, NULL, 0);
    StartupResultExact(&startup, true, info, sizeof(info));
    CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_DONE && !startup.identified);
    {
        static const uint8_t lengths[] = { 0, 1, 43, 45, 60 };

        memset(padded, 0, sizeof(padded));
        DeviceInfoReply(info);
        memcpy(padded, info, sizeof(info));
        for (i = 0; i < ARRAYSIZE(lengths); ++i) {
            padded[3] = lengths[i];
            SDL_KonamiP4IO_StartupInit(&startup);
            SDL_KonamiP4IO_StartupResult(&startup, true, NULL, 0);
            StartupResultExact(&startup, true, padded, sizeof(padded));
            CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_DONE && !startup.identified);
        }
    }
    DeviceInfoReply(info);
    SDL_KonamiP4IO_StartupInit(&startup);
    SDL_KonamiP4IO_StartupResult(&startup, true, NULL, 0);
    StartupResultExact(&startup, false, info, sizeof(info));
    CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_DONE && startup.sequence == 2 && !startup.identified);
    CHECK(IsZero(&startup.info, sizeof(startup.info)));

    /* The sequence counts requests, not answers: a failed INIT still moves
       GET DEVICE INFO to 1, so INIT's own reply cannot answer it */
    SDL_KonamiP4IO_StartupInit(&startup);
    SDL_KonamiP4IO_StartupResult(&startup, false, NULL, 0);
    CHECK(SDL_KonamiP4IO_StartupNext(&startup, request) == 4 && request[2] == 1);
    StartupResultExact(&startup, true, (const uint8_t *)"\xAA\x00\x00\x00", 4);
    CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_DONE && !startup.identified);

    /* 5: reconnect. The next board starts over from INIT and sequence 0. */
    DeviceInfoReply(info);
    SDL_KonamiP4IO_StartupInit(&startup);
    SDL_KonamiP4IO_StartupResult(&startup, true, NULL, 0);
    StartupResultExact(&startup, true, info, sizeof(info));
    CHECK(startup.identified);
    SDL_KonamiP4IO_StartupInit(&startup);
    CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_INIT && startup.sequence == 0 && !startup.identified);
    CHECK(IsZero(&startup.info, sizeof(startup.info)));
    CHECK(SDL_KonamiP4IO_StartupNext(&startup, request) == 4 && memcmp(request, "\xAA\x00\x00\x00", 4) == 0);
}

/* A board that answers the way the sources describe, to run the whole
   start-up through */
typedef struct Board
{
    int kind;            /* 0 empty INIT reply, 1 emulator, 2 silent */
    int requests;
    uint8_t sent[4][SDL_KONAMI_P4IO_REQUEST_MAX];
    size_t sent_length[4];
} Board;

static bool BoardExchange(Board *board, const uint8_t *request, size_t length, uint8_t reply[SDL_KONAMI_P4IO_READ_LENGTH],
                          size_t *received)
{
    if (board->requests < 4) {
        memcpy(board->sent[board->requests], request, length);
        board->sent_length[board->requests] = length;
    }
    ++board->requests;
    *received = 0;
    if (board->kind == 2) {
        return false;
    }
    if (request[1] == SDL_KONAMI_P4IO_CMD_INIT) {
        if (board->kind == 1) {
            memset(reply, 0, 64);
            reply[0] = 0xAA;
            reply[2] = request[2];
            *received = 64;
        }
        return true;
    }
    if (board->kind == 1) {
        EmulatorReply(reply, request[2]);
        *received = 64;
    } else {
        DeviceInfoReply(reply);
        reply[2] = request[2];
        *received = 48;
    }
    return true;
}

static void TestBoards(void)
{
    int kind;

    for (kind = 0; kind < 3; ++kind) {
        SDL_KonamiP4IOStartup startup;
        uint8_t request[SDL_KONAMI_P4IO_REQUEST_MAX];
        uint8_t reply[SDL_KONAMI_P4IO_READ_LENGTH];
        size_t length, received;
        Board board;
        int guard = 0;

        memset(&board, 0, sizeof(board));
        board.kind = kind;
        SDL_KonamiP4IO_StartupInit(&startup);
        while ((length = SDL_KonamiP4IO_StartupNext(&startup, request)) > 0 && guard++ < 10) {
            const bool exchanged = BoardExchange(&board, request, length, reply, &received);

            SDL_KonamiP4IO_StartupResult(&startup, exchanged, reply, received);
        }
        /* Two requests, INIT then GET DEVICE INFO, whatever the board said */
        CHECK(board.requests == 2);
        CHECK(board.sent_length[0] == 4 && memcmp(board.sent[0], "\xAA\x00\x00\x00", 4) == 0);
        CHECK(board.sent_length[1] == 4 && memcmp(board.sent[1], "\xAA\x01\x01\x00", 4) == 0);
        CHECK(startup.step == SDL_KONAMI_P4IO_STARTUP_DONE);
        CHECK(startup.identified == (kind != 2));
        if (kind == 0) {
            CHECK(strcmp(startup.info.product, "BMPU") == 0);
        } else if (kind == 1) {
            CHECK(strcmp(startup.info.product, "P4IO") == 0);
        }
    }
}

/* The layouts, restated from the sources as tables of their own */

/* jubeat, from the bit table in bemanitools' emulator (jbhook-util/p4io.c),
   which agrees with its driver (jbio-p4io/jbio.c): byte, bit, panel */
static const struct
{
    int byte, bit, panel;
} jubeat_table[] = {
    { 0, 1, 2 }, { 0, 2, 6 }, { 0, 3, 10 }, { 0, 4, 14 }, { 0, 5, 1 }, { 0, 6, 5 }, { 0, 7, 9 },
    { 1, 1, 4 }, { 1, 2, 8 }, { 1, 3, 12 }, { 1, 4, 16 }, { 1, 5, 3 }, { 1, 6, 7 }, { 1, 7, 11 },
    { 2, 0, 13 }, { 2, 4, 15 },
};

static uint32_t ReferenceJubeat(uint32_t word)
{
    uint32_t buttons = 0;
    size_t i;

    for (i = 0; i < ARRAYSIZE(jubeat_table); ++i) {
        const int bit = jubeat_table[i].byte * 8 + jubeat_table[i].bit;

        if (!((word >> bit) & 1)) {
            buttons |= (uint32_t)1 << (jubeat_table[i].panel - 1);
        }
    }
    if ((word >> 28) & 1) {
        buttons |= (uint32_t)1 << 16; /* Test, byte 3 bit 4 */
    }
    if ((word >> 25) & 1) {
        buttons |= (uint32_t)1 << 17; /* Service, byte 3 bit 1 */
    }
    return buttons;
}

/* DDR, from p4io-mdxfdrv's report structure (src/p4io.h): byte, bit, button */
static const struct
{
    int byte, bit, button;
} ddr_table[] = {
    { 0, 0, 0 }, { 0, 1, 1 }, { 0, 2, 2 }, { 0, 3, 3 }, { 0, 4, 4 },
    { 1, 0, 5 }, { 1, 1, 6 }, { 1, 2, 7 }, { 1, 3, 8 }, { 1, 4, 9 },
    { 3, 0, 10 }, { 3, 1, 11 }, { 3, 4, 12 },
};

static uint32_t ReferenceDDR(uint32_t word)
{
    uint32_t buttons = 0;
    size_t i;

    for (i = 0; i < ARRAYSIZE(ddr_table); ++i) {
        if ((word >> (ddr_table[i].byte * 8 + ddr_table[i].bit)) & 1) {
            buttons |= (uint32_t)1 << ddr_table[i].button;
        }
    }
    return buttons;
}

static uint32_t Reference(int layout, uint32_t word)
{
    switch (layout) {
    case SDL_KONAMI_P4IO_LAYOUT_JUBEAT:
        return ReferenceJubeat(word);
    case SDL_KONAMI_P4IO_LAYOUT_DDR:
        return ReferenceDDR(word);
    default:
        return word;
    }
}

static void Report(uint8_t report[SDL_KONAMI_P4IO_REPORT_LENGTH], uint32_t word, uint8_t fill)
{
    memset(report, fill, SDL_KONAMI_P4IO_REPORT_LENGTH);
    report[0] = (uint8_t)(word & 0xFF);
    report[1] = (uint8_t)((word >> 8) & 0xFF);
    report[2] = (uint8_t)((word >> 16) & 0xFF);
    report[3] = (uint8_t)(word >> 24);
}

static bool Decodes(int layout, uint32_t word, uint8_t fill, uint32_t expected)
{
    uint8_t report[SDL_KONAMI_P4IO_REPORT_LENGTH];
    SDL_KonamiP4IOState state;

    Report(report, word, fill);
    memset(&state, 0xEE, sizeof(state));
    return SDL_KonamiP4IO_Decode(layout, report, sizeof(report), &state) && state.buttons == expected;
}

/* 2: jubeat */
static void TestJubeat(void)
{
    const int J = SDL_KONAMI_P4IO_LAYOUT_JUBEAT;
    const uint32_t idle = 0x0011FEFE; /* FE FE 11 00 */
    SDL_KonamiP4IOState state;
    uint8_t report[SDL_KONAMI_P4IO_REPORT_LENGTH];
    size_t i;

    memset(report, 0, sizeof(report));
    memcpy(report, "\xFE\xFE\x11\x00", 4);
    CHECK(DecodeExact(J, report, sizeof(report), &state) && state.buttons == 0);
    memcpy(report, "\xDE\xFE\x11\x00", 4);
    CHECK(DecodeExact(J, report, sizeof(report), &state) && state.buttons == 0x00001); /* Panel 1 */
    memcpy(report, "\xFE\xFE\x11\x10", 4);
    CHECK(DecodeExact(J, report, sizeof(report), &state) && state.buttons == 0x10000); /* Test */
    memcpy(report, "\xFE\xFE\x11\x02", 4);
    CHECK(DecodeExact(J, report, sizeof(report), &state) && state.buttons == 0x20000); /* Service */

    /* Each of the 16 panel bits cleared alone presses one panel. The panel
       bits of the part's table, in panel order: 5, 1, 13, 9, 6, 2, 14, 10,
       7, 3, 15, 11, 16, 4, 20, 12. */
    {
        static const int bits[16] = { 5, 1, 13, 9, 6, 2, 14, 10, 7, 3, 15, 11, 16, 4, 20, 12 };

        for (i = 0; i < 16; ++i) {
            CHECK(Decodes(J, idle & ~((uint32_t)1 << bits[i]), 0x00, (uint32_t)1 << i));
            CHECK(Decodes(J, idle & ~((uint32_t)1 << bits[i]), 0xFF, (uint32_t)1 << i));
        }
    }
    /* The emulator's rest word, the low three bytes all set */
    CHECK(Decodes(J, 0x00FFFFFF, 0x00, 0));
    /* A word of zeros holds every panel, and all ones every panel released
       with Test and Service */
    CHECK(Decodes(J, 0x00000000, 0x00, 0x0FFFF));
    CHECK(Decodes(J, 0xFFFFFFFF, 0x00, 0x30000));
    /* Coin, bit 24, is not in this layout */
    CHECK(Decodes(J, idle | ((uint32_t)1 << 24), 0x00, 0));
}

/* 3: DDR */
static void TestDDR(void)
{
    const int D = SDL_KONAMI_P4IO_LAYOUT_DDR;
    SDL_KonamiP4IOState state;
    uint8_t report[SDL_KONAMI_P4IO_REPORT_LENGTH];

    memset(report, 0, sizeof(report));
    CHECK(DecodeExact(D, report, sizeof(report), &state) && state.buttons == 0);
    report[0] = 0x02;
    CHECK(DecodeExact(D, report, sizeof(report), &state) && state.buttons == ((uint32_t)1 << 1)); /* P1 up */
    report[0] = 0x00;
    report[1] = 0x01;
    CHECK(DecodeExact(D, report, sizeof(report), &state) && state.buttons == ((uint32_t)1 << 5)); /* P2 OK */
    report[1] = 0x00;
    report[3] = 0x01;
    CHECK(DecodeExact(D, report, sizeof(report), &state) && state.buttons == ((uint32_t)1 << 10)); /* Coin */
    report[3] = 0x02;
    CHECK(DecodeExact(D, report, sizeof(report), &state) && state.buttons == ((uint32_t)1 << 11)); /* Service */
    report[3] = 0x10;
    CHECK(DecodeExact(D, report, sizeof(report), &state) && state.buttons == ((uint32_t)1 << 12)); /* Test */
    /* Everything at once */
    CHECK(Decodes(D, 0xFFFFFFFF, 0xFF, 0x1FFF));
}

/* 4 and the rest: every bit alone, every pair of bits, and a sweep, in
   every layout, against the tables above */
static void TestEveryBit(void)
{
    size_t l;
    int a, b;

    /* 4: each of the 32 bits alone sets one raw button */
    for (a = 0; a < 32; ++a) {
        CHECK(Decodes(SDL_KONAMI_P4IO_LAYOUT_RAW, (uint32_t)1 << a, 0x00, (uint32_t)1 << a));
        CHECK(Decodes(SDL_KONAMI_P4IO_LAYOUT_RAW, (uint32_t)1 << a, 0xFF, (uint32_t)1 << a));
    }
    /* The raw layout sends the jubeat rest word as it comes */
    CHECK(Decodes(SDL_KONAMI_P4IO_LAYOUT_RAW, 0x0011FEFE, 0x00, 0x0011FEFE));

    for (l = 0; l < ARRAYSIZE(layouts); ++l) {
        const int layout = layouts[l];
        int mismatches = 0;
        uint32_t seed = 0x12345678;
        int n;

        for (a = 0; a < 32; ++a) {
            const uint32_t single = (uint32_t)1 << a;

            /* Alone from all zeros, and alone cleared from all ones */
            mismatches += !Decodes(layout, single, 0x00, Reference(layout, single));
            mismatches += !Decodes(layout, ~single, 0x00, Reference(layout, ~single));
            for (b = a + 1; b < 32; ++b) {
                const uint32_t pair = single | ((uint32_t)1 << b);

                mismatches += !Decodes(layout, pair, 0x00, Reference(layout, pair));
                mismatches += !Decodes(layout, ~pair, 0x00, Reference(layout, ~pair));
            }
        }
        for (n = 0; n < 65536; ++n) {
            seed = seed * 1664525u + 1013904223u;
            /* Bytes 4 to 15 carry nothing, whatever they hold */
            mismatches += !Decodes(layout, seed, (uint8_t)(n & 0xFF), Reference(layout, seed));
        }
        CHECK(mismatches == 0);
    }

    /* Which bits each layout reads at all */
    {
        uint32_t jubeat_bits = 0, ddr_bits = 0;

        for (a = 0; a < 32; ++a) {
            const uint32_t single = (uint32_t)1 << a;

            if (ReferenceJubeat(single) != ReferenceJubeat(0)) {
                jubeat_bits |= single;
            }
            if (ReferenceDDR(single) != ReferenceDDR(0)) {
                ddr_bits |= single;
            }
        }
        CHECK(jubeat_bits == 0x1211FEFE);
        CHECK(ddr_bits == 0x13001F1F);
    }
}

/* 5: every truncation below 16 bytes, with a pressed report still in the
   buffer past the length, changes nothing */
static void TestTruncation(void)
{
    uint8_t stale[64];
    size_t l, n;

    memset(stale, 0x00, sizeof(stale));
    memcpy(stale, "\x21\x01\x10\x12", 4);
    for (l = 0; l < ARRAYSIZE(layouts); ++l) {
        const int layout = layouts[l];
        SDL_KonamiP4IOState state, before;

        for (n = 0; n < SDL_KONAMI_P4IO_REPORT_LENGTH; ++n) {
            state.buttons = 0xA5A5A5A5;
            before = state;
            CHECK(!SDL_KonamiP4IO_Decode(layout, stale, n, &state));
            CHECK(state.buttons == before.buttons);
            CHECK(!DecodeExact(layout, stale, n, &state));
            CHECK(state.buttons == before.buttons);
        }
        /* 16 bytes and more decode the first four */
        for (n = SDL_KONAMI_P4IO_REPORT_LENGTH; n <= sizeof(stale); ++n) {
            state.buttons = 0xA5A5A5A5;
            CHECK(DecodeExact(layout, stale, n, &state));
            CHECK(state.buttons == Reference(layout, 0x12100121));
        }
        state.buttons = 0xA5A5A5A5;
        CHECK(!SDL_KonamiP4IO_Decode(layout, NULL, 16, &state) && state.buttons == 0xA5A5A5A5);
        CHECK(!SDL_KonamiP4IO_Decode(layout, stale, 16, NULL));
        /* The part's numbers, whatever the header says: 15 bytes are short,
           16 are a report */
        CHECK(!DecodeExact(layout, stale, 15, &state) && state.buttons == 0xA5A5A5A5);
        CHECK(DecodeExact(layout, stale, 16, &state) && state.buttons == Reference(layout, 0x12100121));
    }

    /* An unknown layout decodes nothing */
    {
        static const int unknown[] = { -1, 3, 4, 100 };
        SDL_KonamiP4IOState state;

        for (l = 0; l < ARRAYSIZE(unknown); ++l) {
            state.buttons = 0x5A5A5A5A;
            CHECK(!SDL_KonamiP4IO_Decode(unknown[l], stale, 16, &state) && state.buttons == 0x5A5A5A5A);
        }
    }

    /* 5: reconnect. A new board starts with nothing pressed. */
    {
        SDL_KonamiP4IOState state;

        state.buttons = 0xFFFFFFFF;
        SDL_KonamiP4IO_ResetState(&state);
        CHECK(state.buttons == 0);
    }
}

static void TestLayouts(void)
{
    static const struct
    {
        const char *hint;
        int layout;
    } hints[] = {
        { "raw", SDL_KONAMI_P4IO_LAYOUT_RAW },
        { "RAW", SDL_KONAMI_P4IO_LAYOUT_RAW },
        { "jubeat", SDL_KONAMI_P4IO_LAYOUT_JUBEAT },
        { "JUBEAT", SDL_KONAMI_P4IO_LAYOUT_JUBEAT },
        { "JuBeAt", SDL_KONAMI_P4IO_LAYOUT_JUBEAT },
        { "ddr", SDL_KONAMI_P4IO_LAYOUT_DDR },
        { "DDR", SDL_KONAMI_P4IO_LAYOUT_DDR },
        { "dDr", SDL_KONAMI_P4IO_LAYOUT_DDR },
        { "", SDL_KONAMI_P4IO_LAYOUT_RAW },
        { "dd", SDL_KONAMI_P4IO_LAYOUT_RAW },
        { "ddrr", SDL_KONAMI_P4IO_LAYOUT_RAW },
        { "ddr ", SDL_KONAMI_P4IO_LAYOUT_RAW },
        { " ddr", SDL_KONAMI_P4IO_LAYOUT_RAW },
        { "jubea", SDL_KONAMI_P4IO_LAYOUT_RAW },
        { "jubeats", SDL_KONAMI_P4IO_LAYOUT_RAW },
        { "1", SDL_KONAMI_P4IO_LAYOUT_RAW },
        { "iidx", SDL_KONAMI_P4IO_LAYOUT_RAW },
    };
    SDL_KonamiP4IOIdentity identity;
    size_t i;

    CHECK(SDL_KonamiP4IO_ParseLayout(NULL) == SDL_KONAMI_P4IO_LAYOUT_RAW);
    for (i = 0; i < ARRAYSIZE(hints); ++i) {
        CHECK(SDL_KonamiP4IO_ParseLayout(hints[i].hint) == hints[i].layout);
    }

    CHECK(SDL_KonamiP4IO_GetIdentity(SDL_KONAMI_P4IO_LAYOUT_RAW, &identity));
    CHECK(strcmp(identity.name, "Konami P4IO") == 0 && identity.nbuttons == 32);
    CHECK(SDL_KonamiP4IO_GetIdentity(SDL_KONAMI_P4IO_LAYOUT_JUBEAT, &identity));
    CHECK(strcmp(identity.name, "Konami jubeat") == 0 && identity.nbuttons == 18);
    CHECK(SDL_KonamiP4IO_GetIdentity(SDL_KONAMI_P4IO_LAYOUT_DDR, &identity));
    CHECK(strcmp(identity.name, "Konami P4IO DDR") == 0 && identity.nbuttons == 13);
    identity.name = "unchanged";
    identity.nbuttons = -7;
    CHECK(!SDL_KonamiP4IO_GetIdentity(3, &identity) && !SDL_KonamiP4IO_GetIdentity(-1, &identity));
    CHECK(strcmp(identity.name, "unchanged") == 0 && identity.nbuttons == -7);

    /* Every button a layout decodes has a joystick button */
    for (i = 0; i < ARRAYSIZE(layouts); ++i) {
        CHECK(SDL_KonamiP4IO_GetIdentity(layouts[i], &identity));
        if (identity.nbuttons < 32) {
            CHECK(Decodes(layouts[i], 0xFFFFFFFF, 0x00, Reference(layouts[i], 0xFFFFFFFF)));
            CHECK((Reference(layouts[i], 0xFFFFFFFF) | Reference(layouts[i], 0)) < ((uint32_t)1 << identity.nbuttons));
        }
    }
}

/* The interface of p4io-mdxfdrv: exactly three endpoints, bulk OUT, bulk IN
   and interrupt IN, in that order (src/p4io.c:40-76). No source records the
   addresses. */
static void TestEndpoints(void)
{
    static const SDL_KonamiP4IOEndpoint p4io[3] = { { 0x01, 0x02 }, { 0x82, 0x02 }, { 0x83, 0x03 } };
    SDL_KonamiP4IOEndpoint endpoints[4];
    SDL_KonamiP4IOEndpoints found;
    int position, attributes, count;

    memset(&found, 0, sizeof(found));
    CHECK(SDL_KonamiP4IO_FindEndpoints(p4io, 3, &found));
    CHECK(found.bulk_out == 0x01 && found.bulk_in == 0x82 && found.interrupt_in == 0x83);

    /* Any addresses */
    memcpy(endpoints, p4io, sizeof(p4io));
    endpoints[0].address = 0x04;
    endpoints[1].address = 0x81;
    endpoints[2].address = 0x8F;
    CHECK(SDL_KonamiP4IO_FindEndpoints(endpoints, 3, &found));
    CHECK(found.bulk_out == 0x04 && found.bulk_in == 0x81 && found.interrupt_in == 0x8F);
    /* Only the transfer type counts in bmAttributes */
    endpoints[0].attributes = 0xFE;
    endpoints[1].attributes = 0x06;
    endpoints[2].attributes = 0x33;
    CHECK(SDL_KonamiP4IO_FindEndpoints(endpoints, 3, &found));

    /* Exactly three */
    memcpy(endpoints, p4io, sizeof(p4io));
    endpoints[3] = p4io[2];
    for (count = 0; count <= 4; ++count) {
        CHECK(SDL_KonamiP4IO_FindEndpoints(endpoints, count, &found) == (count == 3));
    }
    CHECK(!SDL_KonamiP4IO_FindEndpoints(NULL, 3, &found));
    CHECK(!SDL_KonamiP4IO_FindEndpoints(p4io, 3, NULL));

    /* The wrong type or direction in any position, and nothing is written */
    for (position = 0; position < 3; ++position) {
        for (attributes = 0; attributes < 4; ++attributes) {
            memcpy(endpoints, p4io, sizeof(p4io));
            endpoints[position].attributes = (uint8_t)attributes;
            memset(&found, 0xEE, sizeof(found));
            CHECK(SDL_KonamiP4IO_FindEndpoints(endpoints, 3, &found) == (attributes == (position == 2 ? 3 : 2)));
        }
        memcpy(endpoints, p4io, sizeof(p4io));
        endpoints[position].address = (uint8_t)(endpoints[position].address ^ 0x80);
        memset(&found, 0xEE, sizeof(found));
        CHECK(!SDL_KonamiP4IO_FindEndpoints(endpoints, 3, &found));
        CHECK(found.bulk_out == 0xEE && found.bulk_in == 0xEE && found.interrupt_in == 0xEE);
    }
    /* Every other order of the same three */
    {
        static const int orders[5][3] = { { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
        int o;

        for (o = 0; o < 5; ++o) {
            endpoints[0] = p4io[orders[o][0]];
            endpoints[1] = p4io[orders[o][1]];
            endpoints[2] = p4io[orders[o][2]];
            CHECK(!SDL_KonamiP4IO_FindEndpoints(endpoints, 3, &found));
        }
    }
}

int main(void)
{
    TestRequests();
    TestReplies();
    TestDeviceInfo();
    TestStartup();
    TestBoards();
    TestJubeat();
    TestDDR();
    TestEveryBit();
    TestTruncation();
    TestLayouts();
    TestEndpoints();

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
