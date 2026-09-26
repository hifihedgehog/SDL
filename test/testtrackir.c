/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_trackir_proto.c, the
   NaturalPoint TrackIR 2 and TrackIR 3 of hifihedgehog/SDL#33 Part 14, and
   for their vendor rules in src/hidapi/SDL_hidapi_vendorusb.c. Test numbers
   follow the part. No capture of either camera exists, so every stream is
   constructed from linuxtrack's parser and blob code, and every expected
   axis was worked out from linuxtrack's formulas with exact fractions. The
   information answer is the one linuxtrack's fake device returns
   (src/fakeusb.c:159). */

#include "../src/joystick/hidapi/SDL_hidapi_trackir_proto.h"
#include "../src/hidapi/SDL_hidapi_vendorusb.h"
#include "../src/joystick/usb_ids.h"

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

static void *Allocate(size_t size)
{
    void *memory = calloc(1, size ? size : 1);

    if (!memory) {
        printf("FAILED: out of memory\n");
        exit(1);
    }
    return memory;
}

static SDL_TrackIRState *NewState(void)
{
    return (SDL_TrackIRState *)Allocate(sizeof(SDL_TrackIRState));
}

/* Feeds from an exact-size heap copy, so a read past the length reaches
   the sanitizer */
static bool Feed(SDL_TrackIRState *state, const uint8_t *data, size_t length, uint64_t now)
{
    uint8_t *copy = (uint8_t *)Allocate(length);
    bool result;

    if (length) {
        memcpy(copy, data, length);
    }
    result = SDL_TrackIR_Feed(state, copy, length, now);
    free(copy);
    return result;
}

static bool Shows(const SDL_TrackIRState *state, bool in_view, int x, int y, int size)
{
    return state->in_view == in_view && state->axes[0] == x && state->axes[1] == y && state->axes[2] == size;
}

static bool Idle(const SDL_TrackIRState *state)
{
    return state->packet_length == 0 && state->skip == 0;
}

static bool SameBlob(const SDL_TrackIRBlob *a, const SDL_TrackIRBlob *b)
{
    return a->pixels == b->pixels && a->sum_x == b->sum_x && a->sum_y == b->sum_y;
}

/* The joystick's output and the frame under construction */
static bool SameFrame(const SDL_TrackIRState *a, const SDL_TrackIRState *b)
{
    int list, i;

    if (a->frames != b->frames || a->in_view != b->in_view || a->axes[0] != b->axes[0] ||
        a->axes[1] != b->axes[1] || a->axes[2] != b->axes[2]) {
        return false;
    }
    if (a->frame_line != b->frame_line || a->blob_line != b->blob_line || a->previous != b->previous ||
        a->have_best != b->have_best || (a->have_best && !SameBlob(&a->best, &b->best))) {
        return false;
    }
    for (list = 0; list < 2; ++list) {
        if (a->range_count[list] != b->range_count[list]) {
            return false;
        }
        for (i = 0; i < a->range_count[list]; ++i) {
            const SDL_TrackIRRange *ra = &a->ranges[list][i];
            const SDL_TrackIRRange *rb = &b->ranges[list][i];

            if (ra->first != rb->first || ra->last != rb->last ||
                !SameBlob(&a->blobs[ra->blob], &b->blobs[rb->blob])) {
                return false;
            }
        }
    }
    return true;
}

/* A growing byte stream for building reads */
typedef struct Stream
{
    uint8_t *data;
    size_t length;
    size_t capacity;
} Stream;

static void PutBytes(Stream *stream, const uint8_t *bytes, size_t count)
{
    if (stream->length + count > stream->capacity) {
        size_t capacity = stream->capacity ? stream->capacity : 256;
        uint8_t *grown;

        while (capacity < stream->length + count) {
            capacity *= 2;
        }
        grown = (uint8_t *)realloc(stream->data, capacity);
        if (!grown) {
            printf("FAILED: out of memory\n");
            exit(1);
        }
        stream->data = grown;
        stream->capacity = capacity;
    }
    memcpy(stream->data + stream->length, bytes, count);
    stream->length += count;
}

static void PutByte(Stream *stream, uint8_t byte)
{
    PutBytes(stream, &byte, 1);
}

static void FreeStream(Stream *stream)
{
    free(stream->data);
    memset(stream, 0, sizeof(*stream));
}

typedef struct Stripe
{
    uint32_t line;
    uint32_t first;
    uint32_t last;
} Stripe;

/* One stripe as the camera encodes it: 3 bytes on the TrackIR 2, and on
   the TrackIR 3 a fourth whose bits 20, 80, 10, 40 and 08 carry line bit 8,
   first x bits 8 and 9 and last x bits 8 and 9 */
static size_t EncodeStripe(SDL_TrackIRModel model, const Stripe *stripe, uint8_t out[4])
{
    out[0] = (uint8_t)(stripe->line & 0xFF);
    out[1] = (uint8_t)(stripe->first & 0xFF);
    out[2] = (uint8_t)(stripe->last & 0xFF);
    if (model == SDL_TRACKIR_2) {
        return 3;
    }
    out[3] = 0;
    if (stripe->line & 0x100) {
        out[3] |= 0x20;
    }
    if (stripe->first & 0x100) {
        out[3] |= 0x80;
    }
    if (stripe->first & 0x200) {
        out[3] |= 0x10;
    }
    if (stripe->last & 0x100) {
        out[3] |= 0x40;
    }
    if (stripe->last & 0x200) {
        out[3] |= 0x08;
    }
    return 4;
}

/* Stripe packets of at most per_packet stripes. A packet of size 3E is
   followed by two bytes, FF FF, which a parser that did not skip them would
   take for a header of an unknown type. */
static void PutStripes(Stream *stream, SDL_TrackIRModel model, const Stripe *stripes, size_t count, size_t per_packet)
{
    const size_t stride = (model == SDL_TRACKIR_2) ? 3 : 4;
    size_t i = 0;

    while (i < count) {
        size_t n = count - i;
        size_t k;

        if (n > per_packet) {
            n = per_packet;
        }
        PutByte(stream, (uint8_t)(2 + n * stride));
        PutByte(stream, 0x1C);
        for (k = 0; k < n; ++k) {
            uint8_t bytes[4];

            PutBytes(stream, bytes, EncodeStripe(model, &stripes[i + k], bytes));
        }
        if (2 + n * stride == 0x3E) {
            PutByte(stream, 0xFF);
            PutByte(stream, 0xFF);
        }
        i += n;
    }
}

/* A stripe packet holding only the all-zero stripe that ends a frame */
static void PutZero(Stream *stream, SDL_TrackIRModel model)
{
    static const uint8_t zero2[5] = { 0x05, 0x1C, 0x00, 0x00, 0x00 };
    static const uint8_t zero3[6] = { 0x06, 0x1C, 0x00, 0x00, 0x00, 0x00 };

    if (model == SDL_TRACKIR_2) {
        PutBytes(stream, zero2, sizeof(zero2));
    } else {
        PutBytes(stream, zero3, sizeof(zero3));
    }
}

/* The end of the last whole packet among a read's first n bytes. An unknown
   type or a size under 2 ends the walk, since the rest of that read is
   dropped. The two bytes after a stripe packet of size 3E are skipped. */
static size_t WholeLength(const uint8_t *read, size_t n)
{
    size_t position = 0, end = 0;

    while (position + 2 <= n) {
        const size_t size = read[position];
        const uint8_t type = read[position + 1];

        if (size < 2 || (type != 0x1C && type != 0x20 && type != 0x40) || position + size > n) {
            break;
        }
        position += size;
        end = position;
        if (type == 0x1C && size == 0x3E) {
            position += 2;
        }
    }
    return end;
}

/* Status packets that fill exactly count bytes, count at least 2 */
static void Filler(uint8_t *out, size_t count)
{
    size_t i = 0;

    if (count & 1) {
        out[0] = 0x03;
        out[1] = 0x20;
        out[2] = 0x00;
        i = 3;
    }
    for (; i < count; i += 2) {
        out[i] = 0x02;
        out[i + 1] = 0x20;
    }
}

/* The command rig: runs the driver's update loop one millisecond at a
   time, the reads first and then the writes, and records every write */

#define MAX_SENT 64

typedef struct Sent
{
    uint64_t at;
    uint8_t data[SDL_TRACKIR_COMMAND_MAX];
    size_t length;
} Sent;

typedef struct Planned
{
    uint64_t at;
    const uint8_t *data;
    size_t length;
} Planned;

typedef struct Rig
{
    SDL_TrackIRState *state;
    Sent sent[MAX_SENT];
    int count;
    int fail;          /* The write with this index fails, or -1 */
    uint64_t write_ms; /* How long each write takes */
    int answer;        /* The camera answers each 17 01 this many ms later, or -1 */
    bool answer_pending;
    uint64_t answer_at;
    const Planned *planned;
    size_t planned_count;
} Rig;

static const uint8_t info_answer[9] = { 0x09, 0x40, 0x03, 0x00, 0x00, 0x34, 0x5D, 0x03, 0x00 };
static const uint8_t status_read[7] = { 0x07, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00 };

static void InitRig(Rig *rig, SDL_TrackIRState *state, SDL_TrackIRModel model, uint64_t now)
{
    memset(rig, 0, sizeof(*rig));
    rig->state = state;
    rig->fail = -1;
    rig->answer = -1;
    SDL_TrackIR_Init(state, model, now);
}

static bool IsRequest(const SDL_TrackIRCommand *command)
{
    return command->length == 2 && command->data[0] == 0x17 && command->data[1] == 0x01;
}

static void Commands(Rig *rig, uint64_t now)
{
    SDL_TrackIRCommand command;

    while (SDL_TrackIR_NextCommand(rig->state, now, &command)) {
        const bool written = (rig->count != rig->fail);

        CHECK(command.length >= 1 && command.length <= SDL_TRACKIR_COMMAND_MAX);
        if (rig->count < MAX_SENT) {
            rig->sent[rig->count].at = now;
            memcpy(rig->sent[rig->count].data, command.data, sizeof(command.data));
            rig->sent[rig->count].length = command.length;
        }
        if (rig->answer >= 0 && IsRequest(&command)) {
            rig->answer_pending = true;
            rig->answer_at = now + (uint64_t)rig->answer;
        }
        ++rig->count;
        /* Nothing more until the command in flight completes */
        {
            SDL_TrackIRCommand again;

            CHECK(!SDL_TrackIR_NextCommand(rig->state, now, &again));
        }
        SDL_TrackIR_CommandDone(rig->state, written, now + rig->write_ms);
    }
}

static void Tick(Rig *rig, uint64_t t)
{
    size_t i;

    for (i = 0; i < rig->planned_count; ++i) {
        if (rig->planned[i].at == t) {
            Feed(rig->state, rig->planned[i].data, rig->planned[i].length, t);
        }
    }
    if (rig->answer_pending && rig->answer_at == t) {
        rig->answer_pending = false;
        Feed(rig->state, info_answer, sizeof(info_answer), t);
    }
    Commands(rig, t);
}

static void Run(Rig *rig, uint64_t from, uint64_t to)
{
    uint64_t t;

    for (t = from; t <= to; ++t) {
        Tick(rig, t);
    }
}

/* Runs the start-up with a camera that answers a millisecond after each
   request and returns the time the camera runs */
static uint64_t StartRunning(SDL_TrackIRState *state, SDL_TrackIRModel model)
{
    Rig *rig = (Rig *)Allocate(sizeof(Rig));
    uint64_t t;

    InitRig(rig, state, model, 0);
    rig->answer = 1;
    for (t = 0; t < 100000; ++t) {
        Tick(rig, t);
        if (SDL_TrackIR_IsRunning(state)) {
            break;
        }
    }
    free(rig);
    return t;
}

typedef struct Expected
{
    uint64_t at;
    size_t length;
    uint8_t data[SDL_TRACKIR_COMMAND_MAX];
} Expected;

#define MAX_EXPECTED 64

static int Add(Expected *list, int n, uint64_t at, size_t length, const uint8_t *bytes)
{
    if (n < MAX_EXPECTED) {
        memset(&list[n], 0, sizeof(list[n]));
        list[n].at = at;
        list[n].length = length;
        memcpy(list[n].data, bytes, length);
    }
    return n + 1;
}

