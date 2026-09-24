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

/* Guitar Hero Live guitars: the PS3 and Wii U dongle (12BA:074B), the PS4
 * dongle (1430:07BB) and the Xbox One dongle (1430:079B). Pure C99: no SDL
 * runtime and no I/O. SDL_hidapi_ghl.c, the GameInput backend and the GIP
 * driver send the keep-alives built here and post what the decoders return.
 *
 * The byte layouts are facts from PlasticBand, RB4InstrumentMapper, Linux
 * hid-sony, RPCS3, GHLtarUtility and Santroller. No code from any of them is
 * copied.
 */

#ifndef SDL_ghl_proto_h_
#define SDL_ghl_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_GHL_DONGLE_NONE    0
#define SDL_GHL_DONGLE_PS3     1 /* PS3 and Wii U, 12BA:074B */
#define SDL_GHL_DONGLE_PS4     2 /* 1430:07BB */
#define SDL_GHL_DONGLE_XBOXONE 3 /* 1430:079B */

/* The dongle a USB ID names, or SDL_GHL_DONGLE_NONE. */
extern int SDL_GHL_GetDongle(uint16_t vendor, uint16_t product);
extern const char *SDL_GHL_DongleName(int dongle);

/* Buttons and axes in SDL gamepad order, so the default mapping applies */
#define SDL_GHL_BUTTON_SOUTH          0 /* Black 1 */
#define SDL_GHL_BUTTON_EAST           1 /* Black 2 */
#define SDL_GHL_BUTTON_WEST           2 /* White 1 */
#define SDL_GHL_BUTTON_NORTH          3 /* Black 3 */
#define SDL_GHL_BUTTON_BACK           4 /* Hero Power */
#define SDL_GHL_BUTTON_GUIDE          5 /* D-pad center */
#define SDL_GHL_BUTTON_START          6 /* Pause */
#define SDL_GHL_BUTTON_LEFT_STICK     7 /* GHTV */
#define SDL_GHL_BUTTON_RIGHT_STICK    8 /* Never pressed */
#define SDL_GHL_BUTTON_LEFT_SHOULDER  9 /* White 2 */
#define SDL_GHL_BUTTON_RIGHT_SHOULDER 10 /* White 3 */
#define SDL_GHL_NUM_BUTTONS           11

#define SDL_GHL_AXIS_LEFTX         0 /* Unused, rests at 0 */
#define SDL_GHL_AXIS_LEFTY         1 /* Strum bar */
#define SDL_GHL_AXIS_RIGHTX        2 /* Tilt */
#define SDL_GHL_AXIS_RIGHTY        3 /* Whammy */
#define SDL_GHL_AXIS_LEFT_TRIGGER  4 /* Unused, rests released */
#define SDL_GHL_AXIS_RIGHT_TRIGGER 5 /* Unused, rests released */
#define SDL_GHL_NUM_AXES           6

/* Hat bits, the values of SDL_HAT_UP and its neighbors */
#define SDL_GHL_HAT_CENTERED 0x00
#define SDL_GHL_HAT_UP       0x01
#define SDL_GHL_HAT_RIGHT    0x02
#define SDL_GHL_HAT_DOWN     0x04
#define SDL_GHL_HAT_LEFT     0x08

typedef struct SDL_GHLOutput
{
    uint16_t buttons;     /* Bit i is button i */
    uint16_t button_mask; /* The buttons this report posts */
    int16_t axes[SDL_GHL_NUM_AXES];
    uint8_t hat;
} SDL_GHLOutput;

/* Frame A: the 27-byte report of the PS3 and Wii U dongle with no report ID,
 * and the payload of Xbox One message 0x21. 27 to 32 bytes are accepted and
 * bytes past 26 are ignored. Any other length changes nothing. */
#define SDL_GHL_FRAME_A_LENGTH     27
#define SDL_GHL_FRAME_A_MAX_LENGTH 32
extern bool SDL_GHL_DecodeFrameA(const uint8_t *report, size_t length, SDL_GHLOutput *out);

/* Frame B: the PS4 report, report ID 0x01 in byte 0, 64 bytes or more.
 * *connected is byte 25, set whenever the report is accepted. */
#define SDL_GHL_FRAME_B_REPORT_ID 0x01
#define SDL_GHL_FRAME_B_LENGTH    64
extern bool SDL_GHL_DecodeFrameB(const uint8_t *report, size_t length, SDL_GHLOutput *out, bool *connected);

/* Xbox One GIP messages. 0x21 is frame A. The GIP guide message carries the
 * d-pad center, so this path posts no GUIDE. 0x20, the gamepad-shaped copy
 * for console menus, and every other message change nothing. */
#define SDL_GHL_XBOX_MESSAGE_NAVIGATION 0x20
#define SDL_GHL_XBOX_MESSAGE_GUITAR     0x21
#define SDL_GHL_XBOX_MESSAGE_OUTPUT     0x22
extern bool SDL_GHL_DecodeXboxMessage(uint8_t message, const uint8_t *payload, size_t length, SDL_GHLOutput *out);

/* The keep-alive. Without it the strum bar cuts out held frets. */
#define SDL_GHL_KEEPALIVE_INTERVAL_MS 8000

/* The buffer for hid_send_output_report: the report ID, then 8 data bytes.
 * Returns the length, 9, or 0 for a dongle that is not HID. */
#define SDL_GHL_HID_KEEPALIVE_LENGTH 9
extern size_t SDL_GHL_BuildHIDKeepAlive(int dongle, uint8_t out[SDL_GHL_HID_KEEPALIVE_LENGTH]);

/* The payload of Xbox One message 0x22. Returns its length, 8. */
#define SDL_GHL_XBOX_KEEPALIVE_LENGTH 8
extern size_t SDL_GHL_BuildXboxKeepAlive(uint8_t out[SDL_GHL_XBOX_KEEPALIVE_LENGTH]);

/* The schedule, on a millisecond clock the caller supplies. Start makes one
 * due at once. A success sets the next one 8000 ms after the send. A failed
 * send leaves it due, so the next update retries it. */
typedef struct SDL_GHLKeepAlive
{
    bool active;
    uint64_t next_due_ms;
} SDL_GHLKeepAlive;

extern void SDL_GHL_KeepAliveStart(SDL_GHLKeepAlive *keepalive, uint64_t now_ms);
extern void SDL_GHL_KeepAliveStop(SDL_GHLKeepAlive *keepalive);
extern bool SDL_GHL_KeepAliveDue(const SDL_GHLKeepAlive *keepalive, uint64_t now_ms);
extern void SDL_GHL_KeepAliveSent(SDL_GHLKeepAlive *keepalive, uint64_t now_ms, bool success);

#endif /* SDL_ghl_proto_h_ */
