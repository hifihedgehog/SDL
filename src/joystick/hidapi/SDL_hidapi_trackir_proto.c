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

/* The NaturalPoint TrackIR 2 and TrackIR 3. See SDL_hidapi_trackir_proto.h.
 * Line numbers refer to linuxtrack's src folder at 205f8d6. */

#include "SDL_hidapi_trackir_proto.h"

#include <string.h>

enum
{
    TRACKIR_STARTING,
    TRACKIR_RUNNING,
    TRACKIR_FAILED,
    TRACKIR_CLOSING,
    TRACKIR_CLOSED
};

enum
{
    TRACKIR_SEND,  /* One command, then the delay */
    TRACKIR_FLUSH, /* Three reads, each ending with data or after 100 ms */
    TRACKIR_INFO   /* The 17 01 request until a read starts 09 40, then the delay */
};

/* No line yet. One more than it is not a line either. */
#define TRACKIR_NO_LINE 0xFFFFFFFEu

typedef struct TrackIR_Step
{
    uint8_t kind;
    uint8_t length;
    uint8_t data[SDL_TRACKIR_COMMAND_MAX];
    uint16_t delay; /* ms from the end of the step to the start of the next */
} TrackIR_Step;

#define TRACKIR_ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))

/* A light command is 10, the lights to turn on, then the lights it sets:
 * 10 red, 20 green, 40 blue, 80 infrared (tir_hw.c:393-414, tir_hw.h:7-10).
 * linuxtrack waits 2 ms after each (tir_hw.c:400), and the delays below
 * include those 2 ms. The infrared lights go on, as linuxtrack turns them on
 * for reflective markers (tir_driver.c:66). The red and green status lights
 * that linuxtrack sets during the start when its status option is on
 * (tir_hw.c:684-687, :708-711) are not sent. */

/* init_camera_tir2, then start_camera_tir2 (tir_hw.c:779-815, :673-693),
 * then the threshold the first frame read sends (tir_driver.c:104-109). */
static const TrackIR_Step TrackIR2_Start[] = {
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x80 }, 4 },  /* Infrared off */
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x10 }, 4 },  /* Red off */
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x20 }, 4 },  /* Green off */
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x40 }, 4 },  /* Blue off */
    { TRACKIR_SEND, 2, { 0x14, 0x01 }, 2 },        /* Video off */
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x80 }, 4 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x10 }, 4 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x20 }, 4 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x40 }, 4 },
    { TRACKIR_FLUSH, 0, { 0 }, 0 },
    { TRACKIR_INFO, 0, { 0 }, 70 },
    { TRACKIR_SEND, 3, { 0x15, 0x8C, 0x01 }, 2 },  /* Threshold 140 */
    { TRACKIR_SEND, 2, { 0x14, 0x00 }, 120 },      /* Video on */
    { TRACKIR_SEND, 3, { 0x10, 0x80, 0x80 }, 66 }, /* Infrared on */
    { TRACKIR_SEND, 1, { 0x12 }, 64 },             /* Flush the FIFO */
    { TRACKIR_SEND, 3, { 0x10, 0x80, 0x80 }, 66 },
    { TRACKIR_SEND, 3, { 0x15, 0x8C, 0x01 }, 0 },
};

/* init_camera_tir3, which starts with stop_camera_tir3, then
 * start_camera_tir3 (tir_hw.c:817-846, :564-585, :695-713), then the
 * threshold the first frame read sends (tir_driver.c:104-109). */
