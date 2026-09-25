/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_mastercontroller_proto.c,
   Pony Canyon's Master Controllers of hifihedgehog/SDL#33 Part 10. Test
   numbers follow the part's Master Controller section. The events are
   constructed from the Train Controller Database and Mackoy's decoder. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_mastercontroller_proto.h"

/* Open, a 19200 8N1 line with no flow control and DTR and RTS off, the
   timeouts and the purge */
static bool ExpectOpened(Harness *h, uint64_t time)
{
    const H_Call *open = H_NextCall(h);
    const H_Call *line = H_NextCall(h);
    const H_Call *timeouts = H_NextCall(h);
    const H_Call *purge = H_NextCall(h);

    return H_IsCall(open, 'O', time) &&
           H_IsLine(line, 19200, 8, SDL_SERIAL_NOPARITY, 1, SDL_SERIAL_RTS_CONTROL_DISABLE, false, SDL_SERIAL_DTR_CONTROL_DISABLE, time) &&
           H_IsCall(timeouts, 'T', time) && H_IsCall(purge, 'P', time) && purge->value == 0x0F;
}

static void FeedText(Harness *h, const char *text)
{
    H_Feed(h, (const uint8_t *)text, strlen(text));
}

static int16_t Lever(Harness *h)
{
    return H_Axis(h, 0, 1);
}

static int16_t Reverser(Harness *h)
{
    return H_Axis(h, 0, 3);
}

/* 1 */
static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialMasterControllerModule);

    CHECK(SDL_SerialMasterControllerModule.effect_max == 0 && !SDL_SerialMasterControllerModule.rumble);
    H_Start(h);
    CHECK(ExpectOpened(h, 0));
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x00", 1, 0));
    CHECK(H_NextCall(h) == NULL && h->presence[0] == 0);
    /* Nothing more in the next 60 s, and no joystick without an event */
    H_Advance(h, 60000);
    CHECK(H_NextCall(h) == NULL && h->presence[0] == 0);

    /* A port error, a reopen, and one more 00 */
    FeedText(h, "TSA55\r");
    CHECK(h->presence[0] == 1);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', h->now));
    H_Advance(h, h->now + SDL_SERIAL_RETRY_MS);
    CHECK(ExpectOpened(h, h->now));
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x00", 1, h->now));
    CHECK(H_NextCall(h) == NULL);
    H_Destroy(h);
}

/* 8, and the identity */
static void TestPresence(void)
{
    Harness *h = H_Create(&SDL_SerialMasterControllerModule);

    H_Start(h);
    H_SkipCalls(h);
    /* A button first: the lever reads 0 and the reverser Neutral */
    FeedText(h, "TSK99\r");
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Pony Canyon Master Controller") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_GAMEPAD && h->identity[0].naxes == 4 && h->identity[0].nbuttons == 4);
    CHECK(h->identity[0].nhats == 0 && h->identity[0].has_mapping);
    CHECK(h->identity[0].mapping.a.kind == SDL_SERIAL_MAP_BUTTON && h->identity[0].mapping.a.target == 0);
    CHECK(h->identity[0].mapping.b.kind == SDL_SERIAL_MAP_BUTTON && h->identity[0].mapping.b.target == 1);
    CHECK(h->identity[0].mapping.x.kind == SDL_SERIAL_MAP_BUTTON && h->identity[0].mapping.x.target == 2);
    CHECK(h->identity[0].mapping.y.kind == SDL_SERIAL_MAP_BUTTON && h->identity[0].mapping.y.target == 3);
    CHECK(h->identity[0].mapping.lefty.kind == SDL_SERIAL_MAP_AXIS && h->identity[0].mapping.lefty.target == 1);
    CHECK(h->identity[0].mapping.righty.kind == SDL_SERIAL_MAP_AXIS && h->identity[0].mapping.righty.target == 3);
    CHECK(h->identity[0].mapping.leftx.kind == SDL_SERIAL_MAP_NONE && h->identity[0].mapping.start.kind == SDL_SERIAL_MAP_NONE);
    CHECK(Lever(h) == 0 && Reverser(h) == -32768 && H_OnlyButton(h, 0, 3));
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 2) == 0);

    /* A port error, then the state starts over */
    FeedText(h, "TSB20\rTSG99\r");
    CHECK(Lever(h) == -32768 && Reverser(h) == 128);
    H_LosePort(h);
    H_Advance(h, h->now + SDL_SERIAL_RETRY_MS);
    FeedText(h, "TSY99\r");
    CHECK(h->presence[0] == 2 && Lever(h) == 0 && Reverser(h) == -32768 && H_OnlyButton(h, 0, 0));
    H_Destroy(h);
}

