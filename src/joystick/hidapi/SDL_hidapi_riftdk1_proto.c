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

#include "SDL_hidapi_riftdk1_proto.h"

#include <math.h>
#include <string.h>

#define RIFTDK1_PI 3.14159265358979323846

bool SDL_RiftDK1_IsManufacturer(const char *manufacturer)
{
    return manufacturer && strcmp(manufacturer, SDL_RIFTDK1_MANUFACTURER) == 0;
}

static int32_t RiftDK1_Signed21(uint32_t value)
{
    return (value & 0x100000) ? (int32_t)value - 0x200000 : (int32_t)value;
}

void SDL_RiftDK1_Unpack21(const uint8_t block[8], int32_t out[3])
{
    uint64_t bits = 0;
    int i;

    for (i = 0; i < 8; ++i) {
        bits = (bits << 8) | block[i];
    }
    /* x in bits 63-43, y in 42-22, z in 21-1, bit 0 unused */
    out[0] = RiftDK1_Signed21((uint32_t)((bits >> 43) & 0x1FFFFF));
    out[1] = RiftDK1_Signed21((uint32_t)((bits >> 22) & 0x1FFFFF));
    out[2] = RiftDK1_Signed21((uint32_t)((bits >> 1) & 0x1FFFFF));
}

bool SDL_RiftDK1_DecodeReport(const uint8_t *report, size_t length, SDL_RiftDK1Report *out)
{
    int i;

    if (!report || !out || length < SDL_RIFTDK1_REPORT_LENGTH || report[0] != SDL_RIFTDK1_REPORT_ID) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->sample_count = report[1];
    out->timestamp = (uint16_t)(report[2] | (report[3] << 8));
    out->last_command = (uint16_t)(report[4] | (report[5] << 8));
    out->temperature = (int16_t)(uint16_t)(report[6] | (report[7] << 8));
    for (i = 0; i < SDL_RIFTDK1_MAX_SAMPLES; ++i) {
        SDL_RiftDK1_Unpack21(&report[8 + 16 * i], out->accel[i]);
        SDL_RiftDK1_Unpack21(&report[16 + 16 * i], out->gyro[i]);
    }
    for (i = 0; i < 3; ++i) {
        out->mag[i] = (int16_t)(uint16_t)(report[56 + 2 * i] | (report[57 + 2 * i] << 8));
    }
    return true;
}

static void RiftDK1_Emit(SDL_RiftDK1Timing *timing, SDL_RiftDK1Event *event, uint64_t delta_us,
                         const float accel[3], const float gyro[3], bool repeat)
{
    timing->time_us += delta_us;
    event->time_us = timing->time_us;
    memcpy(event->accel, accel, sizeof(event->accel));
    memcpy(event->gyro, gyro, sizeof(event->gyro));
    event->repeat = repeat;
}

int SDL_RiftDK1_TimeReport(SDL_RiftDK1Timing *timing, const SDL_RiftDK1Report *report,
                           SDL_RiftDK1Event events[SDL_RIFTDK1_MAX_SAMPLES + 1])
{
    int count = 0;
    int iterations, i, j;
    uint64_t first_delta_ms;

    if (timing->valid) {
        /* The 16-bit millisecond stamp, taken across a wrap */
        const unsigned delta = (unsigned)(uint16_t)(report->timestamp - timing->last_timestamp);

        /* A few skipped samples: repeat the last one, that far after it */
        if (delta > timing->last_count && delta <= 254) {
            RiftDK1_Emit(timing, &events[count++], (uint64_t)(delta - timing->last_count) * 1000,
                         timing->last_accel, timing->last_gyro, true);
        }
    } else {
        memset(timing->last_accel, 0, sizeof(timing->last_accel));
        memset(timing->last_gyro, 0, sizeof(timing->last_gyro));
        timing->valid = true;
    }
    timing->last_count = report->sample_count;
    timing->last_timestamp = report->timestamp;

    iterations = report->sample_count;
    first_delta_ms = 1;
    if (report->sample_count > SDL_RIFTDK1_MAX_SAMPLES) {
        iterations = SDL_RIFTDK1_MAX_SAMPLES;
        first_delta_ms = (uint64_t)report->sample_count - 2;
    }
    for (i = 0; i < iterations; ++i) {
        float accel[3], gyro[3];
        for (j = 0; j < 3; ++j) {
            accel[j] = (float)report->accel[i][j] * 0.0001f;
            gyro[j] = (float)report->gyro[i][j] * 0.0001f;
        }
        RiftDK1_Emit(timing, &events[count++], (i == 0 ? first_delta_ms : 1) * 1000, accel, gyro, false);
        memcpy(timing->last_accel, accel, sizeof(accel));
        memcpy(timing->last_gyro, gyro, sizeof(gyro));
    }
    return count;
}

