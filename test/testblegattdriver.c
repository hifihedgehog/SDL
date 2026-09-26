/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_BLEGATT_JoystickDriver, the Bluetooth LE GATT joystick driver of
   hifihedgehog/SDL#33 Part 12, inside a static SDL against a fake transport.
   No radio is used. The fake answers every transport call the driver makes,
   hands advertisements to the driver's listener and delivers values through
   the callbacks the driver subscribed. Each scenario starts SDL, reaches the
   device through SDL's joystick and gamepad API, and quits SDL.

   The driver in this file calls the fake, not SDL's transport. Every
   function of SDL_ble_gatt.h is renamed below before the driver source is
   included, so the driver here calls Fake_BLEGATT_* while SDL_ble_gatt.c in
   the static SDL keeps SDL_BLEGATT_* for the Switch 2 driver, and the names
   never clash. This object defines SDL_BLEGATT_JoystickDriver. The linker
   takes every object on its command line and searches a library only for a
   symbol still undefined, so the driver table in SDL_joystick.c binds to
   this copy and the library's copy of the driver is never linked. Were that
   copy pulled in for some other symbol, its SDL_BLEGATT_JoystickDriver would
   collide with this one and the link would fail with LNK2005, so a test that
   links runs this copy only. Before the link,
   test/ble-driver/CheckTransport.cmake reads this object's symbols and
   fails the build when anything in it still calls an SDL_BLEGATT_ function,
   a WinRT entry point or a function that resolves one at run time.

   Arguments: the gearvr-controller clone, whose tests_fixtures.json holds
   the Gear VR packets, then optionally the letters of the scenarios to run. */

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"

/* Every transport function, renamed to the fake below */
#define SDL_BLEGATT_Init           Fake_BLEGATT_Init
#define SDL_BLEGATT_Quit           Fake_BLEGATT_Quit
#define SDL_BLEGATT_InitThread     Fake_BLEGATT_InitThread
#define SDL_BLEGATT_QuitThread     Fake_BLEGATT_QuitThread
#define SDL_BLEGATT_AddListener    Fake_BLEGATT_AddListener
#define SDL_BLEGATT_RemoveListener Fake_BLEGATT_RemoveListener
#define SDL_BLEGATT_CheckWatcher   Fake_BLEGATT_CheckWatcher
#define SDL_BLEGATT_Open           Fake_BLEGATT_Open
#define SDL_BLEGATT_Pair           Fake_BLEGATT_Pair
#define SDL_BLEGATT_RemoveBond     Fake_BLEGATT_RemoveBond
#define SDL_BLEGATT_Discover       Fake_BLEGATT_Discover
#define SDL_BLEGATT_Subscribe      Fake_BLEGATT_Subscribe
#define SDL_BLEGATT_Read           Fake_BLEGATT_Read
#define SDL_BLEGATT_Write          Fake_BLEGATT_Write
#define SDL_BLEGATT_Close          Fake_BLEGATT_Close

/* Always the tree's driver, by its path from this file */
#include "../src/joystick/windows/SDL_blegattjoystick.c"

#include "core/windows/SDL_windows.h"
#include "joystick/SDL_joystick_c.h"
#include "joystick/SDL_sysjoystick.h"
#include "joystick/windows/SDL_ble_gatt.h"
#include "joystick/ble/SDL_ble_devices.h"
#include "joystick/ble/SDL_ble_pokeball_proto.h"
#include "joystick/ble/SDL_ble_daydream_proto.h"
#include "joystick/ble/SDL_ble_gearvr_proto.h"
#include "joystick/ble/SDL_ble_oculusgo_proto.h"
#include "joystick/ble/SDL_ble_ghlios_proto.h"
#include "joystick/ble/SDL_ble_zwift_proto.h"
#include "joystick/ble/SDL_ble_myo_proto.h"

#include <process.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define DT_WAIT_MS           3000  /* Longest wait for anything the driver does */
#define DT_QUIET_MS          300   /* How long nothing must happen */
#define DT_QUIT_LIMIT_MS     5000  /* SDL_Quit in any scenario */
#define DT_CANCEL_LIMIT_MS   2000  /* SDL_Quit while an Open or a closing write blocks, whose own ceilings are 20 s and 3 s */
#define DT_SCENARIO_LIMIT_MS 45000 /* The watchdog ends a scenario that hangs */
#define DT_BATTERY           64    /* The Battery Level a read returns unless a scenario sets another */
#define DT_TICK_SLACK_MS     10    /* SDL_GetTicks counts whole milliseconds */

#define DT_HINT_BLE     "SDL_JOYSTICK_BLE"
#define DT_HINT_PAIRING "SDL_JOYSTICK_BLE_PAIRING"

#define DT_COUNT(array) ((int)SDL_arraysize(array))

/* Each family's hint, in the order of SDL_BLE_Families */
static const char *const dt_family_hints[SDL_BLE_FAMILY_COUNT] = {
    "SDL_JOYSTICK_BLE_POKEBALL",
    "SDL_JOYSTICK_BLE_DAYDREAM",
    "SDL_JOYSTICK_BLE_GEARVR",
    "SDL_JOYSTICK_BLE_OCULUSGO",
    "SDL_JOYSTICK_BLE_GHLIVE",
    "SDL_JOYSTICK_BLE_ZWIFT",
    "SDL_JOYSTICK_BLE_MYO"
};

/* Every other joystick driver off, so only the driver under test adds a
   joystick, whatever the environment holds */
