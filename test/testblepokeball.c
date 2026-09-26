/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/ble/SDL_ble_pokeball_proto.c, the Poke Ball
   Plus of hifihedgehog/SDL#33 Part 12. Blocks named "Test N" follow the
   part's Poke Ball Plus tests. The other blocks cover the decisions those
   tests leave open. No source publishes a capture as text, so the reports
   are built from the part's layout. Values reach the module through the
   session as exact-size heap copies (testbleharness.h). */

#include "testbleharness.h"
#include "SDL_ble_pokeball_proto.h"

#define REPORT_SIZE SDL_POKEBALL_REPORT_SIZE
#define INPUT       SDL_POKEBALL_INPUT
#define BATTERY     SDL_POKEBALL_BATTERY
#define EAST_BIT    (1u << SDL_BLE_BUTTON_EAST)
#define SOUTH_BIT   (1u << SDL_BLE_BUTTON_SOUTH)
#define ADDRESS     0xA1B2C3D4E5F6ULL

typedef void (*TestFunction)(void);

/* Runs one block and prints its own check count */
static void Run(const char *label, TestFunction test)
{
    const int checks = bh_checks;
    const int failures = bh_failures;

    test();
    printf("%s: %d checks, %d failures\n", label, bh_checks - checks, bh_failures - failures);
}

/* A report as the part lays it out: the counter, the buttons, X across bytes
   2 and 3, Y, then twelve motion bytes at 00 */
static void Report(uint8_t *out, uint8_t counter, uint8_t buttons, uint8_t b2, uint8_t b3, uint8_t y)
{
    memset(out, 0, REPORT_SIZE);
    out[0] = counter;
    out[1] = buttons;
    out[2] = b2;
    out[3] = b3;
    out[4] = y;
}

/* A report at rest with X set to x: the low nibble of X is the high nibble
   of byte 2 and the high nibble of X the low nibble of byte 3 */
static void ReportX(uint8_t *out, int x, uint8_t buttons)
{
    Report(out, 0, buttons, (uint8_t)((x & 0x0F) << 4), (uint8_t)((x >> 4) & 0x0F), 0x6C);
}

/* X as the Windows tool and the sniffer read it (PokeballController.cs:159-161,
   pbp_sniff.py:22) */
static int RawX(const uint8_t *report)
{
    return ((report[3] & 0x0F) << 4) | (report[2] >> 4);
}

/* The part's mapping: (value - center) x 32767 / span, truncated toward zero
   and clamped to -32767..32767 */
static int16_t Mapped(int value, int center, int span)
{
    long result = (long)(value - center) * 32767 / span;

    if (result < -32767) {
        result = -32767;
    } else if (result > 32767) {
        result = 32767;
    }
    return (int16_t)result;
}

static void Input(BH_Harness *h, const uint8_t *report, size_t length)
{
    BH_Value(h, INPUT, report, length, false);
}

/* A session through its start: connect, discovery, both subscriptions and a
   battery read that fails */
static BH_Harness *Started(bool pairing, bool bonded)
{
    BH_Harness *h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, pairing);

    BH_Start(h, bonded);
    return h;
}

/* A published joystick: started, then one report at rest */
static BH_Harness *Up(void)
{
    BH_Harness *h = Started(false, false);
    uint8_t report[REPORT_SIZE];

    Report(report, 0x00, 0x00, 0x00, 0x07, 0x6C);
    Input(h, report, sizeof(report));
    return h;
}

static bool IsSubscribe(const BH_Harness *h, int index, int characteristic, int cccd)
{
    const SDL_BLEAction *action;

    if (index < 0 || index >= h->nactions) {
        return false;
    }
    action = &h->actions[index].action;
    return action->kind == SDL_BLE_ACTION_SUBSCRIBE && action->characteristic == characteristic && action->cccd == cccd;
}

static bool IsAction(const BH_Harness *h, int index, int kind)
{
    return index >= 0 && index < h->nactions && h->actions[index].action.kind == kind;
}

static bool AtRest(const SDL_BLEControls *controls)
{
    int i;

    for (i = 0; i < SDL_BLE_MAX_AXES; ++i) {
        if (controls->axes[i] != 0) {
            return false;
        }
    }
    return controls->buttons == 0 && !controls->finger;
}

static void NameRecord(SDL_BLEAdvertisement *ad, const char *name, uint8_t kind)
{
    memset(ad, 0, sizeof(*ad));
    ad->address = ADDRESS;
    ad->kind = kind;
    if (name) {
        ad->has_name = true;
        memcpy(ad->name, name, strlen(name) + 1);
    }
}

static int Match(const SDL_BLEAdvertisement *ad, const bool *enabled)
{
    uint8_t variant = 0xAA;
    int family = SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, enabled, ad, &variant);

    if (family >= 0 && variant != 0) {
        return -2; /* a name match never carries a variant */
    }
    return family;
}

