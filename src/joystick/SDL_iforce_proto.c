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

/* I-Force wheels and joysticks. See SDL_iforce_proto.h. */

#include "SDL_iforce_proto.h"

#include <string.h>

/* The USB IDs of Linux's iforce driver, the Force RS ID that only the
 * forcers INF package carries, and the two serial wheels Linux knows by
 * their M and P replies. Linux files the Jet Leader under 0001, so a real
 * 0003 gets its joystick layout with the rudder here. */
static const SDL_IForceModel iforce_models[] = {
    { 0x044F, 0xA01C, "Thrustmaster Motor Sport GT", SDL_IFORCE_LAYOUT_WHEEL, false, true },
    { 0x046D, 0xC281, "Logitech WingMan Force", SDL_IFORCE_LAYOUT_JOYSTICK, false, true },
    { 0x046D, 0xC291, "Logitech WingMan Formula Force", SDL_IFORCE_LAYOUT_WHEEL, false, true },
    { 0x05EF, 0x020A, "AVB Top Shot Pegasus", SDL_IFORCE_LAYOUT_AVB, true, true },
    { 0x05EF, 0x8884, "AVB Mag Turbo Force", SDL_IFORCE_LAYOUT_WHEEL, false, true },
    { 0x05EF, 0x8888, "AVB Top Shot Force Feedback Racing Wheel", SDL_IFORCE_LAYOUT_WHEEL, false, true },
    { 0x061C, 0xC084, "ACT LABS Force RS", SDL_IFORCE_LAYOUT_WHEEL, false, true },
    { 0x061C, 0xC094, "ACT LABS Force RS", SDL_IFORCE_LAYOUT_WHEEL, false, true },
    { 0x061C, 0xC0A4, "ACT LABS Force RS", SDL_IFORCE_LAYOUT_WHEEL, false, true },
    { 0x06A3, 0xFF04, "Saitek R440 Force Wheel", SDL_IFORCE_LAYOUT_WHEEL, false, true },
    { 0x06F8, 0x0001, "Guillemot Race Leader Force Feedback", SDL_IFORCE_LAYOUT_WHEEL, false, true },
    { 0x06F8, 0x0003, "Guillemot Jet Leader Force Feedback", SDL_IFORCE_LAYOUT_JOYSTICK, true, true },
    { 0x06F8, 0x0004, "Guillemot Force Feedback Racing Wheel", SDL_IFORCE_LAYOUT_GUILLEMOT_WHEEL, false, true },
    { 0x06F8, 0xA302, "Guillemot Jet Leader 3D", SDL_IFORCE_LAYOUT_JOYSTICK, false, true },
    { 0x05EF, 0x8886, "Boeder Force Feedback Wheel", SDL_IFORCE_LAYOUT_WHEEL, false, false },
    { 0x06D6, 0x29BC, "Trust Force Feedback Race Master", SDL_IFORCE_LAYOUT_WHEEL, false, false },
};

static const SDL_IForceModel iforce_unknown = {
    0x0000, 0x0000, "Unknown I-Force Device", SDL_IFORCE_LAYOUT_JOYSTICK, false, false
};

const SDL_IForceModel *SDL_IForce_FindModel(uint16_t vendor_id, uint16_t product_id, bool usb_only)
{
    size_t i;

    for (i = 0; i < sizeof(iforce_models) / sizeof(iforce_models[0]); ++i) {
        const SDL_IForceModel *model = &iforce_models[i];

        if (model->vendor_id == vendor_id && model->product_id == product_id && (model->usb || !usb_only)) {
            return model;
        }
    }
    return NULL;
}

const SDL_IForceModel *SDL_IForce_UnknownModel(void)
{
    return &iforce_unknown;
}

bool SDL_IForce_IsWheel(const SDL_IForceModel *model)
{
    return model && (model->layout == SDL_IFORCE_LAYOUT_WHEEL || model->layout == SDL_IFORCE_LAYOUT_GUILLEMOT_WHEEL);
}

