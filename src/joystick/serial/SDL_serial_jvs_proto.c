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

#include "SDL_serial_jvs_proto.h"

#include <stdio.h>
#include <string.h>

#define JVS_CMD_RESET       0xF0
#define JVS_CMD_SET_ADDRESS 0xF1
#define JVS_CMD_IDENTIFY    0x10
#define JVS_CMD_FEATURES    0x14
#define JVS_CMD_SWITCHES    0x20
#define JVS_CMD_COINS       0x21
#define JVS_CMD_ANALOG      0x22
#define JVS_CMD_ROTARY      0x23
#define JVS_CMD_OUTPUT      0x32
#define JVS_RESET_MAGIC     0xD9
#define JVS_NORMAL          0x01

size_t SDL_JVS_BuildFrame(uint8_t node, const uint8_t *data, size_t length, uint8_t *out, size_t out_size)
{
    uint8_t raw[2 + 254 + 1];
    size_t n = 0, o = 0, i;
    uint8_t sum = 0;

    if (length > 254 || (length && !data) || !out) {
        return 0;
    }
    raw[n++] = node;
    raw[n++] = (uint8_t)(length + 1);
    if (length) {
        memcpy(raw + n, data, length);
        n += length;
    }
    for (i = 0; i < n; ++i) {
        sum = (uint8_t)(sum + raw[i]);
    }
    raw[n++] = sum;

    if (o >= out_size) {
        return 0;
    }
    out[o++] = SDL_JVS_SYNC;
    /* Inside a frame E0 and D0 travel as D0 and the value less one */
    for (i = 0; i < n; ++i) {
        if (raw[i] == SDL_JVS_SYNC || raw[i] == SDL_JVS_MARK) {
            if (o + 2 > out_size) {
                return 0;
            }
            out[o++] = SDL_JVS_MARK;
            out[o++] = (uint8_t)(raw[i] - 1);
        } else {
            if (o + 1 > out_size) {
                return 0;
            }
            out[o++] = raw[i];
        }
    }
    return o;
}

static uint8_t JVS_NextSeq(SDL_JVSState *s)
{
    if (++s->action_seq == 0) {
        s->action_seq = 1;
    }
    return s->action_seq;
}

static void JVS_SetTimer(SDL_JVSState *s, uint64_t at)
{
    s->deadline = at;
    s->timer = true;
}

/* Writes a frame. Its completion is awaited by tag. A frame that cannot be
 * queued times out at once, so no step waits for it forever. */
static void JVS_Send(SDL_JVSState *s, uint8_t node, const uint8_t *data, size_t length)
{
    uint8_t frame[SDL_SERIAL_MAX_WRITE];
    const size_t n = SDL_JVS_BuildFrame(node, data, length, frame, sizeof(frame));

    s->waiting_seq = JVS_NextSeq(s);
    s->timer = false;
    if (n == 0 || !SDL_Serial_QueueWrite(&s->base, s->waiting_seq, frame, n)) {
        s->waiting_seq = 0;
        JVS_SetTimer(s, 0);
    }
}

/* A request whose answer is due 100 ms after it leaves */
static void JVS_Request(SDL_JVSState *s, uint8_t node, const uint8_t *data, size_t length)
{
    JVS_Send(s, node, data, length);
    s->awaiting = true;
}

/* Step 2: reset, 500 ms, reset again */
static void JVS_StartReset(SDL_JVSState *s)
{
    static const uint8_t reset[2] = { JVS_CMD_RESET, JVS_RESET_MAGIC };

    memset(s->board_info, 0, sizeof(s->board_info));
    s->boards = 0;
    s->board = 0;
    s->players = 0;
    s->poll_board_of_player1 = -1;
    s->awaiting = false;
    s->step = SDL_JVS_STEP_RESET1;
    JVS_Send(s, SDL_JVS_BROADCAST, reset, sizeof(reset));
}

/* No answer within 100 ms, or a status or report other than 01: every
 * joystick goes, and the resets follow 500 ms later */
static void JVS_Error(SDL_JVSState *s, uint64_t now)
{
    ++s->errors;
    SDL_Serial_ClearActions(&s->base);
    SDL_Serial_AbsentAll(&s->base);
    s->waiting_seq = 0;
    s->awaiting = false;
    s->players = 0;
    s->step = SDL_JVS_STEP_ERROR;
    JVS_SetTimer(s, now + SDL_JVS_ERROR_MS);
}

