/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/SDL_iforce_proto.c, the I-Force wheels and
   joysticks of hifihedgehog/SDL#33 Part 8. Test numbers follow the part's
   USB section. The packets are constructed from Linux's layouts, and test 7
   transcribes captures of the 06F8:0004 wheel. */

#include "../src/joystick/SDL_iforce_proto.h"

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

#define HAT_UP    SDL_IFORCE_HAT_UP
#define HAT_RIGHT SDL_IFORCE_HAT_RIGHT
#define HAT_DOWN  SDL_IFORCE_HAT_DOWN
#define HAT_LEFT  SDL_IFORCE_HAT_LEFT

static const SDL_IForceModel *Model(uint16_t vendor, uint16_t product)
{
    const SDL_IForceModel *model = SDL_IForce_FindModel(vendor, product, true);

    CHECK(model != NULL);
    return model;
}

/* Decodes a whole packet, type byte first, from an exact-size heap copy so a
   read past the length reaches the sanitizer */
static int Decode(const SDL_IForceModel *model, const uint8_t *packet, size_t length, SDL_IForceState *state,
                  SDL_IForceStatus *status)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    int kind;

    if (length) {
        memcpy(copy, packet, length);
    }
    kind = length ? SDL_IForce_DecodePacket(model, copy[0], copy + 1, length - 1, state, status)
                  : SDL_IFORCE_PACKET_IGNORED;
    free(copy);
    return kind;
}

/* Decodes with 64 bytes of FF after the length, which must change nothing */
static int DecodePadded(const SDL_IForceModel *model, const uint8_t *packet, size_t length, SDL_IForceState *state)
{
    uint8_t buffer[80];

    memset(buffer, 0xFF, sizeof(buffer));
    memcpy(buffer, packet, length);
    return length ? SDL_IForce_DecodePacket(model, buffer[0], buffer + 1, length - 1, state, NULL)
                  : SDL_IFORCE_PACKET_IGNORED;
}

static bool StateEqual(const SDL_IForceState *a, const SDL_IForceState *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* The identities of every model */
static void TestModels(void)
{
    static const struct
    {
        uint16_t vendor, product;
        int naxes, nbuttons, nhats;
        bool wheel;
    } expected[] = {
        { 0x044F, 0xA01C, 3, 9, 1, true },
        { 0x046D, 0xC281, 3, 13, 1, false },
        { 0x046D, 0xC291, 3, 9, 1, true },
        { 0x05EF, 0x020A, 4, 10, 1, false },
        { 0x05EF, 0x8884, 3, 9, 1, true },
        { 0x05EF, 0x8888, 3, 9, 1, true },
        { 0x061C, 0xC084, 3, 9, 1, true },
        { 0x061C, 0xC094, 3, 9, 1, true },
        { 0x061C, 0xC0A4, 3, 9, 1, true },
        { 0x06A3, 0xFF04, 3, 9, 1, true },
        { 0x06F8, 0x0001, 3, 9, 1, true },
        { 0x06F8, 0x0003, 4, 13, 1, false },
        { 0x06F8, 0x0004, 3, 7, 2, true },
        { 0x06F8, 0xA302, 3, 13, 1, false },
    };
    size_t i;
    SDL_IForceIdentity identity;

    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        const SDL_IForceModel *model = Model(expected[i].vendor, expected[i].product);

        if (!model) {
            continue;
        }
        SDL_IForce_GetIdentity(model, &identity);
        CHECK(model->usb);
        CHECK(identity.naxes == expected[i].naxes && identity.nbuttons == expected[i].nbuttons);
        CHECK(identity.nhats == expected[i].nhats && identity.wheel == expected[i].wheel);
    }
    CHECK(strcmp(Model(0x06F8, 0x0003)->name, "Guillemot Jet Leader Force Feedback") == 0);
    CHECK(strcmp(Model(0x061C, 0xC094)->name, "ACT LABS Force RS") == 0);

    /* The serial wheels are not USB IDs */
    CHECK(SDL_IForce_FindModel(0x05EF, 0x8886, true) == NULL);
    CHECK(SDL_IForce_FindModel(0x06D6, 0x29BC, true) == NULL);
    CHECK(strcmp(SDL_IForce_FindModel(0x05EF, 0x8886, false)->name, "Boeder Force Feedback Wheel") == 0);
    CHECK(strcmp(SDL_IForce_FindModel(0x06D6, 0x29BC, false)->name, "Trust Force Feedback Race Master") == 0);
    CHECK(SDL_IForce_FindModel(0x046D, 0xC20E, false) == NULL);
    CHECK(SDL_IForce_FindModel(0x06A3, 0xCF17, false) == NULL);

    /* Any other device gets the joystick table without rudder */
    SDL_IForce_GetIdentity(SDL_IForce_UnknownModel(), &identity);
    CHECK(identity.naxes == 3 && identity.nbuttons == 13 && identity.nhats == 1 && !identity.wheel);
}

/* 1 */
static void TestQuerySequence(void)
{
    static const char letters[] = "OMPBNCEOV";
    SDL_IForceQueries queries;
    SDL_IForceControlSetup setup;
    uint8_t reply[SDL_IFORCE_MAX_LENGTH];
    int i, opens;

    /* Byte-exact setup packets, in order */
    SDL_IForce_InitQueries(&queries);
    for (i = 0; i < 9; ++i) {
        const uint8_t letter = SDL_IForce_NextQuery(&queries);

        CHECK(letter == (uint8_t)letters[i]);
        SDL_IForce_QuerySetup(letter, &setup);
        CHECK(setup.request_type == 0xC1 && setup.request == letter);
        CHECK(setup.value == 0x0000 && setup.index == 0x0000 && setup.length == 0x0010);
        reply[0] = letter;
        SDL_IForce_QueryResult(&queries, reply, 1);
    }
    CHECK(queries.done && queries.open && SDL_IForce_NextQuery(&queries) == 0);

    /* 19 failures, then an answer: 20 O requests before M */
    SDL_IForce_InitQueries(&queries);
    opens = 0;
    for (i = 0; i < 19; ++i) {
        CHECK(SDL_IForce_NextQuery(&queries) == 'O');
        ++opens;
        SDL_IForce_QueryResult(&queries, NULL, 0);
    }
    CHECK(SDL_IForce_NextQuery(&queries) == 'O');
    ++opens;
    reply[0] = 'O';
    SDL_IForce_QueryResult(&queries, reply, 1);
    CHECK(opens == 20 && queries.open_tries == 20 && queries.open);
    CHECK(SDL_IForce_NextQuery(&queries) == 'M');

    /* Twenty failures: no further query, and no effects for force feedback */
    SDL_IForce_InitQueries(&queries);
    for (i = 0; i < 20; ++i) {
        CHECK(SDL_IForce_NextQuery(&queries) == 'O');
        SDL_IForce_QueryResult(&queries, NULL, 0);
    }
    CHECK(queries.done && !queries.open && SDL_IForce_NextQuery(&queries) == 0 && queries.effects == 0);
    SDL_IForce_QueryResult(&queries, reply, 1);
    CHECK(queries.open_tries == 20 && SDL_IForce_NextQuery(&queries) == 0);

    /* A reply for another letter is a failure */
    SDL_IForce_InitQueries(&queries);
    reply[0] = 'M';
    SDL_IForce_QueryResult(&queries, reply, 1);
    CHECK(!queries.open && queries.open_tries == 1 && SDL_IForce_NextQuery(&queries) == 'O');

    /* So is an empty reply, whatever its buffer holds */
    SDL_IForce_InitQueries(&queries);
    reply[0] = 'O';
    SDL_IForce_QueryResult(&queries, reply, 0);
    CHECK(!queries.open && queries.open_tries == 1 && SDL_IForce_NextQuery(&queries) == 'O');
}

