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

/* From gearvr-controller, verified on an ET-YO324 (PROTOCOL.md, gearvr.py):
 * - Start-up: write 08 00, wait for its 2-byte echo on the data
 *   characteristic, about 1.5 s later, then write 01 00, which streams 68
 *   packets per second. 01 00 before the echo makes the controller echo
 *   01 00 and stop (PROTOCOL.md:68-78). Without an echo in 4 s, 01 00 alone
 *   streams about 30 packets per second (PROTOCOL.md:35).
 * - 04 00 every 10 s keeps it alive, the interval Samsung's service used
 *   (daydream-catcher writeup 2, :68). 00 00 stops the stream (gearvr.py:133-140).
 * - A stream can stop for good: when 01 00 arrives before the 08 00 echo,
 *   caps/vr_first.jsonl streams 103 packets, echoes 01 00 at 2.4983 s and
 *   sends nothing more, as caps/vr_ka.jsonl does at 2.5715 s
 *   (PROTOCOL.md:77). The 4 s fallback can cause that when the echo is late.
 *   So 3 s without a packet after 01 00 went out, whether the stream stopped
 *   or never started, releases every control and starts over from 08 00.
 *   The reference's probe restarts a stopped stream the same way, 08 00 and
 *   then 01 00 (probe_features.py:120-125). A streaming controller echoes
 *   08 00 about 1.5 s later: caps/vr_idle.jsonl writes 01 00 at 1.0710 s,
 *   receives three packets, writes 08 00 at 1.1342 s, which stops the
 *   stream (PROTOCOL.md:78), and receives the echo at 2.6942 s. Whether a
 *   controller that stopped on its own echoes 08 00 is untested, and
 *   without the echo 01 00 follows at 4 s. A 01 00 echo alone is no stop.
 * - After two restarts in a row that bring no packet, the module gives the
 *   connection up. The session ends with its backoff, and the device's next
 *   connection starts over, where the joystick would otherwise stay at rest
 *   with only the keep-alive going out until the link dropped.
 * - The 3 s: at 68 packets per second the captures never leave more than
 *   62 ms between packets (caps/guided.jsonl), and 01 00 alone gives 31 a
 *   second with gaps of up to 339 ms (caps/sensor_idle.jsonl), so 3 s is
 *   about nine times the longest gap. A restart of a stream that had only
 *   stalled costs about 1.5 s of input, since 08 00 stops a stream at once
 *   and is echoed about 1.5 s later (PROTOCOL.md:78). A real stop holds the
 *   last controls until the release, for the same 3 s the Oculus Go's
 *   reference waits before it releases on a silence
 *   (OculusGo-Air-Mouse-4-macOS main.swift:647).
 * - A 60-byte packet, little-endian: three samples of a uint32 microsecond
 *   time, accelerometer at 2048 per g and gyroscope at 14.285 per degree per
 *   second, then the magnetometer, the touchpad at 54-56, the temperature,
 *   the buttons at 58 and the battery percentage at 59 (PROTOCOL.md:85-150).
 *   Every other length names another packet type and is not input.
 * - Axes: +X right, +Y toward the trigger end, +Z out of the touchpad
 *   (PROTOCOL.md:114-117). SDL X is device X, SDL Y device Z and SDL Z minus
 *   device Y, so the controller lying flat reads +1 g on SDL Y. */

#include "SDL_ble_gearvr_proto.h"

#include <string.h>

#define GEARVR_PI 3.14159265358979323846

static const uint8_t gearvr_vr_mode[2] = { 0x08, 0x00 };
static const uint8_t gearvr_sensor[2] = { 0x01, 0x00 };
static const uint8_t gearvr_keepalive[2] = { 0x04, 0x00 };
static const uint8_t gearvr_off[2] = { 0x00, 0x00 };

