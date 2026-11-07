/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_DINPUT_HapticMaybeAddDevice, SDL's DirectInput haptic listing,
   against a scripted DirectInput, for hifihedgehog/SDL#47. Where the broken
   EZFRD64.DLL is installed and its loader patch is not in place, no device
   is created, so no capability query can load the module, as the joystick
   open skips its query. The file includes SDL_dinputhaptic.c after naming a
   fake for WIN_HasBrokenEZFRD64DLL, and sets the file's DirectInput object
   to a fake whose CreateDevice counts its calls and fails. The haptic
   subsystem never starts and no device opens. */

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"

bool Fake_HasBrokenEZFRD64DLL(void);
#define WIN_HasBrokenEZFRD64DLL Fake_HasBrokenEZFRD64DLL
#include "../src/haptic/windows/SDL_dinputhaptic.c"
#undef WIN_HasBrokenEZFRD64DLL

#include <stdio.h>

static int checks;
static int failures;

#define CHECK(condition, ...)                     \
    do {                                          \
        ++checks;                                 \
        if (!(condition)) {                       \
            ++failures;                           \
            printf("FAILED line %d: ", __LINE__); \
            printf(__VA_ARGS__);                  \
            printf("\n");                         \
        }                                         \
    } while (0)

static bool broken;
static int broken_calls;
static int create_calls;

bool Fake_HasBrokenEZFRD64DLL(void)
{
    ++broken_calls;
    return broken;
}

static HRESULT STDMETHODCALLTYPE Fake_CreateDevice(IDirectInput8 *self, REFGUID guid, LPDIRECTINPUTDEVICE8 *device, LPUNKNOWN outer)
{
    (void)self;
    (void)guid;
    (void)outer;
    ++create_calls;
    if (device) {
        *device = NULL;
    }
    return E_FAIL;
}

int main(int argc, char *argv[])
{
    static IDirectInput8Vtbl vtbl;
    static IDirectInput8 fake;
    DIDEVICEINSTANCE instance;

    (void)argc;
    (void)argv;
    vtbl.CreateDevice = Fake_CreateDevice;
    fake.lpVtbl = &vtbl;
    SDL_zero(instance);

    // Without DirectInput nothing is asked
    dinput = NULL;
    CHECK(!SDL_DINPUT_HapticMaybeAddDevice(&instance) && broken_calls == 0 && create_calls == 0,
          "without DirectInput: %d answers asked, %d devices created", broken_calls, create_calls);

    // Positive control: without the broken module the device is created, which the fake refuses
    dinput = &fake;
    broken = false;
    CHECK(!SDL_DINPUT_HapticMaybeAddDevice(&instance) && broken_calls == 1 && create_calls == 1,
          "without the broken module: %d answers asked, %d devices created", broken_calls, create_calls);

    // With it, no device is created
    broken = true;
    CHECK(!SDL_DINPUT_HapticMaybeAddDevice(&instance) && broken_calls == 2 && create_calls == 1,
          "with the broken module: %d answers asked, %d devices created", broken_calls, create_calls);
    dinput = NULL;

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
