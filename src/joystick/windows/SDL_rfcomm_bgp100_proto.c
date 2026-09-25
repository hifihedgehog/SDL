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

/* The Chainpus BGP100 and the Phonejoy. See SDL_rfcomm_bgp100_proto.h. */

#include "SDL_rfcomm_bgp100_proto.h"

#include <string.h>

/* The button for each key, -1 for none. On the Phonejoy keys 1 and 2 are
   the triggers. */
static const int8_t bgp100_keys[16] = {
    -1, -1, -1, -1,
    SDL_BGP100_BUTTON_START, SDL_BGP100_BUTTON_B, SDL_BGP100_BUTTON_A, SDL_BGP100_BUTTON_C,
    SDL_BGP100_BUTTON_L, SDL_BGP100_BUTTON_R, SDL_BGP100_BUTTON_UP, SDL_BGP100_BUTTON_LEFT,
    SDL_BGP100_BUTTON_RIGHT, SDL_BGP100_BUTTON_DOWN, SDL_BGP100_BUTTON_D, -1
};

static const int8_t phonejoy_keys[16] = {
    -1, -1, -1, SDL_PHONEJOY_BUTTON_SELECT,
    SDL_PHONEJOY_BUTTON_START, SDL_PHONEJOY_BUTTON_2, SDL_PHONEJOY_BUTTON_1, SDL_PHONEJOY_BUTTON_4,
    SDL_PHONEJOY_BUTTON_L1, SDL_PHONEJOY_BUTTON_R1, SDL_PHONEJOY_BUTTON_UP, SDL_PHONEJOY_BUTTON_LEFT,
    SDL_PHONEJOY_BUTTON_RIGHT, SDL_PHONEJOY_BUTTON_DOWN, SDL_PHONEJOY_BUTTON_3, -1
};

/* Direction pair n moves this axis, the odd n toward negative */
static const uint8_t phonejoy_direction_axes[8] = {
    SDL_PHONEJOY_AXIS_LEFT_Y, SDL_PHONEJOY_AXIS_LEFT_Y, SDL_PHONEJOY_AXIS_LEFT_X, SDL_PHONEJOY_AXIS_LEFT_X,
    SDL_PHONEJOY_AXIS_RIGHT_Y, SDL_PHONEJOY_AXIS_RIGHT_Y, SDL_PHONEJOY_AXIS_RIGHT_X, SDL_PHONEJOY_AXIS_RIGHT_X
};

/* Stick position frames, axis 11 to 14 */
static const uint8_t phonejoy_position_axes[4] = {
    SDL_PHONEJOY_AXIS_LEFT_Y, SDL_PHONEJOY_AXIS_LEFT_X, SDL_PHONEJOY_AXIS_RIGHT_Y, SDL_PHONEJOY_AXIS_RIGHT_X
};

