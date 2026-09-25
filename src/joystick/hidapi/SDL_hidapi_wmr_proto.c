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

#include "SDL_hidapi_wmr_proto.h"

#include <string.h>

#define WMR_VENDOR_MICROSOFT         0x045E
#define WMR_PRODUCT_FIRST_GENERATION 0x065B
#define WMR_PRODUCT_ODYSSEY          0x065D
#define WMR_PRODUCT_REVERB_G2        0x066A

#define WMR_COMMAND_RESET   0x00
#define WMR_COMMAND_READ    0x02
#define WMR_COMMAND_ENABLE  0x03
#define WMR_COMMAND_QUIESCE 0x04
#define WMR_ANSWER_COMMAND  0x06 /* The answer to the reset and the quiesce */
#define WMR_ANSWER_BLOCK    0x02 /* The answer to a block read */

/* The answer: code, 0, command echo, 0, block echo, bytes remaining (LE32),
   data length, 68 data bytes */
#define WMR_ANSWER_ECHO   2
#define WMR_ANSWER_REMAIN 5
#define WMR_ANSWER_LENGTH 9
#define WMR_ANSWER_DATA   10

bool SDL_WMR_IsControllerID(uint16_t vendor, uint16_t product, SDL_WMRModel *model)
{
    SDL_WMRModel found;

    if (vendor != WMR_VENDOR_MICROSOFT) {
        return false;
    }
    switch (product) {
    case WMR_PRODUCT_FIRST_GENERATION:
    case WMR_PRODUCT_ODYSSEY:
        found = SDL_WMR_MODEL_FIRST_GENERATION;
        break;
    case WMR_PRODUCT_REVERB_G2:
        found = SDL_WMR_MODEL_REVERB_G2;
        break;
    default:
        return false;
    }
    if (model) {
        *model = found;
    }
    return true;
}

SDL_WMRHand SDL_WMR_GetHand(const char *product)
{
    if (product && strcmp(product, SDL_WMR_LEFT_PRODUCT) == 0) {
        return SDL_WMR_HAND_LEFT;
    }
    if (product && strcmp(product, SDL_WMR_RIGHT_PRODUCT) == 0) {
        return SDL_WMR_HAND_RIGHT;
    }
    return SDL_WMR_HAND_NONE;
}

static uint32_t WMR_Read32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int32_t WMR_Read24(const uint8_t *data)
{
    const int32_t value = (int32_t)((uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16));

    return (value & 0x800000) ? value - 0x1000000 : value;
}

bool SDL_WMR_DecodeStatus(const uint8_t *report, size_t length, bool padded, SDL_WMRModel model, SDL_WMRInput *out)
{
    const uint8_t *data;
    int i;

    if (!report || !out || (padded ? length < SDL_WMR_STATUS_LENGTH : length != SDL_WMR_STATUS_LENGTH) ||
        report[0] != SDL_WMR_STATUS_REPORT_ID) {
        return false;
    }
    data = report + 1;
    memset(out, 0, sizeof(*out));
    out->buttons = data[0];
    out->stick_x = (uint16_t)(data[1] | ((data[2] & 0x0F) << 8));
    out->stick_y = (uint16_t)((data[2] >> 4) | (data[3] << 4));
    out->trigger = data[4];
    if (model == SDL_WMR_MODEL_REVERB_G2) {
        out->grip = data[5];
        out->face_buttons = data[6];
    } else {
        out->touchpad_x = data[5];
        out->touchpad_y = data[6];
    }
    out->battery = data[7];
    for (i = 0; i < 3; ++i) {
        out->accel[i] = WMR_Read24(&data[8 + 3 * i]);
        out->gyro[i] = WMR_Read24(&data[19 + 3 * i]);
    }
    out->temperature = (int16_t)(uint16_t)(data[17] | (data[18] << 8));
    out->timestamp = WMR_Read32(&data[28]);
    return true;
}

int16_t SDL_WMR_StickAxis(uint16_t value, bool invert)
{
    /* Monado centers the 12 bits on 0x7FF, leaving 2047 steps below and 2048 above */
    const int32_t offset = (int32_t)(value & 0xFFF) - 0x7FF;

    if (invert) {
        return (int16_t)(offset > 0 ? -(offset * 32768 / 2048) : -offset * 32767 / 2047);
    }
    return (int16_t)(offset < 0 ? offset * 32768 / 2047 : offset * 32767 / 2048);
}

