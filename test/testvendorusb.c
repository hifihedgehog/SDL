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

/* Offline test of the shared vendor-USB path: the rules, the routing between
 * the libusb and platform backends, and the endpoint walk. No device, no SDL
 * runtime. Exact-size heap buffers let AddressSanitizer catch any read past
 * the length a function was given. */

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

/* Interface 0 of the Intel Wireless Series base station, both alternates, as
 * read from hardware with PyUSB on 2026-09-07. JoypadOS
 * tools/intel-wireless-series/test.c:94-102. */
static const unsigned char intel_interface0[] = {
    9, 4, 0, 0, 1, 3, 1, 1, 0,           /* alternate 0: HID boot keyboard */
    9, 0x21, 0, 1, 0x21, 1, 0x22, 0x3f, 0, /* HID descriptor */
    7, 5, 0x81, 3, 8, 0, 10,             /* interrupt IN 0x81, 8 bytes */
    9, 4, 0, 1, 3, 0, 0, 0, 0,           /* alternate 1: vendor, 3 endpoints */
    7, 5, 0x81, 3, 27, 0, 1,             /* interrupt IN 0x81, 27 bytes */
    7, 5, 0x01, 3, 25, 0, 1,             /* interrupt OUT 0x01, 25 bytes */
    7, 5, 0x02, 0, 8, 0, 0,              /* control 0x02, never opened */
};

static const SDL_VendorUSBRule *IntelRule(void)
{
    return SDL_VendorUSB_FindRule(USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES, 0, 3, 1, 1);
}

/* Runs the walk on an exact-size heap copy, then on a larger buffer whose
 * bytes past length are stale, and requires both to agree. */
static bool Select(const SDL_VendorUSBRule *rule, uint8_t interface_number,
                   const unsigned char *bytes, size_t length, SDL_VendorUSBSelection *out)
{
    SDL_VendorUSBSelection exact, stale;
    unsigned char *copy = (unsigned char *)malloc(length ? length : 1);
    unsigned char padded[512];
    bool a, b;

    if (!copy || length > sizeof(padded) - 64) {
        free(copy);
        ++failures;
        printf("FAIL: test buffer\n");
        return false;
    }
    memcpy(copy, bytes, length);
    memset(&exact, 0xEE, sizeof(exact));
    a = SDL_VendorUSB_SelectEndpoints(rule, interface_number, copy, length, &exact);
    free(copy);

    memset(padded, 0xA5, sizeof(padded));
    memcpy(padded, bytes, length);
    /* Stale bytes past length that would complete a truncated descriptor. */
    if (length < sizeof(intel_interface0)) {
        memcpy(padded + length, intel_interface0 + length, sizeof(intel_interface0) - length);
    }
    memset(&stale, 0xEE, sizeof(stale));
    b = SDL_VendorUSB_SelectEndpoints(rule, interface_number, padded, length, &stale);

    CHECK(a == b);
    if (a && b) {
        CHECK(memcmp(&exact, &stale, sizeof(exact)) == 0);
    }
    if (a && out) {
        *out = exact;
    }
    return a;
}

