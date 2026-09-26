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

/* Compares two selections field by field. The structure has a padding byte
 * after the alternate, and a structure copy leaves that byte unspecified
 * (C11 6.2.6.1p6), so memcmp could differ on equal selections. */
static bool SameSelection(const SDL_VendorUSBSelection *a, const SDL_VendorUSBSelection *b)
{
    return a->alternate == b->alternate &&
           a->in.address == b->in.address && a->in.transfer == b->in.transfer &&
           a->in.max_packet_size == b->in.max_packet_size && a->in.read_size == b->in.read_size &&
           a->out.address == b->out.address && a->out.transfer == b->out.transfer &&
           a->out.max_packet_size == b->out.max_packet_size && a->out.read_size == b->out.read_size;
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
        CHECK(SameSelection(&exact, &stale));
    }
    if (a && out) {
        *out = exact;
    }
    return a;
}

/* Runs the walk on every prefix of a descriptor set, as an exact-size heap
 * copy and again inside the whole set, whose bytes past the prefix would
 * complete a truncated descriptor. Only the listed prefix lengths select, and
 * both runs select the same endpoints. */
static void CheckPrefixes(const SDL_VendorUSBRule *rule, uint8_t interface_number,
                          const unsigned char *bytes, size_t length,
                          const size_t *selecting, size_t count)
{
    size_t n, i;

    for (n = 0; n <= length; ++n) {
        SDL_VendorUSBSelection exact, stale;
        unsigned char *copy = (unsigned char *)malloc(n ? n : 1);
        bool expected = false;
        bool a, b;

        if (!copy) {
            ++failures;
            printf("FAIL: test buffer\n");
            return;
        }
        for (i = 0; i < count; ++i) {
            if (selecting[i] == n) {
                expected = true;
            }
        }
        memcpy(copy, bytes, n);
        memset(&exact, 0xEE, sizeof(exact));
        a = SDL_VendorUSB_SelectEndpoints(rule, interface_number, copy, n, &exact);
        free(copy);
        memset(&stale, 0xEE, sizeof(stale));
        b = SDL_VendorUSB_SelectEndpoints(rule, interface_number, bytes, n, &stale);
        if (a != expected || b != expected) {
            printf("  prefix of %u bytes: %d and %d, expected %d\n", (unsigned int)n, a, b, expected);
        }
        CHECK(a == expected && b == expected);
        if (a && b) {
            CHECK(SameSelection(&exact, &stale));
        }
    }
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
    /* The GameCube adapter's and the Intel base station's rules serve every
       platform */
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

    /* Part 9: the GunCon 2, interface 0 whatever its class, report numbers
       stripped, on every platform. The GunCon 3 has no rule. */
    CHECK(USB_VENDOR_NAMCO == 0x0B9A && USB_PRODUCT_NAMCO_GUNCON2 == 0x016A && USB_PRODUCT_NAMCO_GUNCON3 == 0x0800);
    rule = SDL_VendorUSB_FindRule(USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_GUNCON2, 0, 0xFF, 0x6A, 0x00);
    CHECK(rule && rule->flags == 0 && rule->interface_number == 0 && rule->alternate == 0);
    CHECK(rule && rule->in_endpoint == 0 && rule->out_endpoint == 0 && rule->in_size == 0 && rule->out_size == 0);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_GUNCON2, 0, 0x03, 0x00, 0x00) == rule);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_GUNCON2, 1, 0xFF, 0x6A, 0x00) == NULL);
    CHECK(SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_WINDOWS) && SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_OTHER) &&
          SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_MACOS));
    CHECK(SDL_VendorUSB_RequiresLibUSB(SDL_VENDORUSB_PLATFORM_OTHER, USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_GUNCON2, false, false));
    CHECK(!SDL_VendorUSB_IsVendorDevice(USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_GUNCON3));
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_GUNCON3, 0, 0xFF, 0x00, 0x00) == NULL);
    CHECK(!SDL_VendorUSB_RequiresLibUSB(SDL_VENDORUSB_PLATFORM_WINDOWS, USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_GUNCON3, false, false));

    /* Part 10: the five train controllers, interface 0 whatever its class,
       on every platform */
    {
        static const uint16_t trains[][2] = {
            { USB_VENDOR_TAITO, 0x0004 }, { USB_VENDOR_TAITO, 0x0005 }, { USB_VENDOR_TAITO, 0x0007 },
            { USB_VENDOR_TAITO, 0x0101 }, { USB_VENDOR_TRAIN_MASCON, 0x77A7 },
        };
        size_t i;

        CHECK(USB_VENDOR_TAITO == 0x0AE4 && USB_VENDOR_TRAIN_MASCON == 0x1C06);
        for (i = 0; i < sizeof(trains) / sizeof(trains[0]); ++i) {
            rule = SDL_VendorUSB_FindRule(trains[i][0], trains[i][1], 0, 0x03, 0x00, 0x00);
            CHECK(rule && rule->flags == 0 && rule->interface_number == 0 && rule->alternate == 0);
            CHECK(rule && rule->in_endpoint == 0 && rule->out_endpoint == 0 && rule->in_size == 0 && rule->out_size == 0);
            CHECK(SDL_VendorUSB_FindRule(trains[i][0], trains[i][1], 0, 0x00, 0x00, 0x00) == rule);
            CHECK(SDL_VendorUSB_FindRule(trains[i][0], trains[i][1], 1, 0x03, 0x00, 0x00) == NULL);
            CHECK(SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_OTHER) && SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_MACOS));
            CHECK(SDL_VendorUSB_RequiresLibUSB(SDL_VENDORUSB_PLATFORM_OTHER, trains[i][0], trains[i][1], false, false));
        }
        /* Other Taito IDs, including the HID DGOC-44U, stay off the path */
        CHECK(!SDL_VendorUSB_IsVendorDevice(USB_VENDOR_TAITO, 0x0003));
        CHECK(!SDL_VendorUSB_IsVendorDevice(USB_VENDOR_TAITO, 0x0006));
        CHECK(!SDL_VendorUSB_IsVendorDevice(USB_VENDOR_TAITO, 0x0008));
    }

    /* Part 14: the Namco USIO and the H050 USJ(C), interface 0 whatever its
       class, bulk IN 0x82 and bulk OUT 0x01 named, commands unchanged, on
       Windows only */
    {
        static const uint16_t usio[] = { 0x0910, 0x0900 };
        size_t i;

        CHECK(USB_PRODUCT_NAMCO_USIO == 0x0910 && USB_PRODUCT_NAMCO_H050_USJC == 0x0900);
        for (i = 0; i < sizeof(usio) / sizeof(usio[0]); ++i) {
            rule = SDL_VendorUSB_FindRule(USB_VENDOR_NAMCO, usio[i], 0, 0x00, 0x00, 0x00);
            CHECK(rule && rule->flags == (SDL_VENDORUSB_RAW_OUTPUT | SDL_VENDORUSB_WINDOWS_ONLY) && rule->interface_number == 0 && rule->alternate == 0);
            CHECK(rule && rule->in_endpoint == 0x82 && rule->out_endpoint == 0x01 && rule->in_size == 0 && rule->out_size == 0);
            CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_NAMCO, usio[i], 0, 0xFF, 0x00, 0x00) == rule);
            CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_NAMCO, usio[i], 1, 0x00, 0x00, 0x00) == NULL);
            CHECK(SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_WINDOWS));
            CHECK(!SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_OTHER) && !SDL_VendorUSB_RuleApplies(rule, SDL_VENDORUSB_PLATFORM_MACOS));
            /* Only the interface's rule takes it to libusb, and elsewhere none applies */
            CHECK(!SDL_VendorUSB_RequiresLibUSB(SDL_VENDORUSB_PLATFORM_OTHER, USB_VENDOR_NAMCO, usio[i], false, false));
            CHECK(!SDL_VendorUSB_RequiresLibUSB(SDL_VENDORUSB_PLATFORM_MACOS, USB_VENDOR_NAMCO, usio[i], false, false));
            CHECK(SDL_VendorUSB_IsVendorDevice(USB_VENDOR_NAMCO, usio[i]));
        }
        /* RPCS3 names the range 0900 to 0910 (sys_usbd.cpp:270-271). Only
           its two ends are boards any source describes. */
        CHECK(!SDL_VendorUSB_IsVendorDevice(USB_VENDOR_NAMCO, 0x0901));
        CHECK(!SDL_VendorUSB_IsVendorDevice(USB_VENDOR_NAMCO, 0x090F));
        CHECK(!SDL_VendorUSB_IsVendorDevice(USB_VENDOR_NAMCO, 0x0911));
    }
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

    /* Constructed from the emulators' descriptors for the Namco USIO.
     * RPCS3: interface 0, class 00, bulk OUT 0x01 and bulk IN 0x82 of 64
     * bytes, then interrupt IN 0x83 of 8 bytes (usio.cpp:93-122).
     * TaikoZucchini lists 0x82 before 0x01, class FF and no 0x83
     * (bpreader_hook.c:118-125). Both select the two bulk endpoints. */
    {
        static const unsigned char rpcs3[] = {
            9, 4, 0, 0, 3, 0x00, 0x00, 0x00, 0,
            7, 5, 0x01, 2, 64, 0, 0,
            7, 5, 0x82, 2, 64, 0, 0,
            7, 5, 0x83, 3, 8, 0, 16,
        };
        static const unsigned char zucchini[] = {
            9, 4, 0, 0, 2, 0xFF, 0x00, 0x00, 0,
            7, 5, 0x82, 2, 64, 0, 0,
            7, 5, 0x01, 2, 64, 0, 0,
        };
        static const unsigned char status_first[] = {
            9, 4, 0, 0, 3, 0x00, 0x00, 0x00, 0,
            7, 5, 0x83, 3, 8, 0, 16,
            7, 5, 0x01, 2, 64, 0, 0,
            7, 5, 0x82, 2, 64, 0, 0,
        };
        const SDL_VendorUSBRule *usio = SDL_VendorUSB_FindRule(USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_USIO, 0, 0x00, 0x00, 0x00);

        CHECK(usio && Select(usio, 0, rpcs3, sizeof(rpcs3), &selection));
        CHECK(selection.alternate == 0);
        CHECK(selection.in.address == 0x82 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_BULK && selection.in.read_size == 64);
        CHECK(selection.out.address == 0x01 && selection.out.transfer == SDL_VENDORUSB_TRANSFER_BULK);
        CHECK(usio && Select(usio, 0, zucchini, sizeof(zucchini), &selection));
        CHECK(selection.in.address == 0x82 && selection.out.address == 0x01);
        /* The interrupt endpoint is never taken, even ahead of 0x82 */
        CHECK(usio && Select(usio, 0, status_first, sizeof(status_first), &selection));
        CHECK(selection.in.address == 0x82 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_BULK);
        /* Without 0x82 or without 0x01 the interface cannot be used */
        CHECK(usio && !Select(usio, 0, rpcs3, 16, &selection));
        CHECK(usio && !Select(usio, 0, zucchini, 16, &selection));
    }

    /* Null and empty inputs fail cleanly. */
    memset(&rule, 0, sizeof(rule));
    CHECK(!SDL_VendorUSB_SelectEndpoints(NULL, 0, intel_interface0, sizeof(intel_interface0), &selection));
    CHECK(!SDL_VendorUSB_SelectEndpoints(&rule, 0, intel_interface0, sizeof(intel_interface0), NULL));
    CHECK(!SDL_VendorUSB_SelectEndpoints(&rule, 0, NULL, 4, &selection));
    CHECK(!SDL_VendorUSB_SelectEndpoints(&rule, 0, NULL, 0, &selection));
}

