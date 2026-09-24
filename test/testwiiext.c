/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_wii_ext_proto.c, the Wii
   Remote extensions of hifihedgehog/SDL#33 Part 2. The numbered groups follow
   the part's replay tests for each device. Every report is also fed as an
   exact-size heap copy, so AddressSanitizer catches a read past its length,
   and truncated inside a buffer full of stale bytes. */

#include "../src/joystick/hidapi/SDL_hidapi_wii_ext_proto.h"

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

#define AXIS_MIN (-32768)
#define AXIS_MAX 32767

/* ------------------------------------------------------------------------ */
/* Helpers */

typedef struct
{
    uint8_t bytes[22];
    size_t length;
} Report;

/* 32 BB BB then E0-E7 */
static Report Report32(const uint8_t e[8])
{
    Report r;
    memset(&r, 0, sizeof(r));
    r.bytes[0] = 0x32;
    memcpy(&r.bytes[3], e, 8);
    r.length = 11;
    return r;
}

/* Decodes a report the way the driver does: the span, then the decoder,
   from an exact-size heap copy. */
static bool Decode(SDL_WiiExtState *state, const Report *r, size_t length, SDL_WiiExtOutput *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    const uint8_t *span = NULL;
    size_t span_length = 0;
    bool result;

    if (length) {
        memcpy(copy, r->bytes, length);
    }
    result = SDL_WiiExt_GetSpan(copy, length, &span, &span_length, NULL) &&
             SDL_WiiExt_Decode(state, span, span_length, out);
    free(copy);
    return result;
}

static bool DecodeFull(SDL_WiiExtState *state, const uint8_t e[8], SDL_WiiExtOutput *out)
{
    Report r = Report32(e);
    return Decode(state, &r, r.length, out);
}

static bool Down(const SDL_WiiExtOutput *o, int button)
{
    return ((o->buttons >> button) & 1) != 0;
}

static bool Posted(const SDL_WiiExtOutput *o, int button)
{
    return ((o->button_mask >> button) & 1) != 0;
}

static bool AxisPosted(const SDL_WiiExtOutput *o, int axis)
{
    return ((o->axis_mask >> axis) & 1) != 0;
}

static int CountDown(const SDL_WiiExtOutput *o)
{
    int count = 0, i;
    for (i = 0; i < 32; ++i) {
        count += Down(o, i) ? 1 : 0;
    }
    return count;
}

/* How many of a report's outputs differ from a reference: buttons, the hat
   and the axes both posted. */
static int Differences(const SDL_WiiExtOutput *a, const SDL_WiiExtOutput *b)
{
    int count = 0, i;
    for (i = 0; i < 32; ++i) {
        if (Down(a, i) != Down(b, i)) {
            ++count;
        }
    }
    if (a->hat != b->hat) {
        ++count;
    }
    for (i = 0; i < SDL_WII_EXT_MAX_AXES; ++i) {
        if (AxisPosted(a, i) && AxisPosted(b, i) && a->axes[i] != b->axes[i]) {
            ++count;
        }
    }
    return count;
}

/* Every truncation, 0 bytes up to one short of full length, changes neither
   the state nor produces an output: once from an exact heap copy, once from a
   22-byte buffer whose tail holds a stale earlier report. */
static void CheckTruncations(SDL_WiiExtState *state, const Report *r)
{
    static const uint8_t stale[22] = { 0x32, 0x12, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x35, 0x00, 0x00, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00 };
    size_t n;

    for (n = 0; n < r->length; ++n) {
        SDL_WiiExtState before = *state;
        SDL_WiiExtOutput out;
        Report dirty;

        CHECK(!Decode(state, r, n, &out));
        CHECK(memcmp(&before, state, sizeof(before)) == 0);

        memcpy(dirty.bytes, stale, sizeof(stale));
        memcpy(dirty.bytes, r->bytes, n);
        dirty.length = n;
        {
            const uint8_t *span = NULL;
            size_t span_length = 0;
            CHECK(!SDL_WiiExt_GetSpan(dirty.bytes, n, &span, &span_length, NULL));
        }
        CHECK(memcmp(&before, state, sizeof(before)) == 0);
    }
}

