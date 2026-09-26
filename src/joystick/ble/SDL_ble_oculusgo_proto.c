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

/* From OculusGo-Air-Mouse-4-macOS, mapped on firmware 2.7.1 (main.swift:3-9,
 * :106-131, :670-672). 8126FACE sends 20 little-endian bytes: a counter, the
 * accelerometer at about 127 per g, the magnetometer, touch X and Y from 0
 * to about 315, a slow value, and the buttons in byte 19: Oculus, back,
 * trigger, touchpad click and touching. 8126BEEF sends three gyroscope
 * samples whose scale no source gives. Nothing is decoded from it.
 *
 * No sensor is registered. The only reference names raw Y the axis along
 * the controller (main.swift:90, :491-492, README.md:121), so raw X, Y and Z
 * passed through as SDL's axes would put gravity off SDL's positive Y at
 * rest (SDL_sensor.h:69-72, :98-103), and no source gives the frame. The
 * gyroscope is withheld as well, since no source gives its scale or axes
 * (main.swift:84-90 works in raw units).
 *
 * The macOS app checks once a second. Once 3 s pass without a value of 20
 * bytes or more on either characteristic, it releases every control, once
 * for the silence, and writes the subscriptions again, then again at every
 * check while the silence lasts (main.swift:646-655, :667-668). Only an
 * 8126FACE value lets a later silence release again (main.swift:674-675).
 * The module does the same on its own clock: the first request 3 s after
 * the last value, then one every 1 s. The app's first check, 1 s after it
 * subscribes, already counts as a silence, because its last value time
 * starts at Date.distantPast (main.swift:535). The module keeps the part's
 * 3 s from its start instead. */

#include "SDL_ble_oculusgo_proto.h"

#include <string.h>

static void OculusGo_Reset(void *state, const SDL_BLESink *sink, const SDL_BLEModuleContext *context)
{
    SDL_OculusGoState *go = (SDL_OculusGoState *)state;
    SDL_BLEIdentity identity;

    (void)context;
    memset(go, 0, sizeof(*go));
    SDL_BLE_SetIdentity(&identity, "Oculus Go Controller", SDL_BLE_TYPE_GAMEPAD,
                        (1u << SDL_BLE_BUTTON_SOUTH) | (1u << SDL_BLE_BUTTON_BACK) | (1u << SDL_BLE_BUTTON_GUIDE),
                        (1u << SDL_BLE_AXIS_LEFTX) | (1u << SDL_BLE_AXIS_LEFTY) | (1u << SDL_BLE_AXIS_RIGHT_TRIGGER), true);
    identity.touchpad = true;
    SDL_BLE_ResetBase(&go->base, sink, &identity);
}

static void OculusGo_Start(void *state, uint64_t now)
{
    SDL_OculusGoState *go = (SDL_OculusGoState *)state;

    go->started = true;
    go->deadline = now + SDL_OCULUSGO_SILENCE_MS;
}

static int16_t OculusGo_TouchAxis(int value)
{
    return (int16_t)SDL_BLE_Clamp((int32_t)(value - 158) * 32767 / 157, -32768, 32767);
}

static void OculusGo_Value(void *state, int characteristic, const uint8_t *data, size_t length, uint64_t time_ns)
{
    SDL_OculusGoState *go = (SDL_OculusGoState *)state;
    SDL_BLEControls controls = go->base.controls;
    int touch_x, touch_y;

    if ((characteristic != SDL_OCULUSGO_FACE && characteristic != SDL_OCULUSGO_BEEF) ||
        length < SDL_OCULUSGO_REPORT_SIZE) {
        return;
    }
    /* A value on either characteristic holds the next check off
       (main.swift:667-668), and only an 8126FACE value clears the release
       made for a silence (main.swift:674-675) */
    go->deadline = time_ns / 1000000 + SDL_OCULUSGO_SILENCE_MS;
    if (characteristic == SDL_OCULUSGO_BEEF) {
        return;
    }
    go->silent = false;

    touch_x = data[14] | (data[15] << 8);
    touch_y = data[16] | (data[17] << 8);
    controls.finger = (data[19] & 0x10) != 0;
    if (controls.finger) {
        /* The fields are 16 bits wide, and a value past 315 reads as the edge */
        controls.finger_x = (float)SDL_BLE_Clamp(touch_x, 0, 315) / 315.0f;
        controls.finger_y = (float)SDL_BLE_Clamp(touch_y, 0, 315) / 315.0f;
        controls.axes[SDL_BLE_AXIS_LEFTX] = OculusGo_TouchAxis(touch_x);
        controls.axes[SDL_BLE_AXIS_LEFTY] = OculusGo_TouchAxis(touch_y);
    } else {
        controls.finger_x = 0.0f;
        controls.finger_y = 0.0f;
        controls.axes[SDL_BLE_AXIS_LEFTX] = 0;
        controls.axes[SDL_BLE_AXIS_LEFTY] = 0;
    }
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_GUIDE, (data[19] & 0x01) != 0);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_BACK, (data[19] & 0x02) != 0);
    controls.axes[SDL_BLE_AXIS_RIGHT_TRIGGER] = (data[19] & 0x04) ? 32767 : -32768;
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_SOUTH, (data[19] & 0x08) != 0);
    SDL_BLE_Commit(&go->base, &controls, time_ns);
    if (go->started) {
        go->base.ready = true;
    }
}

