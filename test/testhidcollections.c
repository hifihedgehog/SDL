/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/hidapi/SDL_hidapi_collections.c, the admitted-collection
   table and the report descriptor walker of hifihedgehog/SDL#33 Part 7. The
   descriptors are the ones the part quotes: the Speed Force Wireless from
   WiiBrew, the SideWinder Game Voice from the 2007 linux-kernel thread and
   both VRC-2 interfaces from linux-input. Each is fed as an exact-size heap
   copy at every length, so AddressSanitizer catches a read past it. */

#include "../src/hidapi/SDL_hidapi_collections.h"

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

static const uint8_t speed_force_descriptor[] = {
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0xA1, 0x02, 0x95, 0x01, 0x75, 0x0A, 0x15, 0x00, 0x26, 0xFF,
    0x03, 0x35, 0x00, 0x46, 0xFF, 0x03, 0x09, 0x30, 0x81, 0x02, 0x06, 0x00, 0xFF, 0x95, 0x02, 0x75,
    0x01, 0x25, 0x01, 0x45, 0x01, 0x09, 0x01, 0x81, 0x02, 0x95, 0x0B, 0x19, 0x01, 0x29, 0x0B, 0x05,
    0x09, 0x81, 0x02, 0x06, 0x00, 0xFF, 0x95, 0x01, 0x75, 0x01, 0x09, 0x02, 0x81, 0x02, 0x05, 0x01,
    0x75, 0x08, 0x26, 0xFF, 0x00, 0x46, 0xFF, 0x00, 0x09, 0x31, 0x09, 0x32, 0x95, 0x02, 0x81, 0x02,
    0xC0, 0xA1, 0x02, 0x06, 0x00, 0xFF, 0x95, 0x07, 0x09, 0x03, 0x91, 0x02, 0xC0, 0x0A, 0xFF, 0xFF,
    0x95, 0x08, 0xB1, 0x02, 0xC0
};

static const uint8_t game_voice_descriptor[] = {
    0x05, 0x0B, 0x09, 0x05, 0xA1, 0x01, 0x05, 0x09, 0x19, 0x11, 0x29, 0x18, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x22, 0x05, 0x08, 0x09, 0x3A, 0xA1, 0x02, 0x05, 0x09, 0x19, 0x11,
    0x29, 0x17, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x07, 0xB1, 0xA2, 0xC0, 0x06, 0xFF, 0xFF,
    0x09, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0xB1, 0x22, 0xC0
};

static const uint8_t vrc2_joystick_descriptor[] = {
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32,
    0x15, 0x00, 0x26, 0xFF, 0x07, 0x35, 0x00, 0x46, 0xFF, 0x00, 0x75, 0x10, 0x95, 0x03, 0x81, 0x02,
    0xC0, 0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x95, 0x02, 0x75, 0x01, 0x81,
    0x02, 0x95, 0x01, 0x75, 0x06, 0x81, 0x01, 0x09, 0x00, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08,
    0x95, 0x08, 0xB1, 0x02, 0xC0
};

static const uint8_t vrc2_buttons_descriptor[] = {
    0x05, 0x01, 0x09, 0x00, 0xA1, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x3F, 0x95, 0x40, 0x81, 0x02, 0xC0
};

/* Constructed: input reports 1, 3 and 4 in a vendor collection, output 6,
   feature 9, a Push and Pop around a second collection, and a long item */
static const uint8_t report_id_descriptor[] = {
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01,
    0x85, 0x01, 0x75, 0x08, 0x95, 0x03, 0x09, 0x01, 0x81, 0x02,
    0x85, 0x03, 0x95, 0x10, 0x09, 0x02, 0x81, 0x02,
    0xA4,                         /* Push: report 3 */
    0x85, 0x06, 0x95, 0x02, 0x09, 0x03, 0x91, 0x02,
    0xB4,                         /* Pop: back to report 3 */
    0x95, 0x01, 0x09, 0x04, 0x81, 0x02, /* one more input item on report 3 */
    0xFE, 0x02, 0x10, 0xAA, 0xBB, /* a long item */
    0x85, 0x04, 0x95, 0x03, 0x09, 0x05, 0x81, 0x02,
    0x85, 0x09, 0x95, 0x01, 0x09, 0x06, 0xB2, 0x02, 0x00, /* a Feature item with 2 data bytes */
    0xC0
};

