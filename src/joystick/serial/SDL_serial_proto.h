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

/* The module contract of the serial joystick driver. A module turns one
 * device's byte stream into controls. Pure C99: no SDL runtime and no I/O.
 * The engine (SDL_serial_engine.h) feeds it bytes with clock values in
 * milliseconds, executes the actions it queues one at a time, and publishes
 * the snapshots it emits.
 *
 * Every module state begins with an SDL_SerialBase, which holds the action
 * queue, the snapshots and the pulse timers, so the helpers below serve every
 * module. Rules for every module:
 * - Parse only the bytes fed. A partial packet survives across feeds, so any
 *   split of a byte stream decodes the same as one feed.
 * - All timing comes from the clock argument.
 * - Reset queues a set line action first. The engine applies the timeouts and
 *   purges the port after it, before the next action runs.
 * - A pulsed input holds its button for SDL_SERIAL_PULSE_MS. A repeat during
 *   the pulse emits a released snapshot and a pressed one, then restarts the
 *   pulse, so event consumers see every press and state pollers see a held
 *   button.
 */

#ifndef SDL_serial_proto_h_
#define SDL_serial_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_SERIAL_MAX_SUBDEVICES 4
#define SDL_SERIAL_MAX_AXES       16
#define SDL_SERIAL_MAX_BUTTONS    128
#define SDL_SERIAL_MAX_HATS       2
#define SDL_SERIAL_MAX_BALLS      1
#define SDL_SERIAL_NAME_LENGTH    64
#define SDL_SERIAL_MAX_WRITE      64 /* bytes in one write action */
#define SDL_SERIAL_MAX_ACTIONS    16 /* queued actions per module */
#define SDL_SERIAL_MAX_EFFECT     16 /* bytes in one effect request, an I-Force effect core the longest */
#define SDL_SERIAL_PULSE_MS       100

/* Hat bits, equal to SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN and SDL_HAT_LEFT */
#define SDL_SERIAL_HAT_UP    0x01
#define SDL_SERIAL_HAT_RIGHT 0x02
#define SDL_SERIAL_HAT_DOWN  0x04
#define SDL_SERIAL_HAT_LEFT  0x08

/* Joystick types, equal to the SDL_JoystickType values */
#define SDL_SERIAL_TYPE_UNKNOWN      0
#define SDL_SERIAL_TYPE_GAMEPAD      1
#define SDL_SERIAL_TYPE_WHEEL        2
#define SDL_SERIAL_TYPE_ARCADE_STICK 3
#define SDL_SERIAL_TYPE_FLIGHT_STICK 4

typedef enum SDL_SerialParity
{
    SDL_SERIAL_PARITY_NONE,
    SDL_SERIAL_PARITY_ODD,
    SDL_SERIAL_PARITY_EVEN
} SDL_SerialParity;

typedef enum SDL_SerialFlow
{
    SDL_SERIAL_FLOW_NONE,
    SDL_SERIAL_FLOW_RTSCTS,    /* output waits for CTS, RTS in handshake mode */
    SDL_SERIAL_FLOW_RTS_TOGGLE /* RTS high while bytes wait to be sent */
} SDL_SerialFlow;

typedef struct SDL_SerialLine
{
    uint32_t rate;
    uint8_t data_bits; /* 7 or 8 */
    uint8_t parity;    /* SDL_SerialParity */
    uint8_t stop_bits; /* 1 or 2 */
    uint8_t flow;      /* SDL_SerialFlow */
    bool dtr;
    bool rts; /* Ignored unless flow is SDL_SERIAL_FLOW_NONE */
} SDL_SerialLine;

typedef enum SDL_SerialActionKind
{
    SDL_SERIAL_ACTION_NONE,
    SDL_SERIAL_ACTION_SET_LINE,  /* line: every field */
    SDL_SERIAL_ACTION_SET_MODEM, /* line.dtr and line.rts only */
    SDL_SERIAL_ACTION_WRITE,     /* data, length */
    SDL_SERIAL_ACTION_DRAIN      /* returns once written bytes have left the host */
} SDL_SerialActionKind;

typedef struct SDL_SerialAction
{
    SDL_SerialActionKind kind;
    uint8_t tag; /* Module-defined, handed back when the action is done */
    SDL_SerialLine line;
    size_t length;
    uint8_t data[SDL_SERIAL_MAX_WRITE];
} SDL_SerialAction;

typedef enum SDL_SerialMapKind
{
    SDL_SERIAL_MAP_NONE,
    SDL_SERIAL_MAP_BUTTON,
    SDL_SERIAL_MAP_AXIS,
    SDL_SERIAL_MAP_HAT,
    SDL_SERIAL_MAP_AXIS_POSITIVE, /* The positive half of an axis, as +aN */
    SDL_SERIAL_MAP_AXIS_NEGATIVE  /* The negative half of an axis, as -aN */
} SDL_SerialMapKind;

