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
 * PadForge fork: DJI screen remotes over TCP port 40007, the DJI RC and the
 * DJI RC 2 on firmware that still opens the port. See hifihedgehog/SDL#33
 * Part 6.
 *
 * The application names each remote in SDL_HINT_JOYSTICK_DJI_REMOTE_TCP_HOSTS,
 * so SDL opens no connection unless the hint names one. Each host gets a
 * thread that owns its sockets and runs the pure session in
 * src/joystick/dji/SDL_dji_tcp_proto.c, which decides every connect, send
 * and close. The thread hands presence and snapshots to the joystick thread
 * under the host's mutex, as the serial driver does, and never takes the
 * joystick lock. ws2_32 is loaded at run time, so SDL links no Winsock
 * library.
 */

#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_DJI_TCP

#include "../SDL_sysjoystick.h"
#include "../../core/windows/SDL_windows.h"
#include "../dji/SDL_dji_tcp_proto.h"
#include "../serial/SDL_serial_engine.h"

#include <winsock2.h>

#ifndef TCP_NODELAY
#define TCP_NODELAY 0x0001
#endif

#define DJITCP_READ_SIZE   2048
#define DJITCP_MAX_ACTIONS 256 /* Actions one pass may run, a guard only */

typedef int(WSAAPI *DJITCP_WSAStartup)(WORD, LPWSADATA);
typedef int(WSAAPI *DJITCP_WSACleanup)(void);
typedef SOCKET(WSAAPI *DJITCP_Socket)(int, int, int);
typedef int(WSAAPI *DJITCP_CloseSocket)(SOCKET);
typedef int(WSAAPI *DJITCP_Connect)(SOCKET, const struct sockaddr *, int);
typedef int(WSAAPI *DJITCP_Send)(SOCKET, const char *, int, int);
typedef int(WSAAPI *DJITCP_Recv)(SOCKET, char *, int, int);
typedef int(WSAAPI *DJITCP_SetSockOpt)(SOCKET, int, int, const char *, int);
typedef int(WSAAPI *DJITCP_WSAGetLastError)(void);
typedef int(WSAAPI *DJITCP_WSAEventSelect)(SOCKET, WSAEVENT, long);
typedef int(WSAAPI *DJITCP_WSAEnumNetworkEvents)(SOCKET, WSAEVENT, LPWSANETWORKEVENTS);

typedef struct DJITCP_Host
{
    SDL_DJITCPHost address; /* Its key is the joystick's path */
    SDL_DJITCPState *state;
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    SDL_AtomicInt presence_changed;

    /* The host thread's */
    HANDLE stop_event;
    HANDLE link_events[SDL_DJI_TCP_MAX_LINKS];
    SOCKET sockets[SDL_DJI_TCP_MAX_LINKS];
    bool present;
    uint32_t next_generation;
    int published_battery;
    int logged_error;

    /* Under the mutex */
    uint32_t generation; /* 0 while absent */
    SDL_SerialIdentity identity;
    SDL_SerialControls latest;
    uint64_t latest_sequence;
    int battery;
    SDL_SerialSnapshotQueue queue;
    uint64_t sequence;

    /* The joystick thread's */
    uint32_t registered; /* The generation it has a joystick for, 0 when none */
    SDL_JoystickID instance_id;
    SDL_GUID guid;
    SDL_SerialIdentity joystick_identity;

    struct DJITCP_Host *next;
} DJITCP_Host;

struct joystick_hwdata
{
    DJITCP_Host *host; /* NULL once the host has stopped */
    uint32_t generation;
    uint64_t sequence; /* The last snapshot sent */
    int battery;       /* The last battery level sent, -1 for none */
};

static SDL_Mutex *djitcp_lock;
static char *djitcp_hint;
static SDL_AtomicInt djitcp_hint_changed;
static DJITCP_Host *djitcp_hosts;

static HMODULE djitcp_ws2;
static bool djitcp_started;
static DJITCP_WSAStartup djitcp_wsa_startup;
static DJITCP_WSACleanup djitcp_wsa_cleanup;
static DJITCP_Socket djitcp_socket;
static DJITCP_CloseSocket djitcp_closesocket;
static DJITCP_Connect djitcp_connect;
static DJITCP_Send djitcp_send;
static DJITCP_Recv djitcp_recv;
static DJITCP_SetSockOpt djitcp_setsockopt;
static DJITCP_WSAGetLastError djitcp_wsa_get_last_error;
static DJITCP_WSAEventSelect djitcp_wsa_event_select;
static DJITCP_WSAEnumNetworkEvents djitcp_wsa_enum_network_events;

