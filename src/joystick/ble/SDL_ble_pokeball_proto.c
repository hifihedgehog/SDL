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

/* The report, from pokeball-plus-4-windows (PokeballController.cs:137-170)
 * and find-a-cig (README.md:28, pbp_sniff.py:21-22): byte 0 counts packets,
 * byte 1 bit 0 is the top button and bit 1 the stick press
 * (pokeball_mouse_working.py:113, :120, find-a-cig app/js/ball.js:18-19),
 * X is the low nibble of byte 3 over the high nibble of byte 2, Y is byte 4
 * and grows downward, and bytes 5-16 carry motion data whose layout no
 * source settles, so no sensor is registered. The stick centers and spans
 * are the midpoints and half-widths of the Windows tool's ranges, X 32-192
 * and Y 36-180 (PokeballController.cs:23-26). */

#include "SDL_ble_pokeball_proto.h"

#include <string.h>

static void Pokeball_Reset(void *state, const SDL_BLESink *sink, const SDL_BLEModuleContext *context)
{
    SDL_PokeballState *pokeball = (SDL_PokeballState *)state;
    SDL_BLEIdentity identity;

    (void)context;
    memset(pokeball, 0, sizeof(*pokeball));
    SDL_BLE_SetIdentity(&identity, "Poke Ball Plus", SDL_BLE_TYPE_GAMEPAD,
                        (1u << SDL_BLE_BUTTON_SOUTH) | (1u << SDL_BLE_BUTTON_EAST),
                        (1u << SDL_BLE_AXIS_LEFTX) | (1u << SDL_BLE_AXIS_LEFTY), true);
    SDL_BLE_ResetBase(&pokeball->base, sink, &identity);
}

/* Nothing to write. The joystick appears with the next report. */
static void Pokeball_Start(void *state, uint64_t now)
{
    SDL_PokeballState *pokeball = (SDL_PokeballState *)state;

    (void)now;
    pokeball->started = true;
}

static int16_t Pokeball_Axis(int value, int center, int span)
{
    return (int16_t)SDL_BLE_Clamp((int32_t)(value - center) * 32767 / span, -32767, 32767);
}

static void Pokeball_Value(void *state, int characteristic, const uint8_t *data, size_t length, uint64_t time_ns)
{
    SDL_PokeballState *pokeball = (SDL_PokeballState *)state;
    SDL_BLEControls controls = pokeball->base.controls;

    if (characteristic == SDL_POKEBALL_BATTERY) {
        if (length >= 1) {
            /* The Battery Level characteristic is a percentage */
            controls.battery = (int8_t)SDL_BLE_Clamp(data[0], 0, 100);
            SDL_BLE_Commit(&pokeball->base, &controls, time_ns);
        }
        return;
    }
    if (characteristic != SDL_POKEBALL_INPUT || length < SDL_POKEBALL_REPORT_SIZE) {
        return;
    }
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_EAST, (data[1] & 0x01) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_SOUTH, (data[1] & 0x02) != 0);
    controls.axes[SDL_BLE_AXIS_LEFTX] = Pokeball_Axis(((data[3] & 0x0F) << 4) | (data[2] >> 4), 112, 80);
    controls.axes[SDL_BLE_AXIS_LEFTY] = Pokeball_Axis(data[4], 108, 72);
    SDL_BLE_Commit(&pokeball->base, &controls, time_ns);
    if (pokeball->started) {
        pokeball->base.ready = true;
    }
}

const SDL_BLEModule SDL_BLEPokeballModule = {
    sizeof(SDL_PokeballState),
    Pokeball_Reset,
    Pokeball_Start,
    Pokeball_Value,
    SDL_BLE_NoWriteDone,
    SDL_BLE_NoTick,
    SDL_BLE_NoDeadline,
    SDL_BLE_NoClose
};

/* Found by its exact name (PokeballVigemDriver.cs:18, :59). Works without a
   bond: the Windows tool connects by address from the advertisement
   (PokeballVigemDriver.cs:68). The battery level is optional. It is
   subscribed, then read once after the input subscription, as find-a-cig
   reads it (app/js/ball.js:36-49). The Windows tool reads it and subscribes
   it too (PokeballController.cs:90-117). */
const SDL_BLEFamily SDL_BLEPokeballFamily = {
    "Poke Ball Plus",
    1,
    {
        { SDL_BLE_KEY_NAME_EQUALS, false, "Pokemon PBP", SDL_BLE_UUID16_INIT(0x00, 0x00), 0, 0, { 0 } },
    },
    2,
    {
        /* 6675e16c-f36d-4567-bb55-6b51e27a23e5 */
        SDL_BLE_UUID_INIT(0x66, 0x75, 0xe1, 0x6c, 0xf3, 0x6d, 0x45, 0x67, 0xbb, 0x55, 0x6b, 0x51, 0xe2, 0x7a, 0x23, 0xe5),
        SDL_BLE_UUID16_INIT(0x18, 0x0f),
    },
    false,
    SDL_BLE_UUID16_INIT(0x00, 0x00),
    2,
    {
        /* 6675e16c-f36d-4567-bb55-6b51e27a23e6 */
        { 0, SDL_BLE_UUID_INIT(0x66, 0x75, 0xe1, 0x6c, 0xf3, 0x6d, 0x45, 0x67, 0xbb, 0x55, 0x6b, 0x51, 0xe2, 0x7a, 0x23, 0xe6),
          SDL_BLE_CHAR_SUBSCRIBE },
        { 1, SDL_BLE_UUID16_INIT(0x2a, 0x19), SDL_BLE_CHAR_SUBSCRIBE | SDL_BLE_CHAR_READ | SDL_BLE_CHAR_OPTIONAL },
    },
    SDL_BLE_PAIR_NOT_NEEDED,
    &SDL_BLEPokeballModule
};