static void TestRules(void)
{
    const SDL_VendorUSBRule *rule;

    rule = IntelRule();
    CHECK(rule != NULL);
    if (rule) {
        CHECK(rule->alternate == 1);
        CHECK(rule->in_endpoint == 0x81 && rule->in_size == 27);
        CHECK(rule->out_endpoint == 0x01 && rule->out_size == 25);
        CHECK((rule->flags & SDL_VENDORUSB_RAW_OUTPUT) != 0);
    }
    /* The rule names interface 0 whatever its alternate or class. */
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES, 0, 0, 0, 0) == rule);
    /* Interface 1, the boot mouse, has no rule. */
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES, 1, 3, 1, 2) == NULL);

    /* The Big Button receiver matches its class triple on any interface. */
    rule = SDL_VendorUSB_FindRule(USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER, 0, 0xFF, 0x5D, 0x04);
    CHECK(rule != NULL);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER, 3, 0xFF, 0x5D, 0x04) == rule);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER, 0, 0xFF, 0x5D, 0x01) == NULL);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_MICROSOFT, 0x028E, 0, 0xFF, 0x5D, 0x04) == NULL);

    /* The GameCube adapter's interface 0, which is HID class 3/0/0. */
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_GAMECUBE_ADAPTER, 0, 3, 0, 0) != NULL);

    CHECK(SDL_VendorUSB_IsVendorDevice(USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES));
    CHECK(SDL_VendorUSB_IsVendorDevice(USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER));
    CHECK(SDL_VendorUSB_IsVendorDevice(USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_GAMECUBE_ADAPTER));
    CHECK(!SDL_VendorUSB_IsVendorDevice(USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_PRO));
    CHECK(!SDL_VendorUSB_IsVendorDevice(0x054C, 0x09CC));
    CHECK(!SDL_VendorUSB_IsVendorDevice(USB_VENDOR_INTEL, 0xC014));

    /* The Gametrak's rule serves Windows only: its one HID interface, the
       first IN endpoint, and no OUT endpoint required */
    rule = SDL_VendorUSB_FindRule(USB_VENDOR_IN2GAMES, USB_PRODUCT_IN2GAMES_GAMETRAK, 0, 3, 0, 0);
    CHECK(rule && (rule->flags & SDL_VENDORUSB_WINDOWS_ONLY) && !(rule->flags & SDL_VENDORUSB_RAW_OUTPUT));
    CHECK(rule && rule->in_endpoint == 0 && rule->out_endpoint == 0 && rule->alternate == 0);
    CHECK(SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_WINDOWS));
    CHECK(!SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_OTHER));
    CHECK(!SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_MACOS));
    CHECK(SDL_VendorUSB_IsVendorDevice(USB_VENDOR_IN2GAMES, USB_PRODUCT_IN2GAMES_GAMETRAK));
    CHECK(!SDL_VendorUSB_RuleApplies(NULL, SDL_VENDORUSB_PLATFORM_WINDOWS));
    /* Every other rule serves every platform */
    rule = SDL_VendorUSB_FindRule(USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_GAMECUBE_ADAPTER, 0, 3, 0, 0);
    CHECK(SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_OTHER) && SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_MACOS));
    rule = SDL_VendorUSB_FindRule(USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES, 0, 3, 1, 1);
    CHECK(SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_OTHER) && SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_WINDOWS));

    /* Part 6: the DJI RC's DUML interface, class 0xFF and subclass 0x43 on
       any interface number and with any protocol. MTP and ADB stay out. */
    rule = SDL_VendorUSB_FindRule(USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 1, 0xFF, 0x43, 0x01);
    CHECK(rule && (rule->flags & SDL_VENDORUSB_MATCH_CLASS) && (rule->flags & SDL_VENDORUSB_ANY_PROTOCOL) &&
          (rule->flags & SDL_VENDORUSB_RAW_OUTPUT) && !(rule->flags & SDL_VENDORUSB_WINDOWS_ONLY));
    CHECK(rule && rule->in_endpoint == 0x83 && rule->out_endpoint == 0x02 && rule->alternate == 0);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 0, 0xFF, 0x43, 0x00) == rule);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 1, 0xFF, 0x43, 0x7F) == rule);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 2, 0xFF, 0x42, 0x01) == NULL);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 0, 0x06, 0x01, 0x01) == NULL);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 1, 0xFE, 0x43, 0x01) == NULL);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_DJI, 0x1021, 0, 0xFF, 0x43, 0x01) == NULL);
    CHECK(SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_WINDOWS) && SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_OTHER));
    CHECK(SDL_VendorUSB_IsVendorDevice(USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330));
    /* Without the flag the protocol still counts, as for the Big Button receiver */
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER, 0, 0xFF, 0x5D, 0x05) == NULL);

    /* Part 8: the I-Force devices, interface 0 whatever its class, commands
       unchanged, on Windows only */
    {
        static const uint16_t iforce[][2] = {
            { USB_VENDOR_THRUSTMASTER, 0xA01C }, { USB_VENDOR_LOGITECH, 0xC281 }, { USB_VENDOR_LOGITECH, 0xC291 },
            { USB_VENDOR_AVB, 0x020A }, { USB_VENDOR_AVB, 0x8884 }, { USB_VENDOR_AVB, 0x8888 },
            { USB_VENDOR_ACTLABS, 0xC084 }, { USB_VENDOR_ACTLABS, 0xC094 }, { USB_VENDOR_ACTLABS, 0xC0A4 },
            { USB_VENDOR_SAITEK, 0xFF04 }, { USB_VENDOR_GUILLEMOT, 0x0001 }, { USB_VENDOR_GUILLEMOT, 0x0003 },
            { USB_VENDOR_GUILLEMOT, 0x0004 }, { USB_VENDOR_GUILLEMOT, 0xA302 },
        };
        size_t i;

        CHECK(USB_VENDOR_AVB == 0x05EF && USB_VENDOR_ACTLABS == 0x061C && USB_VENDOR_GUILLEMOT == 0x06F8);
        for (i = 0; i < sizeof(iforce) / sizeof(iforce[0]); ++i) {
            rule = SDL_VendorUSB_FindRule(iforce[i][0], iforce[i][1], 0, 0xFF, 0x00, 0x00);
            CHECK(rule && rule->flags == (SDL_VENDORUSB_RAW_OUTPUT | SDL_VENDORUSB_WINDOWS_ONLY));
            CHECK(rule && rule->interface_number == 0 && rule->alternate == 0 && rule->in_endpoint == 0 && rule->out_endpoint == 0);
            CHECK(rule && rule->in_size == 0 && rule->out_size == 0);
            CHECK(SDL_VendorUSB_FindRule(iforce[i][0], iforce[i][1], 0, 0x03, 0x00, 0x00) == rule);
            CHECK(SDL_VendorUSB_FindRule(iforce[i][0], iforce[i][1], 1, 0xFF, 0x00, 0x00) == NULL);
            CHECK(SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_WINDOWS));
            CHECK(!SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_OTHER) && !SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_MACOS));
            CHECK(SDL_VendorUSB_IsVendorDevice(iforce[i][0], iforce[i][1]));
        }
        /* The same vendors' other devices, and the IDs that stay out of the part */
        CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_LOGITECH, 0xC215, 0, 3, 0, 0) == NULL);
        CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_LOGITECH, 0xC20E, 0, 3, 0, 0) == NULL);
        CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_SAITEK, 0xCF17, 0, 3, 0, 0) == NULL);
        CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_AVB, 0x8886, 0, 0xFF, 0, 0) == NULL);
    }

    /* A rule that serves another platform leaves the device as if it had
       none. On Windows it takes interface 0 alone. */
    CHECK(SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_WINDOWS, USB_VENDOR_LOGITECH, 0xC291, 0, 0xFF, 0, 0, false));
    CHECK(!SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_OTHER, USB_VENDOR_LOGITECH, 0xC291, 0, 0xFF, 0, 0, false));
    CHECK(!SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_MACOS, USB_VENDOR_LOGITECH, 0xC291, 0, 0xFF, 0, 0, false));
    CHECK(!SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_WINDOWS, USB_VENDOR_LOGITECH, 0xC291, 1, 0x03, 0, 0, false));
    CHECK(SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_OTHER, USB_VENDOR_LOGITECH, 0xC291, 1, 0x03, 0, 0, false));
    CHECK(SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_OTHER, USB_VENDOR_IN2GAMES, USB_PRODUCT_IN2GAMES_GAMETRAK, 0, 0x03, 0, 0, false));
    CHECK(SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_MACOS, USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES, 0, 0x03, 1, 1, false));
    CHECK(!SDL_VendorUSB_IsCandidate(SDL_VENDORUSB_PLATFORM_OTHER, USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES, 1, 0x03, 1, 2, false));
}