/* Constructed from p4io-mdxfdrv: the Konami P4IO's interface 0 holds exactly
 * three endpoints, bulk OUT, bulk IN and interrupt IN in that order
 * (src/p4io.c:40-76), and its input comes from the interrupt endpoint in
 * reads of 16 bytes (src/p4io.h:7, src/p4io.c:114-121). No source records the
 * addresses, the class or the bulk packet sizes, so those are placeholders. */
static const unsigned char p4io_interface0[] = {
    9, 4, 0, 0, 3, 0xFF, 0, 0, 0,
    7, 5, 0x01, 2, 64, 0, 0,  /* bulk OUT */
    7, 5, 0x82, 2, 64, 0, 0,  /* bulk IN */
    7, 5, 0x83, 3, 16, 0, 1,  /* interrupt IN, 16 bytes */
};

/* Constructed from linuxtrack: the TrackIR 2 and 3 use interface 0 with bulk
 * OUT 0x02 and bulk IN 0x82 (src/tir_hw.c:19-23, :1086-1089) and read 16384
 * bytes at a time (src/tir_hw.h:12, src/tir_img.c:554). No source records the
 * packet size or the endpoint order, so 64 bytes and this order are
 * placeholders. */
static const unsigned char trackir_interface0[] = {
    9, 4, 0, 0, 2, 0xFF, 0, 0, 0,
    7, 5, 0x02, 2, 64, 0, 0,  /* bulk OUT */
    7, 5, 0x82, 2, 64, 0, 0,  /* bulk IN */
};

/* The DJI RC's bulk interface 1, bulk IN 0x83 of 512 bytes at high speed, as
 * TestOtherSelections builds it. */
static const unsigned char rm330_interface1[] = {
    9, 4, 1, 0, 2, 0xFF, 0x43, 0x01, 0,
    7, 5, 0x02, 2, 0x00, 0x02, 0,
    7, 5, 0x83, 2, 0x00, 0x02, 0,
};

/* Part 14: SDL_VENDORUSB_IN_INTERRUPT, for input that comes on an interrupt
 * endpoint behind a bulk IN endpoint. */