static void CheckAllFFChangesNothing(SDL_WiiExtState *state)
{
    static const uint8_t ff[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    SDL_WiiExtState before = *state;
    SDL_WiiExtOutput out;

    CHECK(!DecodeFull(state, ff, &out));
    CHECK(memcmp(&before, state, sizeof(before)) == 0);
    CHECK(out.button_mask == 0 && out.axis_mask == 0);
}

static bool Bytes(const uint8_t *actual, const uint8_t *expected, size_t length)
{
    return memcmp(actual, expected, length) == 0;
}

/* A write request as the driver sends it: 22 bytes, zero padded */
static bool IsWrite(const uint8_t *request, uint32_t address, uint8_t value)
{
    uint8_t expected[SDL_WII_EXT_WRITE_REQUEST_SIZE];
    memset(expected, 0, sizeof(expected));
    expected[0] = 0x16;
    expected[1] = 0x04;
    expected[2] = (uint8_t)(address >> 16);
    expected[3] = (uint8_t)(address >> 8);
    expected[4] = (uint8_t)address;
    expected[5] = 0x01;
    expected[6] = value;
    return Bytes(request, expected, sizeof(expected));
}

/* ------------------------------------------------------------------------ */
/* Shared: the six-byte ID and classification */

static uint8_t IdentifyReply(uint8_t *reply, const uint8_t id[6])
{
    memset(reply, 0, 22);
    reply[0] = 0x21;
    reply[3] = 0x50;
    reply[4] = 0x00;
    reply[5] = 0xFA;
    memcpy(&reply[6], id, 6);
    return 22;
}

static int ClassifyReply(const uint8_t id[6], bool enabled)
{
    uint8_t reply[22];
    uint8_t parsed[6];
    const size_t length = IdentifyReply(reply, id);

    if (SDL_WiiExt_ParseIdentifyReply(reply, length, SDL_WII_EXT_REG_IDENTIFY, parsed) != SDL_WII_EXT_REPLY_OK) {
        return -1;
    }
    return SDL_WiiExt_Classify(parsed[0], parsed[4], parsed[5], enabled);
}

static void TestIdentify(void)
{
    uint8_t request[SDL_WII_EXT_READ_REQUEST_SIZE];
    uint8_t reply[22];
    uint8_t id[6];

    /* The requests: the full ID at 0xA400FA and the inactive Motion Plus
       probe at 0xA600FA, 6 bytes each. Byte 1 carries the rumble bit. */
    SDL_WiiExt_BuildReadRequest(request, false, SDL_WII_EXT_REG_IDENTIFY, 6);
    {
        static const uint8_t expected[] = { 0x17, 0x04, 0xA4, 0x00, 0xFA, 0x00, 0x06 };
        CHECK(Bytes(request, expected, sizeof(expected)));
    }
    SDL_WiiExt_BuildReadRequest(request, true, SDL_WII_EXT_REG_IDENTIFY, 6);
    CHECK(request[1] == 0x05);
    SDL_WiiExt_BuildReadRequest(request, false, SDL_WII_EXT_REG_MOTIONPLUS_PROBE, 6);
    {
        static const uint8_t expected[] = { 0x17, 0x04, 0xA6, 0x00, 0xFA, 0x00, 0x06 };
        CHECK(Bytes(request, expected, sizeof(expected)));
    }
    SDL_WiiExt_BuildReadRequest(request, false, SDL_WII_EXT_REG_STORED_ID, 4);
    {
        static const uint8_t expected[] = { 0x17, 0x04, 0xA4, 0x00, 0xF6, 0x00, 0x04 };
        CHECK(Bytes(request, expected, sizeof(expected)));
    }

    /* Guitar test 1: the success reply, then the old 2-byte echo rejected */
    {
        static const uint8_t guitar[6] = { 0x00, 0x00, 0xA4, 0x20, 0x01, 0x03 };
        CHECK(ClassifyReply(guitar, true) == SDL_WII_EXT_GUITAR);
    }
    memset(reply, 0, sizeof(reply));
    reply[0] = 0x21;
    reply[3] = 0x10;
    reply[4] = 0x00;
    reply[5] = 0xFE;
    reply[6] = 0x01;
    reply[7] = 0x03;
    CHECK(SDL_WiiExt_ParseIdentifyReply(reply, sizeof(reply), SDL_WII_EXT_REG_IDENTIFY, id) == SDL_WII_EXT_REPLY_REJECTED);

    /* Absent extension: error 7 in SE's low nibble */
    IdentifyReply(reply, (const uint8_t[6]){ 0 });
    reply[3] = 0x57;
    CHECK(SDL_WiiExt_ParseIdentifyReply(reply, sizeof(reply), SDL_WII_EXT_REG_IDENTIFY, id) == SDL_WII_EXT_REPLY_ABSENT);
    /* Another error, and a size other than 6 */
    reply[3] = 0x58;
    CHECK(SDL_WiiExt_ParseIdentifyReply(reply, sizeof(reply), SDL_WII_EXT_REG_IDENTIFY, id) == SDL_WII_EXT_REPLY_REJECTED);
    reply[3] = 0x10;
    CHECK(SDL_WiiExt_ParseIdentifyReply(reply, sizeof(reply), SDL_WII_EXT_REG_IDENTIFY, id) == SDL_WII_EXT_REPLY_REJECTED);
    /* Another report type, and a reply cut short of its six bytes */
    IdentifyReply(reply, (const uint8_t[6]){ 0x00, 0x00, 0xA4, 0x20, 0x01, 0x03 });
    reply[0] = 0x22;
    CHECK(SDL_WiiExt_ParseIdentifyReply(reply, sizeof(reply), SDL_WII_EXT_REG_IDENTIFY, id) == SDL_WII_EXT_REPLY_REJECTED);
    reply[0] = 0x21;
    {
        size_t n;
        for (n = 0; n < 12; ++n) {
            uint8_t *copy = (uint8_t *)malloc(n ? n : 1);
            if (n) {
                memcpy(copy, reply, n);
            }
            CHECK(SDL_WiiExt_ParseIdentifyReply(copy, n, SDL_WII_EXT_REG_IDENTIFY, id) == SDL_WII_EXT_REPLY_REJECTED);
            free(copy);
        }
        CHECK(SDL_WiiExt_ParseIdentifyReply(reply, 12, SDL_WII_EXT_REG_IDENTIFY, id) == SDL_WII_EXT_REPLY_OK);
    }
    /* The Motion Plus probe echoes its own address */
    reply[5] = 0xFA;
    CHECK(SDL_WiiExt_ParseIdentifyReply(reply, sizeof(reply), SDL_WII_EXT_REG_MOTIONPLUS_PROBE, id) == SDL_WII_EXT_REPLY_OK);
    reply[5] = 0xFE;
    CHECK(SDL_WiiExt_ParseIdentifyReply(reply, sizeof(reply), SDL_WII_EXT_REG_MOTIONPLUS_PROBE, id) == SDL_WII_EXT_REPLY_REJECTED);
}

static void TestClassification(void)
{
    static const struct
    {
        uint8_t id[6];
        int type;
    } table[] = {
        /* Unchanged: keyed on bytes 4 and 5 alone */
        { { 0x00, 0x00, 0xA4, 0x20, 0x00, 0x00 }, SDL_WII_EXT_NUNCHUK },
        { { 0xFF, 0x00, 0xA4, 0x20, 0x00, 0x00 }, SDL_WII_EXT_NUNCHUK }, /* the second Nunchuk ID */
        { { 0x00, 0x00, 0xA4, 0x20, 0x01, 0x01 }, SDL_WII_EXT_GAMEPAD },
        { { 0x01, 0x00, 0xA4, 0x20, 0x01, 0x01 }, SDL_WII_EXT_GAMEPAD }, /* Classic Pro, NES and SNES Classic */
        { { 0x00, 0x00, 0xA4, 0x20, 0x01, 0x20 }, SDL_WII_EXT_WIIUPRO },
        { { 0x00, 0x00, 0xA4, 0x20, 0x04, 0x02 }, SDL_WII_EXT_BALANCEBOARD },
        /* New */
        { { 0x00, 0x00, 0xA4, 0x20, 0x01, 0x03 }, SDL_WII_EXT_GUITAR },
        { { 0x01, 0x00, 0xA4, 0x20, 0x01, 0x03 }, SDL_WII_EXT_DRUMS },
        { { 0x03, 0x00, 0xA4, 0x20, 0x01, 0x03 }, SDL_WII_EXT_TURNTABLE },
        { { 0x02, 0x00, 0xA4, 0x20, 0x01, 0x03 }, SDL_WII_EXT_UNKNOWN },
        { { 0xFF, 0x00, 0xA4, 0x20, 0x01, 0x03 }, SDL_WII_EXT_UNKNOWN },
        { { 0x00, 0x00, 0xA4, 0x20, 0x01, 0x11 }, SDL_WII_EXT_TAIKO },
        { { 0xFF, 0x00, 0xA4, 0x20, 0x01, 0x12 }, SDL_WII_EXT_UDRAW },
        { { 0xFF, 0x00, 0xA4, 0x20, 0x00, 0x13 }, SDL_WII_EXT_DRAWSOME },
        { { 0x00, 0x00, 0xA4, 0x20, 0x03, 0x10 }, SDL_WII_EXT_SHINKANSEN },
        { { 0x00, 0x00, 0xA4, 0x20, 0x01, 0x10 }, SDL_WII_EXT_UNKNOWN }, /* Dolphin's placeholder */
        /* Bytes 1 to 3 are not checked */
        { { 0x00, 0x77, 0x00, 0x00, 0x01, 0x03 }, SDL_WII_EXT_GUITAR },
        /* Motion Plus IDs and anything else are the driver's or unknown */
        { { 0x00, 0x00, 0xA4, 0x20, 0x04, 0x05 }, SDL_WII_EXT_UNKNOWN },
        { { 0x00, 0x00, 0xA4, 0x20, 0x07, 0x05 }, SDL_WII_EXT_UNKNOWN },
        { { 0x00, 0x00, 0xA4, 0x20, 0x12, 0x34 }, SDL_WII_EXT_UNKNOWN },
    };
    size_t i;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        const int with = ClassifyReply(table[i].id, true);
        const int without = ClassifyReply(table[i].id, false);
        CHECK(with == table[i].type);
        /* With decoding off, only the configurations that existed before
           classify, and every new one is unknown as before */
        if (SDL_WiiExt_IsDecodedType(table[i].type)) {
            CHECK(without == SDL_WII_EXT_UNKNOWN);
        } else {
            CHECK(without == table[i].type);
        }
    }
    CHECK(!SDL_WiiExt_IsDecodedType(SDL_WII_EXT_BALANCEBOARD));
    CHECK(!SDL_WiiExt_IsDecodedType(SDL_WII_EXT_UNKNOWN));
    CHECK(!SDL_WiiExt_IsDecodedType(SDL_WII_EXT_NONE));
}

/* Guitar test 11, drum test 10, turntable test 8, TaTaCon test 1 and the
   Shinkansen's open fact: the stored copy behind an active Motion Plus */
static void TestStoredId(void)
{
    static const struct
    {
        uint8_t reply[10];
        int type;
    } table[] = {
        { { 0x21, 0x00, 0x00, 0x30, 0x00, 0xF6, 0x01, 0x0E, 0x00, 0x03 }, SDL_WII_EXT_GUITAR },
        { { 0x21, 0x00, 0x00, 0x30, 0x00, 0xF6, 0x01, 0x0E, 0x01, 0x03 }, SDL_WII_EXT_DRUMS },
        { { 0x21, 0x00, 0x00, 0x30, 0x00, 0xF6, 0x01, 0x0E, 0x03, 0x03 }, SDL_WII_EXT_TURNTABLE },
        { { 0x21, 0x00, 0x00, 0x30, 0x00, 0xF6, 0x01, 0x0E, 0x00, 0x11 }, SDL_WII_EXT_TAIKO },
        { { 0x21, 0x00, 0x00, 0x30, 0x00, 0xF6, 0x03, 0x0E, 0x00, 0x10 }, SDL_WII_EXT_SHINKANSEN },
        { { 0x21, 0x00, 0x00, 0x30, 0x00, 0xF6, 0x00, 0x0E, 0x00, 0x00 }, SDL_WII_EXT_NUNCHUK },
    };
    size_t i, n;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        uint8_t id0 = 0xAA, id4 = 0xAA, id5 = 0xAA;
        CHECK(SDL_WiiExt_ParseStoredIdReply(table[i].reply, sizeof(table[i].reply), &id0, &id4, &id5));
        CHECK(SDL_WiiExt_Classify(id0, id4, id5, true) == table[i].type);
        for (n = 0; n < sizeof(table[i].reply); ++n) {
            uint8_t *copy = (uint8_t *)malloc(n ? n : 1);
            if (n) {
                memcpy(copy, table[i].reply, n);
            }
            CHECK(!SDL_WiiExt_ParseStoredIdReply(copy, n, &id0, &id4, &id5));
            free(copy);
        }
    }
    /* A read error or another address is not a stored ID */
    {
        uint8_t reply[10] = { 0x21, 0x00, 0x00, 0x37, 0x00, 0xF6, 0x01, 0x0E, 0x00, 0x03 };
        uint8_t id0, id4, id5;
        CHECK(!SDL_WiiExt_ParseStoredIdReply(reply, sizeof(reply), &id0, &id4, &id5));
        reply[3] = 0x30;
        reply[5] = 0xFA;
        CHECK(!SDL_WiiExt_ParseStoredIdReply(reply, sizeof(reply), &id0, &id4, &id5));
    }

    /* Every new type drops the Motion Plus rather than ride in passthrough.
       The driver then deactivates it with 55 to 0xA400F0, resets the port
       and identifies directly. */
    CHECK(!SDL_WiiExt_AllowsMotionPlus(SDL_WII_EXT_GUITAR));
    CHECK(!SDL_WiiExt_AllowsMotionPlus(SDL_WII_EXT_DRUMS));
    CHECK(!SDL_WiiExt_AllowsMotionPlus(SDL_WII_EXT_TURNTABLE));
    CHECK(!SDL_WiiExt_AllowsMotionPlus(SDL_WII_EXT_TAIKO));
    CHECK(!SDL_WiiExt_AllowsMotionPlus(SDL_WII_EXT_UDRAW));
    CHECK(!SDL_WiiExt_AllowsMotionPlus(SDL_WII_EXT_DRAWSOME));
    CHECK(!SDL_WiiExt_AllowsMotionPlus(SDL_WII_EXT_SHINKANSEN));
    CHECK(SDL_WiiExt_AllowsMotionPlus(SDL_WII_EXT_NONE));
    CHECK(SDL_WiiExt_AllowsMotionPlus(SDL_WII_EXT_NUNCHUK));
    CHECK(SDL_WiiExt_AllowsMotionPlus(SDL_WII_EXT_GAMEPAD));
    {
        uint8_t request[SDL_WII_EXT_WRITE_REQUEST_SIZE];
        const uint8_t deactivate = 0x55;
        CHECK(SDL_WiiExt_BuildWriteRequest(request, false, SDL_WII_EXT_REG_INIT1, &deactivate, 1));
        CHECK(IsWrite(request, 0xA400F0, 0x55));
    }
}

