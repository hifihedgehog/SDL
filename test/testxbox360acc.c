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

/* Offline replay test of the Xbox 360 chatpads and the Xbox 360 uDraw
 * (hifihedgehog/SDL#33 Part 15). No device, no SDL runtime, time from an
 * injected millisecond clock. Every state lives in an exact-size heap block
 * and every packet is fed from an exact-size heap copy, so AddressSanitizer
 * catches any access outside either. Truncated packets are fed a second
 * time from a buffer whose bytes past the length belong to another report.
 *
 * The key reports are built from Spivey's serial capture in xboxdrv's
 * 5-byte USB framing, the status from xboxdrv's PROTOCOL notes, the receiver
 * packets from [MS-XUSBI] 3.5.5.1.1 and the tablet packets from
 * brandonw.net's table. */

#include "../src/joystick/hidapi/SDL_hidapi_xbox360acc_proto.h"

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

#define BIT(n) ((uint64_t)1 << (n))

/* The key table restated from the part's rows, independent of the module's
 * table: code, button, US legend */
static const struct
{
    uint8_t code;
    int button;
    const char *legend;
} keys[] = {
    { 0x17, 4, "1" }, { 0x16, 5, "2" }, { 0x15, 6, "3" }, { 0x14, 7, "4" }, { 0x13, 8, "5" },
    { 0x12, 9, "6" }, { 0x11, 10, "7" }, { 0x67, 11, "8" }, { 0x66, 12, "9" }, { 0x65, 13, "0" },
    { 0x27, 14, "Q" }, { 0x26, 15, "W" }, { 0x25, 16, "E" }, { 0x24, 17, "R" }, { 0x23, 18, "T" },
    { 0x22, 19, "Y" }, { 0x21, 20, "U" }, { 0x76, 21, "I" }, { 0x75, 22, "O" }, { 0x64, 23, "P" },
    { 0x37, 24, "A" }, { 0x36, 25, "S" }, { 0x35, 26, "D" }, { 0x34, 27, "F" }, { 0x33, 28, "G" },
    { 0x32, 29, "H" }, { 0x31, 30, "J" }, { 0x77, 31, "K" }, { 0x72, 32, "L" }, { 0x62, 33, "comma" },
    { 0x46, 34, "Z" }, { 0x45, 35, "X" }, { 0x44, 36, "C" }, { 0x43, 37, "V" }, { 0x42, 38, "B" },
    { 0x41, 39, "N" }, { 0x52, 40, "M" }, { 0x53, 41, "period" }, { 0x63, 42, "Enter" },
    { 0x55, 43, "Left" }, { 0x54, 44, "Space" }, { 0x51, 45, "Right" }, { 0x71, 46, "Backspace" },
};
#define NUM_KEYS ((int)(sizeof(keys) / sizeof(keys[0])))

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

static bool SameChatpad(const SDL_Xbox360AccChatpad *a, const SDL_Xbox360AccChatpad *b)
{
    return a->present == b->present && a->buttons == b->buttons;
}

static bool SameWired(const SDL_Xbox360AccWired *a, const SDL_Xbox360AccWired *b)
{
    return a->bcd_device == b->bcd_device && a->step == b->step && a->in_flight == b->in_flight &&
           a->in_flight_extra == b->in_flight_extra && a->send_1b == b->send_1b &&
           a->keepalive_1e == b->keepalive_1e && a->due_ms == b->due_ms && SameChatpad(&a->chatpad, &b->chatpad);
}

static bool SameTablet(const SDL_Xbox360AccTablet *a, const SDL_Xbox360AccTablet *b)
{
    int i;

    for (i = 0; i < SDL_XBOX360ACC_TABLET_AXES; ++i) {
        if (a->axes[i] != b->axes[i]) {
            return false;
        }
    }
    return a->buttons == b->buttons && a->hat == b->hat;
}

static bool SameSlot(const SDL_Xbox360AccSlot *a, const SDL_Xbox360AccSlot *b)
{
    int i;

    if (a->chatpad_enabled != b->chatpad_enabled || a->udraw_enabled != b->udraw_enabled ||
        a->link != b->link || a->kind != b->kind || a->scheduled != b->scheduled ||
        a->need_init != b->need_init || a->keepalive_1f != b->keepalive_1f || a->due_ms != b->due_ms ||
        a->lamps != b->lamps || a->command_count != b->command_count) {
        return false;
    }
    for (i = 0; i < a->command_count; ++i) {
        if (a->commands[i] != b->commands[i]) {
            return false;
        }
    }
    return SameChatpad(&a->chatpad, &b->chatpad) && SameTablet(&a->tablet, &b->tablet);
}

/* The wired chatpad */

static SDL_Xbox360AccWired *NewWired(void)
{
    SDL_Xbox360AccWired *wired = (SDL_Xbox360AccWired *)malloc(sizeof(*wired));

    if (!wired) {
        printf("FAIL: out of memory\n");
        exit(1);
    }
    memset(wired, 0xA5, sizeof(*wired));
    return wired;
}

static void WiredReport(SDL_Xbox360AccWired *wired, const uint8_t *bytes, size_t length)
{
    uint8_t *copy = Copy(bytes, length);
    SDL_Xbox360Acc_WiredChatpadReport(wired, copy, length);
    free(copy);
}

static void PadReport(SDL_Xbox360AccWired *wired, const uint8_t *bytes, size_t length)
{
    uint8_t *copy = Copy(bytes, length);
    SDL_Xbox360Acc_WiredPadReport(wired, copy, length);
    free(copy);
}

static bool IsControl(const SDL_Xbox360AccControl *control, uint8_t request_type, uint8_t request,
                      uint16_t value, uint16_t index, uint16_t length)
{
    return control->request_type == request_type && control->request == request && control->value == value &&
           control->index == index && control->length == length;
}

static bool IsCommand(const SDL_Xbox360AccControl *control, uint16_t command)
{
    return IsControl(control, 0x41, 0x00, command, 0x0002, 0);
}

/* Sends every transfer due at now, each completing at once, and returns
 * how many went out. The commands they carried land in sent. */
static int WiredPump(SDL_Xbox360AccWired *wired, uint64_t now, SDL_Xbox360AccControl *sent, int max)
{
    SDL_Xbox360AccControl control;
    int count = 0;

    while (count < 64 && SDL_Xbox360Acc_WiredNext(wired, now, &control)) {
        if (count < max) {
            sent[count] = control;
        }
        ++count;
        SDL_Xbox360Acc_WiredDone(wired, now, true);
    }
    return count;
}

/* Starts a wired chatpad at bcdDevice 0x0114 and runs it to step 9, every
 * transfer completing when it is sent. Returns the time step 9 completed. */
static uint64_t WiredReady(SDL_Xbox360AccWired *wired, uint64_t start)
{
    SDL_Xbox360AccControl sent[16];

    SDL_Xbox360Acc_WiredStart(wired, 0x0114, start);
    CHECK(WiredPump(wired, start, sent, 16) == 6);
    CHECK(WiredPump(wired, start + 1000, sent, 16) == 1);
    CHECK(WiredPump(wired, start + 2000, sent, 16) == 2);
    CHECK(IsCommand(&sent[0], 0x1E) && IsCommand(&sent[1], 0x1B));
    CHECK(SDL_Xbox360Acc_WiredReading(wired));
    return start + 2000;
}

static void TestIdentity(void)
{
    int code, i;
    int seen[SDL_XBOX360ACC_CHATPAD_BUTTONS];

    /* 045E:028E at the two revisions xboxdrv accepts */
    CHECK(SDL_Xbox360Acc_WiredSupported(0x045E, 0x028E, 0x0110));
    CHECK(SDL_Xbox360Acc_WiredSupported(0x045E, 0x028E, 0x0114));
    CHECK(!SDL_Xbox360Acc_WiredSupported(0x045E, 0x028E, 0x0113));
    CHECK(!SDL_Xbox360Acc_WiredSupported(0x045E, 0x028E, 0x0115));
    CHECK(!SDL_Xbox360Acc_WiredSupported(0x045E, 0x028E, 0x0100));
    CHECK(!SDL_Xbox360Acc_WiredSupported(0x045E, 0x028F, 0x0114));
    CHECK(!SDL_Xbox360Acc_WiredSupported(0x045E, 0x0719, 0x0110));
    CHECK(!SDL_Xbox360Acc_WiredSupported(0x0E6F, 0x028E, 0x0114));

    /* The receiver IDs the receiver driver takes as the 0719 receiver */
    CHECK(SDL_Xbox360Acc_ReceiverSupported(0x045E, 0x0719));
    CHECK(SDL_Xbox360Acc_ReceiverSupported(0x045E, 0x0291));
    CHECK(SDL_Xbox360Acc_ReceiverSupported(0x045E, 0x02A9));
    CHECK(!SDL_Xbox360Acc_ReceiverSupported(0x045E, 0x02A0));
    CHECK(!SDL_Xbox360Acc_ReceiverSupported(0x045E, 0x028E));
    CHECK(!SDL_Xbox360Acc_ReceiverSupported(0x045E, 0x0000));
    CHECK(!SDL_Xbox360Acc_ReceiverSupported(0x0E6F, 0x0719));
    CHECK(!SDL_Xbox360Acc_ReceiverSupported(0x045F, 0x0291));

    /* The chatpad interface: 2, FF/5D/02 (Javan Cook's lsusb of a 1.14 pad
       and [MS-XUSBI] 3.4.1.1.1) */
    CHECK(SDL_Xbox360Acc_IsChatpadInterface(2, 0xFF, 0x5D, 0x02));
    CHECK(!SDL_Xbox360Acc_IsChatpadInterface(0, 0xFF, 0x5D, 0x01));
    CHECK(!SDL_Xbox360Acc_IsChatpadInterface(1, 0xFF, 0x5D, 0x03));
    CHECK(!SDL_Xbox360Acc_IsChatpadInterface(3, 0xFF, 0xFD, 0x13));
    CHECK(!SDL_Xbox360Acc_IsChatpadInterface(2, 0xFE, 0x5D, 0x02));
    CHECK(!SDL_Xbox360Acc_IsChatpadInterface(2, 0xFF, 0x5C, 0x02));
    CHECK(!SDL_Xbox360Acc_IsChatpadInterface(2, 0xFF, 0x5D, 0x01));
    CHECK(!SDL_Xbox360Acc_IsChatpadInterface(2, 0xFF, 0x5D, 0x03));
    CHECK(!SDL_Xbox360Acc_IsChatpadInterface(3, 0xFF, 0x5D, 0x02));
    CHECK(!SDL_Xbox360Acc_IsChatpadInterface(1, 0xFF, 0x5D, 0x02));

    /* The key table, both ways: the 43 codes name buttons 4 to 46 once
       each, and no other code names a button */
    memset(seen, 0, sizeof(seen));
    for (code = 0; code < 256; ++code) {
        int button = SDL_Xbox360Acc_ChatpadButton((uint8_t)code);
        int expected = -1;

        for (i = 0; i < NUM_KEYS; ++i) {
            if (keys[i].code == code) {
                expected = keys[i].button;
            }
        }
        if (button != expected) {
            printf("  code 0x%02x: button %d, expected %d\n", code, button, expected);
        }
        CHECK(button == expected);
        if (button >= 0 && button < SDL_XBOX360ACC_CHATPAD_BUTTONS) {
            ++seen[button];
        }
    }
    for (i = 0; i < SDL_XBOX360ACC_CHATPAD_BUTTONS; ++i) {
        CHECK(seen[i] == (i >= 4 ? 1 : 0));
    }

    /* The GUID bytes that keep the two joysticks off the gamepad mappings */
    CHECK(SDL_XBOX360ACC_GUID_CHATPAD == 0x82 && SDL_XBOX360ACC_GUID_UDRAW == 0xA3);
    CHECK(SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x028E, 0x82));
    CHECK(SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x0719, 0x82));
    CHECK(SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x02A9, 0x82));
    CHECK(SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x0291, 0x82));
    CHECK(SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x0719, 0xA3));
    CHECK(SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x02A9, 0xA3));
    CHECK(SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x0291, 0xA3));
    CHECK(!SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x028E, 0xA3));  /* No tablet on a wired pad */
    CHECK(!SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x02A0, 0x82));  /* The Big Button receiver */
    CHECK(!SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x0719, 0x23));  /* A slot's own subtype byte */
    CHECK(!SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x028E, 0x01));
    CHECK(!SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x0719, 0x02));
    CHECK(!SDL_Xbox360Acc_IsAccessoryGUID(0x057E, 0x0306, 0x82));
    CHECK(!SDL_Xbox360Acc_IsAccessoryGUID(0x045F, 0x0719, 0xA3));
    CHECK(!SDL_Xbox360Acc_IsAccessoryGUID(0x045F, 0x028E, 0x82));
    CHECK(!SDL_Xbox360Acc_IsAccessoryGUID(0x045F, 0x0719, 0x82));
    for (code = 0; code < 256; ++code) {
        const bool chatpad = (code == 0x82);
        const bool tablet = (code == 0xA3);
        CHECK(SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x028E, (uint8_t)code) == chatpad);
        CHECK(SDL_Xbox360Acc_IsAccessoryGUID(0x045E, 0x0719, (uint8_t)code) == (chatpad || tablet));
    }

    CHECK(strcmp(SDL_XBOX360ACC_CHATPAD_NAME, "Xbox 360 Chatpad") == 0);
    CHECK(strcmp(SDL_XBOX360ACC_UDRAW_NAME, "Xbox 360 uDraw GameTablet") == 0);
}

