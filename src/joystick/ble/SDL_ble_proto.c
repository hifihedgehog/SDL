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

#include "SDL_ble_proto.h"

#include <string.h>

SDL_BLEUUID SDL_BLE_UUID16(uint16_t uuid)
{
    /* 0000xxxx-0000-1000-8000-00805f9b34fb */
    static const uint8_t base[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb
    };
    SDL_BLEUUID result;

    memcpy(result.bytes, base, sizeof(result.bytes));
    result.bytes[2] = (uint8_t)(uuid >> 8);
    result.bytes[3] = (uint8_t)uuid;
    return result;
}

bool SDL_BLE_UUIDEqual(const SDL_BLEUUID *a, const SDL_BLEUUID *b)
{
    return memcmp(a->bytes, b->bytes, sizeof(a->bytes)) == 0;
}

void SDL_BLE_SetIdentity(SDL_BLEIdentity *identity, const char *name, uint8_t type, uint32_t buttons, uint8_t axes, bool gamepad)
{
    size_t length = strlen(name);

    memset(identity, 0, sizeof(*identity));
    if (length >= sizeof(identity->name)) {
        length = sizeof(identity->name) - 1;
    }
    memcpy(identity->name, name, length);
    identity->type = type;
    identity->buttons = buttons;
    identity->axes = axes;
    identity->gamepad = gamepad;
}

void SDL_BLE_RestControls(const SDL_BLEIdentity *identity, SDL_BLEControls *controls)
{
    memset(controls, 0, sizeof(*controls));
    if (identity->gamepad) {
        if (identity->axes & (1 << SDL_BLE_AXIS_LEFT_TRIGGER)) {
            controls->axes[SDL_BLE_AXIS_LEFT_TRIGGER] = -32768;
        }
        if (identity->axes & (1 << SDL_BLE_AXIS_RIGHT_TRIGGER)) {
            controls->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] = -32768;
        }
    }
    controls->battery = -1;
}

void SDL_BLE_SetButton(SDL_BLEControls *controls, int button, bool pressed)
{
    if (button < 0 || button >= SDL_BLE_MAX_BUTTONS) {
        return;
    }
    if (pressed) {
        controls->buttons |= (uint32_t)1 << button;
    } else {
        controls->buttons &= ~((uint32_t)1 << button);
    }
}

bool SDL_BLE_GetButton(const SDL_BLEControls *controls, int button)
{
    if (button < 0 || button >= SDL_BLE_MAX_BUTTONS) {
        return false;
    }
    return (controls->buttons & ((uint32_t)1 << button)) != 0;
}

bool SDL_BLE_ControlsEqual(const SDL_BLEControls *a, const SDL_BLEControls *b)
{
    int i;

    for (i = 0; i < SDL_BLE_MAX_AXES; ++i) {
        if (a->axes[i] != b->axes[i]) {
            return false;
        }
    }
    return a->buttons == b->buttons && a->finger == b->finger &&
           a->finger_x == b->finger_x && a->finger_y == b->finger_y &&
           a->battery == b->battery;
}

int32_t SDL_BLE_Clamp(int32_t value, int32_t lo, int32_t hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

void SDL_BLE_ResetBase(SDL_BLEBase *base, const SDL_BLESink *sink, const SDL_BLEIdentity *identity)
{
    memset(base, 0, sizeof(*base));
    if (sink) {
        base->sink = *sink;
    }
    base->identity = *identity;
    SDL_BLE_RestControls(identity, &base->controls);
}

/* The time for a change or sample the sink gets: the later of its own and
   the last one handed on. Values carry their nanosecond receive time, while
   the session stamps the release that ends a connection from its
   millisecond clock, which can fall up to 1 ms before the last value. The
   sink takes changes and samples in order, so their times keep that order. */
static uint64_t BLE_EmitTime(SDL_BLEBase *base, uint64_t time_ns)
{
    if (time_ns < base->emitted_ns) {
        return base->emitted_ns;
    }
    base->emitted_ns = time_ns;
    return time_ns;
}

void SDL_BLE_Commit(SDL_BLEBase *base, const SDL_BLEControls *controls, uint64_t time_ns)
{
    if (SDL_BLE_ControlsEqual(&base->controls, controls)) {
        return;
    }
    base->controls = *controls;
    ++base->changes;
    if (base->sink.changed) {
        base->sink.changed(base->sink.userdata, &base->controls, BLE_EmitTime(base, time_ns));
    }
}

void SDL_BLE_Release(SDL_BLEBase *base, uint64_t time_ns)
{
    SDL_BLEControls rest;

    SDL_BLE_RestControls(&base->identity, &rest);
    rest.battery = base->controls.battery;
    SDL_BLE_Commit(base, &rest, time_ns);
}

void SDL_BLE_Sensor(SDL_BLEBase *base, int sensor, uint64_t time_ns, uint64_t sensor_ns, float x, float y, float z)
{
    float data[3];

    if (!base->sink.sensor) {
        return;
    }
    data[0] = x;
    data[1] = y;
    data[2] = z;
    base->sink.sensor(base->sink.userdata, sensor, BLE_EmitTime(base, time_ns), sensor_ns, data);
}

bool SDL_BLE_QueueWrite(SDL_BLEBase *base, int characteristic, const uint8_t *data, size_t length, bool response)
{
    SDL_BLEWrite *write;

    if (base->write_count >= SDL_BLE_MAX_WRITES || length > SDL_BLE_MAX_WRITE ||
        characteristic < 0 || characteristic >= SDL_BLE_MAX_CHARS) {
        return false;
    }
    write = &base->writes[(base->write_head + base->write_count) % SDL_BLE_MAX_WRITES];
    memset(write, 0, sizeof(*write));
    write->characteristic = (uint8_t)characteristic;
    write->response = response;
    write->length = (uint8_t)length;
    if (length) {
        memcpy(write->data, data, length);
    }
    ++base->write_count;
    return true;
}

bool SDL_BLE_NextWrite(SDL_BLEBase *base, SDL_BLEWrite *write)
{
    if (base->write_count == 0) {
        return false;
    }
    *write = base->writes[base->write_head];
    base->write_head = (base->write_head + 1) % SDL_BLE_MAX_WRITES;
    --base->write_count;
    return true;
}

void SDL_BLE_ClearWrites(SDL_BLEBase *base)
{
    base->write_head = 0;
    base->write_count = 0;
}

void SDL_BLE_Log(SDL_BLEBase *base, const char *text)
{
    if (base->sink.log) {
        base->sink.log(base->sink.userdata, text);
    }
}

void SDL_BLE_EarlierDeadline(bool *have, uint64_t *deadline, uint64_t candidate)
{
    if (!*have || candidate < *deadline) {
        *have = true;
        *deadline = candidate;
    }
}

void SDL_BLE_NoWriteDone(void *state, bool success, uint64_t now)
{
    (void)state;
    (void)success;
    (void)now;
}

void SDL_BLE_NoTick(void *state, uint64_t now)
{
    (void)state;
    (void)now;
}

bool SDL_BLE_NoDeadline(void *state, uint64_t *deadline)
{
    (void)state;
    (void)deadline;
    return false;
}

void SDL_BLE_NoClose(void *state, uint64_t now)
{
    (void)state;
    (void)now;
}
