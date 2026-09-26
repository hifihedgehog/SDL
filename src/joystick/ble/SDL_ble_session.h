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

/* Everything the Bluetooth LE GATT joystick driver decides, in pure C99 with
 * no SDL runtime and no I/O (hifihedgehog/SDL#33 Part 12):
 * - the matcher, which finds a family in an advertisement or scan response,
 * - the host, which reserves each address for one session and makes it wait
 *   after a failed one,
 * - the value queue between the GATT callback and the connection's thread,
 * - the session, one per connection, which orders every connect, pairing,
 *   discovery, subscription, read, write, publish and removal as actions.
 * The WinRT driver only executes the actions and reports their results.
 */

#ifndef SDL_ble_session_h_
#define SDL_ble_session_h_

#include "SDL_ble_proto.h"

#define SDL_BLE_QUEUE_CAPACITY       1024 /* Values one connection holds, the paddle reader's capacity */
#define SDL_BLE_MAX_VALUE            128  /* Bytes one queued value holds, above the 72 of a Zwift key reply */
#define SDL_BLE_AD_NAME_LENGTH       64
#define SDL_BLE_AD_SERVICES          8
#define SDL_BLE_AD_MANUFACTURER      4
#define SDL_BLE_AD_MANUFACTURER_DATA 32
#define SDL_BLE_MAX_HOSTED           16     /* Addresses the host tracks */
#define SDL_BLE_BACKOFF_MS           15000  /* The first backoff, doubled each time */
#define SDL_BLE_BACKOFF_MAX_MS       300000
#define SDL_BLE_CONNECT_RETRY_MS     5000   /* The fixed wait after a failed connect */
#define SDL_BLE_PAIRING_TRIES        3      /* Failed pairings before the backoff applies */
#define SDL_BLE_MAX_SESSION_ACTIONS  8
#define SDL_BLE_START_TIMEOUT_MS     30000  /* A session not ready this long after its start ends */

/* GattClientCharacteristicConfigurationDescriptorValue */
#define SDL_BLE_CCCD_NONE     0
#define SDL_BLE_CCCD_NOTIFY   1
#define SDL_BLE_CCCD_INDICATE 2

/* ATT error codes of the Bluetooth Core specification, which Windows reports
   through GattProtocolError */
#define SDL_BLE_ATT_INSUFFICIENT_AUTHENTICATION 0x05
#define SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION     0x0F

typedef enum SDL_BLEAdvertisementKind
{
    SDL_BLE_AD_ADVERTISEMENT,
    SDL_BLE_AD_SCAN_RESPONSE
} SDL_BLEAdvertisementKind;

typedef struct SDL_BLEManufacturerData
{
    uint16_t company;
    uint8_t length; /* Bytes held, at most SDL_BLE_AD_MANUFACTURER_DATA */
    uint8_t data[SDL_BLE_AD_MANUFACTURER_DATA]; /* After the company ID */
} SDL_BLEManufacturerData;

/* One Received event of the advertisement watcher */
typedef struct SDL_BLEAdvertisement
{
    uint64_t address;
    uint8_t kind; /* SDL_BLEAdvertisementKind */
    bool has_name;
    char name[SDL_BLE_AD_NAME_LENGTH]; /* UTF-8 */
    int nservices;
    SDL_BLEUUID services[SDL_BLE_AD_SERVICES];
    int nmanufacturer;
    SDL_BLEManufacturerData manufacturer[SDL_BLE_AD_MANUFACTURER];
} SDL_BLEAdvertisement;

typedef struct SDL_BLEMatch
{
    int family;      /* Index into the families */
    uint8_t variant; /* SDL_BLEModuleContext.variant */
} SDL_BLEMatch;

/* The first enabled family with a key that matches alone, or -1. enabled may
   be NULL for every family. */
extern int SDL_BLE_Match(const SDL_BLEFamily *const *families, int nfamilies, const bool *enabled,
                         const SDL_BLEAdvertisement *ad, uint8_t *variant);

