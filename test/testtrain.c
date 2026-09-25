/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_train_proto.c, the train
   controllers of hifihedgehog/SDL#33 Part 10. Test numbers follow the part's
   three sections. No capture of any report exists, so the reports are
   constructed from the Train Controller Database's tables and OpenBVE's
   decoders. */

#include "../src/joystick/hidapi/SDL_hidapi_train_proto.h"

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

#define BRAKE 4
#define POWER 5
#define LEVER 1
#define REVERSER 3

#define B_SOUTH 0
#define B_EAST  1
#define B_WEST  2
#define B_NORTH 3
#define B_BACK  4
#define B_START 6
#define B_LEFT_SHOULDER  9
#define B_RIGHT_SHOULDER 10
#define B_MISC1 11
#define B_MISC2 12

#define UP    SDL_TRAIN_HAT_UP
#define RIGHT SDL_TRAIN_HAT_RIGHT
#define DOWN  SDL_TRAIN_HAT_DOWN
#define LEFT  SDL_TRAIN_HAT_LEFT

/* Decodes from an exact-size heap copy, so a read past the length reaches
   the sanitizer */
static bool Decode(int model, const uint8_t *report, size_t length, SDL_TrainState *state)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, report, length);
    }
    result = SDL_Train_Decode(model, copy, length, state);
    free(copy);
    return result;
}

static bool Same(const SDL_TrainState *a, const SDL_TrainState *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static SDL_TrainState Rest(int model)
{
    SDL_TrainState state;

    SDL_Train_ResetState(model, &state);
    return state;
}

/* The setup packet, as it goes on the wire */
static void Setup(const SDL_TrainControl *control, uint8_t setup[8])
{
    setup[0] = control->request_type;
    setup[1] = control->request;
    setup[2] = (uint8_t)(control->value & 0xFF);
    setup[3] = (uint8_t)(control->value >> 8);
    setup[4] = (uint8_t)(control->index & 0xFF);
    setup[5] = (uint8_t)(control->index >> 8);
    setup[6] = (uint8_t)(control->length & 0xFF);
    setup[7] = (uint8_t)(control->length >> 8);
}

static bool IsControl(const SDL_TrainControl *control, const char *setup_bytes, const char *data, size_t length)
{
    uint8_t setup[8];

    Setup(control, setup);
    return memcmp(setup, setup_bytes, 8) == 0 && control->length == length && (length == 0 || memcmp(control->data, data, length) == 0);
}

/* Taito 11 */
static void TestIdentity(void)
{
    SDL_TrainIdentity identity;

    CHECK(SDL_Train_Identify(0x0AE4, 0x0004, 0x0102) == SDL_TRAIN_TYPE2);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0004, 0x0000) == SDL_TRAIN_TYPE2);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0004, 0x0100) == SDL_TRAIN_MTC_P5B8);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0005, 0x0102) == SDL_TRAIN_SHINKANSEN);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0005, 0x0100) == SDL_TRAIN_SHINKANSEN);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0007, 0x0000) == SDL_TRAIN_RYOJOHEN);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0006, 0x0102) == SDL_TRAIN_NONE);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0008, 0x0102) == SDL_TRAIN_NONE);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0003, 0x0102) == SDL_TRAIN_NONE);
    CHECK(SDL_Train_Identify(0x0AE5, 0x0004, 0x0102) == SDL_TRAIN_NONE);
    CHECK(strcmp(SDL_Train_Name(SDL_TRAIN_TYPE2), "Taito Densha de GO! Type 2 Controller") == 0);
    CHECK(strcmp(SDL_Train_Name(SDL_TRAIN_MTC_P5B8), "Multi Train Controller (P5/B8)") == 0);
    CHECK(strcmp(SDL_Train_Name(SDL_TRAIN_SHINKANSEN), "Taito Densha de GO! Shinkansen Controller") == 0);
    CHECK(strcmp(SDL_Train_Name(SDL_TRAIN_RYOJOHEN), "Taito Densha de GO! Ryojohen Controller") == 0);

    /* Multi Train Controller 1: four cartridges by bcdDevice, and the Train Mascon */
    CHECK(SDL_Train_Identify(0x0AE4, 0x0101, 0x0300) == SDL_TRAIN_MTC_P4B7);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0101, 0x0400) == SDL_TRAIN_MTC_P4B2B7);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0101, 0x0800) == SDL_TRAIN_MTC_P5B7);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0101, 0x0A00) == SDL_TRAIN_MTC_P13B7);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0101, 0x0000) == SDL_TRAIN_NONE);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0101, 0x0100) == SDL_TRAIN_NONE);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0101, 0x0500) == SDL_TRAIN_NONE);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0101, 0x0900) == SDL_TRAIN_NONE);
    CHECK(SDL_Train_Identify(0x1C06, 0x77A7, 0x0202) == SDL_TRAIN_MASCON);
    CHECK(SDL_Train_Identify(0x1C06, 0x77A7, 0x0000) == SDL_TRAIN_MASCON);
    CHECK(SDL_Train_Identify(0x1C06, 0x77A8, 0x0202) == SDL_TRAIN_NONE);
    CHECK(SDL_Train_Identify(0x1C07, 0x77A7, 0x0202) == SDL_TRAIN_NONE);
    CHECK(SDL_Train_Identify(0x0AE4, 0x77A7, 0x0202) == SDL_TRAIN_NONE);
    CHECK(strcmp(SDL_Train_Name(SDL_TRAIN_MTC_P4B7), "Multi Train Controller (P4/B7)") == 0);
    CHECK(strcmp(SDL_Train_Name(SDL_TRAIN_MTC_P4B2B7), "Multi Train Controller (P4/B2-B7)") == 0);
    CHECK(strcmp(SDL_Train_Name(SDL_TRAIN_MTC_P5B7), "Multi Train Controller (P5/B7)") == 0);
    CHECK(strcmp(SDL_Train_Name(SDL_TRAIN_MTC_P13B7), "Multi Train Controller (P13/B7)") == 0);
    CHECK(strcmp(SDL_Train_Name(SDL_TRAIN_MASCON), "Train Mascon") == 0);
    CHECK(strcmp(SDL_Train_Name(SDL_TRAIN_MASTER), "Pony Canyon Master Controller") == 0);
    CHECK(SDL_Train_Name(SDL_TRAIN_NONE) == NULL);

    CHECK(SDL_Train_GetIdentity(SDL_TRAIN_TYPE2, &identity) && identity.naxes == 6 && identity.nbuttons == 12 &&
          identity.nhats == 1 && identity.report_length == 6);
    CHECK(SDL_Train_GetIdentity(SDL_TRAIN_SHINKANSEN, &identity) && identity.report_length == 6);
    CHECK(SDL_Train_GetIdentity(SDL_TRAIN_RYOJOHEN, &identity) && identity.report_length == 8 && identity.naxes == 6);
    CHECK(SDL_Train_GetIdentity(SDL_TRAIN_MASCON, &identity) && identity.naxes == 4 && identity.nbuttons == 13 &&
          identity.nhats == 1 && identity.report_length == 4);
    CHECK(SDL_Train_GetIdentity(SDL_TRAIN_MASTER, &identity) && identity.naxes == 4 && identity.nbuttons == 4 &&
          identity.nhats == 0 && identity.report_length == 0);
    CHECK(!SDL_Train_GetIdentity(SDL_TRAIN_NONE, &identity));

    CHECK(strcmp(SDL_Train_GetMapping(SDL_TRAIN_RYOJOHEN),
                 "a:b0,b:b1,x:b2,y:b3,back:b4,start:b6,leftshoulder:b9,rightshoulder:b10,misc1:b11,"
                 "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,lefttrigger:a4,righttrigger:a5,") == 0);
    CHECK(SDL_Train_GetMapping(SDL_TRAIN_TYPE2) == SDL_Train_GetMapping(SDL_TRAIN_SHINKANSEN));
    CHECK(strcmp(SDL_Train_GetMapping(SDL_TRAIN_MTC_P13B7),
                 "a:b0,b:b1,x:b2,y:b3,back:b4,start:b6,misc1:b11,misc2:b12,"
                 "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,lefty:a1,righty:a3,") == 0);
    CHECK(SDL_Train_GetMapping(SDL_TRAIN_MASTER) == NULL && SDL_Train_GetMapping(SDL_TRAIN_NONE) == NULL);
}