static const char *const dt_other_drivers[][2] = {
    { "SDL_JOYSTICK_HIDAPI", "0" },
    { "SDL_JOYSTICK_RAWINPUT", "0" },
    { "SDL_JOYSTICK_DIRECTINPUT", "0" },
    { "SDL_XINPUT_ENABLED", "0" },
    { "SDL_JOYSTICK_WGI", "0" },
    { "SDL_JOYSTICK_GAMEINPUT", "0" },
    { "SDL_JOYSTICK_GAMEINPUT_RAW", "0" },
    { "SDL_JOYSTICK_BLE_SWITCH2", "0" },
    { "SDL_JOYSTICK_SERIAL", "" },
    { "SDL_JOYSTICK_SERIAL_AUTO", "0" },
    { "SDL_JOYSTICK_DJI_REMOTE_TCP_HOSTS", "" },
    { "SDL_JOYSTICK_RFCOMM", "0" },
    { "SDL_JOYSTICK_ICADE", "0" },
    { "SDL_JOYSTICK_WINMM", "0" },
    { "SDL_JOYSTICK_THREAD", "0" },
    { "SDL_JOYSTICK_ROG_CHAKRAM", "0" },
    { SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1" }
};

/* Checks */

static int dt_checks;
static int dt_failures;
static const char *volatile dt_scenario = "-";

static bool DT_Check(bool condition, int line, const char *format, ...)
{
    va_list args;

    ++dt_checks;
    if (condition) {
        return true;
    }
    ++dt_failures;
    printf("FAIL (%s) line %d: ", dt_scenario, line);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    return false;
}

#define DT_CHECK(condition, ...) DT_Check((condition) ? true : false, __LINE__, __VA_ARGS__)

static int DT_HexDigit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Parses hex such as "5B EB FF" or "5bebff", skipping other characters.
   Returns the byte count. */
static size_t DT_Hex(const char *text, Uint8 *out, size_t max)
{
    size_t count = 0;
    int high = -1;

    for (; *text; ++text) {
        const int value = DT_HexDigit(*text);

        if (value < 0) {
            continue;
        }
        if (high < 0) {
            high = value;
            continue;
        }
        if (count < max) {
            out[count] = (Uint8)((high << 4) | value);
        }
        ++count;
        high = -1;
    }
    return count;
}

/* A UUID in the order it is written, as SDL_BLEUUID holds it */
static SDL_BLEUUID DT_UUID(const char *text)
{
    SDL_BLEUUID uuid;

    SDL_zero(uuid);
    (void)DT_Hex(text, uuid.bytes, sizeof(uuid.bytes));
    return uuid;
}

static void DT_HexText(const Uint8 *data, size_t length, char *out, size_t size)
{
    size_t i, used = 0;

    out[0] = '\0';
    for (i = 0; i < length && used + 4 < size; ++i) {
        used += (size_t)SDL_snprintf(&out[used], size - used, i ? " %02X" : "%02X", data[i]);
    }
}

/* The fake transport */

#define FAKE_MAX_LINKS        32
#define FAKE_MAX_CALLS        2048
#define FAKE_OPEN_CEILING_MS  20000 /* SDL_BLEGATT_Open's own ceiling */
#define FAKE_PAIR_CEILING_MS  20000 /* SDL_BLEGATT_Pair's own ceiling */
#define FAKE_WRITE_CEILING_MS 3000  /* SDL_BLEGATT_Write's own ceiling */
#define FAKE_CLOSE_CEILING_MS 15000 /* A hanging Close ends by itself after this, so no run of the test hangs */
#define FAKE_HOLD_CEILING_MS  15000 /* The same for a held write or subscription */

typedef enum FakeKind
{
    FAKE_INIT,
    FAKE_QUIT,
    FAKE_ADD_LISTENER,
    FAKE_REMOVE_LISTENER,
    FAKE_OPEN,
    FAKE_PAIR,
    FAKE_REMOVE_BOND,
    FAKE_DISCOVER,
    FAKE_SUBSCRIBE,
    FAKE_READ,
    FAKE_WRITE,
    FAKE_CLOSE,
    FAKE_CHECK_WATCHER
} FakeKind;

static const char *const fake_kind_names[] = {
    "init", "quit", "add listener", "remove listener", "open", "pair", "remove bond",
    "discover", "subscribe", "read", "write", "close", "check watcher"
};

typedef struct FakeCall
{
    int kind;           /* FakeKind */
    int link;           /* The link's index, -1 for none */
    Uint64 address;     /* Open */
    int characteristic; /* Subscribe, read and write, else -1 */
    int cccd;           /* Subscribe, else -1 */
    int status;         /* Subscribe: the SDL_BLEStatus returned */
    bool response;      /* Write */
    size_t length;      /* Write */
    Uint8 data[SDL_BLE_MAX_WRITE];
    bool result;        /* Open: a link came back. Otherwise the call succeeded. */
    Uint64 time_ns;     /* SDL_GetTicksNS when the call was recorded */
} FakeCall;

struct SDL_BLEGATTLink
{
    int index;
    Uint64 address;
    SDL_AtomicInt *lost;
    const SDL_BLEFamily *family;
    int closes;
    SDL_BLEGATT_ValueCallback callbacks[SDL_BLE_MAX_CHARS];
    void *userdata[SDL_BLE_MAX_CHARS];
};

typedef struct FakeConfig
{
    bool open_blocks;        /* Open waits until the driver cancels it */
    bool open_fails;         /* Open fails at once, as when Windows cannot open the device */
    bool bonded;             /* Open reports a Windows bond */
    int subscribe_fail;      /* The characteristic whose next subscriptions fail, -1 for none */
    int subscribe_failures;  /* How many of them fail */
    SDL_BLEStatus subscribe_status;
    Uint8 subscribe_error;   /* The ATT error of a ProtocolError status */
    Uint8 battery;           /* The Battery Level a read returns */
    bool write_blocks;       /* A write, as to a dying link, fails at its flag or its ceiling */
    int write_delay_ms;      /* A write takes this long, then succeeds unless its flag ended it */
    bool close_blocks;       /* Close hangs until the scenario releases it */
    bool pair_blocks;        /* Pair runs until the driver cancels it */
    bool write_holds;        /* A write is held until the scenario releases it, whatever its flag says */
    bool subscribe_holds;    /* The same for a subscription */
} FakeConfig;

typedef struct FakeTransport
{
    SDL_Mutex *mutex;
    FakeConfig config;
    int inits;
    int quits;
    SDL_ThreadID init_thread;  /* Where the last Init ran */
    SDL_ThreadID quit_thread;  /* Where the last Quit ran */
    int thread_inits;          /* Executors that joined the MTA */
    int thread_quits;          /* Executors that left it */
    int listeners_added;
    int listeners_removed;
    SDL_BLEGATT_Listener listener;
    void *listener_userdata;
    bool watcher_aborted;         /* The next check finds the watcher stopped */
    int watcher_restarts;
    int checks_without_listener;  /* A watcher check with no listener added */
    int opens_entered;
    int opens_left;
    int pairs_entered;
    int nlinks;
    SDL_BLEGATTLink *links[FAKE_MAX_LINKS];
    int ncalls;
    FakeCall calls[FAKE_MAX_CALLS];
    int bad_calls;        /* Calls on a link the fake never opened or already closed */
    int null_closes;      /* Close with no link */
    int early_quits;      /* SDL_BLEGATT_Quit while a link was open or an Open ran */
    bool quit_returned;   /* SDL_Quit has returned, and the driver must stay silent */
    int calls_after_quit;
    int closes_hanging;   /* Close calls that hang now */
    SDL_AtomicInt close_release; /* Ends every hanging Close */
    int calls_held;       /* Writes and subscriptions held now */
    SDL_AtomicInt hold_release; /* Ends every hold */
} FakeTransport;

static FakeTransport fake;

/* Characteristic properties as the devices report them, from the transport
   rows of the part's device sections. Discover finds a characteristic only
   when its UUID is listed here. */
static const struct
{
    const char *uuid;
    Uint8 properties;
} fake_characteristic_table[] = {
    /* Poke Ball Plus input */
    { "6675e16c-f36d-4567-bb55-6b51e27a23e6", SDL_BLE_PROPERTY_NOTIFY },
    /* Battery Level */
    { "00002a19-0000-1000-8000-00805f9b34fb", SDL_BLE_PROPERTY_READ | SDL_BLE_PROPERTY_NOTIFY },
    /* Daydream pose */
    { "00000001-1000-1000-8000-00805f9b34fb", SDL_BLE_PROPERTY_NOTIFY },
    /* Gear VR data and command */
    { "c8c51726-81bc-483b-a052-f7a14ea3d281", SDL_BLE_PROPERTY_READ | SDL_BLE_PROPERTY_NOTIFY },
    { "c8c51726-81bc-483b-a052-f7a14ea3d282", SDL_BLE_PROPERTY_READ | SDL_BLE_PROPERTY_WRITE },
    /* Oculus Go 8126FACE and 8126BEEF */
    { "8126face-3692-ae93-e711-270f223c83b3", SDL_BLE_PROPERTY_NOTIFY },
    { "8126beef-3692-ae93-e711-270f223c83b3", SDL_BLE_PROPERTY_NOTIFY },
    /* Guitar Hero Live iOS input */
    { "533e1524-3abe-f33f-cd00-594e8b0a8ea3", SDL_BLE_PROPERTY_NOTIFY },
    /* Zwift ASYNC, SYNC_TX and SYNC_RX */
    { "00000002-19ca-4651-86e5-fa29dcdd09d1", SDL_BLE_PROPERTY_NOTIFY },
    { "00000004-19ca-4651-86e5-fa29dcdd09d1", SDL_BLE_PROPERTY_INDICATE | SDL_BLE_PROPERTY_READ },
    { "00000003-19ca-4651-86e5-fa29dcdd09d1", SDL_BLE_PROPERTY_WRITE | SDL_BLE_PROPERTY_WRITE_WITHOUT_RESPONSE },
    /* Myo IMU data, classifier event, motion event and command */
    { "d5060402-a904-deb9-4748-2c7f4a124842", SDL_BLE_PROPERTY_NOTIFY },
    { "d5060103-a904-deb9-4748-2c7f4a124842", SDL_BLE_PROPERTY_INDICATE },
    { "d5060502-a904-deb9-4748-2c7f4a124842", SDL_BLE_PROPERTY_INDICATE },
    { "d5060401-a904-deb9-4748-2c7f4a124842", SDL_BLE_PROPERTY_WRITE },
};

static SDL_BLEUUID fake_characteristic_uuids[SDL_arraysize(fake_characteristic_table)];
static SDL_BLEUUID fake_battery_uuid;

static void Fake_Setup(void)
{
    size_t i;

    for (i = 0; i < SDL_arraysize(fake_characteristic_table); ++i) {
        fake_characteristic_uuids[i] = DT_UUID(fake_characteristic_table[i].uuid);
    }
    fake_battery_uuid = DT_UUID("00002a19-0000-1000-8000-00805f9b34fb");
    fake.mutex = SDL_CreateMutex();
}

/* Frees the links and clears every record. SDL is not running. */
static void Fake_Reset(void)
{
    SDL_Mutex *mutex = fake.mutex;
    int i;

    SDL_LockMutex(mutex);
    for (i = 0; i < fake.nlinks; ++i) {
        SDL_free(fake.links[i]);
    }
    SDL_zero(fake);
    fake.mutex = mutex;
    fake.config.subscribe_fail = -1;
    fake.config.battery = DT_BATTERY;
    SDL_UnlockMutex(mutex);
}

static Uint8 Fake_Properties(const SDL_BLEUUID *uuid)
{
    size_t i;

    for (i = 0; i < SDL_arraysize(fake_characteristic_table); ++i) {
        if (SDL_memcmp(fake_characteristic_uuids[i].bytes, uuid->bytes, sizeof(uuid->bytes)) == 0) {
            return fake_characteristic_table[i].properties;
        }
    }
    return 0;
}

/* The caller holds the mutex from here to Fake_Record */
static int Fake_Index(const SDL_BLEGATTLink *link)
{
    int i;

    for (i = 0; link && i < fake.nlinks; ++i) {
        if (fake.links[i] == link) {
            return i;
        }
    }
    return -1;
}

/* True when the fake opened the link and has not closed it. Counts a call
   on any other link as bad. */
static bool Fake_Live(const SDL_BLEGATTLink *link)
{
    const int index = Fake_Index(link);

    if (index < 0 || fake.links[index]->closes != 0) {
        ++fake.bad_calls;
        return false;
    }
    return true;
}

static bool Fake_Canceled(SDL_AtomicInt *cancel)
{
    return cancel && SDL_GetAtomicInt(cancel) != 0;
}

/* Holds a write or a subscription, as a GATT call whose answer has not come
   back, until the scenario releases it. The call's flag does not end the
   hold, so the answer can come after the session has ended. */
static void Fake_Hold(void)
{
    const ULONGLONG start = GetTickCount64();

    SDL_LockMutex(fake.mutex);
    ++fake.calls_held;
    SDL_UnlockMutex(fake.mutex);
    while (!SDL_GetAtomicInt(&fake.hold_release) && GetTickCount64() - start < FAKE_HOLD_CEILING_MS) {
        SDL_Delay(1);
    }
    SDL_LockMutex(fake.mutex);
    --fake.calls_held;
    SDL_UnlockMutex(fake.mutex);
}

static void Fake_Call(FakeCall *call, int kind, int link)
{
    SDL_zerop(call);
    call->kind = kind;
    call->link = link;
    call->characteristic = -1;
    call->cccd = -1;
}

static void Fake_Record(const FakeCall *call)
{
    if (fake.quit_returned) {
        ++fake.calls_after_quit;
    }
    if (fake.ncalls < FAKE_MAX_CALLS) {
        fake.calls[fake.ncalls] = *call;
        fake.calls[fake.ncalls].time_ns = SDL_GetTicksNS();
        ++fake.ncalls;
    }
}

bool Fake_BLEGATT_Init(void)
{
    FakeCall call;

    SDL_LockMutex(fake.mutex);
    ++fake.inits;
    fake.init_thread = SDL_GetCurrentThreadID();
    Fake_Call(&call, FAKE_INIT, -1);
    call.result = true;
    Fake_Record(&call);
    SDL_UnlockMutex(fake.mutex);
    return true;
}

void Fake_BLEGATT_Quit(void)
{
    FakeCall call;
    int i;

    SDL_LockMutex(fake.mutex);
    ++fake.quits;
    fake.quit_thread = SDL_GetCurrentThreadID();
    /* The transport's runtime must outlive every link and every Open, so
       the driver quits it only after its threads have ended */
    for (i = 0; i < fake.nlinks; ++i) {
        if (fake.links[i]->closes == 0) {
            ++fake.early_quits;
        }
    }
    if (fake.opens_entered != fake.opens_left) {
        ++fake.early_quits;
    }
    Fake_Call(&call, FAKE_QUIT, -1);
    call.result = true;
    Fake_Record(&call);
    SDL_UnlockMutex(fake.mutex);
}

/* An executor joins and leaves the MTA through these. They are not recorded
   as calls, since no link is involved. */
bool Fake_BLEGATT_InitThread(void)
{
    SDL_LockMutex(fake.mutex);
    ++fake.thread_inits;
    SDL_UnlockMutex(fake.mutex);
    return true;
}

void Fake_BLEGATT_QuitThread(void)
{
    SDL_LockMutex(fake.mutex);
    ++fake.thread_quits;
    SDL_UnlockMutex(fake.mutex);
}

/* Starts a new watcher in place of one a scenario marked as stopped */
bool Fake_BLEGATT_CheckWatcher(void)
{
    FakeCall call;

    SDL_LockMutex(fake.mutex);
    if (!fake.listener) {
        ++fake.checks_without_listener;
    }
    Fake_Call(&call, FAKE_CHECK_WATCHER, -1);
    call.result = fake.watcher_aborted;
    if (fake.watcher_aborted) {
        fake.watcher_aborted = false;
        ++fake.watcher_restarts;
    }
    Fake_Record(&call);
    SDL_UnlockMutex(fake.mutex);
    return call.result;
}

bool Fake_BLEGATT_AddListener(SDL_BLEGATT_Listener listener, void *userdata)
{
    FakeCall call;

    SDL_LockMutex(fake.mutex);
    ++fake.listeners_added;
    if (fake.listener) {
        ++fake.bad_calls; /* A second listener while one is in place */
    }
    fake.listener = listener;
    fake.listener_userdata = userdata;
    Fake_Call(&call, FAKE_ADD_LISTENER, -1);
    call.result = true;
    Fake_Record(&call);
    SDL_UnlockMutex(fake.mutex);
    return true;
}

void Fake_BLEGATT_RemoveListener(SDL_BLEGATT_Listener listener, void *userdata)
{
    FakeCall call;

    SDL_LockMutex(fake.mutex);
    ++fake.listeners_removed;
    Fake_Call(&call, FAKE_REMOVE_LISTENER, -1);
    call.result = (fake.listener == listener && fake.listener_userdata == userdata);
    if (call.result) {
        fake.listener = NULL;
        fake.listener_userdata = NULL;
    } else {
        ++fake.bad_calls;
    }
    Fake_Record(&call);
    SDL_UnlockMutex(fake.mutex);
}

SDL_BLEGATTLink *Fake_BLEGATT_Open(Uint64 address, SDL_AtomicInt *lost, SDL_AtomicInt *cancel, bool *bonded)
{
    SDL_BLEGATTLink *link = NULL;
    FakeConfig config;
    FakeCall call;

    SDL_LockMutex(fake.mutex);
    ++fake.opens_entered;
    config = fake.config;
    SDL_UnlockMutex(fake.mutex);

    if (config.open_blocks) {
        /* A connect that never completes, as with a device out of range,
           until the driver cancels it or the ceiling passes */
        const Uint64 start = SDL_GetTicks();

        while (!Fake_Canceled(cancel) && SDL_GetTicks() - start < FAKE_OPEN_CEILING_MS) {
            SDL_Delay(1);
        }
    } else if (!config.open_fails && !Fake_Canceled(cancel)) {
        link = (SDL_BLEGATTLink *)SDL_calloc(1, sizeof(*link));
    }

    SDL_LockMutex(fake.mutex);
    if (link) {
        if (fake.nlinks < FAKE_MAX_LINKS) {
            link->index = fake.nlinks;
            link->address = address;
            link->lost = lost;
            fake.links[fake.nlinks++] = link;
        } else {
            SDL_free(link);
            link = NULL;
        }
    }
    Fake_Call(&call, FAKE_OPEN, link ? link->index : -1);
    call.address = address;
    call.result = (link != NULL);
    Fake_Record(&call);
    ++fake.opens_left;
    SDL_UnlockMutex(fake.mutex);

    if (bonded) {
        *bonded = link ? config.bonded : false;
    }
    return link;
}

bool Fake_BLEGATT_Pair(SDL_BLEGATTLink *link, SDL_AtomicInt *cancel)
{
    FakeConfig config;
    FakeCall call;

    SDL_LockMutex(fake.mutex);
    ++fake.pairs_entered;
    config = fake.config;
    SDL_UnlockMutex(fake.mutex);
    if (config.pair_blocks) {
        /* A pairing that runs, as one waiting on the user can, until the
           driver cancels it or the transport's ceiling passes */
        const Uint64 start = SDL_GetTicks();

        while (!Fake_Canceled(cancel) && SDL_GetTicks() - start < FAKE_PAIR_CEILING_MS) {
            SDL_Delay(1);
        }
    }

    SDL_LockMutex(fake.mutex);
    Fake_Call(&call, FAKE_PAIR, Fake_Index(link));
    call.result = Fake_Live(link) && !Fake_Canceled(cancel);
    Fake_Record(&call);
    SDL_UnlockMutex(fake.mutex);
    return call.result;
}

bool Fake_BLEGATT_RemoveBond(SDL_BLEGATTLink *link, SDL_AtomicInt *cancel)
{
    FakeCall call;

    SDL_LockMutex(fake.mutex);
    Fake_Call(&call, FAKE_REMOVE_BOND, Fake_Index(link));
    call.result = Fake_Live(link) && !Fake_Canceled(cancel);
    Fake_Record(&call);
    SDL_UnlockMutex(fake.mutex);
    return call.result;
}

bool Fake_BLEGATT_Discover(SDL_BLEGATTLink *link, const SDL_BLEFamily *family, bool *found, Uint8 *properties,
                           SDL_AtomicInt *cancel)
{
    FakeCall call;
    int i;

    SDL_LockMutex(fake.mutex);
    Fake_Call(&call, FAKE_DISCOVER, Fake_Index(link));
    call.result = Fake_Live(link) && family && found && properties && !Fake_Canceled(cancel);
    if (call.result) {
        link->family = family;
        for (i = 0; i < family->ncharacteristics && i < SDL_BLE_MAX_CHARS; ++i) {
            properties[i] = Fake_Properties(&family->characteristics[i].uuid);
            found[i] = (properties[i] != 0);
        }
    }
    Fake_Record(&call);
    SDL_UnlockMutex(fake.mutex);
    return call.result;
}

SDL_BLEStatus Fake_BLEGATT_Subscribe(SDL_BLEGATTLink *link, int characteristic, Uint8 cccd, Uint8 *protocol_error,
                                     SDL_BLEGATT_ValueCallback callback, void *userdata, SDL_AtomicInt *cancel)
{
    SDL_BLEStatus status = SDL_BLE_STATUS_SUCCESS;
    FakeCall call;
    bool hold;

    SDL_LockMutex(fake.mutex);
    hold = fake.config.subscribe_holds;
    SDL_UnlockMutex(fake.mutex);
    if (hold) {
        Fake_Hold();
    }

    SDL_LockMutex(fake.mutex);
    Fake_Call(&call, FAKE_SUBSCRIBE, Fake_Index(link));
    call.characteristic = characteristic;
    call.cccd = cccd;
    if (!Fake_Live(link) || characteristic < 0 || characteristic >= SDL_BLE_MAX_CHARS || !callback) {
        status = SDL_BLE_STATUS_FAILED;
    } else if (Fake_Canceled(cancel)) {
        status = SDL_BLE_STATUS_FAILED;
    } else {
        /* The callback is registered before the descriptor write, so it
           stays in place when the write fails */
        link->callbacks[characteristic] = callback;
        link->userdata[characteristic] = userdata;
        if (characteristic == fake.config.subscribe_fail && fake.config.subscribe_failures > 0) {
            --fake.config.subscribe_failures;
            status = fake.config.subscribe_status;
            if (protocol_error && status == SDL_BLE_STATUS_PROTOCOL_ERROR) {
                *protocol_error = fake.config.subscribe_error;
            }
        }
    }
    call.status = status;
    call.result = (status == SDL_BLE_STATUS_SUCCESS);
    Fake_Record(&call);
    SDL_UnlockMutex(fake.mutex);
    return status;
}

bool Fake_BLEGATT_Read(SDL_BLEGATTLink *link, int characteristic, Uint8 *data, size_t *length, SDL_AtomicInt *cancel)
{
    FakeCall call;

    SDL_LockMutex(fake.mutex);
    Fake_Call(&call, FAKE_READ, Fake_Index(link));
    call.characteristic = characteristic;
    call.result = Fake_Live(link) && link->family && characteristic >= 0 &&
                  characteristic < link->family->ncharacteristics &&
                  SDL_memcmp(link->family->characteristics[characteristic].uuid.bytes, fake_battery_uuid.bytes,
                             sizeof(fake_battery_uuid.bytes)) == 0 &&
                  data && length && *length >= 1 && !Fake_Canceled(cancel);
    if (call.result) {
        data[0] = fake.config.battery;
        *length = 1;
    }
    Fake_Record(&call);
    SDL_UnlockMutex(fake.mutex);
    return call.result;
}

bool Fake_BLEGATT_Write(SDL_BLEGATTLink *link, int characteristic, const Uint8 *data, size_t length, bool response,
                        SDL_AtomicInt *cancel)
{
    FakeConfig config;
    FakeCall call;
    bool completed = true;

    SDL_LockMutex(fake.mutex);
    config = fake.config;
    SDL_UnlockMutex(fake.mutex);
    if (config.write_holds) {
        Fake_Hold();
    }
    if (config.write_blocks || config.write_delay_ms > 0) {
        /* A write to a dying link runs until its flag ends it or the
           transport's ceiling passes, and then fails. A slow write
           completes after its delay unless its flag ends it first. */
        const Uint64 start = SDL_GetTicks();
        const Uint64 limit = config.write_blocks ? FAKE_WRITE_CEILING_MS : (Uint64)config.write_delay_ms;

        while (!Fake_Canceled(cancel) && SDL_GetTicks() - start < limit) {
            SDL_Delay(1);
        }
        completed = !config.write_blocks;
    }

    SDL_LockMutex(fake.mutex);
    Fake_Call(&call, FAKE_WRITE, Fake_Index(link));
    call.characteristic = characteristic;
    call.response = response;
    call.length = length;
    if (data && length) {
        SDL_memcpy(call.data, data, SDL_min(length, sizeof(call.data)));
    }
    call.result = Fake_Live(link) && completed && (data || length == 0) && !Fake_Canceled(cancel);
    Fake_Record(&call);
    SDL_UnlockMutex(fake.mutex);
    return call.result;
}

void Fake_BLEGATT_Close(SDL_BLEGATTLink *link)
{
    FakeCall call;
    bool hang;
    int index;

    SDL_LockMutex(fake.mutex);
    index = Fake_Index(link);
    Fake_Call(&call, FAKE_CLOSE, index);
    if (!link) {
        ++fake.null_closes;
    } else if (index < 0) {
        ++fake.bad_calls;
    } else {
        ++fake.links[index]->closes;
    }
    call.result = (index >= 0);
    Fake_Record(&call);
    hang = fake.config.close_blocks && index >= 0;
    if (hang) {
        ++fake.closes_hanging;
    }
    SDL_UnlockMutex(fake.mutex);

    if (hang) {
        /* A Close that hangs, as GattDeviceService.Close sometimes does
           (bleak backends/winrt/client.py:488-491), until the scenario
           releases it. The hang outlasts SDL_Quit, which starts SDL's ticks
           over, so its ceiling runs on GetTickCount64. */
        const ULONGLONG start = GetTickCount64();

        while (!SDL_GetAtomicInt(&fake.close_release) && GetTickCount64() - start < FAKE_CLOSE_CEILING_MS) {
            SDL_Delay(1);
        }
        SDL_LockMutex(fake.mutex);
        --fake.closes_hanging;
        SDL_UnlockMutex(fake.mutex);
    }
}

/* What the test reads from the fake */

static int DT_Count(int kind, int link)
{
    int i, count = 0;

    SDL_LockMutex(fake.mutex);
    for (i = 0; i < fake.ncalls; ++i) {
        if (fake.calls[i].kind == kind && (link < 0 || fake.calls[i].link == link)) {
            ++count;
        }
    }
    SDL_UnlockMutex(fake.mutex);
    return count;
}

/* The position in the record of the nth call of that kind on the link, or -1 */
static int DT_Position(int kind, int link, int nth, FakeCall *out)
{
    int i, position = -1;

    SDL_LockMutex(fake.mutex);
    for (i = 0; i < fake.ncalls; ++i) {
        if (fake.calls[i].kind == kind && (link < 0 || fake.calls[i].link == link) && nth-- == 0) {
            position = i;
            if (out) {
                *out = fake.calls[i];
            }
            break;
        }
    }
    SDL_UnlockMutex(fake.mutex);
    return position;
}

/* The newest link to the address, or -1 */
static int DT_Link(Uint64 address)
{
    int i, index = -1;

    SDL_LockMutex(fake.mutex);
    for (i = 0; i < fake.nlinks; ++i) {
        if (fake.links[i]->address == address) {
            index = i;
        }
    }
    SDL_UnlockMutex(fake.mutex);
    return index;
}

static bool DT_ListenerPresent(void)
{
    bool present;

    SDL_LockMutex(fake.mutex);
    present = (fake.listener != NULL);
    SDL_UnlockMutex(fake.mutex);
    return present;
}

static void DT_SetConfig(const FakeConfig *config)
{
    SDL_LockMutex(fake.mutex);
    fake.config = *config;
    SDL_UnlockMutex(fake.mutex);
}

static void DT_GetConfig(FakeConfig *config)
{
    SDL_LockMutex(fake.mutex);
    *config = fake.config;
    SDL_UnlockMutex(fake.mutex);
}

/* One expected call on a link. characteristic and cccd of -1 match any. */
typedef struct DT_Step
{
    int kind;
    int characteristic;
    int cccd;
} DT_Step;

#define DT_OPEN      { FAKE_OPEN, -1, -1 }
#define DT_PAIR      { FAKE_PAIR, -1, -1 }
#define DT_DISCOVER  { FAKE_DISCOVER, -1, -1 }
#define DT_SUB(c, v) { FAKE_SUBSCRIBE, (c), (v) }
#define DT_READ(c)   { FAKE_READ, (c), -1 }
#define DT_WRITE(c)  { FAKE_WRITE, (c), -1 }
#define DT_CLOSE     { FAKE_CLOSE, -1, -1 }

static void DT_DescribeCall(const FakeCall *call, char *out, size_t size)
{
    static const char *const cccd_names[] = { "none", "notify", "indicate" };
    char hex[3 * SDL_BLE_MAX_WRITE + 1];

    switch (call->kind) {
    case FAKE_SUBSCRIBE:
        (void)SDL_snprintf(out, size, "subscribe %d %s%s", call->characteristic,
                           (call->cccd >= 0 && call->cccd <= 2) ? cccd_names[call->cccd] : "?",
                           call->result ? "" : " (failed)");
        break;
    case FAKE_READ:
        (void)SDL_snprintf(out, size, "read %d", call->characteristic);
        break;
    case FAKE_WRITE:
        DT_HexText(call->data, SDL_min(call->length, sizeof(call->data)), hex, sizeof(hex));
        (void)SDL_snprintf(out, size, "write %d [%s]%s", call->characteristic, hex, call->response ? " with response" : "");
        break;
    default:
        (void)SDL_snprintf(out, size, "%s", fake_kind_names[call->kind]);
        break;
    }
}

/* Checks the calls on the link against steps. With exact, the link has no
   other call. */
static bool DT_CheckSteps(int line, int link, const DT_Step *steps, int nsteps, bool exact)
{
    char actual[1024], text[160];
    bool ok = true;
    int i, n = 0;

    actual[0] = '\0';
    SDL_LockMutex(fake.mutex);
    for (i = 0; i < fake.ncalls; ++i) {
        const FakeCall *call = &fake.calls[i];

        if (call->link != link) {
            continue;
        }
        if (n < nsteps) {
            const DT_Step *step = &steps[n];

            if (call->kind != step->kind || (step->characteristic >= 0 && call->characteristic != step->characteristic) ||
                (step->cccd >= 0 && call->cccd != step->cccd)) {
                ok = false;
            }
        } else if (exact) {
            ok = false;
        }
        ++n;
        DT_DescribeCall(call, text, sizeof(text));
        if (SDL_strlen(actual) + SDL_strlen(text) + 3 < sizeof(actual)) {
            if (actual[0]) {
                SDL_strlcat(actual, ", ", sizeof(actual));
            }
            SDL_strlcat(actual, text, sizeof(actual));
        }
    }
    SDL_UnlockMutex(fake.mutex);
    if (n < nsteps) {
        ok = false;
    }
    return DT_Check(ok, line, "link %d made the calls: %s", link, actual[0] ? actual : "none");
}

/* The nth write on the link went to that characteristic with these bytes and
   that response flag, and the fake delivered it */
static bool DT_CheckWrite(int line, int link, int nth, int characteristic, const Uint8 *data, size_t length, bool response)
{
    char want[3 * SDL_BLE_MAX_WRITE + 1], got[3 * SDL_BLE_MAX_WRITE + 1];
    FakeCall call;

    if (DT_Position(FAKE_WRITE, link, nth, &call) < 0) {
        return DT_Check(false, line, "write %d on link %d never came", nth, link);
    }
    DT_HexText(data, length, want, sizeof(want));
    DT_HexText(call.data, SDL_min(call.length, sizeof(call.data)), got, sizeof(got));
    return DT_Check(call.characteristic == characteristic && call.length == length &&
                        SDL_memcmp(call.data, data, length) == 0 && call.response == response && call.result,
                    line, "write %d on link %d: characteristic %d [%s] response %d delivered %d, expected characteristic %d [%s] response %d",
                    nth, link, call.characteristic, got, call.response ? 1 : 0, call.result ? 1 : 0, characteristic, want,
                    response ? 1 : 0);
}

/* Advertisements and values */

typedef struct DT_AdCall
{
    SDL_BLEGATT_Listener listener;
    void *userdata;
    SDL_BLEAdvertisement ad;
} DT_AdCall;

static int SDLCALL DT_AdThread(void *data)
{
    DT_AdCall *call = (DT_AdCall *)data;

    call->listener(&call->ad, call->userdata);
    return 0;
}

/* Hands an advertisement to the driver's listener on a thread of its own, as
   the watcher does from the thread pool. False without a listener. */
static bool DT_Advertise(const SDL_BLEAdvertisement *ad)
{
    SDL_Thread *thread;
    DT_AdCall call;

    SDL_LockMutex(fake.mutex);
    call.listener = fake.listener;
    call.userdata = fake.listener_userdata;
    SDL_UnlockMutex(fake.mutex);
    if (!call.listener) {
        return false;
    }
    call.ad = *ad;
    thread = SDL_CreateThread(DT_AdThread, "FakeBLEWatcher", &call);
    if (!thread) {
        return false;
    }
    SDL_WaitThread(thread, NULL);
    return true;
}

static void DT_Ad(SDL_BLEAdvertisement *ad, Uint64 address, SDL_BLEAdvertisementKind kind, const char *name)
{
    SDL_zerop(ad);
    ad->address = address;
    ad->kind = (Uint8)kind;
    if (name) {
        ad->has_name = true;
        SDL_strlcpy(ad->name, name, sizeof(ad->name));
    }
}

static void DT_AdService(SDL_BLEAdvertisement *ad, const char *uuid)
{
    if (ad->nservices < SDL_BLE_AD_SERVICES) {
        ad->services[ad->nservices++] = DT_UUID(uuid);
    }
}

static void DT_AdManufacturer(SDL_BLEAdvertisement *ad, Uint16 company, const Uint8 *data, Uint8 length)
{
    SDL_BLEManufacturerData *entry;

    if (ad->nmanufacturer >= SDL_BLE_AD_MANUFACTURER || length > SDL_BLE_AD_MANUFACTURER_DATA) {
        return;
    }
    entry = &ad->manufacturer[ad->nmanufacturer++];
    entry->company = company;
    entry->length = length;
    SDL_memcpy(entry->data, data, length);
}

/* A value on a characteristic, as the transport delivers one: through the
   callback the driver subscribed, as an exact-size heap copy so
   AddressSanitizer sees a read past it. The copy comes from the C runtime,
   since SDL_malloc allocates 1 byte for 0 (SDL_malloc.c:6566-6568).
   MSVC's malloc(0) returns a block,
   so an empty value arrives as a zero-byte block, and a read of its first
   byte is past the end. False when the link is closed or the characteristic
   has no callback. */
static bool DT_Push(int link, int characteristic, const Uint8 *data, size_t length)
{
    SDL_BLEGATT_ValueCallback callback = NULL;
    void *userdata = NULL;
    Uint8 *copy;

    SDL_LockMutex(fake.mutex);
    if (link >= 0 && link < fake.nlinks && fake.links[link]->closes == 0 && characteristic >= 0 &&
        characteristic < SDL_BLE_MAX_CHARS) {
        callback = fake.links[link]->callbacks[characteristic];
        userdata = fake.links[link]->userdata[characteristic];
    }
    SDL_UnlockMutex(fake.mutex);
    if (!callback) {
        return false;
    }
    copy = (Uint8 *)malloc(length);
    if (!copy) {
        return false;
    }
    if (length) {
        SDL_memcpy(copy, data, length);
    }
    callback(userdata, characteristic, copy, length);
    free(copy);
    return true;
}

/* The writes and subscriptions the fake holds now */
static int DT_Held(void)
{
    int held;

    SDL_LockMutex(fake.mutex);
    held = fake.calls_held;
    SDL_UnlockMutex(fake.mutex);
    return held;
}

/* Sets the atomic Open received, as ConnectionStatusChanged does */
static bool DT_LoseLink(int link)
{
    bool lost = false;

    SDL_LockMutex(fake.mutex);
    if (link >= 0 && link < fake.nlinks && fake.links[link]->closes == 0 && fake.links[link]->lost) {
        SDL_SetAtomicInt(fake.links[link]->lost, 1);
        lost = true;
    }
    SDL_UnlockMutex(fake.mutex);
    return lost;
}

/* SDL */

static bool DT_Expired(Uint64 start, int timeout_ms)
{
    return SDL_GetTicks() - start >= (Uint64)timeout_ms;
}

/* What the driver's connection to an address holds for Update */
typedef struct DT_Queues
{
    bool found;
    Uint64 sequence; /* The last change or sample queued */
    int changes;
    int samples;
    Uint32 changes_dropped;
    Uint32 samples_dropped;
    Uint32 empty; /* Empty values the value queue ignored */
    bool sensors;
} DT_Queues;

/* The joystick thread owns the connection list, so this runs under the
   joystick lock, and reads the queues under the connection's mutex */
static DT_Queues DT_GetQueues(Uint64 address)
{
    BLEGATT_Connection *connection;
    DT_Queues queues;

    SDL_zero(queues);
    SDL_LockJoysticks();
    for (connection = blegatt_connections; connection; connection = connection->next) {
        if (connection->address == address) {
            SDL_LockMutex(connection->mutex);
            queues.found = true;
            queues.sequence = connection->sequence;
            queues.changes = connection->change_count;
            queues.samples = connection->sample_count;
            queues.changes_dropped = connection->changes_dropped;
            queues.samples_dropped = connection->samples_dropped;
            queues.empty = connection->values.empty;
            queues.sensors = connection->sensors;
            SDL_UnlockMutex(connection->mutex);
        }
    }
    SDL_UnlockJoysticks();
    return queues;
}

/* Waits with no SDL update, so nothing leaves the queues, until the
   connection to the address has queued count changes and samples after the
   sequence number from. True when exactly count came, after a pause that
   lets any more arrive. */
static bool DT_WaitQueued(Uint64 address, Uint64 from, Uint64 count, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        DT_Queues queues = DT_GetQueues(address);

        if (queues.found && queues.sequence - from >= count) {
            SDL_Delay(50);
            queues = DT_GetQueues(address);
            return queues.sequence - from == count;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        SDL_Delay(1);
    }
}

/* The driver's connection to the address, NULL when it has none */
static BLEGATT_Connection *DT_FindConnection(Uint64 address)
{
    BLEGATT_Connection *connection, *found = NULL;

    SDL_LockJoysticks();
    for (connection = blegatt_connections; connection; connection = connection->next) {
        if (connection->address == address) {
            found = connection;
        }
    }
    SDL_UnlockJoysticks();
    return found;
}

/* A streaming device keeps streaming while a scenario waits. With a value
   set, DT_Pump sends it again once DT_KEEPALIVE_MS has passed since it last
   went out, so a module's silence timeout cannot fire however slowly the
   scenario runs. */
#define DT_KEEPALIVE_MS (SDL_GEARVR_SILENCE_MS / 3)

static struct
{
    bool on;
    int link;
    int characteristic;
    Uint8 data[SDL_BLE_MAX_VALUE];
    size_t length;
    Uint64 sent;
} dt_keepalive;

static void DT_KeepAlive(int link, int characteristic, const Uint8 *data, size_t length)
{
    dt_keepalive.on = true;
    dt_keepalive.link = link;
    dt_keepalive.characteristic = characteristic;
    dt_keepalive.length = SDL_min(length, sizeof(dt_keepalive.data));
    SDL_memcpy(dt_keepalive.data, data, dt_keepalive.length);
    dt_keepalive.sent = SDL_GetTicks();
}

static void DT_StopKeepAlive(void)
{
    dt_keepalive.on = false;
}

/* One pass of SDL's joystick update, which runs every driver's Detect and
   the opened joysticks' Update, then a short sleep */
static void DT_Pump(void)
{
    if (dt_keepalive.on && SDL_GetTicks() - dt_keepalive.sent >= DT_KEEPALIVE_MS) {
        dt_keepalive.sent = SDL_GetTicks();
        (void)DT_Push(dt_keepalive.link, dt_keepalive.characteristic, dt_keepalive.data, dt_keepalive.length);
    }
    SDL_UpdateJoysticks();
    SDL_Delay(1);
}

static void DT_PumpFor(int ms)
{
    const Uint64 start = SDL_GetTicks();

    while (!DT_Expired(start, ms)) {
        DT_Pump();
    }
}

static bool DT_WaitCalls(int kind, int link, int count, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (DT_Count(kind, link) >= count) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

/* Waits, with SDL's updates, until the fake holds count calls */
static bool DT_WaitHeld(int count, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (DT_Held() >= count) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

static bool DT_WaitListener(bool present, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (DT_ListenerPresent() == present) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

typedef struct DT_ListenerWait
{
    bool present;
    int timeout_ms;
    bool reached;
} DT_ListenerWait;

static int SDLCALL DT_ListenerWaitThread(void *data)
{
    DT_ListenerWait *wait = (DT_ListenerWait *)data;

    wait->reached = DT_WaitListener(wait->present, wait->timeout_ms);
    return 0;
}

/* As DT_WaitListener, with SDL's joystick update, and so Detect, on a thread
   other than the one that ran SDL_Init */
static bool DT_WaitListenerElsewhere(bool present, int timeout_ms)
{
    DT_ListenerWait wait;
    SDL_Thread *thread;

    wait.present = present;
    wait.timeout_ms = timeout_ms;
    wait.reached = false;
    thread = SDL_CreateThread(DT_ListenerWaitThread, "FakeJoystickUpdate", &wait);
    if (!thread) {
        return false;
    }
    SDL_WaitThread(thread, NULL);
    return wait.reached;
}

/* The joystick count, and the first joystick in first */
static int DT_Joysticks(SDL_JoystickID *first)
{
    SDL_JoystickID *ids;
    int count = 0;

    ids = SDL_GetJoysticks(&count);
    if (first) {
        *first = (ids && count > 0) ? ids[0] : 0;
    }
    SDL_free(ids);
    return count;
}

/* The timestamp of the queued SDL_EVENT_JOYSTICK_REMOVED for the joystick,
   0 when none is queued */
static Uint64 DT_RemovedTime(SDL_JoystickID id)
{
    SDL_Event events[16];
    int count, i;

    count = SDL_PeepEvents(events, DT_COUNT(events), SDL_PEEKEVENT, SDL_EVENT_JOYSTICK_REMOVED, SDL_EVENT_JOYSTICK_REMOVED);
    for (i = 0; i < count; ++i) {
        if (events[i].jdevice.which == id) {
            return events[i].common.timestamp;
        }
    }
    return 0;
}

static bool DT_WaitNoJoystick(int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (DT_Joysticks(NULL) == 0) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

/* Sends the value every 20 ms, as a streaming device does, until one
   joystick is present. Returns it, or 0. */
static SDL_JoystickID DT_StreamUntilJoystick(int link, int characteristic, const Uint8 *data, size_t length)
{
    const Uint64 start = SDL_GetTicks();
    Uint64 sent = 0;
    bool pushed = false;
    SDL_JoystickID id = 0;

    for (;;) {
        if (DT_Joysticks(&id) == 1) {
            return id;
        }
        if (DT_Expired(start, DT_WAIT_MS)) {
            return 0;
        }
        if (!pushed || SDL_GetTicks() - sent >= 20) {
            DT_Push(link, characteristic, data, length);
            sent = SDL_GetTicks();
            pushed = true;
        }
        DT_Pump();
    }
}

/* As DT_StreamUntilJoystick, until count joysticks are present. Returns the
   one that is not other, or 0. */
static SDL_JoystickID DT_StreamUntilJoysticks(int link, int characteristic, const Uint8 *data, size_t length, int count,
                                              SDL_JoystickID other)
{
    const Uint64 start = SDL_GetTicks();
    Uint64 sent = 0;
    bool pushed = false;

    for (;;) {
        SDL_JoystickID *ids;
        int n = 0, i;
        SDL_JoystickID found = 0;

        ids = SDL_GetJoysticks(&n);
        if (ids && n == count) {
            for (i = 0; i < n; ++i) {
                if (ids[i] != other) {
                    found = ids[i];
                }
            }
        }
        SDL_free(ids);
        if (found) {
            return found;
        }
        if (DT_Expired(start, DT_WAIT_MS)) {
            return 0;
        }
        if (!pushed || SDL_GetTicks() - sent >= 20) {
            DT_Push(link, characteristic, data, length);
            sent = SDL_GetTicks();
            pushed = true;
        }
        DT_Pump();
    }
}

static bool DT_WaitJoystickAxis(SDL_Joystick *joystick, int axis, Sint16 value, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (SDL_GetJoystickAxis(joystick, axis) == value) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

static bool DT_WaitButton(SDL_Gamepad *gamepad, SDL_GamepadButton button, bool down, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (SDL_GetGamepadButton(gamepad, button) == down) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

static bool DT_WaitAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis, Sint16 value, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (SDL_GetGamepadAxis(gamepad, axis) == value) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

static bool DT_WaitJoystickButton(SDL_Joystick *joystick, int button, bool down, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (SDL_GetJoystickButton(joystick, button) == down) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

static bool DT_Near(float value, float expected)
{
    return SDL_fabsf(value - expected) <= 1e-3f + 1e-4f * SDL_fabsf(expected);
}

static bool DT_WaitSensor(SDL_Gamepad *gamepad, SDL_SensorType type, const float *expected, float *data, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (SDL_GetGamepadSensorData(gamepad, type, data, 3) && DT_Near(data[0], expected[0]) &&
            DT_Near(data[1], expected[1]) && DT_Near(data[2], expected[2])) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

/* Finger 0 of touchpad 0 reaches that state and position */
static bool DT_WaitFinger(SDL_Gamepad *gamepad, bool down, float x, float y, float *pressure, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();
    bool is_down = false;
    float fx = 0.0f, fy = 0.0f;

    for (;;) {
        if (SDL_GetGamepadTouchpadFinger(gamepad, 0, 0, &is_down, &fx, &fy, pressure) && is_down == down &&
            (!down || (DT_Near(fx, x) && DT_Near(fy, y)))) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

/* The gamepad reports that power state and percentage */
static bool DT_CheckPower(int line, SDL_Gamepad *gamepad, SDL_PowerState state, int percent, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();
    SDL_PowerState got_state;
    int got_percent = -1;

    for (;;) {
        got_state = SDL_GetGamepadPowerInfo(gamepad, &got_percent);
        if (got_state == state && got_percent == percent) {
            return DT_Check(true, line, "power");
        }
        if (DT_Expired(start, timeout_ms)) {
            return DT_Check(false, line, "power state %d at %d percent, expected state %d at %d percent", (int)got_state, got_percent,
                            (int)state, percent);
        }
        DT_Pump();
    }
}

/* The GUID is SDL_CreateJoystickGUID(Bluetooth, vendor, product, 0, NULL,
   key, 'l', type), with 'l' and the type in bytes 14 and 15. The key is the
   family's code, the variant as two hex digits and the name. Each scenario
   writes its key out, so a change to a shipped GUID fails the test. */
static void DT_CheckGUID(int line, SDL_JoystickID id, Uint16 vendor, Uint16 product, const char *key, SDL_JoystickType type)
{
    const SDL_GUID guid = SDL_GetJoystickGUIDForID(id);
    const SDL_GUID expected = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, vendor, product, 0, NULL, key, 'l', (Uint8)type);
    char got_text[33], want_text[33];

    SDL_GUIDToString(guid, got_text, sizeof(got_text));
    SDL_GUIDToString(expected, want_text, sizeof(want_text));
    DT_Check(guid.data[0] == SDL_HARDWARE_BUS_BLUETOOTH && guid.data[1] == 0, line, "GUID %s is not on the Bluetooth bus", got_text);
    DT_Check(guid.data[14] == 'l', line, "GUID %s has signature 0x%02X, not 'l'", got_text, guid.data[14]);
    DT_Check(guid.data[15] == (Uint8)type, line, "GUID %s carries type %d, not %d", got_text, guid.data[15], (int)type);
    DT_Check(SDL_memcmp(guid.data, expected.data, sizeof(guid.data)) == 0, line, "GUID %s, expected %s", got_text, want_text);
}

static void DT_CheckIdentity(int line, SDL_JoystickID id, const char *name, SDL_JoystickType type, bool gamepad)
{
    const char *got = SDL_GetJoystickNameForID(id);

    DT_Check(got && SDL_strcmp(got, name) == 0, line, "name \"%s\", expected \"%s\"", got ? got : "(null)", name);
    DT_Check(SDL_GetJoystickTypeForID(id) == type, line, "joystick type %d, expected %d", (int)SDL_GetJoystickTypeForID(id), (int)type);
    DT_Check(SDL_IsGamepad(id) == gamepad, line, "SDL_IsGamepad is %d, expected %d", SDL_IsGamepad(id) ? 1 : 0, gamepad ? 1 : 0);
}

static bool DT_IsMappingMetaField(const char *field)
{
    static const char *const prefixes[] = { "platform:", "crc:", "type:", "face:", "hint:", "sdk>=:", "sdk<=:" };
    size_t i;

    for (i = 0; i < SDL_arraysize(prefixes); ++i) {
        if (SDL_strncmp(field, prefixes[i], SDL_strlen(prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

/* The gamepad's mapping binds exactly these entries, in any order, past the
   GUID and the name. Fields that describe the mapping itself are skipped. */
static void DT_CheckMapping(int line, SDL_Gamepad *gamepad, const char *const *expected, int nexpected)
{
    char *mapping = SDL_GetGamepadMapping(gamepad);
    char *copy, *field, *next;
    bool matched[32];
    int index = 0, i;

    if (!DT_Check(mapping != NULL, line, "the gamepad has no mapping: %s", SDL_GetError())) {
        return;
    }
    if (!DT_Check(nexpected <= DT_COUNT(matched), line, "%d mapping entries, at most %d", nexpected, DT_COUNT(matched))) {
        SDL_free(mapping);
        return;
    }
    copy = SDL_strdup(mapping);
    SDL_zeroa(matched);
    for (field = copy; field; field = next, ++index) {
        next = SDL_strchr(field, ',');
        if (next) {
            *next++ = '\0';
        }
        if (index < 2 || !*field || DT_IsMappingMetaField(field)) {
            continue;
        }
        for (i = 0; i < nexpected && SDL_strcmp(field, expected[i]) != 0; ++i) {
        }
        if (i == nexpected || matched[i]) {
            DT_Check(false, line, "mapping entry %s is not expected in %s", field, mapping);
        } else {
            matched[i] = true;
        }
    }
    for (i = 0; i < nexpected; ++i) {
        DT_Check(matched[i], line, "mapping lacks %s: %s", expected[i], mapping);
    }
    SDL_free(copy);
    SDL_free(mapping);
}

/* The driver's log. Every line still reaches SDL's default output. The line
   the driver writes when it stops watching is kept for scenario (j), and the
   one it writes when SDL_Quit leaves a connection to end on its own for
   scenario (s). SDL_Quit's report of a thread still joinable is counted. */

static SDL_Mutex *dt_log_lock;
static char dt_log_watch[256];
static int dt_log_watch_lines;
static char dt_log_abandon[256];
static int dt_log_abandon_lines;
static int dt_log_leaked_threads;
static int dt_log_ended_lines;

static void SDLCALL DT_LogOutput(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    SDL_LogOutputFunction output = SDL_GetDefaultLogOutputFunction();

    (void)userdata;
    if (message) {
        SDL_LockMutex(dt_log_lock);
        if (SDL_strstr(message, "stopped watching advertisements")) {
            SDL_strlcpy(dt_log_watch, message, sizeof(dt_log_watch));
            ++dt_log_watch_lines;
        }
        if (SDL_strstr(message, "left to end on its own")) {
            SDL_strlcpy(dt_log_abandon, message, sizeof(dt_log_abandon));
            ++dt_log_abandon_lines;
        }
        if (SDL_strncmp(message, "Leaked thread", 13) == 0) {
            ++dt_log_leaked_threads;
        }
        if (SDL_strstr(message, "): ended, ")) {
            ++dt_log_ended_lines;
        }
        SDL_UnlockMutex(dt_log_lock);
    }
    if (output) {
        output(NULL, category, priority, message);
    }
}

/* The number of stop lines so far, and the last one in line */
static int DT_WatchLog(char *line, size_t size)
{
    int count;

    SDL_LockMutex(dt_log_lock);
    count = dt_log_watch_lines;
    if (line) {
        SDL_strlcpy(line, dt_log_watch, size);
    }
    SDL_UnlockMutex(dt_log_lock);
    return count;
}

/* The number of lines for a connection left to end on its own, and the last
   one in line */
static int DT_AbandonLog(char *line, size_t size)
{
    int count;

    SDL_LockMutex(dt_log_lock);
    count = dt_log_abandon_lines;
    if (line) {
        SDL_strlcpy(line, dt_log_abandon, size);
    }
    SDL_UnlockMutex(dt_log_lock);
    return count;
}

/* The threads SDL_Quit found still joinable, as SDL_SetObjectsInvalid
   logs them (SDL_utils.c:243-270) */
static int DT_LeakedThreads(void)
{
    int count;

    SDL_LockMutex(dt_log_lock);
    count = dt_log_leaked_threads;
    SDL_UnlockMutex(dt_log_lock);
    return count;
}

/* The lines session threads wrote as they ended */
static int DT_EndedLines(void)
{
    int count;

    SDL_LockMutex(dt_log_lock);
    count = dt_log_ended_lines;
    SDL_UnlockMutex(dt_log_lock);
    return count;
}

/* Starting and stopping SDL */

static void DT_SetHint(const char *name, const char *value)
{
    SDL_SetHintWithPriority(name, value, SDL_HINT_OVERRIDE);
}

/* SDL_Init with every other driver off. ble and pairing are the values of
   SDL_JOYSTICK_BLE and SDL_JOYSTICK_BLE_PAIRING, NULL to leave one unset. */
static bool DT_Start(const char *ble, const char *pairing)
{
    size_t i;

    for (i = 0; i < SDL_arraysize(dt_other_drivers); ++i) {
        DT_SetHint(dt_other_drivers[i][0], dt_other_drivers[i][1]);
    }
    if (ble) {
        DT_SetHint(DT_HINT_BLE, ble);
    }
    if (pairing) {
        DT_SetHint(DT_HINT_PAIRING, pairing);
    }
    SDL_SetLogPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_DEBUG);
    /* For SDL_Quit's report of threads still joinable */
    SDL_SetLogPriority(SDL_LOG_CATEGORY_SYSTEM, SDL_LOG_PRIORITY_DEBUG);
    SDL_SetLogOutputFunction(DT_LogOutput, NULL);
    return DT_CHECK(SDL_Init(SDL_INIT_GAMEPAD), "SDL_Init failed: %s", SDL_GetError());
}

/* SDL with the driver on, once its listener is in place */
static bool DT_StartListening(const char *pairing)
{
    if (!DT_Start("1", pairing)) {
        return false;
    }
    return DT_CHECK(DT_WaitListener(true, DT_WAIT_MS),
                    "no listener with SDL_JOYSTICK_BLE on: SDL_BLEGATT_JoystickDriver is not in SDL's driver table, or it did not start");
}

/* The thread that runs main, and with it SDL_Init and SDL_Quit */
static SDL_ThreadID dt_main_thread;

/* SDL_Quit, then what every scenario must leave behind: the transport quit
   as often as it started, on the thread that started it, every executor
   out of the MTA it joined, no listener, every link closed once, no Open
   still running and no call afterwards. Every connection's threads ended
   inside SDL_Quit, so none was left to end on its own and no thread was
   left joinable. Returns how long SDL_Quit took, in ms. */
static int DT_Stop(void)
{
    Uint64 start, end;
    int i, elapsed, abandoned, leaked;

    abandoned = DT_AbandonLog(NULL, 0);
    leaked = DT_LeakedThreads();
    start = SDL_GetPerformanceCounter();
    SDL_Quit();
    end = SDL_GetPerformanceCounter();
    elapsed = (int)((end - start) * 1000 / SDL_GetPerformanceFrequency());
    DT_CHECK(DT_AbandonLog(NULL, 0) == abandoned, "SDL_Quit left %d connections to end on their own",
             DT_AbandonLog(NULL, 0) - abandoned);
    DT_CHECK(DT_LeakedThreads() == leaked, "SDL_Quit found %d threads still joinable", DT_LeakedThreads() - leaked);

    SDL_LockMutex(fake.mutex);
    fake.quit_returned = true;
    DT_CHECK(fake.inits == fake.quits, "SDL_BLEGATT_Init ran %d times and SDL_BLEGATT_Quit %d", fake.inits, fake.quits);
    if (fake.inits > 0) {
        DT_CHECK(fake.init_thread == dt_main_thread && fake.quit_thread == dt_main_thread,
                 "SDL_BLEGATT_Init ran on thread %" SDL_PRIu64 " and SDL_BLEGATT_Quit on %" SDL_PRIu64
                 ", not on %" SDL_PRIu64 ", which ran SDL_Init and SDL_Quit",
                 (Uint64)fake.init_thread, (Uint64)fake.quit_thread, (Uint64)dt_main_thread);
    }
    /* Every Open runs on an executor that joined the MTA first */
    DT_CHECK(fake.thread_inits == fake.thread_quits && fake.thread_inits >= fake.opens_entered,
             "executors joined the MTA %d times and left it %d times for %d Open calls", fake.thread_inits, fake.thread_quits,
             fake.opens_entered);
    DT_CHECK(fake.checks_without_listener == 0, "the watcher was checked %d times with no listener", fake.checks_without_listener);
    DT_CHECK(!fake.listener && fake.listeners_added == fake.listeners_removed,
             "the listener was added %d times and removed %d times", fake.listeners_added, fake.listeners_removed);
    DT_CHECK(fake.opens_entered == fake.opens_left, "%d Open calls still ran after SDL_Quit", fake.opens_entered - fake.opens_left);
    for (i = 0; i < fake.nlinks; ++i) {
        DT_CHECK(fake.links[i]->closes == 1, "link %d was closed %d times", i, fake.links[i]->closes);
    }
    DT_CHECK(fake.bad_calls == 0, "%d calls used a link the fake never opened or had closed", fake.bad_calls);
    DT_CHECK(fake.null_closes == 0, "SDL_BLEGATT_Close ran %d times with no link", fake.null_closes);
    DT_CHECK(fake.early_quits == 0, "SDL_BLEGATT_Quit ran while %d links were open or opening", fake.early_quits);
    SDL_UnlockMutex(fake.mutex);

    DT_CHECK(elapsed < DT_QUIT_LIMIT_MS, "SDL_Quit took %d ms", elapsed);
    SDL_Delay(100);
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.calls_after_quit == 0, "the driver made %d transport calls after SDL_Quit returned", fake.calls_after_quit);
    SDL_UnlockMutex(fake.mutex);
    return elapsed;
}

/* The watchdog ends a scenario that hangs, so the log names it */

static volatile LONG64 dt_deadline;

static unsigned __stdcall DT_Watchdog(void *unused)
{
    (void)unused;
    for (;;) {
        const LONG64 deadline = InterlockedCompareExchange64(&dt_deadline, 0, 0);

        if (deadline && (LONG64)GetTickCount64() > deadline) {
            printf("FAIL (%s): the scenario did not finish within %d s\n", dt_scenario, DT_SCENARIO_LIMIT_MS / 1000);
            fflush(stdout);
            _Exit(1);
        }
        Sleep(100);
    }
}

static int dt_scenario_failures;

static void DT_Begin(const char *id, const char *title)
{
    dt_scenario = id;
    dt_scenario_failures = dt_failures;
    printf("scenario (%s) %s\n", id, title);
    DT_StopKeepAlive();
    Fake_Reset();
    InterlockedExchange64(&dt_deadline, (LONG64)GetTickCount64() + DT_SCENARIO_LIMIT_MS);
}

static void DT_End(void)
{
    InterlockedExchange64(&dt_deadline, 0);
    printf("scenario (%s) %s\n", dt_scenario, (dt_failures == dt_scenario_failures) ? "passed" : "FAILED");
}

/* Recorded packets and their values */

/* Captures from the part's Daydream section: 3 lying flat, 4 touching,
   6 rotating and 7 with volume up held */
static const char dt_daydream_flat[] = "0B 83 FD FC B1 FE BF FE 44 16 00 A0 00 00 00 00 00 00 00 01";
static const char dt_daydream_touch[] = "E5 80 03 BC 71 FE A7 F6 44 16 00 70 00 00 00 00 11 09 60 01";
static const char dt_daydream_rotating[] = "FC 80 00 DB 3B FF 67 FA C4 16 00 D0 02 7D E3 FC 80 00 00 01";
static const char dt_daydream_volume[] = "26 F8 03 61 C2 00 20 02 C4 0F FE 40 00 00 00 00 00 00 10 11";

/* Accelerometer (11, 519, -28) of capture 7 and gyroscope (4, -136, -28)
   of capture 6, at 8 g and 2048 degrees per second per 4095 */
static const float dt_daydream_accel[3] = {
    (float)(11.0 / 4095.0 * 8.0 * 9.80665), (float)(519.0 / 4095.0 * 8.0 * 9.80665), (float)(-28.0 / 4095.0 * 8.0 * 9.80665)
};
static const float dt_daydream_gyro[3] = {
    (float)(4.0 / 4095.0 * 2048.0 * 3.14159265358979323846 / 180.0),
    (float)(-136.0 / 4095.0 * 2048.0 * 3.14159265358979323846 / 180.0),
    (float)(-28.0 / 4095.0 * 2048.0 * 3.14159265358979323846 / 180.0)
};

/* Makinolo's captures from a right Zwift Play half: A pressed, the lever at
   95 and a full pull */
static const char dt_zwift_a[] = "07 08 00 10 01 18 01 20 00 28 01 30 01 38 01 40 00 48 00";
static const char dt_zwift_lever[] = "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 BE 01 48 00";
static const char dt_zwift_pull[] = "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 C8 01";

typedef struct DT_GearVRFixtures
{
    bool loaded;
    Uint8 idle[SDL_GEARVR_PACKET_SIZE];
    Uint8 trigger[SDL_GEARVR_PACKET_SIZE];
    Uint8 touch_center[SDL_GEARVR_PACKET_SIZE];
} DT_GearVRFixtures;

static DT_GearVRFixtures dt_gearvr;

/* One packet of tests_fixtures.json, a flat object of hex strings */
static bool DT_FixtureValue(const char *json, const char *key, Uint8 *out)
{
    char pattern[64];
    const char *value, *end;
    char hex[2 * SDL_GEARVR_PACKET_SIZE + 1];

    (void)SDL_snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    value = SDL_strstr(json, pattern);
    if (!value) {
        return false;
    }
    value = SDL_strchr(value + SDL_strlen(pattern), '"');
    if (!value) {
        return false;
    }
    ++value;
    end = SDL_strchr(value, '"');
    if (!end || end - value != 2 * SDL_GEARVR_PACKET_SIZE) {
        return false;
    }
    SDL_memcpy(hex, value, 2 * SDL_GEARVR_PACKET_SIZE);
    hex[2 * SDL_GEARVR_PACKET_SIZE] = '\0';
    return DT_Hex(hex, out, SDL_GEARVR_PACKET_SIZE) == SDL_GEARVR_PACKET_SIZE;
}

static void DT_LoadGearVRFixtures(const char *folder)
{
    char path[1024];
    size_t size = 0;
    char *json;

    if (!folder) {
        return;
    }
    (void)SDL_snprintf(path, sizeof(path), "%s/tests_fixtures.json", folder);
    json = (char *)SDL_LoadFile(path, &size);
    if (!json) {
        return;
    }
    dt_gearvr.loaded = DT_FixtureValue(json, "idle", dt_gearvr.idle) &&
                       DT_FixtureValue(json, "trigger", dt_gearvr.trigger) &&
                       DT_FixtureValue(json, "touch_center", dt_gearvr.touch_center);
    SDL_free(json);
}

static Uint32 DT_Uint32(const Uint8 *data)
{
    return (Uint32)data[0] | ((Uint32)data[1] << 8) | ((Uint32)data[2] << 16) | ((Uint32)data[3] << 24);
}

static float DT_Int16(const Uint8 *data)
{
    return (float)(Sint16)(Uint16)(data[0] | (data[1] << 8));
}

/* Scenario (a): the Poke Ball Plus from its advertisement to the gamepad
   API */
static void ScenarioPokeBall(void)
{
    static const Uint64 address = 0xA4C138F1E2D3;
    static const DT_Step startup[] = {
        DT_OPEN, DT_DISCOVER, DT_SUB(SDL_POKEBALL_INPUT, SDL_BLE_CCCD_NOTIFY),
        DT_SUB(SDL_POKEBALL_BATTERY, SDL_BLE_CCCD_NOTIFY), DT_READ(SDL_POKEBALL_BATTERY)
    };
    static const char *const mapping[] = { "a:b0", "b:b1", "leftx:a0", "lefty:a1" };
    /* X 112 and Y 108, the centers */
    static const Uint8 rest[SDL_POKEBALL_REPORT_SIZE] = { 0x00, 0x00, 0x00, 0x07, 0x6C };
    Uint8 report[SDL_POKEBALL_REPORT_SIZE];
    SDL_BLEAdvertisement ad;
    SDL_Gamepad *gamepad = NULL;
    SDL_JoystickID id;
    DT_Queues before, after;
    const char *text;
    int link;

    DT_Begin("a", "Poke Ball Plus");
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, address, SDL_BLE_AD_ADVERTISEMENT, "Pokemon PBP");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_READ, -1, 1, DT_WAIT_MS), "no battery read followed the advertisement")) {
        goto done;
    }
    link = DT_Link(address);
    DT_CheckSteps(__LINE__, link, startup, DT_COUNT(startup), true);
    DT_PumpFor(50);
    DT_CHECK(DT_Joysticks(NULL) == 0, "the joystick appeared before any input");

    id = DT_StreamUntilJoystick(link, SDL_POKEBALL_INPUT, rest, sizeof(rest));
    if (!DT_CHECK(id != 0, "no joystick after the first report")) {
        goto done;
    }
    DT_CheckIdentity(__LINE__, id, "Poke Ball Plus", SDL_JOYSTICK_TYPE_GAMEPAD, true);
    DT_CheckGUID(__LINE__, id, 0, 0, "PBP00 Poke Ball Plus", SDL_JOYSTICK_TYPE_GAMEPAD);
    text = SDL_GetJoystickPathForID(id);
    DT_CHECK(text && SDL_strcmp(text, "A4:C1:38:F1:E2:D3") == 0, "path \"%s\"", text ? text : "(null)");

    gamepad = SDL_OpenGamepad(id);
    if (!DT_CHECK(gamepad != NULL, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        goto done;
    }
    DT_CheckMapping(__LINE__, gamepad, mapping, DT_COUNT(mapping));
    text = SDL_GetGamepadSerial(gamepad);
    DT_CHECK(text && SDL_strcmp(text, "a4c138f1e2d3") == 0, "serial \"%s\"", text ? text : "(null)");
    DT_CheckPower(__LINE__, gamepad, SDL_POWERSTATE_ON_BATTERY, DT_BATTERY, DT_WAIT_MS);

    /* Both buttons, X 192 and Y 36, the ends of the Windows tool's ranges */
    SDL_memcpy(report, rest, sizeof(report));
    report[1] = 0x03;
    report[2] = 0x00;
    report[3] = 0x0C;
    report[4] = 0x24;
    DT_CHECK(DT_Push(link, SDL_POKEBALL_INPUT, report, sizeof(report)), "the input characteristic has no callback");
    DT_CHECK(DT_WaitButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH, true, DT_WAIT_MS), "the stick press did not reach South");
    DT_CHECK(SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST), "the top button did not reach East");
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX, 32767, DT_WAIT_MS), "left X %d", SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX));
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY, -32767, DT_WAIT_MS), "left Y %d", SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));

    DT_CHECK(DT_Push(link, SDL_POKEBALL_INPUT, rest, sizeof(rest)), "the input characteristic has no callback");
    DT_CHECK(DT_WaitButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH, false, DT_WAIT_MS), "South stayed down at rest");
    DT_CHECK(!SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST), "East stayed down at rest");
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX, 0, DT_WAIT_MS), "left X %d at rest", SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX));

    /* An empty value, which DT_Push hands over as a zero-byte block, is
       counted and goes no further */
    before = DT_GetQueues(address);
    DT_CHECK(DT_Push(link, SDL_POKEBALL_INPUT, NULL, 0), "the input characteristic has no callback");
    after = DT_GetQueues(address);
    DT_CHECK(after.found && after.empty == before.empty + 1 && after.sequence == before.sequence,
             "an empty value was counted %u times and queued %d changes and samples", after.empty - before.empty,
             (int)(after.sequence - before.sequence));
    DT_CheckSteps(__LINE__, link, startup, DT_COUNT(startup), true);

