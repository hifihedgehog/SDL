/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_jvs_proto.c, the JVS I/O
   boards of hifihedgehog/SDL#33 Part 5. The vectors are constructed from
   the framing of jvsio, MAME and JoypadOS. Revision values 13, 30 and 10 are
   MAME's defaults. Test numbers follow the ticket. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_jvs_proto.h"

static SDL_JVSState *State(Harness *h)
{
    return (SDL_JVSState *)h->state;
}

/* The ticket's bytes */
static const uint8_t reset_frame[6] = { 0xE0, 0xFF, 0x03, 0xF0, 0xD9, 0xCB };
static const uint8_t address1[6] = { 0xE0, 0xFF, 0x03, 0xF1, 0x01, 0xF4 };
static const uint8_t address2[6] = { 0xE0, 0xFF, 0x03, 0xF1, 0x02, 0xF5 };
static const uint8_t address_ok[6] = { 0xE0, 0x00, 0x03, 0x01, 0x01, 0x05 };
static const uint8_t ask_10[5] = { 0xE0, 0x01, 0x02, 0x10, 0x13 };
static const uint8_t ask_11[5] = { 0xE0, 0x01, 0x02, 0x11, 0x14 };
static const uint8_t ask_12[5] = { 0xE0, 0x01, 0x02, 0x12, 0x15 };
static const uint8_t ask_13[5] = { 0xE0, 0x01, 0x02, 0x13, 0x16 };
static const uint8_t ask_14[5] = { 0xE0, 0x01, 0x02, 0x14, 0x17 };
static const uint8_t ident_reply[65] = {
    0xE0, 0x00, 0x3D, 0x01, 0x01, 0x53, 0x45, 0x47, 0x41, 0x20, 0x45, 0x4E, 0x54, 0x45, 0x52, 0x50,
    0x52, 0x49, 0x53, 0x45, 0x53, 0x2C, 0x4C, 0x54, 0x44, 0x2E, 0x3B, 0x49, 0x2F, 0x4F, 0x20, 0x42,
    0x44, 0x20, 0x4A, 0x56, 0x53, 0x3B, 0x38, 0x33, 0x37, 0x2D, 0x31, 0x33, 0x35, 0x35, 0x31, 0x20,
    0x3B, 0x56, 0x65, 0x72, 0x31, 0x2E, 0x30, 0x30, 0x3B, 0x39, 0x38, 0x2F, 0x31, 0x30, 0x00, 0x58
};
static const uint8_t rev_13[7] = { 0xE0, 0x00, 0x04, 0x01, 0x01, 0x13, 0x19 };
static const uint8_t rev_30[7] = { 0xE0, 0x00, 0x04, 0x01, 0x01, 0x30, 0x36 };
static const uint8_t rev_10[7] = { 0xE0, 0x00, 0x04, 0x01, 0x01, 0x10, 0x16 };
static const uint8_t features_full[24] = { 0xE0, 0x00, 0x14, 0x01, 0x01, 0x01, 0x02, 0x0D, 0x00, 0x02, 0x02, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x12, 0x06, 0x00, 0x00, 0x00, 0x4D };
static const uint8_t poll_full[11] = { 0xE0, 0x01, 0x08, 0x20, 0x02, 0x02, 0x21, 0x02, 0x22, 0x08, 0x7A };
static const uint8_t features_basic[15] = { 0xE0, 0x00, 0x0C, 0x01, 0x01, 0x01, 0x02, 0x0D, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x22 };
static const uint8_t poll_basic[9] = { 0xE0, 0x01, 0x06, 0x20, 0x02, 0x02, 0x21, 0x02, 0x4E };
static const uint8_t features_analog[19] = { 0xE0, 0x00, 0x10, 0x01, 0x01, 0x01, 0x02, 0x0D, 0x00, 0x02, 0x02, 0x00, 0x00, 0x03, 0x08, 0x0A, 0x00, 0x00, 0x3B };
static const uint8_t idle[16] = { 0xE0, 0x00, 0x0D, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10 };
static const uint8_t r_start[16] = { 0xE0, 0x00, 0x0D, 0x01, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x90 };
static const uint8_t r_service[16] = { 0xE0, 0x00, 0x0D, 0x01, 0x01, 0x00, 0x40, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x50 };
static const uint8_t r_button1[16] = { 0xE0, 0x00, 0x0D, 0x01, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x12 };
static const uint8_t r_button10[16] = { 0xE0, 0x00, 0x0D, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x11 };
static const uint8_t r_p2_up[16] = { 0xE0, 0x00, 0x0D, 0x01, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x30 };
static const uint8_t r_test[16] = { 0xE0, 0x00, 0x0D, 0x01, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x90 };
static const uint8_t r_coin1[16] = { 0xE0, 0x00, 0x0D, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x11 };
static const uint8_t r_coin3fff[16] = { 0xE0, 0x00, 0x0D, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3F, 0xFF, 0x00, 0x00, 0x4E };
static const uint8_t r_coin_condition[16] = { 0xE0, 0x00, 0x0D, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x00, 0x00, 0x51 };
static const uint8_t r_escaped[17] = { 0xE0, 0x00, 0x0D, 0x01, 0x01, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xD0, 0xCF };
static const uint8_t r_bad_checksum[16] = { 0xE0, 0x00, 0x0D, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x11 };
static const uint8_t r_status02[5] = { 0xE0, 0x00, 0x02, 0x02, 0x04 };
static const uint8_t r_report02[6] = { 0xE0, 0x00, 0x03, 0x01, 0x02, 0x06 };
static const uint8_t echo[9] = { 0xE0, 0x01, 0x06, 0x20, 0x02, 0x02, 0x21, 0x02, 0x4E };
static uint8_t analog_rest[33], analog_min[33], analog_max[33];

