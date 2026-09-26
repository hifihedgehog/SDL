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

/* Konami's ACIO node bus, hifihedgehog/SDL#33 Part 14. The BIO2 carries it
 * over USB CDC and the older arcade boards over RS-232, always 8N1 with DTR
 * and RTS on. Pure C99: no SDL runtime and no I/O. The facts are from
 * bemanitools (Unlicense) and arcade-docs (WTFPL).
 *
 * A frame is AA, then the body: the node address, the command as a 16-bit
 * big-endian word, the sequence, the payload length n, n payload bytes and a
 * checksum, the low byte of the sum of every body byte before it. A body
 * byte of AA or FF, the checksum included, travels as FF and its complement.
 * A frame to address 70 is a broadcast, whose body holds only the length and
 * the payload after the address. A reply sets bit 7 of the address, and
 * bemanitools' emulator sends the enumeration reply from 00. The host
 * numbers its frames from 1.
 *
 * The bus below runs inside a serial module (SDL_serial_proto.h). An ACIO
 * module's state begins with an SDL_ACIOState, whose first member is the
 * SDL_SerialBase. The module:
 * - Calls SDL_ACIO_Reset from its Reset entry point, then sets its own
 *   fields, which SDL_ACIO_Reset clears.
 * - Takes SDL_ACIO_Feed, SDL_ACIO_Tick, SDL_ACIO_ActionDone and
 *   SDL_ACIO_GetDeadline as entry points, beside SDL_Serial_NextAction and
 *   SDL_Serial_GetSnapshot.
 * - Learns what the bus does through its SDL_ACIOHandler.
 * - Once Ready is called, runs one exchange at a time with SDL_ACIO_Request,
 *   sends frames that no reply answers with SDL_ACIO_Send, and starts the
 *   bus over with SDL_ACIO_Restart.
 *
 * The bus resets the nodes as bemanitools' aciodrv does: 525 bytes of 00, a
 * line break held for 1450 ms, 1200 ms in which every received byte is
 * dropped, then single AA probes until an AA comes back. It then enumerates
 * the nodes, reads each node's version and starts each node, and calls
 * Ready. A failure before Ready starts the bus over. A frame longer than
 * SDL_SERIAL_MAX_WRITE goes out as several writes back to back.
 *
 * For a board whose rate no source records, the module names a second rate,
 * and each bring-up that fails switches the line to the other one. A node
 * that streams until a reset stops it asks for a reset when the port
 * closes: the zeros and the break go out once more, and the port closes
 * when the break ends.
 */

#ifndef SDL_konami_acio_proto_h_
#define SDL_konami_acio_proto_h_

#include "SDL_serial_proto.h"

#define SDL_ACIO_SOF          0xAA
#define SDL_ACIO_ESCAPE       0xFF
#define SDL_ACIO_REPLY        0x80 /* Address bit of a reply */
#define SDL_ACIO_BROADCAST    0x70
#define SDL_ACIO_HEADER       5    /* Address, command, sequence and length */
#define SDL_ACIO_MAX_PAYLOAD  255
#define SDL_ACIO_MAX_BODY     (SDL_ACIO_HEADER + SDL_ACIO_MAX_PAYLOAD + 1)
#define SDL_ACIO_MAX_FRAME    (1 + 2 * SDL_ACIO_MAX_BODY) /* Every body byte escaped */
#define SDL_ACIO_MAX_NODES    16

/* Commands every node takes */
#define SDL_ACIO_CMD_ENUMERATE   0x0001
#define SDL_ACIO_CMD_GET_VERSION 0x0002
#define SDL_ACIO_CMD_START       0x0003

#define SDL_ACIO_VERSION_LENGTH 44 /* Payload of a version reply */

/* Node types in a version reply */
#define SDL_ACIO_TYPE_ICCA 0x03000000u
#define SDL_ACIO_TYPE_KFCA 0x09060000u
#define SDL_ACIO_TYPE_RVOL 0x09060001u
#define SDL_ACIO_TYPE_MDXF 0x09070000u
#define SDL_ACIO_TYPE_PANB 0x090E0000u
#define SDL_ACIO_TYPE_BI2A 0x0D060000u

/* The reset, from bemanitools */
#define SDL_ACIO_RESET_ZEROS 525
#define SDL_ACIO_BREAK_MS    1450
#define SDL_ACIO_SETTLE_MS   1200
/* The fork's timing. bemanitools probes without a pause and waits for a
 * reply without a limit. */
