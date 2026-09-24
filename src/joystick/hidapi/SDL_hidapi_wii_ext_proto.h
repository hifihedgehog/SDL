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

/* Wii Remote extensions beyond the Nunchuk, Classic Controller, Wii U Pro and
 * Balance Board: the Guitar Hero guitars, the World Tour drum kit, the DJ Hero
 * turntable, the Taiko TaTaCon, the uDraw and Drawsome tablets and the
 * Densha de GO! Shinkansen controller. Pure C99: no SDL runtime and no I/O.
 * SDL_hidapi_wii.c sends the requests built here and posts what the decoders
 * return.
 *
 * The byte layouts are facts from wiibrew, Dolphin, Linux, WiitarThing,
 * NintendoExtensionCtrl, PlasticBand, raphnet and uDrawTablet. No code from
 * any of them is copied.
 */

#ifndef SDL_hidapi_wii_ext_proto_h_
#define SDL_hidapi_wii_ext_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Extension types, the values of EWiiExtensionControllerType. The driver
 * copies them into GUID byte 15, so each configuration has its own GUID. */
#define SDL_WII_EXT_UNKNOWN      0
#define SDL_WII_EXT_NONE         128
#define SDL_WII_EXT_NUNCHUK      129
#define SDL_WII_EXT_GAMEPAD      130
#define SDL_WII_EXT_WIIUPRO      131
#define SDL_WII_EXT_BALANCEBOARD 132
#define SDL_WII_EXT_GUITAR       133
#define SDL_WII_EXT_DRUMS        134
#define SDL_WII_EXT_TURNTABLE    135
#define SDL_WII_EXT_TAIKO        136
#define SDL_WII_EXT_UDRAW        137
#define SDL_WII_EXT_DRAWSOME     138
#define SDL_WII_EXT_SHINKANSEN   139

/* Whether this module decodes the type. */
extern bool SDL_WiiExt_IsDecodedType(int type);

/* Classifies extension ID bytes 0, 4 and 5, the three bytes both the direct
 * identify and a Motion Plus's stored copy deliver. With decoding off, every
 * type this module adds classifies as unknown, as before it existed. The
 * active Motion Plus, uninitialized and absent cases are the driver's. */
extern int SDL_WiiExt_Classify(uint8_t id0, uint8_t id4, uint8_t id5, bool decoding_enabled);

/* Register addresses, with the 0x04 register-space selector left out as the
 * driver's requests carry it in byte 1. */
#define SDL_WII_EXT_REG_IDENTIFY         0xA400FA /* 6 bytes: the full extension ID */
#define SDL_WII_EXT_REG_MOTIONPLUS_PROBE 0xA600FA /* 6 bytes: an inactive Motion Plus */
#define SDL_WII_EXT_REG_STORED_ID        0xA400F6 /* 4 bytes behind an active Motion Plus */
#define SDL_WII_EXT_REG_INIT1            0xA400F0 /* 0x55 */
#define SDL_WII_EXT_REG_INIT2            0xA400FB /* 0x00, the Euphoria LED on the turntable */

#define SDL_WII_EXT_READ_REQUEST_SIZE  7
#define SDL_WII_EXT_WRITE_REQUEST_SIZE 22
#define SDL_WII_EXT_MODE_REQUEST_SIZE  3

/* Output report 0x17: read size bytes at address. */
extern void SDL_WiiExt_BuildReadRequest(uint8_t out[SDL_WII_EXT_READ_REQUEST_SIZE], bool rumble, uint32_t address, uint16_t size);

/* Output report 0x16: write 1 to 16 bytes at address, zero padded to 22 bytes. */
extern bool SDL_WiiExt_BuildWriteRequest(uint8_t out[SDL_WII_EXT_WRITE_REQUEST_SIZE], bool rumble, uint32_t address, const uint8_t *data, size_t size);

/* Output report 0x12: continuous reporting in the given data report. */
extern void SDL_WiiExt_BuildModeRequest(uint8_t out[SDL_WII_EXT_MODE_REQUEST_SIZE], bool rumble, uint8_t report);

/* Output report 0x15: a status report, which also resets the data report. */
#define SDL_WII_EXT_STATUS_REQUEST_SIZE 2
extern void SDL_WiiExt_BuildStatusRequest(uint8_t out[SDL_WII_EXT_STATUS_REQUEST_SIZE], bool rumble);

