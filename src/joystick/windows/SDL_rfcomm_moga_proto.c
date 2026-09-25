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

/* PowerA MOGA in Mode A. See SDL_rfcomm_moga_proto.h. */

#include "SDL_rfcomm_moga_proto.h"

#include <string.h>

#define MOGA_REPORT_START 0x7A

bool SDL_MOGA_IsAnalogName(const char *name)
{
    return !SDL_RFCOMM_StartsWith(name, "BD&A", true) && !SDL_RFCOMM_StartsWith(name, "BDA", true);
}

uint8_t SDL_MOGA_IdForPlayerIndex(int player_index)
{
    return (player_index >= 0 && player_index <= 3) ? (uint8_t)(player_index + 1) : SDL_MOGA_ID_NONE;
}

void SDL_MOGA_BuildCommand(uint8_t command, uint8_t id, uint8_t out[SDL_MOGA_COMMAND_LENGTH])
{
    out[0] = 0x5A;
    out[1] = SDL_MOGA_COMMAND_LENGTH;
    out[2] = command;
    out[3] = id;
    out[4] = (uint8_t)(out[0] ^ out[1] ^ out[2] ^ out[3]);
}

bool SDL_MOGA_DecodeReport(const uint8_t *report, size_t length, SDL_SerialControls *controls)
{
    uint8_t buttons, pad;

    if (!report || !controls || (length != 12 && length != 14)) {
        return false;
    }
    memset(controls, 0, sizeof(*controls));
    buttons = report[4];
    pad = report[5];
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_Y, (buttons & 0x01) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_B, (buttons & 0x02) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_A, (buttons & 0x04) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_X, (buttons & 0x08) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_START, (buttons & 0x10) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_SELECT, (buttons & 0x20) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_L1, (buttons & 0x40) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_R1, (buttons & 0x80) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_UP, (pad & 0x01) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_DOWN, (pad & 0x02) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_LEFT, (pad & 0x04) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_RIGHT, (pad & 0x08) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_L3, (pad & 0x40) != 0);
    SDL_Serial_SetButton(controls, SDL_MOGA_BUTTON_R3, (pad & 0x80) != 0);

    /* Two's complement, up positive, so both Y axes turn over for SDL */
    controls->axes[SDL_MOGA_AXIS_LEFT_X] = SDL_RFCOMM_AxisFromS8(report[6]);
    controls->axes[SDL_MOGA_AXIS_LEFT_Y] = SDL_RFCOMM_AxisFromS8Negated(report[7]);
    controls->axes[SDL_MOGA_AXIS_RIGHT_X] = SDL_RFCOMM_AxisFromS8(report[8]);
    controls->axes[SDL_MOGA_AXIS_RIGHT_Y] = SDL_RFCOMM_AxisFromS8Negated(report[9]);

    /* Analog triggers in 14-byte reports, the pressed bits otherwise */
    if (length == 14) {
        controls->axes[SDL_MOGA_AXIS_LEFT_TRIGGER] = SDL_RFCOMM_TriggerFromU8(report[10]);
        controls->axes[SDL_MOGA_AXIS_RIGHT_TRIGGER] = SDL_RFCOMM_TriggerFromU8(report[11]);
    } else {
        controls->axes[SDL_MOGA_AXIS_LEFT_TRIGGER] = (pad & 0x10) ? 32767 : -32768;
        controls->axes[SDL_MOGA_AXIS_RIGHT_TRIGGER] = (pad & 0x20) ? 32767 : -32768;
    }
    return true;
}

static void MOGA_Command(SDL_MOGAState *s, uint8_t command)
{
    uint8_t bytes[SDL_MOGA_COMMAND_LENGTH];

    SDL_MOGA_BuildCommand(command, s->id, bytes);
    SDL_RFCOMM_Send(&s->base, bytes, sizeof(bytes));
}