static const TrackIR_Step TrackIR3_Start[] = {
    { TRACKIR_SEND, 2, { 0x14, 0x01 }, 2 },        /* Video off */
    { TRACKIR_SEND, 2, { 0x14, 0x01 }, 2 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x80 }, 4 },
    { TRACKIR_SEND, 1, { 0x12 }, 2 },              /* Flush the FIFO */
    { TRACKIR_SEND, 1, { 0x13 }, 2 },              /* Camera stop */
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x10 }, 4 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x20 }, 4 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x40 }, 52 },
    { TRACKIR_FLUSH, 0, { 0 }, 0 },
    { TRACKIR_INFO, 0, { 0 }, 70 },
    { TRACKIR_SEND, 4, { 0x15, 0x8C, 0x01, 0x00 }, 70 },             /* Threshold 140 */
    { TRACKIR_SEND, 6, { 0x23, 0x40, 0x1C, 0x5E, 0x00, 0x00 }, 2 }, /* Unknown, sent as linuxtrack sends it */
    { TRACKIR_SEND, 6, { 0x23, 0x40, 0x1D, 0x01, 0x00, 0x00 }, 2 }, /* Unknown */
    { TRACKIR_SEND, 4, { 0x15, 0xD0, 0x01, 0x00 }, 2 },             /* Threshold 208 */
    { TRACKIR_SEND, 5, { 0x19, 0x03, 0x03, 0x00, 0x00 }, 2 },       /* Unknown */
    { TRACKIR_SEND, 1, { 0x12 }, 2 },
    { TRACKIR_SEND, 1, { 0x13 }, 2 },
    { TRACKIR_SEND, 3, { 0x10, 0x80, 0x80 }, 4 },
    { TRACKIR_SEND, 2, { 0x14, 0x00 }, 2 },
    { TRACKIR_SEND, 4, { 0x15, 0x8C, 0x01, 0x00 }, 0 },
};

/* close_camera_tir2, which starts with stop_camera_tir2 (tir_hw.c:1156-1171,
 * :546-561). */
static const TrackIR_Step TrackIR2_Close[] = {
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x10 }, 72 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x20 }, 72 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x40 }, 72 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x80 }, 72 },
    { TRACKIR_SEND, 1, { 0x13 }, 70 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x80 }, 27 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x10 }, 4 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x20 }, 4 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x40 }, 4 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x80 }, 4 },
};

/* close_camera_tir3, which starts with stop_camera_tir3 (tir_hw.c:1173-1187,
 * :564-585). */
static const TrackIR_Step TrackIR3_Close[] = {
    { TRACKIR_SEND, 2, { 0x14, 0x01 }, 2 },
    { TRACKIR_SEND, 2, { 0x14, 0x01 }, 2 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x80 }, 4 },
    { TRACKIR_SEND, 1, { 0x12 }, 2 },
    { TRACKIR_SEND, 1, { 0x13 }, 2 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x10 }, 4 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x20 }, 4 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x40 }, 52 },
    { TRACKIR_SEND, 5, { 0x19, 0x15, 0x10, 0x40, 0x00 }, 25 }, /* Unknown */
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x10 }, 4 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x20 }, 4 },
    { TRACKIR_SEND, 3, { 0x10, 0x00, 0x40 }, 4 },
};

SDL_TrackIRModel SDL_TrackIR_Identify(uint16_t vendor, uint16_t product)
{
    if (vendor == SDL_TRACKIR_VENDOR) {
        if (product == SDL_TRACKIR2_PRODUCT) {
            return SDL_TRACKIR_2;
        }
        if (product == SDL_TRACKIR3_PRODUCT) {
            return SDL_TRACKIR_3;
        }
    }
    return SDL_TRACKIR_NONE;
}

const char *SDL_TrackIR_Name(SDL_TrackIRModel model)
{
    switch (model) {
    case SDL_TRACKIR_2:
        return "NaturalPoint TrackIR 2";
    case SDL_TRACKIR_3:
        return "NaturalPoint TrackIR 3";
    default:
        return NULL;
    }
}

/* The current step of the start-up or the close, or NULL past the last one */
static const TrackIR_Step *TrackIR_CurrentStep(const SDL_TrackIRState *state)
{
    const bool closing = (state->phase == TRACKIR_CLOSING);
    const TrackIR_Step *steps;
    size_t count;

    if (state->model == SDL_TRACKIR_2) {
        steps = closing ? TrackIR2_Close : TrackIR2_Start;
        count = closing ? TRACKIR_ARRAYSIZE(TrackIR2_Close) : TRACKIR_ARRAYSIZE(TrackIR2_Start);
    } else {
        steps = closing ? TrackIR3_Close : TrackIR3_Start;
        count = closing ? TRACKIR_ARRAYSIZE(TrackIR3_Close) : TRACKIR_ARRAYSIZE(TrackIR3_Start);
    }
    return (state->step < count) ? &steps[state->step] : NULL;
}

