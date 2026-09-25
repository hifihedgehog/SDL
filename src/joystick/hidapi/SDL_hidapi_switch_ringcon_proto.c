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

#include "SDL_hidapi_switch_ringcon_proto.h"

#include <string.h>

// The commands the machine sends. 1 to 6 are the start steps.
#define RINGCON_CMD_NONE         0
#define RINGCON_CMD_INPUT_MODE   1
#define RINGCON_CMD_MCU_RESUME   2
#define RINGCON_CMD_MCU_STANDBY  3
#define RINGCON_CMD_QUERY        4
#define RINGCON_CMD_FORMAT       5
#define RINGCON_CMD_POLL         6
#define RINGCON_CMD_POLL_OFF     7
#define RINGCON_CMD_FORMAT_RESET 8
#define RINGCON_CMD_MCU_SUSPEND  9
#define RINGCON_CMD_RESTORE_MODE 10

#define RINGCON_START_STEPS 6

/* Step 5's arguments, the ones eden, joy-con-webhid, the WebHID demo,
   osc-ringcon and Ringcon-Driver send. In the Switch's own captures bytes 8-12,
   16-21 and 32-36 differ from these and from one session to the next, and the
   Joy-Con accepts every version. */
static const uint8_t ringcon_format_config[37] = {
    0x06, 0x03, 0x25, 0x06, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x16, 0xED, 0x34, 0x36,
    0x00, 0x00, 0x00, 0x0A, 0x64, 0x0B, 0xE6, 0xA9, 0x22, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0xA8, 0xE1, 0x34, 0x36
};

/* The format reset: 5C with format 0. These are the arguments the Switch sent
   in the Yamakaky/joy capture trace/ringfit-multiple-connect.log, and the ones
   joy's disable_ringcon sends. */
static const uint8_t ringcon_format_reset[38] = {
    0x00, 0x00, 0x96, 0xE3, 0x1C, 0x00, 0x00, 0x00, 0xEC, 0x99, 0xAC, 0xE3, 0x1C,
    0x00, 0x00, 0x00, 0xF3, 0x82, 0xF1, 0x59, 0x2E, 0x59, 0x00, 0x00, 0xE0, 0x58,
    0xB3, 0xE3, 0x1C, 0x00, 0x00, 0x00, 0x00, 0xF2, 0x05, 0x2A, 0x01, 0x00
};

// CRC-8, polynomial 0x07, initial value 0: the MCU config checksum
uint8_t SDL_RingCon_Crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    size_t i;
    int bit;

    for (i = 0; i < length; ++i) {
        crc = (uint8_t)(crc ^ data[i]);
        for (bit = 0; bit < 8; ++bit) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ 0x07);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

