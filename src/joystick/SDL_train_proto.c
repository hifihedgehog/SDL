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

/* Train controllers. See SDL_train_proto.h. */

#include "SDL_train_proto.h"
#include "usb_ids.h"

#include <string.h>

int SDL_Train_Identify(uint16_t vendor, uint16_t product, uint16_t version)
{
    if (vendor == USB_VENDOR_TAITO) {
        switch (product) {
        case USB_PRODUCT_TAITO_DENSHA_TYPE2:
            /* The Multi Train Controller's P5/B8 cartridge presents a Type 2 */
            return (version == 0x0100) ? SDL_TRAIN_MTC_P5B8 : SDL_TRAIN_TYPE2;
        case USB_PRODUCT_TAITO_DENSHA_SHINKANSEN:
            return SDL_TRAIN_SHINKANSEN;
        case USB_PRODUCT_TAITO_DENSHA_RYOJOHEN:
            return SDL_TRAIN_RYOJOHEN;
        case USB_PRODUCT_TAITO_MULTI_TRAIN_CONTROLLER:
            /* The cartridge sets bcdDevice */
            switch (version) {
            case 0x0300:
                return SDL_TRAIN_MTC_P4B7;
            case 0x0400:
                return SDL_TRAIN_MTC_P4B2B7;
            case 0x0800:
                return SDL_TRAIN_MTC_P5B7;
            case 0x0A00:
                return SDL_TRAIN_MTC_P13B7;
            default:
                return SDL_TRAIN_NONE;
            }
        default:
            return SDL_TRAIN_NONE;
        }
    }
    if (vendor == USB_VENDOR_TRAIN_MASCON && product == USB_PRODUCT_TRAIN_MASCON) {
        return SDL_TRAIN_MASCON;
    }
    return SDL_TRAIN_NONE;
}

const char *SDL_Train_Name(int model)
{
    switch (model) {
    case SDL_TRAIN_TYPE2:
        return "Taito Densha de GO! Type 2 Controller";
    case SDL_TRAIN_MTC_P5B8:
        return "Multi Train Controller (P5/B8)";
    case SDL_TRAIN_SHINKANSEN:
        return "Taito Densha de GO! Shinkansen Controller";
    case SDL_TRAIN_RYOJOHEN:
        return "Taito Densha de GO! Ryojohen Controller";
    case SDL_TRAIN_MTC_P4B7:
        return "Multi Train Controller (P4/B7)";
    case SDL_TRAIN_MTC_P4B2B7:
        return "Multi Train Controller (P4/B2-B7)";
    case SDL_TRAIN_MTC_P5B7:
        return "Multi Train Controller (P5/B7)";
    case SDL_TRAIN_MTC_P13B7:
        return "Multi Train Controller (P13/B7)";
    case SDL_TRAIN_MASCON:
        return "Train Mascon";
    case SDL_TRAIN_MASTER:
        return "Pony Canyon Master Controller";
    default:
        return NULL;
    }
}

bool SDL_Train_IsTwoHandle(int model)
{
    return model == SDL_TRAIN_TYPE2 || model == SDL_TRAIN_MTC_P5B8 || model == SDL_TRAIN_SHINKANSEN ||
           model == SDL_TRAIN_RYOJOHEN;
}

static bool Train_IsLever(int model)
{
    return model >= SDL_TRAIN_MTC_P4B7 && model <= SDL_TRAIN_MASCON;
}

bool SDL_Train_GetIdentity(int model, SDL_TrainIdentity *identity)
{
    memset(identity, 0, sizeof(*identity));
    if (SDL_Train_IsTwoHandle(model)) {
        identity->naxes = 6;
        identity->nbuttons = 12;
        identity->nhats = 1;
        identity->report_length = (model == SDL_TRAIN_RYOJOHEN) ? 8 : 6;
        return true;
    }
    if (Train_IsLever(model)) {
        identity->naxes = 4;
        identity->nbuttons = 13;
        identity->nhats = 1;
        identity->report_length = 4;
        return true;
    }
    if (model == SDL_TRAIN_MASTER) {
        identity->naxes = 4;
        identity->nbuttons = 4;
        return true;
    }
    return false;
}

