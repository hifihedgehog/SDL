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

#include "SDL_serial_spaceorb_proto.h"

#include <stdio.h>
#include <string.h>

static const uint8_t spaceorb_key[9] = { 'S', 'p', 'a', 'c', 'e', 'W', 'a', 'r', 'e' };

bool SDL_SpaceOrb_DecodeD(const uint8_t *stored, size_t length, int *axes, uint8_t *buttons)
{
    uint8_t body[9];
    int i;

    if (!stored || length != SDL_SPACEORB_D_LENGTH || stored[0] != 'D') {
        return false;
    }
    for (i = 0; i < 9; ++i) {
        body[i] = (uint8_t)((stored[2 + i] ^ spaceorb_key[i]) & 0x7F);
    }
    /* Bytes 2 to 10 hold 63 bits, most significant first from byte 2 bit 6.
     * Each value takes the next 10, and byte 10 bits 2-0 go unused. */
    for (i = 0; i < SDL_SPACEORB_AXES; ++i) {
        int value = 0, bit;

        for (bit = 0; bit < 10; ++bit) {
            const int position = i * 10 + bit;

            value = (value << 1) | ((body[position / 7] >> (6 - position % 7)) & 1);
        }
        axes[i] = (value > 511) ? value - 1024 : value;
    }
    *buttons = (uint8_t)(stored[1] & 0x7F);
    return true;
}

static uint8_t SpaceOrb_Xor(const SDL_SpaceOrbState *s)
{
    uint8_t x = 0;
    size_t i;

    for (i = 0; i < s->length; ++i) {
        x ^= s->packet[i];
    }
    return x;
}

static void SpaceOrb_Present(SDL_SpaceOrbState *s)
{
    SDL_SerialIdentity identity;

    if (SDL_Serial_IsPresent(&s->base, 0)) {
        return;
    }
    SDL_Serial_SetIdentity(&identity, "SpaceTec SpaceOrb 360", SDL_SERIAL_TYPE_UNKNOWN,
                           SDL_SPACEORB_AXES, SDL_SPACEORB_BUTTONS, 0, 0);
    SDL_Serial_Present(&s->base, 0, &identity);
}

static void SpaceOrb_SetButtons(SDL_SerialControls *controls, uint8_t bits)
{
    int i;

    for (i = 0; i < SDL_SPACEORB_BUTTONS; ++i) {
        SDL_Serial_SetButton(controls, i, (bits & (1 << i)) != 0);
    }
}

/* A D, K or E packet that reached its length */
static void SpaceOrb_Finish(SDL_SpaceOrbState *s)
{
    SDL_SerialControls controls;

    s->finished = true;
    if (SpaceOrb_Xor(s) != 0) {
        return;
    }
    switch (s->packet[0]) {
    case 'D':
    {
        int axes[SDL_SPACEORB_AXES], i;
        uint8_t buttons;

        if (!SDL_SpaceOrb_DecodeD(s->packet, s->length, axes, &buttons)) {
            return;
        }
        SpaceOrb_Present(s);
        controls = s->base.snapshots[0].controls;
        for (i = 0; i < SDL_SPACEORB_AXES; ++i) {
            controls.axes[i] = SDL_Serial_ScaleSigned(axes[i], -512, 511);
        }
        SpaceOrb_SetButtons(&controls, buttons);
        SDL_Serial_Commit(&s->base, 0, &controls);
        break;
    }
    case 'K':
        SpaceOrb_Present(s);
        controls = s->base.snapshots[0].controls;
        SpaceOrb_SetButtons(&controls, s->packet[2]);
        SDL_Serial_Commit(&s->base, 0, &controls);
        break;
    case 'E':
    {
        char text[64];

        ++s->errors;
        s->last_error = s->packet[1];
        (void)snprintf(text, sizeof(text), "SpaceOrb reported errors 0x%02X", s->packet[1]);
        SDL_Serial_Log(&s->base, text);
        break;
    }
    default:
        break;
    }
}

/* Called at the next start byte. Only an R packet runs to it. */
static void SpaceOrb_EndPacket(SDL_SpaceOrbState *s)
{
    if (s->in_packet && !s->finished && !s->overflow && s->length >= 2 &&
        s->packet[0] == 'R' && SpaceOrb_Xor(s) == 0) {
        SpaceOrb_Present(s);
    }
    s->in_packet = false;
}

static size_t SpaceOrb_TypeLength(uint8_t type)
{
    switch (type) {
    case 'D':
        return SDL_SPACEORB_D_LENGTH;
    case 'K':
        return SDL_SPACEORB_K_LENGTH;
    case 'E':
        return SDL_SPACEORB_E_LENGTH;
    default:
        return 0;
    }
}

static void SpaceOrb_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_SpaceOrbState *s = (SDL_SpaceOrbState *)state;
    const SDL_SerialLine line = SDL_Serial_Line(SDL_SPACEORB_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);

    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    SDL_Serial_QueueLine(&s->base, 0, &line);
    /* Nothing is written. What waits in the port is drained until 100 ms of
     * silence, as inputattach flushes for this device. */
    s->draining = true;
    s->drain_until = now + SDL_SPACEORB_DRAIN_MS;
}

static void SpaceOrb_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_SpaceOrbState *s = (SDL_SpaceOrbState *)state;
    size_t i;

    for (i = 0; i < length; ++i) {
        const uint8_t byte = data[i];
        size_t expected;

        if (s->draining) {
            s->drain_until = now + SDL_SPACEORB_DRAIN_MS;
            continue;
        }
        if (!(byte & 0x80)) {
            SpaceOrb_EndPacket(s);
            s->in_packet = true;
            s->finished = false;
            s->overflow = false;
            s->packet[0] = byte;
            s->length = 1;
            continue;
        }
        if (!s->in_packet) {
            continue;
        }
        if (s->length < SDL_SPACEORB_MAX_LENGTH) {
            s->packet[s->length++] = (uint8_t)(byte & 0x7F);
        } else {
            s->overflow = true;
        }
        expected = SpaceOrb_TypeLength(s->packet[0]);
        if (expected && !s->finished && !s->overflow && s->length == expected) {
            SpaceOrb_Finish(s);
        }
    }
}

static void SpaceOrb_Tick(void *state, uint64_t now)
{
    SDL_SpaceOrbState *s = (SDL_SpaceOrbState *)state;

    if (s->draining && now >= s->drain_until) {
        s->draining = false;
    }
}

static void SpaceOrb_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_SpaceOrbState *s = (SDL_SpaceOrbState *)state;
    uint8_t tag;

    (void)success;
    (void)now;
    (void)SDL_Serial_FinishAction(&s->base, &tag);
}

static void SpaceOrb_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

static bool SpaceOrb_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_SpaceOrbState *s = (SDL_SpaceOrbState *)state;

    if (!s->draining) {
        return false;
    }
    *deadline = s->drain_until;
    return true;
}

const SDL_SerialModule SDL_SerialSpaceOrbModule = {
    "spaceorb",
    sizeof(SDL_SpaceOrbState),
    false,
    0,
    0,
    SpaceOrb_Reset,
    SpaceOrb_Feed,
    SpaceOrb_Tick,
    SDL_Serial_NextAction,
    SpaceOrb_ActionDone,
    SpaceOrb_Output,
    SpaceOrb_GetDeadline,
    SDL_Serial_GetSnapshot
};