static bool Parse(const uint8_t *descriptor, size_t length, SDL_HIDAPIReportIDs *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, descriptor, length);
    }
    result = SDL_HIDAPI_ParseReportIDs(copy, length, out);
    free(copy);
    return result;
}

static int CountIDs(const uint8_t ids[32])
{
    int id, count = 0;

    for (id = 0; id < 256; ++id) {
        count += SDL_HIDAPI_HasReportID(ids, (uint8_t)id) ? 1 : 0;
    }
    return count;
}

static void TestAdmitted(void)
{
    static const struct
    {
        uint16_t vendor, product, page, usage;
    } admitted[] = {
        { 0x1781, 0x0898, 0x0001, 0x0004 }, { 0x1781, 0x0898, 0x0000, 0x0000 }, { 0x1781, 0x0898, 0xFF00, 0x0001 },
        { 0x045E, 0x003B, 0x000B, 0x0005 },
        { 0x0D7F, 0x0100, 0x008C, 0x0001 },
        { 0x1941, 0x8021, 0x0001, 0x0000 }, { 0x1941, 0x8021, 0xFF00, 0x0001 },
        { 0x1234, 0x0000, 0xFF00, 0xFF01 },
        { 0x2833, 0x0001, 0x0003, 0x0005 }, { 0x2833, 0x0001, 0xFF00, 0x0001 },
        { 0x045E, 0x065B, 0x0001, 0x0005 }, { 0x045E, 0x065D, 0xFF00, 0x0000 }, { 0x045E, 0x066A, 0x000C, 0x0001 },
        { 0x0111, 0x1420, 0x0001, 0x0005 }, { 0x0111, 0x1420, 0xFF00, 0x0001 },
        { 0x041E, 0x2801, 0xFF00, 0x0001 }, { 0x041E, 0x2801, 0x000C, 0x0001 },
    };
    static const struct
    {
        uint16_t vendor, product, page, usage;
    } refused[] = {
        /* The collections Windows opens exclusively, on every row's device */
        { 0x1781, 0x0898, 0x0001, 0x0002 }, { 0x1941, 0x8021, 0x0001, 0x0006 }, { 0x2833, 0x0001, 0x0001, 0x0001 },
        { 0x045E, 0x065B, 0x0001, 0x0007 }, { 0x0111, 0x1420, 0x000D, 0x0004 }, { 0x041E, 0x2801, 0x0001, 0x0006 },
        { 0x041E, 0x2801, 0x0001, 0x0002 }, { 0x0D7F, 0x0100, 0x0001, 0x0002 }, { 0x045E, 0x003B, 0x0001, 0x0006 },
        { 0x1234, 0x0000, 0x0001, 0x0006 }, { 0x045E, 0x066A, 0x000D, 0x0005 }, { 0x2833, 0x0001, 0x000D, 0x0002 },
        /* Rows with a usage admit only that usage */
        { 0x045E, 0x003B, 0x000C, 0x0001 }, { 0x045E, 0x003B, 0x000B, 0x0001 }, { 0x0D7F, 0x0100, 0x008C, 0x0002 },
        { 0x0D7F, 0x0100, 0xFF00, 0x0001 }, { 0x1234, 0x0000, 0xFF00, 0x0001 }, { 0x1234, 0x0000, 0xFF01, 0xFF01 },
        /* Other products of the same vendors, and ordinary gamepads, keep their own path */
        { 0x1781, 0x0A9D, 0xFF00, 0x0001 }, { 0x045E, 0x028E, 0xFF00, 0x0001 }, { 0x1941, 0x8020, 0x0001, 0x0000 },
        { 0x0111, 0x1419, 0x0001, 0x0005 }, { 0x1038, 0x1420, 0x0001, 0x0005 }, { 0x054C, 0x0CE6, 0x0001, 0x0005 },
        { 0x046D, 0xC29C, 0x0001, 0x0004 }, { 0x14B7, 0x0982, 0x0001, 0x0004 }, { 0x046D, 0xC21C, 0xFF00, 0x0000 },
        { 0x07C0, 0x1125, 0x0001, 0x0000 },
    };
    size_t i;

    for (i = 0; i < sizeof(admitted) / sizeof(admitted[0]); ++i) {
        CHECK(SDL_HIDAPI_IsAdmittedCollection(admitted[i].vendor, admitted[i].product, admitted[i].page, admitted[i].usage));
    }
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        CHECK(!SDL_HIDAPI_IsAdmittedCollection(refused[i].vendor, refused[i].product, refused[i].page, refused[i].usage));
    }
}

