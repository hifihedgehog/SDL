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

/* Controllers that carry their input over a Bluetooth RFCOMM channel:
 * which paired devices qualify, the contract between the family modules
 * and the link, and the link itself. Pure C99: no SDL runtime and no I/O.
 * SDL_rfcommjoystick.c owns the threads and the sockets. It runs one link
 * per matching paired device, executes the actions the link queues and
 * reports what each socket does. hifihedgehog/SDL#33 Part 11.
 *
 * A link connects at once, by the family's service UUID with port 0 where
 * the family has one, else on its channel. A service connect that fails
 * other than by a page timeout is tried again on the channel at once. A
 * failed connect or a lost link connects again 3000 ms later, as MogaSerial
 * waits, until the device leaves the paired list. The module decides what to
 * send, when the link has gone quiet and when the joystick appears. Every
 * snapshot reaches the sink in order.
 */

#ifndef SDL_rfcomm_proto_h_
#define SDL_rfcomm_proto_h_

#include "../serial/SDL_serial_proto.h"

#define SDL_RFCOMM_RETRY_MS     3000
#define SDL_RFCOMM_DISCOVERY_MS 3000 /* The paired list is read this often */
#define SDL_RFCOMM_CHANNEL      1    /* The channel every family falls back to */
#define SDL_RFCOMM_MAX_SEND     8
#define SDL_RFCOMM_MAX_OUT      8    /* Sends a module can queue at once */
#define SDL_RFCOMM_MAX_ACTIONS  16
#define SDL_RFCOMM_NAME_LENGTH  248  /* BLUETOOTH_MAX_NAME_SIZE, in UTF-8 bytes here */
#define SDL_RFCOMM_MAX_DEVICES  8

typedef enum SDL_RFCOMMFamily
{
    SDL_RFCOMM_FAMILY_NONE,
    SDL_RFCOMM_FAMILY_MOGA,
    SDL_RFCOMM_FAMILY_ZEEMOTE,
    SDL_RFCOMM_FAMILY_BGP100,
    SDL_RFCOMM_FAMILY_PHONEJOY,
    SDL_RFCOMM_FAMILY_COUNT
} SDL_RFCOMMFamily;

/* The family a Bluetooth name belongs to. Names starting "Zeemote:
 * SteelSeries", in any case, belong to none: that pad is a HID gamepad. */
extern SDL_RFCOMMFamily SDL_RFCOMM_MatchName(const char *name);
/* Whether the name starts with the prefix, any_case comparing ASCII letters
   without case */
extern bool SDL_RFCOMM_StartsWith(const char *name, const char *prefix, bool any_case);
/* Copies UTF-8 text into size bytes with its terminator, cutting only at a
   character boundary */
extern void SDL_RFCOMM_CopyText(char *dst, size_t size, const char *src);

/* A device from the system's list of known devices */
typedef struct SDL_RFCOMMPaired
{
    uint64_t address;
    char name[SDL_RFCOMM_NAME_LENGTH];
    bool paired; /* Remembered or authenticated. A connection alone does not count. */
} SDL_RFCOMMPaired;

/* A device the driver runs a link for */
typedef struct SDL_RFCOMMDevice
{
    uint64_t address;
    SDL_RFCOMMFamily family;
    char name[SDL_RFCOMM_NAME_LENGTH];
} SDL_RFCOMMDevice;

/* The paired devices whose family is enabled, in list order. A repeated
 * address keeps its first entry. enabled is indexed by SDL_RFCOMMFamily.
 * Returns the number of devices. */
extern int SDL_RFCOMM_SelectDevices(const SDL_RFCOMMPaired *paired, int count, const bool *enabled, SDL_RFCOMMDevice *devices, int max);

typedef enum SDL_RFCOMMChange
{
    SDL_RFCOMM_KEEP,
    SDL_RFCOMM_START,
    SDL_RFCOMM_STOP
} SDL_RFCOMMChange;

/* old_changes gets KEEP or STOP for each running device, and new_changes
 * START or KEEP for each selected one. A device whose family or name changed
 * stops and starts again. */
extern void SDL_RFCOMM_DiffDevices(const SDL_RFCOMMDevice *old_devices, int nold, const SDL_RFCOMMDevice *new_devices, int nnew, SDL_RFCOMMChange *old_changes, SDL_RFCOMMChange *new_changes);

/* Every module state begins with an SDL_RFCOMMBase */
typedef struct SDL_RFCOMMBase
{
    SDL_SerialBase serial; /* The joystick's presence and snapshots, sub-device 0 */
    uint8_t out[SDL_RFCOMM_MAX_OUT][SDL_RFCOMM_MAX_SEND];
    uint8_t out_length[SDL_RFCOMM_MAX_OUT];
    int out_count;
    bool end;       /* The module ends the link */
    int battery_mv; /* The last battery reading, -1 for none */
} SDL_RFCOMMBase;

/* Clears the base and sets the sink */
extern void SDL_RFCOMM_ResetBase(SDL_RFCOMMBase *base, const SDL_SerialSink *sink);
/* Queues bytes to send. False, queuing nothing, when they do not fit. */
extern bool SDL_RFCOMM_Send(SDL_RFCOMMBase *base, const uint8_t *data, size_t length);
extern void SDL_RFCOMM_End(SDL_RFCOMMBase *base);