void SDL_RiftDK1_Angles(const float q[4], float angles[SDL_RIFTDK1_AXES])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    float s = 2.0f * (w * x - y * z);

    if (s > 1.0f) {
        s = 1.0f;
    } else if (s < -1.0f) {
        s = -1.0f;
    }
    angles[0] = atan2f(2.0f * (w * y + x * z), 1.0f - 2.0f * (x * x + y * y)); /* yaw about Y */
    angles[1] = asinf(s);                                                      /* pitch about X */
    angles[2] = atan2f(2.0f * (w * z + x * y), 1.0f - 2.0f * (x * x + z * z)); /* roll about Z */
}

int16_t SDL_RiftDK1_AngleAxis(float radians)
{
    /* Symmetric about 0: -pi is -32767 and +pi is 32767 */
    const double value = floor((double)radians / RIFTDK1_PI * 32767.0 + 0.5);

    if (value >= 32767.0) {
        return 32767;
    }
    if (value <= -32767.0) {
        return -32767;
    }
    return (int16_t)value;
}

static void RiftDK1_SendKeepAlive(SDL_RiftDK1Session *session, uint64_t now_ms, const SDL_RiftDK1Sink *sink)
{
    uint8_t report[SDL_RIFTDK1_KEEPALIVE_LENGTH];

    /* Command 0, then the interval the tracker keeps sending without another */
    report[0] = SDL_RIFTDK1_FEATURE_KEEPALIVE;
    report[1] = 0x00;
    report[2] = 0x00;
    report[3] = (uint8_t)(SDL_RIFTDK1_KEEPALIVE_TIMEOUT_MS & 0xFF);
    report[4] = (uint8_t)(SDL_RIFTDK1_KEEPALIVE_TIMEOUT_MS >> 8);
    sink->set_feature(sink->userdata, report, sizeof(report));
    session->next_keepalive_ms = now_ms + SDL_RIFTDK1_KEEPALIVE_PERIOD_MS;
}

void SDL_RiftDK1_Start(SDL_RiftDK1Session *session, uint64_t now_ms, const SDL_RiftDK1Sink *sink)
{
    uint8_t config[SDL_RIFTDK1_CONFIG_LENGTH];
    int received;

    memset(session, 0, sizeof(*session));
    SDL_RiftDK1_FusionInit(&session->fusion);

    /* Feature 2: command ID, flags, packet interval, keep-alive interval.
       Keep every field but the sensor frame flag and the interval. */
    memset(config, 0, sizeof(config));
    config[0] = SDL_RIFTDK1_FEATURE_CONFIG;
    received = sink->get_feature(sink->userdata, config, sizeof(config));
    if (received >= SDL_RIFTDK1_CONFIG_LENGTH && config[0] == SDL_RIFTDK1_FEATURE_CONFIG) {
        config[3] = (uint8_t)(config[3] & ~SDL_RIFTDK1_FLAG_SENSOR_FRAME);
        config[4] = 1; /* 1000 / 500 Hz - 1 */
        sink->set_feature(sink->userdata, config, sizeof(config));
        memset(config, 0, sizeof(config));
        config[0] = SDL_RIFTDK1_FEATURE_CONFIG;
        (void)sink->get_feature(sink->userdata, config, sizeof(config));
    }
    RiftDK1_SendKeepAlive(session, now_ms, sink);
}

void SDL_RiftDK1_Update(SDL_RiftDK1Session *session, uint64_t now_ms, const SDL_RiftDK1Sink *sink)
{
    if (now_ms >= session->next_keepalive_ms) {
        RiftDK1_SendKeepAlive(session, now_ms, sink);
    }
}

int SDL_RiftDK1_HandleReport(SDL_RiftDK1Session *session, const SDL_RiftDK1Report *report,
                             SDL_RiftDK1Event events[SDL_RIFTDK1_MAX_SAMPLES + 1])
{
    uint64_t previous_us = session->timing.time_us;
    float angles[SDL_RIFTDK1_AXES];
    int count, i;

    count = SDL_RiftDK1_TimeReport(&session->timing, report, events);
    for (i = 0; i < count; ++i) {
        const float dt = (float)(events[i].time_us - previous_us) / 1000000.0f;
        SDL_RiftDK1_FusionUpdate(&session->fusion, dt, events[i].gyro, events[i].accel);
        previous_us = events[i].time_us;
    }
    SDL_RiftDK1_Angles(session->fusion.orient, angles);
    for (i = 0; i < SDL_RIFTDK1_AXES; ++i) {
        session->axes[i] = SDL_RiftDK1_AngleAxis(angles[i]);
    }
    return count;
}
