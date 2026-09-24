/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Tests the community mappings in src/joystick/SDL_gamepad_db_community.h
   against the static library from this tree. test/community-mappings builds
   and runs it.

   testcommunitymappings <SDL_gamepad_db.h>

   1. Every generated row parses: SDL_AddGamepadMapping finds it already in the
      built-in database, and every one of its fields becomes a binding when a
      gamepad opens with it.
   2. No generated vendor and product pair appears in SDL_gamepad_db.h on any
      platform, or in controller_list.h.
   3. A GUID built the way the DirectInput backend builds it, with a nonzero
      version and a device name, resolves to the row.
   4. No HIDAPI driver supports the device, whether or not a hint enables it.
      Known HIDAPI devices are the positive control.
   Regeneration from the pinned file is the other test, run by the generator.
*/

#include <SDL3/SDL.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../src/joystick/controller_type.h"
#include "../src/joystick/controller_list.h"

/* Internal functions of the static library, declared in
   src/joystick/SDL_joystick_c.h and src/joystick/hidapi/SDL_hidapijoystick_c.h. */
extern SDL_GUID SDL_CreateJoystickGUID(Uint16 bus, Uint16 vendor, Uint16 product, Uint16 version, const char *vendor_name, const char *product_name, Uint8 driver_signature, Uint8 driver_data);
extern bool HIDAPI_IsDeviceSupportedByAnyDriver(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name);

#define HARDWARE_BUS_USB 0x03 /* SDL_HARDWARE_BUS_USB in src/joystick/SDL_sysjoystick.h */
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* The rows exactly as SDL_gamepad_db.h compiles them. */
static const char *community_rows[] = {
#include "../src/joystick/SDL_gamepad_db_community.h"
    NULL
};

static int checks;
static int failures;

#define CHECK(condition, ...)                  \
    do {                                       \
        ++checks;                              \
        if (!(condition)) {                    \
            ++failures;                        \
            if (failures <= 40) {              \
                printf("FAILED line %d: ", __LINE__); \
                printf(__VA_ARGS__);           \
                printf("\n");                  \
            }                                  \
        }                                      \
    } while (0)

typedef struct Row
{
    const char *text;
    char guid[33];
    char name[256];
    const char *fields; /* After the name */
    Uint16 vendor;
    Uint16 product;
} Row;

static bool SplitRow(const char *text, Row *row)
{
    const char *name = text + 33;
    const char *comma;
    Uint16 version = 0, crc = 0;

    if (SDL_strlen(text) < 34 || text[32] != ',') {
        return false;
    }
    comma = SDL_strchr(name, ',');
    if (!comma || (size_t)(comma - name) >= sizeof(row->name)) {
        return false;
    }
    row->text = text;
    SDL_memcpy(row->guid, text, 32);
    row->guid[32] = '\0';
    SDL_memcpy(row->name, name, (size_t)(comma - name));
    row->name[comma - name] = '\0';
    row->fields = comma + 1;
    SDL_GetJoystickGUIDInfo(SDL_StringToGUID(row->guid), &row->vendor, &row->product, &version, &crc);
    return row->vendor && row->product && version == 0 && crc == 0;
}