static void MOGA_Identity(const SDL_MOGAState *s, SDL_SerialIdentity *identity)
{
    SDL_Serial_SetIdentity(identity, s->name, SDL_SERIAL_TYPE_GAMEPAD, SDL_MOGA_AXES, SDL_MOGA_BUTTONS, 0, 0);
    identity->has_mapping = true;
    identity->mapping.a = SDL_Serial_MapButton(SDL_MOGA_BUTTON_A);
    identity->mapping.b = SDL_Serial_MapButton(SDL_MOGA_BUTTON_B);
    identity->mapping.x = SDL_Serial_MapButton(SDL_MOGA_BUTTON_X);
    identity->mapping.y = SDL_Serial_MapButton(SDL_MOGA_BUTTON_Y);
    identity->mapping.back = SDL_Serial_MapButton(SDL_MOGA_BUTTON_SELECT);
    identity->mapping.start = SDL_Serial_MapButton(SDL_MOGA_BUTTON_START);
    identity->mapping.leftstick = SDL_Serial_MapButton(SDL_MOGA_BUTTON_L3);
    identity->mapping.rightstick = SDL_Serial_MapButton(SDL_MOGA_BUTTON_R3);
    identity->mapping.leftshoulder = SDL_Serial_MapButton(SDL_MOGA_BUTTON_L1);
    identity->mapping.rightshoulder = SDL_Serial_MapButton(SDL_MOGA_BUTTON_R1);
    identity->mapping.dpup = SDL_Serial_MapButton(SDL_MOGA_BUTTON_UP);
    identity->mapping.dpdown = SDL_Serial_MapButton(SDL_MOGA_BUTTON_DOWN);
    identity->mapping.dpleft = SDL_Serial_MapButton(SDL_MOGA_BUTTON_LEFT);
    identity->mapping.dpright = SDL_Serial_MapButton(SDL_MOGA_BUTTON_RIGHT);
    identity->mapping.leftx = SDL_Serial_MapAxis(SDL_MOGA_AXIS_LEFT_X);
    identity->mapping.lefty = SDL_Serial_MapAxis(SDL_MOGA_AXIS_LEFT_Y);
    identity->mapping.rightx = SDL_Serial_MapAxis(SDL_MOGA_AXIS_RIGHT_X);
    identity->mapping.righty = SDL_Serial_MapAxis(SDL_MOGA_AXIS_RIGHT_Y);
    identity->mapping.lefttrigger = SDL_Serial_MapAxis(SDL_MOGA_AXIS_LEFT_TRIGGER);
    identity->mapping.righttrigger = SDL_Serial_MapAxis(SDL_MOGA_AXIS_RIGHT_TRIGGER);
}

/* A report that passed its checks. The first one makes the joystick. */
static void MOGA_Report(SDL_MOGAState *s, const uint8_t *report, size_t length, uint64_t now)
{
    if (!SDL_MOGA_DecodeReport(report, length, &s->controls)) {
        return;
    }
    s->quiet_at = now + SDL_MOGA_QUIET_MS;
    if (!SDL_Serial_IsPresent(&s->base.serial, 0)) {
        SDL_SerialIdentity identity;

        MOGA_Identity(s, &identity);
        SDL_Serial_PresentWith(&s->base.serial, 0, &identity, &s->controls);
    } else {
        SDL_Serial_Commit(&s->base.serial, 0, &s->controls);
    }
    if (s->polling) {
        /* The poll was answered, so listen again. This also recovers a
           listen command the pad ignored after connecting. */
        s->polling = false;
        MOGA_Command(s, s->analog ? SDL_MOGA_LISTEN_ANALOG : SDL_MOGA_LISTEN_DIGITAL);
    }
}

static void MOGA_Drop(SDL_MOGAState *s, size_t count)
{
    if (count >= s->length) {
        s->length = 0;
        return;
    }
    memmove(s->report, s->report + count, s->length - count);
    s->length -= count;
}

/* Finds 7A, a length of 12 or 14 and a matching checksum, else drops one
   byte and looks again */
