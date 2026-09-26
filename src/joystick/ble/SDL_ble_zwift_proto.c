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

/* Facts from zwiftplay, SwiftControl, qdomyos-zwift and Makinolo's posts,
 * read for facts only:
 * - Start-up: with ASYNC notifying and SYNC_TX indicating, write "RideOn"
 *   to SYNC_RX without response (SwiftControl zwift_device.dart:138-146).
 *   SYNC_TX answers with RideOn, and plain messages arrive on ASYNC.
 * - Older firmware sends no plain message. When no ASYNC value decodes as
 *   one in 10 s, the host sends RideOn, 01 02 and its 64-byte P-256 public
 *   key, SYNC_TX answers with RideOn, two bytes and the device's key, and
 *   every later value is 4 counter bytes, AES-CCM ciphertext and a 4-byte
 *   tag. A value of 8 bytes or less is not encrypted data (zwiftplay
 *   AbstractZapDevice.kt:42-48).
 * - A message is a type byte and a protocol buffers body: 07 a Play half's
 *   nine varint fields, 37 a Click's two, 23 a button map with lever entries,
 *   15 idle, 19 the battery in field 2. A button reads 0 when pressed.
 *   Levers are zigzag sint32 from -100 to 100. The pull of field 9 is an
 *   amount from 0 to 200. */

#include "SDL_ble_zwift_proto.h"

#include <string.h>

#define ZWIFT_MAX_GROUP_DEPTH 16 /* Groups open at once. Deeper nesting is malformed. */

static const uint8_t zwift_ride_on[6] = { 0x52, 0x69, 0x64, 0x65, 0x4F, 0x6E };

int32_t SDL_Zwift_ZigZag(uint64_t value)
{
    const uint32_t v = (uint32_t)value;

    return (int32_t)((v >> 1) ^ (0u - (v & 1u)));
}

/* A protocol buffers field */
typedef struct ZwiftField
{
    uint32_t number;
    uint8_t wire;
    uint64_t value;      /* Wire type 0 */
    const uint8_t *data; /* Wire type 2 */
    size_t length;
} ZwiftField;

static bool Zwift_ReadVarint(const uint8_t *data, size_t length, size_t *index, uint64_t *value)
{
    uint64_t result = 0;
    int shift;

    for (shift = 0; shift < 64 && *index < length; shift += 7) {
        const uint8_t byte = data[(*index)++];

        result |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) {
            *value = result;
            return true;
        }
    }
    return false;
}

/* A tag: a field number from 1 to 2^29 - 1 and a wire type */
static bool Zwift_ReadTag(const uint8_t *data, size_t length, size_t *index, ZwiftField *field)
{
    uint64_t tag;

    if (!Zwift_ReadVarint(data, length, index, &tag) || (tag >> 3) == 0 || (tag >> 3) > 0x1FFFFFFF) {
        return false;
    }
    memset(field, 0, sizeof(*field));
    field->number = (uint32_t)(tag >> 3);
    field->wire = (uint8_t)(tag & 7);
    return true;
}

/* The payload of wire type 0 (varint), 1 (8 bytes), 2 (length-delimited)
   or 5 (4 bytes). Every other wire type fails here. */
static bool Zwift_ReadPayload(const uint8_t *data, size_t length, size_t *index, ZwiftField *field)
{
    uint64_t size;

    switch (field->wire) {
    case 0:
        return Zwift_ReadVarint(data, length, index, &field->value);
    case 1:
        if (length - *index < 8) {
            return false;
        }
        *index += 8;
        return true;
    case 2:
        if (!Zwift_ReadVarint(data, length, index, &size) || size > length - *index) {
            return false;
        }
        field->data = &data[*index];
        field->length = (size_t)size;
        *index += (size_t)size;
        return true;
    case 5:
        if (length - *index < 4) {
            return false;
        }
        *index += 4;
        return true;
    default:
        return false;
    }
}

/* Moves past the fields of a group that wire type 3 opened, through the
   wire type 4 tag that closes it with the same field number. Groups nest,
   as zwiftplay's generic decoder reads them (utils/ProtoDecode.kt:38-42,
   :72). */
