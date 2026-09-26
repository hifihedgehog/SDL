/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/ble/SDL_ble_oculusgo_proto.c, the Oculus Go
   controller of hifihedgehog/SDL#33 Part 12. Test numbers follow the part's
   Oculus Go section. The blocks marked Decision cover what the part's tests
   leave open. No capture of this controller is published, so every report
   is built from the layout OculusGo-Air-Mouse-4-macOS maps in
   Sources/main.swift:106-131 and :670-672 (MIT), on firmware 2.7.1. */

#include "testbleharness.h"
#include "SDL_ble_oculusgo_proto.h"

#define CHECK(condition) BH_CHECK(condition, "%s", #condition)

#define REPORT SDL_OCULUSGO_REPORT_SIZE
#define FACE   SDL_OCULUSGO_FACE
#define BEEF   SDL_OCULUSGO_BEEF
#define MS     ((uint64_t)1000000)

/* Byte 19 of an 8126FACE report (main.swift:106-113) */
#define GO_OCULUS   0x01
#define GO_BACK     0x02
#define GO_TRIGGER  0x04
#define GO_CLICK    0x08
#define GO_TOUCHING 0x10

/* Blocks, so the log shows the checks of each test */
static const char *section;
static int section_checks;
static int section_failures;

static void Section(const char *name)
{
    if (section) {
        printf("%s: %d checks, %d failures\n", section, bh_checks - section_checks, bh_failures - section_failures);
    }
    section = name;
    section_checks = bh_checks;
    section_failures = bh_failures;
}

static void Put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

/* An 8126FACE report. The magnetometer and byte 18, which the module does
   not read, hold values of their own. */
static void Face(uint8_t *out, uint16_t counter, int16_t ax, int16_t ay, int16_t az, uint16_t x, uint16_t y,
                 uint8_t buttons)
{
    memset(out, 0, REPORT);
    Put16(out, counter);
    Put16(out + 2, (uint16_t)ax);
    Put16(out + 4, (uint16_t)ay);
    Put16(out + 6, (uint16_t)az);
    Put16(out + 8, 0x1234);
    Put16(out + 10, 0x5678);
    Put16(out + 12, 0x9ABC);
    Put16(out + 14, x);
    Put16(out + 16, y);
    out[18] = 0x2A;
    out[19] = buttons;
}

/* An 8126BEEF report: a counter and three gyroscope samples, sample k axis
   a holding 1000 x (a + 1) + 100 x k */
static void Beef(uint8_t *out, uint16_t counter)
{
    int k, axis;

    memset(out, 0, REPORT);
    Put16(out, counter);
    for (k = 0; k < 3; ++k) {
        for (axis = 0; axis < 3; ++axis) {
            Put16(out + 2 + 6 * k + 2 * axis, (uint16_t)(1000 * (axis + 1) + 100 * k));
        }
    }
}

/* Every button held and the finger down at (200, 100) */
static void Held(uint8_t *out)
{
    Face(out, 10, 0, 0, 127, 200, 100, GO_OCULUS | GO_BACK | GO_TRIGGER | GO_CLICK | GO_TOUCHING);
}

static bool AllHeld(const SDL_BLEControls *c)
{
    return c->buttons == ((1u << SDL_BLE_BUTTON_SOUTH) | (1u << SDL_BLE_BUTTON_BACK) | (1u << SDL_BLE_BUTTON_GUIDE)) &&
           c->finger && c->finger_x == 200.0f / 315.0f && c->finger_y == 100.0f / 315.0f &&
           c->axes[SDL_BLE_AXIS_LEFTX] == 8765 && c->axes[SDL_BLE_AXIS_LEFTY] == -12105 &&
           c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == 32767;
}

static bool AtRest(const SDL_BLEControls *c)
{
    return c->buttons == 0 && !c->finger && c->finger_x == 0.0f && c->finger_y == 0.0f &&
           c->axes[SDL_BLE_AXIS_LEFTX] == 0 && c->axes[SDL_BLE_AXIS_LEFTY] == 0 &&
           c->axes[SDL_BLE_AXIS_RIGHTX] == 0 && c->axes[SDL_BLE_AXIS_RIGHTY] == 0 &&
           c->axes[SDL_BLE_AXIS_LEFT_TRIGGER] == 0 && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768;
}

/* 81265652-3692-ae93-e711-270f223c83b3, 8126face-... and 8126beef-... */
static const SDL_BLEUUID service_uuid =
    SDL_BLE_UUID_INIT(0x81, 0x26, 0x56, 0x52, 0x36, 0x92, 0xae, 0x93, 0xe7, 0x11, 0x27, 0x0f, 0x22, 0x3c, 0x83, 0xb3);
static const SDL_BLEUUID face_uuid =
    SDL_BLE_UUID_INIT(0x81, 0x26, 0xfa, 0xce, 0x36, 0x92, 0xae, 0x93, 0xe7, 0x11, 0x27, 0x0f, 0x22, 0x3c, 0x83, 0xb3);
static const SDL_BLEUUID beef_uuid =
    SDL_BLE_UUID_INIT(0x81, 0x26, 0xbe, 0xef, 0x36, 0x92, 0xae, 0x93, 0xe7, 0x11, 0x27, 0x0f, 0x22, 0x3c, 0x83, 0xb3);

static BH_Harness *Create(bool pairing_allowed)
{
    return BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, pairing_allowed);
}

static SDL_OculusGoState *State(const BH_Harness *h)
{
    return (SDL_OculusGoState *)h->state;
}

/* A session started at start, both subscriptions written. A bonded device
   gets None before Notify on each. */
static BH_Harness *Started(uint64_t start, bool bonded)
{
    BH_Harness *h = Create(true);

    h->now = start;
    BH_Start(h, bonded);
    return h;
}

static void FaceValue(BH_Harness *h, const uint8_t *data, size_t length)
{
    BH_Value(h, FACE, data, length, false);
}

static void BeefValue(BH_Harness *h, const uint8_t *data, size_t length)
{
    BH_Value(h, BEEF, data, length, false);
}

/* Started, and published by a report of 20 zero bytes 10 ms later */
static BH_Harness *Published(uint64_t start, bool bonded)
{
    uint8_t report[REPORT];
    BH_Harness *h = Started(start, bonded);

    memset(report, 0, sizeof(report));
    BH_Advance(h, start + 10);
    FaceValue(h, report, REPORT);
    return h;
}

/* What a value that changes nothing leaves alone */
typedef struct Mark
{
    int nactions;
    int nsnapshots;
    int nsamples;
    int published;
    SDL_BLEControls controls;
    uint8_t state[sizeof(SDL_OculusGoState)];
} Mark;