static void JVS_Address(SDL_JVSState *s, int address)
{
    uint8_t data[2];

    data[0] = JVS_CMD_SET_ADDRESS;
    data[1] = (uint8_t)address;
    s->address = address;
    s->step = SDL_JVS_STEP_ADDRESS;
    JVS_Request(s, SDL_JVS_BROADCAST, data, sizeof(data));
}

static void JVS_Identify(SDL_JVSState *s, int board, uint8_t command)
{
    s->board = board;
    s->command = command;
    s->step = SDL_JVS_STEP_IDENTIFY;
    JVS_Request(s, (uint8_t)(board + 1), &command, 1);
}

static int JVS_SwitchBytes(const SDL_JVSBoard *b)
{
    return (b->switches + 7) / 8;
}

static bool JVS_BoardPolls(const SDL_JVSBoard *b)
{
    return (b->players && JVS_SwitchBytes(b)) || b->slots || b->analog || b->rotary;
}

static void JVS_Poll(SDL_JVSState *s, int board)
{
    SDL_JVSBoard *b = &s->board_info[board];
    uint8_t data[32];
    size_t n = 0;

    if (b->players && JVS_SwitchBytes(b)) {
        data[n++] = JVS_CMD_SWITCHES;
        data[n++] = b->players;
        data[n++] = (uint8_t)JVS_SwitchBytes(b);
    }
    if (b->slots) {
        data[n++] = JVS_CMD_COINS;
        data[n++] = b->slots;
    }
    if (b->analog) {
        data[n++] = JVS_CMD_ANALOG;
        data[n++] = b->analog;
    }
    if (b->rotary) {
        data[n++] = JVS_CMD_ROTARY;
        data[n++] = b->rotary;
    }
    b->output_sent = false;
    if (b->output_pending) {
        data[n++] = JVS_CMD_OUTPUT;
        data[n++] = b->output_length;
        memcpy(data + n, b->output, b->output_length);
        n += b->output_length;
        b->output_pending = false;
        b->output_sent = true;
    }
    s->board = board;
    s->step = SDL_JVS_STEP_POLL;
    JVS_Request(s, (uint8_t)(board + 1), data, n);
}

/* The next board with something to poll after the one just polled */
static void JVS_NextPoll(SDL_JVSState *s)
{
    int i;

    for (i = 1; i <= s->boards; ++i) {
        const int board = (s->board + i) % s->boards;

        if (JVS_BoardPolls(&s->board_info[board])) {
            JVS_Poll(s, board);
            return;
        }
    }
    s->step = SDL_JVS_STEP_POLL;
    s->awaiting = false;
}

static void JVS_PresentPlayers(SDL_JVSState *s)
{
    int board, total = 0, g;

    s->poll_board_of_player1 = -1;
    for (board = 0; board < s->boards; ++board) {
        SDL_JVSBoard *b = &s->board_info[board];
        const int room = SDL_JVS_MAX_PLAYERS - total;

        b->first_player = (uint8_t)total;
        b->mapped_players = (uint8_t)((b->players < room) ? b->players : room);
        if (b->mapped_players && total == 0) {
            s->poll_board_of_player1 = board;
        }
        total += b->mapped_players;
    }
    s->players = total;
    for (g = 0; g < total; ++g) {
        SDL_SerialIdentity identity;
        char name[SDL_SERIAL_NAME_LENGTH];
        int naxes = 0;

        if (g == 0) {
            const SDL_JVSBoard *b = &s->board_info[s->poll_board_of_player1];
            const int analog = (b->analog < 8) ? b->analog : 8;
            const int rotary = (analog + b->rotary > SDL_SERIAL_MAX_AXES) ? SDL_SERIAL_MAX_AXES - analog : b->rotary;

            naxes = analog + rotary;
        }
        (void)snprintf(name, sizeof(name), "JVS I/O Player %d", g + 1);
        SDL_Serial_SetIdentity(&identity, name, SDL_SERIAL_TYPE_ARCADE_STICK, naxes, (g == 0) ? 14 : 13, 1, 0);
        /* JoypadOS's arcade layout. Analog and rotary axes stay out. */
        identity.has_mapping = true;
        identity.mapping.x = SDL_Serial_MapButton(0);
        identity.mapping.y = SDL_Serial_MapButton(1);
        identity.mapping.rightshoulder = SDL_Serial_MapButton(2);
        identity.mapping.a = SDL_Serial_MapButton(3);
        identity.mapping.b = SDL_Serial_MapButton(4);
        identity.mapping.righttrigger = SDL_Serial_MapButton(5);
        identity.mapping.leftshoulder = SDL_Serial_MapButton(6);
        identity.mapping.lefttrigger = SDL_Serial_MapButton(7);
        identity.mapping.leftstick = SDL_Serial_MapButton(8);
        identity.mapping.rightstick = SDL_Serial_MapButton(9);
        identity.mapping.start = SDL_Serial_MapButton(SDL_JVS_BUTTON_START);
        identity.mapping.back = SDL_Serial_MapButton(SDL_JVS_BUTTON_SERVICE);
        identity.mapping.misc1 = SDL_Serial_MapButton(SDL_JVS_BUTTON_COIN);
        if (g == 0) {
            identity.mapping.guide = SDL_Serial_MapButton(SDL_JVS_BUTTON_TEST);
        }
        identity.mapping.dpup = SDL_Serial_MapHat(0, SDL_SERIAL_HAT_UP);
        identity.mapping.dpdown = SDL_Serial_MapHat(0, SDL_SERIAL_HAT_DOWN);
        identity.mapping.dpleft = SDL_Serial_MapHat(0, SDL_SERIAL_HAT_LEFT);
        identity.mapping.dpright = SDL_Serial_MapHat(0, SDL_SERIAL_HAT_RIGHT);
        SDL_Serial_Present(&s->base, g, &identity);
    }
    if (total == 0) {
        SDL_Serial_Log(&s->base, "No JVS board reports player switches");
    }
}