void SDL_IForce_GetIdentity(const SDL_IForceModel *model, SDL_IForceIdentity *identity)
{
    identity->naxes = 3;
    identity->nhats = 1;
    identity->wheel = SDL_IForce_IsWheel(model);
    switch (model->layout) {
    case SDL_IFORCE_LAYOUT_JOYSTICK:
        /* Entry 12 of the table is the hat's low bit, so 12 buttons */
        identity->nbuttons = 12;
        break;
    case SDL_IFORCE_LAYOUT_AVB:
        identity->nbuttons = 9;
        break;
    case SDL_IFORCE_LAYOUT_GUILLEMOT_WHEEL:
        identity->nbuttons = 6;
        identity->nhats = 2;
        break;
    default:
        identity->nbuttons = 8;
        break;
    }
    if (model->rudder) {
        identity->naxes = 4;
    }
    /* The grip sensor of the status packet follows the others */
    ++identity->nbuttons;
}

static int16_t IForce_Pedal(uint8_t raw)
{
    /* Linux reports 255 minus the byte. 0xFF is released. */
    return (int16_t)((255 - raw) * 257 - 32768);
}

static int16_t IForce_Stick(const uint8_t *data)
{
    /* Linux declares -1920 to 1920 */
    const int16_t raw = (int16_t)(uint16_t)(data[0] | (data[1] << 8));
    int32_t value = (int32_t)raw * 32767 / 1920;

    if (value > 32767) {
        value = 32767;
    } else if (value < -32768) {
        value = -32768;
    }
    return (int16_t)value;
}

static int16_t IForce_Rudder(uint8_t raw)
{
    return (int16_t)(((int)(int8_t)raw + 128) * 257 - 32768);
}

void SDL_IForce_ResetState(const SDL_IForceModel *model, SDL_IForceState *state)
{
    memset(state, 0, sizeof(*state));
    if (SDL_IForce_IsWheel(model)) {
        state->axes[1] = IForce_Pedal(0xFF);
        state->axes[2] = IForce_Pedal(0xFF);
    } else {
        state->axes[2] = IForce_Pedal(0xFF);
    }
}

/* The high nibble of data 6: 0 up, clockwise to 7 up-left, 8 to F centered */
static const uint8_t iforce_hats[16] = {
    SDL_IFORCE_HAT_UP,
    SDL_IFORCE_HAT_UP | SDL_IFORCE_HAT_RIGHT,
    SDL_IFORCE_HAT_RIGHT,
    SDL_IFORCE_HAT_DOWN | SDL_IFORCE_HAT_RIGHT,
    SDL_IFORCE_HAT_DOWN,
    SDL_IFORCE_HAT_DOWN | SDL_IFORCE_HAT_LEFT,
    SDL_IFORCE_HAT_LEFT,
    SDL_IFORCE_HAT_UP | SDL_IFORCE_HAT_LEFT,
    0, 0, 0, 0, 0, 0, 0, 0
};

/* The buttons and hats both input packets carry in data 5 and 6 */
static void IForce_ButtonsAndHats(const SDL_IForceModel *model, const uint8_t *data, SDL_IForceState *state)
{
    SDL_IForceIdentity identity;
    uint16_t grip;
    uint16_t buttons;

    SDL_IForce_GetIdentity(model, &identity);
    grip = (uint16_t)(state->buttons & (1u << (identity.nbuttons - 1)));

    state->hats[0] = iforce_hats[data[6] >> 4];
    switch (model->layout) {
    case SDL_IFORCE_LAYOUT_JOYSTICK:
        buttons = (uint16_t)(data[5] | ((data[6] & 0x0F) << 8));
        break;
    case SDL_IFORCE_LAYOUT_AVB:
        buttons = (uint16_t)(data[5] | ((data[6] & 0x01) << 8));
        break;
    case SDL_IFORCE_LAYOUT_GUILLEMOT_WHEEL:
        /* The right hat: up and right in data 5, down and left in data 6 */
        buttons = (uint16_t)(data[5] & 0x3F);
        state->hats[1] = (uint8_t)(((data[5] & 0x40) ? SDL_IFORCE_HAT_UP : 0) |
                                   ((data[5] & 0x80) ? SDL_IFORCE_HAT_RIGHT : 0) |
                                   ((data[6] & 0x01) ? SDL_IFORCE_HAT_DOWN : 0) |
                                   ((data[6] & 0x02) ? SDL_IFORCE_HAT_LEFT : 0));
        break;
    default:
        buttons = data[5];
        break;
    }
    state->buttons = (uint16_t)(buttons | grip);
}

