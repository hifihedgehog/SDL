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

/* The NaturalPoint TrackIR 2 and TrackIR 3 head trackers, hifihedgehog/SDL#33
 * Part 14. Pure C99: no SDL runtime and no I/O, so every decision here runs in
 * the offline tests exactly as it runs in the library.
 *
 * Each camera has one vendor interface: commands go out on bulk OUT 0x02, and
 * bulk IN 0x82 carries a stream of packets. A packet starts with its total
 * size and a type: 1C holds stripes of bright pixels, 20 is status and 40 is
 * device information. The host starts the camera with a fixed sequence of
 * commands and delays, then joins the stripes of each frame into blobs. This
 * module times that sequence on an injected millisecond clock, parses the
 * stream, and keeps the largest blob of each frame.
 *
 * The protocol facts follow linuxtrack (MIT), which drives both cameras. No
 * code from it is copied.
 */

#ifndef SDL_hidapi_trackir_proto_h_
#define SDL_hidapi_trackir_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_TRACKIR_VENDOR   0x131D
#define SDL_TRACKIR2_PRODUCT 0x0150
#define SDL_TRACKIR3_PRODUCT 0x0155

/* Bytes per bulk IN read, linuxtrack's read buffer. A transfer ends at a short
 * packet or when this many bytes arrived, so only a read of exactly this
 * length can stop inside a packet. */
#define SDL_TRACKIR_READ_SIZE 16384

#define SDL_TRACKIR_COMMAND_MAX 6 /* 23 40 1C 5E 00 00 */

/* A line keeps at most this many stripes, as many as a 440-pixel line holds
 * with a gap after each. linuxtrack sizes its line arrays (width / 2) + 1. */
#define SDL_TRACKIR_LINE_STRIPES 221
/* Every open blob belongs to a stripe of the previous line or of the newest
 * one, so this many slots never run out. */
#define SDL_TRACKIR_OPEN_BLOBS (2 * SDL_TRACKIR_LINE_STRIPES)

/* Start-up timing, in ms */
#define SDL_TRACKIR_FLUSH_READS        3    /* Reads discarded before the information request */
#define SDL_TRACKIR_FLUSH_TIMEOUT_MS   100  /* Each of them ends after this long without data */
#define SDL_TRACKIR_INFO_TRIES         20   /* 17 01 requests before the start-up goes on without an answer */
#define SDL_TRACKIR_INFO_WAIT_MS       6    /* After each request, before reading */
#define SDL_TRACKIR_INFO_WINDOW_MS     100  /* A try ends at a read that completes later than this */
#define SDL_TRACKIR_INFO_READ_MS       1000 /* A read with no data completes after this long */

/* The joystick: the dot's horizontal and vertical offset from the image
 * center and its size in pixels, and one button held while a dot is in view */
#define SDL_TRACKIR_AXES    3
#define SDL_TRACKIR_BUTTONS 1

typedef enum SDL_TrackIRModel
{
    SDL_TRACKIR_NONE,
    SDL_TRACKIR_2, /* 256 x 256 pixels, 3-byte stripes */
    SDL_TRACKIR_3  /* 440 x 314 pixels, 4-byte stripes */
} SDL_TrackIRModel;

typedef struct SDL_TrackIRCommand
{
    uint8_t data[SDL_TRACKIR_COMMAND_MAX];
    size_t length;
} SDL_TrackIRCommand;

/* One stripe of a line, kept for joining the stripes of the next line */
typedef struct SDL_TrackIRRange
{
    uint16_t first;
    uint16_t last;
    uint16_t blob;
} SDL_TrackIRRange;

typedef struct SDL_TrackIRBlob
{
    uint64_t sum_x;  /* The x of every pixel, added */
    uint64_t sum_y;  /* The line of every pixel, added */
    uint32_t pixels;
    bool open;       /* A stripe of the previous or the newest line belongs to it */
    bool matched;    /* A stripe of the newest line joined it */
} SDL_TrackIRBlob;