static void BuildCommand(int command, uint8_t restore_mode, SDL_RingConCommand *out)
{
    memset(out, 0, sizeof(*out));

    switch (command) {
    case RINGCON_CMD_INPUT_MODE:
        out->subcommand = SDL_RINGCON_SUBCMD_SET_INPUT_MODE;
        out->args[0] = SDL_RINGCON_REPORT_FULL;
        out->length = 1;
        break;
    case RINGCON_CMD_MCU_RESUME:
        out->subcommand = SDL_RINGCON_SUBCMD_SET_MCU_STATE;
        out->args[0] = 0x01;
        out->length = 1;
        break;
    case RINGCON_CMD_MCU_STANDBY:
        // MCU command 0x21 configure, sub-command 0x01 device mode, mode 0x01 standby, CRC over bytes 1-36
        out->subcommand = SDL_RINGCON_SUBCMD_SET_MCU_CONFIG;
        out->args[0] = 0x21;
        out->args[1] = 0x01;
        out->args[2] = 0x01;
        out->args[37] = SDL_RingCon_Crc8(&out->args[1], 36);
        out->length = SDL_RINGCON_ARGS_MAX;
        break;
    case RINGCON_CMD_QUERY:
        out->subcommand = SDL_RINGCON_SUBCMD_GET_EXTERNAL_DEVICE_INFO;
        break;
    case RINGCON_CMD_FORMAT:
        out->subcommand = SDL_RINGCON_SUBCMD_SET_EXTERNAL_FORMAT_CONFIG;
        memcpy(out->args, ringcon_format_config, sizeof(ringcon_format_config));
        out->length = (uint8_t)sizeof(ringcon_format_config);
        break;
    case RINGCON_CMD_POLL:
        out->subcommand = SDL_RINGCON_SUBCMD_ENABLE_EXTERNAL_POLLING;
        out->args[0] = 0x04;
        out->args[1] = 0x01;
        out->args[2] = 0x01;
        out->args[3] = 0x02;
        out->length = 4;
        break;
    case RINGCON_CMD_POLL_OFF:
        out->subcommand = SDL_RINGCON_SUBCMD_DISABLE_EXTERNAL_POLLING;
        break;
    case RINGCON_CMD_FORMAT_RESET:
        out->subcommand = SDL_RINGCON_SUBCMD_SET_EXTERNAL_FORMAT_CONFIG;
        memcpy(out->args, ringcon_format_reset, sizeof(ringcon_format_reset));
        out->length = (uint8_t)sizeof(ringcon_format_reset);
        break;
    case RINGCON_CMD_MCU_SUSPEND:
        out->subcommand = SDL_RINGCON_SUBCMD_SET_MCU_STATE;
        out->args[0] = 0x00;
        out->length = 1;
        break;
    case RINGCON_CMD_RESTORE_MODE:
        out->subcommand = SDL_RINGCON_SUBCMD_SET_INPUT_MODE;
        out->args[0] = restore_mode;
        out->length = 1;
        break;
    default:
        break;
    }
}

bool SDL_RingCon_BuildStartCommand(int step, SDL_RingConCommand *out)
{
    if (!out || step < 1 || step > RINGCON_START_STEPS) {
        return false;
    }
    BuildCommand(RINGCON_CMD_INPUT_MODE + (step - 1), 0, out);
    return true;
}

int SDL_RingCon_GateReply(const SDL_RingConCommand *command, const uint8_t *report, size_t length)
{
    // Byte 13 bit 7 is the acknowledge, byte 14 echoes the subcommand
    if (!command || !report || length < SDL_RINGCON_REPLY_MIN_LENGTH ||
        report[0] != SDL_RINGCON_REPORT_REPLY ||
        !(report[13] & 0x80) || report[14] != command->subcommand) {
        return SDL_RINGCON_GATE_NONE;
    }

    switch (command->subcommand) {
    case SDL_RINGCON_SUBCMD_SET_INPUT_MODE:
    case SDL_RINGCON_SUBCMD_SET_MCU_STATE:
        // These answer with the plain acknowledge, 0x80, and no data
        return (report[13] == 0x80) ? SDL_RINGCON_GATE_MATCH : SDL_RINGCON_GATE_NONE;
    case SDL_RINGCON_SUBCMD_GET_EXTERNAL_DEVICE_INFO:
        if (report[15] == SDL_RINGCON_DEVICE_ID_HIGH && report[16] == SDL_RINGCON_DEVICE_ID_LOW) {
            return SDL_RINGCON_GATE_MATCH;
        }
        return SDL_RINGCON_GATE_MISMATCH;
    default:
        return SDL_RINGCON_GATE_MATCH;
    }
}

bool SDL_RingCon_DecodeStrain(const uint8_t *report, size_t length, int16_t *value)
{
    int raw;

    if (!report || length < SDL_RINGCON_STRAIN_MIN_LENGTH || report[0] != SDL_RINGCON_REPORT_FULL) {
        return false;
    }
    raw = report[39] | (report[40] << 8);
    if (raw == 0) {
        return false;
    }
    if (value) {
        *value = (int16_t)((raw >= 0x8000) ? (raw - 0x10000) : raw);
    }
    return true;
}

void SDL_RingCon_Init(SDL_RingConMachine *machine)
{
    memset(machine, 0, sizeof(*machine));
}

void SDL_RingCon_Open(SDL_RingConMachine *machine, bool enabled)
{
    SDL_RingCon_Init(machine);
    machine->enabled = enabled;
    machine->pending = enabled;
}

static bool RestoresMode(int reason)
{
    return reason == SDL_RINGCON_STOP_DISABLED ||
           reason == SDL_RINGCON_STOP_ABSENT ||
           reason == SDL_RINGCON_STOP_FAILED;
}

