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

/* Konami's P3IO board. See SDL_hidapi_konami_p3io_proto.h. */

#include "SDL_hidapi_konami_p3io_proto.h"

#include <string.h>

#define P3IO_TRANSFER_BULK      2
#define P3IO_TRANSFER_INTERRUPT 3

size_t SDL_P3IO_EncodeRequest(uint8_t sequence, uint8_t command, const uint8_t *payload,
                              size_t payload_length, uint8_t *out, size_t out_size)
{
    uint8_t body[SDL_P3IO_MAX_BODY];
    size_t body_length, i, length = 0;

    /* The length byte counts the sequence, the command and the payload */
    if (!out || (payload_length && !payload) || payload_length > SDL_P3IO_MAX_BODY - 3) {
        return 0;
    }
    body[0] = (uint8_t)(payload_length + 2);
    body[1] = sequence;
    body[2] = command;
    if (payload_length) {
        memcpy(&body[3], payload, payload_length);
    }
    body_length = payload_length + 3;

    if (out_size < 1) {
        return 0;
    }
    out[length++] = SDL_P3IO_START;
    for (i = 0; i < body_length; ++i) {
        const uint8_t b = body[i];

        if (b == SDL_P3IO_START || b == SDL_P3IO_ESCAPE) {
            if (out_size - length < 2) {
                return 0;
            }
            out[length++] = SDL_P3IO_ESCAPE;
            out[length++] = (uint8_t)~b;
        } else {
            if (out_size - length < 1) {
                return 0;
            }
            out[length++] = b;
        }
    }
    return length;
}

void SDL_P3IO_ResetParser(SDL_P3IOParser *parser)
{
    parser->count = 0;
    parser->in_frame = false;
    parser->escape = false;
}

bool SDL_P3IO_ParseByte(SDL_P3IOParser *parser, uint8_t byte)
{
    uint8_t value;

    if (byte == SDL_P3IO_START) {
        /* A frame starts here, whatever came before */
        parser->in_frame = true;
        parser->escape = false;
        parser->count = 0;
        return false;
    }
    if (!parser->in_frame) {
        return false;
    }
    if (byte == SDL_P3IO_ESCAPE) {
        if (parser->escape) {
            /* FF FF escapes nothing */
            parser->in_frame = false;
            return false;
        }
        parser->escape = true;
        return false;
    }
    value = parser->escape ? (uint8_t)~byte : byte;
    parser->escape = false;
    parser->body[parser->count++] = value;
    if (parser->count == 1 && value < 2) {
        /* No room for the sequence and the command */
        parser->in_frame = false;
        return false;
    }
    if (parser->count == (size_t)parser->body[0] + 1) {
        parser->in_frame = false;
        return true;
    }
    return false;
}

/* INIT goes out now, with sequence 0. No request is out at this point, and
   the parser starts over when the request goes. */
static void P3IO_Begin(SDL_P3IOSession *session, uint64_t now)
{
    session->phase = SDL_P3IO_PHASE_INIT;
    session->sequence = 0;
    session->due_at = now;
}

static void P3IO_Lost(SDL_P3IOSession *session, uint64_t now)
{
    session->phase = SDL_P3IO_PHASE_LOST;
    session->awaiting = false;
    session->due_at = now + SDL_P3IO_RETRY_MS;
}

void SDL_P3IO_SessionInit(SDL_P3IOSession *session, uint64_t now)
{
    memset(session, 0, sizeof(*session));
    P3IO_Begin(session, now);
}

size_t SDL_P3IO_SessionPoll(SDL_P3IOSession *session, uint64_t now, uint8_t *out, size_t out_size)
{
    const uint8_t watchdog = SDL_P3IO_WATCHDOG_OFF;
    const uint8_t *payload = NULL;
    size_t payload_length = 0, length;
    uint8_t command;

    if (!session) {
        return 0;
    }
    if (session->awaiting) {
        if (now < session->sent_at + SDL_P3IO_REPLY_TIMEOUT_MS) {
            return 0;
        }
        /* The reply never came */
        P3IO_Lost(session, now);
    }
    if (session->phase == SDL_P3IO_PHASE_LOST) {
        if (now < session->due_at) {
            return 0;
        }
        P3IO_Begin(session, now);
    }
    if (now < session->due_at) {
        return 0;
    }

    switch (session->phase) {
    case SDL_P3IO_PHASE_INIT:
        command = SDL_P3IO_CMD_INIT;
        break;
    case SDL_P3IO_PHASE_WATCHDOG:
        command = SDL_P3IO_CMD_SET_WATCHDOG;
        payload = &watchdog;
        payload_length = 1;
        break;
    default:
        command = SDL_P3IO_CMD_GET_VERSION;
        break;
    }
    length = SDL_P3IO_EncodeRequest(session->sequence, command, payload, payload_length, out, out_size);
    if (length == 0) {
        return 0;
    }
    session->awaiting = true;
    session->awaited_sequence = session->sequence;
    session->awaited_command = command;
    session->sent_at = now;
    session->sequence = (uint8_t)((session->sequence + 1) & SDL_P3IO_SEQUENCE_MASK);
    /* Bytes that came before this request are not its reply */
    SDL_P3IO_ResetParser(&session->parser);
    return length;
}

