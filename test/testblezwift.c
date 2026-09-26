/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/ble/SDL_ble_zwift_proto.c, the Zwift Play
   and Zwift Click of hifihedgehog/SDL#33 Part 12. Test numbers follow the
   part's Zwift section. Tests 1 to 4 and 8 replay the captures the part
   quotes from Makinolo's posts. Test 14, the key exchange itself, belongs
   to testblezwiftcrypto.c. Here a fake SDL_ZwiftCrypto stands in for it:
   MakeKey fills a fixed pattern, Derive records the device key, and
   Decrypt strips the 4 counter bytes and a fixed 4-byte tag it checks. */

#include "testbleharness.h"
#include "SDL_ble_zwift_proto.h"

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define BTN(name) (1u << SDL_BLE_BUTTON_##name)
#define DPAD (BTN(DPAD_UP) | BTN(DPAD_DOWN) | BTN(DPAD_LEFT) | BTN(DPAD_RIGHT))
#define FACES (BTN(SOUTH) | BTN(EAST) | BTN(WEST) | BTN(NORTH))

#define LEFT  SDL_ZWIFT_PLAY_LEFT
#define RIGHT SDL_ZWIFT_PLAY_RIGHT
#define CLICK SDL_ZWIFT_CLICK
#define FW2   SDL_ZWIFT_PLAY_FW2

#define START_MS 1000

static const uint8_t ride_on[6] = { 0x52, 0x69, 0x64, 0x65, 0x4F, 0x6E };
static const uint8_t fake_tag[4] = { 0xA5, 0x5A, 0xC3, 0x3C };
static const uint8_t variants[4] = { LEFT, RIGHT, CLICK, FW2 };
static const uint64_t zwift_address = 0xE1D2C3B4A596;

/* The fake key exchange */
typedef struct FakeCrypto
{
    SDL_ZwiftCrypto crypto;
    bool make_result;
    bool derive_result;
    int make_calls;
    int derive_calls;
    int decrypt_calls;
    uint8_t public_key[SDL_ZWIFT_KEY_SIZE];
    uint8_t device_key[SDL_ZWIFT_KEY_SIZE];
    uint8_t counter[4];
    size_t decrypt_length;
} FakeCrypto;

static bool Fake_MakeKey(void *userdata, uint8_t *public_key)
{
    FakeCrypto *fake = (FakeCrypto *)userdata;
    int i;

    ++fake->make_calls;
    if (!fake->make_result) {
        return false;
    }
    for (i = 0; i < SDL_ZWIFT_KEY_SIZE; ++i) {
        public_key[i] = (uint8_t)(0x40 + i);
    }
    memcpy(fake->public_key, public_key, SDL_ZWIFT_KEY_SIZE);
    return true;
}

static bool Fake_Derive(void *userdata, const uint8_t *device_key)
{
    FakeCrypto *fake = (FakeCrypto *)userdata;

    ++fake->derive_calls;
    memcpy(fake->device_key, device_key, SDL_ZWIFT_KEY_SIZE);
    return fake->derive_result;
}

static bool Fake_Decrypt(void *userdata, const uint8_t *frame, size_t length, uint8_t *plain)
{
    FakeCrypto *fake = (FakeCrypto *)userdata;

    ++fake->decrypt_calls;
    fake->decrypt_length = length;
    if (length <= SDL_ZWIFT_FRAME_EXTRA) {
        return false;
    }
    memcpy(fake->counter, frame, 4);
    if (memcmp(&frame[length - 4], fake_tag, sizeof(fake_tag)) != 0) {
        return false;
    }
    memcpy(plain, &frame[4], length - SDL_ZWIFT_FRAME_EXTRA);
    return true;
}

static void FakeInit(FakeCrypto *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->crypto.userdata = fake;
    fake->crypto.MakeKey = Fake_MakeKey;
    fake->crypto.Derive = Fake_Derive;
    fake->crypto.Decrypt = Fake_Decrypt;
    fake->make_result = true;
    fake->derive_result = true;
}

/* The device key a controller sends back: a pattern the fake public key lacks */
static void DeviceKey(uint8_t *key)
{
    int i;

    for (i = 0; i < SDL_ZWIFT_KEY_SIZE; ++i) {
        key[i] = (uint8_t)(0xC0 ^ (i * 7));
    }
}

/* An encrypted frame: the counter little-endian, the message, the fake tag */
static size_t Frame(uint8_t *out, uint32_t counter, const uint8_t *message, size_t length)
{
    out[0] = (uint8_t)counter;
    out[1] = (uint8_t)(counter >> 8);
    out[2] = (uint8_t)(counter >> 16);
    out[3] = (uint8_t)(counter >> 24);
    memcpy(&out[4], message, length);
    memcpy(&out[4 + length], fake_tag, sizeof(fake_tag));
    return length + SDL_ZWIFT_FRAME_EXTRA;
}

/* Protocol buffers writers for the constructed messages */
static size_t PutVarint(uint8_t *out, uint64_t value)
{
    size_t n = 0;

    do {
        uint8_t byte = (uint8_t)(value & 0x7F);

        value >>= 7;
        if (value) {
            byte |= 0x80;
        }
        out[n++] = byte;
    } while (value);
    return n;
}

static uint32_t ZigZagEncode(int32_t value)
{
    if (value < 0) {
        return ((uint32_t)(-(value + 1)) << 1) | 1u;
    }
    return (uint32_t)value << 1;
}

/* An 07 message: field 1 the pad (1 left, 0 right), fields 2 to 7 a
   button each (bit n of pressed for field n, 0 on the wire when pressed),
   then the lever in field 8 as zigzag and the pull in field 9 as its raw
   varint, Makinolo's amount from 0 to 200 */
static size_t Play(uint8_t *out, uint64_t pad, uint32_t pressed, int32_t lever, uint64_t pull)
{
    size_t n = 0;
    int field;

    out[n++] = SDL_ZWIFT_MESSAGE_PLAY;
    out[n++] = 0x08;
    n += PutVarint(&out[n], pad);
    for (field = 2; field <= 7; ++field) {
        out[n++] = (uint8_t)(field << 3);
        out[n++] = (pressed & (1u << field)) ? 0x00 : 0x01;
    }
    out[n++] = 0x40;
    n += PutVarint(&out[n], ZigZagEncode(lever));
    out[n++] = 0x48;
    n += PutVarint(&out[n], pull);
    return n;
}

/* One lever entry: field 1 location, field 2 value as zigzag */
static size_t Lever(uint8_t *out, uint64_t location, int32_t value)
{
    size_t n = 0;

    out[n++] = 0x08;
    n += PutVarint(&out[n], location);
    out[n++] = 0x10;
    n += PutVarint(&out[n], ZigZagEncode(value));
    return n;
}

/* A 23 message: the button map, then the lever entries, as repeated
   field 3 or inside one field 2 wrapper whose field 1 repeats */
static size_t Ride(uint8_t *out, uint32_t map, int nlevers, const uint64_t *locations, const int32_t *values, bool wrapped)
{
    uint8_t entries[256];
    size_t n = 0, e = 0;
    int i;

    out[n++] = SDL_ZWIFT_MESSAGE_RIDE;
    out[n++] = 0x08;
    n += PutVarint(&out[n], map);
    for (i = 0; i < nlevers; ++i) {
        uint8_t entry[32];
        size_t length = Lever(entry, locations[i], values[i]);

        if (wrapped) {
            entries[e++] = 0x0A;
            entries[e++] = (uint8_t)length;
            memcpy(&entries[e], entry, length);
            e += length;
        } else {
            out[n++] = 0x1A;
            out[n++] = (uint8_t)length;
            memcpy(&out[n], entry, length);
            n += length;
        }
    }
    if (wrapped) {
        out[n++] = 0x12;
        n += PutVarint(&out[n], e);
        memcpy(&out[n], entries, e);
        n += e;
    }
    return n;
}

/* Both levers at the given values */
static size_t RideLevers(uint8_t *out, uint32_t map, int32_t left, int32_t right, bool wrapped)
{
    const uint64_t locations[2] = { 0, 1 };
    const int32_t values[2] = { left, right };

    return Ride(out, map, 2, locations, values, wrapped);
}

static void Properties(uint8_t *properties)
{
    /* zwiftplay README.md:27-33 */
    memset(properties, 0, SDL_BLE_MAX_CHARS);
    properties[SDL_ZWIFT_ASYNC] = SDL_BLE_PROPERTY_NOTIFY;
    properties[SDL_ZWIFT_SYNC_TX] = SDL_BLE_PROPERTY_INDICATE | SDL_BLE_PROPERTY_READ;
    properties[SDL_ZWIFT_SYNC_RX] = SDL_BLE_PROPERTY_WRITE | SDL_BLE_PROPERTY_WRITE_WITHOUT_RESPONSE;
}

/* A session through both subscriptions, the RideOn write out and not yet
   answered */
static BH_Harness *Subscribed(uint8_t variant, const FakeCrypto *fake)
{
    BH_Harness *h = BH_Create(&SDL_BLEZwiftFamily, variant, fake ? &fake->crypto : NULL, false);
    uint8_t properties[SDL_BLE_MAX_CHARS];

    h->now = START_MS;
    Properties(properties);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    return h;
}

/* The same with the RideOn write answered at START_MS */
static BH_Harness *Handshaken(uint8_t variant, const FakeCrypto *fake)
{
    BH_Harness *h = Subscribed(variant, fake);

    BH_WriteAll(h);
    return h;
}

static void Async(BH_Harness *h, const uint8_t *data, size_t length)
{
    BH_Value(h, SDL_ZWIFT_ASYNC, data, length, false);
}

static void AsyncHex(BH_Harness *h, const char *hex)
{
    uint8_t data[256];
    size_t length = BH_Hex(hex, data, sizeof(data));

    Async(h, data, length);
}

static void SyncTx(BH_Harness *h, const uint8_t *data, size_t length)
{
    BH_Value(h, SDL_ZWIFT_SYNC_TX, data, length, false);
}

/* A published session in plain mode */
static BH_Harness *Plain(uint8_t variant)
{
    BH_Harness *h = Handshaken(variant, NULL);

    AsyncHex(h, "15");
    return h;
}

static const SDL_ZwiftState *Zwift(const BH_Harness *h)
{
    return (const SDL_ZwiftState *)h->state;
}

/* The trigger value the module writes for a pull, as decided */
/* SDL's trigger binding of a full joystick axis, SDL_gamepad.c:3779-3780,
   with the trigger output range 0 to 32767 (SDL_gamepad.c:1855-1856) */
static int GamepadTrigger(int joystick)
{
    const int axis_min = -32768, axis_max = 32767, out_min = 0, out_max = 32767;
    float normalized_value = (float)(joystick - axis_min) / (axis_max - axis_min);

    return out_min + (int)(normalized_value * (out_max - out_min));
}

/* The smallest joystick value that SDL's binding maps to the part's gamepad
   value v x 32767 / 100, found by search, independent of the module's
   arithmetic */
static int JoystickTrigger(int value)
{
    static int found[101];
    static bool searched;
    const int v = (value < 0) ? 0 : (value > 100) ? 100 : value;

    if (!searched) {
        int pull, joystick = -32768;

        for (pull = 0; pull <= 100; ++pull) {
            const int want = pull * 32767 / 100;

            while (joystick < 32767 && GamepadTrigger(joystick) < want) {
                ++joystick;
            }
            found[pull] = joystick;
        }
        searched = true;
    }
    return found[v];
}

static int AxisOf(int value)
{
    const int v = (value < -100) ? -100 : (value > 100) ? 100 : value;

    return v * 32767 / 100;
}

/* Checks every control. The axes a variant lacks stay at rest. */
static void Expect(const BH_Harness *h, uint32_t buttons, int leftx, int rightx, int left_trigger, int right_trigger,
                   const char *what)
{
    const SDL_BLEControls *c = BH_Controls(h);

    BH_CHECK(c->buttons == buttons, "%s: buttons %08x, expected %08x", what, (unsigned)c->buttons, (unsigned)buttons);
    BH_CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == leftx, "%s: left X %d, expected %d", what, c->axes[SDL_BLE_AXIS_LEFTX], leftx);
    BH_CHECK(c->axes[SDL_BLE_AXIS_RIGHTX] == rightx, "%s: right X %d, expected %d", what, c->axes[SDL_BLE_AXIS_RIGHTX], rightx);
    BH_CHECK(c->axes[SDL_BLE_AXIS_LEFT_TRIGGER] == left_trigger, "%s: left trigger %d, expected %d", what,
             c->axes[SDL_BLE_AXIS_LEFT_TRIGGER], left_trigger);
    BH_CHECK(c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == right_trigger, "%s: right trigger %d, expected %d", what,
             c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER], right_trigger);
    BH_CHECK(c->axes[SDL_BLE_AXIS_LEFTY] == 0 && c->axes[SDL_BLE_AXIS_RIGHTY] == 0, "%s: a Y axis moved", what);
}

/* The rest triggers of a variant: the ones it has sit at -32768 */
static int RestLeftTrigger(uint8_t variant)
{
    return (variant == LEFT) ? -32768 : 0;
}

static int RestRightTrigger(uint8_t variant)
{
    return (variant == RIGHT) ? -32768 : 0;
}

/* A value that must change nothing: no snapshot, no controls moved */
static void Unchanged(BH_Harness *h, int characteristic, const uint8_t *data, size_t length, const char *what)
{
    const SDL_BLEControls before = *BH_Controls(h);
    const int snapshots = h->nsnapshots;

    BH_Value(h, characteristic, data, length, false);
    BH_CHECK(h->nsnapshots == snapshots && SDL_BLE_ControlsEqual(BH_Controls(h), &before), "%s: changed the controls", what);
}

static void UnchangedHex(BH_Harness *h, const char *hex, const char *what)
{
    uint8_t data[256];
    size_t length = BH_Hex(hex, data, sizeof(data));

    Unchanged(h, SDL_ZWIFT_ASYNC, data, length, what);
}