/* The family table and the identity */
static void TestTable(void)
{
    const SDL_BLEFamily *family = &SDL_BLEPokeballFamily;
    SDL_PokeballState state;
    uint8_t uuid[16];
    uint64_t deadline = 0;
    SDL_BLEUUID battery_service = SDL_BLE_UUID16(0x180F);
    SDL_BLEUUID battery_level = SDL_BLE_UUID16(0x2A19);

    BH_CHECK(SDL_BLE_Families[SDL_BLE_FAMILY_POKEBALL] == family, "Poke Ball is not the first family");
    BH_CHECK(family->module == &SDL_BLEPokeballModule, "module");
    BH_CHECK(SDL_BLEPokeballModule.state_size == sizeof(SDL_PokeballState), "state size");
    BH_CHECK(family->pairing == SDL_BLE_PAIR_NOT_NEEDED, "pairing rule %d", family->pairing);

    /* Found by its exact local name only (PokeballVigemDriver.cs:18, :59) */
    BH_CHECK(family->nkeys == 1, "keys %d", family->nkeys);
    BH_CHECK(family->keys[0].kind == SDL_BLE_KEY_NAME_EQUALS && !family->keys[0].corroborating,
             "key kind %d", family->keys[0].kind);
    BH_CHECK(family->keys[0].name && strcmp(family->keys[0].name, "Pokemon PBP") == 0, "key name");

    /* The vendor service and characteristic (PokeballController.cs:17-18) and
       the battery level (:19-20) */
    BH_CHECK(family->nservices == 2 && !family->has_alternate, "services %d", family->nservices);
    BH_CHECK(BH_Hex("6675e16c-f36d-4567-bb55-6b51e27a23e5", uuid, sizeof(uuid)) == 16 &&
                 memcmp(family->services[0].bytes, uuid, 16) == 0,
             "vendor service");
    BH_CHECK(SDL_BLE_UUIDEqual(&family->services[1], &battery_service), "battery service");
    BH_CHECK(family->ncharacteristics == 2, "characteristics %d", family->ncharacteristics);
    BH_CHECK(BH_Hex("6675e16c-f36d-4567-bb55-6b51e27a23e6", uuid, sizeof(uuid)) == 16 &&
                 memcmp(family->characteristics[INPUT].uuid.bytes, uuid, 16) == 0,
             "input characteristic");
    BH_CHECK(family->characteristics[INPUT].service == 0 && family->characteristics[INPUT].flags == SDL_BLE_CHAR_SUBSCRIBE,
             "input flags %02X", family->characteristics[INPUT].flags);
    BH_CHECK(SDL_BLE_UUIDEqual(&family->characteristics[BATTERY].uuid, &battery_level) &&
                 family->characteristics[BATTERY].service == 1,
             "battery characteristic");
    BH_CHECK(family->characteristics[BATTERY].flags == (SDL_BLE_CHAR_SUBSCRIBE | SDL_BLE_CHAR_READ | SDL_BLE_CHAR_OPTIONAL),
             "battery flags %02X", family->characteristics[BATTERY].flags);

    /* The identity of the SDL mapping */
    memset(&state, 0x5A, sizeof(state));
    SDL_BLEPokeballModule.Reset(&state, NULL, NULL);
    BH_CHECK(strcmp(state.base.identity.name, "Poke Ball Plus") == 0, "name %s", state.base.identity.name);
    BH_CHECK(state.base.identity.type == SDL_BLE_TYPE_GAMEPAD && state.base.identity.gamepad, "type");
    BH_CHECK(state.base.identity.vendor == 0 && state.base.identity.product == 0, "no VID or PID in any source");
    BH_CHECK(state.base.identity.buttons == (EAST_BIT | SOUTH_BIT), "buttons %08X", state.base.identity.buttons);
    BH_CHECK(state.base.identity.axes == ((1u << SDL_BLE_AXIS_LEFTX) | (1u << SDL_BLE_AXIS_LEFTY)),
             "axes %02X", state.base.identity.axes);
    BH_CHECK(!state.base.identity.touchpad, "no touchpad");
    BH_CHECK(state.base.identity.accel_rate == 0.0f && state.base.identity.gyro_rate == 0.0f,
             "no sensor until the motion layout is confirmed");

    /* Rest after the reset, whatever the memory held before */
    BH_CHECK(AtRest(&state.base.controls) && state.base.controls.battery == -1, "rest");
    BH_CHECK(!state.base.ready && !state.base.failed && !state.base.resubscribe, "flags");
    BH_CHECK(state.base.write_count == 0 && state.base.changes == 0, "queue");

    /* No start-up write, no timer, no closing write */
    SDL_BLEPokeballModule.Start(&state, 0);
    BH_CHECK(state.base.write_count == 0 && !state.base.ready, "start");
    BH_CHECK(!SDL_BLEPokeballModule.GetDeadline(&state, &deadline), "a deadline");
    SDL_BLEPokeballModule.Tick(&state, 1000000);
    SDL_BLEPokeballModule.WriteDone(&state, true, 1000000);
    SDL_BLEPokeballModule.Close(&state, 1000000);
    BH_CHECK(state.base.write_count == 0 && AtRest(&state.base.controls), "tick, write done and close");

    /* A module reset without a sink decodes a value of the part's 17 bytes
       without calling one */
    {
        uint8_t report[32];

        memset(report, 0, sizeof(report));
        Report(report, 0x00, 0x01, 0x00, 0x07, 0x6C);
        SDL_BLEPokeballModule.Value(&state, INPUT, report, 17, 0);
        BH_CHECK(state.base.controls.buttons == EAST_BIT && state.base.changes == 1 && state.base.ready,
                 "a 17-byte report without a sink: buttons %08X, %u changes", state.base.controls.buttons,
                 (unsigned int)state.base.changes);
    }
}

/* Test 1: the rest report 00 00 00 07 6C and twelve 00 bytes: X 112, Y 108,
   the stick at (0, 0), no button */
static void Test1_Rest(void)
{
    BH_Harness *h = Started(false, false);
    uint8_t report[REPORT_SIZE];

    BH_CHECK(BH_Hex("00 00 00 07 6C 00 00 00 00 00 00 00 00 00 00 00 00", report, sizeof(report)) == 17, "hex");
    BH_CHECK(RawX(report) == 112 && report[4] == 108, "raw X %d Y %d", RawX(report), report[4]);
    Input(h, report, sizeof(report));
    BH_CHECK(BH_Axis(h, SDL_BLE_AXIS_LEFTX) == 0 && BH_Axis(h, SDL_BLE_AXIS_LEFTY) == 0,
             "stick (%d, %d)", BH_Axis(h, SDL_BLE_AXIS_LEFTX), BH_Axis(h, SDL_BLE_AXIS_LEFTY));
    BH_CHECK(BH_Controls(h)->buttons == 0, "buttons %08X", BH_Controls(h)->buttons);
    /* The report equals the rest the module starts in, so nothing is emitted,
       and it is the first report after the start, so it publishes */
    BH_CHECK(h->nsnapshots == 0, "snapshots %d", h->nsnapshots);
    BH_CHECK(h->published == 1 && BH_LastKind(h) == SDL_BLE_ACTION_PUBLISH, "not published");
    BH_CHECK(h->nsamples == 0, "samples %d", h->nsamples);
    BH_Destroy(h);
}

/* Test 2: byte 1 01 is EAST only, 02 SOUTH only, 03 both, FC neither. Bit 0
   is the top button, B, and bit 1 the stick press, A
   (pokeball_mouse_working.py:113, :120, find-a-cig app/js/ball.js:18-19,
   macOS main.py:19-23, the Reddit post's "The Buttons"). */
