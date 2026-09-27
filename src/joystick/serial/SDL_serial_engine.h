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

/* The serial port engine. Pure C99: no SDL runtime and no I/O. It opens a
 * port and retries, runs one module, executes the module's actions one at a
 * time, tracks each sub-device's presence and publishes snapshots. It reaches
 * the port through SDL_SerialPortOps, so tests substitute a fake port. One
 * port thread owns each engine. The thread passes SDL_GetTicksNS at every
 * call. Modules see that time in milliseconds, which equals SDL_GetTicks.
 * When the port closes for good, a module that asks for it runs a close
 * sequence first.
 *
 * The line settings, timeouts, purge flags and modem and break functions
 * below carry the values of the Windows DCB, COMMTIMEOUTS, PurgeComm and
 * EscapeCommFunction documentation, so the Windows port layer copies them
 * field by field.
 */

#ifndef SDL_serial_engine_h_
#define SDL_serial_engine_h_

#include "SDL_serial_proto.h"

#define SDL_SERIAL_RETRY_MS      1000
#define SDL_SERIAL_CLOSE_MS      3000 /* The longest a close sequence keeps the port */
#define SDL_SERIAL_QUEUE_ENTRIES 64
#define SDL_SERIAL_READ_SIZE     256
#define SDL_SERIAL_MAX_PORTS     16
#define SDL_SERIAL_KEY_LENGTH    201 /* A device instance ID has at most 200 characters */

/* DCB values */
#define SDL_SERIAL_NOPARITY          0
#define SDL_SERIAL_ODDPARITY         1
#define SDL_SERIAL_EVENPARITY        2
#define SDL_SERIAL_ONESTOPBIT        0
#define SDL_SERIAL_TWOSTOPBITS       2
#define SDL_SERIAL_DTR_CONTROL_DISABLE 0
#define SDL_SERIAL_DTR_CONTROL_ENABLE  1
#define SDL_SERIAL_RTS_CONTROL_DISABLE   0
#define SDL_SERIAL_RTS_CONTROL_ENABLE    1
#define SDL_SERIAL_RTS_CONTROL_HANDSHAKE 2
#define SDL_SERIAL_RTS_CONTROL_TOGGLE    3

/* COMMTIMEOUTS: a read returns at once with buffered bytes, waits for the
 * first byte when none are buffered, and gives up after 1000 ms */
#define SDL_SERIAL_MAXDWORD                0xFFFFFFFFu
#define SDL_SERIAL_READ_TOTAL_CONSTANT_MS  1000
#define SDL_SERIAL_WRITE_TOTAL_MULTIPLIER  20
#define SDL_SERIAL_WRITE_TOTAL_CONSTANT_MS 1000

/* PurgeComm: PURGE_TXABORT 0x1, PURGE_RXABORT 0x2, PURGE_TXCLEAR 0x4, PURGE_RXCLEAR 0x8 */
#define SDL_SERIAL_PURGE_ALL 0x000Fu

/* EscapeCommFunction. SETBREAK and CLRBREAK do what SetCommBreak and
 * ClearCommBreak do. */
#define SDL_SERIAL_SETRTS   3
#define SDL_SERIAL_CLRRTS   4
#define SDL_SERIAL_SETDTR   5
#define SDL_SERIAL_CLRDTR   6
#define SDL_SERIAL_SETBREAK 8
#define SDL_SERIAL_CLRBREAK 9

typedef struct SDL_SerialLineConfig
{
    uint32_t BaudRate;
    uint8_t ByteSize;
    uint8_t Parity;
    uint8_t StopBits;
    bool fBinary;
    bool fParity;
    bool fOutxCtsFlow;
    bool fOutxDsrFlow;
    uint8_t fDtrControl;
    bool fDsrSensitivity;
    bool fTXContinueOnXoff;
    bool fOutX;
    bool fInX;
    bool fErrorChar;
    bool fNull;
    uint8_t fRtsControl;
    bool fAbortOnError;
} SDL_SerialLineConfig;

typedef struct SDL_SerialTimeouts
{
    uint32_t ReadIntervalTimeout;
    uint32_t ReadTotalTimeoutMultiplier;
    uint32_t ReadTotalTimeoutConstant;
    uint32_t WriteTotalTimeoutMultiplier;
    uint32_t WriteTotalTimeoutConstant;
} SDL_SerialTimeouts;