typedef enum SDL_WiiExtReply
{
    SDL_WII_EXT_REPLY_OK,       /* The requested bytes */
    SDL_WII_EXT_REPLY_ABSENT,   /* Error 7: no extension answers the address */
    SDL_WII_EXT_REPLY_REJECTED  /* Another reply, a stale one, an error or a short read */
} SDL_WiiExtReply;

/* A 0x21 reply to a 6-byte identify at the given address. It must echo the
 * address's low 16 bits and carry SE = 0x50: 6 bytes, no error. */
extern SDL_WiiExtReply SDL_WiiExt_ParseIdentifyReply(const uint8_t *report, size_t length, uint32_t address, uint8_t id[6]);

/* A 0x21 reply to the 4-byte read at 0xA400F6: ID byte 4, the challenge
 * state, ID byte 0 and ID byte 5. */
extern bool SDL_WiiExt_ParseStoredIdReply(const uint8_t *report, size_t length, uint8_t *id0, uint8_t *id4, uint8_t *id5);

/* Whether a Motion Plus may carry this type in passthrough. The types this
 * module adds never run behind an active Motion Plus: passthrough drops or
 * moves bits they use, and the Shinkansen's 8 bytes cannot pass at all. */
extern bool SDL_WiiExt_AllowsMotionPlus(int type);

/* The data report a type streams in: 0x35 for the guitar with sensors on,
 * 0x32 otherwise. */
extern uint8_t SDL_WiiExt_ReportMode(int type, bool sensors);

/* Writes a type needs after the standard init (0x55 to 0xF0, then 0x00 to
 * 0xFB) before it streams, in order. */
typedef struct SDL_WiiExtWrite
{
    uint32_t address;
    uint8_t value;
} SDL_WiiExtWrite;

#define SDL_WII_EXT_MAX_STARTUP_WRITES 2
extern size_t SDL_WiiExt_StartupWrites(int type, SDL_WiiExtWrite writes[SDL_WII_EXT_MAX_STARTUP_WRITES]);

/* Sends the start-up writes in order. write() sends one and returns whether
 * its acknowledge came back. The first failure stops the sequence. Returns
 * whether every write was acknowledged. */
extern bool SDL_WiiExt_RunStartup(int type, bool (*write)(void *userdata, uint32_t address, uint8_t value), void *userdata);

/* The turntable's Euphoria LED: 0x01 to 0xA400FB for on, 0x00 for off. */
extern void SDL_WiiExt_BuildLEDWrite(uint8_t out[SDL_WII_EXT_WRITE_REQUEST_SIZE], bool rumble, bool on);

/* The layout of a data report, 0x30 to 0x37 and 0x3D. The interleaved 0x3E
 * and 0x3F are not decoded. */
typedef struct SDL_WiiExtLayout
{
    size_t length;      /* The full report, report ID included */
    bool buttons;       /* The remote's buttons in bytes 1-2 */
    bool accel;         /* The remote's accelerometer in bytes 3-5 */
    size_t span_offset; /* The extension span, 0 bytes when absent */
    size_t span_length;
} SDL_WiiExtLayout;

extern bool SDL_WiiExt_GetLayout(uint8_t report_id, SDL_WiiExtLayout *layout);

/* A data report's extension span. False when the report is shorter than its
 * full length or carries no span. has_accel reports whether the report
 * carries the remote's accelerometer bytes. Nothing at or past length is
 * read. */
extern bool SDL_WiiExt_GetSpan(const uint8_t *report, size_t length, const uint8_t **span, size_t *span_length, bool *has_accel);

/* What SDL sees for a type. */
#define SDL_WII_EXT_JOYSTICK_TYPE_UNKNOWN  0 /* SDL_JOYSTICK_TYPE_UNKNOWN */
#define SDL_WII_EXT_JOYSTICK_TYPE_GAMEPAD  1 /* SDL_JOYSTICK_TYPE_GAMEPAD */
#define SDL_WII_EXT_JOYSTICK_TYPE_GUITAR   6 /* SDL_JOYSTICK_TYPE_GUITAR */
#define SDL_WII_EXT_JOYSTICK_TYPE_DRUM_KIT 7 /* SDL_JOYSTICK_TYPE_DRUM_KIT */

