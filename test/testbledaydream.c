/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/ble/SDL_ble_daydream_proto.c, the Google
   Daydream controller of hifihedgehog/SDL#33 Part 12. Blocks named "Test N"
   follow the part's Daydream tests. The other blocks cover the decisions
   those tests leave open. Captures 1 to 7 are hardware captures: 1, 2 and 5
   from Stack Overflow answer 40753551, 3, 4 and 6 from the hardfault.life
   Daydream2HID article, 7 from daydream-catcher (Writeups/1. Snapping out of
   a Daydream.md:69). Each capture's fields are packed back into its bytes by
   this file's own bit writer before the module's reader is trusted with
   them. Values reach the module through the session as exact-size heap
   copies (testbleharness.h). */

#include "testbleharness.h"
#include "SDL_ble_daydream_proto.h"

#define PACKET_SIZE SDL_DAYDREAM_REPORT_SIZE
#define POSE        SDL_DAYDREAM_POSE
#define BATTERY     SDL_DAYDREAM_BATTERY
#define ADDRESS     0xC0FFEE123456ULL
#define TEST_PI     3.14159265358979323846
#define TEST_G      9.80665

#define SOUTH_BIT (1u << SDL_BLE_BUTTON_SOUTH)
#define GUIDE_BIT (1u << SDL_BLE_BUTTON_GUIDE)
#define START_BIT (1u << SDL_BLE_BUTTON_START)
#define LS_BIT    (1u << SDL_BLE_BUTTON_LEFT_SHOULDER)
#define RS_BIT    (1u << SDL_BLE_BUTTON_RIGHT_SHOULDER)

/* The button bits of byte 18, stream bits 147 to 151 */
#define VOLUME_UP   0x10
#define VOLUME_DOWN 0x08
#define APP         0x04
#define HOME        0x02
#define CLICK       0x01

typedef void (*TestFunction)(void);

/* Runs one block and prints its own check count */
static void Run(const char *label, TestFunction test)
{
    const int checks = bh_checks;
    const int failures = bh_failures;

    test();
    printf("%s: %d checks, %d failures\n", label, bh_checks - checks, bh_failures - failures);
}

/* Every field of the record, in stream order */
typedef struct Fields
{
    int time;
    int sequence;
    int orientation[3];
    int accel[3];
    int gyro[3];
    int touch_x;
    int touch_y;
    int buttons; /* VOLUME_UP, VOLUME_DOWN, APP, HOME, CLICK */
    int status;
} Fields;

typedef struct Capture
{
    const char *name;
    const char *hex;
    Fields fields;
} Capture;

enum
{
    CAPTURE_1,
    CAPTURE_2A,
    CAPTURE_2B,
    CAPTURE_3,
    CAPTURE_4,
    CAPTURE_5,
    CAPTURE_6,
    CAPTURE_7,
    CAPTURE_COUNT
};

/* The fields the part lists for a capture are its expectations. The others
   are read from the layout. Pack() proves every one against the bytes. */
static const Capture captures[CAPTURE_COUNT] = {
    /* Stack Overflow, lying flat on a table */
    { "capture 1", "5B EB FF B8 25 FD B0 00 04 10 00 B0 00 00 00 00 00 00 00 00",
      { 183, 26, { -3, -2011, -74 }, { 0, 520, 11 }, { 0, 0, 0 }, 0, 0, 0, 0x00 } },
    { "capture 2a", "63 EF FF B8 25 FD B0 00 04 10 00 B0 00 00 00 00 00 00 00 08",
      { 199, 27, { -3, -2011, -74 }, { 0, 520, 11 }, { 0, 0, 0 }, 0, 0, 0, 0x08 } },
    { "capture 2b", "6C 73 FF B8 25 FD B0 00 04 10 00 B0 00 00 00 00 00 00 00 38",
      { 216, 28, { -3, -2011, -74 }, { 0, 520, 11 }, { 0, 0, 0 }, 0, 0, 0, 0x38 } },
    /* The article, firmware 1.0.39, sitting flat on a table */
    { "capture 3", "0B 83 FD FC B1 FE BF FE 44 16 00 A0 00 00 00 00 00 00 00 01",
      { 23, 0, { -17, -847, -41 }, { -7, 523, 10 }, { 0, 0, 0 }, 0, 0, 0, 0x01 } },
    /* The article, touching */
    { "capture 4", "E5 80 03 BC 71 FE A7 F6 44 16 00 70 00 00 00 00 11 09 60 01",
      { 459, 0, { 29, -911, -44 }, { -39, 523, 7 }, { 0, 0, 0 }, 136, 75, 0, 0x01 } },
    /* Stack Overflow, using the touch pad */
    { "capture 5", "48 0B FE 87 EB 00 E8 01 84 10 00 B0 00 00 00 01 91 FB A0 08",
      { 144, 2, { -12, 2027, 29 }, { 6, 520, 11 }, { 0, 0, 12 }, 143, 221, 0, 0x08 } },
    /* The article, spun 180 degrees on the table */
    { "capture 6", "FC 80 00 DB 3B FF 67 FA C4 16 00 D0 02 7D E3 FC 80 00 00 01",
      { 505, 0, { 6, -1221, -20 }, { -21, 523, 13 }, { 4, -136, -28 }, 0, 0, 0, 0x01 } },
    /* daydream-catcher, after pairing */
    { "capture 7", "26 F8 03 61 C2 00 20 02 C4 0F FE 40 00 00 00 00 00 00 10 11",
      { 77, 30, { 27, 450, 4 }, { 11, 519, -28 }, { 0, 0, 0 }, 0, 0, VOLUME_UP, 0x11 } },
};

/* Writes count bits of value at stream bit start, most significant first
   from bit 7 of byte 0 */
static void PutBits(uint8_t *data, int start, int count, uint32_t value)
{
    int i;

    for (i = 0; i < count; ++i) {
        const int bit = start + i;
        const uint8_t mask = (uint8_t)(0x80 >> (bit % 8));

        if (value & ((uint32_t)1 << (count - 1 - i))) {
            data[bit / 8] |= mask;
        } else {
            data[bit / 8] &= (uint8_t)~mask;
        }
    }
}

/* The layout of the part's input report table */
static void Pack(const Fields *f, uint8_t *out)
{
    int i;

    memset(out, 0, PACKET_SIZE);
    PutBits(out, 0, 9, (uint32_t)f->time);
    PutBits(out, 9, 5, (uint32_t)f->sequence);
    for (i = 0; i < 3; ++i) {
        PutBits(out, 14 + 13 * i, 13, (uint32_t)f->orientation[i] & 0x1FFF);
        PutBits(out, 53 + 13 * i, 13, (uint32_t)f->accel[i] & 0x1FFF);
        PutBits(out, 92 + 13 * i, 13, (uint32_t)f->gyro[i] & 0x1FFF);
    }
    PutBits(out, 131, 8, (uint32_t)f->touch_x);
    PutBits(out, 139, 8, (uint32_t)f->touch_y);
    PutBits(out, 147, 5, (uint32_t)f->buttons);
    PutBits(out, 152, 8, (uint32_t)f->status);
}

/* Every field through the module's bit reader */
static void Read(const uint8_t *packet, Fields *f)
{
    int i;

    memset(f, 0, sizeof(*f));
    f->time = (int)SDL_Daydream_Bits(packet, 0, 9);
    f->sequence = (int)SDL_Daydream_Bits(packet, 9, 5);
    for (i = 0; i < 3; ++i) {
        f->orientation[i] = (int)SDL_Daydream_Signed13(packet, 14 + 13 * i);
        f->accel[i] = (int)SDL_Daydream_Signed13(packet, 53 + 13 * i);
        f->gyro[i] = (int)SDL_Daydream_Signed13(packet, 92 + 13 * i);
    }
    f->touch_x = (int)SDL_Daydream_Bits(packet, 131, 8);
    f->touch_y = (int)SDL_Daydream_Bits(packet, 139, 8);
    f->buttons = (int)SDL_Daydream_Bits(packet, 147, 5);
    f->status = (int)SDL_Daydream_Bits(packet, 152, 8);
}

static bool FieldsEqual(const Fields *a, const Fields *b)
{
    int i;

    for (i = 0; i < 3; ++i) {
        if (a->orientation[i] != b->orientation[i] || a->accel[i] != b->accel[i] || a->gyro[i] != b->gyro[i]) {
            return false;
        }
    }
    return a->time == b->time && a->sequence == b->sequence && a->touch_x == b->touch_x && a->touch_y == b->touch_y &&
           a->buttons == b->buttons && a->status == b->status;
}

static void PrintFields(const char *what, const Fields *f)
{
    printf("  %s: time %d sequence %d orientation (%d, %d, %d) accel (%d, %d, %d) gyro (%d, %d, %d) touch (%d, %d) buttons %02X status %02X\n",
           what, f->time, f->sequence, f->orientation[0], f->orientation[1], f->orientation[2], f->accel[0], f->accel[1],
           f->accel[2], f->gyro[0], f->gyro[1], f->gyro[2], f->touch_x, f->touch_y, f->buttons, f->status);
}

/* Parses a capture and proves its fields both ways: packed by this file they
   give the capture's bytes, read by the module they give the fields */
static void Decode(int index, uint8_t *packet)
{
    const Capture *capture = &captures[index];
    uint8_t packed[PACKET_SIZE];
    Fields read;

    BH_CHECK(BH_Hex(capture->hex, packet, PACKET_SIZE) == PACKET_SIZE, "%s is not 20 bytes", capture->name);
    Pack(&capture->fields, packed);
    BH_CHECK(memcmp(packed, packet, PACKET_SIZE) == 0, "%s: the fields do not pack to the capture", capture->name);
    Read(packet, &read);
    BH_CHECK(FieldsEqual(&read, &capture->fields), "%s: the reader disagrees", capture->name);
    if (!FieldsEqual(&read, &capture->fields)) {
        PrintFields("read", &read);
        PrintFields("want", &capture->fields);
    }
}

