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

/* DJI's DUML v1 framing, which DJI remotes speak over their USB serial port,
 * their USB bulk interface and TCP port 40007. Pure C99: no SDL runtime and
 * no I/O.
 *
 * A frame is 55, a 16-bit little-endian word holding the frame length in
 * bits 0-9 and the version, 1, in bits 10-15, a CRC8 of those three bytes,
 * the sender, the receiver, a 16-bit little-endian sequence, the command
 * type, the command set, the command id, the payload and a little-endian
 * CRC16 of everything before it. Over TCP every frame travels behind
 * 55 CC 30 75 and its length as a 32-bit little-endian word.
 *
 * CRC8: reflected polynomial 0x8C, initial value 0x77, no final XOR. CRC16:
 * reflected polynomial 0x8408, initial value 0x3692, no final XOR. Both are
 * computed from these parameters. The facts are from dji-firmware-tools
 * (GPL, facts only), DjiMini2RCasJoystick, DJI-RC-Emulator, dji-rc-linux and
 * DJI_RC_Motion_Bridge.
 */

#ifndef SDL_dji_duml_h_
#define SDL_dji_duml_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_DJI_SOF             0x55
#define SDL_DJI_VERSION         1
#define SDL_DJI_HEADER_LENGTH   11
#define SDL_DJI_MIN_FRAME       13
#define SDL_DJI_MAX_FRAME       1023
#define SDL_DJI_ENVELOPE_LENGTH 8 /* 55 CC 30 75, then the frame length as a 32-bit word */

/* Module types, the low 5 bits of a sender or receiver byte */
#define SDL_DJI_MODULE_APP        0x02
#define SDL_DJI_MODULE_FLIGHT     0x03
#define SDL_DJI_MODULE_RC         0x06
#define SDL_DJI_MODULE_PC         0x0A
#define SDL_DJI_MODULE_HD_GROUND  0x0E

/* Command type: bit 7 marks a response, bits 5-6 ask for an acknowledgement */
#define SDL_DJI_TYPE_RESPONSE 0x80
#define SDL_DJI_TYPE_ACK_MASK 0x60
#define SDL_DJI_TYPE_REQUEST  0x40

typedef struct SDL_DJIFrame
{
    const uint8_t *data; /* The whole frame */
    size_t length;
    uint8_t sender;
    uint8_t receiver;
    uint16_t sequence;
    uint8_t type;
    uint8_t set;
    uint8_t id;
    const uint8_t *payload;
    size_t payload_length;
} SDL_DJIFrame;

extern uint8_t SDL_DJI_CRC8(const uint8_t *data, size_t length);
extern uint16_t SDL_DJI_CRC16(const uint8_t *data, size_t length);

/* Writes one frame. Returns its length, or 0 when it does not fit in size
 * or the payload would make it longer than SDL_DJI_MAX_FRAME. */
extern size_t SDL_DJI_BuildFrame(uint8_t *out, size_t size, uint8_t sender, uint8_t receiver, uint16_t sequence,
                                 uint8_t type, uint8_t set, uint8_t id, const uint8_t *payload, size_t payload_length);

/* An empty response to a request: the addresses swapped, its sequence, its
 * command, type 80. Returns the frame length or 0. */
extern size_t SDL_DJI_BuildResponse(uint8_t *out, size_t size, const SDL_DJIFrame *request);

/* Puts a frame behind the TCP envelope. Returns the envelope length or 0. */
extern size_t SDL_DJI_BuildEnvelope(uint8_t *out, size_t size, const uint8_t *frame, size_t frame_length);

/* Whether data holds exactly one valid frame of this length. Fills frame,
 * which then points into data. */
extern bool SDL_DJI_CheckFrame(const uint8_t *data, size_t length, SDL_DJIFrame *frame);

typedef void (*SDL_DJIFrameHandler)(void *userdata, const SDL_DJIFrame *frame);

/* A stream parser. At a start byte it needs the header CRC8, version 1 and
 * a length of at least 13, then the whole frame and its CRC16. Any failure
 * drops one byte and the search goes on. It keeps at most one frame of the
 * largest size, or one envelope, of bytes that wait for their end. */
typedef struct SDL_DJIParser
{
    bool envelope; /* Frames travel behind the TCP envelope */
    size_t length;
    uint32_t dropped; /* Bytes dropped while searching */
    uint8_t buffer[SDL_DJI_ENVELOPE_LENGTH + SDL_DJI_MAX_FRAME];
} SDL_DJIParser;

extern void SDL_DJIParser_Init(SDL_DJIParser *parser, bool envelope);

/* Parses the bytes fed, calling handler for each frame in order. The frame
 * points into the parser and lasts until the handler returns. */
extern void SDL_DJIParser_Feed(SDL_DJIParser *parser, const uint8_t *data, size_t length,
                               SDL_DJIFrameHandler handler, void *userdata);

#endif /* SDL_dji_duml_h_ */