done:
    if (gamepad) {
        SDL_CloseGamepad(gamepad);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (b): the Daydream pairs first with the pairing hint on, is never
   published with it off, and skips pairing with a bond. Then its touchpad,
   sensors and battery. */
static void ScenarioDaydream(void)
{
    static const DT_Step paired[] = {
        DT_OPEN, DT_PAIR, DT_DISCOVER,
        DT_SUB(SDL_DAYDREAM_POSE, SDL_BLE_CCCD_NONE), DT_SUB(SDL_DAYDREAM_POSE, SDL_BLE_CCCD_NOTIFY),
        DT_SUB(SDL_DAYDREAM_BATTERY, SDL_BLE_CCCD_NONE), DT_SUB(SDL_DAYDREAM_BATTERY, SDL_BLE_CCCD_NOTIFY),
        DT_READ(SDL_DAYDREAM_BATTERY)
    };
    static const DT_Step bonded[] = {
        DT_OPEN, DT_DISCOVER,
        DT_SUB(SDL_DAYDREAM_POSE, SDL_BLE_CCCD_NONE), DT_SUB(SDL_DAYDREAM_POSE, SDL_BLE_CCCD_NOTIFY),
        DT_SUB(SDL_DAYDREAM_BATTERY, SDL_BLE_CCCD_NONE), DT_SUB(SDL_DAYDREAM_BATTERY, SDL_BLE_CCCD_NOTIFY),
        DT_READ(SDL_DAYDREAM_BATTERY)
    };
    static const DT_Step refused[] = { DT_OPEN, DT_CLOSE };
    static const char *const mapping[] = {
        "a:b0", "guide:b5", "start:b6", "leftshoulder:b9", "rightshoulder:b10", "leftx:a0", "lefty:a1"
    };
    const char *name = "Google Daydream Controller";
    Uint8 flat[20], touch[20], rotating[20], volume[20];
    SDL_BLEAdvertisement ad;
    SDL_Gamepad *gamepad = NULL;
    SDL_JoystickID id;
    FakeConfig config;
    float data[3], pressure = 0.0f;
    int link;

    DT_Begin("b", "Daydream pairing, touchpad, sensors and battery");
    (void)DT_Hex(dt_daydream_flat, flat, sizeof(flat));
    (void)DT_Hex(dt_daydream_touch, touch, sizeof(touch));
    (void)DT_Hex(dt_daydream_rotating, rotating, sizeof(rotating));
    (void)DT_Hex(dt_daydream_volume, volume, sizeof(volume));

    /* The pairing hint on and no Windows bond: pair, then discover */
    DT_GetConfig(&config);
    config.battery = 77;
    DT_SetConfig(&config);
    if (!DT_StartListening("1")) {
        goto done;
    }
    DT_Ad(&ad, 0xD0B5C27A4E11, SDL_BLE_AD_ADVERTISEMENT, "Daydream controller");
    DT_AdService(&ad, "0000fe55-0000-1000-8000-00805f9b34fb");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_READ, -1, 1, DT_WAIT_MS), "no battery read followed the advertisement")) {
        goto done;
    }
    link = DT_Link(0xD0B5C27A4E11);
    DT_CheckSteps(__LINE__, link, paired, DT_COUNT(paired), true);

    id = DT_StreamUntilJoystick(link, SDL_DAYDREAM_POSE, volume, sizeof(volume));
    if (!DT_CHECK(id != 0, "no joystick after the first record")) {
        goto done;
    }
    DT_CheckIdentity(__LINE__, id, name, SDL_JOYSTICK_TYPE_GAMEPAD, true);
    DT_CheckGUID(__LINE__, id, SDL_DAYDREAM_VENDOR, SDL_DAYDREAM_PRODUCT, "DAY00 Google Daydream Controller", SDL_JOYSTICK_TYPE_GAMEPAD);
    gamepad = SDL_OpenGamepad(id);
    if (!DT_CHECK(gamepad != NULL, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        goto done;
    }
    DT_CheckMapping(__LINE__, gamepad, mapping, DT_COUNT(mapping));
    DT_CHECK(DT_WaitButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true, DT_WAIT_MS), "volume up did not reach the right shoulder");
    DT_CheckPower(__LINE__, gamepad, SDL_POWERSTATE_ON_BATTERY, 77, DT_WAIT_MS);

    /* The touchpad, and the left stick it moves */
    DT_CHECK(SDL_GetNumGamepadTouchpads(gamepad) == 1 && SDL_GetNumGamepadTouchpadFingers(gamepad, 0) == 1,
             "%d touchpads, %d fingers", SDL_GetNumGamepadTouchpads(gamepad), SDL_GetNumGamepadTouchpadFingers(gamepad, 0));
    DT_CHECK(DT_Push(link, SDL_DAYDREAM_POSE, touch, sizeof(touch)), "the pose characteristic has no callback");
    DT_CHECK(DT_WaitFinger(gamepad, true, 136.0f / 255.0f, 75.0f / 255.0f, &pressure, DT_WAIT_MS), "no finger down at (136, 75)");
    DT_CHECK(pressure == 1.0f, "finger pressure %f", pressure);
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX, 2064, DT_WAIT_MS), "left X %d", SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX));
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY, -13674, DT_WAIT_MS), "left Y %d", SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
    DT_CHECK(!SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), "the right shoulder stayed down");

    /* The sensors, once enabled */
    DT_CHECK(SDL_GamepadHasSensor(gamepad, SDL_SENSOR_ACCEL) && SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO),
             "the accelerometer or the gyroscope is missing");
    DT_CHECK(SDL_GetGamepadSensorDataRate(gamepad, SDL_SENSOR_ACCEL) == 40.0f && SDL_GetGamepadSensorDataRate(gamepad, SDL_SENSOR_GYRO) == 40.0f,
             "sensor rates %f and %f", SDL_GetGamepadSensorDataRate(gamepad, SDL_SENSOR_ACCEL),
             SDL_GetGamepadSensorDataRate(gamepad, SDL_SENSOR_GYRO));
    DT_CHECK(SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_ACCEL, true) && SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, true),
             "the sensors could not be enabled: %s", SDL_GetError());
    DT_PumpFor(20);
    DT_CHECK(DT_Push(link, SDL_DAYDREAM_POSE, volume, sizeof(volume)), "the pose characteristic has no callback");
    DT_CHECK(DT_WaitSensor(gamepad, SDL_SENSOR_ACCEL, dt_daydream_accel, data, DT_WAIT_MS), "accelerometer (%f, %f, %f)",
             data[0], data[1], data[2]);
    DT_CHECK(DT_WaitFinger(gamepad, false, 0.0f, 0.0f, &pressure, DT_WAIT_MS), "the finger stayed down");
    DT_CHECK(DT_Push(link, SDL_DAYDREAM_POSE, rotating, sizeof(rotating)), "the pose characteristic has no callback");
    DT_CHECK(DT_WaitSensor(gamepad, SDL_SENSOR_GYRO, dt_daydream_gyro, data, DT_WAIT_MS), "gyroscope (%f, %f, %f)",
             data[0], data[1], data[2]);
    SDL_CloseGamepad(gamepad);
    gamepad = NULL;
    DT_Stop();

    /* The pairing hint off and no bond: never published, then backing off */
    Fake_Reset();
    if (!DT_StartListening("0")) {
        goto done;
    }
    DT_Ad(&ad, 0xD0B5C27A4E12, SDL_BLE_AD_ADVERTISEMENT, "Daydream controller");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (DT_CHECK(DT_WaitCalls(FAKE_CLOSE, -1, 1, DT_WAIT_MS), "the unbonded Daydream stayed connected with pairing off")) {
        link = DT_Link(0xD0B5C27A4E12);
        DT_CheckSteps(__LINE__, link, refused, DT_COUNT(refused), true);
    }
    /* The session has ended by the second advertisement, so only the
       backoff holds the address then */
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    DT_PumpFor(DT_QUIET_MS);
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    DT_PumpFor(DT_QUIET_MS);
    DT_CHECK(DT_Count(FAKE_OPEN, -1) == 1, "%d connects, expected the backoff to hold the address", DT_Count(FAKE_OPEN, -1));
    DT_CHECK(DT_Joysticks(NULL) == 0, "an unbonded Daydream was published with pairing off");
    DT_Stop();

    /* A Windows bond: no pairing, and each subscription writes None first */
    Fake_Reset();
    DT_GetConfig(&config);
    config.bonded = true;
    DT_SetConfig(&config);
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    /* The name matches. FE55 only corroborates, since other Google devices
       advertise it. */
    DT_Ad(&ad, 0xD0B5C27A4E13, SDL_BLE_AD_SCAN_RESPONSE, "Daydream controller");
    DT_AdService(&ad, "0000fe55-0000-1000-8000-00805f9b34fb");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (DT_CHECK(DT_WaitCalls(FAKE_READ, -1, 1, DT_WAIT_MS), "no battery read followed the advertisement")) {
        link = DT_Link(0xD0B5C27A4E13);
        DT_CheckSteps(__LINE__, link, bonded, DT_COUNT(bonded), true);
        DT_CHECK(DT_StreamUntilJoystick(link, SDL_DAYDREAM_POSE, flat, sizeof(flat)) != 0, "no joystick for the bonded Daydream");
    }