/* The part's touch mapping: (t - 128) x 32767 / 127, truncated toward zero
   and clamped to -32768..32767 */
static int16_t StickAxis(int t)
{
    long result = (long)(t - 128) * 32767 / 127;

    if (result < -32768) {
        result = -32768;
    } else if (result > 32767) {
        result = 32767;
    }
    return (int16_t)result;
}

static uint32_t ButtonsOf(int bits)
{
    uint32_t buttons = 0;

    if (bits & CLICK) {
        buttons |= SOUTH_BIT;
    }
    if (bits & HOME) {
        buttons |= GUIDE_BIT;
    }
    if (bits & APP) {
        buttons |= START_BIT;
    }
    if (bits & VOLUME_DOWN) {
        buttons |= LS_BIT;
    }
    if (bits & VOLUME_UP) {
        buttons |= RS_BIT;
    }
    return buttons;
}

/* The controls the SDL mapping makes of a record, the battery aside */
static bool ControlsMatch(const SDL_BLEControls *c, const Fields *f)
{
    const bool finger = (f->touch_x != 0 || f->touch_y != 0);

    if (c->buttons != ButtonsOf(f->buttons) || c->finger != finger) {
        return false;
    }
    if (c->axes[SDL_BLE_AXIS_RIGHTX] != 0 || c->axes[SDL_BLE_AXIS_RIGHTY] != 0 ||
        c->axes[SDL_BLE_AXIS_LEFT_TRIGGER] != 0 || c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] != 0) {
        return false;
    }
    if (!finger) {
        return c->axes[SDL_BLE_AXIS_LEFTX] == 0 && c->axes[SDL_BLE_AXIS_LEFTY] == 0 && c->finger_x == 0.0f &&
               c->finger_y == 0.0f;
    }
    return c->axes[SDL_BLE_AXIS_LEFTX] == StickAxis(f->touch_x) && c->axes[SDL_BLE_AXIS_LEFTY] == StickAxis(f->touch_y) &&
           fabs((double)c->finger_x - f->touch_x / 255.0) < 1e-6 && fabs((double)c->finger_y - f->touch_y / 255.0) < 1e-6;
}

static bool AtRest(const SDL_BLEControls *c)
{
    int i;

    for (i = 0; i < SDL_BLE_MAX_AXES; ++i) {
        if (c->axes[i] != 0) {
            return false;
        }
    }
    return c->buttons == 0 && !c->finger && c->finger_x == 0.0f && c->finger_y == 0.0f;
}

static double AccelOf(int raw)
{
    return raw / 4095.0 * 8.0 * TEST_G;
}

static double GyroOf(int raw)
{
    return raw / 4095.0 * 2048.0 * TEST_PI / 180.0;
}

static bool Near(float value, double want)
{
    return fabs((double)value - want) <= 1e-4 + 1e-6 * fabs(want);
}

/* The two samples of one record, from index on: the accelerometer, then the
   gyroscope, fields 1, 2 and 3 as SDL X, Y and Z */
static bool SamplesMatch(const BH_Harness *h, int index, const Fields *f, uint64_t time_ns, uint64_t sensor_ms)
{
    const BH_Sample *accel, *gyro;
    int i;

    if (index < 0 || index + 2 > h->nsamples) {
        return false;
    }
    accel = &h->samples[index];
    gyro = &h->samples[index + 1];
    if (accel->sensor != SDL_BLE_SENSOR_ACCEL || gyro->sensor != SDL_BLE_SENSOR_GYRO) {
        return false;
    }
    if (accel->time_ns != time_ns || gyro->time_ns != time_ns || accel->sensor_ns != sensor_ms * 1000000 ||
        gyro->sensor_ns != sensor_ms * 1000000) {
        return false;
    }
    for (i = 0; i < 3; ++i) {
        if (!Near(accel->data[i], AccelOf(f->accel[i])) || !Near(gyro->data[i], GyroOf(f->gyro[i]))) {
            return false;
        }
    }
    return true;
}

/* The sensor time of a sample in milliseconds, counted back from the newest
   at 0, or ~0 when there is no such sample */
static uint64_t SensorMs(const BH_Harness *h, int back)
{
    if (back < 0 || back >= h->nsamples) {
        return ~(uint64_t)0;
    }
    return h->samples[h->nsamples - 1 - back].sensor_ns / 1000000;
}

static void PrintSample(const BH_Harness *h, int index)
{
    if (index >= 0 && index < h->nsamples) {
        const BH_Sample *s = &h->samples[index];

        printf("  sample %d: sensor %d time %llu sensor %llu (%f, %f, %f)\n", index, s->sensor,
               (unsigned long long)s->time_ns, (unsigned long long)s->sensor_ns, s->data[0], s->data[1], s->data[2]);
    }
}

/* A bonded session through its start, not yet published */
static BH_Harness *Started(void)
{
    BH_Harness *h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, false);

    BH_Start(h, true);
    return h;
}

static void Feed(BH_Harness *h, const uint8_t *packet, uint64_t now)
{
    h->now = now;
    BH_Value(h, POSE, packet, PACKET_SIZE, false);
}

/* A published joystick: started, then capture 1 at 1000 ms */
static BH_Harness *Up(void)
{
    BH_Harness *h = Started();
    uint8_t packet[PACKET_SIZE];

    BH_CHECK(BH_Hex(captures[CAPTURE_1].hex, packet, sizeof(packet)) == PACKET_SIZE, "%s is not 20 bytes",
             captures[CAPTURE_1].name);
    Feed(h, packet, 1000);
    return h;
}

static bool IsAction(const BH_Harness *h, int index, int kind)
{
    return index >= 0 && index < h->nactions && h->actions[index].action.kind == kind;
}

static bool IsSubscribe(const BH_Harness *h, int index, int characteristic, int cccd)
{
    return IsAction(h, index, SDL_BLE_ACTION_SUBSCRIBE) && h->actions[index].action.characteristic == characteristic &&
           h->actions[index].action.cccd == cccd;
}

static void Record(SDL_BLEAdvertisement *ad, const char *name, uint8_t kind)
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
        return -2; /* a name or service match never carries a variant */
    }
    return family;
}

/* The family table and the identity */
static void TestTable(void)
{
    const SDL_BLEFamily *family = &SDL_BLEDaydreamFamily;
    SDL_DaydreamState state;
    uint64_t deadline = 0;
    uint8_t uuid[16];
    SDL_BLEUUID fe55 = SDL_BLE_UUID16(0xFE55);
    SDL_BLEUUID battery_service = SDL_BLE_UUID16(0x180F);
    SDL_BLEUUID battery_level = SDL_BLE_UUID16(0x2A19);
    int i;

    BH_CHECK(SDL_BLE_Families[SDL_BLE_FAMILY_DAYDREAM] == family, "Daydream is not the second family");
    BH_CHECK(family->module == &SDL_BLEDaydreamModule, "module");
    BH_CHECK(SDL_BLEDaydreamModule.state_size == sizeof(SDL_DaydreamState), "state size");
    BH_CHECK(family->pairing == SDL_BLE_PAIR_FIRST, "pairing rule %d", family->pairing);

    /* The exact local name (daydream2hid src/bluetooth.c:77-88). FE55 only
       corroborates: the Bluetooth SIG lists it as a member UUID of Google,
       not of this controller. */
    BH_CHECK(family->nkeys == 2, "keys %d", family->nkeys);
    BH_CHECK(family->keys[0].kind == SDL_BLE_KEY_NAME_EQUALS && !family->keys[0].corroborating && family->keys[0].name &&
                 strcmp(family->keys[0].name, "Daydream controller") == 0,
             "name key");
    BH_CHECK(family->keys[1].kind == SDL_BLE_KEY_SERVICE && family->keys[1].corroborating &&
                 SDL_BLE_UUIDEqual(&family->keys[1].uuid, &fe55),
             "service key");

    /* Service FE55 and the pose characteristic (DaydreamController.js:21-28),
       then the battery level */
    BH_CHECK(family->nservices == 2 && !family->has_alternate, "services %d", family->nservices);
    BH_CHECK(BH_Hex("0000fe55-0000-1000-8000-00805f9b34fb", uuid, sizeof(uuid)) == 16 &&
                 memcmp(family->services[0].bytes, uuid, 16) == 0,
             "service FE55");
    BH_CHECK(SDL_BLE_UUIDEqual(&family->services[1], &battery_service), "battery service");
    BH_CHECK(family->ncharacteristics == 2, "characteristics %d", family->ncharacteristics);
    BH_CHECK(BH_Hex("00000001-1000-1000-8000-00805f9b34fb", uuid, sizeof(uuid)) == 16 &&
                 memcmp(family->characteristics[POSE].uuid.bytes, uuid, 16) == 0,
             "pose characteristic");
    BH_CHECK(family->characteristics[POSE].service == 0 && family->characteristics[POSE].flags == SDL_BLE_CHAR_SUBSCRIBE,
             "pose flags %02X", family->characteristics[POSE].flags);
    BH_CHECK(SDL_BLE_UUIDEqual(&family->characteristics[BATTERY].uuid, &battery_level) &&
                 family->characteristics[BATTERY].service == 1,
             "battery characteristic");
    BH_CHECK(family->characteristics[BATTERY].flags == (SDL_BLE_CHAR_SUBSCRIBE | SDL_BLE_CHAR_READ | SDL_BLE_CHAR_OPTIONAL),
             "battery flags %02X", family->characteristics[BATTERY].flags);
    /* The control characteristic is not in the table, so it cannot be written */
    BH_CHECK(BH_Hex("00000002-1000-1000-8000-00805f9b34fb", uuid, sizeof(uuid)) == 16, "the control UUID");
    for (i = 0; i < family->ncharacteristics; ++i) {
        BH_CHECK(memcmp(family->characteristics[i].uuid.bytes, uuid, 16) != 0, "the control characteristic is listed");
    }

    /* The identity of the SDL mapping */
    memset(&state, 0x5A, sizeof(state));
    SDL_BLEDaydreamModule.Reset(&state, NULL, NULL);
    BH_CHECK(strcmp(state.base.identity.name, "Google Daydream Controller") == 0, "name %s", state.base.identity.name);
    BH_CHECK(state.base.identity.type == SDL_BLE_TYPE_GAMEPAD && state.base.identity.gamepad, "type");
    BH_CHECK(state.base.identity.vendor == 0x18D1 && state.base.identity.product == 0x9210, "VID %04X PID %04X",
             state.base.identity.vendor, state.base.identity.product);
    BH_CHECK(state.base.identity.buttons == (SOUTH_BIT | GUIDE_BIT | START_BIT | LS_BIT | RS_BIT), "buttons %08X",
             state.base.identity.buttons);
    BH_CHECK(state.base.identity.axes == ((1u << SDL_BLE_AXIS_LEFTX) | (1u << SDL_BLE_AXIS_LEFTY)), "axes %02X",
             state.base.identity.axes);
    BH_CHECK(state.base.identity.touchpad, "no touchpad");
    BH_CHECK(state.base.identity.accel_rate == 40.0f && state.base.identity.gyro_rate == 40.0f, "sensor rates");
    BH_CHECK(AtRest(&state.base.controls) && state.base.controls.battery == -1, "rest");
    BH_CHECK(!state.base.ready && !state.base.failed && !state.base.resubscribe, "flags");
    BH_CHECK(state.base.write_count == 0 && state.base.changes == 0, "queue");

    /* No start-up write, no timer, no closing write: the control byte 01
       would put it to sleep (daydream-catcher Reference :14) */
    SDL_BLEDaydreamModule.Start(&state, 0);
    BH_CHECK(state.base.write_count == 0 && !state.base.ready, "start");
    BH_CHECK(!SDL_BLEDaydreamModule.GetDeadline(&state, &deadline), "a deadline");
    SDL_BLEDaydreamModule.Tick(&state, 1000000);
    SDL_BLEDaydreamModule.WriteDone(&state, true, 1000000);
    SDL_BLEDaydreamModule.Close(&state, 1000000);
    BH_CHECK(state.base.write_count == 0 && AtRest(&state.base.controls), "tick, write done and close");
}