static void TestEndpoint(void)
{
    SDL_Xbox360AccEndpoint endpoints[3];
    uint16_t size = 0;

    /* Javan Cook's 1.14 pad: interface 2 holds interrupt IN 0x84, 32 bytes */
    endpoints[0].address = 0x84;
    endpoints[0].attributes = 0x03;
    endpoints[0].max_packet_size = 32;
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 1, &size) == 0x84 && size == 32);
    /* [MS-XUSBI] 3.4.1.1.3 shows 0x87, and the address comes from the
       descriptor whatever it is */
    endpoints[0].address = 0x87;
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 1, &size) == 0x87);
    endpoints[0].address = 0x86; /* xboxdrv's endpoint 6 on a 1.10 pad */
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 1, &size) == 0x86);
    /* An OUT endpoint, a bulk and an isochronous one are passed over */
    endpoints[0].address = 0x04;
    endpoints[0].attributes = 0x03;
    endpoints[0].max_packet_size = 32;
    endpoints[1].address = 0x85;
    endpoints[1].attributes = 0x02;
    endpoints[1].max_packet_size = 32;
    endpoints[2].address = 0x83;
    endpoints[2].attributes = 0x01;
    endpoints[2].max_packet_size = 32;
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 3, &size) == 0);
    endpoints[2].attributes = 0x03;
    size = 0;
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 3, &size) == 0x83 && size == 32);
    /* A count that stops short of it */
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 2, &size) == 0);
    /* The packet size: bits 0 to 10, 5 to 64 bytes */
    endpoints[0].address = 0x84;
    endpoints[0].attributes = 0x03;
    endpoints[0].max_packet_size = 0x0820;
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 1, &size) == 0x84 && size == 32);
    endpoints[0].max_packet_size = 5;
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 1, &size) == 0x84 && size == 5);
    endpoints[0].max_packet_size = 4;
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 1, &size) == 0);
    endpoints[0].max_packet_size = 64;
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 1, &size) == 0x84 && size == 64);
    endpoints[0].max_packet_size = 65;
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 1, &size) == 0);
    endpoints[0].max_packet_size = 0;
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 1, &size) == 0);
    /* The first interrupt IN endpoint decides even when a later one would
       fit */
    endpoints[0].max_packet_size = 128;
    endpoints[1].address = 0x85;
    endpoints[1].attributes = 0x03;
    endpoints[1].max_packet_size = 32;
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 2, &size) == 0);
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(NULL, 1, &size) == 0);
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 1, NULL) == 0);
    CHECK(SDL_Xbox360Acc_ChatpadEndpoint(endpoints, 0, &size) == 0);
}