static bool Zwift_SkipGroup(const uint8_t *data, size_t length, size_t *index, uint32_t number)
{
    uint32_t open[ZWIFT_MAX_GROUP_DEPTH];
    int depth = 0;
    ZwiftField inner;

    open[depth++] = number;
    while (depth > 0) {
        if (!Zwift_ReadTag(data, length, index, &inner)) {
            return false;
        }
        if (inner.wire == 3) {
            if (depth == ZWIFT_MAX_GROUP_DEPTH) {
                return false;
            }
            open[depth++] = inner.number;
        } else if (inner.wire == 4) {
            if (inner.number != open[depth - 1]) {
                return false;
            }
            --depth;
        } else if (!Zwift_ReadPayload(data, length, index, &inner)) {
            return false;
        }
    }
    return true;
}

/* 1 for a field, 0 at the end, -1 when malformed. A group comes back as one
   field of wire type 3 with its contents skipped. Wire type 4 outside a
   group, and wire types 6 and 7, are malformed. */
static int Zwift_NextField(const uint8_t *data, size_t length, size_t *index, ZwiftField *field)
{
    if (*index >= length) {
        return 0;
    }
    if (!Zwift_ReadTag(data, length, index, field)) {
        return -1;
    }
    if (field->wire == 3) {
        return Zwift_SkipGroup(data, length, index, field->number) ? 1 : -1;
    }
    return Zwift_ReadPayload(data, length, index, field) ? 1 : -1;
}

/* A lever of -100 to 100, truncated toward zero */
static int16_t Zwift_Axis(int32_t value)
{
    return (int16_t)(SDL_BLE_Clamp(value, -100, 100) * 32767 / 100);
}

/* A pull of 0 to 100 on the joystick axis behind SDL's trigger binding,
   which maps -32768 to 0 and 32767 to 32767. The axis value is the ceiling
   of want * 65535 / 32767, minus 32768, which SDL's float binding
   (SDL_gamepad.c:3779-3780) maps back to exactly want, v * 32767 / 100, for
   every v from 0 to 100. */
static int16_t Zwift_Trigger(int32_t value)
{
    const int64_t want = (int64_t)SDL_BLE_Clamp(value, 0, 100) * 32767 / 100;

    return (int16_t)((want * 65535 + 32766) / 32767 - 32768);
}

/* Field 9, the pull, as 0 to 100. Makinolo, who holds the captures, reads it
   as an amount from 0 to 200 whose low bit means nothing (2023, "Key press
   messages"), and zwiftplay and qdomyos-zwift keep the raw varint
   (ControllerNotification.kt:58, controllerNotification.h:69-71). Only
   SwiftControl declares it sint32 (zwift.pb.dart:76), which reads an odd
   value as negative. The readings agree on every even value. */
static int32_t Zwift_Pull(uint64_t value)
{
    const uint64_t pull = value >> 1;

    return (pull > 100) ? 100 : (int32_t)pull;
}

/* 07: fields 1 to 9, all varints, all present */
static bool Zwift_Play(SDL_ZwiftState *zwift, const uint8_t *body, size_t length, SDL_BLEControls *controls)
{
    uint64_t values[10];
    uint32_t seen = 0;
    size_t index = 0;
    ZwiftField field;
    int result;
    bool left;

    while ((result = Zwift_NextField(body, length, &index, &field)) > 0) {
        if (field.number >= 1 && field.number <= 9) {
            if (field.wire != 0) {
                return false;
            }
            values[field.number] = field.value;
            seen |= 1u << field.number;
        }
    }
    if (result < 0 || seen != 0x3FE) {
        return false;
    }
    /* Field 1 names the half: 1 left, 0 right. It must agree with the
       advertisement's type byte. */
    left = (zwift->variant == SDL_ZWIFT_PLAY_LEFT);
    if ((values[1] == 1) != left || values[1] > 1) {
        return false;
    }
    if (left) {
        SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_DPAD_UP, values[2] == 0);
        SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_DPAD_LEFT, values[3] == 0);
        SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_DPAD_RIGHT, values[4] == 0);
        SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_DPAD_DOWN, values[5] == 0);
        SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_LEFT_SHOULDER, values[6] == 0);
        SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_BACK, values[7] == 0);
        controls->axes[SDL_BLE_AXIS_LEFTX] = Zwift_Axis(SDL_Zwift_ZigZag(values[8]));
        controls->axes[SDL_BLE_AXIS_LEFT_TRIGGER] = Zwift_Trigger(Zwift_Pull(values[9]));
    } else {
        SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_NORTH, values[2] == 0);
        SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_WEST, values[3] == 0);
        SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_EAST, values[4] == 0);
        SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_SOUTH, values[5] == 0);
        SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_RIGHT_SHOULDER, values[6] == 0);
        SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_START, values[7] == 0);
        controls->axes[SDL_BLE_AXIS_RIGHTX] = Zwift_Axis(SDL_Zwift_ZigZag(values[8]));
        controls->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] = Zwift_Trigger(Zwift_Pull(values[9]));
    }
    return true;
}