static void TestIntelDescriptors(void)
{
    const SDL_VendorUSBRule *rule = IntelRule();
    SDL_VendorUSBSelection selection;
    unsigned char bad[sizeof(intel_interface0)];
    size_t n;

    /* The full descriptors select alternate 1, 0x81 and 0x01, never 0x02. */
    memset(&selection, 0, sizeof(selection));
    CHECK(Select(rule, 0, intel_interface0, sizeof(intel_interface0), &selection));
    CHECK(selection.alternate == 1);
    CHECK(selection.in.address == 0x81 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT &&
          selection.in.max_packet_size == 27 && selection.in.read_size == 27);
    CHECK(selection.out.address == 0x01 && selection.out.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT &&
          selection.out.max_packet_size == 25 && selection.out.read_size == 25);

    /* Every truncation shorter than 48 bytes fails. 48 ends exactly after
     * endpoint 0x01 and succeeds. 49 to 54 end inside endpoint 0x02. */
    for (n = 0; n <= sizeof(intel_interface0); ++n) {
        const bool expected = (n == 48 || n == sizeof(intel_interface0));
        memset(&selection, 0, sizeof(selection));
        CHECK(Select(rule, 0, intel_interface0, n, &selection) == expected);
        if (expected) {
            CHECK(selection.in.address == 0x81 && selection.out.address == 0x01);
        }
    }

    /* A zero length in the alternate 1 interface descriptor fails, with no
     * endless walk. */
    memcpy(bad, intel_interface0, sizeof(bad));
    bad[25] = 0;
    CHECK(!Select(rule, 0, bad, sizeof(bad), NULL));
    /* A length of 1 anywhere fails. */
    memcpy(bad, intel_interface0, sizeof(bad));
    bad[41] = 1;
    CHECK(!Select(rule, 0, bad, sizeof(bad), NULL));
    /* An interface descriptor shorter than 9 bytes fails. */
    memcpy(bad, intel_interface0, sizeof(bad));
    bad[0] = 8;
    CHECK(!Select(rule, 0, bad, sizeof(bad), NULL));
    /* An endpoint descriptor shorter than 7 bytes fails. */
    memcpy(bad, intel_interface0, sizeof(bad));
    bad[34] = 6;
    CHECK(!Select(rule, 0, bad, sizeof(bad), NULL));

    /* The rule names the sizes. A 26-byte IN endpoint is another device. */
    memcpy(bad, intel_interface0, sizeof(bad));
    bad[38] = 26;
    CHECK(!Select(rule, 0, bad, sizeof(bad), NULL));
    /* So is a 24-byte OUT endpoint. */
    memcpy(bad, intel_interface0, sizeof(bad));
    bad[45] = 24;
    CHECK(!Select(rule, 0, bad, sizeof(bad), NULL));

    /* Another interface number has no alternate 1 here. */
    CHECK(!Select(rule, 1, intel_interface0, sizeof(intel_interface0), NULL));

    /* Without alternate 1 nothing is selected, and alternate 0's 8-byte
     * keyboard endpoint is never taken. */
    CHECK(!Select(rule, 0, intel_interface0, 25, NULL));

    /* The control endpoint 0x02 is never taken, even ahead of 0x01 and even
     * by a rule that names no OUT address. */
    {
        static const unsigned char reordered[] = {
            9, 4, 0, 1, 3, 0, 0, 0, 0,
            7, 5, 0x02, 0, 8, 0, 0,
            7, 5, 0x81, 3, 27, 0, 1,
            7, 5, 0x01, 3, 25, 0, 1,
        };
        SDL_VendorUSBRule any = *rule;
        any.out_endpoint = 0;
        any.out_size = 0;
        CHECK(Select(rule, 0, reordered, sizeof(reordered), &selection));
        CHECK(selection.out.address == 0x01);
        CHECK(Select(&any, 0, reordered, sizeof(reordered), &selection));
        CHECK(selection.out.address == 0x01);
    }
}

