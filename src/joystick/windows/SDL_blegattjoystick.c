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

/*
 * PadForge fork: controllers that send their input as notifications on a
 * vendor GATT service over Bluetooth LE, found by their advertisements:
 * Nintendo's Poke Ball Plus, the Google Daydream, Samsung Gear VR and Oculus
 * Go controllers, the iOS Guitar Hero Live guitar, the Zwift Play and Zwift
 * Click, and the Thalmic Myo. See hifihedgehog/SDL#33 Part 12.
 *
 * The pure code in src/joystick/ble decides. The host picks the
 * advertisements to connect to, the session orders every connect, pairing,
 * discovery, subscription, read, write, publish and removal, and one
 * protocol module per family decodes. This driver executes. A matching
 * advertisement gets a connection with two threads. The session thread owns
 * the session and the module, feeds them the values in order and runs their
 * timers, since SDL calls Update only for an opened joystick and the
 * start-up runs before the joystick exists. The executor thread owns the
 * link and runs the blocking calls of the WinRT transport in SDL_ble_gatt.c
 * one at a time, so a GATT await never delays decoding. The value callback
 * only queues. As in the RFCOMM driver, the session thread hands presence
 * and events to the joystick thread under the connection's mutex and never
 * takes the joystick lock.
 */

#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_BLE

#include "../SDL_sysjoystick.h"
#include "../usb_ids.h"
#include "../../core/windows/SDL_windows.h"
#include "../ble/SDL_ble_session.h"
#include "../ble/SDL_ble_devices.h"
#include "../ble/SDL_ble_daydream_proto.h"
#include "../ble/SDL_ble_zwift_proto.h"
#include "../ble/SDL_ble_zwift_crypto.h"
#include "SDL_ble_gatt.h"

/* Changes of the controls and sensor samples wait for Update in queues of
   their own, so no run of samples can push a change out. Each queue drops
   its oldest entry when full. A change holds every control, so the newest
   one always stays, as in the serial and RFCOMM drivers
   (SDL_serial_engine.c:97-106, SDL_rfcommjoystick.c:376-385). The Gear VR
   sends 68 packets a second, each with three samples of each sensor
   (gearvr-controller PROTOCOL.md:108). 256 changes hold 3.8 s of a change in
   every packet, and 512 samples hold 1.25 s of its 408 samples a second. */
#define BLEGATT_CHANGE_CAPACITY  256
#define BLEGATT_SAMPLE_CAPACITY  512
#define BLEGATT_UPDATE_BATCH     32   /* Changes and samples Update takes under one lock */
#define BLEGATT_EXEC_CAPACITY    SDL_BLE_MAX_SESSION_ACTIONS /* Actions for the executor, and results back */
#define BLEGATT_LOST_POLL_MS     100  /* The transport reports link loss with no wake-up */
#define BLEGATT_WATCHER_CHECK_MS 2000 /* How often Detect has the transport check its watcher */

/* How long SDL_Quit lets the closing writes run before it abandons them. The
   families that close with writes, the Gear VR with one and the Myo with
   two, stream samples at 206 Hz and 50 Hz while their links are up. A
   closing write is one ATT write request, which a live link answers within
   a few connection events, tens of milliseconds at such rates. 1 s leaves
   ample room for every closing write of a live link. A link that has not
   answered by then is dying, and each write's own 3 s ceiling would
   otherwise hold SDL_Quit for up to 6 s. A hint change waits for the
   writes, since no app waits on it. */
#define BLEGATT_CLOSE_GRACE_MS 1000

/* How long SDL_Quit waits for the connections' threads in all, the grace
   included. Once the grace has passed, every call out has been told to
   stop, and a transport await returns within one 50 ms slice of its flag
   (SDL_BLEGATT_WaitCancelable in SDL_ble_gatt.c). The executor's close then
   pauses 100 ms before it closes the services, the pause bleak takes
   because GattDeviceService.Close sometimes hangs forever (bleak
   backends/winrt/client.py:488-491). A connection whose Close calls return
   ends about 150 ms after the grace. SDL_Quit waits 2 s more, then leaves
   the threads still running to end on their own, so its wait for them ends
   within 3 s. That stays under the 5 s after which Windows considers an
   application whose window reads no messages not responding (sdk-api
   nf-winuser-ishungappwindow.md:62-65), since an app can quit the joystick
   subsystem from its window's thread. */
#define BLEGATT_QUIT_WAIT_MS 3000

/* The pure layer carries SDL's values */
SDL_COMPILE_TIME_ASSERT(blegatt_type_unknown, SDL_BLE_TYPE_UNKNOWN == SDL_JOYSTICK_TYPE_UNKNOWN);
SDL_COMPILE_TIME_ASSERT(blegatt_type_gamepad, SDL_BLE_TYPE_GAMEPAD == SDL_JOYSTICK_TYPE_GAMEPAD);
SDL_COMPILE_TIME_ASSERT(blegatt_type_guitar, SDL_BLE_TYPE_GUITAR == SDL_JOYSTICK_TYPE_GUITAR);
SDL_COMPILE_TIME_ASSERT(blegatt_daydream_vendor, SDL_DAYDREAM_VENDOR == USB_VENDOR_GOOGLE);
SDL_COMPILE_TIME_ASSERT(blegatt_daydream_product, SDL_DAYDREAM_PRODUCT == USB_PRODUCT_GOOGLE_DAYDREAM_CONTROLLER);
SDL_COMPILE_TIME_ASSERT(blegatt_family_tables, SDL_BLE_FAMILY_COUNT == 7);

/* In the order of SDL_BLEFamilyID */
static const char *const blegatt_family_hints[SDL_BLE_FAMILY_COUNT] = {
    SDL_HINT_JOYSTICK_BLE_POKEBALL,
    SDL_HINT_JOYSTICK_BLE_DAYDREAM,
    SDL_HINT_JOYSTICK_BLE_GEARVR,
    SDL_HINT_JOYSTICK_BLE_OCULUSGO,
    SDL_HINT_JOYSTICK_BLE_GHLIVE,
    SDL_HINT_JOYSTICK_BLE_ZWIFT,
    SDL_HINT_JOYSTICK_BLE_MYO
};

/* Each family's code in its GUID key (BLEGATT_CreateGUID), in the order of
   SDL_BLEFamilyID. Every GUID the driver makes carries one, so a code never
   changes once shipped, and a new family takes a code no other has. */
static const char *const blegatt_guid_codes[SDL_BLE_FAMILY_COUNT] = {
    "PBP", /* Poke Ball Plus */
    "DAY", /* Daydream */
    "GVR", /* Gear VR */
    "OGO", /* Oculus Go */
    "GHL", /* Guitar Hero Live iOS */
    "ZWF", /* Zwift */
    "MYO"  /* Myo */
};

/* What the transport's callbacks reach. Once the transport has a pointer
   into it, it is never freed, since a value or status callback that is
   running when the link closes can finish afterward, as it can in the
   Switch 2 driver. The value callback reaches the connection only through
   it, so the connection itself can be freed. */
typedef struct BLEGATT_Inbox
{
    SDL_AtomicInt lost;      /* The transport sets it when the link drops */
    SDL_AtomicInt cancel;    /* Ends the transport's calls early, the closing writes aside */
    SDL_AtomicInt abandon;   /* Ends the closing writes early. Only Quit sets it. */
    SDL_AtomicInt callbacks; /* Value callbacks inside the connection */
    void *connection;        /* Atomic, NULL once the link has closed */
} BLEGATT_Inbox;

/* The executor's answer to one action */
typedef struct BLEGATT_Result
{
    Uint8 kind; /* The SDL_BLEActionKind answered */
    bool success;
    bool bonded;
    SDL_BLEStatus status;
    Uint8 protocol_error;
    bool found[SDL_BLE_MAX_CHARS];
    Uint8 properties[SDL_BLE_MAX_CHARS];
    Uint64 time_ns; /* When a read completed */
    size_t length;
    Uint8 data[SDL_BLE_MAX_VALUE];
} BLEGATT_Result;

/* One change of the controls, for Update */
typedef struct BLEGATT_Change
{
    Uint64 sequence;
    Uint64 time_ns; /* When the value behind it arrived */
    SDL_BLEControls controls;
} BLEGATT_Change;

/* One sensor sample, for Update */
typedef struct BLEGATT_Sample
{
    Uint64 sequence;
    Uint64 time_ns;   /* When the value behind it arrived */
    Uint64 sensor_ns; /* The device's sample time */
    Uint8 type;       /* SDL_BLE_SENSOR_* */
    float data[3];
} BLEGATT_Sample;

/* A change or a sample, as Update takes them */
typedef struct BLEGATT_Event
{
    bool sensor; /* A sample, else a change */
    BLEGATT_Change change;
    BLEGATT_Sample sample;
} BLEGATT_Event;

