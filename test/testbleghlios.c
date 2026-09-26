/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/ble/SDL_ble_ghlios_proto.c, the Guitar Hero
   Live iOS guitar of hifihedgehog/SDL#33 Part 12. Test numbers follow the
   part's guitar section. No capture is published, so every report is built
   from the part's layout (GHLtarUtility iOSGuitar.cs:69-110, ghlioscon
   ghlioscon.swift:210-223, PlasticBand iOS.md:74-79). */

#include "testbleharness.h"
#include "SDL_ble_ghlios_proto.h"

#define REPORT SDL_GHLIOS_REPORT_SIZE
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define BTN(name) (1u << SDL_BLE_BUTTON_##name)
#define DPAD (BTN(DPAD_UP) | BTN(DPAD_DOWN) | BTN(DPAD_LEFT) | BTN(DPAD_RIGHT))

static const uint64_t guitar_address = 0xC0FFEE123456;

/* Test 1's rest report: hat 0F, strum, whammy and tilt at 80 */
static void Rest(uint8_t *report)
{
    memset(report, 0, REPORT);
    report[2] = 0x0F;
    report[4] = 0x80;
    report[6] = 0x80;
    report[19] = 0x80;
}

/* The rest report with one byte changed */
static void RestWith(uint8_t *report, int index, uint8_t value)
{
    Rest(report);
    report[index] = value;
}

static void Feed(BH_Harness *h, const uint8_t *report, size_t length)
{
    BH_Value(h, SDL_GHLIOS_INPUT, report, length, false);
}

/* A session past its start-up, not yet published */
static BH_Harness *Started(void)
{
    BH_Harness *h = BH_Create(&SDL_BLEGHLiOSFamily, 0, NULL, false);

    h->now = 1000;
    BH_Start(h, false);
    return h;
}

/* A published session holding the rest report */
static BH_Harness *AtRest(void)
{
    BH_Harness *h = Started();
    uint8_t report[REPORT];

    Rest(report);
    Feed(h, report, sizeof(report));
    return h;
}

/* Applies a full report and checks every control the guitar has */
static void Expect(BH_Harness *h, const uint8_t *report, uint32_t buttons, int lefty, int rightx, int righty,
                   const char *what)
{
    const SDL_BLEControls *c;

    Feed(h, report, REPORT);
    c = BH_Controls(h);
    BH_CHECK(c->buttons == buttons, "%s: buttons %08x, expected %08x", what, (unsigned)c->buttons, (unsigned)buttons);
    BH_CHECK(c->axes[SDL_BLE_AXIS_LEFTY] == lefty, "%s: left Y %d, expected %d", what, c->axes[SDL_BLE_AXIS_LEFTY], lefty);
    BH_CHECK(c->axes[SDL_BLE_AXIS_RIGHTX] == rightx, "%s: right X %d, expected %d", what, c->axes[SDL_BLE_AXIS_RIGHTX], rightx);
    BH_CHECK(c->axes[SDL_BLE_AXIS_RIGHTY] == righty, "%s: right Y %d, expected %d", what, c->axes[SDL_BLE_AXIS_RIGHTY], righty);
    BH_CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == 0 && c->axes[SDL_BLE_AXIS_LEFT_TRIGGER] == 0 &&
                 c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == 0,
             "%s: an axis the guitar lacks moved", what);
    BH_CHECK(!c->finger && c->battery == -1, "%s: finger or battery moved", what);
}

/* The D-pad buttons the hat alone presses: 0 up, clockwise to 7 up-left */
static uint32_t HatButtons(int hat)
{
    switch (hat) {
    case 0:
        return BTN(DPAD_UP);
    case 1:
        return BTN(DPAD_UP) | BTN(DPAD_RIGHT);
    case 2:
        return BTN(DPAD_RIGHT);
    case 3:
        return BTN(DPAD_DOWN) | BTN(DPAD_RIGHT);
    case 4:
        return BTN(DPAD_DOWN);
    case 5:
        return BTN(DPAD_DOWN) | BTN(DPAD_LEFT);
    case 6:
        return BTN(DPAD_LEFT);
    case 7:
        return BTN(DPAD_UP) | BTN(DPAD_LEFT);
    default:
        return 0;
    }
}