/* Test 1: capture 1, lying flat: time 183, sequence 26, orientation (-3,
   -2011, -74), accelerometer (0, 520, 11), gyroscope (0, 0, 0), no touch, no
   button */
static void Test1_Flat(void)
{
    BH_Harness *h = Started();
    uint8_t packet[PACKET_SIZE];
    const Fields *f = &captures[CAPTURE_1].fields;

    Decode(CAPTURE_1, packet);
    Feed(h, packet, 1000);
    BH_CHECK(ControlsMatch(BH_Controls(h), f) && AtRest(BH_Controls(h)), "controls");
    BH_CHECK(h->nsnapshots == 0, "a record at rest emitted %d snapshots", h->nsnapshots);
    BH_CHECK(h->published == 1 && BH_LastKind(h) == SDL_BLE_ACTION_PUBLISH, "not published");
    /* The first record's time is the first sensor timestamp */
    BH_CHECK(h->nsamples == 2 && SamplesMatch(h, 0, f, 1000000000ULL, 183), "samples");
    if (!SamplesMatch(h, 0, f, 1000000000ULL, 183)) {
        PrintSample(h, 0);
        PrintSample(h, 1);
    }
    /* Field 2 reads about +1 g lying flat, SDL's positive Y at rest */
    BH_CHECK(h->nsamples >= 1 && h->samples[0].data[1] > 9.5f && h->samples[0].data[1] < 10.5f, "flat Y");
    BH_Destroy(h);
}

/* Test 2: the next two flat records: sequence 27 and 28, time 199 and 216,
   every other field unchanged but the status byte */
static void Test2_Next(void)
{
    BH_Harness *h = Started();
    uint8_t first[PACKET_SIZE], second[PACKET_SIZE], third[PACKET_SIZE];

    Decode(CAPTURE_1, first);
    Decode(CAPTURE_2A, second);
    Decode(CAPTURE_2B, third);
    Feed(h, first, 1000);
    Feed(h, second, 1016);
    Feed(h, third, 1033);
    BH_CHECK(h->nsnapshots == 0 && AtRest(BH_Controls(h)), "controls changed");
    BH_CHECK(h->nsamples == 6, "samples %d", h->nsamples);
    BH_CHECK(SamplesMatch(h, 2, &captures[CAPTURE_2A].fields, 1016000000ULL, 199), "second record");
    BH_CHECK(SamplesMatch(h, 4, &captures[CAPTURE_2B].fields, 1033000000ULL, 216), "third record");
    BH_Destroy(h);
}

/* Test 3: capture 3, firmware 1.0.39, flat: time 23, sequence 0, orientation
   (-17, -847, -41), accelerometer (-7, 523, 10). The mask 80 of
   DaydreamController.js:49 takes one of the three bits the field has there
   and reads -20. The mask E0 of the Stack Overflow code and the bit reader
   read -17. */
static void Test3_Firmware1039(void)
{
    BH_Harness *h = Started();
    uint8_t packet[PACKET_SIZE];
    const Fields *f = &captures[CAPTURE_3].fields;
    int js, so;

    Decode(CAPTURE_3, packet);
    js = ((packet[1] & 0x03) << 11) | (packet[2] << 3) | ((packet[3] & 0x80) >> 5);
    js = (js & 0x1000) ? js - 0x2000 : js;
    so = ((packet[1] & 0x03) << 11) | (packet[2] << 3) | ((packet[3] & 0xE0) >> 5);
    so = (so & 0x1000) ? so - 0x2000 : so;
    BH_CHECK(js == -20 && so == -17, "mask 80 reads %d, mask E0 %d", js, so);
    BH_CHECK(SDL_Daydream_Signed13(packet, 14) == -17, "orientation 1 reads %d", (int)SDL_Daydream_Signed13(packet, 14));
    Feed(h, packet, 2000);
    BH_CHECK(AtRest(BH_Controls(h)) && h->published == 1, "controls");
    BH_CHECK(SamplesMatch(h, 0, f, 2000000000ULL, 23), "samples");
    BH_Destroy(h);
}

/* Test 4: capture 4, touching: touch (136, 75), the finger down at (136/255,
   75/255), the left stick at (2064, -13674), no button */
static void Test4_Touch(void)
{
    BH_Harness *h = Up();
    uint8_t packet[PACKET_SIZE];
    const Fields *f = &captures[CAPTURE_4].fields;
    const SDL_BLEControls *c;

    Decode(CAPTURE_4, packet);
    Feed(h, packet, 1025);
    c = BH_Controls(h);
    BH_CHECK(c->finger && fabs((double)c->finger_x - 136.0 / 255.0) < 1e-6 && fabs((double)c->finger_y - 75.0 / 255.0) < 1e-6,
             "finger %d at (%f, %f)", c->finger, c->finger_x, c->finger_y);
    BH_CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == 2064 && c->axes[SDL_BLE_AXIS_LEFTY] == -13674, "stick (%d, %d)",
             c->axes[SDL_BLE_AXIS_LEFTX], c->axes[SDL_BLE_AXIS_LEFTY]);
    BH_CHECK(c->buttons == 0 && ControlsMatch(c, f), "buttons %08X", c->buttons);
    BH_CHECK(h->nsnapshots == 1, "snapshots %d", h->nsnapshots);
    /* From time 183 to 459 */
    BH_CHECK(SamplesMatch(h, 2, f, 1025000000ULL, 459), "samples");
    BH_Destroy(h);
}

/* Test 5: capture 5, using the touch pad: touch (143, 221), gyroscope 3 at
   12, accelerometer (6, 520, 11). The answer's next two records keep the
   finger there while gyroscope 3 moves. */
static void Test5_TouchPad(void)
{
    static const char *const next[] = {
        "4F8FFE47EB00E800441000B0000003FEB1FBA038",
        "5893FE27EB00EFFF041000B0000003FF51FBA000",
    };
    static const Fields next_fields[] = {
        { 159, 3, { -14, 2027, 29 }, { 1, 520, 11 }, { 0, 0, -11 }, 143, 221, 0, 0x38 },
        { 177, 4, { -15, 2027, 29 }, { -4, 520, 11 }, { 0, 0, -6 }, 143, 221, 0, 0x00 },
    };
    BH_Harness *h = Started();
    uint8_t packet[PACKET_SIZE], packed[PACKET_SIZE];
    const Fields *f = &captures[CAPTURE_5].fields;
    Fields read;
    int i;

    Decode(CAPTURE_5, packet);
    BH_CHECK(f->touch_x == 143 && f->touch_y == 221 && f->gyro[2] == 12 && f->accel[0] == 6 && f->accel[1] == 520 &&
                 f->accel[2] == 11,
             "the part's values");
    Feed(h, packet, 3000);
    BH_CHECK(ControlsMatch(BH_Controls(h), f) && BH_Controls(h)->finger, "controls");
    BH_CHECK(BH_Axis(h, SDL_BLE_AXIS_LEFTX) == 3870 && BH_Axis(h, SDL_BLE_AXIS_LEFTY) == 23994, "stick (%d, %d)",
             BH_Axis(h, SDL_BLE_AXIS_LEFTX), BH_Axis(h, SDL_BLE_AXIS_LEFTY));
    BH_CHECK(SamplesMatch(h, 0, f, 3000000000ULL, 144), "samples");
    for (i = 0; i < 2; ++i) {
        BH_CHECK(BH_Hex(next[i], packet, sizeof(packet)) == PACKET_SIZE, "next %d", i);
        Pack(&next_fields[i], packed);
        Read(packet, &read);
        BH_CHECK(memcmp(packed, packet, PACKET_SIZE) == 0 && FieldsEqual(&read, &next_fields[i]), "next %d fields", i);
        Feed(h, packet, 3016 + 18 * (uint64_t)i);
        BH_CHECK(ControlsMatch(BH_Controls(h), &next_fields[i]), "next %d controls", i);
        BH_CHECK(SamplesMatch(h, 2 + 2 * i, &next_fields[i], (3016 + 18 * (uint64_t)i) * 1000000, (uint64_t)next_fields[i].time),
                 "next %d samples", i);
    }
    BH_CHECK(h->nsnapshots == 1, "the finger did not move, %d snapshots", h->nsnapshots);
    BH_Destroy(h);
}

