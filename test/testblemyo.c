/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/ble/SDL_ble_myo_proto.c, the Thalmic Myo
   armband of hifihedgehog/SDL#33 Part 12. Test numbers follow the part's
   Myo section. The values are built from myo-bluetooth's myohw.h, read
   little-endian as pyomyo, dl-myo and myo-raw read the streams, except the
   device values dl-myo's tests hold, restated in TestDlMyo. */

#include "testbleharness.h"
#include "SDL_ble_myo_proto.h"

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define PI 3.14159265358979323846
#define ALL_POSES 0x1Fu

static const uint64_t myo_address = 0xD5060001A904;

static const uint8_t set_mode[5] = { 0x01, 0x03, 0x00, 0x03, 0x01 };
static const uint8_t never_sleep[3] = { 0x09, 0x01, 0x01 };
static const uint8_t unlock_hold[3] = { 0x0A, 0x01, 0x02 };
static const uint8_t normal_sleep[3] = { 0x09, 0x01, 0x00 };
static const uint8_t lock[3] = { 0x0A, 0x01, 0x00 };

/* myohw.h:63-93: IMU data notify-only, motion event and classifier event
   indicate-only, command write-only, battery read and notify */
static void Properties(uint8_t *properties)
{
    memset(properties, 0, SDL_BLE_MAX_CHARS);
    properties[SDL_MYO_IMU] = SDL_BLE_PROPERTY_NOTIFY;
    properties[SDL_MYO_CLASSIFIER] = SDL_BLE_PROPERTY_INDICATE;
    properties[SDL_MYO_MOTION] = SDL_BLE_PROPERTY_INDICATE;
    properties[SDL_MYO_COMMAND] = SDL_BLE_PROPERTY_WRITE;
    properties[SDL_MYO_BATTERY] = SDL_BLE_PROPERTY_READ | SDL_BLE_PROPERTY_NOTIFY;
}

/* Through the subscriptions and the battery read, the first command out */
static void StartUp(BH_Harness *h, uint8_t battery)
{
    uint8_t properties[SDL_BLE_MAX_CHARS];

    Properties(properties);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    BH_ReadAll(h, &battery, 1);
}

/* A session with its start-up written */
static BH_Harness *Started(void)
{
    BH_Harness *h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);

    h->now = 1000;
    StartUp(h, 0x55);
    BH_WriteAll(h);
    return h;
}

static void PutInt16(uint8_t *out, int value)
{
    out[0] = (uint8_t)(value & 0xFF);
    out[1] = (uint8_t)((value >> 8) & 0xFF);
}

/* An IMU value: the quaternion, the accelerometer and the gyroscope, raw */
static void Imu(uint8_t *out, int w, int x, int y, int z, const int *accel, const int *gyro)
{
    int i;

    PutInt16(&out[0], w);
    PutInt16(&out[2], x);
    PutInt16(&out[4], y);
    PutInt16(&out[6], z);
    for (i = 0; i < 3; ++i) {
        PutInt16(&out[8 + 2 * i], accel ? accel[i] : 0);
        PutInt16(&out[14 + 2 * i], gyro ? gyro[i] : 0);
    }
}

static void Feed(BH_Harness *h, int characteristic, const uint8_t *data, size_t length)
{
    BH_Value(h, characteristic, data, length, false);
}

/* A published session holding the identity quaternion */
static BH_Harness *Published(void)
{
    BH_Harness *h = Started();
    uint8_t imu[SDL_MYO_IMU_SIZE];

    Imu(imu, 16384, 0, 0, 0, NULL, NULL);
    Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
    return h;
}

static void Classifier(BH_Harness *h, uint8_t type, uint16_t pose, size_t length)
{
    uint8_t event[6] = { 0, 0, 0, 0, 0, 0 };

    event[0] = type;
    event[1] = (uint8_t)(pose & 0xFF);
    event[2] = (uint8_t)(pose >> 8);
    Feed(h, SDL_MYO_CLASSIFIER, event, length);
}

/* A value that must change nothing */
static void Unchanged(BH_Harness *h, int characteristic, const uint8_t *data, size_t length, const char *what)
{
    const SDL_BLEControls before = *BH_Controls(h);
    const int snapshots = h->nsnapshots;
    const int samples = h->nsamples;

    Feed(h, characteristic, data, length);
    BH_CHECK(h->nsnapshots == snapshots && h->nsamples == samples && SDL_BLE_ControlsEqual(BH_Controls(h), &before),
             "%s: changed something", what);
}

static void CheckAxes(const BH_Harness *h, int roll, int pitch, int yaw, const char *what)
{
    BH_CHECK(BH_Axis(h, SDL_MYO_AXIS_ROLL) == roll && BH_Axis(h, SDL_MYO_AXIS_PITCH) == pitch &&
                 BH_Axis(h, SDL_MYO_AXIS_YAW) == yaw,
             "%s: axes (%d, %d, %d), expected (%d, %d, %d)", what, BH_Axis(h, SDL_MYO_AXIS_ROLL),
             BH_Axis(h, SDL_MYO_AXIS_PITCH), BH_Axis(h, SDL_MYO_AXIS_YAW), roll, pitch, yaw);
}

static bool Near(double a, double b, double tolerance)
{
    return fabs(a - b) <= tolerance;
}

/* Radians to the axis range as the part gives it: degrees x 32767 / 180,
   truncated */
static int ToAxis(double radians)
{
    return (int)(radians * 180.0 / PI * 32767.0 / 180.0);
}

/* The last sample of a sensor */
static const BH_Sample *LastSample(const BH_Harness *h, int sensor)
{
    int i;

    for (i = h->nsamples - 1; i >= 0; --i) {
        if (h->samples[i].sensor == sensor) {
            return &h->samples[i];
        }
    }
    return NULL;
}

static void TestIdentity(void)
{
    BH_Harness *h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    const SDL_BLEIdentity *id = &((const SDL_BLEBase *)h->state)->identity;

    printf("Identity\n");
    BH_CHECK(strcmp(id->name, "Thalmic Myo Armband") == 0, "name %s", id->name);
    BH_CHECK(id->type == SDL_BLE_TYPE_UNKNOWN && !id->gamepad, "a plain joystick, not a gamepad");
    BH_CHECK(id->buttons == ALL_POSES && id->axes == 0x07, "five buttons and three axes");
    BH_CHECK(id->accel_rate == 50.0f && id->gyro_rate == 50.0f, "50 Hz sensors (myohw.h:304)");
    BH_CHECK(id->vendor == 0 && id->product == 0 && !id->touchpad, "no VID, PID or touchpad");
    BH_CHECK(SDL_MYO_BUTTON_FIST == 0 && SDL_MYO_BUTTON_WAVE_IN == 1 && SDL_MYO_BUTTON_WAVE_OUT == 2 &&
                 SDL_MYO_BUTTON_SPREAD == 3 && SDL_MYO_BUTTON_DOUBLE_TAP == 4,
             "buttons in the part's order");
    BH_CHECK(SDL_MYO_AXIS_ROLL == 0 && SDL_MYO_AXIS_PITCH == 1 && SDL_MYO_AXIS_YAW == 2, "roll, pitch, yaw");
    BH_Destroy(h);

    /* A module reset without a sink decodes an IMU value, its two samples
       included, without calling one */
    {
        SDL_MyoState state;
        uint8_t imu[SDL_MYO_IMU_SIZE];

        memset(&state, 0x5A, sizeof(state));
        SDL_BLEMyoModule.Reset(&state, NULL, NULL);
        SDL_BLEMyoModule.Start(&state, 0);
        Imu(imu, 11585, 11585, 0, 0, NULL, NULL);
        SDL_BLEMyoModule.Value(&state, SDL_MYO_IMU, imu, sizeof(imu), 0);
        BH_CHECK(state.base.ready && state.base.changes == 1 && state.base.controls.axes[SDL_MYO_AXIS_ROLL] == 16383,
                 "an IMU value without a sink");
    }
}