/* Tests 1 and 2: held keys and a held modifier */
static void TestWiredKeys(void)
{
    SDL_Xbox360AccWired *wired = NewWired();
    static const uint8_t a[] = { 0x00, 0x00, 0x37, 0x00, 0x00 };
    static const uint8_t as[] = { 0x00, 0x00, 0x37, 0x36, 0x00 };
    static const uint8_t s[] = { 0x00, 0x00, 0x36, 0x00, 0x00 };
    static const uint8_t none[] = { 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t shift[] = { 0x00, 0x01, 0x00, 0x00, 0x00 };
    static const uint8_t shift_a[] = { 0x00, 0x01, 0x37, 0x00, 0x00 };

    WiredReady(wired, 5000);
    CHECK(!wired->chatpad.present && wired->chatpad.buttons == 0);

    WiredReport(wired, a, sizeof(a));
    CHECK(wired->chatpad.present && wired->chatpad.buttons == BIT(24));
    WiredReport(wired, as, sizeof(as));
    CHECK(wired->chatpad.buttons == (BIT(24) | BIT(25)));
    WiredReport(wired, s, sizeof(s));
    CHECK(wired->chatpad.buttons == BIT(25));
    WiredReport(wired, none, sizeof(none));
    CHECK(wired->chatpad.present && wired->chatpad.buttons == 0);

    WiredReport(wired, shift, sizeof(shift));
    CHECK(wired->chatpad.buttons == BIT(SDL_XBOX360ACC_BUTTON_SHIFT));
    WiredReport(wired, shift_a, sizeof(shift_a));
    CHECK(wired->chatpad.buttons == (BIT(SDL_XBOX360ACC_BUTTON_SHIFT) | BIT(24)));
    WiredReport(wired, shift, sizeof(shift));
    CHECK(wired->chatpad.buttons == BIT(SDL_XBOX360ACC_BUTTON_SHIFT));
    WiredReport(wired, none, sizeof(none));
    CHECK(wired->chatpad.buttons == 0);
    free(wired);
}

/* Tests 3 and 4: every code alone, in either key byte, and the modifier
 * byte */
static void TestWiredCodes(void)
{
    SDL_Xbox360AccWired *wired = NewWired();
    uint8_t report[5] = { 0x00, 0x00, 0x34, 0x00, 0x00 };
    static const uint8_t unused[] = { 0x10, 0x18, 0x56, 0xFF };
    int i, m;

    WiredReady(wired, 100);
    WiredReport(wired, report, sizeof(report));
    CHECK(wired->chatpad.buttons == BIT(27)); /* F */

    for (i = 0; i < NUM_KEYS; ++i) {
        memset(report, 0, sizeof(report));
        report[2] = keys[i].code;
        WiredReport(wired, report, sizeof(report));
        if (wired->chatpad.buttons != BIT(keys[i].button)) {
            printf("  key %s in key 1\n", keys[i].legend);
        }
        CHECK(wired->chatpad.buttons == BIT(keys[i].button));
        report[2] = 0x00;
        report[3] = keys[i].code;
        WiredReport(wired, report, sizeof(report));
        if (wired->chatpad.buttons != BIT(keys[i].button)) {
            printf("  key %s in key 2\n", keys[i].legend);
        }
        CHECK(wired->chatpad.buttons == BIT(keys[i].button));
    }
    for (i = 0; i < (int)sizeof(unused); ++i) {
        memset(report, 0, sizeof(report));
        report[2] = unused[i];
        WiredReport(wired, report, sizeof(report));
        CHECK(wired->chatpad.present && wired->chatpad.buttons == 0);
        report[2] = 0x37;
        report[3] = unused[i];
        WiredReport(wired, report, sizeof(report));
        CHECK(wired->chatpad.buttons == BIT(24));
    }

    /* The modifier byte: bits 0 to 3 are buttons 0 to 3, bits 4 to 7 change
       nothing */
    memset(report, 0, sizeof(report));
    report[1] = 0x01;
    WiredReport(wired, report, sizeof(report));
    CHECK(wired->chatpad.buttons == BIT(0));
    report[1] = 0x02;
    WiredReport(wired, report, sizeof(report));
    CHECK(wired->chatpad.buttons == BIT(1));
    report[1] = 0x04;
    WiredReport(wired, report, sizeof(report));
    CHECK(wired->chatpad.buttons == BIT(2));
    report[1] = 0x08;
    WiredReport(wired, report, sizeof(report));
    CHECK(wired->chatpad.buttons == BIT(3));
    report[1] = 0x0F;
    WiredReport(wired, report, sizeof(report));
    CHECK(wired->chatpad.buttons == (BIT(0) | BIT(1) | BIT(2) | BIT(3)));
    for (m = 0; m < 256; ++m) {
        report[1] = (uint8_t)m;
        report[2] = 0x21; /* U */
        WiredReport(wired, report, sizeof(report));
        CHECK(wired->chatpad.buttons == ((uint64_t)(m & 0x0F) | BIT(20)));
    }
    free(wired);
}

/* Tests 5, 6 and 7: the status, truncations and other first bytes */
static void TestWiredStatus(void)
{
    SDL_Xbox360AccWired *wired = NewWired();
    SDL_Xbox360AccWired before;
    SDL_Xbox360AccControl sent[8];
    static const uint8_t status[] = { 0xF0, 0x04, 0xA0, 0x0E, 0x0E };
    static const uint8_t handshake[] = { 0xF0, 0x03, 0x00, 0x00, 0x00 };
    static const uint8_t f[] = { 0x00, 0x00, 0x34, 0x00, 0x00 };
    static const uint8_t as[] = { 0x00, 0x00, 0x37, 0x36, 0x00 };
    uint8_t read32[32];
    uint64_t now;
    size_t n;
    int b;

    now = WiredReady(wired, 0);

    /* F0 04 connects the joystick and holds nothing */
    WiredReport(wired, status, sizeof(status));
    CHECK(wired->chatpad.present && wired->chatpad.buttons == 0);
    WiredReport(wired, f, sizeof(f));
    CHECK(wired->chatpad.buttons == BIT(27));
    WiredReport(wired, status, sizeof(status));
    CHECK(wired->chatpad.present && wired->chatpad.buttons == BIT(27));

    /* F0 03 queues one 1B, at once and without moving the keep-alives */
    CHECK(WiredPump(wired, now, sent, 8) == 0);
    WiredReport(wired, handshake, sizeof(handshake));
    WiredReport(wired, handshake, sizeof(handshake));
    CHECK(wired->chatpad.buttons == BIT(27));
    CHECK(WiredPump(wired, now + 10, sent, 8) == 1 && IsCommand(&sent[0], 0x1B));
    CHECK(WiredPump(wired, now + 10, sent, 8) == 0);
    CHECK(WiredPump(wired, now + 999, sent, 8) == 0);
    CHECK(WiredPump(wired, now + 1000, sent, 8) == 1 && IsCommand(&sent[0], 0x1F));

    /* The 1B waits for a transfer in flight, then goes before a due
       keep-alive */
    {
        SDL_Xbox360AccControl control;

        CHECK(SDL_Xbox360Acc_WiredNext(wired, now + 2000, &control) && IsCommand(&control, 0x1E));
        WiredReport(wired, handshake, sizeof(handshake));
        CHECK(!SDL_Xbox360Acc_WiredNext(wired, now + 2000, &control));
        SDL_Xbox360Acc_WiredDone(wired, now + 2000, true);
        CHECK(SDL_Xbox360Acc_WiredNext(wired, now + 3000, &control) && IsCommand(&control, 0x1B));
        SDL_Xbox360Acc_WiredDone(wired, now + 3000, true);
        CHECK(SDL_Xbox360Acc_WiredNext(wired, now + 3000, &control) && IsCommand(&control, 0x1F));
        SDL_Xbox360Acc_WiredDone(wired, now + 3000, true);
        now += 3000;
    }

    /* Every truncation changes nothing, with the rest of another report
       past the length */
    WiredReport(wired, f, sizeof(f));
    for (n = 0; n < 5; ++n) {
        before = *wired;
        WiredReport(wired, as, n);
        CHECK(SameWired(wired, &before));
        SDL_Xbox360Acc_WiredChatpadReport(wired, as, n);
        CHECK(SameWired(wired, &before));
        WiredReport(wired, handshake, n);
        SDL_Xbox360Acc_WiredChatpadReport(wired, handshake, n);
        CHECK(SameWired(wired, &before));
    }
    /* A 32-byte read decodes its first 5 bytes */
    memset(read32, 0xFF, sizeof(read32));
    memcpy(read32, as, sizeof(as));
    WiredReport(wired, read32, sizeof(read32));
    CHECK(wired->chatpad.buttons == (BIT(24) | BIT(25)));
    memcpy(read32, handshake, sizeof(handshake));
    WiredReport(wired, read32, sizeof(read32));
    CHECK(wired->send_1b && wired->chatpad.buttons == (BIT(24) | BIT(25)));
    CHECK(WiredPump(wired, now, sent, 8) == 1 && IsCommand(&sent[0], 0x1B));

    /* Any first byte but 00 and F0 changes nothing */
    for (b = 0; b < 256; ++b) {
        uint8_t other[5] = { 0, 0x01, 0x37, 0x36, 0x00 };

        if (b == 0x00 || b == 0xF0) {
            continue;
        }
        other[0] = (uint8_t)b;
        before = *wired;
        WiredReport(wired, other, sizeof(other));
        CHECK(SameWired(wired, &before));
        other[1] = 0x03;
        WiredReport(wired, other, sizeof(other));
        CHECK(SameWired(wired, &before));
    }
    /* Nor does anything connect a chatpad that is not there yet */
    WiredReady(wired, now);
    for (b = 0; b < 256; ++b) {
        uint8_t other[5] = { 0, 0x00, 0x37, 0x00, 0x00 };

        if (b == 0x00 || b == 0xF0) {
            continue;
        }
        other[0] = (uint8_t)b;
        WiredReport(wired, other, sizeof(other));
        CHECK(!wired->chatpad.present);
    }
    /* F0 with any status kind connects it, and only 03 asks for a 1B */
    for (b = 0; b < 256; ++b) {
        uint8_t other[5] = { 0xF0, 0, 0x00, 0x00, 0x00 };

        WiredReady(wired, now);
        other[1] = (uint8_t)b;
        WiredReport(wired, other, sizeof(other));
        CHECK(wired->chatpad.present && wired->chatpad.buttons == 0);
        CHECK(wired->send_1b == (b == 0x03));
    }
    free(wired);
}

/* Test 8: the start-up and the keep-alives on an injected clock */
static void TestWiredSchedule(uint16_t bcd_device)
{
    SDL_Xbox360AccWired *wired = NewWired();
    SDL_Xbox360AccControl control;
    const uint64_t start = 70000;
    uint64_t t;
    int i;

    SDL_Xbox360Acc_WiredStart(wired, bcd_device, start);
    CHECK(!SDL_Xbox360Acc_WiredReading(wired));
    CHECK(!SDL_Xbox360Acc_WiredNext(wired, start - 1, &control));

    /* Steps 1 to 6 in order, each after the previous one completes. The
       first three stall on real pads and the sequence goes on. */
    CHECK(SDL_Xbox360Acc_WiredNext(wired, start, &control) && IsControl(&control, 0x40, 0xA9, 0xA30C, 0x4423, 0));
    CHECK(!SDL_Xbox360Acc_WiredNext(wired, start + 5000, &control));
    SDL_Xbox360Acc_WiredDone(wired, start + 1, false);
    CHECK(SDL_Xbox360Acc_WiredNext(wired, start + 1, &control) && IsControl(&control, 0x40, 0xA9, 0x2344, 0x7F03, 0));
    SDL_Xbox360Acc_WiredDone(wired, start + 2, false);
    CHECK(SDL_Xbox360Acc_WiredNext(wired, start + 2, &control) && IsControl(&control, 0x40, 0xA9, 0x5839, 0x6832, 0));
    SDL_Xbox360Acc_WiredDone(wired, start + 3, false);
    CHECK(SDL_Xbox360Acc_WiredNext(wired, start + 3, &control) && IsControl(&control, 0xC0, 0xA1, 0x0000, 0xE416, 2));
    SDL_Xbox360Acc_WiredDone(wired, start + 4, true);
    CHECK(SDL_Xbox360Acc_WiredNext(wired, start + 4, &control) && IsControl(&control, 0x40, 0xA1, 0x0000, 0xE416, 2));
    if (bcd_device == 0x0114) {
        CHECK(control.data[0] == 0x09 && control.data[1] == 0x00);
    } else {
        CHECK(control.data[0] == 0x01 && control.data[1] == 0x02);
    }
    SDL_Xbox360Acc_WiredDone(wired, start + 5, false);
    CHECK(SDL_Xbox360Acc_WiredNext(wired, start + 5, &control) && IsControl(&control, 0xC0, 0xA1, 0x0000, 0xE416, 2));
    CHECK(!SDL_Xbox360Acc_WiredReading(wired));
    /* t: step 6 completes */
    t = start + 9;
    SDL_Xbox360Acc_WiredDone(wired, t, true);

    /* 1F at t + 1000, 1E at t + 2000, 1B right after */
    CHECK(!SDL_Xbox360Acc_WiredNext(wired, t + 999, &control));
    CHECK(SDL_Xbox360Acc_WiredNext(wired, t + 1000, &control) && IsCommand(&control, 0x001F));
    SDL_Xbox360Acc_WiredDone(wired, t + 1000, false);
    CHECK(!SDL_Xbox360Acc_WiredNext(wired, t + 1999, &control));
    CHECK(SDL_Xbox360Acc_WiredNext(wired, t + 2000, &control) && IsCommand(&control, 0x001E));
    SDL_Xbox360Acc_WiredDone(wired, t + 2000, true);
    CHECK(!SDL_Xbox360Acc_WiredReading(wired));
    CHECK(SDL_Xbox360Acc_WiredNext(wired, t + 2000, &control) && IsCommand(&control, 0x001B));
    CHECK(!SDL_Xbox360Acc_WiredReading(wired));
    SDL_Xbox360Acc_WiredDone(wired, t + 2000, false);
    CHECK(SDL_Xbox360Acc_WiredReading(wired));

    /* Then 1F and 1E in turn, every 1000 ms */
    for (i = 0; i < 6; ++i) {
        const uint64_t due = t + 3000 + (uint64_t)i * 1000;

        CHECK(!SDL_Xbox360Acc_WiredNext(wired, due - 1, &control));
        CHECK(SDL_Xbox360Acc_WiredNext(wired, due, &control) && IsCommand(&control, (i % 2) ? 0x001E : 0x001F));
        SDL_Xbox360Acc_WiredDone(wired, due, (i % 3) != 0);
    }
    /* Each keep-alive waits 1000 ms from the one before completing */
    t += 9000;
    CHECK(SDL_Xbox360Acc_WiredNext(wired, t, &control) && IsCommand(&control, 0x001F));
    SDL_Xbox360Acc_WiredDone(wired, t + 40, true);
    CHECK(!SDL_Xbox360Acc_WiredNext(wired, t + 1039, &control));
    CHECK(SDL_Xbox360Acc_WiredNext(wired, t + 1040, &control) && IsCommand(&control, 0x001E));
    SDL_Xbox360Acc_WiredDone(wired, t + 1040, true);
    /* A late update sends one keep-alive, not the ones it missed */
    CHECK(SDL_Xbox360Acc_WiredNext(wired, t + 9000, &control) && IsCommand(&control, 0x001F));
    SDL_Xbox360Acc_WiredDone(wired, t + 9000, true);
    CHECK(!SDL_Xbox360Acc_WiredNext(wired, t + 9999, &control));
    CHECK(SDL_Xbox360Acc_WiredNext(wired, t + 10000, &control) && IsCommand(&control, 0x001E));
    SDL_Xbox360Acc_WiredDone(wired, t + 10000, true);

    /* A completion with nothing in flight changes nothing */
    {
        SDL_Xbox360AccWired before = *wired;
        SDL_Xbox360Acc_WiredDone(wired, t + 10500, true);
        CHECK(SameWired(wired, &before));
    }
    free(wired);
}

/* Test 9: nothing before step 9, and the pad's own status */
static void TestWiredPresence(void)
{
    SDL_Xbox360AccWired *wired = NewWired();
    SDL_Xbox360AccControl control;
    SDL_Xbox360AccWired before;
    static const uint8_t a[] = { 0x00, 0x00, 0x37, 0x00, 0x00 };
    static const uint8_t status[] = { 0xF0, 0x04, 0xA0, 0x0E, 0x0E };
    static const uint8_t handshake[] = { 0xF0, 0x03, 0x00, 0x00, 0x00 };
    static const uint8_t tid[] = { 0x08, 0x03, 0x01 };
    static const uint8_t tid_voice[] = { 0x08, 0x03, 0x03 };
    static const uint8_t none[] = { 0x08, 0x03, 0x00 };
    static const uint8_t voice[] = { 0x08, 0x03, 0x02 };
    uint64_t now = 0;
    int step;
    size_t n;
    int b;

    /* Before step 9 completes no report counts, however far the start-up
       has gone */
    SDL_Xbox360Acc_WiredStart(wired, 0x0110, now);
    for (step = 0; step < 9; ++step) {
        WiredReport(wired, a, sizeof(a));
        WiredReport(wired, status, sizeof(status));
        WiredReport(wired, handshake, sizeof(handshake));
        CHECK(!wired->chatpad.present && wired->chatpad.buttons == 0 && !wired->send_1b);
        now += 1000;
        CHECK(SDL_Xbox360Acc_WiredNext(wired, now, &control));
        WiredReport(wired, a, sizeof(a));
        CHECK(!wired->chatpad.present);
        SDL_Xbox360Acc_WiredDone(wired, now, true);
    }
    CHECK(SDL_Xbox360Acc_WiredReading(wired));
    WiredReport(wired, a, sizeof(a));
    CHECK(wired->chatpad.present && wired->chatpad.buttons == BIT(24));

    /* 08 03 01 and 08 03 03 change nothing */
    before = *wired;
    PadReport(wired, tid, sizeof(tid));
    CHECK(SameWired(wired, &before));
    PadReport(wired, tid_voice, sizeof(tid_voice));
    CHECK(SameWired(wired, &before));
    /* A truncated 08 03 00 changes nothing, with the rest past the length */
    for (n = 0; n < 3; ++n) {
        PadReport(wired, none, n);
        SDL_Xbox360Acc_WiredPadReport(wired, none, n);
        CHECK(SameWired(wired, &before));
    }
    /* Other pad reports change nothing: the state report and any other ID or
       size */
    for (b = 0; b < 256; ++b) {
        uint8_t other[3] = { 0, 0x03, 0x00 };

        if (b == 0x08) {
            continue;
        }
        other[0] = (uint8_t)b;
        PadReport(wired, other, sizeof(other));
        CHECK(SameWired(wired, &before));
    }
    for (b = 0; b < 256; ++b) {
        uint8_t other[3] = { 0x08, 0, 0x00 };

        if (b == 0x03) {
            continue;
        }
        other[1] = (uint8_t)b;
        PadReport(wired, other, sizeof(other));
        CHECK(SameWired(wired, &before));
    }
    /* 08 03 00 disconnects the chatpad joystick, and so does 08 03 02 */
    PadReport(wired, none, sizeof(none));
    CHECK(!wired->chatpad.present && wired->chatpad.buttons == 0);
    CHECK(SDL_Xbox360Acc_WiredReading(wired));
    WiredReport(wired, status, sizeof(status));
    CHECK(wired->chatpad.present && wired->chatpad.buttons == 0);
    WiredReport(wired, a, sizeof(a));
    PadReport(wired, voice, sizeof(voice));
    CHECK(!wired->chatpad.present && wired->chatpad.buttons == 0);
    /* A longer 08 03 00 counts too */
    WiredReport(wired, a, sizeof(a));
    {
        static const uint8_t longer[] = { 0x08, 0x03, 0x00, 0x55, 0x55 };
        PadReport(wired, longer, sizeof(longer));
        CHECK(!wired->chatpad.present);
    }
    free(wired);
}

/* Test 10: unplug and replug */
static void TestWiredUnplug(void)
{
    SDL_Xbox360AccWired *wired = NewWired();
    SDL_Xbox360AccControl control;
    static const uint8_t a[] = { 0x00, 0x00, 0x37, 0x00, 0x00 };
    uint64_t now;

    now = WiredReady(wired, 1000);
    WiredReport(wired, a, sizeof(a));
    CHECK(wired->chatpad.present);
    CHECK(SDL_Xbox360Acc_WiredNext(wired, now + 1000, &control));

    /* The pad goes, with a transfer in flight: the chatpad goes with it and
       nothing more is sent */
    SDL_Xbox360Acc_WiredStop(wired);
    CHECK(!wired->chatpad.present && wired->chatpad.buttons == 0);
    CHECK(!SDL_Xbox360Acc_WiredReading(wired));
    CHECK(!SDL_Xbox360Acc_WiredNext(wired, now + 1000, &control));
    CHECK(!SDL_Xbox360Acc_WiredNext(wired, now + 100000, &control));
    SDL_Xbox360Acc_WiredDone(wired, now + 100000, true);
    CHECK(!SDL_Xbox360Acc_WiredNext(wired, now + 200000, &control));
    WiredReport(wired, a, sizeof(a));
    CHECK(!wired->chatpad.present);
    {
        static const uint8_t handshake[] = { 0xF0, 0x03, 0x00, 0x00, 0x00 };
        WiredReport(wired, handshake, sizeof(handshake));
        CHECK(!wired->send_1b && !SDL_Xbox360Acc_WiredNext(wired, now + 200000, &control));
    }

    /* Plugged again: the start-up begins at step 1 */
    SDL_Xbox360Acc_WiredStart(wired, 0x0110, now + 300000);
    CHECK(SDL_Xbox360Acc_WiredNext(wired, now + 300000, &control) && IsControl(&control, 0x40, 0xA9, 0xA30C, 0x4423, 0));
    CHECK(!wired->chatpad.present);
    free(wired);
}

/* Test 11: the lamps */
static void TestWiredLamps(void)
{
    SDL_Xbox360AccControl control;
    uint8_t setup[8];
    uint8_t effect[2] = { 0x08, 0x00 };
    int b;

    CHECK(SDL_Xbox360Acc_WiredLamp(effect, 1, &control));
    SDL_Xbox360Acc_ControlSetup(&control, setup);
    CHECK(setup[0] == 0x41 && setup[1] == 0x00 && setup[2] == 0x08 && setup[3] == 0x00 &&
          setup[4] == 0x02 && setup[5] == 0x00 && setup[6] == 0x00 && setup[7] == 0x00);
    effect[0] = 0x05;
    CHECK(!SDL_Xbox360Acc_WiredLamp(effect, 1, &control));
    effect[0] = 0x08;
    CHECK(!SDL_Xbox360Acc_WiredLamp(effect, 2, &control));
    CHECK(!SDL_Xbox360Acc_WiredLamp(effect, 0, &control));
    CHECK(!SDL_Xbox360Acc_WiredLamp(NULL, 1, &control));
    CHECK(!SDL_Xbox360Acc_WiredLamp(effect, 1, NULL));
    for (b = 0; b < 256; ++b) {
        const bool valid = (b <= 0x04) || (b >= 0x08 && b <= 0x0C);

        effect[0] = (uint8_t)b;
        memset(&control, 0xEE, sizeof(control));
        CHECK(SDL_Xbox360Acc_WiredLamp(effect, 1, &control) == valid);
        CHECK(SDL_Xbox360Acc_ChatpadLampValid((uint8_t)b) == valid);
        if (valid) {
            CHECK(IsCommand(&control, (uint16_t)b));
        }
    }

    /* The setup packet's byte order on every field */
    control.request_type = 0xC0;
    control.request = 0xA1;
    control.value = 0xA30C;
    control.index = 0xE416;
    control.length = 0x0102;
    SDL_Xbox360Acc_ControlSetup(&control, setup);
    CHECK(setup[0] == 0xC0 && setup[1] == 0xA1 && setup[2] == 0x0C && setup[3] == 0xA3 &&
          setup[4] == 0x16 && setup[5] == 0xE4 && setup[6] == 0x02 && setup[7] == 0x01);
}

/* The wireless chatpad */

static SDL_Xbox360AccSlot *NewSlots(int count, bool chatpad, bool udraw)
{
    SDL_Xbox360AccSlot *slots = (SDL_Xbox360AccSlot *)malloc(sizeof(*slots) * (size_t)count);
    int i;

    if (!slots) {
        printf("FAIL: out of memory\n");
        exit(1);
    }
    memset(slots, 0xA5, sizeof(*slots) * (size_t)count);
    for (i = 0; i < count; ++i) {
        SDL_Xbox360Acc_SlotInit(&slots[i], chatpad, udraw);
    }
    return slots;
}

static int Feed(SDL_Xbox360AccSlot *slot, uint64_t now, const uint8_t *bytes, size_t length)
{
    uint8_t *copy = Copy(bytes, length);
    int result = SDL_Xbox360Acc_SlotPacket(slot, now, copy, length);
    free(copy);
    return result;
}

static void Status(uint8_t out[2], bool device)
{
    out[0] = 0x08;
    out[1] = device ? 0x80 : 0x00;
}

/* A receiver packet of type 0x02: 00 02 00 F0, 20 zero bytes, the four
 * plug-in bytes and one zero byte */
static void ChatpadPacket(uint8_t out[29], uint8_t b24, uint8_t b25, uint8_t b26, uint8_t b27)
{
    memset(out, 0, 29);
    out[1] = 0x02;
    out[3] = 0xF0;
    out[24] = b24;
    out[25] = b25;
    out[26] = b26;
    out[27] = b27;
}

/* The link control packet: 00 0F 00 F0, zeros but the subtype in byte 25 */
static void InfoPacket(uint8_t out[29], uint8_t subtype)
{
    memset(out, 0, 29);
    out[1] = 0x0F;
    out[3] = 0xF0;
    out[25] = subtype;
}

/* The pad state packet at rest, which a tablet's packets share */
static void PadPacket(uint8_t out[29])
{
    memset(out, 0, 29);
    out[1] = 0x01;
    out[3] = 0xF0;
    out[5] = 0x13;
}

/* Takes every queued command, checking each packet's framing */
static int Take(SDL_Xbox360AccSlot *slot, uint8_t *commands, int max)
{
    uint8_t packet[SDL_XBOX360ACC_COMMAND_SIZE];
    int count = 0;
    int i;

    while (count < 64 && SDL_Xbox360Acc_SlotTakeCommand(slot, packet)) {
        bool zeros = true;

        for (i = 4; i < SDL_XBOX360ACC_COMMAND_SIZE; ++i) {
            if (packet[i] != 0x00) {
                zeros = false;
            }
        }
        CHECK(packet[0] == 0x00 && packet[1] == 0x00 && packet[2] == 0x0C && zeros);
        if (count < max) {
            commands[count] = packet[3];
        }
        ++count;
    }
    return count;
}

/* Runs the slot's clock from one time to another, 1 ms at a time, and
 * returns each command with the time it went out */
static int Run(SDL_Xbox360AccSlot *slot, uint64_t from, uint64_t to, uint8_t *commands, uint64_t *times, int max)
{
    uint64_t now;
    int count = 0;

    for (now = from; now <= to; ++now) {
        uint8_t taken[16];
        int n, i;

        SDL_Xbox360Acc_SlotUpdate(slot, now);
        n = Take(slot, taken, 16);
        for (i = 0; i < n && i < 16; ++i) {
            if (count < max) {
                commands[count] = taken[i];
                times[count] = now;
            }
            ++count;
        }
    }
    return count;
}

/* Test 1: held keys in a type 0x02 packet */
static void TestWirelessKeys(void)
{
    SDL_Xbox360AccSlot *slot = NewSlots(1, true, true);
    uint8_t packet[29];
    uint8_t status[2];
    int i;

    Status(status, true);
    CHECK(Feed(slot, 0, status, 2) == SDL_XBOX360ACC_PACKET_STATUS);

    ChatpadPacket(packet, 0x00, 0x01, 0x37, 0x00);
    CHECK(Feed(slot, 10, packet, 29) == SDL_XBOX360ACC_PACKET_CHATPAD);
    CHECK(slot->chatpad.present && slot->chatpad.buttons == (BIT(SDL_XBOX360ACC_BUTTON_SHIFT) | BIT(24)));
    ChatpadPacket(packet, 0x00, 0x00, 0x00, 0x00);
    CHECK(Feed(slot, 20, packet, 29) == SDL_XBOX360ACC_PACKET_CHATPAD);
    CHECK(slot->chatpad.present && slot->chatpad.buttons == 0);

    /* The same codes and modifiers as the wired chatpad, in either key byte */
    for (i = 0; i < NUM_KEYS; ++i) {
        ChatpadPacket(packet, 0x00, 0x08, keys[i].code, 0x00);
        Feed(slot, 30, packet, 29);
        CHECK(slot->chatpad.buttons == (BIT(SDL_XBOX360ACC_BUTTON_PEOPLE) | BIT(keys[i].button)));
        ChatpadPacket(packet, 0x00, 0xF4, 0x37, keys[i].code);
        Feed(slot, 30, packet, 29);
        CHECK(slot->chatpad.buttons == (BIT(SDL_XBOX360ACC_BUTTON_ORANGE) | BIT(24) | BIT(keys[i].button)));
    }
    /* Byte 28 is not read */
    ChatpadPacket(packet, 0x00, 0x02, 0x36, 0x00);
    packet[28] = 0xFF;
    Feed(slot, 40, packet, 29);
    CHECK(slot->chatpad.buttons == (BIT(SDL_XBOX360ACC_BUTTON_GREEN) | BIT(25)));
    free(slot);
}

/* Tests 2 and 5: the schedule, the status and the handshake */
static void TestWirelessSchedule(void)
{
    SDL_Xbox360AccSlot *slot = NewSlots(1, true, true);
    uint8_t packet[29];
    uint8_t status[2];
    uint8_t commands[32];
    uint64_t times[32];
    const uint64_t t = 50000;
    int n;

    /* Nothing goes out before the slot shows a device */
    CHECK(Run(slot, 0, 5000, commands, times, 32) == 0);

    Status(status, true);
    Feed(slot, t, status, 2);
    CHECK(slot->link && slot->scheduled && slot->kind == SDL_XBOX360ACC_SLOT_NONE);
    /* 1B at t + 100, 1E at t + 1100, 1F at t + 2100, then every 1000 ms */
    n = Run(slot, t, t + 5100, commands, times, 32);
    CHECK(n == 6);
    CHECK(commands[0] == 0x1B && times[0] == t + 100);
    CHECK(commands[1] == 0x1E && times[1] == t + 1100);
    CHECK(commands[2] == 0x1F && times[2] == t + 2100);
    CHECK(commands[3] == 0x1E && times[3] == t + 3100);
    CHECK(commands[4] == 0x1F && times[4] == t + 4100);
    CHECK(commands[5] == 0x1E && times[5] == t + 5100);

    /* Another 08 80 moves nothing */
    Feed(slot, t + 5500, status, 2);
    n = Run(slot, t + 5101, t + 6100, commands, times, 32);
    CHECK(n == 1 && commands[0] == 0x1F && times[0] == t + 6100);

    /* F0 04 connects the chatpad joystick and changes no button */
    ChatpadPacket(packet, 0xF0, 0x04, 0xA0, 0x0E);
    CHECK(Feed(slot, t + 6200, packet, 29) == SDL_XBOX360ACC_PACKET_CHATPAD);
    CHECK(slot->chatpad.present && slot->chatpad.buttons == 0);
    ChatpadPacket(packet, 0x00, 0x00, 0x34, 0x00);
    Feed(slot, t + 6300, packet, 29);
    ChatpadPacket(packet, 0xF0, 0x04, 0xA0, 0x0E);
    Feed(slot, t + 6400, packet, 29);
    CHECK(slot->chatpad.present && slot->chatpad.buttons == BIT(27));

    /* F0 03: the next scheduled send is one 1B in place of the keep-alive,
       and the keep-alives go on from where they were */
    ChatpadPacket(packet, 0xF0, 0x03, 0x00, 0x00);
    Feed(slot, t + 6500, packet, 29);
    Feed(slot, t + 6600, packet, 29);
    CHECK(slot->chatpad.buttons == BIT(27));
    n = Run(slot, t + 6101, t + 9100, commands, times, 32);
    CHECK(n == 3);
    CHECK(commands[0] == 0x1B && times[0] == t + 7100);
    CHECK(commands[1] == 0x1E && times[1] == t + 8100);
    CHECK(commands[2] == 0x1F && times[2] == t + 9100);

    /* 08 00 stops the schedule and disconnects the chatpad joystick */
    Status(status, false);
    CHECK(Feed(slot, t + 9500, status, 2) == SDL_XBOX360ACC_PACKET_STATUS);
    CHECK(!slot->link && !slot->scheduled && !slot->chatpad.present && slot->chatpad.buttons == 0);
    CHECK(Run(slot, t + 9101, t + 20000, commands, times, 32) == 0);
    /* A chatpad packet on the empty slot still decodes, and sends nothing */
    ChatpadPacket(packet, 0x00, 0x00, 0x37, 0x00);
    Feed(slot, t + 20000, packet, 29);
    CHECK(Run(slot, t + 20000, t + 22000, commands, times, 32) == 0);
    Status(status, false);
    Feed(slot, t + 22000, status, 2);
    CHECK(!slot->chatpad.present);

    /* 08 80 again restarts at 1B after 100 ms, and 1E follows */
    Status(status, true);
    Feed(slot, t + 30000, status, 2);
    n = Run(slot, t + 30000, t + 31100, commands, times, 32);
    CHECK(n == 2 && commands[0] == 0x1B && times[0] == t + 30100 && commands[1] == 0x1E && times[1] == t + 31100);

    /* A late update sends one command, not the ones it missed, and the next
       comes 1000 ms after it */
    SDL_Xbox360Acc_SlotUpdate(slot, t + 36000);
    CHECK(Take(slot, commands, 32) == 1 && commands[0] == 0x1F);
    n = Run(slot, t + 36000, t + 37000, commands, times, 32);
    CHECK(n == 1 && commands[0] == 0x1E && times[0] == t + 37000);

    /* The info packet and a pad state packet start the commands as well */
    {
        uint8_t info[29];
        uint8_t pad[29];

        SDL_Xbox360Acc_SlotLost(slot);
        CHECK(!slot->scheduled && slot->kind == SDL_XBOX360ACC_SLOT_NONE);
        InfoPacket(info, 0x01);
        CHECK(Feed(slot, t + 40000, info, 29) == SDL_XBOX360ACC_PACKET_INFO);
        n = Run(slot, t + 40000, t + 40100, commands, times, 32);
        CHECK(n == 1 && commands[0] == 0x1B && times[0] == t + 40100);
        SDL_Xbox360Acc_SlotLost(slot);
        PadPacket(pad);
        CHECK(Feed(slot, t + 50000, pad, 29) == SDL_XBOX360ACC_PACKET_PAD);
        n = Run(slot, t + 50000, t + 50100, commands, times, 32);
        CHECK(n == 1 && commands[0] == 0x1B && times[0] == t + 50100);
        /* Chatpad and other packets start nothing */
        SDL_Xbox360Acc_SlotLost(slot);
        ChatpadPacket(packet, 0xF0, 0x03, 0x00, 0x00);
        Feed(slot, t + 60000, packet, 29);
        CHECK(Run(slot, t + 60000, t + 62000, commands, times, 32) == 0);
    }
    free(slot);
}

/* Tests 3 and 4: truncations, other types and framing */
static void TestWirelessPackets(void)
{
    SDL_Xbox360AccSlot *slot = NewSlots(1, true, true);
    SDL_Xbox360AccSlot before;
    uint8_t packet[29];
    uint8_t stale[32];
    uint8_t status[2];
    size_t n;
    int i, b;

    Status(status, true);
    Feed(slot, 0, status, 2);
    ChatpadPacket(packet, 0x00, 0x00, 0x34, 0x00);
    Feed(slot, 1, packet, 29);
    CHECK(slot->chatpad.buttons == BIT(27));

    /* Every truncation from 0 to 27 bytes changes nothing, with the rest of
       another report past the length */
    ChatpadPacket(stale, 0x00, 0x0F, 0x37, 0x36);
    stale[29] = stale[30] = stale[31] = 0x00;
    for (n = 0; n < 28; ++n) {
        before = *slot;
        CHECK(Feed(slot, 2, stale, n) == SDL_XBOX360ACC_PACKET_OTHER);
        CHECK(SameSlot(slot, &before));
        CHECK(SDL_Xbox360Acc_SlotPacket(slot, 2, stale, n) == SDL_XBOX360ACC_PACKET_OTHER);
        CHECK(SameSlot(slot, &before));
    }
    /* 28 bytes are enough, and a 32-byte packet decodes bytes 0 to 27 */
    CHECK(Feed(slot, 3, stale, 28) == SDL_XBOX360ACC_PACKET_CHATPAD);
    CHECK(slot->chatpad.buttons == (0x0F | BIT(24) | BIT(25)));
    memset(stale, 0xFF, sizeof(stale));
    ChatpadPacket(stale, 0x00, 0x00, 0x21, 0x00);
    memset(&stale[28], 0xFF, 4);
    CHECK(Feed(slot, 4, stale, 32) == SDL_XBOX360ACC_PACKET_CHATPAD);
    CHECK(slot->chatpad.buttons == BIT(20));

    /* Byte 1 at 0x01 or 0x00, or bytes 0, 2 and 3 other than 00 00 F0, give
       no chatpad decode */
    for (b = 0; b < 256; ++b) {
        if (b == 0x02) {
            continue;
        }
        ChatpadPacket(packet, 0x00, 0x01, 0x37, 0x36);
        packet[1] = (uint8_t)b;
        before = *slot;
        CHECK(Feed(slot, 5, packet, 29) != SDL_XBOX360ACC_PACKET_CHATPAD);
        CHECK(SameChatpad(&slot->chatpad, &before.chatpad));
    }
    for (i = 0; i < 4; ++i) {
        if (i == 1) {
            continue;
        }
        for (b = 0; b < 256; ++b) {
            ChatpadPacket(packet, 0x00, 0x01, 0x37, 0x36);
            if (packet[i] == b) {
                continue;
            }
            packet[i] = (uint8_t)b;
            before = *slot;
            CHECK(Feed(slot, 6, packet, 29) != SDL_XBOX360ACC_PACKET_CHATPAD);
            CHECK(SameChatpad(&slot->chatpad, &before.chatpad));
        }
    }
    /* Byte 1 at 0x03 decodes the pad state only */
    ChatpadPacket(packet, 0x00, 0x01, 0x37, 0x36);
    packet[1] = 0x03;
    before = *slot;
    CHECK(Feed(slot, 7, packet, 29) == SDL_XBOX360ACC_PACKET_PAD);
    CHECK(SameChatpad(&slot->chatpad, &before.chatpad));
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_PAD);

    /* A payload other than 00 and F0 changes nothing */
    for (b = 0; b < 256; ++b) {
        if (b == 0x00 || b == 0xF0) {
            continue;
        }
        ChatpadPacket(packet, (uint8_t)b, 0x03, 0x37, 0x36);
        before = *slot;
        CHECK(Feed(slot, 8, packet, 29) == SDL_XBOX360ACC_PACKET_CHATPAD);
        CHECK(SameSlot(slot, &before));
    }

    /* The other packet types the receiver sends are not the chatpad's: the
       battery report, the capabilities reply, a short status and a long one */
    {
        uint8_t battery[29];
        uint8_t caps[29];
        uint8_t status3[3] = { 0x08, 0x80, 0x00 };
        uint8_t status1[1] = { 0x08 };

        memset(battery, 0, sizeof(battery));
        battery[3] = 0x13;
        battery[4] = 0x03;
        before = *slot;
        CHECK(Feed(slot, 9, battery, 29) == SDL_XBOX360ACC_PACKET_OTHER);
        CHECK(SameSlot(slot, &before));
        memset(caps, 0, sizeof(caps));
        caps[1] = 0x05;
        caps[5] = 0x12;
        CHECK(Feed(slot, 9, caps, 29) == SDL_XBOX360ACC_PACKET_OTHER);
        CHECK(SameSlot(slot, &before));
        caps[5] = 0x13;
        CHECK(Feed(slot, 9, caps, 29) == SDL_XBOX360ACC_PACKET_PAD);
        SDL_Xbox360Acc_SlotLost(slot);
        before = *slot;
        CHECK(Feed(slot, 10, status3, 3) == SDL_XBOX360ACC_PACKET_OTHER);
        CHECK(Feed(slot, 10, status1, 1) == SDL_XBOX360ACC_PACKET_OTHER);
        CHECK(SameSlot(slot, &before));
    }
    free(slot);
}

