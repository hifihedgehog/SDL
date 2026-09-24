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

#include "SDL_hidapi_wii_ext_proto.h"

#include <string.h>

#define WII_EXT_AXIS_MIN (-32768)
#define WII_EXT_AXIS_MAX 32767

#define WII_REPORT_READ_MEMORY    0x21
#define WII_REPORT_WRITE_MEMORY   0x16
#define WII_REPORT_READ_REQUEST   0x17
#define WII_REPORT_MODE           0x12
#define WII_REPORT_STATUS_REQUEST 0x15

bool SDL_WiiExt_IsDecodedType(int type)
{
    return type >= SDL_WII_EXT_GUITAR && type <= SDL_WII_EXT_SHINKANSEN;
}

int SDL_WiiExt_Classify(uint8_t id0, uint8_t id4, uint8_t id5, bool decoding_enabled)
{
    /* The configurations that existed before, keyed on bytes 4 and 5 alone */
    if (id4 == 0x00 && id5 == 0x00) {
        return SDL_WII_EXT_NUNCHUK;
    }
    if (id4 == 0x01 && id5 == 0x01) {
        return SDL_WII_EXT_GAMEPAD;
    }
    if (id4 == 0x01 && id5 == 0x20) {
        return SDL_WII_EXT_WIIUPRO;
    }
    if (id4 == 0x04 && id5 == 0x02) {
        return SDL_WII_EXT_BALANCEBOARD;
    }
    if (!decoding_enabled) {
        return SDL_WII_EXT_UNKNOWN;
    }

    /* The guitar, drum kit and turntable share 01 03 and differ in byte 0 */
    if (id4 == 0x01 && id5 == 0x03) {
        switch (id0) {
        case 0x00:
            return SDL_WII_EXT_GUITAR;
        case 0x01:
            return SDL_WII_EXT_DRUMS;
        case 0x03:
            return SDL_WII_EXT_TURNTABLE;
        default:
            return SDL_WII_EXT_UNKNOWN;
        }
    }
    if (id4 == 0x01 && id5 == 0x11) {
        return SDL_WII_EXT_TAIKO;
    }
    if (id4 == 0x01 && id5 == 0x12) {
        return SDL_WII_EXT_UDRAW;
    }
    if (id4 == 0x00 && id5 == 0x13) {
        return SDL_WII_EXT_DRAWSOME;
    }
    /* Byte 4 is the data format code, and format 03 is the 8-byte layout
       the Shinkansen sends. 01 10 would mean a 6-byte format no source
       documents for it. */
    if (id4 == 0x03 && id5 == 0x10) {
        return SDL_WII_EXT_SHINKANSEN;
    }
    return SDL_WII_EXT_UNKNOWN;
}

void SDL_WiiExt_BuildReadRequest(uint8_t out[SDL_WII_EXT_READ_REQUEST_SIZE], bool rumble, uint32_t address, uint16_t size)
{
    out[0] = WII_REPORT_READ_REQUEST;
    out[1] = (uint8_t)(0x04 | (rumble ? 0x01 : 0x00));
    out[2] = (uint8_t)(address >> 16);
    out[3] = (uint8_t)(address >> 8);
    out[4] = (uint8_t)address;
    out[5] = (uint8_t)(size >> 8);
    out[6] = (uint8_t)size;
}

bool SDL_WiiExt_BuildWriteRequest(uint8_t out[SDL_WII_EXT_WRITE_REQUEST_SIZE], bool rumble, uint32_t address, const uint8_t *data, size_t size)
{
    if (!data || size == 0 || size > 16) {
        return false;
    }
    memset(out, 0, SDL_WII_EXT_WRITE_REQUEST_SIZE);
    out[0] = WII_REPORT_WRITE_MEMORY;
    out[1] = (uint8_t)(0x04 | (rumble ? 0x01 : 0x00));
    out[2] = (uint8_t)(address >> 16);
    out[3] = (uint8_t)(address >> 8);
    out[4] = (uint8_t)address;
    out[5] = (uint8_t)size;
    memcpy(&out[6], data, size);
    return true;
}

void SDL_WiiExt_BuildModeRequest(uint8_t out[SDL_WII_EXT_MODE_REQUEST_SIZE], bool rumble, uint8_t report)
{
    out[0] = WII_REPORT_MODE;
    out[1] = (uint8_t)(0x04 | (rumble ? 0x01 : 0x00)); /* 0x04: continuous reporting */
    out[2] = report;
}

