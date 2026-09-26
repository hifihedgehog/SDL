/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/ble/SDL_ble_session.c, the matcher, the host,
   the value queue and the connection session of the Bluetooth LE GATT
   joystick driver of hifihedgehog/SDL#33 Part 12. Tests 1 to 9 follow the
   part's session tests. The other blocks check the device table against the
   UUIDs the part gives, the contract helpers, the host, the queue and the
   session's flows with the real families. A probe module stands in where no
   real module shows a call. */

#include "testbleharness.h"
#include "SDL_ble_pokeball_proto.h"
#include "SDL_ble_daydream_proto.h"
#include "SDL_ble_gearvr_proto.h"
#include "SDL_ble_oculusgo_proto.h"
#include "SDL_ble_ghlios_proto.h"
#include "SDL_ble_zwift_proto.h"
#include "SDL_ble_myo_proto.h"

#define CHECK(condition) BH_CHECK(condition, "%s", #condition)
#define COUNT(array)     ((int)(sizeof(array) / sizeof((array)[0])))

/* Ends a harness. No session may drop an action to a full queue, since at
   most five actions wait (SDL_BLESession_NextAction). */
static void DestroyAt(BH_Harness *h, int line)
{
    BH_CHECK(h->session.actions_dropped == 0, "line %d: the session dropped %u actions", line,
             (unsigned int)h->session.actions_dropped);
    BH_Destroy(h);
}

#define DESTROY(h) DestroyAt(h, __LINE__)

/* Action kinds, short for the logs below */
#define CONNECT     SDL_BLE_ACTION_CONNECT
#define PAIR        SDL_BLE_ACTION_PAIR
#define REMOVE_BOND SDL_BLE_ACTION_REMOVE_BOND
#define DISCOVER    SDL_BLE_ACTION_DISCOVER
#define SUBSCRIBE   SDL_BLE_ACTION_SUBSCRIBE
#define READ        SDL_BLE_ACTION_READ
#define WRITE       SDL_BLE_ACTION_WRITE
#define PUBLISH     SDL_BLE_ACTION_PUBLISH
#define REMOVE      SDL_BLE_ACTION_REMOVE
#define BACKOFF     SDL_BLE_ACTION_BACKOFF
#define DISCONNECT  SDL_BLE_ACTION_DISCONNECT
#define NO_ANSWER   (-1)

/* The action log as words */

static const char *CCCDName(uint8_t cccd)
{
    switch (cccd) {
    case SDL_BLE_CCCD_NONE:
        return "none";
    case SDL_BLE_CCCD_NOTIFY:
        return "notify";
    case SDL_BLE_CCCD_INDICATE:
        return "indicate";
    default:
        return "?";
    }
}

/* One word per action from index from on: sub<c>=<value>, read<c>,
   write<c>:<hex> with +r for a response and +c for a closing write, and the
   plain names of the others */
static void Render(const BH_Harness *h, int from, char *out, size_t size)
{
    size_t used = 0;
    int i, b;

    out[0] = '\0';
    for (i = (from < 0) ? 0 : from; i < h->nactions; ++i) {
        const SDL_BLEAction *action = &h->actions[i].action;
        char word[256];
        size_t length;

        switch (action->kind) {
        case CONNECT:
            snprintf(word, sizeof(word), "connect");
            break;
        case PAIR:
            snprintf(word, sizeof(word), "pair");
            break;
        case REMOVE_BOND:
            snprintf(word, sizeof(word), "unbond");
            break;
        case DISCOVER:
            snprintf(word, sizeof(word), "discover");
            break;
        case SUBSCRIBE:
            snprintf(word, sizeof(word), "sub%d=%s", (int)action->characteristic, CCCDName(action->cccd));
            break;
        case READ:
            snprintf(word, sizeof(word), "read%d", (int)action->characteristic);
            break;
        case WRITE:
            length = (size_t)snprintf(word, sizeof(word), "write%d:", (int)action->characteristic);
            for (b = 0; b < (int)action->length && length + 3 < sizeof(word); ++b) {
                length += (size_t)snprintf(&word[length], sizeof(word) - length, "%02x", (unsigned int)action->data[b]);
            }
            snprintf(&word[length], sizeof(word) - length, "%s%s", action->response ? "+r" : "", action->closing ? "+c" : "");
            break;
        case PUBLISH:
            snprintf(word, sizeof(word), "publish");
            break;
        case REMOVE:
            snprintf(word, sizeof(word), "remove");
            break;
        case BACKOFF:
            snprintf(word, sizeof(word), "backoff");
            break;
        case DISCONNECT:
            snprintf(word, sizeof(word), "disconnect");
            break;
        default:
            snprintf(word, sizeof(word), "kind%d", (int)action->kind);
            break;
        }
        length = strlen(word);
        if (used + length + 2 > size) {
            break;
        }
        if (used) {
            out[used++] = ' ';
        }
        memcpy(&out[used], word, length + 1);
        used += length;
    }
}

static void ExpectLog(const BH_Harness *h, int from, const char *expected, int line)
{
    char text[8192];

    Render(h, from, text, sizeof(text));
    BH_CHECK(strcmp(text, expected) == 0, "line %d, the actions\n  are      \"%s\"\n  expected \"%s\"", line, text, expected);
}

#define EXPECT_LOG(h, from, expected) ExpectLog(h, from, expected, __LINE__)

/* The clock when the action at index was taken */
static uint64_t TimeAt(const BH_Harness *h, int index)
{
    return (index >= 0 && index < h->nactions) ? h->actions[index].now : (uint64_t)-1;
}

static uint8_t Phase(const BH_Harness *h)
{
    return h->session.phase;
}

/* Values and answers */

/* A queued value through the session at its receive time, as an exact-size
   heap copy. An empty value is malloc(0), a block with no bytes, so
   AddressSanitizer reports a read of its byte 0. It may be NULL. */
static void Deliver(BH_Harness *h, const SDL_BLEValue *value)
{
    uint8_t *copy = (uint8_t *)malloc(value->length);

    if (!copy && value->length) {
        printf("out of memory\n");
        exit(2);
    }
    if (value->length) {
        memcpy(copy, value->data, value->length);
    }
    SDL_BLESession_Value(&h->session, value->characteristic, copy, value->length, value->gap, value->time_ns);
    free(copy);
    BH_Drain(h);
}

static void Lost(BH_Harness *h)
{
    SDL_BLESession_Lost(&h->session, h->now);
    BH_Drain(h);
}

static void Close(BH_Harness *h)
{
    SDL_BLESession_Close(&h->session, h->now);
    BH_Drain(h);
}

static void Discover(BH_Harness *h, const bool *found, const uint8_t *properties)
{
    SDL_BLESession_Discovered(&h->session, true, found, properties, h->now);
    BH_Drain(h);
}

static void Read(BH_Harness *h, const uint8_t *data, size_t length)
{
    SDL_BLESession_Read(&h->session, data != NULL, data, length, h->now * 1000000);
    BH_Drain(h);
}

static void AllFound(bool *found)
{
    int i;

    for (i = 0; i < SDL_BLE_MAX_CHARS; ++i) {
        found[i] = true;
    }
}

/* Every answer but the awaited one, each succeeding and failing. None may
   take the wait, move the session or change a control. */
static void Strays(BH_Harness *h, int awaited, int line)
{
    static const uint8_t byte = 0x01;
    const SDL_BLEBase *base = (const SDL_BLEBase *)h->state;
    const int nactions = h->nactions;
    const int nsnapshots = h->nsnapshots;
    const uint8_t phase = h->session.phase;
    const bool waiting = h->session.waiting;
    const int step = h->session.step;
    const uint32_t changes = base->changes;
    bool found[SDL_BLE_MAX_CHARS];
    uint8_t properties[SDL_BLE_MAX_CHARS];
    int pass;

    AllFound(found);
    memset(properties, SDL_BLE_PROPERTY_NOTIFY | SDL_BLE_PROPERTY_READ | SDL_BLE_PROPERTY_WRITE, sizeof(properties));
    for (pass = 0; pass < 2; ++pass) {
        const bool success = (pass == 0);

        if (awaited != CONNECT) {
            SDL_BLESession_Connected(&h->session, success, !success, h->now);
        }
        if (awaited != PAIR) {
            SDL_BLESession_Paired(&h->session, success, h->now);
        }
        if (awaited != REMOVE_BOND) {
            SDL_BLESession_BondRemoved(&h->session, success, h->now);
        }
        if (awaited != DISCOVER) {
            SDL_BLESession_Discovered(&h->session, success, found, properties, h->now);
        }
        if (awaited != SUBSCRIBE) {
            SDL_BLESession_Subscribed(&h->session, success ? SDL_BLE_STATUS_SUCCESS : SDL_BLE_STATUS_PROTOCOL_ERROR,
                                      success ? 0 : SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION, h->now);
        }
        if (awaited != READ) {
            SDL_BLESession_Read(&h->session, success, success ? &byte : NULL, success ? 1 : 0, h->now * 1000000);
        }
        if (awaited != WRITE) {
            SDL_BLESession_Written(&h->session, success, h->now);
        }
        BH_Drain(h);
    }
    BH_CHECK(h->nactions == nactions && h->session.phase == phase && h->session.waiting == waiting &&
                 h->session.step == step && base->changes == changes && h->nsnapshots == nsnapshots,
             "line %d: an answer nobody waited for moved the session", line);
}

#define STRAYS(h, awaited) Strays(h, awaited, __LINE__)

/* Advertisements */

static SDL_BLEUUID UUID(const char *text)
{
    SDL_BLEUUID uuid;
    size_t count;

    memset(&uuid, 0, sizeof(uuid));
    count = BH_Hex(text, uuid.bytes, sizeof(uuid.bytes));
    BH_CHECK(count == sizeof(uuid.bytes), "UUID %s has %d bytes", text, (int)count);
    return uuid;
}

/* The lowercase text form, 36 characters */
static void FormatUUID(const SDL_BLEUUID *uuid, char *out)
{
    static const char hex[] = "0123456789abcdef";
    int i, n = 0;

    for (i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[n++] = '-';
        }
        out[n++] = hex[uuid->bytes[i] >> 4];
        out[n++] = hex[uuid->bytes[i] & 0x0F];
    }
    out[n] = '\0';
}

static SDL_BLEAdvertisement Ad(uint64_t address, uint8_t kind)
{
    SDL_BLEAdvertisement ad;

    memset(&ad, 0, sizeof(ad));
    ad.address = address;
    ad.kind = kind;
    return ad;
}

static void AdName(SDL_BLEAdvertisement *ad, const char *name)
{
    size_t length = strlen(name);

    if (length >= sizeof(ad->name)) {
        length = sizeof(ad->name) - 1;
    }
    memset(ad->name, 0, sizeof(ad->name));
    memcpy(ad->name, name, length);
    ad->has_name = true;
}

static void AdService(SDL_BLEAdvertisement *ad, const char *uuid)
{
    if (ad->nservices < SDL_BLE_AD_SERVICES) {
        ad->services[ad->nservices++] = UUID(uuid);
    }
}

/* Manufacturer data after the company ID, as hex */
static void AdMaker(SDL_BLEAdvertisement *ad, uint16_t company, const char *hex)
{
    if (ad->nmanufacturer < SDL_BLE_AD_MANUFACTURER) {
        SDL_BLEManufacturerData *data = &ad->manufacturer[ad->nmanufacturer++];

        memset(data, 0, sizeof(*data));
        data->company = company;
        data->length = (uint8_t)BH_Hex(hex, data->data, sizeof(data->data));
    }
}

static int Match(const SDL_BLEAdvertisement *ad, uint8_t *variant)
{
    uint8_t found = 0xEE;
    const int family = SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, ad, &found);

    if (variant) {
        *variant = found;
    }
    return family;
}

static int MatchName(uint8_t kind, const char *name, uint8_t *variant)
{
    SDL_BLEAdvertisement ad = Ad(0xC0FFEE000001, kind);

    AdName(&ad, name);
    return Match(&ad, variant);
}

static int MatchService(uint8_t kind, const char *uuid, uint8_t *variant)
{
    SDL_BLEAdvertisement ad = Ad(0xC0FFEE000001, kind);

    AdService(&ad, uuid);
    return Match(&ad, variant);
}

static int MatchMaker(uint8_t kind, uint16_t company, const char *hex, uint8_t *variant)
{
    SDL_BLEAdvertisement ad = Ad(0xC0FFEE000001, kind);

    AdMaker(&ad, company, hex);
    return Match(&ad, variant);
}

static bool Offer(SDL_BLEHost *host, const SDL_BLEAdvertisement *ad, uint64_t now, SDL_BLEMatch *match)
{
    return SDL_BLEHost_Advertise(host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, NULL, ad, now, match);
}

/* The host learns how a session ended, as the driver tells it */
static void Ended(SDL_BLEHost *host, uint64_t address, const BH_Harness *h)
{
    SDL_BLEOutcome outcome;

    CHECK(SDL_BLESession_Ended(&h->session));
    SDL_BLESession_GetOutcome(&h->session, &outcome);
    SDL_BLEHost_SessionEnded(host, address, &outcome, h->now);
}

/* Outcomes for the host tests, as flags */
#define END_BACKOFF   0x01
#define END_PUBLISHED 0x02
#define END_PAIRING   0x04
#define END_CONNECT   0x08

static void EndWith(SDL_BLEHost *host, uint64_t address, int flags, uint64_t now)
{
    SDL_BLEOutcome outcome;

    memset(&outcome, 0, sizeof(outcome));
    outcome.backoff = (flags & END_BACKOFF) != 0;
    outcome.published = (flags & END_PUBLISHED) != 0;
    outcome.pairing_failed = (flags & END_PAIRING) != 0;
    outcome.connect_failed = (flags & END_CONNECT) != 0;
    SDL_BLEHost_SessionEnded(host, address, &outcome, now);
}

/* The session's outcome is exactly these flags, whatever the outcome held
   before */
static void ExpectOutcome(const BH_Harness *h, int flags, int line)
{
    int pass;

    for (pass = 0; pass < 2; ++pass) {
        const bool before = (pass == 0);
        SDL_BLEOutcome outcome;

        outcome.backoff = before;
        outcome.published = before;
        outcome.pairing_failed = before;
        outcome.connect_failed = before;
        SDL_BLESession_GetOutcome(&h->session, &outcome);
        BH_CHECK(outcome.backoff == ((flags & END_BACKOFF) != 0) && outcome.published == ((flags & END_PUBLISHED) != 0) &&
                     outcome.pairing_failed == ((flags & END_PAIRING) != 0) &&
                     outcome.connect_failed == ((flags & END_CONNECT) != 0),
                 "line %d: the outcome is backoff %d, published %d, pairing failed %d, connect failed %d", line,
                 (int)outcome.backoff, (int)outcome.published, (int)outcome.pairing_failed, (int)outcome.connect_failed);
    }
}

#define EXPECT_OUTCOME(h, flags) ExpectOutcome(h, flags, __LINE__)

/* Reports */

/* A Poke Ball Plus report: byte 1 the buttons, X and Y the raw stick */
static void PokeballReport(uint8_t *report, uint8_t counter, uint8_t buttons, int x, int y)
{
    memset(report, 0, SDL_POKEBALL_REPORT_SIZE);
    report[0] = counter;
    report[1] = buttons;
    report[2] = (uint8_t)((x & 0x0F) << 4);
    report[3] = (uint8_t)((x >> 4) & 0x0F);
    report[4] = (uint8_t)y;
}

/* A 60-byte Gear VR packet: the touch state, the touch position, byte 58 and
   the battery */
static void GearVRPacket(uint8_t *packet, int state, int x, int y, uint8_t buttons, uint8_t battery)
{
    memset(packet, 0, SDL_GEARVR_PACKET_SIZE);
    packet[54] = (uint8_t)((state << 4) | ((x >> 6) & 0x0F));
    packet[55] = (uint8_t)(((x & 0x3F) << 2) | ((y >> 8) & 0x03));
    packet[56] = (uint8_t)(y & 0xFF);
    packet[58] = buttons;
    packet[59] = battery;
}

/* A live Gear VR stream: the clock advances to now with an idle packet
   every 500 ms, inside the module's silence timeout, and the timers run on
   the way */
static void GearVRStream(BH_Harness *h, uint64_t now)
{
    uint8_t packet[SDL_GEARVR_PACKET_SIZE];

    GearVRPacket(packet, 2, 0, 0, 0, 100);
    while (h->now < now) {
        BH_Advance(h, (now - h->now > 500) ? h->now + 500 : now);
        BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    }
}

/* A 20-byte Oculus Go 8126FACE value with byte 19 set */
static void OculusGoFace(uint8_t *value, uint8_t buttons)
{
    memset(value, 0, SDL_OCULUSGO_REPORT_SIZE);
    value[19] = buttons;
}

/* The Myo's properties (myohw.h:63-93): IMU data notify, classifier and
   motion events indicate, the command write, the battery read and notify */
static void MyoProperties(uint8_t *properties)
{
    memset(properties, 0, SDL_BLE_MAX_CHARS);
    properties[SDL_MYO_IMU] = SDL_BLE_PROPERTY_NOTIFY;
    properties[SDL_MYO_CLASSIFIER] = SDL_BLE_PROPERTY_INDICATE;
    properties[SDL_MYO_MOTION] = SDL_BLE_PROPERTY_INDICATE;
    properties[SDL_MYO_COMMAND] = SDL_BLE_PROPERTY_WRITE;
    properties[SDL_MYO_BATTERY] = SDL_BLE_PROPERTY_READ | SDL_BLE_PROPERTY_NOTIFY;
}

/* Quaternion W of 16384 and nothing else */
static const uint8_t myo_imu_level[SDL_MYO_IMU_SIZE] = { 0x00, 0x40 };

/* The probe: a module that records every call */

#define PROBE_INPUT     0
#define PROBE_BATTERY   1
#define PROBE_COMMAND   2
#define PROBE_MAX_CALLS 64

typedef struct ProbeCall
{
    char kind; /* 'S' start, 'V' value, 'D' write done, 'T' tick, 'C' close */
    int characteristic;
    bool success;
    size_t length;
    uint8_t data[SDL_BLE_MAX_VALUE];
    uint64_t time;
} ProbeCall;

typedef struct ProbeState
{
    SDL_BLEBase base;
    int start_writes; /* Writes Start queues, set once the session exists */
    bool started;
    bool has_deadline;
    uint64_t deadline;
    int ncalls;
    ProbeCall calls[PROBE_MAX_CALLS];
} ProbeState;

static ProbeCall probe_overflow;

static ProbeCall *Probe_Record(ProbeState *probe, char kind, uint64_t time)
{
    ProbeCall *call = (probe->ncalls < PROBE_MAX_CALLS) ? &probe->calls[probe->ncalls++] : &probe_overflow;

    memset(call, 0, sizeof(*call));
    call->kind = kind;
    call->time = time;
    return call;
}

static void Probe_Reset(void *state, const SDL_BLESink *sink, const SDL_BLEModuleContext *context)
{
    ProbeState *probe = (ProbeState *)state;
    SDL_BLEIdentity identity;

    (void)context;
    memset(probe, 0, sizeof(*probe));
    SDL_BLE_SetIdentity(&identity, "Probe", SDL_BLE_TYPE_GAMEPAD, 1u << SDL_BLE_BUTTON_SOUTH, 1u << SDL_BLE_AXIS_LEFTX, true);
    SDL_BLE_ResetBase(&probe->base, sink, &identity);
}

/* Queues start_writes one-byte writes, 10, 11 and on, with a response on
   every other one */
static void Probe_Start(void *state, uint64_t now)
{
    ProbeState *probe = (ProbeState *)state;
    int i;

    Probe_Record(probe, 'S', now);
    for (i = 0; i < probe->start_writes; ++i) {
        const uint8_t byte = (uint8_t)(0x10 + i);

        SDL_BLE_QueueWrite(&probe->base, PROBE_COMMAND, &byte, 1, (i % 2) == 0);
    }
    probe->started = true;
}

/* Input byte 0: FF fails the module, else bit 0 is SOUTH. Ready with the
   first input after the start. */
static void Probe_Value(void *state, int characteristic, const uint8_t *data, size_t length, uint64_t time_ns)
{
    ProbeState *probe = (ProbeState *)state;
    ProbeCall *call = Probe_Record(probe, 'V', time_ns);
    SDL_BLEControls controls = probe->base.controls;

    call->characteristic = characteristic;
    call->length = length;
    if (length) {
        memcpy(call->data, data, (length < sizeof(call->data)) ? length : sizeof(call->data));
    }
    if (characteristic != PROBE_INPUT || length < 1) {
        return;
    }
    if (data[0] == 0xFF) {
        probe->base.failed = true;
        return;
    }
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_SOUTH, (data[0] & 0x01) != 0);
    SDL_BLE_Commit(&probe->base, &controls, time_ns);
    if (probe->started) {
        probe->base.ready = true;
    }
}

static void Probe_WriteDone(void *state, bool success, uint64_t now)
{
    ProbeState *probe = (ProbeState *)state;

    Probe_Record(probe, 'D', now)->success = success;
}

static void Probe_Tick(void *state, uint64_t now)
{
    ProbeState *probe = (ProbeState *)state;

    Probe_Record(probe, 'T', now);
    if (probe->has_deadline && now >= probe->deadline) {
        probe->has_deadline = false;
    }
}

static bool Probe_GetDeadline(void *state, uint64_t *deadline)
{
    ProbeState *probe = (ProbeState *)state;

    if (!probe->has_deadline) {
        return false;
    }
    *deadline = probe->deadline;
    return true;
}

/* One closing write, C0 */
static void Probe_Close(void *state, uint64_t now)
{
    ProbeState *probe = (ProbeState *)state;
    static const uint8_t byte = 0xC0;

    Probe_Record(probe, 'C', now);
    SDL_BLE_QueueWrite(&probe->base, PROBE_COMMAND, &byte, 1, true);
    probe->started = false;
}

static const SDL_BLEModule probe_module = {
    sizeof(ProbeState),
    Probe_Reset,
    Probe_Start,
    Probe_Value,
    Probe_WriteDone,
    Probe_Tick,
    Probe_GetDeadline,
    Probe_Close
};

/* An input to subscribe, a battery to subscribe and read, a command to write */
static const SDL_BLEFamily probe_family = {
    "Probe",
    1,
    {
        { SDL_BLE_KEY_NAME_EQUALS, false, "Probe", SDL_BLE_UUID16_INIT(0x00, 0x00), 0, 0, { 0 } },
    },
    1,
    {
        SDL_BLE_UUID_INIT(0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0),
    },
    false,
    SDL_BLE_UUID16_INIT(0x00, 0x00),
    3,
    {
        { 0, SDL_BLE_UUID_INIT(0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf1),
          SDL_BLE_CHAR_SUBSCRIBE },
        { 0, SDL_BLE_UUID16_INIT(0x2a, 0x19), SDL_BLE_CHAR_SUBSCRIBE | SDL_BLE_CHAR_READ | SDL_BLE_CHAR_OPTIONAL },
        { 0, SDL_BLE_UUID_INIT(0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf3),
          0 },
    },
    SDL_BLE_PAIR_NOT_NEEDED,
    &probe_module
};

static ProbeState *Probe(BH_Harness *h)
{
    return (ProbeState *)h->state;
}

static int ProbeCount(BH_Harness *h, char kind)
{
    const ProbeState *probe = Probe(h);
    int i, count = 0;

    for (i = 0; i < probe->ncalls; ++i) {
        if (probe->calls[i].kind == kind) {
            ++count;
        }
    }
    return count;
}

#define PROBE_RESUBSCRIBE 0xFE

/* The probe, where input byte FE asks for a resubscription and queues write
   FE with a response in the same call */
static void Probe_ResubscribeValue(void *state, int characteristic, const uint8_t *data, size_t length, uint64_t time_ns)
{
    ProbeState *probe = (ProbeState *)state;

    if (characteristic == PROBE_INPUT && length >= 1 && data[0] == PROBE_RESUBSCRIBE) {
        Probe_Record(probe, 'V', time_ns);
        probe->base.resubscribe = true;
        SDL_BLE_QueueWrite(&probe->base, PROBE_COMMAND, data, 1, true);
        return;
    }
    Probe_Value(state, characteristic, data, length, time_ns);
}

