/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* The Rock Band 3 Pro keyboard on the XInput backend, hifihedgehog/SDL#33
   Part 3, through the real SDL_XINPUT_JoystickUpdate of a static SDL.
   test/xinput-paddles builds it beside the paddle tests, which share
   SDL_xinputjoystick.c.

   No XInput DLL loads and no device opens. The hardware backends are off at
   override priority, and SDL's XInput function pointers point at fakes that
   return the state each step sets. A virtual joystick with the keyboard's
   layout carries the Windows backend's joystick data while the update runs,
   so what the update posts lands on the virtual joystick.

   1. A new packet number posts the state.
   2. An unchanged packet number with unchanged trailing bytes posts nothing.
   3. An unchanged packet number with new trailing bytes posts the state: the
      touch strip moves and the pedal connects. No source says whether
      xusb22 counts those six bytes in dwPacketNumber.
   4. Packet number 0 posts nothing.
   5. Without OpenXInput's extended export only the packet number counts, and
      the trailing controls read as absent. */

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"
#include "joystick/SDL_sysjoystick.h"
#include "joystick/SDL_joystick_c.h"
#include "joystick/windows/SDL_windowsjoystick_c.h"
#include "joystick/windows/SDL_xinputjoystick_c.h"
#include "joystick/SDL_rb3pro_proto.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned checks, failures;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    printf("FAIL line=%d expression=%s\n", __LINE__, #expr); } } while (0)
#define REQUIRE(expr) do { ++checks; if (!(expr)) { ++failures; \
    printf("FAIL line=%d expression=%s error=%s\n", __LINE__, #expr, SDL_GetError()); exit(2); } } while (0)

/* What the fake XInput returns */
static XINPUT_STATE fake_state;
static BYTE fake_extra[6];

static DWORD WINAPI FakeGetState(DWORD user, XINPUT_STATE *state)
{
    if (user != 0) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    *state = fake_state;
    return ERROR_SUCCESS;
}

static DWORD WINAPI FakeGetStateExtended(DWORD user, SDL_XINPUT_STATE_EXTENDED_V1 *state)
{
    if (user != 0 || state->cbSize != sizeof(*state)) {
        return ERROR_BAD_ARGUMENTS;
    }
    state->state = fake_state;
    state->extraByteCount = sizeof(fake_extra);
    SDL_memcpy(state->extraBytes, fake_extra, sizeof(fake_extra));
    return ERROR_SUCCESS;
}

/* One SDL_XINPUT_JoystickUpdate with the Windows backend's joystick data in
   place of the virtual joystick's */
static void Update(SDL_Joystick *joystick, struct joystick_hwdata *hwdata)
{
    struct joystick_hwdata *virtual_hwdata;

    SDL_LockJoysticks();
    virtual_hwdata = joystick->hwdata;
    joystick->hwdata = hwdata;
    SDL_XINPUT_JoystickUpdate(joystick);
    joystick->hwdata = virtual_hwdata;
    SDL_UnlockJoysticks();
}

/* What the decoder makes of the fake state, with or without the six bytes */
static SDL_RB3ProOutput Decoded(bool trailing)
{
    SDL_RB3ProXInputState state;
    SDL_RB3ProOutput output;

    SDL_zero(state);
    state.buttons = fake_state.Gamepad.wButtons;
    state.left_trigger = fake_state.Gamepad.bLeftTrigger;
    state.right_trigger = fake_state.Gamepad.bRightTrigger;
    state.thumb_lx = fake_state.Gamepad.sThumbLX;
    state.thumb_ly = fake_state.Gamepad.sThumbLY;
    state.thumb_rx = fake_state.Gamepad.sThumbRX;
    state.thumb_ry = fake_state.Gamepad.sThumbRY;
    state.has_trailing = trailing;
    SDL_memcpy(state.trailing, fake_extra, sizeof(state.trailing));
    REQUIRE(SDL_RB3Pro_DecodeXInput(SDL_RB3PRO_KEYBOARD, &state, &output));
    return output;
}

static Sint16 TouchStrip(SDL_Joystick *joystick)
{
    return SDL_GetJoystickAxis(joystick, SDL_RB3PRO_KEYBOARD_TOUCH_STRIP);
}

static bool PedalConnected(SDL_Joystick *joystick)
{
    return SDL_GetJoystickButton(joystick, SDL_RB3PRO_KEYBOARD_PEDAL_CONNECTED);
}

int main(void)
{
    const char *disabled[] = {
        SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_RAWINPUT, SDL_HINT_JOYSTICK_DIRECTINPUT,
        SDL_HINT_XINPUT_ENABLED, SDL_HINT_JOYSTICK_WGI, SDL_HINT_JOYSTICK_GAMEINPUT,
        SDL_HINT_JOYSTICK_GAMEINPUT_RAW, SDL_HINT_JOYSTICK_BLE, SDL_HINT_JOYSTICK_BLE_SWITCH2,
        SDL_HINT_JOYSTICK_SERIAL_AUTO, SDL_HINT_JOYSTICK_RFCOMM,
        SDL_HINT_JOYSTICK_THREAD, "SDL_JOYSTICK_WINMM", "SDL_JOYSTICK_ROG_CHAKRAM",
        "SDL_JOYSTICK_ICADE"
    };
    /* The serial port and DJI remote hints are lists, so they are set empty */
    const char *emptied[] = { SDL_HINT_JOYSTICK_SERIAL, SDL_HINT_JOYSTICK_DJI_REMOTE_TCP_HOSTS };
    struct joystick_hwdata hwdata;
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID *initial;
    SDL_JoystickID id;
    SDL_Joystick *joystick;
    SDL_RB3ProOutput expected;
    int initial_count = -1;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    SDL_SetMainReady();
    for (i = 0; i < (int)SDL_arraysize(disabled); ++i) {
        REQUIRE(SDL_SetHintWithPriority(disabled[i], "0", SDL_HINT_OVERRIDE));
    }
    for (i = 0; i < (int)SDL_arraysize(emptied); ++i) {
        REQUIRE(SDL_SetHintWithPriority(emptied[i], "", SDL_HINT_OVERRIDE));
    }
    REQUIRE(SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1", SDL_HINT_OVERRIDE));
    REQUIRE(SDL_SetHintWithPriority(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0", SDL_HINT_OVERRIDE));
    REQUIRE(SDL_Init(SDL_INIT_JOYSTICK));
    initial = SDL_GetJoysticks(&initial_count);
    REQUIRE(initial != NULL && initial_count == 0);
    SDL_free(initial);
    /* The XInput backend is off, so no DLL filled these */
    REQUIRE(SDL_XInputGetState == NULL && SDL_XInputGetStateExtended == NULL);

    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
    desc.naxes = SDL_RB3PRO_KEYBOARD_AXES;
    desc.nbuttons = SDL_RB3PRO_KEYBOARD_BUTTONS;
    desc.nhats = 1;
    desc.vendor_id = 0xf00d;
    desc.product_id = 0x0034;
    desc.name = "Rock Band 3 keyboard fixture";
    id = SDL_AttachVirtualJoystick(&desc);
    REQUIRE(id != 0);
    joystick = SDL_OpenJoystick(id);
    REQUIRE(joystick != NULL);

    /* The data SDL_XINPUT_JoystickOpen leaves for a keyboard on user 0 */
    SDL_zero(hwdata);
    hwdata.bXInputDevice = true;
    hwdata.userid = 0;
    hwdata.rb3pro_variant = SDL_RB3Pro_VariantForXInputSubtype(SDL_RB3PRO_XINPUT_SUBTYPE_KEYBOARD);
    REQUIRE(hwdata.rb3pro_variant == SDL_RB3PRO_KEYBOARD);
    SDL_XInputGetState = FakeGetState;
    SDL_XInputGetStateExtended = FakeGetStateExtended;

    /* 1. A new packet number posts the state, touch strip 0x20 */
    SDL_zero(fake_state);
    SDL_zeroa(fake_extra);
    fake_state.dwPacketNumber = 1;
    fake_extra[0] = 0x20;
    Update(joystick, &hwdata);
    expected = Decoded(true);
    CHECK(TouchStrip(joystick) == expected.axes[SDL_RB3PRO_KEYBOARD_TOUCH_STRIP]);
    CHECK(TouchStrip(joystick) != 0);
    CHECK(!PedalConnected(joystick));

    /* 2. The same packet number and the same six bytes: nothing posts, even
       where the fake's twelve bytes moved */
    fake_state.Gamepad.wButtons = 0x1000;
    Update(joystick, &hwdata);
    CHECK(!SDL_GetJoystickButton(joystick, SDL_RB3PRO_BUTTON_SOUTH));
    CHECK(TouchStrip(joystick) == expected.axes[SDL_RB3PRO_KEYBOARD_TOUCH_STRIP]);
    fake_state.Gamepad.wButtons = 0;

    /* 3. The same packet number and new trailing bytes: the touch strip and
       the pedal connection post */
    fake_extra[0] = 0x40;
    fake_extra[5] = 0x01;
    Update(joystick, &hwdata);
    expected = Decoded(true);
    CHECK(TouchStrip(joystick) == expected.axes[SDL_RB3PRO_KEYBOARD_TOUCH_STRIP]);
    CHECK(PedalConnected(joystick));
    /* And once posted, the same bytes post nothing again */
    fake_state.Gamepad.wButtons = 0x1000;
    Update(joystick, &hwdata);
    CHECK(!SDL_GetJoystickButton(joystick, SDL_RB3PRO_BUTTON_SOUTH));
    fake_state.Gamepad.wButtons = 0;

    /* 4. Packet number 0 posts nothing */
    fake_state.dwPacketNumber = 0;
    fake_extra[0] = 0x7F;
    Update(joystick, &hwdata);
    CHECK(TouchStrip(joystick) == expected.axes[SDL_RB3PRO_KEYBOARD_TOUCH_STRIP]);

    /* 5. Without the extended export: the packet number alone gates, and a
       new one posts the trailing controls as absent */
    SDL_XInputGetStateExtended = NULL;
    fake_state.dwPacketNumber = 1;
    fake_extra[0] = 0x10;
    Update(joystick, &hwdata);
    CHECK(TouchStrip(joystick) == expected.axes[SDL_RB3PRO_KEYBOARD_TOUCH_STRIP]);
    fake_state.dwPacketNumber = 2;
    Update(joystick, &hwdata);
    expected = Decoded(false);
    CHECK(TouchStrip(joystick) == expected.axes[SDL_RB3PRO_KEYBOARD_TOUCH_STRIP]);
    CHECK(TouchStrip(joystick) == 0);
    CHECK(!PedalConnected(joystick));

    SDL_XInputGetState = NULL;
    SDL_XInputGetStateExtended = NULL;
    SDL_CloseJoystick(joystick);
    REQUIRE(SDL_DetachVirtualJoystick(id));
    SDL_Quit();

    printf("SUMMARY checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