/* Test 6: the lamps and the keep-alives */
static void TestWirelessLamps(void)
{
    SDL_Xbox360AccSlot *slot = NewSlots(1, true, true);
    uint8_t status[2];
    uint8_t commands[32];
    uint64_t times[32];
    uint8_t effect[2] = { 0x09, 0x00 };
    int n, b;

    Status(status, true);
    Feed(slot, 0, status, 2);
    CHECK(Run(slot, 0, 100, commands, times, 32) == 1);

    /* Effect 09 goes out at once, and again after each keep-alive */
    CHECK(SDL_Xbox360Acc_SlotLamp(slot, effect, 1));
    CHECK(Take(slot, commands, 32) == 1 && commands[0] == 0x09);
    n = Run(slot, 101, 2100, commands, times, 32);
    CHECK(n == 4);
    CHECK(commands[0] == 0x1E && commands[1] == 0x09 && times[1] == 1100);
    CHECK(commands[2] == 0x1F && commands[3] == 0x09 && times[3] == 2100);

    /* After effect 01 it is not */
    effect[0] = 0x01;
    CHECK(SDL_Xbox360Acc_SlotLamp(slot, effect, 1));
    CHECK(Take(slot, commands, 32) == 1 && commands[0] == 0x01);
    n = Run(slot, 2101, 4100, commands, times, 32);
    CHECK(n == 2 && commands[0] == 0x1E && commands[1] == 0x1F);

    /* Every lamp lit follows, in the order Shift, Green, Orange, People, and
       after a 1B as well */
    {
        static const uint8_t on[] = { 0x0B, 0x08, 0x0A, 0x09 };
        uint8_t packet[29];
        int i;

        for (i = 0; i < 4; ++i) {
            effect[0] = on[i];
            CHECK(SDL_Xbox360Acc_SlotLamp(slot, effect, 1));
        }
        CHECK(Take(slot, commands, 32) == 4);
        n = Run(slot, 4101, 5100, commands, times, 32);
        CHECK(n == 5 && commands[0] == 0x1E && commands[1] == 0x08 && commands[2] == 0x09 &&
              commands[3] == 0x0A && commands[4] == 0x0B);
        ChatpadPacket(packet, 0xF0, 0x03, 0x00, 0x00);
        Feed(slot, 5200, packet, 29);
        n = Run(slot, 5101, 6100, commands, times, 32);
        CHECK(n == 5 && commands[0] == 0x1B && commands[1] == 0x08 && commands[4] == 0x0B);
        /* Each off turns one lamp off */
        effect[0] = 0x00;
        SDL_Xbox360Acc_SlotLamp(slot, effect, 1);
        effect[0] = 0x03;
        SDL_Xbox360Acc_SlotLamp(slot, effect, 1);
        CHECK(Take(slot, commands, 32) == 2 && commands[0] == 0x00 && commands[1] == 0x03);
        n = Run(slot, 6101, 7100, commands, times, 32);
        CHECK(n == 3 && commands[0] == 0x1F && commands[1] == 0x09 && commands[2] == 0x0A);
        effect[0] = 0x01;
        SDL_Xbox360Acc_SlotLamp(slot, effect, 1);
        effect[0] = 0x02;
        SDL_Xbox360Acc_SlotLamp(slot, effect, 1);
        Take(slot, commands, 32);
    }

    /* The backlight's 04 and 0C go out once and are not sent again */
    CHECK(slot->lamps == 0);
    effect[0] = 0x0C;
    CHECK(SDL_Xbox360Acc_SlotLamp(slot, effect, 1));
    CHECK(slot->lamps == 0);
    effect[0] = 0x04;
    CHECK(SDL_Xbox360Acc_SlotLamp(slot, effect, 1));
    CHECK(slot->lamps == 0);
    CHECK(Take(slot, commands, 32) == 2 && commands[0] == 0x0C && commands[1] == 0x04);
    n = Run(slot, 7101, 8100, commands, times, 32);
    CHECK(n == 1 && commands[0] == 0x1E);

    /* Only the valid bytes, one at a time */
    for (b = 0; b < 256; ++b) {
        const bool valid = (b <= 0x04) || (b >= 0x08 && b <= 0x0C);

        effect[0] = (uint8_t)b;
        CHECK(SDL_Xbox360Acc_SlotLamp(slot, effect, 1) == valid);
        CHECK(Take(slot, commands, 32) == (valid ? 1 : 0));
        if (valid) {
            CHECK(commands[0] == b);
        }
    }
    effect[0] = 0x08;
    CHECK(!SDL_Xbox360Acc_SlotLamp(slot, effect, 2));
    CHECK(!SDL_Xbox360Acc_SlotLamp(slot, effect, 0));
    CHECK(!SDL_Xbox360Acc_SlotLamp(slot, NULL, 1));
    CHECK(Take(slot, commands, 32) == 0);

    /* The queue holds 16 commands in order and drops what comes after */
    for (b = 0; b < 20; ++b) {
        effect[0] = (uint8_t)((b % 2) ? 0x0C : 0x04);
        SDL_Xbox360Acc_SlotLamp(slot, effect, 1);
    }
    CHECK(slot->command_count == SDL_XBOX360ACC_COMMAND_QUEUE);
    n = Take(slot, commands, 32);
    CHECK(n == SDL_XBOX360ACC_COMMAND_QUEUE);
    for (b = 0; b < n && b < 32; ++b) {
        CHECK(commands[b] == ((b % 2) ? 0x0C : 0x04));
    }

    /* The slot empties and its lamps go dark */
    effect[0] = 0x09;
    SDL_Xbox360Acc_SlotLamp(slot, effect, 1);
    Status(status, false);
    Feed(slot, 9000, status, 2);
    CHECK(slot->lamps == 0 && Take(slot, commands, 32) == 0);
    Status(status, true);
    Feed(slot, 10000, status, 2);
    n = Run(slot, 10000, 11100, commands, times, 32);
    CHECK(n == 2 && commands[0] == 0x1B && commands[1] == 0x1E);

    /* With the chatpad off no lamp goes out */
    {
        SDL_Xbox360AccSlot *off = NewSlots(1, false, true);

        effect[0] = 0x08;
        CHECK(!SDL_Xbox360Acc_SlotLamp(off, effect, 1));
        CHECK(Take(off, commands, 32) == 0);
        free(off);
    }
    free(slot);
}