static void SendCurrent(SDL_RingConMachine *machine, uint64_t now_ms, SDL_RingConOutput *out)
{
    BuildCommand(machine->command, machine->restore_mode, &out->command);
    out->send = true;
    ++machine->sends;
    machine->sent_ms = now_ms;
    machine->acked = false;
    machine->retry = false;

    // The report format changes from the moment these may reach the Joy-Con
    if (machine->command == RINGCON_CMD_FORMAT) {
        machine->format_set = true;
    } else if (machine->command == RINGCON_CMD_POLL) {
        machine->polling_set = true;
    }
}

static void BeginCommand(SDL_RingConMachine *machine, int command, uint64_t now_ms, SDL_RingConOutput *out)
{
    machine->command = command;
    machine->sends = 0;
    SendCurrent(machine, now_ms, out);
}

static void BeginStart(SDL_RingConMachine *machine, uint64_t now_ms, SDL_RingConOutput *out)
{
    machine->phase = SDL_RINGCON_PHASE_START;
    machine->stop_reason = SDL_RINGCON_STOP_NONE;
    machine->stop_request = SDL_RINGCON_STOP_NONE;
    machine->pending = false;
    machine->index = 0;
    out->start_began = true;
    BeginCommand(machine, RINGCON_CMD_INPUT_MODE, now_ms, out);
}

// Undo what the start sent, newest first, then suspend the MCU
static void BeginStop(SDL_RingConMachine *machine, int reason, uint64_t now_ms, SDL_RingConOutput *out)
{
    if (machine->phase == SDL_RINGCON_PHASE_POLLING) {
        out->polling_changed = true;
        out->polling = false;
    }
    machine->phase = SDL_RINGCON_PHASE_STOP;
    machine->stop_reason = reason;
    machine->stop_request = SDL_RINGCON_STOP_NONE;
    machine->program_length = 0;
    if (machine->polling_set) {
        machine->program[machine->program_length++] = RINGCON_CMD_POLL_OFF;
    }
    if (machine->format_set) {
        machine->program[machine->program_length++] = RINGCON_CMD_FORMAT_RESET;
    }
    machine->program[machine->program_length++] = RINGCON_CMD_MCU_SUSPEND;
    if (RestoresMode(reason)) {
        machine->program[machine->program_length++] = RINGCON_CMD_RESTORE_MODE;
    }
    machine->index = 0;
    out->stop_began = reason;
    BeginCommand(machine, machine->program[0], now_ms, out);
}

static void FinishStop(SDL_RingConMachine *machine, uint64_t now_ms, SDL_RingConOutput *out)
{
    int reason = machine->stop_reason;

    machine->phase = SDL_RINGCON_PHASE_IDLE;
    machine->stop_reason = SDL_RINGCON_STOP_NONE;
    machine->command = RINGCON_CMD_NONE;
    machine->sends = 0;
    machine->acked = false;

    /* The Switch's captures show bytes 37-48 still without IMU data for up to
       45 ms after the reset's acknowledgment. The stop's later commands take
       longer than that, so the IMU samples count as three again only here. */
    if (machine->format_reset) {
        machine->format_set = false;
        machine->format_reset = false;
    }

    switch (reason) {
    case SDL_RINGCON_STOP_WATCHDOG:
        // The one fresh start. A second silent watchdog before any strain ends as absent.
        machine->reprobe_spent = true;
        BeginStart(machine, now_ms, out);
        break;
    case SDL_RINGCON_STOP_YIELD:
        machine->pending = machine->enabled;
        break;
    case SDL_RINGCON_STOP_ABSENT:
    case SDL_RINGCON_STOP_FAILED:
        machine->pending = false;
        break;
    default:
        // Disabled: the hint's edges own pending
        break;
    }
}