static const SDL_BLEModule resubscribe_probe_module = {
    sizeof(ProbeState),
    Probe_Reset,
    Probe_Start,
    Probe_ResubscribeValue,
    Probe_WriteDone,
    Probe_Tick,
    Probe_GetDeadline,
    Probe_Close
};

/* Test 1: every row of the discovery table matches its family from a
   name-only, a service-only and a manufacturer-only record wherever those
   are match keys, in advertisements and scan responses alike. Corroborating
   keys never match alone, and the part's other records match nothing. */
static void Test1_Matcher(void)
{
    static const char *const zwift_in[] = { "02", "03", "09", "0E" };
    static const uint8_t zwift_types[] = { 0x02, 0x03, 0x09, 0x0E };
    static const char *const zwift_out[] = { "07", "08", "0A", "0B", "00", "01", "FF" };
    bool enabled[SDL_BLE_FAMILY_COUNT];
    SDL_BLEAdvertisement ad;
    char data[16];
    uint8_t variant;
    int k, i;

    for (k = 0; k < 2; ++k) {
        const uint8_t kind = (k == 0) ? SDL_BLE_AD_ADVERTISEMENT : SDL_BLE_AD_SCAN_RESPONSE;

        /* Name-only records */
        variant = 0xEE;
        CHECK(MatchName(kind, "Pokemon PBP", &variant) == SDL_BLE_FAMILY_POKEBALL && variant == 0);
        variant = 0xEE;
        CHECK(MatchName(kind, "Daydream controller", &variant) == SDL_BLE_FAMILY_DAYDREAM && variant == 0);
        variant = 0xEE;
        CHECK(MatchName(kind, "Gear VR Controller(466A)", &variant) == SDL_BLE_FAMILY_GEARVR && variant == 0);
        CHECK(MatchName(kind, "Gear VR Controller", NULL) == SDL_BLE_FAMILY_GEARVR);
        variant = 0xEE;
        CHECK(MatchName(kind, "OMVR-V190", &variant) == SDL_BLE_FAMILY_OCULUSGO && variant == 0);
        variant = 0xEE;
        CHECK(MatchName(kind, "Ble Guitar", &variant) == SDL_BLE_FAMILY_GHLIOS && variant == 0);
        CHECK(MatchName(kind, "XBle Guitar 2", NULL) == SDL_BLE_FAMILY_GHLIOS);
        /* Zwift is not found by name and the Myo never is */
        CHECK(MatchName(kind, "Zwift Play", NULL) == -1);
        CHECK(MatchName(kind, "Zwift Click", NULL) == -1);
        CHECK(MatchName(kind, "Myo", NULL) == -1);

        /* Service-only records. FE55, Google's member UUID, only
           corroborates the Daydream's name. */
        CHECK(MatchService(kind, "0000fe55-0000-1000-8000-00805f9b34fb", NULL) == -1);
        variant = 0xEE;
        CHECK(MatchService(kind, "4f63756c-7573-2054-6872-65656d6f7465", &variant) == SDL_BLE_FAMILY_GEARVR && variant == 0);
        variant = 0xEE;
        CHECK(MatchService(kind, "81265652-3692-ae93-e711-270f223c83b3", &variant) == SDL_BLE_FAMILY_OCULUSGO && variant == 0);
        variant = 0xEE;
        CHECK(MatchService(kind, "00001523-0000-1000-8000-00805f9b34fb", &variant) == SDL_BLE_FAMILY_GHLIOS && variant == 0);
        variant = 0xEE;
        CHECK(MatchService(kind, "d5060001-a904-deb9-4748-2c7f4a124842", &variant) == SDL_BLE_FAMILY_MYO && variant == 0);

        /* Manufacturer-only records: Zwift's type byte, then two address
           bytes, and the type byte is the variant */
        for (i = 0; i < COUNT(zwift_in); ++i) {
            snprintf(data, sizeof(data), "%s 34 12", zwift_in[i]);
            variant = 0xEE;
            BH_CHECK(MatchMaker(kind, 0x094A, data, &variant) == SDL_BLE_FAMILY_ZWIFT && variant == zwift_types[i],
                     "Zwift type %s", zwift_in[i]);
            /* The type byte alone is enough */
            BH_CHECK(MatchMaker(kind, 0x094A, zwift_in[i], NULL) == SDL_BLE_FAMILY_ZWIFT, "Zwift type %s alone", zwift_in[i]);
        }

        /* The part's records that match nothing */
        CHECK(MatchName(kind, "Pokemon PBX", NULL) == -1);
        CHECK(MatchMaker(kind, 0x094A, "07 34 12", NULL) == -1);
        CHECK(MatchService(kind, "0000fe59-0000-1000-8000-00805f9b34fb", NULL) == -1);
        CHECK(MatchMaker(kind, 0x0075, "01 00 02 00", NULL) == -1);
        ad = Ad(0xC0FFEE000001, kind);
        AdMaker(&ad, 0x0075, "01 00 02 00");
        AdName(&ad, "Galaxy Buds");
        AdService(&ad, "0000180f-0000-1000-8000-00805f9b34fb");
        CHECK(Match(&ad, NULL) == -1);

        /* Corroborating keys never match alone, and change nothing beside
           a key that matches */
        CHECK(MatchService(kind, "00000001-19ca-4651-86e5-fa29dcdd09d1", NULL) == -1);
        CHECK(MatchService(kind, "0000fc82-0000-1000-8000-00805f9b34fb", NULL) == -1);
        ad = Ad(0xC0FFEE000001, kind);
        AdService(&ad, "00000001-19ca-4651-86e5-fa29dcdd09d1");
        AdService(&ad, "0000fc82-0000-1000-8000-00805f9b34fb");
        AdName(&ad, "Zwift Play");
        CHECK(Match(&ad, NULL) == -1);
        AdMaker(&ad, 0x094A, "03 34 12");
        variant = 0xEE;
        CHECK(Match(&ad, &variant) == SDL_BLE_FAMILY_ZWIFT && variant == 0x03);
        ad = Ad(0xC0FFEE000001, kind);
        AdMaker(&ad, 0x0075, "01");
        AdService(&ad, "4f63756c-7573-2054-6872-65656d6f7465");
        variant = 0xEE;
        CHECK(Match(&ad, &variant) == SDL_BLE_FAMILY_GEARVR && variant == 0);

        /* Zwift models out of scope and malformed Zwift data */
        for (i = 0; i < COUNT(zwift_out); ++i) {
            snprintf(data, sizeof(data), "%s 34 12", zwift_out[i]);
            BH_CHECK(MatchMaker(kind, 0x094A, data, NULL) == -1, "Zwift type %s", zwift_out[i]);
        }
        CHECK(MatchMaker(kind, 0x094A, "", NULL) == -1);
        /* The Switch 2's company belongs to its own driver */
        CHECK(MatchMaker(kind, 0x0553, "01 00 03 7e 05 66 20 00 01 81 00", NULL) == -1);

        /* Names are compared case for case, an equals key takes the whole
           name and a prefix key the start */
        CHECK(MatchName(kind, "Pokemon PB", NULL) == -1);
        CHECK(MatchName(kind, "pokemon pbp", NULL) == -1);
        CHECK(MatchName(kind, "Pokemon PBP ", NULL) == -1);
        CHECK(MatchName(kind, "Daydream controller 2", NULL) == -1);
        CHECK(MatchName(kind, "daydream controller", NULL) == -1);
        CHECK(MatchName(kind, "Gear VR", NULL) == -1);
        CHECK(MatchName(kind, "My Gear VR Controller", NULL) == -1);
        CHECK(MatchName(kind, "omvr-v190", NULL) == -1);
        CHECK(MatchName(kind, "OMV", NULL) == -1);
        CHECK(MatchName(kind, "BLE Guitar", NULL) == -1);
        CHECK(MatchName(kind, "Ble Guita", NULL) == -1);
        CHECK(MatchName(kind, "", NULL) == -1);
        /* A name the event does not carry is not read */
        ad = Ad(0xC0FFEE000001, kind);
        AdName(&ad, "Pokemon PBP");
        ad.has_name = false;
        CHECK(Match(&ad, NULL) == -1);

        /* A key found past the first entry */
        ad = Ad(0xC0FFEE000001, kind);
        AdMaker(&ad, 0x0075, "02");
        AdMaker(&ad, 0x094A, "09 34 12");
        variant = 0xEE;
        CHECK(Match(&ad, &variant) == SDL_BLE_FAMILY_ZWIFT && variant == 0x09);
        ad = Ad(0xC0FFEE000001, kind);
        AdService(&ad, "0000180f-0000-1000-8000-00805f9b34fb");
        AdService(&ad, "0000fe59-0000-1000-8000-00805f9b34fb");
        AdService(&ad, "d5060001-a904-deb9-4748-2c7f4a124842");
        CHECK(Match(&ad, NULL) == SDL_BLE_FAMILY_MYO);
    }

    /* The first enabled family in table order wins, and a disabled family
       is skipped */
    for (i = 0; i < SDL_BLE_FAMILY_COUNT; ++i) {
        enabled[i] = true;
    }
    ad = Ad(0xC0FFEE000001, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&ad, "Daydream controller");
    AdService(&ad, "d5060001-a904-deb9-4748-2c7f4a124842");
    CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, enabled, &ad, NULL) == SDL_BLE_FAMILY_DAYDREAM);
    enabled[SDL_BLE_FAMILY_DAYDREAM] = false;
    CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, enabled, &ad, NULL) == SDL_BLE_FAMILY_MYO);
    enabled[SDL_BLE_FAMILY_MYO] = false;
    CHECK(SDL_BLE_Match(SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, enabled, &ad, NULL) == -1);
}

/* Families with one odd key each, for the matcher's edges */
static const SDL_BLEFamily nameless_family = {
    "Nameless key",
    1,
    {
        { SDL_BLE_KEY_NAME_EQUALS, false, NULL, SDL_BLE_UUID16_INIT(0x00, 0x00), 0, 0, { 0 } },
    },
    0,
    { SDL_BLE_UUID16_INIT(0x00, 0x00) },
    false,
    SDL_BLE_UUID16_INIT(0x00, 0x00),
    0,
    { { 0, SDL_BLE_UUID16_INIT(0x00, 0x00), 0 } },
    SDL_BLE_PAIR_NOT_NEEDED,
    &probe_module
};

static const SDL_BLEFamily untyped_family = {
    "Untyped key",
    1,
    {
        { SDL_BLE_KEY_MANUFACTURER, false, NULL, SDL_BLE_UUID16_INIT(0x00, 0x00), 0x1234, 0, { 0 } },
    },
    0,
    { SDL_BLE_UUID16_INIT(0x00, 0x00) },
    false,
    SDL_BLE_UUID16_INIT(0x00, 0x00),
    0,
    { { 0, SDL_BLE_UUID16_INIT(0x00, 0x00), 0 } },
    SDL_BLE_PAIR_NOT_NEEDED,
    &probe_module
};

static const SDL_BLEFamily unknown_key_family = {
    "Unknown key kind",
    1,
    {
        { 99, false, NULL, SDL_BLE_UUID16_INIT(0x00, 0x00), 0, 0, { 0 } },
    },
    0,
    { SDL_BLE_UUID16_INIT(0x00, 0x00) },
    false,
    SDL_BLE_UUID16_INIT(0x00, 0x00),
    0,
    { { 0, SDL_BLE_UUID16_INIT(0x00, 0x00), 0 } },
    SDL_BLE_PAIR_NOT_NEEDED,
    &probe_module
};

/* The matcher's edges: names that fill the buffer, keys without a name or
   with an unknown kind, the last byte of a key, counts past the arrays and
   empty manufacturer data */
static void TestMatcherEdges(void)
{
    const SDL_BLEFamily *nameless[1] = { &nameless_family };
    const SDL_BLEFamily *untyped[1] = { &untyped_family };
    const SDL_BLEFamily *unknown[1] = { &unknown_key_family };
    SDL_BLEAdvertisement ad, *heap;
    uint8_t variant;
    int i;

    /* A 64-byte name without a terminator is read whole */
    ad = Ad(0xC0FFEE000001, SDL_BLE_AD_ADVERTISEMENT);
    memset(ad.name, 'x', sizeof(ad.name));
    memcpy(&ad.name[sizeof(ad.name) - 10], "Ble Guitar", 10);
    ad.has_name = true;
    CHECK(Match(&ad, NULL) == SDL_BLE_FAMILY_GHLIOS);

    /* A name key without a name matches nothing */
    ad = Ad(0xC0FFEE000001, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&ad, "Probe");
    CHECK(SDL_BLE_Match(nameless, 1, NULL, &ad, NULL) == -1);

    /* The last byte of a prefix key and of a contains key counts */
    CHECK(MatchName(SDL_BLE_AD_ADVERTISEMENT, "Gear VR ControlleX", NULL) == -1);
    CHECK(MatchName(SDL_BLE_AD_ADVERTISEMENT, "Ble Guitax", NULL) == -1);

    /* Counts past the arrays are not followed. The record sits on the heap,
       so AddressSanitizer sees a read past it. */
    heap = (SDL_BLEAdvertisement *)malloc(sizeof(*heap));
    if (!heap) {
        printf("out of memory\n");
        exit(2);
    }
    *heap = Ad(0xC0FFEE000001, SDL_BLE_AD_ADVERTISEMENT);
    for (i = 0; i < SDL_BLE_AD_SERVICES; ++i) {
        AdService(heap, "0000180f-0000-1000-8000-00805f9b34fb");
    }
    heap->nservices = 1000;
    CHECK(Match(heap, NULL) == -1);
    *heap = Ad(0xC0FFEE000001, SDL_BLE_AD_ADVERTISEMENT);
    for (i = 0; i < SDL_BLE_AD_MANUFACTURER; ++i) {
        AdMaker(heap, 0x0001, "02");
    }
    heap->nmanufacturer = 1000;
    CHECK(Match(heap, NULL) == -1);
    free(heap);

    /* A key without type bytes and an empty record give variant 0 */
    ad = Ad(0xC0FFEE000001, SDL_BLE_AD_ADVERTISEMENT);
    AdMaker(&ad, 0x1234, "5A");
    ad.manufacturer[0].length = 0;
    variant = 0xEE;
    CHECK(SDL_BLE_Match(untyped, 1, NULL, &ad, &variant) == 0 && variant == 0);

    /* An empty record is not read for its type */
    ad = Ad(0xC0FFEE000001, SDL_BLE_AD_ADVERTISEMENT);
    AdMaker(&ad, 0x094A, "02");
    ad.manufacturer[0].length = 0;
    CHECK(Match(&ad, NULL) == -1);

    /* A key of an unknown kind matches nothing */
    ad = Ad(0xC0FFEE000001, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&ad, "Probe");
    AdService(&ad, "0000180f-0000-1000-8000-00805f9b34fb");
    AdMaker(&ad, 0x1234, "02");
    CHECK(SDL_BLE_Match(unknown, 1, NULL, &ad, NULL) == -1);
}

/* The device table against the part's tables: every UUID of every family as
   lowercase text, the keys, the characteristic flags and service indices, the
   pairing rules and the modules' characteristic indices */

typedef struct ExpectedKey
{
    uint8_t kind;
    bool corroborating;
    const char *text; /* The name, or the service UUID */
    uint16_t company;
    const char *types; /* Manufacturer type bytes as hex */
} ExpectedKey;

typedef struct ExpectedCharacteristic
{
    uint8_t service;
    const char *uuid;
    uint8_t flags;
} ExpectedCharacteristic;

typedef struct ExpectedFamily
{
    int id;
    const SDL_BLEFamily *family;
    const SDL_BLEModule *module;
    int nkeys;
    ExpectedKey keys[SDL_BLE_MAX_KEYS];
    int nservices;
    const char *services[SDL_BLE_MAX_SERVICES];
    const char *alternate; /* NULL for none */
    int ncharacteristics;
    ExpectedCharacteristic characteristics[SDL_BLE_MAX_CHARS];
    uint8_t pairing;
} ExpectedFamily;

#define BATTERY_SERVICE "0000180f-0000-1000-8000-00805f9b34fb"
#define BATTERY_LEVEL   "00002a19-0000-1000-8000-00805f9b34fb"
#define BATTERY_FLAGS   (SDL_BLE_CHAR_SUBSCRIBE | SDL_BLE_CHAR_READ | SDL_BLE_CHAR_OPTIONAL)

/* The strings of comment-15.md and comment-16.md. The Myo's follow the
   d506xxxx-a904-deb9-4748-2c7f4a124842 rule with the short UUIDs of
   myohw.h:63-85: control service 0001, IMU service 0002, classifier service
   0003, command 0401, IMU data 0402, motion event 0502, classifier event
   0103. */
static const ExpectedFamily expected_families[SDL_BLE_FAMILY_COUNT] = {
    { SDL_BLE_FAMILY_POKEBALL, &SDL_BLEPokeballFamily, &SDL_BLEPokeballModule,
      1,
      { { SDL_BLE_KEY_NAME_EQUALS, false, "Pokemon PBP", 0, "" } },
      2,
      { "6675e16c-f36d-4567-bb55-6b51e27a23e5", BATTERY_SERVICE },
      NULL,
      2,
      { { 0, "6675e16c-f36d-4567-bb55-6b51e27a23e6", SDL_BLE_CHAR_SUBSCRIBE },
        { 1, BATTERY_LEVEL, BATTERY_FLAGS } },
      SDL_BLE_PAIR_NOT_NEEDED },
    { SDL_BLE_FAMILY_DAYDREAM, &SDL_BLEDaydreamFamily, &SDL_BLEDaydreamModule,
      2,
      { { SDL_BLE_KEY_NAME_EQUALS, false, "Daydream controller", 0, "" },
        { SDL_BLE_KEY_SERVICE, true, "0000fe55-0000-1000-8000-00805f9b34fb", 0, "" } },
      2,
      { "0000fe55-0000-1000-8000-00805f9b34fb", BATTERY_SERVICE },
      NULL,
      2,
      { { 0, "00000001-1000-1000-8000-00805f9b34fb", SDL_BLE_CHAR_SUBSCRIBE },
        { 1, BATTERY_LEVEL, BATTERY_FLAGS } },
      SDL_BLE_PAIR_FIRST },
    { SDL_BLE_FAMILY_GEARVR, &SDL_BLEGearVRFamily, &SDL_BLEGearVRModule,
      3,
      { { SDL_BLE_KEY_SERVICE, false, "4f63756c-7573-2054-6872-65656d6f7465", 0, "" },
        { SDL_BLE_KEY_NAME_PREFIX, false, "Gear VR Controller", 0, "" },
        { SDL_BLE_KEY_MANUFACTURER, true, NULL, 0x0075, "" } },
      1,
      { "4f63756c-7573-2054-6872-65656d6f7465" },
      NULL,
      2,
      { { 0, "c8c51726-81bc-483b-a052-f7a14ea3d281", SDL_BLE_CHAR_SUBSCRIBE },
        { 0, "c8c51726-81bc-483b-a052-f7a14ea3d282", 0 } },
      SDL_BLE_PAIR_ON_ERROR },
    { SDL_BLE_FAMILY_OCULUSGO, &SDL_BLEOculusGoFamily, &SDL_BLEOculusGoModule,
      2,
      { { SDL_BLE_KEY_NAME_PREFIX, false, "OMVR", 0, "" },
        { SDL_BLE_KEY_SERVICE, false, "81265652-3692-ae93-e711-270f223c83b3", 0, "" } },
      1,
      { "81265652-3692-ae93-e711-270f223c83b3" },
      NULL,
      2,
      { { 0, "8126face-3692-ae93-e711-270f223c83b3", SDL_BLE_CHAR_SUBSCRIBE },
        { 0, "8126beef-3692-ae93-e711-270f223c83b3", SDL_BLE_CHAR_SUBSCRIBE | SDL_BLE_CHAR_OPTIONAL } },
      SDL_BLE_PAIR_ON_ERROR },
    { SDL_BLE_FAMILY_GHLIOS, &SDL_BLEGHLiOSFamily, &SDL_BLEGHLiOSModule,
      2,
      { { SDL_BLE_KEY_NAME_CONTAINS, false, "Ble Guitar", 0, "" },
        { SDL_BLE_KEY_SERVICE, false, "00001523-0000-1000-8000-00805f9b34fb", 0, "" } },
      1,
      { "533e1523-3abe-f33f-cd00-594e8b0a8ea3" },
      NULL,
      1,
      { { 0, "533e1524-3abe-f33f-cd00-594e8b0a8ea3", SDL_BLE_CHAR_SUBSCRIBE } },
      SDL_BLE_PAIR_NOT_NEEDED },
    { SDL_BLE_FAMILY_ZWIFT, &SDL_BLEZwiftFamily, &SDL_BLEZwiftModule,
      3,
      { { SDL_BLE_KEY_MANUFACTURER, false, NULL, 0x094A, "02 03 09 0E" },
        { SDL_BLE_KEY_SERVICE, true, "00000001-19ca-4651-86e5-fa29dcdd09d1", 0, "" },
        { SDL_BLE_KEY_SERVICE, true, "0000fc82-0000-1000-8000-00805f9b34fb", 0, "" } },
      1,
      { "00000001-19ca-4651-86e5-fa29dcdd09d1" },
      "0000fc82-0000-1000-8000-00805f9b34fb",
      3,
      { { 0, "00000002-19ca-4651-86e5-fa29dcdd09d1", SDL_BLE_CHAR_SUBSCRIBE },
        { 0, "00000004-19ca-4651-86e5-fa29dcdd09d1", SDL_BLE_CHAR_SUBSCRIBE },
        { 0, "00000003-19ca-4651-86e5-fa29dcdd09d1", 0 } },
      SDL_BLE_PAIR_NOT_NEEDED },
    { SDL_BLE_FAMILY_MYO, &SDL_BLEMyoFamily, &SDL_BLEMyoModule,
      1,
      { { SDL_BLE_KEY_SERVICE, false, "d5060001-a904-deb9-4748-2c7f4a124842", 0, "" } },
      4,
      { "d5060001-a904-deb9-4748-2c7f4a124842", "d5060002-a904-deb9-4748-2c7f4a124842",
        "d5060003-a904-deb9-4748-2c7f4a124842", BATTERY_SERVICE },
      NULL,
      5,
      { { 1, "d5060402-a904-deb9-4748-2c7f4a124842", SDL_BLE_CHAR_SUBSCRIBE },
        { 2, "d5060103-a904-deb9-4748-2c7f4a124842", SDL_BLE_CHAR_SUBSCRIBE },
        { 1, "d5060502-a904-deb9-4748-2c7f4a124842", SDL_BLE_CHAR_SUBSCRIBE },
        { 0, "d5060401-a904-deb9-4748-2c7f4a124842", 0 },
        { 3, BATTERY_LEVEL, BATTERY_FLAGS } },
      SDL_BLE_PAIR_NOT_NEEDED },
};