typedef struct BLEGATT_Connection
{
    Uint64 address;
    int family; /* SDL_BLEFamilyID */
    Uint8 variant;
    bool pairing_allowed;
    char path[18]; /* The address as XX:XX:XX:XX:XX:XX, the joystick's path */
    BLEGATT_Inbox *inbox;
    SDL_Mutex *mutex;
    HANDLE wake_event; /* Auto reset, wakes the session thread */
    HANDLE exec_event; /* Auto reset, wakes the executor */
    SDL_Thread *session_thread;
    SDL_Thread *executor_thread;
    SDL_AtomicInt stop;
    SDL_AtomicInt presence_changed;
    SDL_AtomicInt session_exited;
    SDL_AtomicInt executor_exited;
    SDL_AtomicInt quiet; /* Set when SDL_Quit abandons it: its threads log nothing more */

    /* The session thread's */
    SDL_BLESession session;
    void *state; /* The module's */
    SDL_ZwiftCrypto crypto;
    SDL_ZwiftKeys *keys;
    Uint32 next_generation;
    Uint32 values_taken;

    /* The executor's */
    SDL_BLEGATTLink *link;
    bool inbox_shared; /* The transport has pointers into the inbox */

    /* Under the mutex */
    SDL_BLEValueQueue values;
    SDL_BLEValue *value_entries;
    SDL_BLEAction exec_actions[BLEGATT_EXEC_CAPACITY];
    int exec_head;
    int exec_count;
    BLEGATT_Result results[BLEGATT_EXEC_CAPACITY];
    int result_head;
    int result_count;
    bool closed;       /* The executor has released the link */
    Uint32 generation; /* 0 while absent */
    SDL_BLEIdentity identity;
    SDL_BLEControls latest;
    Uint64 latest_ns; /* When the value behind latest arrived */
    Uint64 sequence;  /* The last change or sample queued */
    BLEGATT_Change *changes;
    int change_head;
    int change_count;
    Uint32 changes_dropped;
    BLEGATT_Sample *samples;
    int sample_head;
    int sample_count;
    Uint32 samples_dropped;
    bool sensors; /* The opened joystick has a sensor enabled */

    /* Under blegatt_lock. SDL_Quit gave up waiting for the threads. */
    bool abandoned;

    /* The joystick thread's */
    bool stopping;
    Uint32 registered; /* The generation it has a joystick for, 0 when none */
    SDL_JoystickID instance_id;
    SDL_GUID guid;
    SDL_BLEIdentity joystick_identity;

    struct BLEGATT_Connection *next;
} BLEGATT_Connection;

struct joystick_hwdata
{
    BLEGATT_Connection *connection; /* NULL once the joystick is removed */
    Uint64 sequence;                /* The last change or sample sent */
    Uint32 changes_dropped;         /* The connection's count when the newest controls last went out */
    bool touchpad;
    int battery;       /* The last percent sent, -1 before one */
    bool send_initial; /* The state Open took is not sent yet */
    Uint64 initial_stamp;
    SDL_BLEControls initial;
};

/* Created once and never destroyed, since a listener call that is running
   when the listener is removed can still take it */
static SDL_Mutex *blegatt_lock;

/* Under blegatt_lock */
static bool blegatt_accepting; /* Between Init and Quit */
static bool blegatt_enabled[SDL_BLE_FAMILY_COUNT];
static bool blegatt_pairing;
static SDL_BLEHost blegatt_host;
static BLEGATT_Connection *blegatt_new; /* From the listener, not yet taken by Detect */

/* The joystick thread's */
static bool blegatt_initialized;
static bool blegatt_transport; /* SDL_BLEGATT_Init has succeeded */
static bool blegatt_listening;
static Uint64 blegatt_watcher_checked; /* When Detect last had the watcher checked */
static SDL_AtomicInt blegatt_hints_changed;
static BLEGATT_Connection *blegatt_connections;

/* Counts for the log, never names: advertisements the listener got and
   connections it made while watching */
static SDL_AtomicInt blegatt_heard;
static SDL_AtomicInt blegatt_connects;

static const SDL_BLEFamily *BLEGATT_Family(const BLEGATT_Connection *connection)
{
    return SDL_BLE_Families[connection->family];
}

/* Under the mutex. A full queue drops its oldest change. */
static void BLEGATT_PushChange(BLEGATT_Connection *connection, const SDL_BLEControls *controls, Uint64 time_ns)
{
    BLEGATT_Change *change;

    if (connection->change_count == BLEGATT_CHANGE_CAPACITY) {
        connection->change_head = (connection->change_head + 1) % BLEGATT_CHANGE_CAPACITY;
        --connection->change_count;
        ++connection->changes_dropped;
    }
    change = &connection->changes[(connection->change_head + connection->change_count) % BLEGATT_CHANGE_CAPACITY];
    change->sequence = ++connection->sequence;
    change->time_ns = time_ns;
    change->controls = *controls;
    ++connection->change_count;
}

/* Under the mutex. A full ring drops its oldest sample. */
static void BLEGATT_PushSample(BLEGATT_Connection *connection, int type, Uint64 time_ns, Uint64 sensor_ns, const float *data)
{
    BLEGATT_Sample *sample;

    if (connection->sample_count == BLEGATT_SAMPLE_CAPACITY) {
        connection->sample_head = (connection->sample_head + 1) % BLEGATT_SAMPLE_CAPACITY;
        --connection->sample_count;
        ++connection->samples_dropped;
    }
    sample = &connection->samples[(connection->sample_head + connection->sample_count) % BLEGATT_SAMPLE_CAPACITY];
    sample->sequence = ++connection->sequence;
    sample->time_ns = time_ns;
    sample->sensor_ns = sensor_ns;
    sample->type = (Uint8)type;
    sample->data[0] = data[0];
    sample->data[1] = data[1];
    sample->data[2] = data[2];
    ++connection->sample_count;
}

/* Under the mutex. The first change or the first sample, whichever came
   first, so both leave in the order they were queued. */
static bool BLEGATT_PopEvent(BLEGATT_Connection *connection, BLEGATT_Event *event)
{
    const BLEGATT_Change *change = connection->change_count ? &connection->changes[connection->change_head] : NULL;
    const BLEGATT_Sample *sample = connection->sample_count ? &connection->samples[connection->sample_head] : NULL;

    if (change && (!sample || change->sequence < sample->sequence)) {
        event->sensor = false;
        event->change = *change;
        connection->change_head = (connection->change_head + 1) % BLEGATT_CHANGE_CAPACITY;
        --connection->change_count;
        return true;
    }
    if (sample) {
        event->sensor = true;
        event->sample = *sample;
        connection->sample_head = (connection->sample_head + 1) % BLEGATT_SAMPLE_CAPACITY;
        --connection->sample_count;
        return true;
    }
    return false;
}

/* The module's changes, in order, on the session thread */
static void BLEGATT_Changed(void *userdata, const SDL_BLEControls *controls, uint64_t time_ns)
{
    BLEGATT_Connection *connection = (BLEGATT_Connection *)userdata;

    SDL_LockMutex(connection->mutex);
    BLEGATT_PushChange(connection, controls, time_ns);
    connection->latest = *controls;
    connection->latest_ns = time_ns;
    SDL_UnlockMutex(connection->mutex);
}

/* The module's sensor samples, in order with its changes. None is queued
   while the opened joystick has no sensor enabled, as the PS4 driver sends
   samples only while its report_sensors is set (SDL_hidapi_ps4.c:1179). */
static void BLEGATT_Sensor(void *userdata, int sensor, uint64_t time_ns, uint64_t sensor_ns, const float *data)
{
    BLEGATT_Connection *connection = (BLEGATT_Connection *)userdata;

    SDL_LockMutex(connection->mutex);
    if (connection->sensors) {
        BLEGATT_PushSample(connection, sensor, time_ns, sensor_ns, data);
    }
    SDL_UnlockMutex(connection->mutex);
}

/* A log line from a connection's own threads. A connection that SDL_Quit
   abandoned logs nothing, since SDL_Quit goes on to shut the log down while
   its threads can still run. */
#define BLEGATT_CONNECTION_LOG(connection, ...) do { if (!SDL_GetAtomicInt(&(connection)->quiet)) { SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, __VA_ARGS__); } } while (0)

static void BLEGATT_Log(void *userdata, const char *text)
{
    BLEGATT_Connection *connection = (BLEGATT_Connection *)userdata;

    BLEGATT_CONNECTION_LOG(connection, "BLE GATT %s (%s): %s", connection->path, BLEGATT_Family(connection)->name, text);
}

/* A characteristic value, on a WinRT thread-pool thread. It only queues the
   value and wakes the session thread. Its count in callbacks keeps the
   connection alive until it returns. */
