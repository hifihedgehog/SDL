/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_tacx_proto.c, the Tacx
   trainer head units of hifihedgehog/SDL#33 Part 14. Test numbers follow the
   part. The replies come byte for byte from antifier's T1942 capture
   (T1942/out.log) and T1932 capture (tacx_trainer_debug.log), both replayed
   whole, and the host frames from the TX lines of the same files. Test 9's
   legacy case and test 10, the firmware loader, cover the T1902 and the
   T1942 before its firmware, which the fork does not handle, and do not
   run. */

#include "../src/joystick/hidapi/SDL_hidapi_tacx_proto.h"
#include "../src/hidapi/SDL_hidapi_vendorusb.h"
#include "../src/joystick/usb_ids.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testtacx_captures.h"

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

#define MS(x) ((uint64_t)(x) * 1000000ULL)

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Parses hex text, with or without spaces between the bytes */
static size_t FromHex(const char *hex, uint8_t *out, size_t max)
{
    size_t n = 0;

    while (*hex == ' ') {
        ++hex;
    }
    while (hex[0] && hex[1] && n < max) {
        const int high = HexDigit(hex[0]);
        const int low = HexDigit(hex[1]);

        if (high < 0 || low < 0) {
            ++failures;
            printf("FAIL: bad hex text\n");
            return n;
        }
        out[n++] = (uint8_t)((high << 4) | low);
        hex += 2;
        while (*hex == ' ') {
            ++hex;
        }
    }
    return n;
}

/* The reply printed on this line of a capture */
static size_t CaptureReply(const TacxCaptureReply *table, size_t count, int line, uint8_t out[64])
{
    size_t i;

    for (i = 0; i < count; ++i) {
        if (table[i].line == line) {
            return FromHex(table[i].hex, out, 64);
        }
    }
    ++failures;
    printf("FAIL: no reply on line %d\n", line);
    return 0;
}

static size_t T1942(int line, uint8_t out[64])
{
    return CaptureReply(tacx_t1942_replies, TACX_T1942_REPLIES, line, out);
}

static size_t T1932(int line, uint8_t out[64])
{
    return CaptureReply(tacx_t1932_replies, TACX_T1932_REPLIES, line, out);
}

/* Each helper hands the module an exact-size heap copy, so a read at or
   past the length reaches the sanitizer */
static uint8_t *Copy(const uint8_t *bytes, size_t length)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);

    if (!copy) {
        printf("FAIL: out of memory\n");
        exit(1);
    }
    if (length) {
        memcpy(copy, bytes, length);
    }
    return copy;
}

static int Handle(SDL_TacxState *state, const uint8_t *reply, size_t length, uint64_t now)
{
    uint8_t *copy = Copy(reply, length);
    const int changed = SDL_Tacx_HandleReply(state, copy, length, now);

    free(copy);
    return changed;
}

static bool DecodeData(const uint8_t *reply, size_t length, SDL_TacxData *data)
{
    uint8_t *copy = Copy(reply, length);
    const bool result = SDL_Tacx_DecodeData(copy, length, data);

    free(copy);
    return result;
}

static bool DecodeBrake(const uint8_t *reply, size_t length, SDL_TacxBrake *brake)
{
    uint8_t *copy = Copy(reply, length);
    const bool result = SDL_Tacx_DecodeBrake(copy, length, brake);

    free(copy);
    return result;
}

/* Field by field: the structures have padding, which assignment need not
   copy */
static bool SameData(const SDL_TacxData *a, const SDL_TacxData *b)
{
    return a->unit_serial == b->unit_serial && a->unit_year == b->unit_year && a->heart_rate == b->heart_rate &&
           a->buttons == b->buttons && a->axes[0] == b->axes[0] && a->axes[1] == b->axes[1] &&
           a->axes[2] == b->axes[2] && a->axes[3] == b->axes[3] && a->distance == b->distance &&
           a->wheel_speed == b->wheel_speed && a->resistance == b->resistance && a->target == b->target &&
           a->events == b->events && a->cadence == b->cadence && a->mode == b->mode;
}

static bool SameBrake(const SDL_TacxBrake *a, const SDL_TacxBrake *b)
{
    return a->firmware == b->firmware && a->serial == b->serial && a->version2 == b->version2 &&
           a->type == b->type && a->year == b->year && a->number == b->number && a->magnetic == b->magnetic;
}

static bool SameState(const SDL_TacxState *a, const SDL_TacxState *b)
{
    return a->frame_at == b->frame_at && a->version_at == b->version_at &&
           a->version_requests == b->version_requests && a->version_known == b->version_known &&
           a->in_flight == b->in_flight && a->in_flight_version == b->in_flight_version &&
           a->present == b->present && a->data_at == b->data_at && SameData(&a->data, &b->data) &&
           SameBrake(&a->brake, &b->brake);
}

static void Controls(const SDL_TacxState *state, SDL_TacxControls *controls)
{
    SDL_Tacx_GetControls(&state->data, controls);
}

/* An independent reading of a data reply, straight from the capture's hex
   text by character position, as the part's table gives the bytes */
static unsigned int HexByte(const char *hex, int index)
{
    return (unsigned int)((HexDigit(hex[2 * index]) << 4) | HexDigit(hex[2 * index + 1]));
}

static unsigned int HexU16(const char *hex, int index)
{
    return HexByte(hex, index) | (HexByte(hex, index + 1) << 8);
}

static int HexS16(const char *hex, int index)
{
    const unsigned int value = HexU16(hex, index);
    return (value & 0x8000) ? (int)value - 65536 : (int)value;
}

static unsigned long HexU32(const char *hex, int index)
{
    return (unsigned long)HexU16(hex, index) | ((unsigned long)HexU16(hex, index + 2) << 16);
}

static bool MatchesHex(const SDL_TacxData *data, const char *hex)
{
    return data->unit_serial == HexU16(hex, 0) && data->unit_year == HexByte(hex, 8) &&
           data->heart_rate == HexByte(hex, 12) && data->buttons == HexByte(hex, 13) &&
           data->axes[0] == HexU16(hex, 16) && data->axes[1] == HexU16(hex, 18) &&
           data->axes[2] == HexU16(hex, 20) && data->axes[3] == HexU16(hex, 22) &&
           data->distance == HexU32(hex, 28) && data->wheel_speed == HexU16(hex, 32) &&
           data->resistance == HexS16(hex, 38) && data->target == HexS16(hex, 40) &&
           data->events == HexByte(hex, 42) && data->cadence == HexByte(hex, 44) && data->mode == HexByte(hex, 46);
}

/* FortiusANT's split of a brake serial, restated with strings: the decimal
   text, the first two characters the type, the next two the year and the
   rest the number, each 0 when missing */
static void ReferenceSplit(unsigned long serial, unsigned int *type, unsigned int *year, unsigned long *number)
{
    char text[16];
    size_t length;

    snprintf(text, sizeof(text), "%lu", serial);
    length = strlen(text);
    *type = 0;
    *year = 0;
    *number = 0;
    {
        char part[16];

        memset(part, 0, sizeof(part));
        memcpy(part, text, length < 2 ? length : 2);
        *type = (unsigned int)strtoul(part, NULL, 10);
    }
    if (length > 2) {
        char part[16];

        memset(part, 0, sizeof(part));
        memcpy(part, text + 2, length < 4 ? length - 2 : 2);
        *year = (unsigned int)strtoul(part, NULL, 10);
    }
    if (length > 4) {
        *number = strtoul(text + 4, NULL, 10);
    }
}

/* Builds a brake version reply: the head unit block of T1942 line 3 and the
   given brake block */
static size_t BrakeReply(uint32_t firmware, uint32_t serial, uint16_t version2, uint8_t out[64])
{
    const size_t length = T1942(3, out);
    int i;

    for (i = 0; i < 4; ++i) {
        out[28 + i] = (uint8_t)(firmware >> (8 * i));
        out[32 + i] = (uint8_t)(serial >> (8 * i));
    }
    out[36] = (uint8_t)(version2 & 0xFF);
    out[37] = (uint8_t)(version2 >> 8);
    return length;
}

/* The part's mapping and the module's sizes, headers and timing, read at run
   time so each value is checked where the tree defines it */