/* 2 and 3 */
static void TestEvents(void)
{
    static const struct
    {
        const char *line;
        int16_t lever;
    } handles[] = {
        { "TSB20\r", -32768 }, { "TSB30\r", -29127 }, { "TSB40\r", -25486 }, { "TSE99\r", -21845 },
        { "TSA05\r", -18204 }, { "TSA15\r", -14564 }, { "TSA25\r", -10923 }, { "TSA35\r", -7282 },
        { "TSA45\r", -3641 }, { "TSA50\r", 0 }, { "TSA55\r", 4096 }, { "TSA65\r", 8192 },
        { "TSA75\r", 12288 }, { "TSA85\r", 16384 }, { "TSA95\r", 20479 }, { "TSB60\r", 24575 },
        { "TSB70\r", 28671 }, { "TSB80\r", 32767 },
    };
    static const struct
    {
        const char *press;
        const char *release;
        int button;
    } buttons[] = {
        { "TSY99\r", "TSY00\r", 0 }, { "TSZ99\r", "TSZ00\r", 1 }, { "TSX99\r", "TSX00\r", 2 }, { "TSK99\r", "TSK00\r", 3 },
    };
    Harness *h = H_Create(&SDL_SerialMasterControllerModule);
    size_t i;
    int mark;

    H_Start(h);
    for (i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
        FeedText(h, handles[i].line);
        CHECK(Lever(h) == handles[i].lever && Reverser(h) == -32768 && H_NoButtons(h, 0));
    }
    FeedText(h, "TSG99\r");
    CHECK(Reverser(h) == 128);
    FeedText(h, "TSG00\r");
    CHECK(Reverser(h) == 32767);
    FeedText(h, "TSG50\r");
    CHECK(Reverser(h) == -32768 && Lever(h) == 32767);

    for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        mark = h->npublished;
        FeedText(h, buttons[i].release);
        CHECK(h->npublished == mark && H_NoButtons(h, 0));
        FeedText(h, buttons[i].press);
        CHECK(H_OnlyButton(h, 0, buttons[i].button));
        FeedText(h, buttons[i].release);
        CHECK(H_NoButtons(h, 0));
    }
    H_Destroy(h);
}

/* 4 to 7 */
static void TestFraming(void)
{
    static const char *ignored[] = { "TSB99\r", "TSA5\r", "TSA500\r", "tsa50\r", "TSQ12\r", "\nTSA50\r", "TS\xC1" "50\r" };
    Harness *h = H_Create(&SDL_SerialMasterControllerModule);
    uint8_t *run;
    size_t i;
    int mark;

    H_Start(h);
    FeedText(h, "TSA55\r");
    /* 4 */
    H_Feed(h, (const uint8_t *)"\x54\x53\x41", 3);
    CHECK(Lever(h) == 4096);
    H_Feed(h, (const uint8_t *)"\x35\x30\x0D", 3);
    CHECK(Lever(h) == 0);
    FeedText(h, "TSA55\rTSA65\r");
    CHECK(Lever(h) == 8192);
    FeedText(h, "TSA55");
    CHECK(Lever(h) == 8192);
    FeedText(h, "\r");
    CHECK(Lever(h) == 4096);

    /* 5 */
    for (i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
        mark = h->npublished;
        FeedText(h, ignored[i]);
        CHECK(h->npublished == mark && Lever(h) == 4096);
    }

    /* 6 */
    run = (uint8_t *)malloc(10000);
    memset(run, 0x41, 10000);
    mark = h->npublished;
    H_Feed(h, run, 10000);
    CHECK(((SDL_MasterControllerState *)h->state)->parser.count <= 5);
    FeedText(h, "\r");
    CHECK(h->npublished == mark && Lever(h) == 4096);
    FeedText(h, "TSA65\rTSA55\r");
    CHECK(Lever(h) == 4096 && H_NoButtons(h, 0));
    free(run);

    /* 7: bytes past the count a read returns */
    H_FeedPadded(h, (const uint8_t *)"TSA75", 5);
    CHECK(Lever(h) == 4096);
    H_FeedPadded(h, (const uint8_t *)"\r", 1);
    CHECK(Lever(h) == 12288);
    H_Destroy(h);
}

static void BringUp(Harness *h)
{
    FeedText(h, "TSA50\r");
}

/* Batteries A and B on a lever event and a button event */
static void TestBatteries(void)
{
    Harness *h = H_Create(&SDL_SerialMasterControllerModule);
    H_Vector lever, button, valid;

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1);
    H_SetVector(&lever, "lever P6", (const uint8_t *)"TSB60\r", 6, 1, 0);
    H_SetVector(&button, "S", (const uint8_t *)"TSK99\r", 6, 1, 0);
    H_BatteryA(h, &lever, BringUp);
    H_BatteryA(h, &button, BringUp);
    H_SetVector(&valid, "valid", (const uint8_t *)"TSB60\r", 6, 1, 0);
    H_BatteryA6(h, "brake 6 as p4ken lists it", (const uint8_t *)"TSB99\r", 6, &valid);
    H_BatteryA6(h, "leading LF", (const uint8_t *)"\nTSA55\r", 7, &valid);
    H_BatteryA6(h, "six characters", (const uint8_t *)"TSA555\r", 7, &valid);
    H_Destroy(h);
}

int main(void)
{
    TestStartup();
    TestPresence();
    TestEvents();
    TestFraming();
    TestBatteries();
    return H_Finish();
}