static void Test2_Buttons(void)
{
    static const struct
    {
        uint8_t byte1;
        uint32_t buttons;
    } cases[] = {
        { 0x01, EAST_BIT }, { 0x02, SOUTH_BIT }, { 0x03, EAST_BIT | SOUTH_BIT }, { 0xFC, 0 },
        /* Every bit alone: bits 2-7 have no meaning in any source */
        { 0x04, 0 }, { 0x08, 0 }, { 0x10, 0 }, { 0x20, 0 }, { 0x40, 0 }, { 0x80, 0 },
        /* The two known bits among the unknown ones */
        { 0xFF, EAST_BIT | SOUTH_BIT }, { 0xFD, EAST_BIT }, { 0xFE, SOUTH_BIT }, { 0x00, 0 },
    };
    BH_Harness *h = Up();
    uint8_t report[REPORT_SIZE];
    int i;

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        Report(report, (uint8_t)i, cases[i].byte1, 0x00, 0x07, 0x6C);
        Input(h, report, sizeof(report));
        BH_CHECK(BH_Controls(h)->buttons == cases[i].buttons, "byte 1 %02X: buttons %08X", cases[i].byte1,
                 BH_Controls(h)->buttons);
        BH_CHECK(BH_Axis(h, SDL_BLE_AXIS_LEFTX) == 0 && BH_Axis(h, SDL_BLE_AXIS_LEFTY) == 0, "byte 1 %02X moved the stick",
                 cases[i].byte1);
    }
    /* Each change emitted one snapshot carrying it */
    Report(report, 0, 0x00, 0x00, 0x07, 0x6C);
    Input(h, report, sizeof(report));
    i = h->nsnapshots;
    Report(report, 0, 0x01, 0x00, 0x07, 0x6C);
    Input(h, report, sizeof(report));
    BH_CHECK(h->nsnapshots == i + 1 && h->snapshots[i].controls.buttons == EAST_BIT, "press snapshot");
    Report(report, 0, 0x00, 0x00, 0x07, 0x6C);
    Input(h, report, sizeof(report));
    BH_CHECK(h->nsnapshots == i + 2 && h->snapshots[i + 1].controls.buttons == 0, "release snapshot");
    BH_Destroy(h);
}

/* Test 3: X from bytes 2 and 3. 00 02 is 32 and -32767, 00 0C is 192 and
   32767, D0 07 is 125 and 5324, 00 0D (208) and 00 01 (16) clamp. */
static void Test3_X(void)
{
    static const struct
    {
        uint8_t b2, b3;
        int raw;
        int16_t axis;
    } cases[] = {
        { 0x00, 0x02, 32, -32767 },
        { 0x00, 0x0C, 192, 32767 },
        { 0xD0, 0x07, 125, 5324 },
        { 0x00, 0x0D, 208, 32767 },
        { 0x00, 0x01, 16, -32767 },
        /* The ends of the byte and one step either side of the center,
           truncated toward zero */
        { 0x00, 0x00, 0, -32767 },
        { 0xF0, 0x0F, 255, 32767 },
        { 0x10, 0x07, 113, 409 },
        { 0xF0, 0x06, 111, -409 },
        { 0x00, 0x07, 112, 0 },
    };
    BH_Harness *h = Up();
    uint8_t report[REPORT_SIZE];
    int16_t previous = -32767;
    int i, x;

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        Report(report, 0, 0x00, cases[i].b2, cases[i].b3, 0x6C);
        BH_CHECK(RawX(report) == cases[i].raw, "bytes %02X %02X: raw X %d", cases[i].b2, cases[i].b3, RawX(report));
        Input(h, report, sizeof(report));
        BH_CHECK(BH_Axis(h, SDL_BLE_AXIS_LEFTX) == cases[i].axis, "X %d: axis %d, want %d", cases[i].raw,
                 BH_Axis(h, SDL_BLE_AXIS_LEFTX), cases[i].axis);
        BH_CHECK(BH_Axis(h, SDL_BLE_AXIS_LEFTY) == 0 && BH_Controls(h)->buttons == 0, "X %d moved another control",
                 cases[i].raw);
    }

    /* Every X: the part's formula, rising with X, symmetric about 112 */
    for (x = 0; x < 256; ++x) {
        int16_t axis;

        ReportX(report, x, 0x00);
        BH_CHECK(RawX(report) == x, "ReportX %d", x);
        Input(h, report, sizeof(report));
        axis = BH_Axis(h, SDL_BLE_AXIS_LEFTX);
        BH_CHECK(axis == Mapped(x, 112, 80), "X %d: axis %d", x, axis);
        BH_CHECK(axis >= previous, "X %d: axis %d below %d", x, axis, previous);
        BH_CHECK(x > 32 || axis == -32767, "X %d not at the low end", x);
        BH_CHECK(x < 192 || axis == 32767, "X %d not at the high end", x);
        if (x >= 32 && x <= 192) {
            BH_CHECK(Mapped(x, 112, 80) == -Mapped(224 - x, 112, 80), "X %d not symmetric", x);
        }
        previous = axis;
    }
    BH_Destroy(h);
}

/* Test 4: Y is byte 4. 24 (36) is -32767 and B4 (180) 32767. Y grows
   downward, so the low end is up (PokeballController.cs:166-170). */
static void Test4_Y(void)
{
    static const struct
    {
        uint8_t y;
        int16_t axis;
    } cases[] = {
        { 0x24, -32767 }, { 0xB4, 32767 },
        { 0x00, -32767 }, { 0xFF, 32767 }, { 0x6D, 455 }, { 0x6B, -455 }, { 0x6C, 0 }, { 0x23, -32767 }, { 0xB5, 32767 },
    };
    BH_Harness *h = Up();
    uint8_t report[REPORT_SIZE];
    int16_t previous = -32767;
    int i, y;

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        Report(report, 0, 0x00, 0x00, 0x07, cases[i].y);
        Input(h, report, sizeof(report));
        BH_CHECK(BH_Axis(h, SDL_BLE_AXIS_LEFTY) == cases[i].axis, "Y %02X: axis %d, want %d", cases[i].y,
                 BH_Axis(h, SDL_BLE_AXIS_LEFTY), cases[i].axis);
        BH_CHECK(BH_Axis(h, SDL_BLE_AXIS_LEFTX) == 0 && BH_Controls(h)->buttons == 0, "Y %02X moved another control",
                 cases[i].y);
    }
    for (y = 0; y < 256; ++y) {
        int16_t axis;

        Report(report, 0, 0x00, 0x00, 0x07, (uint8_t)y);
        Input(h, report, sizeof(report));
        axis = BH_Axis(h, SDL_BLE_AXIS_LEFTY);
        BH_CHECK(axis == Mapped(y, 108, 72), "Y %d: axis %d", y, axis);
        BH_CHECK(axis >= previous, "Y %d: axis %d below %d", y, axis, previous);
        if (y >= 36 && y <= 180) {
            BH_CHECK(Mapped(y, 108, 72) == -Mapped(216 - y, 108, 72), "Y %d not symmetric", y);
        }
        previous = axis;
    }
    BH_Destroy(h);
}

