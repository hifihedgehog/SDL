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

/* Offline replay test of the Intel Wireless Series base station protocol.
 * No device, no SDL runtime. Every packet is fed through an exact-size heap
 * buffer, so AddressSanitizer catches any read past the length the module
 * was given, and again with stale bytes past that length. */

#include "../src/joystick/hidapi/SDL_hidapi_intelwireless_proto.h"
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

/* The captured session, JoypadOS docs/INTEL_WIRELESS_SERIES.md:41-48. */
static const uint8_t remembered1[] = { 0x03, 0x01, 0x0c, 0x02, 0xff, 0x00, 0x00 };
static const uint8_t remembered2[] = { 0x03, 0x02, 0x0c, 0x02, 0xff, 0x00, 0x00 };
static const uint8_t activate3[] = { 0x03, 0x03, 0x01, 0x02, 0xff, 0x00, 0x00 };
static const uint8_t zero3[] = { 0x06, 0x03, 0x00, 0x00, 0xff, 0x00, 0x00 };
static const uint8_t ready3[] = { 0x03, 0x03, 0x04, 0x02, 0xff, 0x00, 0x00 };
static const uint8_t input3_b[] = { 0x01, 0x17, 0x13, 0x00, 0x03, 0x03, 0x06, 0x63, 0x04, 0x00, 0x00, 0x02, 0x00 };
static const uint8_t input3_released[] = { 0x01, 0x00, 0x00, 0x00, 0x03, 0x03, 0x06, 0x63, 0x04, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t reply3[] = { 0x03, 0x01, 0xff, 0x00, 0x01, 0x63 };

/* A sink that records every call. A slot is open when its bit is set. */
typedef enum
{
    EVENT_CONNECT,
    EVENT_DISCONNECT,
    EVENT_CONTROLS
} EventType;

typedef struct
{
    EventType type;
    int slot;
    uint16_t buttons;
    uint8_t hat;
} Event;

typedef struct
{
    Event events[256];
    int count;
    uint8_t open;
    int handles[SDL_INTEL_WIRELESS_SLOTS];
} Recorder;

static void OnConnect(void *userdata, int slot)
{
    Recorder *r = (Recorder *)userdata;
    CHECK(slot >= 0 && slot < SDL_INTEL_WIRELESS_SLOTS);
    if (r->count < 256) {
        r->events[r->count].type = EVENT_CONNECT;
        r->events[r->count].slot = slot;
        r->events[r->count].buttons = 0;
        r->events[r->count].hat = 0;
        ++r->count;
    }
}

static void OnDisconnect(void *userdata, int slot)
{
    Recorder *r = (Recorder *)userdata;
    CHECK(slot >= 0 && slot < SDL_INTEL_WIRELESS_SLOTS);
    if (r->count < 256) {
        r->events[r->count].type = EVENT_DISCONNECT;
        r->events[r->count].slot = slot;
        r->events[r->count].buttons = 0;
        r->events[r->count].hat = 0;
        ++r->count;
    }
}

static void *OnJoystick(void *userdata, int slot)
{
    Recorder *r = (Recorder *)userdata;
    CHECK(slot >= 0 && slot < SDL_INTEL_WIRELESS_SLOTS);
    if (slot < 0 || slot >= SDL_INTEL_WIRELESS_SLOTS || !(r->open & (1u << slot))) {
        return NULL;
    }
    return &r->handles[slot];
}

static void OnControls(void *userdata, void *joystick, uint16_t buttons, uint8_t hat)
{
    Recorder *r = (Recorder *)userdata;
    int slot;
    /* A NULL joystick here would crash the real driver. */
    CHECK(joystick != NULL);
    if (!joystick) {
        return;
    }
    slot = (int)((int *)joystick - r->handles);
    CHECK(slot >= 0 && slot < SDL_INTEL_WIRELESS_SLOTS);
    if (r->count < 256) {
        r->events[r->count].type = EVENT_CONTROLS;
        r->events[r->count].slot = slot;
        r->events[r->count].buttons = buttons;
        r->events[r->count].hat = hat;
        ++r->count;
    }
}

static SDL_IntelWirelessSink MakeSink(Recorder *r)
{
    SDL_IntelWirelessSink sink;
    sink.userdata = r;
    sink.connect = OnConnect;
    sink.disconnect = OnDisconnect;
    sink.joystick = OnJoystick;
    sink.controls = OnControls;
    return sink;
}

static void Reset(Recorder *r, uint8_t open)
{
    memset(r, 0, sizeof(*r));
    r->open = open;
}

/* Feeds length bytes of packet through an exact-size heap copy. */
static void Feed(SDL_IntelWirelessState *state, const uint8_t *packet, size_t length, const SDL_IntelWirelessSink *sink)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    if (!copy) {
        ++failures;
        return;
    }
    memcpy(copy, packet, length);
    SDL_IntelWireless_HandlePacket(state, copy, length, sink);
    free(copy);
}

/* Feeds length bytes of packet from a buffer whose bytes past length are
 * stale: either the packet's own later bytes or a fill value. */
