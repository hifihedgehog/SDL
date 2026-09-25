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

/* The Ring Fit Adventure Ring-Con, read through the right Joy-Con it holds
 * (057E:2007, Bluetooth). Pure C99: no SDL runtime and no I/O. The Switch
 * driver sends the commands this machine returns, feeds it the 0x21 replies
 * and 0x30 reports it reads, and posts the strain it decodes.
 *
 * The start is six subcommands: input mode 0x30, MCU resume, MCU standby,
 * the external device query, the external format config and external
 * polling, as eden, joy-con-webhid and the WebHID demo send them. The stop
 * undoes what the start sent, in the order the Switch itself uses in the
 * console captures of Yamakaky/joy: 5B ends polling, a 5C with format 0
 * resets the report format, 22 00 suspends the MCU. From the 5C of the start
 * until up to 45 ms after that reset is acknowledged, bytes 37 to 48 of
 * report 0x30 carry Ring-Con data instead of the third IMU sample.
 *
 * The bytes are facts from those sources, Ringcon-Driver, osc-ringcon and
 * dekuNukem's notes. No code from any of them is copied.
 */

#ifndef SDL_hidapi_switch_ringcon_proto_h_
#define SDL_hidapi_switch_ringcon_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Subcommands, output report 0x01 byte 10
#define SDL_RINGCON_SUBCMD_SET_INPUT_MODE             0x03
#define SDL_RINGCON_SUBCMD_SET_MCU_CONFIG             0x21
#define SDL_RINGCON_SUBCMD_SET_MCU_STATE              0x22
#define SDL_RINGCON_SUBCMD_SET_EXTERNAL_CONFIG        0x58
#define SDL_RINGCON_SUBCMD_GET_EXTERNAL_DEVICE_INFO   0x59
#define SDL_RINGCON_SUBCMD_ENABLE_EXTERNAL_POLLING    0x5A
#define SDL_RINGCON_SUBCMD_DISABLE_EXTERNAL_POLLING   0x5B
#define SDL_RINGCON_SUBCMD_SET_EXTERNAL_FORMAT_CONFIG 0x5C

#define SDL_RINGCON_REPORT_REPLY 0x21 // subcommand reply
#define SDL_RINGCON_REPORT_FULL  0x30 // standard full report, the one that carries the strain

// The external device ID the Ring-Con answers 0x59 with, reply bytes 15-16
#define SDL_RINGCON_DEVICE_ID_HIGH 0x00
#define SDL_RINGCON_DEVICE_ID_LOW  0x20

#define SDL_RINGCON_ARGS_MAX          38 // output report bytes 11 to 48
#define SDL_RINGCON_REPLY_MIN_LENGTH  17 // a reply is read through byte 16
#define SDL_RINGCON_STRAIN_MIN_LENGTH 41 // the strain is bytes 39-40

#define SDL_RINGCON_STEP_GAP_MS      50   // after a matching reply, before the next command
#define SDL_RINGCON_RESEND_MS        600  // an unanswered command is sent again
#define SDL_RINGCON_MAX_SENDS        8    // the first send and 7 resends
#define SDL_RINGCON_QUERY_SILENCE_MS 150  // 0x59 with no reply is sent again
#define SDL_RINGCON_QUERY_MAX_SENDS  42   // unmatched 0x59 sends before the Ring-Con counts as absent
#define SDL_RINGCON_WATCHDOG_MS      2000 // no nonzero strain while polling

// Joystick properties, set by the driver
#define SDL_RINGCON_PROP_ACTIVE "SDL.joystick.switch.ringcon"
#define SDL_RINGCON_PROP_REST   "SDL.joystick.switch.ringcon_rest"

// Phases
#define SDL_RINGCON_PHASE_IDLE    0
#define SDL_RINGCON_PHASE_START   1
#define SDL_RINGCON_PHASE_POLLING 2
#define SDL_RINGCON_PHASE_STOP    3

// Why a stop runs, which decides what follows it
#define SDL_RINGCON_STOP_NONE     0
#define SDL_RINGCON_STOP_DISABLED 1 // the hint turned off: restore the input mode, then idle
#define SDL_RINGCON_STOP_YIELD    2 // the NIR camera or NFC wants the MCU: idle, resume when it is free
#define SDL_RINGCON_STOP_ABSENT   3 // no Ring-Con answered 0x59: restore the input mode, then idle
#define SDL_RINGCON_STOP_FAILED   4 // a start command went unanswered: restore the input mode, then idle
#define SDL_RINGCON_STOP_WATCHDOG 5 // the strain went silent: one fresh start

// Gate results for a 0x21 reply
#define SDL_RINGCON_GATE_NONE     0 // not a reply to this command
#define SDL_RINGCON_GATE_MATCH    1
#define SDL_RINGCON_GATE_MISMATCH 2 // 0x59 answered with another external device ID