/* The whammy and tilt the part specifies, computed in floating point here
   so the module's integer arithmetic is checked against another route */
static int WhammyOf(int value)
{
    if (value <= 128) {
        return 0;
    }
    return -(int)((double)(value - 128) * 32768.0 / 127.0);
}

static int TiltOf(int value)
{
    return (value - 128) * 256;
}

static void TestIdentity(void)
{
    BH_Harness *h = Started();
    const SDL_BLEIdentity *id = &((const SDL_BLEBase *)h->state)->identity;
    const uint32_t buttons = BTN(SOUTH) | BTN(EAST) | BTN(WEST) | BTN(NORTH) | BTN(BACK) | BTN(GUIDE) | BTN(START) |
                             BTN(LEFT_STICK) | BTN(LEFT_SHOULDER) | BTN(RIGHT_SHOULDER) | DPAD;

    printf("Identity\n");
    BH_CHECK(strcmp(id->name, "Guitar Hero Live Guitar (iOS)") == 0, "name %s", id->name);
    BH_CHECK(id->type == SDL_BLE_TYPE_GUITAR && id->gamepad, "type %d gamepad %d", id->type, id->gamepad);
    BH_CHECK(id->buttons == buttons, "buttons %08x", (unsigned)id->buttons);
    BH_CHECK(id->axes == ((1u << SDL_BLE_AXIS_LEFTY) | (1u << SDL_BLE_AXIS_RIGHTX) | (1u << SDL_BLE_AXIS_RIGHTY)),
             "axes %02x", id->axes);
    BH_CHECK(id->vendor == 0 && id->product == 0, "VID and PID are unknown (iOS.md:5)");
    BH_CHECK(!id->touchpad && id->accel_rate == 0.0f && id->gyro_rate == 0.0f, "no touchpad or sensors");
    BH_Destroy(h);

    /* The family's service and its one characteristic, as the part gives
       them: 533e1523-3abe-f33f-cd00-594e8b0a8ea3 and 533e1524-..., the
       input, subscribed */
    {
        const SDL_BLEFamily *family = &SDL_BLEGHLiOSFamily;
        uint8_t service[16], input[16];

        BH_CHECK(BH_Hex("533e1523-3abe-f33f-cd00-594e8b0a8ea3", service, sizeof(service)) == 16 &&
                     BH_Hex("533e1524-3abe-f33f-cd00-594e8b0a8ea3", input, sizeof(input)) == 16,
                 "the UUID texts");
        BH_CHECK(SDL_BLE_Families[SDL_BLE_FAMILY_GHLIOS] == family && family->module == &SDL_BLEGHLiOSModule,
                 "the guitar's family");
        BH_CHECK(family->nservices == 1 && !family->has_alternate && memcmp(family->services[0].bytes, service, 16) == 0,
                 "service 533e1523");
        BH_CHECK(family->ncharacteristics == 1 && family->characteristics[SDL_GHLIOS_INPUT].service == 0 &&
                     family->characteristics[SDL_GHLIOS_INPUT].flags == SDL_BLE_CHAR_SUBSCRIBE &&
                     memcmp(family->characteristics[SDL_GHLIOS_INPUT].uuid.bytes, input, 16) == 0,
                 "input characteristic 533e1524");
    }
}

static void Test1(void)
{
    BH_Harness *h = Started();
    uint8_t report[REPORT];
    const uint8_t rest[REPORT] = { 0x00, 0x00, 0x0F, 0x00, 0x80, 0x00, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80 };

    printf("Test 1: rest\n");
    Rest(report);
    BH_CHECK(memcmp(report, rest, REPORT) == 0, "the rest report is the part's bytes");
    BH_CHECK(!h->published, "nothing is published before the first report");
    Expect(h, rest, 0, 0, 0, 0, "rest");
    BH_CHECK(h->published && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1, "the first full report publishes");
    BH_CHECK(h->nsnapshots == 0, "the rest report changes nothing from the rest controls");
    BH_Destroy(h);
}