static void TestInterruptRule(void)
{
    SDL_VendorUSBSelection selection;
    SDL_VendorUSBRule rule;

    /* A bit of its own */
    CHECK(SDL_VENDORUSB_IN_INTERRUPT == 0x10);
    CHECK((SDL_VENDORUSB_IN_INTERRUPT & (SDL_VENDORUSB_MATCH_CLASS | SDL_VENDORUSB_RAW_OUTPUT |
                                         SDL_VENDORUSB_WINDOWS_ONLY | SDL_VENDORUSB_ANY_PROTOCOL)) == 0);

    /* The P4IO's order. Without the flag the bulk IN endpoint is taken, as
     * before, and with it the interrupt IN endpoint behind it. The bulk OUT
     * endpoint is taken either way. */
    memset(&rule, 0, sizeof(rule));
    memset(&selection, 0, sizeof(selection));
    CHECK(Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection));
    CHECK(selection.in.address == 0x82 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_BULK &&
          selection.in.read_size == 64);
    CHECK(selection.out.address == 0x01 && selection.out.transfer == SDL_VENDORUSB_TRANSFER_BULK);
    rule.flags = SDL_VENDORUSB_IN_INTERRUPT;
    memset(&selection, 0, sizeof(selection));
    CHECK(Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection));
    CHECK(selection.alternate == 0);
    CHECK(selection.in.address == 0x83 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT &&
          selection.in.max_packet_size == 16 && selection.in.read_size == 16);
    CHECK(selection.out.address == 0x01 && selection.out.transfer == SDL_VENDORUSB_TRANSFER_BULK &&
          selection.out.max_packet_size == 64 && selection.out.read_size == 64);
    /* The flag counts with other flags beside it */
    rule.flags = SDL_VENDORUSB_IN_INTERRUPT | SDL_VENDORUSB_RAW_OUTPUT | SDL_VENDORUSB_WINDOWS_ONLY;
    CHECK(Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection));
    CHECK(selection.in.address == 0x83);

    /* Every prefix, with the rest of the set as stale bytes. Without the flag
     * the walk selects once the bulk IN endpoint is whole (23 bytes), with it
     * only once the interrupt endpoint is (30 bytes). */
    {
        static const size_t bulk_whole[] = { 23, sizeof(p4io_interface0) };
        static const size_t interrupt_whole[] = { sizeof(p4io_interface0) };

        rule.flags = 0;
        CheckPrefixes(&rule, 0, p4io_interface0, sizeof(p4io_interface0), bulk_whole, 2);
        rule.flags = SDL_VENDORUSB_IN_INTERRUPT;
        CheckPrefixes(&rule, 0, p4io_interface0, sizeof(p4io_interface0), interrupt_whole, 1);
    }

    /* Addresses play no part */
    {
        static const unsigned char other[] = {
            9, 4, 0, 0, 3, 0xFF, 0, 0, 0,
            7, 5, 0x02, 2, 64, 0, 0,
            7, 5, 0x81, 2, 64, 0, 0,
            7, 5, 0x84, 3, 16, 0, 4,
        };
        memset(&rule, 0, sizeof(rule));
        CHECK(Select(&rule, 0, other, sizeof(other), &selection) && selection.in.address == 0x81);
        rule.flags = SDL_VENDORUSB_IN_INTERRUPT;
        CHECK(Select(&rule, 0, other, sizeof(other), &selection) && selection.in.address == 0x84);
        CHECK(selection.out.address == 0x02);
    }

    /* An interrupt IN endpoint ahead of the bulk one is the first IN
     * endpoint either way */
    {
        static const unsigned char ahead[] = {
            9, 4, 0, 0, 3, 0xFF, 0, 0, 0,
            7, 5, 0x01, 2, 64, 0, 0,
            7, 5, 0x83, 3, 16, 0, 1,
            7, 5, 0x82, 2, 64, 0, 0,
        };
        memset(&rule, 0, sizeof(rule));
        CHECK(Select(&rule, 0, ahead, sizeof(ahead), &selection) && selection.in.address == 0x83);
        rule.flags = SDL_VENDORUSB_IN_INTERRUPT;
        CHECK(Select(&rule, 0, ahead, sizeof(ahead), &selection) && selection.in.address == 0x83);
    }

    /* A named IN endpoint must be interrupt as well. Without the flag a
     * name takes either type, as before. */
    memset(&rule, 0, sizeof(rule));
    rule.flags = SDL_VENDORUSB_IN_INTERRUPT;
    rule.in_endpoint = 0x83;
    CHECK(Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection) && selection.in.address == 0x83);
    rule.in_endpoint = 0x82;
    CHECK(!Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection));
    rule.in_endpoint = 0x85;
    CHECK(!Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection));
    rule.flags = 0;
    rule.in_endpoint = 0x82;
    CHECK(Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection) &&
          selection.in.address == 0x82 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_BULK);
    rule.in_endpoint = 0x83;
    CHECK(Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection) &&
          selection.in.address == 0x83 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT);

    /* A required size applies to the interrupt endpoint. The bulk IN
     * endpoint's 64 bytes never match under the flag. */
    memset(&rule, 0, sizeof(rule));
    rule.flags = SDL_VENDORUSB_IN_INTERRUPT;
    rule.in_size = 16;
    CHECK(Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection) && selection.in.address == 0x83);
    rule.in_size = 64;
    CHECK(!Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection));

    /* On an interface with no bulk IN endpoint the flag changes nothing */
    {
        const SDL_VendorUSBRule *intel = IntelRule();

        if (intel) {
            rule = *intel;
            rule.flags |= SDL_VENDORUSB_IN_INTERRUPT;
            CHECK(Select(&rule, 0, intel_interface0, sizeof(intel_interface0), &selection));
            CHECK(selection.alternate == 1 && selection.in.address == 0x81 &&
                  selection.in.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT && selection.in.read_size == 27);
            CHECK(selection.out.address == 0x01 && selection.out.read_size == 25);
        }
    }
    /* With no interrupt IN endpoint nothing is selected */
    {
        const SDL_VendorUSBRule *dji = SDL_VendorUSB_FindRule(USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 1, 0xFF, 0x43, 0x01);

        if (dji) {
            rule = *dji;
            CHECK(Select(&rule, 1, rm330_interface1, sizeof(rm330_interface1), &selection));
            rule.flags |= SDL_VENDORUSB_IN_INTERRUPT;
            CHECK(!Select(&rule, 1, rm330_interface1, sizeof(rm330_interface1), &selection));
            rule.in_endpoint = 0;
            CHECK(!Select(&rule, 1, rm330_interface1, sizeof(rm330_interface1), &selection));
        }
    }

    /* Only the selected alternate counts */
    {
        static const unsigned char alternates[] = {
            9, 4, 0, 0, 1, 0xFF, 0, 0, 0,
            7, 5, 0x81, 3, 8, 0, 10,  /* alternate 0: interrupt IN */
            9, 4, 0, 1, 2, 0xFF, 0, 0, 0,
            7, 5, 0x82, 2, 64, 0, 0,  /* alternate 1: bulk IN */
            7, 5, 0x83, 3, 16, 0, 1,  /* alternate 1: interrupt IN */
            9, 4, 0, 2, 1, 0xFF, 0, 0, 0,
            7, 5, 0x82, 2, 64, 0, 0,  /* alternate 2: bulk IN only */
        };
        static const size_t bulk_whole[] = { 32, 39, 48, sizeof(alternates) };
        static const size_t interrupt_whole[] = { 39, 48, sizeof(alternates) };

        memset(&rule, 0, sizeof(rule));
        rule.alternate = 1;
        CHECK(Select(&rule, 0, alternates, sizeof(alternates), &selection) &&
              selection.alternate == 1 && selection.in.address == 0x82);
        CheckPrefixes(&rule, 0, alternates, sizeof(alternates), bulk_whole, 4);
        rule.flags = SDL_VENDORUSB_IN_INTERRUPT;
        CHECK(Select(&rule, 0, alternates, sizeof(alternates), &selection) &&
              selection.alternate == 1 && selection.in.address == 0x83);
        CheckPrefixes(&rule, 0, alternates, sizeof(alternates), interrupt_whole, 3);
        rule.alternate = 2;
        CHECK(!Select(&rule, 0, alternates, sizeof(alternates), &selection));
        rule.alternate = 0;
        CHECK(Select(&rule, 0, alternates, sizeof(alternates), &selection) &&
              selection.alternate == 0 && selection.in.address == 0x81);
    }

    /* OUT endpoints are taken as before: the first one, bulk or interrupt,
     * or the named one */
    {
        static const unsigned char outs[] = {
            9, 4, 0, 0, 3, 0xFF, 0, 0, 0,
            7, 5, 0x01, 2, 64, 0, 0,  /* bulk OUT */
            7, 5, 0x02, 3, 8, 0, 1,   /* interrupt OUT */
            7, 5, 0x83, 3, 16, 0, 1,  /* interrupt IN */
        };
        memset(&rule, 0, sizeof(rule));
        rule.flags = SDL_VENDORUSB_IN_INTERRUPT;
        CHECK(Select(&rule, 0, outs, sizeof(outs), &selection));
        CHECK(selection.in.address == 0x83 && selection.out.address == 0x01 &&
              selection.out.transfer == SDL_VENDORUSB_TRANSFER_BULK);
        rule.out_endpoint = 0x02;
        CHECK(Select(&rule, 0, outs, sizeof(outs), &selection) && selection.out.address == 0x02);
    }

    /* A rule with the flag alone serves every platform */
    memset(&rule, 0, sizeof(rule));
    rule.flags = SDL_VENDORUSB_IN_INTERRUPT;
    CHECK(SDL_VendorUSB_RuleApplies(&rule, SDL_VENDORUSB_PLATFORM_WINDOWS));
    CHECK(SDL_VendorUSB_RuleApplies(&rule, SDL_VENDORUSB_PLATFORM_OTHER));
    CHECK(SDL_VendorUSB_RuleApplies(&rule, SDL_VENDORUSB_PLATFORM_MACOS));
}

