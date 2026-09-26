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

/* The report has no report ID: GHLtarUtility, ghlioscon and Santroller read
 * the frets at byte 0 (iOSGuitar.cs:69, ghlioscon.swift:210,
 * bt_ble_host.cpp:100). Byte 0 holds white 1, black 1, 2 and 3, white 2 and
 * 3 in bits 0 to 5. Byte 1 holds pause, GHTV, hero power and sync in bits 1
 * to 4. Byte 2 is the D-pad hat of PlasticBand's struct shifted by one byte
 * (iOS.md:74-79), 0 up, clockwise to 7, 15 neutral. Byte 4 is the strum bar,
 * 80 at rest, 00 up and FF down (iOS.md:31-33). Byte 6 is the whammy from 80
 * to FF and byte 19 the tilt (ghlioscon.swift:222-223), level at 128
 * (ghlioscon.swift:23).
 *
 * The layout follows the Xbox 360 GHL guitar that GHLtarUtility produces
 * (iOSGuitar.cs:70-110), with SDL's signs: SDL negates XInput's Y axes, so
 * strum up is left Y -32768 and a pressed whammy is right Y -32768. */

#include "SDL_ble_ghlios_proto.h"

#include <string.h>

static void GHLiOS_Reset(void *state, const SDL_BLESink *sink, const SDL_BLEModuleContext *context)
{
    SDL_GHLiOSState *guitar = (SDL_GHLiOSState *)state;
    SDL_BLEIdentity identity;

    (void)context;
    memset(guitar, 0, sizeof(*guitar));
    SDL_BLE_SetIdentity(&identity, "Guitar Hero Live Guitar (iOS)", SDL_BLE_TYPE_GUITAR,
                        (1u << SDL_BLE_BUTTON_SOUTH) | (1u << SDL_BLE_BUTTON_EAST) | (1u << SDL_BLE_BUTTON_WEST) |
                            (1u << SDL_BLE_BUTTON_NORTH) | (1u << SDL_BLE_BUTTON_BACK) | (1u << SDL_BLE_BUTTON_GUIDE) |
                            (1u << SDL_BLE_BUTTON_START) | (1u << SDL_BLE_BUTTON_LEFT_STICK) |
                            (1u << SDL_BLE_BUTTON_LEFT_SHOULDER) | (1u << SDL_BLE_BUTTON_RIGHT_SHOULDER) |
                            (1u << SDL_BLE_BUTTON_DPAD_UP) | (1u << SDL_BLE_BUTTON_DPAD_DOWN) |
                            (1u << SDL_BLE_BUTTON_DPAD_LEFT) | (1u << SDL_BLE_BUTTON_DPAD_RIGHT),
                        (1u << SDL_BLE_AXIS_LEFTY) | (1u << SDL_BLE_AXIS_RIGHTX) | (1u << SDL_BLE_AXIS_RIGHTY), true);
    SDL_BLE_ResetBase(&guitar->base, sink, &identity);
}

/* Nothing to write. The joystick appears with the next report. */
static void GHLiOS_Start(void *state, uint64_t now)
{
    SDL_GHLiOSState *guitar = (SDL_GHLiOSState *)state;

    (void)now;
    guitar->started = true;
}