static void Test2(void)
{
    static const uint8_t bytes[6] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20 };
    const uint32_t buttons[6] = { BTN(WEST), BTN(SOUTH), BTN(EAST), BTN(NORTH), BTN(LEFT_SHOULDER), BTN(RIGHT_SHOULDER) };
    BH_Harness *h = AtRest();
    uint8_t report[REPORT];
    char what[64];
    int i;

    printf("Test 2: frets\n");
    for (i = 0; i < 6; ++i) {
        RestWith(report, 0, bytes[i]);
        (void)snprintf(what, sizeof(what), "byte 0 %02X", bytes[i]);
        Expect(h, report, buttons[i], 0, 0, 0, what);
    }
    RestWith(report, 0, 0xC0);
    Expect(h, report, 0, 0, 0, 0, "byte 0 C0");
    RestWith(report, 0, 0x3F);
    Expect(h, report, BTN(WEST) | BTN(SOUTH) | BTN(EAST) | BTN(NORTH) | BTN(LEFT_SHOULDER) | BTN(RIGHT_SHOULDER), 0, 0,
           0, "byte 0 3F");
    RestWith(report, 0, 0xFF);
    Expect(h, report, BTN(WEST) | BTN(SOUTH) | BTN(EAST) | BTN(NORTH) | BTN(LEFT_SHOULDER) | BTN(RIGHT_SHOULDER), 0, 0,
           0, "byte 0 FF");
    BH_Destroy(h);
}

static void Test3(void)
{
    static const uint8_t bytes[4] = { 0x02, 0x04, 0x08, 0x10 };
    const uint32_t buttons[4] = { BTN(START), BTN(LEFT_STICK), BTN(BACK), BTN(GUIDE) };
    BH_Harness *h = AtRest();
    uint8_t report[REPORT];
    char what[64];
    int i;

    printf("Test 3: pause, GHTV, hero power and sync\n");
    for (i = 0; i < 4; ++i) {
        RestWith(report, 1, bytes[i]);
        (void)snprintf(what, sizeof(what), "byte 1 %02X", bytes[i]);
        Expect(h, report, buttons[i], 0, 0, 0, what);
    }
    RestWith(report, 1, 0x01);
    Expect(h, report, 0, 0, 0, 0, "byte 1 01");
    RestWith(report, 1, 0xE0);
    Expect(h, report, 0, 0, 0, 0, "byte 1 E0");
    RestWith(report, 1, 0xFF);
    Expect(h, report, BTN(START) | BTN(LEFT_STICK) | BTN(BACK) | BTN(GUIDE), 0, 0, 0, "byte 1 FF");
    BH_Destroy(h);
}

static void Test4(void)
{
    BH_Harness *h = AtRest();
    uint8_t report[REPORT];
    int value;
    char what[64];

    printf("Test 4: strum\n");
    RestWith(report, 4, 0x00);
    Expect(h, report, BTN(DPAD_UP), -32768, 0, 0, "strum up");
    RestWith(report, 4, 0xFF);
    Expect(h, report, BTN(DPAD_DOWN), 32767, 0, 0, "strum down");
    RestWith(report, 4, 0x7F);
    Expect(h, report, 0, 0, 0, 0, "strum 7F");
    RestWith(report, 4, 0x81);
    Expect(h, report, 0, 0, 0, 0, "strum 81");
    /* Only 00 and FF strum, as GHLtarUtility and ghlioscon compare the whole
       byte (iOSGuitar.cs:79-99, ghlioscon.swift:216-217) */
    for (value = 1; value < 255; ++value) {
        RestWith(report, 4, (uint8_t)value);
        (void)snprintf(what, sizeof(what), "strum %02X", value);
        Expect(h, report, 0, 0, 0, 0, what);
    }
    BH_Destroy(h);
}

static void Test5(void)
{
    BH_Harness *h = AtRest();
    uint8_t report[REPORT];

    printf("Test 5: whammy\n");
    RestWith(report, 6, 0xC0);
    Expect(h, report, 0, 0, 0, -16513, "whammy C0");
    RestWith(report, 6, 0xFF);
    Expect(h, report, 0, 0, 0, -32768, "whammy FF");
    RestWith(report, 6, 0x7F);
    Expect(h, report, 0, 0, 0, 0, "whammy 7F");
    RestWith(report, 6, 0x80);
    Expect(h, report, 0, 0, 0, 0, "whammy 80");
    BH_Destroy(h);
}