static void Advance(SDL_RingConMachine *machine, uint64_t now_ms, SDL_RingConOutput *out)
{
    if (machine->phase == SDL_RINGCON_PHASE_START) {
        if (machine->stop_request != SDL_RINGCON_STOP_NONE) {
            BeginStop(machine, machine->stop_request, now_ms, out);
        } else if (machine->index + 1 < RINGCON_START_STEPS) {
            ++machine->index;
            BeginCommand(machine, RINGCON_CMD_INPUT_MODE + machine->index, now_ms, out);
        }
    } else if (machine->phase == SDL_RINGCON_PHASE_STOP) {
        if (machine->index + 1 < machine->program_length) {
            ++machine->index;
            BeginCommand(machine, machine->program[machine->index], now_ms, out);
        } else {
            FinishStop(machine, now_ms, out);
        }
    }
}

void SDL_RingCon_Update(SDL_RingConMachine *machine, uint64_t now_ms, bool enabled, bool mcu_wanted, uint8_t restore_mode, SDL_RingConOutput *out)
{
    uint64_t due;

    memset(out, 0, sizeof(*out));
    machine->restore_mode = restore_mode;

    // The hint's edges: turning it on arms a start, turning it off cancels one
    if (enabled && !machine->enabled) {
        machine->pending = true;
        machine->reprobe_spent = false;
    } else if (!enabled && machine->enabled) {
        machine->pending = false;
    }
    machine->enabled = enabled;

    switch (machine->phase) {
    case SDL_RINGCON_PHASE_IDLE:
        if (machine->pending && enabled && !mcu_wanted) {
            BeginStart(machine, now_ms, out);
        }
        return;

    case SDL_RINGCON_PHASE_START:
    case SDL_RINGCON_PHASE_POLLING:
        if (!enabled) {
            machine->stop_request = SDL_RINGCON_STOP_DISABLED;
        } else if (mcu_wanted) {
            machine->stop_request = SDL_RINGCON_STOP_YIELD;
        } else {
            machine->stop_request = SDL_RINGCON_STOP_NONE;
        }
        break;

    case SDL_RINGCON_PHASE_STOP:
        // A stop is running: make it end the way the new state needs
        if (!enabled && !RestoresMode(machine->stop_reason)) {
            machine->stop_reason = SDL_RINGCON_STOP_DISABLED;
            machine->program[machine->program_length++] = RINGCON_CMD_RESTORE_MODE;
        } else if (enabled && mcu_wanted && machine->stop_reason == SDL_RINGCON_STOP_WATCHDOG) {
            machine->stop_reason = SDL_RINGCON_STOP_YIELD;
        }
        break;

    default:
        return;
    }

    if (machine->phase == SDL_RINGCON_PHASE_POLLING) {
        if (machine->stop_request == SDL_RINGCON_STOP_NONE &&
            now_ms >= machine->alive_ms + SDL_RINGCON_WATCHDOG_MS) {
            machine->stop_request = machine->reprobe_spent ? SDL_RINGCON_STOP_ABSENT : SDL_RINGCON_STOP_WATCHDOG;
        }
        if (machine->stop_request != SDL_RINGCON_STOP_NONE) {
            BeginStop(machine, machine->stop_request, now_ms, out);
        }
        return;
    }

    // Start and stop: the next command goes out 50 ms after a matching reply
    if (machine->acked) {
        if (now_ms >= machine->next_ms) {
            Advance(machine, now_ms, out);
        }
        return;
    }

    // Unanswered: 0x59 again 150 ms after a silent send or 50 ms after another ID, anything else after 600 ms
    if (machine->command == RINGCON_CMD_QUERY) {
        due = machine->retry ? machine->next_ms : machine->sent_ms + SDL_RINGCON_QUERY_SILENCE_MS;
    } else {
        due = machine->sent_ms + SDL_RINGCON_RESEND_MS;
    }
    if (now_ms < due) {
        return;
    }
    if (machine->phase == SDL_RINGCON_PHASE_START && machine->stop_request != SDL_RINGCON_STOP_NONE) {
        BeginStop(machine, machine->stop_request, now_ms, out);
    } else if (machine->command == RINGCON_CMD_QUERY && machine->sends >= SDL_RINGCON_QUERY_MAX_SENDS) {
        BeginStop(machine, SDL_RINGCON_STOP_ABSENT, now_ms, out);
    } else if (machine->command != RINGCON_CMD_QUERY && machine->sends >= SDL_RINGCON_MAX_SENDS) {
        if (machine->phase == SDL_RINGCON_PHASE_START) {
            BeginStop(machine, SDL_RINGCON_STOP_FAILED, now_ms, out);
        } else {
            // A stop command that is never acknowledged is given up, and the stop goes on
            Advance(machine, now_ms, out);
        }
    } else {
        SendCurrent(machine, now_ms, out);
    }
}