static void Take(const BH_Harness *h, Mark *mark)
{
    mark->nactions = h->nactions;
    mark->nsnapshots = h->nsnapshots;
    mark->nsamples = h->nsamples;
    mark->published = h->published;
    mark->controls = *BH_Controls(h);
    memcpy(mark->state, h->state, sizeof(mark->state));
}

/* No action, no change, no sample */
static bool InputUnchanged(const BH_Harness *h, const Mark *mark)
{
    return h->nactions == mark->nactions && h->nsnapshots == mark->nsnapshots && h->nsamples == mark->nsamples &&
           h->published == mark->published && SDL_BLE_ControlsEqual(BH_Controls(h), &mark->controls);
}

/* The module state as well, its silence timer included */
static bool Unchanged(const BH_Harness *h, const Mark *mark)
{
    return InputUnchanged(h, mark) && memcmp(h->state, mark->state, sizeof(mark->state)) == 0;
}

/* Descriptor writes of one characteristic with one value, from log index
   from on */
static int Subscriptions(const BH_Harness *h, int from, int characteristic, uint8_t cccd)
{
    int i, count = 0;

    for (i = from; i < h->nactions; ++i) {
        const SDL_BLEAction *action = &h->actions[i].action;

        if (action->kind == SDL_BLE_ACTION_SUBSCRIBE && action->characteristic == characteristic &&
            action->cccd == cccd) {
            ++count;
        }
    }
    return count;
}

/* The four descriptor writes a bonded device gets from index on, in order */
static bool Resubscribed(const BH_Harness *h, int index)
{
    static const struct
    {
        int characteristic;
        uint8_t cccd;
    } order[4] = {
        { FACE, SDL_BLE_CCCD_NONE }, { FACE, SDL_BLE_CCCD_NOTIFY }, { BEEF, SDL_BLE_CCCD_NONE }, { BEEF, SDL_BLE_CCCD_NOTIFY }
    };
    int i;

    if (index < 0 || index + 4 > h->nactions) {
        return false;
    }
    for (i = 0; i < 4; ++i) {
        const SDL_BLEAction *action = &h->actions[index + i].action;

        if (action->kind != SDL_BLE_ACTION_SUBSCRIBE || action->characteristic != order[i].characteristic ||
            action->cccd != order[i].cccd) {
            return false;
        }
    }
    return true;
}

static int Writes(const BH_Harness *h)
{
    return BH_Count(h, SDL_BLE_ACTION_WRITE);
}

/* Decision: the identity the joystick is published with, and the family */
static void TestIdentity(void)
{
    BH_Harness *h = Create(true);
    const SDL_BLEIdentity *identity = &((const SDL_BLEBase *)h->state)->identity;
    const SDL_BLEFamily *family = &SDL_BLEOculusGoFamily;
    uint64_t deadline;

    Section("Decision: identity and family");
    CHECK(strcmp(identity->name, "Oculus Go Controller") == 0);
    CHECK(identity->type == SDL_BLE_TYPE_GAMEPAD && identity->gamepad && identity->touchpad);
    CHECK(identity->vendor == 0 && identity->product == 0);
    CHECK(identity->buttons == ((1u << SDL_BLE_BUTTON_SOUTH) | (1u << SDL_BLE_BUTTON_BACK) | (1u << SDL_BLE_BUTTON_GUIDE)));
    CHECK(identity->axes == (uint8_t)((1u << SDL_BLE_AXIS_LEFTX) | (1u << SDL_BLE_AXIS_LEFTY) | (1u << SDL_BLE_AXIS_RIGHT_TRIGGER)));
    /* No sensor: raw Y is the axis along the controller in the only
       reference (main.swift:90), so the raw axes are not SDL's frame, and
       the gyroscope's scale is unknown */
    CHECK(identity->accel_rate == 0.0f && identity->gyro_rate == 0.0f);
    CHECK(AtRest(BH_Controls(h)) && BH_Controls(h)->battery == -1);
    CHECK(!SDL_BLEOculusGoModule.GetDeadline(h->state, &deadline));

    CHECK(SDL_BLE_Families[SDL_BLE_FAMILY_OCULUSGO] == family && family->module == &SDL_BLEOculusGoModule);
    CHECK(SDL_BLEOculusGoModule.state_size == sizeof(SDL_OculusGoState));
    CHECK(family->nkeys == 2);
    CHECK(family->keys[0].kind == SDL_BLE_KEY_NAME_PREFIX && strcmp(family->keys[0].name, "OMVR") == 0 &&
          !family->keys[0].corroborating);
    CHECK(family->keys[1].kind == SDL_BLE_KEY_SERVICE && SDL_BLE_UUIDEqual(&family->keys[1].uuid, &service_uuid) &&
          !family->keys[1].corroborating);
    CHECK(family->nservices == 1 && SDL_BLE_UUIDEqual(&family->services[0], &service_uuid) && !family->has_alternate);
    CHECK(family->ncharacteristics == 2);
    CHECK(SDL_BLE_UUIDEqual(&family->characteristics[FACE].uuid, &face_uuid));
    CHECK(family->characteristics[FACE].flags == SDL_BLE_CHAR_SUBSCRIBE && family->characteristics[FACE].service == 0);
    CHECK(SDL_BLE_UUIDEqual(&family->characteristics[BEEF].uuid, &beef_uuid));
    /* 8126BEEF is optional (main.swift:641-644 subscribes whichever it finds) */
    CHECK(family->characteristics[BEEF].flags == (SDL_BLE_CHAR_SUBSCRIBE | SDL_BLE_CHAR_OPTIONAL) &&
          family->characteristics[BEEF].service == 0);
    CHECK(family->pairing == SDL_BLE_PAIR_ON_ERROR);
    BH_Destroy(h);
}

/* Test 1: 20 zero bytes on 8126FACE */
static void Test1(void)
{
    uint8_t report[REPORT];
    BH_Harness *h = Started(0, true);

    Section("Test 1: rest");
    CHECK(h->published == 0 && State(h)->started);
    memset(report, 0, sizeof(report));
    BH_Advance(h, 10);
    FaceValue(h, report, REPORT);
    /* No buttons, no finger, the sticks centered and the trigger at rest */
    CHECK(AtRest(BH_Controls(h)) && BH_Controls(h)->battery == -1);
    CHECK(h->nsnapshots == 0);
    CHECK(h->published == 1 && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1);
    CHECK(h->nsamples == 0);
    BH_Destroy(h);
}