const char *SDL_Train_GetMapping(int model)
{
    if (SDL_Train_IsTwoHandle(model)) {
        return "a:b0,b:b1,x:b2,y:b3,back:b4,start:b6,leftshoulder:b9,rightshoulder:b10,misc1:b11,"
               "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,lefttrigger:a4,righttrigger:a5,";
    }
    if (Train_IsLever(model)) {
        return "a:b0,b:b1,x:b2,y:b3,back:b4,start:b6,misc1:b11,misc2:b12,"
               "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,lefty:a1,righty:a3,";
    }
    return NULL;
}

#define TRAIN_REVERSER_NEUTRAL  (-32768)
#define TRAIN_REVERSER_FORWARD  128
#define TRAIN_REVERSER_BACKWARD 32767

void SDL_Train_ResetState(int model, SDL_TrainState *state)
{
    memset(state, 0, sizeof(*state));
    if (SDL_Train_IsTwoHandle(model)) {
        state->axes[4] = -32768;
        state->axes[5] = -32768;
    } else {
        state->axes[3] = TRAIN_REVERSER_NEUTRAL;
    }
}

/* Step s of a two-handle handle with S steps, rest at 0 */
static int16_t Train_Trigger(int step, int steps)
{
    return (int16_t)(-32768 + (step * 65535 + steps / 2) / steps);
}

/* One lever: brake b of Bt reads below 0 and power p of P above it */
static int16_t Train_Brake(int brake, int brake_steps)
{
    return (int16_t)-((brake * 32768 + brake_steps / 2) / brake_steps);
}

static int16_t Train_Power(int power, int power_steps)
{
    return (int16_t)((power * 32767 + power_steps / 2) / power_steps);
}

/* The byte of each step, rest first */
static const uint8_t train_type2_brake[] = { 0x79, 0x8A, 0x94, 0x9A, 0xA2, 0xA8, 0xAF, 0xB2, 0xB5, 0xB9 };
static const uint8_t train_type2_power[] = { 0x81, 0x6D, 0x54, 0x3F, 0x21, 0x00 };
static const uint8_t train_shinkansen_brake[] = { 0x1C, 0x38, 0x54, 0x70, 0x8B, 0xA7, 0xC3, 0xDF, 0xFB };
static const uint8_t train_shinkansen_power[] = { 0x12, 0x24, 0x36, 0x48, 0x5A, 0x6C, 0x7E, 0x90, 0xA2, 0xB4, 0xC6, 0xD7, 0xE9, 0xFB };
static const uint8_t train_ryojohen_power[] = { 0x00, 0x3C, 0x78, 0xB4, 0xF0 };

/* A handle byte that matches no step keeps the axis */
static void Train_Notch(const uint8_t *table, int count, uint8_t value, int16_t *axis)
{
    int i;

    for (i = 0; i < count; ++i) {
        if (table[i] == value) {
            *axis = Train_Trigger(i, count - 1);
            return;
        }
    }
}

/* The Ryojohen's analog brake: 23 to D7 across the trigger, D8 to DF at the
 * top, anything else kept */
static void Train_RyojohenBrake(uint8_t value, int16_t *axis)
{
    if (value >= 0x23 && value <= 0xD7) {
        *axis = (int16_t)(-32768 + ((value - 0x23) * 65535 + 90) / 180);
    } else if (value >= 0xD8 && value <= 0xDF) {
        *axis = 32767;
    }
}

/* The D-pad byte: 0 up, clockwise to 7 up-left, anything else none */
static uint8_t Train_Hat(uint8_t value)
{
    static const uint8_t hats[8] = {
        SDL_TRAIN_HAT_UP,
        SDL_TRAIN_HAT_UP | SDL_TRAIN_HAT_RIGHT,
        SDL_TRAIN_HAT_RIGHT,
        SDL_TRAIN_HAT_DOWN | SDL_TRAIN_HAT_RIGHT,
        SDL_TRAIN_HAT_DOWN,
        SDL_TRAIN_HAT_DOWN | SDL_TRAIN_HAT_LEFT,
        SDL_TRAIN_HAT_LEFT,
        SDL_TRAIN_HAT_UP | SDL_TRAIN_HAT_LEFT
    };

    return (value < 8) ? hats[value] : 0;
}

/* The SDL button of each bit of the buttons byte, -1 for none. A goes to
 * West, B to South, C to East and D to North. */