/* No timer may sit at or before the clock after it has run */
static void NoSpentDeadline(BH_Harness *h, const char *what)
{
    uint64_t deadline;

    if (SDL_BLESession_GetDeadline(&h->session, &deadline)) {
        BH_CHECK(deadline > h->now, "%s: a deadline of %llu is spent at %llu", what, (unsigned long long)deadline,
                 (unsigned long long)h->now);
    }
}

static void TestIdentity(void)
{
    static const struct
    {
        uint8_t variant;
        const char *name;
        uint32_t buttons;
        uint8_t axes;
    } expected[4] = {
        { LEFT, "Zwift Play (L)", DPAD | BTN(LEFT_SHOULDER) | BTN(BACK),
          (1u << SDL_BLE_AXIS_LEFTX) | (1u << SDL_BLE_AXIS_LEFT_TRIGGER) },
        { RIGHT, "Zwift Play (R)", FACES | BTN(RIGHT_SHOULDER) | BTN(START),
          (1u << SDL_BLE_AXIS_RIGHTX) | (1u << SDL_BLE_AXIS_RIGHT_TRIGGER) },
        { CLICK, "Zwift Click", BTN(LEFT_SHOULDER) | BTN(RIGHT_SHOULDER), 0 },
        { FW2, "Zwift Play", DPAD | FACES | BTN(LEFT_SHOULDER) | BTN(RIGHT_SHOULDER) | BTN(BACK) | BTN(START),
          (1u << SDL_BLE_AXIS_LEFTX) | (1u << SDL_BLE_AXIS_RIGHTX) }
    };
    size_t i;

    printf("Identity\n");
    for (i = 0; i < COUNT(expected); ++i) {
        BH_Harness *h = BH_Create(&SDL_BLEZwiftFamily, expected[i].variant, NULL, false);
        const SDL_BLEBase *base = (const SDL_BLEBase *)h->state;

        BH_CHECK(strcmp(base->identity.name, expected[i].name) == 0, "type %02X: name %s", expected[i].variant,
                 base->identity.name);
        BH_CHECK(base->identity.type == SDL_BLE_TYPE_GAMEPAD && base->identity.gamepad, "type %02X: a gamepad",
                 expected[i].variant);
        BH_CHECK(base->identity.buttons == expected[i].buttons, "type %02X: buttons %08x", expected[i].variant,
                 (unsigned)base->identity.buttons);
        BH_CHECK(base->identity.axes == expected[i].axes, "type %02X: axes %02x", expected[i].variant,
                 base->identity.axes);
        BH_CHECK(base->identity.vendor == 0 && base->identity.product == 0 && !base->identity.touchpad &&
                     base->identity.accel_rate == 0.0f && base->identity.gyro_rate == 0.0f,
                 "type %02X: no VID, PID, touchpad or sensors", expected[i].variant);
        Expect(h, 0, 0, 0, RestLeftTrigger(expected[i].variant), RestRightTrigger(expected[i].variant), "at rest");
        BH_CHECK(base->controls.battery == -1, "type %02X: the battery starts unknown", expected[i].variant);
        BH_Destroy(h);
    }

    /* The family: ASYNC notify, SYNC_TX subscribed, SYNC_RX written, no pairing */
    BH_CHECK(SDL_BLEZwiftFamily.ncharacteristics == 3 &&
                 SDL_BLEZwiftFamily.characteristics[SDL_ZWIFT_ASYNC].flags == SDL_BLE_CHAR_SUBSCRIBE &&
                 SDL_BLEZwiftFamily.characteristics[SDL_ZWIFT_SYNC_TX].flags == SDL_BLE_CHAR_SUBSCRIBE &&
                 SDL_BLEZwiftFamily.characteristics[SDL_ZWIFT_SYNC_RX].flags == 0,
             "the family's characteristics");
    BH_CHECK(SDL_BLEZwiftFamily.pairing == SDL_BLE_PAIR_NOT_NEEDED, "the Zwift controllers never pair");
}

static void Test1(void)
{
    BH_Harness *h = Plain(RIGHT);
    uint8_t built[64], capture[64];
    size_t length = BH_Hex("07 08 00 10 01 18 01 20 00 28 01 30 01 38 01 40 00 48 00", capture, sizeof(capture));

    printf("Test 1: Makinolo's right half, A pressed\n");
    BH_CHECK(Play(built, 0, 1u << 4, 0, 0) == length && memcmp(built, capture, length) == 0,
             "the message builder reproduces the capture");
    Async(h, capture, length);
    Expect(h, BTN(EAST), 0, 0, 0, -32768, "test 1");
    BH_Destroy(h);
}