/* 37: field 1 plus and field 2 minus, both present */
static bool Zwift_Click(const uint8_t *body, size_t length, SDL_BLEControls *controls)
{
    uint64_t plus = 1, minus = 1;
    uint32_t seen = 0;
    size_t index = 0;
    ZwiftField field;
    int result;

    while ((result = Zwift_NextField(body, length, &index, &field)) > 0) {
        if (field.number == 1 || field.number == 2) {
            if (field.wire != 0) {
                return false;
            }
            if (field.number == 1) {
                plus = field.value;
            } else {
                minus = field.value;
            }
            seen |= 1u << field.number;
        }
    }
    if (result < 0 || seen != 0x6) {
        return false;
    }
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_RIGHT_SHOULDER, plus == 0);
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_LEFT_SHOULDER, minus == 0);
    return true;
}

/* A lever entry: field 1 location, field 2 zigzag value. False when
   malformed. An entry that lacks either field moves no lever. */
static bool Zwift_Lever(const uint8_t *data, size_t length, int32_t *levers)
{
    uint64_t location = 0;
    int32_t value = 0;
    bool has_location = false, has_value = false;
    size_t index = 0;
    ZwiftField field;
    int result;

    while ((result = Zwift_NextField(data, length, &index, &field)) > 0) {
        if (field.number == 1 || field.number == 2) {
            if (field.wire != 0) {
                return false;
            }
            if (field.number == 1) {
                location = field.value;
                has_location = true;
            } else {
                value = SDL_Zwift_ZigZag(field.value);
                has_value = true;
            }
        }
    }
    if (result < 0) {
        return false;
    }
    if (has_location && has_value && location < 4) {
        levers[location] = value;
    }
    return true;
}

/* 23: field 1 the button map, where a clear bit is pressed, and lever
   entries as repeated field 3 (SwiftControl zwift.pb.dart:503-507), inside a
   field 2 wrapper whose field 1 repeats (Makinolo's capture), or as field 2
   itself, which qdomyos-zwift reads as one entry (abstractZapDevice.h:446-495).
   A wrapper's first field is a message and an entry's a varint, which tells
   the two apart. A message with the map sets every control: the buttons
   from the map, each lever from its entry, and a lever without an entry at
   rest. Both decoders read the map without lever entries
   (zwift_ride.dart:194-212, qdomyos-zwift abstractZapDevice.h:382-406) and
   release a lever whose entry is missing: qdomyos-zwift starts every
   message from released paddles (abstractZapDevice.h:342, :439, :508-517),
   and SwiftControl adds a paddle only for an entry that is there and
   releases everything on an empty list (zwift_ride.dart:214-239,
   base_device.dart:113-115). SwiftControl calls the map-only frame
   23 08 FF FF FF FF 0F exactly what a key-up emits
   (controller_keep_alive.dart:3-6), and its emulator sends map-only frames
   for a press and a release (ftms_mdns_emulator.dart:150-168). Without the
   map nothing applies, as qdomyos-zwift drops such a message
   (abstractZapDevice.h:382-384). */