typedef struct SDL_WiiExtCaps
{
    const char *name;
    int joystick_type;
    uint8_t nbuttons;
    uint8_t naxes;
    uint8_t nhats;
    bool accel;    /* The remote's accelerometer as SDL_SENSOR_ACCEL */
    bool mono_led; /* SDL_JOYSTICK_CAP_MONO_LED */
    bool mapping;  /* Whether the gamepad layer maps it */
} SDL_WiiExtCaps;

extern bool SDL_WiiExt_GetCaps(int type, SDL_WiiExtCaps *caps);

/* The device name and joystick type for every configuration, the ones that
 * existed before this module included. */
extern const char *SDL_WiiExt_TypeName(int type);
extern int SDL_WiiExt_JoystickType(int type);

/* Joystick button and axis numbers, SDL's gamepad positions. */
#define SDL_WII_EXT_BUTTON_SOUTH          0
#define SDL_WII_EXT_BUTTON_EAST           1
#define SDL_WII_EXT_BUTTON_WEST           2
#define SDL_WII_EXT_BUTTON_NORTH          3
#define SDL_WII_EXT_BUTTON_BACK           4
#define SDL_WII_EXT_BUTTON_START          6
#define SDL_WII_EXT_BUTTON_LEFT_STICK     7
#define SDL_WII_EXT_BUTTON_RIGHT_STICK    8
#define SDL_WII_EXT_BUTTON_LEFT_SHOULDER  9
#define SDL_WII_EXT_BUTTON_RIGHT_SHOULDER 10
#define SDL_WII_EXT_BUTTON_TURNTABLE_LEFT_GREEN 26 /* Then left red, left blue, right green, right red, right blue */

#define SDL_WII_EXT_AXIS_LEFTX         0
#define SDL_WII_EXT_AXIS_LEFTY         1
#define SDL_WII_EXT_AXIS_RIGHTX        2
#define SDL_WII_EXT_AXIS_RIGHTY        3
#define SDL_WII_EXT_AXIS_LEFT_TRIGGER  4
#define SDL_WII_EXT_AXIS_RIGHT_TRIGGER 5
#define SDL_WII_EXT_MAX_AXES           16

/* Hat bits, SDL's values. */
#define SDL_WII_EXT_HAT_UP    0x01
#define SDL_WII_EXT_HAT_RIGHT 0x02
#define SDL_WII_EXT_HAT_DOWN  0x04
#define SDL_WII_EXT_HAT_LEFT  0x08

typedef struct SDL_WiiExtStick
{
    uint16_t min;
    uint16_t max;
    uint16_t center; /* 0 until the first sample sets it */
    uint16_t deadzone;
} SDL_WiiExtStick;

typedef struct SDL_WiiExtState
{
    int type;
    SDL_WiiExtStick stick[2];
    bool enabled; /* Start-up writes acknowledged, or none needed */

    /* Guitar */
    bool whammy_seen;
    uint8_t whammy_lo;
    uint8_t whammy_largest;
    bool touch_seen; /* The touch bar read something other than 0x1F */

    /* Drums: green, red, blue, yellow, bass, orange */
    int16_t pad_axis[6];
    int16_t hihat_axis;
} SDL_WiiExtState;

typedef struct SDL_WiiExtOutput
{
    uint32_t buttons;     /* Bit n: joystick button n is down */
    uint32_t button_mask; /* The buttons this report posts */
    int16_t axes[SDL_WII_EXT_MAX_AXES];
    uint16_t axis_mask;      /* The axes this report posts */
    uint16_t data_axis_mask; /* Raw data axes, seeded past the anti-jitter gate */
    uint8_t hat;
    bool has_hat;
} SDL_WiiExtOutput;

/* Starts a type from scratch: stick calibration seeds, no whammy or touch
 * history, drum pads released. enabled is true, and the driver clears it when
 * a start-up write goes unacknowledged. */
extern void SDL_WiiExt_Reset(SDL_WiiExtState *state, int type);

/* Decodes one extension span. Returns false when the span changes nothing:
 * decoding is off for the state, the span is shorter than the type needs, or
 * every byte is 0xFF, a failed bus read, which the TaTaCon alone reads as all
 * four surfaces released. */
extern bool SDL_WiiExt_Decode(SDL_WiiExtState *state, const uint8_t *span, size_t length, SDL_WiiExtOutput *out);

#endif /* SDL_hidapi_wii_ext_proto_h_ */