static void TestConstants(void)
{
    const int axes[5] = { SDL_TACX_AXIS_STEERING, SDL_TACX_AXIS_WHEEL_SPEED, SDL_TACX_AXIS_CADENCE,
                          SDL_TACX_AXIS_HEART_RATE, SDL_TACX_AXIS_RESISTANCE };
    const int buttons[4] = { SDL_TACX_BUTTON_ENTER, SDL_TACX_BUTTON_DOWN, SDL_TACX_BUTTON_UP, SDL_TACX_BUTTON_CANCEL };
    const unsigned long sizes[6] = { SDL_TACX_FRAME_LENGTH, SDL_TACX_VERSION_LENGTH, SDL_TACX_READ_SIZE,
                                     SDL_TACX_REPLY_MINIMUM, SDL_TACX_AXES, SDL_TACX_BUTTONS };
    const unsigned long expected_sizes[6] = { 12, 4, 64, 48, 5, 4 };
    const unsigned long headers[2] = { SDL_TACX_HEADER_DATA, SDL_TACX_HEADER_VERSION };
    const int modes[3] = { SDL_TACX_MODE_STOP, SDL_TACX_MODE_RESISTANCE, SDL_TACX_MODE_CALIBRATE };
    const unsigned long long timing[3] = { SDL_TACX_FRAME_INTERVAL_NS, SDL_TACX_VERSION_RETRY_NS, SDL_TACX_TIMEOUT_NS };
    const int requests = SDL_TACX_VERSION_REQUESTS;
    const int ids[3] = { SDL_TACX_VENDOR, SDL_TACX_PRODUCT_T1904, SDL_TACX_PRODUCT_T1932 };
    const int tree_ids[3] = { USB_VENDOR_TACX, USB_PRODUCT_TACX_T1904, USB_PRODUCT_TACX_T1932 };
    int i;

    for (i = 0; i < 5; ++i) {
        CHECK(axes[i] == i);
    }
    for (i = 0; i < 4; ++i) {
        CHECK(buttons[i] == i);
    }
    for (i = 0; i < 6; ++i) {
        CHECK(sizes[i] == expected_sizes[i]);
    }
    CHECK(headers[0] == 0x00021303ul && headers[1] == 0x00000C03ul);
    CHECK(modes[0] == 0x00 && modes[1] == 0x02 && modes[2] == 0x03);
    CHECK(timing[0] == MS(100) && timing[1] == MS(500) && timing[2] == MS(1000));
    CHECK(requests == 6);
    CHECK(ids[0] == 0x3561 && ids[1] == 0x1904 && ids[2] == 0x1932);
    /* The module's IDs are the ones usb_ids.h gives the tree */
    for (i = 0; i < 3; ++i) {
        CHECK(ids[i] == tree_ids[i]);
    }
}

/* Identity, the vendor rules and test 12's IDs */
static void TestIdentify(void)
{
    uint32_t product, vendor;
    int claimed = 0, foreign = 0;

    CHECK(SDL_Tacx_Identify(0x3561, 0x1904) == SDL_TACX_T1904);
    CHECK(SDL_Tacx_Identify(0x3561, 0x1932) == SDL_TACX_T1932);
    /* Outside the ticket: they need Tacx firmware from the host, and the
       T1942 is outside it after its firmware too, since only the T1904
       and T1932 are part of it */
    CHECK(SDL_Tacx_Identify(0x3561, 0x1942) == SDL_TACX_NONE);
    CHECK(SDL_Tacx_Identify(0x3561, 0x1902) == SDL_TACX_NONE);
    CHECK(SDL_Tacx_Identify(0x3561, 0xE6BE) == SDL_TACX_NONE);
    CHECK(SDL_Tacx_Identify(0x0547, 0x2131) == SDL_TACX_NONE);

    CHECK(strcmp(SDL_Tacx_Name(SDL_TACX_T1904), "Tacx T1904") == 0);
    CHECK(strcmp(SDL_Tacx_Name(SDL_TACX_T1932), "Tacx T1932") == 0);
    CHECK(SDL_Tacx_Name(SDL_TACX_NONE) == NULL);

    /* Exactly two IDs under the vendor, and the same products under any
       other vendor are nothing */
    for (product = 0; product <= 0xFFFF; ++product) {
        if (SDL_Tacx_Identify(0x3561, (uint16_t)product) != SDL_TACX_NONE) {
            ++claimed;
        }
    }
    for (vendor = 0; vendor <= 0xFFFF; ++vendor) {
        if (vendor != 0x3561 && (SDL_Tacx_Identify((uint16_t)vendor, 0x1904) != SDL_TACX_NONE ||
                                 SDL_Tacx_Identify((uint16_t)vendor, 0x1932) != SDL_TACX_NONE)) {
            ++foreign;
        }
    }
    CHECK(claimed == 2);
    CHECK(foreign == 0);
}

/* Constructed descriptors: no source records the head units' descriptors,
   only the endpoint addresses 82 and 02 */
static const unsigned char bulk64[] = {
    9, 4, 0, 0, 2, 0xFF, 0, 0, 0,
    7, 5, 0x82, 2, 64, 0, 0,
    7, 5, 0x02, 2, 64, 0, 0,
};
static const unsigned char bulk32[] = {
    9, 4, 0, 0, 2, 0xFF, 0, 0, 0,
    7, 5, 0x02, 2, 32, 0, 0,
    7, 5, 0x82, 2, 32, 0, 0,
};
static const unsigned char interrupt64[] = {
    9, 4, 0, 0, 2, 0xFF, 0, 0, 0,
    7, 5, 0x82, 3, 64, 0, 10,
    7, 5, 0x02, 3, 64, 0, 10,
};
static const unsigned char high_speed[] = {
    9, 4, 0, 0, 2, 0xFF, 0, 0, 0,
    7, 5, 0x82, 2, 0x00, 0x02, 0,
    7, 5, 0x02, 2, 0x00, 0x02, 0,
};
static const unsigned char other_addresses[] = {
    9, 4, 0, 0, 2, 0xFF, 0, 0, 0,
    7, 5, 0x81, 2, 64, 0, 0,
    7, 5, 0x01, 2, 64, 0, 0,
};
static const unsigned char no_out[] = {
    9, 4, 0, 0, 1, 0xFF, 0, 0, 0,
    7, 5, 0x82, 2, 64, 0, 0,
};

static bool Select(const SDL_VendorUSBRule *rule, const unsigned char *descriptors, size_t length,
                   SDL_VendorUSBSelection *selection)
{
    unsigned char *copy = (unsigned char *)Copy(descriptors, length);
    const bool result = SDL_VendorUSB_SelectEndpoints(rule, 0, copy, length, selection);

    free(copy);
    return result;
}

