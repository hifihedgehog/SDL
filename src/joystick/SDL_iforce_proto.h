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

/* I-Force wheels and joysticks, hifihedgehog/SDL#33 Part 8: the models, the
 * input packets, the query sequence, the RS-232 framing and the force
 * feedback commands. Pure C99: no SDL runtime and no I/O, so every decision
 * here runs in the offline tests exactly as it runs in the library. The
 * HIDAPI driver, the haptic driver and the serial module share it.
 *
 * The protocol is Immersion's. Its facts are restated from Linux's iforce
 * driver and protocol document and from two Windows bridges that read the
 * devices. */

#ifndef SDL_iforce_proto_h_
#define SDL_iforce_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_IFORCE_MAX_LENGTH        16   /* The most data a packet carries, and a query reply's size */
#define SDL_IFORCE_EFFECTS_MAX       32   /* The most effects kept, whatever N reports */
#define SDL_IFORCE_OPEN_TRIES        20
#define SDL_IFORCE_QUERY_TIMEOUT_MS  1000
#define SDL_IFORCE_MEMORY_DEFAULT    200  /* The effect memory's end when B goes unanswered */
#define SDL_IFORCE_SERIAL_RATE       38400
#define SDL_IFORCE_SERIAL_START      0x2B
#define SDL_IFORCE_UPDATE_SPACING_MS 20   /* Between two writes of one parameter block */

/* Packet types from the device */
#define SDL_IFORCE_PACKET_JOYSTICK 0x01
#define SDL_IFORCE_PACKET_STATUS   0x02
#define SDL_IFORCE_PACKET_WHEEL    0x03
#define SDL_IFORCE_PACKET_QUERY    0xFF

/* Commands to the device */
#define SDL_IFORCE_CMD_EFFECT    0x01
#define SDL_IFORCE_CMD_ENVELOPE  0x02
#define SDL_IFORCE_CMD_MAGNITUDE 0x03
#define SDL_IFORCE_CMD_PERIOD    0x04
#define SDL_IFORCE_CMD_CONDITION 0x05
#define SDL_IFORCE_CMD_CONTROL   0x40
#define SDL_IFORCE_CMD_PLAY      0x41
#define SDL_IFORCE_CMD_STATE     0x42
#define SDL_IFORCE_CMD_GAIN      0x43
#define SDL_IFORCE_CMD_QUERY     0xFF

/* Command 42 bits */
#define SDL_IFORCE_STATE_STOP_ALL 0x01
#define SDL_IFORCE_STATE_ENABLE   0x04
#define SDL_IFORCE_STATE_PAUSE    0x08

/* A USB query is a vendor IN control request to the interface */
#define SDL_IFORCE_QUERY_REQUEST_TYPE 0xC1

/* Joystick properties the HIDAPI driver sets once N and B are known, and
 * the haptic driver reads: the effects from N, and B's memory end */
#define SDL_IFORCE_PROP_EFFECTS_NUMBER "SDL.joystick.iforce.effects"
#define SDL_IFORCE_PROP_MEMORY_NUMBER  "SDL.joystick.iforce.memory"

/* Hat bits, equal to SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN and SDL_HAT_LEFT */
#define SDL_IFORCE_HAT_UP    0x01
#define SDL_IFORCE_HAT_RIGHT 0x02
#define SDL_IFORCE_HAT_DOWN  0x04
#define SDL_IFORCE_HAT_LEFT  0x08

typedef enum SDL_IForceLayout
{
    SDL_IFORCE_LAYOUT_JOYSTICK,       /* Linux's 13-entry joystick table */
    SDL_IFORCE_LAYOUT_AVB,            /* The Pegasus's 9-entry table */
    SDL_IFORCE_LAYOUT_WHEEL,          /* The 8-entry wheel table */
    SDL_IFORCE_LAYOUT_GUILLEMOT_WHEEL /* 06F8:0004 as measured on the wheel */
} SDL_IForceLayout;

