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

#include "SDL_serial_proto.h"

#include <string.h>

int16_t SDL_Serial_ScaleSigned(int32_t value, int32_t min, int32_t max)
{
    int64_t v = value;

    if (v < min) {
        v = min;
    }
    if (v > max) {
        v = max;
    }
    if (v >= 0) {
        return (int16_t)((max > 0) ? (v * 32767) / max : 0);
    }
    return (int16_t)((min < 0) ? (v * 32768) / -(int64_t)min : 0);
}

int16_t SDL_Serial_ScaleUnsigned(int32_t value, int32_t lo, int32_t hi)
{
    const int32_t center = (lo + hi) / 2;

    return SDL_Serial_ScaleSigned(value - center, lo - center, hi - center);
}

void SDL_Serial_SetButton(SDL_SerialControls *controls, int button, bool pressed)
{
    if (button < 0 || button >= SDL_SERIAL_MAX_BUTTONS) {
        return;
    }
    if (pressed) {
        controls->buttons[button / 8] |= (uint8_t)(1 << (button % 8));
    } else {
        controls->buttons[button / 8] &= (uint8_t)~(1 << (button % 8));
    }
}

bool SDL_Serial_GetButton(const SDL_SerialControls *controls, int button)
{
    if (button < 0 || button >= SDL_SERIAL_MAX_BUTTONS) {
        return false;
    }
    return (controls->buttons[button / 8] & (1 << (button % 8))) != 0;
}

/* Ball motion is not state, so it takes no part */
bool SDL_Serial_ControlsEqual(const SDL_SerialControls *a, const SDL_SerialControls *b)
{
    int i;

    for (i = 0; i < SDL_SERIAL_MAX_AXES; ++i) {
        if (a->axes[i] != b->axes[i]) {
            return false;
        }
    }
    for (i = 0; i < SDL_SERIAL_MAX_BUTTONS / 8; ++i) {
        if (a->buttons[i] != b->buttons[i]) {
            return false;
        }
    }
    for (i = 0; i < SDL_SERIAL_MAX_HATS; ++i) {
        if (a->hats[i] != b->hats[i]) {
            return false;
        }
    }
    return true;
}

void SDL_Serial_ResetBase(SDL_SerialBase *base, const SDL_SerialSink *sink)
{
    const uint32_t epoch = base->epoch;

    memset(base, 0, sizeof(*base));
    base->epoch = epoch + 1;
    if (sink) {
        base->sink = *sink;
    }
}

static SDL_SerialAction *Serial_AppendAction(SDL_SerialBase *base, SDL_SerialActionKind kind, uint8_t tag)
{
    SDL_SerialAction *action;

    if (base->action_count >= SDL_SERIAL_MAX_ACTIONS) {
        return NULL;
    }
    action = &base->actions[(base->action_head + base->action_count) % SDL_SERIAL_MAX_ACTIONS];
    memset(action, 0, sizeof(*action));
    action->kind = kind;
    action->tag = tag;
    ++base->action_count;
    return action;
}

bool SDL_Serial_QueueLine(SDL_SerialBase *base, uint8_t tag, const SDL_SerialLine *line)
{
    SDL_SerialAction *action = Serial_AppendAction(base, SDL_SERIAL_ACTION_SET_LINE, tag);

    if (!action) {
        return false;
    }
    action->line = *line;
    return true;
}

bool SDL_Serial_QueueModem(SDL_SerialBase *base, uint8_t tag, bool dtr, bool rts)
{
    SDL_SerialAction *action = Serial_AppendAction(base, SDL_SERIAL_ACTION_SET_MODEM, tag);

    if (!action) {
        return false;
    }
    action->line.dtr = dtr;
    action->line.rts = rts;
    return true;
}

bool SDL_Serial_QueueWrite(SDL_SerialBase *base, uint8_t tag, const uint8_t *data, size_t length)
{
    SDL_SerialAction *action;

    if (!data || length == 0 || length > SDL_SERIAL_MAX_WRITE) {
        return false;
    }
    action = Serial_AppendAction(base, SDL_SERIAL_ACTION_WRITE, tag);
    if (!action) {
        return false;
    }
    memcpy(action->data, data, length);
    action->length = length;
    return true;
}

bool SDL_Serial_ReplaceWrite(SDL_SerialBase *base, uint8_t tag, const uint8_t *data, size_t length)
{
    int i;

    if (!data || length == 0 || length > SDL_SERIAL_MAX_WRITE) {
        return false;
    }
    for (i = 0; i < base->action_count; ++i) {
        SDL_SerialAction *action = &base->actions[(base->action_head + i) % SDL_SERIAL_MAX_ACTIONS];

        if (action->kind == SDL_SERIAL_ACTION_WRITE && action->tag == tag) {
            memcpy(action->data, data, length);
            action->length = length;
            return true;
        }
    }
    return false;
}