/* Test 6: capture 6, spun on the table: time 505, gyroscope (4, -136, -28),
   in radians per second as SDL X, Y and Z */
static void Test6_Rotating(void)
{
    BH_Harness *h = Started();
    uint8_t packet[PACKET_SIZE];
    const Fields *f = &captures[CAPTURE_6].fields;

    Decode(CAPTURE_6, packet);
    Feed(h, packet, 4000);
    BH_CHECK(AtRest(BH_Controls(h)), "controls");
    BH_CHECK(SamplesMatch(h, 0, f, 4000000000ULL, 505), "samples");
    BH_CHECK(h->nsamples == 2 && Near(h->samples[1].data[0], 0.0349151) && Near(h->samples[1].data[1], -1.1871137) &&
                 Near(h->samples[1].data[2], -0.2444058),
             "gyroscope in radians per second");
    PrintSample(h, 1);
    BH_Destroy(h);
}

/* Test 7: capture 7, after pairing: volume up held, accelerometer (11, 519,
   -28). The writeup's next three records (:70-72, also packet.rs:268-274)
   hold the same state. */
static void Test7_Paired(void)
{
    static const char *const next[] = {
        "36 80 03 61 C2 00 20 02 C4 0F FE 40 00 00 00 00 00 00 10 11",
        "47 08 03 61 C2 00 20 02 C4 0F FE 40 00 00 00 00 00 00 10 11",
        "4F 0C 03 61 C2 00 20 02 C4 0F FE 40 00 00 00 00 00 00 10 11",
    };
    static const int times[] = { 109, 142, 158 };
    static const int sequences[] = { 0, 2, 3 };
    BH_Harness *h = Started();
    uint8_t packet[PACKET_SIZE];
    const Fields *f = &captures[CAPTURE_7].fields;
    Fields expected, read;
    int i;

    Decode(CAPTURE_7, packet);
    Feed(h, packet, 5000);
    BH_CHECK(BH_Controls(h)->buttons == RS_BIT && ControlsMatch(BH_Controls(h), f), "volume up is not RIGHT_SHOULDER alone");
    BH_CHECK(SamplesMatch(h, 0, f, 5000000000ULL, 77), "samples");
    BH_CHECK(h->nsnapshots == 1, "snapshots %d", h->nsnapshots);
    for (i = 0; i < 3; ++i) {
        expected = *f;
        expected.time = times[i];
        expected.sequence = sequences[i];
        BH_CHECK(BH_Hex(next[i], packet, sizeof(packet)) == PACKET_SIZE, "next %d", i);
        Read(packet, &read);
        BH_CHECK(FieldsEqual(&read, &expected), "record %d", i + 2);
        Feed(h, packet, 5032 + 20 * (uint64_t)i);
        BH_CHECK(SamplesMatch(h, 2 + 2 * i, &expected, (5032 + 20 * (uint64_t)i) * 1000000, (uint64_t)times[i]),
                 "record %d samples", i + 2);
    }
    BH_CHECK(h->nsnapshots == 1 && BH_Controls(h)->buttons == RS_BIT, "the held button changed");
    BH_Destroy(h);
}

/* Test 8: capture 3 with byte 18 at 01, 02, 04, 08 or 10 presses SOUTH,
   GUIDE, START, LEFT_SHOULDER or RIGHT_SHOULDER alone (daydream2hid
   src/daydream.c:132-136, DaydreamController.js:39-43) */
static void Test8_Buttons(void)
{
    static const struct
    {
        uint8_t byte18;
        uint32_t buttons;
    } cases[] = {
        { 0x01, SOUTH_BIT }, { 0x02, GUIDE_BIT }, { 0x04, START_BIT }, { 0x08, LS_BIT }, { 0x10, RS_BIT },
        { 0x1F, SOUTH_BIT | GUIDE_BIT | START_BIT | LS_BIT | RS_BIT }, { 0x00, 0 }, { 0x11, SOUTH_BIT | RS_BIT },
    };
    BH_Harness *h = Up();
    uint8_t packet[PACKET_SIZE];
    uint64_t now = 1100;
    int i, n;

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        BH_CHECK(BH_Hex(captures[CAPTURE_3].hex, packet, sizeof(packet)) == PACKET_SIZE, "%s is not 20 bytes",
                 captures[CAPTURE_3].name);
        packet[18] = cases[i].byte18;
        n = h->nsnapshots;
        Feed(h, packet, now);
        now += 25;
        BH_CHECK(BH_Controls(h)->buttons == cases[i].buttons, "byte 18 %02X: buttons %08X", cases[i].byte18,
                 BH_Controls(h)->buttons);
        BH_CHECK(!BH_Controls(h)->finger && BH_Axis(h, SDL_BLE_AXIS_LEFTX) == 0, "byte 18 %02X touched", cases[i].byte18);
        BH_CHECK(i == 0 || h->nsnapshots == n + 1, "byte 18 %02X: %d snapshots", cases[i].byte18, h->nsnapshots - n);
    }
    /* Bits 7 to 5 of byte 18 are the low bits of touch Y, not buttons */
    BH_CHECK(BH_Hex(captures[CAPTURE_3].hex, packet, sizeof(packet)) == PACKET_SIZE, "%s is not 20 bytes",
             captures[CAPTURE_3].name);
    packet[18] = 0xE0;
    Feed(h, packet, now);
    BH_CHECK(BH_Controls(h)->buttons == 0 && BH_Controls(h)->finger, "byte 18 E0");
    BH_CHECK(SDL_Daydream_Bits(packet, 139, 8) == 7 && BH_Axis(h, SDL_BLE_AXIS_LEFTX) == -32768 &&
                 BH_Axis(h, SDL_BLE_AXIS_LEFTY) == -31218,
             "touch (0, 7): stick (%d, %d)", BH_Axis(h, SDL_BLE_AXIS_LEFTX), BH_Axis(h, SDL_BLE_AXIS_LEFTY));
    /* The status byte, 19, holds no button */
    BH_CHECK(BH_Hex(captures[CAPTURE_3].hex, packet, sizeof(packet)) == PACKET_SIZE, "%s is not 20 bytes",
             captures[CAPTURE_3].name);
    packet[19] = 0xFF;
    Feed(h, packet, now + 25);
    BH_CHECK(AtRest(BH_Controls(h)), "the status byte pressed something");
    BH_Destroy(h);
}

/* Test 9: a 13-bit field of 0FFF reads 4095 and 1000 reads -4096, in each of
   the nine fields, without touching its neighbors */
static void Test9_Signed13(void)
{
    static const struct
    {
        uint32_t raw;
        int value;
    } cases[] = {
        { 0x0FFF, 4095 }, { 0x1000, -4096 }, { 0x1FFF, -1 }, { 0x0001, 1 }, { 0x0000, 0 }, { 0x0800, 2048 }, { 0x1800, -2048 },
    };
    BH_Harness *h = Up();
    uint8_t packet[PACKET_SIZE];
    Fields read;
    int field, c, other, sample;
    uint64_t now = 1100;
    bool isolated;

    for (field = 0; field < 9; ++field) {
        for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); ++c) {
            memset(packet, 0, sizeof(packet));
            PutBits(packet, 14 + 13 * field, 13, cases[c].raw);
            BH_CHECK(SDL_Daydream_Signed13(packet, 14 + 13 * field) == cases[c].value, "field %d raw %04X reads %d", field,
                     cases[c].raw, (int)SDL_Daydream_Signed13(packet, 14 + 13 * field));
            BH_CHECK(SDL_Daydream_Bits(packet, 14 + 13 * field, 13) == cases[c].raw, "field %d raw %04X bits", field,
                     cases[c].raw);
            isolated = true;
            for (other = 0; other < 9; ++other) {
                if (other != field && SDL_Daydream_Signed13(packet, 14 + 13 * other) != 0) {
                    isolated = false;
                }
            }
            Read(packet, &read);
            BH_CHECK(isolated && read.time == 0 && read.sequence == 0 && read.touch_x == 0 && read.touch_y == 0 &&
                         read.buttons == 0,
                     "field %d raw %04X leaked", field, cases[c].raw);

            /* The accelerometer and gyroscope fields reach the samples */
            if (field >= 3) {
                Fields f;

                memset(&f, 0, sizeof(f));
                if (field < 6) {
                    f.accel[field - 3] = cases[c].value;
                } else {
                    f.gyro[field - 6] = cases[c].value;
                }
                sample = h->nsamples;
                Feed(h, packet, now);
                now += 25;
                BH_CHECK(sample + 2 == h->nsamples, "field %d: %d samples", field, h->nsamples - sample);
                if (sample + 2 == h->nsamples) {
                    int i;
                    bool good = true;

                    for (i = 0; i < 3; ++i) {
                        if (!Near(h->samples[sample].data[i], AccelOf(f.accel[i])) ||
                            !Near(h->samples[sample + 1].data[i], GyroOf(f.gyro[i]))) {
                            good = false;
                        }
                    }
                    BH_CHECK(good, "field %d raw %04X sample", field, cases[c].raw);
                }
            }
        }
    }
    /* The ends of the scales: 8 g and 2048 degrees per second */
    BH_CHECK(Near((float)AccelOf(4095), 8.0 * TEST_G) && Near((float)GyroOf(4095), 2048.0 * TEST_PI / 180.0), "scales");
    BH_Destroy(h);
}