/* A failing call is logged once until the error changes */
static void DJITCP_LogError(DJITCP_Host *host, const char *call, int error)
{
    if (host->logged_error == error) {
        return;
    }
    host->logged_error = error;
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "DJI remote %s: %s failed with error %d", host->address.key, call, error);
}

static bool DJITCP_LoadWinsock(void)
{
    WSADATA data;

    if (djitcp_started) {
        return true;
    }
    if (!djitcp_ws2) {
        djitcp_ws2 = LoadLibrary(TEXT("ws2_32.dll"));
        if (!djitcp_ws2) {
            return false;
        }
    }
    djitcp_wsa_startup = (DJITCP_WSAStartup)GetProcAddress(djitcp_ws2, "WSAStartup");
    djitcp_wsa_cleanup = (DJITCP_WSACleanup)GetProcAddress(djitcp_ws2, "WSACleanup");
    djitcp_socket = (DJITCP_Socket)GetProcAddress(djitcp_ws2, "socket");
    djitcp_closesocket = (DJITCP_CloseSocket)GetProcAddress(djitcp_ws2, "closesocket");
    djitcp_connect = (DJITCP_Connect)GetProcAddress(djitcp_ws2, "connect");
    djitcp_send = (DJITCP_Send)GetProcAddress(djitcp_ws2, "send");
    djitcp_recv = (DJITCP_Recv)GetProcAddress(djitcp_ws2, "recv");
    djitcp_setsockopt = (DJITCP_SetSockOpt)GetProcAddress(djitcp_ws2, "setsockopt");
    djitcp_wsa_get_last_error = (DJITCP_WSAGetLastError)GetProcAddress(djitcp_ws2, "WSAGetLastError");
    djitcp_wsa_event_select = (DJITCP_WSAEventSelect)GetProcAddress(djitcp_ws2, "WSAEventSelect");
    djitcp_wsa_enum_network_events = (DJITCP_WSAEnumNetworkEvents)GetProcAddress(djitcp_ws2, "WSAEnumNetworkEvents");
    if (!djitcp_wsa_startup || !djitcp_wsa_cleanup || !djitcp_socket || !djitcp_closesocket || !djitcp_connect ||
        !djitcp_send || !djitcp_recv || !djitcp_setsockopt || !djitcp_wsa_get_last_error || !djitcp_wsa_event_select ||
        !djitcp_wsa_enum_network_events) {
        return false;
    }
    if (djitcp_wsa_startup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }
    djitcp_started = true;
    return true;
}

static void DJITCP_UnloadWinsock(void)
{
    if (djitcp_started) {
        djitcp_wsa_cleanup();
        djitcp_started = false;
    }
    if (djitcp_ws2) {
        FreeLibrary(djitcp_ws2);
        djitcp_ws2 = NULL;
    }
}

/* The module's snapshots, in order, on the host thread */
static void DJITCP_Changed(void *userdata, int sub, const SDL_SerialSnapshot *snapshot)
{
    DJITCP_Host *host = (DJITCP_Host *)userdata;
    SDL_SerialQueueEntry entry;

    if (sub != 0) {
        return;
    }
    if (!snapshot->present) {
        if (host->present) {
            host->present = false;
            SDL_LockMutex(host->mutex);
            host->generation = 0;
            SDL_zero(host->latest);
            SDL_UnlockMutex(host->mutex);
            SDL_SetAtomicInt(&host->presence_changed, 1);
        }
        return;
    }
    if (!host->present) {
        host->present = true;
        if (++host->next_generation == 0) {
            host->next_generation = 1;
        }
        SDL_LockMutex(host->mutex);
        host->generation = host->next_generation;
        host->identity = snapshot->identity;
        SDL_zero(host->latest);
        SDL_UnlockMutex(host->mutex);
        SDL_SetAtomicInt(&host->presence_changed, 1);
    }
    SDL_zero(entry);
    entry.sub = 0;
    entry.generation = host->next_generation;
    entry.stamp_ns = SDL_GetTicksNS();
    entry.controls = snapshot->controls;
    SDL_LockMutex(host->mutex);
    entry.sequence = ++host->sequence;
    SDL_Serial_PushQueue(&host->queue, &entry);
    host->latest = entry.controls;
    host->latest_sequence = entry.sequence;
    SDL_UnlockMutex(host->mutex);
}

