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

#include "SDL_serial_cyberman_proto.h"

#include <string.h>

#define CYBERMAN_OUTPUT_TAG 0xFF /* The rumble write, replaced while it waits */

static int CyberMan_Signed(int value, int bits)
{
    return (value & (1 << (bits - 1))) ? value - (1 << bits) : value;
}

bool SDL_CyberMan_Decode3D(const uint8_t *r, size_t length, int *axes, uint8_t *buttons)
{
    if (!r || length != 5 || (r[0] & 0xE0) != 0x80) {
        return false;
    }
    /* Byte 1 bits 2-0 L, M, R. X is byte 2 bits 6-0 then byte 3 bit 6. Y is
     * byte 3 bits 5-0 then byte 4 bits 6-5. Byte 4 holds Z in bits 4-3,
     * pitch in bits 2-1 and roll's high bit, byte 5 roll's low bit in bit 6
     * and yaw in bits 5-4. */
    axes[0] = CyberMan_Signed(((r[1] & 0x7F) << 1) | ((r[2] >> 6) & 1), 8);
    axes[1] = CyberMan_Signed(((r[2] & 0x3F) << 2) | ((r[3] >> 5) & 3), 8);
    axes[2] = CyberMan_Signed((r[3] >> 3) & 3, 2);
    axes[3] = CyberMan_Signed((r[3] >> 1) & 3, 2);
    axes[4] = CyberMan_Signed(((r[3] & 1) << 1) | ((r[4] >> 6) & 1), 2);
    axes[5] = CyberMan_Signed((r[4] >> 4) & 3, 2);
    *buttons = (uint8_t)((((r[0] >> 2) & 1) << 0) | (((r[0] >> 1) & 1) << 1) | ((r[0] & 1) << 2));
    return true;
}

void SDL_CyberMan_TactileCommand(uint16_t low_frequency_rumble, uint16_t high_frequency_rumble, uint8_t *command)
{
    const uint16_t amplitude = (low_frequency_rumble > high_frequency_rumble) ? low_frequency_rumble : high_frequency_rumble;

    command[0] = '!';
    command[1] = 'T';
    if (amplitude == 0) {
        /* The shortest burst: 5 ms on, 5 ms off, no length */
        command[2] = 0x01;
        command[3] = 0x01;
        command[4] = 0x00;
        return;
    }
    /* On-time in 5 ms units grows with the amplitude, off-time one unit,
     * the longest length */
    command[2] = (uint8_t)(1 + ((uint32_t)amplitude * 254) / 65535);
    command[3] = 0x01;
    command[4] = 0xFF;
}

static uint8_t CyberMan_NextSeq(SDL_CyberManState *s)
{
    if (++s->action_seq == 0 || s->action_seq == CYBERMAN_OUTPUT_TAG) {
        s->action_seq = 1;
    }
    return s->action_seq;
}

static void CyberMan_SetTimer(SDL_CyberManState *s, uint64_t at)
{
    s->deadline = at;
    s->deadline_set = true;
}

/* Step 1 and 2: 1200 7N1 with DTR and RTS on, then RTS off for 200 ms */
static void CyberMan_StartReset(SDL_CyberManState *s)
{
    const SDL_SerialLine line = SDL_Serial_Line(SDL_CYBERMAN_MOUSE_RATE, 7, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);

    SDL_Serial_QueueLine(&s->base, 0, &line);
    s->waiting_seq = CyberMan_NextSeq(s);
    SDL_Serial_QueueModem(&s->base, s->waiting_seq, true, false);
    s->step = SDL_CYBERMAN_STEP_RTS_LOW;
    s->deadline_set = false;
    s->report_expected = 0;
    s->report_length = 0;
}

static void CyberMan_Fail(SDL_CyberManState *s, uint64_t now)
{
    SDL_Serial_ClearActions(&s->base);
    s->waiting_seq = 0;
    s->step = SDL_CYBERMAN_STEP_RETRY;
    CyberMan_SetTimer(s, now + SDL_CYBERMAN_RETRY_MS);
}

static void CyberMan_Present(SDL_CyberManState *s)
{
    SDL_SerialIdentity identity;

    SDL_Serial_SetIdentity(&identity, "Logitech CyberMan", SDL_SERIAL_TYPE_UNKNOWN, SDL_CYBERMAN_AXES, SDL_CYBERMAN_BUTTONS, 0, 0);
    s->step = SDL_CYBERMAN_STEP_PRESENT;
    s->deadline_set = false;
    SDL_Serial_Present(&s->base, 0, &identity);
}