bool SDL_Serial_QueueDrain(SDL_SerialBase *base, uint8_t tag)
{
    return Serial_AppendAction(base, SDL_SERIAL_ACTION_DRAIN, tag) != NULL;
}

void SDL_Serial_ClearActions(SDL_SerialBase *base)
{
    base->action_head = 0;
    base->action_count = 0;
    ++base->epoch;
}

bool SDL_Serial_NextAction(void *state, SDL_SerialAction *action)
{
    SDL_SerialBase *base = (SDL_SerialBase *)state;

    if (!action || base->action_count == 0) {
        return false;
    }
    *action = base->actions[base->action_head];
    base->action_head = (base->action_head + 1) % SDL_SERIAL_MAX_ACTIONS;
    --base->action_count;
    base->taken = true;
    base->taken_tag = action->tag;
    base->taken_epoch = base->epoch;
    return true;
}

bool SDL_Serial_FinishAction(SDL_SerialBase *base, uint8_t *tag)
{
    if (!base->taken) {
        return false;
    }
    base->taken = false;
    if (base->taken_epoch != base->epoch) {
        return false;
    }
    *tag = base->taken_tag;
    return true;
}

const SDL_SerialSnapshot *SDL_Serial_GetSnapshot(void *state, int sub)
{
    SDL_SerialBase *base = (SDL_SerialBase *)state;

    if (sub < 0 || sub >= SDL_SERIAL_MAX_SUBDEVICES) {
        return NULL;
    }
    return &base->snapshots[sub];
}

static void Serial_Emit(SDL_SerialBase *base, int sub)
{
    SDL_SerialSnapshot *snapshot = &base->snapshots[sub];

    ++snapshot->changes;
    if (base->sink.changed) {
        base->sink.changed(base->sink.userdata, sub, snapshot);
    }
    snapshot->controls.ball[0] = 0;
    snapshot->controls.ball[1] = 0;
}

void SDL_Serial_Present(SDL_SerialBase *base, int sub, const SDL_SerialIdentity *identity)
{
    SDL_SerialSnapshot *snapshot;

    if (sub < 0 || sub >= SDL_SERIAL_MAX_SUBDEVICES || !identity) {
        return;
    }
    snapshot = &base->snapshots[sub];
    if (snapshot->present) {
        return;
    }
    snapshot->present = true;
    snapshot->identity = *identity;
    snapshot->identity.name[SDL_SERIAL_NAME_LENGTH - 1] = '\0';
    memset(&snapshot->controls, 0, sizeof(snapshot->controls));
    memset(base->pulse_end[sub], 0, sizeof(base->pulse_end[sub]));
    Serial_Emit(base, sub);
}

void SDL_Serial_Absent(SDL_SerialBase *base, int sub)
{
    SDL_SerialSnapshot *snapshot;

    if (sub < 0 || sub >= SDL_SERIAL_MAX_SUBDEVICES) {
        return;
    }
    snapshot = &base->snapshots[sub];
    if (!snapshot->present) {
        return;
    }
    snapshot->present = false;
    memset(&snapshot->controls, 0, sizeof(snapshot->controls));
    memset(base->pulse_end[sub], 0, sizeof(base->pulse_end[sub]));
    Serial_Emit(base, sub);
}

void SDL_Serial_AbsentAll(SDL_SerialBase *base)
{
    int sub;

    for (sub = 0; sub < SDL_SERIAL_MAX_SUBDEVICES; ++sub) {
        SDL_Serial_Absent(base, sub);
    }
}

bool SDL_Serial_IsPresent(const SDL_SerialBase *base, int sub)
{
    return sub >= 0 && sub < SDL_SERIAL_MAX_SUBDEVICES && base->snapshots[sub].present;
}

void SDL_Serial_Commit(SDL_SerialBase *base, int sub, const SDL_SerialControls *controls)
{
    SDL_SerialSnapshot *snapshot;
    SDL_SerialControls next;
    int button;

    if (!SDL_Serial_IsPresent(base, sub) || !controls) {
        return;
    }
    snapshot = &base->snapshots[sub];
    next = *controls;
    for (button = 0; button < SDL_SERIAL_MAX_BUTTONS; ++button) {
        if (base->pulse_end[sub][button]) {
            SDL_Serial_SetButton(&next, button, true);
        }
    }
    if (SDL_Serial_ControlsEqual(&snapshot->controls, &next) && next.ball[0] == 0 && next.ball[1] == 0) {
        return;
    }
    snapshot->controls = next;
    Serial_Emit(base, sub);
}