static void DJITCP_CloseLink(DJITCP_Host *host, int link)
{
    if (host->sockets[link] != INVALID_SOCKET) {
        djitcp_closesocket(host->sockets[link]);
        host->sockets[link] = INVALID_SOCKET;
    }
    ResetEvent(host->link_events[link]);
}

static void DJITCP_StartConnect(DJITCP_Host *host, int link, uint64_t now)
{
    struct sockaddr_in address;
    const BOOL nodelay = TRUE;
    SOCKET sock;

    DJITCP_CloseLink(host, link);
    sock = djitcp_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        DJITCP_LogError(host, "socket", djitcp_wsa_get_last_error());
        SDL_DJITCP_Connected(host->state, link, false, now);
        return;
    }
    djitcp_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    /* Also makes the socket non-blocking */
    if (djitcp_wsa_event_select(sock, host->link_events[link], FD_CONNECT | FD_READ | FD_CLOSE) != 0) {
        DJITCP_LogError(host, "WSAEventSelect", djitcp_wsa_get_last_error());
        djitcp_closesocket(sock);
        SDL_DJITCP_Connected(host->state, link, false, now);
        return;
    }
    host->sockets[link] = sock;

    SDL_zero(address);
    address.sin_family = AF_INET;
    /* Network byte order, most significant byte first */
    address.sin_port = (USHORT)(((host->address.port & 0xFF) << 8) | (host->address.port >> 8));
    address.sin_addr.S_un.S_un_b.s_b1 = (UCHAR)(host->address.address >> 24);
    address.sin_addr.S_un.S_un_b.s_b2 = (UCHAR)(host->address.address >> 16);
    address.sin_addr.S_un.S_un_b.s_b3 = (UCHAR)(host->address.address >> 8);
    address.sin_addr.S_un.S_un_b.s_b4 = (UCHAR)host->address.address;
    if (djitcp_connect(sock, (const struct sockaddr *)&address, sizeof(address)) == 0) {
        SDL_DJITCP_Connected(host->state, link, true, now);
        return;
    }
    if (djitcp_wsa_get_last_error() != WSAEWOULDBLOCK) {
        DJITCP_LogError(host, "connect", djitcp_wsa_get_last_error());
        DJITCP_CloseLink(host, link);
        SDL_DJITCP_Connected(host->state, link, false, now);
    }
    /* Otherwise FD_CONNECT reports the result */
}

/* Takes what arrived until the socket has nothing more. A close or an error
 * is reported as loss. */
static void DJITCP_Read(DJITCP_Host *host, int link)
{
    char buffer[DJITCP_READ_SIZE];

    while (host->sockets[link] != INVALID_SOCKET) {
        const int received = djitcp_recv(host->sockets[link], buffer, (int)sizeof(buffer), 0);

        if (received > 0) {
            SDL_DJITCP_Received(host->state, link, (const uint8_t *)buffer, (size_t)received, SDL_GetTicks());
            continue;
        }
        if (received < 0 && djitcp_wsa_get_last_error() == WSAEWOULDBLOCK) {
            return;
        }
        if (received < 0) {
            DJITCP_LogError(host, "recv", djitcp_wsa_get_last_error());
        }
        DJITCP_CloseLink(host, link);
        SDL_DJITCP_Lost(host->state, link, SDL_GetTicks());
        return;
    }
}

