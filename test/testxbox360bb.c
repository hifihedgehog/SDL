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

/* Offline replay test of the Xbox 360 Big Button receiver packet handling.
 * No device, no SDL runtime, time from an injected clock. The state lives
 * in an exact-size heap block and packets in exact-size heap copies, so
 * AddressSanitizer catches any access outside either. */

#include "../src/joystick/hidapi/SDL_hidapi_xbox360bb_proto.h"
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

#define MS(x) ((uint64_t)(x) * 1000000ULL)

typedef struct
{
    int connects[SDL_XBOX360BB_PADS];
    int publishes[SDL_XBOX360BB_PADS];
    uint8_t buttons[SDL_XBOX360BB_PADS];
    uint8_t hat[SDL_XBOX360BB_PADS];
    uint8_t open;
    int handles[SDL_XBOX360BB_PADS];
} Recorder;

static void OnConnect(void *userdata, int pad)
{
    Recorder *r = (Recorder *)userdata;
    CHECK(pad >= 0 && pad < SDL_XBOX360BB_PADS);
    if (pad >= 0 && pad < SDL_XBOX360BB_PADS) {
        ++r->connects[pad];
    }
}

static void *OnJoystick(void *userdata, int pad)
{
    Recorder *r = (Recorder *)userdata;
    CHECK(pad >= 0 && pad < SDL_XBOX360BB_PADS);
    if (pad < 0 || pad >= SDL_XBOX360BB_PADS || !(r->open & (1u << pad))) {
        return NULL;
    }
    return &r->handles[pad];
}

static void OnControls(void *userdata, void *joystick, uint8_t buttons, uint8_t hat)
{
    Recorder *r = (Recorder *)userdata;
    int pad;
    /* A NULL joystick here would crash the real driver. */
    CHECK(joystick != NULL);
    if (!joystick) {
        return;
    }
    pad = (int)((int *)joystick - r->handles);
    CHECK(pad >= 0 && pad < SDL_XBOX360BB_PADS);
    if (pad >= 0 && pad < SDL_XBOX360BB_PADS) {
        ++r->publishes[pad];
        r->buttons[pad] = buttons;
        r->hat[pad] = hat;
    }
}

static SDL_Xbox360BBSink MakeSink(Recorder *r)
{
    SDL_Xbox360BBSink sink;
    sink.userdata = r;
    sink.connect = OnConnect;
    sink.joystick = OnJoystick;
    sink.controls = OnControls;
    return sink;
}

static int Feed(SDL_Xbox360BBState *state, const uint8_t *packet, size_t length, uint64_t now,
                const SDL_Xbox360BBSink *sink)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    int pad;
    if (!copy) {
        ++failures;
        return -2;
    }
    memcpy(copy, packet, length);
    pad = SDL_Xbox360BB_HandlePacket(state, copy, length, now, sink);
    free(copy);
    return pad;
}

static SDL_Xbox360BBState *NewState(void)
{
    /* Exactly one state's worth of heap: a write past the four pads lands in
     * AddressSanitizer's redzone. */
    SDL_Xbox360BBState *state = (SDL_Xbox360BBState *)malloc(sizeof(SDL_Xbox360BBState));
    if (!state) {
        ++failures;
        return NULL;
    }
    SDL_Xbox360BB_Init(state);
    return state;
}

static bool SameStates(const SDL_Xbox360BBState *a, const SDL_Xbox360BBState *b)
{
    int i;
    for (i = 0; i < SDL_XBOX360BB_PADS; ++i) {
        if (a->pads[i].buttons != b->pads[i].buttons || a->pads[i].hat != b->pads[i].hat ||
            a->pads[i].last_packet != b->pads[i].last_packet) {
            return false;
        }
    }
    return true;
}

static bool AllZeroExcept(const SDL_Xbox360BBState *state, int pad)
{
    int i;
    for (i = 0; i < SDL_XBOX360BB_PADS; ++i) {
        if (i != pad && (state->pads[i].buttons || state->pads[i].hat)) {
            return false;
        }
    }
    return true;
}

typedef struct
{
    int byte;
    uint8_t bit;
    uint8_t buttons;
    uint8_t hat;
} ReadBit;

/* The 12 bits the driver reads, SDL_hidapi_xbox360bb.c:167-191 and
 * xbox360bb.c:298-323. */
