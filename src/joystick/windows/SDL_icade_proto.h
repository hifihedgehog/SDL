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

/* The iCade letter protocol. The ION iCade arcade cabinet, and pads in iCade
 * mode, pair as Bluetooth HID keyboards. Each of the twelve controls has a
 * press letter and a release letter, and each letter arrives as a whole
 * keystroke. Pure C99: no SDL runtime and no I/O. SDL_icadejoystick.c owns
 * the window, the Raw Input records and the device list, and passes every
 * keyboard record of a marked device here. hifihedgehog/SDL#33 Part 16.
 *
 * The Nth letter of "wdxayhujikol" sets state bit N and the Nth letter of
 * "eczqtrfnmpgv" clears it (iCade-iOS iCadeReaderView.m:25-26, bits in
 * iCadeState.h:23-43). The decoder reads each letter's Scan 1 make code,
 * which names the key whatever the keyboard layout. It acts on key-down
 * records only, as Linux does (hid-icade.c:173-175), and ignores records
 * with the E0 or E1 prefix and the overrun code FF. A held button sends
 * nothing, so there is no timeout, and a lost release letter leaves its
 * control held until the next letter for it.
 */

#ifndef SDL_icade_proto_h_
#define SDL_icade_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The cabinet's Bluetooth IDs (Linux hid-ids.h:735-736) */
#define SDL_ICADE_VENDOR  0x15E4
#define SDL_ICADE_PRODUCT 0x0132

/* State bits. Buttons A to H are joystick buttons 0 to 7. */
#define SDL_ICADE_UP       0x0001
#define SDL_ICADE_RIGHT    0x0002
#define SDL_ICADE_DOWN     0x0004
#define SDL_ICADE_LEFT     0x0008
#define SDL_ICADE_BUTTON_A 0x0010 /* Top row, first */
#define SDL_ICADE_BUTTON_B 0x0020 /* Bottom row, first */
#define SDL_ICADE_BUTTON_C 0x0040 /* Top row, second */
#define SDL_ICADE_BUTTON_D 0x0080 /* Bottom row, second */
#define SDL_ICADE_BUTTON_E 0x0100 /* Top row, third */
#define SDL_ICADE_BUTTON_F 0x0200 /* Bottom row, third */
#define SDL_ICADE_BUTTON_G 0x0400 /* Top row, fourth */
#define SDL_ICADE_BUTTON_H 0x0800 /* Bottom row, fourth */
#define SDL_ICADE_CONTROLS 12
#define SDL_ICADE_BUTTONS  8

/* The RAWKEYBOARD values the decoder reads, as WinUser.h defines them:
   RI_KEY_BREAK, RI_KEY_E0, RI_KEY_E1 and KEYBOARD_OVERRUN_MAKE_CODE */
#define SDL_ICADE_KEY_BREAK 0x0001
#define SDL_ICADE_KEY_E0    0x0002
#define SDL_ICADE_KEY_E1    0x0004
#define SDL_ICADE_OVERRUN   0x00FF

/* Hat values, equal to SDL's SDL_HAT_* */
#define SDL_ICADE_HAT_CENTERED 0x00
#define SDL_ICADE_HAT_UP       0x01
#define SDL_ICADE_HAT_RIGHT    0x02
#define SDL_ICADE_HAT_DOWN     0x04
#define SDL_ICADE_HAT_LEFT     0x08

typedef enum SDL_ICadeLayout
{
    SDL_ICADE_LAYOUT_NONE,    /* Not decoded as an iCade */
    SDL_ICADE_LAYOUT_CABINET, /* The ION iCade, an arcade stick */
    SDL_ICADE_LAYOUT_PAD      /* A pair from SDL_HINT_JOYSTICK_ICADE_DEVICES, a gamepad */
} SDL_ICadeLayout;