typedef struct SDL_BLEHostEntry
{
    uint64_t address;
    bool active;       /* A session runs for it */
    uint64_t until;    /* No session starts before this time */
    uint8_t failures;  /* Backoffs in a row */
    uint8_t pairing_failures; /* Failed pairings since its last publish or SDL_BLEHost_ClearBackoff */
} SDL_BLEHostEntry;

typedef struct SDL_BLEHost
{
    int count;
    SDL_BLEHostEntry entries[SDL_BLE_MAX_HOSTED];
} SDL_BLEHost;

/* How a session ended, as the host weighs it */
typedef struct SDL_BLEOutcome
{
    bool backoff;        /* The session ended with a backoff */
    bool published;      /* Its joystick appeared */
    bool pairing_failed; /* A pairing failed, or the link dropped while one was out, and ended it */
    bool connect_failed; /* The connect failed, no session could run, or the link dropped before the joystick
                            appeared, in any phase but SDL_BLE_PHASE_REPAIRING */
} SDL_BLEOutcome;

extern void SDL_BLEHost_Init(SDL_BLEHost *host);
/* True when a session is to start for this advertisement: its address
   matched an enabled family, has no session and is not waiting. The address
   is reserved until SDL_BLEHost_SessionEnded. */
extern bool SDL_BLEHost_Advertise(SDL_BLEHost *host, const SDL_BLEFamily *const *families, int nfamilies,
                                  const bool *enabled, const SDL_BLEAdvertisement *ad, uint64_t now,
                                  SDL_BLEMatch *match);
/* A session has ended and its address is free once its wait is over:
   - A publish clears the schedule and the failed pairings.
   - A backoff arms the next step of the schedule, 15 s doubling to 300 s.
   - A failed pairing counts. Once SDL_BLE_PAIRING_TRIES have failed since
     the last publish or SDL_BLEHost_ClearBackoff, each arms the schedule as
     a backoff does.
   - Without a backoff, a failed connect, which includes most links lost
     before the joystick appeared (SDL_BLESession_Lost), waits
     SDL_BLE_CONNECT_RETRY_MS, and the schedule does not advance. */
extern void SDL_BLEHost_SessionEnded(SDL_BLEHost *host, uint64_t address, const SDL_BLEOutcome *outcome, uint64_t now);
/* Every address without a session waits no more, and its schedule and its
   failed pairings start over. An address with a session keeps its
   reservation and its counts. The driver calls it when the pairing hint
   changes, since a wait earned under the old setting says nothing under the
   new one. */
extern void SDL_BLEHost_ClearBackoff(SDL_BLEHost *host);
extern bool SDL_BLEHost_IsActive(const SDL_BLEHost *host, uint64_t address);

/* One value from a characteristic, as the connection's queue holds it */
typedef struct SDL_BLEValue
{
    uint8_t characteristic;
    bool gap;          /* Values were lost before this one */
    uint16_t length;
    uint64_t time_ns;  /* Receive time */
    uint8_t data[SDL_BLE_MAX_VALUE];
} SDL_BLEValue;

typedef enum SDL_BLEPushResult
{
    SDL_BLE_PUSH_QUEUED,
    SDL_BLE_PUSH_FULL,     /* Dropped: the queue is full */
    SDL_BLE_PUSH_TOO_LONG, /* Dropped: longer than the queue's value size */
    SDL_BLE_PUSH_EMPTY     /* Ignored: no bytes */
} SDL_BLEPushResult;

/* A first-in, first-out queue. A dropped value marks a gap, and the next
   queued value carries the gap flag, so the flag lands after every value
   queued before the loss. The caller serializes access. */
typedef struct SDL_BLEValueQueue
{
    SDL_BLEValue *entries;
    int capacity;
    size_t value_size; /* At most SDL_BLE_MAX_VALUE */
    int head;
    int count;
    bool gap;
    uint32_t full;     /* Values dropped on a full queue */
    uint32_t too_long; /* Values longer than value_size */
    uint32_t empty;    /* Values with no bytes */
} SDL_BLEValueQueue;

extern void SDL_BLE_InitQueue(SDL_BLEValueQueue *queue, SDL_BLEValue *entries, int capacity, size_t value_size);
extern SDL_BLEPushResult SDL_BLE_PushValue(SDL_BLEValueQueue *queue, int characteristic, const uint8_t *data,
                                           size_t length, uint64_t time_ns);