static void TestOtherSelections(void)
{
    SDL_VendorUSBSelection selection;
    SDL_VendorUSBRule rule;

    memset(&selection, 0, sizeof(selection));

    /* Constructed: the GameCube adapter's one alternate. The first interrupt
     * IN and OUT are taken, as the generic path takes them. */
    {
        static const unsigned char gamecube[] = {
            9, 4, 0, 0, 2, 3, 0, 0, 0,
            9, 0x21, 0x10, 0x01, 0, 1, 0x22, 214, 0,
            7, 5, 0x81, 3, 37, 0, 8,
            7, 5, 0x02, 3, 5, 0, 8,
        };
        const SDL_VendorUSBRule *gc = SDL_VendorUSB_FindRule(USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_GAMECUBE_ADAPTER, 0, 3, 0, 0);
        CHECK(gc != NULL);
        CHECK(gc && Select(gc, 0, gamecube, sizeof(gamecube), &selection));
        CHECK(selection.alternate == 0);
        CHECK(selection.in.address == 0x81 && selection.in.max_packet_size == 37);
        CHECK(selection.out.address == 0x02 && selection.out.max_packet_size == 5);
        /* The read length is the descriptor's wMaxPacketSize, as the generic
         * path reads it. */
        CHECK(selection.in.read_size == 37);
    }

    /* Constructed: the Big Button receiver. No source records its endpoints,
     * so only the first interrupt IN is required and OUT is optional. */
    {
        static const unsigned char bigbutton[] = {
            9, 4, 0, 0, 1, 0xFF, 0x5D, 0x04, 0,
            7, 5, 0x81, 3, 32, 0, 4,
        };
        const SDL_VendorUSBRule *bb = SDL_VendorUSB_FindRule(USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER, 0, 0xFF, 0x5D, 0x04);
        CHECK(bb && Select(bb, 0, bigbutton, sizeof(bigbutton), &selection));
        CHECK(selection.in.address == 0x81 && selection.out.address == 0);
    }

    /* Bulk endpoints are taken by type, and control and isochronous ones
     * never are. A rule may name the endpoints it uses. */
    {
        static const unsigned char bulk[] = {
            9, 4, 2, 0, 5, 0xFF, 0, 0, 0,
            7, 5, 0x84, 1, 64, 0, 1,  /* isochronous IN */
            7, 5, 0x05, 0, 8, 0, 0,   /* control */
            7, 5, 0x82, 2, 64, 0, 0,  /* bulk IN */
            7, 5, 0x03, 2, 64, 0, 0,  /* bulk OUT */
            9, 5, 0x86, 3, 16, 0, 4, 0, 0, /* interrupt IN, audio-sized */
        };
        memset(&rule, 0, sizeof(rule));
        CHECK(Select(&rule, 2, bulk, sizeof(bulk), &selection));
        CHECK(selection.in.address == 0x82 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_BULK &&
              selection.in.max_packet_size == 64 && selection.in.read_size == 64);
        CHECK(selection.out.address == 0x03 && selection.out.transfer == SDL_VENDORUSB_TRANSFER_BULK);
        rule.in_endpoint = 0x86;
        CHECK(Select(&rule, 2, bulk, sizeof(bulk), &selection));
        CHECK(selection.in.address == 0x86 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT);
        rule.in_endpoint = 0x84; /* isochronous: never */
        CHECK(!Select(&rule, 2, bulk, sizeof(bulk), &selection));
        rule.in_endpoint = 0;
        rule.out_endpoint = 0x05; /* control: never */
        CHECK(!Select(&rule, 2, bulk, sizeof(bulk), &selection));
        /* wMaxPacketSize bits 11 and 12 carry the high-speed multiplier, so
         * 0x1240 is a 576-byte packet. A rule's size matches the packet
         * size. The read length keeps the whole field, as the generic path
         * does. */
        {
            unsigned char high[sizeof(bulk)];
            memcpy(high, bulk, sizeof(high));
            high[28] = 0x12;
            memset(&rule, 0, sizeof(rule));
            CHECK(Select(&rule, 2, high, sizeof(high), &selection));
            CHECK(selection.in.address == 0x82 && selection.in.max_packet_size == 0x0240);
            CHECK(selection.in.read_size == 0x1240);
            rule.in_size = 0x0240;
            CHECK(Select(&rule, 2, high, sizeof(high), &selection));
            CHECK(selection.in.address == 0x82);
            rule.in_size = 0x1240;
            CHECK(!Select(&rule, 2, high, sizeof(high), &selection));
        }
    }

    /* Constructed from dji-rc-joystick's endpoints: the DJI RC's bulk
     * interface 1, bulk OUT 0x02 and bulk IN 0x83 of 512 bytes at high
     * speed. */
    {
        static const unsigned char rm330[] = {
            9, 4, 1, 0, 2, 0xFF, 0x43, 0x01, 0,
            7, 5, 0x02, 2, 0x00, 0x02, 0,
            7, 5, 0x83, 2, 0x00, 0x02, 0,
        };
        const SDL_VendorUSBRule *dji = SDL_VendorUSB_FindRule(USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 1, 0xFF, 0x43, 0x01);

        CHECK(dji && Select(dji, 1, rm330, sizeof(rm330), &selection));
        CHECK(selection.in.address == 0x83 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_BULK && selection.in.read_size == 512);
        CHECK(selection.out.address == 0x02 && selection.out.transfer == SDL_VENDORUSB_TRANSFER_BULK);
        /* Without its IN endpoint the interface cannot be used */
        CHECK(dji && !SDL_VendorUSB_SelectEndpoints(dji, 1, rm330, 16, &selection));
    }

    /* Null and empty inputs fail cleanly. */
    memset(&rule, 0, sizeof(rule));
    CHECK(!SDL_VendorUSB_SelectEndpoints(NULL, 0, intel_interface0, sizeof(intel_interface0), &selection));
    CHECK(!SDL_VendorUSB_SelectEndpoints(&rule, 0, intel_interface0, sizeof(intel_interface0), NULL));
    CHECK(!SDL_VendorUSB_SelectEndpoints(&rule, 0, NULL, 4, &selection));
    CHECK(!SDL_VendorUSB_SelectEndpoints(&rule, 0, NULL, 0, &selection));
}