typedef struct SDL_IForceModel
{
    uint16_t vendor_id;
    uint16_t product_id;
    const char *name;
    uint8_t layout; /* SDL_IForceLayout */
    bool rudder;
    bool usb; /* A USB ID. The others are serial models known by M and P. */
} SDL_IForceModel;

/* The model with this ID, or NULL. usb_only leaves out the serial models. */
extern const SDL_IForceModel *SDL_IForce_FindModel(uint16_t vendor_id, uint16_t product_id, bool usb_only);

/* The layout Linux gives any other device: the joystick table without rudder */
extern const SDL_IForceModel *SDL_IForce_UnknownModel(void);

extern bool SDL_IForce_IsWheel(const SDL_IForceModel *model);

#define SDL_IFORCE_MAX_AXES    4
#define SDL_IFORCE_MAX_BUTTONS 13
#define SDL_IFORCE_MAX_HATS    2

/* Axis 0 is X or the wheel, 1 is Y or gas, 2 is the throttle or brake and
 * 3 the rudder. The last button is the grip sensor of the status packet. */
typedef struct SDL_IForceIdentity
{
    int naxes;
    int nbuttons;
    int nhats;
    bool wheel;
} SDL_IForceIdentity;

extern void SDL_IForce_GetIdentity(const SDL_IForceModel *model, SDL_IForceIdentity *identity);

typedef struct SDL_IForceState
{
    int16_t axes[SDL_IFORCE_MAX_AXES];
    uint16_t buttons; /* Bit i is button i */
    uint8_t hats[SDL_IFORCE_MAX_HATS];
} SDL_IForceState;

/* Before any packet: sticks, wheel and rudder centered, pedals and
 * throttle released, nothing pressed, hats centered */
extern void SDL_IForce_ResetState(const SDL_IForceModel *model, SDL_IForceState *state);

#define SDL_IFORCE_MAX_ADDRESSES ((SDL_IFORCE_MAX_LENGTH - 3) / 2)

typedef struct SDL_IForceStatus
{
    bool deadman;  /* A hand on the grip */
    uint8_t effect;
    bool playing;
    int addresses; /* Parameter blocks the device reports stored */
    uint16_t address[SDL_IFORCE_MAX_ADDRESSES];
} SDL_IForceStatus;

typedef enum SDL_IForcePacketKind
{
    SDL_IFORCE_PACKET_IGNORED,
    SDL_IFORCE_PACKET_INPUT,
    SDL_IFORCE_PACKET_STATUS_REPORT
} SDL_IForcePacketKind;

/* Decodes one packet: its type byte and the data after it. An input packet
 * updates state. A status packet sets the grip button in state and fills
 * status. Any other type, or a packet shorter than its minimum, changes
 * nothing. Returns an SDL_IForcePacketKind. */
extern int SDL_IForce_DecodePacket(const SDL_IForceModel *model, uint8_t type, const uint8_t *data, size_t length,
                                   SDL_IForceState *state, SDL_IForceStatus *status);

/* The query sequence: O until the device answers, at most 20 times, then
 * M, P, B, N, C, E, O and V, each asked once. */
typedef struct SDL_IForceQueries
{
    int step;
    int open_tries;
    bool done;
    bool open;          /* The device answered O */
    uint16_t vendor_id; /* From M, 0 when unanswered */
    uint16_t product_id;
    uint16_t memory_end;
    int effects;        /* From N, 0 when unanswered */
} SDL_IForceQueries;

extern void SDL_IForce_InitQueries(SDL_IForceQueries *queries);

/* The letter to ask next, or 0 once the sequence is over */
extern uint8_t SDL_IForce_NextQuery(const SDL_IForceQueries *queries);

/* The outcome of asking NextQuery's letter: the reply bytes, byte 0 the
 * letter. NULL or a length of 0 is a query that failed. */
extern void SDL_IForce_QueryResult(SDL_IForceQueries *queries, const uint8_t *reply, size_t length);

