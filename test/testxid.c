/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_xid_proto.c and the XID
   interface test in src/hidapi/SDL_hidapi_vendorusb.c: the original Xbox
   devices and the Steel Battalion controller of hifihedgehog/SDL#33 Part 4.
   The numbers follow the part's replay tests, "G" for the gamepad family and
   "SB" for the Steel Battalion. Every report is fed as an exact-size heap
   copy, so AddressSanitizer catches a read past its length, and truncated
   inside a buffer of stale bytes. No captured XID input report is published,
   so the reports are built from the layouts. The descriptors are the dumps
   the part quotes. */

#include "../src/joystick/hidapi/SDL_hidapi_xid_proto.h"
#include "../src/hidapi/SDL_hidapi_vendorusb.h"

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

// The Duke's XID descriptor (xemu xid-gamepad.c) and the Steel Battalion's dump. The Controller S is the Duke's with bSubType 02.
static const uint8_t duke_descriptor[16] = {
    0x10, 0x42, 0x00, 0x01, 0x01, 0x01, 0x14, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
static const uint8_t sb_descriptor[16] = {
    0x10, 0x42, 0x00, 0x01, 0x80, 0x01, 0x1A, 0x16, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
// The DVD remote: bLength 8, bType 3, a 6-byte report
static const uint8_t dvd_descriptor[8] = { 0x08, 0x42, 0x00, 0x01, 0x03, 0x00, 0x06, 0x00 };

static bool Parse(const uint8_t *data, size_t length, SDL_XIDDescriptor *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, data, length);
    }
    result = SDL_XID_ParseDescriptor(copy, length, out);
    free(copy);
    return result;
}

static bool IdentifyWith(uint16_t vendor, uint16_t product, const uint8_t *descriptor, size_t length,
                         const char *product_string, SDL_XIDIdentity *identity)
{
    SDL_XIDDescriptor parsed;

    if (descriptor && Parse(descriptor, length, &parsed)) {
        return SDL_XID_Identify(vendor, product, &parsed, product_string, identity);
    }
    return SDL_XID_Identify(vendor, product, NULL, product_string, identity);
}

static bool Decode(const SDL_XIDIdentity *identity, const uint8_t *report, size_t length, SDL_XIDGamepadState *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    memset(out, 0xA5, sizeof(*out));
    result = SDL_XID_DecodeGamepad(identity, copy, length, out);
    free(copy);
    return result;
}

static bool DecodeSB(const uint8_t *report, size_t length, SDL_XIDSteelBattalionState *out)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    memset(out, 0xA5, sizeof(*out));
    result = SDL_XID_DecodeSteelBattalion(copy, length, out);
    free(copy);
    return result;
}

static void GamepadNeutral(uint8_t report[20])
{
    memset(report, 0, 20);
    report[1] = 0x14;
}

static void SBNeutral(uint8_t report[26])
{
    memset(report, 0, 26);
    report[1] = 0x1A;
    report[9] = 0x80;
    report[11] = 0x80;
    report[25] = 0xFF;
}

static bool Down(uint16_t buttons, int button)
{
    return (buttons & (1u << button)) != 0;
}

static int CountDown(uint64_t buttons)
{
    int count = 0;

    while (buttons) {
        count += (int)(buttons & 1);
        buttons >>= 1;
    }
    return count;
}

static const SDL_XIDIdentity *Pad(void)
{
    static SDL_XIDIdentity identity;

    IdentifyWith(0x045E, 0x0202, duke_descriptor, sizeof(duke_descriptor), NULL, &identity);
    return &identity;
}

#define MAX_SB_PACKETS 128

// Every packet of SB1 to SB5, 26 bytes each
static int SBPackets(uint8_t packets[MAX_SB_PACKETS][26])
{
    static const uint8_t aiming[] = { 0x00, 0x80, 0xFF };
    static const uint8_t signed_values[] = { 0x80, 0x01, 0x7F };
    static const uint8_t gears[] = { 0xFE, 0xFF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x08, 0x09, 0x0A, 0x0B,
                                     0x0C, 0x0D, 0x00, 0x06, 0x0E, 0x0F };
    int n = 0, i, j;

    SBNeutral(packets[n++]);
    for (i = 0; i < 39; ++i) {
        SBNeutral(packets[n]);
        packets[n][25] = 0x00;
        packets[n++][2 + i / 8] = (uint8_t)(1 << (i % 8));
    }
    SBNeutral(packets[n]);
    packets[n][6] = 0x80;
    packets[n++][7] = 0xFF;
    for (i = 0; i < (int)sizeof(aiming); ++i) {
        SBNeutral(packets[n]);
        packets[n++][9] = aiming[i];
        SBNeutral(packets[n]);
        packets[n++][11] = aiming[i];
    }
    for (i = 13; i <= 17; i += 2) {
        for (j = 0; j < (int)sizeof(signed_values); ++j) {
            SBNeutral(packets[n]);
            packets[n++][i] = signed_values[j];
        }
    }
    for (i = 19; i <= 23; i += 2) {
        SBNeutral(packets[n]);
        packets[n++][i] = 0xFF;
    }
    SBNeutral(packets[n]);
    for (i = 8; i <= 22; i += 2) {
        packets[n][i] = 0xFF;
    }
    ++n;
    for (i = 0; i < 16; ++i) {
        SBNeutral(packets[n]);
        packets[n++][24] = (uint8_t)i;
    }
    SBNeutral(packets[n]);
    packets[n++][24] = 0xF3;
    for (i = 0; i < (int)sizeof(gears); ++i) {
        SBNeutral(packets[n]);
        packets[n++][25] = gears[i];
    }
    return n;
}

/* ------------------------------------------------------------------------ */
/* The interface test (G15) and the descriptors (G13, G14) */

static void TestInterface(void)
{
    SDL_VendorUSBEndpointInfo endpoints[3];

    // The Duke and the Controller S: 0x82 or 0x81 in, 0x02 out, both interrupt
    endpoints[0].address = 0x82;
    endpoints[0].attributes = 0x03;
    endpoints[1].address = 0x02;
    endpoints[1].attributes = 0x03;
    CHECK(SDL_VendorUSB_IsXIDInterface(0x58, 0x42, 0x00, endpoints, 2));
    endpoints[0].address = 0x81;
    CHECK(SDL_VendorUSB_IsXIDInterface(0x58, 0x42, 0x00, endpoints, 2));
    // The Steel Battalion's dump: 0x82 in and 0x01 out
    endpoints[0].address = 0x82;
    endpoints[1].address = 0x01;
    CHECK(SDL_VendorUSB_IsXIDInterface(0x58, 0x42, 0x00, endpoints, 2));
    // The DVD remote: one in endpoint
    CHECK(!SDL_VendorUSB_IsXIDInterface(0x58, 0x42, 0x00, endpoints, 1));
    // Two in endpoints, two out endpoints
    endpoints[1].address = 0x83;
    CHECK(!SDL_VendorUSB_IsXIDInterface(0x58, 0x42, 0x00, endpoints, 2));
    endpoints[0].address = 0x01;
    endpoints[1].address = 0x02;
    CHECK(!SDL_VendorUSB_IsXIDInterface(0x58, 0x42, 0x00, endpoints, 2));
    // A bulk or isochronous endpoint
    endpoints[0].address = 0x82;
    endpoints[0].attributes = 0x02;
    CHECK(!SDL_VendorUSB_IsXIDInterface(0x58, 0x42, 0x00, endpoints, 2));
    endpoints[0].attributes = 0x01;
    CHECK(!SDL_VendorUSB_IsXIDInterface(0x58, 0x42, 0x00, endpoints, 2));
    endpoints[0].attributes = 0x03;
    // Three endpoints
    endpoints[2].address = 0x83;
    endpoints[2].attributes = 0x03;
    CHECK(!SDL_VendorUSB_IsXIDInterface(0x58, 0x42, 0x00, endpoints, 3));
    // Class, subclass and protocol each count. HID class interfaces are not XID.
    CHECK(!SDL_VendorUSB_IsXIDInterface(0x03, 0x00, 0x00, endpoints, 2));
    CHECK(!SDL_VendorUSB_IsXIDInterface(0x58, 0x43, 0x00, endpoints, 2));
    CHECK(!SDL_VendorUSB_IsXIDInterface(0x58, 0x42, 0x01, endpoints, 2));
    CHECK(!SDL_VendorUSB_IsXIDInterface(0xFF, 0x5D, 0x01, endpoints, 2));
    CHECK(!SDL_VendorUSB_IsXIDInterface(0x58, 0x42, 0x00, NULL, 2));
    CHECK(SDL_VendorUSB_IsXIDInterface(SDL_XID_INTERFACE_CLASS, SDL_XID_INTERFACE_SUBCLASS,
                                       SDL_XID_INTERFACE_PROTOCOL, endpoints, 2));

    // The Steel Battalion's own ID picks its driver and hint
    CHECK(SDL_XID_IsSteelBattalionID(0x0A7B, 0xD000));
    CHECK(!SDL_XID_IsSteelBattalionID(0x0A7B, 0xD001));
    CHECK(!SDL_XID_IsSteelBattalionID(0x0A7C, 0xD000));
    CHECK(!SDL_XID_IsSteelBattalionID(0x045E, 0x0289));

    // The driver serves the class triple. HIDAPI's presence queries pass class 0.
    CHECK(SDL_XID_IsSupportedInterface(0x58, 0x42, 0x00));
    CHECK(!SDL_XID_IsSupportedInterface(0x03, 0x00, 0x00));
    CHECK(!SDL_XID_IsSupportedInterface(0x00, 0x00, 0x00));
    CHECK(!SDL_XID_IsSupportedInterface(-1, 0, 0));
    CHECK(!SDL_XID_IsSupportedInterface(0x58, 0x42, 0x01));
    CHECK(!SDL_XID_IsSupportedInterface(0xFF, 0x5D, 0x01));
}

/* G15 through the libusb routing, with an XID interface passed as the Xbox
   interface it is treated as. */