static void FeedStale(SDL_IntelWirelessState *state, const uint8_t *packet, size_t full, size_t length,
                      uint8_t fill, const SDL_IntelWirelessSink *sink)
{
    uint8_t buffer[64];
    memset(buffer, fill, sizeof(buffer));
    memcpy(buffer, packet, full);
    SDL_IntelWireless_HandlePacket(state, buffer, length, sink);
}

static bool SameSlot(const SDL_IntelWirelessSlot *a, const SDL_IntelWirelessSlot *b)
{
    return a->gamepad == b->gamepad && a->connected == b->connected && a->buttons == b->buttons && a->hat == b->hat;
}

static bool SameState(const SDL_IntelWirelessState *a, const SDL_IntelWirelessState *b)
{
    int i;
    for (i = 0; i < SDL_INTEL_WIRELESS_SLOTS; ++i) {
        if (!SameSlot(&a->slots[i], &b->slots[i])) {
            return false;
        }
    }
    return a->pending == b->pending && a->in_flight == b->in_flight &&
           (!a->in_flight || a->in_flight_slot == b->in_flight_slot) && a->retry_at == b->retry_at;
}

static bool SameEvents(const Recorder *a, const Recorder *b)
{
    int i;
    if (a->count != b->count) {
        return false;
    }
    for (i = 0; i < a->count; ++i) {
        if (a->events[i].type != b->events[i].type || a->events[i].slot != b->events[i].slot ||
            a->events[i].buttons != b->events[i].buttons || a->events[i].hat != b->events[i].hat) {
            return false;
        }
    }
    return true;
}

/* Drives the captured session through the slot 3 ready, with the one reply
 * written in full. */
static void SessionToReady(SDL_IntelWirelessState *state, const SDL_IntelWirelessSink *sink)
{
    uint8_t reply[SDL_INTEL_WIRELESS_REPLY_SIZE];
    SDL_IntelWireless_Init(state);
    Feed(state, remembered1, sizeof(remembered1), sink);
    Feed(state, remembered2, sizeof(remembered2), sink);
    Feed(state, activate3, sizeof(activate3), sink);
    CHECK(SDL_IntelWireless_NextReply(state, 0, reply));
    SDL_IntelWireless_ReplyDone(state, SDL_INTEL_WIRELESS_REPLY_SIZE, 0);
    Feed(state, zero3, sizeof(zero3), sink);
    Feed(state, ready3, sizeof(ready3), sink);
}

static void TestCapturedSession(void)
{
    SDL_IntelWirelessState state;
    Recorder r;
    SDL_IntelWirelessSink sink = MakeSink(&r);
    uint8_t reply[SDL_INTEL_WIRELESS_REPLY_SIZE];

    Reset(&r, 0xFF);
    SDL_IntelWireless_Init(&state);

    /* Slots 1 and 2 become known gamepads and connect nothing. */
    Feed(&state, remembered1, sizeof(remembered1), &sink);
    Feed(&state, remembered2, sizeof(remembered2), &sink);
    CHECK(r.count == 0);
    CHECK(state.slots[1].gamepad && !state.slots[1].connected);
    CHECK(state.slots[2].gamepad && !state.slots[2].connected);
    CHECK(!SDL_IntelWireless_NextReply(&state, 0, reply));

    /* One activation reply follows the slot 3 activation. */
    Feed(&state, activate3, sizeof(activate3), &sink);
    CHECK(r.count == 0);
    CHECK(SDL_IntelWireless_NextReply(&state, 0, reply));
    CHECK(memcmp(reply, reply3, sizeof(reply3)) == 0);
    CHECK(!SDL_IntelWireless_NextReply(&state, 0, reply));
    SDL_IntelWireless_ReplyDone(&state, SDL_INTEL_WIRELESS_REPLY_SIZE, 0);
    CHECK(!SDL_IntelWireless_NextReply(&state, 0, reply));

    /* The zero state before ready connects nothing. */
    Feed(&state, zero3, sizeof(zero3), &sink);
    CHECK(r.count == 0);

    /* Slot 3 connects on ready. */
    Feed(&state, ready3, sizeof(ready3), &sink);
    CHECK(r.count == 1 && r.events[0].type == EVENT_CONNECT && r.events[0].slot == 3);

    /* B is held on slot 3, then released. */
    Feed(&state, input3_b, sizeof(input3_b), &sink);
    CHECK(r.count == 2 && r.events[1].type == EVENT_CONTROLS && r.events[1].slot == 3);
    CHECK(r.events[1].buttons == SDL_INTEL_WIRELESS_BUTTON_B && r.events[1].hat == 0);
    Feed(&state, input3_released, sizeof(input3_released), &sink);
    CHECK(r.count == 3 && r.events[2].type == EVENT_CONTROLS && r.events[2].slot == 3);
    CHECK(r.events[2].buttons == 0 && r.events[2].hat == 0);

    /* No other slot ever connected. */
    CHECK(!state.slots[0].connected && !state.slots[1].connected && !state.slots[2].connected);
}

typedef struct
{
    const uint8_t *packet;
    size_t size;
    size_t minimum;
} Captured;