static void Test6(void)
{
    BH_Harness *h = AtRest();
    uint8_t report[REPORT];

    printf("Test 6: tilt\n");
    RestWith(report, 19, 0xFF);
    Expect(h, report, 0, 0, 32512, 0, "tilt FF");
    RestWith(report, 19, 0x00);
    Expect(h, report, 0, 0, -32768, 0, "tilt 00");
    RestWith(report, 19, 0x40);
    Expect(h, report, 0, 0, -16384, 0, "tilt 40");
    RestWith(report, 19, 0x80);
    Expect(h, report, 0, 0, 0, 0, "tilt 80");
    BH_Destroy(h);
}

static void Test7(void)
{
    BH_Harness *h = AtRest();
    uint8_t report[REPORT];
    char what[64];
    int value;

    printf("Test 7: hat\n");
    RestWith(report, 2, 0x00);
    Expect(h, report, BTN(DPAD_UP), 0, 0, 0, "hat 00");
    RestWith(report, 2, 0x02);
    Expect(h, report, BTN(DPAD_RIGHT), 0, 0, 0, "hat 02");
    RestWith(report, 2, 0x07);
    Expect(h, report, BTN(DPAD_UP) | BTN(DPAD_LEFT), 0, 0, 0, "hat 07");
    for (value = 0x08; value <= 0x0F; ++value) {
        RestWith(report, 2, (uint8_t)value);
        (void)snprintf(what, sizeof(what), "hat %02X", value);
        Expect(h, report, 0, 0, 0, 0, what);
    }
    BH_Destroy(h);
}

/* Every packet of tests 1 to 7 */
#define NPACKETS 27
static int Packets(uint8_t packets[NPACKETS][REPORT])
{
    static const struct
    {
        int index;
        uint8_t value;
    } changes[NPACKETS] = {
        { 2, 0x0F }, /* test 1's rest */
        { 0, 0x01 }, { 0, 0x02 }, { 0, 0x04 }, { 0, 0x08 }, { 0, 0x10 }, { 0, 0x20 }, { 0, 0xC0 },
        { 1, 0x02 }, { 1, 0x04 }, { 1, 0x08 }, { 1, 0x10 }, { 1, 0x01 }, { 1, 0xE0 },
        { 4, 0x00 }, { 4, 0xFF }, { 4, 0x7F }, { 4, 0x81 },
        { 6, 0xC0 }, { 6, 0xFF }, { 6, 0x7F },
        { 19, 0xFF }, { 19, 0x00 }, { 19, 0x40 },
        { 2, 0x00 }, { 2, 0x02 }, { 2, 0x07 }
    };
    int i;

    for (i = 0; i < NPACKETS; ++i) {
        RestWith(packets[i], changes[i].index, changes[i].value);
    }
    return NPACKETS;
}