int SDL_IForce_DecodePacket(const SDL_IForceModel *model, uint8_t type, const uint8_t *data, size_t length,
                            SDL_IForceState *state, SDL_IForceStatus *status)
{
    size_t j;

    if (!model || !state || (length && !data)) {
        return SDL_IFORCE_PACKET_IGNORED;
    }
    switch (type) {
    case SDL_IFORCE_PACKET_JOYSTICK:
        if (length < 7) {
            return SDL_IFORCE_PACKET_IGNORED;
        }
        /* A wheel declares none of these axes, and Linux drops them */
        if (!SDL_IForce_IsWheel(model)) {
            state->axes[0] = IForce_Stick(data);
            state->axes[1] = IForce_Stick(data + 2);
            state->axes[2] = IForce_Pedal(data[4]);
            if (model->rudder && length >= 8) {
                state->axes[3] = IForce_Rudder(data[7]);
            }
        }
        IForce_ButtonsAndHats(model, data, state);
        return SDL_IFORCE_PACKET_INPUT;

    case SDL_IFORCE_PACKET_WHEEL:
        if (length < 7) {
            return SDL_IFORCE_PACKET_IGNORED;
        }
        if (SDL_IForce_IsWheel(model)) {
            state->axes[0] = IForce_Stick(data);
            state->axes[1] = IForce_Pedal(data[2]);
            state->axes[2] = IForce_Pedal(data[3]);
        }
        IForce_ButtonsAndHats(model, data, state);
        return SDL_IFORCE_PACKET_INPUT;

    case SDL_IFORCE_PACKET_STATUS:
    {
        SDL_IForceIdentity identity;
        SDL_IForceStatus report;

        if (length < 2) {
            return SDL_IFORCE_PACKET_IGNORED;
        }
        memset(&report, 0, sizeof(report));
        report.deadman = (data[0] & 0x02) != 0;
        report.effect = (uint8_t)(data[1] & 0x7F);
        report.playing = (data[1] & 0x80) != 0;
        for (j = 3; j + 2 <= length && report.addresses < SDL_IFORCE_MAX_ADDRESSES; j += 2) {
            report.address[report.addresses++] = (uint16_t)(data[j] | (data[j + 1] << 8));
        }
        SDL_IForce_GetIdentity(model, &identity);
        if (report.deadman) {
            state->buttons = (uint16_t)(state->buttons | (1u << (identity.nbuttons - 1)));
        } else {
            state->buttons = (uint16_t)(state->buttons & ~(1u << (identity.nbuttons - 1)));
        }
        if (status) {
            *status = report;
        }
        return SDL_IFORCE_PACKET_STATUS_REPORT;
    }

    default:
        return SDL_IFORCE_PACKET_IGNORED;
    }
}

static const uint8_t iforce_queries[] = { 'O', 'M', 'P', 'B', 'N', 'C', 'E', 'O', 'V' };

void SDL_IForce_InitQueries(SDL_IForceQueries *queries)
{
    memset(queries, 0, sizeof(*queries));
    queries->memory_end = SDL_IFORCE_MEMORY_DEFAULT;
}

uint8_t SDL_IForce_NextQuery(const SDL_IForceQueries *queries)
{
    if (queries->done || queries->step < 0 || queries->step >= (int)sizeof(iforce_queries)) {
        return 0;
    }
    return iforce_queries[queries->step];
}

void SDL_IForce_QueryResult(SDL_IForceQueries *queries, const uint8_t *reply, size_t length)
{
    const uint8_t letter = SDL_IForce_NextQuery(queries);
    const bool valid = letter && reply && length >= 1 && reply[0] == letter;

    if (!letter) {
        return;
    }
    if (queries->step == 0) {
        ++queries->open_tries;
        if (valid) {
            queries->open = true;
            queries->step = 1;
        } else if (queries->open_tries >= SDL_IFORCE_OPEN_TRIES) {
            queries->done = true;
        }
        return;
    }

    switch (letter) {
    case 'M':
        if (valid && length >= 3) {
            queries->vendor_id = (uint16_t)(reply[1] | (reply[2] << 8));
        }
        break;
    case 'P':
        if (valid && length >= 3) {
            queries->product_id = (uint16_t)(reply[1] | (reply[2] << 8));
        }
        break;
    case 'B':
        if (valid && length >= 3) {
            queries->memory_end = (uint16_t)(reply[1] | (reply[2] << 8));
        }
        break;
    case 'N':
        if (valid && length >= 2) {
            queries->effects = (reply[1] > SDL_IFORCE_EFFECTS_MAX) ? SDL_IFORCE_EFFECTS_MAX : reply[1];
        }
        break;
    default:
        /* C, E, the second O and V are for the log only */
        break;
    }
    if (++queries->step >= (int)sizeof(iforce_queries)) {
        queries->done = true;
    }
}

