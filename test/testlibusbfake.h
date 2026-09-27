/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* The control interface of the fake libusb-1.0.dll that test/libusb-backend
   builds from testlibusbfake.c. The fake serves two devices on bus 1. On
   port 3 is an Intel Wireless Series base station, 8086:C013, with the
   descriptors JoypadOS records (joypad-os docs/INTEL_WIRELESS_SERIES.md):
   interface 0 is a boot keyboard at alternate 0 and the pads' interrupt IN
   0x81 of 27 bytes and interrupt OUT 0x01 of 25 bytes at alternate 1, and
   interface 1 is a boot mouse. libusb's path for interface 0 is
   FAKE_LIBUSB_INTEL_PATH. On port 4 is a DJI RC (RM330), 2CA3:1023, whose
   DUML interface 1 has bulk OUT 0x02 and bulk IN 0x83 of 512 bytes, at
   FAKE_LIBUSB_DJI_PATH. A queued packet goes to the first IN transfer
   pending, so a test opens one device at a time. Control transfers are
   answered only as the test sets them. The test finds these
   functions with GetProcAddress. */

#ifndef testlibusbfake_h_
#define testlibusbfake_h_

#define FAKE_LIBUSB_INTEL_PATH   "1-3:1.0"
#define FAKE_LIBUSB_DJI_PATH     "1-4:1.1"
#define FAKE_LIBUSB_MAX_WRITES   64
#define FAKE_LIBUSB_MAX_DATA     32
#define FAKE_LIBUSB_MAX_CONTROLS 64

typedef struct FakeLibUSB_Counts
{
    int opens;          /* libusb_open */
    int closes;         /* libusb_close */
    int claims;         /* libusb_claim_interface */
    int releases;       /* libusb_release_interface */
    int alternates;     /* libusb_set_interface_alt_setting */
    int last_alternate; /* The alternate the last of those asked for */
    int submits;        /* libusb_submit_transfer */
    int writes;         /* Interrupt and bulk OUT transfers, recorded in order */
    int held;           /* OUT transfers that waited while writes were held */
    int writing;        /* OUT transfers running now */
    int writing_closes; /* libusb_close calls on a handle with an OUT transfer running */
    int inputs;         /* Input packets queued and not yet delivered */
    int controls;       /* Control transfers, recorded in order */
} FakeLibUSB_Counts;

typedef struct FakeLibUSB_Write
{
    unsigned char endpoint;
    int length;
    unsigned int timeout; /* In milliseconds, as libusb takes it */
    int result;           /* What the fake returned */
    unsigned char data[FAKE_LIBUSB_MAX_DATA];
    double ms;            /* When it arrived, on the performance counter */
    double done_ms;       /* When it returned. 0 while it runs. */
} FakeLibUSB_Write;

typedef struct FakeLibUSB_Control
{
    unsigned char request_type;
    unsigned char request;
    unsigned short value;
    unsigned short index;
    unsigned short length;  /* wLength */
    unsigned int timeout;   /* In milliseconds, as libusb_control_transfer takes it */
    int result;             /* What the fake returned */
    double ms;              /* When it arrived, on the performance counter */
    double done_ms;         /* When the fake returned */
} FakeLibUSB_Control;

/* Every counter, record and queued packet cleared, every knob back to
   success with writes let go, and no control request answered */
typedef void (*FakeLibUSB_ResetFunc)(void);
/* What SET_INTERFACE to any alternate but 0 returns: 0, or a libusb error */
typedef void (*FakeLibUSB_SetAlternateResultFunc)(int result);
/* What the next OUT transfer returns. It moves no bytes when the result is
   an error. Later transfers succeed. */
typedef void (*FakeLibUSB_FailNextWriteFunc)(int result);
/* 1 makes every OUT transfer wait, as on an endpoint that refuses data,
   until 0 lets them go or the transfer's timeout passes */
typedef void (*FakeLibUSB_HoldWritesFunc)(int hold);
/* One packet for the next IN transfer */
typedef int (*FakeLibUSB_QueueInputFunc)(const unsigned char *data, int length);
typedef void (*FakeLibUSB_GetCountsFunc)(FakeLibUSB_Counts *counts);
/* Returns 0 when that write was never made */
typedef int (*FakeLibUSB_GetWriteFunc)(int index, FakeLibUSB_Write *write);
/* How the fake answers a control transfer with this bmRequestType and
   bRequest: after delay_ms, with the first result bytes of reply when result
   is positive, else with result, a libusb error. Returns 0 when the table is
   full or the reply too long. */
typedef int (*FakeLibUSB_SetControlFunc)(unsigned char request_type, unsigned char request, int result,
                                         const unsigned char *reply, int delay_ms);
/* Returns 0 when that control transfer was never made */
typedef int (*FakeLibUSB_GetControlFunc)(int index, FakeLibUSB_Control *control);
/* Calls the model does not serve: string transfers, control transfers the
   test did not set, synchronous reads, kernel driver changes, hotplug,
   freeing a pending transfer, and closing a handle while an OUT transfer on
   it runs */
typedef int (*FakeLibUSB_GetUnexpectedFunc)(void);

#endif /* testlibusbfake_h_ */