static bool Zwift_Ride(const uint8_t *body, size_t length, SDL_BLEControls *controls)
{
    int32_t levers[4] = { 0, 0, 0, 0 }; /* A location without an entry rests at 0 */
    uint32_t map = 0;
    bool has_map = false;
    size_t index = 0, inner;
    ZwiftField field, entry;
    int result, inner_result;

    while ((result = Zwift_NextField(body, length, &index, &field)) > 0) {
        switch (field.number) {
        case 1:
            if (field.wire != 0) {
                return false;
            }
            map = (uint32_t)field.value;
            has_map = true;
            break;
        case 2:
            if (field.wire != 2) {
                return false;
            }
            inner = 0;
            inner_result = Zwift_NextField(field.data, field.length, &inner, &entry);
            if (inner_result < 0) {
                return false;
            }
            if (inner_result > 0 && entry.wire == 0) {
                if (!Zwift_Lever(field.data, field.length, levers)) {
                    return false;
                }
                break;
            }
            inner = 0;
            while ((inner_result = Zwift_NextField(field.data, field.length, &inner, &entry)) > 0) {
                if (entry.number == 1) {
                    if (entry.wire != 2 || !Zwift_Lever(entry.data, entry.length, levers)) {
                        return false;
                    }
                }
            }
            if (inner_result < 0) {
                return false;
            }
            break;
        case 3:
            if (field.wire != 2 || !Zwift_Lever(field.data, field.length, levers)) {
                return false;
            }
            break;
        default:
            break;
        }
    }
    if (result < 0 || !has_map) {
        return false;
    }
    /* SwiftControl's decoder masks (zwift_ride.dart:271-290) */
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_DPAD_LEFT, (map & 0x00001) == 0);
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_DPAD_UP, (map & 0x00002) == 0);
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_DPAD_RIGHT, (map & 0x00004) == 0);
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_DPAD_DOWN, (map & 0x00008) == 0);
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_EAST, (map & 0x00010) == 0);
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_SOUTH, (map & 0x00020) == 0);
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_NORTH, (map & 0x00040) == 0);
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_WEST, (map & 0x00080) == 0);
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_LEFT_SHOULDER, (map & 0x00100) == 0);
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_BACK, (map & 0x00800) == 0);
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_RIGHT_SHOULDER, (map & 0x01000) == 0);
    SDL_BLE_SetButton(controls, SDL_BLE_BUTTON_START, (map & 0x08000) == 0);
    controls->axes[SDL_BLE_AXIS_LEFTX] = Zwift_Axis(levers[0]);
    controls->axes[SDL_BLE_AXIS_RIGHTX] = Zwift_Axis(levers[1]);
    return true;
}

/* 19: field 2, the percentage */
static bool Zwift_Battery(const uint8_t *body, size_t length, SDL_BLEControls *controls)
{
    size_t index = 0;
    ZwiftField field;
    int result;
    bool found = false;

    while ((result = Zwift_NextField(body, length, &index, &field)) > 0) {
        if (field.number == 2) {
            if (field.wire != 0) {
                return false;
            }
            controls->battery = (int8_t)(field.value > 100 ? 100 : field.value);
            found = true;
        }
    }
    return result == 0 && found;
}

/* One plain message: a type byte and its body. True when it decoded. */
static bool Zwift_Message(SDL_ZwiftState *zwift, const uint8_t *message, size_t length, uint64_t time_ns)
{
    SDL_BLEControls controls = zwift->base.controls;
    bool decoded;

    if (length < 1) {
        return false;
    }
    switch (message[0]) {
    case SDL_ZWIFT_MESSAGE_PLAY:
        decoded = (zwift->variant == SDL_ZWIFT_PLAY_LEFT || zwift->variant == SDL_ZWIFT_PLAY_RIGHT) &&
                  Zwift_Play(zwift, &message[1], length - 1, &controls);
        break;
    case SDL_ZWIFT_MESSAGE_CLICK:
        decoded = zwift->variant == SDL_ZWIFT_CLICK && Zwift_Click(&message[1], length - 1, &controls);
        break;
    case SDL_ZWIFT_MESSAGE_RIDE:
        decoded = zwift->variant == SDL_ZWIFT_PLAY_FW2 && Zwift_Ride(&message[1], length - 1, &controls);
        break;
    case SDL_ZWIFT_MESSAGE_BATTERY:
        decoded = Zwift_Battery(&message[1], length - 1, &controls);
        break;
    case SDL_ZWIFT_MESSAGE_IDLE:
        /* "just a one byte message" (Makinolo 2023, "Protocol Messages"), so
           an encrypted frame whose first counter byte is 15 is not one */
        decoded = (length == 1);
        break;
    default:
        decoded = false;
        break;
    }
    if (!decoded) {
        return false;
    }
    SDL_BLE_Commit(&zwift->base, &controls, time_ns);
    if (zwift->started) {
        zwift->base.ready = true;
    }
    return true;
}

