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

/* The In2Games Gametrak, 14B7:0982. Pure C99: no SDL runtime and no I/O,
 * time from an injected clock.
 *
 * The unit streams only after the host writes "Gametrak" and then 45 23, and
 * keeps streaming while the host writes 46 and the next byte of a rolling
 * key after every 100 sensor reports. Each 16-byte report holds six 16-bit
 * words: bits 1-15 an angle or length, bit 0 one bit of the key byte the
 * unit expects next. The facts are from GameTrak-Liberation, libgametrak
 * and PCSX2. No code from them is copied.
 *
 * The writes are output reports whose first byte is the report ID, 0x47,
 * 0x45 or 0x46. hid.dll cannot send those on a collection without report
 * IDs, so on Windows the unit goes through libusb, which sends each write as
 * a SET_REPORT control transfer with that ID.
 */

#ifndef SDL_hidapi_gametrak_proto_h_
#define SDL_hidapi_gametrak_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_GAMETRAK_REPORT_LENGTH     16
#define SDL_GAMETRAK_AXES              6 /* Left X, Y and length, then right X, Y and length */
#define SDL_GAMETRAK_BUTTONS           12
#define SDL_GAMETRAK_INITIAL_KEY       0x23
#define SDL_GAMETRAK_KEEPALIVE_REPORTS 100
#define SDL_GAMETRAK_ANSWER_WAIT_MS    10

typedef struct SDL_GametrakReport
{
    uint16_t words[SDL_GAMETRAK_AXES]; /* 0-2047, bits 1-15 of each word */
    int16_t axes[SDL_GAMETRAK_AXES];
    uint8_t key_bits;                  /* Bit j is bit 0 of word j */
    uint8_t hat;                       /* SDL hat bits */
    uint16_t buttons;                  /* Bit 0 the foot switch */
} SDL_GametrakReport;

/* A sensor report: exactly 16 bytes with every word at most 4095. The
 * unlock answer fails, since its first word is 0x6147. */
extern bool SDL_Gametrak_DecodeReport(const uint8_t *report, size_t length, SDL_GametrakReport *out);

/* Whether a report is the answer to the "Gametrak" write */
extern bool SDL_Gametrak_IsUnlockAnswer(const uint8_t *report, size_t length);

/* The rolling key, 24 bits, one step */
extern uint32_t SDL_Gametrak_NextKey(uint32_t key);

typedef struct SDL_GametrakSink
{
    void *userdata;
    /* One output report, the first byte its report ID */
    bool (*write)(void *userdata, const uint8_t *data, size_t length);
} SDL_GametrakSink;

#define SDL_GAMETRAK_PHASE_UNLOCK    0 /* "Gametrak" sent, waiting for the answer */
#define SDL_GAMETRAK_PHASE_STREAMING 1

typedef struct SDL_GametrakSession
{
    int phase;
    uint64_t answer_due_ms;
    uint32_t key;          /* The key byte last sent is its low byte */
    uint32_t sensor_reports;
    bool desync;           /* The last sensor report's key bits did not match */
} SDL_GametrakSession;

/* Sends "Gametrak". A reconnect starts here again with the key back at 0x23. */
extern void SDL_Gametrak_Open(SDL_GametrakSession *session, uint64_t now_ms, const SDL_GametrakSink *sink);

/* Sends 45 23 once 10 ms have passed without an answer */
extern void SDL_Gametrak_Update(SDL_GametrakSession *session, uint64_t now_ms, const SDL_GametrakSink *sink);

/* Applies one transfer. The unlock answer sends 45 23. A sensor report is
 * decoded into out, counted, checked against the key and, every 100th,
 * followed by a keep-alive. Returns true for a sensor report. */
extern bool SDL_Gametrak_HandleReport(SDL_GametrakSession *session, uint64_t now_ms, const uint8_t *report, size_t length,
                                      const SDL_GametrakSink *sink, SDL_GametrakReport *out);

#endif /* SDL_hidapi_gametrak_proto_h_ */
