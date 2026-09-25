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

#include "SDL_serial_stinger_proto.h"

#include <string.h>

static const uint8_t stinger_enable[5] = { ' ', 'E', '5', 'E', '5' };
static const uint8_t stinger_reply[SDL_STINGER_REPLY_LENGTH] = {
    0x0D, 0x0A, '0', '6', '0', '0', '5', '2', '0', '0', '5', '8', 'C', '2', '7', '2'
};

void SDL_Stinger_DecodePacket(const uint8_t *p, int *x, int *y, uint16_t *buttons)
{
    uint16_t b = 0;

    /* Byte 0 bits 5-2: A, B, C, X. Byte 3 bits 5-0: Y, Z, TL, TR, Select, Start. */
    if (p[0] & 0x20) {
        b |= 1 << 0;
    }
    if (p[0] & 0x10) {
        b |= 1 << 1;
    }
    if (p[0] & 0x08) {
        b |= 1 << 2;
    }
    if (p[0] & 0x04) {
        b |= 1 << 3;
    }
    if (p[3] & 0x20) {
        b |= 1 << 4;
    }
    if (p[3] & 0x10) {
        b |= 1 << 5;
    }
    if (p[3] & 0x08) {
        b |= 1 << 6;
    }
    if (p[3] & 0x04) {
        b |= 1 << 7;
    }
    if (p[3] & 0x02) {
        b |= 1 << 8;
    }
    if (p[3] & 0x01) {
        b |= 1 << 9;
    }
    *buttons = b;
    /* X is its magnitude less 64 when byte 0 bit 0 is set. Y is 64 when
     * byte 0 bit 1 is set, less its magnitude. */
    *x = (p[1] & 0x3F) - ((p[0] & 0x01) ? 64 : 0);
    *y = ((p[0] & 0x02) ? 64 : 0) - (p[2] & 0x3F);
}

/* Discard what arrives until 100 ms of silence, and until not_before */
static void Stinger_Drain(SDL_StingerState *s, uint64_t not_before, uint64_t now)
{
    SDL_Serial_ClearActions(&s->base);
    s->step = SDL_STINGER_STEP_DRAIN;
    s->not_before = not_before;
    s->last_byte = now;
    s->reply_timer = false;
    s->waiting_seq = 0;
}

static uint64_t Stinger_DrainEnd(const SDL_StingerState *s)
{
    const uint64_t quiet = s->last_byte + SDL_STINGER_DRAIN_MS;

    return (quiet > s->not_before) ? quiet : s->not_before;
}

static void Stinger_Present(SDL_StingerState *s)
{
    SDL_SerialIdentity identity;

    SDL_Serial_SetIdentity(&identity, "Gravis Stinger", SDL_SERIAL_TYPE_GAMEPAD, SDL_STINGER_AXES, SDL_STINGER_BUTTONS, 0, 0);
    /* A South, B East, X West, Y North. C and Z go to the right and left
     * shoulders, TL and TR to the triggers. */
    identity.has_mapping = true;
    identity.mapping.a = SDL_Serial_MapButton(0);
    identity.mapping.b = SDL_Serial_MapButton(1);
    identity.mapping.rightshoulder = SDL_Serial_MapButton(2);
    identity.mapping.x = SDL_Serial_MapButton(3);
    identity.mapping.y = SDL_Serial_MapButton(4);
    identity.mapping.leftshoulder = SDL_Serial_MapButton(5);
    identity.mapping.lefttrigger = SDL_Serial_MapButton(6);
    identity.mapping.righttrigger = SDL_Serial_MapButton(7);
    identity.mapping.back = SDL_Serial_MapButton(8);
    identity.mapping.start = SDL_Serial_MapButton(9);
    identity.mapping.leftx = SDL_Serial_MapAxis(0);
    identity.mapping.lefty = SDL_Serial_MapAxis(1);
    s->step = SDL_STINGER_STEP_PRESENT;
    s->reply_timer = false;
    s->packet_length = 0;
    SDL_Serial_Present(&s->base, 0, &identity);
}