static void DJITCP_LinkEvents(DJITCP_Host *host, int link)
{
    WSANETWORKEVENTS events;

    if (host->sockets[link] == INVALID_SOCKET) {
        ResetEvent(host->link_events[link]);
        return;
    }
    /* This also resets the link's event */
    if (djitcp_wsa_enum_network_events(host->sockets[link], host->link_events[link], &events) != 0) {
        DJITCP_LogError(host, "WSAEnumNetworkEvents", djitcp_wsa_get_last_error());
        DJITCP_CloseLink(host, link);
        SDL_DJITCP_Lost(host->state, link, SDL_GetTicks());
        return;
    }
    if (events.lNetworkEvents & FD_CONNECT) {
        const bool connected = (events.iErrorCode[FD_CONNECT_BIT] == 0);

        if (!connected) {
            DJITCP_LogError(host, "connect", events.iErrorCode[FD_CONNECT_BIT]);
            DJITCP_CloseLink(host, link);
        }
        SDL_DJITCP_Connected(host->state, link, connected, SDL_GetTicks());
    }
    if (events.lNetworkEvents & (FD_READ | FD_CLOSE)) {
        DJITCP_Read(host, link);
    }
    if ((events.lNetworkEvents & FD_CLOSE) && host->sockets[link] != INVALID_SOCKET) {
        DJITCP_CloseLink(host, link);
        SDL_DJITCP_Lost(host->state, link, SDL_GetTicks());
    }
}

/* Runs the session's actions, which can queue more */
static void DJITCP_RunActions(DJITCP_Host *host)
{
    SDL_DJITCPAction action;
    int count = 0;

    while (count++ < DJITCP_MAX_ACTIONS && SDL_DJITCP_NextAction(host->state, &action)) {
        const uint64_t now = SDL_GetTicks();

        if (action.link < 0 || action.link >= SDL_DJI_TCP_MAX_LINKS) {
            continue;
        }
        switch (action.kind) {
        case SDL_DJI_TCP_CONNECT:
            DJITCP_StartConnect(host, action.link, now);
            break;
        case SDL_DJI_TCP_CLOSE:
            DJITCP_CloseLink(host, action.link);
            break;
        case SDL_DJI_TCP_SEND:
            if (host->sockets[action.link] != INVALID_SOCKET &&
                djitcp_send(host->sockets[action.link], (const char *)action.data, (int)action.length, 0) != (int)action.length) {
                DJITCP_LogError(host, "send", djitcp_wsa_get_last_error());
                DJITCP_CloseLink(host, action.link);
                SDL_DJITCP_Lost(host->state, action.link, now);
            }
            break;
        }
    }
}

static void DJITCP_PublishBattery(DJITCP_Host *host)
{
    if (host->state->battery != host->published_battery) {
        host->published_battery = host->state->battery;
        SDL_LockMutex(host->mutex);
        host->battery = host->published_battery;
        SDL_UnlockMutex(host->mutex);
    }
}

/* Only this thread touches the sockets and the session */
static int SDLCALL DJITCP_HostThread(void *data)
{
    DJITCP_Host *host = (DJITCP_Host *)data;
    SDL_SerialSink sink;
    int i;

    sink.userdata = host;
    sink.changed = DJITCP_Changed;
    sink.log = NULL;
    SDL_DJITCP_Init(host->state, &sink, SDL_GetTicks());
    for (;;) {
        HANDLE handles[1 + SDL_DJI_TCP_MAX_LINKS];
        int links[1 + SDL_DJI_TCP_MAX_LINKS];
        DWORD count = 0, timeout = INFINITE, result;
        uint64_t deadline;

        if (SDL_DJITCP_GetDeadline(host->state, &deadline) && SDL_GetTicks() >= deadline) {
            SDL_DJITCP_Tick(host->state, SDL_GetTicks());
        }
        DJITCP_RunActions(host);
        DJITCP_PublishBattery(host);
        if (SDL_DJITCP_GetDeadline(host->state, &deadline)) {
            const Uint64 now = SDL_GetTicks();

            timeout = (deadline <= now) ? 0 : (DWORD)SDL_min(deadline - now, (Uint64)0x7FFFFFFF);
        }
        handles[count] = host->stop_event;
        links[count++] = -1;
        for (i = 0; i < SDL_DJI_TCP_MAX_LINKS; ++i) {
            if (host->sockets[i] != INVALID_SOCKET) {
                handles[count] = host->link_events[i];
                links[count++] = i;
            }
        }
        result = WaitForMultipleObjects(count, handles, FALSE, timeout);
        if (result == WAIT_TIMEOUT) {
            continue;
        }
        if (result >= WAIT_OBJECT_0 + count) {
            DJITCP_LogError(host, "WaitForMultipleObjects", (int)GetLastError());
            SDL_Delay(100);
            continue;
        }
        if (links[result - WAIT_OBJECT_0] < 0) {
            break;
        }
        /* The wait names the lowest signaled link. Every link whose event is
           set is served, so a busy link cannot hold the others back. */
        for (i = 0; i < (int)count; ++i) {
            if (links[i] >= 0 && (i == (int)(result - WAIT_OBJECT_0) || WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0)) {
                DJITCP_LinkEvents(host, links[i]);
            }
        }
    }
    for (i = 0; i < SDL_DJI_TCP_MAX_LINKS; ++i) {
        DJITCP_CloseLink(host, i);
    }
    return 0;
}