extern void SDL_Serial_BuildLineConfig(const SDL_SerialLine *line, SDL_SerialLineConfig *config);
extern void SDL_Serial_GetTimeouts(SDL_SerialTimeouts *timeouts);

/* One published snapshot. generation tells a sub-device's instances apart.
 * The port layer numbers the entries it queues in sequence. */
typedef struct SDL_SerialQueueEntry
{
    int sub;
    uint32_t generation;
    uint64_t stamp_ns;
    uint64_t sequence;
    SDL_SerialControls controls;
} SDL_SerialQueueEntry;

/* The hand-off from the port thread to the joystick thread. When it is full
 * the oldest entry goes. The port layer guards it with a mutex. */
typedef struct SDL_SerialSnapshotQueue
{
    SDL_SerialQueueEntry entries[SDL_SERIAL_QUEUE_ENTRIES];
    int head;
    int count;
    uint32_t dropped;
} SDL_SerialSnapshotQueue;

extern void SDL_Serial_ClearQueue(SDL_SerialSnapshotQueue *queue);
extern void SDL_Serial_PushQueue(SDL_SerialSnapshotQueue *queue, const SDL_SerialQueueEntry *entry);
extern bool SDL_Serial_PopQueue(SDL_SerialSnapshotQueue *queue, SDL_SerialQueueEntry *entry);

typedef enum SDL_SerialIO
{
    SDL_SERIAL_IO_DONE,
    SDL_SERIAL_IO_PENDING, /* Completion arrives through SDL_SerialEngine_WriteDone */
    SDL_SERIAL_IO_FAILED
} SDL_SerialIO;

typedef struct SDL_SerialPortOps
{
    /* Port calls, all on the port thread. A false return is port loss,
     * except that Escape refusing a break, or the end of one while no break
     * is held, finishes the break action as failed. */
    bool (*Open)(void *userdata);
    bool (*SetLine)(void *userdata, const SDL_SerialLineConfig *config);
    bool (*SetTimeouts)(void *userdata, const SDL_SerialTimeouts *timeouts);
    bool (*Purge)(void *userdata, uint32_t flags);
    bool (*Escape)(void *userdata, uint32_t function);
    SDL_SerialIO (*Write)(void *userdata, const uint8_t *data, size_t length);
    bool (*Drain)(void *userdata);
    /* Cancels pending I/O, waits for it and closes the handle */
    void (*Close)(void *userdata);
    /* Hand-off: generation 0 means absent, any other value a presence that
     * differs from every earlier one of this sub-device */
    void (*Presence)(void *userdata, int sub, uint32_t generation, const SDL_SerialIdentity *identity);
    void (*Publish)(void *userdata, const SDL_SerialQueueEntry *entry);
    /* A module's log line. May be NULL. */
    void (*Log)(void *userdata, const char *text);
} SDL_SerialPortOps;

typedef struct SDL_SerialEngine
{
    const SDL_SerialPortOps *ops;
    void *userdata;
    const SDL_SerialModule *module;
    void *state; /* module->state_size bytes */
    bool open;
    bool configured; /* The first line is set, the timeouts too, and the port purged */
    bool busy;       /* A write is pending */
    bool breaking;   /* The line is held in a break */
    SDL_SerialLine line;
    uint64_t retry_at; /* ms, while closed */
    uint64_t now;      /* ms of the current call */
    uint64_t stamp;    /* ns of the current call */
    bool present[SDL_SERIAL_MAX_SUBDEVICES];
    uint32_t generation[SDL_SERIAL_MAX_SUBDEVICES];
    uint32_t opens;
    uint32_t losses;
    bool stopping;    /* Closing for good: the port never opens again */
    uint64_t stop_at; /* ms, the end of the time a close sequence has */
    bool stalled;     /* The last call ran out of steps, see SDL_SerialEngine_GetDeadline */
} SDL_SerialEngine;