/* Test 5: bytes 2-3 0F F7 leave X at 112. The low nibble of byte 2 and the
   high nibble of byte 3 are not decoded. */
static void Test5_Nibbles(void)
{
    BH_Harness *h = Up();
    uint8_t report[REPORT_SIZE];
    int low, high;

    /* From a pushed stick, so the return to 112 shows */
    Report(report, 0, 0x00, 0x00, 0x0C, 0x6C);
    Input(h, report, sizeof(report));
    BH_CHECK(BH_Axis(h, SDL_BLE_AXIS_LEFTX) == 32767, "positive control %d", BH_Axis(h, SDL_BLE_AXIS_LEFTX));
    Report(report, 0, 0x00, 0x0F, 0xF7, 0x6C);
    BH_CHECK(RawX(report) == 112, "raw X %d", RawX(report));
    Input(h, report, sizeof(report));
    BH_CHECK(BH_Axis(h, SDL_BLE_AXIS_LEFTX) == 0, "X axis %d", BH_Axis(h, SDL_BLE_AXIS_LEFTX));

    /* Every value of the two undecoded nibbles leaves X where it is */
    for (low = 0; low < 16; ++low) {
        for (high = 0; high < 16; ++high) {
            Report(report, 0, 0x00, (uint8_t)(0xD0 | low), (uint8_t)((high << 4) | 0x07), 0x6C);
            Input(h, report, sizeof(report));
            BH_CHECK(BH_Axis(h, SDL_BLE_AXIS_LEFTX) == 5324, "nibbles %X %X: X axis %d", low, high,
                     BH_Axis(h, SDL_BLE_AXIS_LEFTX));
        }
    }
    BH_Destroy(h);
}

/* Test 6: a change in byte 0 alone, or in bytes 5-16 alone, emits nothing */
static void Test6_Ignored(void)
{
    static const uint8_t values[] = { 0x01, 0x7F, 0x80, 0xFF };
    BH_Harness *h = Up();
    uint8_t base[REPORT_SIZE], report[REPORT_SIZE];
    const SDL_BLEControls *controls;
    int n, i, v;

    /* Both buttons, X 125 and Y 180, so each control sits away from rest */
    Report(base, 0x00, 0x03, 0xD0, 0x07, 0xB4);
    Input(h, base, sizeof(base));
    controls = BH_Controls(h);
    BH_CHECK(controls->buttons == (EAST_BIT | SOUTH_BIT) && controls->axes[SDL_BLE_AXIS_LEFTX] == 5324 &&
                 controls->axes[SDL_BLE_AXIS_LEFTY] == 32767,
             "base state");
    n = h->nsnapshots;
    for (v = 0; v < 256; ++v) {
        memcpy(report, base, sizeof(report));
        report[0] = (uint8_t)v;
        Input(h, report, sizeof(report));
    }
    BH_CHECK(h->nsnapshots == n, "the counter emitted %d snapshots", h->nsnapshots - n);
    for (i = 5; i < REPORT_SIZE; ++i) {
        for (v = 0; v < (int)sizeof(values); ++v) {
            memcpy(report, base, sizeof(report));
            report[i] = values[v];
            Input(h, report, sizeof(report));
            BH_CHECK(h->nsnapshots == n, "byte %d at %02X emitted a snapshot", i, values[v]);
        }
    }
    /* All motion bytes at once */
    memcpy(report, base, sizeof(report));
    memset(&report[5], 0xA5, 12);
    Input(h, report, sizeof(report));
    BH_CHECK(h->nsnapshots == n && h->nsamples == 0, "motion bytes emitted %d snapshots, %d samples", h->nsnapshots - n,
             h->nsamples);
    /* Positive control: byte 1 in the same stream does emit */
    memcpy(report, base, sizeof(report));
    report[1] = 0x01;
    Input(h, report, sizeof(report));
    BH_CHECK(h->nsnapshots == n + 1, "byte 1 emitted %d snapshots", h->nsnapshots - n);
    BH_Destroy(h);
}

/* Test 7: every truncation of each packet above, 0 to 16 bytes, changes
   nothing, since the decoder needs 17 bytes. Bytes after byte 16 are
   ignored. */
static void Test7_Lengths(void)
{
    static const uint8_t packets[][5] = {
        { 0x00, 0x00, 0x00, 0x07, 0x6C }, /* test 1 */
        { 0x00, 0x01, 0x00, 0x07, 0x6C }, /* test 2 */
        { 0x00, 0x02, 0x00, 0x07, 0x6C },
        { 0x00, 0x03, 0x00, 0x07, 0x6C },
        { 0x00, 0xFC, 0x00, 0x07, 0x6C },
        { 0x00, 0x00, 0x00, 0x02, 0x6C }, /* test 3 */
        { 0x00, 0x00, 0x00, 0x0C, 0x6C },
        { 0x00, 0x00, 0xD0, 0x07, 0x6C },
        { 0x00, 0x00, 0x00, 0x0D, 0x6C },
        { 0x00, 0x00, 0x00, 0x01, 0x6C },
        { 0x00, 0x00, 0x00, 0x07, 0x24 }, /* test 4 */
        { 0x00, 0x00, 0x00, 0x07, 0xB4 },
        { 0x00, 0x00, 0x0F, 0xF7, 0x6C }, /* test 5 */
    };
    static const size_t longer[] = { 18, 19, 20, 32, 64, 68, 128 };
    BH_Harness *h = Up();
    uint8_t sentinel[REPORT_SIZE], report[REPORT_SIZE], buffer[128], clean[REPORT_SIZE];
    SDL_BLEControls held, decoded;
    int p, n;
    size_t length;

    /* Both buttons and the stick at (-32767, -32767): every packet above
       decodes to something else */
    Report(sentinel, 0x00, 0x03, 0x00, 0x02, 0x24);
    for (p = 0; p < (int)(sizeof(packets) / sizeof(packets[0])); ++p) {
        Report(report, packets[p][0], packets[p][1], packets[p][2], packets[p][3], packets[p][4]);
        Input(h, sentinel, sizeof(sentinel));
        held = *BH_Controls(h);
        n = h->nsnapshots;
        for (length = 0; length < REPORT_SIZE; ++length) {
            Input(h, report, length);
            BH_CHECK(h->nsnapshots == n && SDL_BLE_ControlsEqual(BH_Controls(h), &held), "packet %d cut to %d bytes changed the state",
                     p, (int)length);
        }
        /* Positive control: the whole packet changes the state */
        Input(h, report, sizeof(report));
        BH_CHECK(h->nsnapshots == n + 1, "packet %d: %d snapshots from the full packet", p, h->nsnapshots - n);
        decoded = *BH_Controls(h);

        /* Longer values decode their first 17 bytes, whatever follows */
        for (length = 0; length < sizeof(longer) / sizeof(longer[0]); ++length) {
            memset(buffer, 0xFF, sizeof(buffer));
            memcpy(buffer, report, sizeof(report));
            Input(h, sentinel, sizeof(sentinel));
            Input(h, buffer, longer[length]);
            BH_CHECK(SDL_BLE_ControlsEqual(BH_Controls(h), &decoded), "packet %d as %d bytes decoded differently", p,
                     (int)longer[length]);
        }
    }

    /* A 17-byte value read from a larger buffer that holds stale bytes after
       it decodes as the clean value */
    Report(clean, 0x00, 0x02, 0xD0, 0x07, 0x24);
    Input(h, clean, sizeof(clean));
    decoded = *BH_Controls(h);
    Input(h, sentinel, sizeof(sentinel));
    memset(buffer, 0xFF, sizeof(buffer));
    memcpy(buffer, clean, sizeof(clean));
    SDL_BLEPokeballModule.Value(h->state, INPUT, buffer, REPORT_SIZE, h->now * 1000000);
    BH_CHECK(SDL_BLE_ControlsEqual(BH_Controls(h), &decoded), "stale bytes after byte 16 changed the result");
    BH_Destroy(h);
}

