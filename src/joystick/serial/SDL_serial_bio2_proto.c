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

#include "SDL_serial_bio2_proto.h"

#include <string.h>

static int16_t BIO2_Slider(uint8_t value)
{
    return (int16_t)((value >> 4) * 4369 - 32768);
}

static int16_t BIO2_Turntable(uint8_t value)
{
    return (int16_t)((value - 128) * 256);
}

static int16_t BIO2_Knob(uint8_t high, uint8_t low)
{
    return (int16_t)((((high << 8) | low) >> 6) * 64 - 32768);
}

void SDL_BIO2_DecodeIIDX(const uint8_t *payload, SDL_SerialControls *controls)
{
    int i;

    memset(controls, 0, sizeof(*controls));
    controls->axes[SDL_BIO2_IIDX_AXIS_TT1] = BIO2_Turntable(payload[16]);
    controls->axes[SDL_BIO2_IIDX_AXIS_TT2] = BIO2_Turntable(payload[17]);
    controls->axes[SDL_BIO2_IIDX_AXIS_SLIDER1] = BIO2_Slider(payload[0]);
    controls->axes[SDL_BIO2_IIDX_AXIS_SLIDER1 + 1] = BIO2_Slider(payload[2]);
    controls->axes[SDL_BIO2_IIDX_AXIS_SLIDER1 + 2] = BIO2_Slider(payload[4]);
    controls->axes[SDL_BIO2_IIDX_AXIS_SLIDER1 + 3] = BIO2_Slider(payload[6]);
    controls->axes[SDL_BIO2_IIDX_AXIS_SLIDER1 + 4] = BIO2_Slider(payload[7]);
    for (i = 0; i < 7; ++i) {
        SDL_Serial_SetButton(controls, SDL_BIO2_IIDX_BUTTON_P1_KEY1 + i, (payload[18 + 2 * i] & 0x80) != 0);
        SDL_Serial_SetButton(controls, SDL_BIO2_IIDX_BUTTON_P2_KEY1 + i, (payload[32 + 2 * i] & 0x80) != 0);
    }
    SDL_Serial_SetButton(controls, SDL_BIO2_IIDX_BUTTON_P1_START, (payload[9] & 0x80) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_IIDX_BUTTON_P2_START, (payload[9] & 0x40) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_IIDX_BUTTON_VEFX, (payload[9] & 0x20) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_IIDX_BUTTON_EFFECT, (payload[9] & 0x10) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_IIDX_BUTTON_TEST, (payload[1] & 0x08) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_IIDX_BUTTON_SERVICE, (payload[1] & 0x04) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_IIDX_BUTTON_COIN, (payload[1] & 0x02) != 0);
}

void SDL_BIO2_DecodeSDVX(const uint8_t *payload, SDL_SerialControls *controls)
{
    memset(controls, 0, sizeof(*controls));
    controls->axes[SDL_BIO2_SDVX_AXIS_KNOB_L] = BIO2_Knob(payload[0], payload[1]);
    controls->axes[SDL_BIO2_SDVX_AXIS_KNOB_R] = BIO2_Knob(payload[2], payload[3]);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_FX_L, (payload[9] & 0x01) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_BT_D, (payload[9] & 0x02) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_BT_C, (payload[9] & 0x04) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_BT_B, (payload[9] & 0x08) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_BT_A, (payload[9] & 0x10) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_START, (payload[9] & 0x20) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_RECORDER, (payload[9] & 0x40) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_HEADPHONE, (payload[9] & 0x80) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_EX2, (payload[10] & 0x20) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_EX1, (payload[10] & 0x40) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_FX_R, (payload[10] & 0x80) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_COIN, (payload[1] & 0x02) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_SERVICE, (payload[1] & 0x04) != 0);
    SDL_Serial_SetButton(controls, SDL_BIO2_SDVX_BUTTON_TEST, (payload[1] & 0x08) != 0);
}

/* The output bytes, lamps and text, all off */
static const uint8_t bio2_output[SDL_BIO2_OUTPUT_IIDX] = { 0 };