/* One gamepad input: a joystick button or axis index, or for a hat
 * (hat << 4) | SDL_SERIAL_HAT_* bit, as SDL_InputMapping encodes it */
typedef struct SDL_SerialMapInput
{
    uint8_t kind; /* SDL_SerialMapKind */
    uint8_t target;
} SDL_SerialMapInput;

typedef struct SDL_SerialGamepadMap
{
    SDL_SerialMapInput a, b, x, y;
    SDL_SerialMapInput back, guide, start;
    SDL_SerialMapInput leftstick, rightstick;
    SDL_SerialMapInput leftshoulder, rightshoulder;
    SDL_SerialMapInput dpup, dpdown, dpleft, dpright;
    SDL_SerialMapInput misc1;
    SDL_SerialMapInput leftx, lefty, rightx, righty;
    SDL_SerialMapInput lefttrigger, righttrigger;
} SDL_SerialGamepadMap;

typedef struct SDL_SerialIdentity
{
    char name[SDL_SERIAL_NAME_LENGTH];
    uint8_t type; /* SDL_SERIAL_TYPE_* */
    uint8_t naxes;
    uint8_t nbuttons;
    uint8_t nhats;
    uint8_t nballs;
    bool has_mapping;
    SDL_SerialGamepadMap mapping;
} SDL_SerialIdentity;

typedef struct SDL_SerialControls
{
    int16_t axes[SDL_SERIAL_MAX_AXES];
    uint8_t buttons[SDL_SERIAL_MAX_BUTTONS / 8]; /* Bit b % 8 of byte b / 8 */
    uint8_t hats[SDL_SERIAL_MAX_HATS];
    int16_t ball[2]; /* Horizontal and vertical motion since the previous snapshot */
} SDL_SerialControls;

typedef struct SDL_SerialSnapshot
{
    bool present;
    SDL_SerialIdentity identity; /* Fixed while present */
    SDL_SerialControls controls;
    uint32_t changes; /* Counts every snapshot this sub-device emitted */
} SDL_SerialSnapshot;

typedef enum SDL_SerialOutputKind
{
    SDL_SERIAL_OUTPUT_RUMBLE,
    SDL_SERIAL_OUTPUT_EFFECT
} SDL_SerialOutputKind;

typedef struct SDL_SerialOutput
{
    SDL_SerialOutputKind kind;
    int sub; /* The sub-device the request was made on */
    uint16_t low_frequency_rumble;
    uint16_t high_frequency_rumble;
    size_t length;
    uint8_t data[SDL_SERIAL_MAX_EFFECT];
} SDL_SerialOutput;

/* Receives every snapshot a module emits, in order, and its log lines */
typedef struct SDL_SerialSink
{
    void *userdata;
    void (*changed)(void *userdata, int sub, const SDL_SerialSnapshot *snapshot);
    void (*log)(void *userdata, const char *text);
} SDL_SerialSink;

typedef struct SDL_SerialBase
{
    SDL_SerialSink sink;

    SDL_SerialAction actions[SDL_SERIAL_MAX_ACTIONS];
    int action_head;
    int action_count;
    uint32_t epoch;          /* Advanced whenever the queue is cleared */
    bool taken;              /* An action was taken and is not yet done */
    uint8_t taken_tag;
    uint32_t taken_epoch;

    SDL_SerialSnapshot snapshots[SDL_SERIAL_MAX_SUBDEVICES];
    uint64_t pulse_end[SDL_SERIAL_MAX_SUBDEVICES][SDL_SERIAL_MAX_BUTTONS]; /* 0 when not pulsing */
} SDL_SerialBase;

/* The entry points. state points to the module's state, which begins with an
 * SDL_SerialBase. */
typedef struct SDL_SerialModule
{
    const char *token; /* Protocol token in SDL_HINT_JOYSTICK_SERIAL */
    size_t state_size;
    bool rumble;       /* Accepts SDL_SERIAL_OUTPUT_RUMBLE */
    size_t effect_min; /* Effect payload sizes it accepts, both 0 when none */
    size_t effect_max;
    /* Clear all state and queue the start-up actions */
    void (*Reset)(void *state, const SDL_SerialSink *sink, uint64_t now);
    /* Parse bytes, update controls, queue actions, change presence */
    void (*Feed)(void *state, const uint8_t *data, size_t length, uint64_t now);
    /* Run due deadlines: reply timeouts, retries, polls, keep-alives, pulse ends */
    void (*Tick)(void *state, uint64_t now);
    /* Take the oldest queued action. False when none */
    bool (*NextAction)(void *state, SDL_SerialAction *action);
    /* The action taken last has finished */
    void (*ActionDone)(void *state, bool success, uint64_t now);
    /* Queue writes for rumble or an effect */
    void (*Output)(void *state, const SDL_SerialOutput *request, uint64_t now);
    /* Earliest clock value at which Tick must run. False when none */
    bool (*GetDeadline)(void *state, uint64_t *deadline);
    const SDL_SerialSnapshot *(*GetSnapshot)(void *state, int sub);
} SDL_SerialModule;

