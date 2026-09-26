/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/* From myo-bluetooth's myohw.h. Every UUID is d506xxxx-a904-deb9-4748-
 * 2c7f4a124842 (myohw.h:52-85). The streams are read little-endian, as
 * pyomyo, dl-myo and myo-raw read them, although myohw.h:49 calls every
 * value big-endian.
 * - IMU data, 20 bytes at 50 Hz: the orientation quaternion W, X, Y, Z at
 *   16384 per unit, the accelerometer at 2048 per g and the gyroscope at 16
 *   per degree per second (myohw.h:289-309).
 * - Classifier event, 3 bytes or more: a type, then for a pose event a
 *   16-bit pose (myohw.h:331-379). A pose holds its button and releases the
 *   others. Rest, an unknown pose, arm unsynced and locked release all five.
 * - Start-up, with response as dl-myo writes (myo/core.py:168-176): set mode
 *   with EMG off, IMU data and events and the classifier on, never sleep,
 *   and unlock until told to lock (myohw.h:171-267). On close: normal sleep
 *   and lock.
 * Poses as buttons and orientation as axes are the fork's mapping, not part
 * of the protocol. */

#include "SDL_ble_myo_proto.h"

#include <math.h>
#include <string.h>

#define MYO_PI 3.14159265358979323846

static const uint8_t myo_set_mode[5] = { 0x01, 0x03, 0x00, 0x03, 0x01 };
static const uint8_t myo_never_sleep[3] = { 0x09, 0x01, 0x01 };
static const uint8_t myo_unlock_hold[3] = { 0x0A, 0x01, 0x02 };
static const uint8_t myo_normal_sleep[3] = { 0x09, 0x01, 0x00 };
static const uint8_t myo_lock[3] = { 0x0A, 0x01, 0x00 };

static void Myo_Reset(void *state, const SDL_BLESink *sink, const SDL_BLEModuleContext *context)
{
    SDL_MyoState *myo = (SDL_MyoState *)state;
    SDL_BLEIdentity identity;

    (void)context;
    memset(myo, 0, sizeof(*myo));
    SDL_BLE_SetIdentity(&identity, "Thalmic Myo Armband", SDL_BLE_TYPE_UNKNOWN, 0x1F, 0x07, false);
    identity.accel_rate = 50.0f; /* MYOHW_DEFAULT_IMU_SAMPLE_RATE */
    identity.gyro_rate = 50.0f;
    SDL_BLE_ResetBase(&myo->base, sink, &identity);
}

static void Myo_Start(void *state, uint64_t now)
{
    SDL_MyoState *myo = (SDL_MyoState *)state;

    (void)now;
    SDL_BLE_QueueWrite(&myo->base, SDL_MYO_COMMAND, myo_set_mode, sizeof(myo_set_mode), true);
    SDL_BLE_QueueWrite(&myo->base, SDL_MYO_COMMAND, myo_never_sleep, sizeof(myo_never_sleep), true);
    SDL_BLE_QueueWrite(&myo->base, SDL_MYO_COMMAND, myo_unlock_hold, sizeof(myo_unlock_hold), true);
    myo->started = true;
}

static int16_t Myo_Int16(const uint8_t *data)
{
    return (int16_t)(uint16_t)(data[0] | (data[1] << 8));
}

/* Degrees to the axis range, truncated toward zero */
static int16_t Myo_Axis(double radians)
{
    return (int16_t)SDL_BLE_Clamp((int32_t)(radians * 180.0 / MYO_PI * 32767.0 / 180.0), -32768, 32767);
}

static void Myo_IMU(SDL_MyoState *myo, const uint8_t *data, uint64_t time_ns)
{
    SDL_BLEControls controls = myo->base.controls;
    const double w = Myo_Int16(&data[0]) / 16384.0;
    const double x = Myo_Int16(&data[2]) / 16384.0;
    const double y = Myo_Int16(&data[4]) / 16384.0;
    const double z = Myo_Int16(&data[6]) / 16384.0;
    const float accel = SDL_BLE_STANDARD_GRAVITY / 2048.0f;
    const float gyro = (float)(MYO_PI / 180.0 / 16.0);
    double sine;

    /* Tait-Bryan angles */
    controls.axes[SDL_MYO_AXIS_ROLL] = Myo_Axis(atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y)));
    sine = 2.0 * (w * y - z * x);
    if (sine > 1.0) {
        sine = 1.0;
    } else if (sine < -1.0) {
        sine = -1.0;
    }
    controls.axes[SDL_MYO_AXIS_PITCH] = Myo_Axis(asin(sine));
    controls.axes[SDL_MYO_AXIS_YAW] = Myo_Axis(atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z)));
    SDL_BLE_Commit(&myo->base, &controls, time_ns);

    /* No device clock: the receive time stamps the sample */
    SDL_BLE_Sensor(&myo->base, SDL_BLE_SENSOR_ACCEL, time_ns, time_ns,
                   Myo_Int16(&data[8]) * accel, Myo_Int16(&data[10]) * accel, Myo_Int16(&data[12]) * accel);
    SDL_BLE_Sensor(&myo->base, SDL_BLE_SENSOR_GYRO, time_ns, time_ns,
                   Myo_Int16(&data[14]) * gyro, Myo_Int16(&data[16]) * gyro, Myo_Int16(&data[18]) * gyro);
}

