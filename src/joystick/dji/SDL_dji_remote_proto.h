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

/* DJI drone remotes on their USB serial port and on their USB bulk
 * interface. Pure C99: no SDL runtime and no I/O. Each remote is a module of
 * the serial joystick contract (src/joystick/serial/SDL_serial_proto.h): the
 * serial driver runs the four serial modules on COM ports, and the HIDAPI
 * driver SDL_hidapi_djiremote.c runs the bulk module, completing its line
 * action at once and sending its writes as bulk transfers.
 *
 * Every remote becomes a gamepad with the gimbals by physical position:
 * axes 0 and 1 the left gimbal, 2 and 3 the right gimbal, 4 the gimbal dial
 * and 5 a second dial. Up is negative on both vertical axes. The facts are
 * from DjiMini2RCasJoystick, DJI_RC-N1_SIMULATOR_FLY_DCL, miniDjiController,
 * the three mDjiController projects, DJI-RC-Emulator, DJI_RCN1_ and
 * DJI_RCNx_for_drone_simulators (no license, facts only), dji-rc-joystick
 * by gregszero (GPL, facts only) and dji-firmware-tools (GPL, facts only).
 * No code from them is copied.
 */

#ifndef SDL_dji_remote_proto_h_
#define SDL_dji_remote_proto_h_

#include "SDL_dji_duml.h"
#include "../serial/SDL_serial_proto.h"

#define SDL_DJI_RATE 115200

/* Joystick axes */
#define SDL_DJI_AXIS_LEFT_X  0
#define SDL_DJI_AXIS_LEFT_Y  1
#define SDL_DJI_AXIS_RIGHT_X 2
#define SDL_DJI_AXIS_RIGHT_Y 3
#define SDL_DJI_AXIS_DIAL    4
#define SDL_DJI_AXIS_DIAL2   5

/* Joystick buttons. A remote reports only the buttons it has. */
#define SDL_DJI_BUTTON_HOME         0  /* Return-to-home or pause */
#define SDL_DJI_BUTTON_RECORD       1
#define SDL_DJI_BUTTON_SHUTTER      2  /* Full press */
#define SDL_DJI_BUTTON_SHUTTER_HALF 3
#define SDL_DJI_BUTTON_C1           4
#define SDL_DJI_BUTTON_C2           5
#define SDL_DJI_BUTTON_FN           6
#define SDL_DJI_BUTTON_PLAYBACK     7
#define SDL_DJI_BUTTON_DIAL_PRESS   8
#define SDL_DJI_BUTTON_MODE         9  /* 9 to 11: one flight mode position held */
#define SDL_DJI_BUTTON_LEVER        12 /* 12 to 14: the Phantom 2's right lever */
#define SDL_DJI_BUTTON_RCN1_BIT1    15 /* 15 to 18: the RC-N1's unnamed status bits */
#define SDL_DJI_BUTTON_RCN1_BIT2    16
#define SDL_DJI_BUTTON_RCN1_BITS56  17
#define SDL_DJI_BUTTON_RCN1_BIT7    18
#define SDL_DJI_BUTTON_DIAL_UP      19 /* Phantom 3 right dial steps, pulsed */
#define SDL_DJI_BUTTON_DIAL_DOWN    20

/* Timing, in ms */
#define SDL_DJI_SIMULATOR_SETTLE_MS 50   /* Serial: after 06/24, input dropped */
#define SDL_DJI_POLL_RESEND_MS      25   /* Serial: no 06/01 reply, the poll again */
#define SDL_DJI_SERIAL_SILENCE_MS   2000 /* Serial: no stick report, start-up again */
#define SDL_DJI_PHANTOM_CYCLE_MS    10   /* Phantom 2 and 3: the ping loop */
#define SDL_DJI_PHANTOM2_WAIT_MS    2000 /* Phantom 2: after the init frame */
#define SDL_DJI_BULK_SETTLE_MS      100  /* Bulk: after the first 06/24 */
#define SDL_DJI_BULK_POLL_MS        10   /* Bulk: no 06/01 reply, the poll again */
#define SDL_DJI_BULK_SIMULATOR_MS   3000 /* Bulk: 06/24 again */
#define SDL_DJI_BULK_FALLBACK_MS    200  /* Bulk: no 32-byte 06/01 reply, 06/F5 instead */
#define SDL_DJI_BULK_TEST_STICK_MS  12   /* Bulk: the 06/F5 poll loop */
#define SDL_DJI_BULK_SILENCE_MS     1000 /* Bulk: no stick report, start-up again */

#define SDL_DJI_PHANTOM2_REPLY_LENGTH 76
#define SDL_DJI_MODEL_LENGTH          16

typedef enum SDL_DJIRemoteKind
{
    SDL_DJI_REMOTE_RCN1,       /* RC-N1 family: 06/24, then 06/01 and 06/27 to 06 */
    SDL_DJI_REMOTE_MAVIC_MINI, /* Mavic Mini remote: 06/01 to 0E */
    SDL_DJI_REMOTE_PHANTOM3,   /* Phantom 3 remote: 00/0E to 03 and 06/27 to 0E every 10 ms */
    SDL_DJI_REMOTE_PHANTOM2,   /* Phantom 2 remote: two fixed frames, no DUML framing */
    SDL_DJI_REMOTE_BULK        /* DJI RC over USB bulk: 06/24, 06/01, else 06/F5 */
} SDL_DJIRemoteKind;

