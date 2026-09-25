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

/* The Dream Cheeky USB Roll-Up Drum Kit, 1941:8021. Pure C99: no SDL
 * runtime and no I/O.
 *
 * Each 8-byte report, no report ID, holds the six pads in bits 0-5 of every
 * byte, and a pad is down when its bit is set in at least 4 of the 8 bytes.
 * A weather station and a missile launcher share the ID, which is why the
 * driver's hint defaults off. The facts are from VRPN. No code is copied.
 */

#ifndef SDL_hidapi_dreamcheeky_proto_h_
#define SDL_hidapi_dreamcheeky_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_DREAMCHEEKY_REPORT_LENGTH 8
#define SDL_DREAMCHEEKY_PADS          6
#define SDL_DREAMCHEEKY_DEBOUNCE      4 /* Of the 8 bytes */

/* One report of exactly 8 bytes: bit n of the result is pad n */
extern bool SDL_DreamCheeky_DecodeReport(const uint8_t *report, size_t length, uint8_t *pads);

#endif /* SDL_hidapi_dreamcheeky_proto_h_ */