/* An encoder written apart from the module's: checksum, then escapes */
static size_t Encode(uint8_t node, const uint8_t *data, size_t length, uint8_t *out)
{
    uint8_t raw[300];
    size_t n = 0, o = 0, i;
    uint8_t sum = 0;

    raw[n++] = node;
    raw[n++] = (uint8_t)(length + 1);
    memcpy(raw + n, data, length);
    n += length;
    for (i = 0; i < n; ++i) {
        sum = (uint8_t)(sum + raw[i]);
    }
    raw[n++] = sum;
    out[o++] = 0xE0;
    for (i = 0; i < n; ++i) {
        if (raw[i] == 0xE0 || raw[i] == 0xD0) {
            out[o++] = 0xD0;
            out[o++] = (uint8_t)(raw[i] - 1);
        } else {
            out[o++] = raw[i];
        }
    }
    return o;
}

static void BuildVectors(void)
{
    uint8_t data[32];
    size_t n = 0;
    int i;

    /* Status, switches for two players of two bytes, two coin slots, eight analog channels */
    data[n++] = 0x01;
    data[n++] = 0x01;
    data[n++] = 0x00;
    for (i = 0; i < 4; ++i) {
        data[n++] = 0x00;
    }
    data[n++] = 0x01;
    for (i = 0; i < 4; ++i) {
        data[n++] = 0x00;
    }
    data[n++] = 0x01;
    for (i = 0; i < 8; ++i) {
        data[n++] = 0x80;
        data[n++] = 0x00;
    }
    CHECK(Encode(0x00, data, n, analog_rest) == 33);
    /* Channel 1 is data bytes 13 and 14 */
    data[13] = 0x00;
    data[14] = 0x00;
    CHECK(Encode(0x00, data, n, analog_min) == 33);
    data[13] = 0xFF;
    data[14] = 0xC0;
    CHECK(Encode(0x00, data, n, analog_max) == 33);
    /* The ticket's checksums */
    CHECK(analog_rest[32] == 0x22 && analog_min[32] == 0xA2 && analog_max[32] == 0x61);
    CHECK(analog_rest[2] == 0x1E);
}

static void Write(Harness *h, uint64_t t, const uint8_t *data, size_t length)
{
    H_FeedAt(h, t, data, length);
}

/* From the open or a reset at time t, up to the first poll of one board
   with the given feature answer */
static void BringUpWith(Harness *h, const uint8_t *features, size_t features_length)
{
    const uint64_t t = h->now;

    H_Advance(h, t + 500);
    Write(h, t + 510, address_ok, sizeof(address_ok));
    H_Advance(h, t + 610);
    Write(h, t + 620, ident_reply, sizeof(ident_reply));
    Write(h, t + 630, rev_13, sizeof(rev_13));
    Write(h, t + 640, rev_30, sizeof(rev_30));
    Write(h, t + 650, rev_10, sizeof(rev_10));
    Write(h, t + 660, features, features_length);
}

static void BringUp(Harness *h)
{
    BringUpWith(h, features_basic, sizeof(features_basic));
}

static Harness *PresentBoard(void)
{
    Harness *h = H_Create(&SDL_SerialJVSModule);

    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1 && h->presence[1] == 1);
    H_SkipCalls(h);
    return h;
}

static void TestFraming(void)
{
    uint8_t out[64];
    static const uint8_t reset[2] = { 0xF0, 0xD9 };
    static const uint8_t poll[7] = { 0x20, 0x02, 0x02, 0x21, 0x02, 0x22, 0x08 };

    CHECK(SDL_JVS_BuildFrame(0xFF, reset, 2, out, sizeof(out)) == 6 && memcmp(out, reset_frame, 6) == 0);
    CHECK(SDL_JVS_BuildFrame(0x01, poll, 7, out, sizeof(out)) == 11 && memcmp(out, poll_full, 11) == 0);
    /* E0 and D0 inside a frame, the checksum too */
    {
        static const uint8_t data[2] = { 0xE0, 0xD0 };
        uint8_t expected[16];
        const size_t n = Encode(0x02, data, 2, expected);
        CHECK(SDL_JVS_BuildFrame(0x02, data, 2, out, sizeof(out)) == n && memcmp(out, expected, n) == 0);
        CHECK(n == 8 && out[3] == 0xD0 && out[4] == 0xDF && out[5] == 0xD0 && out[6] == 0xCF);
    }
    CHECK(SDL_JVS_BuildFrame(0x01, poll, 7, out, 10) == 0);
}