static void DJITCP_FreeHost(DJITCP_Host *host)
{
    int i;

    for (i = 0; i < SDL_DJI_TCP_MAX_LINKS; ++i) {
        if (host->link_events[i]) {
            CloseHandle(host->link_events[i]);
        }
    }
    if (host->stop_event) {
        CloseHandle(host->stop_event);
    }
    if (host->mutex) {
        SDL_DestroyMutex(host->mutex);
    }
    SDL_free(host->state);
    SDL_free(host);
}

static DJITCP_Host *DJITCP_StartHost(const SDL_DJITCPHost *address)
{
    DJITCP_Host *host = (DJITCP_Host *)SDL_calloc(1, sizeof(*host));
    DJITCP_Host **tail;
    char name[SDL_DJI_TCP_KEY_LENGTH + 16];
    bool created = true;
    int i;

    if (!host) {
        return NULL;
    }
    host->address = *address;
    host->state = (SDL_DJITCPState *)SDL_calloc(1, sizeof(*host->state));
    host->mutex = SDL_CreateMutex();
    host->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    host->battery = -1;
    host->published_battery = -1;
    for (i = 0; i < SDL_DJI_TCP_MAX_LINKS; ++i) {
        host->sockets[i] = INVALID_SOCKET;
        /* Manual reset, as WSAEventSelect expects */
        host->link_events[i] = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!host->link_events[i]) {
            created = false;
        }
    }
    if (!created || !host->state || !host->mutex || !host->stop_event) {
        DJITCP_FreeHost(host);
        return NULL;
    }
    SDL_Serial_ClearQueue(&host->queue);

    for (tail = &djitcp_hosts; *tail; tail = &(*tail)->next) {
    }
    *tail = host;

    (void)SDL_snprintf(name, sizeof(name), "SDLDJITCP %s", address->key);
    host->thread = SDL_CreateThread(DJITCP_HostThread, name, host);
    if (!host->thread) {
        for (tail = &djitcp_hosts; *tail; tail = &(*tail)->next) {
            if (*tail == host) {
                *tail = host->next;
                break;
            }
        }
        DJITCP_FreeHost(host);
        return NULL;
    }
    return host;
}

/* Removes the host's joystick, stops its thread and frees it */
static void DJITCP_StopHost(DJITCP_Host *host, bool notify)
{
    DJITCP_Host **link;

    if (host->registered) {
        SDL_Joystick *joystick = SDL_GetJoystickFromID(host->instance_id);

        if (joystick && joystick->hwdata) {
            joystick->hwdata->host = NULL;
        }
        if (notify) {
            SDL_PrivateJoystickRemoved(host->instance_id);
        }
        host->registered = 0;
    }
    SetEvent(host->stop_event);
    SDL_WaitThread(host->thread, NULL);
    host->thread = NULL;

    for (link = &djitcp_hosts; *link; link = &(*link)->next) {
        if (*link == host) {
            *link = host->next;
            break;
        }
    }
    DJITCP_FreeHost(host);
}

static void DJITCP_LogHintEntry(void *userdata, const char *entry, size_t length, const char *reason)
{
    (void)userdata;
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "SDL_JOYSTICK_DJI_REMOTE_TCP_HOSTS entry \"%.*s\" skipped: %s", (int)length, entry, reason);
}