static void GearVR_Reset(void *state, const SDL_BLESink *sink, const SDL_BLEModuleContext *context)
{
    SDL_GearVRState *gearvr = (SDL_GearVRState *)state;
    SDL_BLEIdentity identity;

    (void)context;
    memset(gearvr, 0, sizeof(*gearvr));
    SDL_BLE_SetIdentity(&identity, "Samsung Gear VR Controller", SDL_BLE_TYPE_GAMEPAD,
                        (1u << SDL_BLE_BUTTON_SOUTH) | (1u << SDL_BLE_BUTTON_BACK) | (1u << SDL_BLE_BUTTON_GUIDE) |
                            (1u << SDL_BLE_BUTTON_LEFT_SHOULDER) | (1u << SDL_BLE_BUTTON_RIGHT_SHOULDER),
                        (1u << SDL_BLE_AXIS_LEFTX) | (1u << SDL_BLE_AXIS_LEFTY) | (1u << SDL_BLE_AXIS_RIGHT_TRIGGER), true);
    identity.touchpad = true;
    identity.accel_rate = 206.0f;
    identity.gyro_rate = 206.0f;
    SDL_BLE_ResetBase(&gearvr->base, sink, &identity);
}

/* 08 00, then its echo awaited: the start-up, and the restart after a
   stream stopped */
static void GearVR_VRMode(SDL_GearVRState *gearvr, uint64_t now)
{
    SDL_BLE_QueueWrite(&gearvr->base, SDL_GEARVR_COMMAND, gearvr_vr_mode, sizeof(gearvr_vr_mode), true);
    gearvr->phase = SDL_GEARVR_WAIT_ACK;
    gearvr->deadline = now + SDL_GEARVR_ACK_WAIT_MS;
}

static void GearVR_Start(void *state, uint64_t now)
{
    GearVR_VRMode((SDL_GearVRState *)state, now);
}

/* 01 00 starts the stream, and the silence timer runs from it, so a stream
   that never starts counts as a stop */
static void GearVR_StartStream(SDL_GearVRState *gearvr, uint64_t now)
{
    SDL_BLE_QueueWrite(&gearvr->base, SDL_GEARVR_COMMAND, gearvr_sensor, sizeof(gearvr_sensor), true);
    gearvr->phase = SDL_GEARVR_STREAMING;
    gearvr->deadline = now + SDL_GEARVR_KEEPALIVE_MS;
    gearvr->silence = now + SDL_GEARVR_SILENCE_MS;
}

static int16_t GearVR_Int16(const uint8_t *data)
{
    return (int16_t)(uint16_t)(data[0] | (data[1] << 8));
}

static uint32_t GearVR_Uint32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int16_t GearVR_TouchAxis(int value)
{
    return (int16_t)SDL_BLE_Clamp((int32_t)(value - 160) * 32767 / 160, -32768, 32767);
}

static void GearVR_Sample(SDL_GearVRState *gearvr, const uint8_t *sample, uint64_t time_ns)
{
    const uint32_t time = GearVR_Uint32(sample);
    const float accel = SDL_BLE_STANDARD_GRAVITY / 2048.0f;
    const float gyro = (float)(GEARVR_PI / 180.0 / 14.285);
    float ax, ay, az, gx, gy, gz;

    /* uint32 subtraction carries the delta across the counter's wrap */
    if (!gearvr->have_time) {
        gearvr->have_time = true;
        gearvr->sensor_us = time;
    } else {
        gearvr->sensor_us += (uint32_t)(time - gearvr->last_time);
    }
    gearvr->last_time = time;

    ax = GearVR_Int16(&sample[4]) * accel;
    ay = GearVR_Int16(&sample[6]) * accel;
    az = GearVR_Int16(&sample[8]) * accel;
    gx = GearVR_Int16(&sample[10]) * gyro;
    gy = GearVR_Int16(&sample[12]) * gyro;
    gz = GearVR_Int16(&sample[14]) * gyro;
    SDL_BLE_Sensor(&gearvr->base, SDL_BLE_SENSOR_ACCEL, time_ns, gearvr->sensor_us * 1000, ax, az, -ay);
    SDL_BLE_Sensor(&gearvr->base, SDL_BLE_SENSOR_GYRO, time_ns, gearvr->sensor_us * 1000, gx, gz, -gy);
}

