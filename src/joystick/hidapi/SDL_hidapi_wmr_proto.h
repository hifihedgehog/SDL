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

/* The Windows Mixed Reality motion controllers on Bluetooth: the first
 * generation 045E:065B, the Samsung Odyssey 045E:065D and the HP Reverb G2
 * 045E:066A, each left or right by its product string. Pure C99: no SDL
 * runtime and no I/O, time from an injected clock.
 *
 * The start-up follows Monado's wmr_controller_base.c. Commands are 64-byte
 * output reports 6: a reset, a quiesce, reads of firmware blocks 0 and 3
 * and of configuration block 2, then two commands that turn on the status
 * reports and the IMU. Each answer is a 78-byte input report whose first
 * byte is the response code. Block 2 is two bytes and then JSON XOR-ed with
 * the key in SDL_hidapi_wmr_key.c, and the JSON gives each sensor a mixing
 * matrix, a bias and a rotation. Status report 1 carries the controls, one
 * accelerometer and gyroscope sample and a timestamp in 100 ns ticks.
 *
 * hid.dll returns every input report at the length of the collection's
 * longest input report. With padded set, as on Windows, a report is taken
 * at its own length or longer and the rest is ignored. Otherwise its length
 * must match, as Monado requires.
 */

#ifndef SDL_hidapi_wmr_proto_h_
#define SDL_hidapi_wmr_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_WMR_LEFT_PRODUCT      "Motion controller - Left"
#define SDL_WMR_RIGHT_PRODUCT     "Motion controller - Right"
#define SDL_WMR_STATUS_REPORT_ID  0x01
#define SDL_WMR_STATUS_LENGTH     45 /* The report ID and 44 bytes */
#define SDL_WMR_COMMAND_PREFIX    0x06
#define SDL_WMR_COMMAND_LENGTH    64
#define SDL_WMR_REPLY_LENGTH      78
#define SDL_WMR_REPLY_DATA_LENGTH 68
#define SDL_WMR_REPLY_TIMEOUT_MS  250
#define SDL_WMR_FOLLOWUP_DELAY_MS 10
#define SDL_WMR_KEY_LENGTH        1024
#define SDL_WMR_BLOCK0_MINIMUM    0x36 /* The calibration size ends here */
#define SDL_WMR_BLOCK3_MINIMUM    0x94 /* The serial number ends here */
#define SDL_WMR_BLOCK_MAXIMUM     (2 + 0xFFFF) /* Block 2's header and a calibration of block 0's 16-bit size */
#define SDL_WMR_NS_PER_TICK       100
#define SDL_WMR_MAX_AXES          5
#define SDL_WMR_BUTTONS           6

extern const uint8_t SDL_WMR_ConfigKey[SDL_WMR_KEY_LENGTH];

typedef enum SDL_WMRModel
{
    SDL_WMR_MODEL_FIRST_GENERATION, /* 065B and the Odyssey's 065D */
    SDL_WMR_MODEL_REVERB_G2         /* 066A */
} SDL_WMRModel;

typedef enum SDL_WMRHand
{
    SDL_WMR_HAND_NONE,
    SDL_WMR_HAND_LEFT,
    SDL_WMR_HAND_RIGHT
} SDL_WMRHand;

extern bool SDL_WMR_IsControllerID(uint16_t vendor, uint16_t product, SDL_WMRModel *model);

/* The whole product string must match, as Monado compares it */
extern SDL_WMRHand SDL_WMR_GetHand(const char *product);

typedef struct SDL_WMRInput
{
    uint8_t buttons;      /* Byte 0 after the report ID, as sent */
    uint16_t stick_x;     /* 12 bits, center 0x7FF */
    uint16_t stick_y;
    uint8_t trigger;
    uint8_t touchpad_x;   /* First generation: 0-0x64, 0xFF while untouched */
    uint8_t touchpad_y;
    uint8_t grip;         /* Reverb G2 */
    uint8_t face_buttons; /* Reverb G2: 0x02 X or A, 0x01 Y or B */
    uint8_t battery;
    int32_t accel[3];     /* 49000 per m/s^2 */
    int16_t temperature;
    int32_t gyro[3];      /* 1e-5 rad/s */
    uint32_t timestamp;   /* 100 ns ticks, wrapping */
} SDL_WMRInput;