static void TestJoystickOnly(void)
{
    static const uint16_t only[][2] = {
        { 0x1781, 0x0898 }, { 0x045E, 0x003B }, { 0x0D7F, 0x0100 }, { 0x1941, 0x8021 }, { 0x1234, 0x0000 },
        { 0x14B7, 0x0982 }, { 0x2833, 0x0001 }, { 0x045E, 0x065B }, { 0x045E, 0x065D }, { 0x045E, 0x066A },
        { 0x041E, 0x2801 },
    };
    size_t i;

    for (i = 0; i < sizeof(only) / sizeof(only[0]); ++i) {
        CHECK(SDL_HIDAPI_IsJoystickOnlyDevice(only[i][0], only[i][1]));
    }
    /* The Nimbus is a gamepad, the Speed Force a wheel on the wheel list */
    CHECK(!SDL_HIDAPI_IsJoystickOnlyDevice(0x0111, 0x1420));
    CHECK(!SDL_HIDAPI_IsJoystickOnlyDevice(0x046D, 0xC29C));
    CHECK(!SDL_HIDAPI_IsJoystickOnlyDevice(0x045E, 0x028E));
}

static void CheckTruncations(const uint8_t *descriptor, size_t length)
{
    SDL_HIDAPIReportIDs ids;
    size_t cut;

    for (cut = 0; cut <= length; ++cut) {
        /* Any prefix either parses or is refused, and never reads past its length */
        (void)Parse(descriptor, cut, &ids);
    }
    CHECK(Parse(descriptor, length, &ids));
}

