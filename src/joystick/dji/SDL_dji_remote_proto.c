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

#include "SDL_dji_remote_proto.h"

#include <string.h>

#define DJI_SERIAL_SEQUENCE 0x34EB /* Every RC-N1 source sends this sequence */
#define DJI_SIMULATOR_TAG   0xFF   /* Bulk: the 06/24 written every 3000 ms */

/* The Phantom 2's two frames, written byte for byte as mDjiController does.
 * No source decodes their framing. */
static const uint8_t dji_phantom2_init[34] = {
    0x55, 0xAA, 0x55, 0xAA, 0x1E, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x80, 0x00, 0x04, 0x04, 0x74,
    0x94, 0x35, 0x00, 0xD8, 0xC0, 0x41, 0x00, 0x30, 0xF6, 0x08, 0x00, 0x00, 0xF6, 0x69, 0x9C, 0x01, 0xE8
};
static const uint8_t dji_phantom2_ping[34] = {
    0x55, 0xAA, 0x55, 0xAA, 0x1E, 0x00, 0x01, 0x00, 0x00, 0x1C, 0x02, 0x00, 0x80, 0x00, 0x06, 0x01, 0x28,
    0x97, 0xAE, 0x03, 0x28, 0x36, 0xA4, 0x03, 0x28, 0x36, 0xA4, 0x03, 0xAB, 0xA7, 0x30, 0x00, 0x03, 0x53
};

static uint16_t DJI_Read16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

int16_t SDL_DJI_ScaleAxis(int raw, int center, int half)
{
    int64_t value;

    if (half <= 0) {
        return 0;
    }
    value = ((int64_t)raw - center) * 32767 / half;
    if (value > 32767) {
        value = 32767;
    } else if (value < -32767) {
        value = -32767;
    }
    return (int16_t)value;
}

/* The 06/F5 axes: within 2 percent of the half range of the center reads 0 */
static int16_t DJI_ScaleDeadZone(int raw, int center, int half)
{
    const int offset = raw - center;

    if (offset * 50 < half && -offset * 50 < half) {
        return 0;
    }
    return SDL_DJI_ScaleAxis(raw, center, half);
}

int SDL_DJI_DecodeChannels(const SDL_DJIFrame *frame, uint16_t *values)
{
    size_t base, count, i;

    if (!frame || !values || frame->set != 0x06 || frame->id != 0x01) {
        return 0;
    }
    if (frame->length == 38) {
        base = 13;
        count = 5;
    } else if (frame->length == 32) {
        base = 12;
        count = 6;
    } else {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        values[i] = DJI_Read16(frame->data + base + 3 * i);
    }
    return (int)count;
}

bool SDL_DJI_DecodeRCN1Status(const SDL_DJIFrame *frame, int *mode, uint8_t *bits)
{
    const uint8_t *d;

    if (!frame || frame->set != 0x06 || frame->id != 0x27 || frame->length != 58) {
        return false;
    }
    d = frame->data;
    /* DjiMini2RCasJoystick reads bytes 27 and 28 big-endian: 0 is one end,
       bit 5 of byte 28 the other, anything else the middle */
    if (d[27] == 0 && d[28] == 0) {
        *mode = 0;
    } else if (d[28] & 0x20) {
        *mode = 2;
    } else {
        *mode = 1;
    }
    *bits = d[29];
    return true;
}

bool SDL_DJI_DecodePhantom3(const SDL_DJIFrame *frame, SDL_DJIPhantom3Report *report)
{
    const uint8_t *d;

    if (!frame || !report || frame->set != 0x06 || frame->id != 0x27 || frame->length != 58) {
        return false;
    }
    d = frame->data;
    report->right_y = (int16_t)DJI_Read16(d + 16);
    report->right_x = (int16_t)DJI_Read16(d + 18);
    report->left_y = (int16_t)DJI_Read16(d + 20);
    report->left_x = (int16_t)DJI_Read16(d + 22);
    report->dial = (int16_t)DJI_Read16(d + 24);
    report->wheel = (int8_t)d[26];
    report->mode = (int8_t)d[28];
    report->buttons = d[29];
    return true;
}

bool SDL_DJI_DecodeTestStick(const SDL_DJIFrame *frame, uint16_t *values)
{
    int i;

    if (!frame || !values || frame->set != 0x06 || frame->id != 0xF5 || frame->length != 26) {
        return false;
    }
    for (i = 0; i < 4; ++i) {
        values[i] = DJI_Read16(frame->payload + 1 + 2 * i);
    }
    return true;
}