/* Test 8: the battery level. 64 is 100 percent and 00 is 0 percent. The
   Battery Level characteristic holds a percentage (daydream-catcher lib.rs
   :28-36 for the same characteristic, PokeballController.cs:129-134). */
static void Test8_Battery(void)
{
    static const struct
    {
        const char *hex;
        int8_t percent;
    } cases[] = {
        { "64", 100 }, { "00", 0 },
        { "32", 50 }, { "01", 1 }, { "63", 99 },
        /* Out of the characteristic's range: clamped to 100 */
        { "65", 100 }, { "80", 100 }, { "FF", 100 },
        /* A longer value: the first byte */
        { "21 99", 33 },
    };
    BH_Harness *h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    uint8_t value[4], report[REPORT_SIZE];
    size_t length;
    int i, n;

    /* The read after the subscriptions delivers the level once */
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    BH_CHECK(BH_LastKind(h) == SDL_BLE_ACTION_READ && BH_Last(h)->characteristic == BATTERY, "no battery read");
    BH_ReadAll(h, (const uint8_t *)"\x4B", 1);
    BH_CHECK(BH_Controls(h)->battery == 75, "read level %d", BH_Controls(h)->battery);
    BH_CHECK(h->nsnapshots == 1 && h->snapshots[0].controls.battery == 75, "read snapshot");
    /* A battery level alone does not publish */
    BH_Value(h, BATTERY, (const uint8_t *)"\x4C", 1, false);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0, "a battery value published");
    Report(report, 0, 0x00, 0x00, 0x07, 0x6C);
    Input(h, report, sizeof(report));
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1, "not published");

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        length = BH_Hex(cases[i].hex, value, sizeof(value));
        BH_Value(h, BATTERY, value, length, false);
        BH_CHECK(BH_Controls(h)->battery == cases[i].percent, "battery %s: %d, want %d", cases[i].hex,
                 BH_Controls(h)->battery, cases[i].percent);
        BH_CHECK(AtRest(BH_Controls(h)), "battery %s moved a control", cases[i].hex);
    }
    /* An empty value changes nothing */
    n = h->nsnapshots;
    BH_Value(h, BATTERY, value, 0, false);
    BH_CHECK(h->nsnapshots == n && BH_Controls(h)->battery == 33, "empty value: battery %d", BH_Controls(h)->battery);
    /* Input reports keep the level */
    Report(report, 0, 0x03, 0x00, 0x0C, 0xB4);
    Input(h, report, sizeof(report));
    BH_CHECK(BH_Controls(h)->battery == 33, "a report changed the battery to %d", BH_Controls(h)->battery);
    /* A battery value on the input index is too short to decode */
    BH_Value(h, INPUT, (const uint8_t *)"\x10", 1, false);
    BH_CHECK(BH_Controls(h)->battery == 33 && BH_Controls(h)->buttons == (EAST_BIT | SOUTH_BIT), "short input");
    BH_Destroy(h);
}

/* Test 9: discovery. Pokemon PBP matches. Pokemon PB and pokemon pbp do not:
   the match is exact, as PokeballVigemDriver.cs:59 compares. */
static void Test9_Discovery(void)
{
    static const char *const misses[] = {
        "Pokemon PB", "pokemon pbp", "Pokemon PBX", "POKEMON PBP", "Pokemon PBP ", " Pokemon PBP", "Pokemon PBPX",
        "Pokemon  PBP", "PokemonPBP", "Pokemon PBp", "", "Pokemon",
    };
    SDL_BLEAdvertisement ad;
    bool enabled[SDL_BLE_FAMILY_COUNT];
    uint8_t uuid[16];
    int i;

    NameRecord(&ad, "Pokemon PBP", SDL_BLE_AD_ADVERTISEMENT);
    BH_CHECK(Match(&ad, NULL) == SDL_BLE_FAMILY_POKEBALL, "advertisement: %d", Match(&ad, NULL));
    /* A scan response carries the name alike */
    NameRecord(&ad, "Pokemon PBP", SDL_BLE_AD_SCAN_RESPONSE);
    BH_CHECK(Match(&ad, NULL) == SDL_BLE_FAMILY_POKEBALL, "scan response: %d", Match(&ad, NULL));
    for (i = 0; i < (int)(sizeof(misses) / sizeof(misses[0])); ++i) {
        NameRecord(&ad, misses[i], SDL_BLE_AD_ADVERTISEMENT);
        BH_CHECK(Match(&ad, NULL) == -1, "'%s' matched %d", misses[i], Match(&ad, NULL));
    }
    /* The name bytes count only when the event carries a name */
    NameRecord(&ad, "Pokemon PBP", SDL_BLE_AD_ADVERTISEMENT);
    ad.has_name = false;
    BH_CHECK(Match(&ad, NULL) == -1, "a record without a name matched");
    /* No service or manufacturer key is documented: the vendor service in the
       service list matches nothing */
    NameRecord(&ad, NULL, SDL_BLE_AD_ADVERTISEMENT);
    BH_CHECK(BH_Hex("6675e16c-f36d-4567-bb55-6b51e27a23e5", uuid, sizeof(uuid)) == 16, "the vendor service UUID");
    memcpy(ad.services[0].bytes, uuid, 16);
    ad.services[1] = SDL_BLE_UUID16(0x180F);
    ad.nservices = 2;
    BH_CHECK(Match(&ad, NULL) == -1, "the services matched");
    /* Its hint off, the family matches nothing, and it alone on still matches */
    NameRecord(&ad, "Pokemon PBP", SDL_BLE_AD_ADVERTISEMENT);
    for (i = 0; i < SDL_BLE_FAMILY_COUNT; ++i) {
        enabled[i] = true;
    }
    enabled[SDL_BLE_FAMILY_POKEBALL] = false;
    BH_CHECK(Match(&ad, enabled) == -1, "matched with its hint off");
    for (i = 0; i < SDL_BLE_FAMILY_COUNT; ++i) {
        enabled[i] = (i == SDL_BLE_FAMILY_POKEBALL);
    }
    BH_CHECK(Match(&ad, enabled) == SDL_BLE_FAMILY_POKEBALL, "no match with only its hint on");
}