int16_t SDL_WMR_TouchpadAxis(uint8_t value, bool invert)
{
    int32_t offset;

    if (value == 0xFF) {
        return 0;
    }
    offset = (int32_t)(value > 0x64 ? 0x64 : value) - 0x32;
    if (invert) {
        return (int16_t)(offset > 0 ? -(offset * 32768 / 0x32) : -offset * 32767 / 0x32);
    }
    return (int16_t)(offset < 0 ? offset * 32768 / 0x32 : offset * 32767 / 0x32);
}

int SDL_WMR_GetNumAxes(SDL_WMRModel model)
{
    return model == SDL_WMR_MODEL_REVERB_G2 ? 4 : 5;
}

void SDL_WMR_GetState(SDL_WMRModel model, const SDL_WMRInput *input, SDL_WMRState *state)
{
    memset(state, 0, sizeof(*state));
    state->num_axes = SDL_WMR_GetNumAxes(model);
    state->axes[0] = SDL_WMR_StickAxis(input->stick_x, false);
    state->axes[1] = SDL_WMR_StickAxis(input->stick_y, true);
    state->axes[2] = (int16_t)(input->trigger * 257 - 32768);
    state->buttons[0] = (input->buttons & 0x01) != 0; /* Stick press */
    state->buttons[1] = (input->buttons & 0x02) != 0; /* Windows */
    state->buttons[2] = (input->buttons & 0x04) != 0; /* Menu */
    state->buttons[3] = (input->buttons & 0x08) != 0; /* Grip */
    if (model == SDL_WMR_MODEL_REVERB_G2) {
        state->axes[3] = (int16_t)(input->grip * 257 - 32768);
        state->buttons[4] = (input->face_buttons & 0x02) != 0; /* X or A */
        state->buttons[5] = (input->face_buttons & 0x01) != 0; /* Y or B */
    } else {
        state->axes[3] = SDL_WMR_TouchpadAxis(input->touchpad_x, false);
        state->axes[4] = SDL_WMR_TouchpadAxis(input->touchpad_y, true);
        state->buttons[4] = (input->buttons & 0x10) != 0; /* Touchpad press */
        state->buttons[5] = (input->buttons & 0x40) != 0; /* Touchpad touch */
    }
    /* 0x20, the pairing button, maps to nothing */
}

void SDL_WMR_InitCalibration(SDL_WMRCalibration *calibration)
{
    int i;

    memset(calibration, 0, sizeof(*calibration));
    for (i = 0; i < 3; ++i) {
        calibration->mix[i][i] = 1.0f;
        calibration->rotation[i][i] = 1.0f;
    }
}

void SDL_WMR_Calibrate(const SDL_WMRCalibration *calibration, const float value[3], float out[3])
{
    float mixed[3], turned[3];
    int i;

    for (i = 0; i < 3; ++i) {
        mixed[i] = calibration->mix[i][0] * value[0] + calibration->mix[i][1] * value[1] +
                   calibration->mix[i][2] * value[2] + calibration->bias[i];
    }
    for (i = 0; i < 3; ++i) {
        turned[i] = calibration->rotation[0][i] * mixed[0] + calibration->rotation[1][i] * mixed[1] +
                    calibration->rotation[2][i] * mixed[2];
    }
    /* WMR's Y down and Z forward become Y up and Z backward */
    out[0] = turned[0];
    out[1] = -turned[1];
    out[2] = -turned[2];
}

void SDL_WMR_AccelFromRaw(const int32_t raw[3], float out[3])
{
    int i;

    for (i = 0; i < 3; ++i) {
        out[i] = (float)raw[i] / 49000.0f;
    }
}

void SDL_WMR_GyroFromRaw(const int32_t raw[3], float out[3])
{
    int i;

    for (i = 0; i < 3; ++i) {
        out[i] = (float)raw[i] * 0.00001f;
    }
}

void SDL_WMR_Deobfuscate(uint8_t *data, size_t length)
{
    size_t i;

    for (i = 0; i < length; ++i) {
        data[i] ^= SDL_WMR_ConfigKey[i % SDL_WMR_KEY_LENGTH];
    }
}

uint64_t SDL_WMR_ExtendTimestamp(uint64_t *ticks, uint32_t timestamp)
{
    const uint32_t previous = (uint32_t)(*ticks & 0xFFFFFFFFu);

    *ticks = (*ticks & ~(uint64_t)0xFFFFFFFFu) | timestamp;
    if (timestamp < previous) {
        *ticks += (uint64_t)1 << 32;
    }
    return *ticks;
}

/* Configuration JSON. cJSON 1.7.18, which Monado builds, decides what is
 * valid: every byte up to 32 is white space, a number is the longest prefix
 * strtod takes of up to 63 characters of 0-9, +, -, e, E and the point, an
 * invalid \u escape is U+0000, text after the root value is ignored and
 * containers nest at most 1000 deep. Names compare as C strings. */