typedef struct Notch
{
    uint8_t value;
    int16_t axis;
} Notch;

static const Notch type2_brake[] = {
    { 0x79, -32768 }, { 0x8A, -25486 }, { 0x94, -18205 }, { 0x9A, -10923 }, { 0xA2, -3641 },
    { 0xA8, 3640 }, { 0xAF, 10922 }, { 0xB2, 18204 }, { 0xB5, 25485 }, { 0xB9, 32767 },
};
static const Notch type2_power[] = {
    { 0x81, -32768 }, { 0x6D, -19661 }, { 0x54, -6554 }, { 0x3F, 6553 }, { 0x21, 19660 }, { 0x00, 32767 },
};
static const Notch shinkansen_brake[] = {
    { 0x1C, -32768 }, { 0x38, -24576 }, { 0x54, -16384 }, { 0x70, -8192 }, { 0x8B, 0 },
    { 0xA7, 8191 }, { 0xC3, 16383 }, { 0xDF, 24575 }, { 0xFB, 32767 },
};
static const Notch shinkansen_power[] = {
    { 0x12, -32768 }, { 0x24, -27727 }, { 0x36, -22686 }, { 0x48, -17645 }, { 0x5A, -12603 },
    { 0x6C, -7562 }, { 0x7E, -2521 }, { 0x90, 2520 }, { 0xA2, 7561 }, { 0xB4, 12602 },
    { 0xC6, 17644 }, { 0xD7, 22685 }, { 0xE9, 27726 }, { 0xFB, 32767 },
};
static const Notch ryojohen_power[] = {
    { 0x00, -32768 }, { 0x3C, -16384 }, { 0x78, 0 }, { 0xB4, 16383 }, { 0xF0, 32767 },
};

static const uint8_t type2_rest[6] = { 0x01, 0x79, 0x81, 0xFF, 0x08, 0x00 };
static const uint8_t shinkansen_rest[6] = { 0x1C, 0x12, 0xFF, 0x08, 0x00, 0x00 };
static const uint8_t ryojohen_rest[8] = { 0x23, 0x00, 0xFF, 0x08, 0x00, 0x00, 0x00, 0x00 };

/* Taito 1 */
static void TestTaitoRest(void)
{
    const int models[3] = { SDL_TRAIN_TYPE2, SDL_TRAIN_SHINKANSEN, SDL_TRAIN_RYOJOHEN };
    const uint8_t *reports[3] = { type2_rest, shinkansen_rest, ryojohen_rest };
    const size_t lengths[3] = { 6, 6, 8 };
    int i;

    for (i = 0; i < 3; ++i) {
        SDL_TrainState state = Rest(models[i]);
        const SDL_TrainState rest = state;

        CHECK(state.axes[BRAKE] == -32768 && state.axes[POWER] == -32768);
        CHECK(state.axes[0] == 0 && state.axes[1] == 0 && state.axes[2] == 0 && state.axes[3] == 0);
        CHECK(Decode(models[i], reports[i], lengths[i], &state));
        CHECK(Same(&state, &rest));
    }
    /* The P5/B8 cartridge decodes as a Type 2 */
    {
        SDL_TrainState state = Rest(SDL_TRAIN_MTC_P5B8);
        const SDL_TrainState rest = state;

        CHECK(Decode(SDL_TRAIN_MTC_P5B8, type2_rest, 6, &state) && Same(&state, &rest));
    }
}

static void CheckNotches(int model, const uint8_t *rest, size_t length, int offset, int axis, const Notch *notches, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        SDL_TrainState state = Rest(model), expected;
        uint8_t report[8];

        memcpy(report, rest, length);
        report[offset] = notches[i].value;
        CHECK(Decode(model, report, length, &state));
        expected = Rest(model);
        expected.axes[axis] = notches[i].axis;
        CHECK(Same(&state, &expected));
    }
}

/* Taito 2 */
static void TestTaitoNotches(void)
{
    CheckNotches(SDL_TRAIN_TYPE2, type2_rest, 6, 1, BRAKE, type2_brake, sizeof(type2_brake) / sizeof(type2_brake[0]));
    CheckNotches(SDL_TRAIN_TYPE2, type2_rest, 6, 2, POWER, type2_power, sizeof(type2_power) / sizeof(type2_power[0]));
    CheckNotches(SDL_TRAIN_MTC_P5B8, type2_rest, 6, 1, BRAKE, type2_brake, sizeof(type2_brake) / sizeof(type2_brake[0]));
    CheckNotches(SDL_TRAIN_SHINKANSEN, shinkansen_rest, 6, 0, BRAKE, shinkansen_brake, sizeof(shinkansen_brake) / sizeof(shinkansen_brake[0]));
    CheckNotches(SDL_TRAIN_SHINKANSEN, shinkansen_rest, 6, 1, POWER, shinkansen_power, sizeof(shinkansen_power) / sizeof(shinkansen_power[0]));
    CheckNotches(SDL_TRAIN_RYOJOHEN, ryojohen_rest, 8, 1, POWER, ryojohen_power, sizeof(ryojohen_power) / sizeof(ryojohen_power[0]));
}

