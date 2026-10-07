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
    bool no_native_input;       /* the side's input is missing */
    const Uint8 *auto_report;   /* a genuine pad's first unified report, 30 ms after its notifications are on */
    int auto_report_length;
} SW2Script;

typedef enum SW2EventKind
{
    SW2_EVENT_WRITE,      /* a characteristic write */
    SW2_EVENT_NOTIFY,     /* notifications enabled */
    SW2_EVENT_HANDLER,    /* a value handler added */
    SW2_EVENT_DESCRIPTOR, /* a descriptor write */
    SW2_EVENT_STATUS      /* the status handler added */
} SW2EventKind;

typedef struct SW2Event
{
    SW2EventKind kind;
    int characteristic; /* SW2_*, -1 for the device */
    bool response;      /* a write that asked for a response */
    bool report_rate;   /* a descriptor write to 679d5510 */
    Uint8 data[96];
    int length;
    Uint64 ms;          /* SDL_GetTicks when the call arrived */
} SW2Event;

#define SW2_MAX_EVENTS 256

/* Longer than the unified path's 500 ms reply wait and shorter than the
   console channel's 700 ms (BLE_CONSOLE_REPLY_MS) */
#define SW2_SLOW_REPLY_MS 600

extern void SW2_Setup(void);
extern void SW2_Cleanup(void);
/* Puts the device in range for the driver's transport calls */
extern void SW2_Arm(const SW2Script *script);
/* Takes it away again, after delivering what is queued. Call after SDL_Quit. */
extern void SW2_Disarm(void);
/* Calls of the Switch 2 helpers, armed or not */
extern int SW2_HelperCalls(void);
extern void SW2_ResetHelperCalls(void);
/* Copies up to max events, in order. Returns how many there are. */
extern int SW2_Events(SW2Event *events, int max);
/* Delivers a value on the characteristic, as the radio would. False when the
   device would not send it there: no handler, notifications off, or for the
   console input a session not yet complete. */
extern bool SW2_Push(int characteristic, const Uint8 *data, int length);
/* Sets the flag the driver's status handler holds, as a link that dropped */
extern bool SW2_LoseLink(void);
/* References the driver holds on the device's objects */
extern int SW2_References(void);
/* Calls that broke the device's rules: a release too many, a console
   command without its 17 zero bytes, a frame whose length byte is wrong */
extern int SW2_BadCalls(void);
/* Memory reads the device answered */
extern int SW2_Reads(void);

#endif /* testblegattswitch2_h_ */