/* Each module's characteristic indices name the characteristic its code means */
static const struct
{
    int family;
    int index;
    const char *uuid;
} module_indices[] = {
    { SDL_BLE_FAMILY_POKEBALL, SDL_POKEBALL_INPUT, "6675e16c-f36d-4567-bb55-6b51e27a23e6" },
    { SDL_BLE_FAMILY_POKEBALL, SDL_POKEBALL_BATTERY, BATTERY_LEVEL },
    { SDL_BLE_FAMILY_DAYDREAM, SDL_DAYDREAM_POSE, "00000001-1000-1000-8000-00805f9b34fb" },
    { SDL_BLE_FAMILY_DAYDREAM, SDL_DAYDREAM_BATTERY, BATTERY_LEVEL },
    { SDL_BLE_FAMILY_GEARVR, SDL_GEARVR_DATA, "c8c51726-81bc-483b-a052-f7a14ea3d281" },
    { SDL_BLE_FAMILY_GEARVR, SDL_GEARVR_COMMAND, "c8c51726-81bc-483b-a052-f7a14ea3d282" },
    { SDL_BLE_FAMILY_OCULUSGO, SDL_OCULUSGO_FACE, "8126face-3692-ae93-e711-270f223c83b3" },
    { SDL_BLE_FAMILY_OCULUSGO, SDL_OCULUSGO_BEEF, "8126beef-3692-ae93-e711-270f223c83b3" },
    { SDL_BLE_FAMILY_GHLIOS, SDL_GHLIOS_INPUT, "533e1524-3abe-f33f-cd00-594e8b0a8ea3" },
    { SDL_BLE_FAMILY_ZWIFT, SDL_ZWIFT_ASYNC, "00000002-19ca-4651-86e5-fa29dcdd09d1" },
    { SDL_BLE_FAMILY_ZWIFT, SDL_ZWIFT_SYNC_TX, "00000004-19ca-4651-86e5-fa29dcdd09d1" },
    { SDL_BLE_FAMILY_ZWIFT, SDL_ZWIFT_SYNC_RX, "00000003-19ca-4651-86e5-fa29dcdd09d1" },
    { SDL_BLE_FAMILY_MYO, SDL_MYO_IMU, "d5060402-a904-deb9-4748-2c7f4a124842" },
    { SDL_BLE_FAMILY_MYO, SDL_MYO_CLASSIFIER, "d5060103-a904-deb9-4748-2c7f4a124842" },
    { SDL_BLE_FAMILY_MYO, SDL_MYO_MOTION, "d5060502-a904-deb9-4748-2c7f4a124842" },
    { SDL_BLE_FAMILY_MYO, SDL_MYO_COMMAND, "d5060401-a904-deb9-4748-2c7f4a124842" },
    { SDL_BLE_FAMILY_MYO, SDL_MYO_BATTERY, BATTERY_LEVEL },
};

static void CheckUUID(const char *family, const char *what, int index, const SDL_BLEUUID *uuid, const char *expected)
{
    char text[40];

    FormatUUID(uuid, text);
    BH_CHECK(expected && strcmp(text, expected) == 0, "%s %s %d is %s, the part gives %s", family, what, index, text,
             expected ? expected : "none");
}

static void CheckFamily(const ExpectedFamily *expected)
{
    const SDL_BLEFamily *family = SDL_BLE_Families[expected->id];
    const SDL_BLEModule *module;
    int i;

    BH_CHECK(family == expected->family, "family %d is out of the part's order", expected->id);
    if (!family) {
        return;
    }
    module = family->module;
    BH_CHECK(module == expected->module, "%s has another module", family->name);
    BH_CHECK(module && module->state_size >= sizeof(SDL_BLEBase) && module->Reset && module->Start && module->Value &&
                 module->WriteDone && module->Tick && module->GetDeadline && module->Close,
             "%s has an incomplete module", family->name);

    BH_CHECK(family->nkeys == expected->nkeys, "%s has %d keys", family->name, family->nkeys);
    for (i = 0; i < expected->nkeys && i < family->nkeys; ++i) {
        const SDL_BLEMatchKey *key = &family->keys[i];
        const ExpectedKey *want = &expected->keys[i];
        uint8_t types[4];
        size_t ntypes;

        BH_CHECK(key->kind == want->kind && key->corroborating == want->corroborating, "%s key %d kind", family->name, i);
        switch (want->kind) {
        case SDL_BLE_KEY_SERVICE:
            CheckUUID(family->name, "key", i, &key->uuid, want->text);
            break;
        case SDL_BLE_KEY_MANUFACTURER:
            ntypes = BH_Hex(want->types, types, sizeof(types));
            BH_CHECK(key->company == want->company && (size_t)key->ntypes == ntypes && memcmp(key->types, types, ntypes) == 0,
                     "%s key %d is company %04X with %d types", family->name, i, (unsigned int)key->company, (int)key->ntypes);
            break;
        default:
            BH_CHECK(key->name && strcmp(key->name, want->text) == 0, "%s key %d is \"%s\", the part gives \"%s\"",
                     family->name, i, key->name ? key->name : "", want->text);
            break;
        }
    }

    BH_CHECK(family->nservices == expected->nservices, "%s has %d services", family->name, family->nservices);
    for (i = 0; i < expected->nservices && i < family->nservices; ++i) {
        CheckUUID(family->name, "service", i, &family->services[i], expected->services[i]);
    }
    BH_CHECK(family->has_alternate == (expected->alternate != NULL), "%s alternate service", family->name);
    if (family->has_alternate && expected->alternate) {
        CheckUUID(family->name, "alternate service", 0, &family->alternate, expected->alternate);
    }

    BH_CHECK(family->ncharacteristics == expected->ncharacteristics && family->ncharacteristics <= SDL_BLE_MAX_CHARS,
             "%s has %d characteristics", family->name, family->ncharacteristics);
    for (i = 0; i < expected->ncharacteristics && i < family->ncharacteristics; ++i) {
        const SDL_BLECharacteristic *characteristic = &family->characteristics[i];
        const ExpectedCharacteristic *want = &expected->characteristics[i];

        CheckUUID(family->name, "characteristic", i, &characteristic->uuid, want->uuid);
        BH_CHECK(characteristic->service == want->service && characteristic->service < family->nservices,
                 "%s characteristic %d is in service %d", family->name, i, (int)characteristic->service);
        BH_CHECK(characteristic->flags == want->flags, "%s characteristic %d has flags %02X", family->name, i,
                 (unsigned int)characteristic->flags);
    }
    BH_CHECK(family->pairing == expected->pairing, "%s pairs by rule %d", family->name, (int)family->pairing);
}

static void TestTranscription(void)
{
    int i;

    for (i = 0; i < SDL_BLE_FAMILY_COUNT; ++i) {
        CheckFamily(&expected_families[i]);
    }
    for (i = 0; i < COUNT(module_indices); ++i) {
        const SDL_BLEFamily *family = SDL_BLE_Families[module_indices[i].family];

        BH_CHECK(module_indices[i].index < family->ncharacteristics, "%s index %d", family->name, module_indices[i].index);
        if (module_indices[i].index < family->ncharacteristics) {
            CheckUUID(family->name, "module index", module_indices[i].index,
                      &family->characteristics[module_indices[i].index].uuid, module_indices[i].uuid);
        }
    }

    /* The helpers the table is written with */
    {
        SDL_BLEUUID a = SDL_BLE_UUID16(0xFE55);
        const SDL_BLEUUID b = SDL_BLE_UUID16_INIT(0xfe, 0x55);
        char text[40];

        FormatUUID(&a, text);
        CHECK(strcmp(text, "0000fe55-0000-1000-8000-00805f9b34fb") == 0);
        CHECK(SDL_BLE_UUIDEqual(&a, &b));
        a.bytes[15] ^= 1;
        CHECK(!SDL_BLE_UUIDEqual(&a, &b));
    }

    /* The names the log uses */
    CHECK(strcmp(SDL_BLE_ActionName(SDL_BLE_ACTION_CONNECT), "connect") == 0);
    CHECK(strcmp(SDL_BLE_ActionName(SDL_BLE_ACTION_PAIR), "pair") == 0);
    CHECK(strcmp(SDL_BLE_ActionName(SDL_BLE_ACTION_REMOVE_BOND), "remove bond") == 0);
    CHECK(strcmp(SDL_BLE_ActionName(SDL_BLE_ACTION_DISCOVER), "discover") == 0);
    CHECK(strcmp(SDL_BLE_ActionName(SDL_BLE_ACTION_SUBSCRIBE), "subscribe") == 0);
    CHECK(strcmp(SDL_BLE_ActionName(SDL_BLE_ACTION_READ), "read") == 0);
    CHECK(strcmp(SDL_BLE_ActionName(SDL_BLE_ACTION_WRITE), "write") == 0);
    CHECK(strcmp(SDL_BLE_ActionName(SDL_BLE_ACTION_PUBLISH), "publish") == 0);
    CHECK(strcmp(SDL_BLE_ActionName(SDL_BLE_ACTION_REMOVE), "remove") == 0);
    CHECK(strcmp(SDL_BLE_ActionName(SDL_BLE_ACTION_BACKOFF), "back off") == 0);
    CHECK(strcmp(SDL_BLE_ActionName(SDL_BLE_ACTION_DISCONNECT), "disconnect") == 0);
    CHECK(strcmp(SDL_BLE_ActionName(99), "?") == 0);
}

static int log_count;
static char log_text[64];

static void CountLog(void *userdata, const char *text)
{
    (void)userdata;
    ++log_count;
    snprintf(log_text, sizeof(log_text), "%s", text);
}

/* The contract helpers of SDL_ble_proto.c: UUID equality on every byte, the
   identity name cut to 63 bytes, the button range, control equality on the
   finger, the write queue's limits and ring, and the log */
static void TestContract(void)
{
    char name[SDL_BLE_NAME_LENGTH + 1];
    SDL_BLEIdentity identity;
    SDL_BLEControls a, b;
    SDL_BLEBase base;
    SDL_BLESink sink;
    SDL_BLEWrite write;
    SDL_BLEUUID u, v;
    uint8_t data[SDL_BLE_MAX_WRITE + 1];
    int i;

    /* Equality compares the first byte too */
    u = SDL_BLE_UUID16(0xFE55);
    v = u;
    v.bytes[0] ^= 1;
    CHECK(!SDL_BLE_UUIDEqual(&u, &v));

    /* A 64-byte name keeps 63 bytes and its terminator */
    memset(name, 'A', SDL_BLE_NAME_LENGTH);
    name[SDL_BLE_NAME_LENGTH] = '\0';
    SDL_BLE_SetIdentity(&identity, name, SDL_BLE_TYPE_GAMEPAD, 0, 0, true);
    CHECK(identity.name[SDL_BLE_NAME_LENGTH - 1] == '\0' && strlen(identity.name) == 63);

    /* Buttons 32 and -1 do not exist */
    memset(&a, 0, sizeof(a));
    SDL_BLE_SetButton(&a, 32, true);
    SDL_BLE_SetButton(&a, -1, true);
    CHECK(a.buttons == 0);
    a.buttons = 0x80000001u;
    CHECK(!SDL_BLE_GetButton(&a, 32) && !SDL_BLE_GetButton(&a, -1));
    CHECK(SDL_BLE_GetButton(&a, 0) && SDL_BLE_GetButton(&a, 31));

    /* The finger and its position count in equality */
    memset(&a, 0, sizeof(a));
    b = a;
    CHECK(SDL_BLE_ControlsEqual(&a, &b));
    b.finger = true;
    CHECK(!SDL_BLE_ControlsEqual(&a, &b));
    b = a;
    b.finger_x = 0.5f;
    CHECK(!SDL_BLE_ControlsEqual(&a, &b));
    b = a;
    b.finger_y = 0.5f;
    CHECK(!SDL_BLE_ControlsEqual(&a, &b));

    /* At most 16 writes of at most 80 bytes, to characteristics 0 to 7 */
    memset(&sink, 0, sizeof(sink));
    sink.log = CountLog;
    SDL_BLE_SetIdentity(&identity, "Contract", SDL_BLE_TYPE_GAMEPAD, 0, 0, true);
    SDL_BLE_ResetBase(&base, &sink, &identity);
    memset(data, 0x33, sizeof(data));
    for (i = 0; i < 16; ++i) {
        BH_CHECK(SDL_BLE_QueueWrite(&base, 0, data, 1, false), "write %d is queued", i);
    }
    CHECK(!SDL_BLE_QueueWrite(&base, 0, data, 1, false) && base.write_count == 16);
    SDL_BLE_ClearWrites(&base);
    CHECK(!SDL_BLE_QueueWrite(&base, 0, data, 81, false) && base.write_count == 0);
    CHECK(SDL_BLE_QueueWrite(&base, 0, data, 80, false) && SDL_BLE_NextWrite(&base, &write) && write.length == 80);
    CHECK(!SDL_BLE_QueueWrite(&base, 8, data, 1, false) && !SDL_BLE_QueueWrite(&base, -1, data, 1, false));
    CHECK(base.write_count == 0);

    /* The write ring wraps */
    SDL_BLE_ClearWrites(&base);
    for (i = 0; i < 40; ++i) {
        const uint8_t byte = (uint8_t)i;

        SDL_BLE_QueueWrite(&base, 1, &byte, 1, false);
        BH_CHECK(SDL_BLE_NextWrite(&base, &write) && write.length == 1 && write.data[0] == byte,
                 "write %d after the ring wrapped", i);
    }

    /* The log reaches the sink */
    log_count = 0;
    SDL_BLE_Log(&base, "contract text");
    CHECK(log_count == 1 && strcmp(log_text, "contract text") == 0);
}

typedef struct TimeRecord
{
    int count;
    char kind[16];     /* 'C' a change, 'S' a sample */
    uint64_t time_ns[16];
    uint64_t sensor_ns[16];
} TimeRecord;

static void RecordChange(void *userdata, const SDL_BLEControls *controls, uint64_t time_ns)
{
    TimeRecord *record = (TimeRecord *)userdata;

    (void)controls;
    if (record->count < COUNT(record->time_ns)) {
        record->kind[record->count] = 'C';
        record->time_ns[record->count] = time_ns;
        record->sensor_ns[record->count] = 0;
        ++record->count;
    }
}

static void RecordSample(void *userdata, int sensor, uint64_t time_ns, uint64_t sensor_ns, const float *data)
{
    TimeRecord *record = (TimeRecord *)userdata;

    (void)sensor;
    (void)data;
    if (record->count < COUNT(record->time_ns)) {
        record->kind[record->count] = 'S';
        record->time_ns[record->count] = time_ns;
        record->sensor_ns[record->count] = sensor_ns;
        ++record->count;
    }
}

/* The times a sink gets never go back: each change and sample is stamped
   at the later of its own time and the last one handed on. A commit that
   changes nothing and an event without a sink hand nothing on and leave the
   time where it was. The device's own sample time passes as it is. */
static void TestEmitTimes(void)
{
    TimeRecord record;
    SDL_BLEIdentity identity;
    SDL_BLEControls controls;
    SDL_BLEBase base;
    SDL_BLESink sink;

    memset(&record, 0, sizeof(record));
    memset(&sink, 0, sizeof(sink));
    sink.userdata = &record;
    sink.changed = RecordChange;
    sink.sensor = RecordSample;
    SDL_BLE_SetIdentity(&identity, "Times", SDL_BLE_TYPE_GAMEPAD, 1u << SDL_BLE_BUTTON_SOUTH, 0, true);
    SDL_BLE_ResetBase(&base, &sink, &identity);
    CHECK(base.emitted_ns == 0);
    controls = base.controls;
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_SOUTH, true);
    SDL_BLE_Commit(&base, &controls, 1000);
    SDL_BLE_Sensor(&base, SDL_BLE_SENSOR_ACCEL, 900, 12345, 1.0f, 2.0f, 3.0f);
    SDL_BLE_Sensor(&base, SDL_BLE_SENSOR_GYRO, 1500, 7, 0.0f, 0.0f, 0.0f);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_SOUTH, false);
    SDL_BLE_Commit(&base, &controls, 1200);
    SDL_BLE_Commit(&base, &controls, 3000);
    SDL_BLE_Sensor(&base, SDL_BLE_SENSOR_ACCEL, 2000, 8, 0.0f, 0.0f, 0.0f);
    SDL_BLE_Release(&base, 1999);
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_SOUTH, true);
    SDL_BLE_Commit(&base, &controls, 2000);
    CHECK(record.count == 6);
    if (record.count == 6) {
        CHECK(record.kind[0] == 'C' && record.time_ns[0] == 1000);
        CHECK(record.kind[1] == 'S' && record.time_ns[1] == 1000 && record.sensor_ns[1] == 12345);
        CHECK(record.kind[2] == 'S' && record.time_ns[2] == 1500 && record.sensor_ns[2] == 7);
        CHECK(record.kind[3] == 'C' && record.time_ns[3] == 1500);
        CHECK(record.kind[4] == 'S' && record.time_ns[4] == 2000 && record.sensor_ns[4] == 8);
        /* A time equal to the last one passes as it is */
        CHECK(record.kind[5] == 'C' && record.time_ns[5] == 2000);
    }
    /* A release later than the last time keeps its own */
    SDL_BLE_Release(&base, 2500);
    CHECK(record.count == 7 && record.kind[6] == 'C' && record.time_ns[6] == 2500 && base.emitted_ns == 2500);

    /* A reset starts over */
    SDL_BLE_ResetBase(&base, &sink, &identity);
    CHECK(base.emitted_ns == 0);
    controls = base.controls;
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_SOUTH, true);
    SDL_BLE_Commit(&base, &controls, 10);
    CHECK(record.count == 8 && record.time_ns[7] == 10);

    /* Without a change sink a commit hands nothing on, so a later sample
       keeps its own earlier time. Without a sample sink the same holds for a
       sample. */
    sink.changed = NULL;
    SDL_BLE_ResetBase(&base, &sink, &identity);
    controls = base.controls;
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_SOUTH, true);
    SDL_BLE_Commit(&base, &controls, 5000);
    SDL_BLE_Sensor(&base, SDL_BLE_SENSOR_ACCEL, 100, 9, 0.0f, 0.0f, 0.0f);
    CHECK(record.count == 9 && record.kind[8] == 'S' && record.time_ns[8] == 100 && base.changes == 1);
    sink.changed = RecordChange;
    sink.sensor = NULL;
    SDL_BLE_ResetBase(&base, &sink, &identity);
    SDL_BLE_Sensor(&base, SDL_BLE_SENSOR_ACCEL, 5000, 10, 0.0f, 0.0f, 0.0f);
    controls = base.controls;
    SDL_BLE_SetButton(&controls, SDL_BLE_BUTTON_SOUTH, true);
    SDL_BLE_Commit(&base, &controls, 100);
    CHECK(record.count == 10 && record.kind[9] == 'C' && record.time_ns[9] == 100);
}

/* Test 2: the same address matching twice during a connect produces one
   connect action. The advertisement and the scan response both match. */
static void Test2_OneConnect(void)
{
    SDL_BLEAdvertisement events[3];
    SDL_BLEHost host;
    SDL_BLEMatch match = { -1, 0 };
    BH_Harness *sessions[3] = { NULL, NULL, NULL };
    int nsessions = 0, connects = 0, i;

    events[0] = Ad(0x1234, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&events[0], "Daydream controller");
    events[1] = Ad(0x1234, SDL_BLE_AD_SCAN_RESPONSE);
    AdName(&events[1], "Daydream controller");
    AdService(&events[1], "0000fe55-0000-1000-8000-00805f9b34fb");
    events[2] = events[0];
    for (i = 0; i < 3; ++i) {
        BH_CHECK(Match(&events[i], NULL) == SDL_BLE_FAMILY_DAYDREAM, "event %d matches the Daydream", i);
    }

    SDL_BLEHost_Init(&host);
    for (i = 0; i < 3; ++i) {
        if (Offer(&host, &events[i], 1000 + (uint64_t)i, &match) && nsessions < 3) {
            sessions[nsessions] = BH_Create(SDL_BLE_Families[match.family], match.variant, NULL, false);
            sessions[nsessions]->now = 1000 + (uint64_t)i;
            connects += BH_Count(sessions[nsessions], CONNECT);
            ++nsessions;
        }
    }
    CHECK(nsessions == 1 && connects == 1 && match.family == SDL_BLE_FAMILY_DAYDREAM);
    if (nsessions != 1) {
        return;
    }
    EXPECT_LOG(sessions[0], 0, "connect");

    /* A failed connect ends the session without a backoff. The address waits
       SDL_BLE_CONNECT_RETRY_MS, and then the next advertisement connects
       again, once. */
    BH_Connected(sessions[0], false, false);
    EXPECT_LOG(sessions[0], 0, "connect disconnect");
    CHECK(!sessions[0]->session.backoff && SDL_BLESession_Ended(&sessions[0]->session));
    EXPECT_OUTCOME(sessions[0], END_CONNECT);
    Ended(&host, 0x1234, sessions[0]);
    for (i = 0; i < 3; ++i) {
        CHECK(!Offer(&host, &events[i], 5999, &match));
    }
    for (i = 0; i < 3; ++i) {
        if (Offer(&host, &events[i], 6000 + (uint64_t)i, &match) && nsessions < 3) {
            sessions[nsessions] = BH_Create(SDL_BLE_Families[match.family], match.variant, NULL, false);
            connects += BH_Count(sessions[nsessions], CONNECT);
            ++nsessions;
        }
    }
    CHECK(nsessions == 2 && connects == 2);
    for (i = 0; i < nsessions; ++i) {
        DESTROY(sessions[i]);
    }
}

/* Host: the reservation lasts the whole session, the backoff follows the
   Switch 2 driver's schedule of 15, 30, 60, 120, 240, then 300 s
   (SDL_ble_switch2joystick.c:304-337), a published session clears it, and a
   full table gives up an idle entry only */