bool SDL_DJI_DecodeModel(const SDL_DJIFrame *frame, char *model, size_t size)
{
    size_t length = 0;

    if (!frame || !model || size == 0 || frame->set != 0x00 || (frame->id != 0x81 && frame->id != 0x82)) {
        return false;
    }
    while (length < frame->payload_length && frame->payload[length] != 0) {
        const uint8_t c = frame->payload[length];

        if (c < 0x20 || c > 0x7E || length + 1 >= size) {
            return false;
        }
        ++length;
    }
    if (length == 0) {
        return false;
    }
    memcpy(model, frame->payload, length);
    model[length] = '\0';
    return true;
}

const char *SDL_DJI_NameForModel(const char *model, const char *fallback)
{
    if (model && strcmp(model, "rm330") == 0) {
        return "DJI RC (RM330)";
    }
    if (model && strcmp(model, "rc331") == 0) {
        return "DJI RC 2";
    }
    return fallback;
}

bool SDL_DJI_DecodePhantom2(const uint8_t *data, size_t length, SDL_DJIPhantom2Report *report)
{
    if (!data || !report || length != SDL_DJI_PHANTOM2_REPLY_LENGTH || data[0] != 0x55) {
        return false;
    }
    report->right_x = (int16_t)DJI_Read16(data + 31);
    report->right_y = (int16_t)DJI_Read16(data + 35);
    report->left_y = (int16_t)DJI_Read16(data + 39);
    report->left_x = (int16_t)DJI_Read16(data + 43);
    report->levers[0] = (int16_t)DJI_Read16(data + 47);
    report->levers[1] = (int16_t)DJI_Read16(data + 51);
    report->dial = (int16_t)DJI_Read16(data + 55);
    return true;
}

/* The module */

static uint64_t DJI_SilenceMs(const SDL_DJIRemoteState *s)
{
    return (s->kind == SDL_DJI_REMOTE_BULK) ? SDL_DJI_BULK_SILENCE_MS : SDL_DJI_SERIAL_SILENCE_MS;
}

static uint8_t DJI_NextSeq(SDL_DJIRemoteState *s)
{
    if (++s->write_seq == 0 || s->write_seq == DJI_SIMULATOR_TAG) {
        s->write_seq = 1;
    }
    return s->write_seq;
}

static void DJI_SetTimer(SDL_DJIRemoteState *s, uint64_t at)
{
    s->timer = true;
    s->deadline = at;
}

/* The step timer's period once a poll or ping has gone out */
static uint64_t DJI_PollMs(const SDL_DJIRemoteState *s)
{
    switch (s->kind) {
    case SDL_DJI_REMOTE_PHANTOM3:
    case SDL_DJI_REMOTE_PHANTOM2:
        return SDL_DJI_PHANTOM_CYCLE_MS;
    case SDL_DJI_REMOTE_BULK:
        return s->test_stick ? SDL_DJI_BULK_TEST_STICK_MS : SDL_DJI_BULK_POLL_MS;
    default:
        return SDL_DJI_POLL_RESEND_MS;
    }
}

/* A start-up or poll write. Its completion runs the step timer. A write that
 * cannot be queued is tried again one period later. */
static void DJI_Write(SDL_DJIRemoteState *s, const uint8_t *data, size_t length, uint64_t now)
{
    const uint8_t tag = DJI_NextSeq(s);

    s->timer = false;
    if (length && SDL_Serial_QueueWrite(&s->base, tag, data, length)) {
        s->waiting_seq = tag;
    } else {
        s->waiting_seq = 0;
        DJI_SetTimer(s, now + DJI_PollMs(s));
    }
}

