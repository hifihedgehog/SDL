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
 * PadForge fork: controllers that carry their input over a Bluetooth RFCOMM
 * channel, the MOGA in Mode A, the Zeemote JS1, the Chainpus BGP100 and the
 * 2011 Phonejoy. See hifihedgehog/SDL#33 Part 11.
 *
 * A discovery thread reads the system's list of known Bluetooth devices
 * every 3000 ms without an inquiry, so the user pairs the controller in
 * Windows first. The joystick thread picks the paired devices whose name
 * matches a family and whose hint is on, and gives each a thread that owns
 * its socket and runs the pure link in SDL_rfcomm_proto.c, which decides
 * every connect, send and close. As in the serial and DJI TCP drivers, the
 * device thread hands presence and snapshots to the joystick thread under
 * the device's mutex and never takes the joystick lock. ws2_32 and
 * bthprops.cpl are loaded at run time, so SDL links no Bluetooth or Winsock
 * library, and without them the driver finds nothing.
 */

#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_RFCOMM

#include "../SDL_sysjoystick.h"
#include "../../core/windows/SDL_windows.h"
#include "../serial/SDL_serial_engine.h"
#include "SDL_rfcomm_proto.h"
#include "SDL_rfcomm_moga_proto.h"
#include "SDL_rfcomm_zeemote_proto.h"
#include "SDL_rfcomm_bgp100_proto.h"

#include <winsock2.h>
#include <ws2bth.h>
#include <bluetoothapis.h>

#define RFCOMM_READ_SIZE   1024
#define RFCOMM_MAX_ACTIONS 64 /* Actions one pass may run, a guard only */
#define RFCOMM_MAX_PAIRED  64

typedef int(WSAAPI *RFCOMM_WSAStartupFunc)(WORD, LPWSADATA);
typedef int(WSAAPI *RFCOMM_WSACleanupFunc)(void);
typedef SOCKET(WSAAPI *RFCOMM_SocketFunc)(int, int, int);
typedef int(WSAAPI *RFCOMM_CloseSocketFunc)(SOCKET);
typedef int(WSAAPI *RFCOMM_ConnectFunc)(SOCKET, const struct sockaddr *, int);
typedef int(WSAAPI *RFCOMM_SendFunc)(SOCKET, const char *, int, int);
typedef int(WSAAPI *RFCOMM_RecvFunc)(SOCKET, char *, int, int);
typedef int(WSAAPI *RFCOMM_WSAGetLastErrorFunc)(void);
typedef int(WSAAPI *RFCOMM_WSAEventSelectFunc)(SOCKET, WSAEVENT, long);
typedef int(WSAAPI *RFCOMM_WSAEnumNetworkEventsFunc)(SOCKET, WSAEVENT, LPWSANETWORKEVENTS);
typedef HBLUETOOTH_DEVICE_FIND(WINAPI *RFCOMM_FindFirstDeviceFunc)(const BLUETOOTH_DEVICE_SEARCH_PARAMS *, BLUETOOTH_DEVICE_INFO *);
typedef BOOL(WINAPI *RFCOMM_FindNextDeviceFunc)(HBLUETOOTH_DEVICE_FIND, BLUETOOTH_DEVICE_INFO *);
typedef BOOL(WINAPI *RFCOMM_FindDeviceCloseFunc)(HBLUETOOTH_DEVICE_FIND);

typedef union RFCOMM_ModuleState
{
    SDL_MOGAState moga;
    SDL_ZeemoteState zeemote;
    SDL_BGP100State bgp100;
} RFCOMM_ModuleState;

typedef struct RFCOMM_Device
{
    SDL_RFCOMMDevice info;
    const SDL_RFCOMMModule *module;
    char path[24]; /* The address as 12 hex digits in six pairs, the joystick's path */
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    SDL_AtomicInt presence_changed;
    SDL_AtomicInt exited;
    HANDLE stop_event;   /* Manual reset */
    HANDLE wake_event;   /* Auto reset, for a new player index */
    HANDLE socket_event; /* Manual reset, as WSAEventSelect expects */

    /* The device thread's */
    SDL_RFCOMMLink link;
    RFCOMM_ModuleState state;
    SOCKET socket;
    bool present;
    uint32_t next_generation;
    int published_battery;
    int logged_error;

    /* Under the mutex */
    uint32_t generation; /* 0 while absent */
    SDL_SerialIdentity identity;
    SDL_SerialControls latest;
    uint64_t latest_sequence;
    int battery_mv;
    SDL_SerialSnapshotQueue queue;
    uint64_t sequence;
    int player_index;
    bool player_pending;

    /* The joystick thread's */
    bool stopping;
    uint32_t registered; /* The generation it has a joystick for, 0 when none */
    SDL_JoystickID instance_id;
    SDL_GUID guid;
    SDL_SerialIdentity joystick_identity;

    struct RFCOMM_Device *next;
} RFCOMM_Device;

struct joystick_hwdata
{
    RFCOMM_Device *device; /* NULL once the device has stopped */
    uint32_t generation;
    uint64_t sequence;  /* The last snapshot sent */
    bool battery_sent;
    bool send_initial;  /* The state Open took is not sent yet */
    Uint64 initial_stamp;
    SDL_SerialControls initial;
};

static const SDL_RFCOMMModule *const rfcomm_modules[SDL_RFCOMM_FAMILY_COUNT] = {
    NULL,
    &SDL_RFCOMMMogaModule,
    &SDL_RFCOMMZeemoteModule,
    &SDL_RFCOMMBGP100Module,
    &SDL_RFCOMMPhonejoyModule
};

static const char *const rfcomm_family_hints[SDL_RFCOMM_FAMILY_COUNT] = {
    NULL,
    SDL_HINT_JOYSTICK_RFCOMM_MOGA,
    SDL_HINT_JOYSTICK_RFCOMM_ZEEMOTE,
    SDL_HINT_JOYSTICK_RFCOMM_BGP100,
    SDL_HINT_JOYSTICK_RFCOMM_PHONEJOY
};