/* Closed, with the first open due at once */
extern void SDL_SerialEngine_Init(SDL_SerialEngine *engine, const SDL_SerialPortOps *ops, void *userdata, const SDL_SerialModule *module, void *state, uint64_t now_ns);
/* Opens the port when due, runs due deadlines and queued actions */
extern void SDL_SerialEngine_Run(SDL_SerialEngine *engine, uint64_t now_ns);
extern void SDL_SerialEngine_Received(SDL_SerialEngine *engine, const uint8_t *data, size_t length, uint64_t now_ns);
/* A pending write finished. success is false when not every byte was written. */
extern void SDL_SerialEngine_WriteDone(SDL_SerialEngine *engine, bool success, uint64_t now_ns);
/* A read, write or completion failed: the port is gone */
extern void SDL_SerialEngine_Lost(SDL_SerialEngine *engine, uint64_t now_ns);
/* A COM port arrived or left: a closed port retries at once */
extern void SDL_SerialEngine_Rescan(SDL_SerialEngine *engine, uint64_t now_ns);
extern void SDL_SerialEngine_Output(SDL_SerialEngine *engine, const SDL_SerialOutput *request, uint64_t now_ns);
/* Earliest time in ms at which Run must be called. False when none. After a
 * call that ran out of steps with work left it is 1 ms after that call. */
extern bool SDL_SerialEngine_GetDeadline(const SDL_SerialEngine *engine, uint64_t *deadline);
/* The port layer keeps a read posted while this is true */
extern bool SDL_SerialEngine_IsReading(const SDL_SerialEngine *engine);
/* Closes an open port and clears presence */
extern void SDL_SerialEngine_Stop(SDL_SerialEngine *engine);
/* Starts closing the port for good. A module whose base asks for a close
 * sequence runs it first: the port stays open, read and written, until the
 * module sets closed or SDL_SERIAL_CLOSE_MS pass. True while the sequence
 * runs, false when the port closed at once. */
extern bool SDL_SerialEngine_BeginStop(SDL_SerialEngine *engine, uint64_t now_ns);
/* A close sequence runs */
extern bool SDL_SerialEngine_IsStopping(const SDL_SerialEngine *engine);

/* SDL_HINT_JOYSTICK_SERIAL: a comma-separated list of PORT=PROTOCOL
 * entries. PORT is COMn, case-insensitive, or a device instance ID prefix
 * containing a backslash. */
typedef struct SDL_SerialPortEntry
{
    char key[SDL_SERIAL_KEY_LENGTH];
    const SDL_SerialModule *module;
} SDL_SerialPortEntry;

typedef void (*SDL_SerialLogFunc)(void *userdata, const char *entry, size_t length, const char *reason);

/* Spaces around tokens are ignored. A malformed entry or an unknown protocol
 * is skipped with one call to log. A repeated port keeps its last entry.
 * Returns the number of entries. */
extern int SDL_Serial_ParseHint(const char *hint, const SDL_SerialModule *const *modules, int nmodules, SDL_SerialPortEntry *entries, int max_entries, SDL_SerialLogFunc log, void *userdata);
/* COMn with n from 1 to 9999 and no leading zero */
extern bool SDL_Serial_IsComKey(const char *key, unsigned int *number);
extern bool SDL_Serial_KeysEqual(const char *a, const char *b);
/* Whether a device instance ID starts with the prefix, ignoring case */
extern bool SDL_Serial_InstanceMatches(const char *prefix, const char *instance_id);

/* A port whose device identifies itself: the COM port is an interface of
 * the device, so its device instance ID names the device. Parts that add
 * such devices add rows to the driver's table. */
typedef struct SDL_SerialAutoRule
{
    const char *prefix; /* Device instance ID prefix, where ? matches any one character */
    const char *token;  /* Protocol token */
    uint16_t vendor_id;
    uint16_t product_id; /* 0 takes the PID from the instance ID */
} SDL_SerialAutoRule;

/* The first rule whose prefix starts the instance ID, ignoring case, or NULL */
extern const SDL_SerialAutoRule *SDL_Serial_MatchAuto(const SDL_SerialAutoRule *rules, int nrules, const char *instance_id);

/* The VID_ and PID_ fields of a USB device instance ID, four hex digits
 * each. False when either is missing or malformed. */
extern bool SDL_Serial_ParseUSBIds(const char *instance_id, uint16_t *vendor_id, uint16_t *product_id);

typedef enum SDL_SerialPortChange
{
    SDL_SERIAL_PORT_KEEP,
    SDL_SERIAL_PORT_START,
    SDL_SERIAL_PORT_RESTART, /* Same port, another protocol */
    SDL_SERIAL_PORT_STOP
} SDL_SerialPortChange;

/* old_changes gets KEEP, RESTART or STOP for each running port, and
 * new_changes START or KEEP for each entry of the new hint */
extern void SDL_Serial_DiffPorts(const SDL_SerialPortEntry *old_entries, int nold, const SDL_SerialPortEntry *new_entries, int nnew, SDL_SerialPortChange *old_changes, SDL_SerialPortChange *new_changes);