static void TestTruncations(void)
{
    /* Minimum lengths: 13 for input, 2 for zero state, 4 for information
     * operations 1, 0x0A, 0x0B and 0x0C, 3 for the other operations. */
    const Captured captured[] = {
        { remembered1, sizeof(remembered1), 4 },
        { remembered2, sizeof(remembered2), 4 },
        { activate3, sizeof(activate3), 4 },
        { zero3, sizeof(zero3), 2 },
        { ready3, sizeof(ready3), 3 },
        { input3_b, sizeof(input3_b), 13 },
        { input3_released, sizeof(input3_released), 13 },
    };
    size_t c, n;
    int prefix;

    /* Each packet is tested at the point of the session where it arrived,
     * so its fields act on real state. */
    for (c = 0; c < sizeof(captured) / sizeof(captured[0]); ++c) {
        for (n = 0; n <= captured[c].size; ++n) {
            const uint8_t fills[] = { 0x00, 0xFF, 0xA5 };
            size_t f;
            for (f = 0; f <= sizeof(fills); ++f) {
                SDL_IntelWirelessState before, full, truncated;
                Recorder r_before, r_full, r_truncated;
                SDL_IntelWirelessSink sink_before = MakeSink(&r_before);
                SDL_IntelWirelessSink sink_full = MakeSink(&r_full);
                SDL_IntelWirelessSink sink_truncated = MakeSink(&r_truncated);

                Reset(&r_before, 0xFF);
                SDL_IntelWireless_Init(&before);
                /* Replay the session up to this packet. */
                for (prefix = 0; prefix < (int)c; ++prefix) {
                    Feed(&before, captured[prefix].packet, captured[prefix].size, &sink_before);
                    if (captured[prefix].packet == activate3) {
                        uint8_t reply[SDL_INTEL_WIRELESS_REPLY_SIZE];
                        CHECK(SDL_IntelWireless_NextReply(&before, 0, reply));
                        SDL_IntelWireless_ReplyDone(&before, SDL_INTEL_WIRELESS_REPLY_SIZE, 0);
                    }
                }
                full = before;
                truncated = before;
                Reset(&r_full, 0xFF);
                Reset(&r_truncated, 0xFF);
                Feed(&full, captured[c].packet, captured[c].size, &sink_full);
                if (f == sizeof(fills)) {
                    /* Exact size: AddressSanitizer catches any read past n. */
                    Feed(&truncated, captured[c].packet, n, &sink_truncated);
                } else {
                    FeedStale(&truncated, captured[c].packet, captured[c].size, n, fills[f], &sink_truncated);
                }
                if (n < captured[c].minimum) {
                    CHECK(SameState(&truncated, &before));
                    CHECK(r_truncated.count == 0);
                } else {
                    CHECK(SameState(&truncated, &full));
                    CHECK(SameEvents(&r_truncated, &r_full));
                }
            }
        }
    }
}

static void TestControls(void)
{
    static const uint16_t byte11[8] = {
        SDL_INTEL_WIRELESS_BUTTON_A, SDL_INTEL_WIRELESS_BUTTON_B, SDL_INTEL_WIRELESS_BUTTON_C,
        SDL_INTEL_WIRELESS_BUTTON_X, SDL_INTEL_WIRELESS_BUTTON_Y, SDL_INTEL_WIRELESS_BUTTON_Z,
        SDL_INTEL_WIRELESS_BUTTON_L, SDL_INTEL_WIRELESS_BUTTON_R
    };
    static const uint16_t byte12[3] = {
        SDL_INTEL_WIRELESS_BUTTON_START, SDL_INTEL_WIRELESS_BUTTON_SHIFT, SDL_INTEL_WIRELESS_BUTTON_MOUSE
    };
    SDL_IntelWirelessState state;
    Recorder r;
    SDL_IntelWirelessSink sink = MakeSink(&r);
    uint8_t packet[13];
    int bit;

    Reset(&r, 0xFF);
    SessionToReady(&state, &sink);
    memcpy(packet, input3_b, sizeof(packet));

    /* Each of the 8 buttons in byte 11 and the 3 in byte 12 maps to its one
     * control. */
    for (bit = 0; bit < 8; ++bit) {
        packet[9] = packet[10] = 0;
        packet[11] = (uint8_t)(1u << bit);
        packet[12] = 0;
        r.count = 0;
        Feed(&state, packet, sizeof(packet), &sink);
        CHECK(r.count == 1 && r.events[0].buttons == byte11[bit] && r.events[0].hat == 0);
    }
    for (bit = 0; bit < 3; ++bit) {
        packet[11] = 0;
        packet[12] = (uint8_t)(1u << bit);
        r.count = 0;
        Feed(&state, packet, sizeof(packet), &sink);
        CHECK(r.count == 1 && r.events[0].buttons == byte12[bit] && r.events[0].hat == 0);
    }
    /* Bits 3 to 7 of byte 12 carry no control. */
    packet[12] = 0xF8;
    r.count = 0;
    Feed(&state, packet, sizeof(packet), &sink);
    CHECK(r.count == 1 && r.events[0].buttons == 0);
    packet[12] = 0;

    /* The four D-pad directions. */
    {
        const struct
        {
            uint8_t x, y, hat;
        } directions[] = {
            { 0x81, 0x00, SDL_INTEL_WIRELESS_HAT_LEFT },
            { 0x7F, 0x00, SDL_INTEL_WIRELESS_HAT_RIGHT },
            { 0x00, 0x81, SDL_INTEL_WIRELESS_HAT_UP },
            { 0x00, 0x7F, SDL_INTEL_WIRELESS_HAT_DOWN },
            { 0x81, 0x81, SDL_INTEL_WIRELESS_HAT_LEFT | SDL_INTEL_WIRELESS_HAT_UP },
            { 0x7F, 0x7F, SDL_INTEL_WIRELESS_HAT_RIGHT | SDL_INTEL_WIRELESS_HAT_DOWN },
        };
        size_t i;
        for (i = 0; i < sizeof(directions) / sizeof(directions[0]); ++i) {
            packet[9] = directions[i].x;
            packet[10] = directions[i].y;
            r.count = 0;
            Feed(&state, packet, sizeof(packet), &sink);
            CHECK(r.count == 1 && r.events[0].hat == directions[i].hat && r.events[0].buttons == 0);
        }
    }
    /* 0x00, 0x80 and 0xFF leave an axis centered. */
    {
        const uint8_t neutral[] = { 0x00, 0x80, 0xFF, 0x01, 0x7E, 0x82 };
        size_t i, j;
        for (i = 0; i < sizeof(neutral); ++i) {
            for (j = 0; j < sizeof(neutral); ++j) {
                packet[9] = neutral[i];
                packet[10] = neutral[j];
                r.count = 0;
                Feed(&state, packet, sizeof(packet), &sink);
                CHECK(r.count == 1 && r.events[0].hat == 0);
            }
        }
    }
    /* Bytes the protocol does not read change nothing. */
    {
        int k;
        memcpy(packet, input3_b, sizeof(packet));
        for (k = 0; k < 13; ++k) {
            uint8_t altered[13];
            if (k == 0 || k == 4 || k == 9 || k == 10 || k == 11 || k == 12) {
                continue;
            }
            memcpy(altered, packet, sizeof(altered));
            altered[k] ^= 0xFF;
            r.count = 0;
            Feed(&state, altered, sizeof(altered), &sink);
            CHECK(r.count == 1 && r.events[0].buttons == SDL_INTEL_WIRELESS_BUTTON_B && r.events[0].hat == 0);
        }
    }
}