static bool rfcomm_initialized;
static SDL_AtomicInt rfcomm_hints_changed;
static RFCOMM_Device *rfcomm_devices;
static int rfcomm_logged_known = -1;
static int rfcomm_logged_selected = -1;

/* The discovery thread's list, under rfcomm_paired_lock */
static SDL_Mutex *rfcomm_paired_lock;
static SDL_RFCOMMPaired *rfcomm_paired;
static int rfcomm_npaired;
static SDL_AtomicInt rfcomm_paired_changed;
static SDL_Thread *rfcomm_discovery;
static HANDLE rfcomm_discovery_stop;

static HMODULE rfcomm_ws2;
static HMODULE rfcomm_bthprops;
static bool rfcomm_started;
static RFCOMM_WSAStartupFunc rfcomm_wsa_startup;
static RFCOMM_WSACleanupFunc rfcomm_wsa_cleanup;
static RFCOMM_SocketFunc rfcomm_socket;
static RFCOMM_CloseSocketFunc rfcomm_closesocket;
static RFCOMM_ConnectFunc rfcomm_connect;
static RFCOMM_SendFunc rfcomm_send;
static RFCOMM_RecvFunc rfcomm_recv;
static RFCOMM_WSAGetLastErrorFunc rfcomm_wsa_get_last_error;
static RFCOMM_WSAEventSelectFunc rfcomm_wsa_event_select;
static RFCOMM_WSAEnumNetworkEventsFunc rfcomm_wsa_enum_network_events;
static RFCOMM_FindFirstDeviceFunc rfcomm_find_first;
static RFCOMM_FindNextDeviceFunc rfcomm_find_next;
static RFCOMM_FindDeviceCloseFunc rfcomm_find_close;

static void RFCOMM_UnloadLibraries(void)
{
    if (rfcomm_started) {
        rfcomm_wsa_cleanup();
        rfcomm_started = false;
    }
    if (rfcomm_ws2) {
        FreeLibrary(rfcomm_ws2);
        rfcomm_ws2 = NULL;
    }
    if (rfcomm_bthprops) {
        FreeLibrary(rfcomm_bthprops);
        rfcomm_bthprops = NULL;
    }
}

static bool RFCOMM_LoadLibraries(void)
{
    WSADATA data;

    rfcomm_ws2 = LoadLibrary(TEXT("ws2_32.dll"));
    /* Microsoft names bthprops.cpl as the home of the device search */
    rfcomm_bthprops = LoadLibrary(TEXT("bthprops.cpl"));
    if (!rfcomm_ws2 || !rfcomm_bthprops) {
        RFCOMM_UnloadLibraries();
        return false;
    }
    rfcomm_wsa_startup = (RFCOMM_WSAStartupFunc)GetProcAddress(rfcomm_ws2, "WSAStartup");
    rfcomm_wsa_cleanup = (RFCOMM_WSACleanupFunc)GetProcAddress(rfcomm_ws2, "WSACleanup");
    rfcomm_socket = (RFCOMM_SocketFunc)GetProcAddress(rfcomm_ws2, "socket");
    rfcomm_closesocket = (RFCOMM_CloseSocketFunc)GetProcAddress(rfcomm_ws2, "closesocket");
    rfcomm_connect = (RFCOMM_ConnectFunc)GetProcAddress(rfcomm_ws2, "connect");
    rfcomm_send = (RFCOMM_SendFunc)GetProcAddress(rfcomm_ws2, "send");
    rfcomm_recv = (RFCOMM_RecvFunc)GetProcAddress(rfcomm_ws2, "recv");
    rfcomm_wsa_get_last_error = (RFCOMM_WSAGetLastErrorFunc)GetProcAddress(rfcomm_ws2, "WSAGetLastError");
    rfcomm_wsa_event_select = (RFCOMM_WSAEventSelectFunc)GetProcAddress(rfcomm_ws2, "WSAEventSelect");
    rfcomm_wsa_enum_network_events = (RFCOMM_WSAEnumNetworkEventsFunc)GetProcAddress(rfcomm_ws2, "WSAEnumNetworkEvents");
    rfcomm_find_first = (RFCOMM_FindFirstDeviceFunc)GetProcAddress(rfcomm_bthprops, "BluetoothFindFirstDevice");
    rfcomm_find_next = (RFCOMM_FindNextDeviceFunc)GetProcAddress(rfcomm_bthprops, "BluetoothFindNextDevice");
    rfcomm_find_close = (RFCOMM_FindDeviceCloseFunc)GetProcAddress(rfcomm_bthprops, "BluetoothFindDeviceClose");
    if (!rfcomm_wsa_startup || !rfcomm_wsa_cleanup || !rfcomm_socket || !rfcomm_closesocket || !rfcomm_connect ||
        !rfcomm_send || !rfcomm_recv || !rfcomm_wsa_get_last_error || !rfcomm_wsa_event_select ||
        !rfcomm_wsa_enum_network_events || !rfcomm_find_first || !rfcomm_find_next || !rfcomm_find_close) {
        RFCOMM_UnloadLibraries();
        return false;
    }
    if (rfcomm_wsa_startup(MAKEWORD(2, 2), &data) != 0) {
        RFCOMM_UnloadLibraries();
        return false;
    }
    rfcomm_started = true;
    return true;
}

/* The system's known devices, without an inquiry, up to max */
static int RFCOMM_ReadPaired(SDL_RFCOMMPaired *paired, int max)
{
    BLUETOOTH_DEVICE_SEARCH_PARAMS params;
    BLUETOOTH_DEVICE_INFO info;
    HBLUETOOTH_DEVICE_FIND find;
    int count = 0;

    SDL_zero(params);
    params.dwSize = sizeof(params);
    params.fReturnAuthenticated = TRUE;
    params.fReturnRemembered = TRUE;
    params.fReturnConnected = TRUE;
    params.fReturnUnknown = FALSE;
    params.fIssueInquiry = FALSE;
    params.hRadio = NULL;
    SDL_zero(info);
    info.dwSize = sizeof(info);
    find = rfcomm_find_first(&params, &info);
    if (!find) {
        return 0;
    }
    do {
        if (count < max) {
            char *name;

            info.szName[BLUETOOTH_MAX_NAME_SIZE - 1] = 0;
            name = WIN_StringToUTF8W(info.szName);
            SDL_zerop(&paired[count]);
            paired[count].address = info.Address.ullLong;
            /* A connection alone does not count, so the driver's own link
               cannot keep an unpaired device in the list */
            paired[count].paired = (info.fRemembered || info.fAuthenticated);
            if (name) {
                SDL_utf8strlcpy(paired[count].name, name, sizeof(paired[count].name));
                SDL_free(name);
            }
            ++count;
        }
        SDL_zero(info);
        info.dwSize = sizeof(info);
    } while (rfcomm_find_next(find, &info));
    rfcomm_find_close(find);
    return count;
}