static void BLEGATT_ValueReceived(void *userdata, int characteristic, const Uint8 *data, size_t length)
{
    BLEGATT_Inbox *inbox = (BLEGATT_Inbox *)userdata;
    BLEGATT_Connection *connection;

    SDL_AddAtomicInt(&inbox->callbacks, 1);
    connection = (BLEGATT_Connection *)SDL_GetAtomicPointer(&inbox->connection);
    if (connection) {
        const Uint64 now = SDL_GetTicksNS();

        SDL_LockMutex(connection->mutex);
        (void)SDL_BLE_PushValue(&connection->values, characteristic, data, length, now);
        SDL_UnlockMutex(connection->mutex);
        SetEvent(connection->wake_event);
    }
    SDL_AddAtomicInt(&inbox->callbacks, -1);
}

/* After this returns no value callback is inside the connection, and a
   later one finds none */
static void BLEGATT_DetachInbox(BLEGATT_Inbox *inbox)
{
    SDL_SetAtomicPointer(&inbox->connection, NULL);
    while (SDL_GetAtomicInt(&inbox->callbacks) != 0) {
        SDL_Delay(1);
    }
}

/* The session thread hands an action to the executor. The last slot is kept
   for the disconnect, so the action that releases the link always fits. */
static void BLEGATT_PostAction(BLEGATT_Connection *connection, const SDL_BLEAction *action)
{
    const int limit = (action->kind == SDL_BLE_ACTION_DISCONNECT) ? BLEGATT_EXEC_CAPACITY : (BLEGATT_EXEC_CAPACITY - 1);
    bool queued = false;

    SDL_LockMutex(connection->mutex);
    if (connection->exec_count < limit) {
        connection->exec_actions[(connection->exec_head + connection->exec_count) % BLEGATT_EXEC_CAPACITY] = *action;
        ++connection->exec_count;
        queued = true;
    }
    SDL_UnlockMutex(connection->mutex);
    /* Never dropped: the session has one answered action out at most, and
       one disconnect comes last */
    SDL_assert(queued);
    SetEvent(connection->exec_event);
}

static bool BLEGATT_TakeAction(BLEGATT_Connection *connection, SDL_BLEAction *action)
{
    bool taken = false;

    SDL_LockMutex(connection->mutex);
    if (connection->exec_count > 0) {
        *action = connection->exec_actions[connection->exec_head];
        connection->exec_head = (connection->exec_head + 1) % BLEGATT_EXEC_CAPACITY;
        --connection->exec_count;
        taken = true;
    }
    SDL_UnlockMutex(connection->mutex);
    return taken;
}

static void BLEGATT_PostResult(BLEGATT_Connection *connection, const BLEGATT_Result *result)
{
    bool queued = false;

    SDL_LockMutex(connection->mutex);
    if (connection->result_count < BLEGATT_EXEC_CAPACITY) {
        connection->results[(connection->result_head + connection->result_count) % BLEGATT_EXEC_CAPACITY] = *result;
        ++connection->result_count;
        queued = true;
    }
    SDL_UnlockMutex(connection->mutex);
    /* Never dropped: the session takes each result before it posts its next
       answered action, so one result waits at most */
    SDL_assert(queued);
    SetEvent(connection->wake_event);
}

static bool BLEGATT_TakeResult(BLEGATT_Connection *connection, BLEGATT_Result *result)
{
    bool taken = false;

    SDL_LockMutex(connection->mutex);
    if (connection->result_count > 0) {
        *result = connection->results[connection->result_head];
        connection->result_head = (connection->result_head + 1) % BLEGATT_EXEC_CAPACITY;
        --connection->result_count;
        taken = true;
    }
    SDL_UnlockMutex(connection->mutex);
    return taken;
}

/* Runs one action with the transport's blocking calls and posts its answer */
static void BLEGATT_Execute(BLEGATT_Connection *connection, const SDL_BLEAction *action)
{
    const SDL_BLEFamily *family = BLEGATT_Family(connection);
    BLEGATT_Inbox *inbox = connection->inbox;
    SDL_BLEGATTLink *link;
    BLEGATT_Result result;

    SDL_zero(result);
    result.kind = action->kind;
    /* A connection stopped before its connect keeps the inbox to itself */
    if (action->kind == SDL_BLE_ACTION_CONNECT && !connection->link && !SDL_GetAtomicInt(&inbox->cancel)) {
        connection->inbox_shared = true;
        connection->link = SDL_BLEGATT_Open(connection->address, &inbox->lost, &inbox->cancel, &result.bonded);
    }
    link = connection->link;
    switch (action->kind) {
    case SDL_BLE_ACTION_CONNECT:
        result.success = (link != NULL);
        break;
    case SDL_BLE_ACTION_PAIR:
        result.success = link && SDL_BLEGATT_Pair(link, &inbox->cancel);
        break;
    case SDL_BLE_ACTION_REMOVE_BOND:
        result.success = link && SDL_BLEGATT_RemoveBond(link, &inbox->cancel);
        break;
    case SDL_BLE_ACTION_DISCOVER:
        result.success = link && SDL_BLEGATT_Discover(link, family, result.found, result.properties, &inbox->cancel);
        break;
    case SDL_BLE_ACTION_SUBSCRIBE:
        result.status = SDL_BLE_STATUS_FAILED;
        if (link) {
            result.status = SDL_BLEGATT_Subscribe(link, action->characteristic, action->cccd, &result.protocol_error,
                                                  BLEGATT_ValueReceived, inbox, &inbox->cancel);
        }
        result.success = (result.status == SDL_BLE_STATUS_SUCCESS);
        break;
    case SDL_BLE_ACTION_READ:
        result.length = sizeof(result.data);
        result.success = link && SDL_BLEGATT_Read(link, action->characteristic, result.data, &result.length, &inbox->cancel);
        if (!result.success) {
            result.length = 0;
        }
        result.time_ns = SDL_GetTicksNS();
        break;
    case SDL_BLE_ACTION_WRITE:
        /* A closing write goes out even while the connection stops. Only
           Quit ends it early, once BLEGATT_CLOSE_GRACE_MS has passed. */
        result.success = link && SDL_BLEGATT_Write(link, action->characteristic, action->data, action->length,
                                                   action->response, action->closing ? &inbox->abandon : &inbox->cancel);
        break;
    default:
        return;
    }
    if (!result.success) {
        if (action->kind == SDL_BLE_ACTION_SUBSCRIBE) {
            BLEGATT_CONNECTION_LOG(connection, "BLE GATT %s (%s): subscribe %d failed with status %d, error 0x%02X",
                                   connection->path, family->name, (int)action->characteristic, (int)result.status,
                                   (unsigned int)result.protocol_error);
        } else {
            BLEGATT_CONNECTION_LOG(connection, "BLE GATT %s (%s): %s failed",
                                   connection->path, family->name, SDL_BLE_ActionName(action->kind));
        }
    }
    BLEGATT_PostResult(connection, &result);
}

/* Only this thread touches the link. It ends with the session's disconnect. */
static int SDLCALL BLEGATT_ExecutorThread(void *data)
{
    BLEGATT_Connection *connection = (BLEGATT_Connection *)data;
    /* This thread joins the MTA through the transport, as the Switch 2
       driver's connect thread does (SDL_ble_switch2joystick.c:258), since it
       blocks on the transport's awaits */
    const bool apartment = SDL_BLEGATT_InitThread();
    SDL_BLEAction action;

    for (;;) {
        if (!BLEGATT_TakeAction(connection, &action)) {
            WaitForSingleObject(connection->exec_event, INFINITE);
            continue;
        }
        if (action.kind == SDL_BLE_ACTION_DISCONNECT) {
            break;
        }
        BLEGATT_Execute(connection, &action);
    }
    if (connection->link) {
        SDL_BLEGATT_Close(connection->link);
        connection->link = NULL;
    }
    BLEGATT_DetachInbox(connection->inbox);
    if (apartment) {
        SDL_BLEGATT_QuitThread();
    }
    SDL_LockMutex(connection->mutex);
    connection->closed = true;
    SDL_UnlockMutex(connection->mutex);
    SetEvent(connection->wake_event);
    SDL_SetAtomicInt(&connection->executor_exited, 1);
    return 0;
}

static void BLEGATT_Publish(BLEGATT_Connection *connection)
{
    const SDL_BLEBase *base = (const SDL_BLEBase *)connection->state;

    if (++connection->next_generation == 0) {
        connection->next_generation = 1;
    }
    SDL_LockMutex(connection->mutex);
    connection->generation = connection->next_generation;
    connection->identity = base->identity;
    SDL_UnlockMutex(connection->mutex);
    SDL_SetAtomicInt(&connection->presence_changed, 1);
    BLEGATT_CONNECTION_LOG(connection, "BLE GATT %s (%s): publish as \"%s\"",
                           connection->path, BLEGATT_Family(connection)->name, base->identity.name);
}

static void BLEGATT_Unpublish(BLEGATT_Connection *connection)
{
    SDL_LockMutex(connection->mutex);
    connection->generation = 0;
    SDL_UnlockMutex(connection->mutex);
    SDL_SetAtomicInt(&connection->presence_changed, 1);
}

/* Hands out the session's actions. Presence stays on this thread, and the
   rest goes to the executor in order. */