static size_t DJI_BuildPoll(SDL_DJIRemoteState *s, uint8_t *out, size_t size)
{
    size_t first, second;

    switch (s->kind) {
    case SDL_DJI_REMOTE_RCN1:
        /* 06/01 for the sticks and dial, 06/27 for the mode switch and the
           status bits, as DjiMini2RCasJoystick sends them */
        first = SDL_DJI_BuildFrame(out, size, SDL_DJI_MODULE_PC, SDL_DJI_MODULE_RC, DJI_SERIAL_SEQUENCE,
                                   SDL_DJI_TYPE_REQUEST, 0x06, 0x01, NULL, 0);
        second = first ? SDL_DJI_BuildFrame(out + first, size - first, SDL_DJI_MODULE_PC, SDL_DJI_MODULE_RC,
                                            DJI_SERIAL_SEQUENCE, SDL_DJI_TYPE_REQUEST, 0x06, 0x27, NULL, 0)
                       : 0;
        return second ? first + second : 0;
    case SDL_DJI_REMOTE_MAVIC_MINI:
        /* miniDjiController's ping, sequence 3 */
        return SDL_DJI_BuildFrame(out, size, SDL_DJI_MODULE_PC, SDL_DJI_MODULE_HD_GROUND, 3,
                                  SDL_DJI_TYPE_REQUEST, 0x06, 0x01, NULL, 0);
    case SDL_DJI_REMOTE_PHANTOM3:
        /* mDjiController's two frames: 00/0E to the flight controller and
           06/27 to 0E, sequences 4 and 5 */
        first = SDL_DJI_BuildFrame(out, size, SDL_DJI_MODULE_PC, SDL_DJI_MODULE_FLIGHT, 4,
                                   SDL_DJI_TYPE_REQUEST, 0x00, 0x0E, NULL, 0);
        second = first ? SDL_DJI_BuildFrame(out + first, size - first, SDL_DJI_MODULE_PC, SDL_DJI_MODULE_HD_GROUND,
                                            5, SDL_DJI_TYPE_REQUEST, 0x06, 0x27, NULL, 0)
                       : 0;
        return second ? first + second : 0;
    case SDL_DJI_REMOTE_PHANTOM2:
        if (size < sizeof(dji_phantom2_ping)) {
            return 0;
        }
        memcpy(out, dji_phantom2_ping, sizeof(dji_phantom2_ping));
        return sizeof(dji_phantom2_ping);
    case SDL_DJI_REMOTE_BULK:
        if (s->test_stick) {
            /* dji-rc-joystick's poll, its sequence counting from 1 */
            ++s->test_sequence;
            return SDL_DJI_BuildFrame(out, size, SDL_DJI_MODULE_PC, SDL_DJI_MODULE_RC, s->test_sequence,
                                      SDL_DJI_TYPE_REQUEST, 0x06, 0xF5, NULL, 0);
        }
        return SDL_DJI_BuildFrame(out, size, SDL_DJI_MODULE_PC, SDL_DJI_MODULE_RC, DJI_SERIAL_SEQUENCE,
                                  SDL_DJI_TYPE_REQUEST, 0x06, 0x01, NULL, 0);
    }
    return 0;
}

/* One poll, unless a start-up or poll write is still on its way */
static void DJI_SendPoll(SDL_DJIRemoteState *s, uint64_t now)
{
    uint8_t frame[SDL_SERIAL_MAX_WRITE];

    if (s->waiting_seq) {
        return;
    }
    DJI_Write(s, frame, DJI_BuildPoll(s, frame, sizeof(frame)), now);
}

static void DJI_WriteSimulator(SDL_DJIRemoteState *s, uint64_t now)
{
    uint8_t frame[SDL_DJI_MIN_FRAME + 1];
    const uint8_t on = 0x01;
    const size_t length = SDL_DJI_BuildFrame(frame, sizeof(frame), SDL_DJI_MODULE_PC, SDL_DJI_MODULE_RC, DJI_SERIAL_SEQUENCE,
                                             SDL_DJI_TYPE_REQUEST, 0x06, 0x24, &on, 1);

    DJI_Write(s, frame, length, now);
}

static void DJI_ArmSilence(SDL_DJIRemoteState *s, uint64_t now)
{
    s->silence_timer = true;
    s->silence_deadline = now + DJI_SilenceMs(s);
}

static void DJI_Identity(const SDL_DJIRemoteState *s, SDL_SerialIdentity *identity)
{
    const char *name;
    int nbuttons = 0;
    int naxes = s->naxes;

    switch (s->kind) {
    case SDL_DJI_REMOTE_RCN1:
        name = "DJI RC-N1";
        nbuttons = SDL_DJI_BUTTON_RCN1_BIT7 + 1;
        break;
    case SDL_DJI_REMOTE_MAVIC_MINI:
        name = "DJI Mavic Mini Remote";
        break;
    case SDL_DJI_REMOTE_PHANTOM3:
        name = "DJI Phantom 3 Remote";
        nbuttons = SDL_DJI_BUTTON_DIAL_DOWN + 1;
        naxes = 5;
        break;
    case SDL_DJI_REMOTE_PHANTOM2:
        name = "DJI Phantom 2 Remote";
        nbuttons = SDL_DJI_BUTTON_LEVER + 3;
        naxes = 5;
        break;
    default:
        name = SDL_DJI_NameForModel(s->model, "DJI RC (RM330)");
        naxes = 6;
        break;
    }
    SDL_Serial_SetIdentity(identity, name, SDL_SERIAL_TYPE_GAMEPAD, naxes, nbuttons, 0, 0);
    identity->has_mapping = true;
    identity->mapping.leftx = SDL_Serial_MapAxis(SDL_DJI_AXIS_LEFT_X);
    identity->mapping.lefty = SDL_Serial_MapAxis(SDL_DJI_AXIS_LEFT_Y);
    identity->mapping.rightx = SDL_Serial_MapAxis(SDL_DJI_AXIS_RIGHT_X);
    identity->mapping.righty = SDL_Serial_MapAxis(SDL_DJI_AXIS_RIGHT_Y);
    /* DJI-RC-Emulator's defaults: the dial one way on Y, the other on B */
    identity->mapping.y = SDL_Serial_MapHalfAxis(SDL_DJI_AXIS_DIAL, true);
    identity->mapping.b = SDL_Serial_MapHalfAxis(SDL_DJI_AXIS_DIAL, false);
    if (s->kind == SDL_DJI_REMOTE_PHANTOM3) {
        identity->mapping.a = SDL_Serial_MapButton(SDL_DJI_BUTTON_SHUTTER);
        identity->mapping.x = SDL_Serial_MapButton(SDL_DJI_BUTTON_RECORD);
        identity->mapping.leftshoulder = SDL_Serial_MapButton(SDL_DJI_BUTTON_C1);
        identity->mapping.rightshoulder = SDL_Serial_MapButton(SDL_DJI_BUTTON_C2);
        identity->mapping.guide = SDL_Serial_MapButton(SDL_DJI_BUTTON_HOME);
    }
}