static void BIO2_Poll(SDL_BIO2State *s, uint64_t now)
{
    const bool iidx = (s->mode == SDL_BIO2_MODE_IIDX);
    const size_t reply = iidx ? SDL_BIO2_INPUT_IIDX : SDL_BIO2_INPUT_SDVX;

    s->step = SDL_BIO2_STEP_POLL;
    if (!SDL_ACIO_Request(s, s->node, iidx ? SDL_BIO2_CMD_POLL_IIDX : SDL_BIO2_CMD_POLL_SDVX, bio2_output,
                          iidx ? SDL_BIO2_OUTPUT_IIDX : SDL_BIO2_OUTPUT_SDVX, reply, reply)) {
        SDL_ACIO_Restart(s, now);
    }
}

/* The first reply presents the joystick with its controls */
static void BIO2_Commit(SDL_BIO2State *s, const uint8_t *payload)
{
    SDL_SerialControls controls;
    SDL_SerialIdentity identity;

    if (s->mode == SDL_BIO2_MODE_IIDX) {
        SDL_BIO2_DecodeIIDX(payload, &controls);
    } else {
        SDL_BIO2_DecodeSDVX(payload, &controls);
    }
    if (SDL_Serial_IsPresent(&s->acio.base, 0)) {
        SDL_Serial_Commit(&s->acio.base, 0, &controls);
        return;
    }
    if (s->mode == SDL_BIO2_MODE_IIDX) {
        SDL_Serial_SetIdentity(&identity, "Konami BIO2 IIDX", SDL_SERIAL_TYPE_UNKNOWN, SDL_BIO2_IIDX_AXES, SDL_BIO2_IIDX_BUTTONS, 0, 0);
    } else {
        SDL_Serial_SetIdentity(&identity, "Konami SOUND VOLTEX", SDL_SERIAL_TYPE_UNKNOWN, SDL_BIO2_SDVX_AXES, SDL_BIO2_SDVX_BUTTONS, 0, 0);
    }
    SDL_Serial_PresentWith(&s->acio.base, 0, &identity, &controls);
}

static void BIO2_Ready(void *state, uint64_t now)
{
    SDL_BIO2State *s = (SDL_BIO2State *)state;
    uint8_t init;

    s->node = (uint8_t)SDL_ACIO_FindProduct(state, SDL_BIO2_PRODUCT);
    s->step = SDL_BIO2_STEP_IDLE;
    if (s->node == 0) {
        SDL_Serial_Log(&s->acio.base, "No BI2A node on the BIO2's bus, so no joystick opens");
        return;
    }
    if (s->mode == SDL_BIO2_MODE_NONE) {
        SDL_Serial_Log(&s->acio.base, "SDL_JOYSTICK_KONAMI_BIO2_MODE names no cabinet, so no joystick opens");
        return;
    }
    /* The init reply's status is not used, so any payload length is taken */
    init = (s->mode == SDL_BIO2_MODE_IIDX) ? SDL_BIO2_INIT_IIDX : SDL_BIO2_INIT_SDVX;
    s->step = SDL_BIO2_STEP_INIT;
    if (!SDL_ACIO_Request(state, s->node, SDL_BIO2_CMD_INIT, &init, 1, 0, SDL_ACIO_MAX_PAYLOAD)) {
        SDL_ACIO_Restart(state, now);
    }
}

