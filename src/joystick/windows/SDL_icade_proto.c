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

/* The iCade letter protocol. See SDL_icade_proto.h. */

#include "SDL_icade_proto.h"

#include <string.h>

/* The Scan 1 make codes of the press and release letters, in state bit
   order, from Microsoft's scan code table ("Keyboard Input Overview") */
static const uint16_t icade_press[SDL_ICADE_CONTROLS] = {
    0x11, /* W, stick up */
    0x20, /* D, stick right */
    0x2D, /* X, stick down */
    0x1E, /* A, stick left */
    0x15, /* Y, button A */
    0x23, /* H, button B */
    0x16, /* U, button C */
    0x24, /* J, button D */
    0x17, /* I, button E */
    0x25, /* K, button F */
    0x18, /* O, button G */
    0x26  /* L, button H */
};

static const uint16_t icade_release[SDL_ICADE_CONTROLS] = {
    0x12, /* E, stick up */
    0x2E, /* C, stick right */
    0x2C, /* Z, stick down */
    0x10, /* Q, stick left */
    0x14, /* T, button A */
    0x13, /* R, button B */
    0x21, /* F, button C */
    0x31, /* N, button D */
    0x32, /* M, button E */
    0x19, /* P, button F */
    0x22, /* G, button G */
    0x2F  /* V, button H */
};

/* No source maps the cabinet to a gamepad. The bottom row B, D, F and H goes
   on south, east, right trigger and left trigger, and the top row A, C, E
   and G on west, north, right shoulder and left shoulder. */
static const int8_t icade_cabinet_slots[SDL_ICADE_SLOT_COUNT] = {
    1,  /* South: B */
    3,  /* East: D */
    0,  /* West: A */
    2,  /* North: C */
    -1, /* Back */
    -1, /* Start */
    6,  /* Left shoulder: G */
    4,  /* Right shoulder: E */
    7,  /* Left trigger: H */
    5   /* Right trigger: F */
};

/* iCade-iOS's table of what a pad's iCade letters mean (README.md:27-37),
   with the pad's Y, A, B and X read as on an Xbox pad */
static const int8_t icade_pad_slots[SDL_ICADE_SLOT_COUNT] = {
    5,  /* South: F, the pad's A */
    6,  /* East: G, the pad's B */
    7,  /* West: H, the pad's X */
    4,  /* North: E, the pad's Y */
    0,  /* Back: A, Select */
    2,  /* Start: C */
    1,  /* Left shoulder: B */
    3,  /* Right shoulder: D */
    -1, /* Left trigger */
    -1  /* Right trigger */
};

bool SDL_ICade_ApplyKey(uint16_t *state, uint16_t make_code, uint16_t flags)
{
    uint16_t before;
    int i;

    if (!state) {
        return false;
    }
    /* The key-up of a letter is ignored, since the next letter says what
       changed. E0 and E1 records and the overrun code carry no letter. */
    if ((flags & (SDL_ICADE_KEY_BREAK | SDL_ICADE_KEY_E0 | SDL_ICADE_KEY_E1)) != 0 || make_code == SDL_ICADE_OVERRUN) {
        return false;
    }
    before = *state;
    for (i = 0; i < SDL_ICADE_CONTROLS; ++i) {
        if (make_code == icade_press[i]) {
            *state = (uint16_t)(*state | (1u << i));
            break;
        }
        if (make_code == icade_release[i]) {
            *state = (uint16_t)(*state & ~(1u << i));
            break;
        }
    }
    return *state != before;
}

uint8_t SDL_ICade_GetHat(uint16_t state)
{
    const bool up = (state & SDL_ICADE_UP) != 0;
    const bool right = (state & SDL_ICADE_RIGHT) != 0;
    const bool down = (state & SDL_ICADE_DOWN) != 0;
    const bool left = (state & SDL_ICADE_LEFT) != 0;
    uint8_t hat = SDL_ICADE_HAT_CENTERED;

    if (up != down) {
        hat = (uint8_t)(hat | (up ? SDL_ICADE_HAT_UP : SDL_ICADE_HAT_DOWN));
    }
    if (left != right) {
        hat = (uint8_t)(hat | (left ? SDL_ICADE_HAT_LEFT : SDL_ICADE_HAT_RIGHT));
    }
    return hat;
}

bool SDL_ICade_GetButton(uint16_t state, int button)
{
    if (button < 0 || button >= SDL_ICADE_BUTTONS) {
        return false;
    }
    return (state & (SDL_ICADE_BUTTON_A << button)) != 0;
}

int SDL_ICade_GetSlotButton(SDL_ICadeLayout layout, SDL_ICadeSlot slot)
{
    if ((int)slot < 0 || (int)slot >= SDL_ICADE_SLOT_COUNT) {
        return -1;
    }
    switch (layout) {
    case SDL_ICADE_LAYOUT_CABINET:
        return icade_cabinet_slots[slot];
    case SDL_ICADE_LAYOUT_PAD:
        return icade_pad_slots[slot];
    default:
        return -1;
    }
}

static bool ICade_IsSpace(char c)
{
    return c == ' ' || c == '\t';
}

static void ICade_Trim(const char **start, const char **end)
{
    while (*start < *end && ICade_IsSpace(**start)) {
        ++*start;
    }
    while (*end > *start && ICade_IsSpace((*end)[-1])) {
        --*end;
    }
}

