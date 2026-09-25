/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_rcadapter_proto.c, the
   PhoenixRC USB adapter of hifihedgehog/SDL#33 Part 7. The numbers are the
   part's, and the packets are built from hid-pxrc and hid-rcsim. Every
   report is fed as an exact-size heap copy, and truncated inside a buffer
   of 0xFF bytes. */

#include "../src/joystick/hidapi/SDL_hidapi_rcadapter_proto.h"

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

static int16_t Axis(uint8_t value)
{
    return (int16_t)((int)value * 257 - 32768);
}

static bool Feed(SDL_RCAdapterState *state, const uint8_t *report, size_t length)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    result = SDL_RCAdapter_HandleReport(state, copy, length);
    free(copy);
    return result;
}

static void TestChannels(void)
{
    static const uint8_t first[8] = { 0x10, 0xAA, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 };
    static const uint8_t second[8] = { 0x10, 0xAA, 0x20, 0x30, 0x40, 0x50, 0x60, 0x80 };
    static const uint8_t repeat8[8] = { 0x10, 0x00, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 };
    static const uint8_t repeat7[8] = { 0x10, 0xFF, 0x20, 0x30, 0x40, 0x50, 0x60, 0x80 };
    static const int byte_of_channel[6] = { 0, 2, 3, 4, 5, 6 };
    static const uint8_t values[3] = { 0x00, 0x80, 0xFF };
    SDL_RCAdapterState state, before;
    uint8_t r[8];
    int c, v, i;

    /* 1: channels 1-6 and channel 8, channel 7 still centered */
    SDL_RCAdapter_Init(&state);
    for (i = 0; i < 8; ++i) {
        CHECK(state.axes[i] == 0);
    }
    CHECK(Feed(&state, first, 8));
    CHECK(state.axes[0] == Axis(0x10) && state.axes[1] == Axis(0x20) && state.axes[2] == Axis(0x30));
    CHECK(state.axes[3] == Axis(0x40) && state.axes[4] == Axis(0x50) && state.axes[5] == Axis(0x60));
    CHECK(state.axes[7] == Axis(0x70) && state.axes[6] == 0);

    /* 2: channel 7 from the next report, channel 8 held */
    CHECK(Feed(&state, second, 8));
    CHECK(state.axes[6] == Axis(0x80) && state.axes[7] == Axis(0x70));

    /* 3: byte 1 is ignored, and byte 7 repeats what channels 8 and 7 hold */
    before = state;
    CHECK(Feed(&state, repeat8, 8));
    CHECK(Feed(&state, repeat7, 8));
    CHECK(memcmp(state.axes, before.axes, sizeof(state.axes)) == 0);

    /* 4: each of bytes 0 and 2-6 alone moves only its channel */
    for (c = 0; c < 6; ++c) {
        for (v = 0; v < 3; ++v) {
            memcpy(r, state.next_is_channel_7 ? repeat7 : repeat8, 8);
            r[byte_of_channel[c]] = values[v];
            before = state;
            CHECK(Feed(&state, r, 8));
            for (i = 0; i < 8; ++i) {
                if (i == c) {
                    CHECK(state.axes[i] == Axis(values[v]));
                } else {
                    CHECK(state.axes[i] == before.axes[i]);
                }
            }
            /* Put the channel back */
            memcpy(r, state.next_is_channel_7 ? repeat7 : repeat8, 8);
            CHECK(Feed(&state, r, 8));
        }
    }
    CHECK(Axis(0x00) == -32768 && Axis(0x80) == 128 && Axis(0xFF) == 32767);
}

static void TestFraming(void)
{
    static const uint8_t first[8] = { 0x10, 0xAA, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 };
    SDL_RCAdapterState state, before;
    uint8_t stale[16], nine[9];
    size_t length;

    /* 5: every prefix changes nothing and does not advance the byte 7 channel */
    SDL_RCAdapter_Init(&state);
    for (length = 0; length < 8; ++length) {
        before = state;
        CHECK(!Feed(&state, first, length));
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, first, length);
        CHECK(!SDL_RCAdapter_HandleReport(&state, stale, length));
        CHECK(memcmp(&state, &before, sizeof(state)) == 0);
    }
    /* A 9-byte read is ignored the same way */
    memcpy(nine, first, 8);
    nine[8] = 0x00;
    CHECK(!Feed(&state, nine, 9));
    CHECK(memcmp(&state, &before, sizeof(state)) == 0);
    /* The first complete report still lands on channel 8 */
    CHECK(Feed(&state, first, 8) && state.axes[7] == Axis(0x70) && state.axes[6] == 0);
    CHECK(!SDL_RCAdapter_HandleReport(NULL, first, 8));
    CHECK(!SDL_RCAdapter_HandleReport(&state, NULL, 8));

    /* 6: a reconnect starts over on channel 8 */
    CHECK(Feed(&state, first, 8) && state.axes[6] == Axis(0x70));
    SDL_RCAdapter_Init(&state);
    CHECK(Feed(&state, first, 8) && state.axes[7] == Axis(0x70) && state.axes[6] == 0);
}

int main(void)
{
    TestChannels();
    TestFraming();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