/* Taito 3 and 4 */
static void TestTaitoKeep(void)
{
    SDL_TrainState state = Rest(SDL_TRAIN_TYPE2);
    uint8_t report[8];
    int value;

    CHECK(Decode(SDL_TRAIN_TYPE2, (const uint8_t *)"\x01\x9A\x81\xFF\x08\x00", 6, &state) && state.axes[BRAKE] == -10923);
    CHECK(Decode(SDL_TRAIN_TYPE2, (const uint8_t *)"\x01\xFF\x81\xFF\x08\x00", 6, &state) && state.axes[BRAKE] == -10923);
    CHECK(Decode(SDL_TRAIN_TYPE2, (const uint8_t *)"\x01\xA2\x81\xFF\x08\x00", 6, &state) && state.axes[BRAKE] == -3641);
    CHECK(Decode(SDL_TRAIN_TYPE2, (const uint8_t *)"\x01\x7A\x81\xFF\x08\x00", 6, &state) && state.axes[BRAKE] == -3641);

    /* FF, and every value outside a table, keeps the last step */
    state = Rest(SDL_TRAIN_TYPE2);
    CHECK(Decode(SDL_TRAIN_TYPE2, (const uint8_t *)"\x01\x79\x3F\xFF\x08\x00", 6, &state) && state.axes[POWER] == 6553);
    for (value = 0; value < 256; ++value) {
        size_t i;
        bool listed = false;

        for (i = 0; i < sizeof(type2_power) / sizeof(type2_power[0]); ++i) {
            listed = listed || type2_power[i].value == value;
        }
        if (!listed) {
            memcpy(report, "\x01\x79\x3F\xFF\x08\x00", 6);
            report[2] = (uint8_t)value;
            CHECK(Decode(SDL_TRAIN_TYPE2, report, 6, &state) && state.axes[POWER] == 6553);
        }
    }
    CHECK(Decode(SDL_TRAIN_TYPE2, (const uint8_t *)"\x01\x79\x6C\xFF\x08\x00", 6, &state) && state.axes[POWER] == 6553);

    state = Rest(SDL_TRAIN_SHINKANSEN);
    CHECK(Decode(SDL_TRAIN_SHINKANSEN, (const uint8_t *)"\x70\x90\xFF\x08\x00\x00", 6, &state));
    CHECK(state.axes[BRAKE] == -8192 && state.axes[POWER] == 2520);
    CHECK(Decode(SDL_TRAIN_SHINKANSEN, (const uint8_t *)"\xFF\xFF\xFF\x08\x00\x00", 6, &state));
    CHECK(state.axes[BRAKE] == -8192 && state.axes[POWER] == 2520);
    CHECK(Decode(SDL_TRAIN_SHINKANSEN, (const uint8_t *)"\x1D\x91\xFF\x08\x00\x00", 6, &state));
    CHECK(state.axes[BRAKE] == -8192 && state.axes[POWER] == 2520);

    state = Rest(SDL_TRAIN_RYOJOHEN);
    CHECK(Decode(SDL_TRAIN_RYOJOHEN, (const uint8_t *)"\x23\x78\xFF\x08\x00\x00\x00\x00", 8, &state) && state.axes[POWER] == 0);
    CHECK(Decode(SDL_TRAIN_RYOJOHEN, (const uint8_t *)"\x23\x3D\xFF\x08\x00\x00\x00\x00", 8, &state) && state.axes[POWER] == 0);
    CHECK(Decode(SDL_TRAIN_RYOJOHEN, (const uint8_t *)"\x23\xFF\xFF\x08\x00\x00\x00\x00", 8, &state) && state.axes[POWER] == 0);
}

/* Taito 5 */
static void TestRyojohenBrake(void)
{
    static const Notch brake[] = {
        { 0x23, -32768 }, { 0x2A, -30219 }, { 0x64, -9103 }, { 0x89, 4369 }, { 0xD6, 32403 },
        { 0xD7, 32767 }, { 0xD8, 32767 }, { 0xDF, 32767 },
    };
    SDL_TrainState state = Rest(SDL_TRAIN_RYOJOHEN);
    uint8_t report[8];
    size_t i;
    int value;

    for (i = 0; i < sizeof(brake) / sizeof(brake[0]); ++i) {
        memcpy(report, ryojohen_rest, 8);
        report[0] = brake[i].value;
        CHECK(Decode(SDL_TRAIN_RYOJOHEN, report, 8, &state) && state.axes[BRAKE] == brake[i].axis);
    }
    /* Each of D8 to DF reaches the top from the bottom */
    for (value = 0xD8; value <= 0xDF; ++value) {
        memcpy(report, ryojohen_rest, 8);
        CHECK(Decode(SDL_TRAIN_RYOJOHEN, report, 8, &state) && state.axes[BRAKE] == -32768);
        report[0] = (uint8_t)value;
        CHECK(Decode(SDL_TRAIN_RYOJOHEN, report, 8, &state) && state.axes[BRAKE] == 32767);
    }
    /* Every value in the span, rising, and the rest kept */
    {
        int16_t last = -32768;
        int rising = 1;

        for (value = 0x23; value <= 0xD7; ++value) {
            memcpy(report, ryojohen_rest, 8);
            report[0] = (uint8_t)value;
            Decode(SDL_TRAIN_RYOJOHEN, report, 8, &state);
            if (value > 0x23 && state.axes[BRAKE] <= last) {
                rising = 0;
            }
            last = state.axes[BRAKE];
        }
        CHECK(rising && last == 32767);
    }
    memcpy(report, ryojohen_rest, 8);
    report[0] = 0x64;
    CHECK(Decode(SDL_TRAIN_RYOJOHEN, report, 8, &state) && state.axes[BRAKE] == -9103);
    for (value = 0; value < 256; ++value) {
        if (value < 0x23 || value > 0xDF) {
            report[0] = (uint8_t)value;
            CHECK(Decode(SDL_TRAIN_RYOJOHEN, report, 8, &state) && state.axes[BRAKE] == -9103);
        }
    }
}