static void TestHost(void)
{
    static const uint64_t schedule[] = { 15000, 30000, 60000, 120000, 240000, 300000, 300000, 300000 };
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad, scan, other, unknown, many[SDL_BLE_MAX_HOSTED + 1];
    SDL_BLEMatch match = { -1, 0 };
    bool enabled[SDL_BLE_FAMILY_COUNT];
    uint64_t t;
    int i;

    ad = Ad(0xA1, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&ad, "Pokemon PBP");
    scan = Ad(0xA1, SDL_BLE_AD_SCAN_RESPONSE);
    AdName(&scan, "Pokemon PBP");
    other = Ad(0xA2, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&other, "OMVR-V190");
    unknown = Ad(0xA3, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&unknown, "Keyboard");

    /* Reserved for the whole session */
    SDL_BLEHost_Init(&host);
    CHECK(!SDL_BLEHost_IsActive(&host, 0xA1));
    CHECK(Offer(&host, &ad, 1000, &match) && match.family == SDL_BLE_FAMILY_POKEBALL && match.variant == 0);
    CHECK(SDL_BLEHost_IsActive(&host, 0xA1));
    CHECK(!Offer(&host, &ad, 1000, &match));
    CHECK(!Offer(&host, &scan, 1001, &match));
    CHECK(!Offer(&host, &ad, 10000000, &match));
    /* Another address has its own entry */
    CHECK(Offer(&host, &other, 1002, &match) && match.family == SDL_BLE_FAMILY_OCULUSGO);
    CHECK(SDL_BLEHost_IsActive(&host, 0xA2) && host.count == 2);
    /* A record no enabled family takes reserves nothing */
    CHECK(!Offer(&host, &unknown, 1003, &match) && !SDL_BLEHost_IsActive(&host, 0xA3) && host.count == 2);
    for (i = 0; i < SDL_BLE_FAMILY_COUNT; ++i) {
        enabled[i] = (i != SDL_BLE_FAMILY_POKEBALL);
    }
    ad.address = 0xA4;
    CHECK(!SDL_BLEHost_Advertise(&host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, enabled, &ad, 1004, &match));
    CHECK(!SDL_BLEHost_IsActive(&host, 0xA4) && host.count == 2);
    ad.address = 0xA1;
    /* An end for an address the host does not hold changes nothing */
    EndWith(&host, 0xA9, END_BACKOFF, 1005);
    CHECK(host.count == 2);

    /* Released when the session ends */
    EndWith(&host, 0xA1, 0, 2000);
    CHECK(!SDL_BLEHost_IsActive(&host, 0xA1) && SDL_BLEHost_IsActive(&host, 0xA2));
    CHECK(Offer(&host, &ad, 2000, &match));

    /* The schedule, step by step, then held at 300 s */
    t = 10000;
    for (i = 0; i < COUNT(schedule); ++i) {
        EndWith(&host, 0xA1, END_BACKOFF, t);
        BH_CHECK(!SDL_BLEHost_IsActive(&host, 0xA1) && !Offer(&host, &ad, t, &match) &&
                     !Offer(&host, &ad, t + schedule[i] - 1, &match),
                 "backoff %d ends early", i);
        BH_CHECK(Offer(&host, &ad, t + schedule[i], &match), "backoff %d is not %d ms", i, (int)schedule[i]);
        t += schedule[i];
    }

    /* A published session clears the schedule */
    EndWith(&host, 0xA1, END_PUBLISHED, t);
    CHECK(Offer(&host, &ad, t, &match));
    EndWith(&host, 0xA1, END_BACKOFF, t);
    CHECK(!Offer(&host, &ad, t + 14999, &match) && Offer(&host, &ad, t + 15000, &match));
    t += 15000;
    EndWith(&host, 0xA1, END_BACKOFF, t);
    CHECK(!Offer(&host, &ad, t + 29999, &match) && Offer(&host, &ad, t + 30000, &match));
    t += 30000;
    /* A session that published and then backs off starts over at 15 s */
    EndWith(&host, 0xA1, END_BACKOFF | END_PUBLISHED, t);
    CHECK(!Offer(&host, &ad, t + 14999, &match) && Offer(&host, &ad, t + 15000, &match));
    t += 15000;
    /* An end without a backoff, as after a link loss, frees the address at
       once. Only a publish clears the count. */
    EndWith(&host, 0xA1, 0, t);
    CHECK(Offer(&host, &ad, t, &match));
    EndWith(&host, 0xA1, END_BACKOFF, t);
    CHECK(!Offer(&host, &ad, t + 29999, &match) && Offer(&host, &ad, t + 30000, &match));

    /* The count stops at 255: the 257th backoff in a row still waits 300 s */
    SDL_BLEHost_Init(&host);
    CHECK(Offer(&host, &ad, 0, &match));
    t = 0;
    for (i = 0; i < 256; ++i) {
        EndWith(&host, 0xA1, END_BACKOFF, t);
        t += 300000;
    }
    EndWith(&host, 0xA1, END_BACKOFF, t);
    CHECK(host.entries[0].failures == 255);
    CHECK(!Offer(&host, &ad, t + 299999, &match) && Offer(&host, &ad, t + 300000, &match));

    /* A full table: 16 sessions, as many as the Switch 2 driver's connect
       and cooldown arrays hold, then an address that finds no entry */
    CHECK(SDL_BLE_MAX_HOSTED == 16);
    SDL_BLEHost_Init(&host);
    for (i = 0; i <= SDL_BLE_MAX_HOSTED; ++i) {
        many[i] = Ad(0x100 + (uint64_t)i, SDL_BLE_AD_ADVERTISEMENT);
        AdName(&many[i], "Ble Guitar");
    }
    for (i = 0; i < SDL_BLE_MAX_HOSTED; ++i) {
        BH_CHECK(Offer(&host, &many[i], 0, &match), "address %d", i);
    }
    CHECK(host.count == SDL_BLE_MAX_HOSTED);
    CHECK(!Offer(&host, &many[SDL_BLE_MAX_HOSTED], 0, &match) && !SDL_BLEHost_IsActive(&host, 0x110));
    /* An entry backing off is not given up */
    EndWith(&host, 0x100, END_BACKOFF, 0);
    CHECK(!Offer(&host, &many[SDL_BLE_MAX_HOSTED], 14999, &match));
    /* Once its backoff is over, the idle entry goes to the new address */
    CHECK(Offer(&host, &many[SDL_BLE_MAX_HOSTED], 15000, &match) && SDL_BLEHost_IsActive(&host, 0x110));
    CHECK(host.count == SDL_BLE_MAX_HOSTED);
    /* The address that lost its entry finds none while every other is busy */
    CHECK(!Offer(&host, &many[0], 15000, &match) && !SDL_BLEHost_IsActive(&host, 0x100));
    /* The new address keeps nothing of the old one's schedule */
    EndWith(&host, 0x110, END_BACKOFF, 16000);
    CHECK(!Offer(&host, &many[SDL_BLE_MAX_HOSTED], 30999, &match) && Offer(&host, &many[SDL_BLE_MAX_HOSTED], 31000, &match));
    /* An idle entry that never backed off is given up at once, and the
       returning address starts a fresh schedule */
    EndWith(&host, 0x101, 0, 15000);
    CHECK(Offer(&host, &many[0], 15000, &match) && SDL_BLEHost_IsActive(&host, 0x100) && !SDL_BLEHost_IsActive(&host, 0x101));
    EndWith(&host, 0x100, END_BACKOFF, 20000);
    CHECK(!Offer(&host, &many[0], 34999, &match) && Offer(&host, &many[0], 35000, &match));
}

/* Host: failed pairings, failed connects and SDL_BLEHost_ClearBackoff */
static void TestHostOutcomes(void)
{
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad, other, third;
    SDL_BLEMatch match = { -1, 0 };
    uint64_t t;
    int i;

    ad = Ad(0xA1, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&ad, "OMVR-V190");
    other = Ad(0xA2, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&other, "Daydream controller");
    third = Ad(0xA3, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&third, "Ble Guitar");

    /* Failed pairings: the first two free the address at once, the third
       arms the schedule, and so does each after it until a publish */
    CHECK(SDL_BLE_PAIRING_TRIES == 3);
    SDL_BLEHost_Init(&host);
    CHECK(Offer(&host, &ad, 0, &match));
    EndWith(&host, 0xA1, END_PAIRING, 1000);
    CHECK(!SDL_BLEHost_IsActive(&host, 0xA1) && Offer(&host, &ad, 1000, &match));
    EndWith(&host, 0xA1, END_PAIRING, 2000);
    CHECK(Offer(&host, &ad, 2000, &match));
    EndWith(&host, 0xA1, END_PAIRING, 3000);
    CHECK(!SDL_BLEHost_IsActive(&host, 0xA1) && !Offer(&host, &ad, 3000, &match));
    CHECK(!Offer(&host, &ad, 17999, &match) && Offer(&host, &ad, 18000, &match));
    EndWith(&host, 0xA1, END_PAIRING, 20000);
    CHECK(!Offer(&host, &ad, 49999, &match) && Offer(&host, &ad, 50000, &match));
    /* Another address counts its own */
    CHECK(Offer(&host, &other, 50000, &match));
    EndWith(&host, 0xA2, END_PAIRING, 50000);
    CHECK(Offer(&host, &other, 50000, &match));
    /* Other ends keep the count, and a backoff takes the next step: 60 s */
    EndWith(&host, 0xA1, 0, 50000);
    CHECK(Offer(&host, &ad, 50000, &match));
    EndWith(&host, 0xA1, END_BACKOFF, 50000);
    CHECK(!Offer(&host, &ad, 109999, &match) && Offer(&host, &ad, 110000, &match));
    EndWith(&host, 0xA1, END_PAIRING, 110000);
    CHECK(!Offer(&host, &ad, 229999, &match) && Offer(&host, &ad, 230000, &match));
    /* A publish clears the count and the schedule: two failed pairings free
       the address at once again, and the third waits 15 s */
    EndWith(&host, 0xA1, END_PUBLISHED, 230000);
    t = 230000;
    for (i = 0; i < SDL_BLE_PAIRING_TRIES - 1; ++i) {
        BH_CHECK(Offer(&host, &ad, t, &match), "failed pairing %d after the publish", i);
        EndWith(&host, 0xA1, END_PAIRING, t);
    }
    CHECK(Offer(&host, &ad, t, &match));
    EndWith(&host, 0xA1, END_PAIRING, t);
    CHECK(!Offer(&host, &ad, t + 14999, &match) && Offer(&host, &ad, t + 15000, &match));
    /* The count stops at 255: the 257th failed pairing in a row still waits */
    SDL_BLEHost_Init(&host);
    CHECK(Offer(&host, &ad, 0, &match));
    t = 0;
    for (i = 0; i < 256; ++i) {
        EndWith(&host, 0xA1, END_PAIRING, t);
        t += 300000;
    }
    CHECK(host.entries[0].pairing_failures == 255);
    EndWith(&host, 0xA1, END_PAIRING, t);
    CHECK(!Offer(&host, &ad, t + 299999, &match) && Offer(&host, &ad, t + 300000, &match));

    /* Failed connects: a fixed SDL_BLE_CONNECT_RETRY_MS however often, and
       the schedule does not advance */
    CHECK(SDL_BLE_CONNECT_RETRY_MS == 5000);
    SDL_BLEHost_Init(&host);
    CHECK(Offer(&host, &ad, 0, &match));
    t = 1000;
    for (i = 0; i < 4; ++i) {
        EndWith(&host, 0xA1, END_CONNECT, t);
        BH_CHECK(!SDL_BLEHost_IsActive(&host, 0xA1) && !Offer(&host, &ad, t, &match) && !Offer(&host, &ad, t + 4999, &match),
                 "failed connect %d waits less than 5 s", i);
        BH_CHECK(Offer(&host, &ad, t + 5000, &match), "failed connect %d waits more than 5 s", i);
        t += 5000;
    }
    EndWith(&host, 0xA1, END_BACKOFF, t);
    CHECK(!Offer(&host, &ad, t + 14999, &match) && Offer(&host, &ad, t + 15000, &match));
    t += 15000;
    /* After a backoff a failed connect still waits 5 s, and the next backoff
       is the next step, 30 s */
    EndWith(&host, 0xA1, END_CONNECT, t);
    CHECK(!Offer(&host, &ad, t + 4999, &match) && Offer(&host, &ad, t + 5000, &match));
    t += 5000;
    EndWith(&host, 0xA1, END_BACKOFF, t);
    CHECK(!Offer(&host, &ad, t + 29999, &match) && Offer(&host, &ad, t + 30000, &match));
    t += 30000;
    /* With a backoff, the backoff's wait applies: 60 s */
    EndWith(&host, 0xA1, END_BACKOFF | END_CONNECT, t);
    CHECK(!Offer(&host, &ad, t + 59999, &match) && Offer(&host, &ad, t + 60000, &match));
    t += 60000;
    /* A failed connect keeps the failed pairings: the third after it waits
       the next step, 120 s */
    for (i = 0; i < SDL_BLE_PAIRING_TRIES - 1; ++i) {
        EndWith(&host, 0xA1, END_PAIRING, t);
        BH_CHECK(Offer(&host, &ad, t, &match), "failed pairing %d before the failed connect", i);
    }
    EndWith(&host, 0xA1, END_CONNECT, t);
    CHECK(!Offer(&host, &ad, t + 4999, &match) && Offer(&host, &ad, t + 5000, &match));
    t += 5000;
    EndWith(&host, 0xA1, END_PAIRING, t);
    CHECK(!Offer(&host, &ad, t + 119999, &match) && Offer(&host, &ad, t + 120000, &match));

    /* SDL_BLEHost_ClearBackoff: every address without a session may start
       one at once, and its schedule and its failed pairings start over. The
       address with a session keeps its reservation and its count. The idle
       entries sit first and last in the table. */
    SDL_BLEHost_Init(&host);
    SDL_BLEHost_ClearBackoff(&host);
    CHECK(host.count == 0);
    CHECK(Offer(&host, &ad, 0, &match));
    EndWith(&host, 0xA1, END_BACKOFF, 0);
    CHECK(Offer(&host, &third, 0, &match));
    EndWith(&host, 0xA3, END_BACKOFF, 0);
    for (i = 0; i < SDL_BLE_PAIRING_TRIES - 1; ++i) {
        CHECK(Offer(&host, &other, 15000, &match));
        EndWith(&host, 0xA2, END_PAIRING, 15000);
    }
    CHECK(Offer(&host, &other, 15000, &match));
    EndWith(&host, 0xA2, END_CONNECT, 15000);
    CHECK(Offer(&host, &ad, 15000, &match));
    EndWith(&host, 0xA1, END_BACKOFF, 15000);
    CHECK(Offer(&host, &third, 15000, &match));
    CHECK(host.count == 3 && host.entries[0].address == 0xA1 && host.entries[1].address == 0xA3 &&
          host.entries[2].address == 0xA2);
    CHECK(!Offer(&host, &ad, 15001, &match) && !Offer(&host, &other, 15001, &match));
    SDL_BLEHost_ClearBackoff(&host);
    CHECK(host.count == 3);
    CHECK(Offer(&host, &ad, 15001, &match) && Offer(&host, &other, 15001, &match));
    CHECK(SDL_BLEHost_IsActive(&host, 0xA3) && !Offer(&host, &third, 15001, &match));
    /* The first starts its schedule over at 15 s */
    EndWith(&host, 0xA1, END_BACKOFF, 16000);
    CHECK(!Offer(&host, &ad, 30999, &match) && Offer(&host, &ad, 31000, &match));
    /* The last has two failed pairings to spend again */
    for (i = 0; i < SDL_BLE_PAIRING_TRIES - 1; ++i) {
        EndWith(&host, 0xA2, END_PAIRING, 16000);
        BH_CHECK(Offer(&host, &other, 16000, &match), "failed pairing %d after the clear", i);
    }
    EndWith(&host, 0xA2, END_PAIRING, 16000);
    CHECK(!Offer(&host, &other, 30999, &match) && Offer(&host, &other, 31000, &match));
    /* The one with a session kept its count: its next backoff is 30 s */
    EndWith(&host, 0xA3, END_BACKOFF, 16000);
    CHECK(!Offer(&host, &third, 45999, &match) && Offer(&host, &third, 46000, &match));
}

/* The host and the session together, advertisements coming every 20 ms: a
   device whose pairing always fails is tried SDL_BLE_PAIRING_TRIES times at
   once, then waits the schedule. An address that cannot be opened, or whose
   link drops before the joystick appears, is tried every
   SDL_BLE_CONNECT_RETRY_MS. One whose link drops while its stale bond is
   removed is tried again at once. */
static void TestRetries(void)
{
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad;
    SDL_BLEMatch match = { -1, 0 };
    uint64_t now, started[8];
    int sessions = 0;

    SDL_BLEHost_Init(&host);
    ad = Ad(0xD5, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&ad, "Daydream controller");
    for (now = 5000; now < 30000; now += 20) {
        if (Offer(&host, &ad, now, &match)) {
            BH_Harness *h = BH_Create(SDL_BLE_Families[match.family], match.variant, NULL, true);

            h->now = now;
            BH_Connected(h, true, false);
            BH_Paired(h, false);
            Ended(&host, ad.address, h);
            if (sessions < COUNT(started)) {
                started[sessions] = now;
            }
            ++sessions;
            DESTROY(h);
        }
    }
    CHECK(sessions == SDL_BLE_PAIRING_TRIES + 1);
    if (sessions == SDL_BLE_PAIRING_TRIES + 1) {
        CHECK(started[0] == 5000 && started[1] == 5020 && started[2] == 5040 && started[3] == 5040 + 15000);
    }

    SDL_BLEHost_Init(&host);
    ad.address = 0xD6;
    sessions = 0;
    for (now = 0; now < 20000; now += 20) {
        if (Offer(&host, &ad, now, &match)) {
            BH_Harness *h = BH_Create(SDL_BLE_Families[match.family], match.variant, NULL, true);

            h->now = now;
            BH_Connected(h, false, false);
            Ended(&host, ad.address, h);
            if (sessions < COUNT(started)) {
                started[sessions] = now;
            }
            ++sessions;
            DESTROY(h);
        }
    }
    CHECK(sessions == 4);
    if (sessions == 4) {
        CHECK(started[0] == 0 && started[1] == 5000 && started[2] == 10000 && started[3] == 15000);
    }

    /* A link that drops during discovery each time waits the same fixed
       SDL_BLE_CONNECT_RETRY_MS, which does not grow, and a backoff after
       those losses still takes the first step of the schedule */
    SDL_BLEHost_Init(&host);
    ad.address = 0xD7;
    sessions = 0;
    for (now = 0; now < 20000; now += 20) {
        if (Offer(&host, &ad, now, &match)) {
            BH_Harness *h = BH_Create(SDL_BLE_Families[match.family], match.variant, NULL, true);

            h->now = now;
            BH_Connected(h, true, true);
            Lost(h);
            Ended(&host, ad.address, h);
            if (sessions < COUNT(started)) {
                started[sessions] = now;
            }
            ++sessions;
            DESTROY(h);
        }
    }
    CHECK(sessions == 4);
    if (sessions == 4) {
        CHECK(started[0] == 0 && started[1] == 5000 && started[2] == 10000 && started[3] == 15000);
    }
    CHECK(Offer(&host, &ad, 20000, &match));
    EndWith(&host, ad.address, END_BACKOFF, 20000);
    CHECK(!Offer(&host, &ad, 34999, &match) && Offer(&host, &ad, 35000, &match));

    /* A link that drops while a stale bond is removed: the next
       advertisement connects at once, so the next session pairs afresh */
    SDL_BLEHost_Init(&host);
    ad.address = 0xD8;
    sessions = 0;
    for (now = 0; now < 100; now += 20) {
        if (Offer(&host, &ad, now, &match)) {
            BH_Harness *h = BH_Create(SDL_BLE_Families[match.family], match.variant, NULL, true);

            h->now = now;
            BH_Connected(h, true, true);
            BH_Discovered(h, true, NULL);
            BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
            BH_CHECK(BH_LastKind(h) == REMOVE_BOND, "session %d removes the bond", sessions);
            Lost(h);
            Ended(&host, ad.address, h);
            if (sessions < COUNT(started)) {
                started[sessions] = now;
            }
            ++sessions;
            DESTROY(h);
        }
    }
    CHECK(sessions == 5);
    if (sessions == 5) {
        CHECK(started[0] == 0 && started[1] == 20 && started[4] == 80);
    }

    /* A link that drops while the Oculus Go pairs after a failed
       subscription counts as a failed pairing: two retries at once, then
       the schedule, as a refused pairing gets */
    SDL_BLEHost_Init(&host);
    ad = Ad(0xD9, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&ad, "OMVR test");
    sessions = 0;
    for (now = 0; now < 20000; now += 20) {
        if (Offer(&host, &ad, now, &match)) {
            BH_Harness *h = BH_Create(SDL_BLE_Families[match.family], match.variant, NULL, true);

            h->now = now;
            BH_Connected(h, true, false);
            BH_Discovered(h, true, NULL);
            BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
            BH_CHECK(BH_LastKind(h) == PAIR, "session %d pairs", sessions);
            Lost(h);
            Ended(&host, ad.address, h);
            if (sessions < COUNT(started)) {
                started[sessions] = now;
            }
            ++sessions;
            DESTROY(h);
        }
    }
    CHECK(sessions == SDL_BLE_PAIRING_TRIES + 1);
    if (sessions == SDL_BLE_PAIRING_TRIES + 1) {
        CHECK(started[0] == 0 && started[1] == 20 && started[2] == 40 && started[3] == 40 + 15000);
    }

    /* A link that drops while the Daydream pairs before discovery waits
       the fixed SDL_BLE_CONNECT_RETRY_MS twice, then the schedule */
    SDL_BLEHost_Init(&host);
    ad = Ad(0xDA, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&ad, "Daydream controller");
    sessions = 0;
    for (now = 0; now < 30000; now += 20) {
        if (Offer(&host, &ad, now, &match)) {
            BH_Harness *h = BH_Create(SDL_BLE_Families[match.family], match.variant, NULL, true);

            h->now = now;
            BH_Connected(h, true, false);
            BH_CHECK(BH_LastKind(h) == PAIR, "session %d pairs", sessions);
            Lost(h);
            Ended(&host, ad.address, h);
            if (sessions < COUNT(started)) {
                started[sessions] = now;
            }
            ++sessions;
            DESTROY(h);
        }
    }
    CHECK(sessions == SDL_BLE_PAIRING_TRIES + 1);
    if (sessions == SDL_BLE_PAIRING_TRIES + 1) {
        CHECK(started[0] == 0 && started[1] == 5000 && started[2] == 10000 && started[3] == 10000 + 15000);
    }
}

/* Queue: first in, first out across the ring's wrap, the part's capacity,
   and a loss marking the next queued value */
static void TestQueue(void)
{
    SDL_BLEValue *entries = (SDL_BLEValue *)calloc(SDL_BLE_QUEUE_CAPACITY, sizeof(SDL_BLEValue));
    SDL_BLEValueQueue queue;
    SDL_BLEValue value;
    uint8_t data[SDL_BLE_MAX_VALUE + 2];
    int i;

    if (!entries) {
        printf("out of memory\n");
        exit(2);
    }
    for (i = 0; i < (int)sizeof(data); ++i) {
        data[i] = (uint8_t)(i * 7 + 1);
    }

    /* Order across the wrap of a ring of 4 */
    SDL_BLE_InitQueue(&queue, entries, 4, SDL_BLE_MAX_VALUE);
    for (i = 0; i < 3; ++i) {
        data[0] = (uint8_t)i;
        CHECK(SDL_BLE_PushValue(&queue, i % 2, data, 3, (uint64_t)i) == SDL_BLE_PUSH_QUEUED);
    }
    for (i = 0; i < 2; ++i) {
        CHECK(SDL_BLE_PopValue(&queue, &value) && value.data[0] == i && value.time_ns == (uint64_t)i && !value.gap);
        CHECK(value.characteristic == i % 2 && value.length == 3 && value.data[1] == data[1] && value.data[2] == data[2]);
    }
    for (i = 3; i < 6; ++i) {
        data[0] = (uint8_t)i;
        CHECK(SDL_BLE_PushValue(&queue, 0, data, 3, (uint64_t)i) == SDL_BLE_PUSH_QUEUED);
    }
    CHECK(queue.count == 4);
    data[0] = 6;
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 3, 6) == SDL_BLE_PUSH_FULL && queue.full == 1 && queue.count == 4);
    for (i = 2; i < 6; ++i) {
        CHECK(SDL_BLE_PopValue(&queue, &value) && value.data[0] == i && value.time_ns == (uint64_t)i && !value.gap);
    }
    CHECK(!SDL_BLE_PopValue(&queue, &value) && queue.count == 0);
    /* The first value queued after the loss carries the gap, the next does not */
    data[0] = 7;
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 3, 7) == SDL_BLE_PUSH_QUEUED);
    data[0] = 8;
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 3, 8) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PopValue(&queue, &value) && value.data[0] == 7 && value.gap);
    CHECK(SDL_BLE_PopValue(&queue, &value) && value.data[0] == 8 && !value.gap);

    /* The part's capacity. Every loss counts, one gap covers them all, and
       the gap lands after every value queued before the loss. */
    SDL_BLE_InitQueue(&queue, entries, SDL_BLE_QUEUE_CAPACITY, SDL_BLE_MAX_VALUE);
    CHECK(SDL_BLE_QUEUE_CAPACITY == 1024 && queue.capacity == 1024);
    for (i = 0; i < SDL_BLE_QUEUE_CAPACITY; ++i) {
        data[0] = (uint8_t)i;
        data[1] = (uint8_t)(i >> 8);
        BH_CHECK(SDL_BLE_PushValue(&queue, 0, data, 2, (uint64_t)i) == SDL_BLE_PUSH_QUEUED, "value %d", i);
    }
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 2, 5000) == SDL_BLE_PUSH_FULL);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 2, 5001) == SDL_BLE_PUSH_FULL);
    CHECK(queue.full == 2 && queue.count == SDL_BLE_QUEUE_CAPACITY && queue.gap);
    CHECK(SDL_BLE_PopValue(&queue, &value) && value.data[0] == 0 && value.data[1] == 0 && !value.gap);
    data[0] = 0xAA;
    data[1] = 0xBB;
    CHECK(SDL_BLE_PushValue(&queue, 1, data, 2, 6000) == SDL_BLE_PUSH_QUEUED && !queue.gap);
    for (i = 1; i < SDL_BLE_QUEUE_CAPACITY; ++i) {
        BH_CHECK(SDL_BLE_PopValue(&queue, &value) && value.data[0] == (uint8_t)i && value.data[1] == (uint8_t)(i >> 8) &&
                     value.time_ns == (uint64_t)i && !value.gap,
                 "value %d out of order", i);
    }
    CHECK(SDL_BLE_PopValue(&queue, &value) && value.characteristic == 1 && value.data[0] == 0xAA && value.gap &&
          value.time_ns == 6000);
    CHECK(!SDL_BLE_PopValue(&queue, &value));

    /* A value longer than the value size is dropped, counted and marks a
       gap. It is never cut short. */
    SDL_BLE_InitQueue(&queue, entries, 4, 68);
    CHECK(queue.value_size == 68);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 68, 1) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 69, 2) == SDL_BLE_PUSH_TOO_LONG && queue.too_long == 1 && queue.count == 1);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 5, 3) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PopValue(&queue, &value) && value.length == 68 && !value.gap && memcmp(value.data, data, 68) == 0);
    CHECK(SDL_BLE_PopValue(&queue, &value) && value.length == 5 && value.gap && value.time_ns == 3);

    /* An empty value is ignored and counted, without a gap */
    SDL_BLE_InitQueue(&queue, entries, 4, 68);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 0, 1) == SDL_BLE_PUSH_EMPTY);
    CHECK(SDL_BLE_PushValue(&queue, 0, NULL, 4, 2) == SDL_BLE_PUSH_EMPTY);
    CHECK(queue.empty == 2 && queue.count == 0 && !queue.gap);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 1, 3) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PopValue(&queue, &value) && !value.gap && value.length == 1);
    /* An empty value leaves a pending gap where it is */
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 69, 4) == SDL_BLE_PUSH_TOO_LONG);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 0, 5) == SDL_BLE_PUSH_EMPTY && queue.gap);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 1, 6) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PopValue(&queue, &value) && value.gap && value.time_ns == 6);
    CHECK(queue.full == 0 && queue.too_long == 1 && queue.empty == 3);

    /* The value size stops at the entry size */
    SDL_BLE_InitQueue(&queue, entries, 4, 4096);
    CHECK(queue.value_size == SDL_BLE_MAX_VALUE);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, SDL_BLE_MAX_VALUE, 1) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, SDL_BLE_MAX_VALUE + 1, 2) == SDL_BLE_PUSH_TOO_LONG);
    CHECK(SDL_BLE_PopValue(&queue, &value) && value.length == SDL_BLE_MAX_VALUE &&
          memcmp(value.data, data, SDL_BLE_MAX_VALUE) == 0);

    /* Clearing empties the queue and drops a pending gap, and keeps the counts */
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 1, 3) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, SDL_BLE_MAX_VALUE + 1, 4) == SDL_BLE_PUSH_TOO_LONG && queue.gap);
    SDL_BLE_ClearQueue(&queue);
    CHECK(queue.count == 0 && !queue.gap && queue.too_long == 2);
    CHECK(!SDL_BLE_PopValue(&queue, &value));
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 1, 5) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PopValue(&queue, &value) && !value.gap && value.time_ns == 5);

    /* An entry holds 128 bytes */
    SDL_BLE_InitQueue(&queue, entries, 4, 4096);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 128, 1) == SDL_BLE_PUSH_QUEUED);

    /* A ring without room drops everything, and a capacity below 0 gives
       none */
    SDL_BLE_InitQueue(&queue, entries, 0, SDL_BLE_MAX_VALUE);
    CHECK(SDL_BLE_PushValue(&queue, 0, data, 1, 1) == SDL_BLE_PUSH_FULL && !SDL_BLE_PopValue(&queue, &value));
    SDL_BLE_InitQueue(&queue, entries, -1, SDL_BLE_MAX_VALUE);
    CHECK(queue.capacity == 0 && SDL_BLE_PushValue(&queue, 0, data, 1, 1) == SDL_BLE_PUSH_FULL);

    free(entries);
}