static void BLEGATT_RunActions(BLEGATT_Connection *connection)
{
    const SDL_BLEFamily *family = BLEGATT_Family(connection);
    SDL_BLEAction action;

    SDL_assert(connection->session.actions_dropped == 0);
    while (SDL_BLESession_NextAction(&connection->session, &action)) {
        if (action.kind != SDL_BLE_ACTION_WRITE && action.kind != SDL_BLE_ACTION_PUBLISH) {
            BLEGATT_CONNECTION_LOG(connection, "BLE GATT %s (%s): %s",
                                   connection->path, family->name, SDL_BLE_ActionName(action.kind));
        }
        switch (action.kind) {
        case SDL_BLE_ACTION_PUBLISH:
            BLEGATT_Publish(connection);
            break;
        case SDL_BLE_ACTION_REMOVE:
            BLEGATT_Unpublish(connection);
            break;
        case SDL_BLE_ACTION_BACKOFF:
            /* The host learns it when the session thread ends */
            break;
        default:
            BLEGATT_PostAction(connection, &action);
            break;
        }
    }
}

/* Answers the session from the executor's results, in order */
/* A lost link, then a stop, as the session thread takes them. It runs again
   before every result, so a failure the stop's cancel caused reaches a
   session that is already closing and counts for nothing. */
static void BLEGATT_TakeEnds(BLEGATT_Connection *connection, bool *stopped)
{
    SDL_BLESession *session = &connection->session;

    /* The loss goes first, so a stop that comes with it queues no closing
       write to a link that is gone */
    if (SDL_GetAtomicInt(&connection->inbox->lost)) {
        SDL_BLESession_Lost(session, SDL_GetTicks());
    }
    if (!*stopped && SDL_GetAtomicInt(&connection->stop)) {
        *stopped = true;
        SDL_BLESession_Close(session, SDL_GetTicks());
    }
}

static void BLEGATT_TakeResults(BLEGATT_Connection *connection, bool *stopped)
{
    SDL_BLESession *session = &connection->session;
    BLEGATT_Result result;

    while (BLEGATT_TakeResult(connection, &result)) {
        Uint64 now;

        BLEGATT_TakeEnds(connection, stopped);
        now = SDL_GetTicks();
        switch (result.kind) {
        case SDL_BLE_ACTION_CONNECT:
            SDL_BLESession_Connected(session, result.success, result.bonded, now);
            break;
        case SDL_BLE_ACTION_PAIR:
            SDL_BLESession_Paired(session, result.success, now);
            break;
        case SDL_BLE_ACTION_REMOVE_BOND:
            SDL_BLESession_BondRemoved(session, result.success, now);
            break;
        case SDL_BLE_ACTION_DISCOVER:
            SDL_BLESession_Discovered(session, result.success, result.found, result.properties, now);
            break;
        case SDL_BLE_ACTION_SUBSCRIBE:
            SDL_BLESession_Subscribed(session, result.status, result.protocol_error, now);
            break;
        case SDL_BLE_ACTION_READ:
            SDL_BLESession_Read(session, result.success, result.data, result.length, result.time_ns);
            break;
        case SDL_BLE_ACTION_WRITE:
            SDL_BLESession_Written(session, result.success, now);
            break;
        default:
            break;
        }
    }
}

/* Feeds the queued values to the session in order, with their gap flag and
   receive time. It takes those queued when it starts, since a value that
   comes later sets the wake event again. */
static void BLEGATT_TakeValues(BLEGATT_Connection *connection)
{
    SDL_BLEValue value;
    int count;

    SDL_LockMutex(connection->mutex);
    count = connection->values.count;
    SDL_UnlockMutex(connection->mutex);
    while (count-- > 0) {
        bool taken;

        SDL_LockMutex(connection->mutex);
        taken = SDL_BLE_PopValue(&connection->values, &value);
        SDL_UnlockMutex(connection->mutex);
        if (!taken) {
            break;
        }
        ++connection->values_taken;
        SDL_BLESession_Value(&connection->session, value.characteristic, value.data, value.length, value.gap, value.time_ns);
    }
}

static bool BLEGATT_IsClosed(BLEGATT_Connection *connection)
{
    bool closed;

    SDL_LockMutex(connection->mutex);
    closed = connection->closed;
    SDL_UnlockMutex(connection->mutex);
    return closed;
}

/* Owns the session and the module. It ends once the session has ended and
   the executor has released the link, and then gives the address back to
   the host, unless SDL_Quit abandoned the connection. */
static int SDLCALL BLEGATT_SessionThread(void *data)
{
    BLEGATT_Connection *connection = (BLEGATT_Connection *)data;
    const SDL_BLEFamily *family = BLEGATT_Family(connection);
    SDL_BLESession *session = &connection->session;
    SDL_BLEModuleContext context;
    SDL_BLEOutcome outcome;
    SDL_BLESink sink;
    bool stopped = false;
    bool abandoned;
    Uint32 full, too_long, empty, changes_dropped, samples_dropped;

    /* The key exchange of older Zwift firmware, when CNG provides it */
    if (connection->family == SDL_BLE_FAMILY_ZWIFT) {
        connection->keys = SDL_ZwiftCrypto_Create(&connection->crypto);
    }
    SDL_zero(context);
    context.variant = connection->variant;
    context.crypto = connection->keys ? &connection->crypto : NULL;
    sink.userdata = connection;
    sink.changed = BLEGATT_Changed;
    sink.sensor = BLEGATT_Sensor;
    sink.log = BLEGATT_Log;
    SDL_BLESession_Init(session, family, connection->state, &sink, &context, connection->pairing_allowed);
    SDL_LockMutex(connection->mutex);
    connection->latest = ((const SDL_BLEBase *)connection->state)->controls;
    SDL_UnlockMutex(connection->mutex);

    for (;;) {
        DWORD timeout = BLEGATT_LOST_POLL_MS;
        Uint64 deadline;

        BLEGATT_TakeEnds(connection, &stopped);
        BLEGATT_TakeResults(connection, &stopped);
        BLEGATT_TakeValues(connection);
        if (SDL_BLESession_GetDeadline(session, &deadline) && SDL_GetTicks() >= deadline) {
            SDL_BLESession_Tick(session, SDL_GetTicks());
        }
        BLEGATT_RunActions(connection);
        if (SDL_BLESession_Ended(session)) {
            /* No answer counts now, so the call out ends early */
            SDL_SetAtomicInt(&connection->inbox->cancel, 1);
            if (BLEGATT_IsClosed(connection)) {
                break;
            }
            WaitForSingleObject(connection->wake_event, INFINITE);
            continue;
        }
        if (SDL_BLESession_GetDeadline(session, &deadline)) {
            const Uint64 now = SDL_GetTicks();
            /* At least 1 ms, so a deadline that did not move cannot spin */
            const Uint64 wait = (deadline > now) ? (deadline - now) : 1;

            if (wait < timeout) {
                timeout = (DWORD)wait;
            }
        }
        WaitForSingleObject(connection->wake_event, timeout);
    }

    /* An abandoned connection leaves the host alone. SDL_Quit has cleared
       the host, and after the next Init the host can hold a new session for
       this address. */
    SDL_BLESession_GetOutcome(session, &outcome);
    SDL_LockMutex(blegatt_lock);
    abandoned = connection->abandoned;
    if (!abandoned) {
        SDL_BLEHost_SessionEnded(&blegatt_host, connection->address, &outcome, SDL_GetTicks());
    }
    SDL_UnlockMutex(blegatt_lock);
    if (connection->keys) {
        SDL_ZwiftCrypto_Destroy(connection->keys);
        connection->keys = NULL;
    }

    /* Nothing is logged after SDL_Quit has given up on the thread */
    if (!abandoned) {
        SDL_LockMutex(connection->mutex);
        full = connection->values.full;
        too_long = connection->values.too_long;
        empty = connection->values.empty;
        changes_dropped = connection->changes_dropped;
        samples_dropped = connection->samples_dropped;
        SDL_UnlockMutex(connection->mutex);
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                     "BLE GATT %s (%s): ended, %u values, %u lost to a full queue, %u too long, %u empty, "
                     "%u changes and %u samples dropped unread",
                     connection->path, family->name, (unsigned int)connection->values_taken, (unsigned int)full,
                     (unsigned int)too_long, (unsigned int)empty, (unsigned int)changes_dropped,
                     (unsigned int)samples_dropped);
    }
    SDL_SetAtomicInt(&connection->session_exited, 1);
    return 0;
}

static void BLEGATT_FreeConnection(BLEGATT_Connection *connection)
{
    if (connection->inbox) {
        BLEGATT_DetachInbox(connection->inbox);
        /* Kept once the transport has pointers into it, see BLEGATT_Inbox */
        if (!connection->inbox_shared) {
            SDL_free(connection->inbox);
        }
    }
    if (connection->exec_event) {
        CloseHandle(connection->exec_event);
    }
    if (connection->wake_event) {
        CloseHandle(connection->wake_event);
    }
    if (connection->mutex) {
        SDL_DestroyMutex(connection->mutex);
    }
    SDL_free(connection->changes);
    SDL_free(connection->samples);
    SDL_free(connection->value_entries);
    SDL_free(connection->state);
    SDL_free(connection);
}