static void Answer(SDL_IForceQueries *queries, const uint8_t *reply, size_t length)
{
    SDL_IForce_QueryResult(queries, reply, length);
}

/* 2 */
static void TestReplies(void)
{
    static const uint8_t open[] = { 'O' };
    SDL_IForceQueries queries;

    /* The WingMan Force capture */
    SDL_IForce_InitQueries(&queries);
    CHECK(queries.memory_end == 200 && queries.effects == 0);
    Answer(&queries, open, 1);
    Answer(&queries, (const uint8_t *)"\x4D\x6D\x04", 3);
    Answer(&queries, (const uint8_t *)"\x50\x81\xC2", 3);
    Answer(&queries, (const uint8_t *)"\x42\xC8\x00", 3);
    Answer(&queries, (const uint8_t *)"\x4E\x0A", 2);
    CHECK(queries.vendor_id == 0x046D && queries.product_id == 0xC281);
    CHECK(queries.memory_end == 200 && queries.effects == 10);

    /* 1000 bytes, and 40 effects clamp to 32 */
    SDL_IForce_InitQueries(&queries);
    Answer(&queries, open, 1);
    Answer(&queries, open, 1); /* M answered with O: rejected */
    Answer(&queries, (const uint8_t *)"\x50", 1);
    Answer(&queries, (const uint8_t *)"\x42\xE8\x03", 3);
    Answer(&queries, (const uint8_t *)"\x4E\x28", 2);
    CHECK(queries.vendor_id == 0 && queries.product_id == 0);
    CHECK(queries.memory_end == 1000 && queries.effects == 32);

    /* Each reply one byte short, with the missing byte in the buffer past the
       length: M, P and B keep their defaults, and N gives no effects */
    SDL_IForce_InitQueries(&queries);
    Answer(&queries, open, 1);
    Answer(&queries, (const uint8_t *)"\x4D\x6D\x04", 2);
    Answer(&queries, (const uint8_t *)"\x50\x81\xC2", 2);
    Answer(&queries, (const uint8_t *)"\x42\xE8\x03", 2);
    Answer(&queries, (const uint8_t *)"\x4E\x0A", 1);
    CHECK(queries.vendor_id == 0 && queries.product_id == 0 && queries.memory_end == 200 && queries.effects == 0);

    /* Replies with the wrong letter change nothing */
    SDL_IForce_InitQueries(&queries);
    Answer(&queries, open, 1);
    Answer(&queries, (const uint8_t *)"\x50\x6D\x04", 3);
    Answer(&queries, (const uint8_t *)"\x4D\x81\xC2", 3);
    Answer(&queries, (const uint8_t *)"\x4E\xE8\x03", 3);
    Answer(&queries, (const uint8_t *)"\x42\x0A", 2);
    CHECK(queries.vendor_id == 0 && queries.product_id == 0 && queries.memory_end == 200 && queries.effects == 0);
    /* C, E, O and V are asked whatever they answer, then the sequence ends */
    CHECK(SDL_IForce_NextQuery(&queries) == 'C');
    Answer(&queries, NULL, 0);
    CHECK(SDL_IForce_NextQuery(&queries) == 'E');
    Answer(&queries, NULL, 0);
    CHECK(SDL_IForce_NextQuery(&queries) == 'O');
    Answer(&queries, NULL, 0);
    CHECK(SDL_IForce_NextQuery(&queries) == 'V');
    Answer(&queries, (const uint8_t *)"\x56\x01\x02\x03", 4);
    CHECK(queries.done && SDL_IForce_NextQuery(&queries) == 0);
}

static const uint8_t wheel_neutral[] = { 0x03, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xF0 };
static const uint8_t joystick_neutral[] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xF0 };

/* 3 and 4 */
static void TestWheel(void)
{
    const SDL_IForceModel *model = Model(0x046D, 0xC291);
    SDL_IForceState state;
    uint8_t packet[8];

    SDL_IForce_ResetState(model, &state);
    CHECK(Decode(model, wheel_neutral, sizeof(wheel_neutral), &state, NULL) == SDL_IFORCE_PACKET_INPUT);
    CHECK(state.axes[0] == 0 && state.axes[1] == -32768 && state.axes[2] == -32768);
    CHECK(state.buttons == 0 && state.hats[0] == 0);

    memcpy(packet, wheel_neutral, sizeof(packet));
    packet[1] = 0x80;
    packet[2] = 0x07;
    Decode(model, packet, sizeof(packet), &state, NULL);
    CHECK(state.axes[0] == 32767);
    packet[2] = 0xF8;
    Decode(model, packet, sizeof(packet), &state, NULL);
    CHECK(state.axes[0] == -32767);
    packet[1] = 0xFF;
    packet[2] = 0x7F;
    Decode(model, packet, sizeof(packet), &state, NULL);
    CHECK(state.axes[0] == 32767);
    packet[1] = 0x00;
    packet[2] = 0x80;
    Decode(model, packet, sizeof(packet), &state, NULL);
    CHECK(state.axes[0] == -32768);
    packet[1] = 0xC0;
    packet[2] = 0x03;
    Decode(model, packet, sizeof(packet), &state, NULL);
    CHECK(state.axes[0] == 16383);
    packet[1] = 0xFF;
    packet[2] = 0xFF;
    Decode(model, packet, sizeof(packet), &state, NULL);
    CHECK(state.axes[0] == -17);

    /* Gas and brake: Linux's 255 minus the byte, then 8 bits to 16 */
    memcpy(packet, wheel_neutral, sizeof(packet));
    packet[3] = 0x00;
    Decode(model, packet, sizeof(packet), &state, NULL);
    CHECK(state.axes[1] == 32767 && state.axes[2] == -32768);
    packet[3] = 0xFF;
    packet[4] = 0x80;
    Decode(model, packet, sizeof(packet), &state, NULL);
    CHECK(state.axes[1] == -32768 && state.axes[2] == -129);
    /* Data 4 is unused */
    packet[4] = 0xFF;
    packet[5] = 0x5A;
    Decode(model, packet, sizeof(packet), &state, NULL);
    CHECK(state.axes[0] == 0 && state.axes[1] == -32768 && state.axes[2] == -32768 && state.buttons == 0);
}

/* 5 and 6 */
static void TestButtonsAndHat(void)
{
    static const uint8_t hats[16] = {
        HAT_UP, HAT_UP | HAT_RIGHT, HAT_RIGHT, HAT_DOWN | HAT_RIGHT, HAT_DOWN, HAT_DOWN | HAT_LEFT, HAT_LEFT, HAT_UP | HAT_LEFT,
        0, 0, 0, 0, 0, 0, 0, 0
    };
    const SDL_IForceModel *model = Model(0x046D, 0xC291);
    SDL_IForceState state;
    uint8_t packet[8];
    int bit, nibble, low;

    SDL_IForce_ResetState(model, &state);
    memcpy(packet, wheel_neutral, sizeof(packet));
    for (bit = 0; bit < 8; ++bit) {
        packet[6] = (uint8_t)(1 << bit);
        Decode(model, packet, sizeof(packet), &state, NULL);
        CHECK(state.buttons == (1u << bit) && state.hats[0] == 0);
    }
    packet[6] = 0x00;
    for (low = 0; low < 16; low += 5) {
        for (nibble = 0; nibble < 16; ++nibble) {
            packet[7] = (uint8_t)((nibble << 4) | low);
            Decode(model, packet, sizeof(packet), &state, NULL);
            /* The wheel table has 8 entries and no second hat: the low
               nibble is nothing */
            CHECK(state.hats[0] == hats[nibble] && state.buttons == 0);
        }
    }
}