/* Applies the latest SDL_HINT_JOYSTICK_DJI_REMOTE_TCP_HOSTS on the joystick thread */
static void DJITCP_ApplyHint(void)
{
    SDL_DJITCPHost hosts[SDL_DJI_TCP_MAX_HOSTS];
    DJITCP_Host *host, *next;
    char *hint = NULL;
    int count, i;

    SDL_LockMutex(djitcp_lock);
    if (djitcp_hint) {
        hint = SDL_strdup(djitcp_hint);
    }
    SDL_UnlockMutex(djitcp_lock);
    count = SDL_DJITCP_ParseHosts(hint, hosts, SDL_DJI_TCP_MAX_HOSTS, DJITCP_LogHintEntry, NULL);
    SDL_free(hint);

    for (host = djitcp_hosts; host; host = next) {
        next = host->next;
        for (i = 0; i < count; ++i) {
            if (SDL_strcmp(host->address.key, hosts[i].key) == 0) {
                break;
            }
        }
        if (i == count) {
            DJITCP_StopHost(host, true);
        }
    }
    for (i = 0; i < count; ++i) {
        for (host = djitcp_hosts; host; host = host->next) {
            if (SDL_strcmp(host->address.key, hosts[i].key) == 0) {
                break;
            }
        }
        if (host) {
            continue;
        }
        if (!DJITCP_LoadWinsock()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "DJI remote %s: Winsock could not be loaded", hosts[i].key);
            break;
        }
        if (!DJITCP_StartHost(&hosts[i])) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "DJI remote %s could not start", hosts[i].key);
        }
    }
}

static void DJITCP_CheckPresence(DJITCP_Host *host)
{
    SDL_SerialIdentity identity;
    uint32_t generation;

    SDL_LockMutex(host->mutex);
    generation = host->generation;
    identity = host->identity;
    SDL_UnlockMutex(host->mutex);

    if (host->registered && host->registered != generation) {
        SDL_Joystick *joystick = SDL_GetJoystickFromID(host->instance_id);

        if (joystick && joystick->hwdata) {
            joystick->hwdata->host = NULL;
        }
        SDL_PrivateJoystickRemoved(host->instance_id);
        host->registered = 0;
    }
    if (!host->registered && generation) {
        host->registered = generation;
        host->joystick_identity = identity;
        host->instance_id = SDL_GetNextObjectID();
        /* Vendor 0 puts the name in the GUID. 'd' is this driver's. */
        host->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_UNKNOWN, 0, 0, 0, NULL, identity.name, 'd', 0);
        SDL_PrivateJoystickAdded(host->instance_id);
    }
}

static DJITCP_Host *DJITCP_GetDevice(int device_index)
{
    DJITCP_Host *host;

    if (device_index < 0) {
        return NULL;
    }
    for (host = djitcp_hosts; host; host = host->next) {
        if (host->registered && device_index-- == 0) {
            return host;
        }
    }
    return NULL;
}

static void SDLCALL DJITCP_HintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    (void)userdata;
    (void)name;
    (void)oldValue;
    SDL_LockMutex(djitcp_lock);
    SDL_free(djitcp_hint);
    djitcp_hint = hint ? SDL_strdup(hint) : NULL;
    SDL_UnlockMutex(djitcp_lock);
    SDL_SetAtomicInt(&djitcp_hint_changed, 1);
}

static bool DJITCP_JoystickInit(void)
{
    djitcp_lock = SDL_CreateMutex();
    if (!djitcp_lock) {
        return false;
    }
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_DJI_REMOTE_TCP_HOSTS, DJITCP_HintChanged, NULL);
    return true;
}

static int DJITCP_JoystickGetCount(void)
{
    DJITCP_Host *host;
    int count = 0;

    for (host = djitcp_hosts; host; host = host->next) {
        if (host->registered) {
            ++count;
        }
    }
    return count;
}

static void DJITCP_JoystickDetect(void)
{
    DJITCP_Host *host;

    if (SDL_GetAtomicInt(&djitcp_hint_changed)) {
        SDL_SetAtomicInt(&djitcp_hint_changed, 0);
        DJITCP_ApplyHint();
    }
    for (host = djitcp_hosts; host; host = host->next) {
        if (SDL_GetAtomicInt(&host->presence_changed)) {
            SDL_SetAtomicInt(&host->presence_changed, 0);
            DJITCP_CheckPresence(host);
        }
    }
}

/* A remote on the network is no other driver's device */
static bool DJITCP_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    (void)vendor_id;
    (void)product_id;
    (void)version;
    (void)name;
    return false;
}

static const char *DJITCP_JoystickGetDeviceName(int device_index)
{
    DJITCP_Host *host = DJITCP_GetDevice(device_index);

    return host ? host->joystick_identity.name : NULL;
}