static void Test8(void)
{
    uint8_t packets[NPACKETS][REPORT];
    uint8_t other[REPORT], stale[64];
    SDL_BLEControls expected, before;
    BH_Harness *h = AtRest();
    int count = Packets(packets);
    int i, snapshots;
    size_t length;

    printf("Test 8: truncations and stale bytes\n");
    /* A state every packet differs from: every fret and button, strum
       down, hat right, whammy and tilt at their ends */
    memset(other, 0, sizeof(other));
    other[0] = 0x3F;
    other[1] = 0x1E;
    other[2] = 0x02;
    other[4] = 0xFF;
    other[6] = 0xFF;
    other[19] = 0xFF;
    for (i = 0; i < count; ++i) {
        Feed(h, other, sizeof(other));
        before = *BH_Controls(h);
        snapshots = h->nsnapshots;
        for (length = 0; length < REPORT; ++length) {
            Feed(h, packets[i], length);
        }
        BH_CHECK(h->nsnapshots == snapshots && SDL_BLE_ControlsEqual(BH_Controls(h), &before),
                 "packet %d: a truncation changed the controls", i);
        Feed(h, packets[i], REPORT);
        BH_CHECK(h->nsnapshots == snapshots + 1, "packet %d: the full report changes the controls", i);
        expected = *BH_Controls(h);

        /* A longer value: the bytes after byte 19 are not part of the report */
        Feed(h, other, sizeof(other));
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, packets[i], REPORT);
        Feed(h, stale, sizeof(stale));
        BH_CHECK(SDL_BLE_ControlsEqual(BH_Controls(h), &expected), "packet %d: bytes after byte 19 changed the result", i);

        /* A buffer that holds more than the value: only length bytes are read */
        Feed(h, other, sizeof(other));
        SDL_BLEGHLiOSModule.Value(h->state, SDL_GHLIOS_INPUT, stale, REPORT, h->now * 1000000);
        BH_CHECK(SDL_BLE_ControlsEqual(BH_Controls(h), &expected), "packet %d: stale buffer bytes changed the result", i);
    }
    BH_Destroy(h);
}

static void Test9(void)
{
    static const char *const matches[] = { "Ble Guitar", "XBle Guitar 2", "Ble Guitar ", "My Ble Guitar" };
    static const char *const misses[] = { "BLE Guitar", "ble guitar", "Ble Guita", "Ble  Guitar", "Guitar", "" };
    SDL_BLEAdvertisement ad;
    uint8_t variant;
    size_t i;

    printf("Test 9: discovery\n");
    for (i = 0; i < COUNT(matches); ++i) {
        memset(&ad, 0, sizeof(ad));
        ad.address = guitar_address;
        ad.has_name = true;
        memcpy(ad.name, matches[i], strlen(matches[i]));
        variant = 0xAA;
        BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, &variant) == SDL_BLE_FAMILY_GHLIOS &&
                     variant == 0,
                 "the name \"%s\" matches the guitar", matches[i]);
    }
    for (i = 0; i < COUNT(misses); ++i) {
        memset(&ad, 0, sizeof(ad));
        ad.address = guitar_address;
        ad.has_name = true;
        memcpy(ad.name, misses[i], strlen(misses[i]));
        BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == -1,
                 "the name \"%s\" matches nothing", misses[i]);
    }

    /* The 16-bit service alone, in an advertisement or a scan response */
    memset(&ad, 0, sizeof(ad));
    ad.address = guitar_address;
    ad.nservices = 1;
    ad.services[0] = SDL_BLE_UUID16(0x1523);
    BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == SDL_BLE_FAMILY_GHLIOS,
             "service 1523 alone matches");
    ad.kind = SDL_BLE_AD_SCAN_RESPONSE;
    BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == SDL_BLE_FAMILY_GHLIOS,
             "service 1523 in a scan response matches");
    ad.services[0] = SDL_BLE_UUID16(0x1524);
    BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == -1, "service 1524 matches nothing");

    /* A name without has_name is no name */
    memset(&ad, 0, sizeof(ad));
    memcpy(ad.name, "Ble Guitar", 10);
    BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == -1, "an absent name matches nothing");
}