/* Test 10: every length other than 20 is ignored, which covers every
   truncation of captures 1 to 7 (daydream2hid src/bluetooth.c:148-151).
   Stale bytes after byte 19 are ignored. */
static void Test10_Lengths(void)
{
    static const Fields sentinel_fields = { 100, 1, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, 200, 60, 0x1F, 0 };
    BH_Harness *h = Up();
    uint8_t sentinel[PACKET_SIZE], packet[PACKET_SIZE], buffer[128];
    SDL_BLEControls held, clean;
    uint64_t now = 2000, sensor_ms;
    size_t length;
    int index, n, samples;
    int delta;
    bool quiet;

    Pack(&sentinel_fields, sentinel);
    for (index = 0; index < CAPTURE_COUNT; ++index) {
        const Fields *f = &captures[index].fields;

        Decode(index, packet);
        Feed(h, sentinel, now);
        now += 25;
        held = *BH_Controls(h);
        sensor_ms = SensorMs(h, 0);
        n = h->nsnapshots;
        samples = h->nsamples;
        quiet = true;
        /* Up to the 128 bytes a queued value holds (SDL_BLE_MAX_VALUE) */
        for (length = 0; length <= sizeof(buffer); ++length) {
            if (length == PACKET_SIZE) {
                continue;
            }
            memset(buffer, 0, sizeof(buffer));
            memcpy(buffer, packet, PACKET_SIZE);
            h->now = now;
            BH_Value(h, POSE, buffer, length, false);
            if (h->nsnapshots != n || h->nsamples != samples || !SDL_BLE_ControlsEqual(BH_Controls(h), &held)) {
                quiet = false;
                printf("  %s: length %d was decoded\n", captures[index].name, (int)length);
            }
        }
        BH_CHECK(quiet, "%s: a length other than 20 was decoded", captures[index].name);
        /* Positive control: the 20 bytes decode, and the ignored values left
           the time where the sentinel put it */
        Feed(h, packet, now);
        delta = (f->time > sentinel_fields.time) ? f->time - sentinel_fields.time : 512 - sentinel_fields.time + f->time;
        BH_CHECK(h->nsnapshots == n + 1 && ControlsMatch(BH_Controls(h), f), "%s: not decoded", captures[index].name);
        BH_CHECK(SamplesMatch(h, samples, f, now * 1000000, sensor_ms + (uint64_t)delta), "%s: samples",
                 captures[index].name);
        now += 25;
    }

    /* A 20-byte value read from a larger buffer that holds stale bytes after
       it decodes as the clean value */
    Decode(CAPTURE_4, packet);
    Feed(h, sentinel, now);
    Feed(h, packet, now + 25);
    clean = *BH_Controls(h);
    Feed(h, sentinel, now + 50);
    memset(buffer, 0xFF, sizeof(buffer));
    memcpy(buffer, packet, PACKET_SIZE);
    samples = h->nsamples;
    SDL_BLEDaydreamModule.Value(h->state, POSE, buffer, PACKET_SIZE, (now + 75) * 1000000);
    BH_CHECK(SDL_BLE_ControlsEqual(BH_Controls(h), &clean), "stale bytes after byte 19 changed the controls");
    BH_CHECK(h->nsamples == samples + 2 && Near(h->samples[samples].data[0], AccelOf(-39)), "stale bytes: samples");
    BH_Destroy(h);
}

/* Test 11: time 505 followed by time 23 advances the sensor timestamp by
   30 ms, the article's wrap rule (also daydream2hid src/daydream.c:109-113) */
static void Test11_Wrap(void)
{
    static const struct
    {
        int from, to, delta;
    } steps[] = {
        { 505, 23, 30 },
        { 511, 0, 1 },
        { 0, 511, 511 },
        { 1, 2, 1 },
        /* A time that is not larger has wrapped, so an equal time is 512 */
        { 300, 300, 512 },
        { 23, 22, 511 },
    };
    BH_Harness *h = Started();
    uint8_t first[PACKET_SIZE], second[PACKET_SIZE];
    Fields f;
    uint64_t sensor_ms, now = 1000;
    int i;

    Decode(CAPTURE_6, first);
    Decode(CAPTURE_3, second);
    Feed(h, first, now);
    Feed(h, second, now + 30);
    BH_CHECK(h->nsamples == 4 && h->samples[0].sensor_ns == 505000000ULL && h->samples[2].sensor_ns == 535000000ULL,
             "505 then 23");
    now += 60;

    for (i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); ++i) {
        memset(&f, 0, sizeof(f));
        f.time = steps[i].from;
        Pack(&f, first);
        f.time = steps[i].to;
        Pack(&f, second);
        Feed(h, first, now);
        sensor_ms = SensorMs(h, 0);
        Feed(h, second, now + 25);
        now += 50;
        BH_CHECK(SensorMs(h, 0) == sensor_ms + (uint64_t)steps[i].delta, "%d then %d: %llu ms after %llu", steps[i].from,
                 steps[i].to, (unsigned long long)SensorMs(h, 0), (unsigned long long)sensor_ms);
        BH_CHECK(SensorMs(h, 0) == SensorMs(h, 1), "the pair differs");
    }
    BH_Destroy(h);
}

/* The article's twenty flat records, sequences 0 to 19 in a row, its time
   wrapping after 484: 509 ms in all */
static void TestArticleStream(void)
{
    static const char *const stream[] = {
        "0B-83-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "10-87-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "29-0B-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "31-8F-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "41-93-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "49-17-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "61-9B-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "6A-1F-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "71-A3-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "7A-27-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "91-2B-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "99-AF-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "B2-33-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "B9-B7-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "C2-3B-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "D9-BF-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "E2-43-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "EA-C7-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-00",
        "F2-4B-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
        "0A-4F-FD-FC-B1-FE-BF-FE-44-16-00-A0-00-00-00-00-00-00-00-01",
    };
    static const int times[] = { 23, 33, 82, 99, 131, 146, 195, 212, 227, 244, 290, 307, 356, 371, 388, 435, 452, 469, 484, 20 };
    BH_Harness *h = Started();
    uint8_t packet[PACKET_SIZE];
    Fields read;
    bool fields = true, stamps = true;
    int i;

    for (i = 0; i < 20; ++i) {
        BH_CHECK(BH_Hex(stream[i], packet, sizeof(packet)) == PACKET_SIZE, "stream record %d is not 20 bytes", i);
        Read(packet, &read);
        if (read.time != times[i] || read.sequence != i || read.accel[0] != -7 || read.accel[1] != 523 || read.accel[2] != 10 ||
            read.orientation[0] != -17) {
            fields = false;
        }
        Feed(h, packet, 1000 + 25 * (uint64_t)i);
        if (h->nsamples != 2 * (i + 1) || SensorMs(h, 0) != (uint64_t)(i < 19 ? times[i] : 484 + 48)) {
            stamps = false;
        }
    }
    BH_CHECK(fields, "the stream's fields");
    BH_CHECK(stamps, "the stream's sensor timestamps");
    BH_CHECK(h->nsamples == 40 && SensorMs(h, 0) - SensorMs(h, 39) == 509, "509 ms in all, got %llu ms",
             (unsigned long long)(SensorMs(h, 0) - SensorMs(h, 39)));
    BH_CHECK(h->nsnapshots == 0 && h->published == 1, "controls");
    BH_Destroy(h);
}

/* Test 12: pairing. With the hint on and no Windows bond the session pairs
   right after the connection opens, before discovery, then subscribes, and
   capture 7 decodes. With a bond it does not pair. With the hint off and no
   bond it publishes nothing and backs off. */