static bool TrackIR_Sequencing(const SDL_TrackIRState *state)
{
    return state->phase == TRACKIR_STARTING || state->phase == TRACKIR_CLOSING;
}

/* Moves to the next step, which may start at ready_at. The flush and the
 * information wait each come once after SDL_TrackIR_Init, which zeroes their
 * counters. */
static void TrackIR_NextStep(SDL_TrackIRState *state, uint64_t ready_at)
{
    const TrackIR_Step *step;

    ++state->step;
    state->ready_at = ready_at;
    step = TrackIR_CurrentStep(state);
    if (step && step->kind == TRACKIR_FLUSH) {
        state->flush_deadline = ready_at + SDL_TRACKIR_FLUSH_TIMEOUT_MS;
    } else if (step && step->kind == TRACKIR_INFO) {
        state->awaiting_send = true;
    }
}

/* The information wait is over at when, answered or not */
static void TrackIR_InfoDone(SDL_TrackIRState *state, uint64_t when)
{
    TrackIR_NextStep(state, when + TrackIR_CurrentStep(state)->delay);
}

/* One try of the request ended at when without the answer. After the last
 * try linuxtrack goes on as if it had answered (tir_hw.c:239, :264-266). */
static void TrackIR_EndTry(SDL_TrackIRState *state, uint64_t when)
{
    if (state->tries >= SDL_TRACKIR_INFO_TRIES) {
        TrackIR_InfoDone(state, when);
    } else {
        state->awaiting_send = true;
        state->ready_at = when;
    }
}

/* Runs the timers of the current step up to now */
static void TrackIR_Advance(SDL_TrackIRState *state, uint64_t now)
{
    while (TrackIR_Sequencing(state) && !state->in_flight) {
        const TrackIR_Step *step = TrackIR_CurrentStep(state);

        if (!step) {
            if (now >= state->ready_at) {
                state->phase = (state->phase == TRACKIR_STARTING) ? TRACKIR_RUNNING : TRACKIR_CLOSED;
            }
            return;
        }
        if (step->kind == TRACKIR_FLUSH) {
            uint64_t completed;

            if (now < state->flush_deadline) {
                return;
            }
            /* A flush read that gets no data ends after 100 ms
               (tir_hw.c:805-807, :828-830) */
            completed = state->flush_deadline;
            state->flush_deadline += SDL_TRACKIR_FLUSH_TIMEOUT_MS;
            if (++state->flush_reads >= SDL_TRACKIR_FLUSH_READS) {
                TrackIR_NextStep(state, completed + step->delay);
            }
        } else if (step->kind == TRACKIR_INFO && !state->awaiting_send) {
            if (state->info_seen && now >= state->window_start) {
                /* An answer that came before the try's reads began is read
                   when they begin. One that comes later is taken at once. */
                TrackIR_InfoDone(state, state->window_start);
            } else if (now >= state->last_read + SDL_TRACKIR_INFO_READ_MS) {
                /* A read that gets no data ends after 1000 ms, which is past
                   the try's 100 ms (tir_hw.c:247, :254-259) */
                TrackIR_EndTry(state, state->last_read + SDL_TRACKIR_INFO_READ_MS);
            } else {
                return;
            }
        } else {
            return; /* Waiting for SDL_TrackIR_NextCommand */
        }
    }
}