/* Taito 6 */
static void TestTaitoButtons(void)
{
    static const int type2[8] = { B_SOUTH, B_WEST, B_EAST, B_NORTH, B_BACK, B_START, -1, -1 };
    static const int shinkansen[8] = { B_NORTH, B_EAST, B_SOUTH, B_WEST, B_BACK, B_START, -1, -1 };
    static const int ryojohen[8] = { B_EAST, B_SOUTH, B_WEST, B_RIGHT_SHOULDER, B_LEFT_SHOULDER, B_BACK, B_START, -1 };
    int bit;

    for (bit = 0; bit < 8; ++bit) {
        SDL_TrainState state, expected;
        uint8_t report[8];

        state = Rest(SDL_TRAIN_TYPE2);
        memcpy(report, type2_rest, 6);
        report[5] = (uint8_t)(1u << bit);
        CHECK(Decode(SDL_TRAIN_TYPE2, report, 6, &state));
        expected = Rest(SDL_TRAIN_TYPE2);
        expected.buttons = (uint16_t)((type2[bit] >= 0) ? (1u << type2[bit]) : 0);
        CHECK(Same(&state, &expected));

        state = Rest(SDL_TRAIN_SHINKANSEN);
        memcpy(report, shinkansen_rest, 6);
        report[4] = (uint8_t)(1u << bit);
        CHECK(Decode(SDL_TRAIN_SHINKANSEN, report, 6, &state));
        expected = Rest(SDL_TRAIN_SHINKANSEN);
        expected.buttons = (uint16_t)((shinkansen[bit] >= 0) ? (1u << shinkansen[bit]) : 0);
        CHECK(Same(&state, &expected));

        state = Rest(SDL_TRAIN_RYOJOHEN);
        memcpy(report, ryojohen_rest, 8);
        report[4] = (uint8_t)(1u << bit);
        CHECK(Decode(SDL_TRAIN_RYOJOHEN, report, 8, &state));
        expected = Rest(SDL_TRAIN_RYOJOHEN);
        expected.buttons = (uint16_t)((ryojohen[bit] >= 0) ? (1u << ryojohen[bit]) : 0);
        CHECK(Same(&state, &expected));
    }
}

/* Taito 7 and 8 */
static void TestTaitoHatAndPedal(void)
{
    static const uint8_t hats[8] = { UP, UP | RIGHT, RIGHT, DOWN | RIGHT, DOWN, DOWN | LEFT, LEFT, UP | LEFT };
    static const uint8_t centered[] = { 0x08, 0x09, 0x0F, 0x80, 0xFF };
    SDL_TrainState state;
    uint8_t report[8];
    int value;
    size_t i;

    for (value = 0; value < 8; ++value) {
        state = Rest(SDL_TRAIN_TYPE2);
        memcpy(report, type2_rest, 6);
        report[4] = (uint8_t)value;
        CHECK(Decode(SDL_TRAIN_TYPE2, report, 6, &state) && state.hat == hats[value] && state.buttons == 0);
        state = Rest(SDL_TRAIN_SHINKANSEN);
        memcpy(report, shinkansen_rest, 6);
        report[3] = (uint8_t)value;
        CHECK(Decode(SDL_TRAIN_SHINKANSEN, report, 6, &state) && state.hat == hats[value]);
        state = Rest(SDL_TRAIN_RYOJOHEN);
        memcpy(report, ryojohen_rest, 8);
        report[3] = (uint8_t)value;
        CHECK(Decode(SDL_TRAIN_RYOJOHEN, report, 8, &state) && state.hat == hats[value]);
    }
    for (i = 0; i < sizeof(centered); ++i) {
        state = Rest(SDL_TRAIN_TYPE2);
        state.hat = UP;
        memcpy(report, type2_rest, 6);
        report[4] = centered[i];
        CHECK(Decode(SDL_TRAIN_TYPE2, report, 6, &state) && state.hat == 0);
    }

    /* The pedal: 00 presses Misc1 */
    state = Rest(SDL_TRAIN_TYPE2);
    CHECK(Decode(SDL_TRAIN_TYPE2, (const uint8_t *)"\x01\x79\x81\x00\x08\x00", 6, &state) && state.buttons == (1u << B_MISC1));
    CHECK(Decode(SDL_TRAIN_TYPE2, (const uint8_t *)"\x01\x79\x81\xFF\x08\x00", 6, &state) && state.buttons == 0);
    CHECK(Decode(SDL_TRAIN_TYPE2, (const uint8_t *)"\x01\x79\x81\x01\x08\x00", 6, &state) && state.buttons == 0);
    CHECK(Decode(SDL_TRAIN_TYPE2, (const uint8_t *)"\x01\x79\x81\x7F\x08\x00", 6, &state) && state.buttons == 0);
    state = Rest(SDL_TRAIN_SHINKANSEN);
    CHECK(Decode(SDL_TRAIN_SHINKANSEN, (const uint8_t *)"\x1C\x12\x00\x08\x00\x00", 6, &state) && state.buttons == (1u << B_MISC1));
    state = Rest(SDL_TRAIN_RYOJOHEN);
    CHECK(Decode(SDL_TRAIN_RYOJOHEN, (const uint8_t *)"\x23\x00\x00\x08\x00\x00\x00\x00", 8, &state) && state.buttons == (1u << B_MISC1));
}

/* Taito 9, 10 and 14 */
static void TestTaitoRejects(void)
{
    const int models[3] = { SDL_TRAIN_TYPE2, SDL_TRAIN_SHINKANSEN, SDL_TRAIN_RYOJOHEN };
    static const uint8_t busy[3][8] = {
        { 0x01, 0x8A, 0x6D, 0x00, 0x02, 0x21, 0x00, 0x00 },
        { 0x38, 0x24, 0x00, 0x02, 0x21, 0x00, 0x00, 0x00 },
        { 0x2A, 0x3C, 0x00, 0x02, 0x41, 0x00, 0x00, 0x00 },
    };
    const size_t lengths[3] = { 6, 6, 8 };
    SDL_TrainState state, before;
    uint8_t stale[16];
    int m;
    size_t n;

    state = Rest(SDL_TRAIN_TYPE2);
    before = state;
    CHECK(!Decode(SDL_TRAIN_TYPE2, (const uint8_t *)"\x00\x8A\x81\xFF\x08\x00", 6, &state) && Same(&state, &before));
    CHECK(!Decode(SDL_TRAIN_TYPE2, (const uint8_t *)"\x02\x8A\x81\xFF\x08\x00", 6, &state) && Same(&state, &before));

    for (m = 0; m < 3; ++m) {
        for (n = 0; n < lengths[m]; ++n) {
            state = Rest(models[m]);
            before = state;
            CHECK(!Decode(models[m], busy[m], n, &state) && Same(&state, &before));
            memset(stale, 0x8A, sizeof(stale));
            memcpy(stale, busy[m], n);
            CHECK(!SDL_Train_Decode(models[m], stale, n, &state) && Same(&state, &before));
        }
        /* The whole report changes the state */
        state = Rest(models[m]);
        CHECK(Decode(models[m], busy[m], lengths[m], &state) && !Same(&state, &before));
    }

    /* Bytes past the report are ignored, stale or delivered */
    {
        SDL_TrainState whole = Rest(SDL_TRAIN_TYPE2);
        uint8_t longer[8];

        Decode(SDL_TRAIN_TYPE2, busy[0], 6, &whole);
        memcpy(longer, busy[0], 6);
        longer[6] = 0x8A;
        longer[7] = 0x8A;
        for (n = 6; n <= 8; ++n) {
            state = Rest(SDL_TRAIN_TYPE2);
            CHECK(Decode(SDL_TRAIN_TYPE2, longer, n, &state) && Same(&state, &whole));
        }
        whole = Rest(SDL_TRAIN_SHINKANSEN);
        Decode(SDL_TRAIN_SHINKANSEN, busy[1], 6, &whole);
        memcpy(longer, busy[1], 6);
        longer[6] = 0xFF;
        longer[7] = 0xFF;
        for (n = 7; n <= 8; ++n) {
            state = Rest(SDL_TRAIN_SHINKANSEN);
            CHECK(Decode(SDL_TRAIN_SHINKANSEN, longer, n, &state) && Same(&state, &whole));
        }
    }
    CHECK(!SDL_Train_Decode(SDL_TRAIN_TYPE2, NULL, 6, &state));
    CHECK(!SDL_Train_Decode(SDL_TRAIN_NONE, busy[0], 6, &state));
    CHECK(!SDL_Train_Decode(SDL_TRAIN_MASTER, busy[0], 6, &state));

    /* 14: a device that returns starts at rest */
    state = Rest(SDL_TRAIN_SHINKANSEN);
    Decode(SDL_TRAIN_SHINKANSEN, busy[1], 6, &state);
    SDL_Train_ResetState(SDL_TRAIN_SHINKANSEN, &state);
    before = Rest(SDL_TRAIN_SHINKANSEN);
    CHECK(Same(&state, &before));
}

