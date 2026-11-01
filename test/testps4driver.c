/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_HIDAPI_DriverPS4, the HIDAPI PS4 driver, against a scripted
   DualShock 4 on Bluetooth, for hifihedgehog/SDL#41. No HID device opens and
   the rumble thread never starts: the driver's reads, feature reads,
   rumble-thread writes and joystick connections go to the fake below. The
   test calls the driver's functions on a hand-built device and joystick
   record, under SDL's joystick lock, as the HIDAPI layer and
   SDL_OpenJoystick do.

   The scripted controller sends Bluetooth state reports 0x11 to 0x19 at the
   lengths its report descriptor declares (Linux v4.0 hid-sony.c:528-573,
   hid-playstation.c:340). Byte 1 bit 7 is set when controller state follows
   the three header bytes, byte 2 bit 7 when audio follows, and the last four
   bytes hold the CRC-32 of 0xA1 and every byte before them
   (khallmark/ds4mac docs/05-DS4-Bluetooth-Protocol.md:331-349,
   hbashton/DS4Windows DualShock4BluetoothAudioProtocol.cs:65-66, :91-111,
   :395-403). A read returns the report at its own length, as Linux and macOS
   do, or padded to the longest report, 547 bytes, as Windows does
   (src/hidapi/windows/hid.c:1546, :1721, :1764-1770). The test computes the
   CRC with its own CRC-32, checked against the standard check value.

   1. Report 0x11 behaves as before: a good CRC raises the counter and the
      state posts, and a bad CRC is taken until the counter reaches 3.
   2. A report 0x12 to 0x19 with a correct CRC at its own length is taken and
      its state posts, also while the counter is high, where the 78-byte
      check rejected it. One with a wrong CRC is rejected.
   3. A report with byte 1 bit 7 clear, audio alone, is skipped, and a report
      shorter than its length is rejected, the counter unmoved either way.
   4. Every report 0x11 to 0x19 arrives whole through UpdateDevice, at its
      own length and padded to 547 bytes, and a headset microphone stream of
      state and audio reports keeps the state posting.

   The driver in this file calls the fake, not SDL's HID backends. Each name
   through which it reads, queues a write or adds a joystick is defined below
   before the driver source is included. This object defines
   SDL_HIDAPI_DriverPS4, so SDL's own copy of the driver is never linked, as
   in the Wii driver's test. Before the link, test/ps4-driver/CheckSystem.cmake
   reads this object's symbols and fails the build when it still imports a
   function that reaches a HID device. */

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"
#include "joystick/SDL_sysjoystick.h"
#include "joystick/hidapi/SDL_hidapijoystick_c.h"

/* The fake, defined below. SDL_hidapi_rumble.h has no include guard, so only
   the driver includes it, after these names: its declarations then name the
   fake, and keep the internal linkage declared here. */
static int Fake_hid_read_timeout(SDL_hid_device *dev, unsigned char *data, size_t length, int milliseconds);
static int Fake_hid_get_feature_report(SDL_hid_device *dev, unsigned char *data, size_t length);
static bool Fake_LockRumble(void);
static int Fake_SendRumbleAndUnlock(SDL_HIDAPI_Device *device, const Uint8 *data, int size);
static int Fake_SendRumble(SDL_HIDAPI_Device *device, const Uint8 *data, int size);
static bool Fake_JoystickConnected(SDL_HIDAPI_Device *device, SDL_JoystickID *pJoystickID);
static void Fake_JoystickDisconnected(SDL_HIDAPI_Device *device, SDL_JoystickID joystickID);
static SDL_Joystick *Fake_GetJoystickFromID(SDL_JoystickID instance_id);

#undef SDL_hid_read_timeout
#define SDL_hid_read_timeout Fake_hid_read_timeout
#undef SDL_hid_get_feature_report
#define SDL_hid_get_feature_report Fake_hid_get_feature_report
#define SDL_HIDAPI_LockRumble Fake_LockRumble
#define SDL_HIDAPI_SendRumbleAndUnlock Fake_SendRumbleAndUnlock
#define SDL_HIDAPI_SendRumble Fake_SendRumble
#define HIDAPI_JoystickConnected Fake_JoystickConnected
#define HIDAPI_JoystickDisconnected Fake_JoystickDisconnected
#undef SDL_GetJoystickFromID
#define SDL_GetJoystickFromID Fake_GetJoystickFromID

#include "../src/joystick/hidapi/SDL_hidapi_ps4.c"