/* Test 7: four slots, four schedules, four chatpads */
static void TestWirelessSlots(void)
{
    SDL_Xbox360AccSlot *slots = NewSlots(4, true, true);
    uint8_t status[2];
    uint8_t packet[29];
    uint8_t commands[4][64];
    int counts[4];
    uint64_t now;
    int i;

    memset(counts, 0, sizeof(counts));
    Status(status, true);
    Feed(&slots[0], 0, status, 2);
    Feed(&slots[1], 250, status, 2);
    Feed(&slots[3], 700, status, 2);
    for (now = 0; now <= 4000; ++now) {
        if (now == 1500) {
            ChatpadPacket(packet, 0x00, 0x01, 0x37, 0x00);
            Feed(&slots[1], now, packet, 29);
            ChatpadPacket(packet, 0xF0, 0x03, 0x00, 0x00);
            Feed(&slots[3], now, packet, 29);
        }
        if (now == 2000) {
            Feed(&slots[2], now, status, 2);
            Status(status, false);
            Feed(&slots[0], now, status, 2);
            Status(status, true);
        }
        for (i = 0; i < 4; ++i) {
            uint8_t taken[16];
            int n, k;

            SDL_Xbox360Acc_SlotUpdate(&slots[i], now);
            n = Take(&slots[i], taken, 16);
            for (k = 0; k < n && k < 16; ++k) {
                if (counts[i] < 64) {
                    commands[i][counts[i]++] = taken[k];
                }
            }
        }
    }
    /* Slot 0: 1B at 100, 1E at 1100, then emptied at 2000 */
    CHECK(counts[0] == 2 && commands[0][0] == 0x1B && commands[0][1] == 0x1E);
    CHECK(!slots[0].scheduled && !slots[0].chatpad.present);
    /* Slot 1: 1B at 350, then 1E, 1F, 1E at 1350, 2350, 3350, and its
       chatpad holds Shift and A */
    CHECK(counts[1] == 4 && commands[1][0] == 0x1B && commands[1][1] == 0x1E &&
          commands[1][2] == 0x1F && commands[1][3] == 0x1E);
    CHECK(slots[1].chatpad.present && slots[1].chatpad.buttons == (BIT(0) | BIT(24)));
    /* Slot 2: 1B at 2100, 1E at 3100 */
    CHECK(counts[2] == 2 && commands[2][0] == 0x1B && commands[2][1] == 0x1E);
    CHECK(!slots[2].chatpad.present);
    /* Slot 3: 1B at 800, the handshake at 1500 puts a 1B at 1800 in place of
       1E, then 1E at 2800 and 1F at 3800 */
    CHECK(counts[3] == 4 && commands[3][0] == 0x1B && commands[3][1] == 0x1B &&
          commands[3][2] == 0x1E && commands[3][3] == 0x1F);
    CHECK(slots[3].chatpad.present && slots[3].chatpad.buttons == 0);
    free(slots);
}