/* Vendor and product pairs of every quoted GUID in a C source, comments skipped. */
static int CollectPairs(const char *text, size_t length, Uint32 *pairs, int capacity)
{
    size_t i = 0;
    int count = 0;

    while (i < length) {
        if (text[i] == '/' && i + 1 < length && text[i + 1] == '/') {
            while (i < length && text[i] != '\n') {
                ++i;
            }
        } else if (text[i] == '/' && i + 1 < length && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < length && !(text[i] == '*' && text[i + 1] == '/')) {
                ++i;
            }
            i += 2;
        } else if (text[i] == '"') {
            const size_t start = ++i;
            while (i < length && text[i] != '"') {
                i += (text[i] == '\\') ? 2 : 1;
            }
            if (i - start >= 33 && text[start + 32] == ',') {
                char guid[33];
                bool hex = true;
                int j;
                for (j = 0; j < 32; ++j) {
                    hex = hex && SDL_isxdigit((unsigned char)text[start + j]);
                }
                if (hex) {
                    Uint16 vendor = 0, product = 0;
                    SDL_memcpy(guid, &text[start], 32);
                    guid[32] = '\0';
                    SDL_GetJoystickGUIDInfo(SDL_StringToGUID(guid), &vendor, &product, NULL, NULL);
                    if (vendor && product && count < capacity) {
                        pairs[count++] = ((Uint32)vendor << 16) | product;
                    }
                }
            }
            ++i;
        } else {
            ++i;
        }
    }
    return count;
}

/* A binding element as SDL's mapping parser accepts it: a gamepad button or
   axis, optionally half, and a joystick axis, button or hat. */
static bool IsBindingField(const char *field, size_t length)
{
    char key[32], value[32];
    const char *colon = SDL_strchr(field, ':');
    const char *key_start = field;
    const char *v;
    size_t key_length, value_length;

    if (!colon || (size_t)(colon - field) >= length) {
        return false;
    }
    key_length = (size_t)(colon - field);
    value_length = length - key_length - 1;
    if (key_length == 0 || key_length >= sizeof(key) || value_length == 0 || value_length >= sizeof(value)) {
        return false;
    }
    if (*key_start == '+' || *key_start == '-') {
        ++key_start;
        --key_length;
    }
    SDL_memcpy(key, key_start, key_length);
    key[key_length] = '\0';
    SDL_memcpy(value, colon + 1, value_length);
    value[value_length] = '\0';

    if (SDL_GetGamepadAxisFromString(key) == SDL_GAMEPAD_AXIS_INVALID &&
        SDL_GetGamepadButtonFromString(key) == SDL_GAMEPAD_BUTTON_INVALID) {
        return false;
    }

    v = value;
    if (*v == '+' || *v == '-') {
        ++v;
    }
    if (v[0] == 'a' && SDL_isdigit((unsigned char)v[1])) {
        v += 1;
        while (SDL_isdigit((unsigned char)*v)) {
            ++v;
        }
        if (*v == '~') {
            ++v;
        }
        return *v == '\0';
    }
    if (value[0] == 'b' && SDL_isdigit((unsigned char)value[1])) {
        v = value + 1;
        while (SDL_isdigit((unsigned char)*v)) {
            ++v;
        }
        return *v == '\0';
    }
    if (value[0] == 'h' && SDL_isdigit((unsigned char)value[1]) && value[2] == '.' && SDL_isdigit((unsigned char)value[3])) {
        v = value + 3;
        while (SDL_isdigit((unsigned char)*v)) {
            ++v;
        }
        return *v == '\0';
    }
    return false;
}

/* SDL_gamepad.c's fields that carry no binding. */
static bool IsSpecialField(const char *field, size_t length)
{
    static const char *prefixes[] = { "crc:", "type:", "face:", "platform:", "hint:", "sdk>=:", "sdk<=:" };
    size_t i;

    for (i = 0; i < ARRAY_COUNT(prefixes); ++i) {
        const size_t prefix_length = SDL_strlen(prefixes[i]);
        if (length >= prefix_length && SDL_strncmp(field, prefixes[i], prefix_length) == 0) {
            return true;
        }
    }
    return false;
}

/* The number of distinct binding fields, or -1 when a field is neither a
   binding nor one of SDL's special fields. */