/* A silence: every control released once, and the subscriptions written
   again now and every 1 s while it lasts (main.swift:646-655) */
static void OculusGo_Tick(void *state, uint64_t now)
{
    SDL_OculusGoState *go = (SDL_OculusGoState *)state;

    if (!go->started || now < go->deadline) {
        return;
    }
    if (!go->silent) {
        go->silent = true;
        SDL_BLE_Release(&go->base, now * 1000000);
    }
    go->base.resubscribe = true;
    go->deadline = now + SDL_OCULUSGO_RESUBSCRIBE_MS;
}

static bool OculusGo_GetDeadline(void *state, uint64_t *deadline)
{
    SDL_OculusGoState *go = (SDL_OculusGoState *)state;

    if (!go->started) {
        return false;
    }
    *deadline = go->deadline;
    return true;
}

static void OculusGo_Close(void *state, uint64_t now)
{
    SDL_OculusGoState *go = (SDL_OculusGoState *)state;

    (void)now;
    go->started = false;
}

const SDL_BLEModule SDL_BLEOculusGoModule = {
    sizeof(SDL_OculusGoState),
    OculusGo_Reset,
    OculusGo_Start,
    OculusGo_Value,
    SDL_BLE_NoWriteDone,
    OculusGo_Tick,
    OculusGo_GetDeadline,
    OculusGo_Close
};

/* Found by a name starting with OMVR or by its service, as the macOS app
   accepts either (main.swift:589). An unpaired subscription fails with
   insufficient encryption (README.md:139), so it pairs on error. 8126BEEF is
   optional: nothing is decoded from it, and the macOS app subscribes to
   whichever of the two it finds (main.swift:641-644). */
const SDL_BLEFamily SDL_BLEOculusGoFamily = {
    "Oculus Go",
    2,
    {
        { SDL_BLE_KEY_NAME_PREFIX, false, "OMVR", SDL_BLE_UUID16_INIT(0x00, 0x00), 0, 0, { 0 } },
        { SDL_BLE_KEY_SERVICE, false, NULL,
          /* 81265652-3692-ae93-e711-270f223c83b3 */
          SDL_BLE_UUID_INIT(0x81, 0x26, 0x56, 0x52, 0x36, 0x92, 0xae, 0x93, 0xe7, 0x11, 0x27, 0x0f, 0x22, 0x3c, 0x83, 0xb3),
          0, 0, { 0 } },
    },
    1,
    {
        SDL_BLE_UUID_INIT(0x81, 0x26, 0x56, 0x52, 0x36, 0x92, 0xae, 0x93, 0xe7, 0x11, 0x27, 0x0f, 0x22, 0x3c, 0x83, 0xb3),
    },
    false,
    SDL_BLE_UUID16_INIT(0x00, 0x00),
    2,
    {
        /* 8126face-3692-ae93-e711-270f223c83b3 and 8126beef-... */
        { 0, SDL_BLE_UUID_INIT(0x81, 0x26, 0xfa, 0xce, 0x36, 0x92, 0xae, 0x93, 0xe7, 0x11, 0x27, 0x0f, 0x22, 0x3c, 0x83, 0xb3),
          SDL_BLE_CHAR_SUBSCRIBE },
        { 0, SDL_BLE_UUID_INIT(0x81, 0x26, 0xbe, 0xef, 0x36, 0x92, 0xae, 0x93, 0xe7, 0x11, 0x27, 0x0f, 0x22, 0x3c, 0x83, 0xb3),
          SDL_BLE_CHAR_SUBSCRIBE | SDL_BLE_CHAR_OPTIONAL },
    },
    SDL_BLE_PAIR_ON_ERROR,
    &SDL_BLEOculusGoModule
};
