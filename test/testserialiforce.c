/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_iforce_proto.c, the
   RS-232 I-Force wheels and joysticks of hifihedgehog/SDL#33 Part 8. Test
   numbers follow the part's serial section. The replies are constructed
   from the protocol document's format and the WingMan Force's values. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_iforce_proto.h"

static const uint8_t query_o[] = { 0x2B, 0xFF, 0x01, 0x4F, 0x9A };
static const uint8_t query_m[] = { 0x2B, 0xFF, 0x01, 0x4D, 0x98 };
static const uint8_t query_p[] = { 0x2B, 0xFF, 0x01, 0x50, 0x85 };
static const uint8_t query_b[] = { 0x2B, 0xFF, 0x01, 0x42, 0x97 };
static const uint8_t query_n[] = { 0x2B, 0xFF, 0x01, 0x4E, 0x9B };
static const uint8_t query_c[] = { 0x2B, 0xFF, 0x01, 0x43, 0x96 };
static const uint8_t query_e[] = { 0x2B, 0xFF, 0x01, 0x45, 0x90 };
static const uint8_t query_v[] = { 0x2B, 0xFF, 0x01, 0x56, 0x83 };

static const uint8_t reply_o[] = { 0x2B, 0xFF, 0x01, 0x4F, 0x9A };
static const uint8_t reply_m_boeder[] = { 0x2B, 0xFF, 0x03, 0x4D, 0xEF, 0x05, 0x70 };
static const uint8_t reply_p_boeder[] = { 0x2B, 0xFF, 0x03, 0x50, 0x86, 0x88, 0x89 };
static const uint8_t reply_m_trust[] = { 0x2B, 0xFF, 0x03, 0x4D, 0xD6, 0x06, 0x4A };
static const uint8_t reply_p_trust[] = { 0x2B, 0xFF, 0x03, 0x50, 0xBC, 0x29, 0x12 };
static const uint8_t reply_b[] = { 0x2B, 0xFF, 0x03, 0x42, 0xC8, 0x00, 0x5D };
static const uint8_t reply_n[] = { 0x2B, 0xFF, 0x02, 0x4E, 0x0A, 0x92 };
static const uint8_t reply_c[] = { 0x2B, 0xFF, 0x01, 0x43, 0x96 };
static const uint8_t reply_e[] = { 0x2B, 0xFF, 0x03, 0x45, 0x01, 0x00, 0x93 };
static const uint8_t reply_v[] = { 0x2B, 0xFF, 0x04, 0x56, 0x01, 0x02, 0x03, 0x86 };

/* 3 */
static const uint8_t wheel_neutral[] = { 0x2B, 0x03, 0x07, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xF0, 0xDF };

static SDL_IForceSerialState *State(Harness *h)
{
    return (SDL_IForceSerialState *)h->state;
}

static size_t Frame(uint8_t *out, uint8_t command, const uint8_t *data, size_t length)
{
    return SDL_IForce_BuildSerialFrame(out, 32, command, data, length);
}

/* Answers every query, the given M and P among them */
static void Answer(Harness *h, const uint8_t *m, size_t m_length, const uint8_t *p, size_t p_length)
{
    H_Feed(h, reply_o, sizeof(reply_o));
    H_Feed(h, m, m_length);
    H_Feed(h, p, p_length);
    H_Feed(h, reply_b, sizeof(reply_b));
    H_Feed(h, reply_n, sizeof(reply_n));
    H_Feed(h, reply_c, sizeof(reply_c));
    H_Feed(h, reply_e, sizeof(reply_e));
    H_Feed(h, reply_o, sizeof(reply_o));
    H_Feed(h, reply_v, sizeof(reply_v));
}

static void BringUpBoeder(Harness *h)
{
    Answer(h, reply_m_boeder, sizeof(reply_m_boeder), reply_p_boeder, sizeof(reply_p_boeder));
}

static Harness *PresentBoeder(void)
{
    Harness *h = H_Create(&SDL_SerialIForceModule);

    H_Start(h);
    BringUpBoeder(h);
    CHECK(h->presence[0] == 1);
    H_SkipCalls(h);
    return h;
}