/* True when myohw.h defines the event type (myohw.h:332-339) and, for a
   pose event, the pose (myohw.h:96-104) */
static bool Myo_Classifier(SDL_MyoState *myo, const uint8_t *data, uint64_t time_ns)
{
    SDL_BLEControls controls = myo->base.controls;
    const uint16_t pose = (uint16_t)(data[1] | (data[2] << 8));

    switch (data[0]) {
    case 0x01: /* arm synced */
    case 0x04: /* unlocked */
    case 0x06: /* sync failed */
        return true;
    case 0x02: /* arm unsynced */
    case 0x05: /* locked */
        controls.buttons = 0;
        break;
    case 0x03: /* pose */
        if (pose == 0x0000 || pose == 0xFFFF) {
            controls.buttons = 0; /* rest, unknown */
        } else if (pose >= 0x0001 && pose <= 0x0005) {
            controls.buttons = 1u << (pose - 1);
        } else {
            return false;
        }
        break;
    default:
        return false;
    }
    SDL_BLE_Commit(&myo->base, &controls, time_ns);
    return true;
}

static void Myo_Value(void *state, int characteristic, const uint8_t *data, size_t length, uint64_t time_ns)
{
    SDL_MyoState *myo = (SDL_MyoState *)state;

    switch (characteristic) {
    case SDL_MYO_IMU:
        if (length < SDL_MYO_IMU_SIZE) {
            return;
        }
        Myo_IMU(myo, data, time_ns);
        break;
    case SDL_MYO_CLASSIFIER:
        if (length < SDL_MYO_CLASSIFIER_SIZE || !Myo_Classifier(myo, data, time_ns)) {
            return;
        }
        break;
    case SDL_MYO_BATTERY:
        if (length >= 1) {
            SDL_BLEControls controls = myo->base.controls;

            controls.battery = (int8_t)SDL_BLE_Clamp(data[0], 0, 100);
            SDL_BLE_Commit(&myo->base, &controls, time_ns);
        }
        return;
    default:
        /* Motion events are subscribed, as the part's start-up asks, and not mapped */
        return;
    }
    if (myo->started) {
        myo->base.ready = true;
    }
}

static void Myo_Close(void *state, uint64_t now)
{
    SDL_MyoState *myo = (SDL_MyoState *)state;

    (void)now;
    SDL_BLE_QueueWrite(&myo->base, SDL_MYO_COMMAND, myo_normal_sleep, sizeof(myo_normal_sleep), true);
    SDL_BLE_QueueWrite(&myo->base, SDL_MYO_COMMAND, myo_lock, sizeof(myo_lock), true);
    myo->started = false;
}

const SDL_BLEModule SDL_BLEMyoModule = {
    sizeof(SDL_MyoState),
    Myo_Reset,
    Myo_Start,
    Myo_Value,
    SDL_BLE_NoWriteDone,
    SDL_BLE_NoTick,
    SDL_BLE_NoDeadline,
    Myo_Close
};

#define MYO_UUID(hi, lo) \
    SDL_BLE_UUID_INIT(0xd5, 0x06, hi, lo, 0xa9, 0x04, 0xde, 0xb9, 0x47, 0x48, 0x2c, 0x7f, 0x4a, 0x12, 0x48, 0x42)

/* Found only by its control service, which myo-raw finds at the end of the
   scan response (myo_raw.py:211-213). The name is writable (myohw.h:92), so
   it is never matched. dl-myo connects through Bleak on Windows without
   pairing (myo/core.py:380-395). */
const SDL_BLEFamily SDL_BLEMyoFamily = {
    "Myo",
    1,
    {
        { SDL_BLE_KEY_SERVICE, false, NULL, MYO_UUID(0x00, 0x01), 0, 0, { 0 } },
    },
    4,
    {
        MYO_UUID(0x00, 0x01), /* control */
        MYO_UUID(0x00, 0x02), /* IMU */
        MYO_UUID(0x00, 0x03), /* classifier */
        SDL_BLE_UUID16_INIT(0x18, 0x0f),
    },
    false,
    SDL_BLE_UUID16_INIT(0x00, 0x00),
    5,
    {
        { 1, MYO_UUID(0x04, 0x02), SDL_BLE_CHAR_SUBSCRIBE },
        { 2, MYO_UUID(0x01, 0x03), SDL_BLE_CHAR_SUBSCRIBE },
        { 1, MYO_UUID(0x05, 0x02), SDL_BLE_CHAR_SUBSCRIBE },
        { 0, MYO_UUID(0x04, 0x01), 0 },
        { 3, SDL_BLE_UUID16_INIT(0x2a, 0x19), SDL_BLE_CHAR_SUBSCRIBE | SDL_BLE_CHAR_READ | SDL_BLE_CHAR_OPTIONAL },
    },
    SDL_BLE_PAIR_NOT_NEEDED,
    &SDL_BLEMyoModule
};