static void TestReplyQueue(void)
{
    SDL_IntelWirelessState state;
    Recorder r;
    SDL_IntelWirelessSink sink = MakeSink(&r);
    uint8_t reply[SDL_INTEL_WIRELESS_REPLY_SIZE];
    static const uint8_t activate7[] = { 0x03, 0x07, 0x01, 0x02 };
    static const uint8_t reply7[] = { 0x07, 0x01, 0xff, 0x00, 0x01, 0x63 };

    Reset(&r, 0xFF);
    SDL_IntelWireless_Init(&state);
    Feed(&state, activate3, sizeof(activate3), &sink);
    CHECK(SDL_IntelWireless_NextReply(&state, 0, reply));
    CHECK(memcmp(reply, reply3, sizeof(reply3)) == 0);

    /* A second activation never overwrites the one in flight. */
    Feed(&state, activate7, sizeof(activate7), &sink);
    CHECK(!SDL_IntelWireless_NextReply(&state, 0, reply));
    CHECK(memcmp(reply, reply3, sizeof(reply3)) == 0);
    SDL_IntelWireless_ReplyDone(&state, SDL_INTEL_WIRELESS_REPLY_SIZE, 0);
    CHECK(SDL_IntelWireless_NextReply(&state, 0, reply));
    CHECK(memcmp(reply, reply7, sizeof(reply7)) == 0);

    /* A failed write is retried after the delay on the injected clock. */
    SDL_IntelWireless_ReplyDone(&state, -1, 1000);
    CHECK(!SDL_IntelWireless_NextReply(&state, 1000, reply));
    CHECK(!SDL_IntelWireless_NextReply(&state, 1000 + SDL_INTEL_WIRELESS_RETRY_NS - 1, reply));
    CHECK(SDL_IntelWireless_NextReply(&state, 1000 + SDL_INTEL_WIRELESS_RETRY_NS, reply));
    CHECK(memcmp(reply, reply7, sizeof(reply7)) == 0);

    /* So is a short one. */
    SDL_IntelWireless_ReplyDone(&state, 5, 5000);
    CHECK(SDL_IntelWireless_NextReply(&state, 5000 + SDL_INTEL_WIRELESS_RETRY_NS, reply));
    CHECK(memcmp(reply, reply7, sizeof(reply7)) == 0);
    SDL_IntelWireless_ReplyDone(&state, SDL_INTEL_WIRELESS_REPLY_SIZE, 6000);
    CHECK(!SDL_IntelWireless_NextReply(&state, 1000000000, reply));

    /* Pending slots go lowest first. */
    {
        static const uint8_t activate0[] = { 0x03, 0x00, 0x01, 0x02 };
        static const uint8_t activate5[] = { 0x03, 0x05, 0x01, 0x02 };
        SDL_IntelWireless_Init(&state);
        Feed(&state, activate5, sizeof(activate5), &sink);
        Feed(&state, activate0, sizeof(activate0), &sink);
        CHECK(SDL_IntelWireless_NextReply(&state, 0, reply) && reply[0] == 0);
        SDL_IntelWireless_ReplyDone(&state, SDL_INTEL_WIRELESS_REPLY_SIZE, 0);
        CHECK(SDL_IntelWireless_NextReply(&state, 0, reply) && reply[0] == 5);
        SDL_IntelWireless_ReplyDone(&state, SDL_INTEL_WIRELESS_REPLY_SIZE, 0);
    }

    /* A slot that stops being a gamepad drops its pending reply, and a failed
     * reply for it is not queued again. */
    {
        static const uint8_t keyboard7[] = { 0x03, 0x07, 0x01, 0x00 };
        static const uint8_t mouse7[] = { 0x03, 0x07, 0x05 };
        SDL_IntelWireless_Init(&state);
        Feed(&state, activate3, sizeof(activate3), &sink);
        Feed(&state, activate7, sizeof(activate7), &sink);
        CHECK(SDL_IntelWireless_NextReply(&state, 0, reply) && reply[0] == 3);
        Feed(&state, keyboard7, sizeof(keyboard7), &sink);
        SDL_IntelWireless_ReplyDone(&state, SDL_INTEL_WIRELESS_REPLY_SIZE, 0);
        CHECK(!SDL_IntelWireless_NextReply(&state, 0, reply));

        SDL_IntelWireless_Init(&state);
        Feed(&state, activate7, sizeof(activate7), &sink);
        CHECK(SDL_IntelWireless_NextReply(&state, 0, reply) && reply[0] == 7);
        Feed(&state, mouse7, sizeof(mouse7), &sink);
        SDL_IntelWireless_ReplyDone(&state, -1, 0);
        CHECK(!SDL_IntelWireless_NextReply(&state, 1000000000, reply));
    }

    /* A completion with nothing in flight changes nothing. */
    SDL_IntelWireless_Init(&state);
    SDL_IntelWireless_ReplyDone(&state, -1, 0);
    CHECK(state.pending == 0 && !state.in_flight);
}