/* A read during the start-up */
static void TrackIR_StartupRead(SDL_TrackIRState *state, const uint8_t *data, size_t length, uint64_t now)
{
    const TrackIR_Step *step = TrackIR_CurrentStep(state);

    if (!step) {
        return;
    }
    if (step->kind == TRACKIR_FLUSH) {
        /* A read that came before the flush began is dropped uncounted */
        if (now >= state->ready_at) {
            state->flush_deadline = now + SDL_TRACKIR_FLUSH_TIMEOUT_MS;
            if (++state->flush_reads >= SDL_TRACKIR_FLUSH_READS) {
                TrackIR_NextStep(state, now + step->delay);
            }
        }
    } else if (step->kind == TRACKIR_INFO) {
        if (length >= 2 && data[0] == 0x09 && data[1] == 0x40) {
            /* The device information answers the request (tir_hw.c:251, :261) */
            state->info_seen = true;
            if (!state->awaiting_send && now >= state->window_start) {
                TrackIR_InfoDone(state, now);
            }
        } else if (!state->awaiting_send) {
            /* Any other read completes one of the try's reads. The try ends
               at the first that completes more than 100 ms after its reads
               began (tir_hw.c:254-259). */
            const uint64_t completed = (now > state->window_start) ? now : state->window_start;

            if (completed - state->window_start > SDL_TRACKIR_INFO_WINDOW_MS) {
                TrackIR_EndTry(state, completed);
            } else {
                state->last_read = completed;
            }
        }
    }
}

/* x' = (size - 1) / 2 - mean, the offset linuxtrack reports
 * (image_process.c:486-491), times 65535 / size, rounded half away from
 * zero. The highest value, at mean 0, stays below 32767. A mean past the
 * last pixel, which image_process.c:272-288 lets in, can pass -32768. */
static int16_t TrackIR_Axis(uint64_t sum, uint32_t pixels, uint16_t size)
{
    const int64_t numerator = ((int64_t)(size - 1) * pixels - 2 * (int64_t)sum) * 65535;
    const int64_t denominator = 2 * (int64_t)pixels * size;
    int64_t value;

    if (numerator >= 0) {
        value = (numerator + denominator / 2) / denominator;
    } else {
        value = -((-numerator + denominator / 2) / denominator);
    }
    if (value < -32768) {
        value = -32768;
    }
    return (int16_t)value;
}

/* A blob is done. The largest of the frame wins, and on a tie the one
 * closed later, as pose.c:414-431 keeps the later index. */
static void TrackIR_Close(SDL_TrackIRState *state, SDL_TrackIRBlob *blob)
{
    blob->open = false;
    if (!state->have_best || blob->pixels >= state->best.pixels) {
        state->best = *blob;
        state->have_best = true;
    }
}

/* Closes blobs in the order linuxtrack stores them (image_process.c:219-262):
 * those of the previous line that no stripe of the newest line joined, then,
 * with all, every blob of the newest line. */
static void TrackIR_StoreBlobs(SDL_TrackIRState *state, bool all)
{
    const int newest = 1 - state->previous;
    int i;

    for (i = 0; i < state->range_count[state->previous]; ++i) {
        SDL_TrackIRBlob *blob = &state->blobs[state->ranges[state->previous][i].blob];

        if (!blob->matched && blob->open) {
            TrackIR_Close(state, blob);
        }
    }
    for (i = 0; i < state->range_count[newest]; ++i) {
        SDL_TrackIRBlob *blob = &state->blobs[state->ranges[newest][i].blob];

        blob->matched = false;
        if (all && blob->open) {
            TrackIR_Close(state, blob);
        }
    }
}

static void TrackIR_EndFrame(SDL_TrackIRState *state)
{
    TrackIR_StoreBlobs(state, true);
    state->range_count[0] = 0;
    state->range_count[1] = 0;
    state->blob_line = TRACKIR_NO_LINE;
    ++state->frames;
    state->in_view = state->have_best;
    if (state->have_best) {
        state->axes[0] = TrackIR_Axis(state->best.sum_x, state->best.pixels, state->width);
        state->axes[1] = TrackIR_Axis(state->best.sum_y, state->best.pixels, state->height);
        state->axes[2] = (int16_t)((state->best.pixels > 32767) ? 32767 : state->best.pixels);
    }
    state->have_best = false;
}

/* A free slot. One always exists: see SDL_TRACKIR_OPEN_BLOBS. linuxtrack
 * marks a new blob matched (image_process.c:212), which nothing reads before
 * the next line clears it. */