/* A stick report came: presence, the controls, the silence timer */
static void DJI_StickReport(SDL_DJIRemoteState *s, int naxes, uint64_t now)
{
    DJI_ArmSilence(s, now);
    if (!SDL_Serial_IsPresent(&s->base, 0)) {
        SDL_SerialIdentity identity;

        s->naxes = naxes;
        DJI_Identity(s, &identity);
        SDL_Serial_Present(&s->base, 0, &identity);
    }
    SDL_Serial_Commit(&s->base, 0, &s->controls);
}

static void DJI_SetMode(SDL_SerialControls *controls, int first, int position)
{
    int i;

    for (i = 0; i < 3; ++i) {
        SDL_Serial_SetButton(controls, first + i, position == i);
    }
}

static void DJI_Start(SDL_DJIRemoteState *s, uint64_t now)
{
    s->timer = false;
    s->silence_timer = false;
    s->simulator_timer = false;
    s->simulator_waiting = false;
    s->fallback_timer = false;
    s->fallback_armed = false;
    s->test_stick = false;
    s->dropping = false;
    s->wheel_known = false;
    s->window_length = 0;
    /* A remote that comes back starts from rest */
    memset(&s->controls, 0, sizeof(s->controls));
    s->mode = -1;
    s->levers[0] = -1;
    s->levers[1] = -1;
    SDL_DJIParser_Init(&s->parser, false);

    switch (s->kind) {
    case SDL_DJI_REMOTE_RCN1:
    case SDL_DJI_REMOTE_BULK:
        s->step = SDL_DJI_STEP_SIMULATOR;
        DJI_WriteSimulator(s, now);
        break;
    case SDL_DJI_REMOTE_PHANTOM2:
        s->step = SDL_DJI_STEP_INIT;
        DJI_Write(s, dji_phantom2_init, sizeof(dji_phantom2_init), now);
        break;
    default:
        s->step = SDL_DJI_STEP_POLL;
        DJI_ArmSilence(s, now);
        DJI_SendPoll(s, now);
        break;
    }
}

/* Silence: the remote goes and start-up begins again */
static void DJI_Restart(SDL_DJIRemoteState *s, uint64_t now)
{
    SDL_Serial_Absent(&s->base, 0);
    SDL_Serial_ClearActions(&s->base);
    s->waiting_seq = 0;
    DJI_Start(s, now);
}

/* Phantom 2: judge the bytes of the last 10 ms as mDjiController judges one
 * read, then ping */
static void DJI_Phantom2Pass(SDL_DJIRemoteState *s, uint64_t now)
{
    SDL_DJIPhantom2Report report;
    int i;

    if (s->window_length == SDL_DJI_PHANTOM2_REPLY_LENGTH &&
        SDL_DJI_DecodePhantom2(s->window, s->window_length, &report)) {
        s->controls.axes[SDL_DJI_AXIS_LEFT_X] = SDL_DJI_ScaleAxis(report.left_x, 0, 1000);
        s->controls.axes[SDL_DJI_AXIS_LEFT_Y] = (int16_t)-SDL_DJI_ScaleAxis(report.left_y, 0, 1000);
        s->controls.axes[SDL_DJI_AXIS_RIGHT_X] = SDL_DJI_ScaleAxis(report.right_x, 0, 1000);
        s->controls.axes[SDL_DJI_AXIS_RIGHT_Y] = (int16_t)-SDL_DJI_ScaleAxis(report.right_y, 0, 1000);
        s->controls.axes[SDL_DJI_AXIS_DIAL] = SDL_DJI_ScaleAxis(report.dial, 0, 1000);
        /* -780, 0 and 780 are the three positions. Any other value keeps
           the last one, as mDjiController does for the left lever. */
        for (i = 0; i < 2; ++i) {
            if (report.levers[i] == -780) {
                s->levers[i] = 0;
            } else if (report.levers[i] == 0) {
                s->levers[i] = 1;
            } else if (report.levers[i] == 780) {
                s->levers[i] = 2;
            }
        }
        DJI_SetMode(&s->controls, SDL_DJI_BUTTON_MODE, s->levers[0]);
        DJI_SetMode(&s->controls, SDL_DJI_BUTTON_LEVER, s->levers[1]);
        DJI_StickReport(s, 5, now);
    }
    s->window_length = 0;
    DJI_SendPoll(s, now);
}

