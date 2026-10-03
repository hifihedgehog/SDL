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

#include "../src/joystick/hidapi/SDL_hidapi_wii.c"

#undef SDL_hid_read_timeout
#undef SDL_hid_write
#undef SDL_HIDAPI_LockRumble
#undef SDL_HIDAPI_SendRumbleAndUnlock
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

/* The scripted remote */
#define FAKE_MAX_REPORTS 64
#define FAKE_MAX_WRITES  128
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
    bool async; /* Queued through the rumble thread */
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
    Uint8 queue[FAKE_MAX_REPORTS][FAKE_REPORT_SIZE];
    int queue_length[FAKE_MAX_REPORTS];
    int queue_head;
    int queue_count;
    FakeOutput outputs[FAKE_MAX_WRITES];
    int noutputs;
    int unexpected; /* Requests no Wii Remote answers this way, or a full log */
    int locks;      /* SDL_HIDAPI_LockRumble without the send that releases it */
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

static void Fake_Queue(const Uint8 *data, int length)
{
    int slot;

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
   host selects one again (Linux hid-wiimote-core.c:1420-1421). */
static void Fake_QueueStatus(void)
{
    Uint8 report[7];

    report[0] = 0x20;
    report[1] = fake.buttons[0];
    report[2] = fake.buttons[1];
    report[3] = (Uint8)(((fake.extension != FAKE_EXT_NONE || Fake_MotionPlusActive()) ? 0x02 : 0x00) |
                        ((fake.camera && fake.camera_logic) ? 0x08 : 0x00) | 0x10);
    report[4] = 0x00;
    report[5] = 0x00;
    report[6] = 0xC8;
    Fake_Queue(report, sizeof(report));
    fake.report_mode = 0;
}

/* 22 BB BB 16 EE */
static void Fake_QueueAck(Uint8 error)
{
    Uint8 report[5];

    report[0] = 0x22;
    report[1] = fake.buttons[0];
    report[2] = fake.buttons[1];
    report[3] = 0x16;
    report[4] = error;
    Fake_Queue(report, sizeof(report));
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
           (hid-wiimote-core.c:493-510) */
        if (fake.motion_plus && !Fake_MotionPlusActive() && size == 1 && Fake_IsPassthroughMode(data[6])) {
            fake.motion_plus_mode = data[6];
            pulse = true;
        } else {
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
        break;
    case 0xB00033:
        fake.camera_mode = data[6];
        break;
    default:
        Fake_Unexpected("write address");
        error = 8;
        break;
    }
    Fake_QueueAck(error);
    if (pulse) {
        Fake_QueueStatus();
    }
}

/* Every output report, the way a Wii Remote takes it */
static void Fake_Output(const Uint8 *data, int length, bool async)
{
    if (fake.noutputs == FAKE_MAX_WRITES || length < 2 || length > FAKE_REPORT_SIZE) {
        Fake_Unexpected("output report");
        return;
    }
    fake.outputs[fake.noutputs].async = async;
    fake.outputs[fake.noutputs].length = length;
    SDL_memcpy(fake.outputs[fake.noutputs].data, data, (size_t)length);
    ++fake.noutputs;

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
    case 0x13:
        fake.camera = (data[1] & 0x04) != 0;
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
        break;
    default:
        Fake_Unexpected("output report type");
        break;
    }
}

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
    Fake_Output(data, (int)length, false);
    return (int)length;
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