/* Test 2: one button per bit of byte 19 */
static void Test2(void)
{
    static const struct
    {
        uint8_t byte19;
        int button; /* -1 for the trigger */
    } cases[] = {
        { GO_OCULUS, SDL_BLE_BUTTON_GUIDE },
        { GO_BACK, SDL_BLE_BUTTON_BACK },
        { GO_TRIGGER, -1 },
        { GO_CLICK, SDL_BLE_BUTTON_SOUTH },
    };
    uint8_t report[REPORT];
    size_t i;

    Section("Test 2: buttons");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        BH_Harness *h = Published(0, true);
        const SDL_BLEControls *c = BH_Controls(h);

        memset(report, 0, sizeof(report));
        report[19] = cases[i].byte19;
        BH_Advance(h, 20);
        FaceValue(h, report, REPORT);
        if (cases[i].button < 0) {
            CHECK(c->buttons == 0 && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == 32767);
        } else {
            CHECK(c->buttons == (1u << cases[i].button) && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768);
        }
        CHECK(!c->finger && c->axes[SDL_BLE_AXIS_LEFTX] == 0 && c->axes[SDL_BLE_AXIS_LEFTY] == 0);
        CHECK(h->nsnapshots == 1);
        /* And back to rest */
        report[19] = 0;
        FaceValue(h, report, REPORT);
        CHECK(AtRest(c) && h->nsnapshots == 2);
        BH_Destroy(h);
    }
}

/* Test 3: the touchpad */
static void Test3(void)
{
    uint8_t report[REPORT];
    BH_Harness *h = Published(0, true);
    const SDL_BLEControls *c = BH_Controls(h);

    Section("Test 3: touch");
    memset(report, 0, sizeof(report));
    report[14] = 0x9E;
    report[16] = 0x9E;
    report[19] = 0x10;
    BH_Advance(h, 20);
    FaceValue(h, report, REPORT);
    CHECK(c->finger && c->finger_x == 158.0f / 315.0f && c->finger_y == 158.0f / 315.0f);
    CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == 0 && c->axes[SDL_BLE_AXIS_LEFTY] == 0);
    CHECK(c->buttons == 0 && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768);
    /* 315 */
    report[14] = 0x3B;
    report[15] = 0x01;
    FaceValue(h, report, REPORT);
    CHECK(c->finger && c->axes[SDL_BLE_AXIS_LEFTX] == 32767 && c->finger_x == 1.0f);
    CHECK(c->axes[SDL_BLE_AXIS_LEFTY] == 0);
    /* 0, with the touching bit set */
    report[14] = 0x00;
    report[15] = 0x00;
    FaceValue(h, report, REPORT);
    CHECK(c->finger && c->axes[SDL_BLE_AXIS_LEFTX] == -32768 && c->finger_x == 0.0f);
    BH_Destroy(h);
}

/* Test 4: the coordinates of test 3 with bit 4 clear, each after a touch
   there, so a stick or a position left over from the touch would show */
static void Test4(void)
{
    static const uint8_t coordinates[3][4] = {
        { 0x9E, 0x00, 0x9E, 0x00 }, { 0x3B, 0x01, 0x9E, 0x00 }, { 0x00, 0x00, 0x9E, 0x00 }
    };
    uint8_t report[REPORT];
    BH_Harness *h = Published(0, true);
    const SDL_BLEControls *c = BH_Controls(h);
    int i;

    Section("Test 4: not touching");
    BH_Advance(h, 20);
    for (i = 0; i < 3; ++i) {
        memset(report, 0, sizeof(report));
        memcpy(report + 14, coordinates[i], 4);
        report[19] = 0x10;
        FaceValue(h, report, REPORT);
        CHECK(c->finger);
        report[19] = 0x00;
        FaceValue(h, report, REPORT);
        CHECK(!c->finger && c->finger_x == 0.0f && c->finger_y == 0.0f);
        CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == 0 && c->axes[SDL_BLE_AXIS_LEFTY] == 0);
        CHECK(AtRest(c));
    }
    BH_Destroy(h);
}

/* Test 5: the accelerometer is not exposed. Its bytes send no sample and
   move no control, whatever they hold, since raw Y is the axis along the
   controller (main.swift:90, README.md:121) and passing the raw axes through
   would put gravity off SDL's positive Y at rest. */
static void Test5(void)
{
    static const uint8_t cases[][6] = {
        { 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x00, 0x00, 0x7F, 0x00, 0x00, 0x00 },
        { 0x00, 0x00, 0x00, 0x00, 0x7F, 0x00 },
        { 0x81, 0xFF, 0x00, 0x00, 0x00, 0x00 },
        { 0xFF, 0x7F, 0x00, 0x80, 0x01, 0x00 },
    };
    uint8_t report[REPORT];
    size_t i;

    Section("Test 5: no accelerometer");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        BH_Harness *h = Published(0, true);

        memset(report, 0, sizeof(report));
        memcpy(report + 2, cases[i], 6);
        BH_Advance(h, 20);
        FaceValue(h, report, REPORT);
        CHECK(h->nsamples == 0);
        CHECK(AtRest(BH_Controls(h)) && h->nsnapshots == 0);
        BH_Destroy(h);
    }
}

/* Test 6: 8126BEEF values change no state */
static void Test6(void)
{
    uint8_t report[REPORT], beef[REPORT];
    BH_Harness *h;
    Mark mark;
    int k;

    Section("Test 6: 8126BEEF");
    h = Published(0, true);
    Held(report);
    BH_Advance(h, 20);
    FaceValue(h, report, REPORT);
    CHECK(AllHeld(BH_Controls(h)));
    Take(h, &mark);
    for (k = 0; k < 10; ++k) {
        Beef(beef, (uint16_t)(10 * k));
        BH_Advance(h, 31 + 11 * (uint64_t)k);
        BeefValue(h, beef, REPORT);
        CHECK(InputUnchanged(h, &mark));
    }
    /* A report of zeros, too */
    memset(beef, 0, sizeof(beef));
    BeefValue(h, beef, REPORT);
    CHECK(InputUnchanged(h, &mark));
    BH_Destroy(h);
}

/* Test 7: truncations, stale bytes and the characteristic that selects the
   decoder */
