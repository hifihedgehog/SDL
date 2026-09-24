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

/* PS3 peripherals that send the 27-byte instrument report with their main
 * input in the vendor-page bytes: the uDraw GameTablet (20D6:CB17), the Top
 * Shot Elite and Top Shot Fearmaster light guns (12BA:04A0, 12BA:04A1) and
 * the Tony Hawk RIDE and SHRED skateboards (12BA:0400, 1430:0100). Pure C99:
 * no SDL runtime and no I/O. The PS3 third-party driver posts what the
 * decoders return and sends the requests built here.
 *
 * The byte layouts are facts from brandonw.net, uDrawTablet, Linux
 * hid-udraw-ps3, RPCS3 and Dolphin pull request 11618. No code from any of
 * them is copied. The skateboard's encrypted accelerometer words are not
 * decoded: their decryption exists only in GPL source.
 */

#ifndef SDL_hidapi_ps3ext_proto_h_
#define SDL_hidapi_ps3ext_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_PS3EXT_NONE               0
#define SDL_PS3EXT_UDRAW              1
#define SDL_PS3EXT_TOPSHOT_ELITE      2
#define SDL_PS3EXT_TOPSHOT_FEARMASTER 3
#define SDL_PS3EXT_TONYHAWK           4

typedef struct SDL_PS3ExtDevice
{
    int variant;
    const char *name;
} SDL_PS3ExtDevice;

extern bool SDL_PS3Ext_GetDevice(uint16_t vendor, uint16_t product, SDL_PS3ExtDevice *device);

/* The layout and the SDL gamepad mapping of each variant. Buttons 0 to 6 are
 * in SDL gamepad order: cross, circle, square, triangle, select, PS, start.
 *
 * uDraw: 11 pen touching, 12 one finger, 13 two fingers. Axes 0 X, 1 Y, 2 pen
 * pressure, 3 two-finger distance, and an accelerometer.
 *
 * Top Shot: 7 L3, 8 R3 (Elite), 11 trigger, 12 reload (Elite), 13 left LED
 * seen, 14 right LED seen, 15 heart-rate sensor touched (Fearmaster). Elite
 * axes: 0 and 1 the left stick, 2 and 3 the right stick, 4 to 7 left LED X
 * and Y, right LED X and Y. Fearmaster axes: 0 and 1 the left stick, 2 to 5
 * the LED coordinates, 6 the heart-rate word.
 *
 * Tony Hawk: axes 0 to 3 the nose, tail, left and right IR sensors, 4 and 5
 * the nose and tail single-axis accelerometers.
 *
 * Hat 0 is the d-pad on every variant.
 */
/* The SDL gamepad mappings, also used where only the USB ID is known */
#define SDL_PS3EXT_MAPPING_BASE "a:b0,b:b1,back:b4,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b5,start:b6,x:b2,y:b3,"
#define SDL_PS3EXT_MAPPING_TOPSHOT_ELITE SDL_PS3EXT_MAPPING_BASE "leftstick:b7,leftx:a0,lefty:a1,rightstick:b8,rightx:a2,righty:a3,"
#define SDL_PS3EXT_MAPPING_TOPSHOT_FEARMASTER SDL_PS3EXT_MAPPING_BASE "leftstick:b7,leftx:a0,lefty:a1,"

typedef struct SDL_PS3ExtLayout
{
    int nbuttons;
    int naxes;
    int nhats;
    bool accelerometer;
    const char *mapping;
} SDL_PS3ExtLayout;

extern bool SDL_PS3Ext_GetLayout(int variant, SDL_PS3ExtLayout *layout);

#define SDL_PS3EXT_BUTTON_SOUTH       0
#define SDL_PS3EXT_BUTTON_EAST        1
#define SDL_PS3EXT_BUTTON_WEST        2
#define SDL_PS3EXT_BUTTON_NORTH       3
#define SDL_PS3EXT_BUTTON_BACK        4
#define SDL_PS3EXT_BUTTON_GUIDE       5
#define SDL_PS3EXT_BUTTON_START       6
#define SDL_PS3EXT_BUTTON_LEFT_STICK  7
#define SDL_PS3EXT_BUTTON_RIGHT_STICK 8

#define SDL_PS3EXT_UDRAW_PEN         11
#define SDL_PS3EXT_UDRAW_FINGER      12
#define SDL_PS3EXT_UDRAW_TWO_FINGERS 13
#define SDL_PS3EXT_UDRAW_AXIS_X        0
#define SDL_PS3EXT_UDRAW_AXIS_Y        1
#define SDL_PS3EXT_UDRAW_AXIS_PRESSURE 2
#define SDL_PS3EXT_UDRAW_AXIS_DISTANCE 3
#define SDL_PS3EXT_UDRAW_MAX_X         1919
#define SDL_PS3EXT_UDRAW_MAX_Y         1079
#define SDL_PS3EXT_UDRAW_MAX_PRESSURE  142

