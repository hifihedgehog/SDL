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

#include "SDL_serial_warrior_proto.h"

#include <string.h>

int SDL_Warrior_PacketLength(uint8_t first)
{
    /* Types 0 to 7 in bits 4-6 */
    static const int lengths[8] = { 0, 4, 12, 3, 4, 4, 0, 0 };

    return lengths[(first >> 4) & 7];
}

static SDL_SerialLine Warrior_HandshakeLine(void)
{
    return SDL_Serial_Line(SDL_WARRIOR_HANDSHAKE_RATE, 7, SDL_SERIAL_PARITY_NONE, 2, SDL_SERIAL_FLOW_NONE, true, true);
}

static uint8_t Warrior_NextSeq(SDL_WarriorState *s)
{
    if (++s->action_seq == 0) {
        s->action_seq = 1;
    }
    return s->action_seq;
}

static void Warrior_Drain(SDL_WarriorState *s, uint64_t not_before, uint64_t now)
{
    s->step = SDL_WARRIOR_STEP_DRAIN;
    s->not_before = not_before;
    s->last_byte = now;
    s->echo_timer = false;
    s->waiting_seq = 0;
}

static uint64_t Warrior_DrainEnd(const SDL_WarriorState *s)
{
    const uint64_t quiet = s->last_byte + SDL_WARRIOR_DRAIN_MS;

    return (quiet > s->not_before) ? quiet : s->not_before;
}

static void Warrior_Fail(SDL_WarriorState *s, uint64_t now)
{
    SDL_Serial_ClearActions(&s->base);
    Warrior_Drain(s, now + SDL_WARRIOR_RETRY_MS, now);
}

static void Warrior_WriteByte(SDL_WarriorState *s, uint8_t byte, SDL_WarriorStep next)
{
    s->waiting_seq = Warrior_NextSeq(s);
    SDL_Serial_QueueWrite(&s->base, s->waiting_seq, &byte, 1);
    s->step = next;
    s->echo_timer = false;
}

static void Warrior_Present(SDL_WarriorState *s)
{
    SDL_SerialIdentity identity;

    SDL_Serial_SetIdentity(&identity, "Logitech WingMan Warrior", SDL_SERIAL_TYPE_FLIGHT_STICK,
                           SDL_WARRIOR_AXES, SDL_WARRIOR_BUTTONS, 1, 1);
    s->step = SDL_WARRIOR_STEP_PRESENT;
    s->packet_length = 0;
    s->packet_expected = 0;
    SDL_Serial_Present(&s->base, 0, &identity);
}

/* A value from a low byte, its bit 7 in one bit of the first byte and a
 * bit that subtracts 256 */
static int Warrior_Value(uint8_t low, bool bit7, bool minus256)
{
    return (low + (bit7 ? 128 : 0)) - (minus256 ? 256 : 0);
}

static void Warrior_Decode(SDL_WarriorState *s)
{
    const uint8_t *p = s->packet;
    SDL_SerialControls controls = s->base.snapshots[0].controls;
    int i;

    switch ((p[0] >> 4) & 7) {
    case 1:
        for (i = 0; i < SDL_WARRIOR_BUTTONS; ++i) {
            SDL_Serial_SetButton(&controls, i, (p[3] & (1 << i)) != 0);
        }
        break;
    case 3:
    {
        /* X = 256 x (byte 0 bit 3) - (byte 2 + 128 x byte 0 bit 2),
         * Y = (byte 1 + 128 x byte 0 bit 0) - 256 x (byte 0 bit 1) */
        const int x = -Warrior_Value(p[2], (p[0] & 0x04) != 0, (p[0] & 0x08) != 0);
        const int y = Warrior_Value(p[1], (p[0] & 0x01) != 0, (p[0] & 0x02) != 0);

        controls.axes[0] = SDL_Serial_ScaleSigned(x, -255, 256);
        controls.axes[1] = SDL_Serial_ScaleSigned(y, -256, 255);
        break;
    }
    case 5:
    {
        const int throttle = Warrior_Value(p[1], (p[0] & 0x01) != 0, (p[0] & 0x02) != 0);
        const int dial = Warrior_Value(p[2], (p[0] & 0x04) != 0, (p[0] & 0x08) != 0);
        uint8_t hat = 0;

        controls.axes[2] = SDL_Serial_ScaleSigned(throttle, -256, 255);
        if (p[3] & 0x04) {
            hat |= SDL_SERIAL_HAT_UP;
        }
        if (p[3] & 0x08) {
            hat |= SDL_SERIAL_HAT_DOWN;
        }
        if (p[3] & 0x01) {
            hat |= SDL_SERIAL_HAT_LEFT;
        }
        if (p[3] & 0x02) {
            hat |= SDL_SERIAL_HAT_RIGHT;
        }
        controls.hats[0] = hat;
        controls.ball[0] = (int16_t)dial;
        controls.ball[1] = 0;
        break;
    }
    default:
        /* Types 2 and 4 are skipped */
        return;
    }
    SDL_Serial_Commit(&s->base, 0, &controls);
}