static const ReadBit read_bits[12] = {
    { 3, 0x01, 0, SDL_XBOX360BB_HAT_UP },
    { 3, 0x02, 0, SDL_XBOX360BB_HAT_DOWN },
    { 3, 0x04, 0, SDL_XBOX360BB_HAT_LEFT },
    { 3, 0x08, 0, SDL_XBOX360BB_HAT_RIGHT },
    { 3, 0x10, SDL_XBOX360BB_BUTTON_START, 0 },
    { 3, 0x20, SDL_XBOX360BB_BUTTON_BACK, 0 },
    { 4, 0x04, SDL_XBOX360BB_BUTTON_GUIDE, 0 },
    { 4, 0x08, SDL_XBOX360BB_BUTTON_BIG, 0 },
    { 4, 0x10, SDL_XBOX360BB_BUTTON_A, 0 },
    { 4, 0x20, SDL_XBOX360BB_BUTTON_B, 0 },
    { 4, 0x40, SDL_XBOX360BB_BUTTON_X, 0 },
    { 4, 0x80, SDL_XBOX360BB_BUTTON_Y, 0 },
};

static void TestReadBits(void)
{
    SDL_Xbox360BBState *state = NewState();
    Recorder r;
    SDL_Xbox360BBSink sink = MakeSink(&r);
    int pad, i;

    if (!state) {
        return;
    }
    memset(&r, 0, sizeof(r));
    r.open = 0x0F;

    /* Each of the 12 read bits changes exactly one control on its pad and
     * none on the others. */
    for (pad = 0; pad < SDL_XBOX360BB_PADS; ++pad) {
        for (i = 0; i < 12; ++i) {
            uint8_t packet[5] = { 0, 0, 0, 0, 0 };
            packet[2] = (uint8_t)pad;
            packet[read_bits[i].byte] = read_bits[i].bit;
            SDL_Xbox360BB_Init(state);
            CHECK(Feed(state, packet, sizeof(packet), MS(1), &sink) == pad);
            CHECK(state->pads[pad].buttons == read_bits[i].buttons);
            CHECK(state->pads[pad].hat == read_bits[i].hat);
            CHECK(AllZeroExcept(state, pad));
            CHECK(r.buttons[pad] == read_bits[i].buttons && r.hat[pad] == read_bits[i].hat);
        }
    }

    /* The four unread bits and any value of bytes 0 and 1 change nothing. */
    for (pad = 0; pad < SDL_XBOX360BB_PADS; ++pad) {
        int value;
        for (value = 0; value < 256; ++value) {
            uint8_t packet[5];
            packet[0] = (uint8_t)value;
            packet[1] = (uint8_t)(255 - value);
            packet[2] = (uint8_t)pad;
            packet[3] = 0xC0;
            packet[4] = 0x03;
            SDL_Xbox360BB_Init(state);
            CHECK(Feed(state, packet, sizeof(packet), MS(1), &sink) == pad);
            CHECK(state->pads[pad].buttons == 0 && state->pads[pad].hat == 0);
            CHECK(AllZeroExcept(state, pad));
        }
    }

    /* All 12 at once. */
    {
        const uint8_t packet[5] = { 0, 0, 2, 0x3F, 0xFC };
        SDL_Xbox360BB_Init(state);
        CHECK(Feed(state, packet, sizeof(packet), MS(1), &sink) == 2);
        CHECK(state->pads[2].buttons == 0xFF);
        CHECK(state->pads[2].hat == (SDL_XBOX360BB_HAT_UP | SDL_XBOX360BB_HAT_DOWN |
                                     SDL_XBOX360BB_HAT_LEFT | SDL_XBOX360BB_HAT_RIGHT));
    }
    free(state);
}

static void TestBadIndex(void)
{
    SDL_Xbox360BBState *state = NewState();
    Recorder r;
    SDL_Xbox360BBSink sink = MakeSink(&r);
    SDL_Xbox360BBState before;
    int index;

    if (!state) {
        return;
    }
    memset(&r, 0, sizeof(r));
    r.open = 0x0F;
    {
        const uint8_t held[5] = { 0, 0, 3, 0x01, 0x10 };
        CHECK(Feed(state, held, sizeof(held), MS(10), &sink) == 3);
        r.publishes[3] = 0;
    }
    /* Indexes 4 and above change nothing and touch nothing outside the
     * state. The exact-size heap block makes any such write an
     * AddressSanitizer error. */
    for (index = SDL_XBOX360BB_PADS; index < 256; ++index) {
        uint8_t packet[5] = { 0, 0, 0, 0xFF, 0xFF };
        packet[2] = (uint8_t)index;
        before = *state;
        CHECK(Feed(state, packet, sizeof(packet), MS(1000), &sink) == -1);
        CHECK(SameStates(&before, state));
    }
    CHECK(r.publishes[0] + r.publishes[1] + r.publishes[2] + r.publishes[3] == 0);
    free(state);
}

