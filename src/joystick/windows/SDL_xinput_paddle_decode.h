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

#ifndef SDL_xinput_paddle_decode_h_
#define SDL_xinput_paddle_decode_h_

#include <stddef.h>
#include <stdint.h>

/* Private payload decoder. There are no device, runtime, or output calls.
 * The caller owns one state per controller generation and serializes access.
 * Payload storage must not overlap state. No payload pointer is retained.
 */
typedef enum SDL_XInputPaddleTransport
{
    SDL_XINPUT_PADDLE_VENDOR_GATT = 1, /* Bluetooth vendor characteristic only. */
    SDL_XINPUT_PADDLE_SERVICE_GIP = 2 /* Raw GIP payload, with headers removed. */
} SDL_XInputPaddleTransport;

typedef enum SDL_XInputPaddleDelivery
{
    SDL_XINPUT_PADDLE_DELIVERY_NONE,
    SDL_XINPUT_PADDLE_DELIVERY_IN_BAND, /* A field was read, firmware support is unqualified. */
    SDL_XINPUT_PADDLE_DELIVERY_GATT,
    SDL_XINPUT_PADDLE_DELIVERY_SEPARATE /* An actual service 0x0C was received. */
} SDL_XInputPaddleDelivery;

typedef enum SDL_XInputPaddleComparison
{
    SDL_XINPUT_PADDLE_COMPARE_UNKNOWN,
    SDL_XINPUT_PADDLE_COMPARE_EQUAL,
    SDL_XINPUT_PADDLE_COMPARE_DIFFERENT
} SDL_XInputPaddleComparison;

typedef enum SDL_XInputPaddleProfileKind
{
    SDL_XINPUT_PADDLE_PROFILE_NONE,
    SDL_XINPUT_PADDLE_PROFILE_SLOT,
    SDL_XINPUT_PADDLE_PROFILE_MODE /* Series 1 bit 0x10, normalized to 0 or 1. */
} SDL_XInputPaddleProfileKind;

enum
{
    SDL_XINPUT_PADDLE_NORMAL_READY = 0x01, /* Service 0x20, normal[] was copied. */
    SDL_XINPUT_PADDLE_RAW_NIBBLE = 0x02, /* This frame contains a known field. */
    SDL_XINPUT_PADDLE_STATE_READY = 0x04, /* State has this frame's paddle sample. */
    SDL_XINPUT_PADDLE_SEPARATE_FRAME = 0x08 /* Service 0x0C, never a 0x20. */
};

typedef struct SDL_XInputPaddleSample
{
    uint16_t button_word; /* Observed digital word, never an XInput replacement. */
    uint8_t raw_mask; /* Original low nibble, before Series 1 remapping. */
    uint8_t physical_mask; /* P1/P2/P3/P4 = 0x01/0x02/0x04/0x08. */
    uint8_t profile;
    SDL_XInputPaddleProfileKind profile_kind;
    SDL_XInputPaddleComparison digital_comparison; /* Metadata only. */
    SDL_XInputPaddleDelivery delivery;
} SDL_XInputPaddleSample;

typedef struct SDL_XInputPaddleState
{
    uint64_t generation;
    SDL_XInputPaddleTransport transport;
    uint8_t normal[14]; /* Last recognized service 0x20, copied without edits. */
    uint8_t have_normal;
    uint8_t have_paddles;
    uint8_t have_separate;
    SDL_XInputPaddleSample paddles; /* Last STATE_READY sample. */
} SDL_XInputPaddleState;

typedef struct SDL_XInputPaddleResult
{
    uint32_t flags; /* Zero means rejected, with all state unchanged. */
    SDL_XInputPaddleSample sample; /* Current frame, including a stale in-band field. */
} SDL_XInputPaddleResult;

#ifdef __cplusplus
extern "C" {
#endif

/* Reset clears all receipt evidence, caches, and masks. Choose a new generation
 * on replacement/reconnect so late frames from a retired generation are rejected.
 */
void SDL_XInputPaddleReset(SDL_XInputPaddleState *state,
                         SDL_XInputPaddleTransport transport, uint64_t generation);

/* GATT has no report ID: pass 0 and exactly 17 bytes. Byte 16 stays opaque.
 * Service IDs are the raw command: 0x20 (14/29/34/46/47 bytes), 0x0C (17 only).
 * The 14-byte ordinary layout has no paddle field. Other lengths are rejected.
 * A 46-byte 0x20 cannot distinguish in-band firmware from split-report firmware.
 * STATE_READY allows raw publication, but IN_BAND is not support qualification.
 * Once 0x0C arrives, every 0x20 updates only normal[]: RAW_NIBBLE can be set
 * without STATE_READY, and the authoritative paddle sample remains unchanged.
 * Neither profile nor digital comparison gates publication. Ordinary controls
 * remain the caller's existing XInput data. Firmware/transport qualification
 * and any button-index assignment belong to the caller.
 */
SDL_XInputPaddleResult SDL_XInputPaddleDecode(SDL_XInputPaddleState *state,
                                           SDL_XInputPaddleTransport transport,
                                           uint64_t generation, uint32_t report_id,
                                           const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* SDL_xinput_paddle_decode_h_ */