/* Test 3: a characteristic with Notify gets value 1, one with only Indicate
   gets value 2, one with neither fails the session */
static void Test3_DescriptorValues(void)
{
    uint8_t properties[SDL_BLE_MAX_CHARS];
    BH_Harness *h;

    CHECK(SDL_BLE_CCCD_NONE == 0 && SDL_BLE_CCCD_NOTIFY == 1 && SDL_BLE_CCCD_INDICATE == 2);

    /* Zwift: ASYNC offers both and gets Notify, SYNC_TX indicates only
       (zwiftplay README.md:31) */
    h = BH_Create(&SDL_BLEZwiftFamily, SDL_ZWIFT_PLAY_RIGHT, NULL, false);
    memset(properties, 0, sizeof(properties));
    properties[SDL_ZWIFT_ASYNC] = SDL_BLE_PROPERTY_NOTIFY | SDL_BLE_PROPERTY_INDICATE;
    properties[SDL_ZWIFT_SYNC_TX] = SDL_BLE_PROPERTY_INDICATE | SDL_BLE_PROPERTY_READ;
    properties[SDL_ZWIFT_SYNC_RX] = SDL_BLE_PROPERTY_WRITE | SDL_BLE_PROPERTY_WRITE_WITHOUT_RESPONSE;
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=indicate write2:526964654f6e");
    CHECK(h->actions[2].action.cccd == 1 && h->actions[3].action.cccd == 2);
    DESTROY(h);

    /* Gear VR: a data characteristic that neither notifies nor indicates */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, true);
    memset(properties, 0, sizeof(properties));
    properties[SDL_GEARVR_DATA] = SDL_BLE_PROPERTY_READ | SDL_BLE_PROPERTY_WRITE;
    properties[SDL_GEARVR_COMMAND] = SDL_BLE_PROPERTY_WRITE;
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    EXPECT_LOG(h, 0, "connect discover backoff disconnect");
    CHECK(h->session.backoff && SDL_BLESession_Ended(&h->session) && !h->published);
    DESTROY(h);
}

/* Test 4: for a bonded device the descriptor writes are None then the wanted
   value, in that order, for each characteristic */
static void Test4_BondedNoneFirst(void)
{
    static const uint8_t rest[SDL_GHLIOS_REPORT_SIZE] = { 0x00, 0x00, 0x0F, 0x00, 0x80, 0x00, 0x80, 0, 0, 0,
                                                          0,    0,    0,    0,    0,    0,    0,    0, 0, 0x80 };
    uint8_t properties[SDL_BLE_MAX_CHARS];
    BH_Harness *h;

    h = BH_Create(&SDL_BLEGHLiOSFamily, 0, NULL, false);
    BH_Start(h, true);
    EXPECT_LOG(h, 0, "connect discover sub0=none sub0=notify");
    CHECK(Phase(h) == SDL_BLE_PHASE_STARTING && !h->session.waiting);
    BH_Value(h, SDL_GHLIOS_INPUT, rest, sizeof(rest), false);
    EXPECT_LOG(h, 0, "connect discover sub0=none sub0=notify publish");
    DESTROY(h);

    /* An indicate characteristic gets None, then Indicate */
    h = BH_Create(&SDL_BLEZwiftFamily, SDL_ZWIFT_CLICK, NULL, false);
    memset(properties, 0, sizeof(properties));
    properties[SDL_ZWIFT_ASYNC] = SDL_BLE_PROPERTY_NOTIFY;
    properties[SDL_ZWIFT_SYNC_TX] = SDL_BLE_PROPERTY_INDICATE;
    properties[SDL_ZWIFT_SYNC_RX] = SDL_BLE_PROPERTY_WRITE;
    BH_Connected(h, true, true);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=none sub0=notify sub1=none sub1=indicate write2:526964654f6e");
    DESTROY(h);

    /* The wanted value follows only a None that succeeded */
    h = BH_Create(&SDL_BLEGHLiOSFamily, 0, NULL, false);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_FAILED, 0);
    EXPECT_LOG(h, 0, "connect discover sub0=none backoff disconnect");
    DESTROY(h);
}

/* Test 5: pairing. With the hint on, an on-error device whose descriptor
   write fails with InsufficientEncryption or InsufficientAuthentication gets
   pair, then one repeat of the subscriptions, and a second failure backs
   off. A pair-first device without a bond pairs before discovery and one
   with a bond does not. A stale bond is removed once, with the hint on,
   when Unreachable reveals it, and when one of those security errors does
   on a bond this session did not make, after which the device pairs afresh.
   With the hint off, no pair or bond removal appears and an unbonded
   pair-first device is never published. */
static void Test5_Pairing(void)
{
    static const uint8_t capture7[20] = { 0x26, 0xF8, 0x03, 0x61, 0xC2, 0x00, 0x20, 0x02, 0xC4, 0x0F,
                                          0xFE, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x11 };
    static const uint8_t battery = 0x50;
    /* 06 and 0E sit beside the two codes that pair (Bluetooth Core Vol 3
       Part F 3.4.1.1) */
    static const uint8_t other_errors[] = { 0x03, 0x06, 0x08, 0x0E };
    static const SDL_BLEStatus other_statuses[] = { SDL_BLE_STATUS_ACCESS_DENIED, SDL_BLE_STATUS_FAILED,
                                                    SDL_BLE_STATUS_UNREACHABLE };
    static const SDL_BLEStatus bonded_statuses[] = { SDL_BLE_STATUS_ACCESS_DENIED, SDL_BLE_STATUS_FAILED };
    uint8_t face[SDL_OCULUSGO_REPORT_SIZE];
    BH_Harness *h;
    int i;

    /* On error: Oculus Go, InsufficientEncryption on its first descriptor */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    EXPECT_LOG(h, 0, "connect discover sub0=notify pair");
    CHECK(Phase(h) == SDL_BLE_PHASE_REPAIRING);
    BH_Paired(h, true);
    BH_SubscribeAll(h);
    /* The repeat runs on the new bond, None first */
    EXPECT_LOG(h, 0, "connect discover sub0=notify pair sub0=none sub0=notify sub1=none sub1=notify");
    CHECK(Phase(h) == SDL_BLE_PHASE_STARTING && h->session.paired && h->session.bonded && h->session.repeated);
    OculusGoFace(face, 0);
    BH_Value(h, SDL_OCULUSGO_FACE, face, sizeof(face), false);
    EXPECT_LOG(h, 7, "sub1=notify publish");
    DESTROY(h);

    /* A failure on a later characteristic repeats every subscription from
       the first. The Oculus Go's 8126BEEF is optional, and a failed
       optional characteristic is skipped, so this runs on a copy of its
       family that requires it. */
    {
        static SDL_BLEFamily required;

        required = SDL_BLEOculusGoFamily;
        required.characteristics[SDL_OCULUSGO_BEEF].flags = SDL_BLE_CHAR_SUBSCRIBE;
        h = BH_Create(&required, 0, NULL, true);
    }
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION);
    BH_Paired(h, true);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify pair sub0=none sub0=notify sub1=none sub1=notify");
    DESTROY(h);

    /* A second failure backs off, with one pair in the session */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    BH_Paired(h, true);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    EXPECT_LOG(h, 0, "connect discover sub0=notify pair sub0=none sub0=notify backoff disconnect");
    CHECK(h->session.backoff && !h->session.published && BH_Count(h, PAIR) == 1);
    DESTROY(h);

    /* On error: Gear VR, InsufficientAuthentication */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION);
    BH_Paired(h, true);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify pair sub0=none sub0=notify write1:0800+r");
    DESTROY(h);

    /* Other errors do not pair */
    for (i = 0; i < COUNT(other_errors); ++i) {
        h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
        BH_Connected(h, true, false);
        BH_Discovered(h, true, NULL);
        BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, other_errors[i]);
        EXPECT_LOG(h, 0, "connect discover sub0=notify backoff disconnect");
        DESTROY(h);
    }
    for (i = 0; i < COUNT(other_statuses); ++i) {
        h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
        BH_Connected(h, true, false);
        BH_Discovered(h, true, NULL);
        BH_Subscribed(h, other_statuses[i], SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
        EXPECT_LOG(h, 0, "connect discover sub0=notify backoff disconnect");
        DESTROY(h);
    }
    /* A family that works unpaired never pairs */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION);
    EXPECT_LOG(h, 0, "connect discover sub0=notify backoff disconnect");
    DESTROY(h);

    /* Pair first: the Daydream without a bond pairs before discovery, then
       subscribes on the bond, and capture 7 decodes (Daydream test 12) */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    EXPECT_LOG(h, 0, "connect pair");
    CHECK(Phase(h) == SDL_BLE_PHASE_PAIRING);
    BH_Paired(h, true);
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    BH_ReadAll(h, &battery, 1);
    EXPECT_LOG(h, 0, "connect pair discover sub0=none sub0=notify sub1=none sub1=notify read1");
    CHECK(Phase(h) == SDL_BLE_PHASE_STARTING && BH_Controls(h)->battery == 80);
    BH_Value(h, SDL_DAYDREAM_POSE, capture7, sizeof(capture7), false);
    EXPECT_LOG(h, 7, "read1 publish");
    CHECK(BH_Button(h, SDL_BLE_BUTTON_RIGHT_SHOULDER) && BH_Controls(h)->buttons == (1u << SDL_BLE_BUTTON_RIGHT_SHOULDER));
    DESTROY(h);

    /* With a bond it does not pair */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_Start(h, true);
    EXPECT_LOG(h, 0, "connect discover sub0=none sub0=notify sub1=none sub1=notify read1");
    DESTROY(h);

    /* A bond this session made is not removed: Unreachable backs off */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Paired(h, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    EXPECT_LOG(h, 0, "connect pair discover sub0=none backoff disconnect");
    CHECK(h->session.paired && !h->session.repeated);
    DESTROY(h);

    /* A failed pairing ends the session without a backoff, before
       discovery and after a failed subscription alike. The host counts it. */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Paired(h, false);
    EXPECT_LOG(h, 0, "connect pair disconnect");
    CHECK(!h->session.backoff && SDL_BLESession_Ended(&h->session));
    EXPECT_OUTCOME(h, END_PAIRING);
    DESTROY(h);
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    BH_Paired(h, false);
    EXPECT_LOG(h, 0, "connect discover sub0=notify pair disconnect");
    CHECK(!h->session.backoff);
    EXPECT_OUTCOME(h, END_PAIRING);
    DESTROY(h);

    /* A stale bond: Unreachable on a device Windows reports bonded removes
       the bond, then the subscriptions run once more without it (Gear VR
       test 9) */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond");
    CHECK(Phase(h) == SDL_BLE_PHASE_REPAIRING);
    BH_BondRemoved(h, true);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond sub0=notify write1:0800+r");
    CHECK(h->session.removed_bond && !h->session.bonded);
    DESTROY(h);
    /* Unreachable on the wanted value, after None went through */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    BH_BondRemoved(h, true);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=none sub0=notify unbond sub0=notify write1:0800+r");
    DESTROY(h);
    /* A pair-first family pairs again before the repeat */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    BH_BondRemoved(h, true);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond pair");
    BH_Paired(h, true);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond pair sub0=none sub0=notify sub1=none sub1=notify read1");
    DESTROY(h);
    /* A family that works unpaired also sheds a stale bond */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    BH_BondRemoved(h, true);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond sub0=notify sub1=notify read1");
    DESTROY(h);
    /* A failed bond removal backs off */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    BH_BondRemoved(h, false);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond backoff disconnect");
    CHECK(h->session.backoff);
    DESTROY(h);
    /* A family that never pairs keeps its bond */
    {
        SDL_BLEFamily never = probe_family;

        never.pairing = SDL_BLE_PAIR_NEVER;
        h = BH_Create(&never, 0, NULL, true);
        BH_Connected(h, true, true);
        BH_Discovered(h, true, NULL);
        BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
        EXPECT_LOG(h, 0, "connect discover sub0=none backoff disconnect");
        DESTROY(h);
        h = BH_Create(&never, 0, NULL, true);
        BH_Connected(h, true, true);
        BH_Discovered(h, true, NULL);
        BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION);
        EXPECT_LOG(h, 0, "connect discover sub0=none backoff disconnect");
        DESTROY(h);
    }

    /* A stale bond that a security error reveals: on a bond this session did
       not make, InsufficientEncryption or InsufficientAuthentication removes
       the bond, the device pairs afresh, and the subscriptions run once more
       on the new bond, None first. The Oculus Go pairs on error. */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond");
    CHECK(Phase(h) == SDL_BLE_PHASE_REPAIRING && h->session.pair_after_removal);
    STRAYS(h, REMOVE_BOND);
    BH_BondRemoved(h, true);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond pair");
    CHECK(Phase(h) == SDL_BLE_PHASE_REPAIRING && h->session.removed_bond && !h->session.bonded);
    STRAYS(h, PAIR);
    BH_Paired(h, true);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond pair sub0=none sub0=notify sub1=none sub1=notify");
    CHECK(Phase(h) == SDL_BLE_PHASE_STARTING && h->session.paired && h->session.bonded && h->session.repeated);
    OculusGoFace(face, 0);
    BH_Value(h, SDL_OCULUSGO_FACE, face, sizeof(face), false);
    CHECK(h->published);
    DESTROY(h);
    /* The error on the wanted value, after None went through, and the
       repeat starts with None again. A second failure backs off, with one
       removal and one pairing in the session. */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION);
    BH_BondRemoved(h, true);
    BH_Paired(h, true);
    EXPECT_LOG(h, 0, "connect discover sub0=none sub0=notify unbond pair sub0=none");
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION);
    EXPECT_LOG(h, 0, "connect discover sub0=none sub0=notify unbond pair sub0=none backoff disconnect");
    CHECK(BH_Count(h, REMOVE_BOND) == 1 && BH_Count(h, PAIR) == 1 && h->session.backoff);
    EXPECT_OUTCOME(h, END_BACKOFF);
    DESTROY(h);
    /* A pair-first family: the Daydream */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    BH_BondRemoved(h, true);
    BH_Paired(h, true);
    BH_SubscribeAll(h);
    BH_ReadAll(h, &battery, 1);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond pair sub0=none sub0=notify sub1=none sub1=notify read1");
    BH_Value(h, SDL_DAYDREAM_POSE, capture7, sizeof(capture7), false);
    CHECK(h->published && BH_Button(h, SDL_BLE_BUTTON_RIGHT_SHOULDER));
    DESTROY(h);
    /* A failed removal backs off, and a failed pairing after it ends the
       session without a backoff, as any failed pairing does */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    BH_BondRemoved(h, false);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond backoff disconnect");
    EXPECT_OUTCOME(h, END_BACKOFF);
    DESTROY(h);
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    BH_BondRemoved(h, true);
    BH_Paired(h, false);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond pair disconnect");
    EXPECT_OUTCOME(h, END_PAIRING);
    DESTROY(h);
    /* No removal: with the hint off, for a family that works unpaired, for
       another error, on a bond this session made, and on an optional
       characteristic, which is skipped */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    EXPECT_LOG(h, 0, "connect discover sub0=none backoff disconnect");
    DESTROY(h);
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    EXPECT_LOG(h, 0, "connect discover sub0=none backoff disconnect");
    DESTROY(h);
    for (i = 0; i < COUNT(other_errors); ++i) {
        h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
        BH_Connected(h, true, true);
        BH_Discovered(h, true, NULL);
        BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, other_errors[i]);
        EXPECT_LOG(h, 0, "connect discover sub0=none backoff disconnect");
        DESTROY(h);
    }
    for (i = 0; i < COUNT(bonded_statuses); ++i) {
        h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
        BH_Connected(h, true, true);
        BH_Discovered(h, true, NULL);
        BH_Subscribed(h, bonded_statuses[i], SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
        EXPECT_LOG(h, 0, "connect discover sub0=none backoff disconnect");
        DESTROY(h);
    }
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Paired(h, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    EXPECT_LOG(h, 0, "connect pair discover sub0=none backoff disconnect");
    DESTROY(h);
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    CHECK(h->session.step == SDL_OCULUSGO_BEEF);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    EXPECT_LOG(h, 0, "connect discover sub0=none sub0=notify sub1=none");
    CHECK(Phase(h) == SDL_BLE_PHASE_STARTING && !h->session.removed_bond);
    DESTROY(h);

    /* One repeat per session: no pairing after a bond removal's repeat, no
       bond removal after this session paired */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    BH_BondRemoved(h, true);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond sub0=notify backoff disconnect");
    DESTROY(h);
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    BH_Paired(h, true);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    EXPECT_LOG(h, 0, "connect discover sub0=notify pair sub0=none backoff disconnect");
    DESTROY(h);

    /* The hint off: no pair and no bond removal */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    EXPECT_LOG(h, 0, "connect discover sub0=notify backoff disconnect");
    DESTROY(h);
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, false);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    EXPECT_LOG(h, 0, "connect discover sub0=none backoff disconnect");
    DESTROY(h);
    /* An unbonded pair-first device backs off at once and is never
       published, whatever arrives later */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    EXPECT_LOG(h, 0, "connect backoff disconnect");
    CHECK(h->session.backoff && SDL_BLESession_Ended(&h->session));
    BH_Value(h, SDL_DAYDREAM_POSE, capture7, sizeof(capture7), false);
    BH_Discovered(h, true, NULL);
    EXPECT_LOG(h, 0, "connect backoff disconnect");
    CHECK(!h->published && BH_Controls(h)->buttons == 0);
    DESTROY(h);
    /* A bond made in Settings is enough with the hint off */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, false);
    BH_Start(h, true);
    EXPECT_LOG(h, 0, "connect discover sub0=none sub0=notify sub1=none sub1=notify read1");
    BH_Value(h, SDL_DAYDREAM_POSE, capture7, sizeof(capture7), false);
    CHECK(h->published && BH_Button(h, SDL_BLE_BUTTON_RIGHT_SHOULDER));
    DESTROY(h);
}

/* Test 6: a 68-byte value reaches the module intact, a 69-byte value with a
   68-byte buffer is dropped and counted, a zero-length value is ignored */
static void Test6_ValueSizes(void)
{
    SDL_BLEValue entries[4];
    SDL_BLEValueQueue queue;
    SDL_BLEValue value;
    uint8_t data[69];
    BH_Harness *h = BH_Create(&probe_family, 0, NULL, false);
    const ProbeCall *call;
    int i;

    for (i = 0; i < (int)sizeof(data); ++i) {
        data[i] = (uint8_t)(0xA0 ^ (i * 3));
    }
    data[0] = 0x01;
    SDL_BLE_InitQueue(&queue, entries, COUNT(entries), 68);
    BH_Start(h, false);
    CHECK(Phase(h) == SDL_BLE_PHASE_STARTING && ProbeCount(h, 'S') == 1);

    CHECK(SDL_BLE_PushValue(&queue, PROBE_INPUT, data, 68, 1234567) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PushValue(&queue, PROBE_INPUT, data, 69, 1234568) == SDL_BLE_PUSH_TOO_LONG && queue.too_long == 1);
    CHECK(SDL_BLE_PushValue(&queue, PROBE_INPUT, data, 0, 1234569) == SDL_BLE_PUSH_EMPTY && queue.empty == 1);
    CHECK(queue.count == 1);
    CHECK(SDL_BLE_PopValue(&queue, &value) && !value.gap);
    Deliver(h, &value);
    call = &Probe(h)->calls[Probe(h)->ncalls - 1];
    CHECK(ProbeCount(h, 'V') == 1 && call->kind == 'V' && call->characteristic == PROBE_INPUT);
    CHECK(call->length == 68 && memcmp(call->data, data, 68) == 0 && call->time == 1234567);
    CHECK(h->published && BH_Button(h, SDL_BLE_BUTTON_SOUTH));
    CHECK(!SDL_BLE_PopValue(&queue, &value));
    DESTROY(h);
}

/* Test 7: after 1024 queued values the next one is dropped. The 1024 values
   drain in order, and the first value queued after them carries the gap
   flag, which releases every held button before that value applies. */