/* 7: the right hat of 06F8:0004, from the wheel's captures */
static void TestGuillemotWheel(void)
{
    const SDL_IForceModel *model = Model(0x06F8, 0x0004);
    SDL_IForceState state;
    uint8_t packet[8];
    int bit;

    SDL_IForce_ResetState(model, &state);
    Decode(model, (const uint8_t *)"\x03\x00\x00\xFF\xFF\x00\x00\xF2", 8, &state, NULL);
    CHECK(state.hats[1] == HAT_LEFT && state.hats[0] == 0 && state.buttons == 0);
    Decode(model, (const uint8_t *)"\x03\x00\x00\xFF\xFF\x00\x00\xF1", 8, &state, NULL);
    CHECK(state.hats[1] == HAT_DOWN && state.hats[0] == 0 && state.buttons == 0);
    Decode(model, (const uint8_t *)"\x03\x00\x00\xFF\xFF\x00\x40\xF0", 8, &state, NULL);
    CHECK(state.hats[1] == HAT_UP && state.hats[0] == 0 && state.buttons == 0);
    Decode(model, (const uint8_t *)"\x03\x00\x00\xFF\xFF\x00\x80\xF0", 8, &state, NULL);
    CHECK(state.hats[1] == HAT_RIGHT && state.hats[0] == 0 && state.buttons == 0);
    Decode(model, (const uint8_t *)"\x03\x00\x00\xFF\xFF\x00\xC0\xF3", 8, &state, NULL);
    CHECK(state.hats[1] == (HAT_UP | HAT_RIGHT | HAT_DOWN | HAT_LEFT) && state.buttons == 0);

    /* Buttons 0 to 5 are data 5 bits 0 to 5, and hat 0 is the left hat */
    memcpy(packet, wheel_neutral, sizeof(packet));
    for (bit = 0; bit < 6; ++bit) {
        packet[6] = (uint8_t)(1 << bit);
        Decode(model, packet, sizeof(packet), &state, NULL);
        CHECK(state.buttons == (1u << bit) && state.hats[1] == 0);
    }
    packet[6] = 0x00;
    packet[7] = 0x6C;
    Decode(model, packet, sizeof(packet), &state, NULL);
    CHECK(state.hats[0] == HAT_LEFT && state.hats[1] == 0 && state.buttons == 0);
    /* Wheel and pedals as on the other wheels */
    Decode(model, (const uint8_t *)"\x03\x80\x07\x00\xFF\x00\x00\xF0", 8, &state, NULL);
    CHECK(state.axes[0] == 32767 && state.axes[1] == 32767 && state.axes[2] == -32768);
}

/* 8 and 9 */
static void TestJoystick(void)
{
    const SDL_IForceModel *wingman = Model(0x046D, 0xC281);
    const SDL_IForceModel *jet = Model(0x06F8, 0x0003);
    const SDL_IForceModel *pegasus = Model(0x05EF, 0x020A);
    SDL_IForceState state;
    uint8_t packet[9];
    int bit;

    SDL_IForce_ResetState(wingman, &state);
    CHECK(Decode(wingman, joystick_neutral, sizeof(joystick_neutral), &state, NULL) == SDL_IFORCE_PACKET_INPUT);
    CHECK(state.axes[0] == 0 && state.axes[1] == 0 && state.axes[2] == -32768 && state.buttons == 0 && state.hats[0] == 0);
    Decode(wingman, (const uint8_t *)"\x01\x80\x07\x80\xF8\x00\xFF\x00\xF0", 9, &state, NULL);
    CHECK(state.axes[0] == 32767 && state.axes[1] == -32767 && state.axes[2] == 32767 && state.axes[3] == 0);

    /* The rudder model */
    SDL_IForce_ResetState(jet, &state);
    Decode(jet, (const uint8_t *)"\x01\x00\x00\x00\x00\xFF\x00\xF0\x80", 9, &state, NULL);
    CHECK(state.axes[3] == -32768);
    Decode(jet, (const uint8_t *)"\x01\x00\x00\x00\x00\xFF\x00\xF0\x7F", 9, &state, NULL);
    CHECK(state.axes[3] == 32767);
    /* A 7-data-byte packet leaves the rudder */
    Decode(jet, (const uint8_t *)"\x01\x00\x00\x00\x00\x00\x01\x70", 8, &state, NULL);
    CHECK(state.axes[3] == 32767 && state.axes[2] == 32767 && state.buttons == 1 && state.hats[0] == (HAT_UP | HAT_LEFT));
    Decode(jet, (const uint8_t *)"\x01\x00\x00\x00\x00\xFF\x00\xF0\x00", 9, &state, NULL);
    CHECK(state.axes[3] == 128);

    /* 9: data 6 bits 0 to 3 are buttons 8 to 11. Bit 4 is the hat's. */
    SDL_IForce_ResetState(wingman, &state);
    memcpy(packet, joystick_neutral, 8);
    for (bit = 0; bit < 4; ++bit) {
        packet[7] = (uint8_t)(0xF0 | (1 << bit));
        Decode(wingman, packet, 8, &state, NULL);
        CHECK(state.buttons == (1u << (8 + bit)) && state.hats[0] == 0);
    }
    packet[7] = 0xE0;
    Decode(wingman, packet, 8, &state, NULL);
    CHECK(state.buttons == 0 && state.hats[0] == 0);
    packet[7] = 0x10;
    Decode(wingman, packet, 8, &state, NULL);
    CHECK(state.buttons == 0 && state.hats[0] == (HAT_UP | HAT_RIGHT));

    /* The Pegasus: buttons 0 to 8, data 6 bit 0 the ninth, no second hat */
    SDL_IForce_ResetState(pegasus, &state);
    memcpy(packet, joystick_neutral, 8);
    packet[8] = 0x00;
    packet[7] = 0xF1;
    Decode(pegasus, packet, 9, &state, NULL);
    CHECK(state.buttons == (1u << 8) && state.hats[0] == 0 && state.hats[1] == 0 && state.axes[3] == 128);
    packet[7] = 0xFE;
    Decode(pegasus, packet, 9, &state, NULL);
    CHECK(state.buttons == 0 && state.hats[1] == 0);
    packet[6] = 0xFF;
    packet[7] = 0xF0;
    Decode(pegasus, packet, 9, &state, NULL);
    CHECK(state.buttons == 0xFF);

    /* A wheel packet moves no joystick axis, and a joystick packet no wheel axis */
    SDL_IForce_ResetState(wingman, &state);
    Decode(wingman, (const uint8_t *)"\x03\x80\x07\x00\x00\x00\x04\x20", 8, &state, NULL);
    CHECK(state.axes[0] == 0 && state.axes[1] == 0 && state.axes[2] == -32768);
    CHECK(state.buttons == 4 && state.hats[0] == HAT_RIGHT);
    {
        const SDL_IForceModel *wheel = Model(0x046D, 0xC291);

        SDL_IForce_ResetState(wheel, &state);
        Decode(wheel, (const uint8_t *)"\x01\x80\x07\x80\x07\x00\x02\x40", 8, &state, NULL);
        CHECK(state.axes[0] == 0 && state.axes[1] == -32768 && state.axes[2] == -32768);
        CHECK(state.buttons == 2 && state.hats[0] == HAT_DOWN);
    }
}