static void TestLengths(void)
{
    SDL_Xbox360BBState *state = NewState();
    Recorder r;
    SDL_Xbox360BBSink sink = MakeSink(&r);
    uint8_t packet[64];
    size_t length;

    if (!state) {
        return;
    }
    memset(&r, 0, sizeof(r));
    r.open = 0x0F;
    memset(packet, 0, sizeof(packet));
    packet[2] = 1;
    packet[4] = 0x10; /* A on pad 1 */
    /* Every length but 5 changes nothing. */
    for (length = 0; length <= sizeof(packet); ++length) {
        SDL_Xbox360BB_Init(state);
        if (length == SDL_XBOX360BB_PACKET_SIZE) {
            CHECK(Feed(state, packet, length, MS(1), &sink) == 1);
            CHECK(state->pads[1].buttons == SDL_XBOX360BB_BUTTON_A);
        } else {
            CHECK(Feed(state, packet, length, MS(1), &sink) == -1);
            CHECK(AllZeroExcept(state, -1));
        }
    }
    /* Null and missing arguments. */
    CHECK(SDL_Xbox360BB_HandlePacket(state, NULL, 5, 0, &sink) == -1);
    CHECK(SDL_Xbox360BB_HandlePacket(state, packet, 5, 0, NULL) == -1);
    CHECK(SDL_Xbox360BB_HandlePacket(NULL, packet, 5, 0, &sink) == -1);
    free(state);
}

static void TestRelease(void)
{
    SDL_Xbox360BBState *state = NewState();
    Recorder r;
    SDL_Xbox360BBSink sink = MakeSink(&r);
    const uint8_t held[5] = { 0, 0, 0, 0, 0x20 }; /* B on pad 0 */
    uint64_t t;

    if (!state) {
        return;
    }
    memset(&r, 0, sizeof(r));
    r.open = 0x0F;

    /* A held button stays down while packets arrive less than 120 ms apart. */
    for (t = 0; t <= MS(1000); t += MS(119)) {
        CHECK(Feed(state, held, sizeof(held), MS(10) + t, &sink) == 0);
        CHECK(SDL_Xbox360BB_Expire(state, MS(10) + t + MS(118), &sink) == 0);
        CHECK(state->pads[0].buttons == SDL_XBOX360BB_BUTTON_B);
    }
    /* It releases 120 ms after the last one, and not a nanosecond before. */
    {
        const uint64_t last = MS(5000);
        CHECK(Feed(state, held, sizeof(held), last, &sink) == 0);
        CHECK(SDL_Xbox360BB_Expire(state, last + SDL_XBOX360BB_RELEASE_NS - 1, &sink) == 0);
        CHECK(state->pads[0].buttons == SDL_XBOX360BB_BUTTON_B);
        CHECK(SDL_Xbox360BB_Expire(state, last + SDL_XBOX360BB_RELEASE_NS, &sink) == 0x01);
        CHECK(state->pads[0].buttons == 0 && state->pads[0].hat == 0);
        CHECK(r.buttons[0] == 0 && r.hat[0] == 0);
        /* Nothing more to release. */
        CHECK(SDL_Xbox360BB_Expire(state, last + MS(10000), &sink) == 0);
    }
    /* A 188 ms gap, as measured on hardware, releases the button, and the
     * next packet presses it again. */
    CHECK(Feed(state, held, sizeof(held), MS(20000), &sink) == 0);
    CHECK(SDL_Xbox360BB_Expire(state, MS(20188), &sink) == 0x01);
    CHECK(Feed(state, held, sizeof(held), MS(20188), &sink) == 0);
    CHECK(state->pads[0].buttons == SDL_XBOX360BB_BUTTON_B);

    /* Pads expire independently. */
    {
        const uint8_t pad2[5] = { 0, 0, 2, 0x01, 0 };
        SDL_Xbox360BB_Init(state);
        CHECK(Feed(state, held, sizeof(held), MS(100), &sink) == 0);
        CHECK(Feed(state, pad2, sizeof(pad2), MS(200), &sink) == 2);
        CHECK(SDL_Xbox360BB_Expire(state, MS(220), &sink) == 0x01);
        CHECK(state->pads[2].hat == SDL_XBOX360BB_HAT_UP);
        CHECK(SDL_Xbox360BB_Expire(state, MS(320), &sink) == 0x04);
    }
    /* A clock that appears to run backward releases nothing. */
    SDL_Xbox360BB_Init(state);
    CHECK(Feed(state, held, sizeof(held), MS(500), &sink) == 0);
    CHECK(SDL_Xbox360BB_Expire(state, MS(100), &sink) == 0);
    CHECK(SDL_Xbox360BB_Expire(NULL, MS(100), &sink) == 0);
    CHECK(SDL_Xbox360BB_Expire(state, MS(100), NULL) == 0);
    free(state);
}