/* Part 14: a rule's read size, for devices that send several packets in one
 * bulk transfer. */
static void TestReadSize(void)
{
    SDL_VendorUSBSelection selection;
    SDL_VendorUSBRule rule;

    /* 0 reads wMaxPacketSize, as before */
    memset(&rule, 0, sizeof(rule));
    CHECK(Select(&rule, 0, trackir_interface0, sizeof(trackir_interface0), &selection));
    CHECK(selection.in.address == 0x82 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_BULK &&
          selection.in.max_packet_size == 64 && selection.in.read_size == 64);

    /* linuxtrack's 16384 bytes. The IN endpoint's packet size stays the
     * descriptor's 64, and the OUT endpoint, which is never read, keeps
     * wMaxPacketSize. */
    rule.read_size = 16384;
    memset(&selection, 0, sizeof(selection));
    CHECK(Select(&rule, 0, trackir_interface0, sizeof(trackir_interface0), &selection));
    CHECK(selection.in.address == 0x82 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_BULK &&
          selection.in.max_packet_size == 64 && selection.in.read_size == 16384);
    CHECK(selection.out.address == 0x02 && selection.out.max_packet_size == 64 && selection.out.read_size == 64);
    {
        static const size_t whole[] = { sizeof(trackir_interface0) };
        CheckPrefixes(&rule, 0, trackir_interface0, sizeof(trackir_interface0), whole, 1);
    }

    /* Whole numbers of 64-byte packets, down to one and up to the largest a
     * rule holds */
    rule.read_size = 64;
    CHECK(Select(&rule, 0, trackir_interface0, sizeof(trackir_interface0), &selection) && selection.in.read_size == 64);
    rule.read_size = 128;
    CHECK(Select(&rule, 0, trackir_interface0, sizeof(trackir_interface0), &selection) && selection.in.read_size == 128);
    rule.read_size = 65472;
    CHECK(Select(&rule, 0, trackir_interface0, sizeof(trackir_interface0), &selection) && selection.in.read_size == 65472);

    /* Any other size can leave the last packet too little room, so nothing
     * is selected and the selection is not written */
    {
        static const uint16_t partial[] = { 1, 32, 63, 65, 100, 16416, 65535 };
        size_t i;

        for (i = 0; i < sizeof(partial) / sizeof(partial[0]); ++i) {
            rule.read_size = partial[i];
            memset(&selection, 0xEE, sizeof(selection));
            CHECK(!SDL_VendorUSB_SelectEndpoints(&rule, 0, trackir_interface0, sizeof(trackir_interface0), &selection));
            CHECK(selection.alternate == 0xEE && selection.in.address == 0xEE && selection.in.read_size == 0xEEEE);
            CheckPrefixes(&rule, 0, trackir_interface0, sizeof(trackir_interface0), NULL, 0);
        }
    }

    /* A high-speed bulk IN endpoint of 512 bytes, the DJI RC's */
    {
        const SDL_VendorUSBRule *dji = SDL_VendorUSB_FindRule(USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 1, 0xFF, 0x43, 0x01);

        if (dji) {
            rule = *dji;
            rule.read_size = 16384;
            CHECK(Select(&rule, 1, rm330_interface1, sizeof(rm330_interface1), &selection));
            CHECK(selection.in.address == 0x83 && selection.in.max_packet_size == 512 && selection.in.read_size == 16384);
            rule.read_size = 1024;
            CHECK(Select(&rule, 1, rm330_interface1, sizeof(rm330_interface1), &selection) && selection.in.read_size == 1024);
            rule.read_size = 576;
            CHECK(!Select(&rule, 1, rm330_interface1, sizeof(rm330_interface1), &selection));
            rule.read_size = 256;
            CHECK(!Select(&rule, 1, rm330_interface1, sizeof(rm330_interface1), &selection));
        }
    }

    /* The read size never chooses the endpoint. It applies to the first
     * bulk IN endpoint, or the named one. */
    {
        static const unsigned char two[] = {
            9, 4, 0, 0, 2, 0xFF, 0, 0, 0,
            7, 5, 0x81, 2, 64, 0, 0,
            7, 5, 0x82, 2, 0x00, 0x02, 0,
        };
        memset(&rule, 0, sizeof(rule));
        rule.read_size = 1024;
        CHECK(Select(&rule, 0, two, sizeof(two), &selection) && selection.in.address == 0x81 && selection.in.read_size == 1024);
        rule.read_size = 96;
        CHECK(!Select(&rule, 0, two, sizeof(two), &selection));
        rule.in_endpoint = 0x82;
        rule.read_size = 1024;
        CHECK(Select(&rule, 0, two, sizeof(two), &selection) && selection.in.address == 0x82 && selection.in.read_size == 1024);
        rule.read_size = 576;
        CHECK(!Select(&rule, 0, two, sizeof(two), &selection));
    }

    /* A bulk IN endpoint with a packet size of 0 cannot be read in whole
     * packets. It selects as before without a read size. */
    {
        static const unsigned char zero[] = {
            9, 4, 0, 0, 1, 0xFF, 0, 0, 0,
            7, 5, 0x81, 2, 0, 0, 0,
        };
        memset(&rule, 0, sizeof(rule));
        CHECK(Select(&rule, 0, zero, sizeof(zero), &selection) && selection.in.address == 0x81 && selection.in.read_size == 0);
        rule.read_size = 16384;
        CHECK(!Select(&rule, 0, zero, sizeof(zero), &selection));
    }

    /* The packet size is bits 0 to 10 of wMaxPacketSize, as for a rule's
     * size, so a bulk endpoint that sets bit 11 still has 64-byte packets */
    {
        static const unsigned char multiplier[] = {
            9, 4, 0, 0, 1, 0xFF, 0, 0, 0,
            7, 5, 0x81, 2, 0x40, 0x08, 0,
        };
        memset(&rule, 0, sizeof(rule));
        CHECK(Select(&rule, 0, multiplier, sizeof(multiplier), &selection) && selection.in.read_size == 0x0840);
        rule.read_size = 16384;
        CHECK(Select(&rule, 0, multiplier, sizeof(multiplier), &selection));
        CHECK(selection.in.max_packet_size == 64 && selection.in.read_size == 16384);
    }

    /* The check is arithmetic on whatever packet size the descriptor gives.
     * A 48-byte bulk endpoint breaks USB 2.0 section 5.8.3. 65520 bytes are
     * 1365 of its packets, and 65504 bytes leave 32 over. */
    {
        static const unsigned char odd[] = {
            9, 4, 0, 0, 1, 0xFF, 0, 0, 0,
            7, 5, 0x81, 2, 48, 0, 0,
        };
        memset(&rule, 0, sizeof(rule));
        rule.read_size = 65520;
        CHECK(Select(&rule, 0, odd, sizeof(odd), &selection) && selection.in.read_size == 65520);
        rule.read_size = 65504;
        CHECK(!Select(&rule, 0, odd, sizeof(odd), &selection));
    }

    /* An interrupt IN endpoint ignores the read size, whole packets or not */
    {
        const SDL_VendorUSBRule *intel = IntelRule();
        static const unsigned char high_bandwidth[] = {
            9, 4, 0, 0, 1, 0xFF, 0, 0, 0,
            7, 5, 0x81, 3, 0x40, 0x12, 1,
        };

        if (intel) {
            rule = *intel;
            rule.read_size = 16384;
            CHECK(Select(&rule, 0, intel_interface0, sizeof(intel_interface0), &selection));
            CHECK(selection.in.address == 0x81 && selection.in.max_packet_size == 27 && selection.in.read_size == 27);
            CHECK(selection.out.read_size == 25);
            rule.read_size = 100;
            CHECK(Select(&rule, 0, intel_interface0, sizeof(intel_interface0), &selection) && selection.in.read_size == 27);
        }
        memset(&rule, 0, sizeof(rule));
        rule.read_size = 16384;
        CHECK(Select(&rule, 0, high_bandwidth, sizeof(high_bandwidth), &selection));
        CHECK(selection.in.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT && selection.in.read_size == 0x1240);
    }

    /* Under SDL_VENDORUSB_IN_INTERRUPT the P4IO's interrupt endpoint ignores
     * it too, and without the flag its bulk IN endpoint takes it */
    memset(&rule, 0, sizeof(rule));
    rule.read_size = 16384;
    CHECK(Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection));
    CHECK(selection.in.address == 0x82 && selection.in.read_size == 16384);
    rule.flags = SDL_VENDORUSB_IN_INTERRUPT;
    CHECK(Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection));
    CHECK(selection.in.address == 0x83 && selection.in.read_size == 16);
    /* The bulk IN endpoint it passes over is not checked against it */
    rule.read_size = 100;
    CHECK(Select(&rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection) && selection.in.read_size == 16);
    {
        static const size_t whole[] = { sizeof(p4io_interface0) };
        CheckPrefixes(&rule, 0, p4io_interface0, sizeof(p4io_interface0), whole, 1);
    }
}

