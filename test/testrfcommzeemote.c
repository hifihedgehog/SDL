/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/windows/SDL_rfcomm_zeemote_proto.c, the
   Zeemote JS1 of hifihedgehog/SDL#33 Part 11. Test numbers follow the
   part's Zeemote section. The frames are built from the part's layout,
   since no capture exists. */

#include "testrfcommharness.h"

static const char name[] = "Zeemote JS1";

#define A_BIT (1u << SDL_ZEEMOTE_BUTTON_A)
#define B_BIT (1u << SDL_ZEEMOTE_BUTTON_B)
#define C_BIT (1u << SDL_ZEEMOTE_BUTTON_C)
#define D_BIT (1u << SDL_ZEEMOTE_BUTTON_D)

static void TestModule(void)
{
    RHarness *r = R_Create(&SDL_RFCOMMZeemoteModule, name);

    CHECK(SDL_RFCOMMZeemoteModule.family == SDL_RFCOMM_FAMILY_ZEEMOTE && SDL_RFCOMMZeemoteModule.has_service);
    CHECK(memcmp(SDL_RFCOMMZeemoteModule.service, "\x8E\x1F\x0C\xF7\x50\x8F\x48\x75\xB6\x2C\xFB\xB6\x7F\xD3\x48\x12", 16) == 0);
    /* By the service UUID, and nothing sent after connecting */
    CHECK(R_IsConnect(R_Next(r), 0, 0));
    R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    CHECK(R_Next(r) == NULL && r->nsnapshots == 0);
    /* No keep-alive: a quiet link asks for nothing */
    R_Advance(r, 60000);
    CHECK(R_Next(r) == NULL && r->link.phase == SDL_RFCOMM_UP);
    R_Destroy(r);
}

/* 5: the status frame makes the joystick and changes no control */
static void TestStatus(void)
{
    RHarness *r = R_Up(&SDL_RFCOMMZeemoteModule, name);
    const SDL_SerialSnapshot *last;

    R_Receive(r, (const uint8_t *)"\x07\xA1\x05\xFF\x00\x00\x00\x00", 8);
    CHECK(r->nsnapshots == 1 && R_Present(r));
    last = R_Last(r);
    CHECK(strcmp(last->identity.name, name) == 0 && last->identity.type == SDL_SERIAL_TYPE_GAMEPAD);
    CHECK(last->identity.naxes == 2 && last->identity.nbuttons == 4 && last->identity.nhats == 0);
    CHECK(R_Buttons(r) == 0 && R_Axis(r, SDL_ZEEMOTE_AXIS_X) == 0 && R_Axis(r, SDL_ZEEMOTE_AXIS_Y) == 0);
    CHECK(last->identity.has_mapping);
    CHECK(last->identity.mapping.a.kind == SDL_SERIAL_MAP_BUTTON && last->identity.mapping.a.target == SDL_ZEEMOTE_BUTTON_A);
    CHECK(last->identity.mapping.b.target == SDL_ZEEMOTE_BUTTON_B && last->identity.mapping.back.target == SDL_ZEEMOTE_BUTTON_C);
    CHECK(last->identity.mapping.start.target == SDL_ZEEMOTE_BUTTON_D && last->identity.mapping.start.kind == SDL_SERIAL_MAP_BUTTON);
    CHECK(last->identity.mapping.leftx.kind == SDL_SERIAL_MAP_AXIS && last->identity.mapping.leftx.target == SDL_ZEEMOTE_AXIS_X);
    CHECK(last->identity.mapping.lefty.target == SDL_ZEEMOTE_AXIS_Y);
    CHECK(last->identity.mapping.x.kind == SDL_SERIAL_MAP_NONE && last->identity.mapping.y.kind == SDL_SERIAL_MAP_NONE);
    /* A second status frame emits nothing */
    R_Receive(r, (const uint8_t *)"\x07\xA1\x05\xFF\x00\x00\x00\x00", 8);
    CHECK(r->nsnapshots == 1);
    R_Destroy(r);

    /* The JS1 H keeps its own name */
    r = R_Up(&SDL_RFCOMMZeemoteModule, "Zeemote JS1 H");
    R_Receive(r, (const uint8_t *)"\x08\xA1\x07\x00\xFE\xFE\xFE\xFE\xFE", 9);
    CHECK(strcmp(R_Last(r)->identity.name, "Zeemote JS1 H") == 0);
    R_Destroy(r);
}

