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

/* The Microsoft SideWinder Game Voice, 045E:003B. Pure C99: no SDL runtime
 * and no I/O. Its one input report is a single byte with no report ID, one
 * bit per button usage 0x11 to 0x18, in a Telephony headset collection. The
 * facts are from the 2007 linux-kernel thread and its Windows USBSnoop log.
 */

#ifndef SDL_hidapi_gamevoice_proto_h_
#define SDL_hidapi_gamevoice_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_GAMEVOICE_REPORT_LENGTH 1
#define SDL_GAMEVOICE_BUTTONS       8

/* One report of exactly one byte: bit n is button n */
extern bool SDL_GameVoice_DecodeReport(const uint8_t *report, size_t length, uint8_t *buttons);

#endif /* SDL_hidapi_gamevoice_proto_h_ */