static void CyberMan_HandleReport(SDL_CyberManState *s)
{
    const uint8_t *r = s->report;

    switch ((r[0] >> 5) & 3) {
    case 0:
        if (s->step == SDL_CYBERMAN_STEP_PRESENT) {
            SDL_SerialControls controls = s->base.snapshots[0].controls;
            int axes[SDL_CYBERMAN_AXES], i;
            uint8_t buttons;

            if (!SDL_CyberMan_Decode3D(r, 5, axes, &buttons)) {
                return;
            }
            controls.axes[0] = SDL_Serial_ScaleSigned(axes[0], -128, 127);
            controls.axes[1] = SDL_Serial_ScaleSigned(axes[1], -128, 127);
            for (i = 2; i < SDL_CYBERMAN_AXES; ++i) {
                controls.axes[i] = SDL_Serial_ScaleSigned(axes[i], -2, 1);
            }
            for (i = 0; i < SDL_CYBERMAN_BUTTONS; ++i) {
                SDL_Serial_SetButton(&controls, i, (buttons & (1 << i)) != 0);
            }
            SDL_Serial_Commit(&s->base, 0, &controls);
        }
        break;
    case 1:
        /* The static report: bytes 2 and 3 are the version */
        s->version_major = r[1] & 0x7F;
        s->version_minor = r[2] & 0x7F;
        if (s->step == SDL_CYBERMAN_STEP_STATIC) {
            CyberMan_Present(s);
        }
        break;
    case 2:
    {
        const bool connected = (r[0] & 0x01) != 0;
        const bool too_high = (r[0] & 0x02) != 0;

        if (too_high && !s->power_too_high) {
            SDL_Serial_Log(&s->base, "CyberMan reports its external power too high for the tactile motor");
        }
        s->power_connected = connected;
        s->power_too_high = too_high;
        break;
    }
    default:
        break;
    }
}

/* The first byte of a report has bit 7 set, later bytes bit 7 clear. A
 * byte with bit 7 set inside a report starts a new one. */
static void CyberMan_ReportByte(SDL_CyberManState *s, uint8_t byte)
{
    static const int lengths[4] = { 5, 12, 4, 0 };

    if (byte & 0x80) {
        s->report_expected = lengths[(byte >> 5) & 3];
        s->report_length = 0;
        if (s->report_expected == 0) {
            return;
        }
    } else if (s->report_expected == 0) {
        return;
    }
    s->report[s->report_length++] = byte;
    if (s->report_length == s->report_expected) {
        s->report_expected = 0;
        CyberMan_HandleReport(s);
        s->report_length = 0;
    }
}

static void CyberMan_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_CyberManState *s = (SDL_CyberManState *)state;
    const uint8_t action_seq = s->action_seq;

    (void)now;
    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->action_seq = action_seq;
    CyberMan_StartReset(s);
}

static void CyberMan_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_CyberManState *s = (SDL_CyberManState *)state;
    size_t i;

    for (i = 0; i < length; ++i) {
        const uint8_t byte = data[i];

        switch (s->step) {
        case SDL_CYBERMAN_STEP_WAIT_M:
            if (byte == 'M') {
                s->step = SDL_CYBERMAN_STEP_WAIT_3;
                CyberMan_SetTimer(s, now + SDL_CYBERMAN_3_MS);
            }
            break;
        case SDL_CYBERMAN_STEP_WAIT_3:
            if (byte == '3') {
                static const uint8_t swift[2] = { '*', 'S' };

                SDL_Serial_QueueWrite(&s->base, 0, swift, sizeof(swift));
                s->waiting_seq = CyberMan_NextSeq(s);
                SDL_Serial_QueueDrain(&s->base, s->waiting_seq);
                s->step = SDL_CYBERMAN_STEP_SWITCH;
                s->deadline_set = false;
            } else {
                CyberMan_Fail(s, now);
            }
            break;
        case SDL_CYBERMAN_STEP_STATIC:
        case SDL_CYBERMAN_STEP_PRESENT:
            CyberMan_ReportByte(s, byte);
            break;
        default:
            /* Echoes and bytes at the old rate */
            break;
        }
    }
}