static int SDLCALL RFCOMM_DiscoveryThread(void *data)
{
    SDL_RFCOMMPaired *found = (SDL_RFCOMMPaired *)SDL_calloc(RFCOMM_MAX_PAIRED, sizeof(*found));

    (void)data;
    if (!found) {
        return 0;
    }
    for (;;) {
        const int count = RFCOMM_ReadPaired(found, RFCOMM_MAX_PAIRED);

        SDL_LockMutex(rfcomm_paired_lock);
        if (count != rfcomm_npaired || SDL_memcmp(found, rfcomm_paired, count * sizeof(*found)) != 0) {
            SDL_memcpy(rfcomm_paired, found, count * sizeof(*found));
            rfcomm_npaired = count;
            SDL_SetAtomicInt(&rfcomm_paired_changed, 1);
        }
        SDL_UnlockMutex(rfcomm_paired_lock);
        if (WaitForSingleObject(rfcomm_discovery_stop, SDL_RFCOMM_DISCOVERY_MS) != WAIT_TIMEOUT) {
            break;
        }
    }
    SDL_free(found);
    return 0;
}

static void RFCOMM_StartDiscovery(void)
{
    if (rfcomm_discovery || !rfcomm_initialized) {
        return;
    }
    ResetEvent(rfcomm_discovery_stop);
    rfcomm_discovery = SDL_CreateThread(RFCOMM_DiscoveryThread, "SDLRFCOMMDiscovery", NULL);
    if (rfcomm_discovery) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "RFCOMM: reading the paired Bluetooth devices");
    }
}

static void RFCOMM_StopDiscovery(void)
{
    if (!rfcomm_discovery) {
        return;
    }
    SetEvent(rfcomm_discovery_stop);
    SDL_WaitThread(rfcomm_discovery, NULL);
    rfcomm_discovery = NULL;
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "RFCOMM: stopped reading the paired Bluetooth devices");
    SDL_LockMutex(rfcomm_paired_lock);
    rfcomm_npaired = 0;
    SDL_UnlockMutex(rfcomm_paired_lock);
    SDL_SetAtomicInt(&rfcomm_paired_changed, 1);
}

/* A failing call is logged once until the error changes */
static void RFCOMM_LogError(RFCOMM_Device *device, const char *call, int error)
{
    if (device->logged_error == error) {
        return;
    }
    device->logged_error = error;
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "RFCOMM %s (%s): %s failed with error %d", device->path, device->info.name, call, error);
}

/* The module's snapshots, in order, on the device thread */
static void RFCOMM_Changed(void *userdata, int sub, const SDL_SerialSnapshot *snapshot)
{
    RFCOMM_Device *device = (RFCOMM_Device *)userdata;
    SDL_SerialQueueEntry entry;

    if (sub != 0) {
        return;
    }
    if (!snapshot->present) {
        if (device->present) {
            device->present = false;
            SDL_LockMutex(device->mutex);
            device->generation = 0;
            SDL_zero(device->latest);
            SDL_UnlockMutex(device->mutex);
            SDL_SetAtomicInt(&device->presence_changed, 1);
        }
        return;
    }
    if (!device->present) {
        device->present = true;
        if (++device->next_generation == 0) {
            device->next_generation = 1;
        }
        SDL_LockMutex(device->mutex);
        device->generation = device->next_generation;
        device->identity = snapshot->identity;
        SDL_zero(device->latest);
        SDL_UnlockMutex(device->mutex);
        SDL_SetAtomicInt(&device->presence_changed, 1);
    }
    SDL_zero(entry);
    entry.sub = 0;
    entry.generation = device->next_generation;
    entry.stamp_ns = SDL_GetTicksNS();
    entry.controls = snapshot->controls;
    SDL_LockMutex(device->mutex);
    entry.sequence = ++device->sequence;
    SDL_Serial_PushQueue(&device->queue, &entry);
    device->latest = entry.controls;
    device->latest_sequence = entry.sequence;
    SDL_UnlockMutex(device->mutex);
}

static void RFCOMM_CloseSocket(RFCOMM_Device *device)
{
    if (device->socket != INVALID_SOCKET) {
        rfcomm_closesocket(device->socket);
        device->socket = INVALID_SOCKET;
    }
    ResetEvent(device->socket_event);
}

static SDL_RFCOMMConnectResult RFCOMM_ConnectResult(int error)
{
    /* WSAETIMEDOUT is the page timeout: the device is away */
    return (error == WSAETIMEDOUT) ? SDL_RFCOMM_CONNECT_TIMED_OUT : SDL_RFCOMM_CONNECT_FAILED;
}

/* The service UUID as written, most significant byte first, as a GUID */
static GUID RFCOMM_ServiceGUID(const uint8_t *uuid)
{
    GUID guid;
    int i;

    guid.Data1 = ((DWORD)uuid[0] << 24) | ((DWORD)uuid[1] << 16) | ((DWORD)uuid[2] << 8) | uuid[3];
    guid.Data2 = (WORD)((uuid[4] << 8) | uuid[5]);
    guid.Data3 = (WORD)((uuid[6] << 8) | uuid[7]);
    for (i = 0; i < 8; ++i) {
        guid.Data4[i] = uuid[8 + i];
    }
    return guid;
}

/* A connect that does not block: the socket is non-blocking once
   WSAEventSelect runs, and FD_CONNECT reports the result */
