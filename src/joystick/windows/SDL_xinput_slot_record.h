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
#ifndef SDL_xinput_slot_record_h_
#define SDL_xinput_slot_record_h_

/* PadForge fork: which physical controller an XInput slot's interface
   belongs to, for the firmware lookup's container match
   (hifihedgehog/SDL#44). The paddle route's identity resolver reads it, in
   SDL_xinput_paddle_identity.cpp, from PnP and registry metadata only. No
   device opens. The metadata gives no firmware version: a product version
   is not one (hifihedgehog/SDL#46). */

#include <guiddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

typedef struct SDL_XInputSlotRecord
{
    GUID container; // The controller's device container
} SDL_XInputSlotRecord;

#ifdef __cplusplus
extern "C" {
#endif

/* False, with the record zeroed, when the resolver does not qualify the
   ancestry of the slot's interface */
extern bool SDL_XINPUT_ReadSlotRecord(const wchar_t *path, SDL_XInputSlotRecord *record);

#ifdef __cplusplus
}
#endif

#endif // SDL_xinput_slot_record_h_