static const uint8_t c_ir_off[3] = { 0x10, 0x00, 0x80 };
static const uint8_t c_red_off[3] = { 0x10, 0x00, 0x10 };
static const uint8_t c_green_off[3] = { 0x10, 0x00, 0x20 };
static const uint8_t c_blue_off[3] = { 0x10, 0x00, 0x40 };
static const uint8_t c_ir_on[3] = { 0x10, 0x80, 0x80 };
static const uint8_t c_video_off[2] = { 0x14, 0x01 };
static const uint8_t c_video_on[2] = { 0x14, 0x00 };
static const uint8_t c_fifo[1] = { 0x12 };
static const uint8_t c_stop[1] = { 0x13 };
static const uint8_t c_request[2] = { 0x17, 0x01 };
static const uint8_t c_threshold2[3] = { 0x15, 0x8C, 0x01 };
static const uint8_t c_threshold3[4] = { 0x15, 0x8C, 0x01, 0x00 };
static const uint8_t c_threshold3_d0[4] = { 0x15, 0xD0, 0x01, 0x00 };
static const uint8_t c_unknown5[6] = { 0x23, 0x40, 0x1C, 0x5E, 0x00, 0x00 };
static const uint8_t c_unknown6[6] = { 0x23, 0x40, 0x1D, 0x01, 0x00, 0x00 };
static const uint8_t c_unknown8[5] = { 0x19, 0x03, 0x03, 0x00, 0x00 };
static const uint8_t c_unknown4[5] = { 0x19, 0x15, 0x10, 0x40, 0x00 };

/* tir_hw.c:786-803: the four lights off, video off, the four lights off */
static int Prologue2(Expected *list, uint64_t t)
{
    int n = 0;

    n = Add(list, n, t + 0, 3, c_ir_off);
    n = Add(list, n, t + 4, 3, c_red_off);
    n = Add(list, n, t + 8, 3, c_green_off);
    n = Add(list, n, t + 12, 3, c_blue_off);
    n = Add(list, n, t + 16, 2, c_video_off);
    n = Add(list, n, t + 18, 3, c_ir_off);
    n = Add(list, n, t + 22, 3, c_red_off);
    n = Add(list, n, t + 26, 3, c_green_off);
    n = Add(list, n, t + 30, 3, c_blue_off);
    return n; /* The flush begins at t + 34 */
}

/* tir_hw.c:811-813, :676-691 and tir_driver.c:104-109, from the end of the
   70 ms after the information wait */
static int Epilogue2(Expected *list, int n, uint64_t t)
{
    n = Add(list, n, t + 0, 3, c_threshold2);
    n = Add(list, n, t + 2, 2, c_video_on);
    n = Add(list, n, t + 122, 3, c_ir_on);
    n = Add(list, n, t + 188, 1, c_fifo);
    n = Add(list, n, t + 252, 3, c_ir_on);
    n = Add(list, n, t + 318, 3, c_threshold2);
    return n; /* Running at t + 318 */
}

/* tir_hw.c:564-585 */
static int Prologue3(Expected *list, uint64_t t)
{
    int n = 0;

    n = Add(list, n, t + 0, 2, c_video_off);
    n = Add(list, n, t + 2, 2, c_video_off);
    n = Add(list, n, t + 4, 3, c_ir_off);
    n = Add(list, n, t + 8, 1, c_fifo);
    n = Add(list, n, t + 10, 1, c_stop);
    n = Add(list, n, t + 12, 3, c_red_off);
    n = Add(list, n, t + 16, 3, c_green_off);
    n = Add(list, n, t + 20, 3, c_blue_off);
    return n; /* The flush begins at t + 72 */
}

/* tir_hw.c:834-844, :698-707 and tir_driver.c:104-109 */
static int Epilogue3(Expected *list, int n, uint64_t t)
{
    n = Add(list, n, t + 0, 4, c_threshold3);
    n = Add(list, n, t + 70, 6, c_unknown5);
    n = Add(list, n, t + 72, 6, c_unknown6);
    n = Add(list, n, t + 74, 4, c_threshold3_d0);
    n = Add(list, n, t + 76, 5, c_unknown8);
    n = Add(list, n, t + 78, 1, c_fifo);
    n = Add(list, n, t + 80, 1, c_stop);
    n = Add(list, n, t + 82, 3, c_ir_on);
    n = Add(list, n, t + 86, 2, c_video_on);
    n = Add(list, n, t + 88, 4, c_threshold3);
    return n; /* Running at t + 88 */
}

static void CheckSent(const Rig *rig, const Expected *expected, int count)
{
    int i;

    CHECK(rig->count == count);
    if (rig->count != count) {
        printf("  %d commands, expected %d\n", rig->count, count);
    }
    for (i = 0; i < count && i < rig->count && i < MAX_SENT; ++i) {
        const Sent *sent = &rig->sent[i];
        const bool same = sent->at == expected[i].at && sent->length == expected[i].length &&
                          memcmp(sent->data, expected[i].data, expected[i].length) == 0;

        if (!same) {
            printf("  command %d: %u bytes at %u, expected %u bytes at %u\n", i, (unsigned int)sent->length,
                   (unsigned int)sent->at, (unsigned int)expected[i].length, (unsigned int)expected[i].at);
        }
        CHECK(same);
        /* The bytes past the command are zero */
        if (sent->length < SDL_TRACKIR_COMMAND_MAX) {
            CHECK(sent->data[SDL_TRACKIR_COMMAND_MAX - 1] == 0);
        }
    }
}

/* Runs a rig until the camera runs, and checks it runs at exactly that
   time */
static void RunUntilRunning(Rig *rig, uint64_t from, uint64_t running_at)
{
    Run(rig, from, running_at - 1);
    CHECK(!SDL_TrackIR_IsRunning(rig->state));
    Tick(rig, running_at);
    CHECK(SDL_TrackIR_IsRunning(rig->state));
}

static void TestIdentity(void)
{
    CHECK(SDL_TRACKIR_VENDOR == USB_VENDOR_NATURALPOINT);
    CHECK(SDL_TRACKIR2_PRODUCT == USB_PRODUCT_NATURALPOINT_TRACKIR2);
    CHECK(SDL_TRACKIR3_PRODUCT == USB_PRODUCT_NATURALPOINT_TRACKIR3);
    /* libusb_ifc.c:64, :177-197 */
    CHECK(SDL_TrackIR_Identify(0x131D, 0x0150) == SDL_TRACKIR_2);
    CHECK(SDL_TrackIR_Identify(0x131D, 0x0155) == SDL_TRACKIR_3);
    /* The TrackIR 4, the TrackIR 5 revisions and the SmartNavs need
       firmware or another protocol */
    CHECK(SDL_TrackIR_Identify(0x131D, 0x0156) == SDL_TRACKIR_NONE);
    CHECK(SDL_TrackIR_Identify(0x131D, 0x0157) == SDL_TRACKIR_NONE);
    CHECK(SDL_TrackIR_Identify(0x131D, 0x0159) == SDL_TRACKIR_NONE);
    CHECK(SDL_TrackIR_Identify(0x131D, 0x0105) == SDL_TRACKIR_NONE);
    CHECK(SDL_TrackIR_Identify(0x131D, 0x0106) == SDL_TRACKIR_NONE);
    CHECK(SDL_TrackIR_Identify(0x131C, 0x0150) == SDL_TRACKIR_NONE);
    CHECK(SDL_TrackIR_Identify(0x131E, 0x0155) == SDL_TRACKIR_NONE);
    CHECK(SDL_TrackIR_Name(SDL_TRACKIR_2) && strcmp(SDL_TrackIR_Name(SDL_TRACKIR_2), "NaturalPoint TrackIR 2") == 0);
    CHECK(SDL_TrackIR_Name(SDL_TRACKIR_3) && strcmp(SDL_TrackIR_Name(SDL_TRACKIR_3), "NaturalPoint TrackIR 3") == 0);
    CHECK(SDL_TrackIR_Name(SDL_TRACKIR_NONE) == NULL);
    CHECK(SDL_TRACKIR_AXES == 3 && SDL_TRACKIR_BUTTONS == 1);
    /* linuxtrack's buffer (tir_hw.h:12) */
    CHECK(SDL_TRACKIR_READ_SIZE == 16384);
}

/* Constructed from linuxtrack: interface 0 with bulk OUT 0x02 and bulk IN
   0x82 (tir_hw.c:19-23, :1086-1089). No source records the packet size or
   the order, so 64 bytes, and both orders, are placeholders. */
static const unsigned char out_then_in[] = {
    9, 4, 0, 0, 2, 0xFF, 0, 0, 0,
    7, 5, 0x02, 2, 64, 0, 0,
    7, 5, 0x82, 2, 64, 0, 0,
};
static const unsigned char in_then_out[] = {
    9, 4, 0, 0, 2, 0xFF, 0, 0, 0,
    7, 5, 0x82, 2, 64, 0, 0,
    7, 5, 0x02, 2, 64, 0, 0,
};

static void TestVendorRules(void)
{
    static const uint16_t products[2] = { USB_PRODUCT_NATURALPOINT_TRACKIR2, USB_PRODUCT_NATURALPOINT_TRACKIR3 };
    static const uint8_t full_speed[4] = { 8, 16, 32, 64 };
    int p;

    for (p = 0; p < 2; ++p) {
        const SDL_VendorUSBRule *rule = SDL_VendorUSB_FindRule(USB_VENDOR_NATURALPOINT, products[p], 0, 0xFF, 0, 0);
        SDL_VendorUSBSelection selection;
        size_t i;

        CHECK(rule != NULL);
        if (!rule) {
            continue;
        }
        /* Interface 0 whatever its class, commands unchanged, Windows only like
           every rule of Part 14 */
        CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_NATURALPOINT, products[p], 0, 0x00, 0, 0) == rule);
        CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_NATURALPOINT, products[p], 1, 0xFF, 0, 0) == NULL);
        CHECK(rule->flags == (SDL_VENDORUSB_RAW_OUTPUT | SDL_VENDORUSB_WINDOWS_ONLY));
        CHECK(rule->interface_number == 0 && rule->alternate == 0);
        CHECK(rule->in_endpoint == 0x82 && rule->out_endpoint == 0x02);
        CHECK(rule->in_size == 0 && rule->out_size == 0);
        CHECK(rule->read_size == SDL_TRACKIR_READ_SIZE);
        CHECK(SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_WINDOWS));
        CHECK(!SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_MACOS));
        CHECK(!SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_OTHER));
        CHECK(SDL_VendorUSB_IsVendorDevice(USB_VENDOR_NATURALPOINT, products[p]));
        /* Only the interface's rule takes a camera to libusb */
        CHECK(!SDL_VendorUSB_RequiresLibUSB(SDL_VENDORUSB_PLATFORM_OTHER, USB_VENDOR_NATURALPOINT, products[p], false, false));
        CHECK(!SDL_VendorUSB_RequiresLibUSB(SDL_VENDORUSB_PLATFORM_MACOS, USB_VENDOR_NATURALPOINT, products[p], false, false));
        CHECK(SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_WINDOWS, USB_VENDOR_NATURALPOINT, products[p], 0, 0xFF, 0, 0, false));
        CHECK(!SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_WINDOWS, USB_VENDOR_NATURALPOINT, products[p], 1, 0x03, 0, 0, false));
        CHECK(!SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_OTHER, USB_VENDOR_NATURALPOINT, products[p], 0, 0xFF, 0, 0, false));

        /* 16384 bytes per bulk read, in either endpoint order */
        memset(&selection, 0, sizeof(selection));
        CHECK(SDL_VendorUSB_SelectEndpoints(rule, 0, out_then_in, sizeof(out_then_in), &selection));
        CHECK(selection.in.address == 0x82 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_BULK &&
              selection.in.read_size == SDL_TRACKIR_READ_SIZE);
        CHECK(selection.out.address == 0x02 && selection.out.transfer == SDL_VENDORUSB_TRANSFER_BULK);
        memset(&selection, 0, sizeof(selection));
        CHECK(SDL_VendorUSB_SelectEndpoints(rule, 0, in_then_out, sizeof(in_then_out), &selection));
        CHECK(selection.in.address == 0x82 && selection.in.read_size == SDL_TRACKIR_READ_SIZE);
        CHECK(selection.out.address == 0x02);

        /* A whole number of packets at every full-speed bulk size */
        for (i = 0; i < sizeof(full_speed); ++i) {
            unsigned char descriptors[sizeof(out_then_in)];

            memcpy(descriptors, out_then_in, sizeof(descriptors));
            descriptors[9 + 4] = full_speed[i];
            descriptors[16 + 4] = full_speed[i];
            memset(&selection, 0, sizeof(selection));
            CHECK(SDL_VendorUSB_SelectEndpoints(rule, 0, descriptors, sizeof(descriptors), &selection));
            CHECK(selection.in.max_packet_size == full_speed[i] && selection.in.read_size == SDL_TRACKIR_READ_SIZE);
        }

        /* Without the OUT endpoint the interface does not open */
        CHECK(!SDL_VendorUSB_SelectEndpoints(rule, 0, in_then_out, 16, &selection));

        /* On Windows the HID backend leaves it to libusb */
        {
            SDL_VendorUSBRouting routing;

            memset(&routing, 0, sizeof(routing));
            routing.platform = SDL_VENDORUSB_PLATFORM_WINDOWS;
            routing.vendor = USB_VENDOR_NATURALPOINT;
            routing.product = products[p];
            CHECK(SDL_VendorUSB_Ignore(&routing));
            routing.libusb = true;
            routing.whitelist = true;
            routing.vendor_interface = true;
            CHECK(!SDL_VendorUSB_Ignore(&routing));
            /* On Linux nothing changes: the hidraw backend is not told to
               leave it, and libusb leaves it to linuxtrack */
            memset(&routing, 0, sizeof(routing));
            routing.platform = SDL_VENDORUSB_PLATFORM_OTHER;
            routing.vendor = USB_VENDOR_NATURALPOINT;
            routing.product = products[p];
            CHECK(!SDL_VendorUSB_Ignore(&routing));
            routing.libusb = true;
            routing.whitelist = true;
            routing.vendor_interface = SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_OTHER);
            CHECK(SDL_VendorUSB_Ignore(&routing));
        }
    }
}