void SDL_WiiExt_BuildStatusRequest(uint8_t out[SDL_WII_EXT_STATUS_REQUEST_SIZE], bool rumble)
{
    out[0] = WII_REPORT_STATUS_REQUEST;
    out[1] = (uint8_t)(rumble ? 0x01 : 0x00);
}

SDL_WiiExtReply SDL_WiiExt_ParseIdentifyReply(const uint8_t *report, size_t length, uint32_t address, uint8_t id[6])
{
    if (!report || length < 6 || report[0] != WII_REPORT_READ_MEMORY) {
        return SDL_WII_EXT_REPLY_REJECTED;
    }
    /* A reply to another read, such as the old 2-byte read at 0xFE */
    if (report[4] != (uint8_t)(address >> 8) || report[5] != (uint8_t)address) {
        return SDL_WII_EXT_REPLY_REJECTED;
    }
    if ((report[3] & 0x0F) == 0x07) {
        return SDL_WII_EXT_REPLY_ABSENT;
    }
    /* SE: size minus 1 in the high nibble, the error code in the low one */
    if (report[3] != 0x50 || length < 12) {
        return SDL_WII_EXT_REPLY_REJECTED;
    }
    memcpy(id, &report[6], 6);
    return SDL_WII_EXT_REPLY_OK;
}

bool SDL_WiiExt_ParseStoredIdReply(const uint8_t *report, size_t length, uint8_t *id0, uint8_t *id4, uint8_t *id5)
{
    if (!report || length < 10 || report[0] != WII_REPORT_READ_MEMORY) {
        return false;
    }
    if (report[4] != 0x00 || report[5] != 0xF6 || report[3] != 0x30) {
        return false;
    }
    /* 0xF6 holds ID byte 4, 0xF7 the challenge state, 0xF8 byte 0, 0xF9 byte 5 */
    *id4 = report[6];
    *id0 = report[8];
    *id5 = report[9];
    return true;
}

bool SDL_WiiExt_AllowsMotionPlus(int type)
{
    return !SDL_WiiExt_IsDecodedType(type);
}

uint8_t SDL_WiiExt_ReportMode(int type, bool sensors)
{
    /* 0x35 adds the remote's accelerometer, the guitar's tilt. 0x32 carries
       8 extension bytes, all the Shinkansen's format needs. */
    if (type == SDL_WII_EXT_GUITAR && sensors) {
        return 0x35;
    }
    return 0x32;
}

size_t SDL_WiiExt_StartupWrites(int type, SDL_WiiExtWrite writes[SDL_WII_EXT_MAX_STARTUP_WRITES])
{
    if (type == SDL_WII_EXT_DRAWSOME) {
        /* The Drawsome sends no pen data until 01 goes to 0xFB and then 55
           to 0xF0, which the standard init's 00 to 0xFB undoes. */
        writes[0].address = SDL_WII_EXT_REG_INIT2;
        writes[0].value = 0x01;
        writes[1].address = SDL_WII_EXT_REG_INIT1;
        writes[1].value = 0x55;
        return 2;
    }
    return 0;
}

bool SDL_WiiExt_RunStartup(int type, bool (*write)(void *userdata, uint32_t address, uint8_t value), void *userdata)
{
    SDL_WiiExtWrite writes[SDL_WII_EXT_MAX_STARTUP_WRITES];
    const size_t count = SDL_WiiExt_StartupWrites(type, writes);
    size_t i;

    for (i = 0; i < count; ++i) {
        if (!write || !write(userdata, writes[i].address, writes[i].value)) {
            return false;
        }
    }
    return true;
}

void SDL_WiiExt_BuildLEDWrite(uint8_t out[SDL_WII_EXT_WRITE_REQUEST_SIZE], bool rumble, bool on)
{
    const uint8_t value = on ? 0x01 : 0x00;
    (void)SDL_WiiExt_BuildWriteRequest(out, rumble, SDL_WII_EXT_REG_INIT2, &value, 1);
}