/* Taito 12 and 13 */
static void TestTaitoOutputs(void)
{
    SDL_TrainOutputs outputs;
    SDL_TrainControl out[3];
    SDL_TrainState state;
    int i;

    SDL_Train_InitOutputs(&outputs);
    CHECK(SDL_Train_Rumble(SDL_TRAIN_TYPE2, &outputs, 0x4000, 0, out) == 1);
    CHECK(IsControl(&out[0], "\x41\x09\x01\x02\x00\x00\x02\x00", "\x01\x01", 2));
    CHECK(SDL_Train_Rumble(SDL_TRAIN_TYPE2, &outputs, 0xFFFF, 0, out) == 0);
    CHECK(SDL_Train_Rumble(SDL_TRAIN_TYPE2, &outputs, 0xFFFF, 1, out) == 1);
    CHECK(IsControl(&out[0], "\x41\x09\x01\x02\x00\x00\x02\x00", "\x01\x02", 2));
    CHECK(SDL_Train_Rumble(SDL_TRAIN_TYPE2, &outputs, 0, 0, out) == 2);
    CHECK(IsControl(&out[0], "\x41\x09\x01\x02\x00\x00\x02\x00", "\x00\x01", 2));
    CHECK(IsControl(&out[1], "\x41\x09\x01\x02\x00\x00\x02\x00", "\x00\x02", 2));
    CHECK(SDL_Train_Effect(SDL_TRAIN_TYPE2, &outputs, (const uint8_t *)"\x01\x03", 2, out) == 1);
    CHECK(IsControl(&out[0], "\x41\x09\x01\x02\x00\x00\x02\x00", "\x01\x03", 2));
    /* An effect that turns a motor on counts for the next rumble */
    CHECK(SDL_Train_Effect(SDL_TRAIN_TYPE2, &outputs, (const uint8_t *)"\x01\x01", 2, out) == 1 && outputs.left_motor);
    CHECK(SDL_Train_Rumble(SDL_TRAIN_TYPE2, &outputs, 1, 0, out) == 0);
    CHECK(SDL_Train_Effect(SDL_TRAIN_TYPE2, &outputs, (const uint8_t *)"\x01\x02", 2, out) == 1 && outputs.right_motor);
    CHECK(SDL_Train_Effect(SDL_TRAIN_TYPE2, &outputs, (const uint8_t *)"\x00\x02", 2, out) == 1 && !outputs.right_motor);
    CHECK(SDL_Train_Effect(SDL_TRAIN_TYPE2, &outputs, (const uint8_t *)"\x01\x03\x00", 3, out) == -1);
    CHECK(SDL_Train_Effect(SDL_TRAIN_TYPE2, &outputs, NULL, 2, out) == -1);
    CHECK(SDL_Train_Close(SDL_TRAIN_TYPE2, &outputs, out) == 3);
    CHECK(IsControl(&out[0], "\x41\x09\x01\x02\x00\x00\x02\x00", "\x00\x01", 2));
    CHECK(IsControl(&out[1], "\x41\x09\x01\x02\x00\x00\x02\x00", "\x00\x02", 2));
    CHECK(IsControl(&out[2], "\x41\x09\x01\x02\x00\x00\x02\x00", "\x00\x03", 2));
    CHECK(!outputs.left_motor && !outputs.right_motor);

    /* Shinkansen: one 8-byte payload carries both motors and the displays */
    SDL_Train_InitOutputs(&outputs);
    CHECK(memcmp(outputs.shinkansen, "\x00\x00\x00\x00\xFF\xFF\xFF\xFF", 8) == 0);
    CHECK(SDL_Train_Effect(SDL_TRAIN_SHINKANSEN, &outputs, (const uint8_t *)"\x00\x00\x85\x09\x25\x01\x30\x01", 8, out) == 1);
    CHECK(IsControl(&out[0], "\x40\x09\x01\x03\x00\x00\x08\x00", "\x00\x00\x85\x09\x25\x01\x30\x01", 8));
    CHECK(SDL_Train_Rumble(SDL_TRAIN_SHINKANSEN, &outputs, 0, 0, out) == 0);
    CHECK(SDL_Train_Rumble(SDL_TRAIN_SHINKANSEN, &outputs, 0, 9, out) == 1);
    CHECK(IsControl(&out[0], "\x40\x09\x01\x03\x00\x00\x08\x00", "\x00\x01\x85\x09\x25\x01\x30\x01", 8));
    CHECK(SDL_Train_Rumble(SDL_TRAIN_SHINKANSEN, &outputs, 9, 9, out) == 1);
    CHECK(IsControl(&out[0], "\x40\x09\x01\x03\x00\x00\x08\x00", "\x01\x01\x85\x09\x25\x01\x30\x01", 8));
    CHECK(SDL_Train_Rumble(SDL_TRAIN_SHINKANSEN, &outputs, 1, 1, out) == 0);
    CHECK(SDL_Train_Effect(SDL_TRAIN_SHINKANSEN, &outputs, (const uint8_t *)"\x00\x00", 2, out) == -1);
    CHECK(SDL_Train_Close(SDL_TRAIN_SHINKANSEN, &outputs, out) == 1);
    CHECK(IsControl(&out[0], "\x40\x09\x01\x03\x00\x00\x08\x00", "\x00\x00\x00\x00\xFF\xFF\xFF\xFF", 8));
    CHECK(!outputs.left_motor && !outputs.right_motor);

    /* The Ryojohen and the P5/B8 cartridge have no motors */
    CHECK(SDL_Train_Rumble(SDL_TRAIN_RYOJOHEN, &outputs, 1, 1, out) == -1);
    CHECK(SDL_Train_Rumble(SDL_TRAIN_MTC_P5B8, &outputs, 1, 1, out) == -1);
    CHECK(SDL_Train_Effect(SDL_TRAIN_RYOJOHEN, &outputs, (const uint8_t *)"\x01\x01", 2, out) == -1);
    CHECK(SDL_Train_Close(SDL_TRAIN_RYOJOHEN, &outputs, out) == 0);
    CHECK(SDL_Train_Effect(SDL_TRAIN_MTC_P5B8, &outputs, (const uint8_t *)"\x01\x03", 2, out) == 1);
    CHECK(IsControl(&out[0], "\x41\x09\x01\x02\x00\x00\x02\x00", "\x01\x03", 2));
    CHECK(SDL_Train_Close(SDL_TRAIN_MTC_P5B8, &outputs, out) == 3);

    /* 13: reports build no output. Decoding takes no output state at all,
       and 1000 reports leave the outputs as they were. */
    SDL_Train_InitOutputs(&outputs);
    {
        SDL_TrainOutputs before = outputs;

        state = Rest(SDL_TRAIN_SHINKANSEN);
        for (i = 0; i < 1000; ++i) {
            Decode(SDL_TRAIN_SHINKANSEN, shinkansen_rest, 6, &state);
        }
        CHECK(memcmp(&before, &outputs, sizeof(outputs)) == 0);
    }
}