static void RFCOMM_StartConnect(RFCOMM_Device *device, uint8_t channel)
{
    SOCKADDR_BTH address;
    SOCKET sock;
    int error;

    RFCOMM_CloseSocket(device);
    sock = rfcomm_socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (sock == INVALID_SOCKET) {
        error = rfcomm_wsa_get_last_error();
        RFCOMM_LogError(device, "socket", error);
        SDL_RFCOMMLink_Connected(&device->link, SDL_RFCOMM_CONNECT_FAILED, SDL_GetTicks());
        return;
    }
    if (rfcomm_wsa_event_select(sock, device->socket_event, FD_CONNECT | FD_READ | FD_CLOSE) != 0) {
        error = rfcomm_wsa_get_last_error();
        RFCOMM_LogError(device, "WSAEventSelect", error);
        rfcomm_closesocket(sock);
        SDL_RFCOMMLink_Connected(&device->link, SDL_RFCOMM_CONNECT_FAILED, SDL_GetTicks());
        return;
    }
    device->socket = sock;

    SDL_zero(address);
    address.addressFamily = AF_BTH;
    address.btAddr = device->info.address;
    if (channel == 0) {
        /* Port 0: the stack finds the channel by the service class */
        address.serviceClassId = RFCOMM_ServiceGUID(device->module->service);
    }
    address.port = channel;
    if (rfcomm_connect(sock, (const struct sockaddr *)&address, sizeof(address)) == 0) {
        SDL_RFCOMMLink_Connected(&device->link, SDL_RFCOMM_CONNECT_OK, SDL_GetTicks());
        return;
    }
    error = rfcomm_wsa_get_last_error();
    if (error != WSAEWOULDBLOCK) {
        RFCOMM_LogError(device, "connect", error);
        RFCOMM_CloseSocket(device);
        SDL_RFCOMMLink_Connected(&device->link, RFCOMM_ConnectResult(error), SDL_GetTicks());
    }
}

/* Takes what arrived until the socket has nothing more. A close or an error
   is loss. */
static void RFCOMM_Read(RFCOMM_Device *device)
{
    char buffer[RFCOMM_READ_SIZE];

    while (device->socket != INVALID_SOCKET) {
        const int received = rfcomm_recv(device->socket, buffer, (int)sizeof(buffer), 0);

        if (received > 0) {
            SDL_RFCOMMLink_Received(&device->link, (const uint8_t *)buffer, (size_t)received, SDL_GetTicks());
            continue;
        }
        if (received < 0 && rfcomm_wsa_get_last_error() == WSAEWOULDBLOCK) {
            return;
        }
        if (received < 0) {
            RFCOMM_LogError(device, "recv", rfcomm_wsa_get_last_error());
        }
        RFCOMM_CloseSocket(device);
        SDL_RFCOMMLink_Lost(&device->link, SDL_GetTicks());
        return;
    }
}

static void RFCOMM_SocketEvents(RFCOMM_Device *device)
{
    WSANETWORKEVENTS events;

    if (device->socket == INVALID_SOCKET) {
        ResetEvent(device->socket_event);
        return;
    }
    /* This also resets the event */
    if (rfcomm_wsa_enum_network_events(device->socket, device->socket_event, &events) != 0) {
        RFCOMM_LogError(device, "WSAEnumNetworkEvents", rfcomm_wsa_get_last_error());
        RFCOMM_CloseSocket(device);
        SDL_RFCOMMLink_Lost(&device->link, SDL_GetTicks());
        return;
    }
    if (events.lNetworkEvents & FD_CONNECT) {
        const int error = events.iErrorCode[FD_CONNECT_BIT];

        if (error != 0) {
            RFCOMM_LogError(device, "connect", error);
            RFCOMM_CloseSocket(device);
            SDL_RFCOMMLink_Connected(&device->link, RFCOMM_ConnectResult(error), SDL_GetTicks());
            return;
        }
        device->logged_error = 0;
        SDL_RFCOMMLink_Connected(&device->link, SDL_RFCOMM_CONNECT_OK, SDL_GetTicks());
    }
    if (events.lNetworkEvents & (FD_READ | FD_CLOSE)) {
        RFCOMM_Read(device);
    }
    if ((events.lNetworkEvents & FD_CLOSE) && device->socket != INVALID_SOCKET) {
        RFCOMM_CloseSocket(device);
        SDL_RFCOMMLink_Lost(&device->link, SDL_GetTicks());
    }
}

/* Runs the link's actions, which can queue more */
static void RFCOMM_RunActions(RFCOMM_Device *device)
{
    SDL_RFCOMMAction action;
    int count = 0;

    while (count++ < RFCOMM_MAX_ACTIONS && SDL_RFCOMMLink_NextAction(&device->link, &action)) {
        switch (action.kind) {
        case SDL_RFCOMM_ACTION_CONNECT:
            RFCOMM_StartConnect(device, action.channel);
            break;
        case SDL_RFCOMM_ACTION_CLOSE:
            RFCOMM_CloseSocket(device);
            break;
        case SDL_RFCOMM_ACTION_SEND:
            if (device->socket != INVALID_SOCKET &&
                rfcomm_send(device->socket, (const char *)action.data, (int)action.length, 0) != (int)action.length) {
                RFCOMM_LogError(device, "send", rfcomm_wsa_get_last_error());
                RFCOMM_CloseSocket(device);
                SDL_RFCOMMLink_Lost(&device->link, SDL_GetTicks());
            }
            break;
        }
    }
}

static void RFCOMM_ApplyPlayer(RFCOMM_Device *device)
{
    bool pending;
    int player_index;

    SDL_LockMutex(device->mutex);
    pending = device->player_pending;
    player_index = device->player_index;
    device->player_pending = false;
    SDL_UnlockMutex(device->mutex);
    if (pending) {
        SDL_RFCOMMLink_SetPlayerIndex(&device->link, player_index, SDL_GetTicks());
    }
}

static void RFCOMM_PublishBattery(RFCOMM_Device *device)
{
    const int battery = ((const SDL_RFCOMMBase *)&device->state)->battery_mv;

    if (battery != device->published_battery) {
        device->published_battery = battery;
        SDL_LockMutex(device->mutex);
        device->battery_mv = battery;
        SDL_UnlockMutex(device->mutex);
    }
}