static void Zwift_Reset(void *state, const SDL_BLESink *sink, const SDL_BLEModuleContext *context)
{
    SDL_ZwiftState *zwift = (SDL_ZwiftState *)state;
    SDL_BLEIdentity identity;
    const uint32_t dpad = (1u << SDL_BLE_BUTTON_DPAD_UP) | (1u << SDL_BLE_BUTTON_DPAD_DOWN) |
                          (1u << SDL_BLE_BUTTON_DPAD_LEFT) | (1u << SDL_BLE_BUTTON_DPAD_RIGHT);
    const uint32_t faces = (1u << SDL_BLE_BUTTON_SOUTH) | (1u << SDL_BLE_BUTTON_EAST) |
                           (1u << SDL_BLE_BUTTON_WEST) | (1u << SDL_BLE_BUTTON_NORTH);

    memset(zwift, 0, sizeof(*zwift));
    zwift->variant = context ? context->variant : 0;
    zwift->crypto = context ? (const SDL_ZwiftCrypto *)context->crypto : NULL;
    switch (zwift->variant) {
    case SDL_ZWIFT_PLAY_LEFT:
        SDL_BLE_SetIdentity(&identity, "Zwift Play (L)", SDL_BLE_TYPE_GAMEPAD,
                            dpad | (1u << SDL_BLE_BUTTON_LEFT_SHOULDER) | (1u << SDL_BLE_BUTTON_BACK),
                            (1u << SDL_BLE_AXIS_LEFTX) | (1u << SDL_BLE_AXIS_LEFT_TRIGGER), true);
        break;
    case SDL_ZWIFT_PLAY_RIGHT:
        SDL_BLE_SetIdentity(&identity, "Zwift Play (R)", SDL_BLE_TYPE_GAMEPAD,
                            faces | (1u << SDL_BLE_BUTTON_RIGHT_SHOULDER) | (1u << SDL_BLE_BUTTON_START),
                            (1u << SDL_BLE_AXIS_RIGHTX) | (1u << SDL_BLE_AXIS_RIGHT_TRIGGER), true);
        break;
    case SDL_ZWIFT_CLICK:
        SDL_BLE_SetIdentity(&identity, "Zwift Click", SDL_BLE_TYPE_GAMEPAD,
                            (1u << SDL_BLE_BUTTON_LEFT_SHOULDER) | (1u << SDL_BLE_BUTTON_RIGHT_SHOULDER), 0, true);
        break;
    default:
        SDL_BLE_SetIdentity(&identity, "Zwift Play", SDL_BLE_TYPE_GAMEPAD,
                            dpad | faces | (1u << SDL_BLE_BUTTON_LEFT_SHOULDER) | (1u << SDL_BLE_BUTTON_RIGHT_SHOULDER) |
                                (1u << SDL_BLE_BUTTON_BACK) | (1u << SDL_BLE_BUTTON_START),
                            (1u << SDL_BLE_AXIS_LEFTX) | (1u << SDL_BLE_AXIS_RIGHTX), true);
        break;
    }
    SDL_BLE_ResetBase(&zwift->base, sink, &identity);
}

static void Zwift_Start(void *state, uint64_t now)
{
    SDL_ZwiftState *zwift = (SDL_ZwiftState *)state;

    if (zwift->variant != SDL_ZWIFT_PLAY_LEFT && zwift->variant != SDL_ZWIFT_PLAY_RIGHT &&
        zwift->variant != SDL_ZWIFT_CLICK && zwift->variant != SDL_ZWIFT_PLAY_FW2) {
        zwift->base.failed = true;
        return;
    }
    SDL_BLE_QueueWrite(&zwift->base, SDL_ZWIFT_SYNC_RX, zwift_ride_on, sizeof(zwift_ride_on), false);
    zwift->phase = SDL_ZWIFT_PLAIN;
    zwift->started = true;
    /* Armed until a message decodes after the RideOn write */
    zwift->timing = true;
    zwift->armed = true;
    zwift->deadline = now + SDL_ZWIFT_HANDSHAKE_MS;
}

