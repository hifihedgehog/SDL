/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_HIDAPI_DriverWii, the HIDAPI Wii driver, against a scripted Wii
   Remote, for hifihedgehog/SDL#36. No HID device opens and the rumble thread
   never starts: the driver's reads and writes, its rumble-thread writes and
   its joystick connections go to the fake below, which answers the way a
   Wii Remote does. The test calls the driver's functions on a hand-built
   device and joystick record, under SDL's joystick lock, as the HIDAPI layer
   and SDL_OpenJoystick do, and enables sensors through
   SDL_SetJoystickSensorEnabled, which reaches the driver only for a sensor
   the joystick registered.

   The remote's register map follows Linux's hid-wiimote-core.c (extension
   IDs, the Motion Plus at 0xA600FA and its pass-through modes) and Dolphin's
   Motion Plus (the active address and stored child ID). The reports follow
   Dolphin's layouts: the core buttons (WiimoteEmu.h), basic IR (Camera.h),
   the Classic Controller (Classic.h), the Motion Plus (MotionPlus.h) and the
   Classic's pass-through changes (MotionPlus.cpp). The camera sequence
   follows WiimoteLib's EnableIR.

   1. A remote with a Classic Controller or Classic Controller Pro registers
      SDL_SENSOR_ACCEL, and SDL_SENSOR_GYRO when a Motion Plus is present, as
      the bare remote and the Nunchuk do. The Wii U Pro Controller, the
      Balance Board and an unrecognized extension register none. No
      configuration powers the camera at open.
   2. Enabling the accelerometer powers the camera with the bare remote's
      sequence in Basic mode and selects report 0x37. With a Motion Plus the
      Motion Plus first runs in Classic pass-through. A sensor the joystick
      lacks never reaches the driver.
   3. A 0x37 report with accelerometer bytes, two IR dots and a Classic
      Controller's 6 bytes posts one accelerometer sample, the four IR axes,
      the Classic's buttons, sticks, triggers and D-pad, and the remote's
      buttons. Behind a Motion Plus, a gyro frame posts SDL_SENSOR_GYRO and a
      pass-through frame posts the Classic's controls.
   4. Disabling the accelerometer turns the camera off and returns to report
      0x32, after deactivating the Motion Plus where one was running.

   For hifihedgehog/SDL#39 the remote also follows Dolphin's camera, which
   refuses register writes with error 7 until report 0x13 has enabled it and
   reports its state in bit 3 of a status report's flags
   (EmuSubroutines.cpp:304-310, :348-356, Camera.cpp:41-49), and Dolphin's
   Motion Plus, whose detect pin changes raise a status report
   (MotionPlus.cpp:371-387, WiimoteEmu.cpp:431-447). The driver's clock can
   skip ahead, and the remote can power off, lose acknowledgments or take
   queued writes late, as the rumble thread delivers them.

   5. The camera's enable reports reach the remote before its register
      writes, even when queued writes arrive late.
   6. A camera start that fails runs again after 1, 2 and 4 seconds, then
      every 8, logging the register and the error, and a start that holds
      ends the retries. A write without an acknowledgment goes again after
      250 ms, and four sends without one fail the start.
   7. A status report that shows the camera off while sensors are on starts
      it again, except inside a Motion Plus settle window. A start that
      landed is checked with a status request once that window passes.
   8. A remote with no input for 3 seconds gets a status request. One that
      takes it stays, and its status report restarts the data reports. One
      that refuses it loses its joystick and keeps its device, which asks
      again every 3 seconds, and the remote's first report brings a new
      joystick from a fresh context. So does a path that refuses the
      request at InitDevice. A read error ends the joystick and sets
      reads_ended.
   9. Across a Motion Plus attach and detach the remote streams the data
      report the driver asked for, and its buttons post, also while the
      driver still believes a Motion Plus active that sends raw extension
      frames.

   For hifihedgehog/SDL#40 the remote acknowledges reports 0x13 and 0x1a
   when byte 1 asks (Dolphin WiimoteReport.h:46-53, EmuSubroutines.cpp:340-360),
   and can hold its acknowledgments back, leave the camera's unanswered, or
   report the camera off whatever it does.

   10. The camera's writes go through the rumble thread one at a time, each
       once the remote acknowledged the one before it, and the last one
       selects report 0x33, or 0x37 with an extension or a Motion Plus. An
       acknowledgment of another report moves nothing. The remote's reports
       and the acknowledgments post its buttons meanwhile.
   11. A camera that never acknowledges, or acknowledges every write and
       reports the camera off, has its start retried on the schedule while
       every update returns at once.
   12. No sensor call waits on the remote, with or without a Motion Plus. A
       status report from before a Motion Plus deactivation adds no
       joystick, a sensor change or a close stops a start under way, and a
       write still queued on the rumble thread holds its patience.
   13. A Motion Plus activation that waits for a camera write goes once the
       start lands, its check is armed meanwhile, and the camera's check
       waits out the activation's settle window.

   The driver in this file calls the fake, not SDL's HID backends. Each name
   through which it reads, writes, queues a write or adds a joystick is
   defined below before the driver source is included. This object defines
   SDL_HIDAPI_DriverWii, so SDL's own copy of the driver is never linked, as
   in the iCade driver's test. Before the link,
   test/wii-driver/CheckSystem.cmake reads this object's symbols and fails
   the build when it still imports a function that reaches a HID device. */

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"
#include "SDL_hints_c.h"
#include "joystick/SDL_sysjoystick.h"
#include "joystick/hidapi/SDL_hidapijoystick_c.h"
#include "joystick/hidapi/SDL_hidapi_wii_ext_proto.h"

/* The fake, defined below. SDL_hidapi_rumble.h and SDL_hidapi_nintendo.h
   have no include guard, so only the driver includes them, after these
   names: the rumble header's declarations then name the fake, and keep the
   internal linkage declared here. */
static int Fake_hid_read_timeout(SDL_hid_device *dev, unsigned char *data, size_t length, int milliseconds);
static int Fake_hid_write(SDL_hid_device *dev, const unsigned char *data, size_t length);
static bool Fake_LockRumble(void);
static int Fake_SendRumbleAndUnlock(SDL_HIDAPI_Device *device, const Uint8 *data, int size);
static bool Fake_JoystickConnected(SDL_HIDAPI_Device *device, SDL_JoystickID *pJoystickID);
static void Fake_JoystickDisconnected(SDL_HIDAPI_Device *device, SDL_JoystickID joystickID);
static SDL_Joystick *Fake_GetJoystickFromID(SDL_JoystickID instance_id);
static Uint64 Fake_GetTicks(void);
static void Fake_Delay(Uint32 ms);

#undef SDL_hid_read_timeout
#define SDL_hid_read_timeout Fake_hid_read_timeout
#undef SDL_hid_write
#define SDL_hid_write Fake_hid_write
#define SDL_HIDAPI_LockRumble Fake_LockRumble
#define SDL_HIDAPI_SendRumbleAndUnlock Fake_SendRumbleAndUnlock
#define HIDAPI_JoystickConnected Fake_JoystickConnected
#define HIDAPI_JoystickDisconnected Fake_JoystickDisconnected
#undef SDL_GetJoystickFromID
#define SDL_GetJoystickFromID Fake_GetJoystickFromID
#undef SDL_GetTicks
#define SDL_GetTicks Fake_GetTicks
#undef SDL_Delay
#define SDL_Delay Fake_Delay

#include "../src/joystick/hidapi/SDL_hidapi_wii.c"

#undef SDL_hid_read_timeout
#undef SDL_hid_write
#undef SDL_HIDAPI_LockRumble
#undef SDL_HIDAPI_SendRumbleAndUnlock
#undef HIDAPI_JoystickConnected
#undef HIDAPI_JoystickDisconnected
#undef SDL_GetJoystickFromID
#undef SDL_GetTicks
#define SDL_GetTicks SDL_GetTicks_REAL
#undef SDL_Delay
#define SDL_Delay SDL_Delay_REAL

#include <stdio.h>

static int checks;
static int failures;
static const char *scenario = "";

