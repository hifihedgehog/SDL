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

#include "SDL_serial_magellan_proto.h"

#include <string.h>

#define MAGELLAN_CR 0x0D

/* Each nibble travels as the character at its index here */
static const char magellan_nibbles[16] = { '0', 'A', 'B', '3', 'D', '5', '6', 'G', 'H', '9', ':', 'K', '<', 'M', 'N', '?' };

static bool Magellan_Nibble(uint8_t c, int *value)
{
    if (c != (uint8_t)magellan_nibbles[c & 0x0F]) {
        return false;
    }
    *value = c & 0x0F;
    return true;
}

bool SDL_Magellan_DecodeAxes(const uint8_t *line, size_t length, int16_t *axes)
{
    int32_t values[SDL_MAGELLAN_AXES];
    int i, j;

    if (!line || length != 25 || line[0] != 'd') {
        return false;
    }
    for (i = 0; i < SDL_MAGELLAN_AXES; ++i) {
        int32_t value = 0;

        for (j = 0; j < 4; ++j) {
            int nibble;

            if (!Magellan_Nibble(line[1 + 4 * i + j], &nibble)) {
                return false;
            }
            value = (value << 4) | nibble;
        }
        values[i] = value - 32768;
    }
    for (i = 0; i < SDL_MAGELLAN_AXES; ++i) {
        axes[i] = (int16_t)values[i];
    }
    return true;
}

bool SDL_Magellan_DecodeKeys(const uint8_t *line, size_t length, uint16_t *keys)
{
    int n1, n2, n3;

    if (!line || length != 4 || line[0] != 'k') {
        return false;
    }
    if (!Magellan_Nibble(line[1], &n1) || !Magellan_Nibble(line[2], &n2) || !Magellan_Nibble(line[3], &n3)) {
        return false;
    }
    /* Keys 1-4, then 5-8, then *, then the three bits that follow it */
    *keys = (uint16_t)(n1 | (n2 << 4) | (n3 << 8));
    return true;
}

static bool Magellan_Contains(const uint8_t *line, size_t length, const char *text)
{
    const size_t n = strlen(text);
    size_t i;

    for (i = 0; i + n <= length; ++i) {
        if (memcmp(line + i, text, n) == 0) {
            return true;
        }
    }
    return false;
}

const char *SDL_Magellan_NameFromVersion(const uint8_t *line, size_t length)
{
    if (Magellan_Contains(line, length, "MAGELLAN")) {
        return "Magellan SpaceMouse";
    }
    if (Magellan_Contains(line, length, "SPACEBALL")) {
        return "Spaceball 5000";
    }
    if (Magellan_Contains(line, length, "CadMan")) {
        return "CadMan";
    }
    return "Magellan SpaceMouse";
}

static void Magellan_SetTimer(SDL_MagellanState *s, uint64_t at)
{
    s->deadline = at;
    s->deadline_set = true;
}

static void Magellan_Write(SDL_MagellanState *s, const char *command, SDL_MagellanStep next)
{
    if (++s->write_seq == 0) {
        s->write_seq = 1;
    }
    SDL_Serial_QueueWrite(&s->base, s->write_seq, (const uint8_t *)command, strlen(command));
    s->waiting_seq = s->write_seq;
    s->step = next;
    s->deadline_set = false;
}

/* Step 2: mode off. The first start-up waits 1000 ms before it. */
static void Magellan_ModeOff(SDL_MagellanState *s)
{
    Magellan_Write(s, "\r\rm0\r", SDL_MAGELLAN_STEP_MODE_OFF);
}

static void Magellan_Fail(SDL_MagellanState *s, uint64_t now)
{
    SDL_Serial_ClearActions(&s->base);
    s->waiting_seq = 0;
    s->step = SDL_MAGELLAN_STEP_RETRY;
    Magellan_SetTimer(s, now + SDL_MAGELLAN_RETRY_MS);
}

