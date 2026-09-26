/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/ble/SDL_ble_gearvr_proto.c, the Samsung Gear
   VR controller of hifihedgehog/SDL#33 Part 12. Test numbers follow the
   part's Gear VR section. The blocks marked Decision cover what the part's
   tests leave open. The packets are the hardware fixtures of
   gearvr-controller tests_fixtures.json (MIT), recorded on an ET-YO324 with
   firmware YO324XXU0AQC1 (PROTOCOL.md:3).

   The capture replay reads caps/seq.jsonl, caps/vr_first.jsonl,
   caps/vr_ka.jsonl, caps/vr_idle.jsonl and caps/guided.jsonl from the
   gearvr-controller clone named by the first argument. They are never
   copied into this tree. When they are missing, the test fails. */

#define _CRT_SECURE_NO_WARNINGS
#include "testbleharness.h"
#include "SDL_ble_gearvr_proto.h"

#define CHECK(condition) BH_CHECK(condition, "%s", #condition)

#define PACKET           SDL_GEARVR_PACKET_SIZE
#define STANDARD_GRAVITY 9.80665
#define ACCEL_SCALE      (STANDARD_GRAVITY / 2048.0)
#define GYRO_SCALE       (3.14159265358979323846 / 180.0 / 14.285)
#define MS               ((uint64_t)1000000)

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

typedef struct Fixture
{
    const char *name;
    const char *hex;
} Fixture;

/* gearvr-controller tests_fixtures.json:2-12 */
static const Fixture fixtures[] = {
    { "trigger", "8f63f003ddffee049206e900180150ff8176f00377ff25072c06ee005d0182ff5a89f0030bff4e01f207da004301a4ff0e18effefaff2000001a0164" },
    { "home", "4c5aaa0407001e04a005cbfe060266003e6daa04c1ffd503ea05cafe6e0244003080aa04e5ff7803650600ffae021e00ba1786ff80ff2000001a0264" },
    { "back", "c2cb6705b4ff9304c205cafcfbf76f00b4de670523ffc703770514fd7cf7eb00a6f167057eff4f04f304adfe65f63901e91796feb6ff2000001a0464" },
    { "touchpad", "bc561906ed01b106f50473fffb00b1feae691906f801f2060a059affc300c8feeb7c1906ed01fa06e904bfff8f00e8fee2176800f3ff12449d1a0864" },
    { "volume_up", "98e4db065bffe003ab0664fffaf9a3ff71f7db06a1ff54041c065200c6f9f9ff630adc06ebffac0430063c0189fa6d003418f7fdb0ff2000001a1064" },
    { "volume_down", "050594076fff1004f7065f01e9ffda01f717940735ff1f0411078c01d2ff1a02e92a940701ff24043507a901abff58029518ecfc76ff2000001a2064" },
    { "idle", "ce7f2c0374fff001f307efffb2ff1c00c0922c0372ffef01f507efffb6ff1c00b2a52c0373fff101f407f0ffbcff190051167dfb45ff200000194064" },
    { "touch_center", "58b35508ba005b060a05e2ffc4ff25004ac65508c1005d060805e8ffbdff20003cd95508c0006106ff04edffbbff1d007217f50047001230b31a4064" },
    { "touch_lift", "8de43409bf00ae06a604170094ff0f007ff73409c200a50699041e009eff1a00710a3509b5009206b4042500b3ff17005d171d014f000000001b4064" },
    { "point_up", "6a9e3a0ddaffc9077fff6f01cffeb9005cb13a0de8ffc9076cff4a01d3fe9f004ec43a0de0ffc4076dff3b01e5fe8a009d1888fda3002000001c4064" },
    { "roll_left", "0b5c530ef5075601df00feffc6ff2a00fd6e530ef0074801e300f9ffc1ff3200ef81530ee6074201dd00f3ffbdff3200db16c1fba8ff2000001c4064" },
};

#define NFIXTURES (sizeof(fixtures) / sizeof(fixtures[0]))

static uint8_t packets[NFIXTURES][PACKET];

static void LoadFixtures(void)
{
    size_t i;

    for (i = 0; i < NFIXTURES; ++i) {
        CHECK(strlen(fixtures[i].hex) == 2 * PACKET);
        CHECK(BH_Hex(fixtures[i].hex, packets[i], PACKET) == PACKET);
    }
}

static const uint8_t *Packet(const char *name)
{
    size_t i;

    for (i = 0; i < NFIXTURES; ++i) {
        if (strcmp(fixtures[i].name, name) == 0) {
            return packets[i];
        }
    }
    printf("no fixture named %s\n", name);
    exit(2);
}

/* A packet read straight from the layout of PROTOCOL.md:93-150, to check the
   module against */
typedef struct Raw
{
    uint32_t time[3];
    int16_t accel[3][3];
    int16_t gyro[3][3];
    int state;
    int x;
    int y;
    uint8_t temperature;
    uint8_t buttons;
    uint8_t battery;
} Raw;

static int16_t S16(const uint8_t *p)
{
    return (int16_t)(uint16_t)(p[0] | (p[1] << 8));
}

static void Decode(const uint8_t *p, Raw *raw)
{
    int s, axis;

    for (s = 0; s < 3; ++s) {
        const uint8_t *q = p + 16 * s;

        raw->time[s] = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
        for (axis = 0; axis < 3; ++axis) {
            raw->accel[s][axis] = S16(q + 4 + 2 * axis);
            raw->gyro[s][axis] = S16(q + 10 + 2 * axis);
        }
    }
    raw->state = p[54] >> 4;
    raw->x = ((p[54] & 0x0F) << 6) | (p[55] >> 2);
    raw->y = ((p[55] & 0x03) << 8) | p[56];
    raw->temperature = p[57];
    raw->buttons = p[58];
    raw->battery = p[59];
}

/* Replaces the touch state and coordinates of a packet */
static void SetTouch(uint8_t *p, int state, int x, int y)
{
    p[54] = (uint8_t)((state << 4) | ((x >> 6) & 0x0F));
    p[55] = (uint8_t)(((x & 0x3F) << 2) | ((y >> 8) & 0x03));
    p[56] = (uint8_t)(y & 0xFF);
}

static void SetTime(uint8_t *p, int sample, uint32_t time)
{
    uint8_t *q = p + 16 * sample;

    q[0] = (uint8_t)time;
    q[1] = (uint8_t)(time >> 8);
    q[2] = (uint8_t)(time >> 16);
    q[3] = (uint8_t)(time >> 24);
}

static bool Near(double actual, double expected)
{
    return fabs(actual - expected) <= 1e-5 * (fabs(expected) + 1.0);
}

/* Samples index and index + 1 are the accelerometer and gyroscope of one raw
   sample in SDL units and axes: SDL X = device X, SDL Y = device Z, SDL Z =
   minus device Y */
static bool SampleIs(const BH_Harness *h, int index, const int16_t *accel, const int16_t *gyro, uint64_t sensor_ns,
                     uint64_t time_ns)
{
    const BH_Sample *a, *g;

    if (index < 0 || index + 1 >= h->nsamples) {
        return false;
    }
    a = &h->samples[index];
    g = &h->samples[index + 1];
    return a->sensor == SDL_BLE_SENSOR_ACCEL && g->sensor == SDL_BLE_SENSOR_GYRO &&
           a->sensor_ns == sensor_ns && g->sensor_ns == sensor_ns && a->time_ns == time_ns && g->time_ns == time_ns &&
           Near(a->data[0], accel[0] * ACCEL_SCALE) && Near(a->data[1], accel[2] * ACCEL_SCALE) &&
           Near(a->data[2], -accel[1] * ACCEL_SCALE) && Near(g->data[0], gyro[0] * GYRO_SCALE) &&
           Near(g->data[1], gyro[2] * GYRO_SCALE) && Near(g->data[2], -gyro[1] * GYRO_SCALE);
}

/* The six samples of a packet from index on, each stamped with its own
   device time, for a session whose device clock has not wrapped */
static bool PacketSamplesAre(const BH_Harness *h, int index, const uint8_t *p, uint64_t time_ns)
{
    Raw raw;
    int s;

    Decode(p, &raw);
    for (s = 0; s < 3; ++s) {
        if (!SampleIs(h, index + 2 * s, raw.accel[s], raw.gyro[s], (uint64_t)raw.time[s] * 1000, time_ns)) {
            return false;
        }
    }
    return true;
}

static bool AtRest(const SDL_BLEControls *c)
{
    return c->buttons == 0 && !c->finger && c->finger_x == 0.0f && c->finger_y == 0.0f &&
           c->axes[SDL_BLE_AXIS_LEFTX] == 0 && c->axes[SDL_BLE_AXIS_LEFTY] == 0 &&
           c->axes[SDL_BLE_AXIS_RIGHTX] == 0 && c->axes[SDL_BLE_AXIS_RIGHTY] == 0 &&
           c->axes[SDL_BLE_AXIS_LEFT_TRIGGER] == 0 && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768;
}

static const uint8_t vr_mode[2] = { 0x08, 0x00 };
static const uint8_t sensor_mode[2] = { 0x01, 0x00 };
static const uint8_t keep_alive[2] = { 0x04, 0x00 };
static const uint8_t stop[2] = { 0x00, 0x00 };

/* 4f63756c-7573-2054-6872-65656d6f7465 */
static const SDL_BLEUUID service_uuid =
    SDL_BLE_UUID_INIT(0x4f, 0x63, 0x75, 0x6c, 0x75, 0x73, 0x20, 0x54, 0x68, 0x72, 0x65, 0x65, 0x6d, 0x6f, 0x74, 0x65);
/* c8c51726-81bc-483b-a052-f7a14ea3d281 and ...d282 */
static const SDL_BLEUUID data_uuid =
    SDL_BLE_UUID_INIT(0xc8, 0xc5, 0x17, 0x26, 0x81, 0xbc, 0x48, 0x3b, 0xa0, 0x52, 0xf7, 0xa1, 0x4e, 0xa3, 0xd2, 0x81);
static const SDL_BLEUUID command_uuid =
    SDL_BLE_UUID_INIT(0xc8, 0xc5, 0x17, 0x26, 0x81, 0xbc, 0x48, 0x3b, 0xa0, 0x52, 0xf7, 0xa1, 0x4e, 0xa3, 0xd2, 0x82);

static BH_Harness *Create(bool pairing_allowed)
{
    return BH_Create(&SDL_BLEGearVRFamily, 0, NULL, pairing_allowed);
}

static SDL_GearVRState *State(const BH_Harness *h)
{
    return (SDL_GearVRState *)h->state;
}

static int Writes(const BH_Harness *h)
{
    int unused[1];

    return BH_Writes(h, unused, 0);
}

/* The log index of write n, or -1 */
static int WriteAt(const BH_Harness *h, int n)
{
    int indices[64];
    const int count = BH_Writes(h, indices, 64);

    return (n >= 0 && n < count && n < 64) ? indices[n] : -1;
}

/* The action at index writes command to the command characteristic with
   response, at time now */
static bool IsCommand(const BH_Harness *h, int index, const uint8_t *command, uint64_t now)
{
    return BH_IsWrite(h, index, command, 2) && h->actions[index].action.characteristic == SDL_GEARVR_COMMAND &&
           h->actions[index].action.response && h->actions[index].now == now;
}

/* One value on the data characteristic at the harness clock, with any write
   it causes answered */
static void Data(BH_Harness *h, const uint8_t *data, size_t length)
{
    BH_Value(h, SDL_GEARVR_DATA, data, length, false);
    BH_WriteAll(h);
}

/* What a value that changes nothing leaves alone, the module state included */
typedef struct Mark
{
    int nactions;
    int nsnapshots;
    int nsamples;
    int published;
    uint8_t state[sizeof(SDL_GearVRState)];
} Mark;

static void Take(const BH_Harness *h, Mark *mark)
{
    mark->nactions = h->nactions;
    mark->nsnapshots = h->nsnapshots;
    mark->nsamples = h->nsamples;
    mark->published = h->published;
    memcpy(mark->state, h->state, sizeof(mark->state));
}

static bool Unchanged(const BH_Harness *h, const Mark *mark)
{
    return h->nactions == mark->nactions && h->nsnapshots == mark->nsnapshots && h->nsamples == mark->nsamples &&
           h->published == mark->published && memcmp(h->state, mark->state, sizeof(mark->state)) == 0;
}

/* A session started at start, its 08 00 written and answered */
static BH_Harness *Started(uint64_t start)
{
    BH_Harness *h = Create(false);

    h->now = start;
    BH_Start(h, false);
    BH_WriteAll(h);
    return h;
}

/* Started, with the 08 00 echo 1580 ms later and its 01 00 answered */
static BH_Harness *Streaming(uint64_t start)
{
    BH_Harness *h = Started(start);

    BH_Advance(h, start + 1580);
    Data(h, vr_mode, sizeof(vr_mode));
    return h;
}

/* Streaming, and published by the idle fixture 20 ms after the echo */
static BH_Harness *Published(uint64_t start)
{
    BH_Harness *h = Streaming(start);

    BH_Advance(h, start + 1600);
    Data(h, Packet("idle"), PACKET);
    return h;
}

/* touch_center with every button and the trigger held */
static void Held(uint8_t *p)
{
    memcpy(p, Packet("touch_center"), PACKET);
    p[58] = 0x3F;
}

/* A live stream: the clock advances to until with the packet every 100 ms,
   well inside the silence timeout, and the timers run on the way */