/* 8: a camera that never answers. Each try waits 6 ms, then reads until a
   read completes more than 100 ms after the reads began, and a read with no
   data completes after 1000 ms, so each try takes 1006 ms. After 20 tries
   the start-up goes on (tir_hw.c:233-267). */
static void TestStartSilent(void)
{
    Expected *expected = (Expected *)Allocate(sizeof(Expected) * MAX_EXPECTED);
    SDL_TrackIRState *state = NewState();
    Rig *rig = (Rig *)Allocate(sizeof(Rig));
    uint64_t deadline = 0;
    int n, k;

    /* TrackIR 2: the flush runs from 34 to 334 */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    n = Prologue2(expected, 0);
    for (k = 0; k < SDL_TRACKIR_INFO_TRIES; ++k) {
        n = Add(expected, n, 334 + 1006 * (uint64_t)k, 2, c_request);
    }
    n = Epilogue2(expected, n, 334 + 1006 * 19 + 1006 + 70);
    CHECK(n == 35);
    RunUntilRunning(rig, 0, 334 + 1006 * 20 + 70 + 318);
    CheckSent(rig, expected, n);
    CHECK(!SDL_TrackIR_GetDeadline(state, &deadline));

    /* TrackIR 3: the flush runs from 72 to 372 */
    InitRig(rig, state, SDL_TRACKIR_3, 0);
    n = Prologue3(expected, 0);
    for (k = 0; k < SDL_TRACKIR_INFO_TRIES; ++k) {
        n = Add(expected, n, 372 + 1006 * (uint64_t)k, 2, c_request);
    }
    n = Epilogue3(expected, n, 372 + 1006 * 20 + 70);
    CHECK(n == 38);
    RunUntilRunning(rig, 0, 372 + 1006 * 20 + 70 + 88);
    CheckSent(rig, expected, n);

    /* The same from another start time */
    InitRig(rig, state, SDL_TRACKIR_3, 1000000);
    n = Prologue3(expected, 1000000);
    for (k = 0; k < SDL_TRACKIR_INFO_TRIES; ++k) {
        n = Add(expected, n, 1000000 + 372 + 1006 * (uint64_t)k, 2, c_request);
    }
    n = Epilogue3(expected, n, 1000000 + 372 + 1006 * 20 + 70);
    RunUntilRunning(rig, 1000000, 1000000 + 372 + 1006 * 20 + 70 + 88);
    CheckSent(rig, expected, n);

    free(rig);
    free(state);
    free(expected);
}

/* 8: a camera that answers each request a millisecond later. The answer
   comes during the 6 ms wait and is read when the try reads. */
static void TestStartAnswered(void)
{
    Expected *expected = (Expected *)Allocate(sizeof(Expected) * MAX_EXPECTED);
    SDL_TrackIRState *state = NewState();
    Rig *rig = (Rig *)Allocate(sizeof(Rig));
    int n;

    InitRig(rig, state, SDL_TRACKIR_2, 0);
    rig->answer = 1;
    n = Prologue2(expected, 0);
    n = Add(expected, n, 334, 2, c_request);
    n = Epilogue2(expected, n, 410);
    RunUntilRunning(rig, 0, 728);
    CheckSent(rig, expected, n);
    CHECK(StartRunning(state, SDL_TRACKIR_2) == 728);

    InitRig(rig, state, SDL_TRACKIR_3, 0);
    rig->answer = 1;
    n = Prologue3(expected, 0);
    n = Add(expected, n, 372, 2, c_request);
    n = Epilogue3(expected, n, 448);
    RunUntilRunning(rig, 0, 536);
    CheckSent(rig, expected, n);
    CHECK(StartRunning(state, SDL_TRACKIR_3) == 536);

    /* An answer in the try's window is taken when it comes */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    rig->answer = 66;
    n = Prologue2(expected, 0);
    n = Add(expected, n, 334, 2, c_request);
    n = Epilogue2(expected, n, 400 + 70);
    RunUntilRunning(rig, 0, 470 + 318);
    CheckSent(rig, expected, n);

    /* Every delay starts when the write before it completes */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    rig->write_ms = 3;
    Run(rig, 0, 39);
    CHECK(rig->count == 6);
    CHECK(rig->sent[0].at == 0 && rig->sent[1].at == 7 && rig->sent[2].at == 14 && rig->sent[3].at == 21 &&
          rig->sent[4].at == 28 && rig->sent[5].at == 33);

    free(rig);
    free(state);
    free(expected);
}

/* The flush: three reads, each ending with data or after 100 ms. A read
   that came before the flush began is dropped uncounted. */
static void TestFlush(void)
{
    SDL_TrackIRState *state = NewState();
    Rig *rig = (Rig *)Allocate(sizeof(Rig));
    uint64_t deadline = 0;
    Planned planned[4];

    /* Reads at 40, 45 and 50 end it at 50 */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 40;
    planned[1].at = 45;
    planned[2].at = 50;
    planned[0].data = planned[1].data = planned[2].data = status_read;
    planned[0].length = planned[1].length = planned[2].length = sizeof(status_read);
    rig->planned = planned;
    rig->planned_count = 3;
    Run(rig, 0, 49);
    CHECK(rig->count == 9);
    Tick(rig, 50);
    CHECK(rig->count == 10 && rig->sent[9].at == 50 && memcmp(rig->sent[9].data, c_request, 2) == 0);

    /* The flush begins at 34. A read at 33 does not count, one at 34 does. */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 33;
    planned[1].at = 34;
    planned[0].data = planned[1].data = status_read;
    planned[0].length = planned[1].length = sizeof(status_read);
    rig->planned = planned;
    rig->planned_count = 2;
    Run(rig, 0, 34);
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 134);
    Run(rig, 35, 233);
    CHECK(rig->count == 9);
    Tick(rig, 234);
    CHECK(rig->count == 10 && rig->sent[9].at == 234);

    /* A read, then silence: the timeouts count from the read */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 100;
    planned[0].data = status_read;
    planned[0].length = sizeof(status_read);
    rig->planned = planned;
    rig->planned_count = 1;
    Run(rig, 0, 299);
    CHECK(rig->count == 9);
    Tick(rig, 300);
    CHECK(rig->count == 10 && rig->sent[9].at == 300);

    /* Silence: 100 ms each. At 133 nothing, at 334 the request. */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    Run(rig, 0, 133);
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 134);
    Tick(rig, 134);
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 234);
    Run(rig, 135, 333);
    CHECK(rig->count == 9);
    Tick(rig, 334);
    CHECK(rig->count == 10 && rig->sent[9].at == 334);

    /* A late look finds each timeout where it fell. The zero-length read at
       500 comes after the flush and answers nothing. */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    Run(rig, 0, 34);
    CHECK(!SDL_TrackIR_Feed(state, NULL, 0, 500));
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 334);
    Tick(rig, 500);
    CHECK(rig->count == 10 && rig->sent[9].at == 500);
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 1506);

    /* An answer during the flush is only a flush read, and a zero-length
       read counts too */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 40;
    planned[0].data = info_answer;
    planned[0].length = sizeof(info_answer);
    planned[1].at = 41;
    planned[1].data = info_answer;
    planned[1].length = 0;
    planned[2].at = 42;
    planned[2].data = info_answer;
    planned[2].length = sizeof(info_answer);
    rig->planned = planned;
    rig->planned_count = 3;
    Run(rig, 0, 42);
    CHECK(rig->count == 10 && rig->sent[9].at == 42);
    /* No answer after the request: the try times out at 1048 */
    Run(rig, 43, 1047);
    CHECK(rig->count == 10);
    Tick(rig, 1048);
    CHECK(rig->count == 11 && rig->sent[10].at == 1048 && memcmp(rig->sent[10].data, c_request, 2) == 0);

    /* The TrackIR 3 flush begins at 72 */
    InitRig(rig, state, SDL_TRACKIR_3, 0);
    planned[0].at = 71;
    planned[1].at = 72;
    planned[2].at = 73;
    planned[3].at = 74;
    planned[0].data = planned[1].data = planned[2].data = planned[3].data = status_read;
    planned[0].length = planned[1].length = planned[2].length = planned[3].length = sizeof(status_read);
    rig->planned = planned;
    rig->planned_count = 4;
    Run(rig, 0, 73);
    CHECK(rig->count == 8);
    Tick(rig, 74);
    CHECK(rig->count == 9 && rig->sent[8].at == 74);

    free(rig);
    free(state);
}

