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

/* HID collections the fork reads although they are not game controllers,
 * and what a report descriptor declares. Pure C99: no SDL runtime and no
 * I/O, so every decision here runs in the offline tests exactly as it runs
 * in the library.
 *
 * With SDL_HINT_HIDAPI_ENUMERATE_ONLY_CONTROLLERS on, the enumeration drops
 * every collection that is not a Generic Desktop joystick, gamepad or
 * multi-axis controller. The devices here carry their input in another
 * collection: a Telephony headset, a vendor page, or one no source records.
 */

#ifndef SDL_hidapi_collections_h_
#define SDL_hidapi_collections_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether a collection that is not a game controller reaches the HIDAPI
 * drivers: a row names its vendor, product and usage, or the device at any
 * usage. The collections Windows opens exclusively for itself (mouse,
 * keyboard, pens and touch) are never admitted. */
extern bool SDL_HIDAPI_IsAdmittedCollection(uint16_t vendor, uint16_t product, uint16_t usage_page, uint16_t usage);

/* The devices of this group that stay joysticks, with no gamepad mapping.
 * The Speed Force Wireless goes on the wheel list instead. */
extern bool SDL_HIDAPI_IsJoystickOnlyDevice(uint16_t vendor, uint16_t product);

/* The report IDs a report descriptor declares, one bit per ID. A descriptor
 * without Report ID items declares report 0 in each list it has items in. */
typedef struct SDL_HIDAPIReportIDs
{
    bool uses_report_ids;
    uint8_t input[32];
    uint8_t output[32];
    uint8_t feature[32];
} SDL_HIDAPIReportIDs;

/* Walks the items of a report descriptor. Push and Pop keep the Report ID
 * with the rest of the global state. Returns false for an item that runs
 * past the given length, a Pop without a Push or a Push deeper than 8.
 * Reads nothing at or past the given length. */
extern bool SDL_HIDAPI_ParseReportIDs(const uint8_t *descriptor, size_t length, SDL_HIDAPIReportIDs *out);

extern bool SDL_HIDAPI_HasReportID(const uint8_t ids[32], uint8_t id);

#endif /* SDL_hidapi_collections_h_ */
