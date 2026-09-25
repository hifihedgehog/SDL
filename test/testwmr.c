/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_wmr_proto.c, the Windows
   Mixed Reality motion controllers of hifihedgehog/SDL#33 Part 7. All are
   constructed from Monado (wmr_controller_base.c, wmr_controller_og.c,
   wmr_controller_hp.c, wmr_config.c, u_json.c) and from cJSON 1.7.18, which
   Monado builds. Tests 1 to 8 are the ticket's. */

#include "../src/joystick/hidapi/SDL_hidapi_wmr_proto.h"

#include <math.h>
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

#define MAX_WRITES 1024

typedef struct Tracker
{
    int writes;
    int fail_at; /* The write that fails, or -1 */
    uint8_t last[SDL_WMR_COMMAND_LENGTH];
    uint8_t log[MAX_WRITES][SDL_WMR_COMMAND_LENGTH];
} Tracker;

static Tracker tracker;
static uint8_t config_buffer[SDL_WMR_BLOCK_MAXIMUM];

static bool TestWrite(void *userdata, const uint8_t *data, size_t length)
{
    Tracker *t = (Tracker *)userdata;
    const int index = t->writes++;

    CHECK(length == SDL_WMR_COMMAND_LENGTH);
    if (length == SDL_WMR_COMMAND_LENGTH) {
        memcpy(t->last, data, length);
        if (index < MAX_WRITES) {
            memcpy(t->log[index], data, length);
        }
    }
    return index != t->fail_at;
}

static const SDL_WMRSink sink = { &tracker, TestWrite };

static void ResetTracker(void)
{
    memset(&tracker, 0, sizeof(tracker));
    tracker.fail_at = -1;
}