/* A whole frame is in the parser */
static void P3IO_Reply(SDL_P3IOSession *session, uint64_t now)
{
    const uint8_t *body = session->parser.body;
    const uint8_t length = body[0];

    if (!session->awaiting || body[1] != session->awaited_sequence || body[2] != session->awaited_command) {
        return;
    }
    session->awaiting = false;

    switch (session->awaited_command) {
    case SDL_P3IO_CMD_INIT:
        if (length != SDL_P3IO_INIT_REPLY_LENGTH || body[3] != 0x00) {
            P3IO_Lost(session, now);
            return;
        }
        session->phase = SDL_P3IO_PHASE_WATCHDOG;
        session->due_at = now;
        break;
    case SDL_P3IO_CMD_SET_WATCHDOG:
        if (length != SDL_P3IO_WATCHDOG_REPLY_LENGTH) {
            P3IO_Lost(session, now);
            return;
        }
        session->phase = SDL_P3IO_PHASE_UP;
        ++session->ups;
        session->due_at = now + SDL_P3IO_KEEPALIVE_MS;
        break;
    default:
        if (length != SDL_P3IO_VERSION_REPLY_LENGTH) {
            P3IO_Lost(session, now);
            return;
        }
        memcpy(session->version, &body[3], SDL_P3IO_VERSION_LENGTH);
        session->due_at = now + SDL_P3IO_KEEPALIVE_MS;
        break;
    }
}

void SDL_P3IO_SessionReceive(SDL_P3IOSession *session, const uint8_t *data, size_t length, uint64_t now)
{
    size_t i;

    if (!session || !data) {
        return;
    }
    for (i = 0; i < length; ++i) {
        if (SDL_P3IO_ParseByte(&session->parser, data[i])) {
            P3IO_Reply(session, now);
        }
    }
}

void SDL_P3IO_SessionFailed(SDL_P3IOSession *session, uint64_t now)
{
    if (session) {
        P3IO_Lost(session, now);
    }
}

bool SDL_P3IO_SessionUp(const SDL_P3IOSession *session)
{
    return session && session->phase == SDL_P3IO_PHASE_UP;
}

bool SDL_P3IO_SessionAwaiting(const SDL_P3IOSession *session)
{
    return session && session->awaiting;
}

uint64_t SDL_P3IO_SessionDeadline(const SDL_P3IOSession *session)
{
    if (!session) {
        return 0;
    }
    if (session->awaiting) {
        return session->sent_at + SDL_P3IO_REPLY_TIMEOUT_MS;
    }
    return session->due_at;
}

void SDL_P3IO_InputInit(SDL_P3IOInput *input, SDL_P3IOState *state)
{
    memset(input, 0, sizeof(*input));
    memset(state, 0, sizeof(*state));
}

/* One player's byte, active high by now, and that player's HD menu bits */
static uint16_t P3IO_Player(uint8_t pressed, bool menu_up, bool menu_down)
{
    uint16_t buttons = 0;

    if (pressed & SDL_P3IO_PLAYER_START) {
        buttons |= 1u << SDL_P3IO_BUTTON_START;
    }
    if (pressed & SDL_P3IO_PLAYER_UP) {
        buttons |= 1u << SDL_P3IO_BUTTON_UP;
    }
    if (pressed & SDL_P3IO_PLAYER_DOWN) {
        buttons |= 1u << SDL_P3IO_BUTTON_DOWN;
    }
    if (pressed & SDL_P3IO_PLAYER_LEFT) {
        buttons |= 1u << SDL_P3IO_BUTTON_LEFT;
    }
    if (pressed & SDL_P3IO_PLAYER_RIGHT) {
        buttons |= 1u << SDL_P3IO_BUTTON_RIGHT;
    }
    if (pressed & SDL_P3IO_PLAYER_MENU_LEFT) {
        buttons |= 1u << SDL_P3IO_BUTTON_MENU_LEFT;
    }
    if (pressed & SDL_P3IO_PLAYER_MENU_RIGHT) {
        buttons |= 1u << SDL_P3IO_BUTTON_MENU_RIGHT;
    }
    if (menu_up) {
        buttons |= 1u << SDL_P3IO_BUTTON_MENU_UP;
    }
    if (menu_down) {
        buttons |= 1u << SDL_P3IO_BUTTON_MENU_DOWN;
    }
    return buttons;
}

