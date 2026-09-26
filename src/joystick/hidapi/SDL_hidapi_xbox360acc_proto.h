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

/* The Xbox 360 chatpad on a wired pad and on a wireless pad, and the Xbox
 * 360 uDraw GameTablet (hifihedgehog/SDL#33 Part 15), as pure state
 * machines: C99, no SDL runtime, no I/O, time in milliseconds from an
 * injected clock. SDL_hidapi_xbox360.c and SDL_hidapi_xbox360w.c move bytes
 * between the device and these functions and turn the state into joystick
 * calls.
 *
 * The bytes and sequences are facts from xboxdrv, the Chatpad Super Driver,
 * uDrawTablet, xbox360wirelesschatpad, tusb_xinput, Microsoft's [MS-XUSBI]
 * and brandonw.net. No code from any of them is copied.
 */

#ifndef SDL_hidapi_xbox360acc_proto_h_
#define SDL_hidapi_xbox360acc_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The chatpad joystick's buttons: the four modifiers, then the keys row by
 * row, left to right. 4 to 13 are 1 to 9 and 0, 14 to 23 Q to P, 24 to 33 A
 * to L and comma, 34 to 42 Z to M, period and Enter, 43 to 46 Left, Space,
 * Right and Backspace. */
#define SDL_XBOX360ACC_BUTTON_SHIFT     0
#define SDL_XBOX360ACC_BUTTON_GREEN     1
#define SDL_XBOX360ACC_BUTTON_ORANGE    2
#define SDL_XBOX360ACC_BUTTON_PEOPLE    3
#define SDL_XBOX360ACC_CHATPAD_BUTTONS  47

/* The names of the joysticks the drivers add beside a pad or in its place */
#define SDL_XBOX360ACC_CHATPAD_NAME "Xbox 360 Chatpad"
#define SDL_XBOX360ACC_UDRAW_NAME   "Xbox 360 uDraw GameTablet"

/* The GUID byte 15 of the joysticks the drivers name apart from the pad.
 * Bit 7 is set, which the wireless driver never leaves in a subtype it
 * puts there. The chatpad's is bit 7 over its interface protocol 0x02, the
 * tablet's bit 7 over its subtype 0x23. */
#define SDL_XBOX360ACC_GUID_CHATPAD     0x82
#define SDL_XBOX360ACC_GUID_UDRAW       0xA3

/* The chatpad button a key code names, or -1 for a code no key sends */
extern int SDL_Xbox360Acc_ChatpadButton(uint8_t code);

/* Whether a GUID's vendor, product and byte 15 are those of a chatpad or a
 * tablet joystick, which has no gamepad shape. */
extern bool SDL_Xbox360Acc_IsAccessoryGUID(uint16_t vendor, uint16_t product, uint8_t guid_byte15);

typedef struct SDL_Xbox360AccChatpad
{
    bool present;     /* The chatpad joystick exists */
    uint64_t buttons; /* Bit n holds button n */
} SDL_Xbox360AccChatpad;

/* Whether a lamp effect byte is one the chatpad takes: 00 to 03 turn Shift,
 * Green, Orange and People off, 08 to 0B turn them on, 04 turns the
 * backlight off and 0C on. */
extern bool SDL_Xbox360Acc_ChatpadLampValid(uint8_t effect);

/* The wired chatpad */

/* A wired pad whose chatpad the start-up serves: 045E:028E at bcdDevice
 * 0x0110 or 0x0114, the revisions xboxdrv accepts. */
extern bool SDL_Xbox360Acc_WiredSupported(uint16_t vendor, uint16_t product, uint16_t bcd_device);

/* The chatpad interface: number 2, class 0xFF, subclass 0x5D, protocol 2 */
extern bool SDL_Xbox360Acc_IsChatpadInterface(uint8_t number, uint8_t interface_class,
                                              uint8_t interface_subclass, uint8_t interface_protocol);

typedef struct SDL_Xbox360AccEndpoint
{
    uint8_t address;          /* bEndpointAddress */
    uint8_t attributes;       /* bmAttributes */
    uint16_t max_packet_size; /* wMaxPacketSize */
} SDL_Xbox360AccEndpoint;

