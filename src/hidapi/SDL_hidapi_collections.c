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

#include "SDL_hidapi_collections.h"

#include "../joystick/usb_ids.h"

#include <string.h>

#define COLLECTIONS_ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))

#define COLLECTIONS_ANY_USAGE 0x01

typedef struct SDL_HIDAPIAdmittedCollection
{
    uint16_t vendor;
    uint16_t product;
    uint8_t flags;
    uint16_t usage_page; /* Without COLLECTIONS_ANY_USAGE */
    uint16_t usage;
} SDL_HIDAPIAdmittedCollection;

/* Windows makes one device per top-level collection. A row with a usage
 * admits that collection. A row with any usage admits every collection of
 * the device, and the driver picks among them. */
static const SDL_HIDAPIAdmittedCollection SDL_hidapi_admitted_collections[] = {
    /* The descriptor is broken and not recorded */
    { USB_VENDOR_MULTIPLE_1781, USB_PRODUCT_PHOENIXRC_ADAPTER, COLLECTIONS_ANY_USAGE, 0, 0 },
    /* Telephony, headset */
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_MICROSOFT_SIDEWINDER_GAME_VOICE, 0, 0x000B, 0x0005 },
    /* The glove's native collection. Its mouse interface stays with Windows. */
    { USB_VENDOR_P5, USB_PRODUCT_P5_GLOVE, 0, 0x008C, 0x0001 },
    /* Not recorded */
    { USB_VENDOR_DREAMCHEEKY, USB_PRODUCT_DREAMCHEEKY_DRUM_KIT, COLLECTIONS_ANY_USAGE, 0, 0 },
    /* Vendor page, from the NIA's descriptor */
    { USB_VENDOR_NIA, USB_PRODUCT_NIA, 0, 0xFF00, 0xFF01 },
    /* VR Controls, head tracker in the one log that names it */
    { USB_VENDOR_OCULUS, USB_PRODUCT_OCULUS_RIFT_DK1, COLLECTIONS_ANY_USAGE, 0, 0 },
    /* Not recorded */
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_MICROSOFT_WMR_CONTROLLER, COLLECTIONS_ANY_USAGE, 0, 0 },
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_MICROSOFT_WMR_CONTROLLER_ODYSSEY, COLLECTIONS_ANY_USAGE, 0, 0 },
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_MICROSOFT_WMR_CONTROLLER_REVERB_G2, COLLECTIONS_ANY_USAGE, 0, 0 },
    /* Bluetooth. Windows leaves it out of the game controllers. */
    { USB_VENDOR_STEELSERIES_BT, USB_PRODUCT_STEELSERIES_NIMBUS, COLLECTIONS_ANY_USAGE, 0, 0 },
    /* Interface 1. The driver picks the collection that declares the key reports. */
    { USB_VENDOR_CREATIVE, USB_PRODUCT_CREATIVE_PRODIKEYS, COLLECTIONS_ANY_USAGE, 0, 0 },
};

static const struct
{
    uint16_t vendor;
    uint16_t product;
} SDL_hidapi_joystick_only_devices[] = {
    { USB_VENDOR_MULTIPLE_1781, USB_PRODUCT_PHOENIXRC_ADAPTER },
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_MICROSOFT_SIDEWINDER_GAME_VOICE },
    { USB_VENDOR_P5, USB_PRODUCT_P5_GLOVE },
    { USB_VENDOR_DREAMCHEEKY, USB_PRODUCT_DREAMCHEEKY_DRUM_KIT },
    { USB_VENDOR_NIA, USB_PRODUCT_NIA },
    { USB_VENDOR_IN2GAMES, USB_PRODUCT_IN2GAMES_GAMETRAK },
    { USB_VENDOR_OCULUS, USB_PRODUCT_OCULUS_RIFT_DK1 },
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_MICROSOFT_WMR_CONTROLLER },
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_MICROSOFT_WMR_CONTROLLER_ODYSSEY },
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_MICROSOFT_WMR_CONTROLLER_REVERB_G2 },
    { USB_VENDOR_CREATIVE, USB_PRODUCT_CREATIVE_PRODIKEYS },
    { USB_VENDOR_NATURALPOINT, USB_PRODUCT_NATURALPOINT_TRACKIR2 },
    { USB_VENDOR_NATURALPOINT, USB_PRODUCT_NATURALPOINT_TRACKIR3 },
};