static void Stream(BH_Harness *h, uint64_t until, const uint8_t *packet)
{
    while (h->now < until) {
        BH_Advance(h, (until - h->now > 100) ? h->now + 100 : until);
        Data(h, packet, PACKET);
    }
}

static bool AllHeld(const SDL_BLEControls *c)
{
    const uint32_t buttons = (1u << SDL_BLE_BUTTON_SOUTH) | (1u << SDL_BLE_BUTTON_BACK) | (1u << SDL_BLE_BUTTON_GUIDE) |
                             (1u << SDL_BLE_BUTTON_LEFT_SHOULDER) | (1u << SDL_BLE_BUTTON_RIGHT_SHOULDER);

    return c->buttons == buttons && c->finger && c->axes[SDL_BLE_AXIS_LEFTX] == -4095 &&
           c->axes[SDL_BLE_AXIS_LEFTY] == 3891 && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == 32767;
}

/* Decision: the identity the joystick is published with, and the family */
static void TestIdentity(void)
{
    BH_Harness *h = Create(false);
    const SDL_BLEIdentity *identity = &((const SDL_BLEBase *)h->state)->identity;
    const SDL_BLEFamily *family = &SDL_BLEGearVRFamily;
    uint64_t deadline;

    Section("Decision: identity and family");
    CHECK(strcmp(identity->name, "Samsung Gear VR Controller") == 0);
    CHECK(identity->type == SDL_BLE_TYPE_GAMEPAD && identity->gamepad && identity->touchpad);
    CHECK(identity->vendor == 0 && identity->product == 0);
    CHECK(identity->buttons == ((1u << SDL_BLE_BUTTON_SOUTH) | (1u << SDL_BLE_BUTTON_BACK) | (1u << SDL_BLE_BUTTON_GUIDE) |
                                (1u << SDL_BLE_BUTTON_LEFT_SHOULDER) | (1u << SDL_BLE_BUTTON_RIGHT_SHOULDER)));
    CHECK(identity->axes == (uint8_t)((1u << SDL_BLE_AXIS_LEFTX) | (1u << SDL_BLE_AXIS_LEFTY) | (1u << SDL_BLE_AXIS_RIGHT_TRIGGER)));
    CHECK(identity->accel_rate == 206.0f && identity->gyro_rate == 206.0f);
    CHECK(AtRest(BH_Controls(h)) && BH_Controls(h)->battery == -1);
    CHECK(!SDL_BLEGearVRModule.GetDeadline(h->state, &deadline));

    CHECK(SDL_BLE_Families[SDL_BLE_FAMILY_GEARVR] == family && family->module == &SDL_BLEGearVRModule);
    CHECK(SDL_BLEGearVRModule.state_size == sizeof(SDL_GearVRState));
    CHECK(family->nservices == 1 && SDL_BLE_UUIDEqual(&family->services[0], &service_uuid) && !family->has_alternate);
    CHECK(family->ncharacteristics == 2);
    CHECK(SDL_BLE_UUIDEqual(&family->characteristics[SDL_GEARVR_DATA].uuid, &data_uuid));
    CHECK(family->characteristics[SDL_GEARVR_DATA].flags == SDL_BLE_CHAR_SUBSCRIBE);
    CHECK(family->characteristics[SDL_GEARVR_DATA].service == 0);
    CHECK(SDL_BLE_UUIDEqual(&family->characteristics[SDL_GEARVR_COMMAND].uuid, &command_uuid));
    CHECK(family->characteristics[SDL_GEARVR_COMMAND].flags == 0);
    CHECK(family->characteristics[SDL_GEARVR_COMMAND].service == 0);
    CHECK(family->pairing == SDL_BLE_PAIR_ON_ERROR);
    BH_Destroy(h);
}

/* Test 1: the idle fixture */
static void Test1(void)
{
    static const uint32_t times[3] = { 53247950, 53252800, 53257650 };
    static const int16_t accel3[3] = { -141, 497, 2036 };
    BH_Harness *h = Published(0);
    const SDL_BLEControls *c = BH_Controls(h);
    Raw raw;
    int s;

    Section("Test 1: idle");
    Decode(Packet("idle"), &raw);
    CHECK(h->published == 1 && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1);
    /* No buttons and no finger. Bit 6 of byte 58 is the idle flag. */
    CHECK(raw.buttons == 0x40 && raw.state == 2);
    CHECK(AtRest(c));
    CHECK(c->battery == 100 && raw.battery == 100);
    /* The temperature, 25 degrees Celsius in byte 57, is not exposed */
    CHECK(raw.temperature == 25);
    CHECK(h->nsamples == 6);
    for (s = 0; s < 3; ++s) {
        CHECK(raw.time[s] == times[s]);
        CHECK(SampleIs(h, 2 * s, raw.accel[s], raw.gyro[s], (uint64_t)times[s] * 1000, 1600 * MS));
    }
    CHECK(memcmp(raw.accel[2], accel3, sizeof(accel3)) == 0);
    /* +0.99 g on SDL Y, the axis with the most of it */
    CHECK(fabs(h->samples[4].data[1] / STANDARD_GRAVITY - 0.99) < 0.005);
    CHECK(h->samples[4].data[1] > fabs(h->samples[4].data[0]) && h->samples[4].data[1] > fabs(h->samples[4].data[2]));
    BH_Destroy(h);
}

/* Test 2: the trigger fixture */
static void Test2(void)
{
    BH_Harness *h = Streaming(0);
    const uint8_t *p = Packet("trigger");
    const SDL_BLEControls *c = BH_Controls(h);

    Section("Test 2: trigger");
    BH_Advance(h, 1600);
    Data(h, p, PACKET);
    CHECK(p[58] == 0x01);
    CHECK(c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == 32767);
    CHECK(c->buttons == 0 && !c->finger);
    CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == 0 && c->axes[SDL_BLE_AXIS_LEFTY] == 0 && c->axes[SDL_BLE_AXIS_LEFT_TRIGGER] == 0);
    CHECK(c->battery == 100 && h->published == 1);
    CHECK(h->nsamples == 6 && PacketSamplesAre(h, 0, p, 1600 * MS));
    BH_Destroy(h);
}

/* Test 3: one button per fixture */
static void Test3(void)
{
    static const struct
    {
        const char *name;
        uint8_t byte58;
        int button;
    } cases[] = {
        { "home", 0x02, SDL_BLE_BUTTON_GUIDE },
        { "back", 0x04, SDL_BLE_BUTTON_BACK },
        { "touchpad", 0x08, SDL_BLE_BUTTON_SOUTH },
        { "volume_up", 0x10, SDL_BLE_BUTTON_RIGHT_SHOULDER },
        { "volume_down", 0x20, SDL_BLE_BUTTON_LEFT_SHOULDER },
    };
    size_t i;

    Section("Test 3: buttons");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        BH_Harness *h = Streaming(0);
        const uint8_t *p = Packet(cases[i].name);
        const SDL_BLEControls *c = BH_Controls(h);

        BH_Advance(h, 1600);
        Data(h, p, PACKET);
        CHECK(p[58] == cases[i].byte58);
        CHECK(c->buttons == (1u << cases[i].button));
        CHECK(c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768);
        if (strcmp(cases[i].name, "touchpad") == 0) {
            /* The finger is down at (145, 157) */
            CHECK(c->finger && c->finger_x == 145.0f / 320.0f && c->finger_y == 157.0f / 320.0f);
            CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == -3071 && c->axes[SDL_BLE_AXIS_LEFTY] == -614);
        } else {
            CHECK(!c->finger && c->axes[SDL_BLE_AXIS_LEFTX] == 0 && c->axes[SDL_BLE_AXIS_LEFTY] == 0);
        }
        CHECK(h->nsamples == 6 && PacketSamplesAre(h, 0, p, 1600 * MS));
        BH_Destroy(h);
    }
}

/* Test 4: touch_center, then touch_lift */
static void Test4(void)
{
    BH_Harness *h = Streaming(0);
    const uint8_t *lift = Packet("touch_lift");
    const SDL_BLEControls *c = BH_Controls(h);
    Raw raw;

    Section("Test 4: touch");
    BH_Advance(h, 1600);
    Data(h, Packet("touch_center"), PACKET);
    Decode(Packet("touch_center"), &raw);
    CHECK(raw.state == 1 && raw.x == 140 && raw.y == 179);
    CHECK(c->finger && c->finger_x == 140.0f / 320.0f && c->finger_y == 179.0f / 320.0f);
    CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == -4095 && c->axes[SDL_BLE_AXIS_LEFTY] == 3891);
    CHECK(c->buttons == 0 && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768);
    BH_Advance(h, 1615);
    Data(h, lift, PACKET);
    CHECK(lift[54] == 0x00 && lift[55] == 0x00 && lift[56] == 0x00);
    CHECK(!c->finger && c->finger_x == 0.0f && c->finger_y == 0.0f);
    CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == 0 && c->axes[SDL_BLE_AXIS_LEFTY] == 0);
    /* Down, then up: one change each */
    CHECK(h->nsnapshots == 2);
    CHECK(h->nsamples == 12 && PacketSamplesAre(h, 6, lift, 1615 * MS));
    BH_Destroy(h);
}

/* Test 5: point_up and roll_left */
static void Test5(void)
{
    static const int16_t up3[3] = { -32, 1988, -147 };
    static const int16_t left3[3] = { 2022, 322, 221 };
    BH_Harness *h;
    const BH_Sample *a;
    Raw raw;

    Section("Test 5: gravity axes");
    h = Streaming(0);
    BH_Advance(h, 1600);
    Data(h, Packet("point_up"), PACKET);
    Decode(Packet("point_up"), &raw);
    CHECK(memcmp(raw.accel[2], up3, sizeof(up3)) == 0);
    CHECK(h->nsamples == 6 && PacketSamplesAre(h, 0, Packet("point_up"), 1600 * MS));
    /* The far end up: about -0.97 g on SDL Z, which grows toward the player */
    a = &h->samples[4];
    CHECK(fabs(a->data[2] / STANDARD_GRAVITY + 0.97) < 0.005);
    CHECK(fabs(a->data[2]) > fabs(a->data[0]) && fabs(a->data[2]) > fabs(a->data[1]));
    BH_Destroy(h);

    h = Streaming(0);
    BH_Advance(h, 1600);
    Data(h, Packet("roll_left"), PACKET);
    Decode(Packet("roll_left"), &raw);
    CHECK(memcmp(raw.accel[2], left3, sizeof(left3)) == 0);
    CHECK(h->nsamples == 6 && PacketSamplesAre(h, 0, Packet("roll_left"), 1600 * MS));
    /* Rolled onto its left side: about +0.99 g on SDL X */
    a = &h->samples[4];
    CHECK(fabs(a->data[0] / STANDARD_GRAVITY - 0.99) < 0.005);
    CHECK(a->data[0] > fabs(a->data[1]) && a->data[0] > fabs(a->data[2]));
    BH_Destroy(h);
}

/* Test 6: truncations, the 2-byte truncation and stale bytes */
static void Test6(void)
{
    uint8_t stale[SDL_BLE_MAX_VALUE];
    size_t f, length;
    Mark mark;
    BH_Harness *h;

    Section("Test 6: truncations");
    for (f = 0; f < NFIXTURES; ++f) {
        h = Published(0);
        /* With the finger down and the trigger held, so a release would show */
        BH_Advance(h, 1615);
        Held(stale);
        Data(h, stale, PACKET);
        CHECK(AllHeld(BH_Controls(h)));
        Take(h, &mark);
        for (length = 0; length < PACKET; ++length) {
            Data(h, packets[f], length);
            CHECK(Unchanged(h, &mark));
        }
        BH_Destroy(h);
    }

    /* A 2-byte truncation is no echo, while the echo is awaited or later */
    h = Started(0);
    Take(h, &mark);
    for (f = 0; f < NFIXTURES; ++f) {
        Data(h, packets[f], 2);
        CHECK(Unchanged(h, &mark));
    }
    CHECK(State(h)->phase == SDL_GEARVR_WAIT_ACK);
    BH_Advance(h, 1580);
    Data(h, vr_mode, 2);
    CHECK(Writes(h) == 2 && IsCommand(h, WriteAt(h, 1), sensor_mode, 1580));
    BH_Advance(h, 4000);
    CHECK(Writes(h) == 2);
    BH_Destroy(h);

    /* Bytes after byte 59 in the buffer change nothing */
    for (f = 0; f < NFIXTURES; ++f) {
        BH_Harness *exact = Streaming(0);
        BH_Harness *padded = Streaming(0);
        int i;

        BH_Advance(exact, 1600);
        BH_Advance(padded, 1600);
        Data(exact, packets[f], PACKET);
        memset(stale, 0xA5, sizeof(stale));
        memcpy(stale, packets[f], PACKET);
        /* Straight to the session, so the buffer holds bytes past the length */
        SDL_BLESession_Value(&padded->session, SDL_GEARVR_DATA, stale, PACKET, false, padded->now * MS);
        BH_Drain(padded);
        BH_WriteAll(padded);
        CHECK(padded->nsnapshots == exact->nsnapshots && padded->nsamples == 6 && exact->nsamples == 6);
        CHECK(SDL_BLE_ControlsEqual(BH_Controls(exact), BH_Controls(padded)));
        CHECK(padded->published == 1 && exact->published == 1);
        for (i = 0; i < 6; ++i) {
            const BH_Sample *a = &exact->samples[i];
            const BH_Sample *b = &padded->samples[i];

            CHECK(a->sensor == b->sensor && a->time_ns == b->time_ns && a->sensor_ns == b->sensor_ns &&
                  a->data[0] == b->data[0] && a->data[1] == b->data[1] && a->data[2] == b->data[2]);
        }
        BH_Destroy(exact);
        BH_Destroy(padded);
    }
}