#undef SDL_hid_read_timeout
#undef SDL_hid_get_feature_report
#undef SDL_HIDAPI_LockRumble
#undef SDL_HIDAPI_SendRumbleAndUnlock
#undef SDL_HIDAPI_SendRumble
#undef HIDAPI_JoystickConnected
#undef HIDAPI_JoystickDisconnected
#undef SDL_GetJoystickFromID

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

/* The scripted controller */
#define FAKE_MAX_REPORTS 64
#define FAKE_REPORT_SIZE 547 /* The longest input report, 0x19 (hid-sony.c:572-573) */

static struct
{
    bool windows; /* Every read returns the longest input report's length, as hid.dll does */
    Uint8 queue[FAKE_MAX_REPORTS][FAKE_REPORT_SIZE];
    int queue_length[FAKE_MAX_REPORTS];
    int queue_head;
    int queue_count;
    int reads;   /* Reads that returned a report */
    int writes;  /* Writes through the rumble thread */
    Uint8 last_write[FAKE_REPORT_SIZE];
    int last_write_length;
    int locks;      /* SDL_HIDAPI_LockRumble without the send that releases it */
    int unexpected; /* Requests no DualShock 4 answers this way, or a full queue */
} fake;

static SDL_hid_device *FakeHandle(void)
{
    return (SDL_hid_device *)(void *)&fake;
}

static void Fake_Unexpected(const char *what)
{
    ++fake.unexpected;
    printf("fake: unexpected %s\n", what);
}

/* A report the controller sends, zero beyond its length */
static void Fake_Queue(const Uint8 *report, int length)
{
    int slot;

    if (fake.queue_count == FAKE_MAX_REPORTS || length > FAKE_REPORT_SIZE) {
        Fake_Unexpected("report beyond the queue");
        return;
    }
    slot = (fake.queue_head + fake.queue_count) % FAKE_MAX_REPORTS;
    SDL_memset(fake.queue[slot], 0, sizeof(fake.queue[slot]));
    SDL_memcpy(fake.queue[slot], report, (size_t)length);
    fake.queue_length[slot] = length;
    ++fake.queue_count;
}

/* As hidapi reads: the report cut to the buffer, at its own length on Linux
   and macOS, and padded with zeros to the collection's longest input report
   on Windows (src/hidapi/windows/hid.c:1546, :1721, :1764-1770) */
static int Fake_hid_read_timeout(SDL_hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
    int size;

    (void)milliseconds;
    if (dev != FakeHandle()) {
        Fake_Unexpected("read handle");
        return -1;
    }
    if (fake.queue_count == 0) {
        return 0;
    }
    size = fake.windows ? FAKE_REPORT_SIZE : fake.queue_length[fake.queue_head];
    if ((size_t)size > length) {
        size = (int)length;
    }
    SDL_memcpy(data, fake.queue[fake.queue_head], (size_t)size);
    fake.queue_head = (fake.queue_head + 1) % FAKE_MAX_REPORTS;
    --fake.queue_count;
    ++fake.reads;
    return size;
}

/* An official controller on Bluetooth asks for no feature report until
   sensors are enabled, which no scenario does */
static int Fake_hid_get_feature_report(SDL_hid_device *dev, unsigned char *data, size_t length)
{
    (void)dev;
    (void)data;
    (void)length;
    Fake_Unexpected("feature report");
    return -1;
}

static int Fake_RecordWrite(SDL_HIDAPI_Device *dev, const Uint8 *data, int size)
{
    if (!dev || dev->dev != FakeHandle() || size < 1 || size > FAKE_REPORT_SIZE) {
        Fake_Unexpected("write");
        return -1;
    }
    SDL_memcpy(fake.last_write, data, (size_t)size);
    fake.last_write_length = size;
    ++fake.writes;
    return size;
}

static bool Fake_LockRumble(void)
{
    ++fake.locks;
    return true;
}

static int Fake_SendRumbleAndUnlock(SDL_HIDAPI_Device *dev, const Uint8 *data, int size)
{
    if (fake.locks != 1) {
        Fake_Unexpected("queued write without the lock");
    }
    fake.locks = 0;
    return Fake_RecordWrite(dev, data, size);
}