/* The rules written before Part 14 keep their meaning: none reads an
 * interrupt endpoint by the flag, and each reads wMaxPacketSize, since their
 * initializers leave the new trailing member zero. */
static void TestEarlierRules(void)
{
    static const struct
    {
        uint16_t vendor, product;
        uint8_t number, cls, subclass, protocol;
    } earlier[] = {
        { USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_GAMECUBE_ADAPTER, 0, 3, 0, 0 },
        { USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER, 0, 0xFF, 0x5D, 0x04 },
        { USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES, 0, 3, 1, 1 },
        { USB_VENDOR_IN2GAMES, USB_PRODUCT_IN2GAMES_GAMETRAK, 0, 3, 0, 0 },
        { USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 1, 0xFF, 0x43, 0x01 },
        { USB_VENDOR_THRUSTMASTER, 0xA01C, 0, 0xFF, 0, 0 },
        { USB_VENDOR_LOGITECH, 0xC281, 0, 0xFF, 0, 0 },
        { USB_VENDOR_LOGITECH, 0xC291, 0, 0xFF, 0, 0 },
        { USB_VENDOR_AVB, 0x020A, 0, 0xFF, 0, 0 },
        { USB_VENDOR_AVB, 0x8884, 0, 0xFF, 0, 0 },
        { USB_VENDOR_AVB, 0x8888, 0, 0xFF, 0, 0 },
        { USB_VENDOR_ACTLABS, 0xC084, 0, 0xFF, 0, 0 },
        { USB_VENDOR_ACTLABS, 0xC094, 0, 0xFF, 0, 0 },
        { USB_VENDOR_ACTLABS, 0xC0A4, 0, 0xFF, 0, 0 },
        { USB_VENDOR_SAITEK, 0xFF04, 0, 0xFF, 0, 0 },
        { USB_VENDOR_GUILLEMOT, 0x0001, 0, 0xFF, 0, 0 },
        { USB_VENDOR_GUILLEMOT, 0x0003, 0, 0xFF, 0, 0 },
        { USB_VENDOR_GUILLEMOT, 0x0004, 0, 0xFF, 0, 0 },
        { USB_VENDOR_GUILLEMOT, 0xA302, 0, 0xFF, 0, 0 },
        { USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_GUNCON2, 0, 0xFF, 0x6A, 0 },
        { USB_VENDOR_TAITO, USB_PRODUCT_TAITO_DENSHA_TYPE2, 0, 3, 0, 0 },
        { USB_VENDOR_TAITO, USB_PRODUCT_TAITO_DENSHA_SHINKANSEN, 0, 3, 0, 0 },
        { USB_VENDOR_TAITO, USB_PRODUCT_TAITO_DENSHA_RYOJOHEN, 0, 3, 0, 0 },
        { USB_VENDOR_TAITO, USB_PRODUCT_TAITO_MULTI_TRAIN_CONTROLLER, 0, 0, 0, 0 },
        { USB_VENDOR_TRAIN_MASCON, USB_PRODUCT_TRAIN_MASCON, 0, 0, 0, 0 },
    };
    size_t i;

    for (i = 0; i < sizeof(earlier) / sizeof(earlier[0]); ++i) {
        const SDL_VendorUSBRule *rule = SDL_VendorUSB_FindRule(earlier[i].vendor, earlier[i].product, earlier[i].number,
                                                               earlier[i].cls, earlier[i].subclass, earlier[i].protocol);
        CHECK(rule != NULL);
        if (rule) {
            CHECK(!(rule->flags & SDL_VENDORUSB_IN_INTERRUPT));
            CHECK(rule->read_size == 0);
        }
    }
    /* The trailing member follows the members every earlier initializer
     * fills, so their values land where they did */
    {
        const SDL_VendorUSBRule *intel = IntelRule();
        CHECK(intel && intel->in_size == 27 && intel->out_size == 25 && intel->read_size == 0);
        CHECK(offsetof(SDL_VendorUSBRule, read_size) > offsetof(SDL_VendorUSBRule, out_size));
    }
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
    /* Part 15: the wired pad's other interfaces, from Javan Cook's lsusb of
     * 045E:028E at bcdDevice 1.14, and a receiver headset interface, from
     * rombert's lsusb of 045E:0719. is_xbox360 takes none of them. */
    { "Xbox 360 wired pad headset", USB_VENDOR_MICROSOFT, 0x028E, 1, 0xFF, 0x5D, 0x03, false },
    { "Xbox 360 wired pad chatpad", USB_VENDOR_MICROSOFT, 0x028E, 2, 0xFF, 0x5D, 0x02, false },
    { "Xbox 360 wired pad security", USB_VENDOR_MICROSOFT, 0x028E, 3, 0xFF, 0xFD, 0x13, false },
    { "Xbox 360 receiver headset", USB_VENDOR_MICROSOFT, 0x0719, 1, 0xFF, 0x5D, 0x82, false },
    { "Vendor interface, no rule", 0x1234, 0x5678, 0, 0xFF, 0, 0, false },
    { "HID keyboard", 0x046D, 0xC31C, 0, 3, 1, 1, false },
    { "Gametrak", USB_VENDOR_IN2GAMES, USB_PRODUCT_IN2GAMES_GAMETRAK, 0, 3, 0, 0, false },
    { "DJI RC MTP", USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 0, 0x06, 0x01, 0x01, false },
    { "DJI RC bulk", USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 1, 0xFF, 0x43, 0x01, false },
    { "DJI RC ADB", USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, 2, 0xFF, 0x42, 0x01, false },
    { "WingMan Formula Force", USB_VENDOR_LOGITECH, 0xC291, 0, 0xFF, 0x00, 0x00, false },
    { "Guillemot Force Feedback Racing Wheel", USB_VENDOR_GUILLEMOT, 0x0004, 0, 0x03, 0x00, 0x00, false },
    { "GunCon 2", USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_GUNCON2, 0, 0xFF, 0x6A, 0x00, false },
    { "GunCon 3", USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_GUNCON3, 0, 0xFF, 0x00, 0x00, false },
    { "Taito Type 2", USB_VENDOR_TAITO, USB_PRODUCT_TAITO_DENSHA_TYPE2, 0, 0x03, 0x00, 0x00, false },
    { "Multi Train Controller", USB_VENDOR_TAITO, USB_PRODUCT_TAITO_MULTI_TRAIN_CONTROLLER, 0, 0x00, 0x00, 0x00, false },
    { "Train Mascon", USB_VENDOR_TRAIN_MASCON, USB_PRODUCT_TRAIN_MASCON, 0, 0x00, 0x00, 0x00, false },
    { "DGOC-44U", USB_VENDOR_TAITO, 0x0003, 0, 0x03, 0x00, 0x00, false },
    { "USIO", USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_USIO, 0, 0x00, 0x00, 0x00, false },
    { "USIO, class FF", USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_USIO, 0, 0xFF, 0x00, 0x00, false },
    { "H050 USJ(C)", USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_H050_USJC, 0, 0x00, 0x00, 0x00, false },
    { "Konami P3IO", USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P3IO, 0, 0xFF, 0x00, 0x00, false },
    { "Konami P3IO interface 1", USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P3IO, 1, 0xFF, 0x00, 0x00, false },
    /* No source records the P4IO's class, so vendor class stands in */
    { "Konami P4IO", USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P4IO, 0, 0xFF, 0x00, 0x00, false },
    { "CH MFP", USB_VENDOR_CH_PRODUCTS, USB_PRODUCT_CH_PRODUCTS_MFP, 0, 0xFF, 0x00, 0x00, false },
    { "DX1 keyboard", USB_VENDOR_ERGODEX, USB_PRODUCT_ERGODEX_DX1, 0, 0x03, 0x00, 0x01, false },
    { "DX1 vendor", USB_VENDOR_ERGODEX, USB_PRODUCT_ERGODEX_DX1, 1, 0xFF, 0x00, 0x01, false },
    { "TrackIR 3", USB_VENDOR_NATURALPOINT, USB_PRODUCT_NATURALPOINT_TRACKIR3, 0, 0xFF, 0x00, 0x00, false },
    { "Tacx T1904", USB_VENDOR_TACX, USB_PRODUCT_TACX_T1904, 0, 0xFF, 0x00, 0x00, false },
    { "Tacx T1932", USB_VENDOR_TACX, USB_PRODUCT_TACX_T1932, 0, 0xFF, 0x00, 0x00, false },
    { "Tacx T1942", USB_VENDOR_TACX, 0x1942, 0, 0xFF, 0x00, 0x00, false },
    { "Tacx T1942 before its firmware", USB_VENDOR_TACX, 0xE6BE, 0, 0xFF, 0x00, 0x00, false },
    { "Tacx T1902", USB_VENDOR_TACX, 0x1902, 0, 0xFF, 0x00, 0x00, false },
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

    /* Part 9: the GunCon 2 reaches libusb everywhere, with the whitelist
       on or off, and no platform backend keeps it. The GunCon 3 stays off
       libusb, as before. */
    CHECK(OnLibUSB(W, "GunCon 2", true) && OnLibUSB(L, "GunCon 2", true) && OnLibUSB(M, "GunCon 2", true));
    CHECK(PlatformIgnores(W, "GunCon 2") && PlatformIgnores(L, "GunCon 2") && PlatformIgnores(M, "GunCon 2"));
    CHECK(!OnLibUSB(W, "GunCon 3", true) && !OnLibUSB(L, "GunCon 3", true) && !OnLibUSB(M, "GunCon 3", true));
    CHECK(!PlatformIgnores(W, "GunCon 3") && !PlatformIgnores(L, "GunCon 3"));
    {
        const Device *d = Find("GunCon 2");
        SDL_VendorUSBRouting r = Route(L, true, d);

        r.whitelist = false;
        CHECK(NewLibUSBEnumerates(&r, d, true));
        d = Find("GunCon 3");
        r = Route(W, true, d);
        r.whitelist = false;
        CHECK(!NewLibUSBEnumerates(&r, d, true));
    }

    /* Part 10: the train controllers reach libusb everywhere, and no
       platform backend keeps them. The HID DGOC-44U stays with Windows. */
    CHECK(OnLibUSB(W, "Taito Type 2", true) && OnLibUSB(L, "Taito Type 2", true) && OnLibUSB(M, "Taito Type 2", true));
    CHECK(OnLibUSB(W, "Multi Train Controller", true) && OnLibUSB(L, "Multi Train Controller", true));
    CHECK(OnLibUSB(M, "Train Mascon", true) && OnLibUSB(W, "Train Mascon", true));
    CHECK(PlatformIgnores(W, "Taito Type 2") && PlatformIgnores(L, "Taito Type 2") && PlatformIgnores(M, "Train Mascon"));
    CHECK(!OnLibUSB(W, "DGOC-44U", true) && !PlatformIgnores(W, "DGOC-44U"));

    /* Part 14: the Namco USIO and the H050 USJ(C) reach libusb on Windows
       once WinUSB is bound, and the platform backend leaves them. Elsewhere
       nothing changes. */
    CHECK(OnLibUSB(W, "USIO", true) && OnLibUSB(W, "USIO, class FF", true) && OnLibUSB(W, "H050 USJ(C)", true));
    CHECK(PlatformIgnores(W, "USIO") && PlatformIgnores(W, "H050 USJ(C)"));
    CHECK(!OnLibUSB(L, "USIO", true) && !OnLibUSB(M, "USIO", true) && !OnLibUSB(L, "USIO, class FF", true));
    CHECK(!OnLibUSB(L, "H050 USJ(C)", true) && !OnLibUSB(M, "H050 USJ(C)", true));
    CHECK(!PlatformIgnores(L, "USIO") && !PlatformIgnores(M, "USIO") && !PlatformIgnores(L, "H050 USJ(C)"));
    /* Without the whitelist a vendor-class interface still stays off libusb there */
    {
        const Device *d = Find("USIO, class FF");
        SDL_VendorUSBRouting r = Route(L, true, d);

        r.whitelist = false;
        CHECK(!NewLibUSBEnumerates(&r, d, true));
        r = Route(W, true, d);
        r.whitelist = false;
        CHECK(NewLibUSBEnumerates(&r, d, true));
    }

    /* Part 14: the Konami P3IO reaches libusb on Windows on interface 0,
       and the platform backend leaves it. No other interface of the
       board is enumerated, and elsewhere nothing changes. */
    CHECK(OnLibUSB(W, "Konami P3IO", true) && PlatformIgnores(W, "Konami P3IO"));
    CHECK(!OnLibUSB(L, "Konami P3IO", true) && !OnLibUSB(M, "Konami P3IO", true));
    CHECK(!PlatformIgnores(L, "Konami P3IO") && !PlatformIgnores(M, "Konami P3IO"));
    CHECK(!OnLibUSB(W, "Konami P3IO interface 1", true) && !OnLibUSB(L, "Konami P3IO interface 1", true) &&
          !OnLibUSB(M, "Konami P3IO interface 1", true));

    /* Part 14: the CH Products MFP and the Ergodex DX1's vendor interface
       reach libusb on Windows, and the platform backend leaves them. The
       DX1's keyboard interface never reaches libusb there. Elsewhere
       nothing changes, and libusb may take the keyboard as any HID
       interface. */
    CHECK(OnLibUSB(W, "CH MFP", true) && PlatformIgnores(W, "CH MFP"));
    CHECK(!OnLibUSB(L, "CH MFP", true) && !OnLibUSB(M, "CH MFP", true));
    CHECK(!PlatformIgnores(L, "CH MFP") && !PlatformIgnores(M, "CH MFP"));
    CHECK(OnLibUSB(W, "DX1 vendor", true) && !OnLibUSB(L, "DX1 vendor", true) && !OnLibUSB(M, "DX1 vendor", true));
    CHECK(!OnLibUSB(W, "DX1 keyboard", true) && !OnLibUSB(L, "DX1 keyboard", true) && !OnLibUSB(M, "DX1 keyboard", true));
    CHECK(PlatformIgnores(W, "DX1 keyboard") && !PlatformIgnores(L, "DX1 keyboard") && !PlatformIgnores(M, "DX1 keyboard"));
    {
        const Device *d = Find("DX1 keyboard");
        SDL_VendorUSBRouting r = Route(W, true, d);

        r.whitelist = false;
        CHECK(!NewLibUSBEnumerates(&r, d, true));
        r = Route(L, true, d);
        r.whitelist = false;
        CHECK(NewLibUSBEnumerates(&r, d, true));
        d = Find("DX1 vendor");
        r = Route(W, true, d);
        r.whitelist = false;
        CHECK(NewLibUSBEnumerates(&r, d, true));
        r = Route(L, true, d);
        r.whitelist = false;
        CHECK(!NewLibUSBEnumerates(&r, d, true));
    }

    /* Part 14: the TrackIR 3 reaches libusb on Windows, and the platform
       backend leaves it. Elsewhere linuxtrack keeps it. */
    CHECK(OnLibUSB(W, "TrackIR 3", true) && PlatformIgnores(W, "TrackIR 3"));
    CHECK(!OnLibUSB(L, "TrackIR 3", true) && !OnLibUSB(M, "TrackIR 3", true));
    CHECK(!PlatformIgnores(L, "TrackIR 3") && !PlatformIgnores(M, "TrackIR 3"));

    /* Part 14: the Tacx T1904 and T1932 reach libusb on Windows, with the
       whitelist on or off, and the platform backend leaves them. The T1902
       and the T1942, before or after its firmware, stay off the path.
       Elsewhere nothing changes. */
    CHECK(OnLibUSB(W, "Tacx T1904", true) && OnLibUSB(W, "Tacx T1932", true));
    CHECK(!OnLibUSB(L, "Tacx T1904", true) && !OnLibUSB(L, "Tacx T1932", true) && !OnLibUSB(M, "Tacx T1932", true));
    CHECK(PlatformIgnores(W, "Tacx T1904") && PlatformIgnores(W, "Tacx T1932"));
    CHECK(!PlatformIgnores(L, "Tacx T1932") && !PlatformIgnores(M, "Tacx T1904"));
    CHECK(!OnLibUSB(W, "Tacx T1942", true) && !OnLibUSB(L, "Tacx T1942", true) && !PlatformIgnores(W, "Tacx T1942"));
    CHECK(!OnLibUSB(W, "Tacx T1942 before its firmware", true) && !OnLibUSB(L, "Tacx T1942 before its firmware", true));
    CHECK(!OnLibUSB(W, "Tacx T1902", true) && !OnLibUSB(M, "Tacx T1902", true));
    CHECK(!PlatformIgnores(W, "Tacx T1902") && !PlatformIgnores(L, "Tacx T1942 before its firmware"));
    {
        const Device *d = Find("Tacx T1932");
        SDL_VendorUSBRouting r = Route(W, true, d);

        r.whitelist = false;
        CHECK(NewLibUSBEnumerates(&r, d, true));
        r = Route(L, true, d);
        r.whitelist = false;
        CHECK(!NewLibUSBEnumerates(&r, d, true));
        d = Find("Tacx T1902");
        r = Route(W, true, d);
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

/* Part 15: WinUSB lets one handle open a device, so the enumeration's own
 * open of an Xbox interface fails while this process holds any interface of
 * the device. hid.c passes the interface as opened while that holder still
 * reads, so the joystick layer keeps the pad it has open and finds the
 * receiver's other slots. The wired pad's chatpad, headset and security
 * interfaces, and the receiver's headset interfaces, are never enumerated. */
static void TestHeldInterfaces(void)
{
    static const SDL_VendorUSBPlatform platforms[] = {
        SDL_VENDORUSB_PLATFORM_OTHER, SDL_VENDORUSB_PLATFORM_WINDOWS, SDL_VENDORUSB_PLATFORM_MACOS
    };
    static const char *const others[] = {
        "Xbox 360 wired pad headset", "Xbox 360 wired pad chatpad", "Xbox 360 wired pad security",
        "Xbox 360 receiver headset"
    };
    const SDL_VendorUSBPlatform W = SDL_VENDORUSB_PLATFORM_WINDOWS;
    size_t p, i;
    int flags;

    /* Only an Xbox interface on Windows that neither opened nor is held is
       skipped */
    for (p = 0; p < sizeof(platforms) / sizeof(platforms[0]); ++p) {
        for (flags = 0; flags < 8; ++flags) {
            const bool xbox = (flags & 1) != 0;
            const bool opened = (flags & 2) != 0;
            const bool held = (flags & 4) != 0;

            CHECK(SDL_VendorUSB_SkipUnopened(platforms[p], xbox, opened || held) ==
                  (platforms[p] == W && xbox && !opened && !held));
        }
    }

    /* The wired pad and the receiver stay on libusb while this process
       holds them */
    {
        const char *const held[] = { "Xbox 360 wired pad", "Xbox 360 wireless receiver" };

        for (i = 0; i < sizeof(held) / sizeof(held[0]); ++i) {
            const Device *d = Find(held[i]);
            SDL_VendorUSBRouting r = Route(W, true, d);

            CHECK(!SDL_VendorUSB_Ignore(&r));
            CHECK(SDL_VendorUSB_IsCandidate(W, d->vendor, d->product, d->number, d->cls, d->subclass, d->protocol, d->xbox));
            CHECK(NewLibUSBEnumerates(&r, d, true));
            CHECK(!NewLibUSBEnumerates(&r, d, false));
        }
    }

    for (p = 0; p < sizeof(platforms) / sizeof(platforms[0]); ++p) {
        for (i = 0; i < sizeof(others) / sizeof(others[0]); ++i) {
            const Device *d = Find(others[i]);
            SDL_VendorUSBRouting r = Route(platforms[p], true, d);

            CHECK(!OnLibUSB(platforms[p], others[i], true));
            r.whitelist = false;
            CHECK(!NewLibUSBEnumerates(&r, d, true));
            CHECK(!SDL_VendorUSB_IsCandidate(platforms[p], d->vendor, d->product, d->number, d->cls, d->subclass, d->protocol, d->xbox));
            CHECK(SDL_VendorUSB_FindRule(d->vendor, d->product, d->number, d->cls, d->subclass, d->protocol) == NULL);
        }
    }
}

/* Part 14: Konami's P4IO, whose input comes on the interrupt endpoint behind
 * its bulk IN endpoint */
static void TestKonamiP4IO(void)
{
    const SDL_VendorUSBPlatform W = SDL_VENDORUSB_PLATFORM_WINDOWS;
    const SDL_VendorUSBPlatform L = SDL_VENDORUSB_PLATFORM_OTHER;
    const SDL_VendorUSBPlatform M = SDL_VENDORUSB_PLATFORM_MACOS;
    const SDL_VendorUSBRule *rule;
    SDL_VendorUSBSelection selection;

    CHECK(USB_VENDOR_KONAMI == 0x1CCF && USB_PRODUCT_KONAMI_P4IO == 0x8010);
    rule = SDL_VendorUSB_FindRule(USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P4IO, 0, 0xFF, 0x00, 0x00);
    CHECK(rule && rule->flags == (SDL_VENDORUSB_IN_INTERRUPT | SDL_VENDORUSB_WINDOWS_ONLY) &&
          rule->interface_number == 0 && rule->alternate == 0);
    CHECK(rule && rule->in_endpoint == 0 && rule->out_endpoint == 0 && rule->in_size == 0 && rule->out_size == 0 &&
          rule->read_size == 0);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P4IO, 0, 0x00, 0x00, 0x00) == rule);
    CHECK(SDL_VendorUSB_FindRule(USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P4IO, 1, 0xFF, 0x00, 0x00) == NULL);
    /* Windows only, like every rule of Part 14. Only the interface's rule
       takes the board to libusb. */
    CHECK(SDL_VendorUSB_RuleApplies(rule, W) && !SDL_VendorUSB_RuleApplies(rule, L) && !SDL_VendorUSB_RuleApplies(rule, M));
    CHECK(SDL_VendorUSB_IsVendorDevice(USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P4IO));
    CHECK(!SDL_VendorUSB_RequiresLibUSB(L, USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P4IO, false, false));
    CHECK(!SDL_VendorUSB_RequiresLibUSB(M, USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P4IO, false, false));
    CHECK(!SDL_VendorUSB_IsVendorDevice(USB_VENDOR_KONAMI, 0x8011));

    /* On the order p4io-mdxfdrv reads, the rule takes the interrupt IN
       endpoint and the bulk OUT one, and leaves bulk IN to the driver */
    if (rule) {
        static const size_t whole[] = { sizeof(p4io_interface0) };
        static const unsigned char bulk_only[] = {
            9, 4, 0, 0, 2, 0xFF, 0, 0, 0,
            7, 5, 0x01, 2, 64, 0, 0,
            7, 5, 0x82, 2, 64, 0, 0,
        };

        memset(&selection, 0, sizeof(selection));
        CHECK(Select(rule, 0, p4io_interface0, sizeof(p4io_interface0), &selection));
        CHECK(selection.alternate == 0);
        CHECK(selection.in.address == 0x83 && selection.in.transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT &&
              selection.in.read_size == 16);
        CHECK(selection.out.address == 0x01 && selection.out.transfer == SDL_VENDORUSB_TRANSFER_BULK);
        CheckPrefixes(rule, 0, p4io_interface0, sizeof(p4io_interface0), whole, 1);
        /* Without an interrupt IN endpoint the interface does not open */
        CHECK(!Select(rule, 0, bulk_only, sizeof(bulk_only), &selection));
    }

    /* libusb reaches it on Windows, with the whitelist on or off, and the
       platform backend leaves it. Only interface 0 is enumerated there.
       Elsewhere nothing changes. */
    CHECK(OnLibUSB(W, "Konami P4IO", true) && PlatformIgnores(W, "Konami P4IO"));
    CHECK(!OnLibUSB(L, "Konami P4IO", true) && !OnLibUSB(M, "Konami P4IO", true));
    CHECK(!PlatformIgnores(L, "Konami P4IO") && !PlatformIgnores(M, "Konami P4IO"));
    {
        const Device *d = Find("Konami P4IO");
        SDL_VendorUSBRouting r = Route(W, true, d);

        r.whitelist = false;
        CHECK(NewLibUSBEnumerates(&r, d, true));
        r = Route(L, true, d);
        r.whitelist = false;
        CHECK(!NewLibUSBEnumerates(&r, d, true));
        CHECK(!SDL_VendorUSB_IsCandidate(W, USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P4IO, 1, 0x03, 0x00, 0x00, false));
        CHECK(SDL_VendorUSB_IsCandidate(L, USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P4IO, 1, 0x03, 0x00, 0x00, false));
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
    const bool guncon2 = (d->vendor == USB_VENDOR_NAMCO && d->product == USB_PRODUCT_NAMCO_GUNCON2);
    const bool train = ((d->vendor == USB_VENDOR_TAITO && d->product != 0x0003) || d->vendor == USB_VENDOR_TRAIN_MASCON);
    const bool usio = (d->vendor == USB_VENDOR_NAMCO &&
                       (d->product == USB_PRODUCT_NAMCO_USIO || d->product == USB_PRODUCT_NAMCO_H050_USJC));
    const bool p3io = (d->vendor == USB_VENDOR_KONAMI && d->product == USB_PRODUCT_KONAMI_P3IO);
    const bool p4io = (d->vendor == USB_VENDOR_KONAMI && d->product == USB_PRODUCT_KONAMI_P4IO);
    const bool keypad = ((d->vendor == USB_VENDOR_CH_PRODUCTS && d->product == USB_PRODUCT_CH_PRODUCTS_MFP) ||
                         (d->vendor == USB_VENDOR_ERGODEX && d->product == USB_PRODUCT_ERGODEX_DX1));
    const bool trackir = (d->vendor == USB_VENDOR_NATURALPOINT);
    const bool tacx = (d->vendor == USB_VENDOR_TACX && (d->product == USB_PRODUCT_TACX_T1904 ||
                                                        d->product == USB_PRODUCT_TACX_T1932));

    if (intel || dji || guncon2 || train) {
        return true; /* A new member of the path */
    }
    if ((gametrak || iforce || usio || p3io || p4io || keypad || trackir || tacx) &&
        r->platform == SDL_VENDORUSB_PLATFORM_WINDOWS) {
        return true; /* Parts 7, 8 and 14: on the path on Windows only */
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
    TestInterruptRule();
    TestReadSize();
    TestEarlierRules();
    TestRoutingTable();
    TestHeldInterfaces();
    TestKonamiP4IO();
    TestAgainstOldRules();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