#define CHECK(condition, ...)                                 \
    do {                                                      \
        ++checks;                                             \
        if (!(condition)) {                                   \
            ++failures;                                       \
            printf("FAILED line %d (%s): ", __LINE__, scenario); \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

/* The scripted remote */
#define FAKE_MAX_REPORTS 64
#define FAKE_MAX_WRITES  512
#define FAKE_REPORT_SIZE 22

typedef enum FakeExtension
{
    FAKE_EXT_NONE,
    FAKE_EXT_NUNCHUK,
    FAKE_EXT_CLASSIC,
    FAKE_EXT_CLASSIC_PRO,
    FAKE_EXT_WIIUPRO,
    FAKE_EXT_BALANCE_BOARD,
    FAKE_EXT_UNRECOGNIZED
} FakeExtension;

/* The six ID bytes at 0xA400FA (hid-wiimote-core.c:447-453, Dolphin
   WiimoteController.cpp:723-737). Byte 0 tells the Classic Controller Pro
   apart, and 09 09 is no extension SDL knows. */
static const Uint8 fake_extension_ids[][6] = {
    { 0 },
    { 0x00, 0x00, 0xA4, 0x20, 0x00, 0x00 },
    { 0x00, 0x00, 0xA4, 0x20, 0x01, 0x01 },
    { 0x01, 0x00, 0xA4, 0x20, 0x01, 0x01 },
    { 0x00, 0x00, 0xA4, 0x20, 0x01, 0x20 },
    { 0x00, 0x00, 0xA4, 0x20, 0x04, 0x02 },
    { 0x00, 0x00, 0xA4, 0x20, 0x09, 0x09 }
};

typedef struct FakeOutput
{
    bool async;   /* Queued through the rumble thread */
    bool refused; /* Sent while the remote was off */
    Uint64 ticks; /* The driver's clock when it sent it */
    int length;
    Uint8 data[FAKE_REPORT_SIZE];
} FakeOutput;

static struct
{
    FakeExtension extension;
    bool motion_plus;       /* Present, as in every Wii Remote Plus */
    Uint8 motion_plus_mode; /* 0 while inactive, else the pass-through mode it runs */
    Uint8 report_mode;      /* The data report it streams, 0 after a status report */
    bool camera;            /* Output report 0x13 */
    bool camera_logic;      /* Output report 0x1A */
    Uint8 camera_mode;      /* Register 0xB00033 */
    Uint8 buttons[2];       /* The core buttons every report carries */
    bool off;               /* Powered off: every write fails and nothing arrives */
    bool defer_queued;      /* Queued writes reach the remote at the driver's next read */
    int camera_nacks;       /* Camera register writes still to refuse with error 7 */
    Uint32 nack_address;    /* The register camera_nacks refuses, 0 for any */
    int lost_acks;          /* Write acknowledgments still to lose */
    int lost_replies;       /* Read replies still to lose */
    bool mute;              /* Takes every write and answers none */
    int read_errors;        /* Reads still to fail */
    int activation_nacks;   /* Motion Plus mode writes still to refuse with error 7 */
    bool hold_acks;         /* Acknowledgments wait until the test releases them */
    bool camera_silent;     /* No acknowledgment for any camera report or register write */
    bool camera_bit_clear;  /* Status reports show the camera off whatever it does */
    Uint8 held[FAKE_MAX_REPORTS][5];
    int nheld;
    Uint8 queue[FAKE_MAX_REPORTS][FAKE_REPORT_SIZE];
    int queue_length[FAKE_MAX_REPORTS];
    int queue_head;
    int queue_count;
    FakeOutput outputs[FAKE_MAX_WRITES];
    int noutputs;
    int deferred[FAKE_MAX_WRITES]; /* Indexes into outputs of queued writes not yet delivered */
    int ndeferred;
    int unexpected; /* Requests no Wii Remote answers this way, or a full log */
    int locks;      /* SDL_HIDAPI_LockRumble without the send that releases it */
} fake;

/* The driver's clock, which moves only when the driver waits or a test
   skips ahead, so every delay is exact. It starts far from 0, which the
   driver takes as "never". */
static Uint64 fake_now = 1000000;

static Uint64 Fake_GetTicks(void)
{
    return fake_now;
}

static void Fake_Delay(Uint32 ms)
{
    fake_now += ms;
}

static void Skip(Uint64 ms)
{
    fake_now += ms;
}

/* The driver's log lines, which SDL_LogDebug sends here */
#define LOG_LINES 512
static char log_lines[LOG_LINES][160];
static int nlog;

static void SDLCALL CaptureLog(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    (void)userdata;
    (void)priority;
    if (category == SDL_LOG_CATEGORY_INPUT && nlog < LOG_LINES) {
        SDL_strlcpy(log_lines[nlog++], message, sizeof(log_lines[0]));
    }
}

/* How many lines logged since from contain text */
static int CountLogged(int from, const char *text)
{
    int i, count = 0;

    for (i = from; i < nlog; ++i) {
        if (SDL_strstr(log_lines[i], text)) {
            ++count;
        }
    }
    return count;
}

static bool Logged(int from, const char *text)
{
    return CountLogged(from, text) > 0;
}

static SDL_hid_device *FakeHandle(void)
{
    return (SDL_hid_device *)(void *)&fake;
}

static void Fake_Unexpected(const char *what)
{
    ++fake.unexpected;
    printf("fake: unexpected %s\n", what);
}

static void Fake_Queue(const Uint8 *data, int length)
{
    int slot;

    if (fake.mute) {
        return;
    }
    if (data[0] == 0x21 && fake.lost_replies > 0) {
        --fake.lost_replies;
        return;
    }
    if (fake.queue_count == FAKE_MAX_REPORTS || length > FAKE_REPORT_SIZE) {
        Fake_Unexpected("report beyond the queue");
        return;
    }
    slot = (fake.queue_head + fake.queue_count) % FAKE_MAX_REPORTS;
    SDL_memcpy(fake.queue[slot], data, (size_t)length);
    fake.queue_length[slot] = length;
    ++fake.queue_count;
}

static bool Fake_MotionPlusActive(void)
{
    return fake.motion_plus && fake.motion_plus_mode != 0;
}

/* 20 BB BB LF 00 00 VV. A status report stops the data reports until the
   host selects one again (Linux hid-wiimote-core.c:1420-1421). Bit 3 of the
   flags is the camera, which report 0x13 alone sets (Dolphin
   EmuSubroutines.cpp:354). */
static void Fake_QueueStatus(void)
{
    Uint8 report[7];

    report[0] = 0x20;
    report[1] = fake.buttons[0];
    report[2] = fake.buttons[1];
    report[3] = (Uint8)(((fake.extension != FAKE_EXT_NONE || Fake_MotionPlusActive()) ? 0x02 : 0x00) |
                        ((fake.camera && !fake.camera_bit_clear) ? 0x08 : 0x00) | 0x10);
    report[4] = 0x00;
    report[5] = 0x00;
    report[6] = 0xC8;
    Fake_Queue(report, sizeof(report));
    fake.report_mode = 0;
}

/* 22 BB BB RR EE, the acknowledgment of output report RR (Dolphin
   WiimoteReport.h:224-231), unless the test loses it or holds it back */
static void Fake_QueueAckFor(Uint8 output, Uint8 error)
{
    Uint8 report[5];

    if (fake.lost_acks > 0) {
        --fake.lost_acks;
        return;
    }
    report[0] = 0x22;
    report[1] = fake.buttons[0];
    report[2] = fake.buttons[1];
    report[3] = output;
    report[4] = error;
    if (fake.hold_acks) {
        if (fake.nheld == FAKE_MAX_REPORTS) {
            Fake_Unexpected("held acknowledgment beyond the list");
            return;
        }
        SDL_memcpy(fake.held[fake.nheld++], report, sizeof(report));
        return;
    }
    Fake_Queue(report, sizeof(report));
}

// The acknowledgment of a register write, 22 BB BB 16 EE
static void Fake_QueueAck(Uint8 error)
{
    Fake_QueueAckFor(0x16, error);
}

/* The oldest held acknowledgment reaches the driver's reads, with the
   buttons the remote holds now */
static bool Fake_ReleaseAck(void)
{
    if (fake.nheld == 0) {
        return false;
    }
    fake.held[0][1] = fake.buttons[0];
    fake.held[0][2] = fake.buttons[1];
    Fake_Queue(fake.held[0], 5);
    SDL_memmove(fake.held[0], fake.held[1], (size_t)(fake.nheld - 1) * sizeof(fake.held[0]));
    --fake.nheld;
    return true;
}

/* 21 BB BB SE AA AA DD*16: S is the size less one, E the error, 7 for no
   device at the address */
static void Fake_QueueRead(Uint32 address, const Uint8 *bytes, int size, Uint8 error)
{
    Uint8 report[FAKE_REPORT_SIZE];

    SDL_zeroa(report);
    report[0] = 0x21;
    report[1] = fake.buttons[0];
    report[2] = fake.buttons[1];
    report[3] = (Uint8)(((size - 1) << 4) | error);
    report[4] = (Uint8)(address >> 8);
    report[5] = (Uint8)address;
    if (bytes && error == 0) {
        SDL_memcpy(&report[6], bytes, (size_t)size);
    }
    Fake_Queue(report, sizeof(report));
}

static void Fake_Read(const Uint8 *data, int length)
{
    Uint32 address;
    int size;

    if (length != SDL_WII_EXT_READ_REQUEST_SIZE || !(data[1] & 0x04)) {
        Fake_Unexpected("read request");
        return;
    }
    address = ((Uint32)data[2] << 16) | ((Uint32)data[3] << 8) | data[4];
    size = (data[5] << 8) | data[6];
    if (size < 1 || size > 16) {
        Fake_Unexpected("read size");
        return;
    }

    if (address == 0xA400FA && size == 6) {
        /* An active Motion Plus answers here with its mode in byte 4
           (hid-wiimote-core.c:550-554), else the extension does */
        if (Fake_MotionPlusActive()) {
            const Uint8 id[6] = { 0x00, 0x00, 0xA4, 0x20, fake.motion_plus_mode, 0x05 };

            Fake_QueueRead(address, id, size, 0);
        } else if (fake.extension != FAKE_EXT_NONE) {
            Fake_QueueRead(address, fake_extension_ids[fake.extension], size, 0);
        } else {
            Fake_QueueRead(address, NULL, size, 7);
        }
    } else if (address == 0xA600FA && size == 6) {
        /* An inactive Motion Plus (hid-wiimote-core.c:519-525) */
        if (fake.motion_plus && !Fake_MotionPlusActive()) {
            static const Uint8 id[6] = { 0x00, 0x00, 0xA6, 0x20, 0x00, 0x05 };

            Fake_QueueRead(address, id, size, 0);
        } else {
            Fake_QueueRead(address, NULL, size, 7);
        }
    } else if (address == 0xA400F6 && size == 4 && Fake_MotionPlusActive()) {
        /* The child's ID bytes 4, the challenge state, 0 and 5 (Dolphin
           MotionPlus.h) */
        const Uint8 *id = fake_extension_ids[fake.extension];
        const Uint8 stored[4] = { id[4], 0x00, id[0], id[5] };

        Fake_QueueRead(address, stored, size, 0);
    } else if ((address == 0xA40024 && size == 16) || (address == 0xA40034 && size == 8)) {
        Uint8 calibration[16];
        int i;

        for (i = 0; i < 16; ++i) {
            calibration[i] = (Uint8)(address + (Uint32)i);
        }
        if (fake.extension == FAKE_EXT_BALANCE_BOARD) {
            Fake_QueueRead(address, calibration, size, 0);
        } else {
            Fake_QueueRead(address, NULL, size, 7);
        }
    } else {
        Fake_Unexpected("read address");
        Fake_QueueRead(address, NULL, size, 8);
    }
}

static bool Fake_IsPassthroughMode(Uint8 mode)
{
    return mode == 0x04 || mode == 0x05 || mode == 0x07;
}

static void Fake_Write(const Uint8 *data, int length)
{
    Uint32 address;
    int size;
    Uint8 error = 0;
    bool pulse = false;

    if (length != SDL_WII_EXT_WRITE_REQUEST_SIZE || !(data[1] & 0x04)) {
        Fake_Unexpected("write request");
        return;
    }
    address = ((Uint32)data[2] << 16) | ((Uint32)data[3] << 8) | data[4];
    size = data[5];
    if (size < 1 || size > 16) {
        Fake_Unexpected("write size");
        return;
    }

    switch (address) {
    case 0xA400F0:
        /* 0x55 starts the extension, and deactivates an active Motion Plus,
           whose extension port then pulses a status report (Dolphin
           MotionPlus.cpp:262-268) */
        if (Fake_MotionPlusActive()) {
            fake.motion_plus_mode = 0;
            pulse = true;
        } else if (fake.extension == FAKE_EXT_NONE) {
            error = 7;
        }
        break;
    case 0xA400FB:
        if (!Fake_MotionPlusActive() && fake.extension == FAKE_EXT_NONE) {
            error = 7;
        }
        break;
    case 0xA600FE:
        /* Activates an inactive Motion Plus in a pass-through mode
           (hid-wiimote-core.c:493-510), unless the test refuses it */
        if (fake.motion_plus && !Fake_MotionPlusActive() && size == 1 && Fake_IsPassthroughMode(data[6]) &&
            fake.activation_nacks == 0) {
            fake.motion_plus_mode = data[6];
            pulse = true;
        } else {
            if (fake.activation_nacks > 0) {
                --fake.activation_nacks;
            }
            error = 7;
        }
        break;
    case 0xA400FE:
        /* An active Motion Plus changes mode */
        if (Fake_MotionPlusActive() && size == 1 && Fake_IsPassthroughMode(data[6])) {
            fake.motion_plus_mode = data[6];
            pulse = true;
        } else {
            error = 7;
        }
        break;
    case 0xB00000:
    case 0xB0001A:
    case 0xB00030:
    case 0xB00033:
        /* The camera answers on its bus only once report 0x13 has enabled
           it, and a write it does not take is error 7 (Dolphin
           EmuSubroutines.cpp:304-310, :348-356, Camera.cpp:41-49). The test
           can also have a camera that is on refuse writes. */
        if (!fake.camera) {
            error = 7;
        } else if (fake.camera_nacks > 0 && (fake.nack_address == 0 || fake.nack_address == address)) {
            --fake.camera_nacks;
            error = 7;
        } else if (address == 0xB00033) {
            fake.camera_mode = data[6];
        }
        break;
    default:
        Fake_Unexpected("write address");
        error = 8;
        break;
    }
    if (fake.camera_silent && (address & 0xFF0000) == 0xB00000) {
        return;
    }
    Fake_QueueAck(error);
    if (pulse) {
        Fake_QueueStatus();
    }
}

/* Every output report the remote receives, the way a Wii Remote takes it */
static void Fake_Deliver(const Uint8 *data, int length)
{
    switch (data[0]) {
    case 0x10: /* Rumble */
    case 0x11: /* LEDs */
        break;
    case 0x12:
        if (length != SDL_WII_EXT_MODE_REQUEST_SIZE) {
            Fake_Unexpected("data report request");
        }
        fake.report_mode = data[2];
        break;
    /* Bit 1 of byte 1 asks for an acknowledgment of the camera reports
       (Dolphin WiimoteReport.h:46-53, EmuSubroutines.cpp:340-360) */
    case 0x13:
        fake.camera = (data[1] & 0x04) != 0;
        if ((data[1] & 0x02) && !fake.camera_silent) {
            Fake_QueueAckFor(0x13, 0);
        }
        break;
    case 0x15:
        Fake_QueueStatus();
        break;
    case 0x16:
        Fake_Write(data, length);
        break;
    case 0x17:
        Fake_Read(data, length);
        break;
    case 0x1A:
        fake.camera_logic = (data[1] & 0x04) != 0;
        if ((data[1] & 0x02) && !fake.camera_silent) {
            Fake_QueueAckFor(0x1A, 0);
        }
        break;
    default:
        Fake_Unexpected("output report type");
        break;
    }
}

/* Every output report the driver sends, logged in the order it sent them. A
   remote that is off takes none. A queued write waits for the driver's next
   read while the test defers them, as when the rumble thread runs behind. */
static void Fake_Output(const Uint8 *data, int length, bool async)
{
    FakeOutput *output;

    if (fake.noutputs == FAKE_MAX_WRITES || length < 2 || length > FAKE_REPORT_SIZE) {
        Fake_Unexpected("output report");
        return;
    }
    output = &fake.outputs[fake.noutputs];
    output->async = async;
    output->refused = fake.off;
    output->ticks = fake_now;
    output->length = length;
    SDL_memcpy(output->data, data, (size_t)length);
    ++fake.noutputs;

    if (fake.off) {
        return;
    }
    if (async && fake.defer_queued) {
        fake.deferred[fake.ndeferred++] = fake.noutputs - 1;
        return;
    }
    Fake_Deliver(data, length);
}

static void Fake_DeliverDeferred(void)
{
    int i;

    for (i = 0; i < fake.ndeferred; ++i) {
        const FakeOutput *output = &fake.outputs[fake.deferred[i]];

        Fake_Deliver(output->data, output->length);
    }
    fake.ndeferred = 0;
}

static int Fake_hid_read_timeout(SDL_hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
    int size;

    (void)milliseconds;
    if (dev != FakeHandle()) {
        Fake_Unexpected("read handle");
        return -1;
    }
    /* The rumble thread caught up while the driver waited to read */
    Fake_DeliverDeferred();
    if (fake.read_errors > 0) {
        --fake.read_errors;
        return -1;
    }
    if (fake.queue_count == 0) {
        return 0;
    }
    size = fake.queue_length[fake.queue_head];
    if ((size_t)size > length) {
        size = (int)length;
    }
    SDL_memcpy(data, fake.queue[fake.queue_head], (size_t)size);
    fake.queue_head = (fake.queue_head + 1) % FAKE_MAX_REPORTS;
    --fake.queue_count;
    return size;
}

static int Fake_hid_write(SDL_hid_device *dev, const unsigned char *data, size_t length)
{
    if (dev != FakeHandle()) {
        Fake_Unexpected("write handle");
        return -1;
    }
    /* A remote that is off fails the write, as Windows does
       (Dolphin IOWin.cpp:428-438) */
    Fake_Output(data, (int)length, false);
    return fake.off ? -1 : (int)length;
}

static bool Fake_LockRumble(void)
{
    ++fake.locks;
    return true;
}

static int Fake_SendRumbleAndUnlock(SDL_HIDAPI_Device *device, const Uint8 *data, int size)
{
    if (fake.locks != 1 || !device || device->dev != FakeHandle()) {
        Fake_Unexpected("queued write");
    }
    fake.locks = 0;
    Fake_Output(data, size, true);
    return size;
}

/* The device and its joystick record */
static SDL_HIDAPI_Device device;
static SDL_Joystick *joystick;
static bool record_closed; /* The driver closed the record when its joystick went */
static SDL_JoystickID next_instance_id = 1000;
static int connections;
static int disconnections;

/* As HIDAPI_JoystickConnected, without SDL's joystick list */
static bool Fake_JoystickConnected(SDL_HIDAPI_Device *dev, SDL_JoystickID *pJoystickID)
{
    SDL_JoystickID *joysticks = (SDL_JoystickID *)SDL_realloc(dev->joysticks, (size_t)(dev->num_joysticks + 1) * sizeof(*joysticks));

    if (!joysticks) {
        return false;
    }
    dev->joysticks = joysticks;
    dev->joysticks[dev->num_joysticks++] = ++next_instance_id;
    if (pJoystickID) {
        *pJoystickID = next_instance_id;
    }
    ++connections;
    return true;
}

/* As HIDAPI_JoystickDisconnected, which closes the joystick first when the
   app has it open (SDL_hidapijoystick.c, HIDAPI_JoystickClose) */
static void Fake_JoystickDisconnected(SDL_HIDAPI_Device *dev, SDL_JoystickID joystickID)
{
    int i;

    for (i = 0; i < dev->num_joysticks; ++i) {
        if (dev->joysticks[i] == joystickID) {
            if (joystick && joystick->instance_id == joystickID && !record_closed) {
                dev->driver->CloseJoystick(dev, joystick);
                record_closed = true;
            }
            SDL_memmove(&dev->joysticks[i], &dev->joysticks[i + 1], (size_t)(dev->num_joysticks - i - 1) * sizeof(*dev->joysticks));
            --dev->num_joysticks;
            break;
        }
    }
    ++disconnections;
}

static SDL_Joystick *Fake_GetJoystickFromID(SDL_JoystickID instance_id)
{
    return (joystick && joystick->instance_id == instance_id) ? joystick : NULL;
}

/* SDL_SetJoystickSensorEnabled reaches the driver through this */
static bool TestSetSensorsEnabled(SDL_Joystick *record, bool enabled)
{
    bool result;

    if (record != joystick) {
        return SDL_SetError("Not the test's joystick");
    }
    result = SDL_HIDAPI_DriverWii.SetJoystickSensorsEnabled(&device, record, enabled);
    return result;
}

static SDL_JoystickDriver test_joystick_driver;
static int sensor_calls;

static bool CountingSetSensorsEnabled(SDL_Joystick *record, bool enabled)
{
    ++sensor_calls;
    return TestSetSensorsEnabled(record, enabled);
}

static void ResetFake(FakeExtension extension, bool motion_plus)
{
    SDL_zero(fake);
    fake.extension = extension;
    fake.motion_plus = motion_plus;
}

/* Whether the next StartDevice finds the remote powered off */
static bool start_off;

/* As the HIDAPI layer sets a device up: InitDevice under the joystick lock */
static bool StartDevice(FakeExtension extension, bool motion_plus, Uint16 product_id)
{
    bool result;

    ResetFake(extension, motion_plus);
    fake.off = start_off;
    nlog = 0;
    SDL_zero(device);
    device.dev = FakeHandle();
    device.name = SDL_strdup("Nintendo Wii Remote");
    device.path = SDL_strdup("fake");
    device.vendor_id = USB_VENDOR_NINTENDO;
    device.product_id = product_id;
    device.is_bluetooth = true;
    device.guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, USB_VENDOR_NINTENDO, product_id, 0, NULL, device.name, 'h', 0);
    device.driver = &SDL_HIDAPI_DriverWii;
    connections = 0;
    disconnections = 0;
    SDL_LockJoysticks();
    result = device.driver->InitDevice(&device);
    SDL_UnlockJoysticks();
    return result;
}

static void StopDevice(void)
{
    if (device.driver) {
        device.driver->FreeDevice(&device);
    }
    SDL_free(device.context);
    SDL_free(device.joysticks);
    SDL_free(device.name);
    SDL_free(device.path);
    SDL_zero(device);
}

/* As SDL_OpenJoystick: the driver's open, then the arrays, then the
   initial values SDL knows for a HIDAPI joystick */
static bool OpenRecord(void)
{
    bool opened;
    int i;

    if (device.num_joysticks < 1) {
        return false;
    }
    joystick = (SDL_Joystick *)SDL_calloc(1, sizeof(*joystick));
    if (!joystick) {
        return false;
    }
    test_joystick_driver.SetSensorsEnabled = CountingSetSensorsEnabled;
    joystick->driver = &test_joystick_driver;
    joystick->instance_id = device.joysticks[0];
    joystick->guid = device.guid;
    joystick->attached = true;
    joystick->ref_count = 1;
    SDL_SetObjectValid(joystick, SDL_OBJECT_TYPE_JOYSTICK, true);
    record_closed = false;

    SDL_LockJoysticks();
    opened = SDL_HIDAPI_DriverWii.OpenJoystick(&device, joystick);
    SDL_UnlockJoysticks();
    if (!opened) {
        return false;
    }
    if (joystick->naxes > 0) {
        joystick->axes = (SDL_JoystickAxisInfo *)SDL_calloc((size_t)joystick->naxes, sizeof(*joystick->axes));
    }
    if (joystick->nhats > 0) {
        joystick->hats = (Uint8 *)SDL_calloc((size_t)joystick->nhats, sizeof(*joystick->hats));
    }
    if (joystick->nbuttons > 0) {
        joystick->buttons = (bool *)SDL_calloc((size_t)joystick->nbuttons, sizeof(*joystick->buttons));
    }
    if ((joystick->naxes > 0 && !joystick->axes) || (joystick->nhats > 0 && !joystick->hats) ||
        (joystick->nbuttons > 0 && !joystick->buttons)) {
        return false;
    }
    for (i = 0; i < joystick->naxes && i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
        const int initial = (i == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || i == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) ? SDL_MIN_SINT16 : 0;

        joystick->axes[i].value = (Sint16)initial;
        joystick->axes[i].zero = (Sint16)initial;
        joystick->axes[i].initial_value = (Sint16)initial;
        joystick->axes[i].has_initial_value = true;
    }
    return true;
}

static void CloseRecord(void)
{
    if (!joystick) {
        return;
    }
    /* A joystick the driver disconnected was closed then, and SDL closes it
       only once */
    if (!record_closed) {
        SDL_LockJoysticks();
        SDL_HIDAPI_DriverWii.CloseJoystick(&device, joystick);
        SDL_UnlockJoysticks();
    }
    record_closed = false;
    SDL_SetObjectValid(joystick, SDL_OBJECT_TYPE_JOYSTICK, false);
    if (joystick->props) {
        SDL_DestroyProperties(joystick->props);
    }
    SDL_free(joystick->axes);
    SDL_free(joystick->hats);
    SDL_free(joystick->buttons);
    SDL_free(joystick->sensors);
    SDL_free(joystick);
    joystick = NULL;
}

static void Update(void)
{
    SDL_LockJoysticks();
    SDL_HIDAPI_DriverWii.UpdateDevice(&device);
    SDL_UnlockJoysticks();
}

/* Updates until the remote has nothing left to say, so a status report
   the driver asked for is handled and the data report it selects again is
   back */
static void Settle(void)
{
    int i;

    for (i = 0; i < 8; ++i) {
        Update();
        if (fake.queue_count == 0) {
            Update();
            if (fake.queue_count == 0) {
                return;
            }
        }
    }
    CHECK(false, "the driver and the remote did not settle");
}

static EWiiExtensionControllerType DriverType(void)
{
    return ((SDL_DriverWii_Context *)device.context)->m_eExtensionControllerType;
}

static int CountSensor(SDL_SensorType type)
{
    int i, count = 0;

    for (i = 0; joystick && i < joystick->nsensors; ++i) {
        if (joystick->sensors[i].type == type) {
            ++count;
        }
    }
    return count;
}

static const float *SensorData(SDL_SensorType type)
{
    int i;

    for (i = 0; joystick && i < joystick->nsensors; ++i) {
        if (joystick->sensors[i].type == type) {
            return joystick->sensors[i].data;
        }
    }
    return NULL;
}

static bool Near(float a, float b)
{
    return SDL_fabsf(a - b) < 0.001f;
}

static bool CameraWritten(int from)
{
    int i;

    for (i = from; i < fake.noutputs; ++i) {
        if (fake.outputs[i].data[0] == 0x13 || fake.outputs[i].data[0] == 0x1A ||
            (fake.outputs[i].data[0] == 0x16 && fake.outputs[i].data[2] == 0xB0)) {
            return true;
        }
    }
    return false;
}

/* The data report of the last 0x12 since from, or 0 */
static Uint8 LastModeRequest(int from)
{
    Uint8 mode = 0;
    int i;

    for (i = from; i < fake.noutputs; ++i) {
        if (fake.outputs[i].data[0] == 0x12) {
            mode = fake.outputs[i].data[2];
        }
    }
    return mode;
}

static SDL_DriverWii_Context *Ctx(void)
{
    return (SDL_DriverWii_Context *)device.context;
}

/* The index of the first output of a report since from, or -1 */
static int FindReport(int from, Uint8 report)
{
    int i;

    for (i = from; i < fake.noutputs; ++i) {
        if (fake.outputs[i].data[0] == report) {
            return i;
        }
    }
    return -1;
}

static int CountReport(int from, Uint8 report)
{
    int i, count = 0;

    for (i = from; i < fake.noutputs; ++i) {
        if (fake.outputs[i].data[0] == report) {
            ++count;
        }
    }
    return count;
}

/* The writes of one camera start, as WiimoteLib's EnableIR sends them
   (Wiimote.cs:1137-1183) with maximum sensitivity (:1013, :1175-1176). All
   go through the rumble thread, one at a time, and byte 1 of reports 0x13
   and 0x1a asks for an acknowledgment, as Dolphin's do
   (WiimoteController.cpp:950-962, hifihedgehog/SDL#40). */
typedef struct ExpectedOutput
{
    bool async;
    int length;
    Uint8 data[FAKE_REPORT_SIZE];
} ExpectedOutput;

static int CameraSequence(ExpectedOutput *out, Uint8 camera_mode)
{
    static const Uint8 sensitivity1[9] = { 0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x90, 0x00, 0x41 };
    static const Uint8 sensitivity2[2] = { 0x40, 0x00 };
    const struct
    {
        Uint32 address;
        const Uint8 *bytes;
        int size;
    } writes[5] = {
        { 0xB00030, (const Uint8 *)"\x08", 1 },
        { 0xB00000, sensitivity1, 9 },
        { 0xB0001A, sensitivity2, 2 },
        { 0xB00033, &camera_mode, 1 },
        { 0xB00030, (const Uint8 *)"\x08", 1 }
    };
    int n = 0, i;

    SDL_zerop(&out[n]);
    out[n].async = true;
    out[n].length = 2;
    out[n].data[0] = 0x13;
    out[n++].data[1] = 0x06;
    SDL_zerop(&out[n]);
    out[n].async = true;
    out[n].length = 2;
    out[n].data[0] = 0x1A;
    out[n++].data[1] = 0x06;
    for (i = 0; i < 5; ++i) {
        SDL_zerop(&out[n]);
        out[n].async = true;
        out[n].length = SDL_WII_EXT_WRITE_REQUEST_SIZE;
        out[n].data[0] = 0x16;
        out[n].data[1] = 0x04;
        out[n].data[2] = (Uint8)(writes[i].address >> 16);
        out[n].data[3] = (Uint8)(writes[i].address >> 8);
        out[n].data[4] = (Uint8)writes[i].address;
        out[n].data[5] = (Uint8)writes[i].size;
        SDL_memcpy(&out[n].data[6], writes[i].bytes, (size_t)writes[i].size);
        ++n;
    }
    return n;
}

static bool OutputIs(int index, const ExpectedOutput *expected)
{
    const FakeOutput *output = &fake.outputs[index];

    return index < fake.noutputs && output->async == expected->async && output->length == expected->length &&
           SDL_memcmp(output->data, expected->data, (size_t)expected->length) == 0;
}

static void CheckOutputs(int from, const ExpectedOutput *expected, int count, const char *what)
{
    int i;

    CHECK(fake.noutputs - from == count, "%s: %d writes, not %d", what, fake.noutputs - from, count);
    for (i = 0; i < count; ++i) {
        CHECK(OutputIs(from + i, &expected[i]), "%s: write %d is not %02x %02x %02x %02x %02x %02x %02x", what, i,
              expected[i].data[0], expected[i].data[1], expected[i].data[2], expected[i].data[3],
              expected[i].data[4], expected[i].data[5], expected[i].data[6]);
    }
}

/* The camera's and the Motion Plus's writes since from, reports 0x13, 0x1a
   and 0x16, in the order the driver sent them */
static void CheckCameraWrites(int from, const ExpectedOutput *expected, int count, const char *what)
{
    int i, n = 0;

    for (i = from; i < fake.noutputs; ++i) {
        const FakeOutput *output = &fake.outputs[i];

        if (output->data[0] != 0x13 && output->data[0] != 0x1A && output->data[0] != 0x16) {
            continue;
        }
        CHECK(n < count && OutputIs(i, &expected[n]), "%s: write %d is %s %02x %02x %02x %02x %02x %02x %02x", what, n,
              output->async ? "queued" : "direct", output->data[0], output->data[1], output->data[2], output->data[3],
              output->data[4], output->data[5], output->data[6]);
        ++n;
    }
    CHECK(n == count, "%s: %d writes, not %d", what, n, count);
}

/* The writes since from that turn the camera on or set its registers */
static int CountCameraWrites(int from)
{
    int i, count = 0;

    for (i = from; i < fake.noutputs; ++i) {
        const Uint8 *data = fake.outputs[i].data;

        if (((data[0] == 0x13 || data[0] == 0x1A) && (data[1] & 0x04)) || (data[0] == 0x16 && data[2] == 0xB0)) {
            ++count;
        }
    }
    return count;
}

/* The index of the first write to a register since from, or -1 */
static int FindRegisterWrite(int from, Uint32 address)
{
    int i;

    for (i = from; i < fake.noutputs; ++i) {
        const Uint8 *data = fake.outputs[i].data;

        if (data[0] == 0x16 && data[2] == (Uint8)(address >> 16) && data[3] == (Uint8)(address >> 8) && data[4] == (Uint8)address) {
            return i;
        }
    }
    return -1;
}

static ExpectedOutput Register(Uint32 address, Uint8 value)
{
    ExpectedOutput out;

    SDL_zero(out);
    out.async = true;
    out.length = SDL_WII_EXT_WRITE_REQUEST_SIZE;
    out.data[0] = 0x16;
    out.data[1] = 0x04;
    out.data[2] = (Uint8)(address >> 16);
    out.data[3] = (Uint8)(address >> 8);
    out.data[4] = (Uint8)address;
    out.data[5] = 1;
    out.data[6] = value;
    return out;
}

static ExpectedOutput Short(Uint8 report, Uint8 byte1, Uint8 byte2, int length)
{
    ExpectedOutput out;

    SDL_zero(out);
    out.async = true;
    out.length = length;
    out.data[0] = report;
    out.data[1] = byte1;
    out.data[2] = byte2;
    return out;
}

/* Reports the remote sends */

/* Basic IR, Dolphin Camera.h IRBasic: X1, Y1, then bits 8-9 of Y1, X1, Y2
   and X2 from the top, X2, Y2, and a second group with no dots */
static void BasicIR(Uint8 ir[10], int x1, int y1, int x2, int y2)
{
    ir[0] = (Uint8)x1;
    ir[1] = (Uint8)y1;
    ir[2] = (Uint8)((((y1 >> 8) & 3) << 6) | (((x1 >> 8) & 3) << 4) | (((y2 >> 8) & 3) << 2) | ((x2 >> 8) & 3));
    ir[3] = (Uint8)x2;
    ir[4] = (Uint8)y2;
    SDL_memset(&ir[5], 0xFF, 5);
}

/* Dolphin Classic.h's ButtonFormat, one bit per control */
#define CLASSIC_PAD_RIGHT    0x0080
#define CLASSIC_PAD_DOWN     0x0040
#define CLASSIC_TRIGGER_L    0x0020
#define CLASSIC_BUTTON_MINUS 0x0010
#define CLASSIC_BUTTON_HOME  0x0008
#define CLASSIC_BUTTON_PLUS  0x0004
#define CLASSIC_TRIGGER_R    0x0002
#define CLASSIC_BUTTON_ZL    0x8000
#define CLASSIC_BUTTON_B     0x4000
#define CLASSIC_BUTTON_Y     0x2000
#define CLASSIC_BUTTON_A     0x1000
#define CLASSIC_BUTTON_X     0x0800
#define CLASSIC_BUTTON_ZR    0x0400
#define CLASSIC_PAD_LEFT     0x0200
#define CLASSIC_PAD_UP       0x0100

/* Dolphin Classic.h's DataFormat: RX<4:3> LX<5:0>, RX<2:1> LY<5:0>,
   RX<0> LT<4:3> RY<4:0>, LT<2:0> RT<4:0>, then the buttons, 0 when
   pressed */
static void Classic(Uint8 ext[6], int lx, int ly, int rx, int ry, Uint16 pressed)
{
    const Uint16 buttons = (Uint16)~pressed;

    ext[0] = (Uint8)((((rx >> 3) & 3) << 6) | (lx & 0x3F));
    ext[1] = (Uint8)((((rx >> 1) & 3) << 6) | (ly & 0x3F));
    ext[2] = (Uint8)(((rx & 1) << 7) | (ry & 0x1F));
    ext[3] = 0;
    ext[4] = (Uint8)buttons;
    ext[5] = (Uint8)(buttons >> 8);
}

/* The same frame through a Motion Plus in Classic pass-through (Dolphin
   MotionPlus.cpp:666-678 and DataFormat): D-pad up and left move to bit 0
   of bytes 0 and 1, byte 4 bit 0 is the extension-connected flag, and
   byte 5 bits 0 and 1 are the zero bit and the clear M+ data flag */
static void ClassicPassthrough(Uint8 ext[6], int lx, int ly, int rx, int ry, Uint16 pressed)
{
    Classic(ext, lx, ly, rx, ry, pressed);
    ext[0] = (Uint8)((ext[0] & ~0x01) | (ext[5] & 0x01));
    ext[1] = (Uint8)((ext[1] & ~0x01) | ((ext[5] >> 1) & 0x01));
    ext[4] |= 0x01;
    ext[5] &= (Uint8)~0x03;
}

/* Dolphin MotionPlus.h DataFormat: yaw, roll and pitch bits 0-7, then each
   high 6 bits above two flags. Byte 4 bit 0 is the pass-through extension's
   connected flag, byte 5 bit 0 is always clear (MotionPlus.cpp:645-646) and
   byte 5 bit 1 marks M+ data. */
static void MotionPlusFrame(Uint8 ext[6], int yaw, int roll, int pitch, bool yaw_slow, bool roll_slow, bool pitch_slow, bool child)
{
    ext[0] = (Uint8)yaw;
    ext[1] = (Uint8)roll;
    ext[2] = (Uint8)pitch;
    ext[3] = (Uint8)((((yaw >> 8) & 0x3F) << 2) | (yaw_slow ? 0x02 : 0x00) | (pitch_slow ? 0x01 : 0x00));
    ext[4] = (Uint8)((((roll >> 8) & 0x3F) << 2) | (roll_slow ? 0x02 : 0x00) | (child ? 0x01 : 0x00));
    ext[5] = (Uint8)((((pitch >> 8) & 0x3F) << 2) | 0x02);
}

/* Dolphin Nunchuk.h DataFormat: the stick, the accelerometer's high bits,
   then Z and C in bits 0 and 1 of byte 5, each set while released
   (Nunchuk.h:44-45, :96-102) */
static void Nunchuk(Uint8 ext[6], int x, int y, bool c, bool z)
{
    ext[0] = (Uint8)x;
    ext[1] = (Uint8)y;
    ext[2] = 0x80;
    ext[3] = 0x80;
    ext[4] = 0x99;
    ext[5] = (Uint8)((c ? 0x00 : 0x02) | (z ? 0x00 : 0x01));
}

/* 37 BB BB AA AA AA II*10 EE*6, the remote at rest face up: X and Y at
   0x200, Z one g above it (100 counts per g, the driver's scale) */
static int Report37(Uint8 report[FAKE_REPORT_SIZE], const Uint8 ir[10], const Uint8 ext[6])
{
    report[0] = 0x37;
    report[1] = fake.buttons[0];
    report[2] = fake.buttons[1];
    report[3] = 0x80;
    report[4] = 0x80;
    report[5] = 0x99; /* 0x264 >> 2: 0x200 + 100 */
    SDL_memcpy(&report[6], ir, 10);
    SDL_memcpy(&report[16], ext, 6);
    return 22;
}

/* 32 BB BB EE*8 */
static int Report32(Uint8 report[FAKE_REPORT_SIZE], const Uint8 ext[6])
{
    report[0] = 0x32;
    report[1] = fake.buttons[0];
    report[2] = fake.buttons[1];
    SDL_memcpy(&report[3], ext, 6);
    report[9] = 0;
    report[10] = 0;
    return 11;
}

/* 30 BB BB, and 33 BB BB AA AA AA II*12 with no IR dot, the remote at rest
   face up */
static int Report30(Uint8 report[FAKE_REPORT_SIZE])
{
    report[0] = 0x30;
    report[1] = fake.buttons[0];
    report[2] = fake.buttons[1];
    return 3;
}

static int Report33(Uint8 report[FAKE_REPORT_SIZE])
{
    report[0] = 0x33;
    report[1] = fake.buttons[0];
    report[2] = fake.buttons[1];
    report[3] = 0x80;
    report[4] = 0x80;
    report[5] = 0x99;
    SDL_memset(&report[6], 0xFF, 12);
    return 18;
}

/* 35 BB BB AA AA AA EE*16, the six extension bytes and ten more 0xFF */
static int Report35(Uint8 report[FAKE_REPORT_SIZE], const Uint8 ext[6])
{
    report[0] = 0x35;
    report[1] = fake.buttons[0];
    report[2] = fake.buttons[1];
    report[3] = 0x80;
    report[4] = 0x80;
    report[5] = 0x99;
    SDL_memcpy(&report[6], ext, 6);
    SDL_memset(&report[12], 0xFF, 10);
    return 22;
}

/* The remote sends a data report in the mode it streams */
static void Feed(const Uint8 *report, int length)
{
    CHECK(fake.report_mode == report[0], "the remote streams report %02x, not %02x", fake.report_mode, report[0]);
    Fake_Queue(report, length);
    Update();
}

/* The data report the remote streams, if any, as a bare remote with the
   empty port sends it */
static void QueueStreamed(void)
{
    Uint8 report[FAKE_REPORT_SIZE], ext[6];

    SDL_memset(ext, 0xFF, sizeof(ext));
    if (fake.report_mode == 0x30) {
        Fake_Queue(report, Report30(report));
    } else if (fake.report_mode == 0x33) {
        Fake_Queue(report, Report33(report));
    } else if (fake.report_mode == 0x35) {
        Fake_Queue(report, Report35(report, ext));
    }
}

/* Updates every 50 ms for ms, each with the data report the remote streams,
   and returns how many updates waited on the clock */
static int RunFor(Uint32 ms)
{
    Uint32 elapsed;
    int waited = 0;

    for (elapsed = 0; elapsed < ms; elapsed += 50) {
        Uint64 before;

        Skip(50);
        QueueStreamed();
        before = Fake_GetTicks();
        Update();
        if (Fake_GetTicks() != before) {
            ++waited;
        }
    }
    return waited;
}

/* An acknowledgment no write is waiting for, as a late one for an earlier
   send of output report output */
static void QueueStrayAck(Uint8 output)
{
    Uint8 report[5];

    report[0] = 0x22;
    report[1] = fake.buttons[0];
    report[2] = fake.buttons[1];
    report[3] = output;
    report[4] = 0;
    Fake_Queue(report, sizeof(report));
}

/* The remote's A, Dolphin WiimoteEmu.h BUTTON_A 0x0800: bit 3 of the second
   button byte */
#define REMOTE_A_BYTE 1
#define REMOTE_A_BIT  0x08

/* 1 */
typedef struct Configuration
{
    const char *name;
    FakeExtension extension;
    bool motion_plus;
    Uint16 product_id;
    EWiiExtensionControllerType type;
    int accel;
    int accel_l;
    int gyro;
    int naxes;
    Uint8 sensors_off_report;
} Configuration;

static const Configuration configurations[] = {
    { "bare remote", FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE, k_eWiiExtensionControllerType_None, 1, 0, 0, 10, 0x30 },
    { "Wii Remote Plus", FAKE_EXT_NONE, true, USB_PRODUCT_NINTENDO_WII_REMOTE2, k_eWiiExtensionControllerType_None, 1, 0, 1, 10, 0x30 },
    { "Nunchuk", FAKE_EXT_NUNCHUK, false, USB_PRODUCT_NINTENDO_WII_REMOTE, k_eWiiExtensionControllerType_Nunchuk, 1, 1, 0, 10, 0x32 },
    { "Nunchuk and Motion Plus", FAKE_EXT_NUNCHUK, true, USB_PRODUCT_NINTENDO_WII_REMOTE2, k_eWiiExtensionControllerType_Nunchuk, 1, 1, 1, 10, 0x32 },
    { "Classic Controller", FAKE_EXT_CLASSIC, false, USB_PRODUCT_NINTENDO_WII_REMOTE, k_eWiiExtensionControllerType_Gamepad, 1, 0, 0, 10, 0x32 },
    { "Classic Controller Pro", FAKE_EXT_CLASSIC_PRO, false, USB_PRODUCT_NINTENDO_WII_REMOTE, k_eWiiExtensionControllerType_Gamepad, 1, 0, 0, 10, 0x32 },
    { "Classic Controller and Motion Plus", FAKE_EXT_CLASSIC, true, USB_PRODUCT_NINTENDO_WII_REMOTE2, k_eWiiExtensionControllerType_Gamepad, 1, 0, 1, 10, 0x32 },
    { "Wii U Pro Controller", FAKE_EXT_WIIUPRO, false, USB_PRODUCT_NINTENDO_WII_REMOTE2, k_eWiiExtensionControllerType_WiiUPro, 0, 0, 0, 6, 0x3D },
    { "Balance Board", FAKE_EXT_BALANCE_BOARD, false, USB_PRODUCT_NINTENDO_WII_REMOTE, k_eWiiExtensionControllerType_BalanceBoard, 0, 0, 0, 6, 0x34 },
    { "unrecognized extension", FAKE_EXT_UNRECOGNIZED, false, USB_PRODUCT_NINTENDO_WII_REMOTE, k_eWiiExtensionControllerType_Unknown, 0, 0, 0, 6, 0x30 }
};

static void TestRegistration(void)
{
    int c;

    for (c = 0; c < (int)SDL_arraysize(configurations); ++c) {
        const Configuration *config = &configurations[c];

        scenario = config->name;
        if (!StartDevice(config->extension, config->motion_plus, config->product_id) || !OpenRecord()) {
            CHECK(false, "the device did not start and open");
            CloseRecord();
            StopDevice();
            continue;
        }
        /* Positive control: the remote's answers identified the configuration */
        CHECK(DriverType() == config->type, "identified as extension type %d, not %d", DriverType(), config->type);
        CHECK(((SDL_DriverWii_Context *)device.context)->m_bMotionPlusPresent == config->motion_plus,
              "Motion Plus %s", config->motion_plus ? "missed" : "seen");
        CHECK(CountSensor(SDL_SENSOR_ACCEL) == config->accel, "%d accelerometers, not %d", CountSensor(SDL_SENSOR_ACCEL), config->accel);
        CHECK(CountSensor(SDL_SENSOR_ACCEL_L) == config->accel_l, "%d left accelerometers, not %d", CountSensor(SDL_SENSOR_ACCEL_L), config->accel_l);
        CHECK(CountSensor(SDL_SENSOR_GYRO) == config->gyro, "%d gyros, not %d", CountSensor(SDL_SENSOR_GYRO), config->gyro);
        CHECK(joystick->nsensors == config->accel + config->accel_l + config->gyro, "%d sensors", joystick->nsensors);
        CHECK(joystick->naxes == config->naxes, "%d axes, not %d", joystick->naxes, config->naxes);
        /* Nothing powers the camera or the Motion Plus until a sensor is
           enabled */
        Settle();
        CHECK(!CameraWritten(0) && !fake.camera && !fake.camera_logic, "the camera powered at open");
        CHECK(fake.motion_plus_mode == 0, "the Motion Plus runs at open");
        CHECK(fake.report_mode == config->sensors_off_report, "streams report %02x at open, not %02x",
              fake.report_mode, config->sensors_off_report);
        CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
        CloseRecord();
        StopDevice();
    }
}

/* 2, 3 and 4 without a Motion Plus */
static void TestClassicCamera(void)
{
    ExpectedOutput expected[16];
    Uint8 report[FAKE_REPORT_SIZE], ir[10], ext[6];
    int mark, n;
    const float *accel;

    scenario = "Classic Controller camera";
    if (!StartDevice(FAKE_EXT_CLASSIC, false, USB_PRODUCT_NINTENDO_WII_REMOTE) || !OpenRecord()) {
        CHECK(false, "the device did not start and open");
        CloseRecord();
        StopDevice();
        return;
    }
    Settle();

    /* A sensor the joystick lacks never reaches the driver */
    mark = fake.noutputs;
    sensor_calls = 0;
    CHECK(!SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_GYRO, true), "a gyro enabled without a Motion Plus");
    CHECK(sensor_calls == 0 && fake.noutputs == mark, "the gyro request reached the driver");

    /* 2: the camera, in Basic mode, then report 0x37 */
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable: %s", SDL_GetError());
    CHECK(sensor_calls == 1, "the driver was asked %d times", sensor_calls);
    Settle();
    n = CameraSequence(expected, 0x01);
    CheckCameraWrites(mark, expected, n, "Classic enable");
    CHECK(fake.camera && fake.camera_logic && fake.camera_mode == 0x01 && LastModeRequest(mark) == 0x37,
          "the remote's camera is %d %d mode %02x, report %02x selected", fake.camera, fake.camera_logic, fake.camera_mode, LastModeRequest(mark));
    CHECK(CountReport(mark, 0x15) == 1 && fake.report_mode == 0x37, "after %d status reports the remote streams %02x",
          CountReport(mark, 0x15), fake.report_mode);

    /* 3: centered, then moved */
    BasicIR(ir, 511, 383, 672, 384);
    Classic(ext, 32, 32, 16, 16, 0);
    Feed(report, Report37(report, ir, ext));
    accel = SensorData(SDL_SENSOR_ACCEL);
    CHECK(accel && Near(accel[0], 0.0f) && Near(accel[1], SDL_STANDARD_GRAVITY) && Near(accel[2], 0.0f),
          "the accelerometer reads %f %f %f, not 0 g 0", accel ? accel[0] : -1.0f, accel ? accel[1] : -1.0f, accel ? accel[2] : -1.0f);
    CHECK(joystick->axes[WII_IR_AXIS_DOT0_X].value == 511 && joystick->axes[WII_IR_AXIS_DOT0_Y].value == 383 &&
              joystick->axes[WII_IR_AXIS_DOT1_X].value == 672 && joystick->axes[WII_IR_AXIS_DOT1_Y].value == 384,
          "the IR axes read %d %d %d %d", joystick->axes[WII_IR_AXIS_DOT0_X].value, joystick->axes[WII_IR_AXIS_DOT0_Y].value,
          joystick->axes[WII_IR_AXIS_DOT1_X].value, joystick->axes[WII_IR_AXIS_DOT1_Y].value);
    CHECK(joystick->hats[0] == SDL_HAT_CENTERED && !joystick->buttons[SDL_GAMEPAD_BUTTON_EAST], "the centered Classic posts input");

    mark = fake.noutputs;
    fake.buttons[REMOTE_A_BYTE] = REMOTE_A_BIT;
    BasicIR(ir, 1000, 700, 300, 600);
    Classic(ext, 63, 32, 16, 16, CLASSIC_BUTTON_A | CLASSIC_BUTTON_ZR | CLASSIC_PAD_UP);
    Feed(report, Report37(report, ir, ext));
    CHECK(fake.noutputs == mark, "the driver asked for another data report");
    CHECK(joystick->axes[SDL_GAMEPAD_AXIS_LEFTX].value == SDL_JOYSTICK_AXIS_MAX, "LEFTX reads %d", joystick->axes[SDL_GAMEPAD_AXIS_LEFTX].value);
    CHECK(joystick->buttons[SDL_GAMEPAD_BUTTON_EAST] && !joystick->buttons[SDL_GAMEPAD_BUTTON_SOUTH], "A is not EAST alone");
    CHECK(joystick->axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER].value == SDL_JOYSTICK_AXIS_MAX &&
              joystick->axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER].value == SDL_JOYSTICK_AXIS_MIN,
          "the triggers read %d %d", joystick->axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER].value, joystick->axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER].value);
    CHECK(joystick->hats[0] == SDL_HAT_UP, "the hat reads %02x", joystick->hats[0]);
    CHECK(joystick->buttons[k_eWiiButtons_A], "the remote's A is not button %d", k_eWiiButtons_A);
    CHECK(joystick->axes[WII_IR_AXIS_DOT0_X].value == 1000 && joystick->axes[WII_IR_AXIS_DOT0_Y].value == 700 &&
              joystick->axes[WII_IR_AXIS_DOT1_X].value == 300 && joystick->axes[WII_IR_AXIS_DOT1_Y].value == 600,
          "the IR axes read %d %d %d %d after the move", joystick->axes[WII_IR_AXIS_DOT0_X].value, joystick->axes[WII_IR_AXIS_DOT0_Y].value,
          joystick->axes[WII_IR_AXIS_DOT1_X].value, joystick->axes[WII_IR_AXIS_DOT1_Y].value);

    /* 4: the camera off, report 0x32 */
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, false), "the accelerometer did not disable");
    expected[0] = Short(0x13, 0x00, 0, 2);
    expected[1] = Short(0x1A, 0x00, 0, 2);
    expected[2] = Short(0x12, 0x04, 0x32, 3);
    CheckOutputs(mark, expected, 3, "Classic disable");
    CHECK(!fake.camera && !fake.camera_logic && fake.report_mode == 0x32, "the camera stayed on, or the remote streams %02x", fake.report_mode);
    Settle();

    /* The Classic still reads, and the accelerometer is off */
    fake.buttons[REMOTE_A_BYTE] = 0;
    Classic(ext, 32, 32, 16, 16, CLASSIC_BUTTON_B);
    accel = SensorData(SDL_SENSOR_ACCEL);
    {
        const float before = accel ? accel[1] : -1.0f;

        Feed(report, Report32(report, ext));
        CHECK(joystick->buttons[SDL_GAMEPAD_BUTTON_SOUTH] && !joystick->buttons[SDL_GAMEPAD_BUTTON_EAST], "B is not SOUTH alone");
        CHECK(accel && accel[1] == before, "the accelerometer posted while off");
    }
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();
}

