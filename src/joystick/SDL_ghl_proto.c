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

#include "SDL_ghl_proto.h"

#include <string.h>

#define GHL_AXIS_MIN (-32768)

int SDL_GHL_GetDongle(uint16_t vendor, uint16_t product)
{
    if (vendor == 0x12BA && product == 0x074B) {
        return SDL_GHL_DONGLE_PS3;
    }
    if (vendor == 0x1430 && product == 0x07BB) {
        return SDL_GHL_DONGLE_PS4;
    }
    if (vendor == 0x1430 && product == 0x079B) {
        return SDL_GHL_DONGLE_XBOXONE;
    }
    return SDL_GHL_DONGLE_NONE;
}

const char *SDL_GHL_DongleName(int dongle)
{
    switch (dongle) {
    case SDL_GHL_DONGLE_PS3:
        return "Guitar Hero Live Guitar (PS3/Wii U)";
    case SDL_GHL_DONGLE_PS4:
        return "Guitar Hero Live Guitar (PS4)";
    case SDL_GHL_DONGLE_XBOXONE:
        return "Guitar Hero Live Guitar (Xbox One)";
    default:
        return NULL;
    }
}

static int16_t ScaleByte(uint8_t value)
{
    return (int16_t)((int)value * 257 + GHL_AXIS_MIN);
}

/* 0 is up and the values run clockwise. 8 and above are neutral: 0x0F on
   the PS3 and Xbox One guitars, 8 on the PS4 one. */
static uint8_t DecodeDpad(uint8_t value)
{
    switch (value) {
    case 0:
        return SDL_GHL_HAT_UP;
    case 1:
        return SDL_GHL_HAT_UP | SDL_GHL_HAT_RIGHT;
    case 2:
        return SDL_GHL_HAT_RIGHT;
    case 3:
        return SDL_GHL_HAT_DOWN | SDL_GHL_HAT_RIGHT;
    case 4:
        return SDL_GHL_HAT_DOWN;
    case 5:
        return SDL_GHL_HAT_DOWN | SDL_GHL_HAT_LEFT;
    case 6:
        return SDL_GHL_HAT_LEFT;
    case 7:
        return SDL_GHL_HAT_UP | SDL_GHL_HAT_LEFT;
    default:
        return SDL_GHL_HAT_CENTERED;
    }
}

/* The strum bar rests at 0x80 and reads 0x00 up and 0xFF down. The hat adds
   up and down only at those two values. */
static uint8_t StrumHat(uint8_t strum)
{
    if (strum == 0x00) {
        return SDL_GHL_HAT_UP;
    }
    if (strum == 0xFF) {
        return SDL_GHL_HAT_DOWN;
    }
    return SDL_GHL_HAT_CENTERED;
}

static void SetButton(SDL_GHLOutput *out, int button, bool pressed)
{
    if (pressed) {
        out->buttons |= (uint16_t)(1u << button);
    }
}

static void Finish(SDL_GHLOutput *out, uint8_t dpad, uint8_t strum, uint8_t tilt, uint8_t whammy)
{
    out->button_mask = (uint16_t)((1u << SDL_GHL_NUM_BUTTONS) - 1);
    out->hat = (uint8_t)(DecodeDpad(dpad) | StrumHat(strum));
    out->axes[SDL_GHL_AXIS_LEFTX] = 0;
    out->axes[SDL_GHL_AXIS_LEFTY] = ScaleByte(strum);
    out->axes[SDL_GHL_AXIS_RIGHTX] = ScaleByte(tilt);
    out->axes[SDL_GHL_AXIS_RIGHTY] = ScaleByte(whammy);
    out->axes[SDL_GHL_AXIS_LEFT_TRIGGER] = GHL_AXIS_MIN;
    out->axes[SDL_GHL_AXIS_RIGHT_TRIGGER] = GHL_AXIS_MIN;
}

bool SDL_GHL_DecodeFrameA(const uint8_t *report, size_t length, SDL_GHLOutput *out)
{
    if (!report || !out || length < SDL_GHL_FRAME_A_LENGTH || length > SDL_GHL_FRAME_A_MAX_LENGTH) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    SetButton(out, SDL_GHL_BUTTON_WEST, (report[0] & 0x01) != 0);
    SetButton(out, SDL_GHL_BUTTON_SOUTH, (report[0] & 0x02) != 0);
    SetButton(out, SDL_GHL_BUTTON_EAST, (report[0] & 0x04) != 0);
    SetButton(out, SDL_GHL_BUTTON_NORTH, (report[0] & 0x08) != 0);
    SetButton(out, SDL_GHL_BUTTON_LEFT_SHOULDER, (report[0] & 0x10) != 0);
    SetButton(out, SDL_GHL_BUTTON_RIGHT_SHOULDER, (report[0] & 0x20) != 0);
    SetButton(out, SDL_GHL_BUTTON_BACK, (report[1] & 0x01) != 0);
    SetButton(out, SDL_GHL_BUTTON_START, (report[1] & 0x02) != 0);
    SetButton(out, SDL_GHL_BUTTON_LEFT_STICK, (report[1] & 0x04) != 0);
    SetButton(out, SDL_GHL_BUTTON_GUIDE, (report[1] & 0x10) != 0);

    /* Byte 4 is the strum bar, byte 6 the whammy and byte 19 the tilt.
       Byte 5 reads 0x00 or 0xFF only at the tilt extremes and is not used. */
    Finish(out, report[2], report[4], report[19], report[6]);
    return true;
}

