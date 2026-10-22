/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs the HIDAPI joystick core, src/joystick/hidapi/SDL_hidapijoystick.c,
   against a scripted device list, for hifihedgehog/SDL#39. The list holds
   one Wii Remote path, the system's device change count is the test's, and a
   scripted driver stands in the Wii driver's place in the core's driver
   table. No HID device opens.

   1. A driver that ends its reads in an update, after disconnecting its
      joystick, is released, and the core stores the system's device change
      count with the path. The rescan the disconnect asked for runs and leaves
      the path released. The next device change opens it again, and the
      driver adds its joystick.
   2. A path whose open fails is opened again at the next device change.
   3. A released path that leaves the list is deleted, and a path that comes
      back is a new device.

   The core in this file calls the fake, not SDL's HID layer: each name
   through which it lists, opens, closes or counts devices, or adds and
   looks up joysticks, is defined below before the core source is included.
   This object defines the core's symbols, so SDL's own copy of the core is
   never linked, as test/testwiidriver.c does for the Wii driver. Before the
   link, test/wii-driver/CheckSystem.cmake reads this object's symbols and
   fails the build when it still imports a function that reaches a HID
   device. */

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"
#include "SDL_hints_c.h"
#include "joystick/SDL_joystick_c.h"
#include "joystick/SDL_sysjoystick.h"
#include "joystick/hidapi/SDL_hidapijoystick_c.h"

/* The fake, defined below */
static int Fake_hid_init(void);
static int Fake_hid_exit(void);
static Uint32 Fake_hid_device_change_count(void);
static SDL_hid_device_info *Fake_hid_enumerate(unsigned short vendor_id, unsigned short product_id);
static void Fake_hid_free_enumeration(SDL_hid_device_info *devs);
static SDL_hid_device *Fake_hid_open_path(const char *path);
static int Fake_hid_close(SDL_hid_device *dev);
static int Fake_hid_set_nonblocking(SDL_hid_device *dev, int nonblock);
static SDL_PropertiesID Fake_hid_get_properties(SDL_hid_device *dev);
static SDL_Joystick *Fake_GetJoystickFromID(SDL_JoystickID instance_id);
static void Fake_PrivateJoystickAdded(SDL_JoystickID instance_id);
static void Fake_PrivateJoystickRemoved(SDL_JoystickID instance_id);
static SDL_HIDAPI_DeviceDriver Test_DriverWii;

#undef SDL_hid_init
#define SDL_hid_init Fake_hid_init
#undef SDL_hid_exit
#define SDL_hid_exit Fake_hid_exit
#undef SDL_hid_device_change_count
#define SDL_hid_device_change_count Fake_hid_device_change_count
#undef SDL_hid_enumerate
#define SDL_hid_enumerate Fake_hid_enumerate
#undef SDL_hid_free_enumeration
#define SDL_hid_free_enumeration Fake_hid_free_enumeration
#undef SDL_hid_open_path
#define SDL_hid_open_path Fake_hid_open_path
#undef SDL_hid_close
#define SDL_hid_close Fake_hid_close
#undef SDL_hid_set_nonblocking
#define SDL_hid_set_nonblocking Fake_hid_set_nonblocking
#undef SDL_hid_get_properties
#define SDL_hid_get_properties Fake_hid_get_properties
#undef SDL_GetJoystickFromID
#define SDL_GetJoystickFromID Fake_GetJoystickFromID
#define SDL_PrivateJoystickAdded Fake_PrivateJoystickAdded
#define SDL_PrivateJoystickRemoved Fake_PrivateJoystickRemoved
#define SDL_HIDAPI_DriverWii Test_DriverWii

#include "../src/joystick/hidapi/SDL_hidapijoystick.c"

#undef SDL_hid_init
#undef SDL_hid_exit
#undef SDL_hid_device_change_count
#undef SDL_hid_enumerate
#undef SDL_hid_free_enumeration
#undef SDL_hid_open_path
#undef SDL_hid_close
#undef SDL_hid_set_nonblocking
#undef SDL_hid_get_properties
#undef SDL_GetJoystickFromID
#undef SDL_PrivateJoystickAdded
#undef SDL_PrivateJoystickRemoved
#undef SDL_HIDAPI_DriverWii

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

/* The system: one Wii Remote path, listed or not, and its device change
   count. The count starts far from 0, which the core takes as "rescan". */
static char fake_path[] = "test-wii";
static wchar_t fake_product[] = L"RVL-CNT-01";
static SDL_hid_device_info fake_info;
static bool fake_listed = true;
static bool fake_open_fails;
static Uint32 fake_change_count = 100;
static int fake_handle;
static int enumerations, opens, closes, added, removed;

static int Fake_hid_init(void)
{
    return 0;
}

