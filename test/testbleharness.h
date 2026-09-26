/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* A fake executor for the Bluetooth LE GATT session and its modules
 * (hifihedgehog/SDL#33 Part 12). It logs every action the session hands out,
 * answers them as a test tells it, records every control change and sensor
 * sample, and feeds values as exact-size heap copies, so AddressSanitizer
 * sees any read past a value. The harness fails the run itself when a log
 * entry does not fit, when a helper gives up, when hex text is malformed and
 * when a run makes no check. */

#ifndef testbleharness_h_
#define testbleharness_h_

#include "SDL_ble_session.h"
#include "SDL_ble_devices.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BH_MAX_LOG   4096   /* Actions, snapshots and samples one harness logs */
#define BH_MAX_TICKS 100000 /* Ticks one BH_Advance runs before it fails */

static int bh_checks;
static int bh_failures;
static int bh_overflows; /* Log entries past BH_MAX_LOG, which BH_Report fails */

#define BH_CHECK(condition, ...)                                   \
    do {                                                           \
        ++bh_checks;                                               \
        if (!(condition)) {                                        \
            ++bh_failures;                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
            printf(__VA_ARGS__);                                   \
            printf("\n");                                          \
        }                                                          \
    } while (0)

/* A failure the harness finds itself, counted as a failed check */
#define BH_FAIL(...)                                               \
    do {                                                           \
        ++bh_checks;                                               \
        ++bh_failures;                                             \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);                \
        printf(__VA_ARGS__);                                       \
        printf("\n");                                              \
    } while (0)

/* Returns the process exit code. A run fails on a failed check, on a log
   entry that did not fit and when it made no check. */
static int BH_Report(const char *name)
{
    if (bh_checks == 0) {
        BH_FAIL("%s made no check", name);
    }
    if (bh_overflows) {
        BH_FAIL("%d log entries did not fit in the harness's %d", bh_overflows, BH_MAX_LOG);
    }
    printf("%s: %d checks, %d failures\n", name, bh_checks, bh_failures);
    return bh_failures ? 1 : 0;
}

typedef struct BH_Snapshot
{
    SDL_BLEControls controls;
    uint64_t time_ns;
} BH_Snapshot;

typedef struct BH_Sample
{
    int sensor;
    uint64_t time_ns;
    uint64_t sensor_ns;
    float data[3];
} BH_Sample;

typedef struct BH_Logged
{
    SDL_BLEAction action;
    uint64_t now; /* The harness clock when the action was taken */
} BH_Logged;

typedef struct BH_Harness
{
    SDL_BLESession session;
    const SDL_BLEFamily *family;
    void *state;
    uint64_t now;
    int nactions;
    BH_Logged actions[BH_MAX_LOG];
    int nsnapshots;
    BH_Snapshot snapshots[BH_MAX_LOG];
    int nsamples;
    BH_Sample samples[BH_MAX_LOG];
    int published; /* 1 between a publish and a remove */
} BH_Harness;

static void BH_Changed(void *userdata, const SDL_BLEControls *controls, uint64_t time_ns)
{
    BH_Harness *h = (BH_Harness *)userdata;

    if (h->nsnapshots == BH_MAX_LOG) {
        ++bh_overflows;
        return;
    }
    h->snapshots[h->nsnapshots].controls = *controls;
    h->snapshots[h->nsnapshots].time_ns = time_ns;
    ++h->nsnapshots;
}

static void BH_Sensor(void *userdata, int sensor, uint64_t time_ns, uint64_t sensor_ns, const float *data)
{
    BH_Harness *h = (BH_Harness *)userdata;
    BH_Sample *sample;

    if (h->nsamples == BH_MAX_LOG) {
        ++bh_overflows;
        return;
    }
    sample = &h->samples[h->nsamples++];
    sample->sensor = sensor;
    sample->time_ns = time_ns;
    sample->sensor_ns = sensor_ns;
    memcpy(sample->data, data, sizeof(sample->data));
}

static void BH_Log(void *userdata, const char *text)
{
    (void)userdata;
    printf("  log: %s\n", text);
}

/* Takes every queued action into the log */
static int BH_Drain(BH_Harness *h)
{
    SDL_BLEAction action;
    int count = 0;

    while (SDL_BLESession_NextAction(&h->session, &action)) {
        if (h->nactions < BH_MAX_LOG) {
            h->actions[h->nactions].action = action;
            h->actions[h->nactions].now = h->now;
            ++h->nactions;
        } else {
            ++bh_overflows;
        }
        if (action.kind == SDL_BLE_ACTION_PUBLISH) {
            h->published = 1;
        } else if (action.kind == SDL_BLE_ACTION_REMOVE) {
            h->published = 0;
        }
        ++count;
    }
    return count;
}