static void Test7_FullQueue(void)
{
    SDL_BLEValue *entries = (SDL_BLEValue *)calloc(SDL_BLE_QUEUE_CAPACITY, sizeof(SDL_BLEValue));
    SDL_BLEValueQueue queue;
    SDL_BLEValue value;
    uint8_t report[SDL_POKEBALL_REPORT_SIZE];
    BH_Harness *h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    int i, mark, in_order = 0;

    if (!entries) {
        printf("out of memory\n");
        exit(2);
    }
    SDL_BLE_InitQueue(&queue, entries, SDL_BLE_QUEUE_CAPACITY, SDL_BLE_MAX_VALUE);
    BH_Start(h, false);

    /* The top button held in all 1024, the index in the motion bytes, which
       the module does not decode */
    for (i = 0; i < SDL_BLE_QUEUE_CAPACITY; ++i) {
        PokeballReport(report, (uint8_t)i, 0x01, 112, 108);
        report[5] = (uint8_t)(i >> 8);
        report[6] = (uint8_t)i;
        BH_CHECK(SDL_BLE_PushValue(&queue, SDL_POKEBALL_INPUT, report, sizeof(report), (uint64_t)i * 1000) ==
                     SDL_BLE_PUSH_QUEUED,
                 "value %d", i);
    }
    /* The release that follows is lost */
    PokeballReport(report, 0, 0x00, 112, 108);
    CHECK(SDL_BLE_PushValue(&queue, SDL_POKEBALL_INPUT, report, sizeof(report), 2000000) == SDL_BLE_PUSH_FULL);
    CHECK(queue.full == 1 && queue.count == SDL_BLE_QUEUE_CAPACITY);

    for (i = 0; i < SDL_BLE_QUEUE_CAPACITY; ++i) {
        if (SDL_BLE_PopValue(&queue, &value) && !value.gap && value.data[0] == (uint8_t)i &&
            value.data[5] == (uint8_t)(i >> 8) && value.data[6] == (uint8_t)i && value.time_ns == (uint64_t)i * 1000) {
            ++in_order;
        }
        Deliver(h, &value);
    }
    CHECK(in_order == SDL_BLE_QUEUE_CAPACITY);
    CHECK(h->published && BH_Count(h, PUBLISH) == 1 && BH_Button(h, SDL_BLE_BUTTON_EAST));

    /* The next value carries the gap: the held button is released, then the
       stick press applies */
    PokeballReport(report, 0, 0x02, 112, 108);
    CHECK(SDL_BLE_PushValue(&queue, SDL_POKEBALL_INPUT, report, sizeof(report), 3000000) == SDL_BLE_PUSH_QUEUED);
    CHECK(SDL_BLE_PopValue(&queue, &value) && value.gap);
    mark = h->nsnapshots;
    Deliver(h, &value);
    CHECK(h->nsnapshots == mark + 2);
    if (h->nsnapshots == mark + 2) {
        CHECK(h->snapshots[mark].controls.buttons == 0 && h->snapshots[mark].time_ns == 3000000);
        CHECK(h->snapshots[mark + 1].controls.buttons == (1u << SDL_BLE_BUTTON_SOUTH));
    }
    DESTROY(h);
    free(entries);
}

/* Test 8: link loss removes the joystick, releases held controls and stops
   every timer. The next advertisement reconnects and replays the start-up
   script from its first write. A link lost before the joystick appeared
   waits SDL_BLE_CONNECT_RETRY_MS first, as a failed connect does. */
static void Test8_LinkLoss(void)
{
    static const uint8_t ack[2] = { 0x08, 0x00 };
    static const uint8_t fist[3] = { 0x03, 0x01, 0x00 };
    uint8_t properties[SDL_BLE_MAX_CHARS];
    uint8_t packet[SDL_GEARVR_PACKET_SIZE];
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad;
    SDL_BLEMatch match = { -1, 0 };
    const SDL_BLEControls *controls;
    uint64_t deadline;
    BH_Harness *h;
    int mark;

    /* Gear VR: streaming with the trigger, the touchpad click, volume up and
       a finger down */
    SDL_BLEHost_Init(&host);
    ad = Ad(0x2C8DB1A0466A, SDL_BLE_AD_ADVERTISEMENT);
    AdService(&ad, "4f63756c-7573-2054-6872-65656d6f7465");
    CHECK(Offer(&host, &ad, 1000, &match) && match.family == SDL_BLE_FAMILY_GEARVR);
    h = BH_Create(SDL_BLE_Families[SDL_BLE_FAMILY_GEARVR], match.variant, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    BH_WriteAll(h);
    h->now = 2580;
    BH_Value(h, SDL_GEARVR_DATA, ack, sizeof(ack), false);
    BH_WriteAll(h);
    GearVRPacket(packet, 1, 200, 100, 0x01 | 0x08 | 0x10, 90);
    h->now = 2600;
    BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    EXPECT_LOG(h, 0, "connect discover sub0=notify write1:0800+r write1:0100+r publish");
    CHECK(h->published && BH_Axis(h, SDL_BLE_AXIS_RIGHT_TRIGGER) == 32767 && BH_Button(h, SDL_BLE_BUTTON_SOUTH));
    CHECK(BH_Button(h, SDL_BLE_BUTTON_RIGHT_SHOULDER) && BH_Controls(h)->finger);
    CHECK(BH_Axis(h, SDL_BLE_AXIS_LEFTX) == 8191 && BH_Axis(h, SDL_BLE_AXIS_LEFTY) == -12287);
    /* The module's silence timer, 1 s after the packet, is the first */
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 2600 + SDL_GEARVR_SILENCE_MS);

    mark = h->nactions;
    h->now = 3000;
    Lost(h);
    EXPECT_LOG(h, mark, "remove disconnect");
    controls = BH_Controls(h);
    CHECK(!h->published && SDL_BLESession_Ended(&h->session) && !h->session.backoff);
    EXPECT_OUTCOME(h, END_PUBLISHED);
    CHECK(controls->buttons == 0 && !controls->finger && controls->axes[SDL_BLE_AXIS_LEFTX] == 0 &&
          controls->axes[SDL_BLE_AXIS_LEFTY] == 0 && controls->axes[SDL_BLE_AXIS_RIGHT_TRIGGER] == -32768);
    CHECK(controls->battery == 90);
    CHECK(h->snapshots[h->nsnapshots - 1].time_ns == 3000000000ull);
    /* No timer runs, whatever the clock does */
    CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline));
    BH_Advance(h, 1000000);
    SDL_BLESession_Tick(&h->session, 1000000);
    BH_Drain(h);
    EXPECT_LOG(h, mark, "remove disconnect");
    CHECK(((const SDL_GearVRState *)h->state)->deadline == 12580);

    /* The next advertisement reconnects and starts over from 08 00 */
    Ended(&host, ad.address, h);
    DESTROY(h);
    CHECK(!SDL_BLEHost_IsActive(&host, ad.address));
    CHECK(Offer(&host, &ad, 3001, &match) && match.family == SDL_BLE_FAMILY_GEARVR);
    h = BH_Create(SDL_BLE_Families[match.family], match.variant, NULL, false);
    h->now = 3001;
    BH_Start(h, false);
    EXPECT_LOG(h, 0, "connect discover sub0=notify write1:0800+r");
    CHECK(TimeAt(h, 3) == 3001 && BH_Controls(h)->buttons == 0 && BH_Controls(h)->battery == -1);
    DESTROY(h);

    /* Myo: a pose decoded during the subscriptions, before the start and so
       not ready, then the link lost in the middle of the start-up script,
       before the joystick appeared */
    SDL_BLEHost_Init(&host);
    ad = Ad(0xE1A2B3C4D5E6, SDL_BLE_AD_SCAN_RESPONSE);
    AdService(&ad, "d5060001-a904-deb9-4748-2c7f4a124842");
    CHECK(Offer(&host, &ad, 100, &match) && match.family == SDL_BLE_FAMILY_MYO);
    h = BH_Create(SDL_BLE_Families[match.family], match.variant, NULL, false);
    h->now = 100;
    MyoProperties(properties);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_Value(h, SDL_MYO_CLASSIFIER, fist, sizeof(fist), false);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    BH_Written(h, true);
    EXPECT_LOG(h, 0,
               "connect discover sub0=notify sub1=indicate sub2=indicate sub4=notify read4 write3:0103000301+r "
               "write3:090101+r");
    CHECK(!h->published && BH_Button(h, SDL_MYO_BUTTON_FIST));
    h->now = 200;
    Lost(h);
    EXPECT_LOG(h, 8, "write3:090101+r disconnect");
    CHECK(BH_Controls(h)->buttons == 0);
    EXPECT_OUTCOME(h, END_CONNECT);
    BH_Written(h, true);
    EXPECT_LOG(h, 8, "write3:090101+r disconnect");
    Ended(&host, ad.address, h);
    DESTROY(h);
    /* The address waits as after a failed connect */
    CHECK(!Offer(&host, &ad, 201, &match) && !Offer(&host, &ad, 200 + SDL_BLE_CONNECT_RETRY_MS - 1, &match));
    CHECK(Offer(&host, &ad, 200 + SDL_BLE_CONNECT_RETRY_MS, &match));
    h = BH_Create(SDL_BLE_Families[match.family], match.variant, NULL, false);
    h->now = 200 + SDL_BLE_CONNECT_RETRY_MS;
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=indicate sub2=indicate sub4=notify read4 write3:0103000301+r");
    BH_WriteAll(h);
    BH_Value(h, SDL_MYO_IMU, myo_imu_level, sizeof(myo_imu_level), false);
    EXPECT_LOG(h, 7, "write3:0103000301+r write3:090101+r write3:0a0102+r publish");
    /* Published, then lost: the joystick goes */
    BH_Value(h, SDL_MYO_CLASSIFIER, fist, sizeof(fist), false);
    CHECK(BH_Button(h, SDL_MYO_BUTTON_FIST));
    Lost(h);
    EXPECT_LOG(h, 10, "publish remove disconnect");
    CHECK(BH_Controls(h)->buttons == 0 && !h->published);
    DESTROY(h);
}

/* Test 9: on an injected clock every timer fires at its exact time, and none
   fires after the link is lost or the session closed */
static void Test9_Clock(void)
{
    static const uint8_t ack[2] = { 0x08, 0x00 };
    static const uint8_t echo[2] = { 0x01, 0x00 };
    static const uint8_t idle[1] = { SDL_ZWIFT_MESSAGE_IDLE };
    uint8_t packet[SDL_GEARVR_PACKET_SIZE];
    uint8_t face[SDL_OCULUSGO_REPORT_SIZE];
    uint64_t deadline;
    BH_Harness *h;
    int mark, nsnapshots;

    /* Gear VR without the acknowledgement: 01 00 at +4000, then 04 00 every
       10 s */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, false);
    h->now = 1000;
    CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline));
    BH_Start(h, false);
    EXPECT_LOG(h, 0, "connect discover sub0=notify write1:0800+r");
    CHECK(TimeAt(h, 3) == 1000);
    BH_WriteAll(h);
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 5000);
    mark = h->nactions;
    BH_Advance(h, 4999);
    EXPECT_LOG(h, mark, "");
    BH_Advance(h, 5000);
    EXPECT_LOG(h, mark, "write1:0100+r");
    CHECK(TimeAt(h, mark) == 5000);
    /* The first packet publishes, so the keep-alive runs on a live joystick
       past the start timeout. Packets keep coming, so the module's silence
       timer never runs out. */
    GearVRPacket(packet, 2, 0, 0, 0, 100);
    BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    EXPECT_LOG(h, mark, "write1:0100+r publish");
    mark = h->nactions - 1;
    GearVRStream(h, 14999);
    EXPECT_LOG(h, mark, "publish");
    GearVRStream(h, 35000);
    EXPECT_LOG(h, mark, "publish write1:0400+r write1:0400+r write1:0400+r");
    CHECK(TimeAt(h, mark + 1) == 15000 && TimeAt(h, mark + 2) == 25000 && TimeAt(h, mark + 3) == 35000);
    /* A keep-alive whose write is still out when the next falls due waits
       for it, and none is lost */
    GearVRStream(h, 44999);
    h->now = 45000;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(TimeAt(h, h->nactions - 1) == 45000 && h->session.waiting);
    h->now = 55000;
    BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    EXPECT_LOG(h, mark + 4, "write1:0400+r");
    BH_Written(h, true);
    EXPECT_LOG(h, mark + 4, "write1:0400+r write1:0400+r");
    CHECK(TimeAt(h, mark + 5) == 55000);
    BH_Written(h, true);
    /* Lost: nothing more */
    mark = h->nactions;
    h->now = 56000;
    Lost(h);
    EXPECT_LOG(h, mark, "remove disconnect");
    CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline));
    BH_Advance(h, 1000000);
    EXPECT_LOG(h, mark, "remove disconnect");
    DESTROY(h);

    /* Gear VR with the acknowledgement at +1580 ms: 01 00 at once, then the
       keep-alive 10 s later. A later 2-byte echo is not input. */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    BH_WriteAll(h);
    h->now = 2580;
    BH_Value(h, SDL_GEARVR_DATA, ack, sizeof(ack), false);
    EXPECT_LOG(h, 3, "write1:0800+r write1:0100+r");
    CHECK(TimeAt(h, 4) == 2580);
    BH_WriteAll(h);
    GearVRPacket(packet, 2, 0, 0, 0x00, 100);
    h->now = 2600;
    BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    nsnapshots = h->nsnapshots;
    GearVRStream(h, 4000);
    BH_Value(h, SDL_GEARVR_DATA, echo, sizeof(echo), false);
    CHECK(h->nsnapshots == nsnapshots);
    CHECK(((const SDL_GearVRState *)h->state)->deadline == 12580);
    mark = h->nactions;
    GearVRStream(h, 12579);
    EXPECT_LOG(h, mark, "");
    GearVRStream(h, 12580);
    EXPECT_LOG(h, mark, "write1:0400+r");
    CHECK(TimeAt(h, mark) == 12580);
    /* Closed: the closing write, and no keep-alive while it is out or after */
    h->now = 13000;
    Close(h);
    EXPECT_LOG(h, mark, "write1:0400+r write1:0000+r+c");
    CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline));
    BH_Advance(h, 30000);
    EXPECT_LOG(h, mark, "write1:0400+r write1:0000+r+c");
    BH_Written(h, true);
    EXPECT_LOG(h, mark, "write1:0400+r write1:0000+r+c remove disconnect");
    BH_Advance(h, 100000);
    EXPECT_LOG(h, mark, "write1:0400+r write1:0000+r+c remove disconnect");
    DESTROY(h);

    /* Oculus Go: the silence timer 3 s after the start and after each value */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 4000);
    OculusGoFace(face, 0x01);
    h->now = 2000;
    BH_Value(h, SDL_OCULUSGO_FACE, face, sizeof(face), false);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify publish");
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 5000);
    mark = h->nactions;
    BH_Advance(h, 4999);
    EXPECT_LOG(h, mark, "");
    CHECK(BH_Button(h, SDL_BLE_BUTTON_GUIDE));
    BH_Advance(h, 5000);
    EXPECT_LOG(h, mark, "sub0=notify");
    CHECK(TimeAt(h, mark) == 5000 && !BH_Button(h, SDL_BLE_BUTTON_GUIDE));
    BH_SubscribeAll(h);
    EXPECT_LOG(h, mark, "sub0=notify sub1=notify");
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 5000 + SDL_OCULUSGO_RESUBSCRIBE_MS);
    /* Lost: the timer stops */
    h->now = 6000;
    Lost(h);
    EXPECT_LOG(h, mark, "sub0=notify sub1=notify remove disconnect");
    CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline));
    BH_Advance(h, 1000000);
    EXPECT_LOG(h, mark, "sub0=notify sub1=notify remove disconnect");
    DESTROY(h);

    /* Zwift with a NULL crypto: the 10 s handshake timer runs from the
       completion of the RideOn write, then the session backs off */
    h = BH_Create(&SDL_BLEZwiftFamily, SDL_ZWIFT_PLAY_RIGHT, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify write2:526964654f6e");
    /* No handshake timer while the write is out, only the start timeout */
    CHECK(!SDL_BLEZwiftModule.GetDeadline(h->state, &deadline));
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 1000 + SDL_BLE_START_TIMEOUT_MS);
    BH_Advance(h, 1200);
    BH_Written(h, true);
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 11200);
    mark = h->nactions;
    BH_Advance(h, 11199);
    EXPECT_LOG(h, mark, "");
    BH_Advance(h, 11200);
    EXPECT_LOG(h, mark, "backoff disconnect");
    CHECK(TimeAt(h, mark) == 11200 && h->session.backoff && !h->published);
    CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline));
    DESTROY(h);

    /* Zwift heard before the deadline: published, and the timer is gone */
    h = BH_Create(&SDL_BLEZwiftFamily, SDL_ZWIFT_PLAY_RIGHT, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    BH_WriteAll(h);
    h->now = 5000;
    BH_Value(h, SDL_ZWIFT_ASYNC, idle, sizeof(idle), false);
    EXPECT_LOG(h, 4, "write2:526964654f6e publish");
    CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline));
    mark = h->nactions;
    BH_Advance(h, 100000);
    EXPECT_LOG(h, mark, "");
    /* Closed: no timer, no closing write */
    Close(h);
    EXPECT_LOG(h, mark, "remove disconnect");
    DESTROY(h);
}

/* Session flows with the real families: the order of every action */
static void TestFlowOrder(void)
{
    static const uint8_t battery = 0x55;
    uint8_t properties[SDL_BLE_MAX_CHARS];
    BH_Harness *h;

    /* Myo, unbonded: Notify or Indicate from the properties, the battery read
       before the start-up, then the start-up writes one at a time */
    MyoProperties(properties);
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=indicate sub2=indicate sub4=notify read4");
    CHECK(Phase(h) == SDL_BLE_PHASE_READING);
    BH_ReadAll(h, &battery, 1);
    EXPECT_LOG(h, 6, "read4 write3:0103000301+r");
    CHECK(BH_Controls(h)->battery == 85 && Phase(h) == SDL_BLE_PHASE_STARTING && !h->published);
    BH_WriteAll(h);
    EXPECT_LOG(h, 7, "write3:0103000301+r write3:090101+r write3:0a0102+r");
    BH_Value(h, SDL_MYO_IMU, myo_imu_level, sizeof(myo_imu_level), false);
    EXPECT_LOG(h, 7, "write3:0103000301+r write3:090101+r write3:0a0102+r publish");
    CHECK(Phase(h) == SDL_BLE_PHASE_RUNNING && h->published && h->nsamples == 2);
    DESTROY(h);

    /* Myo, bonded: None, then the wanted value, per characteristic */
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    BH_ReadAll(h, &battery, 1);
    EXPECT_LOG(h, 0,
               "connect discover sub0=none sub0=notify sub1=none sub1=indicate sub2=none sub2=indicate sub4=none "
               "sub4=notify read4 write3:0103000301+r");
    DESTROY(h);
}

/* Session flows: which characteristics are subscribed, skipped or fatal */
static void TestFlowCharacteristics(void)
{
    static const uint8_t battery = 0x64;
    uint8_t properties[SDL_BLE_MAX_CHARS];
    bool found[SDL_BLE_MAX_CHARS];
    BH_Harness *h;

    /* An optional characteristic that is missing is neither subscribed nor read */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    AllFound(found);
    found[SDL_POKEBALL_BATTERY] = false;
    BH_DefaultProperties(&SDL_BLEPokeballFamily, properties);
    BH_Connected(h, true, false);
    Discover(h, found, properties);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify");
    CHECK(Phase(h) == SDL_BLE_PHASE_STARTING && !h->session.waiting);
    DESTROY(h);

    /* An optional characteristic without Notify or Indicate is not
       subscribed, and is still read */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    memset(properties, 0, sizeof(properties));
    properties[SDL_POKEBALL_INPUT] = SDL_BLE_PROPERTY_NOTIFY;
    properties[SDL_POKEBALL_BATTERY] = SDL_BLE_PROPERTY_READ;
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    BH_ReadAll(h, &battery, 1);
    EXPECT_LOG(h, 0, "connect discover sub0=notify read1");
    CHECK(Phase(h) == SDL_BLE_PHASE_STARTING && BH_Controls(h)->battery == 100);
    DESTROY(h);

    /* An optional characteristic whose descriptor write fails is skipped,
       unbonded and bonded, whatever the error */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1");
    DESTROY(h);
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    EXPECT_LOG(h, 0, "connect discover sub0=none sub0=notify sub1=none read1");
    CHECK(!h->session.removed_bond);
    DESTROY(h);

    /* A characteristic that is read needs the Read property */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    memset(properties, 0, sizeof(properties));
    properties[SDL_POKEBALL_INPUT] = SDL_BLE_PROPERTY_NOTIFY;
    properties[SDL_POKEBALL_BATTERY] = SDL_BLE_PROPERTY_NOTIFY;
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify");
    CHECK(Phase(h) == SDL_BLE_PHASE_STARTING);
    DESTROY(h);

    /* A required characteristic that is missing backs off, subscribed or
       written */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, false);
    AllFound(found);
    found[SDL_GEARVR_DATA] = false;
    BH_DefaultProperties(&SDL_BLEGearVRFamily, properties);
    BH_Connected(h, true, false);
    Discover(h, found, properties);
    EXPECT_LOG(h, 0, "connect discover backoff disconnect");
    CHECK(h->session.backoff);
    DESTROY(h);
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    AllFound(found);
    found[SDL_MYO_COMMAND] = false;
    MyoProperties(properties);
    BH_Connected(h, true, false);
    Discover(h, found, properties);
    EXPECT_LOG(h, 0, "connect discover backoff disconnect");
    DESTROY(h);

    /* A required characteristic whose descriptor write fails backs off */
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    MyoProperties(properties);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_Subscribed(h, SDL_BLE_STATUS_ACCESS_DENIED, 0);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=indicate backoff disconnect");
    DESTROY(h);

    /* A failed discovery backs off. A failed connect does not, and says so
       to the host. */
    h = BH_Create(&SDL_BLEGHLiOSFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, false, NULL);
    EXPECT_LOG(h, 0, "connect discover backoff disconnect");
    CHECK(h->session.backoff);
    EXPECT_OUTCOME(h, END_BACKOFF);
    DESTROY(h);
    h = BH_Create(&SDL_BLEGHLiOSFamily, 0, NULL, true);
    BH_Connected(h, false, false);
    EXPECT_LOG(h, 0, "connect disconnect");
    CHECK(!h->session.backoff);
    EXPECT_OUTCOME(h, END_CONNECT);
    DESTROY(h);
}