static void TestRouting(void)
{
    SDL_VendorUSBRouting routing;

    // A class 0x58 interface on FFFF:FFFF is a candidate, and so is a HID interface
    CHECK(SDL_VendorUSB_IsCandidate(0xFFFF, 0xFFFF, 0, 0x58, 0x42, 0x00, true));
    CHECK(SDL_VendorUSB_IsCandidate(0x0738, 0x4540, 0, 0x58, 0x42, 0x00, true));
    CHECK(SDL_VendorUSB_IsCandidate(0x0F30, 0x010B, 0, 0x03, 0x00, 0x00, false));
    CHECK(!SDL_VendorUSB_IsCandidate(0x0F30, 0x010B, 0, 0x58, 0x42, 0x00, false));

    // On Windows and macOS libusb keeps it, whitelist or not. Elsewhere Linux xpad keeps it.
    memset(&routing, 0, sizeof(routing));
    routing.libusb = true;
    routing.whitelist = true;
    routing.vendor = 0xFFFF;
    routing.product = 0xFFFF;
    routing.xbox = true;
    routing.platform = SDL_VENDORUSB_PLATFORM_WINDOWS;
    CHECK(!SDL_VendorUSB_Ignore(&routing));
    routing.platform = SDL_VENDORUSB_PLATFORM_MACOS;
    CHECK(!SDL_VendorUSB_Ignore(&routing));
    routing.platform = SDL_VENDORUSB_PLATFORM_OTHER;
    CHECK(SDL_VendorUSB_Ignore(&routing));
    routing.whitelist = false;
    routing.platform = SDL_VENDORUSB_PLATFORM_WINDOWS;
    CHECK(!SDL_VendorUSB_Ignore(&routing));
    // The HID-class Beat Pad and GGE909 stay with the Windows HID backend
    routing.whitelist = true;
    routing.xbox = false;
    routing.vendor = 0x0F30;
    routing.product = 0x010B;
    CHECK(SDL_VendorUSB_Ignore(&routing));
    routing.vendor = 0x0738;
    routing.product = 0x4540;
    CHECK(SDL_VendorUSB_Ignore(&routing));
    routing.libusb = false;
    CHECK(!SDL_VendorUSB_Ignore(&routing));

    // A driverless interface cannot be opened on Windows and is skipped. One bound to WinUSB opens.
    CHECK(SDL_VendorUSB_SkipUnopened(SDL_VENDORUSB_PLATFORM_WINDOWS, true, false));
    CHECK(!SDL_VendorUSB_SkipUnopened(SDL_VENDORUSB_PLATFORM_WINDOWS, true, true));
}

static void TestDescriptor(void)
{
    SDL_XIDDescriptor d;
    uint8_t bad[16];
    size_t length;

    CHECK(Parse(duke_descriptor, 16, &d));
    CHECK(d.type == 0x01 && d.subtype == 0x01 && d.max_input == 0x14 && d.max_output == 0x06);
    CHECK(Parse(sb_descriptor, 16, &d));
    CHECK(d.type == 0x80 && d.subtype == 0x01 && d.max_input == 0x1A && d.max_output == 0x16);
    CHECK(Parse(dvd_descriptor, 8, &d));
    CHECK(d.type == 0x03 && d.max_input == 0x06);

    // G14: every truncation below 8 bytes, byte 1 other than 0x42, and a bLength that does not fit
    for (length = 0; length < 8; ++length) {
        CHECK(!Parse(duke_descriptor, length, &d));
    }
    CHECK(Parse(duke_descriptor, 8, &d) == false); // bLength 16 does not fit in 8
    memcpy(bad, duke_descriptor, 16);
    bad[1] = 0x21;
    CHECK(!Parse(bad, 16, &d));
    bad[1] = 0x42;
    bad[0] = 0x07;
    CHECK(!Parse(bad, 16, &d));
    bad[0] = 0x08;
    CHECK(Parse(bad, 8, &d));
    CHECK(!SDL_XID_ParseDescriptor(NULL, 16, &d));
    CHECK(!SDL_XID_ParseDescriptor(duke_descriptor, 16, NULL));
}

static void TestIdentify(void)
{
    SDL_XIDIdentity id;
    uint8_t desc[16];
    size_t length;
    static const struct
    {
        uint8_t subtype;
        int type;
        bool light_gun;
        bool dance;
    } subtypes[] = {
        { 0x01, SDL_XID_JOYSTICK_GAMEPAD, false, false },
        { 0x02, SDL_XID_JOYSTICK_GAMEPAD, false, false },
        { 0x10, SDL_XID_JOYSTICK_WHEEL, false, false },
        { 0x20, SDL_XID_JOYSTICK_ARCADE_STICK, false, false },
        { 0x21, SDL_XID_JOYSTICK_ARCADE_STICK, false, false },
        { 0x30, SDL_XID_JOYSTICK_FLIGHT_STICK, false, false },
        { 0x40, SDL_XID_JOYSTICK_UNKNOWN, false, false },
        { 0x50, SDL_XID_JOYSTICK_UNKNOWN, true, false },
        { 0x60, SDL_XID_JOYSTICK_UNKNOWN, false, false },
        { 0x70, SDL_XID_JOYSTICK_UNKNOWN, false, false },
        { 0x80, SDL_XID_JOYSTICK_DANCE_PAD, false, true },
    };
    size_t i;

    // G13: the Duke and Controller S descriptors on an ID with no table entry
    CHECK(IdentifyWith(0x1234, 0x5678, duke_descriptor, 16, NULL, &id));
    CHECK(id.kind == SDL_XID_KIND_GAMEPAD && id.joystick_type == SDL_XID_JOYSTICK_GAMEPAD);
    CHECK(strcmp(id.name, "Xbox Controller") == 0);
    CHECK(id.naxes == 12 && id.nbuttons == 11 && id.nhats == 1 && !id.dance && !id.light_gun && !id.null_sticks);
    memcpy(desc, duke_descriptor, 16);
    desc[5] = 0x02;
    CHECK(IdentifyWith(0x1234, 0x5678, desc, 16, NULL, &id));
    CHECK(strcmp(id.name, "Xbox Controller S") == 0 && id.guid_byte == 0x02);
    for (i = 0; i < sizeof(subtypes) / sizeof(subtypes[0]); ++i) {
        desc[5] = subtypes[i].subtype;
        CHECK(IdentifyWith(0x1234, 0x5678, desc, 16, NULL, &id));
        CHECK(id.joystick_type == subtypes[i].type);
        CHECK(id.light_gun == subtypes[i].light_gun && id.dance == subtypes[i].dance);
        CHECK(id.nbuttons == (subtypes[i].dance ? 15 : subtypes[i].light_gun ? 12 : 11));
        CHECK(id.nhats == (subtypes[i].dance ? 0 : 1));
        CHECK(id.guid_byte == subtypes[i].subtype);
        if (subtypes[i].subtype > 0x02) {
            CHECK(strcmp(id.name, "Xbox Input Device") == 0);
        }
    }
    // An unknown subtype falls back to the ID's flags
    desc[5] = 0x33;
    CHECK(IdentifyWith(0x1234, 0x5678, desc, 16, NULL, &id) && id.joystick_type == SDL_XID_JOYSTICK_GAMEPAD);
    CHECK(IdentifyWith(0x0738, 0x45FF, desc, 16, NULL, &id) && id.joystick_type == SDL_XID_JOYSTICK_DANCE_PAD);

    // bType 3 closes the device, bType 0x80 is the Steel Battalion
    CHECK(!IdentifyWith(0x045E, 0x0284, dvd_descriptor, 8, NULL, &id));
    CHECK(IdentifyWith(0x1234, 0x5678, sb_descriptor, 16, NULL, &id));
    CHECK(id.kind == SDL_XID_KIND_STEEL_BATTALION && id.joystick_type == SDL_XID_JOYSTICK_UNKNOWN);
    CHECK(id.naxes == 9 && id.nbuttons == 46 && id.nhats == 0 && id.guid_byte == SDL_XID_GUID_STEEL_BATTALION);
    CHECK(strcmp(id.name, "Steel Battalion Controller") == 0);
    // SB9: the Steel Battalion descriptor on the pad IDs, even with a product string
    CHECK(IdentifyWith(0x045E, 0x0289, sb_descriptor, 16, "OGX360", &id));
    CHECK(id.kind == SDL_XID_KIND_STEEL_BATTALION && strcmp(id.name, "Steel Battalion Controller") == 0);

    // G14: no usable descriptor falls back to the ID tables
    for (length = 0; length < 8; ++length) {
        CHECK(IdentifyWith(0x0738, 0x45FF, duke_descriptor, length, NULL, &id));
        CHECK(id.joystick_type == SDL_XID_JOYSTICK_DANCE_PAD && id.dance && id.nhats == 0);
        CHECK(IdentifyWith(0x045E, 0x0289, duke_descriptor, length, NULL, &id));
        CHECK(id.joystick_type == SDL_XID_JOYSTICK_GAMEPAD && strcmp(id.name, "Xbox Controller S") == 0);
        CHECK(IdentifyWith(0x1234, 0x5678, duke_descriptor, length, NULL, &id));
        CHECK(id.joystick_type == SDL_XID_JOYSTICK_GAMEPAD && strcmp(id.name, "Xbox Input Device") == 0);
    }
    memcpy(desc, duke_descriptor, 16);
    desc[1] = 0x00;
    CHECK(IdentifyWith(0x0738, 0x45FF, desc, 16, NULL, &id) && id.joystick_type == SDL_XID_JOYSTICK_DANCE_PAD);
    // A stall: no descriptor at all
    CHECK(SDL_XID_Identify(0x0738, 0x45FF, NULL, NULL, &id) && id.dance);
    CHECK(SDL_XID_Identify(0x045E, 0x0289, NULL, NULL, &id) && id.joystick_type == SDL_XID_JOYSTICK_GAMEPAD);
    // Without a descriptor, the Steel Battalion's own ID
    CHECK(SDL_XID_Identify(0x0A7B, 0xD000, NULL, NULL, &id) && id.kind == SDL_XID_KIND_STEEL_BATTALION);
    // With one, the descriptor decides
    CHECK(IdentifyWith(0x0A7B, 0xD000, duke_descriptor, 16, NULL, &id) && id.kind == SDL_XID_KIND_GAMEPAD);

    // The dance flags apply with a gamepad descriptor too, as Linux applies them per ID
    CHECK(IdentifyWith(0x0C12, 0x8809, duke_descriptor, 16, NULL, &id));
    CHECK(id.dance && id.null_sticks && id.joystick_type == SDL_XID_JOYSTICK_DANCE_PAD && id.guid_byte == SDL_XID_GUID_DANCE_PAD);
    CHECK(strcmp(id.name, "RedOctane Dance Pad") == 0);
    // A dance-pad ID wins over the light-gun subtype
    memcpy(desc, duke_descriptor, 16);
    desc[5] = 0x50;
    CHECK(IdentifyWith(0x0738, 0x45FF, desc, 16, NULL, &id));
    CHECK(id.dance && !id.light_gun && id.nbuttons == 15 && id.nhats == 0 && id.guid_byte == SDL_XID_GUID_DANCE_PAD);
    // The trigger flag changes nothing: triggers stay axes
    CHECK(IdentifyWith(0x0E4C, 0x1103, NULL, 0, NULL, &id));
    CHECK(!id.dance && !id.null_sticks && id.joystick_type == SDL_XID_JOYSTICK_GAMEPAD && id.naxes == 12);

    // Names: the product string first, then the table, then the subtype
    CHECK(IdentifyWith(0x0738, 0x4520, duke_descriptor, 16, "MC2 Racing Wheel", &id) && strcmp(id.name, "MC2 Racing Wheel") == 0);
    CHECK(IdentifyWith(0x0738, 0x4520, duke_descriptor, 16, "", &id) && strcmp(id.name, "Mad Catz Control Pad Pro") == 0);
    CHECK(IdentifyWith(0xFFFF, 0xFFFF, NULL, 0, NULL, &id) && strcmp(id.name, "Xbox Controller") == 0);
    CHECK(!SDL_XID_Identify(0x045E, 0x0202, NULL, NULL, NULL));
}