/* Test 7: the start-up on an injected clock */
static void Test7(void)
{
    BH_Harness *h;
    Mark mark;
    int index;

    Section("Test 7: start-up");
    /* After the subscription, exactly one write 08 00 */
    h = Create(false);
    h->now = 5000;
    BH_Start(h, false);
    index = WriteAt(h, 0);
    CHECK(Writes(h) == 1 && IsCommand(h, index, vr_mode, 5000));
    CHECK(index > 0 && h->actions[index - 1].action.kind == SDL_BLE_ACTION_SUBSCRIBE);
    CHECK(State(h)->phase == SDL_GEARVR_WAIT_ACK);
    BH_WriteAll(h);
    BH_Advance(h, 6579);
    CHECK(Writes(h) == 1);
    /* The echo at +1580 ms is followed at once by one write 01 00 */
    BH_Advance(h, 6580);
    index = h->nactions;
    BH_Value(h, SDL_GEARVR_DATA, vr_mode, 2, false);
    CHECK(h->nactions == index + 1 && IsCommand(h, index, sensor_mode, 6580));
    CHECK(Writes(h) == 2 && State(h)->phase == SDL_GEARVR_STREAMING);
    BH_WriteAll(h);
    /* Nothing more at +4000 ms */
    BH_Advance(h, 9000);
    CHECK(Writes(h) == 2);
    /* A later 01 00 echo is not input: no change, no write and no joystick */
    Take(h, &mark);
    Data(h, sensor_mode, 2);
    CHECK(Unchanged(h, &mark) && h->published == 0);
    BH_Destroy(h);

    /* Without the echo, one write 01 00 at +4000 ms */
    h = Started(5000);
    BH_Advance(h, 8999);
    CHECK(Writes(h) == 1 && State(h)->phase == SDL_GEARVR_WAIT_ACK);
    BH_Advance(h, 9000);
    CHECK(Writes(h) == 2 && IsCommand(h, WriteAt(h, 1), sensor_mode, 9000));
    CHECK(State(h)->phase == SDL_GEARVR_STREAMING);
    /* A late echo writes nothing */
    BH_Advance(h, 9500);
    Take(h, &mark);
    Data(h, vr_mode, 2);
    CHECK(Unchanged(h, &mark));
    Data(h, sensor_mode, 2);
    CHECK(Unchanged(h, &mark) && h->published == 0);
    BH_Destroy(h);
}

/* Test 8: the keep-alive, and 00 00 on close */
static void Test8(void)
{
    BH_Harness *h;
    uint64_t deadline;
    int index;

    Section("Test 8: keep-alive and close");
    /* 01 00 at 1580 ms: 04 00 at +10 s and +20 s after it, on a live stream */
    h = Published(0);
    Stream(h, 11579, Packet("idle"));
    CHECK(Writes(h) == 2);
    Stream(h, 11580, Packet("idle"));
    CHECK(Writes(h) == 3 && IsCommand(h, WriteAt(h, 2), keep_alive, 11580));
    Stream(h, 21579, Packet("idle"));
    CHECK(Writes(h) == 3);
    Stream(h, 21580, Packet("idle"));
    CHECK(Writes(h) == 4 && IsCommand(h, WriteAt(h, 3), keep_alive, 21580));
    /* None after link loss */
    Stream(h, 25000, Packet("idle"));
    SDL_BLESession_Lost(&h->session, h->now);
    BH_Drain(h);
    CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline));
    BH_Advance(h, 100000);
    CHECK(Writes(h) == 4);
    BH_Destroy(h);

    /* A late tick sends one keep-alive and keeps the cadence. A packet just
       before each tick keeps the stream live. */
    h = Published(0);
    h->now = 14000;
    Data(h, Packet("idle"), PACKET);
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    BH_WriteAll(h);
    CHECK(Writes(h) == 3 && IsCommand(h, WriteAt(h, 2), keep_alive, 14000));
    CHECK(State(h)->deadline == 21580);
    /* The silence timer, 3 s after the packet, comes first */
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 17000);
    /* A tick that missed two periods sends one, and the cadence restarts
       from it */
    h->now = 45000;
    Data(h, Packet("idle"), PACKET);
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    BH_WriteAll(h);
    CHECK(Writes(h) == 4 && IsCommand(h, WriteAt(h, 3), keep_alive, 45000));
    CHECK(State(h)->deadline == 55000);
    h->now = 54999;
    Data(h, Packet("idle"), PACKET);
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(Writes(h) == 4);
    BH_Destroy(h);

    /* 00 00 on close, then the joystick goes */
    h = Published(0);
    Stream(h, 12000, Packet("idle"));
    CHECK(Writes(h) == 3);
    index = h->nactions;
    SDL_BLESession_Close(&h->session, h->now);
    BH_Drain(h);
    CHECK(h->nactions == index + 1 && IsCommand(h, index, stop, 12000) && h->actions[index].action.closing);
    BH_Written(h, true);
    CHECK(h->nactions == index + 3);
    CHECK(h->actions[index + 1].action.kind == SDL_BLE_ACTION_REMOVE);
    CHECK(h->actions[index + 2].action.kind == SDL_BLE_ACTION_DISCONNECT);
    CHECK(SDL_BLESession_Ended(&h->session) && !h->session.backoff && h->published == 0);
    /* The module itself keeps no timer after its close */
    CHECK(!SDL_BLEGearVRModule.GetDeadline(h->state, &deadline) && State(h)->phase == SDL_GEARVR_IDLE);
    BH_Advance(h, 100000);
    CHECK(Writes(h) == 4);
    BH_Destroy(h);

    /* A close while a keep-alive is out waits for it, then writes 00 00 */
    h = Published(0);
    h->now = 11580;
    Data(h, Packet("idle"), PACKET);
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(Writes(h) == 3 && h->session.waiting);
    index = h->nactions;
    SDL_BLESession_Close(&h->session, 11581);
    BH_Drain(h);
    CHECK(h->nactions == index);
    h->now = 11590;
    BH_Written(h, true);
    CHECK(h->nactions == index + 1 && IsCommand(h, index, stop, 11590) && h->actions[index].action.closing);
    BH_Written(h, true);
    CHECK(BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT && SDL_BLESession_Ended(&h->session));
    CHECK(Writes(h) == 4);
    BH_Destroy(h);

    /* A close while the echo is awaited writes 00 00 and never 01 00 */
    h = Started(0);
    BH_Advance(h, 1000);
    SDL_BLESession_Close(&h->session, h->now);
    BH_Drain(h);
    CHECK(Writes(h) == 2 && IsCommand(h, WriteAt(h, 1), stop, 1000));
    BH_Written(h, true);
    BH_Advance(h, 60000);
    CHECK(Writes(h) == 2 && SDL_BLESession_Ended(&h->session));
    CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0 && BH_Count(h, SDL_BLE_ACTION_REMOVE) == 0);
    BH_Destroy(h);
}

static bool LastIsSubscribe(const BH_Harness *h, uint8_t cccd)
{
    const SDL_BLEAction *action = BH_Last(h);

    return action && action->kind == SDL_BLE_ACTION_SUBSCRIBE && action->characteristic == SDL_GEARVR_DATA &&
           action->cccd == cccd && h->session.waiting;
}

/* Test 9: pairing */
static void Test9(void)
{
    static const uint8_t errors[2] = { SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION };
    BH_Harness *h;
    int i;

    Section("Test 9: pairing");
    for (i = 0; i < 2; ++i) {
        /* With the hint on, a failure for want of a bond leads to pair, then
           one repeat, which a bond starts with None */
        h = Create(true);
        BH_Connected(h, true, false);
        BH_Discovered(h, true, NULL);
        CHECK(LastIsSubscribe(h, SDL_BLE_CCCD_NOTIFY));
        BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, errors[i]);
        CHECK(BH_LastKind(h) == SDL_BLE_ACTION_PAIR);
        BH_Paired(h, true);
        CHECK(LastIsSubscribe(h, SDL_BLE_CCCD_NONE));
        BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
        CHECK(LastIsSubscribe(h, SDL_BLE_CCCD_NOTIFY));
        BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
        CHECK(Writes(h) == 1 && IsCommand(h, WriteAt(h, 0), vr_mode, 0));
        CHECK(BH_Count(h, SDL_BLE_ACTION_PAIR) == 1 && BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == 3);
        CHECK(BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 0 && !SDL_BLESession_Ended(&h->session));
        BH_Destroy(h);

        /* A second failure, in the repeat, backs off */
        h = Create(true);
        BH_Connected(h, true, false);
        BH_Discovered(h, true, NULL);
        BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, errors[i]);
        BH_Paired(h, true);
        BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, errors[i]);
        CHECK(BH_Count(h, SDL_BLE_ACTION_PAIR) == 1 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1);
        CHECK(BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT && SDL_BLESession_Ended(&h->session) && Writes(h) == 0);
        BH_Destroy(h);

        /* With the hint off, no pairing */
        h = Create(false);
        BH_Connected(h, true, false);
        BH_Discovered(h, true, NULL);
        BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, errors[i]);
        CHECK(BH_Count(h, SDL_BLE_ACTION_PAIR) == 0 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1);
        CHECK(BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT && SDL_BLESession_Ended(&h->session) && Writes(h) == 0);
        BH_Destroy(h);
    }

    /* Unreachable on a device Windows reports bonded: remove the bond, then
       one repeat, without the bond and so without None */
    h = Create(true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    CHECK(LastIsSubscribe(h, SDL_BLE_CCCD_NONE));
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    CHECK(BH_LastKind(h) == SDL_BLE_ACTION_REMOVE_BOND);
    BH_BondRemoved(h, true);
    CHECK(LastIsSubscribe(h, SDL_BLE_CCCD_NOTIFY));
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    CHECK(Writes(h) == 1 && IsCommand(h, WriteAt(h, 0), vr_mode, 0));
    CHECK(BH_Count(h, SDL_BLE_ACTION_REMOVE_BOND) == 1 && BH_Count(h, SDL_BLE_ACTION_PAIR) == 0);
    CHECK(BH_Count(h, SDL_BLE_ACTION_SUBSCRIBE) == 2);
    BH_Destroy(h);

    /* With the hint off, the bond stays and the session backs off */
    h = Create(false);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    CHECK(BH_Count(h, SDL_BLE_ACTION_REMOVE_BOND) == 0 && BH_Count(h, SDL_BLE_ACTION_BACKOFF) == 1);
    CHECK(BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT && Writes(h) == 0);
    BH_Destroy(h);
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

static void AddManufacturer(SDL_BLEAdvertisement *ad, uint16_t company, const uint8_t *data, size_t length)
{
    SDL_BLEManufacturerData *m;

    if (ad->nmanufacturer >= SDL_BLE_AD_MANUFACTURER) {
        return;
    }
    m = &ad->manufacturer[ad->nmanufacturer++];
    m->company = company;
    if (length > sizeof(m->data)) {
        length = sizeof(m->data);
    }
    m->length = (uint8_t)length;
    memcpy(m->data, data, length);
}

static int Match(const SDL_BLEAdvertisement *ad)
{
    uint8_t variant = 0xFF;
    const int family = SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, ad, &variant);

    return (family < 0 || variant == 0) ? family : -2;
}

/* The manufacturer data of PROTOCOL.md:16-17, after company 0x0075 */
static const uint8_t samsung_data[] = {
    0x01, 0x00, 0x02, 0x00, 0xfb, 0x01, 0x02, 0x0e, 0x03,
    'v', 'r', 's', 'e', 't', 'u', 'p', 'w', 'i', 'z', 'a', 'r', 'd', 0x10
};

/* Test 10: discovery */
static void Test10(void)
{
    static const char *const misses[] = {
        "Gear VR Controlle", "gear vr controller(466A)", "GEAR VR CONTROLLER(466A)", " Gear VR Controller(466A)", ""
    };
    SDL_BLEAdvertisement ad;
    bool enabled[SDL_BLE_FAMILY_COUNT];
    size_t i;
    int f;

    Section("Test 10: discovery");
    /* The service UUID alone */
    Advertisement(&ad, 0x6A46A1B2C3D4, SDL_BLE_AD_ADVERTISEMENT);
    AddService(&ad, &service_uuid);
    CHECK(Match(&ad) == SDL_BLE_FAMILY_GEARVR);
    /* The name alone, in an advertisement or a scan response */
    Advertisement(&ad, 0x6A46A1B2C3D4, SDL_BLE_AD_ADVERTISEMENT);
    AddName(&ad, "Gear VR Controller(466A)");
    CHECK(Match(&ad) == SDL_BLE_FAMILY_GEARVR);
    Advertisement(&ad, 0x6A46A1B2C3D4, SDL_BLE_AD_SCAN_RESPONSE);
    AddName(&ad, "Gear VR Controller(466A)");
    CHECK(Match(&ad) == SDL_BLE_FAMILY_GEARVR);
    /* Company 0x0075 alone does not */
    Advertisement(&ad, 0x6A46A1B2C3D4, SDL_BLE_AD_ADVERTISEMENT);
    AddManufacturer(&ad, 0x0075, samsung_data, sizeof(samsung_data));
    CHECK(Match(&ad) == -1);
    /* With the service, it matches by the service, with no variant */
    AddService(&ad, &service_uuid);
    CHECK(Match(&ad) == SDL_BLE_FAMILY_GEARVR);
    /* Names that do not start with the key, case for case */
    for (i = 0; i < sizeof(misses) / sizeof(misses[0]); ++i) {
        Advertisement(&ad, 0x6A46A1B2C3D4, SDL_BLE_AD_ADVERTISEMENT);
        AddName(&ad, misses[i]);
        AddManufacturer(&ad, 0x0075, samsung_data, sizeof(samsung_data));
        CHECK(Match(&ad) == -1);
    }
    /* A disabled family matches nothing */
    for (f = 0; f < SDL_BLE_FAMILY_COUNT; ++f) {
        enabled[f] = (f != SDL_BLE_FAMILY_GEARVR);
    }
    Advertisement(&ad, 0x6A46A1B2C3D4, SDL_BLE_AD_ADVERTISEMENT);
    AddName(&ad, "Gear VR Controller(466A)");
    AddService(&ad, &service_uuid);
    CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, enabled, &ad, NULL) == -1);
}

