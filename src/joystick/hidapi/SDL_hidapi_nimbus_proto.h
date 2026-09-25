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

/* The SteelSeries Nimbus over Bluetooth, 0111:1420. Pure C99: no SDL
 * runtime and no I/O.
 *
 * After the report ID, if the collection declares one, each report is 17
 * bytes, one per control: the D-pad, A, B, X, Y, the shoulders and triggers
 * as 0-255 pressures, Menu, then four stick bytes that read two's
 * complement. The facts are from MFIGamepadFeeder's Nimbus mapping. No code
 * is copied.
 */

#ifndef SDL_hidapi_nimbus_proto_h_
#define SDL_hidapi_nimbus_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_NIMBUS_PAYLOAD_LENGTH 17
#define SDL_NIMBUS_AXES           6 /* Left X and Y, right X and Y, left and right trigger */
#define SDL_NIMBUS_BUTTONS        7 /* A, B, X, Y, left and right shoulder, Menu */

#define SDL_NIMBUS_MAPPING "a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,start:b6,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,"

typedef struct SDL_NimbusState
{
    uint8_t buttons;
    uint8_t hat; /* SDL hat bits */
    int16_t axes[SDL_NIMBUS_AXES];
} SDL_NimbusState;

/* One report: 18 bytes when the collection declares a report ID, 17 when it
 * does not. Any other length changes nothing. */
extern bool SDL_Nimbus_DecodeReport(const uint8_t *report, size_t length, bool has_report_id, SDL_NimbusState *out);

/* A stick byte as an axis: 0-127 up to 32767, 128-255 as -128 to -1 times 256 */
extern int16_t SDL_Nimbus_StickAxis(uint8_t value);

#endif /* SDL_hidapi_nimbus_proto_h_ */