extern bool SDL_BLE_PopValue(SDL_BLEValueQueue *queue, SDL_BLEValue *value);
/* Empties the queue and clears the gap */
extern void SDL_BLE_ClearQueue(SDL_BLEValueQueue *queue);

typedef enum SDL_BLEActionKind
{
    SDL_BLE_ACTION_CONNECT,     /* Open the device by address. Answer: SDL_BLESession_Connected */
    SDL_BLE_ACTION_PAIR,        /* Answer: SDL_BLESession_Paired */
    SDL_BLE_ACTION_REMOVE_BOND, /* Answer: SDL_BLESession_BondRemoved */
    SDL_BLE_ACTION_DISCOVER,    /* Answer: SDL_BLESession_Discovered */
    SDL_BLE_ACTION_SUBSCRIBE,   /* characteristic, cccd. Answer: SDL_BLESession_Subscribed */
    SDL_BLE_ACTION_READ,        /* characteristic. Answer: SDL_BLESession_Read */
    SDL_BLE_ACTION_WRITE,       /* characteristic, data, response. Answer: SDL_BLESession_Written */
    SDL_BLE_ACTION_PUBLISH,     /* The joystick appears */
    SDL_BLE_ACTION_REMOVE,      /* The joystick goes */
    SDL_BLE_ACTION_BACKOFF,     /* The address backs off before its next session */
    SDL_BLE_ACTION_DISCONNECT   /* Release the device. The session has ended. */
} SDL_BLEActionKind;

/* GattCommunicationStatus, plus a call that failed or timed out */
typedef enum SDL_BLEStatus
{
    SDL_BLE_STATUS_SUCCESS,
    SDL_BLE_STATUS_UNREACHABLE,
    SDL_BLE_STATUS_PROTOCOL_ERROR,
    SDL_BLE_STATUS_ACCESS_DENIED,
    SDL_BLE_STATUS_FAILED
} SDL_BLEStatus;

typedef struct SDL_BLEAction
{
    uint8_t kind; /* SDL_BLEActionKind */
    uint8_t characteristic;
    uint8_t cccd;     /* SDL_BLE_CCCD_* */
    bool response;    /* Write with response */
    bool closing;     /* Part of an orderly close, sent even while the connection stops */
    uint8_t length;
    uint8_t data[SDL_BLE_MAX_WRITE];
} SDL_BLEAction;

typedef enum SDL_BLEPhase
{
    SDL_BLE_PHASE_CONNECTING,
    SDL_BLE_PHASE_PAIRING,      /* Pairing before discovery */
    SDL_BLE_PHASE_DISCOVERING,
    SDL_BLE_PHASE_SUBSCRIBING,
    SDL_BLE_PHASE_REPAIRING,    /* Pairing or removing a bond after a failed subscription */
    SDL_BLE_PHASE_READING,
    SDL_BLE_PHASE_STARTING,
    SDL_BLE_PHASE_RUNNING,
    SDL_BLE_PHASE_CLOSING,
    SDL_BLE_PHASE_ENDED
} SDL_BLEPhase;

typedef struct SDL_BLESession
{
    const SDL_BLEFamily *family;
    void *state; /* The module's, family->module->state_size bytes */
    bool pairing_allowed;

    uint8_t phase; /* SDL_BLEPhase */
    bool waiting;  /* An action with an answer is out */
    uint8_t answer; /* Its SDL_BLEActionKind. Only its answer is taken. */
    bool bonded;
    bool paired;       /* This session paired */
    bool removed_bond; /* This session removed a bond */
    bool pair_after_removal; /* The bond removal is followed by a pairing */
    bool repeated;     /* The subscriptions run a second time */
    bool resubscribing; /* Writing every subscription again while running */
    bool resubscribe_pending;
    bool published;
    bool backoff;      /* The session ended with a backoff */
    bool pairing_failed; /* The session ended on a failed pairing, or on a loss while one was out */
    bool connect_failed; /* The session ended without opening the device, or lost the link before the publish
                            outside SDL_BLE_PHASE_REPAIRING */
    uint64_t start_deadline; /* The start's time plus SDL_BLE_START_TIMEOUT_MS */
    bool found[SDL_BLE_MAX_CHARS];
    uint8_t properties[SDL_BLE_MAX_CHARS];
    int step;           /* The characteristic the subscriptions or reads are at */
    bool none_written;  /* For a bond, None goes before the wanted value */
    uint8_t out_cccd;   /* The value of the subscription out */

    SDL_BLEAction actions[SDL_BLE_MAX_SESSION_ACTIONS];
    int action_head;
    int action_count;
    uint32_t actions_dropped; /* Actions lost to a full queue. It stays 0, see SDL_BLESession_NextAction. */
} SDL_BLESession;