static void TestVendorRules(void)
{
    static const uint16_t products[2] = { 0x1904, 0x1932 };
    static const uint8_t classes[3] = { 0xFF, 0x00, 0x03 };
    const SDL_VendorUSBPlatform platforms[3] = {
        SDL_VENDORUSB_PLATFORM_WINDOWS, SDL_VENDORUSB_PLATFORM_OTHER, SDL_VENDORUSB_PLATFORM_MACOS
    };
    size_t p, c, k;

    for (p = 0; p < 2; ++p) {
        const SDL_VendorUSBRule *rule = NULL;
        SDL_VendorUSBSelection selection;

        /* Interface 0, whatever its class */
        for (c = 0; c < 3; ++c) {
            rule = SDL_VendorUSB_FindRule(0x3561, products[p], 0, classes[c], 0, 0);
            CHECK(rule != NULL);
        }
        CHECK(SDL_VendorUSB_FindRule(0x3561, products[p], 1, 0xFF, 0, 0) == NULL);
        CHECK(SDL_VendorUSB_IsVendorDevice(0x3561, products[p]));
        if (!rule) {
            continue;
        }
        CHECK(rule->flags == (SDL_VENDORUSB_RAW_OUTPUT | SDL_VENDORUSB_WINDOWS_ONLY));
        CHECK(rule->interface_number == 0 && rule->alternate == 0);
        CHECK(rule->in_endpoint == 0x82 && rule->out_endpoint == 0x02);
        CHECK(rule->in_size == 0 && rule->out_size == 0);
        CHECK(rule->read_size == SDL_TACX_READ_SIZE);

        for (k = 0; k < 3; ++k) {
            const bool windows = (platforms[k] == SDL_VENDORUSB_PLATFORM_WINDOWS);
            SDL_VendorUSBRouting routing;

            /* Windows only, like every rule of Part 14. Only the interface's
               rule takes the head unit to libusb. */
            CHECK(SDL_VendorUSB_RuleApplies(rule, platforms[k]) == windows);
            CHECK(!SDL_VendorUSB_RequiresLibUSB(platforms[k], 0x3561, products[p], false, false));
            CHECK(SDL_VendorUSB_IsCandidate(platforms[k], 0x3561, products[p], 0, 0xFF, 0, 0, false) == windows);
            CHECK(SDL_VendorUSB_IsCandidate(platforms[k], 0x3561, products[p], 1, 0x03, 0, 0, false) == !windows);
            /* On Windows the platform backend leaves it, and libusb keeps it
               with the whitelist on or off. Elsewhere nothing changes. */
            memset(&routing, 0, sizeof(routing));
            routing.platform = platforms[k];
            routing.vendor = 0x3561;
            routing.product = products[p];
            routing.gamecube = true;
            CHECK(SDL_VendorUSB_Ignore(&routing) == windows);
            routing.libusb = true;
            routing.vendor_interface = SDL_VendorUSB_RuleApplies(rule, platforms[k]);
            routing.whitelist = true;
            CHECK(SDL_VendorUSB_Ignore(&routing) == !windows);
            routing.whitelist = false;
            CHECK(!SDL_VendorUSB_Ignore(&routing));
        }

        /* Bulk endpoints of 64 bytes, read 64 bytes at a time */
        memset(&selection, 0, sizeof(selection));
        CHECK(Select(rule, bulk64, sizeof(bulk64), &selection));
        CHECK(selection.alternate == 0);
        CHECK(selection.in.address == 0x82 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_BULK && selection.in.read_size == 64);
        CHECK(selection.out.address == 0x02 && selection.out.transfer == SDL_VENDORUSB_TRANSFER_BULK);
        /* Bulk packets of 32 bytes, OUT first: a read still holds a whole reply */
        memset(&selection, 0, sizeof(selection));
        CHECK(Select(rule, bulk32, sizeof(bulk32), &selection));
        CHECK(selection.in.address == 0x82 && selection.in.max_packet_size == 32 && selection.in.read_size == 64);
        CHECK(selection.out.address == 0x02);
        /* Interrupt endpoints are read one packet at a time */
        memset(&selection, 0, sizeof(selection));
        CHECK(Select(rule, interrupt64, sizeof(interrupt64), &selection));
        CHECK(selection.in.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT && selection.in.read_size == 64);
        CHECK(selection.out.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT);
        /* The head units are USB 1.1. A 512-byte bulk endpoint cannot be read
           in whole packets of 64 bytes, so it does not open. */
        CHECK(!Select(rule, high_speed, sizeof(high_speed), &selection));
        /* The named endpoints are required */
        CHECK(!Select(rule, other_addresses, sizeof(other_addresses), &selection));
        CHECK(!Select(rule, no_out, sizeof(no_out), &selection));
    }

    /* Outside the ticket: no rule, and not a vendor device. The T1942 is
       outside it after its firmware too. */
    CHECK(SDL_VendorUSB_FindRule(0x3561, 0x1942, 0, 0xFF, 0, 0) == NULL);
    CHECK(!SDL_VendorUSB_IsVendorDevice(0x3561, 0x1942));
    CHECK(SDL_VendorUSB_FindRule(0x3561, 0x1902, 0, 0xFF, 0, 0) == NULL);
    CHECK(SDL_VendorUSB_FindRule(0x3561, 0xE6BE, 0, 0xFF, 0, 0) == NULL);
    CHECK(SDL_VendorUSB_FindRule(0x0547, 0x2131, 0, 0xFF, 0, 0) == NULL);
    CHECK(!SDL_VendorUSB_IsVendorDevice(0x3561, 0x1902));
    CHECK(!SDL_VendorUSB_IsVendorDevice(0x3561, 0xE6BE));
    CHECK(!SDL_VendorUSB_IsVendorDevice(0x0547, 0x2131));
    CHECK(!SDL_VendorUSB_RequiresLibUSB(SDL_VENDORUSB_PLATFORM_WINDOWS, 0x3561, 0xE6BE, false, false));
    CHECK(!SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_WINDOWS, 0x3561, 0x1902, 0, 0xFF, 0, 0, false));
    CHECK(!SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_OTHER, 0x0547, 0x2131, 0, 0xFF, 0, 0, false));
}

/* 7, the host frames */
static void TestFrames(void)
{
    /* TX lines of the two captures: every one of them sends mode 2, weight
       0x52 and calibration 0x0410, with the target the capture prints and
       the pedal echo its byte 6 carries */
    static const struct
    {
        bool t1932;
        int line;
        int target;
        const char *bytes;
    } frames[] = {
        { false, 2, -3273, "01 08 01 00 37 f3 00 00 02 52 10 04" },
        { false, 164, -2866, "01 08 01 00 ce f4 01 00 02 52 10 04" },
        { false, 1310, 0, "01 08 01 00 00 00 00 00 02 52 10 04" },
        { false, 1312, 5, "01 08 01 00 05 00 01 00 02 52 10 04" },
        { false, 1464, 385, "01 08 01 00 81 01 00 00 02 52 10 04" },
        { false, 3854, 6361, "01 08 01 00 d9 18 04 00 02 52 10 04" },
        { false, 5364, 10137, "01 08 01 00 99 27 00 00 02 52 10 04" },
        { true, 3, -3273, "01 08 01 00 37 f3 00 00 02 52 10 04" },
        { true, 57, -3136, "01 08 01 00 c0 f3 00 00 02 52 10 04" },
    };
    uint8_t out[SDL_TACX_FRAME_LENGTH], expected[SDL_TACX_FRAME_LENGTH];
    size_t i;

    /* The part's case: line 2 from target -3273, echo 0, mode 2, weight 82
       and calibration 1040 */
    SDL_Tacx_EncodeFrame(-3273, 0, 2, 82, 1040, out);
    CHECK(FromHex("01 08 01 00 37 F3 00 00 02 52 10 04", expected, sizeof(expected)) == 12);
    CHECK(memcmp(out, expected, 12) == 0);

    for (i = 0; i < sizeof(frames) / sizeof(frames[0]); ++i) {
        CHECK(FromHex(frames[i].bytes, expected, sizeof(expected)) == 12);
        SDL_Tacx_EncodeFrame((int16_t)frames[i].target, expected[6], SDL_TACX_MODE_RESISTANCE, 0x52, 0x0410, out);
        CHECK(memcmp(out, expected, 12) == 0);
    }

    /* The stop frame FortiusANT sends while idle, and TotalReverse's tool to
       stop the brake: the command and eleven zeros after it */
    {
        static const uint8_t stop[12] = { 0x01, 0x08, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0 };

        SDL_Tacx_EncodeFrame(0, 0, SDL_TACX_MODE_STOP, 0, 0, out);
        CHECK(memcmp(out, stop, 12) == 0);
    }

    /* Each field alone, at its extremes */
    {
        static const uint8_t low[12] = { 0x01, 0x08, 0x01, 0x00, 0x00, 0x80, 0xFF, 0x00, 0x03, 0xFF, 0xCD, 0xAB };
        static const uint8_t high[12] = { 0x01, 0x08, 0x01, 0x00, 0xFF, 0x7F, 0x04, 0x00, 0xFE, 0x0A, 0xFF, 0xFF };

        SDL_Tacx_EncodeFrame(-32768, 0xFF, SDL_TACX_MODE_CALIBRATE, 0xFF, 0xABCD, out);
        CHECK(memcmp(out, low, 12) == 0);
        SDL_Tacx_EncodeFrame(32767, 0x04, 0xFE, 0x0A, 0xFFFF, out);
        CHECK(memcmp(out, high, 12) == 0);
        SDL_Tacx_EncodeFrame(-1, 0, 0, 0, 0x0001, out);
        CHECK(out[4] == 0xFF && out[5] == 0xFF && out[10] == 0x01 && out[11] == 0x00);
        SDL_Tacx_EncodeFrame(256, 0, 0, 0, 0x0100, out);
        CHECK(out[4] == 0x00 && out[5] == 0x01 && out[10] == 0x00 && out[11] == 0x01);
    }

    /* The version request */
    {
        uint8_t version[SDL_TACX_VERSION_LENGTH];

        memset(version, 0xAA, sizeof(version));
        SDL_Tacx_VersionRequest(version);
        CHECK(version[0] == 0x02 && version[1] == 0x00 && version[2] == 0x00 && version[3] == 0x00);
    }
}

