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

/* Standalone C99 test, with no SDL runtime or Windows headers.
 * cc -std=c99 -Wall -Wextra -Werror -pedantic test/testxinputpaddledecode.c \
 *    src/joystick/windows/SDL_xinput_paddle_decode.c -o testxinputpaddledecode
 */
#include "../src/joystick/windows/SDL_xinput_paddle_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define BT SDL_XINPUT_PADDLE_VENDOR_GATT
#define GIP SDL_XINPUT_PADDLE_SERVICE_GIP
#define NORMAL SDL_XINPUT_PADDLE_NORMAL_READY
#define RAW SDL_XINPUT_PADDLE_RAW_NIBBLE
#define READY SDL_XINPUT_PADDLE_STATE_READY
#define SPLIT SDL_XINPUT_PADDLE_SEPARATE_FRAME

static unsigned checks;
static const char *test_name;
static unsigned capture_line;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s: line %d, capture line %u: %s\n", \
                test_name, __LINE__, capture_line, #condition); \
        exit(1); \
    } \
} while (0)

typedef struct ActualFixture
{
    int bluetooth;
    unsigned line;
    uint16_t word;
    uint8_t mask;
    uint8_t profile;
    const char *hex;
} ActualFixture;

typedef struct CaptureRun
{
    unsigned line;
    unsigned repeat;
    uint32_t report_id;
    uint16_t word;
    uint8_t mask;
    const char *hex;
} CaptureRun;

/* BEGIN CAPTURE FIXTURES */
/* Actual firmware 5.13.3146.0/profile 0 capture bytes.
 * RLE merges consecutive identical payloads only. Replay expands every
 * report in order. USB includes all 17 ordinary and 16 separate reports.
 * No physical paddle-name experiment is inferred from these captures.
 * vendor-notify-3.log
 * SHA256 7ad419411ca291d60fe505d03dac0d6084ece27511a3a556dda24ff5a80ded7e
 * wgi-usb-background-68772-165963968.log
 * SHA256 85239d1b827d22ba62ec44997a6b0ba58f78c85229b511c9b6564c35f636e735
 * elite-paddle-corpus.json
 * SHA256 35390220c5f165659c73895cc565672f39643cc6738cdaf53c700891f24328d2
 */