static const int8_t train_type2_buttons[8] = { 0, 2, 1, 3, 4, 6, -1, -1 };
static const int8_t train_shinkansen_buttons[8] = { 3, 1, 0, 2, 4, 6, -1, -1 };
/* Horn, Announce, Camera, right doors, left doors, Select, Start */
static const int8_t train_ryojohen_buttons[8] = { 1, 0, 2, 10, 9, 4, 6, -1 };

/* Misc1 holds the horn pedal or ATS, and Misc2 the hard press of A */
#define TRAIN_BUTTON_MISC1 11
#define TRAIN_BUTTON_MISC2 12

static uint16_t Train_Buttons(const int8_t map[8], uint8_t bits)
{
    uint16_t buttons = 0;
    int i;

    for (i = 0; i < 8; ++i) {
        if ((bits & (1u << i)) && map[i] >= 0) {
            buttons = (uint16_t)(buttons | (1u << map[i]));
        }
    }
    return buttons;
}

static bool Train_DecodeTwoHandle(int model, const uint8_t *report, size_t length, SDL_TrainState *state)
{
    const uint8_t *data = report;
    const int8_t *map;

    switch (model) {
    case SDL_TRAIN_TYPE2:
    case SDL_TRAIN_MTC_P5B8:
        if (length < 6 || report[0] != 0x01) {
            return false;
        }
        /* Byte 0 is fixed, and the rest sit one byte later */
        data = report + 1;
        Train_Notch(train_type2_brake, (int)sizeof(train_type2_brake), data[0], &state->axes[4]);
        Train_Notch(train_type2_power, (int)sizeof(train_type2_power), data[1], &state->axes[5]);
        map = train_type2_buttons;
        break;
    case SDL_TRAIN_SHINKANSEN:
        if (length < 6) {
            return false;
        }
        Train_Notch(train_shinkansen_brake, (int)sizeof(train_shinkansen_brake), data[0], &state->axes[4]);
        Train_Notch(train_shinkansen_power, (int)sizeof(train_shinkansen_power), data[1], &state->axes[5]);
        map = train_shinkansen_buttons;
        break;
    case SDL_TRAIN_RYOJOHEN:
        if (length < 8) {
            return false;
        }
        Train_RyojohenBrake(data[0], &state->axes[4]);
        Train_Notch(train_ryojohen_power, (int)sizeof(train_ryojohen_power), data[1], &state->axes[5]);
        map = train_ryojohen_buttons;
        break;
    default:
        return false;
    }
    /* Then the horn pedal, the D-pad and the buttons */
    state->buttons = Train_Buttons(map, data[4]);
    if (data[2] == 0x00) {
        state->buttons = (uint16_t)(state->buttons | (1u << TRAIN_BUTTON_MISC1));
    }
    state->hat = Train_Hat(data[3]);
    return true;
}

/* A lever's steps: emergency and the brake notches, then the power notches */
typedef struct Train_Lever
{
    int brake_steps; /* The service notches and emergency */
    int power_steps;
    bool whole_byte; /* The handle fills the byte, and there is no reverser */
} Train_Lever;

static Train_Lever Train_LeverOf(int model)
{
    Train_Lever lever;

    lever.whole_byte = false;
    switch (model) {
    case SDL_TRAIN_MTC_P4B7:
        lever.brake_steps = 8;
        lever.power_steps = 4;
        break;
    case SDL_TRAIN_MTC_P4B2B7:
        lever.brake_steps = 7;
        lever.power_steps = 4;
        break;
    case SDL_TRAIN_MTC_P5B7:
        lever.brake_steps = 8;
        lever.power_steps = 5;
        break;
    case SDL_TRAIN_MTC_P13B7:
        lever.brake_steps = 8;
        lever.power_steps = 13;
        lever.whole_byte = true;
        break;
    default:
        lever.brake_steps = 6;
        lever.power_steps = 5;
        break;
    }
    return lever;
}