/* A connection and its two threads, under blegatt_lock. It writes no log
   line of its own, since a log callback would run under the lock, and the
   session thread logs the connect. */
static BLEGATT_Connection *BLEGATT_CreateConnection(Uint64 address, const SDL_BLEMatch *match, bool pairing)
{
    const SDL_BLEFamily *family;
    BLEGATT_Connection *connection;

    if (match->family < 0 || match->family >= SDL_BLE_FAMILY_COUNT) {
        return NULL;
    }
    family = SDL_BLE_Families[match->family];
    connection = (BLEGATT_Connection *)SDL_calloc(1, sizeof(*connection));
    if (!connection) {
        return NULL;
    }
    connection->address = address;
    connection->family = match->family;
    connection->variant = match->variant;
    connection->pairing_allowed = pairing;
    (void)SDL_snprintf(connection->path, sizeof(connection->path), "%02X:%02X:%02X:%02X:%02X:%02X",
                       (unsigned int)((address >> 40) & 0xFF), (unsigned int)((address >> 32) & 0xFF),
                       (unsigned int)((address >> 24) & 0xFF), (unsigned int)((address >> 16) & 0xFF),
                       (unsigned int)((address >> 8) & 0xFF), (unsigned int)(address & 0xFF));
    connection->inbox = (BLEGATT_Inbox *)SDL_calloc(1, sizeof(*connection->inbox));
    connection->state = SDL_calloc(1, family->module->state_size);
    connection->value_entries = (SDL_BLEValue *)SDL_calloc(SDL_BLE_QUEUE_CAPACITY, sizeof(*connection->value_entries));
    connection->changes = (BLEGATT_Change *)SDL_calloc(BLEGATT_CHANGE_CAPACITY, sizeof(*connection->changes));
    connection->samples = (BLEGATT_Sample *)SDL_calloc(BLEGATT_SAMPLE_CAPACITY, sizeof(*connection->samples));
    connection->mutex = SDL_CreateMutex();
    connection->wake_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    connection->exec_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!connection->inbox || !connection->state || !connection->value_entries || !connection->changes ||
        !connection->samples || !connection->mutex || !connection->wake_event || !connection->exec_event) {
        BLEGATT_FreeConnection(connection);
        return NULL;
    }
    SDL_BLE_InitQueue(&connection->values, connection->value_entries, SDL_BLE_QUEUE_CAPACITY, SDL_BLE_MAX_VALUE);
    SDL_SetAtomicPointer(&connection->inbox->connection, connection);

    connection->executor_thread = SDL_CreateThread(BLEGATT_ExecutorThread, "SDLBLEGATTExecutor", connection);
    if (!connection->executor_thread) {
        BLEGATT_FreeConnection(connection);
        return NULL;
    }
    connection->session_thread = SDL_CreateThread(BLEGATT_SessionThread, "SDLBLEGATTSession", connection);
    if (!connection->session_thread) {
        /* The idle executor ends at once with a disconnect */
        SDL_BLEAction action;

        SDL_zero(action);
        action.kind = SDL_BLE_ACTION_DISCONNECT;
        BLEGATT_PostAction(connection, &action);
        SDL_WaitThread(connection->executor_thread, NULL);
        BLEGATT_FreeConnection(connection);
        return NULL;
    }
    return connection;
}

/* An advertisement, which the transport has parsed, on a WinRT thread-pool
   thread. It never takes the joystick lock and returns at once. It can run
   after the listener is removed (SDL_ble_gatt.h), and under blegatt_lock
   such a late call makes no connection. After Quit, blegatt_accepting is
   false. After ApplyHints removed the listener, every family is off in
   blegatt_enabled, so the host matches nothing. */
static void BLEGATT_Listener(const SDL_BLEAdvertisement *advertisement, void *userdata)
{
    SDL_BLEMatch match;

    (void)userdata;
    SDL_AddAtomicInt(&blegatt_heard, 1);
    SDL_LockMutex(blegatt_lock);
    if (blegatt_accepting) {
        const Uint64 now = SDL_GetTicks();

        if (SDL_BLEHost_Advertise(&blegatt_host, SDL_BLE_Families, SDL_BLE_FAMILY_COUNT, blegatt_enabled,
                                  advertisement, now, &match)) {
            BLEGATT_Connection *connection = BLEGATT_CreateConnection(advertisement->address, &match, blegatt_pairing);

            if (connection) {
                BLEGATT_Connection **tail;

                for (tail = &blegatt_new; *tail; tail = &(*tail)->next) {
                }
                *tail = connection;
                SDL_AddAtomicInt(&blegatt_connects, 1);
            } else {
                /* No memory, mutex, event or thread for the connection. The
                   address waits as after a failed connect, since a try at
                   every advertisement would ask for them again. */
                SDL_BLEOutcome outcome;

                SDL_zero(outcome);
                outcome.connect_failed = true;
                SDL_BLEHost_SessionEnded(&blegatt_host, advertisement->address, &outcome, now);
            }
        }
    }
    SDL_UnlockMutex(blegatt_lock);
}

static void BLEGATT_RemoveJoystick(BLEGATT_Connection *connection, bool notify)
{
    if (connection->registered) {
        SDL_Joystick *joystick = SDL_GetJoystickFromID(connection->instance_id);

        if (joystick && joystick->hwdata) {
            joystick->hwdata->connection = NULL;
        }
        if (notify) {
            SDL_PrivateJoystickRemoved(connection->instance_id);
        }
        connection->registered = 0;
    }
}

/* Removes the connection's joystick and asks its session to close. The
   record is freed once both threads have ended, so the joystick thread
   never waits on a connect. */
static void BLEGATT_StopConnection(BLEGATT_Connection *connection, bool notify)
{
    BLEGATT_RemoveJoystick(connection, notify);
    connection->stopping = true;
    /* The stop goes first, so the failure of a call the cancel ends is
       posted after the stop can be seen, and BLEGATT_TakeResults closes the
       session before it passes that failure on. The call out ends early.
       The closing writes watch the abandon flag instead, which only Quit
       sets. */
    SDL_SetAtomicInt(&connection->stop, 1);
    SDL_SetAtomicInt(&connection->inbox->cancel, 1);
    SetEvent(connection->wake_event);
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE GATT %s (%s): stopping", connection->path, BLEGATT_Family(connection)->name);
}

static void BLEGATT_UnlinkConnection(BLEGATT_Connection *connection)
{
    BLEGATT_Connection **link;

    for (link = &blegatt_connections; *link; link = &(*link)->next) {
        if (*link == connection) {
            *link = connection->next;
            break;
        }
    }
}

static bool BLEGATT_ConnectionEnded(BLEGATT_Connection *connection)
{
    return SDL_GetAtomicInt(&connection->session_exited) && SDL_GetAtomicInt(&connection->executor_exited);
}

/* Waits until the threads of every connection have ended, or timeout_ms has
   passed since start */
static void BLEGATT_WaitConnections(Uint64 start, Uint64 timeout_ms)
{
    for (;;) {
        BLEGATT_Connection *connection;
        bool running = false;

        for (connection = blegatt_connections; connection; connection = connection->next) {
            if (!BLEGATT_ConnectionEnded(connection)) {
                running = true;
                break;
            }
        }
        if (!running || SDL_GetTicks() - start >= timeout_ms) {
            return;
        }
        SDL_Delay(1);
    }
}

/* A connection whose threads outlive SDL_Quit's wait, as when
   GattDeviceService.Close hangs (bleak backends/winrt/client.py:488-491).
   Its threads are detached and end on their own, as the Switch 2 driver
   detaches its connect threads and has one that finishes after Quit check
   a flag under a lock before it touches the driver's state
   (SDL_ble_switch2joystick.c:717-719, :911-918). Here the flag is
   abandoned. The record and inbox are never freed, since those threads and
   the transport's callbacks still use them. */
static void BLEGATT_AbandonConnection(BLEGATT_Connection *connection)
{
    SDL_SetAtomicInt(&connection->quiet, 1);
    SDL_LockMutex(blegatt_lock);
    connection->abandoned = true;
    SDL_UnlockMutex(blegatt_lock);
    SDL_DetachThread(connection->session_thread);
    SDL_DetachThread(connection->executor_thread);
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE GATT %s (%s): still closing after %d ms, left to end on its own",
                 connection->path, BLEGATT_Family(connection)->name, BLEGATT_QUIT_WAIT_MS);
}

/* Frees the connections whose threads have ended. At Quit it takes every
   connection, and one whose threads still run is abandoned. */
