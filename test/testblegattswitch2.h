/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* The scripted Switch 2 of testblegattdriver. testblegattswitch2.c answers
   the Switch 2 driver's transport calls with it, and testblegattdriver.c's
   scenarios arm it, push its reports and read what the driver wrote. */

#ifndef testblegattswitch2_h_
#define testblegattswitch2_h_

/* A Switch 2's characteristics, by what they carry */
enum
{
    SW2_UNIFIED_INPUT,     /* ab7de9be-89fe-49ad-828f-118f09df7fd2 */
    SW2_COMMAND,           /* 649d4ac9-8eb7-4e6c-af44-1ea54fe5f005 */
    SW2_RESPONSE,          /* c765a961-d9d8-4d36-a20a-5315b111836a */
    SW2_VIBRATION,         /* the type's vibration output, handle 0x0012 */
    SW2_CONSOLE_COMMAND,   /* the side's command, handle 0x0016 */
    SW2_NATIVE_INPUT,      /* the side's input, handle 0x000E */
    SW2_EXTENDED_RESPONSE, /* the side's extended response, handle 0x001E */
    SW2_SESSION_START,     /* 00c5af5d-1964-4e30-8f51-1956f96bd282 */
    SW2_CHARACTERISTICS
};

typedef enum SW2Kind
{
    SW2_GENUINE,    /* streams on the unified input and answers 649d4ac9 */
    SW2_CLONE,      /* a NYXI Hyperion 3: answers only the console channel */
    SW2_CLONE_ULTRA /* a Hyperion 3 Ultra: also answers 649d4ac9, never streams there */
} SW2Kind;

typedef struct SW2Script
{
    Uint64 address;
    Uint16 product;
    SW2Kind kind;
    bool extended_replies;      /* console replies on the side's extended response, else on c765a961 */
    const Uint8 *factory1;      /* 9 bytes at 0x130A8, NULL for erased flash */
    const Uint8 *user1;         /* 11 bytes at 0x1FC040 */
    const Uint8 *factory2;      /* 9 bytes at 0x130E8 */
    const Uint8 *user2;         /* 11 bytes at 0x1FC080 */
    bool vendor_before_replies; /* an EA vendor frame on c765a961 before each reply */
    bool decoys_before_replies; /* before each reply, on its characteristic, notifications that are not it */
    Uint32 slow_console_read;   /* a read of this address on the console channel answers after SW2_SLOW_REPLY_MS */
    int reply_ms;               /* how long every other reply takes, 0 for SW2_REPLY_MS */
    bool no_native_input;       /* the side's input is missing */
    const Uint8 *auto_report;   /* a genuine pad's first unified report, 30 ms after its notifications are on */
    int auto_report_length;
    bool paired;                /* Windows holds a pairing with it (SDL_BLEGATT_IsPaired) */
    bool no_session;            /* Windows gives no GattSession for it (SDL_BLEGATT_OpenSession) */
    int stale_discoveries;      /* discovery tries that find no service once the link is back */
    int stale_services;         /* once the link is back, service lookups that find the service with no
                                   characteristics, as a stale Windows GATT session gives them (FlexInput
                                   crates/joycon2/src/hub.rs:790-805 at 0905b2fe) */
    Uint32 stale_missing_roles; /* the characteristics a stale service hides, one bit per SW2_*, 0 for all */
    bool silent_reads_after_restore; /* on a link that came back, memory reads go unanswered */
    bool silent_reads;          /* memory reads go unanswered */
    bool input_notify_fails;    /* the unified input's notifications fail on a link that holds */
    int input_notify_failures_after_restore; /* on each link that came back, the first enables of the unified input that fail */
} SW2Script;

typedef enum SW2EventKind
{
    SW2_EVENT_WRITE,         /* a characteristic write */
    SW2_EVENT_NOTIFY,        /* notifications enabled */
    SW2_EVENT_HANDLER,       /* a value handler added */
    SW2_EVENT_DESCRIPTOR,    /* a descriptor write */
    SW2_EVENT_STATUS,        /* the status handler added */
    SW2_EVENT_HANDLER_AGAIN, /* a handler the driver added before, registered on a characteristic */
    SW2_EVENT_SESSION_OPEN,  /* the GattSession given out */
    SW2_EVENT_SESSION_CLOSE, /* the GattSession closed */
    SW2_EVENT_LINK           /* the link dropped (data 00) or came back (data 01) */
} SW2EventKind;

typedef struct SW2Event
{
    SW2EventKind kind;
    int characteristic; /* SW2_*, -1 for the device */
    int generation;     /* the link whose objects took the call: 0 for the first, -1 for the device */
    bool response;      /* a write that asked for a response */
    bool report_rate;   /* a descriptor write to 679d5510 */
    Uint8 data[96];
    int length;
    Uint64 ms;          /* SW2_Now when the call arrived */
} SW2Event;

