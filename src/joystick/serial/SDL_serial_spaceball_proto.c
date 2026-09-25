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

#include "SDL_serial_spaceball_proto.h"

#include <string.h>

#define SPACEBALL_XON 0x11
#define SPACEBALL_CR  0x0D
#define SPACEBALL_LF  0x0A

static bool Spaceball_StartsWith(const uint8_t *line, size_t length, const char *prefix)
{
    const size_t n = strlen(prefix);

    return length >= n && memcmp(line, prefix, n) == 0;
}

static char Spaceball_Lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool Spaceball_StartsWithNoCase(const char *text, const char *prefix)
{
    while (*prefix) {
        if (Spaceball_Lower(*text) != Spaceball_Lower(*prefix)) {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

static bool Spaceball_Contains(const uint8_t *line, size_t length, const char *text)
{
    const size_t n = strlen(text);
    size_t i;

    for (i = 0; i + n <= length; ++i) {
        if (memcmp(line + i, text, n) == 0) {
            return true;
        }
    }
    return false;
}

SDL_SpaceballModel SDL_Spaceball_Classify(const char *reply, int firmware_major, int firmware_minor)
{
    if (reply) {
        if (strncmp(reply, "Hm2003B", 7) == 0) {
            return SDL_SPACEBALL_MODEL_2003B;
        }
        if (strncmp(reply, "Hm2003C", 7) == 0) {
            return SDL_SPACEBALL_MODEL_2003C;
        }
        if (strncmp(reply, "Hm3003C", 7) == 0) {
            return SDL_SPACEBALL_MODEL_3003C;
        }
        if (Spaceball_StartsWithNoCase(reply, "HvFirmware")) {
            return SDL_SPACEBALL_MODEL_4000FLX;
        }
    }
    if (firmware_major == 2) {
        switch (firmware_minor) {
        case 35:
        case 62:
        case 63:
            return SDL_SPACEBALL_MODEL_3003;
        case 43:
        case 45:
            /* A 5000 FLX-A in 4000 mode, presented as a 4000 FLX */
            return SDL_SPACEBALL_MODEL_4000FLX;
        case 2:
        case 13:
        case 15:
        case 42:
            return SDL_SPACEBALL_MODEL_1003_2003;
        default:
            break;
        }
    }
    return SDL_SPACEBALL_MODEL_UNKNOWN;
}

const char *SDL_Spaceball_ModelName(SDL_SpaceballModel model)
{
    switch (model) {
    case SDL_SPACEBALL_MODEL_1003_2003:
        return "Spaceball 1003/2003";
    case SDL_SPACEBALL_MODEL_2003B:
        return "Spaceball 2003B";
    case SDL_SPACEBALL_MODEL_2003C:
        return "Spaceball 2003C";
    case SDL_SPACEBALL_MODEL_3003C:
        return "Spaceball 3003C";
    case SDL_SPACEBALL_MODEL_3003:
        return "Spaceball 3003/3003C";
    case SDL_SPACEBALL_MODEL_4000FLX:
        return "Spaceball 4000 FLX";
    case SDL_SPACEBALL_MODEL_4000FLX_LEFTY:
        return "Spaceball 4000 FLX Lefty";
    default:
        return "Spaceball";
    }
}

/* "Firmware version M.mm" anywhere in a line */
static void Spaceball_ParseFirmware(SDL_SpaceballState *s, const uint8_t *line, size_t length)
{
    static const char marker[] = "Firmware version";
    const size_t n = sizeof(marker) - 1;
    size_t i;

    for (i = 0; i + n <= length; ++i) {
        size_t p = i + n;
        int major = 0, minor = 0, digits;

        if (memcmp(line + i, marker, n) != 0) {
            continue;
        }
        while (p < length && line[p] == ' ') {
            ++p;
        }
        for (digits = 0; p < length && line[p] >= '0' && line[p] <= '9' && digits < 4; ++p, ++digits) {
            major = major * 10 + (line[p] - '0');
        }
        if (digits == 0 || p >= length || line[p] != '.') {
            return;
        }
        ++p;
        for (digits = 0; p < length && line[p] >= '0' && line[p] <= '9' && digits < 4; ++p, ++digits) {
            minor = minor * 10 + (line[p] - '0');
        }
        if (digits == 0) {
            return;
        }
        s->firmware_major = major;
        s->firmware_minor = minor;
        return;
    }
}

static void Spaceball_SetTimer(SDL_SpaceballState *s, uint64_t at)
{
    s->deadline = at;
    s->deadline_set = true;
}

/* Writes a command and its 0D. The reply timer starts when the write is done. */
static void Spaceball_Command(SDL_SpaceballState *s, const char *command, SDL_SpaceballStep next)
{
    uint8_t data[16];
    const size_t n = strlen(command);

    memcpy(data, command, n);
    data[n] = SPACEBALL_CR;
    if (++s->write_seq == 0) {
        s->write_seq = 1;
    }
    SDL_Serial_QueueWrite(&s->base, s->write_seq, data, n + 1);
    s->waiting_seq = s->write_seq;
    s->step = next;
    s->lines = 0;
    s->deadline_set = false;
}

/* Step 2: \r@RESET\r, then a line holding "@1" within 2000 ms */
static void Spaceball_StartReset(SDL_SpaceballState *s)
{
    static const uint8_t reset[8] = { 0x0D, '@', 'R', 'E', 'S', 'E', 'T', 0x0D };

    if (++s->write_seq == 0) {
        s->write_seq = 1;
    }
    SDL_Serial_QueueWrite(&s->base, s->write_seq, reset, sizeof(reset));
    s->waiting_seq = s->write_seq;
    s->step = SDL_SPACEBALL_STEP_RESET;
    s->lines = 0;
    s->deadline_set = false;
}

/* Any failure after the banner restarts at step 2 after 1000 ms */
static void Spaceball_Fail(SDL_SpaceballState *s, uint64_t now)
{
    SDL_Serial_ClearActions(&s->base);
    s->waiting_seq = 0;
    s->step = SDL_SPACEBALL_STEP_RETRY;
    s->lines = 0;
    Spaceball_SetTimer(s, now + SDL_SPACEBALL_RETRY_MS);
}

static void Spaceball_Present(SDL_SpaceballState *s)
{
    SDL_SerialIdentity identity;

    SDL_Serial_SetIdentity(&identity, SDL_Spaceball_ModelName(s->model), SDL_SERIAL_TYPE_UNKNOWN,
                           SDL_SPACEBALL_AXES, SDL_SPACEBALL_BUTTONS, 0, 0);
    s->step = SDL_SPACEBALL_STEP_PRESENT;
    s->deadline_set = false;
    s->packet_length = 0;
    s->packet_overflow = false;
    s->escape = false;
    s->seen_dot = false;
    SDL_Serial_Present(&s->base, 0, &identity);
}

/* One reply line in a wait for a line starting with c: true when it matched.
 * After 8 lines without a match the start-up fails. */
static bool Spaceball_WaitLine(SDL_SpaceballState *s, char c, uint64_t now)
{
    ++s->lines;
    if (!s->line_overflow && s->line_length > 0 && s->line[0] == (uint8_t)c) {
        return true;
    }
    if (s->lines >= SDL_SPACEBALL_REPLY_LINES) {
        Spaceball_Fail(s, now);
    } else if (s->waiting_seq == 0) {
        Spaceball_SetTimer(s, now + SDL_SPACEBALL_REPLY_MS);
    }
    return false;
}

static void Spaceball_NextWait(SDL_SpaceballState *s, SDL_SpaceballStep next, uint64_t now)
{
    s->step = next;
    s->lines = 0;
    Spaceball_SetTimer(s, now + SDL_SPACEBALL_REPLY_MS);
}

static void Spaceball_HandleLine(SDL_SpaceballState *s, uint64_t now)
{
    const uint8_t *line = s->line;
    const size_t length = s->line_overflow ? 0 : s->line_length;

    switch (s->step) {
    case SDL_SPACEBALL_STEP_BANNER:
        if (memchr(line, SPACEBALL_XON, length)) {
            Spaceball_NextWait(s, SDL_SPACEBALL_STEP_ALIVE, now);
        }
        break;
    case SDL_SPACEBALL_STEP_ALIVE:
        if (Spaceball_WaitLine(s, '@', now)) {
            if (Spaceball_StartsWith(line, length, "@1 Spaceball alive")) {
                Spaceball_NextWait(s, SDL_SPACEBALL_STEP_FIRMWARE, now);
            } else {
                Spaceball_StartReset(s);
            }
        } else if (s->step == SDL_SPACEBALL_STEP_RETRY) {
            /* No banner: step 2 at once */
            s->deadline_set = false;
            Spaceball_StartReset(s);
        }
        break;
    case SDL_SPACEBALL_STEP_RESET:
        if (Spaceball_Contains(line, length, "@1")) {
            Spaceball_NextWait(s, SDL_SPACEBALL_STEP_FIRMWARE, now);
        }
        break;
    case SDL_SPACEBALL_STEP_FIRMWARE:
        if (Spaceball_WaitLine(s, '@', now)) {
            Spaceball_ParseFirmware(s, line, length);
            Spaceball_Command(s, "hm", SDL_SPACEBALL_STEP_MODEL);
        }
        break;
    case SDL_SPACEBALL_STEP_MODEL:
        if (Spaceball_WaitLine(s, 'H', now)) {
            char reply[SDL_SPACEBALL_LINE_LENGTH + 1];

            memcpy(reply, line, length);
            reply[length] = '\0';
            s->model = SDL_Spaceball_Classify(reply, s->firmware_major, s->firmware_minor);
            if (Spaceball_StartsWithNoCase(reply, "HvFirmware")) {
                Spaceball_Command(s, "\"", SDL_SPACEBALL_STEP_FLX_NAME);
            } else {
                Spaceball_Command(s, "P@A@A", SDL_SPACEBALL_STEP_P);
            }
        }
        break;
    case SDL_SPACEBALL_STEP_FLX_NAME:
        if (Spaceball_WaitLine(s, '"', now)) {
            if (Spaceball_StartsWith(line, length, "\"1 Spaceball 4000 FLX")) {
                Spaceball_NextWait(s, SDL_SPACEBALL_STEP_FLX_HAND, now);
            } else {
                Spaceball_Fail(s, now);
            }
        }
        break;
    case SDL_SPACEBALL_STEP_FLX_HAND:
        if (Spaceball_WaitLine(s, '"', now)) {
            s->model = Spaceball_Contains(line, length, " L ") ? SDL_SPACEBALL_MODEL_4000FLX_LEFTY : SDL_SPACEBALL_MODEL_4000FLX;
            Spaceball_NextWait(s, SDL_SPACEBALL_STEP_FLX_THIRD, now);
        }
        break;
    case SDL_SPACEBALL_STEP_FLX_THIRD:
        if (Spaceball_WaitLine(s, '"', now)) {
            Spaceball_Command(s, "YS", SDL_SPACEBALL_STEP_FLX_YS);
        }
        break;
    case SDL_SPACEBALL_STEP_FLX_YS:
        if (Spaceball_WaitLine(s, 'Y', now)) {
            Spaceball_Command(s, "M", SDL_SPACEBALL_STEP_FLX_M);
        }
        break;
    case SDL_SPACEBALL_STEP_P:
        if (Spaceball_WaitLine(s, 'P', now)) {
            Spaceball_Command(s, "FT@", SDL_SPACEBALL_STEP_F);
        }
        break;
    case SDL_SPACEBALL_STEP_F:
        if (Spaceball_WaitLine(s, 'F', now)) {
            Spaceball_Command(s, "MSS", SDL_SPACEBALL_STEP_M);
        }
        break;
    case SDL_SPACEBALL_STEP_FLX_M:
    case SDL_SPACEBALL_STEP_M:
        if (Spaceball_WaitLine(s, 'M', now)) {
            Spaceball_Present(s);
        }
        break;
    default:
        break;
    }
}

/* Start-up lines end at 0D. 0A bytes are skipped. */
static void Spaceball_LineByte(SDL_SpaceballState *s, uint8_t byte, uint64_t now)
{
    if (byte == SPACEBALL_LF) {
        return;
    }
    if (byte == SPACEBALL_CR) {
        Spaceball_HandleLine(s, now);
        s->line_length = 0;
        s->line_overflow = false;
        return;
    }
    if (s->line_length < sizeof(s->line)) {
        s->line[s->line_length++] = byte;
    } else {
        s->line_overflow = true;
    }
}

static void Spaceball_DecodeK(SDL_SpaceballState *s, const uint8_t *p)
{
    SDL_SerialControls controls = s->base.snapshots[0].controls;
    int i;

    for (i = 0; i < SDL_SPACEBALL_BUTTONS; ++i) {
        SDL_Serial_SetButton(&controls, i, false);
    }
    /* VRPN's layout: 0 and 1 are buttons 1 and 2 or the 3003's left and
     * right, 2 to 6 buttons 3 to 7, 7 either rezero bit, 8 the pick button */
    SDL_Serial_SetButton(&controls, 0, (p[2] & 0x01) || (p[2] & 0x10));
    SDL_Serial_SetButton(&controls, 1, (p[2] & 0x02) || (p[2] & 0x20));
    SDL_Serial_SetButton(&controls, 2, (p[2] & 0x04) != 0);
    SDL_Serial_SetButton(&controls, 3, (p[2] & 0x08) != 0);
    SDL_Serial_SetButton(&controls, 4, (p[1] & 0x01) != 0);
    SDL_Serial_SetButton(&controls, 5, (p[1] & 0x02) != 0);
    SDL_Serial_SetButton(&controls, 6, (p[1] & 0x04) != 0);
    SDL_Serial_SetButton(&controls, 7, (p[1] & 0x08) || (p[1] & 0x20));
    SDL_Serial_SetButton(&controls, 8, (p[1] & 0x10) != 0);
    SDL_Serial_Commit(&s->base, 0, &controls);
}

static void Spaceball_DecodeDot(SDL_SpaceballState *s, const uint8_t *p)
{
    SDL_SerialControls controls = s->base.snapshots[0].controls;
    int i;

    for (i = 0; i < 6; ++i) {
        SDL_Serial_SetButton(&controls, i, (p[2] & (1 << i)) != 0);
    }
    SDL_Serial_SetButton(&controls, 6, (p[2] & 0x80) != 0);
    for (i = 0; i < 5; ++i) {
        SDL_Serial_SetButton(&controls, 7 + i, (p[1] & (1 << i)) != 0);
    }
    /* Handedness is not a button. Clear means left-handed. */
    s->right_handed = (p[1] & 0x20) != 0;
    s->seen_dot = true;
    SDL_Serial_Commit(&s->base, 0, &controls);
}

static void Spaceball_ProcessPacket(SDL_SpaceballState *s, uint64_t now)
{
    const uint8_t *p = s->packet;
    const size_t length = s->packet_length;

    if (s->packet_overflow || length < 2) {
        return;
    }
    switch (p[0]) {
    case 'D':
        if (length == 15) {
            SDL_SerialControls controls = s->base.snapshots[0].controls;
            int i;

            /* Bytes 1 and 2 are the time since the previous D. Six signed
             * big-endian values follow, in wire order. */
            for (i = 0; i < SDL_SPACEBALL_AXES; ++i) {
                int32_t value = (p[3 + 2 * i] << 8) | p[4 + 2 * i];

                if (value >= 0x8000) {
                    value -= 0x10000;
                }
                controls.axes[i] = (int16_t)value;
            }
            SDL_Serial_Commit(&s->base, 0, &controls);
        }
        break;
    case 'K':
        if (length == 3 && !s->seen_dot) {
            Spaceball_DecodeK(s, p);
        }
        break;
    case '.':
        if (length == 3) {
            Spaceball_DecodeDot(s, p);
        }
        break;
    case '@':
        /* The ball reset. Start-up resumes at the firmware line. */
        SDL_Serial_Absent(&s->base, 0);
        s->model = SDL_SPACEBALL_MODEL_UNKNOWN;
        s->firmware_major = -1;
        s->firmware_minor = -1;
        s->line_length = 0;
        s->line_overflow = false;
        Spaceball_NextWait(s, SDL_SPACEBALL_STEP_FIRMWARE, now);
        break;
    case 'E':
        ++s->errors;
        SDL_Serial_Log(&s->base, "Spaceball reported a device error");
        break;
    case '?':
        ++s->errors;
        SDL_Serial_Log(&s->base, "Spaceball rejected a command");
        break;
    default:
        break; /* Command replies */
    }
}

/* Packets run from the type byte to a raw 0D. After '^', M, Q and S stand
 * for 0D, 11 and 13, and any other byte for itself. */
static void Spaceball_PacketByte(SDL_SpaceballState *s, uint8_t byte, uint64_t now)
{
    if (byte == SPACEBALL_CR) {
        Spaceball_ProcessPacket(s, now);
        s->packet_length = 0;
        s->packet_overflow = false;
        s->escape = false;
        return;
    }
    if (s->escape) {
        s->escape = false;
        if (byte == 'M' || byte == 'Q' || byte == 'S') {
            byte &= 0x1F;
        }
    } else if (byte == '^') {
        s->escape = true;
        return;
    }
    if (s->packet_length < sizeof(s->packet)) {
        s->packet[s->packet_length++] = byte;
    } else {
        s->packet_overflow = true;
    }
}

static void Spaceball_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_SpaceballState *s = (SDL_SpaceballState *)state;
    const SDL_SerialLine line = SDL_Serial_Line(SDL_SPACEBALL_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);
    const uint8_t write_seq = s->write_seq;

    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->write_seq = write_seq;
    s->firmware_major = -1;
    s->firmware_minor = -1;
    SDL_Serial_QueueLine(&s->base, 0, &line);
    s->step = SDL_SPACEBALL_STEP_BANNER;
    Spaceball_SetTimer(s, now + SDL_SPACEBALL_BANNER_MS);
}

static void Spaceball_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_SpaceballState *s = (SDL_SpaceballState *)state;
    size_t i;

    for (i = 0; i < length; ++i) {
        if (s->step == SDL_SPACEBALL_STEP_PRESENT) {
            Spaceball_PacketByte(s, data[i], now);
        } else {
            Spaceball_LineByte(s, data[i], now);
        }
    }
}

static void Spaceball_Tick(void *state, uint64_t now)
{
    SDL_SpaceballState *s = (SDL_SpaceballState *)state;

    if (!s->deadline_set || now < s->deadline) {
        return;
    }
    s->deadline_set = false;
    switch (s->step) {
    case SDL_SPACEBALL_STEP_BANNER:
    case SDL_SPACEBALL_STEP_ALIVE:
    case SDL_SPACEBALL_STEP_RETRY:
        Spaceball_StartReset(s);
        break;
    case SDL_SPACEBALL_STEP_PRESENT:
        break;
    default:
        Spaceball_Fail(s, now);
        break;
    }
}

static void Spaceball_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_SpaceballState *s = (SDL_SpaceballState *)state;
    uint8_t tag;

    if (!SDL_Serial_FinishAction(&s->base, &tag) || tag == 0 || tag != s->waiting_seq) {
        return;
    }
    s->waiting_seq = 0;
    if (!success) {
        Spaceball_Fail(s, now);
        return;
    }
    Spaceball_SetTimer(s, now + ((s->step == SDL_SPACEBALL_STEP_RESET) ? SDL_SPACEBALL_RESET_MS : SDL_SPACEBALL_REPLY_MS));
}

static void Spaceball_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

static bool Spaceball_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_SpaceballState *s = (SDL_SpaceballState *)state;

    if (!s->deadline_set) {
        return false;
    }
    *deadline = s->deadline;
    return true;
}

const SDL_SerialModule SDL_SerialSpaceballModule = {
    "spaceball",
    sizeof(SDL_SpaceballState),
    false,
    0,
    0,
    Spaceball_Reset,
    Spaceball_Feed,
    Spaceball_Tick,
    SDL_Serial_NextAction,
    Spaceball_ActionDone,
    Spaceball_Output,
    Spaceball_GetDeadline,
    SDL_Serial_GetSnapshot
};