typedef struct SDL_TrackIRState
{
    SDL_TrackIRModel model;
    uint16_t width;
    uint16_t height;

    /* The command sequences */
    uint8_t phase;
    uint8_t step;
    bool in_flight;       /* A command was handed out and not yet completed */
    bool touched;         /* A command went out to the camera */
    uint64_t ready_at;    /* The current step may start then */
    uint8_t flush_reads;
    uint64_t flush_deadline;
    uint8_t tries;        /* 17 01 requests sent */
    bool awaiting_send;   /* The next request is due at ready_at */
    bool info_seen;       /* A read started 09 40 before the try's reads began */
    uint64_t window_start;
    uint64_t last_read;

    /* The stream */
    uint8_t packet[255];
    size_t packet_length;
    uint8_t skip;         /* Bytes after a size 3E stripe packet still to skip */

    /* The frame */
    uint32_t frame_line;  /* Line of the last stripe, which ends the frame when a lower one comes */
    uint32_t blob_line;   /* Line of the newest ranges */
    SDL_TrackIRRange ranges[2][SDL_TRACKIR_LINE_STRIPES];
    int range_count[2];
    int previous;         /* ranges[previous] holds the line before blob_line */
    SDL_TrackIRBlob blobs[SDL_TRACKIR_OPEN_BLOBS];
    bool have_best;
    SDL_TrackIRBlob best; /* The largest blob the frame closed so far */

    /* What the joystick shows */
    int16_t axes[SDL_TRACKIR_AXES];
    bool in_view;
    uint32_t frames;      /* Frames ended so far */
} SDL_TrackIRState;

/* SDL_TRACKIR_2 or SDL_TRACKIR_3 for 131D:0150 or 131D:0155 */
extern SDL_TrackIRModel SDL_TrackIR_Identify(uint16_t vendor, uint16_t product);

/* "NaturalPoint TrackIR 2", "NaturalPoint TrackIR 3", or NULL */
extern const char *SDL_TrackIR_Name(SDL_TrackIRModel model);

/* Forgets everything and starts the start-up sequence, whose first command
 * is due at now. A reconnect starts again this way. The axes rest at 0 and
 * no dot is in view. An unknown model sends nothing and decodes nothing. */
extern void SDL_TrackIR_Init(SDL_TrackIRState *state, SDL_TrackIRModel model, uint64_t now);

/* Hands out the command due at now, if any. One command is in flight at a
 * time, until SDL_TrackIR_CommandDone completes it. */
extern bool SDL_TrackIR_NextCommand(SDL_TrackIRState *state, uint64_t now, SDL_TrackIRCommand *command);

/* Completes the command in flight. now is the time after the write, which
 * starts the delay that follows the command. A failed write is ignored
 * during the start-up, as linuxtrack ignores it, except for the 17 01
 * request, whose failure ends the start-up for good. A failed write ends the
 * close sequence. */
extern void SDL_TrackIR_CommandDone(SDL_TrackIRState *state, bool written, uint64_t now);

/* Applies one read of the given length. Reads nothing at or past length.
 * During the start-up a read only counts toward the flush or the
 * information wait. Once the camera runs, the read is parsed as the stream.
 * Returns true when at least one frame ended in it. */
extern bool SDL_TrackIR_Feed(SDL_TrackIRState *state, const uint8_t *data, size_t length, uint64_t now);

/* Starts the stop sequence linuxtrack sends when it closes the camera, due
 * at now. When no command ever went out, or the start-up failed, nothing is
 * sent and the state is closed at once. A close already begun or finished is
 * left as it is. */
extern void SDL_TrackIR_BeginClose(SDL_TrackIRState *state, uint64_t now);

extern bool SDL_TrackIR_IsRunning(const SDL_TrackIRState *state);
extern bool SDL_TrackIR_IsClosed(const SDL_TrackIRState *state);

/* The time of the next command or timer, while a sequence runs and no
 * command is in flight. */
extern bool SDL_TrackIR_GetDeadline(const SDL_TrackIRState *state, uint64_t *deadline);

#endif /* SDL_hidapi_trackir_proto_h_ */