static void Test10(void)
{
    BH_Harness *h = AtRest();
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad;
    SDL_BLEMatch match;
    SDL_BLESink sink;
    SDL_BLEModuleContext context;
    uint8_t report[REPORT];
    const SDL_BLEControls *c;
    int before;

    printf("Test 10: link loss and reconnect\n");
    memset(&ad, 0, sizeof(ad));
    ad.address = guitar_address;
    ad.has_name = true;
    memcpy(ad.name, "Ble Guitar", 10);
    SDL_BLEHost_Init(&host);
    BH_CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, h->now, &match) &&
                 match.family == SDL_BLE_FAMILY_GHLIOS,
             "the first advertisement starts a session");
    BH_CHECK(!SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, h->now, &match),
             "a second advertisement during the session starts none");

    /* Everything held: frets, buttons, strum down, hat left, whammy, tilt */
    memset(report, 0, sizeof(report));
    report[0] = 0x3F;
    report[1] = 0x1E;
    report[2] = 0x06;
    report[4] = 0xFF;
    report[6] = 0xFF;
    report[19] = 0x00;
    Feed(h, report, sizeof(report));
    c = BH_Controls(h);
    BH_CHECK(c->buttons != 0 && c->axes[SDL_BLE_AXIS_LEFTY] == 32767 && c->axes[SDL_BLE_AXIS_RIGHTY] == -32768 &&
                 c->axes[SDL_BLE_AXIS_RIGHTX] == -32768,
             "everything is held before the loss");

    before = h->nactions;
    SDL_BLESession_Lost(&h->session, h->now);
    BH_Drain(h);
    c = BH_Controls(h);
    BH_CHECK(c->buttons == 0, "link loss releases every button");
    BH_CHECK(c->axes[SDL_BLE_AXIS_LEFTY] == 0 && c->axes[SDL_BLE_AXIS_RIGHTX] == 0 && c->axes[SDL_BLE_AXIS_RIGHTY] == 0,
             "link loss centers every axis");
    BH_CHECK(h->nactions == before + 2 && h->actions[before].action.kind == SDL_BLE_ACTION_REMOVE &&
                 h->actions[before + 1].action.kind == SDL_BLE_ACTION_DISCONNECT,
             "link loss removes the joystick and disconnects, without a backoff");
    BH_CHECK(SDL_BLESession_Ended(&h->session) && !h->published, "the session has ended");

    {
        SDL_BLEOutcome outcome;

        SDL_BLESession_GetOutcome(&h->session, &outcome);
        SDL_BLEHost_SessionEnded(&host, guitar_address, &outcome, h->now);
    }
    BH_CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, h->now, &match) &&
                 match.family == SDL_BLE_FAMILY_GHLIOS,
             "the next advertisement reconnects at once");

    /* The next session on the same state memory starts from nothing */
    memset(&context, 0, sizeof(context));
    context.variant = match.variant;
    sink.userdata = h;
    sink.changed = BH_Changed;
    sink.sensor = BH_Sensor;
    sink.log = BH_Log;
    SDL_BLESession_Init(&h->session, &SDL_BLEGHLiOSFamily, h->state, &sink, &context, false);
    BH_Drain(h);
    BH_CHECK(BH_LastKind(h) == SDL_BLE_ACTION_CONNECT, "the new session connects");
    BH_Start(h, false);
    BH_CHECK(!h->published, "the new session waits for a report");
    Rest(report);
    report[0] = 0x02;
    Feed(h, report, sizeof(report));
    BH_CHECK(h->published && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 2, "the new session publishes on its first report");
    BH_CHECK(BH_Controls(h)->buttons == BTN(SOUTH), "the new session decodes");
    BH_Destroy(h);
}

