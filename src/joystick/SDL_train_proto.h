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

/* Train controllers, hifihedgehog/SDL#33 Part 10. Pure C99: no SDL
 * runtime and no I/O, so every decision here runs in the offline tests
 * exactly as it runs in the library. The HIDAPI driver and the serial
 * module share it.
 *
 * USB: Taito's two-handle Densha de GO! controllers for the PlayStation 2
 * (Type 2, Shinkansen, Ryojohen) and the one-lever Multi Train Controller
 * and Train Mascon. Each has one interface with one interrupt IN endpoint,
 * and every output is a vendor control transfer. RS-232: Pony Canyon's
 * Master Controllers, which send five ASCII characters and CR per event.
 *
 * A handle goes to one axis with its notches spread evenly. A two-handle
 * controller puts the brake on the left trigger and the power on the right
 * one, and a one-lever controller puts the lever on the left stick's Y and
 * the reverser on the right stick's Y. The facts are restated from the
 * Train Controller Database, OpenBVE's decoders and the emulations that
 * follow them. No code from them is copied.
 */

#ifndef SDL_train_proto_h_
#define SDL_train_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Hat bits, equal to SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN and SDL_HAT_LEFT */
#define SDL_TRAIN_HAT_UP    0x01
#define SDL_TRAIN_HAT_RIGHT 0x02
#define SDL_TRAIN_HAT_DOWN  0x04
#define SDL_TRAIN_HAT_LEFT  0x08

typedef enum SDL_TrainModel
{
    SDL_TRAIN_NONE,
    SDL_TRAIN_TYPE2,      /* 0AE4:0004 */
    SDL_TRAIN_MTC_P5B8,   /* 0AE4:0004 with bcdDevice 0100: a Type 2 */
    SDL_TRAIN_SHINKANSEN, /* 0AE4:0005 */
    SDL_TRAIN_RYOJOHEN,   /* 0AE4:0007 */
    SDL_TRAIN_MTC_P4B7,   /* 0AE4:0101 with bcdDevice 0300 */
    SDL_TRAIN_MTC_P4B2B7, /* 0400 */
    SDL_TRAIN_MTC_P5B7,   /* 0800 */
    SDL_TRAIN_MTC_P13B7,  /* 0A00 */
    SDL_TRAIN_MASCON,     /* 1C06:77A7, the Train Mascon and the P5/B5 cartridge */
    SDL_TRAIN_MASTER      /* The RS-232 Master Controllers */
} SDL_TrainModel;

/* The USB model with this identity, or SDL_TRAIN_NONE */
extern int SDL_Train_Identify(uint16_t vendor, uint16_t product, uint16_t version);
extern const char *SDL_Train_Name(int model);
/* Type 2, P5/B8, Shinkansen and Ryojohen */
extern bool SDL_Train_IsTwoHandle(int model);

/* Two-handle: axes 4 brake and 5 power, buttons 0 to 11, hat 0.
 * One lever: axes 1 lever and 3 reverser, buttons 0 to 12, hat 0.
 * Master Controller: axes 1 and 3, buttons 0 to 3, no hat. */
typedef struct SDL_TrainIdentity
{
    int naxes;
    int nbuttons;
    int nhats;
    int report_length; /* The input report's length, 0 for RS-232 */
} SDL_TrainIdentity;

extern bool SDL_Train_GetIdentity(int model, SDL_TrainIdentity *identity);

/* The gamepad mapping, after "none,*,", or NULL */
extern const char *SDL_Train_GetMapping(int model);

#define SDL_TRAIN_MAX_AXES 6

typedef struct SDL_TrainState
{
    int16_t axes[SDL_TRAIN_MAX_AXES];
    uint16_t buttons; /* Bit i is button i */
    uint8_t hat;      /* SDL hat bits */
} SDL_TrainState;

/* At rest: two-handle triggers at -32768, a lever at 0 with the reverser at
 * -32768 (Neutral), nothing pressed, the hat centered */
extern void SDL_Train_ResetState(int model, SDL_TrainState *state);

/* Decodes one USB read. A read shorter than the model's report, or one whose
 * fixed first byte is not 01, changes nothing and returns false. Bytes past
 * the report are ignored. A handle value in no table keeps its axis. */
extern bool SDL_Train_Decode(int model, const uint8_t *report, size_t length, SDL_TrainState *state);

/* A vendor control OUT transfer on endpoint 0 */
typedef struct SDL_TrainControl
{
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
    uint8_t data[8];
} SDL_TrainControl;

/* The outputs last sent, so a repeated request sends nothing */
typedef struct SDL_TrainOutputs
{
    bool left_motor;
    bool right_motor;
    uint8_t shinkansen[8]; /* The Shinkansen's display payload, motors in bytes 0 and 1 */
} SDL_TrainOutputs;

extern void SDL_Train_InitOutputs(SDL_TrainOutputs *outputs);

/* Rumble on the Type 2 and the Shinkansen: a nonzero low-frequency value
 * turns the left motor on and a nonzero high-frequency value the right one.
 * Fills up to two transfers for what changed and returns their count, or -1
 * on a model without motors. */
extern int SDL_Train_Rumble(int model, SDL_TrainOutputs *outputs, uint16_t low, uint16_t high, SDL_TrainControl out[2]);

/* A raw output: 2 bytes (status, function) on the Type 2, the 8-byte display
 * payload on the Shinkansen, one lamp byte on the Multi Train Controller and
 * the Train Mascon. Fills one transfer and returns 1, or returns -1. */
extern int SDL_Train_Effect(int model, SDL_TrainOutputs *outputs, const uint8_t *data, size_t size, SDL_TrainControl *out);

/* The off states for a close. Fills up to three transfers and returns their
 * count. */
extern int SDL_Train_Close(int model, SDL_TrainOutputs *outputs, SDL_TrainControl out[3]);

/* The Master Controllers' ASCII events: a word of five characters, then CR.
 * A word longer than five characters is dropped whole, as an exact
 * comparison drops it, and never holds more than five bytes. */
#define SDL_TRAIN_WORD_LENGTH 5

typedef struct SDL_TrainLineParser
{
    uint8_t word[SDL_TRAIN_WORD_LENGTH];
    int count;
    bool overflow; /* The current word is past five bytes */
} SDL_TrainLineParser;

extern void SDL_Train_InitLine(SDL_TrainLineParser *parser);

/* Applies each complete event in the bytes to state. Returns true when an
 * event was recognized. */
extern bool SDL_Train_FeedLine(SDL_TrainLineParser *parser, const uint8_t *bytes, size_t length, SDL_TrainState *state);

#endif /* SDL_train_proto_h_ */