static const char *DJITCP_JoystickGetDevicePath(int device_index)
{
    DJITCP_Host *host = DJITCP_GetDevice(device_index);

    return host ? host->address.key : NULL;
}

static int DJITCP_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    (void)device_index;
    return -1;
}

static int DJITCP_JoystickGetDevicePlayerIndex(int device_index)
{
    (void)device_index;
    return -1;
}

static void DJITCP_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
    (void)device_index;
    (void)player_index;
}

static SDL_GUID DJITCP_JoystickGetDeviceGUID(int device_index)
{
    DJITCP_Host *host = DJITCP_GetDevice(device_index);
    SDL_GUID guid;

    if (!host) {
        SDL_zero(guid);
        return guid;
    }
    return host->guid;
}

static SDL_JoystickID DJITCP_JoystickGetDeviceInstanceID(int device_index)
{
    DJITCP_Host *host = DJITCP_GetDevice(device_index);

    return host ? host->instance_id : 0;
}

static void DJITCP_SendControls(SDL_Joystick *joystick, const SDL_SerialControls *controls, Uint64 timestamp)
{
    int i;

    for (i = 0; i < joystick->naxes; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, controls->axes[i]);
    }
    for (i = 0; i < joystick->nbuttons; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, SDL_Serial_GetButton(controls, i));
    }
}

static void DJITCP_SendBattery(SDL_Joystick *joystick, int battery)
{
    if (battery >= 0 && battery != joystick->hwdata->battery) {
        joystick->hwdata->battery = battery;
        SDL_SendJoystickPowerInfo(joystick, SDL_POWERSTATE_ON_BATTERY, battery);
    }
}

static bool DJITCP_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    DJITCP_Host *host = DJITCP_GetDevice(device_index);
    struct joystick_hwdata *hwdata;
    SDL_SerialControls latest;
    int battery;

    if (!host) {
        return SDL_SetError("DJI remote index out of range");
    }
    hwdata = (struct joystick_hwdata *)SDL_calloc(1, sizeof(*hwdata));
    if (!hwdata) {
        return false;
    }
    hwdata->host = host;
    hwdata->generation = host->registered;
    hwdata->battery = -1;
    joystick->hwdata = hwdata;
    joystick->naxes = host->joystick_identity.naxes;
    joystick->nbuttons = host->joystick_identity.nbuttons;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;

    /* The state so far. Queued snapshots up to it are skipped. */
    SDL_LockMutex(host->mutex);
    latest = host->latest;
    hwdata->sequence = host->latest_sequence;
    battery = host->battery;
    SDL_UnlockMutex(host->mutex);
    DJITCP_SendControls(joystick, &latest, SDL_GetTicksNS());
    DJITCP_SendBattery(joystick, battery);
    return true;
}

static bool DJITCP_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    (void)joystick;
    (void)low_frequency_rumble;
    (void)high_frequency_rumble;
    return SDL_Unsupported();
}

static bool DJITCP_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    (void)joystick;
    (void)left_rumble;
    (void)right_rumble;
    return SDL_Unsupported();
}

static bool DJITCP_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)joystick;
    (void)red;
    (void)green;
    (void)blue;
    return SDL_Unsupported();
}

static bool DJITCP_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    (void)joystick;
    (void)data;
    (void)size;
    return SDL_Unsupported();
}

static bool DJITCP_JoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    (void)joystick;
    (void)enabled;
    return SDL_Unsupported();
}

/* Drains the host's snapshot queue in order */
static void DJITCP_JoystickUpdate(SDL_Joystick *joystick)
{
    struct joystick_hwdata *hwdata = joystick->hwdata;
    SDL_SerialQueueEntry entries[SDL_SERIAL_QUEUE_ENTRIES];
    DJITCP_Host *host;
    int count = 0, battery, i;

    if (!hwdata || !hwdata->host) {
        return;
    }
    host = hwdata->host;
    SDL_LockMutex(host->mutex);
    while (count < SDL_SERIAL_QUEUE_ENTRIES && SDL_Serial_PopQueue(&host->queue, &entries[count])) {
        ++count;
    }
    battery = host->battery;
    SDL_UnlockMutex(host->mutex);
    for (i = 0; i < count; ++i) {
        const SDL_SerialQueueEntry *entry = &entries[i];

        if (entry->generation != hwdata->generation || entry->sequence <= hwdata->sequence) {
            continue;
        }
        hwdata->sequence = entry->sequence;
        DJITCP_SendControls(joystick, &entry->controls, entry->stamp_ns);
    }
    DJITCP_SendBattery(joystick, battery);
}