/* Decisions of the Windows port layer, kept here so the offline tests reach
 * them */

#define SDL_SERIAL_OUTPUT_QUEUE    16          /* Effect requests a port holds in order */
#define SDL_SERIAL_IDLE_POLL_MS    10          /* The next read after a read that brought nothing */
#define SDL_SERIAL_WAIT_FOREVER    0xFFFFFFFFu /* The Windows INFINITE */
#define SDL_SERIAL_GUID_KEY_LENGTH (SDL_SERIAL_NAME_LENGTH + 6)

/* Output requests on their way from the joystick thread to the port thread.
 * A rumble, and an effect of a module that does not set queue_effects, waits
 * in one slot per sub-device, where a newer request replaces an older one.
 * The effects of a module that sets queue_effects wait in order. The port
 * layer guards it with a mutex. */
typedef struct SDL_SerialOutbox
{
    SDL_SerialOutput latest[SDL_SERIAL_MAX_SUBDEVICES];
    bool latest_set[SDL_SERIAL_MAX_SUBDEVICES];
    SDL_SerialOutput queued[SDL_SERIAL_OUTPUT_QUEUE];
    int head;
    int count;
} SDL_SerialOutbox;

extern void SDL_Serial_ClearOutbox(SDL_SerialOutbox *outbox);
/* False, keeping nothing, when a queued effect finds SDL_SERIAL_OUTPUT_QUEUE
 * requests waiting */
extern bool SDL_Serial_PostOutput(SDL_SerialOutbox *outbox, const SDL_SerialModule *module, const SDL_SerialOutput *request);
/* The next request for SDL_SerialEngine_Output, false when none: the slots
 * first, then the oldest queued effect. A queued effect waits while the
 * engine's module has an action queued, so each effect reaches the module
 * once the one before it has left, and the module's action queue never
 * overflows. A port that does not read, or that closes, drops every request
 * at once. */
extern bool SDL_Serial_TakeOutput(SDL_SerialOutbox *outbox, const SDL_SerialEngine *engine, SDL_SerialOutput *request);

typedef enum SDL_SerialEffectCheck
{
    SDL_SERIAL_EFFECT_OK,
    SDL_SERIAL_EFFECT_UNSUPPORTED, /* The module takes no effects */
    SDL_SERIAL_EFFECT_BAD_SIZE,    /* Outside the module's range, or longer than SDL_SERIAL_MAX_EFFECT */
    SDL_SERIAL_EFFECT_INVALID      /* Refused by the module's ValidEffect */
} SDL_SerialEffectCheck;

/* Whether a module takes an effect of these bytes */
extern SDL_SerialEffectCheck SDL_Serial_CheckEffect(const SDL_SerialModule *module, const uint8_t *data, size_t length);

/* The port thread's wait in ms before it runs the engine again: until the
 * engine's deadline, or SDL_SERIAL_WAIT_FOREVER when it has none. read_posted
 * says a read is out, and read_idle that the last read finished at once with
 * nothing. While the engine reads and no read is out, the port thread reads
 * again: at once after reads that brought bytes, since the port layer stops
 * after sixteen of them in a row to see its events, and within
 * SDL_SERIAL_IDLE_POLL_MS after one that brought nothing. */
extern uint32_t SDL_SerialEngine_GetWait(const SDL_SerialEngine *engine, uint64_t now, bool read_posted, bool read_idle);

/* The product name in the joystick GUID of a port the hint names. Such a GUID
 * has vendor 0, so SDL_CreateJoystickGUID keeps only the first 9 bytes of
 * the name, and SDL stores an automatic gamepad mapping without the name's
 * CRC. By name alone the players of one JVS board shared player 1's mapping.
 * The key is the CRC-16 of the name (SDL_crc16) as four hex digits, the
 * sub-device as one, a space, then the name, so the kept bytes differ for
 * every name and sub-device. SDL_SERIAL_GUID_KEY_LENGTH bytes hold any key,
 * and a smaller buffer truncates the name. */
extern void SDL_Serial_GUIDKey(char *key, size_t size, uint16_t name_crc, int sub, const char *name);

/* The stamp of a joystick's next event: stamp, or the stamp given out last
 * when that is later, so the events of one joystick never go back in time.
 * last holds the stamp given out last, and moves to the result. */
extern uint64_t SDL_Serial_EventStamp(uint64_t *last, uint64_t stamp);

#endif /* SDL_serial_engine_h_ */