static void Test12_Pairing(void)
{
    static const int flow[] = {
        SDL_BLE_ACTION_CONNECT, SDL_BLE_ACTION_PAIR, SDL_BLE_ACTION_DISCOVER, SDL_BLE_ACTION_SUBSCRIBE, SDL_BLE_ACTION_SUBSCRIBE,
        SDL_BLE_ACTION_SUBSCRIBE, SDL_BLE_ACTION_SUBSCRIBE, SDL_BLE_ACTION_READ, SDL_BLE_ACTION_PUBLISH,
    };
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad;
    SDL_BLEMatch match;
    BH_Harness *h;
    uint8_t packet[PACKET_SIZE];
    const Fields *f = &captures[CAPTURE_7].fields;
    int i, pairing;

    Decode(CAPTURE_7, packet);

    /* The hint on, no bond */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_CHECK(h->nactions == 1 && IsAction(h, 0, SDL_BLE_ACTION_CONNECT), "connect");
    BH_Connected(h, true, false);
    BH_CHECK(h->nactions == 2 && IsAction(h, 1, SDL_BLE_ACTION_PAIR), "no pairing right after the connection");
    BH_CHECK(h->session.phase == SDL_BLE_PHASE_PAIRING && BH_Count(h, SDL_BLE_ACTION_DISCOVER) == 0, "discovery first");
    BH_Paired(h, true);
    BH_CHECK(h->nactions == 3 && IsAction(h, 2, SDL_BLE_ACTION_DISCOVER), "no discovery after pairing");
    BH_Discovered(h, true, NULL);
    /* The new bond gets None before Notify */
    BH_CHECK(IsSubscribe(h, 3, POSE, SDL_BLE_CCCD_NONE), "pose None");
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_CHECK(IsSubscribe(h, 4, POSE, SDL_BLE_CCCD_NOTIFY), "pose Notify");
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_CHECK(IsSubscribe(h, 5, BATTERY, SDL_BLE_CCCD_NONE), "battery None");
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_CHECK(IsSubscribe(h, 6, BATTERY, SDL_BLE_CCCD_NOTIFY), "battery Notify");
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_CHECK(IsAction(h, 7, SDL_BLE_ACTION_READ) && h->actions[7].action.characteristic == BATTERY, "battery read");
    BH_ReadAll(h, (const uint8_t *)"\x55", 1);
    BH_CHECK(BH_Controls(h)->battery == 85 && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0, "read");
    Feed(h, packet, 1000);
    BH_CHECK(h->nactions == (int)(sizeof(flow) / sizeof(flow[0])), "%d actions", h->nactions);
    for (i = 0; i < (int)(sizeof(flow) / sizeof(flow[0])); ++i) {
        BH_CHECK(IsAction(h, i, flow[i]), "action %d is %s", i,
                 i < h->nactions ? SDL_BLE_ActionName(h->actions[i].action.kind) : "missing");
    }
    BH_CHECK(h->published == 1 && BH_Controls(h)->buttons == RS_BIT && ControlsMatch(BH_Controls(h), f), "capture 7");
    BH_CHECK(SamplesMatch(h, 0, f, 1000000000ULL, 77), "capture 7 samples");
    BH_Destroy(h);

    /* A bond, with the hint on or off: no pairing */
    for (pairing = 0; pairing < 2; ++pairing) {
        h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, pairing != 0);
        BH_Connected(h, true, true);
        BH_CHECK(h->nactions == 2 && IsAction(h, 1, SDL_BLE_ACTION_DISCOVER), "hint %d: bonded", pairing);
        BH_Discovered(h, true, NULL);
        BH_SubscribeAll(h);
        BH_ReadAll(h, NULL, 0);
        Feed(h, packet, 1000);
        BH_CHECK(h->published == 1 && BH_Count(h, SDL_BLE_ACTION_PAIR) == 0, "hint %d: bonded flow", pairing);
        BH_CHECK(IsSubscribe(h, 2, POSE, SDL_BLE_CCCD_NONE) && IsSubscribe(h, 3, POSE, SDL_BLE_CCCD_NOTIFY),
                 "hint %d: None, then Notify", pairing);
        BH_Destroy(h);
    }

    /* The hint off, no bond: nothing published, and the address backs off
       under the host's schedule until the user pairs it in Settings */
    SDL_BLEHost_Init(&host);
    Record(&ad, "Daydream controller", SDL_BLE_AD_ADVERTISEMENT);
    memset(&match, 0, sizeof(match));
    BH_CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 0, &match) &&
                 match.family == SDL_BLE_FAMILY_DAYDREAM,
             "advertisement");
    h = BH_Create(&SDL_BLEDaydreamFamily, match.variant, NULL, false);
    BH_Connected(h, true, false);
    BH_CHECK(h->nactions == 3 && IsAction(h, 1, SDL_BLE_ACTION_BACKOFF) && IsAction(h, 2, SDL_BLE_ACTION_DISCONNECT),
             "hint off: back off");
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PAIR) == 0 && BH_Count(h, SDL_BLE_ACTION_DISCOVER) == 0 &&
                 BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0 && SDL_BLESession_Ended(&h->session),
             "hint off: the session went on");
    /* A record that still arrives changes nothing */
    Feed(h, packet, 10);
    BH_CHECK(h->published == 0 && h->nsamples == 0 && AtRest(BH_Controls(h)), "hint off: a record was decoded");
    {
        SDL_BLEOutcome outcome;

        SDL_BLESession_GetOutcome(&h->session, &outcome);
        SDL_BLEHost_SessionEnded(&host, ad.address, &outcome, 0);
    }
    BH_CHECK(!SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 14999, &match),
             "connected again before the backoff ended");
    BH_CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 15000, &match),
             "not connected after the backoff");
    BH_Destroy(h);

    /* A pairing that fails: no discovery, nothing published */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Paired(h, false);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_DISCOVER) == 0 && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0 &&
                 SDL_BLESession_Ended(&h->session) && BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT,
             "failed pairing");
    BH_Destroy(h);

    /* A stale bond with the hint on: the bond is removed, the device paired
       again, since it streams only over a bond, and every subscription runs
       once more */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    BH_CHECK(BH_LastKind(h) == SDL_BLE_ACTION_REMOVE_BOND, "no bond removal");
    BH_BondRemoved(h, true);
    BH_CHECK(BH_LastKind(h) == SDL_BLE_ACTION_PAIR, "no pairing after the removal");
    BH_Paired(h, true);
    BH_CHECK(IsSubscribe(h, h->nactions - 1, POSE, SDL_BLE_CCCD_NONE), "no repeat");
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    Feed(h, packet, 1000);
    BH_CHECK(h->published == 1 && BH_Count(h, SDL_BLE_ACTION_PAIR) == 1, "stale bond flow");
    BH_Destroy(h);
    /* With the hint off the stale bond is left alone and the session backs off */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, false);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_REMOVE_BOND) == 0 && BH_Count(h, SDL_BLE_ACTION_PAIR) == 0 &&
                 BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1,
             "stale bond, hint off");
    BH_Destroy(h);

    /* Paired but silent, as a device without the bond it wants: no joystick,
       and the session's start timeout ends the link with a backoff instead
       of holding it */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Paired(h, true);
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    BH_Advance(h, h->session.start_deadline - 1);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0 && h->session.phase == SDL_BLE_PHASE_STARTING,
             "silent and still starting just before the timeout");
    BH_Advance(h, 600000);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1 &&
                 BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT && SDL_BLESession_Ended(&h->session),
             "a silent device never publishes and ends with a backoff");
    BH_Destroy(h);
}

/* Test 13: link loss releases the held volume button and lifts the finger.
   The next advertisement reconnects. */
static void Test13_LinkLoss(void)
{
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad;
    SDL_BLEMatch match;
    BH_Harness *h;
    Fields f = captures[CAPTURE_7].fields;
    uint8_t packet[PACKET_SIZE];
    const SDL_BLEControls *c;
    int n, k;

    SDL_BLEHost_Init(&host);
    Record(&ad, "Daydream controller", SDL_BLE_AD_ADVERTISEMENT);
    ad.services[0] = SDL_BLE_UUID16(0xFE55);
    ad.nservices = 1;
    memset(&match, 0, sizeof(match));
    BH_CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 0, &match) &&
                 match.family == SDL_BLE_FAMILY_DAYDREAM,
             "advertisement");

    h = Up();
    BH_Value(h, BATTERY, (const uint8_t *)"\x46", 1, false);
    /* Capture 7's volume up with capture 4's finger */
    f.touch_x = 136;
    f.touch_y = 75;
    Pack(&f, packet);
    Feed(h, packet, 1100);
    c = BH_Controls(h);
    BH_CHECK(c->buttons == RS_BIT && c->finger && c->axes[SDL_BLE_AXIS_LEFTX] == 2064, "held state");
    n = h->nsnapshots;
    k = h->nactions;
    SDL_BLESession_Lost(&h->session, 1200);
    BH_Drain(h);
    BH_CHECK(h->nsnapshots == n + 1, "%d snapshots on the loss", h->nsnapshots - n);
    if (h->nsnapshots == n + 1) {
        BH_CHECK(AtRest(&h->snapshots[n].controls), "the loss left something held");
        BH_CHECK(h->snapshots[n].controls.battery == 70, "the loss dropped the battery");
        BH_CHECK(h->snapshots[n].time_ns == 1200000000ULL, "release time %llu", (unsigned long long)h->snapshots[n].time_ns);
    }
    BH_CHECK(h->nactions == k + 2 && IsAction(h, k, SDL_BLE_ACTION_REMOVE) && IsAction(h, k + 1, SDL_BLE_ACTION_DISCONNECT),
             "loss actions");
    BH_CHECK(!h->session.backoff && SDL_BLESession_Ended(&h->session) && h->published == 0, "session");
    n = h->nsamples;
    Feed(h, packet, 1250);
    BH_CHECK(h->nsamples == n && AtRest(BH_Controls(h)), "a record after the loss");

    /* The next advertisement connects at once. Windows now holds the bond, so
       the new session discovers without pairing, and its clock starts over. */
    {
        SDL_BLEOutcome outcome;

        SDL_BLESession_GetOutcome(&h->session, &outcome);
        SDL_BLEHost_SessionEnded(&host, ad.address, &outcome, 1300);
    }
    BH_Destroy(h);
    BH_CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 1300, &match) &&
                 match.family == SDL_BLE_FAMILY_DAYDREAM,
             "no reconnect");
    h = BH_Create(&SDL_BLEDaydreamFamily, match.variant, NULL, true);
    BH_Connected(h, true, true);
    BH_CHECK(IsAction(h, 1, SDL_BLE_ACTION_DISCOVER) && BH_Count(h, SDL_BLE_ACTION_PAIR) == 0, "reconnect paired again");
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    BH_CHECK(AtRest(BH_Controls(h)) && BH_Controls(h)->battery == -1, "the new session starts at rest");
    Decode(CAPTURE_3, packet);
    Feed(h, packet, 2000);
    BH_CHECK(h->published == 1 && h->nsamples == 2 && h->samples[0].sensor_ns == 23000000ULL, "the new session");
    BH_Destroy(h);
}