static void Test7(void)
{
    static const size_t longer[] = { 21, 32, 64, SDL_BLE_MAX_VALUE };
    uint8_t report[REPORT], beef[REPORT], stale[SDL_BLE_MAX_VALUE];
    BH_Harness *h;
    size_t length, i;
    Mark mark;

    Section("Test 7: truncations");
    h = Published(0, true);
    Held(report);
    BH_Advance(h, 20);
    FaceValue(h, report, REPORT);
    Take(h, &mark);
    Face(report, 20, 127, 0, 0, 0, 0, GO_BACK);
    Beef(beef, 20);
    for (length = 0; length < REPORT; ++length) {
        FaceValue(h, report, length);
        CHECK(Unchanged(h, &mark));
        BeefValue(h, beef, length);
        CHECK(Unchanged(h, &mark));
    }
    BH_Destroy(h);

    /* Bytes after byte 19, in a longer value or in the buffer, are ignored */
    Face(report, 20, 127, -3, 5, 250, 60, GO_TRIGGER | GO_TOUCHING);
    for (i = 0; i <= sizeof(longer) / sizeof(longer[0]); ++i) {
        BH_Harness *exact = Published(0, true);
        BH_Harness *padded = Published(0, true);

        BH_Advance(exact, 20);
        BH_Advance(padded, 20);
        FaceValue(exact, report, REPORT);
        memset(stale, 0xFF, sizeof(stale));
        memcpy(stale, report, REPORT);
        if (i < sizeof(longer) / sizeof(longer[0])) {
            FaceValue(padded, stale, longer[i]);
        } else {
            /* Straight to the session, so the buffer holds bytes past the
               length */
            SDL_BLESession_Value(&padded->session, FACE, stale, REPORT, false, padded->now * MS);
            BH_Drain(padded);
        }
        CHECK(SDL_BLE_ControlsEqual(BH_Controls(exact), BH_Controls(padded)));
        CHECK(exact->nsnapshots == 1 && padded->nsnapshots == 1 && exact->nsamples == 0 && padded->nsamples == 0);
        CHECK(State(exact)->deadline == State(padded)->deadline && State(exact)->silent == State(padded)->silent);
        BH_Destroy(exact);
        BH_Destroy(padded);
    }

    /* The characteristic, not the content, selects the decoder */
    h = Published(0, true);
    BH_Advance(h, 20);
    Take(h, &mark);
    Held(report);
    BeefValue(h, report, REPORT);
    CHECK(InputUnchanged(h, &mark));
    /* A gyroscope report on 8126FACE reads as buttons: byte 19 is 0C, the
       trigger and the touchpad click */
    Beef(beef, 0);
    FaceValue(h, beef, REPORT);
    CHECK(BH_Controls(h)->buttons == (1u << SDL_BLE_BUTTON_SOUTH));
    CHECK(BH_Controls(h)->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == 32767 && !BH_Controls(h)->finger);
    CHECK(h->nsamples == 0);
    BH_Destroy(h);
}

static bool LastIsSubscribe(const BH_Harness *h, int characteristic, uint8_t cccd)
{
    const SDL_BLEAction *action = BH_Last(h);

    return action && action->kind == SDL_BLE_ACTION_SUBSCRIBE && action->characteristic == characteristic &&
           action->cccd == cccd && h->session.waiting;
}

static void Advertisement(SDL_BLEAdvertisement *ad, uint64_t address, uint8_t kind)
{
    memset(ad, 0, sizeof(*ad));
    ad->address = address;
    ad->kind = kind;
}

static void AddName(SDL_BLEAdvertisement *ad, const char *name)
{
    size_t length = strlen(name);

    if (length >= sizeof(ad->name)) {
        length = sizeof(ad->name) - 1;
    }
    memcpy(ad->name, name, length);
    ad->name[length] = '\0';
    ad->has_name = true;
}

static void AddService(SDL_BLEAdvertisement *ad, const SDL_BLEUUID *uuid)
{
    if (ad->nservices < SDL_BLE_AD_SERVICES) {
        ad->services[ad->nservices++] = *uuid;
    }
}

/* Test 8: pairing */
static void Test8(void)
{
    uint8_t report[REPORT];
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad;
    SDL_BLEMatch match;
    BH_Harness *h;

    Section("Test 8: pairing");
    /* InsufficientEncryption on the first descriptor write, with the hint
       on: pair, then one repeat, which the new bond starts with None */
    h = Create(true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    CHECK(LastIsSubscribe(h, FACE, SDL_BLE_CCCD_NOTIFY));
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    CHECK(BH_LastKind(h) == SDL_BLE_ACTION_PAIR);
    BH_Paired(h, true);
    CHECK(LastIsSubscribe(h, FACE, SDL_BLE_CCCD_NONE));
    BH_SubscribeAll(h);
    CHECK(Subscriptions(h, 0, FACE, SDL_BLE_CCCD_NOTIFY) == 2 && Subscriptions(h, 0, FACE, SDL_BLE_CCCD_NONE) == 1);
    CHECK(Subscriptions(h, 0, BEEF, SDL_BLE_CCCD_NOTIFY) == 1 && Subscriptions(h, 0, BEEF, SDL_BLE_CCCD_NONE) == 1);
    CHECK(BH_Count(h, SDL_BLE_ACTION_PAIR) == 1 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 0);
    CHECK(State(h)->started && Writes(h) == 0);
    memset(report, 0, sizeof(report));
    FaceValue(h, report, REPORT);
    CHECK(h->published == 1);
    BH_Destroy(h);

    /* A second failure, in the repeat, backs off */
    h = Create(true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    BH_Paired(h, true);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    CHECK(BH_Count(h, SDL_BLE_ACTION_PAIR) == 1 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1);
    CHECK(BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT && SDL_BLESession_Ended(&h->session));
    BH_Destroy(h);

    /* A failed pairing leaves the device to the next advertisement, with no
       backoff */
    SDL_BLEHost_Init(&host);
    Advertisement(&ad, 0x2CC1D7E6F501, SDL_BLE_AD_ADVERTISEMENT);
    AddName(&ad, "OMVR-V190");
    CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 0, &match));
    CHECK(match.family == SDL_BLE_FAMILY_OCULUSGO && match.variant == 0);
    h = Create(true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    CHECK(BH_LastKind(h) == SDL_BLE_ACTION_PAIR);
    BH_Paired(h, false);
    CHECK(BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT && SDL_BLESession_Ended(&h->session));
    CHECK(BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 0 && !h->session.backoff && !h->session.published);
    {
        SDL_BLEOutcome outcome;

        SDL_BLESession_GetOutcome(&h->session, &outcome);
        SDL_BLEHost_SessionEnded(&host, ad.address, &outcome, 100);
    }
    CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 101, &match));
    BH_Destroy(h);

    /* With the hint off, no pairing and a backoff */
    h = Create(false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    CHECK(BH_Count(h, SDL_BLE_ACTION_PAIR) == 0 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1);
    CHECK(BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT && SDL_BLESession_Ended(&h->session));
    BH_Destroy(h);
}