bool SDL_WiiExt_GetLayout(uint8_t report_id, SDL_WiiExtLayout *layout)
{
    /* Report ID, length, buttons, accelerometer, span offset, span length */
    static const struct
    {
        uint8_t id;
        SDL_WiiExtLayout layout;
    } layouts[] = {
        { 0x30, { 3, true, false, 0, 0 } },   /* BB BB */
        { 0x31, { 6, true, true, 0, 0 } },    /* BB BB AA AA AA */
        { 0x32, { 11, true, false, 3, 8 } },  /* BB BB EE*8 */
        { 0x33, { 18, true, true, 0, 0 } },   /* BB BB AA AA AA II*12 */
        { 0x34, { 22, true, false, 3, 19 } }, /* BB BB EE*19 */
        { 0x35, { 22, true, true, 6, 16 } },  /* BB BB AA AA AA EE*16 */
        { 0x36, { 22, true, false, 13, 9 } }, /* BB BB II*10 EE*9 */
        { 0x37, { 22, true, true, 16, 6 } },  /* BB BB AA AA AA II*10 EE*6 */
        { 0x3D, { 22, false, false, 1, 21 } },/* EE*21 */
    };
    size_t i;

    for (i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i) {
        if (layouts[i].id == report_id) {
            if (layout) {
                *layout = layouts[i].layout;
            }
            return true;
        }
    }
    return false;
}

bool SDL_WiiExt_GetSpan(const uint8_t *report, size_t length, const uint8_t **span, size_t *span_length, bool *has_accel)
{
    SDL_WiiExtLayout layout;

    if (!report || length == 0 || !SDL_WiiExt_GetLayout(report[0], &layout) || layout.span_length == 0) {
        return false;
    }
    if (length < layout.length) {
        return false;
    }
    *span = &report[layout.span_offset];
    *span_length = layout.span_length;
    if (has_accel) {
        *has_accel = layout.accel;
    }
    return true;
}

bool SDL_WiiExt_GetCaps(int type, SDL_WiiExtCaps *caps)
{
    if (!caps) {
        return false;
    }
    memset(caps, 0, sizeof(*caps));
    caps->nbuttons = 26; /* The remote's own buttons stay at 15-25 */
    caps->mapping = true;
    caps->joystick_type = SDL_WII_EXT_JOYSTICK_TYPE_GAMEPAD;
    switch (type) {
    case SDL_WII_EXT_GUITAR:
        caps->name = "Nintendo Wii Remote with Guitar";
        caps->joystick_type = SDL_WII_EXT_JOYSTICK_TYPE_GUITAR;
        caps->naxes = 6;
        caps->nhats = 1;
        caps->accel = true;
        return true;
    case SDL_WII_EXT_DRUMS:
        caps->name = "Nintendo Wii Remote with Drum Kit";
        caps->joystick_type = SDL_WII_EXT_JOYSTICK_TYPE_DRUM_KIT;
        caps->naxes = 13;
        return true;
    case SDL_WII_EXT_TURNTABLE:
        caps->name = "Nintendo Wii Remote with DJ Turntable";
        caps->nbuttons = 32;
        caps->naxes = 8;
        caps->mono_led = true;
        return true;
    case SDL_WII_EXT_TAIKO:
        caps->name = "Nintendo Wii Remote with Taiko Drum";
        caps->naxes = 6;
        return true;
    case SDL_WII_EXT_UDRAW:
        caps->name = "Nintendo Wii Remote with uDraw Tablet";
        caps->joystick_type = SDL_WII_EXT_JOYSTICK_TYPE_UNKNOWN;
        caps->naxes = 3;
        caps->mapping = false;
        return true;
    case SDL_WII_EXT_DRAWSOME:
        caps->name = "Nintendo Wii Remote with Drawsome Tablet";
        caps->joystick_type = SDL_WII_EXT_JOYSTICK_TYPE_UNKNOWN;
        caps->naxes = 3;
        caps->mapping = false;
        return true;
    case SDL_WII_EXT_SHINKANSEN:
        caps->name = "Nintendo Wii Remote with Shinkansen Controller";
        caps->naxes = 8;
        caps->nhats = 1;
        return true;
    default:
        return false;
    }
}

