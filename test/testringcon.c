/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_switch_ringcon_proto.c,
   the Ring-Con machine of hifihedgehog/SDL#33 Part 13. The numbers follow the
   part's replay tests. A simulated driver builds each output report the way
   the Switch driver does: byte 0 = 01, byte 1 = the packet counter mod 16,
   bytes 2-9 = the neutral rumble 00 01 40 40 00 01 40 40, byte 10 = the
   subcommand, arguments from byte 11, zero padded to 49 bytes. Every input
   report is fed as an exact-size heap copy, so AddressSanitizer catches a
   read past its length. The hardware captures are in testringcon_captures.h. */

#include "../src/joystick/hidapi/SDL_hidapi_switch_ringcon_proto.h"
#include "testringcon_captures.h"

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

#define REPORT_LENGTH  49
#define GAMEPAD_AXES   6 // SDL_GAMEPAD_AXIS_COUNT
#define MAX_SENT       512
#define MAX_PENDING    64

/* ------------------------------------------------------------------------ */
/* The simulated driver */

static const uint8_t neutral_rumble[8] = { 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40 };

static const uint8_t format_args[37] = {
    0x06, 0x03, 0x25, 0x06, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x16, 0xED, 0x34, 0x36,
    0x00, 0x00, 0x00, 0x0A, 0x64, 0x0B, 0xE6, 0xA9, 0x22, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0xA8, 0xE1, 0x34, 0x36
};

typedef struct PendingReply
{
    uint64_t at;
    uint8_t report[REPORT_LENGTH];
} PendingReply;

typedef struct Sim
{
    SDL_RingConMachine machine;
    uint64_t now;
    uint8_t counter;
    uint8_t sent[MAX_SENT][REPORT_LENGTH];
    uint64_t sent_ms[MAX_SENT];
    int sent_count;
    bool enabled;
    bool mcu_wanted;
    uint8_t restore_mode;
    bool active;   // SDL_RINGCON_PROP_ACTIVE
    int activations; // times it turned true
    int16_t rest;  // SDL_RINGCON_PROP_REST
    int16_t axis;  // the strain axis
    int axis_posts;
    int starts;
    int stops[32];
    int stop_count;
    // The responder: answers each send after a delay unless its subcommand is silent
    bool respond;
    int delay;
    bool silent[256];
    uint8_t query_answer[2];
    PendingReply pending[MAX_PENDING];
    int pending_count;
} Sim;

static void SimOpen(Sim *sim, bool enabled, uint64_t now)
{
    memset(sim, 0, sizeof(*sim));
    sim->enabled = enabled;
    sim->restore_mode = 0x30;
    sim->now = now;
    sim->respond = true;
    sim->delay = 20;
    sim->query_answer[0] = 0x00;
    sim->query_answer[1] = 0x20;
    SDL_RingCon_Open(&sim->machine, enabled);
}

static void Record(Sim *sim, const SDL_RingConCommand *command)
{
    uint8_t *report;

    CHECK(sim->sent_count < MAX_SENT);
    if (sim->sent_count >= MAX_SENT) {
        return;
    }
    report = sim->sent[sim->sent_count];
    memset(report, 0, REPORT_LENGTH);
    report[0] = 0x01;
    report[1] = sim->counter;
    sim->counter = (uint8_t)((sim->counter + 1) & 0x0F);
    memcpy(&report[2], neutral_rumble, sizeof(neutral_rumble));
    report[10] = command->subcommand;
    CHECK(command->length <= SDL_RINGCON_ARGS_MAX);
    memcpy(&report[11], command->args, command->length);
    sim->sent_ms[sim->sent_count] = sim->now;
    ++sim->sent_count;
}

static void Apply(Sim *sim, const SDL_RingConOutput *out)
{
    if (out->send) {
        Record(sim, &out->command);
    }
    if (out->polling_changed) {
        sim->active = out->polling;
        if (out->polling) {
            ++sim->activations;
        }
        if (!out->polling) {
            // The driver parks the axis and clears the rest property
            sim->axis = 0;
            sim->rest = 0;
        }
    }
    if (out->start_began) {
        ++sim->starts;
    }
    if (out->stop_began && sim->stop_count < 32) {
        sim->stops[sim->stop_count++] = out->stop_began;
    }
}

static void Tick(Sim *sim, uint64_t now)
{
    SDL_RingConOutput out;

    sim->now = now;
    SDL_RingCon_Update(&sim->machine, now, sim->enabled, sim->mcu_wanted, sim->restore_mode, &out);
    Apply(sim, &out);
}

static void Reply(Sim *sim, uint64_t now, const uint8_t *report, size_t length)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    SDL_RingConOutput out;

    if (length) {
        memcpy(copy, report, length);
    }
    sim->now = now;
    SDL_RingCon_OnReply(&sim->machine, copy, length, now, &out);
    free(copy);
    Apply(sim, &out);
}

static void Full(Sim *sim, uint64_t now, const uint8_t *report, size_t length)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    SDL_RingConStrain strain;

    if (length) {
        memcpy(copy, report, length);
    }
    sim->now = now;
    SDL_RingCon_OnFullReport(&sim->machine, copy, length, now, &strain);
    free(copy);
    if (strain.post) {
        sim->axis = strain.value;
        ++sim->axis_posts;
    }
    if (strain.rest_changed) {
        sim->rest = strain.rest;
    }
}

static void MakeReply(uint8_t ack, uint8_t subcommand, uint8_t b15, uint8_t b16, uint8_t report[REPORT_LENGTH])
{
    memset(report, 0, REPORT_LENGTH);
    report[0] = 0x21;
    report[1] = 0x52;
    report[2] = 0x8E;
    report[9] = 0xB6;
    report[10] = 0x67;
    report[11] = 0x74;
    report[12] = 0x09;
    report[13] = ack;
    report[14] = subcommand;
    report[15] = b15;
    report[16] = b16;
}

// The answer a Joy-Con with the Ring-Con gives each command, as the captures show
static void AnswerFor(const Sim *sim, const uint8_t *sent, uint8_t report[REPORT_LENGTH])
{
    switch (sent[10]) {
    case SDL_RINGCON_SUBCMD_SET_MCU_CONFIG:
        MakeReply(0xA0, sent[10], 0x01, 0x00, report);
        break;
    case SDL_RINGCON_SUBCMD_GET_EXTERNAL_DEVICE_INFO:
        MakeReply(0xD9, sent[10], sim->query_answer[0], sim->query_answer[1], report);
        break;
    default:
        MakeReply(0x80, sent[10], 0x00, 0x00, report);
        break;
    }
}

static void MakeFull(int16_t strain, uint8_t report[REPORT_LENGTH])
{
    int i;

    memset(report, 0, REPORT_LENGTH);
    report[0] = 0x30;
    report[1] = 0x11;
    report[2] = 0x8E;
    for (i = 13; i < 37; ++i) {
        report[i] = (uint8_t)(0x40 + i); // two IMU samples
    }
    report[37] = 0x00;
    report[38] = 0x00;
    report[39] = (uint8_t)((uint16_t)strain & 0xFF);
    report[40] = (uint8_t)((uint16_t)strain >> 8);
    report[41] = 0x00;
    report[42] = 0x20;
}

/* Run the clock one millisecond at a time: deliver the replies due, tick,
   and schedule the answer to each new send. */
static void Run(Sim *sim, uint64_t from, uint64_t to)
{
    uint64_t t;
    int i;

    for (t = from; t <= to; ++t) {
        int before;

        for (i = 0; i < sim->pending_count;) {
            if (sim->pending[i].at == t) {
                PendingReply reply = sim->pending[i];
                memmove(&sim->pending[i], &sim->pending[i + 1], (size_t)(sim->pending_count - i - 1) * sizeof(PendingReply));
                --sim->pending_count;
                Reply(sim, t, reply.report, REPORT_LENGTH);
            } else {
                ++i;
            }
        }
        before = sim->sent_count;
        Tick(sim, t);
        for (i = before; i < sim->sent_count; ++i) {
            if (sim->respond && !sim->silent[sim->sent[i][10]] && sim->pending_count < MAX_PENDING) {
                PendingReply *reply = &sim->pending[sim->pending_count++];
                reply->at = t + (uint64_t)sim->delay;
                AnswerFor(sim, sim->sent[i], reply->report);
            }
        }
    }
}