typedef struct SDL_RingConCommand
{
    uint8_t subcommand;
    uint8_t length; // argument bytes
    uint8_t args[SDL_RINGCON_ARGS_MAX];
} SDL_RingConCommand;

typedef struct SDL_RingConOutput
{
    bool send; // send command now
    SDL_RingConCommand command;
    bool polling_changed; // the active property takes the value of polling
    bool polling;
    bool start_began;     // a start sent its first command
    int stop_began;       // a stop began, for this reason
} SDL_RingConOutput;

typedef struct SDL_RingConStrain
{
    bool post; // post value on the strain axis
    int16_t value;
    bool rest_changed; // the rest property takes rest
    int16_t rest;
} SDL_RingConStrain;

typedef struct SDL_RingConMachine
{
    int phase;
    int stop_reason;
    int stop_request;  // a stop waits for the command in flight
    int program[4];    // the stop's commands
    int program_length;
    int index;         // position in the start or stop program
    int command;       // the command in flight or last acknowledged
    int sends;         // sends of that command
    bool acked;        // its gate passed
    bool retry;        // 0x59 answered with another ID: resend at next_ms
    uint64_t sent_ms;
    uint64_t next_ms;
    uint8_t restore_mode;
    bool enabled;       // the hint, as last seen
    bool pending;       // a start waits for a free MCU
    bool reprobe_spent; // the watchdog's fresh start ran with no strain since
    bool format_set;    // 5C was sent and no stop has finished with its reset acknowledged
    bool format_reset;  // this stop's format reset was acknowledged
    bool polling_set;   // 5A was sent and no 5B has been acknowledged
    int16_t strain;     // the last nonzero strain, 0 before the first
    int16_t rest;       // the first nonzero strain after polling began, 0 before
    uint64_t alive_ms;  // polling start or the last nonzero strain
} SDL_RingConMachine;

extern uint8_t SDL_RingCon_Crc8(const uint8_t *data, size_t length);

/* A start step's command, step 1 to 6: input mode 0x30, MCU resume, MCU
 * standby config with its CRC at byte 48, the external device query, the
 * external format config, external polling. */
extern bool SDL_RingCon_BuildStartCommand(int step, SDL_RingConCommand *out);

extern int SDL_RingCon_GateReply(const SDL_RingConCommand *command, const uint8_t *report, size_t length);

// The strain of a 0x30 report: bytes 39-40, signed, little endian. 0x0000 is not a reading.
extern bool SDL_RingCon_DecodeStrain(const uint8_t *report, size_t length, int16_t *value);

extern void SDL_RingCon_Init(SDL_RingConMachine *machine);

// A fresh machine for an opened joystick. With the hint on, a start waits.
extern void SDL_RingCon_Open(SDL_RingConMachine *machine, bool enabled);

/* One step per update tick. enabled is the hint, mcu_wanted is true while
 * the NIR camera or NFC holds or wants the MCU, restore_mode is the input
 * mode the driver uses without the Ring-Con. Sends at most one command. */
extern void SDL_RingCon_Update(SDL_RingConMachine *machine, uint64_t now_ms, bool enabled, bool mcu_wanted, uint8_t restore_mode, SDL_RingConOutput *out);

// A 0x21 report. Never sends: the next command goes out on a later Update.
extern void SDL_RingCon_OnReply(SDL_RingConMachine *machine, const uint8_t *report, size_t length, uint64_t now_ms, SDL_RingConOutput *out);

// A full report while polling: the strain axis and the rest property
extern void SDL_RingCon_OnFullReport(SDL_RingConMachine *machine, const uint8_t *report, size_t length, uint64_t now_ms, SDL_RingConStrain *out);

// True from the first command of a start to the end of its stop
extern bool SDL_RingCon_Engaged(const SDL_RingConMachine *machine);

/* The IMU samples of a 0x30 report to post, oldest first, as indices into the
 * driver's three samples: 0 is bytes 13-24, 1 bytes 25-36, 2 bytes 37-48.
 * From the 5C send to the end of the stop that resets the format, bytes 37-48
 * are not IMU data. */
extern int SDL_RingCon_ImuPostOrder(const SDL_RingConMachine *machine, int order[3]);

/* What the close path writes, waiting for each reply: 5B if polling began, the
 * format reset if a format was set, and 22 00. Returns the count. */
extern int SDL_RingCon_BuildCloseCommands(const SDL_RingConMachine *machine, SDL_RingConCommand out[3]);

/* The joystick's axis count: a right Joy-Con has the NIR axis and the strain
 * axis past the gamepad axes. Grow-only, so either half of a pair may open
 * first. The strain axis is gamepad_axis_count + 1. */
extern int SDL_RingCon_AxisCount(bool right_joycon, int naxes, int gamepad_axis_count);

#endif // SDL_hidapi_switch_ringcon_proto_h_