const char *SDL_WiiExt_TypeName(int type)
{
    SDL_WiiExtCaps caps;

    switch (type) {
    case SDL_WII_EXT_NONE:
        return "Nintendo Wii Remote";
    case SDL_WII_EXT_NUNCHUK:
        return "Nintendo Wii Remote with Nunchuk";
    case SDL_WII_EXT_GAMEPAD:
        return "Nintendo Wii Remote with Classic Controller";
    case SDL_WII_EXT_WIIUPRO:
        return "Nintendo Wii U Pro Controller";
    case SDL_WII_EXT_BALANCEBOARD:
        return "Nintendo Wii Balance Board";
    default:
        if (SDL_WiiExt_GetCaps(type, &caps)) {
            return caps.name;
        }
        return "Nintendo Wii Remote with Unknown Extension";
    }
}

int SDL_WiiExt_JoystickType(int type)
{
    SDL_WiiExtCaps caps;

    if (SDL_WiiExt_GetCaps(type, &caps)) {
        return caps.joystick_type;
    }
    return SDL_WII_EXT_JOYSTICK_TYPE_GAMEPAD;
}

void SDL_WiiExt_Reset(SDL_WiiExtState *state, int type)
{
    int i;

    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->type = type;
    state->enabled = true;
    /* The Classic Controller's left-stick seeds, for the 6-bit sticks */
    for (i = 0; i < 2; ++i) {
        state->stick[i].min = 9;
        state->stick[i].max = 54;
        state->stick[i].center = 0;
        state->stick[i].deadzone = 4;
    }
    for (i = 0; i < 6; ++i) {
        state->pad_axis[i] = WII_EXT_AXIS_MIN;
    }
    state->hihat_axis = WII_EXT_AXIS_MIN;
}

/* The driver's PostStickCalibrated arithmetic: the first sample sets the
   center and posts nothing, the range widens to every sample seen, and Y
   inverts. */
static bool CalibrateStick(SDL_WiiExtStick *cal, uint16_t data, bool is_y, int16_t *out)
{
    int16_t value = 0;

    if (!cal->center) {
        cal->center = data;
        return false;
    }
    if (data < cal->min) {
        cal->min = data;
    }
    if (data > cal->max) {
        cal->max = data;
    }
    if (data < cal->center - cal->deadzone) {
        uint16_t zero = (uint16_t)(cal->center - cal->deadzone);
        uint16_t range = (uint16_t)(zero - cal->min);
        uint16_t distance = (uint16_t)(zero - data);
        float fvalue = (float)distance / (float)range;
        value = (int16_t)(fvalue * WII_EXT_AXIS_MIN);
    } else if (data > cal->center + cal->deadzone) {
        uint16_t zero = (uint16_t)(cal->center + cal->deadzone);
        uint16_t range = (uint16_t)(cal->max - zero);
        uint16_t distance = (uint16_t)(data - zero);
        float fvalue = (float)distance / (float)range;
        value = (int16_t)(fvalue * WII_EXT_AXIS_MAX);
    }
    if (is_y && value) {
        value = (int16_t)~value;
    }
    *out = value;
    return true;
}

static void SetButton(SDL_WiiExtOutput *out, int button, bool down)
{
    out->button_mask |= (1u << button);
    if (down) {
        out->buttons |= (1u << button);
    }
}

static void SetAxis(SDL_WiiExtOutput *out, int axis, int value, bool data)
{
    if (value < WII_EXT_AXIS_MIN) {
        value = WII_EXT_AXIS_MIN;
    } else if (value > WII_EXT_AXIS_MAX) {
        value = WII_EXT_AXIS_MAX;
    }
    out->axes[axis] = (int16_t)value;
    out->axis_mask |= (uint16_t)(1u << axis);
    if (data) {
        out->data_axis_mask |= (uint16_t)(1u << axis);
    }
}

static void SetHat(SDL_WiiExtOutput *out, bool up, bool down, bool left, bool right)
{
    out->has_hat = true;
    out->hat = (uint8_t)((up ? SDL_WII_EXT_HAT_UP : 0) | (down ? SDL_WII_EXT_HAT_DOWN : 0) |
                         (left ? SDL_WII_EXT_HAT_LEFT : 0) | (right ? SDL_WII_EXT_HAT_RIGHT : 0));
}