bool SDL_GHL_DecodeFrameB(const uint8_t *report, size_t length, SDL_GHLOutput *out, bool *connected)
{
    if (!report || !out || !connected || length < SDL_GHL_FRAME_B_LENGTH ||
        report[0] != SDL_GHL_FRAME_B_REPORT_ID) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    SetButton(out, SDL_GHL_BUTTON_WEST, (report[5] & 0x10) != 0);
    SetButton(out, SDL_GHL_BUTTON_SOUTH, (report[5] & 0x20) != 0);
    SetButton(out, SDL_GHL_BUTTON_EAST, (report[5] & 0x40) != 0);
    SetButton(out, SDL_GHL_BUTTON_NORTH, (report[5] & 0x80) != 0);
    SetButton(out, SDL_GHL_BUTTON_LEFT_SHOULDER, (report[6] & 0x01) != 0);
    SetButton(out, SDL_GHL_BUTTON_RIGHT_SHOULDER, (report[6] & 0x02) != 0);
    SetButton(out, SDL_GHL_BUTTON_START, (report[6] & 0x20) != 0);
    SetButton(out, SDL_GHL_BUTTON_LEFT_STICK, (report[6] & 0x40) != 0);
    SetButton(out, SDL_GHL_BUTTON_BACK, (report[6] & 0x80) != 0);
    SetButton(out, SDL_GHL_BUTTON_GUIDE, (report[7] & 0x01) != 0);

    /* Byte 2 is the strum bar, byte 3 the tilt and byte 4 the whammy, as
       PlasticBand documents them since its 2023 correction. */
    Finish(out, (uint8_t)(report[5] & 0x0F), report[2], report[3], report[4]);
    *connected = (report[25] != 0);
    return true;
}

bool SDL_GHL_DecodeXboxMessage(uint8_t message, const uint8_t *payload, size_t length, SDL_GHLOutput *out)
{
    const uint16_t guide = (uint16_t)(1u << SDL_GHL_BUTTON_GUIDE);

    if (message != SDL_GHL_XBOX_MESSAGE_GUITAR) {
        return false;
    }
    if (!SDL_GHL_DecodeFrameA(payload, length, out)) {
        return false;
    }
    out->buttons = (uint16_t)(out->buttons & ~guide);
    out->button_mask = (uint16_t)(out->button_mask & ~guide);
    return true;
}

size_t SDL_GHL_BuildHIDKeepAlive(int dongle, uint8_t out[SDL_GHL_HID_KEEPALIVE_LENGTH])
{
    if (!out) {
        return 0;
    }
    memset(out, 0, SDL_GHL_HID_KEEPALIVE_LENGTH);
    switch (dongle) {
    case SDL_GHL_DONGLE_PS3:
        /* No report ID. Output type 0x02 with 08 20. */
        out[0] = 0x00;
        out[1] = 0x02;
        out[2] = 0x08;
        out[3] = 0x20;
        return SDL_GHL_HID_KEEPALIVE_LENGTH;
    case SDL_GHL_DONGLE_PS4:
        /* Report ID 0x30, then 02 08 0A */
        out[0] = 0x30;
        out[1] = 0x02;
        out[2] = 0x08;
        out[3] = 0x0A;
        return SDL_GHL_HID_KEEPALIVE_LENGTH;
    default:
        return 0;
    }
}

size_t SDL_GHL_BuildXboxKeepAlive(uint8_t out[SDL_GHL_XBOX_KEEPALIVE_LENGTH])
{
    if (!out) {
        return 0;
    }
    memset(out, 0, SDL_GHL_XBOX_KEEPALIVE_LENGTH);
    out[0] = 0x02;
    out[1] = 0x08;
    out[2] = 0x0A;
    return SDL_GHL_XBOX_KEEPALIVE_LENGTH;
}

void SDL_GHL_KeepAliveStart(SDL_GHLKeepAlive *keepalive, uint64_t now_ms)
{
    keepalive->active = true;
    keepalive->next_due_ms = now_ms;
}

void SDL_GHL_KeepAliveStop(SDL_GHLKeepAlive *keepalive)
{
    keepalive->active = false;
    keepalive->next_due_ms = 0;
}

bool SDL_GHL_KeepAliveDue(const SDL_GHLKeepAlive *keepalive, uint64_t now_ms)
{
    return keepalive->active && now_ms >= keepalive->next_due_ms;
}

void SDL_GHL_KeepAliveSent(SDL_GHLKeepAlive *keepalive, uint64_t now_ms, bool success)
{
    if (success) {
        keepalive->next_due_ms = now_ms + SDL_GHL_KEEPALIVE_INTERVAL_MS;
    }
}