/* Test 11: link loss and the next advertisement */
static void Test11(void)
{
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad;
    SDL_BLEMatch match;
    uint8_t held[PACKET];
    uint64_t deadline;
    BH_Harness *h;
    const SDL_BLEControls *c;
    int writes;

    Section("Test 11: link loss");
    SDL_BLEHost_Init(&host);
    Advertisement(&ad, 0x6A46A1B2C3D4, SDL_BLE_AD_ADVERTISEMENT);
    AddName(&ad, "Gear VR Controller(466A)");
    CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 0, &match));
    CHECK(match.family == SDL_BLE_FAMILY_GEARVR && match.variant == 0);
    /* The address is reserved while its session runs */
    CHECK(!SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 10, &match));

    h = Published(0);
    c = BH_Controls(h);
    BH_Advance(h, 1615);
    Held(held);
    Data(h, held, PACKET);
    CHECK(AllHeld(c));
    Stream(h, 5000, held);
    CHECK(AllHeld(c));
    writes = Writes(h);
    SDL_BLESession_Lost(&h->session, h->now);
    BH_Drain(h);
    /* Every button released, the finger lifted, the battery kept */
    CHECK(AtRest(c) && c->battery == 100);
    CHECK(h->nsnapshots >= 1 && SDL_BLE_ControlsEqual(&h->snapshots[h->nsnapshots - 1].controls, c));
    CHECK(h->snapshots[h->nsnapshots - 1].time_ns == 5000 * MS);
    CHECK(BH_Count(h, SDL_BLE_ACTION_REMOVE) == 1 && BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT);
    CHECK(h->published == 0 && SDL_BLESession_Ended(&h->session) && !h->session.backoff);
    /* No timer after the loss */
    CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline));
    BH_Advance(h, 60000);
    CHECK(Writes(h) == writes);
    {
        SDL_BLEOutcome outcome;

        SDL_BLESession_GetOutcome(&h->session, &outcome);
        SDL_BLEHost_SessionEnded(&host, ad.address, &outcome, h->now);
    }
    BH_Destroy(h);

    /* The next advertisement reconnects and the start-up repeats from 08 00 */
    CHECK(SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, &ad, 60001, &match));
    CHECK(match.family == SDL_BLE_FAMILY_GEARVR);
    h = Create(false);
    h->now = 60001;
    BH_Start(h, false);
    CHECK(Writes(h) == 1 && IsCommand(h, WriteAt(h, 0), vr_mode, 60001));
    CHECK(State(h)->phase == SDL_GEARVR_WAIT_ACK && !State(h)->have_time && State(h)->restarts == 0 && h->published == 0);
    CHECK(AtRest(BH_Controls(h)) && BH_Controls(h)->battery == -1);
    BH_Destroy(h);
}

/* Test 12: the sensor timestamp across the wrap of the device clock */
static void Test12(void)
{
    uint8_t a[PACKET], b[PACKET];
    BH_Harness *h = Streaming(0);
    Raw raw;
    int s;

    Section("Test 12: clock wrap");
    memcpy(a, Packet("idle"), PACKET);
    SetTime(a, 0, 4294957300u);
    SetTime(a, 1, 4294962150u);
    SetTime(a, 2, 4294967000u);
    memcpy(b, Packet("idle"), PACKET);
    SetTime(b, 0, 4554);
    SetTime(b, 1, 9404);
    SetTime(b, 2, 14254);
    BH_Advance(h, 1600);
    Data(h, a, PACKET);
    BH_Advance(h, 1615);
    Data(h, b, PACKET);
    CHECK(h->nsamples == 12);
    CHECK(PacketSamplesAre(h, 0, a, 1600 * MS));
    CHECK(h->samples[4].sensor_ns == (uint64_t)4294967000u * 1000);
    /* 4294967000 then 4554 is 4850 us */
    CHECK(h->samples[6].sensor_ns - h->samples[4].sensor_ns == 4850 * 1000);
    CHECK(h->samples[6].sensor_ns == ((uint64_t)1 << 32) * 1000 + 4554 * 1000);
    Decode(b, &raw);
    for (s = 0; s < 3; ++s) {
        CHECK(SampleIs(h, 6 + 2 * s, raw.accel[s], raw.gyro[s], (((uint64_t)1 << 32) + raw.time[s]) * 1000, 1615 * MS));
    }
    for (s = 1; s < 6; ++s) {
        CHECK(h->samples[2 * s].sensor_ns - h->samples[2 * s - 2].sensor_ns == 4850 * 1000);
    }
    BH_Destroy(h);
}

/* Decision: only a 2-byte echo or a 60-byte packet is read, since the
   length names the packet (daydream-catcher Reference :125-136) */
static void TestLengths(void)
{
    uint8_t value[SDL_BLE_MAX_VALUE];
    uint8_t record[68];
    uint8_t p[PACKET];
    size_t length;
    Mark mark;
    BH_Harness *h;
    int i;

    Section("Decision: lengths");
    h = Published(0);
    Held(value);
    for (i = PACKET; i < (int)sizeof(value); ++i) {
        value[i] = (uint8_t)i;
    }
    Take(h, &mark);
    for (length = 0; length <= sizeof(value); ++length) {
        if (length == 2 || length == PACKET) {
            continue;
        }
        Data(h, value, length);
        CHECK(Unchanged(h, &mark));
    }
    /* The 12-byte suffix alone, as after 07 00 first (writeup 2 :43) */
    Data(h, value + 48, 12);
    CHECK(Unchanged(h, &mark));
    /* Sensor packets with one, two and four samples */
    memcpy(record, value, 16);
    memcpy(record + 16, value + 48, 12);
    Data(h, record, 28);
    CHECK(Unchanged(h, &mark));
    memcpy(record, value, 32);
    memcpy(record + 32, value + 48, 12);
    Data(h, record, 44);
    CHECK(Unchanged(h, &mark));
    memcpy(value + 60, value, 16);
    Data(h, value, 76);
    CHECK(Unchanged(h, &mark));
    /* The 68-byte device record of PROTOCOL.md:48-57, and its first 40 bytes */
    memset(record, 0, sizeof(record));
    record[0] = 0x05;
    record[4] = 0x0a;
    record[8] = 0x01;
    memcpy(record + 12, "QC1", 4);
    record[16] = 0x3f;
    for (i = 0; i < 4; ++i) {
        record[20 + 4 * i] = 0x40;
        record[21 + 4 * i] = 0x01;
    }
    record[36] = 0x01;
    memcpy(record + 40, "ET-YO324RF7J416JM1Z", 19);
    Data(h, record, 68);
    CHECK(Unchanged(h, &mark));
    Data(h, record, 40);
    CHECK(Unchanged(h, &mark));
    BH_Destroy(h);

    /* While the echo is awaited no other length ends the wait */
    h = Started(0);
    Take(h, &mark);
    for (length = 0; length <= sizeof(value); ++length) {
        if (length == 2 || length == PACKET) {
            continue;
        }
        memcpy(value, vr_mode, 2);
        Data(h, value, length);
        CHECK(Unchanged(h, &mark));
    }
    /* A packet that starts with 08 00 is a packet */
    memcpy(p, Packet("idle"), PACKET);
    p[0] = 0x08;
    p[1] = 0x00;
    Data(h, p, PACKET);
    CHECK(Writes(h) == 1 && State(h)->phase == SDL_GEARVR_WAIT_ACK && h->published == 0);
    CHECK(h->nsamples == mark.nsamples + 6);
    BH_Destroy(h);

    /* A value from the command characteristic is not read */
    h = Started(0);
    Take(h, &mark);
    BH_Value(h, SDL_GEARVR_COMMAND, vr_mode, 2, false);
    CHECK(Unchanged(h, &mark));
    BH_Value(h, SDL_GEARVR_COMMAND, Packet("trigger"), PACKET, false);
    CHECK(Unchanged(h, &mark));
    BH_Destroy(h);
}

/* Decision: only the 08 00 echo ends the wait, once */
static void TestEchoes(void)
{
    static const uint8_t others[][2] = {
        { 0x00, 0x00 }, { 0x01, 0x00 }, { 0x03, 0x00 }, { 0x04, 0x00 }, { 0x05, 0x00 }, { 0x06, 0x00 },
        { 0x07, 0x00 }, { 0x08, 0x01 }, { 0x00, 0x08 }, { 0x80, 0x00 }, { 0x09, 0x00 }, { 0xFF, 0xFF }
    };
    BH_Harness *h = Started(0);
    Mark mark;
    size_t i;

    Section("Decision: echoes");
    Take(h, &mark);
    for (i = 0; i < sizeof(others) / sizeof(others[0]); ++i) {
        Data(h, others[i], 2);
        CHECK(Unchanged(h, &mark));
    }
    BH_Advance(h, 1580);
    Data(h, vr_mode, 2);
    CHECK(Writes(h) == 2 && IsCommand(h, WriteAt(h, 1), sensor_mode, 1580));
    /* A second echo writes nothing */
    BH_Advance(h, 3000);
    Take(h, &mark);
    Data(h, vr_mode, 2);
    CHECK(Unchanged(h, &mark));
    BH_Destroy(h);

    /* An echo before the start-up is not an acknowledgement */
    h = Create(false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    Take(h, &mark);
    Data(h, vr_mode, 2);
    CHECK(Unchanged(h, &mark));
    BH_SubscribeAll(h);
    CHECK(Writes(h) == 1 && IsCommand(h, WriteAt(h, 0), vr_mode, 0) && State(h)->phase == SDL_GEARVR_WAIT_ACK);
    BH_Destroy(h);
}

/* Decision: the finger is down in touch state 1 only (daydream-catcher
   Reference :157), whatever the coordinates */
static void TestTouchStates(void)
{
    int state;

    Section("Decision: touch states");
    for (state = 0; state < 16; ++state) {
        BH_Harness *h = Published(0);
        const SDL_BLEControls *c = BH_Controls(h);
        uint8_t p[PACKET];

        memcpy(p, Packet("touch_center"), PACKET);
        BH_Advance(h, 1615);
        Data(h, p, PACKET);
        CHECK(c->finger);
        p[54] = (uint8_t)((state << 4) | (p[54] & 0x0F));
        BH_Advance(h, 1630);
        Data(h, p, PACKET);
        if (state == 1) {
            CHECK(c->finger && c->axes[SDL_BLE_AXIS_LEFTX] == -4095 && c->axes[SDL_BLE_AXIS_LEFTY] == 3891);
        } else {
            CHECK(!c->finger && c->finger_x == 0.0f && c->finger_y == 0.0f);
            CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == 0 && c->axes[SDL_BLE_AXIS_LEFTY] == 0);
        }
        BH_Destroy(h);
    }
}

/* Decision: each bit of byte 58 alone. Bit 6 is the idle flag and bit 7 is
   padding (daydream-catcher Reference :161-162), and neither is a control.
   The magnetometer and the temperature change no control either. */
static void TestByte58(void)
{
    static const int buttons[6] = {
        -1, SDL_BLE_BUTTON_GUIDE, SDL_BLE_BUTTON_BACK, SDL_BLE_BUTTON_SOUTH, SDL_BLE_BUTTON_RIGHT_SHOULDER,
        SDL_BLE_BUTTON_LEFT_SHOULDER
    };
    uint8_t p[PACKET];
    int bit;

    Section("Decision: byte 58, magnetometer and temperature");
    for (bit = 0; bit < 8; ++bit) {
        BH_Harness *h = Published(0);
        const SDL_BLEControls *c = BH_Controls(h);
        const int n = h->nsnapshots;

        memcpy(p, Packet("idle"), PACKET);
        p[58] = (uint8_t)(1 << bit);
        BH_Advance(h, 1615);
        Data(h, p, PACKET);
        if (bit == 0) {
            CHECK(c->buttons == 0 && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == 32767);
        } else if (bit < 6) {
            CHECK(c->buttons == (1u << buttons[bit]) && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768);
        } else {
            CHECK(h->nsnapshots == n && AtRest(c));
        }
        BH_Destroy(h);
    }
    {
        BH_Harness *h = Published(0);
        const int n = h->nsnapshots;
        const int samples = h->nsamples;

        memcpy(p, Packet("idle"), PACKET);
        memset(p + 48, 0x7F, 6);
        p[57] = 60;
        BH_Advance(h, 1615);
        Data(h, p, PACKET);
        CHECK(h->nsnapshots == n && AtRest(BH_Controls(h)) && h->nsamples == samples + 6);
        BH_Destroy(h);
    }
}