static void Test1(void)
{
    BH_Harness *h = Published();
    int pose;

    printf("Test 1: poses\n");
    for (pose = 1; pose <= 5; ++pose) {
        Classifier(h, 0x03, (uint16_t)pose, 3);
        BH_CHECK(BH_Controls(h)->buttons == (1u << (pose - 1)), "pose %04X holds button %d alone", pose, pose - 1);
    }
    Classifier(h, 0x03, 0x0001, 3);
    Classifier(h, 0x03, 0x0000, 3);
    BH_CHECK(BH_Controls(h)->buttons == 0, "rest releases");
    Classifier(h, 0x03, 0x0004, 3);
    Classifier(h, 0x03, 0xFFFF, 3);
    BH_CHECK(BH_Controls(h)->buttons == 0, "unknown releases");
    BH_Destroy(h);
}

static void Test2(void)
{
    BH_Harness *h = Published();
    const uint8_t six[6] = { 0x03, 0x03, 0x00, 0x00, 0x00, 0x00 };
    const uint8_t filled[6] = { 0x03, 0x05, 0x00, 0xFF, 0xFF, 0xFF };
    size_t length;

    printf("Test 2: the 6-byte form\n");
    Feed(h, SDL_MYO_CLASSIFIER, six, sizeof(six));
    BH_CHECK(BH_Controls(h)->buttons == (1u << SDL_MYO_BUTTON_WAVE_OUT), "03 03 00 00 00 00 holds button 2 only");

    /* 3, 4, 5 and 6 bytes read alike, whatever follows byte 2 */
    for (length = 3; length <= 6; ++length) {
        Classifier(h, 0x03, 0x0000, 3);
        Feed(h, SDL_MYO_CLASSIFIER, filled, length);
        BH_CHECK(BH_Controls(h)->buttons == (1u << SDL_MYO_BUTTON_DOUBLE_TAP), "%u bytes: double tap", (unsigned)length);
    }
    BH_Destroy(h);
}

static void Test3(void)
{
    BH_Harness *h = Published();
    uint8_t event[3];
    char what[64];
    int type;
    long pose;

    printf("Test 3: releases and ignored events\n");
    Classifier(h, 0x03, 0x0002, 3);
    Classifier(h, 0x02, 0x0000, 3);
    BH_CHECK(BH_Controls(h)->buttons == 0, "02 00 00, arm unsynced, releases all");
    Classifier(h, 0x03, 0x0003, 3);
    Classifier(h, 0x05, 0x0000, 3);
    BH_CHECK(BH_Controls(h)->buttons == 0, "05 00 00, locked, releases all");

    Classifier(h, 0x03, 0x0001, 3);
    event[0] = 0x01;
    event[1] = 0x01;
    event[2] = 0x01;
    Unchanged(h, SDL_MYO_CLASSIFIER, event, 3, "01 01 01, arm synced");
    for (type = 0x07; type <= 0xFF; ++type) {
        event[0] = (uint8_t)type;
        event[1] = 0x00;
        event[2] = 0x00;
        (void)snprintf(what, sizeof(what), "type %02X", type);
        Unchanged(h, SDL_MYO_CLASSIFIER, event, 3, what);
    }
    for (pose = 0x0006; pose <= 0xFFFE; ++pose) {
        event[0] = 0x03;
        event[1] = (uint8_t)(pose & 0xFF);
        event[2] = (uint8_t)(pose >> 8);
        (void)snprintf(what, sizeof(what), "pose %04lX", pose);
        Unchanged(h, SDL_MYO_CLASSIFIER, event, 3, what);
    }
    BH_CHECK(BH_Controls(h)->buttons == (1u << SDL_MYO_BUTTON_FIST), "the fist is still held");
    BH_Destroy(h);
}

/* Every type from 00 to FF with every pose the part names */
static void TestEveryType(void)
{
    static const uint16_t poses[] = { 0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x00FF, 0x0100, 0xFFFE, 0xFFFF };
    BH_Harness *h = Published();
    size_t i;
    int type;

    printf("Every classifier type\n");
    for (type = 0; type <= 0xFF; ++type) {
        for (i = 0; i < COUNT(poses); ++i) {
            uint32_t expected = 1u << SDL_MYO_BUTTON_SPREAD;

            Classifier(h, 0x03, 0x0004, 3);
            Classifier(h, (uint8_t)type, poses[i], 3);
            if (type == 0x02 || type == 0x05) {
                expected = 0;
            } else if (type == 0x03) {
                if (poses[i] == 0x0000 || poses[i] == 0xFFFF) {
                    expected = 0;
                } else if (poses[i] >= 0x0001 && poses[i] <= 0x0005) {
                    expected = 1u << (poses[i] - 1);
                }
            }
            BH_CHECK(BH_Controls(h)->buttons == expected, "type %02X pose %04X: buttons %02x, expected %02x", type,
                     poses[i], (unsigned)BH_Controls(h)->buttons, (unsigned)expected);
        }
    }

    /* A pose uses bytes 1 and 2 as one little-endian value (dl-myo
       myo/types.py:36): 01 01 is pose 0101, not a fist */
    Classifier(h, 0x03, 0x0000, 3);
    Classifier(h, 0x03, 0x0101, 3);
    BH_CHECK(BH_Controls(h)->buttons == 0, "pose 0101 is not pose 0001");
    BH_Destroy(h);
}