static bool Train_DecodeLever(int model, const uint8_t *report, size_t length, SDL_TrainState *state)
{
    const Train_Lever lever = Train_LeverOf(model);
    int index;
    uint8_t hat = 0;
    uint16_t buttons;

    if (length < 4 || report[0] != 0x01) {
        return false;
    }
    index = report[1];
    if (!lever.whole_byte) {
        /* The reverser: 2 or 8 Forward, 1 or 4 Backward, anything else Neutral */
        switch (index >> 4) {
        case 2:
        case 8:
            state->axes[3] = TRAIN_REVERSER_FORWARD;
            break;
        case 1:
        case 4:
            state->axes[3] = TRAIN_REVERSER_BACKWARD;
            break;
        default:
            state->axes[3] = TRAIN_REVERSER_NEUTRAL;
            break;
        }
        index &= 0x0F;
    }
    /* 1 is emergency, then the brake notches from highest to lowest, N,
       and the power notches from lowest to highest */
    if (index >= 1 && index <= lever.brake_steps) {
        state->axes[1] = Train_Brake(lever.brake_steps + 1 - index, lever.brake_steps);
    } else if (index == lever.brake_steps + 1) {
        state->axes[1] = 0;
    } else if (index > lever.brake_steps + 1 && index <= lever.brake_steps + 1 + lever.power_steps) {
        state->axes[1] = Train_Power(index - lever.brake_steps - 1, lever.power_steps);
    }

    /* Byte 2: ATS, D, A soft, A hard, B, C. Byte 3: Start, Select, then the
       D-pad bits as reported */
    buttons = 0;
    if (report[2] & 0x01) {
        buttons |= 1u << TRAIN_BUTTON_MISC1; /* ATS */
    }
    if (report[2] & 0x02) {
        buttons |= 1u << 3;
    }
    if (report[2] & 0x04) {
        buttons |= 1u << 2;
    }
    if (report[2] & 0x08) {
        buttons |= 1u << TRAIN_BUTTON_MISC2; /* A, hard press */
    }
    if (report[2] & 0x10) {
        buttons |= 1u << 0;
    }
    if (report[2] & 0x20) {
        buttons |= 1u << 1;
    }
    if (report[3] & 0x01) {
        buttons |= 1u << 6;
    }
    if (report[3] & 0x02) {
        buttons |= 1u << 4;
    }
    if (report[3] & 0x04) {
        hat |= SDL_TRAIN_HAT_UP;
    }
    if (report[3] & 0x08) {
        hat |= SDL_TRAIN_HAT_DOWN;
    }
    if (report[3] & 0x10) {
        hat |= SDL_TRAIN_HAT_LEFT;
    }
    if (report[3] & 0x20) {
        hat |= SDL_TRAIN_HAT_RIGHT;
    }
    state->buttons = buttons;
    state->hat = hat;
    return true;
}

bool SDL_Train_Decode(int model, const uint8_t *report, size_t length, SDL_TrainState *state)
{
    if (!report || !state || length == 0) {
        return false;
    }
    if (SDL_Train_IsTwoHandle(model)) {
        return Train_DecodeTwoHandle(model, report, length, state);
    }
    if (Train_IsLever(model)) {
        return Train_DecodeLever(model, report, length, state);
    }
    return false;
}

void SDL_Train_InitOutputs(SDL_TrainOutputs *outputs)
{
    memset(outputs, 0, sizeof(*outputs));
    /* FF FF in the speedometer and the limit blanks them */
    memset(outputs->shinkansen + 4, 0xFF, 4);
}

static void Train_SetControl(SDL_TrainControl *out, uint8_t request_type, uint8_t request, uint16_t value,
                             const uint8_t *data, uint16_t length)
{
    memset(out, 0, sizeof(*out));
    out->request_type = request_type;
    out->request = request;
    out->value = value;
    out->index = 0;
    out->length = length;
    if (length) {
        memcpy(out->data, data, length);
    }
}

/* Type 2: 41 09 0201, status then function */
static void Train_Type2Output(SDL_TrainControl *out, uint8_t status, uint8_t function)
{
    const uint8_t data[2] = { status, function };

    Train_SetControl(out, 0x41, 0x09, 0x0201, data, 2);
}

/* Shinkansen: 40 09 0301, the 8-byte payload */
static void Train_ShinkansenOutput(SDL_TrainControl *out, const uint8_t payload[8])
{
    Train_SetControl(out, 0x40, 0x09, 0x0301, payload, 8);
}