static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialJVSModule);
    const H_Call *call;

    /* 1 */
    H_Start(h);
    CHECK(H_IsCall(H_NextCall(h), 'O', 0));
    call = H_NextCall(h);
    CHECK(H_IsLine(call, 115200, 8, SDL_SERIAL_NOPARITY, 1, SDL_SERIAL_RTS_CONTROL_TOGGLE, false, SDL_SERIAL_DTR_CONTROL_ENABLE, 0));
    CHECK(H_IsCall(H_NextCall(h), 'T', 0));
    CHECK(H_IsCall(H_NextCall(h), 'P', 0));
    CHECK(H_IsWrite(H_NextCall(h), reset_frame, 6, 0));
    H_Advance(h, 499);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 500);
    CHECK(H_IsWrite(H_NextCall(h), reset_frame, 6, 500));
    CHECK(H_IsWrite(H_NextCall(h), address1, 6, 500));
    Write(h, 550, address_ok, sizeof(address_ok));
    CHECK(H_IsWrite(H_NextCall(h), address2, 6, 550));
    H_Advance(h, 649);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 650);
    CHECK(H_IsWrite(H_NextCall(h), ask_10, 5, 650));
    CHECK(h->npresence == 0);

    /* 2 */
    Write(h, 660, ident_reply, sizeof(ident_reply));
    CHECK(H_IsWrite(H_NextCall(h), ask_11, 5, 660));
    CHECK(strcmp(State(h)->board_info[0].ident, "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10") == 0);
    CHECK(h->nlogs == 1);
    Write(h, 670, rev_13, sizeof(rev_13));
    CHECK(H_IsWrite(H_NextCall(h), ask_12, 5, 670));
    Write(h, 680, rev_30, sizeof(rev_30));
    CHECK(H_IsWrite(H_NextCall(h), ask_13, 5, 680));
    Write(h, 690, rev_10, sizeof(rev_10));
    CHECK(H_IsWrite(H_NextCall(h), ask_14, 5, 690));
    CHECK(State(h)->board_info[0].command_revision == 0x13 && State(h)->board_info[0].jvs_revision == 0x30);
    CHECK(State(h)->board_info[0].communication_revision == 0x10);
    CHECK(h->npresence == 0);
    Write(h, 700, features_full, sizeof(features_full));
    CHECK(h->presence[0] == 1 && h->presence[1] == 1 && h->presence[2] == 0);
    CHECK(H_IsWrite(H_NextCall(h), poll_full, sizeof(poll_full), 700));
    CHECK(strcmp(h->identity[0].name, "JVS I/O Player 1") == 0 && strcmp(h->identity[1].name, "JVS I/O Player 2") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_ARCADE_STICK && h->identity[0].nbuttons == 14 && h->identity[1].nbuttons == 13);
    CHECK(h->identity[0].naxes == 8 && h->identity[1].naxes == 0 && h->identity[0].nhats == 1);
    CHECK(State(h)->board_info[0].players == 2 && State(h)->board_info[0].switches == 13);
    CHECK(State(h)->board_info[0].slots == 2 && State(h)->board_info[0].analog == 8 && State(h)->board_info[0].outputs == 6);
    {
        const SDL_SerialGamepadMap *map = &h->identity[0].mapping;
        CHECK(h->identity[0].has_mapping && h->identity[1].has_mapping);
        CHECK(map->x.kind == SDL_SERIAL_MAP_BUTTON && map->x.target == 0);
        CHECK(map->y.target == 1 && map->rightshoulder.target == 2 && map->a.target == 3 && map->b.target == 4);
        CHECK(map->righttrigger.target == 5 && map->leftshoulder.target == 6 && map->lefttrigger.target == 7);
        CHECK(map->leftstick.target == 8 && map->rightstick.target == 9);
        CHECK(map->start.target == 10 && map->back.target == 11 && map->misc1.target == 12 && map->guide.target == 13);
        CHECK(map->dpup.kind == SDL_SERIAL_MAP_HAT && map->dpup.target == 0x01 && map->dpright.target == 0x02);
        CHECK(map->dpdown.target == 0x04 && map->dpleft.target == 0x08);
        CHECK(map->leftx.kind == SDL_SERIAL_MAP_NONE && map->lefttrigger.kind == SDL_SERIAL_MAP_BUTTON);
        CHECK(h->identity[1].mapping.guide.kind == SDL_SERIAL_MAP_NONE);
    }
    H_Destroy(h);

    /* The same list without analog and outputs, and its poll */
    h = H_Create(&SDL_SerialJVSModule);
    H_Start(h);
    BringUp(h);
    CHECK(h->presence[0] == 1 && h->presence[1] == 1);
    H_SkipCalls(h);
    h->cursor = h->ncalls - 1;
    CHECK(H_IsWrite(H_NextCall(h), poll_basic, sizeof(poll_basic), 660));
    CHECK(h->identity[0].naxes == 0);
    H_Destroy(h);

    /* A port that refuses RTS toggle runs with RTS off */
    h = H_Create(&SDL_SerialJVSModule);
    h->reject_toggle = true;
    H_Start(h);
    CHECK(H_IsCall(H_NextCall(h), 'O', 0));
    CHECK(H_IsLine(H_NextCall(h), 115200, 8, SDL_SERIAL_NOPARITY, 1, SDL_SERIAL_RTS_CONTROL_TOGGLE, false, SDL_SERIAL_DTR_CONTROL_ENABLE, 0));
    CHECK(H_IsLine(H_NextCall(h), 115200, 8, SDL_SERIAL_NOPARITY, 1, SDL_SERIAL_RTS_CONTROL_DISABLE, false, SDL_SERIAL_DTR_CONTROL_ENABLE, 0));
    CHECK(H_IsCall(H_NextCall(h), 'T', 0));
    CHECK(H_IsCall(H_NextCall(h), 'P', 0));
    CHECK(H_IsWrite(H_NextCall(h), reset_frame, 6, 0));
    H_Destroy(h);
}

