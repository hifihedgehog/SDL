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

/* The Oculus Rift DK1 head tracker, 2833:0001 with the manufacturer string
 * "Oculus VR, Inc.". Pure C99: no SDL runtime and no I/O, time from an
 * injected clock.
 *
 * The tracker sends sensor report 1 only while the host writes keep-alive
 * feature 8. The start-up reads feature 2, clears its sensor-coordinates
 * flag so the samples come in the HMD frame, sets the packet interval to 1,
 * reads it back and sends the first keep-alive. Report 1 holds up to three
 * accelerometer and gyroscope samples of three signed 21-bit values and a
 * magnetometer. Sample times follow the 2013 SDK. The facts are from the
 * Oculus SDK and OpenHMD. The orientation comes from the port of OpenHMD's
 * fusion in SDL_hidapi_riftdk1_fusion.c.
 */

#ifndef SDL_hidapi_riftdk1_proto_h_
#define SDL_hidapi_riftdk1_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "SDL_hidapi_riftdk1_fusion.h"

#define SDL_RIFTDK1_MANUFACTURER         "Oculus VR, Inc."
#define SDL_RIFTDK1_REPORT_ID            0x01
#define SDL_RIFTDK1_REPORT_LENGTH        62
#define SDL_RIFTDK1_FEATURE_CONFIG       0x02
#define SDL_RIFTDK1_CONFIG_LENGTH        7
#define SDL_RIFTDK1_FEATURE_KEEPALIVE    0x08
#define SDL_RIFTDK1_KEEPALIVE_LENGTH     5
#define SDL_RIFTDK1_KEEPALIVE_PERIOD_MS  3000
#define SDL_RIFTDK1_KEEPALIVE_TIMEOUT_MS 10000 /* What each keep-alive asks for */
#define SDL_RIFTDK1_FLAG_SENSOR_FRAME    0x40
#define SDL_RIFTDK1_MAX_SAMPLES          3
#define SDL_RIFTDK1_AXES                 3 /* Yaw about Y, pitch about X, roll about Z */

typedef struct SDL_RiftDK1Report
{
    uint8_t sample_count; /* As sent. Up to 3 samples are in the report. */
    uint16_t timestamp;   /* Milliseconds, wrapping */
    uint16_t last_command;
    int16_t temperature;  /* 1/100 degree Celsius */
    int32_t accel[SDL_RIFTDK1_MAX_SAMPLES][3]; /* 1e-4 m/s^2 */
    int32_t gyro[SDL_RIFTDK1_MAX_SAMPLES][3];  /* 1e-4 rad/s */
    int16_t mag[3];                            /* 1e-4 gauss */
} SDL_RiftDK1Report;

extern bool SDL_RiftDK1_IsManufacturer(const char *manufacturer);

/* Three signed 21-bit values packed most significant bit first in 8 bytes */
extern void SDL_RiftDK1_Unpack21(const uint8_t block[8], int32_t out[3]);

/* Report 1 of 62 bytes or more, the bytes past 62 ignored. The SDK takes
   any length from 62 up and its Windows code reads at the collection's
   input report length. OpenHMD takes 62 or 64. */
extern bool SDL_RiftDK1_DecodeReport(const uint8_t *report, size_t length, SDL_RiftDK1Report *out);

typedef struct SDL_RiftDK1Event
{
    uint64_t time_us; /* Since the start of the session */
    float accel[3];   /* m/s^2 */
    float gyro[3];    /* rad/s */
    bool repeat;      /* The previous sample again, for samples the tracker skipped */
} SDL_RiftDK1Event;

typedef struct SDL_RiftDK1Timing
{
    bool valid;
    uint16_t last_timestamp;
    uint8_t last_count;
    uint64_t time_us;
    float last_accel[3];
    float last_gyro[3];
} SDL_RiftDK1Timing;

/* The events a report gives, up to 4, in time order: a repeat of the
 * previous sample when the timestamp jumped by more than the previous
 * report's count and at most 254, then up to 3 samples, the first count - 2
 * ms after the event before when the count is above 3, otherwise 1 ms, and
 * the rest 1 ms apart. Returns the number of events. */
extern int SDL_RiftDK1_TimeReport(SDL_RiftDK1Timing *timing, const SDL_RiftDK1Report *report,
                                  SDL_RiftDK1Event events[SDL_RIFTDK1_MAX_SAMPLES + 1]);

/* Yaw about Y, pitch about X and roll about Z, in radians, from a quaternion
 * x, y, z, w, for the rotation yaw, then pitch, then roll */
extern void SDL_RiftDK1_Angles(const float q[4], float angles[SDL_RIFTDK1_AXES]);

/* An angle in radians as an axis: -pi is -32767 and +pi is 32767 */
extern int16_t SDL_RiftDK1_AngleAxis(float radians);

typedef struct SDL_RiftDK1Sink
{
    void *userdata;
    /* Reads a feature report into data, data[0] its ID. Returns the bytes read or -1. */
    int (*get_feature)(void *userdata, uint8_t *data, size_t length);
    /* Sends a feature report, data[0] its ID */
    bool (*set_feature)(void *userdata, const uint8_t *data, size_t length);
} SDL_RiftDK1Sink;

typedef struct SDL_RiftDK1Session
{
    uint64_t next_keepalive_ms;
    SDL_RiftDK1Timing timing;
    SDL_RiftDK1Fusion fusion;
    int16_t axes[SDL_RIFTDK1_AXES];
} SDL_RiftDK1Session;

/* Reads feature 2 and writes it back in the HMD frame with packet interval
 * 1, reads it again, then sends the first keep-alive. A failed read skips
 * the write. A reconnect starts here again. */
extern void SDL_RiftDK1_Start(SDL_RiftDK1Session *session, uint64_t now_ms, const SDL_RiftDK1Sink *sink);

/* Sends the keep-alive every 3 s */
extern void SDL_RiftDK1_Update(SDL_RiftDK1Session *session, uint64_t now_ms, const SDL_RiftDK1Sink *sink);

/* Times a decoded report, runs each event through the fusion and updates
 * the orientation axes. Returns the events for the sensors. */
extern int SDL_RiftDK1_HandleReport(SDL_RiftDK1Session *session, const SDL_RiftDK1Report *report,
                                    SDL_RiftDK1Event events[SDL_RIFTDK1_MAX_SAMPLES + 1]);

#endif /* SDL_hidapi_riftdk1_proto_h_ */