/* Guitar test 13 and the shared start-up: the standard pair, zero padded to
   22 bytes, the mode request and the status request */
static void TestRequests(void)
{
    uint8_t request[SDL_WII_EXT_WRITE_REQUEST_SIZE];
    uint8_t mode[SDL_WII_EXT_MODE_REQUEST_SIZE];
    uint8_t status[SDL_WII_EXT_STATUS_REQUEST_SIZE];
    const uint8_t init1 = 0x55, init2 = 0x00;
    uint8_t sixteen[16];

    CHECK(SDL_WiiExt_BuildWriteRequest(request, false, SDL_WII_EXT_REG_INIT1, &init1, 1));
    CHECK(IsWrite(request, 0xA400F0, 0x55));
    CHECK(SDL_WiiExt_BuildWriteRequest(request, false, SDL_WII_EXT_REG_INIT2, &init2, 1));
    CHECK(IsWrite(request, 0xA400FB, 0x00));
    CHECK(SDL_WiiExt_BuildWriteRequest(request, true, SDL_WII_EXT_REG_INIT2, &init2, 1));
    CHECK(request[1] == 0x05);
    memset(sixteen, 0x5A, sizeof(sixteen));
    CHECK(SDL_WiiExt_BuildWriteRequest(request, false, 0xA40024, sixteen, 16));
    CHECK(request[5] == 16 && request[6] == 0x5A && request[21] == 0x5A);
    CHECK(!SDL_WiiExt_BuildWriteRequest(request, false, 0xA40024, sixteen, 17));
    CHECK(!SDL_WiiExt_BuildWriteRequest(request, false, 0xA40024, sixteen, 0));

    SDL_WiiExt_BuildModeRequest(mode, false, 0x32);
    {
        static const uint8_t expected[] = { 0x12, 0x04, 0x32 };
        CHECK(Bytes(mode, expected, sizeof(expected)));
    }
    SDL_WiiExt_BuildModeRequest(mode, true, 0x35);
    {
        static const uint8_t expected[] = { 0x12, 0x05, 0x35 };
        CHECK(Bytes(mode, expected, sizeof(expected)));
    }
    SDL_WiiExt_BuildStatusRequest(status, false);
    CHECK(status[0] == 0x15 && status[1] == 0x00);
    SDL_WiiExt_BuildStatusRequest(status, true);
    CHECK(status[1] == 0x01);

    /* The data report per type. Only the guitar with sensors on uses 0x35 */
    CHECK(SDL_WiiExt_ReportMode(SDL_WII_EXT_GUITAR, false) == 0x32);
    CHECK(SDL_WiiExt_ReportMode(SDL_WII_EXT_GUITAR, true) == 0x35);
    CHECK(SDL_WiiExt_ReportMode(SDL_WII_EXT_DRUMS, true) == 0x32);
    CHECK(SDL_WiiExt_ReportMode(SDL_WII_EXT_TURNTABLE, false) == 0x32);
    CHECK(SDL_WiiExt_ReportMode(SDL_WII_EXT_TAIKO, true) == 0x32);
    CHECK(SDL_WiiExt_ReportMode(SDL_WII_EXT_UDRAW, false) == 0x32);
    CHECK(SDL_WiiExt_ReportMode(SDL_WII_EXT_DRAWSOME, false) == 0x32);
    CHECK(SDL_WiiExt_ReportMode(SDL_WII_EXT_SHINKANSEN, true) == 0x32);

    /* Only the Drawsome writes more at start-up */
    {
        SDL_WiiExtWrite writes[SDL_WII_EXT_MAX_STARTUP_WRITES];
        CHECK(SDL_WiiExt_StartupWrites(SDL_WII_EXT_GUITAR, writes) == 0);
        CHECK(SDL_WiiExt_StartupWrites(SDL_WII_EXT_DRUMS, writes) == 0);
        CHECK(SDL_WiiExt_StartupWrites(SDL_WII_EXT_TURNTABLE, writes) == 0);
        CHECK(SDL_WiiExt_StartupWrites(SDL_WII_EXT_TAIKO, writes) == 0);
        CHECK(SDL_WiiExt_StartupWrites(SDL_WII_EXT_UDRAW, writes) == 0);
        CHECK(SDL_WiiExt_StartupWrites(SDL_WII_EXT_SHINKANSEN, writes) == 0);
        CHECK(SDL_WiiExt_StartupWrites(SDL_WII_EXT_NUNCHUK, writes) == 0);
    }
}

/* Spans per report, full length only */
static void TestSpans(void)
{
    static const struct
    {
        uint8_t id;
        size_t full, offset, count;
        bool accel;
    } layouts[] = {
        { 0x32, 11, 3, 8, false },
        { 0x34, 22, 3, 19, false },
        { 0x35, 22, 6, 16, true },
        { 0x36, 22, 13, 9, false },
        { 0x37, 22, 16, 6, true },
        { 0x3D, 22, 1, 21, false },
    };
    static const uint8_t no_span[] = { 0x20, 0x21, 0x22, 0x30, 0x31, 0x33, 0x3E, 0x3F };
    uint8_t report[22];
    size_t i;

    memset(report, 0, sizeof(report));
    for (i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i) {
        const uint8_t *span = NULL;
        size_t length = 0;
        bool accel = !layouts[i].accel;

        report[0] = layouts[i].id;
        CHECK(SDL_WiiExt_GetSpan(report, layouts[i].full, &span, &length, &accel));
        CHECK(span == &report[layouts[i].offset] && length == layouts[i].count && accel == layouts[i].accel);
        CHECK(layouts[i].offset + layouts[i].count == layouts[i].full);
        /* Windows pads every input report to the longest, and that still reads */
        CHECK(SDL_WiiExt_GetSpan(report, 22, &span, &length, NULL));
        CHECK(!SDL_WiiExt_GetSpan(report, layouts[i].full - 1, &span, &length, NULL));
    }
    for (i = 0; i < sizeof(no_span); ++i) {
        const uint8_t *span = NULL;
        size_t length = 0;
        report[0] = no_span[i];
        CHECK(!SDL_WiiExt_GetSpan(report, sizeof(report), &span, &length, NULL));
    }

    /* Every data report's layout, spanless ones included */
    {
        static const struct
        {
            uint8_t id;
            size_t length;
            bool buttons, accel;
            size_t offset, count;
        } table[] = {
            { 0x30, 3, true, false, 0, 0 },   { 0x31, 6, true, true, 0, 0 },    { 0x32, 11, true, false, 3, 8 },
            { 0x33, 18, true, true, 0, 0 },   { 0x34, 22, true, false, 3, 19 }, { 0x35, 22, true, true, 6, 16 },
            { 0x36, 22, true, false, 13, 9 }, { 0x37, 22, true, true, 16, 6 },  { 0x3D, 22, false, false, 1, 21 },
        };
        SDL_WiiExtLayout layout;
        for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
            CHECK(SDL_WiiExt_GetLayout(table[i].id, &layout));
            CHECK(layout.length == table[i].length && layout.buttons == table[i].buttons && layout.accel == table[i].accel);
            CHECK(layout.span_offset == table[i].offset && layout.span_length == table[i].count);
            /* Buttons, accelerometer, IR and span fill the report exactly */
            if (table[i].count) {
                CHECK(table[i].offset + table[i].count == table[i].length);
            }
        }
        CHECK(!SDL_WiiExt_GetLayout(0x20, &layout));
        CHECK(!SDL_WiiExt_GetLayout(0x3E, &layout));
        CHECK(!SDL_WiiExt_GetLayout(0x3F, &layout));
    }
}

/* ------------------------------------------------------------------------ */
/* Guitar */

static const uint8_t guitar_rest[8] = { 0x20, 0x20, 0x0F, 0x10, 0xFF, 0xFF, 0x00, 0x00 };

static void GuitarRest(SDL_WiiExtState *state, SDL_WiiExtOutput *rest)
{
    SDL_WiiExt_Reset(state, SDL_WII_EXT_GUITAR);
    CHECK(DecodeFull(state, guitar_rest, rest));
}

