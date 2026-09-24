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

/* Rock Band 3 Pro instruments: the keyboard, the Mustang and Squier Pro
 * guitars and the MIDI Pro Adapter in its keyboard and guitar modes, on PS3
 * and Wii (HID) and on Xbox 360 (XInput subtypes 15 and 25). Pure C99: no SDL
 * runtime and no I/O. The PS3 third-party driver and the XInput backend post
 * what the decoders return, and the PS3 driver sends the enable built here.
 *
 * The byte layouts are facts from PlasticBand, Linux hid-sony, Dolphin,
 * RPCS3 and AutoCalibrationRB. No code from any of them is copied.
 */

#ifndef SDL_rb3pro_proto_h_
#define SDL_rb3pro_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_RB3PRO_NONE     0
#define SDL_RB3PRO_KEYBOARD 1
#define SDL_RB3PRO_GUITAR   2

typedef struct SDL_RB3ProDevice
{
    int variant;      /* SDL_RB3PRO_KEYBOARD or SDL_RB3PRO_GUITAR */
    bool ps3;         /* A PS3 model, which sends keys and frets only after the enable */
    const char *name;
} SDL_RB3ProDevice;

/* The twelve PS3 and Wii IDs. false for any other device. */
extern bool SDL_RB3Pro_GetDevice(uint16_t vendor, uint16_t product, SDL_RB3ProDevice *device);

/* The XInput subtypes, outside the XInput set */
#define SDL_RB3PRO_XINPUT_SUBTYPE_KEYBOARD 0x0F
#define SDL_RB3PRO_XINPUT_SUBTYPE_GUITAR   0x19
extern int SDL_RB3Pro_VariantForXInputSubtype(uint8_t subtype);
extern const char *SDL_RB3Pro_XInputName(int variant);

/* Joystick layout. Buttons 0 to 10 are in SDL gamepad order: 0 cross, 1
 * circle, 2 square, 3 triangle, 4 select, 5 PS, 6 start, 7 to 10 never
 * pressed.
 *
 * Keyboard: 11 overdrive, 12 digital pedal, 13 to 37 the keys C1 to C3, 38
 * pedal connected. Axes 0 to 4 are velocity slots 1 to 5, 5 the analog
 * pedal, 6 the touch strip.
 *
 * Guitar: 11 solo flag, 12 pedal, 13 to 17 the green, red, yellow, blue and
 * orange flags, 18 pedal connected. Axes 0 to 5 are the fret numbers from low
 * E to high E, 6 to 11 the string velocities in the same order, 12 tilt, 13
 * the microphone sensor, 14 the light sensor.
 *
 * Hat 0 is the d-pad.
 */
#define SDL_RB3PRO_BUTTON_SOUTH  0
#define SDL_RB3PRO_BUTTON_EAST   1
#define SDL_RB3PRO_BUTTON_WEST   2
#define SDL_RB3PRO_BUTTON_NORTH  3
#define SDL_RB3PRO_BUTTON_BACK   4
#define SDL_RB3PRO_BUTTON_GUIDE  5
#define SDL_RB3PRO_BUTTON_START  6
#define SDL_RB3PRO_GAMEPAD_BUTTONS 11

#define SDL_RB3PRO_KEYBOARD_OVERDRIVE       11
#define SDL_RB3PRO_KEYBOARD_PEDAL           12
#define SDL_RB3PRO_KEYBOARD_FIRST_KEY       13
#define SDL_RB3PRO_KEYBOARD_KEYS            25
#define SDL_RB3PRO_KEYBOARD_PEDAL_CONNECTED 38
#define SDL_RB3PRO_KEYBOARD_BUTTONS         39
#define SDL_RB3PRO_KEYBOARD_FIRST_VELOCITY  0
#define SDL_RB3PRO_KEYBOARD_VELOCITIES      5
#define SDL_RB3PRO_KEYBOARD_PEDAL_AXIS      5
#define SDL_RB3PRO_KEYBOARD_TOUCH_STRIP     6
#define SDL_RB3PRO_KEYBOARD_AXES            7

#define SDL_RB3PRO_GUITAR_SOLO            11
#define SDL_RB3PRO_GUITAR_PEDAL           12
#define SDL_RB3PRO_GUITAR_FIRST_COLOR     13
#define SDL_RB3PRO_GUITAR_COLORS          5
#define SDL_RB3PRO_GUITAR_PEDAL_CONNECTED 18
#define SDL_RB3PRO_GUITAR_BUTTONS         19
#define SDL_RB3PRO_GUITAR_FIRST_FRET      0
#define SDL_RB3PRO_GUITAR_FIRST_VELOCITY  6
#define SDL_RB3PRO_GUITAR_STRINGS         6
#define SDL_RB3PRO_GUITAR_TILT            12
#define SDL_RB3PRO_GUITAR_MICROPHONE      13
#define SDL_RB3PRO_GUITAR_LIGHT           14
#define SDL_RB3PRO_GUITAR_AXES            15