static void DJI_EnterPoll(SDL_DJIRemoteState *s, uint64_t now)
{
    s->step = SDL_DJI_STEP_POLL;
    DJI_ArmSilence(s, now);
    if (s->kind == SDL_DJI_REMOTE_PHANTOM2) {
        DJI_Phantom2Pass(s, now);
    } else {
        DJI_SendPoll(s, now);
    }
}

static void DJI_OnChannels(SDL_DJIRemoteState *s, const uint16_t *values, int count, uint64_t now)
{
    s->controls.axes[SDL_DJI_AXIS_RIGHT_X] = SDL_DJI_ScaleAxis(values[0], 1024, 660);
    s->controls.axes[SDL_DJI_AXIS_RIGHT_Y] = (int16_t)-SDL_DJI_ScaleAxis(values[1], 1024, 660);
    s->controls.axes[SDL_DJI_AXIS_LEFT_Y] = (int16_t)-SDL_DJI_ScaleAxis(values[2], 1024, 660);
    s->controls.axes[SDL_DJI_AXIS_LEFT_X] = SDL_DJI_ScaleAxis(values[3], 1024, 660);
    s->controls.axes[SDL_DJI_AXIS_DIAL] = SDL_DJI_ScaleAxis(values[4], 1024, 660);
    if (count > 5) {
        s->controls.axes[SDL_DJI_AXIS_DIAL2] = SDL_DJI_ScaleAxis(values[5], 1024, 660);
    }
    DJI_StickReport(s, count, now);
}

static void DJI_OnPhantom3(SDL_DJIRemoteState *s, const SDL_DJIPhantom3Report *report, uint64_t now)
{
    SDL_SerialControls *c = &s->controls;
    int step = 0;

    c->axes[SDL_DJI_AXIS_LEFT_X] = SDL_DJI_ScaleAxis(report->left_x, 2100, 1350);
    c->axes[SDL_DJI_AXIS_LEFT_Y] = (int16_t)-SDL_DJI_ScaleAxis(report->left_y, 2100, 1350);
    c->axes[SDL_DJI_AXIS_RIGHT_X] = SDL_DJI_ScaleAxis(report->right_x, 2100, 1350);
    c->axes[SDL_DJI_AXIS_RIGHT_Y] = (int16_t)-SDL_DJI_ScaleAxis(report->right_y, 2100, 1350);
    c->axes[SDL_DJI_AXIS_DIAL] = SDL_DJI_ScaleAxis(report->dial, 0, 32767);

    SDL_Serial_SetButton(c, SDL_DJI_BUTTON_HOME, (report->buttons & 0x80) != 0);
    SDL_Serial_SetButton(c, SDL_DJI_BUTTON_RECORD, (report->buttons & 0x40) != 0);
    SDL_Serial_SetButton(c, SDL_DJI_BUTTON_SHUTTER, (report->buttons & 0x20) != 0);
    SDL_Serial_SetButton(c, SDL_DJI_BUTTON_PLAYBACK, (report->buttons & 0x10) != 0);
    SDL_Serial_SetButton(c, SDL_DJI_BUTTON_DIAL_PRESS, (report->buttons & 0x08) != 0);
    SDL_Serial_SetButton(c, SDL_DJI_BUTTON_C2, (report->buttons & 0x04) != 0);
    SDL_Serial_SetButton(c, SDL_DJI_BUTTON_C1, (report->buttons & 0x02) != 0);

    /* The mode byte read as signed: below 16 P, below 32 A, else F. A
       negative value keeps the last position, as mDjiController does. */
    if (report->mode >= 32) {
        s->mode = 2;
    } else if (report->mode >= 16) {
        s->mode = 1;
    } else if (report->mode >= 0) {
        s->mode = 0;
    }
    DJI_SetMode(c, SDL_DJI_BUTTON_MODE, s->mode);

    /* The right dial is a counter. Its change, taken modulo 256, is a step. */
    if (s->wheel_known) {
        step = (int8_t)(uint8_t)(report->wheel - s->wheel);
    }
    s->wheel = report->wheel;
    s->wheel_known = true;

    DJI_StickReport(s, 5, now);
    if (step > 0) {
        SDL_Serial_Pulse(&s->base, 0, SDL_DJI_BUTTON_DIAL_UP, now);
    } else if (step < 0) {
        SDL_Serial_Pulse(&s->base, 0, SDL_DJI_BUTTON_DIAL_DOWN, now);
    }
}