#define WMR_JSON_MAX_DEPTH  1000
#define WMR_JSON_MAX_NUMBER 63
#define WMR_JSON_MAX_NAME   64

static bool WMR_IsDigit(char c)
{
    return c >= '0' && c <= '9';
}

static const char *WMR_JsonSpace(const char *p, const char *end)
{
    while (p < end && (unsigned char)*p <= 32) {
        ++p;
    }
    return p;
}

static unsigned WMR_JsonHex4(const char *p)
{
    unsigned value = 0;
    int i;

    for (i = 0; i < 4; ++i) {
        const char c = p[i];
        unsigned digit;

        if (c >= '0' && c <= '9') {
            digit = (unsigned)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            digit = (unsigned)(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            digit = (unsigned)(c - 'a' + 10);
        } else {
            return 0;
        }
        value = (value << 4) | digit;
    }
    return value;
}

typedef struct WMR_JsonText
{
    char *out;    /* NULL to validate only */
    size_t size;
    size_t used;
    bool cut;     /* A NUL ended the C string */
    bool fits;
} WMR_JsonText;

static void WMR_JsonPut(WMR_JsonText *text, unsigned char c)
{
    if (!text || !text->out || text->cut) {
        return;
    }
    if (c == '\0') {
        text->cut = true;
    } else if (text->used + 1 < text->size) {
        text->out[text->used++] = (char)c;
    } else {
        text->fits = false;
    }
}

/* The string at p. Returns the position after its closing quote, or NULL. */
static const char *WMR_JsonString(const char *p, const char *end, WMR_JsonText *text)
{
    const char *close;

    if (p >= end || *p != '"') {
        return NULL;
    }
    for (close = p + 1; close < end && *close != '"'; ++close) {
        if (*close == '\\') {
            if (close + 1 >= end) {
                return NULL;
            }
            ++close;
        }
    }
    if (close >= end) {
        return NULL;
    }
    for (p = p + 1; p < close;) {
        unsigned long codepoint;
        unsigned char bytes[4];
        int count, i;

        if (*p != '\\') {
            WMR_JsonPut(text, (unsigned char)*p++);
            continue;
        }
        switch (p[1]) {
        case 'b':
            WMR_JsonPut(text, '\b');
            p += 2;
            continue;
        case 'f':
            WMR_JsonPut(text, '\f');
            p += 2;
            continue;
        case 'n':
            WMR_JsonPut(text, '\n');
            p += 2;
            continue;
        case 'r':
            WMR_JsonPut(text, '\r');
            p += 2;
            continue;
        case 't':
            WMR_JsonPut(text, '\t');
            p += 2;
            continue;
        case '"':
        case '\\':
        case '/':
            WMR_JsonPut(text, (unsigned char)p[1]);
            p += 2;
            continue;
        case 'u':
            break;
        default:
            return NULL;
        }
        if (close - p < 6) {
            return NULL;
        }
        codepoint = WMR_JsonHex4(p + 2);
        if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            return NULL;
        }
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            unsigned long second;

            if (close - (p + 6) < 6 || p[6] != '\\' || p[7] != 'u') {
                return NULL;
            }
            second = WMR_JsonHex4(p + 8);
            if (second < 0xDC00 || second > 0xDFFF) {
                return NULL;
            }
            codepoint = 0x10000 + (((codepoint & 0x3FF) << 10) | (second & 0x3FF));
            p += 12;
        } else {
            p += 6;
        }
        if (codepoint < 0x80) {
            bytes[0] = (unsigned char)codepoint;
            count = 1;
        } else if (codepoint < 0x800) {
            bytes[0] = (unsigned char)(0xC0 | (codepoint >> 6));
            count = 2;
        } else if (codepoint < 0x10000) {
            bytes[0] = (unsigned char)(0xE0 | (codepoint >> 12));
            count = 3;
        } else {
            bytes[0] = (unsigned char)(0xF0 | (codepoint >> 18));
            count = 4;
        }
        for (i = 1; i < count; ++i) {
            bytes[i] = (unsigned char)(0x80 | ((codepoint >> (6 * (count - 1 - i))) & 0x3F));
        }
        for (i = 0; i < count; ++i) {
            WMR_JsonPut(text, bytes[i]);
        }
    }
    return close + 1;
}

/* The number at p, as strtod reads the characters cJSON collects. The value
 * is exact for up to 19 significant digits within 1e-22 to 1e22. */