static int Fake_SendRumble(SDL_HIDAPI_Device *dev, const Uint8 *data, int size)
{
    if (fake.locks != 0) {
        Fake_Unexpected("rumble write under the lock");
    }
    return Fake_RecordWrite(dev, data, size);
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

static void ResetFake(void)
{
    SDL_zero(fake);
}

/* As the HIDAPI layer sets a device up: InitDevice under the joystick lock.
   A CUH-ZCT2 on Bluetooth, whose first report InitDevice reads is a 0x11. */
static bool StartDevice(void)
{
    Uint8 first[78];
    bool result;

    ResetFake();
    SDL_zero(device);
    device.dev = FakeHandle();
    device.name = SDL_strdup("Wireless Controller");
    device.path = SDL_strdup("fake");
    device.vendor_id = USB_VENDOR_SONY;
    device.product_id = USB_PRODUCT_SONY_DS4_SLIM;
    device.is_bluetooth = true;
    device.guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, USB_VENDOR_SONY, USB_PRODUCT_SONY_DS4_SLIM, 0, NULL, device.name, 'h', 0);
    device.driver = &SDL_HIDAPI_DriverPS4;
    connections = 0;
    disconnections = 0;

    SDL_zeroa(first);
    first[0] = 0x11;
    Fake_Queue(first, sizeof(first));

    SDL_LockJoysticks();
    result = device.driver->InitDevice(&device);
    SDL_UnlockJoysticks();
    return result;
}

static void StopDevice(void)
{
    if (device.driver && device.driver->FreeDevice) {
        device.driver->FreeDevice(&device);
    }
    SDL_free(device.context);
    SDL_free(device.joysticks);
    SDL_free(device.name);
    SDL_free(device.serial);
    SDL_free(device.path);
    SDL_zero(device);
}

/* As SDL_OpenJoystick: the driver's open, then the arrays, then the initial
   values SDL knows for a HIDAPI joystick */
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
    joystick->instance_id = device.joysticks[0];
    joystick->guid = device.guid;
    joystick->attached = true;
    joystick->ref_count = 1;
    SDL_SetObjectValid(joystick, SDL_OBJECT_TYPE_JOYSTICK, true);
    record_closed = false;

    SDL_LockJoysticks();
    opened = SDL_HIDAPI_DriverPS4.OpenJoystick(&device, joystick);
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
    int i;

    if (!joystick) {
        return;
    }
    /* A joystick the driver disconnected was closed then, and SDL closes it
       only once */
    if (!record_closed) {
        SDL_LockJoysticks();
        SDL_HIDAPI_DriverPS4.CloseJoystick(&device, joystick);
        SDL_UnlockJoysticks();
    }
    record_closed = false;
    SDL_SetObjectValid(joystick, SDL_OBJECT_TYPE_JOYSTICK, false);
    if (joystick->props) {
        SDL_DestroyProperties(joystick->props);
    }
    for (i = 0; i < joystick->ntouchpads; ++i) {
        SDL_free(joystick->touchpads[i].fingers);
    }
    SDL_free(joystick->touchpads);
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
    SDL_HIDAPI_DriverPS4.UpdateDevice(&device);
    SDL_UnlockJoysticks();
}

static SDL_DriverPS4_Context *Ctx(void)
{
    return (SDL_DriverPS4_Context *)device.context;
}

/* CRC-32 as zlib computes it, bit by bit, so the reports do not depend on
   the CRC code under test. Chained calls continue one CRC. */