static void TestGuitar(void)
{
    SDL_WiiExtState state;
    SDL_WiiExtOutput rest, out;

    /* 2. Rest. The first report only sets the stick center. */
    GuitarRest(&state, &rest);
    CHECK(!AxisPosted(&rest, SDL_WII_EXT_AXIS_LEFTX) && !AxisPosted(&rest, SDL_WII_EXT_AXIS_LEFTY));
    CHECK(state.stick[0].center == 0x20 && state.stick[1].center == 0x20);
    CHECK(CountDown(&rest) == 0 && rest.has_hat && rest.hat == 0);
    CHECK(rest.axes[SDL_WII_EXT_AXIS_RIGHTX] == 0);
    CHECK(rest.axes[SDL_WII_EXT_AXIS_RIGHTY] == 128);
    CHECK(rest.axes[SDL_WII_EXT_AXIS_LEFT_TRIGGER] == AXIS_MIN && rest.axes[SDL_WII_EXT_AXIS_RIGHT_TRIGGER] == AXIS_MIN);
    /* No GUIDE and no stick clicks */
    CHECK(!Posted(&rest, 5) && !Posted(&rest, SDL_WII_EXT_BUTTON_LEFT_STICK) && !Posted(&rest, SDL_WII_EXT_BUTTON_RIGHT_STICK));
    /* The second report posts the stick at center */
    CHECK(DecodeFull(&state, guitar_rest, &rest));
    CHECK(AxisPosted(&rest, SDL_WII_EXT_AXIS_LEFTX) && rest.axes[SDL_WII_EXT_AXIS_LEFTX] == 0);
    CHECK(AxisPosted(&rest, SDL_WII_EXT_AXIS_LEFTY) && rest.axes[SDL_WII_EXT_AXIS_LEFTY] == 0);

    /* 3. One control at a time */
    {
        static const struct
        {
            uint8_t e4, e5;
            int button; /* -1: the hat */
            uint8_t hat;
        } cases[] = {
            { 0xFF, 0xEF, SDL_WII_EXT_BUTTON_SOUTH, 0 },
            { 0xFF, 0xBF, SDL_WII_EXT_BUTTON_EAST, 0 },
            { 0xFF, 0xF7, SDL_WII_EXT_BUTTON_WEST, 0 },
            { 0xFF, 0xDF, SDL_WII_EXT_BUTTON_NORTH, 0 },
            { 0xFF, 0x7F, SDL_WII_EXT_BUTTON_LEFT_SHOULDER, 0 },
            { 0xFF, 0xFB, SDL_WII_EXT_BUTTON_RIGHT_SHOULDER, 0 },
            { 0xFF, 0xFE, -1, SDL_WII_EXT_HAT_UP },
            { 0xBF, 0xFF, -1, SDL_WII_EXT_HAT_DOWN },
            { 0xEF, 0xFF, SDL_WII_EXT_BUTTON_BACK, 0 },
            { 0xFB, 0xFF, SDL_WII_EXT_BUTTON_START, 0 },
        };
        size_t i;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            uint8_t e[8];
            memcpy(e, guitar_rest, sizeof(e));
            e[4] = cases[i].e4;
            e[5] = cases[i].e5;
            GuitarRest(&state, &rest);
            CHECK(DecodeFull(&state, guitar_rest, &rest));
            CHECK(DecodeFull(&state, e, &out));
            CHECK(Differences(&rest, &out) == 1);
            if (cases[i].button >= 0) {
                CHECK(Down(&out, cases[i].button) && CountDown(&out) == 1 && out.hat == 0);
            } else {
                CHECK(CountDown(&out) == 0 && out.hat == cases[i].hat);
            }
        }
    }

    /* 4. Touch bar, E0 = 20 */
    {
        static const struct
        {
            uint8_t code;
            int value;
        } cases[] = {
            { 0x04, -27371 }, { 0x07, -20432 }, { 0x0A, -12979 }, { 0x0D, -6554 }, { 0x12, 6810 },
            { 0x14, 12207 }, { 0x17, 18889 }, { 0x1A, 26342 }, { 0x1F, 32767 }, { 0x0F, 128 },
            /* The rest of each band, and the unmeasured low codes */
            { 0x05, -27371 }, { 0x06, -20432 }, { 0x09, -20432 }, { 0x0C, -12979 }, { 0x0E, -6554 },
            { 0x10, -6554 }, { 0x11, -6554 }, { 0x13, 6810 }, { 0x16, 12207 }, { 0x19, 18889 },
            { 0x1E, 26342 }, { 0x00, 128 }, { 0x03, 128 },
        };
        size_t i;
        GuitarRest(&state, &rest);
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            uint8_t e[8];
            memcpy(e, guitar_rest, sizeof(e));
            e[2] = cases[i].code;
            CHECK(DecodeFull(&state, e, &out));
            CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTY] == cases[i].value);
        }
    }

    /* 5. Guitar Hero III: bits 7-6 set, the stick masked, no slider */
    {
        static const uint8_t gh3[8] = { 0xE0, 0xE0, 0x1F, 0x10, 0xFF, 0xFF, 0x00, 0x00 };
        uint8_t e[8];
        SDL_WiiExt_Reset(&state, SDL_WII_EXT_GUITAR);
        CHECK(DecodeFull(&state, gh3, &out));
        CHECK(state.stick[0].center == 0x20 && state.stick[1].center == 0x20);
        CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTY] == 128);
        CHECK(DecodeFull(&state, gh3, &out));
        CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTY] == 128);
        CHECK(out.axes[SDL_WII_EXT_AXIS_LEFTX] == 0 && out.axes[SDL_WII_EXT_AXIS_LEFTY] == 0);
        /* Bit 7 keeps the slider centered whatever E2 says */
        memcpy(e, gh3, sizeof(e));
        e[2] = 0x04;
        CHECK(DecodeFull(&state, e, &out));
        CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTY] == 128);
        /* A World Tour guitar whose bar has only ever read 1F stays centered */
        memcpy(e, guitar_rest, sizeof(e));
        e[2] = 0x1F;
        SDL_WiiExt_Reset(&state, SDL_WII_EXT_GUITAR);
        CHECK(DecodeFull(&state, e, &out));
        CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTY] == 128);
        e[2] = 0x04;
        CHECK(DecodeFull(&state, e, &out));
        CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTY] == -27371);
        e[2] = 0x1F;
        CHECK(DecodeFull(&state, e, &out));
        CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTY] == 32767);
    }

    /* 6. Whammy from a first sample of 10 */
    {
        static const struct
        {
            uint8_t e3;
            int value;
        } steps[] = { { 0x10, 0 }, { 0x1B, 32767 }, { 0x15, 14894 }, { 0x1F, 32767 }, { 0x1B, 24029 }, { 0x0E, 0 } };
        size_t i;
        SDL_WiiExt_Reset(&state, SDL_WII_EXT_GUITAR);
        for (i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
            uint8_t e[8];
            memcpy(e, guitar_rest, sizeof(e));
            e[3] = steps[i].e3;
            CHECK(DecodeFull(&state, e, &out));
            CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTX] == steps[i].value);
        }
        CHECK(state.whammy_lo == 0x0E && state.whammy_largest == 0x1F);

        /* Before any full press the seed sets the scale: lo + 0x0B */
        SDL_WiiExt_Reset(&state, SDL_WII_EXT_GUITAR);
        {
            uint8_t e[8];
            memcpy(e, guitar_rest, sizeof(e));
            CHECK(DecodeFull(&state, e, &out));
            e[3] = 0x15;
            CHECK(DecodeFull(&state, e, &out));
            CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTX] == 14894);
        }
        /* Top bits of E3 are not whammy */
        {
            uint8_t e[8];
            memcpy(e, guitar_rest, sizeof(e));
            e[3] = 0xFF;
            CHECK(DecodeFull(&state, e, &out));
            CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTX] == AXIS_MAX);
        }
    }

    /* 7. Sensors on: report 0x35, the accelerometer and the same controls */
    {
        uint8_t mode[SDL_WII_EXT_MODE_REQUEST_SIZE];
        Report r;
        const uint8_t *span = NULL;
        size_t span_length = 0;
        bool accel = false;
        SDL_WiiExtOutput reference;

        SDL_WiiExt_BuildModeRequest(mode, false, SDL_WiiExt_ReportMode(SDL_WII_EXT_GUITAR, true));
        CHECK(mode[0] == 0x12 && mode[1] == 0x04 && mode[2] == 0x35);

        memset(&r, 0, sizeof(r));
        r.bytes[0] = 0x35;
        r.bytes[3] = r.bytes[4] = r.bytes[5] = 0x80;
        memcpy(&r.bytes[6], guitar_rest, 6);
        r.length = 22;
        CHECK(SDL_WiiExt_GetSpan(r.bytes, r.length, &span, &span_length, &accel));
        CHECK(accel && span_length == 16);
        GuitarRest(&state, &reference);
        SDL_WiiExt_Reset(&state, SDL_WII_EXT_GUITAR);
        CHECK(Decode(&state, &r, r.length, &out));
        CHECK(Differences(&reference, &out) == 0);
        CHECK(memcmp(reference.axes, out.axes, sizeof(out.axes)) == 0);
        CheckTruncations(&state, &r);

        /* A 0x32 report carries no accelerometer, so no sample posts from it */
        r = Report32(guitar_rest);
        accel = true;
        CHECK(SDL_WiiExt_GetSpan(r.bytes, r.length, &span, &span_length, &accel));
        CHECK(!accel);
    }

    /* 8. Truncations of the reports of tests 2 to 6 */
    {
        uint8_t e[8];
        Report r;
        GuitarRest(&state, &rest);
        r = Report32(guitar_rest);
        CheckTruncations(&state, &r);
        memcpy(e, guitar_rest, sizeof(e));
        e[2] = 0x1A;
        e[5] = 0x6E;
        r = Report32(e);
        CheckTruncations(&state, &r);
    }

    /* 9. A failed bus read */
    GuitarRest(&state, &rest);
    CheckAllFFChangesNothing(&state);

    /* 10. Names and types across a hot-plug */
    CHECK(strcmp(SDL_WiiExt_TypeName(SDL_WII_EXT_GUITAR), "Nintendo Wii Remote with Guitar") == 0);
    CHECK(SDL_WiiExt_JoystickType(SDL_WII_EXT_GUITAR) == SDL_WII_EXT_JOYSTICK_TYPE_GUITAR);
    CHECK(strcmp(SDL_WiiExt_TypeName(SDL_WII_EXT_NONE), "Nintendo Wii Remote") == 0);
    CHECK(SDL_WiiExt_JoystickType(SDL_WII_EXT_NONE) == SDL_WII_EXT_JOYSTICK_TYPE_GAMEPAD);

    /* Capabilities */
    {
        SDL_WiiExtCaps caps;
        CHECK(SDL_WiiExt_GetCaps(SDL_WII_EXT_GUITAR, &caps));
        CHECK(caps.nbuttons == 26 && caps.naxes == 6 && caps.nhats == 1 && caps.accel && !caps.mono_led && caps.mapping);
    }
}