static void TestScan(void)
{
    Harness *h = H_Create(&SDL_SerialJVSModule);
    int i;

    /* No board: the resets and the scan repeat 1000 ms after the silent first address */
    H_Start(h);
    H_Advance(h, 600);
    CHECK(State(h)->step == SDL_JVS_STEP_SCAN);
    H_SkipCalls(h);
    H_Advance(h, 1599);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1600);
    CHECK(H_IsWrite(H_NextCall(h), reset_frame, 6, 1600));
    H_Advance(h, 2100);
    CHECK(H_IsWrite(H_NextCall(h), reset_frame, 6, 2100));
    CHECK(H_IsWrite(H_NextCall(h), address1, 6, 2100));
    H_Destroy(h);

    /* Addresses up to 1F, then identification of board 1 */
    h = H_Create(&SDL_SerialJVSModule);
    H_Start(h);
    H_Advance(h, 500);
    for (i = 1; i <= 31; ++i) {
        Write(h, 500 + (uint64_t)i, address_ok, sizeof(address_ok));
    }
    CHECK(State(h)->boards == 31 && State(h)->step == SDL_JVS_STEP_IDENTIFY && State(h)->board == 0);
    H_Destroy(h);

    /* A board that answers a second set address while it keeps its first,
       as jvsio's node does: board 2 is silent at its identify request, so
       the list ends at one board */
    h = H_Create(&SDL_SerialJVSModule);
    H_Start(h);
    H_Advance(h, 500);
    Write(h, 510, address_ok, sizeof(address_ok));
    Write(h, 520, address_ok, sizeof(address_ok));
    H_Advance(h, 620);
    CHECK(State(h)->boards == 2);
    Write(h, 630, ident_reply, sizeof(ident_reply));
    Write(h, 640, rev_13, sizeof(rev_13));
    Write(h, 650, rev_30, sizeof(rev_30));
    Write(h, 660, rev_10, sizeof(rev_10));
    Write(h, 670, features_basic, sizeof(features_basic));
    CHECK(h->npresence == 0 && State(h)->board == 1);
    H_SkipCalls(h);
    H_Advance(h, 770);
    CHECK(State(h)->boards == 1 && h->presence[0] == 1 && h->presence[1] == 1);
    CHECK(H_IsWrite(H_NextCall(h), poll_basic, sizeof(poll_basic), 770));
    H_Destroy(h);

    /* Board 1 silent at a later identify command is an error */
    h = H_Create(&SDL_SerialJVSModule);
    H_Start(h);
    H_Advance(h, 500);
    Write(h, 510, address_ok, sizeof(address_ok));
    H_Advance(h, 610);
    Write(h, 620, ident_reply, sizeof(ident_reply));
    H_Advance(h, 720);
    CHECK(State(h)->step == SDL_JVS_STEP_ERROR && State(h)->errors == 1);
    H_Destroy(h);

    /* A set address answered with report 02 is an error */
    h = H_Create(&SDL_SerialJVSModule);
    H_Start(h);
    H_Advance(h, 500);
    Write(h, 510, r_report02, sizeof(r_report02));
    CHECK(State(h)->step == SDL_JVS_STEP_ERROR);
    H_Destroy(h);
}

static void TestSwitches(void)
{
    Harness *h = PresentBoard();

    /* 3 */
    Write(h, 700, idle, sizeof(idle));
    CHECK(H_NoButtons(h, 0) && H_NoButtons(h, 1));
    CHECK(H_Snapshot(h, 0)->controls.hats[0] == 0);
    /* The next poll goes out at once */
    CHECK(H_IsWrite(H_NextCall(h), poll_basic, sizeof(poll_basic), 700));

    /* 4 */
    Write(h, 710, r_start, sizeof(r_start));
    CHECK(H_OnlyButton(h, 0, SDL_JVS_BUTTON_START) && H_NoButtons(h, 1));
    Write(h, 720, r_service, sizeof(r_service));
    CHECK(H_OnlyButton(h, 0, SDL_JVS_BUTTON_SERVICE));
    Write(h, 730, r_button1, sizeof(r_button1));
    CHECK(H_OnlyButton(h, 0, 0));
    Write(h, 740, r_button10, sizeof(r_button10));
    CHECK(H_OnlyButton(h, 0, 9));
    Write(h, 750, r_p2_up, sizeof(r_p2_up));
    CHECK(H_NoButtons(h, 0) && H_NoButtons(h, 1) && H_Snapshot(h, 1)->controls.hats[0] == SDL_SERIAL_HAT_UP);
    CHECK(H_Snapshot(h, 0)->controls.hats[0] == 0);
    Write(h, 760, r_test, sizeof(r_test));
    CHECK(H_OnlyButton(h, 0, SDL_JVS_BUTTON_TEST) && H_NoButtons(h, 1) && H_Snapshot(h, 1)->controls.hats[0] == 0);

    /* Every switch bit to its button or hat direction */
    {
        static const int byte0_buttons[8] = { 1, 0, -1, -1, -1, -1, SDL_JVS_BUTTON_SERVICE, SDL_JVS_BUTTON_START };
        static const uint8_t byte0_hats[8] = { 0, 0, SDL_SERIAL_HAT_RIGHT, SDL_SERIAL_HAT_LEFT, SDL_SERIAL_HAT_DOWN, SDL_SERIAL_HAT_UP, 0, 0 };
        uint8_t data[12], frame[32];
        int bit;

        for (bit = 0; bit < 8; ++bit) {
            memset(data, 0, sizeof(data));
            data[0] = 0x01;
            data[1] = 0x01;
            data[3] = (uint8_t)(1 << bit);
            data[7] = 0x01;
            H_Feed(h, frame, Encode(0x00, data, sizeof(data), frame));
            if (byte0_buttons[bit] >= 0) {
                CHECK(H_OnlyButton(h, 0, byte0_buttons[bit]) && H_Snapshot(h, 0)->controls.hats[0] == 0);
            } else {
                CHECK(H_NoButtons(h, 0) && H_Snapshot(h, 0)->controls.hats[0] == byte0_hats[bit]);
            }
            memset(data, 0, sizeof(data));
            data[0] = 0x01;
            data[1] = 0x01;
            data[4] = (uint8_t)(0x80 >> bit);
            data[7] = 0x01;
            H_Feed(h, frame, Encode(0x00, data, sizeof(data), frame));
            CHECK(H_OnlyButton(h, 0, 2 + bit));
            /* Player 2's bytes */
            memset(data, 0, sizeof(data));
            data[0] = 0x01;
            data[1] = 0x01;
            data[6] = (uint8_t)(0x80 >> bit);
            data[7] = 0x01;
            H_Feed(h, frame, Encode(0x00, data, sizeof(data), frame));
            CHECK(H_NoButtons(h, 0) && H_OnlyButton(h, 1, 2 + bit));
        }
    }

    /* 6: the checksum D0 travels escaped */
    Write(h, 800, r_escaped, sizeof(r_escaped));
    {
        static const int both[] = { SDL_JVS_BUTTON_START, SDL_JVS_BUTTON_SERVICE, -1 };
        CHECK(H_OnlyButtons(h, 0, both));
    }

    /* 9: the host's own echo is ignored */
    {
        const int mark = h->npublished;
        Write(h, 810, echo, sizeof(echo));
        CHECK(h->npublished == mark && State(h)->awaiting);
        Write(h, 811, idle, sizeof(idle));
        CHECK(H_NoButtons(h, 0));
    }
    H_Destroy(h);
}