static void TestSlots(void)
{
    SDL_IntelWirelessState state;
    Recorder r;
    SDL_IntelWirelessSink sink = MakeSink(&r);
    uint8_t packet[13];
    int slot;

    /* All 8 slots are independent. */
    Reset(&r, 0xFF);
    SDL_IntelWireless_Init(&state);
    for (slot = 0; slot < SDL_INTEL_WIRELESS_SLOTS; ++slot) {
        uint8_t ready[3];
        ready[0] = 0x03;
        ready[1] = (uint8_t)slot;
        ready[2] = 0x04;
        Feed(&state, ready, sizeof(ready), &sink);
    }
    CHECK(r.count == SDL_INTEL_WIRELESS_SLOTS);
    for (slot = 0; slot < SDL_INTEL_WIRELESS_SLOTS; ++slot) {
        CHECK(r.events[slot].type == EVENT_CONNECT && r.events[slot].slot == slot);
    }
    memcpy(packet, input3_b, sizeof(packet));
    for (slot = 0; slot < SDL_INTEL_WIRELESS_SLOTS; ++slot) {
        packet[4] = (uint8_t)slot;
        packet[11] = (uint8_t)(1u << slot);
        r.count = 0;
        Feed(&state, packet, sizeof(packet), &sink);
        CHECK(r.count == 1 && r.events[0].slot == slot && r.events[0].buttons == (uint16_t)(1u << slot));
    }
    for (slot = 0; slot < SDL_INTEL_WIRELESS_SLOTS; ++slot) {
        CHECK(state.slots[slot].buttons == (uint16_t)(1u << slot));
    }

    /* Slot bits above bit 2 are masked: byte 4 of 0xFB is slot 3. */
    packet[4] = 0xFB;
    packet[11] = 0x80;
    r.count = 0;
    Feed(&state, packet, sizeof(packet), &sink);
    CHECK(r.count == 1 && r.events[0].slot == 3 && state.slots[3].buttons == SDL_INTEL_WIRELESS_BUTTON_R);
    {
        const uint8_t info_high[] = { 0x03, 0xFD, 0x05 }; /* slot 5, mouse ready */
        r.count = 0;
        Feed(&state, info_high, sizeof(info_high), &sink);
        CHECK(r.count == 1 && r.events[0].type == EVENT_DISCONNECT && r.events[0].slot == 5);
        CHECK(!state.slots[5].gamepad && !state.slots[5].connected);
    }
    /* So is the major type: 0xF9 is type 1. */
    packet[0] = 0xF9;
    packet[4] = 2;
    packet[11] = 0x01;
    r.count = 0;
    Feed(&state, packet, sizeof(packet), &sink);
    CHECK(r.count == 1 && r.events[0].slot == 2 && r.events[0].buttons == SDL_INTEL_WIRELESS_BUTTON_A);
    packet[0] = 0x01;

    /* Input on a slot never reported as a gamepad creates nothing. */
    Reset(&r, 0xFF);
    SDL_IntelWireless_Init(&state);
    packet[4] = 6;
    Feed(&state, packet, sizeof(packet), &sink);
    CHECK(r.count == 0 && !state.slots[6].connected);

    /* A remembered slot accepts input without an activation, and its first
     * input connects it. */
    {
        const uint8_t remembered6[] = { 0x03, 0x06, 0x0B, 0x02 };
        Feed(&state, remembered6, sizeof(remembered6), &sink);
        CHECK(r.count == 0);
        Feed(&state, packet, sizeof(packet), &sink);
        CHECK(r.count == 2 && r.events[0].type == EVENT_CONNECT && r.events[0].slot == 6);
        CHECK(r.events[1].type == EVENT_CONTROLS && r.events[1].slot == 6);
    }

    /* A slot that becomes a keyboard or a mouse is forgotten. */
    {
        static const uint8_t keyboard6[] = { 0x03, 0x06, 0x01, 0x00 };
        static const uint8_t ready6[] = { 0x03, 0x06, 0x04 };
        static const uint8_t mouse6[] = { 0x03, 0x06, 0x05 };
        static const uint8_t sees_mouse6[] = { 0x03, 0x06, 0x0A, 0x01 };
        r.count = 0;
        Feed(&state, keyboard6, sizeof(keyboard6), &sink);
        CHECK(r.count == 1 && r.events[0].type == EVENT_DISCONNECT && r.events[0].slot == 6);
        CHECK(!state.slots[6].gamepad);
        r.count = 0;
        Feed(&state, packet, sizeof(packet), &sink);
        CHECK(r.count == 0);

        Feed(&state, ready6, sizeof(ready6), &sink);
        CHECK(state.slots[6].connected);
        r.count = 0;
        Feed(&state, mouse6, sizeof(mouse6), &sink);
        CHECK(r.count == 1 && r.events[0].type == EVENT_DISCONNECT);
        CHECK(!state.slots[6].gamepad && !state.slots[6].connected);

        /* Another announcement with a changed type also forgets the slot. */
        Feed(&state, ready6, sizeof(ready6), &sink);
        r.count = 0;
        Feed(&state, sees_mouse6, sizeof(sees_mouse6), &sink);
        CHECK(r.count == 1 && r.events[0].type == EVENT_DISCONNECT);
        CHECK(!state.slots[6].gamepad);
        /* An announcement with the same type keeps the slot. */
        Feed(&state, ready6, sizeof(ready6), &sink);
        r.count = 0;
        {
            const uint8_t sees6[] = { 0x03, 0x06, 0x0A, 0x02 };
            Feed(&state, sees6, sizeof(sees6), &sink);
        }
        CHECK(r.count == 0 && state.slots[6].connected);
    }
}

