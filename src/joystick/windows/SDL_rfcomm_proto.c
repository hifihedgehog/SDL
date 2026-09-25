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

/* Bluetooth RFCOMM controllers. See SDL_rfcomm_proto.h. */

#include "SDL_rfcomm_proto.h"

#include <string.h>

static char RFCOMM_Upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Stops at the name's end, since no prefix character is 0 */
bool SDL_RFCOMM_StartsWith(const char *name, const char *prefix, bool any_case)
{
    size_t i;

    if (!name || !prefix) {
        return false;
    }
    for (i = 0; prefix[i]; ++i) {
        const char a = any_case ? RFCOMM_Upper(name[i]) : name[i];
        const char b = any_case ? RFCOMM_Upper(prefix[i]) : prefix[i];

        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool RFCOMM_ContainsAnyCase(const char *name, const char *needle)
{
    size_t i;

    for (i = 0; name[i]; ++i) {
        if (SDL_RFCOMM_StartsWith(name + i, needle, true)) {
            return true;
        }
    }
    return false;
}

SDL_RFCOMMFamily SDL_RFCOMM_MatchName(const char *name)
{
    if (!name) {
        return SDL_RFCOMM_FAMILY_NONE;
    }
    /* The SteelSeries FREE pairs as "Zeemote: SteelSeries FREE" and works
       as a HID gamepad, so it never appears twice */
    if (SDL_RFCOMM_StartsWith(name, "Zeemote: SteelSeries", true)) {
        return SDL_RFCOMM_FAMILY_NONE;
    }
    /* A first generation MOGA, and then the second generation, whose Mode B
       names carry "HID" (moga-uinput) */
    if (SDL_RFCOMM_StartsWith(name, "BD&A", true) || SDL_RFCOMM_StartsWith(name, "BDA", true)) {
        return SDL_RFCOMM_FAMILY_MOGA;
    }
    if (SDL_RFCOMM_StartsWith(name, "MOGA", true) && !RFCOMM_ContainsAnyCase(name, "HID")) {
        return SDL_RFCOMM_FAMILY_MOGA;
    }
    /* A prefix, as zeemouse matches it, which also takes the JS1 H */
    if (SDL_RFCOMM_StartsWith(name, "Zeemote JS1", false)) {
        return SDL_RFCOMM_FAMILY_ZEEMOTE;
    }
    if (SDL_RFCOMM_StartsWith(name, "GAMEPAD", false)) {
        return SDL_RFCOMM_FAMILY_BGP100;
    }
    if (SDL_RFCOMM_StartsWith(name, "Phonejoy", true)) {
        return SDL_RFCOMM_FAMILY_PHONEJOY;
    }
    return SDL_RFCOMM_FAMILY_NONE;
}

void SDL_RFCOMM_CopyText(char *dst, size_t size, const char *src)
{
    size_t length = 0;

    if (!dst || size == 0) {
        return;
    }
    if (src) {
        while (length < size - 1 && src[length]) {
            ++length;
        }
        /* A cut inside a UTF-8 sequence backs up to the sequence's lead byte */
        if (src[length]) {
            while (length > 0 && ((unsigned char)src[length] & 0xC0) == 0x80) {
                --length;
            }
        }
        memcpy(dst, src, length);
    }
    dst[length] = '\0';
}

static void RFCOMM_CopyName(char *dst, const char *src)
{
    SDL_RFCOMM_CopyText(dst, SDL_RFCOMM_NAME_LENGTH, src);
}

int SDL_RFCOMM_SelectDevices(const SDL_RFCOMMPaired *paired, int count, const bool *enabled, SDL_RFCOMMDevice *devices, int max)
{
    int selected = 0, i, j;

    if (!paired || !enabled || !devices || count <= 0 || max <= 0) {
        return 0;
    }
    for (i = 0; i < count && selected < max; ++i) {
        char name[SDL_RFCOMM_NAME_LENGTH];
        SDL_RFCOMMFamily family;

        if (!paired[i].paired) {
            continue;
        }
        RFCOMM_CopyName(name, paired[i].name);
        family = SDL_RFCOMM_MatchName(name);
        if (family == SDL_RFCOMM_FAMILY_NONE || !enabled[family]) {
            continue;
        }
        for (j = 0; j < selected; ++j) {
            if (devices[j].address == paired[i].address) {
                break;
            }
        }
        if (j < selected) {
            continue;
        }
        devices[selected].address = paired[i].address;
        devices[selected].family = family;
        RFCOMM_CopyName(devices[selected].name, name);
        ++selected;
    }
    return selected;
}

static bool RFCOMM_SameDevice(const SDL_RFCOMMDevice *a, const SDL_RFCOMMDevice *b)
{
    return a->address == b->address && a->family == b->family && strcmp(a->name, b->name) == 0;
}

void SDL_RFCOMM_DiffDevices(const SDL_RFCOMMDevice *old_devices, int nold, const SDL_RFCOMMDevice *new_devices, int nnew, SDL_RFCOMMChange *old_changes, SDL_RFCOMMChange *new_changes)
{
    int i, j;

    for (i = 0; i < nold; ++i) {
        old_changes[i] = SDL_RFCOMM_STOP;
        for (j = 0; j < nnew; ++j) {
            if (RFCOMM_SameDevice(&old_devices[i], &new_devices[j])) {
                old_changes[i] = SDL_RFCOMM_KEEP;
                break;
            }
        }
    }
    for (j = 0; j < nnew; ++j) {
        new_changes[j] = SDL_RFCOMM_START;
        for (i = 0; i < nold; ++i) {
            if (RFCOMM_SameDevice(&old_devices[i], &new_devices[j])) {
                new_changes[j] = SDL_RFCOMM_KEEP;
                break;
            }
        }
    }
}

void SDL_RFCOMM_ResetBase(SDL_RFCOMMBase *base, const SDL_SerialSink *sink)
{
    SDL_Serial_ResetBase(&base->serial, sink);
    memset(base->out, 0, sizeof(base->out));
    memset(base->out_length, 0, sizeof(base->out_length));
    base->out_count = 0;
    base->end = false;
    base->battery_mv = -1;
}

bool SDL_RFCOMM_Send(SDL_RFCOMMBase *base, const uint8_t *data, size_t length)
{
    if (!data || length == 0 || length > SDL_RFCOMM_MAX_SEND || base->out_count >= SDL_RFCOMM_MAX_OUT) {
        return false;
    }
    memcpy(base->out[base->out_count], data, length);
    base->out_length[base->out_count] = (uint8_t)length;
    ++base->out_count;
    return true;
}

void SDL_RFCOMM_End(SDL_RFCOMMBase *base)
{
    base->end = true;
}

static SDL_RFCOMMAction *RFCOMM_Append(SDL_RFCOMMLink *link, SDL_RFCOMMActionKind kind)
{
    SDL_RFCOMMAction *action;

    if (link->action_count >= SDL_RFCOMM_MAX_ACTIONS) {
        ++link->dropped_actions;
        return NULL;
    }
    action = &link->actions[(link->action_head + link->action_count) % SDL_RFCOMM_MAX_ACTIONS];
    memset(action, 0, sizeof(*action));
    action->kind = kind;
    ++link->action_count;
    return action;
}

static SDL_RFCOMMBase *RFCOMM_Base(const SDL_RFCOMMLink *link)
{
    return (SDL_RFCOMMBase *)link->state;
}

/* Connecting, or waiting another retry period when the queue is full */
static void RFCOMM_Connect(SDL_RFCOMMLink *link, uint8_t channel, uint64_t now)
{
    SDL_RFCOMMAction *action = RFCOMM_Append(link, SDL_RFCOMM_ACTION_CONNECT);

    if (!action) {
        link->phase = SDL_RFCOMM_WAITING;
        link->connect_at = now + SDL_RFCOMM_RETRY_MS;
        return;
    }
    action->channel = channel;
    link->channel = channel;
    link->phase = SDL_RFCOMM_CONNECTING;
    ++link->connects;
}

/* The link went down. Queued sends belonged to the old socket and go. */
static void RFCOMM_Down(SDL_RFCOMMLink *link, bool close, uint64_t now)
{
    link->action_head = 0;
    link->action_count = 0;
    if (close) {
        RFCOMM_Append(link, SDL_RFCOMM_ACTION_CLOSE);
    }
    SDL_Serial_AbsentAll(&RFCOMM_Base(link)->serial);
    link->phase = SDL_RFCOMM_WAITING;
    link->connect_at = now + SDL_RFCOMM_RETRY_MS;
}

/* Moves the module's sends to the action queue, then ends the link when the
   module asked */
static void RFCOMM_Collect(SDL_RFCOMMLink *link, uint64_t now)
{
    SDL_RFCOMMBase *base = RFCOMM_Base(link);
    int i;

    for (i = 0; i < base->out_count; ++i) {
        SDL_RFCOMMAction *action = RFCOMM_Append(link, SDL_RFCOMM_ACTION_SEND);

        if (action) {
            memcpy(action->data, base->out[i], base->out_length[i]);
            action->length = base->out_length[i];
        }
    }
    base->out_count = 0;
    if (base->end) {
        base->end = false;
        RFCOMM_Down(link, true, now);
    }
}

void SDL_RFCOMMLink_Init(SDL_RFCOMMLink *link, const SDL_RFCOMMModule *module, void *state, const SDL_SerialSink *sink, const char *name, uint64_t now)
{
    memset(link, 0, sizeof(*link));
    link->module = module;
    link->state = state;
    link->sink = *sink;
    RFCOMM_CopyName(link->name, name ? name : "");
    link->player_index = -1;
    memset(state, 0, module->state_size);
    SDL_RFCOMM_ResetBase((SDL_RFCOMMBase *)state, sink);
    link->phase = SDL_RFCOMM_WAITING;
    link->connect_at = now;
}

void SDL_RFCOMMLink_Connected(SDL_RFCOMMLink *link, SDL_RFCOMMConnectResult result, uint64_t now)
{
    if (link->phase != SDL_RFCOMM_CONNECTING) {
        return;
    }
    if (result == SDL_RFCOMM_CONNECT_OK) {
        link->phase = SDL_RFCOMM_UP;
        link->module->Reset(link->state, &link->sink, link->name, link->player_index, now);
        RFCOMM_Collect(link, now);
        return;
    }
    if (link->channel == 0 && result == SDL_RFCOMM_CONNECT_FAILED) {
        /* The service lookup can fail where the channel works */
        RFCOMM_Connect(link, SDL_RFCOMM_CHANNEL, now);
        return;
    }
    link->phase = SDL_RFCOMM_WAITING;
    link->connect_at = now + SDL_RFCOMM_RETRY_MS;
}

void SDL_RFCOMMLink_Received(SDL_RFCOMMLink *link, const uint8_t *data, size_t length, uint64_t now)
{
    if (link->phase != SDL_RFCOMM_UP || !data || length == 0) {
        return;
    }
    link->module->Feed(link->state, data, length, now);
    RFCOMM_Collect(link, now);
}

void SDL_RFCOMMLink_Lost(SDL_RFCOMMLink *link, uint64_t now)
{
    if (link->phase == SDL_RFCOMM_UP || link->phase == SDL_RFCOMM_CONNECTING) {
        RFCOMM_Down(link, false, now);
    }
}

void SDL_RFCOMMLink_Tick(SDL_RFCOMMLink *link, uint64_t now)
{
    switch (link->phase) {
    case SDL_RFCOMM_WAITING:
        if (now >= link->connect_at) {
            RFCOMM_Connect(link, link->module->has_service ? 0 : SDL_RFCOMM_CHANNEL, now);
        }
        break;
    case SDL_RFCOMM_UP:
        link->module->Tick(link->state, now);
        RFCOMM_Collect(link, now);
        break;
    default:
        break;
    }
}

bool SDL_RFCOMMLink_GetDeadline(const SDL_RFCOMMLink *link, uint64_t *deadline)
{
    switch (link->phase) {
    case SDL_RFCOMM_WAITING:
        *deadline = link->connect_at;
        return true;
    case SDL_RFCOMM_UP:
        return link->module->GetDeadline(link->state, deadline);
    default:
        return false;
    }
}

bool SDL_RFCOMMLink_NextAction(SDL_RFCOMMLink *link, SDL_RFCOMMAction *action)
{
    if (link->action_count == 0) {
        return false;
    }
    *action = link->actions[link->action_head];
    link->action_head = (link->action_head + 1) % SDL_RFCOMM_MAX_ACTIONS;
    --link->action_count;
    return true;
}

void SDL_RFCOMMLink_SetPlayerIndex(SDL_RFCOMMLink *link, int player_index, uint64_t now)
{
    link->player_index = player_index;
    if (link->phase == SDL_RFCOMM_UP && link->module->SetPlayerIndex) {
        link->module->SetPlayerIndex(link->state, player_index, now);
        RFCOMM_Collect(link, now);
    }
}

void SDL_RFCOMMLink_Stop(SDL_RFCOMMLink *link)
{
    const bool open = (link->phase == SDL_RFCOMM_UP || link->phase == SDL_RFCOMM_CONNECTING);

    link->action_head = 0;
    link->action_count = 0;
    if (open) {
        RFCOMM_Append(link, SDL_RFCOMM_ACTION_CLOSE);
    }
    SDL_Serial_AbsentAll(&RFCOMM_Base(link)->serial);
    link->phase = SDL_RFCOMM_STOPPED;
}

static int32_t RFCOMM_Signed8(uint8_t value)
{
    return (value < 0x80) ? (int32_t)value : (int32_t)value - 256;
}

int16_t SDL_RFCOMM_AxisFromS8(uint8_t value)
{
    return SDL_Serial_ScaleSigned(RFCOMM_Signed8(value), -128, 127);
}

int16_t SDL_RFCOMM_AxisFromS8Negated(uint8_t value)
{
    return SDL_Serial_ScaleSigned(-RFCOMM_Signed8(value), -127, 128);
}

int16_t SDL_RFCOMM_TriggerFromU8(uint8_t value)
{
    return (int16_t)((int32_t)value * 257 - 32768);
}