/* The information wait, try by try (tir_hw.c:233-267) */
static void TestInfoWait(void)
{
    SDL_TrackIRState *state = NewState();
    Rig *rig = (Rig *)Allocate(sizeof(Rig));
    uint64_t deadline = 0;
    Planned planned[4];
    static const uint8_t just_09[1] = { 0x09 };
    static const uint8_t just_09_40[2] = { 0x09, 0x40 };
    static const uint8_t swapped[2] = { 0x40, 0x09 };
    static const uint8_t wrong_type[9] = { 0x09, 0x41, 0x03, 0x00, 0x00, 0x34, 0x5D, 0x03, 0x00 };
    static const uint8_t wrong_size[9] = { 0x19, 0x40, 0x03, 0x00, 0x00, 0x34, 0x5D, 0x03, 0x00 };
    static const uint8_t late_answer[16] = { 0x07, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
                                             0x09, 0x40, 0x03, 0x00, 0x00, 0x34, 0x5D, 0x03, 0x00 };

    /* Reads that do not start 09 40 answer nothing */
    {
        const struct
        {
            const uint8_t *data;
            size_t length;
        } misses[] = {
            { just_09, sizeof(just_09) }, { swapped, sizeof(swapped) }, { wrong_type, sizeof(wrong_type) },
            { wrong_size, sizeof(wrong_size) }, { late_answer, sizeof(late_answer) }, { info_answer, 0 },
        };
        size_t i;

        for (i = 0; i < sizeof(misses) / sizeof(misses[0]); ++i) {
            InitRig(rig, state, SDL_TRACKIR_2, 0);
            planned[0].at = 336;
            planned[0].data = misses[i].data;
            planned[0].length = misses[i].length;
            rig->planned = planned;
            rig->planned_count = 1;
            Run(rig, 0, 1339);
            CHECK(rig->count == 10);
            Tick(rig, 1340);
            CHECK(rig->count == 11 && rig->sent[10].at == 1340);
        }
        /* Two bytes are enough, and a stale byte past a one-byte read is not read */
        InitRig(rig, state, SDL_TRACKIR_2, 0);
        Run(rig, 0, 340);
        CHECK(!SDL_TrackIR_Feed(state, just_09_40, 1, 341));
        Run(rig, 341, 409);
        CHECK(rig->count == 10);
        CHECK(!SDL_TrackIR_Feed(state, just_09_40, 2, 410));
        Run(rig, 410, 479);
        CHECK(rig->count == 10);
        Tick(rig, 480);
        CHECK(rig->count == 11 && rig->sent[10].at == 480 && memcmp(rig->sent[10].data, c_threshold2, 3) == 0);
    }

    /* Other reads within 100 ms of the try's start keep it going, and one
       later ends it. The try's reads began at 340. */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 350;
    planned[1].at = 440;
    planned[2].at = 441;
    planned[0].data = planned[1].data = planned[2].data = status_read;
    planned[0].length = planned[1].length = planned[2].length = sizeof(status_read);
    rig->planned = planned;
    rig->planned_count = 3;
    Run(rig, 0, 440);
    CHECK(rig->count == 10);
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 1440);
    Tick(rig, 441);
    CHECK(rig->count == 11 && rig->sent[10].at == 441 && memcmp(rig->sent[10].data, c_request, 2) == 0);

    /* A late look ends the try when its read timed out, at 1340 */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    Run(rig, 0, 334);
    CHECK(!SDL_TrackIR_Feed(state, NULL, 0, 2000));
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 1340);
    Tick(rig, 2000);
    CHECK(rig->count == 11 && rig->sent[10].at == 2000);

    /* A read at 440 exactly keeps the try, and the read timeout then counts
       from it */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 440;
    planned[0].data = status_read;
    planned[0].length = sizeof(status_read);
    rig->planned = planned;
    rig->planned_count = 1;
    Run(rig, 0, 1439);
    CHECK(rig->count == 10);
    Tick(rig, 1440);
    CHECK(rig->count == 11 && rig->sent[10].at == 1440);

    /* Reads during the 6 ms wait complete when the reads begin */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 336;
    planned[1].at = 441;
    planned[0].data = planned[1].data = status_read;
    planned[0].length = planned[1].length = sizeof(status_read);
    rig->planned = planned;
    rig->planned_count = 2;
    Run(rig, 0, 440);
    CHECK(rig->count == 10);
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 1340);
    Tick(rig, 441);
    CHECK(rig->count == 11 && rig->sent[10].at == 441);

    /* An answer among other reads is taken */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 350;
    planned[1].at = 360;
    planned[0].data = status_read;
    planned[0].length = sizeof(status_read);
    planned[1].data = info_answer;
    planned[1].length = sizeof(info_answer);
    rig->planned = planned;
    rig->planned_count = 2;
    Run(rig, 0, 429);
    CHECK(rig->count == 10);
    Tick(rig, 430);
    CHECK(rig->count == 11 && rig->sent[10].at == 430 && memcmp(rig->sent[10].data, c_threshold2, 3) == 0);

    /* An answer that came before the reads began is read when they begin */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 334;
    planned[0].data = info_answer;
    planned[0].length = sizeof(info_answer);
    rig->planned = planned;
    rig->planned_count = 1;
    Run(rig, 0, 339);
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 340);
    Run(rig, 340, 409);
    CHECK(rig->count == 10);
    Tick(rig, 410);
    CHECK(rig->count == 11 && rig->sent[10].at == 410);

    /* A late look takes an early answer when the try's reads began, at 340 */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 336;
    planned[0].data = info_answer;
    planned[0].length = sizeof(info_answer);
    rig->planned = planned;
    rig->planned_count = 1;
    Run(rig, 0, 336);
    CHECK(!SDL_TrackIR_Feed(state, NULL, 0, 500));
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 410);

    /* An answer just after a try timed out is taken by the next try: the
       request goes out once more, as linuxtrack's loop sends it */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 1340;
    planned[0].data = info_answer;
    planned[0].length = sizeof(info_answer);
    rig->planned = planned;
    rig->planned_count = 1;
    Run(rig, 0, 1340);
    CHECK(rig->count == 11 && rig->sent[10].at == 1340);
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 1346);
    Run(rig, 1341, 1415);
    CHECK(rig->count == 11);
    Tick(rig, 1416);
    CHECK(rig->count == 12 && rig->sent[11].at == 1416 && memcmp(rig->sent[11].data, c_threshold2, 3) == 0);

    /* The answer to the 20th try counts, and one after it does not */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 334 + 1006 * 19 + 7;
    planned[0].data = info_answer;
    planned[0].length = sizeof(info_answer);
    rig->planned = planned;
    rig->planned_count = 1;
    Run(rig, 0, 334 + 1006 * 19 + 7 + 69);
    CHECK(rig->count == 29);
    Tick(rig, 334 + 1006 * 19 + 7 + 70);
    CHECK(rig->count == 30 && memcmp(rig->sent[29].data, c_threshold2, 3) == 0);

    InitRig(rig, state, SDL_TRACKIR_2, 0);
    planned[0].at = 334 + 1006 * 20;
    rig->planned = planned;
    rig->planned_count = 1;
    Run(rig, 0, 334 + 1006 * 20 + 70);
    CHECK(rig->count == 30 && rig->sent[29].at == 334 + 1006 * 20 + 70);

    /* A failed request ends the start-up for good: no more commands, no
       decoding, and no close */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    rig->fail = 9;
    Run(rig, 0, 30000);
    CHECK(rig->count == 10 && !SDL_TrackIR_IsRunning(state));
    CHECK(!SDL_TrackIR_GetDeadline(state, &deadline));
    CHECK(!Feed(state, info_answer, sizeof(info_answer), 30001));
    SDL_TrackIR_BeginClose(state, 30002);
    CHECK(SDL_TrackIR_IsClosed(state));
    Run(rig, 30002, 31000);
    CHECK(rig->count == 10);

    /* So does a failed second request */
    InitRig(rig, state, SDL_TRACKIR_3, 0);
    rig->fail = 9;
    Run(rig, 0, 30000);
    CHECK(rig->count == 10 && rig->sent[9].at == 372 + 1006 && !SDL_TrackIR_IsRunning(state));

    /* A failed light command does not stop the start-up */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    rig->fail = 2;
    rig->answer = 1;
    Run(rig, 0, 728);
    CHECK(SDL_TrackIR_IsRunning(state) && rig->count == 16);

    free(rig);
    free(state);
}

/* The deadlines the driver's close loop waits for */
static void TestDeadlines(void)
{
    SDL_TrackIRState *state = NewState();
    SDL_TrackIRCommand command;
    uint64_t deadline = 0;

    /* A completion with nothing in flight moves nothing */
    SDL_TrackIR_Init(state, SDL_TRACKIR_2, 0);
    SDL_TrackIR_CommandDone(state, true, 0);
    CHECK(SDL_TrackIR_NextCommand(state, 0, &command) && command.length == 3 && memcmp(command.data, c_ir_off, 3) == 0);

    SDL_TrackIR_Init(state, SDL_TRACKIR_2, 50);
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 50);
    CHECK(!SDL_TrackIR_NextCommand(state, 49, &command));
    CHECK(SDL_TrackIR_NextCommand(state, 50, &command));
    /* Nothing while a command is in flight */
    CHECK(!SDL_TrackIR_GetDeadline(state, &deadline));
    SDL_TrackIR_CommandDone(state, true, 51);
    CHECK(SDL_TrackIR_GetDeadline(state, &deadline) && deadline == 55);
    CHECK(!SDL_TrackIR_GetDeadline(state, NULL));
    CHECK(!SDL_TrackIR_GetDeadline(NULL, &deadline));

    /* An unknown model sends nothing and is never running */
    SDL_TrackIR_Init(state, SDL_TRACKIR_NONE, 0);
    CHECK(!SDL_TrackIR_NextCommand(state, 100000, &command));
    CHECK(!SDL_TrackIR_GetDeadline(state, &deadline));
    CHECK(!SDL_TrackIR_Feed(state, info_answer, sizeof(info_answer), 100001));
    CHECK(!SDL_TrackIR_IsRunning(state) && !SDL_TrackIR_IsClosed(state));
    SDL_TrackIR_BeginClose(state, 100002);
    CHECK(SDL_TrackIR_IsClosed(state));

    free(state);
}

/* 1 and 2 */
static const uint8_t test1_read[11] = { 0x0B, 0x1C, 0x10, 0x20, 0x22, 0x11, 0x20, 0x22, 0x00, 0x00, 0x00 };
static const uint8_t test2_read[14] = { 0x0E, 0x1C, 0x10, 0x20, 0x22, 0x00, 0x11, 0x20, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00 };

