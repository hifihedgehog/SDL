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

/* Konami's P3IO board of the DDR SuperNova 2 and DDR X cabinets,
 * hifihedgehog/SDL#33 Part 14. Pure C99: no SDL runtime and no I/O, so every
 * decision here runs in the offline tests exactly as it runs in the library.
 *
 * The board, 1CCF:8008, reports the cabinet's inputs on interrupt IN
 * endpoint 83 and takes commands on bulk OUT endpoint 02, answering on bulk
 * IN endpoint 81. A command frame is AA, then the escaped body: the count of
 * the bytes after it, a 4-bit sequence, the command and its payload. A body
 * byte of AA or FF goes out as FF and its complement, and there is no
 * checksum. A reply echoes the sequence and the command, and it can arrive
 * split across reads. The host sends INIT, turns the watchdog off, and then
 * sends GET VERSION as a keep-alive.
 *
 * The facts are restated from bemanitools and OpenITG, which drive the board.
 * No code from them is copied.
 */

#ifndef SDL_hidapi_konami_p3io_proto_h_
#define SDL_hidapi_konami_p3io_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Endpoints */
#define SDL_P3IO_INPUT_IN    0x83 /* Interrupt IN: the input reports */
#define SDL_P3IO_COMMAND_OUT 0x02 /* Bulk OUT: the command frames */
#define SDL_P3IO_COMMAND_IN  0x81 /* Bulk IN: the replies */

/* Framing */
#define SDL_P3IO_START     0xAA /* Opens a frame, and never appears inside one */
#define SDL_P3IO_ESCAPE    0xFF /* The next byte is the complement of a body byte */
#define SDL_P3IO_MAX_BODY  256  /* The length byte and at most 255 bytes after it */
#define SDL_P3IO_SEQUENCE_MASK 0x0F

/* Commands the session sends */
#define SDL_P3IO_CMD_GET_VERSION  0x01
#define SDL_P3IO_CMD_SET_WATCHDOG 0x05
#define SDL_P3IO_CMD_INIT         0x2F
#define SDL_P3IO_WATCHDOG_OFF     0x00

/* Room for any request the session sends: AA and a body of at most 4 bytes,
 * each escaped */
#define SDL_P3IO_REQUEST_SIZE 16

/* The length byte of each reply the session accepts: the sequence, the
 * command and the payload. INIT answers a status, 00 for success. SET
 * WATCHDOG answers one byte of state. GET VERSION answers 4 characters and
 * a major, minor and patch number. */
#define SDL_P3IO_INIT_REPLY_LENGTH     3
#define SDL_P3IO_WATCHDOG_REPLY_LENGTH 3
#define SDL_P3IO_VERSION_REPLY_LENGTH  9
#define SDL_P3IO_VERSION_LENGTH        7

/* Timing, in milliseconds on the caller's clock.
 *
 * With its watchdog on, the board resets 5 to 7 seconds after the last
 * command it received. GET VERSION goes out 1000 ms after each reply, and a
 * reply gets 1000 ms before the session ends, so while the session is up
 * the board gets a command at least every 2 seconds. A failed exchange ends
 * the session, and INIT goes out 1000 ms later, less than 3 seconds after
 * the last command the board took. A board that ignored SET WATCHDOG 00
 * therefore never resets while its commands go through. The pause also
 * keeps a transfer that fails at once from turning into a busy loop. */
#define SDL_P3IO_KEEPALIVE_MS     1000
#define SDL_P3IO_REPLY_TIMEOUT_MS 1000
#define SDL_P3IO_RETRY_MS         1000

/* Builds one request frame: AA, then the escaped body of length, sequence,
 * command and payload, where the length counts the bytes after itself.
 * Returns the frame's length, or 0 when the payload is over 253 bytes or
 * the frame does not fit in out_size. */
extern size_t SDL_P3IO_EncodeRequest(uint8_t sequence, uint8_t command, const uint8_t *payload,
                                     size_t payload_length, uint8_t *out, size_t out_size);

/* Gathers one frame from a byte stream, across any number of reads. Bytes
 * before an AA are skipped, and an AA starts the frame over. FF FF and a
 * length below 2, which leaves no room for the sequence and the command,
 * drop the frame. */
typedef struct SDL_P3IOParser
{
    uint8_t body[SDL_P3IO_MAX_BODY]; /* Length, sequence, command, payload */
    size_t count;                    /* Body bytes decoded so far */
    bool in_frame;                   /* An AA opened a frame that has not ended */
    bool escape;                     /* The previous byte was FF */
} SDL_P3IOParser;

extern void SDL_P3IO_ResetParser(SDL_P3IOParser *parser);