/* Only this thread touches the socket and the link */
static int SDLCALL RFCOMM_DeviceThread(void *data)
{
    RFCOMM_Device *device = (RFCOMM_Device *)data;
    SDL_SerialSink sink;

    sink.userdata = device;
    sink.changed = RFCOMM_Changed;
    sink.log = NULL;
    SDL_RFCOMMLink_Init(&device->link, device->module, &device->state, &sink, device->info.name, SDL_GetTicks());
    for (;;) {
        HANDLE handles[3];
        DWORD count = 0, timeout = INFINITE, result;
        uint64_t deadline;

        RFCOMM_ApplyPlayer(device);
        if (SDL_RFCOMMLink_GetDeadline(&device->link, &deadline) && SDL_GetTicks() >= deadline) {
            SDL_RFCOMMLink_Tick(&device->link, SDL_GetTicks());
        }
        RFCOMM_RunActions(device);
        RFCOMM_PublishBattery(device);
        if (SDL_RFCOMMLink_GetDeadline(&device->link, &deadline)) {
            const Uint64 now = SDL_GetTicks();

            timeout = (deadline <= now) ? 0 : (DWORD)SDL_min(deadline - now, (Uint64)0x7FFFFFFF);
        }
        handles[count++] = device->stop_event;
        handles[count++] = device->wake_event;
        if (device->socket != INVALID_SOCKET) {
            handles[count++] = device->socket_event;
        }
        result = WaitForMultipleObjects(count, handles, FALSE, timeout);
        if (result == WAIT_TIMEOUT || result == WAIT_OBJECT_0 + 1) {
            continue;
        }
        if (result == WAIT_OBJECT_0) {
            break;
        }
        if (result == WAIT_OBJECT_0 + 2) {
            RFCOMM_SocketEvents(device);
            continue;
        }
        RFCOMM_LogError(device, "WaitForMultipleObjects", (int)GetLastError());
        SDL_Delay(100);
    }
    SDL_RFCOMMLink_Stop(&device->link);
    RFCOMM_CloseSocket(device);
    SDL_SetAtomicInt(&device->exited, 1);
    return 0;
}

static void RFCOMM_FreeDevice(RFCOMM_Device *device)
{
    if (device->socket_event) {
        CloseHandle(device->socket_event);
    }
    if (device->wake_event) {
        CloseHandle(device->wake_event);
    }
    if (device->stop_event) {
        CloseHandle(device->stop_event);
    }
    if (device->mutex) {
        SDL_DestroyMutex(device->mutex);
    }
    SDL_free(device);
}

static void RFCOMM_StartDevice(const SDL_RFCOMMDevice *info)
{
    RFCOMM_Device *device = (RFCOMM_Device *)SDL_calloc(1, sizeof(*device));
    RFCOMM_Device **tail;
    const uint64_t a = info->address;

    if (!device) {
        return;
    }
    device->info = *info;
    device->module = rfcomm_modules[info->family];
    device->socket = INVALID_SOCKET;
    device->published_battery = -1;
    device->battery_mv = -1;
    device->player_index = -1;
    (void)SDL_snprintf(device->path, sizeof(device->path), "%02X:%02X:%02X:%02X:%02X:%02X",
                       (unsigned)((a >> 40) & 0xFF), (unsigned)((a >> 32) & 0xFF), (unsigned)((a >> 24) & 0xFF),
                       (unsigned)((a >> 16) & 0xFF), (unsigned)((a >> 8) & 0xFF), (unsigned)(a & 0xFF));
    device->mutex = SDL_CreateMutex();
    device->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    device->wake_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    device->socket_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!device->module || !device->mutex || !device->stop_event || !device->wake_event || !device->socket_event) {
        RFCOMM_FreeDevice(device);
        return;
    }
    SDL_Serial_ClearQueue(&device->queue);
    device->thread = SDL_CreateThread(RFCOMM_DeviceThread, "SDLRFCOMMDevice", device);
    if (!device->thread) {
        RFCOMM_FreeDevice(device);
        return;
    }
    for (tail = &rfcomm_devices; *tail; tail = &(*tail)->next) {
    }
    *tail = device;
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "RFCOMM %s (%s): connecting", device->path, device->info.name);
}

static void RFCOMM_RemoveJoystick(RFCOMM_Device *device, bool notify)
{
    if (device->registered) {
        SDL_Joystick *joystick = SDL_GetJoystickFromID(device->instance_id);

        if (joystick && joystick->hwdata) {
            joystick->hwdata->device = NULL;
        }
        if (notify) {
            SDL_PrivateJoystickRemoved(device->instance_id);
        }
        device->registered = 0;
    }
}

/* Removes the device's joystick and asks its thread to end. The record is
   freed once the thread has ended, so the joystick thread never waits on a
   connect. */
static void RFCOMM_StopDevice(RFCOMM_Device *device, bool notify)
{
    RFCOMM_RemoveJoystick(device, notify);
    device->stopping = true;
    SetEvent(device->stop_event);
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "RFCOMM %s (%s): stopped", device->path, device->info.name);
}

static void RFCOMM_UnlinkDevice(RFCOMM_Device *device)
{
    RFCOMM_Device **link;

    for (link = &rfcomm_devices; *link; link = &(*link)->next) {
        if (*link == device) {
            *link = device->next;
            break;
        }
    }
}

/* Frees the stopped devices whose thread has ended, or all of them */
static void RFCOMM_ReapDevices(bool wait)
{
    RFCOMM_Device *device, *next;

    for (device = rfcomm_devices; device; device = next) {
        next = device->next;
        if (!device->stopping || (!wait && !SDL_GetAtomicInt(&device->exited))) {
            continue;
        }
        SDL_WaitThread(device->thread, NULL);
        device->thread = NULL;
        RFCOMM_UnlinkDevice(device);
        RFCOMM_FreeDevice(device);
    }
}

/* Each family's hint defaults to SDL_HINT_JOYSTICK_RFCOMM, as the HIDAPI
   drivers' hints default to SDL_HINT_JOYSTICK_HIDAPI. True when any family
   is on. */