static void Test2to4(void)
{
    BH_Harness *h = Plain(RIGHT);

    printf("Test 2: Makinolo's lever to the right\n");
    AsyncHex(h, "07 08 00 10 01 18 01 20 00 28 01 30 01 38 01 40 00 48 00");
    AsyncHex(h, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 BE 01 48 00");
    Expect(h, 0, 0, 31128, 0, -32768, "test 2");
    BH_CHECK(SDL_Zwift_ZigZag(190) == 95, "190 reads 95");

    printf("Test 3: Makinolo's lever to the left\n");
    AsyncHex(h, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 6B 48 00");
    Expect(h, 0, 0, -17694, 0, -32768, "test 3");
    BH_CHECK(SDL_Zwift_ZigZag(107) == -54, "107 reads -54");

    printf("Test 4: Makinolo's full pull\n");
    AsyncHex(h, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 C8 01");
    Expect(h, 0, 0, 0, 0, 32767, "test 4");
    BH_CHECK(GamepadTrigger(32767) == 32767, "200, the most Makinolo reads, is a full trigger");
    BH_Destroy(h);
}

static void Test5(void)
{
    static const uint32_t left[6] = { BTN(DPAD_UP), BTN(DPAD_LEFT), BTN(DPAD_RIGHT), BTN(DPAD_DOWN), BTN(LEFT_SHOULDER),
                                      BTN(BACK) };
    static const uint32_t right[6] = { BTN(NORTH), BTN(WEST), BTN(EAST), BTN(SOUTH), BTN(RIGHT_SHOULDER), BTN(START) };
    BH_Harness *hl = Plain(LEFT);
    BH_Harness *hr = Plain(RIGHT);
    uint8_t message[64], capture[64];
    size_t length, captured;
    char what[64];
    int field;

    printf("Test 5: every button of both halves\n");
    captured = BH_Hex("07 08 01 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 00", capture, sizeof(capture));
    length = Play(message, 1, 0, 0, 0);
    BH_CHECK(length == captured && memcmp(message, capture, length) == 0, "the left rest message is the part's");
    Async(hl, capture, captured);
    Expect(hl, 0, 0, 0, -32768, 0, "left at rest");
    for (field = 2; field <= 7; ++field) {
        /* The part's construction: the value after tag field << 3 set to 00 */
        memcpy(message, capture, captured);
        BH_CHECK(message[2 * field - 1] == (uint8_t)(field << 3), "field %d sits where the part says", field);
        message[2 * field] = 0x00;
        Async(hl, message, captured);
        (void)snprintf(what, sizeof(what), "left field %d", field);
        Expect(hl, left[field - 2], 0, 0, -32768, 0, what);

        message[2] = 0x00; /* the right pad */
        Async(hr, message, captured);
        (void)snprintf(what, sizeof(what), "right field %d", field);
        Expect(hr, right[field - 2], 0, 0, 0, -32768, what);
    }
    /* All six at once */
    length = Play(message, 1, 0xFC, 0, 0);
    Async(hl, message, length);
    Expect(hl, left[0] | left[1] | left[2] | left[3] | left[4] | left[5], 0, 0, -32768, 0, "every left button");
    length = Play(message, 0, 0xFC, 0, 0);
    Async(hr, message, length);
    Expect(hr, right[0] | right[1] | right[2] | right[3] | right[4] | right[5], 0, 0, 0, -32768, "every right button");

    /* Any value but 0 is released, as zwiftplay compares with 0
       (ControllerNotification.kt:12, :51-56) */
    length = BH_Hex("07 08 01 10 02 18 7F 20 80 01 28 01 30 05 38 FF FF FF FF 0F 40 00 48 00", message, sizeof(message));
    Async(hl, message, length);
    Expect(hl, 0, 0, 0, -32768, 0, "values other than 0 release");
    BH_Destroy(hl);
    BH_Destroy(hr);
}

static void Test6(void)
{
    BH_Harness *hl = Plain(LEFT);
    BH_Harness *hr = Plain(RIGHT);

    printf("Test 6: extremes\n");
    AsyncHex(hl, "07 08 01 10 01 18 01 20 01 28 01 30 01 38 01 40 C7 01 48 00");
    Expect(hl, 0, -32767, 0, -32768, 0, "left lever C7 01");
    AsyncHex(hl, "07 08 01 10 01 18 01 20 01 28 01 30 01 38 01 40 C8 01 48 00");
    Expect(hl, 0, 32767, 0, -32768, 0, "left lever C8 01");
    AsyncHex(hr, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 C7 01 48 00");
    Expect(hr, 0, 0, -32767, 0, -32768, "right lever C7 01");
    AsyncHex(hr, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 C8 01 48 00");
    Expect(hr, 0, 0, 32767, 0, -32768, "right lever C8 01");
    AsyncHex(hr, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 C8 01");
    Expect(hr, 0, 0, 0, 0, 32767, "right pull C8 01");
    /* Field 9 is the raw varint shifted right by one, Makinolo's amount whose
       low bit means nothing: C7 01 is 199, a pull of 99. The trigger's
       joystick value is ceil(32439 x 65535 / 32767) - 32768 = 32111, which
       SDL's binding reads as 99 x 32767 / 100 = 32439. */
    AsyncHex(hr, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 C7 01");
    Expect(hr, 0, 0, 0, 0, 32111, "right pull C7 01 is 99 percent");
    AsyncHex(hl, "07 08 01 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 C8 01");
    Expect(hl, 0, 0, 0, 32767, 0, "left pull C8 01");
    AsyncHex(hl, "07 08 01 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 C7 01");
    Expect(hl, 0, 0, 0, 32111, 0, "left pull C7 01 is 99 percent");
    BH_CHECK(JoystickTrigger(99) == 32111 && GamepadTrigger(32111) == 32439, "99 percent is joystick 32111, gamepad 32439");
    BH_CHECK(SDL_Zwift_ZigZag(199) == -100, "199 reads -100 as a lever");
    BH_Destroy(hl);
    BH_Destroy(hr);
}

static void Test7(void)
{
    BH_Harness *h = Plain(CLICK);

    printf("Test 7: Click\n");
    AsyncHex(h, "37 08 00 10 01");
    Expect(h, BTN(RIGHT_SHOULDER), 0, 0, 0, 0, "plus");
    AsyncHex(h, "37 08 01 10 00");
    Expect(h, BTN(LEFT_SHOULDER), 0, 0, 0, 0, "minus");
    AsyncHex(h, "37 08 01 10 01");
    Expect(h, 0, 0, 0, 0, 0, "neither");
    AsyncHex(h, "37 08 00 10 00");
    Expect(h, BTN(LEFT_SHOULDER) | BTN(RIGHT_SHOULDER), 0, 0, 0, 0, "both");
    AsyncHex(h, "37 10 00 08 01");
    Expect(h, BTN(LEFT_SHOULDER), 0, 0, 0, 0, "fields in either order");
    BH_Destroy(h);
}

static void Test8to10(void)
{
    BH_Harness *h = Plain(FW2);
    uint8_t message[128], capture[128];
    size_t length, captured;

    printf("Test 8: Makinolo's Ride-style capture, field 2 wrapper\n");
    captured = BH_Hex("23 08 FE FF FF FF 0F 12 18 0A 04 08 00 10 00 0A 04 08 01 10 00 0A 04 08 02 10 00 0A 04 08 03 10 00",
                      capture, sizeof(capture));
    {
        const uint64_t locations[4] = { 0, 1, 2, 3 };
        const int32_t values[4] = { 0, 0, 0, 0 };

        length = Ride(message, 0xFFFFFFFE, 4, locations, values, true);
        BH_CHECK(length == captured && memcmp(message, capture, length) == 0, "the builder reproduces the capture");
    }
    Async(h, capture, captured);
    Expect(h, BTN(DPAD_LEFT), 0, 0, 0, 0, "test 8");

    printf("Test 9: field 3 entries\n");
    AsyncHex(h, "23 08 FF FF FF FF 0F 1A 05 08 00 10 C8 01 1A 04 08 01 10 00");
    Expect(h, 0, 32767, 0, 0, 0, "test 9");
    length = RideLevers(message, 0xFFFFFFFF, 100, 0, false);
    captured = BH_Hex("23 08 FF FF FF FF 0F 1A 05 08 00 10 C8 01 1A 04 08 01 10 00", capture, sizeof(capture));
    BH_CHECK(length == captured && memcmp(message, capture, length) == 0, "the builder reproduces test 9");

    printf("Test 10: SwiftControl's released frame, a map without levers\n");
    /* What a key-up emits (controller_keep_alive.dart:3-6): every button
       released, and both levers at rest, since a message without a lever's
       entry releases it, as qdomyos-zwift (abstractZapDevice.h:342, :439)
       and SwiftControl (zwift_ride.dart:214-239) read it */
    AsyncHex(h, "23 08 FE FF FF FF 0F 1A 05 08 00 10 C8 01 1A 04 08 01 10 27");
    Expect(h, BTN(DPAD_LEFT), 32767, AxisOf(-20), 0, 0, "left pressed and both levers moved before the released frame");
    AsyncHex(h, "23 08 FF FF FF FF 0F");
    Expect(h, 0, 0, 0, 0, 0, "the released frame");
    /* The emulator's press is a map alone too (ftms_mdns_emulator.dart:150-160):
       Y, mask 40, and both levers rest */
    AsyncHex(h, "23 08 FE FF FF FF 0F 1A 05 08 00 10 C8 01 1A 04 08 01 10 27");
    AsyncHex(h, "23 08 BF FF FF FF 0F");
    Expect(h, BTN(NORTH), 0, 0, 0, 0, "a map-only press");
    /* An entry moves its own lever, in either form, and the other rests */
    AsyncHex(h, "23 08 FE FF FF FF 0F 1A 05 08 00 10 C8 01 1A 04 08 01 10 27");
    AsyncHex(h, "23 08 FF FF FF FF 0F 1A 04 08 01 10 14");
    Expect(h, 0, 0, AxisOf(10), 0, 0, "lever 1 alone");
    AsyncHex(h, "23 08 FF FF FF FF 0F 12 06 0A 04 08 00 10 13");
    Expect(h, 0, AxisOf(-10), 0, 0, 0, "lever 0 alone, in the wrapper");
    /* Levers without the map change nothing */
    UnchangedHex(h, "23 1A 04 08 00 10 14 1A 04 08 01 10 13", "levers without the map");
    BH_Destroy(h);

    /* The released frame decodes, so it publishes */
    h = Handshaken(FW2, NULL);
    AsyncHex(h, "23 08 FF FF FF FF 0F");
    BH_CHECK(h->published, "the released frame alone publishes");
    Expect(h, 0, 0, 0, 0, 0, "the released frame at rest");
    BH_Destroy(h);
}

static void Test11(void)
{
    size_t i;

    printf("Test 11: battery, idle and device information\n");
    for (i = 0; i < COUNT(variants); ++i) {
        BH_Harness *h = Plain(variants[i]);

        AsyncHex(h, "19 10 59");
        BH_CHECK(BH_Controls(h)->battery == 89, "type %02X: 19 10 59 is 89 percent", variants[i]);
        UnchangedHex(h, "15", "idle");
        UnchangedHex(h, "3C 00", "device information");
        UnchangedHex(h, "FE", "disconnect");
        BH_CHECK(BH_Controls(h)->battery == 89, "type %02X: the battery holds", variants[i]);
        BH_Destroy(h);
    }
}

/* Every value from 1 byte short of full down to nothing */
static void Truncations(BH_Harness *h, const uint8_t *message, size_t length, const char *what)
{
    char label[96];
    size_t cut;

    for (cut = 0; cut < length; ++cut) {
        (void)snprintf(label, sizeof(label), "%s cut to %u bytes", what, (unsigned)cut);
        Unchanged(h, SDL_ZWIFT_ASYNC, message, cut, label);
    }
}

static void Test12(void)
{
    static const char *const right_messages[] = {
        "07 08 00 10 01 18 01 20 00 28 01 30 01 38 01 40 00 48 00",
        "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 BE 01 48 00",
        "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 6B 48 00",
        "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 C8 01",
        "07 08 00 10 00 18 00 20 00 28 00 30 00 38 00 40 00 48 00",
    };
    static const char *const click_messages[] = { "37 08 00 10 01", "37 08 01 10 00", "37 08 00 10 00" };
    static const char *const fw2_messages[] = {
        "23 08 FE FF FF FF 0F 12 18 0A 04 08 00 10 00 0A 04 08 01 10 00 0A 04 08 02 10 00 0A 04 08 03 10 00",
        "23 08 FF FF FF FF 0F 1A 05 08 00 10 C8 01 1A 04 08 01 10 00",
    };
    uint8_t message[128], other[64], buffer[64];
    size_t length, other_length, i;
    BH_Harness *h;

    printf("Test 12: truncations and malformed values\n");

    /* Right half: every truncation of tests 1 to 5, from a state none of
       them produces */
    h = Plain(RIGHT);
    other_length = Play(other, 0, 0xFC, -77, 33);
    for (i = 0; i < COUNT(right_messages); ++i) {
        Async(h, other, other_length);
        length = BH_Hex(right_messages[i], message, sizeof(message));
        Truncations(h, message, length, right_messages[i]);
    }
    BH_Destroy(h);

    /* Left half: test 5's left messages */
    h = Plain(LEFT);
    other_length = Play(other, 1, 0x00, 55, 12);
    for (i = 2; i <= 7; ++i) {
        Async(h, other, other_length);
        length = Play(message, 1, 1u << i, 0, 0);
        Truncations(h, message, length, "left test 5");
    }
    BH_Destroy(h);

    h = Plain(CLICK);
    for (i = 0; i < COUNT(click_messages); ++i) {
        AsyncHex(h, "37 08 01 10 01");
        length = BH_Hex(click_messages[i], message, sizeof(message));
        Truncations(h, message, length, click_messages[i]);
    }
    BH_Destroy(h);

    /* Firmware 2.0.1: a cut inside a field changes nothing, but a cut at a
       field boundary after the button map leaves a whole message, whose map
       applies with the levers it holds, and a lever it holds no entry for
       rests (test 10). Test 8 has one such cut, after the map, and test 9
       two, after the map and after lever 0. */
    h = Plain(FW2);
    other_length = RideLevers(other, 0xFFFF0000, -60, 60, false);
    for (i = 0; i < COUNT(fw2_messages); ++i) {
        size_t cut;

        length = BH_Hex(fw2_messages[i], message, sizeof(message));
        for (cut = 0; cut < length; ++cut) {
            char label[128];

            Async(h, other, other_length);
            (void)snprintf(label, sizeof(label), "%s cut to %u bytes", fw2_messages[i], (unsigned)cut);
            if (cut == 7) {
                Async(h, message, cut);
                Expect(h, (i == 0) ? BTN(DPAD_LEFT) : 0, 0, 0, 0, 0, label);
            } else if (i == 1 && cut == 14) {
                Async(h, message, cut);
                Expect(h, 0, 32767, 0, 0, 0, label);
            } else {
                Unchanged(h, SDL_ZWIFT_ASYNC, message, cut, label);
            }
        }
    }
    BH_Destroy(h);

    /* The part's malformed values, on the right half */
    h = Plain(RIGHT);
    AsyncHex(h, "07 08 00 10 01 18 01 20 00 28 01 30 01 38 01 40 00 48 00");
    UnchangedHex(h, "07 08", "a cut-off varint");
    UnchangedHex(h, "07 08 00 12 01 00 18 01 20 01 28 01 30 01 38 01 40 00 48 00", "a length-delimited field 2");
    UnchangedHex(h, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 00 52 05 01 02", "a length past the end");
    AsyncHex(h, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 00 50 01 5A 02 AA BB");
    Expect(h, 0, 0, 0, 0, -32768, "unknown fields 10 and 11 are skipped");

    /* Stale bytes after the value: the module reads length bytes only */
    length = BH_Hex("07 08 00 10 01 18 01 20 00 28 01 30 01 38 01 40 00 48 00 20 01 40 C8 01", buffer, sizeof(buffer));
    SDL_BLEZwiftModule.Value(h->state, SDL_ZWIFT_ASYNC, buffer, length - 5, h->now * 1000000);
    Expect(h, BTN(EAST), 0, 0, 0, -32768, "stale bytes after a 07 message");
    length = BH_Hex("19 10 59 10 64", buffer, sizeof(buffer));
    SDL_BLEZwiftModule.Value(h->state, SDL_ZWIFT_ASYNC, buffer, length - 2, h->now * 1000000);
    BH_CHECK(BH_Controls(h)->battery == 89, "stale bytes after a 19 message");
    BH_Destroy(h);
}

/* The protocol buffers reader, on the battery message and the others */
static void TestReader(void)
{
    static const struct
    {
        const char *hex;
        int battery; /* -1: the value is rejected */
        const char *what;
    } battery[] = {
        { "19 10 59", 89, "plain" },
        { "19 08 05 10 59", 89, "an unknown varint field before" },
        { "19 10 59 08 05", 89, "an unknown varint field after" },
        { "19 19 01 02 03 04 05 06 07 08 10 59", 89, "an unknown 64-bit field" },
        { "19 1A 03 AA BB CC 10 59", 89, "an unknown length-delimited field" },
        { "19 1A 00 10 59", 89, "an empty length-delimited field" },
        { "19 1D 01 02 03 04 10 59", 89, "an unknown 32-bit field" },
        { "19 1B 08 01 1C 10 59", 89, "an unknown group" },
        { "19 1B 1C 10 59", 89, "an empty group" },
        { "19 1B 19 01 02 03 04 05 06 07 08 22 01 AA 2D 01 02 03 04 1C 10 59", 89, "a group of every payload" },
        { "19 1B 23 08 01 24 1C 10 59", 89, "nested groups" },
        { "19 F8 FF FF FF 0F 01 10 59", 89, "field 536870911, the largest" },
        { "19 10 D9 80 80 80 80 80 80 80 80 00", 89, "a 10-byte varint" },
        { "19 10 00", 0, "zero" },
        { "19 10 64", 100, "100 percent" },
        { "19 10 65", 100, "101 clamps to 100" },
        { "19 10 C8 01", 100, "200 clamps to 100" },
        { "19 10 FF FF FF FF 0F", 100, "a 32-bit value clamps to 100" },
        { "19 10 FF FF FF FF FF FF FF FF FF 01", 100, "a 64-bit value clamps to 100" },
        { "19 10 20 10 30", 48, "a repeated field: the last wins" },
        { "19", -1, "no field 2" },
        { "19 08 59", -1, "field 1 only" },
        { "19 10", -1, "a cut-off value" },
        { "19 10 D9", -1, "a varint cut off after its first byte" },
        { "19 90", -1, "a cut-off tag" },
        { "19 10 FF FF FF FF FF FF FF FF FF FF 01", -1, "an 11-byte varint" },
        { "19 00 10 59", -1, "field number 0" },
        { "19 80 80 80 80 10 01 10 59", -1, "field number 536870912" },
        { "19 12 01 59", -1, "field 2 length-delimited" },
        { "19 11 59 00 00 00 00 00 00 00", -1, "field 2 as 64 bits" },
        { "19 15 59 00 00 00", -1, "field 2 as 32 bits" },
        { "19 13 14", -1, "field 2 as a group" },
        { "19 16 59 10 59", -1, "wire type 6" },
        { "19 17 59 10 59", -1, "wire type 7" },
        { "19 1C 10 59", -1, "a group end with no group open" },
        { "19 1B 24 10 59", -1, "a group closed by another field number" },
        { "19 1B 08 01", -1, "a group never closed" },
        { "19 1B 0A 05 01 1C 10 59", -1, "a group holding a length past the end" },
        { "19 1B 0E 1C 10 59", -1, "a group holding wire type 6" },
        { "19 1A 04 AA BB CC", -1, "a length past the end" },
        { "19 1A FF FF FF FF 0F AA", -1, "a huge length" },
        { "19 19 01 02 03 04 05 06 07", -1, "64 bits cut short" },
        { "19 1D 01 02 03", -1, "32 bits cut short" },
        { "19 10 59 08", -1, "a cut-off field after field 2" },
    };
    uint8_t message[256];
    size_t length, i;
    int depth;
    BH_Harness *h = Plain(RIGHT);

    printf("Protocol buffers reader\n");
    for (i = 0; i < COUNT(battery); ++i) {
        SDL_BLEControls before;

        AsyncHex(h, "19 10 07");
        before = *BH_Controls(h);
        length = BH_Hex(battery[i].hex, message, sizeof(message));
        Async(h, message, length);
        if (battery[i].battery < 0) {
            BH_CHECK(SDL_BLE_ControlsEqual(BH_Controls(h), &before), "%s (%s): rejected", battery[i].hex, battery[i].what);
        } else {
            BH_CHECK(BH_Controls(h)->battery == battery[i].battery, "%s (%s): battery %d, expected %d", battery[i].hex,
                     battery[i].what, BH_Controls(h)->battery, battery[i].battery);
        }
    }

    /* Groups nest 16 deep and no deeper */
    for (depth = 15; depth <= 17; ++depth) {
        int d;

        AsyncHex(h, "19 10 07");
        length = 0;
        message[length++] = 0x19;
        for (d = 0; d < depth; ++d) {
            message[length++] = 0x1B;
        }
        for (d = 0; d < depth; ++d) {
            message[length++] = 0x1C;
        }
        message[length++] = 0x10;
        message[length++] = 0x59;
        Async(h, message, length);
        BH_CHECK(BH_Controls(h)->battery == ((depth <= 16) ? 89 : 7), "groups %d deep", depth);
    }

    /* Wrong wire types for every known field of an 07 message */
    AsyncHex(h, "07 08 00 10 01 18 01 20 00 28 01 30 01 38 01 40 00 48 00");
    for (i = 1; i <= 9; ++i) {
        static const char *const forms[4] = { "1 (64 bits)", "2 (length-delimited)", "5 (32 bits)", "3 (a group)" };
        size_t form;

        for (form = 0; form < 4; ++form) {
            char what[96];
            size_t n = 0;
            size_t field;

            message[n++] = SDL_ZWIFT_MESSAGE_PLAY;
            for (field = 1; field <= 9; ++field) {
                if (field != i) {
                    message[n++] = (uint8_t)(field << 3);
                    message[n++] = (field == 1) ? 0x00 : 0x01;
                } else if (form == 0) {
                    message[n++] = (uint8_t)((field << 3) | 1);
                    memset(&message[n], 0, 8);
                    n += 8;
                } else if (form == 1) {
                    message[n++] = (uint8_t)((field << 3) | 2);
                    message[n++] = 0x01;
                    message[n++] = 0x00;
                } else if (form == 2) {
                    message[n++] = (uint8_t)((field << 3) | 5);
                    memset(&message[n], 0, 4);
                    n += 4;
                } else {
                    message[n++] = (uint8_t)((field << 3) | 3);
                    message[n++] = (uint8_t)((field << 3) | 4);
                }
            }
            (void)snprintf(what, sizeof(what), "07 field %u as wire type %s", (unsigned)i, forms[form]);
            Unchanged(h, SDL_ZWIFT_ASYNC, message, n, what);
        }
    }

    /* Unknown fields of every wire type between the known ones */
    AsyncHex(h, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 00 "
                "50 01 59 01 02 03 04 05 06 07 08 62 02 AA BB 6B 08 01 6C 75 01 02 03 04");
    Expect(h, 0, 0, 0, 0, -32768, "unknown fields 10 to 14 after field 9");
    AsyncHex(h, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 00");
    Expect(h, 0, 0, 0, 0, -32768, "nothing held before the next message");
    AsyncHex(h, "07 50 01 08 00 59 01 02 03 04 05 06 07 08 10 01 62 02 AA BB 18 01 6B 08 01 6C 20 00 "
                "75 01 02 03 04 28 01 30 01 38 01 40 00 48 00");
    Expect(h, BTN(EAST), 0, 0, 0, -32768, "unknown fields between the known ones");
    AsyncHex(h, "07 08 00 10 01 18 01 20 00 28 01 30 01 38 01 40 00 48 00 20 01");
    Expect(h, 0, 0, 0, 0, -32768, "a repeated field 4: the last wins");
    BH_Destroy(h);

    /* The Click and the Ride-style message */
    h = Plain(CLICK);
    AsyncHex(h, "37 08 00 10 00");
    UnchangedHex(h, "37 0A 01 00 10 01", "37 field 1 length-delimited");
    UnchangedHex(h, "37 08 01 12 01 00", "37 field 2 length-delimited");
    UnchangedHex(h, "37 0D 00 00 00 00 10 01", "37 field 1 as 32 bits");
    UnchangedHex(h, "37 08 01", "37 without field 2");
    UnchangedHex(h, "37 10 01", "37 without field 1");
    AsyncHex(h, "37 18 00 08 01 22 01 00 10 01 2D 01 02 03 04");
    Expect(h, 0, 0, 0, 0, 0, "37 with unknown fields");
    BH_Destroy(h);

    h = Plain(FW2);
    AsyncHex(h, "23 08 FE FF FF FF 0F 1A 04 08 00 10 00 1A 04 08 01 10 00");
    UnchangedHex(h, "23 0A 01 00 1A 04 08 00 10 00 1A 04 08 01 10 00", "23 map length-delimited");
    UnchangedHex(h, "23 08 00 10 00 1A 04 08 01 10 00", "23 field 2 as a varint");
    UnchangedHex(h, "23 08 00 18 00 1A 04 08 01 10 00", "23 field 3 as a varint");
    UnchangedHex(h, "23 08 00 1A 04 0A 00 10 00 1A 04 08 01 10 00", "23 location length-delimited");
    UnchangedHex(h, "23 08 00 1A 04 08 00 12 00 1A 04 08 01 10 00", "23 value length-delimited");
    UnchangedHex(h, "23 08 00 1A 05 08 00 10 00 1A 04 08 01 10 00", "23 entry one byte longer than its fields");
    UnchangedHex(h, "23 08 00 12 07 0A 05 08 00 10 00 1A 04 08 01 10 00", "23 wrapped entry one byte longer than its fields");
    UnchangedHex(h, "23 08 00 12 09 0A 04 08 00 10 00 1A 04 08 01 10 00", "23 wrapper ending inside a field");
    UnchangedHex(h, "23 08 00 1A 04 08 01 10 00 12 0F 0A 04 08 00 10 00", "23 wrapper longer than the message");
    UnchangedHex(h, "23 1A 04 08 00 10 00 1A 04 08 01 10 00", "23 without the map");
    /* An entry without a value or a location is no entry, as both decoders
       skip it (SwiftControl zwift_ride.dart:219, qdomyos-zwift
       abstractZapDevice.h:485-487): its lever rests unless a whole entry in
       the same message moves it, and a value without a location moves no
       lever. The map applies with the entries that are whole. A field 2
       whose first field is a varint is one bare entry. */
    {
        static const struct
        {
            const char *hex;
            int leftx;
        } partial[] = {
            /* Location 0 without a value, bare in field 2 and as field 3 */
            { "23 08 00 12 02 08 00 1A 04 08 01 10 00", 0 },
            { "23 08 00 1A 02 08 00 1A 04 08 01 10 00", 0 },
            /* A value of 20 without a location */
            { "23 08 00 1A 02 10 28 1A 04 08 01 10 00", 0 },
            /* Location 0 without a value after a whole entry of 10 for it */
            { "23 08 00 1A 04 08 00 10 14 12 02 08 00 1A 04 08 01 10 00", 10 },
            { "23 08 00 1A 04 08 00 10 14 1A 02 08 00 1A 04 08 01 10 00", 10 },
        };
        const uint32_t every = DPAD | FACES | BTN(LEFT_SHOULDER) | BTN(RIGHT_SHOULDER) | BTN(BACK) | BTN(START);

        for (i = 0; i < COUNT(partial); ++i) {
            AsyncHex(h, "23 08 FE FF FF FF 0F 1A 04 08 00 10 1E 1A 04 08 01 10 28");
            Expect(h, BTN(DPAD_LEFT), AxisOf(15), AxisOf(20), 0, 0, partial[i].hex);
            AsyncHex(h, partial[i].hex);
            Expect(h, every, AxisOf(partial[i].leftx), 0, 0, 0, partial[i].hex);
        }
    }
    /* Unknown field 5 in the message, field 3 in an entry, field 2 in the
       wrapper. The wrapped entry for location 1 comes last and wins. */
    AsyncHex(h, "23 08 FF FF FF FF 0F 28 01 1A 06 08 00 10 04 18 05 1A 04 08 01 10 03 12 08 0A 04 08 01 10 06 10 01");
    Expect(h, 0, AxisOf(2), AxisOf(3), 0, 0, "23 with unknown fields in the message, the entries and the wrapper");
    BH_Destroy(h);
}

static void TestSide(void)
{
    uint8_t message[64];
    size_t length;
    BH_Harness *h;

    printf("Side check of field 1\n");
    h = Plain(LEFT);
    length = Play(message, 1, 1u << 2, 0, 0);
    Async(h, message, length);
    Expect(h, BTN(DPAD_UP), 0, 0, -32768, 0, "the left half accepts field 1 = 1");
    length = Play(message, 0, 1u << 3, 0, 0);
    Unchanged(h, SDL_ZWIFT_ASYNC, message, length, "the left half rejects field 1 = 0");
    length = Play(message, 2, 1u << 3, 0, 0);
    Unchanged(h, SDL_ZWIFT_ASYNC, message, length, "the left half rejects field 1 = 2");
    length = Play(message, 0x101, 1u << 3, 0, 0);
    Unchanged(h, SDL_ZWIFT_ASYNC, message, length, "the left half rejects field 1 = 257");
    BH_Destroy(h);

    h = Plain(RIGHT);
    length = Play(message, 0, 1u << 2, 0, 0);
    Async(h, message, length);
    Expect(h, BTN(NORTH), 0, 0, 0, -32768, "the right half accepts field 1 = 0");
    length = Play(message, 1, 1u << 3, 0, 0);
    Unchanged(h, SDL_ZWIFT_ASYNC, message, length, "the right half rejects field 1 = 1");
    length = Play(message, 2, 1u << 3, 0, 0);
    Unchanged(h, SDL_ZWIFT_ASYNC, message, length, "the right half rejects field 1 = 2");
    BH_Destroy(h);
}

static void TestOtherModels(void)
{
    uint8_t play_left[64], play_right[64], click[16], ride[64];
    size_t play_left_length, play_right_length, click_length, ride_length, i;

    printf("Messages of another controller type\n");
    play_left_length = Play(play_left, 1, 1u << 2, 50, 50);
    play_right_length = Play(play_right, 0, 1u << 2, 50, 50);
    click_length = BH_Hex("37 08 00 10 00", click, sizeof(click));
    ride_length = RideLevers(ride, 0, 50, 50, false);
    for (i = 0; i < COUNT(variants); ++i) {
        BH_Harness *h = Plain(variants[i]);
        char what[64];

        (void)snprintf(what, sizeof(what), "type %02X", variants[i]);
        if (variants[i] != LEFT) {
            Unchanged(h, SDL_ZWIFT_ASYNC, play_left, play_left_length, what);
        }
        if (variants[i] != RIGHT) {
            Unchanged(h, SDL_ZWIFT_ASYNC, play_right, play_right_length, what);
        }
        if (variants[i] != CLICK) {
            Unchanged(h, SDL_ZWIFT_ASYNC, click, click_length, what);
        }
        if (variants[i] != FW2) {
            Unchanged(h, SDL_ZWIFT_ASYNC, ride, ride_length, what);
        }
        BH_Destroy(h);
    }

    /* Nor does another controller type's message publish */
    for (i = 0; i < COUNT(variants); ++i) {
        BH_Harness *h = Handshaken(variants[i], NULL);

        if (variants[i] != LEFT && variants[i] != RIGHT) {
            Async(h, play_left, play_left_length);
            Async(h, play_right, play_right_length);
        }
        if (variants[i] != CLICK) {
            Async(h, click, click_length);
        }
        if (variants[i] != FW2) {
            Async(h, ride, ride_length);
        }
        BH_CHECK(!h->published, "type %02X: another controller type's messages publish nothing", variants[i]);
        BH_Destroy(h);
    }
}

/* Each bit of the button map alone, a clear bit pressed (SwiftControl
   zwift_ride.dart:271-290, qdomyos-zwift abstractZapDevice.h:391-406) */
static void TestButtonMap(void)
{
    uint32_t expected[32];
    uint8_t message[64];
    size_t length;
    uint32_t all = 0;
    char what[64];
    int bit;
    BH_Harness *h = Plain(FW2);

    printf("Button map masks\n");
    memset(expected, 0, sizeof(expected));
    expected[0] = BTN(DPAD_LEFT);       /* LEFT 1 */
    expected[1] = BTN(DPAD_UP);         /* UP 2 */
    expected[2] = BTN(DPAD_RIGHT);      /* RIGHT 4 */
    expected[3] = BTN(DPAD_DOWN);       /* DOWN 8 */
    expected[4] = BTN(EAST);            /* A 10 */
    expected[5] = BTN(SOUTH);           /* B 20 */
    expected[6] = BTN(NORTH);           /* Y 40 */
    expected[7] = BTN(WEST);            /* Z 80 */
    expected[8] = BTN(LEFT_SHOULDER);   /* SHIFT_UP_L 100 */
    expected[11] = BTN(BACK);           /* ONOFF_L 800 */
    expected[12] = BTN(RIGHT_SHOULDER); /* SHIFT_UP_R 1000 */
    expected[15] = BTN(START);          /* ONOFF_R 8000 */
    for (bit = 0; bit < 32; ++bit) {
        length = RideLevers(message, ~(1u << bit), 0, 0, (bit & 1) != 0);
        Async(h, message, length);
        (void)snprintf(what, sizeof(what), "bit %d clear", bit);
        Expect(h, expected[bit], 0, 0, 0, 0, what);
        all |= expected[bit];
    }
    length = RideLevers(message, 0, 0, 0, false);
    Async(h, message, length);
    Expect(h, all, 0, 0, 0, 0, "every bit clear");
    BH_CHECK(all == (DPAD | FACES | BTN(LEFT_SHOULDER) | BTN(RIGHT_SHOULDER) | BTN(BACK) | BTN(START)),
             "the map covers every button of the Play");

    /* A map sent as a 64-bit varint keeps its low 32 bits */
    AsyncHex(h, "23 08 FE FF FF FF FF FF FF FF FF 01 1A 04 08 00 10 00 1A 04 08 01 10 00");
    Expect(h, BTN(DPAD_LEFT), 0, 0, 0, 0, "a 64-bit map");
    BH_Destroy(h);
}

static void TestLevers(void)
{
    uint8_t message[128];
    size_t length;
    char what[64];
    int v;
    BH_Harness *h = Plain(FW2);

    printf("Lever entries\n");
    for (v = -100; v <= 100; ++v) {
        length = RideLevers(message, 0xFFFFFFFF, v, -v, (v & 1) != 0);
        Async(h, message, length);
        (void)snprintf(what, sizeof(what), "levers %d and %d", v, -v);
        Expect(h, 0, AxisOf(v), AxisOf(-v), 0, 0, what);
    }
    length = RideLevers(message, 0xFFFFFFFF, 150, -150, false);
    Async(h, message, length);
    Expect(h, 0, 32767, -32767, 0, 0, "levers past 100 clamp");

    /* Locations 2 and 3 are not levers of the Play */
    {
        const uint64_t locations[4] = { 0, 1, 2, 3 };
        const int32_t values[4] = { 10, 20, 100, -100 };

        length = Ride(message, 0xFFFFFFFF, 4, locations, values, false);
        Async(h, message, length);
        Expect(h, 0, AxisOf(10), AxisOf(20), 0, 0, "locations 2 and 3 are ignored");
        length = Ride(message, 0xFFFFFFFF, 4, locations, values, true);
        Async(h, message, length);
        Expect(h, 0, AxisOf(10), AxisOf(20), 0, 0, "locations 2 and 3 in the wrapper are ignored");
    }
    /* The levers present move, a lever without an entry rests, and the map
       applies with them. Locations past 3 are not levers. */
    {
        const uint64_t locations[3] = { 2, 3, 1 };
        const int32_t values[3] = { -100, -50, 100 };
        const uint32_t every = DPAD | FACES | BTN(LEFT_SHOULDER) | BTN(RIGHT_SHOULDER) | BTN(BACK) | BTN(START);

        Expect(h, 0, AxisOf(10), AxisOf(20), 0, 0, "both levers moved before the message without lever 0");
        length = Ride(message, 0, 3, locations, values, false);
        Async(h, message, length);
        Expect(h, every, 0, 32767, 0, 0, "locations 1, 2 and 3 without 0");
    }
    {
        const uint64_t locations[3] = { 0, 4, 99 };
        const int32_t values[3] = { 40, -100, -100 };

        length = Ride(message, 0xFFFFFFFF, 3, locations, values, false);
        Async(h, message, length);
        Expect(h, 0, AxisOf(40), 0, 0, 0, "location 0 with 4 and 99 but without 1");
    }
    {
        const uint64_t locations[4] = { 0, 1, 0, 1 };
        const int32_t values[4] = { 10, 20, 30, 40 };

        length = Ride(message, 0xFFFFFFFF, 4, locations, values, false);
        Async(h, message, length);
        Expect(h, 0, AxisOf(30), AxisOf(40), 0, 0, "a location twice: the last wins");
    }

    /* Both forms in one message */
    AsyncHex(h, "23 08 FF FF FF FF 0F 12 06 0A 04 08 00 10 14 1A 04 08 01 10 28");
    Expect(h, 0, AxisOf(10), AxisOf(20), 0, 0, "lever 0 wrapped and lever 1 as field 3");

    /* Field 2 as one bare entry, the form qdomyos-zwift reads
       (abstractZapDevice.h:446-495): its first field is a varint */
    AsyncHex(h, "23 08 FF FF FF FF 0F 12 05 08 00 10 C8 01 12 04 08 01 10 00");
    Expect(h, 0, 32767, 0, 0, 0, "both levers as bare field 2 entries");
    AsyncHex(h, "23 08 FF FF FF FF 0F 12 04 08 00 10 14 1A 04 08 01 10 28");
    Expect(h, 0, AxisOf(10), AxisOf(20), 0, 0, "lever 0 bare in field 2 and lever 1 as field 3");
    AsyncHex(h, "23 08 FF FF FF FF 0F 12 04 10 28 08 01 1A 04 08 00 10 14");
    Expect(h, 0, AxisOf(10), AxisOf(20), 0, 0, "a bare entry with its value first");
    UnchangedHex(h, "23 08 FF FF FF FF 0F 12 04 08 00 10 80 1A 04 08 01 10 00", "a bare entry cut off inside a varint");
    BH_Destroy(h);
}

/* The lever and the pull of the 07 message at every value */
static void TestPlayAxes(void)
{
    uint8_t message[64];
    size_t length;
    char what[64];
    int v, mismatches = 0;
    BH_Harness *hl = Plain(LEFT);
    BH_Harness *hr = Plain(RIGHT);

    printf("Lever and pull values\n");
    for (v = -100; v <= 100; ++v) {
        length = Play(message, 1, 0, v, 0);
        Async(hl, message, length);
        (void)snprintf(what, sizeof(what), "left lever %d", v);
        Expect(hl, 0, AxisOf(v), 0, -32768, 0, what);
        length = Play(message, 0, 0, v, 0);
        Async(hr, message, length);
        (void)snprintf(what, sizeof(what), "right lever %d", v);
        Expect(hr, 0, 0, AxisOf(v), 0, -32768, what);
    }
    /* Field 9 is the raw varint shifted right by one, up to 100: Makinolo's
       amount of 0 to 200 whose low bit means nothing */
    for (v = 0; v <= 260; ++v) {
        const int pull = ((v >> 1) > 100) ? 100 : (v >> 1);

        length = Play(message, 1, 0, 0, (uint64_t)v);
        Async(hl, message, length);
        (void)snprintf(what, sizeof(what), "left field 9 raw %d", v);
        Expect(hl, 0, 0, 0, JoystickTrigger(pull), 0, what);
        length = Play(message, 0, 0, 0, (uint64_t)v);
        Async(hr, message, length);
        (void)snprintf(what, sizeof(what), "right field 9 raw %d", v);
        Expect(hr, 0, 0, 0, 0, JoystickTrigger(pull), what);
    }
    length = Play(message, 0, 0, 1000, 1000);
    Async(hr, message, length);
    Expect(hr, 0, 0, 32767, 0, 32767, "lever and pull past 100 clamp");
    /* The shift keeps every bit of a 64-bit varint before the clamp */
    length = Play(message, 0, 0, 0, 0);
    Async(hr, message, length);
    length = Play(message, 0, 0, 0, 0x100000000ull);
    Async(hr, message, length);
    Expect(hr, 0, 0, 0, 0, 32767, "field 9 raw 2^32 is a full pull");
    length = Play(message, 0, 0, 0, 0xFFFFFFFFFFFFFFFFull);
    Async(hr, message, length);
    Expect(hr, 0, 0, 0, 0, 32767, "field 9 raw 2^64 - 1 is a full pull");

    /* Odd raw values lose their low bit, where zigzag would read them as
       negative (SwiftControl zwift.pb.dart:76) */
    AsyncHex(hr, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 01");
    Expect(hr, 0, 0, 0, 0, -32768, "field 9 raw 1 is 0");
    AsyncHex(hr, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 03");
    Expect(hr, 0, 0, 0, 0, JoystickTrigger(1), "field 9 raw 3 is 1");
    AsyncHex(hr, "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 64");
    Expect(hr, 0, 0, 0, 0, JoystickTrigger(50), "field 9 raw 100 is 50");

    /* The joystick value the module writes, through SDL's trigger binding,
       against the part's gamepad value v x 32767 / 100 */
    for (v = 0; v <= 100; ++v) {
        const int joystick = JoystickTrigger(v);
        const int gamepad = GamepadTrigger(joystick);
        const int want = v * 32767 / 100;

        length = Play(message, 0, 0, 0, 2 * (uint64_t)v);
        Async(hr, message, length);
        BH_CHECK(BH_Axis(hr, SDL_BLE_AXIS_RIGHT_TRIGGER) == joystick, "pull %d: the joystick value", v);
        if (gamepad != want) {
            ++mismatches;
            printf("  trigger %d: SDL's binding gives %d, v x 32767 / 100 is %d\n", v, gamepad, want);
        }
        BH_CHECK(gamepad == want, "pull %d: SDL gives %d, the part %d", v, gamepad, want);
    }
    BH_CHECK(mismatches == 0, "%d of 101 pulls miss v x 32767 / 100 through SDL's binding", mismatches);
    BH_CHECK(GamepadTrigger(JoystickTrigger(0)) == 0 && GamepadTrigger(JoystickTrigger(100)) == 32767,
             "the trigger ends are exact");
    BH_Destroy(hl);
    BH_Destroy(hr);
}

static void TestZigZag(void)
{
    printf("Zigzag\n");
    BH_CHECK(SDL_Zwift_ZigZag(0) == 0 && SDL_Zwift_ZigZag(1) == -1 && SDL_Zwift_ZigZag(2) == 1 &&
                 SDL_Zwift_ZigZag(3) == -2 && SDL_Zwift_ZigZag(4) == 2,
             "small values");
    BH_CHECK(SDL_Zwift_ZigZag(190) == 95 && SDL_Zwift_ZigZag(107) == -54 && SDL_Zwift_ZigZag(199) == -100 &&
                 SDL_Zwift_ZigZag(200) == 100,
             "Makinolo's values");
    BH_CHECK(SDL_Zwift_ZigZag(0xFFFFFFFEu) == 2147483647 && SDL_Zwift_ZigZag(0xFFFFFFFFu) == (-2147483647 - 1),
             "the ends of sint32");
    BH_CHECK(SDL_Zwift_ZigZag(0x100000002ull) == 1, "a value past 32 bits keeps its low 32 bits");
    BH_CHECK(ZigZagEncode(95) == 190 && ZigZagEncode(-54) == 107 && ZigZagEncode(-100) == 199, "the test's encoder");
}

static void TestBattery(void)
{
    size_t i;

    printf("Battery clamping\n");
    for (i = 0; i < COUNT(variants); ++i) {
        BH_Harness *h = Plain(variants[i]);

        AsyncHex(h, "19 10 00");
        BH_CHECK(BH_Controls(h)->battery == 0, "type %02X: 0 percent", variants[i]);
        AsyncHex(h, "19 10 64");
        BH_CHECK(BH_Controls(h)->battery == 100, "type %02X: 100 percent", variants[i]);
        AsyncHex(h, "19 10 C8 01");
        BH_CHECK(BH_Controls(h)->battery == 100, "type %02X: 200 clamps to 100", variants[i]);
        AsyncHex(h, "19 10 32");
        BH_CHECK(BH_Controls(h)->battery == 50, "type %02X: 50 percent", variants[i]);
        BH_Destroy(h);
    }
}

/* Ready comes with the first decoded ASYNC message after the RideOn
   write: 07, 37, 23, 15 or 19 */
static void TestReady(void)
{
    static const struct
    {
        uint8_t variant;
        const char *hex;
        bool publishes;
    } cases[] = {
        { RIGHT, "07 08 00 10 01 18 01 20 00 28 01 30 01 38 01 40 00 48 00", true },
        { LEFT, "07 08 01 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 00", true },
        { CLICK, "37 08 01 10 01", true },
        { FW2, "23 08 FF FF FF FF 0F 1A 04 08 00 10 00 1A 04 08 01 10 00", true },
        { RIGHT, "15", true },
        { CLICK, "15", true },
        { RIGHT, "15 00", false },
        { CLICK, "15 00 00 00 A5 5A C3 3C 77", false },
        { FW2, "19 10 50", true },
        { LEFT, "19 10 50", true },
        { RIGHT, "3C 00", false },
        { RIGHT, "FE", false },
        { RIGHT, "07 08", false },
        { RIGHT, "07 08 01 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 00", false },
        { CLICK, "07 08 00 10 01 18 01 20 00 28 01 30 01 38 01 40 00 48 00", false },
        { FW2, "37 08 01 10 01", false },
        { LEFT, "23 08 FF FF FF FF 0F 1A 04 08 00 10 00 1A 04 08 01 10 00", false },
        { RIGHT, "19", false },
        { RIGHT, "52 69 64 65 4F 6E", false },
    };
    size_t i;

    printf("Ready rule\n");
    for (i = 0; i < COUNT(cases); ++i) {
        BH_Harness *h = Handshaken(cases[i].variant, NULL);

        BH_CHECK(!h->published, "%s: nothing is published before an ASYNC value", cases[i].hex);
        AsyncHex(h, cases[i].hex);
        BH_CHECK(h->published == (cases[i].publishes ? 1 : 0), "type %02X, %s: %s", cases[i].variant, cases[i].hex,
                 cases[i].publishes ? "publishes" : "publishes nothing");
        BH_Destroy(h);
    }

    /* The RideOn write still out: an ASYNC message then counts, and
       disarms the switch to the encrypted handshake */
    {
        BH_Harness *h = Subscribed(RIGHT, NULL);
        uint64_t deadline;

        BH_CHECK(BH_LastKind(h) == SDL_BLE_ACTION_WRITE && h->session.waiting, "the RideOn write is out");
        AsyncHex(h, "15");
        BH_CHECK(((const SDL_BLEBase *)h->state)->ready && h->published && h->session.waiting,
                 "an idle message while the write is out is ready, and the session publishes at once");
        BH_CHECK(BH_WriteAll(h) == 1 && !h->session.waiting, "the RideOn write is answered after the publish");
        BH_CHECK(h->published && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1, "published once, the write answered");
        BH_CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline), "no switch is armed");
        BH_Destroy(h);
    }

    /* A message before the RideOn write decodes but is not ready */
    {
        BH_Harness *h = BH_Create(&SDL_BLEZwiftFamily, CLICK, NULL, false);
        uint8_t properties[SDL_BLE_MAX_CHARS];

        h->now = START_MS;
        Properties(properties);
        BH_Connected(h, true, false);
        BH_Discovered(h, true, properties);
        AsyncHex(h, "37 08 00 10 01");
        BH_CHECK(BH_Button(h, SDL_BLE_BUTTON_RIGHT_SHOULDER) && !((const SDL_BLEBase *)h->state)->ready,
                 "a message during the subscription decodes, not ready");
        BH_SubscribeAll(h);
        BH_CHECK(!h->published && BH_Count(h, SDL_BLE_ACTION_WRITE) == 1, "the start-up writes RideOn, nothing published");
        BH_Destroy(h);
    }
}

/* A plain controller that is only slow: its first message comes after the
   key went out. The key exchange stops, the joystick stays, and nothing
   fails when the key's 10 s run out. Only a message that decodes stops it. */
static void TestSlowPlain(void)
{
    uint8_t device_key[SDL_ZWIFT_KEY_SIZE];
    uint8_t value[128];
    uint64_t deadline;
    FakeCrypto fake;
    BH_Harness *h;

    printf("A slow plain controller\n");
    DeviceKey(device_key);
    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(Zwift(h)->phase == SDL_ZWIFT_KEY_SENT && fake.make_calls == 1, "the key went out at 10 s");

    /* A value that does not decode leaves the exchange running, an
       encrypted frame whose first counter byte is 15 among them: idle is one
       byte alone (Makinolo 2023, "Protocol Messages") */
    h->now = START_MS + SDL_ZWIFT_HANDSHAKE_MS + 1000;
    AsyncHex(h, "01 02 03");
    BH_CHECK(Zwift(h)->phase == SDL_ZWIFT_KEY_SENT && !h->published, "an undecoded value changes nothing");
    AsyncHex(h, "15 00 00 00 A5 5A C3 3C 77");
    AsyncHex(h, "15 00");
    BH_CHECK(Zwift(h)->phase == SDL_ZWIFT_KEY_SENT && !h->published, "a frame that starts with 15 is no idle message");
    BH_CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == START_MS + 2 * SDL_ZWIFT_HANDSHAKE_MS,
             "the key's deadline stays");

    /* A plain idle message stops it */
    h->now = START_MS + SDL_ZWIFT_HANDSHAKE_MS + 2000;
    AsyncHex(h, "15");
    BH_CHECK(h->published && Zwift(h)->phase == SDL_ZWIFT_PLAIN, "a plain message stops the exchange and publishes");
    BH_CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline), "no deadline left");
    BH_Advance(h, START_MS + 3 * SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(!((const SDL_BLEBase *)h->state)->failed && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 0 && h->published,
             "nothing fails when the key's 10 s run out");

    /* A key reply that comes late is not acted on */
    memcpy(value, ride_on, 6);
    value[6] = 0x01;
    value[7] = 0x01;
    memcpy(&value[8], device_key, SDL_ZWIFT_KEY_SIZE);
    SyncTx(h, value, 72);
    BH_CHECK(fake.derive_calls == 0 && Zwift(h)->phase == SDL_ZWIFT_PLAIN, "a late key reply is ignored");
    AsyncHex(h, "07 08 00 10 01 18 01 20 00 28 01 30 01 38 01 40 00 48 00");
    Expect(h, BTN(EAST), 0, 0, 0, -32768, "plain messages keep decoding");
    BH_Destroy(h);

    /* The key write still out when the plain message comes: its completion
       arms nothing */
    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    h->now = START_MS + SDL_ZWIFT_HANDSHAKE_MS;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(BH_LastKind(h) == SDL_BLE_ACTION_WRITE && h->session.waiting, "the key write is out");
    h->now += 500;
    AsyncHex(h, "15");
    BH_Written(h, true);
    BH_CHECK(Zwift(h)->phase == SDL_ZWIFT_PLAIN && !SDL_BLESession_GetDeadline(&h->session, &deadline),
             "the key write's completion arms no deadline");
    BH_Destroy(h);
}

/* The part's test 13 */
static void Test13(void)
{
    static const struct
    {
        uint8_t variant;
        const char *reply;
    } replies[] = {
        { LEFT, "52 69 64 65 4F 6E 01 04" },
        { RIGHT, "52 69 64 65 4F 6E 01 04" },
        { CLICK, "52 69 64 65 4F 6E 01 03" },
        { LEFT, "52 69 64 65 4F 6E" },
        { RIGHT, "52 69 64 65 4F 6E" },
        { CLICK, "52 69 64 65 4F 6E" },
        { FW2, "52 69 64 65 4F 6E" },
    };
    uint8_t device_key[SDL_ZWIFT_KEY_SIZE];
    uint8_t value[256], message[64];
    size_t i, k, length;
    int indices[8];
    uint64_t deadline;
    FakeCrypto fake;
    BH_Harness *h;

    printf("Test 13: handshake\n");
    DeviceKey(device_key);

    /* After both descriptor writes, one RideOn write without response */
    for (i = 0; i < COUNT(variants); ++i) {
        const SDL_BLEAction *a;

        FakeInit(&fake);
        h = Handshaken(variants[i], &fake);
        BH_CHECK(h->nactions == 5, "type %02X: five actions, %d seen", variants[i], h->nactions);
        BH_CHECK(h->actions[0].action.kind == SDL_BLE_ACTION_CONNECT && h->actions[1].action.kind == SDL_BLE_ACTION_DISCOVER,
                 "type %02X: connect, discover", variants[i]);
        a = &h->actions[2].action;
        BH_CHECK(a->kind == SDL_BLE_ACTION_SUBSCRIBE && a->characteristic == SDL_ZWIFT_ASYNC && a->cccd == SDL_BLE_CCCD_NOTIFY,
                 "type %02X: ASYNC with Notify", variants[i]);
        a = &h->actions[3].action;
        BH_CHECK(a->kind == SDL_BLE_ACTION_SUBSCRIBE && a->characteristic == SDL_ZWIFT_SYNC_TX &&
                     a->cccd == SDL_BLE_CCCD_INDICATE,
                 "type %02X: SYNC_TX with Indicate", variants[i]);
        a = &h->actions[4].action;
        BH_CHECK(BH_IsWrite(h, 4, ride_on, sizeof(ride_on)) && a->characteristic == SDL_ZWIFT_SYNC_RX && !a->response,
                 "type %02X: RideOn to SYNC_RX without response", variants[i]);
        BH_CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == START_MS + SDL_ZWIFT_HANDSHAKE_MS,
                 "type %02X: the switch is armed 10 s after the write", variants[i]);

        /* A plain controller streams, and nothing more is ever written */
        AsyncHex(h, "15");
        BH_CHECK(h->published, "type %02X: the first idle message publishes", variants[i]);
        BH_CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline), "type %02X: the switch is off", variants[i]);
        BH_Advance(h, h->now + 600000);
        BH_CHECK(BH_Writes(h, indices, 8) == 1 && fake.make_calls == 0 && fake.decrypt_calls == 0,
                 "type %02X: one write in ten minutes, no crypto", variants[i]);
        BH_Destroy(h);
    }

    /* The plain reply forms */
    for (i = 0; i < COUNT(replies); ++i) {
        size_t key;

        for (key = 0; key < 2; ++key) {
            FakeInit(&fake);
            h = Handshaken(replies[i].variant, &fake);
            length = BH_Hex(replies[i].reply, value, sizeof(value));
            if (key && length == 8) {
                /* SwiftControl logs what follows as the controller's key
                   (zwift_device.dart:171-178) */
                memcpy(&value[length], device_key, SDL_ZWIFT_KEY_SIZE);
                length += SDL_ZWIFT_KEY_SIZE;
            }
            SyncTx(h, value, length);
            BH_CHECK(!((const SDL_BLEBase *)h->state)->failed && fake.derive_calls == 0 && fake.make_calls == 0,
                     "type %02X, %s, %u bytes: accepted, no key exchange", replies[i].variant, replies[i].reply,
                     (unsigned)length);
            BH_CHECK(!h->published, "type %02X, %s: the reply alone publishes nothing", replies[i].variant, replies[i].reply);
            AsyncHex(h, "15");
            BH_CHECK(h->published && Zwift(h)->phase == SDL_ZWIFT_PLAIN,
                     "type %02X, %s: the first ASYNC message publishes in plain mode", replies[i].variant, replies[i].reply);
            BH_Advance(h, h->now + 60000);
            BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == 1 && fake.make_calls == 0, "type %02X, %s: no key write",
                     replies[i].variant, replies[i].reply);
            BH_Destroy(h);
        }
    }

    /* A reply without ASYNC values still switches: only a decoded ASYNC
       message shows the plain handshake worked */
    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    length = BH_Hex("52 69 64 65 4F 6E 01 04", value, sizeof(value));
    SyncTx(h, value, length);
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(fake.make_calls == 1 && BH_Count(h, SDL_BLE_ACTION_WRITE) == 2, "a silent controller that replied still switches");
    BH_Destroy(h);

    /* ASYNC values that decode as no message leave the switch armed: a
       controller of older firmware that holds a session sends encrypted
       frames, which zwiftplay takes to be any value longer than 8 bytes
       (AbstractZapDevice.kt:42-48). Frames whose first counter byte is a
       message type, 15 idle among them, and short values that are no
       message all count as silence. */
    {
        static const char *const undecoded[] = {
            "15 00 00 00 A5 5A C3 3C 77",
            "15 01 00 00 00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF 10 20 A5 5A C3 3C",
            "07 00 00 00 08 00 10 01 A5 5A C3 3C",
            "19 00 00 00 10 59 A5 5A C3 3C",
            "37 00 00 00 08 00 10 01 A5 5A C3 3C",
            "23 00 00 00 08 00 A5 5A C3 3C",
            "15 00",
            "01 02 03",
            "3C 00",
            "FE",
        };
        const uint64_t step = SDL_ZWIFT_HANDSHAKE_MS / (COUNT(undecoded) + 1);

        for (k = 0; k < COUNT(variants); ++k) {
            FakeInit(&fake);
            h = Handshaken(variants[k], &fake);
            for (i = 0; i < COUNT(undecoded); ++i) {
                h->now = START_MS + step * (i + 1);
                AsyncHex(h, undecoded[i]);
            }
            BH_CHECK(!h->published && Zwift(h)->armed && Zwift(h)->phase == SDL_ZWIFT_PLAIN,
                     "type %02X: undecoded values leave the switch armed", variants[k]);
            BH_CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == START_MS + SDL_ZWIFT_HANDSHAKE_MS,
                     "type %02X: the switch stays 10 s after the RideOn write", variants[k]);
            BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
            BH_CHECK(fake.make_calls == 1 && BH_Count(h, SDL_BLE_ACTION_WRITE) == 2 && Zwift(h)->phase == SDL_ZWIFT_KEY_SENT,
                     "type %02X: the key exchange at 10 s", variants[k]);
            BH_Destroy(h);
        }
    }

    /* The switch 10 s after the RideOn write with no decoded ASYNC message */
    FakeInit(&fake);
    h = Handshaken(LEFT, &fake);
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS - 1);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == 1 && fake.make_calls == 0, "nothing before 10 s");
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(BH_Writes(h, indices, 8) == 2 && fake.make_calls == 1, "the key write at 10 s");
    if (BH_Writes(h, indices, 8) == 2) {
        const BH_Logged *logged = &h->actions[indices[1]];

        BH_CHECK(logged->now == START_MS + SDL_ZWIFT_HANDSHAKE_MS, "the key write goes out at %llu",
                 (unsigned long long)logged->now);
        BH_CHECK(logged->action.characteristic == SDL_ZWIFT_SYNC_RX && logged->action.response &&
                     logged->action.length == 72,
                 "72 bytes to SYNC_RX with response");
        BH_CHECK(memcmp(logged->action.data, ride_on, 6) == 0 && logged->action.data[6] == 0x01 &&
                     logged->action.data[7] == 0x02 && memcmp(&logged->action.data[8], fake.public_key, 64) == 0,
                 "RideOn, 01 02, then the host key");
    }
    BH_CHECK(!h->published && Zwift(h)->phase == SDL_ZWIFT_KEY_SENT, "waiting for the device key");
    NoSpentDeadline(h, "after the key write");

    /* A reply too short to hold a key changes nothing */
    SyncTx(h, value, 8);
    BH_CHECK(fake.derive_calls == 0 && Zwift(h)->phase == SDL_ZWIFT_KEY_SENT, "RideOn 01 04 is not the key reply");

    /* The device key reply leads to Derive with the key */
    memcpy(value, ride_on, 6);
    value[6] = 0x01;
    value[7] = 0x01;
    memcpy(&value[8], device_key, SDL_ZWIFT_KEY_SIZE);
    SyncTx(h, value, 72);
    BH_CHECK(fake.derive_calls == 1 && memcmp(fake.device_key, device_key, SDL_ZWIFT_KEY_SIZE) == 0,
             "Derive gets the device key");
    BH_CHECK(Zwift(h)->phase == SDL_ZWIFT_ENCRYPTED && !SDL_BLEZwiftModule.GetDeadline(h->state, &deadline),
             "encrypted, no timer left");
    BH_CHECK(!h->published, "the key reply alone publishes nothing");

    /* Decrypted messages decode, and the first publishes */
    length = Play(message, 1, 1u << 2, -100, 200);
    length = Frame(value, 1, message, length);
    Async(h, value, length);
    BH_CHECK(fake.decrypt_calls == 1 && fake.decrypt_length == length &&
                 memcmp(fake.counter, "\x01\x00\x00\x00", 4) == 0,
             "Decrypt gets the frame as received");
    Expect(h, BTN(DPAD_UP), -32767, 0, 32767, 0, "a decrypted 07 message");
    BH_CHECK(h->published, "the first decrypted message publishes");
    length = BH_Hex("19 10 2A", message, sizeof(message));
    length = Frame(value, 2, message, length);
    Async(h, value, length);
    BH_CHECK(BH_Controls(h)->battery == 42, "a decrypted battery message");

    /* A frame that fails Decrypt is dropped */
    length = Play(message, 1, 1u << 3, 0, 0);
    length = Frame(value, 3, message, length);
    value[length - 1] ^= 0x01;
    Unchanged(h, SDL_ZWIFT_ASYNC, value, length, "a frame with a bad tag");
    BH_CHECK(fake.decrypt_calls == 3, "the bad frame reached Decrypt");

    /* A plain message is no frame */
    length = Play(message, 1, 1u << 3, 0, 0);
    Unchanged(h, SDL_ZWIFT_ASYNC, message, length, "a plain message in encrypted mode");

    /* Values of 8 bytes or less are never decrypted (AbstractZapDevice.kt:42-48) */
    {
        const int calls = fake.decrypt_calls;
        size_t n;

        memset(value, 0x15, sizeof(value));
        for (n = 1; n <= 8; ++n) {
            Unchanged(h, SDL_ZWIFT_ASYNC, value, n, "a short value in encrypted mode");
        }
        BH_CHECK(fake.decrypt_calls == calls, "no value of 8 bytes or less reaches Decrypt");
        length = Frame(value, 9, (const uint8_t *)"\x15", 1);
        BH_CHECK(length == 9, "an idle frame is 9 bytes");
        Async(h, value, length);
        BH_CHECK(fake.decrypt_calls == calls + 1, "9 bytes reach Decrypt");
    }

    /* A second key reply after the exchange is not a second key */
    memcpy(value, ride_on, 6);
    value[6] = 0x01;
    value[7] = 0x01;
    memcpy(&value[8], device_key, SDL_ZWIFT_KEY_SIZE);
    SyncTx(h, value, 72);
    BH_CHECK(fake.derive_calls == 1 && Zwift(h)->phase == SDL_ZWIFT_ENCRYPTED, "no second Derive");
    BH_Destroy(h);

    /* The same with the NULL crypto of a build without CNG: the session backs off */
    h = Handshaken(RIGHT, NULL);
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == 1, "no key write without crypto");
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1 && BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT && !h->published &&
                 SDL_BLESession_Ended(&h->session),
             "without crypto the session backs off");
    BH_Destroy(h);

    /* A key pair that cannot be made ends the same way */
    FakeInit(&fake);
    fake.make_result = false;
    h = Handshaken(CLICK, &fake);
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(fake.make_calls == 1 && BH_Count(h, SDL_BLE_ACTION_WRITE) == 1 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1,
             "a failed MakeKey backs off");
    BH_Destroy(h);

    /* No key reply within 10 s of the key write */
    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    BH_Advance(h, START_MS + 2 * SDL_ZWIFT_HANDSHAKE_MS - 1);
    BH_CHECK(!SDL_BLESession_Ended(&h->session), "still waiting just before 10 s");
    BH_Advance(h, START_MS + 2 * SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1 && SDL_BLESession_Ended(&h->session) && fake.derive_calls == 0,
             "no key reply in 10 s backs off");
    BH_Destroy(h);

    /* A handshake write that fails backs off at once, where waiting out the
       10 s would hold a link that gives nothing: the RideOn write, then the
       key write */
    FakeInit(&fake);
    h = Subscribed(RIGHT, &fake);
    h->now = START_MS + 20;
    BH_Written(h, false);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1 && BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT &&
                 SDL_BLESession_Ended(&h->session) && ((const SDL_BLEBase *)h->state)->failed && !h->published,
             "a failed RideOn write backs off");
    BH_CHECK(h->nactions >= 2 && h->actions[h->nactions - 2].now == START_MS + 20, "at once");
    BH_CHECK(!SDL_BLEZwiftModule.GetDeadline(h->state, &deadline), "and leaves no timer");
    BH_Advance(h, START_MS + 3 * SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(fake.make_calls == 0 && BH_Count(h, SDL_BLE_ACTION_WRITE) == 1, "and no key exchange follows");
    BH_Destroy(h);

    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    h->now = START_MS + SDL_ZWIFT_HANDSHAKE_MS;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(BH_LastKind(h) == SDL_BLE_ACTION_WRITE && h->session.waiting && fake.make_calls == 1, "the key write is out");
    h->now += 30;
    BH_Written(h, false);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1 && SDL_BLESession_Ended(&h->session) && fake.derive_calls == 0,
             "a failed key write backs off");
    BH_CHECK(h->nactions >= 2 && h->actions[h->nactions - 2].now == START_MS + SDL_ZWIFT_HANDSHAKE_MS + 30, "at once");
    BH_CHECK(!SDL_BLEZwiftModule.GetDeadline(h->state, &deadline), "and leaves no timer");
    BH_Destroy(h);

    /* Once a message has decoded the controller answers, and a failed write
       changes nothing */
    FakeInit(&fake);
    h = Subscribed(RIGHT, &fake);
    AsyncHex(h, "15");
    BH_Written(h, false);
    BH_CHECK(h->published && !SDL_BLESession_Ended(&h->session) && !((const SDL_BLEBase *)h->state)->failed,
             "a failed RideOn write after a message keeps the session");
    BH_Advance(h, START_MS + 3 * SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(fake.make_calls == 0 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 0 && BH_Count(h, SDL_BLE_ACTION_WRITE) == 1,
             "and nothing follows it");
    BH_Destroy(h);
    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    h->now = START_MS + SDL_ZWIFT_HANDSHAKE_MS;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    h->now += 500;
    AsyncHex(h, "15");
    BH_Written(h, false);
    BH_CHECK(h->published && Zwift(h)->phase == SDL_ZWIFT_PLAIN && !SDL_BLESession_Ended(&h->session) &&
                 BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 0,
             "a failed key write after a plain message keeps the session");
    BH_Destroy(h);

    /* A key that Derive rejects */
    FakeInit(&fake);
    fake.derive_result = false;
    h = Handshaken(RIGHT, &fake);
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    memcpy(value, ride_on, 6);
    value[6] = 0x00;
    value[7] = 0x09;
    memcpy(&value[8], device_key, SDL_ZWIFT_KEY_SIZE);
    SyncTx(h, value, 72);
    BH_CHECK(fake.derive_calls == 1 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1, "a rejected key backs off");
    BH_Destroy(h);

    /* A longer key reply: the key is the 64 bytes after the header */
    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    memset(value, 0xEE, sizeof(value));
    memcpy(value, ride_on, 6);
    value[6] = 0x01;
    value[7] = 0x03;
    memcpy(&value[8], device_key, SDL_ZWIFT_KEY_SIZE);
    SyncTx(h, value, 80);
    BH_CHECK(fake.derive_calls == 1 && memcmp(fake.device_key, device_key, SDL_ZWIFT_KEY_SIZE) == 0 &&
                 Zwift(h)->phase == SDL_ZWIFT_ENCRYPTED,
             "an 80-byte reply gives the same key");
    BH_Destroy(h);

    /* The key reply comes before the key write's answer */
    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    SDL_BLESession_Tick(&h->session, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    BH_Drain(h);
    BH_CHECK(BH_LastKind(h) == SDL_BLE_ACTION_WRITE && h->session.waiting, "the key write is out");
    memcpy(value, ride_on, 6);
    value[6] = 0x01;
    value[7] = 0x01;
    memcpy(&value[8], device_key, SDL_ZWIFT_KEY_SIZE);
    SyncTx(h, value, 72);
    h->now = START_MS + SDL_ZWIFT_HANDSHAKE_MS + 50;
    BH_WriteAll(h);
    BH_CHECK(Zwift(h)->phase == SDL_ZWIFT_ENCRYPTED && !SDL_BLEZwiftModule.GetDeadline(h->state, &deadline),
             "an early key reply leaves no timer");
    BH_Destroy(h);

    /* The switch runs 10 s from the RideOn write's completion */
    FakeInit(&fake);
    h = Subscribed(RIGHT, &fake);
    BH_CHECK(!SDL_BLEZwiftModule.GetDeadline(h->state, &deadline), "no timer while the RideOn write is out");
    h->now = START_MS + 3000;
    BH_WriteAll(h);
    BH_Advance(h, START_MS + 3000 + SDL_ZWIFT_HANDSHAKE_MS - 1);
    BH_CHECK(fake.make_calls == 0, "no switch before 10 s from the write's completion");
    BH_Advance(h, START_MS + 3000 + SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(fake.make_calls == 1, "the switch 10 s from the write's completion");
    BH_Destroy(h);

    /* An ASYNC value just before the switch keeps the plain handshake */
    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS - 1);
    AsyncHex(h, "15");
    BH_Advance(h, START_MS + 10 * SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(fake.make_calls == 0 && h->published && BH_Count(h, SDL_BLE_ACTION_WRITE) == 1, "a value at 9.999 s keeps plain");
    BH_Destroy(h);

    /* A value that arrived before the RideOn write does not count: the
       controller is still silent after it, so the switch comes at 10 s,
       and no spent timer is left behind */
    FakeInit(&fake);
    h = BH_Create(&SDL_BLEZwiftFamily, RIGHT, &fake.crypto, false);
    {
        uint8_t properties[SDL_BLE_MAX_CHARS];

        h->now = START_MS;
        Properties(properties);
        BH_Connected(h, true, false);
        BH_Discovered(h, true, properties);
        AsyncHex(h, "15");
        BH_SubscribeAll(h);
        BH_WriteAll(h);
    }
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(fake.make_calls == 1 && BH_Count(h, SDL_BLE_ACTION_WRITE) == 2,
             "a value before the RideOn write does not stop the switch");
    NoSpentDeadline(h, "a value before the RideOn write");
    BH_Advance(h, START_MS + 2 * SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1, "and no key reply backs off");
    BH_Destroy(h);

    printf("Test 14: the key exchange is testblezwiftcrypto.c's\n");
}

static void Test15(void)
{
    SDL_BLEAdvertisement ad;
    SDL_BLEHost host;
    SDL_BLEMatch match;
    uint8_t variant;
    int type, family;

    printf("Test 15: discovery\n");
    for (type = 0; type <= 0xFF; ++type) {
        const bool in_scope = (type == 0x02 || type == 0x03 || type == 0x09 || type == 0x0E);

        memset(&ad, 0, sizeof(ad));
        ad.address = zwift_address;
        ad.nmanufacturer = 1;
        ad.manufacturer[0].company = 0x094A;
        ad.manufacturer[0].length = 3;
        ad.manufacturer[0].data[0] = (uint8_t)type;
        ad.manufacturer[0].data[1] = 0xA5;
        ad.manufacturer[0].data[2] = 0x96;
        variant = 0xAA;
        family = SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, &variant);
        if (in_scope) {
            BH_CHECK(family == SDL_BLE_FAMILY_ZWIFT && variant == type, "type %02X matches with its variant", type);
        } else {
            BH_CHECK(family == -1, "type %02X matches nothing", type);
        }
        if (type == 0x07 || type == 0x08 || type == 0x0A || type == 0x0B) {
            SDL_BLEHost_Init(&host);
            BH_CHECK(!SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 0, &match),
                     "type %02X starts no connect", type);
        }
    }

    /* The type byte must follow Zwift's company ID */
    memset(&ad, 0, sizeof(ad));
    ad.nmanufacturer = 2;
    ad.manufacturer[0].company = 0x094B;
    ad.manufacturer[0].length = 1;
    ad.manufacturer[0].data[0] = 0x02;
    ad.manufacturer[1].company = 0x094A;
    ad.manufacturer[1].length = 0;
    BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == -1,
             "another company, and Zwift's without a type byte, match nothing");
    ad.manufacturer[1].length = 1;
    ad.manufacturer[1].data[0] = 0x03;
    variant = 0;
    BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, &variant) == SDL_BLE_FAMILY_ZWIFT &&
                 variant == 0x03,
             "Zwift's record second in the list matches");

    /* The services only corroborate, and the DFU service is nothing */
    memset(&ad, 0, sizeof(ad));
    ad.nservices = 1;
    ad.services[0] = SDL_BLEZwiftFamily.services[0];
    BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == -1, "the 128-bit service alone");
    ad.services[0] = SDL_BLE_UUID16(0xFC82);
    BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == -1, "FC82 alone");
    ad.services[0] = SDL_BLE_UUID16(0xFE59);
    BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == -1, "FE59 alone matches nothing");
    ad.has_name = true;
    memcpy(ad.name, "Zwift Play", 10);
    ad.nservices = 0;
    BH_CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, NULL) == -1, "the name alone");

    /* The family's service is the 128-bit one, with FC82 tried first */
    {
        const SDL_BLEUUID service = SDL_BLE_UUID16(0xFC82);
        const uint8_t zwift[16] = { 0x00, 0x00, 0x00, 0x01, 0x19, 0xca, 0x46, 0x51,
                                    0x86, 0xe5, 0xfa, 0x29, 0xdc, 0xdd, 0x09, 0xd1 };

        BH_CHECK(SDL_BLEZwiftFamily.nservices == 1 && memcmp(SDL_BLEZwiftFamily.services[0].bytes, zwift, 16) == 0,
                 "service 00000001-19ca-4651-86e5-fa29dcdd09d1");
        BH_CHECK(SDL_BLEZwiftFamily.has_alternate && SDL_BLE_UUIDEqual(&SDL_BLEZwiftFamily.alternate, &service),
                 "FC82 is the alternate");
    }
}

