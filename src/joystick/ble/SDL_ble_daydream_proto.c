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

/* The record is one 160-bit stream read from bit 7 of byte 0 (daydream2hid
 * src/daydream.c:101-136, daydream-catcher raw.rs:5-71): a 9-bit
 * millisecond time, a 5-bit sequence, three 13-bit orientation fields,
 * three accelerometer fields, three gyroscope fields, touch X and Y as
 * bytes, then volume up, volume down, app, home and touchpad click, then a
 * status byte. The accelerometer reads 8 g per 4095 and the gyroscope 2048
 * degrees per second per 4095 (daydream-catcher packet.rs:240, :243). Touch
 * (0, 0) means no finger (raw.rs:48-58). The sensor fields 1, 2 and 3 pass
 * through as SDL X, Y and Z: field 2 reads +1 g lying flat, SDL's positive Y
 * at rest. */

#include "SDL_ble_daydream_proto.h"

#include <string.h>

#define DAYDREAM_PI 3.14159265358979323846

uint32_t SDL_Daydream_Bits(const uint8_t *data, int start, int count)
{
    uint32_t value = 0;
    int i;

    for (i = 0; i < count; ++i) {
        const int bit = start + i;

        value = (value << 1) | ((data[bit / 8] >> (7 - (bit % 8))) & 1u);
    }
    return value;
}

int32_t SDL_Daydream_Signed13(const uint8_t *data, int start)
{
    const int32_t value = (int32_t)SDL_Daydream_Bits(data, start, 13);

    return (value & 0x1000) ? (value - 0x2000) : value;
}

static void Daydream_Reset(void *state, const SDL_BLESink *sink, const SDL_BLEModuleContext *context)
{
    SDL_DaydreamState *daydream = (SDL_DaydreamState *)state;
    SDL_BLEIdentity identity;

    (void)context;
    memset(daydream, 0, sizeof(*daydream));
    SDL_BLE_SetIdentity(&identity, "Google Daydream Controller", SDL_BLE_TYPE_GAMEPAD,
                        (1u << SDL_BLE_BUTTON_SOUTH) | (1u << SDL_BLE_BUTTON_GUIDE) | (1u << SDL_BLE_BUTTON_START) |
                            (1u << SDL_BLE_BUTTON_LEFT_SHOULDER) | (1u << SDL_BLE_BUTTON_RIGHT_SHOULDER),
                        (1u << SDL_BLE_AXIS_LEFTX) | (1u << SDL_BLE_AXIS_LEFTY), true);
    identity.vendor = SDL_DAYDREAM_VENDOR;
    identity.product = SDL_DAYDREAM_PRODUCT;
    identity.touchpad = true;
    /* The reciprocal of the 25 ms average interval the hardfault.life
       article measured */
    identity.accel_rate = 40.0f;
    identity.gyro_rate = 40.0f;
    SDL_BLE_ResetBase(&daydream->base, sink, &identity);
}

/* Nothing to write. The joystick appears with the next record. */
static void Daydream_Start(void *state, uint64_t now)
{
    SDL_DaydreamState *daydream = (SDL_DaydreamState *)state;

    (void)now;
    daydream->started = true;
}

static int16_t Daydream_TouchAxis(int value)
{
    return (int16_t)SDL_BLE_Clamp((int32_t)(value - 128) * 32767 / 127, -32768, 32767);
}