void SDL_IForce_QuerySetup(uint8_t letter, SDL_IForceControlSetup *setup)
{
    setup->request_type = SDL_IFORCE_QUERY_REQUEST_TYPE;
    setup->request = letter;
    setup->value = 0;
    setup->index = 0;
    setup->length = SDL_IFORCE_MAX_LENGTH;
}

void SDL_IForce_InitSerialParser(SDL_IForceSerialParser *parser)
{
    memset(parser, 0, sizeof(*parser));
}

static void IForce_EndFrame(SDL_IForceSerialParser *parser)
{
    parser->started = false;
    parser->type = 0;
    parser->length = 0;
    parser->count = 0;
    parser->check = 0;
}

void SDL_IForce_FeedSerial(SDL_IForceSerialParser *parser, const uint8_t *bytes, size_t length,
                           SDL_IForceFrameHandler handler, void *userdata)
{
    size_t i;

    if (!bytes) {
        return;
    }
    for (i = 0; i < length; ++i) {
        const uint8_t byte = bytes[i];

        if (!parser->started) {
            if (byte == SDL_IFORCE_SERIAL_START) {
                parser->started = true;
                parser->check = byte;
            }
            continue;
        }
        if (!parser->type) {
            /* A 00 is skipped, and the next byte is read as the type */
            if (byte > SDL_IFORCE_PACKET_WHEEL && byte != SDL_IFORCE_PACKET_QUERY) {
                IForce_EndFrame(parser);
            } else if (byte) {
                parser->type = byte;
                parser->check ^= byte;
            }
            continue;
        }
        if (!parser->length) {
            /* A 00 is skipped, and the next byte is read as the length */
            if (byte > SDL_IFORCE_MAX_LENGTH) {
                IForce_EndFrame(parser);
            } else if (byte) {
                parser->length = byte;
                parser->check ^= byte;
            }
            continue;
        }
        if (parser->count < parser->length) {
            parser->data[parser->count++] = byte;
            parser->check ^= byte;
            continue;
        }
        /* The checksum, which Linux takes without comparing */
        if (byte != parser->check) {
            ++parser->mismatches;
        }
        if (handler) {
            handler(userdata, parser->type, parser->data, parser->length);
        }
        IForce_EndFrame(parser);
    }
}

size_t SDL_IForce_BuildSerialFrame(uint8_t *out, size_t size, uint8_t command, const uint8_t *data, size_t length)
{
    uint8_t check;
    size_t i;

    if (!out || length > SDL_IFORCE_MAX_LENGTH || (length && !data) || size < length + 4) {
        return 0;
    }
    out[0] = SDL_IFORCE_SERIAL_START;
    out[1] = command;
    out[2] = (uint8_t)length;
    check = (uint8_t)(out[0] ^ out[1] ^ out[2]);
    for (i = 0; i < length; ++i) {
        out[3 + i] = data[i];
        check ^= data[i];
    }
    out[3 + length] = check;
    return length + 4;
}

bool SDL_IForce_IsCommand(const uint8_t *bytes, size_t length)
{
    if (!bytes || length < 2) {
        return false;
    }
    switch (bytes[0]) {
    case SDL_IFORCE_CMD_EFFECT:
        return length == 1 + 14;
    case SDL_IFORCE_CMD_ENVELOPE:
        return length == 1 + 8;
    case SDL_IFORCE_CMD_MAGNITUDE:
        return length == 1 + 3;
    case SDL_IFORCE_CMD_PERIOD:
        return length == 1 + 7;
    case SDL_IFORCE_CMD_CONDITION:
        return length == 1 + 10;
    case SDL_IFORCE_CMD_CONTROL:
        return length == 1 + 2 || length == 1 + 3;
    case SDL_IFORCE_CMD_PLAY:
        return length == 1 + 3;
    case SDL_IFORCE_CMD_STATE:
    case SDL_IFORCE_CMD_GAIN:
        return length == 1 + 1;
    default:
        return false;
    }
}