/* ------------------------------------------------------------------------ */
/* Drums */

static const uint8_t drums_rest[8] = { 0x20, 0x20, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00 };

static void DrumsFrom(SDL_WiiExtState *state, uint8_t e2, uint8_t e3, uint8_t e4, uint8_t e5, SDL_WiiExtOutput *out)
{
    uint8_t e[8];
    memcpy(e, drums_rest, sizeof(e));
    e[2] = e2;
    e[3] = e3;
    e[4] = e4;
    e[5] = e5;
    CHECK(DecodeFull(state, e, out));
}

static void TestDrums(void)
{
    SDL_WiiExtState state;
    SDL_WiiExtOutput out;
    int i;

    /* 1. The drum kit ID is not the guitar's */
    {
        static const uint8_t id[6] = { 0x01, 0x00, 0xA4, 0x20, 0x01, 0x03 };
        CHECK(ClassifyReply(id, true) == SDL_WII_EXT_DRUMS);
    }

    /* 2. Rest */
    SDL_WiiExt_Reset(&state, SDL_WII_EXT_DRUMS);
    CHECK(DecodeFull(&state, drums_rest, &out));
    CHECK(CountDown(&out) == 0);
    for (i = 6; i <= 12; ++i) {
        CHECK(AxisPosted(&out, i) && out.axes[i] == AXIS_MIN);
    }
    CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTX] == 0 && out.axes[SDL_WII_EXT_AXIS_RIGHTY] == 0);
    CHECK(out.axes[SDL_WII_EXT_AXIS_LEFT_TRIGGER] == AXIS_MIN && out.axes[SDL_WII_EXT_AXIS_RIGHT_TRIGGER] == AXIS_MIN);
    CHECK(!out.has_hat);

    /* 3. Red at velocity 127, held by its pad bit, then released */
    DrumsFrom(&state, 0xB2, 0x0C, 0x7E, 0xFF, &out);
    CHECK(Down(&out, SDL_WII_EXT_BUTTON_EAST) && CountDown(&out) == 1);
    CHECK(out.axes[7] == -129);
    DrumsFrom(&state, 0xFF, 0xFF, 0xFF, 0xBF, &out);
    CHECK(Down(&out, SDL_WII_EXT_BUTTON_EAST) && out.axes[7] == -129);
    DrumsFrom(&state, 0xFF, 0xFF, 0xFF, 0xFF, &out);
    CHECK(!Down(&out, SDL_WII_EXT_BUTTON_EAST) && out.axes[7] == AXIS_MIN);

    /* 4. Red at velocity 32, for that report only */
    DrumsFrom(&state, 0xB3, 0xAD, 0xFF, 0xFF, &out);
    CHECK(Down(&out, SDL_WII_EXT_BUTTON_EAST) && out.axes[7] == -24544);
    DrumsFrom(&state, 0xFF, 0xFF, 0xFF, 0xFF, &out);
    CHECK(!Down(&out, SDL_WII_EXT_BUTTON_EAST) && out.axes[7] == AXIS_MIN);

    /* 5. Each note alone */
    {
        static const struct
        {
            uint8_t e2;
            int button; /* -1: the hi-hat, axis only */
            int axis;
        } notes[] = {
            { 0xB6, SDL_WII_EXT_BUTTON_LEFT_SHOULDER, 10 },
            { 0xB2, SDL_WII_EXT_BUTTON_EAST, 7 },
            { 0xA4, SDL_WII_EXT_BUTTON_SOUTH, 6 },
            { 0xA2, SDL_WII_EXT_BUTTON_NORTH, 9 },
            { 0x9E, SDL_WII_EXT_BUTTON_WEST, 8 },
            { 0x9C, SDL_WII_EXT_BUTTON_RIGHT_SHOULDER, 11 },
            { 0x36, -1, 12 },
        };
        size_t n;
        for (n = 0; n < sizeof(notes) / sizeof(notes[0]); ++n) {
            int a;
            SDL_WiiExt_Reset(&state, SDL_WII_EXT_DRUMS);
            CHECK(DecodeFull(&state, drums_rest, &out));
            DrumsFrom(&state, notes[n].e2, 0x0C, 0x7E, 0xFF, &out);
            if (notes[n].button >= 0) {
                CHECK(Down(&out, notes[n].button) && CountDown(&out) == 1);
            } else {
                CHECK(CountDown(&out) == 0);
            }
            for (a = 6; a <= 12; ++a) {
                CHECK(out.axes[a] == (a == notes[n].axis ? -129 : AXIS_MIN));
            }
        }
        /* The hi-hat value holds after its message */
        DrumsFrom(&state, 0xFF, 0xFF, 0xFF, 0xFF, &out);
        CHECK(out.axes[12] == -129);
    }

    /* 6. A note that is not the kit's */
    SDL_WiiExt_Reset(&state, SDL_WII_EXT_DRUMS);
    CHECK(DecodeFull(&state, drums_rest, &out));
    DrumsFrom(&state, 0x86, 0x0C, 0x7E, 0xFF, &out);
    CHECK(CountDown(&out) == 0);
    for (i = 6; i <= 12; ++i) {
        CHECK(out.axes[i] == AXIS_MIN);
    }

    /* 7. Plus, minus, and a pad bit with no message */
    DrumsFrom(&state, 0xFF, 0xFF, 0xFB, 0xFF, &out);
    CHECK(Down(&out, SDL_WII_EXT_BUTTON_START) && CountDown(&out) == 1);
    DrumsFrom(&state, 0xFF, 0xFF, 0xEF, 0xFF, &out);
    CHECK(Down(&out, SDL_WII_EXT_BUTTON_BACK) && CountDown(&out) == 1);
    DrumsFrom(&state, 0xFF, 0xFF, 0xFF, 0xBF, &out);
    CHECK(Down(&out, SDL_WII_EXT_BUTTON_EAST) && out.axes[7] == AXIS_MIN);

    /* 8. The stick's top bits are masked */
    {
        static const uint8_t e[8] = { 0xE0, 0x60, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00 };
        SDL_WiiExt_Reset(&state, SDL_WII_EXT_DRUMS);
        CHECK(DecodeFull(&state, e, &out));
        CHECK(state.stick[0].center == 0x20 && state.stick[1].center == 0x20);
        CHECK(DecodeFull(&state, drums_rest, &out));
        CHECK(out.axes[SDL_WII_EXT_AXIS_LEFTX] == 0 && out.axes[SDL_WII_EXT_AXIS_LEFTY] == 0);
    }

    /* 9. Truncations and a failed bus read */
    {
        uint8_t e[8];
        Report r;
        SDL_WiiExt_Reset(&state, SDL_WII_EXT_DRUMS);
        CHECK(DecodeFull(&state, drums_rest, &out));
        memcpy(e, drums_rest, sizeof(e));
        e[2] = 0xB2;
        e[3] = 0x0C;
        e[4] = 0x7E;
        r = Report32(e);
        CheckTruncations(&state, &r);
        r = Report32(drums_rest);
        CheckTruncations(&state, &r);
        CheckAllFFChangesNothing(&state);
    }

    /* 10. Names, types and capabilities */
    {
        SDL_WiiExtCaps caps;
        CHECK(strcmp(SDL_WiiExt_TypeName(SDL_WII_EXT_DRUMS), "Nintendo Wii Remote with Drum Kit") == 0);
        CHECK(SDL_WiiExt_JoystickType(SDL_WII_EXT_DRUMS) == SDL_WII_EXT_JOYSTICK_TYPE_DRUM_KIT);
        CHECK(SDL_WiiExt_GetCaps(SDL_WII_EXT_DRUMS, &caps));
        CHECK(caps.nbuttons == 26 && caps.naxes == 13 && caps.nhats == 0 && !caps.accel && caps.mapping);
    }
}

/* ------------------------------------------------------------------------ */
/* Turntable */

static const uint8_t turntable_rest[8] = { 0x20, 0x20, 0x50, 0x00, 0x36, 0xBC, 0x00, 0x00 };

static void TurntableFrom(SDL_WiiExtState *state, const uint8_t *changes, size_t offset, size_t count, SDL_WiiExtOutput *out)
{
    uint8_t e[8];
    memcpy(e, turntable_rest, sizeof(e));
    memcpy(&e[offset], changes, count);
    CHECK(DecodeFull(state, e, out));
}