static void Output(Harness *h, const char *bytes, size_t length)
{
    SDL_SerialOutput request;

    memset(&request, 0, sizeof(request));
    request.kind = SDL_SERIAL_OUTPUT_EFFECT;
    request.length = length;
    memcpy(request.data, bytes, length);
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
}

/* 1 */
static void TestQueries(void)
{
    Harness *h = H_Create(&SDL_SerialIForceModule);
    int i;

    /* The nine frames, each after the reply to the one before it */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 38400, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), query_o, sizeof(query_o), 0));
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 10);
    H_Feed(h, reply_o, sizeof(reply_o));
    CHECK(H_IsWrite(H_NextCall(h), query_m, sizeof(query_m), 10));
    H_Feed(h, reply_m_boeder, sizeof(reply_m_boeder));
    CHECK(H_IsWrite(H_NextCall(h), query_p, sizeof(query_p), 10));
    H_Feed(h, reply_p_boeder, sizeof(reply_p_boeder));
    CHECK(H_IsWrite(H_NextCall(h), query_b, sizeof(query_b), 10));
    H_Feed(h, reply_b, sizeof(reply_b));
    CHECK(H_IsWrite(H_NextCall(h), query_n, sizeof(query_n), 10));
    H_Feed(h, reply_n, sizeof(reply_n));
    CHECK(H_IsWrite(H_NextCall(h), query_c, sizeof(query_c), 10));
    H_Feed(h, reply_c, sizeof(reply_c));
    CHECK(H_IsWrite(H_NextCall(h), query_e, sizeof(query_e), 10));
    H_Feed(h, reply_e, sizeof(reply_e));
    CHECK(H_IsWrite(H_NextCall(h), query_o, sizeof(query_o), 10));
    CHECK(h->presence[0] == 0);
    H_Feed(h, reply_o, sizeof(reply_o));
    CHECK(H_IsWrite(H_NextCall(h), query_v, sizeof(query_v), 10));
    H_Feed(h, reply_v, sizeof(reply_v));
    CHECK(H_NextCall(h) == NULL && h->presence[0] == 1);
    /* No spring or enable command follows */
    H_Advance(h, 5000);
    CHECK(H_NextCall(h) == NULL);
    H_Destroy(h);

    /* O unanswered: one try per second from the moment it left, 20 at most */
    h = H_Create(&SDL_SerialIForceModule);
    H_Start(h);
    CHECK(H_ExpectOpened(h, 38400, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), query_o, sizeof(query_o), 0));
    for (i = 1; i < 20; ++i) {
        H_Advance(h, (uint64_t)i * 1000 - 1);
        CHECK(H_NextCall(h) == NULL);
        H_Advance(h, (uint64_t)i * 1000);
        CHECK(H_IsWrite(H_NextCall(h), query_o, sizeof(query_o), (uint64_t)i * 1000));
    }
    H_Advance(h, 60000);
    CHECK(H_NextCall(h) == NULL && h->presence[0] == 0 && h->nlogs == 1);
    CHECK(strcmp(h->last_log, "no answer to O after 20 tries") == 0);
    /* A reply after that changes nothing */
    H_Feed(h, reply_o, sizeof(reply_o));
    CHECK(H_NextCall(h) == NULL && h->presence[0] == 0);
    H_Destroy(h);

    /* The window starts when the query leaves, not when it is queued */
    h = H_Create(&SDL_SerialIForceModule);
    h->pend_writes = true;
    H_Start(h);
    CHECK(H_ExpectOpened(h, 38400, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), query_o, sizeof(query_o), 0));
    H_Advance(h, 1500);
    CHECK(H_NextCall(h) == NULL);
    H_CompleteWrite(h, true);
    H_Advance(h, 2499);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 2500);
    CHECK(H_IsWrite(H_NextCall(h), query_o, sizeof(query_o), 2500));
    /* A failed write counts as sent */
    H_CompleteWrite(h, false);
    H_Advance(h, 3500);
    CHECK(H_IsWrite(H_NextCall(h), query_o, sizeof(query_o), 3500));
    H_Destroy(h);

    /* A reply that comes before its own write is done: the next letter's
       window starts when that letter's write is done */
    h = H_Create(&SDL_SerialIForceModule);
    h->pend_writes = true;
    H_Start(h);
    CHECK(H_ExpectOpened(h, 38400, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), query_o, sizeof(query_o), 0));
    H_Feed(h, reply_o, sizeof(reply_o));
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 100);
    H_CompleteWrite(h, true);
    CHECK(H_IsWrite(H_NextCall(h), query_m, sizeof(query_m), 100));
    H_Advance(h, 1200);
    H_CompleteWrite(h, true);
    H_Advance(h, 2199);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 2200);
    CHECK(H_IsWrite(H_NextCall(h), query_p, sizeof(query_p), 2200));
    H_Destroy(h);

    /* Replies while the port holds every write: 16 O requests fill the
       queue, and a request that finds it full counts as unanswered when its
       window ends */
    h = H_Create(&SDL_SerialIForceModule);
    h->pend_writes = true;
    H_Start(h);
    CHECK(H_ExpectOpened(h, 38400, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWrite(H_NextCall(h), query_o, sizeof(query_o), 0));
    for (i = 0; i < 17; ++i) {
        H_Feed(h, reply_c, sizeof(reply_c));
    }
    CHECK(State(h)->queries.open_tries == 17 && State(h)->base.action_count == SDL_SERIAL_MAX_ACTIONS);
    H_Advance(h, 2999);
    CHECK(h->nlogs == 0 && State(h)->queries.open_tries == 19);
    H_Advance(h, 3000);
    CHECK(h->nlogs == 1 && strcmp(h->last_log, "no answer to O after 20 tries") == 0);
    CHECK(H_NextCall(h) == NULL && h->presence[0] == 0);
    H_Destroy(h);

    /* A reply for another letter fails the query at once */
    h = H_Create(&SDL_SerialIForceModule);
    H_Start(h);
    H_SkipCalls(h);
    H_Advance(h, 5);
    H_Feed(h, reply_c, sizeof(reply_c));
    CHECK(H_IsWrite(H_NextCall(h), query_o, sizeof(query_o), 5));
    CHECK(State(h)->queries.open_tries == 1);
    H_Feed(h, reply_o, sizeof(reply_o));
    H_Feed(h, reply_c, sizeof(reply_c));
    CHECK(H_IsWrite(H_NextCall(h), query_m, sizeof(query_m), 5));
    CHECK(H_IsWrite(H_NextCall(h), query_p, sizeof(query_p), 5));
    CHECK(State(h)->queries.vendor_id == 0);
    /* 19 wrong letters and a right one make 20 O tries */
    H_Destroy(h);
    h = H_Create(&SDL_SerialIForceModule);
    H_Start(h);
    for (i = 0; i < 19; ++i) {
        H_Feed(h, reply_v, sizeof(reply_v));
    }
    H_Feed(h, reply_o, sizeof(reply_o));
    CHECK(State(h)->queries.open && State(h)->queries.open_tries == 20);
    H_Destroy(h);
}