/* Session flows: reads reach the module before its start */
static void TestFlowReads(void)
{
    static const uint8_t level = 0x42;
    BH_Harness *h;
    const ProbeState *probe;

    h = BH_Create(&probe_family, 0, NULL, false);
    Probe(h)->start_writes = 1;
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1");
    CHECK(Probe(h)->ncalls == 0);
    h->now = 700;
    Read(h, &level, 1);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 write2:10+r");
    probe = Probe(h);
    CHECK(probe->ncalls == 2 && probe->calls[0].kind == 'V' && probe->calls[1].kind == 'S');
    CHECK(probe->calls[0].characteristic == PROBE_BATTERY && probe->calls[0].length == 1 && probe->calls[0].data[0] == 0x42);
    CHECK(probe->calls[0].time == 700000000ull && probe->calls[1].time == 700);
    DESTROY(h);

    /* A failed read, or an empty one, reaches nothing, and the start follows */
    h = BH_Create(&probe_family, 0, NULL, false);
    BH_Start(h, false);
    CHECK(Probe(h)->ncalls == 1 && Probe(h)->calls[0].kind == 'S');
    DESTROY(h);
    h = BH_Create(&probe_family, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    Read(h, &level, 0);
    CHECK(Probe(h)->ncalls == 1 && Probe(h)->calls[0].kind == 'S' && Phase(h) == SDL_BLE_PHASE_STARTING);
    DESTROY(h);
    /* A failed read reaches nothing, whatever its buffer holds */
    h = BH_Create(&probe_family, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    SDL_BLESession_Read(&h->session, false, &level, 1, h->now * 1000000);
    BH_Drain(h);
    CHECK(ProbeCount(h, 'V') == 0 && ProbeCount(h, 'S') == 1);
    DESTROY(h);

    /* Every characteristic marked for reading is read, in order */
    {
        SDL_BLEFamily reads = probe_family;

        reads.characteristics[PROBE_COMMAND].flags = SDL_BLE_CHAR_READ;
        h = BH_Create(&reads, 0, NULL, false);
        BH_Connected(h, true, false);
        BH_Discovered(h, true, NULL);
        BH_SubscribeAll(h);
        Read(h, &level, 1);
        EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 read2");
        DESTROY(h);
    }
}

/* Session flows: the joystick appears with the first input after the
   start-up, once */
static void TestFlowPublish(void)
{
    static const uint8_t ack[2] = { 0x08, 0x00 };
    uint8_t report[SDL_POKEBALL_REPORT_SIZE];
    uint8_t packet[SDL_GEARVR_PACKET_SIZE];
    BH_Harness *h;

    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    BH_Start(h, false);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1");
    PokeballReport(report, 1, 0x01, 112, 108);
    BH_Value(h, SDL_POKEBALL_INPUT, report, SDL_POKEBALL_REPORT_SIZE - 1, false);
    CHECK(!h->published && Phase(h) == SDL_BLE_PHASE_STARTING);
    BH_Value(h, SDL_POKEBALL_INPUT, report, sizeof(report), false);
    EXPECT_LOG(h, 5, "publish");
    CHECK(Phase(h) == SDL_BLE_PHASE_RUNNING && h->session.published && BH_Button(h, SDL_BLE_BUTTON_EAST));
    report[1] = 0x02;
    BH_Value(h, SDL_POKEBALL_INPUT, report, sizeof(report), false);
    CHECK(BH_Count(h, PUBLISH) == 1 && BH_Button(h, SDL_BLE_BUTTON_SOUTH) && !BH_Button(h, SDL_BLE_BUTTON_EAST));
    DESTROY(h);

    /* Gear VR: input before the acknowledgement does not publish */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, false);
    BH_Start(h, false);
    BH_WriteAll(h);
    GearVRPacket(packet, 2, 0, 0, 0x04, 100);
    BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    CHECK(!h->published && BH_Button(h, SDL_BLE_BUTTON_BACK));
    BH_Value(h, SDL_GEARVR_DATA, ack, sizeof(ack), false);
    BH_WriteAll(h);
    BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    EXPECT_LOG(h, 0, "connect discover sub0=notify write1:0800+r write1:0100+r publish");
    DESTROY(h);
}

/* Session flows: one write out at a time, with the module's response flag */
static void TestFlowWrites(void)
{
    uint8_t properties[SDL_BLE_MAX_CHARS];
    BH_Harness *h;

    MyoProperties(properties);
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    CHECK(BH_Count(h, WRITE) == 1 && h->session.waiting);
    /* No other write goes out while the write is out, whatever arrives. The
       publish awaits no answer and goes out at once. */
    BH_Value(h, SDL_MYO_IMU, myo_imu_level, sizeof(myo_imu_level), false);
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    CHECK(BH_Count(h, WRITE) == 1 && h->published && BH_LastKind(h) == PUBLISH && h->session.waiting);
    /* A failed write does not stop the next */
    BH_Written(h, false);
    CHECK(BH_Count(h, WRITE) == 2);
    BH_Written(h, true);
    CHECK(BH_Count(h, WRITE) == 3);
    BH_Written(h, true);
    EXPECT_LOG(h, 7, "write3:0103000301+r publish write3:090101+r write3:0a0102+r");
    CHECK(!h->session.waiting && BH_Count(h, WRITE) == 3);
    DESTROY(h);

    /* Zwift writes RideOn without a response */
    h = BH_Create(&SDL_BLEZwiftFamily, SDL_ZWIFT_CLICK, NULL, false);
    BH_Start(h, false);
    EXPECT_LOG(h, 4, "write2:526964654f6e");
    CHECK(!h->actions[4].action.response && !h->actions[4].action.closing);
    DESTROY(h);

    /* The flag is the module's, write by write */
    h = BH_Create(&probe_family, 0, NULL, false);
    Probe(h)->start_writes = 3;
    BH_Start(h, false);
    BH_WriteAll(h);
    EXPECT_LOG(h, 5, "write2:10+r write2:11 write2:12+r");
    CHECK(ProbeCount(h, 'D') == 3 && Probe(h)->calls[1].kind == 'D' && Probe(h)->calls[1].success);
    DESTROY(h);

    /* A failed write reaches the module as failed */
    h = BH_Create(&probe_family, 0, NULL, false);
    Probe(h)->start_writes = 1;
    BH_Start(h, false);
    BH_Written(h, false);
    CHECK(Probe(h)->ncalls == 2 && Probe(h)->calls[1].kind == 'D' && !Probe(h)->calls[1].success);
    DESTROY(h);
}

/* Session flows: a resubscription the module asks for. The Oculus Go asks
   after 3 s without data. None goes first on a bonded link, a failure is
   skipped, and the joystick stays. */
static void TestFlowResubscribe(void)
{
    uint8_t face[SDL_OCULUSGO_REPORT_SIZE];
    BH_Harness *h;
    int mark;

    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, true);
    EXPECT_LOG(h, 0, "connect discover sub0=none sub0=notify sub1=none sub1=notify");
    OculusGoFace(face, 0x02);
    h->now = 1500;
    BH_Value(h, SDL_OCULUSGO_FACE, face, sizeof(face), false);
    CHECK(h->published && BH_Button(h, SDL_BLE_BUTTON_BACK));
    mark = h->nactions;
    BH_Advance(h, 4500);
    EXPECT_LOG(h, mark, "sub0=none");
    CHECK(!BH_Button(h, SDL_BLE_BUTTON_BACK) && Phase(h) == SDL_BLE_PHASE_RUNNING && h->session.resubscribing);
    /* Values keep flowing while it runs */
    OculusGoFace(face, 0x04);
    BH_Value(h, SDL_OCULUSGO_FACE, face, sizeof(face), false);
    CHECK(BH_Axis(h, SDL_BLE_AXIS_RIGHT_TRIGGER) == 32767);
    BH_Subscribed(h, SDL_BLE_STATUS_FAILED, 0);
    EXPECT_LOG(h, mark, "sub0=none sub1=none");
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    EXPECT_LOG(h, mark, "sub0=none sub1=none sub1=notify");
    CHECK(h->published && Phase(h) == SDL_BLE_PHASE_RUNNING && !h->session.waiting && !h->session.resubscribing);
    CHECK(!h->session.removed_bond);
    DESTROY(h);

    /* Unbonded, with the pairing hint on: an authentication error is
       skipped too, and nothing pairs */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    h->now = 1000;
    BH_Start(h, false);
    OculusGoFace(face, 0);
    BH_Value(h, SDL_OCULUSGO_FACE, face, sizeof(face), false);
    mark = h->nactions;
    BH_Advance(h, 4000);
    EXPECT_LOG(h, mark, "sub0=notify");
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION);
    EXPECT_LOG(h, mark, "sub0=notify sub1=notify");
    CHECK(h->published && Phase(h) == SDL_BLE_PHASE_RUNNING && BH_Count(h, PAIR) == 0);
    DESTROY(h);

    /* A request made while a resubscription runs follows it */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    mark = h->nactions;
    BH_Advance(h, 4000);
    EXPECT_LOG(h, mark, "sub0=notify");
    BH_Advance(h, 7000);
    EXPECT_LOG(h, mark, "sub0=notify");
    BH_SubscribeAll(h);
    EXPECT_LOG(h, mark, "sub0=notify sub1=notify sub0=notify sub1=notify");
    CHECK(!h->published && Phase(h) == SDL_BLE_PHASE_STARTING);
    DESTROY(h);

    /* A write the module queues with its request waits for the
       resubscription */
    {
        static const uint8_t press = 0x01, again = PROBE_RESUBSCRIBE;
        SDL_BLEFamily family = probe_family;

        family.module = &resubscribe_probe_module;
        h = BH_Create(&family, 0, NULL, false);
        BH_Start(h, false);
        BH_Value(h, PROBE_INPUT, &press, 1, false);
        mark = h->nactions;
        BH_Value(h, PROBE_INPUT, &again, 1, false);
        EXPECT_LOG(h, mark, "sub0=notify");
        BH_SubscribeAll(h);
        EXPECT_LOG(h, mark, "sub0=notify sub1=notify write2:fe+r");
        DESTROY(h);

        /* A close during a resubscription goes on when its answer comes */
        h = BH_Create(&family, 0, NULL, false);
        BH_Start(h, false);
        BH_Value(h, PROBE_INPUT, &press, 1, false);
        BH_Value(h, PROBE_INPUT, &again, 1, false);
        mark = h->nactions;
        Close(h);
        EXPECT_LOG(h, mark, "");
        BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
        EXPECT_LOG(h, mark, "write2:c0+r+c");
        BH_Written(h, true);
        EXPECT_LOG(h, mark, "write2:c0+r+c remove disconnect");
        DESTROY(h);

        /* On a bond, the characteristic after a skipped failure starts with
           None */
        h = BH_Create(&family, 0, NULL, false);
        BH_Start(h, true);
        BH_Value(h, PROBE_INPUT, &press, 1, false);
        mark = h->nactions;
        BH_Value(h, PROBE_INPUT, &again, 1, false);
        BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
        BH_Subscribed(h, SDL_BLE_STATUS_FAILED, 0);
        EXPECT_LOG(h, mark, "sub0=none sub0=notify sub1=none");
        DESTROY(h);
    }
}

/* Session flows: an orderly close sends the module's closing writes, then
   removes the joystick and releases the device */
static void TestFlowClose(void)
{
    static const uint8_t ack[2] = { 0x08, 0x00 };
    uint8_t properties[SDL_BLE_MAX_CHARS];
    uint8_t packet[SDL_GEARVR_PACKET_SIZE];
    BH_Harness *h;
    int mark;

    /* Gear VR writes 00 00 */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, false);
    BH_Start(h, false);
    BH_WriteAll(h);
    BH_Value(h, SDL_GEARVR_DATA, ack, sizeof(ack), false);
    BH_WriteAll(h);
    GearVRPacket(packet, 1, 160, 160, 0x09, 50);
    BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    CHECK(h->published && BH_Button(h, SDL_BLE_BUTTON_SOUTH));
    mark = h->nactions;
    Close(h);
    EXPECT_LOG(h, mark, "write1:0000+r+c");
    CHECK(Phase(h) == SDL_BLE_PHASE_CLOSING && h->published);
    /* Input during the close is not taken */
    GearVRPacket(packet, 2, 0, 0, 0x00, 50);
    BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    CHECK(BH_Button(h, SDL_BLE_BUTTON_SOUTH));
    /* A second close changes nothing */
    Close(h);
    EXPECT_LOG(h, mark, "write1:0000+r+c");
    CHECK(Phase(h) == SDL_BLE_PHASE_CLOSING && h->session.waiting);
    BH_Written(h, true);
    EXPECT_LOG(h, mark, "write1:0000+r+c remove disconnect");
    CHECK(!h->session.backoff && !h->published && BH_Controls(h)->buttons == 0);
    CHECK(BH_Axis(h, SDL_BLE_AXIS_RIGHT_TRIGGER) == -32768 && !BH_Controls(h)->finger);
    DESTROY(h);

    /* Myo writes 09 01 00 then 0A 01 00 */
    MyoProperties(properties);
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    BH_WriteAll(h);
    BH_Value(h, SDL_MYO_IMU, myo_imu_level, sizeof(myo_imu_level), false);
    mark = h->nactions;
    Close(h);
    BH_WriteAll(h);
    EXPECT_LOG(h, mark, "write3:090100+r+c write3:0a0100+r+c remove disconnect");
    DESTROY(h);

    /* Closed while a start-up write is out: the rest of the start-up is
       dropped, the closing writes follow the answer, and an unpublished
       joystick is not removed */
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    mark = h->nactions;
    Close(h);
    EXPECT_LOG(h, mark, "");
    BH_WriteAll(h);
    EXPECT_LOG(h, mark - 1, "write3:0103000301+r write3:090100+r+c write3:0a0100+r+c disconnect");
    DESTROY(h);

    /* A closing write that fails does not stop the close */
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    BH_WriteAll(h);
    mark = h->nactions;
    Close(h);
    BH_Written(h, false);
    BH_Written(h, false);
    EXPECT_LOG(h, mark, "write3:090100+r+c write3:0a0100+r+c disconnect");
    DESTROY(h);

    /* No closing writes: the end follows at once */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    BH_Start(h, false);
    mark = h->nactions;
    Close(h);
    EXPECT_LOG(h, mark, "disconnect");
    DESTROY(h);

    /* Closed before the start: no closing writes, and the answer out is
       dropped */
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    Close(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify disconnect");
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    EXPECT_LOG(h, 0, "connect discover sub0=notify disconnect");
    CHECK(!h->session.backoff);
    DESTROY(h);
    h = BH_Create(&probe_family, 0, NULL, false);
    Probe(h)->start_writes = 1;
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    Close(h);
    Read(h, ack, 1);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 disconnect");
    CHECK(Probe(h)->ncalls == 0);
    DESTROY(h);
    h = BH_Create(&SDL_BLEGHLiOSFamily, 0, NULL, false);
    Close(h);
    BH_Connected(h, true, false);
    EXPECT_LOG(h, 0, "connect disconnect");
    DESTROY(h);
    /* Closed after the end: nothing */
    h = BH_Create(&SDL_BLEGHLiOSFamily, 0, NULL, false);
    Lost(h);
    Close(h);
    EXPECT_LOG(h, 0, "connect disconnect");
    DESTROY(h);
}

/* Session flows: link loss while an answer is out drops the answer, and no
   answer reaches the module after the end */
static void TestFlowLost(void)
{
    static const uint8_t ack[2] = { 0x08, 0x00 };
    static const uint8_t level = 0x42;
    uint8_t packet[SDL_GEARVR_PACKET_SIZE];
    bool found[SDL_BLE_MAX_CHARS];
    uint8_t properties[SDL_BLE_MAX_CHARS];
    const SDL_ZwiftState *zwift;
    BH_Harness *h;
    int mark, calls;

    /* Zwift, lost while RideOn is out: the answer never reaches the module */
    h = BH_Create(&SDL_BLEZwiftFamily, SDL_ZWIFT_PLAY_LEFT, NULL, false);
    BH_Start(h, false);
    zwift = (const SDL_ZwiftState *)h->state;
    CHECK(zwift->timing && h->session.waiting);
    Lost(h);
    EXPECT_LOG(h, 4, "write2:526964654f6e disconnect");
    BH_Written(h, true);
    EXPECT_LOG(h, 4, "write2:526964654f6e disconnect");
    CHECK(zwift->timing);
    DESTROY(h);

    /* Gear VR, published, lost while a keep-alive is out */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, false);
    BH_Start(h, false);
    BH_WriteAll(h);
    BH_Value(h, SDL_GEARVR_DATA, ack, sizeof(ack), false);
    BH_WriteAll(h);
    GearVRPacket(packet, 2, 0, 0, 0x02, 60);
    BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    h->now = 10000;
    BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    mark = h->nactions - 1;
    EXPECT_LOG(h, mark, "write1:0400+r");
    Lost(h);
    BH_Written(h, true);
    EXPECT_LOG(h, mark, "write1:0400+r remove disconnect");
    CHECK(!BH_Button(h, SDL_BLE_BUTTON_GUIDE) && BH_Controls(h)->battery == 60);
    DESTROY(h);

    /* Lost during the close: the rest of the closing writes is dropped */
    h = BH_Create(&SDL_BLEMyoFamily, 0, NULL, false);
    MyoProperties(properties);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, properties);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    BH_WriteAll(h);
    BH_Value(h, SDL_MYO_IMU, myo_imu_level, sizeof(myo_imu_level), false);
    mark = h->nactions;
    Close(h);
    Lost(h);
    BH_WriteAll(h);
    EXPECT_LOG(h, mark, "write3:090100+r+c remove disconnect");
    /* A second loss changes nothing */
    Lost(h);
    EXPECT_LOG(h, mark, "write3:090100+r+c remove disconnect");
    DESTROY(h);

    /* After the end no answer reaches the module or moves the session, the
       one that was out included */
    h = BH_Create(&probe_family, 0, NULL, false);
    Probe(h)->start_writes = 2;
    BH_Start(h, false);
    Lost(h);
    calls = Probe(h)->ncalls;
    AllFound(found);
    BH_DefaultProperties(&probe_family, properties);
    SDL_BLESession_Written(&h->session, true, h->now);
    SDL_BLESession_Connected(&h->session, true, false, h->now);
    SDL_BLESession_Paired(&h->session, true, h->now);
    SDL_BLESession_BondRemoved(&h->session, true, h->now);
    SDL_BLESession_Discovered(&h->session, true, found, properties, h->now);
    SDL_BLESession_Subscribed(&h->session, SDL_BLE_STATUS_SUCCESS, 0, h->now);
    SDL_BLESession_Read(&h->session, true, &level, 1, h->now * 1000000);
    SDL_BLESession_Value(&h->session, PROBE_INPUT, &level, 1, false, h->now * 1000000);
    SDL_BLESession_Tick(&h->session, h->now + 100000);
    BH_Drain(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 write2:10+r disconnect");
    CHECK(Probe(h)->ncalls == calls && SDL_BLESession_Ended(&h->session) && !h->session.waiting);
    DESTROY(h);
}

/* Session flows: a link lost before the joystick appeared reports a failed
   connect, so the host holds the address SDL_BLE_CONNECT_RETRY_MS, in every
   phase from the connect to a close before the publish. A loss while a
   bond is removed or a pairing runs after a failed subscription reports
   nothing, so the next session pairs afresh at once. A loss after the
   publish reports the publish only. */
