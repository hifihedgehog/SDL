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

/* Namco's USB light guns, hifihedgehog/SDL#33 Part 9. Pure C99: no SDL
 * runtime and no I/O, so every decision here runs in the offline tests
 * exactly as it runs in the library.
 *
 * GunCon 2, 0B9A:016A, which the EMS LCD TopGun shares: one vendor-class
 * interface with one interrupt IN endpoint. The host sends one mode request
 * as a SET_REPORT control transfer, then reads 6-byte reports with the
 * buttons active low and the beam position in raw counts.
 *
 * GunCon 3, 0B9A:0800: the host writes an 8-byte key to the interrupt OUT
 * endpoint, and the gun answers with 15-byte encrypted reports. This module
 * holds the key, the checksum and the layout of a decrypted report. The
 * decryption needs a 256-byte table that only GPL and unlicensed sources
 * carry, so it is not here, and no driver claims the gun.
 *
 * The facts are restated from the hardware drivers and emulators that read
 * these guns. No code from them is copied.
 */

#ifndef SDL_hidapi_guncon_proto_h_
#define SDL_hidapi_guncon_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Hat bits, equal to SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN and SDL_HAT_LEFT */
#define SDL_GUNCON_HAT_UP    0x01
#define SDL_GUNCON_HAT_RIGHT 0x02
#define SDL_GUNCON_HAT_DOWN  0x04
#define SDL_GUNCON_HAT_LEFT  0x08

typedef struct SDL_GunConSink
{
    void *userdata;
    /* One write through hid_write. For the GunCon 2 the first byte is the
       report number, 0, which hid_write strips before its SET_REPORT. */
    bool (*write)(void *userdata, const uint8_t *data, size_t length);
} SDL_GunConSink;

#define SDL_GUNCON2_REPORT_LENGTH 6
#define SDL_GUNCON2_MODE_LENGTH   7 /* The report number, then 6 bytes */
#define SDL_GUNCON2_AXES          2 /* X and Y */
#define SDL_GUNCON2_BUTTONS       6 /* Trigger, A, B, C, Start, Select */

typedef struct SDL_GunCon2State
{
    int16_t axes[SDL_GUNCON2_AXES];
    uint8_t buttons; /* Bit i is button i */
    uint8_t hat;     /* SDL hat bits */
} SDL_GunCon2State;

/* The mode request as hid_write takes it: report number 0, then X offset,
 * Y offset and mode, each 16-bit little-endian as PCSX2 reads them:
 * 00 00 00 00 00 00 01. */
extern void SDL_GunCon2_ModeRequest(uint8_t out[SDL_GUNCON2_MODE_LENGTH]);

/* Sends the mode request, once per open. A reconnect opens again. */
extern bool SDL_GunCon2_Open(const SDL_GunConSink *sink);

/* Before any report: nothing pressed, the hat centered, X and Y 0 */
extern void SDL_GunCon2_ResetState(SDL_GunCon2State *state);

/* Decodes one read. A read shorter than 6 bytes changes nothing and
 * returns false. A longer one decodes its first 6. X and Y are the raw
 * counts, clamped to 32767. */
extern bool SDL_GunCon2_Decode(const uint8_t *report, size_t length, SDL_GunCon2State *state);

#define SDL_GUNCON3_KEY_LENGTH    8
#define SDL_GUNCON3_REPORT_LENGTH 15
#define SDL_GUNCON3_AXES          7  /* Aim X, Y and Z, A stick X and Y, B stick X and Y */
#define SDL_GUNCON3_BUTTONS       11 /* Trigger, A1-A3, B1-B3, C1, C2, markers out of view, one marker in view */
#define SDL_GUNCON3_RETRY_FAILURES 2 /* Checksum failures in a row that send the key again */

/* The key every PC tool writes, captured from Time Crisis 4 */
extern const uint8_t SDL_GunCon3_Key[SDL_GUNCON3_KEY_LENGTH];

/* The checksum of an encrypted report: from key byte 7 through bytes 0 to
 * 12, modulo 256 at every step */
extern uint8_t SDL_GunCon3_Checksum(const uint8_t *encrypted, uint8_t key7);

/* Whether a 15-byte report's byte 13 is its checksum under this key */
extern bool SDL_GunCon3_ChecksumValid(const uint8_t *report, size_t length, const uint8_t *key);

typedef struct SDL_GunCon3State
{
    int16_t axes[SDL_GUNCON3_AXES];
    uint16_t buttons; /* Bit i is button i */
} SDL_GunCon3State;

/* Decodes bytes 0 to 12 of a report after decryption */
extern void SDL_GunCon3_DecodePlain(const uint8_t *plain, SDL_GunCon3State *state);

typedef enum SDL_GunCon3Result
{
    SDL_GUNCON3_REPORT_SHORT,  /* Fewer than 15 bytes: dropped */
    SDL_GUNCON3_REPORT_FAILED, /* The checksum failed: dropped */
    SDL_GUNCON3_REPORT_VALID
} SDL_GunCon3Result;

typedef struct SDL_GunCon3Session
{
    int failures; /* Checksum failures in a row */
} SDL_GunCon3Session;

/* Writes the key. A reconnect opens again. The session keeps no clock:
 * silence before the first report is normal. */
extern bool SDL_GunCon3_Open(SDL_GunCon3Session *session, const SDL_GunConSink *sink);

/* Checks one read. Two checksum failures in a row write the key again, and
 * a valid report clears the count. Returns an SDL_GunCon3Result. */
extern int SDL_GunCon3_HandleReport(SDL_GunCon3Session *session, const uint8_t *report, size_t length,
                                     const SDL_GunConSink *sink);

#endif /* SDL_hidapi_guncon_proto_h_ */