/* The routing rules as they stood at 5df5eff539 (src/hidapi/SDL_hidapi.c
 * RequiresLibUSB and SDL_HIDAPI_ShouldIgnoreDevice, and
 * should_enumerate_interface in src/hidapi/libusb/hid.c), restated so the new
 * rules can be compared with them case by case. */
static bool OldRequired(SDL_VendorUSBPlatform platform, uint16_t vendor, uint16_t product, bool xbox)
{
    static const uint16_t list[][2] = {
        { USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_GAMECUBE_ADAPTER },
        { USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER },
        { USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT },
        { USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT },
        { USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_PRO },
        { USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER },
    };
    size_t i;
    for (i = 0; i < sizeof(list) / sizeof(list[0]); ++i) {
        if (list[i][0] == vendor && list[i][1] == product) {
            return true;
        }
    }
    return platform == SDL_VENDORUSB_PLATFORM_MACOS && xbox;
}

static bool OldIgnore(const SDL_VendorUSBRouting *r)
{
    const bool gamecube = (r->vendor == USB_VENDOR_NINTENDO && r->product == USB_PRODUCT_NINTENDO_GAMECUBE_ADAPTER);
    if (r->libusb) {
        if (r->whitelist && !OldRequired(r->platform, r->vendor, r->product, r->xbox)) {
            return true;
        }
        if (!r->gamecube && gamecube) {
            return true;
        }
        if (r->platform == SDL_VENDORUSB_PLATFORM_WINDOWS &&
            OldRequired(r->platform, r->vendor, r->product, r->xbox) && !gamecube) {
            return true;
        }
        return false;
    }
    if (r->platform == SDL_VENDORUSB_PLATFORM_WINDOWS) {
        return gamecube;
    }
    return OldRequired(r->platform, r->vendor, r->product, false);
}

typedef struct Device
{
    const char *name;
    uint16_t vendor, product;
    uint8_t number, cls, subclass, protocol;
    bool xbox; /* What is_xbox360 or is_xboxone in hid.c computes for this interface */
} Device;