static void DJI_OnTestStick(SDL_DJIRemoteState *s, const uint16_t *values, uint64_t now)
{
    s->controls.axes[SDL_DJI_AXIS_RIGHT_X] = DJI_ScaleDeadZone(values[0], 2048, 1565);
    s->controls.axes[SDL_DJI_AXIS_RIGHT_Y] = (int16_t)-DJI_ScaleDeadZone(values[1], 2048, 1565);
    s->controls.axes[SDL_DJI_AXIS_LEFT_Y] = (int16_t)-DJI_ScaleDeadZone(values[2], 2048, 1565);
    s->controls.axes[SDL_DJI_AXIS_LEFT_X] = DJI_ScaleDeadZone(values[3], 2048, 1565);
    DJI_StickReport(s, 6, now);
}

typedef struct DJI_FrameContext
{
    SDL_DJIRemoteState *state;
    uint64_t now;
} DJI_FrameContext;

static void DJI_OnFrame(void *userdata, const SDL_DJIFrame *frame)
{
    const DJI_FrameContext *context = (const DJI_FrameContext *)userdata;
    SDL_DJIRemoteState *s = context->state;
    const uint64_t now = context->now;
    uint16_t values[6];
    int count;
    char model[SDL_DJI_MODEL_LENGTH];

    if (SDL_DJI_DecodeModel(frame, model, sizeof(model))) {
        memcpy(s->model, model, sizeof(model));
        return;
    }

    switch (s->kind) {
    case SDL_DJI_REMOTE_RCN1:
    case SDL_DJI_REMOTE_MAVIC_MINI:
    case SDL_DJI_REMOTE_BULK:
        count = SDL_DJI_DecodeChannels(frame, values);
        if (count > 0) {
            if (s->kind == SDL_DJI_REMOTE_BULK && count == 6) {
                /* The 06/01 answer came, so 06/F5 is not needed */
                s->fallback_timer = false;
            }
            DJI_OnChannels(s, values, count, now);
            /* A reply asks for the next poll at once. The 06/F5 loop keeps
               its own period, and a reply during a wait leaves the wait. */
            if (s->step == SDL_DJI_STEP_POLL && !(s->kind == SDL_DJI_REMOTE_BULK && s->test_stick)) {
                DJI_SendPoll(s, now);
            }
            return;
        }
        if (s->kind == SDL_DJI_REMOTE_RCN1) {
            int mode;
            uint8_t bits;

            if (SDL_DJI_DecodeRCN1Status(frame, &mode, &bits)) {
                s->mode = mode;
                DJI_SetMode(&s->controls, SDL_DJI_BUTTON_MODE, mode);
                SDL_Serial_SetButton(&s->controls, SDL_DJI_BUTTON_RCN1_BIT1, (bits & 0x02) != 0);
                SDL_Serial_SetButton(&s->controls, SDL_DJI_BUTTON_RCN1_BIT2, (bits & 0x04) != 0);
                SDL_Serial_SetButton(&s->controls, SDL_DJI_BUTTON_RCN1_BITS56, (bits & 0x60) == 0x60);
                SDL_Serial_SetButton(&s->controls, SDL_DJI_BUTTON_RCN1_BIT7, (bits & 0x80) != 0);
                if (SDL_Serial_IsPresent(&s->base, 0)) {
                    SDL_Serial_Commit(&s->base, 0, &s->controls);
                }
            }
        } else if (s->kind == SDL_DJI_REMOTE_BULK && SDL_DJI_DecodeTestStick(frame, values)) {
            DJI_OnTestStick(s, values, now);
        }
        break;
    case SDL_DJI_REMOTE_PHANTOM3:
    {
        SDL_DJIPhantom3Report report;

        if (SDL_DJI_DecodePhantom3(frame, &report)) {
            DJI_OnPhantom3(s, &report, now);
        }
        break;
    }
    default:
        break;
    }
}

static void DJI_Reset(SDL_DJIRemoteState *s, SDL_DJIRemoteKind kind, const SDL_SerialSink *sink, uint64_t now)
{
    SDL_SerialLine line;
    const uint8_t write_seq = s->write_seq;

    SDL_Serial_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->write_seq = write_seq;
    s->kind = kind;
    line = SDL_Serial_Line(SDL_DJI_RATE, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);
    SDL_Serial_QueueLine(&s->base, 0, &line);
    DJI_Start(s, now);
}