static void TestKnownDevices(void)
{
    // The dance-pad IDs and the one trigger ID, as Linux flags them
    static const struct
    {
        uint16_t vendor;
        uint16_t product;
        uint8_t flags;
    } flagged[] = {
        { 0x0738, 0x4540, SDL_XID_FLAG_DPAD_BUTTONS },
        { 0x0738, 0x45FF, SDL_XID_FLAG_DPAD_BUTTONS },
        { 0x0738, 0x4743, SDL_XID_FLAG_DPAD_BUTTONS },
        { 0x0738, 0x6040, SDL_XID_FLAG_DPAD_BUTTONS },
        { 0x0C12, 0x8809, SDL_XID_FLAG_DANCE_PAD },
        { 0x0D2F, 0x0002, SDL_XID_FLAG_DPAD_BUTTONS },
        { 0x0E4C, 0x1103, SDL_XID_FLAG_TRIGGER_BUTTONS },
        { 0x12AB, 0x8809, SDL_XID_FLAG_DPAD_BUTTONS },
        { 0x1430, 0x8888, SDL_XID_FLAG_DPAD_BUTTONS },
    };
    // A sample of the rest, including the two light guns and the placeholder ID
    static const uint16_t plain[][2] = {
        { 0x044F, 0x0F00 }, { 0x045E, 0x0202 }, { 0x045E, 0x0285 }, { 0x045E, 0x0287 },
        { 0x045E, 0x0288 }, { 0x045E, 0x0289 }, { 0x046D, 0xCA84 }, { 0x0738, 0x4520 },
        { 0x0C12, 0x9902 }, { 0x0E4C, 0x3240 }, { 0x0F30, 0x010B }, { 0x3767, 0x0101 },
        { 0x0B9A, 0x016B }, { 0x1292, 0x3006 }, { 0xFFFF, 0xFFFF },
    };
    size_t i;
    int total = 0;
    uint32_t vendor, product;

    for (i = 0; i < sizeof(flagged) / sizeof(flagged[0]); ++i) {
        const SDL_XIDKnownDevice *known = SDL_XID_FindKnownDevice(flagged[i].vendor, flagged[i].product);
        CHECK(known && known->flags == flagged[i].flags && known->name && *known->name);
    }
    for (i = 0; i < sizeof(plain) / sizeof(plain[0]); ++i) {
        const SDL_XIDKnownDevice *known = SDL_XID_FindKnownDevice(plain[i][0], plain[i][1]);
        CHECK(known && known->flags == 0 && known->name && *known->name);
    }
    // 63 IDs in all, each listed once, and nothing else
    for (vendor = 0; vendor <= 0xFFFF; vendor += 1) {
        if (vendor != 0x044F && vendor != 0x045E && vendor != 0x046D && vendor != 0x05FD &&
            vendor != 0x05FE && vendor != 0x062A && vendor != 0x06A3 && vendor != 0x0738 &&
            vendor != 0x0B9A && vendor != 0x0C12 && vendor != 0x0D2F && vendor != 0x0E4C &&
            vendor != 0x0E6F && vendor != 0x0E8F && vendor != 0x0F30 && vendor != 0x102C &&
            vendor != 0x1292 && vendor != 0x12AB && vendor != 0x1430 && vendor != 0x3767 && vendor != 0xFFFF) {
            CHECK(!SDL_XID_FindKnownDevice((uint16_t)vendor, 0x0202));
            continue;
        }
        for (product = 0; product <= 0xFFFF; ++product) {
            if (SDL_XID_FindKnownDevice((uint16_t)vendor, (uint16_t)product)) {
                ++total;
            }
        }
    }
    CHECK(total == 63);
    CHECK(!SDL_XID_FindKnownDevice(0x0A7B, 0xD000)); // the Steel Battalion goes by its descriptor or kind
}

/* ------------------------------------------------------------------------ */
/* The gamepad report, G1 to G12 */

#define MAX_PACKETS 128

// Every packet of G1 to G5, 20 bytes each
static int GamepadPackets(uint8_t packets[MAX_PACKETS][20])
{
    static const uint8_t digital[] = { 0x01, 0x02, 0x04, 0x08, 0x05, 0x10, 0x20, 0x40, 0x80 };
    static const uint8_t levels[] = { 0xFF, 0x20, 0x21 };
    static const uint8_t words[][2] = { { 0x00, 0x80 }, { 0xFF, 0x7F } };
    int n = 0, i, j;

    GamepadNeutral(packets[n++]);
    for (i = 0; i < (int)sizeof(digital); ++i) {
        GamepadNeutral(packets[n]);
        packets[n++][2] = digital[i];
    }
    for (i = 4; i <= 9; ++i) {
        for (j = 0; j < (int)sizeof(levels); ++j) {
            GamepadNeutral(packets[n]);
            packets[n++][i] = levels[j];
        }
    }
    for (i = 10; i <= 11; ++i) {
        GamepadNeutral(packets[n]);
        packets[n++][i] = 0xFF;
    }
    for (i = 12; i <= 18; i += 2) {
        for (j = 0; j < 2; ++j) {
            GamepadNeutral(packets[n]);
            packets[n][i] = words[j][0];
            packets[n++][i + 1] = words[j][1];
        }
    }
    return n;
}

