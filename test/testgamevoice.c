/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_gamevoice_proto.c, the
   SideWinder Game Voice of hifihedgehog/SDL#33 Part 7. Test 1 is the byte
   the Windows USBSnoop log of May 2007 recorded. */

#include "../src/joystick/hidapi/SDL_hidapi_gamevoice_proto.h"

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

static bool Decode(const uint8_t *report, size_t length, uint8_t *buttons)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    *buttons = 0xA5;
    result = SDL_GameVoice_DecodeReport(copy, length, buttons);
    free(copy);
    return result;
}

int main(void)
{
    uint8_t r[4], buttons;
    int bit, value;

    /* 1: the captured 0C holds buttons 2 and 3 */
    r[0] = 0x0C;
    CHECK(Decode(r, 1, &buttons) && buttons == ((1 << 2) | (1 << 3)));

    /* 2: each bit alone, none and all */
    for (bit = 0; bit < 8; ++bit) {
        r[0] = (uint8_t)(1 << bit);
        CHECK(Decode(r, 1, &buttons) && buttons == (1 << bit));
    }
    r[0] = 0x00;
    CHECK(Decode(r, 1, &buttons) && buttons == 0);
    r[0] = 0xFF;
    CHECK(Decode(r, 1, &buttons) && buttons == 0xFF);
    for (value = 0; value < 256; ++value) {
        r[0] = (uint8_t)value;
        CHECK(Decode(r, 1, &buttons) && buttons == value);
    }

    /* 3: a 0-byte read changes nothing, and a 1-byte report in a buffer of 0xFF decodes the same */
    buttons = 0x5A;
    CHECK(!SDL_GameVoice_DecodeReport(r, 0, &buttons) && buttons == 0x5A);
    memset(r, 0xFF, sizeof(r));
    r[0] = 0x0C;
    CHECK(SDL_GameVoice_DecodeReport(r, 1, &buttons) && buttons == 0x0C);

    /* 4: a read of 2 or more bytes is ignored */
    CHECK(!Decode(r, 2, &buttons) && !Decode(r, 3, &buttons) && !Decode(r, 4, &buttons));
    CHECK(!SDL_GameVoice_DecodeReport(NULL, 1, &buttons));
    CHECK(!SDL_GameVoice_DecodeReport(r, 1, NULL));

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
