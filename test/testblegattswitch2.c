/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* The Switch 2 driver, SDL_BLE_JoystickDriver, for testblegattdriver: the
   tree's driver with every transport function renamed to the fake transport
   of testblegattdriver.c, as that file compiles the BLE GATT driver. The
   driver table in the static SDL binds to this copy, so the Switch 2 driver
   in the test never reaches SDL's transport or a radio (see
   testblegattdriver.c). Scenario D turns it on. */

#include "SDL_internal.h"

/* Every transport function, renamed to the fake */
#include "testblegattfake.h"

/* Always the tree's driver, by its path from this file */
#include "../src/joystick/windows/SDL_ble_switch2joystick.c"