static void BGP100_Identity(const SDL_BGP100State *s, SDL_SerialIdentity *identity)
{
    if (!s->phonejoy) {
        SDL_Serial_SetIdentity(identity, "Chainpus BGP100", SDL_SERIAL_TYPE_GAMEPAD, 0, SDL_BGP100_BUTTONS, 0, 0);
        identity->has_mapping = true;
        /* D on West as android-bluez-ime sends it. No source places C, so it
           takes North. */
        identity->mapping.a = SDL_Serial_MapButton(SDL_BGP100_BUTTON_A);
        identity->mapping.b = SDL_Serial_MapButton(SDL_BGP100_BUTTON_B);
        identity->mapping.x = SDL_Serial_MapButton(SDL_BGP100_BUTTON_D);
        identity->mapping.y = SDL_Serial_MapButton(SDL_BGP100_BUTTON_C);
        identity->mapping.leftshoulder = SDL_Serial_MapButton(SDL_BGP100_BUTTON_L);
        identity->mapping.rightshoulder = SDL_Serial_MapButton(SDL_BGP100_BUTTON_R);
        identity->mapping.start = SDL_Serial_MapButton(SDL_BGP100_BUTTON_START);
        identity->mapping.dpup = SDL_Serial_MapButton(SDL_BGP100_BUTTON_UP);
        identity->mapping.dpdown = SDL_Serial_MapButton(SDL_BGP100_BUTTON_DOWN);
        identity->mapping.dpleft = SDL_Serial_MapButton(SDL_BGP100_BUTTON_LEFT);
        identity->mapping.dpright = SDL_Serial_MapButton(SDL_BGP100_BUTTON_RIGHT);
        return;
    }
    SDL_Serial_SetIdentity(identity, "Phonejoy", SDL_SERIAL_TYPE_GAMEPAD, SDL_PHONEJOY_AXES, SDL_PHONEJOY_BUTTONS, 0, 0);
    identity->has_mapping = true;
    /* No source places buttons 1 to 4 on the pad: 1 North, 2 East, 3 South
       and 4 West */
    identity->mapping.a = SDL_Serial_MapButton(SDL_PHONEJOY_BUTTON_3);
    identity->mapping.b = SDL_Serial_MapButton(SDL_PHONEJOY_BUTTON_2);
    identity->mapping.x = SDL_Serial_MapButton(SDL_PHONEJOY_BUTTON_4);
    identity->mapping.y = SDL_Serial_MapButton(SDL_PHONEJOY_BUTTON_1);
    identity->mapping.leftshoulder = SDL_Serial_MapButton(SDL_PHONEJOY_BUTTON_L1);
    identity->mapping.rightshoulder = SDL_Serial_MapButton(SDL_PHONEJOY_BUTTON_R1);
    identity->mapping.back = SDL_Serial_MapButton(SDL_PHONEJOY_BUTTON_SELECT);
    identity->mapping.start = SDL_Serial_MapButton(SDL_PHONEJOY_BUTTON_START);
    identity->mapping.dpup = SDL_Serial_MapButton(SDL_PHONEJOY_BUTTON_UP);
    identity->mapping.dpdown = SDL_Serial_MapButton(SDL_PHONEJOY_BUTTON_DOWN);
    identity->mapping.dpleft = SDL_Serial_MapButton(SDL_PHONEJOY_BUTTON_LEFT);
    identity->mapping.dpright = SDL_Serial_MapButton(SDL_PHONEJOY_BUTTON_RIGHT);
    identity->mapping.leftx = SDL_Serial_MapAxis(SDL_PHONEJOY_AXIS_LEFT_X);
    identity->mapping.lefty = SDL_Serial_MapAxis(SDL_PHONEJOY_AXIS_LEFT_Y);
    identity->mapping.rightx = SDL_Serial_MapAxis(SDL_PHONEJOY_AXIS_RIGHT_X);
    identity->mapping.righty = SDL_Serial_MapAxis(SDL_PHONEJOY_AXIS_RIGHT_Y);
    identity->mapping.lefttrigger = SDL_Serial_MapAxis(SDL_PHONEJOY_AXIS_L2);
    identity->mapping.righttrigger = SDL_Serial_MapAxis(SDL_PHONEJOY_AXIS_R2);
}

static void BGP100_Key(SDL_BGP100State *s, uint8_t key, bool pressed)
{
    int button;

    if (s->phonejoy && (key == 1 || key == 2)) {
        s->controls.axes[(key == 1) ? SDL_PHONEJOY_AXIS_L2 : SDL_PHONEJOY_AXIS_R2] = pressed ? 32767 : -32768;
        return;
    }
    button = s->phonejoy ? phonejoy_keys[key & 0x0F] : bgp100_keys[key & 0x0F];
    if (button >= 0) {
        SDL_Serial_SetButton(&s->controls, button, pressed);
    }
}

/* Sum FF, bit 7 set in the first byte and clear in the second, B or F on
   top and a key from 4 to E */
static bool BGP100_Pair(SDL_BGP100State *s, uint8_t first, uint8_t second)
{
    const uint8_t event = (uint8_t)(first >> 4);
    const uint8_t key = (uint8_t)(first & 0x0F);

    if ((int)first + (int)second != 0xFF || !(first & 0x80) || (second & 0x80)) {
        return false;
    }
    if ((event != 0xB && event != 0xF) || key < 4 || key > 0xE) {
        return false;
    }
    BGP100_Key(s, key, event == 0xB);
    return true;
}

static void Phonejoy_Direction(SDL_BGP100State *s, uint8_t n, bool pressed)
{
    const uint8_t bit = (uint8_t)(1 << (n - 1));
    const int first = (n - 1) & ~1; /* The negative direction of the pair */
    bool negative, positive;

    if (pressed) {
        s->directions |= bit;
    } else {
        s->directions &= (uint8_t)~bit;
    }
    negative = (s->directions & (1 << first)) != 0;
    positive = (s->directions & (1 << (first + 1))) != 0;
    s->controls.axes[phonejoy_direction_axes[n - 1]] = (negative && !positive) ? -32768 : (positive && !negative) ? 32767 : 0;
}

/* A listed pair only */
static bool Phonejoy_Pair(SDL_BGP100State *s, uint8_t first, uint8_t second)
{
    const uint8_t event = (uint8_t)(first >> 4);
    const uint8_t n = (uint8_t)(first & 0x0F);

    if (event == 0xB || event == 0xF) {
        const bool pressed = (event == 0xB);
        uint8_t expected;

        if (n >= 3 && n <= 0xE) {
            expected = (uint8_t)(0xFF - first);
        } else if (n == 1) {
            expected = pressed ? 0x4D : 0x0D; /* L2, summing to FE */
        } else if (n == 2) {
            expected = pressed ? 0x4E : 0x0E; /* R2, summing to 100 */
        } else {
            return false;
        }
        if (second != expected) {
            return false;
        }
        BGP100_Key(s, n, pressed);
        return true;
    }
    if (event == 0xA || event == 0xE) {
        if (n < 1 || n > 8 || second != (uint8_t)(((event == 0xA) ? 0x20 : 0x10) + n)) {
            return false;
        }
        Phonejoy_Direction(s, n, event == 0xA);
        return true;
    }
    return false;
}