static void TestReplay(void)
{
    SDL_TrackIRState *tir2 = NewState();
    SDL_TrackIRState *tir3 = NewState();
    SDL_TrackIRState *state = NewState();
    uint64_t now2 = StartRunning(tir2, SDL_TRACKIR_2);
    uint64_t now3 = StartRunning(tir3, SDL_TRACKIR_3);

    CHECK(Shows(tir2, false, 0, 0, 0) && tir2->frames == 0);

    /* 1: one blob of 6 pixels, x 33 and y 16.5, so x' 94.5 and y' 111 */
    *state = *tir2;
    CHECK(Feed(state, test1_read, sizeof(test1_read), now2));
    CHECK(state->frames == 1 && Shows(state, true, 24192, 28416, 6) && Idle(state));

    /* 2: the same blob on the TrackIR 3, x' 186.5 and y' 140 */
    *state = *tir3;
    CHECK(Feed(state, test2_read, sizeof(test2_read), now3));
    CHECK(state->frames == 1 && Shows(state, true, 27778, 29219, 6));

    /* 3: flag C0 puts the stripe at x 288 to 290, flag 20 on line 272, and
       flag 18 at x 544, past the sensor */
    {
        static const uint8_t c0[10] = { 0x0A, 0x1C, 0x10, 0x20, 0x22, 0xC0, 0x00, 0x00, 0x00, 0x00 };
        static const uint8_t line[10] = { 0x0A, 0x1C, 0x10, 0x20, 0x22, 0x20, 0x00, 0x00, 0x00, 0x00 };
        static const uint8_t past[10] = { 0x0A, 0x1C, 0x10, 0x20, 0x22, 0x18, 0x00, 0x00, 0x00, 0x00 };
        static const uint8_t first_only[10] = { 0x0A, 0x1C, 0x10, 0x20, 0x22, 0x10, 0x00, 0x00, 0x00, 0x00 };
        static const uint8_t last_only[10] = { 0x0A, 0x1C, 0x10, 0x20, 0x22, 0x08, 0x00, 0x00, 0x00, 0x00 };
        static const uint8_t unused_bits[10] = { 0x0A, 0x1C, 0x10, 0x20, 0x22, 0x07, 0x00, 0x00, 0x00, 0x00 };
        /* 10 and 40: x 544 to 290. 88: x 288 to 546. Both are ignored. */
        static const uint8_t first_high[10] = { 0x0A, 0x1C, 0x10, 0x20, 0x22, 0x50, 0x00, 0x00, 0x00, 0x00 };
        static const uint8_t last_high[10] = { 0x0A, 0x1C, 0x10, 0x20, 0x22, 0x88, 0x00, 0x00, 0x00, 0x00 };

        *state = *tir3;
        CHECK(Feed(state, c0, sizeof(c0), now3));
        CHECK(Shows(state, true, -10352, 29324, 3));
        CHECK(Feed(state, line, sizeof(line), now3));
        CHECK(Shows(state, true, 27778, -24106, 3));
        /* Ignored: the frame ends with no blob, and the axes keep their
           values */
        CHECK(Feed(state, past, sizeof(past), now3));
        CHECK(state->frames == 3 && Shows(state, false, 27778, -24106, 3));
        CHECK(Feed(state, c0, sizeof(c0), now3));
        CHECK(Feed(state, first_only, sizeof(first_only), now3));
        CHECK(Shows(state, false, -10352, 29324, 3));
        CHECK(Feed(state, c0, sizeof(c0), now3));
        CHECK(Feed(state, last_only, sizeof(last_only), now3));
        CHECK(Shows(state, false, -10352, 29324, 3));
        /* Bits 04, 02 and 01 change nothing */
        CHECK(Feed(state, unused_bits, sizeof(unused_bits), now3));
        CHECK(Shows(state, true, 27778, 29324, 3));
        CHECK(Feed(state, first_high, sizeof(first_high), now3));
        CHECK(Shows(state, false, 27778, 29324, 3));
        CHECK(Feed(state, last_high, sizeof(last_high), now3));
        CHECK(Shows(state, false, 27778, 29324, 3));
    }

    /* 4: two blobs of 3 and 2 pixels, the larger drives the axes, in either
       order. Two of 3 pixels: the later one. */
    {
        static const uint8_t two[11] = { 0x0B, 0x1C, 0x10, 0x20, 0x22, 0x10, 0x40, 0x41, 0x00, 0x00, 0x00 };
        static const uint8_t reversed[11] = { 0x0B, 0x1C, 0x10, 0x40, 0x41, 0x10, 0x20, 0x22, 0x00, 0x00, 0x00 };
        static const uint8_t tie[11] = { 0x0B, 0x1C, 0x10, 0x20, 0x22, 0x10, 0x40, 0x42, 0x00, 0x00, 0x00 };

        *state = *tir2;
        CHECK(Feed(state, two, sizeof(two), now2));
        CHECK(Shows(state, true, 24192, 28544, 3));
        *state = *tir2;
        CHECK(Feed(state, reversed, sizeof(reversed), now2));
        CHECK(Shows(state, true, 24192, 28544, 3));
        *state = *tir2;
        CHECK(Feed(state, tie, sizeof(tie), now2));
        CHECK(Shows(state, true, 16000, 28544, 3));
    }

    /* 5: line 11 then line 10 with no zero stripe ends the first frame at
       the second stripe, which starts the next */
    {
        static const uint8_t lower[8] = { 0x08, 0x1C, 0x11, 0x20, 0x22, 0x10, 0x20, 0x22 };
        static const uint8_t zero[5] = { 0x05, 0x1C, 0x00, 0x00, 0x00 };
        static const uint8_t equal[8] = { 0x08, 0x1C, 0x10, 0x20, 0x22, 0x10, 0x40, 0x41 };
        static const uint8_t higher[5] = { 0x05, 0x1C, 0x12, 0x20, 0x22 };

        *state = *tir2;
        CHECK(Feed(state, lower, sizeof(lower), now2));
        CHECK(state->frames == 1 && Shows(state, true, 24192, 28288, 3));
        CHECK(state->frame_line == 0x10 && state->range_count[1 - state->previous] == 1);
        CHECK(Feed(state, zero, sizeof(zero), now2));
        CHECK(state->frames == 2 && Shows(state, true, 24192, 28544, 3));
        /* An equal line or a higher one does not end a frame */
        *state = *tir2;
        CHECK(!Feed(state, equal, sizeof(equal), now2) && state->frames == 0);
        CHECK(!Feed(state, higher, sizeof(higher), now2) && state->frames == 0);
        CHECK(state->frame_line == 0x12);
    }

    /* 6: a status packet before the stripes changes nothing, a stripe packet
       of size 3E skips the two bytes after it, and an unknown type drops the
       rest of the read */
    {
        Stream stream;
        Stripe bar[20];
        size_t i;
        static const uint8_t unknown[16] = { 0x05, 0x33, 0x00, 0x00, 0x00,
                                             0x0B, 0x1C, 0x10, 0x20, 0x22, 0x11, 0x20, 0x22, 0x00, 0x00, 0x00 };

        memset(&stream, 0, sizeof(stream));
        PutBytes(&stream, status_read, sizeof(status_read));
        PutBytes(&stream, test1_read, sizeof(test1_read));
        *state = *tir2;
        CHECK(Feed(state, stream.data, stream.length, now2));
        CHECK(state->frames == 1 && Shows(state, true, 24192, 28416, 6));
        FreeStream(&stream);

        /* 20 TrackIR 2 stripes make a packet of size 3E */
        for (i = 0; i < 20; ++i) {
            bar[i].line = (uint32_t)(16 + i);
            bar[i].first = 0x20;
            bar[i].last = 0x22;
        }
        PutStripes(&stream, SDL_TRACKIR_2, bar, 20, 20);
        CHECK(stream.length == 64 && stream.data[0] == 0x3E && stream.data[62] == 0xFF);
        PutZero(&stream, SDL_TRACKIR_2);
        *state = *tir2;
        CHECK(Feed(state, stream.data, stream.length, now2));
        CHECK(state->frames == 1 && Shows(state, true, 24192, 26112, 60));
        FreeStream(&stream);

        /* 15 TrackIR 3 stripes make one too */
        PutStripes(&stream, SDL_TRACKIR_3, bar, 15, 15);
        CHECK(stream.length == 64 && stream.data[0] == 0x3E);
        PutZero(&stream, SDL_TRACKIR_3);
        *state = *tir3;
        CHECK(Feed(state, stream.data, stream.length, now3));
        CHECK(state->frames == 1 && Shows(state, true, 27778, 27863, 45));
        FreeStream(&stream);

        /* Only a stripe packet of size 3E has the two bytes after it */
        {
            uint8_t status3e[62 + 5];

            memset(status3e, 0, sizeof(status3e));
            status3e[0] = 0x3E;
            status3e[1] = 0x20;
            memcpy(&status3e[62], "\x05\x1C\x00\x00\x00", 5);
            *state = *tir2;
            CHECK(Feed(state, status3e, sizeof(status3e), now2));
            CHECK(state->frames == 1 && Shows(state, false, 0, 0, 0));
            status3e[1] = 0x40;
            CHECK(Feed(state, status3e, sizeof(status3e), now2));
            CHECK(state->frames == 2);
        }

        /* The unknown type drops the stripe packet behind it, and the next
           read decodes */
        *state = *tir2;
        CHECK(!Feed(state, unknown, sizeof(unknown), now2));
        CHECK(state->frames == 0 && Idle(state));
        CHECK(Feed(state, test1_read, sizeof(test1_read), now2));
        CHECK(state->frames == 1 && Shows(state, true, 24192, 28416, 6));

        /* A size under 2 does too. A size of exactly 2 is an empty packet. */
        {
            uint8_t read[13];

            memcpy(&read[2], test1_read, sizeof(test1_read));
            read[1] = 0x1C;
            read[0] = 0x00;
            *state = *tir2;
            CHECK(!Feed(state, read, sizeof(read), now2) && state->frames == 0);
            read[0] = 0x01;
            CHECK(!Feed(state, read, sizeof(read), now2) && state->frames == 0);
            read[0] = 0x02;
            CHECK(Feed(state, read, sizeof(read), now2) && state->frames == 1);
            read[1] = 0x20;
            read[0] = 0x01;
            CHECK(!Feed(state, read, sizeof(read), now2) && state->frames == 1);
        }
        /* The size byte 01 is not a packet of its own: the zero stripe
           behind it goes with the rest of the read */
        {
            static const uint8_t one[6] = { 0x01, 0x05, 0x1C, 0x00, 0x00, 0x00 };

            *state = *tir2;
            CHECK(!Feed(state, one, sizeof(one), now2) && state->frames == 0);
        }
        /* A size of 1 on a known type drops a long read too, and never
           fills the packet buffer past its end into the fields behind it */
        {
            uint8_t *read = (uint8_t *)Allocate(2 + 300);
            size_t k;

            read[0] = 0x01;
            read[1] = 0x1C;
            for (k = 0; k + sizeof(test1_read) <= 300; k += sizeof(test1_read)) {
                memcpy(&read[2 + k], test1_read, sizeof(test1_read));
            }
            *state = *tir2;
            CHECK(!Feed(state, read, 2 + 300, now2) && SameFrame(state, tir2) && Idle(state));
            free(read);
        }
        /* Every other type byte is unknown, among them the other models'
           types 00, 04 and 10 */
        {
            uint8_t read[13];
            int type;

            memcpy(&read[2], test1_read, sizeof(test1_read));
            read[0] = 0x02;
            for (type = 0; type < 256; ++type) {
                const bool known = (type == 0x1C || type == 0x20 || type == 0x40);

                read[1] = (uint8_t)type;
                *state = *tir2;
                CHECK(Feed(state, read, sizeof(read), now2) == known);
            }
        }
    }

    /* A device information packet in the stream is skipped */
    {
        Stream stream;

        memset(&stream, 0, sizeof(stream));
        PutBytes(&stream, info_answer, sizeof(info_answer));
        PutBytes(&stream, test2_read, sizeof(test2_read));
        *state = *tir3;
        CHECK(Feed(state, stream.data, stream.length, now3));
        CHECK(state->frames == 1 && Shows(state, true, 27778, 29219, 6));
        FreeStream(&stream);
    }

    free(state);
    free(tir3);
    free(tir2);
}

/* Every prefix of a read, as a short read and with the rest of the read in
   the buffer past it: it decodes as its whole packets alone would, and the
   next read starts clean */
static void CheckPrefixes(const SDL_TrackIRState *start, const uint8_t *read, size_t length,
                          const uint8_t *next, size_t next_length)
{
    SDL_TrackIRState *exact = NewState();
    SDL_TrackIRState *stale = NewState();
    SDL_TrackIRState *whole = NewState();
    size_t n;

    for (n = 0; n <= length; ++n) {
        *exact = *start;
        *stale = *start;
        *whole = *start;
        Feed(exact, read, n, 5000);
        SDL_TrackIR_Feed(stale, read, n, 5000);
        Feed(whole, read, WholeLength(read, n), 5000);
        CHECK(SameFrame(exact, whole) && SameFrame(stale, whole));
        CHECK(Idle(exact) && Idle(stale));
        Feed(exact, next, next_length, 5001);
        Feed(whole, next, next_length, 5001);
        CHECK(SameFrame(exact, whole) && Idle(exact));
    }
    free(whole);
    free(stale);
    free(exact);
}

/* 7: every truncation of every packet changes nothing, and the parser starts
   the next read clean */
