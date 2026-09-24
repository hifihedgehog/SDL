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

#include "SDL_rb3pro_proto.h"

#include <string.h>

#define RB3PRO_AXIS_MAX 32767

/* PS3 report byte 5 onward: the block both transports share. On Xbox 360 it
   starts at bLeftTrigger, and bytes 10 to 15 are the six trailing XUSB
   bytes. */
#define RB3PRO_BLOCK_OFFSET  5
#define RB3PRO_BLOCK_LENGTH  16
#define RB3PRO_TRAILING      10

/* The navigation-only marker Linux watches for */
#define RB3PRO_MODE_BYTE       24
#define RB3PRO_MODE_NAVIGATION 0x02

static const struct
{
    uint16_t vendor;
    uint16_t product;
    int variant;
    bool ps3;
    const char *name;
} rb3pro_devices[] = {
    { 0x12BA, 0x2330, SDL_RB3PRO_KEYBOARD, true, "Rock Band 3 Keyboard" },
    { 0x12BA, 0x2338, SDL_RB3PRO_KEYBOARD, true, "Rock Band 3 MIDI Pro Adapter (Keyboard)" },
    { 0x12BA, 0x2430, SDL_RB3PRO_GUITAR, true, "Rock Band 3 Pro Guitar" },
    { 0x12BA, 0x2438, SDL_RB3PRO_GUITAR, true, "Rock Band 3 MIDI Pro Adapter (Guitar)" },
    { 0x12BA, 0x2530, SDL_RB3PRO_GUITAR, true, "Rock Band 3 Pro Guitar" },
    { 0x12BA, 0x2538, SDL_RB3PRO_GUITAR, true, "Rock Band 3 MIDI Pro Adapter (Guitar)" },
    { 0x1BAD, 0x3330, SDL_RB3PRO_KEYBOARD, false, "Rock Band 3 Keyboard" },
    { 0x1BAD, 0x3338, SDL_RB3PRO_KEYBOARD, false, "Rock Band 3 MIDI Pro Adapter (Keyboard)" },
    { 0x1BAD, 0x3430, SDL_RB3PRO_GUITAR, false, "Rock Band 3 Pro Guitar" },
    { 0x1BAD, 0x3438, SDL_RB3PRO_GUITAR, false, "Rock Band 3 MIDI Pro Adapter (Guitar)" },
    { 0x1BAD, 0x3530, SDL_RB3PRO_GUITAR, false, "Rock Band 3 Pro Guitar" },
    { 0x1BAD, 0x3538, SDL_RB3PRO_GUITAR, false, "Rock Band 3 MIDI Pro Adapter (Guitar)" },
};

/* The five rows of the enable. Byte 2 of row 1 is 0x89 to enable, 0x81 to
   disable. */