#define SDL_ACIO_PROBE_MS    10  /* Between single AA probes */
#define SDL_ACIO_PROBES      500 /* Probes before the bus starts over */
#define SDL_ACIO_REPLY_MS    200 /* A reply is due this long after its request left */

/* A received frame. payload points into the decoder. */
typedef struct SDL_ACIOFrame
{
    uint8_t address;
    bool broadcast; /* Address 70, which has no command and no sequence */
    uint16_t command;
    uint8_t sequence;
    uint8_t length;
    const uint8_t *payload;
} SDL_ACIOFrame;

/* Writes AA and the escaped body of one frame. Returns its length, or 0 when
 * it does not fit in size, the payload is longer than 255 bytes, or the
 * address is 70, which the nodes would read as a broadcast. */
extern size_t SDL_ACIO_EncodeFrame(uint8_t address, uint16_t command, uint8_t sequence, const uint8_t *payload, size_t length, uint8_t *out, size_t size);

/* A stream decoder. AA starts a frame wherever it comes, even after FF,
 * since a sender escapes every AA inside one. It keeps a partial frame, and
 * an FF waiting for its complement, across calls. */
typedef struct SDL_ACIODecoder
{
    bool in_frame;
    bool escape; /* The last byte was FF */
    size_t length; /* Body bytes held */
    uint8_t body[SDL_ACIO_MAX_BODY];
    uint32_t bad_checksums;
} SDL_ACIODecoder;

extern void SDL_ACIO_InitDecoder(SDL_ACIODecoder *decoder);

/* Takes one received byte. True when it completes a frame whose checksum
 * holds, which then fills frame. The payload lasts until the next byte. */
extern bool SDL_ACIO_DecodeByte(SDL_ACIODecoder *decoder, uint8_t byte, SDL_ACIOFrame *frame);

typedef struct SDL_ACIONodeVersion
{
    uint32_t type;
    uint8_t flag;
    uint8_t major;
    uint8_t minor;
    uint8_t revision;
    char product[5]; /* Up to 4 characters, then NUL */
    char date[17];
    char time[17];
} SDL_ACIONodeVersion;

/* Decodes a 44-byte version payload: the type as a 32-bit big-endian word,
 * the flag, major, minor and revision, the product code in 4 bytes, and the
 * build date and time in 16 bytes each. Text stops at its first 00, and a
 * byte outside 20 to 7E reads as '?'. False for any other length. */
extern bool SDL_ACIO_DecodeVersion(const uint8_t *payload, size_t length, SDL_ACIONodeVersion *version);

/* What the bus reports to the module. state is the module's state. */
typedef struct SDL_ACIOHandler
{
    /* Bring-up is done: every node answered its version and its start */
    void (*Ready)(void *state, uint64_t now);
    /* The exchange SDL_ACIO_Request started has ended. reply is NULL when
     * no reply came in time, the reply carried another command or a payload
     * length out of range, or the request did not finish leaving. The reply
     * lasts until the handler returns or calls SDL_ACIO_Restart. */
    void (*Reply)(void *state, const SDL_ACIOFrame *reply, uint64_t now);
    /* After Ready, a frame that answers no exchange. May be NULL. */
    void (*Frame)(void *state, const SDL_ACIOFrame *frame, uint64_t now);
    /* The bus starts over, or resets the nodes before the port closes, and
     * the module drops what it presented. May be NULL. */
    void (*Down)(void *state, uint64_t now);
    /* The module's own timer, which stops while the port closes. Either may
     * be NULL. */
    bool (*GetDeadline)(void *state, uint64_t *deadline);
    void (*Tick)(void *state, uint64_t now);
} SDL_ACIOHandler;

typedef enum SDL_ACIOStep
{
    SDL_ACIO_STEP_ZEROS,       /* The 525 bytes of 00 are leaving */
    SDL_ACIO_STEP_BREAK_SET,   /* The break is being set */
    SDL_ACIO_STEP_BREAK,       /* The break holds for 1450 ms */
    SDL_ACIO_STEP_BREAK_CLEAR, /* The break is being cleared */
    SDL_ACIO_STEP_SETTLE,      /* 1200 ms in which every byte is dropped */
    SDL_ACIO_STEP_PROBE,       /* Single AA probes until an AA returns */
    SDL_ACIO_STEP_ENUMERATE,
    SDL_ACIO_STEP_VERSION,
    SDL_ACIO_STEP_START,
    SDL_ACIO_STEP_READY
} SDL_ACIOStep;

