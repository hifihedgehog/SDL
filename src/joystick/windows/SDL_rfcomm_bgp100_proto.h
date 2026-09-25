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

/* The Chainpus BGP100 and the 2011 Phonejoy, on RFCOMM channel 1.
 * hifihedgehog/SDL#33 Part 11.
 *
 * The BGP100 sends two bytes per key event: B0 plus the key to press, F0
 * plus the key to release, then FF minus the first byte. A pair counts when
 * its bytes sum to FF, the first has bit 7 set and the second clear, and
 * the key is 4 to E: 4 Start, 5 B, 6 A, 7 C, 8 L, 9 R, A up, B left, C
 * right, D down, E D. Anything else drops one byte. The Phonejoy sends the
 * same pairs and more, decoded by table: L2 B1 4D and F1 0D, R2 B2 4E and
 * F2 0E, Select B3 4C and F3 0C, stick directions A0+n 20+n and E0+n 10+n
 * for n 1 to 8, and stick positions FF, an axis from 11 to 14 and a value
 * with 7F at center. A pair acts only when it is listed, FF with an axis
 * takes three bytes, and anything else drops one byte. No joystick appears
 * before the first valid event. The host sends nothing. The facts are from
 * zeemouse (no license) and android-bluez-ime (LGPL), both for facts only.
 * No code from them is copied.
 */

#ifndef SDL_rfcomm_bgp100_proto_h_
#define SDL_rfcomm_bgp100_proto_h_

#include "SDL_rfcomm_proto.h"

typedef enum SDL_BGP100Button
{
    SDL_BGP100_BUTTON_A,
    SDL_BGP100_BUTTON_B,
    SDL_BGP100_BUTTON_D,
    SDL_BGP100_BUTTON_C,
    SDL_BGP100_BUTTON_L,
    SDL_BGP100_BUTTON_R,
    SDL_BGP100_BUTTON_START,
    SDL_BGP100_BUTTON_UP,
    SDL_BGP100_BUTTON_DOWN,
    SDL_BGP100_BUTTON_LEFT,
    SDL_BGP100_BUTTON_RIGHT,
    SDL_BGP100_BUTTONS
} SDL_BGP100Button;

/* The Phonejoy's buttons 1 to 4 are keys 6, 5, E and 7 */
typedef enum SDL_PhonejoyButton
{
    SDL_PHONEJOY_BUTTON_3,
    SDL_PHONEJOY_BUTTON_2,
    SDL_PHONEJOY_BUTTON_4,
    SDL_PHONEJOY_BUTTON_1,
    SDL_PHONEJOY_BUTTON_L1,
    SDL_PHONEJOY_BUTTON_R1,
    SDL_PHONEJOY_BUTTON_SELECT,
    SDL_PHONEJOY_BUTTON_START,
    SDL_PHONEJOY_BUTTON_UP,
    SDL_PHONEJOY_BUTTON_DOWN,
    SDL_PHONEJOY_BUTTON_LEFT,
    SDL_PHONEJOY_BUTTON_RIGHT,
    SDL_PHONEJOY_BUTTONS
} SDL_PhonejoyButton;

typedef enum SDL_PhonejoyAxis
{
    SDL_PHONEJOY_AXIS_LEFT_X,
    SDL_PHONEJOY_AXIS_LEFT_Y,
    SDL_PHONEJOY_AXIS_RIGHT_X,
    SDL_PHONEJOY_AXIS_RIGHT_Y,
    SDL_PHONEJOY_AXIS_L2,
    SDL_PHONEJOY_AXIS_R2,
    SDL_PHONEJOY_AXES
} SDL_PhonejoyAxis;

typedef struct SDL_BGP100State
{
    SDL_RFCOMMBase base;
    bool phonejoy;
    uint8_t pending[3];
    size_t length;          /* Bytes of an event held from earlier reads */
    uint8_t directions;     /* Phonejoy stick directions held, bit n - 1 for pair n */
    SDL_SerialControls controls;
} SDL_BGP100State;

extern const SDL_RFCOMMModule SDL_RFCOMMBGP100Module;
extern const SDL_RFCOMMModule SDL_RFCOMMPhonejoyModule;

#endif /* SDL_rfcomm_bgp100_proto_h_ */