static void CyberMan_Tick(void *state, uint64_t now)
{
    SDL_CyberManState *s = (SDL_CyberManState *)state;

    if (!s->deadline_set || now < s->deadline) {
        return;
    }
    s->deadline_set = false;
    switch (s->step) {
    case SDL_CYBERMAN_STEP_RTS_LOW:
        s->waiting_seq = CyberMan_NextSeq(s);
        SDL_Serial_QueueModem(&s->base, s->waiting_seq, true, true);
        s->step = SDL_CYBERMAN_STEP_RTS_HIGH;
        break;
    case SDL_CYBERMAN_STEP_SETTLE:
    {
        const SDL_SerialLine line = SDL_Serial_Line(SDL_CYBERMAN_SWIFT_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);
        static const uint8_t status[2] = { '!', 'S' };

        SDL_Serial_QueueLine(&s->base, 0, &line);
        s->waiting_seq = CyberMan_NextSeq(s);
        SDL_Serial_QueueWrite(&s->base, s->waiting_seq, status, sizeof(status));
        s->step = SDL_CYBERMAN_STEP_RATE;
        s->report_expected = 0;
        s->report_length = 0;
        break;
    }
    case SDL_CYBERMAN_STEP_RETRY:
        CyberMan_StartReset(s);
        break;
    case SDL_CYBERMAN_STEP_WAIT_M:
    case SDL_CYBERMAN_STEP_WAIT_3:
    case SDL_CYBERMAN_STEP_STATIC:
        CyberMan_Fail(s, now);
        break;
    default:
        break;
    }
}

static void CyberMan_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_CyberManState *s = (SDL_CyberManState *)state;
    uint8_t tag;

    if (!SDL_Serial_FinishAction(&s->base, &tag) || tag == 0 || tag != s->waiting_seq) {
        return;
    }
    s->waiting_seq = 0;
    if (!success) {
        CyberMan_Fail(s, now);
        return;
    }
    switch (s->step) {
    case SDL_CYBERMAN_STEP_RTS_LOW:
        CyberMan_SetTimer(s, now + SDL_CYBERMAN_RTS_LOW_MS);
        break;
    case SDL_CYBERMAN_STEP_RTS_HIGH:
        s->step = SDL_CYBERMAN_STEP_WAIT_M;
        CyberMan_SetTimer(s, now + SDL_CYBERMAN_M_MS);
        break;
    case SDL_CYBERMAN_STEP_SWITCH:
        /* *S has left the host. It must not meet the new rate. */
        s->step = SDL_CYBERMAN_STEP_SETTLE;
        CyberMan_SetTimer(s, now + SDL_CYBERMAN_SETTLE_MS);
        break;
    case SDL_CYBERMAN_STEP_RATE:
        s->step = SDL_CYBERMAN_STEP_STATIC;
        CyberMan_SetTimer(s, now + SDL_CYBERMAN_STATIC_MS);
        break;
    default:
        break;
    }
}

static void CyberMan_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    SDL_CyberManState *s = (SDL_CyberManState *)state;
    uint8_t command[5];

    (void)now;
    if (s->step != SDL_CYBERMAN_STEP_PRESENT || request->kind != SDL_SERIAL_OUTPUT_RUMBLE) {
        return;
    }
    SDL_CyberMan_TactileCommand(request->low_frequency_rumble, request->high_frequency_rumble, command);
    if (!SDL_Serial_ReplaceWrite(&s->base, CYBERMAN_OUTPUT_TAG, command, sizeof(command))) {
        SDL_Serial_QueueWrite(&s->base, CYBERMAN_OUTPUT_TAG, command, sizeof(command));
    }
}

static bool CyberMan_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_CyberManState *s = (SDL_CyberManState *)state;

    if (!s->deadline_set) {
        return false;
    }
    *deadline = s->deadline;
    return true;
}

const SDL_SerialModule SDL_SerialCyberManModule = {
    "cyberman",
    sizeof(SDL_CyberManState),
    true,
    0,
    0,
    CyberMan_Reset,
    CyberMan_Feed,
    CyberMan_Tick,
    SDL_Serial_NextAction,
    CyberMan_ActionDone,
    CyberMan_Output,
    CyberMan_GetDeadline,
    SDL_Serial_GetSnapshot
};