typedef struct SDL_RFCOMMModule
{
    SDL_RFCOMMFamily family;
    bool has_service;     /* Connect by service UUID first */
    uint8_t service[16];  /* The UUID as written, most significant byte first */
    size_t state_size;
    /* The link is up: clear all state and queue the start-up bytes. The
       player index is the one the joystick had, -1 for none. */
    void (*Reset)(void *state, const SDL_SerialSink *sink, const char *name, int player_index, uint64_t now);
    /* Parse received bytes, update controls, queue sends, change presence */
    void (*Feed)(void *state, const uint8_t *data, size_t length, uint64_t now);
    /* Run due deadlines */
    void (*Tick)(void *state, uint64_t now);
    /* Earliest clock value at which Tick must run. False when none. */
    bool (*GetDeadline)(const void *state, uint64_t *deadline);
    /* The application set a player index, -1 for none. May be NULL. */
    void (*SetPlayerIndex)(void *state, int player_index, uint64_t now);
} SDL_RFCOMMModule;

typedef enum SDL_RFCOMMPhase
{
    SDL_RFCOMM_WAITING,    /* Connects at connect_at */
    SDL_RFCOMM_CONNECTING,
    SDL_RFCOMM_UP,
    SDL_RFCOMM_STOPPED
} SDL_RFCOMMPhase;

typedef enum SDL_RFCOMMActionKind
{
    SDL_RFCOMM_ACTION_CONNECT, /* channel 0 connects by the module's service UUID */
    SDL_RFCOMM_ACTION_SEND,    /* A failure is reported with SDL_RFCOMMLink_Lost */
    SDL_RFCOMM_ACTION_CLOSE    /* Close the socket */
} SDL_RFCOMMActionKind;

typedef struct SDL_RFCOMMAction
{
    SDL_RFCOMMActionKind kind;
    uint8_t channel;
    size_t length;
    uint8_t data[SDL_RFCOMM_MAX_SEND];
} SDL_RFCOMMAction;

typedef enum SDL_RFCOMMConnectResult
{
    SDL_RFCOMM_CONNECT_OK,
    SDL_RFCOMM_CONNECT_TIMED_OUT, /* WSAETIMEDOUT: the page timed out, the device is away */
    SDL_RFCOMM_CONNECT_FAILED
} SDL_RFCOMMConnectResult;

typedef struct SDL_RFCOMMLink
{
    const SDL_RFCOMMModule *module;
    void *state; /* module->state_size bytes, beginning with an SDL_RFCOMMBase */
    SDL_SerialSink sink;
    char name[SDL_RFCOMM_NAME_LENGTH];
    SDL_RFCOMMPhase phase;
    uint64_t connect_at;
    uint8_t channel; /* Of the connect in progress, 0 for the service UUID */
    int player_index;
    uint32_t connects; /* Connect actions queued, for the log */
    SDL_RFCOMMAction actions[SDL_RFCOMM_MAX_ACTIONS];
    int action_head;
    int action_count;
    uint32_t dropped_actions;
} SDL_RFCOMMLink;

/* Waiting, with the first connect due at once */
extern void SDL_RFCOMMLink_Init(SDL_RFCOMMLink *link, const SDL_RFCOMMModule *module, void *state, const SDL_SerialSink *sink, const char *name, uint64_t now);
extern void SDL_RFCOMMLink_Connected(SDL_RFCOMMLink *link, SDL_RFCOMMConnectResult result, uint64_t now);
extern void SDL_RFCOMMLink_Received(SDL_RFCOMMLink *link, const uint8_t *data, size_t length, uint64_t now);
/* recv returned 0 or failed, a send failed, or the peer closed: the socket
   is already closed */
extern void SDL_RFCOMMLink_Lost(SDL_RFCOMMLink *link, uint64_t now);
extern void SDL_RFCOMMLink_Tick(SDL_RFCOMMLink *link, uint64_t now);
/* Earliest clock value at which Tick must run. False when none. */
extern bool SDL_RFCOMMLink_GetDeadline(const SDL_RFCOMMLink *link, uint64_t *deadline);
/* The oldest queued action. False when none. */
extern bool SDL_RFCOMMLink_NextAction(SDL_RFCOMMLink *link, SDL_RFCOMMAction *action);
extern void SDL_RFCOMMLink_SetPlayerIndex(SDL_RFCOMMLink *link, int player_index, uint64_t now);
/* The device left the paired list or its family was turned off: close the
   socket, clear presence and connect no more */
extern void SDL_RFCOMMLink_Stop(SDL_RFCOMMLink *link);

/* A signed 8-bit value, two's complement, as an axis value from -32768 to
   32767 */
extern int16_t SDL_RFCOMM_AxisFromS8(uint8_t value);
/* The same value negated, so full up becomes -32768 and full down 32767 */
extern int16_t SDL_RFCOMM_AxisFromS8Negated(uint8_t value);
/* An unsigned 8-bit trigger, 0 at rest, as -32768 to 32767 */
extern int16_t SDL_RFCOMM_TriggerFromU8(uint8_t value);

#endif /* SDL_rfcomm_proto_h_ */