static void Daydream_Value(void *state, int characteristic, const uint8_t *data, size_t length, uint64_t time_ns)
{
    SDL_DaydreamState *daydream = (SDL_DaydreamState *)state;
    SDL_BLEControls controls = daydream->base.controls;
    uint16_t time;
    int touch_x, touch_y, i;
    float accel[3], gyro[3];

    if (characteristic == SDL_DAYDREAM_BATTERY) {
        if (length >= 1) {
            controls.battery = (int8_t)SDL_BLE_Clamp(data[0], 0, 100);
            SDL_BLE_Commit(&daydream->base, &controls, time_ns);
        }
        return;
    }
    /* daydream2hid rejects every other length too (src/bluetooth.c:148-151) */
    if (characteristic != SDL_DAYDREAM_POSE || length != SDL_DAYDREAM_REPORT_SIZE) {
        return;
    }

    /* The article's wrap rule: a time that is not larger has wrapped */
    time = (uint16_t)SDL_Daydream_Bits(data, 0, 9);
    if (!daydream->have_time) {
        daydream->have_time = true;
        daydream->sensor_ms = time;
    } else if (time > daydream->last_time) {
        daydream->sensor_ms += (uint64_t)(time - daydream->last_time);
    } else {
        daydream->sensor_ms += (uint64_t)(512 - daydream->last_time + time);
    }
    daydream->last_time = time;

    touch_x = (int)SDL_Daydream_Bits(data, 131, 8);
    touch_y = (int)SDL_Daydream_Bits(data, 139, 8);
    controls.finger = (touch_x != 0 || touch_y != 0);
    if (controls.finger) {
        controls.finger_x = (float)touch_x / 255.0f;
        controls.finger_y = (float)touch_y / 255.0f;
        controls.axes[SDL_BLE_AXIS_LEFTX] = Daydream_TouchAxis(touch_x);
        controls.axes[SDL_BLE_AXIS_LEFTY] = Daydream_TouchAxis(touch_y);
    } else {
        controls.finger_x = 0.0f;
        controls.finger_y = 0.0f;
        controls.axes[SDL_BLE_AXIS_LEFTX] = 0;
        controls.axes[SDL_BLE_AXIS_LEFTY] = 0;
    }
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_RIGHT_SHOULDER, (data[18] & 0x10) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_LEFT_SHOULDER, (data[18] & 0x08) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_START, (data[18] & 0x04) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_GUIDE, (data[18] & 0x02) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_SOUTH, (data[18] & 0x01) != 0);
    SDL_BLE_Commit(&daydream->base, &controls, time_ns);
    if (daydream->started) {
        daydream->base.ready = true;
    }

    for (i = 0; i < 3; ++i) {
        accel[i] = (float)((double)SDL_Daydream_Signed13(data, 53 + 13 * i) / 4095.0 * 8.0 * SDL_BLE_STANDARD_GRAVITY);
        gyro[i] = (float)((double)SDL_Daydream_Signed13(data, 92 + 13 * i) / 4095.0 * 2048.0 * DAYDREAM_PI / 180.0);
    }
    SDL_BLE_Sensor(&daydream->base, SDL_BLE_SENSOR_ACCEL, time_ns, daydream->sensor_ms * 1000000, accel[0], accel[1], accel[2]);
    SDL_BLE_Sensor(&daydream->base, SDL_BLE_SENSOR_GYRO, time_ns, daydream->sensor_ms * 1000000, gyro[0], gyro[1], gyro[2]);
}

const SDL_BLEModule SDL_BLEDaydreamModule = {
    sizeof(SDL_DaydreamState),
    Daydream_Reset,
    Daydream_Start,
    Daydream_Value,
    SDL_BLE_NoWriteDone,
    SDL_BLE_NoTick,
    SDL_BLE_NoDeadline,
    SDL_BLE_NoClose
};

/* Found by its exact complete name, as daydream2hid matches it
   (src/bluetooth.c:77-88, :94-102) and daydream-controller.js filters by the
   name alone (DaydreamController.js:11-14). Service FE55 only corroborates:
   the Bluetooth SIG lists it as a member UUID of Google LLC (assigned
   numbers, member_uuids.yaml), not of this controller, and since the family
   pairs before discovery, a match on FE55 alone would ask any FE55
   advertiser to pair when the pairing hint is on. It pairs first: from
   firmware 1.2.11 the pose characteristic sends nothing to a host without a
   bond (daydream-catcher Reference :8-10), and daydream2hid raises the link
   security before discovery (src/bluetooth.c:228-242). The control
   characteristic is never written. */
const SDL_BLEFamily SDL_BLEDaydreamFamily = {
    "Daydream",
    2,
    {
        { SDL_BLE_KEY_NAME_EQUALS, false, "Daydream controller", SDL_BLE_UUID16_INIT(0x00, 0x00), 0, 0, { 0 } },
        { SDL_BLE_KEY_SERVICE, true, NULL, SDL_BLE_UUID16_INIT(0xfe, 0x55), 0, 0, { 0 } },
    },
    2,
    {
        SDL_BLE_UUID16_INIT(0xfe, 0x55),
        SDL_BLE_UUID16_INIT(0x18, 0x0f),
    },
    false,
    SDL_BLE_UUID16_INIT(0x00, 0x00),
    2,
    {
        /* 00000001-1000-1000-8000-00805f9b34fb */
        { 0, SDL_BLE_UUID_INIT(0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb),
          SDL_BLE_CHAR_SUBSCRIBE },
        { 1, SDL_BLE_UUID16_INIT(0x2a, 0x19), SDL_BLE_CHAR_SUBSCRIBE | SDL_BLE_CHAR_READ | SDL_BLE_CHAR_OPTIONAL },
    },
    SDL_BLE_PAIR_FIRST,
    &SDL_BLEDaydreamModule
};
