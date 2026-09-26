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

/* Tacx USB trainer head units, hifihedgehog/SDL#33 Part 14. Pure C99: no
 * SDL runtime and no I/O, so every decision here runs in the offline tests
 * exactly as it runs in the library.
 *
 * The T1904 (3561:1904) and the T1932 (3561:1932) share one protocol on
 * interface 0 with the T1942 once other software has loaded its firmware,
 * whose capture the tests replay. The host writes to OUT 0x02 and reads IN
 * 0x82. It first sends the version request 02 00 00 00, without which the
 * head unit reports no cadence, then a 12-byte frame every 100 ms. The head
 * unit asks the brake only after a frame from the host, and each brake
 * answer comes back in a reply of 64 bytes from the T1904 and T1932, or 48
 * from the T1942. Bytes 24 to 27 of a reply are its header: 00021303 for
 * data, 00000C03 for the brake's version. Multi-byte fields are
 * little-endian. A 24-byte reply carries no brake answer and is ignored.
 *
 * The frames this module hands out are the version request and the stop
 * frame, mode 00 with target 0, which FortiusANT sends while it is idle and
 * TotalReverse's brake tool sends to stop the brake. Resistance control
 * belongs to the application. The T1902 (3561:1902, and 0547:2131 before
 * its firmware) and the T1942 (3561:E6BE before its firmware, 3561:1942
 * after it) need a Tacx firmware image from the host at every power-up.
 * The part leaves them outside the ticket, so they are not handled.
 *
 * The facts are restated from FortiusANT (GPL-3.0), TotalReverse's ttyT1941
 * and its wiki (GPL-3.0) and the fortius_1942 Linux driver (GPL-2.0), all
 * for facts only, and from antifier's captures (MIT). No code from them is
 * copied.
 */

#ifndef SDL_hidapi_tacx_proto_h_
#define SDL_hidapi_tacx_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_TACX_VENDOR        0x3561
#define SDL_TACX_PRODUCT_T1904 0x1904
#define SDL_TACX_PRODUCT_T1932 0x1932

typedef enum SDL_TacxModel
{
    SDL_TACX_NONE,
    SDL_TACX_T1904,
    SDL_TACX_T1932
} SDL_TacxModel;

/* The head unit this ID names, or SDL_TACX_NONE. The T1902, 0547:2131
 * and the T1942, before or after its firmware, are SDL_TACX_NONE. */
extern SDL_TacxModel SDL_Tacx_Identify(uint16_t vendor, uint16_t product);

/* "Tacx T1904" or "Tacx T1932", or NULL */
extern const char *SDL_Tacx_Name(SDL_TacxModel model);

#define SDL_TACX_FRAME_LENGTH   12 /* A host frame */
#define SDL_TACX_VERSION_LENGTH 4  /* The version request */

/* Modes of a host frame */
#define SDL_TACX_MODE_STOP       0x00
#define SDL_TACX_MODE_RESISTANCE 0x02
#define SDL_TACX_MODE_CALIBRATE  0x03

/* A host frame: 01 08 01 00, the target (signed), the pedal echo, 00, the
 * mode, the flywheel weight in kg and the calibration. */
extern void SDL_Tacx_EncodeFrame(int16_t target, uint8_t pedal_echo, uint8_t mode, uint8_t weight,
                                 uint16_t calibration, uint8_t out[SDL_TACX_FRAME_LENGTH]);

/* 02 00 00 00 */
extern void SDL_Tacx_VersionRequest(uint8_t out[SDL_TACX_VERSION_LENGTH]);

#define SDL_TACX_READ_SIZE     64 /* The longest reply */
#define SDL_TACX_REPLY_MINIMUM 48 /* The shortest reply that is decoded */

#define SDL_TACX_HEADER_DATA    0x00021303u
#define SDL_TACX_HEADER_VERSION 0x00000C03u

/* A data reply */
typedef struct SDL_TacxData
{
    uint16_t unit_serial; /* Bytes 0 and 1, the head unit's serial */
    uint8_t unit_year;    /* Byte 8, the head unit's production year */
    uint8_t heart_rate;   /* Byte 12, beats per minute */
    uint8_t buttons;      /* Byte 13: 1 Enter, 2 Down, 4 Up, 8 Cancel */
    uint16_t axes[4];     /* Bytes 16 to 23. Axis 1 is the steering, 0A0D with no steering unit on a T1904 or T1932. */
    uint32_t distance;    /* Bytes 28 to 31 */
    uint16_t wheel_speed; /* Bytes 32 and 33. FortiusANT reads km/h as the value / 289.75. */
    int16_t resistance;   /* Bytes 38 and 39, the current resistance */
    int16_t target;       /* Bytes 40 and 41, the target the brake echoes */
    uint8_t events;       /* Byte 42: 1 a pedal pass, 4 the brake stopped */
    uint8_t cadence;      /* Byte 44, revolutions per minute */
    uint8_t mode;         /* Byte 46, the mode echo */
} SDL_TacxData;