static const char *WMR_JsonNumber(const char *p, const char *end, double *value)
{
    static const double powers[] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
    };
    const char *start = p;
    bool negative = false, digits = false;
    uint64_t mantissa = 0;
    int exponent = 0;
    double result;

    if (p < end && *p == '-') {
        negative = true;
        ++p;
    }
    for (; p < end && WMR_IsDigit(*p); ++p) {
        digits = true;
        if (mantissa < UINT64_C(1000000000000000000)) {
            mantissa = mantissa * 10 + (uint64_t)(*p - '0');
        } else {
            ++exponent;
        }
    }
    if (p < end && *p == '.') {
        for (++p; p < end && WMR_IsDigit(*p); ++p) {
            digits = true;
            if (mantissa < UINT64_C(1000000000000000000)) {
                mantissa = mantissa * 10 + (uint64_t)(*p - '0');
                --exponent;
            }
        }
    }
    if (!digits) {
        return NULL;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;
        bool exponent_negative = false;
        int written = 0;

        if (q < end && (*q == '+' || *q == '-')) {
            exponent_negative = (*q == '-');
            ++q;
        }
        if (q < end && WMR_IsDigit(*q)) {
            for (; q < end && WMR_IsDigit(*q); ++q) {
                if (written < 100000) {
                    written = written * 10 + (*q - '0');
                }
            }
            exponent += exponent_negative ? -written : written;
            p = q;
        }
    }
    if (p - start > WMR_JSON_MAX_NUMBER) {
        return NULL;
    }
    result = (double)mantissa;
    while (exponent > 22) {
        result *= 1e22;
        exponent -= 22;
    }
    while (exponent < -22) {
        result /= 1e22;
        exponent += 22;
    }
    result = exponent >= 0 ? result * powers[exponent] : result / powers[-exponent];
    *value = negative ? -result : result;
    return p;
}

static const char *WMR_JsonLiteral(const char *p, const char *end)
{
    if (end - p >= 4 && memcmp(p, "null", 4) == 0) {
        return p + 4;
    }
    if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
        return p + 5;
    }
    if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
        return p + 4;
    }
    return NULL;
}

/* A member name and its colon. Returns the start of the member's value. */
static const char *WMR_JsonName(const char *p, const char *end, WMR_JsonText *text)
{
    p = WMR_JsonString(WMR_JsonSpace(p, end), end, text);
    if (!p) {
        return NULL;
    }
    p = WMR_JsonSpace(p, end);
    if (p >= end || *p != ':') {
        return NULL;
    }
    return WMR_JsonSpace(p + 1, end);
}

/* Returns the position after the value at p, or NULL where cJSON fails */
static const char *WMR_JsonSkip(const char *p, const char *end)
{
    uint8_t objects[(WMR_JSON_MAX_DEPTH + 7) / 8] = { 0 }; /* A bit per open container, set for an object */
    int depth = 0;

    for (;;) {
        p = WMR_JsonSpace(p, end);
        if (p >= end) {
            return NULL;
        }
        if (*p == '{' || *p == '[') {
            const bool object = (*p == '{');

            if (depth >= WMR_JSON_MAX_DEPTH) {
                return NULL;
            }
            if (object) {
                objects[depth / 8] = (uint8_t)(objects[depth / 8] | (1 << (depth % 8)));
            } else {
                objects[depth / 8] = (uint8_t)(objects[depth / 8] & ~(1 << (depth % 8)));
            }
            ++depth;
            p = WMR_JsonSpace(p + 1, end);
            if (p < end && *p == (object ? '}' : ']')) {
                ++p;
                --depth;
            } else if (object) {
                p = WMR_JsonName(p, end, NULL);
                if (!p) {
                    return NULL;
                }
                continue;
            } else {
                continue;
            }
        } else if (*p == '"') {
            p = WMR_JsonString(p, end, NULL);
        } else if (*p == '-' || WMR_IsDigit(*p)) {
            double unused;

            p = WMR_JsonNumber(p, end, &unused);
        } else {
            p = WMR_JsonLiteral(p, end);
        }
        if (!p) {
            return NULL;
        }

        /* After a value: close containers until one continues */
        for (;;) {
            bool object;

            if (depth == 0) {
                return p;
            }
            object = ((objects[(depth - 1) / 8] >> ((depth - 1) % 8)) & 1) != 0;
            p = WMR_JsonSpace(p, end);
            if (p < end && *p == ',') {
                ++p;
                if (object) {
                    p = WMR_JsonName(p, end, NULL);
                    if (!p) {
                        return NULL;
                    }
                }
                break;
            }
            if (p < end && *p == (object ? '}' : ']')) {
                ++p;
                --depth;
                continue;
            }
            return NULL;
        }
    }
}