static void Stinger_Decode(SDL_StingerState *s)
{
    SDL_SerialControls controls = s->base.snapshots[0].controls;
    uint16_t buttons;
    int x, y, i;

    SDL_Stinger_DecodePacket(s->packet, &x, &y, &buttons);
    controls.axes[0] = SDL_Serial_ScaleSigned(x, -64, 63);
    controls.axes[1] = SDL_Serial_ScaleSigned(y, -63, 64);
    for (i = 0; i < SDL_STINGER_BUTTONS; ++i) {
        SDL_Serial_SetButton(&controls, i, (buttons & (1 << i)) != 0);
    }
    SDL_Serial_Commit(&s->base, 0, &controls);
}

static void Stinger_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_StingerState *s = (SDL_StingerState *)state;
    const SDL_SerialLine line = SDL_Serial_Line(SDL_STINGER_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);
    const uint8_t write_seq = s->write_seq;

    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->write_seq = write_seq;
    SDL_Serial_QueueLine(&s->base, 0, &line);
    s->step = SDL_STINGER_STEP_DRAIN;
    s->not_before = now;
    s->last_byte = now;
}

static void Stinger_Fail(SDL_StingerState *s, uint64_t now)
{
    Stinger_Drain(s, now + SDL_STINGER_RETRY_MS, now);
}

static void Stinger_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_StingerState *s = (SDL_StingerState *)state;
    size_t i;

    for (i = 0; i < length; ++i) {
        const uint8_t byte = data[i];

        switch (s->step) {
        case SDL_STINGER_STEP_DRAIN:
            s->last_byte = now;
            break;
        case SDL_STINGER_STEP_REPLY:
            if (byte != stinger_reply[s->reply_index]) {
                Stinger_Fail(s, now);
                break;
            }
            if (++s->reply_index == SDL_STINGER_REPLY_LENGTH) {
                Stinger_Present(s);
                break;
            }
            s->reply_timer = true;
            s->reply_deadline = now + SDL_STINGER_BYTE_MS;
            break;
        case SDL_STINGER_STEP_PRESENT:
            s->packet[s->packet_length++] = byte;
            if (s->packet_length == 4) {
                Stinger_Decode(s);
                s->packet_length = 0;
            }
            break;
        }
    }
}

static void Stinger_Tick(void *state, uint64_t now)
{
    SDL_StingerState *s = (SDL_StingerState *)state;

    switch (s->step) {
    case SDL_STINGER_STEP_DRAIN:
        if (now >= Stinger_DrainEnd(s)) {
            if (++s->write_seq == 0) {
                s->write_seq = 1;
            }
            SDL_Serial_QueueWrite(&s->base, s->write_seq, stinger_enable, sizeof(stinger_enable));
            s->waiting_seq = s->write_seq;
            s->step = SDL_STINGER_STEP_REPLY;
            s->reply_index = 0;
            s->reply_timer = false;
        }
        break;
    case SDL_STINGER_STEP_REPLY:
        if (s->reply_timer && now >= s->reply_deadline) {
            Stinger_Fail(s, now);
        }
        break;
    default:
        break;
    }
}

static void Stinger_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_StingerState *s = (SDL_StingerState *)state;
    uint8_t tag;

    if (!SDL_Serial_FinishAction(&s->base, &tag) || tag == 0 || tag != s->waiting_seq) {
        return;
    }
    s->waiting_seq = 0;
    if (!success) {
        Stinger_Fail(s, now);
        return;
    }
    /* The first byte of the answer is due 200 ms after the write */
    if (s->step == SDL_STINGER_STEP_REPLY && s->reply_index == 0) {
        s->reply_timer = true;
        s->reply_deadline = now + SDL_STINGER_BYTE_MS;
    }
}

static void Stinger_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

static bool Stinger_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_StingerState *s = (SDL_StingerState *)state;

    switch (s->step) {
    case SDL_STINGER_STEP_DRAIN:
        *deadline = Stinger_DrainEnd(s);
        return true;
    case SDL_STINGER_STEP_REPLY:
        if (s->reply_timer) {
            *deadline = s->reply_deadline;
            return true;
        }
        return false;
    default:
        return false;
    }
}

const SDL_SerialModule SDL_SerialStingerModule = {
    "stinger",
    sizeof(SDL_StingerState),
    false,
    0,
    0,
    Stinger_Reset,
    Stinger_Feed,
    Stinger_Tick,
    SDL_Serial_NextAction,
    Stinger_ActionDone,
    Stinger_Output,
    Stinger_GetDeadline,
    SDL_Serial_GetSnapshot
};