/* Test 10: link loss releases both buttons and centers the stick. The next
   advertisement reconnects with the same serial, which is the address. */
static void Test10_LinkLoss(void)
{
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad;
    SDL_BLEMatch match;
    BH_Harness *h;
    uint8_t report[REPORT_SIZE];
    const SDL_BLEControls *release;
    int n, k;

    SDL_BLEHost_Init(&host);
    NameRecord(&ad, "Pokemon PBP", SDL_BLE_AD_ADVERTISEMENT);
    memset(&match, 0, sizeof(match));
    BH_CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 1000, &match),
             "first advertisement");
    BH_CHECK(match.family == SDL_BLE_FAMILY_POKEBALL && match.variant == 0, "match %d", match.family);
    /* The address is reserved while its session runs */
    BH_CHECK(!SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 1001, &match),
             "a second connect for the same address");

    h = Up();
    BH_Value(h, BATTERY, (const uint8_t *)"\x50", 1, false);
    Report(report, 0x07, 0x03, 0x00, 0x0C, 0xB4);
    Input(h, report, sizeof(report));
    BH_CHECK(BH_Controls(h)->buttons == (EAST_BIT | SOUTH_BIT) && BH_Axis(h, SDL_BLE_AXIS_LEFTX) == 32767 &&
                 BH_Axis(h, SDL_BLE_AXIS_LEFTY) == 32767,
             "held state");
    n = h->nsnapshots;
    k = h->nactions;
    SDL_BLESession_Lost(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(h->nsnapshots == n + 1, "%d snapshots on the loss", h->nsnapshots - n);
    if (h->nsnapshots == n + 1) {
        release = &h->snapshots[n].controls;
        BH_CHECK(AtRest(release), "the loss left a control held");
        BH_CHECK(release->battery == 80, "the loss dropped the battery, %d", release->battery);
    }
    BH_CHECK(h->nactions == k + 2 && IsAction(h, k, SDL_BLE_ACTION_REMOVE) && IsAction(h, k + 1, SDL_BLE_ACTION_DISCONNECT),
             "loss actions");
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 0 && !h->session.backoff, "a link loss backs off");
    BH_CHECK(SDL_BLESession_Ended(&h->session) && !h->published, "session state");
    /* A value still queued when the link dropped changes nothing */
    Input(h, report, sizeof(report));
    BH_CHECK(h->nsnapshots == n + 1 && AtRest(BH_Controls(h)), "a value after the loss");

    /* The next advertisement from the same address connects at once */
    {
        SDL_BLEOutcome outcome;

        SDL_BLESession_GetOutcome(&h->session, &outcome);
        SDL_BLEHost_SessionEnded(&host, ad.address, &outcome, 2000);
    }
    BH_Destroy(h);
    BH_CHECK(!SDL_BLEHost_IsActive(&host, ad.address), "still reserved");
    memset(&match, 0, sizeof(match));
    BH_CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 2000, &match),
             "no reconnect");
    BH_CHECK(match.family == SDL_BLE_FAMILY_POKEBALL && ad.address == ADDRESS, "reconnect match");
    h = BH_Create(&SDL_BLEPokeballFamily, match.variant, NULL, false);
    BH_CHECK(h->nactions == 1 && IsAction(h, 0, SDL_BLE_ACTION_CONNECT), "no connect");
    BH_Start(h, false);
    BH_CHECK(AtRest(BH_Controls(h)) && BH_Controls(h)->battery == -1, "the new session starts at rest");
    Report(report, 0x08, 0x01, 0x00, 0x07, 0x6C);
    Input(h, report, sizeof(report));
    BH_CHECK(h->published == 1 && BH_Controls(h)->buttons == EAST_BIT, "the new session");
    BH_Destroy(h);
}

/* Test 11: on an injected clock the session writes nothing but the
   descriptors, at start-up or later */
