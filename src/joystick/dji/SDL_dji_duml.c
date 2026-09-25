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

#include "SDL_dji_duml.h"

#include <string.h>

typedef enum DJI_ScanResult
{
    DJI_SCAN_WAIT, /* More bytes are needed */
    DJI_SCAN_DROP, /* No frame starts here */
    DJI_SCAN_FRAME
} DJI_ScanResult;

static const uint8_t dji_envelope_magic[4] = { 0x55, 0xCC, 0x30, 0x75 };

uint8_t SDL_DJI_CRC8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0x77;
    size_t i;
    int bit;

    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (uint8_t)((crc >> 1) ^ 0x8C) : (uint8_t)(crc >> 1);
        }
    }
    return crc;
}

uint16_t SDL_DJI_CRC16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0x3692;
    size_t i;
    int bit;

    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

size_t SDL_DJI_BuildFrame(uint8_t *out, size_t size, uint8_t sender, uint8_t receiver, uint16_t sequence,
                          uint8_t type, uint8_t set, uint8_t id, const uint8_t *payload, size_t payload_length)
{
    size_t length;
    uint16_t crc;

    if (!out || payload_length > SDL_DJI_MAX_FRAME - SDL_DJI_MIN_FRAME || (payload_length && !payload)) {
        return 0;
    }
    length = SDL_DJI_MIN_FRAME + payload_length;
    if (length > size) {
        return 0;
    }
    out[0] = SDL_DJI_SOF;
    out[1] = (uint8_t)(length & 0xFF);
    out[2] = (uint8_t)((length >> 8) | (SDL_DJI_VERSION << 2));
    out[3] = SDL_DJI_CRC8(out, 3);
    out[4] = sender;
    out[5] = receiver;
    out[6] = (uint8_t)(sequence & 0xFF);
    out[7] = (uint8_t)(sequence >> 8);
    out[8] = type;
    out[9] = set;
    out[10] = id;
    if (payload_length) {
        memcpy(out + SDL_DJI_HEADER_LENGTH, payload, payload_length);
    }
    crc = SDL_DJI_CRC16(out, length - 2);
    out[length - 2] = (uint8_t)(crc & 0xFF);
    out[length - 1] = (uint8_t)(crc >> 8);
    return length;
}

size_t SDL_DJI_BuildResponse(uint8_t *out, size_t size, const SDL_DJIFrame *request)
{
    if (!request) {
        return 0;
    }
    return SDL_DJI_BuildFrame(out, size, request->receiver, request->sender, request->sequence,
                              SDL_DJI_TYPE_RESPONSE, request->set, request->id, NULL, 0);
}

size_t SDL_DJI_BuildEnvelope(uint8_t *out, size_t size, const uint8_t *frame, size_t frame_length)
{
    if (!out || !frame || frame_length < SDL_DJI_MIN_FRAME || frame_length > SDL_DJI_MAX_FRAME ||
        size < SDL_DJI_ENVELOPE_LENGTH + frame_length) {
        return 0;
    }
    memcpy(out, dji_envelope_magic, sizeof(dji_envelope_magic));
    out[4] = (uint8_t)(frame_length & 0xFF);
    out[5] = (uint8_t)(frame_length >> 8);
    out[6] = 0;
    out[7] = 0;
    memcpy(out + SDL_DJI_ENVELOPE_LENGTH, frame, frame_length);
    return SDL_DJI_ENVELOPE_LENGTH + frame_length;
}

/* A frame at p, with available bytes on hand */
static DJI_ScanResult DJI_ScanFrame(const uint8_t *p, size_t available, size_t *frame_length)
{
    size_t length;

    if (p[0] != SDL_DJI_SOF) {
        return DJI_SCAN_DROP;
    }
    if (available < 4) {
        return DJI_SCAN_WAIT;
    }
    if (SDL_DJI_CRC8(p, 3) != p[3] || (p[2] >> 2) != SDL_DJI_VERSION) {
        return DJI_SCAN_DROP;
    }
    length = (size_t)p[1] | ((size_t)(p[2] & 0x03) << 8);
    if (length < SDL_DJI_MIN_FRAME) {
        return DJI_SCAN_DROP;
    }
    if (available < length) {
        return DJI_SCAN_WAIT;
    }
    if (SDL_DJI_CRC16(p, length - 2) != (uint16_t)(p[length - 2] | (p[length - 1] << 8))) {
        return DJI_SCAN_DROP;
    }
    *frame_length = length;
    return DJI_SCAN_FRAME;
}