/* Every board answered 10 to 14: joysticks, then the polls */
static void JVS_FinishIdentify(SDL_JVSState *s)
{
    JVS_PresentPlayers(s);
    s->board = s->boards - 1;
    JVS_NextPoll(s);
}

static bool JVS_ParseFeatures(SDL_JVSBoard *b, const uint8_t *entries, size_t length)
{
    size_t i = 0;

    while (i < length) {
        const uint8_t code = entries[i];

        if (code == 0x00) {
            return true;
        }
        if (i + 4 > length) {
            return false;
        }
        switch (code) {
        case 0x01:
            b->players = entries[i + 1];
            b->switches = entries[i + 2];
            break;
        case 0x02:
            b->slots = entries[i + 1];
            break;
        case 0x03:
            b->analog = entries[i + 1];
            b->analog_bits = entries[i + 2];
            break;
        case 0x04:
            b->rotary = entries[i + 1];
            break;
        case 0x12:
            b->outputs = entries[i + 1];
            break;
        default:
            break;
        }
        i += 4;
    }
    return false; /* No 00 at the end */
}

static void JVS_IdentifyReply(SDL_JVSState *s, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_JVSBoard *b = &s->board_info[s->board];

    if (length < 2 || data[1] != JVS_NORMAL) {
        JVS_Error(s, now);
        return;
    }
    switch (s->command) {
    case JVS_CMD_IDENTIFY:
    {
        const uint8_t *text = data + 2;
        const size_t available = length - 2;
        const uint8_t *end = (const uint8_t *)memchr(text, 0, available);
        size_t n;
        char line[SDL_JVS_TEXT_LENGTH + 32];

        if (!end) {
            JVS_Error(s, now);
            return;
        }
        n = (size_t)(end - text);
        if (n > SDL_JVS_TEXT_LENGTH - 1) {
            n = SDL_JVS_TEXT_LENGTH - 1;
        }
        memcpy(b->ident, text, n);
        b->ident[n] = '\0';
        (void)snprintf(line, sizeof(line), "JVS board %d: %s", s->board + 1, b->ident);
        SDL_Serial_Log(&s->base, line);
        break;
    }
    case 0x11:
    case 0x12:
    case 0x13:
        if (length != 3) {
            JVS_Error(s, now);
            return;
        }
        if (s->command == 0x11) {
            b->command_revision = data[2];
        } else if (s->command == 0x12) {
            b->jvs_revision = data[2];
        } else {
            b->communication_revision = data[2];
        }
        break;
    case JVS_CMD_FEATURES:
        if (!JVS_ParseFeatures(b, data + 2, length - 2)) {
            JVS_Error(s, now);
            return;
        }
        break;
    default:
        JVS_Error(s, now);
        return;
    }

    if (s->command < JVS_CMD_FEATURES) {
        JVS_Identify(s, s->board, (uint8_t)(s->command + 1));
    } else if (s->board + 1 < s->boards) {
        JVS_Identify(s, s->board + 1, JVS_CMD_IDENTIFY);
    } else {
        JVS_FinishIdentify(s);
    }
}