static uint32_t Read32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/* A command: these bytes, then zeros to 64 */
static bool CommandIs(const uint8_t *command, const uint8_t *expected, size_t count)
{
    size_t i;

    if (memcmp(command, expected, count) != 0) {
        return false;
    }
    for (i = count; i < SDL_WMR_COMMAND_LENGTH; ++i) {
        if (command[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Hands a report over in a heap block of exactly its length */
static SDL_WMRReport Feed(SDL_WMRSession *session, uint64_t now, const uint8_t *report, size_t length, SDL_WMREvent *event)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
    SDL_WMRReport result;

    if (length) {
        memcpy(copy, report, length);
    }
    result = SDL_WMR_HandleReport(session, now, copy, length, &sink, event);
    free(copy);
    return result;
}

static void Answer(uint8_t out[SDL_WMR_REPLY_LENGTH], uint8_t code, uint8_t echo, uint8_t block,
                   uint32_t remain, uint8_t count, const uint8_t *data)
{
    memset(out, 0, SDL_WMR_REPLY_LENGTH);
    out[0] = code;
    out[2] = echo;
    out[4] = block;
    out[5] = (uint8_t)(remain & 0xFF);
    out[6] = (uint8_t)((remain >> 8) & 0xFF);
    out[7] = (uint8_t)((remain >> 16) & 0xFF);
    out[8] = (uint8_t)((remain >> 24) & 0xFF);
    out[9] = count;
    if (data) {
        memcpy(&out[10], data, count > SDL_WMR_REPLY_DATA_LENGTH ? SDL_WMR_REPLY_DATA_LENGTH : count);
    }
}

/* A controller as Monado reads it: the reset and quiesce answered with code
 * 0x06, a block read with 0xFFFFFFFF as the start and a follow-up with the
 * count still to come, each answered with up to 68 bytes */
typedef struct Device
{
    const uint8_t *blocks[4];
    uint32_t sizes[4];
    size_t padding; /* Bytes of 0xEE after each answer, as hid.dll delivers a longer collection */
} Device;

static size_t DeviceAnswer(const Device *device, const uint8_t *command, uint8_t *answer)
{
    size_t length = SDL_WMR_REPLY_LENGTH;

    if (command[1] == 0x00 || command[1] == 0x04) {
        Answer(answer, 0x06, command[1], command[2], 0, 0, NULL);
    } else if (command[1] == 0x02 && command[2] < 4) {
        const uint8_t block = command[2];
        const uint32_t address = Read32(&command[3]);
        const uint32_t size = device->sizes[block];
        const uint32_t offset = (address == 0xFFFFFFFFu) ? 0 : size - address;
        const uint32_t chunk = (size - offset) < 68 ? size - offset : 68;

        Answer(answer, 0x02, 0x02, block, size - offset - chunk, (uint8_t)chunk, device->blocks[block] + offset);
    } else {
        return 0;
    }
    memset(answer + length, 0xEE, device->padding);
    return length + device->padding;
}

/* Answers each command 5 ms after it. A follow-up falls due 10 ms after its
 * answer. Stops when the start-up ends or when stop_block's first read is
 * awaited. */
static void Run(SDL_WMRSession *session, const Device *device, uint64_t *now, int stop_block)
{
    uint8_t answer[SDL_WMR_REPLY_LENGTH + 64];
    SDL_WMREvent event;
    int guard;

    for (guard = 0; guard < 10000; ++guard) {
        if (session->phase == SDL_WMR_PHASE_WAITING) {
            size_t length;

            if (stop_block >= 0 && tracker.last[1] == 0x02 && tracker.last[2] == stop_block &&
                Read32(&tracker.last[3]) == 0xFFFFFFFFu) {
                return;
            }
            length = DeviceAnswer(device, tracker.last, answer);
            *now += 5;
            CHECK(Feed(session, *now, answer, length, &event) == SDL_WMR_REPORT_STARTUP);
            SDL_WMR_Update(session, *now, &sink);
        } else if (session->phase == SDL_WMR_PHASE_DELAY) {
            *now += SDL_WMR_FOLLOWUP_DELAY_MS;
            SDL_WMR_Update(session, *now, &sink);
        } else {
            return;
        }
    }
    ++checks;
    ++failures;
    printf("FAILED %s:%d: the start-up did not end\n", __FILE__, __LINE__);
}

/* The realistic configuration: an accelerometer turned 90 degrees about Z
 * with a mix and a bias, a gyroscope with a bias, and a magnetometer. Every
 * temperature coefficient past the constant terms is 0.5, which no reading
 * of the right index takes. */
static const char config_json[] =
    "{\n"
    "  \"CalibrationInformation\": {\n"
    "    \"ControllerLeds\": [\n"
    "      { \"Position\": [0.0101, -0.0202, 0.0303], \"Normal\": [0.0, 0.0, 1.0] },\n"
    "      { \"Position\": [-0.0101, 0.0202, -0.0303], \"Normal\": [0.0, 1.0, 0.0] }\n"
    "    ],\n"
    "    \"InertialSensors\": [\n"
    "      {\n"
    "        \"BiasTemperatureModel\": [0.1, 0.5, 0.5, 0.5, -0.2, 0.5, 0.5, 0.5, 0.3, 0.5, 0.5, 0.5],\n"
    "        \"BiasUncertainty\": [0.01, 0.01, 0.01],\n"
    "        \"MixingMatrixTemperatureModel\": [1.01, 0.5, 0.5, 0.5, 0.0, 0.5, 0.5, 0.5, 0.0, 0.5, 0.5, 0.5,\n"
    "                                         0.0, 0.5, 0.5, 0.5, 0.99, 0.5, 0.5, 0.5, 0.0, 0.5, 0.5, 0.5,\n"
    "                                         0.0, 0.5, 0.5, 0.5, 0.0, 0.5, 0.5, 0.5, 1.0, 0.5, 0.5, 0.5],\n"
    "        \"ModelTypeMask\": 16,\n"
    "        \"Noise\": [0.00095, 0.00095, 0.00095, 0.0, 0.0, 0.0],\n"
    "        \"Rt\": { \"Rotation\": [0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0],\n"
    "                \"Translation\": [0.001, -0.002, 0.003] },\n"
    "        \"SecondOrderScaling\": [0, 0, 0, 0, 0, 0, 0, 0, 0],\n"
    "        \"SensorType\": \"CALIBRATION_InertialSensorType_Accelerometer\",\n"
    "        \"TemperatureBounds\": [5.0, 60.0],\n"
    "        \"TemperatureC\": 0.0\n"
    "      },\n"
    "      {\n"
    "        \"BiasTemperatureModel\": [0.01, 0.5, 0.5, 0.5, 0.02, 0.5, 0.5, 0.5, 0.03, 0.5, 0.5, 0.5],\n"
    "        \"BiasUncertainty\": [0.0001, 0.0001, 0.0001],\n"
    "        \"MixingMatrixTemperatureModel\": [1.0, 0.5, 0.5, 0.5, 0.0, 0.5, 0.5, 0.5, 0.0, 0.5, 0.5, 0.5,\n"
    "                                         0.0, 0.5, 0.5, 0.5, 1.0, 0.5, 0.5, 0.5, 0.0, 0.5, 0.5, 0.5,\n"
    "                                         0.0, 0.5, 0.5, 0.5, 0.0, 0.5, 0.5, 0.5, 1.0, 0.5, 0.5, 0.5],\n"
    "        \"ModelTypeMask\": 16,\n"
    "        \"Noise\": [0.0012, 0.0012, 0.0012, 0.0, 0.0, 0.0],\n"
    "        \"Rt\": { \"Rotation\": [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0],\n"
    "                \"Translation\": [0.0, 0.0, 0.0] },\n"
    "        \"SensorType\": \"CALIBRATION_InertialSensorType_Gyro\",\n"
    "        \"TemperatureBounds\": [5.0, 60.0]\n"
    "      },\n"
    "      {\n"
    "        \"BiasTemperatureModel\": [7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7],\n"
    "        \"BiasUncertainty\": [1, 1, 1],\n"
    "        \"MixingMatrixTemperatureModel\": [7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,\n"
    "                                         7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7],\n"
    "        \"Noise\": [1, 1, 1, 1, 1, 1],\n"
    "        \"Rt\": { \"Rotation\": [7, 7, 7, 7, 7, 7, 7, 7, 7], \"Translation\": [0, 0, 0] },\n"
    "        \"SensorType\": \"CALIBRATION_InertialSensorType_Magnetometer\"\n"
    "      }\n"
    "    ]\n"
    "  }\n"
    "}\n";

static uint8_t block0[0x40];
static uint8_t block3[0xA0];
static uint8_t block2[2 + sizeof(config_json)];

/* Block 0 with firmware revision 0x12345678 and calibration size 0x0ABC,
   block 3 with serial SERIAL0123456789, block 2 with the key applied */
static void MakeDevice(Device *device)
{
    size_t i;

    memset(block0, 0x11, sizeof(block0));
    block0[0x14] = 0x78;
    block0[0x15] = 0x56;
    block0[0x16] = 0x34;
    block0[0x17] = 0x12;
    block0[0x34] = 0xBC;
    block0[0x35] = 0x0A;
    memset(block3, 0x33, sizeof(block3));
    memcpy(&block3[0x84], "SERIAL0123456789", 16);
    block2[0] = 0x34;
    block2[1] = 0x12;
    for (i = 0; i < sizeof(config_json) - 1; ++i) {
        block2[2 + i] = (uint8_t)((uint8_t)config_json[i] ^ SDL_WMR_ConfigKey[i % SDL_WMR_KEY_LENGTH]);
    }
    memset(device, 0, sizeof(*device));
    device->blocks[0] = block0;
    device->sizes[0] = sizeof(block0);
    device->blocks[3] = block3;
    device->sizes[3] = sizeof(block3);
    device->blocks[2] = block2;
    device->sizes[2] = (uint32_t)(2 + sizeof(config_json) - 1);
}

static bool Near(float a, float b, float tolerance)
{
    return fabsf(a - b) <= tolerance;
}

static void MakeReady(SDL_WMRSession *session, SDL_WMRModel model, bool padded, uint64_t *now)
{
    Device device;

    MakeDevice(&device);
    ResetTracker();
    SDL_WMR_Open(session, model, padded, config_buffer, sizeof(config_buffer), *now, &sink);
    Run(session, &device, now, -1);
    CHECK(session->phase == SDL_WMR_PHASE_READY);
}

static void TestIdentity(void)
{
    SDL_WMRModel model = SDL_WMR_MODEL_REVERB_G2;

    CHECK(SDL_WMR_IsControllerID(0x045E, 0x065B, &model) && model == SDL_WMR_MODEL_FIRST_GENERATION);
    model = SDL_WMR_MODEL_REVERB_G2;
    CHECK(SDL_WMR_IsControllerID(0x045E, 0x065D, &model) && model == SDL_WMR_MODEL_FIRST_GENERATION);
    CHECK(SDL_WMR_IsControllerID(0x045E, 0x066A, &model) && model == SDL_WMR_MODEL_REVERB_G2);
    CHECK(SDL_WMR_IsControllerID(0x045E, 0x065B, NULL));
    CHECK(!SDL_WMR_IsControllerID(0x045E, 0x065C, &model) && !SDL_WMR_IsControllerID(0x045E, 0x0659, &model));
    CHECK(!SDL_WMR_IsControllerID(0x03F0, 0x066A, &model) && !SDL_WMR_IsControllerID(0x04E8, 0x065D, &model));

    CHECK(SDL_WMR_GetHand("Motion controller - Left") == SDL_WMR_HAND_LEFT);
    CHECK(SDL_WMR_GetHand("Motion controller - Right") == SDL_WMR_HAND_RIGHT);
    CHECK(SDL_WMR_GetHand("Motion controller - Left ") == SDL_WMR_HAND_NONE);
    CHECK(SDL_WMR_GetHand("motion controller - left") == SDL_WMR_HAND_NONE);
    CHECK(SDL_WMR_GetHand("Motion controller") == SDL_WMR_HAND_NONE);
    CHECK(SDL_WMR_GetHand("") == SDL_WMR_HAND_NONE && SDL_WMR_GetHand(NULL) == SDL_WMR_HAND_NONE);
}

static void TestKey(void)
{
    uint32_t hash = 0x811C9DC5u;
    uint8_t data[3000];
    size_t i;

    /* FNV-1a of Monado's wmr_config_key.h bytes */
    for (i = 0; i < SDL_WMR_KEY_LENGTH; ++i) {
        hash ^= SDL_WMR_ConfigKey[i];
        hash *= 0x01000193u;
    }
    CHECK(hash == 0xBA7932E0u);
    CHECK(SDL_WMR_ConfigKey[0] == 0x2F && SDL_WMR_ConfigKey[1] == 0xC8 && SDL_WMR_ConfigKey[500] == 0x15);
    CHECK(SDL_WMR_ConfigKey[1022] == 0xA3 && SDL_WMR_ConfigKey[1023] == 0xA7);

    /* The key repeats every 1024 bytes and undoes itself */
    memset(data, 0, sizeof(data));
    SDL_WMR_Deobfuscate(data, sizeof(data));
    CHECK(data[0] == 0x2F && data[1024] == 0x2F && data[2048] == 0x2F && data[1023] == 0xA7 && data[2047] == 0xA7);
    CHECK(data[2999] == SDL_WMR_ConfigKey[2999 % 1024]);
    SDL_WMR_Deobfuscate(data, sizeof(data));
    for (i = 0; i < sizeof(data) && data[i] == 0; ++i) {
    }
    CHECK(i == sizeof(data));
}

/* 1: the four command groups in order, each moving on only after its
   answer, and the answers the start-up refuses */
static void TestStartup(void)
{
    static const uint8_t reset[] = { 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t quiesce[] = { 0x06, 0x04, 0xC1, 0x02, 0x00, 0x00, 0x00 };
    static const uint8_t read0[] = { 0x06, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t read3[] = { 0x06, 0x02, 0x03, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t read3_92[] = { 0x06, 0x02, 0x03, 0x5C, 0x00, 0x00, 0x00 };
    static const uint8_t read3_24[] = { 0x06, 0x02, 0x03, 0x18, 0x00, 0x00, 0x00 };
    static const uint8_t read2[] = { 0x06, 0x02, 0x02, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t status_on[] = { 0x06, 0x03, 0x01, 0x00, 0x02 };
    static const uint8_t imu_on[] = { 0x06, 0x03, 0x02, 0xE1, 0x02 };
    SDL_WMRSession session;
    SDL_WMREvent event;
    Device device;
    uint8_t answer[SDL_WMR_REPLY_LENGTH + 1];
    uint8_t status[SDL_WMR_STATUS_LENGTH];
    uint64_t now = 1000;
    uint32_t remaining, expected_follow_ups;
    int i, index;

    MakeDevice(&device);
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    CHECK(tracker.writes == 1 && CommandIs(tracker.log[0], reset, sizeof(reset)));
    CHECK(session.phase == SDL_WMR_PHASE_WAITING && session.deadline_ms == 1250);

    /* A status report and an answer with the other code are skipped */
    memset(status, 0, sizeof(status));
    status[0] = 0x01;
    CHECK(Feed(&session, now + 1, status, sizeof(status), &event) == SDL_WMR_REPORT_IGNORED);
    Answer(answer, 0x02, 0x00, 0x00, 0, 0, NULL);
    CHECK(Feed(&session, now + 2, answer, SDL_WMR_REPLY_LENGTH, &event) == SDL_WMR_REPORT_IGNORED);
    CHECK(Feed(&session, now + 2, answer, 0, &event) == SDL_WMR_REPORT_IGNORED);
    CHECK(SDL_WMR_HandleReport(&session, now + 2, NULL, 78, &sink, &event) == SDL_WMR_REPORT_IGNORED);
    CHECK(tracker.writes == 1 && session.phase == SDL_WMR_PHASE_WAITING);

    /* The reset's answer brings the quiesce, whose answer brings block 0 */
    Answer(answer, 0x06, 0x00, 0x00, 0, 0, NULL);
    CHECK(Feed(&session, now + 3, answer, SDL_WMR_REPLY_LENGTH, &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(tracker.writes == 2 && CommandIs(tracker.log[1], quiesce, sizeof(quiesce)));
    CHECK(session.deadline_ms == now + 3 + 250);
    Answer(answer, 0x06, 0x04, 0x00, 0, 0, NULL);
    CHECK(Feed(&session, now + 4, answer, SDL_WMR_REPLY_LENGTH, &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(tracker.writes == 3 && CommandIs(tracker.log[2], read0, sizeof(read0)));

    /* The rest from the device */
    now += 4;
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_READY && session.failure == SDL_WMR_FAILURE_NONE);

    /* Block 0 fits one answer. Block 3's 160 bytes take follow-ups for 92
       and 24, each sent right after the block before it ends. */
    CHECK(CommandIs(tracker.log[3], read3, sizeof(read3)));
    CHECK(CommandIs(tracker.log[4], read3_92, sizeof(read3_92)));
    CHECK(CommandIs(tracker.log[5], read3_24, sizeof(read3_24)));
    CHECK(CommandIs(tracker.log[6], read2, sizeof(read2)));
    remaining = device.sizes[2] - 68;
    expected_follow_ups = (device.sizes[2] + 67) / 68 - 1;
    index = 7;
    for (i = 0; i < (int)expected_follow_ups; ++i, ++index) {
        uint8_t follow_up[7] = { 0x06, 0x02, 0x02, 0, 0, 0, 0 };

        follow_up[3] = (uint8_t)(remaining & 0xFF);
        follow_up[4] = (uint8_t)((remaining >> 8) & 0xFF);
        CHECK(CommandIs(tracker.log[index], follow_up, sizeof(follow_up)));
        remaining -= remaining < 68 ? remaining : 68;
    }
    CHECK(remaining == 0);
    CHECK(CommandIs(tracker.log[index], status_on, sizeof(status_on)));
    CHECK(CommandIs(tracker.log[index + 1], imu_on, sizeof(imu_on)));
    CHECK(tracker.writes == index + 2);

    /* What the blocks gave */
    CHECK(session.firmware_revision == 0x12345678u && session.calibration_size == 0x0ABC);
    CHECK(strcmp(session.serial, "SERIAL0123456789") == 0);
    CHECK(Near(session.accel.rotation[0][1], -1.0f, 0) && Near(session.accel.rotation[1][0], 1.0f, 0));
    CHECK(Near(session.accel.mix[0][0], 1.01f, 0) && Near(session.accel.mix[1][1], 0.99f, 0));
    CHECK(Near(session.accel.bias[0], 0.1f, 0) && Near(session.accel.bias[1], -0.2f, 0) && Near(session.accel.bias[2], 0.3f, 0));
    CHECK(Near(session.gyro.bias[0], 0.01f, 0) && Near(session.gyro.bias[1], 0.02f, 0) && Near(session.gyro.bias[2], 0.03f, 0));
    CHECK(session.gyro.rotation[0][0] == 1.0f && session.gyro.rotation[0][1] == 0.0f && session.gyro.mix[2][2] == 1.0f);

    /* Status reports count only now, and answers no longer do */
    CHECK(Feed(&session, now, answer, SDL_WMR_REPLY_LENGTH, &event) == SDL_WMR_REPORT_IGNORED);
    CHECK(Feed(&session, now, status, sizeof(status), &event) == SDL_WMR_REPORT_STATUS);

    /* The right code with the wrong echo or a length other than 78 fails the open */
    for (i = 0; i < 3; ++i) {
        static const size_t lengths[3] = { SDL_WMR_REPLY_LENGTH, SDL_WMR_REPLY_LENGTH - 1, SDL_WMR_REPLY_LENGTH + 1 };

        ResetTracker();
        SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
        memset(answer, 0, sizeof(answer));
        Answer(answer, 0x06, (uint8_t)(i == 0 ? 0x04 : 0x00), 0x00, 0, 0, NULL);
        CHECK(Feed(&session, now + 1, answer, lengths[i], &event) == SDL_WMR_REPORT_STARTUP);
        CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_REPLY);
        CHECK(tracker.writes == 1);
        CHECK(Feed(&session, now + 2, answer, SDL_WMR_REPLY_LENGTH, &event) == SDL_WMR_REPORT_IGNORED);
    }

    /* No matching answer within 250 ms fails the open */
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), 5000, &sink);
    SDL_WMR_Update(&session, 5249, &sink);
    CHECK(session.phase == SDL_WMR_PHASE_WAITING);
    SDL_WMR_Update(&session, 5250, &sink);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_TIMEOUT && tracker.writes == 1);

    /* An answer handed over before the update that finds the deadline passed still counts */
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), 5000, &sink);
    Answer(answer, 0x06, 0x00, 0x00, 0, 0, NULL);
    CHECK(Feed(&session, 5400, answer, SDL_WMR_REPLY_LENGTH, &event) == SDL_WMR_REPORT_STARTUP);
    SDL_WMR_Update(&session, 5400, &sink);
    CHECK(session.phase == SDL_WMR_PHASE_WAITING && tracker.writes == 2 && session.deadline_ms == 5650);
    SDL_WMR_Update(&session, 5649, &sink);
    CHECK(session.phase == SDL_WMR_PHASE_WAITING);
    SDL_WMR_Update(&session, 5650, &sink);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_TIMEOUT);
}

/* 2: the follow-up 10 ms after an answer that leaves 100 bytes, and a
   follow-up answer without data */
static void TestFollowUp(void)
{
    static const uint8_t follow_up[] = { 0x06, 0x02, 0x02, 0x64, 0x00, 0x00, 0x00 };
    SDL_WMRSession session;
    SDL_WMREvent event;
    Device device;
    uint8_t answer[SDL_WMR_REPLY_LENGTH];
    uint8_t data[68];
    uint64_t now = 0;
    int writes;

    MakeDevice(&device);
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, 2);
    CHECK(session.phase == SDL_WMR_PHASE_WAITING && session.step == SDL_WMR_STEP_BLOCK2);
    writes = tracker.writes;

    memset(data, 0x5A, sizeof(data));
    Answer(answer, 0x02, 0x02, 0x02, 100, 68, data);
    CHECK(Feed(&session, 700, answer, sizeof(answer), &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(session.phase == SDL_WMR_PHASE_DELAY && session.block_size == 168 && session.block_received == 68);
    /* An answer while the follow-up waits to go out answers nothing */
    CHECK(Feed(&session, 705, answer, sizeof(answer), &event) == SDL_WMR_REPORT_IGNORED);
    CHECK(session.phase == SDL_WMR_PHASE_DELAY && session.block_received == 68 && session.deadline_ms == 710);
    SDL_WMR_Update(&session, 709, &sink);
    CHECK(tracker.writes == writes && session.phase == SDL_WMR_PHASE_DELAY);
    SDL_WMR_Update(&session, 710, &sink);
    CHECK(tracker.writes == writes + 1 && CommandIs(tracker.last, follow_up, sizeof(follow_up)));
    CHECK(session.phase == SDL_WMR_PHASE_WAITING && session.deadline_ms == 960);

    /* Monado would keep asking */
    Answer(answer, 0x02, 0x02, 0x02, 100, 0, NULL);
    CHECK(Feed(&session, 715, answer, sizeof(answer), &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_BLOCK);
    CHECK(tracker.writes == writes + 1);

    /* A follow-up answer with more than remains copies only what remains */
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, 2);
    Answer(answer, 0x02, 0x02, 0x02, 10, 68, (const uint8_t *)"{\"CalibrationInformation\":{\"InertialSensors\":[],\"ControllerLeds\":[]}}");
    CHECK(Feed(&session, now + 1, answer, sizeof(answer), &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(session.phase == SDL_WMR_PHASE_DELAY && session.block_size == 78);
    SDL_WMR_Update(&session, now + 11, &sink);
    CHECK(tracker.last[3] == 10 && tracker.last[4] == 0);
    memset(data, 0x20, sizeof(data));
    Answer(answer, 0x02, 0x02, 0x07, 0, 68, data);
    CHECK(Feed(&session, now + 12, answer, sizeof(answer), &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(session.block_received == 78);
    /* The data went through the key, so this block's JSON is not JSON */
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_CONFIG);
}

/* A session waiting for the first answer of block 0 */
static void OpenAtBlock0(SDL_WMRSession *session, uint8_t *config, size_t capacity)
{
    SDL_WMREvent event;
    uint8_t answer[SDL_WMR_REPLY_LENGTH];

    ResetTracker();
    SDL_WMR_Open(session, SDL_WMR_MODEL_FIRST_GENERATION, false, config, capacity, 0, &sink);
    Answer(answer, 0x06, 0x00, 0x00, 0, 0, NULL);
    (void)Feed(session, 1, answer, sizeof(answer), &event);
    Answer(answer, 0x06, 0x04, 0x00, 0, 0, NULL);
    (void)Feed(session, 2, answer, sizeof(answer), &event);
    CHECK(session->phase == SDL_WMR_PHASE_WAITING && session->step == SDL_WMR_STEP_BLOCK0);
}

static void TestBlocks(void)
{
    SDL_WMRSession session;
    SDL_WMREvent event;
    Device device;
    uint8_t answer[SDL_WMR_REPLY_LENGTH];
    uint8_t small[2 + 90];
    uint64_t now;
    int writes;

    /* An empty block, more data than an answer holds, and a block past the
       limit, which a 32-bit sum would wrap to 67 bytes */
    OpenAtBlock0(&session, config_buffer, sizeof(config_buffer));
    Answer(answer, 0x02, 0x02, 0x00, 0, 0, NULL);
    CHECK(Feed(&session, 3, answer, sizeof(answer), &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_BLOCK);
    OpenAtBlock0(&session, config_buffer, sizeof(config_buffer));
    Answer(answer, 0x02, 0x02, 0x00, 0, 69, NULL);
    CHECK(Feed(&session, 3, answer, sizeof(answer), &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_REPLY);
    OpenAtBlock0(&session, config_buffer, sizeof(config_buffer));
    Answer(answer, 0x02, 0x02, 0x00, 0xFFFFFFFFu, 68, NULL);
    CHECK(Feed(&session, 3, answer, sizeof(answer), &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_BLOCK);
    OpenAtBlock0(&session, config_buffer, sizeof(config_buffer));
    Answer(answer, 0x02, 0x02, 0x00, SDL_WMR_BLOCK_MAXIMUM - 67, 68, NULL);
    CHECK(Feed(&session, 3, answer, sizeof(answer), &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_BLOCK);
    OpenAtBlock0(&session, config_buffer, sizeof(config_buffer));
    Answer(answer, 0x02, 0x02, 0x00, SDL_WMR_BLOCK_MAXIMUM - 68, 68, NULL);
    CHECK(Feed(&session, 3, answer, sizeof(answer), &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(session.phase == SDL_WMR_PHASE_DELAY && session.block_size == SDL_WMR_BLOCK_MAXIMUM);

    /* The block echo is not checked, as Monado does not */
    OpenAtBlock0(&session, config_buffer, sizeof(config_buffer));
    Answer(answer, 0x02, 0x02, 0x09, 0, 0x40, NULL);
    CHECK(Feed(&session, 3, answer, sizeof(answer), &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(session.phase == SDL_WMR_PHASE_WAITING && session.step == SDL_WMR_STEP_BLOCK3);

    /* Block 0 needs 0x36 bytes and block 3 0x94 */
    MakeDevice(&device);
    device.sizes[0] = 0x35;
    now = 0;
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_BLOCK && session.step == SDL_WMR_STEP_BLOCK0);
    device.sizes[0] = 0x36;
    device.sizes[3] = 0x93;
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_BLOCK && session.step == SDL_WMR_STEP_BLOCK3);
    CHECK(session.firmware_revision == 0x12345678u && session.calibration_size == 0x0ABC);
    device.sizes[3] = 0x94;
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_READY && strcmp(session.serial, "SERIAL0123456789") == 0);

    /* Block 2 needs its 2 bytes, then JSON Monado takes */
    device.sizes[2] = 1;
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_BLOCK && session.step == SDL_WMR_STEP_BLOCK2);
    device.sizes[2] = 2;
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_CONFIG);

    /* A block 2 larger than the buffer fails at its first answer */
    MakeDevice(&device);
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, small, sizeof(small), now, &sink);
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_BLOCK && session.block_size == 0);
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, NULL, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_BLOCK);

    /* Write failures: the first command, a follow-up, and the enable
       commands, whose writes Monado does not check */
    ResetTracker();
    tracker.fail_at = 0;
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_WRITE);
    ResetTracker();
    tracker.fail_at = 4;
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_WRITE && tracker.writes == 5);
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, 2);
    /* The status command follows the block 2 reads: its header read, then a
       follow-up for each further 68 bytes */
    writes = tracker.writes + (int)((device.sizes[2] + 67) / 68) - 1;
    ResetTracker();
    tracker.fail_at = writes;
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_READY && tracker.writes == writes + 2);
    CHECK(tracker.log[writes][1] == 0x03 && tracker.log[writes][2] == 0x01);
    ResetTracker();
    tracker.fail_at = writes + 1;
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_READY && tracker.writes == writes + 2);
}

/* hid.dll pads every report to the collection's longest, so a padded
   session takes longer answers and status reports */
static void TestPadded(void)
{
    SDL_WMRSession session;
    SDL_WMREvent event, exact;
    Device device;
    uint8_t answer[SDL_WMR_REPLY_LENGTH];
    uint8_t status[SDL_WMR_STATUS_LENGTH + 33];
    uint64_t now = 0;

    MakeDevice(&device);
    device.padding = 22;
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, true, config_buffer, sizeof(config_buffer), now, &sink);
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_READY);

    /* Too short still fails */
    ResetTracker();
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, true, config_buffer, sizeof(config_buffer), now, &sink);
    Answer(answer, 0x06, 0x00, 0x00, 0, 0, NULL);
    CHECK(Feed(&session, now + 1, answer, SDL_WMR_REPLY_LENGTH - 1, &event) == SDL_WMR_REPORT_STARTUP);
    CHECK(session.phase == SDL_WMR_PHASE_FAILED && session.failure == SDL_WMR_FAILURE_REPLY);

    /* A status report read at 78 bytes decodes as its 45 */
    MakeReady(&session, SDL_WMR_MODEL_FIRST_GENERATION, true, &now);
    memset(status, 0xEE, sizeof(status));
    memset(status, 0, SDL_WMR_STATUS_LENGTH);
    status[0] = 0x01;
    status[1] = 0x05;
    status[5] = 0x80;
    CHECK(Feed(&session, now, status, sizeof(status), &event) == SDL_WMR_REPORT_STATUS);
    CHECK(Feed(&session, now, status, SDL_WMR_STATUS_LENGTH, &exact) == SDL_WMR_REPORT_STATUS);
    CHECK(memcmp(&event.input, &exact.input, sizeof(exact.input)) == 0);
    CHECK(event.state.buttons[0] && event.state.buttons[2] && event.state.axes[2] == 128);
    CHECK(Feed(&session, now, status, SDL_WMR_STATUS_LENGTH - 1, &event) == SDL_WMR_REPORT_IGNORED);

    /* The same read in an exact session is ignored */
    MakeReady(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, &now);
    CHECK(Feed(&session, now, status, sizeof(status), &event) == SDL_WMR_REPORT_IGNORED);
}