typedef struct SDL_IForceControlSetup
{
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} SDL_IForceControlSetup;

/* A USB query's control request */
extern void SDL_IForce_QuerySetup(uint8_t letter, SDL_IForceControlSetup *setup);

/* RS-232: 2B, the type, the data length, the data and the XOR of every
 * byte before it. The receiver follows Linux, which never compares the
 * XOR. This parser counts the frames whose XOR differs. */
typedef struct SDL_IForceSerialParser
{
    bool started;   /* A 2B came */
    uint8_t type;   /* 0 until the type byte */
    uint8_t length; /* 0 until the length byte */
    uint8_t count;
    uint8_t check;
    uint32_t mismatches;
    uint8_t data[SDL_IFORCE_MAX_LENGTH];
} SDL_IForceSerialParser;

typedef void (*SDL_IForceFrameHandler)(void *userdata, uint8_t type, const uint8_t *data, size_t length);

extern void SDL_IForce_InitSerialParser(SDL_IForceSerialParser *parser);
extern void SDL_IForce_FeedSerial(SDL_IForceSerialParser *parser, const uint8_t *bytes, size_t length,
                                  SDL_IForceFrameHandler handler, void *userdata);

/* A whole frame. Returns its length, or 0 when it does not fit. */
extern size_t SDL_IForce_BuildSerialFrame(uint8_t *out, size_t size, uint8_t command, const uint8_t *data, size_t length);

#define SDL_IFORCE_MAX_COMMAND 15 /* The command byte and 14 data bytes */

typedef struct SDL_IForceCommand
{
    uint8_t length;
    uint8_t bytes[SDL_IFORCE_MAX_COMMAND];
} SDL_IForceCommand;

/* Whether bytes are one whole command to the device: a command byte and
 * exactly its data length. Queries are not commands on USB. */
extern bool SDL_IForce_IsCommand(const uint8_t *bytes, size_t length);

/* 43: the 16-bit gain shifted right 9, as Linux sends it */
extern void SDL_IForce_BuildGain(SDL_IForceCommand *command, uint16_t gain);
/* 42: SDL_IFORCE_STATE_* bits */
extern void SDL_IForce_BuildState(SDL_IForceCommand *command, uint8_t state);
/* 41: 0 iterations stops the effect. More than 255 play 255 times. */
extern void SDL_IForce_BuildPlay(SDL_IForceCommand *command, uint8_t index, uint32_t iterations);
/* 40 03 with the 16-bit strength shifted right 9, then 40 04 01 */
extern void SDL_IForce_BuildAutocenter(SDL_IForceCommand commands[2], uint16_t strength);

/* Force feedback effects, in the units of Linux's force feedback API */
typedef enum SDL_IForceEffectType
{
    SDL_IFORCE_EFFECT_CONSTANT,
    SDL_IFORCE_EFFECT_PERIODIC,
    SDL_IFORCE_EFFECT_SPRING,
    SDL_IFORCE_EFFECT_DAMPER
} SDL_IForceEffectType;

/* Waveform bytes */
#define SDL_IFORCE_WAVE_CONSTANT      0x00
#define SDL_IFORCE_WAVE_SQUARE        0x20
#define SDL_IFORCE_WAVE_TRIANGLE      0x21
#define SDL_IFORCE_WAVE_SINE          0x22
#define SDL_IFORCE_WAVE_SAWTOOTH_UP   0x23
#define SDL_IFORCE_WAVE_SAWTOOTH_DOWN 0x24
#define SDL_IFORCE_WAVE_SPRING        0x40
#define SDL_IFORCE_WAVE_DAMPER        0x41

typedef struct SDL_IForceEnvelope
{
    uint16_t attack_length; /* ms */
    uint16_t attack_level;  /* 0 to 0x7FFF */
    uint16_t fade_length;
    uint16_t fade_level;
} SDL_IForceEnvelope;