static void BLEGATT_ReapConnections(bool quitting)
{
    BLEGATT_Connection *connection, *next;

    for (connection = blegatt_connections; connection; connection = next) {
        const bool ended = BLEGATT_ConnectionEnded(connection);

        next = connection->next;
        if (!ended && !quitting) {
            continue;
        }
        BLEGATT_RemoveJoystick(connection, !quitting);
        BLEGATT_UnlinkConnection(connection);
        if (ended) {
            SDL_WaitThread(connection->session_thread, NULL);
            SDL_WaitThread(connection->executor_thread, NULL);
            BLEGATT_FreeConnection(connection);
        } else {
            BLEGATT_AbandonConnection(connection);
        }
    }
}

/* Takes the listener's new connections, in order */
static void BLEGATT_TakeNewConnections(void)
{
    BLEGATT_Connection *added, **tail;

    SDL_LockMutex(blegatt_lock);
    added = blegatt_new;
    blegatt_new = NULL;
    SDL_UnlockMutex(blegatt_lock);
    if (!added) {
        return;
    }
    for (tail = &blegatt_connections; *tail; tail = &(*tail)->next) {
    }
    *tail = added;
}

/* Each family's hint defaults to SDL_HINT_JOYSTICK_BLE, as the HIDAPI
   drivers' hints default to SDL_HINT_JOYSTICK_HIDAPI. True when any family
   is on. */
static bool BLEGATT_GetEnabled(bool *enabled)
{
    const bool all = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_BLE, false);
    bool any = false;
    int family;

    for (family = 0; family < SDL_BLE_FAMILY_COUNT; ++family) {
        enabled[family] = SDL_GetHintBoolean(blegatt_family_hints[family], all);
        any = any || enabled[family];
    }
    return any;
}

/* Removes the listener and logs what it heard, as counts */
static void BLEGATT_StopListening(void)
{
    SDL_BLEGATT_RemoveListener(BLEGATT_Listener, NULL);
    blegatt_listening = false;
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE GATT: stopped watching advertisements after %d advertisements, %d connections",
                 SDL_GetAtomicInt(&blegatt_heard), SDL_GetAtomicInt(&blegatt_connects));
}

static void BLEGATT_ApplyHints(void)
{
    bool enabled[SDL_BLE_FAMILY_COUNT];
    const bool any = BLEGATT_GetEnabled(enabled);
    const bool pairing = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_BLE_PAIRING, false);
    BLEGATT_Connection *connection;

    SDL_LockMutex(blegatt_lock);
    SDL_memcpy(blegatt_enabled, enabled, sizeof(blegatt_enabled));
    if (pairing != blegatt_pairing) {
        /* A wait earned under the old pairing setting says nothing under the
           new one. An unbonded Daydream that backed off with pairing off can
           pair now, so it is tried at its next advertisement. */
        SDL_BLEHost_ClearBackoff(&blegatt_host);
    }
    blegatt_pairing = pairing;
    SDL_UnlockMutex(blegatt_lock);

    /* A connection the listener made under the old hints is in the new list
       by now, so it is stopped too */
    BLEGATT_TakeNewConnections();
    for (connection = blegatt_connections; connection; connection = connection->next) {
        if (!connection->stopping && !enabled[connection->family]) {
            BLEGATT_StopConnection(connection, true);
        }
    }

    /* The advertisement listener is added only while a family is on. The
       transport's runtime runs from Init to Quit, but no scan runs without a
       listener. */
    if (any && !blegatt_listening) {
        if (!blegatt_transport) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE GATT: the WinRT Bluetooth LE transport is not available");
            return;
        }
        SDL_SetAtomicInt(&blegatt_heard, 0);
        SDL_SetAtomicInt(&blegatt_connects, 0);
        blegatt_listening = SDL_BLEGATT_AddListener(BLEGATT_Listener, NULL);
        if (blegatt_listening) {
            blegatt_watcher_checked = SDL_GetTicks();
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE GATT: watching advertisements");
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE GATT: the advertisement watcher could not start");
        }
    } else if (!any && blegatt_listening) {
        BLEGATT_StopListening();
    }
}

/* The joystick's GUID. With no vendor, SDL_CreateJoystickGUID keeps only the
   first 9 bytes of the name it is given (SDL_joystick.c:3247-3255). SDL
   stores an automatic mapping without the CRC (SDL_gamepad.c:2568-2571), and
   its lookup's second pass ignores the CRC (SDL_gamepad.c:1580-1584). By
   name alone, "Zwift Play (L)" and "Zwift Play (R)" share those 9 bytes, so
   the half connected second took the first half's mapping and name. The key
   is the family's code, then the variant as two hex digits, then the name,
   so its first 9 bytes differ for every family and variant and stay the
   same from version to version. The joystick's name stays identity->name.
   'l' is this driver's signature, and the type rides in the last byte. */
static SDL_GUID BLEGATT_CreateGUID(int family, Uint8 variant, const SDL_BLEIdentity *identity)
{
    char key[8 + SDL_BLE_NAME_LENGTH];

    (void)SDL_snprintf(key, sizeof(key), "%s%02X %s", blegatt_guid_codes[family], (unsigned int)variant, identity->name);
    return SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, identity->vendor, identity->product, 0, NULL, key, 'l',
                                  identity->type);
}

static void BLEGATT_CheckPresence(BLEGATT_Connection *connection)
{
    SDL_BLEIdentity identity;
    Uint32 generation;

    SDL_LockMutex(connection->mutex);
    generation = connection->generation;
    identity = connection->identity;
    SDL_UnlockMutex(connection->mutex);

    if (connection->registered && connection->registered != generation) {
        BLEGATT_RemoveJoystick(connection, true);
    }
    if (!connection->registered && generation) {
        connection->registered = generation;
        connection->joystick_identity = identity;
        connection->instance_id = SDL_GetNextObjectID();
        connection->guid = BLEGATT_CreateGUID(connection->family, connection->variant, &identity);
        SDL_PrivateJoystickAdded(connection->instance_id);
    }
}

static BLEGATT_Connection *BLEGATT_GetConnection(int device_index)
{
    BLEGATT_Connection *connection;

    if (device_index < 0) {
        return NULL;
    }
    for (connection = blegatt_connections; connection; connection = connection->next) {
        if (connection->registered && device_index-- == 0) {
            return connection;
        }
    }
    return NULL;
}

/* The number of the highest set bit, counting from 1, 0 when none is set */
static int BLEGATT_HighestBit(Uint32 bits)
{
    int count = 0;

    while (bits) {
        ++count;
        bits >>= 1;
    }
    return count;
}

static void SDLCALL BLEGATT_HintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    (void)userdata;
    (void)name;
    (void)oldValue;
    (void)hint;
    SDL_SetAtomicInt(&blegatt_hints_changed, 1);
}

/* The hints are watched from the start, so an app can turn a family on at
   any time. The advertisement watcher starts only when a family is on, so
   no BLE scan runs unless the app opts in. The transport's runtime starts
   here and ends in Quit, so its RoInitialize and RoUninitialize run on the
   thread that initializes and quits the joystick subsystem, as the WGI
   driver's do (SDL_windows_gaming_input.c:595-598 and :1007-1009). */
static bool BLEGATT_JoystickInit(void)
{
    int family;

    if (blegatt_initialized) {
        return true;
    }
    if (!blegatt_lock) {
        blegatt_lock = SDL_CreateMutex();
        if (!blegatt_lock) {
            return false;
        }
    }
    SDL_LockMutex(blegatt_lock);
    SDL_BLEHost_Init(&blegatt_host);
    SDL_zeroa(blegatt_enabled);
    blegatt_pairing = false;
    blegatt_new = NULL;
    blegatt_accepting = true;
    SDL_UnlockMutex(blegatt_lock);
    /* Without WinRT the driver stays loaded and says so when a family turns on */
    blegatt_transport = SDL_BLEGATT_Init();
    blegatt_initialized = true;
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_BLE, BLEGATT_HintChanged, NULL);
    for (family = 0; family < SDL_BLE_FAMILY_COUNT; ++family) {
        SDL_AddHintCallback(blegatt_family_hints[family], BLEGATT_HintChanged, NULL);
    }
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_BLE_PAIRING, BLEGATT_HintChanged, NULL);
    /* The callbacks run at once, so Detect applies the hints first */
    SDL_SetAtomicInt(&blegatt_hints_changed, 1);
    return true;
}

static int BLEGATT_JoystickGetCount(void)
{
    BLEGATT_Connection *connection;
    int count = 0;

    for (connection = blegatt_connections; connection; connection = connection->next) {
        if (connection->registered) {
            ++count;
        }
    }
    return count;
}

