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

/* The Intel Wireless Series base station (8086:C013) protocol, as a pure
 * state machine: C99, no SDL runtime, no I/O. The driver feeds it every
 * packet read from interrupt IN 0x81 with the length the read reported, and
 * writes the activation replies it hands out to interrupt OUT 0x01.
 *
 * The protocol facts follow JoypadOS (Apache-2.0), which drives this base
 * station from a hardware capture, and intel-wings 0.2 (GPL-2.0), which is
 * used for facts only. No code from either is copied.
 */

#ifndef SDL_hidapi_intelwireless_proto_h_
#define SDL_hidapi_intelwireless_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_INTEL_WIRELESS_SLOTS      8
#define SDL_INTEL_WIRELESS_READ_SIZE  27 /* wMaxPacketSize of interrupt IN 0x81 */
#define SDL_INTEL_WIRELESS_REPLY_SIZE 6

/* Delay before a failed or short activation reply is written again. */
#define SDL_INTEL_WIRELESS_RETRY_NS (100 * 1000000ULL)

/* Button bits. Bits 0 to 7 are byte 11 of an input packet as it arrives,
 * bits 8 to 10 are bits 0 to 2 of byte 12. */
#define SDL_INTEL_WIRELESS_BUTTON_A     0x0001
#define SDL_INTEL_WIRELESS_BUTTON_B     0x0002
#define SDL_INTEL_WIRELESS_BUTTON_C     0x0004
#define SDL_INTEL_WIRELESS_BUTTON_X     0x0008
#define SDL_INTEL_WIRELESS_BUTTON_Y     0x0010
#define SDL_INTEL_WIRELESS_BUTTON_Z     0x0020
#define SDL_INTEL_WIRELESS_BUTTON_L     0x0040
#define SDL_INTEL_WIRELESS_BUTTON_R     0x0080
#define SDL_INTEL_WIRELESS_BUTTON_START 0x0100
#define SDL_INTEL_WIRELESS_BUTTON_SHIFT 0x0200
#define SDL_INTEL_WIRELESS_BUTTON_MOUSE 0x0400
#define SDL_INTEL_WIRELESS_BUTTON_COUNT 11

/* D-pad bits, with the values of SDL's hat bits. */
#define SDL_INTEL_WIRELESS_HAT_UP    0x01
#define SDL_INTEL_WIRELESS_HAT_RIGHT 0x02
#define SDL_INTEL_WIRELESS_HAT_DOWN  0x04
#define SDL_INTEL_WIRELESS_HAT_LEFT  0x08

/* The joystick the driver exposes: these buttons in this order, and one hat
 * for the D-pad. Z and C are the shoulders and L and R the triggers,
 * following JoypadOS. Mouse is Back and Shift is Guide. */
#define SDL_INTEL_WIRELESS_JOYSTICK_BUTTONS 11
extern const uint16_t SDL_IntelWireless_ButtonOrder[SDL_INTEL_WIRELESS_JOYSTICK_BUTTONS];

/* The gamepad mapping for that joystick. */
#define SDL_INTEL_WIRELESS_MAPPING "a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftshoulder:b7,rightshoulder:b8,lefttrigger:b9,righttrigger:b10,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"

typedef struct SDL_IntelWirelessSlot
{
    bool gamepad;     /* The base station reported a gamepad in this slot */
    bool connected;   /* The slot is a joystick: it became ready or sent input */
    uint16_t buttons; /* SDL_INTEL_WIRELESS_BUTTON_* */
    uint8_t hat;      /* SDL_INTEL_WIRELESS_HAT_* */
} SDL_IntelWirelessSlot;

typedef struct SDL_IntelWirelessState
{
    SDL_IntelWirelessSlot slots[SDL_INTEL_WIRELESS_SLOTS];
    uint8_t pending;       /* Slots waiting for an activation reply */
    bool in_flight;        /* One reply handed out and not yet completed */
    uint8_t in_flight_slot;
    uint64_t retry_at;     /* Injected clock. No reply is handed out before it. */
} SDL_IntelWirelessState;

/* Where the state machine reports what changed. Every callback is required.
 * joystick returns the slot's open joystick, or NULL when the application
 * has not opened it. controls is called only with a non-NULL joystick. */
typedef struct SDL_IntelWirelessSink
{
    void *userdata;
    void (*connect)(void *userdata, int slot);
    void (*disconnect)(void *userdata, int slot);
    void *(*joystick)(void *userdata, int slot);
    void (*controls)(void *userdata, void *joystick, uint16_t buttons, uint8_t hat);
} SDL_IntelWirelessSink;

extern void SDL_IntelWireless_Init(SDL_IntelWirelessState *state);

/* Applies one packet of the given length. Reads nothing at or past length.
 * A packet shorter than its type's minimum, longer than 27 bytes, or of an
 * unknown type changes nothing. */
extern void SDL_IntelWireless_HandlePacket(SDL_IntelWirelessState *state, const uint8_t *data, size_t length,
                                           const SDL_IntelWirelessSink *sink);

/* Sends a connected slot's current controls to its joystick, when open. The
 * pads report changes only, so a joystick the application opens after its
 * slot connected learns what is already held this way. */
extern void SDL_IntelWireless_Refresh(const SDL_IntelWirelessState *state, int slot,
                                      const SDL_IntelWirelessSink *sink);

/* Hands out the next activation reply when none is in flight and the retry
 * delay has passed. Pending slots go lowest first. */
extern bool SDL_IntelWireless_NextReply(SDL_IntelWirelessState *state, uint64_t now,
                                        uint8_t reply[SDL_INTEL_WIRELESS_REPLY_SIZE]);

/* Completes the reply in flight. written is the byte count the write
 * reported, or a negative value when it failed. A failed or short write is
 * queued again while the slot is still a gamepad. */
extern void SDL_IntelWireless_ReplyDone(SDL_IntelWirelessState *state, int written, uint64_t now);

/* The base station went away. Disconnects every connected slot and forgets
 * everything. */
extern void SDL_IntelWireless_Detach(SDL_IntelWirelessState *state, const SDL_IntelWirelessSink *sink);

#endif /* SDL_hidapi_intelwireless_proto_h_ */
