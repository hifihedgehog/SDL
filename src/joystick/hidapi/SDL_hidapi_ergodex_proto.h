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

/* The Ergodex DX1 Input System, 1603:0002, hifihedgehog/SDL#33 Part 14.
 * Pure C99: no SDL runtime and no I/O, so every decision here runs in the
 * offline tests exactly as it runs in the library.
 *
 * The pad is a composite device. Interface 0 is a boot keyboard that stays
 * with the system. Interface 1 is vendor class with interrupt IN 0x82 and
 * interrupt OUT 0x02, and every transfer on it is exactly 16 bytes. Byte 0
 * is the message type, and requests and replies number their types
 * separately.
 *
 * The keys type nothing until the host programs them, and the pad forgets
 * its programming at power-off. On one pad, keys programmed as macro keys
 * reported on the vendor interface in normal mode. On another, no
 * programming made a key report outside test mode, and in test mode every
 * press and release reported. The start-up here covers both: it asks for
 * the serial, programs all 50 keys as macro keys and enables the pad in test
 * mode, and the key reports of both modes decode alike. The pad also sends
 * messages nobody asked for, so the driver reads interrupt IN 0x82 as a
 * stream and dispatches on byte 0. The host waits 10 ms after every OUT
 * transfer, since the pad stops responding without the pause.
 *
 * The facts are restated from dx1-studio and ergodex-dx1-linux (both
 * GPL-3.0), which drive the pad on Windows and Linux. No code from either is
 * copied.
 */

#ifndef SDL_hidapi_ergodex_proto_h_
#define SDL_hidapi_ergodex_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_ERGODEX_PACKET_LENGTH 16
#define SDL_ERGODEX_KEYS          50

/* The joystick: buttons 0 to 49 are key IDs 1 to 50, then the pad's top
 * ("power") and lower ("record") buttons. An ID belongs to the key puck and
 * stays the same wherever the puck sits. */
#define SDL_ERGODEX_BUTTONS      52
#define SDL_ERGODEX_BUTTON_TOP   50
#define SDL_ERGODEX_BUTTON_LOWER 51

/* The pause after every OUT transfer, and the delay before an output that
 * failed is sent again. A second failure of the same output ends the
 * start-up. */
#define SDL_ERGODEX_PACE_NS  (10 * 1000000ULL)
#define SDL_ERGODEX_RETRY_NS (50 * 1000000ULL)

/* The start-up outputs, in order: DEVICE, DISABLE, ten PROGRAM packets and
 * ENABLE in test mode. */
#define SDL_ERGODEX_STARTUP_OUTPUTS 13

typedef enum SDL_ErgodexPacket
{
    SDL_ERGODEX_PACKET_IGNORED, /* Not 16 bytes, or a type the pad does not send: nothing changed */
    SDL_ERGODEX_PACKET_STATUS,  /* STATUS: the pad buttons */
    SDL_ERGODEX_PACKET_KEYS,    /* MACRO or TEST: every key held at that moment */
    SDL_ERGODEX_PACKET_DEVICE   /* DEVICE: the serial and firmware version */
} SDL_ErgodexPacket;

typedef struct SDL_ErgodexState
{
    uint64_t buttons;   /* Bit i is button i */
    bool have_device;   /* A DEVICE reply arrived */
    uint64_t serial;
    uint8_t version[2]; /* Major, minor */
    int step;           /* The next start-up output, SDL_ERGODEX_STARTUP_OUTPUTS when all went out */
    bool in_flight;     /* One output handed out and not yet completed */
    bool retried;       /* The output at step failed once */
    bool stopped;       /* It failed twice, and the start-up ended there */
    uint64_t next_at;   /* Injected clock. No output is handed out before it. */
} SDL_ErgodexState;

/* Nothing held, no DEVICE reply, and the start-up from its first output,
 * due at now. A reconnect starts over, since the pad forgot its keys. */
extern void SDL_Ergodex_Init(SDL_ErgodexState *state, uint64_t now);

/* Writes the start-up output of a step, 0 to SDL_ERGODEX_STARTUP_OUTPUTS - 1,
 * into out, which holds SDL_ERGODEX_PACKET_LENGTH bytes. Returns false and
 * writes nothing for any other step. */
extern bool SDL_Ergodex_StartupOutput(int step, uint8_t *out);

/* Hands out the next start-up output when none is in flight, the start-up
 * has not ended, and the clock has reached the pause after the last one.
 * out holds SDL_ERGODEX_PACKET_LENGTH bytes. */
extern bool SDL_Ergodex_NextOutput(SDL_ErgodexState *state, uint64_t now, uint8_t *out);

/* Completes the output in flight. written is the byte count the write
 * reported, or a negative value when it failed. A whole write moves to the
 * next output after the pause. A failed or short one is sent again after
 * the retry delay, once. */
extern void SDL_Ergodex_OutputDone(SDL_ErgodexState *state, int written, uint64_t now);

/* Applies one read from interrupt IN 0x82. Reads nothing at or past length.
 * Returns an SDL_ErgodexPacket. */
extern int SDL_Ergodex_HandlePacket(SDL_ErgodexState *state, const uint8_t *data, size_t length);

/* Writes the serial as 16 lowercase hex digits and a terminating zero into
 * out, which holds 17 characters. */
extern void SDL_Ergodex_FormatSerial(uint64_t serial, char *out);

#endif /* SDL_hidapi_ergodex_proto_h_ */