/* Takes one byte. Returns true when it completes a frame, whose length
 * byte plus one bytes are then in body. */
extern bool SDL_P3IO_ParseByte(SDL_P3IOParser *parser, uint8_t byte);

typedef enum SDL_P3IOPhase
{
    SDL_P3IO_PHASE_INIT,     /* INIT goes out, or waits for its reply */
    SDL_P3IO_PHASE_WATCHDOG, /* SET WATCHDOG 00 goes out, or waits for its reply */
    SDL_P3IO_PHASE_UP,       /* GET VERSION goes out on the keep-alive schedule */
    SDL_P3IO_PHASE_LOST      /* An exchange failed, and INIT goes out again after a pause */
} SDL_P3IOPhase;

/* The command channel. One request is out at a time. */
typedef struct SDL_P3IOSession
{
    SDL_P3IOPhase phase;
    uint8_t sequence;         /* Of the next request, 0 to 15 */
    bool awaiting;            /* A request is out and its reply has not come */
    uint8_t awaited_sequence; /* Of the request out */
    uint8_t awaited_command;
    uint64_t sent_at;         /* When the request out went */
    uint64_t due_at;          /* When the next request goes */
    uint32_t ups;             /* Times the session came up. Never reset. */
    uint8_t version[SDL_P3IO_VERSION_LENGTH]; /* The last GET VERSION reply's payload */
    SDL_P3IOParser parser;
} SDL_P3IOSession;

/* Starts a session at now: INIT goes out first, with sequence 0 */
extern void SDL_P3IO_SessionInit(SDL_P3IOSession *session, uint64_t now);

/* Runs the timers. Returns the length of a request frame to send now, or 0.
 * A request with no reply after SDL_P3IO_REPLY_TIMEOUT_MS ends the session,
 * and SDL_P3IO_RETRY_MS later INIT goes out again with sequence 0. */
extern size_t SDL_P3IO_SessionPoll(SDL_P3IOSession *session, uint64_t now, uint8_t *out, size_t out_size);

/* Takes bytes read from bulk IN. Only a reply that echoes the sequence and
 * the command of the request out counts, and any other frame changes
 * nothing. A counted reply of the wrong length, or an INIT status other than
 * 00, ends the session. */
extern void SDL_P3IO_SessionReceive(SDL_P3IOSession *session, const uint8_t *data, size_t length, uint64_t now);

/* A transfer failed: the session ends */
extern void SDL_P3IO_SessionFailed(SDL_P3IOSession *session, uint64_t now);

/* Whether INIT and SET WATCHDOG were answered and nothing has failed since */
extern bool SDL_P3IO_SessionUp(const SDL_P3IOSession *session);

/* Whether a request is out, so its reply is to be read */
extern bool SDL_P3IO_SessionAwaiting(const SDL_P3IOSession *session);

/* When the session next needs a poll: the reply deadline of the request
 * out, or when the next request goes */
extern uint64_t SDL_P3IO_SessionDeadline(const SDL_P3IOSession *session);

/* The input report on interrupt IN 83: 12 bytes, byte 0 always 80, bytes 1
 * to 3 active low, bytes 4 to 11 unused. It arrives whole, or as three
 * 4-byte transfers of which the first starts with 80. */
#define SDL_P3IO_REPORT_LENGTH 12
#define SDL_P3IO_CHUNK_LENGTH  4
#define SDL_P3IO_REPORT_START  0x80

/* Byte 1 for P1 and byte 2 for P2. Bit 5 is unused. */
#define SDL_P3IO_PLAYER_START      0x01
#define SDL_P3IO_PLAYER_UP         0x02
#define SDL_P3IO_PLAYER_DOWN       0x04
#define SDL_P3IO_PLAYER_LEFT       0x08
#define SDL_P3IO_PLAYER_RIGHT      0x10
#define SDL_P3IO_PLAYER_MENU_LEFT  0x40
#define SDL_P3IO_PLAYER_MENU_RIGHT 0x80

/* Byte 3. Bits 0 to 3 are the HD cabinet's menu up and down buttons. Bit 7
 * is unused.
 *
 * Test and Service follow bemanitools: Test is bit 4 and Service bit 6.
 * OpenITG names them the other way around, with a question mark on both.
 * bemanitools' emulator hands these bits to DDR itself, and its
 * configuration tool offers bit 4 as Test and bit 6 as Service, so a swap
 * there would send every user's Test key to Service. A capture settles it. */