/* A 16-bit value with bit 15 inverted, so 8000 reads 0 */
static int16_t JVS_Centered(uint16_t value)
{
    const int32_t v = (int32_t)(value ^ 0x8000);

    return (int16_t)((v >= 0x8000) ? v - 0x10000 : v);
}

static void JVS_PollReply(SDL_JVSState *s, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_JVSBoard *b = &s->board_info[s->board];
    const int bytes = JVS_SwitchBytes(b);
    size_t p = 1, switch_at = 0, coin_at = 0, analog_at = 0, rotary_at = 0;
    int i;

    /* Check the whole answer before using any of it */
    if (b->players && bytes) {
        if (p >= length || data[p] != JVS_NORMAL) {
            JVS_Error(s, now);
            return;
        }
        switch_at = p + 1;
        p += 2 + (size_t)b->players * (size_t)bytes;
    }
    if (b->slots) {
        if (p >= length || data[p] != JVS_NORMAL) {
            JVS_Error(s, now);
            return;
        }
        coin_at = p + 1;
        p += 1 + 2 * (size_t)b->slots;
    }
    if (b->analog) {
        if (p >= length || data[p] != JVS_NORMAL) {
            JVS_Error(s, now);
            return;
        }
        analog_at = p + 1;
        p += 1 + 2 * (size_t)b->analog;
    }
    if (b->rotary) {
        if (p >= length || data[p] != JVS_NORMAL) {
            JVS_Error(s, now);
            return;
        }
        rotary_at = p + 1;
        p += 1 + 2 * (size_t)b->rotary;
    }
    if (b->output_sent) {
        if (p >= length || data[p] != JVS_NORMAL) {
            JVS_Error(s, now);
            return;
        }
        p += 1;
    }
    if (p != length) {
        JVS_Error(s, now);
        return;
    }

    /* Switches, and for player 1 TEST, analog and rotary */
    if (b->players && bytes) {
        const uint8_t system = data[switch_at];

        for (i = 0; i < b->mapped_players; ++i) {
            const int g = b->first_player + i;
            const uint8_t *player = data + switch_at + 1 + (size_t)i * (size_t)bytes;
            const uint8_t b0 = player[0];
            const uint8_t b1 = (bytes >= 2) ? player[1] : 0;
            SDL_SerialControls controls = s->base.snapshots[g].controls;
            uint8_t hat = 0;
            int bit;

            SDL_Serial_SetButton(&controls, 0, (b0 & 0x02) != 0);
            SDL_Serial_SetButton(&controls, 1, (b0 & 0x01) != 0);
            for (bit = 0; bit < 8; ++bit) {
                SDL_Serial_SetButton(&controls, 2 + bit, (b1 & (0x80 >> bit)) != 0);
            }
            SDL_Serial_SetButton(&controls, SDL_JVS_BUTTON_START, (b0 & 0x80) != 0);
            SDL_Serial_SetButton(&controls, SDL_JVS_BUTTON_SERVICE, (b0 & 0x40) != 0);
            if (b0 & 0x20) {
                hat |= SDL_SERIAL_HAT_UP;
            }
            if (b0 & 0x10) {
                hat |= SDL_SERIAL_HAT_DOWN;
            }
            if (b0 & 0x08) {
                hat |= SDL_SERIAL_HAT_LEFT;
            }
            if (b0 & 0x04) {
                hat |= SDL_SERIAL_HAT_RIGHT;
            }
            controls.hats[0] = hat;
            if (g == 0) {
                SDL_Serial_SetButton(&controls, SDL_JVS_BUTTON_TEST, (system & 0x80) != 0);
            }
            if (g == 0 && s->board == s->poll_board_of_player1) {
                const int analog = (b->analog < 8) ? b->analog : 8;
                int axis = 0, c;

                for (c = 0; c < analog; ++c) {
                    controls.axes[axis++] = JVS_Centered((uint16_t)((data[analog_at + 2 * (size_t)c] << 8) | data[analog_at + 2 * (size_t)c + 1]));
                }
                for (c = 0; c < b->rotary && axis < SDL_SERIAL_MAX_AXES; ++c) {
                    controls.axes[axis++] = JVS_Centered((uint16_t)((data[rotary_at + 2 * (size_t)c] << 8) | data[rotary_at + 2 * (size_t)c + 1]));
                }
            }
            SDL_Serial_Commit(&s->base, g, &controls);
        }
    }

    /* Coins: a count that rose pulses the coin button once per coin. The
     * first poll sets the baseline and a slot reporting a condition is
     * skipped. */
    for (i = 0; i < b->slots && i < SDL_JVS_MAX_PLAYERS; ++i) {
        const uint16_t value = (uint16_t)((data[coin_at + 2 * (size_t)i] << 8) | data[coin_at + 2 * (size_t)i + 1]);
        const uint16_t count = (uint16_t)(value & 0x3FFF);
        uint16_t delta;
        int pulses;

        if (i >= b->mapped_players || (value & 0xC000)) {
            continue;
        }
        if (!b->coin_baseline[i]) {
            b->coins[i] = count;
            b->coin_baseline[i] = true;
            continue;
        }
        delta = (uint16_t)((count - b->coins[i]) & 0x3FFF);
        b->coins[i] = count;
        if (delta == 0 || delta >= 0x2000) {
            continue; /* No coin, or a count that went back */
        }
        for (pulses = 0; pulses < delta && pulses < SDL_JVS_COIN_PULSES; ++pulses) {
            SDL_Serial_Pulse(&s->base, b->first_player + i, SDL_JVS_BUTTON_COIN, now);
        }
    }

    JVS_NextPoll(s);
}