static void TestDescriptors(void)
{
    SDL_HIDAPIReportIDs ids;
    uint8_t bad[4];

    /* No report IDs: report 0 in each list the descriptor has items in */
    CHECK(Parse(speed_force_descriptor, sizeof(speed_force_descriptor), &ids));
    CHECK(sizeof(speed_force_descriptor) == 101);
    CHECK(!ids.uses_report_ids);
    CHECK(SDL_HIDAPI_HasReportID(ids.input, 0) && SDL_HIDAPI_HasReportID(ids.output, 0) && SDL_HIDAPI_HasReportID(ids.feature, 0));
    CHECK(CountIDs(ids.input) == 1 && CountIDs(ids.output) == 1 && CountIDs(ids.feature) == 1);

    CHECK(Parse(game_voice_descriptor, sizeof(game_voice_descriptor), &ids));
    CHECK(sizeof(game_voice_descriptor) == 61);
    CHECK(!ids.uses_report_ids && SDL_HIDAPI_HasReportID(ids.input, 0) && SDL_HIDAPI_HasReportID(ids.feature, 0));
    CHECK(CountIDs(ids.output) == 0);

    CHECK(Parse(vrc2_joystick_descriptor, sizeof(vrc2_joystick_descriptor), &ids) && sizeof(vrc2_joystick_descriptor) == 69);
    CHECK(!ids.uses_report_ids && SDL_HIDAPI_HasReportID(ids.input, 0) && SDL_HIDAPI_HasReportID(ids.feature, 0));
    CHECK(Parse(vrc2_buttons_descriptor, sizeof(vrc2_buttons_descriptor), &ids) && sizeof(vrc2_buttons_descriptor) == 23);
    CHECK(CountIDs(ids.input) == 1 && CountIDs(ids.feature) == 0);

    /* Report IDs, with Push and Pop and a long item */
    CHECK(Parse(report_id_descriptor, sizeof(report_id_descriptor), &ids));
    CHECK(ids.uses_report_ids);
    CHECK(SDL_HIDAPI_HasReportID(ids.input, 1) && SDL_HIDAPI_HasReportID(ids.input, 3) && SDL_HIDAPI_HasReportID(ids.input, 4));
    CHECK(CountIDs(ids.input) == 3);
    CHECK(SDL_HIDAPI_HasReportID(ids.output, 6) && CountIDs(ids.output) == 1);
    CHECK(SDL_HIDAPI_HasReportID(ids.feature, 9) && CountIDs(ids.feature) == 1);
    CHECK(!SDL_HIDAPI_HasReportID(ids.input, 6) && !SDL_HIDAPI_HasReportID(ids.input, 0));

    CheckTruncations(speed_force_descriptor, sizeof(speed_force_descriptor));
    CheckTruncations(game_voice_descriptor, sizeof(game_voice_descriptor));
    CheckTruncations(vrc2_joystick_descriptor, sizeof(vrc2_joystick_descriptor));
    CheckTruncations(report_id_descriptor, sizeof(report_id_descriptor));

    /* An item whose data runs past the end is refused */
    CHECK(!Parse(report_id_descriptor, 1, &ids)); /* 06 with no data */
    CHECK(!Parse(speed_force_descriptor, 15, &ids)); /* 26 FF with one of its two bytes */
    /* A long item that runs past the end, a Pop without a Push, and nine Pushes */
    bad[0] = 0xFE;
    bad[1] = 0x05;
    bad[2] = 0x10;
    bad[3] = 0x00;
    CHECK(!Parse(bad, 4, &ids));
    CHECK(!Parse(bad, 2, &ids));
    bad[0] = 0xB4;
    CHECK(!Parse(bad, 1, &ids));
    {
        uint8_t pushes[9];
        memset(pushes, 0xA4, sizeof(pushes));
        CHECK(Parse(pushes, 8, &ids));
        CHECK(!Parse(pushes, 9, &ids));
    }
    /* A long item one byte short */
    bad[0] = 0xFE;
    bad[1] = 0x02;
    bad[2] = 0x00;
    bad[3] = 0xAA;
    CHECK(!Parse(bad, 4, &ids));
    {
        /* A 4-byte Logical Maximum. Read as 3 bytes, its last byte would be a
           Report ID item and the rest would run past the end. */
        static const uint8_t four[] = { 0x27, 0x00, 0x00, 0x00, 0x85, 0x85, 0x07, 0x81, 0x02 };
        CHECK(Parse(four, sizeof(four), &ids));
        CHECK(ids.uses_report_ids && SDL_HIDAPI_HasReportID(ids.input, 7) && CountIDs(ids.input) == 1);
    }
    CHECK(Parse(NULL, 0, &ids) && CountIDs(ids.input) == 0);
    CHECK(!SDL_HIDAPI_ParseReportIDs(NULL, 4, &ids));
    CHECK(!SDL_HIDAPI_ParseReportIDs(speed_force_descriptor, 4, NULL));
    CHECK(!SDL_HIDAPI_HasReportID(NULL, 0));
}

int main(void)
{
    TestAdmitted();
    TestJoystickOnly();
    TestDescriptors();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
