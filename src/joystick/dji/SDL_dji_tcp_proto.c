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

#include "SDL_dji_tcp_proto.h"

#include <stdio.h>
#include <string.h>

static SDL_DJITCPAction *DJITCP_Append(SDL_DJITCPState *s, SDL_DJITCPActionKind kind, int link)
{
    SDL_DJITCPAction *action;

    if (s->action_count >= SDL_DJI_TCP_MAX_ACTIONS) {
        ++s->dropped_actions;
        return NULL;
    }
    action = &s->actions[(s->action_head + s->action_count) % SDL_DJI_TCP_MAX_ACTIONS];
    memset(action, 0, sizeof(*action));
    action->kind = kind;
    action->link = link;
    ++s->action_count;
    return action;
}

static void DJITCP_Send(SDL_DJITCPState *s, int link, const uint8_t *frame, size_t length)
{
    SDL_DJITCPAction *action;
    uint8_t envelope[SDL_DJI_TCP_MAX_SEND];
    const size_t total = SDL_DJI_BuildEnvelope(envelope, sizeof(envelope), frame, length);

    if (total == 0) {
        return;
    }
    action = DJITCP_Append(s, SDL_DJI_TCP_SEND, link);
    if (action) {
        memcpy(action->data, envelope, total);
        action->length = total;
    }
}

/* The General Version Inquiry keepalive: 02 to 06, type 00, 00/01 */
static void DJITCP_Keepalive(SDL_DJITCPState *s, int link, uint64_t now)
{
    SDL_DJILink *l = &s->links[link];
    uint8_t frame[SDL_DJI_MIN_FRAME];
    const size_t length = SDL_DJI_BuildFrame(frame, sizeof(frame), SDL_DJI_MODULE_APP, SDL_DJI_MODULE_RC,
                                             l->sequence, 0x00, 0x00, 0x01, NULL, 0);

    ++l->sequence;
    DJITCP_Send(s, link, frame, length);
    l->keepalive_at = now + SDL_DJI_TCP_KEEPALIVE_MS;
}

/* Starts links until count are in the pool, spread across the session span */
static void DJITCP_Grow(SDL_DJITCPState *s, int count, uint64_t now)
{
    const uint64_t step = SDL_DJI_TCP_SESSION_MS / (uint64_t)count;

    for (; s->running < count; ++s->running) {
        SDL_DJILink *l = &s->links[s->running];

        l->phase = SDL_DJI_LINK_WAITING;
        l->connect_at = now + (uint64_t)s->running * step;
        l->sequence = 1;
    }
}

/* A link that was up went down. It redials at once if it brought a 06/AE
 * frame since it connected, else after the redial wait. */
static void DJITCP_Down(SDL_DJITCPState *s, int link, uint64_t now)
{
    SDL_DJILink *l = &s->links[link];

    l->phase = SDL_DJI_LINK_WAITING;
    l->connect_at = l->carried ? now : now + SDL_DJI_TCP_REDIAL_MS;
}

static void DJITCP_Identity(const SDL_DJITCPState *s, SDL_SerialIdentity *identity)
{
    SDL_Serial_SetIdentity(identity, SDL_DJI_NameForModel(s->model, "DJI Remote"), SDL_SERIAL_TYPE_GAMEPAD,
                           6, SDL_DJI_TCP_BUTTONS, 0, 0);
    identity->has_mapping = true;
    identity->mapping.leftx = SDL_Serial_MapAxis(SDL_DJI_AXIS_LEFT_X);
    identity->mapping.lefty = SDL_Serial_MapAxis(SDL_DJI_AXIS_LEFT_Y);
    identity->mapping.rightx = SDL_Serial_MapAxis(SDL_DJI_AXIS_RIGHT_X);
    identity->mapping.righty = SDL_Serial_MapAxis(SDL_DJI_AXIS_RIGHT_Y);
    identity->mapping.a = SDL_Serial_MapButton(SDL_DJI_BUTTON_SHUTTER);
    identity->mapping.x = SDL_Serial_MapButton(SDL_DJI_BUTTON_RECORD);
    identity->mapping.leftshoulder = SDL_Serial_MapButton(SDL_DJI_BUTTON_C1);
    identity->mapping.rightshoulder = SDL_Serial_MapButton(SDL_DJI_BUTTON_C2);
    identity->mapping.guide = SDL_Serial_MapButton(SDL_DJI_BUTTON_HOME);
    identity->mapping.y = SDL_Serial_MapHalfAxis(SDL_DJI_AXIS_DIAL, true);
    identity->mapping.b = SDL_Serial_MapHalfAxis(SDL_DJI_AXIS_DIAL, false);
}