static void GearVR_Value(void *state, int characteristic, const uint8_t *data, size_t length, uint64_t time_ns)
{
    SDL_GearVRState *gearvr = (SDL_GearVRState *)state;
    SDL_BLEControls controls = gearvr->base.controls;
    int touch_x, touch_y;

    if (characteristic != SDL_GEARVR_DATA) {
        return;
    }
    if (length == 2) {
        /* An echo of a command. Only the 08 00 echo moves the start-up on. */
        if (gearvr->phase == SDL_GEARVR_WAIT_ACK && data[0] == gearvr_vr_mode[0] && data[1] == gearvr_vr_mode[1]) {
            GearVR_StartStream(gearvr, time_ns / 1000000);
        }
        return;
    }
    if (length != SDL_GEARVR_PACKET_SIZE) {
        return;
    }

    touch_x = ((data[54] & 0x0F) << 6) | (data[55] >> 2);
    touch_y = ((data[55] & 0x03) << 8) | data[56];
    /* The high nibble of byte 54: 1 touching, 2 idle, 0 for the packet after a lift */
    controls.finger = ((data[54] >> 4) == 1);
    if (controls.finger) {
        /* 320 is the full scale the settings record reports (PROTOCOL.md:56-57).
           The 10-bit fields reach 1023, and a value past 320 reads as the edge. */
        controls.finger_x = (float)SDL_BLE_Clamp(touch_x, 0, 320) / 320.0f;
        controls.finger_y = (float)SDL_BLE_Clamp(touch_y, 0, 320) / 320.0f;
        controls.axes[SDL_BLE_AXIS_LEFTX] = GearVR_TouchAxis(touch_x);
        controls.axes[SDL_BLE_AXIS_LEFTY] = GearVR_TouchAxis(touch_y);
    } else {
        controls.finger_x = 0.0f;
        controls.finger_y = 0.0f;
        controls.axes[SDL_BLE_AXIS_LEFTX] = 0;
        controls.axes[SDL_BLE_AXIS_LEFTY] = 0;
    }
    controls.axes[SDL_BLE_AXIS_RIGHT_TRIGGER] = (data[58] & 0x01) ? 32767 : -32768;
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_GUIDE, (data[58] & 0x02) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_BACK, (data[58] & 0x04) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_SOUTH, (data[58] & 0x08) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_RIGHT_SHOULDER, (data[58] & 0x10) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_LEFT_SHOULDER, (data[58] & 0x20) != 0);
    controls.battery = (int8_t)SDL_BLE_Clamp(data[59], 0, 100);
    SDL_BLE_Commit(&gearvr->base, &controls, time_ns);
    if (gearvr->phase == SDL_GEARVR_STREAMING) {
        gearvr->base.ready = true;
        gearvr->restarts = 0;
        gearvr->silence = time_ns / 1000000 + SDL_GEARVR_SILENCE_MS;
    }

    GearVR_Sample(gearvr, &data[0], time_ns);
    GearVR_Sample(gearvr, &data[16], time_ns);
    GearVR_Sample(gearvr, &data[32], time_ns);
}

static void GearVR_Tick(void *state, uint64_t now)
{
    SDL_GearVRState *gearvr = (SDL_GearVRState *)state;

    if (gearvr->phase == SDL_GEARVR_WAIT_ACK && now >= gearvr->deadline) {
        GearVR_StartStream(gearvr, now);
    } else if (gearvr->phase == SDL_GEARVR_STREAMING && now >= gearvr->silence) {
        if (gearvr->restarts >= SDL_GEARVR_RESTARTS) {
            /* The restarts brought nothing: the connection is given up,
               and the module keeps no timer */
            gearvr->phase = SDL_GEARVR_IDLE;
            gearvr->base.failed = true;
            return;
        }
        /* The stream stopped: nothing stays held, and the start-up runs again */
        ++gearvr->restarts;
        SDL_BLE_Release(&gearvr->base, now * 1000000);
        GearVR_VRMode(gearvr, now);
    } else if (gearvr->phase == SDL_GEARVR_STREAMING && now >= gearvr->deadline) {
        SDL_BLE_QueueWrite(&gearvr->base, SDL_GEARVR_COMMAND, gearvr_keepalive, sizeof(gearvr_keepalive), true);
        gearvr->deadline += SDL_GEARVR_KEEPALIVE_MS;
        if (gearvr->deadline <= now) {
            gearvr->deadline = now + SDL_GEARVR_KEEPALIVE_MS;
        }
    }
}