/* The biggest chatpad packet a read takes */
#define SDL_XBOX360ACC_CHATPAD_READ_MAX 64

/* The chatpad's input endpoint: the interface's first interrupt IN
 * endpoint. Its packet size, bits 0 to 10 of wMaxPacketSize, must be 5 to
 * SDL_XBOX360ACC_CHATPAD_READ_MAX. Returns the address and sets the size,
 * or returns 0. */
extern uint8_t SDL_Xbox360Acc_ChatpadEndpoint(const SDL_Xbox360AccEndpoint *endpoints, int count, uint16_t *packet_size);

/* One control transfer. A request type with bit 7 set reads length bytes. */
typedef struct SDL_Xbox360AccControl
{
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
    uint8_t data[2]; /* The data stage of a write */
} SDL_Xbox360AccControl;

/* The transfer's 8-byte setup packet, the 16-bit fields little-endian */
extern void SDL_Xbox360Acc_ControlSetup(const SDL_Xbox360AccControl *control, uint8_t setup[8]);

/* The keep-alive period, and the wait before the first keep-alive */
#define SDL_XBOX360ACC_WIRED_KEEPALIVE_MS 1000

#define SDL_XBOX360ACC_WIRED_STOPPED   0
#define SDL_XBOX360ACC_WIRED_KEEPALIVE 10 /* Steps 1 to 9 are the start-up */

typedef struct SDL_Xbox360AccWired
{
    uint16_t bcd_device;
    uint8_t step;          /* The start-up step due next, SDL_XBOX360ACC_WIRED_KEEPALIVE or _STOPPED */
    bool in_flight;        /* A transfer went out and has not completed */
    bool in_flight_extra;  /* That transfer is a 1B the chatpad asked for */
    bool send_1b;          /* The chatpad asked for a 1B */
    bool keepalive_1e;     /* The next keep-alive is 1E, otherwise 1F */
    uint64_t due_ms;       /* When the next step or keep-alive is due */
    SDL_Xbox360AccChatpad chatpad;
} SDL_Xbox360AccWired;

/* Starts the chatpad start-up with step 1 due at now_ms, or restarts it.
 * Step 5 writes 09 00 at bcdDevice 0x0114 and 01 02 otherwise. */
extern void SDL_Xbox360Acc_WiredStart(SDL_Xbox360AccWired *wired, uint16_t bcd_device, uint64_t now_ms);

/* The pad is gone: no more transfers, and the chatpad joystick goes. */
extern void SDL_Xbox360Acc_WiredStop(SDL_Xbox360AccWired *wired);

/* The transfer to send now, if one is due and none is in flight. Each goes
 * out after the previous one completes: steps 1 to 6 back to back, 1F 1000
 * ms after step 6, 1E 1000 ms after that, 1B at once, then 1F and 1E in
 * turn, each 1000 ms after the one before. A 1B the chatpad asked for goes
 * first and moves nothing. */
extern bool SDL_Xbox360Acc_WiredNext(SDL_Xbox360AccWired *wired, uint64_t now_ms, SDL_Xbox360AccControl *control);

/* The transfer from SDL_Xbox360Acc_WiredNext ended at now_ms, completed or
 * not. A stall or any other failure counts as a completion, so the
 * sequence always goes on. */
extern void SDL_Xbox360Acc_WiredDone(SDL_Xbox360AccWired *wired, uint64_t now_ms, bool completed);

/* Whether step 9 has completed, after which the chatpad endpoint is read */
extern bool SDL_Xbox360Acc_WiredReading(const SDL_Xbox360AccWired *wired);

/* One read from the chatpad endpoint. At least 5 bytes: 00, the modifiers,
 * two key codes and 00, or F0 and a status. Either connects the chatpad
 * joystick once step 9 has completed. F0 03 asks for one 1B. Reads nothing
 * at or past length. */
extern void SDL_Xbox360Acc_WiredChatpadReport(SDL_Xbox360AccWired *wired, const uint8_t *data, size_t length);