static void TestTurntable(void)
{
    SDL_WiiExtState state;
    SDL_WiiExtOutput rest, out;

    /* 1. Rest: crossfader 8, dial 16 */
    SDL_WiiExt_Reset(&state, SDL_WII_EXT_TURNTABLE);
    CHECK(DecodeFull(&state, turntable_rest, &rest));
    CHECK(CountDown(&rest) == 0);
    CHECK(rest.axes[SDL_WII_EXT_AXIS_RIGHTX] == 0 && rest.axes[SDL_WII_EXT_AXIS_RIGHTY] == 0);
    CHECK(rest.axes[6] == 2184 && rest.axes[7] == 1056);
    CHECK(rest.axes[SDL_WII_EXT_AXIS_LEFT_TRIGGER] == AXIS_MIN && rest.axes[SDL_WII_EXT_AXIS_RIGHT_TRIGGER] == AXIS_MIN);

    /* 2. Right platter */
    TurntableFrom(&state, (const uint8_t[]){ 0xD0 }, 2, 1, &out);
    CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTY] == 1024);
    TurntableFrom(&state, (const uint8_t[]){ 0xE0, 0xE0, 0xD1 }, 0, 3, &out);
    CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTY] == -1024);
    TurntableFrom(&state, (const uint8_t[]){ 0xE0, 0xE0, 0xD0 }, 0, 3, &out);
    CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTY] == 31744);
    TurntableFrom(&state, (const uint8_t[]){ 0x20, 0x20, 0x51 }, 0, 3, &out);
    CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTY] == -32768);

    /* 3. Left platter */
    TurntableFrom(&state, (const uint8_t[]){ 0x01, 0x36 }, 3, 2, &out);
    CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTX] == 1024);
    TurntableFrom(&state, (const uint8_t[]){ 0x1F, 0x37 }, 3, 2, &out);
    CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTX] == -1024);
    TurntableFrom(&state, (const uint8_t[]){ 0x00, 0x37 }, 3, 2, &out);
    CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHTX] == -32768);

    /* 4. One button at a time */
    {
        static const struct
        {
            size_t offset;
            uint8_t value;
            int button;
            int split; /* -1: no split button */
        } cases[] = {
            { 5, 0x9C, SDL_WII_EXT_BUTTON_SOUTH, 29 },
            { 5, 0xB4, SDL_WII_EXT_BUTTON_SOUTH, 26 },
            { 5, 0x3C, SDL_WII_EXT_BUTTON_WEST, 28 },
            { 5, 0xB8, SDL_WII_EXT_BUTTON_WEST, 31 },
            { 5, 0xAC, SDL_WII_EXT_BUTTON_NORTH, -1 },
            { 4, 0x16, SDL_WII_EXT_BUTTON_EAST, 27 },
            { 4, 0x34, SDL_WII_EXT_BUTTON_EAST, 30 },
            { 4, 0x26, SDL_WII_EXT_BUTTON_BACK, -1 },
            { 4, 0x32, SDL_WII_EXT_BUTTON_START, -1 },
        };
        size_t i;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            TurntableFrom(&state, &cases[i].value, cases[i].offset, 1, &out);
            CHECK(Down(&out, cases[i].button));
            if (cases[i].split >= 0) {
                CHECK(Down(&out, cases[i].split) && CountDown(&out) == 2);
            } else {
                CHECK(CountDown(&out) == 1);
            }
        }
    }

    /* 5. Crossfader and effects dial */
    TurntableFrom(&state, (const uint8_t[]){ 0x40 }, 2, 1, &out);
    CHECK(out.axes[6] == -32768);
    TurntableFrom(&state, (const uint8_t[]){ 0x5E }, 2, 1, &out);
    CHECK(out.axes[6] == 32767);
    TurntableFrom(&state, (const uint8_t[]){ 0x10, 0x00 }, 2, 2, &out);
    CHECK(out.axes[7] == -32768);
    TurntableFrom(&state, (const uint8_t[]){ 0x70, 0xE0 }, 2, 2, &out);
    CHECK(out.axes[7] == 32767);

    /* 6. The Euphoria LED */
    {
        uint8_t request[SDL_WII_EXT_WRITE_REQUEST_SIZE];
        SDL_WiiExtCaps caps;
        SDL_WiiExt_BuildLEDWrite(request, false, true);
        CHECK(IsWrite(request, 0xA400FB, 0x01));
        SDL_WiiExt_BuildLEDWrite(request, false, false);
        CHECK(IsWrite(request, 0xA400FB, 0x00));
        CHECK(SDL_WiiExt_GetCaps(SDL_WII_EXT_TURNTABLE, &caps));
        CHECK(caps.mono_led && caps.nbuttons == 32 && caps.naxes == 8 && caps.nhats == 0 && caps.mapping);
        CHECK(caps.joystick_type == SDL_WII_EXT_JOYSTICK_TYPE_GAMEPAD);
        CHECK(strcmp(caps.name, "Nintendo Wii Remote with DJ Turntable") == 0);
    }

    /* 7. Truncations and a failed bus read */
    {
        Report r = Report32(turntable_rest);
        SDL_WiiExt_Reset(&state, SDL_WII_EXT_TURNTABLE);
        CHECK(DecodeFull(&state, turntable_rest, &out));
        CheckTruncations(&state, &r);
        r.bytes[3 + 2] = 0xD1;
        CheckTruncations(&state, &r);
        CheckAllFFChangesNothing(&state);
    }
}

/* ------------------------------------------------------------------------ */
/* TaTaCon */

static void TestTaiko(void)
{
    static const uint8_t rest[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00 };
    SDL_WiiExtState state;
    SDL_WiiExtOutput out, reference;
    uint8_t e[8];

    /* 2. Rest */
    SDL_WiiExt_Reset(&state, SDL_WII_EXT_TAIKO);
    CHECK(DecodeFull(&state, rest, &reference));
    CHECK(CountDown(&reference) == 0);
    CHECK(reference.axes[SDL_WII_EXT_AXIS_LEFT_TRIGGER] == AXIS_MIN && reference.axes[SDL_WII_EXT_AXIS_RIGHT_TRIGGER] == AXIS_MIN);
    CHECK(reference.axes[0] == 0 && reference.axes[1] == 0 && reference.axes[2] == 0 && reference.axes[3] == 0);

    /* 3. One surface at a time, then all four */
    memcpy(e, rest, sizeof(e));
    e[5] = 0xBF;
    CHECK(DecodeFull(&state, e, &out));
    CHECK(Down(&out, SDL_WII_EXT_BUTTON_LEFT_STICK) && CountDown(&out) == 1 && Differences(&reference, &out) == 1);
    e[5] = 0xEF;
    CHECK(DecodeFull(&state, e, &out));
    CHECK(Down(&out, SDL_WII_EXT_BUTTON_RIGHT_STICK) && CountDown(&out) == 1 && Differences(&reference, &out) == 1);
    e[5] = 0xDF;
    CHECK(DecodeFull(&state, e, &out));
    CHECK(out.axes[SDL_WII_EXT_AXIS_LEFT_TRIGGER] == AXIS_MAX && CountDown(&out) == 0 && Differences(&reference, &out) == 1);
    e[5] = 0xF7;
    CHECK(DecodeFull(&state, e, &out));
    CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHT_TRIGGER] == AXIS_MAX && CountDown(&out) == 0 && Differences(&reference, &out) == 1);
    e[5] = 0x87;
    CHECK(DecodeFull(&state, e, &out));
    CHECK(Down(&out, SDL_WII_EXT_BUTTON_LEFT_STICK) && Down(&out, SDL_WII_EXT_BUTTON_RIGHT_STICK));
    CHECK(out.axes[SDL_WII_EXT_AXIS_LEFT_TRIGGER] == AXIS_MAX && out.axes[SDL_WII_EXT_AXIS_RIGHT_TRIGGER] == AXIS_MAX);

    /* 4. An all-0xFF span releases all four on the TaTaCon */
    {
        static const uint8_t ff[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        CHECK(DecodeFull(&state, ff, &out));
        CHECK(CountDown(&out) == 0);
        CHECK(out.axes[SDL_WII_EXT_AXIS_LEFT_TRIGGER] == AXIS_MIN && out.axes[SDL_WII_EXT_AXIS_RIGHT_TRIGGER] == AXIS_MIN);
    }

    /* 5. E0 to E4 are ignored */
    {
        static const uint8_t noise[8] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xFF, 0x00, 0x00 };
        CHECK(DecodeFull(&state, noise, &out));
        CHECK(Differences(&reference, &out) == 0);
    }

    /* 6. Truncations */
    {
        Report r;
        e[5] = 0x87;
        r = Report32(e);
        CheckTruncations(&state, &r);
    }

    /* 1 and 7: the ID, the name and the shape */
    {
        static const uint8_t id[6] = { 0x00, 0x00, 0xA4, 0x20, 0x01, 0x11 };
        SDL_WiiExtCaps caps;
        CHECK(ClassifyReply(id, true) == SDL_WII_EXT_TAIKO);
        CHECK(SDL_WiiExt_GetCaps(SDL_WII_EXT_TAIKO, &caps));
        CHECK(strcmp(caps.name, "Nintendo Wii Remote with Taiko Drum") == 0);
        CHECK(caps.nbuttons == 26 && caps.naxes == 6 && caps.nhats == 0 && caps.mapping);
        CHECK(caps.joystick_type == SDL_WII_EXT_JOYSTICK_TYPE_GAMEPAD);
    }
}

/* ------------------------------------------------------------------------ */
/* uDraw GameTablet */