static void Test4(void)
{
    BH_Harness *h = Started();
    const uint8_t imu[SDL_MYO_IMU_SIZE] = { 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                            0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    const BH_Sample *accel, *gyro;

    printf("Test 4: the identity quaternion\n");
    Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
    CheckAxes(h, 0, 0, 0, "test 4");
    accel = LastSample(h, SDL_BLE_SENSOR_ACCEL);
    gyro = LastSample(h, SDL_BLE_SENSOR_GYRO);
    BH_CHECK(accel && accel->data[0] == 0.0f && Near(accel->data[1], 9.80665, 1e-5) && accel->data[2] == 0.0f,
             "accelerometer (0, 1 g, 0)");
    BH_CHECK(gyro && gyro->data[0] == 0.0f && gyro->data[1] == 0.0f && gyro->data[2] == 0.0f, "gyroscope 0");
    BH_CHECK(h->published, "the first IMU value publishes");

    /* Big-endian, W would be 64 and the axes would not be (0, 0, 0) */
    BH_CHECK(imu[0] == 0x00 && imu[1] == 0x40, "W is 00 40, 16384 little-endian, 64 big-endian");
    BH_Destroy(h);
}

static void Test5(void)
{
    BH_Harness *h = Published();
    uint8_t imu[SDL_MYO_IMU_SIZE];
    const uint8_t yaw[SDL_MYO_IMU_SIZE] = { 0x41, 0x2D, 0, 0, 0, 0, 0x41, 0x2D };
    const uint8_t roll[SDL_MYO_IMU_SIZE] = { 0x41, 0x2D, 0x41, 0x2D };

    printf("Test 5: a quarter turn\n");
    Feed(h, SDL_MYO_IMU, yaw, sizeof(yaw));
    CheckAxes(h, 0, 0, 16383, "W and Z 41 2D");
    Feed(h, SDL_MYO_IMU, roll, sizeof(roll));
    CheckAxes(h, 16383, 0, 0, "W and X 41 2D");
    /* asin is steep near 1, so the quantized quarter turn about Y reads
       89.48 degrees where atan2 reads the others as 89.998 */
    Imu(imu, 11585, 0, 11585, 0, NULL, NULL);
    Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
    CheckAxes(h, 0, ToAxis(asin(2.0 * (11585.0 / 16384.0) * (11585.0 / 16384.0))), 0, "W and Y 11585");
    BH_CHECK(BH_Axis(h, SDL_MYO_AXIS_PITCH) > 16280 && BH_Axis(h, SDL_MYO_AXIS_PITCH) < 16300, "about 89.48 degrees");
    Imu(imu, 11585, 0, 0, -11585, NULL, NULL);
    Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
    CheckAxes(h, 0, 0, -16383, "W 11585 and Z -11585");
    BH_Destroy(h);
}

static void Test6(void)
{
    BH_Harness *h = Published();
    uint8_t imu[SDL_MYO_IMU_SIZE];
    const int gyro[3] = { 16, -32, 32000 };
    const int accel[3] = { -2048, 32767, -32768 };
    const BH_Sample *a, *g;

    printf("Test 6: sensor units\n");
    Imu(imu, 16384, 0, 0, 0, accel, gyro);
    h->now = 5000;
    Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
    a = LastSample(h, SDL_BLE_SENSOR_ACCEL);
    g = LastSample(h, SDL_BLE_SENSOR_GYRO);
    BH_CHECK(g && Near(g->data[0], 0.0174533, 1e-6), "gyroscope X raw 16 is 1 degree per second, %.7f rad/s",
             g ? g->data[0] : 0.0);
    BH_CHECK(g && Near(g->data[1], -2.0 * PI / 180.0, 1e-6) && Near(g->data[2], 2000.0 * PI / 180.0, 1e-4),
             "gyroscope Y -2 and Z 2000 degrees per second");
    BH_CHECK(a && Near(a->data[0], -9.80665, 1e-5) && Near(a->data[1], 32767.0 * 9.80665 / 2048.0, 1e-3) &&
                 Near(a->data[2], -16.0 * 9.80665, 1e-3),
             "accelerometer -1 g, +16 g and -16 g");
    BH_CHECK(a && g && a->time_ns == 5000000000ull && a->sensor_ns == 5000000000ull && g->time_ns == 5000000000ull &&
                 g->sensor_ns == 5000000000ull,
             "the receive time stamps both samples");
    BH_CHECK(h->nsamples >= 2 && h->samples[h->nsamples - 2].sensor == SDL_BLE_SENSOR_ACCEL &&
                 h->samples[h->nsamples - 1].sensor == SDL_BLE_SENSOR_GYRO,
             "one accelerometer sample, then one gyroscope sample");
    BH_Destroy(h);
}

/* Rotations built from roll, pitch and yaw, read back through the module
   and through a rotation of the basis vectors computed here */
static void Rotate(const double *q, const double *v, double *out)
{
    /* q v q* with v as a pure quaternion, divided by |q|^2 */
    const double n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    const double tw = -q[1] * v[0] - q[2] * v[1] - q[3] * v[2];
    const double tx = q[0] * v[0] + q[2] * v[2] - q[3] * v[1];
    const double ty = q[0] * v[1] + q[3] * v[0] - q[1] * v[2];
    const double tz = q[0] * v[2] + q[1] * v[1] - q[2] * v[0];

    out[0] = (-tw * q[1] + tx * q[0] - ty * q[3] + tz * q[2]) / n;
    out[1] = (-tw * q[2] + ty * q[0] - tz * q[1] + tx * q[3]) / n;
    out[2] = (-tw * q[3] + tz * q[0] - tx * q[2] + ty * q[1]) / n;
}

static void TestRotations(void)
{
    /* Pitch stays within 60 degrees, where asin's slope is at most 2 and the
       quantized quaternion moves an axis by a few counts at most */
    static const double angles[][3] = {
        { 30.0, 0.0, 0.0 }, { 0.0, -45.0, 0.0 }, { 0.0, 0.0, 120.0 }, { 10.0, 20.0, 30.0 },
        { -170.0, 5.0, -100.0 }, { 45.0, -60.0, 179.0 }, { -90.0, 40.0, -30.0 }, { 0.5, -0.5, 0.25 },
    };
    const double ex[3] = { 1.0, 0.0, 0.0 }, ey[3] = { 0.0, 1.0, 0.0 }, ez[3] = { 0.0, 0.0, 1.0 };
    BH_Harness *h = Published();
    uint8_t imu[SDL_MYO_IMU_SIZE];
    size_t i;

    printf("Rotations\n");
    for (i = 0; i < COUNT(angles); ++i) {
        const double r = angles[i][0] * PI / 180.0 / 2.0;
        const double p = angles[i][1] * PI / 180.0 / 2.0;
        const double y = angles[i][2] * PI / 180.0 / 2.0;
        double q[4], c0[3], c1[3], c2[3];
        int raw[4], k, roll, pitch, yaw;

        /* Z, then Y, then X, the order of the Tait-Bryan angles */
        q[0] = cos(r) * cos(p) * cos(y) + sin(r) * sin(p) * sin(y);
        q[1] = sin(r) * cos(p) * cos(y) - cos(r) * sin(p) * sin(y);
        q[2] = cos(r) * sin(p) * cos(y) + sin(r) * cos(p) * sin(y);
        q[3] = cos(r) * cos(p) * sin(y) - sin(r) * sin(p) * cos(y);
        for (k = 0; k < 4; ++k) {
            raw[k] = (int)floor(q[k] * 16384.0 + 0.5);
            q[k] = raw[k] / 16384.0;
        }
        Imu(imu, raw[0], raw[1], raw[2], raw[3], NULL, NULL);
        Feed(h, SDL_MYO_IMU, imu, sizeof(imu));

        /* The rotated basis gives the matrix columns */
        Rotate(q, ex, c0);
        Rotate(q, ey, c1);
        Rotate(q, ez, c2);
        roll = ToAxis(atan2(c1[2], c2[2]));
        pitch = ToAxis(asin(-c0[2] > 1.0 ? 1.0 : (-c0[2] < -1.0 ? -1.0 : -c0[2])));
        yaw = ToAxis(atan2(c0[1], c0[0]));
        BH_CHECK(abs(BH_Axis(h, SDL_MYO_AXIS_ROLL) - roll) <= 2 && abs(BH_Axis(h, SDL_MYO_AXIS_PITCH) - pitch) <= 2 &&
                     abs(BH_Axis(h, SDL_MYO_AXIS_YAW) - yaw) <= 2,
                 "rotation (%g, %g, %g): module (%d, %d, %d), basis vectors (%d, %d, %d)", angles[i][0], angles[i][1],
                 angles[i][2], BH_Axis(h, SDL_MYO_AXIS_ROLL), BH_Axis(h, SDL_MYO_AXIS_PITCH), BH_Axis(h, SDL_MYO_AXIS_YAW),
                 roll, pitch, yaw);
        BH_CHECK(abs(BH_Axis(h, SDL_MYO_AXIS_ROLL) - (int)(angles[i][0] * 32767.0 / 180.0)) <= 4 &&
                     abs(BH_Axis(h, SDL_MYO_AXIS_PITCH) - (int)(angles[i][1] * 32767.0 / 180.0)) <= 4 &&
                     abs(BH_Axis(h, SDL_MYO_AXIS_YAW) - (int)(angles[i][2] * 32767.0 / 180.0)) <= 4,
                 "rotation (%g, %g, %g) reads back as its angles", angles[i][0], angles[i][1], angles[i][2]);
    }

    /* 2(wy - zx) past 1, as a quaternion slightly longer than 1 gives, clamps
       to a pitch of 90 degrees instead of asin's NaN. Roll and yaw have no
       meaning there. */
    Imu(imu, 11600, 0, 11600, 0, NULL, NULL);
    Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
    BH_CHECK(BH_Axis(h, SDL_MYO_AXIS_PITCH) == 16383, "pitch past 90 degrees clamps, %d", BH_Axis(h, SDL_MYO_AXIS_PITCH));
    Imu(imu, 11600, 0, -11600, 0, NULL, NULL);
    Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
    BH_CHECK(BH_Axis(h, SDL_MYO_AXIS_PITCH) == -16383, "pitch past -90 degrees clamps, %d", BH_Axis(h, SDL_MYO_AXIS_PITCH));

    /* A half turn about X */
    Imu(imu, 0, 16384, 0, 0, NULL, NULL);
    Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
    CheckAxes(h, 32767, 0, 0, "a half turn about X");
    BH_Destroy(h);
}

/* Device values from dl-myo's tests (tests/test_myo/test_types.py, GPL-3.0,
   restated here as facts). Three IMU values, with the quaternion, the
   accelerometer in g and the gyroscope in degrees per second dl-myo reads
   from each (:89-119). The module reports the accelerometer in m/s^2 and
   the gyroscope in radians per second, so the first reads (1.5993, 9.4858,
   0.4166) m/s^2 and (0.00327, -0.01200, -0.00436) rad/s. And five
   classifier events of 6 bytes (:12-16). */
static void TestDlMyo(void)
{
    static const struct
    {
        const char *hex;
        double quaternion[4]; /* w, x, y, z */
        double accel[3];      /* g */
        double gyro[3];       /* degrees per second */
    } imu[3] = {
        { "3e2eab2be5f824004e01bd0757000300f5fffcff",
          { 0.7225341796875, 0.68231201171875, -0.11102294921875, 0.002197265625 },
          { 0.1630859375, 0.96728515625, 0.04248046875 },
          { 0.1875, -0.6875, -0.25 } },
        { "0a2ee12be5f810005b01c6075300f1ff0400f7ff",
          { 0.7193603515625, 0.68560791015625, -0.11102294921875, 0.0009765625 },
          { 0.16943359375, 0.9716796875, 0.04052734375 },
          { -0.9375, 0.25, -0.5625 } },
        { "5c2d9e2c23f99cff03fc1a06eaf94900feffe3ff",
          { 0.708740234375, 0.6971435546875, -0.10723876953125, -0.006103515625 },
          { -0.49853515625, 0.7626953125, -0.7607421875 },
          { 4.5625, -0.125, -1.8125 } },
    };
    /* Each event with the buttons after it. Arm synced, 01 01 02, is the
       right arm (01) with +X toward the elbow (02) (myohw.h:343, :351), and
       moves no button. */
    static const struct
    {
        const char *hex;
        uint32_t buttons;
        const char *what;
    } events[] = {
        { "030100000000", 1u << SDL_MYO_BUTTON_FIST, ":14, pose 0001, fist" },
        { "01010202d708", 1u << SDL_MYO_BUTTON_FIST, ":12, arm synced" },
        { "030500000000", 1u << SDL_MYO_BUTTON_DOUBLE_TAP, ":15, pose 0005, double tap" },
        { "030000000000", 0, ":13, pose 0000, rest" },
        { "030500000000", 1u << SDL_MYO_BUTTON_DOUBLE_TAP, ":15 again" },
        { "020000000000", 0, ":16, arm unsynced" },
    };
    const double ex[3] = { 1.0, 0.0, 0.0 }, ey[3] = { 0.0, 1.0, 0.0 }, ez[3] = { 0.0, 0.0, 1.0 };
    BH_Harness *h = Published();
    const BH_Sample *a, *g;
    uint8_t value[SDL_MYO_IMU_SIZE];
    size_t i;
    int k;

    printf("dl-myo's device values\n");
    for (i = 0; i < COUNT(imu); ++i) {
        double c0[3], c1[3], c2[3];
        int roll, pitch, yaw;

        BH_CHECK(BH_Hex(imu[i].hex, value, sizeof(value)) == SDL_MYO_IMU_SIZE, "IMU value %u has 20 bytes", (unsigned)i);
        /* The restated quaternion is the bytes little-endian over 16384 */
        for (k = 0; k < 4; ++k) {
            BH_CHECK((int16_t)(uint16_t)(value[2 * k] | (value[2 * k + 1] << 8)) / 16384.0 == imu[i].quaternion[k],
                     "IMU value %u: quaternion %d", (unsigned)i, k);
        }
        Feed(h, SDL_MYO_IMU, value, sizeof(value));
        a = LastSample(h, SDL_BLE_SENSOR_ACCEL);
        g = LastSample(h, SDL_BLE_SENSOR_GYRO);
        BH_CHECK(a && g && h->nsamples >= 2 && a == &h->samples[h->nsamples - 2] && g == &h->samples[h->nsamples - 1],
                 "IMU value %u: one accelerometer and one gyroscope sample", (unsigned)i);
        if (!a || !g) {
            continue;
        }
        for (k = 0; k < 3; ++k) {
            BH_CHECK(Near(a->data[k], imu[i].accel[k] * 9.80665, 1e-5), "IMU value %u: accelerometer %d reads %f m/s^2",
                     (unsigned)i, k, a->data[k]);
            BH_CHECK(Near(g->data[k], imu[i].gyro[k] * PI / 180.0, 1e-7), "IMU value %u: gyroscope %d reads %f rad/s",
                     (unsigned)i, k, g->data[k]);
        }
        /* The axes from dl-myo's quaternion, through the rotated basis */
        Rotate(imu[i].quaternion, ex, c0);
        Rotate(imu[i].quaternion, ey, c1);
        Rotate(imu[i].quaternion, ez, c2);
        roll = ToAxis(atan2(c1[2], c2[2]));
        pitch = ToAxis(asin(-c0[2] > 1.0 ? 1.0 : (-c0[2] < -1.0 ? -1.0 : -c0[2])));
        yaw = ToAxis(atan2(c0[1], c0[0]));
        BH_CHECK(abs(BH_Axis(h, SDL_MYO_AXIS_ROLL) - roll) <= 2 && abs(BH_Axis(h, SDL_MYO_AXIS_PITCH) - pitch) <= 2 &&
                     abs(BH_Axis(h, SDL_MYO_AXIS_YAW) - yaw) <= 2,
                 "IMU value %u: axes (%d, %d, %d), the quaternion gives (%d, %d, %d)", (unsigned)i,
                 BH_Axis(h, SDL_MYO_AXIS_ROLL), BH_Axis(h, SDL_MYO_AXIS_PITCH), BH_Axis(h, SDL_MYO_AXIS_YAW), roll, pitch, yaw);
        if (i == 0) {
            BH_CHECK(Near(a->data[0], 1.5993, 1e-4) && Near(a->data[1], 9.4858, 1e-4) && Near(a->data[2], 0.4166, 1e-4),
                     "the first IMU value in m/s^2");
            BH_CHECK(Near(g->data[0], 0.00327, 1e-5) && Near(g->data[1], -0.01200, 1e-5) && Near(g->data[2], -0.00436, 1e-5),
                     "the first IMU value in rad/s");
            /* Roll near 87 degrees, pitch and yaw near -9 */
            BH_CHECK(abs(BH_Axis(h, SDL_MYO_AXIS_ROLL) - (int)(87.4 * 32767.0 / 180.0)) < 40 &&
                         abs(BH_Axis(h, SDL_MYO_AXIS_PITCH) - (int)(-9.4 * 32767.0 / 180.0)) < 40 &&
                         abs(BH_Axis(h, SDL_MYO_AXIS_YAW) - (int)(-8.6 * 32767.0 / 180.0)) < 40,
                     "the first IMU value's angles");
        }
    }

    for (i = 0; i < COUNT(events); ++i) {
        uint8_t event[6];
        const int snapshots = h->nsnapshots;
        const uint32_t before = BH_Controls(h)->buttons;

        BH_CHECK(BH_Hex(events[i].hex, event, sizeof(event)) == sizeof(event), "event %s has 6 bytes", events[i].hex);
        Feed(h, SDL_MYO_CLASSIFIER, event, sizeof(event));
        BH_CHECK(BH_Controls(h)->buttons == events[i].buttons, "%s: buttons %02x, expected %02x", events[i].what,
                 (unsigned)BH_Controls(h)->buttons, (unsigned)events[i].buttons);
        BH_CHECK(h->nsnapshots == snapshots + ((before != events[i].buttons) ? 1 : 0), "%s: one change or none",
                 events[i].what);
    }
    BH_Destroy(h);
}

static void Test7(void)
{
    BH_Harness *h = Published();
    uint8_t imu[64], event[16];
    const int accel[3] = { 100, 200, 300 };
    const int gyro[3] = { 400, 500, 600 };
    SDL_BLEControls expected;
    size_t length;

    printf("Test 7: truncations and stale bytes\n");
    Imu(imu, 11585, 0, 0, 11585, accel, gyro);
    for (length = 0; length < SDL_MYO_IMU_SIZE; ++length) {
        Unchanged(h, SDL_MYO_IMU, imu, length, "a short IMU value");
    }
    event[0] = 0x03;
    event[1] = 0x01;
    event[2] = 0x00;
    for (length = 0; length < SDL_MYO_CLASSIFIER_SIZE; ++length) {
        Unchanged(h, SDL_MYO_CLASSIFIER, event, length, "a short classifier value");
    }

    /* Bytes past the layout, in the value or in the buffer, are ignored */
    memset(&imu[SDL_MYO_IMU_SIZE], 0x7F, sizeof(imu) - SDL_MYO_IMU_SIZE);
    Feed(h, SDL_MYO_IMU, imu, SDL_MYO_IMU_SIZE);
    expected = *BH_Controls(h);
    CheckAxes(h, 0, 0, 16383, "20 bytes");
    Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
    BH_CHECK(SDL_BLE_ControlsEqual(BH_Controls(h), &expected), "64 bytes read as 20");
    SDL_BLEMyoModule.Value(h->state, SDL_MYO_IMU, imu, SDL_MYO_IMU_SIZE, h->now * 1000000);
    BH_CHECK(SDL_BLE_ControlsEqual(BH_Controls(h), &expected), "stale buffer bytes are not read");
    memset(event, 0xFF, sizeof(event));
    event[0] = 0x03;
    event[1] = 0x02;
    event[2] = 0x00;
    SDL_BLEMyoModule.Value(h->state, SDL_MYO_CLASSIFIER, event, SDL_MYO_CLASSIFIER_SIZE, h->now * 1000000);
    BH_CHECK(BH_Controls(h)->buttons == (1u << SDL_MYO_BUTTON_WAVE_IN), "a classifier value in a longer buffer");
    BH_Destroy(h);
}

static void Test8(void)
{
    BH_Harness *h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    const SDL_BLEAction *a;
    uint64_t deadline;
    uint8_t imu[SDL_MYO_IMU_SIZE];
    int i, before;

    printf("Test 8: start-up and close\n");
    StartUp(h, 0x55);
    BH_WriteAll(h);
    BH_CHECK(h->nactions == 10, "ten actions, %d seen", h->nactions);
    if (h->nactions == 10) {
        BH_CHECK(h->actions[0].action.kind == SDL_BLE_ACTION_CONNECT && h->actions[1].action.kind == SDL_BLE_ACTION_DISCOVER,
                 "connect, discover");
        a = &h->actions[2].action;
        BH_CHECK(a->kind == SDL_BLE_ACTION_SUBSCRIBE && a->characteristic == SDL_MYO_IMU && a->cccd == SDL_BLE_CCCD_NOTIFY,
                 "IMU data with Notify");
        a = &h->actions[3].action;
        BH_CHECK(a->kind == SDL_BLE_ACTION_SUBSCRIBE && a->characteristic == SDL_MYO_CLASSIFIER &&
                     a->cccd == SDL_BLE_CCCD_INDICATE,
                 "classifier event with Indicate");
        a = &h->actions[4].action;
        BH_CHECK(a->kind == SDL_BLE_ACTION_SUBSCRIBE && a->characteristic == SDL_MYO_MOTION && a->cccd == SDL_BLE_CCCD_INDICATE,
                 "motion event with Indicate");
        a = &h->actions[5].action;
        BH_CHECK(a->kind == SDL_BLE_ACTION_SUBSCRIBE && a->characteristic == SDL_MYO_BATTERY && a->cccd == SDL_BLE_CCCD_NOTIFY,
                 "then the battery with Notify");
        a = &h->actions[6].action;
        BH_CHECK(a->kind == SDL_BLE_ACTION_READ && a->characteristic == SDL_MYO_BATTERY, "the battery read once");
        BH_CHECK(BH_IsWrite(h, 7, set_mode, sizeof(set_mode)) && BH_IsWrite(h, 8, never_sleep, sizeof(never_sleep)) &&
                     BH_IsWrite(h, 9, unlock_hold, sizeof(unlock_hold)),
                 "01 03 00 03 01, 09 01 01, 0A 01 02");
        for (i = 7; i < 10; ++i) {
            a = &h->actions[i].action;
            BH_CHECK(a->characteristic == SDL_MYO_COMMAND && a->response && !a->closing,
                     "write %d to the command characteristic with response", i - 7);
        }
    }
    BH_CHECK(BH_Controls(h)->battery == 85, "the battery read gives 85 percent");
    BH_CHECK(!h->published, "not published before input");

    /* No module timer, and nothing more on an injected clock until the
       session's start timeout */
    BH_Advance(h, h->session.start_deadline - 1);
    BH_CHECK(!SDL_BLEMyoModule.GetDeadline(h->state, &deadline) && h->nactions == 10,
             "nothing more before the start timeout");
    Imu(imu, 16384, 0, 0, 0, NULL, NULL);
    Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
    BH_CHECK(h->published, "published on the first IMU value");

    /* Close */
    Classifier(h, 0x03, 0x0001, 3);
    before = h->nactions;
    SDL_BLESession_Close(&h->session, h->now);
    BH_Drain(h);
    BH_WriteAll(h);
    BH_CHECK(h->nactions == before + 4, "two closing writes, remove, disconnect, %d seen", h->nactions - before);
    if (h->nactions == before + 4) {
        BH_CHECK(BH_IsWrite(h, before, normal_sleep, sizeof(normal_sleep)) &&
                     BH_IsWrite(h, before + 1, lock, sizeof(lock)),
                 "09 01 00, then 0A 01 00");
        for (i = before; i < before + 2; ++i) {
            a = &h->actions[i].action;
            BH_CHECK(a->characteristic == SDL_MYO_COMMAND && a->response && a->closing,
                     "closing write %d with response", i - before);
        }
        BH_CHECK(h->actions[before + 2].action.kind == SDL_BLE_ACTION_REMOVE &&
                     h->actions[before + 3].action.kind == SDL_BLE_ACTION_DISCONNECT,
                 "then remove and disconnect");
    }
    BH_CHECK(BH_Controls(h)->buttons == 0 && SDL_BLESession_Ended(&h->session), "close releases and ends");
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == 5, "five writes in all, never deep sleep");
    BH_Destroy(h);

    /* After its close the module makes nothing ready, whatever reaches it */
    h = Started();
    SDL_BLESession_Close(&h->session, h->now);
    BH_Drain(h);
    BH_WriteAll(h);
    Imu(imu, 16384, 0, 0, 0, NULL, NULL);
    SDL_BLEMyoModule.Value(h->state, SDL_MYO_IMU, imu, sizeof(imu), h->now * 1000000);
    BH_CHECK(SDL_BLESession_Ended(&h->session) && !h->published && !((const SDL_BLEBase *)h->state)->ready,
             "an IMU value after the close is not ready");
    BH_Destroy(h);
}