static void TestZeroState(void)
{
    SDL_IntelWirelessState state;
    Recorder r;
    SDL_IntelWirelessSink sink = MakeSink(&r);
    uint8_t reply[SDL_INTEL_WIRELESS_REPLY_SIZE];

    Reset(&r, 0xFF);
    SessionToReady(&state, &sink);
    Feed(&state, input3_b, sizeof(input3_b), &sink);
    CHECK(state.slots[3].buttons == SDL_INTEL_WIRELESS_BUTTON_B);

    /* Zero state releases B and keeps the slot's type. */
    r.count = 0;
    Feed(&state, zero3, sizeof(zero3), &sink);
    CHECK(r.count == 1 && r.events[0].type == EVENT_CONTROLS && r.events[0].buttons == 0 && r.events[0].hat == 0);
    CHECK(state.slots[3].gamepad && state.slots[3].connected);

    /* The next input needs no new activation. */
    r.count = 0;
    Feed(&state, input3_b, sizeof(input3_b), &sink);
    CHECK(r.count == 1 && r.events[0].buttons == SDL_INTEL_WIRELESS_BUTTON_B);
    CHECK(!SDL_IntelWireless_NextReply(&state, 0, reply));
}

static void TestDescriptors(void)
{
    /* JoypadOS tools/intel-wireless-series/test.c:94-102. */
    static const uint8_t interface0[] = {
        9, 4, 0, 0, 1, 3, 1, 1, 0,
        9, 0x21, 0, 1, 0x21, 1, 0x22, 0x3f, 0,
        7, 5, 0x81, 3, 8, 0, 10,
        9, 4, 0, 1, 3, 0, 0, 0, 0,
        7, 5, 0x81, 3, 27, 0, 1,
        7, 5, 0x01, 3, 25, 0, 1,
        7, 5, 0x02, 0, 8, 0, 0,
    };
    const SDL_VendorUSBRule *rule = SDL_VendorUSB_FindRule(USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES, 0, 3, 1, 1);
    SDL_VendorUSBSelection selection;
    size_t n;

    CHECK(rule != NULL);
    for (n = 0; n <= sizeof(interface0); ++n) {
        uint8_t *copy = (uint8_t *)malloc(n ? n : 1);
        bool ok;
        if (!copy) {
            ++failures;
            return;
        }
        memcpy(copy, interface0, n);
        memset(&selection, 0, sizeof(selection));
        ok = SDL_VendorUSB_SelectEndpoints(rule, 0, copy, n, &selection);
        free(copy);
        CHECK(ok == (n == 48 || n == sizeof(interface0)));
        if (ok) {
            CHECK(selection.alternate == 1 && selection.in.address == 0x81 && selection.out.address == 0x01);
            CHECK(selection.in.address != 0x02 && selection.out.address != 0x02);
        }
    }
}