static void IForce_Command(SDL_IForceCommand *command, uint8_t code, const uint8_t *data, size_t length)
{
    command->length = (uint8_t)(1 + length);
    command->bytes[0] = code;
    memcpy(command->bytes + 1, data, length);
}

void SDL_IForce_BuildGain(SDL_IForceCommand *command, uint16_t gain)
{
    const uint8_t data = (uint8_t)(gain >> 9);

    IForce_Command(command, SDL_IFORCE_CMD_GAIN, &data, 1);
}

void SDL_IForce_BuildState(SDL_IForceCommand *command, uint8_t state)
{
    IForce_Command(command, SDL_IFORCE_CMD_STATE, &state, 1);
}

void SDL_IForce_BuildPlay(SDL_IForceCommand *command, uint8_t index, uint32_t iterations)
{
    uint8_t data[3];

    data[0] = index;
    data[1] = (iterations > 0) ? ((iterations > 1) ? 0x41 : 0x01) : 0x00;
    data[2] = (uint8_t)((iterations > 255) ? 255 : iterations);
    IForce_Command(command, SDL_IFORCE_CMD_PLAY, data, sizeof(data));
}

void SDL_IForce_BuildAutocenter(SDL_IForceCommand commands[2], uint16_t strength)
{
    uint8_t data[2];

    data[0] = 0x03;
    data[1] = (uint8_t)(strength >> 9);
    IForce_Command(&commands[0], SDL_IFORCE_CMD_CONTROL, data, sizeof(data));
    data[0] = 0x04;
    data[1] = 0x01;
    IForce_Command(&commands[1], SDL_IFORCE_CMD_CONTROL, data, sizeof(data));
}

/* Division rounding toward negative infinity, which is what Linux's
 * arithmetic shifts of negative values give */
static int32_t IForce_FloorDivide(int32_t value, int32_t divisor)
{
    int32_t quotient = value / divisor;

    if ((value % divisor) != 0 && value < 0) {
        --quotient;
    }
    return quotient;
}

/* The high byte of a signed level, rounded toward zero, as Linux's
 * HIFIX80 gives it */
static uint8_t IForce_Level(int16_t level)
{
    return (uint8_t)(int8_t)(level / 256);
}

void SDL_IForce_InitFF(SDL_IForceFF *ff, int effects, uint16_t memory_end)
{
    memset(ff, 0, sizeof(*ff));
    ff->effects = (effects < 0) ? 0 : (effects > SDL_IFORCE_EFFECTS_MAX) ? SDL_IFORCE_EFFECTS_MAX : effects;
    ff->memory_end = memory_end;
}

/* First fit between 0 and memory_end, both included, as Linux's
 * allocate_resource does it. Every block size is even, so every start is
 * even, which is Linux's alignment of 2. */
static bool IForce_Allocate(SDL_IForceFF *ff, uint16_t size, SDL_IForceBlock *block)
{
    uint32_t start = 0;

    for (;;) {
        uint32_t next = start;
        int i, b;
        bool clash = false;

        if (start + size - 1 > ff->memory_end) {
            return false;
        }
        for (i = 0; i < ff->effects; ++i) {
            for (b = 0; b < 2; ++b) {
                const SDL_IForceBlock *other = &ff->slots[i].block[b];
                uint32_t end;

                if (!other->used || other == block) {
                    continue;
                }
                end = (uint32_t)other->start + other->size;
                if (other->start < start + size && start < end) {
                    clash = true;
                    if (end > next) {
                        next = end;
                    }
                }
            }
        }
        if (!clash) {
            memset(block, 0, sizeof(*block));
            block->used = true;
            block->start = (uint16_t)start;
            block->size = size;
            return true;
        }
        start = next;
    }
}

static uint16_t IForce_BlockSize(uint8_t type, int which)
{
    switch (type) {
    case SDL_IFORCE_EFFECT_CONSTANT:
        return which == 0 ? 0x02 : 0x0E;
    case SDL_IFORCE_EFFECT_PERIODIC:
        return which == 0 ? 0x0C : 0x0E;
    default:
        return 0x08;
    }
}

static bool IForce_EnvelopeChanged(const SDL_IForceEnvelope *a, const SDL_IForceEnvelope *b)
{
    return a->attack_length != b->attack_length || a->attack_level != b->attack_level ||
           a->fade_length != b->fade_length || a->fade_level != b->fade_level;
}