static char WMR_Lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool WMR_JsonNameIs(const char *name, const char *key, bool case_sensitive)
{
    if (case_sensitive) {
        return strcmp(name, key) == 0;
    }
    for (;; ++name, ++key) {
        if (WMR_Lower(*name) != WMR_Lower(*key)) {
            return false;
        }
        if (*name == '\0') {
            return true;
        }
    }
}

/* The value of the first member named key in the object at p, or NULL,
 * as cJSON_GetObjectItemCaseSensitive or cJSON_GetObjectItem finds it.
 * Only for JSON that WMR_JsonSkip took. */
static const char *WMR_JsonFind(const char *p, const char *end, const char *key, bool case_sensitive)
{
    if (!p || p >= end || *p != '{') {
        return NULL;
    }
    p = WMR_JsonSpace(p + 1, end);
    if (p >= end || *p == '}') {
        return NULL;
    }
    for (;;) {
        char name[WMR_JSON_MAX_NAME];
        WMR_JsonText text;
        const char *value;

        text.out = name;
        text.size = sizeof(name);
        text.used = 0;
        text.cut = false;
        text.fits = true;
        value = WMR_JsonName(p, end, &text);
        if (!value) {
            return NULL;
        }
        name[text.used] = '\0';
        if (text.fits && WMR_JsonNameIs(name, key, case_sensitive)) {
            return value;
        }
        p = WMR_JsonSkip(value, end);
        if (!p) {
            return NULL;
        }
        p = WMR_JsonSpace(p, end);
        if (p >= end || *p != ',') {
            return NULL;
        }
        ++p;
    }
}

/* The first element of the array at p, or NULL when it is empty */
static const char *WMR_JsonFirst(const char *p, const char *end)
{
    if (!p || p >= end || *p != '[') {
        return NULL;
    }
    p = WMR_JsonSpace(p + 1, end);
    return (p < end && *p != ']') ? p : NULL;
}

/* The element after the one at p, or NULL after the last */
static const char *WMR_JsonNext(const char *p, const char *end)
{
    p = WMR_JsonSkip(p, end);
    if (!p) {
        return NULL;
    }
    p = WMR_JsonSpace(p, end);
    if (p >= end || *p != ',') {
        return NULL;
    }
    return WMR_JsonSpace(p + 1, end);
}

static bool WMR_JsonIsArray(const char *p, const char *end)
{
    return p && p < end && *p == '[';
}

static size_t WMR_JsonCount(const char *p, const char *end)
{
    size_t count = 0;
    const char *element;

    for (element = WMR_JsonFirst(p, end); element; element = WMR_JsonNext(element, end)) {
        ++count;
    }
    return count;
}

/* Monado's u_json_get_float_array: the leading numbers, up to max */
static size_t WMR_JsonFloats(const char *p, const char *end, float *out, size_t max)
{
    size_t count = 0;
    const char *element;

    for (element = WMR_JsonFirst(p, end); element && count < max; element = WMR_JsonNext(element, end)) {
        double value;

        /* A string, literal or container ends the run */
        if (!WMR_JsonNumber(element, end, &value)) {
            break;
        }
        out[count++] = (float)value;
    }
    return count;
}

/* Monado's wmr_inertial_sensor_config_parse: each part is kept once read,
   and the first part missing ends the entry */
