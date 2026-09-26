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

/* Konami's P4IO, 1CCF:8010, the I/O board of jubeat and of the DDR White
 * cabinet, hifihedgehog/SDL#33 Part 14. Pure C99: no SDL runtime and no
 * I/O, so every decision here runs in the offline tests exactly as it runs
 * in the library.
 *
 * The board's first interface has three endpoints in this order: bulk OUT
 * and bulk IN, which carry commands, and interrupt IN, which sends 16-byte
 * input reports with no request. A command is AA, the command, a sequence
 * number and the payload length, then at most 60 payload bytes. A reply has
 * the same four-byte header with the request's sequence. The first four
 * bytes of an input report are a 32-bit little-endian word, and only the
 * cabinet's harness gives its bits a meaning, so the caller picks a layout.
 *
 * No keep-alive is documented. The command set has a watchdog reset, 1C,
 * which bemanitools' emulator answers for the game and neither host driver
 * sends, so nothing here sends it.
 *
 * The facts are restated from bemanitools (Unlicense), p4io-mdxfdrv
 * (GPL-3.0, used for facts only) and arcade-docs. No code from them is
 * copied.
 */

#ifndef SDL_hidapi_konami_p4io_proto_h_
#define SDL_hidapi_konami_p4io_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_KONAMI_P4IO_SOF                 0xAA
#define SDL_KONAMI_P4IO_CMD_INIT            0x00
#define SDL_KONAMI_P4IO_CMD_GET_DEVICE_INFO 0x01
#define SDL_KONAMI_P4IO_HEADER_LENGTH       4
#define SDL_KONAMI_P4IO_MAX_PAYLOAD         60
#define SDL_KONAMI_P4IO_REQUEST_MAX         (SDL_KONAMI_P4IO_HEADER_LENGTH + SDL_KONAMI_P4IO_MAX_PAYLOAD)

/* Every reply is read with 65 bytes. A 64-byte read can stall, and no more
 * than 64 bytes arrive. */
#define SDL_KONAMI_P4IO_READ_LENGTH 65

#define SDL_KONAMI_P4IO_DEVICE_INFO_LENGTH 44
#define SDL_KONAMI_P4IO_REPORT_LENGTH      16
#define SDL_KONAMI_P4IO_ENDPOINTS          3

/* The layouts of SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO_LAYOUT */
#define SDL_KONAMI_P4IO_LAYOUT_RAW    0
#define SDL_KONAMI_P4IO_LAYOUT_JUBEAT 1
#define SDL_KONAMI_P4IO_LAYOUT_DDR    2

/* raw: button N is bit N of the word, as the board sends it */
#define SDL_KONAMI_P4IO_RAW_BUTTONS 32

/* jubeat: buttons 0 to 15 are panels 1 to 16, panel 1 at the top left and
 * panel 16 at the bottom right, then Test and Service */
#define SDL_KONAMI_P4IO_JUBEAT_PANELS  16
#define SDL_KONAMI_P4IO_JUBEAT_TEST    16
#define SDL_KONAMI_P4IO_JUBEAT_SERVICE 17
#define SDL_KONAMI_P4IO_JUBEAT_BUTTONS 18

/* ddr: buttons 0 to 4 are player 1's menu OK, up, down, left and right,
 * buttons 5 to 9 the same for player 2, then Coin, Service and Test. The
 * stage arrows are on another board. */
#define SDL_KONAMI_P4IO_DDR_P2      5
#define SDL_KONAMI_P4IO_DDR_COIN    10
#define SDL_KONAMI_P4IO_DDR_SERVICE 11
#define SDL_KONAMI_P4IO_DDR_TEST    12
#define SDL_KONAMI_P4IO_DDR_BUTTONS 13

typedef struct SDL_KonamiP4IOIdentity
{
    const char *name;
    int nbuttons;
} SDL_KonamiP4IOIdentity;

/* The layout a hint value names: "raw", "jubeat" or "ddr", in any case.
 * NULL, an empty string and every other value give the raw layout. */
extern int SDL_KonamiP4IO_ParseLayout(const char *hint);

/* The joystick a layout makes. Returns false for an unknown layout. */
extern bool SDL_KonamiP4IO_GetIdentity(int layout, SDL_KonamiP4IOIdentity *out);

typedef struct SDL_KonamiP4IOState
{
    uint32_t buttons; /* Bit i is button i */
} SDL_KonamiP4IOState;

/* Before any report: nothing pressed */
extern void SDL_KonamiP4IO_ResetState(SDL_KonamiP4IOState *state);

