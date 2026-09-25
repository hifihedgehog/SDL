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

/* The PhoenixRC USB adapter's HID clones, 1781:0898. Pure C99: no SDL
 * runtime and no I/O.
 *
 * Each 8-byte report, no report ID, carries channels 1-6 in bytes 0 and 2-6.
 * Byte 1 carries nothing. Byte 7 carries channel 8 and channel 7 on
 * alternating reports, starting with channel 8, with nothing in the report
 * to say which. The facts are from Linux hid-pxrc and the hid-rcsim patches.
 * No code from them is copied.
 */

#ifndef SDL_hidapi_rcadapter_proto_h_
#define SDL_hidapi_rcadapter_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_RCADAPTER_REPORT_LENGTH 8
#define SDL_RCADAPTER_AXES          8 /* Channels 1-8 in order */

typedef struct SDL_RCAdapterState
{
    int16_t axes[SDL_RCADAPTER_AXES];
    bool next_is_channel_7; /* false: the next report's byte 7 is channel 8 */
} SDL_RCAdapterState;

/* Every channel centered, and the next report's byte 7 channel 8 */
extern void SDL_RCAdapter_Init(SDL_RCAdapterState *state);

/* Applies one report of exactly 8 bytes. Anything else changes nothing and
 * does not advance the byte 7 channel. */
extern bool SDL_RCAdapter_HandleReport(SDL_RCAdapterState *state, const uint8_t *report, size_t length);

#endif /* SDL_hidapi_rcadapter_proto_h_ */
