/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "SDL_ble_session.h"

#include <string.h>

/* Matcher */

static size_t BLE_NameLength(const SDL_BLEAdvertisement *ad)
{
    size_t length = 0;

    while (length < sizeof(ad->name) && ad->name[length] != '\0') {
        ++length;
    }
    return length;
}

static bool BLE_NameMatches(const SDL_BLEMatchKey *key, const SDL_BLEAdvertisement *ad)
{
    size_t length, key_length, i;

    if (!ad->has_name || !key->name) {
        return false;
    }
    length = BLE_NameLength(ad);
    key_length = strlen(key->name);
    switch (key->kind) {
    case SDL_BLE_KEY_NAME_EQUALS:
        return length == key_length && memcmp(ad->name, key->name, key_length) == 0;
    case SDL_BLE_KEY_NAME_PREFIX:
        return length >= key_length && memcmp(ad->name, key->name, key_length) == 0;
    case SDL_BLE_KEY_NAME_CONTAINS:
        if (key_length > length) {
            return false;
        }
        for (i = 0; i + key_length <= length; ++i) {
            if (memcmp(&ad->name[i], key->name, key_length) == 0) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

static bool BLE_KeyMatches(const SDL_BLEMatchKey *key, const SDL_BLEAdvertisement *ad, uint8_t *variant)
{
    int i, t;

    switch (key->kind) {
    case SDL_BLE_KEY_NAME_EQUALS:
    case SDL_BLE_KEY_NAME_PREFIX:
    case SDL_BLE_KEY_NAME_CONTAINS:
        if (BLE_NameMatches(key, ad)) {
            *variant = 0;
            return true;
        }
        return false;
    case SDL_BLE_KEY_SERVICE:
        for (i = 0; i < ad->nservices && i < SDL_BLE_AD_SERVICES; ++i) {
            if (SDL_BLE_UUIDEqual(&ad->services[i], &key->uuid)) {
                *variant = 0;
                return true;
            }
        }
        return false;
    case SDL_BLE_KEY_MANUFACTURER:
        for (i = 0; i < ad->nmanufacturer && i < SDL_BLE_AD_MANUFACTURER; ++i) {
            const SDL_BLEManufacturerData *data = &ad->manufacturer[i];

            if (data->company != key->company) {
                continue;
            }
            if (key->ntypes == 0) {
                *variant = (data->length > 0) ? data->data[0] : 0;
                return true;
            }
            if (data->length == 0) {
                continue;
            }
            for (t = 0; t < key->ntypes && t < (int)sizeof(key->types); ++t) {
                if (data->data[0] == key->types[t]) {
                    *variant = data->data[0];
                    return true;
                }
            }
        }
        return false;
    default:
        return false;
    }
}

int SDL_BLE_Match(const SDL_BLEFamily *const *families, int nfamilies, const bool *enabled,
                  const SDL_BLEAdvertisement *ad, uint8_t *variant)
{
    int f, k;

    for (f = 0; f < nfamilies; ++f) {
        const SDL_BLEFamily *family = families[f];

        if (!family || (enabled && !enabled[f])) {
            continue;
        }
        for (k = 0; k < family->nkeys && k < SDL_BLE_MAX_KEYS; ++k) {
            uint8_t found = 0;

            if (family->keys[k].corroborating) {
                continue;
            }
            if (BLE_KeyMatches(&family->keys[k], ad, &found)) {
                if (variant) {
                    *variant = found;
                }
                return f;
            }
        }
    }
    return -1;
}

/* Host */

void SDL_BLEHost_Init(SDL_BLEHost *host)
{
    memset(host, 0, sizeof(*host));
}

static SDL_BLEHostEntry *BLE_FindHosted(SDL_BLEHost *host, uint64_t address)
{
    int i;

    for (i = 0; i < host->count; ++i) {
        if (host->entries[i].address == address) {
            return &host->entries[i];
        }
    }
    return NULL;
}

/* A free entry: a new one, or one without a session whose backoff is over */
static SDL_BLEHostEntry *BLE_NewHosted(SDL_BLEHost *host, uint64_t now)
{
    int i;

    if (host->count < SDL_BLE_MAX_HOSTED) {
        return &host->entries[host->count++];
    }
    for (i = 0; i < host->count; ++i) {
        if (!host->entries[i].active && now >= host->entries[i].until) {
            return &host->entries[i];
        }
    }
    return NULL;
}

bool SDL_BLEHost_Advertise(SDL_BLEHost *host, const SDL_BLEFamily *const *families, int nfamilies,
                           const bool *enabled, const SDL_BLEAdvertisement *ad, uint64_t now,
                           SDL_BLEMatch *match)
{
    SDL_BLEHostEntry *entry;
    uint8_t variant = 0;
    int family;

    family = SDL_BLE_Match(families, nfamilies, enabled, ad, &variant);
    if (family < 0) {
        return false;
    }
    entry = BLE_FindHosted(host, ad->address);
    if (entry) {
        if (entry->active || now < entry->until) {
            return false;
        }
    } else {
        entry = BLE_NewHosted(host, now);
        if (!entry) {
            return false;
        }
        memset(entry, 0, sizeof(*entry));
        entry->address = ad->address;
    }
    entry->active = true;
    if (match) {
        match->family = family;
        match->variant = variant;
    }
    return true;
}

void SDL_BLEHost_SessionEnded(SDL_BLEHost *host, uint64_t address, const SDL_BLEOutcome *outcome, uint64_t now)
{
    SDL_BLEHostEntry *entry = BLE_FindHosted(host, address);
    bool backoff = outcome->backoff;

    if (!entry) {
        return;
    }
    entry->active = false;
    if (outcome->published) {
        entry->failures = 0;
        entry->pairing_failures = 0;
        entry->until = 0;
    }
    if (outcome->pairing_failed) {
        /* The first two failed pairings are retried at the next
           advertisement, as the part's Oculus Go test 8 asks, since that
           controller often needs two or three tries
           (OculusGo-Air-Mouse-4-macOS README.md:49). From the third on, a
           device that never completes a pairing waits out the schedule,
           where it would otherwise be opened and paired again on every
           advertisement. */
        if (entry->pairing_failures < 255) {
            ++entry->pairing_failures;
        }
        if (entry->pairing_failures >= SDL_BLE_PAIRING_TRIES) {
            backoff = true;
        }
    }
    if (backoff) {
        /* 15, 30, 60, 120, 240, then 300 s, the Switch 2 driver's schedule
           (SDL_ble_switch2joystick.c:304-337) */
        const int shift = (entry->failures < 5) ? entry->failures : 5;
        uint64_t wait = (uint64_t)SDL_BLE_BACKOFF_MS << shift;

        if (wait > SDL_BLE_BACKOFF_MAX_MS) {
            wait = SDL_BLE_BACKOFF_MAX_MS;
        }
        entry->until = now + wait;
        if (entry->failures < 255) {
            ++entry->failures;
        }
    } else if (outcome->connect_failed) {
        /* A failed connect comes before the session learns anything about
           the device, so the wait is fixed and the schedule does not
           advance. Most links lost before the joystick appeared end the
           same way (SDL_BLESession_Lost) and wait as long. The Switch 2
           driver retries a failed open at the next advertisement
           (SDL_ble_switch2joystick.c:748-753). Here each attempt starts a
           connection with two threads and its queues, and advertisements
           can come every 20 ms (Bluetooth Core 5.4 Vol 6 Part B 4.4.2.2.1),
           so an address that advertises but cannot be opened waits 5 s: at
           most 12 attempts a minute, and still a retry well inside the
           first 15 s backoff for a device that was waking up. The macOS
           Oculus Go app also waits a fixed time after a failed connect, 2 s
           (OculusGo-Air-Mouse-4-macOS main.swift:616-630). */
        entry->until = now + SDL_BLE_CONNECT_RETRY_MS;
    }
}

void SDL_BLEHost_ClearBackoff(SDL_BLEHost *host)
{
    int i;

    for (i = 0; i < host->count; ++i) {
        SDL_BLEHostEntry *entry = &host->entries[i];

        if (!entry->active) {
            entry->until = 0;
            entry->failures = 0;
            entry->pairing_failures = 0;
        }
    }
}

bool SDL_BLEHost_IsActive(const SDL_BLEHost *host, uint64_t address)
{
    int i;

    for (i = 0; i < host->count; ++i) {
        if (host->entries[i].address == address) {
            return host->entries[i].active;
        }
    }
    return false;
}

/* Value queue */

void SDL_BLE_InitQueue(SDL_BLEValueQueue *queue, SDL_BLEValue *entries, int capacity, size_t value_size)
{
    memset(queue, 0, sizeof(*queue));
    queue->entries = entries;
    queue->capacity = (capacity > 0) ? capacity : 0;
    queue->value_size = (value_size < SDL_BLE_MAX_VALUE) ? value_size : SDL_BLE_MAX_VALUE;
}

SDL_BLEPushResult SDL_BLE_PushValue(SDL_BLEValueQueue *queue, int characteristic, const uint8_t *data,
                                    size_t length, uint64_t time_ns)
{
    SDL_BLEValue *value;

    if (length == 0 || !data) {
        ++queue->empty;
        return SDL_BLE_PUSH_EMPTY;
    }
    /* Never cut short. A value of an unexpected size is a loss, as the
       paddle reader treats one (SDL_xinput_paddle_gatt.cpp:139). */
    if (length > queue->value_size) {
        ++queue->too_long;
        queue->gap = true;
        return SDL_BLE_PUSH_TOO_LONG;
    }
    if (queue->count >= queue->capacity) {
        ++queue->full;
        queue->gap = true;
        return SDL_BLE_PUSH_FULL;
    }
    value = &queue->entries[(queue->head + queue->count) % queue->capacity];
    value->characteristic = (uint8_t)characteristic;
    value->gap = queue->gap;
    value->length = (uint16_t)length;
    value->time_ns = time_ns;
    memcpy(value->data, data, length);
    queue->gap = false;
    ++queue->count;
    return SDL_BLE_PUSH_QUEUED;
}

bool SDL_BLE_PopValue(SDL_BLEValueQueue *queue, SDL_BLEValue *value)
{
    const SDL_BLEValue *entry;

    if (queue->count == 0) {
        return false;
    }
    entry = &queue->entries[queue->head];
    value->characteristic = entry->characteristic;
    value->gap = entry->gap;
    value->length = entry->length;
    value->time_ns = entry->time_ns;
    memcpy(value->data, entry->data, entry->length);
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->count;
    return true;
}

void SDL_BLE_ClearQueue(SDL_BLEValueQueue *queue)
{
    queue->head = 0;
    queue->count = 0;
    queue->gap = false;
}

/* Session */

static SDL_BLEBase *BLE_Base(SDL_BLESession *session)
{
    return (SDL_BLEBase *)session->state;
}

static const SDL_BLEModule *BLE_Module(SDL_BLESession *session)
{
    return session->family->module;
}

static void BLE_Queue(SDL_BLESession *session, const SDL_BLEAction *action)
{
    if (session->action_count >= SDL_BLE_MAX_SESSION_ACTIONS) {
        /* Not reached: at most five actions wait (SDL_BLESession_NextAction).
           This layer has no assertion, so the count makes a breach visible,
           and the tests check it. */
        ++session->actions_dropped;
        return;
    }
    session->actions[(session->action_head + session->action_count) % SDL_BLE_MAX_SESSION_ACTIONS] = *action;
    ++session->action_count;
}

/* The action just queued is answered, and only its own answer is taken */
static void BLE_Await(SDL_BLESession *session, SDL_BLEActionKind kind)
{
    session->waiting = true;
    session->answer = (uint8_t)kind;
}

static void BLE_QueueKind(SDL_BLESession *session, SDL_BLEActionKind kind, bool answered)
{
    SDL_BLEAction action;

    memset(&action, 0, sizeof(action));
    action.kind = (uint8_t)kind;
    BLE_Queue(session, &action);
    if (answered) {
        BLE_Await(session, kind);
    }
}

static void BLE_QueueSubscribe(SDL_BLESession *session, int characteristic, uint8_t cccd)
{
    SDL_BLEAction action;

    memset(&action, 0, sizeof(action));
    action.kind = SDL_BLE_ACTION_SUBSCRIBE;
    action.characteristic = (uint8_t)characteristic;
    action.cccd = cccd;
    session->out_cccd = cccd;
    BLE_Queue(session, &action);
    BLE_Await(session, SDL_BLE_ACTION_SUBSCRIBE);
}

static void BLE_QueueRead(SDL_BLESession *session, int characteristic)
{
    SDL_BLEAction action;

    memset(&action, 0, sizeof(action));
    action.kind = SDL_BLE_ACTION_READ;
    action.characteristic = (uint8_t)characteristic;
    BLE_Queue(session, &action);
    BLE_Await(session, SDL_BLE_ACTION_READ);
}

static void BLE_QueueWrite(SDL_BLESession *session, const SDL_BLEWrite *write, bool closing)
{
    SDL_BLEAction action;

    memset(&action, 0, sizeof(action));
    action.kind = SDL_BLE_ACTION_WRITE;
    action.characteristic = write->characteristic;
    action.response = write->response;
    action.closing = closing;
    action.length = write->length;
    memcpy(action.data, write->data, write->length);
    BLE_Queue(session, &action);
    BLE_Await(session, SDL_BLE_ACTION_WRITE);
}

/* Ends the session: every control at rest, the joystick removed, then the
   device released. An answer still out is dropped when it comes. */
static void BLE_End(SDL_BLESession *session, bool backoff, uint64_t now)
{
    SDL_BLEBase *base = BLE_Base(session);

    SDL_BLE_ClearWrites(base);
    SDL_BLE_Release(base, now * 1000000);
    session->resubscribing = false;
    session->resubscribe_pending = false;
    if (session->published) {
        BLE_QueueKind(session, SDL_BLE_ACTION_REMOVE, false);
    }
    if (backoff) {
        session->backoff = true;
        BLE_QueueKind(session, SDL_BLE_ACTION_BACKOFF, false);
    }
    BLE_QueueKind(session, SDL_BLE_ACTION_DISCONNECT, false);
    session->phase = SDL_BLE_PHASE_ENDED;
}

/* Notify when the properties allow it, else indicate, else 0xFF */
static uint8_t BLE_WantedCCCD(uint8_t properties)
{
    if (properties & SDL_BLE_PROPERTY_NOTIFY) {
        return SDL_BLE_CCCD_NOTIFY;
    }
    if (properties & SDL_BLE_PROPERTY_INDICATE) {
        return SDL_BLE_CCCD_INDICATE;
    }
    return 0xFF;
}

static void BLE_Pump(SDL_BLESession *session, uint64_t now);

static void BLE_Start(SDL_BLESession *session, uint64_t now)
{
    session->phase = SDL_BLE_PHASE_STARTING;
    session->start_deadline = now + SDL_BLE_START_TIMEOUT_MS;
    BLE_Module(session)->Start(session->state, now);
    BLE_Pump(session, now);
}

static void BLE_ReadNext(SDL_BLESession *session, uint64_t now)
{
    const SDL_BLEFamily *family = session->family;

    for (; session->step < family->ncharacteristics; ++session->step) {
        const SDL_BLECharacteristic *characteristic = &family->characteristics[session->step];

        if ((characteristic->flags & SDL_BLE_CHAR_READ) && session->found[session->step] &&
            (session->properties[session->step] & SDL_BLE_PROPERTY_READ)) {
            BLE_QueueRead(session, session->step);
            return;
        }
    }
    BLE_Start(session, now);
}

static void BLE_SubscriptionsDone(SDL_BLESession *session, uint64_t now)
{
    if (session->resubscribing) {
        session->resubscribing = false;
        BLE_Pump(session, now);
        return;
    }
    session->phase = SDL_BLE_PHASE_READING;
    session->step = 0;
    BLE_ReadNext(session, now);
}

/* Queues the next descriptor write from session->step on. On a bond every
   subscription writes None first, as step 4 of the part's connection
   sequence asks, because Windows caches the descriptor per bond and can
   satisfy a write of the value it holds without reaching the device. The
   paddle reader writes None only when the descriptor it reads back already
   holds Notify (SDL_xinput_paddle_gatt.h:46-49,
   SDL_xinput_paddle_gatt.cpp:820-822). This session reads no descriptor, so
   it writes None on every bonded subscription. */
static void BLE_SubscribeNext(SDL_BLESession *session, uint64_t now)
{
    const SDL_BLEFamily *family = session->family;

    for (; session->step < family->ncharacteristics; ++session->step, session->none_written = false) {
        const SDL_BLECharacteristic *characteristic = &family->characteristics[session->step];
        uint8_t cccd;

        if (!(characteristic->flags & SDL_BLE_CHAR_SUBSCRIBE) || !session->found[session->step]) {
            continue;
        }
        cccd = BLE_WantedCCCD(session->properties[session->step]);
        if (cccd == 0xFF) {
            if (characteristic->flags & SDL_BLE_CHAR_OPTIONAL) {
                continue;
            }
            BLE_End(session, true, now);
            return;
        }
        if (session->bonded && !session->none_written) {
            BLE_QueueSubscribe(session, session->step, SDL_BLE_CCCD_NONE);
        } else {
            BLE_QueueSubscribe(session, session->step, cccd);
        }
        return;
    }
    BLE_SubscriptionsDone(session, now);
}

/* After a pairing or a removed bond, every subscription runs once more */
static void BLE_RepeatSubscriptions(SDL_BLESession *session, uint64_t now)
{
    session->repeated = true;
    session->phase = SDL_BLE_PHASE_SUBSCRIBING;
    session->step = 0;
    session->none_written = false;
    BLE_SubscribeNext(session, now);
}

/* Hands out whatever the module asks for. A failure ends the session and a
   ready module is published at once, whatever answer is out, since neither
   the end nor the publish awaits an answer. A write or a resubscription
   waits until no answer is out. */
static void BLE_Pump(SDL_BLESession *session, uint64_t now)
{
    SDL_BLEBase *base = BLE_Base(session);
    SDL_BLEWrite write;

    if (session->phase == SDL_BLE_PHASE_CLOSING) {
        if (session->waiting) {
            return;
        }
        if (SDL_BLE_NextWrite(base, &write)) {
            BLE_QueueWrite(session, &write, true);
            return;
        }
        BLE_End(session, false, now);
        return;
    }
    if (session->phase != SDL_BLE_PHASE_STARTING && session->phase != SDL_BLE_PHASE_RUNNING) {
        return;
    }
    if (base->failed) {
        BLE_End(session, true, now);
        return;
    }
    if (session->phase == SDL_BLE_PHASE_STARTING && base->ready) {
        session->phase = SDL_BLE_PHASE_RUNNING;
        session->published = true;
        BLE_QueueKind(session, SDL_BLE_ACTION_PUBLISH, false);
    }
    if (session->waiting) {
        return;
    }
    if (base->resubscribe) {
        base->resubscribe = false;
        session->resubscribe_pending = true;
    }
    if (session->resubscribe_pending && !session->resubscribing) {
        session->resubscribe_pending = false;
        session->resubscribing = true;
        session->step = 0;
        session->none_written = false;
        BLE_SubscribeNext(session, now);
        if (session->waiting || session->phase == SDL_BLE_PHASE_ENDED) {
            return;
        }
    }
    if (SDL_BLE_NextWrite(base, &write)) {
        BLE_QueueWrite(session, &write, false);
    }
}

void SDL_BLESession_Init(SDL_BLESession *session, const SDL_BLEFamily *family, void *state,
                         const SDL_BLESink *sink, const SDL_BLEModuleContext *context,
                         bool pairing_allowed)
{
    memset(session, 0, sizeof(*session));
    session->family = family;
    session->state = state;
    session->pairing_allowed = pairing_allowed;
    family->module->Reset(state, sink, context);
    if (family->nkeys > SDL_BLE_MAX_KEYS || family->nservices > SDL_BLE_MAX_SERVICES ||
        family->ncharacteristics > SDL_BLE_MAX_CHARS) {
        /* A table error. Every loop over the family's characteristics, and
           the transport's over its services, stays inside the arrays because
           no session runs for such a family. Only the disconnect goes out,
           since the driver's executor ends on nothing else, and the address
           waits as after a failed connect. */
        session->connect_failed = true;
        session->phase = SDL_BLE_PHASE_ENDED;
        BLE_QueueKind(session, SDL_BLE_ACTION_DISCONNECT, false);
        return;
    }
    session->phase = SDL_BLE_PHASE_CONNECTING;
    BLE_QueueKind(session, SDL_BLE_ACTION_CONNECT, true);
}

bool SDL_BLESession_NextAction(SDL_BLESession *session, SDL_BLEAction *action)
{
    if (session->action_count == 0) {
        return false;
    }
    *action = session->actions[session->action_head];
    session->action_head = (session->action_head + 1) % SDL_BLE_MAX_SESSION_ACTIONS;
    --session->action_count;
    return true;
}

void SDL_BLESession_GetOutcome(const SDL_BLESession *session, SDL_BLEOutcome *outcome)
{
    memset(outcome, 0, sizeof(*outcome));
    outcome->backoff = session->backoff;
    outcome->published = session->published;
    outcome->pairing_failed = session->pairing_failed;
    outcome->connect_failed = session->connect_failed;
}

/* True when the answer belongs to the session as it stands. An answer to an
   action that is not out is ignored and leaves the wait in place, so the
   answer that is out still lands. After the end, the answer that was out is
   taken and dropped. */
static bool BLE_TakeAnswer(SDL_BLESession *session, SDL_BLEActionKind kind)
{
    if (!session->waiting || session->answer != kind) {
        return false;
    }
    session->waiting = false;
    return session->phase != SDL_BLE_PHASE_ENDED;
}

void SDL_BLESession_Connected(SDL_BLESession *session, bool success, bool bonded, uint64_t now)
{
    if (!BLE_TakeAnswer(session, SDL_BLE_ACTION_CONNECT) || session->phase != SDL_BLE_PHASE_CONNECTING) {
        return;
    }
    if (!success) {
        /* No backoff: the host holds the address SDL_BLE_CONNECT_RETRY_MS */
        session->connect_failed = true;
        BLE_End(session, false, now);
        return;
    }
    session->bonded = bonded;
    if (session->family->pairing == SDL_BLE_PAIR_FIRST && !bonded) {
        if (!session->pairing_allowed) {
            /* Never published: it waits under the backoff until the user
               pairs it in Settings */
            BLE_End(session, true, now);
            return;
        }
        session->phase = SDL_BLE_PHASE_PAIRING;
        BLE_QueueKind(session, SDL_BLE_ACTION_PAIR, true);
        return;
    }
    session->phase = SDL_BLE_PHASE_DISCOVERING;
    BLE_QueueKind(session, SDL_BLE_ACTION_DISCOVER, true);
}

void SDL_BLESession_Paired(SDL_BLESession *session, bool success, uint64_t now)
{
    if (!BLE_TakeAnswer(session, SDL_BLE_ACTION_PAIR)) {
        return;
    }
    if (session->phase != SDL_BLE_PHASE_PAIRING && session->phase != SDL_BLE_PHASE_REPAIRING) {
        return;
    }
    if (!success) {
        /* No backoff: the host counts the failed pairing and retries the
           first ones at the next advertisement */
        session->pairing_failed = true;
        BLE_End(session, false, now);
        return;
    }
    session->paired = true;
    session->bonded = true;
    if (session->phase == SDL_BLE_PHASE_PAIRING) {
        session->phase = SDL_BLE_PHASE_DISCOVERING;
        BLE_QueueKind(session, SDL_BLE_ACTION_DISCOVER, true);
        return;
    }
    BLE_RepeatSubscriptions(session, now);
}

void SDL_BLESession_BondRemoved(SDL_BLESession *session, bool success, uint64_t now)
{
    if (!BLE_TakeAnswer(session, SDL_BLE_ACTION_REMOVE_BOND) || session->phase != SDL_BLE_PHASE_REPAIRING) {
        return;
    }
    if (!success) {
        BLE_End(session, true, now);
        return;
    }
    session->removed_bond = true;
    session->bonded = false;
    if (session->family->pairing == SDL_BLE_PAIR_FIRST || session->pair_after_removal) {
        /* A device that streams only over a bond, or one whose subscription
           asked for security, pairs afresh before the repeat */
        BLE_QueueKind(session, SDL_BLE_ACTION_PAIR, true);
        return;
    }
    BLE_RepeatSubscriptions(session, now);
}

void SDL_BLESession_Discovered(SDL_BLESession *session, bool success, const bool *found,
                               const uint8_t *properties, uint64_t now)
{
    const SDL_BLEFamily *family = session->family;
    int i;

    if (!BLE_TakeAnswer(session, SDL_BLE_ACTION_DISCOVER) || session->phase != SDL_BLE_PHASE_DISCOVERING) {
        return;
    }
    if (!success) {
        BLE_End(session, true, now);
        return;
    }
    for (i = 0; i < family->ncharacteristics && i < SDL_BLE_MAX_CHARS; ++i) {
        session->found[i] = found[i];
        session->properties[i] = found[i] ? properties[i] : 0;
        if (!found[i] && !(family->characteristics[i].flags & SDL_BLE_CHAR_OPTIONAL)) {
            BLE_End(session, true, now);
            return;
        }
    }
    session->phase = SDL_BLE_PHASE_SUBSCRIBING;
    session->step = 0;
    session->none_written = false;
    BLE_SubscribeNext(session, now);
}

void SDL_BLESession_Subscribed(SDL_BLESession *session, SDL_BLEStatus status, uint8_t protocol_error, uint64_t now)
{
    const SDL_BLEFamily *family = session->family;
    const SDL_BLECharacteristic *characteristic;
    bool security;

    if (!BLE_TakeAnswer(session, SDL_BLE_ACTION_SUBSCRIBE)) {
        return;
    }
    if (session->phase == SDL_BLE_PHASE_CLOSING) {
        BLE_Pump(session, now);
        return;
    }
    if (session->phase != SDL_BLE_PHASE_SUBSCRIBING && !session->resubscribing) {
        return;
    }
    if (status == SDL_BLE_STATUS_SUCCESS) {
        if (session->out_cccd == SDL_BLE_CCCD_NONE) {
            session->none_written = true;
        } else {
            ++session->step;
            session->none_written = false;
        }
        BLE_SubscribeNext(session, now);
        return;
    }
    characteristic = &family->characteristics[session->step];
    if (session->resubscribing || (characteristic->flags & SDL_BLE_CHAR_OPTIONAL)) {
        /* A running link keeps going. An optional characteristic is skipped. */
        ++session->step;
        session->none_written = false;
        BLE_SubscribeNext(session, now);
        return;
    }
    security = (status == SDL_BLE_STATUS_PROTOCOL_ERROR &&
                (protocol_error == SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION ||
                 protocol_error == SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION));
    if (security && session->bonded && !session->paired &&
        (family->pairing == SDL_BLE_PAIR_ON_ERROR || family->pairing == SDL_BLE_PAIR_FIRST) &&
        session->pairing_allowed && !session->removed_bond && !session->repeated) {
        /* A stale bond: Windows holds a bond the device no longer accepts,
           as when an Oculus Go was paired with its headset again, since a
           new pairing replaces its old one (OculusGo-Air-Mouse-4-macOS
           README.md:54). Pairing on top of it changes nothing, because
           SDL_BLEGATT_Pair counts AlreadyPaired as paired
           (MicrosoftDocs/winrt-api
           windows.devices.enumeration/devicepairingresultstatus.md:25-26),
           and the repeat would fail the same way. So the bond is removed,
           with the pairing hint on, the way the part removes a stale bond
           that Unreachable reveals after the Windows Gear VR driver
           (gear_vr_controller connection/notifications.rs:21-39, :70-75,
           connection/pairing.rs:113-152), and the device pairs afresh before
           the subscriptions run once more. */
        session->phase = SDL_BLE_PHASE_REPAIRING;
        session->pair_after_removal = true;
        BLE_QueueKind(session, SDL_BLE_ACTION_REMOVE_BOND, true);
        return;
    }
    if (security && family->pairing == SDL_BLE_PAIR_ON_ERROR && session->pairing_allowed &&
        !session->paired && !session->repeated) {
        session->phase = SDL_BLE_PHASE_REPAIRING;
        BLE_QueueKind(session, SDL_BLE_ACTION_PAIR, true);
        return;
    }
    if (status == SDL_BLE_STATUS_UNREACHABLE && session->bonded && !session->paired &&
        family->pairing != SDL_BLE_PAIR_NEVER && session->pairing_allowed &&
        !session->removed_bond && !session->repeated) {
        /* A stale bond, as the Windows Gear VR driver handles it */
        session->phase = SDL_BLE_PHASE_REPAIRING;
        BLE_QueueKind(session, SDL_BLE_ACTION_REMOVE_BOND, true);
        return;
    }
    BLE_End(session, true, now);
}

void SDL_BLESession_Read(SDL_BLESession *session, bool success, const uint8_t *data, size_t length, uint64_t time_ns)
{
    const uint64_t now = time_ns / 1000000;

    if (!BLE_TakeAnswer(session, SDL_BLE_ACTION_READ)) {
        return;
    }
    if (session->phase == SDL_BLE_PHASE_CLOSING) {
        BLE_Pump(session, now);
        return;
    }
    if (session->phase != SDL_BLE_PHASE_READING) {
        return;
    }
    if (success && data && length > 0) {
        BLE_Module(session)->Value(session->state, session->step, data, length, time_ns);
    }
    ++session->step;
    BLE_ReadNext(session, now);
}

void SDL_BLESession_Written(SDL_BLESession *session, bool success, uint64_t now)
{
    if (!BLE_TakeAnswer(session, SDL_BLE_ACTION_WRITE)) {
        return;
    }
    BLE_Module(session)->WriteDone(session->state, success, now);
    BLE_Pump(session, now);
}

void SDL_BLESession_Value(SDL_BLESession *session, int characteristic, const uint8_t *data, size_t length,
                          bool gap, uint64_t time_ns)
{
    SDL_BLEBase *base = BLE_Base(session);

    switch (session->phase) {
    case SDL_BLE_PHASE_SUBSCRIBING:
    case SDL_BLE_PHASE_REPAIRING:
    case SDL_BLE_PHASE_READING:
    case SDL_BLE_PHASE_STARTING:
    case SDL_BLE_PHASE_RUNNING:
        break;
    default:
        return;
    }
    if (characteristic < 0 || characteristic >= session->family->ncharacteristics) {
        return;
    }
    if (gap) {
        SDL_BLE_Release(base, time_ns);
    }
    BLE_Module(session)->Value(session->state, characteristic, data, length, time_ns);
    BLE_Pump(session, time_ns / 1000000);
}

void SDL_BLESession_Lost(SDL_BLESession *session, uint64_t now)
{
    if (session->phase == SDL_BLE_PHASE_ENDED) {
        return;
    }
    if (!session->published && session->phase != SDL_BLE_PHASE_REPAIRING) {
        /* A link lost before the joystick appeared waits as a failed connect
           does, SDL_BLE_CONNECT_RETRY_MS. Freed at once, a device that drops
           its link during discovery, the subscriptions, the reads or the
           start-up would be connected again at its next advertisement, each
           time with two threads and a GattSession. A loss in the repairing
           phase, while a bond is removed or a pairing runs after a failed
           subscription, frees the address at once: removing a bond can drop
           the link (bleak client.py:643 says unpairing disconnects), and the
           Oculus Go can drop it while pairing (OculusGo-Air-Mouse-4-macOS
           README.md:49), so the next session pairs afresh without a wait. */
        session->connect_failed = true;
    }
    if (session->waiting && session->answer == SDL_BLE_ACTION_PAIR) {
        /* The link dropped while a pairing was out, and the driver takes a
           loss before any answer, so the pairing's own failure never comes.
           It counts as a failed pairing, so a device that drops its link
           whenever it pairs still waits out the schedule from the third
           try, as one that refuses the pairing does. */
        session->pairing_failed = true;
    }
    BLE_End(session, false, now);
}

void SDL_BLESession_Close(SDL_BLESession *session, uint64_t now)
{
    SDL_BLEBase *base = BLE_Base(session);

    if (session->phase == SDL_BLE_PHASE_ENDED || session->phase == SDL_BLE_PHASE_CLOSING) {
        return;
    }
    if (session->phase == SDL_BLE_PHASE_STARTING || session->phase == SDL_BLE_PHASE_RUNNING) {
        SDL_BLE_ClearWrites(base);
        session->resubscribing = false;
        session->resubscribe_pending = false;
        session->phase = SDL_BLE_PHASE_CLOSING;
        BLE_Module(session)->Close(session->state, now);
        BLE_Pump(session, now);
        return;
    }
    BLE_End(session, false, now);
}

void SDL_BLESession_Tick(SDL_BLESession *session, uint64_t now)
{
    if (session->phase != SDL_BLE_PHASE_STARTING && session->phase != SDL_BLE_PHASE_RUNNING) {
        return;
    }
    if (session->phase == SDL_BLE_PHASE_STARTING && now >= session->start_deadline && !BLE_Base(session)->ready) {
        /* No input 30 s after the start: a start-up write failed or the
           device stays silent, such as a Daydream whose bond Windows holds
           but the controller lost. The address backs off and its next
           session starts over, where holding the link would leave the
           device connected with no joystick. The end comes before the
           module's timers, so a timer due at the same time sends nothing to
           a device about to be released. A ready module is not ended: the
           pump below publishes it. */
        BLE_End(session, true, now);
        return;
    }
    BLE_Module(session)->Tick(session->state, now);
    BLE_Pump(session, now);
}

bool SDL_BLESession_GetDeadline(SDL_BLESession *session, uint64_t *deadline)
{
    uint64_t module_deadline;
    bool have = false;

    if (session->phase != SDL_BLE_PHASE_STARTING && session->phase != SDL_BLE_PHASE_RUNNING) {
        return false;
    }
    if (BLE_Module(session)->GetDeadline(session->state, &module_deadline)) {
        SDL_BLE_EarlierDeadline(&have, deadline, module_deadline);
    }
    if (session->phase == SDL_BLE_PHASE_STARTING) {
        SDL_BLE_EarlierDeadline(&have, deadline, session->start_deadline);
    }
    return have;
}

bool SDL_BLESession_Ended(const SDL_BLESession *session)
{
    return session->phase == SDL_BLE_PHASE_ENDED;
}

const char *SDL_BLE_ActionName(int kind)
{
    switch (kind) {
    case SDL_BLE_ACTION_CONNECT:
        return "connect";
    case SDL_BLE_ACTION_PAIR:
        return "pair";
    case SDL_BLE_ACTION_REMOVE_BOND:
        return "remove bond";
    case SDL_BLE_ACTION_DISCOVER:
        return "discover";
    case SDL_BLE_ACTION_SUBSCRIBE:
        return "subscribe";
    case SDL_BLE_ACTION_READ:
        return "read";
    case SDL_BLE_ACTION_WRITE:
        return "write";
    case SDL_BLE_ACTION_PUBLISH:
        return "publish";
    case SDL_BLE_ACTION_REMOVE:
        return "remove";
    case SDL_BLE_ACTION_BACKOFF:
        return "back off";
    case SDL_BLE_ACTION_DISCONNECT:
        return "disconnect";
    default:
        return "?";
    }
}
