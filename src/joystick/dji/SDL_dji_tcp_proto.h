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

/* A DJI screen remote over TCP port 40007: the links to one host, what goes
 * out on them, and the one joystick their 06/AE frames drive. Pure C99: no
 * SDL runtime and no I/O. SDL_djitcpjoystick.c owns the sockets. It takes
 * the actions this session queues and reports what each link does.
 *
 * The DJI RC gets one link and a 00/01 keepalive at connect and after every
 * 40 ms without data, as dji-rc-linux sends one after two empty 20 ms reads.
 * The DJI RC 2, which announces "rc331", recycles its session and resets a
 * link it is sent anything on, so it gets five links started 500 ms apart
 * and no keepalive. Every request that asks for an acknowledgement gets an
 * empty response. A link closes when no link has brought a 06/AE frame for
 * 1000 ms, and redials at once if it had brought one since it connected,
 * else 100 ms later. The facts are from dji-rc-linux and dji-rc-joystick
 * by voluminor (LGPL, facts only). No code from them is copied.
 */

#ifndef SDL_dji_tcp_proto_h_
#define SDL_dji_tcp_proto_h_

#include "SDL_dji_remote_proto.h"

#define SDL_DJI_TCP_PORT            40007
#define SDL_DJI_TCP_MAX_LINKS       5
#define SDL_DJI_TCP_CONNECT_MS      100  /* A connect not done by then has failed */
#define SDL_DJI_TCP_REDIAL_MS       100
#define SDL_DJI_TCP_KEEPALIVE_MS    40
#define SDL_DJI_TCP_LIVENESS_MS     1000
#define SDL_DJI_TCP_SESSION_MS      2500 /* Links start spread across this span */
#define SDL_DJI_TCP_MAX_SEND        64
#define SDL_DJI_TCP_MAX_ACTIONS     32
#define SDL_DJI_TCP_AE_LENGTH       17   /* The 06/AE payload */
#define SDL_DJI_TCP_BUTTONS         12

typedef enum SDL_DJILinkPhase
{
    SDL_DJI_LINK_IDLE,       /* Not in the pool */
    SDL_DJI_LINK_WAITING,    /* Connects at connect_at */
    SDL_DJI_LINK_CONNECTING,
    SDL_DJI_LINK_UP
} SDL_DJILinkPhase;

typedef struct SDL_DJILink
{
    SDL_DJILinkPhase phase;
    uint64_t connect_at;
    uint64_t connect_deadline;
    uint64_t connected_at;
    uint64_t keepalive_at;
    bool carried;      /* A 06/AE frame came since the link connected */
    uint16_t sequence; /* The next keepalive's sequence, from 1 */
    SDL_DJIParser parser;
} SDL_DJILink;

typedef enum SDL_DJITCPActionKind
{
    SDL_DJI_TCP_CONNECT, /* Start a connect. Report it with SDL_DJITCP_Connected. */
    SDL_DJI_TCP_CLOSE,   /* Close the link's socket */
    SDL_DJI_TCP_SEND     /* Send data. A failure is reported with SDL_DJITCP_Lost. */
} SDL_DJITCPActionKind;

typedef struct SDL_DJITCPAction
{
    SDL_DJITCPActionKind kind;
    int link;
    size_t length;
    uint8_t data[SDL_DJI_TCP_MAX_SEND];
} SDL_DJITCPAction;

typedef struct SDL_DJITCPState
{
    SDL_SerialBase base; /* The joystick's snapshots, sub-device 0 */
    SDL_DJILink links[SDL_DJI_TCP_MAX_LINKS];
    int running;         /* Links in the pool */
    bool quiet;          /* No keepalive: the DJI RC 2 */
    bool live;           /* A 06/AE frame came within the last 1000 ms */
    uint64_t last_frame; /* The last 06/AE frame on any link */
    char model[SDL_DJI_MODEL_LENGTH];
    int battery;         /* Percent from 06/1E, -1 until one comes */
    uint32_t dropped_actions;
    SDL_SerialControls controls;
    SDL_DJITCPAction actions[SDL_DJI_TCP_MAX_ACTIONS];
    int action_head;
    int action_count;
} SDL_DJITCPState;

/* Starts the pool with one link, which connects at once */
extern void SDL_DJITCP_Init(SDL_DJITCPState *s, const SDL_SerialSink *sink, uint64_t now);
/* The oldest queued action. False when none */
extern bool SDL_DJITCP_NextAction(SDL_DJITCPState *s, SDL_DJITCPAction *action);
/* A connect finished. On failure the socket is already closed. */
extern void SDL_DJITCP_Connected(SDL_DJITCPState *s, int link, bool success, uint64_t now);
extern void SDL_DJITCP_Received(SDL_DJITCPState *s, int link, const uint8_t *data, size_t length, uint64_t now);
/* A read, a send or the peer failed. The socket is already closed. */
extern void SDL_DJITCP_Lost(SDL_DJITCPState *s, int link, uint64_t now);
extern void SDL_DJITCP_Tick(SDL_DJITCPState *s, uint64_t now);
/* Earliest clock value at which Tick must run. False when none */
extern bool SDL_DJITCP_GetDeadline(const SDL_DJITCPState *s, uint64_t *deadline);

/* A 06/AE payload of 17 bytes or more into the joystick's controls */
extern bool SDL_DJITCP_DecodeControls(const uint8_t *payload, size_t length, SDL_SerialControls *controls);

#define SDL_DJI_TCP_MAX_HOSTS  8
#define SDL_DJI_TCP_KEY_LENGTH 24 /* "255.255.255.255:65535" */

typedef struct SDL_DJITCPHost
{
    uint32_t address; /* a.b.c.d as (a << 24) | (b << 16) | (c << 8) | d */
    uint16_t port;
    char key[SDL_DJI_TCP_KEY_LENGTH]; /* a.b.c.d:port */
} SDL_DJITCPHost;

typedef void (*SDL_DJITCPHostLog)(void *userdata, const char *entry, size_t length, const char *reason);

/* SDL_HINT_JOYSTICK_DJI_REMOTE_TCP_HOSTS: comma-separated IPv4 addresses in
 * dotted decimal, each with an optional :port, 40007 when absent. Spaces
 * around entries are ignored. An entry that cannot be used is skipped with
 * one call to log, and an address named twice keeps one entry. Returns the
 * number of hosts. */
extern int SDL_DJITCP_ParseHosts(const char *hint, SDL_DJITCPHost *hosts, int max, SDL_DJITCPHostLog log, void *userdata);

#endif /* SDL_dji_tcp_proto_h_ */