static const uint8_t lever_rest[4] = { 0x01, 0x00, 0x00, 0x00 };

/* The lever axis for handle byte h on a model, from a lever at 12345 */
static int16_t LeverAxis(int model, uint8_t handle, int16_t *reverser)
{
    SDL_TrainState state = Rest(model);
    uint8_t report[4];

    memcpy(report, lever_rest, 4);
    report[1] = handle;
    state.axes[LEVER] = 12345;
    Decode(model, report, 4, &state);
    if (reverser) {
        *reverser = state.axes[REVERSER];
    }
    return state.axes[LEVER];
}

static void CheckLever(int model, const int16_t *axes, int count)
{
    int i;

    for (i = 0; i < count; ++i) {
        CHECK(LeverAxis(model, (uint8_t)(i + 1), NULL) == axes[i]);
    }
}

/* Multi Train Controller 2 to 7 */
static void TestLevers(void)
{
    static const int16_t p4b7[13] = { -32768, -28672, -24576, -20480, -16384, -12288, -8192, -4096, 0, 8192, 16384, 24575, 32767 };
    static const int16_t p4b2b7[12] = { -32768, -28087, -23406, -18725, -14043, -9362, -4681, 0, 8192, 16384, 24575, 32767 };
    static const int16_t p5b7[14] = { -32768, -28672, -24576, -20480, -16384, -12288, -8192, -4096, 0, 6553, 13107, 19660, 26214, 32767 };
    static const int16_t p13b7[22] = { -32768, -28672, -24576, -20480, -16384, -12288, -8192, -4096, 0, 2521, 5041, 7562, 10082,
                                       12603, 15123, 17644, 20164, 22685, 25205, 27726, 30246, 32767 };
    static const int16_t mascon[12] = { -32768, -27307, -21845, -16384, -10923, -5461, 0, 6553, 13107, 19660, 26214, 32767 };
    int16_t reverser;
    int nibble;

    CheckLever(SDL_TRAIN_MTC_P4B7, p4b7, 13);
    CheckLever(SDL_TRAIN_MTC_P4B2B7, p4b2b7, 12);
    CheckLever(SDL_TRAIN_MTC_P5B7, p5b7, 14);
    CheckLever(SDL_TRAIN_MTC_P13B7, p13b7, 22);
    CheckLever(SDL_TRAIN_MASCON, mascon, 12);

    /* Indexes outside the range keep the lever */
    CHECK(LeverAxis(SDL_TRAIN_MTC_P4B7, 0x00, NULL) == 12345 && LeverAxis(SDL_TRAIN_MTC_P4B7, 0x0E, NULL) == 12345);
    CHECK(LeverAxis(SDL_TRAIN_MTC_P4B2B7, 0x0D, NULL) == 12345);
    CHECK(LeverAxis(SDL_TRAIN_MTC_P5B7, 0x0F, NULL) == 12345);
    CHECK(LeverAxis(SDL_TRAIN_MASCON, 0x0D, NULL) == 12345);
    CHECK(LeverAxis(SDL_TRAIN_MTC_P13B7, 0x17, &reverser) == 12345 && reverser == -32768);
    CHECK(LeverAxis(SDL_TRAIN_MTC_P13B7, 0x89, &reverser) == 12345 && reverser == -32768);
    CHECK(LeverAxis(SDL_TRAIN_MTC_P13B7, 0x16, &reverser) == 32767 && reverser == -32768);

    /* P5/B7: the low nibble is the handle and the high one the reverser */
    CHECK(LeverAxis(SDL_TRAIN_MTC_P5B7, 0x8E, &reverser) == 32767 && reverser == 128);

    /* 7: the reverser nibble, on the models that have one */
    for (nibble = 0; nibble < 16; ++nibble) {
        const int16_t expected = (nibble == 2 || nibble == 8) ? 128 : (nibble == 1 || nibble == 4) ? 32767 : -32768;
        int16_t p4b7_reverser, mascon_reverser, p4b2b7_reverser;

        CHECK(LeverAxis(SDL_TRAIN_MTC_P4B7, (uint8_t)((nibble << 4) | 0x09), &p4b7_reverser) == 0);
        CHECK(p4b7_reverser == expected);
        CHECK(LeverAxis(SDL_TRAIN_MASCON, (uint8_t)((nibble << 4) | 0x07), &mascon_reverser) == 0);
        CHECK(mascon_reverser == expected);
        LeverAxis(SDL_TRAIN_MTC_P4B2B7, (uint8_t)((nibble << 4) | 0x08), &p4b2b7_reverser);
        CHECK(p4b2b7_reverser == expected);
    }
    /* A handle outside the range still sets the reverser */
    CHECK(LeverAxis(SDL_TRAIN_MTC_P4B7, 0x2F, &reverser) == 12345 && reverser == 128);
}