/* 10: every prefix, from 0 bytes to one short, changes nothing */
static void TestTruncation(void)
{
    static const struct
    {
        uint16_t vendor, product;
        uint8_t bytes[9];
        size_t length;
    } packets[] = {
        { 0x046D, 0xC291, { 0x03, 0x80, 0x07, 0x00, 0x00, 0x00, 0xFF, 0x5F }, 8 },
        { 0x06F8, 0x0004, { 0x03, 0x00, 0x80, 0x10, 0x20, 0x00, 0xC0, 0x33 }, 8 },
        { 0x046D, 0xC281, { 0x01, 0x80, 0x07, 0x80, 0xF8, 0x00, 0xFF, 0x2F }, 8 },
        { 0x06F8, 0x0003, { 0x01, 0x80, 0x07, 0x80, 0xF8, 0x00, 0xFF, 0x2F, 0x7F }, 9 },
        { 0x05EF, 0x020A, { 0x01, 0x80, 0x07, 0x80, 0xF8, 0x00, 0xFF, 0x2F, 0x80 }, 9 },
        { 0x046D, 0xC281, { 0x02, 0x02, 0x81, 0x00, 0x10, 0x00 }, 6 },
    };
    size_t i, k;

    for (i = 0; i < sizeof(packets) / sizeof(packets[0]); ++i) {
        const SDL_IForceModel *model = Model(packets[i].vendor, packets[i].product);
        const size_t minimum = (packets[i].bytes[0] == SDL_IFORCE_PACKET_STATUS) ? 3 : 8;
        SDL_IForceState before, state;

        SDL_IForce_ResetState(model, &before);
        for (k = 0; k < packets[i].length; ++k) {
            state = before;
            Decode(model, packets[i].bytes, k, &state, NULL);
            if (k < minimum) {
                CHECK(StateEqual(&state, &before));
            }
            state = before;
            DecodePadded(model, packets[i].bytes, k, &state);
            if (k < minimum) {
                CHECK(StateEqual(&state, &before));
            }
        }
        /* The whole packet does change the state */
        state = before;
        Decode(model, packets[i].bytes, packets[i].length, &state, NULL);
        CHECK(!StateEqual(&state, &before));
    }
}

/* 11 and 12 */
static void TestStatusAndOthers(void)
{
    const SDL_IForceModel *wingman = Model(0x046D, 0xC281);
    const SDL_IForceModel *wheel = Model(0x046D, 0xC291);
    SDL_IForceState state, before;
    SDL_IForceStatus status;
    static const uint8_t types[] = { 0x00, 0x04, 0x05, 0x7F, 0xFF };
    size_t i;

    /* The grip, from the WingMan Force capture: 01 off, 03 on */
    SDL_IForce_ResetState(wingman, &state);
    Decode(wingman, (const uint8_t *)"\x01\x80\x07\x00\x00\x80\x00\x20", 8, &state, NULL);
    before = state;
    CHECK(Decode(wingman, (const uint8_t *)"\x02\x03\x00", 3, &state, &status) == SDL_IFORCE_PACKET_STATUS_REPORT);
    CHECK(status.deadman && state.buttons == (1u << 12) && state.axes[0] == before.axes[0] && state.axes[2] == before.axes[2]);
    CHECK(state.hats[0] == before.hats[0]);
    CHECK(Decode(wingman, (const uint8_t *)"\x02\x01\x00", 3, &state, &status) == SDL_IFORCE_PACKET_STATUS_REPORT);
    CHECK(!status.deadman && StateEqual(&state, &before));

    /* An input packet keeps the grip button */
    Decode(wingman, (const uint8_t *)"\x02\x02\x00", 3, &state, &status);
    Decode(wingman, joystick_neutral, sizeof(joystick_neutral), &state, NULL);
    CHECK(state.buttons == (1u << 12));

    /* Effect 1 playing, the block at 0010 stored */
    CHECK(Decode(wingman, (const uint8_t *)"\x02\x00\x81\x00\x10\x00", 6, &state, &status) == SDL_IFORCE_PACKET_STATUS_REPORT);
    CHECK(status.effect == 1 && status.playing && status.addresses == 1 && status.address[0] == 0x0010);
    CHECK(Decode(wingman, (const uint8_t *)"\x02\x00\x05\x00\x10\x00\x2A\x00\x33", 9, &state, &status) == SDL_IFORCE_PACKET_STATUS_REPORT);
    CHECK(status.effect == 5 && !status.playing && status.addresses == 2 && status.address[1] == 0x002A);
    /* A status packet of one data byte is too short */
    before = state;
    CHECK(Decode(wingman, (const uint8_t *)"\x02\x02", 2, &state, &status) == SDL_IFORCE_PACKET_IGNORED);
    CHECK(StateEqual(&state, &before));
    /* A full status packet's addresses stop at six */
    {
        uint8_t packet[1 + 20];

        memset(packet, 0x11, sizeof(packet));
        packet[0] = 0x02;
        CHECK(Decode(wingman, packet, sizeof(packet), &state, &status) == SDL_IFORCE_PACKET_STATUS_REPORT);
        CHECK(status.addresses == SDL_IFORCE_MAX_ADDRESSES && status.address[5] == 0x1111);
    }

    /* The wheel's grip is its ninth button */
    SDL_IForce_ResetState(wheel, &state);
    Decode(wheel, (const uint8_t *)"\x02\x02\x00", 3, &state, NULL);
    CHECK(state.buttons == (1u << 8));

    /* 12: other types change nothing */
    SDL_IForce_ResetState(wingman, &state);
    before = state;
    for (i = 0; i < sizeof(types); ++i) {
        uint8_t packet[9];

        memcpy(packet, joystick_neutral, 8);
        packet[0] = types[i];
        packet[1] = 0x80;
        packet[2] = 0x07;
        packet[6] = 0xFF;
        packet[8] = 0x02;
        CHECK(Decode(wingman, packet, sizeof(packet), &state, &status) == SDL_IFORCE_PACKET_IGNORED);
        CHECK(StateEqual(&state, &before));
    }
    CHECK(SDL_IForce_DecodePacket(NULL, 0x01, joystick_neutral + 1, 7, &state, NULL) == SDL_IFORCE_PACKET_IGNORED);
    CHECK(SDL_IForce_DecodePacket(wingman, 0x01, NULL, 7, &state, NULL) == SDL_IFORCE_PACKET_IGNORED);
}

/* 13: reset gives the neutral state and a new query sequence */
static void TestReset(void)
{
    const SDL_IForceModel *wheel = Model(0x046D, 0xC291);
    const SDL_IForceModel *wingman = Model(0x046D, 0xC281);
    SDL_IForceState state, fresh;
    SDL_IForceQueries queries;
    static const uint8_t open[] = { 'O' };

    SDL_IForce_ResetState(wheel, &fresh);
    CHECK(fresh.axes[0] == 0 && fresh.axes[1] == -32768 && fresh.axes[2] == -32768 && fresh.axes[3] == 0);
    CHECK(fresh.buttons == 0 && fresh.hats[0] == 0 && fresh.hats[1] == 0);
    state = fresh;
    Decode(wheel, (const uint8_t *)"\x03\x80\x07\x00\x00\x00\xFF\x5F", 8, &state, NULL);
    Decode(wheel, (const uint8_t *)"\x02\x02\x00", 3, &state, NULL);
    CHECK(!StateEqual(&state, &fresh));
    SDL_IForce_ResetState(wheel, &state);
    CHECK(StateEqual(&state, &fresh));
    SDL_IForce_ResetState(wingman, &state);
    CHECK(state.axes[0] == 0 && state.axes[1] == 0 && state.axes[2] == -32768 && state.buttons == 0);

    SDL_IForce_InitQueries(&queries);
    Answer(&queries, open, 1);
    Answer(&queries, (const uint8_t *)"\x4D\x6D\x04", 3);
    SDL_IForce_InitQueries(&queries);
    CHECK(SDL_IForce_NextQuery(&queries) == 'O' && queries.vendor_id == 0 && queries.open_tries == 0 && !queries.open);
}