/* The handshake's clock runs from the moment its write went out. A
   handshake write that fails while the switch is armed gives the
   connection up and leaves no timer, so the session ends with its backoff
   at once instead of waiting out the 10 s. Once a message has decoded the
   controller answers, and a failed write changes nothing. */
static void Zwift_WriteDone(void *state, bool success, uint64_t now)
{
    SDL_ZwiftState *zwift = (SDL_ZwiftState *)state;

    if (zwift->timing) {
        zwift->timing = false;
        if (zwift->armed) {
            if (!success) {
                zwift->base.failed = true;
                zwift->armed = false;
            } else {
                zwift->deadline = now + SDL_ZWIFT_HANDSHAKE_MS;
            }
        }
    }
}

static void Zwift_SyncTx(SDL_ZwiftState *zwift, const uint8_t *data, size_t length)
{
    if (length < sizeof(zwift_ride_on) || memcmp(data, zwift_ride_on, sizeof(zwift_ride_on)) != 0) {
        return;
    }
    /* RideOn, two bytes and the device's key answer the key exchange. RideOn
       alone, or with 01 04 from a Play or 01 03 from a Click, answers the
       plain handshake and needs nothing more. */
    if (zwift->phase == SDL_ZWIFT_KEY_SENT && length >= sizeof(zwift_ride_on) + 2 + SDL_ZWIFT_KEY_SIZE) {
        if (zwift->crypto && zwift->crypto->Derive(zwift->crypto->userdata, &data[sizeof(zwift_ride_on) + 2])) {
            zwift->phase = SDL_ZWIFT_ENCRYPTED;
            zwift->armed = false;
        } else {
            zwift->base.failed = true;
        }
    }
}

static void Zwift_Async(SDL_ZwiftState *zwift, const uint8_t *data, size_t length, uint64_t time_ns)
{
    uint8_t plain[256];

    if (zwift->phase != SDL_ZWIFT_ENCRYPTED) {
        /* Only a value that decodes as a message answers the handshake, in
           either phase. A controller of older firmware can hold a session
           from an earlier connection (Makinolo 2023, "Race Controller
           service") and send encrypted frames, and zwiftplay takes any value
           longer than 8 bytes for one (AbstractZapDevice.kt:42-48). Such a
           frame decodes as nothing here, so the key exchange still comes
           10 s after the RideOn write. A message after the key went out
           means the controller runs plain and was only slow, so the key
           exchange stops and its deadline with it. */
        if (Zwift_Message(zwift, data, length, time_ns) &&
            (zwift->phase == SDL_ZWIFT_PLAIN || zwift->phase == SDL_ZWIFT_KEY_SENT)) {
            zwift->phase = SDL_ZWIFT_PLAIN;
            zwift->armed = false;
        }
        return;
    }
    if (length <= SDL_ZWIFT_FRAME_EXTRA || length - SDL_ZWIFT_FRAME_EXTRA > sizeof(plain)) {
        return;
    }
    if (zwift->crypto->Decrypt(zwift->crypto->userdata, data, length, plain)) {
        Zwift_Message(zwift, plain, length - SDL_ZWIFT_FRAME_EXTRA, time_ns);
    }
}

static void Zwift_Value(void *state, int characteristic, const uint8_t *data, size_t length, uint64_t time_ns)
{
    SDL_ZwiftState *zwift = (SDL_ZwiftState *)state;

    if (length == 0) {
        return;
    }
    if (characteristic == SDL_ZWIFT_SYNC_TX) {
        Zwift_SyncTx(zwift, data, length);
    } else if (characteristic == SDL_ZWIFT_ASYNC) {
        Zwift_Async(zwift, data, length, time_ns);
    }
}