static void BLEGATT_JoystickDetect(void)
{
    BLEGATT_Connection *connection;

    if (!blegatt_initialized) {
        return;
    }
    if (SDL_GetAtomicInt(&blegatt_hints_changed)) {
        SDL_SetAtomicInt(&blegatt_hints_changed, 0);
        BLEGATT_ApplyHints();
    }
    /* A watcher that aborted, as when the radio went off, runs again */
    if (blegatt_listening && SDL_GetTicks() - blegatt_watcher_checked >= BLEGATT_WATCHER_CHECK_MS) {
        blegatt_watcher_checked = SDL_GetTicks();
        (void)SDL_BLEGATT_CheckWatcher();
    }
    BLEGATT_TakeNewConnections();
    for (connection = blegatt_connections; connection; connection = connection->next) {
        if (!connection->stopping && SDL_GetAtomicInt(&connection->presence_changed)) {
            SDL_SetAtomicInt(&connection->presence_changed, 0);
            BLEGATT_CheckPresence(connection);
        }
    }
    BLEGATT_ReapConnections(false);
}

/* A vendor GATT service is no other driver's device */
static bool BLEGATT_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    (void)vendor_id;
    (void)product_id;
    (void)version;
    (void)name;
    return false;
}

static const char *BLEGATT_JoystickGetDeviceName(int device_index)
{
    BLEGATT_Connection *connection = BLEGATT_GetConnection(device_index);

    return connection ? connection->joystick_identity.name : NULL;
}

static const char *BLEGATT_JoystickGetDevicePath(int device_index)
{
    BLEGATT_Connection *connection = BLEGATT_GetConnection(device_index);

    return connection ? connection->path : NULL;
}

static int BLEGATT_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    (void)device_index;
    return -1;
}

static int BLEGATT_JoystickGetDevicePlayerIndex(int device_index)
{
    (void)device_index;
    return -1;
}

static void BLEGATT_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
    (void)device_index;
    (void)player_index;
}

static SDL_GUID BLEGATT_JoystickGetDeviceGUID(int device_index)
{
    BLEGATT_Connection *connection = BLEGATT_GetConnection(device_index);
    SDL_GUID guid;

    if (!connection) {
        SDL_zero(guid);
        return guid;
    }
    return connection->guid;
}

static SDL_JoystickID BLEGATT_JoystickGetDeviceInstanceID(int device_index)
{
    BLEGATT_Connection *connection = BLEGATT_GetConnection(device_index);

    return connection ? connection->instance_id : 0;
}

static void BLEGATT_SendControls(SDL_Joystick *joystick, struct joystick_hwdata *hwdata, const SDL_BLEControls *controls,
                                 Uint64 timestamp)
{
    int i;

    for (i = 0; i < joystick->naxes; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, controls->axes[i]);
    }
    for (i = 0; i < joystick->nbuttons; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, SDL_BLE_GetButton(controls, i));
    }
    if (hwdata->touchpad) {
        SDL_SendJoystickTouchpad(timestamp, joystick, 0, 0, controls->finger, controls->finger_x, controls->finger_y,
                                 controls->finger ? 1.0f : 0.0f);
    }
    if (controls->battery >= 0 && controls->battery != hwdata->battery) {
        hwdata->battery = controls->battery;
        SDL_SendJoystickPowerInfo(joystick, SDL_POWERSTATE_ON_BATTERY, controls->battery);
    }
}

static void BLEGATT_SendSensor(SDL_Joystick *joystick, const BLEGATT_Sample *sample)
{
    SDL_SensorType type;

    switch (sample->type) {
    case SDL_BLE_SENSOR_ACCEL:
        type = SDL_SENSOR_ACCEL;
        break;
    case SDL_BLE_SENSOR_GYRO:
        type = SDL_SENSOR_GYRO;
        break;
    default:
        return;
    }
    SDL_SendJoystickSensor(sample->time_ns, joystick, type, sample->sensor_ns, sample->data, 3);
}

static bool BLEGATT_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    BLEGATT_Connection *connection = BLEGATT_GetConnection(device_index);
    const SDL_BLEIdentity *identity;
    struct joystick_hwdata *hwdata;
    char serial[13];

    if (!connection) {
        return SDL_SetError("BLE GATT device index out of range");
    }
    hwdata = (struct joystick_hwdata *)SDL_calloc(1, sizeof(*hwdata));
    if (!hwdata) {
        return false;
    }
    identity = &connection->joystick_identity;
    hwdata->connection = connection;
    hwdata->touchpad = identity->touchpad;
    hwdata->battery = -1;
    joystick->hwdata = hwdata;
    joystick->naxes = SDL_min(BLEGATT_HighestBit(identity->axes), SDL_BLE_MAX_AXES);
    joystick->nbuttons = BLEGATT_HighestBit(identity->buttons);
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
    if (identity->accel_rate > 0.0f) {
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, identity->accel_rate);
    }
    if (identity->gyro_rate > 0.0f) {
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, identity->gyro_rate);
    }
    if (identity->touchpad) {
        SDL_PrivateJoystickAddTouchpad(joystick, 1);
    }

    /* The address as the serial, twelve lowercase hex digits, as the Switch
       2 driver sets it, so a device with a fixed address keeps its identity
       across reconnects */
    (void)SDL_snprintf(serial, sizeof(serial), "%012llx", (unsigned long long)connection->address);
    joystick->serial = SDL_strdup(serial);

    /* The state so far, sent by the first update, as SDL allocates the
       joystick's axes and buttons only after Open returns. Every change and
       sample queued up to now is skipped. A new joystick's sensors start
       off, as the PS4 driver's do (SDL_hidapi_ps4.c:874). */
    SDL_LockMutex(connection->mutex);
    hwdata->initial = connection->latest;
    hwdata->sequence = connection->sequence;
    hwdata->changes_dropped = connection->changes_dropped;
    connection->sensors = false;
    SDL_UnlockMutex(connection->mutex);
    hwdata->initial_stamp = SDL_GetTicksNS();
    hwdata->send_initial = true;
    return true;
}

static bool BLEGATT_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    (void)joystick;
    (void)low_frequency_rumble;
    (void)high_frequency_rumble;
    return SDL_Unsupported();
}

static bool BLEGATT_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    (void)joystick;
    (void)left_rumble;
    (void)right_rumble;
    return SDL_Unsupported();
}

static bool BLEGATT_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)joystick;
    (void)red;
    (void)green;
    (void)blue;
    return SDL_Unsupported();
}

static bool BLEGATT_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    (void)joystick;
    (void)data;
    (void)size;
    return SDL_Unsupported();
}

/* SDL calls this when the joystick's first sensor turns on and when its last
   turns off (SDL_joystick.c:1998-2014). Samples are queued only in between,
   as the PS4 driver keeps report_sensors (SDL_hidapi_ps4.c:1031-1047). */
static bool BLEGATT_JoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    struct joystick_hwdata *hwdata = joystick->hwdata;

    if (hwdata && hwdata->connection) {
        SDL_LockMutex(hwdata->connection->mutex);
        hwdata->connection->sensors = enabled;
        SDL_UnlockMutex(hwdata->connection->mutex);
    }
    return true;
}

/* Drains the connection's changes and samples in the order they were
   queued */
static void BLEGATT_JoystickUpdate(SDL_Joystick *joystick)
{
    struct joystick_hwdata *hwdata = joystick->hwdata;
    BLEGATT_Event events[BLEGATT_UPDATE_BATCH];
    BLEGATT_Connection *connection;
    SDL_BLEControls latest;
    Uint64 latest_ns = 0;
    bool resend = false;
    int rounds, count, i;

    if (!hwdata || !hwdata->connection) {
        return;
    }
    connection = hwdata->connection;
    if (hwdata->send_initial) {
        SDL_BLEControls rest;

        hwdata->send_initial = false;
        /* Every axis rests where the module contract puts it: 0, and -32768
           for a gamepad trigger (SDL_BLE_RestControls). SDL's core presets
           rest values only for its HIDAPI, XInput, RawInput and WGI GUIDs
           (SDL_joystick.c:1484-1510). Without them SDL_SendJoystickAxis takes
           a trigger's first reading near the middle for its rest
           (SDL_joystick.c:2801-2806). Seeding the rest values keeps them, and
           SDL then passes every later value on, as it does for the fork's
           data axes (SDL_hidapi_ps3.c:1204-1206, SDL_xinputjoystick.c:426-427,
           SDL_ble_switch2joystick.c:1117-1120). */
        SDL_BLE_RestControls(&connection->joystick_identity, &rest);
        for (i = 0; i < joystick->naxes; ++i) {
            SDL_SeedJoystickDataAxis(joystick, (Uint8)i, rest.axes[i]);
        }
        BLEGATT_SendControls(joystick, hwdata, &hwdata->initial, hwdata->initial_stamp);
    }
    /* A batch per lock, at most both queues' worth per update */
    for (rounds = 0; rounds < (BLEGATT_CHANGE_CAPACITY + BLEGATT_SAMPLE_CAPACITY) / BLEGATT_UPDATE_BATCH; ++rounds) {
        count = 0;
        SDL_LockMutex(connection->mutex);
        while (count < BLEGATT_UPDATE_BATCH && BLEGATT_PopEvent(connection, &events[count])) {
            ++count;
        }
        SDL_UnlockMutex(connection->mutex);
        for (i = 0; i < count; ++i) {
            const BLEGATT_Event *event = &events[i];
            const Uint64 sequence = event->sensor ? event->sample.sequence : event->change.sequence;

            if (sequence <= hwdata->sequence) {
                continue;
            }
            hwdata->sequence = sequence;
            if (event->sensor) {
                BLEGATT_SendSensor(joystick, &event->sample);
            } else {
                BLEGATT_SendControls(joystick, hwdata, &event->change.controls, event->change.time_ns);
            }
        }
        if (count < BLEGATT_UPDATE_BATCH) {
            break;
        }
    }

    /* After a full queue dropped a change, the newest controls go out once
       every queued change has, so the joystick ends on them whatever was
       dropped. SDL passes on only what differs from its state
       (SDL_joystick.c:2807-2808, :2957-2959 and :4141-4146). */
    SDL_zero(latest);
    SDL_LockMutex(connection->mutex);
    if (connection->changes_dropped != hwdata->changes_dropped && connection->change_count == 0) {
        hwdata->changes_dropped = connection->changes_dropped;
        latest = connection->latest;
        latest_ns = connection->latest_ns;
        resend = true;
    }
    SDL_UnlockMutex(connection->mutex);
    if (resend) {
        BLEGATT_SendControls(joystick, hwdata, &latest, latest_ns);
    }
}