done:
    if (gamepad) {
        SDL_CloseGamepad(gamepad);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (c): the Gear VR start-up, a recorded packet, its trigger and
   sensors, and the closing write when its family hint turns off */
static void ScenarioGearVR(void)
{
    static const Uint64 address = 0x2C6B7D1E9F40;
    static const Uint8 vr_mode[2] = { 0x08, 0x00 };
    static const Uint8 sensor[2] = { 0x01, 0x00 };
    static const Uint8 off[2] = { 0x00, 0x00 };
    static const DT_Step startup[] = {
        DT_OPEN, DT_DISCOVER, DT_SUB(SDL_GEARVR_DATA, SDL_BLE_CCCD_NOTIFY), DT_WRITE(SDL_GEARVR_COMMAND)
    };
    static const char *const mapping[] = {
        "a:b0", "back:b4", "guide:b5", "leftshoulder:b9", "rightshoulder:b10", "leftx:a0", "lefty:a1", "righttrigger:a5"
    };
    SDL_Event events[64];
    Uint64 stamps[3], removed_ns;
    Uint32 times[3];
    float expected[3], data[3], pressure = 0.0f;
    SDL_BLEAdvertisement ad;
    SDL_Gamepad *gamepad = NULL;
    SDL_JoystickID id;
    FakeCall write, close;
    int link, writes, nevents, naccel, i, write_position, close_position;

    DT_Begin("c", "Gear VR start-up, trigger, sensors and closing write");
    if (!DT_CHECK(dt_gearvr.loaded, "tests_fixtures.json of the gearvr-controller clone could not be read")) {
        goto done;
    }
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, address, SDL_BLE_AD_ADVERTISEMENT, "Gear VR Controller(9F40)");
    DT_AdService(&ad, "4f63756c-7573-2054-6872-65656d6f7465");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_WRITE, -1, 1, DT_WAIT_MS), "no start-up write followed the advertisement")) {
        goto done;
    }
    link = DT_Link(address);
    DT_CheckWrite(__LINE__, link, 0, SDL_GEARVR_COMMAND, vr_mode, sizeof(vr_mode), true);
    DT_PumpFor(DT_QUIET_MS);
    DT_CheckSteps(__LINE__, link, startup, DT_COUNT(startup), true);

    /* The echo of 08 00 moves the start-up on to 01 00 */
    DT_CHECK(DT_Push(link, SDL_GEARVR_DATA, vr_mode, sizeof(vr_mode)), "the data characteristic has no callback");
    DT_CHECK(DT_WaitCalls(FAKE_WRITE, link, 2, DT_WAIT_MS), "01 00 did not follow the echo");
    DT_CheckWrite(__LINE__, link, 1, SDL_GEARVR_COMMAND, sensor, sizeof(sensor), true);

    id = DT_StreamUntilJoystick(link, SDL_GEARVR_DATA, dt_gearvr.idle, sizeof(dt_gearvr.idle));
    if (!DT_CHECK(id != 0, "no joystick after the idle packet")) {
        goto done;
    }
    /* The stream goes on, so the module's silence timeout never restarts
       the start-up while the scenario waits */
    DT_KeepAlive(link, SDL_GEARVR_DATA, dt_gearvr.idle, sizeof(dt_gearvr.idle));
    DT_CheckIdentity(__LINE__, id, "Samsung Gear VR Controller", SDL_JOYSTICK_TYPE_GAMEPAD, true);
    DT_CheckGUID(__LINE__, id, 0, 0, "GVR00 Samsung Gear VR Controller", SDL_JOYSTICK_TYPE_GAMEPAD);
    gamepad = SDL_OpenGamepad(id);
    if (!DT_CHECK(gamepad != NULL, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        goto done;
    }
    DT_CheckMapping(__LINE__, gamepad, mapping, DT_COUNT(mapping));
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0, DT_WAIT_MS), "the right trigger reads %d at rest",
             SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    DT_CHECK(SDL_GetJoystickAxis(SDL_GetGamepadJoystick(gamepad), SDL_BLE_AXIS_RIGHT_TRIGGER) == -32768,
             "the trigger's joystick axis rests at %d", SDL_GetJoystickAxis(SDL_GetGamepadJoystick(gamepad), SDL_BLE_AXIS_RIGHT_TRIGGER));
    DT_CheckPower(__LINE__, gamepad, SDL_POWERSTATE_ON_BATTERY, 100, DT_WAIT_MS);

    /* The trigger packet: the trigger pressed, and three samples */
    DT_CHECK(SDL_GetGamepadSensorDataRate(gamepad, SDL_SENSOR_ACCEL) == 206.0f, "accelerometer rate %f",
             SDL_GetGamepadSensorDataRate(gamepad, SDL_SENSOR_ACCEL));
    DT_CHECK(SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_ACCEL, true), "the accelerometer could not be enabled: %s", SDL_GetError());
    /* Only the trigger packet goes out until its samples are counted, and
       the pause lets any earlier one arrive before the flush */
    DT_StopKeepAlive();
    DT_PumpFor(50);
    SDL_FlushEvents(SDL_EVENT_GAMEPAD_SENSOR_UPDATE, SDL_EVENT_GAMEPAD_SENSOR_UPDATE);
    DT_CHECK(DT_Push(link, SDL_GEARVR_DATA, dt_gearvr.trigger, sizeof(dt_gearvr.trigger)), "the data characteristic has no callback");
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767, DT_WAIT_MS), "the right trigger reads %d pressed",
             SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    DT_PumpFor(50);
    nevents = SDL_PeepEvents(events, DT_COUNT(events), SDL_GETEVENT, SDL_EVENT_GAMEPAD_SENSOR_UPDATE, SDL_EVENT_GAMEPAD_SENSOR_UPDATE);
    naccel = 0;
    for (i = 0; i < nevents; ++i) {
        if (events[i].gsensor.sensor == SDL_SENSOR_ACCEL && events[i].gsensor.which == id) {
            if (naccel < 3) {
                stamps[naccel] = events[i].gsensor.sensor_timestamp;
            }
            ++naccel;
        }
    }
    DT_CHECK(naccel == 3, "%d accelerometer samples from one packet, expected 3", naccel);
    if (naccel == 3) {
        /* The sensor time is the device's microsecond counter accumulated
           across its wraps, times 1000. The idle packet can have gone out
           more than once, so only the low 32 bits of the microseconds equal
           the counter. */
        for (i = 0; i < 3; ++i) {
            times[i] = DT_Uint32(&dt_gearvr.trigger[16 * i]);
            DT_CHECK(stamps[i] % 1000 == 0 && (Uint32)(stamps[i] / 1000) == times[i],
                     "sample %d has sensor time %" SDL_PRIu64 " ns, whose microseconds do not end in the counter %" SDL_PRIu32,
                     i + 1, stamps[i], times[i]);
        }
        DT_CHECK(stamps[1] - stamps[0] == (Uint64)(times[1] - times[0]) * 1000 &&
                     stamps[2] - stamps[1] == (Uint64)(times[2] - times[1]) * 1000,
                 "sample times %" SDL_PRIu64 ", %" SDL_PRIu64 " and %" SDL_PRIu64 " ns are not the counter's steps",
                 stamps[0], stamps[1], stamps[2]);
    }
    DT_KeepAlive(link, SDL_GEARVR_DATA, dt_gearvr.trigger, sizeof(dt_gearvr.trigger));
    /* Sample 3: SDL X is device X, SDL Y device Z and SDL Z minus device Y */
    expected[0] = DT_Int16(&dt_gearvr.trigger[36]) * 9.80665f / 2048.0f;
    expected[1] = DT_Int16(&dt_gearvr.trigger[40]) * 9.80665f / 2048.0f;
    expected[2] = -DT_Int16(&dt_gearvr.trigger[38]) * 9.80665f / 2048.0f;
    DT_CHECK(DT_WaitSensor(gamepad, SDL_SENSOR_ACCEL, expected, data, DT_WAIT_MS), "accelerometer (%f, %f, %f), expected (%f, %f, %f)",
             data[0], data[1], data[2], expected[0], expected[1], expected[2]);

    /* The touchpad at its center, and the trigger released */
    DT_CHECK(DT_Push(link, SDL_GEARVR_DATA, dt_gearvr.touch_center, sizeof(dt_gearvr.touch_center)), "the data characteristic has no callback");
    DT_KeepAlive(link, SDL_GEARVR_DATA, dt_gearvr.touch_center, sizeof(dt_gearvr.touch_center));
    DT_CHECK(DT_WaitFinger(gamepad, true, 140.0f / 320.0f, 179.0f / 320.0f, &pressure, DT_WAIT_MS), "no finger down at (140, 179)");
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX, -4095, DT_WAIT_MS), "left X %d", SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX));
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY, 3891, DT_WAIT_MS), "left Y %d", SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0, DT_WAIT_MS), "the right trigger reads %d released",
             SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));

    /* The family hint off: the joystick goes at once, then 00 00 goes out,
       then the link closes */
    writes = DT_Count(FAKE_WRITE, link);
    SDL_FlushEvent(SDL_EVENT_JOYSTICK_REMOVED);
    DT_SetHint("SDL_JOYSTICK_BLE_GEARVR", "0");
    DT_CHECK(DT_WaitNoJoystick(DT_WAIT_MS), "the Gear VR joystick stayed after its hint turned off");
    removed_ns = DT_RemovedTime(id);
    DT_CHECK(removed_ns != 0, "no SDL_EVENT_JOYSTICK_REMOVED for the Gear VR joystick");
    if (DT_CHECK(DT_WaitCalls(FAKE_WRITE, link, writes + 1, DT_WAIT_MS), "no closing write after the hint turned off")) {
        DT_CheckWrite(__LINE__, link, writes, SDL_GEARVR_COMMAND, off, sizeof(off), true);
    }
    if (DT_CHECK(DT_WaitCalls(FAKE_CLOSE, link, 1, DT_WAIT_MS), "the Gear VR link stayed open")) {
        DT_CHECK(DT_Count(FAKE_WRITE, link) == writes + 1, "%d writes after the hint turned off, expected 00 00 alone",
                 DT_Count(FAKE_WRITE, link) - writes);
        write_position = DT_Position(FAKE_WRITE, link, writes, &write);
        close_position = DT_Position(FAKE_CLOSE, link, 0, &close);
        if (write_position >= 0 && removed_ns != 0) {
            DT_CHECK(removed_ns <= write.time_ns, "00 00 went out %" SDL_PRIu64 " ns before the removal event",
                     removed_ns - write.time_ns);
            DT_CHECK(write_position < close_position && write.time_ns <= close.time_ns, "the link closed before 00 00");
        }
    }
    DT_CHECK(DT_ListenerPresent(), "the listener went although six families stay on");

done:
    DT_StopKeepAlive();
    if (gamepad) {
        SDL_CloseGamepad(gamepad);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (d): the Oculus Go pairs when its first subscription fails for
   want of encryption, with the pairing hint on, and backs off with it off */
static void ScenarioOculusGo(void)
{
    static const DT_Step paired[] = {
        DT_OPEN, DT_DISCOVER, DT_SUB(SDL_OCULUSGO_FACE, SDL_BLE_CCCD_NOTIFY), DT_PAIR,
        DT_SUB(SDL_OCULUSGO_FACE, SDL_BLE_CCCD_NONE), DT_SUB(SDL_OCULUSGO_FACE, SDL_BLE_CCCD_NOTIFY),
        DT_SUB(SDL_OCULUSGO_BEEF, SDL_BLE_CCCD_NONE), DT_SUB(SDL_OCULUSGO_BEEF, SDL_BLE_CCCD_NOTIFY)
    };
    static const DT_Step refused[] = {
        DT_OPEN, DT_DISCOVER, DT_SUB(SDL_OCULUSGO_FACE, SDL_BLE_CCCD_NOTIFY), DT_CLOSE
    };
    static const char *const mapping[] = { "a:b0", "back:b4", "guide:b5", "leftx:a0", "lefty:a1", "righttrigger:a5" };
    Uint8 report[SDL_OCULUSGO_REPORT_SIZE];
    SDL_BLEAdvertisement ad;
    SDL_Gamepad *gamepad = NULL;
    SDL_JoystickID id;
    FakeConfig config;
    FakeCall call;
    int link;

    DT_Begin("d", "Oculus Go pairing on an encryption error");
    SDL_zeroa(report);
    report[19] = 0x01; /* The Oculus button */

    DT_GetConfig(&config);
    config.subscribe_fail = SDL_OCULUSGO_FACE;
    config.subscribe_failures = 1;
    config.subscribe_status = SDL_BLE_STATUS_PROTOCOL_ERROR;
    config.subscribe_error = SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION;
    DT_SetConfig(&config);
    if (!DT_StartListening("1")) {
        goto done;
    }
    DT_Ad(&ad, 0x2CD0AB16E7F8, SDL_BLE_AD_ADVERTISEMENT, "OMVR-V190");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_SUBSCRIBE, -1, 5, DT_WAIT_MS), "the subscriptions did not run again after the error")) {
        goto done;
    }
    link = DT_Link(0x2CD0AB16E7F8);
    DT_CheckSteps(__LINE__, link, paired, DT_COUNT(paired), false);
    if (DT_Position(FAKE_SUBSCRIBE, link, 0, &call) >= 0) {
        DT_CHECK(call.status == SDL_BLE_STATUS_PROTOCOL_ERROR, "the first subscription returned %d", call.status);
    }
    id = DT_StreamUntilJoystick(link, SDL_OCULUSGO_FACE, report, sizeof(report));
    if (!DT_CHECK(id != 0, "no joystick after pairing")) {
        goto done;
    }
    DT_CheckIdentity(__LINE__, id, "Oculus Go Controller", SDL_JOYSTICK_TYPE_GAMEPAD, true);
    DT_CheckGUID(__LINE__, id, 0, 0, "OGO00 Oculus Go Controller", SDL_JOYSTICK_TYPE_GAMEPAD);
    gamepad = SDL_OpenGamepad(id);
    if (!DT_CHECK(gamepad != NULL, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        goto done;
    }
    DT_CheckMapping(__LINE__, gamepad, mapping, DT_COUNT(mapping));
    DT_CHECK(DT_WaitButton(gamepad, SDL_GAMEPAD_BUTTON_GUIDE, true, DT_WAIT_MS), "the Oculus button did not reach Guide");
    DT_CHECK(DT_Count(FAKE_PAIR, link) == 1, "%d pairings", DT_Count(FAKE_PAIR, link));
    SDL_CloseGamepad(gamepad);
    gamepad = NULL;
    DT_Stop();

    /* With the pairing hint off the error ends the connection */
    Fake_Reset();
    DT_SetConfig(&config);
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, 0x2CD0AB16E7F9, SDL_BLE_AD_SCAN_RESPONSE, NULL);
    DT_AdService(&ad, "81265652-3692-ae93-e711-270f223c83b3");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (DT_CHECK(DT_WaitCalls(FAKE_CLOSE, -1, 1, DT_WAIT_MS), "the Oculus Go stayed connected with pairing off")) {
        link = DT_Link(0x2CD0AB16E7F9);
        DT_CheckSteps(__LINE__, link, refused, DT_COUNT(refused), true);
    }
    DT_PumpFor(DT_QUIET_MS);
    DT_CHECK(DT_Joysticks(NULL) == 0, "an Oculus Go was published without its subscription");