/* 1, 2 and 7: the pressed set */
static void TestButtons(void)
{
    RHarness *r = R_Up(&SDL_RFCOMMZeemoteModule, name);
    int slot;

    R_Receive(r, (const uint8_t *)"\x08\xA1\x07\x00\xFE\xFE\xFE\xFE\xFE", 9);
    CHECK(R_Present(r) && R_Buttons(r) == A_BIT);
    R_Receive(r, (const uint8_t *)"\x08\xA1\x07\xFE\xFE\xFE\xFE\xFE\xFE", 9);
    CHECK(R_Buttons(r) == 0);
    /* 2 */
    R_Receive(r, (const uint8_t *)"\x08\xA1\x07\x03\x01\xFE\xFE\xFE\xFE", 9);
    CHECK(R_Buttons(r) == (D_BIT | B_BIT));
    /* A button missing from the set is released */
    R_Receive(r, (const uint8_t *)"\x08\xA1\x07\x01\xFE\xFE\xFE\xFE\xFE", 9);
    CHECK(R_Buttons(r) == B_BIT);
    /* All six slots count */
    R_Receive(r, (const uint8_t *)"\x08\xA1\x07\xFE\xFE\xFE\xFE\xFE\x02", 9);
    CHECK(R_Buttons(r) == C_BIT);
    R_Receive(r, (const uint8_t *)"\x08\xA1\x07\x00\x01\x02\x03\xFE\xFE", 9);
    CHECK(R_Buttons(r) == (A_BIT | B_BIT | C_BIT | D_BIT));
    R_Destroy(r);

    /* 7: slots of 4 and FD name no button, in every slot */
    for (slot = 0; slot < 6; ++slot) {
        uint8_t frame[9] = { 0x08, 0xA1, 0x07, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE };

        r = R_Up(&SDL_RFCOMMZeemoteModule, name);
        frame[3 + slot] = 0x04;
        R_Receive(r, frame, 9);
        CHECK(R_Present(r) && R_Buttons(r) == 0);
        frame[3 + slot] = 0xFD;
        R_Receive(r, frame, 9);
        CHECK(R_Buttons(r) == 0);
        frame[3 + slot] = 0x03;
        R_Receive(r, frame, 9);
        CHECK(R_Buttons(r) == D_BIT);
        R_Destroy(r);
    }
    /* A button frame of another length changes nothing but is consumed */
    r = R_Up(&SDL_RFCOMMZeemoteModule, name);
    R_Receive(r, (const uint8_t *)"\x06\xA1\x07\x00\x01\x02\x03\x08\xA1\x07\x01\xFE\xFE\xFE\xFE\xFE", 16);
    CHECK(r->nsnapshots == 1 && R_Buttons(r) == B_BIT);
    R_Destroy(r);
}

/* 3: the stick */
static void TestStick(void)
{
    RHarness *r = R_Up(&SDL_RFCOMMZeemoteModule, name);

    R_Receive(r, (const uint8_t *)"\x05\xA1\x08\x00\x7F\x80", 6);
    CHECK(R_Axis(r, SDL_ZEEMOTE_AXIS_X) == 32767 && R_Axis(r, SDL_ZEEMOTE_AXIS_Y) == -32768);
    R_Receive(r, (const uint8_t *)"\x05\xA1\x08\x00\x00\x00", 6);
    CHECK(R_Axis(r, SDL_ZEEMOTE_AXIS_X) == 0 && R_Axis(r, SDL_ZEEMOTE_AXIS_Y) == 0);
    /* Down is positive in both */
    R_Receive(r, (const uint8_t *)"\x05\xA1\x08\x00\x80\x7F", 6);
    CHECK(R_Axis(r, SDL_ZEEMOTE_AXIS_X) == -32768 && R_Axis(r, SDL_ZEEMOTE_AXIS_Y) == 32767);
    R_Receive(r, (const uint8_t *)"\x05\xA1\x08\x00\x81\x01", 6);
    CHECK(R_Axis(r, SDL_ZEEMOTE_AXIS_X) == -32512 && R_Axis(r, SDL_ZEEMOTE_AXIS_Y) == 258);
    /* Another stick index changes nothing */
    R_Receive(r, (const uint8_t *)"\x05\xA1\x08\x01\x7F\x7F", 6);
    CHECK(R_Axis(r, SDL_ZEEMOTE_AXIS_X) == -32512 && R_Axis(r, SDL_ZEEMOTE_AXIS_Y) == 258);
    R_Destroy(r);

    /* A stick frame can make the joystick with its first position */
    r = R_Up(&SDL_RFCOMMZeemoteModule, name);
    R_Receive(r, (const uint8_t *)"\x05\xA1\x08\x00\x7F\x00", 6);
    CHECK(r->nsnapshots == 1 && R_Present(r) && R_Axis(r, SDL_ZEEMOTE_AXIS_X) == 32767);
    R_Destroy(r);
}

/* 4: the battery */
static void TestBattery(void)
{
    RHarness *r = R_Up(&SDL_RFCOMMZeemoteModule, name);
    const SDL_RFCOMMBase *base = (const SDL_RFCOMMBase *)r->state;

    CHECK(base->battery_mv == -1);
    R_Receive(r, (const uint8_t *)"\x04\xA1\x11\x0F\xA0", 5);
    CHECK(base->battery_mv == 4000 && R_Present(r) && R_Buttons(r) == 0);
    R_Receive(r, (const uint8_t *)"\x04\xA1\x11\x0B\xB8", 5);
    CHECK(base->battery_mv == 3000);
    /* Of another length it is skipped */
    R_Receive(r, (const uint8_t *)"\x05\xA1\x11\x0F\xA0\x00", 6);
    CHECK(base->battery_mv == 3000);
    R_Destroy(r);
}