bool SDL_DJITCP_DecodeControls(const uint8_t *payload, size_t length, SDL_SerialControls *controls)
{
    uint16_t buttons, mode;
    int i;

    if (!payload || !controls || length < SDL_DJI_TCP_AE_LENGTH) {
        return false;
    }
    buttons = (uint16_t)(payload[0] | (payload[1] << 8));
    mode = (uint16_t)(payload[2] | (payload[3] << 8));

    /* Each value is 1024 at rest and 660 away at full deflection. The
       channel values run as on the other DJI transports, so the vertical
       axes are negated the same way. */
    controls->axes[SDL_DJI_AXIS_RIGHT_X] = SDL_DJI_ScaleAxis(payload[5] | (payload[6] << 8), 1024, 660);
    controls->axes[SDL_DJI_AXIS_RIGHT_Y] = (int16_t)-SDL_DJI_ScaleAxis(payload[7] | (payload[8] << 8), 1024, 660);
    controls->axes[SDL_DJI_AXIS_LEFT_Y] = (int16_t)-SDL_DJI_ScaleAxis(payload[9] | (payload[10] << 8), 1024, 660);
    controls->axes[SDL_DJI_AXIS_LEFT_X] = SDL_DJI_ScaleAxis(payload[11] | (payload[12] << 8), 1024, 660);
    controls->axes[SDL_DJI_AXIS_DIAL] = SDL_DJI_ScaleAxis(payload[13] | (payload[14] << 8), 1024, 660);
    controls->axes[SDL_DJI_AXIS_DIAL2] = SDL_DJI_ScaleAxis(payload[15] | (payload[16] << 8), 1024, 660);

    SDL_Serial_SetButton(controls, SDL_DJI_BUTTON_HOME, (buttons & 0x0020) != 0);
    SDL_Serial_SetButton(controls, SDL_DJI_BUTTON_SHUTTER, (buttons & 0x0040) != 0);
    SDL_Serial_SetButton(controls, SDL_DJI_BUTTON_SHUTTER_HALF, (buttons & 0x0080) != 0);
    SDL_Serial_SetButton(controls, SDL_DJI_BUTTON_RECORD, (buttons & 0x0100) != 0);
    SDL_Serial_SetButton(controls, SDL_DJI_BUTTON_C1, (mode & 0x0004) != 0);
    SDL_Serial_SetButton(controls, SDL_DJI_BUTTON_C2, (mode & 0x0008) != 0);
    /* Bits 0-1: 0 Sport, 1 Normal, 2 Cinema. 3 names none. */
    for (i = 0; i < 3; ++i) {
        SDL_Serial_SetButton(controls, SDL_DJI_BUTTON_MODE + i, (mode & 0x0003) == (uint16_t)i);
    }
    return true;
}

void SDL_DJITCP_Init(SDL_DJITCPState *s, const SDL_SerialSink *sink, uint64_t now)
{
    int i;

    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->battery = -1;
    for (i = 0; i < SDL_DJI_TCP_MAX_LINKS; ++i) {
        SDL_DJIParser_Init(&s->links[i].parser, true);
    }
    DJITCP_Grow(s, 1, now);
}

bool SDL_DJITCP_NextAction(SDL_DJITCPState *s, SDL_DJITCPAction *action)
{
    if (s->action_count == 0) {
        return false;
    }
    *action = s->actions[s->action_head];
    s->action_head = (s->action_head + 1) % SDL_DJI_TCP_MAX_ACTIONS;
    --s->action_count;
    return true;
}

void SDL_DJITCP_Connected(SDL_DJITCPState *s, int link, bool success, uint64_t now)
{
    SDL_DJILink *l;

    if (link < 0 || link >= s->running || s->links[link].phase != SDL_DJI_LINK_CONNECTING) {
        return;
    }
    l = &s->links[link];
    if (!success) {
        l->phase = SDL_DJI_LINK_WAITING;
        l->connect_at = now + SDL_DJI_TCP_REDIAL_MS;
        return;
    }
    l->phase = SDL_DJI_LINK_UP;
    l->connected_at = now;
    l->carried = false;
    SDL_DJIParser_Init(&l->parser, true);
    /* The first keepalive right away, to start the stream */
    if (!s->quiet) {
        DJITCP_Keepalive(s, link, now);
    }
}

typedef struct DJITCP_Batch
{
    SDL_DJITCPState *state;
    int link;
    bool have_controls;
    uint8_t controls[SDL_DJI_TCP_AE_LENGTH];
} DJITCP_Batch;