static void DecodeStick(SDL_WiiExtState *state, uint8_t x, uint8_t y, SDL_WiiExtOutput *out)
{
    int16_t value;

    if (CalibrateStick(&state->stick[0], (uint16_t)(x & 0x3F), false, &value)) {
        SetAxis(out, SDL_WII_EXT_AXIS_LEFTX, value, false);
    }
    if (CalibrateStick(&state->stick[1], (uint16_t)(y & 0x3F), true, &value)) {
        SetAxis(out, SDL_WII_EXT_AXIS_LEFTY, value, false);
    }
}

/* The touch bar code as the byte a PS3 Guitar Hero guitar sends for the same
   fret state. Bands from WiitarThing, PS3 bytes from PlasticBand. */
static uint8_t GuitarSliderByte(uint8_t code)
{
    if (code >= 0x04 && code <= 0x05) {
        return 0x15;
    }
    if (code >= 0x06 && code <= 0x09) {
        return 0x30;
    }
    if (code >= 0x0A && code <= 0x0C) {
        return 0x4D;
    }
    if (code == 0x0D || code == 0x0E || code == 0x10 || code == 0x11) {
        return 0x66;
    }
    if (code >= 0x12 && code <= 0x13) {
        return 0x9A;
    }
    if (code >= 0x14 && code <= 0x16) {
        return 0xAF;
    }
    if (code >= 0x17 && code <= 0x19) {
        return 0xC9;
    }
    if (code >= 0x1A && code <= 0x1E) {
        return 0xE6;
    }
    if (code == 0x1F) {
        return 0xFF;
    }
    return 0x80; /* 0x0F untouched, and 0x00-0x03, which no source measured */
}

static void DecodeGuitar(SDL_WiiExtState *state, const uint8_t *e, SDL_WiiExtOutput *out)
{
    const bool gh3 = (e[0] & 0x80) != 0;
    const uint8_t touch = (uint8_t)(e[2] & 0x1F);
    const uint8_t whammy = (uint8_t)(e[3] & 0x1F);
    uint8_t slider, hi;
    int value;

    DecodeStick(state, e[0], e[1], out);

    /* Active low */
    SetButton(out, SDL_WII_EXT_BUTTON_SOUTH, !(e[5] & 0x10));          /* green */
    SetButton(out, SDL_WII_EXT_BUTTON_EAST, !(e[5] & 0x40));           /* red */
    SetButton(out, SDL_WII_EXT_BUTTON_WEST, !(e[5] & 0x08));           /* yellow */
    SetButton(out, SDL_WII_EXT_BUTTON_NORTH, !(e[5] & 0x20));          /* blue */
    SetButton(out, SDL_WII_EXT_BUTTON_LEFT_SHOULDER, !(e[5] & 0x80));  /* orange */
    SetButton(out, SDL_WII_EXT_BUTTON_RIGHT_SHOULDER, !(e[5] & 0x04)); /* pedal */
    SetButton(out, SDL_WII_EXT_BUTTON_BACK, !(e[4] & 0x10));           /* minus */
    SetButton(out, SDL_WII_EXT_BUTTON_START, !(e[4] & 0x04));          /* plus */
    SetHat(out, !(e[5] & 0x01), !(e[4] & 0x40), false, false);         /* strum up and down */

    /* Whammy: 0 at rest, full scale at full travel */
    if (!state->whammy_seen) {
        state->whammy_seen = true;
        state->whammy_lo = whammy;
        state->whammy_largest = whammy;
    }
    if (whammy < state->whammy_lo) {
        state->whammy_lo = whammy;
    }
    if (whammy > state->whammy_largest) {
        state->whammy_largest = whammy;
    }
    hi = (uint8_t)(state->whammy_lo + 0x0B);
    if (state->whammy_largest > hi) {
        hi = state->whammy_largest;
    }
    value = ((int)whammy - state->whammy_lo) * WII_EXT_AXIS_MAX / ((int)hi - state->whammy_lo);
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHTX, value, false);

    /* Touch bar, as the PS3 guitar path scales its slider byte. A Guitar Hero
       III guitar has none, and one that has only ever read 0x1F may not either. */
    if (touch != 0x1F) {
        state->touch_seen = true;
    }
    slider = (gh3 || !state->touch_seen) ? 0x80 : GuitarSliderByte(touch);
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHTY, slider * 257 - 32768, false);

    SetAxis(out, SDL_WII_EXT_AXIS_LEFT_TRIGGER, WII_EXT_AXIS_MIN, false);
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHT_TRIGGER, WII_EXT_AXIS_MIN, false);
}

