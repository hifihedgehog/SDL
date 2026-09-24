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

/* The Xbox 360 Big Button receiver (045E:02A0) packet handling, as a pure
 * state machine: C99, no SDL runtime, no I/O, time from an injected clock.
 * The receiver serves four pads and sends a 5-byte packet only while a
 * button is held: byte 2 is the pad, bytes 3 and 4 carry the controls.
 */

#ifndef SDL_hidapi_xbox360bb_proto_h_
#define SDL_hidapi_xbox360bb_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_XBOX360BB_PADS        4
#define SDL_XBOX360BB_PACKET_SIZE 5

/* A pad releases every control this long after its last packet. */
#define SDL_XBOX360BB_RELEASE_NS (120 * 1000000ULL)

/* Joystick buttons, one bit each in the button mask, in the order the
 * driver's gamepad mapping names them. */
#define SDL_XBOX360BB_BUTTON_A     0x01
#define SDL_XBOX360BB_BUTTON_B     0x02
#define SDL_XBOX360BB_BUTTON_X     0x04
#define SDL_XBOX360BB_BUTTON_Y     0x08
#define SDL_XBOX360BB_BUTTON_BACK  0x10
#define SDL_XBOX360BB_BUTTON_GUIDE 0x20
#define SDL_XBOX360BB_BUTTON_START 0x40
#define SDL_XBOX360BB_BUTTON_BIG   0x80
#define SDL_XBOX360BB_BUTTON_COUNT 8

/* D-pad bits, with the values of SDL's hat bits. */
#define SDL_XBOX360BB_HAT_UP    0x01
#define SDL_XBOX360BB_HAT_RIGHT 0x02
#define SDL_XBOX360BB_HAT_DOWN  0x04
#define SDL_XBOX360BB_HAT_LEFT  0x08

typedef struct SDL_Xbox360BBPad
{
    uint8_t buttons;      /* SDL_XBOX360BB_BUTTON_* */
    uint8_t hat;          /* SDL_XBOX360BB_HAT_* */
    uint64_t last_packet; /* Injected clock */
} SDL_Xbox360BBPad;

typedef struct SDL_Xbox360BBState
{
    SDL_Xbox360BBPad pads[SDL_XBOX360BB_PADS];
} SDL_Xbox360BBState;

/* Where the state machine reports what changed. Every callback is required.
 * joystick returns the pad's open joystick, or NULL when the application has
 * not opened it. controls is called only with a non-NULL joystick. */
typedef struct SDL_Xbox360BBSink
{
    void *userdata;
    void (*connect)(void *userdata, int pad);
    void *(*joystick)(void *userdata, int pad);
    void (*controls)(void *userdata, void *joystick, uint8_t buttons, uint8_t hat);
} SDL_Xbox360BBSink;

extern void SDL_Xbox360BB_Init(SDL_Xbox360BBState *state);

/* The receiver cannot report which pads exist, so all four connect when it
 * opens. */
extern void SDL_Xbox360BB_Attach(SDL_Xbox360BBState *state, const SDL_Xbox360BBSink *sink);

/* Applies one packet. Returns the pad it applied to, or -1 when it was
 * dropped: any length but 5, or a pad index above 3. Reads nothing at or
 * past length. */
extern int SDL_Xbox360BB_HandlePacket(SDL_Xbox360BBState *state, const uint8_t *data, size_t length,
                                      uint64_t now, const SDL_Xbox360BBSink *sink);

/* Releases every control of a pad whose last packet is at least
 * SDL_XBOX360BB_RELEASE_NS old. Returns a mask of the pads released. */
extern uint8_t SDL_Xbox360BB_Expire(SDL_Xbox360BBState *state, uint64_t now, const SDL_Xbox360BBSink *sink);

#endif /* SDL_hidapi_xbox360bb_proto_h_ */
