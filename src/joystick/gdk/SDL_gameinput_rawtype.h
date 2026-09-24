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

/* The instruments the GameInput backend reads through raw device reports,
 * keyed by USB ID. Pure C: no SDL runtime, so the offline tests include it
 * as SDL_gameinputjoystick.cpp does. */

#ifndef SDL_gameinput_rawtype_h_
#define SDL_gameinput_rawtype_h_

#include <stdint.h>

#include "../usb_ids.h"

enum
{
    SDL_GAMEINPUT_RAWTYPE_NONE,
    SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_GUITAR,
    SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_DRUM_KIT,
    SDL_GAMEINPUT_RAWTYPE_GUITAR_HERO_LIVE_GUITAR,
    SDL_GAMEINPUT_RAWTYPE_LEGACY_ADAPTER,
};

static int SDL_GameInputRawTypeForDevice(uint16_t vendor, uint16_t product)
{
    switch (vendor) {
    case USB_VENDOR_MADCATZ:
        switch (product) {
        case USB_PRODUCT_MADCATZ_XB1_STRATOCASTER_GUITAR:
            return SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_GUITAR;
        case USB_PRODUCT_MADCATZ_XB1_DRUM_KIT:
            return SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_DRUM_KIT;
        case USB_PRODUCT_MADCATZ_XB1_LEGACY_ADAPTER:
            return SDL_GAMEINPUT_RAWTYPE_LEGACY_ADAPTER;
        default:
            break;
        }
        break;
    case USB_VENDOR_PDP:
        switch (product) {
        case USB_PRODUCT_PDP_XB1_JAGUAR_GUITAR:
        case USB_PRODUCT_PDP_XB1_RIFFMASTER_GUITAR:
            return SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_GUITAR;
        case USB_PRODUCT_PDP_XB1_DRUM_KIT:
            return SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_DRUM_KIT;
        default:
            break;
        }
        break;
    case USB_VENDOR_CRKD:
        switch (product) {
        case USB_PRODUCT_RED_OCTANE_XB1_STAGE_TOUR_GUITAR:
            return SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_GUITAR;
        default:
            break;
        }
        break;
    case USB_VENDOR_RED_OCTANE:
        switch (product) {
        case USB_PRODUCT_RED_OCTANE_XB1_GUITAR_HERO_LIVE_GUITAR:
            return SDL_GAMEINPUT_RAWTYPE_GUITAR_HERO_LIVE_GUITAR;
        default:
            break;
        }
        break;
    case USB_VENDOR_RED_OCTANE_GAMES:
        switch (product) {
        case USB_PRODUCT_RED_OCTANE_XB1_STAGE_TOUR_GUITAR:
            return SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_GUITAR;
        case USB_PRODUCT_RED_OCTANE_XB1_STAGE_TOUR_DRUMS:
            return SDL_GAMEINPUT_RAWTYPE_ROCK_BAND_DRUM_KIT;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return SDL_GAMEINPUT_RAWTYPE_NONE;
}

#endif /* SDL_gameinput_rawtype_h_ */
