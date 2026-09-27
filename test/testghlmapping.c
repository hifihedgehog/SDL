/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* The gamepad mappings SDL generates for the Guitar Hero Live dongles of
   hifihedgehog/SDL#33 Part 3, from the real gamepad implementation.
   test/xinput-paddles builds it, because that project links a static SDL and
   includes SDL_gamepad.c.

   SDL_Init is never called, so no backend starts and no device opens:
   SDL_GetGamepadMappingForGUID generates a HIDAPI GUID's mapping from the
   GUID and the gamepad type. SDL reads that type from the connected device.
   The HIDAPI GHL driver sets PS3 for the PS3/Wii U dongle and PS4 for the PS4
   dongle, and the GIP driver sets XBOXONE for the Xbox One dongle. This test
   stands in for the device: SDL_gamepad.c's calls to
   SDL_GetGamepadTypeFromGUID come here and get the type the driver sets.

   1. Each dongle's mapping binds only inputs its guitar has: a button below
      SDL_GHL_NUM_BUTTONS, an axis below SDL_GHL_NUM_AXES or hat 0. So the
      PS4 dongle's mapping has no touchpad, and the Xbox One dongle's has no
      Share button, which SDL gives every Xbox One ID outside its list of
      older controllers.
   2. The positive controls: another HIDAPI device typed PS4 gets the
      touchpad as button 11, and one typed XBOXONE gets Share as button 11,
      so the check in 1 can see both. */

#define SDL_MAIN_HANDLED
#define SDL_GetGamepadTypeFromGUID TestGetGamepadTypeFromGUID
#include "../src/joystick/SDL_gamepad.c"
#undef SDL_GetGamepadTypeFromGUID

#include "../src/joystick/SDL_ghl_proto.h"

#include <stdio.h>

/* The real function, whose declaration the rename above took */
extern SDL_GamepadType SDL_GetGamepadTypeFromGUID(SDL_GUID guid, const char *name);

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

typedef struct
{
    const char *label;
    Uint16 vendor;
    Uint16 product;
    const char *name;
    SDL_GamepadType type;
    const char *control_field; /* A control's field that shows the check works */
    SDL_GUID guid;
} TypedDevice;

static TypedDevice devices[] = {
    { "PS3/Wii U dongle", 0x12BA, 0x074B, "Guitar Hero Live Guitar (PS3/Wii U)", SDL_GAMEPAD_TYPE_PS3, NULL, { { 0 } } },
    { "PS4 dongle", 0x1430, 0x07BB, "Guitar Hero Live Guitar (PS4)", SDL_GAMEPAD_TYPE_PS4, NULL, { { 0 } } },
    { "Xbox One dongle", 0x1430, 0x079B, "Guitar Hero Live Guitar (Xbox One)", SDL_GAMEPAD_TYPE_XBOXONE, NULL, { { 0 } } },
    { "PS4-typed control", 0xF00D, 0x0033, "PS4-typed control", SDL_GAMEPAD_TYPE_PS4, "touchpad:b11,", { { 0 } } },
    { "Xbox One-typed control", 0xF00D, 0x0034, "Xbox One-typed control", SDL_GAMEPAD_TYPE_XBOXONE, "misc1:b11,", { { 0 } } },
};

SDL_GamepadType TestGetGamepadTypeFromGUID(SDL_GUID guid, const char *name)
{
    size_t i;

    for (i = 0; i < SDL_arraysize(devices); ++i) {
        if (SDL_memcmp(&guid, &devices[i].guid, sizeof(guid)) == 0) {
            return devices[i].type;
        }
    }
    return SDL_GetGamepadTypeFromGUID(guid, name);
}

/* The fields after the GUID and the name */
static const char *Body(const char *mapping)
{
    const char *comma = mapping ? SDL_strchr(mapping, ',') : NULL;

    comma = comma ? SDL_strchr(comma + 1, ',') : NULL;
    return comma ? comma + 1 : NULL;
}

/* Rule 1 for every field of a mapping body. Fields that name no gamepad
   control, such as crc: and platform:, pass. */
static void CheckGuitarBindings(const TypedDevice *device, const char *body)
{
    const char *field = body;

    while (field && *field) {
        const char *comma = SDL_strchr(field, ',');
        const size_t length = comma ? (size_t)(comma - field) : SDL_strlen(field);
        const char *colon = SDL_strchr(field, ':');
        const char *input;
        char name[32];
        char *end = NULL;
        long index;

        CHECK(colon && colon < field + length && (size_t)(colon - field) < sizeof(name),
              "%s: the malformed field %.*s", device->label, (int)length, field);
        if (!colon || colon >= field + length || (size_t)(colon - field) >= sizeof(name)) {
            break;
        }
        SDL_strlcpy(name, field, (size_t)(colon - field) + 1);
        field = comma ? comma + 1 : NULL;
        if (SDL_GetGamepadButtonFromString(name) == SDL_GAMEPAD_BUTTON_INVALID &&
            SDL_GetGamepadAxisFromString(name) == SDL_GAMEPAD_AXIS_INVALID) {
            continue;
        }

        CHECK(SDL_strcmp(name, "touchpad") != 0, "%s binds the touchpad", device->label);
        input = colon + 1;
        if (*input == '+' || *input == '-') {
            ++input;
        }
        index = SDL_strtol(input + 1, &end, 10);
        if (*input == 'b') {
            CHECK(index >= 0 && index < SDL_GHL_NUM_BUTTONS, "%s has %d buttons and binds %s:b%ld",
                  device->label, SDL_GHL_NUM_BUTTONS, name, index);
        } else if (*input == 'a') {
            CHECK(index >= 0 && index < SDL_GHL_NUM_AXES, "%s has %d axes and binds %s:a%ld",
                  device->label, SDL_GHL_NUM_AXES, name, index);
        } else {
            CHECK(*input == 'h' && index == 0 && end && *end == '.', "%s has hat 0 only and binds %s:%s",
                  device->label, name, input);
        }
    }
}

int main(void)
{
    size_t i;

    setvbuf(stdout, NULL, _IONBF, 0);
    SDL_SetMainReady();

    /* The GUIDs the HIDAPI drivers build, named as the drivers name them */
    for (i = 0; i < SDL_arraysize(devices); ++i) {
        devices[i].guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_USB, devices[i].vendor, devices[i].product, 0,
                                                 NULL, devices[i].name, 'h', 0);
    }

    for (i = 0; i < SDL_arraysize(devices); ++i) {
        char *mapping = SDL_GetGamepadMappingForGUID(devices[i].guid);
        const char *body = Body(mapping);

        printf("%s %04x:%04x: %s\n", devices[i].label, devices[i].vendor, devices[i].product, body ? body : "(none)");
        CHECK(body != NULL, "%s has no mapping", devices[i].label);
        if (devices[i].control_field) {
            /* Rule 2 */
            CHECK(body && SDL_strstr(body, devices[i].control_field) != NULL, "%s lacks %s", devices[i].label,
                  devices[i].control_field);
        } else {
            CheckGuitarBindings(&devices[i], body);
        }
        SDL_free(mapping);
    }

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