static Uint32 Crc32(Uint32 crc, const Uint8 *data, size_t length)
{
    size_t i;
    int bit;

    crc = ~crc;
    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* The CRC of a Bluetooth report: its HID header, 0xA1 for input and 0xA2 for
   output (hid-playstation.c:77-78), then every byte before the CRC */
static Uint32 ReportCrc(Uint8 header, const Uint8 *report, int length)
{
    return Crc32(Crc32(0, &header, 1), report, (size_t)(length - 4));
}

static Uint32 StoredCrc(const Uint8 *report, int length)
{
    return (Uint32)report[length - 4] | ((Uint32)report[length - 3] << 8) |
           ((Uint32)report[length - 2] << 16) | ((Uint32)report[length - 1] << 24);
}

/* Each state report's length, report ID through CRC: 78 for 0x11
   (hid-playstation.c:340), and for 0x12 to 0x19 one more than the count of
   8-bit fields the descriptor declares (hid-sony.c:528-573 at v4.0) */
static int ReportLength(Uint8 id)
{
    static const int counts[8] = { 141, 205, 269, 333, 397, 461, 525, 546 };

    return (id == 0x11) ? 78 : 1 + counts[id - 0x12];
}

#define STATE_PRESENT 0x80 /* Byte 1: controller state follows the header */
#define CRC_PRESENT   0x40 /* Byte 1: the report ends with a CRC */
#define AUDIO_PRESENT 0x80 /* Byte 2: audio follows */

/* A face button in the high nibble of the state's fifth byte, beside the hat,
   8 when centered (SDL_hidapi_ps4.c HandleStatePacket) */
#define FACE_CROSS  0x20
#define FACE_CIRCLE 0x40

/* A Bluetooth state report as the controller sends it: the ID, the two flag
   bytes, then with state 75 bytes of controller state from byte 3, the
   sticks centered but for left X, and audio after them, or without state
   audio from byte 3, where hbashton/DS4Windows finds it (:395-403), up to
   the CRC. The audio here is bytes no state parse expects. */
static int BuildReport(Uint8 report[FAKE_REPORT_SIZE], Uint8 id, bool state, bool audio, Uint8 lx, Uint8 face, bool good_crc)
{
    const int length = ReportLength(id);
    const int audio_start = state ? 3 + 75 : 3;
    Uint32 crc;
    int i;

    SDL_memset(report, 0, FAKE_REPORT_SIZE);
    report[0] = id;
    report[1] = (Uint8)(CRC_PRESENT | (state ? STATE_PRESENT : 0));
    report[2] = audio ? AUDIO_PRESENT : 0;
    if (audio) {
        for (i = audio_start; i < length - 4; ++i) {
            report[i] = (Uint8)(0x9C ^ i);
        }
    }
    if (state) {
        report[3] = lx;
        report[4] = 0x80;
        report[5] = 0x80;
        report[6] = 0x80;
        report[7] = (Uint8)(face | 0x08);
        report[8] = 0x00;
        report[9] = 0x00;
        report[10] = 0x00;
        report[11] = 0x00;
    }
    crc = ReportCrc(0xA1, report, length);
    if (!good_crc) {
        crc ^= 0x00000001;
    }
    report[length - 4] = (Uint8)crc;
    report[length - 3] = (Uint8)(crc >> 8);
    report[length - 2] = (Uint8)(crc >> 16);
    report[length - 1] = (Uint8)(crc >> 24);
    return length;
}

/* The left stick's X as the driver posts it */
static Sint16 AxisFor(Uint8 raw)
{
    return (Sint16)((int)raw * 257 - 32768);
}

static Sint16 LeftX(void)
{
    return joystick ? joystick->axes[SDL_GAMEPAD_AXIS_LEFTX].value : 0;
}

/* The controller sends one report and the driver reads it */
static void Send(Uint8 id, bool state, bool audio, Uint8 lx, Uint8 face, bool good_crc)
{
    Uint8 report[FAKE_REPORT_SIZE];

    Fake_Queue(report, BuildReport(report, id, state, audio, lx, face, good_crc));
    Update();
}

/* The same, arriving short by missing bytes, as a read cut short would */
static void SendShort(Uint8 id, int missing, Uint8 lx)
{
    Uint8 report[FAKE_REPORT_SIZE];

    Fake_Queue(report, BuildReport(report, id, true, id != 0x11, lx, FACE_CROSS, true) - missing);
    Update();
}

/* Brings the counter to count with good 0x11 reports, the last at lx */
static void RaiseCounter(int count, Uint8 lx)
{
    while (Ctx()->valid_crc_packets < count) {
        Send(0x11, true, false, lx, 0, true);
    }
}

static bool StartAndOpen(bool windows)
{
    if (!StartDevice() || !OpenRecord()) {
        CHECK(false, "the device did not start and open");
        CloseRecord();
        StopDevice();
        return false;
    }
    fake.windows = windows;
    /* Positive control: InitDevice read the first report and took the
       controller for one in enhanced mode */
    CHECK(fake.reads == 1 && Ctx()->enhanced_reports && connections == 1, "InitDevice: %d reads, enhanced %d, %d connections",
          fake.reads, Ctx()->enhanced_reports, connections);
    /* A first report moves the stick far from center, past the jitter gate
       an axis passes once */
    Send(0x11, true, false, 0x00, 0, true);
    CHECK(LeftX() == AxisFor(0x00), "the first report did not post: left X %d", LeftX());
    return true;
}

static void Finish(void)
{
    CHECK(fake.unexpected == 0, "%d requests the controller does not answer", fake.unexpected);
    CloseRecord();
    StopDevice();
}

/* The test's CRC against the standard check value, and against the CRC SDL
   puts on the effects report OpenJoystick sends */
static void TestCrc(void)
{
    static const Uint8 check[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };

    scenario = "CRC";
    CHECK(Crc32(0, check, sizeof(check)) == 0xCBF43926u, "CRC-32 of 123456789 is %08x, not cbf43926", Crc32(0, check, sizeof(check)));
    if (!StartAndOpen(false)) {
        return;
    }
    CHECK(fake.writes >= 1 && fake.last_write_length == 78 && fake.last_write[0] == 0x11 &&
              StoredCrc(fake.last_write, 78) == ReportCrc(0xA2, fake.last_write, 78),
          "the effects report: %d writes, %d bytes, CRC %08x, the test's %08x", fake.writes, fake.last_write_length,
          StoredCrc(fake.last_write, 78), ReportCrc(0xA2, fake.last_write, 78));
    Finish();
}

/* 1: report 0x11 as before */
static void TestReport11(void)
{
    scenario = "report 0x11";
    if (!StartAndOpen(false)) {
        return;
    }
    CHECK(Ctx()->valid_crc_packets == 1, "one good report counted %d", Ctx()->valid_crc_packets);
    Send(0x11, true, false, 0x20, FACE_CROSS, true);
    CHECK(LeftX() == AxisFor(0x20) && joystick->buttons[SDL_GAMEPAD_BUTTON_SOUTH] && Ctx()->valid_crc_packets == 2,
          "a good 0x11: left X %d, cross %d, counter %d", LeftX(), joystick->buttons[SDL_GAMEPAD_BUTTON_SOUTH], Ctx()->valid_crc_packets);
    /* A bad CRC while the counter is low is taken, as for a clone that sets
       no CRC */
    Send(0x11, true, false, 0x30, 0, false);
    CHECK(LeftX() == AxisFor(0x30) && Ctx()->valid_crc_packets == 1, "a bad 0x11 at counter 2: left X %d, counter %d", LeftX(),
          Ctx()->valid_crc_packets);
    /* Once good reports have raised it, a bad one is dropped */
    RaiseCounter(5, 0x40);
    Send(0x11, true, false, 0x50, 0, false);
    CHECK(LeftX() == AxisFor(0x40) && Ctx()->valid_crc_packets == 4, "a bad 0x11 at counter 5: left X %d, counter %d", LeftX(),
          Ctx()->valid_crc_packets);
    Finish();
}

/* 2: reports 0x12 to 0x19 checked at their own length */
static void TestLargerReports(void)
{
    Uint8 id;
    Uint8 lx = 0x10;

    scenario = "reports 0x12 to 0x19";
    if (!StartAndOpen(false)) {
        return;
    }
    RaiseCounter(5, 0x08);
    for (id = 0x12; id <= 0x19; ++id) {
        const Uint16 before = Ctx()->valid_crc_packets;

        /* State and audio with a correct CRC at the report's length */
        lx = (Uint8)(lx + 0x10);
        Send(id, true, true, lx, (id & 1) ? FACE_CIRCLE : FACE_CROSS, true);
        CHECK(LeftX() == AxisFor(lx) && Ctx()->valid_crc_packets == before + 1, "report %02x with a correct CRC: left X %d, not %d, counter %d",
              id, LeftX(), AxisFor(lx), Ctx()->valid_crc_packets);
        CHECK(joystick->buttons[(id & 1) ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH], "report %02x: its face button did not post", id);
        /* The same with a wrong CRC, dropped while the counter is high */
        Send(id, true, true, (Uint8)(lx + 0x08), 0, false);
        CHECK(LeftX() == AxisFor(lx) && Ctx()->valid_crc_packets == before, "report %02x with a wrong CRC: left X %d, counter %d", id,
              LeftX(), Ctx()->valid_crc_packets);
    }
    Finish();
}

/* 3: audio alone, and reports shorter than their length */
static void TestSkipped(void)
{
    Uint8 id;
    Uint16 before;

    scenario = "audio alone";
    if (!StartAndOpen(false)) {
        return;
    }
    RaiseCounter(5, 0x70);
    before = Ctx()->valid_crc_packets;
    for (id = 0x12; id <= 0x19; ++id) {
        const int reads = fake.reads;

        Send(id, false, true, 0, 0, true);
        CHECK(fake.reads == reads + 1 && LeftX() == AxisFor(0x70) && Ctx()->valid_crc_packets == before,
              "report %02x with audio alone: %d reads, left X %d, counter %d", id, fake.reads - reads, LeftX(), Ctx()->valid_crc_packets);
    }
    Finish();

    scenario = "reports shorter than their length";
    if (!StartAndOpen(false)) {
        return;
    }
    RaiseCounter(5, 0x70);
    before = Ctx()->valid_crc_packets;
    for (id = 0x11; id <= 0x19; ++id) {
        const int reads = fake.reads;

        SendShort(id, 1, 0x10);
        CHECK(fake.reads == reads + 1 && LeftX() == AxisFor(0x70) && Ctx()->valid_crc_packets == before,
              "report %02x one byte short: %d reads, left X %d, counter %d", id, fake.reads - reads, LeftX(), Ctx()->valid_crc_packets);
    }
    /* Positive control: the same report at its full length is taken */
    Send(0x14, true, true, 0x10, 0, true);
    CHECK(LeftX() == AxisFor(0x10) && Ctx()->valid_crc_packets == before + 1, "report 14 at its length: left X %d, counter %d", LeftX(),
          Ctx()->valid_crc_packets);
    Finish();
}

/* 4: every report read whole, at its own length and as Windows reads it */
static void TestWholeReads(void)
{
    int pass;

    for (pass = 0; pass < 2; ++pass) {
        const bool windows = (pass == 1);
        Uint8 id;
        Uint8 lx = 0x04;

        scenario = windows ? "reads padded to 547 bytes" : "reads at the report's length";
        if (!StartAndOpen(windows)) {
            continue;
        }
        RaiseCounter(5, 0x02);
        for (id = 0x11; id <= 0x19; ++id) {
            const Uint16 before = Ctx()->valid_crc_packets;

            lx = (Uint8)(lx + 0x1C);
            Send(id, true, id != 0x11, lx, 0, true);
            CHECK(LeftX() == AxisFor(lx) && Ctx()->valid_crc_packets == before + 1, "report %02x read whole: left X %d, not %d, counter %d", id,
                  LeftX(), AxisFor(lx), Ctx()->valid_crc_packets);
            Send(id, true, id != 0x11, (Uint8)(lx + 0x0E), 0, false);
            CHECK(LeftX() == AxisFor(lx) && Ctx()->valid_crc_packets == before, "report %02x with a wrong CRC: left X %d, counter %d", id,
                  LeftX(), Ctx()->valid_crc_packets);
        }
        Finish();
    }
}

/* 4: a headset microphone stream, where state rides in reports with audio
   and audio also comes alone, keeps every state report posting */
static void TestMicrophoneStream(void)
{
    static const Uint8 ids[3] = { 0x15, 0x17, 0x19 };
    int pass;

    for (pass = 0; pass < 2; ++pass) {
        const bool windows = (pass == 1);
        int i, posted = 0, missed = 0, moved = 0;
        Uint16 counter;
        Uint8 lx = 0;

        scenario = windows ? "microphone stream, Windows reads" : "microphone stream";
        if (!StartAndOpen(windows)) {
            continue;
        }
        RaiseCounter(5, 0x01);
        counter = Ctx()->valid_crc_packets;
        for (i = 0; i < 120; ++i) {
            const Uint8 id = ids[i % 3];

            if (i % 4 == 3) {
                /* Audio alone leaves the state where it was */
                const Sint16 held = LeftX();

                Send(id, false, true, 0, 0, true);
                if (LeftX() != held) {
                    ++moved;
                }
            } else {
                lx = (Uint8)((lx + 0x25) | 0x01);
                Send(id, true, true, lx, 0, true);
                ++counter;
                if (LeftX() == AxisFor(lx)) {
                    ++posted;
                } else {
                    ++missed;
                }
            }
        }
        CHECK(posted == 90 && missed == 0 && moved == 0, "%d state reports posted, %d missed, audio alone moved the stick %d times", posted,
              missed, moved);
        CHECK(Ctx()->valid_crc_packets == counter, "the counter is %d, not %d", Ctx()->valid_crc_packets, counter);
        Finish();
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* Enhanced reports on, as for a Bluetooth controller an app has opened */
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    TestCrc();
    TestReport11();
    TestLargerReports();
    TestSkipped();
    TestWholeReads();
    TestMicrophoneStream();

    SDL_Quit();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
