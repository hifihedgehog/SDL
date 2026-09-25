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

#include "SDL_serial_vrinsight_proto.h"

#include <stdio.h>
#include <string.h>

static const uint8_t vrinsight_connect[16] = { 'C', 'M', 'D', 'R', 'S', 'T', 0, 0, 'C', 'M', 'D', 'C', 'O', 'N', 0, 0 };
static const uint8_t vrinsight_keepalive[8] = { 'C', 'M', 'D', 'C', 'O', 'N', 0, 0 };
static const uint8_t vrinsight_function[8] = { 'C', 'M', 'D', 'F', 'U', 'N', 0, 0 };
static const uint8_t vrinsight_version[8] = { 'C', 'M', 'D', 'V', 'E', 'R', 0, 0 };

/* The key names of the CDU II driver's key map, in button order */
static const char *const vrinsight_cdu_keys[SDL_VRINSIGHT_CDU_BUTTONS] = {
    "LSKL1", "LSKL2", "LSKL3", "LSKL4", "LSKL5", "LSKL6",
    "LSKR1", "LSKR2", "LSKR3", "LSKR4", "LSKR5", "LSKR6",
    "KEY0", "KEY1", "KEY2", "KEY3", "KEY4", "KEY5", "KEY6", "KEY7", "KEY8", "KEY9",
    "KEYA", "KEYB", "KEYC", "KEYD", "KEYE", "KEYF", "KEYG", "KEYH", "KEYI", "KEYJ", "KEYK", "KEYL", "KEYM",
    "KEYN", "KEYO", "KEYP", "KEYQ", "KEYR", "KEYS", "KEYT", "KEYU", "KEYV", "KEYW", "KEYX", "KEYY", "KEYZ",
    "KEY+", "KEY.", "KEY/", "KEYSP", "KEYCLR", "KEYDEL",
    "FUN11", "FUN12", "FUN13", "FUN14", "FUN15", "FUN16",
    "FUN21", "FUN22", "FUN23", "FUN24", "FUN25", "FUN26",
    "FUN31", "FUN32", "FUN41", "FUN42"
};

/* Whether message bytes from offset on start with text */
static bool VRinsight_At(const uint8_t *message, int offset, const char *text)
{
    const size_t n = strlen(text);

    return (size_t)offset + n <= SDL_VRINSIGHT_MESSAGE && memcmp(message + offset, text, n) == 0;
}

static bool VRinsight_IsDigit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

SDL_VRinsightPanel SDL_VRinsight_Panel(const uint8_t *m)
{
    if (!VRinsight_At(m, 0, "CMD")) {
        return SDL_VRINSIGHT_PANEL_NONE;
    }
    if (VRinsight_At(m, 3, "CDU2")) {
        return SDL_VRINSIGHT_PANEL_CDU2;
    }
    if (VRinsight_At(m, 3, "FMER")) {
        return SDL_VRINSIGHT_PANEL_COMBO1;
    }
    if (VRinsight_At(m, 3, "MCP2A")) {
        return SDL_VRINSIGHT_PANEL_COMBO2_AIRBUS;
    }
    if (VRinsight_At(m, 3, "MCP2B")) {
        return SDL_VRINSIGHT_PANEL_COMBO2_BOEING;
    }
    if (VRinsight_At(m, 3, "MPanl")) {
        return SDL_VRINSIGHT_PANEL_MPANEL;
    }
    return SDL_VRINSIGHT_PANEL_OTHER;
}

int SDL_VRinsight_CduButton(const uint8_t *m)
{
    size_t length = 0;
    int i;

    while (length < SDL_VRINSIGHT_MESSAGE && m[length]) {
        ++length;
    }
    for (i = 0; i < SDL_VRINSIGHT_CDU_BUTTONS; ++i) {
        if (strlen(vrinsight_cdu_keys[i]) == length && memcmp(vrinsight_cdu_keys[i], m, length) == 0) {
            return i;
        }
    }
    return -1;
}

/* The command list of VRInsight-Xplane-Interface, in its order, without
 * the values no Combo I message presses */