static bool RFCOMM_GetEnabled(bool *enabled)
{
    const bool all = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_RFCOMM, true);
    bool any = false;
    int family;

    enabled[SDL_RFCOMM_FAMILY_NONE] = false;
    for (family = 1; family < SDL_RFCOMM_FAMILY_COUNT; ++family) {
        enabled[family] = SDL_GetHintBoolean(rfcomm_family_hints[family], all);
        any = any || enabled[family];
    }
    return any;
}

/* The paired devices whose family is on, against the running devices */
static void RFCOMM_ApplyDevices(void)
{
    SDL_RFCOMMPaired *paired = (SDL_RFCOMMPaired *)SDL_calloc(RFCOMM_MAX_PAIRED, sizeof(*paired));
    SDL_RFCOMMDevice selected[SDL_RFCOMM_MAX_DEVICES], running[SDL_RFCOMM_MAX_DEVICES];
    RFCOMM_Device *devices[SDL_RFCOMM_MAX_DEVICES];
    SDL_RFCOMMChange old_changes[SDL_RFCOMM_MAX_DEVICES], new_changes[SDL_RFCOMM_MAX_DEVICES];
    bool enabled[SDL_RFCOMM_FAMILY_COUNT];
    RFCOMM_Device *device;
    int npaired, nselected, nrunning = 0, i;

    if (!paired) {
        return;
    }
    SDL_LockMutex(rfcomm_paired_lock);
    npaired = rfcomm_npaired;
    SDL_memcpy(paired, rfcomm_paired, npaired * sizeof(*paired));
    SDL_UnlockMutex(rfcomm_paired_lock);

    RFCOMM_GetEnabled(enabled);
    nselected = SDL_RFCOMM_SelectDevices(paired, npaired, enabled, selected, SDL_RFCOMM_MAX_DEVICES);
    SDL_free(paired);
    /* Counts only: the names of other devices stay out of the log */
    if (npaired != rfcomm_logged_known || nselected != rfcomm_logged_selected) {
        rfcomm_logged_known = npaired;
        rfcomm_logged_selected = nselected;
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "RFCOMM: %d known Bluetooth devices, %d controllers to run", npaired, nselected);
    }

    for (device = rfcomm_devices; device && nrunning < SDL_RFCOMM_MAX_DEVICES; device = device->next) {
        if (!device->stopping) {
            devices[nrunning] = device;
            running[nrunning++] = device->info;
        }
    }
    SDL_RFCOMM_DiffDevices(running, nrunning, selected, nselected, old_changes, new_changes);
    for (i = 0; i < nrunning; ++i) {
        if (old_changes[i] == SDL_RFCOMM_STOP) {
            RFCOMM_StopDevice(devices[i], true);
        }
    }
    for (i = 0; i < nselected; ++i) {
        if (new_changes[i] == SDL_RFCOMM_START) {
            RFCOMM_StartDevice(&selected[i]);
        }
    }
}

static void RFCOMM_CheckPresence(RFCOMM_Device *device)
{
    SDL_SerialIdentity identity;
    uint32_t generation;

    SDL_LockMutex(device->mutex);
    generation = device->generation;
    identity = device->identity;
    SDL_UnlockMutex(device->mutex);

    if (device->registered && device->registered != generation) {
        RFCOMM_RemoveJoystick(device, true);
    }
    if (!device->registered && generation) {
        device->registered = generation;
        device->joystick_identity = identity;
        device->instance_id = SDL_GetNextObjectID();
        /* Vendor 0 puts the name in the GUID. 'b' is this driver's. */
        device->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, 0, 0, 0, NULL, identity.name, 'b', 0);
        SDL_PrivateJoystickAdded(device->instance_id);
    }
}

static RFCOMM_Device *RFCOMM_GetDevice(int device_index)
{
    RFCOMM_Device *device;

    if (device_index < 0) {
        return NULL;
    }
    for (device = rfcomm_devices; device; device = device->next) {
        if (device->registered && device_index-- == 0) {
            return device;
        }
    }
    return NULL;
}

static void SDLCALL RFCOMM_HintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    (void)userdata;
    (void)name;
    (void)oldValue;
    (void)hint;
    SDL_SetAtomicInt(&rfcomm_hints_changed, 1);
}

static bool RFCOMM_JoystickInit(void)
{
    int family;

    if (rfcomm_initialized) {
        return true;
    }
    /* Without Winsock or the Bluetooth device search the driver stays empty */
    if (!RFCOMM_LoadLibraries()) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "RFCOMM: ws2_32.dll or bthprops.cpl could not be loaded");
        return false;
    }
    rfcomm_paired_lock = SDL_CreateMutex();
    rfcomm_paired = (SDL_RFCOMMPaired *)SDL_calloc(RFCOMM_MAX_PAIRED, sizeof(*rfcomm_paired));
    rfcomm_discovery_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!rfcomm_paired_lock || !rfcomm_paired || !rfcomm_discovery_stop) {
        if (rfcomm_discovery_stop) {
            CloseHandle(rfcomm_discovery_stop);
            rfcomm_discovery_stop = NULL;
        }
        SDL_free(rfcomm_paired);
        rfcomm_paired = NULL;
        if (rfcomm_paired_lock) {
            SDL_DestroyMutex(rfcomm_paired_lock);
            rfcomm_paired_lock = NULL;
        }
        RFCOMM_UnloadLibraries();
        return false;
    }
    rfcomm_npaired = 0;
    rfcomm_initialized = true;
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_RFCOMM, RFCOMM_HintChanged, NULL);
    for (family = 1; family < SDL_RFCOMM_FAMILY_COUNT; ++family) {
        SDL_AddHintCallback(rfcomm_family_hints[family], RFCOMM_HintChanged, NULL);
    }
    /* The callbacks run at once, so Detect applies the hints first */
    SDL_SetAtomicInt(&rfcomm_hints_changed, 1);
    return true;
}

