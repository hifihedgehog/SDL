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

/* The Namco USIO, the I/O board of the System 357 and 369 cabinets,
 * hifihedgehog/SDL#33 Part 14. Pure C99: no SDL runtime and no I/O, so every
 * decision here runs in the offline tests exactly as it runs in the library.
 *
 * 0B9A:0910 and 0B9A:0900. The board keeps its inputs in registers that the
 * host reads through a command protocol on vendor interface 0. A 6-byte
 * command goes out on bulk OUT 0x01, and the reply comes back on bulk IN 0x82
 * in transfers of up to 64 bytes. The module reads the identification block
 * at register 1800 once, then the input block of the layout the driver names,
 * one read at a time, and sends the next read as soon as the last reply is
 * whole. It never writes a register and never addresses a channel other than
 * 0.
 *
 * No capture of a real board exists. The layouts are the ones that RPCS3
 * (rpcs3/Emu/Io/usio.cpp, GPL-2.0, used for facts only), TaikoZucchini and
 * ITAIKO-firmware (both MIT) serve to the games. No code from them is copied.
 */

#ifndef SDL_hidapi_usio_proto_h_
#define SDL_hidapi_usio_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_USIO_COMMAND_SIZE 6

/* The operation in bits 4 to 7 of command byte 0, with the channel in the low
 * nibble. The board tests for a write first, then a read, then an init
 * (usio.cpp:865-887). The module only ever sends a read. */
#define SDL_USIO_READ  0x10
#define SDL_USIO_WRITE 0x90
#define SDL_USIO_INIT  0xA0

/* Channel 0 registers, and the lengths the emulators build for them */
#define SDL_USIO_REGISTER_TEKKEN 0x1000
#define SDL_USIO_REGISTER_TAIKO  0x1080
#define SDL_USIO_REGISTER_IDENT  0x1800
#define SDL_USIO_TEKKEN_LENGTH   0x180
#define SDL_USIO_TAIKO_LENGTH    0x60
#define SDL_USIO_IDENT_LENGTH    0x180
#define SDL_USIO_MAX_LENGTH      0x180 /* The longest read the module sends */

/* A reply that is not whole this long after its command was written abandons
 * the read. Every source answers at once. RPCS3 completes each transfer 1 ms
 * after it is submitted (usio.cpp:830). ITAIKO-firmware stages the reply
 * inside the command's OUT completion (usio_driver.c:665-667). TaikoZucchini
 * stages it when the command arrives and completes the next IN transfer with
 * it (bpreader_hook.c:482-541, :986-1000). The longest reply is six bulk
 * packets at full speed. The fork's JVS module waits as long for its replies
 * (SDL_serial_jvs_proto.h:44). */
#define SDL_USIO_REPLY_TIMEOUT_NS (100 * 1000000ULL)

/* After an abandoned read, or bytes that no read asked for, the next command
 * waits until no byte has come for this long. A late reply then drains
 * before the next command goes out and cannot be taken as its answer. */
#define SDL_USIO_QUIET_NS (100 * 1000000ULL)

/* Abandoned reads in a row after which the board counts as gone, half a
 * second without an answer. Its joysticks disconnect, so nothing stays held,
 * and the module starts over with the identification block. The JVS module
 * drops its players on a reply timeout the same way
 * (SDL_serial_jvs_proto.c:134-144). */
#define SDL_USIO_MAX_FAILURES 3

/* A drum pad counts as hit from this sensor value up. No source reads a real
 * sensor. On an idle value of 0, RPCS3 writes 1800 for a hit
 * (usio.cpp:217), while TaikoZucchini and ITAIKO-firmware write FFFF
 * (taiko_frame.c:7, usio_driver.c:311). 0C00 is half the lower of the two,
 * midway between rest and the weakest hit any source sends. */
#define SDL_USIO_TAIKO_HIT_THRESHOLD 0x0C00

/* The most coins that wait for their press, the cap the JVS module puts on
 * its coin pulses (SDL_serial_jvs_proto.h:50) */
#define SDL_USIO_MAX_PENDING_COINS 16

typedef enum SDL_USIOLayout
{
    SDL_USIO_LAYOUT_TAIKO,  /* Register 1080, two drums */
    SDL_USIO_LAYOUT_TEKKEN  /* Register 1000, four sticks */
} SDL_USIOLayout;

#define SDL_USIO_MAX_PLAYERS 4
#define SDL_USIO_MAX_AXES    4