static bool CommandIs(const SDL_IForceCommand *command, const char *bytes, size_t length)
{
    return command->length == length && memcmp(command->bytes, bytes, length) == 0;
}

/* 14: the control commands */
static void TestCommands(void)
{
    SDL_IForceCommand command, spring[2];

    SDL_IForce_BuildGain(&command, 0xFFFF);
    CHECK(CommandIs(&command, "\x43\x7F", 2));
    SDL_IForce_BuildGain(&command, (uint16_t)(0xFFFF * 50 / 100));
    CHECK(CommandIs(&command, "\x43\x3F", 2));
    SDL_IForce_BuildGain(&command, 0);
    CHECK(CommandIs(&command, "\x43\x00", 2));
    SDL_IForce_BuildState(&command, SDL_IFORCE_STATE_ENABLE);
    CHECK(CommandIs(&command, "\x42\x04", 2));
    SDL_IForce_BuildState(&command, SDL_IFORCE_STATE_STOP_ALL);
    CHECK(CommandIs(&command, "\x42\x01", 2));
    SDL_IForce_BuildPlay(&command, 2, 1);
    CHECK(CommandIs(&command, "\x41\x02\x01\x01", 4));
    SDL_IForce_BuildPlay(&command, 2, 0);
    CHECK(CommandIs(&command, "\x41\x02\x00\x00", 4));
    SDL_IForce_BuildPlay(&command, 7, 5);
    CHECK(CommandIs(&command, "\x41\x07\x41\x05", 4));
    SDL_IForce_BuildPlay(&command, 7, 255);
    CHECK(CommandIs(&command, "\x41\x07\x41\xFF", 4));
    SDL_IForce_BuildPlay(&command, 7, 256);
    CHECK(CommandIs(&command, "\x41\x07\x41\xFF", 4));
    SDL_IForce_BuildPlay(&command, 7, 0xFFFFFFFFu);
    CHECK(CommandIs(&command, "\x41\x07\x41\xFF", 4));
    SDL_IForce_BuildAutocenter(spring, 0);
    CHECK(CommandIs(&spring[0], "\x40\x03\x00", 3) && CommandIs(&spring[1], "\x40\x04\x01", 3));
    SDL_IForce_BuildAutocenter(spring, 0xFFFF);
    CHECK(CommandIs(&spring[0], "\x40\x03\x7F", 3) && CommandIs(&spring[1], "\x40\x04\x01", 3));

    /* Whole commands only */
    CHECK(SDL_IForce_IsCommand((const uint8_t *)"\x42\x04", 2));
    CHECK(!SDL_IForce_IsCommand((const uint8_t *)"\x42\x04\x00", 3));
    CHECK(SDL_IForce_IsCommand((const uint8_t *)"\x40\x03\x00", 3));
    CHECK(SDL_IForce_IsCommand((const uint8_t *)"\x40\x00\x00\x04", 4));
    CHECK(!SDL_IForce_IsCommand((const uint8_t *)"\x40\x03", 2));
    CHECK(SDL_IForce_IsCommand((const uint8_t *)"\x41\x02\x01\x01", 4));
    CHECK(SDL_IForce_IsCommand((const uint8_t *)"\x43\x7F", 2));
    CHECK(SDL_IForce_IsCommand((const uint8_t *)"\x03\x00\x00\x40", 4));
    CHECK(!SDL_IForce_IsCommand((const uint8_t *)"\x03\x00\x00", 3));
    CHECK(!SDL_IForce_IsCommand((const uint8_t *)"\xFF\x4F", 2));
    CHECK(!SDL_IForce_IsCommand((const uint8_t *)"\x44\x00", 2));
    CHECK(!SDL_IForce_IsCommand((const uint8_t *)"\x42", 1));
    CHECK(!SDL_IForce_IsCommand(NULL, 2));
    {
        uint8_t core[15], block[11], period[8], envelope[9];

        memset(core, 0, sizeof(core));
        memset(block, 0, sizeof(block));
        memset(period, 0, sizeof(period));
        memset(envelope, 0, sizeof(envelope));
        core[0] = 0x01;
        block[0] = 0x05;
        period[0] = 0x04;
        envelope[0] = 0x02;
        CHECK(SDL_IForce_IsCommand(core, 15) && !SDL_IForce_IsCommand(core, 14));
        CHECK(SDL_IForce_IsCommand(block, 11) && !SDL_IForce_IsCommand(block, 10));
        CHECK(SDL_IForce_IsCommand(period, 8) && !SDL_IForce_IsCommand(period, 9));
        CHECK(SDL_IForce_IsCommand(envelope, 9) && !SDL_IForce_IsCommand(envelope, 8));
    }
}

static SDL_IForceEffect Constant(void)
{
    SDL_IForceEffect effect;

    memset(&effect, 0, sizeof(effect));
    effect.type = SDL_IFORCE_EFFECT_CONSTANT;
    effect.level = 0x4000;
    effect.envelope.attack_length = 100;
    effect.envelope.attack_level = 0x2000;
    effect.envelope.fade_length = 200;
    effect.envelope.fade_level = 0x1000;
    effect.length = 1000;
    effect.delay = 5;
    effect.direction = 0x4000;
    effect.trigger_button = 2;
    effect.trigger_interval = 50;
    return effect;
}

static SDL_IForceEffect Sine(void)
{
    SDL_IForceEffect effect;

    memset(&effect, 0, sizeof(effect));
    effect.type = SDL_IFORCE_EFFECT_PERIODIC;
    effect.waveform = SDL_IFORCE_WAVE_SINE;
    effect.magnitude = -12000;
    effect.offset = 3000;
    effect.phase = 0x4000;
    effect.period = 250;
    effect.direction = 0xC000;
    return effect;
}

static SDL_IForceEffect Spring(void)
{
    SDL_IForceEffect effect;

    memset(&effect, 0, sizeof(effect));
    effect.type = SDL_IFORCE_EFFECT_SPRING;
    effect.condition[0].right_saturation = 0xFFFF;
    effect.condition[0].left_saturation = 0x8000;
    effect.condition[0].right_coeff = 32767;
    effect.condition[0].left_coeff = -32768;
    effect.condition[0].deadband = 0xFFFF;
    effect.condition[0].center = -32768;
    effect.condition[1].right_coeff = -1;
    effect.condition[1].left_coeff = 1;
    effect.condition[1].center = 32767;
    return effect;
}

/* Effect uploads, their blocks and the encodings, in the byte forms of
   Linux's iforce-ff.c */