// G1 to G5 on one identity: a pad, or a dance pad whose D-pad bits are buttons 11 to 14
static void RunGamepadTests(const SDL_XIDIdentity *pad)
{
    SDL_XIDGamepadState s;
    uint8_t r[20];
    int i;
    static const struct
    {
        uint8_t offset;
        int button;
        int axis;
    } faces[] = {
        { 4, 0, 6 }, { 5, 1, 7 }, { 6, 2, 8 }, { 7, 3, 9 }, { 8, 10, 11 }, { 9, 9, 10 },
    };
    static const struct
    {
        uint8_t bit;
        int button;
    } digital[] = {
        { 0x10, 6 }, { 0x20, 4 }, { 0x40, 7 }, { 0x80, 8 },
    };
    static const struct
    {
        uint8_t bits;
        uint8_t hat;
        uint16_t arrows; // the dance-pad buttons
    } hats[] = {
        { 0x01, SDL_XID_HAT_UP, 1u << 11 },
        { 0x02, SDL_XID_HAT_DOWN, 1u << 12 },
        { 0x04, SDL_XID_HAT_LEFT, 1u << 13 },
        { 0x08, SDL_XID_HAT_RIGHT, 1u << 14 },
        { 0x05, SDL_XID_HAT_UP | SDL_XID_HAT_LEFT, (1u << 11) | (1u << 13) },
        { 0x0A, SDL_XID_HAT_DOWN | SDL_XID_HAT_RIGHT, (1u << 12) | (1u << 14) },
        { 0x03, 0, (1u << 11) | (1u << 12) },
        { 0x0C, 0, (1u << 13) | (1u << 14) },
        { 0x0F, 0, (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) },
        { 0x07, SDL_XID_HAT_LEFT, (1u << 11) | (1u << 12) | (1u << 13) },
    };

    // G1: neutral
    GamepadNeutral(r);
    CHECK(Decode(pad, r, 20, &s));
    CHECK(s.buttons == 0 && s.hat == 0);
    CHECK(s.axes[0] == 0 && s.axes[2] == 0 && s.axes[1] == -1 && s.axes[3] == -1);
    for (i = 4; i < 12; ++i) {
        CHECK(s.axes[i] == AXIS_MIN);
    }

    // G2: the D-pad and the digital buttons one at a time
    for (i = 0; i < (int)(sizeof(hats) / sizeof(hats[0])); ++i) {
        GamepadNeutral(r);
        r[2] = hats[i].bits;
        CHECK(Decode(pad, r, 20, &s));
        if (pad->dance) {
            CHECK(s.hat == 0 && s.buttons == hats[i].arrows);
        } else {
            CHECK(s.hat == hats[i].hat && s.buttons == 0);
        }
    }
    for (i = 0; i < (int)(sizeof(digital) / sizeof(digital[0])); ++i) {
        GamepadNeutral(r);
        r[2] = digital[i].bit;
        CHECK(Decode(pad, r, 20, &s) && s.buttons == (1u << digital[i].button) && s.hat == 0);
    }

    // G3: the face buttons and their pressure
    for (i = 0; i < (int)(sizeof(faces) / sizeof(faces[0])); ++i) {
        int v;
        GamepadNeutral(r);
        r[faces[i].offset] = 0xFF;
        CHECK(Decode(pad, r, 20, &s) && s.buttons == (1u << faces[i].button) && s.axes[faces[i].axis] == AXIS_MAX);
        r[faces[i].offset] = 0x20;
        CHECK(Decode(pad, r, 20, &s) && s.buttons == 0 && s.axes[faces[i].axis] == -24544);
        r[faces[i].offset] = 0x21;
        CHECK(Decode(pad, r, 20, &s) && s.buttons == (1u << faces[i].button) && s.axes[faces[i].axis] == -24287);
        for (v = 0; v < 256; ++v) {
            r[faces[i].offset] = (uint8_t)v;
            CHECK(Decode(pad, r, 20, &s));
            CHECK(Down(s.buttons, faces[i].button) == (v > 0x20));
            CHECK(s.axes[faces[i].axis] == v * 257 - 32768);
        }
    }
    CHECK(!Down(s.buttons, 5)); // no Guide

    // G4: the triggers
    GamepadNeutral(r);
    r[10] = 0xFF;
    CHECK(Decode(pad, r, 20, &s) && s.axes[4] == AXIS_MAX && s.axes[5] == AXIS_MIN && s.buttons == 0);
    r[10] = 0;
    r[11] = 0xFF;
    CHECK(Decode(pad, r, 20, &s) && s.axes[5] == AXIS_MAX && s.axes[4] == AXIS_MIN);

    // G5: the sticks, Y inverted
    GamepadNeutral(r);
    r[12] = 0x00;
    r[13] = 0x80;
    CHECK(Decode(pad, r, 20, &s) && s.axes[0] == AXIS_MIN);
    r[12] = 0xFF;
    r[13] = 0x7F;
    CHECK(Decode(pad, r, 20, &s) && s.axes[0] == AXIS_MAX);
    GamepadNeutral(r);
    r[14] = 0xFF;
    r[15] = 0x7F;
    CHECK(Decode(pad, r, 20, &s) && s.axes[1] == AXIS_MIN);
    r[14] = 0x00;
    r[15] = 0x80;
    CHECK(Decode(pad, r, 20, &s) && s.axes[1] == AXIS_MAX);
    GamepadNeutral(r);
    r[16] = 0x00;
    r[17] = 0x80;
    CHECK(Decode(pad, r, 20, &s) && s.axes[2] == AXIS_MIN);
    r[16] = 0xFF;
    r[17] = 0x7F;
    CHECK(Decode(pad, r, 20, &s) && s.axes[2] == AXIS_MAX);
    GamepadNeutral(r);
    r[18] = 0xFF;
    r[19] = 0x7F;
    CHECK(Decode(pad, r, 20, &s) && s.axes[3] == AXIS_MIN);
    r[18] = 0x00;
    r[19] = 0x80;
    CHECK(Decode(pad, r, 20, &s) && s.axes[3] == AXIS_MAX);
    GamepadNeutral(r);
    r[12] = 0x34;
    r[13] = 0x12;
    r[14] = 0x78;
    r[15] = 0x56;
    CHECK(Decode(pad, r, 20, &s) && s.axes[0] == 0x1234 && s.axes[1] == (int16_t)~0x5678);
}

// G6 to G8 on one identity
static void RunGamepadFraming(const SDL_XIDIdentity *pad)
{
    static uint8_t packets[MAX_PACKETS][20];
    SDL_XIDGamepadState s;
    uint8_t r[20], other[20], big[32], stale[64];
    size_t length;
    int i, count;

    /* G6: every truncation of every packet of G1 to G5 changes nothing, also
       when the bytes past the received length are a stale copy of another
       report, one with every control held */
    memset(other, 0xFF, sizeof(other));
    other[0] = 0x00;
    other[1] = 0x14;
    count = GamepadPackets(packets);
    CHECK(count <= MAX_PACKETS);
    for (i = 0; i < count; ++i) {
        for (length = 0; length < 20; ++length) {
            CHECK(!Decode(pad, packets[i], length, &s));
            memset(stale, 0xFF, sizeof(stale));
            memcpy(stale, other, sizeof(other));
            memcpy(stale, packets[i], length);
            CHECK(!SDL_XID_DecodeGamepad(pad, stale, length, &s));
        }
        CHECK(Decode(pad, packets[i], 20, &s));
    }

    GamepadNeutral(r);
    r[2] = 0x11;
    r[4] = 0xFF;
    r[12] = 0x00;
    r[13] = 0x40;

    // G7: a 32-byte transfer decodes only its first 20 bytes
    memcpy(big, r, 20);
    for (i = 20; i < 32; ++i) {
        big[i] = (uint8_t)(0x5A + i);
    }
    {
        SDL_XIDGamepadState t;
        CHECK(Decode(pad, r, 20, &s) && Decode(pad, big, 32, &t));
        CHECK(memcmp(&s, &t, sizeof(s)) == 0);
    }

    // G8: byte 0 other than 0x00, or byte 1 other than 0x14
    for (i = 1; i < 256; ++i) {
        r[0] = (uint8_t)i;
        CHECK(!Decode(pad, r, 20, &s));
    }
    r[0] = 0;
    for (i = 0; i < 256; ++i) {
        r[1] = (uint8_t)i;
        CHECK(Decode(pad, r, 20, &s) == (i == 0x14));
    }
    r[1] = 0x14;
    CHECK(!SDL_XID_DecodeGamepad(NULL, r, 20, &s));
    CHECK(!SDL_XID_DecodeGamepad(pad, NULL, 20, &s));
    CHECK(!SDL_XID_DecodeGamepad(pad, r, 20, NULL));
}

static void TestGamepad(void)
{
    RunGamepadTests(Pad());
    RunGamepadFraming(Pad());
}

/* The Beat Pad section: tests 1 to 9 of the gamepad family in dance-pad mode,
   on 0738:4540 identified by its ID alone. The HID-class variant has no XID
   descriptor to read. */
static void TestBeatPad(void)
{
    SDL_XIDIdentity beat;
    SDL_XIDGamepadState s;
    uint8_t r[20];

    CHECK(SDL_XID_Identify(0x0738, 0x4540, NULL, NULL, &beat));
    CHECK(beat.dance && !beat.null_sticks && !beat.light_gun && beat.nhats == 0 && beat.nbuttons == 15);
    CHECK(beat.joystick_type == SDL_XID_JOYSTICK_DANCE_PAD && strcmp(beat.name, "Mad Catz Beat Pad") == 0);
    RunGamepadTests(&beat);
    RunGamepadFraming(&beat);

    // G9 on it: opposite arrows held together
    GamepadNeutral(r);
    r[2] = 0x03;
    CHECK(Decode(&beat, r, 20, &s) && s.hat == 0 && s.buttons == ((1u << 11) | (1u << 12)));
    r[2] = 0x0F;
    CHECK(Decode(&beat, r, 20, &s) && s.hat == 0 && s.buttons == ((1u << 11) | (1u << 12) | (1u << 13) | (1u << 14)));
}

static void TestGamepadModes(void)
{
    SDL_XIDIdentity dance, gun, redoctane, sb;
    SDL_XIDGamepadState s;
    uint8_t r[20], desc[16];
    int i;

    // G9: dance-pad mode from the subtype
    memcpy(desc, duke_descriptor, 16);
    desc[5] = 0x80;
    CHECK(IdentifyWith(0x1234, 0x5678, desc, 16, NULL, &dance));
    GamepadNeutral(r);
    r[2] = 0x03;
    CHECK(Decode(&dance, r, 20, &s) && s.hat == 0);
    CHECK(Down(s.buttons, 11) && Down(s.buttons, 12) && !Down(s.buttons, 13) && !Down(s.buttons, 14));
    r[2] = 0x0F;
    CHECK(Decode(&dance, r, 20, &s) && s.hat == 0);
    for (i = 11; i <= 14; ++i) {
        CHECK(Down(s.buttons, i));
    }
    for (i = 0; i < 4; ++i) {
        r[2] = (uint8_t)(1 << i);
        CHECK(Decode(&dance, r, 20, &s) && s.buttons == (1u << (11 + i)) && s.hat == 0);
    }
    // The sticks still read on a dance pad without the stick flag
    r[2] = 0;
    r[12] = 0xFF;
    r[13] = 0x7F;
    CHECK(Decode(&dance, r, 20, &s) && s.axes[0] == AXIS_MAX);

    // G10: 0C12:8809 without a descriptor, the sticks read centered
    CHECK(SDL_XID_Identify(0x0C12, 0x8809, NULL, NULL, &redoctane));
    CHECK(redoctane.dance && redoctane.null_sticks && redoctane.joystick_type == SDL_XID_JOYSTICK_DANCE_PAD);
    GamepadNeutral(r);
    for (i = 12; i < 20; i += 2) {
        r[i] = 0xFF;
        r[i + 1] = 0x7F;
    }
    r[2] = 0x09;
    CHECK(Decode(&redoctane, r, 20, &s));
    CHECK(s.axes[0] == 0 && s.axes[1] == 0 && s.axes[2] == 0 && s.axes[3] == 0);
    CHECK(Down(s.buttons, 11) && Down(s.buttons, 14) && s.hat == 0);
    r[10] = 0xFF;
    CHECK(Decode(&redoctane, r, 20, &s) && s.axes[4] == AXIS_MAX); // triggers stay axes

    // G11: light-gun mode
    desc[5] = 0x50;
    CHECK(IdentifyWith(0x0B9A, 0x016B, desc, 16, NULL, &gun));
    CHECK(gun.light_gun && gun.nbuttons == 12);
    GamepadNeutral(r);
    r[3] = 0x20;
    CHECK(Decode(&gun, r, 20, &s) && s.buttons == (1u << 11));
    r[3] = 0xC0;
    CHECK(Decode(&gun, r, 20, &s) && s.buttons == 0);
    r[3] = 0xFF;
    CHECK(Decode(&gun, r, 20, &s) && s.buttons == (1u << 11));
    // Byte 3 means nothing on a pad
    CHECK(Decode(Pad(), r, 20, &s) && s.buttons == 0);

    // SB9: a gamepad report means nothing to the Steel Battalion
    CHECK(IdentifyWith(0x045E, 0x0289, sb_descriptor, 16, NULL, &sb));
    GamepadNeutral(r);
    CHECK(!Decode(&sb, r, 20, &s));
}