static void TestTruncations(void)
{
    SDL_TrackIRState *tir2 = NewState();
    SDL_TrackIRState *tir3 = NewState();
    Stream stream;
    Stripe bar[20];
    size_t i;
    static const uint8_t lower[8] = { 0x08, 0x1C, 0x11, 0x20, 0x22, 0x10, 0x20, 0x22 };
    static const uint8_t unknown[16] = { 0x05, 0x33, 0x00, 0x00, 0x00,
                                         0x0B, 0x1C, 0x10, 0x20, 0x22, 0x11, 0x20, 0x22, 0x00, 0x00, 0x00 };

    StartRunning(tir2, SDL_TRACKIR_2);
    StartRunning(tir3, SDL_TRACKIR_3);
    /* A frame in progress, so a stray stripe would show */
    Feed(tir2, lower, sizeof(lower), 4000);
    CHECK(tir2->frames == 1 && tir2->range_count[1 - tir2->previous] == 1);

    CheckPrefixes(tir2, test1_read, sizeof(test1_read), test1_read, sizeof(test1_read));
    CheckPrefixes(tir2, status_read, sizeof(status_read), test1_read, sizeof(test1_read));
    CheckPrefixes(tir2, info_answer, sizeof(info_answer), test1_read, sizeof(test1_read));
    CheckPrefixes(tir2, unknown, sizeof(unknown), test1_read, sizeof(test1_read));
    CheckPrefixes(tir2, lower, sizeof(lower), test1_read, sizeof(test1_read));
    CheckPrefixes(tir3, test2_read, sizeof(test2_read), test2_read, sizeof(test2_read));
    CheckPrefixes(tir3, status_read, sizeof(status_read), test2_read, sizeof(test2_read));

    memset(&stream, 0, sizeof(stream));
    PutBytes(&stream, status_read, sizeof(status_read));
    PutBytes(&stream, test1_read, sizeof(test1_read));
    PutBytes(&stream, info_answer, sizeof(info_answer));
    PutBytes(&stream, lower, sizeof(lower));
    PutZero(&stream, SDL_TRACKIR_2);
    CheckPrefixes(tir2, stream.data, stream.length, test1_read, sizeof(test1_read));
    FreeStream(&stream);

    for (i = 0; i < 20; ++i) {
        bar[i].line = (uint32_t)(16 + i);
        bar[i].first = 0x20;
        bar[i].last = 0x22;
    }
    PutStripes(&stream, SDL_TRACKIR_2, bar, 20, 20);
    PutZero(&stream, SDL_TRACKIR_2);
    CheckPrefixes(tir2, stream.data, stream.length, test1_read, sizeof(test1_read));
    FreeStream(&stream);

    PutStripes(&stream, SDL_TRACKIR_3, bar, 15, 15);
    PutZero(&stream, SDL_TRACKIR_3);
    CheckPrefixes(tir3, stream.data, stream.length, test2_read, sizeof(test2_read));
    FreeStream(&stream);

    /* A short read that ends between a stripe packet of size 3E and the two
       bytes after it: the next read starts at a header */
    {
        SDL_TrackIRState *state = NewState();
        static const uint8_t after[6] = { 0xFF, 0x05, 0x1C, 0x00, 0x00, 0x00 };

        PutStripes(&stream, SDL_TRACKIR_2, bar, 20, 20);
        StartRunning(state, SDL_TRACKIR_2);
        CHECK(!Feed(state, stream.data, 63, 5000) && Idle(state));
        CHECK(!Feed(state, after, sizeof(after), 5001));
        CHECK(Feed(state, after + 1, sizeof(after) - 1, 5002));
        CHECK(Shows(state, true, 24192, 26112, 60));
        FreeStream(&stream);
        free(state);
    }

    free(tir3);
    free(tir2);
}

/* A stream cut at every byte by a read of the whole transfer: the first
   read decodes its whole packets and keeps the cut one, and the second
   finishes the stream as one read would */
static void CheckSplits(const SDL_TrackIRState *start, const uint8_t *stream, size_t length)
{
    uint8_t *first = (uint8_t *)Allocate(SDL_TRACKIR_READ_SIZE);
    SDL_TrackIRState *split = NewState();
    SDL_TrackIRState *prefix = NewState();
    SDL_TrackIRState *whole = NewState();
    size_t k;

    *whole = *start;
    Feed(whole, stream, length, 6000);
    for (k = 0; k <= length; ++k) {
        const size_t fill = SDL_TRACKIR_READ_SIZE - k;

        Filler(first, fill);
        memcpy(first + fill, stream, k);
        *split = *start;
        *prefix = *start;
        Feed(split, first, SDL_TRACKIR_READ_SIZE, 6000);
        Feed(prefix, stream, WholeLength(stream, k), 6000);
        CHECK(SameFrame(split, prefix));
        Feed(split, stream + k, length - k, 6001);
        CHECK(SameFrame(split, whole) && Idle(split));
    }
    free(whole);
    free(prefix);
    free(split);
    free(first);
}

static void TestSplits(void)
{
    SDL_TrackIRState *tir2 = NewState();
    SDL_TrackIRState *tir3 = NewState();
    SDL_TrackIRState *state = NewState();
    uint8_t *full = (uint8_t *)Allocate(SDL_TRACKIR_READ_SIZE);
    Stream stream;
    Stripe bar[20];
    size_t i;
    static const uint8_t lower[8] = { 0x08, 0x1C, 0x11, 0x20, 0x22, 0x10, 0x20, 0x22 };

    StartRunning(tir2, SDL_TRACKIR_2);
    StartRunning(tir3, SDL_TRACKIR_3);
    for (i = 0; i < 20; ++i) {
        bar[i].line = (uint32_t)(16 + i);
        bar[i].first = 0x20;
        bar[i].last = 0x22;
    }

    CheckSplits(tir2, test1_read, sizeof(test1_read));
    CheckSplits(tir3, test2_read, sizeof(test2_read));

    memset(&stream, 0, sizeof(stream));
    PutBytes(&stream, status_read, sizeof(status_read));
    PutBytes(&stream, test1_read, sizeof(test1_read));
    PutBytes(&stream, info_answer, sizeof(info_answer));
    PutStripes(&stream, SDL_TRACKIR_2, bar, 20, 20);
    PutBytes(&stream, lower, sizeof(lower));
    PutStripes(&stream, SDL_TRACKIR_2, bar, 7, 3);
    PutZero(&stream, SDL_TRACKIR_2);
    CheckSplits(tir2, stream.data, stream.length);
    FreeStream(&stream);

    PutBytes(&stream, test2_read, sizeof(test2_read));
    PutStripes(&stream, SDL_TRACKIR_3, bar, 15, 15);
    PutBytes(&stream, status_read, sizeof(status_read));
    PutStripes(&stream, SDL_TRACKIR_3, bar, 20, 6);
    PutZero(&stream, SDL_TRACKIR_3);
    CheckSplits(tir3, stream.data, stream.length);
    FreeStream(&stream);

    /* The type byte of a header cut by a whole transfer is the next read's
       first byte. An unknown one drops the rest of that read. */
    Filler(full, SDL_TRACKIR_READ_SIZE - 1);
    full[SDL_TRACKIR_READ_SIZE - 1] = 0x05;
    *state = *tir2;
    CHECK(!Feed(state, full, SDL_TRACKIR_READ_SIZE, 6000) && state->packet_length == 1);
    {
        static const uint8_t rest[15] = { 0x33, 0x00, 0x00, 0x00,
                                          0x0B, 0x1C, 0x10, 0x20, 0x22, 0x11, 0x20, 0x22, 0x00, 0x00, 0x00 };

        CHECK(!Feed(state, rest, sizeof(rest), 6001) && state->frames == 0 && Idle(state));
        CHECK(Feed(state, test1_read, sizeof(test1_read), 6002) && state->frames == 1);
    }
    /* The same cut before a known type completes the packet */
    full[SDL_TRACKIR_READ_SIZE - 1] = 0x0B;
    *state = *tir2;
    CHECK(!Feed(state, full, SDL_TRACKIR_READ_SIZE, 6000));
    CHECK(Feed(state, test1_read + 1, sizeof(test1_read) - 1, 6001));
    CHECK(Shows(state, true, 24192, 28416, 6));

    /* A read one byte short of the transfer, or one byte past it, keeps
       nothing */
    {
        uint8_t *longer = (uint8_t *)Allocate(SDL_TRACKIR_READ_SIZE + 1);

        Filler(full, SDL_TRACKIR_READ_SIZE - 2);
        full[SDL_TRACKIR_READ_SIZE - 2] = 0x0B;
        *state = *tir2;
        CHECK(!Feed(state, full, SDL_TRACKIR_READ_SIZE - 1, 6000) && Idle(state));
        CHECK(!Feed(state, test1_read + 1, sizeof(test1_read) - 1, 6001) && state->frames == 0);

        Filler(longer, SDL_TRACKIR_READ_SIZE);
        longer[SDL_TRACKIR_READ_SIZE] = 0x0B;
        *state = *tir2;
        CHECK(!Feed(state, longer, SDL_TRACKIR_READ_SIZE + 1, 6000) && Idle(state));
        free(longer);
    }

    /* A zero-length read after a whole transfer ends the transfer too, with
       or without a buffer */
    Filler(full, SDL_TRACKIR_READ_SIZE - 1);
    full[SDL_TRACKIR_READ_SIZE - 1] = 0x0B;
    *state = *tir2;
    CHECK(!Feed(state, full, SDL_TRACKIR_READ_SIZE, 6000) && state->packet_length == 1);
    CHECK(!Feed(state, full, 0, 6001) && Idle(state));
    *state = *tir2;
    CHECK(!Feed(state, full, SDL_TRACKIR_READ_SIZE, 6000) && state->packet_length == 1);
    CHECK(!SDL_TrackIR_Feed(state, NULL, 0, 6001) && Idle(state));

    free(full);
    free(state);
    free(tir3);
    free(tir2);
}

static void FeedStripes(SDL_TrackIRState *state, SDL_TrackIRModel model, const Stripe *stripes, size_t count, bool end)
{
    Stream stream;

    memset(&stream, 0, sizeof(stream));
    PutStripes(&stream, model, stripes, count, (model == SDL_TRACKIR_2) ? 84 : 63);
    if (end) {
        PutZero(&stream, model);
    }
    Feed(state, stream.data, stream.length, 7000);
    FreeStream(&stream);
}

/* Frame boundaries and blobs (image_process.c:161-213, :264-376) */
static void TestFrames(void)
{
    SDL_TrackIRState *tir2 = NewState();
    SDL_TrackIRState *tir3 = NewState();
    SDL_TrackIRState *state = NewState();

    StartRunning(tir2, SDL_TRACKIR_2);
    StartRunning(tir3, SDL_TRACKIR_3);

    /* A frame across two reads */
    {
        static const uint8_t first[8] = { 0x08, 0x1C, 0x10, 0x20, 0x22, 0x11, 0x20, 0x22 };
        static const uint8_t second[8] = { 0x08, 0x1C, 0x12, 0x20, 0x22, 0x00, 0x00, 0x00 };

        *state = *tir2;
        CHECK(!Feed(state, first, sizeof(first), 7000) && state->frames == 0);
        CHECK(Feed(state, second, sizeof(second), 7001));
        CHECK(state->frames == 1 && Shows(state, true, 24192, 28288, 9));
    }

    /* An ignored stripe on a lower line still ends the frame */
    {
        static const uint8_t read[14] = { 0x0E, 0x1C, 0x20, 0x20, 0x22, 0x00, 0x10, 0x20, 0x22, 0x18,
                                          0x00, 0x00, 0x00, 0x00 };

        *state = *tir3;
        CHECK(Feed(state, read, sizeof(read), 7000));
        CHECK(state->frames == 2 && Shows(state, false, 27778, 25984, 3));
    }

    /* The zero stripe sets the frame's line back to 0 */
    {
        static const uint8_t high[8] = { 0x08, 0x1C, 0x20, 0x20, 0x22, 0x00, 0x00, 0x00 };
        static const uint8_t low[8] = { 0x08, 0x1C, 0x05, 0x20, 0x22, 0x00, 0x00, 0x00 };

        *state = *tir2;
        CHECK(Feed(state, high, sizeof(high), 7000) && state->frames == 1);
        CHECK(Shows(state, true, 24192, 24448, 3));
        CHECK(Feed(state, low, sizeof(low), 7001) && state->frames == 2);
        CHECK(Shows(state, true, 24192, 31360, 3));
    }

    /* An empty frame shows no dot and keeps the axes */
    {
        static const uint8_t empty[5] = { 0x05, 0x1C, 0x00, 0x00, 0x00 };

        *state = *tir2;
        CHECK(Feed(state, test1_read, sizeof(test1_read), 7000));
        CHECK(Feed(state, empty, sizeof(empty), 7001));
        CHECK(state->frames == 2 && Shows(state, false, 24192, 28416, 6));
        CHECK(Feed(state, test1_read, sizeof(test1_read), 7002));
        CHECK(state->frames == 3 && Shows(state, true, 24192, 28416, 6));
    }

    /* Diagonal neighbors join on either side, and a one-pixel gap does not */
    {
        static const Stripe right[2] = { { 16, 32, 34 }, { 17, 35, 36 } };
        static const Stripe right_gap[2] = { { 16, 32, 34 }, { 17, 36, 37 } };
        static const Stripe left[2] = { { 16, 32, 34 }, { 17, 28, 31 } };
        static const Stripe left_gap[2] = { { 16, 32, 34 }, { 17, 27, 30 } };
        static const Stripe same_line[2] = { { 16, 32, 34 }, { 16, 35, 36 } };
        static const Stripe line_gap[2] = { { 16, 32, 34 }, { 18, 32, 34 } };

        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, right, 2, true);
        CHECK(Shows(state, true, 23936, 28441, 5));
        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, right_gap, 2, true);
        CHECK(Shows(state, true, 24192, 28544, 3));
        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, left, 2, true);
        CHECK(Shows(state, true, 24704, 28397, 7));
        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, left_gap, 2, true);
        CHECK(Shows(state, true, 25344, 28288, 4));
        /* Stripes on one line join only through another line */
        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, same_line, 2, true);
        CHECK(Shows(state, true, 24192, 28544, 3));
        /* A skipped line closes the blob, and the later one wins the tie */
        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, line_gap, 2, true);
        CHECK(Shows(state, true, 24192, 28032, 3));
    }

    /* Merges: a U, and three blobs joined in two steps */
    {
        static const Stripe u[3] = { { 16, 10, 12 }, { 16, 20, 22 }, { 17, 10, 22 } };
        static const Stripe three[6] = { { 16, 10, 12 }, { 16, 20, 22 }, { 16, 30, 32 },
                                         { 17, 11, 21 }, { 17, 31, 31 }, { 18, 11, 31 } };

        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, u, 3, true);
        CHECK(Shows(state, true, 28544, 28368, 19));
        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, three, 6, true);
        CHECK(Shows(state, true, 27538, 28214, 42));
    }

    /* A blob the line under it joined, and the next line leaves, still
       closes. A blob two stripes of the newest line hold closes once, so a
       tie with a blob between them goes to that blob. */
    {
        static const Stripe abandoned[3] = { { 16, 10, 14 }, { 17, 10, 14 }, { 18, 50, 52 } };
        static const Stripe twice[4] = { { 16, 10, 12 }, { 17, 10, 10 }, { 17, 30, 34 }, { 17, 12, 12 } };

        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, abandoned, 3, true);
        CHECK(Shows(state, true, 29568, 28416, 10));
        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, twice, 4, true);
        CHECK(Shows(state, true, 24448, 28288, 5));
    }

    /* The same when the blob held twice is on the previous line and no
       stripe below joins it: A, then B, and A is not closed again */
    {
        static const Stripe twice_above[5] = { { 16, 10, 20 }, { 17, 10, 10 }, { 17, 30, 42 }, { 17, 20, 20 },
                                               { 18, 60, 60 } };

        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, twice_above, 5, true);
        CHECK(Shows(state, true, 23424, 28288, 13));
    }

    /* Ties follow linuxtrack's order of closing. A blob the next line leaves
       closes at that line, before blobs still open at the frame's end. */
    {
        static const Stripe order[4] = { { 16, 10, 14 }, { 16, 50, 64 }, { 17, 10, 14 }, { 18, 10, 14 } };
        static const Stripe down[2] = { { 16, 10, 12 }, { 17, 50, 52 } };
        static const Stripe ranges[2] = { { 16, 50, 52 }, { 16, 10, 12 } };

        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, order, 4, true);
        CHECK(Shows(state, true, 29568, 28288, 15));
        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, down, 2, true);
        CHECK(Shows(state, true, 19584, 28288, 3));
        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, ranges, 2, true);
        CHECK(Shows(state, true, 29824, 28544, 3));
    }

    free(state);
    free(tir3);
    free(tir2);
}