/* The collections Windows opens exclusively for its own use (windows-driver-docs,
 * top-level-collections-opened-by-windows-for-system-use): mouse, keyboard,
 * pens, touchscreen and touchpad. */
static bool Collections_IsSystemExclusive(uint16_t usage_page, uint16_t usage)
{
    if (usage_page == 0x0001) {
        return usage == 0x0001 || usage == 0x0002 || usage == 0x0006 || usage == 0x0007;
    }
    if (usage_page == 0x000D) {
        return usage == 0x0001 || usage == 0x0002 || usage == 0x0004 || usage == 0x0005;
    }
    return false;
}

bool SDL_HIDAPI_IsAdmittedCollection(uint16_t vendor, uint16_t product, uint16_t usage_page, uint16_t usage)
{
    size_t i;

    if (Collections_IsSystemExclusive(usage_page, usage)) {
        return false;
    }
    for (i = 0; i < COLLECTIONS_ARRAYSIZE(SDL_hidapi_admitted_collections); ++i) {
        const SDL_HIDAPIAdmittedCollection *row = &SDL_hidapi_admitted_collections[i];
        if (row->vendor != vendor || row->product != product) {
            continue;
        }
        if ((row->flags & COLLECTIONS_ANY_USAGE) ||
            (row->usage_page == usage_page && row->usage == usage)) {
            return true;
        }
    }
    return false;
}

bool SDL_HIDAPI_IsJoystickOnlyDevice(uint16_t vendor, uint16_t product)
{
    size_t i;

    for (i = 0; i < COLLECTIONS_ARRAYSIZE(SDL_hidapi_joystick_only_devices); ++i) {
        if (SDL_hidapi_joystick_only_devices[i].vendor == vendor &&
            SDL_hidapi_joystick_only_devices[i].product == product) {
            return true;
        }
    }
    return false;
}

#define COLLECTIONS_STACK_DEPTH 8

static void Collections_Mark(uint8_t ids[32], uint8_t id)
{
    ids[id >> 3] |= (uint8_t)(1u << (id & 7));
}

bool SDL_HIDAPI_HasReportID(const uint8_t ids[32], uint8_t id)
{
    return ids && (ids[id >> 3] & (1u << (id & 7))) != 0;
}

bool SDL_HIDAPI_ParseReportIDs(const uint8_t *descriptor, size_t length, SDL_HIDAPIReportIDs *out)
{
    uint8_t stack[COLLECTIONS_STACK_DEPTH];
    int depth = 0;
    uint8_t report_id = 0;
    size_t position = 0;

    if (!out || (length && !descriptor)) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    while (position < length) {
        const uint8_t prefix = descriptor[position];
        size_t size, i;
        uint32_t value = 0;
        uint8_t type, tag;

        if (prefix == 0xFE) {
            /* A long item: its data size, its tag, then the data */
            if (length - position < 3 || (size_t)descriptor[position + 1] > length - position - 3) {
                return false;
            }
            position += 3 + descriptor[position + 1];
            continue;
        }

        size = prefix & 0x03;
        if (size == 3) {
            size = 4;
        }
        if (size > length - position - 1) {
            return false;
        }
        for (i = 0; i < size; ++i) {
            value |= (uint32_t)descriptor[position + 1 + i] << (8 * i);
        }
        type = (uint8_t)((prefix >> 2) & 0x03);
        tag = (uint8_t)(prefix >> 4);

        if (type == 0) {
            /* Main items */
            if (tag == 0x8) {
                Collections_Mark(out->input, report_id);
            } else if (tag == 0x9) {
                Collections_Mark(out->output, report_id);
            } else if (tag == 0xB) {
                Collections_Mark(out->feature, report_id);
            }
        } else if (type == 1) {
            /* Global items: Report ID, Push and Pop */
            if (tag == 0x8) {
                report_id = (uint8_t)(value & 0xFF);
                out->uses_report_ids = true;
            } else if (tag == 0xA) {
                if (depth == COLLECTIONS_STACK_DEPTH) {
                    return false;
                }
                stack[depth++] = report_id;
            } else if (tag == 0xB) {
                if (depth == 0) {
                    return false;
                }
                report_id = stack[--depth];
            }
        }
        position += 1 + size;
    }
    return true;
}