/* Decision: the stick, the finger position and the battery clamp */
static void TestClamps(void)
{
    static const struct
    {
        int x, y;
        int16_t stick_x, stick_y;
        float finger_x, finger_y;
    } touches[] = {
        { 1023, 1023, 32767, 32767, 1.0f, 1.0f },
        { 321, 0, 32767, -32767, 1.0f, 0.0f },
        { 320, 160, 32767, 0, 1.0f, 0.5f },
        { 315, 315, 31743, 31743, 315.0f / 320.0f, 315.0f / 320.0f },
        { 0, 0, -32767, -32767, 0.0f, 0.0f },
    };
    static const struct
    {
        uint8_t raw;
        int8_t percent;
    } batteries[] = { { 0, 0 }, { 57, 57 }, { 100, 100 }, { 101, 100 }, { 255, 100 } };
    BH_Harness *h = Published(0);
    const SDL_BLEControls *c = BH_Controls(h);
    uint8_t p[PACKET];
    size_t i;

    Section("Decision: clamps");
    for (i = 0; i < sizeof(touches) / sizeof(touches[0]); ++i) {
        memcpy(p, Packet("idle"), PACKET);
        SetTouch(p, 1, touches[i].x, touches[i].y);
        Data(h, p, PACKET);
        CHECK(c->finger);
        CHECK(c->axes[SDL_BLE_AXIS_LEFTX] == touches[i].stick_x && c->axes[SDL_BLE_AXIS_LEFTY] == touches[i].stick_y);
        CHECK(c->finger_x == touches[i].finger_x && c->finger_y == touches[i].finger_y);
        CHECK(c->finger_x >= 0.0f && c->finger_x <= 1.0f && c->finger_y >= 0.0f && c->finger_y <= 1.0f);
    }
    for (i = 0; i < sizeof(batteries) / sizeof(batteries[0]); ++i) {
        memcpy(p, Packet("idle"), PACKET);
        p[59] = batteries[i].raw;
        Data(h, p, PACKET);
        CHECK(c->battery == batteries[i].percent);
    }
    BH_Destroy(h);
}

/* Decision: the joystick appears with the first packet after 01 00 is
   queued, and packets before it are decoded without publishing */
static void TestReady(void)
{
    BH_Harness *h;
    const SDL_BLEControls *c;

    Section("Decision: ready");
    /* A packet while the subscription is out */
    h = Create(false);
    c = BH_Controls(h);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    Data(h, Packet("trigger"), PACKET);
    CHECK(c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == 32767 && h->nsamples == 6);
    CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0 && Writes(h) == 0);
    BH_SubscribeAll(h);
    CHECK(Writes(h) == 1 && h->published == 0);
    BH_WriteAll(h);
    /* A packet while the echo is awaited */
    BH_Advance(h, 500);
    Data(h, Packet("idle"), PACKET);
    CHECK(c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768 && h->published == 0 && !State(h)->base.ready);
    /* The echo is not input */
    BH_Advance(h, 1580);
    Data(h, vr_mode, 2);
    CHECK(Writes(h) == 2 && h->published == 0 && !State(h)->base.ready);
    /* The first packet after 01 00 publishes, once */
    BH_Advance(h, 1600);
    Data(h, Packet("touchpad"), PACKET);
    CHECK(h->published == 1 && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1);
    CHECK(h->actions[h->nactions - 1].action.kind == SDL_BLE_ACTION_PUBLISH && h->actions[h->nactions - 1].now == 1600);
    CHECK(c->finger && c->buttons == (1u << SDL_BLE_BUTTON_SOUTH));
    BH_Advance(h, 1615);
    Data(h, Packet("touch_center"), PACKET);
    CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1);
    BH_Destroy(h);

    /* After the fallback 01 00, the first packet publishes */
    h = Started(0);
    BH_Advance(h, 4000);
    CHECK(Writes(h) == 2 && h->published == 0);
    BH_Advance(h, 4030);
    Data(h, Packet("idle"), PACKET);
    CHECK(h->published == 1 && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1);
    BH_Destroy(h);
}

/* Decision: a gap, as the connection's queue marks it, releases every held
   control before the value after it applies */
static void TestGap(void)
{
    SDL_BLEValue entries[2];
    SDL_BLEValueQueue queue;
    SDL_BLEValue value;
    uint8_t held[PACKET];
    BH_Harness *h;
    const SDL_BLEControls *c;
    int n;

    Section("Decision: gap");
    h = Published(0);
    c = BH_Controls(h);
    BH_Advance(h, 1615);
    Held(held);
    Data(h, held, PACKET);
    CHECK(AllHeld(c));
    n = h->nsnapshots;
    BH_Advance(h, 1630);
    BH_Value(h, SDL_GEARVR_DATA, Packet("volume_up"), PACKET, true);
    CHECK(h->nsnapshots == n + 2);
    CHECK(AtRest(&h->snapshots[n].controls) && h->snapshots[n].controls.battery == 100);
    CHECK(h->snapshots[n + 1].controls.buttons == (1u << SDL_BLE_BUTTON_RIGHT_SHOULDER));
    CHECK(c->buttons == (1u << SDL_BLE_BUTTON_RIGHT_SHOULDER) && !c->finger);
    BH_Destroy(h);

    /* Through a queue of two: the third value is dropped, and the fourth
       carries the gap */
    h = Published(0);
    c = BH_Controls(h);
    SDL_BLE_InitQueue(&queue, entries, 2, SDL_BLE_MAX_VALUE);
    CHECK(SDL_BLE_PushValue(&queue, SDL_GEARVR_DATA, Packet("touchpad"), PACKET, 1615 * MS) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PushValue(&queue, SDL_GEARVR_DATA, Packet("trigger"), PACKET, 1630 * MS) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PushValue(&queue, SDL_GEARVR_DATA, Packet("idle"), PACKET, 1645 * MS) == SDL_BLE_PUSH_FULL);
    while (SDL_BLE_PopValue(&queue, &value)) {
        CHECK(!value.gap);
        h->now = value.time_ns / MS;
        BH_Value(h, value.characteristic, value.data, value.length, value.gap);
    }
    /* The idle packet that would have let the trigger go is lost */
    CHECK(c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == 32767);
    n = h->nsnapshots;
    CHECK(SDL_BLE_PushValue(&queue, SDL_GEARVR_DATA, Packet("volume_down"), PACKET, 1660 * MS) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PopValue(&queue, &value) && value.gap);
    h->now = value.time_ns / MS;
    BH_Value(h, value.characteristic, value.data, value.length, value.gap);
    CHECK(h->nsnapshots == n + 2 && AtRest(&h->snapshots[n].controls));
    CHECK(h->snapshots[n].time_ns == 1660 * MS);
    CHECK(c->buttons == (1u << SDL_BLE_BUTTON_LEFT_SHOULDER) && c->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768);
    BH_Destroy(h);
}

/* Decision: the silence timer. 3 s without a packet after 01 00 went out,
   or after the last packet, is a stream that stopped for good or never
   started (PROTOCOL.md:77): every control is released and the start-up
   runs again from 08 00. A 01 00 echo alone is no stop, and only a packet
   holds the timer off. After two restarts in a row that bring no packet
   the module gives the connection up. */
static void TestSilence(void)
{
    uint8_t held[PACKET];
    uint64_t deadline;
    BH_Harness *h;
    const SDL_BLEControls *c;
    Mark mark;
    int n;

    Section("Decision: silence");
    Held(held);
    CHECK(SDL_GEARVR_SILENCE_MS == 3000 && SDL_GEARVR_RESTARTS == 2);
    /* The timer runs from 01 00, before any packet, and comes before the
       keep-alive */
    h = Streaming(0);
    c = BH_Controls(h);
    CHECK(State(h)->phase == SDL_GEARVR_STREAMING && State(h)->restarts == 0 && h->published == 0);
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 1580 + SDL_GEARVR_SILENCE_MS);
    CHECK(State(h)->deadline == 11580);
    /* The first packet pushes it on, and so does each packet after it */
    BH_Advance(h, 1600);
    Data(h, held, PACKET);
    CHECK(AllHeld(c) && h->published == 1);
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 4600);
    BH_Advance(h, 4599);
    Data(h, held, PACKET);
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 7599);
    /* A 01 00 echo alone neither stops the stream nor holds the timer off */
    BH_Advance(h, 6000);
    Take(h, &mark);
    Data(h, sensor_mode, 2);
    CHECK(Unchanged(h, &mark) && AllHeld(c));
    /* 2999 ms of silence changes nothing. At 3000 ms every control is
       released, the battery kept, and 08 00 goes out, while the joystick
       stays. */
    n = h->nsnapshots;
    BH_Advance(h, 7598);
    CHECK(Writes(h) == 2 && AllHeld(c) && h->nsnapshots == n);
    BH_Advance(h, 7599);
    CHECK(AtRest(c) && c->battery == 100 && h->nsnapshots == n + 1 && h->snapshots[n].time_ns == 7599 * MS);
    CHECK(Writes(h) == 3 && IsCommand(h, WriteAt(h, 2), vr_mode, 7599) && State(h)->restarts == 1);
    CHECK(State(h)->phase == SDL_GEARVR_WAIT_ACK && h->published == 1 && BH_Count(h, SDL_BLE_ACTION_REMOVE) == 0);
    /* A tick while the echo is awaited changes nothing */
    h->now = 7700;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(Writes(h) == 3 && State(h)->phase == SDL_GEARVR_WAIT_ACK && AtRest(c));
    /* While the echo is awaited a packet is decoded, runs no timer and does
       not count as a packet the restart brought */
    BH_Advance(h, 8000);
    Data(h, held, PACKET);
    CHECK(AllHeld(c) && SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 7599 + SDL_GEARVR_ACK_WAIT_MS);
    CHECK(State(h)->restarts == 1);
    /* The echo 1560 ms after 08 00, as caps/vr_idle.jsonl's streaming
       controller echoes it, leads to 01 00 at once */
    BH_Advance(h, 9159);
    Data(h, vr_mode, 2);
    CHECK(Writes(h) == 4 && IsCommand(h, WriteAt(h, 3), sensor_mode, 9159) && State(h)->phase == SDL_GEARVR_STREAMING);
    /* No packet since that 01 00: the silence timer runs from it */
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 9159 + SDL_GEARVR_SILENCE_MS);
    /* The stream resumes, the count starts over, and the keep-alive keeps
       its time */
    BH_Advance(h, 9200);
    Data(h, Packet("idle"), PACKET);
    CHECK(AtRest(c) && SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 12200);
    CHECK(State(h)->restarts == 0);
    Stream(h, 19159, Packet("idle"));
    CHECK(Writes(h) == 5 && IsCommand(h, WriteAt(h, 4), keep_alive, 19159));
    BH_Destroy(h);

    /* A controller that stays stopped: without an echo after the restart,
       01 00 alone at 4 s, and 3 s without a packet after it restarts again.
       The third silence in a row, after two restarts that brought no
       packet, gives the connection up: the joystick goes, the session backs
       off and the module keeps no timer. */
    h = Published(0);
    c = BH_Controls(h);
    BH_Advance(h, 4599);
    CHECK(Writes(h) == 2);
    BH_Advance(h, 4600);
    CHECK(Writes(h) == 3 && IsCommand(h, WriteAt(h, 2), vr_mode, 4600) && State(h)->restarts == 1);
    BH_Advance(h, 8599);
    CHECK(Writes(h) == 3);
    BH_Advance(h, 8600);
    CHECK(Writes(h) == 4 && IsCommand(h, WriteAt(h, 3), sensor_mode, 8600));
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 8600 + SDL_GEARVR_SILENCE_MS);
    BH_Advance(h, 11599);
    CHECK(Writes(h) == 4);
    BH_Advance(h, 11600);
    CHECK(Writes(h) == 5 && IsCommand(h, WriteAt(h, 4), vr_mode, 11600) && State(h)->restarts == 2);
    BH_Advance(h, 15600);
    CHECK(Writes(h) == 6 && IsCommand(h, WriteAt(h, 5), sensor_mode, 15600));
    BH_Advance(h, 18599);
    CHECK(Writes(h) == 6 && h->published == 1 && !SDL_BLESession_Ended(&h->session));
    n = h->nactions;
    BH_Advance(h, 18600);
    CHECK(Writes(h) == 6 && SDL_BLESession_Ended(&h->session) && h->session.backoff && h->published == 0);
    CHECK(h->nactions == n + 3 && h->actions[n].action.kind == SDL_BLE_ACTION_REMOVE &&
          h->actions[n + 1].action.kind == SDL_BLE_ACTION_BACKOFF &&
          h->actions[n + 2].action.kind == SDL_BLE_ACTION_DISCONNECT && h->actions[n + 2].now == 18600);
    CHECK(State(h)->base.failed && State(h)->phase == SDL_GEARVR_IDLE);
    CHECK(!SDL_BLEGearVRModule.GetDeadline(h->state, &deadline) && AtRest(c) && c->battery == 100);
    BH_Destroy(h);

    /* A start-up that never streams, the echo late every time: the same
       three silences, with the joystick never published, well before the
       session's start timeout */
    h = Started(0);
    BH_Advance(h, 3999);
    CHECK(Writes(h) == 1);
    BH_Advance(h, 21000);
    CHECK(Writes(h) == 6 && IsCommand(h, WriteAt(h, 1), sensor_mode, 4000) && IsCommand(h, WriteAt(h, 2), vr_mode, 7000));
    CHECK(IsCommand(h, WriteAt(h, 3), sensor_mode, 11000) && IsCommand(h, WriteAt(h, 4), vr_mode, 14000));
    CHECK(IsCommand(h, WriteAt(h, 5), sensor_mode, 18000));
    CHECK(SDL_BLESession_Ended(&h->session) && h->session.backoff && h->published == 0);
    CHECK(BH_LastKind(h) == SDL_BLE_ACTION_DISCONNECT && h->actions[h->nactions - 1].now == 21000);
    CHECK(21000 < SDL_BLE_START_TIMEOUT_MS && BH_Count(h, SDL_BLE_ACTION_REMOVE) == 0);
    BH_Destroy(h);

    /* With the echo each time, 1580 ms after 08 00, the start-up that never
       streams ends the same way */
    h = Streaming(0);
    BH_Advance(h, 4580);
    CHECK(Writes(h) == 3 && IsCommand(h, WriteAt(h, 2), vr_mode, 4580));
    BH_Advance(h, 6160);
    Data(h, vr_mode, 2);
    CHECK(Writes(h) == 4 && IsCommand(h, WriteAt(h, 3), sensor_mode, 6160));
    BH_Advance(h, 9160);
    CHECK(Writes(h) == 5 && IsCommand(h, WriteAt(h, 4), vr_mode, 9160) && State(h)->restarts == 2);
    BH_Advance(h, 10740);
    Data(h, vr_mode, 2);
    CHECK(Writes(h) == 6 && IsCommand(h, WriteAt(h, 5), sensor_mode, 10740));
    BH_Advance(h, 13739);
    CHECK(!SDL_BLESession_Ended(&h->session));
    BH_Advance(h, 13740);
    CHECK(Writes(h) == 6 && SDL_BLESession_Ended(&h->session) && h->session.backoff && h->published == 0);
    BH_Destroy(h);

    /* A packet after a restart starts the count over: two more restarts
       come before the connection is given up */
    h = Published(0);
    BH_Advance(h, 4600);
    BH_Advance(h, 8600);
    CHECK(Writes(h) == 4 && State(h)->restarts == 1);
    BH_Advance(h, 9000);
    Data(h, Packet("idle"), PACKET);
    CHECK(State(h)->restarts == 0);
    BH_Advance(h, 12000);
    CHECK(Writes(h) == 5 && IsCommand(h, WriteAt(h, 4), vr_mode, 12000) && State(h)->restarts == 1);
    BH_Advance(h, 16000);
    BH_Advance(h, 19000);
    CHECK(Writes(h) == 7 && IsCommand(h, WriteAt(h, 6), vr_mode, 19000) && State(h)->restarts == 2);
    CHECK(!SDL_BLESession_Ended(&h->session) && h->published == 1);
    BH_Advance(h, 26000);
    CHECK(Writes(h) == 8 && SDL_BLESession_Ended(&h->session) && h->session.backoff);
    BH_Destroy(h);

    /* With the keep-alive due too, the stop comes first */
    h = Published(0);
    h->now = 11580;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(Writes(h) == 3 && IsCommand(h, WriteAt(h, 2), vr_mode, 11580) && State(h)->phase == SDL_GEARVR_WAIT_ACK);
    BH_Destroy(h);
}