static void Test9(void)
{
    /* kMyoServiceInfoUuid in myohw.h:36-42 is little-endian */
    static const uint8_t myohw[16] = { 0x42, 0x48, 0x12, 0x4a, 0x7f, 0x2c, 0x48, 0x47,
                                       0xb9, 0xde, 0x04, 0xa9, 0x01, 0x00, 0x06, 0xd5 };
    static const char *const names[] = { "Myo", "Thalmic Myo", "Myo Armband", "My Myo", "" };
    SDL_BLEAdvertisement ad;
    SDL_BLEUUID service;
    size_t i;
    int k;

    printf("Test 9: discovery\n");
    for (k = 0; k < 16; ++k) {
        service.bytes[k] = myohw[15 - k];
    }
    BH_CHECK(SDL_BLEMyoFamily.nkeys == 1 && SDL_BLEMyoFamily.keys[0].kind == SDL_BLE_KEY_SERVICE &&
                 !SDL_BLEMyoFamily.keys[0].corroborating && SDL_BLE_UUIDEqual(&SDL_BLEMyoFamily.keys[0].uuid, &service),
             "the one key is the control service of myohw.h");
    for (i = 0; i < COUNT(names); ++i) {
        memset(&ad, 0, sizeof(ad));
        ad.address = myo_address;
        ad.has_name = true;
        memcpy(ad.name, names[i], strlen(names[i]));
        BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == -1,
                 "the name \"%s\" alone matches nothing", names[i]);
        ad.nservices = 1;
        ad.services[0] = service;
        BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == SDL_BLE_FAMILY_MYO,
                 "the service matches under the name \"%s\"", names[i]);
    }
    memset(&ad, 0, sizeof(ad));
    ad.kind = SDL_BLE_AD_SCAN_RESPONSE;
    ad.nservices = 3;
    ad.services[0] = SDL_BLE_UUID16(0x180F);
    ad.services[1] = SDL_BLE_UUID16(0x1800);
    ad.services[2] = service;
    BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == SDL_BLE_FAMILY_MYO,
             "the service last in a scan response, where myo-raw finds it (myo_raw.py:211-213)");
    ad.nservices = 1;
    ad.services[0] = service;
    ad.services[0].bytes[3] = 0x02;
    BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == -1, "the IMU service matches nothing");
}