/* 2 */
static void TestReplies(void)
{
    Harness *h = H_Create(&SDL_SerialIForceModule);

    H_Start(h);
    BringUpBoeder(h);
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Boeder Force Feedback Wheel") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_WHEEL && h->identity[0].naxes == 3);
    CHECK(h->identity[0].nbuttons == 9 && h->identity[0].nhats == 1 && !h->identity[0].has_mapping);
    CHECK(State(h)->queries.memory_end == 200 && State(h)->queries.effects == 10);
    H_Destroy(h);

    h = H_Create(&SDL_SerialIForceModule);
    H_Start(h);
    Answer(h, reply_m_trust, sizeof(reply_m_trust), reply_p_trust, sizeof(reply_p_trust));
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Trust Force Feedback Race Master") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_WHEEL);
    H_Destroy(h);

    /* A USB model's IDs give its layout */
    {
        uint8_t m[16], p[16];
        size_t m_length, p_length;

        h = H_Create(&SDL_SerialIForceModule);
        H_Start(h);
        m_length = Frame(m, 0xFF, (const uint8_t *)"\x4D\x6D\x04", 3);
        p_length = Frame(p, 0xFF, (const uint8_t *)"\x50\x81\xC2", 3);
        Answer(h, m, m_length, p, p_length);
        CHECK(strcmp(h->identity[0].name, "Logitech WingMan Force") == 0);
        CHECK(h->identity[0].type == SDL_SERIAL_TYPE_FLIGHT_STICK && h->identity[0].naxes == 3);
        CHECK(h->identity[0].nbuttons == 13 && h->identity[0].nhats == 1);
        H_Destroy(h);

        /* Any other pair, and an unanswered pair, get the joystick layout */
        h = H_Create(&SDL_SerialIForceModule);
        H_Start(h);
        m_length = Frame(m, 0xFF, (const uint8_t *)"\x4D\x34\x12", 3);
        p_length = Frame(p, 0xFF, (const uint8_t *)"\x50\x78\x56", 3);
        Answer(h, m, m_length, p, p_length);
        CHECK(strcmp(h->identity[0].name, "Unknown I-Force Device [1234:5678]") == 0);
        CHECK(h->identity[0].type == SDL_SERIAL_TYPE_FLIGHT_STICK && h->identity[0].nbuttons == 13);
        H_Destroy(h);

        h = H_Create(&SDL_SerialIForceModule);
        H_Start(h);
        H_Feed(h, reply_o, sizeof(reply_o));
        H_Advance(h, 1000);
        H_Advance(h, 2000);
        H_Feed(h, reply_b, sizeof(reply_b));
        H_Feed(h, reply_n, sizeof(reply_n));
        H_Advance(h, 6000);
        CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Unknown I-Force Device [0000:0000]") == 0);
        H_Destroy(h);
    }
}