/* Buttons of the Taiko joysticks: the four pads, then player 1's panel */
#define SDL_USIO_TAIKO_SIDE_LEFT    0
#define SDL_USIO_TAIKO_CENTER_LEFT  1
#define SDL_USIO_TAIKO_CENTER_RIGHT 2
#define SDL_USIO_TAIKO_SIDE_RIGHT   3
#define SDL_USIO_TAIKO_ENTER        4
#define SDL_USIO_TAIKO_UP           5
#define SDL_USIO_TAIKO_DOWN         6
#define SDL_USIO_TAIKO_SERVICE      7
#define SDL_USIO_TAIKO_TEST         8
#define SDL_USIO_TAIKO_COIN         9

/* Buttons of the Tekken joysticks. Players 1 and 3 add Test, Service and
 * Coin. */
#define SDL_USIO_TEKKEN_BUTTON1 0
#define SDL_USIO_TEKKEN_BUTTON2 1
#define SDL_USIO_TEKKEN_BUTTON3 2
#define SDL_USIO_TEKKEN_BUTTON4 3
#define SDL_USIO_TEKKEN_BUTTON5 4
#define SDL_USIO_TEKKEN_ENTER   5
#define SDL_USIO_TEKKEN_TEST    6
#define SDL_USIO_TEKKEN_SERVICE 7
#define SDL_USIO_TEKKEN_COIN    8

/* Hat bits, equal to SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN and SDL_HAT_LEFT */
#define SDL_USIO_HAT_UP    0x01
#define SDL_USIO_HAT_RIGHT 0x02
#define SDL_USIO_HAT_DOWN  0x04
#define SDL_USIO_HAT_LEFT  0x08

/* Joystick types, equal to SDL_JOYSTICK_TYPE_ARCADE_STICK and
 * SDL_JOYSTICK_TYPE_DRUM_KIT */
#define SDL_USIO_JOYSTICK_ARCADE_STICK 3
#define SDL_USIO_JOYSTICK_DRUM_KIT     7

/* Byte 15 of the joystick GUID for each layout. Both layouts share one USB
 * ID, and the byte keeps their gamepad mappings apart. */
#define SDL_USIO_GUID_TAIKO  0x01
#define SDL_USIO_GUID_TEKKEN 0x02

/* The gamepad mapping of the four Tekken joysticks, RPCS3's default pad
 * assignment turned around (usio_config.h:36-49): the stick is the D-pad,
 * button 1 West, button 2 North, button 3 South, button 4 East, button 5 the
 * right shoulder and Enter Start. Test, Service and Coin stay unmapped. */
#define SDL_USIO_TEKKEN_MAPPING "a:b2,b:b3,x:b0,y:b1,rightshoulder:b4,start:b5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"

typedef struct SDL_USIOControls
{
    int16_t axes[SDL_USIO_MAX_AXES]; /* Taiko: the four pads' sensor values halved */
    uint16_t buttons;                /* Bit i is button i */
    uint8_t hat;                     /* SDL_USIO_HAT_* */
} SDL_USIOControls;

typedef struct SDL_USIOJoystickInfo
{
    const char *name;
    int naxes;
    int nbuttons;
    int nhats;
} SDL_USIOJoystickInfo;

typedef enum SDL_USIOPhase
{
    SDL_USIO_PHASE_IDENTIFY, /* Reading the identification block */
    SDL_USIO_PHASE_POLL,     /* Reading the layout's input block */
    SDL_USIO_PHASE_REJECTED  /* A 0900 board that is not a USIO. Nothing more is sent. */
} SDL_USIOPhase;

typedef struct SDL_USIOState
{
    SDL_USIOLayout layout;
    bool require_ident; /* Use the board only when its identification begins "NBGI." */
    SDL_USIOPhase phase;
    bool connected;     /* The joysticks exist */

    bool waiting;        /* A read is out and its reply is not whole */
    uint16_t length;     /* The length of that reply */
    size_t received;
    uint64_t deadline;   /* Injected clock, in nanoseconds */
    bool quiet;          /* The next command waits for quiet_until */
    uint64_t quiet_until;
    int failures;        /* Abandoned reads in a row, up to SDL_USIO_MAX_FAILURES */
    uint32_t dropped;    /* Bytes dropped since the start, for the log */
    uint8_t reply[SDL_USIO_MAX_LENGTH];

    /* Coin slot 0 is the Taiko counter and the Tekken counter of players 1
     * and 2. Slot 1 is the Tekken counter of players 3 and 4. */
    bool baseline;       /* The counters have a baseline */
    uint16_t coins[2];
    uint8_t pending_coins[2];
    bool coin_pressed[2];

    SDL_USIOControls controls[SDL_USIO_MAX_PLAYERS];
} SDL_USIOState;