static void TestUDraw(void)
{
    SDL_WiiExtState state;
    SDL_WiiExtOutput out, reference;

    /* 1. The ID */
    {
        static const uint8_t id[6] = { 0xFF, 0x00, 0xA4, 0x20, 0x01, 0x12 };
        CHECK(ClassifyReply(id, true) == SDL_WII_EXT_UDRAW);
    }

    /* 2. Pen away */
    SDL_WiiExt_Reset(&state, SDL_WII_EXT_UDRAW);
    {
        static const uint8_t e[8] = { 0xFF, 0xFF, 0xFF, 0x08, 0xFF, 0xFB, 0x00, 0x00 };
        CHECK(DecodeFull(&state, e, &reference));
        CHECK(reference.axes[0] == -1 && reference.axes[1] == -1 && reference.axes[2] == 8);
        CHECK(!Down(&reference, 0) && !Down(&reference, 1) && !Down(&reference, 2));
        CHECK(reference.data_axis_mask == 0x7 && reference.axis_mask == 0x7);
    }

    /* 3. Pen down */
    {
        static const uint8_t e[8] = { 0x23, 0x56, 0x41, 0x80, 0xFF, 0xFB, 0x00, 0x00 };
        CHECK(DecodeFull(&state, e, &out));
        CHECK(out.axes[0] == 291 && out.axes[1] == 1110 && out.axes[2] == 128 && Down(&out, 2));
    }

    /* 4. Pressure bit 8 */
    {
        static const uint8_t e[8] = { 0x23, 0x56, 0x41, 0xF0, 0xFF, 0xFF, 0x00, 0x00 };
        CHECK(DecodeFull(&state, e, &out));
        CHECK(out.axes[2] == 496);
    }

    /* 5. The pen buttons */
    {
        uint8_t e[8] = { 0x23, 0x56, 0x41, 0x80, 0xFF, 0xF9, 0x00, 0x00 };
        CHECK(DecodeFull(&state, e, &out));
        CHECK(Down(&out, 0) && !Down(&out, 1));
        e[5] = 0xFA;
        CHECK(DecodeFull(&state, e, &out));
        CHECK(!Down(&out, 0) && Down(&out, 1));

        /* 6. Bit 3 is not read */
        e[5] = 0xF3;
        CHECK(DecodeFull(&state, e, &reference));
        e[5] = 0xFB;
        CHECK(DecodeFull(&state, e, &out));
        CHECK(Differences(&reference, &out) == 0);
    }

    /* 7. Truncations and a failed bus read */
    {
        static const uint8_t e[8] = { 0x23, 0x56, 0x41, 0x80, 0xFF, 0xFB, 0x00, 0x00 };
        Report r = Report32(e);
        CheckTruncations(&state, &r);
        CheckAllFFChangesNothing(&state);
    }

    /* The shape: no gamepad mapping, raw axes */
    {
        SDL_WiiExtCaps caps;
        CHECK(SDL_WiiExt_GetCaps(SDL_WII_EXT_UDRAW, &caps));
        CHECK(strcmp(caps.name, "Nintendo Wii Remote with uDraw Tablet") == 0);
        CHECK(!caps.mapping && caps.joystick_type == SDL_WII_EXT_JOYSTICK_TYPE_UNKNOWN);
        CHECK(caps.nbuttons == 26 && caps.naxes == 3 && caps.nhats == 0);
    }
}

/* ------------------------------------------------------------------------ */
/* Drawsome */

typedef struct
{
    uint32_t addresses[8];
    uint8_t values[8];
    int count;
    int fail_at; /* The write that goes unacknowledged, or -1 */
} WriteLog;

static bool LogWrite(void *userdata, uint32_t address, uint8_t value)
{
    WriteLog *log = (WriteLog *)userdata;
    if (log->count < 8) {
        log->addresses[log->count] = address;
        log->values[log->count] = value;
    }
    return log->count++ != log->fail_at;
}

static void TestDrawsome(void)
{
    SDL_WiiExtState state;
    SDL_WiiExtOutput out;

    /* 1. The ID, not the uDraw's */
    {
        static const uint8_t id[6] = { 0xFF, 0x00, 0xA4, 0x20, 0x00, 0x13 };
        CHECK(ClassifyReply(id, true) == SDL_WII_EXT_DRAWSOME);
    }

    /* 2. The enable pair after the standard pair, each acknowledged */
    {
        WriteLog log;
        SDL_WiiExtWrite writes[SDL_WII_EXT_MAX_STARTUP_WRITES];
        uint8_t request[SDL_WII_EXT_WRITE_REQUEST_SIZE];

        CHECK(SDL_WiiExt_StartupWrites(SDL_WII_EXT_DRAWSOME, writes) == 2);
        CHECK(writes[0].address == 0xA400FB && writes[0].value == 0x01);
        CHECK(writes[1].address == 0xA400F0 && writes[1].value == 0x55);
        CHECK(SDL_WiiExt_BuildWriteRequest(request, false, writes[0].address, &writes[0].value, 1));
        CHECK(IsWrite(request, 0xA400FB, 0x01));
        CHECK(SDL_WiiExt_BuildWriteRequest(request, false, writes[1].address, &writes[1].value, 1));
        CHECK(IsWrite(request, 0xA400F0, 0x55));

        memset(&log, 0, sizeof(log));
        log.fail_at = -1;
        CHECK(SDL_WiiExt_RunStartup(SDL_WII_EXT_DRAWSOME, LogWrite, &log));
        CHECK(log.count == 2 && log.addresses[0] == 0xA400FB && log.values[0] == 0x01 &&
              log.addresses[1] == 0xA400F0 && log.values[1] == 0x55);

        /* A missing acknowledge stops the sequence */
        memset(&log, 0, sizeof(log));
        log.fail_at = 0;
        CHECK(!SDL_WiiExt_RunStartup(SDL_WII_EXT_DRAWSOME, LogWrite, &log));
        CHECK(log.count == 1);
        memset(&log, 0, sizeof(log));
        log.fail_at = 1;
        CHECK(!SDL_WiiExt_RunStartup(SDL_WII_EXT_DRAWSOME, LogWrite, &log));
        CHECK(log.count == 2);

        /* and leaves the tablet unenabled, posting nothing */
        {
            static const uint8_t e[8] = { 0x34, 0x12, 0x67, 0x05, 0xFF, 0x43, 0x00, 0x00 };
            SDL_WiiExt_Reset(&state, SDL_WII_EXT_DRAWSOME);
            state.enabled = false;
            CHECK(!DecodeFull(&state, e, &out));
            CHECK(out.axis_mask == 0 && out.button_mask == 0);
        }

        /* Every other type needs no writes and succeeds without any */
        memset(&log, 0, sizeof(log));
        log.fail_at = 0;
        CHECK(SDL_WiiExt_RunStartup(SDL_WII_EXT_GUITAR, LogWrite, &log));
        CHECK(log.count == 0);
    }

    SDL_WiiExt_Reset(&state, SDL_WII_EXT_DRAWSOME);

    /* 3. Before enable, as raphnet read it */
    {
        static const uint8_t e[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00 };
        CHECK(DecodeFull(&state, e, &out));
        CHECK(out.axes[0] == -1 && out.axes[1] == -1 && out.axes[2] == 0 && !Down(&out, 2));
    }

    /* 4. Pen in range */
    {
        static const uint8_t e[8] = { 0x34, 0x12, 0x67, 0x05, 0xFF, 0x43, 0x00, 0x00 };
        CHECK(DecodeFull(&state, e, &out));
        CHECK(out.axes[0] == 4660 && out.axes[1] == 1383 && out.axes[2] == 1023 && Down(&out, 2));
        CHECK(!Down(&out, 0) && !Down(&out, 1));
        CHECK(out.data_axis_mask == 0x7);
    }

    /* 5. Pen lifted */
    {
        static const uint8_t e[8] = { 0x34, 0x12, 0x67, 0x05, 0x00, 0xC0, 0x00, 0x00 };
        CHECK(DecodeFull(&state, e, &out));
        CHECK(out.axes[0] == -1 && out.axes[1] == -1 && !Down(&out, 2));
    }

    /* 6. The 12-bit pressure field */
    {
        static const uint8_t e[8] = { 0x34, 0x12, 0x67, 0x05, 0xFF, 0x4F, 0x00, 0x00 };
        CHECK(DecodeFull(&state, e, &out));
        CHECK(out.axes[2] == 4095);
    }

    /* Coordinates clamp to the axis range */
    {
        static const uint8_t e[8] = { 0xFF, 0xFF, 0xFF, 0x90, 0x00, 0x40, 0x00, 0x00 };
        CHECK(DecodeFull(&state, e, &out));
        CHECK(out.axes[0] == 32767 && out.axes[1] == 32767);
    }

    /* 8. Truncations and a failed bus read */
    {
        static const uint8_t e[8] = { 0x34, 0x12, 0x67, 0x05, 0xFF, 0x43, 0x00, 0x00 };
        Report r = Report32(e);
        CheckTruncations(&state, &r);
        CheckAllFFChangesNothing(&state);
    }

    {
        SDL_WiiExtCaps caps;
        CHECK(SDL_WiiExt_GetCaps(SDL_WII_EXT_DRAWSOME, &caps));
        CHECK(strcmp(caps.name, "Nintendo Wii Remote with Drawsome Tablet") == 0);
        CHECK(!caps.mapping && caps.joystick_type == SDL_WII_EXT_JOYSTICK_TYPE_UNKNOWN);
        CHECK(caps.nbuttons == 26 && caps.naxes == 3 && caps.nhats == 0);
    }
}

/* ------------------------------------------------------------------------ */
/* Shinkansen */

static const uint8_t shinkansen_rest[8] = { 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0xFF };