/* Recorded interfaces where a source records them: the Intel base station
 * (JoypadOS), the Big Button receiver's class triple (SDL, xbox360bb), the
 * GameCube adapter's class (linux-hardware.org), the wired Xbox 360 pad's
 * interface 0 (linux-hardware.org). The rest are the usual layouts. */
static const Device devices[] = {
    { "GameCube adapter", USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_GAMECUBE_ADAPTER, 0, 3, 0, 0, false },
    { "Big Button receiver", USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER, 0, 0xFF, 0x5D, 0x04, true },
    { "Big Button ID, wired pad layout", USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER, 0, 0xFF, 0x5D, 0x01, true },
    { "Intel interface 0", USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES, 0, 3, 1, 1, false },
    { "Intel interface 1", USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES, 1, 3, 1, 2, false },
    { "DualShock 4", 0x054C, 0x09CC, 3, 3, 0, 0, false },
    { "Switch 2 Pro HID", USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_PRO, 0, 3, 0, 0, false },
    { "Switch 2 Pro vendor", USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_PRO, 1, 0xFF, 0, 0, false },
    { "Switch 2 GameCube HID", USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER, 0, 3, 0, 0, false },
    { "Xbox 360 wired pad", USB_VENDOR_MICROSOFT, 0x028E, 0, 0xFF, 0x5D, 0x01, true },
    { "Xbox 360 wireless receiver", USB_VENDOR_MICROSOFT, 0x0719, 0, 0xFF, 0x5D, 0x81, true },
    { "Xbox One pad", USB_VENDOR_MICROSOFT, 0x02EA, 0, 0xFF, 0x47, 0xD0, true },
    { "Vendor interface, no rule", 0x1234, 0x5678, 0, 0xFF, 0, 0, false },
    { "HID keyboard", 0x046D, 0xC31C, 0, 3, 1, 1, false },
    { "Gametrak", USB_VENDOR_IN2GAMES, USB_PRODUCT_IN2GAMES_GAMETRAK, 0, 3, 0, 0, false },
    { "DJI RC MTP", USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 0, 0x06, 0x01, 0x01, false },
    { "DJI RC bulk", USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 1, 0xFF, 0x43, 0x01, false },
    { "DJI RC ADB", USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 2, 0xFF, 0x42, 0x01, false },
    { "WingMan Formula Force", USB_VENDOR_LOGITECH, 0xC291, 0, 0xFF, 0x00, 0x00, false },
    { "Guillemot Force Feedback Racing Wheel", USB_VENDOR_GUILLEMOT, 0x0004, 0, 0x03, 0x00, 0x00, false },
};

static bool NewLibUSBEnumerates(const SDL_VendorUSBRouting *r, const Device *d, bool opened)
{
    return !SDL_VendorUSB_Ignore(r) &&
           SDL_VendorUSB_IsCandidate(r->platform, d->vendor, d->product, d->number, d->cls, d->subclass, d->protocol, d->xbox) &&
           !SDL_VendorUSB_SkipUnopened(r->platform, d->xbox, opened);
}

static bool OldLibUSBEnumerates(const SDL_VendorUSBRouting *r, const Device *d)
{
    return !OldIgnore(r) && (d->xbox || d->cls == 3);
}