/* Test 9: 3 s without data on a live link */
static void Test9(void)
{
    uint8_t held[REPORT];
    BH_Harness *h = Started(0, true);
    const SDL_BLEControls *c = BH_Controls(h);
    int from, n;

    Section("Test 9: silence");
    Held(held);
    BH_Advance(h, 100);
    FaceValue(h, held, REPORT);
    CHECK(h->published == 1 && AllHeld(c));
    from = h->nactions;
    n = h->nsnapshots;
    BH_Advance(h, 3099);
    CHECK(h->nactions == from && h->nsnapshots == n && AllHeld(c));
    BH_Advance(h, 3100);
    /* Every held control released at 3 s */
    CHECK(h->nsnapshots == n + 1 && AtRest(c) && h->snapshots[n].time_ns == 3100 * MS);
    /* Both characteristics subscribed again, once, None first on the bond */
    CHECK(LastIsSubscribe(h, FACE, SDL_BLE_CCCD_NONE));
    BH_SubscribeAll(h);
    CHECK(h->nactions == from + 4 && Resubscribed(h, from));
    CHECK(Subscriptions(h, from, FACE, SDL_BLE_CCCD_NOTIFY) == 1 && Subscriptions(h, from, BEEF, SDL_BLE_CCCD_NOTIFY) == 1);
    /* The joystick stays */
    CHECK(h->published == 1 && BH_Count(h, SDL_BLE_ACTION_REMOVE) == 0 && !SDL_BLESession_Ended(&h->session));
    CHECK(Writes(h) == 0);
    /* The next report brings the controls back */
    BH_Advance(h, 3200);
    FaceValue(h, held, REPORT);
    CHECK(AllHeld(c) && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1);
    BH_Destroy(h);
}

/* Decision: the silence as the macOS app runs it (main.swift:646-655): the
   first request 3 s after the last value, then one every 1 s, the app's
   check period, while the silence lasts. One release per silence, none
   while values flow, and gyroscope values count as data, as
   main.swift:667-668 counts any value of 20 bytes or more. */
static void TestSilence(void)
{
    uint8_t held[REPORT], beef[REPORT];
    BH_Harness *h;
    const SDL_BLEControls *c;
    uint64_t t;
    int k, from, n;

    Section("Decision: silence");
    CHECK(SDL_OCULUSGO_SILENCE_MS == 3000 && SDL_OCULUSGO_RESUBSCRIBE_MS == 1000);
    Held(held);
    Beef(beef, 0);
    h = Started(0, true);
    c = BH_Controls(h);
    BH_Advance(h, 100);
    FaceValue(h, held, REPORT);
    n = h->nsnapshots;
    for (k = 0; k < 4; ++k) {
        const uint64_t at = 3100 + 1000 * (uint64_t)k;

        from = h->nactions;
        BH_Advance(h, at - 1);
        CHECK(h->nactions == from);
        BH_Advance(h, at);
        CHECK(h->nactions == from + 1 && h->actions[from].now == at);
        BH_SubscribeAll(h);
        CHECK(h->nactions == from + 4 && Resubscribed(h, from));
    }
    /* One release for the whole silence */
    CHECK(h->nsnapshots == n + 1 && AtRest(c) && h->published == 1 && h->snapshots[n].time_ns == 3100 * MS);
    /* While reports flow, no request */
    from = h->nactions;
    for (t = 6200; t <= 16200; t += 11) {
        BH_Advance(h, t);
        FaceValue(h, held, REPORT);
    }
    t -= 11;
    CHECK(h->nactions == from && AllHeld(c) && h->nsnapshots == n + 2);
    /* The next silence releases again, 3 s after the last report */
    BH_Advance(h, t + 2999);
    CHECK(h->nactions == from && AllHeld(c));
    BH_Advance(h, t + 3000);
    CHECK(AtRest(c) && h->nsnapshots == n + 3);
    BH_SubscribeAll(h);
    CHECK(h->nactions == from + 4 && Resubscribed(h, from));
    BH_Destroy(h);

    /* Gyroscope values alone hold the silence off */
    h = Started(0, true);
    c = BH_Controls(h);
    BH_Advance(h, 100);
    FaceValue(h, held, REPORT);
    from = h->nactions;
    n = h->nsnapshots;
    for (t = 111; t <= 10111; t += 11) {
        BH_Advance(h, t);
        BeefValue(h, beef, REPORT);
    }
    t -= 11;
    CHECK(h->nactions == from && h->nsnapshots == n && AllHeld(c));
    BH_Advance(h, t + 2999);
    CHECK(h->nactions == from && AllHeld(c));
    BH_Advance(h, t + 3000);
    CHECK(AtRest(c) && h->nsnapshots == n + 1);
    BH_SubscribeAll(h);
    CHECK(h->nactions == from + 4 && Resubscribed(h, from));
    BH_Destroy(h);

    /* Values shorter than 20 bytes are no data */
    h = Started(0, true);
    c = BH_Controls(h);
    BH_Advance(h, 100);
    FaceValue(h, held, REPORT);
    from = h->nactions;
    for (t = 111; t < 3100; t += 11) {
        BH_Advance(h, t);
        FaceValue(h, held, REPORT - 1);
        BeefValue(h, beef, REPORT - 1);
    }
    BH_Advance(h, 3100);
    CHECK(AtRest(c));
    BH_SubscribeAll(h);
    CHECK(h->nactions == from + 4 && Resubscribed(h, from));
    BH_Destroy(h);

    /* Silent from the start: the request comes 3 s after the subscriptions,
       the next 1 s later, and nothing is published */
    h = Started(0, true);
    from = h->nactions;
    BH_Advance(h, 2999);
    CHECK(h->nactions == from);
    BH_Advance(h, 3000);
    BH_SubscribeAll(h);
    CHECK(h->nactions == from + 4 && Resubscribed(h, from));
    BH_Advance(h, 3999);
    CHECK(h->nactions == from + 4);
    BH_Advance(h, 4000);
    BH_SubscribeAll(h);
    CHECK(h->nactions == from + 8 && Resubscribed(h, from + 4));
    CHECK(h->published == 0 && h->nsnapshots == 0);
    BH_Destroy(h);

    /* Without a bond each characteristic gets Notify alone */
    h = Started(0, false);
    c = BH_Controls(h);
    BH_Advance(h, 100);
    FaceValue(h, held, REPORT);
    from = h->nactions;
    BH_Advance(h, 3100);
    BH_SubscribeAll(h);
    CHECK(h->nactions == from + 2 && AtRest(c));
    CHECK(Subscriptions(h, from, FACE, SDL_BLE_CCCD_NOTIFY) == 1 && Subscriptions(h, from, BEEF, SDL_BLE_CCCD_NOTIFY) == 1);
    BH_Destroy(h);

    /* Only an 8126FACE value clears the release made for a silence
       (main.swift:674-675). A gyroscope value holds the next request off
       for 3 s (main.swift:667-668) and leaves the release in place. */
    h = Started(0, true);
    c = BH_Controls(h);
    BH_Advance(h, 100);
    FaceValue(h, held, REPORT);
    BH_Advance(h, 3100);
    BH_SubscribeAll(h);
    CHECK(AtRest(c) && State(h)->silent && State(h)->deadline == 4100);
    BH_Advance(h, 3500);
    BeefValue(h, beef, REPORT);
    CHECK(State(h)->silent && State(h)->deadline == 6500);
    from = h->nactions;
    BH_Advance(h, 6499);
    CHECK(h->nactions == from);
    BH_Advance(h, 6500);
    BH_SubscribeAll(h);
    CHECK(h->nactions == from + 4 && Resubscribed(h, from) && State(h)->silent && AtRest(c));
    FaceValue(h, held, REPORT);
    CHECK(AllHeld(c) && !State(h)->silent);
    BH_Destroy(h);

    /* A late check sets the next one 1 s after itself */
    h = Started(0, true);
    BH_Advance(h, 100);
    FaceValue(h, held, REPORT);
    from = h->nactions;
    h->now = 3600;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    BH_SubscribeAll(h);
    CHECK(h->nactions == from + 4 && Resubscribed(h, from) && State(h)->deadline == 4600);
    BH_Destroy(h);
}