#define SDL_RB3PRO_MAX_AXES 15

/* Hat bits, the values of SDL_HAT_UP and its neighbors */
#define SDL_RB3PRO_HAT_CENTERED 0x00
#define SDL_RB3PRO_HAT_UP       0x01
#define SDL_RB3PRO_HAT_RIGHT    0x02
#define SDL_RB3PRO_HAT_DOWN     0x04
#define SDL_RB3PRO_HAT_LEFT     0x08

extern bool SDL_RB3Pro_GetLayout(int variant, int *nbuttons, int *naxes, int *nhats);

/* The SDL gamepad mapping: the face buttons, back, guide, start and the hat */
#define SDL_RB3PRO_MAPPING "a:b0,b:b1,back:b4,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b5,start:b6,x:b2,y:b3,"

typedef struct SDL_RB3ProOutput
{
    uint64_t buttons;     /* Bit i is button i */
    uint64_t button_mask; /* The buttons this report posts */
    int16_t axes[SDL_RB3PRO_MAX_AXES];
    uint16_t axis_mask;   /* The axes this report posts. All of them are data:
                             seed them past the anti-jitter gate. */
    uint8_t hat;
} SDL_RB3ProOutput;

/* The 27-byte PS3 and Wii report, no report ID. 27 bytes or more are
 * accepted and bytes past 26 are ignored. Anything shorter changes nothing. */
#define SDL_RB3PRO_REPORT_LENGTH 27
extern bool SDL_RB3Pro_DecodeReport(int variant, const uint8_t *report, size_t length, SDL_RB3ProOutput *out);

/* The Xbox 360 instruments through XInput. The six bytes that follow
 * sThumbRY in the XUSB report are optional: without them the touch strip,
 * the tilt, both sensors, the guitar's pedal and the pedal connection read
 * as absent, which is 0 and released. */
typedef struct SDL_RB3ProXInputState
{
    uint16_t buttons; /* wButtons */
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t thumb_lx;
    int16_t thumb_ly;
    int16_t thumb_rx;
    int16_t thumb_ry;
    bool has_trailing;
    uint8_t trailing[6];
} SDL_RB3ProXInputState;

extern bool SDL_RB3Pro_DecodeXInput(int variant, const SDL_RB3ProXInputState *state, SDL_RB3ProOutput *out);

/* The enable for the PS3 models: 40 bytes, feature report 0 */
#define SDL_RB3PRO_ENABLE_LENGTH    40
#define SDL_RB3PRO_ENABLE_ROWS      5
#define SDL_RB3PRO_ENABLE_ROW_BYTES 8
#define SDL_RB3PRO_ENABLE_PAUSE_US  1473
#define SDL_RB3PRO_ENABLE_REPEAT_US 8000000

/* The single feature report: report ID 0, then the 40 bytes. */
extern void SDL_RB3Pro_BuildEnable(uint8_t out[1 + SDL_RB3PRO_ENABLE_LENGTH]);

/* The enable schedule, on a microsecond clock the caller supplies.
 *
 * Split is the Windows HID form, where HidD_SetFeature cuts a report to the
 * declared 8 bytes: five feature reports of report ID 0 and one row each,
 * rows 1 and 2 back to back, then a 1473 us pause after each of rows 2 to 5.
 * Unsplit, the caller sends SDL_RB3Pro_BuildEnable once per start.
 *
 * A start happens at open and, following Linux, when a report of 25 bytes or
 * more reads 0x02 in byte 24, which the instruments send while they report
 * navigation only, strictly more than 8 s after the previous start. */
typedef struct SDL_RB3ProEnable
{
    bool armed;
    bool split;
    bool started;
    uint64_t last_start_us;
    int next_row;         /* SDL_RB3PRO_ENABLE_ROWS when no rows are pending */
    uint64_t next_due_us;
} SDL_RB3ProEnable;

extern void SDL_RB3ProEnable_Init(SDL_RB3ProEnable *enable, bool armed, bool split);
extern bool SDL_RB3ProEnable_Open(SDL_RB3ProEnable *enable, uint64_t now_us);
extern bool SDL_RB3ProEnable_OnReport(SDL_RB3ProEnable *enable, const uint8_t *report, size_t length, uint64_t now_us);
#define SDL_RB3PRO_ENABLE_ROW_REPORT_LENGTH (1 + SDL_RB3PRO_ENABLE_ROW_BYTES)
extern bool SDL_RB3ProEnable_NextRow(SDL_RB3ProEnable *enable, uint64_t now_us, uint8_t out[SDL_RB3PRO_ENABLE_ROW_REPORT_LENGTH]);

#endif /* SDL_rb3pro_proto_h_ */