static void DJITCP_OnFrame(void *userdata, const SDL_DJIFrame *frame)
{
    DJITCP_Batch *batch = (DJITCP_Batch *)userdata;
    SDL_DJITCPState *s = batch->state;
    char model[SDL_DJI_MODEL_LENGTH];

    /* A request that asks for an acknowledgement gets an empty response */
    if (!(frame->type & SDL_DJI_TYPE_RESPONSE) && (frame->type & SDL_DJI_TYPE_ACK_MASK)) {
        uint8_t response[SDL_DJI_MIN_FRAME];
        const size_t length = SDL_DJI_BuildResponse(response, sizeof(response), frame);

        DJITCP_Send(s, batch->link, response, length);
    }
    if (SDL_DJI_DecodeModel(frame, model, sizeof(model))) {
        memcpy(s->model, model, sizeof(model));
        return;
    }
    if (frame->set == 0x06 && frame->id == 0xAE && frame->payload_length >= SDL_DJI_TCP_AE_LENGTH) {
        /* Only the last controls frame of one read counts */
        memcpy(batch->controls, frame->payload, SDL_DJI_TCP_AE_LENGTH);
        batch->have_controls = true;
    } else if (frame->set == 0x06 && frame->id == 0x1E && frame->payload_length >= 5 && frame->payload[4] <= 100) {
        s->battery = frame->payload[4];
    }
}

void SDL_DJITCP_Received(SDL_DJITCPState *s, int link, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_DJILink *l;
    DJITCP_Batch batch;

    if (link < 0 || link >= s->running || s->links[link].phase != SDL_DJI_LINK_UP || !data || length == 0) {
        return;
    }
    l = &s->links[link];
    l->keepalive_at = now + SDL_DJI_TCP_KEEPALIVE_MS;

    batch.state = s;
    batch.link = link;
    batch.have_controls = false;
    SDL_DJIParser_Feed(&l->parser, data, length, DJITCP_OnFrame, &batch);

    /* The DJI RC 2 wants five quiet links */
    if (!s->quiet && strcmp(s->model, "rc331") == 0) {
        s->quiet = true;
        DJITCP_Grow(s, SDL_DJI_TCP_MAX_LINKS, now);
    }

    if (batch.have_controls) {
        l->carried = true;
        s->last_frame = now;
        SDL_DJITCP_DecodeControls(batch.controls, sizeof(batch.controls), &s->controls);
        if (!s->live) {
            SDL_SerialIdentity identity;

            s->live = true;
            DJITCP_Identity(s, &identity);
            SDL_Serial_Present(&s->base, 0, &identity);
        }
        SDL_Serial_Commit(&s->base, 0, &s->controls);
    }
}

void SDL_DJITCP_Lost(SDL_DJITCPState *s, int link, uint64_t now)
{
    SDL_DJILink *l;

    if (link < 0 || link >= s->running) {
        return;
    }
    l = &s->links[link];
    if (l->phase == SDL_DJI_LINK_UP) {
        DJITCP_Down(s, link, now);
    } else if (l->phase == SDL_DJI_LINK_CONNECTING) {
        l->phase = SDL_DJI_LINK_WAITING;
        l->connect_at = now + SDL_DJI_TCP_REDIAL_MS;
    }
}

/* A link closes 1000 ms after the later of its connect and the last 06/AE
 * frame on any link */
static uint64_t DJITCP_Expiry(const SDL_DJITCPState *s, const SDL_DJILink *l)
{
    const uint64_t since = (s->last_frame > l->connected_at) ? s->last_frame : l->connected_at;

    return since + SDL_DJI_TCP_LIVENESS_MS;
}

void SDL_DJITCP_Tick(SDL_DJITCPState *s, uint64_t now)
{
    int i;

    if (s->live && now >= s->last_frame + SDL_DJI_TCP_LIVENESS_MS) {
        s->live = false;
        SDL_Serial_Absent(&s->base, 0);
    }
    for (i = 0; i < s->running; ++i) {
        SDL_DJILink *l = &s->links[i];

        switch (l->phase) {
        case SDL_DJI_LINK_WAITING:
            if (now >= l->connect_at && DJITCP_Append(s, SDL_DJI_TCP_CONNECT, i)) {
                l->phase = SDL_DJI_LINK_CONNECTING;
                l->connect_deadline = now + SDL_DJI_TCP_CONNECT_MS;
            }
            break;
        case SDL_DJI_LINK_CONNECTING:
            if (now >= l->connect_deadline) {
                DJITCP_Append(s, SDL_DJI_TCP_CLOSE, i);
                l->phase = SDL_DJI_LINK_WAITING;
                l->connect_at = now + SDL_DJI_TCP_REDIAL_MS;
            }
            break;
        case SDL_DJI_LINK_UP:
            if (now >= DJITCP_Expiry(s, l)) {
                DJITCP_Append(s, SDL_DJI_TCP_CLOSE, i);
                DJITCP_Down(s, i, now);
            } else if (!s->quiet && now >= l->keepalive_at) {
                DJITCP_Keepalive(s, i, now);
            }
            break;
        default:
            break;
        }
    }
}