int SDL_Train_Rumble(int model, SDL_TrainOutputs *outputs, uint16_t low, uint16_t high, SDL_TrainControl out[2])
{
    const bool left = (low != 0);
    const bool right = (high != 0);
    int count = 0;

    switch (model) {
    case SDL_TRAIN_TYPE2:
        if (left != outputs->left_motor) {
            Train_Type2Output(&out[count++], left ? 0x01 : 0x00, 0x01);
            outputs->left_motor = left;
        }
        if (right != outputs->right_motor) {
            Train_Type2Output(&out[count++], right ? 0x01 : 0x00, 0x02);
            outputs->right_motor = right;
        }
        return count;
    case SDL_TRAIN_SHINKANSEN:
        if (left != outputs->left_motor || right != outputs->right_motor) {
            outputs->left_motor = left;
            outputs->right_motor = right;
            outputs->shinkansen[0] = left ? 0x01 : 0x00;
            outputs->shinkansen[1] = right ? 0x01 : 0x00;
            Train_ShinkansenOutput(&out[count++], outputs->shinkansen);
        }
        return count;
    default:
        return -1;
    }
}

int SDL_Train_Effect(int model, SDL_TrainOutputs *outputs, const uint8_t *data, size_t size, SDL_TrainControl *out)
{
    if (!data) {
        return -1;
    }
    switch (model) {
    case SDL_TRAIN_TYPE2:
    case SDL_TRAIN_MTC_P5B8:
        if (size != 2) {
            return -1;
        }
        Train_Type2Output(out, data[0], data[1]);
        if (data[1] == 0x01) {
            outputs->left_motor = (data[0] != 0);
        } else if (data[1] == 0x02) {
            outputs->right_motor = (data[0] != 0);
        }
        return 1;
    case SDL_TRAIN_SHINKANSEN:
        if (size != 8) {
            return -1;
        }
        memcpy(outputs->shinkansen, data, 8);
        outputs->left_motor = (data[0] != 0);
        outputs->right_motor = (data[1] != 0);
        Train_ShinkansenOutput(out, outputs->shinkansen);
        return 1;
    case SDL_TRAIN_MTC_P4B7:
    case SDL_TRAIN_MTC_P4B2B7:
    case SDL_TRAIN_MTC_P5B7:
    case SDL_TRAIN_MTC_P13B7:
    case SDL_TRAIN_MASCON:
        if (size != 1) {
            return -1;
        }
        /* 40 50 with the lamps in wValue and no data stage */
        Train_SetControl(out, 0x40, 0x50, data[0], NULL, 0);
        return 1;
    default:
        return -1;
    }
}

int SDL_Train_Close(int model, SDL_TrainOutputs *outputs, SDL_TrainControl out[3])
{
    switch (model) {
    case SDL_TRAIN_TYPE2:
    case SDL_TRAIN_MTC_P5B8:
        Train_Type2Output(&out[0], 0x00, 0x01);
        Train_Type2Output(&out[1], 0x00, 0x02);
        Train_Type2Output(&out[2], 0x00, 0x03);
        SDL_Train_InitOutputs(outputs);
        return 3;
    case SDL_TRAIN_SHINKANSEN:
        SDL_Train_InitOutputs(outputs);
        Train_ShinkansenOutput(&out[0], outputs->shinkansen);
        return 1;
    case SDL_TRAIN_MTC_P4B7:
    case SDL_TRAIN_MTC_P4B2B7:
    case SDL_TRAIN_MTC_P5B7:
    case SDL_TRAIN_MTC_P13B7:
    case SDL_TRAIN_MASCON:
        Train_SetControl(&out[0], 0x40, 0x50, 0x0000, NULL, 0);
        return 1;
    default:
        return 0;
    }
}

void SDL_Train_InitLine(SDL_TrainLineParser *parser)
{
    memset(parser, 0, sizeof(*parser));
}

typedef enum Train_WordKind
{
    TRAIN_WORD_LEVER,
    TRAIN_WORD_REVERSER,
    TRAIN_WORD_PRESS,
    TRAIN_WORD_RELEASE
} Train_WordKind;

/* Lever codes carry the step, brake below 0 and power above. Button codes
 * carry the SDL button: S North, A West, B South, C East. */