static int RFCOMM_JoystickGetCount(void)
{
    RFCOMM_Device *device;
    int count = 0;

    for (device = rfcomm_devices; device; device = device->next) {
        if (device->registered) {
            ++count;
        }
    }
    return count;
}

static void RFCOMM_JoystickDetect(void)
{
    RFCOMM_Device *device;
    bool apply = false;

    if (!rfcomm_initialized) {
        return;
    }
    if (SDL_GetAtomicInt(&rfcomm_hints_changed)) {
        bool enabled[SDL_RFCOMM_FAMILY_COUNT];

        SDL_SetAtomicInt(&rfcomm_hints_changed, 0);
        /* The paired list is read only while a family is on */
        if (RFCOMM_GetEnabled(enabled)) {
            RFCOMM_StartDiscovery();
        } else {
            RFCOMM_StopDiscovery();
        }
        apply = true;
    }
    if (SDL_GetAtomicInt(&rfcomm_paired_changed)) {
        SDL_SetAtomicInt(&rfcomm_paired_changed, 0);
        apply = true;
    }
    if (apply) {
        RFCOMM_ApplyDevices();
    }
    for (device = rfcomm_devices; device; device = device->next) {
        if (!device->stopping && SDL_GetAtomicInt(&device->presence_changed)) {
            SDL_SetAtomicInt(&device->presence_changed, 0);
            RFCOMM_CheckPresence(device);
        }
    }
    RFCOMM_ReapDevices(false);
}

/* A Bluetooth serial device is no other driver's device */
static bool RFCOMM_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    (void)vendor_id;
    (void)product_id;
    (void)version;
    (void)name;
    return false;
}

static const char *RFCOMM_JoystickGetDeviceName(int device_index)
{
    RFCOMM_Device *device = RFCOMM_GetDevice(device_index);

    return device ? device->joystick_identity.name : NULL;
}

static const char *RFCOMM_JoystickGetDevicePath(int device_index)
{
    RFCOMM_Device *device = RFCOMM_GetDevice(device_index);

    return device ? device->path : NULL;
}

static int RFCOMM_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    (void)device_index;
    return -1;
}

static int RFCOMM_JoystickGetDevicePlayerIndex(int device_index)
{
    (void)device_index;
    return -1;
}

/* The MOGA lights the LED of the new id */
static void RFCOMM_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
    RFCOMM_Device *device = RFCOMM_GetDevice(device_index);

    if (!device) {
        return;
    }
    SDL_LockMutex(device->mutex);
    device->player_index = player_index;
    device->player_pending = true;
    SDL_UnlockMutex(device->mutex);
    SetEvent(device->wake_event);
}

static SDL_GUID RFCOMM_JoystickGetDeviceGUID(int device_index)
{
    RFCOMM_Device *device = RFCOMM_GetDevice(device_index);
    SDL_GUID guid;

    if (!device) {
        SDL_zero(guid);
        return guid;
    }
    return device->guid;
}

static SDL_JoystickID RFCOMM_JoystickGetDeviceInstanceID(int device_index)
{
    RFCOMM_Device *device = RFCOMM_GetDevice(device_index);

    return device ? device->instance_id : 0;
}

static void RFCOMM_SendControls(SDL_Joystick *joystick, const SDL_SerialControls *controls, Uint64 timestamp)
{
    int i;

    for (i = 0; i < joystick->naxes; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, controls->axes[i]);
    }
    for (i = 0; i < joystick->nbuttons; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, SDL_Serial_GetButton(controls, i));
    }
}

static bool RFCOMM_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    RFCOMM_Device *device = RFCOMM_GetDevice(device_index);
    struct joystick_hwdata *hwdata;

    if (!device) {
        return SDL_SetError("RFCOMM device index out of range");
    }
    hwdata = (struct joystick_hwdata *)SDL_calloc(1, sizeof(*hwdata));
    if (!hwdata) {
        return false;
    }
    hwdata->device = device;
    hwdata->generation = device->registered;
    joystick->hwdata = hwdata;
    joystick->naxes = device->joystick_identity.naxes;
    joystick->nbuttons = device->joystick_identity.nbuttons;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;

    /* The state so far, sent by the first update, as SDL allocates the
       joystick's axes and buttons only after Open returns. Queued snapshots
       up to it are skipped. */
    SDL_LockMutex(device->mutex);
    hwdata->initial = device->latest;
    hwdata->sequence = device->latest_sequence;
    SDL_UnlockMutex(device->mutex);
    hwdata->initial_stamp = SDL_GetTicksNS();
    hwdata->send_initial = true;
    return true;
}

static bool RFCOMM_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    (void)joystick;
    (void)low_frequency_rumble;
    (void)high_frequency_rumble;
    return SDL_Unsupported();
}

static bool RFCOMM_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    (void)joystick;
    (void)left_rumble;
    (void)right_rumble;
    return SDL_Unsupported();
}

static bool RFCOMM_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)joystick;
    (void)red;
    (void)green;
    (void)blue;
    return SDL_Unsupported();
}

static bool RFCOMM_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    (void)joystick;
    (void)data;
    (void)size;
    return SDL_Unsupported();
}

static bool RFCOMM_JoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    (void)joystick;
    (void)enabled;
    return SDL_Unsupported();
}

/* Drains the device's snapshot queue in order */
static void RFCOMM_JoystickUpdate(SDL_Joystick *joystick)
{
    struct joystick_hwdata *hwdata = joystick->hwdata;
    SDL_SerialQueueEntry entries[SDL_SERIAL_QUEUE_ENTRIES];
    RFCOMM_Device *device;
    int count = 0, battery, i;

    if (!hwdata || !hwdata->device) {
        return;
    }
    if (hwdata->send_initial) {
        hwdata->send_initial = false;
        RFCOMM_SendControls(joystick, &hwdata->initial, hwdata->initial_stamp);
    }
    device = hwdata->device;
    SDL_LockMutex(device->mutex);
    while (count < SDL_SERIAL_QUEUE_ENTRIES && SDL_Serial_PopQueue(&device->queue, &entries[count])) {
        ++count;
    }
    battery = device->battery_mv;
    SDL_UnlockMutex(device->mutex);
    for (i = 0; i < count; ++i) {
        const SDL_SerialQueueEntry *entry = &entries[i];

        if (entry->generation != hwdata->generation || entry->sequence <= hwdata->sequence) {
            continue;
        }
        hwdata->sequence = entry->sequence;
        RFCOMM_SendControls(joystick, &entry->controls, entry->stamp_ns);
    }
    /* The Zeemote reports millivolts, and no source gives a charge curve */
    if (battery >= 0 && !hwdata->battery_sent) {
        hwdata->battery_sent = true;
        SDL_SendJoystickPowerInfo(joystick, SDL_POWERSTATE_ON_BATTERY, -1);
    }
}