static void TestCoins(void)
{
    Harness *h = PresentBoard();
    int mark;

    /* 5: count 0000 sets the baseline, then 0001 pulses once on player 1 */
    Write(h, 700, idle, sizeof(idle));
    mark = h->npublished;
    Write(h, 710, r_coin1, sizeof(r_coin1));
    CHECK(h->npublished == mark + 1 && H_Button(h, 0, SDL_JVS_BUTTON_COIN) && !H_Button(h, 1, SDL_JVS_BUTTON_COIN));
    /* The pulse holds 100 ms while the polls go on */
    Write(h, 720, r_coin1, sizeof(r_coin1));
    CHECK(H_Button(h, 0, SDL_JVS_BUTTON_COIN));
    H_Advance(h, 809);
    CHECK(H_Button(h, 0, SDL_JVS_BUTTON_COIN));
    H_Advance(h, 810);
    CHECK(!H_Button(h, 0, SDL_JVS_BUTTON_COIN));

    /* A condition skips the slot and keeps the baseline */
    mark = h->npublished;
    Write(h, 820, r_coin_condition, sizeof(r_coin_condition));
    CHECK(h->npublished == mark && !H_Button(h, 0, SDL_JVS_BUTTON_COIN));
    H_Destroy(h);

    /* From the 0000 baseline, a count of 1 under a condition is skipped */
    h = PresentBoard();
    Write(h, 700, idle, sizeof(idle));
    mark = h->npublished;
    Write(h, 710, r_coin_condition, sizeof(r_coin_condition));
    CHECK(h->npublished == mark && !H_Button(h, 0, SDL_JVS_BUTTON_COIN) && State(h)->board_info[0].coins[0] == 0);
    H_Destroy(h);

    /* 3FFF then 0000 is one coin */
    h = PresentBoard();
    Write(h, 700, r_coin3fff, sizeof(r_coin3fff));
    CHECK(!H_Button(h, 0, SDL_JVS_BUTTON_COIN));
    Write(h, 710, idle, sizeof(idle));
    CHECK(H_Button(h, 0, SDL_JVS_BUTTON_COIN));
    H_Destroy(h);

    /* Condition set on the first poll: no baseline, so 0001 later is the baseline */
    h = PresentBoard();
    Write(h, 700, r_coin_condition, sizeof(r_coin_condition));
    Write(h, 710, r_coin1, sizeof(r_coin1));
    CHECK(!H_Button(h, 0, SDL_JVS_BUTTON_COIN));
    H_Destroy(h);

    /* Three coins at once: three presses. A count that goes back is a new
       baseline, not 16382 coins. At most 16 presses per poll. */
    h = PresentBoard();
    {
        uint8_t data[12], frame[32];

        memset(data, 0, sizeof(data));
        data[0] = 0x01;
        data[1] = 0x01;
        data[7] = 0x01;
        H_Feed(h, frame, Encode(0x00, data, sizeof(data), frame));
        data[9] = 0x03;
        mark = h->npublished;
        H_Feed(h, frame, Encode(0x00, data, sizeof(data), frame));
        CHECK(h->npublished == mark + 5); /* press, release and press, release and press */
        H_Advance(h, h->now + 10);
        data[9] = 0x01;
        mark = h->npublished;
        H_Feed(h, frame, Encode(0x00, data, sizeof(data), frame));
        CHECK(h->npublished == mark && State(h)->board_info[0].coins[0] == 1);
        data[8] = 0x01;
        data[9] = 0x01; /* 0101: 256 coins */
        mark = h->npublished;
        H_Feed(h, frame, Encode(0x00, data, sizeof(data), frame));
        /* The coin button still holds from the three coins, so each of the
           16 presses is a release and a press */
        CHECK(h->npublished == mark + 2 * SDL_JVS_COIN_PULSES);
        /* Slot 2 pulses player 2 */
        H_Advance(h, h->now + 10);
        data[11] = 0x02;
        mark = h->npublished;
        H_Feed(h, frame, Encode(0x00, data, sizeof(data), frame));
        CHECK(h->npublished == mark + 3 && H_Last(h)->sub == 1 && H_Button(h, 1, SDL_JVS_BUTTON_COIN));
    }
    H_Destroy(h);
}