static void GHLiOS_Value(void *state, int characteristic, const uint8_t *data, size_t length, uint64_t time_ns)
{
    SDL_GHLiOSState *guitar = (SDL_GHLiOSState *)state;
    SDL_BLEControls controls = guitar->base.controls;
    uint8_t hat;
    bool up, down, left, right;

    if (characteristic != SDL_GHLIOS_INPUT || length < SDL_GHLIOS_REPORT_SIZE) {
        return;
    }
    hat = data[2];
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_WEST, (data[0] & 0x01) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_SOUTH, (data[0] & 0x02) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_EAST, (data[0] & 0x04) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_NORTH, (data[0] & 0x08) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_LEFT_SHOULDER, (data[0] & 0x10) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_RIGHT_SHOULDER, (data[0] & 0x20) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_START, (data[1] & 0x02) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_LEFT_STICK, (data[1] & 0x04) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_BACK, (data[1] & 0x08) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_GUIDE, (data[1] & 0x10) != 0);

    /* The strum bar and the hat share the D-pad buttons */
    up = (data[4] == 0x00) || hat == 0 || hat == 1 || hat == 7;
    right = hat == 1 || hat == 2 || hat == 3;
    down = (data[4] == 0xFF) || hat == 3 || hat == 4 || hat == 5;
    left = hat == 5 || hat == 6 || hat == 7;
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_DPAD_UP, up);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_DPAD_DOWN, down);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_DPAD_LEFT, left);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_DPAD_RIGHT, right);
    if (data[4] == 0x00) {
        controls.axes[SDL_BLE_AXIS_LEFTY] = -32768;
    } else if (data[4] == 0xFF) {
        controls.axes[SDL_BLE_AXIS_LEFTY] = 32767;
    } else {
        controls.axes[SDL_BLE_AXIS_LEFTY] = 0;
    }
    controls.axes[SDL_BLE_AXIS_RIGHTY] = (int16_t)SDL_BLE_Clamp(-(((int32_t)data[6] - 128) * 32768 / 127), -32768, 0);
    controls.axes[SDL_BLE_AXIS_RIGHTX] = (int16_t)SDL_BLE_Clamp(((int32_t)data[19] - 128) * 256, -32768, 32767);
    SDL_BLE_Commit(&guitar->base, &controls, time_ns);
    if (guitar->started) {
        guitar->base.ready = true;
    }
}

const SDL_BLEModule SDL_BLEGHLiOSModule = {
    sizeof(SDL_GHLiOSState),
    GHLiOS_Reset,
    GHLiOS_Start,
    GHLiOS_Value,
    SDL_BLE_NoWriteDone,
    SDL_BLE_NoTick,
    SDL_BLE_NoDeadline,
    SDL_BLE_NoClose
};

/* Found by a name containing "Ble Guitar", as GHLtarUtility and Santroller
   accept (MainWindow.cs:300-306, ble_rx.cpp:677), or by the 16-bit service
   1523 ghlioscon scans for (ghlioscon.swift:172, :190). Every implementation
   only enables notifications. No bond is needed: Santroller asks to pair
   (ble_rx.cpp:811) and goes on to the guitar's characteristic when pairing
   fails (ble_rx.cpp:878), and GHLtarUtility connects by address straight
   from the advertisement (MainWindow.cs:304). */
const SDL_BLEFamily SDL_BLEGHLiOSFamily = {
    "Guitar Hero Live iOS",
    2,
    {
        { SDL_BLE_KEY_NAME_CONTAINS, false, "Ble Guitar", SDL_BLE_UUID16_INIT(0x00, 0x00), 0, 0, { 0 } },
        { SDL_BLE_KEY_SERVICE, false, NULL, SDL_BLE_UUID16_INIT(0x15, 0x23), 0, 0, { 0 } },
    },
    1,
    {
        /* 533e1523-3abe-f33f-cd00-594e8b0a8ea3 */
        SDL_BLE_UUID_INIT(0x53, 0x3e, 0x15, 0x23, 0x3a, 0xbe, 0xf3, 0x3f, 0xcd, 0x00, 0x59, 0x4e, 0x8b, 0x0a, 0x8e, 0xa3),
    },
    false,
    SDL_BLE_UUID16_INIT(0x00, 0x00),
    1,
    {
        /* 533e1524-3abe-f33f-cd00-594e8b0a8ea3 */
        { 0, SDL_BLE_UUID_INIT(0x53, 0x3e, 0x15, 0x24, 0x3a, 0xbe, 0xf3, 0x3f, 0xcd, 0x00, 0x59, 0x4e, 0x8b, 0x0a, 0x8e, 0xa3),
          SDL_BLE_CHAR_SUBSCRIBE },
    },
    SDL_BLE_PAIR_NOT_NEEDED,
    &SDL_BLEGHLiOSModule
};