static void TestEffects(void)
{
    SDL_IForceFF ff;
    SDL_IForceCommands out;
    SDL_IForceEffect constant = Constant(), sine = Sine(), spring = Spring();

    SDL_IForce_InitFF(&ff, 10, 200);
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 0, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 3);
    CHECK(CommandIs(&out.command[0], "\x03\x00\x00\x40", 4));
    CHECK(CommandIs(&out.command[1], "\x02\x02\x00\x64\x00\x20\xC8\x00\x10", 9));
    CHECK(CommandIs(&out.command[2], "\x01\x00\x00\x22\xE8\x03\x40\x32\x00\x00\x00\x02\x00\x05\x00", 15));

    CHECK(SDL_IForce_Upload(&ff, 1, &sine, 0, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 3);
    CHECK(CommandIs(&out.command[0], "\x04\x10\x00\xD2\x0B\x40\xFA\x00", 8));
    CHECK(CommandIs(&out.command[1], "\x02\x1C\x00\x00\x00\x00\x00\x00\x00", 9));
    CHECK(CommandIs(&out.command[2], "\x01\x01\x22\x20\x00\x00\xC0\x00\x00\x10\x00\x1C\x00\x00\x00", 15));

    CHECK(SDL_IForce_Upload(&ff, 2, &spring, 0, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 3);
    CHECK(CommandIs(&out.command[0], "\x05\x2A\x00\x63\x9C\x0C\xFE\xE7\x03\x63\x32", 11));
    CHECK(CommandIs(&out.command[1], "\x05\x32\x00\xFF\x00\xF3\x01\x00\x00\x00\x00", 11));
    CHECK(CommandIs(&out.command[2], "\x01\x02\x40\xC0\x00\x00\x00\x00\x00\x2A\x00\x32\x00\x00\x00", 15));

    /* A damper is the spring's layout with waveform 41 */
    spring.type = SDL_IFORCE_EFFECT_DAMPER;
    CHECK(SDL_IForce_Upload(&ff, 3, &spring, 0, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 3 && out.command[2].bytes[2] == 0x41 && out.command[2].bytes[3] == 0xC0);
    CHECK(out.command[0].bytes[1] == 58 && out.command[1].bytes[1] == 66);

    /* Every waveform byte */
    {
        static const uint8_t waves[] = { SDL_IFORCE_WAVE_SQUARE, SDL_IFORCE_WAVE_TRIANGLE, SDL_IFORCE_WAVE_SINE,
                                         SDL_IFORCE_WAVE_SAWTOOTH_UP, SDL_IFORCE_WAVE_SAWTOOTH_DOWN };
        size_t i;

        for (i = 0; i < sizeof(waves); ++i) {
            SDL_IForceEffect wave = Sine();

            wave.waveform = waves[i];
            SDL_IForce_Erase(&ff, 4);
            CHECK(SDL_IForce_Upload(&ff, 4, &wave, 0, &out) == SDL_IFORCE_UPLOAD_SENT);
            CHECK(out.count == 3 && out.command[2].bytes[2] == waves[i]);
        }
    }

    /* Levels at the ends, rounded toward zero as Linux's HIFIX80 does */
    {
        static const int16_t levels[] = { -32768, -32767, -257, -256, -1, 0, 255, 256, 32767 };
        static const uint8_t bytes[] = { 0x80, 0x81, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x7F };
        size_t i;

        for (i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i) {
            SDL_IForceEffect level = Constant();

            level.level = levels[i];
            SDL_IForce_Erase(&ff, 5);
            SDL_IForce_Upload(&ff, 5, &level, 0, &out);
            CHECK(out.command[0].bytes[3] == bytes[i]);
        }
    }

    /* The trigger fills the low nibble of the axes byte, and past 15 the
       upload is invalid */
    SDL_IForce_Erase(&ff, 5);
    constant.trigger_button = 15;
    CHECK(SDL_IForce_Upload(&ff, 5, &constant, 0, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 3 && out.command[2].bytes[3] == 0x2F);
    SDL_IForce_Erase(&ff, 5);
    constant.trigger_button = 16;
    CHECK(SDL_IForce_Upload(&ff, 5, &constant, 0, &out) == SDL_IFORCE_UPLOAD_INVALID);
    CHECK(!ff.slots[5].used);
    constant.trigger_button = 2;

    /* An update keeps the type, and a periodic effect its waveform */
    out.count = 9;
    CHECK(SDL_IForce_Upload(&ff, 0, &spring, 0, &out) == SDL_IFORCE_UPLOAD_INVALID && out.count == 0);
    CHECK(ff.slots[0].effect.type == SDL_IFORCE_EFFECT_CONSTANT);
    CHECK(SDL_IForce_Upload(&ff, 1, &constant, 0, &out) == SDL_IFORCE_UPLOAD_INVALID && out.count == 0);
    CHECK(ff.slots[1].effect.type == SDL_IFORCE_EFFECT_PERIODIC);

    /* Invalid uploads */
    CHECK(SDL_IForce_Upload(&ff, 10, &constant, 0, &out) == SDL_IFORCE_UPLOAD_INVALID);
    CHECK(SDL_IForce_Upload(&ff, -1, &constant, 0, &out) == SDL_IFORCE_UPLOAD_INVALID);
    CHECK(SDL_IForce_Upload(&ff, 0, &sine, 0, &out) == SDL_IFORCE_UPLOAD_INVALID);
    sine.waveform = 0x25;
    CHECK(SDL_IForce_Upload(&ff, 6, &sine, 0, &out) == SDL_IFORCE_UPLOAD_INVALID);
    sine.waveform = SDL_IFORCE_WAVE_SQUARE;
    CHECK(SDL_IForce_Upload(&ff, 1, &sine, 0, &out) == SDL_IFORCE_UPLOAD_INVALID);
    constant.type = 7;
    CHECK(SDL_IForce_Upload(&ff, 6, &constant, 0, &out) == SDL_IFORCE_UPLOAD_INVALID);
    CHECK(SDL_IForce_Upload(NULL, 0, &constant, 0, &out) == SDL_IFORCE_UPLOAD_INVALID);
}

/* Blocks between 0 and the B address, both included, at even addresses */
static void TestMemory(void)
{
    SDL_IForceFF ff;
    SDL_IForceCommands out;
    SDL_IForceEffect sine = Sine(), constant = Constant();
    int i;

    SDL_IForce_InitFF(&ff, 32, 200);
    for (i = 0; i < 7; ++i) {
        CHECK(SDL_IForce_Upload(&ff, i, &sine, 0, &out) == SDL_IFORCE_UPLOAD_SENT);
        CHECK(ff.slots[i].block[0].start == 26 * i && ff.slots[i].block[1].start == 26 * i + 12);
    }
    CHECK(SDL_IForce_Upload(&ff, 7, &sine, 0, &out) == SDL_IFORCE_UPLOAD_NO_MEMORY);
    CHECK(!ff.slots[7].used && !ff.slots[7].block[0].used && !ff.slots[7].block[1].used);
    /* 182 to 200 still hold a constant's 2 and 14 bytes */
    CHECK(SDL_IForce_Upload(&ff, 7, &constant, 0, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(ff.slots[7].block[0].start == 182 && ff.slots[7].block[1].start == 184);
    CHECK(SDL_IForce_Upload(&ff, 8, &constant, 0, &out) == SDL_IFORCE_UPLOAD_NO_MEMORY);

    /* An erased effect's space goes to the next, at the first fit */
    SDL_IForce_Erase(&ff, 2);
    CHECK(SDL_IForce_Upload(&ff, 8, &constant, 0, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(ff.slots[8].block[0].start == 52 && ff.slots[8].block[1].start == 54);
    /* The end address counts: a constant's 2 and 14 bytes fill 0 to 15 */
    SDL_IForce_InitFF(&ff, 2, 15);
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 0, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(ff.slots[0].block[1].start == 2);
    SDL_IForce_InitFF(&ff, 2, 14);
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 0, &out) == SDL_IFORCE_UPLOAD_NO_MEMORY);

    /* No effects, no uploads */
    SDL_IForce_InitFF(&ff, 0, 200);
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 0, &out) == SDL_IFORCE_UPLOAD_INVALID);
    SDL_IForce_InitFF(&ff, 40, 200);
    CHECK(ff.effects == 32);
    SDL_IForce_InitFF(&ff, -1, 200);
    CHECK(ff.effects == 0);
}

/* Updates send only what changed, and a block waits 20 ms between writes */
static void TestUpdates(void)
{
    SDL_IForceFF ff;
    SDL_IForceCommands out;
    SDL_IForceEffect constant = Constant(), sine = Sine(), spring = Spring();
    uint64_t deadline = 0;

    SDL_IForce_InitFF(&ff, 4, 200);
    SDL_IForce_Upload(&ff, 0, &constant, 1000, &out);
    SDL_IForce_Upload(&ff, 1, &sine, 1000, &out);
    SDL_IForce_Upload(&ff, 2, &spring, 1000, &out);

    /* Nothing changed */
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 1000, &out) == SDL_IFORCE_UPLOAD_UNCHANGED && out.count == 0);
    /* The core alone goes at once */
    constant.length = 2000;
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 1001, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 1 && out.command[0].bytes[0] == 0x01 && out.command[0].bytes[4] == 0xD0);
    /* A new level within 20 ms of the block's last write waits */
    constant.level = 0x7FFF;
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 1019, &out) == SDL_IFORCE_UPLOAD_DEFERRED && out.count == 0);
    CHECK(SDL_IForce_DeferredDeadline(&ff, &deadline) && deadline == 1020);
    CHECK(!SDL_IForce_NextDeferred(&ff, 1019, &out));
    /* A later update replaces it */
    constant.level = 0x3000;
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 1019, &out) == SDL_IFORCE_UPLOAD_DEFERRED);
    CHECK(SDL_IForce_NextDeferred(&ff, 1020, &out));
    CHECK(out.count == 1 && CommandIs(&out.command[0], "\x03\x00\x00\x30", 4));
    CHECK(!SDL_IForce_NextDeferred(&ff, 1020, &out) && !SDL_IForce_DeferredDeadline(&ff, &deadline));
    /* After 20 ms a write goes at once, with the core when it changed too */
    constant.level = 0x1000;
    constant.delay = 9;
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 1040, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 2 && out.command[0].bytes[0] == 0x03 && out.command[1].bytes[0] == 0x01 && out.command[1].bytes[13] == 9);

    /* The envelope block keeps its own clock */
    constant.envelope.fade_level = 0x7000;
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 1041, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 1 && CommandIs(&out.command[0], "\x02\x02\x00\x64\x00\x20\xC8\x00\x70", 9));
    /* A waiting block holds back the core with it */
    constant.level = 0x0100;
    constant.direction = 0x8000;
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 1050, &out) == SDL_IFORCE_UPLOAD_DEFERRED && out.count == 0);
    CHECK(SDL_IForce_NextDeferred(&ff, 1060, &out));
    CHECK(out.count == 2 && CommandIs(&out.command[0], "\x03\x00\x00\x01", 4) && out.command[1].bytes[6] == 0x80);

    /* Periodic: the period block and the envelope block */
    sine.period = 100;
    CHECK(SDL_IForce_Upload(&ff, 1, &sine, 1100, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 1 && out.command[0].bytes[0] == 0x04 && out.command[0].bytes[6] == 100);
    sine.envelope.attack_length = 7;
    CHECK(SDL_IForce_Upload(&ff, 1, &sine, 1100, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 1 && out.command[0].bytes[0] == 0x02 && out.command[0].bytes[3] == 7);
    sine.phase = 0x8000;
    CHECK(SDL_IForce_Upload(&ff, 1, &sine, 1105, &out) == SDL_IFORCE_UPLOAD_DEFERRED);
    sine.offset = -3000;
    sine.magnitude = 1;
    CHECK(SDL_IForce_Upload(&ff, 1, &sine, 1106, &out) == SDL_IFORCE_UPLOAD_DEFERRED);
    CHECK(SDL_IForce_NextDeferred(&ff, 1120, &out));
    CHECK(out.count == 1 && CommandIs(&out.command[0], "\x04\x10\x00\x00\xF5\x80\x64\x00", 8));

    /* A condition change sends both blocks */
    spring.condition[1].deadband = 0x8000;
    CHECK(SDL_IForce_Upload(&ff, 2, &spring, 1100, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 2 && out.command[0].bytes[0] == 0x05 && out.command[1].bytes[0] == 0x05);
    CHECK(out.command[1].bytes[7] == 0xF4 && out.command[1].bytes[8] == 0x01);
    spring.trigger_interval = 300;
    CHECK(SDL_IForce_Upload(&ff, 2, &spring, 1101, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 1 && out.command[0].bytes[0] == 0x01 && out.command[0].bytes[7] == 0x2C && out.command[0].bytes[8] == 0x01);

    /* An effect that should play is played again after its core */
    SDL_IForce_SetPlaying(&ff, 2, true);
    spring.delay = 1;
    CHECK(SDL_IForce_Upload(&ff, 2, &spring, 1102, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 2 && CommandIs(&out.command[1], "\x41\x02\x01\x01", 4));
    /* but not when only a block changed */
    spring.condition[0].center = 0;
    CHECK(SDL_IForce_Upload(&ff, 2, &spring, 1200, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 2 && out.command[0].bytes[0] == 0x05 && out.command[1].bytes[0] == 0x05);
    SDL_IForce_SetPlaying(&ff, 2, false);
    spring.delay = 2;
    CHECK(SDL_IForce_Upload(&ff, 2, &spring, 1300, &out) == SDL_IFORCE_UPLOAD_SENT && out.count == 1);
    SDL_IForce_SetPlaying(&ff, 9, true);

    /* Erasing drops a waiting update */
    constant.level = 0x0200;
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 1061, &out) == SDL_IFORCE_UPLOAD_DEFERRED);
    SDL_IForce_Erase(&ff, 0);
    CHECK(!SDL_IForce_NextDeferred(&ff, 5000, &out) && !SDL_IForce_DeferredDeadline(&ff, &deadline));
    CHECK(!SDL_IForce_NextDeferred(NULL, 0, &out) && !SDL_IForce_DeferredDeadline(NULL, &deadline));

    /* The first waiting update by time comes first */
    SDL_IForce_InitFF(&ff, 3, 200);
    constant = Constant();
    SDL_IForce_Upload(&ff, 0, &constant, 100, &out);
    SDL_IForce_Upload(&ff, 1, &constant, 90, &out);
    constant.level = 5;
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 101, &out) == SDL_IFORCE_UPLOAD_DEFERRED);
    CHECK(SDL_IForce_Upload(&ff, 1, &constant, 101, &out) == SDL_IFORCE_UPLOAD_DEFERRED);
    CHECK(SDL_IForce_DeferredDeadline(&ff, &deadline) && deadline == 110);
    CHECK(SDL_IForce_NextDeferred(&ff, 110, &out) && out.command[0].bytes[1] == ff.slots[1].block[0].start);
    CHECK(!SDL_IForce_NextDeferred(&ff, 119, &out));
    CHECK(SDL_IForce_DeferredDeadline(&ff, &deadline) && deadline == 120);
    CHECK(SDL_IForce_NextDeferred(&ff, 120, &out) && out.command[0].bytes[1] == 0);

    /* An update that goes at once drops the one waiting before it */
    SDL_IForce_InitFF(&ff, 1, 200);
    constant = Constant();
    SDL_IForce_Upload(&ff, 0, &constant, 100, &out);
    constant.level = 0x1100;
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 105, &out) == SDL_IFORCE_UPLOAD_DEFERRED);
    constant.level = 0x2200;
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 125, &out) == SDL_IFORCE_UPLOAD_SENT);
    CHECK(out.count == 1 && CommandIs(&out.command[0], "\x03\x00\x00\x22", 4));
    CHECK(!SDL_IForce_DeferredDeadline(&ff, &deadline) && !SDL_IForce_NextDeferred(&ff, 1000, &out));
    /* and so does an update back to what the device holds */
    constant.level = 0x3300;
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 130, &out) == SDL_IFORCE_UPLOAD_DEFERRED);
    constant.level = 0x2200;
    CHECK(SDL_IForce_Upload(&ff, 0, &constant, 131, &out) == SDL_IFORCE_UPLOAD_UNCHANGED && out.count == 0);
    CHECK(!SDL_IForce_DeferredDeadline(&ff, &deadline) && !SDL_IForce_NextDeferred(&ff, 1000, &out));
}