typedef struct SDL_IForceCondition
{
    uint16_t right_saturation;
    uint16_t left_saturation;
    int16_t right_coeff;
    int16_t left_coeff;
    uint16_t deadband;
    int16_t center;
} SDL_IForceCondition;

typedef struct SDL_IForceEffect
{
    uint8_t type;             /* SDL_IForceEffectType */
    uint8_t waveform;         /* Periodic: SDL_IFORCE_WAVE_SQUARE to _SAWTOOTH_DOWN */
    uint16_t direction;       /* 0x4000 is 90 degrees */
    uint16_t length;          /* ms, 0 for no end */
    uint16_t delay;           /* ms */
    uint8_t trigger_button;   /* 0 for none, else the device's button number plus 1, 15 at most */
    uint16_t trigger_interval;
    int16_t level;            /* Constant */
    int16_t magnitude;        /* Periodic */
    int16_t offset;
    uint16_t period;          /* ms */
    uint16_t phase;           /* 0x10000 is 360 degrees */
    SDL_IForceEnvelope envelope;       /* Constant and periodic */
    SDL_IForceCondition condition[2];  /* Spring and damper: X, then Y */
} SDL_IForceEffect;

typedef struct SDL_IForceBlock
{
    bool used;
    uint16_t start;
    uint16_t size;
    bool written;
    uint64_t written_at;
} SDL_IForceBlock;

typedef struct SDL_IForceSlot
{
    bool used;
    bool should_play;
    bool pending;           /* An update waits for its blocks' spacing */
    SDL_IForceEffect effect; /* As last sent */
    SDL_IForceEffect next;   /* The update that waits */
    SDL_IForceBlock block[2];
} SDL_IForceSlot;

typedef struct SDL_IForceFF
{
    int effects;
    uint16_t memory_end; /* Blocks go between 0 and this address, both included */
    SDL_IForceSlot slots[SDL_IFORCE_EFFECTS_MAX];
} SDL_IForceFF;

#define SDL_IFORCE_MAX_UPLOAD 4 /* Two parameter blocks, the core, and a play */

typedef struct SDL_IForceCommands
{
    int count;
    SDL_IForceCommand command[SDL_IFORCE_MAX_UPLOAD];
} SDL_IForceCommands;

#define SDL_IFORCE_UPLOAD_SENT       0
#define SDL_IFORCE_UPLOAD_UNCHANGED  1
#define SDL_IFORCE_UPLOAD_DEFERRED   2
#define SDL_IFORCE_UPLOAD_INVALID    (-1)
#define SDL_IFORCE_UPLOAD_NO_MEMORY  (-2)

/* effects from N, memory_end from B */
extern void SDL_IForce_InitFF(SDL_IForceFF *ff, int effects, uint16_t memory_end);

/* Uploads effect index, or updates it with only the parts that changed. An
 * update that would write a block less than 20 ms after its last write waits,
 * and a later update replaces it. An update that goes at once drops a waiting
 * one. An update may not change the type or the waveform, as Linux's
 * input_ff_upload refuses it. Returns SDL_IFORCE_UPLOAD_*. */
extern int SDL_IForce_Upload(SDL_IForceFF *ff, int index, const SDL_IForceEffect *effect, uint64_t now,
                             SDL_IForceCommands *commands);

/* One waiting update whose blocks may be written now, as commands. false
 * when none can go. */
extern bool SDL_IForce_NextDeferred(SDL_IForceFF *ff, uint64_t now, SDL_IForceCommands *commands);

/* When the first waiting update can go. false when none waits. */
extern bool SDL_IForce_DeferredDeadline(const SDL_IForceFF *ff, uint64_t *deadline);

/* Records whether the application wants the effect playing, which a core
 * upload replays */
extern void SDL_IForce_SetPlaying(SDL_IForceFF *ff, int index, bool playing);

/* Frees the effect's blocks */
extern void SDL_IForce_Erase(SDL_IForceFF *ff, int index);

#endif /* SDL_iforce_proto_h_ */