/* A closed joystick queues no sample, as the PS4 driver clears
   report_sensors when its joystick closes (SDL_hidapi_ps4.c:1426) */
static void BLEGATT_JoystickClose(SDL_Joystick *joystick)
{
    struct joystick_hwdata *hwdata = joystick->hwdata;

    if (hwdata && hwdata->connection) {
        SDL_LockMutex(hwdata->connection->mutex);
        hwdata->connection->sensors = false;
        SDL_UnlockMutex(hwdata->connection->mutex);
    }
    SDL_free(joystick->hwdata);
    joystick->hwdata = NULL;
}

static void BLEGATT_JoystickQuit(void)
{
    BLEGATT_Connection *connection;
    Uint64 start;
    int family;

    if (!blegatt_initialized) {
        return;
    }
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_BLE, BLEGATT_HintChanged, NULL);
    for (family = 0; family < SDL_BLE_FAMILY_COUNT; ++family) {
        SDL_RemoveHintCallback(blegatt_family_hints[family], BLEGATT_HintChanged, NULL);
    }
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_BLE_PAIRING, BLEGATT_HintChanged, NULL);

    /* A listener call from here on makes no connection */
    SDL_LockMutex(blegatt_lock);
    blegatt_accepting = false;
    SDL_UnlockMutex(blegatt_lock);
    if (blegatt_listening) {
        BLEGATT_StopListening();
    }
    BLEGATT_TakeNewConnections();

    /* Every session is told first, so the closes overlap. Each one sends its
       closing writes, and then its executor releases the link. Writes still
       out after BLEGATT_CLOSE_GRACE_MS are abandoned, which fails every
       closing write left, so SDL_Quit waits no longer on a dying link. A
       connection still closing after BLEGATT_QUIT_WAIT_MS in all is left to
       end on its own. */
    for (connection = blegatt_connections; connection; connection = connection->next) {
        if (!connection->stopping) {
            BLEGATT_StopConnection(connection, false);
        }
    }
    start = SDL_GetTicks();
    BLEGATT_WaitConnections(start, BLEGATT_CLOSE_GRACE_MS);
    for (connection = blegatt_connections; connection; connection = connection->next) {
        SDL_SetAtomicInt(&connection->inbox->abandon, 1);
    }
    BLEGATT_WaitConnections(start, BLEGATT_QUIT_WAIT_MS);
    BLEGATT_ReapConnections(true);
    /* After every link, as the transport's runtime has to outlive them. The
       executor of a connection left to end on its own joined the MTA itself
       (SDL_BLEGATT_InitThread), which the transport keeps pinned with
       CoIncrementMTAUsage, so it can finish its close afterward. */
    if (blegatt_transport) {
        SDL_BLEGATT_Quit();
        blegatt_transport = false;
    }

    SDL_LockMutex(blegatt_lock);
    SDL_BLEHost_Init(&blegatt_host);
    SDL_UnlockMutex(blegatt_lock);
    SDL_SetAtomicInt(&blegatt_hints_changed, 0);
    blegatt_initialized = false;
}

static void BLEGATT_MapButton(SDL_InputMapping *mapping, const SDL_BLEIdentity *identity, int button)
{
    if (identity->buttons & ((Uint32)1 << button)) {
        mapping->kind = EMappingKind_Button;
        mapping->target = (Uint8)button;
    }
}

static void BLEGATT_MapAxis(SDL_InputMapping *mapping, const SDL_BLEIdentity *identity, int axis)
{
    if (identity->axes & (1 << axis)) {
        mapping->kind = EMappingKind_Axis;
        mapping->target = (Uint8)axis;
    }
}

/* The identity mapping the GameInput driver fills, for the buttons and axes
   the device has. The D-pad is buttons instead of a hat, and each trigger is
   a full axis that rests at -32768. The Myo is a plain joystick. */
static bool BLEGATT_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    BLEGATT_Connection *connection = BLEGATT_GetConnection(device_index);
    const SDL_BLEIdentity *identity;

    if (!connection || !connection->joystick_identity.gamepad) {
        return false;
    }
    identity = &connection->joystick_identity;
    SDL_zerop(out);
    BLEGATT_MapButton(&out->a, identity, SDL_BLE_BUTTON_SOUTH);
    BLEGATT_MapButton(&out->b, identity, SDL_BLE_BUTTON_EAST);
    BLEGATT_MapButton(&out->x, identity, SDL_BLE_BUTTON_WEST);
    BLEGATT_MapButton(&out->y, identity, SDL_BLE_BUTTON_NORTH);
    BLEGATT_MapButton(&out->back, identity, SDL_BLE_BUTTON_BACK);
    BLEGATT_MapButton(&out->guide, identity, SDL_BLE_BUTTON_GUIDE);
    BLEGATT_MapButton(&out->start, identity, SDL_BLE_BUTTON_START);
    BLEGATT_MapButton(&out->leftstick, identity, SDL_BLE_BUTTON_LEFT_STICK);
    BLEGATT_MapButton(&out->rightstick, identity, SDL_BLE_BUTTON_RIGHT_STICK);
    BLEGATT_MapButton(&out->leftshoulder, identity, SDL_BLE_BUTTON_LEFT_SHOULDER);
    BLEGATT_MapButton(&out->rightshoulder, identity, SDL_BLE_BUTTON_RIGHT_SHOULDER);
    BLEGATT_MapButton(&out->dpup, identity, SDL_BLE_BUTTON_DPAD_UP);
    BLEGATT_MapButton(&out->dpdown, identity, SDL_BLE_BUTTON_DPAD_DOWN);
    BLEGATT_MapButton(&out->dpleft, identity, SDL_BLE_BUTTON_DPAD_LEFT);
    BLEGATT_MapButton(&out->dpright, identity, SDL_BLE_BUTTON_DPAD_RIGHT);
    BLEGATT_MapAxis(&out->leftx, identity, SDL_BLE_AXIS_LEFTX);
    BLEGATT_MapAxis(&out->lefty, identity, SDL_BLE_AXIS_LEFTY);
    BLEGATT_MapAxis(&out->rightx, identity, SDL_BLE_AXIS_RIGHTX);
    BLEGATT_MapAxis(&out->righty, identity, SDL_BLE_AXIS_RIGHTY);
    BLEGATT_MapAxis(&out->lefttrigger, identity, SDL_BLE_AXIS_LEFT_TRIGGER);
    BLEGATT_MapAxis(&out->righttrigger, identity, SDL_BLE_AXIS_RIGHT_TRIGGER);
    return true;
}

SDL_JoystickDriver SDL_BLEGATT_JoystickDriver = {
    BLEGATT_JoystickInit,
    BLEGATT_JoystickGetCount,
    BLEGATT_JoystickDetect,
    BLEGATT_JoystickIsDevicePresent,
    BLEGATT_JoystickGetDeviceName,
    BLEGATT_JoystickGetDevicePath,
    BLEGATT_JoystickGetDeviceSteamVirtualGamepadSlot,
    BLEGATT_JoystickGetDevicePlayerIndex,
    BLEGATT_JoystickSetDevicePlayerIndex,
    BLEGATT_JoystickGetDeviceGUID,
    BLEGATT_JoystickGetDeviceInstanceID,
    BLEGATT_JoystickOpen,
    BLEGATT_JoystickRumble,
    BLEGATT_JoystickRumbleTriggers,
    BLEGATT_JoystickSetLED,
    BLEGATT_JoystickSendEffect,
    BLEGATT_JoystickSetSensorsEnabled,
    BLEGATT_JoystickUpdate,
    BLEGATT_JoystickClose,
    BLEGATT_JoystickQuit,
    BLEGATT_JoystickGetGamepadMapping
};

#endif /* SDL_JOYSTICK_BLE */