/* The step after a line, or its timeout, in steps 2 and 3 */
static void Magellan_Advance(SDL_MagellanState *s)
{
    switch (s->step) {
    case SDL_MAGELLAN_STEP_MODE_OFF:
        Magellan_Write(s, "z\r", SDL_MAGELLAN_STEP_ZERO1);
        break;
    case SDL_MAGELLAN_STEP_ZERO1:
        Magellan_Write(s, "z\r", SDL_MAGELLAN_STEP_ZERO2);
        break;
    case SDL_MAGELLAN_STEP_ZERO2:
        Magellan_Write(s, "q00\r", SDL_MAGELLAN_STEP_Q);
        break;
    default:
        break;
    }
}

static bool Magellan_LineIs(const uint8_t *line, size_t length, const char *text)
{
    return length == strlen(text) && memcmp(line, text, length) == 0;
}

static void Magellan_Present(SDL_MagellanState *s, const uint8_t *line, size_t length)
{
    SDL_SerialIdentity identity;

    SDL_Serial_SetIdentity(&identity, SDL_Magellan_NameFromVersion(line, length), SDL_SERIAL_TYPE_UNKNOWN,
                           SDL_MAGELLAN_AXES, SDL_MAGELLAN_BUTTONS, 0, 0);
    s->step = SDL_MAGELLAN_STEP_PRESENT;
    s->deadline_set = false;
    SDL_Serial_Present(&s->base, 0, &identity);
}

static void Magellan_Decode(SDL_MagellanState *s, const uint8_t *line, size_t length)
{
    SDL_SerialControls controls = s->base.snapshots[0].controls;
    int16_t axes[SDL_MAGELLAN_AXES];
    uint16_t keys;
    int i;

    if (SDL_Magellan_DecodeAxes(line, length, axes)) {
        for (i = 0; i < SDL_MAGELLAN_AXES; ++i) {
            controls.axes[i] = axes[i];
        }
        SDL_Serial_Commit(&s->base, 0, &controls);
    } else if (SDL_Magellan_DecodeKeys(line, length, &keys)) {
        for (i = 0; i < SDL_MAGELLAN_BUTTONS; ++i) {
            SDL_Serial_SetButton(&controls, i, (keys & (1 << i)) != 0);
        }
        SDL_Serial_Commit(&s->base, 0, &controls);
    }
}

static void Magellan_HandleLine(SDL_MagellanState *s, uint64_t now)
{
    const uint8_t *line = s->line;
    const size_t length = s->line_overflow ? 0 : s->line_length;

    (void)now;
    if (length > 0 && line[0] == 'e') {
        /* The byte after the type is 1 for an illegal command, 2 for a framing error */
        ++s->errors;
        if (length >= 2 && line[1] == 0x01) {
            SDL_Serial_Log(&s->base, "Magellan rejected a command");
        } else if (length >= 2 && line[1] == 0x02) {
            SDL_Serial_Log(&s->base, "Magellan reported a framing error");
        } else {
            SDL_Serial_Log(&s->base, "Magellan reported an error");
        }
    }
    if (length > 0 && SDL_Serial_IsPresent(&s->base, 0)) {
        switch (line[0]) {
        case 'd':
        case 'k':
            Magellan_Decode(s, line, length);
            break;
        case 'm':
            /* Mouse mode: start over from step 2, the device still present */
            if (s->step == SDL_MAGELLAN_STEP_PRESENT && length >= 2 && (line[1] & 0x08)) {
                ++s->reinits;
                Magellan_ModeOff(s);
                return;
            }
            break;
        default:
            break;
        }
    }

    switch (s->step) {
    case SDL_MAGELLAN_STEP_MODE_OFF:
    case SDL_MAGELLAN_STEP_ZERO1:
    case SDL_MAGELLAN_STEP_ZERO2:
        Magellan_Advance(s);
        break;
    case SDL_MAGELLAN_STEP_Q:
        if (Magellan_LineIs(line, length, "q00")) {
            Magellan_Write(s, "pAA\r", SDL_MAGELLAN_STEP_P);
        }
        break;
    case SDL_MAGELLAN_STEP_P:
        if (Magellan_LineIs(line, length, "pAA")) {
            Magellan_Write(s, "nH\r", SDL_MAGELLAN_STEP_N);
        }
        break;
    case SDL_MAGELLAN_STEP_N:
        if (Magellan_LineIs(line, length, "nH")) {
            Magellan_Write(s, "m3\r", SDL_MAGELLAN_STEP_M);
        }
        break;
    case SDL_MAGELLAN_STEP_M:
        if (Magellan_LineIs(line, length, "m3")) {
            Magellan_Write(s, "vQ\r", SDL_MAGELLAN_STEP_V);
        }
        break;
    case SDL_MAGELLAN_STEP_V:
        if (length > 0 && line[0] == 'v') {
            Magellan_Present(s, line, length);
        }
        break;
    default:
        break;
    }
}