/* An envelope at p. Its frame must fill it exactly. */
static DJI_ScanResult DJI_ScanEnvelope(const uint8_t *p, size_t available, size_t *envelope_length)
{
    uint32_t length;
    size_t frame_length = 0;
    size_t i;

    for (i = 0; i < sizeof(dji_envelope_magic) && i < available; ++i) {
        if (p[i] != dji_envelope_magic[i]) {
            return DJI_SCAN_DROP;
        }
    }
    if (available < SDL_DJI_ENVELOPE_LENGTH) {
        return DJI_SCAN_WAIT;
    }
    length = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    if (length < SDL_DJI_MIN_FRAME || length > SDL_DJI_MAX_FRAME) {
        return DJI_SCAN_DROP;
    }
    if (available - SDL_DJI_ENVELOPE_LENGTH < length) {
        return DJI_SCAN_WAIT;
    }
    if (DJI_ScanFrame(p + SDL_DJI_ENVELOPE_LENGTH, length, &frame_length) != DJI_SCAN_FRAME || frame_length != length) {
        return DJI_SCAN_DROP;
    }
    *envelope_length = SDL_DJI_ENVELOPE_LENGTH + length;
    return DJI_SCAN_FRAME;
}

bool SDL_DJI_CheckFrame(const uint8_t *data, size_t length, SDL_DJIFrame *frame)
{
    size_t frame_length = 0;

    if (!data || length < SDL_DJI_MIN_FRAME || length > SDL_DJI_MAX_FRAME) {
        return false;
    }
    if (DJI_ScanFrame(data, length, &frame_length) != DJI_SCAN_FRAME || frame_length != length) {
        return false;
    }
    if (frame) {
        frame->data = data;
        frame->length = length;
        frame->sender = data[4];
        frame->receiver = data[5];
        frame->sequence = (uint16_t)(data[6] | (data[7] << 8));
        frame->type = data[8];
        frame->set = data[9];
        frame->id = data[10];
        frame->payload = data + SDL_DJI_HEADER_LENGTH;
        frame->payload_length = length - SDL_DJI_MIN_FRAME;
    }
    return true;
}

void SDL_DJIParser_Init(SDL_DJIParser *parser, bool envelope)
{
    parser->envelope = envelope;
    parser->length = 0;
    parser->dropped = 0;
}

static void DJI_Scan(SDL_DJIParser *parser, SDL_DJIFrameHandler handler, void *userdata)
{
    size_t position = 0;

    while (position < parser->length) {
        const uint8_t *p = parser->buffer + position;
        const size_t available = parser->length - position;
        const size_t skip = parser->envelope ? SDL_DJI_ENVELOPE_LENGTH : 0;
        size_t total = 0;
        SDL_DJIFrame frame;
        DJI_ScanResult result;

        result = parser->envelope ? DJI_ScanEnvelope(p, available, &total) : DJI_ScanFrame(p, available, &total);
        if (result == DJI_SCAN_WAIT) {
            break;
        }
        if (result == DJI_SCAN_DROP) {
            ++position;
            ++parser->dropped;
            continue;
        }
        if (handler && SDL_DJI_CheckFrame(p + skip, total - skip, &frame)) {
            handler(userdata, &frame);
        }
        position += total;
    }
    if (position > 0) {
        memmove(parser->buffer, parser->buffer + position, parser->length - position);
        parser->length -= position;
    }
}

void SDL_DJIParser_Feed(SDL_DJIParser *parser, const uint8_t *data, size_t length,
                        SDL_DJIFrameHandler handler, void *userdata)
{
    /* A waiting frame is always shorter than the capacity, so each pass
       takes at least one byte */
    const size_t capacity = parser->envelope ? SDL_DJI_ENVELOPE_LENGTH + SDL_DJI_MAX_FRAME : SDL_DJI_MAX_FRAME;

    while (data && length > 0) {
        size_t take = capacity - parser->length;

        if (take > length) {
            take = length;
        }
        memcpy(parser->buffer + parser->length, data, take);
        parser->length += take;
        data += take;
        length -= take;
        DJI_Scan(parser, handler, userdata);
    }
}