/* Decision: the timers at their edges */
static void TestTimerEdges(void)
{
    uint8_t packet[PACKET];
    uint64_t due, deadline;
    BH_Harness *h;
    int writes;

    Section("Decision: timers at their edges");
    /* The start-up fallback runs at 4 s, not a millisecond before */
    h = Create(false);
    h->now = 1000;
    BH_Start(h, false);
    BH_WriteAll(h);
    h->now = 4999;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(Writes(h) == 1);
    h->now = 5000;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(Writes(h) == 2 && IsCommand(h, WriteAt(h, 1), sensor_mode, 5000));
    BH_Destroy(h);

    /* A tick one whole period late sends one keep-alive and leaves no spent
       deadline. A packet just before it keeps the stream live, so the
       silence timer, 3 s after that packet, comes first. */
    h = Create(false);
    h->now = 1000;
    BH_Start(h, false);
    BH_WriteAll(h);
    Data(h, vr_mode, sizeof(vr_mode));
    memset(packet, 0, sizeof(packet));
    packet[54] = 0x20;
    packet[59] = 50;
    Data(h, packet, sizeof(packet));
    due = State(h)->deadline;
    CHECK(h->published && due == 11000);
    writes = Writes(h);
    h->now = due + SDL_GEARVR_KEEPALIVE_MS;
    Data(h, packet, sizeof(packet));
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(Writes(h) == writes + 1 && IsCommand(h, WriteAt(h, writes), keep_alive, due + SDL_GEARVR_KEEPALIVE_MS));
    CHECK(State(h)->deadline == due + 2 * SDL_GEARVR_KEEPALIVE_MS);
    CHECK(SDL_BLEGearVRModule.GetDeadline(h->state, &deadline) && deadline == h->now + SDL_GEARVR_SILENCE_MS);
    BH_Destroy(h);

    /* No keep-alive before the start or after the close */
    h = Create(false);
    SDL_BLEGearVRModule.Tick(h->state, 0);
    CHECK(((const SDL_BLEBase *)h->state)->write_count == 0);
    BH_Destroy(h);
    h = Create(false);
    BH_Start(h, false);
    BH_WriteAll(h);
    SDL_BLESession_Close(&h->session, h->now);
    BH_Drain(h);
    BH_WriteAll(h);
    writes = ((const SDL_BLEBase *)h->state)->write_count;
    SDL_BLEGearVRModule.Tick(h->state, 1000000);
    CHECK(((const SDL_BLEBase *)h->state)->write_count == writes);
    BH_Destroy(h);
}

/* The capture replay */

#define LABEL_LENGTH 32

typedef struct Record
{
    uint64_t us; /* t, in microseconds */
    bool data;   /* src "data" */
    bool hid;    /* src "hid_report", a report of the HID service. Neither is "cmd". */
    char label[LABEL_LENGTH]; /* guided.py's step, empty when the line has none */
    size_t length;
    uint8_t bytes[SDL_BLE_MAX_VALUE];
} Record;

typedef struct Capture
{
    int count;
    Record *records;
} Capture;

static char *ReadText(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    char *text;
    long size;

    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    text = (char *)malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    *length = fread(text, 1, (size_t)size, file);
    fclose(file);
    if (*length != (size_t)size) {
        free(text);
        return NULL;
    }
    text[*length] = '\0';
    return text;
}

/* A reader for one JSON object per line, holding strings without escapes
   and numbers, as the captures do */
typedef struct Cursor
{
    const char *p;
    const char *end;
} Cursor;

static void SkipSpace(Cursor *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r')) {
        ++c->p;
    }
}

static bool Expect(Cursor *c, char ch)
{
    SkipSpace(c);
    if (c->p < c->end && *c->p == ch) {
        ++c->p;
        return true;
    }
    return false;
}

static bool String(Cursor *c, const char **text, size_t *length)
{
    if (!Expect(c, '"')) {
        return false;
    }
    *text = c->p;
    while (c->p < c->end && *c->p != '"') {
        if (*c->p == '\\') {
            return false;
        }
        ++c->p;
    }
    if (c->p >= c->end) {
        return false;
    }
    *length = (size_t)(c->p - *text);
    ++c->p;
    return true;
}

/* A number of seconds such as 0.9558, as microseconds */
static bool Seconds(Cursor *c, uint64_t *us)
{
    uint64_t whole = 0, fraction = 0;
    int digits = 0, places = 0;

    SkipSpace(c);
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        whole = whole * 10 + (uint64_t)(*c->p - '0');
        ++c->p;
        ++digits;
    }
    if (c->p < c->end && *c->p == '.') {
        ++c->p;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            if (places < 6) {
                fraction = fraction * 10 + (uint64_t)(*c->p - '0');
                ++places;
            }
            ++c->p;
            ++digits;
        }
    }
    if (digits == 0 || (c->p < c->end && (*c->p == 'e' || *c->p == 'E'))) {
        return false;
    }
    for (; places < 6; ++places) {
        fraction *= 10;
    }
    *us = whole * 1000000 + fraction;
    return true;
}

/* Skips a string, number or literal */
static bool SkipValue(Cursor *c)
{
    const char *start;
    size_t length;

    SkipSpace(c);
    if (c->p < c->end && *c->p == '"') {
        return String(c, &start, &length);
    }
    start = c->p;
    while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != '{' && *c->p != '[') {
        ++c->p;
    }
    return c->p > start && c->p < c->end && (*c->p == ',' || *c->p == '}');
}

static int HexDigit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/* One line such as {"t": 0.9558, "src": "cmd", "hex": "0800"}, keys in any
   order, a label kept, other keys skipped */
static bool ParseRecord(const char *line, const char *end, Record *record)
{
    bool have_t = false, have_src = false, have_hex = false;
    Cursor c;

    c.p = line;
    c.end = end;
    memset(record, 0, sizeof(*record));
    if (!Expect(&c, '{')) {
        return false;
    }
    for (;;) {
        const char *key, *text;
        size_t key_length, length, i;

        if (!String(&c, &key, &key_length) || !Expect(&c, ':')) {
            return false;
        }
        if (key_length == 1 && key[0] == 't') {
            if (!Seconds(&c, &record->us)) {
                return false;
            }
            have_t = true;
        } else if (key_length == 3 && memcmp(key, "src", 3) == 0) {
            if (!String(&c, &text, &length)) {
                return false;
            }
            if (length == 4 && memcmp(text, "data", 4) == 0) {
                record->data = true;
            } else if (length == 10 && memcmp(text, "hid_report", 10) == 0) {
                record->hid = true;
            } else if (!(length == 3 && memcmp(text, "cmd", 3) == 0)) {
                return false;
            }
            have_src = true;
        } else if (key_length == 5 && memcmp(key, "label", 5) == 0) {
            if (!String(&c, &text, &length) || length >= sizeof(record->label)) {
                return false;
            }
            memcpy(record->label, text, length);
            record->label[length] = '\0';
        } else if (key_length == 3 && memcmp(key, "hex", 3) == 0) {
            if (!String(&c, &text, &length) || length % 2 != 0 || length / 2 > sizeof(record->bytes)) {
                return false;
            }
            for (i = 0; i < length; i += 2) {
                const int high = HexDigit(text[i]);
                const int low = HexDigit(text[i + 1]);

                if (high < 0 || low < 0) {
                    return false;
                }
                record->bytes[i / 2] = (uint8_t)((high << 4) | low);
            }
            record->length = length / 2;
            have_hex = true;
        } else if (!SkipValue(&c)) {
            return false;
        }
        if (Expect(&c, ',')) {
            continue;
        }
        if (Expect(&c, '}')) {
            break;
        }
        return false;
    }
    SkipSpace(&c);
    return c.p == c.end && have_t && have_src && have_hex;
}

/* False when the file is missing. A line that does not parse fails a check. */
static bool LoadCapture(const char *folder, const char *name, Capture *capture)
{
    char path[1024];
    char *text;
    size_t length = 0, i, start = 0, lines = 1;
    int line = 0, bad = 0, first_bad = 0;

    capture->count = 0;
    capture->records = NULL;
    if (snprintf(path, sizeof(path), "%s/caps/%s", folder, name) >= (int)sizeof(path)) {
        return false;
    }
    text = ReadText(path, &length);
    if (!text) {
        return false;
    }
    /* One record for each line at most */
    for (i = 0; i < length; ++i) {
        if (text[i] == '\n') {
            ++lines;
        }
    }
    capture->records = (Record *)calloc(lines, sizeof(Record));
    if (!capture->records) {
        printf("out of memory\n");
        exit(2);
    }
    for (i = 0; i <= length; ++i) {
        if (i == length || text[i] == '\n') {
            const char *first = text + start;
            const char *end = text + i;
            Cursor c;

            ++line;
            c.p = first;
            c.end = end;
            SkipSpace(&c);
            if (c.p < c.end) {
                if (ParseRecord(first, end, &capture->records[capture->count])) {
                    ++capture->count;
                } else if (bad++ == 0) {
                    first_bad = line;
                }
            }
            start = i + 1;
        }
    }
    free(text);
    BH_CHECK(bad == 0, "%s: %d lines do not parse, the first at line %d", name, bad, first_bad);
    return true;
}

static void FreeCapture(Capture *capture)
{
    free(capture->records);
    capture->records = NULL;
    capture->count = 0;
}

/* The first command record of a capture, the host's 08 00 */
static const Record *FirstCommand(const Capture *capture)
{
    int i;

    for (i = 0; i < capture->count; ++i) {
        if (!capture->records[i].data) {
            return &capture->records[i];
        }
    }
    return NULL;
}

/* seq.jsonl: 08 00, its echo, 01 00 and 541 packets. The module starts when
   the capture's host wrote 08 00 and replays the data records on their
   times. */