static const uint8_t rb3pro_enable[SDL_RB3PRO_ENABLE_LENGTH] = {
    0xE9, 0x00, 0x89, 0x1B, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x89, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xE9, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

bool SDL_RB3Pro_GetDevice(uint16_t vendor, uint16_t product, SDL_RB3ProDevice *device)
{
    size_t i;

    for (i = 0; i < sizeof(rb3pro_devices) / sizeof(rb3pro_devices[0]); ++i) {
        if (rb3pro_devices[i].vendor == vendor && rb3pro_devices[i].product == product) {
            if (device) {
                device->variant = rb3pro_devices[i].variant;
                device->ps3 = rb3pro_devices[i].ps3;
                device->name = rb3pro_devices[i].name;
            }
            return true;
        }
    }
    return false;
}

int SDL_RB3Pro_VariantForXInputSubtype(uint8_t subtype)
{
    switch (subtype) {
    case SDL_RB3PRO_XINPUT_SUBTYPE_KEYBOARD:
        return SDL_RB3PRO_KEYBOARD;
    case SDL_RB3PRO_XINPUT_SUBTYPE_GUITAR:
        return SDL_RB3PRO_GUITAR;
    default:
        return SDL_RB3PRO_NONE;
    }
}

const char *SDL_RB3Pro_XInputName(int variant)
{
    switch (variant) {
    case SDL_RB3PRO_KEYBOARD:
        return "Rock Band 3 Keyboard (Xbox 360)";
    case SDL_RB3PRO_GUITAR:
        return "Rock Band 3 Pro Guitar (Xbox 360)";
    default:
        return NULL;
    }
}

bool SDL_RB3Pro_GetLayout(int variant, int *nbuttons, int *naxes, int *nhats)
{
    int buttons, axes;

    switch (variant) {
    case SDL_RB3PRO_KEYBOARD:
        buttons = SDL_RB3PRO_KEYBOARD_BUTTONS;
        axes = SDL_RB3PRO_KEYBOARD_AXES;
        break;
    case SDL_RB3PRO_GUITAR:
        buttons = SDL_RB3PRO_GUITAR_BUTTONS;
        axes = SDL_RB3PRO_GUITAR_AXES;
        break;
    default:
        return false;
    }
    if (nbuttons) {
        *nbuttons = buttons;
    }
    if (naxes) {
        *naxes = axes;
    }
    if (nhats) {
        *nhats = 1;
    }
    return true;
}

/* 0 to 127 onto 0 to 32767. Bytes past 127 read as 127. */
static int16_t Scale7(uint8_t value)
{
    if (value > 127) {
        value = 127;
    }
    return (int16_t)(((int)value * RB3PRO_AXIS_MAX) / 127);
}

/* A fret number, 0 to 31, onto 0 to 32767 */
static int16_t Scale5(uint8_t value)
{
    return (int16_t)(((int)(value & 0x1F) * RB3PRO_AXIS_MAX) / 31);
}

static void SetButton(SDL_RB3ProOutput *out, int button, bool pressed)
{
    out->button_mask |= (uint64_t)1 << button;
    if (pressed) {
        out->buttons |= (uint64_t)1 << button;
    }
}

static void SetAxis(SDL_RB3ProOutput *out, int axis, int16_t value)
{
    out->axis_mask |= (uint16_t)(1u << axis);
    out->axes[axis] = value;
}

/* 0 is up and the values run clockwise, 8 and above are neutral */
static uint8_t DecodeDpad(uint8_t value)
{
    switch (value) {
    case 0:
        return SDL_RB3PRO_HAT_UP;
    case 1:
        return SDL_RB3PRO_HAT_UP | SDL_RB3PRO_HAT_RIGHT;
    case 2:
        return SDL_RB3PRO_HAT_RIGHT;
    case 3:
        return SDL_RB3PRO_HAT_DOWN | SDL_RB3PRO_HAT_RIGHT;
    case 4:
        return SDL_RB3PRO_HAT_DOWN;
    case 5:
        return SDL_RB3PRO_HAT_DOWN | SDL_RB3PRO_HAT_LEFT;
    case 6:
        return SDL_RB3PRO_HAT_LEFT;
    case 7:
        return SDL_RB3PRO_HAT_UP | SDL_RB3PRO_HAT_LEFT;
    default:
        return SDL_RB3PRO_HAT_CENTERED;
    }
}

static void DecodeKeyboardBlock(const uint8_t *block, SDL_RB3ProOutput *out)
{
    int key;

    /* Key 1, C1, is bit 7 of the first byte, and key 25, C3, bit 7 of the
       fourth, which also holds velocity slot 1 */
    for (key = 0; key < SDL_RB3PRO_KEYBOARD_KEYS; ++key) {
        const uint8_t byte = block[key / 8];
        const int bit = 7 - (key % 8);
        SetButton(out, SDL_RB3PRO_KEYBOARD_FIRST_KEY + key, ((byte >> bit) & 1) != 0);
    }
    for (key = 0; key < SDL_RB3PRO_KEYBOARD_VELOCITIES; ++key) {
        SetAxis(out, SDL_RB3PRO_KEYBOARD_FIRST_VELOCITY + key, Scale7((uint8_t)(block[3 + key] & 0x7F)));
    }
    SetButton(out, SDL_RB3PRO_KEYBOARD_OVERDRIVE, (block[8] & 0x80) != 0);
    SetButton(out, SDL_RB3PRO_KEYBOARD_PEDAL, (block[9] & 0x80) != 0);
    SetAxis(out, SDL_RB3PRO_KEYBOARD_PEDAL_AXIS, Scale7((uint8_t)(block[9] & 0x7F)));

    SetAxis(out, SDL_RB3PRO_KEYBOARD_TOUCH_STRIP, Scale7((uint8_t)(block[10] & 0x7F)));
    SetButton(out, SDL_RB3PRO_KEYBOARD_PEDAL_CONNECTED, (block[15] & 0x01) != 0);
}

static void DecodeGuitarBlock(const uint8_t *block, SDL_RB3ProOutput *out)
{
    uint8_t frets[SDL_RB3PRO_GUITAR_STRINGS];
    int i;

    /* Two little-endian words of three 5-bit fields each: low E, A and D,
       then G, B and high E. Bit 15 of the second is the solo flag. */
    frets[0] = (uint8_t)(block[0] & 0x1F);
    frets[1] = (uint8_t)(((block[0] >> 5) & 0x07) | ((block[1] & 0x03) << 3));
    frets[2] = (uint8_t)((block[1] >> 2) & 0x1F);
    frets[3] = (uint8_t)(block[2] & 0x1F);
    frets[4] = (uint8_t)(((block[2] >> 5) & 0x07) | ((block[3] & 0x03) << 3));
    frets[5] = (uint8_t)((block[3] >> 2) & 0x1F);
    for (i = 0; i < SDL_RB3PRO_GUITAR_STRINGS; ++i) {
        SetAxis(out, SDL_RB3PRO_GUITAR_FIRST_FRET + i, Scale5(frets[i]));
        SetAxis(out, SDL_RB3PRO_GUITAR_FIRST_VELOCITY + i, Scale7((uint8_t)(block[4 + i] & 0x7F)));
    }
    SetButton(out, SDL_RB3PRO_GUITAR_SOLO, (block[3] & 0x80) != 0);
    for (i = 0; i < SDL_RB3PRO_GUITAR_COLORS; ++i) {
        SetButton(out, SDL_RB3PRO_GUITAR_FIRST_COLOR + i, (block[4 + i] & 0x80) != 0);
    }

    /* The six trailing bytes: microphone, light sensor, tilt, pedal, unused,
       pedal connected. With the sensors off the first two copy the tilt. */
    SetAxis(out, SDL_RB3PRO_GUITAR_MICROPHONE, Scale7(block[10]));
    SetAxis(out, SDL_RB3PRO_GUITAR_LIGHT, Scale7(block[11]));
    SetAxis(out, SDL_RB3PRO_GUITAR_TILT, Scale7(block[12]));
    SetButton(out, SDL_RB3PRO_GUITAR_PEDAL, (block[13] & 0x80) != 0);
    SetButton(out, SDL_RB3PRO_GUITAR_PEDAL_CONNECTED, (block[15] & 0x01) != 0);
}

static void DecodeBlock(int variant, const uint8_t *block, SDL_RB3ProOutput *out)
{
    int button;

    /* Buttons 7 to 10 exist for the gamepad order and are never pressed */
    for (button = SDL_RB3PRO_BUTTON_START + 1; button < SDL_RB3PRO_GAMEPAD_BUTTONS; ++button) {
        SetButton(out, button, false);
    }
    if (variant == SDL_RB3PRO_KEYBOARD) {
        DecodeKeyboardBlock(block, out);
    } else {
        DecodeGuitarBlock(block, out);
    }
}

bool SDL_RB3Pro_DecodeReport(int variant, const uint8_t *report, size_t length, SDL_RB3ProOutput *out)
{
    if (!report || !out || length < SDL_RB3PRO_REPORT_LENGTH ||
        (variant != SDL_RB3PRO_KEYBOARD && variant != SDL_RB3PRO_GUITAR)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    SetButton(out, SDL_RB3PRO_BUTTON_WEST, (report[0] & 0x01) != 0);
    SetButton(out, SDL_RB3PRO_BUTTON_SOUTH, (report[0] & 0x02) != 0);
    SetButton(out, SDL_RB3PRO_BUTTON_EAST, (report[0] & 0x04) != 0);
    SetButton(out, SDL_RB3PRO_BUTTON_NORTH, (report[0] & 0x08) != 0);
    SetButton(out, SDL_RB3PRO_BUTTON_BACK, (report[1] & 0x01) != 0);
    SetButton(out, SDL_RB3PRO_BUTTON_START, (report[1] & 0x02) != 0);
    SetButton(out, SDL_RB3PRO_BUTTON_GUIDE, (report[1] & 0x10) != 0);
    out->hat = DecodeDpad(report[2]);
    DecodeBlock(variant, report + RB3PRO_BLOCK_OFFSET, out);
    return true;
}

bool SDL_RB3Pro_DecodeXInput(int variant, const SDL_RB3ProXInputState *state, SDL_RB3ProOutput *out)
{
    uint8_t block[RB3PRO_BLOCK_LENGTH];

    if (!state || !out || (variant != SDL_RB3PRO_KEYBOARD && variant != SDL_RB3PRO_GUITAR)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    SetButton(out, SDL_RB3PRO_BUTTON_SOUTH, (state->buttons & 0x1000) != 0);
    SetButton(out, SDL_RB3PRO_BUTTON_EAST, (state->buttons & 0x2000) != 0);
    SetButton(out, SDL_RB3PRO_BUTTON_WEST, (state->buttons & 0x4000) != 0);
    SetButton(out, SDL_RB3PRO_BUTTON_NORTH, (state->buttons & 0x8000) != 0);
    SetButton(out, SDL_RB3PRO_BUTTON_BACK, (state->buttons & 0x0020) != 0);
    SetButton(out, SDL_RB3PRO_BUTTON_START, (state->buttons & 0x0010) != 0);
    SetButton(out, SDL_RB3PRO_BUTTON_GUIDE, (state->buttons & 0x0400) != 0);
    if (state->buttons & 0x0001) {
        out->hat |= SDL_RB3PRO_HAT_UP;
    }
    if (state->buttons & 0x0002) {
        out->hat |= SDL_RB3PRO_HAT_DOWN;
    }
    if (state->buttons & 0x0004) {
        out->hat |= SDL_RB3PRO_HAT_LEFT;
    }
    if (state->buttons & 0x0008) {
        out->hat |= SDL_RB3PRO_HAT_RIGHT;
    }

    /* XInput passes the report's fields through: the triggers and the four
       little-endian stick words are PS3 bytes 5 to 14 */
    block[0] = state->left_trigger;
    block[1] = state->right_trigger;
    block[2] = (uint8_t)((uint16_t)state->thumb_lx & 0xFF);
    block[3] = (uint8_t)((uint16_t)state->thumb_lx >> 8);
    block[4] = (uint8_t)((uint16_t)state->thumb_ly & 0xFF);
    block[5] = (uint8_t)((uint16_t)state->thumb_ly >> 8);
    block[6] = (uint8_t)((uint16_t)state->thumb_rx & 0xFF);
    block[7] = (uint8_t)((uint16_t)state->thumb_rx >> 8);
    block[8] = (uint8_t)((uint16_t)state->thumb_ry & 0xFF);
    block[9] = (uint8_t)((uint16_t)state->thumb_ry >> 8);
    /* Without the trailing bytes, what they carry reads as zero: no touch
       strip, tilt or sensor reading, no pedal and no pedal connected */
    if (state->has_trailing) {
        memcpy(&block[RB3PRO_TRAILING], state->trailing, sizeof(state->trailing));
    } else {
        memset(&block[RB3PRO_TRAILING], 0, sizeof(state->trailing));
    }
    DecodeBlock(variant, block, out);
    return true;
}

void SDL_RB3Pro_BuildEnable(uint8_t out[1 + SDL_RB3PRO_ENABLE_LENGTH])
{
    out[0] = 0x00;
    memcpy(&out[1], rb3pro_enable, sizeof(rb3pro_enable));
}

void SDL_RB3ProEnable_Init(SDL_RB3ProEnable *enable, bool armed, bool split)
{
    memset(enable, 0, sizeof(*enable));
    enable->armed = armed;
    enable->split = split;
    enable->next_row = SDL_RB3PRO_ENABLE_ROWS;
}

static void Begin(SDL_RB3ProEnable *enable, uint64_t now_us)
{
    enable->started = true;
    enable->last_start_us = now_us;
    if (enable->split) {
        enable->next_row = 0;
        /* A pause still running after row 5 of the last sequence holds */
        if (enable->next_due_us < now_us) {
            enable->next_due_us = now_us;
        }
    }
}

bool SDL_RB3ProEnable_Open(SDL_RB3ProEnable *enable, uint64_t now_us)
{
    if (!enable->armed) {
        return false;
    }
    Begin(enable, now_us);
    return true;
}

bool SDL_RB3ProEnable_OnReport(SDL_RB3ProEnable *enable, const uint8_t *report, size_t length, uint64_t now_us)
{
    if (!enable->armed || !report || length <= RB3PRO_MODE_BYTE ||
        report[RB3PRO_MODE_BYTE] != RB3PRO_MODE_NAVIGATION) {
        return false;
    }
    if (enable->started && now_us <= enable->last_start_us + SDL_RB3PRO_ENABLE_REPEAT_US) {
        return false;
    }
    Begin(enable, now_us);
    return true;
}

bool SDL_RB3ProEnable_NextRow(SDL_RB3ProEnable *enable, uint64_t now_us, uint8_t out[SDL_RB3PRO_ENABLE_ROW_REPORT_LENGTH])
{
    int row;

    if (!enable->armed || !enable->split || enable->next_row >= SDL_RB3PRO_ENABLE_ROWS ||
        now_us < enable->next_due_us) {
        return false;
    }

    row = enable->next_row++;
    out[0] = 0x00;
    memcpy(&out[1], &rb3pro_enable[row * SDL_RB3PRO_ENABLE_ROW_BYTES], SDL_RB3PRO_ENABLE_ROW_BYTES);

    /* Rows 1 and 2 go back to back. A pause follows each of rows 2 to 5. */
    enable->next_due_us = (row == 0) ? now_us : now_us + SDL_RB3PRO_ENABLE_PAUSE_US;
    return true;
}
