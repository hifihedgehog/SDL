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

#include "SDL_serial_kfca_proto.h"

#include <string.h>

/* A 10-bit knob position in bits 15-6, as value x 64 - 32768 */
static int16_t KFCA_Knob(uint8_t high, uint8_t low)
{
    return (int16_t)((((high << 8) | low) >> 6) * 64 - 32768);
}

void SDL_KFCA_Decode(const uint8_t *payload, SDL_SerialControls *controls)
{
    memset(controls, 0, sizeof(*controls));
    controls->axes[SDL_BIO2_SDVX_AXIS_KNOB_L] = KFCA_Knob(payload[0], payload[1]);
    controls->axes[SDL_BIO2_SDVX_AXIS_KNOB_R] = KFCA_Knob(payload[2], payload[3]);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_BT_C, (payload[9] & 0x01) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_BT_B, (payload[9] & 0x02) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_BT_A, (payload[9] & 0x04) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_START, (payload[9] & 0x08) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_RECORDER, (payload[9] & 0x10) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_HEADPHONE, (payload[9] & 0x20) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_FX_R, (payload[11] & 0x08) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_FX_L, (payload[11] & 0x10) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_BT_D, (payload[11] & 0x20) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_COIN, (payload[1] & SDL_KFCA_SYS_COIN) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_SERVICE, (payload[1] & SDL_KFCA_SYS_SERVICE) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_TEST, (payload[1] & SDL_KFCA_SYS_TEST) != 0);
}

/* The output bytes, lamps and PWM, all off */
static const uint8_t kfca_output[SDL_KFCA_OUTPUT] = { 0 };

static void KFCA_Poll(SDL_KFCAState *s, uint64_t now)
{
    s->step = SDL_KFCA_STEP_POLL;
    if (!SDL_ACIO_Request(s, s->node, SDL_KFCA_CMD_POLL, kfca_output, sizeof(kfca_output), SDL_KFCA_INPUT, SDL_KFCA_INPUT)) {
        SDL_ACIO_Restart(s, now);
    }
}

/* The first reply presents the joystick with its controls */
static void KFCA_Commit(SDL_KFCAState *s, const uint8_t *payload)
{
    SDL_SerialControls controls;
    SDL_SerialIdentity identity;

    SDL_KFCA_Decode(payload, &controls);
    if (SDL_Serial_IsPresent(&s->acio.base, 0)) {
        SDL_Serial_Commit(&s->acio.base, 0, &controls);
        return;
    }
    SDL_Serial_SetIdentity(&identity, SDL_KFCA_NAME, SDL_SERIAL_TYPE_UNKNOWN, SDL_BIO2_SDVX_AXES, SDL_BIO2_SDVX_BUTTONS, 0, 0);
    SDL_Serial_PresentWith(&s->acio.base, 0, &identity, &controls);
}

static void KFCA_Ready(void *state, uint64_t now)
{
    static const uint8_t watchdog[2] = { (uint8_t)(SDL_KFCA_WATCHDOG >> 8), (uint8_t)(SDL_KFCA_WATCHDOG & 0xFF) };
    SDL_KFCAState *s = (SDL_KFCAState *)state;

    s->node = (uint8_t)SDL_ACIO_FindType(state, SDL_ACIO_TYPE_KFCA);
    s->step = SDL_KFCA_STEP_IDLE;
    if (s->node == 0) {
        SDL_Serial_Log(&s->acio.base, "No KFCA node on the bus, so no joystick opens");
        return;
    }
    /* The watchdog and amplifier replies carry a status that is not used, so
       any payload length is taken */
    s->step = SDL_KFCA_STEP_WATCHDOG;
    if (!SDL_ACIO_Request(state, s->node, SDL_KFCA_CMD_WATCHDOG, watchdog, sizeof(watchdog), 0, SDL_ACIO_MAX_PAYLOAD)) {
        SDL_ACIO_Restart(state, now);
    }
}

static void KFCA_Reply(void *state, const SDL_ACIOFrame *reply, uint64_t now)
{
    static const uint8_t amp[SDL_KFCA_AMP_LENGTH] = { 0 };
    SDL_KFCAState *s = (SDL_KFCAState *)state;

    switch (s->step) {
    case SDL_KFCA_STEP_WATCHDOG:
        if (!reply) {
            SDL_Serial_Log(&s->acio.base, "The KFCA node did not answer its watchdog command, so the bus starts over");
            SDL_ACIO_Restart(state, now);
            break;
        }
        s->step = SDL_KFCA_STEP_AMP;
        if (!SDL_ACIO_Request(state, s->node, SDL_KFCA_CMD_AMP, amp, sizeof(amp), 0, SDL_ACIO_MAX_PAYLOAD)) {
            SDL_ACIO_Restart(state, now);
        }
        break;
    case SDL_KFCA_STEP_AMP:
        if (!reply) {
            SDL_Serial_Log(&s->acio.base, "The KFCA node did not answer the amplifier command, so the bus starts over");
            SDL_ACIO_Restart(state, now);
            break;
        }
        KFCA_Poll(s, now);
        break;
    case SDL_KFCA_STEP_POLL:
        if (!reply) {
            if (++s->failures >= SDL_KFCA_FAILURES) {
                SDL_Serial_Log(&s->acio.base, "The KFCA node failed three polls, so the bus starts over");
                SDL_ACIO_Restart(state, now);
                return;
            }
        } else {
            s->failures = 0;
            KFCA_Commit(s, reply->payload);
        }
        KFCA_Poll(s, now);
        break;
    default:
        break;
    }
}

static void KFCA_Down(void *state, uint64_t now)
{
    SDL_KFCAState *s = (SDL_KFCAState *)state;

    (void)now;
    SDL_Serial_AbsentAll(&s->acio.base);
    s->step = SDL_KFCA_STEP_BUS;
    s->node = 0;
    s->failures = 0;
}

static const SDL_ACIOHandler kfca_handler = {
    KFCA_Ready,
    KFCA_Reply,
    NULL,
    KFCA_Down,
    NULL,
    NULL
};

static void KFCA_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_ACIO_Reset(state, sizeof(SDL_KFCAState), sink, &kfca_handler, SDL_KFCA_RATE, now);
}

/* Lamps stay off: no output request is taken */
static void KFCA_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

const SDL_SerialModule SDL_SerialKFCAModule = {
    "kfca",
    sizeof(SDL_KFCAState),
    false,
    0,
    0,
    KFCA_Reset,
    SDL_ACIO_Feed,
    SDL_ACIO_Tick,
    SDL_Serial_NextAction,
    SDL_ACIO_ActionDone,
    KFCA_Output,
    SDL_ACIO_GetDeadline,
    SDL_Serial_GetSnapshot
};