#define SDL_P3IO_OPERATOR_P1_MENU_UP   0x01
#define SDL_P3IO_OPERATOR_P1_MENU_DOWN 0x02
#define SDL_P3IO_OPERATOR_P2_MENU_UP   0x04
#define SDL_P3IO_OPERATOR_P2_MENU_DOWN 0x08
#define SDL_P3IO_OPERATOR_HD_MENU      0x0F
#define SDL_P3IO_OPERATOR_TEST         0x10
#define SDL_P3IO_OPERATOR_COIN         0x20
#define SDL_P3IO_OPERATOR_SERVICE      0x40

/* Joystick buttons. P1 has all 12, and P2 the first 9. The arrows are
 * buttons, as on the fork's XID dance pad, so a jump holds two opposite
 * arrows at once. */
#define SDL_P3IO_BUTTON_START      0
#define SDL_P3IO_BUTTON_UP         1
#define SDL_P3IO_BUTTON_DOWN       2
#define SDL_P3IO_BUTTON_LEFT       3
#define SDL_P3IO_BUTTON_RIGHT      4
#define SDL_P3IO_BUTTON_MENU_LEFT  5
#define SDL_P3IO_BUTTON_MENU_RIGHT 6
#define SDL_P3IO_BUTTON_MENU_UP    7
#define SDL_P3IO_BUTTON_MENU_DOWN  8
#define SDL_P3IO_BUTTON_TEST       9
#define SDL_P3IO_BUTTON_SERVICE    10
#define SDL_P3IO_BUTTON_COIN       11
#define SDL_P3IO_PLAYERS    2
#define SDL_P3IO_P1_BUTTONS 12
#define SDL_P3IO_P2_BUTTONS 9

/* The pad arrows on the D-pad, menu start on Start, menu left and right on
 * the shoulders, menu up on North and menu down on South. Both joysticks
 * share the device's GUID and so this mapping, which leaves out Test,
 * Service and Coin because P2 has no such buttons. */
#define SDL_P3IO_MAPPING "a:b8,dpdown:b2,dpleft:b3,dpright:b4,dpup:b1,leftshoulder:b5,rightshoulder:b6,start:b0,y:b7,"

typedef struct SDL_P3IOState
{
    uint16_t buttons[SDL_P3IO_PLAYERS]; /* Bit i is joystick button i */
} SDL_P3IOState;

/* Gathers reports from the interrupt transfers.
 *
 * The HD menu buttons exist only on the HD cabinet, and OpenITG saw their
 * bits read as held through libusbK. Each of the four bits arms once a
 * report shows it released, and it presses nothing before that. A cabinet
 * whose lines read as held never arms them. On an HD cabinet each arms with
 * the first report in which nobody holds that button. */
typedef struct SDL_P3IOInput
{
    uint8_t report[SDL_P3IO_REPORT_LENGTH];
    size_t filled; /* Bytes gathered from 4-byte transfers: 0, 4 or 8 */
    uint8_t armed; /* The HD menu bits of byte 3 that have read released */
} SDL_P3IOInput;

/* Nothing gathered, nothing armed, nothing pressed */
extern void SDL_P3IO_InputInit(SDL_P3IOInput *input, SDL_P3IOState *state);

/* Takes one interrupt transfer. A transfer of 12 bytes or more is a report
 * when it starts with 80, and bytes past 12 are ignored. A 4-byte transfer
 * that starts with 80 starts a report, and two more 4-byte transfers finish
 * it. A 4-byte transfer before the first 80 is dropped. A transfer of any
 * other length drops what was gathered, and an empty one changes nothing.
 * Returns true when a whole report was decoded into state. */
extern bool SDL_P3IO_InputFeed(SDL_P3IOInput *input, const uint8_t *data, size_t length, SDL_P3IOState *state);

/* One endpoint of the active configuration, at alternate setting 0 */
typedef struct SDL_P3IOEndpoint
{
    uint8_t interface_number;
    uint8_t address;
    uint8_t attributes; /* bmAttributes */
} SDL_P3IOEndpoint;

typedef struct SDL_P3IOCommandEndpoints
{
    uint8_t out_interface; /* The interface that holds 02 */
    uint8_t in_interface;  /* The interface that holds 81 */
    bool out_interrupt;    /* 02 is an interrupt endpoint, so it takes interrupt transfers */
    bool in_interrupt;
} SDL_P3IOCommandEndpoints;

/* Finds OUT 02 and IN 81, whatever interface holds them. The references
 * use them as bulk endpoints. An interrupt endpoint at either address is
 * taken too, and its transfers are then interrupt transfers, since Linux
 * refuses a transfer whose type differs from the endpoint's. Fails when
 * either is missing or is a control or isochronous endpoint. */
extern bool SDL_P3IO_FindCommandEndpoints(const SDL_P3IOEndpoint *endpoints, size_t count,
                                          SDL_P3IOCommandEndpoints *found);

#endif /* SDL_hidapi_konami_p3io_proto_h_ */