static void DJI_ResetRCN1(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    DJI_Reset((SDL_DJIRemoteState *)state, SDL_DJI_REMOTE_RCN1, sink, now);
}

static void DJI_ResetMavicMini(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    DJI_Reset((SDL_DJIRemoteState *)state, SDL_DJI_REMOTE_MAVIC_MINI, sink, now);
}

static void DJI_ResetPhantom3(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    DJI_Reset((SDL_DJIRemoteState *)state, SDL_DJI_REMOTE_PHANTOM3, sink, now);
}

static void DJI_ResetPhantom2(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    DJI_Reset((SDL_DJIRemoteState *)state, SDL_DJI_REMOTE_PHANTOM2, sink, now);
}

static void DJI_ResetBulk(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    DJI_Reset((SDL_DJIRemoteState *)state, SDL_DJI_REMOTE_BULK, sink, now);
}

static void DJI_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_DJIRemoteState *s = (SDL_DJIRemoteState *)state;
    DJI_FrameContext context;

    if (s->kind == SDL_DJI_REMOTE_PHANTOM2) {
        size_t i;

        for (i = 0; i < length; ++i) {
            if (s->window_length < SDL_DJI_PHANTOM2_REPLY_LENGTH) {
                s->window[s->window_length] = data[i];
            }
            /* One past 76 marks a read too long to be a reply */
            if (s->window_length <= SDL_DJI_PHANTOM2_REPLY_LENGTH) {
                ++s->window_length;
            }
        }
        return;
    }
    if (s->dropping) {
        return;
    }
    context.state = s;
    context.now = now;
    SDL_DJIParser_Feed(&s->parser, data, length, DJI_OnFrame, &context);
}

static void DJI_Tick(void *state, uint64_t now)
{
    SDL_DJIRemoteState *s = (SDL_DJIRemoteState *)state;

    SDL_Serial_TickPulses(&s->base, now);
    if (s->silence_timer && now >= s->silence_deadline) {
        DJI_Restart(s, now);
        return;
    }
    if (s->fallback_timer && now >= s->fallback_deadline) {
        /* No 32-byte 06/01 answer: poll with 06/F5 as dji-rc-joystick does,
           which sends no 06/24 */
        s->fallback_timer = false;
        s->test_stick = true;
        s->simulator_timer = false;
        if (!s->waiting_seq) {
            DJI_SendPoll(s, now);
        }
    }
    if (s->simulator_timer && now >= s->simulator_deadline) {
        uint8_t frame[SDL_DJI_MIN_FRAME + 1];
        const uint8_t on = 0x01;
        const size_t length = SDL_DJI_BuildFrame(frame, sizeof(frame), SDL_DJI_MODULE_PC, SDL_DJI_MODULE_RC,
                                                 DJI_SERIAL_SEQUENCE, SDL_DJI_TYPE_REQUEST, 0x06, 0x24, &on, 1);

        s->simulator_timer = false;
        if (SDL_Serial_QueueWrite(&s->base, DJI_SIMULATOR_TAG, frame, length)) {
            s->simulator_waiting = true;
        } else {
            s->simulator_timer = true;
            s->simulator_deadline = now + SDL_DJI_BULK_SIMULATOR_MS;
        }
    }
    if (s->timer && now >= s->deadline) {
        s->timer = false;
        switch (s->step) {
        case SDL_DJI_STEP_SETTLE:
            s->dropping = false;
            DJI_EnterPoll(s, now);
            break;
        case SDL_DJI_STEP_INIT_WAIT:
            DJI_EnterPoll(s, now);
            break;
        case SDL_DJI_STEP_POLL:
            if (s->kind == SDL_DJI_REMOTE_PHANTOM2) {
                DJI_Phantom2Pass(s, now);
            } else {
                DJI_SendPoll(s, now);
            }
            break;
        default:
            /* A write that could not be queued: try the step again */
            if (s->step == SDL_DJI_STEP_SIMULATOR) {
                DJI_WriteSimulator(s, now);
            } else if (s->step == SDL_DJI_STEP_INIT) {
                DJI_Write(s, dji_phantom2_init, sizeof(dji_phantom2_init), now);
            }
            break;
        }
    }
}