/* Multi Train Controller 8 and 9 */
static void TestLeverControls(void)
{
    static const struct
    {
        int byte;
        uint8_t bit;
        int button;
        uint8_t hat;
    } controls[] = {
        { 2, 0x01, B_MISC1, 0 }, { 2, 0x02, B_NORTH, 0 }, { 2, 0x04, B_WEST, 0 }, { 2, 0x08, B_MISC2, 0 },
        { 2, 0x10, B_SOUTH, 0 }, { 2, 0x20, B_EAST, 0 }, { 3, 0x01, B_START, 0 }, { 3, 0x02, B_BACK, 0 },
        { 3, 0x04, -1, UP }, { 3, 0x08, -1, DOWN }, { 3, 0x10, -1, LEFT }, { 3, 0x20, -1, RIGHT },
        { 2, 0x40, -1, 0 }, { 2, 0x80, -1, 0 }, { 3, 0x40, -1, 0 }, { 3, 0x80, -1, 0 },
    };
    SDL_TrainState state, before;
    uint8_t report[8], stale[16];
    size_t i, n;

    for (i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        state = Rest(SDL_TRAIN_MTC_P4B7);
        memcpy(report, "\x01\x09\x00\x00", 4);
        report[controls[i].byte] = controls[i].bit;
        CHECK(Decode(SDL_TRAIN_MTC_P4B7, report, 4, &state));
        CHECK(state.buttons == ((controls[i].button >= 0) ? (1u << controls[i].button) : 0) && state.hat == controls[i].hat);
    }
    state = Rest(SDL_TRAIN_MASCON);
    CHECK(Decode(SDL_TRAIN_MASCON, (const uint8_t *)"\x01\x07\x00\x24", 4, &state) && state.hat == (UP | RIGHT));
    CHECK(Decode(SDL_TRAIN_MASCON, (const uint8_t *)"\x01\x07\x00\x0C", 4, &state) && state.hat == (UP | DOWN));

    /* 9: byte 0 must be 01, every truncation changes nothing, and bytes
       past the fourth are ignored */
    state = Rest(SDL_TRAIN_MTC_P4B7);
    before = state;
    CHECK(!Decode(SDL_TRAIN_MTC_P4B7, (const uint8_t *)"\x00\x0D\x3F\x3F", 4, &state) && Same(&state, &before));
    CHECK(!Decode(SDL_TRAIN_MTC_P4B7, (const uint8_t *)"\x02\x0D\x3F\x3F", 4, &state) && Same(&state, &before));
    for (n = 0; n < 4; ++n) {
        CHECK(!Decode(SDL_TRAIN_MTC_P4B7, (const uint8_t *)"\x01\x2D\x3F\x3F", n, &state) && Same(&state, &before));
        memset(stale, 0x3F, sizeof(stale));
        memcpy(stale, "\x01\x2D\x3F\x3F", n);
        CHECK(!SDL_Train_Decode(SDL_TRAIN_MTC_P4B7, stale, n, &state) && Same(&state, &before));
    }
    {
        SDL_TrainState whole = Rest(SDL_TRAIN_MTC_P4B7);

        Decode(SDL_TRAIN_MTC_P4B7, (const uint8_t *)"\x01\x2D\x3F\x3F", 4, &whole);
        CHECK(!Same(&whole, &before));
        memcpy(report, "\x01\x2D\x3F\x3F\xFF\xFF\xFF\xFF", 8);
        for (n = 4; n <= 8; ++n) {
            state = Rest(SDL_TRAIN_MTC_P4B7);
            CHECK(Decode(SDL_TRAIN_MTC_P4B7, report, n, &state) && Same(&state, &whole));
        }
    }

    /* 11: a cartridge change returns as another model, at rest */
    state = Rest(SDL_TRAIN_MTC_P4B7);
    Decode(SDL_TRAIN_MTC_P4B7, (const uint8_t *)"\x01\x8D\x00\x00", 4, &state);
    CHECK(SDL_Train_Identify(0x0AE4, 0x0101, 0x0A00) == SDL_TRAIN_MTC_P13B7);
    SDL_Train_ResetState(SDL_TRAIN_MTC_P13B7, &state);
    CHECK(state.axes[LEVER] == 0 && state.axes[REVERSER] == -32768 && state.buttons == 0 && state.hat == 0);
}

/* Multi Train Controller 10 */
static void TestLamps(void)
{
    SDL_TrainOutputs outputs;
    SDL_TrainControl out[3];
    SDL_TrainState state;
    int i;

    SDL_Train_InitOutputs(&outputs);
    CHECK(SDL_Train_Effect(SDL_TRAIN_MTC_P4B7, &outputs, (const uint8_t *)"\x12", 1, out) == 1);
    CHECK(IsControl(&out[0], "\x40\x50\x12\x00\x00\x00\x00\x00", NULL, 0));
    CHECK(SDL_Train_Effect(SDL_TRAIN_MASCON, &outputs, (const uint8_t *)"\x10", 1, out) == 1);
    CHECK(IsControl(&out[0], "\x40\x50\x10\x00\x00\x00\x00\x00", NULL, 0));
    CHECK(SDL_Train_Effect(SDL_TRAIN_MTC_P13B7, &outputs, (const uint8_t *)"\x12\x00", 2, out) == -1);
    CHECK(SDL_Train_Close(SDL_TRAIN_MTC_P4B2B7, &outputs, out) == 1);
    CHECK(IsControl(&out[0], "\x40\x50\x00\x00\x00\x00\x00\x00", NULL, 0));
    CHECK(SDL_Train_Close(SDL_TRAIN_MTC_P5B7, &outputs, out) == 1 && SDL_Train_Close(SDL_TRAIN_MASCON, &outputs, out) == 1);
    CHECK(SDL_Train_Rumble(SDL_TRAIN_MASCON, &outputs, 1, 1, out) == -1);
    CHECK(SDL_Train_Close(SDL_TRAIN_MASTER, &outputs, out) == 0 && SDL_Train_Effect(SDL_TRAIN_MASTER, &outputs, (const uint8_t *)"\x10", 1, out) == -1);

    /* No transfer while 1000 reports arrive */
    SDL_Train_InitOutputs(&outputs);
    {
        SDL_TrainOutputs before = outputs;

        state = Rest(SDL_TRAIN_MASCON);
        for (i = 0; i < 1000; ++i) {
            Decode(SDL_TRAIN_MASCON, (const uint8_t *)"\x01\x07\x00\x00", 4, &state);
        }
        CHECK(memcmp(&before, &outputs, sizeof(outputs)) == 0);
    }
}

static bool Feed(SDL_TrainLineParser *parser, const char *text, size_t length, SDL_TrainState *state)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, text, length);
    }
    result = SDL_Train_FeedLine(parser, copy, length, state);
    free(copy);
    return result;
}

static int16_t Event(const char *word)
{
    SDL_TrainLineParser parser;
    SDL_TrainState state = Rest(SDL_TRAIN_MASTER);
    char line[8];

    SDL_Train_InitLine(&parser);
    memcpy(line, word, 5);
    line[5] = '\r';
    state.axes[LEVER] = 12345;
    CHECK(Feed(&parser, line, 6, &state));
    return state.axes[LEVER];
}