/* The uDraw */

/* The tablet at rest, from brandonw.net: 00 01 00 F0 00 13 00 00 72 0C FF
 * 0F FF 0F 00 00 20 24, then zeros to 29 bytes */
static void TabletRest(uint8_t out[29])
{
    static const uint8_t rest[18] = {
        0x00, 0x01, 0x00, 0xF0, 0x00, 0x13, 0x00, 0x00, 0x72,
        0x0C, 0xFF, 0x0F, 0xFF, 0x0F, 0x00, 0x00, 0x20, 0x24
    };

    memset(out, 0, 29);
    memcpy(out, rest, sizeof(rest));
}

static SDL_Xbox360AccSlot *NewTabletSlot(uint64_t now)
{
    SDL_Xbox360AccSlot *slot = NewSlots(1, true, true);
    uint8_t status[2];
    uint8_t info[29];

    Status(status, true);
    Feed(slot, now, status, 2);
    InfoPacket(info, SDL_XBOX360ACC_SUBTYPE_UDRAW);
    CHECK(Feed(slot, now, info, 29) == SDL_XBOX360ACC_PACKET_INFO);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_TABLET);
    return slot;
}

/* Tests 1 and 2: the info packet types the slot, and the tablet at rest */
static void TestTabletRest(void)
{
    SDL_Xbox360AccSlot *slot = NewSlots(1, true, true);
    uint8_t info[29];
    uint8_t packet[29];
    int i;

    InfoPacket(info, 0x23);
    CHECK(Feed(slot, 0, info, 29) == SDL_XBOX360ACC_PACKET_INFO);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_TABLET && slot->link);
    for (i = 0; i < SDL_XBOX360ACC_TABLET_AXES; ++i) {
        CHECK(slot->tablet.axes[i] == 0);
    }
    /* Bit 7 of byte 25 is the force feedback flag, not the subtype */
    SDL_Xbox360Acc_SlotLost(slot);
    InfoPacket(info, 0xA3);
    Feed(slot, 0, info, 29);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_TABLET);

    TabletRest(packet);
    CHECK(Feed(slot, 1, packet, 29) == SDL_XBOX360ACC_PACKET_TABLET);
    CHECK(slot->tablet.buttons == 0 && slot->tablet.hat == 0);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == 0 && slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_Y] == 0);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_PRESSURE] == -3470);    /* 0x72 * 257 - 32768 */
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_SPREAD] == -32768);     /* 0x00 */
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_TILT_X] == -24544);     /* 0x20 */
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_TILT_Y] == -23516);     /* 0x24 */
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_TILT_Z] == -29684);     /* 0x0C */
    free(slot);
}