/* 3 */
static void TestInput(void)
{
    Harness *h = H_Create(&SDL_SerialIForceModule);
    uint8_t frame[32];
    int mark;

    /* Input before identification changes nothing */
    H_Start(h);
    H_Feed(h, reply_o, sizeof(reply_o));
    mark = h->npublished;
    H_Feed(h, wheel_neutral, sizeof(wheel_neutral));
    H_Feed(h, frame, Frame(frame, 0x03, (const uint8_t *)"\x80\x07\x00\x00\x00\x01\x20", 7));
    CHECK(h->npublished == mark && h->presence[0] == 0);
    /* and leaves the device unidentified, so no effect goes */
    H_SkipCalls(h);
    Output(h, "\x42\x04", 2);
    CHECK(H_NextCall(h) == NULL);
    H_Destroy(h);

    h = PresentBoeder();
    /* The neutral wheel is the state it came up with */
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 1) == -32768 && H_Axis(h, 0, 2) == -32768 && H_NoButtons(h, 0));
    mark = h->npublished;
    H_Feed(h, wheel_neutral, sizeof(wheel_neutral));
    CHECK(h->npublished == mark);
    H_Feed(h, frame, Frame(frame, 0x03, (const uint8_t *)"\x80\x07\x00\x80\x00\x01\x20", 7));
    CHECK(H_Axis(h, 0, 0) == 32767 && H_Axis(h, 0, 1) == 32767 && H_Axis(h, 0, 2) == -129);
    CHECK(H_OnlyButton(h, 0, 0) && H_Snapshot(h, 0)->controls.hats[0] == SDL_IFORCE_HAT_RIGHT);
    /* The grip is the ninth button */
    H_Feed(h, frame, Frame(frame, 0x02, (const uint8_t *)"\x02\x00", 2));
    CHECK(H_Button(h, 0, 8) && H_Button(h, 0, 0));
    H_Feed(h, frame, Frame(frame, 0x02, (const uint8_t *)"\x01\x81\x00\x10\x00", 5));
    CHECK(!H_Button(h, 0, 8));
    /* A query reply now changes nothing */
    mark = h->npublished;
    H_Feed(h, reply_o, sizeof(reply_o));
    CHECK(h->npublished == mark);
    H_Destroy(h);

    /* A rudder model's fourth axis */
    h = H_Create(&SDL_SerialIForceModule);
    H_Start(h);
    {
        uint8_t m[16], p[16];
        const size_t m_length = Frame(m, 0xFF, (const uint8_t *)"\x4D\xF8\x06", 3);
        const size_t p_length = Frame(p, 0xFF, (const uint8_t *)"\x50\x03\x00", 3);

        Answer(h, m, m_length, p, p_length);
    }
    CHECK(strcmp(h->identity[0].name, "Guillemot Jet Leader Force Feedback") == 0 && h->identity[0].naxes == 4);
    H_Feed(h, frame, Frame(frame, 0x01, (const uint8_t *)"\x00\x00\x00\x00\xFF\x00\xF0\x7F", 8));
    CHECK(H_Axis(h, 0, 3) == 32767 && H_Axis(h, 0, 2) == -32768);
    H_Destroy(h);
}

