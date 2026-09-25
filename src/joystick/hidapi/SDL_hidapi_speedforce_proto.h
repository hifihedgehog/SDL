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

/* The Logitech Speed Force Wireless (Wii), 046D:C29C. Pure C99: no SDL
 * runtime and no I/O, time from an injected clock.
 *
 * The dongle bonds with the wheel only after two feature reports, AF and then
 * B2 with a random address 40 ms later, and reports nothing before. Its input
 * is a 5-byte report with no report ID: a 10-bit wheel, 11 buttons and two
 * pedals. The facts are from Linux hid-lg and hid-lg4ff, new-lg4ff and
 * WiiBrew. No code from them is copied.
 */

#ifndef SDL_hidapi_speedforce_proto_h_
#define SDL_hidapi_speedforce_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_SPEEDFORCE_REPORT_LENGTH  5
#define SDL_SPEEDFORCE_FEATURE_BUFFER 9 /* The 8-byte feature report after a 0x00 report ID */
#define SDL_SPEEDFORCE_OUTPUT_BUFFER  8 /* The 7-byte output report after a 0x00 report ID */
#define SDL_SPEEDFORCE_BOND_DELAY_MS  40

#define SDL_SPEEDFORCE_AXES    3 /* Wheel, then the pedals in bytes 3 and 4 */
#define SDL_SPEEDFORCE_BUTTONS 11

typedef struct SDL_SpeedForceState
{
    uint16_t buttons;
    int16_t axes[SDL_SPEEDFORCE_AXES];
} SDL_SpeedForceState;

/* A 5-byte report, and only a 5-byte report */
extern bool SDL_SpeedForce_DecodeReport(const uint8_t *report, size_t length, SDL_SpeedForceState *out);

/* The state before the first report: the wheel at its rest reading, 520,
 * and both pedals released at 255 */
extern void SDL_SpeedForce_RestState(SDL_SpeedForceState *out);

/* The output report for a constant force of level -0x80 to 0x7F: level 0
 * stops the force. For the haptic step, not sent by the joystick driver. */
extern void SDL_SpeedForce_BuildConstantForce(int level, uint8_t out[SDL_SPEEDFORCE_OUTPUT_BUFFER]);

/* The output report that turns autocentering off */
extern void SDL_SpeedForce_BuildAutocenterOff(uint8_t out[SDL_SPEEDFORCE_OUTPUT_BUFFER]);

/* Where the bond sends its reports. Every callback is required. */
typedef struct SDL_SpeedForceSink
{
    void *userdata;
    /* One feature report, the buffer starting with the 0x00 report ID.
     * Returns false when the write failed. */
    bool (*feature)(void *userdata, const uint8_t *data, size_t length);
    /* One output report, the buffer starting with the 0x00 report ID */
    bool (*output)(void *userdata, const uint8_t *data, size_t length);
    /* One random byte for the address */
    uint8_t (*random)(void *userdata);
} SDL_SpeedForceSink;

typedef struct SDL_SpeedForceBond
{
    bool b2_pending;
    uint64_t b2_due_ms;
    bool done;
} SDL_SpeedForceBond;

/* Sends AF. If it succeeds, B2 follows 40 ms later. If it fails, the
 * autocenter-off report goes out at once, as Linux starts force feedback
 * whatever the bond returned. */
extern void SDL_SpeedForce_StartBond(SDL_SpeedForceBond *bond, uint64_t now_ms, const SDL_SpeedForceSink *sink);

/* Sends B2 and then the autocenter-off report once their time has come.
 * Returns true once the bond has sent everything it will. */
extern bool SDL_SpeedForce_UpdateBond(SDL_SpeedForceBond *bond, uint64_t now_ms, const SDL_SpeedForceSink *sink);

#endif /* SDL_hidapi_speedforce_proto_h_ */