static void RFCOMM_JoystickClose(SDL_Joystick *joystick)
{
    SDL_free(joystick->hwdata);
    joystick->hwdata = NULL;
}

static void RFCOMM_JoystickQuit(void)
{
    RFCOMM_Device *device;
    int family;

    if (!rfcomm_initialized) {
        return;
    }
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_RFCOMM, RFCOMM_HintChanged, NULL);
    for (family = 1; family < SDL_RFCOMM_FAMILY_COUNT; ++family) {
        SDL_RemoveHintCallback(rfcomm_family_hints[family], RFCOMM_HintChanged, NULL);
    }
    RFCOMM_StopDiscovery();
    /* Every thread is told first, so the waits overlap */
    for (device = rfcomm_devices; device; device = device->next) {
        if (!device->stopping) {
            RFCOMM_StopDevice(device, false);
        }
    }
    RFCOMM_ReapDevices(true);
    CloseHandle(rfcomm_discovery_stop);
    rfcomm_discovery_stop = NULL;
    SDL_free(rfcomm_paired);
    rfcomm_paired = NULL;
    rfcomm_npaired = 0;
    SDL_DestroyMutex(rfcomm_paired_lock);
    rfcomm_paired_lock = NULL;
    RFCOMM_UnloadLibraries();
    SDL_SetAtomicInt(&rfcomm_hints_changed, 0);
    SDL_SetAtomicInt(&rfcomm_paired_changed, 0);
    rfcomm_logged_known = -1;
    rfcomm_logged_selected = -1;
    rfcomm_initialized = false;
}

static void RFCOMM_MapInput(SDL_InputMapping *out, const SDL_SerialMapInput *in)
{
    SDL_zerop(out);
    switch (in->kind) {
    case SDL_SERIAL_MAP_BUTTON:
        out->kind = EMappingKind_Button;
        break;
    case SDL_SERIAL_MAP_AXIS:
        out->kind = EMappingKind_Axis;
        break;
    case SDL_SERIAL_MAP_AXIS_POSITIVE:
        out->kind = EMappingKind_Axis;
        out->half_axis_positive = true;
        break;
    case SDL_SERIAL_MAP_AXIS_NEGATIVE:
        out->kind = EMappingKind_Axis;
        out->half_axis_negative = true;
        break;
    default:
        out->kind = EMappingKind_None;
        return;
    }
    out->target = in->target;
}

static bool RFCOMM_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    RFCOMM_Device *device = RFCOMM_GetDevice(device_index);
    const SDL_SerialGamepadMap *map;

    if (!device || !device->joystick_identity.has_mapping) {
        return false;
    }
    map = &device->joystick_identity.mapping;
    SDL_zerop(out);
    RFCOMM_MapInput(&out->a, &map->a);
    RFCOMM_MapInput(&out->b, &map->b);
    RFCOMM_MapInput(&out->x, &map->x);
    RFCOMM_MapInput(&out->y, &map->y);
    RFCOMM_MapInput(&out->back, &map->back);
    RFCOMM_MapInput(&out->guide, &map->guide);
    RFCOMM_MapInput(&out->start, &map->start);
    RFCOMM_MapInput(&out->leftstick, &map->leftstick);
    RFCOMM_MapInput(&out->rightstick, &map->rightstick);
    RFCOMM_MapInput(&out->leftshoulder, &map->leftshoulder);
    RFCOMM_MapInput(&out->rightshoulder, &map->rightshoulder);
    RFCOMM_MapInput(&out->dpup, &map->dpup);
    RFCOMM_MapInput(&out->dpdown, &map->dpdown);
    RFCOMM_MapInput(&out->dpleft, &map->dpleft);
    RFCOMM_MapInput(&out->dpright, &map->dpright);
    RFCOMM_MapInput(&out->misc1, &map->misc1);
    RFCOMM_MapInput(&out->leftx, &map->leftx);
    RFCOMM_MapInput(&out->lefty, &map->lefty);
    RFCOMM_MapInput(&out->rightx, &map->rightx);
    RFCOMM_MapInput(&out->righty, &map->righty);
    RFCOMM_MapInput(&out->lefttrigger, &map->lefttrigger);
    RFCOMM_MapInput(&out->righttrigger, &map->righttrigger);
    return true;
}

SDL_JoystickDriver SDL_RFCOMM_JoystickDriver = {
    RFCOMM_JoystickInit,
    RFCOMM_JoystickGetCount,
    RFCOMM_JoystickDetect,
    RFCOMM_JoystickIsDevicePresent,
    RFCOMM_JoystickGetDeviceName,
    RFCOMM_JoystickGetDevicePath,
    RFCOMM_JoystickGetDeviceSteamVirtualGamepadSlot,
    RFCOMM_JoystickGetDevicePlayerIndex,
    RFCOMM_JoystickSetDevicePlayerIndex,
    RFCOMM_JoystickGetDeviceGUID,
    RFCOMM_JoystickGetDeviceInstanceID,
    RFCOMM_JoystickOpen,
    RFCOMM_JoystickRumble,
    RFCOMM_JoystickRumbleTriggers,
    RFCOMM_JoystickSetLED,
    RFCOMM_JoystickSendEffect,
    RFCOMM_JoystickSetSensorsEnabled,
    RFCOMM_JoystickUpdate,
    RFCOMM_JoystickClose,
    RFCOMM_JoystickQuit,
    RFCOMM_JoystickGetGamepadMapping
};

#endif /* SDL_JOYSTICK_RFCOMM */