/* 1 */
static void TestShortReply(void)
{
    SDL_TacxState state, before;
    SDL_TacxData data;
    SDL_TacxBrake brake;
    uint8_t reply[64], whole[64];
    size_t length;

    length = T1942(1, reply);
    CHECK(length == 24);
    CHECK(reply[0] == 0x0B && reply[1] == 0x17 && reply[16] == 0xEE && reply[23] == 0x05);
    SDL_Tacx_Init(&state, MS(100));
    before = state;
    CHECK(Handle(&state, reply, length, MS(200)) == 0);
    CHECK(SameState(&state, &before) && !state.present);
    CHECK(!DecodeData(reply, length, &data));
    CHECK(!DecodeBrake(reply, length, &brake));

    /* Dropped even when the buffer past it holds a whole data reply */
    CHECK(T1942(5, whole) == 48);
    memcpy(whole, reply, 24);
    CHECK(SDL_Tacx_HandleReply(&state, whole, 24, MS(300)) == 0);
    CHECK(SameState(&state, &before));
    CHECK(Handle(&state, whole, 48, MS(300)) == (SDL_TACX_CHANGED_PRESENT | SDL_TACX_CHANGED_CONTROLS));
}

/* 2 and the version reply of 8 */
static void TestBrakeVersion(void)
{
    SDL_TacxState state;
    SDL_TacxBrake brake;
    uint8_t reply[64];
    size_t length;

    /* T1942 line 3: header 03 0C 00 00, firmware 07 10 00 00, serial
       5C C9 7A 18, which is 410700124: a T1941 motor brake of 2007, unit
       124 */
    length = T1942(3, reply);
    CHECK(length == 48);
    CHECK(reply[24] == 0x03 && reply[25] == 0x0C && reply[26] == 0x00 && reply[27] == 0x00);
    CHECK(DecodeBrake(reply, length, &brake));
    CHECK(brake.firmware == 0x00001007);
    CHECK(brake.serial == 410700124u);
    CHECK(brake.version2 == 0x010B);
    CHECK(brake.type == 41 && brake.year == 7 && brake.number == 124);
    CHECK(!brake.magnetic);
    {
        SDL_TacxData data;
        CHECK(!DecodeData(reply, length, &data));
    }

    SDL_Tacx_Init(&state, 0);
    CHECK(Handle(&state, reply, length, MS(250)) == SDL_TACX_CHANGED_BRAKE);
    CHECK(state.version_known && !state.present);
    CHECK(SameBrake(&state.brake, &brake));
    /* The same reply again changes nothing the driver logs */
    CHECK(Handle(&state, reply, length, MS(500)) == 0);
    CHECK(state.version_known && !state.present && state.data_at == 0);

    /* T1932 line 4: firmware 02 19 00 00 and serial 0, which FortiusANT
       reads as a magnetic brake */
    length = T1932(4, reply);
    CHECK(length == 64);
    CHECK(DecodeBrake(reply, length, &brake));
    CHECK(brake.firmware == 0x00001902 && brake.serial == 0 && brake.version2 == 0);
    CHECK(brake.magnetic && brake.type == 0 && brake.year == 0 && brake.number == 0);
    /* A different brake is news */
    CHECK(Handle(&state, reply, length, MS(750)) == SDL_TACX_CHANGED_BRAKE);
    CHECK(state.brake.magnetic);
    /* So is any one field that differs */
    length = BrakeReply(0x00001902, 0, 0, reply);
    CHECK(Handle(&state, reply, length, MS(800)) == 0);
    length = BrakeReply(0x00001902, 410700124u, 0, reply);
    CHECK(Handle(&state, reply, length, MS(810)) == SDL_TACX_CHANGED_BRAKE && !state.brake.magnetic);
    length = BrakeReply(0x00001902, 410700124u, 0x010B, reply);
    CHECK(Handle(&state, reply, length, MS(820)) == SDL_TACX_CHANGED_BRAKE && state.brake.version2 == 0x010B);
    length = BrakeReply(0x00001007, 410700124u, 0x010B, reply);
    CHECK(Handle(&state, reply, length, MS(830)) == SDL_TACX_CHANGED_BRAKE && state.brake.firmware == 0x00001007);
    CHECK(Handle(&state, reply, length, MS(840)) == 0);

    /* The version answer in TotalReverse's notes: firmware 00000965 and
       serial 1877C4BA, 41.05.02330 */
    length = BrakeReply(0x00000965, 0x1877C4BA, 0x0C08, reply);
    CHECK(DecodeBrake(reply, length, &brake));
    CHECK(brake.serial == 410502330u && brake.type == 41 && brake.year == 5 && brake.number == 2330);
    CHECK(brake.firmware == 0x00000965 && brake.version2 == 0x0C08 && !brake.magnetic);

    /* The serial split, against FortiusANT's string slices */
    {
        static const uint32_t serials[] = {
            0, 1, 5, 9, 10, 41, 99, 100, 410, 999, 1000, 4107, 9999, 10000, 41070, 99999, 100000,
            410700124u, 410502330u, 460100001u, 490000000u, 999999999u, 1000000000u, 4294967295u,
        };
        size_t i;
        uint32_t serial;
        int mismatches = 0;

        for (i = 0; i < sizeof(serials) / sizeof(serials[0]); ++i) {
            unsigned int type, year;
            unsigned long number;

            length = BrakeReply(0, serials[i], 0, reply);
            CHECK(DecodeBrake(reply, length, &brake));
            ReferenceSplit(serials[i], &type, &year, &number);
            CHECK(brake.type == type && brake.year == year && brake.number == number);
            CHECK(brake.magnetic == (serials[i] == 0));
        }
        /* A walk through the whole range */
        serial = 7;
        for (i = 0; i < 200000; ++i) {
            unsigned int type, year;
            unsigned long number;

            serial = serial * 1664525u + 1013904223u;
            length = BrakeReply(0, serial >> (i % 29), 0, reply);
            if (!SDL_Tacx_DecodeBrake(reply, length, &brake)) {
                ++mismatches;
                continue;
            }
            ReferenceSplit(serial >> (i % 29), &type, &year, &number);
            if (brake.type != type || brake.year != year || brake.number != number) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
        CHECK(serials[23] == 4294967295u);
        length = BrakeReply(0, 4294967295u, 0, reply);
        CHECK(DecodeBrake(reply, length, &brake) && brake.type == 42 && brake.year == 94 && brake.number == 967295);
    }
}

/* 3, 4, 5, 6 and the data reply of 8 */
static void TestDataReplies(void)
{
    SDL_TacxState state;
    SDL_TacxControls controls;
    uint8_t reply[64];
    size_t length;

    SDL_Tacx_Init(&state, 0);

    /* 3: line 5, speed 0, cadence 0, heart rate 0, no buttons, target echo
       F337 */
    length = T1942(5, reply);
    CHECK(length == 48);
    CHECK(Handle(&state, reply, length, MS(500)) == (SDL_TACX_CHANGED_PRESENT | SDL_TACX_CHANGED_CONTROLS));
    CHECK(state.present && state.data_at == MS(500));
    CHECK(state.data.wheel_speed == 0 && state.data.cadence == 0 && state.data.heart_rate == 0);
    CHECK(state.data.buttons == 0);
    CHECK(state.data.target == -3273 && (uint16_t)state.data.target == 0xF337);
    CHECK(state.data.unit_serial == 0x170B && state.data.unit_year == 6);
    CHECK(state.data.axes[0] == 0x025A && state.data.axes[1] == 0x03D7 && state.data.axes[2] == 0x07D0 && state.data.axes[3] == 0x07D0);
    CHECK(state.data.distance == 0x2B28 && state.data.resistance == 0 && state.data.events == 0 && state.data.mode == 2);
    Controls(&state, &controls);
    CHECK(controls.axes[SDL_TACX_AXIS_STEERING] == 0x03D7 - 32768);
    CHECK(controls.axes[SDL_TACX_AXIS_WHEEL_SPEED] == 0 && controls.axes[SDL_TACX_AXIS_CADENCE] == 0);
    CHECK(controls.axes[SDL_TACX_AXIS_HEART_RATE] == 0 && controls.axes[SDL_TACX_AXIS_RESISTANCE] == 0);
    CHECK(controls.buttons == 0);

    /* 4: line 161, heart rate 104, speed 0CC7, 11.3 km/h by 289.75, and
       current resistance 0154 */
    length = T1942(161, reply);
    CHECK(Handle(&state, reply, length, MS(750)) == SDL_TACX_CHANGED_CONTROLS);
    CHECK(state.data.heart_rate == 104 && state.data.wheel_speed == 0x0CC7 && state.data.resistance == 0x0154);
    CHECK((int)(state.data.wheel_speed * 10.0 / 289.75 + 0.5) == 113);
    Controls(&state, &controls);
    CHECK(controls.axes[SDL_TACX_AXIS_HEART_RATE] == 104 * 128);
    CHECK(controls.axes[SDL_TACX_AXIS_WHEEL_SPEED] == 3271 && controls.axes[SDL_TACX_AXIS_RESISTANCE] == 340);

    /* 5: line 185, speed 1208, cadence 16, heart rate 108, events 01 */
    length = T1942(185, reply);
    CHECK(Handle(&state, reply, length, MS(1000)) == SDL_TACX_CHANGED_CONTROLS);
    CHECK(state.data.wheel_speed == 4616 && state.data.cadence == 22 && state.data.heart_rate == 108);
    CHECK(state.data.events == 0x01);
    Controls(&state, &controls);
    CHECK(controls.axes[SDL_TACX_AXIS_CADENCE] == 22 * 128 && controls.axes[SDL_TACX_AXIS_HEART_RATE] == 108 * 128);
    CHECK(controls.axes[SDL_TACX_AXIS_WHEEL_SPEED] == 4616);

    /* 6: line 3853, heart rate 178, speed 0869, current resistance 0365,
       target 18CF, events 04, which TotalReverse reads as the brake
       stopping */
    length = T1942(3853, reply);
    CHECK(Handle(&state, reply, length, MS(1250)) == SDL_TACX_CHANGED_CONTROLS);
    CHECK(state.data.heart_rate == 178 && state.data.wheel_speed == 0x0869 && state.data.resistance == 0x0365);
    CHECK(state.data.target == 0x18CF && state.data.events == 0x04);
    Controls(&state, &controls);
    CHECK(controls.axes[SDL_TACX_AXIS_HEART_RATE] == 22784 && controls.axes[SDL_TACX_AXIS_RESISTANCE] == 869);

    /* 8: T1932 line 2, axis 1 0A0D with no steering unit, current
       resistance 0F 04, mode echo 02 */
    length = T1932(2, reply);
    CHECK(length == 64);
    CHECK(reply[18] == 0x0D && reply[19] == 0x0A);
    SDL_Tacx_Init(&state, 0);
    CHECK(Handle(&state, reply, length, MS(10)) == (SDL_TACX_CHANGED_PRESENT | SDL_TACX_CHANGED_CONTROLS));
    CHECK(state.data.axes[1] == 0x0A0D && state.data.resistance == 1039 && state.data.mode == 0x02);
    CHECK(state.data.unit_serial == 0xA90E && state.data.unit_year == 12);
    Controls(&state, &controls);
    CHECK(controls.axes[SDL_TACX_AXIS_STEERING] == 0x0A0D - 32768 && controls.axes[SDL_TACX_AXIS_RESISTANCE] == 1039);
}

/* 9 */
static void TestButtons(void)
{
    static const struct
    {
        uint8_t byte13;
        int button;
    } single[] = {
        { 0x01, SDL_TACX_BUTTON_ENTER },
        { 0x02, SDL_TACX_BUTTON_DOWN },
        { 0x04, SDL_TACX_BUTTON_UP },
        { 0x08, SDL_TACX_BUTTON_CANCEL },
    };
    SDL_TacxState state;
    SDL_TacxControls controls, rest;
    uint8_t reply[64];
    size_t length, i;
    int value, mismatches = 0;

    length = T1942(185, reply);
    SDL_Tacx_Init(&state, 0);
    CHECK(Handle(&state, reply, length, 0) & SDL_TACX_CHANGED_CONTROLS);
    Controls(&state, &rest);
    CHECK(rest.buttons == 0);

    for (i = 0; i < sizeof(single) / sizeof(single[0]); ++i) {
        reply[13] = single[i].byte13;
        CHECK(Handle(&state, reply, length, 0) == SDL_TACX_CHANGED_CONTROLS);
        Controls(&state, &controls);
        CHECK(controls.buttons == (1u << single[i].button));
        CHECK(memcmp(controls.axes, rest.axes, sizeof(rest.axes)) == 0);
    }
    reply[13] = 0x00;
    CHECK(Handle(&state, reply, length, 0) == SDL_TACX_CHANGED_CONTROLS);
    Controls(&state, &controls);
    CHECK(controls.buttons == 0);

    /* Every value of byte 13: bits 0 to 3 are the buttons, bits 4 to 7 press
       nothing, and no other control moves */
    for (value = 0; value < 256; ++value) {
        reply[13] = (uint8_t)value;
        if (!(SDL_Tacx_HandleReply(&state, reply, length, 0) & SDL_TACX_CHANGED_CONTROLS)) {
            ++mismatches;
            continue;
        }
        Controls(&state, &controls);
        if (controls.buttons != (value & 0x0F) || memcmp(controls.axes, rest.axes, sizeof(rest.axes)) != 0) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

/* The axes over the full range of each input, with the clamps */
static void TestAxes(void)
{
    SDL_TacxData data;
    SDL_TacxControls controls, rest;
    uint8_t reply[64];
    size_t length;
    int32_t value;
    int mismatches;

    length = T1942(185, reply);
    CHECK(DecodeData(reply, length, &data));
    SDL_Tacx_GetControls(&data, &rest);

    /* Steering: axis 1 less 32768, which spans the axis exactly */
    mismatches = 0;
    for (value = 0; value <= 0xFFFF; ++value) {
        reply[18] = (uint8_t)(value & 0xFF);
        reply[19] = (uint8_t)(value >> 8);
        if (!SDL_Tacx_DecodeData(reply, length, &data)) {
            ++mismatches;
            continue;
        }
        SDL_Tacx_GetControls(&data, &controls);
        if (controls.axes[SDL_TACX_AXIS_STEERING] != value - 32768 ||
            controls.axes[SDL_TACX_AXIS_WHEEL_SPEED] != rest.axes[SDL_TACX_AXIS_WHEEL_SPEED]) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    reply[18] = 0x00;
    reply[19] = 0x00;
    CHECK(DecodeData(reply, length, &data));
    SDL_Tacx_GetControls(&data, &controls);
    CHECK(controls.axes[SDL_TACX_AXIS_STEERING] == -32768);
    reply[18] = 0xFF;
    reply[19] = 0xFF;
    CHECK(DecodeData(reply, length, &data));
    SDL_Tacx_GetControls(&data, &controls);
    CHECK(controls.axes[SDL_TACX_AXIS_STEERING] == 32767);
    reply[18] = 0x00;
    reply[19] = 0x80;
    CHECK(DecodeData(reply, length, &data));
    SDL_Tacx_GetControls(&data, &controls);
    CHECK(controls.axes[SDL_TACX_AXIS_STEERING] == 0);

    /* Wheel speed: the raw value, held at 32767 above it */
    length = T1942(185, reply);
    mismatches = 0;
    for (value = 0; value <= 0xFFFF; ++value) {
        reply[32] = (uint8_t)(value & 0xFF);
        reply[33] = (uint8_t)(value >> 8);
        if (!SDL_Tacx_DecodeData(reply, length, &data)) {
            ++mismatches;
            continue;
        }
        SDL_Tacx_GetControls(&data, &controls);
        if (controls.axes[SDL_TACX_AXIS_WHEEL_SPEED] != (value > 32767 ? 32767 : value) || data.wheel_speed != value) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    reply[32] = 0xFF;
    reply[33] = 0x7F;
    CHECK(DecodeData(reply, length, &data));
    SDL_Tacx_GetControls(&data, &controls);
    CHECK(controls.axes[SDL_TACX_AXIS_WHEEL_SPEED] == 32767);
    reply[32] = 0x00;
    reply[33] = 0x80;
    CHECK(DecodeData(reply, length, &data));
    SDL_Tacx_GetControls(&data, &controls);
    CHECK(controls.axes[SDL_TACX_AXIS_WHEEL_SPEED] == 32767);
    reply[32] = 0xFF;
    reply[33] = 0xFF;
    CHECK(DecodeData(reply, length, &data));
    SDL_Tacx_GetControls(&data, &controls);
    CHECK(controls.axes[SDL_TACX_AXIS_WHEEL_SPEED] == 32767);

    /* Cadence and heart rate: x 128, at most 32640 */
    length = T1942(185, reply);
    mismatches = 0;
    for (value = 0; value <= 0xFF; ++value) {
        reply[44] = (uint8_t)value;
        reply[12] = (uint8_t)(255 - value);
        if (!SDL_Tacx_DecodeData(reply, length, &data)) {
            ++mismatches;
            continue;
        }
        SDL_Tacx_GetControls(&data, &controls);
        if (controls.axes[SDL_TACX_AXIS_CADENCE] != value * 128 ||
            controls.axes[SDL_TACX_AXIS_HEART_RATE] != (255 - value) * 128) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    reply[44] = 0xFF;
    reply[12] = 0xFF;
    CHECK(DecodeData(reply, length, &data));
    SDL_Tacx_GetControls(&data, &controls);
    CHECK(controls.axes[SDL_TACX_AXIS_CADENCE] == 32640 && controls.axes[SDL_TACX_AXIS_HEART_RATE] == 32640);

    /* Current resistance: signed, as reported */
    length = T1942(185, reply);
    mismatches = 0;
    for (value = 0; value <= 0xFFFF; ++value) {
        reply[38] = (uint8_t)(value & 0xFF);
        reply[39] = (uint8_t)(value >> 8);
        if (!SDL_Tacx_DecodeData(reply, length, &data)) {
            ++mismatches;
            continue;
        }
        SDL_Tacx_GetControls(&data, &controls);
        if (controls.axes[SDL_TACX_AXIS_RESISTANCE] != (value > 32767 ? value - 65536 : value) ||
            data.resistance != controls.axes[SDL_TACX_AXIS_RESISTANCE]) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);

    /* Axes 0, 2 and 3 and the fields the joystick does not show move
       nothing */
    length = T1942(185, reply);
    mismatches = 0;
    {
        static const int others[] = { 0, 1, 8, 14, 15, 16, 17, 20, 21, 22, 23, 28, 29, 30, 31, 34, 35, 36, 37, 40, 41, 42, 43, 45, 46, 47 };
        size_t k;

        for (k = 0; k < sizeof(others) / sizeof(others[0]); ++k) {
            const uint8_t saved = reply[others[k]];

            reply[others[k]] = (uint8_t)(saved ^ 0xA5);
            if (!SDL_Tacx_DecodeData(reply, length, &data)) {
                ++mismatches;
            } else {
                SDL_Tacx_GetControls(&data, &controls);
                if (memcmp(&controls.axes, &rest.axes, sizeof(rest.axes)) != 0 || controls.buttons != rest.buttons) {
                    ++mismatches;
                }
            }
            reply[others[k]] = saved;
        }
    }
    CHECK(mismatches == 0);
}

/* 11: every truncation, stale bytes and a wrong header */
static void CheckTruncations(const uint8_t *reply, size_t length)
{
    SDL_TacxState state, before;
    uint8_t other[64];
    size_t n;
    int mismatches = 0;

    /* A state that already holds a different data reply and a version */
    SDL_Tacx_Init(&state, 0);
    CHECK(T1932(2, other) == 64);
    CHECK(Handle(&state, other, 64, MS(1)) == (SDL_TACX_CHANGED_PRESENT | SDL_TACX_CHANGED_CONTROLS));
    CHECK(T1932(4, other) == 64);
    CHECK(Handle(&state, other, 64, MS(2)) == SDL_TACX_CHANGED_BRAKE);
    before = state;

    for (n = 0; n < SDL_TACX_REPLY_MINIMUM; ++n) {
        /* Exact-size copies, and the whole reply as stale bytes past n */
        if (Handle(&state, reply, n, MS(3)) != 0 || !SameState(&state, &before)) {
            ++mismatches;
        }
        if (SDL_Tacx_HandleReply(&state, reply, n, MS(3)) != 0 || !SameState(&state, &before)) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);

    /* From 48 bytes up to the whole read, one reply decodes the same */
    mismatches = 0;
    for (n = SDL_TACX_REPLY_MINIMUM; n <= length; ++n) {
        SDL_TacxState fresh;
        SDL_TacxData data;
        SDL_TacxBrake brake;
        const bool is_data = SDL_Tacx_DecodeData(reply, length, &data);
        const bool is_brake = SDL_Tacx_DecodeBrake(reply, length, &brake);

        SDL_Tacx_Init(&fresh, 0);
        if (is_data) {
            if (Handle(&fresh, reply, n, MS(5)) != (SDL_TACX_CHANGED_PRESENT | SDL_TACX_CHANGED_CONTROLS) ||
                !SameData(&fresh.data, &data)) {
                ++mismatches;
            }
        } else if (is_brake) {
            if (Handle(&fresh, reply, n, MS(5)) != SDL_TACX_CHANGED_BRAKE || !SameBrake(&fresh.brake, &brake)) {
                ++mismatches;
            }
        } else {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);

    /* Every single-bit error in the header, and the header read the other
       way round */
    mismatches = 0;
    {
        uint8_t copy[64];
        int bit;

        for (bit = 0; bit < 32; ++bit) {
            memcpy(copy, reply, length);
            copy[24 + bit / 8] ^= (uint8_t)(1u << (bit % 8));
            if (Handle(&state, copy, length, MS(4)) != 0 || !SameState(&state, &before)) {
                ++mismatches;
            }
        }
        memcpy(copy, reply, length);
        copy[24] = reply[27];
        copy[25] = reply[26];
        copy[26] = reply[25];
        copy[27] = reply[24];
        if (Handle(&state, copy, length, MS(4)) != 0 || !SameState(&state, &before)) {
            ++mismatches;
        }
        memset(copy + 24, 0, 4);
        if (Handle(&state, copy, length, MS(4)) != 0 || !SameState(&state, &before)) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

static void TestLengths(void)
{
    static const int t1942_lines[] = { 3, 5, 161, 185, 3853 };
    static const int t1932_lines[] = { 2, 4 };
    SDL_TacxState state;
    uint8_t reply[64];
    size_t i, length;

    for (i = 0; i < sizeof(t1942_lines) / sizeof(t1942_lines[0]); ++i) {
        length = T1942(t1942_lines[i], reply);
        CHECK(length == 48);
        CheckTruncations(reply, length);
    }
    for (i = 0; i < sizeof(t1932_lines) / sizeof(t1932_lines[0]); ++i) {
        length = T1932(t1932_lines[i], reply);
        CHECK(length == 64);
        CheckTruncations(reply, length);
    }

    /* FortiusANT would take 44 of line 185's bytes and read a cadence of 0.
       Here the cadence keeps the 22 the whole reply brought. */
    length = T1942(185, reply);
    SDL_Tacx_Init(&state, 0);
    CHECK(Handle(&state, reply, length, 0) & SDL_TACX_CHANGED_CONTROLS);
    CHECK(state.data.cadence == 22);
    for (i = 40; i < SDL_TACX_REPLY_MINIMUM; ++i) {
        CHECK(Handle(&state, reply, i, 0) == 0);
        CHECK(state.data.cadence == 22);
    }

    /* No reply and no state */
    SDL_Tacx_Init(&state, 0);
    CHECK(SDL_Tacx_HandleReply(&state, NULL, 64, 0) == 0 && !state.present);
    CHECK(SDL_Tacx_HandleReply(NULL, reply, length, 0) == 0);
    {
        SDL_TacxData data;
        SDL_TacxBrake brake;

        CHECK(!SDL_Tacx_DecodeData(NULL, 64, &data) && !SDL_Tacx_DecodeData(reply, length, NULL));
        CHECK(!SDL_Tacx_DecodeBrake(NULL, 64, &brake) && !SDL_Tacx_DecodeBrake(reply, length, NULL));
    }
}

/* A reply split across two reads decodes only from a first part of 48
   bytes or more, since the module keeps no bytes between reads */
static void TestSplits(void)
{
    uint8_t reply[64];
    size_t length, k;
    int mismatches = 0;
    int which;

    for (which = 0; which < 2; ++which) {
        length = which ? T1932(2, reply) : T1942(185, reply);
        for (k = 1; k < length; ++k) {
            SDL_TacxState state;
            SDL_TacxData data;
            const bool first_whole = (k >= SDL_TACX_REPLY_MINIMUM);
            const bool second_header = (length - k >= SDL_TACX_REPLY_MINIMUM &&
                                        reply[k + 24] == 0x03 && reply[k + 25] == 0x13 &&
                                        reply[k + 26] == 0x02 && reply[k + 27] == 0x00);
            int first, second;

            SDL_Tacx_Init(&state, 0);
            first = Handle(&state, reply, k, 0);
            second = Handle(&state, reply + k, length - k, 0);
            if ((first != 0) != first_whole || (second != 0 && !second_header)) {
                ++mismatches;
            }
            if (first_whole && (!DecodeData(reply, length, &data) || !SameData(&state.data, &data))) {
                ++mismatches;
            }
            if (!first_whole && state.present) {
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);
}

/* Both captures, whole, in order, one reply every 250 ms as recorded */
static void ReplayCapture(const TacxCaptureReply *table, size_t count, size_t expect_data, size_t expect_brake,
                          size_t expect_ignored)
{
    SDL_TacxState state;
    size_t i, data_replies = 0, brake_replies = 0, ignored = 0;
    int mismatches = 0;
    bool was_present = false;

    SDL_Tacx_Init(&state, 0);
    for (i = 0; i < count; ++i) {
        uint8_t reply[64];
        const size_t length = FromHex(table[i].hex, reply, sizeof(reply));
        const uint64_t now = MS(250) * (i + 1);
        const bool header_data = (length >= 48 && HexByte(table[i].hex, 24) == 0x03 && HexByte(table[i].hex, 25) == 0x13 &&
                                  HexByte(table[i].hex, 26) == 0x02 && HexByte(table[i].hex, 27) == 0x00);
        const bool header_brake = (length >= 48 && HexByte(table[i].hex, 24) == 0x03 && HexByte(table[i].hex, 25) == 0x0C &&
                                   HexByte(table[i].hex, 26) == 0x00 && HexByte(table[i].hex, 27) == 0x00);
        const int changed = Handle(&state, reply, length, now);

        if (header_data) {
            ++data_replies;
            if (!(changed & SDL_TACX_CHANGED_CONTROLS) || !MatchesHex(&state.data, table[i].hex) ||
                state.data_at != now || !state.present) {
                ++mismatches;
                printf("  line %d did not decode\n", table[i].line);
            }
            if (((changed & SDL_TACX_CHANGED_PRESENT) != 0) == was_present) {
                ++mismatches;
            }
        } else if (header_brake) {
            ++brake_replies;
            if (changed != SDL_TACX_CHANGED_BRAKE || !state.version_known ||
                state.brake.firmware != HexU32(table[i].hex, 28) || state.brake.serial != HexU32(table[i].hex, 32)) {
                ++mismatches;
            }
        } else {
            ++ignored;
            if (changed != 0) {
                ++mismatches;
            }
        }
        was_present = state.present;
        /* The head unit answered every 250 ms, so it is never lost */
        if (SDL_Tacx_Tick(&state, now + MS(250) - 1) != 0) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    CHECK(data_replies == expect_data && brake_replies == expect_brake && ignored == expect_ignored);
    CHECK(state.present && state.version_known);
}

static void TestCaptures(void)
{
    /* T1942: the 24-byte reply on line 1, the version reply on line 3 and
       2680 data replies */
    ReplayCapture(tacx_t1942_replies, TACX_T1942_REPLIES, 2680, 1, 1);
    /* T1932: 28 replies of 64 bytes, the version reply on line 4 */
    ReplayCapture(tacx_t1932_replies, TACX_T1932_REPLIES, 27, 1, 0);
    {
        const size_t counts[2] = { TACX_T1942_REPLIES, TACX_T1932_REPLIES };
        CHECK(counts[0] == 2682 && counts[1] == 28);
    }
    CHECK(tacx_t1942_replies[0].line == 1 && tacx_t1942_replies[TACX_T1942_REPLIES - 1].line == 5363);
    CHECK(tacx_t1932_replies[0].line == 2 && tacx_t1932_replies[TACX_T1932_REPLIES - 1].line == 56);
    {
        size_t i;
        int wrong = 0;

        for (i = 0; i < TACX_T1932_REPLIES; ++i) {
            if (strlen(tacx_t1932_replies[i].hex) != 128) {
                ++wrong;
            }
        }
        CHECK(wrong == 0);
    }
}

static bool Frame(SDL_TacxState *state, uint64_t now, uint8_t out[SDL_TACX_FRAME_LENGTH], size_t *length)
{
    memset(out, 0xEE, SDL_TACX_FRAME_LENGTH);
    *length = 0;
    return SDL_Tacx_NextFrame(state, now, out, length);
}

static bool IsVersion(const uint8_t *out, size_t length)
{
    return length == 4 && out[0] == 0x02 && out[1] == 0x00 && out[2] == 0x00 && out[3] == 0x00;
}

static bool IsStop(const uint8_t *out, size_t length)
{
    static const uint8_t stop[12] = { 0x01, 0x08, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0 };

    return length == 12 && memcmp(out, stop, 12) == 0;
}

/* The frame schedule on the injected clock, and test 12 */
static void TestSchedule(void)
{
    const uint64_t t0 = MS(5000);
    SDL_TacxState state;
    uint8_t out[SDL_TACX_FRAME_LENGTH];
    size_t length;
    uint64_t now;
    int versions, stops;

    /* 12: a head unit that attaches gets the version request first. The
       part's case is a T1942 after its firmware load, which the part also
       leaves outside the ticket, so this is any head unit that attaches. */
    SDL_Tacx_Init(&state, t0);
    CHECK(!state.present && !state.version_known && state.version_requests == 0);
    CHECK(!Frame(&state, t0 - 1, out, &length));
    CHECK(Frame(&state, t0, out, &length) && IsVersion(out, length));
    /* One frame in flight at a time */
    CHECK(!Frame(&state, t0, out, &length));
    CHECK(!Frame(&state, t0 + MS(1000), out, &length));
    SDL_Tacx_FrameDone(&state, 4, t0 + MS(1));
    CHECK(state.version_requests == 1 && state.frame_at == t0 + MS(101) && state.version_at == t0 + MS(501));
    /* The next frame is due 100 ms after the write completed, and it is the
       stop frame */
    CHECK(!Frame(&state, t0 + MS(101) - 1, out, &length));
    CHECK(Frame(&state, t0 + MS(101), out, &length) && IsStop(out, length));
    SDL_Tacx_FrameDone(&state, 12, t0 + MS(101));
    /* A completion without a frame in flight changes nothing */
    SDL_Tacx_FrameDone(&state, 12, t0 + MS(150));
    CHECK(state.frame_at == t0 + MS(201) && state.version_requests == 1);

    /* No version reply: a version request every 500 ms, six in all, and
       stop frames between and after them */
    SDL_Tacx_Init(&state, t0);
    versions = 0;
    stops = 0;
    for (now = t0; now < t0 + MS(10000); now += MS(100)) {
        CHECK(Frame(&state, now, out, &length));
        if (IsVersion(out, length)) {
            CHECK(now - t0 == MS(500) * (uint64_t)versions);
            ++versions;
        } else if (IsStop(out, length)) {
            ++stops;
        }
        SDL_Tacx_FrameDone(&state, (int)length, now);
    }
    CHECK(versions == SDL_TACX_VERSION_REQUESTS && versions == 6);
    CHECK(stops == 100 - 6);

    /* A version reply ends the requests */
    SDL_Tacx_Init(&state, t0);
    CHECK(Frame(&state, t0, out, &length) && IsVersion(out, length));
    SDL_Tacx_FrameDone(&state, 4, t0);
    {
        uint8_t reply[64];
        const size_t reply_length = T1942(3, reply);

        CHECK(Handle(&state, reply, reply_length, t0 + MS(70)) == SDL_TACX_CHANGED_BRAKE);
    }
    versions = 0;
    for (now = t0 + MS(100); now < t0 + MS(5000); now += MS(100)) {
        CHECK(Frame(&state, now, out, &length));
        versions += IsVersion(out, length);
        SDL_Tacx_FrameDone(&state, (int)length, now);
    }
    CHECK(versions == 0);

    /* A failed or short version request does not count and goes again with
       the next frame. A failed stop frame still waits its 100 ms. */
    SDL_Tacx_Init(&state, t0);
    CHECK(Frame(&state, t0, out, &length) && IsVersion(out, length));
    SDL_Tacx_FrameDone(&state, -1, t0);
    CHECK(state.version_requests == 0 && state.frame_at == t0 + MS(100));
    CHECK(Frame(&state, t0 + MS(100), out, &length) && IsVersion(out, length));
    SDL_Tacx_FrameDone(&state, 3, t0 + MS(100));
    CHECK(state.version_requests == 0);
    CHECK(Frame(&state, t0 + MS(200), out, &length) && IsVersion(out, length));
    SDL_Tacx_FrameDone(&state, 4, t0 + MS(200));
    CHECK(state.version_requests == 1 && state.version_at == t0 + MS(700));
    CHECK(Frame(&state, t0 + MS(300), out, &length) && IsStop(out, length));
    SDL_Tacx_FrameDone(&state, -1, t0 + MS(300));
    CHECK(!Frame(&state, t0 + MS(399), out, &length));
    CHECK(Frame(&state, t0 + MS(400), out, &length) && IsStop(out, length));
    /* A stop frame cut to 4 bytes is not a version request */
    SDL_Tacx_FrameDone(&state, 4, t0 + MS(400));
    CHECK(state.version_requests == 1 && state.version_at == t0 + MS(700) && state.frame_at == t0 + MS(500));
    /* A late update sends one frame, not a burst */
    CHECK(Frame(&state, t0 + MS(2000), out, &length) && IsVersion(out, length));
    SDL_Tacx_FrameDone(&state, 4, t0 + MS(2000));
    CHECK(!Frame(&state, t0 + MS(2099), out, &length));
    CHECK(Frame(&state, t0 + MS(2100), out, &length) && IsStop(out, length));
    SDL_Tacx_FrameDone(&state, 12, t0 + MS(2100));

    /* A re-attached head unit is a new device and starts over */
    SDL_Tacx_Init(&state, t0 + MS(9000));
    CHECK(Frame(&state, t0 + MS(9000), out, &length) && IsVersion(out, length));

    /* Missing arguments */
    SDL_Tacx_Init(&state, 0);
    CHECK(!SDL_Tacx_NextFrame(NULL, 0, out, &length));
    CHECK(!SDL_Tacx_NextFrame(&state, 0, NULL, &length));
    CHECK(!SDL_Tacx_NextFrame(&state, 0, out, NULL));
    CHECK(!state.in_flight);
    SDL_Tacx_FrameDone(NULL, 4, 0);
    SDL_Tacx_Init(NULL, 0);
    CHECK(SDL_Tacx_Tick(NULL, 0) == 0);
}

/* The lost-device timeout, and the version requests around it */
static void TestPresence(void)
{
    const uint64_t t0 = MS(3000);
    SDL_TacxState state;
    uint8_t data_reply[64], brake_reply[64], short_reply[64], out[SDL_TACX_FRAME_LENGTH];
    size_t data_length, brake_length, short_length, length;
    uint64_t now;
    int i;

    data_length = T1942(5, data_reply);
    brake_length = T1942(3, brake_reply);
    short_length = T1942(1, short_reply);

    SDL_Tacx_Init(&state, t0);
    CHECK(SDL_Tacx_Tick(&state, t0 + MS(5000)) == 0);
    CHECK(Handle(&state, data_reply, data_length, t0) == (SDL_TACX_CHANGED_PRESENT | SDL_TACX_CHANGED_CONTROLS));
    CHECK(Handle(&state, brake_reply, brake_length, t0) == SDL_TACX_CHANGED_BRAKE);
    CHECK(SDL_Tacx_Tick(&state, t0 + MS(1000) - 1) == 0 && state.present);
    /* A later data reply holds it. Short and version replies do not. */
    CHECK(Handle(&state, data_reply, data_length, t0 + MS(500)) == SDL_TACX_CHANGED_CONTROLS);
    CHECK(Handle(&state, short_reply, short_length, t0 + MS(900)) == 0);
    CHECK(Handle(&state, brake_reply, brake_length, t0 + MS(950)) == 0);
    CHECK(SDL_Tacx_Tick(&state, t0 + MS(1000)) == 0);
    CHECK(SDL_Tacx_Tick(&state, t0 + MS(1500) - 1) == 0);
    /* 1000 ms after the last data reply it is lost, and the brake is
       unknown */
    CHECK(SDL_Tacx_Tick(&state, t0 + MS(1500)) == SDL_TACX_CHANGED_PRESENT);
    CHECK(!state.present && !state.version_known && state.version_requests == 0 && state.version_at == t0 + MS(1500));
    CHECK(SDL_Tacx_Tick(&state, t0 + MS(1600)) == 0);
    /* The next frame asks for the version again, and the same brake's reply
       is news again */
    CHECK(Frame(&state, t0 + MS(1500), out, &length) && IsVersion(out, length));
    SDL_Tacx_FrameDone(&state, 4, t0 + MS(1500));
    CHECK(Handle(&state, brake_reply, brake_length, t0 + MS(1520)) == SDL_TACX_CHANGED_BRAKE);
    CHECK(state.version_known && !state.present);
    /* It returns with the next data reply */
    CHECK(Handle(&state, data_reply, data_length, t0 + MS(1550)) == (SDL_TACX_CHANGED_PRESENT | SDL_TACX_CHANGED_CONTROLS));
    CHECK(state.present && state.version_requests == 1 && state.version_at == t0 + MS(2000));

    /* A head unit whose brake answers late: a round of six requests goes
       unanswered, then the first data reply starts another */
    SDL_Tacx_Init(&state, t0);
    for (now = t0; now < t0 + MS(4000); now += MS(100)) {
        CHECK(Frame(&state, now, out, &length));
        SDL_Tacx_FrameDone(&state, (int)length, now);
    }
    CHECK(state.version_requests == 6 && !state.version_known);
    CHECK(Frame(&state, now, out, &length) && IsStop(out, length));
    SDL_Tacx_FrameDone(&state, 12, now);
    CHECK(Handle(&state, data_reply, data_length, now + MS(50)) == (SDL_TACX_CHANGED_PRESENT | SDL_TACX_CHANGED_CONTROLS));
    CHECK(state.version_requests == 0 && state.version_at == now + MS(50));
    CHECK(Frame(&state, now + MS(100), out, &length) && IsVersion(out, length));
    SDL_Tacx_FrameDone(&state, 4, now + MS(100));

    /* With the brake known, the head unit's return asks for nothing */
    SDL_Tacx_Init(&state, t0);
    for (i = 0; i < 6; ++i) {
        CHECK(Frame(&state, t0 + MS(500) * (uint64_t)i, out, &length) && IsVersion(out, length));
        SDL_Tacx_FrameDone(&state, 4, t0 + MS(500) * (uint64_t)i);
    }
    CHECK(state.version_requests == 6);
    CHECK(Handle(&state, brake_reply, brake_length, t0 + MS(2600)) == SDL_TACX_CHANGED_BRAKE);
    CHECK(Handle(&state, data_reply, data_length, t0 + MS(2700)) == (SDL_TACX_CHANGED_PRESENT | SDL_TACX_CHANGED_CONTROLS));
    CHECK(state.version_requests == 6 && state.version_known);
    CHECK(Frame(&state, t0 + MS(3000), out, &length) && IsStop(out, length));
    SDL_Tacx_FrameDone(&state, 12, t0 + MS(3000));
    /* Losing the head unit then starts a full round, used or not */
    CHECK(SDL_Tacx_Tick(&state, t0 + MS(3700)) == SDL_TACX_CHANGED_PRESENT);
    CHECK(state.version_requests == 0 && !state.version_known);
    CHECK(Frame(&state, t0 + MS(3700), out, &length) && IsVersion(out, length));
    SDL_Tacx_FrameDone(&state, 4, t0 + MS(3700));

    /* A round still running when the head unit appears goes on as it was */
    SDL_Tacx_Init(&state, t0);
    CHECK(Frame(&state, t0, out, &length) && IsVersion(out, length));
    SDL_Tacx_FrameDone(&state, 4, t0);
    CHECK(Handle(&state, data_reply, data_length, t0 + MS(80)) == (SDL_TACX_CHANGED_PRESENT | SDL_TACX_CHANGED_CONTROLS));
    CHECK(state.version_requests == 1 && state.version_at == t0 + MS(500));
    CHECK(Frame(&state, t0 + MS(100), out, &length) && IsStop(out, length));
    SDL_Tacx_FrameDone(&state, 12, t0 + MS(100));
}

int main(void)
{
    TestConstants();
    TestIdentify();
    TestVendorRules();
    TestFrames();
    TestShortReply();
    TestBrakeVersion();
    TestDataReplies();
    TestButtons();
    TestAxes();
    TestLengths();
    TestSplits();
    TestCaptures();
    TestSchedule();
    TestPresence();

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