static bool SentIs(const Sim *sim, int index, uint8_t subcommand, const uint8_t *args, size_t length)
{
    uint8_t expected[REPORT_LENGTH];

    if (index < 0 || index >= sim->sent_count) {
        return false;
    }
    memset(expected, 0, sizeof(expected));
    expected[0] = 0x01;
    expected[1] = (uint8_t)(index & 0x0F);
    memcpy(&expected[2], neutral_rumble, sizeof(neutral_rumble));
    expected[10] = subcommand;
    if (length) {
        memcpy(&expected[11], args, length);
    }
    return memcmp(sim->sent[index], expected, REPORT_LENGTH) == 0;
}

static int CountSent(const Sim *sim, uint8_t subcommand)
{
    int i, count = 0;

    for (i = 0; i < sim->sent_count; ++i) {
        if (sim->sent[i][10] == subcommand) {
            ++count;
        }
    }
    return count;
}

static const uint8_t args_mode30[] = { 0x30 };
static const uint8_t args_mode3f[] = { 0x3F };
static const uint8_t args_on[] = { 0x01 };
static const uint8_t args_off[] = { 0x00 };
static const uint8_t args_poll[] = { 0x04, 0x01, 0x01, 0x02 };

static bool IsFormatReset(const Sim *sim, int index)
{
    return index >= 0 && index < sim->sent_count &&
           sim->sent[index][10] == SDL_RINGCON_SUBCMD_SET_EXTERNAL_FORMAT_CONFIG &&
           memcmp(&sim->sent[index][11], &console_format_reset_frame.report[11], 38) == 0;
}

// Open with the hint on and answer everything until polling runs. Returns the time polling began.
static uint64_t StartPolling(Sim *sim, uint64_t t0)
{
    uint64_t t;

    SimOpen(sim, true, t0);
    for (t = t0; t < t0 + 2000 && !sim->active; ++t) {
        Run(sim, t, t);
    }
    return sim->now;
}

/* ------------------------------------------------------------------------ */
/* The commands and the checksum, test 2 */

static void TestCommands(void)
{
    SDL_RingConCommand command;
    uint8_t standby[38];
    uint8_t span[36];

    CHECK(SDL_RingCon_BuildStartCommand(1, &command));
    CHECK(command.subcommand == 0x03 && command.length == 1 && command.args[0] == 0x30);
    CHECK(SDL_RingCon_BuildStartCommand(2, &command));
    CHECK(command.subcommand == 0x22 && command.length == 1 && command.args[0] == 0x01);
    CHECK(SDL_RingCon_BuildStartCommand(3, &command));
    memset(standby, 0, sizeof(standby));
    standby[0] = 0x21;
    standby[1] = 0x01;
    standby[2] = 0x01;
    standby[37] = 0xF3;
    CHECK(command.subcommand == 0x21 && command.length == 38 && memcmp(command.args, standby, 38) == 0);
    CHECK(SDL_RingCon_BuildStartCommand(4, &command));
    CHECK(command.subcommand == 0x59 && command.length == 0);
    CHECK(SDL_RingCon_BuildStartCommand(5, &command));
    CHECK(command.subcommand == 0x5C && command.length == 37 && memcmp(command.args, format_args, 37) == 0);
    CHECK(SDL_RingCon_BuildStartCommand(6, &command));
    CHECK(command.subcommand == 0x5A && command.length == 4 && memcmp(command.args, args_poll, 4) == 0);
    CHECK(!SDL_RingCon_BuildStartCommand(0, &command));
    CHECK(!SDL_RingCon_BuildStartCommand(7, &command));
    CHECK(!SDL_RingCon_BuildStartCommand(1, NULL));

    // Test 2: 01 01, 00 03 and 01 00, each with 34 zero bytes
    memset(span, 0, sizeof(span));
    span[0] = 0x01;
    span[1] = 0x01;
    CHECK(SDL_RingCon_Crc8(span, 36) == 0xF3);
    span[0] = 0x00;
    span[1] = 0x03;
    CHECK(SDL_RingCon_Crc8(span, 36) == 0xFA);
    span[0] = 0x01;
    span[1] = 0x00;
    CHECK(SDL_RingCon_Crc8(span, 36) == 0x58);
    CHECK(SDL_RingCon_Crc8(span, 0) == 0x00);

    /* The Switch's own MCU configs: 21 21 00 03 with FA, 21 21 01 01 with F3
       and 21 21 01 00 with 58 at byte 48, each the CRC of bytes 12-47. */
    CHECK(console_mode3_frame.report[48] == 0xFA);
    CHECK(SDL_RingCon_Crc8(&console_mode3_frame.report[12], 36) == console_mode3_frame.report[48]);
    CHECK(console_standby_frame.report[48] == 0xF3);
    CHECK(SDL_RingCon_Crc8(&console_standby_frame.report[12], 36) == console_standby_frame.report[48]);
    CHECK(console_reset_mcu_frame.report[48] == 0x58);
    CHECK(SDL_RingCon_Crc8(&console_reset_mcu_frame.report[12], 36) == console_reset_mcu_frame.report[48]);
    // The Switch's 21 21 01 01 is the machine's step 3, byte for byte past the header
    CHECK(SDL_RingCon_BuildStartCommand(3, &command));
    CHECK(console_standby_frame.report[10] == command.subcommand);
    CHECK(memcmp(&console_standby_frame.report[11], command.args, 38) == 0);
}

/* ------------------------------------------------------------------------ */
/* Test 1: start-up */

static void TestStartUp(void)
{
    Sim sim;
    uint8_t reply[REPORT_LENGTH];
    uint8_t standby_args[38];
    uint64_t t;
    int i;

    SimOpen(&sim, true, 1000);
    sim.respond = false;
    CHECK(!SDL_RingCon_Engaged(&sim.machine));

    Tick(&sim, 1000);
    CHECK(sim.sent_count == 1 && sim.sent_ms[0] == 1000);
    CHECK(SentIs(&sim, 0, 0x03, args_mode30, 1));
    CHECK(sim.starts == 1);
    CHECK(SDL_RingCon_Engaged(&sim.machine));

    // Step 1 answered at 1030: step 2 at 1080, not before
    MakeReply(0x80, 0x03, 0x00, 0x00, reply);
    for (t = 1001; t < 1080; ++t) {
        if (t == 1030) {
            Reply(&sim, t, reply, REPORT_LENGTH);
        }
        Tick(&sim, t);
    }
    CHECK(sim.sent_count == 1);
    Tick(&sim, 1080);
    CHECK(sim.sent_count == 2 && sim.sent_ms[1] == 1080);
    CHECK(SentIs(&sim, 1, 0x22, args_on, 1));

    MakeReply(0x80, 0x22, 0x00, 0x00, reply);
    Reply(&sim, 1100, reply, REPORT_LENGTH);
    for (t = 1100; t < 1150; ++t) {
        Tick(&sim, t);
    }
    CHECK(sim.sent_count == 2);
    Tick(&sim, 1150);
    memset(standby_args, 0, sizeof(standby_args));
    standby_args[0] = 0x21;
    standby_args[1] = 0x01;
    standby_args[2] = 0x01;
    standby_args[37] = 0xF3;
    CHECK(sim.sent_count == 3 && SentIs(&sim, 2, 0x21, standby_args, 38));
    CHECK(sim.sent[2][48] == 0xF3);

    MakeReply(0xA0, 0x21, 0x01, 0x00, reply);
    Reply(&sim, 1170, reply, REPORT_LENGTH);
    Tick(&sim, 1219);
    CHECK(sim.sent_count == 3);
    Tick(&sim, 1220);
    CHECK(sim.sent_count == 4 && SentIs(&sim, 3, 0x59, NULL, 0));

    MakeReply(0xD9, 0x59, 0x00, 0x20, reply);
    Reply(&sim, 1250, reply, REPORT_LENGTH);
    Tick(&sim, 1299);
    CHECK(sim.sent_count == 4);
    Tick(&sim, 1300);
    CHECK(sim.sent_count == 5 && SentIs(&sim, 4, 0x5C, format_args, 37));

    MakeReply(0x80, 0x5C, 0x00, 0x00, reply);
    Reply(&sim, 1330, reply, REPORT_LENGTH);
    Tick(&sim, 1379);
    CHECK(sim.sent_count == 5);
    Tick(&sim, 1380);
    CHECK(sim.sent_count == 6 && SentIs(&sim, 5, 0x5A, args_poll, 4));
    CHECK(!sim.active);

    MakeReply(0x80, 0x5A, 0x00, 0x00, reply);
    Reply(&sim, 1400, reply, REPORT_LENGTH);
    CHECK(sim.active);
    CHECK(sim.machine.phase == SDL_RINGCON_PHASE_POLLING);

    // Polling sends nothing more
    for (t = 1400; t < 1500; ++t) {
        uint8_t full[REPORT_LENGTH];
        MakeFull(2600, full);
        Full(&sim, t, full, REPORT_LENGTH);
        Tick(&sim, t);
    }
    CHECK(sim.sent_count == 6);
    for (i = 0; i < 6; ++i) {
        CHECK(sim.sent[i][1] == i); // the packet counter from 0
    }

    // The hint off: no start at open
    SimOpen(&sim, false, 1000);
    Run(&sim, 1000, 3000);
    CHECK(sim.sent_count == 0 && !SDL_RingCon_Engaged(&sim.machine));
}