/* 2's comparison: the bare remote's enable, the reference sequence in
   Extended mode and report 0x33 */
static void TestBareRemoteCamera(void)
{
    ExpectedOutput expected[16];
    int mark, n;

    scenario = "bare remote camera";
    if (!StartDevice(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE) || !OpenRecord()) {
        CHECK(false, "the device did not start and open");
        CloseRecord();
        StopDevice();
        return;
    }
    Settle();
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    Settle();
    n = CameraSequence(expected, 0x03);
    CheckCameraWrites(mark, expected, n, "bare enable");
    CHECK(fake.camera_mode == 0x03 && LastModeRequest(mark) == 0x33 && fake.report_mode == 0x33, "camera mode %02x, report %02x",
          fake.camera_mode, fake.report_mode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();
}

/* 2, 3 and 4 with a Motion Plus, as in a Wii Remote Plus. PadForge enables
   the gyro before the accelerometer. */
static void TestClassicMotionPlus(void)
{
    ExpectedOutput expected[16];
    Uint8 report[FAKE_REPORT_SIZE], ir[10], ext[6];
    int mark, n;
    const float *gyro;

    scenario = "Classic Controller and Motion Plus";
    if (!StartDevice(FAKE_EXT_CLASSIC, true, USB_PRODUCT_NINTENDO_WII_REMOTE2) || !OpenRecord()) {
        CHECK(false, "the device did not start and open");
        CloseRecord();
        StopDevice();
        return;
    }
    Settle();

    /* 2: the Motion Plus in Classic pass-through, then the camera */
    mark = fake.noutputs;
    sensor_calls = 0;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_GYRO, true), "the gyro did not enable: %s", SDL_GetError());
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable: %s", SDL_GetError());
    CHECK(sensor_calls == 1, "the driver was asked %d times", sensor_calls);
    Settle();
    expected[0] = Register(0xA600FE, 0x07);
    n = 1 + CameraSequence(expected + 1, 0x01);
    CheckCameraWrites(mark, expected, n, "Classic and Motion Plus enable");
    CHECK(fake.motion_plus_mode == 0x07 && fake.camera_mode == 0x01 && LastModeRequest(mark) == 0x37,
          "Motion Plus mode %02x, camera mode %02x, report %02x selected", fake.motion_plus_mode, fake.camera_mode, LastModeRequest(mark));
    CHECK(fake.report_mode == 0x37, "after the status reports the remote streams %02x", fake.report_mode);

    /* 3: a gyro frame, then Classic frames, alternating as a Motion Plus
       sends them (Dolphin MotionPlus.cpp:571-573) */
    BasicIR(ir, 100, 200, 300, 400);
    MotionPlusFrame(ext, 8192 + 4096, 8192, 8192 - 4096, true, false, false, true);
    Feed(report, Report37(report, ir, ext));
    gyro = SensorData(SDL_SENSOR_GYRO);
    /* The driver's axes: -pitch, yaw, roll. Slow yaw: 4096 * 440 / 8192 =
       220 deg/s. Fast pitch: -4096 * 2000 / 8192 = -1000 deg/s. */
    CHECK(gyro && Near(gyro[0], 1000.0f * SDL_PI_F / 180.0f) && Near(gyro[1], 220.0f * SDL_PI_F / 180.0f) && Near(gyro[2], 0.0f),
          "the gyro reads %f %f %f", gyro ? gyro[0] : -1.0f, gyro ? gyro[1] : -1.0f, gyro ? gyro[2] : -1.0f);
    CHECK(joystick->axes[WII_IR_AXIS_DOT0_X].value == 100 && joystick->axes[WII_IR_AXIS_DOT1_Y].value == 400,
          "the IR axes read %d %d", joystick->axes[WII_IR_AXIS_DOT0_X].value, joystick->axes[WII_IR_AXIS_DOT1_Y].value);

    ClassicPassthrough(ext, 32, 32, 16, 16, 0);
    Feed(report, Report37(report, ir, ext));
    mark = fake.noutputs;
    ClassicPassthrough(ext, 63, 32, 16, 16, CLASSIC_BUTTON_X | CLASSIC_PAD_LEFT);
    Feed(report, Report37(report, ir, ext));
    CHECK(fake.noutputs == mark, "the driver asked for another data report");
    CHECK(DriverType() == k_eWiiExtensionControllerType_Gamepad && disconnections == 0, "the pass-through frames read as a removed Classic");
    CHECK(joystick->axes[SDL_GAMEPAD_AXIS_LEFTX].value == SDL_JOYSTICK_AXIS_MAX, "LEFTX reads %d", joystick->axes[SDL_GAMEPAD_AXIS_LEFTX].value);
    CHECK(joystick->buttons[SDL_GAMEPAD_BUTTON_NORTH], "X is not NORTH");
    CHECK(joystick->hats[0] == SDL_HAT_LEFT, "the hat reads %02x", joystick->hats[0]);

    /* 4: the Motion Plus off, then the camera, then report 0x32 */
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_GYRO, false) && SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, false),
          "the sensors did not disable");
    expected[0] = Register(0xA400F0, 0x55);
    expected[1] = Short(0x13, 0x00, 0, 2);
    expected[2] = Short(0x1A, 0x00, 0, 2);
    expected[3] = Short(0x12, 0x04, 0x32, 3);
    CheckOutputs(mark, expected, 4, "Classic and Motion Plus disable");
    CHECK(fake.motion_plus_mode == 0 && !fake.camera && fake.report_mode == 0x32,
          "Motion Plus mode %02x, camera %d, report %02x", fake.motion_plus_mode, fake.camera, fake.report_mode);
    Settle();
    Classic(ext, 32, 32, 16, 16, CLASSIC_BUTTON_Y);
    Feed(report, Report32(report, ext));
    CHECK(joystick->buttons[SDL_GAMEPAD_BUTTON_WEST], "Y is not WEST after the Motion Plus stops");
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();
}