static void JVS_Frame(SDL_JVSState *s, uint64_t now)
{
    const uint8_t *rx = s->rx;
    const size_t total = s->rx_length;
    const uint8_t *data;
    size_t length, i;
    uint8_t sum = 0;

    /* Frames for other nodes, such as the host's own echoed request */
    if (rx[0] != SDL_JVS_HOST || !s->awaiting) {
        return;
    }
    for (i = 0; i + 1 < total; ++i) {
        sum = (uint8_t)(sum + rx[i]);
    }
    if (sum != rx[total - 1] || rx[1] < 2) {
        JVS_Error(s, now);
        return;
    }
    data = rx + 2;
    length = (size_t)rx[1] - 1;
    s->awaiting = false;
    s->timer = false;
    s->waiting_seq = 0;
    if (data[0] != JVS_NORMAL) {
        JVS_Error(s, now);
        return;
    }
    switch (s->step) {
    case SDL_JVS_STEP_ADDRESS:
        if (length != 2 || data[1] != JVS_NORMAL) {
            JVS_Error(s, now);
            return;
        }
        s->boards = s->address;
        if (s->address < SDL_JVS_MAX_BOARDS) {
            JVS_Address(s, s->address + 1);
        } else {
            JVS_Identify(s, 0, JVS_CMD_IDENTIFY);
        }
        break;
    case SDL_JVS_STEP_IDENTIFY:
        JVS_IdentifyReply(s, data, length, now);
        break;
    case SDL_JVS_STEP_POLL:
        JVS_PollReply(s, data, length, now);
        break;
    default:
        break;
    }
}

static void JVS_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_JVSState *s = (SDL_JVSState *)state;
    const SDL_SerialLine line = SDL_Serial_Line(SDL_JVS_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_RTS_TOGGLE, true, false);
    const uint8_t action_seq = s->action_seq;

    (void)now;
    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->action_seq = action_seq;
    SDL_Serial_QueueLine(&s->base, 0, &line);
    JVS_StartReset(s);
}

static void JVS_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_JVSState *s = (SDL_JVSState *)state;
    size_t i;

    for (i = 0; i < length; ++i) {
        uint8_t byte = data[i];

        /* E0 always starts a frame */
        if (byte == SDL_JVS_SYNC) {
            s->rx_in = true;
            s->rx_escape = false;
            s->rx_length = 0;
            continue;
        }
        if (!s->rx_in) {
            continue;
        }
        if (byte == SDL_JVS_MARK) {
            s->rx_escape = true;
            continue;
        }
        if (s->rx_escape) {
            byte = (uint8_t)(byte + 1);
            s->rx_escape = false;
        }
        s->rx[s->rx_length++] = byte;
        if (s->rx_length >= 2 && s->rx_length == 2 + (size_t)s->rx[1]) {
            s->rx_in = false;
            if (s->rx[1] >= 1) {
                JVS_Frame(s, now);
            }
        }
    }
}