static void Test11_Clock(void)
{
    static const int kinds[] = {
        SDL_BLE_ACTION_CONNECT, SDL_BLE_ACTION_DISCOVER, SDL_BLE_ACTION_SUBSCRIBE,
        SDL_BLE_ACTION_SUBSCRIBE, SDL_BLE_ACTION_READ, SDL_BLE_ACTION_PUBLISH,
    };
    BH_Harness *h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, true);
    uint8_t report[REPORT_SIZE];
    uint64_t deadline;
    bool deadlines = false;
    int step, i;

    BH_Start(h, false);
    Report(report, 0x00, 0x00, 0x00, 0x07, 0x6C);
    /* One hour, a report and a tick every second */
    for (step = 0; step <= 3600; ++step) {
        BH_Advance(h, (uint64_t)step * 1000);
        SDL_BLESession_Tick(&h->session, h->now);
        BH_Drain(h);
        report[0] = (uint8_t)step;
        report[1] = (uint8_t)((step / 60) & 0x03);
        Input(h, report, sizeof(report));
        if (SDL_BLESession_GetDeadline(&h->session, &deadline)) {
            deadlines = true;
        }
    }
    BH_CHECK(!deadlines, "a timer was set");
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == 0, "%d writes", BH_Count(h, SDL_BLE_ACTION_WRITE));
    BH_CHECK(h->nactions == (int)(sizeof(kinds) / sizeof(kinds[0])), "%d actions", h->nactions);
    for (i = 0; i < (int)(sizeof(kinds) / sizeof(kinds[0])); ++i) {
        BH_CHECK(IsAction(h, i, kinds[i]), "action %d is %s", i,
                 i < h->nactions ? SDL_BLE_ActionName(h->actions[i].action.kind) : "missing");
    }
    BH_CHECK(IsSubscribe(h, 2, INPUT, SDL_BLE_CCCD_NOTIFY) && IsSubscribe(h, 3, BATTERY, SDL_BLE_CCCD_NOTIFY),
             "the descriptors");
    BH_CHECK(((const SDL_BLEBase *)h->state)->write_count == 0, "queued writes");
    /* Closing on purpose writes nothing either */
    SDL_BLESession_Close(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == 0 && BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT, "close");
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_REMOVE) == 1 && SDL_BLESession_Ended(&h->session), "close ends");
    BH_Destroy(h);
}

/* The ready rule: the joystick appears with the first full report after the
   start, never before and never from another value */
static void TestReady(void)
{
    BH_Harness *h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    uint8_t report[REPORT_SIZE];

    Report(report, 0x01, 0x01, 0x00, 0x07, 0x6C);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_CHECK(IsSubscribe(h, h->nactions - 1, INPUT, SDL_BLE_CCCD_NOTIFY), "input subscription");
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_CHECK(IsSubscribe(h, h->nactions - 1, BATTERY, SDL_BLE_CCCD_NOTIFY), "battery subscription");
    /* A report while the subscriptions run is decoded but does not publish */
    Input(h, report, sizeof(report));
    BH_CHECK(BH_Controls(h)->buttons == EAST_BIT, "report before the start not decoded");
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0, "published before the start");
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_CHECK(BH_LastKind(h) == SDL_BLE_ACTION_READ, "no read");
    Input(h, report, sizeof(report));
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0, "published while reading");
    BH_ReadAll(h, (const uint8_t *)"\x5A", 1);
    BH_CHECK(h->session.phase == SDL_BLE_PHASE_STARTING, "phase %d", h->session.phase);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0, "published by the start alone");
    BH_CHECK(!((const SDL_BLEBase *)h->state)->ready, "ready before a report after the start");
    /* After the start, a short report and a battery value do not publish */
    Input(h, report, REPORT_SIZE - 1);
    BH_Value(h, BATTERY, (const uint8_t *)"\x59", 1, false);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0, "published by a short report or the battery");
    /* The first full report publishes, once */
    Input(h, report, sizeof(report));
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1 && h->published == 1, "not published");
    BH_CHECK(h->session.phase == SDL_BLE_PHASE_RUNNING, "phase %d", h->session.phase);
    Input(h, report, sizeof(report));
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1, "published twice");
    BH_Destroy(h);
}

/* A gap releases every held control before its value applies */
static void TestGap(void)
{
    BH_Harness *h = Up();
    uint8_t held[REPORT_SIZE], rest[REPORT_SIZE];
    int n;

    BH_Value(h, BATTERY, (const uint8_t *)"\x3C", 1, false);
    Report(held, 0x01, 0x03, 0x00, 0x0C, 0xB4);
    Report(rest, 0x02, 0x00, 0x00, 0x07, 0x6C);
    Input(h, held, sizeof(held));

    /* Still held after the gap: released, then held again */
    n = h->nsnapshots;
    BH_Value(h, INPUT, held, sizeof(held), true);
    BH_CHECK(h->nsnapshots == n + 2, "%d snapshots", h->nsnapshots - n);
    if (h->nsnapshots == n + 2) {
        BH_CHECK(AtRest(&h->snapshots[n].controls) && h->snapshots[n].controls.battery == 60, "release");
        BH_CHECK(h->snapshots[n + 1].controls.buttons == (EAST_BIT | SOUTH_BIT) &&
                     h->snapshots[n + 1].controls.axes[SDL_BLE_AXIS_LEFTX] == 32767 &&
                     h->snapshots[n + 1].controls.axes[SDL_BLE_AXIS_LEFTY] == 32767,
                 "held again");
    }
    /* A rest value after the gap: only the release */
    n = h->nsnapshots;
    BH_Value(h, INPUT, rest, sizeof(rest), true);
    BH_CHECK(h->nsnapshots == n + 1 && AtRest(BH_Controls(h)), "gap before rest: %d snapshots", h->nsnapshots - n);
    /* A gap at rest emits nothing */
    n = h->nsnapshots;
    BH_Value(h, INPUT, rest, sizeof(rest), true);
    BH_CHECK(h->nsnapshots == n, "gap at rest: %d snapshots", h->nsnapshots - n);
    /* A gap on a value too short to decode still releases */
    Input(h, held, sizeof(held));
    n = h->nsnapshots;
    BH_Value(h, INPUT, held, 10, true);
    BH_CHECK(h->nsnapshots == n + 1 && AtRest(BH_Controls(h)), "gap on a short value");
    /* A gap on a battery value releases too, and keeps the level it brings */
    Input(h, held, sizeof(held));
    n = h->nsnapshots;
    BH_Value(h, BATTERY, (const uint8_t *)"\x3B", 1, true);
    BH_CHECK(h->nsnapshots == n + 2 && AtRest(BH_Controls(h)) && BH_Controls(h)->battery == 59, "gap on the battery");
    BH_Destroy(h);
}

/* Pairing and subscriptions: this family never pairs, a bond gets None
   before Notify, the battery is optional and the input is not */