/* Test 14: on an injected clock the session writes nothing but the pairing
   and the descriptors, at start-up or later. The control characteristic is
   never written. */
static void Test14_Clock(void)
{
    static const int kinds[] = {
        SDL_BLE_ACTION_CONNECT, SDL_BLE_ACTION_PAIR, SDL_BLE_ACTION_DISCOVER, SDL_BLE_ACTION_SUBSCRIBE, SDL_BLE_ACTION_SUBSCRIBE,
        SDL_BLE_ACTION_SUBSCRIBE, SDL_BLE_ACTION_SUBSCRIBE, SDL_BLE_ACTION_READ, SDL_BLE_ACTION_PUBLISH,
    };
    BH_Harness *h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    uint8_t packet[PACKET_SIZE];
    Fields f = captures[CAPTURE_1].fields;
    uint64_t deadline, now;
    bool deadlines = false, stamps = true;
    int i, count;

    BH_Connected(h, true, false);
    BH_Paired(h, true);
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    /* Ten minutes of records every 25 ms, the article's average, with a tick
       each time */
    count = 10 * 60 * 40;
    for (i = 0; i < count; ++i) {
        now = 1000 + 25 * (uint64_t)i;
        f.time = (int)((183 + 25 * (uint64_t)i) % 512);
        f.sequence = (26 + i) % 32;
        f.buttons = ((i / 400) % 2) ? HOME : 0;
        Pack(&f, packet);
        BH_Advance(h, now);
        SDL_BLESession_Tick(&h->session, now);
        BH_Drain(h);
        Feed(h, packet, now);
        if (SDL_BLESession_GetDeadline(&h->session, &deadline)) {
            deadlines = true;
        }
        if (h->nsamples != 2 || h->samples[1].sensor_ns != (183 + 25 * (uint64_t)i) * 1000000) {
            stamps = false;
        }
        h->nsamples = 0;
    }
    BH_CHECK(!deadlines, "a timer was set");
    BH_CHECK(stamps, "the sensor clock drifted from 25 ms a record");
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == 0, "%d writes", BH_Count(h, SDL_BLE_ACTION_WRITE));
    BH_CHECK(h->nactions == (int)(sizeof(kinds) / sizeof(kinds[0])), "%d actions", h->nactions);
    for (i = 0; i < (int)(sizeof(kinds) / sizeof(kinds[0])); ++i) {
        BH_CHECK(IsAction(h, i, kinds[i]), "action %d is %s", i,
                 i < h->nactions ? SDL_BLE_ActionName(h->actions[i].action.kind) : "missing");
    }
    BH_CHECK(((const SDL_BLEBase *)h->state)->write_count == 0, "queued writes");
    /* Closing on purpose sends no control byte */
    SDL_BLESession_Close(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == 0 && BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT &&
                 SDL_BLESession_Ended(&h->session),
             "close");
    BH_Destroy(h);
}

/* Discovery keys: the exact complete name, in an advertisement or a scan
   response, as daydream2hid matches it (src/bluetooth.c:77-88) and
   daydream-controller.js filters (DaydreamController.js:11-14). Service
   FE55, Google's member UUID, only corroborates, so a Google device that
   advertises FE55 gets no connection, and with the pairing hint on no
   pairing request either. */
static void TestDiscovery(void)
{
    static const char *const misses[] = {
        "Daydream Controller", "daydream controller", "Daydream controller ", " Daydream controller", "Daydream",
        "Daydream controlle", "Daydream controller2", "Daydream  controller", "DAYDREAM CONTROLLER", "",
    };
    SDL_BLEAdvertisement ad;
    bool enabled[SDL_BLE_FAMILY_COUNT];
    int i;

    Record(&ad, "Daydream controller", SDL_BLE_AD_ADVERTISEMENT);
    BH_CHECK(Match(&ad, NULL) == SDL_BLE_FAMILY_DAYDREAM, "name: %d", Match(&ad, NULL));
    Record(&ad, "Daydream controller", SDL_BLE_AD_SCAN_RESPONSE);
    BH_CHECK(Match(&ad, NULL) == SDL_BLE_FAMILY_DAYDREAM, "scan response: %d", Match(&ad, NULL));
    for (i = 0; i < (int)(sizeof(misses) / sizeof(misses[0])); ++i) {
        Record(&ad, misses[i], SDL_BLE_AD_ADVERTISEMENT);
        BH_CHECK(Match(&ad, NULL) == -1, "'%s' matched %d", misses[i], Match(&ad, NULL));
    }
    /* FE55 alone, and beside the SUOTA service FEF5, matches nothing, in an
       advertisement or a scan response */
    Record(&ad, NULL, SDL_BLE_AD_ADVERTISEMENT);
    ad.services[0] = SDL_BLE_UUID16(0xFE55);
    ad.nservices = 1;
    BH_CHECK(Match(&ad, NULL) == -1, "FE55 alone: %d", Match(&ad, NULL));
    ad.kind = SDL_BLE_AD_SCAN_RESPONSE;
    BH_CHECK(Match(&ad, NULL) == -1, "FE55 alone in a scan response: %d", Match(&ad, NULL));
    ad.kind = SDL_BLE_AD_ADVERTISEMENT;
    ad.services[0] = SDL_BLE_UUID16(0xFEF5);
    ad.services[1] = SDL_BLE_UUID16(0xFE55);
    ad.nservices = 2;
    BH_CHECK(Match(&ad, NULL) == -1, "FEF5 and FE55: %d", Match(&ad, NULL));
    /* FEF5 is Dialog's update service, on other devices too: no match */
    ad.nservices = 1;
    BH_CHECK(Match(&ad, NULL) == -1, "FEF5 alone matched");
    /* A name that misses matches nothing beside FE55 */
    Record(&ad, "Daydream Controller", SDL_BLE_AD_ADVERTISEMENT);
    ad.services[0] = SDL_BLE_UUID16(0xFE55);
    ad.nservices = 1;
    BH_CHECK(Match(&ad, NULL) == -1, "FE55 beside another name: %d", Match(&ad, NULL));
    /* The name beside FE55 and FEF5, as the controller advertises them
       (hardfault.life article, "Bluetooth Poking") */
    Record(&ad, "Daydream controller", SDL_BLE_AD_ADVERTISEMENT);
    ad.services[0] = SDL_BLE_UUID16(0xFE55);
    ad.services[1] = SDL_BLE_UUID16(0xFEF5);
    ad.nservices = 2;
    BH_CHECK(Match(&ad, NULL) == SDL_BLE_FAMILY_DAYDREAM, "the name beside FE55 and FEF5: %d", Match(&ad, NULL));
    /* Another FE55 advertiser gets no connection, so no pairing either */
    {
        SDL_BLEHost host;
        SDL_BLEMatch match;

        SDL_BLEHost_Init(&host);
        Record(&ad, "Another device", SDL_BLE_AD_ADVERTISEMENT);
        ad.services[0] = SDL_BLE_UUID16(0xFE55);
        ad.nservices = 1;
        BH_CHECK(!SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 0, &match) &&
                     !SDL_BLEHost_IsActive(&host, ad.address),
                 "an FE55 advertiser started a session");
    }
    /* Google's company ID alone is not a key */
    Record(&ad, NULL, SDL_BLE_AD_ADVERTISEMENT);
    ad.manufacturer[0].company = 0x00E0;
    ad.manufacturer[0].length = 2;
    ad.nmanufacturer = 1;
    BH_CHECK(Match(&ad, NULL) == -1, "manufacturer data matched");
    /* The hint off: neither key matches */
    for (i = 0; i < SDL_BLE_FAMILY_COUNT; ++i) {
        enabled[i] = (i != SDL_BLE_FAMILY_DAYDREAM);
    }
    Record(&ad, "Daydream controller", SDL_BLE_AD_ADVERTISEMENT);
    ad.services[0] = SDL_BLE_UUID16(0xFE55);
    ad.nservices = 1;
    BH_CHECK(Match(&ad, enabled) == -1, "matched with its hint off");
    for (i = 0; i < SDL_BLE_FAMILY_COUNT; ++i) {
        enabled[i] = (i == SDL_BLE_FAMILY_DAYDREAM);
    }
    BH_CHECK(Match(&ad, enabled) == SDL_BLE_FAMILY_DAYDREAM, "no match with only its hint on");
}

/* The finger and the stick over every touch value: (0, 0) is no finger, and
   either coordinate alone may be 0 while touching (daydream-catcher
   raw.rs:48-58) */