done:
    if (gamepad) {
        SDL_CloseGamepad(gamepad);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (e): the guitar's joystick type, its mapping with the D-pad as
   buttons 11 to 14, and a strum */
static void ScenarioGuitar(void)
{
    static const Uint64 address = 0xE8B1FC0A3D5C;
    static const char *const mapping[] = {
        "a:b0", "b:b1", "x:b2", "y:b3", "back:b4", "guide:b5", "start:b6", "leftstick:b7",
        "leftshoulder:b9", "rightshoulder:b10", "dpup:b11", "dpdown:b12", "dpleft:b13", "dpright:b14",
        "lefty:a1", "rightx:a2", "righty:a3"
    };
    /* Hat centered, strum and whammy at 80, tilt level */
    static const Uint8 rest[SDL_GHLIOS_REPORT_SIZE] = {
        0x00, 0x00, 0x0F, 0x00, 0x80, 0x00, 0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80
    };
    Uint8 report[SDL_GHLIOS_REPORT_SIZE];
    SDL_BLEAdvertisement ad;
    SDL_Gamepad *gamepad = NULL;
    SDL_JoystickID id;
    int link;

    DT_Begin("e", "Guitar Hero Live guitar type");
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, address, SDL_BLE_AD_ADVERTISEMENT, "Ble Guitar");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_SUBSCRIBE, -1, 1, DT_WAIT_MS), "no subscription followed the advertisement")) {
        goto done;
    }
    link = DT_Link(address);
    id = DT_StreamUntilJoystick(link, SDL_GHLIOS_INPUT, rest, sizeof(rest));
    if (!DT_CHECK(id != 0, "no joystick after the first report")) {
        goto done;
    }
    DT_CHECK(SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_GUITAR, "SDL_GetJoystickTypeForID is %d, expected the guitar's %d",
             (int)SDL_GetJoystickTypeForID(id), (int)SDL_JOYSTICK_TYPE_GUITAR);
    DT_CheckIdentity(__LINE__, id, "Guitar Hero Live Guitar (iOS)", SDL_JOYSTICK_TYPE_GUITAR, true);
    DT_CheckGUID(__LINE__, id, 0, 0, "GHL00 Guitar Hero Live Guitar (iOS)", SDL_JOYSTICK_TYPE_GUITAR);
    gamepad = SDL_OpenGamepad(id);
    if (!DT_CHECK(gamepad != NULL, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        goto done;
    }
    DT_CheckMapping(__LINE__, gamepad, mapping, DT_COUNT(mapping));
    SDL_memcpy(report, rest, sizeof(report));
    report[4] = 0xFF; /* Strum down */
    DT_CHECK(DT_Push(link, SDL_GHLIOS_INPUT, report, sizeof(report)), "the input characteristic has no callback");
    DT_CHECK(DT_WaitButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true, DT_WAIT_MS), "strum down did not reach the D-pad");
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY, 32767, DT_WAIT_MS), "left Y %d on strum down", SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));

done:
    if (gamepad) {
        SDL_CloseGamepad(gamepad);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (f): the right half of a Zwift Play on the plain handshake */
static void ScenarioZwift(void)
{
    static const Uint64 address = 0xF1D7329A4C60;
    static const Uint8 ride_on[6] = { 0x52, 0x69, 0x64, 0x65, 0x4F, 0x6E };
    static const Uint8 ride_on_play[8] = { 0x52, 0x69, 0x64, 0x65, 0x4F, 0x6E, 0x01, 0x04 };
    static const Uint8 type[3] = { SDL_ZWIFT_PLAY_RIGHT, 0x4C, 0x60 };
    static const DT_Step startup[] = {
        DT_OPEN, DT_DISCOVER, DT_SUB(SDL_ZWIFT_ASYNC, SDL_BLE_CCCD_NOTIFY), DT_SUB(SDL_ZWIFT_SYNC_TX, SDL_BLE_CCCD_INDICATE),
        DT_WRITE(SDL_ZWIFT_SYNC_RX)
    };
    static const char *const mapping[] = {
        "a:b0", "b:b1", "x:b2", "y:b3", "start:b6", "rightshoulder:b10", "rightx:a2", "righttrigger:a5"
    };
    Uint8 a[32], lever[32], pull[32];
    size_t a_length, lever_length, pull_length;
    SDL_BLEAdvertisement ad;
    SDL_Gamepad *gamepad = NULL;
    SDL_JoystickID id;
    int link;

    DT_Begin("f", "Zwift Play right half, plain handshake");
    a_length = DT_Hex(dt_zwift_a, a, sizeof(a));
    lever_length = DT_Hex(dt_zwift_lever, lever, sizeof(lever));
    pull_length = DT_Hex(dt_zwift_pull, pull, sizeof(pull));
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, address, SDL_BLE_AD_ADVERTISEMENT, "Zwift Play");
    DT_AdService(&ad, "00000001-19ca-4651-86e5-fa29dcdd09d1");
    DT_AdManufacturer(&ad, 0x094A, type, (Uint8)sizeof(type));
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_WRITE, -1, 1, DT_WAIT_MS), "no RideOn followed the advertisement")) {
        goto done;
    }
    link = DT_Link(address);
    DT_CheckSteps(__LINE__, link, startup, DT_COUNT(startup), true);
    DT_CheckWrite(__LINE__, link, 0, SDL_ZWIFT_SYNC_RX, ride_on, sizeof(ride_on), false);

    DT_CHECK(DT_Push(link, SDL_ZWIFT_SYNC_TX, ride_on_play, sizeof(ride_on_play)), "SYNC_TX has no callback");
    id = DT_StreamUntilJoystick(link, SDL_ZWIFT_ASYNC, a, a_length);
    if (!DT_CHECK(id != 0, "no joystick after the first key pad message")) {
        goto done;
    }
    DT_CheckIdentity(__LINE__, id, "Zwift Play (R)", SDL_JOYSTICK_TYPE_GAMEPAD, true);
    DT_CheckGUID(__LINE__, id, 0, 0, "ZWF02 Zwift Play (R)", SDL_JOYSTICK_TYPE_GAMEPAD);
    gamepad = SDL_OpenGamepad(id);
    if (!DT_CHECK(gamepad != NULL, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        goto done;
    }
    DT_CheckMapping(__LINE__, gamepad, mapping, DT_COUNT(mapping));
    DT_CHECK(DT_WaitButton(gamepad, SDL_GAMEPAD_BUTTON_EAST, true, DT_WAIT_MS), "A did not reach East");
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0, DT_WAIT_MS), "the right trigger reads %d at rest",
             SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    DT_CHECK(DT_Push(link, SDL_ZWIFT_ASYNC, pull, pull_length), "ASYNC has no callback");
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767, DT_WAIT_MS), "the right trigger reads %d on a full pull",
             SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    DT_CHECK(!SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST), "East stayed down");
    DT_CHECK(DT_Push(link, SDL_ZWIFT_ASYNC, lever, lever_length), "ASYNC has no callback");
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX, 31128, DT_WAIT_MS), "right X %d with the lever at 95",
             SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
    DT_CHECK(DT_Count(FAKE_WRITE, link) == 1, "%d writes, expected RideOn alone", DT_Count(FAKE_WRITE, link));

done:
    if (gamepad) {
        SDL_CloseGamepad(gamepad);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (g): the Myo as a plain joystick, and its closing writes when
   SDL quits */
static void ScenarioMyo(void)
{
    static const Uint64 address = 0xC8E2F47B1A09;
    static const Uint8 set_mode[5] = { 0x01, 0x03, 0x00, 0x03, 0x01 };
    static const Uint8 never_sleep[3] = { 0x09, 0x01, 0x01 };
    static const Uint8 unlock_hold[3] = { 0x0A, 0x01, 0x02 };
    static const Uint8 normal_sleep[3] = { 0x09, 0x01, 0x00 };
    static const Uint8 lock[3] = { 0x0A, 0x01, 0x00 };
    /* Orientation W 1, the accelerometer at 1 g on Y */
    static const Uint8 imu[SDL_MYO_IMU_SIZE] = { 0x00, 0x40, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x00, 0x08 };
    static const Uint8 fist[SDL_MYO_CLASSIFIER_SIZE] = { 0x03, 0x01, 0x00 };
    static const DT_Step startup[] = {
        DT_OPEN, DT_DISCOVER, DT_SUB(SDL_MYO_IMU, SDL_BLE_CCCD_NOTIFY), DT_SUB(SDL_MYO_CLASSIFIER, SDL_BLE_CCCD_INDICATE),
        DT_SUB(SDL_MYO_MOTION, SDL_BLE_CCCD_INDICATE), DT_SUB(SDL_MYO_BATTERY, SDL_BLE_CCCD_NOTIFY), DT_READ(SDL_MYO_BATTERY),
        DT_WRITE(SDL_MYO_COMMAND), DT_WRITE(SDL_MYO_COMMAND), DT_WRITE(SDL_MYO_COMMAND)
    };
    SDL_BLEAdvertisement ad;
    SDL_Joystick *joystick = NULL;
    SDL_JoystickID id;
    char *mapping;
    int link = -1, writes = 0;

    DT_Begin("g", "Myo as a plain joystick, closing writes on SDL_Quit");
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    /* The name is writable, so any name will do */
    DT_Ad(&ad, address, SDL_BLE_AD_SCAN_RESPONSE, "Lab Armband 7");
    DT_AdService(&ad, "d5060001-a904-deb9-4748-2c7f4a124842");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_WRITE, -1, 3, DT_WAIT_MS), "the three start-up writes did not follow the advertisement")) {
        goto done;
    }
    link = DT_Link(address);
    DT_CheckSteps(__LINE__, link, startup, DT_COUNT(startup), true);
    DT_CheckWrite(__LINE__, link, 0, SDL_MYO_COMMAND, set_mode, sizeof(set_mode), true);
    DT_CheckWrite(__LINE__, link, 1, SDL_MYO_COMMAND, never_sleep, sizeof(never_sleep), true);
    DT_CheckWrite(__LINE__, link, 2, SDL_MYO_COMMAND, unlock_hold, sizeof(unlock_hold), true);

    id = DT_StreamUntilJoystick(link, SDL_MYO_IMU, imu, sizeof(imu));
    if (!DT_CHECK(id != 0, "no joystick after the first IMU value")) {
        goto done;
    }
    DT_CheckIdentity(__LINE__, id, "Thalmic Myo Armband", SDL_JOYSTICK_TYPE_UNKNOWN, false);
    DT_CheckGUID(__LINE__, id, 0, 0, "MYO00 Thalmic Myo Armband", SDL_JOYSTICK_TYPE_UNKNOWN);
    mapping = SDL_GetGamepadMappingForID(id);
    DT_CHECK(mapping == NULL, "the Myo has the mapping %s", mapping ? mapping : "");
    SDL_free(mapping);
    joystick = SDL_OpenJoystick(id);
    if (!DT_CHECK(joystick != NULL, "SDL_OpenJoystick failed: %s", SDL_GetError())) {
        goto done;
    }
    DT_CHECK(SDL_GetNumJoystickButtons(joystick) == 5 && SDL_GetNumJoystickAxes(joystick) == 3 && SDL_GetNumJoystickHats(joystick) == 0,
             "%d buttons, %d axes, %d hats", SDL_GetNumJoystickButtons(joystick), SDL_GetNumJoystickAxes(joystick),
             SDL_GetNumJoystickHats(joystick));
    DT_CHECK(DT_Push(link, SDL_MYO_CLASSIFIER, fist, sizeof(fist)), "the classifier has no callback");
    DT_CHECK(DT_WaitJoystickButton(joystick, SDL_MYO_BUTTON_FIST, true, DT_WAIT_MS), "the fist did not reach button 0");
    writes = DT_Count(FAKE_WRITE, link);

done:
    if (joystick) {
        SDL_CloseJoystick(joystick);
    }
    DT_Stop();
    if (link >= 0 && writes > 0) {
        DT_CheckWrite(__LINE__, link, writes, SDL_MYO_COMMAND, normal_sleep, sizeof(normal_sleep), true);
        DT_CheckWrite(__LINE__, link, writes + 1, SDL_MYO_COMMAND, lock, sizeof(lock), true);
        DT_CHECK(DT_Count(FAKE_WRITE, link) == writes + 2, "%d writes on SDL_Quit, expected 2", DT_Count(FAKE_WRITE, link) - writes);
        DT_CHECK(DT_Position(FAKE_CLOSE, link, 0, NULL) >= 0 &&
                     DT_Position(FAKE_WRITE, link, writes + 1, NULL) < DT_Position(FAKE_CLOSE, link, 0, NULL),
                 "the link closed before the closing writes, or never");
    }
    DT_End();
}

/* Scenario (h): link loss removes the joystick, and the next advertisement
   brings it back with a new instance ID and the same serial */