static void TestAnalog(void)
{
    Harness *h = H_Create(&SDL_SerialJVSModule);
    static const uint8_t poll_analog[11] = { 0xE0, 0x01, 0x08, 0x20, 0x02, 0x02, 0x21, 0x02, 0x22, 0x08, 0x7A };

    /* 7 */
    H_Start(h);
    BringUpWith(h, features_analog, sizeof(features_analog));
    CHECK(State(h)->board_info[0].analog == 8 && State(h)->board_info[0].analog_bits == 10);
    h->cursor = h->ncalls - 1;
    CHECK(H_IsWrite(H_NextCall(h), poll_analog, sizeof(poll_analog), 660));
    CHECK(h->identity[0].naxes == 8);
    Write(h, 700, analog_rest, sizeof(analog_rest));
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 7) == 0);
    Write(h, 710, analog_min, sizeof(analog_min));
    CHECK(H_Axis(h, 0, 0) == -32768 && H_Axis(h, 0, 1) == 0);
    Write(h, 720, analog_max, sizeof(analog_max));
    CHECK(H_Axis(h, 0, 0) == 32704);
    /* The axes are player 1's only */
    CHECK(H_Axis(h, 1, 0) == 0);
    /* An answer one channel short is an error */
    {
        uint8_t data[32], frame[48];
        memset(data, 0, sizeof(data));
        data[0] = 0x01;
        data[1] = 0x01;
        data[7] = 0x01;
        data[12] = 0x01;
        H_Feed(h, frame, Encode(0x00, data, 13 + 14, frame));
        CHECK(State(h)->step == SDL_JVS_STEP_ERROR && h->presence[0] == 0);
    }
    H_Destroy(h);
}

static void TestErrors(void)
{
    const uint8_t *vectors[3];
    size_t lengths[3];
    int i;

    /* 8 */
    vectors[0] = r_bad_checksum;
    lengths[0] = sizeof(r_bad_checksum);
    vectors[1] = r_status02;
    lengths[1] = sizeof(r_status02);
    vectors[2] = r_report02;
    lengths[2] = sizeof(r_report02);
    for (i = 0; i < 3; ++i) {
        Harness *h = PresentBoard();

        Write(h, 700, vectors[i], lengths[i]);
        CHECK(h->presence[0] == 0 && h->presence[1] == 0);
        CHECK(State(h)->step == SDL_JVS_STEP_ERROR);
        H_Advance(h, 1199);
        CHECK(H_NextCall(h) == NULL);
        H_Advance(h, 1200);
        CHECK(H_IsWrite(H_NextCall(h), reset_frame, 6, 1200));
        H_Destroy(h);
    }

    /* No answer within 100 ms of the poll */
    {
        Harness *h = PresentBoard();

        H_Advance(h, 759);
        CHECK(h->presence[0] == 1);
        H_Advance(h, 760);
        CHECK(h->presence[0] == 0 && h->presence[1] == 0);
        H_Advance(h, 1260);
        CHECK(H_IsWrite(H_NextCall(h), reset_frame, 6, 1260));
        /* After the reset the board comes back as new instances */
        BringUp(h);
        CHECK(h->presence[0] == 2 && h->presence[1] == 2);
        H_Destroy(h);
    }

    /* An answer one byte too long, and a whole answer under status 02, are errors */
    {
        uint8_t data[13], frame[40];
        int k;

        for (k = 0; k < 2; ++k) {
            Harness *h = PresentBoard();

            memset(data, 0, sizeof(data));
            data[0] = (k == 0) ? 0x01 : 0x02;
            data[1] = 0x01;
            data[7] = 0x01;
            Write(h, 700, frame, Encode(0x00, data, (k == 0) ? 13 : 12, frame));
            CHECK(State(h)->step == SDL_JVS_STEP_ERROR && h->presence[0] == 0);
            H_Destroy(h);
        }
    }

    /* A reply to node 00 with no request out, and a frame too short to hold a status, change nothing */
    {
        Harness *h = PresentBoard();
        static const uint8_t short_frame[4] = { 0xE0, 0x00, 0x01, 0x01 };

        Write(h, 700, idle, sizeof(idle));
        State(h)->awaiting = false;
        Write(h, 705, idle, sizeof(idle));
        CHECK(State(h)->step == SDL_JVS_STEP_POLL && h->presence[0] == 1);
        State(h)->awaiting = true;
        Write(h, 706, short_frame, sizeof(short_frame));
        CHECK(State(h)->step == SDL_JVS_STEP_ERROR);
        H_Destroy(h);
    }
}