typedef enum SDL_DJIRemoteStep
{
    SDL_DJI_STEP_SIMULATOR, /* 06/24 written */
    SDL_DJI_STEP_SETTLE,    /* The wait after 06/24 */
    SDL_DJI_STEP_INIT,      /* Phantom 2: the init frame written */
    SDL_DJI_STEP_INIT_WAIT, /* Phantom 2: the wait after it */
    SDL_DJI_STEP_POLL
} SDL_DJIRemoteStep;

typedef struct SDL_DJIRemoteState
{
    SDL_SerialBase base;
    SDL_DJIRemoteKind kind;
    SDL_DJIRemoteStep step;
    SDL_DJIParser parser;
    uint8_t write_seq;   /* Tag of the last start-up or poll write, kept across resets */
    uint8_t waiting_seq; /* The write whose completion starts the step timer, 0 when none */
    bool timer;          /* The step timer: a wait, a poll resend or the ping loop */
    uint64_t deadline;
    bool silence_timer;
    uint64_t silence_deadline;
    bool simulator_timer; /* Bulk: 06/24 again */
    uint64_t simulator_deadline;
    bool simulator_waiting; /* Bulk: that 06/24 is written and not done */
    bool fallback_timer;    /* Bulk: 200 ms after the first 06/01 poll */
    uint64_t fallback_deadline;
    bool fallback_armed;    /* Bulk: the first 06/01 poll of this start-up is out */
    bool test_stick;        /* Bulk: polling with 06/F5 */
    uint16_t test_sequence; /* Bulk: the 06/F5 sequence, from 1 */
    bool dropping;          /* RC-N1: input during the settle is dropped */
    SDL_SerialControls controls;
    int naxes;             /* Values in the first stick report of this presence */
    int mode;              /* Flight mode position 0 to 2, -1 unknown */
    int levers[2];         /* Phantom 2 lever positions 0 to 2, -1 unknown */
    bool wheel_known;      /* Phantom 3: the dial counter has a baseline */
    int8_t wheel;
    size_t window_length;  /* Phantom 2: bytes since the last loop pass */
    uint8_t window[SDL_DJI_PHANTOM2_REPLY_LENGTH];
    char model[SDL_DJI_MODEL_LENGTH]; /* From 00/81 or 00/82, empty until then */
} SDL_DJIRemoteState;

extern const SDL_SerialModule SDL_DJIRemoteRCN1Module;      /* Token "dji" */
extern const SDL_SerialModule SDL_DJIRemoteMavicMiniModule; /* Token "djimavicmini" */
extern const SDL_SerialModule SDL_DJIRemotePhantom3Module;  /* Token "djiphantom3" */
extern const SDL_SerialModule SDL_DJIRemotePhantom2Module;  /* Token "djiphantom2" */
extern const SDL_SerialModule SDL_DJIRemoteBulkModule;      /* Run by the HIDAPI driver */

/* (raw - center) * 32767 / half, dividing toward zero, clamped to +-32767 */
extern int16_t SDL_DJI_ScaleAxis(int raw, int center, int half);

/* The 06/01 reply: 38 bytes with five values from byte 13, or 32 bytes with
 * six values from byte 12, each value 3 bytes after the one before. Fills
 * values and returns their count, 0 for any other frame. */
extern int SDL_DJI_DecodeChannels(const SDL_DJIFrame *frame, uint16_t *values);

/* The RC-N1's 58-byte 06/27 reply: the flight mode position 0 to 2 and the
 * status byte 29 */
extern bool SDL_DJI_DecodeRCN1Status(const SDL_DJIFrame *frame, int *mode, uint8_t *bits);

/* The Phantom 3's 58-byte 06/27 reply */
typedef struct SDL_DJIPhantom3Report
{
    int16_t right_y;
    int16_t right_x;
    int16_t left_y;
    int16_t left_x;
    int16_t dial;
    int8_t wheel;   /* The right dial's counter */
    int8_t mode;    /* Byte 28 */
    uint8_t buttons; /* Byte 29 */
} SDL_DJIPhantom3Report;
extern bool SDL_DJI_DecodePhantom3(const SDL_DJIFrame *frame, SDL_DJIPhantom3Report *report);

/* The 26-byte 06/F5 reply: the right horizontal, right vertical, left
 * vertical and left horizontal values */
extern bool SDL_DJI_DecodeTestStick(const SDL_DJIFrame *frame, uint16_t *values);

/* A model string from 00/81 or 00/82: printable ASCII up to the first 00 */
extern bool SDL_DJI_DecodeModel(const SDL_DJIFrame *frame, char *model, size_t size);
/* The joystick name for a model string, else fallback */
extern const char *SDL_DJI_NameForModel(const char *model, const char *fallback);

/* A Phantom 2 read of exactly 76 bytes starting with 55: the right
 * horizontal, right vertical, left vertical and left horizontal values, the
 * left and right levers and the dial */
typedef struct SDL_DJIPhantom2Report
{
    int16_t right_x;
    int16_t right_y;
    int16_t left_y;
    int16_t left_x;
    int16_t levers[2];
    int16_t dial;
} SDL_DJIPhantom2Report;
extern bool SDL_DJI_DecodePhantom2(const uint8_t *data, size_t length, SDL_DJIPhantom2Report *report);

/* The gamepad mapping of the bulk remote, for SDL_gamepad.c */
#define SDL_DJI_BULK_MAPPING "b:-a4,leftx:a0,lefty:a1,rightx:a2,righty:a3,y:+a4,"

#endif /* SDL_dji_remote_proto_h_ */