/* Master Controller 2 to 8 */
static void TestMasterLine(void)
{
    static const struct
    {
        const char *word;
        int16_t axis;
    } lever[] = {
        { "TSB20", -32768 }, { "TSB30", -29127 }, { "TSB40", -25486 }, { "TSE99", -21845 }, { "TSA05", -18204 },
        { "TSA15", -14564 }, { "TSA25", -10923 }, { "TSA35", -7282 }, { "TSA45", -3641 }, { "TSA50", 0 },
        { "TSA55", 4096 }, { "TSA65", 8192 }, { "TSA75", 12288 }, { "TSA85", 16384 }, { "TSA95", 20479 },
        { "TSB60", 24575 }, { "TSB70", 28671 }, { "TSB80", 32767 },
    };
    static const struct
    {
        const char *press;
        const char *release;
        int button;
    } buttons[] = {
        { "TSK99\r", "TSK00\r", B_NORTH }, { "TSX99\r", "TSX00\r", B_WEST },
        { "TSY99\r", "TSY00\r", B_SOUTH }, { "TSZ99\r", "TSZ00\r", B_EAST },
    };
    static const char *ignored[] = { "TSB99\r", "TSA5\r", "TSA500\r", "tsa50\r", "TSQ12\r", "\nTSA50\r", "TS\xC1" "50\r", "\r", "TSA50TSA50\r",
                                     "ABCDEFTSA55\r", "XTSA55\r" };
    SDL_TrainLineParser parser;
    SDL_TrainState state, before;
    size_t i;

    /* 8: before any event */
    state = Rest(SDL_TRAIN_MASTER);
    CHECK(state.axes[LEVER] == 0 && state.axes[REVERSER] == -32768 && state.buttons == 0 && state.axes[0] == 0 && state.axes[2] == 0);

    for (i = 0; i < sizeof(lever) / sizeof(lever[0]); ++i) {
        CHECK(Event(lever[i].word) == lever[i].axis);
    }

    SDL_Train_InitLine(&parser);
    state = Rest(SDL_TRAIN_MASTER);
    CHECK(Feed(&parser, "TSG99\r", 6, &state) && state.axes[REVERSER] == 128);
    CHECK(Feed(&parser, "TSG00\r", 6, &state) && state.axes[REVERSER] == 32767);
    CHECK(Feed(&parser, "TSG50\r", 6, &state) && state.axes[REVERSER] == -32768);

    /* 3: each button, and a release with no press */
    for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        state = Rest(SDL_TRAIN_MASTER);
        before = state;
        CHECK(Feed(&parser, buttons[i].release, 6, &state) && Same(&state, &before));
        CHECK(Feed(&parser, buttons[i].press, 6, &state) && state.buttons == (1u << buttons[i].button));
        CHECK(Feed(&parser, buttons[i].release, 6, &state) && Same(&state, &before));
    }

    /* 4: framing */
    SDL_Train_InitLine(&parser);
    state = Rest(SDL_TRAIN_MASTER);
    state.axes[LEVER] = 12345;
    CHECK(!Feed(&parser, "\x54\x53\x41", 3, &state) && state.axes[LEVER] == 12345);
    CHECK(Feed(&parser, "\x35\x30\x0D", 3, &state) && state.axes[LEVER] == 0);
    CHECK(Feed(&parser, "TSA55\rTSA65\r", 12, &state) && state.axes[LEVER] == 8192);
    CHECK(!Feed(&parser, "TSA55", 5, &state) && state.axes[LEVER] == 8192);
    CHECK(Feed(&parser, "\r", 1, &state) && state.axes[LEVER] == 4096);
    CHECK(memcmp("\x54\x53\x41\x35\x30\x0D", "TSA50\r", 6) == 0);
    CHECK(memcmp("\x54\x53\x42\x32\x30\x0D", "TSB20\r", 6) == 0);

    /* 5: words that change nothing */
    for (i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
        SDL_Train_InitLine(&parser);
        state = Rest(SDL_TRAIN_MASTER);
        before = state;
        CHECK(!Feed(&parser, ignored[i], strlen(ignored[i]), &state) && Same(&state, &before));
        /* and the next word still counts */
        CHECK(Feed(&parser, "TSA55\r", 6, &state) && state.axes[LEVER] == 4096);
    }

    /* A short word never borrows the last word's fifth byte */
    SDL_Train_InitLine(&parser);
    state = Rest(SDL_TRAIN_MASTER);
    CHECK(Feed(&parser, "TSA65\r", 6, &state) && state.axes[LEVER] == 8192);
    CHECK(!Feed(&parser, "TSA5\r", 5, &state) && state.axes[LEVER] == 8192);
    CHECK(!Feed(&parser, "TS\r", 3, &state) && state.axes[LEVER] == 8192);

    /* 6: a long run with no CR */
    {
        uint8_t *run = (uint8_t *)malloc(10000);
        int bounded = 1;

        memset(run, 0x41, 10000);
        SDL_Train_InitLine(&parser);
        state = Rest(SDL_TRAIN_MASTER);
        for (i = 0; i < 10000; i += 100) {
            SDL_Train_FeedLine(&parser, run + i, 100, &state);
            if (parser.count > SDL_TRAIN_WORD_LENGTH) {
                bounded = 0;
            }
        }
        CHECK(bounded);
        before = Rest(SDL_TRAIN_MASTER);
        CHECK(!Feed(&parser, "\r", 1, &state) && Same(&state, &before));
        CHECK(Feed(&parser, "TSA55\r", 6, &state) && state.axes[LEVER] == 4096 && state.buttons == 0);
        free(run);
    }

    /* 7: bytes past the count are ignored */
    {
        uint8_t buffer[16];

        SDL_Train_InitLine(&parser);
        state = Rest(SDL_TRAIN_MASTER);
        memcpy(buffer, "TSA55\rTSA65\r", 12);
        CHECK(SDL_Train_FeedLine(&parser, buffer, 6, &state) && state.axes[LEVER] == 4096);
        memcpy(buffer, "TSB2", 4);
        memcpy(buffer + 4, "0\r", 2);
        CHECK(!SDL_Train_FeedLine(&parser, buffer, 4, &state) && state.axes[LEVER] == 4096);
    }
    CHECK(!SDL_Train_FeedLine(&parser, NULL, 5, &state));
}

int main(void)
{
    TestIdentity();
    TestTaitoRest();
    TestTaitoNotches();
    TestTaitoKeep();
    TestRyojohenBrake();
    TestTaitoButtons();
    TestTaitoHatAndPedal();
    TestTaitoRejects();
    TestTaitoOutputs();
    TestLevers();
    TestLeverControls();
    TestLamps();
    TestMasterLine();

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