static const uint8_t first_generation[SDL_WMR_STATUS_LENGTH] = {
    0x01, 0x01, 0xFF, 0xF7, 0x7F, 0x80, 0xFF, 0xFF, 0x64, 0x00, 0x00, 0x00, 0xC8, 0x53, 0x07, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA0, 0x86, 0x01, 0x00, 0x00, 0x00, 0x80, 0x96, 0x98,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* 3: a first-generation status report */
static void TestFirstGeneration(void)
{
    SDL_WMRSession session;
    SDL_WMREvent event;
    SDL_WMRInput input;
    float value[3];
    uint64_t now = 0;
    int i;

    CHECK(SDL_WMR_DecodeStatus(first_generation, sizeof(first_generation), false, SDL_WMR_MODEL_FIRST_GENERATION, &input));
    CHECK(input.buttons == 0x01 && input.stick_x == 0x7FF && input.stick_y == 0x7FF && input.trigger == 0x80);
    CHECK(input.touchpad_x == 0xFF && input.touchpad_y == 0xFF && input.battery == 0x64);
    CHECK(input.accel[0] == 0 && input.accel[1] == 480200 && input.accel[2] == 0 && input.temperature == 0);
    CHECK(input.gyro[0] == 0 && input.gyro[1] == 100000 && input.gyro[2] == 0 && input.timestamp == 10000000u);
    CHECK(input.grip == 0 && input.face_buttons == 0);
    SDL_WMR_AccelFromRaw(input.accel, value);
    CHECK(value[0] == 0 && Near(value[1], 9.8f, 1e-6f) && value[2] == 0);
    SDL_WMR_GyroFromRaw(input.gyro, value);
    CHECK(value[0] == 0 && Near(value[1], 1.0f, 1e-6f) && value[2] == 0);

    MakeReady(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, &now);
    CHECK(Feed(&session, now, first_generation, sizeof(first_generation), &event) == SDL_WMR_REPORT_STATUS);
    CHECK(event.state.num_axes == 5);
    CHECK(event.state.axes[0] == 0 && event.state.axes[1] == 0 && event.state.axes[2] == 128);
    CHECK(event.state.axes[3] == 0 && event.state.axes[4] == 0);
    CHECK(event.state.buttons[0]);
    for (i = 1; i < SDL_WMR_BUTTONS; ++i) {
        CHECK(!event.state.buttons[i]);
    }
    /* The accelerometer: mix (0, 9.702, 0), bias (0.1, 9.502, 0.3), the
       transposed rotation (9.502, -0.1, 0.3), then Y and Z negated. The
       gyroscope: (0.01, 1.02, 0.03) with Y and Z negated. */
    CHECK(Near(event.accel[0], 9.502f, 1e-4f) && Near(event.accel[1], 0.1f, 1e-5f) && Near(event.accel[2], -0.3f, 1e-5f));
    CHECK(Near(event.gyro[0], 0.01f, 1e-6f) && Near(event.gyro[1], -1.02f, 1e-6f) && Near(event.gyro[2], -0.03f, 1e-6f));
    CHECK(event.timestamp_ns == UINT64_C(1000000000));

    /* The touchpad, X from byte 5 and Y from byte 6, Y negated like the stick */
    {
        uint8_t touched[SDL_WMR_STATUS_LENGTH];

        memcpy(touched, first_generation, sizeof(touched));
        touched[6] = 0x0A;
        touched[7] = 0x28;
        CHECK(Feed(&session, now, touched, sizeof(touched), &event) == SDL_WMR_REPORT_STATUS);
        CHECK(event.input.touchpad_x == 0x0A && event.input.touchpad_y == 0x28);
        CHECK(event.state.axes[3] == -26214 && event.state.axes[4] == 6553);
    }
}

/* 4: a Reverb G2 status report */
static void TestReverbG2(void)
{
    uint8_t report[SDL_WMR_STATUS_LENGTH];
    SDL_WMRSession session;
    SDL_WMREvent event;
    uint64_t now = 0;

    memset(report, 0, sizeof(report));
    report[0] = 0x01;
    report[1] = 0x02;
    report[5] = 0xFF;
    report[6] = 0x40;
    report[7] = 0x03;
    report[8] = 0x50;
    MakeReady(&session, SDL_WMR_MODEL_REVERB_G2, false, &now);
    CHECK(Feed(&session, now, report, sizeof(report), &event) == SDL_WMR_REPORT_STATUS);
    CHECK(event.input.buttons == 0x02 && event.input.stick_x == 0 && event.input.stick_y == 0);
    CHECK(event.input.trigger == 0xFF && event.input.grip == 0x40 && event.input.face_buttons == 0x03 && event.input.battery == 0x50);
    CHECK(event.input.touchpad_x == 0 && event.input.touchpad_y == 0);
    CHECK(event.state.num_axes == 4);
    CHECK(event.state.axes[0] == -32768 && event.state.axes[1] == 32767 && event.state.axes[2] == 32767);
    CHECK(event.state.axes[3] == 0x40 * 257 - 32768 && event.state.axes[4] == 0);
    CHECK(!event.state.buttons[0] && event.state.buttons[1] && !event.state.buttons[2] && !event.state.buttons[3]);
    CHECK(event.state.buttons[4] && event.state.buttons[5]);
}

/* 5: each button bit alone gives exactly its button, and the stick nibbles */
static void TestButtons(void)
{
    static const int first_generation_button[8] = { 0, 1, 2, 3, 4, -1, 5, -1 };
    static const int reverb_button[8] = { 0, 1, 2, 3, -1, -1, -1, -1 };
    static const int reverb_face_button[8] = { 5, 4, -1, -1, -1, -1, -1, -1 };
    uint8_t report[SDL_WMR_STATUS_LENGTH];
    SDL_WMRInput input;
    SDL_WMRState state;
    int bit, i;

    for (bit = 0; bit < 8; ++bit) {
        memcpy(report, first_generation, sizeof(report));
        report[1] = (uint8_t)(1 << bit);
        CHECK(SDL_WMR_DecodeStatus(report, sizeof(report), false, SDL_WMR_MODEL_FIRST_GENERATION, &input));
        SDL_WMR_GetState(SDL_WMR_MODEL_FIRST_GENERATION, &input, &state);
        for (i = 0; i < SDL_WMR_BUTTONS; ++i) {
            CHECK(state.buttons[i] == (i == first_generation_button[bit]));
        }
        report[7] = 0;
        CHECK(SDL_WMR_DecodeStatus(report, sizeof(report), false, SDL_WMR_MODEL_REVERB_G2, &input));
        SDL_WMR_GetState(SDL_WMR_MODEL_REVERB_G2, &input, &state);
        for (i = 0; i < SDL_WMR_BUTTONS; ++i) {
            CHECK(state.buttons[i] == (i == reverb_button[bit]));
        }
        memcpy(report, first_generation, sizeof(report));
        report[1] = 0;
        report[7] = (uint8_t)(1 << bit);
        CHECK(SDL_WMR_DecodeStatus(report, sizeof(report), false, SDL_WMR_MODEL_REVERB_G2, &input));
        SDL_WMR_GetState(SDL_WMR_MODEL_REVERB_G2, &input, &state);
        for (i = 0; i < SDL_WMR_BUTTONS; ++i) {
            CHECK(state.buttons[i] == (i == reverb_face_button[bit]));
        }
    }

    memcpy(report, first_generation, sizeof(report));
    report[2] = 0xFF;
    report[3] = 0xFF;
    report[4] = 0xFF;
    CHECK(SDL_WMR_DecodeStatus(report, sizeof(report), false, SDL_WMR_MODEL_FIRST_GENERATION, &input));
    CHECK(input.stick_x == 0xFFF && input.stick_y == 0xFFF);
    report[3] = 0x0F;
    CHECK(SDL_WMR_DecodeStatus(report, sizeof(report), false, SDL_WMR_MODEL_FIRST_GENERATION, &input));
    CHECK(input.stick_x == 0xFFF && input.stick_y == 0xFF0);
    report[2] = 0x34;
    report[3] = 0x12;
    report[4] = 0xAB;
    CHECK(SDL_WMR_DecodeStatus(report, sizeof(report), false, SDL_WMR_MODEL_FIRST_GENERATION, &input));
    CHECK(input.stick_x == 0x234 && input.stick_y == 0xAB1);

    /* Signed values and the temperature */
    memcpy(report, first_generation, sizeof(report));
    report[9] = 0xFF;
    report[10] = 0xFF;
    report[11] = 0xFF;
    report[12] = 0x00;
    report[13] = 0x00;
    report[14] = 0x80;
    report[18] = 0x18;
    report[19] = 0xFC;
    report[20] = 0xFF;
    report[21] = 0xFF;
    report[22] = 0x7F;
    CHECK(SDL_WMR_DecodeStatus(report, sizeof(report), false, SDL_WMR_MODEL_FIRST_GENERATION, &input));
    CHECK(input.accel[0] == -1 && input.accel[1] == -8388608 && input.temperature == -1000 && input.gyro[0] == 8388607);
}

static void TestAxes(void)
{
    int value, previous = 32768, previous_inverted = -32769;

    CHECK(SDL_WMR_StickAxis(0, false) == -32768 && SDL_WMR_StickAxis(0x7FF, false) == 0 && SDL_WMR_StickAxis(0xFFF, false) == 32767);
    CHECK(SDL_WMR_StickAxis(0, true) == 32767 && SDL_WMR_StickAxis(0x7FF, true) == 0 && SDL_WMR_StickAxis(0xFFF, true) == -32768);
    CHECK(SDL_WMR_StickAxis(0x7FE, false) == -16 && SDL_WMR_StickAxis(0x800, false) == 15);
    CHECK(SDL_WMR_StickAxis(0x7FE, true) == 16 && SDL_WMR_StickAxis(0x800, true) == -16);
    for (value = 0xFFF; value >= 0; --value) {
        const int axis = SDL_WMR_StickAxis((uint16_t)value, false);
        const int inverted = SDL_WMR_StickAxis((uint16_t)value, true);

        CHECK(axis < previous && inverted > previous_inverted);
        previous = axis;
        previous_inverted = inverted;
    }

    CHECK(SDL_WMR_TouchpadAxis(0, false) == -32768 && SDL_WMR_TouchpadAxis(0x32, false) == 0 && SDL_WMR_TouchpadAxis(0x64, false) == 32767);
    CHECK(SDL_WMR_TouchpadAxis(0, true) == 32767 && SDL_WMR_TouchpadAxis(0x32, true) == 0 && SDL_WMR_TouchpadAxis(0x64, true) == -32768);
    CHECK(SDL_WMR_TouchpadAxis(0xFF, false) == 0 && SDL_WMR_TouchpadAxis(0xFF, true) == 0);
    CHECK(SDL_WMR_TouchpadAxis(0x65, false) == 32767 && SDL_WMR_TouchpadAxis(0xFE, true) == -32768);
    CHECK(SDL_WMR_TouchpadAxis(0x19, false) == -16384 && SDL_WMR_TouchpadAxis(0x4B, false) == 16383);
    CHECK(SDL_WMR_GetNumAxes(SDL_WMR_MODEL_FIRST_GENERATION) == 5 && SDL_WMR_GetNumAxes(SDL_WMR_MODEL_REVERB_G2) == 4);
}

/* 6: lengths other than 45 and first bytes other than 0x01 are ignored,
   with 0xFF past the received length */
static void TestFraming(void)
{
    SDL_WMRSession session;
    SDL_WMREvent event;
    SDL_WMRInput input;
    uint8_t stale[SDL_WMR_STATUS_LENGTH + 40];
    uint64_t now = 0;
    size_t length;

    MakeReady(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, &now);
    for (length = 0; length < sizeof(stale); ++length) {
        const SDL_WMRReport expected = (length == SDL_WMR_STATUS_LENGTH) ? SDL_WMR_REPORT_STATUS : SDL_WMR_REPORT_IGNORED;

        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, first_generation, length < sizeof(first_generation) ? length : sizeof(first_generation));
        CHECK(Feed(&session, now, stale, length, &event) == expected);
        CHECK(SDL_WMR_HandleReport(&session, now, stale, length, &sink, &event) == expected);
        CHECK(SDL_WMR_DecodeStatus(stale, length, false, SDL_WMR_MODEL_FIRST_GENERATION, &input) == (length == SDL_WMR_STATUS_LENGTH));
    }
    memcpy(stale, first_generation, sizeof(first_generation));
    for (length = 0; length < 256; ++length) {
        stale[0] = (uint8_t)length;
        CHECK(Feed(&session, now, stale, SDL_WMR_STATUS_LENGTH, &event) == (length == 0x01 ? SDL_WMR_REPORT_STATUS : SDL_WMR_REPORT_IGNORED));
    }
    CHECK(!SDL_WMR_DecodeStatus(NULL, 45, false, SDL_WMR_MODEL_FIRST_GENERATION, &input));
    CHECK(!SDL_WMR_DecodeStatus(first_generation, 45, false, SDL_WMR_MODEL_FIRST_GENERATION, NULL));
    CHECK(!SDL_WMR_DecodeStatus(first_generation, 44, true, SDL_WMR_MODEL_FIRST_GENERATION, &input));
}