static void Test11(void)
{
    BH_Harness *h = Started();
    uint8_t report[REPORT];
    uint64_t deadline;
    int i, subscribes = 0;

    printf("Test 11: nothing written but the descriptor\n");
    BH_Advance(h, h->now + 5000);
    Rest(report);
    Feed(h, report, sizeof(report));
    for (i = 0; i < 100; ++i) {
        report[0] = (uint8_t)(i & 0x3F);
        report[4] = (i & 1) ? 0x00 : 0x80;
        Feed(h, report, sizeof(report));
        BH_Advance(h, h->now + 1000);
    }
    BH_Advance(h, h->now + 3600000);
    BH_CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline), "the guitar sets no timer");
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == 0 && BH_Count(h, SDL_BLE_ACTION_READ) == 0, "no write and no read");
    for (i = 0; i < h->nactions; ++i) {
        const SDL_BLEAction *a = &h->actions[i].action;

        if (a->kind == SDL_BLE_ACTION_SUBSCRIBE) {
            ++subscribes;
            BH_CHECK(a->characteristic == SDL_GHLIOS_INPUT && a->cccd == SDL_BLE_CCCD_NOTIFY,
                     "the one descriptor write asks the input characteristic for notifications");
        }
    }
    BH_CHECK(subscribes == 1, "one descriptor write, %d seen", subscribes);
    BH_CHECK(h->nactions == 4 && h->actions[0].action.kind == SDL_BLE_ACTION_CONNECT &&
                 h->actions[1].action.kind == SDL_BLE_ACTION_DISCOVER &&
                 h->actions[2].action.kind == SDL_BLE_ACTION_SUBSCRIBE &&
                 h->actions[3].action.kind == SDL_BLE_ACTION_PUBLISH,
             "connect, discover, subscribe and publish, then nothing");
    BH_CHECK(h->family->pairing == SDL_BLE_PAIR_NOT_NEEDED, "the guitar never pairs");
    BH_Destroy(h);

    /* A bonded guitar gets None before Notify and nothing more */
    h = BH_Create(&SDL_BLEGHLiOSFamily, 0, NULL, false);
    BH_Start(h, true);
    Rest(report);
    Feed(h, report, sizeof(report));
    BH_Advance(h, h->now + 3600000);
    BH_CHECK(h->nactions == 5 && h->actions[2].action.kind == SDL_BLE_ACTION_SUBSCRIBE &&
                 h->actions[2].action.cccd == SDL_BLE_CCCD_NONE && h->actions[3].action.kind == SDL_BLE_ACTION_SUBSCRIBE &&
                 h->actions[3].action.cccd == SDL_BLE_CCCD_NOTIFY && h->actions[4].action.kind == SDL_BLE_ACTION_PUBLISH,
             "a bonded guitar: None, Notify, publish");
    BH_Destroy(h);
}

/* Every hat value from 0 to 15, and the byte read whole above that */
static void TestHat(void)
{
    BH_Harness *h = AtRest();
    uint8_t report[REPORT];
    char what[64];
    int value;

    printf("Hat values\n");
    for (value = 0; value <= 0xFF; ++value) {
        RestWith(report, 2, (uint8_t)value);
        (void)snprintf(what, sizeof(what), "hat %02X", value);
        Expect(h, report, HatButtons(value), 0, 0, 0, what);
    }
    BH_Destroy(h);
}

/* The strum bar and the hat share the D-pad buttons */
static void TestStrumAndHat(void)
{
    static const uint8_t strums[3] = { 0x00, 0x80, 0xFF };
    BH_Harness *h = AtRest();
    uint8_t report[REPORT];
    char what[64];
    int s, hat;

    printf("Strum and hat together\n");
    for (s = 0; s < 3; ++s) {
        for (hat = 0; hat <= 0x0F; ++hat) {
            uint32_t buttons = HatButtons(hat);
            int lefty = 0;

            Rest(report);
            report[2] = (uint8_t)hat;
            report[4] = strums[s];
            if (strums[s] == 0x00) {
                buttons |= BTN(DPAD_UP);
                lefty = -32768;
            } else if (strums[s] == 0xFF) {
                buttons |= BTN(DPAD_DOWN);
                lefty = 32767;
            }
            (void)snprintf(what, sizeof(what), "strum %02X hat %02X", strums[s], hat);
            Expect(h, report, buttons, lefty, 0, 0, what);
        }
    }
    BH_Destroy(h);
}

/* Whammy and tilt at every byte value, which covers every boundary */
static void TestAxes(void)
{
    BH_Harness *h = AtRest();
    uint8_t report[REPORT];
    char what[64];
    int value;

    printf("Whammy and tilt\n");
    for (value = 0; value <= 0xFF; ++value) {
        RestWith(report, 6, (uint8_t)value);
        (void)snprintf(what, sizeof(what), "whammy %02X", value);
        Expect(h, report, 0, 0, 0, WhammyOf(value), what);
    }
    BH_CHECK(WhammyOf(0x81) == -258 && WhammyOf(0xFE) == -32509 && WhammyOf(0xFF) == -32768 && WhammyOf(0x00) == 0,
             "whammy boundaries");
    for (value = 0; value <= 0xFF; ++value) {
        RestWith(report, 19, (uint8_t)value);
        (void)snprintf(what, sizeof(what), "tilt %02X", value);
        Expect(h, report, 0, 0, TiltOf(value), 0, what);
    }

    /* Both at once, with a fret, keep apart */
    Rest(report);
    report[0] = 0x01;
    report[6] = 0xC0;
    report[19] = 0x40;
    Expect(h, report, BTN(WEST), 0, -16384, -16513, "fret, whammy and tilt together");
    BH_Destroy(h);
}