/* The family table against myohw.h:63-85: the control, IMU and classifier
   services and the battery service, then IMU data and motion events on the
   IMU service, classifier events on the classifier service, the command on
   the control service and the battery level. dl-myo connects through Bleak
   without pairing (myo/core.py:380-395), so the family never pairs. */
static void TestFamily(void)
{
    static const struct
    {
        uint8_t service;
        uint16_t id; /* The short UUID, 0 for the battery level */
        uint8_t flags;
    } characteristics[5] = {
        { 1, 0x0402, SDL_BLE_CHAR_SUBSCRIBE },
        { 2, 0x0103, SDL_BLE_CHAR_SUBSCRIBE },
        { 1, 0x0502, SDL_BLE_CHAR_SUBSCRIBE },
        { 0, 0x0401, 0 },
        { 3, 0x0000, SDL_BLE_CHAR_SUBSCRIBE | SDL_BLE_CHAR_READ | SDL_BLE_CHAR_OPTIONAL },
    };
    static const uint16_t services[3] = { 0x0001, 0x0002, 0x0003 };
    const SDL_BLEUUID battery_service = SDL_BLE_UUID16(0x180F);
    const SDL_BLEUUID battery_level = SDL_BLE_UUID16(0x2A19);
    uint8_t properties[SDL_BLE_MAX_CHARS];
    SDL_BLEUUID uuid;
    BH_Harness *h;
    int i;

    printf("Family table\n");
    BH_CHECK(BH_Hex("d5060001-a904-deb9-4748-2c7f4a124842", uuid.bytes, sizeof(uuid.bytes)) == 16, "the base UUID");
    BH_CHECK(SDL_BLEMyoFamily.nservices == 4 && !SDL_BLEMyoFamily.has_alternate, "four services");
    for (i = 0; i < 3; ++i) {
        uuid.bytes[2] = (uint8_t)(services[i] >> 8);
        uuid.bytes[3] = (uint8_t)services[i];
        BH_CHECK(SDL_BLE_UUIDEqual(&SDL_BLEMyoFamily.services[i], &uuid), "service %d is d506%04x", i, services[i]);
    }
    BH_CHECK(SDL_BLE_UUIDEqual(&SDL_BLEMyoFamily.services[3], &battery_service), "service 3 is the battery service");
    BH_CHECK(SDL_BLEMyoFamily.ncharacteristics == 5, "five characteristics");
    for (i = 0; i < 5; ++i) {
        const SDL_BLECharacteristic *characteristic = &SDL_BLEMyoFamily.characteristics[i];

        if (characteristics[i].id) {
            uuid.bytes[2] = (uint8_t)(characteristics[i].id >> 8);
            uuid.bytes[3] = (uint8_t)characteristics[i].id;
        } else {
            uuid = battery_level;
        }
        BH_CHECK(SDL_BLE_UUIDEqual(&characteristic->uuid, &uuid) && characteristic->service == characteristics[i].service &&
                     characteristic->flags == characteristics[i].flags,
                 "characteristic %d: service %d, flags %02X", i, characteristic->service, characteristic->flags);
    }
    BH_CHECK(SDL_BLEMyoFamily.pairing == SDL_BLE_PAIR_NOT_NEEDED, "the Myo never pairs");

    /* With the pairing hint on, an authentication error backs off and pairs
       nothing */
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, true);
    Properties(properties);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PAIR) == 0 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1 &&
                 SDL_BLESession_Ended(&h->session),
             "an authentication error backs off without a pairing");
    BH_Destroy(h);
}