/* Starts a new session on the same state memory, as the driver reuses it */
static void Reconnect(BH_Harness *h, uint8_t variant, const FakeCrypto *fake)
{
    SDL_BLEModuleContext context;
    SDL_BLESink sink;
    uint8_t properties[SDL_BLE_MAX_CHARS];

    memset(&context, 0, sizeof(context));
    context.variant = variant;
    context.crypto = fake ? &fake->crypto : NULL;
    sink.userdata = h;
    sink.changed = BH_Changed;
    sink.sensor = BH_Sensor;
    sink.log = BH_Log;
    SDL_BLESession_Init(&h->session, &SDL_BLEZwiftFamily, h->state, &sink, &context, false);
    BH_Drain(h);
    Properties(properties);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
}

static void Test16(void)
{
    SDL_BLEAdvertisement ad;
    SDL_BLEHost host;
    SDL_BLEMatch match;
    uint8_t message[64], value[128];
    uint8_t device_key[SDL_ZWIFT_KEY_SIZE];
    size_t length;
    int before, writes;
    FakeCrypto fake;
    BH_Harness *h;

    printf("Test 16: link loss and reconnect\n");
    memset(&ad, 0, sizeof(ad));
    ad.address = zwift_address;
    ad.nmanufacturer = 1;
    ad.manufacturer[0].company = 0x094A;
    ad.manufacturer[0].length = 3;
    ad.manufacturer[0].data[0] = LEFT;
    SDL_BLEHost_Init(&host);
    BH_CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 0, &match) &&
                 match.family == SDL_BLE_FAMILY_ZWIFT && match.variant == LEFT,
             "the left half connects");

    h = Plain(LEFT);
    length = Play(message, 1, 0xFC, -80, 90);
    Async(h, message, length);
    AsyncHex(h, "19 10 3C");
    BH_CHECK(BH_Controls(h)->buttons != 0 && BH_Axis(h, SDL_BLE_AXIS_LEFTX) != 0 &&
                 BH_Axis(h, SDL_BLE_AXIS_LEFT_TRIGGER) != -32768,
             "the left half is held");
    before = h->nactions;
    SDL_BLESession_Lost(&h->session, h->now);
    BH_Drain(h);
    Expect(h, 0, 0, 0, -32768, 0, "link loss");
    BH_CHECK(BH_Controls(h)->battery == 60, "link loss keeps the battery");
    BH_CHECK(h->nactions == before + 2 && h->actions[before].action.kind == SDL_BLE_ACTION_REMOVE &&
                 h->actions[before + 1].action.kind == SDL_BLE_ACTION_DISCONNECT,
             "remove and disconnect, no backoff");
    {
        SDL_BLEOutcome outcome;

        SDL_BLESession_GetOutcome(&h->session, &outcome);
        SDL_BLEHost_SessionEnded(&host, zwift_address, &outcome, h->now);
    }
    BH_CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, h->now, &match),
             "the next advertisement reconnects");

    before = h->nactions;
    Reconnect(h, match.variant, NULL);
    BH_CHECK(BH_Writes(h, NULL, 0) == 2 && BH_IsWrite(h, h->nactions - 1, ride_on, sizeof(ride_on)),
             "the reconnect writes RideOn again");
    BH_CHECK(!h->published && Zwift(h)->phase == SDL_ZWIFT_PLAIN && Zwift(h)->armed && Zwift(h)->timing,
             "the new session starts over, its switch armed from the RideOn write");
    BH_WriteAll(h);
    AsyncHex(h, "15");
    BH_CHECK(h->published, "and publishes on its first message");
    BH_Destroy(h);

    /* An encrypted session that drops starts its next one plain */
    FakeInit(&fake);
    DeviceKey(device_key);
    h = Handshaken(RIGHT, &fake);
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    memcpy(value, ride_on, 6);
    value[6] = 0x01;
    value[7] = 0x01;
    memcpy(&value[8], device_key, SDL_ZWIFT_KEY_SIZE);
    SyncTx(h, value, 72);
    length = Play(message, 0, 1u << 5, 30, 40);
    length = Frame(value, 7, message, length);
    Async(h, value, length);
    BH_CHECK(h->published && BH_Button(h, SDL_BLE_BUTTON_SOUTH), "the encrypted right half is held");
    SDL_BLESession_Lost(&h->session, h->now);
    BH_Drain(h);
    Expect(h, 0, 0, 0, 0, -32768, "encrypted link loss");
    writes = BH_Count(h, SDL_BLE_ACTION_WRITE);
    Reconnect(h, RIGHT, &fake);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == writes + 1 && BH_IsWrite(h, h->nactions - 1, ride_on, sizeof(ride_on)) &&
                 Zwift(h)->phase == SDL_ZWIFT_PLAIN,
             "the next session repeats the handshake from the plain RideOn");
    BH_WriteAll(h);
    BH_Advance(h, h->now + SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(fake.make_calls == 2, "and repeats the key exchange when the controller stays silent");
    BH_Destroy(h);
}