static void TestPairing(void)
{
    bool found[SDL_BLE_MAX_CHARS];
    uint8_t properties[SDL_BLE_MAX_CHARS];
    uint8_t report[REPORT_SIZE];
    BH_Harness *h;
    int pairing, i;

    Report(report, 0x00, 0x00, 0x00, 0x07, 0x6C);

    /* No bond, the hint off or on: never a pairing (PokeballVigemDriver.cs:68) */
    for (pairing = 0; pairing < 2; ++pairing) {
        h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, pairing != 0);
        BH_Connected(h, true, false);
        BH_CHECK(h->nactions == 2 && BH_LastKind(h) == SDL_BLE_ACTION_DISCOVER, "hint %d: discover", pairing);
        BH_Discovered(h, true, NULL);
        BH_CHECK(IsSubscribe(h, 2, INPUT, SDL_BLE_CCCD_NOTIFY), "hint %d: input", pairing);
        BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
        BH_CHECK(IsSubscribe(h, 3, BATTERY, SDL_BLE_CCCD_NOTIFY), "hint %d: battery", pairing);
        BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
        BH_CHECK(IsAction(h, 4, SDL_BLE_ACTION_READ) && h->actions[4].action.characteristic == BATTERY, "hint %d: read",
                 pairing);
        BH_ReadAll(h, NULL, 0);
        Input(h, report, sizeof(report));
        BH_CHECK(h->published == 1 && BH_Count(h, SDL_BLE_ACTION_PAIR) == 0, "hint %d: published without pairing", pairing);
        BH_Destroy(h);
    }

    /* A bond: None, then Notify, for each characteristic in order */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    BH_CHECK(IsSubscribe(h, 2, INPUT, SDL_BLE_CCCD_NONE) && IsSubscribe(h, 3, INPUT, SDL_BLE_CCCD_NOTIFY) &&
                 IsSubscribe(h, 4, BATTERY, SDL_BLE_CCCD_NONE) && IsSubscribe(h, 5, BATTERY, SDL_BLE_CCCD_NOTIFY) &&
                 IsAction(h, 6, SDL_BLE_ACTION_READ),
             "bonded subscriptions");
    BH_Destroy(h);

    /* An input subscription that wants encryption backs off: the family does
       not pair on error */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PAIR) == 0 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1 &&
                 BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT,
             "insufficient encryption");
    BH_Destroy(h);

    /* A stale bond with the hint on is removed and the subscriptions run once
       more, as the tool's README asks users to remove old pairings
       (README.md:56). With the hint off the session backs off. */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    BH_CHECK(BH_LastKind(h) == SDL_BLE_ACTION_REMOVE_BOND, "no bond removal");
    BH_BondRemoved(h, true);
    BH_CHECK(IsSubscribe(h, h->nactions - 1, INPUT, SDL_BLE_CCCD_NOTIFY), "no repeat without the bond");
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    Input(h, report, sizeof(report));
    BH_CHECK(h->published == 1 && BH_Count(h, SDL_BLE_ACTION_PAIR) == 0, "stale bond: not published");
    BH_Destroy(h);
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_REMOVE_BOND) == 0 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1, "hint off");
    BH_Destroy(h);

    /* Without the battery service it still runs (find-a-cig app/js/ball.js:44-49) */
    for (i = 0; i < SDL_BLE_MAX_CHARS; ++i) {
        found[i] = true;
    }
    BH_DefaultProperties(&SDL_BLEPokeballFamily, properties);
    found[BATTERY] = false;
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    SDL_BLESession_Discovered(&h->session, true, found, properties, h->now);
    BH_Drain(h);
    BH_SubscribeAll(h);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == 1 && BH_Count(h, SDL_BLE_ACTION_READ) == 0, "no battery");
    Input(h, report, sizeof(report));
    BH_CHECK(h->published == 1, "not published without the battery");
    BH_Destroy(h);

    /* A battery level that only reads is read, not subscribed */
    found[BATTERY] = true;
    properties[BATTERY] = SDL_BLE_PROPERTY_READ;
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    SDL_BLESession_Discovered(&h->session, true, found, properties, h->now);
    BH_Drain(h);
    BH_SubscribeAll(h);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == 1 && BH_LastKind(h) == SDL_BLE_ACTION_READ, "read only battery");
    BH_ReadAll(h, (const uint8_t *)"\x2A", 1);
    Input(h, report, sizeof(report));
    BH_CHECK(h->published == 1 && BH_Controls(h)->battery == 42, "read only battery: %d", BH_Controls(h)->battery);
    BH_Destroy(h);

    /* The input characteristic is required: missing, or without Notify and
       Indicate, the session backs off */
    found[INPUT] = false;
    BH_DefaultProperties(&SDL_BLEPokeballFamily, properties);
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    SDL_BLESession_Discovered(&h->session, true, found, properties, h->now);
    BH_Drain(h);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == 0 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1, "missing input");
    BH_Destroy(h);
    found[INPUT] = true;
    properties[INPUT] = SDL_BLE_PROPERTY_READ;
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    SDL_BLESession_Discovered(&h->session, true, found, properties, h->now);
    BH_Drain(h);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == 0 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1, "input without notify");
    BH_Destroy(h);
}

/* Values on an index the family does not have are ignored */
static void TestOtherCharacteristics(void)
{
    BH_Harness *h = Up();
    uint8_t report[REPORT_SIZE];
    SDL_BLEControls before;
    int n = h->nsnapshots;

    Report(report, 0x00, 0x03, 0x00, 0x0C, 0xB4);
    before = *BH_Controls(h);
    SDL_BLEPokeballModule.Value(h->state, 2, report, sizeof(report), 0);
    SDL_BLEPokeballModule.Value(h->state, -1, report, sizeof(report), 0);
    SDL_BLEPokeballModule.Value(h->state, 7, report, sizeof(report), 0);
    BH_Value(h, 2, report, sizeof(report), false);
    BH_CHECK(h->nsnapshots == n && SDL_BLE_ControlsEqual(BH_Controls(h), &before), "another index changed the state");
    /* Positive control: the same report on the input index */
    Input(h, report, sizeof(report));
    BH_CHECK(h->nsnapshots == n + 1, "the input index");
    BH_Destroy(h);
}

int main(void)
{
    Run("Table: family and identity", TestTable);
    Run("Test 1: rest report", Test1_Rest);
    Run("Test 2: buttons", Test2_Buttons);
    Run("Test 3: stick X", Test3_X);
    Run("Test 4: stick Y", Test4_Y);
    Run("Test 5: undecoded nibbles", Test5_Nibbles);
    Run("Test 6: counter and motion bytes", Test6_Ignored);
    Run("Test 7: lengths", Test7_Lengths);
    Run("Test 8: battery", Test8_Battery);
    Run("Test 9: discovery", Test9_Discovery);
    Run("Test 10: link loss and reconnect", Test10_LinkLoss);
    Run("Test 11: injected clock", Test11_Clock);
    Run("Ready rule", TestReady);
    Run("Gap release", TestGap);
    Run("Pairing and subscriptions", TestPairing);
    Run("Other characteristics", TestOtherCharacteristics);
    return BH_Report("testblepokeball");
}
