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
#include "SDL_internal.h"

#ifndef SDL_xinputjoystick_hwdata_h_
#define SDL_xinputjoystick_hwdata_h_

#ifdef SDL_JOYSTICK_XINPUT

#include "../SDL_sysjoystick.h"
#include "../../core/windows/SDL_windows.h"

/* The XInput driver's joystick data, shared with the Elite paddle reader.
   The DirectInput driver defines its own struct joystick_hwdata in
   SDL_dinputjoystick_c.h, so no file includes both headers. */
struct joystick_hwdata
{
    SDL_GUID guid;
    bool bXInputDevice; // true if this device supports using the xinput API rather than DirectInput
    bool bXInputHaptic; // Supports force feedback via XInput.
    Uint8 userid;       // XInput userid index for this joystick
    DWORD dwPacketNumber;
    int rb3pro_variant; // A Rock Band 3 Pro instrument, subtype 15 or 25
    BYTE rb3pro_extra[6]; // Its six trailing XUSB bytes in the last state posted
    // The firmware version lookup in GameInput (hifihedgehog/SDL#44)
    bool firmware_settled;   // Found, or no lookup can find it
    int firmware_generation; // GameInput's device list at the last miss, -1 to look again
#ifdef SDL_JOYSTICK_XINPUT_PADDLES
    void *paddle_context;
#endif
};

#endif // SDL_JOYSTICK_XINPUT

#endif // SDL_xinputjoystick_hwdata_h_