static void ScenarioLinkLoss(void)
{
    static const Uint64 address = 0x5EA107C3B299;
    static const DT_Step startup[] = {
        DT_OPEN, DT_DISCOVER, DT_SUB(SDL_POKEBALL_INPUT, SDL_BLE_CCCD_NOTIFY),
        DT_SUB(SDL_POKEBALL_BATTERY, SDL_BLE_CCCD_NOTIFY), DT_READ(SDL_POKEBALL_BATTERY)
    };
    static const Uint8 rest[SDL_POKEBALL_REPORT_SIZE] = { 0x00, 0x00, 0x00, 0x07, 0x6C };
    Uint8 press[SDL_POKEBALL_REPORT_SIZE];
    char serial[32];
    const char *text;
    SDL_BLEAdvertisement ad;
    SDL_Gamepad *gamepad = NULL;
    SDL_JoystickID first, second;
    Uint64 start;
    int link, relink;

    DT_Begin("h", "link loss and reconnect");
    SDL_memcpy(press, rest, sizeof(press));
    press[1] = 0x02; /* The stick press */
    serial[0] = '\0';
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, address, SDL_BLE_AD_ADVERTISEMENT, "Pokemon PBP");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_READ, -1, 1, DT_WAIT_MS), "no battery read followed the advertisement")) {
        goto done;
    }
    link = DT_Link(address);
    first = DT_StreamUntilJoystick(link, SDL_POKEBALL_INPUT, rest, sizeof(rest));
    if (!DT_CHECK(first != 0, "no joystick after the first report")) {
        goto done;
    }
    gamepad = SDL_OpenGamepad(first);
    if (!DT_CHECK(gamepad != NULL, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        goto done;
    }
    text = SDL_GetGamepadSerial(gamepad);
    SDL_strlcpy(serial, text ? text : "", sizeof(serial));
    DT_CHECK(SDL_strcmp(serial, "5ea107c3b299") == 0, "serial \"%s\"", serial);
    DT_CHECK(DT_Push(link, SDL_POKEBALL_INPUT, press, sizeof(press)), "the input characteristic has no callback");
    DT_CHECK(DT_WaitButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH, true, DT_WAIT_MS), "the stick press did not reach South");

    /* The link drops */
    DT_CHECK(DT_LoseLink(link), "Open received no atomic for link loss");
    DT_CHECK(DT_WaitNoJoystick(DT_WAIT_MS), "the joystick stayed after the link dropped");
    DT_CHECK(!SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH), "South stayed down after the link dropped");
    DT_CHECK(DT_WaitCalls(FAKE_CLOSE, link, 1, DT_WAIT_MS), "the lost link was not closed");
    SDL_CloseGamepad(gamepad);
    gamepad = NULL;

    /* The device advertises again, as the watcher reports it repeatedly */
    start = SDL_GetTicks();
    while (DT_Count(FAKE_OPEN, -1) < 2 && !DT_Expired(start, DT_WAIT_MS)) {
        DT_Advertise(&ad);
        DT_PumpFor(20);
    }
    if (!DT_CHECK(DT_Count(FAKE_OPEN, -1) == 2, "%d connects, expected a reconnect", DT_Count(FAKE_OPEN, -1))) {
        goto done;
    }
    relink = DT_Link(address);
    DT_CHECK(relink != link, "the reconnect reused link %d", link);
    DT_CHECK(DT_WaitCalls(FAKE_READ, relink, 1, DT_WAIT_MS), "the start-up did not run again");
    DT_CheckSteps(__LINE__, relink, startup, DT_COUNT(startup), true);
    second = DT_StreamUntilJoystick(relink, SDL_POKEBALL_INPUT, rest, sizeof(rest));
    if (!DT_CHECK(second != 0, "no joystick after the reconnect")) {
        goto done;
    }
    DT_CHECK(second != first, "the reconnect kept instance ID %" SDL_PRIu32, first);
    gamepad = SDL_OpenGamepad(second);
    if (!DT_CHECK(gamepad != NULL, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        goto done;
    }
    text = SDL_GetGamepadSerial(gamepad);
    DT_CHECK(text && SDL_strcmp(text, serial) == 0, "serial \"%s\" after the reconnect, \"%s\" before", text ? text : "(null)", serial);

done:
    if (gamepad) {
        SDL_CloseGamepad(gamepad);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (i): SDL_Quit returns promptly while an Open blocks */
static void ScenarioQuitWhileOpening(void)
{
    SDL_BLEAdvertisement ad;
    FakeConfig config;
    Uint64 start;
    int elapsed, entered = 0, links;

    DT_Begin("i", "SDL_Quit while an Open blocks");
    DT_GetConfig(&config);
    config.open_blocks = true;
    DT_SetConfig(&config);
    if (DT_StartListening(NULL)) {
        DT_Ad(&ad, 0x7FA3B2C1D0E9, SDL_BLE_AD_ADVERTISEMENT, "Pokemon PBP");
        DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
        start = SDL_GetTicks();
        while (!DT_Expired(start, DT_WAIT_MS)) {
            SDL_LockMutex(fake.mutex);
            entered = fake.opens_entered;
            SDL_UnlockMutex(fake.mutex);
            if (entered) {
                break;
            }
            DT_Pump();
        }
        DT_CHECK(entered == 1, "%d Open calls began", entered);
        DT_PumpFor(50);
    }
    elapsed = DT_Stop();
    DT_CHECK(elapsed < DT_CANCEL_LIMIT_MS, "SDL_Quit took %d ms while an Open blocked", elapsed);
    SDL_LockMutex(fake.mutex);
    links = fake.nlinks;
    SDL_UnlockMutex(fake.mutex);
    DT_CHECK(links == 0, "a canceled Open left %d links", links);
    DT_End();
}

/* Scenario (j): the hints apply at any time. The transport's runtime starts
   in SDL_Init whatever the hints say, on the thread that runs SDL_Init, and
   quits in SDL_Quit on the same thread. With SDL_JOYSTICK_BLE unset the
   listener does not start. Turning the hint or one family's hint on later
   adds it at the next joystick update, from whatever thread runs that,
   turning every family off removes the listener and logs what it heard, as
   counts, and SDL_Quit leaves every start balanced. */
static void ScenarioHintOff(void)
{
    static const Uint8 samsung[2] = { 0x01, 0x00 };
    char line[256], expected[256];
    SDL_BLEAdvertisement ad;
    const char *value;
    size_t i;
    int lines;

    DT_Begin("j", "hints at any time, the listener and its counts");
    value = SDL_GetHint(DT_HINT_BLE);
    DT_CHECK(value == NULL, "SDL_JOYSTICK_BLE is \"%s\" before the test sets it, from the environment", value ? value : "");
    if (DT_Start(NULL, NULL)) {
        DT_CHECK(DT_Count(FAKE_INIT, -1) == 1, "SDL_BLEGATT_Init ran %d times in SDL_Init with SDL_JOYSTICK_BLE unset",
                 DT_Count(FAKE_INIT, -1));
        DT_PumpFor(DT_QUIET_MS);
        DT_CHECK(DT_Count(FAKE_ADD_LISTENER, -1) == 0, "the listener was added with SDL_JOYSTICK_BLE unset");

        /* On after SDL_Init, with SDL's joystick update on another thread:
           the listener starts on the transport SDL_Init started, and DT_Stop
           checks where that ran */
        DT_SetHint(DT_HINT_BLE, "1");
        DT_CHECK(DT_WaitListenerElsewhere(true, DT_WAIT_MS), "no listener after SDL_JOYSTICK_BLE turned on after SDL_Init");
        DT_CHECK(DT_Count(FAKE_INIT, -1) == 1, "SDL_BLEGATT_Init ran %d times", DT_Count(FAKE_INIT, -1));

        /* Two advertisements that match no family, a name and Samsung's
           company alone, then a Poke Ball that connects */
        DT_Ad(&ad, 0x0A1B2C3D4E5F, SDL_BLE_AD_ADVERTISEMENT, "Office Mouse");
        DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
        DT_Ad(&ad, 0x0A1B2C3D4E60, SDL_BLE_AD_ADVERTISEMENT, NULL);
        DT_AdManufacturer(&ad, 0x0075, samsung, (Uint8)sizeof(samsung));
        DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
        DT_Ad(&ad, 0x0A1B2C3D4E61, SDL_BLE_AD_ADVERTISEMENT, "Pokemon PBP");
        DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
        DT_CHECK(DT_WaitCalls(FAKE_READ, -1, 1, DT_WAIT_MS), "the Poke Ball did not connect");
        DT_CHECK(DT_Count(FAKE_OPEN, -1) == 1, "%d connects from one matching advertisement", DT_Count(FAKE_OPEN, -1));

        /* Off: the connection stops, the listener goes and the log counts
           the three advertisements and the one connection */
        lines = DT_WatchLog(NULL, 0);
        DT_SetHint(DT_HINT_BLE, "0");
        DT_CHECK(DT_WaitListener(false, DT_WAIT_MS), "the listener stayed after SDL_JOYSTICK_BLE turned off");
        DT_CHECK(DT_WaitCalls(FAKE_CLOSE, -1, 1, DT_WAIT_MS), "the Poke Ball stayed connected with its family off");
        SDL_strlcpy(expected, "BLE GATT: stopped watching advertisements after 3 advertisements, 1 connections", sizeof(expected));
        if (DT_CHECK(DT_WatchLog(line, sizeof(line)) == lines + 1, "no line when the driver stopped watching")) {
            DT_CHECK(SDL_strcmp(line, expected) == 0, "the driver logged \"%s\", expected \"%s\"", line, expected);
        }

        /* On again: the listener comes back on the running transport */
        DT_SetHint(DT_HINT_BLE, "1");
        DT_CHECK(DT_WaitListener(true, DT_WAIT_MS), "no listener after SDL_JOYSTICK_BLE turned on again");
        DT_CHECK(DT_Count(FAKE_INIT, -1) == 1, "SDL_BLEGATT_Init ran %d times", DT_Count(FAKE_INIT, -1));
        DT_CHECK(DT_Count(FAKE_ADD_LISTENER, -1) == 2, "the listener was added %d times", DT_Count(FAKE_ADD_LISTENER, -1));
    }
    /* SDL_Quit with the hint on: one Init, one Quit, two listeners removed */
    DT_Stop();

    /* One family's hint on after SDL_Init is enough, and the listener stays
       until every family is off */
    Fake_Reset();
    if (DT_Start(NULL, NULL)) {
        DT_SetHint(dt_family_hints[SDL_BLE_FAMILY_MYO], "1");
        DT_CHECK(DT_WaitListener(true, DT_WAIT_MS), "no listener after the Myo's hint turned on");
        DT_SetHint(DT_HINT_BLE, "1");
        DT_SetHint(dt_family_hints[SDL_BLE_FAMILY_MYO], "0");
        DT_PumpFor(DT_QUIET_MS);
        DT_CHECK(DT_ListenerPresent(), "the listener went although six families stay on");
        DT_CHECK(DT_Count(FAKE_ADD_LISTENER, -1) == 1, "the listener was added %d times", DT_Count(FAKE_ADD_LISTENER, -1));
        DT_SetHint(DT_HINT_BLE, "0");
        DT_CHECK(DT_WaitListener(false, DT_WAIT_MS), "the listener stayed with every family off");
    }
    DT_Stop();

    /* The driver on with every family off: the runtime alone, no listener */
    Fake_Reset();
    for (i = 0; i < SDL_arraysize(dt_family_hints); ++i) {
        DT_SetHint(dt_family_hints[i], "0");
    }
    if (DT_Start("1", NULL)) {
        DT_PumpFor(DT_QUIET_MS);
        DT_CHECK(DT_Count(FAKE_INIT, -1) == 1, "SDL_BLEGATT_Init ran %d times with every family off", DT_Count(FAKE_INIT, -1));
        DT_CHECK(DT_Count(FAKE_ADD_LISTENER, -1) == 0, "the listener was added with every family off");
    }
    DT_Stop();
    DT_End();
}

/* Connects a Zwift Play half of that type byte: the advertisement, the
   RideOn write, the reply and idle messages until count joysticks are
   present. Returns the new joystick, 0 on failure, and the link in *link. */
static SDL_JoystickID DT_ConnectZwiftHalf(Uint64 address, Uint8 type, const Uint8 *idle, size_t idle_length, int count,
                                          SDL_JoystickID other, int *link)
{
    static const Uint8 ride_on_play[8] = { 0x52, 0x69, 0x64, 0x65, 0x4F, 0x6E, 0x01, 0x04 };
    const Uint8 data[3] = { type, (Uint8)(address >> 8), (Uint8)address };
    const int writes = DT_Count(FAKE_WRITE, -1);
    SDL_BLEAdvertisement ad;

    *link = -1;
    DT_Ad(&ad, address, SDL_BLE_AD_ADVERTISEMENT, "Zwift Play");
    DT_AdService(&ad, "00000001-19ca-4651-86e5-fa29dcdd09d1");
    DT_AdManufacturer(&ad, 0x094A, data, (Uint8)sizeof(data));
    if (!DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement of type %02X", type) ||
        !DT_CHECK(DT_WaitCalls(FAKE_WRITE, -1, writes + 1, DT_WAIT_MS), "no RideOn followed the advertisement of type %02X", type)) {
        return 0;
    }
    *link = DT_Link(address);
    DT_CHECK(DT_Push(*link, SDL_ZWIFT_SYNC_TX, ride_on_play, sizeof(ride_on_play)), "SYNC_TX has no callback");
    return DT_StreamUntilJoysticks(*link, SDL_ZWIFT_ASYNC, idle, idle_length, count, other);
}

/* Scenario (k): both halves of a Zwift Play in one session. By name alone
   their GUIDs shared all but the CRC, and the half connected second took the
   first half's mapping and name. Each half keeps its own here, the right
   half's South reaches its gamepad, and a first trigger reading near the
   middle does not become the trigger's rest. */
static void ScenarioZwiftHalves(void)
{
    static const Uint64 left_address = 0xF1D7329A4C61;
    static const Uint64 right_address = 0xF1D7329A4C62;
    static const char left_idle[] = "07 08 01 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 00";
    static const char right_idle[] = "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 00";
    static const char right_south[] = "07 08 00 10 01 18 01 20 01 28 00 30 01 38 01 40 00 48 00";
    /* A pull of 50, zigzag 100: the trigger axis at -1, 16383 on the gamepad */
    static const char right_half_pull[] = "07 08 00 10 01 18 01 20 01 28 01 30 01 38 01 40 00 48 64";
    static const char *const left_mapping[] = {
        "back:b4", "leftshoulder:b9", "dpup:b11", "dpdown:b12", "dpleft:b13", "dpright:b14", "leftx:a0", "lefttrigger:a4"
    };
    static const char *const right_mapping[] = {
        "a:b0", "b:b1", "x:b2", "y:b3", "start:b6", "rightshoulder:b10", "rightx:a2", "righttrigger:a5"
    };
    Uint8 message[32];
    size_t length;
    SDL_Gamepad *left = NULL, *right = NULL;
    SDL_JoystickID left_id, right_id;
    SDL_GUID left_guid, right_guid;
    const char *name;
    Sint16 rest = 0;
    int left_link, right_link;

    DT_Begin("k", "both Zwift Play halves, their GUIDs, names and mappings, and a trigger's rest");
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    length = DT_Hex(left_idle, message, sizeof(message));
    left_id = DT_ConnectZwiftHalf(left_address, SDL_ZWIFT_PLAY_LEFT, message, length, 1, 0, &left_link);
    if (!DT_CHECK(left_id != 0, "no joystick for the left half")) {
        goto done;
    }
    length = DT_Hex(right_idle, message, sizeof(message));
    right_id = DT_ConnectZwiftHalf(right_address, SDL_ZWIFT_PLAY_RIGHT, message, length, 2, left_id, &right_link);
    if (!DT_CHECK(right_id != 0, "no second joystick for the right half")) {
        goto done;
    }
    DT_CheckIdentity(__LINE__, left_id, "Zwift Play (L)", SDL_JOYSTICK_TYPE_GAMEPAD, true);
    DT_CheckIdentity(__LINE__, right_id, "Zwift Play (R)", SDL_JOYSTICK_TYPE_GAMEPAD, true);
    DT_CheckGUID(__LINE__, left_id, 0, 0, "ZWF03 Zwift Play (L)", SDL_JOYSTICK_TYPE_GAMEPAD);
    DT_CheckGUID(__LINE__, right_id, 0, 0, "ZWF02 Zwift Play (R)", SDL_JOYSTICK_TYPE_GAMEPAD);
    left_guid = SDL_GetJoystickGUIDForID(left_id);
    right_guid = SDL_GetJoystickGUIDForID(right_id);
    SDL_SetJoystickGUIDCRC(&left_guid, 0);
    SDL_SetJoystickGUIDCRC(&right_guid, 0);
    DT_CHECK(SDL_memcmp(left_guid.data, right_guid.data, sizeof(left_guid.data)) != 0,
             "the halves share one GUID once the CRC is cleared");

    left = SDL_OpenGamepad(left_id);
    right = SDL_OpenGamepad(right_id);
    if (!DT_CHECK(left && right, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        goto done;
    }
    name = SDL_GetGamepadName(left);
    DT_CHECK(name && SDL_strcmp(name, "Zwift Play (L)") == 0, "the left gamepad is named \"%s\"", name ? name : "(null)");
    name = SDL_GetGamepadName(right);
    DT_CHECK(name && SDL_strcmp(name, "Zwift Play (R)") == 0, "the right gamepad is named \"%s\"", name ? name : "(null)");
    DT_CheckMapping(__LINE__, left, left_mapping, DT_COUNT(left_mapping));
    DT_CheckMapping(__LINE__, right, right_mapping, DT_COUNT(right_mapping));

    length = DT_Hex(right_south, message, sizeof(message));
    DT_CHECK(DT_Push(right_link, SDL_ZWIFT_ASYNC, message, length), "ASYNC has no callback");
    DT_CHECK(DT_WaitButton(right, SDL_GAMEPAD_BUTTON_SOUTH, true, DT_WAIT_MS), "the right half's South did not reach its gamepad");
    DT_CHECK(!SDL_GetGamepadButton(left, SDL_GAMEPAD_BUTTON_SOUTH), "the right half's South reached the left gamepad");

    /* The trigger's first reading off its rest is near the middle. The rest
       stays -32768, and the reading arrives whole. */
    length = DT_Hex(right_half_pull, message, sizeof(message));
    DT_CHECK(DT_Push(right_link, SDL_ZWIFT_ASYNC, message, length), "ASYNC has no callback");
    DT_CHECK(DT_WaitJoystickAxis(SDL_GetGamepadJoystick(right), SDL_BLE_AXIS_RIGHT_TRIGGER, -1, DT_WAIT_MS),
             "the trigger axis reads %d, expected -1", SDL_GetJoystickAxis(SDL_GetGamepadJoystick(right), SDL_BLE_AXIS_RIGHT_TRIGGER));
    DT_CHECK(SDL_GetJoystickAxisInitialState(SDL_GetGamepadJoystick(right), SDL_BLE_AXIS_RIGHT_TRIGGER, &rest) && rest == -32768,
             "the trigger's rest is %d, expected -32768", rest);
    DT_CHECK(SDL_GetGamepadAxis(right, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) == 16383, "the right trigger reads %d, expected 16383",
             SDL_GetGamepadAxis(right, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));

done:
    if (left) {
        SDL_CloseGamepad(left);
    }
    if (right) {
        SDL_CloseGamepad(right);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (l): every family and variant the tables allow gets its own GUID
   once the CRC is cleared, as SDL's mapping lookup compares them, and each
   GUID is the one its key names, the family's code, the variant as two hex
   digits and the name. A manufacturer key with types allows each type, one
   without types any first data byte, and every other key variant 0.
   Corroborating keys never match alone. */
static void ScenarioGUIDs(void)
{
    /* The codes, in the order of SDL_BLE_Families, written out here so a
       change to a shipped GUID fails the test */
    static const char *const codes[SDL_BLE_FAMILY_COUNT] = { "PBP", "DAY", "GVR", "OGO", "GHL", "ZWF", "MYO" };
    static SDL_GUID guids[SDL_BLE_FAMILY_COUNT * 256];
    static int owners[SDL_BLE_FAMILY_COUNT * 256];
    char text[33], other[33], key[80];
    int nguids = 0, family, variant, k, t, g;

    DT_Begin("l", "a distinct GUID for every family and variant");
    for (family = 0; family < SDL_BLE_FAMILY_COUNT; ++family) {
        const SDL_BLEFamily *table = SDL_BLE_Families[family];
        bool allowed[256];

        SDL_zeroa(allowed);
        for (k = 0; k < table->nkeys && k < SDL_BLE_MAX_KEYS; ++k) {
            const SDL_BLEMatchKey *match = &table->keys[k];

            if (match->corroborating) {
                continue;
            }
            if (match->kind != SDL_BLE_KEY_MANUFACTURER) {
                allowed[0] = true;
            } else if (match->ntypes == 0) {
                for (variant = 0; variant < 256; ++variant) {
                    allowed[variant] = true;
                }
            } else {
                for (t = 0; t < match->ntypes && t < (int)SDL_arraysize(match->types); ++t) {
                    allowed[match->types[t]] = true;
                }
            }
        }
        for (variant = 0; variant < 256; ++variant) {
            SDL_BLEModuleContext context;
            SDL_BLESink sink;
            SDL_GUID guid;
            void *state;

            if (!allowed[variant]) {
                continue;
            }
            state = SDL_calloc(1, table->module->state_size);
            if (!DT_CHECK(state != NULL, "no memory for the %s module", table->name)) {
                continue;
            }
            SDL_zero(context);
            SDL_zero(sink);
            context.variant = (Uint8)variant;
            table->module->Reset(state, &sink, &context);
            {
                const SDL_BLEIdentity *identity = &((const SDL_BLEBase *)state)->identity;
                SDL_GUID expected;

                guid = BLEGATT_CreateGUID(family, (Uint8)variant, identity);
                (void)SDL_snprintf(key, sizeof(key), "%s%02X %s", codes[family], (unsigned int)variant, identity->name);
                expected = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, identity->vendor, identity->product, 0, NULL, key,
                                                  'l', identity->type);
                SDL_GUIDToString(guid, text, sizeof(text));
                DT_CHECK(SDL_memcmp(guid.data, expected.data, sizeof(guid.data)) == 0, "%s variant %02X has GUID %s, not the one of \"%s\"",
                         table->name, variant, text, key);
                DT_CHECK(guid.data[14] == 'l' && guid.data[15] == identity->type, "%s variant %02X: signature 0x%02X and type %d",
                         table->name, variant, guid.data[14], guid.data[15]);
            }
            SDL_free(state);
            SDL_SetJoystickGUIDCRC(&guid, 0);
            for (g = 0; g < nguids; ++g) {
                if (SDL_memcmp(guid.data, guids[g].data, sizeof(guid.data)) == 0) {
                    SDL_GUIDToString(guid, text, sizeof(text));
                    SDL_GUIDToString(guids[g], other, sizeof(other));
                    DT_Check(false, __LINE__, "%s variant %02X has GUID %s, as %s variant %02X has %s", table->name, variant, text,
                             SDL_BLE_Families[owners[g] >> 8]->name, owners[g] & 0xFF, other);
                }
            }
            guids[nguids] = guid;
            owners[nguids] = (family << 8) | variant;
            ++nguids;
        }
    }
    /* Seven families, and the Zwift family's four types */
    DT_CHECK(nguids >= SDL_BLE_FAMILY_COUNT + 3, "%d family and variant pairs checked", nguids);
    DT_End();
}

/* Connects a Myo at the address and streams its IMU until its joystick
   appears. Returns the joystick, 0 on failure, and the link in *link. */
static SDL_JoystickID DT_ConnectMyo(Uint64 address, int *link)
{
    static const Uint8 imu[SDL_MYO_IMU_SIZE] = { 0x00, 0x40, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x00, 0x08 };
    const int writes = DT_Count(FAKE_WRITE, -1);
    SDL_BLEAdvertisement ad;

    *link = -1;
    DT_Ad(&ad, address, SDL_BLE_AD_SCAN_RESPONSE, "Lab Armband 7");
    DT_AdService(&ad, "d5060001-a904-deb9-4748-2c7f4a124842");
    if (!DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement") ||
        !DT_CHECK(DT_WaitCalls(FAKE_WRITE, -1, writes + 3, DT_WAIT_MS), "the three start-up writes did not follow the advertisement")) {
        return 0;
    }
    *link = DT_Link(address);
    return DT_StreamUntilJoystick(*link, SDL_MYO_IMU, imu, sizeof(imu));
}

/* Scenario (m): the Myo's closing writes. SDL_Quit on a link that dies
   without a report abandons them once the grace has passed, a hint change
   lets slow ones finish, and a lost link gets none even when SDL_Quit comes
   right after the loss. */
static void ScenarioClosingWrites(void)
{
    static const Uint8 normal_sleep[3] = { 0x09, 0x01, 0x00 };
    static const Uint8 lock[3] = { 0x0A, 0x01, 0x00 };
    const int slow_ms = BLEGATT_CLOSE_GRACE_MS + 500;
    FakeConfig config;
    FakeCall call;
    Uint64 start;
    int link = -1, writes = 0, elapsed, i;

    DT_Begin("m", "closing writes: SDL_Quit bounds them, a hint change waits for them, a lost link gets none");

    /* SDL_Quit while every write hangs, as on a link that died unreported */
    if (DT_StartListening(NULL)) {
        if (DT_CHECK(DT_ConnectMyo(0xC8E2F47B1A10, &link) != 0, "no Myo joystick")) {
            writes = DT_Count(FAKE_WRITE, link);
            DT_GetConfig(&config);
            config.write_blocks = true;
            DT_SetConfig(&config);
        }
    }
    elapsed = DT_Stop();
    if (writes > 0) {
        DT_CHECK(elapsed >= BLEGATT_CLOSE_GRACE_MS - DT_TICK_SLACK_MS && elapsed < DT_CANCEL_LIMIT_MS,
                 "SDL_Quit took %d ms while the closing writes hung, expected the %d ms grace and little more", elapsed,
                 BLEGATT_CLOSE_GRACE_MS);
        DT_CHECK(DT_Count(FAKE_WRITE, link) == writes + 2, "%d closing writes, expected 2", DT_Count(FAKE_WRITE, link) - writes);
        for (i = 0; i < 2; ++i) {
            if (DT_Position(FAKE_WRITE, link, writes + i, &call) >= 0) {
                DT_CHECK(!call.result, "closing write %d was delivered to the dead link", i);
            }
        }
    }

    /* A hint change: the closing writes take longer than the grace and
       still finish */
    Fake_Reset();
    writes = 0;
    if (DT_StartListening(NULL)) {
        if (DT_CHECK(DT_ConnectMyo(0xC8E2F47B1A11, &link) != 0, "no Myo joystick")) {
            writes = DT_Count(FAKE_WRITE, link);
            DT_GetConfig(&config);
            config.write_delay_ms = slow_ms;
            DT_SetConfig(&config);
            start = SDL_GetTicks();
            DT_SetHint(dt_family_hints[SDL_BLE_FAMILY_MYO], "0");
            if (DT_CHECK(DT_WaitCalls(FAKE_CLOSE, link, 1, 2 * slow_ms + DT_WAIT_MS), "the Myo link stayed open after its hint turned off")) {
                DT_CheckWrite(__LINE__, link, writes, SDL_MYO_COMMAND, normal_sleep, sizeof(normal_sleep), true);
                DT_CheckWrite(__LINE__, link, writes + 1, SDL_MYO_COMMAND, lock, sizeof(lock), true);
                DT_CHECK(SDL_GetTicks() - start >= (Uint64)(2 * slow_ms - DT_TICK_SLACK_MS), "the closing writes ended after %d ms",
                         (int)(SDL_GetTicks() - start));
            }
        }
    }
    DT_Stop();

    /* SDL_Quit right after a loss: the lost link gets no closing write */
    Fake_Reset();
    writes = 0;
    if (DT_StartListening(NULL)) {
        if (DT_CHECK(DT_ConnectMyo(0xC8E2F47B1A12, &link) != 0, "no Myo joystick")) {
            writes = DT_Count(FAKE_WRITE, link);
            DT_CHECK(DT_LoseLink(link), "Open received no atomic for link loss");
        }
    }
    DT_Stop();
    if (writes > 0) {
        DT_CHECK(DT_Count(FAKE_WRITE, link) == writes, "%d closing writes went to the lost link", DT_Count(FAKE_WRITE, link) - writes);
    }
    DT_End();
}

/* Scenario (n): while the listener is in place, Detect has the transport
   check the watcher every BLEGATT_WATCHER_CHECK_MS, never sooner, and a
   stopped watcher starts again at the next check. Without a listener there
   is no check. */
static void ScenarioWatcherCheck(void)
{
    FakeCall added, previous, call;
    Uint64 start;
    int count, restarts = 0, i;

    DT_Begin("n", "the watcher check while listening");
    if (!DT_Start(NULL, NULL)) {
        goto done;
    }
    DT_PumpFor(BLEGATT_WATCHER_CHECK_MS + DT_QUIET_MS);
    DT_CHECK(DT_Count(FAKE_CHECK_WATCHER, -1) == 0, "%d watcher checks without a listener", DT_Count(FAKE_CHECK_WATCHER, -1));

    DT_SetHint(DT_HINT_BLE, "1");
    if (!DT_CHECK(DT_WaitListener(true, DT_WAIT_MS), "no listener after SDL_JOYSTICK_BLE turned on")) {
        goto done;
    }
    DT_PumpFor(2 * BLEGATT_WATCHER_CHECK_MS + DT_QUIET_MS);
    count = DT_Count(FAKE_CHECK_WATCHER, -1);
    DT_CHECK(count >= 2, "%d watcher checks in %d ms of listening", count, 2 * BLEGATT_WATCHER_CHECK_MS + DT_QUIET_MS);
    if (DT_Position(FAKE_ADD_LISTENER, -1, 0, &added) >= 0) {
        previous = added;
        for (i = 0; i < count; ++i) {
            if (DT_Position(FAKE_CHECK_WATCHER, -1, i, &call) < 0) {
                break;
            }
            DT_CHECK(call.time_ns - previous.time_ns >= SDL_MS_TO_NS(BLEGATT_WATCHER_CHECK_MS - DT_TICK_SLACK_MS),
                     "watcher check %d came %" SDL_PRIu64 " ms after the one before", i,
                     SDL_NS_TO_MS(call.time_ns - previous.time_ns));
            previous = call;
        }
    }

    /* A stopped watcher starts again at the next check */
    SDL_LockMutex(fake.mutex);
    fake.watcher_aborted = true;
    SDL_UnlockMutex(fake.mutex);
    start = SDL_GetTicks();
    for (;;) {
        SDL_LockMutex(fake.mutex);
        restarts = fake.watcher_restarts;
        SDL_UnlockMutex(fake.mutex);
        if (restarts != 0 || DT_Expired(start, BLEGATT_WATCHER_CHECK_MS + DT_WAIT_MS)) {
            break;
        }
        DT_Pump();
    }
    DT_CHECK(restarts == 1, "%d watcher restarts after the watcher stopped", restarts);

    /* No check once the listener is gone */
    DT_SetHint(DT_HINT_BLE, "0");
    DT_CHECK(DT_WaitListener(false, DT_WAIT_MS), "the listener stayed after SDL_JOYSTICK_BLE turned off");
    count = DT_Count(FAKE_CHECK_WATCHER, -1);
    DT_PumpFor(BLEGATT_WATCHER_CHECK_MS + DT_QUIET_MS);
    DT_CHECK(DT_Count(FAKE_CHECK_WATCHER, -1) == count, "%d watcher checks without a listener",
             DT_Count(FAKE_CHECK_WATCHER, -1) - count);

done:
    DT_Stop();
    DT_End();
}

static int dt_assertions;

static SDL_AssertState SDLCALL DT_CountAssertion(const SDL_AssertData *data, void *userdata)
{
    (void)data;
    (void)userdata;
    ++dt_assertions;
    return SDL_ASSERTION_IGNORE;
}

/* Scenario (o): the executor's queue keeps its last slot for the
   disconnect. The session never fills it, so this fills it directly: one
   action too many is dropped, and the disconnect after it still fits. A
   debug build also reports the drop through SDL_assert, which is counted
   here and ignored. */
static void ScenarioExecutorQueue(void)
{
    BLEGATT_Connection *connection;
    SDL_BLEAction action, last;
    int i;

    DT_Begin("o", "the executor's last slot is the disconnect's");
    connection = (BLEGATT_Connection *)SDL_calloc(1, sizeof(*connection));
    if (!DT_CHECK(connection != NULL, "no memory")) {
        DT_End();
        return;
    }
    connection->mutex = SDL_CreateMutex();
    connection->exec_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (DT_CHECK(connection->mutex && connection->exec_event, "no mutex or event")) {
        dt_assertions = 0;
        SDL_SetAssertionHandler(DT_CountAssertion, NULL);
        SDL_zero(action);
        action.kind = SDL_BLE_ACTION_WRITE;
        for (i = 0; i < BLEGATT_EXEC_CAPACITY; ++i) {
            action.characteristic = (Uint8)i;
            BLEGATT_PostAction(connection, &action);
        }
        DT_CHECK(connection->exec_count == BLEGATT_EXEC_CAPACITY - 1, "%d actions queued, expected %d with the last slot kept",
                 connection->exec_count, BLEGATT_EXEC_CAPACITY - 1);
        SDL_zero(action);
        action.kind = SDL_BLE_ACTION_DISCONNECT;
        BLEGATT_PostAction(connection, &action);
        DT_CHECK(connection->exec_count == BLEGATT_EXEC_CAPACITY, "%d actions queued after the disconnect", connection->exec_count);
        last = connection->exec_actions[(connection->exec_head + connection->exec_count - 1) % BLEGATT_EXEC_CAPACITY];
        DT_CHECK(last.kind == SDL_BLE_ACTION_DISCONNECT, "the last queued action is %s, not the disconnect", SDL_BLE_ActionName(last.kind));
        SDL_SetAssertionHandler(NULL, NULL);
        printf("scenario (o): %d assertions reported the dropped action\n", dt_assertions);
    }
    if (connection->exec_event) {
        CloseHandle(connection->exec_event);
    }
    SDL_DestroyMutex(connection->mutex);
    SDL_free(connection);
    DT_End();
}

/* SDL's allocator, with one calloc size that can be made to fail. It is in
   place before SDL allocates anything and passes every call on to the
   functions SDL had, so every block keeps one allocator. */
static SDL_malloc_func dt_real_malloc;
static SDL_calloc_func dt_real_calloc;
static SDL_realloc_func dt_real_realloc;
static SDL_free_func dt_real_free;
static SDL_AtomicInt dt_failing_calloc; /* The byte count whose calloc fails, 0 for none */

static void *SDLCALL DT_Calloc(size_t count, size_t size)
{
    const int failing = SDL_GetAtomicInt(&dt_failing_calloc);

    if (failing > 0 && count * size == (size_t)failing) {
        return NULL;
    }
    return dt_real_calloc(count, size);
}

static void DT_InstallAllocator(void)
{
    SDL_GetMemoryFunctions(&dt_real_malloc, &dt_real_calloc, &dt_real_realloc, &dt_real_free);
    SDL_SetMemoryFunctions(dt_real_malloc, DT_Calloc, dt_real_realloc, dt_real_free);
}

/* The time before which the driver's host starts no session for the
   address. False when it holds no entry for it. */
static bool DT_HostUntil(Uint64 address, Uint64 *until)
{
    bool found = false;
    int i;

    SDL_LockMutex(blegatt_lock);
    for (i = 0; i < blegatt_host.count; ++i) {
        if (blegatt_host.entries[i].address == address) {
            *until = blegatt_host.entries[i].until;
            found = true;
        }
    }
    SDL_UnlockMutex(blegatt_lock);
    return found;
}

/* True while the driver's host holds a session for the address */
static bool DT_HostActive(Uint64 address)
{
    bool active;

    SDL_LockMutex(blegatt_lock);
    active = SDL_BLEHost_IsActive(&blegatt_host, address);
    SDL_UnlockMutex(blegatt_lock);
    return active;
}

/* Waits until the driver's host holds no session for the address, so the
   session's outcome has reached the host */
static bool DT_WaitHostIdle(Uint64 address, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (!DT_HostActive(address)) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

/* Scenario (p): how long an address waits. A change of the pairing hint
   lets an address that backed off under the old setting connect at its
   next advertisement, and a change of another hint does not. A failed
   connect holds the address SDL_BLE_CONNECT_RETRY_MS, however many
   advertisements come, and then it connects again. A connection the driver
   cannot create holds its address the same way. */
static void ScenarioRetries(void)
{
    static const Uint64 daydream = 0xD0B5C27A4E21, pokeball = 0xA4C138F1E2E4, starved = 0xA4C138F1E2E5;
    static const DT_Step refused[] = { DT_OPEN, DT_CLOSE };
    static const DT_Step paired[] = { DT_OPEN, DT_PAIR, DT_DISCOVER };
    static const DT_Step startup[] = {
        DT_OPEN, DT_DISCOVER, DT_SUB(SDL_POKEBALL_INPUT, SDL_BLE_CCCD_NOTIFY),
        DT_SUB(SDL_POKEBALL_BATTERY, SDL_BLE_CCCD_NOTIFY), DT_READ(SDL_POKEBALL_BATTERY)
    };
    SDL_BLEAdvertisement ad;
    FakeCall first, second;
    FakeConfig config;
    Uint64 failed, before, until = 0;
    bool switched = false;

    DT_Begin("p", "a pairing hint change ends a backoff, and a failed connect waits 5 s");

    /* An unbonded Daydream with pairing off backs off for 15 s */
    if (!DT_StartListening("0")) {
        goto done;
    }
    DT_Ad(&ad, daydream, SDL_BLE_AD_ADVERTISEMENT, "Daydream controller");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_CLOSE, -1, 1, DT_WAIT_MS), "the unbonded Daydream stayed connected with pairing off") ||
        !DT_CHECK(DT_WaitHostIdle(daydream, DT_WAIT_MS), "the Daydream's session did not end")) {
        goto done;
    }
    DT_CheckSteps(__LINE__, DT_Link(daydream), refused, DT_COUNT(refused), true);
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    DT_PumpFor(DT_QUIET_MS);
    DT_CHECK(DT_Count(FAKE_OPEN, -1) == 1, "%d connects, expected the backoff to hold the address", DT_Count(FAKE_OPEN, -1));

    /* Another hint's change leaves the backoff in place */
    DT_SetHint(dt_family_hints[SDL_BLE_FAMILY_MYO], "1");
    DT_PumpFor(DT_QUIET_MS);
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    DT_PumpFor(DT_QUIET_MS);
    DT_CHECK(DT_Count(FAKE_OPEN, -1) == 1, "%d connects after the Myo's hint changed, expected the backoff to hold",
             DT_Count(FAKE_OPEN, -1));

    /* The pairing hint on: the next advertisement connects and pairs, well
       inside the 15 s backoff */
    DT_SetHint(DT_HINT_PAIRING, "1");
    DT_PumpFor(DT_QUIET_MS);
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (DT_CHECK(DT_WaitCalls(FAKE_PAIR, -1, 1, DT_WAIT_MS), "the Daydream did not connect and pair after the pairing hint turned on")) {
        DT_CHECK(DT_Count(FAKE_OPEN, -1) == 2, "%d connects", DT_Count(FAKE_OPEN, -1));
        DT_CheckSteps(__LINE__, DT_Link(daydream), paired, DT_COUNT(paired), false);
        if (DT_Position(FAKE_OPEN, -1, 0, &first) >= 0 && DT_Position(FAKE_OPEN, -1, 1, &second) >= 0) {
            DT_CHECK(second.time_ns - first.time_ns < (Uint64)SDL_BLE_BACKOFF_MS * SDL_NS_PER_MS,
                     "the second connect came %" SDL_PRIu64 " ms after the first", (second.time_ns - first.time_ns) / SDL_NS_PER_MS);
        }
    }
    DT_Stop();

    /* A failed connect: no other for SDL_BLE_CONNECT_RETRY_MS while an
       advertisement comes every 20 ms, then the next one connects. The fake
       opens again from shortly before the wait ends. */
    Fake_Reset();
    DT_GetConfig(&config);
    config.open_fails = true;
    DT_SetConfig(&config);
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, pokeball, SDL_BLE_AD_ADVERTISEMENT, "Pokemon PBP");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_OPEN, -1, 1, DT_WAIT_MS), "the Poke Ball's advertisement made no connect")) {
        goto done;
    }
    failed = SDL_GetTicks();
    DT_CHECK(DT_Position(FAKE_OPEN, -1, 0, &first) >= 0 && !first.result, "the first connect did not fail");
    while (DT_Count(FAKE_OPEN, -1) < 2 && !DT_Expired(failed, SDL_BLE_CONNECT_RETRY_MS + DT_WAIT_MS)) {
        if (!switched && SDL_GetTicks() - failed >= SDL_BLE_CONNECT_RETRY_MS - 500) {
            config.open_fails = false;
            DT_SetConfig(&config);
            switched = true;
        }
        (void)DT_Advertise(&ad);
        DT_PumpFor(20);
    }
    if (DT_CHECK(DT_Position(FAKE_OPEN, -1, 1, &second) >= 0, "no second connect within %d ms of the failed one",
                 SDL_BLE_CONNECT_RETRY_MS + DT_WAIT_MS)) {
        const Uint64 gap = (second.time_ns - first.time_ns) / SDL_NS_PER_MS;

        printf("scenario (p): the second connect came %" SDL_PRIu64 " ms after the failed one\n", gap);
        DT_CHECK(gap + DT_TICK_SLACK_MS >= SDL_BLE_CONNECT_RETRY_MS && gap < SDL_BLE_CONNECT_RETRY_MS + DT_WAIT_MS,
                 "the second connect came %" SDL_PRIu64 " ms after the failed one, expected %d", gap, SDL_BLE_CONNECT_RETRY_MS);
        DT_CHECK(second.result, "the second connect failed");
        if (DT_CHECK(DT_WaitCalls(FAKE_READ, -1, 1, DT_WAIT_MS), "the Poke Ball did not start after the second connect")) {
            DT_CheckSteps(__LINE__, DT_Link(pokeball), startup, DT_COUNT(startup), true);
        }
    }
    DT_Stop();

    /* The connection's value queue cannot be allocated, as when memory runs
       out: no connect, and the address waits SDL_BLE_CONNECT_RETRY_MS from
       the advertisement */
    Fake_Reset();
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, starved, SDL_BLE_AD_ADVERTISEMENT, "Pokemon PBP");
    before = SDL_GetTicks();
    SDL_SetAtomicInt(&dt_failing_calloc, (int)(SDL_BLE_QUEUE_CAPACITY * sizeof(SDL_BLEValue)));
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    SDL_SetAtomicInt(&dt_failing_calloc, 0);
    failed = SDL_GetTicks();
    if (DT_CHECK(DT_HostUntil(starved, &until), "the host holds no entry for the address")) {
        DT_CHECK(until >= before + SDL_BLE_CONNECT_RETRY_MS && until <= failed + SDL_BLE_CONNECT_RETRY_MS,
                 "the address waits until %" SDL_PRIu64 ", expected %" SDL_PRIu64 " to %" SDL_PRIu64, until,
                 before + SDL_BLE_CONNECT_RETRY_MS, failed + SDL_BLE_CONNECT_RETRY_MS);
    }
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    DT_PumpFor(DT_QUIET_MS);
    DT_CHECK(DT_Count(FAKE_OPEN, -1) == 0, "%d connects while the address waits", DT_Count(FAKE_OPEN, -1));