void SDL_Serial_Pulse(SDL_SerialBase *base, int sub, int button, uint64_t now)
{
    SDL_SerialSnapshot *snapshot;

    if (!SDL_Serial_IsPresent(base, sub) || button < 0 || button >= SDL_SERIAL_MAX_BUTTONS) {
        return;
    }
    snapshot = &base->snapshots[sub];
    if (base->pulse_end[sub][button] && SDL_Serial_GetButton(&snapshot->controls, button)) {
        SDL_Serial_SetButton(&snapshot->controls, button, false);
        Serial_Emit(base, sub);
    }
    SDL_Serial_SetButton(&snapshot->controls, button, true);
    Serial_Emit(base, sub);
    base->pulse_end[sub][button] = now + SDL_SERIAL_PULSE_MS;
}

void SDL_Serial_TickPulses(SDL_SerialBase *base, uint64_t now)
{
    int sub, button;

    for (sub = 0; sub < SDL_SERIAL_MAX_SUBDEVICES; ++sub) {
        bool released = false;

        for (button = 0; button < SDL_SERIAL_MAX_BUTTONS; ++button) {
            if (base->pulse_end[sub][button] && base->pulse_end[sub][button] <= now) {
                base->pulse_end[sub][button] = 0;
                SDL_Serial_SetButton(&base->snapshots[sub].controls, button, false);
                released = true;
            }
        }
        if (released && base->snapshots[sub].present) {
            Serial_Emit(base, sub);
        }
    }
}

void SDL_Serial_EarlierDeadline(bool *have, uint64_t *deadline, uint64_t candidate)
{
    if (!*have || candidate < *deadline) {
        *deadline = candidate;
        *have = true;
    }
}

void SDL_Serial_PulseDeadline(const SDL_SerialBase *base, bool *have, uint64_t *deadline)
{
    int sub, button;

    for (sub = 0; sub < SDL_SERIAL_MAX_SUBDEVICES; ++sub) {
        for (button = 0; button < SDL_SERIAL_MAX_BUTTONS; ++button) {
            if (base->pulse_end[sub][button]) {
                SDL_Serial_EarlierDeadline(have, deadline, base->pulse_end[sub][button]);
            }
        }
    }
}

void SDL_Serial_Log(SDL_SerialBase *base, const char *text)
{
    if (base->sink.log && text) {
        base->sink.log(base->sink.userdata, text);
    }
}

void SDL_Serial_SetIdentity(SDL_SerialIdentity *identity, const char *name, uint8_t type, int naxes, int nbuttons, int nhats, int nballs)
{
    size_t length = name ? strlen(name) : 0;

    memset(identity, 0, sizeof(*identity));
    if (length > SDL_SERIAL_NAME_LENGTH - 1) {
        length = SDL_SERIAL_NAME_LENGTH - 1;
    }
    if (length) {
        memcpy(identity->name, name, length);
    }
    identity->type = type;
    identity->naxes = (uint8_t)((naxes < 0) ? 0 : (naxes > SDL_SERIAL_MAX_AXES) ? SDL_SERIAL_MAX_AXES : naxes);
    identity->nbuttons = (uint8_t)((nbuttons < 0) ? 0 : (nbuttons > SDL_SERIAL_MAX_BUTTONS) ? SDL_SERIAL_MAX_BUTTONS : nbuttons);
    identity->nhats = (uint8_t)((nhats < 0) ? 0 : (nhats > SDL_SERIAL_MAX_HATS) ? SDL_SERIAL_MAX_HATS : nhats);
    identity->nballs = (uint8_t)((nballs < 0) ? 0 : (nballs > SDL_SERIAL_MAX_BALLS) ? SDL_SERIAL_MAX_BALLS : nballs);
}

SDL_SerialMapInput SDL_Serial_MapButton(int button)
{
    SDL_SerialMapInput input;

    input.kind = SDL_SERIAL_MAP_BUTTON;
    input.target = (uint8_t)button;
    return input;
}

SDL_SerialMapInput SDL_Serial_MapAxis(int axis)
{
    SDL_SerialMapInput input;

    input.kind = SDL_SERIAL_MAP_AXIS;
    input.target = (uint8_t)axis;
    return input;
}

SDL_SerialMapInput SDL_Serial_MapHat(int hat, uint8_t bit)
{
    SDL_SerialMapInput input;

    input.kind = SDL_SERIAL_MAP_HAT;
    input.target = (uint8_t)((hat << 4) | (bit & 0x0F));
    return input;
}

SDL_SerialLine SDL_Serial_Line(uint32_t rate, int data_bits, SDL_SerialParity parity, int stop_bits, SDL_SerialFlow flow, bool dtr, bool rts)
{
    SDL_SerialLine line;

    memset(&line, 0, sizeof(line));
    line.rate = rate;
    line.data_bits = (uint8_t)data_bits;
    line.parity = (uint8_t)parity;
    line.stop_bits = (uint8_t)stop_bits;
    line.flow = (uint8_t)flow;
    line.dtr = dtr;
    line.rts = rts;
    return line;
}