/* A variant the matcher never passes cannot start */
static void TestUnknownVariant(void)
{
    BH_Harness *h = Subscribed(0x07, NULL);

    printf("Unknown variant\n");
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == 0 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1 &&
                 SDL_BLESession_Ended(&h->session),
             "a Ride's type byte fails at the start-up");
    BH_Destroy(h);
}

/* A value on SYNC_RX, or an index the family lacks, is not input */
static void TestCharacteristics(void)
{
    BH_Harness *h = Plain(RIGHT);
    uint8_t message[64];
    size_t length = Play(message, 0, 1u << 2, 0, 0);

    printf("Characteristics\n");
    Unchanged(h, SDL_ZWIFT_SYNC_RX, message, length, "a message on SYNC_RX");
    Unchanged(h, SDL_ZWIFT_SYNC_TX, message, length, "a message on SYNC_TX");
    Unchanged(h, 3, message, length, "a message on index 3");
    SDL_BLEZwiftModule.Value(h->state, 5, message, length, h->now * 1000000);
    BH_CHECK(!BH_Button(h, SDL_BLE_BUTTON_NORTH), "the module ignores an index past its characteristics");
    Async(h, message, length);
    BH_CHECK(BH_Button(h, SDL_BLE_BUTTON_NORTH), "the same message on ASYNC decodes");
    BH_Destroy(h);
}