static void DJITCP_JoystickClose(SDL_Joystick *joystick)
{
    SDL_free(joystick->hwdata);
    joystick->hwdata = NULL;
}

static void DJITCP_JoystickQuit(void)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_DJI_REMOTE_TCP_HOSTS, DJITCP_HintChanged, NULL);
    while (djitcp_hosts) {
        DJITCP_StopHost(djitcp_hosts, false);
    }
    DJITCP_UnloadWinsock();
    SDL_free(djitcp_hint);
    djitcp_hint = NULL;
    SDL_SetAtomicInt(&djitcp_hint_changed, 0);
    if (djitcp_lock) {
        SDL_DestroyMutex(djitcp_lock);
        djitcp_lock = NULL;
    }
}

static void DJITCP_MapInput(SDL_InputMapping *out, const SDL_SerialMapInput *in)
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

static bool DJITCP_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    DJITCP_Host *host = DJITCP_GetDevice(device_index);
    const SDL_SerialGamepadMap *map;

    if (!host || !host->joystick_identity.has_mapping) {
        return false;
    }
    map = &host->joystick_identity.mapping;
    SDL_zerop(out);
    DJITCP_MapInput(&out->a, &map->a);
    DJITCP_MapInput(&out->b, &map->b);
    DJITCP_MapInput(&out->x, &map->x);
    DJITCP_MapInput(&out->y, &map->y);
    DJITCP_MapInput(&out->back, &map->back);
    DJITCP_MapInput(&out->guide, &map->guide);
    DJITCP_MapInput(&out->start, &map->start);
    DJITCP_MapInput(&out->leftstick, &map->leftstick);
    DJITCP_MapInput(&out->rightstick, &map->rightstick);
    DJITCP_MapInput(&out->leftshoulder, &map->leftshoulder);
    DJITCP_MapInput(&out->rightshoulder, &map->rightshoulder);
    DJITCP_MapInput(&out->dpup, &map->dpup);
    DJITCP_MapInput(&out->dpdown, &map->dpdown);
    DJITCP_MapInput(&out->dpleft, &map->dpleft);
    DJITCP_MapInput(&out->dpright, &map->dpright);
    DJITCP_MapInput(&out->misc1, &map->misc1);
    DJITCP_MapInput(&out->leftx, &map->leftx);
    DJITCP_MapInput(&out->lefty, &map->lefty);
    DJITCP_MapInput(&out->rightx, &map->rightx);
    DJITCP_MapInput(&out->righty, &map->righty);
    DJITCP_MapInput(&out->lefttrigger, &map->lefttrigger);
    DJITCP_MapInput(&out->righttrigger, &map->righttrigger);
    return true;
}

SDL_JoystickDriver SDL_DJITCP_JoystickDriver = {
    DJITCP_JoystickInit,
    DJITCP_JoystickGetCount,
    DJITCP_JoystickDetect,
    DJITCP_JoystickIsDevicePresent,
    DJITCP_JoystickGetDeviceName,
    DJITCP_JoystickGetDevicePath,
    DJITCP_JoystickGetDeviceSteamVirtualGamepadSlot,
    DJITCP_JoystickGetDevicePlayerIndex,
    DJITCP_JoystickSetDevicePlayerIndex,
    DJITCP_JoystickGetDeviceGUID,
    DJITCP_JoystickGetDeviceInstanceID,
    DJITCP_JoystickOpen,
    DJITCP_JoystickRumble,
    DJITCP_JoystickRumbleTriggers,
    DJITCP_JoystickSetLED,
    DJITCP_JoystickSendEffect,
    DJITCP_JoystickSetSensorsEnabled,
    DJITCP_JoystickUpdate,
    DJITCP_JoystickClose,
    DJITCP_JoystickQuit,
    DJITCP_JoystickGetGamepadMapping
};

#endif /* SDL_JOYSTICK_DJI_TCP */