static void TestRumble(void)
{
    uint8_t out[6];
    static const uint8_t full_left[6] = { 0x00, 0x06, 0xFF, 0xFF, 0x00, 0x00 };
    static const uint8_t mixed[6] = { 0x00, 0x06, 0x00, 0x80, 0x34, 0x12 };

    // G12
    memset(out, 0xEE, sizeof(out));
    CHECK(SDL_XID_BuildRumble(0xFFFF, 0x0000, out) == 6 && memcmp(out, full_left, 6) == 0);
    CHECK(SDL_XID_BuildRumble(0x8000, 0x1234, out) == 6 && memcmp(out, mixed, 6) == 0);
    CHECK(SDL_XID_BuildRumble(0, 0, out) == 6 && out[0] == 0 && out[1] == 6 && !out[2] && !out[3] && !out[4] && !out[5]);
}

/* ------------------------------------------------------------------------ */
/* The session: a simulated device behind the sink */

#define MAX_QUEUE  32
#define MAX_LOG    8
#define ONE_MINUTE (60 * 1000)

typedef struct ControlRequest
{
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
    unsigned int timeout_ms;
} ControlRequest;

typedef struct TestDevice
{
    // How the device answers its two requests: bytes returned, or negative for a stall
    int descriptor_result;
    uint8_t descriptor[SDL_XID_READ_LENGTH];
    int report_result;
    uint8_t report[SDL_XID_READ_LENGTH];
    // Interrupt IN transfers waiting, and the result once they run out
    uint8_t queue[MAX_QUEUE][SDL_XID_READ_LENGTH];
    int queue_size[MAX_QUEUE];
    int queued, taken;
    int idle_result;
    bool write_ok;
    bool joystick_open;
    uint64_t now_ms; // the injected clock
    // What the session did
    int controls;
    ControlRequest control_log[MAX_LOG];
    int reads;
    int writes;
    uint8_t written[MAX_LOG][SDL_XID_READ_LENGTH];
    size_t written_size[MAX_LOG];
    int gamepad_posts, sb_posts;
    void *posted_to;
    SDL_XIDGamepadState gamepad;
    SDL_XIDSteelBattalionState sb;
} TestDevice;

static int TestControlIn(void *userdata, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                         uint8_t *data, uint16_t length, unsigned int timeout_ms)
{
    TestDevice *device = (TestDevice *)userdata;
    const uint8_t *source;
    int result;

    CHECK(device->controls < MAX_LOG);
    if (device->controls < MAX_LOG) {
        ControlRequest *log = &device->control_log[device->controls];
        log->request_type = request_type;
        log->request = request;
        log->value = value;
        log->index = index;
        log->length = length;
        log->timeout_ms = timeout_ms;
    }
    ++device->controls;

    if (request_type == 0xC1 && request == 0x06) {
        source = device->descriptor;
        result = device->descriptor_result;
    } else if (request_type == 0xA1 && request == 0x01) {
        source = device->report;
        result = device->report_result;
    } else {
        return -1; // a stall, as the device answers any other request
    }
    if (result > (int)length) {
        result = length; // the device never sends more than wLength
    }
    if (result > 0) {
        memcpy(data, source, (size_t)result);
    }
    return result;
}

static int TestRead(void *userdata, uint8_t *data, size_t length)
{
    TestDevice *device = (TestDevice *)userdata;
    int size;

    ++device->reads;
    if (device->taken == device->queued) {
        return device->idle_result;
    }
    size = device->queue_size[device->taken];
    if ((size_t)size > length) {
        size = (int)length;
    }
    memcpy(data, device->queue[device->taken], (size_t)size);
    ++device->taken;
    return size;
}

static bool TestWrite(void *userdata, const uint8_t *data, size_t length)
{
    TestDevice *device = (TestDevice *)userdata;

    CHECK(length <= SDL_XID_READ_LENGTH);
    if (device->writes < MAX_LOG && length <= SDL_XID_READ_LENGTH) {
        memcpy(device->written[device->writes], data, length);
        device->written_size[device->writes] = length;
    }
    ++device->writes;
    return device->write_ok;
}

static void *TestJoystick(void *userdata)
{
    TestDevice *device = (TestDevice *)userdata;

    return device->joystick_open ? (void *)device : NULL;
}

static void TestPostGamepad(void *userdata, void *joystick, const SDL_XIDGamepadState *state)
{
    TestDevice *device = (TestDevice *)userdata;

    CHECK(joystick == device && device->joystick_open);
    ++device->gamepad_posts;
    device->posted_to = joystick;
    device->gamepad = *state;
}

static void TestPostSB(void *userdata, void *joystick, const SDL_XIDSteelBattalionState *state)
{
    TestDevice *device = (TestDevice *)userdata;

    CHECK(joystick == device && device->joystick_open);
    ++device->sb_posts;
    device->posted_to = joystick;
    device->sb = *state;
}

static SDL_XIDSink Sink(TestDevice *device)
{
    SDL_XIDSink sink;

    sink.userdata = device;
    sink.control_in = TestControlIn;
    sink.read = TestRead;
    sink.write = TestWrite;
    sink.joystick = TestJoystick;
    sink.gamepad = TestPostGamepad;
    sink.steel_battalion = TestPostSB;
    return sink;
}

// A device that answers GET_DESCRIPTOR with the given descriptor and stalls GET_REPORT
static void NewDevice(TestDevice *device, const uint8_t *descriptor, int length)
{
    memset(device, 0, sizeof(*device));
    device->descriptor_result = length;
    if (descriptor && length > 0) {
        memcpy(device->descriptor, descriptor, (size_t)length);
    }
    device->report_result = -1;
    device->write_ok = true;
}

static void Queue(TestDevice *device, const uint8_t *data, int size)
{
    CHECK(device->queued < MAX_QUEUE);
    if (device->queued < MAX_QUEUE) {
        memcpy(device->queue[device->queued], data, (size_t)size);
        device->queue_size[device->queued] = size;
        ++device->queued;
    }
}

static bool SameGamepad(const SDL_XIDGamepadState *a, const SDL_XIDGamepadState *b)
{
    int i;

    if (a->buttons != b->buttons || a->hat != b->hat) {
        return false;
    }
    for (i = 0; i < SDL_XID_GAMEPAD_AXES; ++i) {
        if (a->axes[i] != b->axes[i]) {
            return false;
        }
    }
    return true;
}

static bool SameSB(const SDL_XIDSteelBattalionState *a, const SDL_XIDSteelBattalionState *b)
{
    int i;

    if (a->buttons != b->buttons) {
        return false;
    }
    for (i = 0; i < SDL_XID_SB_AXES; ++i) {
        if (a->axes[i] != b->axes[i]) {
            return false;
        }
    }
    return true;
}

static void RestGamepad(const SDL_XIDIdentity *identity, SDL_XIDGamepadState *rest)
{
    uint8_t r[20];

    GamepadNeutral(r);
    CHECK(Decode(identity, r, 20, rest));
}

/* G13: open sends GET_DESCRIPTOR, then the one GET_REPORT, and nothing else */
static void TestOpenRequests(void)
{
    TestDevice device;
    SDL_XIDSink sink = Sink(&device);
    SDL_XIDSession session;
    SDL_XIDGamepadState rest;
    uint8_t seed[20];
    uint8_t iface;

    for (iface = 0; iface < 4; iface += 3) {
        NewDevice(&device, duke_descriptor, 16);
        CHECK(SDL_XID_Open(&session, 0x045E, 0x0202, iface, NULL, &sink));
        CHECK(device.controls == 2);
        CHECK(device.control_log[0].request_type == 0xC1 && device.control_log[0].request == 0x06);
        CHECK(device.control_log[0].value == 0x4200 && device.control_log[0].index == iface);
        CHECK(device.control_log[0].length == 16 && device.control_log[0].timeout_ms == 100);
        CHECK(device.control_log[1].request_type == 0xA1 && device.control_log[1].request == 0x01);
        CHECK(device.control_log[1].value == 0x0100 && device.control_log[1].index == iface);
        CHECK(device.control_log[1].length == 20 && device.control_log[1].timeout_ms == 100);
        CHECK(device.reads == 0 && device.writes == 0 && device.gamepad_posts == 0);
        CHECK(session.identity.kind == SDL_XID_KIND_GAMEPAD && strcmp(session.identity.name, "Xbox Controller") == 0);
        // The GET_REPORT stalled, so the state is at rest
        RestGamepad(&session.identity, &rest);
        CHECK(SameGamepad(&session.gamepad, &rest));
    }

    // The GET_REPORT reply seeds the state: A held, left stick right
    NewDevice(&device, duke_descriptor, 16);
    GamepadNeutral(seed);
    seed[4] = 0xFF;
    seed[12] = 0xFF;
    seed[13] = 0x7F;
    memcpy(device.report, seed, 20);
    device.report_result = 20;
    CHECK(SDL_XID_Open(&session, 0x045E, 0x0202, 0, NULL, &sink));
    CHECK(Down(session.gamepad.buttons, 0) && session.gamepad.axes[0] == AXIS_MAX);
    // A short, a malformed or a stalled reply leaves the state at rest
    device.report_result = 19;
    CHECK(SDL_XID_Open(&session, 0x045E, 0x0202, 0, NULL, &sink));
    RestGamepad(&session.identity, &rest);
    CHECK(SameGamepad(&session.gamepad, &rest));
    device.report_result = 20;
    device.report[1] = 0x1A;
    CHECK(SDL_XID_Open(&session, 0x045E, 0x0202, 0, NULL, &sink));
    CHECK(SameGamepad(&session.gamepad, &rest));
    device.report_result = 0;
    CHECK(SDL_XID_Open(&session, 0x045E, 0x0202, 0, NULL, &sink));
    CHECK(SameGamepad(&session.gamepad, &rest));

    // bType 3 closes the device after the one request
    NewDevice(&device, dvd_descriptor, 8);
    CHECK(!SDL_XID_Open(&session, 0x045E, 0x0284, 0, NULL, &sink));
    CHECK(device.controls == 1 && device.writes == 0);

    // bType 0x80: the Steel Battalion, GET_DESCRIPTOR only (SB8)
    NewDevice(&device, sb_descriptor, 16);
    CHECK(SDL_XID_Open(&session, 0x0A7B, 0xD000, 0, NULL, &sink));
    CHECK(device.controls == 1 && session.identity.kind == SDL_XID_KIND_STEEL_BATTALION);
    CHECK(device.control_log[0].request_type == 0xC1 && device.control_log[0].value == 0x4200);
}