/* Close sends nothing and ends the session */
static void TestClose(void)
{
    BH_Harness *h = Plain(CLICK);
    uint64_t deadline;
    int writes = BH_Count(h, SDL_BLE_ACTION_WRITE);

    printf("Close\n");
    AsyncHex(h, "37 08 00 10 01");
    SDL_BLESession_Close(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(BH_Count(h, SDL_BLE_ACTION_WRITE) == writes && SDL_BLESession_Ended(&h->session) &&
                 !SDL_BLESession_GetDeadline(&h->session, &deadline),
             "no closing write, the session ends");
    BH_CHECK(BH_Controls(h)->buttons == 0, "close releases the Click");
    BH_Destroy(h);

    /* Closing while the switch is armed leaves no timer */
    h = Handshaken(RIGHT, NULL);
    SDL_BLESession_Close(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline) && SDL_BLESession_Ended(&h->session),
             "close disarms the switch");
    BH_Destroy(h);
}

/* A Decrypt that leaves the ciphertext in plain when the tag does not match */
static bool Leak_Decrypt(void *userdata, const uint8_t *frame, size_t length, uint8_t *plain)
{
    FakeCrypto *fake = (FakeCrypto *)userdata;

    ++fake->decrypt_calls;
    if (length <= SDL_ZWIFT_FRAME_EXTRA) {
        return false;
    }
    memcpy(plain, &frame[4], length - SDL_ZWIFT_FRAME_EXTRA);
    return memcmp(&frame[length - 4], fake_tag, sizeof(fake_tag)) == 0;
}