/* Decision: the silence at its edges */
static void TestSilenceEdges(void)
{
    uint8_t face[REPORT], beef[REPORT];
    uint64_t deadline;
    BH_Harness *h;
    int subscriptions;

    Section("Decision: silence at its edges");
    /* A value on an index the family lacks decodes nothing and holds no
       check off, before a silence or during one */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    memset(face, 0, sizeof(face));
    face[19] = GO_CLICK;
    SDL_BLEOculusGoModule.Value(h->state, 2, face, sizeof(face), 2000 * MS);
    CHECK(!BH_Button(h, SDL_BLE_BUTTON_SOUTH));
    CHECK(SDL_BLEOculusGoModule.GetDeadline(h->state, &deadline) && deadline == 4000);
    BH_Advance(h, 4000);
    BH_SubscribeAll(h);
    CHECK(State(h)->silent && State(h)->deadline == 5000);
    SDL_BLEOculusGoModule.Value(h->state, 2, face, sizeof(face), 4500 * MS);
    CHECK(!BH_Button(h, SDL_BLE_BUTTON_SOUTH) && State(h)->silent && State(h)->deadline == 5000);
    BH_Destroy(h);

    /* The silence acts at 3 s, not a millisecond before, and the next
       request comes at 1 s, not a millisecond before */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    BH_Value(h, SDL_OCULUSGO_FACE, face, sizeof(face), false);
    CHECK(h->published && BH_Button(h, SDL_BLE_BUTTON_SOUTH));
    subscriptions = BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE);
    h->now = 3999;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(BH_Button(h, SDL_BLE_BUTTON_SOUTH) && BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == subscriptions);
    h->now = 4000;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(!BH_Button(h, SDL_BLE_BUTTON_SOUTH) && BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == subscriptions + 1);
    BH_SubscribeAll(h);
    subscriptions = BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE);
    h->now = 4999;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == subscriptions);
    h->now = 5000;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == subscriptions + 1);
    BH_Destroy(h);

    /* No silence before the start */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    SDL_BLEOculusGoModule.Tick(h->state, 5000);
    CHECK(!((const SDL_BLEBase *)h->state)->resubscribe && h->nsnapshots == 0 && !State(h)->silent);
    BH_Destroy(h);

    /* A gyroscope value is data during a silence too: it holds the next
       request off for 3 s (main.swift:667-668) */
    h = Started(0, false);
    Beef(beef, 0);
    BH_Advance(h, 3000);
    BH_SubscribeAll(h);
    BH_Advance(h, 3200);
    BeefValue(h, beef, REPORT);
    subscriptions = BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE);
    BH_Advance(h, 6199);
    CHECK(BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == subscriptions);
    BH_Advance(h, 6200);
    CHECK(BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == subscriptions + 1);
    BH_Destroy(h);
}

/* Decision: 8126BEEF is optional, since nothing is decoded from it and the
   macOS app subscribes to whichever of the two it finds (main.swift:641-644).
   Missing, or failing its descriptor write, it is skipped. */
static void TestOptionalBeef(void)
{
    bool found[SDL_BLE_MAX_CHARS];
    uint8_t properties[SDL_BLE_MAX_CHARS];
    uint8_t report[REPORT];
    BH_Harness *h;
    int i, from;

    Section("Decision: optional 8126BEEF");
    memset(report, 0, sizeof(report));
    /* Missing at discovery: 8126FACE alone is subscribed, the joystick
       appears with its first report, and a silence writes its descriptor
       alone */
    for (i = 0; i < SDL_BLE_MAX_CHARS; ++i) {
        found[i] = (i != BEEF);
    }
    BH_DefaultProperties(&SDL_BLEOculusGoFamily, properties);
    h = Create(true);
    BH_Connected(h, true, false);
    SDL_BLESession_Discovered(&h->session, true, found, properties, h->now);
    BH_Drain(h);
    BH_SubscribeAll(h);
    CHECK(Subscriptions(h, 0, FACE, SDL_BLE_CCCD_NOTIFY) == 1 && BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == 1);
    CHECK(State(h)->started && !SDL_BLESession_Ended(&h->session) && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 0);
    FaceValue(h, report, REPORT);
    CHECK(h->published == 1);
    from = h->nactions;
    BH_Advance(h, 3000);
    BH_SubscribeAll(h);
    CHECK(h->nactions == from + 1 && Subscriptions(h, from, FACE, SDL_BLE_CCCD_NOTIFY) == 1);
    BH_Destroy(h);

    /* Present, but its descriptor write fails, even for want of
       authentication: skipped, with no pairing and no backoff */
    h = Create(true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    CHECK(LastIsSubscribe(h, BEEF, SDL_BLE_CCCD_NOTIFY));
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION);
    CHECK(BH_Count(h, SDL_BLE_ACTION_PAIR) == 0 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 0 && State(h)->started);
    FaceValue(h, report, REPORT);
    CHECK(h->published == 1);
    BH_Destroy(h);
}