/* One report from the pad's own interface. 08 03 with bit 0 of byte 2
 * clear says the chatpad is gone. Reads nothing at or past length. */
extern void SDL_Xbox360Acc_WiredPadReport(SDL_Xbox360AccWired *wired, const uint8_t *data, size_t length);

/* A lamp: one effect byte from the valid set, sent as wValue of 41 00 ...
 * 0002. False for any other byte or size. */
extern bool SDL_Xbox360Acc_WiredLamp(const uint8_t *effect, size_t size, SDL_Xbox360AccControl *control);

/* The wireless chatpad and the uDraw, one slot of the receiver */

/* A receiver whose slots the chatpad and uDraw code serves: 045E:0719, and
 * 045E:0291 and 045E:02A9, which the receiver driver treats the same */
extern bool SDL_Xbox360Acc_ReceiverSupported(uint16_t vendor, uint16_t product);

#define SDL_XBOX360ACC_SUBTYPE_UDRAW 0x23

/* What a slot carries */
#define SDL_XBOX360ACC_SLOT_NONE   0
#define SDL_XBOX360ACC_SLOT_PAD    1
#define SDL_XBOX360ACC_SLOT_TABLET 2

/* What SDL_Xbox360Acc_SlotPacket found */
#define SDL_XBOX360ACC_PACKET_OTHER   0
#define SDL_XBOX360ACC_PACKET_STATUS  1 /* 08 and one byte */
#define SDL_XBOX360ACC_PACKET_INFO    2 /* 29 bytes, 00 0F 00 F0 */
#define SDL_XBOX360ACC_PACKET_PAD     3 /* 29 bytes, 00, an odd type */
#define SDL_XBOX360ACC_PACKET_TABLET  4 /* A tablet's state */
#define SDL_XBOX360ACC_PACKET_CHATPAD 5 /* 00 02 00 F0, at least 28 bytes */

#define SDL_XBOX360ACC_SLOT_START_MS     100
#define SDL_XBOX360ACC_SLOT_KEEPALIVE_MS 1000

/* One command on the slot's OUT endpoint: 00 00 0C, the command byte and 8
 * zero bytes */
#define SDL_XBOX360ACC_COMMAND_SIZE 12
#define SDL_XBOX360ACC_COMMAND_QUEUE 16

/* The tablet joystick: 7 axes, 10 buttons, 1 hat */
#define SDL_XBOX360ACC_TABLET_AXES    7
#define SDL_XBOX360ACC_TABLET_BUTTONS 10

#define SDL_XBOX360ACC_TABLET_AXIS_X        0
#define SDL_XBOX360ACC_TABLET_AXIS_Y        1
#define SDL_XBOX360ACC_TABLET_AXIS_PRESSURE 2
#define SDL_XBOX360ACC_TABLET_AXIS_SPREAD   3
#define SDL_XBOX360ACC_TABLET_AXIS_TILT_X   4
#define SDL_XBOX360ACC_TABLET_AXIS_TILT_Y   5
#define SDL_XBOX360ACC_TABLET_AXIS_TILT_Z   6

#define SDL_XBOX360ACC_TABLET_BUTTON_A      0
#define SDL_XBOX360ACC_TABLET_BUTTON_B      1
#define SDL_XBOX360ACC_TABLET_BUTTON_X      2
#define SDL_XBOX360ACC_TABLET_BUTTON_Y      3
#define SDL_XBOX360ACC_TABLET_BUTTON_BACK   4
#define SDL_XBOX360ACC_TABLET_BUTTON_START  5
#define SDL_XBOX360ACC_TABLET_BUTTON_GUIDE  6
#define SDL_XBOX360ACC_TABLET_BUTTON_PEN    7
#define SDL_XBOX360ACC_TABLET_BUTTON_FINGER 8
#define SDL_XBOX360ACC_TABLET_BUTTON_MULTI  9

/* D-pad bits, with the values of SDL's hat bits */
#define SDL_XBOX360ACC_HAT_UP    0x01
#define SDL_XBOX360ACC_HAT_RIGHT 0x02
#define SDL_XBOX360ACC_HAT_DOWN  0x04
#define SDL_XBOX360ACC_HAT_LEFT  0x08