/* A right half after the silent 10 s: the key write out and answered */
static BH_Harness *KeySent(FakeCrypto *fake)
{
    BH_Harness *h;

    FakeInit(fake);
    h = Handshaken(RIGHT, fake);
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    BH_WriteAll(h);
    BH_CHECK(fake->make_calls == 1 && Zwift(h)->phase == SDL_ZWIFT_KEY_SENT, "the key write is out");
    return h;
}

/* The reader at its edges: field 0, fixed64 and fixed32 a byte short, a
   Click's cut tail */
static void TestReaderEdges(void)
{
    BH_Harness *h = Plain(RIGHT);

    printf("Reader edges\n");
    UnchangedHex(h, "19 00 00 10 59", "field 0 alone");
    UnchangedHex(h, "19 10 59 21 01 02 03 04 05 06 07", "a fixed64 a byte short");
    UnchangedHex(h, "19 10 59 25 01 02 03", "a fixed32 a byte short");
    BH_Destroy(h);
    h = Plain(CLICK);
    UnchangedHex(h, "37 08 00 10 01 18", "a Click that ends in a cut tag");
    BH_Destroy(h);
}

/* 23 messages that are malformed after the map and both levers change
   nothing, the map included. A map alone applies (test 10), so the map
   here presses LEFT, where a message taken in part would show. */