static void Fake_JoystickDisconnected(SDL_HIDAPI_Device *dev, SDL_JoystickID joystickID)
{
    int i;

    for (i = 0; i < dev->num_joysticks; ++i) {
        if (dev->joysticks[i] == joystickID) {
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

/* As the HIDAPI layer sets a device up: InitDevice under the joystick lock */
static bool StartDevice(FakeExtension extension, bool motion_plus, Uint16 product_id)
{
    bool result;

    ResetFake(extension, motion_plus);
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
    SDL_LockJoysticks();
    SDL_HIDAPI_DriverWii.CloseJoystick(&device, joystick);
    SDL_UnlockJoysticks();
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

/* The writes of one camera enable, as WiimoteLib's EnableIR sends them
   (Wiimote.cs:1137-1183) with maximum sensitivity (:1013, :1175-1176), then
   the data report. All but the two output reports wait for the remote's
   acknowledge. */
typedef struct ExpectedOutput
{
    bool async;
    int length;
    Uint8 data[FAKE_REPORT_SIZE];
} ExpectedOutput;

static int CameraSequence(ExpectedOutput *out, Uint8 camera_mode, Uint8 report)
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
    out[n++].data[1] = 0x04;
    SDL_zerop(&out[n]);
    out[n].async = true;
    out[n].length = 2;
    out[n].data[0] = 0x1A;
    out[n++].data[1] = 0x04;
    for (i = 0; i < 5; ++i) {
        SDL_zerop(&out[n]);
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
    SDL_zerop(&out[n]);
    out[n].async = true;
    out[n].length = 3;
    out[n].data[0] = 0x12;
    out[n].data[1] = 0x04;
    out[n++].data[2] = report;
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

static ExpectedOutput Register(Uint32 address, Uint8 value)
{
    ExpectedOutput out;

    SDL_zero(out);
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
   high 6 bits above two flags, and byte 5 bit 1 marks M+ data */
static void MotionPlusFrame(Uint8 ext[6], int yaw, int roll, int pitch, bool yaw_slow, bool roll_slow, bool pitch_slow)
{
    ext[0] = (Uint8)yaw;
    ext[1] = (Uint8)roll;
    ext[2] = (Uint8)pitch;
    ext[3] = (Uint8)((((yaw >> 8) & 0x3F) << 2) | (yaw_slow ? 0x02 : 0x00) | (pitch_slow ? 0x01 : 0x00));
    ext[4] = (Uint8)((((roll >> 8) & 0x3F) << 2) | (roll_slow ? 0x02 : 0x00) | 0x01);
    ext[5] = (Uint8)((((pitch >> 8) & 0x3F) << 2) | 0x02);
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

/* The remote sends a data report in the mode it streams */
static void Feed(const Uint8 *report, int length)
{
    CHECK(fake.report_mode == report[0], "the remote streams report %02x, not %02x", fake.report_mode, report[0]);
    Fake_Queue(report, length);
    Update();
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
    n = CameraSequence(expected, 0x01, 0x37);
    CheckOutputs(mark, expected, n, "Classic enable");
    CHECK(fake.camera && fake.camera_logic && fake.camera_mode == 0x01 && fake.report_mode == 0x37,
          "the remote's camera is %d %d mode %02x, streaming %02x", fake.camera, fake.camera_logic, fake.camera_mode, fake.report_mode);
    Settle();
    CHECK(fake.report_mode == 0x37, "after the status report the remote streams %02x", fake.report_mode);

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
    n = CameraSequence(expected, 0x03, 0x33);
    CheckOutputs(mark, expected, n, "bare enable");
    CHECK(fake.camera_mode == 0x03 && fake.report_mode == 0x33, "camera mode %02x, report %02x", fake.camera_mode, fake.report_mode);
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
    expected[0] = Register(0xA600FE, 0x07);
    n = 1 + CameraSequence(expected + 1, 0x01, 0x37);
    CheckOutputs(mark, expected, n, "Classic and Motion Plus enable");
    CHECK(fake.motion_plus_mode == 0x07 && fake.camera_mode == 0x01 && fake.report_mode == 0x37,
          "Motion Plus mode %02x, camera mode %02x, report %02x", fake.motion_plus_mode, fake.camera_mode, fake.report_mode);
    Settle();
    CHECK(fake.report_mode == 0x37, "after the status reports the remote streams %02x", fake.report_mode);

    /* 3: a gyro frame, then Classic frames, alternating as a Motion Plus
       sends them (Dolphin MotionPlus.cpp:571-573) */
    BasicIR(ir, 100, 200, 300, 400);
    MotionPlusFrame(ext, 8192 + 4096, 8192, 8192 - 4096, true, false, false);
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

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    /* The driver stamps its last status request with SDL_GetTicks and takes
       0 as none sent, so it would ask again on every update within SDL's
       first millisecond. No application reaches a joystick that early. */
    while (SDL_GetTicks() == 0) {
        SDL_Delay(1);
    }

    TestRegistration();
    TestClassicCamera();
    TestBareRemoteCamera();
    TestClassicMotionPlus();
    TestNoCamera();

    SDL_Quit();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