/* ------------------------------------------------------------------------ */
/* Tests 3 and 4: the Ring-Con is absent */

static void CheckAbsent(uint8_t b15, uint8_t b16, uint8_t restore_mode)
{
    Sim sim;
    int first_query, i;
    uint64_t t;

    SimOpen(&sim, true, 5000);
    sim.restore_mode = restore_mode;
    sim.query_answer[0] = b15;
    sim.query_answer[1] = b16;
    Run(&sim, 5000, 15000);

    CHECK(CountSent(&sim, 0x59) == SDL_RINGCON_QUERY_MAX_SENDS);
    first_query = -1;
    for (i = 0; i < sim.sent_count; ++i) {
        if (sim.sent[i][10] == 0x59) {
            first_query = i;
            break;
        }
    }
    CHECK(first_query == 3);
    // Each resend 50 ms after the other ID came back 20 ms after the send
    for (i = first_query + 1; i < first_query + SDL_RINGCON_QUERY_MAX_SENDS; ++i) {
        CHECK(sim.sent[i][10] == 0x59);
        CHECK(sim.sent_ms[i] == sim.sent_ms[i - 1] + 20 + SDL_RINGCON_STEP_GAP_MS);
    }
    // Then 22 00 where the 43rd would go, and the input mode restore after its answer
    i = first_query + SDL_RINGCON_QUERY_MAX_SENDS;
    CHECK(i + 2 == sim.sent_count);
    CHECK(SentIs(&sim, i, 0x22, args_off, 1));
    CHECK(sim.sent_ms[i] == sim.sent_ms[i - 1] + 20 + SDL_RINGCON_STEP_GAP_MS);
    CHECK(SentIs(&sim, i + 1, 0x03, restore_mode == 0x30 ? args_mode30 : args_mode3f, 1));
    CHECK(sim.sent_ms[i + 1] == sim.sent_ms[i] + 20 + SDL_RINGCON_STEP_GAP_MS);
    CHECK(sim.stop_count == 1 && sim.stops[0] == SDL_RINGCON_STOP_ABSENT);
    CHECK(!sim.active && sim.axis == 0 && sim.axis_posts == 0 && sim.rest == 0);
    CHECK(!SDL_RingCon_Engaged(&sim.machine));
    CHECK(sim.machine.format_set == false);

    // No second start without a new trigger
    t = sim.now;
    Run(&sim, t + 1, t + 20000);
    CHECK(sim.starts == 1 && CountSent(&sim, 0x59) == SDL_RINGCON_QUERY_MAX_SENDS);

    // Turning the hint off and on again is a new trigger
    sim.enabled = false;
    Run(&sim, t + 20001, t + 20010);
    sim.enabled = true;
    Run(&sim, t + 20011, t + 20020);
    CHECK(sim.starts == 2);
}

static void TestAbsent(void)
{
    CheckAbsent(0xFE, 0x00, 0x30); // test 3: no device
    CheckAbsent(0xFE, 0x00, 0x3F); // the restore takes the driver's mode
    CheckAbsent(0x00, 0x28, 0x30); // test 4: Starlink
    CheckAbsent(0xFF, 0x00, 0x30); // the Switch's own capture of nothing attached
}

/* ------------------------------------------------------------------------ */
/* Test 5: unanswered commands */

/* Expect one stop command at *index, or 8 copies 600 ms apart when its
   subcommand goes unanswered, and move past it. */
static void ExpectStopCommand(const Sim *sim, int *index, uint8_t subcommand, const uint8_t *args, size_t length, bool reset)
{
    int copies = sim->silent[subcommand] ? SDL_RINGCON_MAX_SENDS : 1;
    int i;

    for (i = 0; i < copies; ++i) {
        if (reset) {
            CHECK(IsFormatReset(sim, *index + i));
        } else {
            CHECK(SentIs(sim, *index + i, subcommand, args, length));
        }
        if (i > 0) {
            CHECK(sim->sent_ms[*index + i] == sim->sent_ms[*index] + (uint64_t)i * SDL_RINGCON_RESEND_MS);
        }
    }
    *index += copies;
}

static void CheckSilentStep(int step)
{
    Sim sim;
    SDL_RingConCommand command;
    uint8_t subcommand;
    int first, i, j;

    CHECK(SDL_RingCon_BuildStartCommand(step, &command));
    subcommand = command.subcommand;
    SimOpen(&sim, true, 1000);
    sim.silent[subcommand] = true;
    Run(&sim, 1000, 20000);

    first = step - 1;
    CHECK(sim.sent_count > first && sim.sent[first][10] == subcommand);
    for (i = 1; i < SDL_RINGCON_MAX_SENDS; ++i) {
        CHECK(sim.sent[first + i][10] == subcommand);
        CHECK(sim.sent_ms[first + i] == sim.sent_ms[first] + (uint64_t)i * SDL_RINGCON_RESEND_MS);
        CHECK(memcmp(&sim.sent[first + i][10], &sim.sent[first][10], REPORT_LENGTH - 10) == 0);
    }
    // 600 ms after the 8th send the stop undoes what was sent, then restores the mode
    j = first + SDL_RINGCON_MAX_SENDS;
    CHECK(sim.sent_ms[j] == sim.sent_ms[first] + (uint64_t)SDL_RINGCON_MAX_SENDS * SDL_RINGCON_RESEND_MS);
    if (step == 6) {
        ExpectStopCommand(&sim, &j, 0x5B, NULL, 0, false);
    }
    if (step >= 5) {
        ExpectStopCommand(&sim, &j, 0x5C, NULL, 0, true);
    }
    ExpectStopCommand(&sim, &j, 0x22, args_off, 1, false);
    ExpectStopCommand(&sim, &j, 0x03, args_mode30, 1, false);
    CHECK(j == sim.sent_count);
    CHECK(sim.stop_count == 1 && sim.stops[0] == SDL_RINGCON_STOP_FAILED);
    CHECK(!sim.active && !SDL_RingCon_Engaged(&sim.machine));
}