static void JVS_Tick(void *state, uint64_t now)
{
    SDL_JVSState *s = (SDL_JVSState *)state;
    static const uint8_t reset[2] = { JVS_CMD_RESET, JVS_RESET_MAGIC };

    SDL_Serial_TickPulses(&s->base, now);
    if (!s->timer || now < s->deadline) {
        return;
    }
    s->timer = false;
    switch (s->step) {
    case SDL_JVS_STEP_RESET1:
        s->step = SDL_JVS_STEP_RESET2;
        JVS_Send(s, SDL_JVS_BROADCAST, reset, sizeof(reset));
        break;
    case SDL_JVS_STEP_ADDRESS:
        /* The first unanswered address ends the scan */
        s->awaiting = false;
        s->boards = s->address - 1;
        if (s->boards == 0) {
            s->step = SDL_JVS_STEP_SCAN;
            JVS_SetTimer(s, now + SDL_JVS_SCAN_MS);
        } else {
            JVS_Identify(s, 0, JVS_CMD_IDENTIFY);
        }
        break;
    case SDL_JVS_STEP_IDENTIFY:
        /* A board that answers every later set address while it keeps its
         * first one, as jvsio's node does, makes the scan count too many.
         * A board after the first that is silent at its first identify
         * request ends the list. */
        if (s->board > 0 && s->command == JVS_CMD_IDENTIFY) {
            s->awaiting = false;
            s->boards = s->board;
            JVS_FinishIdentify(s);
        } else {
            JVS_Error(s, now);
        }
        break;
    case SDL_JVS_STEP_POLL:
        JVS_Error(s, now);
        break;
    case SDL_JVS_STEP_ERROR:
    case SDL_JVS_STEP_SCAN:
        JVS_StartReset(s);
        break;
    default:
        break;
    }
}

static void JVS_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_JVSState *s = (SDL_JVSState *)state;
    uint8_t tag;

    if (!SDL_Serial_FinishAction(&s->base, &tag) || tag == 0 || tag != s->waiting_seq) {
        return;
    }
    s->waiting_seq = 0;
    if (!success) {
        JVS_Error(s, now);
        return;
    }
    switch (s->step) {
    case SDL_JVS_STEP_RESET1:
        JVS_SetTimer(s, now + SDL_JVS_RESET_MS);
        break;
    case SDL_JVS_STEP_RESET2:
        JVS_Address(s, 1);
        break;
    case SDL_JVS_STEP_ADDRESS:
    case SDL_JVS_STEP_IDENTIFY:
    case SDL_JVS_STEP_POLL:
        if (s->awaiting) {
            JVS_SetTimer(s, now + SDL_JVS_REPLY_MS);
        }
        break;
    default:
        break;
    }
}

/* An effect goes to the board of the player it was made on, as command 32
 * in that board's next poll */
static void JVS_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    SDL_JVSState *s = (SDL_JVSState *)state;
    int board;

    (void)now;
    if (request->kind != SDL_SERIAL_OUTPUT_EFFECT || request->length == 0 || request->length > SDL_JVS_MAX_OUTPUT) {
        return;
    }
    for (board = 0; board < s->boards; ++board) {
        SDL_JVSBoard *b = &s->board_info[board];

        if (request->sub >= b->first_player && request->sub < b->first_player + b->mapped_players) {
            if (b->outputs == 0) {
                SDL_Serial_Log(&s->base, "JVS board has no general-purpose outputs");
                return;
            }
            if (request->length > (size_t)((b->outputs + 7) / 8)) {
                SDL_Serial_Log(&s->base, "JVS output request longer than the board's outputs");
                return;
            }
            memcpy(b->output, request->data, request->length);
            b->output_length = (uint8_t)request->length;
            b->output_pending = true;
            return;
        }
    }
}

static bool JVS_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_JVSState *s = (SDL_JVSState *)state;
    bool have = false;

    if (s->timer) {
        SDL_Serial_EarlierDeadline(&have, deadline, s->deadline);
    }
    SDL_Serial_PulseDeadline(&s->base, &have, deadline);
    return have;
}

const SDL_SerialModule SDL_SerialJVSModule = {
    "jvs",
    sizeof(SDL_JVSState),
    false,
    1,
    SDL_JVS_MAX_OUTPUT,
    JVS_Reset,
    JVS_Feed,
    JVS_Tick,
    SDL_Serial_NextAction,
    JVS_ActionDone,
    JVS_Output,
    JVS_GetDeadline,
    SDL_Serial_GetSnapshot
};