static void ReplaySeq(const Capture *capture)
{
    const Record *start = FirstCommand(capture);
    int i, npackets = 0, echoes = 0, commands = 0, in_order = 1, stamped = 1, decoded = 1;
    uint64_t start_ms, echo_ms = 0, first_ms = 0, last_sensor_ns = 0;
    BH_Harness *h;

    Section("Capture: seq.jsonl");
    CHECK(capture->count == 544);
    CHECK(start && start->length == 2 && memcmp(start->bytes, vr_mode, 2) == 0);
    if (!start) {
        return;
    }
    /* The part's times: 08 00 at 0.956 s, the echo at 2.536 s and the host's
       01 00 at 3.991 s */
    CHECK(start->us == 955800);
    start_ms = start->us / 1000;
    h = Create(false);
    h->now = start_ms;
    BH_Start(h, false);
    CHECK(Writes(h) == 1 && IsCommand(h, WriteAt(h, 0), vr_mode, start_ms));
    BH_WriteAll(h);
    for (i = 0; i < capture->count; ++i) {
        const Record *r = &capture->records[i];
        const uint64_t ms = r->us / 1000;
        const int nactions = h->nactions;
        const int nsamples = h->nsamples;

        if (!r->data) {
            /* The capture host's own writes. The module makes its own. */
            ++commands;
            if (commands == 2) {
                CHECK(r->us == 3990700 && r->length == 2 && memcmp(r->bytes, sensor_mode, 2) == 0);
            }
            continue;
        }
        BH_Advance(h, ms);
        BH_Value(h, SDL_GEARVR_DATA, r->bytes, r->length, false);
        if (r->length == 2) {
            ++echoes;
            echo_ms = ms;
            CHECK(r->us == 2536200 && memcmp(r->bytes, vr_mode, 2) == 0);
            /* 01 00 at once, as the next action */
            CHECK(h->nactions == nactions + 1 && IsCommand(h, nactions, sensor_mode, ms));
        } else if (r->length == PACKET && h->nsamples == nsamples + 6) {
            Raw raw;
            int s;

            ++npackets;
            Decode(r->bytes, &raw);
            if (npackets == 1) {
                first_ms = ms;
                CHECK(h->nactions == nactions + 1 && BH_LastKind(h) == SDL_BLE_ACTION_PUBLISH);
            }
            for (s = 0; s < 3; ++s) {
                const uint64_t sensor_ns = h->samples[nsamples + 2 * s].sensor_ns;

                /* The clock never wraps in this capture, so the sensor
                   timestamp is the device time */
                if (sensor_ns != (uint64_t)raw.time[s] * 1000) {
                    stamped = 0;
                }
                if (sensor_ns <= last_sensor_ns) {
                    in_order = 0;
                }
                last_sensor_ns = sensor_ns;
                if (!SampleIs(h, nsamples + 2 * s, raw.accel[s], raw.gyro[s], sensor_ns, ms * MS)) {
                    decoded = 0;
                }
            }
        }
        BH_WriteAll(h);
    }
    CHECK(commands == 2 && echoes == 1 && npackets == 541);
    CHECK(h->nsamples == 541 * 6);
    CHECK(stamped && in_order && decoded);
    /* 08 00 once, then 01 00 once right after the echo */
    CHECK(Writes(h) == 2 && IsCommand(h, WriteAt(h, 1), sensor_mode, echo_ms) && echo_ms == 2536);
    CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 1 && first_ms == 4008);
    /* Every packet is idle: one change, the battery, and no control held */
    CHECK(h->nsnapshots == 1 && AtRest(BH_Controls(h)) && BH_Controls(h)->battery == 100);
    /* The first keep-alive comes 10 s after 01 00, after the capture ends */
    BH_Advance(h, echo_ms + 9999);
    CHECK(Writes(h) == 2);
    BH_Advance(h, echo_ms + 10000);
    CHECK(Writes(h) == 3 && IsCommand(h, WriteAt(h, 2), keep_alive, echo_ms + 10000));
    BH_Destroy(h);
}

/* vr_first.jsonl: its host wrote 01 00 right after 08 00, which the module
   never does. Only the data records are fed: the packets that stream, then
   the controller's 01 00 echo. */
static void ReplayVRFirst(const Capture *capture)
{
    const Record *start = FirstCommand(capture);
    int i, npackets = 0, echoes = 0, commands = 0, stamped = 1, decoded = 1;
    uint64_t start_ms;
    BH_Harness *h;

    Section("Capture: vr_first.jsonl");
    CHECK(capture->count == 106);
    CHECK(start && start->length == 2 && memcmp(start->bytes, vr_mode, 2) == 0);
    if (!start) {
        return;
    }
    start_ms = start->us / 1000;
    CHECK(start_ms == 935);
    h = Create(false);
    h->now = start_ms;
    BH_Start(h, false);
    CHECK(Writes(h) == 1 && IsCommand(h, WriteAt(h, 0), vr_mode, start_ms));
    BH_WriteAll(h);
    for (i = 0; i < capture->count; ++i) {
        const Record *r = &capture->records[i];
        const uint64_t ms = r->us / 1000;
        const int nactions = h->nactions;
        const int nsamples = h->nsamples;

        if (!r->data) {
            ++commands;
            if (commands == 2) {
                /* The host's early 01 00, before any data */
                CHECK(r->us == 990800 && memcmp(r->bytes, sensor_mode, 2) == 0 && npackets == 0);
            }
            continue;
        }
        BH_Advance(h, ms);
        BH_Value(h, SDL_GEARVR_DATA, r->bytes, r->length, false);
        if (r->length == 2) {
            /* The 01 00 echo is no acknowledgement: nothing follows it */
            ++echoes;
            CHECK(r->us == 2498300 && memcmp(r->bytes, sensor_mode, 2) == 0);
            CHECK(h->nactions == nactions && h->nsamples == nsamples);
            CHECK(State(h)->phase == SDL_GEARVR_WAIT_ACK);
        } else if (r->length == PACKET && h->nsamples == nsamples + 6) {
            Raw raw;
            int s;

            ++npackets;
            Decode(r->bytes, &raw);
            for (s = 0; s < 3; ++s) {
                const uint64_t sensor_ns = h->samples[nsamples + 2 * s].sensor_ns;

                if (sensor_ns != (uint64_t)raw.time[s] * 1000) {
                    stamped = 0;
                }
                if (!SampleIs(h, nsamples + 2 * s, raw.accel[s], raw.gyro[s], sensor_ns, ms * MS)) {
                    decoded = 0;
                }
            }
            /* Decoded, but not published before 01 00 */
            CHECK(h->nactions == nactions);
        }
        BH_WriteAll(h);
    }
    CHECK(commands == 2 && echoes == 1 && npackets == 103);
    CHECK(stamped && decoded);
    CHECK(Writes(h) == 1 && BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0 && h->published == 0);
    /* 01 00 on the module's own, 4000 ms after its 08 00 */
    BH_Advance(h, start_ms + 3999);
    CHECK(Writes(h) == 1);
    BH_Advance(h, start_ms + 4000);
    CHECK(Writes(h) == 2 && IsCommand(h, WriteAt(h, 1), sensor_mode, start_ms + 4000));
    CHECK(BH_Count(h, SDL_BLE_ACTION_PUBLISH) == 0);
    BH_Destroy(h);
}

/* The delay of the 08 00 echo from a streaming controller: vr_idle.jsonl's
   host wrote 01 00 at 1.0710 s and received three packets, then wrote 08 00
   at 1.1342 s, which stopped the stream (PROTOCOL.md:78), and received the
   2-byte echo at 2.6942 s. Whether a controller that stopped on its own
   echoes 08 00 is untested. 0 when the capture is not that. */
static uint64_t IdleEchoDelay(const Capture *idle)
{
    const Record *command = NULL;
    int i, streamed = 0;

    CHECK(idle->count == 6);
    /* The host's 01 00 first, at 1.0710 s, so the controller streamed */
    CHECK(idle->count > 0 && !idle->records[0].data && idle->records[0].length == 2 &&
          memcmp(idle->records[0].bytes, sensor_mode, 2) == 0 && idle->records[0].us == 1071000);
    for (i = 0; i < idle->count; ++i) {
        const Record *r = &idle->records[i];

        if (!r->data && r->length == 2 && memcmp(r->bytes, vr_mode, 2) == 0) {
            CHECK(r->us == 1134200);
            command = r;
        } else if (r->data && r->length == PACKET) {
            CHECK(!command);
            ++streamed;
        } else if (r->data && command) {
            CHECK(r->length == 2 && memcmp(r->bytes, vr_mode, 2) == 0 && streamed == 3 && r->us == 2694200);
            return (r->us - command->us) / 1000;
        }
    }
    return 0;
}

/* A capture of a stream that stops, vr_first.jsonl or vr_ka.jsonl, as the
   module meets it after its own 01 00: the fallback at 4 s while the echo
   is late, then a controller that streams, echoes 01 00 and stops, as its
   host's early 01 00 made it do (PROTOCOL.md:77). The capture's times move
   so that its host's 01 00 falls on the module's. 3 s after the last packet
   every control is released and 08 00 goes out again, and an echo as late
   as the one vr_idle.jsonl's streaming controller sent leads to 01 00.
   Whether a controller that stopped on its own echoes 08 00 is untested. */
static void ReplayStop(const Capture *capture, uint64_t echo_delay, int npackets_expected, uint64_t echo_us)
{
    const uint64_t start_ms = 1000, stream_ms = start_ms + SDL_GEARVR_ACK_WAIT_MS;
    const Record *host_sensor = NULL;
    int i, commands = 0, npackets = 0, echoes = 0, restarts = 0;
    uint64_t last_ms = 0;
    BH_Harness *h;

    for (i = 0; i < capture->count; ++i) {
        if (!capture->records[i].data && ++commands == 2) {
            host_sensor = &capture->records[i];
        }
    }
    CHECK(commands == 2 && host_sensor && host_sensor->length == 2 && memcmp(host_sensor->bytes, sensor_mode, 2) == 0);
    CHECK(echo_delay == 1560);
    if (!host_sensor) {
        return;
    }
    h = Create(false);
    h->now = start_ms;
    BH_Start(h, false);
    BH_WriteAll(h);
    BH_Advance(h, stream_ms);
    CHECK(Writes(h) == 2 && IsCommand(h, WriteAt(h, 1), sensor_mode, stream_ms));
    for (i = 0; i < capture->count; ++i) {
        const Record *r = &capture->records[i];
        const int nactions = h->nactions;
        uint64_t ms;

        if (!r->data) {
            continue;
        }
        ms = stream_ms + (r->us - host_sensor->us) / 1000;
        BH_Advance(h, ms);
        BH_Value(h, SDL_GEARVR_DATA, r->bytes, r->length, false);
        BH_WriteAll(h);
        if (Writes(h) != 2) {
            ++restarts;
        }
        if (r->length == 2) {
            /* The 01 00 echo, the stream's last word, stops nothing by itself */
            ++echoes;
            CHECK(r->us == echo_us && memcmp(r->bytes, sensor_mode, 2) == 0);
            CHECK(h->nactions == nactions && State(h)->phase == SDL_GEARVR_STREAMING && State(h)->restarts == 0 &&
                  State(h)->base.ready);
        } else if (r->length == PACKET) {
            ++npackets;
            last_ms = ms;
            CHECK(npackets > 1 || (h->nactions == nactions + 1 && BH_LastKind(h) == SDL_BLE_ACTION_PUBLISH));
        }
    }
    CHECK(npackets == npackets_expected && echoes == 1 && restarts == 0 && h->published == 1);
    CHECK(AtRest(BH_Controls(h)) && BH_Controls(h)->battery == 100);
    /* Nothing more: 3 s after the last packet the stream counts as stopped,
       before the keep-alive 10 s after the module's 01 00 */
    CHECK(last_ms + 3000 < stream_ms + SDL_GEARVR_KEEPALIVE_MS);
    BH_Advance(h, last_ms + 2999);
    CHECK(Writes(h) == 2);
    BH_Advance(h, last_ms + 3000);
    CHECK(Writes(h) == 3 && IsCommand(h, WriteAt(h, 2), vr_mode, last_ms + 3000));
    CHECK(State(h)->phase == SDL_GEARVR_WAIT_ACK && h->published == 1 && AtRest(BH_Controls(h)));
    /* The echo as late as vr_idle.jsonl's, then 01 00 at once, and the
       silence timer runs from it */
    BH_Advance(h, last_ms + 3000 + echo_delay);
    Data(h, vr_mode, 2);
    CHECK(Writes(h) == 4 && IsCommand(h, WriteAt(h, 3), sensor_mode, last_ms + 3000 + echo_delay));
    CHECK(State(h)->restarts == 1 && State(h)->silence == last_ms + 3000 + echo_delay + SDL_GEARVR_SILENCE_MS);
    BH_Destroy(h);
}

/* guided.jsonl, the recording guided.py prompted step by step, each step a
   prompt and a recording (guided.py:31-47, :73-81) */
#define GUIDED_REST  0 /* The rest_flat recording, the controller still on the table */
#define GUIDED_UP    1 /* The point_up step */
#define GUIDED_LEFT  2 /* The roll_left step */
#define GUIDED_STEPS 3

typedef struct Guided
{
    int step;       /* The step the next samples belong to, or -1 */
    bool have_last; /* A gyroscope sample of this step came before */
    uint64_t last_ns;
    bool later;     /* Each gyroscope sample's time follows the one before */
    int all_gyros;
    uint64_t previous_ns;
    int samples;
    int gyros[GUIDED_STEPS];
    double rates[GUIDED_STEPS][3];  /* The rates summed, radians per second */
    double angles[GUIDED_STEPS][3]; /* Each rate times the time since the step's last sample, radians */
    double seconds[GUIDED_STEPS];   /* The time those cover */
    int accels[GUIDED_STEPS];
    float first_accel[GUIDED_STEPS][3];
    float last_accel[GUIDED_STEPS][3];
} Guided;