static void TestShinkansen(void)
{
    SDL_WiiExtState state;
    SDL_WiiExtOutput rest, out;

    /* 1. Format 03 is the Shinkansen. Dolphin's 01 10 is not. */
    {
        static const uint8_t id[6] = { 0x00, 0x00, 0xA4, 0x20, 0x03, 0x10 };
        static const uint8_t placeholder[6] = { 0x00, 0x00, 0xA4, 0x20, 0x01, 0x10 };
        CHECK(ClassifyReply(id, true) == SDL_WII_EXT_SHINKANSEN);
        CHECK(ClassifyReply(placeholder, true) == SDL_WII_EXT_UNKNOWN);
    }

    /* 2. Rest */
    SDL_WiiExt_Reset(&state, SDL_WII_EXT_SHINKANSEN);
    CHECK(DecodeFull(&state, shinkansen_rest, &rest));
    CHECK(CountDown(&rest) == 0 && rest.has_hat && rest.hat == 0);
    CHECK(rest.axes[SDL_WII_EXT_AXIS_LEFT_TRIGGER] == AXIS_MIN && rest.axes[SDL_WII_EXT_AXIS_RIGHT_TRIGGER] == AXIS_MIN);
    CHECK(rest.axes[0] == 0 && rest.axes[1] == 0 && rest.axes[2] == 0 && rest.axes[3] == 0);
    CHECK(rest.data_axis_mask == ((1u << 6) | (1u << 7)));

    /* 3. Brake notches, and values between them */
    {
        static const struct
        {
            uint8_t raw;
            int value;
        } brake[] = {
            { 0x00, -32768 }, { 0x35, -24576 }, { 0x4F, -16384 }, { 0x69, -8192 }, { 0x84, 0 },
            { 0x9F, 8191 }, { 0xBB, 16383 }, { 0xD9, 24575 }, { 0xFA, 32767 },
            { 0x36, -24576 }, { 0x42, -24576 }, { 0x43, -16384 },
        };
        size_t i;
        for (i = 0; i < sizeof(brake) / sizeof(brake[0]); ++i) {
            uint8_t e[8];
            memcpy(e, shinkansen_rest, sizeof(e));
            e[2] = brake[i].raw;
            CHECK(DecodeFull(&state, e, &out));
            CHECK(out.axes[SDL_WII_EXT_AXIS_LEFT_TRIGGER] == brake[i].value);
            CHECK(out.axes[6] == brake[i].raw * 257 - 32768);
        }
    }

    /* 4. Power notches */
    {
        static const struct
        {
            uint8_t raw;
            int value;
        } power[] = {
            { 0xFF, -32768 }, { 0xE5, -27727 }, { 0xD0, -22686 }, { 0xBD, -17645 }, { 0xAA, -12603 },
            { 0x99, -7562 }, { 0x87, -2521 }, { 0x76, 2520 }, { 0x65, 7561 }, { 0x55, 12602 },
            { 0x44, 17644 }, { 0x33, 22685 }, { 0x23, 27726 }, { 0x11, 32767 },
            { 0xF0, -27727 }, { 0x00, 32767 },
        };
        size_t i;
        for (i = 0; i < sizeof(power) / sizeof(power[0]); ++i) {
            uint8_t e[8];
            memcpy(e, shinkansen_rest, sizeof(e));
            e[3] = power[i].raw;
            CHECK(DecodeFull(&state, e, &out));
            CHECK(out.axes[SDL_WII_EXT_AXIS_RIGHT_TRIGGER] == power[i].value);
            CHECK(out.axes[7] == power[i].raw * 257 - 32768);
        }
    }

    /* 5. One button at a time */
    {
        static const struct
        {
            size_t offset;
            uint8_t value;
            int button; /* -1: the hat */
            uint8_t hat;
        } cases[] = {
            { 6, 0xFB, SDL_WII_EXT_BUTTON_START, 0 },
            { 6, 0xEF, SDL_WII_EXT_BUTTON_BACK, 0 },
            { 6, 0xBF, -1, SDL_WII_EXT_HAT_DOWN },
            { 6, 0x7F, -1, SDL_WII_EXT_HAT_RIGHT },
            { 7, 0xFE, -1, SDL_WII_EXT_HAT_UP },
            { 7, 0xFD, -1, SDL_WII_EXT_HAT_LEFT },
            { 7, 0xBF, SDL_WII_EXT_BUTTON_SOUTH, 0 },
            { 7, 0xEF, SDL_WII_EXT_BUTTON_EAST, 0 },
            { 7, 0xDF, SDL_WII_EXT_BUTTON_WEST, 0 },
            { 7, 0xF7, SDL_WII_EXT_BUTTON_NORTH, 0 },
            { 7, 0xFB, SDL_WII_EXT_BUTTON_RIGHT_SHOULDER, 0 },
        };
        size_t i;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            uint8_t e[8];
            memcpy(e, shinkansen_rest, sizeof(e));
            e[cases[i].offset] = cases[i].value;
            CHECK(DecodeFull(&state, e, &out));
            CHECK(Differences(&rest, &out) == 1);
            if (cases[i].button >= 0) {
                CHECK(Down(&out, cases[i].button) && CountDown(&out) == 1 && out.hat == 0);
            } else {
                CHECK(CountDown(&out) == 0 && out.hat == cases[i].hat);
            }
        }
        /* The Classic bits with no Shinkansen control post nothing */
        {
            uint8_t e[8];
            memcpy(e, shinkansen_rest, sizeof(e));
            e[6] = (uint8_t)~(0x20 | 0x08 | 0x02);
            e[7] = (uint8_t)~0x80;
            CHECK(DecodeFull(&state, e, &out));
            CHECK(Differences(&rest, &out) == 0);
        }
    }

    /* 6. A 0x37 report's six bytes never update it, and its mode is 0x32 */
    {
        Report r;
        const uint8_t *span = NULL;
        size_t span_length = 0;
        memset(&r, 0, sizeof(r));
        r.bytes[0] = 0x37;
        memcpy(&r.bytes[16], shinkansen_rest, 6);
        r.length = 22;
        CHECK(SDL_WiiExt_GetSpan(r.bytes, r.length, &span, &span_length, NULL) && span_length == 6);
        {
            SDL_WiiExtState before = state;
            CHECK(!Decode(&state, &r, r.length, &out));
            CHECK(memcmp(&before, &state, sizeof(before)) == 0);
        }
        CHECK(SDL_WiiExt_ReportMode(SDL_WII_EXT_SHINKANSEN, false) == 0x32);
    }

    /* 7. Truncations and a failed bus read */
    {
        Report r = Report32(shinkansen_rest);
        CheckTruncations(&state, &r);
        CheckAllFFChangesNothing(&state);
    }

    {
        SDL_WiiExtCaps caps;
        CHECK(SDL_WiiExt_GetCaps(SDL_WII_EXT_SHINKANSEN, &caps));
        CHECK(strcmp(caps.name, "Nintendo Wii Remote with Shinkansen Controller") == 0);
        CHECK(caps.nbuttons == 26 && caps.naxes == 8 && caps.nhats == 1 && caps.mapping);
        CHECK(caps.joystick_type == SDL_WII_EXT_JOYSTICK_TYPE_GAMEPAD);
    }
}

/* ------------------------------------------------------------------------ */

static void TestStateless(void)
{
    SDL_WiiExtState state;
    SDL_WiiExtOutput out;
    static const uint8_t e[8] = { 0x20, 0x20, 0x0F, 0x10, 0xFF, 0xFF, 0x00, 0x00 };

    /* Types this module does not decode, and a span too short */
    SDL_WiiExt_Reset(&state, SDL_WII_EXT_NUNCHUK);
    CHECK(!SDL_WiiExt_Decode(&state, e, sizeof(e), &out));
    SDL_WiiExt_Reset(&state, SDL_WII_EXT_GUITAR);
    CHECK(!SDL_WiiExt_Decode(&state, e, 5, &out));
    CHECK(SDL_WiiExt_Decode(&state, e, 6, &out));
    CHECK(!SDL_WiiExt_Decode(NULL, e, 6, &out));
    CHECK(!SDL_WiiExt_Decode(&state, NULL, 6, &out));
    {
        SDL_WiiExtCaps caps;
        CHECK(!SDL_WiiExt_GetCaps(SDL_WII_EXT_NUNCHUK, &caps));
        CHECK(!SDL_WiiExt_GetCaps(SDL_WII_EXT_GUITAR, NULL));
    }
    CHECK(strcmp(SDL_WiiExt_TypeName(SDL_WII_EXT_UNKNOWN), "Nintendo Wii Remote with Unknown Extension") == 0);
    CHECK(strcmp(SDL_WiiExt_TypeName(SDL_WII_EXT_NUNCHUK), "Nintendo Wii Remote with Nunchuk") == 0);
    CHECK(strcmp(SDL_WiiExt_TypeName(SDL_WII_EXT_GAMEPAD), "Nintendo Wii Remote with Classic Controller") == 0);
    CHECK(strcmp(SDL_WiiExt_TypeName(SDL_WII_EXT_WIIUPRO), "Nintendo Wii U Pro Controller") == 0);
    CHECK(strcmp(SDL_WiiExt_TypeName(SDL_WII_EXT_BALANCEBOARD), "Nintendo Wii Balance Board") == 0);
    CHECK(SDL_WiiExt_JoystickType(SDL_WII_EXT_UNKNOWN) == SDL_WII_EXT_JOYSTICK_TYPE_GAMEPAD);
}

int main(void)
{
    TestIdentify();
    TestClassification();
    TestStoredId();
    TestRequests();
    TestSpans();
    TestGuitar();
    TestDrums();
    TestTurntable();
    TestTaiko();
    TestUDraw();
    TestDrawsome();
    TestShinkansen();
    TestStateless();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