/* Decodes one interrupt read in a layout. A read shorter than 16 bytes or
 * an unknown layout changes nothing and returns false. A read of 16 bytes
 * or more decodes its first 16, of which the first four carry the inputs. */
extern bool SDL_KonamiP4IO_Decode(int layout, const uint8_t *report, size_t length, SDL_KonamiP4IOState *state);

typedef struct SDL_KonamiP4IOEndpoint
{
    uint8_t address;    /* bEndpointAddress */
    uint8_t attributes; /* bmAttributes */
} SDL_KonamiP4IOEndpoint;

typedef struct SDL_KonamiP4IOEndpoints
{
    uint8_t bulk_out;
    uint8_t bulk_in;
    uint8_t interrupt_in;
} SDL_KonamiP4IOEndpoints;

/* Takes the endpoints of the board's interface in descriptor order. Only
 * exactly three pass: bulk OUT, bulk IN and interrupt IN, in that order. No
 * source records their addresses, so any addresses pass. */
extern bool SDL_KonamiP4IO_FindEndpoints(const SDL_KonamiP4IOEndpoint *endpoints, int count,
                                         SDL_KonamiP4IOEndpoints *out);

/* A request: AA, the command, the sequence, the payload length, then the
 * payload. Returns its length, or 0 for a payload over 60 bytes or a
 * missing one. */
extern size_t SDL_KonamiP4IO_BuildRequest(uint8_t command, uint8_t sequence, const uint8_t *payload, size_t length,
                                          uint8_t out[SDL_KONAMI_P4IO_REQUEST_MAX]);

/* Checks one reply against its request's sequence: AA first, that sequence
 * in byte 2, and a payload of at most 60 bytes that fits in what arrived.
 * Bytes past the payload are ignored. The command byte is not checked, as
 * the host driver in bemanitools does not check it. Reads nothing at or
 * past length. */
extern bool SDL_KonamiP4IO_ParseReply(const uint8_t *reply, size_t length, uint8_t sequence,
                                      const uint8_t **payload, size_t *payload_length);

typedef struct SDL_KonamiP4IODeviceInfo
{
    uint8_t type[4]; /* As sent. Its byte order is not known. */
    uint8_t major;
    uint8_t minor;
    uint8_t revision;
    char product[5]; /* Four characters, BMPU on the firmware arcade-docs lists */
    char date[17];   /* The build date */
    char time[17];   /* The build time */
} SDL_KonamiP4IODeviceInfo;

/* The GET DEVICE INFO payload, exactly 44 bytes: type (4), a pad byte,
 * major, minor, revision, product code (4), build date (16) and build time
 * (16). Each text ends at its first NUL, and a byte outside printable ASCII
 * becomes '?'. Any other length is refused and changes nothing. */
extern bool SDL_KonamiP4IO_ParseDeviceInfo(const uint8_t *payload, size_t length, SDL_KonamiP4IODeviceInfo *out);

#define SDL_KONAMI_P4IO_STARTUP_INIT        0 /* INIT goes out next */
#define SDL_KONAMI_P4IO_STARTUP_DEVICE_INFO 1 /* GET DEVICE INFO goes out next */
#define SDL_KONAMI_P4IO_STARTUP_DONE        2

/* The start-up bemanitools runs: INIT, whose outcome it ignores, then GET
 * DEVICE INFO. The sequence starts at 0 and every request moves it on,
 * answered or not. The input reports need neither command, so the start-up
 * only identifies the board. */
typedef struct SDL_KonamiP4IOStartup
{
    int step;
    uint8_t sequence; /* Of the next request */
    bool identified;  /* GET DEVICE INFO was answered */
    SDL_KonamiP4IODeviceInfo info;
} SDL_KonamiP4IOStartup;

/* Starts over, as for a board that just connected */
extern void SDL_KonamiP4IO_StartupInit(SDL_KonamiP4IOStartup *startup);

/* The next request, or 0 once the start-up is over */
extern size_t SDL_KonamiP4IO_StartupNext(const SDL_KonamiP4IOStartup *startup, uint8_t out[SDL_KONAMI_P4IO_REQUEST_MAX]);

/* The outcome of the request StartupNext gave. exchanged is false when the
 * request did not go out whole or no reply was read. reply and length are
 * what the read returned, which can be zero bytes. INIT's reply is not
 * looked at. Does nothing once the start-up is over. */
extern void SDL_KonamiP4IO_StartupResult(SDL_KonamiP4IOStartup *startup, bool exchanged, const uint8_t *reply,
                                         size_t length);

#endif /* SDL_hidapi_konami_p4io_proto_h_ */