/* A decimal number of 1 to max_digits digits, no leading zero, at most max */
static bool DJITCP_ParseNumber(const char *text, size_t length, size_t max_digits, uint32_t max, uint32_t *value)
{
    uint32_t result = 0;
    size_t i;

    if (length == 0 || length > max_digits || (length > 1 && text[0] == '0')) {
        return false;
    }
    for (i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        result = result * 10 + (uint32_t)(text[i] - '0');
    }
    if (result > max) {
        return false;
    }
    *value = result;
    return true;
}

static bool DJITCP_ParseHost(const char *entry, size_t length, SDL_DJITCPHost *host)
{
    uint32_t address = 0, value = 0, port = SDL_DJI_TCP_PORT;
    size_t position = 0, end;
    int octet;

    for (octet = 0; octet < 4; ++octet) {
        const char separator = (octet < 3) ? '.' : ':';

        for (end = position; end < length && entry[end] != separator; ++end) {
        }
        if (octet < 3 && end == length) {
            return false;
        }
        if (!DJITCP_ParseNumber(entry + position, end - position, 3, 255, &value)) {
            return false;
        }
        address = (address << 8) | value;
        position = end + 1;
    }
    if (position <= length) {
        /* A port follows the colon */
        if (!DJITCP_ParseNumber(entry + position, length - position, 5, 65535, &port) || port == 0) {
            return false;
        }
    }
    host->address = address;
    host->port = (uint16_t)port;
    (void)snprintf(host->key, sizeof(host->key), "%u.%u.%u.%u:%u", (unsigned)(address >> 24), (unsigned)((address >> 16) & 0xFF),
                   (unsigned)((address >> 8) & 0xFF), (unsigned)(address & 0xFF), (unsigned)port);
    return true;
}

int SDL_DJITCP_ParseHosts(const char *hint, SDL_DJITCPHost *hosts, int max, SDL_DJITCPHostLog log, void *userdata)
{
    const char *p = hint;
    int count = 0;

    if (!hint || !hosts || max <= 0) {
        return 0;
    }
    while (*p) {
        const char *start, *end;
        SDL_DJITCPHost host;
        int i;

        while (*p == ' ') {
            ++p;
        }
        start = p;
        while (*p && *p != ',') {
            ++p;
        }
        end = p;
        if (*p == ',') {
            ++p;
        }
        while (end > start && end[-1] == ' ') {
            --end;
        }
        if (end == start) {
            continue; /* An empty entry, as after a trailing comma */
        }
        if (!DJITCP_ParseHost(start, (size_t)(end - start), &host)) {
            if (log) {
                log(userdata, start, (size_t)(end - start), "not an IPv4 address with an optional port");
            }
            continue;
        }
        for (i = 0; i < count; ++i) {
            if (hosts[i].address == host.address && hosts[i].port == host.port) {
                break;
            }
        }
        if (i < count) {
            continue;
        }
        if (count >= max) {
            if (log) {
                log(userdata, start, (size_t)(end - start), "too many hosts");
            }
            continue;
        }
        hosts[count++] = host;
    }
    return count;
}

bool SDL_DJITCP_GetDeadline(const SDL_DJITCPState *s, uint64_t *deadline)
{
    bool have = false;
    uint64_t earliest = 0;
    int i;

    if (s->live) {
        SDL_Serial_EarlierDeadline(&have, &earliest, s->last_frame + SDL_DJI_TCP_LIVENESS_MS);
    }
    for (i = 0; i < s->running; ++i) {
        const SDL_DJILink *l = &s->links[i];

        switch (l->phase) {
        case SDL_DJI_LINK_WAITING:
            SDL_Serial_EarlierDeadline(&have, &earliest, l->connect_at);
            break;
        case SDL_DJI_LINK_CONNECTING:
            SDL_Serial_EarlierDeadline(&have, &earliest, l->connect_deadline);
            break;
        case SDL_DJI_LINK_UP:
            SDL_Serial_EarlierDeadline(&have, &earliest, DJITCP_Expiry(s, l));
            if (!s->quiet) {
                SDL_Serial_EarlierDeadline(&have, &earliest, l->keepalive_at);
            }
            break;
        default:
            break;
        }
    }
    if (have) {
        *deadline = earliest;
    }
    return have;
}