/* No camera, no sensor: the request never reaches the driver */
static void TestNoCamera(void)
{
    static const FakeExtension extensions[] = { FAKE_EXT_WIIUPRO, FAKE_EXT_BALANCE_BOARD };
    int i;

    for (i = 0; i < (int)SDL_arraysize(extensions); ++i) {
        int mark;

        scenario = (extensions[i] == FAKE_EXT_WIIUPRO) ? "Wii U Pro Controller sensors" : "Balance Board sensors";
        if (!StartDevice(extensions[i], false, extensions[i] == FAKE_EXT_WIIUPRO ? USB_PRODUCT_NINTENDO_WII_REMOTE2 : USB_PRODUCT_NINTENDO_WII_REMOTE) ||
            !OpenRecord()) {
            CHECK(false, "the device did not start and open");
            CloseRecord();
            StopDevice();
            continue;
        }
        Settle();
        mark = fake.noutputs;
        sensor_calls = 0;
        CHECK(!SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "an accelerometer enabled");
        CHECK(sensor_calls == 0 && fake.noutputs == mark && !fake.camera, "the request reached the driver");
        CloseRecord();
        StopDevice();
    }
}

/* The remote's A in the bare and Nunchuk configurations, where the gamepad
   layer carries the remote's buttons */
static bool RemoteA(void)
{
    return joystick && joystick->nbuttons > SDL_GAMEPAD_BUTTON_EAST && joystick->buttons[SDL_GAMEPAD_BUTTON_EAST];
}