/* Tests 3 to 6: the pen, the corners, the rest after a press, and touch */
static void TestTabletPen(void)
{
    SDL_Xbox360AccSlot *slot = NewTabletSlot(0);
    uint8_t packet[29];

    /* The pen at (1000, 500), pressure 0xC0 */
    TabletRest(packet);
    packet[8] = 0xC0;
    packet[10] = 0xE8;
    packet[11] = 0x03;
    packet[12] = 0xF4;
    packet[13] = 0x01;
    packet[14] = 0x40;
    CHECK(Feed(slot, 1, packet, 29) == SDL_XBOX360ACC_PACKET_TABLET);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == 1382);  /* 1000 * 65535 / 1919 - 32768 */
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_Y] == -2400); /* 500 * 65535 / 1079 - 32768 */
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_PRESSURE] == 16576); /* 0xC0 * 257 - 32768 */
    CHECK(slot->tablet.buttons == (1u << SDL_XBOX360ACC_TABLET_BUTTON_PEN));

    /* At rest again: X and Y hold, the pen is up */
    TabletRest(packet);
    Feed(slot, 2, packet, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == 1382);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_Y] == -2400);
    CHECK(slot->tablet.buttons == 0);

    /* The corners */
    TabletRest(packet);
    packet[14] = 0x40;
    packet[10] = 0x00;
    packet[11] = 0x00;
    packet[12] = 0x00;
    packet[13] = 0x00;
    Feed(slot, 3, packet, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == -32768);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_Y] == -32768);
    packet[10] = 0x7F;
    packet[11] = 0x07;
    packet[12] = 0x37;
    packet[13] = 0x04;
    Feed(slot, 4, packet, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == 32767);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_Y] == 32767);
    /* One short of the corners */
    packet[10] = 0x7E;
    packet[12] = 0x36;
    Feed(slot, 5, packet, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == 32732);  /* 1918 * 65535 / 1919 - 32768 */
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_Y] == 32706);  /* 1078 * 65535 / 1079 - 32768 */
    /* Past the surface the position stops at its edge */
    packet[10] = 0x80;
    packet[12] = 0x38;
    Feed(slot, 6, packet, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == 32767);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_Y] == 32767);
    packet[10] = 0xFF;
    packet[11] = 0x0E;
    packet[12] = 0xFF;
    packet[13] = 0x0E;
    Feed(slot, 7, packet, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == 32767);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_Y] == 32767);
    /* The one cell edge, 256 */
    packet[10] = 0x00;
    packet[11] = 0x01;
    packet[12] = 0x00;
    packet[13] = 0x01;
    Feed(slot, 8, packet, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == -24026);  /* 256 * 65535 / 1919 - 32768 */
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_Y] == -17220);  /* 256 * 65535 / 1079 - 32768 */

    /* Either cell at 0x0F holds the position, even while touched */
    packet[10] = 0x10;
    packet[11] = 0x0F;
    packet[12] = 0x20;
    packet[13] = 0x02;
    Feed(slot, 9, packet, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == -24026);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_Y] == -17220);
    packet[11] = 0x02;
    packet[13] = 0x0F;
    Feed(slot, 10, packet, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == -24026);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_Y] == -17220);
    /* So does byte 14 at 0 with both cells below 0x0F */
    packet[13] = 0x02;
    packet[14] = 0x00;
    Feed(slot, 11, packet, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == -24026);
    CHECK(slot->tablet.buttons == 0);

    /* A finger, a pinch, and the spread between two fingers */
    TabletRest(packet);
    packet[14] = 0x80;
    Feed(slot, 12, packet, 29);
    CHECK(slot->tablet.buttons == (1u << SDL_XBOX360ACC_TABLET_BUTTON_FINGER));
    packet[14] = 0x93;
    packet[15] = 0x40;
    Feed(slot, 13, packet, 29);
    CHECK(slot->tablet.buttons == (1u << SDL_XBOX360ACC_TABLET_BUTTON_MULTI));
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_SPREAD] == -16320); /* 0x40 * 257 - 32768 */
    {
        int b;
        for (b = 1; b < 256; ++b) {
            packet[14] = (uint8_t)b;
            Feed(slot, 14, packet, 29);
            if (b == 0x40) {
                CHECK(slot->tablet.buttons == (1u << SDL_XBOX360ACC_TABLET_BUTTON_PEN));
            } else if (b == 0x80) {
                CHECK(slot->tablet.buttons == (1u << SDL_XBOX360ACC_TABLET_BUTTON_FINGER));
            } else {
                CHECK(slot->tablet.buttons == (1u << SDL_XBOX360ACC_TABLET_BUTTON_MULTI));
            }
        }
    }
    /* The tilt axes and the spread over their whole range */
    {
        int v;
        for (v = 0; v < 256; ++v) {
            TabletRest(packet);
            packet[9] = (uint8_t)v;
            packet[15] = (uint8_t)(255 - v);
            packet[16] = (uint8_t)v;
            packet[17] = (uint8_t)(v ^ 0x5A);
            packet[8] = (uint8_t)(v ^ 0xA5);
            Feed(slot, 15, packet, 29);
            CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_TILT_Z] == v * 257 - 32768);
            CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_SPREAD] == (255 - v) * 257 - 32768);
            CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_TILT_X] == v * 257 - 32768);
            CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_TILT_Y] == (v ^ 0x5A) * 257 - 32768);
            CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_PRESSURE] == (v ^ 0xA5) * 257 - 32768);
        }
    }
    free(slot);
}

/* Test 7: each bit moves exactly one control */
static void TestTabletButtons(void)
{
    SDL_Xbox360AccSlot *slot = NewTabletSlot(0);
    uint8_t packet[29];
    static const struct
    {
        int byte;
        uint8_t bit;
        uint16_t buttons;
        uint8_t hat;
    } controls[] = {
        { 6, 0x01, 0, SDL_XBOX360ACC_HAT_UP },
        { 6, 0x02, 0, SDL_XBOX360ACC_HAT_DOWN },
        { 6, 0x04, 0, SDL_XBOX360ACC_HAT_LEFT },
        { 6, 0x08, 0, SDL_XBOX360ACC_HAT_RIGHT },
        { 6, 0x10, 1u << SDL_XBOX360ACC_TABLET_BUTTON_START, 0 },
        { 6, 0x20, 1u << SDL_XBOX360ACC_TABLET_BUTTON_BACK, 0 },
        { 6, 0x40, 0, 0 },
        { 6, 0x80, 0, 0 },
        { 7, 0x01, 0, 0 },
        { 7, 0x02, 0, 0 },
        { 7, 0x04, 1u << SDL_XBOX360ACC_TABLET_BUTTON_GUIDE, 0 },
        { 7, 0x08, 0, 0 },
        { 7, 0x10, 1u << SDL_XBOX360ACC_TABLET_BUTTON_A, 0 },
        { 7, 0x20, 1u << SDL_XBOX360ACC_TABLET_BUTTON_B, 0 },
        { 7, 0x40, 1u << SDL_XBOX360ACC_TABLET_BUTTON_X, 0 },
        { 7, 0x80, 1u << SDL_XBOX360ACC_TABLET_BUTTON_Y, 0 },
    };
    int i;

    CHECK(SDL_XBOX360ACC_HAT_UP == 0x01 && SDL_XBOX360ACC_HAT_RIGHT == 0x02 &&
          SDL_XBOX360ACC_HAT_DOWN == 0x04 && SDL_XBOX360ACC_HAT_LEFT == 0x08);
    for (i = 0; i < (int)(sizeof(controls) / sizeof(controls[0])); ++i) {
        TabletRest(packet);
        packet[controls[i].byte] = controls[i].bit;
        Feed(slot, 1, packet, 29);
        CHECK(slot->tablet.buttons == controls[i].buttons && slot->tablet.hat == controls[i].hat);
    }
    /* All of them together */
    TabletRest(packet);
    packet[6] = 0xFF;
    packet[7] = 0xFF;
    Feed(slot, 2, packet, 29);
    CHECK(slot->tablet.buttons == 0x7F && slot->tablet.hat == 0x0F);
    free(slot);
}

/* Test 8: truncations and framing */
static void TestTabletPackets(void)
{
    SDL_Xbox360AccSlot *slot = NewTabletSlot(0);
    SDL_Xbox360AccSlot before;
    uint8_t packet[29];
    uint8_t pressed[29];
    size_t n;
    int i, b;

    TabletRest(packet);
    packet[7] = 0x10;
    Feed(slot, 1, packet, 29);
    CHECK(slot->tablet.buttons == (1u << SDL_XBOX360ACC_TABLET_BUTTON_A));

    /* The pen down at (1000, 500) with every button held, cut short */
    TabletRest(pressed);
    pressed[6] = 0x3F;
    pressed[7] = 0xF4;
    pressed[8] = 0xC0;
    pressed[10] = 0xE8;
    pressed[11] = 0x03;
    pressed[12] = 0xF4;
    pressed[13] = 0x01;
    pressed[14] = 0x40;
    pressed[15] = 0x33;
    for (n = 0; n < 18; ++n) {
        before = *slot;
        CHECK(Feed(slot, 2, pressed, n) == SDL_XBOX360ACC_PACKET_OTHER);
        CHECK(SameSlot(slot, &before));
        CHECK(SDL_Xbox360Acc_SlotPacket(slot, 2, pressed, n) == SDL_XBOX360ACC_PACKET_OTHER);
        CHECK(SameSlot(slot, &before));
        CHECK(!SDL_Xbox360Acc_TabletDecode(&slot->tablet, pressed, n));
        CHECK(SameSlot(slot, &before));
    }
    /* 18 bytes are enough, and nothing past byte 17 is read */
    CHECK(Feed(slot, 3, pressed, 18) == SDL_XBOX360ACC_PACKET_TABLET);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == 1382);
    CHECK(slot->tablet.buttons == 0x7F + (1u << SDL_XBOX360ACC_TABLET_BUTTON_PEN) && slot->tablet.hat == 0x0F);
    memset(&pressed[18], 0xFF, 11);
    before = *slot;
    CHECK(Feed(slot, 4, pressed, 29) == SDL_XBOX360ACC_PACKET_TABLET);
    CHECK(SameSlot(slot, &before));

    /* Framing bytes other than 00 01 00 F0 00 13 change nothing. Byte 1 at
       0x0F makes the link control packet, which types the slot by byte 25
       and is tested with the typing. */
    for (i = 0; i < 6; ++i) {
        for (b = 0; b < 256; ++b) {
            TabletRest(packet);
            if (packet[i] == b || (i == 1 && b == 0x0F)) {
                continue;
            }
            packet[i] = (uint8_t)b;
            packet[7] = 0x80;
            before = *slot;
            CHECK(Feed(slot, 5, packet, 29) != SDL_XBOX360ACC_PACKET_TABLET);
            CHECK(SameTablet(&slot->tablet, &before.tablet));
            CHECK(slot->kind == SDL_XBOX360ACC_SLOT_TABLET);
        }
    }
    /* A chatpad packet on a tablet slot is not decoded */
    ChatpadPacket(packet, 0x00, 0x01, 0x37, 0x00);
    before = *slot;
    CHECK(Feed(slot, 6, packet, 29) == SDL_XBOX360ACC_PACKET_CHATPAD);
    CHECK(SameSlot(slot, &before));
    free(slot);
}