/* Test 10: link loss and the next advertisement */
static void Test10(void)
{
    uint8_t held[REPORT];
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad;
    SDL_BLEMatch match;
    uint64_t deadline;
    BH_Harness *h;
    const SDL_BLEControls *c;
    int n;

    Section("Test 10: link loss");
    SDL_BLEHost_Init(&host);
    Advertisement(&ad, 0x2CC1D7E6F501, SDL_BLE_AD_ADVERTISEMENT);
    AddName(&ad, "OMVR-V190");
    CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 0, &match));
    CHECK(match.family == SDL_BLE_FAMILY_OCULUSGO);
    CHECK(!SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 10, &match));

    h = Published(0, true);
    c = BH_Controls(h);
    Held(held);
    BH_Advance(h, 20);
    FaceValue(h, held, REPORT);
    CHECK(AllHeld(c));
    BH_Advance(h, 1000);
    n = h->nactions;
    SDL_BLESession_Lost(&h->session, h->now);
    BH_Drain(h);
    /* Every button released and the finger lifted */
    CHECK(AtRest(c));
    CHECK(h->nsnapshots >= 1 && SDL_BLE_ControlsEqual(&h->snapshots[h->nsnapshots - 1].controls, c));
    CHECK(h->snapshots[h->nsnapshots - 1].time_ns == 1000 * MS);
    CHECK(h->nactions == n + 2 && h->actions[n].action.kind == SDL_BLE_ACTION_REMOVE);
    CHECK(h->actions[n + 1].action.kind == SDL_BLE_ACTION_DISCONNECT);
    CHECK(h->published == 0 && SDL_BLESession_Ended(&h->session) && !h->session.backoff);
    /* No timer after the loss */
    CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline));
    BH_Advance(h, 60000);
    CHECK(h->nactions == n + 2);
    {
        SDL_BLEOutcome outcome;

        SDL_BLESession_GetOutcome(&h->session, &outcome);
        SDL_BLEHost_SessionEnded(&host, ad.address, &outcome, h->now);
    }
    BH_Destroy(h);

    /* The next advertisement reconnects */
    CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 60001, &match));
    CHECK(match.family == SDL_BLE_FAMILY_OCULUSGO);
    h = Started(60001, true);
    CHECK(Resubscribed(h, 2) && State(h)->started && h->published == 0);
    CHECK(AtRest(BH_Controls(h)) && Writes(h) == 0);
    BH_Advance(h, 60011);
    FaceValue(h, held, REPORT);
    CHECK(h->published == 1 && AllHeld(BH_Controls(h)));
    BH_Destroy(h);
}

/* Test 11: nothing but pairing and the descriptors, on an injected clock */
static void Test11(void)
{
    uint8_t report[REPORT], beef[REPORT];
    BH_Harness *h = Create(true);
    uint64_t t;
    int i, others = 0, subscriptions_elsewhere = 0;

    Section("Test 11: no writes");
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    BH_Paired(h, true);
    BH_SubscribeAll(h);
    Held(report);
    Beef(beef, 0);
    for (t = 10; t < 5000; t += 11) {
        BH_Advance(h, t);
        FaceValue(h, report, REPORT);
        BeefValue(h, beef, REPORT);
    }
    /* 10 s of silence, its requests answered */
    for (t = 5000; t <= 15000; t += 500) {
        BH_Advance(h, t);
        BH_SubscribeAll(h);
    }
    SDL_BLESession_Close(&h->session, h->now);
    BH_Drain(h);
    for (i = 0; i < h->nactions; ++i) {
        const SDL_BLEAction *action = &h->actions[i].action;

        switch (action->kind) {
        case SDL_BLE_ACTION_CONNECT:
        case SDL_BLE_ACTION_PAIR:
        case SDL_BLE_ACTION_DISCOVER:
        case SDL_BLE_ACTION_PUBLISH:
        case SDL_BLE_ACTION_REMOVE:
        case SDL_BLE_ACTION_DISCONNECT:
            break;
        case SDL_BLE_ACTION_SUBSCRIBE:
            if (action->characteristic != FACE && action->characteristic != BEEF) {
                ++subscriptions_elsewhere;
            }
            break;
        default:
            ++others;
            break;
        }
    }
    CHECK(others == 0 && subscriptions_elsewhere == 0);
    CHECK(Writes(h) == 0 && BH_Count(h, SDL_BLE_ACTION_READ) == 0);
    CHECK(BH_Count(h, SDL_BLE_ACTION_PAIR) == 1 && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1);
    /* The last values came at 4993 ms, so the silence brings eight requests
       by 15000 ms, at 7993 ms and every 1 s after, four descriptor writes
       each */
    CHECK(BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == 5 + 8 * 4);
    CHECK(SDL_BLESession_Ended(&h->session) && BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT);
    BH_Destroy(h);
}

/* Decision: each bit of byte 19 alone. Bits 5 to 7 are not read. */
static void TestButtonBits(void)
{
    uint8_t report[REPORT];
    int bit;

    Section("Decision: button bits");
    for (bit = 0; bit < 8; ++bit) {
        BH_Harness *h = Published(0, true);
        const SDL_BLEControls *c = BH_Controls(h);
        const int n = h->nsnapshots;

        Face(report, 10, 0, 0, 127, 0, 0, (uint8_t)(1 << bit));
        BH_Advance(h, 20);
        FaceValue(h, report, REPORT);
        switch (bit) {
        case 0:
            CHECK(c->buttons == (1u << SDL_BLE_BUTTON_GUIDE) && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768 && !c->finger);
            break;
        case 1:
            CHECK(c->buttons == (1u << SDL_BLE_BUTTON_BACK) && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768 && !c->finger);
            break;
        case 2:
            CHECK(c->buttons == 0 && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == 32767 && !c->finger);
            break;
        case 3:
            CHECK(c->buttons == (1u << SDL_BLE_BUTTON_SOUTH) && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768 && !c->finger);
            break;
        case 4:
            /* Touching at (0, 0) */
            CHECK(c->buttons == 0 && c->finger && c->finger_x == 0.0f && c->finger_y == 0.0f);
            CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == -32768 && c->axes[SDL_BLE_AXIS_LEFTY] == -32768);
            break;
        default:
            CHECK(h->nsnapshots == n && AtRest(c));
            break;
        }
        BH_Destroy(h);
    }
}