static void TestOutput(void)
{
    Harness *h = PresentBoard();
    SDL_SerialOutput request;
    uint8_t expected[32];
    size_t n;

    /* The basic board has no general-purpose outputs */
    memset(&request, 0, sizeof(request));
    request.kind = SDL_SERIAL_OUTPUT_EFFECT;
    request.sub = 0;
    request.length = 1;
    request.data[0] = 0x55;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    CHECK(!State(h)->board_info[0].output_pending && h->nlogs == 2);
    H_Destroy(h);

    h = H_Create(&SDL_SerialJVSModule);
    H_Start(h);
    BringUpWith(h, features_full, sizeof(features_full));
    H_SkipCalls(h);
    /* Six outputs fit one byte */
    request.length = 2;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    CHECK(!State(h)->board_info[0].output_pending);
    request.length = 1;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    request.data[0] = 0x2A;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    CHECK(State(h)->board_info[0].output_pending);
    /* The next poll carries 32 01 2A, and its answer one more report */
    {
        uint8_t data[40], frame[64];
        size_t m = 0;
        int i;

        data[m++] = 0x01;
        data[m++] = 0x01;
        for (i = 0; i < 5; ++i) {
            data[m++] = 0x00;
        }
        data[m++] = 0x01;
        for (i = 0; i < 4; ++i) {
            data[m++] = 0x00;
        }
        data[m++] = 0x01;
        for (i = 0; i < 8; ++i) {
            data[m++] = 0x80;
            data[m++] = 0x00;
        }
        H_FeedAt(h, 700, frame, Encode(0x00, data, m, frame));
        {
            static const uint8_t poll[10] = { 0x20, 0x02, 0x02, 0x21, 0x02, 0x22, 0x08, 0x32, 0x01, 0x2A };
            n = Encode(0x01, poll, sizeof(poll), expected);
            CHECK(H_IsWrite(H_NextCall(h), expected, n, 700));
        }
        /* With a report for command 32 the answer checks, and the next poll is plain */
        {
            const int errors = State(h)->errors;
            data[m] = 0x01;
            H_FeedAt(h, 710, frame, Encode(0x00, data, m + 1, frame));
            CHECK(State(h)->errors == errors && State(h)->step == SDL_JVS_STEP_POLL);
            CHECK(H_IsWrite(H_NextCall(h), poll_full, sizeof(poll_full), 710));
        }
    }
    /* A player beyond every board's joysticks gets nothing */
    request.sub = 3;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    CHECK(!State(h)->board_info[0].output_pending);
    H_Destroy(h);
}

static void TestTwoBoards(void)
{
    Harness *h = H_Create(&SDL_SerialJVSModule);
    static const uint8_t ask2_10[5] = { 0xE0, 0x02, 0x02, 0x10, 0x14 };
    static const uint8_t features_three[15] = { 0xE0, 0x00, 0x0C, 0x01, 0x01, 0x01, 0x03, 0x0D, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x24 };
    uint8_t poll2[16];
    size_t n;

    H_Start(h);
    H_Advance(h, 500);
    Write(h, 510, address_ok, sizeof(address_ok));
    Write(h, 520, address_ok, sizeof(address_ok));
    H_Advance(h, 620);
    Write(h, 630, ident_reply, sizeof(ident_reply));
    Write(h, 640, rev_13, sizeof(rev_13));
    Write(h, 650, rev_30, sizeof(rev_30));
    Write(h, 660, rev_10, sizeof(rev_10));
    H_SkipCalls(h);
    Write(h, 670, features_three, sizeof(features_three));
    CHECK(H_IsWrite(H_NextCall(h), ask2_10, 5, 670));
    Write(h, 680, ident_reply, sizeof(ident_reply));
    Write(h, 690, rev_13, sizeof(rev_13));
    Write(h, 700, rev_30, sizeof(rev_30));
    Write(h, 710, rev_10, sizeof(rev_10));
    Write(h, 720, features_basic, sizeof(features_basic));
    /* Three players on board 1 and one of board 2's two: the cap is four */
    CHECK(h->presence[0] == 1 && h->presence[1] == 1 && h->presence[2] == 1 && h->presence[3] == 1);
    CHECK(State(h)->board_info[1].first_player == 3 && State(h)->board_info[1].mapped_players == 1);
    CHECK(strcmp(h->identity[3].name, "JVS I/O Player 4") == 0);
    {
        static const uint8_t data[5] = { 0x20, 0x03, 0x02, 0x21, 0x03 };
        n = Encode(0x01, data, sizeof(data), poll2);
        h->cursor = h->ncalls - 1;
        CHECK(H_IsWrite(H_NextCall(h), poll2, n, 720));
    }
    /* Board 1's answer, then board 2 is polled */
    {
        uint8_t data[20], frame[40];
        memset(data, 0, sizeof(data));
        data[0] = 0x01;
        data[1] = 0x01;
        data[7] = 0x80; /* player 3 Start */
        data[9] = 0x01;
        H_FeedAt(h, 730, frame, Encode(0x00, data, 16, frame));
        CHECK(H_OnlyButton(h, 2, SDL_JVS_BUTTON_START) && H_NoButtons(h, 0) && H_NoButtons(h, 1));
    }
    {
        static const uint8_t data[4] = { 0x20, 0x02, 0x02, 0x21 };
        uint8_t expected[16];
        const uint8_t full[5] = { data[0], data[1], data[2], data[3], 0x02 };
        n = Encode(0x02, full, sizeof(full), expected);
        h->cursor = h->ncalls - 1;
        CHECK(H_IsWrite(H_NextCall(h), expected, n, 730));
    }
    /* Board 2: its first player is player 4, its second has no joystick */
    {
        uint8_t data[12], frame[32];
        memset(data, 0, sizeof(data));
        data[0] = 0x01;
        data[1] = 0x01;
        data[3] = 0x80; /* board 2 player 1 Start: joystick 4 */
        data[5] = 0x80; /* board 2 player 2: none */
        data[7] = 0x01;
        H_FeedAt(h, 740, frame, Encode(0x00, data, sizeof(data), frame));
        CHECK(H_OnlyButton(h, 3, SDL_JVS_BUTTON_START) && H_OnlyButton(h, 2, SDL_JVS_BUTTON_START));
        CHECK(!H_Button(h, 3, SDL_JVS_BUTTON_TEST));
    }
    H_Destroy(h);
}