static void TestFlowLostBeforePublish(void)
{
    uint8_t face[SDL_OCULUSGO_REPORT_SIZE];
    uint8_t press = 0x01;
    BH_Harness *h;

    /* Connecting */
    h = BH_Create(&probe_family, 0, NULL, false);
    Lost(h);
    EXPECT_LOG(h, 0, "connect disconnect");
    EXPECT_OUTCOME(h, END_CONNECT);
    DESTROY(h);
    /* Pairing before discovery: the loss also counts as a failed pairing */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    CHECK(Phase(h) == SDL_BLE_PHASE_PAIRING);
    Lost(h);
    EXPECT_LOG(h, 0, "connect pair disconnect");
    EXPECT_OUTCOME(h, END_CONNECT | END_PAIRING);
    DESTROY(h);
    /* Discovering */
    h = BH_Create(&probe_family, 0, NULL, false);
    BH_Connected(h, true, false);
    CHECK(Phase(h) == SDL_BLE_PHASE_DISCOVERING);
    Lost(h);
    EXPECT_LOG(h, 0, "connect discover disconnect");
    EXPECT_OUTCOME(h, END_CONNECT);
    DESTROY(h);
    /* Subscribing */
    h = BH_Create(&probe_family, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    CHECK(Phase(h) == SDL_BLE_PHASE_SUBSCRIBING);
    Lost(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify disconnect");
    EXPECT_OUTCOME(h, END_CONNECT);
    DESTROY(h);
    /* Reading */
    h = BH_Create(&probe_family, 0, NULL, false);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    CHECK(Phase(h) == SDL_BLE_PHASE_READING);
    Lost(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 disconnect");
    EXPECT_OUTCOME(h, END_CONNECT);
    DESTROY(h);
    /* Starting, with a control held that is not ready */
    h = BH_Create(&probe_family, 0, NULL, false);
    BH_Start(h, false);
    Probe(h)->started = false;
    BH_Value(h, PROBE_INPUT, &press, 1, false);
    CHECK(Phase(h) == SDL_BLE_PHASE_STARTING && BH_Button(h, SDL_BLE_BUTTON_SOUTH) && !h->published);
    Lost(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 disconnect");
    CHECK(!BH_Button(h, SDL_BLE_BUTTON_SOUTH));
    EXPECT_OUTCOME(h, END_CONNECT);
    DESTROY(h);
    /* Closing before the publish */
    h = BH_Create(&probe_family, 0, NULL, false);
    BH_Start(h, false);
    Close(h);
    CHECK(Phase(h) == SDL_BLE_PHASE_CLOSING);
    Lost(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 write2:c0+r+c disconnect");
    EXPECT_OUTCOME(h, END_CONNECT);
    DESTROY(h);

    /* Removing a stale bond after Unreachable */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    CHECK(Phase(h) == SDL_BLE_PHASE_REPAIRING);
    Lost(h);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond disconnect");
    EXPECT_OUTCOME(h, 0);
    DESTROY(h);
    /* Pairing after a failed subscription: no wait, but a failed pairing */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    CHECK(Phase(h) == SDL_BLE_PHASE_REPAIRING);
    Lost(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify pair disconnect");
    EXPECT_OUTCOME(h, END_PAIRING);
    DESTROY(h);
    /* Pairing after the removal of a stale bond */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    BH_BondRemoved(h, true);
    CHECK(Phase(h) == SDL_BLE_PHASE_REPAIRING);
    Lost(h);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond pair disconnect");
    EXPECT_OUTCOME(h, END_PAIRING);
    DESTROY(h);
    /* The repeat after the pairing subscribes again, and a loss there
       reports a failed connect */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    BH_Paired(h, true);
    CHECK(Phase(h) == SDL_BLE_PHASE_SUBSCRIBING);
    Lost(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify pair sub0=none disconnect");
    EXPECT_OUTCOME(h, END_CONNECT);
    DESTROY(h);

    /* After the publish, in a resubscription and at rest */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    OculusGoFace(face, 0);
    BH_Value(h, SDL_OCULUSGO_FACE, face, sizeof(face), false);
    BH_Advance(h, 4000);
    CHECK(h->published && h->session.resubscribing && h->session.waiting);
    Lost(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify publish sub0=notify remove disconnect");
    EXPECT_OUTCOME(h, END_PUBLISHED);
    DESTROY(h);
    h = BH_Create(&probe_family, 0, NULL, false);
    BH_Start(h, false);
    BH_Value(h, PROBE_INPUT, &press, 1, false);
    Lost(h);
    EXPECT_OUTCOME(h, END_PUBLISHED);
    DESTROY(h);
}

/* Session flows: every answer arriving in every phase. Only the answer to
   the action out moves the session, and an answer when none is out is
   dropped. */
static void TestFlowStrays(void)
{
    static const uint8_t level = 0x42;
    uint8_t face[SDL_OCULUSGO_REPORT_SIZE];
    BH_Harness *h;

    /* Connecting, discovering, subscribing, reading, starting with a write
       out, running with nothing out, closing, ended */
    h = BH_Create(&probe_family, 0, NULL, false);
    Probe(h)->start_writes = 1;
    STRAYS(h, CONNECT);
    /* A value before any subscription is dropped */
    BH_Value(h, PROBE_INPUT, &level, 1, true);
    CHECK(ProbeCount(h, 'V') == 0);
    BH_Connected(h, true, false);
    STRAYS(h, DISCOVER);
    BH_Value(h, PROBE_INPUT, &level, 1, true);
    CHECK(ProbeCount(h, 'V') == 0);
    BH_Discovered(h, true, NULL);
    STRAYS(h, SUBSCRIBE);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    STRAYS(h, READ);
    Read(h, &level, 1);
    CHECK(Phase(h) == SDL_BLE_PHASE_STARTING);
    STRAYS(h, WRITE);
    CHECK(ProbeCount(h, 'D') == 0);
    BH_Written(h, true);
    CHECK(ProbeCount(h, 'D') == 1 && !h->session.waiting);
    STRAYS(h, NO_ANSWER);
    BH_Value(h, PROBE_INPUT, &level, 1, false);
    CHECK(Phase(h) == SDL_BLE_PHASE_RUNNING);
    STRAYS(h, NO_ANSWER);
    Close(h);
    STRAYS(h, WRITE);
    BH_Written(h, true);
    STRAYS(h, NO_ANSWER);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 write2:10+r publish write2:c0+r+c remove disconnect");
    CHECK(ProbeCount(h, 'D') == 2 && ProbeCount(h, 'V') == 2);
    DESTROY(h);

    /* Pairing before discovery */
    h = BH_Create(&SDL_BLEDaydreamFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    STRAYS(h, PAIR);
    BH_Paired(h, true);
    EXPECT_LOG(h, 0, "connect pair discover");
    DESTROY(h);

    /* Pairing and removing a bond after a failed subscription */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, true);
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_PROTOCOL_ERROR, SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION);
    STRAYS(h, PAIR);
    BH_Paired(h, true);
    EXPECT_LOG(h, 0, "connect discover sub0=notify pair sub0=none");
    DESTROY(h);
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, true);
    BH_Connected(h, true, true);
    BH_Discovered(h, true, NULL);
    BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
    STRAYS(h, REMOVE_BOND);
    BH_BondRemoved(h, true);
    EXPECT_LOG(h, 0, "connect discover sub0=none unbond sub0=notify");
    DESTROY(h);

    /* A resubscription while running */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    OculusGoFace(face, 0);
    BH_Value(h, SDL_OCULUSGO_FACE, face, sizeof(face), false);
    BH_Advance(h, 4000);
    STRAYS(h, SUBSCRIBE);
    BH_SubscribeAll(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify publish sub0=notify sub1=notify");
    /* Ended with nothing out */
    Lost(h);
    STRAYS(h, NO_ANSWER);
    DESTROY(h);

    /* Ended with a write out: its answer is dropped, and so is every other */
    h = BH_Create(&probe_family, 0, NULL, false);
    Probe(h)->start_writes = 1;
    BH_Start(h, false);
    Lost(h);
    STRAYS(h, WRITE);
    BH_Written(h, true);
    CHECK(ProbeCount(h, 'D') == 0);
    STRAYS(h, NO_ANSWER);
    DESTROY(h);

    /* A value while a bond is removed or a read is out reaches the module,
       and nothing is published before the start */
    {
        static const uint8_t press = 0x01;

        h = BH_Create(&probe_family, 0, NULL, true);
        BH_Connected(h, true, true);
        BH_Discovered(h, true, NULL);
        BH_Subscribed(h, SDL_BLE_STATUS_UNREACHABLE, 0);
        CHECK(Phase(h) == SDL_BLE_PHASE_REPAIRING);
        BH_Value(h, PROBE_INPUT, &press, 1, false);
        CHECK(ProbeCount(h, 'V') == 1 && BH_Button(h, SDL_BLE_BUTTON_SOUTH) && !h->published);
        DESTROY(h);
        h = BH_Create(&probe_family, 0, NULL, false);
        BH_Connected(h, true, false);
        BH_Discovered(h, true, NULL);
        BH_SubscribeAll(h);
        CHECK(Phase(h) == SDL_BLE_PHASE_READING);
        BH_Value(h, PROBE_INPUT, &press, 1, false);
        CHECK(ProbeCount(h, 'V') == 1 && BH_Button(h, SDL_BLE_BUTTON_SOUTH) && !h->published);
        DESTROY(h);
    }
}

/* Session flows: a module that gives up backs off */
static void TestFlowFailure(void)
{
    static const uint8_t press = 0x01, fail = 0xFF;
    BH_Harness *h;

    /* A Zwift record out of scope fails the start */
    h = BH_Create(&SDL_BLEZwiftFamily, 0x07, NULL, false);
    BH_Start(h, false);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify backoff disconnect");
    CHECK(h->session.backoff && !h->session.published);
    DESTROY(h);

    /* A published module that fails: removed, backed off, released */
    h = BH_Create(&probe_family, 0, NULL, false);
    BH_Start(h, false);
    BH_Value(h, PROBE_INPUT, &press, 1, false);
    CHECK(h->published && BH_Button(h, SDL_BLE_BUTTON_SOUTH));
    BH_Value(h, PROBE_INPUT, &fail, 1, false);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 publish remove backoff disconnect");
    CHECK(!BH_Button(h, SDL_BLE_BUTTON_SOUTH) && h->session.backoff && h->session.published);
    DESTROY(h);

    /* A module that fails before its start backs off at the start */
    h = BH_Create(&probe_family, 0, NULL, false);
    Probe(h)->start_writes = 1;
    BH_Connected(h, true, false);
    BH_Discovered(h, true, NULL);
    BH_Value(h, PROBE_INPUT, &fail, 1, false);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 backoff disconnect");
    CHECK(ProbeCount(h, 'S') == 1);
    DESTROY(h);

    /* A module that fails while a write is out ends at once, since the end
       awaits no answer, and the write's late answer is dropped */
    h = BH_Create(&probe_family, 0, NULL, false);
    Probe(h)->start_writes = 1;
    BH_Start(h, false);
    CHECK(h->session.waiting && BH_LastKind(h) == WRITE);
    BH_Value(h, PROBE_INPUT, &fail, 1, false);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 write2:10+r backoff disconnect");
    CHECK(SDL_BLESession_Ended(&h->session) && h->session.backoff && !h->published);
    BH_Written(h, true);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 write2:10+r backoff disconnect");
    CHECK(ProbeCount(h, 'D') == 0);
    DESTROY(h);

    /* A module that fails on a value releases at that value's time */
    h = BH_Create(&probe_family, 0, NULL, false);
    BH_Start(h, false);
    BH_Value(h, PROBE_INPUT, &press, 1, false);
    h->now = 7000;
    BH_Value(h, PROBE_INPUT, &fail, 1, false);
    CHECK(h->nsnapshots > 0 && h->snapshots[h->nsnapshots - 1].time_ns == 7000000000ull);
    CHECK(!BH_Button(h, SDL_BLE_BUTTON_SOUTH) && h->session.backoff);
    DESTROY(h);
}

/* Session flows: a gap releases every held control before its value applies */
static void TestFlowGap(void)
{
    uint8_t report[SDL_POKEBALL_REPORT_SIZE];
    uint8_t packet[SDL_GEARVR_PACKET_SIZE];
    BH_Harness *h;
    int mark;

    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    BH_Start(h, false);
    PokeballReport(report, 0, 0x01, 192, 36);
    h->now = 10;
    BH_Value(h, SDL_POKEBALL_INPUT, report, sizeof(report), false);
    CHECK(BH_Button(h, SDL_BLE_BUTTON_EAST) && BH_Axis(h, SDL_BLE_AXIS_LEFTX) == 32767 && BH_Axis(h, SDL_BLE_AXIS_LEFTY) == -32767);
    /* The same state after a gap: released, then held again */
    mark = h->nsnapshots;
    h->now = 20;
    BH_Value(h, SDL_POKEBALL_INPUT, report, sizeof(report), true);
    CHECK(h->nsnapshots == mark + 2);
    if (h->nsnapshots == mark + 2) {
        const SDL_BLEControls *released = &h->snapshots[mark].controls;

        CHECK(released->buttons == 0 && released->axes[SDL_BLE_AXIS_LEFTX] == 0 && released->axes[SDL_BLE_AXIS_LEFTY] == 0);
        CHECK(h->snapshots[mark].time_ns == 20000000 && h->snapshots[mark + 1].controls.buttons == (1u << SDL_BLE_BUTTON_EAST));
    }
    /* A gap before a value the module cannot use leaves the controls at rest */
    mark = h->nsnapshots;
    BH_Value(h, SDL_POKEBALL_INPUT, report, 3, true);
    CHECK(h->nsnapshots == mark + 1 && BH_Controls(h)->buttons == 0);
    /* A gap with nothing held emits nothing */
    mark = h->nsnapshots;
    BH_Value(h, SDL_POKEBALL_INPUT, report, 3, true);
    CHECK(h->nsnapshots == mark);
    DESTROY(h);

    /* A trigger returns to -32768 and the battery stays */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    BH_Start(h, false);
    OculusGoFace(packet, 0x04 | 0x08);
    BH_Value(h, SDL_OCULUSGO_FACE, packet, SDL_OCULUSGO_REPORT_SIZE, false);
    CHECK(BH_Axis(h, SDL_BLE_AXIS_RIGHT_TRIGGER) == 32767 && BH_Button(h, SDL_BLE_BUTTON_SOUTH));
    BH_Value(h, SDL_OCULUSGO_BEEF, packet, SDL_OCULUSGO_REPORT_SIZE, true);
    CHECK(BH_Axis(h, SDL_BLE_AXIS_RIGHT_TRIGGER) == -32768 && !BH_Button(h, SDL_BLE_BUTTON_SOUTH) && h->published);
    DESTROY(h);
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, false);
    BH_Start(h, false);
    GearVRPacket(packet, 1, 10, 10, 0x01, 77);
    BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    CHECK(BH_Controls(h)->battery == 77 && BH_Controls(h)->finger);
    BH_Value(h, SDL_GEARVR_DATA, packet, 2, true);
    CHECK(BH_Controls(h)->battery == 77 && !BH_Controls(h)->finger && BH_Axis(h, SDL_BLE_AXIS_RIGHT_TRIGGER) == -32768);
    /* A value for a characteristic the family lacks is dropped whole */
    mark = h->nsnapshots;
    GearVRPacket(packet, 1, 10, 10, 0x01, 77);
    BH_Value(h, SDL_GEARVR_DATA, packet, sizeof(packet), false);
    CHECK(h->nsnapshots == mark + 1);
    BH_Value(h, SDL_BLE_MAX_CHARS, packet, sizeof(packet), true);
    BH_Value(h, -1, packet, sizeof(packet), true);
    CHECK(h->nsnapshots == mark + 1 && BH_Controls(h)->finger);
    DESTROY(h);
}

/* The start timeout: a session not ready SDL_BLE_START_TIMEOUT_MS after its
   start ends with a backoff, so a device whose start-up write failed or that
   never streams is not held connected without a joystick. The end comes
   before a module timer due at the same time. A module that is ready is
   published at once, whatever answer is out, so the timeout never ends a
   device whose input has arrived. */
static void TestFlowStartTimeout(void)
{
    const uint64_t timeout = 1000 + SDL_BLE_START_TIMEOUT_MS;
    uint8_t report[SDL_POKEBALL_REPORT_SIZE];
    uint8_t face[SDL_OCULUSGO_REPORT_SIZE];
    uint64_t deadline, t;
    BH_Harness *h;
    int mark;

    /* A module without timers: the start deadline is the only one, 30 s
       after the start */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == timeout && deadline == 31000);
    mark = h->nactions;
    BH_Advance(h, timeout - 1);
    EXPECT_LOG(h, mark, "");
    BH_Advance(h, timeout);
    EXPECT_LOG(h, mark, "backoff disconnect");
    CHECK(TimeAt(h, mark) == timeout && h->session.backoff && !h->published);
    CHECK(!SDL_BLESession_GetDeadline(&h->session, &deadline));
    DESTROY(h);

    /* Input 1 ms before the timeout publishes, and the timeout is gone */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    memset(report, 0, sizeof(report));
    report[3] = 0x07;
    report[4] = 0x6C;
    h->now = timeout - 1;
    BH_Value(h, SDL_POKEBALL_INPUT, report, sizeof(report), false);
    CHECK(h->published && !SDL_BLESession_GetDeadline(&h->session, &deadline));
    mark = h->nactions;
    BH_Advance(h, 1000000);
    EXPECT_LOG(h, mark, "");
    DESTROY(h);

    /* Module timers earlier than the start deadline come first: an Oculus Go
       that stays silent writes its subscriptions again at 4 s and every
       second after, each answered. The timeout then ends the link, and the
       request due at 31 s too sends nothing, since the end comes first. */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 4000);
    for (t = 4000; t < timeout; t += SDL_OCULUSGO_RESUBSCRIBE_MS) {
        BH_Advance(h, t);
        BH_SubscribeAll(h);
    }
    CHECK(BH_Count(h, SUBSCRIBE) == 2 + 2 * 27 && !h->session.waiting && Phase(h) == SDL_BLE_PHASE_STARTING);
    CHECK(SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == timeout &&
          ((const SDL_OculusGoState *)h->state)->deadline == timeout);
    mark = h->nactions;
    BH_Advance(h, timeout);
    EXPECT_LOG(h, mark, "backoff disconnect");
    CHECK(TimeAt(h, mark) == timeout && h->session.backoff && !h->published);
    /* The module's timer did not run */
    CHECK(((const SDL_OculusGoState *)h->state)->deadline == timeout && !((const SDL_BLEBase *)h->state)->resubscribe);
    DESTROY(h);

    /* A module ready while an answer is out is published at once, since the
       publish awaits no answer: the same Oculus Go, whose resubscription at
       30 s still has its first descriptor write out when the first 8126FACE
       value arrives at 30.5 s. The joystick appears then, the tick at the
       timeout ends nothing, and the resubscription goes on. */
    h = BH_Create(&SDL_BLEOculusGoFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    for (t = 4000; t <= 30000; t += SDL_OCULUSGO_RESUBSCRIBE_MS) {
        BH_Advance(h, t);
        if (t < 30000) {
            BH_SubscribeAll(h);
        }
    }
    CHECK(BH_LastKind(h) == SUBSCRIBE && h->session.waiting && h->session.resubscribing &&
          Phase(h) == SDL_BLE_PHASE_STARTING);
    mark = h->nactions;
    OculusGoFace(face, 0);
    h->now = 30500;
    BH_Value(h, SDL_OCULUSGO_FACE, face, sizeof(face), false);
    EXPECT_LOG(h, mark, "publish");
    CHECK(h->published && TimeAt(h, mark) == 30500 && h->session.waiting && Phase(h) == SDL_BLE_PHASE_RUNNING);
    h->now = timeout;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    EXPECT_LOG(h, mark, "publish");
    CHECK(!SDL_BLESession_Ended(&h->session) && !h->session.backoff);
    h->now = timeout + 100;
    BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
    EXPECT_LOG(h, mark, "publish sub1=notify");
    BH_SubscribeAll(h);
    CHECK(Phase(h) == SDL_BLE_PHASE_RUNNING && !h->session.waiting && !h->session.resubscribing);
    DESTROY(h);

    /* A module that is ready at the timeout is published, not ended. The
       session publishes a ready module in every call that reaches the
       module, so the flag is set here by hand. */
    h = BH_Create(&probe_family, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    Probe(h)->base.ready = true;
    mark = h->nactions;
    BH_Advance(h, timeout);
    EXPECT_LOG(h, mark, "publish");
    CHECK(TimeAt(h, mark) == timeout && Phase(h) == SDL_BLE_PHASE_RUNNING && !h->session.backoff);
    DESTROY(h);

    /* A write still out at the timeout: the session ends, and the write's
       late answer is dropped */
    h = BH_Create(&SDL_BLEGearVRFamily, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    CHECK(BH_LastKind(h) == SDL_BLE_ACTION_WRITE && h->session.waiting);
    mark = h->nactions;
    h->now = timeout;
    SDL_BLESession_Tick(&h->session, h->now);
    BH_Drain(h);
    EXPECT_LOG(h, mark, "backoff disconnect");
    mark = h->nactions;
    BH_Written(h, true);
    EXPECT_LOG(h, mark, "");
    CHECK(SDL_BLESession_Ended(&h->session) && !h->session.waiting);
    DESTROY(h);
}

/* A value through the session at its own receive time, as an exact-size
   heap copy */
static void ValueAt(BH_Harness *h, int characteristic, const uint8_t *data, size_t length, bool gap, uint64_t time_ns)
{
    SDL_BLEValue value;

    memset(&value, 0, sizeof(value));
    value.characteristic = (uint8_t)characteristic;
    value.gap = gap;
    value.length = (uint16_t)length;
    value.time_ns = time_ns;
    memcpy(value.data, data, length);
    Deliver(h, &value);
}

/* The session's own releases never come before the change they undo. Values
   carry nanosecond receive times, while the session stamps a loss, a close,
   a timeout and a failure from its millisecond clock. */
static void TestFlowTimes(void)
{
    static const uint8_t press = 0x01, fail = 0xFF;
    uint8_t report[SDL_POKEBALL_REPORT_SIZE];
    uint64_t deadline;
    BH_Harness *h;

    /* A press at 5000.9 ms, then the link lost at 5000 ms: the release is
       stamped 5000.9 ms */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    h->now = 5000;
    BH_Start(h, false);
    PokeballReport(report, 0, 0x01, 112, 108);
    ValueAt(h, SDL_POKEBALL_INPUT, report, sizeof(report), false, 5000900000ull);
    CHECK(h->published && h->nsnapshots == 1 && h->snapshots[0].time_ns == 5000900000ull);
    Lost(h);
    CHECK(h->nsnapshots == 2);
    if (h->nsnapshots == 2) {
        CHECK(h->snapshots[1].controls.buttons == 0 && h->snapshots[1].time_ns == 5000900000ull);
    }
    DESTROY(h);

    /* A close in the same millisecond, and a release later than the press
       keeps its own time */
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    h->now = 5000;
    BH_Start(h, false);
    ValueAt(h, SDL_POKEBALL_INPUT, report, sizeof(report), false, 5000500000ull);
    Close(h);
    CHECK(h->nsnapshots == 2 && h->snapshots[1].time_ns == 5000500000ull);
    DESTROY(h);
    h = BH_Create(&SDL_BLEPokeballFamily, 0, NULL, false);
    h->now = 5000;
    BH_Start(h, false);
    ValueAt(h, SDL_POKEBALL_INPUT, report, sizeof(report), false, 5000500000ull);
    h->now = 5001;
    Lost(h);
    CHECK(h->nsnapshots == 2 && h->snapshots[1].time_ns == 5001000000ull);
    DESTROY(h);

    /* A press at 7000.7 ms, then a failure the pump sees at 7000 ms */
    h = BH_Create(&probe_family, 0, NULL, false);
    h->now = 7000;
    BH_Start(h, false);
    ValueAt(h, PROBE_INPUT, &press, 1, false, 7000700000ull);
    ValueAt(h, PROBE_INPUT, &fail, 1, false, 7000800000ull);
    CHECK(h->session.backoff && h->nsnapshots == 2);
    if (h->nsnapshots == 2) {
        CHECK(h->snapshots[1].controls.buttons == 0 && h->snapshots[1].time_ns == 7000700000ull);
    }
    DESTROY(h);

    /* The start timeout of a session whose module committed a change at
       31000.5 ms, while the clock reads 31000 */
    h = BH_Create(&probe_family, 0, NULL, false);
    h->now = 1000;
    BH_Start(h, false);
    Probe(h)->started = false;
    ValueAt(h, PROBE_INPUT, &press, 1, false, 31000500000ull);
    CHECK(!h->published && SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline == 31000);
    BH_Advance(h, 31000);
    CHECK(h->session.backoff && h->nsnapshots == 2);
    if (h->nsnapshots == 2) {
        CHECK(h->snapshots[1].controls.buttons == 0 && h->snapshots[1].time_ns == 31000500000ull);
    }
    DESTROY(h);
}

/* A family past the table's limits never runs: the session ends at once
   with only a disconnect, and its address waits as after a failed connect.
   A family at the limits runs. */
static void TestFlowLimits(void)
{
    static const uint8_t level = 0x42;
    SDL_BLEHost host;
    SDL_BLEAdvertisement ad;
    SDL_BLEMatch match = { -1, 0 };
    SDL_BLEFamily family;
    uint64_t deadline;
    bool found[SDL_BLE_MAX_CHARS];
    uint8_t properties[SDL_BLE_MAX_CHARS];
    BH_Harness *h;
    int which;

    CHECK(SDL_BLE_MAX_KEYS == 4 && SDL_BLE_MAX_SERVICES == 4 && SDL_BLE_MAX_CHARS == 8);
    AllFound(found);
    BH_DefaultProperties(&probe_family, properties);
    for (which = 0; which < 3; ++which) {
        family = probe_family;
        switch (which) {
        case 0:
            family.nkeys = SDL_BLE_MAX_KEYS + 1;
            break;
        case 1:
            family.nservices = SDL_BLE_MAX_SERVICES + 1;
            break;
        default:
            family.ncharacteristics = SDL_BLE_MAX_CHARS + 1;
            break;
        }
        h = BH_Create(&family, 0, NULL, true);
        EXPECT_LOG(h, 0, "disconnect");
        BH_CHECK(SDL_BLESession_Ended(&h->session) && !h->session.waiting && !h->session.backoff, "limit %d", which);
        EXPECT_OUTCOME(h, END_CONNECT);
        /* The module was reset, and nothing reaches it */
        BH_CHECK(strcmp(((const SDL_BLEBase *)h->state)->identity.name, "Probe") == 0 && Probe(h)->ncalls == 0,
                 "limit %d", which);
        STRAYS(h, NO_ANSWER);
        SDL_BLESession_Connected(&h->session, true, false, h->now);
        SDL_BLESession_Discovered(&h->session, true, found, properties, h->now);
        SDL_BLESession_Value(&h->session, PROBE_INPUT, &level, 1, false, h->now * 1000000);
        SDL_BLESession_Tick(&h->session, h->now + 100000);
        SDL_BLESession_Close(&h->session, h->now);
        SDL_BLESession_Lost(&h->session, h->now);
        BH_Drain(h);
        EXPECT_LOG(h, 0, "disconnect");
        BH_CHECK(Probe(h)->ncalls == 0 && !SDL_BLESession_GetDeadline(&h->session, &deadline), "limit %d", which);
        DESTROY(h);
    }

    /* The host holds the address SDL_BLE_CONNECT_RETRY_MS */
    SDL_BLEHost_Init(&host);
    ad = Ad(0xE1, SDL_BLE_AD_ADVERTISEMENT);
    AdName(&ad, "Pokemon PBP");
    CHECK(Offer(&host, &ad, 100, &match));
    family = probe_family;
    family.ncharacteristics = SDL_BLE_MAX_CHARS + 1;
    h = BH_Create(&family, 0, NULL, false);
    h->now = 100;
    Ended(&host, ad.address, h);
    CHECK(!Offer(&host, &ad, 5099, &match) && Offer(&host, &ad, 5100, &match));
    DESTROY(h);

    /* At the limits the session runs */
    family = probe_family;
    family.nkeys = SDL_BLE_MAX_KEYS;
    family.nservices = SDL_BLE_MAX_SERVICES;
    family.ncharacteristics = SDL_BLE_MAX_CHARS;
    h = BH_Create(&family, 0, NULL, false);
    EXPECT_LOG(h, 0, "connect");
    BH_Connected(h, true, false);
    EXPECT_LOG(h, 0, "connect discover");
    CHECK(Phase(h) == SDL_BLE_PHASE_DISCOVERING);
    DESTROY(h);
}

/* A queue nobody takes fills: the ninth action is dropped and counted, and
   the eight before it come out in order. The driver takes the actions after
   every batch of calls, so this does not happen there. */
static void TestFlowOverflow(void)
{
    bool found[SDL_BLE_MAX_CHARS];
    uint8_t properties[SDL_BLE_MAX_CHARS];
    BH_Harness *h;
    int i;

    AllFound(found);
    BH_DefaultProperties(&probe_family, properties);
    h = BH_Create(&probe_family, 0, NULL, false);
    Probe(h)->start_writes = 6;
    SDL_BLESession_Connected(&h->session, true, false, h->now);
    SDL_BLESession_Discovered(&h->session, true, found, properties, h->now);
    SDL_BLESession_Subscribed(&h->session, SDL_BLE_STATUS_SUCCESS, 0, h->now);
    SDL_BLESession_Subscribed(&h->session, SDL_BLE_STATUS_SUCCESS, 0, h->now);
    SDL_BLESession_Read(&h->session, false, NULL, 0, h->now * 1000000);
    for (i = 0; i < 3; ++i) {
        SDL_BLESession_Written(&h->session, true, h->now);
    }
    CHECK(h->session.action_count == SDL_BLE_MAX_SESSION_ACTIONS && h->session.actions_dropped == 0);
    SDL_BLESession_Written(&h->session, true, h->now);
    CHECK(h->session.action_count == SDL_BLE_MAX_SESSION_ACTIONS && h->session.actions_dropped == 1);
    SDL_BLESession_Written(&h->session, true, h->now);
    CHECK(h->session.actions_dropped == 2);
    BH_Drain(h);
    EXPECT_LOG(h, 0, "connect discover sub0=notify sub1=notify read1 write2:10+r write2:11 write2:12+r write2:13");
    BH_Destroy(h);
}

int main(void)
{
    Test1_Matcher();
    TestMatcherEdges();
    TestTranscription();
    TestContract();
    TestEmitTimes();
    Test2_OneConnect();
    TestHost();
    TestHostOutcomes();
    TestRetries();
    TestQueue();
    Test3_DescriptorValues();
    Test4_BondedNoneFirst();
    Test5_Pairing();
    Test6_ValueSizes();
    Test7_FullQueue();
    Test8_LinkLoss();
    Test9_Clock();
    TestFlowOrder();
    TestFlowCharacteristics();
    TestFlowReads();
    TestFlowPublish();
    TestFlowWrites();
    TestFlowResubscribe();
    TestFlowClose();
    TestFlowLost();
    TestFlowLostBeforePublish();
    TestFlowStrays();
    TestFlowFailure();
    TestFlowGap();
    TestFlowStartTimeout();
    TestFlowTimes();
    TestFlowLimits();
    TestFlowOverflow();
    return BH_Report("testblesession");
}