/* A harness on the heap, the session started and its connect taken */
static BH_Harness *BH_Create(const SDL_BLEFamily *family, uint8_t variant, const void *crypto, bool pairing_allowed)
{
    BH_Harness *h = (BH_Harness *)calloc(1, sizeof(*h));
    SDL_BLEModuleContext context;
    SDL_BLESink sink;

    if (!h) {
        printf("out of memory\n");
        exit(2);
    }
    h->family = family;
    h->state = calloc(1, family->module->state_size);
    if (!h->state) {
        printf("out of memory\n");
        exit(2);
    }
    memset(&context, 0, sizeof(context));
    context.variant = variant;
    context.crypto = crypto;
    sink.userdata = h;
    sink.changed = BH_Changed;
    sink.sensor = BH_Sensor;
    sink.log = BH_Log;
    SDL_BLESession_Init(&h->session, family, h->state, &sink, &context, pairing_allowed);
    BH_Drain(h);
    return h;
}

static void BH_Destroy(BH_Harness *h)
{
    free(h->state);
    free(h);
}

static const SDL_BLEAction *BH_Last(const BH_Harness *h)
{
    return h->nactions ? &h->actions[h->nactions - 1].action : NULL;
}

static int BH_LastKind(const BH_Harness *h)
{
    return h->nactions ? h->actions[h->nactions - 1].action.kind : -1;
}

static int BH_Count(const BH_Harness *h, int kind)
{
    int i, count = 0;

    for (i = 0; i < h->nactions; ++i) {
        if (h->actions[i].action.kind == kind) {
            ++count;
        }
    }
    return count;
}

/* True when the action at index has that kind and, for a write, those bytes */
static bool BH_IsWrite(const BH_Harness *h, int index, const uint8_t *data, size_t length)
{
    const SDL_BLEAction *action;

    if (index < 0 || index >= h->nactions) {
        return false;
    }
    action = &h->actions[index].action;
    return action->kind == SDL_BLE_ACTION_WRITE && action->length == length && memcmp(action->data, data, length) == 0;
}

/* Writes queued so far, as indices into the log, up to max */
static int BH_Writes(const BH_Harness *h, int *indices, int max)
{
    int i, count = 0;

    for (i = 0; i < h->nactions; ++i) {
        if (h->actions[i].action.kind == SDL_BLE_ACTION_WRITE) {
            if (count < max) {
                indices[count] = i;
            }
            ++count;
        }
    }
    return count;
}

/* Properties a device of the family would report: notify on a subscribed
   characteristic, read as well where it is read, write elsewhere */
static void BH_DefaultProperties(const SDL_BLEFamily *family, uint8_t *properties)
{
    int i;

    for (i = 0; i < SDL_BLE_MAX_CHARS; ++i) {
        uint8_t flags = (i < family->ncharacteristics) ? family->characteristics[i].flags : 0;

        properties[i] = 0;
        if (flags & SDL_BLE_CHAR_SUBSCRIBE) {
            properties[i] |= SDL_BLE_PROPERTY_NOTIFY;
        } else {
            properties[i] |= SDL_BLE_PROPERTY_WRITE;
        }
        if (flags & SDL_BLE_CHAR_READ) {
            properties[i] |= SDL_BLE_PROPERTY_READ;
        }
    }
}

static void BH_Connected(BH_Harness *h, bool success, bool bonded)
{
    SDL_BLESession_Connected(&h->session, success, bonded, h->now);
    BH_Drain(h);
}

static void BH_Paired(BH_Harness *h, bool success)
{
    SDL_BLESession_Paired(&h->session, success, h->now);
    BH_Drain(h);
}

static void BH_BondRemoved(BH_Harness *h, bool success)
{
    SDL_BLESession_BondRemoved(&h->session, success, h->now);
    BH_Drain(h);
}

/* Every characteristic found, with properties, or the defaults when NULL */
static void BH_Discovered(BH_Harness *h, bool success, const uint8_t *properties)
{
    bool found[SDL_BLE_MAX_CHARS];
    uint8_t defaults[SDL_BLE_MAX_CHARS];
    int i;

    for (i = 0; i < SDL_BLE_MAX_CHARS; ++i) {
        found[i] = true;
    }
    if (!properties) {
        BH_DefaultProperties(h->family, defaults);
        properties = defaults;
    }
    SDL_BLESession_Discovered(&h->session, success, found, properties, h->now);
    BH_Drain(h);
}

static void BH_Subscribed(BH_Harness *h, SDL_BLEStatus status, uint8_t protocol_error)
{
    SDL_BLESession_Subscribed(&h->session, status, protocol_error, h->now);
    BH_Drain(h);
}

/* The answering loops below go on while the session awaits an answer of
   their kind. The session can log a publish while an answer is out, so the
   last logged action does not tell. A loop that has given BH_MAX_LOG
   answers, as many as the log holds, stops and fails. */

/* True while the session, not ended, awaits an answer of this kind */
static bool BH_Awaits(const BH_Harness *h, int kind)
{
    return h->session.waiting && h->session.answer == kind && h->session.phase != SDL_BLE_PHASE_ENDED;
}

/* Answers subscriptions with success while one is awaited */
static int BH_SubscribeAll(BH_Harness *h)
{
    int count = 0;

    while (BH_Awaits(h, SDL_BLE_ACTION_SUBSCRIBE)) {
        if (count == BH_MAX_LOG) {
            BH_FAIL("BH_SubscribeAll gave up after %d answers", count);
            break;
        }
        BH_Subscribed(h, SDL_BLE_STATUS_SUCCESS, 0);
        ++count;
    }
    return count;
}