static void WMR_ParseSensor(const char *sensor, const char *end, SDL_WMRCalibration *accel,
                            SDL_WMRCalibration *gyro, SDL_WMRCalibration *magnetometer)
{
    char type[WMR_JSON_MAX_NAME];
    WMR_JsonText text;
    SDL_WMRCalibration *target;
    const char *value, *rt, *rotation, *translation, *mix, *bias, *uncertainty, *noise;
    float values[36];
    int r, c;

    value = WMR_JsonFind(sensor, end, "SensorType", false);
    if (!value || *value != '"') {
        return;
    }
    text.out = type;
    text.size = sizeof(type);
    text.used = 0;
    text.cut = false;
    text.fits = true;
    if (!WMR_JsonString(value, end, &text) || !text.fits) {
        return;
    }
    type[text.used] = '\0';
    if (strcmp(type, "CALIBRATION_InertialSensorType_Gyro") == 0) {
        target = gyro;
    } else if (strcmp(type, "CALIBRATION_InertialSensorType_Accelerometer") == 0) {
        target = accel;
    } else if (strcmp(type, "CALIBRATION_InertialSensorType_Magnetometer") == 0) {
        target = magnetometer;
    } else {
        return;
    }

    rt = WMR_JsonFind(sensor, end, "Rt", false);
    rotation = WMR_JsonFind(rt, end, "Rotation", false);
    if (!rt || !rotation) {
        return;
    }
    translation = WMR_JsonFind(rt, end, "Translation", true);
    if (!WMR_JsonIsArray(translation, end) || WMR_JsonCount(translation, end) != 3 ||
        WMR_JsonFloats(rotation, end, values, 9) != 9) {
        return;
    }
    for (r = 0; r < 3; ++r) {
        for (c = 0; c < 3; ++c) {
            target->rotation[r][c] = values[3 * r + c];
        }
    }

    mix = WMR_JsonFind(sensor, end, "MixingMatrixTemperatureModel", false);
    bias = WMR_JsonFind(sensor, end, "BiasTemperatureModel", false);
    uncertainty = WMR_JsonFind(sensor, end, "BiasUncertainty", false);
    noise = WMR_JsonFind(sensor, end, "Noise", false);
    if (!mix || !bias || !noise || !uncertainty) {
        return;
    }
    /* The constant terms of the temperature models */
    if (WMR_JsonFloats(mix, end, values, 36) != 36) {
        return;
    }
    for (r = 0; r < 3; ++r) {
        for (c = 0; c < 3; ++c) {
            target->mix[r][c] = values[4 * (3 * r + c)];
        }
    }
    if (WMR_JsonFloats(bias, end, values, 12) != 12) {
        return;
    }
    target->bias[0] = values[0];
    target->bias[1] = values[4];
    target->bias[2] = values[8];
    /* Monado reads BiasUncertainty and Noise next, which set nothing used here */
}

bool SDL_WMR_ParseConfig(const char *text, size_t length, SDL_WMRCalibration *accel, SDL_WMRCalibration *gyro)
{
    SDL_WMRCalibration magnetometer;
    const char *nul, *end, *root, *calibration, *sensors, *sensor, *leds;

    SDL_WMR_InitCalibration(accel);
    SDL_WMR_InitCalibration(gyro);
    SDL_WMR_InitCalibration(&magnetometer);
    if (!text) {
        return false;
    }
    nul = (const char *)memchr(text, '\0', length);
    end = nul ? nul : text + length;
    root = text;
    /* cJSON skips a byte order mark with at least one byte after it */
    if (end - root >= 4 && memcmp(root, "\xEF\xBB\xBF", 3) == 0) {
        root += 3;
    }
    root = WMR_JsonSpace(root, end);
    if (!WMR_JsonSkip(root, end) || *root != '{') {
        return false;
    }
    calibration = WMR_JsonFind(root, end, "CalibrationInformation", true);
    if (!calibration || *calibration != '{') {
        return false;
    }
    sensors = WMR_JsonFind(calibration, end, "InertialSensors", true);
    if (!WMR_JsonIsArray(sensors, end)) {
        return false;
    }
    for (sensor = WMR_JsonFirst(sensors, end); sensor; sensor = WMR_JsonNext(sensor, end)) {
        WMR_ParseSensor(sensor, end, accel, gyro, &magnetometer);
    }
    /* Monado also reads the LED positions, which SDL does not use, and fails
       past its 40 entries */
    leds = WMR_JsonFind(calibration, end, "ControllerLeds", true);
    return WMR_JsonIsArray(leds, end);
}

/* The start-up */

static void WMR_Fail(SDL_WMRSession *session, SDL_WMRFailure failure)
{
    session->phase = SDL_WMR_PHASE_FAILED;
    session->failure = failure;
}

static void WMR_Request(SDL_WMRSession *session, uint64_t now_ms, uint8_t command, uint8_t block,
                        uint32_t address, uint8_t answer, const SDL_WMRSink *sink)
{
    memset(session->command, 0, sizeof(session->command));
    session->command[0] = SDL_WMR_COMMAND_PREFIX;
    session->command[1] = command;
    session->command[2] = block;
    session->command[3] = (uint8_t)(address & 0xFF);
    session->command[4] = (uint8_t)((address >> 8) & 0xFF);
    session->command[5] = (uint8_t)((address >> 16) & 0xFF);
    session->command[6] = (uint8_t)((address >> 24) & 0xFF);
    if (!sink->write(sink->userdata, session->command, sizeof(session->command))) {
        WMR_Fail(session, SDL_WMR_FAILURE_WRITE);
        return;
    }
    session->response_code = answer;
    session->phase = SDL_WMR_PHASE_WAITING;
    session->deadline_ms = now_ms + SDL_WMR_REPLY_TIMEOUT_MS;
}

static uint8_t WMR_Block(SDL_WMRStep step)
{
    switch (step) {
    case SDL_WMR_STEP_BLOCK0:
        return 0x00;
    case SDL_WMR_STEP_BLOCK3:
        return 0x03;
    default:
        return 0x02;
    }
}