/* 7F is center and lower is up or left, as android-bluez-ime reads it */
static void Phonejoy_Position(SDL_BGP100State *s, uint8_t axis, uint8_t value)
{
    int32_t position = (int32_t)value - 127;

    if (position > 127) {
        position = 127;
    }
    s->controls.axes[phonejoy_position_axes[axis - 0x11]] = SDL_Serial_ScaleSigned(position, -127, 127);
}

static void BGP100_Event(SDL_BGP100State *s)
{
    if (!SDL_Serial_IsPresent(&s->base.serial, 0)) {
        SDL_SerialIdentity identity;

        BGP100_Identity(s, &identity);
        SDL_Serial_PresentWith(&s->base.serial, 0, &identity, &s->controls);
    } else {
        SDL_Serial_Commit(&s->base.serial, 0, &s->controls);
    }
}

static void BGP100_Drop(SDL_BGP100State *s, size_t count)
{
    if (count >= s->length) {
        s->length = 0;
        return;
    }
    memmove(s->pending, s->pending + count, s->length - count);
    s->length -= count;
}

static void BGP100_Scan(SDL_BGP100State *s)
{
    while (s->length > 0) {
        if (s->phonejoy && s->pending[0] == 0xFF) {
            if (s->length < 2) {
                return;
            }
            if (s->pending[1] >= 0x11 && s->pending[1] <= 0x14) {
                if (s->length < 3) {
                    return;
                }
                Phonejoy_Position(s, s->pending[1], s->pending[2]);
                BGP100_Event(s);
                BGP100_Drop(s, 3);
                continue;
            }
            BGP100_Drop(s, 1);
            continue;
        }
        if (s->length < 2) {
            return;
        }
        if (s->phonejoy ? Phonejoy_Pair(s, s->pending[0], s->pending[1]) : BGP100_Pair(s, s->pending[0], s->pending[1])) {
            BGP100_Event(s);
            BGP100_Drop(s, 2);
        } else {
            BGP100_Drop(s, 1);
        }
    }
}

static void BGP100_Start(void *state, const SDL_SerialSink *sink, bool phonejoy)
{
    SDL_BGP100State *s = (SDL_BGP100State *)state;

    SDL_RFCOMM_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->phonejoy = phonejoy;
    if (phonejoy) {
        s->controls.axes[SDL_PHONEJOY_AXIS_L2] = -32768;
        s->controls.axes[SDL_PHONEJOY_AXIS_R2] = -32768;
    }
}

static void BGP100_Reset(void *state, const SDL_SerialSink *sink, const char *name, int player_index, uint64_t now)
{
    (void)name;
    (void)player_index;
    (void)now;
    BGP100_Start(state, sink, false);
}

static void Phonejoy_Reset(void *state, const SDL_SerialSink *sink, const char *name, int player_index, uint64_t now)
{
    (void)name;
    (void)player_index;
    (void)now;
    BGP100_Start(state, sink, true);
}

static void BGP100_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_BGP100State *s = (SDL_BGP100State *)state;
    size_t i;

    (void)now;
    for (i = 0; i < length; ++i) {
        /* A scan leaves at most two bytes */
        s->pending[s->length++] = data[i];
        BGP100_Scan(s);
    }
}

/* No keep-alive is documented */
static void BGP100_Tick(void *state, uint64_t now)
{
    (void)state;
    (void)now;
}

static bool BGP100_GetDeadline(const void *state, uint64_t *deadline)
{
    (void)state;
    (void)deadline;
    return false;
}

const SDL_RFCOMMModule SDL_RFCOMMBGP100Module = {
    SDL_RFCOMM_FAMILY_BGP100,
    false,
    { 0 },
    sizeof(SDL_BGP100State),
    BGP100_Reset,
    BGP100_Feed,
    BGP100_Tick,
    BGP100_GetDeadline,
    NULL
};

const SDL_RFCOMMModule SDL_RFCOMMPhonejoyModule = {
    SDL_RFCOMM_FAMILY_PHONEJOY,
    false,
    { 0 },
    sizeof(SDL_BGP100State),
    Phonejoy_Reset,
    BGP100_Feed,
    BGP100_Tick,
    BGP100_GetDeadline,
    NULL
};