static void Test10(void)
{
    BH_Harness *h = Published();
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad;
    SDL_BLEMatch match;
    SDL_BLEModuleContext context;
    SDL_BLESink sink;
    uint8_t imu[SDL_MYO_IMU_SIZE];
    int before, writes;

    printf("Test 10: link loss and reconnect\n");
    memset(&ad, 0, sizeof(ad));
    ad.address = myo_address;
    ad.nservices = 1;
    ad.services[0] = SDL_BLEMyoFamily.keys[0].uuid;
    SDL_BLEHost_Init(&host);
    BH_CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, h->now, &match) &&
                 match.family == SDL_BLE_FAMILY_MYO,
             "the advertisement starts a session");

    Imu(imu, 11585, 11585, 0, 0, NULL, NULL);
    Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
    Classifier(h, 0x03, 0x0005, 6);
    BH_CHECK(BH_Controls(h)->buttons == (1u << SDL_MYO_BUTTON_DOUBLE_TAP) && BH_Axis(h, SDL_MYO_AXIS_ROLL) == 16383,
             "a pose and a roll are held");
    before = h->nactions;
    SDL_BLESession_Lost(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(BH_Controls(h)->buttons == 0, "link loss releases every button");
    CheckAxes(h, 0, 0, 0, "link loss");
    BH_CHECK(BH_Controls(h)->battery == 85, "link loss keeps the battery");
    BH_CHECK(h->nactions == before + 2 && h->actions[before].action.kind == SDL_BLE_ACTION_REMOVE &&
                 h->actions[before + 1].action.kind == SDL_BLE_ACTION_DISCONNECT,
             "remove and disconnect, no closing writes, no backoff");

    {
        SDL_BLEOutcome outcome;

        SDL_BLESession_GetOutcome(&h->session, &outcome);
        SDL_BLEHost_SessionEnded(&host, myo_address, &outcome, h->now);
    }
    BH_CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, h->now, &match),
             "the next advertisement reconnects");

    /* The next session on the same state memory repeats the start-up */
    writes = BH_Count(h, SDL_BLE_ACTION_WRITE);
    memset(&context, 0, sizeof(context));
    sink.userdata = h;
    sink.changed = BH_Changed;
    sink.sensor = BH_Sensor;
    sink.log = BH_Log;
    SDL_BLESession_Init(&h->session, &SDL_BLEMyoFamily, h->state, &sink, &context, false);
    BH_Drain(h);
    StartUp(h, 0x40);
    BH_WriteAll(h);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == writes + 3 && BH_IsWrite(h, h->nactions - 3, set_mode, sizeof(set_mode)) &&
                 BH_IsWrite(h, h->nactions - 2, never_sleep, sizeof(never_sleep)) &&
                 BH_IsWrite(h, h->nactions - 1, unlock_hold, sizeof(unlock_hold)),
             "the reconnect writes the start-up again");
    BH_CHECK(!h->published && BH_Controls(h)->battery == 64, "the new session reads the battery and waits for input");
    Imu(imu, 16384, 0, 0, 0, NULL, NULL);
    Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
    BH_CHECK(h->published, "and publishes on its first IMU value");
    BH_Destroy(h);
}