/* G14 through the open: a stall, every truncation, and a bad type all fall
   back to the ID tables */
static void TestOpenFallback(void)
{
    TestDevice device;
    SDL_XIDSink sink = Sink(&device);
    SDL_XIDSession session;
    uint8_t bad[16];
    int length;

    NewDevice(&device, NULL, -1);
    CHECK(SDL_XID_Open(&session, 0x0738, 0x45FF, 0, NULL, &sink));
    CHECK(session.identity.joystick_type == SDL_XID_JOYSTICK_DANCE_PAD && device.controls == 2);
    CHECK(SDL_XID_Open(&session, 0x045E, 0x0289, 0, NULL, &sink));
    CHECK(session.identity.joystick_type == SDL_XID_JOYSTICK_GAMEPAD);
    CHECK(SDL_XID_Open(&session, 0x1234, 0x5678, 0, NULL, &sink));
    CHECK(session.identity.joystick_type == SDL_XID_JOYSTICK_GAMEPAD && strcmp(session.identity.name, "Xbox Input Device") == 0);
    CHECK(SDL_XID_Open(&session, 0x0A7B, 0xD000, 0, NULL, &sink));
    CHECK(session.identity.kind == SDL_XID_KIND_STEEL_BATTALION);

    for (length = 0; length < 16; ++length) {
        // The Steel Battalion descriptor cut short: only 8 bytes or more with a fitting bLength count
        NewDevice(&device, sb_descriptor, length);
        CHECK(SDL_XID_Open(&session, 0x0738, 0x45FF, 0, NULL, &sink));
        CHECK(session.identity.kind == SDL_XID_KIND_GAMEPAD && session.identity.dance);
    }
    memcpy(bad, sb_descriptor, 16);
    bad[0] = 0x08;
    NewDevice(&device, bad, 8);
    CHECK(SDL_XID_Open(&session, 0x0738, 0x45FF, 0, NULL, &sink));
    CHECK(session.identity.kind == SDL_XID_KIND_STEEL_BATTALION);
    bad[1] = 0x41;
    NewDevice(&device, bad, 16);
    CHECK(SDL_XID_Open(&session, 0x0738, 0x45FF, 0, NULL, &sink));
    CHECK(session.identity.kind == SDL_XID_KIND_GAMEPAD && session.identity.dance);
    CHECK(!SDL_XID_Open(NULL, 0x0738, 0x45FF, 0, NULL, &sink));
    CHECK(!SDL_XID_Open(&session, 0x0738, 0x45FF, 0, NULL, NULL));
}

/* G16 and SB8: the joystick gets the state when it opens and after each
   report, silence changes nothing, a read error ends the session, and a
   replugged device starts at rest */
static void TestSession(void)
{
    TestDevice device;
    SDL_XIDSink sink = Sink(&device);
    SDL_XIDSession session;
    SDL_XIDGamepadState rest, held;
    uint8_t r[32];
    int i;

    NewDevice(&device, duke_descriptor, 16);
    CHECK(SDL_XID_Open(&session, 0x045E, 0x0289, 0, NULL, &sink));
    RestGamepad(&session.identity, &rest);

    // Reports before the application opens the joystick move the state and post nothing
    GamepadNeutral(r);
    r[5] = 0xFF;
    Queue(&device, r, 20);
    CHECK(SDL_XID_Update(&session, &sink));
    CHECK(device.gamepad_posts == 0 && Down(session.gamepad.buttons, 1));
    held = session.gamepad;

    // The open joystick gets the whole state at the next update
    device.joystick_open = true;
    SDL_XID_Start(&session);
    CHECK(device.gamepad_posts == 0);
    CHECK(SDL_XID_Update(&session, &sink));
    CHECK(device.gamepad_posts == 1 && SameGamepad(&device.gamepad, &held));

    // With an injected clock, 10 minutes of silence at 1 ms per update keeps it connected and emits nothing
    for (device.now_ms = 0; device.now_ms < 10 * ONE_MINUTE; ++device.now_ms) {
        if (!SDL_XID_Update(&session, &sink)) {
            break;
        }
    }
    CHECK(device.now_ms == 10 * ONE_MINUTE);
    CHECK(device.gamepad_posts == 1 && device.writes == 0 && device.controls == 2);

    // Each report is posted in order, a 32-byte transfer as its 20 bytes, and a malformed one not at all
    GamepadNeutral(r);
    r[2] = 0x10;
    Queue(&device, r, 20);
    r[2] = 0x00;
    for (i = 20; i < 32; ++i) {
        r[i] = (uint8_t)(0xC0 + i);
    }
    Queue(&device, r, 32);
    r[1] = 0x13;
    Queue(&device, r, 20);
    CHECK(SDL_XID_Update(&session, &sink));
    CHECK(device.gamepad_posts == 3 && SameGamepad(&device.gamepad, &rest));
    CHECK(SameGamepad(&session.gamepad, &rest));

    /* G6 through the session: truncated transfers after a report with every
       control held. The update's buffer still holds that report's bytes past
       each short transfer, and nothing changes. */
    {
        static uint8_t packets[MAX_PACKETS][20];
        uint8_t other[20];
        SDL_XIDGamepadState expected;
        int p, count = GamepadPackets(packets), posts;
        size_t length;

        memset(other, 0xFF, sizeof(other));
        other[0] = 0x00;
        other[1] = 0x14;
        CHECK(Decode(&session.identity, other, 20, &expected));
        for (p = 0; p < count; ++p) {
            device.queued = device.taken = 0;
            Queue(&device, other, 20);
            for (length = 1; length < 20; ++length) {
                Queue(&device, packets[p], (int)length);
            }
            posts = device.gamepad_posts;
            CHECK(SDL_XID_Update(&session, &sink));
            CHECK(device.gamepad_posts == posts + 1 && SameGamepad(&device.gamepad, &expected));
            CHECK(SameGamepad(&session.gamepad, &expected));
        }
        device.queued = device.taken = 0;
    }

    // A read error disconnects it
    device.idle_result = -1;
    CHECK(!SDL_XID_Update(&session, &sink));
    CHECK(device.writes == 0);

    // Replug: a new session on the same device starts at rest, whatever the old one held
    NewDevice(&device, duke_descriptor, 16);
    CHECK(SDL_XID_Open(&session, 0x045E, 0x0289, 0, NULL, &sink));
    CHECK(SameGamepad(&session.gamepad, &rest));
    device.joystick_open = true;
    SDL_XID_Start(&session);
    CHECK(SDL_XID_Update(&session, &sink));
    CHECK(device.gamepad_posts == 1 && SameGamepad(&device.gamepad, &rest));
    CHECK(!session.post_pending);
    CHECK(device.writes == 0);
    CHECK(!SDL_XID_Update(NULL, &sink) && !SDL_XID_Update(&session, NULL));
}