static int Fake_hid_exit(void)
{
    return 0;
}

static Uint32 Fake_hid_device_change_count(void)
{
    return fake_change_count;
}

static SDL_hid_device_info *Fake_hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
    (void)vendor_id;
    (void)product_id;
    ++enumerations;
    if (!fake_listed) {
        return NULL;
    }
    SDL_zero(fake_info);
    fake_info.path = fake_path;
    fake_info.vendor_id = USB_VENDOR_NINTENDO;
    fake_info.product_id = USB_PRODUCT_NINTENDO_WII_REMOTE;
    fake_info.product_string = fake_product;
    fake_info.interface_number = -1;
    fake_info.bus_type = SDL_HID_API_BUS_BLUETOOTH;
    return &fake_info;
}

static void Fake_hid_free_enumeration(SDL_hid_device_info *devs)
{
    (void)devs;
}

static SDL_hid_device *Fake_hid_open_path(const char *path)
{
    ++opens;
    if (fake_open_fails || SDL_strcmp(path, fake_path) != 0) {
        return NULL;
    }
    return (SDL_hid_device *)(void *)&fake_handle;
}

static int Fake_hid_close(SDL_hid_device *dev)
{
    CHECK(dev == (SDL_hid_device *)(void *)&fake_handle, "closed a handle the test never opened");
    ++closes;
    return 0;
}

static int Fake_hid_set_nonblocking(SDL_hid_device *dev, int nonblock)
{
    (void)dev;
    (void)nonblock;
    return 0;
}

static SDL_PropertiesID Fake_hid_get_properties(SDL_hid_device *dev)
{
    (void)dev;
    return 0;
}

/* No joystick is ever opened, so there is none to find */
static SDL_Joystick *Fake_GetJoystickFromID(SDL_JoystickID instance_id)
{
    (void)instance_id;
    return NULL;
}

static void Fake_PrivateJoystickAdded(SDL_JoystickID instance_id)
{
    (void)instance_id;
    ++added;
}

static void Fake_PrivateJoystickRemoved(SDL_JoystickID instance_id)
{
    (void)instance_id;
    ++removed;
}

/* The driver: InitDevice adds a joystick, and an update ends its reads when
   the test asks, the way the Wii driver releases a remote that refused a
   write: it disconnects its joystick and sets reads_ended */
static int inits, frees;
static bool end_reads;

static void Test_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    (void)callback;
    (void)userdata;
}

static bool Test_IsEnabled(void)
{
    return true;
}

static bool Test_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    (void)device;
    (void)name;
    (void)type;
    (void)version;
    (void)interface_number;
    (void)interface_class;
    (void)interface_subclass;
    (void)interface_protocol;
    return vendor_id == USB_VENDOR_NINTENDO && product_id == USB_PRODUCT_NINTENDO_WII_REMOTE;
}

static bool Test_InitDevice(SDL_HIDAPI_Device *device)
{
    ++inits;
    return HIDAPI_JoystickConnected(device, NULL);
}

static int Test_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    (void)device;
    (void)instance_id;
    return -1;
}

static void Test_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
    (void)device;
    (void)instance_id;
    (void)player_index;
}

static bool Test_UpdateDevice(SDL_HIDAPI_Device *device)
{
    if (device->num_joysticks == 0) {
        return false;
    }
    if (end_reads) {
        end_reads = false;
        HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        device->reads_ended = true;
        return false;
    }
    return true;
}

static bool Test_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    (void)device;
    (void)joystick;
    return false;
}

static bool Test_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    (void)device;
    (void)joystick;
    (void)low_frequency_rumble;
    (void)high_frequency_rumble;
    return SDL_Unsupported();
}

static bool Test_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    (void)device;
    (void)joystick;
    (void)left_rumble;
    (void)right_rumble;
    return SDL_Unsupported();
}

static Uint32 Test_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    (void)device;
    (void)joystick;
    return 0;
}

static bool Test_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)device;
    (void)joystick;
    (void)red;
    (void)green;
    (void)blue;
    return SDL_Unsupported();
}

static bool Test_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    (void)device;
    (void)joystick;
    (void)data;
    (void)size;
    return SDL_Unsupported();
}

static bool Test_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    (void)device;
    (void)joystick;
    (void)enabled;
    return SDL_Unsupported();
}

static void Test_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    (void)device;
    (void)joystick;
}

static void Test_FreeDevice(SDL_HIDAPI_Device *device)
{
    (void)device;
    ++frees;
}