/* The sensor's edges, the axes' range and their rounding */
static void TestBounds(void)
{
    SDL_TrackIRState *tir2 = NewState();
    SDL_TrackIRState *tir3 = NewState();
    SDL_TrackIRState *state = NewState();

    StartRunning(tir2, SDL_TRACKIR_2);
    StartRunning(tir3, SDL_TRACKIR_3);

    /* The TrackIR 3 lets in line 314 and x 440, and nothing past them */
    {
        static const Stripe line314[1] = { { 314, 0x20, 0x22 } };
        static const Stripe line315[1] = { { 315, 0x20, 0x22 } };
        static const Stripe x440[1] = { { 16, 440, 440 } };
        static const Stripe first441[1] = { { 16, 441, 441 } };
        static const Stripe last441[1] = { { 16, 0x20, 441 } };
        static const Stripe backwards[1] = { { 16, 0x22, 0x20 } };
        static const Stripe single[1] = { { 16, 0x20, 0x20 } };

        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, line314, 1, true);
        CHECK(Shows(state, true, 27778, -32768, 3));
        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, line315, 1, true);
        CHECK(state->frames == 1 && !state->in_view);
        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, x440, 1, true);
        CHECK(Shows(state, true, -32768, 29324, 1));
        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, first441, 1, true);
        CHECK(state->frames == 1 && !state->in_view);
        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, last441, 1, true);
        CHECK(state->frames == 1 && !state->in_view);
        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, backwards, 1, true);
        CHECK(state->frames == 1 && !state->in_view);
        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, single, 1, true);
        CHECK(state->in_view && state->axes[2] == 1);
    }

    /* A stripe on line 0 from x 0 is a stripe when its last x is not 0 */
    {
        static const uint8_t line0[8] = { 0x08, 0x1C, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00 };

        *state = *tir2;
        CHECK(Feed(state, line0, sizeof(line0), 7000));
        CHECK(state->frames == 1 && Shows(state, true, 32000, 32640, 6));
    }

    /* A blob that rounds to -32769 clamps to -32768: 73 pixels at x 439 and
       76 at x 440 */
    {
        Stripe column[76];
        uint32_t k;

        for (k = 0; k < 76; ++k) {
            column[k].line = 16 + k;
            column[k].first = (k < 73) ? 439 : 440;
            column[k].last = 440;
        }
        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, column, 76, true);
        CHECK(Shows(state, true, -32768, 21651, 149));
    }

    /* The corners. A pixel at line 0, x 0 needs a spare flag bit on the
       TrackIR 3, since four zero bytes end the frame. */
    {
        static const uint8_t origin3[10] = { 0x0A, 0x1C, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00 };
        static const Stripe corner2[1] = { { 1, 0, 0 } };
        static const Stripe far2[1] = { { 255, 255, 255 } };
        static const Stripe far3[1] = { { 314, 440, 440 } };
        static const Stripe inside3[1] = { { 313, 439, 439 } };
        static const Stripe edge3[1] = { { 313, 439, 440 } };
        static const Stripe edge3_lines[2] = { { 313, 439, 440 }, { 314, 439, 440 } };
        static const Stripe half3[1] = { { 16, 68, 107 } };

        *state = *tir3;
        CHECK(Feed(state, origin3, sizeof(origin3), 7000));
        CHECK(Shows(state, true, 32693, 32663, 1));
        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, corner2, 1, true);
        CHECK(Shows(state, true, 32640, 32384, 1));
        *state = *tir2;
        FeedStripes(state, SDL_TRACKIR_2, far2, 1, true);
        CHECK(Shows(state, true, -32640, -32640, 1));
        /* Past the last pixel the axes clamp */
        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, far3, 1, true);
        CHECK(Shows(state, true, -32768, -32768, 1));
        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, inside3, 1, true);
        CHECK(Shows(state, true, -32693, -32663, 1));
        /* -32767.5 rounds away from zero, and 19660.5 too */
        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, edge3, 1, true);
        CHECK(Shows(state, true, -32768, -32663, 2));
        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, edge3_lines, 2, true);
        CHECK(Shows(state, true, -32768, -32768, 4));
        *state = *tir3;
        FeedStripes(state, SDL_TRACKIR_3, half3, 1, true);
        CHECK(Shows(state, true, 19661, 29324, 40));
    }

    free(state);
    free(tir3);
    free(tir2);
}

/* The line limit: 221 stripes a line, the rest ignored. A frame of the
   most stripes the limit allows, which keeps every sum in range. */
static void TestLimits(void)
{
    SDL_TrackIRState *tir3 = NewState();
    SDL_TrackIRState *state = NewState();
    Stripe *stripes = (Stripe *)Allocate(sizeof(Stripe) * 223);
    uint32_t i;

    StartRunning(tir3, SDL_TRACKIR_3);

    /* 221 one-pixel blobs at x 0, 2 and on to 440 tie, so the last one wins.
       A 222nd at x 1 is ignored. */
    for (i = 0; i < 221; ++i) {
        stripes[i].line = 16;
        stripes[i].first = stripes[i].last = 2 * i;
    }
    stripes[221].line = 16;
    stripes[221].first = stripes[221].last = 1;
    *state = *tir3;
    FeedStripes(state, SDL_TRACKIR_3, stripes, 221, false);
    CHECK(state->range_count[1 - state->previous] == 221);
    *state = *tir3;
    FeedStripes(state, SDL_TRACKIR_3, stripes, 222, true);
    CHECK(Shows(state, true, -32768, 29324, 1));
    /* One fewer: the 220th, at x 438, is the last */
    *state = *tir3;
    FeedStripes(state, SDL_TRACKIR_3, stripes, 220, true);
    CHECK(Shows(state, true, -32544, 29324, 1));
    /* A line under it that spans the sensor joins all 221 */
    stripes[221].line = 17;
    stripes[221].first = 0;
    stripes[221].last = 440;
    *state = *tir3;
    FeedStripes(state, SDL_TRACKIR_3, stripes, 222, true);
    CHECK(Shows(state, true, -74, 29185, 662));

    /* 315 lines of 222 stripes across the sensor, one read a line: 221 a
       line count, all in one blob of 30700215 pixels, whose size clamps */
    {
        uint32_t line, k;

        *state = *tir3;
        for (line = 0; line <= 314; ++line) {
            for (k = 0; k < 222; ++k) {
                stripes[k].line = line;
                stripes[k].first = 0;
                stripes[k].last = 440;
            }
            FeedStripes(state, SDL_TRACKIR_3, stripes, 222, line == 314);
        }
        CHECK(state->frames == 1 && Shows(state, true, -74, -104, 32767));
        CHECK(state->best.pixels == 30700215u && state->best.sum_x == 6754047300ull && state->best.sum_y == 4819933755ull);
    }

    free(stripes);
    free(state);
    free(tir3);
}

/* An oracle that shares no code with the module: union-find over the
   stripes of one frame, joining stripes on consecutive lines that overlap
   or touch */

static uint32_t random_state = 0x2026u;

static uint32_t Random(uint32_t limit)
{
    random_state = random_state * 1103515245u + 12345u;
    return (random_state >> 8) % limit;
}

