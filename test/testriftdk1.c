/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/hidapi/SDL_hidapi_riftdk1_proto.c and the
   fusion port beside it, the Oculus Rift DK1 of hifihedgehog/SDL#33 Part 7.
   No raw capture is published, so the reports are built from the Oculus
   SDK and OpenHMD. */

#include "../src/joystick/hidapi/SDL_hidapi_riftdk1_proto.h"

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

#define PI_F 3.14159265f

#define MAX_LOG 16

typedef struct TestTracker
{
    int get_result;
    uint8_t config[7];
    int calls;
    char kind[MAX_LOG]; /* 'g' get, 's' set */
    uint8_t data[MAX_LOG][8];
    size_t length[MAX_LOG];
    uint64_t time[MAX_LOG];
    uint64_t now;
} TestTracker;

static void Log(TestTracker *tracker, char kind, const uint8_t *data, size_t length)
{
    CHECK(tracker->calls < MAX_LOG && length <= 8);
    if (tracker->calls < MAX_LOG && length <= 8) {
        tracker->kind[tracker->calls] = kind;
        memcpy(tracker->data[tracker->calls], data, length);
        tracker->length[tracker->calls] = length;
        tracker->time[tracker->calls] = tracker->now;
    }
    ++tracker->calls;
}

static int TestGet(void *userdata, uint8_t *data, size_t length)
{
    TestTracker *tracker = (TestTracker *)userdata;

    Log(tracker, 'g', data, length);
    if (tracker->get_result < 0 || data[0] != 0x02) {
        return -1;
    }
    memcpy(data, tracker->config, length < 7 ? length : 7);
    return tracker->get_result;
}

static bool TestSet(void *userdata, const uint8_t *data, size_t length)
{
    TestTracker *tracker = (TestTracker *)userdata;

    Log(tracker, 's', data, length);
    if (data[0] == 0x02 && length == 7) {
        memcpy(tracker->config, data, 7);
    }
    return true;
}

static SDL_RiftDK1Sink Sink(TestTracker *tracker)
{
    SDL_RiftDK1Sink sink;

    sink.userdata = tracker;
    sink.get_feature = TestGet;
    sink.set_feature = TestSet;
    return sink;
}

static void TestStart(void)
{
    static const uint8_t reply[7] = { 0x02, 0x00, 0x00, 0x60, 0x00, 0xE8, 0x03 };
    static const uint8_t config[7] = { 0x02, 0x00, 0x00, 0x20, 0x01, 0xE8, 0x03 };
    static const uint8_t keepalive[5] = { 0x08, 0x00, 0x00, 0x10, 0x27 };
    TestTracker tracker;
    SDL_RiftDK1Sink sink = Sink(&tracker);
    SDL_RiftDK1Session session;
    uint64_t t;
    int keepalives;

    /* 1 */
    memset(&tracker, 0, sizeof(tracker));
    tracker.get_result = 7;
    memcpy(tracker.config, reply, 7);
    SDL_RiftDK1_Start(&session, 0, &sink);
    CHECK(tracker.calls == 4);
    CHECK(tracker.kind[0] == 'g' && tracker.data[0][0] == 0x02 && tracker.length[0] == 7);
    CHECK(tracker.kind[1] == 's' && tracker.length[1] == 7 && memcmp(tracker.data[1], config, 7) == 0);
    CHECK(tracker.kind[2] == 'g' && tracker.data[2][0] == 0x02);
    CHECK(tracker.kind[3] == 's' && tracker.length[3] == 5 && memcmp(tracker.data[3], keepalive, 5) == 0);

    /* 2: keep-alives at 0, 3000 and 6000 ms, nothing between */
    keepalives = 1;
    for (t = 0; t <= 6500; ++t) {
        tracker.now = t;
        SDL_RiftDK1_Update(&session, t, &sink);
    }
    for (t = 4; t < (uint64_t)tracker.calls && t < MAX_LOG; ++t) {
        CHECK(tracker.kind[t] == 's' && memcmp(tracker.data[t], keepalive, 5) == 0);
        ++keepalives;
    }
    CHECK(keepalives == 3 && tracker.time[4] == 3000 && tracker.time[5] == 6000);

    /* A failed read writes no configuration and still sends the keep-alive */
    memset(&tracker, 0, sizeof(tracker));
    tracker.get_result = -1;
    SDL_RiftDK1_Start(&session, 0, &sink);
    CHECK(tracker.calls == 2 && tracker.kind[0] == 'g' && tracker.kind[1] == 's');
    CHECK(memcmp(tracker.data[1], keepalive, 5) == 0);

    /* 9: a reconnect repeats the start-up and restarts the keep-alive clock */
    memset(&tracker, 0, sizeof(tracker));
    tracker.get_result = 7;
    memcpy(tracker.config, reply, 7);
    SDL_RiftDK1_Start(&session, 10000, &sink);
    CHECK(tracker.calls == 4 && session.next_keepalive_ms == 13000);
    SDL_RiftDK1_Update(&session, 12999, &sink);
    CHECK(tracker.calls == 4);
    SDL_RiftDK1_Update(&session, 13000, &sink);
    CHECK(tracker.calls == 5);
}