/* Decision: the stick and the finger position clamp */
static void TestTouchClamps(void)
{
    static const struct
    {
        uint16_t x, y;
        int16_t stick_x, stick_y;
        float finger_x, finger_y;
    } cases[] = {
        { 0xFFFF, 0xFFFF, 32767, 32767, 1.0f, 1.0f },
        { 316, 158, 32767, 0, 1.0f, 158.0f / 315.0f },
        { 1200, 0, 32767, -32768, 1.0f, 0.0f },
        { 157, 159, -208, 208, 157.0f / 315.0f, 159.0f / 315.0f },
        { 1, 315, -32767, 32767, 1.0f / 315.0f, 1.0f },
    };
    uint8_t report[REPORT];
    BH_Harness *h = Published(0, true);
    const SDL_BLEControls *c = BH_Controls(h);
    size_t i;

    Section("Decision: touch clamps");
    BH_Advance(h, 20);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Face(report, 10, 0, 0, 127, cases[i].x, cases[i].y, GO_TOUCHING);
        FaceValue(h, report, REPORT);
        CHECK(c->finger);
        CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == cases[i].stick_x && c->axes[SDL_BLE_AXIS_LEFTY] == cases[i].stick_y);
        CHECK(c->finger_x == cases[i].finger_x && c->finger_y == cases[i].finger_y);
        CHECK(c->finger_x >= 0.0f && c->finger_x <= 1.0f && c->finger_y >= 0.0f && c->finger_y <= 1.0f);
    }
    BH_Destroy(h);
}

/* Decision: the joystick appears with the first 8126FACE report after the
   subscriptions. Reports before are decoded, and gyroscope reports are not
   input. */
static void TestReady(void)
{
    uint8_t report[REPORT], beef[REPORT];
    BH_Harness *h = Create(true);
    const SDL_BLEControls *c = BH_Controls(h);

    Section("Decision: ready");
    Face(report, 0, 0, 0, 127, 0, 0, GO_TRIGGER);
    Beef(beef, 0);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    CHECK(LastIsSubscribe(h, BEEF, SDL_BLE_CCCD_NONE));
    /* A report while 8126BEEF is subscribed */
    FaceValue(h, report, REPORT);
    CHECK(c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == 32767 && h->nsamples == 0);
    CHECK(h->published == 0 && !State(h)->started && !State(h)->base.ready);
    BH_SubscribeAll(h);
    CHECK(State(h)->started && h->published == 0);
    /* A gyroscope report publishes nothing */
    BH_Advance(h, 10);
    BeefValue(h, beef, REPORT);
    CHECK(h->published == 0 && !State(h)->base.ready);
    /* The first report after the start publishes, once */
    BH_Advance(h, 20);
    FaceValue(h, report, REPORT);
    CHECK(h->published == 1 && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1);
    CHECK(BH_LastKind(h) == SDL_BLE_ACTION_PUBLISH && h->actions[h->nactions - 1].now == 20);
    FaceValue(h, report, REPORT);
    CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1);
    BH_Destroy(h);
}

/* Decision: a gap releases every held control before the value after it
   applies, on either characteristic */
static void TestGap(void)
{
    uint8_t held[REPORT], report[REPORT], beef[REPORT];
    BH_Harness *h = Published(0, true);
    const SDL_BLEControls *c = BH_Controls(h);
    int n;

    Section("Decision: gap");
    Held(held);
    BH_Advance(h, 20);
    FaceValue(h, held, REPORT);
    CHECK(AllHeld(c));
    n = h->nsnapshots;
    Face(report, 20, 0, 0, 127, 0, 0, GO_BACK);
    BH_Advance(h, 31);
    BH_Value(h, FACE, report, REPORT, true);
    CHECK(h->nsnapshots == n + 2 && AtRest(&h->snapshots[n].controls));
    CHECK(h->snapshots[n].time_ns == 31 * MS);
    CHECK(c->buttons == (1u << SDL_BLE_BUTTON_BACK) && !c->finger);
    /* On a gyroscope value the release is all that happens */
    FaceValue(h, held, REPORT);
    CHECK(AllHeld(c));
    n = h->nsnapshots;
    Beef(beef, 30);
    BH_Advance(h, 42);
    BH_Value(h, BEEF, beef, REPORT, true);
    CHECK(h->nsnapshots == n + 1 && AtRest(c));
    BH_Destroy(h);
}

/* Decision: discovery by a name starting with OMVR or by the service
   (main.swift:589) */
static void TestDiscovery(void)
{
    static const char *const misses[] = { "omvr-v190", "OMV", "XOMVR-V190", "Oculus Go", "" };
    SDL_BLEAdvertisement ad;
    uint8_t variant;
    size_t i;

    Section("Decision: discovery");
    Advertisement(&ad, 1, SDL_BLE_AD_ADVERTISEMENT);
    AddName(&ad, "OMVR-V190");
    variant = 0xFF;
    CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, &variant) == SDL_BLE_FAMILY_OCULUSGO && variant == 0);
    Advertisement(&ad, 1, SDL_BLE_AD_SCAN_RESPONSE);
    AddName(&ad, "OMVR");
    CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == SDL_BLE_FAMILY_OCULUSGO);
    Advertisement(&ad, 1, SDL_BLE_AD_ADVERTISEMENT);
    AddService(&ad, &service_uuid);
    CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == SDL_BLE_FAMILY_OCULUSGO);
    for (i = 0; i < sizeof(misses) / sizeof(misses[0]); ++i) {
        Advertisement(&ad, 1, SDL_BLE_AD_ADVERTISEMENT);
        AddName(&ad, misses[i]);
        CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == -1);
    }
    /* A characteristic UUID is not the service */
    Advertisement(&ad, 1, SDL_BLE_AD_ADVERTISEMENT);
    AddService(&ad, &face_uuid);
    AddService(&ad, &beef_uuid);
    CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == -1);
}

/* Decision: an orderly close writes nothing and stops the timer */
static void TestClose(void)
{
    uint8_t held[REPORT];
    BH_Harness *h = Published(0, true);
    uint64_t deadline;
    int n;

    Section("Decision: close");
    Held(held);
    BH_Advance(h, 20);
    FaceValue(h, held, REPORT);
    n = h->nactions;
    SDL_BLESession_Close(&h->session, h->now);
    BH_Drain(h);
    CHECK(h->nactions == n + 2 && h->actions[n].action.kind == SDL_BLE_ACTION_REMOVE);
    CHECK(h->actions[n + 1].action.kind == SDL_BLE_ACTION_DISCONNECT);
    CHECK(Writes(h) == 0 && AtRest(BH_Controls(h)) && !State(h)->started);
    CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline));
    CHECK(!SDL_BLEOculusGoModule.GetDeadline(h->state, &deadline));
    BH_Advance(h, 60000);
    CHECK(h->nactions == n + 2);
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
    TestButtonBits();
    TestTouchClamps();
    TestSilence();
    TestSilenceEdges();
    TestOptionalBeef();
    TestReady();
    TestGap();
    TestDiscovery();
    TestClose();
    Section(NULL);
    return BH_Report("testbleoculusgo");
}