static void Magellan_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_MagellanState *s = (SDL_MagellanState *)state;
    const SDL_SerialLine line = SDL_Serial_Line(SDL_MAGELLAN_RATE, 8, SDL_SERIAL_PARITY_NONE, 2, SDL_SERIAL_FLOW_NONE, true, true);
    const uint8_t write_seq = s->write_seq;

    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->write_seq = write_seq;
    SDL_Serial_QueueLine(&s->base, 0, &line);
    s->step = SDL_MAGELLAN_STEP_OPEN;
    Magellan_SetTimer(s, now + SDL_MAGELLAN_OPEN_MS);
}

static void Magellan_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_MagellanState *s = (SDL_MagellanState *)state;
    size_t i;

    for (i = 0; i < length; ++i) {
        if (data[i] == MAGELLAN_CR) {
            Magellan_HandleLine(s, now);
            s->line_length = 0;
            s->line_overflow = false;
        } else if (s->line_length < sizeof(s->line)) {
            s->line[s->line_length++] = data[i];
        } else {
            s->line_overflow = true;
        }
    }
}

static void Magellan_Tick(void *state, uint64_t now)
{
    SDL_MagellanState *s = (SDL_MagellanState *)state;

    if (!s->deadline_set || now < s->deadline) {
        return;
    }
    s->deadline_set = false;
    switch (s->step) {
    case SDL_MAGELLAN_STEP_OPEN:
    case SDL_MAGELLAN_STEP_RETRY:
        Magellan_ModeOff(s);
        break;
    case SDL_MAGELLAN_STEP_MODE_OFF:
    case SDL_MAGELLAN_STEP_ZERO1:
    case SDL_MAGELLAN_STEP_ZERO2:
        Magellan_Advance(s);
        break;
    case SDL_MAGELLAN_STEP_Q:
    case SDL_MAGELLAN_STEP_P:
    case SDL_MAGELLAN_STEP_N:
    case SDL_MAGELLAN_STEP_M:
    case SDL_MAGELLAN_STEP_V:
        Magellan_Fail(s, now);
        break;
    default:
        break;
    }
}

static void Magellan_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_MagellanState *s = (SDL_MagellanState *)state;
    uint8_t tag;
    uint64_t wait;

    if (!SDL_Serial_FinishAction(&s->base, &tag) || tag == 0 || tag != s->waiting_seq) {
        return;
    }
    s->waiting_seq = 0;
    if (!success) {
        Magellan_Fail(s, now);
        return;
    }
    switch (s->step) {
    case SDL_MAGELLAN_STEP_MODE_OFF:
    case SDL_MAGELLAN_STEP_ZERO1:
    case SDL_MAGELLAN_STEP_ZERO2:
        wait = SDL_MAGELLAN_SHORT_MS;
        break;
    default:
        wait = SDL_MAGELLAN_ECHO_MS;
        break;
    }
    Magellan_SetTimer(s, now + wait);
}

static void Magellan_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

static bool Magellan_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_MagellanState *s = (SDL_MagellanState *)state;

    if (!s->deadline_set) {
        return false;
    }
    *deadline = s->deadline;
    return true;
}

const SDL_SerialModule SDL_SerialMagellanModule = {
    "magellan",
    sizeof(SDL_MagellanState),
    false,
    0,
    0,
    Magellan_Reset,
    Magellan_Feed,
    Magellan_Tick,
    SDL_Serial_NextAction,
    Magellan_ActionDone,
    Magellan_Output,
    Magellan_GetDeadline,
    SDL_Serial_GetSnapshot
};