done:
    SDL_SetAtomicInt(&dt_failing_calloc, 0);
    DT_Stop();
    DT_End();
}

/* The first bits of the Daydream record's touch X and Y, counted from bit 7
   of byte 0 (SDL_ble_daydream_proto.c:126-127) */
#define DT_DAYDREAM_TOUCH_X_BIT 131
#define DT_DAYDREAM_TOUCH_Y_BIT 139

/* Writes value into count bits of a Daydream record from bit start, in the
   order SDL_Daydream_Bits reads them */
static void DT_DaydreamSetBits(Uint8 *data, int start, int count, Uint32 value)
{
    int i;

    for (i = 0; i < count; ++i) {
        const int bit = start + i;
        const Uint8 mask = (Uint8)(0x80 >> (bit % 8));

        if (value & ((Uint32)1 << (count - 1 - i))) {
            data[bit / 8] |= mask;
        } else {
            data[bit / 8] &= (Uint8)~mask;
        }
    }
}

/* Record r of scenario (q)'s run of changes: capture 3 with the finger at
   (1 + r % 200, 1 + r / 200), so each record moves it */
static void DT_RunRecord(const Uint8 *flat, int r, Uint8 *record)
{
    SDL_memcpy(record, flat, SDL_DAYDREAM_REPORT_SIZE);
    DT_DaydreamSetBits(record, DT_DAYDREAM_TOUCH_X_BIT, 8, (Uint32)(1 + r % 200));
    DT_DaydreamSetBits(record, DT_DAYDREAM_TOUCH_Y_BIT, 8, (Uint32)(1 + r / 200));
}

/* The run record a touchpad event came from, by the finger's position */
static int DT_RunRecordOf(const SDL_GamepadTouchpadEvent *event)
{
    const int x = (int)(event->x * 255.0f + 0.5f);
    const int y = (int)(event->y * 255.0f + 0.5f);

    return (y - 1) * 200 + (x - 1);
}

/* Scenario (q): SDL's updates stall while a Daydream streams with its right
   shoulder held. With the sensors off, the release, 300 records alike and a
   touch queue two changes and no sample. With them on, the release and 300
   records alike bring 602 samples, more than the ring holds, and the
   release still arrives. A run of 300 changes, more than their queue
   holds, with 600 samples: the newest 256 changes and 512 samples arrive,
   each record's change before its two samples, in time order, and the
   finger ends where the last record put it. Sensors turned off again, and a
   closed gamepad, queue no sample. */
static void ScenarioStall(void)
{
    static const Uint64 address = 0xD0B5C27A4E31;
    static SDL_Event events[BLEGATT_CHANGE_CAPACITY + BLEGATT_SAMPLE_CAPACITY + 64];
    const int records = 300;
    Uint8 flat[20], touch[20], volume[20], record[20];
    SDL_BLEAdvertisement ad;
    SDL_Gamepad *gamepad = NULL;
    SDL_JoystickID id;
    FakeConfig config;
    DT_Queues before, after;
    Uint64 previous = 0;
    float pressure = 0.0f;
    bool order = true;
    int link, i, n, touches = 0, samples = 0, first = -1, last = -1;

    DT_Begin("q", "a stall of SDL's updates while a Daydream streams");
    (void)DT_Hex(dt_daydream_flat, flat, sizeof(flat));
    (void)DT_Hex(dt_daydream_touch, touch, sizeof(touch));
    (void)DT_Hex(dt_daydream_volume, volume, sizeof(volume));
    DT_GetConfig(&config);
    config.bonded = true;
    DT_SetConfig(&config);
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, address, SDL_BLE_AD_ADVERTISEMENT, "Daydream controller");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_READ, -1, 1, DT_WAIT_MS), "no battery read followed the advertisement")) {
        goto done;
    }
    link = DT_Link(address);
    id = DT_StreamUntilJoystick(link, SDL_DAYDREAM_POSE, volume, sizeof(volume));
    if (!DT_CHECK(id != 0, "no joystick after the first record")) {
        goto done;
    }
    gamepad = SDL_OpenGamepad(id);
    if (!DT_CHECK(gamepad != NULL, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        goto done;
    }
    DT_CHECK(DT_WaitButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true, DT_WAIT_MS), "volume up did not reach the right shoulder");
    DT_PumpFor(20);

    /* The sensors off, as a new gamepad has them. From here to the next
       wait for a button, no SDL update runs. */
    before = DT_GetQueues(address);
    DT_CHECK(before.found && !before.sensors && before.changes == 0 && before.samples == 0,
             "before the first stall: found %d, sensors %d, %d changes and %d samples queued", before.found ? 1 : 0,
             before.sensors ? 1 : 0, before.changes, before.samples);
    DT_CHECK(DT_Push(link, SDL_DAYDREAM_POSE, flat, sizeof(flat)), "the pose characteristic has no callback");
    for (i = 0; i < records; ++i) {
        (void)DT_Push(link, SDL_DAYDREAM_POSE, flat, sizeof(flat));
    }
    DT_CHECK(DT_Push(link, SDL_DAYDREAM_POSE, touch, sizeof(touch)), "the pose characteristic has no callback");
    DT_CHECK(DT_WaitQueued(address, before.sequence, 2, DT_WAIT_MS), "the release and the touch were not queued as two changes alone");
    after = DT_GetQueues(address);
    DT_CHECK(after.changes == 2 && after.samples == 0 && after.samples_dropped == before.samples_dropped,
             "with the sensors off, %d changes and %d samples were queued and %u samples dropped, expected 2 changes alone",
             after.changes, after.samples, after.samples_dropped - before.samples_dropped);
    DT_CHECK(DT_WaitButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false, DT_WAIT_MS),
             "the release during the stall was lost: the right shoulder stays down");
    DT_CHECK(DT_WaitFinger(gamepad, true, 136.0f / 255.0f, 75.0f / 255.0f, &pressure, DT_WAIT_MS), "no finger down at (136, 75) after the stall");

    /* The sensors on and the right shoulder held again. The release and 300
       records alike bring 602 samples, more than the ring holds. */
    DT_CHECK(SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_ACCEL, true) && SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, true),
             "the sensors could not be enabled: %s", SDL_GetError());
    DT_CHECK(DT_Push(link, SDL_DAYDREAM_POSE, volume, sizeof(volume)), "the pose characteristic has no callback");
    DT_CHECK(DT_WaitButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true, DT_WAIT_MS), "volume up did not reach the right shoulder");
    DT_PumpFor(20);
    before = DT_GetQueues(address);
    DT_CHECK(before.sensors && before.changes == 0 && before.samples == 0,
             "before the second stall: sensors %d, %d changes and %d samples queued", before.sensors ? 1 : 0, before.changes,
             before.samples);
    DT_CHECK(DT_Push(link, SDL_DAYDREAM_POSE, flat, sizeof(flat)), "the pose characteristic has no callback");
    for (i = 0; i < records; ++i) {
        (void)DT_Push(link, SDL_DAYDREAM_POSE, flat, sizeof(flat));
    }
    DT_CHECK(DT_WaitQueued(address, before.sequence, 1 + 2 * (records + 1), DT_WAIT_MS), "the release and its 602 samples were not queued");
    after = DT_GetQueues(address);
    DT_CHECK(after.changes == 1 && after.changes_dropped == before.changes_dropped,
             "%d changes queued and %u dropped under 602 samples, expected the release alone", after.changes,
             after.changes_dropped - before.changes_dropped);
    DT_CHECK(after.samples == BLEGATT_SAMPLE_CAPACITY &&
                 after.samples_dropped - before.samples_dropped == (Uint32)(2 * (records + 1) - BLEGATT_SAMPLE_CAPACITY),
             "%d samples queued and %u dropped, expected %d and %d", after.samples, after.samples_dropped - before.samples_dropped,
             BLEGATT_SAMPLE_CAPACITY, 2 * (records + 1) - BLEGATT_SAMPLE_CAPACITY);
    DT_CHECK(DT_WaitButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false, DT_WAIT_MS),
             "the samples pushed the release out: the right shoulder stays down");

    /* A run of 300 records, each moving the finger, with 600 samples */
    DT_PumpFor(20);
    SDL_FlushEvents(SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, SDL_EVENT_GAMEPAD_SENSOR_UPDATE);
    before = DT_GetQueues(address);
    for (i = 0; i < records; ++i) {
        DT_RunRecord(flat, i, record);
        (void)DT_Push(link, SDL_DAYDREAM_POSE, record, sizeof(record));
    }
    DT_CHECK(SDL_Daydream_Bits(record, DT_DAYDREAM_TOUCH_X_BIT, 8) == (Uint32)(1 + (records - 1) % 200) &&
                 SDL_Daydream_Bits(record, DT_DAYDREAM_TOUCH_Y_BIT, 8) == (Uint32)(1 + (records - 1) / 200),
             "the last run record holds touch (%u, %u)", (unsigned int)SDL_Daydream_Bits(record, DT_DAYDREAM_TOUCH_X_BIT, 8),
             (unsigned int)SDL_Daydream_Bits(record, DT_DAYDREAM_TOUCH_Y_BIT, 8));
    DT_CHECK(DT_WaitQueued(address, before.sequence, 3 * records, DT_WAIT_MS), "the run's changes and samples were not queued");
    after = DT_GetQueues(address);
    DT_CHECK(after.changes == BLEGATT_CHANGE_CAPACITY &&
                 after.changes_dropped - before.changes_dropped == (Uint32)(records - BLEGATT_CHANGE_CAPACITY),
             "%d changes queued and %u dropped, expected %d and %d", after.changes, after.changes_dropped - before.changes_dropped,
             BLEGATT_CHANGE_CAPACITY, records - BLEGATT_CHANGE_CAPACITY);
    DT_CHECK(after.samples == BLEGATT_SAMPLE_CAPACITY &&
                 after.samples_dropped - before.samples_dropped == (Uint32)(2 * records - BLEGATT_SAMPLE_CAPACITY),
             "%d samples queued and %u dropped, expected %d and %d", after.samples, after.samples_dropped - before.samples_dropped,
             BLEGATT_SAMPLE_CAPACITY, 2 * records - BLEGATT_SAMPLE_CAPACITY);
    DT_PumpFor(20);
    n = SDL_PeepEvents(events, DT_COUNT(events), SDL_GETEVENT, SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, SDL_EVENT_GAMEPAD_SENSOR_UPDATE);
    for (i = 0; i < n; ++i) {
        const SDL_Event *event = &events[i];
        const int position = (touches + samples) % 3;

        if (event->type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN || event->type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION) {
            const int r = DT_RunRecordOf(&event->gtouchpad);

            if (position != 0 || (touches == 0) != (event->type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN) ||
                (last >= 0 && r != last + 1)) {
                order = false;
            }
            if (first < 0) {
                first = r;
            }
            last = r;
            ++touches;
        } else if (event->type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE) {
            if (position != ((event->gsensor.sensor == SDL_SENSOR_ACCEL) ? 1 : 2)) {
                order = false;
            }
            ++samples;
        } else {
            order = false;
        }
        if (event->common.timestamp < previous) {
            order = false;
        }
        previous = event->common.timestamp;
    }
    DT_CHECK(touches == BLEGATT_CHANGE_CAPACITY && samples == BLEGATT_SAMPLE_CAPACITY,
             "%d touchpad and %d sensor events after the run, expected %d and %d", touches, samples, BLEGATT_CHANGE_CAPACITY,
             BLEGATT_SAMPLE_CAPACITY);
    DT_CHECK(first == records - BLEGATT_CHANGE_CAPACITY && last == records - 1,
             "the touchpad events run from record %d to %d, expected %d to %d", first, last, records - BLEGATT_CHANGE_CAPACITY,
             records - 1);
    DT_CHECK(order, "the events after the run are not, record by record, a touch, an accelerometer and a gyroscope sample in time order");
    DT_CHECK(DT_WaitFinger(gamepad, true, (float)(1 + (records - 1) % 200) / 255.0f, (float)(1 + (records - 1) / 200) / 255.0f,
                           &pressure, DT_WAIT_MS),
             "the finger did not end where the last run record put it");

    /* The sensors off again: the finger's lift and ten records alike queue
       the one change and no sample */
    DT_CHECK(SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_ACCEL, false) && SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, false),
             "the sensors could not be disabled: %s", SDL_GetError());
    DT_PumpFor(20);
    before = DT_GetQueues(address);
    for (i = 0; i < 11; ++i) {
        DT_CHECK(DT_Push(link, SDL_DAYDREAM_POSE, flat, sizeof(flat)), "the pose characteristic has no callback");
    }
    DT_CHECK(DT_WaitQueued(address, before.sequence, 1, DT_WAIT_MS), "the finger's lift was not queued as one change alone");
    after = DT_GetQueues(address);
    DT_CHECK(!after.sensors && after.changes == 1 && after.samples == 0,
             "with the sensors off again: sensors %d, %d changes and %d samples queued", after.sensors ? 1 : 0, after.changes,
             after.samples);
    DT_PumpFor(20);

    /* A closed gamepad queues no sample, though its sensors were on */
    DT_CHECK(SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_ACCEL, true), "the accelerometer could not be enabled: %s", SDL_GetError());
    DT_CHECK(DT_GetQueues(address).sensors, "the accelerometer is on, and the connection queues no sample");
    SDL_CloseGamepad(gamepad);
    gamepad = NULL;
    before = DT_GetQueues(address);
    DT_CHECK(DT_Push(link, SDL_DAYDREAM_POSE, touch, sizeof(touch)), "the pose characteristic has no callback");
    DT_CHECK(DT_WaitQueued(address, before.sequence, 1, DT_WAIT_MS), "the touch was not queued as one change alone");
    after = DT_GetQueues(address);
    DT_CHECK(!after.sensors && after.samples == 0, "after the gamepad closed: sensors %d, %d samples queued", after.sensors ? 1 : 0,
             after.samples);