extern bool SDL_WMR_DecodeStatus(const uint8_t *report, size_t length, bool padded, SDL_WMRModel model, SDL_WMRInput *out);

/* The joystick: axes 0 stick X, 1 stick Y, 2 trigger, then 3 and 4 touchpad
 * X and Y (first generation) or 3 grip (Reverb G2). Buttons 0-3 stick press,
 * Windows, menu and grip, then 4 and 5 touchpad press and touch (first
 * generation) or X/A and Y/B (Reverb G2). Y axes are negated so that up is
 * negative, as on every SDL stick. */
typedef struct SDL_WMRState
{
    int num_axes;
    int16_t axes[SDL_WMR_MAX_AXES];
    bool buttons[SDL_WMR_BUTTONS];
} SDL_WMRState;

extern int SDL_WMR_GetNumAxes(SDL_WMRModel model);
extern void SDL_WMR_GetState(SDL_WMRModel model, const SDL_WMRInput *input, SDL_WMRState *state);

/* 0 is -32768, 0x7FF is 0 and 0xFFF is 32767, or the reverse when inverted */
extern int16_t SDL_WMR_StickAxis(uint16_t value, bool invert);

/* 0 is -32768, 0x32 is 0 and 0x64 or more 32767, reversed when inverted,
   and 0 while untouched */
extern int16_t SDL_WMR_TouchpadAxis(uint8_t value, bool invert);

/* One sensor's calibration. The rotation turns the sensor's WMR axes, X
 * right, Y down and Z forward, as Monado reads "Rt" "Rotation" row by row. */
typedef struct SDL_WMRCalibration
{
    float mix[3][3];
    float bias[3];
    float rotation[3][3];
} SDL_WMRCalibration;

/* Identity mix and rotation and no bias, Monado's defaults */
extern void SDL_WMR_InitCalibration(SDL_WMRCalibration *calibration);

/* Monado's order: mix, add the bias, then turn into SDL's axes, X right,
 * Y up and Z toward the user, which are OpenXR's: out = F R^T (M v + b)
 * with F = diag(1, -1, -1). */
extern void SDL_WMR_Calibrate(const SDL_WMRCalibration *calibration, const float value[3], float out[3]);
extern void SDL_WMR_AccelFromRaw(const int32_t raw[3], float out[3]);
extern void SDL_WMR_GyroFromRaw(const int32_t raw[3], float out[3]);

/* Undoes the key, byte i XOR-ed with key[i % 1024] */
extern void SDL_WMR_Deobfuscate(uint8_t *data, size_t length);

/* Reads the calibration from configuration JSON as Monado does, the text
 * ending at the first NUL or at length. Fails where Monado fails: when the
 * JSON is malformed, the root is no object, "CalibrationInformation" is no
 * object, or its "InertialSensors" or "ControllerLeds" is no array. A
 * sensor entry Monado cannot read changes what Monado would have set
 * before stopping, and nothing after. */
extern bool SDL_WMR_ParseConfig(const char *text, size_t length, SDL_WMRCalibration *accel, SDL_WMRCalibration *gyro);

/* Extends the 32-bit tick count, adding 2^32 each time it goes backward */
extern uint64_t SDL_WMR_ExtendTimestamp(uint64_t *ticks, uint32_t timestamp);

typedef struct SDL_WMRSink
{
    void *userdata;
    /* Writes a 64-byte command, first byte the report ID */
    bool (*write)(void *userdata, const uint8_t *data, size_t length);
} SDL_WMRSink;