/* Tests 9 and 10: typing and retyping */
static void TestTabletTyping(void)
{
    SDL_Xbox360AccSlot *slot = NewSlots(1, true, true);
    uint8_t status[2];
    uint8_t info[29];
    uint8_t pad[29];
    uint8_t packet[29];

    /* The 08 status alone types nothing */
    Status(status, true);
    Feed(slot, 0, status, 2);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_NONE);

    /* Pad state before any info packet connects a gamepad, and pad packets
       stay pad packets */
    PadPacket(pad);
    pad[6] = 0x10;
    CHECK(Feed(slot, 1, pad, 29) == SDL_XBOX360ACC_PACKET_PAD);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_PAD);
    ChatpadPacket(packet, 0x00, 0x01, 0x37, 0x00);
    Feed(slot, 2, packet, 29);
    CHECK(slot->chatpad.present);
    /* A later info packet with 0x23 replaces it with the tablet, and the
       pad's chatpad goes with the pad */
    InfoPacket(info, 0x23);
    CHECK(Feed(slot, 3, info, 29) == SDL_XBOX360ACC_PACKET_INFO);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_TABLET);
    CHECK(!slot->chatpad.present && slot->chatpad.buttons == 0);
    CHECK(Feed(slot, 4, pad, 29) == SDL_XBOX360ACC_PACKET_TABLET);
    CHECK(slot->tablet.buttons == (1u << SDL_XBOX360ACC_TABLET_BUTTON_START));
    /* The same info packet again keeps the tablet as it is */
    TabletRest(packet);
    packet[10] = 0xE8;
    packet[11] = 0x03;
    packet[12] = 0xF4;
    packet[13] = 0x01;
    packet[14] = 0x40;
    Feed(slot, 5, packet, 29);
    Feed(slot, 6, info, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == 1382);
    /* An info packet with 0x01 connects a gamepad, and later pad packets
       decode as a pad's */
    InfoPacket(info, 0x01);
    Feed(slot, 7, info, 29);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_PAD);
    CHECK(Feed(slot, 8, pad, 29) == SDL_XBOX360ACC_PACKET_PAD);
    /* Every subtype but 0x23 is a pad */
    {
        int b;
        for (b = 0; b < 256; ++b) {
            SDL_Xbox360Acc_SlotLost(slot);
            InfoPacket(info, (uint8_t)b);
            Feed(slot, 9, info, 29);
            CHECK(slot->kind == (((b & 0x7F) == 0x23) ? SDL_XBOX360ACC_SLOT_TABLET : SDL_XBOX360ACC_SLOT_PAD));
        }
    }

    /* 08 00 on a tablet slot disconnects the tablet, and 08 80 then an info
       packet with 0x23 connect it again, with X and Y back at 0 */
    SDL_Xbox360Acc_SlotLost(slot);
    InfoPacket(info, 0x23);
    Feed(slot, 10, info, 29);
    Feed(slot, 11, packet, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == 1382);
    Status(status, false);
    Feed(slot, 12, status, 2);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_NONE);
    Status(status, true);
    Feed(slot, 13, status, 2);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_NONE);
    Feed(slot, 14, info, 29);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_TABLET);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == 0 && slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_Y] == 0);
    CHECK(slot->tablet.buttons == 0 && slot->tablet.hat == 0);
    /* A tablet that follows a pad starts over too, with no 08 status
       between them */
    Feed(slot, 15, packet, 29);
    CHECK(slot->tablet.axes[SDL_XBOX360ACC_TABLET_AXIS_X] == 1382 && slot->tablet.buttons != 0);
    InfoPacket(info, 0x01);
    Feed(slot, 16, info, 29);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_PAD);
    InfoPacket(info, 0x23);
    Feed(slot, 17, info, 29);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_TABLET);
    {
        int i;
        for (i = 0; i < SDL_XBOX360ACC_TABLET_AXES; ++i) {
            CHECK(slot->tablet.axes[i] == 0);
        }
        CHECK(slot->tablet.buttons == 0 && slot->tablet.hat == 0);
    }

    /* The info packet is exactly 29 bytes */
    SDL_Xbox360Acc_SlotLost(slot);
    InfoPacket(info, 0x23);
    CHECK(Feed(slot, 17, info, 28) == SDL_XBOX360ACC_PACKET_OTHER);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_NONE);

    /* A chatpad seen before the slot has a type stays with a pad, and a
       tablet takes the slot without it or its lamps */
    Status(status, true);
    Feed(slot, 18, status, 2);
    ChatpadPacket(packet, 0x00, 0x01, 0x37, 0x00);
    Feed(slot, 19, packet, 29);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_NONE && slot->chatpad.present);
    {
        const uint8_t lamp = 0x09;
        CHECK(SDL_Xbox360Acc_SlotLamp(slot, &lamp, 1));
        CHECK(slot->lamps == 0x02);
    }
    Feed(slot, 20, pad, 29);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_PAD && slot->chatpad.present && slot->lamps == 0x02);
    CHECK(slot->chatpad.buttons == (BIT(SDL_XBOX360ACC_BUTTON_SHIFT) | BIT(24)));
    SDL_Xbox360Acc_SlotLost(slot);
    Feed(slot, 21, status, 2);
    Feed(slot, 22, packet, 29);
    {
        const uint8_t lamp = 0x0A;
        CHECK(SDL_Xbox360Acc_SlotLamp(slot, &lamp, 1));
    }
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_NONE && slot->chatpad.present && slot->lamps == 0x04);
    InfoPacket(info, 0x23);
    Feed(slot, 23, info, 29);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_TABLET);
    CHECK(!slot->chatpad.present && slot->chatpad.buttons == 0 && slot->lamps == 0);
    free(slot);
}

/* With the uDraw hint off a slot connects as a pad on its 08 status and
 * stays one. With the chatpad hint off nothing goes out and chatpad
 * packets count for nothing. */
static void TestHintsOff(void)
{
    SDL_Xbox360AccSlot *slot = NewSlots(1, true, false);
    uint8_t status[2];
    uint8_t info[29];
    uint8_t packet[29];
    uint8_t commands[16];
    uint64_t times[16];

    Status(status, true);
    Feed(slot, 0, status, 2);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_PAD);
    InfoPacket(info, 0x23);
    Feed(slot, 1, info, 29);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_PAD);
    TabletRest(packet);
    CHECK(Feed(slot, 2, packet, 29) == SDL_XBOX360ACC_PACKET_PAD);
    Status(status, false);
    Feed(slot, 3, status, 2);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_NONE);
    /* Pad state and the info packet type nothing there */
    Feed(slot, 4, packet, 29);
    Feed(slot, 5, info, 29);
    CHECK(slot->kind == SDL_XBOX360ACC_SLOT_NONE);
    free(slot);

    slot = NewSlots(1, false, true);
    Status(status, true);
    Feed(slot, 0, status, 2);
    CHECK(!slot->scheduled);
    CHECK(Run(slot, 0, 3000, commands, times, 16) == 0);
    ChatpadPacket(packet, 0x00, 0x01, 0x37, 0x00);
    CHECK(Feed(slot, 3001, packet, 29) == SDL_XBOX360ACC_PACKET_CHATPAD);
    CHECK(!slot->chatpad.present && slot->chatpad.buttons == 0);
    ChatpadPacket(packet, 0xF0, 0x03, 0x00, 0x00);
    Feed(slot, 3002, packet, 29);
    CHECK(Run(slot, 3002, 6000, commands, times, 16) == 0);
    free(slot);
}

/* Null arguments do nothing */
static void TestNull(void)
{
    SDL_Xbox360AccControl control;
    SDL_Xbox360AccSlot *slot = NewSlots(1, true, true);
    uint8_t packet[SDL_XBOX360ACC_COMMAND_SIZE];

    SDL_Xbox360Acc_WiredStart(NULL, 0x0114, 0);
    SDL_Xbox360Acc_WiredStop(NULL);
    CHECK(!SDL_Xbox360Acc_WiredNext(NULL, 0, &control));
    SDL_Xbox360Acc_WiredDone(NULL, 0, true);
    CHECK(!SDL_Xbox360Acc_WiredReading(NULL));
    SDL_Xbox360Acc_WiredChatpadReport(NULL, packet, 5);
    SDL_Xbox360Acc_WiredPadReport(NULL, packet, 3);
    SDL_Xbox360Acc_SlotInit(NULL, true, true);
    CHECK(SDL_Xbox360Acc_SlotPacket(NULL, 0, packet, 2) == SDL_XBOX360ACC_PACKET_OTHER);
    CHECK(SDL_Xbox360Acc_SlotPacket(slot, 0, NULL, 2) == SDL_XBOX360ACC_PACKET_OTHER);
    SDL_Xbox360Acc_SlotUpdate(NULL, 0);
    CHECK(!SDL_Xbox360Acc_SlotLamp(NULL, packet, 1));
    CHECK(!SDL_Xbox360Acc_SlotTakeCommand(NULL, packet));
    CHECK(!SDL_Xbox360Acc_SlotTakeCommand(slot, NULL));
    SDL_Xbox360Acc_SlotLost(NULL);
    CHECK(!SDL_Xbox360Acc_TabletDecode(NULL, packet, 18));
    {
        SDL_Xbox360AccWired *wired = NewWired();
        SDL_Xbox360Acc_WiredStart(wired, 0x0114, 0);
        CHECK(!SDL_Xbox360Acc_WiredNext(wired, 0, NULL));
        SDL_Xbox360Acc_WiredChatpadReport(wired, NULL, 5);
        SDL_Xbox360Acc_WiredPadReport(wired, NULL, 3);
        CHECK(wired->step == 1 && !wired->in_flight);
        free(wired);
    }
    free(slot);
}

int main(void)
{
    TestIdentity();
    TestEndpoint();
    TestWiredKeys();
    TestWiredCodes();
    TestWiredStatus();
    TestWiredSchedule(0x0114);
    TestWiredSchedule(0x0110);
    TestWiredPresence();
    TestWiredUnplug();
    TestWiredLamps();
    TestWirelessKeys();
    TestWirelessSchedule();
    TestWirelessPackets();
    TestWirelessLamps();
    TestWirelessSlots();
    TestTabletRest();
    TestTabletPen();
    TestTabletButtons();
    TestTabletPackets();
    TestTabletTyping();
    TestHintsOff();
    TestNull();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