static void BIO2_Reply(void *state, const SDL_ACIOFrame *reply, uint64_t now)
{
    static const uint8_t amp[SDL_BIO2_AMP_LENGTH] = { 0 };
    SDL_BIO2State *s = (SDL_BIO2State *)state;

    switch (s->step) {
    case SDL_BIO2_STEP_INIT:
        if (!reply) {
            SDL_Serial_Log(&s->acio.base, "The BI2A node did not answer its init, so the bus starts over");
            SDL_ACIO_Restart(state, now);
        } else if (s->mode == SDL_BIO2_MODE_SDVX) {
            s->step = SDL_BIO2_STEP_AMP;
            if (!SDL_ACIO_Request(state, s->node, SDL_BIO2_CMD_AMP, amp, sizeof(amp), 0, SDL_ACIO_MAX_PAYLOAD)) {
                SDL_ACIO_Restart(state, now);
            }
        } else {
            BIO2_Poll(s, now);
        }
        break;
    case SDL_BIO2_STEP_AMP:
        if (!reply) {
            SDL_Serial_Log(&s->acio.base, "The BI2A node did not answer the amplifier command, so the bus starts over");
            SDL_ACIO_Restart(state, now);
        } else {
            BIO2_Poll(s, now);
        }
        break;
    case SDL_BIO2_STEP_POLL:
        if (!reply) {
            if (++s->failures >= SDL_BIO2_FAILURES) {
                SDL_Serial_Log(&s->acio.base, "The BI2A node failed three polls, so the bus starts over");
                SDL_ACIO_Restart(state, now);
                return;
            }
        } else {
            s->failures = 0;
            BIO2_Commit(s, reply->payload);
        }
        BIO2_Poll(s, now);
        break;
    default:
        break;
    }
}

static void BIO2_Down(void *state, uint64_t now)
{
    SDL_BIO2State *s = (SDL_BIO2State *)state;

    (void)now;
    SDL_Serial_AbsentAll(&s->acio.base);
    s->step = SDL_BIO2_STEP_BUS;
    s->node = 0;
    s->failures = 0;
}

static const SDL_ACIOHandler bio2_handler = {
    BIO2_Ready,
    BIO2_Reply,
    NULL,
    BIO2_Down,
    NULL,
    NULL
};

static void BIO2_Reset(void *state, const SDL_SerialSink *sink, uint64_t now, SDL_BIO2Mode mode)
{
    SDL_BIO2State *s = (SDL_BIO2State *)state;

    SDL_ACIO_Reset(state, sizeof(*s), sink, &bio2_handler, SDL_BIO2_RATE, now);
    s->mode = mode;
}

static void BIO2_ResetNone(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    BIO2_Reset(state, sink, now, SDL_BIO2_MODE_NONE);
}

static void BIO2_ResetIIDX(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    BIO2_Reset(state, sink, now, SDL_BIO2_MODE_IIDX);
}

static void BIO2_ResetSDVX(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    BIO2_Reset(state, sink, now, SDL_BIO2_MODE_SDVX);
}

/* Lamps stay off: no output request is taken */
static void BIO2_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

const SDL_SerialModule SDL_SerialBIO2Module = {
    "bio2",
    sizeof(SDL_BIO2State),
    false,
    0,
    0,
    BIO2_ResetNone,
    SDL_ACIO_Feed,
    SDL_ACIO_Tick,
    SDL_Serial_NextAction,
    SDL_ACIO_ActionDone,
    BIO2_Output,
    SDL_ACIO_GetDeadline,
    SDL_Serial_GetSnapshot
};

const SDL_SerialModule SDL_SerialBIO2IIDXModule = {
    "bio2iidx",
    sizeof(SDL_BIO2State),
    false,
    0,
    0,
    BIO2_ResetIIDX,
    SDL_ACIO_Feed,
    SDL_ACIO_Tick,
    SDL_Serial_NextAction,
    SDL_ACIO_ActionDone,
    BIO2_Output,
    SDL_ACIO_GetDeadline,
    SDL_Serial_GetSnapshot
};

const SDL_SerialModule SDL_SerialBIO2SDVXModule = {
    "bio2sdvx",
    sizeof(SDL_BIO2State),
    false,
    0,
    0,
    BIO2_ResetSDVX,
    SDL_ACIO_Feed,
    SDL_ACIO_Tick,
    SDL_Serial_NextAction,
    SDL_ACIO_ActionDone,
    BIO2_Output,
    SDL_ACIO_GetDeadline,
    SDL_Serial_GetSnapshot
};