static void MOGA_Scan(SDL_MOGAState *s, uint64_t now)
{
    while (s->length > 0) {
        uint8_t checksum = 0;
        size_t size, i;

        if (s->report[0] != MOGA_REPORT_START) {
            MOGA_Drop(s, 1);
            continue;
        }
        if (s->length < 2) {
            return;
        }
        size = s->report[1];
        if (size != 12 && size != 14) {
            MOGA_Drop(s, 1);
            continue;
        }
        if (s->length < size) {
            return;
        }
        for (i = 0; i < size - 1; ++i) {
            checksum ^= s->report[i];
        }
        if (checksum != s->report[size - 1]) {
            MOGA_Drop(s, 1);
            continue;
        }
        MOGA_Report(s, s->report, size, now);
        MOGA_Drop(s, size);
    }
}

static void MOGA_Reset(void *state, const SDL_SerialSink *sink, const char *name, int player_index, uint64_t now)
{
    SDL_MOGAState *s = (SDL_MOGAState *)state;

    SDL_RFCOMM_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->analog = SDL_MOGA_IsAnalogName(name);
    /* Id 1 until the joystick has a player index */
    s->id = (player_index >= 0) ? SDL_MOGA_IdForPlayerIndex(player_index) : 1;
    SDL_RFCOMM_CopyText(s->name, sizeof(s->name), (s->analog && name) ? name : "PowerA MOGA");
    s->quiet_at = now + SDL_MOGA_QUIET_MS;

    /* The id, the state so far, then reports on every change */
    MOGA_Command(s, SDL_MOGA_SET_ID);
    MOGA_Command(s, s->analog ? SDL_MOGA_POLL_ANALOG : SDL_MOGA_POLL_DIGITAL);
    MOGA_Command(s, s->analog ? SDL_MOGA_LISTEN_ANALOG : SDL_MOGA_LISTEN_DIGITAL);
}

static void MOGA_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_MOGAState *s = (SDL_MOGAState *)state;
    size_t i;

    for (i = 0; i < length; ++i) {
        /* A scan leaves at most 13 bytes, so the report buffer never overflows */
        s->report[s->length++] = data[i];
        MOGA_Scan(s, now);
    }
}

/* 2000 ms without a report polls. A second 2000 ms ends the link. */
static void MOGA_Tick(void *state, uint64_t now)
{
    SDL_MOGAState *s = (SDL_MOGAState *)state;

    if (now < s->quiet_at) {
        return;
    }
    if (s->polling) {
        SDL_RFCOMM_End(&s->base);
        return;
    }
    s->polling = true;
    s->quiet_at = now + SDL_MOGA_QUIET_MS;
    MOGA_Command(s, s->analog ? SDL_MOGA_POLL_ANALOG : SDL_MOGA_POLL_DIGITAL);
}

static bool MOGA_GetDeadline(const void *state, uint64_t *deadline)
{
    const SDL_MOGAState *s = (const SDL_MOGAState *)state;

    *deadline = s->quiet_at;
    return true;
}

/* A new id is a new 43 command */
static void MOGA_SetPlayerIndex(void *state, int player_index, uint64_t now)
{
    SDL_MOGAState *s = (SDL_MOGAState *)state;
    const uint8_t id = SDL_MOGA_IdForPlayerIndex(player_index);

    (void)now;
    if (id != s->id) {
        s->id = id;
        MOGA_Command(s, SDL_MOGA_SET_ID);
    }
}

const SDL_RFCOMMModule SDL_RFCOMMMogaModule = {
    SDL_RFCOMM_FAMILY_MOGA,
    true,
    /* Serial Port, 00001101-0000-1000-8000-00805F9B34FB */
    { 0x00, 0x00, 0x11, 0x01, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB },
    sizeof(SDL_MOGAState),
    MOGA_Reset,
    MOGA_Feed,
    MOGA_Tick,
    MOGA_GetDeadline,
    MOGA_SetPlayerIndex
};
