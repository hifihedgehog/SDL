/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* The gamepad mappings SDL generates for the Wii Remote extensions of
   hifihedgehog/SDL#33 Part 2, from the static library of this tree with the
   HIDAPI joystick built in. test/community-mappings builds and runs it.

   SDL_Init is never called, so no backend starts and no device opens:
   SDL_GetGamepadMappingForGUID generates a HIDAPI GUID's mapping from the
   GUID alone. Each GUID is built as the Wii driver builds it, the extension
   type in byte 15, for the Wii Remote and the Wii Remote Plus.

   1. No extension's mapping binds an input its joystick lacks: every button,
      axis and hat index is below the extension's counts, so a drum kit,
      turntable or TaTaCon has no D-pad.
   2. None binds GUIDE, and only the TaTaCon, whose faces are the stick
      buttons, binds a stick click.
   3. The tablets get no mapping.
   4. The Wii Remote alone, with a Nunchuk and with the Balance Board keep
      the mappings they had before these extensions.
   5. Each mapping is the extension module's own, SDL_WiiExt_GetMapping,
      which testwiiext.c checks against the decoders. */

#include <SDL3/SDL.h>

#include <stdio.h>

#include "../src/joystick/hidapi/SDL_hidapi_wii_ext_proto.h"

/* Internal function of the static library, declared in src/joystick/SDL_joystick_c.h */
extern SDL_GUID SDL_CreateJoystickGUID(Uint16 bus, Uint16 vendor, Uint16 product, Uint16 version, const char *vendor_name, const char *product_name, Uint8 driver_signature, Uint8 driver_data);

#define HARDWARE_BUS_BLUETOOTH 0x05 /* SDL_HARDWARE_BUS_BLUETOOTH in src/joystick/SDL_sysjoystick.h */
#define VENDOR_NINTENDO        0x057E
#define PRODUCT_WII_REMOTE     0x0306
#define PRODUCT_WII_REMOTE2    0x0330

static int checks;
static int failures;

#define CHECK(condition, ...)                         \
    do {                                              \
        ++checks;                                     \
        if (!(condition)) {                           \
            ++failures;                               \
            printf("FAILED line %d: ", __LINE__);     \
            printf(__VA_ARGS__);                      \
            printf("\n");                             \
        }                                             \
    } while (0)

static SDL_GUID WiiGUID(Uint16 product, int type)
{
    return SDL_CreateJoystickGUID(HARDWARE_BUS_BLUETOOTH, VENDOR_NINTENDO, product, 0, NULL,
                                  SDL_WiiExt_TypeName(type), 'h', (Uint8)type);
}

/* The fields after the GUID and the name */
static const char *Body(const char *mapping)
{
    const char *comma = mapping ? SDL_strchr(mapping, ',') : NULL;

    comma = comma ? SDL_strchr(comma + 1, ',') : NULL;
    return comma ? comma + 1 : NULL;
}

static bool HasField(const char *body, const char *field)
{
    return body && SDL_strstr(body, field) != NULL;
}

/* Rules 1 and 2 for one field, name:input. Fields that name no gamepad
   control, such as crc: and platform:, pass. */
static void CheckField(Uint16 product, int type, const SDL_WiiExtCaps *caps, const char *name, const char *input)
{
    const char *at = input;
    char *end = NULL;
    long index;

    if (SDL_GetGamepadButtonFromString(name) == SDL_GAMEPAD_BUTTON_INVALID &&
        SDL_GetGamepadAxisFromString(name) == SDL_GAMEPAD_AXIS_INVALID) {
        return;
    }
    CHECK(SDL_strcmp(name, "guide") != 0, "%04x type %d binds guide:%s", product, type, input);
    if (SDL_strcmp(name, "leftstick") == 0 || SDL_strcmp(name, "rightstick") == 0) {
        CHECK(type == SDL_WII_EXT_TAIKO, "%04x type %d binds %s:%s", product, type, name, input);
    }

    if (*at == '+' || *at == '-') {
        ++at;
    }
    if ((*at != 'a' && *at != 'b' && *at != 'h') || at[1] < '0' || at[1] > '9') {
        return;
    }
    index = SDL_strtol(at + 1, &end, 10);
    if (*at == 'h') {
        CHECK(index < caps->nhats, "%04x type %d has %d hats and binds %s:%s", product, type, caps->nhats, name, input);
    } else if (*at == 'b') {
        CHECK(index < caps->nbuttons, "%04x type %d has %d buttons and binds %s:%s", product, type, caps->nbuttons, name, input);
    } else {
        CHECK(index < caps->naxes, "%04x type %d has %d axes and binds %s:%s", product, type, caps->naxes, name, input);
    }
}

