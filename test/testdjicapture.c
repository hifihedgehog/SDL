/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests of hifihedgehog/SDL#33 Part 6 against three hardware
   captures, read from the reference clones at run time and never copied
   into this tree:

   1. brute_log.txt of DJI_RC_Motion_Bridge, an RC Motion 3 over USB bulk
      (codec test 3).
   2. capture.bin of dji-rc-joystick by gregszero, a DJI RC (RM330) over USB
      bulk (codec test 4). GPL-3.0 data, read as facts only.
   3. capture_f5.bin of the same project, 394 06/F5 payloads (RM330 test 2).

   The paths come as arguments. A missing file skips the test with 77. */

#define _CRT_SECURE_NO_WARNINGS
#include "testserialharness.h"
#include "../src/joystick/dji/SDL_dji_remote_proto.h"

#define MAX_READS  64
#define MAX_FRAMES 64

typedef struct Capture
{
    uint8_t *data;
    size_t length;
} Capture;

static bool LoadFile(const char *path, Capture *capture)
{
    FILE *file = fopen(path, "rb");
    long size;

    capture->data = NULL;
    capture->length = 0;
    if (!file) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    capture->data = (uint8_t *)malloc((size_t)size + 1);
    capture->length = fread(capture->data, 1, (size_t)size, file);
    fclose(file);
    return capture->length == (size_t)size;
}

typedef struct Frames
{
    int count;
    uint8_t set[MAX_FRAMES];
    uint8_t id[MAX_FRAMES];
    uint8_t sender[MAX_FRAMES];
    uint8_t receiver[MAX_FRAMES];
    size_t length[MAX_FRAMES];
    char model[MAX_FRAMES][SDL_DJI_MODEL_LENGTH];
} Frames;

static void Collect(void *userdata, const SDL_DJIFrame *frame)
{
    Frames *f = (Frames *)userdata;

    if (f->count >= MAX_FRAMES) {
        H_Fail("too many frames");
        return;
    }
    f->set[f->count] = frame->set;
    f->id[f->count] = frame->id;
    f->sender[f->count] = frame->sender;
    f->receiver[f->count] = frame->receiver;
    f->length[f->count] = frame->length;
    if (!SDL_DJI_DecodeModel(frame, f->model[f->count], SDL_DJI_MODEL_LENGTH)) {
        f->model[f->count][0] = '\0';
    }
    ++f->count;
}

/* Feeds from an exact-size heap copy */
static void Feed(SDL_DJIParser *parser, const uint8_t *data, size_t length, Frames *frames)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);

    memcpy(copy, data, length);
    SDL_DJIParser_Feed(parser, copy, length, Collect, frames);
    free(copy);
}

static int HexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* The [IN] reads of brute_log.txt from the first line up to the line
 * before "--- DATA=0x01" */
static int ParseLog(const Capture *log, uint8_t *stream, size_t *stream_length, size_t *read_lengths)
{
    const char *p = (const char *)log->data;
    const char *end = p + log->length;
    static const char prefix[] = "[DATA=0x00] [IN] len=";
    int reads = 0;

    *stream_length = 0;
    while (p < end) {
        const char *line_end = p;

        while (line_end < end && *line_end != '\n') {
            ++line_end;
        }
        if ((size_t)(line_end - p) >= 13 && memcmp(p, "--- DATA=0x01", 13) == 0) {
            break;
        }
        if ((size_t)(line_end - p) > sizeof(prefix) - 1 && memcmp(p, prefix, sizeof(prefix) - 1) == 0) {
            const char *q = p + sizeof(prefix) - 1;
            size_t declared = 0, count = 0;

            while (q < line_end && *q >= '0' && *q <= '9') {
                declared = declared * 10 + (size_t)(*q - '0');
                ++q;
            }
            while (q + 2 <= line_end && reads < MAX_READS) {
                int high, low;

                while (q < line_end && *q == ' ') {
                    ++q;
                }
                if (q + 2 > line_end || (high = HexValue(q[0])) < 0 || (low = HexValue(q[1])) < 0) {
                    break;
                }
                stream[(*stream_length)++] = (uint8_t)((high << 4) | low);
                ++count;
                q += 2;
            }
            CHECK(count == declared);
            if (reads < MAX_READS) {
                read_lengths[reads++] = count;
            }
        }
        p = line_end + 1;
    }
    return reads;
}