/* Axis scaling. A signed value v in [min, max] is clamped, then v >= 0 maps
 * to v * 32767 / max and v < 0 to v * 32768 / -min, dividing toward zero.
 * An unsigned value in [lo, hi] is centered on (lo + hi) / 2 first. */
extern int16_t SDL_Serial_ScaleSigned(int32_t value, int32_t min, int32_t max);
extern int16_t SDL_Serial_ScaleUnsigned(int32_t value, int32_t lo, int32_t hi);

extern void SDL_Serial_SetButton(SDL_SerialControls *controls, int button, bool pressed);
extern bool SDL_Serial_GetButton(const SDL_SerialControls *controls, int button);
extern bool SDL_Serial_ControlsEqual(const SDL_SerialControls *a, const SDL_SerialControls *b);

/* Clears everything but the sink, which it sets, and advances the epoch */
extern void SDL_Serial_ResetBase(SDL_SerialBase *base, const SDL_SerialSink *sink);

/* Queue helpers. Each returns false, queuing nothing, when the queue is full
 * or the data does not fit. */
extern bool SDL_Serial_QueueLine(SDL_SerialBase *base, uint8_t tag, const SDL_SerialLine *line);
extern bool SDL_Serial_QueueModem(SDL_SerialBase *base, uint8_t tag, bool dtr, bool rts);
extern bool SDL_Serial_QueueWrite(SDL_SerialBase *base, uint8_t tag, const uint8_t *data, size_t length);
extern bool SDL_Serial_QueueDrain(SDL_SerialBase *base, uint8_t tag);
/* Replaces the data of a queued write that has not been taken yet, found by
 * its tag, so repeated output requests collapse into the latest. False when
 * no such write waits. */
extern bool SDL_Serial_ReplaceWrite(SDL_SerialBase *base, uint8_t tag, const uint8_t *data, size_t length);
/* Drops every queued action. The done of an action already taken is then
 * ignored. */
extern void SDL_Serial_ClearActions(SDL_SerialBase *base);
/* The NextAction entry point for every module */
extern bool SDL_Serial_NextAction(void *state, SDL_SerialAction *action);
/* For ActionDone: true, with the action's tag, when the finished action was
 * queued since the last clear */
extern bool SDL_Serial_FinishAction(SDL_SerialBase *base, uint8_t *tag);
/* The GetSnapshot entry point for every module */
extern const SDL_SerialSnapshot *SDL_Serial_GetSnapshot(void *state, int sub);

/* Presents a sub-device with all controls at rest and emits it */
extern void SDL_Serial_Present(SDL_SerialBase *base, int sub, const SDL_SerialIdentity *identity);
/* Clears presence and pulses, emitting when the sub-device was present */
extern void SDL_Serial_Absent(SDL_SerialBase *base, int sub);
extern void SDL_Serial_AbsentAll(SDL_SerialBase *base);
extern bool SDL_Serial_IsPresent(const SDL_SerialBase *base, int sub);
/* Replaces the controls of a present sub-device. Buttons with a running pulse
 * stay pressed. Emits when anything changed or a ball moved. */
extern void SDL_Serial_Commit(SDL_SerialBase *base, int sub, const SDL_SerialControls *controls);
/* Presses a pulsed button, or repeats it */
extern void SDL_Serial_Pulse(SDL_SerialBase *base, int sub, int button, uint64_t now);
/* Releases due pulses, one snapshot per sub-device */
extern void SDL_Serial_TickPulses(SDL_SerialBase *base, uint64_t now);
/* Folds the earliest pulse end into a deadline */
extern void SDL_Serial_PulseDeadline(const SDL_SerialBase *base, bool *have, uint64_t *deadline);
/* Folds one candidate into a deadline */
extern void SDL_Serial_EarlierDeadline(bool *have, uint64_t *deadline, uint64_t candidate);

/* One line for the driver's log, such as a device error */
extern void SDL_Serial_Log(SDL_SerialBase *base, const char *text);

/* Sets an identity. The name is truncated to fit. */
extern void SDL_Serial_SetIdentity(SDL_SerialIdentity *identity, const char *name, uint8_t type, int naxes, int nbuttons, int nhats, int nballs);

/* Gamepad inputs */
extern SDL_SerialMapInput SDL_Serial_MapButton(int button);
extern SDL_SerialMapInput SDL_Serial_MapAxis(int axis);
extern SDL_SerialMapInput SDL_Serial_MapHat(int hat, uint8_t bit);
extern SDL_SerialMapInput SDL_Serial_MapHalfAxis(int axis, bool positive);

/* A line of the given settings */
extern SDL_SerialLine SDL_Serial_Line(uint32_t rate, int data_bits, SDL_SerialParity parity, int stop_bits, SDL_SerialFlow flow, bool dtr, bool rts);

#endif /* SDL_serial_proto_h_ */