static void BuildReport(uint8_t r[62], uint8_t count, uint16_t timestamp)
{
    memset(r, 0, 62);
    r[0] = 0x01;
    r[1] = count;
    r[2] = (uint8_t)(timestamp & 0xFF);
    r[3] = (uint8_t)(timestamp >> 8);
}

static void TestDecode(void)
{
    static const uint8_t accel[8] = { 0x00, 0x00, 0x00, 0x5F, 0xCD, 0x00, 0x00, 0x00 };
    static const uint8_t gyro[8] = { 0x00, 0x00, 0x00, 0x09, 0xC4, 0x00, 0x00, 0x00 };
    static const uint8_t mag[6] = { 0x64, 0x00, 0x38, 0xFF, 0x2C, 0x01 };
    static const uint8_t neg[8] = { 0xF4, 0x06, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t mixed[8] = { 0x7F, 0xFF, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x02 };
    SDL_RiftDK1Report report;
    SDL_RiftDK1Timing timing;
    SDL_RiftDK1Event events[4];
    uint8_t r[64], stale[80];
    int32_t v[3];
    size_t length;
    int n;

    /* 3 */
    BuildReport(r, 1, 0x1234);
    r[6] = 0x60;
    r[7] = 0x09;
    memcpy(&r[8], accel, 8);
    memcpy(&r[16], gyro, 8);
    memcpy(&r[56], mag, 6);
    CHECK(SDL_RiftDK1_DecodeReport(r, 62, &report));
    CHECK(report.sample_count == 1 && report.timestamp == 0x1234 && report.temperature == 2400);
    CHECK(report.accel[0][0] == 0 && report.accel[0][1] == 98100 && report.accel[0][2] == 0);
    CHECK(report.gyro[0][0] == 0 && report.gyro[0][1] == 10000 && report.gyro[0][2] == 0);
    CHECK(report.mag[0] == 100 && report.mag[1] == -200 && report.mag[2] == 300);
    memset(&timing, 0, sizeof(timing));
    n = SDL_RiftDK1_TimeReport(&timing, &report, events);
    CHECK(n == 1 && !events[0].repeat);
    CHECK(fabsf(events[0].accel[1] - 9.81f) < 1e-4f && events[0].accel[0] == 0 && events[0].accel[2] == 0);
    CHECK(fabsf(events[0].gyro[1] - 1.0f) < 1e-6f && events[0].gyro[0] == 0 && events[0].gyro[2] == 0);

    /* 4 */
    SDL_RiftDK1_Unpack21(neg, v);
    CHECK(v[0] == -98100 && v[1] == 0 && v[2] == 0);
    SDL_RiftDK1_Unpack21(mixed, v);
    CHECK(v[0] == 1048575 && v[1] == -1048576 && v[2] == 1);

    /* 7: every prefix and any other first byte change nothing, with 0xFF past the received length */
    for (length = 0; length < 62; ++length) {
        uint8_t *copy = (uint8_t *)malloc(length ? length : 1);
        if (length) {
            memcpy(copy, r, length);
        }
        CHECK(!SDL_RiftDK1_DecodeReport(copy, length, &report));
        free(copy);
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, r, length);
        CHECK(!SDL_RiftDK1_DecodeReport(stale, length, &report));
    }
    r[0] = 0x02;
    CHECK(!SDL_RiftDK1_DecodeReport(r, 62, &report));
    r[0] = 0x01;
    /* Longer reads decode the first 62 bytes, as the SDK does */
    memset(&r[62], 0xFF, 2);
    CHECK(SDL_RiftDK1_DecodeReport(r, 63, &report) && report.accel[0][1] == 98100 && report.mag[2] == 300);
    CHECK(SDL_RiftDK1_DecodeReport(r, 64, &report) && report.accel[0][1] == 98100 && report.mag[2] == 300);
    {
        uint8_t *padded = (uint8_t *)malloc(100);
        CHECK(padded != NULL);
        if (padded) {
            memset(padded, 0xFF, 100);
            memcpy(padded, r, 62);
            CHECK(SDL_RiftDK1_DecodeReport(padded, 100, &report) && report.accel[0][1] == 98100 &&
                  report.mag[0] == 100 && report.mag[1] == -200 && report.mag[2] == 300);
            free(padded);
        }
    }
    CHECK(!SDL_RiftDK1_DecodeReport(NULL, 62, &report) && !SDL_RiftDK1_DecodeReport(r, 62, NULL));
    CHECK(SDL_RiftDK1_IsManufacturer("Oculus VR, Inc.") && !SDL_RiftDK1_IsManufacturer("VR-Tek") && !SDL_RiftDK1_IsManufacturer(NULL));
}