static const Device *Find(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof(devices) / sizeof(devices[0]); ++i) {
        if (strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    printf("FAIL: no device %s\n", name);
    ++failures;
    return &devices[0];
}

static SDL_VendorUSBRouting Route(SDL_VendorUSBPlatform platform, bool libusb, const Device *d)
{
    SDL_VendorUSBRouting r;
    r.platform = platform;
    r.libusb = libusb;
    r.whitelist = true;
    r.gamecube = true;
    r.vendor = d->vendor;
    r.product = d->product;
    r.xbox = libusb ? d->xbox : false;
    /* As should_enumerate_interface in hid.c computes it */
    r.vendor_interface = libusb ? SDL_VendorUSB_RuleApplies(SDL_VendorUSB_FindRule(d->vendor, d->product, d->number, d->cls, d->subclass, d->protocol), platform) : false;
    return r;
}

static bool OnLibUSB(SDL_VendorUSBPlatform platform, const char *name, bool opened)
{
    const Device *d = Find(name);
    SDL_VendorUSBRouting r = Route(platform, true, d);
    return NewLibUSBEnumerates(&r, d, opened);
}

static bool PlatformIgnores(SDL_VendorUSBPlatform platform, const char *name)
{
    const Device *d = Find(name);
    SDL_VendorUSBRouting r = Route(platform, false, d);
    return SDL_VendorUSB_Ignore(&r);
}

static void TestRoutingTable(void)
{
    const SDL_VendorUSBPlatform W = SDL_VENDORUSB_PLATFORM_WINDOWS;
    const SDL_VendorUSBPlatform L = SDL_VENDORUSB_PLATFORM_OTHER;
    const SDL_VendorUSBPlatform M = SDL_VENDORUSB_PLATFORM_MACOS;

    /* Every device of this part that needs the path reaches libusb on
     * Windows once WinUSB is bound, and the platform backend leaves it. */
    CHECK(OnLibUSB(W, "GameCube adapter", true));
    CHECK(PlatformIgnores(W, "GameCube adapter"));
    CHECK(OnLibUSB(W, "Big Button receiver", true));
    CHECK(PlatformIgnores(W, "Big Button receiver"));
    CHECK(OnLibUSB(W, "Intel interface 0", true));
    CHECK(!OnLibUSB(W, "Intel interface 1", true));
    CHECK(PlatformIgnores(W, "Intel interface 0"));
    CHECK(PlatformIgnores(W, "Intel interface 1"));

    /* The Big Button receiver with no driver bound cannot be opened. */
    CHECK(!OnLibUSB(W, "Big Button receiver", false));
    /* A device with the receiver's ID but a wired pad's interface is not the
     * receiver the rule describes. */
    CHECK(!OnLibUSB(W, "Big Button ID, wired pad layout", true));

    /* A HID gamepad and the Switch 2 controllers stay on the platform
     * backend. */
    CHECK(!OnLibUSB(W, "DualShock 4", true));
    CHECK(!PlatformIgnores(W, "DualShock 4"));
    CHECK(!OnLibUSB(W, "Switch 2 Pro HID", true));
    CHECK(!OnLibUSB(W, "Switch 2 Pro vendor", true));
    CHECK(!PlatformIgnores(W, "Switch 2 Pro HID"));
    CHECK(!OnLibUSB(W, "Switch 2 GameCube HID", true));
    CHECK(!PlatformIgnores(W, "Switch 2 GameCube HID"));

    /* A wired Xbox 360 pad that xusb22 holds stays off libusb. Bound to
     * WinUSB it reaches libusb. The same goes for the wireless receiver and
     * an Xbox One pad held by the GIP driver. */
    CHECK(!OnLibUSB(W, "Xbox 360 wired pad", false));
    CHECK(OnLibUSB(W, "Xbox 360 wired pad", true));
    CHECK(!OnLibUSB(W, "Xbox 360 wireless receiver", false));
    CHECK(OnLibUSB(W, "Xbox 360 wireless receiver", true));
    CHECK(!OnLibUSB(W, "Xbox One pad", false));
    CHECK(OnLibUSB(W, "Xbox One pad", true));

    /* Nothing else changes on other platforms. */
    CHECK(OnLibUSB(L, "GameCube adapter", true));
    CHECK(PlatformIgnores(L, "GameCube adapter"));
    CHECK(OnLibUSB(L, "Switch 2 Pro HID", true));
    CHECK(PlatformIgnores(L, "Switch 2 Pro HID"));
    CHECK(!OnLibUSB(L, "Xbox 360 wired pad", true));
    CHECK(OnLibUSB(M, "Xbox 360 wired pad", true));
    CHECK(!OnLibUSB(L, "DualShock 4", true));
    CHECK(!PlatformIgnores(L, "DualShock 4"));

    /* The Gametrak's writes need libusb on Windows only. Elsewhere the
     * platform backend keeps it and libusb leaves it. */
    CHECK(OnLibUSB(W, "Gametrak", true));
    CHECK(PlatformIgnores(W, "Gametrak"));
    CHECK(!OnLibUSB(L, "Gametrak", true));
    CHECK(!PlatformIgnores(L, "Gametrak"));
    CHECK(!OnLibUSB(M, "Gametrak", true));
    CHECK(!PlatformIgnores(M, "Gametrak"));

    /* Part 6: the DJI RC's bulk interface reaches libusb on every platform,
     * its MTP and ADB interfaces never do. */
    CHECK(OnLibUSB(W, "DJI RC bulk", true));
    CHECK(!OnLibUSB(W, "DJI RC MTP", true));
    CHECK(!OnLibUSB(W, "DJI RC ADB", true));
    CHECK(PlatformIgnores(W, "DJI RC bulk"));
    CHECK(OnLibUSB(L, "DJI RC bulk", true));
    CHECK(OnLibUSB(M, "DJI RC bulk", true));
    CHECK(!OnLibUSB(L, "DJI RC ADB", true));

    /* Part 8: the I-Force devices reach libusb on Windows once WinUSB is
       bound, and the platform backend leaves them. Elsewhere nothing changes,
       so Linux's own driver keeps them. */
    CHECK(OnLibUSB(W, "WingMan Formula Force", true));
    CHECK(OnLibUSB(W, "Guillemot Force Feedback Racing Wheel", true));
    CHECK(PlatformIgnores(W, "WingMan Formula Force"));
    CHECK(PlatformIgnores(W, "Guillemot Force Feedback Racing Wheel"));
    CHECK(!OnLibUSB(L, "WingMan Formula Force", true));
    CHECK(!OnLibUSB(M, "WingMan Formula Force", true));
    CHECK(!OnLibUSB(L, "Guillemot Force Feedback Racing Wheel", true));
    CHECK(!PlatformIgnores(L, "WingMan Formula Force"));
    CHECK(!PlatformIgnores(M, "Guillemot Force Feedback Racing Wheel"));
    /* Without the whitelist a vendor-class interface still stays off libusb there */
    {
        const Device *d = Find("WingMan Formula Force");
        SDL_VendorUSBRouting r = Route(L, true, d);

        r.whitelist = false;
        CHECK(!NewLibUSBEnumerates(&r, d, true));
        r = Route(M, true, d);
        r.whitelist = false;
        CHECK(!NewLibUSBEnumerates(&r, d, true));
    }

    /* The GameCube hint still turns the adapter off. */
    {
        const Device *d = Find("GameCube adapter");
        SDL_VendorUSBRouting r = Route(W, true, d);
        r.gamecube = false;
        CHECK(!NewLibUSBEnumerates(&r, d, true));
    }
}

/* The documented differences from the old rules. Every case where the new
 * rules differ from the old ones must be one of these. */
static bool ExpectedChange(const SDL_VendorUSBRouting *r, const Device *d)
{
    const bool intel = (d->vendor == USB_VENDOR_INTEL && d->product == USB_PRODUCT_INTEL_WIRELESS_SERIES);
    const bool bigbutton = (d->vendor == USB_VENDOR_MICROSOFT && d->product == USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER);
    const bool gametrak = (d->vendor == USB_VENDOR_IN2GAMES && d->product == USB_PRODUCT_IN2GAMES_GAMETRAK);
    const bool dji = (d->vendor == USB_VENDOR_DJI && d->product == USB_PRODUCT_DJI_RC_RM330);
    const bool iforce = ((d->vendor == USB_VENDOR_LOGITECH && d->product == 0xC291) ||
                         (d->vendor == USB_VENDOR_GUILLEMOT && d->product == 0x0004));

    if (intel || dji) {
        return true; /* A new member of the path */
    }
    if ((gametrak || iforce) && r->platform == SDL_VENDORUSB_PLATFORM_WINDOWS) {
        return true; /* Parts 7 and 8: on the path on Windows only */
    }
    if (bigbutton && d->protocol != 0x04) {
        return true; /* Only the receiver's own interface is enumerated */
    }
    if (r->platform == SDL_VENDORUSB_PLATFORM_WINDOWS) {
        if (bigbutton) {
            return true; /* Defect 1: reachable on Windows */
        }
        if (r->libusb && d->xbox) {
            return true; /* Item 7: an Xbox interface is enumerated when it opens, and skipped when it does not */
        }
    }
    return false;
}

static void TestAgainstOldRules(void)
{
    const SDL_VendorUSBPlatform platforms[] = {
        SDL_VENDORUSB_PLATFORM_OTHER, SDL_VENDORUSB_PLATFORM_WINDOWS, SDL_VENDORUSB_PLATFORM_MACOS
    };
    size_t p, i;
    int flags, changed = 0, unchanged = 0;

    for (p = 0; p < sizeof(platforms) / sizeof(platforms[0]); ++p) {
        for (i = 0; i < sizeof(devices) / sizeof(devices[0]); ++i) {
            const Device *d = &devices[i];
            for (flags = 0; flags < 16; ++flags) {
                SDL_VendorUSBRouting r = Route(platforms[p], (flags & 1) != 0, d);
                const bool opened = (flags & 8) != 0;
                bool old_result, new_result;

                r.whitelist = (flags & 2) != 0;
                r.gamecube = (flags & 4) != 0;
                if (r.libusb) {
                    old_result = OldLibUSBEnumerates(&r, d);
                    new_result = NewLibUSBEnumerates(&r, d, opened);
                } else {
                    old_result = OldIgnore(&r);
                    new_result = SDL_VendorUSB_Ignore(&r);
                }
                if (old_result != new_result) {
                    ++changed;
                    if (!ExpectedChange(&r, d)) {
                        ++failures;
                        printf("FAIL: unexpected change: platform %d libusb %d whitelist %d gamecube %d opened %d %s: %d -> %d\n",
                               (int)platforms[p], r.libusb, r.whitelist, r.gamecube, opened, d->name, old_result, new_result);
                    }
                } else {
                    ++unchanged;
                }
                ++checks;
            }
        }
    }
    printf("old rules against new: %d cases changed, all documented, %d unchanged\n", changed, unchanged);
    CHECK(changed > 0 && unchanged > 0);
}

int main(void)
{
    TestRules();
    TestIntelDescriptors();
    TestOtherSelections();
    TestRoutingTable();
    TestAgainstOldRules();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