typedef struct SDL_Xbox360AccTablet
{
    int16_t axes[SDL_XBOX360ACC_TABLET_AXES];
    uint16_t buttons; /* Bit n holds button n */
    uint8_t hat;      /* SDL_XBOX360ACC_HAT_* */
} SDL_Xbox360AccTablet;

typedef struct SDL_Xbox360AccSlot
{
    bool chatpad_enabled; /* The chatpad commands go out and its packets count */
    bool udraw_enabled;   /* A subtype 0x23 slot is a tablet */
    bool link;            /* The slot shows a device */
    uint8_t kind;         /* SDL_XBOX360ACC_SLOT_* */
    bool scheduled;       /* The chatpad commands run */
    bool need_init;       /* The next command is 1B */
    bool keepalive_1f;    /* The next keep-alive is 1F, otherwise 1E */
    uint64_t due_ms;      /* When the next command is due */
    uint8_t lamps;        /* Lit: bit 0 Shift, 1 Green, 2 Orange, 3 People */
    uint8_t commands[SDL_XBOX360ACC_COMMAND_QUEUE];
    int command_count;
    SDL_Xbox360AccChatpad chatpad;
    SDL_Xbox360AccTablet tablet;
} SDL_Xbox360AccSlot;

/* An empty slot. chatpad and udraw come from the hints. With udraw off a
 * slot connects as a pad on its 08 status, as the driver did before. */
extern void SDL_Xbox360Acc_SlotInit(SDL_Xbox360AccSlot *slot, bool chatpad, bool udraw);

/* One packet from the slot's receiver interface. Updates the slot's kind,
 * the chatpad and the tablet, and returns the SDL_XBOX360ACC_PACKET_* it
 * found. The kind follows the subtype of the info packet, byte 25 without
 * bit 7: 0x23 makes a tablet, anything else a pad. A pad state packet types
 * a slot no info packet has typed as a pad. A tablet's state packets are
 * decoded. A tablet takes the slot at rest, without a chatpad or lit
 * lamps, and an 08 status with bit 7 clear empties the slot. The 08 status
 * with bit 7 set, the info packet and a pad state packet each start the
 * chatpad commands. Reads nothing at or past length. */
extern int SDL_Xbox360Acc_SlotPacket(SDL_Xbox360AccSlot *slot, uint64_t now_ms, const uint8_t *data, size_t length);

/* Queues the chatpad command that is due: 1B 100 ms after the slot shows a
 * device, then 1E and 1F in turn every 1000 ms, a 1B in place of the next
 * one after F0 03, and after each the lit lamps again. */
extern void SDL_Xbox360Acc_SlotUpdate(SDL_Xbox360AccSlot *slot, uint64_t now_ms);

/* A lamp: one effect byte from the valid set, queued as a command. False
 * for any other byte or size, or with the chatpad off. */
extern bool SDL_Xbox360Acc_SlotLamp(SDL_Xbox360AccSlot *slot, const uint8_t *effect, size_t size);

/* Takes the oldest queued command as the 12 bytes to send. False when none
 * is queued. */
extern bool SDL_Xbox360Acc_SlotTakeCommand(SDL_Xbox360AccSlot *slot, uint8_t packet[SDL_XBOX360ACC_COMMAND_SIZE]);

/* The receiver is gone: the slot empties as on an 08 status with bit 7
 * clear. */
extern void SDL_Xbox360Acc_SlotLost(SDL_Xbox360AccSlot *slot);

/* One tablet state packet: at least 18 bytes starting 00 01 00 F0 00 13.
 * Returns false and changes nothing for any other. X and Y follow the pen
 * or finger only while byte 14 is nonzero and both cells are below 0x0F. */
extern bool SDL_Xbox360Acc_TabletDecode(SDL_Xbox360AccTablet *tablet, const uint8_t *data, size_t length);

#endif /* SDL_hidapi_xbox360acc_proto_h_ */
