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
#ifndef SDL_xinput_firmware_c_h_
#define SDL_xinput_firmware_c_h_

#include "SDL_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SDL_JOYSTICK_XINPUT
/* PadForge fork: the firmware version GameInput reports for the device in an
   XInput slot (hifihedgehog/SDL#44). Device information only: no joystick,
   input or device ownership comes from GameInput. All calls run under SDL's
   joystick lock. */
void SDL_XINPUT_FirmwareInit(void);
void SDL_XINPUT_FirmwareOpen(SDL_Joystick *joystick);
void SDL_XINPUT_FirmwareUpdate(SDL_Joystick *joystick);
void SDL_XINPUT_FirmwareLost(SDL_Joystick *joystick);
void SDL_XINPUT_FirmwareQuit(void);
#endif

#ifdef __cplusplus
}
#endif

#endif // SDL_xinput_firmware_c_h_