static const struct
{
    char code[SDL_TRAIN_WORD_LENGTH + 1];
    uint8_t kind;
    int8_t value;
} train_words[] = {
    { "TSB20", TRAIN_WORD_LEVER, -9 }, /* Emergency */
    { "TSB30", TRAIN_WORD_LEVER, -8 },
    { "TSB40", TRAIN_WORD_LEVER, -7 },
    { "TSE99", TRAIN_WORD_LEVER, -6 },
    { "TSA05", TRAIN_WORD_LEVER, -5 },
    { "TSA15", TRAIN_WORD_LEVER, -4 },
    { "TSA25", TRAIN_WORD_LEVER, -3 },
    { "TSA35", TRAIN_WORD_LEVER, -2 },
    { "TSA45", TRAIN_WORD_LEVER, -1 },
    { "TSA50", TRAIN_WORD_LEVER, 0 },
    { "TSA55", TRAIN_WORD_LEVER, 1 },
    { "TSA65", TRAIN_WORD_LEVER, 2 },
    { "TSA75", TRAIN_WORD_LEVER, 3 },
    { "TSA85", TRAIN_WORD_LEVER, 4 },
    { "TSA95", TRAIN_WORD_LEVER, 5 },
    { "TSB60", TRAIN_WORD_LEVER, 6 },
    { "TSB70", TRAIN_WORD_LEVER, 7 },
    { "TSB80", TRAIN_WORD_LEVER, 8 },
    { "TSG99", TRAIN_WORD_REVERSER, 1 },
    { "TSG50", TRAIN_WORD_REVERSER, 0 },
    { "TSG00", TRAIN_WORD_REVERSER, -1 },
    { "TSK99", TRAIN_WORD_PRESS, 3 },
    { "TSK00", TRAIN_WORD_RELEASE, 3 },
    { "TSX99", TRAIN_WORD_PRESS, 2 },
    { "TSX00", TRAIN_WORD_RELEASE, 2 },
    { "TSY99", TRAIN_WORD_PRESS, 0 },
    { "TSY00", TRAIN_WORD_RELEASE, 0 },
    { "TSZ99", TRAIN_WORD_PRESS, 1 },
    { "TSZ00", TRAIN_WORD_RELEASE, 1 },
};

/* The codes' fixed scale: 9 brake steps with emergency, 8 power steps */
#define TRAIN_MASTER_BRAKE_STEPS 9
#define TRAIN_MASTER_POWER_STEPS 8

static bool Train_ApplyWord(const uint8_t *word, SDL_TrainState *state)
{
    size_t i;

    for (i = 0; i < sizeof(train_words) / sizeof(train_words[0]); ++i) {
        const int value = train_words[i].value;

        if (memcmp(word, train_words[i].code, SDL_TRAIN_WORD_LENGTH) != 0) {
            continue;
        }
        switch (train_words[i].kind) {
        case TRAIN_WORD_LEVER:
            if (value < 0) {
                state->axes[1] = Train_Brake(-value, TRAIN_MASTER_BRAKE_STEPS);
            } else if (value > 0) {
                state->axes[1] = Train_Power(value, TRAIN_MASTER_POWER_STEPS);
            } else {
                state->axes[1] = 0;
            }
            break;
        case TRAIN_WORD_REVERSER:
            state->axes[3] = (int16_t)((value > 0) ? TRAIN_REVERSER_FORWARD : (value < 0) ? TRAIN_REVERSER_BACKWARD : TRAIN_REVERSER_NEUTRAL);
            break;
        case TRAIN_WORD_PRESS:
            state->buttons = (uint16_t)(state->buttons | (1u << value));
            break;
        default:
            state->buttons = (uint16_t)(state->buttons & ~(1u << value));
            break;
        }
        return true;
    }
    return false;
}

bool SDL_Train_FeedLine(SDL_TrainLineParser *parser, const uint8_t *bytes, size_t length, SDL_TrainState *state)
{
    bool recognized = false;
    size_t i;

    if (!bytes || !state) {
        return false;
    }
    for (i = 0; i < length; ++i) {
        const uint8_t byte = bytes[i];

        if (byte == 0x0D) {
            if (!parser->overflow && parser->count == SDL_TRAIN_WORD_LENGTH && Train_ApplyWord(parser->word, state)) {
                recognized = true;
            }
            parser->count = 0;
            parser->overflow = false;
        } else if (!parser->overflow) {
            if (parser->count == SDL_TRAIN_WORD_LENGTH) {
                /* Past five bytes the word cannot match */
                parser->overflow = true;
                parser->count = 0;
            } else {
                parser->word[parser->count++] = byte;
            }
        }
    }
    return recognized;
}