/* Answers reads while one is awaited: with the value, or as failed when
   data is NULL */
static int BH_ReadAll(BH_Harness *h, const uint8_t *data, size_t length)
{
    int count = 0;

    while (BH_Awaits(h, SDL_BLE_ACTION_READ)) {
        if (count == BH_MAX_LOG) {
            BH_FAIL("BH_ReadAll gave up after %d answers", count);
            break;
        }
        SDL_BLESession_Read(&h->session, data != NULL, data, length, h->now * 1000000);
        BH_Drain(h);
        ++count;
    }
    return count;
}

static void BH_Written(BH_Harness *h, bool success)
{
    SDL_BLESession_Written(&h->session, success, h->now);
    BH_Drain(h);
}

/* Answers writes with success while one is awaited */
static int BH_WriteAll(BH_Harness *h)
{
    int count = 0;

    while (BH_Awaits(h, SDL_BLE_ACTION_WRITE)) {
        if (count == BH_MAX_LOG) {
            BH_FAIL("BH_WriteAll gave up after %d answers", count);
            break;
        }
        BH_Written(h, true);
        ++count;
    }
    return count;
}

/* Connect, discovery with the default properties, every subscription, every
   read failing, so the module has started */
static void BH_Start(BH_Harness *h, bool bonded)
{
    BH_Connected(h, true, bonded);
    BH_Discovered(h, true, NULL);
    BH_SubscribeAll(h);
    BH_ReadAll(h, NULL, 0);
}

/* A value through the session, as an exact-size heap copy. An empty value is
   malloc(0), a block with no bytes, so AddressSanitizer reports a read of its
   byte 0. It may be NULL. */
static void BH_Value(BH_Harness *h, int characteristic, const uint8_t *data, size_t length, bool gap)
{
    uint8_t *copy = (uint8_t *)malloc(length);

    if (!copy && length) {
        printf("out of memory\n");
        exit(2);
    }
    if (length) {
        memcpy(copy, data, length);
    }
    SDL_BLESession_Value(&h->session, characteristic, copy, length, gap, h->now * 1000000);
    free(copy);
    BH_Drain(h);
}

/* Advances the clock to now and runs the timers that are due, one deadline
   at a time, as the connection's thread does. It fails after BH_MAX_TICKS
   ticks, since a deadline that never moves past now would hold it forever. */
static void BH_Advance(BH_Harness *h, uint64_t now)
{
    uint64_t deadline;
    int ticks = 0;

    while (SDL_BLESession_GetDeadline(&h->session, &deadline) && deadline <= now) {
        if (ticks == BH_MAX_TICKS) {
            BH_FAIL("BH_Advance to %llu gave up after %d ticks with a deadline at %llu", (unsigned long long)now,
                    ticks, (unsigned long long)deadline);
            break;
        }
        ++ticks;
        if (deadline > h->now) {
            h->now = deadline;
        }
        SDL_BLESession_Tick(&h->session, h->now);
        BH_Drain(h);
        BH_WriteAll(h);
    }
    h->now = now;
}

/* Parses hex like "5B EB FF", "5bebff" or a UUID's text form into bytes.
   Spaces and hyphens may stand between bytes. Any other character, a
   separator inside a byte and an odd trailing digit fail the test. Returns
   the count, which is more than max when the text holds more bytes. */
static size_t BH_Hex(const char *text, uint8_t *out, size_t max)
{
    const char *start = text;
    size_t count = 0;
    int high = -1;

    for (; *text; ++text) {
        int value;

        if (*text >= '0' && *text <= '9') {
            value = *text - '0';
        } else if (*text >= 'a' && *text <= 'f') {
            value = *text - 'a' + 10;
        } else if (*text >= 'A' && *text <= 'F') {
            value = *text - 'A' + 10;
        } else {
            if ((*text != ' ' && *text != '-') || high >= 0) {
                BH_FAIL("BH_Hex: character %d of \"%s\", 0x%02X, is neither a hex digit nor a separator between bytes",
                        (int)(text - start), start, (unsigned int)(unsigned char)*text);
            }
            continue;
        }
        if (high < 0) {
            high = value;
        } else {
            if (count < max) {
                out[count] = (uint8_t)((high << 4) | value);
            }
            ++count;
            high = -1;
        }
    }
    if (high >= 0) {
        BH_FAIL("BH_Hex: \"%s\" ends in half a byte", start);
    }
    return count;
}

/* The latest controls the module committed */
static const SDL_BLEControls *BH_Controls(const BH_Harness *h)
{
    return &((const SDL_BLEBase *)h->state)->controls;
}

static bool BH_Button(const BH_Harness *h, int button)
{
    return SDL_BLE_GetButton(BH_Controls(h), button);
}

static int16_t BH_Axis(const BH_Harness *h, int axis)
{
    return BH_Controls(h)->axes[axis];
}

#endif /* testbleharness_h_ */