/* 4: the framing rules through the module, and batteries A and B */
static void TestFraming(void)
{
    static const uint8_t stream[] = {
        0x55, 0x2B, 0x04, 0x2B, 0x00, 0x03, 0x00, 0x07, 0x80, 0x07, 0x00, 0x80, 0x00, 0x01, 0x20, 0x09,
        0x2B, 0x03, 0x11, 0x2B, 0x03, 0x07, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x02, 0xF0, 0xDD
    };
    Harness *h = PresentBoeder();
    Harness *whole;
    H_Vector vectors[2], valid;
    static uint8_t turn[16], status[16];
    size_t k;
    int mark;

    /* One feed of the stream is the reference */
    whole = PresentBoeder();
    mark = whole->npublished;
    H_Feed(whole, stream, sizeof(stream));
    CHECK(whole->npublished == mark + 2);
    CHECK(H_Axis(whole, 0, 0) == 0 && H_OnlyButton(whole, 0, 1));
    CHECK(State(whole)->parser.mismatches == 0);
    for (k = 0; k <= sizeof(stream); ++k) {
        H_Saved *saved = H_Save(h);

        H_Feed(h, stream, k);
        H_Feed(h, stream + k, sizeof(stream) - k);
        CHECK(H_SameState(h, whole) && H_SamePublished(h, mark, whole, mark));
        H_Restore(h, saved);
        H_FreeSaved(saved);
    }
    H_Destroy(whole);

    H_SetVector(&vectors[0], "wheel right", turn, Frame(turn, 0x03, (const uint8_t *)"\x80\x07\xFF\xFF\x00\x00\xF0", 7), 1, 0);
    H_SetVector(&vectors[1], "grip", status, Frame(status, 0x02, (const uint8_t *)"\x02\x00\x00", 3), 1, 0);
    H_BatteryA(h, &vectors[0], BringUpBoeder);
    H_BatteryA(h, &vectors[1], BringUpBoeder);
    H_SetVector(&valid, "valid", turn, vectors[0].length, 1, 0);
    H_BatteryA6(h, "type 04", (const uint8_t *)"\x2B\x04\x07\x80\x07\xFF\xFF\x00\x00\xF0\x5F", 11, &valid);
    H_BatteryA6(h, "short wheel", (const uint8_t *)"\x2B\x03\x06\x80\x07\xFF\xFF\x00\x00\xA9", 10, &valid);
    H_Destroy(h);
}

/* 5 */
static void TestChecksum(void)
{
    Harness *h = PresentBoeder();
    uint8_t frame[32];
    const size_t length = Frame(frame, 0x03, (const uint8_t *)"\x80\x07\xFF\xFF\x00\x00\xF0", 7);

    frame[length - 1] ^= 0x5A;
    H_Feed(h, frame, length);
    CHECK(H_Axis(h, 0, 0) == 32767 && State(h)->parser.mismatches == 1);
    frame[length - 1] ^= 0x5A;
    H_Feed(h, frame, length);
    CHECK(State(h)->parser.mismatches == 1);
    H_Destroy(h);
}

