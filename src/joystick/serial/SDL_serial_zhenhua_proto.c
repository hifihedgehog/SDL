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

#include "SDL_serial_zhenhua_proto.h"

#include <string.h>

uint8_t SDL_ZhenHua_Reverse(uint8_t byte)
{
    uint8_t result = 0;
    int i;

    for (i = 0; i < 8; ++i) {
        result = (uint8_t)((result << 1) | ((byte >> i) & 1));
    }
    return result;
}

static void ZhenHua_StartDetection(SDL_ZhenHuaState *s)
{
    s->detecting = true;
    s->detect_length = 0;
    s->timer = false;
    s->frame_length = 0;
}

static void ZhenHua_Decode(SDL_ZhenHuaState *s)
{
    SDL_SerialControls controls = s->base.snapshots[0].controls;
    int i;

    for (i = 0; i < SDL_ZHENHUA_CHANNELS; ++i) {
        controls.axes[i] = SDL_Serial_ScaleUnsigned(s->frame[1 + i], SDL_ZHENHUA_MIN, SDL_ZHENHUA_MAX);
    }
    SDL_Serial_Commit(&s->base, 0, &controls);
}

/* A raw EF starts a frame and bytes before it are ignored */
static bool ZhenHua_FrameByte(SDL_ZhenHuaState *s, uint8_t byte)
{
    if (byte == SDL_ZHENHUA_SYNC) {
        s->frame_length = 0;
    } else if (s->frame_length == 0) {
        return false;
    }
    s->frame[s->frame_length++] = SDL_ZhenHua_Reverse(byte);
    if (s->frame_length == SDL_ZHENHUA_FRAME) {
        s->frame_length = 0;
        return true;
    }
    return false;
}

/* inputattach's check: an EF, then 9 more bytes of which the fifth is EF
 * again, each within 500 ms. inputattach gives up when none of the first 5
 * bytes is EF. This module keeps looking. */
static void ZhenHua_DetectByte(SDL_ZhenHuaState *s, uint8_t byte, uint64_t now)
{
    SDL_SerialIdentity identity;

    s->timer = true;
    s->deadline = now + SDL_ZHENHUA_BYTE_MS;
    if (s->detect_length == 0) {
        if (byte != SDL_ZHENHUA_SYNC) {
            return;
        }
        s->detect_length = 1;
        s->frame_length = 0;
        (void)ZhenHua_FrameByte(s, byte);
        return;
    }
    if (s->detect_length == SDL_ZHENHUA_FRAME && byte != SDL_ZHENHUA_SYNC) {
        /* The next frame must start right after this one */
        s->detect_length = 0;
        s->frame_length = 0;
        return;
    }
    ++s->detect_length;
    (void)ZhenHua_FrameByte(s, byte);
    if (s->detect_length < 2 * SDL_ZHENHUA_FRAME) {
        return;
    }
    s->detecting = false;
    SDL_Serial_SetIdentity(&identity, "Zhen Hua RC transmitter", SDL_SERIAL_TYPE_UNKNOWN, SDL_ZHENHUA_CHANNELS, 0, 0, 0);
    SDL_Serial_Present(&s->base, 0, &identity);
    ZhenHua_Decode(s);
}

static void ZhenHua_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_ZhenHuaState *s = (SDL_ZhenHuaState *)state;
    const SDL_SerialLine line = SDL_Serial_Line(SDL_ZHENHUA_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);

    (void)now;
    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    SDL_Serial_QueueLine(&s->base, 0, &line);
    ZhenHua_StartDetection(s);
}

static void ZhenHua_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_ZhenHuaState *s = (SDL_ZhenHuaState *)state;
    size_t i;

    for (i = 0; i < length; ++i) {
        if (s->detecting) {
            ZhenHua_DetectByte(s, data[i], now);
        } else if (ZhenHua_FrameByte(s, data[i])) {
            ZhenHua_Decode(s);
            s->deadline = now + SDL_ZHENHUA_BYTE_MS;
        }
    }
}

static void ZhenHua_Tick(void *state, uint64_t now)
{
    SDL_ZhenHuaState *s = (SDL_ZhenHuaState *)state;

    if (!s->timer || now < s->deadline) {
        return;
    }
    /* 500 ms without the next byte, or once present without a frame */
    SDL_Serial_Absent(&s->base, 0);
    ZhenHua_StartDetection(s);
}

static void ZhenHua_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_ZhenHuaState *s = (SDL_ZhenHuaState *)state;
    uint8_t tag;

    (void)success;
    (void)now;
    (void)SDL_Serial_FinishAction(&s->base, &tag);
}

static void ZhenHua_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

static bool ZhenHua_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_ZhenHuaState *s = (SDL_ZhenHuaState *)state;

    if (!s->timer) {
        return false;
    }
    *deadline = s->deadline;
    return true;
}

const SDL_SerialModule SDL_SerialZhenHuaModule = {
    "zhenhua",
    sizeof(SDL_ZhenHuaState),
    false,
    0,
    0,
    ZhenHua_Reset,
    ZhenHua_Feed,
    ZhenHua_Tick,
    SDL_Serial_NextAction,
    ZhenHua_ActionDone,
    ZhenHua_Output,
    ZhenHua_GetDeadline,
    SDL_Serial_GetSnapshot
};