static int Root(int *parent, int i)
{
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

static int16_t OracleAxis(uint64_t sum, uint32_t pixels, uint32_t size)
{
    /* (size - 1) / 2 - sum / pixels, times 65535 / size, as a fraction over
       2 * pixels * size, rounded half away from zero */
    const int64_t over = 2 * (int64_t)pixels * (int64_t)size;
    const int64_t top = ((int64_t)(size - 1) * (int64_t)pixels - 2 * (int64_t)sum) * 65535;
    const int64_t magnitude = (top < 0) ? -top : top;
    int64_t quotient = magnitude / over;

    if (2 * (magnitude % over) >= over) {
        ++quotient;
    }
    if (top < 0) {
        quotient = -quotient;
    }
    return (int16_t)((quotient < -32768) ? -32768 : quotient);
}

static void TestOracle(SDL_TrackIRModel model)
{
    const uint32_t width = (model == SDL_TRACKIR_2) ? 256 : 440;
    const uint32_t height = (model == SDL_TRACKIR_2) ? 256 : 314;
    const uint32_t max_line = (model == SDL_TRACKIR_2) ? 0xFF : 0x1FF;
    const uint32_t max_x = (model == SDL_TRACKIR_2) ? 0xFF : 0x3FF;
    SDL_TrackIRState *state = NewState();
    Stripe stripes[48];
    int parent[48];
    bool accepted[48];
    uint64_t sum_x[48], sum_y[48];
    uint32_t pixels[48];
    int frame;
    int shown = 0;

    StartRunning(state, model);
    for (frame = 0; frame < 4000; ++frame) {
        const size_t count = Random(48);
        const uint32_t frames = state->frames;
        const int16_t before[3] = { state->axes[0], state->axes[1], state->axes[2] };
        Stream stream;
        uint32_t line = Random(height + 12);
        uint32_t best = 0;
        size_t i, j, made = 0;
        bool matched = false;

        /* Lines never fall within a frame. Some stripes miss the sensor. */
        while (made < count && line <= max_line) {
            Stripe *s = &stripes[made];
            uint32_t first = Random(width + 6);
            uint32_t last = first + Random(24);

            last = (last >= 2) ? last - 2 : 0;
            if (first > max_x) {
                first = max_x;
            }
            if (last > max_x) {
                last = max_x;
            }
            s->line = line;
            s->first = first;
            s->last = last;
            if (!(line == 0 && first == 0 && last == 0)) {
                ++made;
            }
            line += (Random(4) == 0) ? Random(3) : Random(2);
        }

        memset(&stream, 0, sizeof(stream));
        if (Random(3) == 0) {
            PutBytes(&stream, status_read, sizeof(status_read));
        }
        for (i = 0; i < made;) {
            const size_t per_packet = 1 + Random((model == SDL_TRACKIR_2) ? 84 : 63);
            const size_t n = (made - i < per_packet) ? made - i : per_packet;

            PutStripes(&stream, model, &stripes[i], n, n);
            if (Random(4) == 0) {
                PutBytes(&stream, status_read, sizeof(status_read));
            }
            i += n;
        }
        PutZero(&stream, model);
        CHECK(Feed(state, stream.data, stream.length, 8000));
        FreeStream(&stream);
        CHECK(state->frames == frames + 1);

        /* The oracle */
        for (i = 0; i < made; ++i) {
            const Stripe *s = &stripes[i];

            parent[i] = (int)i;
            accepted[i] = (s->line <= height && s->first <= width && s->last <= width && s->first <= s->last);
            sum_x[i] = sum_y[i] = 0;
            pixels[i] = 0;
        }
        for (i = 0; i < made; ++i) {
            if (!accepted[i]) {
                continue;
            }
            for (j = 0; j < i; ++j) {
                if (accepted[j] && stripes[j].line + 1 == stripes[i].line &&
                    (int32_t)stripes[i].first - 1 <= (int32_t)stripes[j].last &&
                    stripes[i].last + 1 >= stripes[j].first) {
                    parent[Root(parent, (int)j)] = Root(parent, (int)i);
                }
            }
        }
        for (i = 0; i < made; ++i) {
            if (accepted[i]) {
                const int root = Root(parent, (int)i);
                const uint32_t n = stripes[i].last - stripes[i].first + 1;
                uint32_t x;

                pixels[root] += n;
                for (x = stripes[i].first; x <= stripes[i].last; ++x) {
                    sum_x[root] += x;
                }
                sum_y[root] += (uint64_t)n * stripes[i].line;
            }
        }
        for (i = 0; i < made; ++i) {
            if (pixels[i] > best) {
                best = pixels[i];
            }
        }
        if (best == 0) {
            CHECK(!state->in_view && state->axes[0] == before[0] && state->axes[1] == before[1] &&
                  state->axes[2] == before[2]);
            continue;
        }
        ++shown;
        for (i = 0; i < made; ++i) {
            if (pixels[i] == best && state->axes[0] == OracleAxis(sum_x[i], pixels[i], width) &&
                state->axes[1] == OracleAxis(sum_y[i], pixels[i], height)) {
                matched = true;
            }
        }
        CHECK(state->in_view && matched && state->axes[2] == (int16_t)((best > 32767) ? 32767 : best));
    }
    /* Most frames show a dot */
    CHECK(shown > 2000);
    free(state);
}

/* The close sequences, with the driver's loop and a clock that jumps to
   each deadline */
static uint64_t DriverClose(Rig *rig, uint64_t now)
{
    uint64_t deadline;
    int guard;

    SDL_TrackIR_BeginClose(rig->state, now);
    for (guard = 0; guard < 100; ++guard) {
        Commands(rig, now);
        if (SDL_TrackIR_IsClosed(rig->state) || !SDL_TrackIR_GetDeadline(rig->state, &deadline)) {
            break;
        }
        if (deadline > now) {
            now = deadline;
        }
    }
    CHECK(SDL_TrackIR_IsClosed(rig->state));
    return now;
}

static void TestClose(void)
{
    Expected *expected = (Expected *)Allocate(sizeof(Expected) * MAX_EXPECTED);
    SDL_TrackIRState *state = NewState();
    Rig *rig = (Rig *)Allocate(sizeof(Rig));
    SDL_TrackIRCommand command;
    uint64_t end;
    int n;

    /* TrackIR 2: tir_hw.c:1156-1171 and :546-561, 401 ms */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    rig->answer = 1;
    Run(rig, 0, 728);
    CHECK(SDL_TrackIR_IsRunning(state));
    rig->count = 0;
    n = 0;
    n = Add(expected, n, 1000, 3, c_red_off);
    n = Add(expected, n, 1072, 3, c_green_off);
    n = Add(expected, n, 1144, 3, c_blue_off);
    n = Add(expected, n, 1216, 3, c_ir_off);
    n = Add(expected, n, 1288, 1, c_stop);
    n = Add(expected, n, 1358, 3, c_ir_off);
    n = Add(expected, n, 1385, 3, c_red_off);
    n = Add(expected, n, 1389, 3, c_green_off);
    n = Add(expected, n, 1393, 3, c_blue_off);
    n = Add(expected, n, 1397, 3, c_ir_off);
    end = DriverClose(rig, 1000);
    CheckSent(rig, expected, n);
    CHECK(end == 1401);
    /* Nothing more, and reads change nothing */
    CHECK(!SDL_TrackIR_NextCommand(state, 2000, &command));
    CHECK(!Feed(state, test1_read, sizeof(test1_read), 2001) && state->frames == 0);
    CHECK(!SDL_TrackIR_IsRunning(state));

    /* The same with one-millisecond updates */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    rig->answer = 1;
    Run(rig, 0, 728);
    rig->count = 0;
    SDL_TrackIR_BeginClose(state, 1000);
    Run(rig, 1000, 1400);
    CHECK(!SDL_TrackIR_IsClosed(state));
    Tick(rig, 1401);
    CHECK(SDL_TrackIR_IsClosed(state));
    CheckSent(rig, expected, n);

    /* TrackIR 3: tir_hw.c:1173-1187 and :564-585, 109 ms */
    InitRig(rig, state, SDL_TRACKIR_3, 0);
    rig->answer = 1;
    Run(rig, 0, 536);
    CHECK(SDL_TrackIR_IsRunning(state));
    rig->count = 0;
    n = 0;
    n = Add(expected, n, 1000, 2, c_video_off);
    n = Add(expected, n, 1002, 2, c_video_off);
    n = Add(expected, n, 1004, 3, c_ir_off);
    n = Add(expected, n, 1008, 1, c_fifo);
    n = Add(expected, n, 1010, 1, c_stop);
    n = Add(expected, n, 1012, 3, c_red_off);
    n = Add(expected, n, 1016, 3, c_green_off);
    n = Add(expected, n, 1020, 3, c_blue_off);
    n = Add(expected, n, 1072, 5, c_unknown4);
    n = Add(expected, n, 1097, 3, c_red_off);
    n = Add(expected, n, 1101, 3, c_green_off);
    n = Add(expected, n, 1105, 3, c_blue_off);
    end = DriverClose(rig, 1000);
    CheckSent(rig, expected, n);
    CHECK(end == 1109);

    /* A failed write ends the close */
    InitRig(rig, state, SDL_TRACKIR_3, 0);
    rig->answer = 1;
    Run(rig, 0, 536);
    rig->count = 0;
    rig->fail = 2;
    end = DriverClose(rig, 1000);
    CHECK(rig->count == 3 && end == 1004);

    /* Closing during the start-up sends the close sequence */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    Run(rig, 0, 10);
    CHECK(rig->count == 3);
    rig->count = 0;
    end = DriverClose(rig, 11);
    CHECK(rig->count == 10 && end == 11 + 401 && rig->sent[0].at == 11 &&
          memcmp(rig->sent[0].data, c_red_off, 3) == 0);
    /* A second close changes nothing */
    SDL_TrackIR_BeginClose(state, 2000);
    CHECK(SDL_TrackIR_IsClosed(state) && !SDL_TrackIR_NextCommand(state, 2000, &command));

    /* Closing before any command sends nothing */
    InitRig(rig, state, SDL_TRACKIR_3, 0);
    end = DriverClose(rig, 0);
    CHECK(rig->count == 0 && end == 0);

    /* A close drops a command left in flight */
    InitRig(rig, state, SDL_TRACKIR_2, 0);
    CHECK(SDL_TrackIR_NextCommand(state, 0, &command));
    SDL_TrackIR_BeginClose(state, 5);
    CHECK(SDL_TrackIR_NextCommand(state, 5, &command) && command.length == 3 &&
          memcmp(command.data, c_red_off, 3) == 0);

    /* A close in progress is not restarted */
    InitRig(rig, state, SDL_TRACKIR_3, 0);
    rig->answer = 1;
    Run(rig, 0, 536);
    rig->count = 0;
    SDL_TrackIR_BeginClose(state, 1000);
    Run(rig, 1000, 1005);
    CHECK(rig->count == 3);
    SDL_TrackIR_BeginClose(state, 1006);
    Run(rig, 1006, 1200);
    CHECK(rig->count == 12 && SDL_TrackIR_IsClosed(state));

    free(rig);
    free(state);
    free(expected);
}

/* 9: a reconnect starts again, and a dot in view is gone */
static void TestReconnect(void)
{
    Expected *expected = (Expected *)Allocate(sizeof(Expected) * MAX_EXPECTED);
    SDL_TrackIRState *state = NewState();
    Rig *rig = (Rig *)Allocate(sizeof(Rig));
    int n;

    CHECK(StartRunning(state, SDL_TRACKIR_2) == 728);
    CHECK(Feed(state, test1_read, sizeof(test1_read), 800));
    CHECK(Shows(state, true, 24192, 28416, 6));

    /* The device goes away and comes back: a new state at a new time */
    InitRig(rig, state, SDL_TRACKIR_2, 50000);
    CHECK(Shows(state, false, 0, 0, 0) && state->frames == 0 && !SDL_TrackIR_IsRunning(state));
    rig->answer = 1;
    n = Prologue2(expected, 50000);
    n = Add(expected, n, 50334, 2, c_request);
    n = Epilogue2(expected, n, 50410);
    RunUntilRunning(rig, 50000, 50728);
    CheckSent(rig, expected, n);
    CHECK(Feed(state, test1_read, sizeof(test1_read), 50800));
    CHECK(state->frames == 1 && Shows(state, true, 24192, 28416, 6));

    free(rig);
    free(state);
    free(expected);
}

/* Arguments the functions refuse */
static void TestArguments(void)
{
    SDL_TrackIRState *state = NewState();
    SDL_TrackIRCommand command;
    uint64_t now = StartRunning(state, SDL_TRACKIR_2);

    CHECK(!SDL_TrackIR_Feed(NULL, test1_read, sizeof(test1_read), now));
    CHECK(!SDL_TrackIR_Feed(state, NULL, 5, now) && state->frames == 0);
    CHECK(!SDL_TrackIR_Feed(state, NULL, 0, now) && state->frames == 0);
    CHECK(!SDL_TrackIR_NextCommand(NULL, now, &command));
    CHECK(!SDL_TrackIR_NextCommand(state, now, NULL));
    CHECK(!SDL_TrackIR_IsRunning(NULL) && !SDL_TrackIR_IsClosed(NULL));
    SDL_TrackIR_CommandDone(NULL, true, now);
    SDL_TrackIR_BeginClose(NULL, now);
    SDL_TrackIR_Init(NULL, SDL_TRACKIR_2, now);
    /* A completion with nothing in flight changes nothing */
    SDL_TrackIR_CommandDone(state, false, now);
    CHECK(SDL_TrackIR_IsRunning(state));
    CHECK(Feed(state, test1_read, sizeof(test1_read), now) && state->frames == 1);
    free(state);
}

int main(void)
{
    TestIdentity();
    TestVendorRules();
    TestStartSilent();
    TestStartAnswered();
    TestFlush();
    TestInfoWait();
    TestDeadlines();
    TestReplay();
    TestTruncations();
    TestSplits();
    TestFrames();
    TestBounds();
    TestLimits();
    TestOracle(SDL_TRACKIR_2);
    TestOracle(SDL_TRACKIR_3);
    TestClose();
    TestReconnect();
    TestArguments();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
