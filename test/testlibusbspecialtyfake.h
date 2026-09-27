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
   builds from testlibusbspecialtyfake.c for testlibusbspecialty.c. The fake
   serves one bulk device at a time on bus 1, port 3, the one
   FakeSpecialty_SelectDevice names, and libusb's path for its interface 0 is
   FAKE_SPECIALTY_PATH. Neither has string descriptors.

   - FAKE_SPECIALTY_TACX, the default: a Tacx T1904 head unit, 3561:1904.
     Interface 0 has bulk IN 0x82 and bulk OUT 0x02 of 16-byte packets, one
     of the sizes the head units' LPC2141 allows (NXP UM10139, Table 96). No
     source records the head units' descriptors or packet size.
   - FAKE_SPECIALTY_USIO: a Namco USIO, 0B9A:0910. Interface 0 has bulk OUT
     0x01 and bulk IN 0x82 of 64 bytes and interrupt IN 0x83 of 8 bytes, as
     RPCS3 describes it (rpcs3/Emu/Io/usio.cpp:68-122).

   The test finds these functions with GetProcAddress. */

#ifndef testlibusbspecialtyfake_h_
#define testlibusbspecialtyfake_h_

#define FAKE_SPECIALTY_PATH       "1-3:1.0"
#define FAKE_SPECIALTY_MAX_WRITES 16
#define FAKE_SPECIALTY_MAX_DATA   32

#define FAKE_SPECIALTY_TACX 0
#define FAKE_SPECIALTY_USIO 1

typedef struct FakeSpecialty_Counts
{
    int opens;      /* libusb_open */
    int closes;     /* libusb_close */
    int submits;    /* libusb_submit_transfer */
    int writes;     /* Interrupt and bulk OUT transfers, recorded in order */
    int inputs;     /* Input packets queued and not yet delivered */
    int in_timeout; /* The timeout of the last IN transfer submitted, in milliseconds */
    int closes_during_writes; /* libusb_close calls made while an OUT transfer ran */
} FakeSpecialty_Counts;

typedef struct FakeSpecialty_Write
{
    unsigned char endpoint;
    int length;
    unsigned int timeout; /* In milliseconds, as libusb_bulk_transfer takes it */
    int result;           /* What the fake returned */
    unsigned char data[FAKE_SPECIALTY_MAX_DATA];
    double ms;            /* When it arrived, on the performance counter */
} FakeSpecialty_Write;

/* Every counter, record and queued packet cleared, the Tacx head unit
   served, and every OUT transfer back to success */
typedef void (*FakeSpecialty_ResetFunc)(void);
/* The device the next enumeration and open find, FAKE_SPECIALTY_* */
typedef void (*FakeSpecialty_SelectDeviceFunc)(int device);
/* The next count OUT transfers wait out their whole timeout and fail with
   LIBUSB_ERROR_TIMEOUT, moving no bytes, as on an endpoint that NAKs every
   packet */
typedef void (*FakeSpecialty_NakWritesFunc)(int count);
/* A reply as the device sends it on its IN endpoint: whole packets, then the
   rest, all queued at once. A packet shorter than the endpoint's packet
   size, or one that fills the transfer, ends the IN transfer it lands in,
   and so does the transfer's timeout, which hands over the bytes the
   transfer holds. A reply that is a whole number of packets ends with a full
   packet and no zero-length packet. */
typedef int (*FakeSpecialty_QueueReplyFunc)(const unsigned char *data, int length);
typedef void (*FakeSpecialty_GetCountsFunc)(FakeSpecialty_Counts *counts);
/* Returns 0 when that write was never made */
typedef int (*FakeSpecialty_GetWriteFunc)(int index, FakeSpecialty_Write *write);
/* Calls the model does not serve: string and control transfers, synchronous
   reads, alternate settings, kernel driver changes, hotplug, a packet longer
   than the room left in its transfer, and freeing a pending transfer */
typedef int (*FakeSpecialty_GetUnexpectedFunc)(void);

#endif /* testlibusbspecialtyfake_h_ */