static SDL_HIDAPI_DeviceDriver Test_DriverWii = {
    "test",
    true,
    Test_RegisterHints,
    Test_RegisterHints,
    Test_IsEnabled,
    Test_IsSupportedDevice,
    Test_InitDevice,
    Test_GetDevicePlayerIndex,
    Test_SetDevicePlayerIndex,
    Test_UpdateDevice,
    Test_OpenJoystick,
    Test_RumbleJoystick,
    Test_RumbleJoystickTriggers,
    Test_GetJoystickCapabilities,
    Test_SetJoystickLED,
    Test_SendJoystickEffect,
    Test_SetJoystickSensorsEnabled,
    Test_CloseJoystick,
    Test_FreeDevice,
};

/* The one device the core lists, set up by the test driver */
static bool DriverHolds(void)
{
    return SDL_HIDAPI_devices && SDL_HIDAPI_devices->driver == &Test_DriverWii && SDL_HIDAPI_devices->dev &&
           SDL_HIDAPI_numjoysticks == 1;
}

static bool Released(void)
{
    return SDL_HIDAPI_devices && !SDL_HIDAPI_devices->driver && !SDL_HIDAPI_devices->dev && SDL_HIDAPI_devices->reopen &&
           SDL_HIDAPI_numjoysticks == 0;
}

/* As SDL's joystick core runs the backend: detection, then the updates,
   under the joystick lock */
static void Poll(void)
{
    HIDAPI_JoystickDetect();
    HIDAPI_UpdateDevices();
}

/* 1 */
static void TestReadsEnded(void)
{
    int scans;

    scenario = "reads ended";
    /* Positive control: the path opened, and the driver added its joystick */
    CHECK(DriverHolds() && opens == 1 && inits == 1 && added == 1, "the device did not start: %d opens, %d inits, %d added",
          opens, inits, added);

    end_reads = true;
    HIDAPI_UpdateDevices();
    CHECK(Released() && frees == 1 && closes == 1 && removed == 1, "the driver was not released: %d frees, %d closes, %d removed",
          frees, closes, removed);
    CHECK(SDL_HIDAPI_devices && SDL_HIDAPI_devices->reopen_change_count == fake_change_count,
          "stored device change count %u, not the system's %u", SDL_HIDAPI_devices ? SDL_HIDAPI_devices->reopen_change_count : 0,
          fake_change_count);
    /* Positive control: the disconnect asked for a rescan */
    CHECK(SDL_HIDAPI_change_count == 0, "no rescan was asked for");

    scans = enumerations;
    Poll();
    CHECK(enumerations == scans + 1, "the rescan did not run");
    CHECK(Released() && opens == 1 && inits == 1, "the path opened again at the rescan, with no device change: %d opens", opens);
    Poll();
    Poll();
    CHECK(Released() && opens == 1 && enumerations == scans + 1, "the path opened again with no device change: %d opens, %d scans",
          opens, enumerations - scans);

    ++fake_change_count;
    Poll();
    CHECK(DriverHolds() && opens == 2 && inits == 2 && added == 2, "the next device change did not open the path: %d opens, %d inits",
          opens, inits);
    CHECK(SDL_HIDAPI_devices && !SDL_HIDAPI_devices->reads_ended, "reads_ended stayed set");
}

/* 2 */
static void TestOpenFails(void)
{
    scenario = "open fails";
    end_reads = true;
    HIDAPI_UpdateDevices();
    CHECK(Released(), "the driver was not released");
    fake_open_fails = true;
    ++fake_change_count;
    Poll();
    CHECK(Released() && opens == 3, "the device change did not try the path: %d opens", opens);
    fake_open_fails = false;
    Poll();
    Poll();
    CHECK(Released() && opens == 3, "the failed path opened again with no device change: %d opens", opens);
    ++fake_change_count;
    Poll();
    CHECK(DriverHolds() && opens == 4 && inits == 3, "the next device change did not open the path: %d opens, %d inits", opens, inits);
}

/* 3 */
static void TestPathLeaves(void)
{
    scenario = "path leaves the list";
    end_reads = true;
    HIDAPI_UpdateDevices();
    CHECK(Released(), "the driver was not released");
    fake_listed = false;
    ++fake_change_count;
    Poll();
    CHECK(!SDL_HIDAPI_devices, "the device stayed after its path left the list");

    fake_listed = true;
    ++fake_change_count;
    Poll();
    CHECK(DriverHolds() && opens == 5 && inits == 4, "the path that came back is not a device: %d opens, %d inits", opens, inits);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_LockJoysticks();
    if (!HIDAPI_JoystickInit()) {
        CHECK(false, "the core did not start: %s", SDL_GetError());
    } else {
        TestReadsEnded();
        TestOpenFails();
        TestPathLeaves();
        HIDAPI_JoystickQuit();
        CHECK(frees == inits && closes == opens - 1, "the core quit with %d frees for %d inits, %d closes for %d opens",
              frees, inits, closes, opens);
    }
    SDL_UnlockJoysticks();

    SDL_Quit();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