static void TestSilence(void)
{
    Sim sim;
    int i, first;

    CheckSilentStep(1);
    CheckSilentStep(2);
    CheckSilentStep(3);
    CheckSilentStep(5);
    CheckSilentStep(6);

    // 0x59 with no report at all: every 150 ms, then the absent path after the 42nd
    SimOpen(&sim, true, 1000);
    sim.silent[0x59] = true;
    Run(&sim, 1000, 12000);
    CHECK(CountSent(&sim, 0x59) == SDL_RINGCON_QUERY_MAX_SENDS);
    first = 3;
    for (i = 1; i < SDL_RINGCON_QUERY_MAX_SENDS; ++i) {
        CHECK(sim.sent_ms[first + i] == sim.sent_ms[first] + (uint64_t)i * SDL_RINGCON_QUERY_SILENCE_MS);
    }
    i = first + SDL_RINGCON_QUERY_MAX_SENDS;
    CHECK(SentIs(&sim, i, 0x22, args_off, 1));
    CHECK(sim.sent_ms[i] == sim.sent_ms[first] + (uint64_t)SDL_RINGCON_QUERY_MAX_SENDS * SDL_RINGCON_QUERY_SILENCE_MS);
    CHECK(SentIs(&sim, i + 1, 0x03, args_mode30, 1));
    CHECK(sim.stops[0] == SDL_RINGCON_STOP_ABSENT);

    // A stop command that is never answered is given up, and the stop goes on
    StartPolling(&sim, 1000);
    CHECK(sim.active);
    sim.silent[0x5B] = true;
    sim.enabled = false;
    first = sim.sent_count;
    Run(&sim, sim.now + 1, sim.now + 8000);
    for (i = 0; i < SDL_RINGCON_MAX_SENDS; ++i) {
        CHECK(sim.sent[first + i][10] == 0x5B);
    }
    CHECK(IsFormatReset(&sim, first + SDL_RINGCON_MAX_SENDS));
    CHECK(SentIs(&sim, first + SDL_RINGCON_MAX_SENDS + 1, 0x22, args_off, 1));
    CHECK(SentIs(&sim, first + SDL_RINGCON_MAX_SENDS + 2, 0x03, args_mode30, 1));
    CHECK(!SDL_RingCon_Engaged(&sim.machine));
    CHECK(sim.machine.polling_set); // 5B was never acknowledged
    CHECK(!sim.machine.format_set);
}

/* ------------------------------------------------------------------------ */
/* Test 6: the strain */

static void TestStrain(void)
{
    Sim sim;
    uint8_t full[REPORT_LENGTH];
    uint64_t t;
    int16_t value;

    t = StartPolling(&sim, 1000);
    CHECK(sim.active && sim.axis == 0 && sim.rest == 0);

    MakeFull(0, full);
    full[39] = 0x34;
    full[40] = 0x0A;
    Full(&sim, ++t, full, REPORT_LENGTH);
    CHECK(sim.axis == 2612 && sim.rest == 2612);
    full[39] = 0x00;
    full[40] = 0x14;
    Full(&sim, ++t, full, REPORT_LENGTH);
    CHECK(sim.axis == 5120 && sim.rest == 2612);
    full[39] = 0x05;
    full[40] = 0x01;
    Full(&sim, ++t, full, REPORT_LENGTH);
    CHECK(sim.axis == 261);
    full[39] = 0xFF;
    full[40] = 0xFF;
    Full(&sim, ++t, full, REPORT_LENGTH);
    CHECK(sim.axis == -1);
    full[39] = 0x00;
    full[40] = 0x00;
    Full(&sim, ++t, full, REPORT_LENGTH);
    CHECK(sim.axis == -1 && sim.axis_posts == 4);
    full[39] = 0x00;
    full[40] = 0x80;
    Full(&sim, ++t, full, REPORT_LENGTH);
    CHECK(sim.axis == -32768);
    full[39] = 0xFF;
    full[40] = 0x7F;
    Full(&sim, ++t, full, REPORT_LENGTH);
    CHECK(sim.axis == 32767 && sim.rest == 2612);

    // Every value round-trips, 0 is never a reading
    for (value = -32768;; ++value) {
        int16_t decoded = 12345;
        MakeFull(value, full);
        if (value == 0) {
            CHECK(!SDL_RingCon_DecodeStrain(full, REPORT_LENGTH, &decoded) && decoded == 12345);
        } else {
            CHECK(SDL_RingCon_DecodeStrain(full, REPORT_LENGTH, &decoded) && decoded == value);
        }
        if (value == 32767) {
            break;
        }
    }
    // Only report 0x30 carries the strain
    MakeFull(2600, full);
    full[0] = 0x31;
    CHECK(!SDL_RingCon_DecodeStrain(full, REPORT_LENGTH, &value));
    full[0] = 0x21;
    CHECK(!SDL_RingCon_DecodeStrain(full, REPORT_LENGTH, &value));
    full[0] = 0x30;
    CHECK(SDL_RingCon_DecodeStrain(full, REPORT_LENGTH, NULL));
    CHECK(!SDL_RingCon_DecodeStrain(NULL, REPORT_LENGTH, &value));

    // Before polling the strain is not read
    SimOpen(&sim, true, 1000);
    Run(&sim, 1000, 1260); // through step 4
    CHECK(sim.machine.phase == SDL_RINGCON_PHASE_START);
    MakeFull(2600, full);
    Full(&sim, 1261, full, REPORT_LENGTH);
    CHECK(sim.axis_posts == 0 && sim.axis == 0);
}

/* ------------------------------------------------------------------------ */
/* Test 7: the IMU samples */

static bool Order(const Sim *sim, int count, int a, int b, int c)
{
    int order[3] = { -1, -1, -1 };
    int n = SDL_RingCon_ImuPostOrder(&sim->machine, order);

    if (n != count || order[0] != a || order[1] != b) {
        return false;
    }
    return count == 2 || order[2] == c;
}

static void TestImu(void)
{
    Sim sim;
    uint64_t t, reset_ack;
    int i, n;

    SimOpen(&sim, true, 1000);
    CHECK(Order(&sim, 3, 2, 1, 0));
    // Steps 1-4 leave the IMU layout alone, the 5C send changes it
    for (t = 1000; t < 3000 && CountSent(&sim, 0x5C) == 0; ++t) {
        Run(&sim, t, t);
        if (CountSent(&sim, 0x5C) == 0) {
            CHECK(Order(&sim, 3, 2, 1, 0));
        }
    }
    CHECK(Order(&sim, 2, 1, 0, -1)); // bytes 25-36 first, then 13-24, none from 37-48
    for (; t < 3000 && !sim.active; ++t) {
        Run(&sim, t, t);
    }
    CHECK(sim.active && Order(&sim, 2, 1, 0, -1));

    // The stop: two samples until it ends, three after
    sim.enabled = false;
    reset_ack = 0;
    for (++t; t < 5000 && SDL_RingCon_Engaged(&sim.machine); ++t) {
        int before = sim.sent_count;
        Run(&sim, t, t);
        if (SDL_RingCon_Engaged(&sim.machine)) {
            CHECK(Order(&sim, 2, 1, 0, -1));
        }
        if (IsFormatReset(&sim, sim.sent_count - 1) && sim.sent_count > before) {
            reset_ack = t + (uint64_t)sim.delay;
        }
    }
    CHECK(reset_ack != 0);
    CHECK(Order(&sim, 3, 2, 1, 0));
    // The layout switched back no earlier than 45 ms after the reset's answer
    CHECK(sim.now >= reset_ack + 45);

    /* The Switch's captures: 37-42 hold the Ring-Con's 00 00, strain, 00 20
       while polling, zeros after 5B, IMU data again after the reset. */
    CHECK(memcmp(&console_polling_before_stop.report[37], "\x00\x00\x43\x0C\x00\x20", 6) == 0);
    for (i = 37; i < 43; ++i) {
        CHECK(console_after_stop_poll.report[i] == 0x00);
    }
    n = 0;
    for (i = 37; i < 43; ++i) {
        n += console_after_format_reset.report[i] != 0x00;
    }
    CHECK(n > 0);
    // The longest lag: bytes 37-42 still zero 45 ms after the answer, IMU data at 48 ms
    for (i = 0; i < 4; ++i) {
        int j, nonzero = 0;
        for (j = 37; j < 43; ++j) {
            nonzero += console_reset_lag[i].report[j] != 0x00;
        }
        CHECK(console_reset_lag[i].report[0] == 0x30);
        if (console_reset_lag[i].ms_after_ack <= 45) {
            CHECK(nonzero == 0);
        } else {
            CHECK(nonzero > 0);
        }
    }
    CHECK(console_reset_lag[2].ms_after_ack == 45 && console_reset_lag[3].ms_after_ack == 48);
    // The machine's stop keeps two samples past that: 22 00 goes out 50 ms after the answer
    CHECK(SDL_RINGCON_STEP_GAP_MS > 45);
}