static void Zwift_Tick(void *state, uint64_t now)
{
    SDL_ZwiftState *zwift = (SDL_ZwiftState *)state;
    uint8_t key[sizeof(zwift_ride_on) + 2 + SDL_ZWIFT_KEY_SIZE];

    if (!zwift->armed || zwift->timing || now < zwift->deadline) {
        return;
    }
    /* A deadline that has passed is spent, unless the key exchange sets the next */
    zwift->armed = false;
    if (zwift->phase == SDL_ZWIFT_PLAIN) {
        /* No message after RideOn: the encrypted handshake Zwift's app performs */
        memcpy(key, zwift_ride_on, sizeof(zwift_ride_on));
        key[6] = 0x01;
        key[7] = 0x02;
        if (!zwift->crypto || !zwift->crypto->MakeKey(zwift->crypto->userdata, &key[8])) {
            zwift->base.failed = true;
            return;
        }
        /* 72 bytes exceed a default ATT payload, so the write asks for a
           response, which Windows can send as a long write */
        SDL_BLE_QueueWrite(&zwift->base, SDL_ZWIFT_SYNC_RX, key, sizeof(key), true);
        zwift->phase = SDL_ZWIFT_KEY_SENT;
        zwift->timing = true;
        zwift->armed = true;
        zwift->deadline = now + SDL_ZWIFT_HANDSHAKE_MS;
        return;
    }
    if (zwift->phase == SDL_ZWIFT_KEY_SENT) {
        /* No key came back */
        zwift->base.failed = true;
    }
}

static bool Zwift_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_ZwiftState *zwift = (SDL_ZwiftState *)state;

    if (!zwift->armed || zwift->timing) {
        return false;
    }
    *deadline = zwift->deadline;
    return true;
}

static void Zwift_Close(void *state, uint64_t now)
{
    SDL_ZwiftState *zwift = (SDL_ZwiftState *)state;

    (void)now;
    zwift->armed = false;
    zwift->started = false;
}

const SDL_BLEModule SDL_BLEZwiftModule = {
    sizeof(SDL_ZwiftState),
    Zwift_Reset,
    Zwift_Start,
    Zwift_Value,
    Zwift_WriteDone,
    Zwift_Tick,
    Zwift_GetDeadline,
    Zwift_Close
};

#define ZWIFT_UUID(n) \
    SDL_BLE_UUID_INIT(0x00, 0x00, 0x00, n, 0x19, 0xca, 0x46, 0x51, 0x86, 0xe5, 0xfa, 0x29, 0xdc, 0xdd, 0x09, 0xd1)

/* Found by company 0x094A (Zwift) with the type byte of an in-scope model
   first (zwiftplay ZapConstants.kt:5-12, SwiftControl constants.dart:21-36).
   The advertised services only corroborate, since unsupported Zwift models
   share them. The service is FC82 on newer firmware and the 128-bit one
   before, and SwiftControl takes FC82 first (zwift_device.dart:37-43). No
   source pairs. */
const SDL_BLEFamily SDL_BLEZwiftFamily = {
    "Zwift",
    3,
    {
        { SDL_BLE_KEY_MANUFACTURER, false, NULL, SDL_BLE_UUID16_INIT(0x00, 0x00), 0x094A, 4,
          { SDL_ZWIFT_PLAY_RIGHT, SDL_ZWIFT_PLAY_LEFT, SDL_ZWIFT_CLICK, SDL_ZWIFT_PLAY_FW2 } },
        { SDL_BLE_KEY_SERVICE, true, NULL, ZWIFT_UUID(0x01), 0, 0, { 0 } },
        { SDL_BLE_KEY_SERVICE, true, NULL, SDL_BLE_UUID16_INIT(0xfc, 0x82), 0, 0, { 0 } },
    },
    1,
    {
        ZWIFT_UUID(0x01),
    },
    true,
    SDL_BLE_UUID16_INIT(0xfc, 0x82),
    3,
    {
        { 0, ZWIFT_UUID(0x02), SDL_BLE_CHAR_SUBSCRIBE },
        { 0, ZWIFT_UUID(0x04), SDL_BLE_CHAR_SUBSCRIBE },
        { 0, ZWIFT_UUID(0x03), 0 },
    },
    SDL_BLE_PAIR_NOT_NEEDED,
    &SDL_BLEZwiftModule
};