static void TestAttach(void)
{
    SDL_Xbox360BBState *state = NewState();
    Recorder r;
    SDL_Xbox360BBSink sink = MakeSink(&r);
    int pad;

    if (!state) {
        return;
    }
    /* All four joysticks exist after open, before any packet. */
    memset(&r, 0, sizeof(r));
    SDL_Xbox360BB_Attach(state, &sink);
    for (pad = 0; pad < SDL_XBOX360BB_PADS; ++pad) {
        CHECK(r.connects[pad] == 1);
    }
    /* Attaching again, as after the receiver is removed and plugged back in,
     * connects four with no state kept. */
    {
        const uint8_t held[5] = { 0, 0, 3, 0x10, 0x80 };
        CHECK(Feed(state, held, sizeof(held), MS(1), &sink) == 3);
        SDL_Xbox360BB_Attach(state, &sink);
        for (pad = 0; pad < SDL_XBOX360BB_PADS; ++pad) {
            CHECK(r.connects[pad] == 2);
        }
        CHECK(AllZeroExcept(state, -1));
    }
    SDL_Xbox360BB_Attach(NULL, &sink);
    SDL_Xbox360BB_Attach(state, NULL);
    CHECK(r.connects[0] == 2);
    free(state);
}

static void TestUnopened(void)
{
    SDL_Xbox360BBState *state = NewState();
    Recorder r;
    SDL_Xbox360BBSink sink = MakeSink(&r);
    const uint8_t packet[5] = { 0x00, 0x00, 0x01, 0x01, 0x00 };

    if (!state) {
        return;
    }
    /* With only pad 0 open, 00 00 01 01 00 sends nothing. */
    memset(&r, 0, sizeof(r));
    r.open = 0x01;
    CHECK(Feed(state, packet, sizeof(packet), MS(1), &sink) == 1);
    CHECK(r.publishes[0] == 0 && r.publishes[1] == 0 && r.publishes[2] == 0 && r.publishes[3] == 0);
    CHECK(state->pads[1].hat == SDL_XBOX360BB_HAT_UP);
    /* Its release sends nothing either. */
    CHECK(SDL_Xbox360BB_Expire(state, MS(1) + SDL_XBOX360BB_RELEASE_NS, &sink) == 0x02);
    CHECK(r.publishes[1] == 0);
    /* The open pad does get its packets. */
    {
        const uint8_t pad0[5] = { 0x00, 0x00, 0x00, 0x00, 0x08 };
        CHECK(Feed(state, pad0, sizeof(pad0), MS(500), &sink) == 0);
        CHECK(r.publishes[0] == 1 && r.buttons[0] == SDL_XBOX360BB_BUTTON_BIG);
    }
    free(state);
}

static void TestEnumeration(void)
{
    /* The receiver's interface is class 0xFF, subclass 0x5D, protocol 4,
     * and on Windows it now reaches the libusb backend once WinUSB is bound. */
    const SDL_VendorUSBRule *rule = SDL_VendorUSB_FindRule(USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER,
                                                           0, 0xFF, 0x5D, 0x04);
    SDL_VendorUSBRouting routing;

    CHECK(rule != NULL);
    memset(&routing, 0, sizeof(routing));
    routing.platform = SDL_VENDORUSB_PLATFORM_WINDOWS;
    routing.libusb = true;
    routing.whitelist = true;
    routing.gamecube = true;
    routing.vendor = USB_VENDOR_MICROSOFT;
    routing.product = USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER;
    routing.xbox = true;
    routing.vendor_interface = (rule != NULL);
    CHECK(!SDL_VendorUSB_Ignore(&routing));
    CHECK(SDL_VendorUSB_IsCandidate(USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER, 0, 0xFF, 0x5D, 0x04, true));
    CHECK(!SDL_VendorUSB_SkipUnopened(SDL_VENDORUSB_PLATFORM_WINDOWS, true, true));
    CHECK(SDL_VendorUSB_SkipUnopened(SDL_VENDORUSB_PLATFORM_WINDOWS, true, false));
}

int main(void)
{
    TestReadBits();
    TestBadIndex();
    TestLengths();
    TestRelease();
    TestAttach();
    TestUnopened();
    TestEnumeration();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
