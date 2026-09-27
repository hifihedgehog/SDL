/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Every function of src/joystick/windows/SDL_ble_gatt.h, renamed to the fake
   transport of testblegattdriver.c. testblegattdriver.c and
   testblegattswitch2.c include this before the driver source each compiles,
   so both drivers in the test call Fake_BLEGATT_* and never SDL's transport.
   test/ble-driver/CheckTransport.cmake fails the build when either object
   still calls an SDL_BLEGATT_ function, such as one added to SDL_ble_gatt.h
   after this list was written. */

#ifndef testblegattfake_h_
#define testblegattfake_h_

/* The SDL_BLEGATTLink API of the BLE GATT driver */
#define SDL_BLEGATT_Init           Fake_BLEGATT_Init
#define SDL_BLEGATT_Quit           Fake_BLEGATT_Quit
#define SDL_BLEGATT_InitThread     Fake_BLEGATT_InitThread
#define SDL_BLEGATT_QuitThread     Fake_BLEGATT_QuitThread
#define SDL_BLEGATT_AddListener    Fake_BLEGATT_AddListener
#define SDL_BLEGATT_RemoveListener Fake_BLEGATT_RemoveListener
#define SDL_BLEGATT_CheckWatcher   Fake_BLEGATT_CheckWatcher
#define SDL_BLEGATT_Open           Fake_BLEGATT_Open
#define SDL_BLEGATT_Pair           Fake_BLEGATT_Pair
#define SDL_BLEGATT_RemoveBond     Fake_BLEGATT_RemoveBond
#define SDL_BLEGATT_Discover       Fake_BLEGATT_Discover
#define SDL_BLEGATT_Subscribe      Fake_BLEGATT_Subscribe
#define SDL_BLEGATT_Read           Fake_BLEGATT_Read
#define SDL_BLEGATT_Write          Fake_BLEGATT_Write
#define SDL_BLEGATT_Close          Fake_BLEGATT_Close

/* The lower-level helpers of the Switch 2 driver */
#define SDL_BLEGATT_OpenDevice          Fake_BLEGATT_OpenDevice
#define SDL_BLEGATT_FindService         Fake_BLEGATT_FindService
#define SDL_BLEGATT_RequestThroughput   Fake_BLEGATT_RequestThroughput
#define SDL_BLEGATT_FindCharacteristic  Fake_BLEGATT_FindCharacteristic
#define SDL_BLEGATT_WriteCharacteristic Fake_BLEGATT_WriteCharacteristic
#define SDL_BLEGATT_EnableNotifications Fake_BLEGATT_EnableNotifications
#define SDL_BLEGATT_AddValueHandler     Fake_BLEGATT_AddValueHandler
#define SDL_BLEGATT_AddStatusHandler    Fake_BLEGATT_AddStatusHandler

#endif /* testblegattfake_h_ */