static int TrackIR_NewBlob(SDL_TrackIRState *state)
{
    int i;

    for (i = 0; i < SDL_TRACKIR_OPEN_BLOBS; ++i) {
        SDL_TrackIRBlob *blob = &state->blobs[i];

        if (!blob->open) {
            memset(blob, 0, sizeof(*blob));
            blob->open = true;
            return i;
        }
    }
    return -1;
}

static void TrackIR_Merge(SDL_TrackIRState *state, int into, int from)
{
    int list, i;

    state->blobs[into].sum_x += state->blobs[from].sum_x;
    state->blobs[into].sum_y += state->blobs[from].sum_y;
    state->blobs[into].pixels += state->blobs[from].pixels;
    state->blobs[from].open = false;
    for (list = 0; list < 2; ++list) {
        for (i = 0; i < state->range_count[list]; ++i) {
            if (state->ranges[list][i].blob == from) {
                state->ranges[list][i].blob = (uint16_t)into;
            }
        }
    }
}

/* One stripe of the frame, joined into blobs as image_process.c:264-376
 * joins them */
static void TrackIR_AddStripe(SDL_TrackIRState *state, uint32_t line, uint32_t first, uint32_t last)
{
    SDL_TrackIRRange *range;
    SDL_TrackIRBlob *blob;
    uint32_t pixels;
    int newest, i, target = -1;

    /* A stripe past the sensor, or one that ends before it starts, is
       ignored. A line or x equal to the size is let in, as linuxtrack
       lets it in (image_process.c:272-298). */
    if (line > state->height || first > state->width || last > state->width || first > last) {
        return;
    }
    pixels = last - first + 1;
    if (state->blob_line != line) {
        if (state->blob_line + 1 != line) {
            /* A gap or a new frame closes every blob */
            TrackIR_StoreBlobs(state, true);
            state->range_count[0] = 0;
            state->range_count[1] = 0;
        } else {
            /* The newest line becomes the previous one */
            TrackIR_StoreBlobs(state, false);
            state->previous = 1 - state->previous;
            state->range_count[1 - state->previous] = 0;
        }
        state->blob_line = line;
    }
    newest = 1 - state->previous;
    if (state->range_count[newest] >= SDL_TRACKIR_LINE_STRIPES) {
        return; /* The line is full, and the stripe is ignored */
    }

    /* The stripe joins every blob of the previous line whose stripe it
       overlaps or touches, diagonally too (image_process.c:161-178,
       :339-363) */
    for (i = 0; i < state->range_count[state->previous]; ++i) {
        const SDL_TrackIRRange *other = &state->ranges[state->previous][i];

        if ((int32_t)first - 1 <= (int32_t)other->last && last + 1 >= other->first) {
            state->blobs[other->blob].matched = true;
            if (target < 0) {
                target = other->blob;
            } else if (other->blob != target) {
                TrackIR_Merge(state, target, other->blob);
            }
        }
    }
    if (target < 0) {
        target = TrackIR_NewBlob(state);
        if (target < 0) {
            return;
        }
    }

    /* The x of the stripe's pixels add up to pixels * first plus
       pixels * (pixels - 1) / 2 (tir_img.c:22-24, image_process.c:198-201) */
    blob = &state->blobs[target];
    blob->sum_x += (uint64_t)pixels * first + (uint64_t)pixels * (pixels - 1) / 2;
    blob->sum_y += (uint64_t)pixels * line;
    blob->pixels += pixels;

    range = &state->ranges[newest][state->range_count[newest]++];
    range->first = (uint16_t)first;
    range->last = (uint16_t)last;
    range->blob = (uint16_t)target;
}

/* The TrackIR 2 sends line, first x and last x. The TrackIR 3 adds flags:
 * 20 adds 100 hex to the line, 80 adds 100 hex to the first x and 10 adds
 * 200 hex, 40 adds 100 hex to the last x and 08 adds 200 hex
 * (tir_img.c:16-29, :124-158). */