static void P3IO_Decode(SDL_P3IOInput *input, const uint8_t *report, SDL_P3IOState *state)
{
    /* Active low: a 0 bit is a pressed control */
    const uint8_t p1 = (uint8_t)~report[1];
    const uint8_t p2 = (uint8_t)~report[2];
    const uint8_t operator_bits = (uint8_t)~report[3];
    uint8_t menu;

    input->armed |= (uint8_t)(~operator_bits & SDL_P3IO_OPERATOR_HD_MENU);
    menu = (uint8_t)(operator_bits & input->armed);

    state->buttons[0] = P3IO_Player(p1, (menu & SDL_P3IO_OPERATOR_P1_MENU_UP) != 0,
                                    (menu & SDL_P3IO_OPERATOR_P1_MENU_DOWN) != 0);
    if (operator_bits & SDL_P3IO_OPERATOR_TEST) {
        state->buttons[0] |= 1u << SDL_P3IO_BUTTON_TEST;
    }
    if (operator_bits & SDL_P3IO_OPERATOR_SERVICE) {
        state->buttons[0] |= 1u << SDL_P3IO_BUTTON_SERVICE;
    }
    if (operator_bits & SDL_P3IO_OPERATOR_COIN) {
        state->buttons[0] |= 1u << SDL_P3IO_BUTTON_COIN;
    }
    state->buttons[1] = P3IO_Player(p2, (menu & SDL_P3IO_OPERATOR_P2_MENU_UP) != 0,
                                    (menu & SDL_P3IO_OPERATOR_P2_MENU_DOWN) != 0);
}

bool SDL_P3IO_InputFeed(SDL_P3IOInput *input, const uint8_t *data, size_t length, SDL_P3IOState *state)
{
    if (!input || !state || !data || length == 0) {
        return false;
    }
    if (length >= SDL_P3IO_REPORT_LENGTH) {
        input->filled = 0;
        if (data[0] != SDL_P3IO_REPORT_START) {
            return false;
        }
        P3IO_Decode(input, data, state);
        return true;
    }
    if (length != SDL_P3IO_CHUNK_LENGTH) {
        input->filled = 0;
        return false;
    }
    if (data[0] == SDL_P3IO_REPORT_START) {
        memcpy(input->report, data, SDL_P3IO_CHUNK_LENGTH);
        input->filled = SDL_P3IO_CHUNK_LENGTH;
        return false;
    }
    if (input->filled == 0) {
        /* Before the first 80 */
        return false;
    }
    memcpy(&input->report[input->filled], data, SDL_P3IO_CHUNK_LENGTH);
    input->filled += SDL_P3IO_CHUNK_LENGTH;
    if (input->filled < SDL_P3IO_REPORT_LENGTH) {
        return false;
    }
    input->filled = 0;
    P3IO_Decode(input, input->report, state);
    return true;
}

bool SDL_P3IO_FindCommandEndpoints(const SDL_P3IOEndpoint *endpoints, size_t count,
                                   SDL_P3IOCommandEndpoints *found)
{
    SDL_P3IOCommandEndpoints result;
    bool have_out = false, have_in = false;
    size_t i;

    if (!found || (count && !endpoints)) {
        return false;
    }
    memset(&result, 0, sizeof(result));
    for (i = 0; i < count; ++i) {
        const uint8_t transfer = (uint8_t)(endpoints[i].attributes & 0x03);

        if (transfer != P3IO_TRANSFER_BULK && transfer != P3IO_TRANSFER_INTERRUPT) {
            continue;
        }
        if (!have_out && endpoints[i].address == SDL_P3IO_COMMAND_OUT) {
            result.out_interface = endpoints[i].interface_number;
            result.out_interrupt = (transfer == P3IO_TRANSFER_INTERRUPT);
            have_out = true;
        } else if (!have_in && endpoints[i].address == SDL_P3IO_COMMAND_IN) {
            result.in_interface = endpoints[i].interface_number;
            result.in_interrupt = (transfer == P3IO_TRANSFER_INTERRUPT);
            have_in = true;
        }
    }
    if (!have_out || !have_in) {
        return false;
    }
    *found = result;
    return true;
}