/* Where the module reports what changed. Every callback is required. */
typedef struct SDL_USIOSink
{
    void *userdata;
    /* The board is confirmed: add one joystick per player of the layout */
    void (*connect)(void *userdata);
    /* Remove every joystick */
    void (*disconnect)(void *userdata);
    /* A player's controls changed */
    void (*controls)(void *userdata, int player, const SDL_USIOControls *controls);
    /* One line for the driver's log */
    void (*log)(void *userdata, const char *text);
} SDL_USIOSink;

/* Byte 1 of a write command, which RPCS3 and ITAIKO-firmware require to be
 * (NOT (register >> 8)) AND F0 (usio.cpp:868, usio_driver.c:655-656). No
 * source checks byte 1 of a read, and the module sends reads with this byte
 * too. */
extern uint8_t SDL_USIO_CheckByte(uint16_t reg);

/* Builds the command for a read of length bytes at a channel 0 register:
 * operation and channel, the check byte, the register and the length, both
 * 16-bit little-endian. Any other operation, any other channel, and a length
 * of 0 or above SDL_USIO_MAX_LENGTH build nothing and return false. */
extern bool SDL_USIO_EncodeCommand(uint8_t operation, uint8_t channel, uint16_t reg, uint16_t length,
                                   uint8_t command[SDL_USIO_COMMAND_SIZE]);

/* Whether an identification block begins with "NBGI.", 4E 42 47 49 2E
 * (usio.cpp:751-752) */
extern bool SDL_USIO_IsIdentified(const uint8_t *block, size_t length);

/* The layout a value of SDL_HINT_JOYSTICK_HIDAPI_USIO_LAYOUT names: "tekken"
 * in any case, and Taiko for anything else, NULL included. */
extern SDL_USIOLayout SDL_USIO_LayoutFromHint(const char *value);

extern int SDL_USIO_PlayerCount(SDL_USIOLayout layout);
extern const char *SDL_USIO_DeviceName(SDL_USIOLayout layout);
extern uint8_t SDL_USIO_JoystickType(SDL_USIOLayout layout);
extern uint8_t SDL_USIO_GUIDByte(SDL_USIOLayout layout);

/* The name and control counts of one player's joystick. False for a player
 * the layout does not have. */
extern bool SDL_USIO_GetJoystickInfo(SDL_USIOLayout layout, int player, SDL_USIOJoystickInfo *info);

/* The gamepad mapping for GUID byte 15: the Tekken mapping, or NULL, since a
 * drum has no gamepad shape. */
extern const char *SDL_USIO_GetMapping(uint8_t guid_byte);

/* Starts a board. require_ident is true for 0900, whose only link to this
 * protocol is the game's probe (bpreader_hook.c:95-98, :110). */
extern void SDL_USIO_Init(SDL_USIOState *state, SDL_USIOLayout layout, bool require_ident);

/* Hands out the next read when none is out and any quiet wait has passed:
 * the identification block until the board is confirmed, then the layout's
 * input block. */
extern bool SDL_USIO_NextCommand(SDL_USIOState *state, uint64_t now, uint8_t command[SDL_USIO_COMMAND_SIZE]);

/* Completes the write of the command handed out last. written is the byte
 * count the write reported, or a negative value when it failed. The reply
 * wait starts here. A failed or short write abandons the read. */
extern void SDL_USIO_CommandDone(SDL_USIOState *state, int written, uint64_t now, const SDL_USIOSink *sink);

/* Takes one IN transfer. Reads nothing at or past length. A transfer of 0
 * bytes changes nothing. Bytes go to the reply of the read that is out until
 * it holds its length, and the reply is used only when it is whole. Bytes
 * with no read out are dropped and logged, and the next command then waits
 * for quiet. */
extern void SDL_USIO_Feed(SDL_USIOState *state, const uint8_t *data, size_t length, uint64_t now,
                          const SDL_USIOSink *sink);

/* Runs the reply timeout */
extern void SDL_USIO_Tick(SDL_USIOState *state, uint64_t now, const SDL_USIOSink *sink);

/* A player's latest controls, for a joystick opened after they changed. NULL
 * for a player the layout does not have. */
extern const SDL_USIOControls *SDL_USIO_GetControls(const SDL_USIOState *state, int player);

/* The board went away. Disconnects the joysticks and forgets everything. The
 * next attach starts over with the identification block and a new coin
 * baseline. */
extern void SDL_USIO_Detach(SDL_USIOState *state, const SDL_USIOSink *sink);

#endif /* SDL_hidapi_usio_proto_h_ */