#define SW2_MAX_EVENTS 1024

/* How long a reply takes unless the script says otherwise */
#define SW2_REPLY_MS 5

/* Longer than the unified path's 500 ms reply wait and shorter than the
   console channel's 700 ms (BLE_CONSOLE_REPLY_MS) */
#define SW2_SLOW_REPLY_MS 600

/* A call the scripted Switch 2 can hold (SW2_HangAt) */
typedef enum SW2HangCall
{
    SW2_HANG_FIND_SERVICE, /* a service lookup: runs until its cancel flag is set or the time passes, then finds nothing */
    SW2_HANG_IS_CONNECTED, /* a status read: takes the whole time, since SDL_BLEGATT_IsConnected takes no cancel flag */
    SW2_HANG_IS_CONNECTED_UP, /* the same, counted among the status reads that find the link up */
    SW2_HANG_IS_PAIRED     /* the pairing read at connect: takes the whole time, as SDL_BLEGATT_IsPaired takes no flag */
} SW2HangCall;

extern void SW2_Setup(void);
extern void SW2_Cleanup(void);
/* Puts the device in range for the driver's transport calls */
extern void SW2_Arm(const SW2Script *script);
/* Takes it away again, after delivering what is queued. Call after SDL_Quit. */
extern void SW2_Disarm(void);
/* Calls of the Switch 2 helpers, armed or not */
extern int SW2_HelperCalls(void);
extern void SW2_ResetHelperCalls(void);
/* Notes that SDL_Quit has returned. Any call of a helper or of a fake
   object after this counts in SW2_CallsAfterQuit. */
extern void SW2_NoteQuit(void);
extern int SW2_CallsAfterQuit(void);
/* Copies up to max events, in order. Returns how many there are. */
extern int SW2_Events(SW2Event *events, int max);
/* Delivers a value on the characteristic, as the radio would. False when the
   device would not send it there: the link down, no handler, notifications
   off, or for the console input a session not yet complete. */
extern bool SW2_Push(int characteristic, const Uint8 *data, int length);
/* Drops the link, as a controller that went out of range or hung up. The
   driver's status handler sees Disconnected, and every object of the link
   closes, as WinRT closes them (RO_E_CLOSED). The link comes back
   restore_ms later with new objects, as Windows connects it again while a
   GattSession holds MaintainConnection, or never when restore_ms is
   negative. False when no status handler is registered. */
extern bool SW2_DropLink(int restore_ms);
/* Once the link next comes back, it drops again at the driver's count-th
   call of that kind on it, and comes back restore_ms after that. A write
   (SW2_EVENT_WRITE) finds the link dropped and fails. A notification enable
   (SW2_EVENT_NOTIFY) succeeds, then the link drops, as one that drops right
   after the driver's last call. */
extern void SW2_DropAgainAt(SW2EventKind kind, int count, int restore_ms);
/* The same on the link up now */
extern void SW2_DropAt(SW2EventKind kind, int count, int restore_ms);
/* The link up now drops at the driver's count-th characteristic lookup on
   it, which then finds nothing, and comes back restore_ms after */
extern void SW2_DropAtLookup(int count, int restore_ms);
/* Helper calls that took a cancel parameter and got NULL */
extern int SW2_NullCancels(void);
/* The count-th call of that kind from now on hangs for up to ms */
extern void SW2_HangAt(SW2HangCall call, int count, int ms);
/* True while a hung call runs */
extern bool SW2_Hanging(void);
/* The scripted Switch 2's clock, in milliseconds: the performance counter,
   which keeps no SDL state, since a driver thread that SDL_Quit left to end
   on its own can call it after SDL_Quit */
extern Uint64 SW2_Now(void);
/* The times of the driver's status reads (SDL_BLEGATT_IsConnected) on
   SW2_Now's clock. Copies up to max. Returns how many there were. */
extern int SW2_StatusReads(Uint64 *times, int max);
/* The driver's lookups of the Switch 2 service, whatever they found */
extern int SW2_ServiceLookups(void);
/* True while the link is up */
extern bool SW2_Connected(void);
/* References the driver holds on the device's objects and its session */
extern int SW2_References(void);
/* Value handlers still registered on a characteristic */
extern int SW2_Registrations(void);
/* Calls that broke the device's rules: a release too many, a console
   command without its 17 zero bytes, a frame whose length byte is wrong */
extern int SW2_BadCalls(void);
/* Writes, notifications, descriptor writes and handler registrations on the
   objects of a link that dropped, each of which failed. A write that
   SW2_DropAt or SW2_DropAgainAt drops the link under is not one. */
extern int SW2_ClosedCalls(void);
/* Memory reads the device answered */
extern int SW2_Reads(void);

#endif /* testblegattswitch2_h_ */