static bool IForce_ConditionChanged(const SDL_IForceEffect *a, const SDL_IForceEffect *b)
{
    int i;

    for (i = 0; i < 2; ++i) {
        const SDL_IForceCondition *x = &a->condition[i];
        const SDL_IForceCondition *y = &b->condition[i];

        if (x->right_saturation != y->right_saturation || x->left_saturation != y->left_saturation ||
            x->right_coeff != y->right_coeff || x->left_coeff != y->left_coeff ||
            x->deadband != y->deadband || x->center != y->center) {
            return true;
        }
    }
    return false;
}

/* Which blocks an upload writes, and whether it sends the core */
static void IForce_Changes(const SDL_IForceSlot *slot, const SDL_IForceEffect *effect, bool write[2], bool *core)
{
    const SDL_IForceEffect *old = &slot->effect;

    if (!slot->used) {
        write[0] = write[1] = true;
        *core = true;
        return;
    }
    switch (effect->type) {
    case SDL_IFORCE_EFFECT_CONSTANT:
        write[0] = old->level != effect->level;
        write[1] = IForce_EnvelopeChanged(&old->envelope, &effect->envelope);
        break;
    case SDL_IFORCE_EFFECT_PERIODIC:
        write[0] = old->period != effect->period || old->magnitude != effect->magnitude ||
                   old->offset != effect->offset || old->phase != effect->phase;
        write[1] = IForce_EnvelopeChanged(&old->envelope, &effect->envelope);
        break;
    default:
        /* Linux sends both condition blocks together */
        write[0] = write[1] = IForce_ConditionChanged(old, effect);
        break;
    }
    *core = old->direction != effect->direction || old->trigger_button != effect->trigger_button ||
            old->trigger_interval != effect->trigger_interval || old->length != effect->length ||
            old->delay != effect->delay;
}

static void IForce_Push(SDL_IForceCommands *commands, uint8_t code, const uint8_t *data, size_t length)
{
    if (commands->count < SDL_IFORCE_MAX_UPLOAD) {
        IForce_Command(&commands->command[commands->count++], code, data, length);
    }
}

static void IForce_WriteCondition(SDL_IForceCommands *commands, const SDL_IForceBlock *block, const SDL_IForceCondition *condition)
{
    uint8_t data[10];
    const int32_t center = IForce_FloorDivide(500 * (int32_t)condition->center, 32768);
    const uint32_t deadband = (1000u * condition->deadband) >> 16;

    data[0] = (uint8_t)(block->start & 0xFF);
    data[1] = (uint8_t)(block->start >> 8);
    data[2] = (uint8_t)IForce_FloorDivide(100 * (int32_t)condition->right_coeff, 32768);
    data[3] = (uint8_t)IForce_FloorDivide(100 * (int32_t)condition->left_coeff, 32768);
    data[4] = (uint8_t)((uint32_t)center & 0xFF);
    data[5] = (uint8_t)(((uint32_t)center >> 8) & 0xFF);
    data[6] = (uint8_t)(deadband & 0xFF);
    data[7] = (uint8_t)(deadband >> 8);
    data[8] = (uint8_t)((100u * condition->right_saturation) >> 16);
    data[9] = (uint8_t)((100u * condition->left_saturation) >> 16);
    IForce_Push(commands, SDL_IFORCE_CMD_CONDITION, data, sizeof(data));
}

static void IForce_WriteBlock(SDL_IForceCommands *commands, const SDL_IForceSlot *slot, int which, const SDL_IForceEffect *effect)
{
    const SDL_IForceBlock *block = &slot->block[which];
    uint8_t data[8];

    data[0] = (uint8_t)(block->start & 0xFF);
    data[1] = (uint8_t)(block->start >> 8);
    if (effect->type == SDL_IFORCE_EFFECT_SPRING || effect->type == SDL_IFORCE_EFFECT_DAMPER) {
        IForce_WriteCondition(commands, block, &effect->condition[which]);
    } else if (which == 1) {
        data[2] = (uint8_t)(effect->envelope.attack_length & 0xFF);
        data[3] = (uint8_t)(effect->envelope.attack_length >> 8);
        data[4] = (uint8_t)(effect->envelope.attack_level >> 8);
        data[5] = (uint8_t)(effect->envelope.fade_length & 0xFF);
        data[6] = (uint8_t)(effect->envelope.fade_length >> 8);
        data[7] = (uint8_t)(effect->envelope.fade_level >> 8);
        IForce_Push(commands, SDL_IFORCE_CMD_ENVELOPE, data, 8);
    } else if (effect->type == SDL_IFORCE_EFFECT_CONSTANT) {
        data[2] = IForce_Level(effect->level);
        IForce_Push(commands, SDL_IFORCE_CMD_MAGNITUDE, data, 3);
    } else {
        data[2] = IForce_Level(effect->magnitude);
        data[3] = IForce_Level(effect->offset);
        data[4] = (uint8_t)(effect->phase >> 8);
        data[5] = (uint8_t)(effect->period & 0xFF);
        data[6] = (uint8_t)(effect->period >> 8);
        IForce_Push(commands, SDL_IFORCE_CMD_PERIOD, data, 7);
    }
}