int SDL_VRinsight_ComboButton(const uint8_t *m)
{
    switch (m[0]) {
    case 'A':
        if (VRinsight_At(m, 0, "ADF")) {
            if (VRinsight_At(m, 3, "SEL1")) {
                return 66;
            }
            if (VRinsight_At(m, 3, "SEL2")) {
                return 67;
            }
        } else if (VRinsight_At(m, 0, "ALT")) {
            if (VRinsight_At(m, 3, "HLD")) {
                return 0;
            }
            if (VRinsight_At(m, 3, "SEL")) {
                return 1;
            }
            if (VRinsight_IsDigit(m[3])) {
                return (m[6] == '+') ? 2 : 3;
            }
        } else if (VRinsight_At(m, 0, "APL")) {
            static const struct
            {
                const char *text;
                int button;
            } apl[] = {
                { "AT+", 18 }, { "AT-", 19 }, { "APP", 28 }, { "CMDA", 22 }, { "CMDB", 23 }, { "CMDC", 24 },
                { "CWSA", 25 }, { "CWSB", 26 }, { "FD+", 20 }, { "FD-", 21 }, { "LOC", 31 }, { "LNAV", 29 },
                { "MAST+", 16 }, { "MAST-", 17 }, { "TOGA", 27 }, { "VNAV", 30 }
            };
            size_t i;

            for (i = 0; i < sizeof(apl) / sizeof(apl[0]); ++i) {
                if (VRinsight_At(m, 3, apl[i].text)) {
                    return apl[i].button;
                }
            }
        }
        break;
    case 'B':
        if (VRinsight_At(m, 0, "BAR")) {
            switch (m[3]) {
            case '+':
                return 49;
            case '-':
                return 50;
            case 'S':
                return 51;
            default:
                break;
            }
        }
        break;
    case 'C':
        if (VRinsight_At(m, 0, "COM")) {
            if (VRinsight_At(m, 3, "SEL1")) {
                return 60;
            }
            if (VRinsight_At(m, 3, "SEL2")) {
                return 61;
            }
            if (VRinsight_At(m, 3, "AUX")) {
                return 62;
            }
        }
        break;
    case 'D':
        if (VRinsight_At(m, 0, "DME")) {
            if (VRinsight_At(m, 3, "SEL1")) {
                return 68;
            }
            if (VRinsight_At(m, 3, "SEL2")) {
                return 69;
            }
        }
        break;
    case 'E':
        if (VRinsight_At(m, 0, "EFI")) {
            static const struct
            {
                const char *text;
                int button;
            } efi[] = {
                { "VOR1", 37 }, { "ADF1", 38 }, { "VOR2", 39 }, { "ADF2", 40 }, { "ARPT", 41 }, { "FPV", 42 },
                { "WX", 43 }, { "STA", 44 }, { "WPT", 45 }, { "DATA", 46 }, { "POS", 47 }, { "TERR", 48 },
                { "MTR", 52 }
            };
            size_t i;

            for (i = 0; i < sizeof(efi) / sizeof(efi[0]); ++i) {
                if (VRinsight_At(m, 3, efi[i].text)) {
                    return efi[i].button;
                }
            }
        }
        break;
    case 'H':
        if (VRinsight_At(m, 0, "HDG")) {
            if (VRinsight_At(m, 3, "HLD")) {
                return 4;
            }
            if (VRinsight_At(m, 3, "HDG")) {
                return 5;
            }
            if (VRinsight_IsDigit(m[3])) {
                return (m[6] == '+') ? 6 : 7;
            }
        }
        break;
    case 'M':
        if (VRinsight_At(m, 0, "MIN")) {
            switch (m[3]) {
            case '+':
                return 53;
            case '-':
                return 54;
            case 'S':
                return 55;
            default:
                break;
            }
        }
        break;
    case 'N':
        if (VRinsight_At(m, 0, "NAV")) {
            if (VRinsight_At(m, 3, "SEL1")) {
                return 63;
            }
            if (VRinsight_At(m, 3, "SEL2")) {
                return 64;
            }
            if (VRinsight_At(m, 3, "AUX")) {
                return 65;
            }
        } else if (VRinsight_At(m, 0, "NDR")) {
            switch (m[3]) {
            case '-':
                return 33;
            case '+':
                return 34;
            case 'S':
                return 57;
            default:
                break;
            }
        } else if (VRinsight_At(m, 0, "NDM")) {
            switch (m[3]) {
            case '+':
                return 35;
            case '-':
                return 36;
            case 'S':
                return 56;
            default:
                break;
            }
        }
        break;
    case 'O':
        if (VRinsight_At(m, 0, "OBS")) {
            if (m[3] == '+') {
                return 58;
            }
            if (m[3] == '-') {
                return 59;
            }
        }
        break;
    case 'S':
        /* The plugin looks at bytes 3 on, whatever bytes 1 and 2 hold */
        switch (m[3]) {
        case 'L':
            return 8;
        case 'N':
            return 9;
        case 'S':
            return (m[4] == 'E') ? 11 : 10;
        default:
            if (VRinsight_IsDigit(m[3])) {
                if (m[6] == '+') {
                    return 12;
                }
                if (m[6] == '-') {
                    return 13;
                }
            }
            break;
        }
        break;
    case 'T':
        if (VRinsight_At(m, 0, "TRN")) {
            if (VRinsight_At(m, 3, "SEL")) {
                return 70;
            }
            if (VRinsight_At(m, 3, "AUX")) {
                return 71;
            }
        }
        break;
    case 'V':
        switch (m[3]) {
        case '+':
            return 14;
        case '-':
            return 15;
        case 'H':
            return 32;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return -1;
}

static uint8_t VRinsight_NextSeq(SDL_VRinsightState *s)
{
    if (++s->action_seq == 0 || s->action_seq == 0xFF) {
        s->action_seq = 1;
    }
    return s->action_seq;
}

static void VRinsight_Write(SDL_VRinsightState *s, const uint8_t *data, size_t length)
{
    s->waiting_seq = VRinsight_NextSeq(s);
    s->timer = false;
    SDL_Serial_QueueWrite(&s->base, s->waiting_seq, data, length);
}

/* CMDRST and CMDCON, then the answer within 3 s */
static void VRinsight_Connect(SDL_VRinsightState *s)
{
    s->step = SDL_VRINSIGHT_STEP_CONNECT;
    s->panel = SDL_VRINSIGHT_PANEL_NONE;
    VRinsight_Write(s, vrinsight_connect, sizeof(vrinsight_connect));
}

static void VRinsight_Identified(SDL_VRinsightState *s, SDL_VRinsightPanel panel, uint64_t now)
{
    SDL_SerialIdentity identity;

    s->panel = panel;
    switch (panel) {
    case SDL_VRINSIGHT_PANEL_CDU2:
        SDL_Serial_SetIdentity(&identity, "VRinsight CDU II", SDL_SERIAL_TYPE_UNKNOWN, 0, SDL_VRINSIGHT_CDU_BUTTONS, 0, 0);
        break;
    case SDL_VRINSIGHT_PANEL_COMBO1:
        SDL_Serial_SetIdentity(&identity, "VRinsight MCP Combo", SDL_SERIAL_TYPE_UNKNOWN, 0, SDL_VRINSIGHT_COMBO_BUTTONS, 0, 0);
        break;
    default:
    {
        static const char *const names[] = { "", "", "", "MCP Combo II Airbus", "MCP Combo II Boeing", "M-Panel", "an unknown panel" };
        char text[80];

        (void)snprintf(text, sizeof(text), "VRinsight %s identified, its messages are not known", names[panel]);
        SDL_Serial_Log(&s->base, text);
        s->step = SDL_VRINSIGHT_STEP_UNSUPPORTED;
        VRinsight_Write(s, vrinsight_version, sizeof(vrinsight_version));
        return;
    }
    }
    SDL_Serial_Present(&s->base, 0, &identity);
    s->step = SDL_VRINSIGHT_STEP_PRESENT;
    VRinsight_Write(s, vrinsight_version, sizeof(vrinsight_version));
    s->timer = true;
    s->deadline = now + SDL_VRINSIGHT_KEEPALIVE_MS;
}

static void VRinsight_Message(SDL_VRinsightState *s, uint64_t now)
{
    const uint8_t *m = s->message;
    const bool reply = VRinsight_At(m, 0, "CMD");

    switch (s->step) {
    case SDL_VRINSIGHT_STEP_CONNECT:
        if (VRinsight_At(m, 0, "CMDCON")) {
            s->step = SDL_VRINSIGHT_STEP_FUNCTION;
            VRinsight_Write(s, vrinsight_function, sizeof(vrinsight_function));
        }
        break;
    case SDL_VRINSIGHT_STEP_FUNCTION:
        if (reply && !VRinsight_At(m, 0, "CMDCON")) {
            VRinsight_Identified(s, SDL_VRinsight_Panel(m), now);
        }
        break;
    case SDL_VRINSIGHT_STEP_KEEPALIVE:
    case SDL_VRINSIGHT_STEP_PRESENT:
        /* Any message is life */
        s->step = SDL_VRINSIGHT_STEP_PRESENT;
        s->timer = true;
        s->deadline = now + SDL_VRINSIGHT_KEEPALIVE_MS;
        if (reply) {
            if (VRinsight_IsDigit(m[3])) {
                memcpy(s->version, m + 3, 5);
                s->version[5] = '\0';
            }
            break;
        }
        {
            const int button = (s->panel == SDL_VRINSIGHT_PANEL_CDU2) ? SDL_VRinsight_CduButton(m) : SDL_VRinsight_ComboButton(m);

            if (button >= 0) {
                SDL_Serial_Pulse(&s->base, 0, button, now);
            }
        }
        break;
    default:
        break;
    }
}

static void VRinsight_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_VRinsightState *s = (SDL_VRinsightState *)state;
    const SDL_SerialLine line = SDL_Serial_Line(SDL_VRINSIGHT_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);
    const uint8_t action_seq = s->action_seq;

    (void)now;
    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->action_seq = action_seq;
    SDL_Serial_QueueLine(&s->base, 0, &line);
    VRinsight_Connect(s);
}

/* Fixed 8-byte chunks. A chunk must start with A to Z, or its first byte
 * is dropped and the next one tried. */
static void VRinsight_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_VRinsightState *s = (SDL_VRinsightState *)state;
    size_t i;

    for (i = 0; i < length; ++i) {
        if (s->message_length == 0 && (data[i] < 'A' || data[i] > 'Z')) {
            ++s->dropped;
            continue;
        }
        s->message[s->message_length++] = data[i];
        if (s->message_length == SDL_VRINSIGHT_MESSAGE) {
            s->message_length = 0;
            VRinsight_Message(s, now);
        }
    }
}