typedef enum SDL_WMRPhase
{
    SDL_WMR_PHASE_WAITING, /* A command sent, its answer awaited until the deadline */
    SDL_WMR_PHASE_DELAY,   /* The next block read due at the deadline */
    SDL_WMR_PHASE_READY,   /* Status reports on */
    SDL_WMR_PHASE_FAILED
} SDL_WMRPhase;

typedef enum SDL_WMRFailure
{
    SDL_WMR_FAILURE_NONE,
    SDL_WMR_FAILURE_WRITE,   /* A command could not be written */
    SDL_WMR_FAILURE_TIMEOUT, /* No answer within 250 ms */
    SDL_WMR_FAILURE_REPLY,   /* An answer of the wrong length, command or data length */
    SDL_WMR_FAILURE_BLOCK,   /* A block empty, too large or too short, or a follow-up answer without data */
    SDL_WMR_FAILURE_CONFIG   /* Configuration JSON Monado refuses */
} SDL_WMRFailure;

typedef enum SDL_WMRStep
{
    SDL_WMR_STEP_RESET,
    SDL_WMR_STEP_QUIESCE,
    SDL_WMR_STEP_BLOCK0,
    SDL_WMR_STEP_BLOCK3,
    SDL_WMR_STEP_BLOCK2,
    SDL_WMR_STEP_DONE
} SDL_WMRStep;

typedef struct SDL_WMRSession
{
    SDL_WMRModel model;
    bool padded;
    SDL_WMRPhase phase;
    SDL_WMRFailure failure;
    SDL_WMRStep step;
    uint8_t command[SDL_WMR_COMMAND_LENGTH]; /* The command whose answer is awaited */
    uint8_t response_code;
    uint64_t deadline_ms;
    uint32_t block_size;
    uint32_t block_received;
    uint8_t info[SDL_WMR_BLOCK3_MINIMUM]; /* The start of block 0 or 3 */
    uint8_t *config;                      /* The caller's buffer for block 2 */
    size_t config_capacity;
    uint32_t firmware_revision;
    uint16_t calibration_size;
    char serial[17];
    SDL_WMRCalibration accel;
    SDL_WMRCalibration gyro;
    uint64_t ticks;
} SDL_WMRSession;

/* Starts or restarts the start-up with the reset command. Block 2 goes into
 * config, which must hold SDL_WMR_BLOCK_MAXIMUM bytes or the open fails on a
 * larger block. */
extern void SDL_WMR_Open(SDL_WMRSession *session, SDL_WMRModel model, bool padded,
                         uint8_t *config, size_t config_capacity, uint64_t now_ms, const SDL_WMRSink *sink);

/* Fails the start-up 250 ms after a command without its answer, and sends
 * each follow-up block read 10 ms after the answer before it. Call it after
 * handing over the reports read. */
extern void SDL_WMR_Update(SDL_WMRSession *session, uint64_t now_ms, const SDL_WMRSink *sink);

typedef enum SDL_WMRReport
{
    SDL_WMR_REPORT_IGNORED,
    SDL_WMR_REPORT_STARTUP, /* An answer the start-up took, or refused and failed on */
    SDL_WMR_REPORT_STATUS   /* A status report, decoded into the event */
} SDL_WMRReport;

typedef struct SDL_WMREvent
{
    SDL_WMRInput input;
    SDL_WMRState state;
    float accel[3];        /* m/s^2, calibrated, SDL axes */
    float gyro[3];         /* rad/s, calibrated, SDL axes */
    uint64_t timestamp_ns; /* The extended tick count in nanoseconds */
} SDL_WMREvent;

/* Before the start-up is done, only the awaited answer counts: its response
 * code at byte 0, 78 bytes, the command echoed at byte 2 and at most 68 data
 * bytes, or the open fails. Anything else is skipped. After it, only status
 * reports count, decoded into event, which must not be NULL. */
extern SDL_WMRReport SDL_WMR_HandleReport(SDL_WMRSession *session, uint64_t now_ms,
                                          const uint8_t *report, size_t length,
                                          const SDL_WMRSink *sink, SDL_WMREvent *event);

#endif /* SDL_hidapi_wmr_proto_h_ */