static void TestTouch(void)
{
    static const struct
    {
        int x, y;
        int16_t sx, sy;
    } cases[] = {
        { 0, 75, -32768, -13674 },
        { 136, 0, 2064, -32768 },
        { 255, 255, 32767, 32767 },
        { 1, 1, -32767, -32767 },
        { 128, 128, 0, 0 },
        { 127, 129, -258, 258 },
    };
    BH_Harness *h = Up();
    uint8_t packet[PACKET_SIZE];
    Fields f;
    uint64_t now = 1100;
    int16_t previous = -32768;
    bool mapped = true, rising = true, symmetric = true;
    int i, t;

    memset(&f, 0, sizeof(f));
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        f.touch_x = cases[i].x;
        f.touch_y = cases[i].y;
        Pack(&f, packet);
        Feed(h, packet, now);
        now += 25;
        BH_CHECK(BH_Controls(h)->finger, "(%d, %d): no finger", cases[i].x, cases[i].y);
        BH_CHECK(BH_Axis(h, SDL_BLE_AXIS_LEFTX) == cases[i].sx && BH_Axis(h, SDL_BLE_AXIS_LEFTY) == cases[i].sy,
                 "(%d, %d): stick (%d, %d)", cases[i].x, cases[i].y, BH_Axis(h, SDL_BLE_AXIS_LEFTX), BH_Axis(h, SDL_BLE_AXIS_LEFTY));
        BH_CHECK(ControlsMatch(BH_Controls(h), &f), "(%d, %d): finger position", cases[i].x, cases[i].y);
    }
    /* The finger lifts: every touch control back at rest */
    f.touch_x = 0;
    f.touch_y = 0;
    Pack(&f, packet);
    Feed(h, packet, now);
    now += 25;
    BH_CHECK(AtRest(BH_Controls(h)), "the finger did not lift");

    /* Every value of each coordinate */
    for (t = 0; t < 256; ++t) {
        f.touch_x = t;
        f.touch_y = 255 - t;
        Pack(&f, packet);
        Feed(h, packet, now);
        now += 25;
        if (!ControlsMatch(BH_Controls(h), &f)) {
            mapped = false;
            printf("  touch (%d, %d) mapped to (%d, %d)\n", t, 255 - t, BH_Axis(h, SDL_BLE_AXIS_LEFTX), BH_Axis(h, SDL_BLE_AXIS_LEFTY));
        }
        if (StickAxis(t) < previous) {
            rising = false;
        }
        if (t >= 1 && StickAxis(t) != -StickAxis(256 - t)) {
            symmetric = false;
        }
        previous = StickAxis(t);
    }
    BH_CHECK(mapped, "touch values");
    BH_CHECK(rising && symmetric, "the stick mapping");
    BH_CHECK(StickAxis(0) == -32768 && StickAxis(255) == 32767 && StickAxis(128) == 0, "the ends");
    BH_Destroy(h);
}

/* The battery level, a percentage, clamped to 100 (daydream-catcher lib.rs
   :28-36, Reference :31-34) */
static void TestBattery(void)
{
    static const struct
    {
        const char *hex;
        int8_t percent;
    } cases[] = {
        { "64", 100 }, { "00", 0 }, { "32", 50 }, { "65", 100 }, { "FF", 100 }, { "21 99", 33 },
    };
    BH_Harness *h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, false);
    uint8_t value[4], packet[PACKET_SIZE];
    size_t length;
    int i, n;

    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    BH_CHECK(BH_LastKind(h) == SDL_BLE_ACTION_READ && BH_Last(h)->characteristic == BATTERY, "no battery read");
    BH_ReadAll(h, (const uint8_t *)"\x4B", 1);
    BH_CHECK(BH_Controls(h)->battery == 75 && h->nsnapshots == 1, "read level %d", BH_Controls(h)->battery);
    BH_Value(h, BATTERY, (const uint8_t *)"\x4C", 1, false);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0 && h->nsamples == 0, "a battery value published or sampled");
    Decode(CAPTURE_1, packet);
    Feed(h, packet, 1000);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1, "not published");
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        length = BH_Hex(cases[i].hex, value, sizeof(value));
        BH_Value(h, BATTERY, value, length, false);
        BH_CHECK(BH_Controls(h)->battery == cases[i].percent, "battery %s: %d", cases[i].hex, BH_Controls(h)->battery);
        BH_CHECK(AtRest(BH_Controls(h)), "battery %s moved a control", cases[i].hex);
    }
    n = h->nsnapshots;
    BH_Value(h, BATTERY, value, 0, false);
    BH_CHECK(h->nsnapshots == n && BH_Controls(h)->battery == 33, "an empty value");
    Feed(h, packet, 1025);
    BH_CHECK(BH_Controls(h)->battery == 33, "a record changed the battery");
    BH_Destroy(h);
}

/* The ready rule: the joystick appears with the first 20-byte record after
   the start, never before and never from another value */
static void TestReady(void)
{
    BH_Harness *h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    uint8_t packet[PACKET_SIZE], longer[PACKET_SIZE + 1];

    Decode(CAPTURE_7, packet);
    memcpy(longer, packet, PACKET_SIZE);
    longer[PACKET_SIZE] = 0;
    BH_Connected(h, true, false);
    BH_Paired(h, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_CHECK(IsSubscribe(h, h->nactions - 1, POSE, SDL_BLE_CCCD_NOTIFY), "pose Notify");
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    /* A record while the subscriptions run is decoded but does not publish */
    Feed(h, packet, 100);
    BH_CHECK(BH_Controls(h)->buttons == RS_BIT && h->nsamples == 2, "record before the start not decoded");
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0, "published before the start");
    BH_SubscribeAll(h);
    BH_CHECK(BH_LastKind(h) == SDL_BLE_ACTION_READ, "no read");
    Feed(h, packet, 125);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0, "published while reading");
    BH_ReadAll(h, (const uint8_t *)"\x50", 1);
    BH_CHECK(h->session.phase == SDL_BLE_PHASE_STARTING && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0,
             "published by the start alone, phase %d", h->session.phase);
    BH_CHECK(!((const SDL_BLEBase *)h->state)->ready, "ready before a record after the start");
    /* After the start: 19 or 21 bytes and the battery do not publish */
    h->now = 150;
    BH_Value(h, POSE, packet, PACKET_SIZE - 1, false);
    BH_Value(h, POSE, longer, sizeof(longer), false);
    BH_Value(h, BATTERY, (const uint8_t *)"\x4F", 1, false);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0, "published by a bad length or the battery");
    Feed(h, packet, 175);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1 && h->session.phase == SDL_BLE_PHASE_RUNNING, "not published");
    Feed(h, packet, 200);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1, "published twice");
    BH_Destroy(h);
}

/* A gap releases every held control before its record applies */
static void TestGap(void)
{
    BH_Harness *h = Up();
    Fields f = captures[CAPTURE_7].fields;
    uint8_t held[PACKET_SIZE], rest[PACKET_SIZE];
    int n, samples;

    f.touch_x = 200;
    f.touch_y = 40;
    Pack(&f, held);
    Decode(CAPTURE_1, rest);
    Feed(h, held, 1100);

    n = h->nsnapshots;
    samples = h->nsamples;
    h->now = 1200;
    BH_Value(h, POSE, held, sizeof(held), true);
    BH_CHECK(h->nsnapshots == n + 2, "%d snapshots", h->nsnapshots - n);
    if (h->nsnapshots == n + 2) {
        BH_CHECK(AtRest(&h->snapshots[n].controls), "release");
        BH_CHECK(ControlsMatch(&h->snapshots[n + 1].controls, &f), "held again");
    }
    BH_CHECK(h->nsamples == samples + 2, "a gap dropped the samples");
    n = h->nsnapshots;
    h->now = 1300;
    BH_Value(h, POSE, rest, sizeof(rest), true);
    BH_CHECK(h->nsnapshots == n + 1 && AtRest(BH_Controls(h)), "gap before rest: %d snapshots", h->nsnapshots - n);
    n = h->nsnapshots;
    h->now = 1400;
    BH_Value(h, POSE, rest, sizeof(rest), true);
    BH_CHECK(h->nsnapshots == n, "gap at rest: %d snapshots", h->nsnapshots - n);
    BH_Destroy(h);
}

/* Values on an index the family does not have are ignored */
static void TestOtherCharacteristics(void)
{
    BH_Harness *h = Up();
    uint8_t packet[PACKET_SIZE];
    SDL_BLEControls before = *BH_Controls(h);
    int n = h->nsnapshots, samples = h->nsamples;

    Decode(CAPTURE_7, packet);
    SDL_BLEDaydreamModule.Value(h->state, 2, packet, sizeof(packet), 0);
    SDL_BLEDaydreamModule.Value(h->state, -1, packet, sizeof(packet), 0);
    BH_Value(h, 2, packet, sizeof(packet), false);
    BH_CHECK(h->nsnapshots == n && h->nsamples == samples && SDL_BLE_ControlsEqual(BH_Controls(h), &before),
             "another index changed the state");
    Feed(h, packet, 1100);
    BH_CHECK(h->nsnapshots == n + 1 && h->nsamples == samples + 2, "the pose index");
    BH_Destroy(h);
}

int main(void)
{
    Run("Table: family and identity", TestTable);
    Run("Test 1: capture 1, flat", Test1_Flat);
    Run("Test 2: the next two flat records", Test2_Next);
    Run("Test 3: capture 3, firmware 1.0.39", Test3_Firmware1039);
    Run("Test 4: capture 4, touching", Test4_Touch);
    Run("Test 5: capture 5, touch pad", Test5_TouchPad);
    Run("Test 6: capture 6, rotating", Test6_Rotating);
    Run("Test 7: capture 7, after pairing", Test7_Paired);
    Run("Test 8: buttons", Test8_Buttons);
    Run("Test 9: 13-bit fields", Test9_Signed13);
    Run("Test 10: lengths", Test10_Lengths);
    Run("Test 11: time wrap", Test11_Wrap);
    Run("Article stream: twenty flat records", TestArticleStream);
    Run("Test 12: pairing", Test12_Pairing);
    Run("Test 13: link loss and reconnect", Test13_LinkLoss);
    Run("Test 14: injected clock", Test14_Clock);
    Run("Discovery keys", TestDiscovery);
    Run("Touch values", TestTouch);
    Run("Battery", TestBattery);
    Run("Ready rule", TestReady);
    Run("Gap release", TestGap);
    Run("Other characteristics", TestOtherCharacteristics);
    return BH_Report("testbledaydream");
}