/* The brake's version reply */
typedef struct SDL_TacxBrake
{
    uint32_t firmware; /* Bytes 28 to 31 */
    uint32_t serial;   /* Bytes 32 to 35 */
    uint16_t version2; /* Bytes 36 and 37 */
    uint8_t type;      /* The first two decimal digits of the serial: 41 is the T1941 motor brake */
    uint8_t year;      /* The next two */
    uint32_t number;   /* The digits after them */
    bool magnetic;     /* Serial 0, which a head unit reports for a magnetic brake */
} SDL_TacxBrake;

/* Decode one reply of the given length. Each reads nothing at or past the
 * length and returns false, changing nothing, for a reply under 48 bytes or
 * with the other header. Bytes 47 and 48 are never read. */
extern bool SDL_Tacx_DecodeData(const uint8_t *reply, size_t length, SDL_TacxData *data);
extern bool SDL_Tacx_DecodeBrake(const uint8_t *reply, size_t length, SDL_TacxBrake *brake);

/* The joystick. The axes are clamped to 16 bits:
 * - 0: the steering, axis 1 - 32768. The raw value covers all 16 bits.
 * - 1: the wheel speed, up to 32767, about 113 km/h by FortiusANT's factor.
 * - 2: the cadence x 128, at most 32640.
 * - 3: the heart rate x 128, at most 32640.
 * - 4: the current resistance, signed, as the head unit reports it. */
#define SDL_TACX_AXIS_STEERING    0
#define SDL_TACX_AXIS_WHEEL_SPEED 1
#define SDL_TACX_AXIS_CADENCE     2
#define SDL_TACX_AXIS_HEART_RATE  3
#define SDL_TACX_AXIS_RESISTANCE  4
#define SDL_TACX_AXES             5

#define SDL_TACX_BUTTON_ENTER  0
#define SDL_TACX_BUTTON_DOWN   1
#define SDL_TACX_BUTTON_UP     2
#define SDL_TACX_BUTTON_CANCEL 3
#define SDL_TACX_BUTTONS       4

typedef struct SDL_TacxControls
{
    int16_t axes[SDL_TACX_AXES];
    uint8_t buttons; /* Bit i is button i */
} SDL_TacxControls;

extern void SDL_Tacx_GetControls(const SDL_TacxData *data, SDL_TacxControls *controls);

/* Timing on the caller's clock, in nanoseconds. A frame goes out every
 * 100 ms, above the 70 ms the brake needs to answer. Until the brake's
 * version reply arrives, the version request replaces the frame every
 * 500 ms, six times at most, as FortiusANT sends it up to six times. The
 * head unit counts as lost after 1000 ms without a data reply. */
#define SDL_TACX_FRAME_INTERVAL_NS (100 * 1000000ULL)
#define SDL_TACX_VERSION_RETRY_NS  (500 * 1000000ULL)
#define SDL_TACX_VERSION_REQUESTS  6
#define SDL_TACX_TIMEOUT_NS        (1000 * 1000000ULL)

typedef struct SDL_TacxState
{
    uint64_t frame_at;      /* No frame is handed out before this */
    uint64_t version_at;    /* No version request is handed out before this */
    int version_requests;   /* Written since the last round began */
    bool version_known;     /* A version reply arrived since the head unit was last lost */
    bool in_flight;         /* A frame was handed out and not yet completed */
    bool in_flight_version; /* That frame is the version request */
    bool present;           /* A data reply arrived within the timeout */
    uint64_t data_at;       /* When the last data reply arrived */
    SDL_TacxData data;      /* The last data reply */
    SDL_TacxBrake brake;    /* The last version reply */
} SDL_TacxState;

/* What a call changed */
#define SDL_TACX_CHANGED_PRESENT  0x01 /* The head unit appeared or was lost */
#define SDL_TACX_CHANGED_CONTROLS 0x02 /* A data reply arrived */
#define SDL_TACX_CHANGED_BRAKE    0x04 /* A new version reply arrived */

/* The head unit is absent, and the first frame, the version request, is
 * due now. */
extern void SDL_Tacx_Init(SDL_TacxState *state, uint64_t now);

/* Hands out the next frame once it is due and none is in flight: the
 * version request, 4 bytes, or the stop frame, 12. */
extern bool SDL_Tacx_NextFrame(SDL_TacxState *state, uint64_t now, uint8_t out[SDL_TACX_FRAME_LENGTH], size_t *length);

/* Completes the frame in flight. written is the byte count the write
 * reported, or a negative value when it failed. The next frame is due one
 * interval later. A version request counts only when all 4 bytes went out,
 * so a failed one goes again with the next frame. */
extern void SDL_Tacx_FrameDone(SDL_TacxState *state, int written, uint64_t now);

/* Applies one reply and returns SDL_TACX_CHANGED_* flags. A data reply
 * makes the head unit present. When it appears after a round of version
 * requests went unanswered, a new round begins. */
extern int SDL_Tacx_HandleReply(SDL_TacxState *state, const uint8_t *reply, size_t length, uint64_t now);

/* Loses the head unit 1000 ms after its last data reply. The brake is then
 * unknown, and a new round of version requests begins. Returns
 * SDL_TACX_CHANGED_PRESENT when that happens. */
extern int SDL_Tacx_Tick(SDL_TacxState *state, uint64_t now);

#endif /* SDL_hidapi_tacx_proto_h_ */
