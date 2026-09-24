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

#include "SDL_hidapi_ps3ext_proto.h"

#include <string.h>

#define PS3EXT_AXIS_MIN (-32768)
#define PS3EXT_AXIS_MAX 32767

static const struct
{
    uint16_t vendor;
    uint16_t product;
    int variant;
    const char *name;
} ps3ext_devices[] = {
    { 0x20D6, 0xCB17, SDL_PS3EXT_UDRAW, "THQ uDraw Game Tablet for PS3" },
    { 0x12BA, 0x04A0, SDL_PS3EXT_TOPSHOT_ELITE, "Top Shot Elite" },
    { 0x12BA, 0x04A1, SDL_PS3EXT_TOPSHOT_FEARMASTER, "Top Shot Fearmaster" },
    { 0x12BA, 0x0400, SDL_PS3EXT_TONYHAWK, "Tony Hawk RIDE Skateboard" },
    { 0x1430, 0x0100, SDL_PS3EXT_TONYHAWK, "Tony Hawk Skateboard" },
};

/* What the skateboard dongle sends with the board switched off, and with the
   board lost */
static const uint8_t board_off[SDL_PS3EXT_REPORT_LENGTH] = {
    0x00, 0x00, 0x0F, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02
};
static const uint8_t board_lost[SDL_PS3EXT_REPORT_LENGTH] = {
    0x00, 0x00, 0x0F, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

bool SDL_PS3Ext_GetDevice(uint16_t vendor, uint16_t product, SDL_PS3ExtDevice *device)
{
    size_t i;

    for (i = 0; i < sizeof(ps3ext_devices) / sizeof(ps3ext_devices[0]); ++i) {
        if (ps3ext_devices[i].vendor == vendor && ps3ext_devices[i].product == product) {
            if (device) {
                device->variant = ps3ext_devices[i].variant;
                device->name = ps3ext_devices[i].name;
            }
            return true;
        }
    }
    return false;
}

bool SDL_PS3Ext_GetLayout(int variant, SDL_PS3ExtLayout *layout)
{
    if (!layout) {
        return false;
    }
    memset(layout, 0, sizeof(*layout));
    layout->nhats = 1;
    switch (variant) {
    case SDL_PS3EXT_UDRAW:
        layout->nbuttons = 14;
        layout->naxes = 4;
        layout->accelerometer = true;
        layout->mapping = SDL_PS3EXT_MAPPING_BASE;
        return true;
    case SDL_PS3EXT_TOPSHOT_ELITE:
        layout->nbuttons = 16;
        layout->naxes = 8;
        layout->mapping = SDL_PS3EXT_MAPPING_TOPSHOT_ELITE;
        return true;
    case SDL_PS3EXT_TOPSHOT_FEARMASTER:
        layout->nbuttons = 16;
        layout->naxes = 7;
        layout->mapping = SDL_PS3EXT_MAPPING_TOPSHOT_FEARMASTER;
        return true;
    case SDL_PS3EXT_TONYHAWK:
        layout->nbuttons = 7;
        layout->naxes = 6;
        layout->mapping = SDL_PS3EXT_MAPPING_BASE;
        return true;
    default:
        return false;
    }
}

static void SetButton(SDL_PS3ExtOutput *out, int button, bool pressed)
{
    out->button_mask |= 1u << button;
    if (pressed) {
        out->buttons |= 1u << button;
    }
}

static void SetAxis(SDL_PS3ExtOutput *out, int axis, int value, bool data)
{
    out->axis_mask |= (uint16_t)(1u << axis);
    if (data) {
        out->data_axis_mask |= (uint16_t)(1u << axis);
    }
    out->axes[axis] = (int16_t)value;
}

/* 0 is up and the values run clockwise. 8 and above are neutral. */
static uint8_t DecodeDpad(uint8_t value)
{
    switch (value) {
    case 0:
        return SDL_PS3EXT_HAT_UP;
    case 1:
        return SDL_PS3EXT_HAT_UP | SDL_PS3EXT_HAT_RIGHT;
    case 2:
        return SDL_PS3EXT_HAT_RIGHT;
    case 3:
        return SDL_PS3EXT_HAT_DOWN | SDL_PS3EXT_HAT_RIGHT;
    case 4:
        return SDL_PS3EXT_HAT_DOWN;
    case 5:
        return SDL_PS3EXT_HAT_DOWN | SDL_PS3EXT_HAT_LEFT;
    case 6:
        return SDL_PS3EXT_HAT_LEFT;
    case 7:
        return SDL_PS3EXT_HAT_UP | SDL_PS3EXT_HAT_LEFT;
    default:
        return SDL_PS3EXT_HAT_CENTERED;
    }
}

/* Square, cross, circle and triangle in byte 0 bits 0 to 3, select, start and
   PS in byte 1 bits 0, 1 and 4, the d-pad in byte 2 */
static void DecodeCommon(const uint8_t *report, SDL_PS3ExtOutput *out)
{
    SetButton(out, SDL_PS3EXT_BUTTON_WEST, (report[0] & 0x01) != 0);
    SetButton(out, SDL_PS3EXT_BUTTON_SOUTH, (report[0] & 0x02) != 0);
    SetButton(out, SDL_PS3EXT_BUTTON_EAST, (report[0] & 0x04) != 0);
    SetButton(out, SDL_PS3EXT_BUTTON_NORTH, (report[0] & 0x08) != 0);
    SetButton(out, SDL_PS3EXT_BUTTON_BACK, (report[1] & 0x01) != 0);
    SetButton(out, SDL_PS3EXT_BUTTON_START, (report[1] & 0x02) != 0);
    SetButton(out, SDL_PS3EXT_BUTTON_GUIDE, (report[1] & 0x10) != 0);
    out->hat = DecodeDpad(report[2]);
}

static int ScaleRange(int value, int max)
{
    if (value > max) {
        value = max;
    }
    return (value * 65535) / max + PS3EXT_AXIS_MIN;
}

static int ScalePositive(int value, int max)
{
    if (value > max) {
        value = max;
    }
    return (value * PS3EXT_AXIS_MAX) / max;
}

static int ScaleByte(uint8_t value)
{
    return (int)value * 257 + PS3EXT_AXIS_MIN;
}

static int16_t Word(const uint8_t *bytes)
{
    return (int16_t)((int)(bytes[0] | (bytes[1] << 8)) - 0x200);
}

static void DecodeUDraw(const uint8_t *report, SDL_PS3ExtOutput *out)
{
    const uint8_t touch = report[11];
    const bool pen = (touch == 0x40);
    const bool finger = (touch == 0x80);
    const bool two_fingers = (touch != 0x00 && !pen && !finger);
    int button, pressure;

    for (button = SDL_PS3EXT_BUTTON_LEFT_STICK; button < SDL_PS3EXT_UDRAW_PEN; ++button) {
        SetButton(out, button, false);
    }
    SetButton(out, SDL_PS3EXT_UDRAW_PEN, pen);
    SetButton(out, SDL_PS3EXT_UDRAW_FINGER, finger);
    SetButton(out, SDL_PS3EXT_UDRAW_TWO_FINGERS, two_fingers);

    /* An 8 by 5 grid of 256 by 256 cells, the last column 128 wide and the
       last row 56 high. A cell byte of 0x0F means nothing is on that axis,
       and the axis keeps its last value. */
    if (touch != 0x00) {
        if (report[15] != 0x0F) {
            SetAxis(out, SDL_PS3EXT_UDRAW_AXIS_X, ScaleRange(report[15] * 256 + report[17], SDL_PS3EXT_UDRAW_MAX_X), true);
        }
        if (report[16] != 0x0F) {
            SetAxis(out, SDL_PS3EXT_UDRAW_AXIS_Y, ScaleRange(report[16] * 256 + report[18], SDL_PS3EXT_UDRAW_MAX_Y), true);
        }
    }

    /* Pressure reads 0x60 released and about 0x74 on light contact. Linux
       treats 113 as zero. */
    pressure = pen ? (int)report[13] - 113 : 0;
    if (pressure < 0) {
        pressure = 0;
    }
    SetAxis(out, SDL_PS3EXT_UDRAW_AXIS_PRESSURE, ScalePositive(pressure, SDL_PS3EXT_UDRAW_MAX_PRESSURE), true);
    SetAxis(out, SDL_PS3EXT_UDRAW_AXIS_DISTANCE, two_fingers ? ScalePositive(report[12], 255) : 0, true);

    out->has_accel = true;
    out->accel[0] = Word(&report[19]);
    out->accel[1] = Word(&report[21]);
    out->accel[2] = Word(&report[23]);
}

/* A 10-bit LED coordinate: X in byte 0 and bits 7 and 6 of byte 1, Y in bits
   5 to 0 of byte 1 and bits 7 to 4 of byte 2. The detect nibble, bits 3 to 0
   of byte 2, reads 0x2 with the LED in view and 0xF without it. */
static void DecodeLED(const uint8_t *bytes, int *x, int *y, bool *seen)
{
    const int detect = bytes[2] & 0x0F;
    *x = (bytes[0] << 2) | (bytes[1] >> 6);
    *y = ((bytes[1] & 0x3F) << 4) | (bytes[2] >> 4);
    *seen = (detect != 0x0 && detect != 0xF);
}

static void DecodeTopShot(int variant, const uint8_t *report, SDL_PS3ExtOutput *out)
{
    const bool elite = (variant == SDL_PS3EXT_TOPSHOT_ELITE);
    int lx, ly, rx, ry, first_led;
    bool left_seen, right_seen;

    SetButton(out, SDL_PS3EXT_BUTTON_LEFT_STICK, (report[1] & 0x04) != 0);
    SetButton(out, SDL_PS3EXT_BUTTON_RIGHT_STICK, elite && (report[1] & 0x08) != 0);
    SetButton(out, 9, false);
    SetButton(out, 10, false);
    SetButton(out, SDL_PS3EXT_TOPSHOT_TRIGGER, (report[0] & 0x20) != 0);
    SetButton(out, SDL_PS3EXT_TOPSHOT_RELOAD, elite && (report[0] & 0x10) != 0);
    SetButton(out, SDL_PS3EXT_TOPSHOT_HEART_TOUCH, !elite && (report[1] & 0x08) != 0);

    DecodeLED(&report[7], &lx, &ly, &left_seen);
    DecodeLED(&report[10], &rx, &ry, &right_seen);
    SetButton(out, SDL_PS3EXT_TOPSHOT_LEFT_SEEN, left_seen);
    SetButton(out, SDL_PS3EXT_TOPSHOT_RIGHT_SEEN, right_seen);

    SetAxis(out, 0, ScaleByte(report[3]), false);
    SetAxis(out, 1, ScaleByte(report[4]), false);
    if (elite) {
        SetAxis(out, 2, ScaleByte(report[5]), false);
        SetAxis(out, 3, ScaleByte(report[6]), false);
        first_led = 4;
    } else {
        /* Bytes 5 and 6 are the heart-rate word */
        const int heart = report[5] | (report[6] << 8);
        SetAxis(out, 6, heart > PS3EXT_AXIS_MAX ? PS3EXT_AXIS_MAX : heart, true);
        first_led = 2;
    }
    SetAxis(out, first_led + 0, ScaleRange(lx, SDL_PS3EXT_TOPSHOT_MAX_LED), true);
    SetAxis(out, first_led + 1, ScaleRange(ly, SDL_PS3EXT_TOPSHOT_MAX_LED), true);
    SetAxis(out, first_led + 2, ScaleRange(rx, SDL_PS3EXT_TOPSHOT_MAX_LED), true);
    SetAxis(out, first_led + 3, ScaleRange(ry, SDL_PS3EXT_TOPSHOT_MAX_LED), true);
}

static void DecodeTonyHawk(const uint8_t *report, SDL_PS3ExtOutput *out)
{
    int i;

    /* The IR sensors, nose, tail, left and right: 0x00 with the LED off up to
       0x38 for a close object */
    for (i = 0; i < 4; ++i) {
        SetAxis(out, i, ScalePositive(report[11 + i], SDL_PS3EXT_TONYHAWK_MAX_IR), true);
    }
    /* The nose and tail accelerometers' unencrypted axis, signed bytes */
    SetAxis(out, 4, (int)(int8_t)report[15] * 256, true);
    SetAxis(out, 5, (int)(int8_t)report[16] * 256, true);
}

bool SDL_PS3Ext_Decode(int variant, const uint8_t *report, size_t length, SDL_PS3ExtOutput *out)
{
    if (!report || !out || length < SDL_PS3EXT_REPORT_LENGTH) {
        return false;
    }
    switch (variant) {
    case SDL_PS3EXT_UDRAW:
        if (length != SDL_PS3EXT_REPORT_LENGTH) {
            return false;
        }
        break;
    case SDL_PS3EXT_TOPSHOT_ELITE:
    case SDL_PS3EXT_TOPSHOT_FEARMASTER:
    case SDL_PS3EXT_TONYHAWK:
        break;
    default:
        return false;
    }

    memset(out, 0, sizeof(*out));
    DecodeCommon(report, out);
    switch (variant) {
    case SDL_PS3EXT_UDRAW:
        DecodeUDraw(report, out);
        break;
    case SDL_PS3EXT_TONYHAWK:
        DecodeTonyHawk(report, out);
        break;
    default:
        DecodeTopShot(variant, report, out);
        break;
    }
    return true;
}

int SDL_PS3Ext_TonyHawkBoardState(const uint8_t *report, size_t length)
{
    if (!report || length < SDL_PS3EXT_REPORT_LENGTH) {
        return SDL_PS3EXT_BOARD_INVALID;
    }
    if (memcmp(report, board_off, SDL_PS3EXT_REPORT_LENGTH) == 0 ||
        memcmp(report, board_lost, SDL_PS3EXT_REPORT_LENGTH) == 0) {
        return SDL_PS3EXT_BOARD_IDLE;
    }
    return SDL_PS3EXT_BOARD_LIVE;
}

size_t SDL_PS3Ext_BuildTonyHawkActivation(uint8_t out[SDL_PS3EXT_TONYHAWK_ACTIVATION_LENGTH])
{
    if (!out) {
        return 0;
    }
    memset(out, 0, SDL_PS3EXT_TONYHAWK_ACTIVATION_LENGTH);
    return SDL_PS3EXT_TONYHAWK_ACTIVATION_LENGTH;
}

size_t SDL_PS3Ext_BuildTopShotRequest(uint8_t out[SDL_PS3EXT_TOPSHOT_REQUEST_LENGTH])
{
    if (!out) {
        return 0;
    }
    memset(out, 0, SDL_PS3EXT_TOPSHOT_REQUEST_LENGTH);
    out[1] = 0x82; /* The sensor-mode command */
    out[3] = 0x01; /* The smallest nonzero mode */
    return SDL_PS3EXT_TOPSHOT_REQUEST_LENGTH;
}

int SDL_PS3ExtAim_Open(SDL_PS3ExtAim *aim, uint64_t now_ms)
{
    memset(aim, 0, sizeof(*aim));
    aim->open = true;
    aim->zero = true;
    aim->zero_since_ms = now_ms;
    return SDL_PS3EXT_REQUEST_OUTPUT;
}

void SDL_PS3ExtAim_OnReport(SDL_PS3ExtAim *aim, const uint8_t *report, size_t length, uint64_t now_ms)
{
    bool zero = true;
    int i;

    if (!aim->open || !report || length < SDL_PS3EXT_REPORT_LENGTH) {
        return;
    }
    for (i = 7; i <= 12; ++i) {
        if (report[i] != 0) {
            zero = false;
            break;
        }
    }
    if (zero) {
        if (!aim->zero) {
            aim->zero = true;
            aim->zero_since_ms = now_ms;
        }
    } else {
        aim->zero = false;
        aim->worked = true;
    }
}

int SDL_PS3ExtAim_Due(const SDL_PS3ExtAim *aim, uint64_t now_ms)
{
    if (!aim->open || !aim->zero ||
        now_ms < aim->zero_since_ms + SDL_PS3EXT_REQUEST_RETRY_MS ||
        now_ms < aim->last_request_ms + SDL_PS3EXT_REQUEST_RETRY_MS) {
        return SDL_PS3EXT_REQUEST_NONE;
    }
    if (aim->worked) {
        return aim->last_kind;
    }
    return (aim->last_kind == SDL_PS3EXT_REQUEST_OUTPUT) ? SDL_PS3EXT_REQUEST_FEATURE : SDL_PS3EXT_REQUEST_OUTPUT;
}

void SDL_PS3ExtAim_Sent(SDL_PS3ExtAim *aim, int kind, uint64_t now_ms)
{
    aim->last_kind = kind;
    aim->last_request_ms = now_ms;
    aim->worked = false;
}
