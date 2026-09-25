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

#include "SDL_serial_ibus_proto.h"

#include <string.h>

bool SDL_IBus_DecodeFrame(const uint8_t *frame, size_t length, uint16_t *channels)
{
    uint16_t checksum = 0xFFFF;
    int i;

    if (!frame || length != SDL_IBUS_FRAME || frame[0] != SDL_IBUS_LENGTH || frame[1] != SDL_IBUS_COMMAND) {
        return false;
    }
    for (i = 0; i < SDL_IBUS_FRAME - 2; ++i) {
        checksum = (uint16_t)(checksum - frame[i]);
    }
    if (checksum != (uint16_t)(frame[30] | (frame[31] << 8))) {
        return false;
    }
    for (i = 0; i < SDL_IBUS_CHANNELS; ++i) {
        channels[i] = (uint16_t)((frame[2 + 2 * i] | (frame[3 + 2 * i] << 8)) & 0x0FFF);
    }
    return true;
}

static void IBus_Drop(SDL_IBusState *s)
{
    memmove(s->window, s->window + 1, (size_t)(s->length - 1));
    --s->length;
}

static void IBus_Frame(SDL_IBusState *s, const uint16_t *channels, uint64_t now)
{
    SDL_SerialControls controls;
    int i;

    if (!SDL_Serial_IsPresent(&s->base, 0)) {
        SDL_SerialIdentity identity;

        SDL_Serial_SetIdentity(&identity, "FlySky FS-iA6B", SDL_SERIAL_TYPE_UNKNOWN, SDL_IBUS_CHANNELS, 0, 0, 0);
        SDL_Serial_Present(&s->base, 0, &identity);
    }
    controls = s->base.snapshots[0].controls;
    for (i = 0; i < SDL_IBUS_CHANNELS; ++i) {
        controls.axes[i] = SDL_Serial_ScaleUnsigned(channels[i], SDL_IBUS_MIN, SDL_IBUS_MAX);
    }
    SDL_Serial_Commit(&s->base, 0, &controls);
    s->timer = true;
    s->deadline = now + SDL_IBUS_SILENCE_MS;
}

static void IBus_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_IBusState *s = (SDL_IBusState *)state;
    const SDL_SerialLine line = SDL_Serial_Line(SDL_IBUS_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);

    (void)now;
    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    SDL_Serial_QueueLine(&s->base, 0, &line);
}

/* A window slides over the stream. It holds bytes from a 20 40 on, and a
 * frame that fails its checksum gives up only its first byte, so a frame
 * that began inside it is found at once. */
static void IBus_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_IBusState *s = (SDL_IBusState *)state;
    size_t i;

    for (i = 0; i < length; ++i) {
        s->window[s->length++] = data[i];
        while (s->length > 0) {
            uint16_t channels[SDL_IBUS_CHANNELS];

            if (s->window[0] != SDL_IBUS_LENGTH || (s->length >= 2 && s->window[1] != SDL_IBUS_COMMAND)) {
                IBus_Drop(s);
                continue;
            }
            if (s->length < SDL_IBUS_FRAME) {
                break;
            }
            if (SDL_IBus_DecodeFrame(s->window, SDL_IBUS_FRAME, channels)) {
                s->length = 0;
                IBus_Frame(s, channels, now);
                break;
            }
            ++s->bad_frames;
            IBus_Drop(s);
        }
    }
}

static void IBus_Tick(void *state, uint64_t now)
{
    SDL_IBusState *s = (SDL_IBusState *)state;

    if (s->timer && now >= s->deadline) {
        s->timer = false;
        SDL_Serial_Absent(&s->base, 0);
    }
}

static void IBus_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_IBusState *s = (SDL_IBusState *)state;
    uint8_t tag;

    (void)success;
    (void)now;
    (void)SDL_Serial_FinishAction(&s->base, &tag);
}

static void IBus_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

static bool IBus_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_IBusState *s = (SDL_IBusState *)state;

    if (!s->timer) {
        return false;
    }
    *deadline = s->deadline;
    return true;
}

const SDL_SerialModule SDL_SerialIBusModule = {
    "ibus",
    sizeof(SDL_IBusState),
    false,
    0,
    0,
    IBus_Reset,
    IBus_Feed,
    IBus_Tick,
    SDL_Serial_NextAction,
    IBus_ActionDone,
    IBus_Output,
    IBus_GetDeadline,
    SDL_Serial_GetSnapshot
};