/* The RS-232 framing, as Linux's receiver takes it */
typedef struct Frames
{
    int count;
    uint8_t type[16];
    uint8_t length[16];
    uint8_t data[16][SDL_IFORCE_MAX_LENGTH];
} Frames;

static void OnFrame(void *userdata, uint8_t type, const uint8_t *data, size_t length)
{
    Frames *frames = (Frames *)userdata;

    if (frames->count < 16) {
        frames->type[frames->count] = type;
        frames->length[frames->count] = (uint8_t)length;
        memcpy(frames->data[frames->count], data, length);
        ++frames->count;
    }
}

static void Feed(SDL_IForceSerialParser *parser, const uint8_t *bytes, size_t length, Frames *frames)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);

    if (length) {
        memcpy(copy, bytes, length);
    }
    SDL_IForce_FeedSerial(parser, copy, length, OnFrame, frames);
    free(copy);
}

static void TestSerialFraming(void)
{
    static const uint8_t wheel[] = { 0x2B, 0x03, 0x07, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xF0, 0xDF };
    static const uint8_t stream[] = {
        0x11, 0x22, 0x2B, 0x00, 0xFF, 0x03, 0x4D, 0xEF, 0x05, 0x70, /* junk, a 00 type skipped, M */
        0x2B, 0x04, 0x2B, 0xFF, 0x01, 0x4F, 0x9A,                   /* type 04 resets, and the next 2B starts O */
        0x2B, 0x02, 0x11,                                           /* length 11 resets */
        0x2B, 0x02, 0x00, 0x02, 0x03, 0x00, 0x28,                   /* a 00 length makes the next byte the length */
        0x2B, 0x03, 0x07, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xF0, 0xDF
    };
    SDL_IForceSerialParser parser;
    Frames frames, whole;
    size_t i, k;
    uint8_t out[32];

    memset(&frames, 0, sizeof(frames));
    SDL_IForce_InitSerialParser(&parser);
    Feed(&parser, wheel, sizeof(wheel), &frames);
    CHECK(frames.count == 1 && frames.type[0] == 0x03 && frames.length[0] == 7 && parser.mismatches == 0);
    CHECK(memcmp(frames.data[0], wheel + 3, 7) == 0);

    memset(&whole, 0, sizeof(whole));
    SDL_IForce_InitSerialParser(&parser);
    Feed(&parser, stream, sizeof(stream), &whole);
    CHECK(whole.count == 4);
    CHECK(whole.type[0] == 0xFF && whole.length[0] == 3 && memcmp(whole.data[0], "\x4D\xEF\x05", 3) == 0);
    CHECK(whole.type[1] == 0xFF && whole.length[1] == 1 && whole.data[1][0] == 0x4F);
    CHECK(whole.type[2] == 0x02 && whole.length[2] == 2 && memcmp(whole.data[2], "\x03\x00", 2) == 0);
    CHECK(whole.type[3] == 0x03 && whole.length[3] == 7);
    CHECK(parser.mismatches == 0);

    /* One byte per read, and split at every position, decode the same */
    memset(&frames, 0, sizeof(frames));
    SDL_IForce_InitSerialParser(&parser);
    for (i = 0; i < sizeof(stream); ++i) {
        Feed(&parser, stream + i, 1, &frames);
    }
    CHECK(frames.count == whole.count && memcmp(frames.data, whole.data, sizeof(whole.data)) == 0);
    CHECK(memcmp(frames.type, whole.type, sizeof(whole.type)) == 0);
    for (k = 0; k <= sizeof(stream); ++k) {
        memset(&frames, 0, sizeof(frames));
        SDL_IForce_InitSerialParser(&parser);
        Feed(&parser, stream, k, &frames);
        Feed(&parser, stream + k, sizeof(stream) - k, &frames);
        CHECK(frames.count == whole.count && memcmp(frames.length, whole.length, sizeof(whole.length)) == 0);
    }

    /* A frame whose last byte is not the XOR still decodes, and is counted */
    memcpy(out, wheel, sizeof(wheel));
    out[sizeof(wheel) - 1] = 0x00;
    memset(&frames, 0, sizeof(frames));
    SDL_IForce_InitSerialParser(&parser);
    Feed(&parser, out, sizeof(wheel), &frames);
    CHECK(frames.count == 1 && parser.mismatches == 1);
    /* 16 data bytes is the most */
    {
        uint8_t big[4 + 16], data[16];

        memset(data, 0x2B, sizeof(data));
        CHECK(SDL_IForce_BuildSerialFrame(big, sizeof(big), 0x02, data, 16) == 20);
        memset(&frames, 0, sizeof(frames));
        SDL_IForce_InitSerialParser(&parser);
        Feed(&parser, big, sizeof(big), &frames);
        CHECK(frames.count == 1 && frames.length[0] == 16 && parser.mismatches == 0);
        CHECK(SDL_IForce_BuildSerialFrame(big, sizeof(big), 0x02, data, 17) == 0);
        CHECK(SDL_IForce_BuildSerialFrame(big, 19, 0x02, data, 16) == 0);
    }

    /* The frames the host sends */
    CHECK(SDL_IForce_BuildSerialFrame(out, sizeof(out), 0xFF, (const uint8_t *)"\x4F", 1) == 5);
    CHECK(memcmp(out, "\x2B\xFF\x01\x4F\x9A", 5) == 0);
    CHECK(SDL_IForce_BuildSerialFrame(out, sizeof(out), 0x41, (const uint8_t *)"\x00\x01\x01", 3) == 7);
    CHECK(memcmp(out, "\x2B\x41\x03\x00\x01\x01\x69", 7) == 0);
    CHECK(SDL_IForce_BuildSerialFrame(out, sizeof(out), 0x40, (const uint8_t *)"\x03\x00", 2) == 6);
    CHECK(memcmp(out, "\x2B\x40\x02\x03\x00\x6A", 6) == 0);
    CHECK(SDL_IForce_BuildSerialFrame(NULL, sizeof(out), 0x40, NULL, 0) == 0);
    SDL_IForce_FeedSerial(&parser, NULL, 5, OnFrame, &frames);
}

int main(void)
{
    TestModels();
    TestQuerySequence();
    TestReplies();
    TestWheel();
    TestButtonsAndHat();
    TestGuillemotWheel();
    TestJoystick();
    TestTruncation();
    TestStatusAndOthers();
    TestReset();
    TestCommands();
    TestEffects();
    TestMemory();
    TestUpdates();
    TestSerialFraming();
    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