#define SDL_PS3EXT_TOPSHOT_TRIGGER     11
#define SDL_PS3EXT_TOPSHOT_RELOAD      12
#define SDL_PS3EXT_TOPSHOT_LEFT_SEEN   13
#define SDL_PS3EXT_TOPSHOT_RIGHT_SEEN  14
#define SDL_PS3EXT_TOPSHOT_HEART_TOUCH 15
#define SDL_PS3EXT_TOPSHOT_MAX_LED     1023

#define SDL_PS3EXT_TONYHAWK_MAX_IR 0x38

#define SDL_PS3EXT_MAX_AXES 8

/* Hat bits, the values of SDL_HAT_UP and its neighbors */
#define SDL_PS3EXT_HAT_CENTERED 0x00
#define SDL_PS3EXT_HAT_UP       0x01
#define SDL_PS3EXT_HAT_RIGHT    0x02
#define SDL_PS3EXT_HAT_DOWN     0x04
#define SDL_PS3EXT_HAT_LEFT     0x08

typedef struct SDL_PS3ExtOutput
{
    uint32_t buttons;     /* Bit i is button i */
    uint32_t button_mask; /* The buttons this report posts */
    int16_t axes[SDL_PS3EXT_MAX_AXES];
    uint16_t axis_mask;      /* The axes this report posts */
    uint16_t data_axis_mask; /* Raw data axes, seeded past the anti-jitter gate */
    uint8_t hat;
    bool has_accel;
    int16_t accel[3]; /* uDraw: each little-endian word minus 0x200 */
} SDL_PS3ExtOutput;

/* The report is 27 bytes with no report ID. A shorter read changes nothing.
 * The uDraw takes exactly 27, as Linux requires. The others accept a longer
 * read and ignore bytes past 26. */
#define SDL_PS3EXT_REPORT_LENGTH 27
extern bool SDL_PS3Ext_Decode(int variant, const uint8_t *report, size_t length, SDL_PS3ExtOutput *out);

/* The uDraw accelerometer: brandonw.net's readings give 20 to 26 counts per g
 * around 0x200. No source states a calibrated scale. */
#define SDL_PS3EXT_UDRAW_COUNTS_PER_G 22

/* The Tony Hawk dongle sends one of two fixed reports while no board is on:
 * the board switched off, or the board lost. RPCS3 counts a board as on when
 * a report differs from both. */
#define SDL_PS3EXT_BOARD_INVALID 0 /* Too short, changes nothing */
#define SDL_PS3EXT_BOARD_IDLE    1
#define SDL_PS3EXT_BOARD_LIVE    2
extern int SDL_PS3Ext_TonyHawkBoardState(const uint8_t *report, size_t length);

/* The activation RPCS3 sends when it adds a board: an output report of 8
 * zero data bytes through hid_write, report ID 0 first. */
#define SDL_PS3EXT_TONYHAWK_ACTIVATION_LENGTH 9
extern size_t SDL_PS3Ext_BuildTonyHawkActivation(uint8_t out[SDL_PS3EXT_TONYHAWK_ACTIVATION_LENGTH]);

/* The Top Shot guns report the sensor bar's LEDs only after a SET_REPORT
 * whose first data byte is 0x82 and third a nonzero mode. The buffer is
 * report ID 0, then 82 00 01 00 00 00 00 00. */
#define SDL_PS3EXT_TOPSHOT_REQUEST_LENGTH 9
extern size_t SDL_PS3Ext_BuildTopShotRequest(uint8_t out[SDL_PS3EXT_TOPSHOT_REQUEST_LENGTH]);

/* The request schedule, on a millisecond clock the caller supplies. Open
 * sends the request as an output report. Whenever bytes 7 to 12 have read
 * zero for 1000 ms it goes again, at most once per 1000 ms. A request that
 * brought data is repeated in the same kind. One that brought none is
 * followed by the other kind, so the first retry after open is a feature
 * report. */
#define SDL_PS3EXT_REQUEST_NONE    0
#define SDL_PS3EXT_REQUEST_OUTPUT  1
#define SDL_PS3EXT_REQUEST_FEATURE 2
#define SDL_PS3EXT_REQUEST_RETRY_MS 1000

typedef struct SDL_PS3ExtAim
{
    bool open;
    int last_kind;
    uint64_t last_request_ms;
    bool worked;         /* Data arrived after the last request */
    bool zero;           /* The last report read zero in bytes 7 to 12 */
    uint64_t zero_since_ms;
} SDL_PS3ExtAim;

extern int SDL_PS3ExtAim_Open(SDL_PS3ExtAim *aim, uint64_t now_ms);
extern void SDL_PS3ExtAim_OnReport(SDL_PS3ExtAim *aim, const uint8_t *report, size_t length, uint64_t now_ms);
extern int SDL_PS3ExtAim_Due(const SDL_PS3ExtAim *aim, uint64_t now_ms);
extern void SDL_PS3ExtAim_Sent(SDL_PS3ExtAim *aim, int kind, uint64_t now_ms);

#endif /* SDL_hidapi_ps3ext_proto_h_ */
