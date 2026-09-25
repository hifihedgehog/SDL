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

/* The Zeemote JS1 and JS1 H in joystick mode, on the service
 * 8E1F0CF7-508F-4875-B62C-FBB67FD34812. hifihedgehog/SDL#33 Part 11.
 *
 * Frames are a length, A1, a type and the payload. The length counts the
 * bytes after itself. A byte other than A1 in the second place is dropped
 * and the scan starts again one byte later. Type 07 holds the whole pressed
 * set in six slots, 0 to 3 for A to D and FE for none. Type 08 holds the
 * stick index, then X and Y as signed bytes, down positive. Type 11 holds
 * the battery in millivolts, most significant byte first. Type 05 comes
 * after connecting and changes no control. The host sends nothing. The
 * facts are from ZeeClient (MIT), zeemouse (no license), zeemoted (GPL) and
 * android-bluez-ime (LGPL), the last three for facts only. No code from
 * them is copied.
 */

#ifndef SDL_rfcomm_zeemote_proto_h_
#define SDL_rfcomm_zeemote_proto_h_

#include "SDL_rfcomm_proto.h"

#define SDL_ZEEMOTE_MAX_FRAME    64 /* A longer frame is taken for noise */
#define SDL_ZEEMOTE_MAGIC        0xA1
#define SDL_ZEEMOTE_TYPE_STATUS  0x05
#define SDL_ZEEMOTE_TYPE_BUTTONS 0x07
#define SDL_ZEEMOTE_TYPE_STICK   0x08
#define SDL_ZEEMOTE_TYPE_BATTERY 0x11
#define SDL_ZEEMOTE_NO_BUTTON    0xFE

typedef enum SDL_ZeemoteAxis
{
    SDL_ZEEMOTE_AXIS_X,
    SDL_ZEEMOTE_AXIS_Y,
    SDL_ZEEMOTE_AXES
} SDL_ZeemoteAxis;

typedef enum SDL_ZeemoteButton
{
    SDL_ZEEMOTE_BUTTON_A,
    SDL_ZEEMOTE_BUTTON_B,
    SDL_ZEEMOTE_BUTTON_C,
    SDL_ZEEMOTE_BUTTON_D,
    SDL_ZEEMOTE_BUTTONS
} SDL_ZeemoteButton;

typedef struct SDL_ZeemoteState
{
    SDL_RFCOMMBase base;
    char name[SDL_SERIAL_NAME_LENGTH];
    uint8_t frame[SDL_ZEEMOTE_MAX_FRAME];
    size_t length; /* Bytes of a frame held from earlier reads */
    SDL_SerialControls controls;
} SDL_ZeemoteState;

extern const SDL_RFCOMMModule SDL_RFCOMMZeemoteModule;

#endif /* SDL_rfcomm_zeemote_proto_h_ */