/* 7: the tick count across its wrap */
static void TestTimestamps(void)
{
    SDL_WMRSession session;
    SDL_WMREvent event;
    uint8_t report[SDL_WMR_STATUS_LENGTH];
    uint64_t now = 0, first, ticks = 0;

    MakeReady(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, &now);
    memcpy(report, first_generation, sizeof(report));
    report[29] = 0xF0;
    report[30] = 0xFF;
    report[31] = 0xFF;
    report[32] = 0xFF;
    CHECK(Feed(&session, now, report, sizeof(report), &event) == SDL_WMR_REPORT_STATUS);
    first = event.timestamp_ns;
    CHECK(first == UINT64_C(0xFFFFFFF0) * 100);
    report[29] = 0x10;
    report[30] = 0x00;
    report[31] = 0x00;
    report[32] = 0x00;
    CHECK(Feed(&session, now, report, sizeof(report), &event) == SDL_WMR_REPORT_STATUS);
    CHECK(event.timestamp_ns - first == 32 * 100);
    CHECK(event.timestamp_ns == UINT64_C(0x100000010) * 100);

    CHECK(SDL_WMR_ExtendTimestamp(&ticks, 5) == 5);
    CHECK(SDL_WMR_ExtendTimestamp(&ticks, 5) == 5);
    CHECK(SDL_WMR_ExtendTimestamp(&ticks, 0xFFFFFFFFu) == 0xFFFFFFFFu);
    CHECK(SDL_WMR_ExtendTimestamp(&ticks, 0) == UINT64_C(0x100000000));
    CHECK(SDL_WMR_ExtendTimestamp(&ticks, 4) == UINT64_C(0x100000004));
    CHECK(SDL_WMR_ExtendTimestamp(&ticks, 3) == UINT64_C(0x200000003));
}