static void TrackIR_Stripe(SDL_TrackIRState *state, const uint8_t *stripe)
{
    const bool tir3 = (state->model == SDL_TRACKIR_3);
    uint32_t line = stripe[0];
    uint32_t first = stripe[1];
    uint32_t last = stripe[2];

    if (line == 0 && first == 0 && last == 0 && (!tir3 || stripe[3] == 0)) {
        /* An all-zero stripe ends the frame (tir_img.c:365-370, :403-408) */
        TrackIR_EndFrame(state);
        state->frame_line = 0;
        return;
    }
    if (tir3) {
        if (stripe[3] & 0x20) {
            line |= 0x100;
        }
        if (stripe[3] & 0x80) {
            first |= 0x100;
        }
        if (stripe[3] & 0x10) {
            first |= 0x200;
        }
        if (stripe[3] & 0x40) {
            last |= 0x100;
        }
        if (stripe[3] & 0x08) {
            last |= 0x200;
        }
    }
    /* A line lower than the one before starts the next frame, whether or
       not the stripe lands on the sensor (tir_img.c:160-186) */
    if (line < state->frame_line) {
        TrackIR_EndFrame(state);
    }
    state->frame_line = line;
    TrackIR_AddStripe(state, line, first, last);
}

/* A whole packet. Only a stripe packet changes anything. */
static void TrackIR_Packet(SDL_TrackIRState *state)
{
    const size_t size = state->packet[0];
    const size_t stride = (state->model == SDL_TRACKIR_3) ? 4 : 3;
    size_t offset;

    if (state->packet[1] != 0x1C) {
        return;
    }
    /* Whole stripes only. linuxtrack reads a stripe that runs past the
       packet's end, and the fork ignores the part stripe instead. */
    for (offset = 2; offset + stride <= size; offset += stride) {
        TrackIR_Stripe(state, &state->packet[offset]);
    }
    if (size == 0x3E) {
        /* Two bytes follow a stripe packet of this size
           (tir_img.c:385-387, :424-426) */
        state->skip = 2;
    }
}

static bool TrackIR_KnownType(uint8_t type)
{
    /* Stripes, status and device information (tir_img.c:448-455) */
    return type == 0x1C || type == 0x20 || type == 0x40;
}

/* One read of the running camera's stream */
static bool TrackIR_Parse(SDL_TrackIRState *state, const uint8_t *data, size_t length)
{
    const uint32_t frames = state->frames;
    size_t i = 0;

    while (i < length) {
        if (state->skip) {
            --state->skip;
            ++i;
        } else if (state->packet_length < 2) {
            state->packet[state->packet_length++] = data[i++];
            if (state->packet_length == 2 && (!TrackIR_KnownType(state->packet[1]) || state->packet[0] < 2)) {
                /* An unknown type drops the rest of the read that brought
                   the type byte (tir_img.c:471-486). So does a size too
                   short for the header, which linuxtrack cannot parse. */
                state->packet_length = 0;
                break;
            }
        } else {
            size_t take = (size_t)state->packet[0] - state->packet_length;

            if (take > length - i) {
                take = length - i;
            }
            memcpy(&state->packet[state->packet_length], &data[i], take);
            state->packet_length += take;
            i += take;
        }
        if (state->packet_length >= 2 && state->packet_length == state->packet[0]) {
            TrackIR_Packet(state);
            state->packet_length = 0;
        }
    }

    /* A read shorter than a whole transfer ended at a short packet, so
       nothing of a packet it cut comes in the next read. A read of the
       whole transfer can end inside a packet, which the next read
       finishes. */
    if (length != SDL_TRACKIR_READ_SIZE) {
        state->packet_length = 0;
        state->skip = 0;
    }
    return state->frames != frames;
}

void SDL_TrackIR_Init(SDL_TrackIRState *state, SDL_TrackIRModel model, uint64_t now)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->model = model;
    state->ready_at = now;
    state->blob_line = TRACKIR_NO_LINE;
    if (model == SDL_TRACKIR_2) {
        /* tir_hw.c:1236-1242 */
        state->width = 256;
        state->height = 256;
        state->phase = TRACKIR_STARTING;
    } else if (model == SDL_TRACKIR_3) {
        /* tir_hw.c:1244-1250 */
        state->width = 440;
        state->height = 314;
        state->phase = TRACKIR_STARTING;
    } else {
        state->phase = TRACKIR_FAILED;
    }
}