void SDL_RingCon_OnReply(SDL_RingConMachine *machine, const uint8_t *report, size_t length, uint64_t now_ms, SDL_RingConOutput *out)
{
    SDL_RingConCommand command;
    int gate;

    memset(out, 0, sizeof(*out));
    if ((machine->phase != SDL_RINGCON_PHASE_START && machine->phase != SDL_RINGCON_PHASE_STOP) ||
        machine->command == RINGCON_CMD_NONE || machine->acked) {
        return;
    }

    BuildCommand(machine->command, machine->restore_mode, &command);
    gate = SDL_RingCon_GateReply(&command, report, length);
    if (gate == SDL_RINGCON_GATE_NONE) {
        return;
    }
    if (gate == SDL_RINGCON_GATE_MISMATCH) {
        if (!machine->retry) {
            machine->retry = true;
            machine->next_ms = now_ms + SDL_RINGCON_STEP_GAP_MS;
        }
        return;
    }

    machine->acked = true;
    machine->retry = false;
    machine->next_ms = now_ms + SDL_RINGCON_STEP_GAP_MS;
    if (machine->command == RINGCON_CMD_FORMAT_RESET) {
        machine->format_reset = true;
    } else if (machine->command == RINGCON_CMD_POLL_OFF) {
        machine->polling_set = false;
    }

    if (machine->phase == SDL_RINGCON_PHASE_START && machine->command == RINGCON_CMD_POLL &&
        machine->stop_request == SDL_RINGCON_STOP_NONE) {
        machine->phase = SDL_RINGCON_PHASE_POLLING;
        machine->strain = 0;
        machine->rest = 0;
        machine->alive_ms = now_ms;
        out->polling_changed = true;
        out->polling = true;
    }
}

void SDL_RingCon_OnFullReport(SDL_RingConMachine *machine, const uint8_t *report, size_t length, uint64_t now_ms, SDL_RingConStrain *out)
{
    int16_t value;

    memset(out, 0, sizeof(*out));
    if (machine->phase != SDL_RINGCON_PHASE_POLLING || !SDL_RingCon_DecodeStrain(report, length, &value)) {
        return;
    }

    machine->strain = value;
    machine->alive_ms = now_ms;
    machine->reprobe_spent = false;
    out->post = true;
    out->value = value;
    if (machine->rest == 0) {
        machine->rest = value;
        out->rest_changed = true;
        out->rest = value;
    }
}

bool SDL_RingCon_Engaged(const SDL_RingConMachine *machine)
{
    return machine->phase != SDL_RINGCON_PHASE_IDLE;
}

int SDL_RingCon_ImuPostOrder(const SDL_RingConMachine *machine, int order[3])
{
    if (machine && machine->format_set) {
        order[0] = 1;
        order[1] = 0;
        return 2;
    }
    order[0] = 2;
    order[1] = 1;
    order[2] = 0;
    return 3;
}

int SDL_RingCon_BuildCloseCommands(const SDL_RingConMachine *machine, SDL_RingConCommand out[3])
{
    int count = 0;

    if (machine->phase == SDL_RINGCON_PHASE_IDLE && !machine->format_set && !machine->polling_set) {
        return 0;
    }
    if (machine->polling_set) {
        BuildCommand(RINGCON_CMD_POLL_OFF, 0, &out[count++]);
    }
    if (machine->format_set) {
        BuildCommand(RINGCON_CMD_FORMAT_RESET, 0, &out[count++]);
    }
    BuildCommand(RINGCON_CMD_MCU_SUSPEND, 0, &out[count++]);
    return count;
}

int SDL_RingCon_AxisCount(bool right_joycon, int naxes, int gamepad_axis_count)
{
    int wanted = right_joycon ? (gamepad_axis_count + 2) : gamepad_axis_count;

    return (naxes > wanted) ? naxes : wanted;
}