static void TestIgnored(void)
{
    SDL_IntelWirelessState state, before;
    Recorder r;
    SDL_IntelWirelessSink sink = MakeSink(&r);
    uint8_t packet[32];
    int type, operation;

    Reset(&r, 0xFF);
    SessionToReady(&state, &sink);
    r.count = 0;

    /* Longer than 27 bytes: nothing, even for a valid input packet. */
    memset(packet, 0, sizeof(packet));
    memcpy(packet, input3_b, sizeof(input3_b));
    before = state;
    Feed(&state, packet, 28, &sink);
    CHECK(SameState(&state, &before) && r.count == 0);
    /* 27 bytes is the most a read carries, and acts. */
    Feed(&state, packet, 27, &sink);
    CHECK(r.count == 1);
    r.count = 0;

    /* Major types 0, 2, 4, 5 and 7. */
    for (type = 0; type < 8; ++type) {
        if (type == 1 || type == 3 || type == 6) {
            continue;
        }
        memcpy(packet, input3_b, sizeof(input3_b));
        packet[0] = (uint8_t)type;
        before = state;
        Feed(&state, packet, sizeof(input3_b), &sink);
        CHECK(SameState(&state, &before) && r.count == 0);
        packet[0] = (uint8_t)(0x08 | type);
        Feed(&state, packet, sizeof(input3_b), &sink);
        CHECK(SameState(&state, &before) && r.count == 0);
    }

    /* Information operations other than 1, 4, 5, 0x0A, 0x0B and 0x0C, on a
     * connected slot and on an unknown one. */
    for (operation = 0; operation < 256; ++operation) {
        uint8_t info[4] = { 0x03, 0x03, 0, 0x00 };
        uint8_t unknown[4] = { 0x03, 0x05, 0, 0x02 };
        if (operation == 1 || operation == 4 || operation == 5 ||
            operation == 0x0A || operation == 0x0B || operation == 0x0C) {
            continue;
        }
        info[2] = (uint8_t)operation;
        unknown[2] = (uint8_t)operation;
        before = state;
        Feed(&state, info, sizeof(info), &sink);
        Feed(&state, unknown, sizeof(unknown), &sink);
        CHECK(SameState(&state, &before) && r.count == 0);
    }

    /* Null and empty input. */
    before = state;
    SDL_IntelWireless_HandlePacket(&state, NULL, 13, &sink);
    SDL_IntelWireless_HandlePacket(&state, input3_b, 0, &sink);
    SDL_IntelWireless_HandlePacket(&state, input3_b, sizeof(input3_b), NULL);
    SDL_IntelWireless_HandlePacket(NULL, input3_b, sizeof(input3_b), &sink);
    CHECK(SameState(&state, &before) && r.count == 0);
}

static void TestReactivationAndDetach(void)
{
    SDL_IntelWirelessState state;
    Recorder r;
    SDL_IntelWirelessSink sink = MakeSink(&r);
    uint8_t reply[SDL_INTEL_WIRELESS_REPLY_SIZE];
    static const uint8_t activate3_short[] = { 0x03, 0x03, 0x01, 0x02 };

    Reset(&r, 0xFF);
    SessionToReady(&state, &sink);
    Feed(&state, input3_b, sizeof(input3_b), &sink);
    CHECK(state.slots[3].connected);

    /* A fresh activation on connected slot 3 disconnects it and queues a
     * reply. */
    r.count = 0;
    Feed(&state, activate3_short, sizeof(activate3_short), &sink);
    CHECK(r.count == 1 && r.events[0].type == EVENT_DISCONNECT && r.events[0].slot == 3);
    CHECK(!state.slots[3].connected && state.slots[3].gamepad && state.slots[3].buttons == 0);
    CHECK(SDL_IntelWireless_NextReply(&state, 0, reply));
    CHECK(memcmp(reply, reply3, sizeof(reply3)) == 0);
    SDL_IntelWireless_ReplyDone(&state, SDL_INTEL_WIRELESS_REPLY_SIZE, 0);

    /* It connects again on its next ready. */
    r.count = 0;
    Feed(&state, ready3, sizeof(ready3), &sink);
    CHECK(r.count == 1 && r.events[0].type == EVENT_CONNECT && r.events[0].slot == 3);

    /* Or on its next input. */
    Feed(&state, activate3_short, sizeof(activate3_short), &sink);
    r.count = 0;
    Feed(&state, input3_b, sizeof(input3_b), &sink);
    CHECK(r.count == 2 && r.events[0].type == EVENT_CONNECT && r.events[1].type == EVENT_CONTROLS);

    /* Removing the base station disconnects every connected slot. */
    {
        static const uint8_t ready0[] = { 0x03, 0x00, 0x04 };
        static const uint8_t ready7[] = { 0x03, 0x07, 0x04 };
        int disconnects = 0, i;
        Feed(&state, ready0, sizeof(ready0), &sink);
        Feed(&state, ready7, sizeof(ready7), &sink);
        Feed(&state, remembered1, sizeof(remembered1), &sink); /* known, not connected */
        r.count = 0;
        SDL_IntelWireless_Detach(&state, &sink);
        for (i = 0; i < r.count; ++i) {
            CHECK(r.events[i].type == EVENT_DISCONNECT);
            CHECK(r.events[i].slot == 0 || r.events[i].slot == 3 || r.events[i].slot == 7);
            ++disconnects;
        }
        CHECK(disconnects == 3);
        for (i = 0; i < SDL_INTEL_WIRELESS_SLOTS; ++i) {
            CHECK(!state.slots[i].connected && !state.slots[i].gamepad);
        }
        CHECK(state.pending == 0 && !state.in_flight);
    }
}