static void IForce_WriteCore(SDL_IForceCommands *commands, int index, const SDL_IForceSlot *slot, const SDL_IForceEffect *effect)
{
    uint8_t data[14];
    uint8_t waveform;
    uint8_t axes;

    switch (effect->type) {
    case SDL_IFORCE_EFFECT_CONSTANT:
        waveform = SDL_IFORCE_WAVE_CONSTANT;
        axes = 0x20;
        break;
    case SDL_IFORCE_EFFECT_PERIODIC:
        waveform = effect->waveform;
        axes = 0x20;
        break;
    case SDL_IFORCE_EFFECT_SPRING:
        waveform = SDL_IFORCE_WAVE_SPRING;
        axes = 0xC0;
        break;
    default:
        waveform = SDL_IFORCE_WAVE_DAMPER;
        axes = 0xC0;
        break;
    }
    data[0] = (uint8_t)index;
    data[1] = waveform;
    data[2] = (uint8_t)(axes | effect->trigger_button);
    data[3] = (uint8_t)(effect->length & 0xFF);
    data[4] = (uint8_t)(effect->length >> 8);
    data[5] = (uint8_t)(effect->direction >> 8);
    data[6] = (uint8_t)(effect->trigger_interval & 0xFF);
    data[7] = (uint8_t)(effect->trigger_interval >> 8);
    data[8] = (uint8_t)(slot->block[0].start & 0xFF);
    data[9] = (uint8_t)(slot->block[0].start >> 8);
    data[10] = (uint8_t)(slot->block[1].start & 0xFF);
    data[11] = (uint8_t)(slot->block[1].start >> 8);
    data[12] = (uint8_t)(effect->delay & 0xFF);
    data[13] = (uint8_t)(effect->delay >> 8);
    IForce_Push(commands, SDL_IFORCE_CMD_EFFECT, data, sizeof(data));
}

static bool IForce_ValidEffect(const SDL_IForceEffect *effect)
{
    /* The trigger has the core's low nibble */
    if (effect->trigger_button > 0x0F) {
        return false;
    }
    switch (effect->type) {
    case SDL_IFORCE_EFFECT_CONSTANT:
    case SDL_IFORCE_EFFECT_SPRING:
    case SDL_IFORCE_EFFECT_DAMPER:
        return true;
    case SDL_IFORCE_EFFECT_PERIODIC:
        return effect->waveform >= SDL_IFORCE_WAVE_SQUARE && effect->waveform <= SDL_IFORCE_WAVE_SAWTOOTH_DOWN;
    default:
        return false;
    }
}

/* Sends what changed from slot->effect to effect, the parameter blocks
 * before the core as Linux orders them */
static int IForce_Send(SDL_IForceSlot *slot, int index, const SDL_IForceEffect *effect, const bool write[2], bool core,
                       uint64_t now, SDL_IForceCommands *commands)
{
    int which;

    for (which = 0; which < 2; ++which) {
        if (write[which]) {
            IForce_WriteBlock(commands, slot, which, effect);
            slot->block[which].written = true;
            slot->block[which].written_at = now;
        }
    }
    if (core) {
        IForce_WriteCore(commands, index, slot, effect);
        if (slot->should_play) {
            uint8_t data[3];

            data[0] = (uint8_t)index;
            data[1] = 0x01;
            data[2] = 0x01;
            IForce_Push(commands, SDL_IFORCE_CMD_PLAY, data, sizeof(data));
        }
    }
    slot->effect = *effect;
    slot->used = true;
    return (write[0] || write[1] || core) ? SDL_IFORCE_UPLOAD_SENT : SDL_IFORCE_UPLOAD_UNCHANGED;
}