static void Warrior_PacketByte(SDL_WarriorState *s, uint8_t byte)
{
    if (byte & 0x80) {
        /* A start byte ends any packet still short of its length. Linux
         * decodes that part with stale bytes, this module drops it. */
        s->packet_length = 0;
        s->packet_expected = SDL_Warrior_PacketLength(byte);
        if (s->packet_expected == 0) {
            return;
        }
    } else if (s->packet_expected == 0) {
        return;
    }
    s->packet[s->packet_length++] = byte;
    if (s->packet_length == s->packet_expected) {
        Warrior_Decode(s);
        s->packet_length = 0;
        s->packet_expected = 0;
    }
}

static void Warrior_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_WarriorState *s = (SDL_WarriorState *)state;
    const SDL_SerialLine line = Warrior_HandshakeLine();
    const uint8_t action_seq = s->action_seq;

    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->action_seq = action_seq;
    SDL_Serial_QueueLine(&s->base, 0, &line);
    Warrior_Drain(s, now, now);
}

static void Warrior_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_WarriorState *s = (SDL_WarriorState *)state;
    size_t i;

    for (i = 0; i < length; ++i) {
        const uint8_t byte = data[i];

        switch (s->step) {
        case SDL_WARRIOR_STEP_DRAIN:
            s->last_byte = now;
            break;
        case SDL_WARRIOR_STEP_STAR:
            if (byte == '*') {
                Warrior_WriteByte(s, 'S', SDL_WARRIOR_STEP_S);
            } else {
                Warrior_Fail(s, now);
            }
            break;
        case SDL_WARRIOR_STEP_S:
            if (byte == 'S') {
                const SDL_SerialLine line = SDL_Serial_Line(SDL_WARRIOR_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);

                s->waiting_seq = Warrior_NextSeq(s);
                SDL_Serial_QueueLine(&s->base, s->waiting_seq, &line);
                s->step = SDL_WARRIOR_STEP_RATE;
                s->echo_timer = false;
            } else {
                Warrior_Fail(s, now);
            }
            break;
        case SDL_WARRIOR_STEP_RATE:
            break; /* Bytes before the new rate applies */
        case SDL_WARRIOR_STEP_PRESENT:
            Warrior_PacketByte(s, byte);
            break;
        }
    }
}

static void Warrior_Tick(void *state, uint64_t now)
{
    SDL_WarriorState *s = (SDL_WarriorState *)state;

    switch (s->step) {
    case SDL_WARRIOR_STEP_DRAIN:
        if (now >= Warrior_DrainEnd(s)) {
            Warrior_WriteByte(s, '*', SDL_WARRIOR_STEP_STAR);
        }
        break;
    case SDL_WARRIOR_STEP_STAR:
    case SDL_WARRIOR_STEP_S:
        if (s->echo_timer && now >= s->echo_deadline) {
            Warrior_Fail(s, now);
        }
        break;
    default:
        break;
    }
}

static void Warrior_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_WarriorState *s = (SDL_WarriorState *)state;
    uint8_t tag;

    if (!SDL_Serial_FinishAction(&s->base, &tag) || tag == 0 || tag != s->waiting_seq) {
        return;
    }
    s->waiting_seq = 0;
    if (!success) {
        Warrior_Fail(s, now);
        return;
    }
    switch (s->step) {
    case SDL_WARRIOR_STEP_STAR:
    case SDL_WARRIOR_STEP_S:
        s->echo_timer = true;
        s->echo_deadline = now + SDL_WARRIOR_ECHO_MS;
        break;
    case SDL_WARRIOR_STEP_RATE:
        Warrior_Present(s);
        break;
    default:
        break;
    }
}

static void Warrior_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

static bool Warrior_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_WarriorState *s = (SDL_WarriorState *)state;

    switch (s->step) {
    case SDL_WARRIOR_STEP_DRAIN:
        *deadline = Warrior_DrainEnd(s);
        return true;
    case SDL_WARRIOR_STEP_STAR:
    case SDL_WARRIOR_STEP_S:
        if (s->echo_timer) {
            *deadline = s->echo_deadline;
            return true;
        }
        return false;
    default:
        return false;
    }
}

const SDL_SerialModule SDL_SerialWarriorModule = {
    "warrior",
    sizeof(SDL_WarriorState),
    false,
    0,
    0,
    Warrior_Reset,
    Warrior_Feed,
    Warrior_Tick,
    SDL_Serial_NextAction,
    Warrior_ActionDone,
    Warrior_Output,
    Warrior_GetDeadline,
    SDL_Serial_GetSnapshot
};