static bool GearVR_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_GearVRState *gearvr = (SDL_GearVRState *)state;

    if (gearvr->phase == SDL_GEARVR_IDLE) {
        return false;
    }
    *deadline = gearvr->deadline;
    if (gearvr->phase == SDL_GEARVR_STREAMING && gearvr->silence < *deadline) {
        *deadline = gearvr->silence;
    }
    return true;
}

static void GearVR_Close(void *state, uint64_t now)
{
    SDL_GearVRState *gearvr = (SDL_GearVRState *)state;

    (void)now;
    SDL_BLE_QueueWrite(&gearvr->base, SDL_GEARVR_COMMAND, gearvr_off, sizeof(gearvr_off), true);
    gearvr->phase = SDL_GEARVR_IDLE;
}

const SDL_BLEModule SDL_BLEGearVRModule = {
    sizeof(SDL_GearVRState),
    GearVR_Reset,
    GearVR_Start,
    GearVR_Value,
    SDL_BLE_NoWriteDone,
    GearVR_Tick,
    GearVR_GetDeadline,
    GearVR_Close
};

/* Found by its service, which the Windows Gear VR driver finds in
   advertisements (gear_vr_controller scanner.rs:48-70), or by its name,
   "Gear VR Controller" and four hex digits (PROTOCOL.md:14-15). Samsung's
   company ID 0x0075 names the maker, not the controller, so it only
   corroborates (PROTOCOL.md:16-17). It pairs on error: macOS reads it
   without a bond (PROTOCOL.md:180-181) and daydream-catcher reports that a
   bond is needed (Reference :8). */
const SDL_BLEFamily SDL_BLEGearVRFamily = {
    "Gear VR",
    3,
    {
        { SDL_BLE_KEY_SERVICE, false, NULL,
          /* 4f63756c-7573-2054-6872-65656d6f7465 */
          SDL_BLE_UUID_INIT(0x4f, 0x63, 0x75, 0x6c, 0x75, 0x73, 0x20, 0x54, 0x68, 0x72, 0x65, 0x65, 0x6d, 0x6f, 0x74, 0x65),
          0, 0, { 0 } },
        { SDL_BLE_KEY_NAME_PREFIX, false, "Gear VR Controller", SDL_BLE_UUID16_INIT(0x00, 0x00), 0, 0, { 0 } },
        { SDL_BLE_KEY_MANUFACTURER, true, NULL, SDL_BLE_UUID16_INIT(0x00, 0x00), 0x0075, 0, { 0 } },
    },
    1,
    {
        SDL_BLE_UUID_INIT(0x4f, 0x63, 0x75, 0x6c, 0x75, 0x73, 0x20, 0x54, 0x68, 0x72, 0x65, 0x65, 0x6d, 0x6f, 0x74, 0x65),
    },
    false,
    SDL_BLE_UUID16_INIT(0x00, 0x00),
    2,
    {
        /* c8c51726-81bc-483b-a052-f7a14ea3d281 and ...d282 */
        { 0, SDL_BLE_UUID_INIT(0xc8, 0xc5, 0x17, 0x26, 0x81, 0xbc, 0x48, 0x3b, 0xa0, 0x52, 0xf7, 0xa1, 0x4e, 0xa3, 0xd2, 0x81),
          SDL_BLE_CHAR_SUBSCRIBE },
        { 0, SDL_BLE_UUID_INIT(0xc8, 0xc5, 0x17, 0x26, 0x81, 0xbc, 0x48, 0x3b, 0xa0, 0x52, 0xf7, 0xa1, 0x4e, 0xa3, 0xd2, 0x82),
          0 },
    },
    SDL_BLE_PAIR_ON_ERROR,
    &SDL_BLEGearVRModule
};