static bool StartAndOpen(FakeExtension extension, bool motion_plus, Uint16 product_id)
{
    if (!StartDevice(extension, motion_plus, product_id) || !OpenRecord()) {
        CHECK(false, "the device did not start and open");
        CloseRecord();
        StopDevice();
        return false;
    }
    Settle();
    return true;
}

/* 5 and 10: the camera's writes go one at a time, each once the remote
   acknowledged the one before it, while the remote's reports and the
   acknowledgments themselves post its buttons */
static void TestCameraOrder(void)
{
    ExpectedOutput expected[16];
    Uint8 report[FAKE_REPORT_SIZE], ext[6];
    Uint64 before;
    int mark, logmark, n, i;

    SDL_memset(ext, 0xFF, sizeof(ext));

    scenario = "camera order";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.hold_acks = true;
    mark = fake.noutputs;
    before = Fake_GetTicks();
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    n = CameraSequence(expected, 0x03);
    for (i = 0; i < n; ++i) {
        /* Positive control: write i went out, and none after it */
        CHECK(CountCameraWrites(mark) == i + 1, "step %d: %d camera writes", i, CountCameraWrites(mark));
        CHECK(!Ctx()->m_bIRActive && LastModeRequest(mark) == 0x35, "step %d: report %02x selected before the last acknowledgment",
              i, LastModeRequest(mark));
        /* A data report while the write waits */
        fake.buttons[REMOTE_A_BYTE] = REMOTE_A_BIT;
        Feed(report, Report35(report, ext));
        CHECK(RemoteA(), "step %d: the data report's A did not post while the write waited", i);
        CHECK(CountCameraWrites(mark) == i + 1, "step %d: a write went out before the acknowledgment", i);
        /* The acknowledgment, which carries the buttons too, moves the start
           one step */
        fake.buttons[REMOTE_A_BYTE] = 0;
        CHECK(Fake_ReleaseAck(), "step %d: no acknowledgment held", i);
        Update();
        CHECK(!RemoteA(), "step %d: the acknowledgment's buttons did not post", i);
    }
    CheckCameraWrites(mark, expected, n, "camera order");
    CHECK(Ctx()->m_bIRActive && LastModeRequest(mark) == 0x33 && fake.camera_mode == 0x03,
          "the start did not land: active %d, report %02x, mode %02x", Ctx()->m_bIRActive, LastModeRequest(mark), fake.camera_mode);
    CHECK(Fake_GetTicks() == before, "the start waited %d ms", (int)(Fake_GetTicks() - before));
    fake.hold_acks = false;
    Settle();
    CHECK(fake.report_mode == 0x33, "the remote streams %02x, not 0x33", fake.report_mode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    /* 5: queued writes reach the remote only at the driver's next read, as
       when the rumble thread runs behind, and the camera still takes every
       write after report 0x13 */
    scenario = "camera order with late queued writes";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.defer_queued = true;
    logmark = nlog;
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    /* Positive control: report 0x13 waits in the queue */
    CHECK(CountCameraWrites(mark) == 1 && fake.ndeferred > 0 && !fake.camera, "report 0x13 reached the remote at once");
    Settle();
    CHECK(Ctx()->m_bIRActive && fake.camera_mode == 0x03 && !Logged(logmark, "not written"),
          "the start failed while queued writes arrived late: active %d, mode %02x", Ctx()->m_bIRActive, fake.camera_mode);
    fake.defer_queued = false;
    Settle();
    CHECK(fake.report_mode == 0x33, "the remote streams %02x, not 0x33", fake.report_mode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();
}

/* 6: starts the camera refuses run again at growing delays until one holds,
   and a write without an acknowledgment goes again until four sends fail
   the start */
static void TestCameraRetry(void)
{
    static const Uint32 delays[] = { 1000, 2000, 4000, 8000, 8000 };
    Uint8 report[FAKE_REPORT_SIZE], ext[6];
    Uint64 before;
    int mark, logmark, i;

    /* The empty extension port */
    SDL_memset(ext, 0xFF, sizeof(ext));

    scenario = "camera retry";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    /* The camera refuses its mode register in five starts */
    fake.nack_address = 0xB00033;
    fake.camera_nacks = 5;
    logmark = nlog;
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    Settle();
    /* The refused write ends the start, so the last control write never
       goes */
    CHECK(CountReport(mark, 0x16) == 4 && !Ctx()->m_bIRActive, "a refused start: %d register writes, active %d",
          CountReport(mark, 0x16), Ctx()->m_bIRActive);
    CHECK(Logged(logmark, "IR camera register 0xb00033 not written: Write memory failed: 7"), "no log line names the register and the error");
    CHECK(LastModeRequest(mark) == 0x35, "report %02x selected after a failed start, not 0x35", LastModeRequest(mark));
    /* The remote streams report 0x35 throughout, so no input timeout comes
       between the starts */
    for (i = 0; i < (int)SDL_arraysize(delays); ++i) {
        const int starts = CountReport(0, 0x13);

        CHECK(Ctx()->m_ulNextIRStart == Fake_GetTicks() + delays[i], "start %d is due in %d ms, not %u", i + 2,
              (int)(Ctx()->m_ulNextIRStart - Fake_GetTicks()), (unsigned int)delays[i]);
        Skip(delays[i] - 1);
        Feed(report, Report35(report, ext));
        CHECK(CountReport(0, 0x13) == starts, "start %d ran early", i + 2);
        Skip(1);
        Feed(report, Report35(report, ext));
        CHECK(CountReport(0, 0x13) == starts + 1, "start %d did not run when due", i + 2);
        Settle();
    }
    /* The sixth start held, its status request found the camera on, and no
       start follows */
    CHECK(Ctx()->m_bIRActive && Ctx()->m_ulNextIRStart == 0 && fake.report_mode == 0x33,
          "the sixth start did not hold: active %d, report %02x", Ctx()->m_bIRActive, fake.report_mode);
    CHECK(CountReport(mark, 0x15) == 1 && Ctx()->m_nIRStartFailures == 0, "%d status requests, %d failures counted",
          CountReport(mark, 0x15), Ctx()->m_nIRStartFailures);
    mark = fake.noutputs;
    Skip(30000);
    Update();
    CHECK(CountReport(mark, 0x13) == 0, "a start ran after one held");
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    /* A lost acknowledgment: the write goes again after 250 ms, and the
       start goes on */
    scenario = "camera acknowledgment lost";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.lost_acks = 1;
    logmark = nlog;
    mark = fake.noutputs;
    before = Fake_GetTicks();
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    Update();
    CHECK(CountReport(mark, 0x13) == 1 && !Ctx()->m_bIRActive && Fake_GetTicks() == before,
          "positive control: report 0x13 sent %d times, active %d, %d ms waited", CountReport(mark, 0x13), Ctx()->m_bIRActive,
          (int)(Fake_GetTicks() - before));
    /* 250 ms, the figure docs/README-wii-extensions.md gives */
    Skip(249);
    Update();
    CHECK(CountReport(mark, 0x13) == 1, "report 0x13 went again before 250 ms");
    Skip(1);
    Update();
    CHECK(CountReport(mark, 0x13) == 2, "report 0x13 did not go again after 250 ms: %d sends", CountReport(mark, 0x13));
    Settle();
    CHECK(Ctx()->m_bIRActive && Ctx()->m_ulNextIRStart == 0 && !Logged(logmark, "not acknowledged"),
          "the resent write did not carry the start: active %d", Ctx()->m_bIRActive);
    CHECK(fake.report_mode == 0x33, "the remote streams %02x, not 0x33", fake.report_mode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    /* Four sends without an acknowledgment fail the start, which runs again
       on the schedule */
    scenario = "camera acknowledgments lost on every send";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.lost_acks = WII_WRITE_MAX_SENDS;
    logmark = nlog;
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    for (i = 1; i < WII_WRITE_MAX_SENDS; ++i) {
        Skip(WII_WRITE_PATIENCE_MS);
        Update();
    }
    CHECK(CountReport(mark, 0x13) == WII_WRITE_MAX_SENDS && Ctx()->m_ulNextIRStart == 0,
          "positive control: %d sends, or the start failed before the last one waited", CountReport(mark, 0x13));
    Skip(WII_WRITE_PATIENCE_MS);
    Update();
    CHECK(CountReport(mark, 0x13) == WII_WRITE_MAX_SENDS && !Ctx()->m_bIRActive, "a fifth send, or the start held");
    CHECK(Logged(logmark, "IR camera report 0x13 not acknowledged after 4 sends"), "no log line for the unacknowledged report");
    CHECK(Ctx()->m_ulNextIRStart == Fake_GetTicks() + 1000, "the next start is due in %d ms", (int)(Ctx()->m_ulNextIRStart - Fake_GetTicks()));
    Skip(1000);
    Update();
    Settle();
    CHECK(Ctx()->m_bIRActive && fake.report_mode == 0x33, "the second start did not hold: active %d, report %02x", Ctx()->m_bIRActive,
          fake.report_mode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    scenario = "camera retry after a sensor change";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.nack_address = 0xB00033;
    fake.camera_nacks = 3;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    Settle();
    Skip(1000);
    Feed(report, Report35(report, ext));
    Settle();
    CHECK(!Ctx()->m_bIRActive && Ctx()->m_ulNextIRStart == Fake_GetTicks() + 2000, "the second start is not due in 2 s");
    /* Turning the sensors off and on starts the delays over */
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, false) && SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true),
          "the sensors did not change");
    Settle();
    CHECK(!Ctx()->m_bIRActive && Ctx()->m_ulNextIRStart == Fake_GetTicks() + 1000, "after a sensor change the next start is due in %d ms, not 1000",
          (int)(Ctx()->m_ulNextIRStart - Fake_GetTicks()));
    Skip(1000);
    Feed(report, Report35(report, ext));
    Settle();
    CHECK(Ctx()->m_bIRActive, "the start after the sensor change did not hold");
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    scenario = "camera retry inside a Motion Plus settle window";
    if (!StartAndOpen(FAKE_EXT_NONE, true, USB_PRODUCT_NINTENDO_WII_REMOTE2)) {
        return;
    }
    fake.nack_address = 0xB00033;
    fake.camera_nacks = 1;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    CHECK(fake.motion_plus_mode == 0x04 && !Ctx()->m_bIRActive, "the Motion Plus did not start, or the camera did");
    Settle();
    CHECK(!Ctx()->m_bIRActive && Ctx()->m_ulNextIRStart, "positive control: the first start held");
    /* The start is due after 1 s, and the Motion Plus write settles for 2 */
    mark = fake.noutputs;
    MotionPlusFrame(ext, 8192, 8192, 8192, false, false, false, false);
    Skip(1000);
    Feed(report, Report35(report, ext));
    CHECK(CountReport(mark, 0x13) == 0, "a start ran inside the Motion Plus settle window");
    Skip(WII_MOTIONPLUS_SETTLE_TIME_MS - 1000);
    Feed(report, Report35(report, ext));
    CHECK(CountReport(mark, 0x13) == 1, "the start did not run once the window passed");
    Settle();
    CHECK(Ctx()->m_bIRActive, "the start once the window passed did not hold");
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    /* An identify in flight no longer holds a start back, and the start's
       acknowledgments leave the identify waiting for its reply */
    scenario = "camera retry with an identify in flight";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    SDL_memset(ext, 0xFF, sizeof(ext));
    fake.nack_address = 0xB00033;
    fake.camera_nacks = 1;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    Settle();
    /* The Motion Plus check goes out just before the start is due, and its
       reply is lost, so the identify stays in flight */
    Skip(999);
    Ctx()->m_ulNextMotionPlusCheck = Fake_GetTicks();
    fake.lost_replies = 1;
    Feed(report, Report35(report, ext));
    CHECK(Ctx()->m_eCommState != k_eWiiCommunicationState_None, "positive control: no identify in flight");
    mark = fake.noutputs;
    Skip(1);
    Feed(report, Report35(report, ext));
    CHECK(CountReport(mark, 0x13) == 1, "the start waited for the identify");
    Settle();
    CHECK(Ctx()->m_bIRActive && Ctx()->m_eCommState != k_eWiiCommunicationState_None,
          "the start beside the identify: active %d, state %d", Ctx()->m_bIRActive, (int)Ctx()->m_eCommState);
    /* The next check gets its reply */
    Skip(MOTION_PLUS_UPDATE_TIME_MS);
    Feed(report, Report33(report));
    Settle();
    CHECK(Ctx()->m_eCommState == k_eWiiCommunicationState_None && CountReport(mark, 0x13) == 1 && Ctx()->m_bIRActive,
          "after the identify: state %d, %d starts, active %d", (int)Ctx()->m_eCommState, CountReport(mark, 0x13), Ctx()->m_bIRActive);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();
}

/* 7: a status report that shows the camera off */
static void TestCameraStatus(void)
{
    int mark, logmark, index;

    scenario = "camera status bit";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    CHECK(!Ctx()->m_bIRActive && CountReport(mark, 0x15) == 0, "the start landed, or was checked, before the remote answered");
    /* The update that reads the last acknowledgment sends the check */
    Update();
    index = FindReport(mark, 0x15);
    CHECK(Ctx()->m_bIRActive && index >= 0 && fake.outputs[index].async, "no queued status request checked the start");
    Settle();
    CHECK(Ctx()->m_bIRActive && Ctx()->m_ulNextIRStart == 0 && fake.report_mode == 0x33, "the camera's status report stopped it");

    /* The camera turns off, as when the remote resets, and a status report
       says so */
    fake.camera = false;
    fake.camera_logic = false;
    logmark = nlog;
    mark = fake.noutputs;
    Fake_QueueStatus();
    Update();
    CHECK(!Ctx()->m_bIRActive && LastModeRequest(mark) == 0x35, "a camera found off kept report %02x", LastModeRequest(mark));
    CHECK(Logged(logmark, "Status shows the IR camera off"), "no log line for the camera found off");
    CHECK(Ctx()->m_ulNextIRStart == Fake_GetTicks() + 1000, "the next start is due in %d ms", (int)(Ctx()->m_ulNextIRStart - Fake_GetTicks()));
    Settle();
    Skip(1000);
    Update();
    Settle();
    CHECK(fake.camera && Ctx()->m_bIRActive && LastModeRequest(mark) == 0x33, "the camera did not start again");
    CHECK(fake.report_mode == 0x33, "the remote streams %02x, not 0x33", fake.report_mode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    scenario = "camera status bit with a Motion Plus";
    if (!StartAndOpen(FAKE_EXT_NONE, true, USB_PRODUCT_NINTENDO_WII_REMOTE2)) {
        return;
    }
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    Settle();
    CHECK(fake.motion_plus_mode == 0x04 && Ctx()->m_bIRActive, "the Motion Plus or the camera did not start");
    CHECK(CountReport(mark, 0x15) == 0, "the start was checked inside the Motion Plus settle window");
    /* A status report from before the camera start, as a Motion Plus pulse
       can be, arrives inside the window */
    fake.camera = false;
    Fake_QueueStatus();
    fake.camera = true;
    Update();
    CHECK(Ctx()->m_bIRActive, "a status report inside the settle window stopped the camera");
    Settle();
    Skip(WII_MOTIONPLUS_SETTLE_TIME_MS);
    Update();
    CHECK(CountReport(mark, 0x15) == 1, "%d status requests checked the start after the window, not 1", CountReport(mark, 0x15));
    Settle();
    CHECK(Ctx()->m_bIRActive && Ctx()->m_nIRStartFailures == 0 && fake.report_mode == 0x37,
          "the camera after the window: active %d, report %02x", Ctx()->m_bIRActive, fake.report_mode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();
}

/* 8: a remote with no input for 3 seconds */
static void TestSilentRemote(void)
{
    Uint8 report[FAKE_REPORT_SIZE];
    int mark, logmark, index;
    bool started;

    scenario = "quiet remote";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    mark = fake.noutputs;
    Skip(INPUT_WAIT_TIMEOUT_MS - 1);
    Update();
    CHECK(CountReport(mark, 0x15) == 0, "a status request went out before the timeout");
    Skip(1);
    Update();
    index = FindReport(mark, 0x15);
    CHECK(index >= 0 && !fake.outputs[index].async, "no status request on the caller's thread after 3 s without input");
    CHECK(disconnections == 0 && device.num_joysticks == 1 && !device.reads_ended, "a quiet remote lost its joystick");
    mark = fake.noutputs;
    Settle();
    CHECK(LastModeRequest(mark) == 0x30 && fake.report_mode == 0x30, "the status report did not restart the data reports");
    fake.buttons[REMOTE_A_BYTE] = REMOTE_A_BIT;
    Feed(report, Report30(report));
    CHECK(RemoteA(), "the remote's A did not post after the restart");
    fake.buttons[REMOTE_A_BYTE] = 0;
    Feed(report, Report30(report));

    /* A write still queued holds the request back */
    SDL_SetAtomicInt(&device.rumble_pending, 1);
    mark = fake.noutputs;
    Skip(INPUT_WAIT_TIMEOUT_MS);
    Update();
    CHECK(CountReport(mark, 0x15) == 0 && disconnections == 0, "the status request went out with a queued write pending");
    SDL_SetAtomicInt(&device.rumble_pending, 0);
    Update();
    CHECK(CountReport(mark, 0x15) == 1 && disconnections == 0, "no status request once the queued write finished");
    Settle();
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    scenario = "remote powered off";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    Settle();
    fake.off = true;
    logmark = nlog;
    mark = fake.noutputs;
    Skip(INPUT_WAIT_TIMEOUT_MS);
    Update();
    index = FindReport(mark, 0x15);
    CHECK(index >= 0 && fake.outputs[index].refused && !fake.outputs[index].async, "no status request went to the remote that is off");
    CHECK(disconnections == 1 && device.num_joysticks == 0, "the remote that is off: %d disconnections, %d joysticks",
          disconnections, device.num_joysticks);
    /* The device keeps its path and handle, so HIDAPI still counts it as present */
    CHECK(!device.reads_ended && device.dev == FakeHandle(), "the remote that is off released its device");
    CHECK(Logged(logmark, "No input for 3000 ms, and the status request failed: Couldn't write output report 0x15"),
          "no log line for the failed status request");
    /* Without a joystick the device asks again every 3 s */
    mark = fake.noutputs;
    Skip(INPUT_WAIT_TIMEOUT_MS - 1);
    Update();
    CHECK(CountReport(mark, 0x15) == 0, "the device asked again before 3 s");
    Skip(1);
    Update();
    index = FindReport(mark, 0x15);
    CHECK(index >= 0 && fake.outputs[index].refused && connections == 1, "the device did not ask again after 3 s");
    /* The remote comes back, reset by its power cycle. It takes the next
       request, and its status report brings a new joystick with its sensors
       off. */
    fake.off = false;
    fake.camera = false;
    fake.camera_logic = false;
    fake.camera_mode = 0;
    fake.report_mode = 0;
    logmark = nlog;
    Skip(INPUT_WAIT_TIMEOUT_MS);
    Update();
    Update();
    CHECK(connections == 2 && device.num_joysticks == 1 && !Ctx()->m_bReportSensors && !Ctx()->m_bIRActive,
          "the remote that came back: %d connections, %d joysticks, sensors %d, camera %d", connections, device.num_joysticks,
          Ctx()->m_bReportSensors, Ctx()->m_bIRActive);
    CHECK(Logged(logmark, "The Wii Remote at fake answers again"), "no log line for the remote that came back");
    CloseRecord();
    if (!OpenRecord()) {
        CHECK(false, "the new joystick did not open");
        CloseRecord();
        StopDevice();
        return;
    }
    Settle();
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable on the new joystick");
    Settle();
    CHECK(Ctx()->m_bIRActive && fake.camera && fake.report_mode == 0x33, "the new joystick's camera: active %d, report %02x",
          Ctx()->m_bIRActive, fake.report_mode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    scenario = "remote that takes writes and answers none";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.mute = true;
    mark = fake.noutputs;
    Skip(INPUT_WAIT_TIMEOUT_MS);
    Update();
    CHECK(CountReport(mark, 0x15) == 1 && disconnections == 0, "the first status request: %d sent, %d disconnections",
          CountReport(mark, 0x15), disconnections);
    Update();
    Update();
    CHECK(CountReport(mark, 0x15) == 1, "the status request went out again within the timeout: %d sent", CountReport(mark, 0x15));
    Skip(INPUT_WAIT_TIMEOUT_MS);
    Update();
    CHECK(CountReport(mark, 0x15) == 2 && disconnections == 0, "the next timeout: %d sent, %d disconnections",
          CountReport(mark, 0x15), disconnections);
    fake.mute = false;
    CloseRecord();
    StopDevice();

    scenario = "read error";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.read_errors = 1;
    Update();
    CHECK(disconnections == 1 && device.num_joysticks == 0 && device.reads_ended, "a read error kept the joystick, or left reads_ended clear");
    CloseRecord();
    StopDevice();

    /* A Wii Remote Plus, whose check waits out the Motion Plus settle
       window, so it is still due when the read fails */
    scenario = "read error with a camera check due";
    if (!StartAndOpen(FAKE_EXT_NONE, true, USB_PRODUCT_NINTENDO_WII_REMOTE2)) {
        return;
    }
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    Settle();
    CHECK(Ctx()->m_bIRActive && Ctx()->m_ulIRCheck, "positive control: no camera check due");
    Skip(Ctx()->m_ulIRCheck - Fake_GetTicks());
    mark = fake.noutputs;
    fake.read_errors = 1;
    Update();
    CHECK(CountReport(mark, 0x15) == 0 && disconnections == 1 && device.reads_ended, "the camera check went out on a read error");
    CloseRecord();
    StopDevice();

    scenario = "read error with a camera start due";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.nack_address = 0xB00033;
    fake.camera_nacks = 1;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    Settle();
    CHECK(!Ctx()->m_bIRActive && Ctx()->m_ulNextIRStart == Fake_GetTicks() + 1000, "positive control: the start held, or no start is due");
    Skip(1000);
    mark = fake.noutputs;
    fake.read_errors = 1;
    Update();
    CHECK(CountReport(mark, 0x13) == 0 && disconnections == 1 && device.reads_ended, "a camera start ran on a read error");
    CloseRecord();
    StopDevice();

    scenario = "no remote at InitDevice";
    start_off = true;
    started = StartDevice(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE);
    start_off = false;
    CHECK(started && connections == 0 && device.num_joysticks == 0 && !device.reads_ended,
          "a path with no remote: started %d, %d connections, reads_ended %d", started, connections, device.reads_ended);
    CHECK(fake.noutputs == 1 && fake.outputs[0].data[0] == 0x15 && fake.outputs[0].refused && !fake.outputs[0].async,
          "InitDevice sent %d writes, not one status request", fake.noutputs);
    CHECK(Logged(0, "No Wii Remote answers at fake: Couldn't write output report 0x15"), "no log line for the path with no remote");
    /* A remote that takes the request but sends nothing gets no joystick */
    fake.off = false;
    fake.mute = true;
    Skip(INPUT_WAIT_TIMEOUT_MS);
    Update();
    Update();
    CHECK(connections == 0 && CountReport(0, 0x15) == 2, "a remote that sends nothing: %d connections, %d requests",
          connections, CountReport(0, 0x15));
    /* Its first report brings the joystick */
    fake.mute = false;
    Skip(INPUT_WAIT_TIMEOUT_MS);
    Update();
    Update();
    CHECK(connections == 1 && device.num_joysticks == 1 && DriverType() == k_eWiiExtensionControllerType_None,
          "the remote's first report: %d connections, type %d", connections, DriverType());
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    StopDevice();

    scenario = "read error without a joystick";
    start_off = true;
    started = StartDevice(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE);
    start_off = false;
    CHECK(started && device.num_joysticks == 0 && !device.reads_ended, "positive control: the path with no remote");
    fake.read_errors = 1;
    Update();
    CHECK(device.reads_ended && connections == 0, "a read error without a joystick left reads_ended clear");
    StopDevice();
}

/* 9: the data report the driver selects and the one the remote streams,
   across a Motion Plus attach and detach with sensors on */
static void TestMotionPlusTrace(void)
{
    Uint8 report[FAKE_REPORT_SIZE], ir[10], ext[6];
    int mark, logmark;

    /* No IR dot */
    SDL_memset(ir, 0xFF, sizeof(ir));

    scenario = "Motion Plus attach";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE2)) {
        return;
    }
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    Settle();
    CHECK(fake.report_mode == 0x33 && LastModeRequest(0) == 0x33, "the bare remote streams %02x", fake.report_mode);
    fake.buttons[REMOTE_A_BYTE] = REMOTE_A_BIT;
    Feed(report, Report33(report));
    CHECK(RemoteA(), "the remote's A did not post before the attach");

    /* A Motion Plus with nothing behind it plugs in. Inactive, it passes the
       empty port's detect pin through, so no status report comes (Dolphin
       MotionPlus.cpp:375-377), and the remote keeps streaming 0x33. */
    fake.motion_plus = true;
    fake.buttons[REMOTE_A_BYTE] = 0;
    Feed(report, Report33(report));
    CHECK(!RemoteA(), "the remote's A stuck after the attach");
    /* The periodic check finds it, and the driver adds the joystick again */
    Skip(MOTION_PLUS_UPDATE_TIME_MS);
    Settle();
    CHECK(disconnections == 1 && connections == 2 && Ctx()->m_bMotionPlusPresent,
          "the Motion Plus was not found: %d disconnections, %d connections, present %d", disconnections, connections, Ctx()->m_bMotionPlusPresent);
    /* The app opens the new joystick only after 4 s. A remote that answers
       the status request keeps it meanwhile. */
    Skip(4000);
    Update();
    Settle();
    CHECK(disconnections == 1 && device.num_joysticks == 1, "the joystick added again was dropped before the app opened it");
    CloseRecord();
    if (!OpenRecord()) {
        CHECK(false, "the joystick added again did not open");
        CloseRecord();
        StopDevice();
        return;
    }
    Settle();
    CHECK(fake.motion_plus_mode == 0x04 && fake.report_mode == 0x37 && LastModeRequest(0) == 0x37 && CountSensor(SDL_SENSOR_GYRO) == 1,
          "after the reopen: Motion Plus mode %02x, report %02x, last request %02x, %d gyros", fake.motion_plus_mode, fake.report_mode,
          LastModeRequest(0), CountSensor(SDL_SENSOR_GYRO));
    fake.buttons[REMOTE_A_BYTE] = REMOTE_A_BIT;
    MotionPlusFrame(ext, 8192, 8192, 8192, false, false, false, false);
    Feed(report, Report37(report, ir, ext));
    CHECK(RemoteA() && disconnections == 1, "the remote's A did not post behind the Motion Plus");

    scenario = "Motion Plus detach";
    /* The active Motion Plus comes out: the detect pin drops, which stops the
       data reports and sends a status report (Dolphin WiimoteEmu.cpp:431-447) */
    fake.motion_plus = false;
    fake.motion_plus_mode = 0;
    mark = fake.noutputs;
    Fake_QueueStatus();
    Update();
    CHECK(LastModeRequest(mark) == 0x37 && fake.report_mode == 0x37, "the status report left report %02x", fake.report_mode);
    /* The empty port reads 0xFF in the extension bytes */
    fake.buttons[REMOTE_A_BYTE] = 0;
    SDL_memset(ext, 0xFF, sizeof(ext));
    Feed(report, Report37(report, ir, ext));
    CHECK(!RemoteA(), "the remote's A stuck after the detach");
    fake.buttons[REMOTE_A_BYTE] = REMOTE_A_BIT;
    Feed(report, Report37(report, ir, ext));
    CHECK(RemoteA(), "the remote's A did not post with the port empty");
    /* The check after the settle window finds the Motion Plus gone */
    Skip(WII_MOTIONPLUS_SETTLE_TIME_MS);
    Settle();
    CHECK(disconnections == 2 && connections == 3 && !Ctx()->m_bMotionPlusPresent,
          "the detach: %d disconnections, %d connections, present %d", disconnections, connections, Ctx()->m_bMotionPlusPresent);
    CloseRecord();
    if (!OpenRecord()) {
        CHECK(false, "the joystick added again did not open");
        CloseRecord();
        StopDevice();
        return;
    }
    Settle();
    CHECK(fake.report_mode == 0x33 && CountSensor(SDL_SENSOR_GYRO) == 0, "after the detach the remote streams %02x with %d gyros",
          fake.report_mode, CountSensor(SDL_SENSOR_GYRO));
    fake.buttons[REMOTE_A_BYTE] = 0;
    Feed(report, Report33(report));
    fake.buttons[REMOTE_A_BYTE] = REMOTE_A_BIT;
    Feed(report, Report33(report));
    CHECK(RemoteA(), "the remote's A did not post after the detach");
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    scenario = "Motion Plus that refuses its activation";
    if (!StartAndOpen(FAKE_EXT_NUNCHUK, true, USB_PRODUCT_NINTENDO_WII_REMOTE2)) {
        return;
    }
    fake.activation_nacks = 1;
    logmark = nlog;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    /* Positive control: the driver took the refused activation as running */
    CHECK(fake.motion_plus_mode == 0 && Ctx()->m_ucMotionPlusMode == 0x05, "the activation was not refused: remote %02x, driver %02x",
          fake.motion_plus_mode, Ctx()->m_ucMotionPlusMode);
    Settle();
    CHECK(Logged(logmark, "Motion Plus write refused: error 7"), "no log line for the refused activation");
    CHECK(fake.report_mode == 0x37, "the remote streams %02x, not 0x37", fake.report_mode);
    /* The inactive Motion Plus passes the Nunchuk's own frames through, Z
       and C released, which sets byte 5 bit 0 */
    fake.buttons[REMOTE_A_BYTE] = REMOTE_A_BIT;
    Nunchuk(ext, 128, 128, false, false);
    Feed(report, Report37(report, ir, ext));
    CHECK(RemoteA(), "the remote's A did not post while the driver took the Nunchuk's frames for Motion Plus frames");
    fake.buttons[REMOTE_A_BYTE] = 0;
    Feed(report, Report37(report, ir, ext));
    CHECK(!RemoteA(), "the remote's A stuck");
    /* After the settle window the camera start's status request goes out,
       its report arms the Motion Plus check, and the check activates the
       Motion Plus */
    Skip(WII_MOTIONPLUS_SETTLE_TIME_MS);
    Settle();
    CHECK(fake.motion_plus_mode == 0x05 && Ctx()->m_ucMotionPlusMode == 0x05,
          "the refused activation was not repaired: remote %02x, driver %02x", fake.motion_plus_mode, Ctx()->m_ucMotionPlusMode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();
}

/* 11: a camera that never acknowledges, and one that acknowledges every
   write and stays off. Every update returns at once, and the starts follow
   the schedule. */
static void TestCameraUnanswered(void)
{
    /* Each start sends report 0x13 four times, 250 ms apart, fails 250 ms
       after the last send, and the next start follows 1, 2 and 4 seconds
       later, then every 8 */
    static const Uint32 silent_starts[] = { 0, 2000, 5000, 10000, 19000, 28000 };
    /* Each start lands on the update after it, 50 ms later at this cadence,
       and its status report is read on the one after that, so the next
       start comes 100 ms plus the delay after it */
    static const Uint32 off_starts[] = { 0, 1100, 3200, 7300, 15400, 23500 };
    Uint64 t0;
    int mark, logmark, waited, i, n;

    scenario = "camera that never acknowledges";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.camera_silent = true;
    logmark = nlog;
    mark = fake.noutputs;
    t0 = Fake_GetTicks();
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    CHECK(Fake_GetTicks() == t0, "the sensor call waited %d ms", (int)(Fake_GetTicks() - t0));
    waited = RunFor(14000);
    /* The remote's buttons reach the joystick between the starts */
    fake.buttons[REMOTE_A_BYTE] = REMOTE_A_BIT;
    waited += RunFor(50);
    CHECK(RemoteA(), "the remote's A did not post while the camera stayed silent");
    fake.buttons[REMOTE_A_BYTE] = 0;
    waited += RunFor(29000 - 14050);
    CHECK(waited == 0, "%d updates waited", waited);
    n = 0;
    for (i = mark; i < fake.noutputs; ++i) {
        if (fake.outputs[i].data[0] == 0x13 && fake.outputs[i].data[1] == 0x06) {
            const int start = n / WII_WRITE_MAX_SENDS;
            const Uint64 at = fake.outputs[i].ticks - t0;

            CHECK(start < (int)SDL_arraysize(silent_starts) &&
                      at == silent_starts[start] + (Uint64)(n % WII_WRITE_MAX_SENDS) * WII_WRITE_PATIENCE_MS,
                  "send %d of report 0x13 at %d ms", n + 1, (int)at);
            ++n;
        }
    }
    CHECK(n == (int)SDL_arraysize(silent_starts) * WII_WRITE_MAX_SENDS, "%d sends of report 0x13", n);
    CHECK(CountCameraWrites(mark) == n, "a camera write followed an unacknowledged report 0x13");
    CHECK(CountLogged(logmark, "IR camera report 0x13 not acknowledged after 4 sends") == (int)SDL_arraysize(silent_starts),
          "%d log lines for unacknowledged starts", CountLogged(logmark, "IR camera report 0x13 not acknowledged after 4 sends"));
    CHECK(!Ctx()->m_bIRActive && LastModeRequest(mark) == 0x35, "the silent camera's start landed: report %02x", LastModeRequest(mark));
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    scenario = "camera that acknowledges and stays off";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.camera_bit_clear = true;
    logmark = nlog;
    mark = fake.noutputs;
    t0 = Fake_GetTicks();
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    CHECK(Fake_GetTicks() == t0, "the sensor call waited %d ms", (int)(Fake_GetTicks() - t0));
    waited = RunFor(24000);
    CHECK(waited == 0, "%d updates waited", waited);
    n = 0;
    for (i = mark; i < fake.noutputs; ++i) {
        if (fake.outputs[i].data[0] == 0x13 && fake.outputs[i].data[1] == 0x06) {
            const Uint64 at = fake.outputs[i].ticks - t0;

            CHECK(n < (int)SDL_arraysize(off_starts) && at == off_starts[n], "start %d at %d ms", n + 1, (int)at);
            ++n;
        }
    }
    CHECK(n == (int)SDL_arraysize(off_starts), "%d starts, not %d", n, (int)SDL_arraysize(off_starts));
    CHECK(CountCameraWrites(mark) == n * 7, "%d camera writes for %d starts", CountCameraWrites(mark), n);
    CHECK(CountLogged(logmark, "Status shows the IR camera off") == n, "%d log lines for the camera found off",
          CountLogged(logmark, "Status shows the IR camera off"));
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();
}

/* 10: an acknowledgment moves the start only when it names the report of
   the write that waits, and the IR report follows the last one */
static void TestAckMatching(void)
{
    static const struct
    {
        const char *name;
        FakeExtension extension;
        bool motion_plus;
        Uint16 product_id;
        int acknowledgments;
        Uint8 camera_mode;
    } configs[] = {
        { "Nunchuk camera order", FAKE_EXT_NUNCHUK, false, USB_PRODUCT_NINTENDO_WII_REMOTE, 7, 0x01 },
        { "Wii Remote Plus camera order", FAKE_EXT_NONE, true, USB_PRODUCT_NINTENDO_WII_REMOTE2, 8, 0x01 }
    };
    int mark, released, c;

    scenario = "acknowledgment of another report";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.hold_acks = true;
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    /* Report 0x13 waits, and acknowledgments of a register write and of
       report 0x12 come */
    QueueStrayAck(0x16);
    QueueStrayAck(0x12);
    Update();
    CHECK(CountCameraWrites(mark) == 1 && Ctx()->m_eWriteStep == k_eWiiWriteStep_CameraEnable,
          "acknowledgments of other reports moved the start: %d writes", CountCameraWrites(mark));
    /* Positive control: the remote's own acknowledgment moves it */
    CHECK(Fake_ReleaseAck(), "report 0x13's acknowledgment was not held");
    Update();
    CHECK(CountCameraWrites(mark) == 2 && Ctx()->m_eWriteStep == k_eWiiWriteStep_CameraEnable2,
          "report 0x13's acknowledgment did not move the start: %d writes", CountCameraWrites(mark));
    /* Report 0x1a waits, and a late acknowledgment of 0x13 comes */
    QueueStrayAck(0x13);
    Update();
    CHECK(CountCameraWrites(mark) == 2, "a late acknowledgment of report 0x13 moved the start");
    CHECK(Fake_ReleaseAck(), "report 0x1a's acknowledgment was not held");
    Update();
    CHECK(CountCameraWrites(mark) == 3 && Ctx()->m_eWriteStep == k_eWiiWriteStep_CameraControl,
          "report 0x1a's acknowledgment did not move the start: %d writes", CountCameraWrites(mark));
    /* A register write waits, and acknowledgments of 0x13 and 0x1a come */
    QueueStrayAck(0x13);
    QueueStrayAck(0x1A);
    Update();
    CHECK(CountCameraWrites(mark) == 3, "acknowledgments of the enable reports moved a register write");
    while (!Ctx()->m_bIRActive && Fake_ReleaseAck()) {
        Update();
    }
    CHECK(Ctx()->m_bIRActive && CountCameraWrites(mark) == 7 && LastModeRequest(mark) == 0x33,
          "the start did not land: %d writes, report %02x", CountCameraWrites(mark), LastModeRequest(mark));
    fake.hold_acks = false;
    Settle();
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    /* With an extension or a Motion Plus the last acknowledgment selects
       report 0x37, and the start's Motion Plus write is acknowledged first */
    for (c = 0; c < (int)SDL_arraysize(configs); ++c) {
        scenario = configs[c].name;
        if (!StartAndOpen(configs[c].extension, configs[c].motion_plus, configs[c].product_id)) {
            continue;
        }
        fake.hold_acks = true;
        mark = fake.noutputs;
        CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
        released = 0;
        while (!Ctx()->m_bIRActive && released < 16) {
            CHECK(LastModeRequest(mark) == 0x35, "report %02x selected before acknowledgment %d", LastModeRequest(mark), released + 1);
            if (!Fake_ReleaseAck()) {
                CHECK(false, "no acknowledgment held after %d", released);
                break;
            }
            ++released;
            Update();
        }
        CHECK(Ctx()->m_bIRActive && released == configs[c].acknowledgments && LastModeRequest(mark) == 0x37 &&
                  fake.camera_mode == configs[c].camera_mode,
              "after %d acknowledgments: active %d, report %02x, camera mode %02x", released, Ctx()->m_bIRActive,
              LastModeRequest(mark), fake.camera_mode);
        fake.hold_acks = false;
        Settle();
        CHECK(fake.report_mode == 0x37 && disconnections == 0, "the remote streams %02x, %d disconnections", fake.report_mode,
              disconnections);
        CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
        CloseRecord();
        StopDevice();
    }
}

/* 12: no sensor call waits on the remote. PadForge turns the sensors off
   and on again to restart the camera. */
static void TestSensorCallsWaitOnNothing(void)
{
    static const struct
    {
        const char *name;
        FakeExtension extension;
        Uint8 mode;
    } configs[] = {
        { "Wii Remote Plus sensor calls", FAKE_EXT_NONE, 0x04 },
        { "Nunchuk and Motion Plus sensor calls", FAKE_EXT_NUNCHUK, 0x05 }
    };
    Uint64 before;
    int mark, c, i;

    for (c = 0; c < (int)SDL_arraysize(configs); ++c) {
        scenario = configs[c].name;
        if (!StartAndOpen(configs[c].extension, true, USB_PRODUCT_NINTENDO_WII_REMOTE2)) {
            continue;
        }
        /* No acknowledgment comes until the calls are done */
        fake.hold_acks = true;
        mark = fake.noutputs;
        before = Fake_GetTicks();
        CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true) &&
                  SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, false) &&
                  SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true),
              "the sensors did not change");
        CHECK(Fake_GetTicks() == before, "the sensor calls waited %d ms", (int)(Fake_GetTicks() - before));
        /* The deactivation went out with the call that turned the sensors
           off, and the camera waits for the Motion Plus writes */
        CHECK(FindRegisterWrite(mark, 0xA400F0) >= 0 && CountCameraWrites(mark) == 0,
              "the deactivation waited, or a camera write went beside a Motion Plus write");
        for (i = 0; i < 3; ++i) {
            Update();
        }
        CHECK(Fake_GetTicks() == before && disconnections == 0, "updates waited %d ms, %d disconnections", (int)(Fake_GetTicks() - before),
              disconnections);
        fake.hold_acks = false;
        while (Fake_ReleaseAck()) {
        }
        Settle();
        CHECK(fake.motion_plus_mode == configs[c].mode && Ctx()->m_ucMotionPlusMode == configs[c].mode,
              "the Motion Plus runs %02x, the driver has %02x, not %02x", fake.motion_plus_mode, Ctx()->m_ucMotionPlusMode, configs[c].mode);
        CHECK(fake.camera && Ctx()->m_bIRActive && fake.report_mode == 0x37 && disconnections == 0,
              "after the calls: camera %d, active %d, report %02x, %d disconnections", fake.camera, Ctx()->m_bIRActive,
              fake.report_mode, disconnections);
        CHECK(Fake_GetTicks() == before, "the start waited %d ms", (int)(Fake_GetTicks() - before));
        CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
        CloseRecord();
        StopDevice();
    }

    /* The Motion Plus pulses a status report when it activates. One read
       after the deactivation went out still counts the Motion Plus as an
       extension. */
    scenario = "status report from before a Motion Plus deactivation";
    if (!StartAndOpen(FAKE_EXT_NONE, true, USB_PRODUCT_NINTENDO_WII_REMOTE2)) {
        return;
    }
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true) && SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, false),
          "the sensors did not change");
    /* Positive control: both writes went out before the driver read the
       activation's status report */
    CHECK(FindRegisterWrite(mark, 0xA600FE) >= 0 && FindRegisterWrite(mark, 0xA400F0) >= 0 && fake.queue_count == 4,
          "the writes went out %d %d, %d reports queued", FindRegisterWrite(mark, 0xA600FE), FindRegisterWrite(mark, 0xA400F0),
          fake.queue_count);
    Settle();
    CHECK(disconnections == 0 && connections == 1 && device.num_joysticks == 1,
          "a status report from before the deactivation added the joystick again: %d disconnections", disconnections);
    CHECK(fake.motion_plus_mode == 0 && Ctx()->m_ucMotionPlusMode == 0 && fake.report_mode == 0x30,
          "after the deactivation: Motion Plus %02x, driver %02x, report %02x", fake.motion_plus_mode, Ctx()->m_ucMotionPlusMode,
          fake.report_mode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();
}