/* 8: a reconnect runs the start-up again from the reset */
static void TestReconnect(void)
{
    static const uint8_t reset[] = { 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    SDL_WMRSession session;
    SDL_WMREvent event;
    Device device;
    uint64_t now = 0;
    int writes;

    MakeReady(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, &now);
    CHECK(Feed(&session, now, first_generation, sizeof(first_generation), &event) == SDL_WMR_REPORT_STATUS);
    writes = tracker.writes;
    SDL_WMR_Open(&session, SDL_WMR_MODEL_FIRST_GENERATION, false, config_buffer, sizeof(config_buffer), now, &sink);
    CHECK(tracker.writes == writes + 1 && CommandIs(tracker.last, reset, sizeof(reset)));
    CHECK(session.phase == SDL_WMR_PHASE_WAITING && session.step == SDL_WMR_STEP_RESET && session.ticks == 0);
    CHECK(session.accel.bias[0] == 0 && session.accel.mix[0][0] == 1.0f && session.serial[0] == '\0');
    CHECK(Feed(&session, now, first_generation, sizeof(first_generation), &event) == SDL_WMR_REPORT_IGNORED);
    MakeDevice(&device);
    Run(&session, &device, &now, -1);
    CHECK(session.phase == SDL_WMR_PHASE_READY && tracker.writes == 2 * writes);
    CHECK(Feed(&session, now, first_generation, sizeof(first_generation), &event) == SDL_WMR_REPORT_STATUS);
    CHECK(event.timestamp_ns == UINT64_C(1000000000));
}

static void TestCalibrate(void)
{
    SDL_WMRCalibration calibration;
    const float value[3] = { 1.0f, 2.0f, 3.0f };
    float out[3];

    SDL_WMR_InitCalibration(&calibration);
    SDL_WMR_Calibrate(&calibration, value, out);
    CHECK(out[0] == 1.0f && out[1] == -2.0f && out[2] == -3.0f);

    /* A rotation of 90 degrees about X and a mix that doubles X */
    memset(&calibration, 0, sizeof(calibration));
    calibration.rotation[0][0] = 1.0f;
    calibration.rotation[1][2] = -1.0f;
    calibration.rotation[2][1] = 1.0f;
    calibration.mix[0][0] = 2.0f;
    calibration.mix[1][1] = 1.0f;
    calibration.mix[2][2] = 1.0f;
    calibration.bias[2] = 0.5f;
    /* mix and bias (2, 2, 3.5), R^T (2, 3.5, -2), negated (2, -3.5, 2) */
    SDL_WMR_Calibrate(&calibration, value, out);
    CHECK(out[0] == 2.0f && out[1] == -3.5f && out[2] == 2.0f);
}

/* Configuration JSON */

#define WRAP_SENSORS(sensors) "{\"CalibrationInformation\":{\"InertialSensors\":[" sensors "],\"ControllerLeds\":[]}}"
#define TYPE_ACCEL "\"SensorType\":\"CALIBRATION_InertialSensorType_Accelerometer\""
#define TYPE_GYRO  "\"SensorType\":\"CALIBRATION_InertialSensorType_Gyro\""
#define RT_FULL    "\"Rt\":{\"Rotation\":[1,2,3,4,5,6,7,8,9],\"Translation\":[0,0,0]}"
#define MIX_FULL   "\"MixingMatrixTemperatureModel\":[11,0,0,0,12,0,0,0,13,0,0,0,21,0,0,0,22,0,0,0,23,0,0,0,31,0,0,0,32,0,0,0,33,0,0,0]"
#define BIAS_FULL  "\"BiasTemperatureModel\":[41,0,0,0,42,0,0,0,43,0,0,0]"
#define REST_FULL  "\"BiasUncertainty\":[0,0,0],\"Noise\":[0,0,0,0,0,0]"
#define ACCEL_FULL "{" TYPE_ACCEL "," RT_FULL "," MIX_FULL "," BIAS_FULL "," REST_FULL "}"

static bool Parse(const char *json, SDL_WMRCalibration *accel, SDL_WMRCalibration *gyro)
{
    const size_t length = strlen(json);
    char *copy = (char *)malloc(length ? length : 1);
    bool result;

    if (length) {
        memcpy(copy, json, length);
    }
    result = SDL_WMR_ParseConfig(copy, length, accel, gyro);
    free(copy);
    return result;
}

static bool IsDefault(const SDL_WMRCalibration *c)
{
    int r, k;

    for (r = 0; r < 3; ++r) {
        if (c->bias[r] != 0) {
            return false;
        }
        for (k = 0; k < 3; ++k) {
            if (c->mix[r][k] != (r == k ? 1.0f : 0.0f) || c->rotation[r][k] != (r == k ? 1.0f : 0.0f)) {
                return false;
            }
        }
    }
    return true;
}

static bool RotationIsFull(const SDL_WMRCalibration *c)
{
    int r, k;

    for (r = 0; r < 3; ++r) {
        for (k = 0; k < 3; ++k) {
            if (c->rotation[r][k] != (float)(1 + 3 * r + k)) {
                return false;
            }
        }
    }
    return true;
}

static bool MixIsFull(const SDL_WMRCalibration *c)
{
    int r, k;

    for (r = 0; r < 3; ++r) {
        for (k = 0; k < 3; ++k) {
            if (c->mix[r][k] != (float)(10 * (r + 1) + k + 1)) {
                return false;
            }
        }
    }
    return true;
}

static bool MixIsIdentity(const SDL_WMRCalibration *c)
{
    int r, k;

    for (r = 0; r < 3; ++r) {
        for (k = 0; k < 3; ++k) {
            if (c->mix[r][k] != (r == k ? 1.0f : 0.0f)) {
                return false;
            }
        }
    }
    return true;
}

static bool BiasIsFull(const SDL_WMRCalibration *c)
{
    return c->bias[0] == 41.0f && c->bias[1] == 42.0f && c->bias[2] == 43.0f;
}

static bool BiasIsZero(const SDL_WMRCalibration *c)
{
    return c->bias[0] == 0 && c->bias[1] == 0 && c->bias[2] == 0;
}

static void TestConfigSensors(void)
{
    SDL_WMRCalibration accel, gyro;

    CHECK(Parse(WRAP_SENSORS(ACCEL_FULL), &accel, &gyro));
    CHECK(RotationIsFull(&accel) && MixIsFull(&accel) && BiasIsFull(&accel) && IsDefault(&gyro));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_GYRO "," RT_FULL "," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel) && RotationIsFull(&gyro) && MixIsFull(&gyro) && BiasIsFull(&gyro));

    /* Each part is kept once read, and the first part missing ends the entry */
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL "," RT_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel) && MixIsIdentity(&accel) && BiasIsZero(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL "," RT_FULL "," MIX_FULL "," BIAS_FULL ",\"BiasUncertainty\":[0,0,0]}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel) && MixIsIdentity(&accel) && BiasIsZero(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL "," RT_FULL "," MIX_FULL "," BIAS_FULL ",\"Noise\":[0,0,0,0,0,0]}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel) && MixIsIdentity(&accel) && BiasIsZero(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL "," RT_FULL "," MIX_FULL ",\"BiasTemperatureModel\":[41,0,0,0,42,0,0,0,43,0,0]," REST_FULL "}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel) && MixIsFull(&accel) && BiasIsZero(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL "," RT_FULL ",\"MixingMatrixTemperatureModel\":[11,0,0,0,12,0,0,0,13,0,0,0,21,0,0,0,22,0,0,0,\"x\",0,0,0,31,0,0,0,32,0,0,0,33,0,0,0]," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel) && MixIsIdentity(&accel) && BiasIsZero(&accel));
    /* More numbers than needed are fine, and so is anything after them */
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL ",\"Rt\":{\"Rotation\":[1,2,3,4,5,6,7,8,9,10,\"x\"],\"Translation\":[0,0,0]}," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel) && MixIsFull(&accel) && BiasIsFull(&accel));

    /* Rt: a rotation of 9 numbers and a translation of exactly 3 elements,
       which Monado's release build reads without checking their type */
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL ",\"Rt\":{\"Rotation\":[1,2,3,4,5,6,7,8],\"Translation\":[0,0,0]}," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL ",\"Rt\":{\"Rotation\":[1,2,3,4,5,6,7,8,9],\"Translation\":[0,0]}," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL ",\"Rt\":{\"Rotation\":[1,2,3,4,5,6,7,8,9],\"Translation\":[0,0,0,0]}," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL ",\"Rt\":{\"Rotation\":[1,2,3,4,5,6,7,8,9],\"Translation\":{\"x\":0,\"y\":0,\"z\":0}}," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL ",\"Rt\":{\"Rotation\":[1,2,3,4,5,6,7,8,9],\"translation\":[0,0,0]}," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL ",\"Rt\":{\"Rotation\":[1,2,3,4,5,6,7,8,9],\"Translation\":[\"a\",null,{}]}," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel) && MixIsFull(&accel) && BiasIsFull(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL ",\"Rt\":[1,2,3,4,5,6,7,8,9]," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL "," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel));

    /* Names below the sensor match in any case, the type string exactly */
    CHECK(Parse(WRAP_SENSORS("{\"sensortype\":\"CALIBRATION_InertialSensorType_Accelerometer\",\"RT\":{\"ROTATION\":[1,2,3,4,5,6,7,8,9],\"Translation\":[0,0,0]},"
                             "\"mixingmatrixtemperaturemodel\":[11,0,0,0,12,0,0,0,13,0,0,0,21,0,0,0,22,0,0,0,23,0,0,0,31,0,0,0,32,0,0,0,33,0,0,0],"
                             "\"BIASTEMPERATUREMODEL\":[41,0,0,0,42,0,0,0,43,0,0,0],\"biasUncertainty\":[0,0,0],\"nOISE\":[0,0,0,0,0,0]}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel) && MixIsFull(&accel) && BiasIsFull(&accel));
    CHECK(Parse(WRAP_SENSORS("{\"SensorType\":\"calibration_inertialsensortype_accelerometer\"," RT_FULL "," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel) && IsDefault(&gyro));
    CHECK(Parse(WRAP_SENSORS("{\"SensorType\":\"CALIBRATION_InertialSensorType_Other\"," RT_FULL "," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel) && IsDefault(&gyro));
    CHECK(Parse(WRAP_SENSORS("{\"SensorType\":7," RT_FULL "," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel) && IsDefault(&gyro));
    /* The magnetometer changes neither */
    CHECK(Parse(WRAP_SENSORS("{\"SensorType\":\"CALIBRATION_InertialSensorType_Magnetometer\"," RT_FULL "," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel) && IsDefault(&gyro));

    /* The first of duplicate names counts, a later entry of the same type
       overwrites what it reads, and entries that are no objects are skipped */
    CHECK(Parse(WRAP_SENSORS("{" TYPE_GYRO "," TYPE_ACCEL "," RT_FULL "," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel) && RotationIsFull(&gyro) && BiasIsFull(&gyro));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL ",\"Rt\":{\"Rotation\":[9,9,9,9,9,9,9,9,9],\"Translation\":[0,0,0]},\"Rt\":[]," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(accel.rotation[2][2] == 9.0f && accel.rotation[0][0] == 9.0f && MixIsFull(&accel));
    CHECK(Parse(WRAP_SENSORS(ACCEL_FULL ",{" TYPE_ACCEL ",\"Rt\":{\"Rotation\":[9,9,9,9,9,9,9,9,9],\"Translation\":[0,0,0]}}"), &accel, &gyro));
    CHECK(accel.rotation[0][0] == 9.0f && accel.rotation[2][2] == 9.0f && MixIsFull(&accel) && BiasIsFull(&accel));
    CHECK(Parse(WRAP_SENSORS("[],1,\"x\",null,true,{}," ACCEL_FULL), &accel, &gyro));
    CHECK(RotationIsFull(&accel) && MixIsFull(&accel) && BiasIsFull(&accel));

    /* Escapes in names: \u0052 is R, and a name is a C string, so it ends at
       an escaped NUL or at an invalid \u, which cJSON reads as NUL */
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL ",\"\\u0052t\":{\"Rotation\":[1,2,3,4,5,6,7,8,9],\"Translation\":[0,0,0]}}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL ",\"Rt\\u0000junk\":{\"Rotation\":[1,2,3,4,5,6,7,8,9],\"Translation\":[0,0,0]}}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel));
    CHECK(Parse(WRAP_SENSORS("{" TYPE_ACCEL ",\"Rt\\uZZZZjunk\":{\"Rotation\":[1,2,3,4,5,6,7,8,9],\"Translation\":[0,0,0]}}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel));
    CHECK(Parse(WRAP_SENSORS("{\"SensorType\":\"CALIBRATION_InertialSensorType_Accelerometer\\u0000x\"," RT_FULL "}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel));
    CHECK(Parse(WRAP_SENSORS("{\"SensorType\":\"CALIBRATION_InertialSensorType_Accelerometer\\/\"," RT_FULL "}"), &accel, &gyro));
    CHECK(IsDefault(&accel));
    CHECK(Parse(WRAP_SENSORS("{\"SensorType\":\"CALIBRATION_\\u0049nertialSensorType_Accelerometer\"," RT_FULL "}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel));
    /* A name longer than any looked for is skipped */
    CHECK(Parse(WRAP_SENSORS("{\"RtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRtRt\":1," TYPE_ACCEL "," RT_FULL "," MIX_FULL "," BIAS_FULL "," REST_FULL "}"), &accel, &gyro));
    CHECK(RotationIsFull(&accel) && MixIsFull(&accel));
}

static bool ParseExtra(const char *extra)
{
    static char json[8192];
    SDL_WMRCalibration accel, gyro;

    snprintf(json, sizeof(json), "{\"CalibrationInformation\":{\"Extra\":%s,\"InertialSensors\":[],\"ControllerLeds\":[]}}", extra);
    return Parse(json, &accel, &gyro);
}

/* The first rotation value, as read from text */
static bool ParseNumber(const char *number, float *value)
{
    static char json[512];
    SDL_WMRCalibration accel, gyro;
    bool result;

    snprintf(json, sizeof(json), WRAP_SENSORS("{" TYPE_ACCEL ",\"Rt\":{\"Rotation\":[%s,2,3,4,5,6,7,8,9],\"Translation\":[0,0,0]}}"), number);
    result = Parse(json, &accel, &gyro);
    *value = accel.rotation[0][0];
    return result && accel.rotation[2][2] == 9.0f;
}

static void TestConfigStructure(void)
{
    static char deep[4096];
    SDL_WMRCalibration accel, gyro;
    char *json;
    size_t length, i;
    int depth;

    CHECK(Parse("{\"CalibrationInformation\":{\"InertialSensors\":[],\"ControllerLeds\":[]}}", &accel, &gyro));
    CHECK(IsDefault(&accel) && IsDefault(&gyro));
    CHECK(Parse("{\"CalibrationInformation\":{\"ControllerLeds\":[1],\"InertialSensors\":[]},\"CalibrationInformation\":1}", &accel, &gyro));

    /* What Monado refuses */
    CHECK(!Parse("[]", &accel, &gyro));
    CHECK(!Parse("{}", &accel, &gyro));
    CHECK(!Parse("{\"CalibrationInformation\":[]}", &accel, &gyro));
    CHECK(!Parse("{\"calibrationInformation\":{\"InertialSensors\":[],\"ControllerLeds\":[]}}", &accel, &gyro));
    CHECK(!Parse("{\"CalibrationInformation\":{\"ControllerLeds\":[]}}", &accel, &gyro));
    CHECK(!Parse("{\"CalibrationInformation\":{\"InertialSensors\":{},\"ControllerLeds\":[]}}", &accel, &gyro));
    CHECK(!Parse("{\"CalibrationInformation\":{\"inertialSensors\":[],\"ControllerLeds\":[]}}", &accel, &gyro));
    CHECK(!Parse("{\"CalibrationInformation\":{\"InertialSensors\":[]}}", &accel, &gyro));
    CHECK(!Parse("{\"CalibrationInformation\":{\"InertialSensors\":[],\"ControllerLeds\":{}}}", &accel, &gyro));
    CHECK(!Parse("{\"CalibrationInformation\":{\"InertialSensors\":[],\"ControllerLeds\":\"x\"}}", &accel, &gyro));
    CHECK(!Parse("{\"CalibrationInformation\":{\"InertialSensors\":[],\"controllerLeds\":[]}}", &accel, &gyro));
    CHECK(!Parse("", &accel, &gyro));
    CHECK(!SDL_WMR_ParseConfig(NULL, 10, &accel, &gyro));

    /* The sensors are read before the LEDs are looked for */
    CHECK(!Parse("{\"CalibrationInformation\":{\"InertialSensors\":[" ACCEL_FULL "]}}", &accel, &gyro));
    CHECK(RotationIsFull(&accel));

    /* SDL reads no LED entries, so it takes more than Monado's 40 */
    {
        static const char head[] = "{\"CalibrationInformation\":{\"InertialSensors\":[],\"ControllerLeds\":[";
        static const char led[] = "{\"Position\":[0,0,0],\"Normal\":[0,0,1]},";
        static const char tail[] = "{\"Position\":[0,0,0],\"Normal\":[0,0,1]}]}}";

        length = (sizeof(head) - 1) + 40 * (sizeof(led) - 1) + sizeof(tail);
        json = (char *)malloc(length);
        if (json) {
            char *at = json;

            memcpy(at, head, sizeof(head) - 1);
            at += sizeof(head) - 1;
            for (i = 0; i < 40; ++i) {
                memcpy(at, led, sizeof(led) - 1);
                at += sizeof(led) - 1;
            }
            memcpy(at, tail, sizeof(tail));
            CHECK(Parse(json, &accel, &gyro));
            free(json);
        }
    }

    /* Text: a byte order mark, bytes up to 32 as white space, anything after
       the root, and the end at the first NUL */
    CHECK(Parse("\xEF\xBB\xBF{\"CalibrationInformation\":{\"InertialSensors\":[],\"ControllerLeds\":[]}}", &accel, &gyro));
    CHECK(Parse("\x01\x1F {\x02\"CalibrationInformation\"\x03:\x04{\"InertialSensors\" : [ ] , \"ControllerLeds\":[]\x05}\x06}", &accel, &gyro));
    CHECK(Parse("{\"CalibrationInformation\":{\"InertialSensors\":[],\"ControllerLeds\":[]}} trailing ]]] garbage", &accel, &gyro));
    {
        static const char with_nul[] = "{\"CalibrationInformation\":{\"InertialSensors\":[],\"ControllerLeds\":[]}}\0{{{";
        static const char cut[] = "{\"CalibrationInformation\":{\"InertialSensors\":[],\0\"ControllerLeds\":[]}}";

        CHECK(SDL_WMR_ParseConfig(with_nul, sizeof(with_nul) - 1, &accel, &gyro));
        CHECK(!SDL_WMR_ParseConfig(cut, sizeof(cut) - 1, &accel, &gyro));
    }
    /* Every prefix of a valid document is refused */
    {
        static const char whole[] = "{\"CalibrationInformation\":{\"InertialSensors\":[" ACCEL_FULL "],\"ControllerLeds\":[]}}";

        for (length = 0; length < sizeof(whole) - 1; ++length) {
            char *copy = (char *)malloc(length ? length : 1);

            memcpy(copy, whole, length);
            CHECK(!SDL_WMR_ParseConfig(copy, length, &accel, &gyro));
            free(copy);
        }
        CHECK(SDL_WMR_ParseConfig(whole, sizeof(whole) - 1, &accel, &gyro) && BiasIsFull(&accel));
    }

    /* Syntax cJSON refuses anywhere in the document */
    CHECK(ParseExtra("[1,2,3]") && ParseExtra("{\"a\":{\"b\":[true,false,null]}}") && ParseExtra("\"\\b\\f\\n\\r\\t\\\"\\\\\\/\""));
    CHECK(!ParseExtra("[1,2,]") && !ParseExtra("[,1]") && !ParseExtra("{\"a\":1,}") && !ParseExtra("{\"a\" 1}"));
    CHECK(!ParseExtra("{1:2}") && !ParseExtra("{\"a\":}") && !ParseExtra("[1 2]") && !ParseExtra("tru") && !ParseExtra("nul"));
    CHECK(!ParseExtra("\"\\q\"") && !ParseExtra("\"\\u12\"") && !ParseExtra("'a'") && !ParseExtra("[1]]"));
    CHECK(ParseExtra("\"\\u00e9\\u20AC\\uD83D\\uDE00\"") && ParseExtra("\"\\uZZZZ\""));
    CHECK(!ParseExtra("\"\\uDE00\"") && !ParseExtra("\"\\uD83D\"") && !ParseExtra("\"\\uD83Dx\\uDE00\"") && !ParseExtra("\"\\uD83D\\u0041\""));
    CHECK(!ParseExtra("\"abc") && !ParseExtra("[\"abc]") && !ParseExtra("\"\\\""));

    /* Numbers: the longest prefix strtod takes, at most 63 characters */
    {
        float value = 0;

        CHECK(ParseNumber("1e2", &value) && value == 100.0f);
        CHECK(ParseNumber("-0.5E-3", &value) && value == -0.0005f);
        CHECK(ParseNumber("1.", &value) && value == 1.0f);
        CHECK(ParseNumber("01", &value) && value == 1.0f);
        CHECK(ParseNumber("-.25", &value) && value == -0.25f);
        CHECK(ParseNumber("0.1", &value) && value == 0.1f);
        CHECK(ParseNumber("123456789012345678901234567890", &value) && value == 1.23456789e29f);
        CHECK(ParseNumber("1e400", &value) && value > 3.4e38f);
        CHECK(ParseNumber("1e-400", &value) && value == 0.0f);
        CHECK(ParseNumber("-0", &value) && value == 0.0f);
        CHECK(ParseNumber("0.000000000000000000000000000000012345", &value) && value == 1.2345e-32f);
        CHECK(ParseNumber("1.0000000000000000000000000000000000000000000000000000000000001", &value) && value == 1.0f);
        CHECK(!ParseNumber("1.00000000000000000000000000000000000000000000000000000000000001", &value));
        CHECK(!ParseNumber("-", &value) && !ParseNumber("+1", &value) && !ParseNumber(".5", &value));
        CHECK(!ParseNumber("1e", &value) && !ParseNumber("1e+", &value) && !ParseNumber("1.5.3", &value));
        CHECK(!ParseNumber("0x10", &value) && !ParseNumber("1-2", &value) && !ParseNumber("--1", &value));
    }

    /* Containers nest up to 1000 deep, the root and CalibrationInformation
       counting as two */
    for (depth = 997; depth <= 999; ++depth) {
        int k;

        for (k = 0; k < depth; ++k) {
            deep[k] = '[';
        }
        for (k = 0; k < depth; ++k) {
            deep[depth + k] = ']';
        }
        deep[2 * depth] = '\0';
        CHECK(ParseExtra(deep) == (depth <= 998));
    }
}

int main(void)
{
    TestIdentity();
    TestKey();
    TestStartup();
    TestFollowUp();
    TestBlocks();
    TestPadded();
    TestFirstGeneration();
    TestReverbG2();
    TestButtons();
    TestAxes();
    TestFraming();
    TestTimestamps();
    TestReconnect();
    TestCalibrate();
    TestConfigSensors();
    TestConfigStructure();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