static void TestBattery(void)
{
    BH_Harness *h = Published();
    uint8_t level[4] = { 0, 0, 0, 0 };

    printf("Battery\n");
    level[0] = 0x64;
    Feed(h, SDL_MYO_BATTERY, level, 1);
    BH_CHECK(BH_Controls(h)->battery == 100, "64 is 100 percent");
    level[0] = 0x00;
    Feed(h, SDL_MYO_BATTERY, level, 1);
    BH_CHECK(BH_Controls(h)->battery == 0, "00 is 0 percent");
    level[0] = 0xFF;
    Feed(h, SDL_MYO_BATTERY, level, 1);
    BH_CHECK(BH_Controls(h)->battery == 100, "FF clamps to 100");
    level[0] = 0x2A;
    level[1] = 0x63;
    Feed(h, SDL_MYO_BATTERY, level, 2);
    BH_CHECK(BH_Controls(h)->battery == 42, "the first byte is the level");
    Unchanged(h, SDL_MYO_BATTERY, level, 0, "an empty battery value");
    BH_Destroy(h);

    /* A failed battery read leaves the level unknown and the start-up goes on */
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    {
        uint8_t properties[SDL_BLE_MAX_CHARS];

        Properties(properties);
        BH_Connected(h, true, false);
        BH_Discovered(h, true, properties);
        BH_SubscribeAll(h);
        BH_ReadAll(h, NULL, 0);
        BH_WriteAll(h);
    }
    BH_CHECK(BH_Controls(h)->battery == -1 && BH_Count(h, SDL_BLE_ACTION_WRITE) == 3, "no level, three writes");
    BH_Destroy(h);

    /* A Myo without the battery service starts the same */
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    {
        uint8_t properties[SDL_BLE_MAX_CHARS];
        bool found[SDL_BLE_MAX_CHARS];
        int i;

        Properties(properties);
        for (i = 0; i < SDL_BLE_MAX_CHARS; ++i) {
            found[i] = (i != SDL_MYO_BATTERY);
        }
        BH_Connected(h, true, false);
        SDL_BLESession_Discovered(&h->session, true, found, properties, h->now);
        BH_Drain(h);
        BH_SubscribeAll(h);
        BH_ReadAll(h, NULL, 0);
        BH_WriteAll(h);
    }
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == 3 && BH_Count(h, SDL_BLE_ACTION_READ) == 0 &&
                 BH_Count(h, SDL_BLE_ACTION_WRITE) == 3,
             "without the battery: three subscriptions and the three writes");
    BH_Destroy(h);
}