static void TestRideMalformed(void)
{
    BH_Harness *h = Plain(FW2);

    printf("Malformed 23 messages\n");
    UnchangedHex(h, "23 08 FE FF 03 1A 05 08 00 10 14 18 1A 04 08 01 10 14", "a lever entry that ends in a cut tag");
    UnchangedHex(h, "23 08 FE FF 03 1A 04 08 00 10 14 1A 04 08 01 10 14 10 05", "field 2 as a varint");
    UnchangedHex(h, "23 08 FE FF 03 12 0E 0A 04 08 00 10 14 0A 04 08 01 10 14 08 05", "a varint entry in the wrapper");
    UnchangedHex(h, "23 08 FE FF 03 12 0D 0A 04 08 00 10 14 0A 04 08 01 10 14 18", "a wrapper that ends in a cut tag");
    UnchangedHex(h, "23 08 FE FF 03 1A 04 08 00 10 14 1A 04 08 01 10 14 18 05", "field 3 as a varint");
    UnchangedHex(h, "23 08 FE FF 03 1A 04 08 00 10 14 1A 04 08 01 10 14 18", "a message that ends in a cut tag");
    /* Positive control: the same message without the tail decodes */
    AsyncHex(h, "23 08 FE FF 03 1A 04 08 00 10 14 1A 04 08 01 10 14");
    Expect(h, BTN(DPAD_LEFT), AxisOf(10), AxisOf(10), 0, 0, "the well-formed 23 message decodes");
    BH_Destroy(h);
}

/* SYNC_TX replies that are not the key reply, and frames at the size limit */
static void TestKeyReplies(void)
{
    uint8_t reply[8 + SDL_ZWIFT_KEY_SIZE];
    uint8_t message[272], frame[288];
    FakeCrypto fake;
    BH_Harness *h = KeySent(&fake);
    size_t n;
    int calls;

    printf("Key replies and frame sizes\n");
    memcpy(reply, ride_on, sizeof(ride_on));
    reply[6] = 0x01;
    reply[7] = 0x01;
    DeviceKey(&reply[8]);

    /* RideOx: not RideOn */
    reply[5] = 'x';
    SyncTx(h, reply, sizeof(reply));
    BH_CHECK(fake.derive_calls == 0 && Zwift(h)->phase == SDL_ZWIFT_KEY_SENT, "a reply that is not RideOn");
    reply[5] = 'n';
    /* RideOn cut to 2 to 5 bytes */
    for (n = 2; n < sizeof(ride_on); ++n) {
        SyncTx(h, ride_on, n);
    }
    BH_CHECK(fake.derive_calls == 0 && Zwift(h)->phase == SDL_ZWIFT_KEY_SENT, "RideOn cut short");
    /* A key reply a byte short */
    SyncTx(h, reply, sizeof(reply) - 1);
    BH_CHECK(fake.derive_calls == 0 && Zwift(h)->phase == SDL_ZWIFT_KEY_SENT, "a key reply of 71 bytes");
    /* Positive control: the whole reply */
    SyncTx(h, reply, sizeof(reply));
    BH_CHECK(fake.derive_calls == 1 && Zwift(h)->phase == SDL_ZWIFT_ENCRYPTED, "the key reply of 72 bytes");

    /* A 256-byte message, 19 1A FA 01, 250 zero bytes, 10 3D, in a 264-byte frame */
    memset(message, 0, sizeof(message));
    message[0] = 0x19;
    message[1] = 0x1A;
    message[2] = 0xFA;
    message[3] = 0x01;
    message[254] = 0x10;
    message[255] = 0x3D;
    n = Frame(frame, 5, message, 256);
    calls = fake.decrypt_calls;
    Async(h, frame, n);
    BH_CHECK(n == 264 && fake.decrypt_calls == calls + 1 && BH_Controls(h)->battery == 61, "a 264-byte frame decodes");
    /* A 257-byte message, 19 1A FB 01, 251 zero bytes, 10 3E, in a 265-byte frame */
    memset(message, 0, sizeof(message));
    message[0] = 0x19;
    message[1] = 0x1A;
    message[2] = 0xFB;
    message[3] = 0x01;
    message[255] = 0x10;
    message[256] = 0x3E;
    n = Frame(frame, 6, message, 257);
    calls = fake.decrypt_calls;
    Async(h, frame, n);
    BH_CHECK(n == 265 && fake.decrypt_calls == calls && BH_Controls(h)->battery == 61, "a 265-byte frame is dropped");

    /* A frame whose tag fails is dropped, whatever Decrypt left in plain */
    fake.crypto.Decrypt = Leak_Decrypt;
    n = BH_Hex("19 10 2A", message, sizeof(message));
    n = Frame(frame, 7, message, n);
    frame[n - 1] ^= 0x01;
    Unchanged(h, SDL_ZWIFT_ASYNC, frame, n, "a frame with a bad tag and its bytes left in plain");
    BH_Destroy(h);
}

/* The handshake's timers at their edges */
static void TestTimerEdges(void)
{
    static const uint8_t idle = 0x15;
    uint64_t deadline;
    FakeCrypto fake;
    BH_Harness *h;

    printf("Handshake timers at their edges\n");
    /* An empty value is no message, so it does not answer RideOn */
    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    BH_Value(h, SDL_ZWIFT_ASYNC, NULL, 0, false);
    SDL_BLEZwiftModule.Value(h->state, SDL_ZWIFT_ASYNC, NULL, 0, h->now * 1000000);
    BH_CHECK(Zwift(h)->armed && Zwift(h)->phase == SDL_ZWIFT_PLAIN, "an empty value leaves the switch armed");
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(fake.make_calls == 1, "the key goes out after an empty value");
    BH_Destroy(h);

    /* Nothing at 9.999 s, the key at 10 s */
    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    h->now = START_MS + 9999;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(fake.make_calls == 0, "no key write at 9.999 s");
    h->now = START_MS + 10000;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(fake.make_calls == 1, "the key write at 10 s");
    BH_Destroy(h);

    /* The clock waits for the RideOn write's answer */
    FakeInit(&fake);
    h = Subscribed(RIGHT, &fake);
    h->now = START_MS + SDL_ZWIFT_HANDSHAKE_MS + 5000;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(fake.make_calls == 0, "no key write while RideOn is out");
    BH_Destroy(h);

    /* A failed key exchange leaves no timer */
    FakeInit(&fake);
    fake.make_result = false;
    h = Handshaken(RIGHT, &fake);
    BH_Advance(h, START_MS + SDL_ZWIFT_HANDSHAKE_MS);
    BH_CHECK(SDL_BLESession_Ended(&h->session) && !SDL_BLEZwiftModule.GetDeadline(h->state, &deadline),
             "no timer after a failed key exchange");
    BH_Destroy(h);

    /* The reply window runs from the key write's answer */
    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    h->now = START_MS + SDL_ZWIFT_HANDSHAKE_MS;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(fake.make_calls == 1 && h->session.waiting, "the key write is out");
    h->now += 5000;
    BH_Written(h, true);
    h->now = START_MS + 2 * SDL_ZWIFT_HANDSHAKE_MS;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    BH_CHECK(!SDL_BLESession_Ended(&h->session) && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 0,
             "10 s after the key write went out, 5 s after its answer");
    BH_Advance(h, START_MS + 2 * SDL_ZWIFT_HANDSHAKE_MS + 5000);
    BH_CHECK(SDL_BLESession_Ended(&h->session) && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1, "10 s after the answer");
    BH_Destroy(h);

    /* Close stops the timer, and nothing after it is ready */
    FakeInit(&fake);
    h = Handshaken(RIGHT, &fake);
    BH_CHECK(SDL_BLEZwiftModule.GetDeadline(h->state, &deadline), "the handshake timer runs");
    SDL_BLEZwiftModule.Close(h->state, h->now);
    BH_CHECK(!SDL_BLEZwiftModule.GetDeadline(h->state, &deadline), "Close stops the handshake timer");
    SDL_BLEZwiftModule.Value(h->state, SDL_ZWIFT_ASYNC, &idle, 1, h->now * 1000000);
    BH_CHECK(!((const SDL_BLEBase *)h->state)->ready, "a message after Close is not ready");
    BH_Destroy(h);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    TestIdentity();
    Test1();
    Test2to4();
    Test5();
    Test6();
    Test7();
    Test8to10();
    Test11();
    Test12();
    Test13();
    TestSlowPlain();
    Test15();
    Test16();
    TestReader();
    TestSide();
    TestOtherModels();
    TestButtonMap();
    TestLevers();
    TestPlayAxes();
    TestZigZag();
    TestBattery();
    TestReady();
    TestUnknownVariant();
    TestCharacteristics();
    TestClose();
    TestReaderEdges();
    TestRideMalformed();
    TestKeyReplies();
    TestTimerEdges();
    return BH_Report("testblezwift");
}