static void TestSessionSteelBattalion(void)
{
    TestDevice device;
    SDL_XIDSink sink = Sink(&device);
    SDL_XIDSession session;
    SDL_XIDSteelBattalionState rest, expected;
    uint8_t r[32];
    int i;

    NewDevice(&device, sb_descriptor, 16);
    CHECK(SDL_XID_Open(&session, 0x045E, 0x0289, 0, NULL, &sink)); // SB9: ogx360 on the pad IDs
    CHECK(session.identity.kind == SDL_XID_KIND_STEEL_BATTALION && device.controls == 1);

    // At rest: levers centered, pedals released, tuner 0, no gear
    CHECK(session.steel_battalion.buttons == 0);
    for (i = 0; i < 5; ++i) {
        CHECK(session.steel_battalion.axes[i] == 0);
    }
    for (i = 5; i < 9; ++i) {
        CHECK(session.steel_battalion.axes[i] == AXIS_MIN);
    }
    rest = session.steel_battalion;

    // SB8: nothing is written until an effect arrives, silence keeps it connected
    device.joystick_open = true;
    SDL_XID_Start(&session);
    for (device.now_ms = 0; device.now_ms < 10 * ONE_MINUTE; ++device.now_ms) {
        if (!SDL_XID_Update(&session, &sink)) {
            break;
        }
    }
    CHECK(device.now_ms == 10 * ONE_MINUTE);
    CHECK(device.sb_posts == 1 && device.gamepad_posts == 0 && SameSB(&device.sb, &rest));
    CHECK(device.writes == 0 && device.controls == 1);

    // SB9: a 20-byte gamepad report changes nothing. A 26-byte report in a 32-byte transfer is read.
    GamepadNeutral(r);
    r[4] = 0xFF;
    Queue(&device, r, 20);
    SBNeutral(r);
    r[2] = 0x01;
    r[26] = 0x77;
    Queue(&device, r, 32);
    CHECK(DecodeSB(r, 26, &expected));
    CHECK(SDL_XID_Update(&session, &sink));
    CHECK(device.sb_posts == 2 && SameSB(&device.sb, &expected) && device.gamepad_posts == 0);

    // SB6 through the session: truncated transfers after a report with everything set change nothing
    {
        static uint8_t packets[MAX_SB_PACKETS][26];
        uint8_t other[26];
        SDL_XIDSteelBattalionState all;
        int p, count = SBPackets(packets), posts;
        size_t length;

        memset(other, 0xFF, sizeof(other));
        other[0] = 0x00;
        other[1] = 0x1A;
        other[25] = 0x03;
        CHECK(DecodeSB(other, 26, &all));
        for (p = 0; p < count; ++p) {
            device.queued = device.taken = 0;
            Queue(&device, other, 26);
            for (length = 1; length < 26; ++length) {
                Queue(&device, packets[p], (int)length);
            }
            posts = device.sb_posts;
            CHECK(SDL_XID_Update(&session, &sink));
            CHECK(device.sb_posts == posts + 1 && SameSB(&device.sb, &all) && SameSB(&session.steel_battalion, &all));
        }
        device.queued = device.taken = 0;
    }

    // A read error disconnects it, and the replugged instance starts at rest and writes nothing
    device.idle_result = -5;
    CHECK(!SDL_XID_Update(&session, &sink));
    NewDevice(&device, sb_descriptor, 16);
    CHECK(SDL_XID_Open(&session, 0x0A7B, 0xD000, 0, NULL, &sink));
    device.joystick_open = true;
    SDL_XID_Start(&session);
    CHECK(SDL_XID_Update(&session, &sink));
    CHECK(device.sb_posts == 1 && SameSB(&device.sb, &rest) && device.writes == 0);
}

static void TestSessionOutput(void)
{
    TestDevice device;
    SDL_XIDSink sink = Sink(&device);
    SDL_XIDSession pad, sb;
    uint8_t effect[23];
    static const uint8_t rumble[6] = { 0x00, 0x06, 0x00, 0x80, 0x34, 0x12 };
    size_t length;

    // G12: rumble writes the one report, and nothing else is written
    NewDevice(&device, duke_descriptor, 16);
    CHECK(SDL_XID_Open(&pad, 0x045E, 0x0202, 0, NULL, &sink));
    CHECK(SDL_XID_Rumble(&pad, 0x8000, 0x1234, &sink) == SDL_XID_OUTPUT_SENT);
    CHECK(device.writes == 1 && device.written_size[0] == 6 && memcmp(device.written[0], rumble, 6) == 0);
    device.write_ok = false;
    CHECK(SDL_XID_Rumble(&pad, 0, 0, &sink) == SDL_XID_OUTPUT_FAILED);
    CHECK(device.writes == 2);
    device.write_ok = true;
    // The pad has no lamps
    memset(effect, 0, sizeof(effect));
    CHECK(SDL_XID_SendEffect(&pad, effect, 22, &sink) == SDL_XID_OUTPUT_UNSUPPORTED);
    CHECK(device.writes == 2);

    // SB7 through the session: 22 bytes become one 32-byte write, other sizes write nothing
    NewDevice(&device, sb_descriptor, 16);
    CHECK(SDL_XID_Open(&sb, 0x0A7B, 0xD000, 0, NULL, &sink));
    CHECK(SDL_XID_Rumble(&sb, 0xFFFF, 0xFFFF, &sink) == SDL_XID_OUTPUT_UNSUPPORTED);
    effect[2] = 0x0F;
    CHECK(SDL_XID_SendEffect(&sb, effect, 22, &sink) == SDL_XID_OUTPUT_SENT);
    CHECK(device.writes == 1 && device.written_size[0] == 32);
    CHECK(device.written[0][0] == 0x00 && device.written[0][1] == 0x16 && device.written[0][2] == 0x0F);
    for (length = 0; length <= 23; ++length) {
        if (length != 22) {
            CHECK(SDL_XID_SendEffect(&sb, effect, length, &sink) == SDL_XID_OUTPUT_INVALID);
        }
    }
    CHECK(device.writes == 1);
    device.write_ok = false;
    CHECK(SDL_XID_SendEffect(&sb, effect, 22, &sink) == SDL_XID_OUTPUT_FAILED);
    CHECK(SDL_XID_Rumble(NULL, 0, 0, &sink) == SDL_XID_OUTPUT_UNSUPPORTED);
    CHECK(SDL_XID_SendEffect(&sb, effect, 22, NULL) == SDL_XID_OUTPUT_UNSUPPORTED);
}

/* ------------------------------------------------------------------------ */
/* The mappings: each element names the control the decoder puts there */

// The binding a mapping gives a name, such as "b11", or "" when it has none
static void Binding(const char *mapping, const char *name, char *out, size_t size)
{
    const size_t name_length = strlen(name);
    const char *at = mapping;

    out[0] = '\0';
    while (*at) {
        const char *end = strchr(at, ',');
        if (!end) {
            end = at + strlen(at);
        }
        if ((size_t)(end - at) > name_length && strncmp(at, name, name_length) == 0 && at[name_length] == ':') {
            size_t length = (size_t)(end - at) - name_length - 1;
            if (length >= size) {
                length = size - 1;
            }
            memcpy(out, at + name_length + 1, length);
            out[length] = '\0';
            return;
        }
        at = *end ? end + 1 : end;
    }
}

static void CheckMapping(const SDL_XIDIdentity *identity, const char *mapping)
{
    // Each control at full travel: one or two report bytes
    static const struct
    {
        const char *name;
        uint8_t offset;
        uint8_t value;
        uint8_t offset2;
        uint8_t value2;
    } controls[] = {
        { "a", 4, 0xFF, 0, 0 }, { "b", 5, 0xFF, 0, 0 }, { "x", 6, 0xFF, 0, 0 }, { "y", 7, 0xFF, 0, 0 },
        { "back", 2, 0x20, 0, 0 }, { "start", 2, 0x10, 0, 0 }, { "leftstick", 2, 0x40, 0, 0 }, { "rightstick", 2, 0x80, 0, 0 },
        { "leftshoulder", 9, 0xFF, 0, 0 }, { "rightshoulder", 8, 0xFF, 0, 0 },
        { "dpup", 2, 0x01, 0, 0 }, { "dpdown", 2, 0x02, 0, 0 }, { "dpleft", 2, 0x04, 0, 0 }, { "dpright", 2, 0x08, 0, 0 },
        { "lefttrigger", 10, 0xFF, 0, 0 }, { "righttrigger", 11, 0xFF, 0, 0 },
        { "leftx", 12, 0xFF, 13, 0x7F }, { "lefty", 14, 0x00, 15, 0x80 },
        { "rightx", 16, 0xFF, 17, 0x7F }, { "righty", 18, 0x00, 19, 0x80 },
    };
    SDL_XIDGamepadState rest, s;
    uint8_t r[20];
    char binding[16];
    size_t i;
    int elements = 0;
    const char *at;

    for (at = mapping; *at; ++at) {
        elements += (*at == ',');
    }
    CHECK(elements == (int)(sizeof(controls) / sizeof(controls[0])));
    Binding(mapping, "guide", binding, sizeof(binding));
    CHECK(binding[0] == '\0');

    RestGamepad(identity, &rest);
    for (i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        char *end = NULL;
        long index = -1;

        GamepadNeutral(r);
        r[controls[i].offset] = controls[i].value;
        if (controls[i].offset2) {
            r[controls[i].offset2] = controls[i].value2;
        }
        CHECK(Decode(identity, r, 20, &s));
        Binding(mapping, controls[i].name, binding, sizeof(binding));
        if (binding[0] == 'h' && strncmp(binding, "h0.", 3) == 0) {
            index = strtol(binding + 3, &end, 10);
            CHECK(end && *end == '\0');
            CHECK(identity->nhats == 1 && s.hat == index && s.buttons == 0);
            continue;
        }
        if (binding[0] == 'b' || binding[0] == 'a') {
            index = strtol(binding + 1, &end, 10);
            CHECK(end && *end == '\0' && end != binding + 1);
        }
        if (binding[0] == 'b') {
            CHECK(index >= 0 && index < identity->nbuttons);
            CHECK(index >= 0 && index < 16 && s.buttons == (1u << index) && s.hat == rest.hat);
        } else if (binding[0] == 'a') {
            // The axis moves to an end, and only that axis moves
            int j;
            CHECK(index >= 0 && index < 6);
            CHECK(index < 6 && (s.axes[index] == AXIS_MAX || (index < 4 && s.axes[index] == AXIS_MIN)));
            for (j = 0; j < SDL_XID_GAMEPAD_AXES; ++j) {
                if (j != index && j < 6) {
                    CHECK(s.axes[j] == rest.axes[j]);
                }
            }
        } else {
            printf("no binding for %s\n", controls[i].name);
            CHECK(false);
        }
    }
}