static void DecodeDrums(SDL_WiiExtState *state, const uint8_t *e, SDL_WiiExtOutput *out)
{
    /* Green, red, blue, yellow, bass, orange: MIDI notes, E5 bits, buttons */
    static const uint8_t notes[6] = { 45, 38, 48, 46, 36, 49 };
    static const uint8_t bits[6] = { 0x10, 0x40, 0x08, 0x20, 0x04, 0x80 };
    static const int buttons[6] = {
        SDL_WII_EXT_BUTTON_SOUTH, SDL_WII_EXT_BUTTON_EAST, SDL_WII_EXT_BUTTON_WEST,
        SDL_WII_EXT_BUTTON_NORTH, SDL_WII_EXT_BUTTON_LEFT_SHOULDER, SDL_WII_EXT_BUTTON_RIGHT_SHOULDER
    };
    const bool message = !(e[2] == 0xFF && e[3] == 0xFF);
    uint8_t note = 0, velocity = 0;
    int i;

    DecodeStick(state, e[0], e[1], out);

    if (message) {
        /* Every field is inverted. The 7-bit velocity is spread over E2 to E4. */
        const uint8_t field = (uint8_t)((((e[3] >> 5) & 0x07) << 4) | ((e[2] & 0x01) << 3) |
                                        ((e[3] & 0x01) << 2) | (((e[4] >> 7) & 0x01) << 1) | (e[4] & 0x01));
        note = (uint8_t)((e[2] >> 1) ^ 0x7F);
        velocity = (uint8_t)(field ^ 0x7F);
    }

    for (i = 0; i < 6; ++i) {
        const bool hit = message && note == notes[i];
        const bool down = !(e[5] & bits[i]) || hit;

        if (hit) {
            state->pad_axis[i] = (int16_t)(velocity * 257 - 32768);
        } else if (!down) {
            state->pad_axis[i] = WII_EXT_AXIS_MIN;
        }
        SetButton(out, buttons[i], down);
        SetAxis(out, 6 + i, state->pad_axis[i], false);
    }
    if (message && note == 100) {
        /* The hi-hat pedal: the latest value holds */
        state->hihat_axis = (int16_t)(velocity * 257 - 32768);
    }
    SetAxis(out, 12, state->hihat_axis, false);

    SetButton(out, SDL_WII_EXT_BUTTON_BACK, !(e[4] & 0x10));  /* minus */
    SetButton(out, SDL_WII_EXT_BUTTON_START, !(e[4] & 0x04)); /* plus */
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHTX, 0, false);
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHTY, 0, false);
    SetAxis(out, SDL_WII_EXT_AXIS_LEFT_TRIGGER, WII_EXT_AXIS_MIN, false);
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHT_TRIGGER, WII_EXT_AXIS_MIN, false);
}

/* A 6-bit two's-complement platter rate */
static int PlatterRate(unsigned int bits)
{
    bits &= 0x3F;
    return (bits & 0x20) ? (int)bits - 64 : (int)bits;
}

