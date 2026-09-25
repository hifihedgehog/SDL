/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_kettler_proto.c, the
   Kettler ergometers of hifihedgehog/SDL#33 Part 5. The vectors are
   constructed from KettlerBLE, kettlerUSB2BLE and the protocol notes. Test
   numbers follow the ticket. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_kettler_proto.h"

static SDL_KettlerState *State(Harness *h)
{
    return (SDL_KettlerState *)h->state;
}

static const uint8_t example[36] = {
    0x31, 0x30, 0x31, 0x09, 0x30, 0x34, 0x37, 0x09, 0x30, 0x37, 0x34, 0x09, 0x30, 0x30, 0x32, 0x09,
    0x30, 0x32, 0x35, 0x09, 0x30, 0x33, 0x31, 0x32, 0x09, 0x30, 0x31, 0x3A, 0x31, 0x32, 0x09, 0x30,
    0x32, 0x35, 0x0D, 0x0A
};
static const char rest_line[] = "000\t000\t000\t000\t000\t0000\t00:00\t000\r\n";
static const char cadence_line[] = "101\t090\t074\t002\t025\t0312\t01:12\t025\r\n";
static const char seven_fields[] = "101\t047\t074\t002\t025\t0312\t01:12\r\n";
static const char letters[] = "101\t0A7\t074\t002\t025\t0312\t01:12\t025\r\n";
static uint8_t long_line[201];

static void Text(Harness *h, const char *text)
{
    H_FeedText(h, text);
}

static void BringUp(Harness *h)
{
    H_Advance(h, h->now + 1050);
    Text(h, rest_line);
}

static Harness *PresentErgometer(void)
{
    Harness *h = H_Create(&SDL_SerialKettlerModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialKettlerModule);
    static const char *const commands[8] = { "VE\r\n", "ID\r\n", "VE\r\n", "KI\r\n", "CA\r\n", "RS\r\n", "CM\r\n", "SP1\r\n" };
    uint64_t t;
    int i;

    /* 1 */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x56\x45\x0D\x0A", 4, 0));
    for (i = 1; i < 8; ++i) {
        H_Advance(h, (uint64_t)i * 150 - 1);
        CHECK(H_NextCall(h) == NULL);
        H_Advance(h, (uint64_t)i * 150);
        CHECK(H_IsWriteText(H_NextCall(h), commands[i], (uint64_t)i * 150));
    }
    CHECK(H_IsWrite(&h->calls[h->ncalls - 1], (const uint8_t *)"\x53\x50\x31\x0D\x0A", 5, 1050));
    H_Advance(h, 3049);
    CHECK(H_NextCall(h) == NULL);
    for (t = 3050; t <= 11050; t += 2000) {
        H_Advance(h, t);
        CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x53\x54\x0D\x0A", 4, t));
        /* An answer to each poll keeps it alive */
        H_FeedAt(h, t + 20, example, sizeof(example));
    }
    CHECK(h->presence[0] == 1);
    H_Destroy(h);
}

static void TestLines(void)
{
    Harness *h = H_Create(&SDL_SerialKettlerModule);
    int fields[8];

    /* 2 */
    H_Start(h);
    H_Advance(h, 3050);
    CHECK(h->npresence == 0);
    H_FeedAt(h, 3070, example, sizeof(example));
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Kettler Ergometer") == 0);
    CHECK(h->identity[0].naxes == 5 && h->identity[0].nbuttons == 0 && h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN);
    CHECK(H_Axis(h, 0, 0) == 47 && H_Axis(h, 0, 1) == 25 && H_Axis(h, 0, 2) == 74 && H_Axis(h, 0, 3) == 101 && H_Axis(h, 0, 4) == 25);
    CHECK(SDL_Kettler_ParseStatus("101\t047\t074\t002\t025\t0312\t01:12\t025", fields));
    CHECK(fields[0] == 101 && fields[1] == 47 && fields[2] == 74 && fields[3] == 2 && fields[4] == 25 && fields[5] == 312 && fields[6] == 0 && fields[7] == 25);

    /* 3 */
    Text(h, cadence_line);
    CHECK(H_Axis(h, 0, 0) == 90 && H_Axis(h, 0, 1) == 25 && H_Axis(h, 0, 3) == 101);

    /* 4 */
    {
        const int mark = h->npublished;
        Text(h, seven_fields);
        Text(h, letters);
        H_Feed(h, long_line, sizeof(long_line));
        H_Feed(h, (const uint8_t *)"\n", 1);
        CHECK(h->npublished == mark && H_Axis(h, 0, 0) == 90);
    }
    CHECK(!SDL_Kettler_ParseStatus("101\t047\t074\t002\t025\t0312\t01:12\t025\t9", fields));
    CHECK(!SDL_Kettler_ParseStatus("101\t\t074\t002\t025\t0312\t01:12\t025", fields));
    CHECK(!SDL_Kettler_ParseStatus("101\t047\t074\t002\t025\t0312\t01:12\t", fields));
    CHECK(!SDL_Kettler_ParseStatus("101\t047\t074\t002\t025\t03:12\t01:12\t025", fields));
    CHECK(!SDL_Kettler_ParseStatus("", fields));
    /* A key line of 4 fields is not a status */
    CHECK(!SDL_Kettler_ParseStatus("000\t000\t000\t003", fields));
    /* Clamped to 32767 */
    CHECK(SDL_Kettler_ParseStatus("99999\t047\t074\t002\t025\t0312\t01:12\t025", fields) && fields[0] == 32767);
    /* 0D anywhere is skipped, 0A alone ends a line, empty lines are nothing */
    {
        const int mark = h->npublished;
        Text(h, "\r\n\n");
        Text(h, "101\t047\t074\t002\t025\t0312\t01:12\t025\n");
        CHECK(h->npublished == mark + 1 && H_Axis(h, 0, 0) == 47);
    }
    H_Destroy(h);
}