static void TestTiming(void)
{
    SDL_RiftDK1Report report;
    SDL_RiftDK1Timing timing;
    SDL_RiftDK1Event events[4];
    uint64_t last;
    int n;

    /* 5: count 3, three pairs 1 ms apart */
    memset(&timing, 0, sizeof(timing));
    memset(&report, 0, sizeof(report));
    report.sample_count = 3;
    report.timestamp = 100;
    n = SDL_RiftDK1_TimeReport(&timing, &report, events);
    CHECK(n == 3 && events[0].time_us == 1000 && events[1].time_us == 2000 && events[2].time_us == 3000);

    /* After count 5, a report 5 ms later with count 5: pairs 3, 4 and 5 ms after the last */
    report.sample_count = 5;
    report.timestamp = 105;
    n = SDL_RiftDK1_TimeReport(&timing, &report, events);
    last = events[n - 1].time_us;
    report.timestamp = 110;
    n = SDL_RiftDK1_TimeReport(&timing, &report, events);
    CHECK(n == 3 && !events[0].repeat);
    CHECK(events[0].time_us == last + 3000 && events[1].time_us == last + 4000 && events[2].time_us == last + 5000);

    /* After count 2, the same report repeats the last pair 3 ms later, then pairs 3, 4 and 5 ms after that */
    report.sample_count = 2;
    report.timestamp = 200;
    n = SDL_RiftDK1_TimeReport(&timing, &report, events);
    last = events[n - 1].time_us;
    report.accel[1][1] = 777;
    report.sample_count = 5;
    report.timestamp = 205;
    n = SDL_RiftDK1_TimeReport(&timing, &report, events);
    CHECK(n == 4 && events[0].repeat && events[0].time_us == last + 3000);
    CHECK(events[1].time_us == last + 6000 && events[2].time_us == last + 7000 && events[3].time_us == last + 8000);
    CHECK(!events[1].repeat && fabsf(events[2].accel[1] - 0.0777f) < 1e-6f);
    /* A jump above 254 repeats nothing */
    report.sample_count = 1;
    report.timestamp = (uint16_t)(205 + 300);
    n = SDL_RiftDK1_TimeReport(&timing, &report, events);
    CHECK(n == 1 && !events[0].repeat);
    /* A jump of 254 still repeats, 253 ms after the last pair, and 255 does not */
    last = events[0].time_us;
    report.timestamp = (uint16_t)(205 + 300 + 254);
    n = SDL_RiftDK1_TimeReport(&timing, &report, events);
    CHECK(n == 2 && events[0].repeat && !events[1].repeat && events[0].time_us == last + 253000);
    report.timestamp = (uint16_t)(205 + 300 + 254 + 255);
    n = SDL_RiftDK1_TimeReport(&timing, &report, events);
    CHECK(n == 1 && !events[0].repeat);

    /* 6: 0xFFFF then 0x0000 is 1 ms */
    memset(&timing, 0, sizeof(timing));
    report.sample_count = 1;
    report.timestamp = 0xFFFF;
    n = SDL_RiftDK1_TimeReport(&timing, &report, events);
    last = events[0].time_us;
    report.timestamp = 0x0000;
    n = SDL_RiftDK1_TimeReport(&timing, &report, events);
    CHECK(n == 1 && !events[0].repeat && events[0].time_us == last + 1000);
    /* Count 0 gives no sample */
    report.sample_count = 0;
    report.timestamp = 0x0001;
    CHECK(SDL_RiftDK1_TimeReport(&timing, &report, events) == 0);
}