static void DJI_ActionDone(void *state, bool success, uint64_t now)
{
    SDL_DJIRemoteState *s = (SDL_DJIRemoteState *)state;
    uint8_t tag;

    /* A failed write counts as sent: every step is repeated anyway */
    (void)success;
    if (!SDL_Serial_FinishAction(&s->base, &tag) || tag == 0) {
        return;
    }
    if (tag == DJI_SIMULATOR_TAG) {
        if (s->simulator_waiting) {
            s->simulator_waiting = false;
            if (!s->test_stick) {
                s->simulator_timer = true;
                s->simulator_deadline = now + SDL_DJI_BULK_SIMULATOR_MS;
            }
        }
        return;
    }
    if (tag != s->waiting_seq) {
        return;
    }
    s->waiting_seq = 0;
    switch (s->step) {
    case SDL_DJI_STEP_SIMULATOR:
        s->step = SDL_DJI_STEP_SETTLE;
        if (s->kind == SDL_DJI_REMOTE_BULK) {
            /* DJI-RC-Emulator: 100 ms, and 06/24 again every 3000 ms */
            DJI_SetTimer(s, now + SDL_DJI_BULK_SETTLE_MS);
            s->simulator_timer = true;
            s->simulator_deadline = now + SDL_DJI_BULK_SIMULATOR_MS;
        } else {
            /* DJI-RC-Emulator: 50 ms, then the input so far is dropped */
            s->dropping = true;
            SDL_DJIParser_Init(&s->parser, false);
            DJI_SetTimer(s, now + SDL_DJI_SIMULATOR_SETTLE_MS);
        }
        break;
    case SDL_DJI_STEP_INIT:
        s->step = SDL_DJI_STEP_INIT_WAIT;
        DJI_SetTimer(s, now + SDL_DJI_PHANTOM2_WAIT_MS);
        break;
    case SDL_DJI_STEP_POLL:
        DJI_SetTimer(s, now + DJI_PollMs(s));
        if (s->kind == SDL_DJI_REMOTE_BULK && !s->test_stick && !s->fallback_armed) {
            s->fallback_armed = true;
            s->fallback_timer = true;
            s->fallback_deadline = now + SDL_DJI_BULK_FALLBACK_MS;
        }
        break;
    default:
        break;
    }
}

static void DJI_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    (void)state;
    (void)request;
    (void)now;
}

static bool DJI_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_DJIRemoteState *s = (SDL_DJIRemoteState *)state;
    bool have = false;
    uint64_t earliest = 0;

    if (s->timer) {
        SDL_Serial_EarlierDeadline(&have, &earliest, s->deadline);
    }
    if (s->silence_timer) {
        SDL_Serial_EarlierDeadline(&have, &earliest, s->silence_deadline);
    }
    if (s->simulator_timer) {
        SDL_Serial_EarlierDeadline(&have, &earliest, s->simulator_deadline);
    }
    if (s->fallback_timer) {
        SDL_Serial_EarlierDeadline(&have, &earliest, s->fallback_deadline);
    }
    SDL_Serial_PulseDeadline(&s->base, &have, &earliest);
    if (have) {
        *deadline = earliest;
    }
    return have;
}

const SDL_SerialModule SDL_DJIRemoteRCN1Module = {
    "dji", sizeof(SDL_DJIRemoteState), false, 0, 0,
    DJI_ResetRCN1, DJI_Feed, DJI_Tick, SDL_Serial_NextAction, DJI_ActionDone, DJI_Output, DJI_GetDeadline, SDL_Serial_GetSnapshot
};

const SDL_SerialModule SDL_DJIRemoteMavicMiniModule = {
    "djimavicmini", sizeof(SDL_DJIRemoteState), false, 0, 0,
    DJI_ResetMavicMini, DJI_Feed, DJI_Tick, SDL_Serial_NextAction, DJI_ActionDone, DJI_Output, DJI_GetDeadline, SDL_Serial_GetSnapshot
};

const SDL_SerialModule SDL_DJIRemotePhantom3Module = {
    "djiphantom3", sizeof(SDL_DJIRemoteState), false, 0, 0,
    DJI_ResetPhantom3, DJI_Feed, DJI_Tick, SDL_Serial_NextAction, DJI_ActionDone, DJI_Output, DJI_GetDeadline, SDL_Serial_GetSnapshot
};

const SDL_SerialModule SDL_DJIRemotePhantom2Module = {
    "djiphantom2", sizeof(SDL_DJIRemoteState), false, 0, 0,
    DJI_ResetPhantom2, DJI_Feed, DJI_Tick, SDL_Serial_NextAction, DJI_ActionDone, DJI_Output, DJI_GetDeadline, SDL_Serial_GetSnapshot
};

const SDL_SerialModule SDL_DJIRemoteBulkModule = {
    "djibulk", sizeof(SDL_DJIRemoteState), false, 0, 0,
    DJI_ResetBulk, DJI_Feed, DJI_Tick, SDL_Serial_NextAction, DJI_ActionDone, DJI_Output, DJI_GetDeadline, SDL_Serial_GetSnapshot
};