static void VRinsight_Tick(void *state, uint64_t now)
{
    SDL_VRinsightState *s = (SDL_VRinsightState *)state;

    SDL_Serial_TickPulses(&s->base, now);
    if (!s->timer || now < s->deadline) {
        return;
    }
    s->timer = false;
    switch (s->step) {
    case SDL_VRINSIGHT_STEP_CONNECT:
    case SDL_VRINSIGHT_STEP_FUNCTION:
        VRinsight_Connect(s);
        break;
    case SDL_VRINSIGHT_STEP_PRESENT:
        /* 60 s of quiet: CMDCON, answered within 3 s */
        s->step = SDL_VRINSIGHT_STEP_KEEPALIVE;
        VRinsight_Write(s, vrinsight_keepalive, sizeof(vrinsight_keepalive));
        break;
    case SDL_VRINSIGHT_STEP_KEEPALIVE:
        SDL_Serial_Absent(&s->base, 0);
        VRinsight_Connect(s);
        break;
    default:
        break;
    }
}

static void VRinsight_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_VRinsightState *s = (SDL_VRinsightState *)state;
    uint8_t tag;

    if (!SDL_Serial_FinishAction(&s->base, &tag) || tag == 0 || tag != s->waiting_seq) {
        return;
    }
    s->waiting_seq = 0;
    (void)success;
    switch (s->step) {
    case SDL_VRINSIGHT_STEP_CONNECT:
    case SDL_VRINSIGHT_STEP_FUNCTION:
    case SDL_VRINSIGHT_STEP_KEEPALIVE:
        s->timer = true;
        s->deadline = now + SDL_VRINSIGHT_REPLY_MS;
        break;
    default:
        break;
    }
}