static void WMR_ReadBlock(SDL_WMRSession *session, uint64_t now_ms, SDL_WMRStep step, const SDL_WMRSink *sink)
{
    session->step = step;
    session->block_size = 0;
    session->block_received = 0;
    WMR_Request(session, now_ms, WMR_COMMAND_READ, WMR_Block(step), 0xFFFFFFFFu, WMR_ANSWER_BLOCK, sink);
}

static void WMR_Enable(SDL_WMRSession *session, const SDL_WMRSink *sink)
{
    static const uint8_t status_on[] = { SDL_WMR_COMMAND_PREFIX, WMR_COMMAND_ENABLE, 0x01, 0x00, 0x02 };
    static const uint8_t imu_on[] = { SDL_WMR_COMMAND_PREFIX, WMR_COMMAND_ENABLE, 0x02, 0xE1, 0x02 };
    uint8_t command[SDL_WMR_COMMAND_LENGTH];

    /* Monado sends both without an answer and without checking the writes */
    memset(command, 0, sizeof(command));
    memcpy(command, status_on, sizeof(status_on));
    (void)sink->write(sink->userdata, command, sizeof(command));
    memset(command, 0, sizeof(command));
    memcpy(command, imu_on, sizeof(imu_on));
    (void)sink->write(sink->userdata, command, sizeof(command));
    session->step = SDL_WMR_STEP_DONE;
    session->phase = SDL_WMR_PHASE_READY;
}

static void WMR_Store(SDL_WMRSession *session, const uint8_t *data, uint32_t count)
{
    if (session->step == SDL_WMR_STEP_BLOCK2) {
        memcpy(session->config + session->block_received, data, count);
    } else if (session->block_received < sizeof(session->info)) {
        const uint32_t room = (uint32_t)sizeof(session->info) - session->block_received;

        memcpy(session->info + session->block_received, data, count < room ? count : room);
    }
    session->block_received += count;
}

static void WMR_FinishBlock(SDL_WMRSession *session, uint64_t now_ms, const SDL_WMRSink *sink)
{
    switch (session->step) {
    case SDL_WMR_STEP_BLOCK0:
        if (session->block_size < SDL_WMR_BLOCK0_MINIMUM) {
            WMR_Fail(session, SDL_WMR_FAILURE_BLOCK);
            return;
        }
        session->firmware_revision = WMR_Read32(&session->info[0x14]);
        session->calibration_size = (uint16_t)(session->info[0x34] | (session->info[0x35] << 8));
        WMR_ReadBlock(session, now_ms, SDL_WMR_STEP_BLOCK3, sink);
        break;
    case SDL_WMR_STEP_BLOCK3:
        if (session->block_size < SDL_WMR_BLOCK3_MINIMUM) {
            WMR_Fail(session, SDL_WMR_FAILURE_BLOCK);
            return;
        }
        memcpy(session->serial, &session->info[0x84], 16);
        session->serial[16] = '\0';
        WMR_ReadBlock(session, now_ms, SDL_WMR_STEP_BLOCK2, sink);
        break;
    case SDL_WMR_STEP_BLOCK2:
        if (session->block_size < 2) {
            WMR_Fail(session, SDL_WMR_FAILURE_BLOCK);
            return;
        }
        SDL_WMR_Deobfuscate(session->config + 2, session->block_size - 2);
        if (!SDL_WMR_ParseConfig((const char *)session->config + 2, session->block_size - 2,
                                 &session->accel, &session->gyro)) {
            WMR_Fail(session, SDL_WMR_FAILURE_CONFIG);
            return;
        }
        WMR_Enable(session, sink);
        break;
    default:
        break;
    }
}