/* Codec test 3 */
static void TestMotionLog(const Capture *log)
{
    uint8_t *stream = (uint8_t *)malloc(log->length);
    size_t read_lengths[MAX_READS];
    size_t stream_length, position, i;
    int reads, beacons81 = 0, beacons82 = 0, statuses = 0, split = 0, j;
    SDL_DJIParser parser;
    Frames *frames = (Frames *)calloc(1, sizeof(Frames));

    reads = ParseLog(log, stream, &stream_length, read_lengths);
    CHECK(reads == 41 && stream_length == 2460);
    for (j = 0; j < reads; ++j) {
        CHECK(read_lengths[j] <= 64);
    }

    /* Read by read */
    SDL_DJIParser_Init(&parser, false);
    position = 0;
    for (j = 0; j < reads; ++j) {
        Feed(&parser, stream + position, read_lengths[j], frames);
        position += read_lengths[j];
    }
    CHECK(frames->count == 44 && parser.dropped == 0 && parser.length == 0);
    for (j = 0; j < frames->count; ++j) {
        if (frames->set[j] == 0x00 && frames->id[j] == 0x81 && frames->length[j] == 77 && strcmp(frames->model[j], "rc221") == 0) {
            ++beacons81;
        } else if (frames->set[j] == 0x00 && frames->id[j] == 0x82 && frames->length[j] == 77 && strcmp(frames->model[j], "rc221") == 0) {
            ++beacons82;
        } else if (frames->set[j] == 0x06 && frames->id[j] == 0x1E && frames->length[j] == 19) {
            ++statuses;
        }
    }
    CHECK(beacons81 == 14 && beacons82 == 14 && statuses == 16);

    /* The frames lie back to back from the first byte. 32 of them cross a
       read boundary. */
    position = 0;
    for (j = 0; j < frames->count; ++j) {
        size_t boundary = 0;
        int r;
        bool crosses = false;

        CHECK(position < stream_length && stream[position] == 0x55);
        for (r = 0; r < reads; ++r) {
            boundary += read_lengths[r];
            if (boundary > position && boundary < position + frames->length[j]) {
                crosses = true;
            }
        }
        if (crosses) {
            ++split;
        }
        position += frames->length[j];
    }
    CHECK(position == stream_length && split == 32);

    /* One byte at a time, and in uneven slices */
    for (i = 1; i <= 97; i += 32) {
        memset(frames, 0, sizeof(*frames));
        SDL_DJIParser_Init(&parser, false);
        for (position = 0; position < stream_length; position += i) {
            Feed(&parser, stream + position, (stream_length - position < i) ? stream_length - position : i, frames);
        }
        CHECK(frames->count == 44 && parser.dropped == 0);
    }
    free(frames);
    free(stream);
}

/* Codec test 4 */
static void TestRM330Capture(const Capture *capture)
{
    Frames *frames = (Frames *)calloc(1, sizeof(Frames));
    SDL_DJIParser parser;
    int j, ids81 = 0, ids82 = 0;
    size_t i;

    CHECK(capture->length == 1386);
    SDL_DJIParser_Init(&parser, false);
    Feed(&parser, capture->data, capture->length, frames);
    CHECK(frames->count == 18);
    for (j = 0; j < frames->count; ++j) {
        CHECK(frames->length[j] == 77 && frames->sender[j] == 0x0D && frames->receiver[j] == 0x2A);
        CHECK(strcmp(frames->model[j], "rm330") == 0);
        if (frames->set[j] == 0x00 && frames->id[j] == 0x81) {
            ++ids81;
        } else if (frames->set[j] == 0x00 && frames->id[j] == 0x82) {
            ++ids82;
        }
    }
    CHECK(ids81 == 9 && ids82 == 9);
    CHECK(strcmp(SDL_DJI_NameForModel(frames->model[0], "x"), "DJI RC (RM330)") == 0);

    memset(frames, 0, sizeof(*frames));
    SDL_DJIParser_Init(&parser, false);
    for (i = 0; i < capture->length; ++i) {
        Feed(&parser, capture->data + i, 1, frames);
    }
    CHECK(frames->count == 18);
    free(frames);
}

static int16_t Expected(int raw, bool vertical)
{
    const int offset = raw - 2048;
    int64_t value;

    if (offset * 50 < 1565 && -offset * 50 < 1565) {
        return 0;
    }
    value = (int64_t)offset * 32767 / 1565;
    if (value > 32767) {
        value = 32767;
    } else if (value < -32767) {
        value = -32767;
    }
    return (int16_t)(vertical ? -value : value);
}

/* RM330 test 2: every captured payload through the bulk module */
static void TestTestStickCapture(const Capture *capture)
{
    Harness *h = H_Create(&SDL_DJIRemoteBulkModule);
    size_t n;
    int zeros = 0, outside = 0;

    CHECK(capture->length == 394 * 13);
    H_Start(h);
    H_Advance(h, 100);
    for (n = 0; n < capture->length / 13; ++n) {
        const uint8_t *p = capture->data + 13 * n;
        uint8_t frame[64];
        const size_t length = SDL_DJI_BuildFrame(frame, sizeof(frame), 0x06, 0x0A, (uint16_t)n, 0x80, 0x06, 0xF5, p, 13);
        const int rh = p[1] | (p[2] << 8), rv = p[3] | (p[4] << 8), lv = p[5] | (p[6] << 8), lh = p[7] | (p[8] << 8);

        if (p[0] == 0) {
            ++zeros;
        }
        if (rh < 479 || rh > 3613 || rv < 479 || rv > 3613 || lv < 479 || lv > 3613 || lh < 479 || lh > 3613) {
            ++outside;
        }
        H_Advance(h, 100 + n * 12);
        H_Feed(h, frame, length);
        CHECK(H_Snapshot(h, 0)->present);
        CHECK(H_Axis(h, 0, SDL_DJI_AXIS_RIGHT_X) == Expected(rh, false));
        CHECK(H_Axis(h, 0, SDL_DJI_AXIS_RIGHT_Y) == Expected(rv, true));
        CHECK(H_Axis(h, 0, SDL_DJI_AXIS_LEFT_Y) == Expected(lv, true));
        CHECK(H_Axis(h, 0, SDL_DJI_AXIS_LEFT_X) == Expected(lh, false));
    }
    CHECK(zeros == 394 && outside == 0);
    CHECK(h->presence[0] == 1);
    H_Destroy(h);
}

int main(int argc, char *argv[])
{
    Capture log, rm330, f5;

    if (argc < 4 || !LoadFile(argv[1], &log) || !LoadFile(argv[2], &rm330) || !LoadFile(argv[3], &f5)) {
        printf("SKIPPED: the reference captures are not at the paths given\n");
        return 77;
    }
    TestMotionLog(&log);
    TestRM330Capture(&rm330);
    TestTestStickCapture(&f5);
    free(log.data);
    free(rm330.data);
    free(f5.data);
    return H_Finish();
}