/* 6 */
static void TestOutput(void)
{
    Harness *h = H_Create(&SDL_SerialIForceModule);
    uint8_t core[15];

    CHECK(SDL_SerialIForceModule.effect_min == 2 && SDL_SerialIForceModule.effect_max == 15);
    CHECK(SDL_SerialIForceModule.effect_max <= SDL_SERIAL_MAX_EFFECT && !SDL_SerialIForceModule.rumble);

    /* Nothing goes before identification */
    H_Start(h);
    H_SkipCalls(h);
    Output(h, "\x42\x04", 2);
    CHECK(H_NextCall(h) == NULL);
    H_Destroy(h);

    h = PresentBoeder();
    Output(h, "\x42\x04", 2);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x2B\x42\x01\x04\x6C", 5, h->now));
    Output(h, "\x42\x01", 2);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x2B\x42\x01\x01\x69", 5, h->now));
    Output(h, "\x43\x7F", 2);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x2B\x43\x01\x7F\x16", 5, h->now));
    Output(h, "\x40\x03\x00", 3);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x2B\x40\x02\x03\x00\x6A", 6, h->now));
    Output(h, "\x40\x04\x01", 3);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x2B\x40\x02\x04\x01\x6C", 6, h->now));
    Output(h, "\x41\x00\x01\x01", 4);
    CHECK(H_IsWrite(H_NextCall(h), (const uint8_t *)"\x2B\x41\x03\x00\x01\x01\x69", 7, h->now));
    /* The longest command, a whole effect core */
    memset(core, 0x11, sizeof(core));
    core[0] = 0x01;
    Output(h, (const char *)core, sizeof(core));
    {
        const H_Call *call = H_NextCall(h);
        uint8_t check = 0;
        size_t i;

        CHECK(call && call->kind == 'W' && call->length == 18 && call->data[0] == 0x2B && call->data[1] == 0x01 && call->data[2] == 14);
        for (i = 0; call && i < 17; ++i) {
            check ^= call->data[i];
        }
        CHECK(call && call->data[17] == check);
    }
    /* Anything that is not one whole command goes nowhere */
    Output(h, "\x42\x04\x00", 3);
    Output(h, "\x44\x00", 2);
    Output(h, "\xFF\x4F", 2);
    Output(h, "\x01\x00", 2);
    CHECK(H_NextCall(h) == NULL);
    H_Destroy(h);
}

/* 7 */
static void TestPortLoss(void)
{
    Harness *h = PresentBoeder();
    uint8_t frame[32];

    H_Feed(h, frame, Frame(frame, 0x03, (const uint8_t *)"\x80\x07\x00\x00\x00\x01\x20", 7));
    H_Feed(h, frame, Frame(frame, 0x02, (const uint8_t *)"\x02\x00", 2));
    CHECK(H_Axis(h, 0, 0) == 32767 && H_Button(h, 0, 8));
    H_Feed(h, (const uint8_t *)"\x2B\x03\x07\x80", 4);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', h->now));
    H_Advance(h, h->now + SDL_SERIAL_RETRY_MS);
    CHECK(H_ExpectOpened(h, 38400, 8, SDL_SERIAL_NOPARITY, 1, h->now));
    CHECK(H_IsWrite(H_NextCall(h), query_o, sizeof(query_o), h->now));
    CHECK(State(h)->model == NULL && !State(h)->queries.open);
    BringUpBoeder(h);
    CHECK(h->presence[0] == 2);
    CHECK(H_Axis(h, 0, 0) == 0 && H_Axis(h, 0, 1) == -32768 && H_NoButtons(h, 0));
    H_Destroy(h);
}

int main(void)
{
    TestQueries();
    TestReplies();
    TestInput();
    TestFraming();
    TestChecksum();
    TestOutput();
    TestPortLoss();
    return H_Finish();
}