static void TestUnopenedAndRefresh(void)
{
    SDL_IntelWirelessState state;
    Recorder r;
    SDL_IntelWirelessSink sink = MakeSink(&r);

    /* A connected slot the application has not opened gets no controls. */
    Reset(&r, 0x00);
    SessionToReady(&state, &sink);
    r.count = 0;
    Feed(&state, input3_b, sizeof(input3_b), &sink);
    CHECK(r.count == 0);
    CHECK(state.slots[3].buttons == SDL_INTEL_WIRELESS_BUTTON_B);

    /* Once it is open, the held B reaches it. */
    r.open = 0x08;
    SDL_IntelWireless_Refresh(&state, 3, &sink);
    CHECK(r.count == 1 && r.events[0].slot == 3 && r.events[0].buttons == SDL_INTEL_WIRELESS_BUTTON_B);

    /* Refresh does nothing for a slot that is not connected or is out of range. */
    r.count = 0;
    r.open = 0xFF;
    SDL_IntelWireless_Refresh(&state, 1, &sink);
    SDL_IntelWireless_Refresh(&state, -1, &sink);
    SDL_IntelWireless_Refresh(&state, SDL_INTEL_WIRELESS_SLOTS, &sink);
    CHECK(r.count == 0);

    /* Only slot 0 open: input on slot 3 sends nothing. */
    Reset(&r, 0x01);
    SessionToReady(&state, &sink);
    r.count = 0;
    Feed(&state, input3_released, sizeof(input3_released), &sink);
    CHECK(r.count == 0);
}

/* The mapping the part specifies: A, B, X and Y are the face buttons, Z and C
 * the left and right shoulders, L and R the left and right triggers, Start is
 * Start, Mouse is Back, Shift is Guide, and the D-pad is the hat. */
static void TestMapping(void)
{
    const struct
    {
        uint16_t button;
        const char *name;
    } expected[] = {
        { SDL_INTEL_WIRELESS_BUTTON_A, "a" },
        { SDL_INTEL_WIRELESS_BUTTON_B, "b" },
        { SDL_INTEL_WIRELESS_BUTTON_X, "x" },
        { SDL_INTEL_WIRELESS_BUTTON_Y, "y" },
        { SDL_INTEL_WIRELESS_BUTTON_Z, "leftshoulder" },
        { SDL_INTEL_WIRELESS_BUTTON_C, "rightshoulder" },
        { SDL_INTEL_WIRELESS_BUTTON_L, "lefttrigger" },
        { SDL_INTEL_WIRELESS_BUTTON_R, "righttrigger" },
        { SDL_INTEL_WIRELESS_BUTTON_START, "start" },
        { SDL_INTEL_WIRELESS_BUTTON_MOUSE, "back" },
        { SDL_INTEL_WIRELESS_BUTTON_SHIFT, "guide" },
    };
    const char *mapping = SDL_INTEL_WIRELESS_MAPPING;
    size_t i;
    int index;
    uint16_t seen = 0;

    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        char entry[64];
        for (index = 0; index < SDL_INTEL_WIRELESS_JOYSTICK_BUTTONS; ++index) {
            if (SDL_IntelWireless_ButtonOrder[index] == expected[i].button) {
                break;
            }
        }
        CHECK(index < SDL_INTEL_WIRELESS_JOYSTICK_BUTTONS);
        snprintf(entry, sizeof(entry), "%s:b%d,", expected[i].name, index);
        {
            /* Match at an element boundary, not inside another name. */
            const char *at = strstr(mapping, entry);
            CHECK(at != NULL && (at == mapping || at[-1] == ','));
        }
        seen |= expected[i].button;
    }
    CHECK(seen == 0x07FF);
    CHECK(strstr(mapping, "dpup:h0.1,") && strstr(mapping, "dpdown:h0.4,") &&
          strstr(mapping, "dpleft:h0.8,") && strstr(mapping, "dpright:h0.2,"));
    /* The hat bits carry SDL's hat values. */
    CHECK(SDL_INTEL_WIRELESS_HAT_UP == 0x01 && SDL_INTEL_WIRELESS_HAT_RIGHT == 0x02 &&
          SDL_INTEL_WIRELESS_HAT_DOWN == 0x04 && SDL_INTEL_WIRELESS_HAT_LEFT == 0x08);
}

int main(void)
{
    TestCapturedSession();
    TestTruncations();
    TestControls();
    TestReplyQueue();
    TestSlots();
    TestZeroState();
    TestDescriptors();
    TestIgnored();
    TestReactivationAndDetach();
    TestUnopenedAndRefresh();
    TestMapping();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