static void TestMappings(void)
{
    SDL_XIDIdentity pad, dance, gun;
    uint8_t desc[16];

    static const uint8_t pad_bytes[] = { 0x00, 0x01, 0x02, 0x10, 0x20, 0x21, 0x30, 0x40, 0x50, 0x60, 0x70 };
    size_t i;

    CHECK(SDL_XID_GetMapping(SDL_XID_GUID_STEEL_BATTALION) == NULL);
    CHECK(SDL_XID_GetMapping(SDL_XID_GUID_DANCE_PAD) != NULL);
    CHECK(strcmp(SDL_XID_GetMapping(SDL_XID_GUID_DANCE_PAD), SDL_XID_MAPPING_DANCE_PAD) == 0);
    for (i = 0; i < sizeof(pad_bytes); ++i) {
        const char *mapping = SDL_XID_GetMapping(pad_bytes[i]);
        CHECK(mapping && strcmp(mapping, SDL_XID_MAPPING_GAMEPAD) == 0);
    }

    CHECK(IdentifyWith(0x045E, 0x0202, duke_descriptor, 16, NULL, &pad));
    CheckMapping(&pad, SDL_XID_GetMapping(pad.guid_byte));
    CHECK(SDL_XID_Identify(0x0738, 0x4540, NULL, NULL, &dance));
    CHECK(dance.guid_byte == SDL_XID_GUID_DANCE_PAD);
    CheckMapping(&dance, SDL_XID_GetMapping(dance.guid_byte));
    memcpy(desc, duke_descriptor, 16);
    desc[5] = 0x50;
    CHECK(IdentifyWith(0x1292, 0x3006, desc, 16, NULL, &gun));
    CheckMapping(&gun, SDL_XID_GetMapping(gun.guid_byte));
}

/* ------------------------------------------------------------------------ */
/* The Steel Battalion, SB1 to SB7 */

static void TestSteelBattalion(void)
{
    SDL_XIDSteelBattalionState s, t;
    uint8_t r[26], big[32], stale[64];
    int bit, v;
    size_t length;

    // SB1: neutral
    SBNeutral(r);
    CHECK(DecodeSB(r, 26, &s));
    CHECK(s.axes[0] == 0 && s.axes[1] == 0 && s.axes[2] == 0 && s.axes[3] == 0 && s.axes[4] == 0);
    CHECK(s.axes[5] == AXIS_MIN && s.axes[6] == AXIS_MIN && s.axes[7] == AXIS_MIN && s.axes[8] == AXIS_MIN);
    CHECK(s.buttons == ((uint64_t)1 << 40));

    // SB2: each of the 39 button bits alone
    for (bit = 0; bit < 39; ++bit) {
        SBNeutral(r);
        r[25] = 0x00;
        r[2 + bit / 8] = (uint8_t)(1 << (bit % 8));
        CHECK(DecodeSB(r, 26, &s) && s.buttons == ((uint64_t)1 << bit));
    }
    SBNeutral(r);
    r[25] = 0x00;
    r[6] = 0x80;
    r[7] = 0xFF;
    CHECK(DecodeSB(r, 26, &s) && s.buttons == 0);

    // SB3: the axes
    SBNeutral(r);
    r[9] = 0x00;
    CHECK(DecodeSB(r, 26, &s) && s.axes[0] == AXIS_MIN);
    r[9] = 0x80;
    CHECK(DecodeSB(r, 26, &s) && s.axes[0] == 0);
    r[9] = 0xFF;
    CHECK(DecodeSB(r, 26, &s) && s.axes[0] == 32512);
    SBNeutral(r);
    r[11] = 0x00;
    CHECK(DecodeSB(r, 26, &s) && s.axes[1] == AXIS_MIN);
    r[11] = 0xFF;
    CHECK(DecodeSB(r, 26, &s) && s.axes[1] == 32512);
    for (v = 0; v < 3; ++v) {
        static const int offsets[3] = { 13, 15, 17 };
        SBNeutral(r);
        r[offsets[v]] = 0x80;
        CHECK(DecodeSB(r, 26, &s) && s.axes[2 + v] == AXIS_MIN);
        r[offsets[v]] = 0x01;
        CHECK(DecodeSB(r, 26, &s) && s.axes[2 + v] == 256);
        r[offsets[v]] = 0x7F;
        CHECK(DecodeSB(r, 26, &s) && s.axes[2 + v] == 32512);
    }
    for (v = 0; v < 3; ++v) {
        static const int pedals[3] = { 19, 21, 23 };
        SBNeutral(r);
        r[pedals[v]] = 0xFF;
        CHECK(DecodeSB(r, 26, &s) && s.axes[5 + v] == 32512);
    }
    // The even bytes change nothing
    SBNeutral(r);
    CHECK(DecodeSB(r, 26, &t));
    for (v = 8; v <= 22; v += 2) {
        r[v] = 0xFF;
    }
    CHECK(DecodeSB(r, 26, &s) && memcmp(&s, &t, sizeof(s)) == 0);

    // SB4: the tuner dial, its low nibble only
    for (v = 0; v < 16; ++v) {
        SBNeutral(r);
        r[24] = (uint8_t)v;
        CHECK(DecodeSB(r, 26, &s) && s.axes[8] == -32768 + 4369 * v);
        r[24] = (uint8_t)(0xF0 | v);
        CHECK(DecodeSB(r, 26, &s) && s.axes[8] == -32768 + 4369 * v);
    }
    SBNeutral(r);
    r[24] = 0xF3;
    CHECK(DecodeSB(r, 26, &s) && s.axes[8] == -32768 + 4369 * 3);

    // SB5: the gear lever
    {
        static const struct
        {
            uint8_t value;
            int button;
        } gears[] = {
            { 0xFE, 39 }, { 0xFF, 40 }, { 0x01, 41 }, { 0x02, 42 }, { 0x03, 43 }, { 0x04, 44 }, { 0x05, 45 },
            { 0x07, 39 }, { 0x08, 40 }, { 0x09, 41 }, { 0x0A, 42 }, { 0x0B, 43 }, { 0x0C, 44 }, { 0x0D, 45 },
            { 0x00, -1 }, { 0x06, -1 }, { 0x0E, -1 }, { 0x0F, -1 }, { 0x10, -1 }, { 0x80, -1 }, { 0xFD, -1 },
        };
        size_t i;
        for (i = 0; i < sizeof(gears) / sizeof(gears[0]); ++i) {
            SBNeutral(r);
            r[25] = gears[i].value;
            CHECK(DecodeSB(r, 26, &s));
            if (gears[i].button < 0) {
                CHECK(s.buttons == 0);
            } else {
                CHECK(s.buttons == ((uint64_t)1 << gears[i].button));
            }
        }
        for (v = 0; v < 256; ++v) {
            SBNeutral(r);
            r[25] = (uint8_t)v;
            CHECK(DecodeSB(r, 26, &s) && CountDown(s.buttons) <= 1);
        }
    }

    /* SB6: every truncation of every packet of SB1 to SB5 changes nothing,
       with a stale copy of another report past the received length, one with
       every button, axis and gear byte set */
    {
        static uint8_t packets[MAX_SB_PACKETS][26];
        uint8_t other[26];
        int p, count = SBPackets(packets);

        memset(other, 0xFF, sizeof(other));
        other[0] = 0x00;
        other[1] = 0x1A;
        other[25] = 0x03;
        CHECK(count <= MAX_SB_PACKETS);
        for (p = 0; p < count; ++p) {
            for (length = 0; length < 26; ++length) {
                CHECK(!DecodeSB(packets[p], length, &s));
                memset(stale, 0xFF, sizeof(stale));
                memcpy(stale, other, sizeof(other));
                memcpy(stale, packets[p], length);
                CHECK(!SDL_XID_DecodeSteelBattalion(stale, length, &s));
            }
            CHECK(DecodeSB(packets[p], 26, &s));
        }
    }
    SBNeutral(r);
    r[2] = 0x01;
    r[9] = 0x10;
    memcpy(big, r, 26);
    memset(big + 26, 0xAB, 6);
    CHECK(DecodeSB(r, 26, &t) && DecodeSB(big, 32, &s) && memcmp(&s, &t, sizeof(s)) == 0);
    for (v = 0; v < 256; ++v) {
        r[1] = (uint8_t)v;
        CHECK(DecodeSB(r, 26, &s) == (v == 0x1A));
    }
    r[1] = 0x1A;
    r[0] = 0x01;
    CHECK(!DecodeSB(r, 26, &s));
    CHECK(!SDL_XID_DecodeSteelBattalion(NULL, 26, &s));
    CHECK(!SDL_XID_DecodeSteelBattalion(r, 26, NULL));
}

static void TestLamps(void)
{
    uint8_t effect[23], out[32], expected[32];
    size_t length;
    int i;

    // SB7: a 22-byte effect lights Emergency eject
    memset(effect, 0, sizeof(effect));
    effect[2] = 0x0F;
    memset(out, 0xEE, sizeof(out));
    CHECK(SDL_XID_BuildLamps(effect, 22, out));
    memset(expected, 0, sizeof(expected));
    expected[1] = 0x16;
    expected[2] = 0x0F;
    CHECK(memcmp(out, expected, 32) == 0);
    // Other sizes write nothing
    for (length = 0; length <= 23; ++length) {
        if (length == 22) {
            continue;
        }
        memset(out, 0xEE, sizeof(out));
        CHECK(!SDL_XID_BuildLamps(effect, length, out));
        CHECK(out[0] == 0xEE && out[31] == 0xEE);
    }
    // Bytes 0 and 1 are always 00 16
    effect[0] = 0xFF;
    effect[1] = 0xFF;
    for (i = 2; i < 22; ++i) {
        effect[i] = (uint8_t)(0x10 * (i % 16) + (i % 16));
    }
    CHECK(SDL_XID_BuildLamps(effect, 22, out));
    CHECK(out[0] == 0x00 && out[1] == 0x16);
    CHECK(memcmp(out + 2, effect + 2, 20) == 0);
    for (i = 22; i < 32; ++i) {
        CHECK(out[i] == 0);
    }
    CHECK(!SDL_XID_BuildLamps(NULL, 22, out));
    CHECK(!SDL_XID_BuildLamps(effect, 22, NULL));
}

int main(void)
{
    TestInterface();
    TestRouting();
    TestDescriptor();
    TestIdentify();
    TestKnownDevices();
    TestGamepad();
    TestBeatPad();
    TestGamepadModes();
    TestRumble();
    TestOpenRequests();
    TestOpenFallback();
    TestSession();
    TestSessionSteelBattalion();
    TestSessionOutput();
    TestMappings();
    TestSteelBattalion();
    TestLamps();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