/* 19 bytes are ignored, 20 decode, a longer value decodes its first 20. A
   value on any other characteristic is not input. */
static void TestLengths(void)
{
    BH_Harness *h = AtRest();
    uint8_t report[REPORT + 44];
    int snapshots;

    printf("Lengths and characteristics\n");
    memset(report, 0, sizeof(report));
    Rest(report);
    report[0] = 0x08;
    snapshots = h->nsnapshots;
    Feed(h, report, REPORT - 1);
    BH_CHECK(h->nsnapshots == snapshots && BH_Controls(h)->buttons == 0, "19 bytes are ignored");
    Feed(h, report, REPORT);
    BH_CHECK(BH_Controls(h)->buttons == BTN(NORTH), "20 bytes decode");
    report[0] = 0x10;
    Feed(h, report, REPORT + 1);
    BH_CHECK(BH_Controls(h)->buttons == BTN(LEFT_SHOULDER), "21 bytes decode");
    report[0] = 0x20;
    Feed(h, report, sizeof(report));
    BH_CHECK(BH_Controls(h)->buttons == BTN(RIGHT_SHOULDER), "64 bytes decode");

    /* The family has one characteristic. The session drops an index past
       it, and the module ignores any index but its input. */
    report[0] = 0x01;
    snapshots = h->nsnapshots;
    BH_Value(h, 1, report, REPORT, false);
    SDL_BLEGHLiOSModule.Value(h->state, 1, report, REPORT, h->now * 1000000);
    SDL_BLEGHLiOSModule.Value(h->state, -1, report, REPORT, h->now * 1000000);
    BH_CHECK(h->nsnapshots == snapshots && BH_Controls(h)->buttons == BTN(RIGHT_SHOULDER),
             "values of other characteristics change nothing");

    /* The part's report is 20 bytes: a value of exactly 20 decodes */
    report[0] = 0x02;
    Feed(h, report, 20);
    BH_CHECK(BH_Controls(h)->buttons == BTN(SOUTH), "a value of exactly 20 bytes decodes");
    BH_Destroy(h);
}

/* A module is ready on its first decoded report. A truncated one is not
   decoded, and neither is a report before the start-up ends. */
static void TestReady(void)
{
    BH_Harness *h = BH_Create(&SDL_BLEGHLiOSFamily, 0, NULL, false);
    uint8_t report[REPORT];

    printf("Ready rule\n");
    Rest(report);
    BH_Start(h, false);
    Feed(h, report, REPORT - 1);
    BH_CHECK(!h->published && !((const SDL_BLEBase *)h->state)->ready, "a truncated report is not input");
    Feed(h, report, REPORT);
    BH_CHECK(h->published && ((const SDL_BLEBase *)h->state)->ready, "a full report publishes");
    BH_Destroy(h);

    /* A report that lands while the subscription is still out is decoded
       but is not input after the start-up: the start alone publishes
       nothing, and the first report after it publishes */
    h = BH_Create(&SDL_BLEGHLiOSFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    report[0] = 0x04;
    Feed(h, report, REPORT);
    BH_CHECK(!h->published && BH_Controls(h)->buttons == BTN(EAST) && !((const SDL_BLEBase *)h->state)->ready,
             "a report during the subscription is decoded, not ready");
    BH_SubscribeAll(h);
    BH_CHECK(!h->published && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0, "the start alone publishes nothing");
    Feed(h, report, REPORT);
    BH_CHECK(h->published && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1, "the first report after the start publishes");
    BH_Destroy(h);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    TestIdentity();
    Test1();
    Test2();
    Test3();
    Test4();
    Test5();
    Test6();
    Test7();
    Test8();
    Test9();
    Test10();
    Test11();
    TestHat();
    TestStrumAndHat();
    TestAxes();
    TestLengths();
    TestReady();
    return BH_Report("testbleghlios");
}
