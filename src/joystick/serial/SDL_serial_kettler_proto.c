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

#include "SDL_serial_kettler_proto.h"

#include <stdio.h>
#include <string.h>

static const char *const kettler_setup[SDL_KETTLER_COMMANDS] = { "VE", "ID", "VE", "KI", "CA", "RS", "CM", "SP1" };

/* Fields 1-6 and 8 are decimal numbers, field 7 the time as MM:SS */
bool SDL_Kettler_ParseStatus(const char *line, int *fields)
{
    int field = 0;
    const char *p = line;

    if (!line || !fields) {
        return false;
    }
    for (;;) {
        const char *start = p;
        long value = 0;
        int digits = 0;

        while (*p && *p != '\t') {
            if (*p >= '0' && *p <= '9') {
                if (digits < 6) {
                    value = value * 10 + (*p - '0');
                }
                ++digits;
            } else if (!(field == 6 && *p == ':')) {
                return false;
            }
            ++p;
        }
        if (p == start || field >= SDL_KETTLER_FIELDS) {
            return false;
        }
        fields[field] = (field == 6) ? 0 : (int)((value > 32767) ? 32767 : value);
        ++field;
        if (*p == '\0') {
            break;
        }
        ++p;
    }
    return field == SDL_KETTLER_FIELDS;
}

static void Kettler_Command(SDL_KettlerState *s, const char *command, uint8_t tag)
{
    char text[16];
    const int n = snprintf(text, sizeof(text), "%s\r\n", command);

    if (n > 0 && n < (int)sizeof(text)) {
        SDL_Serial_QueueWrite(&s->base, tag, (const uint8_t *)text, (size_t)n);
    }
}

static void Kettler_Setup(SDL_KettlerState *s, uint64_t now)
{
    s->step = SDL_KETTLER_STEP_SETUP;
    s->command = 0;
    s->next_at = now;
}

static void Kettler_Line(SDL_KettlerState *s, uint64_t now)
{
    int fields[SDL_KETTLER_FIELDS];
    SDL_SerialControls controls;

    s->last_line = now;
    if (!SDL_Kettler_ParseStatus(s->line, fields)) {
        return; /* Answers to the set-up commands, and 4-field key lines */
    }
    if (!SDL_Serial_IsPresent(&s->base, 0)) {
        SDL_SerialIdentity identity;

        SDL_Serial_SetIdentity(&identity, "Kettler Ergometer", SDL_SERIAL_TYPE_UNKNOWN, SDL_KETTLER_AXES, 0, 0, 0);
        SDL_Serial_Present(&s->base, 0, &identity);
    }
    controls = s->base.snapshots[0].controls;
    controls.axes[0] = (int16_t)fields[1]; /* Cadence */
    controls.axes[1] = (int16_t)fields[7]; /* Power on the brake */
    controls.axes[2] = (int16_t)fields[2]; /* Speed in 0.1 km/h */
    controls.axes[3] = (int16_t)fields[0]; /* Heart rate */
    controls.axes[4] = (int16_t)fields[4]; /* Target power */
    SDL_Serial_Commit(&s->base, 0, &controls);
}

static void Kettler_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_KettlerState *s = (SDL_KettlerState *)state;
    const SDL_SerialLine line = SDL_Serial_Line(SDL_KETTLER_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);

    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    SDL_Serial_QueueLine(&s->base, 0, &line);
    Kettler_Setup(s, now);
}

/* Lines end at 0A. 0D bytes are skipped and empty lines ignored. */
static void Kettler_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_KettlerState *s = (SDL_KettlerState *)state;
    size_t i;

    for (i = 0; i < length; ++i) {
        const uint8_t byte = data[i];

        if (byte == 0x0A) {
            s->line[s->line_length] = '\0';
            if (!s->line_overflow && s->line_length > 0) {
                Kettler_Line(s, now);
            }
            s->line_length = 0;
            s->line_overflow = false;
        } else if (byte == 0x0D) {
            continue;
        } else if (s->line_length < SDL_KETTLER_LINE_LENGTH && byte != 0x00) {
            s->line[s->line_length++] = (char)byte;
        } else {
            s->line_overflow = true;
        }
    }
}

static void Kettler_Tick(void *state, uint64_t now)
{
    SDL_KettlerState *s = (SDL_KettlerState *)state;

    switch (s->step) {
    case SDL_KETTLER_STEP_SETUP:
        if (now >= s->next_at) {
            /* Each command 150 ms after the one before it left */
            Kettler_Command(s, kettler_setup[s->command], 0);
            if (++s->command == SDL_KETTLER_COMMANDS) {
                /* The first ST is due 2000 ms after SP1 */
                s->step = SDL_KETTLER_STEP_RUNNING;
                s->next_at = now + SDL_KETTLER_POLL_MS;
                s->last_line = now;
            } else {
                s->next_at = now + SDL_KETTLER_COMMAND_MS;
            }
        }
        break;
    case SDL_KETTLER_STEP_RUNNING:
        if (now >= s->last_line + SDL_KETTLER_SILENCE_MS) {
            SDL_Serial_Absent(&s->base, 0);
            SDL_Serial_ClearActions(&s->base);
            s->power_pending = false;
            s->step = SDL_KETTLER_STEP_WAIT;
            s->next_at = now + SDL_KETTLER_RECONNECT_MS;
            break;
        }
        if (now >= s->next_at) {
            if (s->power_pending) {
                char command[16];

                (void)snprintf(command, sizeof(command), "PW%u", (unsigned int)s->power);
                Kettler_Command(s, command, 0);
                s->power_pending = false;
            } else {
                Kettler_Command(s, "ST", 0);
            }
            s->next_at = now + SDL_KETTLER_POLL_MS;
        }
        break;
    case SDL_KETTLER_STEP_WAIT:
        if (now >= s->next_at) {
            Kettler_Setup(s, now);
            Kettler_Tick(state, now);
        }
        break;
    }
}

static void Kettler_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_KettlerState *s = (SDL_KettlerState *)state;
    uint8_t tag;

    (void)success;
    (void)now;
    (void)SDL_Serial_FinishAction(&s->base, &tag);
}

/* A little-endian wattage, sent as PW in place of the next ST */
static void Kettler_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    SDL_KettlerState *s = (SDL_KettlerState *)state;

    (void)now;
    if (request->kind != SDL_SERIAL_OUTPUT_EFFECT || request->length != 2 || !SDL_Serial_IsPresent(&s->base, 0)) {
        return;
    }
    s->power = (uint16_t)(request->data[0] | (request->data[1] << 8));
    s->power_pending = true;
}

static bool Kettler_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_KettlerState *s = (SDL_KettlerState *)state;
    bool have = false;

    SDL_Serial_EarlierDeadline(&have, deadline, s->next_at);
    if (s->step == SDL_KETTLER_STEP_RUNNING) {
        SDL_Serial_EarlierDeadline(&have, deadline, s->last_line + SDL_KETTLER_SILENCE_MS);
    }
    return have;
}

const SDL_SerialModule SDL_SerialKettlerModule = {
    "kettler",
    sizeof(SDL_KettlerState),
    false,
    2,
    2,
    Kettler_Reset,
    Kettler_Feed,
    Kettler_Tick,
    SDL_Serial_NextAction,
    Kettler_ActionDone,
    Kettler_Output,
    Kettler_GetDeadline,
    SDL_Serial_GetSnapshot
};
