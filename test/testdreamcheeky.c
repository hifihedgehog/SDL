/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_dreamcheeky_proto.c, the
   Dream Cheeky drum kit of hifihedgehog/SDL#33 Part 7, built from VRPN's
   reading of it. */

#include "../src/joystick/hidapi/SDL_hidapi_dreamcheeky_proto.h"

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

static bool Decode(const uint8_t *report, size_t length, uint8_t *pads)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    *pads = 0xA5;
    result = SDL_DreamCheeky_DecodeReport(copy, length, pads);
    free(copy);
    return result;
}

int main(void)
{
    static const uint8_t held4[8] = { 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t held3[8] = { 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t r[8], stale[16], nine[9], pads;
    int pad, count, i;
    size_t length;

    /* 1 and 2: 4 of 8 holds a pad, 3 of 8 does not */
    CHECK(Decode(held4, 8, &pads) && pads == 0x01);
    CHECK(Decode(held3, 8, &pads) && pads == 0x00);

    /* 3: all six, and bits 6 and 7 alone hold nothing */
    memset(r, 0x3F, 8);
    CHECK(Decode(r, 8, &pads) && pads == 0x3F);
    memset(r, 0xC0, 8);
    CHECK(Decode(r, 8, &pads) && pads == 0x00);

    /* 4: each pad bit alone in all 8 bytes, and every count of set bytes */
    for (pad = 0; pad < 6; ++pad) {
        memset(r, 1 << pad, 8);
        CHECK(Decode(r, 8, &pads) && pads == (1 << pad));
        for (count = 0; count <= 8; ++count) {
            memset(r, 0, 8);
            for (i = 0; i < count; ++i) {
                r[7 - i] = (uint8_t)(1 << pad);
            }
            CHECK(Decode(r, 8, &pads) && pads == (count >= 4 ? (1 << pad) : 0));
        }
    }

    /* 5: every prefix changes nothing, with 0xFF past the received length, and a 9-byte read is ignored */
    for (length = 0; length < 8; ++length) {
        CHECK(!Decode(held4, length, &pads));
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, held4, length);
        CHECK(!SDL_DreamCheeky_DecodeReport(stale, length, &pads));
    }
    memcpy(nine, held4, 8);
    nine[8] = 0;
    CHECK(!Decode(nine, 9, &pads));
    CHECK(!SDL_DreamCheeky_DecodeReport(NULL, 8, &pads));
    CHECK(!SDL_DreamCheeky_DecodeReport(held4, 8, NULL));

    /* 6: the decode keeps no state, so nothing outlives a reconnect: after
       all six pads, a report with none releases every pad */
    memset(r, 0x3F, 8);
    CHECK(Decode(r, 8, &pads) && pads == 0x3F);
    memset(r, 0x00, 8);
    CHECK(Decode(r, 8, &pads) && pads == 0x00);

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