/* The gamepad positions a layout can fill */
typedef enum SDL_ICadeSlot
{
    SDL_ICADE_SLOT_SOUTH,
    SDL_ICADE_SLOT_EAST,
    SDL_ICADE_SLOT_WEST,
    SDL_ICADE_SLOT_NORTH,
    SDL_ICADE_SLOT_BACK,
    SDL_ICADE_SLOT_START,
    SDL_ICADE_SLOT_LEFT_SHOULDER,
    SDL_ICADE_SLOT_RIGHT_SHOULDER,
    SDL_ICADE_SLOT_LEFT_TRIGGER,
    SDL_ICADE_SLOT_RIGHT_TRIGGER,
    SDL_ICADE_SLOT_COUNT
} SDL_ICadeSlot;

/* Applies one keyboard record to the state. True when the state changed. */
extern bool SDL_ICade_ApplyKey(uint16_t *state, uint16_t make_code, uint16_t flags);
/* The hat from bits 0 to 3. Opposing directions held together cancel. */
extern uint8_t SDL_ICade_GetHat(uint16_t state);
/* Button 0 is A and button 7 is H */
extern bool SDL_ICade_GetButton(uint16_t state, int button);
/* The joystick button a layout puts on a gamepad slot, -1 when none. The
   D-pad is always the hat. */
extern int SDL_ICade_GetSlotButton(SDL_ICadeLayout layout, SDL_ICadeSlot slot);

/* SDL_HINT_JOYSTICK_ICADE_DEVICES: a comma-separated list of pairs written
 * VENDOR/PRODUCT, each "0x" or "0X" and then one to four hex digits, in any
 * case. Spaces and tabs around each number, the slash and the commas are
 * ignored, and an empty entry is skipped. Any other character makes its
 * entry malformed. A malformed entry, a vendor of 0, and a pair past
 * SDL_ICADE_MAX_PAIRS are each skipped with one call to log, and the other
 * entries still count. A repeated pair is kept once. Reading stops at
 * length bytes or at a 0 byte. */
#define SDL_ICADE_MAX_PAIRS 32

typedef struct SDL_ICadeDeviceList
{
    int count;
    uint32_t pairs[SDL_ICADE_MAX_PAIRS]; /* The vendor in the high 16 bits */
} SDL_ICadeDeviceList;

typedef void (*SDL_ICadeLogFunc)(void *userdata, const char *entry, size_t length, const char *reason);

/* Returns the number of pairs. A NULL text gives an empty list. */
extern int SDL_ICade_ParseDevices(const char *text, size_t length, SDL_ICadeDeviceList *list, SDL_ICadeLogFunc log, void *userdata);
/* The cabinet's IDs give the cabinet layout even when the list names them.
   A listed pair gives the pad layout. */
extern SDL_ICadeLayout SDL_ICade_Classify(uint16_t vendor, uint16_t product, const SDL_ICadeDeviceList *list);

/* The hand-off from the thread that decodes to the joystick thread, one
 * entry per change of the state. When it is full the oldest entry goes. The
 * driver guards it with its mutex. */
#define SDL_ICADE_QUEUE_CAPACITY 128

typedef struct SDL_ICadeEvent
{
    uint64_t sequence;
    uint64_t time_ns;
    uint16_t state;
} SDL_ICadeEvent;

typedef struct SDL_ICadeQueue
{
    SDL_ICadeEvent events[SDL_ICADE_QUEUE_CAPACITY];
    int head;
    int count;
    uint64_t sequence; /* The last one pushed. Clearing keeps it. */
    uint32_t dropped;
} SDL_ICadeQueue;

extern void SDL_ICade_ClearQueue(SDL_ICadeQueue *queue);
extern void SDL_ICade_PushQueue(SDL_ICadeQueue *queue, uint16_t state, uint64_t time_ns);
extern bool SDL_ICade_PopQueue(SDL_ICadeQueue *queue, SDL_ICadeEvent *event);

#endif /* SDL_icade_proto_h_ */