static void TestFusion(void)
{
    SDL_RiftDK1Session session;
    SDL_RiftDK1Report report;
    SDL_RiftDK1Event events[4];
    float q[4], angles[3];
    int i;

    /* 8: 1000 samples 1 ms apart at 1 rad/s about Y, level, turn yaw by 57.3 degrees */
    memset(&session, 0, sizeof(session));
    SDL_RiftDK1_FusionInit(&session.fusion);
    memset(&report, 0, sizeof(report));
    report.sample_count = 1;
    report.gyro[0][1] = 10000;
    report.accel[0][1] = 98100;
    for (i = 0; i < 1000; ++i) {
        report.timestamp = (uint16_t)i;
        CHECK(SDL_RiftDK1_HandleReport(&session, &report, events) == 1);
    }
    SDL_RiftDK1_Angles(session.fusion.orient, angles);
    CHECK(fabsf(angles[0] * 180.0f / PI_F - 57.3f) < 0.5f);
    CHECK(fabsf(angles[1]) < 0.01f && fabsf(angles[2]) < 0.01f);
    CHECK(session.axes[0] == SDL_RiftDK1_AngleAxis(angles[0]));

    /* The angles of a rotation built as yaw, then pitch, then roll */
    {
        const float yaw = 0.7f, pitch = -0.4f, roll = 1.1f;
        float qy[4] = { 0, sinf(yaw / 2), 0, cosf(yaw / 2) };
        float qx[4] = { sinf(pitch / 2), 0, 0, cosf(pitch / 2) };
        float qz[4] = { 0, 0, sinf(roll / 2), cosf(roll / 2) };
        float t[4];
        /* t = qy * qx, then q = t * qz */
        t[0] = qy[3] * qx[0] + qy[0] * qx[3] + qy[1] * qx[2] - qy[2] * qx[1];
        t[1] = qy[3] * qx[1] - qy[0] * qx[2] + qy[1] * qx[3] + qy[2] * qx[0];
        t[2] = qy[3] * qx[2] + qy[0] * qx[1] - qy[1] * qx[0] + qy[2] * qx[3];
        t[3] = qy[3] * qx[3] - qy[0] * qx[0] - qy[1] * qx[1] - qy[2] * qx[2];
        q[0] = t[3] * qz[0] + t[0] * qz[3] + t[1] * qz[2] - t[2] * qz[1];
        q[1] = t[3] * qz[1] - t[0] * qz[2] + t[1] * qz[3] + t[2] * qz[0];
        q[2] = t[3] * qz[2] + t[0] * qz[1] - t[1] * qz[0] + t[2] * qz[3];
        q[3] = t[3] * qz[3] - t[0] * qz[0] - t[1] * qz[1] - t[2] * qz[2];
        SDL_RiftDK1_Angles(q, angles);
        CHECK(fabsf(angles[0] - yaw) < 1e-4f && fabsf(angles[1] - pitch) < 1e-4f && fabsf(angles[2] - roll) < 1e-4f);
    }

    /* The axis spans -180 to +180 degrees */
    CHECK(SDL_RiftDK1_AngleAxis(0.0f) == 0);
    CHECK(SDL_RiftDK1_AngleAxis(-PI_F) == -32767);
    CHECK(SDL_RiftDK1_AngleAxis(-4.0f) == -32767 && SDL_RiftDK1_AngleAxis(4.0f) == 32767);
    CHECK(SDL_RiftDK1_AngleAxis(PI_F) == 32767);
    CHECK(SDL_RiftDK1_AngleAxis(PI_F / 2) == 16384);
    CHECK(SDL_RiftDK1_AngleAxis(-PI_F / 2) == -16384);

    /* Level and still: gravity correction keeps the orientation where it is */
    memset(&session, 0, sizeof(session));
    SDL_RiftDK1_FusionInit(&session.fusion);
    memset(&report, 0, sizeof(report));
    report.sample_count = 1;
    report.accel[0][1] = 98200;
    for (i = 0; i < 500; ++i) {
        report.timestamp = (uint16_t)i;
        (void)SDL_RiftDK1_HandleReport(&session, &report, events);
    }
    CHECK(session.axes[0] == 0 && session.axes[1] == 0 && session.axes[2] == 0);

    /* Gravity along the tracker's X. After 51 level samples in a row, and
       within the first 2000, the correction turns the orientation all the
       way, 90 degrees about Z, so the measured gravity points up. */
    memset(&session, 0, sizeof(session));
    SDL_RiftDK1_FusionInit(&session.fusion);
    memset(&report, 0, sizeof(report));
    report.sample_count = 1;
    report.accel[0][0] = 98200;
    for (i = 0; i < 50; ++i) {
        report.timestamp = (uint16_t)i;
        (void)SDL_RiftDK1_HandleReport(&session, &report, events);
    }
    CHECK(session.fusion.orient[0] == 0 && session.fusion.orient[1] == 0 && session.fusion.orient[2] == 0 && session.fusion.orient[3] == 1.0f);
    report.timestamp = 50;
    (void)SDL_RiftDK1_HandleReport(&session, &report, events);
    CHECK(fabsf(session.fusion.orient[0]) < 1e-5f && fabsf(session.fusion.orient[1]) < 1e-5f);
    CHECK(fabsf(session.fusion.orient[2] - 0.70710678f) < 1e-4f && fabsf(session.fusion.orient[3] - 0.70710678f) < 1e-4f);
    CHECK(session.axes[2] == 16384 && session.axes[0] == 0 && session.axes[1] == 0);
}

int main(void)
{
    TestStart();
    TestDecode();
    TestTiming();
    TestFusion();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