/* 6: framing */
static void TestFraming(void)
{
    static const uint8_t stream[] = {
        0x07, 0xA1, 0x05, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0x08, 0xA1, 0x07, 0x00, 0x02, 0xFE, 0xFE, 0xFE, 0xFE,
        0x05, 0xA1, 0x08, 0x00, 0x40, 0xC0,
        0x04, 0xA1, 0x11, 0x0F, 0xA0,
        0x08, 0xA1, 0x07, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0x03
    };
    RHarness *r = R_Up(&SDL_RFCOMMZeemoteModule, name);

    /* Two frames in one read */
    R_Receive(r, (const uint8_t *)"\x08\xA1\x07\x00\xFE\xFE\xFE\xFE\xFE\x05\xA1\x08\x00\x7F\x00", 15);
    CHECK(r->nsnapshots == 2 && R_Buttons(r) == A_BIT && R_Axis(r, SDL_ZEEMOTE_AXIS_X) == 32767);
    R_Destroy(r);

    /* Every split gives the same result */
    R_CheckSplits(&SDL_RFCOMMZeemoteModule, name, stream, sizeof(stream));

    /* A leading 00 */
    r = R_Up(&SDL_RFCOMMZeemoteModule, name);
    R_Receive(r, (const uint8_t *)"\x00\x08\xA1\x07\x00\xFE\xFE\xFE\xFE\xFE", 10);
    CHECK(r->nsnapshots == 1 && R_Buttons(r) == A_BIT);
    R_Destroy(r);

    /* A frame with A0 in the second place is skipped one byte at a time */
    r = R_Up(&SDL_RFCOMMZeemoteModule, name);
    R_Receive(r, (const uint8_t *)"\x08\xA0\x07\x00\xFE\xFE\xFE\xFE\xFE\x08\xA1\x07\x01\xFE\xFE\xFE\xFE\xFE", 18);
    CHECK(r->nsnapshots == 1 && R_Buttons(r) == B_BIT);
    R_Destroy(r);

    /* Types 3 and 4 and unknown types are skipped whole and make no joystick */
    r = R_Up(&SDL_RFCOMMZeemoteModule, name);
    R_Receive(r, (const uint8_t *)"\x03\xA1\x03\x00\x04\xA1\x04\x00\x00\x0A\xA1\x1C\x04\x05\x06\x07\x08\x09\x0A\x0B", 20);
    CHECK(r->nsnapshots == 0);
    R_Receive(r, (const uint8_t *)"\x08\xA1\x07\x02\xFE\xFE\xFE\xFE\xFE", 9);
    CHECK(r->nsnapshots == 1 && R_Buttons(r) == C_BIT);
    R_Destroy(r);

    /* A length under 2 or past 63 is noise */
    r = R_Up(&SDL_RFCOMMZeemoteModule, name);
    R_Receive(r, (const uint8_t *)"\x01\xA1\x40\xA1\x08\xA1\x07\x03\xFE\xFE\xFE\xFE\xFE", 13);
    CHECK(r->nsnapshots == 1 && R_Buttons(r) == D_BIT);
    R_Destroy(r);

    /* A length of 63 waits for its whole frame */
    {
        uint8_t big[64 + 9];

        memset(big, 0x00, sizeof(big));
        big[0] = 0x3F;
        big[1] = 0xA1;
        big[2] = 0x30;
        memcpy(big + 64, "\x08\xA1\x07\x00\xFE\xFE\xFE\xFE\xFE", 9);
        r = R_Up(&SDL_RFCOMMZeemoteModule, name);
        R_Receive(r, big, 63);
        CHECK(r->nsnapshots == 0);
        R_Receive(r, big + 63, sizeof(big) - 63);
        CHECK(r->nsnapshots == 1 && R_Buttons(r) == A_BIT);
        R_Destroy(r);
    }

    /* Every proper prefix of a frame changes nothing */
    {
        size_t i;

        for (i = 0; i < 9; ++i) {
            r = R_Up(&SDL_RFCOMMZeemoteModule, name);
            R_Receive(r, (const uint8_t *)"\x08\xA1\x07\x00\xFE\xFE\xFE\xFE\xFE", i);
            CHECK(r->nsnapshots == 0);
            R_Destroy(r);
        }
    }
}

/* A lost link removes the joystick with every control released */
static void TestLost(void)
{
    RHarness *r = R_Up(&SDL_RFCOMMZeemoteModule, name);

    R_Receive(r, (const uint8_t *)"\x08\xA1\x07\x00\x01\x02\x03\xFE\xFE\x05\xA1\x08\x00\x7F\x7F", 15);
    CHECK(R_Buttons(r) != 0);
    R_Lost(r);
    CHECK(!R_Present(r) && R_Buttons(r) == 0 && R_Axis(r, SDL_ZEEMOTE_AXIS_X) == 0);
    R_Advance(r, SDL_RFCOMM_RETRY_MS);
    CHECK(R_IsConnect(R_Next(r), 0, SDL_RFCOMM_RETRY_MS));
    R_Destroy(r);
}

int main(void)
{
    TestModule();
    TestStatus();
    TestButtons();
    TestStick();
    TestBattery();
    TestFraming();
    TestLost();
    return H_Finish();
}