static int GuidedStep(const char *label)
{
    if (strcmp(label, "rest_flat") == 0) {
        return GUIDED_REST;
    }
    if (strcmp(label, "point_up:prompt") == 0 || strcmp(label, "point_up") == 0) {
        return GUIDED_UP;
    }
    if (strcmp(label, "roll_left:prompt") == 0 || strcmp(label, "roll_left") == 0) {
        return GUIDED_LEFT;
    }
    return -1;
}

/* The module's samples in SDL's axes and units, on the device's clock */
static void GuidedSensor(void *userdata, int sensor, uint64_t time_ns, uint64_t sensor_ns, const float *data)
{
    Guided *g = (Guided *)userdata;
    int k;

    (void)time_ns;
    ++g->samples;
    if (sensor == SDL_BLE_SENSOR_GYRO) {
        if (g->all_gyros++ > 0 && sensor_ns <= g->previous_ns) {
            g->later = false;
        }
        g->previous_ns = sensor_ns;
    }
    if (g->step < 0) {
        return;
    }
    if (sensor == SDL_BLE_SENSOR_ACCEL) {
        if (g->accels[g->step]++ == 0) {
            memcpy(g->first_accel[g->step], data, sizeof(g->first_accel[0]));
        }
        memcpy(g->last_accel[g->step], data, sizeof(g->last_accel[0]));
        return;
    }
    for (k = 0; k < 3; ++k) {
        g->rates[g->step][k] += data[k];
    }
    if (g->have_last) {
        const double dt = (double)(sensor_ns - g->last_ns) / 1e9;

        for (k = 0; k < 3; ++k) {
            g->angles[g->step][k] += data[k] * dt;
        }
        g->seconds[g->step] += dt;
    }
    ++g->gyros[g->step];
    g->have_last = true;
    g->last_ns = sensor_ns;
}

/* A step's rotation about SDL's axes in degrees, less the zero-rate bias,
   the rest_flat recording's mean rate */
static void GuidedTurn(const Guided *g, int step, double *degrees)
{
    int k;

    for (k = 0; k < 3; ++k) {
        const double bias = g->rates[GUIDED_REST][k] / g->gyros[GUIDED_REST];

        degrees[k] = (g->angles[step][k] - bias * g->seconds[step]) * 180.0 / 3.14159265358979323846;
    }
}

/* The motions of guided.jsonl, every packet through the module, and the
   gyroscope integrated over two steps in SDL's axes. SDL's gyroscope reads
   a positive rate for a turn that an observer on the positive side of the
   axis sees as counter-clockwise (SDL_sensor.h:110-113), with X to the
   right, Y up and Z toward the player (:119-124). The controller made the
   full counter-clockwise circle on the table that rotate_yaw asks for
   (guided.py:42-43) after that recording, while the point_up prompt
   played, and then raised its far end to the ceiling, so the point_up step
   holds both: the turn a positive yaw on SDL Y and the raised nose a
   positive pitch on SDL X. The roll_left step lowers the far end to level,
   a negative pitch, then rolls the controller onto its left side, the
   touchpad facing left (guided.py:45-46), a positive roll on SDL Z. In
   device axes the capture gives +350.8 degrees about Z and +80.9 about X
   in the point_up step and -66.6 about Y in the roll_left step, which the
   remap makes +350.8 on SDL Y, +80.9 on SDL X and +66.6 on SDL Z. The
   gravity at each step's ends shows the poses. */
static void ReplayGuided(const Capture *capture)
{
    SDL_GearVRState state;
    SDL_BLEModuleContext context;
    SDL_BLESink sink;
    Guided g;
    double bias[3], up[3], left[3];
    int i, k, npackets = 0, echoes = 0;

    Section("Capture: guided.jsonl, the motions");
    CHECK(capture->count == 15046);
    memset(&g, 0, sizeof(g));
    g.step = -1;
    g.later = true;
    memset(&sink, 0, sizeof(sink));
    sink.userdata = &g;
    sink.sensor = GuidedSensor;
    memset(&context, 0, sizeof(context));
    SDL_BLEGearVRModule.Reset(&state, &sink, &context);
    SDL_BLEGearVRModule.Start(&state, 0);
    for (i = 0; i < capture->count; ++i) {
        const Record *r = &capture->records[i];
        const int step = GuidedStep(r->label);

        if (!r->data) {
            continue;
        }
        if (step != g.step) {
            g.step = step;
            g.have_last = false;
        }
        SDL_BLEGearVRModule.Value(&state, SDL_GEARVR_DATA, r->bytes, r->length, r->us * 1000);
        if (r->length == PACKET) {
            ++npackets;
        } else {
            ++echoes;
        }
    }
    /* The one 08 00 echo, then 15041 packets of three samples */
    CHECK(npackets == 15041 && echoes == 1 && g.samples == 6 * npackets && g.all_gyros == 3 * npackets && g.later);
    CHECK(state.phase == SDL_GEARVR_STREAMING && state.base.ready);
    CHECK(g.gyros[GUIDED_REST] == 1728 && g.gyros[GUIDED_UP] == 2655 && g.gyros[GUIDED_LEFT] == 3297);
    for (k = 0; k < GUIDED_STEPS; ++k) {
        BH_CHECK(g.accels[k] == g.gyros[k] && g.seconds[k] > 5.0, "step %d: %d samples over %.2f s", k, g.gyros[k],
                 g.seconds[k]);
    }
    if (g.gyros[GUIDED_REST] == 0) {
        return;
    }

    /* The bias in SDL's axes, degrees per second: PROTOCOL.md:113 gives
       about (-1.1, -4.5, +1.8) in device axes, so (-1.1, +1.8, +4.5) */
    for (k = 0; k < 3; ++k) {
        bias[k] = g.rates[GUIDED_REST][k] / g.gyros[GUIDED_REST] * 180.0 / 3.14159265358979323846;
    }
    CHECK(fabs(bias[0] + 1.1) < 0.25 && fabs(bias[1] - 1.8) < 0.25 && fabs(bias[2] - 4.5) < 0.25);

    GuidedTurn(&g, GUIDED_UP, up);
    GuidedTurn(&g, GUIDED_LEFT, left);
    printf("guided.jsonl, SDL axes in degrees: bias (%.3f, %.3f, %.3f) per second, point_up (%.1f, %.1f, %.1f), "
           "roll_left (%.1f, %.1f, %.1f)\n",
           bias[0], bias[1], bias[2], up[0], up[1], up[2], left[0], left[1], left[2]);
    /* One full circle counter-clockwise on the table: a positive yaw near
       360, 350.8 from the capture */
    CHECK(up[1] > 330.0 && up[1] < 370.0 && fabs(up[1] - 350.8) < 0.5);
    /* The far end raised from the table to the ceiling: a positive pitch
       near 90, 80.9 from the capture */
    CHECK(up[0] > 60.0 && up[0] < 100.0 && fabs(up[0] - 80.9) < 0.5);
    /* The far end lowered to level: a negative pitch. Then the roll onto
       the left side: a positive roll, 66.6 from the capture. */
    CHECK(left[0] < -45.0 && left[0] > -100.0);
    CHECK(left[2] > 45.0 && left[2] < 100.0 && fabs(left[2] - 66.6) < 0.5);

    /* The poses, 1 g on SDL Y when flat, as SDL_sensor.h:69-72 has it: flat
       at the start of point_up, the far end, SDL -Z, up at its end and at
       the start of roll_left, and the touchpad facing left, SDL +X up, at
       the end of roll_left */
    CHECK(g.first_accel[GUIDED_REST][1] / STANDARD_GRAVITY > 0.9);
    CHECK(g.first_accel[GUIDED_UP][1] / STANDARD_GRAVITY > 0.9);
    CHECK(g.last_accel[GUIDED_UP][2] / STANDARD_GRAVITY < -0.9);
    CHECK(g.first_accel[GUIDED_LEFT][2] / STANDARD_GRAVITY < -0.9);
    CHECK(g.last_accel[GUIDED_LEFT][0] / STANDARD_GRAVITY > 0.9);
}

static bool Parses(const char *line, Record *record)
{
    return ParseRecord(line, line + strlen(line), record);
}

/* The reader itself, on lines written here */
static void TestReader(void)
{
    static const char *const bad[] = {
        "{\"t\": 0.5, \"src\": \"x\", \"hex\": \"00\"}",
        "{\"t\": 0.5, \"src\": \"cmd\", \"hex\": \"0\"}",
        "{\"t\": 0.5, \"src\": \"cmd\", \"hex\": \"zz\"}",
        "{\"t\": 0.5, \"src\": \"cmd\"}",
        "{\"t\": 1e3, \"src\": \"cmd\", \"hex\": \"00\"}",
        "{\"t\": -1, \"src\": \"cmd\", \"hex\": \"00\"}",
        "{\"t\": 0.5, \"src\": \"cmd\", \"hex\": \"00\"} x",
        "{\"t\": 0.5, \"src\": \"cmd\", \"hex\": \"00\"",
        "{\"t\": 0.5, \"src\": \"c\\\"md\", \"hex\": \"00\"}",
        "{\"t\": 0.5, \"label\": 7, \"src\": \"data\", \"hex\": \"00\"}",
        "{\"t\": 0.5, \"label\": \"0123456789012345678901234567890x\", \"src\": \"data\", \"hex\": \"00\"}",
        "{\"t\": 0.5, \"src\": \"hid_repor\", \"hex\": \"00\"}",
        "",
    };
    Record record;
    size_t i;

    Section("Reader: JSON lines");
    CHECK(Parses("{\"t\": 0.9558, \"src\": \"cmd\", \"hex\": \"0800\"}", &record));
    CHECK(record.us == 955800 && !record.data && !record.hid && record.length == 2 && record.bytes[0] == 0x08 &&
          record.bytes[1] == 0x00 && record.label[0] == '\0');
    /* guided.jsonl's lines: a label, and reports of the HID service */
    CHECK(Parses("{\"t\": 14.2581, \"label\": \"hid_buttons\", \"src\": \"hid_report\", \"hex\": \"ea00\"}", &record));
    CHECK(record.us == 14258100 && record.hid && !record.data && strcmp(record.label, "hid_buttons") == 0 &&
          record.length == 2 && record.bytes[0] == 0xEA);
    CHECK(Parses("{\"t\": 1, \"label\": \"0123456789012345678901234567890\", \"src\": \"data\", \"hex\": \"00\"}", &record));
    CHECK(record.data && !record.hid && strlen(record.label) == LABEL_LENGTH - 1);
    CHECK(Parses("{\"hex\":\"08aB\",\"src\":\"data\",\"t\":2.5362}", &record));
    CHECK(record.us == 2536200 && record.data && record.length == 2 && record.bytes[0] == 0x08 && record.bytes[1] == 0xAB);
    CHECK(Parses("  { \"t\" : 12 , \"label\": \"point_up:prompt\", \"n\": 3, \"src\": \"data\", \"hex\": \"\" }\r", &record));
    CHECK(record.us == 12000000 && record.data && record.length == 0);
    CHECK(Parses("{\"t\": 1.23456789, \"src\": \"data\", \"hex\": \"00\"}", &record) && record.us == 1234567);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        BH_CHECK(!Parses(bad[i], &record), "line %d of the bad lines parses", (int)i);
    }
}

/* False when a capture is missing */
static bool TestCaptures(const char *folder)
{
    Capture seq, vr_first, vr_ka, vr_idle, guided;
    uint64_t delay;
    bool loaded;

    Section("Capture: reading the JSON lines");
    if (!LoadCapture(folder, "seq.jsonl", &seq)) {
        return false;
    }
    loaded = LoadCapture(folder, "vr_first.jsonl", &vr_first);
    loaded = LoadCapture(folder, "vr_ka.jsonl", &vr_ka) && loaded;
    loaded = LoadCapture(folder, "vr_idle.jsonl", &vr_idle) && loaded;
    loaded = LoadCapture(folder, "guided.jsonl", &guided) && loaded;
    if (loaded) {
        ReplaySeq(&seq);
        ReplayVRFirst(&vr_first);
        Section("Capture: vr_idle.jsonl, the 08 00 echo of a streaming controller");
        delay = IdleEchoDelay(&vr_idle);
        Section("Capture: vr_first.jsonl after the fallback 01 00");
        CHECK(vr_first.count == 106);
        ReplayStop(&vr_first, delay, 103, 2498300);
        Section("Capture: vr_ka.jsonl after the fallback 01 00");
        CHECK(vr_ka.count == 107);
        ReplayStop(&vr_ka, delay, 104, 2571500);
        ReplayGuided(&guided);
    }
    FreeCapture(&seq);
    FreeCapture(&vr_first);
    FreeCapture(&vr_ka);
    FreeCapture(&vr_idle);
    FreeCapture(&guided);
    return loaded;
}

int main(int argc, char *argv[])
{
    bool captures;

    Section("Fixtures: tests_fixtures.json");
    LoadFixtures();
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
    Test12();
    TestLengths();
    TestEchoes();
    TestTouchStates();
    TestByte58();
    TestClamps();
    TestReady();
    TestGap();
    TestSilence();
    TestTimerEdges();
    TestReader();
    captures = argc >= 2 && TestCaptures(argv[1]);
    Section(NULL);
    BH_CHECK(captures, "the captures of the gearvr-controller clone could not be read (folder: %s)",
             argc >= 2 ? argv[1] : "none given");
    return BH_Report("testblegearvr");
}