/* Ready comes with the first decoded input after the start-up */
static void TestReady(void)
{
    static const struct
    {
        int characteristic;
        uint8_t data[6];
        size_t length;
        bool publishes;
        const char *what;
    } cases[] = {
        { SDL_MYO_IMU, { 0 }, 20, true, "an IMU value" },
        { SDL_MYO_CLASSIFIER, { 0x03, 0x01, 0x00 }, 3, true, "a pose" },
        { SDL_MYO_CLASSIFIER, { 0x03, 0x00, 0x00 }, 3, true, "rest" },
        { SDL_MYO_CLASSIFIER, { 0x03, 0xFF, 0xFF }, 3, true, "the unknown pose" },
        { SDL_MYO_CLASSIFIER, { 0x01, 0x01, 0x01 }, 3, true, "arm synced" },
        { SDL_MYO_CLASSIFIER, { 0x02, 0x00, 0x00 }, 3, true, "arm unsynced" },
        { SDL_MYO_CLASSIFIER, { 0x04, 0x00, 0x00 }, 3, true, "unlocked" },
        { SDL_MYO_CLASSIFIER, { 0x05, 0x00, 0x00 }, 3, true, "locked" },
        { SDL_MYO_CLASSIFIER, { 0x06, 0x01, 0x00 }, 6, true, "sync failed" },
        { SDL_MYO_CLASSIFIER, { 0x03, 0x06, 0x00 }, 3, false, "pose 0006" },
        { SDL_MYO_CLASSIFIER, { 0x03, 0xFE, 0xFF }, 3, false, "pose FFFE" },
        { SDL_MYO_CLASSIFIER, { 0x00, 0x00, 0x00 }, 3, false, "type 00" },
        { SDL_MYO_CLASSIFIER, { 0x07, 0x00, 0x00 }, 3, false, "type 07" },
        { SDL_MYO_CLASSIFIER, { 0xFF, 0x00, 0x00 }, 3, false, "type FF" },
        { SDL_MYO_CLASSIFIER, { 0x03, 0x01 }, 2, false, "a short pose" },
        { SDL_MYO_IMU, { 0 }, 19, false, "a short IMU value" },
        { SDL_MYO_MOTION, { 0x00, 0x01, 0x02 }, 3, false, "a tap" },
        { SDL_MYO_BATTERY, { 0x50 }, 1, false, "a battery level" },
    };
    size_t i;

    printf("Ready rule\n");
    for (i = 0; i < COUNT(cases); ++i) {
        BH_Harness *h = Started();
        uint8_t data[SDL_MYO_IMU_SIZE];

        memset(data, 0, sizeof(data));
        memcpy(data, cases[i].data, sizeof(cases[i].data));
        if (cases[i].characteristic == SDL_MYO_IMU) {
            data[1] = 0x40; /* W 16384 */
        }
        Feed(h, cases[i].characteristic, data, cases[i].length);
        BH_CHECK(h->published == (cases[i].publishes ? 1 : 0), "%s: %s", cases[i].what,
                 cases[i].publishes ? "publishes" : "publishes nothing");
        BH_Destroy(h);
    }

    /* Input before the start-up decodes but is not ready */
    {
        BH_Harness *h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
        uint8_t properties[SDL_BLE_MAX_CHARS];
        uint8_t imu[SDL_MYO_IMU_SIZE];
        uint8_t battery = 0x30;

        Properties(properties);
        BH_Connected(h, true, false);
        BH_Discovered(h, true, properties);
        Imu(imu, 11585, 11585, 0, 0, NULL, NULL);
        Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
        BH_CHECK(BH_Axis(h, SDL_MYO_AXIS_ROLL) == 16383 && !((const SDL_BLEBase *)h->state)->ready,
                 "an IMU value during the subscriptions decodes, not ready");
        BH_SubscribeAll(h);
        BH_ReadAll(h, &battery, 1);
        BH_WriteAll(h);
        BH_CHECK(!h->published, "not published after the start-up either");
        Feed(h, SDL_MYO_IMU, imu, sizeof(imu));
        BH_CHECK(h->published, "published on the next IMU value");
        BH_Destroy(h);
    }
}

/* Motion events are subscribed and not mapped */
static void TestMotion(void)
{
    BH_Harness *h = Published();
    const uint8_t tap[3] = { 0x00, 0x01, 0x02 };
    const uint8_t other[6] = { 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    printf("Motion events\n");
    Classifier(h, 0x03, 0x0002, 3);
    Unchanged(h, SDL_MYO_MOTION, tap, sizeof(tap), "a tap");
    Unchanged(h, SDL_MYO_MOTION, other, sizeof(other), "another motion event");
    Unchanged(h, SDL_MYO_COMMAND, tap, sizeof(tap), "a value on the command characteristic");
    Unchanged(h, 5, tap, sizeof(tap), "an index past the family");
    BH_Destroy(h);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    TestIdentity();
    Test1();
    Test2();
    Test3();
    Test4();
    Test5();
    Test6();
    Test7();
    Test8();
    Test9();
    TestFamily();
    Test10();
    TestEveryType();
    TestRotations();
    TestDlMyo();
    TestBattery();
    TestReady();
    TestMotion();
    return BH_Report("testblemyo");
}