static const ActualFixture actual_fixtures[] = {
    { 1, 4, 0x0000, 0x00, 0, "0000220100002902a7f93afcd307000000" },
    { 1, 351, 0x0000, 0x01, 0, "00000000000012fafb0194fb2afb010000" },
    { 1, 401, 0x0000, 0x02, 0, "00000000000012fafb0194fb2afb020000" },
    { 1, 326, 0x0000, 0x04, 0, "00000000000012fafb0194fb2afb040000" },
    { 1, 375, 0x0000, 0x08, 0, "00000000000012fafb0194fb2afb080000" },
    { 1, 428, 0x0000, 0x0F, 0, "00000000000012fafb0194fb2afb0f0000" },
    { 1, 469, 0x0010, 0x0F, 0, "10000000000012fafb0194fb2afb0f0000" },
    { 0, 41, 0x0000, 0x00, 0, "0000000000001cfdbcfd86f766fd000000" },
    { 0, 49, 0x0000, 0x01, 0, "0000000000001cfdbcfd86f766fd010000" },
    { 0, 57, 0x0000, 0x02, 0, "0000000000001cfdbcfd86f766fd020000" },
    { 0, 45, 0x0000, 0x04, 0, "0000000000001cfdbcfd86f766fd040000" },
    { 0, 53, 0x0000, 0x08, 0, "0000000000001cfdbcfd86f766fd080000" },
    { 0, 69, 0x0000, 0x0F, 0, "0000000000001cfdbcfd86f766fd0f0000" },
    { 0, 67, 0x0010, 0x0F, 0, "1000000000001cfdbcfd86f766fd0f0000" },
};
static const CaptureRun bt_replay[] = {
    { 4, 9, 0x00, 0x0000, 0x00, "0000220100002902a7f93afcd307000000" },
    { 13, 5, 0x00, 0x0000, 0x00, "0000200100002902a7f93afcd307000000" },
    { 18, 1, 0x00, 0x0000, 0x00, "00001e0100002902a7f93afcd307000000" },
    { 19, 4, 0x00, 0x0000, 0x00, "00001c0100002902a7f93afcd307000000" },
    { 23, 20, 0x00, 0x0000, 0x00, "00001a0100002902a7f93afcd307000000" },
    { 43, 20, 0x00, 0x0000, 0x00, "00001c0100002902a7f93afcd307000000" },
    { 63, 20, 0x00, 0x0000, 0x00, "00001e0100002902a7f93afcd307000000" },
    { 83, 1, 0x00, 0x0000, 0x00, "0000250100002902a7f93afcd307000000" },
    { 84, 1, 0x00, 0x0000, 0x00, "0000300100002902a7f93afcd307000000" },
    { 85, 1, 0x00, 0x0000, 0x00, "00003b0100002902a7f93afcd307000000" },
    { 86, 1, 0x00, 0x0000, 0x00, "0000540100002902a7f93afcd307000000" },
    { 87, 1, 0x00, 0x0000, 0x00, "0000580100002902a7f93afcd307000000" },
    { 88, 2, 0x00, 0x0000, 0x00, "0000610100002902a7f93afcd307000000" },
    { 90, 1, 0x00, 0x0000, 0x00, "00005f0100002902a7f93afcd307000000" },
    { 91, 1, 0x00, 0x0000, 0x00, "0000620100002902a7f93afcd307000000" },
    { 92, 1, 0x00, 0x0000, 0x00, "00006a0100002902a7f93afcd307000000" },
    { 93, 1, 0x00, 0x0000, 0x00, "0000880100002902a7f93afcd307000000" },
    { 94, 1, 0x00, 0x0000, 0x00, "0000040200002902a7f93afcd307000000" },
    { 95, 1, 0x00, 0x0000, 0x00, "0000180200002902a7f93afcd307000000" },
    { 96, 2, 0x00, 0x0000, 0x00, "0000820200002902a7f93afcd307000000" },
    { 98, 1, 0x00, 0x0000, 0x00, "00007f0200002902a7f93afcd307000000" },
    { 99, 1, 0x00, 0x0000, 0x00, "00007d0200002902a7f93afcd307000000" },
    { 100, 1, 0x00, 0x0000, 0x00, "0000760200002902a7f93afcd307000000" },
    { 101, 1, 0x00, 0x0000, 0x00, "00005f0200002902a7f93afcd307000000" },
    { 102, 1, 0x00, 0x0000, 0x00, "0000450200002902a7f93afcd307000000" },
    { 103, 1, 0x00, 0x0000, 0x00, "0000240200002902a7f93afcd307000000" },
    { 104, 1, 0x00, 0x0000, 0x00, "0000f30000002902a7f93afcd307000000" },
    { 105, 5, 0x00, 0x0000, 0x00, "0000000000002902a7f93afcd307000000" },
    { 110, 1, 0x00, 0x0000, 0x00, "000000000000290253fa3afcd307000000" },
    { 111, 1, 0x00, 0x0000, 0x00, "000000000000290275fb3afcd307000000" },
    { 112, 1, 0x00, 0x0000, 0x00, "00000000000029022afe3afcd307000000" },
    { 113, 1, 0x00, 0x0000, 0x00, "000000000000290255003afcd307000000" },
    { 114, 1, 0x00, 0x0000, 0x00, "0000000000002902bd013afcd307000000" },
    { 115, 1, 0x00, 0x0000, 0x00, "0000000000002902a3023afcd307000000" },
    { 116, 1, 0x00, 0x0000, 0x00, "000000000000290240033afcd307000000" },
    { 117, 1, 0x00, 0x0000, 0x00, "0000000000002902b7033afcd307000000" },
    { 118, 1, 0x00, 0x0000, 0x00, "0000000000002902e2033afcd307000000" },
    { 119, 1, 0x00, 0x0000, 0x00, "00000000000029021f043afcd307000000" },
    { 120, 9, 0x00, 0x0000, 0x00, "000000000000290247043afcd307000000" },
    { 129, 1, 0x00, 0x0000, 0x00, "000000000000d20147043afcd307000000" },
    { 130, 1, 0x00, 0x0000, 0x00, "000000000000a60147043afcd307000000" },
    { 131, 1, 0x00, 0x0000, 0x00, "0000000000002c0147043afcd307000000" },
    { 132, 1, 0x00, 0x0000, 0x00, "000000000000ab0047043afcd307000000" },
    { 133, 1, 0x00, 0x0000, 0x00, "0000000000003b0047043afcd307000000" },
    { 134, 1, 0x00, 0x0000, 0x00, "000000000000bdff47043afcd307000000" },
    { 135, 1, 0x00, 0x0000, 0x00, "00000000000054ff47043afcd307000000" },
    { 136, 1, 0x00, 0x0000, 0x00, "000000000000d8fe47043afcd307000000" },
    { 137, 1, 0x00, 0x0000, 0x00, "00000000000058fe47043afcd307000000" },
    { 138, 1, 0x00, 0x0000, 0x00, "000000000000f3fd47043afcd307000000" },
    { 139, 1, 0x00, 0x0000, 0x00, "000000000000a8fd47043afcd307000000" },
    { 140, 1, 0x00, 0x0000, 0x00, "000000000000ecfc47043afcd307000000" },
    { 141, 1, 0x00, 0x0000, 0x00, "00000000000009fc47043afcd307000000" },
    { 142, 1, 0x00, 0x0000, 0x00, "00000000000072fb47043afcd307000000" },
    { 143, 1, 0x00, 0x0000, 0x00, "0000000000000dfb47043afcd307000000" },
    { 144, 1, 0x00, 0x0000, 0x00, "000000000000ccfa47043afcd307000000" },
    { 145, 2, 0x00, 0x0000, 0x00, "000000000000a3fa47043afcd307000000" },
    { 147, 1, 0x00, 0x0000, 0x00, "00000000000065fa47043afcd307000000" },
    { 148, 1, 0x00, 0x0000, 0x00, "0000000000001bfa47043afcd307000000" },
    { 149, 1, 0x00, 0x0000, 0x00, "000000000000aaf947043afc9806000000" },
    { 150, 1, 0x00, 0x0000, 0x00, "0000000000000cf94704f4fb8702000000" },
    { 151, 1, 0x00, 0x0000, 0x00, "00000000000045fa4704f4fbe1ff000000" },
    { 152, 1, 0x00, 0x0000, 0x00, "000000000000e8fcbe04d2fb2ffe000000" },
    { 153, 1, 0x00, 0x0000, 0x00, "00000000000017fe0d05d2fb1afd000000" },
    { 154, 1, 0x00, 0x0000, 0x00, "0000000000003cfc2605d2fb68fc000000" },
    { 155, 1, 0x00, 0x0000, 0x00, "000000000000fdfa2605d2fbf1fb000000" },
    { 156, 1, 0x00, 0x0000, 0x00, "00000000000037fa2605d2fbc4fb000000" },
    { 157, 1, 0x00, 0x0000, 0x00, "000000000000b7f9260594fb8efb000000" },
    { 158, 1, 0x00, 0x0000, 0x00, "0000000000008cf9260594fb5ffb000000" },
    { 159, 1, 0x00, 0x0000, 0x00, "0000000000004ef9260594fb5ffb000000" },
    { 160, 2, 0x00, 0x0000, 0x00, "00000000000026f9260594fb5ffb000000" },
    { 162, 1, 0x00, 0x0000, 0x00, "00000000000026f99d0494fb2afb000000" },
    { 163, 1, 0x00, 0x0000, 0x00, "00000000000026f9600494fb2afb000000" },
    { 164, 1, 0x00, 0x0000, 0x00, "00000000000026f9cc0394fb2afb000000" },
    { 165, 1, 0x00, 0x0000, 0x00, "000000000000c1f9550394fb2afb000000" },
    { 166, 1, 0x00, 0x0000, 0x00, "000000000000c1f9230394fb2afb000000" },
    { 167, 3, 0x00, 0x0000, 0x00, "000000000000c1f9c10294fb2afb000000" },
    { 170, 2, 0x00, 0x0000, 0x00, "000000000000c1f9760294fb2afb000000" },
    { 172, 8, 0x00, 0x0000, 0x00, "000000000000c1f95c0294fb2afb000000" },
    { 180, 11, 0x00, 0x0000, 0x00, "000000000000c1f9160294fb2afb000000" },
    { 191, 20, 0x00, 0x0000, 0x00, "000000000000c1f9fb0194fb2afb000000" },
    { 211, 20, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb2afb000000" },
    { 231, 5, 0x00, 0x0010, 0x00, "10000000000012fafb0194fb2afb000000" },
    { 236, 20, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb2afb000000" },
    { 256, 3, 0x00, 0x0020, 0x00, "20000000000012fafb0194fb2afb000000" },
    { 259, 20, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb2afb000000" },
    { 279, 3, 0x00, 0x0040, 0x00, "40000000000012fafb0194fb2afb000000" },
    { 282, 20, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb2afb000000" },
    { 302, 4, 0x00, 0x0080, 0x00, "80000000000012fafb0194fb2afb000000" },
    { 306, 20, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb2afb000000" },
    { 326, 5, 0x00, 0x0000, 0x04, "00000000000012fafb0194fb2afb040000" },
    { 331, 20, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb2afb000000" },
    { 351, 4, 0x00, 0x0000, 0x01, "00000000000012fafb0194fb2afb010000" },
    { 355, 20, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb2afb000000" },
    { 375, 6, 0x00, 0x0000, 0x08, "00000000000012fafb0194fb2afb080000" },
    { 381, 20, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb2afb000000" },
    { 401, 5, 0x00, 0x0000, 0x02, "00000000000012fafb0194fb2afb020000" },
    { 406, 20, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb2afb000000" },
    { 426, 1, 0x00, 0x0000, 0x0A, "00000000000012fafb0194fb2afb0a0000" },
    { 427, 1, 0x00, 0x0000, 0x0B, "00000000000012fafb0194fb2afb0b0000" },
    { 428, 18, 0x00, 0x0000, 0x0F, "00000000000012fafb0194fb2afb0f0000" },
    { 446, 1, 0x00, 0x0000, 0x0C, "00000000000012fafb0194fb2afb0c0000" },
    { 447, 20, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb2afb000000" },
    { 467, 1, 0x00, 0x0000, 0x08, "00000000000012fafb0194fb2afb080000" },
    { 468, 1, 0x00, 0x0010, 0x0B, "10000000000012fafb0194fb2afb0b0000" },
    { 469, 20, 0x00, 0x0010, 0x0F, "10000000000012fafb0194fb2afb0f0000" },
    { 489, 1, 0x00, 0x0010, 0x0E, "10000000000012fafb0194fb2afb0e0000" },
    { 490, 1, 0x00, 0x0010, 0x0C, "10000000000012fafb0194fb2afb0c0000" },
    { 491, 20, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb2afb000000" },
    { 511, 4, 0x00, 0x0000, 0x00, "00000000000012fafb0194fbfffa000000" },
    { 515, 1, 0x00, 0x0000, 0x00, "00000000000012fafb0194fbb8fa000000" },
    { 516, 2, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb93fa000000" },
    { 518, 2, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb56fa000000" },
    { 520, 9, 0x00, 0x0000, 0x00, "00000000000012fafb0194fb33fa000000" },
    { 529, 10, 0x00, 0x0000, 0x00, "00000000000012fafb0172fb33fa000000" },
    { 539, 20, 0x00, 0x0000, 0x00, "00000000000012fafb0172fbf5f9000000" },
};
static const char bt_edges[] = "040108020abfc08bfec0";
static const CaptureRun usb_replay[] = {
    { 32, 1, 0x20, 0x0010, 0x00, "1000000000001cfdbcfd86f766fd00000000000000000000000000000000000000000000000063e3f79abae6f79a" },
    { 40, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd0000000000000000000000000000000000000000000000005735fa9a4739fa9a" },
    { 41, 1, 0x0C, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000" },
    { 44, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd0000000000000000000000000000000000000000000000003ac2079b91c5079b" },
    { 45, 1, 0x0C, 0x0000, 0x04, "0000000000001cfdbcfd86f766fd040000" },
    { 46, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd0000000000000000000000000000000000000000000000006ed5099b88d8099b" },
    { 47, 1, 0x0C, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000" },
    { 48, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000000000000000000000000000000000000000000000529e0d9b8aa10d9b" },
    { 49, 1, 0x0C, 0x0000, 0x01, "0000000000001cfdbcfd86f766fd010000" },
    { 50, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd00000000000000000000000000000000000000000000000045920f9b7e950f9b" },
    { 51, 1, 0x0C, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000" },
    { 52, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000000000000000000000000000000000000000000000ad111a9b23151a9b" },
    { 53, 1, 0x0C, 0x0000, 0x08, "0000000000001cfdbcfd86f766fd080000" },
    { 54, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000000000000000000000000000000000000000000000e7ff1c9b20031d9b" },
    { 55, 1, 0x0C, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000" },
    { 56, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000000000000000000000000000000000000000000000dbf31e9b32f71e9b" },
    { 57, 1, 0x0C, 0x0000, 0x02, "0000000000001cfdbcfd86f766fd020000" },
    { 58, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd0000000000000000000000000000000000000000000000005e9d229bb5a0229b" },
    { 59, 1, 0x0C, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000" },
    { 60, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd00000000000000000000000000000000000000000000000094df319b47e3319b" },
    { 61, 1, 0x0C, 0x0000, 0x09, "0000000000001cfdbcfd86f766fd090000" },
    { 62, 1, 0x20, 0x0010, 0x00, "1000000000001cfdbcfd86f766fd000000000000000000000000000000000000000000000000b33d329beb40329b" },
    { 63, 1, 0x0C, 0x0010, 0x0B, "1000000000001cfdbcfd86f766fd0b0000" },
    { 66, 1, 0x20, 0x0010, 0x00, "1000000000001cfdbcfd86f766fd000000000000000000000000000000000000000000000000d65c329b6a60329b" },
    { 67, 1, 0x0C, 0x0010, 0x0F, "1000000000001cfdbcfd86f766fd0f0000" },
    { 68, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000000000000000000000000000000000000000000000e98c449b4090449b" },
    { 69, 1, 0x0C, 0x0000, 0x0F, "0000000000001cfdbcfd86f766fd0f0000" },
    { 72, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000000000000000000000000000000000000000000000a9cb449b00cf449b" },
    { 73, 1, 0x0C, 0x0000, 0x0D, "0000000000001cfdbcfd86f766fd0d0000" },
    { 74, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000000000000000000000000000000000000000000000eaea449b7eee449b" },
    { 75, 1, 0x0C, 0x0000, 0x08, "0000000000001cfdbcfd86f766fd080000" },
    { 76, 1, 0x20, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000000000000000000000000000000000000000000000ee67459b276b459b" },
    { 77, 1, 0x0C, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000" },
};
static const char usb_edges[] = "0401080209bfd80";
/* END CAPTURE FIXTURES */

static unsigned hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return (unsigned)(c - '0');
    }
    CHECK(c >= 'a' && c <= 'f');
    return (unsigned)(c - 'a' + 10);
}

static size_t unhex(const char *hex, uint8_t *data, size_t capacity)
{
    size_t i, size = strlen(hex) / 2;
    CHECK(strlen(hex) % 2 == 0 && size <= capacity);
    for (i = 0; i < size; ++i) {
        data[i] = (uint8_t)((hex_digit(hex[2 * i]) << 4) | hex_digit(hex[2 * i + 1]));
    }
    return size;
}

static int same_sample(const SDL_XInputPaddleSample *a, const SDL_XInputPaddleSample *b)
{
    /* Struct padding is not a semantic field. */
    return a->button_word == b->button_word && a->raw_mask == b->raw_mask &&
           a->physical_mask == b->physical_mask && a->profile == b->profile &&
           a->profile_kind == b->profile_kind &&
           a->digital_comparison == b->digital_comparison && a->delivery == b->delivery;
}

static void actual_cases(void)
{
    size_t i;
    CHECK(COUNT(actual_fixtures) == 14);
    for (i = 0; i < COUNT(actual_fixtures); ++i) {
        const ActualFixture *f = &actual_fixtures[i];
        SDL_XInputPaddleTransport transport = f->bluetooth ? BT : GIP;
        SDL_XInputPaddleState state;
        SDL_XInputPaddleResult r;
        uint8_t data[17], original[17];
        capture_line = f->line;
        CHECK(unhex(f->hex, data, sizeof(data)) == sizeof(data));
        memcpy(original, data, sizeof(data));
        SDL_XInputPaddleReset(&state, transport, 1);
        r = SDL_XInputPaddleDecode(&state, transport, 1, f->bluetooth ? 0 : 0x0C, data, sizeof(data));
        CHECK(r.flags == (uint32_t)(RAW | READY | (f->bluetooth ? 0 : SPLIT)));
        CHECK(r.sample.raw_mask == f->mask && r.sample.physical_mask == f->mask);
        CHECK(r.sample.button_word == f->word && r.sample.profile == f->profile);
        CHECK(r.sample.profile_kind == SDL_XINPUT_PADDLE_PROFILE_SLOT);
        CHECK(r.sample.digital_comparison == SDL_XINPUT_PADDLE_COMPARE_UNKNOWN);
        CHECK(!state.have_normal && state.have_paddles);
        CHECK(state.have_separate == !f->bluetooth);
        CHECK(same_sample(&state.paddles, &r.sample));
        CHECK(memcmp(original, data, sizeof(data)) == 0);
    }
    capture_line = 0;
}

static void replay(const CaptureRun *runs, size_t run_count, const char *edges,
                   SDL_XInputPaddleTransport transport, unsigned expected_frames,
                   unsigned expected_paddles, unsigned expected_normals)
{
    SDL_XInputPaddleState state;
    size_t i, edge = 0;
    unsigned frames = 0, paddles = 0, normals = 0;
    int previous = -1;
    SDL_XInputPaddleReset(&state, transport, 100);
    for (i = 0; i < run_count; ++i) {
        const CaptureRun *run = &runs[i];
        uint8_t bytes[47], original[47];
        size_t size = unhex(run->hex, bytes, sizeof(bytes));
        unsigned repeat;
        memcpy(original, bytes, size);
        for (repeat = 0; repeat < run->repeat; ++repeat) {
            SDL_XInputPaddleSample held = state.paddles;
            int had_split = state.have_separate;
            SDL_XInputPaddleResult r;
            capture_line = run->line + repeat;
            r = SDL_XInputPaddleDecode(&state, transport, 100, run->report_id, bytes, size);
            ++frames;
            CHECK(r.sample.button_word == run->word);
            CHECK(r.sample.raw_mask == run->mask && r.sample.physical_mask == run->mask);
            CHECK(r.sample.profile == 0 && r.sample.profile_kind == SDL_XINPUT_PADDLE_PROFILE_SLOT);
            CHECK(memcmp(original, bytes, size) == 0);
            if (run->report_id == 0x20) {
                ++normals;
                CHECK(r.flags == (uint32_t)(NORMAL | RAW | (had_split ? 0 : READY)));
                CHECK(r.sample.delivery == SDL_XINPUT_PADDLE_DELIVERY_IN_BAND);
                CHECK(state.have_normal && memcmp(state.normal, original, 14) == 0);
                if (had_split) {
                    CHECK(same_sample(&state.paddles, &held));
                } else {
                    CHECK(!state.have_separate);
                }
            } else {
                ++paddles;
                CHECK(r.flags == (uint32_t)(RAW | READY | (transport == BT ? 0 : SPLIT)));
                CHECK(state.have_separate == (transport == GIP));
                CHECK(r.sample.delivery == (transport == BT ? SDL_XINPUT_PADDLE_DELIVERY_GATT : SDL_XINPUT_PADDLE_DELIVERY_SEPARATE));
                CHECK(r.sample.digital_comparison == (transport == BT ? SDL_XINPUT_PADDLE_COMPARE_UNKNOWN : SDL_XINPUT_PADDLE_COMPARE_EQUAL));
                CHECK(same_sample(&state.paddles, &r.sample));
                if (previous != run->mask) {
                    CHECK(edge < strlen(edges) && hex_digit(edges[edge]) == run->mask);
                    previous = run->mask;
                    ++edge;
                }
            }
        }
    }
    CHECK(frames == expected_frames && paddles == expected_paddles && normals == expected_normals);
    CHECK(edge == strlen(edges) && state.paddles.physical_mask == 0);
    capture_line = 0;
    printf("Replay: %u frames, %u paddle reports, %u normal reports, %u mask states.\n",
           frames, paddles, normals, (unsigned)edge);
}

typedef struct SyntheticLayout
{
    SDL_XInputPaddleTransport transport;
    uint32_t report;
    size_t size;
    size_t paddle;
    size_t profile;
} SyntheticLayout;

/* All 34/47 cases are synthetic. These offsets are source-backed, with no
 * claim that the captured firmware emitted either layout.
 */
static const SyntheticLayout layouts[] = {
    { BT, 0, 17, 14, 15 },
    { GIP, 0x0C, 17, 14, 15 },
    { GIP, 0x20, 29, 28, 28 },
    { GIP, 0x20, 34, 14, 15 },
    { GIP, 0x20, 46, 18, 19 },
    { GIP, 0x20, 47, 14, 20 }
};

static void masks_and_profiles(void)
{
    /* Physical outcomes from SDL's Series 1 P1/P2/P3/P4 = 02/08/01/04.
     * The oracle is a complete table, separate from the decoder's shifts.
     */
    static const uint8_t series1[16] = { 0, 4, 1, 5, 8, 12, 9, 13, 2, 6, 3, 7, 10, 14, 11, 15 };
    size_t l;
    unsigned raw, profile;
    for (l = 0; l < COUNT(layouts); ++l) {
        const SyntheticLayout *f = &layouts[l];
        SDL_XInputPaddleState state;
        SDL_XInputPaddleReset(&state, f->transport, 7);
        for (profile = 0; profile <= 255; ++profile) {
            for (raw = 0; raw < 16; ++raw) {
                uint8_t packet[47], original[47];
                SDL_XInputPaddleResult r;
                uint8_t expected = f->size == 29 ? series1[raw] : (uint8_t)raw;
                memset(packet, 0xA5, sizeof(packet));
                packet[0] = 0xF0; /* All ordinary face buttons remain held. */
                packet[1] = 0xFF;
                packet[f->profile] = (uint8_t)profile;
                packet[f->paddle] = (uint8_t)((f->size == 29 ? profile & 0xF0 : 0xF0) | raw);
                memcpy(original, packet, sizeof(packet));
                r = SDL_XInputPaddleDecode(&state, f->transport, 7, f->report, packet, f->size);
                CHECK(r.flags == (uint32_t)(RAW | READY | (f->report == 0x20 ? NORMAL : f->report == 0x0C ? SPLIT : 0)));
                CHECK(r.sample.raw_mask == raw && r.sample.physical_mask == expected);
                CHECK(r.sample.button_word == 0xFFF0);
                CHECK(r.sample.profile == (f->size == 29 ? (profile & 0x10) >> 4 : profile));
                CHECK(r.sample.profile_kind == (f->size == 29 ? SDL_XINPUT_PADDLE_PROFILE_MODE : SDL_XINPUT_PADDLE_PROFILE_SLOT));
                CHECK(memcmp(packet, original, sizeof(packet)) == 0);
                CHECK(same_sample(&state.paddles, &r.sample));
                CHECK(state.have_separate == (f->report == 0x0C));
                if (f->report == 0x20) {
                    CHECK(state.have_normal && memcmp(state.normal, original, 14) == 0);
                    CHECK(state.paddles.delivery == SDL_XINPUT_PADDLE_DELIVERY_IN_BAND);
                }
            }
        }
    }
}

static void comparisons_and_opaque_byte(void)
{
    SDL_XInputPaddleState state;
    SDL_XInputPaddleResult r;
    uint8_t normal[46] = { 0 }, split[17] = { 0 };
    unsigned value, bit;
    SDL_XInputPaddleReset(&state, GIP, 42);
    normal[0] = split[0] = 0x10;
    normal[1] = split[1] = 0x81;
    split[14] = 5;
    split[15] = 3;
    r = SDL_XInputPaddleDecode(&state, GIP, 42, 0x0C, split, sizeof(split));
    CHECK(r.sample.digital_comparison == SDL_XINPUT_PADDLE_COMPARE_UNKNOWN);
    r = SDL_XInputPaddleDecode(&state, GIP, 42, 0x20, normal, sizeof(normal));
    CHECK(r.flags == (NORMAL | RAW) && state.paddles.physical_mask == 5);
    /* Every analog byte differs while the digital word stays equal. */
    memset(split + 2, 0xFF, 12);
    r = SDL_XInputPaddleDecode(&state, GIP, 42, 0x0C, split, sizeof(split));
    CHECK(r.sample.digital_comparison == SDL_XINPUT_PADDLE_COMPARE_EQUAL);
    CHECK(r.sample.physical_mask == 5);
    for (bit = 0; bit < 16; ++bit) {
        split[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        r = SDL_XInputPaddleDecode(&state, GIP, 42, 0x0C, split, sizeof(split));
        CHECK(r.sample.digital_comparison == SDL_XINPUT_PADDLE_COMPARE_DIFFERENT);
        CHECK(r.sample.physical_mask == 5 && r.sample.profile == 3);
        CHECK(memcmp(state.normal, normal, 14) == 0);
        split[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
    for (value = 0; value < 2; ++value) {
        SDL_XInputPaddleTransport transport = value ? GIP : BT;
        SDL_XInputPaddleSample first;
        unsigned opaque;
        SDL_XInputPaddleReset(&state, transport, 42);
        split[16] = 0;
        first = SDL_XInputPaddleDecode(&state, transport, 42, value ? 0x0C : 0, split, sizeof(split)).sample;
        for (opaque = 0; opaque <= 255; ++opaque) {
            split[16] = (uint8_t)opaque;
            r = SDL_XInputPaddleDecode(&state, transport, 42, value ? 0x0C : 0, split, sizeof(split));
            CHECK(same_sample(&first, &r.sample));
            CHECK(same_sample(&first, &state.paddles));
        }
    }
    /* Series 1 has its own in-packet raw digital word. Compare only two bytes. */
    SDL_XInputPaddleReset(&state, GIP, 42);
    normal[14] = normal[0];
    normal[15] = normal[1];
    normal[28] = 0x1F;
    r = SDL_XInputPaddleDecode(&state, GIP, 42, 0x20, normal, 29);
    CHECK(r.sample.digital_comparison == SDL_XINPUT_PADDLE_COMPARE_EQUAL);
    normal[15] ^= 0x80;
    r = SDL_XInputPaddleDecode(&state, GIP, 42, 0x20, normal, 29);
    CHECK(r.sample.digital_comparison == SDL_XINPUT_PADDLE_COMPARE_DIFFERENT);
    CHECK(r.sample.raw_mask == 15 && r.sample.physical_mask == 15);
}

static void retention_reset_generation(void)
{
    SDL_XInputPaddleState state, other, before;
    SDL_XInputPaddleSample held;
    SDL_XInputPaddleResult r;
    uint8_t packet[47] = { 0 }, split[17] = { 0 };
    size_t i;
    SDL_XInputPaddleReset(&state, GIP, UINT64_MAX);
    packet[18] = 5;
    packet[19] = 2;
    r = SDL_XInputPaddleDecode(&state, GIP, UINT64_MAX, 0x20, packet, 46);
    CHECK(r.flags == (NORMAL | RAW | READY));
    CHECK(state.paddles.physical_mask == 5 && !state.have_separate);
    CHECK(state.paddles.delivery == SDL_XINPUT_PADDLE_DELIVERY_IN_BAND);
    split[14] = 9;
    split[15] = 3;
    r = SDL_XInputPaddleDecode(&state, GIP, UINT64_MAX, 0x0C, split, 17);
    CHECK(r.flags == (RAW | READY | SPLIT) && state.have_separate);
    held = state.paddles;
    /* Every ordinary layout carries a conflicting release after split receipt. */
    for (i = 2; i < COUNT(layouts); ++i) {
        memset(packet, 0, sizeof(packet));
        packet[0] = 0xF0;
        packet[1] = (uint8_t)i;
        r = SDL_XInputPaddleDecode(&state, GIP, UINT64_MAX, 0x20, packet, layouts[i].size);
        CHECK(r.flags == (NORMAL | RAW));
        CHECK(r.sample.raw_mask == 0 && r.sample.physical_mask == 0);
        CHECK(same_sample(&state.paddles, &held));
        CHECK(memcmp(state.normal, packet, 14) == 0);
    }
    r = SDL_XInputPaddleDecode(&state, GIP, UINT64_MAX, 0x20, packet, 14);
    CHECK(r.flags == NORMAL && same_sample(&state.paddles, &held));
    CHECK(memcmp(state.normal, packet, 14) == 0);
    /* A second controller's normal stream and reset cannot alter the first. */
    SDL_XInputPaddleReset(&other, GIP, 2);
    r = SDL_XInputPaddleDecode(&other, GIP, 2, 0x20, packet, 46);
    CHECK(r.flags == (NORMAL | RAW | READY) && !other.have_separate);
    CHECK(same_sample(&state.paddles, &held));
    SDL_XInputPaddleReset(&state, GIP, 0);
    CHECK(!state.have_normal && !state.have_paddles && !state.have_separate);
    CHECK(state.paddles.delivery == SDL_XINPUT_PADDLE_DELIVERY_NONE);
    CHECK(state.paddles.physical_mask == 0 && state.paddles.raw_mask == 0);
    CHECK(state.paddles.profile_kind == SDL_XINPUT_PADDLE_PROFILE_NONE);
    for (i = 0; i < sizeof(state.normal); ++i) {
        CHECK(state.normal[i] == 0);
    }
    memcpy(&before, &state, sizeof(state));
    r = SDL_XInputPaddleDecode(&state, GIP, UINT64_MAX, 0x0C, split, 17);
    CHECK(r.flags == 0 && memcmp(&state, &before, sizeof(state)) == 0);
    packet[18] = 2;
    r = SDL_XInputPaddleDecode(&state, GIP, 0, 0x20, packet, 46);
    CHECK(r.flags == (NORMAL | RAW | READY) && state.paddles.physical_mask == 2);
    CHECK(!state.have_separate);
    SDL_XInputPaddleReset(&state, BT, 3);
    r = SDL_XInputPaddleDecode(&state, GIP, 0, 0x0C, split, 17);
    CHECK(r.flags == 0 && !state.have_paddles);
    r = SDL_XInputPaddleDecode(&state, BT, 3, 0, split, 17);
    CHECK(r.flags == (RAW | READY) && state.paddles.physical_mask == 9);
    /* Input storage is disposable, and real release is accepted afterward. */
    memset(split, 0, sizeof(split));
    CHECK(state.paddles.physical_mask == 9);
    r = SDL_XInputPaddleDecode(&state, BT, 3, 0, split, 17);
    CHECK(r.flags == (RAW | READY) && state.paddles.physical_mask == 0);
    SDL_XInputPaddleReset(NULL, BT, 0);
}

static void malformed(void)
{
    SDL_XInputPaddleState state, before;
    SDL_XInputPaddleResult r;
    unsigned transport, id;
    size_t size;
    uint8_t good[17] = { 0 };
    good[14] = 15;
    for (transport = 0; transport <= 3; ++transport) {
        SDL_XInputPaddleTransport tag = (SDL_XInputPaddleTransport)transport;
        SDL_XInputPaddleReset(&state, tag, 5);
        r = SDL_XInputPaddleDecode(&state, tag, 5, transport == 1 ? 0 : 0x0C, good, 17);
        CHECK((r.flags != 0) == (transport == 1 || transport == 2));
        for (id = 0; id <= 255; ++id) {
            for (size = 0; size <= 64; ++size) {
                int accepted = (transport == 1 && id == 0 && size == 17) ||
                               (transport == 2 && ((id == 0x0C && size == 17) ||
                                (id == 0x20 && (size == 14 || size == 29 || size == 34 || size == 46 || size == 47))));
                uint8_t *data = (uint8_t *)malloc(size ? size : 1);
                CHECK(data != NULL);
                memset(data, 0xFF, size ? size : 1);
                memcpy(&before, &state, sizeof(state));
                r = SDL_XInputPaddleDecode(&state, tag, 5, id, data, size);
                CHECK((r.flags != 0) == accepted);
                if (!accepted) {
                    CHECK(memcmp(&state, &before, sizeof(state)) == 0);
                }
                free(data);
            }
        }
    }
    SDL_XInputPaddleReset(&state, GIP, 5);
    (void)SDL_XInputPaddleDecode(&state, GIP, 5, 0x0C, good, sizeof(good));
    memcpy(&before, &state, sizeof(state));
    r = SDL_XInputPaddleDecode(&state, GIP, 5, 0x20, good, SIZE_MAX);
    CHECK(r.flags == 0 && memcmp(&state, &before, sizeof(state)) == 0);
    r = SDL_XInputPaddleDecode(&state, GIP, 5, UINT32_MAX, good, 17);
    CHECK(r.flags == 0 && memcmp(&state, &before, sizeof(state)) == 0);
    r = SDL_XInputPaddleDecode(&state, GIP, 5, 0x0C, NULL, 17);
    CHECK(r.flags == 0 && memcmp(&state, &before, sizeof(state)) == 0);
    r = SDL_XInputPaddleDecode(NULL, GIP, 5, 0x0C, good, 17);
    CHECK(r.flags == 0);
    r = SDL_XInputPaddleDecode(&state, BT, 5, 0, good, 17);
    CHECK(r.flags == 0 && memcmp(&state, &before, sizeof(state)) == 0);
    r = SDL_XInputPaddleDecode(&state, GIP, 6, 0x0C, good, 17);
    CHECK(r.flags == 0 && memcmp(&state, &before, sizeof(state)) == 0);
}

int main(void)
{
    test_name = "actual cases";
    actual_cases();
    test_name = "Bluetooth chronological replay";
    replay(bt_replay, COUNT(bt_replay), bt_edges, BT, 555, 555, 0);
    test_name = "USB chronological replay";
    replay(usb_replay, COUNT(usb_replay), usb_edges, GIP, 33, 16, 17);
    test_name = "all masks and profiles";
    masks_and_profiles();
    test_name = "comparison metadata and opaque byte";
    comparisons_and_opaque_byte();
    test_name = "retention, reset, generation and ownership";
    retention_reset_generation();
    test_name = "malformed frames";
    malformed();
    printf("PASS: 7 test groups, %u checks.\n", checks);
    return 0;
}