/* An 8-byte effect goes out as one message */
static void VRinsight_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    SDL_VRinsightState *s = (SDL_VRinsightState *)state;

    (void)now;
    if (request->kind != SDL_SERIAL_OUTPUT_EFFECT || request->length != SDL_VRINSIGHT_MESSAGE || !SDL_Serial_IsPresent(&s->base, 0)) {
        return;
    }
    SDL_Serial_QueueWrite(&s->base, 0xFF, request->data, request->length);
}

static bool VRinsight_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_VRinsightState *s = (SDL_VRinsightState *)state;
    bool have = false;

    if (s->timer) {
        SDL_Serial_EarlierDeadline(&have, deadline, s->deadline);
    }
    SDL_Serial_PulseDeadline(&s->base, &have, deadline);
    return have;
}

const SDL_SerialModule SDL_SerialVRinsightModule = {
    "vrinsight",
    sizeof(SDL_VRinsightState),
    false,
    SDL_VRINSIGHT_MESSAGE,
    SDL_VRINSIGHT_MESSAGE,
    VRinsight_Reset,
    VRinsight_Feed,
    VRinsight_Tick,
    SDL_Serial_NextAction,
    VRinsight_ActionDone,
    VRinsight_Output,
    VRinsight_GetDeadline,
    SDL_Serial_GetSnapshot
};