bool SDL_TrackIR_NextCommand(SDL_TrackIRState *state, uint64_t now, SDL_TrackIRCommand *command)
{
    const TrackIR_Step *step;

    if (!state || !command) {
        return false;
    }
    TrackIR_Advance(state, now);
    if (!TrackIR_Sequencing(state) || state->in_flight) {
        return false;
    }
    step = TrackIR_CurrentStep(state);
    if (!step || now < state->ready_at) {
        return false;
    }
    if (step->kind == TRACKIR_SEND) {
        memcpy(command->data, step->data, sizeof(command->data));
        command->length = step->length;
    } else if (step->kind == TRACKIR_INFO && state->awaiting_send) {
        /* The device information request (tir_hw.c:46, :240) */
        memset(command->data, 0, sizeof(command->data));
        command->data[0] = 0x17;
        command->data[1] = 0x01;
        command->length = 2;
    } else {
        return false;
    }
    state->in_flight = true;
    state->touched = true;
    return true;
}

void SDL_TrackIR_CommandDone(SDL_TrackIRState *state, bool written, uint64_t now)
{
    const TrackIR_Step *step;

    if (!state || !state->in_flight) {
        return;
    }
    state->in_flight = false;
    step = TrackIR_CurrentStep(state);
    if (!step) {
        return;
    }
    if (step->kind == TRACKIR_INFO) {
        if (!written) {
            /* linuxtrack gives the camera up when the request fails
               (tir_hw.c:240-243, :808-810, :831-833) */
            state->phase = TRACKIR_FAILED;
            return;
        }
        ++state->tries;
        state->awaiting_send = false;
        state->window_start = now + SDL_TRACKIR_INFO_WAIT_MS;
        state->last_read = state->window_start;
        return;
    }
    if (!written && state->phase == TRACKIR_CLOSING) {
        /* The camera is gone, and the rest of the close would only wait */
        state->phase = TRACKIR_CLOSED;
        return;
    }
    TrackIR_NextStep(state, now + step->delay);
}

bool SDL_TrackIR_Feed(SDL_TrackIRState *state, const uint8_t *data, size_t length, uint64_t now)
{
    if (!state || (!data && length)) {
        return false;
    }
    TrackIR_Advance(state, now);
    if (state->phase == TRACKIR_STARTING) {
        TrackIR_StartupRead(state, data, length, now);
    } else if (state->phase == TRACKIR_RUNNING) {
        return TrackIR_Parse(state, data, length);
    }
    return false;
}

void SDL_TrackIR_BeginClose(SDL_TrackIRState *state, uint64_t now)
{
    if (!state || state->phase == TRACKIR_CLOSING || state->phase == TRACKIR_CLOSED) {
        return;
    }
    if (!state->touched || state->phase == TRACKIR_FAILED) {
        state->phase = TRACKIR_CLOSED;
        return;
    }
    state->phase = TRACKIR_CLOSING;
    state->step = 0;
    state->ready_at = now;
    state->in_flight = false;
}

bool SDL_TrackIR_IsRunning(const SDL_TrackIRState *state)
{
    return state && state->phase == TRACKIR_RUNNING;
}

bool SDL_TrackIR_IsClosed(const SDL_TrackIRState *state)
{
    return state && state->phase == TRACKIR_CLOSED;
}

bool SDL_TrackIR_GetDeadline(const SDL_TrackIRState *state, uint64_t *deadline)
{
    const TrackIR_Step *step;

    if (!state || !deadline || !TrackIR_Sequencing(state) || state->in_flight) {
        return false;
    }
    step = TrackIR_CurrentStep(state);
    if (step && step->kind == TRACKIR_FLUSH) {
        *deadline = state->flush_deadline;
    } else if (step && step->kind == TRACKIR_INFO && !state->awaiting_send) {
        *deadline = state->info_seen ? state->window_start : state->last_read + SDL_TRACKIR_INFO_READ_MS;
    } else {
        *deadline = state->ready_at;
    }
    return true;
}