/* Starts a session and queues its connect. state is module->state_size
   bytes, which the module resets. A family with more keys, services or
   characteristics than SDL_BLE_MAX_KEYS, SDL_BLE_MAX_SERVICES or
   SDL_BLE_MAX_CHARS ends the session at once with only a disconnect. */
extern void SDL_BLESession_Init(SDL_BLESession *session, const SDL_BLEFamily *family, void *state,
                                const SDL_BLESink *sink, const SDL_BLEModuleContext *context,
                                bool pairing_allowed);
/* Takes the next action, oldest first. False when none waits. The driver
   can make several calls into the session before it takes the actions, and
   the session keeps these rules however many:
   - One action that awaits an answer is queued or out at a time. The next
     follows its answer.
   - At most five actions wait: that one, and a publish, a remove, a backoff
     and a disconnect, each of which a session queues at most once. A full
     queue would drop an action and count it in actions_dropped.
   - The disconnect is the last action a session queues. An action queued
     before the end still comes out ahead of it, and its answer is dropped. */
extern bool SDL_BLESession_NextAction(SDL_BLESession *session, SDL_BLEAction *action);
/* How the session ended, for SDL_BLEHost_SessionEnded */
extern void SDL_BLESession_GetOutcome(const SDL_BLESession *session, SDL_BLEOutcome *outcome);

/* The answers. Each is taken only while its own action is out. Any other is
   ignored, and one that comes after the end is dropped. */
extern void SDL_BLESession_Connected(SDL_BLESession *session, bool success, bool bonded, uint64_t now);
extern void SDL_BLESession_Paired(SDL_BLESession *session, bool success, uint64_t now);
extern void SDL_BLESession_BondRemoved(SDL_BLESession *session, bool success, uint64_t now);
/* found and properties hold one entry per family characteristic */
extern void SDL_BLESession_Discovered(SDL_BLESession *session, bool success, const bool *found,
                                      const uint8_t *properties, uint64_t now);
extern void SDL_BLESession_Subscribed(SDL_BLESession *session, SDL_BLEStatus status, uint8_t protocol_error, uint64_t now);
extern void SDL_BLESession_Read(SDL_BLESession *session, bool success, const uint8_t *data, size_t length, uint64_t time_ns);
extern void SDL_BLESession_Written(SDL_BLESession *session, bool success, uint64_t now);

/* A value taken from the connection's queue, passed to the module as is.
   gap releases every held control first. */
extern void SDL_BLESession_Value(SDL_BLESession *session, int characteristic, const uint8_t *data, size_t length,
                                 bool gap, uint64_t time_ns);
/* The link dropped. Before the publish, in any phase but
   SDL_BLE_PHASE_REPAIRING, the outcome reports connect_failed. A loss while
   a pairing is out also reports pairing_failed. */
extern void SDL_BLESession_Lost(SDL_BLESession *session, uint64_t now);
/* End the session on purpose, sending the module's closing writes first */
extern void SDL_BLESession_Close(SDL_BLESession *session, uint64_t now);
extern void SDL_BLESession_Tick(SDL_BLESession *session, uint64_t now);
extern bool SDL_BLESession_GetDeadline(SDL_BLESession *session, uint64_t *deadline);
extern bool SDL_BLESession_Ended(const SDL_BLESession *session);

extern const char *SDL_BLE_ActionName(int kind);

#endif /* SDL_ble_session_h_ */