static int CountBindings(const char *fields)
{
    const char *seen[64];
    size_t seen_length[64];
    int count = 0;
    const char *p = fields;

    while (*p) {
        const char *end = SDL_strchr(p, ',');
        const size_t length = end ? (size_t)(end - p) : SDL_strlen(p);
        int i;
        bool duplicate = false;

        if (length && !IsSpecialField(p, length)) {
            if (!IsBindingField(p, length)) {
                return -1;
            }
            for (i = 0; i < count; ++i) {
                duplicate = duplicate || (seen_length[i] == length && SDL_strncmp(seen[i], p, length) == 0);
            }
            if (!duplicate) {
                if (count == (int)ARRAY_COUNT(seen)) {
                    return -1;
                }
                seen[count] = p;
                seen_length[count] = length;
                ++count;
            }
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return count;
}

static void ClearSDLEnvironment(void)
{
    SDL_Environment *environment = SDL_GetEnvironment();
    char **variables = SDL_GetEnvironmentVariables(environment);
    int i;

    if (!variables) {
        return;
    }
    for (i = 0; variables[i]; ++i) {
        if (SDL_strncmp(variables[i], "SDL_", 4) == 0) {
            char *equals = SDL_strchr(variables[i], '=');
            if (equals) {
                *equals = '\0';
            }
            SDL_UnsetEnvironmentVariable(environment, variables[i]);
        }
    }
    SDL_free(variables);
}

/* 3. The DirectInput backend's GUID for the device resolves to the row.
   SDL_GetGamepadMappingForGUID prints the requested GUID, then the mapping as
   SDL stores it, so the text after the GUID identifies the mapping. */
static void TestDirectInputGUIDs(const Row *rows, int row_count)
{
    int i;

    /* Negative control: a device nothing maps resolves to nothing. */
    {
        char *none = SDL_GetGamepadMappingForGUID(SDL_CreateJoystickGUID(HARDWARE_BUS_USB, 0x1234, 0x5678, 0x0110, "Community Test", "Unmapped Pad", 0, 0));
        CHECK(none == NULL, "an unmapped DirectInput device resolves to %s", none);
        SDL_free(none);
    }

    for (i = 0; i < row_count; ++i) {
        const Row *row = &rows[i];
        const SDL_GUID guid = SDL_CreateJoystickGUID(HARDWARE_BUS_USB, row->vendor, row->product, 0x0110,
                                                     "Community Test", row->name, 0, 0);
        Uint16 vendor = 0, product = 0, version = 0, crc = 0;
        char expected[1024];
        char *own, *mapping;

        SDL_GetJoystickGUIDInfo(guid, &vendor, &product, &version, &crc);
        CHECK(vendor == row->vendor && product == row->product && version == 0x0110 && crc != 0,
              "%s: the DirectInput GUID lacks a version or a name CRC", row->guid);

        /* The row's own GUID finds the built-in mapping. A row with no hint
           field is stored exactly as written. A label hint makes SDL rename
           face buttons, which the parse test covers. */
        own = SDL_GetGamepadMappingForGUID(SDL_StringToGUID(row->guid));
        SDL_snprintf(expected, sizeof(expected), "%s%splatform:%s,", row->text,
                     row->text[SDL_strlen(row->text) - 1] == ',' ? "" : ",", SDL_GetPlatform());
        CHECK(own != NULL, "%s: own GUID finds no mapping: %s", row->guid, SDL_GetError());
        if (own && !SDL_strstr(row->fields, "hint:")) {
            CHECK(SDL_strcmp(own, expected) == 0, "%s: own GUID resolves to %s", row->guid, own);
        }

        mapping = SDL_GetGamepadMappingForGUID(guid);
        CHECK(mapping && own && SDL_strcmp(mapping + 32, own + 32) == 0, "%s %s: DirectInput GUID resolves to %s",
              row->guid, row->name, mapping ? mapping : SDL_GetError());
        SDL_free(mapping);
        SDL_free(own);
    }
}

/* 2. Nothing the fork curates is overridden. */
static void TestNoOverlap(const Row *rows, int row_count, const char *gamepad_db_path)
{
    static Uint32 pairs[8192];
    size_t length = 0;
    char *text = (char *)SDL_LoadFile(gamepad_db_path, &length);
    int pair_count, i, j;
    size_t k;

    CHECK(text != NULL, "couldn't read %s", gamepad_db_path);
    if (!text) {
        return;
    }
    pair_count = CollectPairs(text, length, pairs, (int)ARRAY_COUNT(pairs));
    CHECK(pair_count > 500 && pair_count < (int)ARRAY_COUNT(pairs), "%d pairs read from %s", pair_count, gamepad_db_path);

    /* Positive controls: a pair SDL_gamepad_db.h curates and one controller_list.h lists. */
    {
        bool curated = false, listed = false;
        for (j = 0; j < pair_count; ++j) {
            curated = curated || pairs[j] == 0x2DC83019; /* 8BitDo 64 Bluetooth Controller */
        }
        for (k = 0; k < ARRAY_COUNT(arrControllers); ++k) {
            listed = listed || arrControllers[k].m_unDeviceID == 0x045E028E; /* Xbox 360 Controller */
        }
        CHECK(curated, "the scan of %s misses 2DC8:3019", gamepad_db_path);
        CHECK(listed, "controller_list.h misses 045E:028E");
    }
    for (i = 0; i < row_count; ++i) {
        const Uint32 pair = ((Uint32)rows[i].vendor << 16) | rows[i].product;
        bool found = false;
        for (j = 0; j < pair_count; ++j) {
            found = found || pairs[j] == pair;
        }
        CHECK(!found, "%s %s: SDL_gamepad_db.h already maps %04X:%04X", rows[i].guid, rows[i].name, rows[i].vendor, rows[i].product);

        found = false;
        for (k = 0; k < ARRAY_COUNT(arrControllers); ++k) {
            found = found || arrControllers[k].m_unDeviceID == pair;
        }
        CHECK(!found, "%s %s: controller_list.h lists %04X:%04X", rows[i].guid, rows[i].name, rows[i].vendor, rows[i].product);
    }
    SDL_free(text);
}

/* 4. No HIDAPI driver reads the device. */
static void TestNoHIDAPIDriver(const Row *rows, int row_count)
{
    static const struct
    {
        Uint16 vendor;
        Uint16 product;
        const char *name;
    } controls[] = {
        { 0x054C, 0x05C4, "Wireless Controller" },   /* DualShock 4, PS4 driver */
        { 0x054C, 0x0CE6, "DualSense Wireless Controller" }, /* PS5 driver */
        { 0x057E, 0x2009, "Pro Controller" },        /* Switch driver */
        { 0x057E, 0x2073, "Switch 2 GameCube Controller" }, /* Switch 2 driver, built only with libusb */
        { 0x057E, 0x2069, "Switch 2 Pro Controller" }, /* Switch 2 driver */
        { 0x057E, 0x0337, "WUP-028" },               /* GameCube adapter driver */
        { 0x045E, 0x02A0, "Xbox 360 Big Button IR" }, /* Big Button driver */
        { 0x28DE, 0x1102, "Steam Controller" },      /* Steam driver */
    };
    size_t k;
    int i;

    for (k = 0; k < ARRAY_COUNT(controls); ++k) {
        CHECK(HIDAPI_IsDeviceSupportedByAnyDriver(controls[k].vendor, controls[k].product, 0, controls[k].name),
              "positive control %04X:%04X %s is not supported", controls[k].vendor, controls[k].product, controls[k].name);
    }
    for (i = 0; i < row_count; ++i) {
        CHECK(!HIDAPI_IsDeviceSupportedByAnyDriver(rows[i].vendor, rows[i].product, 0, rows[i].name),
              "%s %s: a HIDAPI driver supports %04X:%04X", rows[i].guid, rows[i].name, rows[i].vendor, rows[i].product);
    }
}

/* 1. Every row parses, and every field binds. */
static void TestRowsParse(const Row *rows, int row_count)
{
    int i;

    for (i = 0; i < row_count; ++i) {
        const Row *row = &rows[i];
        const int expected = CountBindings(row->fields);
        SDL_VirtualJoystickDesc desc;
        SDL_JoystickID id;
        char guid_string[33];
        char mapping[1024];
        SDL_Gamepad *gamepad;

        /* The built-in database already holds this exact GUID, with this
           content: adding the row again updates it and changes nothing. */
        {
            const SDL_GUID guid = SDL_StringToGUID(row->guid);
            char *before = SDL_GetGamepadMappingForGUID(guid);
            char *after;

            CHECK(SDL_AddGamepadMapping(row->text) == 0, "%s %s: SDL_AddGamepadMapping did not find it built in: %s",
                  row->guid, row->name, SDL_GetError());
            after = SDL_GetGamepadMappingForGUID(guid);
            CHECK(before && after && SDL_strcmp(before, after) == 0, "%s %s: the built-in mapping %s differs from the row, stored as %s",
                  row->guid, row->name, before ? before : "(none)", after ? after : "(none)");
            SDL_free(before);
            SDL_free(after);
        }

        CHECK(expected > 0, "%s %s: a field is not a binding: %s", row->guid, row->name, row->fields);
        if (expected <= 0) {
            continue;
        }

        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = row->vendor;
        desc.product_id = row->product;
        desc.naxes = 16;
        desc.nbuttons = 64;
        desc.nhats = 4;
        desc.name = row->name;
        id = SDL_AttachVirtualJoystick(&desc);
        CHECK(id != 0, "%s: couldn't attach a virtual joystick: %s", row->guid, SDL_GetError());
        if (!id) {
            continue;
        }

        SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid_string, sizeof(guid_string));
        SDL_snprintf(mapping, sizeof(mapping), "%s%s", guid_string, row->text + 32);
        CHECK(SDL_AddGamepadMapping(mapping) == 1, "%s: couldn't add the mapping for the virtual joystick: %s", row->guid, SDL_GetError());

        gamepad = SDL_OpenGamepad(id);
        CHECK(gamepad != NULL, "%s: couldn't open the virtual gamepad: %s", row->guid, SDL_GetError());
        if (gamepad) {
            int count = 0;
            SDL_GamepadBinding **bindings = SDL_GetGamepadBindings(gamepad, &count);
            CHECK(count == expected, "%s %s: %d bindings from %d fields: %s", row->guid, row->name, count, expected, row->fields);
            SDL_free(bindings);
            SDL_CloseGamepad(gamepad);
        }
        SDL_DetachVirtualJoystick(id);
    }
}

int main(int argc, char *argv[])
{
    static Row rows[ARRAY_COUNT(community_rows)];
    int row_count = 0;
    size_t i;

    if (argc != 2) {
        printf("Usage: %s <SDL_gamepad_db.h>\n", argv[0]);
        return 2;
    }

    ClearSDLEnvironment();
    /* The mapping database loads without any backend. Keep real devices out. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint("SDL_JOYSTICK_BLE_SWITCH2", "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    for (i = 0; community_rows[i]; ++i) {
        const bool split = SplitRow(community_rows[i], &rows[row_count]);
        CHECK(split, "row %u is not a USB-form Windows row: %s", (unsigned int)i, community_rows[i]);
        if (split) {
            ++row_count;
        }
    }
    CHECK(row_count > 0, "no generated rows");
    for (i = 1; i < (size_t)row_count; ++i) {
        CHECK(SDL_strcmp(rows[i - 1].text, rows[i].text) < 0, "rows out of order at %s", rows[i].guid);
    }
    printf("%d generated rows\n", row_count);

    /* The lookups run first, before the parse test adds any mapping. */
    TestDirectInputGUIDs(rows, row_count);
    TestNoOverlap(rows, row_count, argv[1]);
    TestNoHIDAPIDriver(rows, row_count);
    TestRowsParse(rows, row_count);

    SDL_Quit();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