done:
    if (gamepad) {
        SDL_CloseGamepad(gamepad);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (r): a link lost before the joystick appeared makes the address
   wait SDL_BLE_CONNECT_RETRY_MS, as a failed connect does. A Poke Ball lost
   after its start-up and before its first report is not connected again at
   the next advertisement, and connects once the wait is over. A loss while
   the session repairs a failed subscription frees the address at once: an
   Oculus Go whose link drops while it pairs is connected again at its next
   advertisement. */
static void ScenarioEarlyLoss(void)
{
    static const Uint64 pokeball = 0xA4C138F1E2F1, oculus = 0x2CD0AB16E7FA;
    static const DT_Step startup[] = {
        DT_OPEN, DT_DISCOVER, DT_SUB(SDL_POKEBALL_INPUT, SDL_BLE_CCCD_NOTIFY),
        DT_SUB(SDL_POKEBALL_BATTERY, SDL_BLE_CCCD_NOTIFY), DT_READ(SDL_POKEBALL_BATTERY), DT_CLOSE
    };
    static const DT_Step repairing[] = {
        DT_OPEN, DT_DISCOVER, DT_SUB(SDL_OCULUSGO_FACE, SDL_BLE_CCCD_NOTIFY), DT_PAIR, DT_CLOSE
    };
    static const Uint8 rest[SDL_POKEBALL_REPORT_SIZE] = { 0x00, 0x00, 0x00, 0x07, 0x6C };
    SDL_BLEAdvertisement ad;
    FakeConfig config;
    FakeCall second;
    Uint64 lost_ns, idle, until = 0, start;
    int link, entered = 0;

    DT_Begin("r", "a link lost before the joystick appeared");

    /* The Poke Ball's link drops after its start-up, before its first report */
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, pokeball, SDL_BLE_AD_ADVERTISEMENT, "Pokemon PBP");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_READ, -1, 1, DT_WAIT_MS), "no battery read followed the advertisement")) {
        goto done;
    }
    link = DT_Link(pokeball);
    DT_PumpFor(50);
    DT_CHECK(DT_Joysticks(NULL) == 0, "the Poke Ball was published without input");
    lost_ns = SDL_GetTicksNS();
    DT_CHECK(DT_LoseLink(link), "Open received no atomic for link loss");
    DT_CHECK(DT_WaitCalls(FAKE_CLOSE, link, 1, DT_WAIT_MS), "the lost link was not closed");
    if (!DT_CHECK(DT_WaitHostIdle(pokeball, DT_WAIT_MS), "the Poke Ball's session did not end")) {
        goto done;
    }
    idle = SDL_GetTicks();
    DT_CheckSteps(__LINE__, link, startup, DT_COUNT(startup), true);
    if (DT_CHECK(DT_HostUntil(pokeball, &until), "the host holds no entry for the Poke Ball")) {
        DT_CHECK(until >= SDL_NS_TO_MS(lost_ns) + SDL_BLE_CONNECT_RETRY_MS && until <= idle + SDL_BLE_CONNECT_RETRY_MS,
                 "the address waits until %" SDL_PRIu64 ", expected %" SDL_PRIu64 " to %" SDL_PRIu64, until,
                 SDL_NS_TO_MS(lost_ns) + SDL_BLE_CONNECT_RETRY_MS, idle + SDL_BLE_CONNECT_RETRY_MS);
    }
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    DT_PumpFor(DT_QUIET_MS);
    DT_CHECK(DT_Count(FAKE_OPEN, -1) == 1, "%d connects right after the loss, expected the address to wait", DT_Count(FAKE_OPEN, -1));

    /* An advertisement every 20 ms: the next connect comes once the wait is
       over, and the Poke Ball starts up and appears */
    start = SDL_GetTicks();
    while (DT_Count(FAKE_OPEN, -1) < 2 && !DT_Expired(start, SDL_BLE_CONNECT_RETRY_MS + DT_WAIT_MS)) {
        (void)DT_Advertise(&ad);
        DT_PumpFor(20);
    }
    if (DT_CHECK(DT_Position(FAKE_OPEN, -1, 1, &second) >= 0, "no connect within %d ms of the loss",
                 SDL_BLE_CONNECT_RETRY_MS + DT_WAIT_MS)) {
        const Uint64 gap = (second.time_ns - lost_ns) / SDL_NS_PER_MS;

        printf("scenario (r): the Poke Ball connected again %" SDL_PRIu64 " ms after the loss\n", gap);
        DT_CHECK(gap + DT_TICK_SLACK_MS >= SDL_BLE_CONNECT_RETRY_MS && gap < SDL_BLE_CONNECT_RETRY_MS + DT_WAIT_MS,
                 "the Poke Ball connected again %" SDL_PRIu64 " ms after the loss, expected %d", gap, SDL_BLE_CONNECT_RETRY_MS);
        if (DT_CHECK(DT_WaitCalls(FAKE_READ, -1, 2, DT_WAIT_MS), "the Poke Ball did not start up again")) {
            DT_CHECK(DT_StreamUntilJoystick(DT_Link(pokeball), SDL_POKEBALL_INPUT, rest, sizeof(rest)) != 0,
                     "no joystick after the Poke Ball connected again");
        }
    }
    DT_Stop();

    /* An Oculus Go loses its link while it pairs after its first
       subscription failed for want of encryption: the address is free at
       once */
    Fake_Reset();
    DT_GetConfig(&config);
    config.subscribe_fail = SDL_OCULUSGO_FACE;
    config.subscribe_failures = 1;
    config.subscribe_status = SDL_BLE_STATUS_PROTOCOL_ERROR;
    config.subscribe_error = SDL_BLE_ATT_INSUFFICIENT_ENCRYPTION;
    config.pair_blocks = true;
    DT_SetConfig(&config);
    if (!DT_StartListening("1")) {
        goto done;
    }
    DT_Ad(&ad, oculus, SDL_BLE_AD_ADVERTISEMENT, "OMVR-V190");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    start = SDL_GetTicks();
    while (!DT_Expired(start, DT_WAIT_MS)) {
        SDL_LockMutex(fake.mutex);
        entered = fake.pairs_entered;
        SDL_UnlockMutex(fake.mutex);
        if (entered) {
            break;
        }
        DT_Pump();
    }
    if (!DT_CHECK(entered == 1, "%d pairings began after the failed subscription", entered)) {
        goto done;
    }
    link = DT_Link(oculus);
    DT_CHECK(DT_LoseLink(link), "Open received no atomic for link loss");
    DT_CHECK(DT_WaitCalls(FAKE_CLOSE, link, 1, DT_WAIT_MS), "the link lost while pairing was not closed");
    if (!DT_CHECK(DT_WaitHostIdle(oculus, DT_WAIT_MS), "the Oculus Go's session did not end")) {
        goto done;
    }
    DT_CheckSteps(__LINE__, link, repairing, DT_COUNT(repairing), true);
    if (DT_CHECK(DT_HostUntil(oculus, &until), "the host holds no entry for the Oculus Go")) {
        DT_CHECK(until <= SDL_GetTicks(), "the address waits until %" SDL_PRIu64 " after a loss while pairing, expected no wait",
                 until);
    }
    DT_GetConfig(&config);
    config.pair_blocks = false;
    DT_SetConfig(&config);
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    DT_CHECK(DT_WaitCalls(FAKE_OPEN, -1, 2, DT_WAIT_MS), "the Oculus Go was not connected again at its next advertisement");

done:
    DT_Stop();
    DT_End();
}

/* Waits until both threads of a connection SDL_Quit left to end on its own
   have ended */
static bool DT_WaitAbandoned(BLEGATT_Connection *connection, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (SDL_GetAtomicInt(&connection->session_exited) && SDL_GetAtomicInt(&connection->executor_exited)) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        SDL_Delay(1);
    }
}

/* Scenario (s): SDL_Quit while a link's Close hangs, as
   GattDeviceService.Close sometimes does (bleak
   backends/winrt/client.py:488-491). SDL_Quit returns once
   BLEGATT_QUIT_WAIT_MS has passed and leaves the connection to end on its
   own, with one line in the log and no thread left joinable. A new SDL
   session connects the same address, and when the old Close returns, the
   old session leaves the new session's hold on the address alone. */
static void ScenarioHungClose(void)
{
    static const Uint64 address = 0xA4C138F1E2F5;
    static const Uint8 rest[SDL_POKEBALL_REPORT_SIZE] = { 0x00, 0x00, 0x00, 0x07, 0x6C };
    char line[256], expected[256];
    SDL_BLEAdvertisement ad;
    BLEGATT_Connection *old = NULL;
    FakeConfig config;
    Uint64 start;
    int link, elapsed, lines, leaked, hanging, ended;

    DT_Begin("s", "SDL_Quit while a link's Close hangs");
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, address, SDL_BLE_AD_ADVERTISEMENT, "Pokemon PBP");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_READ, -1, 1, DT_WAIT_MS), "no battery read followed the advertisement")) {
        goto done;
    }
    link = DT_Link(address);
    if (!DT_CHECK(DT_StreamUntilJoystick(link, SDL_POKEBALL_INPUT, rest, sizeof(rest)) != 0, "no joystick after the first report")) {
        goto done;
    }
    old = DT_FindConnection(address);
    if (!DT_CHECK(old != NULL, "the driver holds no connection to the address")) {
        goto done;
    }
    DT_GetConfig(&config);
    config.close_blocks = true;
    DT_SetConfig(&config);

    lines = DT_AbandonLog(NULL, 0);
    leaked = DT_LeakedThreads();
    start = SDL_GetPerformanceCounter();
    SDL_Quit();
    elapsed = (int)((SDL_GetPerformanceCounter() - start) * 1000 / SDL_GetPerformanceFrequency());
    SDL_LockMutex(fake.mutex);
    hanging = fake.closes_hanging;
    SDL_UnlockMutex(fake.mutex);
    printf("scenario (s): SDL_Quit took %d ms while the Close hung\n", elapsed);
    DT_CHECK(hanging == 1, "%d Close calls hung through SDL_Quit, expected 1", hanging);
    DT_CHECK(elapsed >= BLEGATT_QUIT_WAIT_MS - DT_TICK_SLACK_MS && elapsed < BLEGATT_QUIT_WAIT_MS + 1000,
             "SDL_Quit took %d ms while the Close hung, expected the %d ms wait and little more", elapsed, BLEGATT_QUIT_WAIT_MS);
    (void)SDL_snprintf(expected, sizeof(expected),
                       "BLE GATT A4:C1:38:F1:E2:F5 (Poke Ball Plus): still closing after %d ms, left to end on its own",
                       BLEGATT_QUIT_WAIT_MS);
    if (DT_CHECK(DT_AbandonLog(line, sizeof(line)) == lines + 1, "%d lines for a connection left to end on its own, expected 1",
                 DT_AbandonLog(NULL, 0) - lines)) {
        DT_CHECK(SDL_strcmp(line, expected) == 0, "the driver logged \"%s\", expected \"%s\"", line, expected);
    }
    DT_CHECK(DT_LeakedThreads() == leaked, "SDL_Quit found %d threads still joinable", DT_LeakedThreads() - leaked);
    DT_CHECK(!SDL_GetAtomicInt(&old->session_exited) && !SDL_GetAtomicInt(&old->executor_exited),
             "the connection's threads ended while its Close hung");

    /* A new SDL session connects the same address while the old Close
       still hangs */
    config.close_blocks = false;
    DT_SetConfig(&config);
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_CHECK(DT_FindConnection(address) == NULL, "the abandoned connection is in the new session's list");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_READ, -1, 2, DT_WAIT_MS), "the Poke Ball did not start in the new session")) {
        goto done;
    }
    DT_CHECK(DT_Link(address) != link, "the new session reused link %d", link);
    DT_CHECK(DT_HostActive(address), "the new session's host holds no session for the address");

    /* The old Close returns, and the old threads end without a word */
    DT_CHECK(!SDL_GetAtomicInt(&old->session_exited) && !SDL_GetAtomicInt(&old->executor_exited),
             "the old connection's threads ended before its Close returned");
    ended = DT_EndedLines();
    SDL_SetAtomicInt(&fake.close_release, 1);
    DT_CHECK(DT_WaitAbandoned(old, DT_WAIT_MS), "the connection's threads did not end once its Close returned");
    DT_CHECK(DT_EndedLines() == ended, "the old session logged its end after SDL_Quit had left it");
    DT_CHECK(DT_HostActive(address), "the old session ended the new session's hold on the address");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    DT_PumpFor(DT_QUIET_MS);
    DT_CHECK(DT_Count(FAKE_OPEN, -1) == 2, "%d connects, expected the new session to keep the address", DT_Count(FAKE_OPEN, -1));

done:
    /* Whatever failed, the hang ends and the old threads finish before the
       fake's records are checked */
    SDL_SetAtomicInt(&fake.close_release, 1);
    if (old) {
        (void)DT_WaitAbandoned(old, DT_WAIT_MS);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (t): the session publishes a ready module while a write is
   still out with the executor, since a publish awaits no answer. A Zwift
   Play half's RideOn write is held while the controller answers it and
   sends a key pad message: the joystick appears with the write still out,
   and the write's answer, taken when it comes, leaves the joystick as it
   was. */
static void ScenarioPublishDuringWrite(void)
{
    static const Uint64 address = 0xF1D7329A4C63;
    static const Uint8 ride_on[6] = { 0x52, 0x69, 0x64, 0x65, 0x4F, 0x6E };
    static const Uint8 ride_on_play[8] = { 0x52, 0x69, 0x64, 0x65, 0x4F, 0x6E, 0x01, 0x04 };
    static const Uint8 type[3] = { SDL_ZWIFT_PLAY_RIGHT, 0x4C, 0x63 };
    static const DT_Step startup[] = {
        DT_OPEN, DT_DISCOVER, DT_SUB(SDL_ZWIFT_ASYNC, SDL_BLE_CCCD_NOTIFY), DT_SUB(SDL_ZWIFT_SYNC_TX, SDL_BLE_CCCD_INDICATE),
        DT_WRITE(SDL_ZWIFT_SYNC_RX)
    };
    Uint8 a[32], pull[32];
    size_t a_length, pull_length;
    SDL_BLEAdvertisement ad;
    SDL_Gamepad *gamepad = NULL;
    SDL_JoystickID id;
    FakeConfig config;
    int link;

    DT_Begin("t", "a publish while a write is out");
    a_length = DT_Hex(dt_zwift_a, a, sizeof(a));
    pull_length = DT_Hex(dt_zwift_pull, pull, sizeof(pull));
    DT_GetConfig(&config);
    config.write_holds = true;
    DT_SetConfig(&config);
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, address, SDL_BLE_AD_ADVERTISEMENT, "Zwift Play");
    DT_AdService(&ad, "00000001-19ca-4651-86e5-fa29dcdd09d1");
    DT_AdManufacturer(&ad, 0x094A, type, (Uint8)sizeof(type));
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitHeld(1, DT_WAIT_MS), "the RideOn write did not go out")) {
        goto done;
    }
    link = DT_Link(address);

    /* The controller answers the RideOn and sends a key pad message while
       the write is out */
    DT_CHECK(DT_Push(link, SDL_ZWIFT_SYNC_TX, ride_on_play, sizeof(ride_on_play)), "SYNC_TX has no callback");
    id = DT_StreamUntilJoystick(link, SDL_ZWIFT_ASYNC, a, a_length);
    if (!DT_CHECK(id != 0, "no joystick while the RideOn write was out")) {
        goto done;
    }
    DT_CHECK(DT_Held() == 1 && DT_Count(FAKE_WRITE, link) == 0, "the RideOn write came back before the joystick appeared");
    gamepad = SDL_OpenGamepad(id);
    if (!DT_CHECK(gamepad != NULL, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        goto done;
    }
    DT_CHECK(DT_WaitButton(gamepad, SDL_GAMEPAD_BUTTON_EAST, true, DT_WAIT_MS), "A did not reach East while the write was out");

    /* The write's answer comes after the publish */
    SDL_SetAtomicInt(&fake.hold_release, 1);
    if (DT_CHECK(DT_WaitCalls(FAKE_WRITE, link, 1, DT_WAIT_MS), "the RideOn write did not come back once released")) {
        DT_CheckWrite(__LINE__, link, 0, SDL_ZWIFT_SYNC_RX, ride_on, sizeof(ride_on), false);
    }
    DT_PumpFor(DT_QUIET_MS);
    DT_CHECK(DT_Joysticks(NULL) == 1, "the joystick went once the write's answer came");
    DT_CHECK(DT_Push(link, SDL_ZWIFT_ASYNC, pull, pull_length), "ASYNC has no callback");
    DT_CHECK(DT_WaitAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767, DT_WAIT_MS), "the right trigger reads %d on a full pull",
             SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    DT_CheckSteps(__LINE__, link, startup, DT_COUNT(startup), true);

done:
    SDL_SetAtomicInt(&fake.hold_release, 1);
    if (gamepad) {
        SDL_CloseGamepad(gamepad);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (u): the session ends while a subscription is still out with the
   executor. An Oculus Go silent for SDL_OCULUSGO_SILENCE_MS is subscribed
   again, and its link drops while that subscription is held: the joystick
   goes at once. When the subscription comes back, its answer is dropped,
   no other subscription follows, and the link closes once. */
static void ScenarioEndDuringSubscribe(void)
{
    static const Uint64 address = 0x2CD0AB16E7FB;
    static const DT_Step calls[] = {
        DT_OPEN, DT_DISCOVER, DT_SUB(SDL_OCULUSGO_FACE, SDL_BLE_CCCD_NOTIFY), DT_SUB(SDL_OCULUSGO_BEEF, SDL_BLE_CCCD_NOTIFY),
        DT_SUB(SDL_OCULUSGO_FACE, SDL_BLE_CCCD_NOTIFY), DT_CLOSE
    };
    Uint8 report[SDL_OCULUSGO_REPORT_SIZE];
    SDL_BLEAdvertisement ad;
    FakeConfig config;
    int link;

    DT_Begin("u", "an end while a subscription is out");
    SDL_zeroa(report);
    report[19] = 0x01; /* The Oculus button */
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, address, SDL_BLE_AD_ADVERTISEMENT, "OMVR-V190");
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_SUBSCRIBE, -1, 2, DT_WAIT_MS), "the subscriptions did not follow the advertisement")) {
        goto done;
    }
    link = DT_Link(address);
    if (!DT_CHECK(DT_StreamUntilJoystick(link, SDL_OCULUSGO_FACE, report, sizeof(report)) != 0, "no joystick after the first report")) {
        goto done;
    }

    /* The stream stops: after the silence the module subscribes again, and
       the fake holds that subscription */
    DT_GetConfig(&config);
    config.subscribe_holds = true;
    DT_SetConfig(&config);
    if (!DT_CHECK(DT_WaitHeld(1, SDL_OCULUSGO_SILENCE_MS + DT_WAIT_MS), "no subscription followed the silence")) {
        goto done;
    }
    DT_CHECK(DT_Joysticks(NULL) == 1, "the joystick went with the silence");

    /* The link drops while the subscription is out */
    DT_CHECK(DT_LoseLink(link), "Open received no atomic for link loss");
    DT_CHECK(DT_WaitNoJoystick(DT_WAIT_MS), "the joystick stayed while the subscription was out");
    DT_CHECK(DT_Held() == 1 && DT_Count(FAKE_CLOSE, link) == 0, "the subscription came back, or the link closed, before the joystick went");

    /* The subscription comes back after the end */
    SDL_SetAtomicInt(&fake.hold_release, 1);
    DT_CHECK(DT_WaitCalls(FAKE_CLOSE, link, 1, DT_WAIT_MS), "the link did not close once the subscription came back");
    DT_CHECK(DT_WaitHostIdle(address, DT_WAIT_MS), "the session did not end");
    DT_PumpFor(DT_QUIET_MS);
    DT_CheckSteps(__LINE__, link, calls, DT_COUNT(calls), true);

done:
    SDL_SetAtomicInt(&fake.hold_release, 1);
    DT_Stop();
    DT_End();
}

/* Scenario (w): a family turned off while a handshake write is out. The
   stop is stored before the cancel, and the session closes before it takes
   the write's failure, so that failure costs the address nothing: no
   backoff, and the family turned on again connects at the next
   advertisement. */
static void ScenarioStopDuringWrite(void)
{
    static const Uint64 address = 0xF1D7329A4C71;
    static const Uint8 type[3] = { SDL_ZWIFT_PLAY_RIGHT, 0x4C, 0x71 };
    SDL_BLEAdvertisement ad;
    FakeConfig config;
    Uint64 until = 0;
    int writes;

    DT_Begin("w", "a family turned off while a handshake write is out");
    DT_GetConfig(&config);
    config.write_delay_ms = 2000;
    DT_SetConfig(&config);
    if (!DT_StartListening(NULL)) {
        goto done;
    }
    DT_Ad(&ad, address, SDL_BLE_AD_ADVERTISEMENT, "Zwift Play");
    DT_AdManufacturer(&ad, 0x094A, type, (Uint8)sizeof(type));
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement");
    if (!DT_CHECK(DT_WaitCalls(FAKE_SUBSCRIBE, -1, 2, DT_WAIT_MS), "no subscriptions")) {
        goto done;
    }
    /* The RideOn write is out for 2000 ms */
    DT_PumpFor(200);
    writes = DT_Count(FAKE_WRITE, -1);
    DT_CHECK(writes == 0, "the RideOn write came back early, %d writes", writes);

    DT_SetHint(dt_family_hints[SDL_BLE_FAMILY_ZWIFT], "0");
    DT_CHECK(DT_WaitHostIdle(address, DT_WAIT_MS), "the session did not end after the hint went off");
    DT_CHECK(!DT_HostUntil(address, &until) || until <= SDL_GetTicks(),
             "the canceled write made the address wait until %" SDL_PRIu64 ", now %" SDL_PRIu64, until, SDL_GetTicks());

    /* Turned on again, the next advertisement connects at once */
    DT_SetHint(dt_family_hints[SDL_BLE_FAMILY_ZWIFT], "1");
    DT_PumpFor(DT_QUIET_MS);
    DT_CHECK(DT_Advertise(&ad), "no listener took the advertisement after the hint came back");
    DT_CHECK(DT_WaitCalls(FAKE_SUBSCRIBE, -1, 4, DT_WAIT_MS), "the address did not connect again at once");

done:
    DT_Stop();
    DT_End();
}

static const struct
{
    char id;
    void (*run)(void);
} dt_scenarios[] = {
    { 'a', ScenarioPokeBall },
    { 'b', ScenarioDaydream },
    { 'c', ScenarioGearVR },
    { 'd', ScenarioOculusGo },
    { 'e', ScenarioGuitar },
    { 'f', ScenarioZwift },
    { 'g', ScenarioMyo },
    { 'h', ScenarioLinkLoss },
    { 'i', ScenarioQuitWhileOpening },
    { 'j', ScenarioHintOff },
    { 'k', ScenarioZwiftHalves },
    { 'l', ScenarioGUIDs },
    { 'm', ScenarioClosingWrites },
    { 'n', ScenarioWatcherCheck },
    { 'o', ScenarioExecutorQueue },
    { 'p', ScenarioRetries },
    { 'q', ScenarioStall },
    { 'r', ScenarioEarlyLoss },
    { 's', ScenarioHungClose },
    { 't', ScenarioPublishDuringWrite },
    { 'u', ScenarioEndDuringSubscribe },
    { 'w', ScenarioStopDuringWrite }
};

int main(int argc, char *argv[])
{
    HANDLE watchdog;
    size_t i;
    int arg;

    setvbuf(stdout, NULL, _IONBF, 0);
    DT_InstallAllocator();
    SDL_SetMainReady();
    dt_main_thread = SDL_GetCurrentThreadID();
    Fake_Setup();
    dt_log_lock = SDL_CreateMutex();
    if (!fake.mutex || !dt_log_lock) {
        printf("testblegattdriver: no mutex\n");
        return 2;
    }
    DT_LoadGearVRFixtures(argc > 1 ? argv[1] : NULL);
    watchdog = (HANDLE)_beginthreadex(NULL, 0, DT_Watchdog, NULL, 0, NULL);
    if (watchdog) {
        CloseHandle(watchdog);
    }

    for (i = 0; i < SDL_arraysize(dt_scenarios); ++i) {
        bool run = (argc <= 2);

        for (arg = 2; arg < argc; ++arg) {
            if (argv[arg][0] == dt_scenarios[i].id && argv[arg][1] == '\0') {
                run = true;
            }
        }
        if (run) {
            dt_scenarios[i].run();
        }
    }
    Fake_Reset();
    SDL_DestroyMutex(dt_log_lock);
    SDL_DestroyMutex(fake.mutex);
    printf("testblegattdriver: %d checks, %d failures\n", dt_checks, dt_failures);
    return dt_failures ? 1 : 0;
}