static void WMR_HandleAnswer(SDL_WMRSession *session, uint64_t now_ms, const uint8_t *report, size_t length,
                             const SDL_WMRSink *sink)
{
    uint32_t count;

    if ((session->padded ? length < SDL_WMR_REPLY_LENGTH : length != SDL_WMR_REPLY_LENGTH) ||
        report[WMR_ANSWER_ECHO] != session->command[1]) {
        WMR_Fail(session, SDL_WMR_FAILURE_REPLY);
        return;
    }
    switch (session->step) {
    case SDL_WMR_STEP_RESET:
        session->step = SDL_WMR_STEP_QUIESCE;
        WMR_Request(session, now_ms, WMR_COMMAND_QUIESCE, 0xC1, 0x02, WMR_ANSWER_COMMAND, sink);
        return;
    case SDL_WMR_STEP_QUIESCE:
        WMR_ReadBlock(session, now_ms, SDL_WMR_STEP_BLOCK0, sink);
        return;
    case SDL_WMR_STEP_BLOCK0:
    case SDL_WMR_STEP_BLOCK3:
    case SDL_WMR_STEP_BLOCK2:
        break;
    default:
        return;
    }

    count = report[WMR_ANSWER_LENGTH];
    if (count > SDL_WMR_REPLY_DATA_LENGTH) {
        WMR_Fail(session, SDL_WMR_FAILURE_REPLY);
        return;
    }
    if (session->block_size == 0) {
        /* The first answer gives the block's size: what remains plus this answer's data */
        const uint64_t size = (uint64_t)WMR_Read32(&report[WMR_ANSWER_REMAIN]) + count;

        if (size == 0 || size > SDL_WMR_BLOCK_MAXIMUM ||
            (session->step == SDL_WMR_STEP_BLOCK2 && size > session->config_capacity)) {
            WMR_Fail(session, SDL_WMR_FAILURE_BLOCK);
            return;
        }
        session->block_size = (uint32_t)size;
    } else {
        /* Monado would ask again forever for an answer without data */
        if (count == 0) {
            WMR_Fail(session, SDL_WMR_FAILURE_BLOCK);
            return;
        }
        if (count > session->block_size - session->block_received) {
            count = session->block_size - session->block_received;
        }
    }
    WMR_Store(session, &report[WMR_ANSWER_DATA], count);
    if (session->block_received < session->block_size) {
        session->phase = SDL_WMR_PHASE_DELAY;
        session->deadline_ms = now_ms + SDL_WMR_FOLLOWUP_DELAY_MS;
    } else {
        WMR_FinishBlock(session, now_ms, sink);
    }
}

void SDL_WMR_Open(SDL_WMRSession *session, SDL_WMRModel model, bool padded,
                  uint8_t *config, size_t config_capacity, uint64_t now_ms, const SDL_WMRSink *sink)
{
    memset(session, 0, sizeof(*session));
    session->model = model;
    session->padded = padded;
    session->config = config;
    session->config_capacity = config ? config_capacity : 0;
    SDL_WMR_InitCalibration(&session->accel);
    SDL_WMR_InitCalibration(&session->gyro);
    session->step = SDL_WMR_STEP_RESET;
    WMR_Request(session, now_ms, WMR_COMMAND_RESET, 0x00, 0x00000000u, WMR_ANSWER_COMMAND, sink);
}

void SDL_WMR_Update(SDL_WMRSession *session, uint64_t now_ms, const SDL_WMRSink *sink)
{
    if (session->phase == SDL_WMR_PHASE_WAITING && now_ms >= session->deadline_ms) {
        WMR_Fail(session, SDL_WMR_FAILURE_TIMEOUT);
    } else if (session->phase == SDL_WMR_PHASE_DELAY && now_ms >= session->deadline_ms) {
        /* The follow-up asks for what remains of the block */
        WMR_Request(session, now_ms, WMR_COMMAND_READ, WMR_Block(session->step),
                    session->block_size - session->block_received, WMR_ANSWER_BLOCK, sink);
    }
}

SDL_WMRReport SDL_WMR_HandleReport(SDL_WMRSession *session, uint64_t now_ms,
                                   const uint8_t *report, size_t length,
                                   const SDL_WMRSink *sink, SDL_WMREvent *event)
{
    float value[3];

    if (!report || length < 1) {
        return SDL_WMR_REPORT_IGNORED;
    }
    switch (session->phase) {
    case SDL_WMR_PHASE_WAITING:
        if (report[0] != session->response_code) {
            return SDL_WMR_REPORT_IGNORED;
        }
        WMR_HandleAnswer(session, now_ms, report, length, sink);
        return SDL_WMR_REPORT_STARTUP;
    case SDL_WMR_PHASE_READY:
        if (!SDL_WMR_DecodeStatus(report, length, session->padded, session->model, &event->input)) {
            return SDL_WMR_REPORT_IGNORED;
        }
        SDL_WMR_GetState(session->model, &event->input, &event->state);
        SDL_WMR_AccelFromRaw(event->input.accel, value);
        SDL_WMR_Calibrate(&session->accel, value, event->accel);
        SDL_WMR_GyroFromRaw(event->input.gyro, value);
        SDL_WMR_Calibrate(&session->gyro, value, event->gyro);
        event->timestamp_ns = SDL_WMR_ExtendTimestamp(&session->ticks, event->input.timestamp) * SDL_WMR_NS_PER_TICK;
        return SDL_WMR_REPORT_STATUS;
    default:
        return SDL_WMR_REPORT_IGNORED;
    }
}