static void CheckExtension(Uint16 product, int type)
{
    const char *module_mapping = SDL_WiiExt_GetMapping(type);
    SDL_WiiExtCaps caps;
    char *mapping;
    const char *body;
    const char *field;

    CHECK(SDL_WiiExt_GetCaps(type, &caps), "no caps for type %d", type);
    mapping = SDL_GetGamepadMappingForGUID(WiiGUID(product, type));
    body = Body(mapping);
    if (type == SDL_WII_EXT_UDRAW || type == SDL_WII_EXT_DRAWSOME) {
        CHECK(!mapping && !module_mapping, "%04x tablet type %d has the mapping %s", product, type,
              mapping ? mapping : module_mapping);
        SDL_free(mapping);
        return;
    }
    CHECK(body != NULL, "%04x type %d has no mapping", product, type);
    printf("%04x type %d: %s\n", product, type, body ? body : "(none)");
    CHECK(body && module_mapping && SDL_strncmp(body, module_mapping, SDL_strlen(module_mapping)) == 0,
          "%04x type %d does not start with the module's %s", product, type, module_mapping ? module_mapping : "(none)");

    for (field = body; field && *field;) {
        const char *comma = SDL_strchr(field, ',');
        const size_t length = comma ? (size_t)(comma - field) : SDL_strlen(field);
        const char *colon = SDL_strchr(field, ':');
        char name[32], input[32];

        if (colon && colon < field + length && (size_t)(colon - field) < sizeof(name) &&
            length - (size_t)(colon - field) - 1 < sizeof(input)) {
            SDL_strlcpy(name, field, (size_t)(colon - field) + 1);
            SDL_strlcpy(input, colon + 1, length - (size_t)(colon - field));
            CheckField(product, type, &caps, name, input);
        } else {
            CHECK(false, "%04x type %d has the malformed field %.*s", product, type, (int)length, field);
        }
        field = comma ? comma + 1 : NULL;
    }
    SDL_free(mapping);
}

int main(void)
{
    static const Uint16 products[] = { PRODUCT_WII_REMOTE, PRODUCT_WII_REMOTE2 };
    size_t i;
    int type;

    for (i = 0; i < SDL_arraysize(products); ++i) {
        char *mapping;

        for (type = SDL_WII_EXT_GUITAR; type <= SDL_WII_EXT_SHINKANSEN; ++type) {
            CHECK(SDL_WiiExt_IsDecodedType(type), "type %d is not decoded", type);
            CheckExtension(products[i], type);
        }

        /* Rule 4, the positive control: the configurations that existed
           before take the Nintendo branch as they did */
        mapping = SDL_GetGamepadMappingForGUID(WiiGUID(products[i], SDL_WII_EXT_NONE));
        CHECK(HasField(Body(mapping), "dpup:h0.1,") && HasField(Body(mapping), "guide:b5,"),
              "%04x Wii Remote: %s", products[i], mapping ? mapping : "(none)");
        SDL_free(mapping);
        mapping = SDL_GetGamepadMappingForGUID(WiiGUID(products[i], SDL_WII_EXT_NUNCHUK));
        CHECK(HasField(Body(mapping), "leftx:a0,lefty:a1,"), "%04x Nunchuk: %s", products[i], mapping ? mapping : "(none)");
        SDL_free(mapping);
        mapping = SDL_GetGamepadMappingForGUID(WiiGUID(products[i], SDL_WII_EXT_BALANCEBOARD));
        CHECK(HasField(Body(mapping), "leftx:a0,lefty:a1,rightx:a2,righty:a3,") && !HasField(Body(mapping), ":b"),
              "%04x Balance Board: %s", products[i], mapping ? mapping : "(none)");
        SDL_free(mapping);
    }

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