static void DecodeTurntable(SDL_WiiExtState *state, const uint8_t *e, SDL_WiiExtOutput *out)
{
    const unsigned int right = ((e[2] & 0x01) << 5) | (((e[0] >> 6) & 0x03) << 3) |
                               (((e[1] >> 6) & 0x03) << 1) | ((e[2] >> 7) & 0x01);
    const unsigned int left = ((e[4] & 0x01) << 5) | (e[3] & 0x1F);
    const unsigned int crossfader = (e[2] >> 1) & 0x0F;
    const unsigned int dial = (((e[2] >> 5) & 0x03) << 3) | ((e[3] >> 5) & 0x07);
    /* Active low */
    const bool left_green = !(e[5] & 0x08);
    const bool left_red = !(e[4] & 0x20);
    const bool left_blue = !(e[5] & 0x80);
    const bool right_green = !(e[5] & 0x20);
    const bool right_red = !(e[4] & 0x02);
    const bool right_blue = !(e[5] & 0x04);

    DecodeStick(state, e[0], e[1], out);

    SetButton(out, SDL_WII_EXT_BUTTON_SOUTH, left_green || right_green);
    SetButton(out, SDL_WII_EXT_BUTTON_EAST, left_red || right_red);
    SetButton(out, SDL_WII_EXT_BUTTON_WEST, left_blue || right_blue);
    SetButton(out, SDL_WII_EXT_BUTTON_NORTH, !(e[5] & 0x10)); /* Euphoria */
    SetButton(out, SDL_WII_EXT_BUTTON_BACK, !(e[4] & 0x10));
    SetButton(out, SDL_WII_EXT_BUTTON_START, !(e[4] & 0x04));
    SetButton(out, SDL_WII_EXT_BUTTON_TURNTABLE_LEFT_GREEN + 0, left_green);
    SetButton(out, SDL_WII_EXT_BUTTON_TURNTABLE_LEFT_GREEN + 1, left_red);
    SetButton(out, SDL_WII_EXT_BUTTON_TURNTABLE_LEFT_GREEN + 2, left_blue);
    SetButton(out, SDL_WII_EXT_BUTTON_TURNTABLE_LEFT_GREEN + 3, right_green);
    SetButton(out, SDL_WII_EXT_BUTTON_TURNTABLE_LEFT_GREEN + 4, right_red);
    SetButton(out, SDL_WII_EXT_BUTTON_TURNTABLE_LEFT_GREEN + 5, right_blue);

    SetAxis(out, SDL_WII_EXT_AXIS_RIGHTX, PlatterRate(left) * 1024, false);
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHTY, PlatterRate(right) * 1024, false);
    SetAxis(out, SDL_WII_EXT_AXIS_LEFT_TRIGGER, WII_EXT_AXIS_MIN, false);
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHT_TRIGGER, WII_EXT_AXIS_MIN, false);
    SetAxis(out, 6, (int)crossfader * 4369 - 32768, false);
    SetAxis(out, 7, (int)(dial * 65535 / 31) - 32768, false);
}

static void DecodeTaiko(const uint8_t *e, SDL_WiiExtOutput *out)
{
    /* A strike clears its bit */
    SetButton(out, SDL_WII_EXT_BUTTON_LEFT_STICK, !(e[5] & 0x40));  /* left face */
    SetButton(out, SDL_WII_EXT_BUTTON_RIGHT_STICK, !(e[5] & 0x10)); /* right face */
    SetAxis(out, SDL_WII_EXT_AXIS_LEFTX, 0, false);
    SetAxis(out, SDL_WII_EXT_AXIS_LEFTY, 0, false);
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHTX, 0, false);
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHTY, 0, false);
    SetAxis(out, SDL_WII_EXT_AXIS_LEFT_TRIGGER, (e[5] & 0x20) ? WII_EXT_AXIS_MIN : WII_EXT_AXIS_MAX, false);  /* left rim */
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHT_TRIGGER, (e[5] & 0x08) ? WII_EXT_AXIS_MIN : WII_EXT_AXIS_MAX, false); /* right rim */
}

static void DecodeUDraw(const uint8_t *e, SDL_WiiExtOutput *out)
{
    const int x = e[0] | ((e[2] & 0x0F) << 8);
    const int y = e[1] | ((e[2] >> 4) << 8);
    const int pressure = e[3] | (((e[5] >> 2) & 0x01) << 8);
    const bool in_range = (x != 0xFFF);

    SetAxis(out, 0, in_range ? x : -1, true);
    SetAxis(out, 1, in_range ? y : -1, true);
    SetAxis(out, 2, pressure, true);
    SetButton(out, 0, !(e[5] & 0x02)); /* lower pen button */
    SetButton(out, 1, !(e[5] & 0x01)); /* upper pen button */
    SetButton(out, 2, in_range);
}

static void DecodeDrawsome(const uint8_t *e, SDL_WiiExtOutput *out)
{
    const int x = e[0] | (e[1] << 8);
    const int y = e[2] | (e[3] << 8);
    const int pressure = e[4] | ((e[5] & 0x0F) << 8);
    const bool in_range = !(e[5] & 0x80);

    SetAxis(out, 0, in_range ? x : -1, true);
    SetAxis(out, 1, in_range ? y : -1, true);
    SetAxis(out, 2, pressure, true);
    SetButton(out, 0, false);
    SetButton(out, 1, false);
    SetButton(out, 2, in_range);
}