typedef struct SDL_ACIOBus
{
    const SDL_ACIOHandler *handler;
    SDL_ACIOStep step;
    uint8_t sequence;    /* Of the next frame sent */
    uint8_t action_tag;  /* The last tag given to an action */
    uint8_t waiting_tag; /* The action whose completion the bus awaits, 0 when none */
    bool timer;
    uint64_t deadline;
    int probes;          /* AA probes sent */
    int nodes;
    int node;            /* The node being asked, from 0 */
    SDL_ACIONodeVersion versions[SDL_ACIO_MAX_NODES];
    uint32_t restarts;   /* Times the bus started over since the port opened */
    uint32_t rate;       /* The line rate */
    uint32_t alternate_rate; /* The rate a failed bring-up switches to, 0 when none */
    bool closing;        /* The port closes when this reset's break ends */

    /* The exchange */
    bool pending;        /* A request waits for its reply */
    uint8_t address;
    uint16_t command;
    size_t min_reply;
    size_t max_reply;

    SDL_ACIODecoder decoder;
} SDL_ACIOBus;

/* The start of every ACIO module's state */
typedef struct SDL_ACIOState
{
    SDL_SerialBase base;
    SDL_ACIOBus bus;
} SDL_ACIOState;

/* For the module's Reset entry point. Clears state_size bytes of state, the
 * module's whole state, sets the sink, queues the line at rate, 8N1 with DTR
 * and RTS on, and starts the reset. The sequence starts at 1. handler must
 * not be NULL, but any of its entries may be. */
extern void SDL_ACIO_Reset(void *state, size_t state_size, const SDL_SerialSink *sink, const SDL_ACIOHandler *handler, uint32_t rate, uint64_t now);

/* Entry points of the module descriptor */
extern void SDL_ACIO_Feed(void *state, const uint8_t *data, size_t length, uint64_t now);
extern void SDL_ACIO_Tick(void *state, uint64_t now);
extern void SDL_ACIO_ActionDone(void *state, bool success, uint64_t now);
extern bool SDL_ACIO_GetDeadline(void *state, uint64_t *deadline);

/* After Ready: sends a request to a node and awaits its reply, which must
 * come from the address, with or without bit 7, carry the same command and
 * hold min_reply to max_reply payload bytes. A frame from that address with
 * another command fails the exchange. False, sending nothing, when the bus
 * is not ready, an exchange is out, the frame cannot be built or the action
 * queue has no room. */
extern bool SDL_ACIO_Request(void *state, uint8_t address, uint16_t command, const uint8_t *payload, size_t length, size_t min_reply, size_t max_reply);

/* After Ready: sends a frame that no reply answers. False, sending nothing,
 * as for SDL_ACIO_Request. */
extern bool SDL_ACIO_Send(void *state, uint8_t address, uint16_t command, const uint8_t *payload, size_t length);

/* Drops every queued action and starts the bus over: Down, then the reset
 * and bring-up. A call during the zeros, the break or the settle time does
 * nothing. */
extern void SDL_ACIO_Restart(void *state, uint64_t now);

/* After Ready, the enumerated nodes, at addresses 1 to the count. Before it,
 * the count is 0 and every node NULL. */
extern int SDL_ACIO_GetNodeCount(const void *state);
extern const SDL_ACIONodeVersion *SDL_ACIO_GetNode(const void *state, int address);

/* After Ready, the highest address whose product code is product, 0 when
 * none */
extern int SDL_ACIO_FindProduct(const void *state, const char *product);

/* After Ready, the highest address whose node type is type, 0 when none */
extern int SDL_ACIO_FindType(const void *state, uint32_t type);

/* After Ready, the lowest address above after whose node type is type, 0
 * when none */
extern int SDL_ACIO_NextType(const void *state, uint32_t type, int after);

/* From the module's Reset entry point, after SDL_ACIO_Reset, for a board
 * whose rate no source records. Each bring-up that fails switches the line
 * between the rate SDL_ACIO_Reset set and this one before the next reset. */
extern void SDL_ACIO_SetAlternateRate(void *state, uint32_t rate);

/* Whether the nodes get one more reset when the port closes, for a node that
 * streams until a reset stops it */
extern void SDL_ACIO_ResetOnClose(void *state, bool on);

#endif /* SDL_konami_acio_proto_h_ */