/* 12: a sensor change or a close stops a start under way, and a write
   still queued on the rumble thread holds its patience */
static void TestStartStops(void)
{
    int mark, logmark;

    scenario = "camera start stopped by a sensor change";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.camera_silent = true;
    logmark = nlog;
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    Skip(WII_WRITE_PATIENCE_MS);
    Update();
    CHECK(CountCameraWrites(mark) == 2, "positive control: report 0x13 sent %d times", CountCameraWrites(mark));
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, false), "the accelerometer did not disable");
    mark = fake.noutputs;
    Skip(1000);
    Update();
    Skip(1000);
    Update();
    CHECK(CountCameraWrites(mark) == 0 && Ctx()->m_eWriteStep == k_eWiiWriteStep_None && Ctx()->m_ulNextIRStart == 0 &&
              !Logged(logmark, "not acknowledged"),
          "the stopped start went on: %d writes, step %d", CountCameraWrites(mark), (int)Ctx()->m_eWriteStep);
    /* On again, with a camera that answers */
    fake.camera_silent = false;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable again");
    Settle();
    CHECK(Ctx()->m_bIRActive && fake.report_mode == 0x33, "the start after the stop: active %d, report %02x", Ctx()->m_bIRActive,
          fake.report_mode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    scenario = "camera start across a close";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.hold_acks = true;
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    CHECK(Ctx()->m_eWriteStep == k_eWiiWriteStep_CameraEnable, "positive control: no write waits");
    CloseRecord();
    CHECK(Ctx()->m_eWriteStep == k_eWiiWriteStep_None && !Ctx()->m_bIRStartPending, "the closed joystick kept its writes");
    /* The acknowledgment arrives while the joystick is closed */
    fake.hold_acks = false;
    CHECK(Fake_ReleaseAck(), "no acknowledgment held");
    Update();
    CHECK(fake.queue_count == 0 && CountCameraWrites(mark) == 1, "the closed joystick's acknowledgment moved a start");
    /* The open with sensors still on starts the camera again */
    if (!OpenRecord()) {
        CHECK(false, "the joystick did not open again");
        CloseRecord();
        StopDevice();
        return;
    }
    Settle();
    CHECK(Ctx()->m_bIRActive && CountCameraWrites(mark) == 8 && fake.report_mode == 0x33,
          "the start after the open: active %d, %d writes, report %02x", Ctx()->m_bIRActive, CountCameraWrites(mark), fake.report_mode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();

    scenario = "camera write still queued";
    if (!StartAndOpen(FAKE_EXT_NONE, false, USB_PRODUCT_NINTENDO_WII_REMOTE)) {
        return;
    }
    fake.camera_silent = true;
    mark = fake.noutputs;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    SDL_SetAtomicInt(&device.rumble_pending, 1);
    Skip(1000);
    Update();
    CHECK(CountReport(mark, 0x13) == 1 && Ctx()->m_ulNextIRStart == 0, "the patience ran while the write was queued: %d sends",
          CountReport(mark, 0x13));
    SDL_SetAtomicInt(&device.rumble_pending, 0);
    Update();
    Skip(WII_WRITE_PATIENCE_MS - 1);
    Update();
    CHECK(CountReport(mark, 0x13) == 1, "report 0x13 went again before 250 ms after the queue drained");
    Skip(1);
    Update();
    CHECK(CountReport(mark, 0x13) == 2, "report 0x13 did not go again 250 ms after the queue drained: %d sends", CountReport(mark, 0x13));
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();
}

/* 13: a Motion Plus activation that has to wait for a camera write goes
   once the start lands, its check is armed meanwhile, and the camera's
   check waits out the activation's settle window */
static void TestMotionPlusBehindCamera(void)
{
    Uint64 deadline;
    int mark, last, i;

    scenario = "Motion Plus found inactive during a camera start";
    if (!StartAndOpen(FAKE_EXT_NONE, true, USB_PRODUCT_NINTENDO_WII_REMOTE2)) {
        return;
    }
    /* The first start fails, so the next comes once the activation's settle
       window passes, with the Motion Plus check */
    fake.nack_address = 0xB00033;
    fake.camera_nacks = 1;
    CHECK(SDL_SetJoystickSensorEnabled(joystick, SDL_SENSOR_ACCEL, true), "the accelerometer did not enable");
    Settle();
    CHECK(fake.motion_plus_mode == 0x04 && !Ctx()->m_bIRActive && Ctx()->m_ulNextIRStart, "positive control: the Motion Plus or the start");
    /* The Motion Plus goes inactive, as when its activation is lost to a
       contact bounce */
    Skip(WII_MOTIONPLUS_SETTLE_TIME_MS);
    fake.motion_plus_mode = 0;
    fake.hold_acks = true;
    mark = fake.noutputs;
    for (i = 0; i < 4 && Ctx()->m_eMotionPlusWrite != k_eWiiMotionPlusWrite_Activate; ++i) {
        Update();
    }
    CHECK(Ctx()->m_eWriteStep == k_eWiiWriteStep_CameraEnable && Ctx()->m_eMotionPlusWrite == k_eWiiMotionPlusWrite_Activate &&
              FindRegisterWrite(mark, 0xA600FE) < 0 && fake.motion_plus_mode == 0,
          "the activation did not wait for report 0x13: step %d, waiting %d", (int)Ctx()->m_eWriteStep, (int)Ctx()->m_eMotionPlusWrite);
    CHECK(Ctx()->m_ulNextMotionPlusCheck == Fake_GetTicks() + 500, "the activation's check is due in %d ms, not 500",
          (int)(Ctx()->m_ulNextMotionPlusCheck - Fake_GetTicks()));
    while (!Ctx()->m_bIRActive && Fake_ReleaseAck()) {
        Update();
    }
    last = -1;
    for (i = mark; i < fake.noutputs; ++i) {
        if (fake.outputs[i].data[0] == 0x16 && fake.outputs[i].data[2] == 0xB0) {
            last = i;
        }
    }
    CHECK(Ctx()->m_bIRActive && last >= 0 && FindRegisterWrite(mark, 0xA600FE) > last,
          "the activation did not follow the start's last write: active %d", Ctx()->m_bIRActive);
    CHECK(Ctx()->m_ulIRCheck == Ctx()->m_ulMotionPlusSettleDeadline && Ctx()->m_ulIRCheck == Fake_GetTicks() + WII_MOTIONPLUS_SETTLE_TIME_MS,
          "the camera's check is due in %d ms, not after the activation's window", (int)(Ctx()->m_ulIRCheck - Fake_GetTicks()));
    deadline = Ctx()->m_ulIRCheck;
    fake.hold_acks = false;
    while (Fake_ReleaseAck()) {
    }
    Settle();
    CHECK(fake.motion_plus_mode == 0x04, "the Motion Plus runs %02x after the activation", fake.motion_plus_mode);
    /* After the window the camera's check finds the camera on, and the
       Motion Plus check, which the activation's status report moved to the
       same deadline, finds the Motion Plus running */
    mark = fake.noutputs;
    Skip(deadline - Fake_GetTicks());
    Update();
    CHECK(CountReport(mark, 0x15) == 1 && CountReport(mark, 0x17) == 1, "%d status requests and %d reads after the window",
          CountReport(mark, 0x15), CountReport(mark, 0x17));
    Settle();
    CHECK(Ctx()->m_bIRActive && Ctx()->m_nIRStartFailures == 0 && fake.report_mode == 0x37 && disconnections == 0,
          "the camera after the window: active %d, failures %d, report %02x, %d disconnections", Ctx()->m_bIRActive,
          Ctx()->m_nIRStartFailures, fake.report_mode, disconnections);
    CHECK(Ctx()->m_eCommState == k_eWiiCommunicationState_None && Ctx()->m_ucMotionPlusMode == 0x04 && Ctx()->m_bMotionPlusPresent,
          "after the Motion Plus check: state %d, mode %02x", (int)Ctx()->m_eCommState, Ctx()->m_ucMotionPlusMode);
    CHECK(fake.unexpected == 0, "%d requests the remote does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    /* The driver's debug lines, which name each failed camera write */
    SDL_SetLogPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_DEBUG);
    SDL_SetLogOutputFunction(CaptureLog, NULL);

    TestRegistration();
    TestClassicCamera();
    TestBareRemoteCamera();
    TestClassicMotionPlus();
    TestNoCamera();
    TestCameraOrder();
    TestCameraRetry();
    TestCameraStatus();
    TestSilentRemote();
    TestMotionPlusTrace();
    TestCameraUnanswered();
    TestAckMatching();
    TestSensorCallsWaitOnNothing();
    TestStartStops();
    TestMotionPlusBehindCamera();

    SDL_Quit();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