static void TestSilence(void)
{
    Harness *h = PresentErgometer();

    /* 5: the last line at 1050 */
    H_Advance(h, 3050);
    CHECK(H_IsWriteText(H_NextCall(h), "ST\r\n", 3050));
    H_Advance(h, 5050);
    CHECK(H_IsWriteText(H_NextCall(h), "ST\r\n", 5050));
    H_Advance(h, 6049);
    CHECK(h->presence[0] == 1);
    H_Advance(h, 6050);
    CHECK(h->presence[0] == 0);
    H_Advance(h, 9049);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 9050);
    CHECK(H_IsWriteText(H_NextCall(h), "VE\r\n", 9050));
    /* A line that is no status also counts as a line */
    H_Advance(h, 9050 + 1050);
    Text(h, "OK\r\n");
    H_Advance(h, 9050 + 1050 + 4999);
    CHECK(State(h)->step == SDL_KETTLER_STEP_RUNNING);
    Text(h, rest_line);
    CHECK(h->presence[0] == 2);
    H_Destroy(h);
}

static void TestOutput(void)
{
    Harness *h = PresentErgometer();
    SDL_SerialOutput request;

    /* 6 */
    memset(&request, 0, sizeof(request));
    request.kind = SDL_SERIAL_OUTPUT_EFFECT;
    request.length = 2;
    request.data[0] = 0x96;
    request.data[1] = 0x00;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 3050);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x50\x57\x31\x35\x30\x0D\x0A", 7, 3050));
    Text(h, rest_line);
    H_Advance(h, 5050);
    CHECK(H_IsWriteText(H_NextCall(h), "ST\r\n", 5050));
    /* The latest request wins, and only 2 bytes are accepted */
    request.data[0] = 0x2C;
    request.data[1] = 0x01;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    request.data[0] = 0x19;
    request.data[1] = 0x00;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    request.length = 3;
    request.data[0] = 0xFF;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    Text(h, rest_line);
    H_Advance(h, 7050);
    CHECK(H_IsWriteText(H_NextCall(h), "PW25\r\n", 7050));
    H_Destroy(h);
}

static void TestBatteryB(void)
{
    Harness *h = PresentErgometer();

    H_Advance(h, 2000);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 2000));
    H_Advance(h, 3000);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 3000));
    CHECK(H_IsWriteText(H_NextCall(h), "VE\r\n", 3000));
    H_Advance(h, 4050);
    CHECK(h->presence[0] == 0);
    H_Feed(h, example, sizeof(example));
    CHECK(h->presence[0] == 2);
    H_Destroy(h);
}

static void TestBatteryA(void)
{
    Harness *h = PresentErgometer();
    H_Vector vectors[4];
    H_Vector rest;

    H_SetVector(&vectors[0], "2 example", example, sizeof(example), 1, 0);
    H_SetVector(&vectors[1], "3 cadence 090", (const uint8_t *)cadence_line, strlen(cadence_line), 1, 0);
    H_BatteryA(h, &vectors[0], BringUp);
    H_BatteryA(h, &vectors[1], BringUp);
    H_SetVector(&rest, "rest", (const uint8_t *)cadence_line, strlen(cadence_line), 1, 0);
    H_BatteryA6(h, "4 seven fields", (const uint8_t *)seven_fields, strlen(seven_fields), &rest);
    H_BatteryA6(h, "4 letters", (const uint8_t *)letters, strlen(letters), &rest);
    {
        uint8_t long_then_end[202];
        memcpy(long_then_end, long_line, sizeof(long_line));
        long_then_end[201] = 0x0A;
        H_BatteryA6(h, "4 200 bytes", long_then_end, sizeof(long_then_end), &rest);
    }
    H_Destroy(h);
}

int main(void)
{
    memset(long_line, '1', sizeof(long_line) - 1);
    long_line[200] = '1';
    TestStartup();
    TestLines();
    TestSilence();
    TestOutput();
    TestBatteryB();
    TestBatteryA();
    return H_Finish();
}