/* ------------------------------------------------------------------------ */
/* Test 8: truncations */

static void TestTruncation(void)
{
    Sim sim;
    uint8_t full[REPORT_LENGTH];
    uint8_t stale[64];
    uint8_t reply[REPORT_LENGTH];
    uint64_t t;
    size_t length;
    SDL_RingConCommand command;

    t = StartPolling(&sim, 1000);
    MakeFull(2612, full);
    Full(&sim, ++t, full, REPORT_LENGTH);
    CHECK(sim.axis == 2612);
    MakeFull(1000, full);
    for (length = 0; length < SDL_RINGCON_STRAIN_MIN_LENGTH; ++length) {
        Full(&sim, ++t, full, length);
        CHECK(sim.axis == 2612);
    }
    Full(&sim, ++t, full, SDL_RINGCON_STRAIN_MIN_LENGTH);
    CHECK(sim.axis == 1000);
    // A short read into a buffer whose bytes 39-40 hold an older strain
    memset(stale, 0, sizeof(stale));
    memcpy(stale, full, REPORT_LENGTH);
    stale[39] = 0x77;
    stale[40] = 0x07;
    for (length = 0; length < SDL_RINGCON_STRAIN_MIN_LENGTH; ++length) {
        SDL_RingConStrain strain;
        SDL_RingCon_OnFullReport(&sim.machine, stale, length, ++t, &strain);
        CHECK(!strain.post);
    }

    // A reply of 16 bytes or fewer advances nothing, even with the right bytes stale past it
    SimOpen(&sim, true, 1000);
    sim.respond = false;
    Tick(&sim, 1000);
    MakeReply(0x80, 0x03, 0x00, 0x00, reply);
    for (length = 0; length < SDL_RINGCON_REPLY_MIN_LENGTH; ++length) {
        SDL_RingConOutput out;

        Reply(&sim, 1001, reply, length);
        CHECK(!sim.machine.acked);
        SDL_RingCon_OnReply(&sim.machine, reply, length, 1001, &out);
        CHECK(!sim.machine.acked && !out.send && !out.polling_changed);
    }
    Reply(&sim, 1002, reply, SDL_RINGCON_REPLY_MIN_LENGTH);
    CHECK(sim.machine.acked);

    CHECK(SDL_RingCon_BuildStartCommand(4, &command));
    MakeReply(0xD9, 0x59, 0x00, 0x20, reply);
    for (length = 0; length < SDL_RINGCON_REPLY_MIN_LENGTH; ++length) {
        uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
        if (length) {
            memcpy(copy, reply, length);
        }
        CHECK(SDL_RingCon_GateReply(&command, copy, length) == SDL_RINGCON_GATE_NONE);
        free(copy);
    }
    CHECK(SDL_RingCon_GateReply(&command, reply, SDL_RINGCON_REPLY_MIN_LENGTH) == SDL_RINGCON_GATE_MATCH);
    CHECK(SDL_RingCon_GateReply(NULL, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_NONE);
    CHECK(SDL_RingCon_GateReply(&command, NULL, REPORT_LENGTH) == SDL_RINGCON_GATE_NONE);
}

/* ------------------------------------------------------------------------ */
/* The gates, on constructed replies and on the Switch's captured ones */

static void TestGates(void)
{
    SDL_RingConCommand command;
    uint8_t reply[REPORT_LENGTH];
    int step;

    for (step = 1; step <= 6; ++step) {
        CHECK(SDL_RingCon_BuildStartCommand(step, &command));
        // The acknowledge bit is required
        MakeReply(0x00, command.subcommand, 0x00, 0x20, reply);
        CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_NONE);
        MakeReply(0x80, (uint8_t)(command.subcommand + 1), 0x00, 0x20, reply);
        CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_NONE);
        MakeReply(0x80, command.subcommand, 0x00, 0x20, reply);
        reply[0] = 0x30;
        CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_NONE);
    }
    // Steps 1 and 2 take the plain acknowledge only
    CHECK(SDL_RingCon_BuildStartCommand(1, &command));
    MakeReply(0x80, 0x03, 0x00, 0x00, reply);
    CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    reply[13] = 0x81;
    CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_NONE);
    CHECK(SDL_RingCon_BuildStartCommand(2, &command));
    MakeReply(0x80, 0x22, 0x00, 0x00, reply);
    CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    reply[13] = 0xA0;
    CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_NONE);
    // Step 3 any acknowledge with 21
    CHECK(SDL_RingCon_BuildStartCommand(3, &command));
    MakeReply(0xA0, 0x21, 0x01, 0x00, reply);
    CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    // Step 4: 00 20 matches, anything else is another device
    CHECK(SDL_RingCon_BuildStartCommand(4, &command));
    MakeReply(0xD9, 0x59, 0x00, 0x20, reply);
    CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    MakeReply(0xD9, 0x59, 0xFE, 0x00, reply);
    CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_MISMATCH);
    MakeReply(0xD9, 0x59, 0x00, 0x28, reply);
    CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_MISMATCH);
    MakeReply(0xD9, 0x59, 0x20, 0x00, reply);
    CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_MISMATCH);
    MakeReply(0xD9, 0x59, 0xFE, 0x20, reply); // byte 15 counts as well as byte 16
    CHECK(SDL_RingCon_GateReply(&command, reply, REPORT_LENGTH) == SDL_RINGCON_GATE_MISMATCH);

    // The Switch's captured answers, command by command
    CHECK(SDL_RingCon_BuildStartCommand(2, &command));
    CHECK(SDL_RingCon_GateReply(&command, console_resume_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    CHECK(SDL_RingCon_BuildStartCommand(3, &command));
    CHECK(SDL_RingCon_GateReply(&command, console_standby_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    CHECK(SDL_RingCon_BuildStartCommand(4, &command));
    CHECK(SDL_RingCon_GateReply(&command, console_query_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    CHECK(SDL_RingCon_GateReply(&command, console_query_nothing.report, REPORT_LENGTH) == SDL_RINGCON_GATE_MISMATCH);
    CHECK(SDL_RingCon_GateReply(&command, working2_query_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    CHECK(SDL_RingCon_GateReply(&command, console_resume_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_NONE);
    CHECK(SDL_RingCon_BuildStartCommand(5, &command));
    CHECK(SDL_RingCon_GateReply(&command, console_format_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    CHECK(SDL_RingCon_GateReply(&command, working2_format_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    CHECK(SDL_RingCon_BuildStartCommand(6, &command));
    CHECK(SDL_RingCon_GateReply(&command, console_poll_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    CHECK(SDL_RingCon_GateReply(&command, working2_poll_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    CHECK(SDL_RingCon_GateReply(&command, console_format_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_NONE);

    // The stop's commands against the Switch's stop answers
    memset(&command, 0, sizeof(command));
    command.subcommand = SDL_RINGCON_SUBCMD_DISABLE_EXTERNAL_POLLING;
    CHECK(SDL_RingCon_GateReply(&command, console_stop_poll_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    command.subcommand = SDL_RINGCON_SUBCMD_SET_EXTERNAL_FORMAT_CONFIG;
    CHECK(SDL_RingCon_GateReply(&command, console_stop_format_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);
    command.subcommand = SDL_RINGCON_SUBCMD_SET_MCU_STATE;
    command.args[0] = 0x00;
    command.length = 1;
    CHECK(SDL_RingCon_GateReply(&command, console_stop_suspend_reply.report, REPORT_LENGTH) == SDL_RINGCON_GATE_MATCH);

    // The Switch's polling reports decode to the strain they carry
    {
        int16_t value = 0;
        CHECK(SDL_RingCon_DecodeStrain(console_first_strain.report, REPORT_LENGTH, &value) && value == 0x0BF1);
        CHECK(SDL_RingCon_DecodeStrain(console_polling_before_stop.report, REPORT_LENGTH, &value) && value == 0x0C43);
        CHECK(!SDL_RingCon_DecodeStrain(console_after_stop_poll.report, REPORT_LENGTH, &value));
        CHECK(SDL_RingCon_DecodeStrain(working2_strain.report, REPORT_LENGTH, &value) && value == 2558);
    }
}

/* ------------------------------------------------------------------------ */
/* Test 9: the NIR camera and NFC come first */

static void TestPriority(void)
{
    Sim sim;
    uint64_t t;
    int first, i;

    // Opening while NFC holds the MCU starts nothing
    SimOpen(&sim, true, 1000);
    sim.mcu_wanted = true;
    Run(&sim, 1000, 6000);
    CHECK(sim.sent_count == 0 && !SDL_RingCon_Engaged(&sim.machine));
    // The start waits and goes once the MCU is free
    sim.mcu_wanted = false;
    Run(&sim, 6001, 6001);
    CHECK(sim.sent_count == 1 && SentIs(&sim, 0, 0x03, args_mode30, 1));

    // NFC wants the MCU while polling: the stop, 22 00 last, no mode restore
    t = StartPolling(&sim, 1000);
    first = sim.sent_count;
    sim.mcu_wanted = true;
    for (++t; t < 5000 && SDL_RingCon_Engaged(&sim.machine); ++t) {
        Run(&sim, t, t);
    }
    CHECK(!sim.active);
    CHECK(sim.sent_count == first + 3);
    CHECK(SentIs(&sim, first, 0x5B, NULL, 0));
    CHECK(IsFormatReset(&sim, first + 1));
    CHECK(SentIs(&sim, first + 2, 0x22, args_off, 1));
    CHECK(sim.stops[0] == SDL_RINGCON_STOP_YIELD);
    // Not engaged from here: the camera or NFC sends its first command after 22 00
    CHECK(!SDL_RingCon_Engaged(&sim.machine));
    Run(&sim, t, t + 5000);
    CHECK(sim.sent_count == first + 3);
    // The MCU is free again: the Ring-Con starts again
    sim.mcu_wanted = false;
    Run(&sim, t + 5001, t + 5001);
    CHECK(sim.sent_count == first + 4 && sim.sent[first + 3][10] == 0x03);
    CHECK(sim.starts == 2);

    // During start-up, the stop waits for the command in flight
    SimOpen(&sim, true, 1000);
    sim.silent[0x21] = true;
    Run(&sim, 1000, 1200); // step 3 sent, unanswered
    CHECK(CountSent(&sim, 0x21) == 1);
    first = sim.sent_count;
    t = sim.sent_ms[first - 1];
    sim.mcu_wanted = true;
    Run(&sim, 1201, t + SDL_RINGCON_RESEND_MS - 1);
    CHECK(sim.sent_count == first);
    Run(&sim, t + SDL_RINGCON_RESEND_MS, t + SDL_RINGCON_RESEND_MS + 200);
    CHECK(SentIs(&sim, first, 0x22, args_off, 1));
    CHECK(sim.sent_ms[first] == t + SDL_RINGCON_RESEND_MS);
    CHECK(sim.sent_count == first + 1); // no 5B or reset: neither was sent
    CHECK(!SDL_RingCon_Engaged(&sim.machine));

    // The hint turned off while polling: the stop restores the mode
    t = StartPolling(&sim, 1000);
    first = sim.sent_count;
    sim.enabled = false;
    Run(&sim, t + 1, t + 2000);
    CHECK(sim.sent_count == first + 4);
    CHECK(SentIs(&sim, first + 3, 0x03, args_mode30, 1));
    CHECK(sim.stops[0] == SDL_RINGCON_STOP_DISABLED);
    for (i = 0; i < 4; ++i) {
        CHECK(sim.sent_ms[first + i] >= t + 1);
    }

    // The hint turned off during a yield: the stop ends with the restore after all
    t = StartPolling(&sim, 1000);
    first = sim.sent_count;
    sim.mcu_wanted = true;
    Run(&sim, t + 1, t + 30); // 5B out, answered
    CHECK(sim.stops[0] == SDL_RINGCON_STOP_YIELD);
    sim.enabled = false;
    Run(&sim, t + 31, t + 2000);
    CHECK(sim.sent_count == first + 4);
    CHECK(SentIs(&sim, first + 2, 0x22, args_off, 1));
    CHECK(SentIs(&sim, first + 3, 0x03, args_mode30, 1));
    CHECK(!SDL_RingCon_Engaged(&sim.machine) && !sim.machine.pending);

    // The hint turned off while 5A is in flight: its answer starts no polling
    SimOpen(&sim, true, 1000);
    for (t = 1000; t < 2000 && CountSent(&sim, 0x5A) == 0; ++t) {
        Run(&sim, t, t);
    }
    sim.enabled = false;
    Run(&sim, t, t + 2000);
    CHECK(!sim.active && sim.axis == 0 && sim.activations == 0);
    CHECK(sim.stop_count == 1 && sim.stops[0] == SDL_RINGCON_STOP_DISABLED);
    CHECK(sim.sent[sim.sent_count - 1][10] == 0x03);
    CHECK(!SDL_RingCon_Engaged(&sim.machine));
}

/* ------------------------------------------------------------------------ */
/* Test 10: the zero watchdog */

static void RunZeros(Sim *sim, uint64_t from, uint64_t to)
{
    uint8_t full[REPORT_LENGTH];
    uint64_t t;

    MakeFull(0, full);
    for (t = from; t <= to; ++t) {
        if ((t % 15) == 0) {
            Full(sim, t, full, REPORT_LENGTH);
        }
        Run(sim, t, t);
    }
}

static void TestWatchdog(void)
{
    Sim sim;
    uint8_t full[REPORT_LENGTH];
    uint64_t t, limit, u;
    int first;

    // A live strain keeps polling going far past the watchdog's window
    t = StartPolling(&sim, 1000);
    first = sim.sent_count;
    MakeFull(2600, full);
    for (u = t + 1; u <= t + 3 * SDL_RINGCON_WATCHDOG_MS; ++u) {
        if ((u % 15) == 0) {
            Full(&sim, u, full, REPORT_LENGTH);
        }
        Run(&sim, u, u);
    }
    CHECK(sim.active && sim.stop_count == 0 && sim.sent_count == first);
    // Then silence: the watchdog counts from the last nonzero strain
    first = sim.sent_count;
    t = u - 1;
    while ((t % 15) != 0) {
        --t;
    }
    RunZeros(&sim, u, t + SDL_RINGCON_WATCHDOG_MS - 1);
    CHECK(sim.sent_count == first && sim.active);
    RunZeros(&sim, t + SDL_RINGCON_WATCHDOG_MS, t + SDL_RINGCON_WATCHDOG_MS);
    CHECK(sim.sent_count == first + 1 && !sim.active);

    t = StartPolling(&sim, 1000);
    first = sim.sent_count;
    RunZeros(&sim, t + 1, t + SDL_RINGCON_WATCHDOG_MS - 1);
    CHECK(sim.sent_count == first && sim.active);
    RunZeros(&sim, t + SDL_RINGCON_WATCHDOG_MS, t + SDL_RINGCON_WATCHDOG_MS);
    CHECK(sim.sent_count == first + 1 && !sim.active);
    CHECK(sim.stops[0] == SDL_RINGCON_STOP_WATCHDOG);
    // 5B, the reset, 22 00, then step 1 of one fresh start
    RunZeros(&sim, t + SDL_RINGCON_WATCHDOG_MS + 1, t + SDL_RINGCON_WATCHDOG_MS + 500);
    CHECK(SentIs(&sim, first, 0x5B, NULL, 0));
    CHECK(IsFormatReset(&sim, first + 1));
    CHECK(SentIs(&sim, first + 2, 0x22, args_off, 1));
    CHECK(sim.sent[first + 3][10] == 0x03 && sim.sent[first + 3][11] == 0x30);
    CHECK(sim.starts == 2);

    // The fresh start polls again, the strain stays silent: absent, no third start
    limit = sim.now + 2000;
    for (t = sim.now + 1; t < limit && !sim.active; ++t) {
        RunZeros(&sim, t, t);
    }
    CHECK(sim.active);
    first = sim.sent_count;
    RunZeros(&sim, t, t + SDL_RINGCON_WATCHDOG_MS + 1000);
    CHECK(sim.stop_count == 2 && sim.stops[1] == SDL_RINGCON_STOP_ABSENT);
    CHECK(SentIs(&sim, first, 0x5B, NULL, 0));
    CHECK(IsFormatReset(&sim, first + 1));
    CHECK(SentIs(&sim, first + 2, 0x22, args_off, 1));
    CHECK(SentIs(&sim, first + 3, 0x03, args_mode30, 1));
    CHECK(sim.sent_count == first + 4);
    CHECK(!SDL_RingCon_Engaged(&sim.machine) && sim.starts == 2);

    // A strain after the fresh start earns the next watchdog another start
    t = StartPolling(&sim, 1000);
    RunZeros(&sim, t + 1, t + SDL_RINGCON_WATCHDOG_MS + 600);
    CHECK(sim.starts == 2);
    limit = sim.now + 2000;
    for (t = sim.now + 1; t < limit && !sim.active; ++t) {
        Run(&sim, t, t);
    }
    CHECK(sim.active);
    MakeFull(2600, full);
    Full(&sim, t, full, REPORT_LENGTH);
    CHECK(sim.axis == 2600 && sim.rest == 2600);
    RunZeros(&sim, t + 1, t + SDL_RINGCON_WATCHDOG_MS + 600);
    CHECK(sim.stop_count == 2 && sim.stops[1] == SDL_RINGCON_STOP_WATCHDOG);
    CHECK(sim.starts == 3);
    CHECK(sim.axis == 0 && sim.rest == 0);

    // The camera or NFC wants the MCU during the watchdog's stop: no fresh start until it is free
    t = StartPolling(&sim, 1000);
    first = sim.sent_count;
    RunZeros(&sim, t + 1, t + SDL_RINGCON_WATCHDOG_MS + 10);
    CHECK(sim.stops[0] == SDL_RINGCON_STOP_WATCHDOG);
    sim.mcu_wanted = true;
    RunZeros(&sim, t + SDL_RINGCON_WATCHDOG_MS + 11, t + SDL_RINGCON_WATCHDOG_MS + 3000);
    CHECK(sim.sent_count == first + 3); // 5B, the reset, 22 00, and no step 1
    CHECK(SentIs(&sim, first + 2, 0x22, args_off, 1));
    CHECK(!SDL_RingCon_Engaged(&sim.machine) && sim.starts == 1);
    sim.mcu_wanted = false;
    Run(&sim, t + SDL_RINGCON_WATCHDOG_MS + 3001, t + SDL_RINGCON_WATCHDOG_MS + 3001);
    CHECK(sim.starts == 2 && sim.sent[sim.sent_count - 1][10] == 0x03);
}

/* ------------------------------------------------------------------------ */
/* Test 11: close */

static void TestClose(void)
{
    Sim sim;
    SDL_RingConCommand commands[3];
    uint64_t t;
    int n;

    SimOpen(&sim, true, 1000);
    CHECK(SDL_RingCon_BuildCloseCommands(&sim.machine, commands) == 0);
    Tick(&sim, 1000);
    n = SDL_RingCon_BuildCloseCommands(&sim.machine, commands);
    CHECK(n == 1 && commands[0].subcommand == 0x22 && commands[0].args[0] == 0x00 && commands[0].length == 1);

    // Up to step 5's send: 22 00 alone. From it: the reset too.
    for (t = 1001; t < 2000 && CountSent(&sim, 0x5C) == 0; ++t) {
        Run(&sim, t, t);
        if (CountSent(&sim, 0x5C) == 0) {
            CHECK(SDL_RingCon_BuildCloseCommands(&sim.machine, commands) == 1);
        }
    }
    n = SDL_RingCon_BuildCloseCommands(&sim.machine, commands);
    CHECK(n == 2 && commands[0].subcommand == 0x5C && commands[0].args[0] == 0x00);
    CHECK(memcmp(commands[0].args, &console_format_reset_frame.report[11], 38) == 0 && commands[0].length == 38);
    CHECK(commands[1].subcommand == 0x22 && commands[1].args[0] == 0x00);

    t = StartPolling(&sim, 1000);
    n = SDL_RingCon_BuildCloseCommands(&sim.machine, commands);
    CHECK(n == 3);
    CHECK(commands[0].subcommand == 0x5B && commands[0].length == 0);
    CHECK(commands[1].subcommand == 0x5C && commands[1].args[0] == 0x00);
    CHECK(commands[2].subcommand == 0x22 && commands[2].args[0] == 0x00);

    // After a finished stop there is nothing left to undo
    sim.enabled = false;
    Run(&sim, t + 1, t + 2000);
    CHECK(!SDL_RingCon_Engaged(&sim.machine));
    CHECK(SDL_RingCon_BuildCloseCommands(&sim.machine, commands) == 0);
}

/* ------------------------------------------------------------------------ */
/* Test 12: the axis count */

static void TestAxisCount(void)
{
    int naxes;

    naxes = SDL_RingCon_AxisCount(true, 0, GAMEPAD_AXES);
    CHECK(naxes == 8);
    naxes = SDL_RingCon_AxisCount(false, naxes, GAMEPAD_AXES);
    CHECK(naxes == 8); // the left half opens second
    naxes = SDL_RingCon_AxisCount(false, 0, GAMEPAD_AXES);
    CHECK(naxes == 6);
    naxes = SDL_RingCon_AxisCount(true, naxes, GAMEPAD_AXES);
    CHECK(naxes == 8); // the right half opens second
    CHECK(SDL_RingCon_AxisCount(false, 0, GAMEPAD_AXES) == 6);
    CHECK(GAMEPAD_AXES + 1 < SDL_RingCon_AxisCount(true, 0, GAMEPAD_AXES)); // axis 7 exists
}

/* ------------------------------------------------------------------------ */
/* Test 13: a stray reply during step 5 */

static void TestStrayReply(void)
{
    Sim sim;
    uint8_t reply[REPORT_LENGTH];
    uint64_t t, sent;
    int first;

    SimOpen(&sim, true, 1000);
    sim.silent[0x5C] = true;
    for (t = 1000; t < 2000 && CountSent(&sim, 0x5C) == 0; ++t) {
        Run(&sim, t, t);
    }
    first = sim.sent_count - 1;
    sent = sim.sent_ms[first];
    Run(&sim, sent + 1, sent + 19);
    MakeReply(0x80, 0x48, 0x00, 0x00, reply);
    Reply(&sim, sent + 20, reply, REPORT_LENGTH);
    CHECK(!sim.machine.acked);
    Run(&sim, sent + 20, sent + SDL_RINGCON_RESEND_MS - 1);
    CHECK(sim.sent_count == first + 1);
    Run(&sim, sent + SDL_RINGCON_RESEND_MS, sent + SDL_RINGCON_RESEND_MS);
    CHECK(sim.sent_count == first + 2 && sim.sent[first + 1][10] == 0x5C);
}

/* ------------------------------------------------------------------------ */
/* Test 14: Ringcon-Driver's hardware log */

static void TestWorking2(void)
{
    Sim sim;
    uint64_t t, query_sent, next;
    size_t i;
    int queries, first;

    SimOpen(&sim, true, 1000);
    sim.respond = true;
    sim.silent[0x59] = true;
    for (t = 1000; t < 2000 && CountSent(&sim, 0x59) == 0; ++t) {
        Run(&sim, t, t);
    }
    query_sent = sim.sent_ms[sim.sent_count - 1];
    CHECK(sim.machine.phase == SDL_RINGCON_PHASE_START);

    /* The logged reports at 60 Hz: 161 of type 0x30 and one late 21 21
       answer, none of them an answer to 0x59. */
    t = query_sent;
    for (i = 0; i < sizeof(working2_silence) / sizeof(working2_silence[0]); ++i) {
        next = query_sent + ((uint64_t)(i + 1) * 1000) / 60;
        Run(&sim, t + 1, next);
        t = next;
        if (working2_silence[i].report[0] == 0x21) {
            Reply(&sim, t, working2_silence[i].report, REPORT_LENGTH);
        } else {
            Full(&sim, t, working2_silence[i].report, REPORT_LENGTH);
        }
    }
    queries = CountSent(&sim, 0x59);
    // 0x59 again every 150 ms of silence, well short of the 42 that would mean absent
    CHECK(sizeof(working2_silence) / sizeof(working2_silence[0]) == 162);
    CHECK(queries == 1 + (int)((t - query_sent) / SDL_RINGCON_QUERY_SILENCE_MS));
    CHECK(queries < SDL_RINGCON_QUERY_MAX_SENDS);
    CHECK(sim.machine.phase == SDL_RINGCON_PHASE_START && sim.stop_count == 0);
    CHECK(sim.axis_posts == 0);

    // Then the logged answer, D9 59 00 20: step 5 50 ms later
    sim.respond = false;
    first = sim.sent_count;
    Reply(&sim, t + 1, working2_query_reply.report, REPORT_LENGTH);
    Run(&sim, t + 1, t + 1 + SDL_RINGCON_STEP_GAP_MS - 1);
    CHECK(sim.sent_count == first);
    Run(&sim, t + 1 + SDL_RINGCON_STEP_GAP_MS, t + 1 + SDL_RINGCON_STEP_GAP_MS);
    CHECK(sim.sent_count == first + 1 && sim.sent[first][10] == 0x5C);

    // The logged 80 5C and 80 5A finish steps 5 and 6
    t = sim.now;
    Run(&sim, t + 1, t + 29);
    Reply(&sim, t + 30, working2_format_reply.report, REPORT_LENGTH);
    Run(&sim, t + 30, t + 30 + SDL_RINGCON_STEP_GAP_MS);
    CHECK(sim.sent[sim.sent_count - 1][10] == 0x5A);
    CHECK(sim.sent_ms[sim.sent_count - 1] == t + 30 + SDL_RINGCON_STEP_GAP_MS);
    t = sim.now;
    Run(&sim, t + 1, t + 29);
    Reply(&sim, t + 30, working2_poll_reply.report, REPORT_LENGTH);
    CHECK(sim.active);

    // The logged report with 00 00 FE 09 00 20 at bytes 37-42
    Full(&sim, t + 40, working2_strain.report, REPORT_LENGTH);
    CHECK(sim.axis == 2558 && sim.rest == 2558);
}

/* ------------------------------------------------------------------------ */
/* The Switch's own session through the machine */

static void TestConsoleSession(void)
{
    Sim sim;
    uint8_t reply[REPORT_LENGTH];
    const RingConCapture *answers[5];
    uint64_t t;
    int step;

    answers[0] = &console_resume_reply;
    answers[1] = &console_standby_reply;
    answers[2] = &console_query_reply;
    answers[3] = &console_format_reply;
    answers[4] = &console_poll_reply;

    SimOpen(&sim, true, 1000);
    sim.respond = false;
    Tick(&sim, 1000);
    MakeReply(0x80, 0x03, 0x00, 0x00, reply);
    Reply(&sim, 1020, reply, REPORT_LENGTH);
    t = 1020;
    for (step = 2; step <= 6; ++step) {
        Run(&sim, t + 1, t + SDL_RINGCON_STEP_GAP_MS);
        CHECK(sim.sent_count == step);
        t = sim.now + 30;
        Reply(&sim, t, answers[step - 2]->report, REPORT_LENGTH);
    }
    CHECK(sim.active);
    Full(&sim, t + 10, console_first_strain.report, REPORT_LENGTH);
    CHECK(sim.axis == 0x0BF1 && sim.rest == 0x0BF1);
    Full(&sim, t + 25, console_polling_before_stop.report, REPORT_LENGTH);
    CHECK(sim.axis == 0x0C43 && sim.rest == 0x0BF1);

    // The Switch's stop answers drive the machine's stop
    sim.enabled = false;
    Run(&sim, t + 26, t + 26);
    CHECK(sim.sent[sim.sent_count - 1][10] == 0x5B && !sim.active && sim.axis == 0);
    Run(&sim, t + 27, t + 49);
    Reply(&sim, t + 50, console_stop_poll_reply.report, REPORT_LENGTH);
    Run(&sim, t + 50, t + 59);
    Full(&sim, t + 60, console_after_stop_poll.report, REPORT_LENGTH);
    CHECK(sim.axis_posts == 2);
    Run(&sim, t + 60, t + 100);
    CHECK(IsFormatReset(&sim, sim.sent_count - 1));
    CHECK(sim.sent_ms[sim.sent_count - 1] == t + 100);
    Run(&sim, t + 101, t + 129);
    Reply(&sim, t + 130, console_stop_format_reply.report, REPORT_LENGTH);
    Run(&sim, t + 130, t + 180);
    CHECK(SentIs(&sim, sim.sent_count - 1, 0x22, args_off, 1));
    CHECK(Order(&sim, 2, 1, 0, -1));
    Run(&sim, t + 181, t + 199);
    Reply(&sim, t + 200, console_stop_suspend_reply.report, REPORT_LENGTH);
    Run(&sim, t + 200, t + 250);
    CHECK(SentIs(&sim, sim.sent_count - 1, 0x03, args_mode30, 1));
    CHECK(Order(&sim, 2, 1, 0, -1)); // the stop ends when the restore is answered
}

/* ------------------------------------------------------------------------ */

static void TestArguments(void)
{
    SDL_RingConMachine machine;
    int order[3];

    SDL_RingCon_Init(&machine);
    CHECK(!SDL_RingCon_Engaged(&machine));
    CHECK(SDL_RingCon_ImuPostOrder(&machine, order) == 3);
    CHECK(SDL_RingCon_ImuPostOrder(NULL, order) == 3 && order[0] == 2 && order[2] == 0);
    SDL_RingCon_Open(&machine, false);
    CHECK(!machine.pending && !machine.enabled);
    SDL_RingCon_Open(&machine, true);
    CHECK(machine.pending && machine.enabled);
    CHECK(strcmp(SDL_RINGCON_PROP_ACTIVE, "SDL.joystick.switch.ringcon") == 0);
    CHECK(strcmp(SDL_RINGCON_PROP_REST, "SDL.joystick.switch.ringcon_rest") == 0);
}

int main(void)
{
    TestCommands();
    TestStartUp();
    TestAbsent();
    TestSilence();
    TestStrain();
    TestImu();
    TestTruncation();
    TestGates();
    TestPriority();
    TestWatchdog();
    TestClose();
    TestAxisCount();
    TestStrayReply();
    TestWorking2();
    TestConsoleSession();
    TestArguments();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
