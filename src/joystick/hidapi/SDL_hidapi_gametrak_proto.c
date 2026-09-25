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

#include "SDL_hidapi_gametrak_proto.h"

#include <string.h>

#define GAMETRAK_HAT_UP    0x01
#define GAMETRAK_HAT_RIGHT 0x02
#define GAMETRAK_HAT_DOWN  0x04
#define GAMETRAK_HAT_LEFT  0x08

static const uint8_t gametrak_unlock[8] = { 0x47, 0x61, 0x6D, 0x65, 0x74, 0x72, 0x61, 0x6B }; /* "Gametrak" */

static uint8_t Gametrak_Hat(uint8_t value)
{
    /* The descriptor's hat: 1 up, then clockwise in 45 degree steps to 8 up-left */
    static const uint8_t hats[9] = {
        0,
        GAMETRAK_HAT_UP,
        GAMETRAK_HAT_UP | GAMETRAK_HAT_RIGHT,
        GAMETRAK_HAT_RIGHT,
        GAMETRAK_HAT_DOWN | GAMETRAK_HAT_RIGHT,
        GAMETRAK_HAT_DOWN,
        GAMETRAK_HAT_DOWN | GAMETRAK_HAT_LEFT,
        GAMETRAK_HAT_LEFT,
        GAMETRAK_HAT_UP | GAMETRAK_HAT_LEFT,
    };

    return (value <= 8) ? hats[value] : 0;
}

bool SDL_Gametrak_DecodeReport(const uint8_t *report, size_t length, SDL_GametrakReport *out)
{
    uint16_t words[SDL_GAMETRAK_AXES];
    int j;

    if (!report || !out || length != SDL_GAMETRAK_REPORT_LENGTH) {
        return false;
    }
    for (j = 0; j < SDL_GAMETRAK_AXES; ++j) {
        words[j] = (uint16_t)(report[2 * j] | (report[2 * j + 1] << 8));
        if (words[j] > 4095) {
            return false;
        }
    }
    memset(out, 0, sizeof(*out));
    for (j = 0; j < SDL_GAMETRAK_AXES; ++j) {
        const int value = words[j] >> 1;
        out->words[j] = (uint16_t)value;
        out->axes[j] = (int16_t)(value * 65535 / 2047 - 32768);
        out->key_bits |= (uint8_t)((words[j] & 1) << j);
    }
    out->hat = Gametrak_Hat((uint8_t)(report[12] & 0x0F));
    /* Byte 12 bit 4 is the foot switch, then buttons 2-12 of the descriptor */
    out->buttons = (uint16_t)(((report[12] >> 4) & 0x0F) | (report[13] << 4));
    return true;
}

bool SDL_Gametrak_IsUnlockAnswer(const uint8_t *report, size_t length)
{
    return report && length == SDL_GAMETRAK_REPORT_LENGTH && memcmp(report, gametrak_unlock, sizeof(gametrak_unlock)) == 0;
}

uint32_t SDL_Gametrak_NextKey(uint32_t key)
{
    uint32_t next = 0;

    next |= key & 0x1;                                 /* bit 0 stays */
    next |= (((key >> 16) ^ (key >> 17)) & 0x1) << 1;  /* bit 1: bits 16 and 17 */
    next |= ((key ^ (key >> 18)) & 0x3F) << 2;         /* bits 2-7: bits 0-5 and 18-23 */
    next |= ((key >> 1) & 0xFFFF) << 8;                /* bits 8-23: bits 1-16 */
    return next & 0xFFFFFF;
}

static void Gametrak_SendStart(SDL_GametrakSession *session, const SDL_GametrakSink *sink)
{
    uint8_t report[2];

    report[0] = 0x45;
    report[1] = SDL_GAMETRAK_INITIAL_KEY;
    session->phase = SDL_GAMETRAK_PHASE_STREAMING;
    session->key = SDL_GAMETRAK_INITIAL_KEY;
    session->sensor_reports = 0;
    sink->write(sink->userdata, report, sizeof(report));
}

void SDL_Gametrak_Open(SDL_GametrakSession *session, uint64_t now_ms, const SDL_GametrakSink *sink)
{
    memset(session, 0, sizeof(*session));
    session->phase = SDL_GAMETRAK_PHASE_UNLOCK;
    session->answer_due_ms = now_ms + SDL_GAMETRAK_ANSWER_WAIT_MS;
    session->key = SDL_GAMETRAK_INITIAL_KEY;
    sink->write(sink->userdata, gametrak_unlock, sizeof(gametrak_unlock));
}

void SDL_Gametrak_Update(SDL_GametrakSession *session, uint64_t now_ms, const SDL_GametrakSink *sink)
{
    if (session->phase == SDL_GAMETRAK_PHASE_UNLOCK && now_ms >= session->answer_due_ms) {
        Gametrak_SendStart(session, sink);
    }
}

bool SDL_Gametrak_HandleReport(SDL_GametrakSession *session, uint64_t now_ms, const uint8_t *report, size_t length,
                               const SDL_GametrakSink *sink, SDL_GametrakReport *out)
{
    uint8_t expected;

    (void)now_ms;
    if (session->phase == SDL_GAMETRAK_PHASE_UNLOCK && SDL_Gametrak_IsUnlockAnswer(report, length)) {
        Gametrak_SendStart(session, sink);
        return false;
    }
    if (!SDL_Gametrak_DecodeReport(report, length, out)) {
        return false;
    }
    if (session->phase != SDL_GAMETRAK_PHASE_STREAMING) {
        return true;
    }

    /* The key bits show the byte the unit expects in the next keep-alive */
    expected = (uint8_t)(SDL_Gametrak_NextKey(session->key) & 0xFF);
    session->desync = (out->key_bits != (expected & 0x3F));
    ++session->sensor_reports;
    if (session->sensor_reports % SDL_GAMETRAK_KEEPALIVE_REPORTS == 0) {
        uint8_t keepalive[2];
        session->key = SDL_Gametrak_NextKey(session->key);
        keepalive[0] = 0x46;
        keepalive[1] = (uint8_t)(session->key & 0xFF);
        sink->write(sink->userdata, keepalive, sizeof(keepalive));
    }
    return true;
}