static bool IForce_BlocksReady(const SDL_IForceSlot *slot, const bool write[2], uint64_t now, uint64_t *ready_at)
{
    uint64_t latest = 0;
    int which;

    for (which = 0; which < 2; ++which) {
        const SDL_IForceBlock *block = &slot->block[which];

        if (write[which] && block->written && block->written_at + SDL_IFORCE_UPDATE_SPACING_MS > latest) {
            latest = block->written_at + SDL_IFORCE_UPDATE_SPACING_MS;
        }
    }
    if (ready_at) {
        *ready_at = latest;
    }
    return now >= latest;
}

int SDL_IForce_Upload(SDL_IForceFF *ff, int index, const SDL_IForceEffect *effect, uint64_t now,
                      SDL_IForceCommands *commands)
{
    SDL_IForceSlot *slot;
    bool write[2];
    bool core;

    if (!ff || !effect || !commands || index < 0 || index >= ff->effects || !IForce_ValidEffect(effect)) {
        return SDL_IFORCE_UPLOAD_INVALID;
    }
    commands->count = 0;
    slot = &ff->slots[index];
    if (slot->used && (slot->effect.type != effect->type ||
                       (effect->type == SDL_IFORCE_EFFECT_PERIODIC && slot->effect.waveform != effect->waveform))) {
        return SDL_IFORCE_UPLOAD_INVALID;
    }

    if (!slot->used) {
        /* Linux keeps a first block whose second one found no room. It is
           freed here, since the effect was never created. */
        if (!IForce_Allocate(ff, IForce_BlockSize(effect->type, 0), &slot->block[0])) {
            return SDL_IFORCE_UPLOAD_NO_MEMORY;
        }
        if (!IForce_Allocate(ff, IForce_BlockSize(effect->type, 1), &slot->block[1])) {
            memset(&slot->block[0], 0, sizeof(slot->block[0]));
            return SDL_IFORCE_UPLOAD_NO_MEMORY;
        }
    }

    IForce_Changes(slot, effect, write, &core);
    if (slot->used && !IForce_BlocksReady(slot, write, now, NULL)) {
        slot->pending = true;
        slot->next = *effect;
        return SDL_IFORCE_UPLOAD_DEFERRED;
    }
    slot->pending = false;
    return IForce_Send(slot, index, effect, write, core, now, commands);
}

bool SDL_IForce_NextDeferred(SDL_IForceFF *ff, uint64_t now, SDL_IForceCommands *commands)
{
    int i;

    if (!ff || !commands) {
        return false;
    }
    commands->count = 0;
    for (i = 0; i < ff->effects; ++i) {
        SDL_IForceSlot *slot = &ff->slots[i];
        bool write[2];
        bool core;

        if (!slot->pending) {
            continue;
        }
        IForce_Changes(slot, &slot->next, write, &core);
        if (!IForce_BlocksReady(slot, write, now, NULL)) {
            continue;
        }
        slot->pending = false;
        IForce_Send(slot, i, &slot->next, write, core, now, commands);
        return true;
    }
    return false;
}

bool SDL_IForce_DeferredDeadline(const SDL_IForceFF *ff, uint64_t *deadline)
{
    bool have = false;
    int i;

    if (!ff) {
        return false;
    }
    for (i = 0; i < ff->effects; ++i) {
        const SDL_IForceSlot *slot = &ff->slots[i];
        bool write[2];
        bool core;
        uint64_t ready_at;

        if (!slot->pending) {
            continue;
        }
        IForce_Changes(slot, &slot->next, write, &core);
        IForce_BlocksReady(slot, write, 0, &ready_at);
        if (!have || ready_at < *deadline) {
            *deadline = ready_at;
            have = true;
        }
    }
    return have;
}

void SDL_IForce_SetPlaying(SDL_IForceFF *ff, int index, bool playing)
{
    if (ff && index >= 0 && index < ff->effects) {
        ff->slots[index].should_play = playing;
    }
}

void SDL_IForce_Erase(SDL_IForceFF *ff, int index)
{
    if (ff && index >= 0 && index < ff->effects) {
        memset(&ff->slots[index], 0, sizeof(ff->slots[index]));
    }
}
