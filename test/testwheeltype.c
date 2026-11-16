/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* For hifihedgehog/SDL#48: SDL types the Logitech RS50 as a wheel in all
   three of its modes, 046D:C274 (PlayStation), 046D:C275 (Xbox) and
   046D:C276 (PC), the IDs Logitech's RS50 specification pages give. Each ID
   attaches as a virtual joystick, whose type SDL_GetJoystickTypeForID and
   SDL_GetJoystickType read through the wheel table. The wheels the table
   listed before stay wheels, and the IDs beside the RS50's do not become
   wheels. Every real joystick backend is off, so no device opens. */

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

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

// The type SDL gives a virtual joystick with the IDs, by ID and when open
static void TypesOf(Uint16 vendor, Uint16 product, SDL_JoystickType *by_id, SDL_JoystickType *opened)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID id;
    SDL_Joystick *joystick;

    *by_id = SDL_JOYSTICK_TYPE_UNKNOWN;
    *opened = SDL_JOYSTICK_TYPE_UNKNOWN;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
    desc.vendor_id = vendor;
    desc.product_id = product;
    desc.naxes = 1;
    desc.nbuttons = 1;
    desc.name = "Wheel type test";
    id = SDL_AttachVirtualJoystick(&desc);
    if (!id) {
        CHECK(false, "%04x:%04x did not attach: %s", vendor, product, SDL_GetError());
        return;
    }
    *by_id = SDL_GetJoystickTypeForID(id);
    joystick = SDL_OpenJoystick(id);
    if (joystick) {
        *opened = SDL_GetJoystickType(joystick);
        SDL_CloseJoystick(joystick);
    } else {
        CHECK(false, "%04x:%04x did not open: %s", vendor, product, SDL_GetError());
    }
    SDL_DetachVirtualJoystick(id);
}

int main(int argc, char *argv[])
{
    static const struct
    {
        Uint16 vendor;
        Uint16 product;
        bool wheel;
        const char *name;
    } devices[] = {
        { 0x046d, 0xc274, true, "Logitech RS50 (PlayStation mode)" },
        { 0x046d, 0xc275, true, "Logitech RS50 (Xbox mode)" },
        { 0x046d, 0xc276, true, "Logitech RS50 (PC mode)" },
        { 0x046d, 0xc272, true, "Logitech PRO Racing Wheel for Xbox (PC mode), already listed" },
        { 0x046d, 0xc24f, true, "Logitech G29 (PS3), already listed" },
        { 0x044f, 0xb65e, true, "Thrustmaster T500RS, already listed" },
        { 0x046d, 0xc273, false, "the ID before the RS50's" },
        { 0x046d, 0xc277, false, "the ID after the RS50's" },
        { 0x045e, 0x028e, false, "Xbox 360 Controller" }
    };
    int i;

    (void)argc;
    (void)argv;

    /* Keep real devices out, with every backend the community mapping test
       turns off, the joystick thread and WinMM too */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint("SDL_JOYSTICK_BLE_SWITCH2", "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_BLE, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_SERIAL_AUTO, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RFCOMM, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ICADE, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ASYNC, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ROG_CHAKRAM, "0");
    SDL_SetHint("SDL_JOYSTICK_WINMM", "0");
    /* The serial port and DJI remote hints are lists, so they are set empty */
    SDL_SetHint(SDL_HINT_JOYSTICK_SERIAL, "");
    SDL_SetHint(SDL_HINT_JOYSTICK_DJI_REMOTE_TCP_HOSTS, "");
    if (!SDL_Init(SDL_INIT_JOYSTICK)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    for (i = 0; i < (int)SDL_arraysize(devices); ++i) {
        SDL_JoystickType by_id, opened;

        TypesOf(devices[i].vendor, devices[i].product, &by_id, &opened);
        if (devices[i].wheel) {
            CHECK(by_id == SDL_JOYSTICK_TYPE_WHEEL && opened == SDL_JOYSTICK_TYPE_WHEEL,
                  "%s, %04x:%04x, typed %d by ID and %d open, not a wheel", devices[i].name, devices[i].vendor,
                  devices[i].product, (int)by_id, (int)opened);
        } else {
            CHECK(by_id != SDL_JOYSTICK_TYPE_WHEEL && opened != SDL_JOYSTICK_TYPE_WHEEL,
                  "%s, %04x:%04x, typed as a wheel", devices[i].name, devices[i].vendor, devices[i].product);
        }
    }

    SDL_Quit();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