static int ICade_HexDigit(char c)
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

/* The whole of [start, end) is "0x" or "0X" and one to four hex digits */
static bool ICade_ParseNumber(const char *start, const char *end, uint16_t *value)
{
    const ptrdiff_t length = end - start;
    uint16_t result = 0;
    const char *p;

    if (length < 3 || length > 6 || start[0] != '0' || (start[1] != 'x' && start[1] != 'X')) {
        return false;
    }
    for (p = start + 2; p < end; ++p) {
        const int digit = ICade_HexDigit(*p);

        if (digit < 0) {
            return false;
        }
        result = (uint16_t)((result << 4) | digit);
    }
    *value = result;
    return true;
}

static void ICade_Reject(SDL_ICadeLogFunc log, void *userdata, const char *start, const char *end, const char *reason)
{
    if (log) {
        log(userdata, start, (size_t)(end - start), reason);
    }
}

int SDL_ICade_ParseDevices(const char *text, size_t length, SDL_ICadeDeviceList *list, SDL_ICadeLogFunc log, void *userdata)
{
    const char *p, *end;
    size_t size = 0;

    if (!list) {
        return 0;
    }
    memset(list, 0, sizeof(*list));
    if (!text) {
        return 0;
    }
    while (size < length && text[size]) {
        ++size;
    }
    p = text;
    end = text + size;
    while (p < end) {
        const char *entry_start = p;
        const char *entry_end = p;
        const char *slash, *vendor_start, *vendor_end, *product_start, *product_end;
        uint16_t vendor, product;
        uint32_t pair;
        int i;

        while (entry_end < end && *entry_end != ',') {
            ++entry_end;
        }
        p = (entry_end < end) ? entry_end + 1 : entry_end;

        ICade_Trim(&entry_start, &entry_end);
        if (entry_start == entry_end) {
            continue; /* An empty entry, as after a trailing comma */
        }
        slash = entry_start;
        while (slash < entry_end && *slash != '/') {
            ++slash;
        }
        if (slash == entry_end) {
            ICade_Reject(log, userdata, entry_start, entry_end, "no '/' between vendor and product");
            continue;
        }
        vendor_start = entry_start;
        vendor_end = slash;
        product_start = slash + 1;
        product_end = entry_end;
        ICade_Trim(&vendor_start, &vendor_end);
        ICade_Trim(&product_start, &product_end);
        if (!ICade_ParseNumber(vendor_start, vendor_end, &vendor)) {
            ICade_Reject(log, userdata, entry_start, entry_end, "vendor is not 0x and one to four hex digits");
            continue;
        }
        if (!ICade_ParseNumber(product_start, product_end, &product)) {
            ICade_Reject(log, userdata, entry_start, entry_end, "product is not 0x and one to four hex digits");
            continue;
        }
        if (vendor == 0) {
            ICade_Reject(log, userdata, entry_start, entry_end, "vendor 0x0000 names no device");
            continue;
        }
        pair = ((uint32_t)vendor << 16) | product;
        for (i = 0; i < list->count; ++i) {
            if (list->pairs[i] == pair) {
                break;
            }
        }
        if (i < list->count) {
            continue; /* A repeated pair is kept once */
        }
        if (list->count == SDL_ICADE_MAX_PAIRS) {
            ICade_Reject(log, userdata, entry_start, entry_end, "too many pairs");
            continue;
        }
        list->pairs[list->count++] = pair;
    }
    return list->count;
}

SDL_ICadeLayout SDL_ICade_Classify(uint16_t vendor, uint16_t product, const SDL_ICadeDeviceList *list)
{
    const uint32_t pair = ((uint32_t)vendor << 16) | product;
    int i;

    if (vendor == SDL_ICADE_VENDOR && product == SDL_ICADE_PRODUCT) {
        return SDL_ICADE_LAYOUT_CABINET;
    }
    if (list) {
        for (i = 0; i < list->count && i < SDL_ICADE_MAX_PAIRS; ++i) {
            if (list->pairs[i] == pair) {
                return SDL_ICADE_LAYOUT_PAD;
            }
        }
    }
    return SDL_ICADE_LAYOUT_NONE;
}

void SDL_ICade_ClearQueue(SDL_ICadeQueue *queue)
{
    if (queue) {
        queue->head = 0;
        queue->count = 0;
        queue->dropped = 0;
    }
}

void SDL_ICade_PushQueue(SDL_ICadeQueue *queue, uint16_t state, uint64_t time_ns)
{
    SDL_ICadeEvent *event;

    if (!queue) {
        return;
    }
    if (queue->count == SDL_ICADE_QUEUE_CAPACITY) {
        queue->head = (queue->head + 1) % SDL_ICADE_QUEUE_CAPACITY;
        --queue->count;
        ++queue->dropped;
    }
    event = &queue->events[(queue->head + queue->count) % SDL_ICADE_QUEUE_CAPACITY];
    event->sequence = ++queue->sequence;
    event->time_ns = time_ns;
    event->state = state;
    ++queue->count;
}

bool SDL_ICade_PopQueue(SDL_ICadeQueue *queue, SDL_ICadeEvent *event)
{
    if (!queue || !event || queue->count == 0) {
        return false;
    }
    *event = queue->events[queue->head];
    queue->head = (queue->head + 1) % SDL_ICADE_QUEUE_CAPACITY;
    --queue->count;
    return true;
}