static void TestBatteryB(void)
{
    Harness *h = PresentBoard();

    Write(h, 700, r_start, sizeof(r_start));
    H_Advance(h, 705);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && h->presence[1] == 0);
    H_SkipCalls(h);
    H_Advance(h, 1705);
    CHECK(H_IsCall(H_NextCall(h), 'O', 1705));
    CHECK(H_IsLine(H_NextCall(h), 115200, 8, SDL_SERIAL_NOPARITY, 1, SDL_SERIAL_RTS_CONTROL_TOGGLE, false, SDL_SERIAL_DTR_CONTROL_ENABLE, 1705));
    CHECK(H_IsCall(H_NextCall(h), 'T', 1705));
    CHECK(H_IsCall(H_NextCall(h), 'P', 1705));
    CHECK(H_IsWrite(H_NextCall(h), reset_frame, 6, 1705));
    BringUp(h);
    CHECK(h->presence[0] == 2 && h->presence[1] == 2 && H_NoButtons(h, 0));
    H_Destroy(h);
}

static void TestBatteryA(void)
{
    Harness *h = PresentBoard();
    H_Vector vectors[20];
    int n = 0, i;

    H_SetVector(&vectors[n++], "3 idle", idle, sizeof(idle), 0, 0);
    H_SetVector(&vectors[n++], "4 Start", r_start, sizeof(r_start), 1, 0);
    H_SetVector(&vectors[n++], "4 Service", r_service, sizeof(r_service), 1, 0);
    H_SetVector(&vectors[n++], "4 button 1", r_button1, sizeof(r_button1), 1, 0);
    H_SetVector(&vectors[n++], "4 button 10", r_button10, sizeof(r_button10), 1, 0);
    H_SetVector(&vectors[n++], "4 player 2 Up", r_p2_up, sizeof(r_p2_up), 1, 0);
    H_SetVector(&vectors[n++], "4 TEST", r_test, sizeof(r_test), 1, 0);
    /* The first poll sets the coin baseline, so no pulse from the baseline */
    H_SetVector(&vectors[n++], "5 coin 0001", r_coin1, sizeof(r_coin1), 0, 0);
    H_SetVector(&vectors[n++], "5 condition", r_coin_condition, sizeof(r_coin_condition), 0, 0);
    H_SetVector(&vectors[n++], "6 escaped checksum", r_escaped, sizeof(r_escaped), 1, 0);
    /* The errors of test 8 remove the joysticks: nothing is published */
    H_SetVector(&vectors[n++], "8 bad checksum", r_bad_checksum, sizeof(r_bad_checksum), 0, 0);
    H_SetVector(&vectors[n++], "8 status 02", r_status02, sizeof(r_status02), 0, 0);
    H_SetVector(&vectors[n++], "8 report 02", r_report02, sizeof(r_report02), 0, 0);
    H_SetVector(&vectors[n++], "9 echo", echo, sizeof(echo), 0, 0);
    for (i = 0; i < n; ++i) {
        H_BatteryA(h, &vectors[i], BringUp);
    }
    H_Destroy(h);

    /* 5 from an idle baseline: 0001 is one pulse, published once */
    h = PresentBoard();
    Write(h, 700, idle, sizeof(idle));
    H_SetVector(&vectors[0], "5 coin after baseline", r_coin1, sizeof(r_coin1), 1, 0);
    H_BatteryA(h, &vectors[0], NULL);
    H_Destroy(h);

    /* 7 */
    h = H_Create(&SDL_SerialJVSModule);
    H_Start(h);
    BringUpWith(h, features_analog, sizeof(features_analog));
    H_SetVector(&vectors[0], "7 analog rest", analog_rest, sizeof(analog_rest), 0, 0);
    H_SetVector(&vectors[1], "7 analog 0000", analog_min, sizeof(analog_min), 1, 0);
    H_SetVector(&vectors[2], "7 analog FFC0", analog_max, sizeof(analog_max), 1, 0);
    for (i = 0; i < 3; ++i) {
        H_BatteryA(h, &vectors[i], NULL);
    }
    H_Destroy(h);
}

int main(void)
{
    BuildVectors();
    TestFraming();
    TestStartup();
    TestScan();
    TestSwitches();
    TestCoins();
    TestAnalog();
    TestErrors();
    TestOutput();
    TestTwoBoards();
    TestBatteryB();
    TestBatteryA();
    return H_Finish();
}