/* The nearest value's position in a list, ties to the lower position */
static int NearestNotch(uint8_t value, const uint8_t *notches, int count)
{
    int best = 0, best_distance = 256, i;

    for (i = 0; i < count; ++i) {
        const int distance = (value > notches[i]) ? value - notches[i] : notches[i] - value;
        if (distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

static void DecodeShinkansen(const uint8_t *e, SDL_WiiExtOutput *out)
{
    /* The lever values the game accepts, released first (Dolphin) */
    static const uint8_t brake[9] = { 0, 53, 79, 105, 132, 159, 187, 217, 250 };
    static const uint8_t power[14] = { 255, 229, 208, 189, 170, 153, 135, 118, 101, 85, 68, 51, 35, 17 };
    const int brake_notch = NearestNotch(e[2], brake, 9);
    const int power_notch = NearestNotch(e[3], power, 14);

    /* Classic Controller format 03 button bits, active low */
    SetButton(out, SDL_WII_EXT_BUTTON_SOUTH, !(e[7] & 0x40));          /* BB */
    SetButton(out, SDL_WII_EXT_BUTTON_EAST, !(e[7] & 0x10));           /* BA */
    SetButton(out, SDL_WII_EXT_BUTTON_WEST, !(e[7] & 0x20));           /* BY */
    SetButton(out, SDL_WII_EXT_BUTTON_NORTH, !(e[7] & 0x08));          /* BX */
    SetButton(out, SDL_WII_EXT_BUTTON_RIGHT_SHOULDER, !(e[7] & 0x04)); /* BZR */
    SetButton(out, SDL_WII_EXT_BUTTON_START, !(e[6] & 0x04));          /* B+ */
    SetButton(out, SDL_WII_EXT_BUTTON_BACK, !(e[6] & 0x10));           /* B- */
    SetHat(out, !(e[7] & 0x01), !(e[6] & 0x40), !(e[7] & 0x02), !(e[6] & 0x80));

    SetAxis(out, SDL_WII_EXT_AXIS_LEFTX, 0, false);
    SetAxis(out, SDL_WII_EXT_AXIS_LEFTY, 0, false);
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHTX, 0, false);
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHTY, 0, false);
    SetAxis(out, SDL_WII_EXT_AXIS_LEFT_TRIGGER, -32768 + (brake_notch * 65535 + 4) / 8, false);
    SetAxis(out, SDL_WII_EXT_AXIS_RIGHT_TRIGGER, -32768 + (power_notch * 65535 + 6) / 13, false);
    SetAxis(out, 6, e[2] * 257 - 32768, true);
    SetAxis(out, 7, e[3] * 257 - 32768, true);
}

bool SDL_WiiExt_Decode(SDL_WiiExtState *state, const uint8_t *span, size_t length, SDL_WiiExtOutput *out)
{
    const size_t needed = (state && state->type == SDL_WII_EXT_SHINKANSEN) ? 8 : 6;
    bool all_ff = true;
    size_t i;

    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!state || !span || !out || !state->enabled || !SDL_WiiExt_IsDecodedType(state->type) || length < needed) {
        return false;
    }
    for (i = 0; i < length; ++i) {
        if (span[i] != 0xFF) {
            all_ff = false;
            break;
        }
    }
    /* A failed bus read. The TaTaCon's released byte is FF and nothing
       records its other bytes, so there it reads as all four released. */
    if (all_ff && state->type != SDL_WII_EXT_TAIKO) {
        return false;
    }

    switch (state->type) {
    case SDL_WII_EXT_GUITAR:
        DecodeGuitar(state, span, out);
        break;
    case SDL_WII_EXT_DRUMS:
        DecodeDrums(state, span, out);
        break;
    case SDL_WII_EXT_TURNTABLE:
        DecodeTurntable(state, span, out);
        break;
    case SDL_WII_EXT_TAIKO:
        DecodeTaiko(span, out);
        break;
    case SDL_WII_EXT_UDRAW:
        DecodeUDraw(span, out);
        break;
    case SDL_WII_EXT_DRAWSOME:
        DecodeDrawsome(span, out);
        break;
    case SDL_WII_EXT_SHINKANSEN:
        DecodeShinkansen(span, out);
        break;
    default:
        return false;
    }
    return true;
}
