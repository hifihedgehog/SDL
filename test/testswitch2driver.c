/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_HIDAPI_DriverSwitch2, the HIDAPI Switch 2 driver, against a
   scripted USB bus, for hifihedgehog/SDL#37. On Windows the driver reads
   input from the controller's HID interface and finds the bulk interface of
   the same controller by walking libusb's device list. This test checks
   which device that walk takes when several share the controller's vendor
   and product ID.

   No USB or HID device opens. The driver's libusb function table, the HID
   serial number it reads, the HIDMaestro lookup and its joystick connections
   go to the fake below. The test calls the driver's InitDevice and
   FreeDevice on a hand-built device record under SDL's joystick lock, as
   the HIDAPI layer does, and keeps the record between starts as that layer
   keeps it.

   The units on the bus follow the references:
   - A real controller declares iSerialNumber 3 and answers it with "00",
     while its flash holds the unit's serial at 0x13002
     (switch2_controller_research descriptors.md and memory_layout.md).
   - A HIDMaestro persona carries the same IDs, answers with HM and twelve hex
     digits, and stores the same string in flash (HIDMaestro
     DeviceIdentity.cs:106, Switch2ProDevice.cs BuildFlash).
   - The descriptors are a Pro Controller 2's: device descriptor
     12 01 00 02 ef 02 01 40 7e 05 69 20 01 01 01 02 03 01, HID on interface
     0, bulk OUT 0x02 and bulk IN 0x82 on interface 1.
   - A flash read answers with 80 bytes, 64 and then 16, and any other
     command with a short reply (commands.md, Read Memory Block).
   - libusb turns a string descriptor into ASCII by keeping a UTF-16 code
     unit below 0x80 and writing '?' for any other (libusb
     descriptor.c:1369-1388).

   1. A controller alone opens on its first start and on every later start
      of the same device record, where device->serial holds the flash serial.
   2. Beside a HIDMaestro persona, in either list order and on every start,
      the controller's own unit takes the start sequence. The persona is
      never claimed and takes no command.
   3. When the controller's USB device cannot be opened, nothing else opens
      in its place: a unit whose serial read back as another is never the
      fallback.
   4. When the HID interface gives no serial, a unit whose serial a
      HIDMaestro persona owns is skipped, and the single-controller fallback
      still opens a controller alone.
   5. A unit with no serial string, or one that cannot be read, may be the
      fallback and loses to a serial match.
   6. Two personas each open their own unit when nothing classifies them, as
      in HIDMaestro's unfiltered acceptance build.
   7. A serial with a character outside ASCII, and one of the longest length
      a string descriptor holds, match their own unit only.

   The driver in this file calls the fake, not libusb or SDL's HID backends.
   Each name through which it reaches a device is defined below before the
   driver source is included. This object defines SDL_HIDAPI_DriverSwitch2,
   so SDL's own copy of the driver is never linked, as in the Wii driver's
   test. Before the link, test/switch2-driver/CheckSystem.cmake reads this
   object's symbols and fails the build when it still imports a function
   that reaches a device. */

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"
#include "SDL_hints_c.h"
#include "joystick/SDL_sysjoystick.h"
#include "joystick/hidapi/SDL_hidapijoystick_c.h"

/* The fake, defined below. SDL_libusb.h and SDL_hidapi_rumble.h have no
   include guard, so only the driver includes them, after these names: their
   declarations then name the fake, and keep the internal linkage declared
   here. */
struct SDL_LibUSBContext;
static bool Fake_InitLibUSB(struct SDL_LibUSBContext **ctx);
static void Fake_QuitLibUSB(void);
static int Fake_hid_get_serial_number_string(SDL_hid_device *dev, wchar_t *string, size_t maxlen);
static int Fake_hid_read_timeout(SDL_hid_device *dev, unsigned char *data, size_t length, int milliseconds);
static int Fake_HidmaestroOwnsUsbSerial(unsigned short vendor_id, unsigned short product_id, const char *serial);
static bool Fake_LockRumble(void);
static int Fake_SendRumbleAndUnlock(SDL_HIDAPI_Device *device, const Uint8 *data, int size);
static bool Fake_JoystickConnected(SDL_HIDAPI_Device *device, SDL_JoystickID *pJoystickID);
static void Fake_JoystickDisconnected(SDL_HIDAPI_Device *device, SDL_JoystickID joystickID);

#define SDL_InitLibUSB Fake_InitLibUSB
#define SDL_QuitLibUSB Fake_QuitLibUSB
#undef SDL_hid_get_serial_number_string
#define SDL_hid_get_serial_number_string Fake_hid_get_serial_number_string
#undef SDL_hid_read_timeout
#define SDL_hid_read_timeout Fake_hid_read_timeout
#define SDL_HidmaestroOwnsUsbSerial Fake_HidmaestroOwnsUsbSerial
#define SDL_HIDAPI_LockRumble Fake_LockRumble
#define SDL_HIDAPI_SendRumbleAndUnlock Fake_SendRumbleAndUnlock
#define HIDAPI_JoystickConnected Fake_JoystickConnected
#define HIDAPI_JoystickDisconnected Fake_JoystickDisconnected

/* The driver leaves parameters of its HIDAPI entry points unused (C4100),
   which SDL's own /W3 build does not report */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
#include "../src/joystick/hidapi/SDL_hidapi_switch2.c"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#undef SDL_InitLibUSB
#undef SDL_QuitLibUSB
#undef SDL_hid_get_serial_number_string
#undef SDL_hid_read_timeout
#undef SDL_HidmaestroOwnsUsbSerial
#undef SDL_HIDAPI_LockRumble
#undef SDL_HIDAPI_SendRumbleAndUnlock
#undef HIDAPI_JoystickConnected
#undef HIDAPI_JoystickDisconnected

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

/* The scripted bus */
#define FAKE_MAX_UNITS    6
#define FAKE_MAX_COMMANDS 32
#define FAKE_REPLY_SIZE   0x50

#define PRODUCT_PRO      USB_PRODUCT_NINTENDO_SWITCH2_PRO
#define PRODUCT_JOYCON_R USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT

/* What a scenario puts on the bus */
typedef struct Unit
{
    Uint16 vendor_id;
    Uint16 product_id;
    Uint8 serial_index;       /* iSerialNumber, 0 when the unit declares none */
    const wchar_t *serial;    /* The string descriptor it names */
    int serial_length;        /* Its count of code units when a NUL is among them, else 0 */
    int serial_error;         /* What the string read returns in its place */
    int open_error;           /* What the open returns in its place */
    const char *flash_serial; /* 0x13002 */
    bool persona;             /* A HIDMaestro persona, whose HID interface the filter hides */
} Unit;

typedef struct FakeUnit
{
    Unit unit;
    char device_tag; /* Its address is the unit's libusb_device */
    char handle_tag; /* Its address is the unit's libusb_device_handle */
    bool open;
    bool claimed;
    bool started; /* Took command 03/0D, after which a controller sends input */
    int opens;
    int closes;
    int string_reads;
    int claims;
    int releases;
    int bulk_out;
    int bulk_in;
    Uint16 commands[FAKE_MAX_COMMANDS]; /* Each bulk OUT's command and subcommand */
    Uint32 addresses[FAKE_MAX_COMMANDS]; /* The address of each flash read among them */
    int ncommands;
    Uint8 reply[FAKE_REPLY_SIZE];
    int reply_length;
    int reply_offset;
} FakeUnit;

static struct
{
    FakeUnit units[FAKE_MAX_UNITS];
    int nunits;
    int list_error;            /* What get_device_list returns in place of a list */
    const wchar_t *hid_serial; /* What the HID interface reports, NULL when the read fails */
    bool filter;               /* The HIDMaestro filter classifies a persona's HID interface */
    char context_tag;          /* Its address is the libusb_context */
    char hid_tag;              /* Its address is the SDL_hid_device */
    int contexts;
    int lists;
    int configs;
    int libusb_refs; /* SDL_InitLibUSB less SDL_QuitLibUSB */
    int hid_serial_reads;
    int hidmaestro_calls;
    int connections;
    int unexpected;
} fake;

static SDL_LibUSBContext fake_libusb;

static void Fake_Unexpected(const char *what)
{
    ++fake.unexpected;
    printf("fake: unexpected %s\n", what);
}

static SDL_hid_device *FakeHandle(void)
{
    return (SDL_hid_device *)(void *)&fake.hid_tag;
}

static libusb_context *FakeContext(void)
{
    return (libusb_context *)(void *)&fake.context_tag;
}

static FakeUnit *UnitOfDevice(libusb_device *dev)
{
    int i;

    for (i = 0; i < fake.nunits; ++i) {
        if (dev == (libusb_device *)(void *)&fake.units[i].device_tag) {
            return &fake.units[i];
        }
    }
    return NULL;
}

static FakeUnit *UnitOfHandle(libusb_device_handle *handle)
{
    int i;

    for (i = 0; i < fake.nunits; ++i) {
        if (handle == (libusb_device_handle *)(void *)&fake.units[i].handle_tag) {
            return &fake.units[i];
        }
    }
    return NULL;
}

/* A serial string descriptor as libusb_get_string_descriptor_ascii returns
   it (libusb descriptor.c:1369-1388): every code unit the descriptor holds,
   a NUL among them, and their count. units is that count, or 0 for a string
   that ends at its NUL. */
static int SerialToASCII(const wchar_t *serial, int units, unsigned char *data, int length)
{
    int i;

    if (units == 0) {
        units = (int)SDL_wcslen(serial);
    }
    for (i = 0; i < units && i < length - 1; ++i) {
        data[i] = (serial[i] < 0x80) ? (unsigned char)serial[i] : (unsigned char)'?';
    }
    data[i] = 0;
    return i;
}

static int LIBUSB_CALL Fake_init(libusb_context **ctx)
{
    *ctx = FakeContext();
    ++fake.contexts;
    return 0;
}

static void LIBUSB_CALL Fake_exit(libusb_context *ctx)
{
    if (ctx != FakeContext() || fake.contexts < 1) {
        Fake_Unexpected("exit of a context the driver did not create");
        return;
    }
    --fake.contexts;
}

static ssize_t LIBUSB_CALL Fake_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    libusb_device **devices;
    int i;

    if (ctx != FakeContext()) {
        Fake_Unexpected("device list from another context");
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    if (fake.list_error) {
        return fake.list_error;
    }
    devices = (libusb_device **)SDL_calloc((size_t)fake.nunits + 1, sizeof(*devices));
    if (!devices) {
        return LIBUSB_ERROR_NO_MEM;
    }
    for (i = 0; i < fake.nunits; ++i) {
        devices[i] = (libusb_device *)(void *)&fake.units[i].device_tag;
    }
    *list = devices;
    ++fake.lists;
    return fake.nunits;
}

static void LIBUSB_CALL Fake_free_device_list(libusb_device **list, int unref_devices)
{
    if (!list || unref_devices != 1 || fake.lists < 1) {
        Fake_Unexpected("free of a device list");
        return;
    }
    SDL_free(list);
    --fake.lists;
}

/* 12 01 00 02 ef 02 01 40 7e 05 69 20 01 01 01 02 03 01, with the unit's IDs
   and serial index */
static int LIBUSB_CALL Fake_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc)
{
    FakeUnit *unit = UnitOfDevice(dev);

    if (!unit) {
        Fake_Unexpected("descriptor of a device not on the bus");
        return LIBUSB_ERROR_NO_DEVICE;
    }
    SDL_zerop(desc);
    desc->bLength = LIBUSB_DT_DEVICE_SIZE;
    desc->bDescriptorType = LIBUSB_DT_DEVICE;
    desc->bcdUSB = 0x0200;
    desc->bDeviceClass = 0xEF;
    desc->bDeviceSubClass = 0x02;
    desc->bDeviceProtocol = 0x01;
    desc->bMaxPacketSize0 = 0x40;
    desc->idVendor = unit->unit.vendor_id;
    desc->idProduct = unit->unit.product_id;
    desc->bcdDevice = 0x0101;
    desc->iManufacturer = 1;
    desc->iProduct = 2;
    desc->iSerialNumber = unit->unit.serial_index;
    desc->bNumConfigurations = 1;
    return 0;
}

static int LIBUSB_CALL Fake_get_string_descriptor_ascii(libusb_device_handle *handle, uint8_t index, unsigned char *data, int length)
{
    FakeUnit *unit = UnitOfHandle(handle);

    if (!unit || !unit->open || index == 0 || index != unit->unit.serial_index || !data || length < 1) {
        Fake_Unexpected("string descriptor read");
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    ++unit->string_reads;
    if (unit->unit.serial_error) {
        return unit->unit.serial_error;
    }
    return SerialToASCII(unit->unit.serial, unit->unit.serial_length, data, length);
}

/* Interface 0 is HID with two interrupt endpoints, interface 1 is class 0xFF
   with the bulk pair */
static const struct libusb_endpoint_descriptor fake_hid_endpoints[] = {
    { LIBUSB_DT_ENDPOINT_SIZE, LIBUSB_DT_ENDPOINT, 0x81, 0x03, 64, 4, 0, 0, NULL, 0 },
    { LIBUSB_DT_ENDPOINT_SIZE, LIBUSB_DT_ENDPOINT, 0x01, 0x03, 64, 4, 0, 0, NULL, 0 }
};
static const struct libusb_endpoint_descriptor fake_bulk_endpoints[] = {
    { LIBUSB_DT_ENDPOINT_SIZE, LIBUSB_DT_ENDPOINT, 0x02, 0x02, 64, 0, 0, 0, NULL, 0 },
    { LIBUSB_DT_ENDPOINT_SIZE, LIBUSB_DT_ENDPOINT, 0x82, 0x02, 64, 0, 0, 0, NULL, 0 }
};
static const struct libusb_interface_descriptor fake_altsettings[] = {
    { LIBUSB_DT_INTERFACE_SIZE, LIBUSB_DT_INTERFACE, 0, 0, 2, 0x03, 0x00, 0x00, 5, fake_hid_endpoints, NULL, 0 },
    { LIBUSB_DT_INTERFACE_SIZE, LIBUSB_DT_INTERFACE, 1, 0, 2, 0xFF, 0x00, 0x00, 6, fake_bulk_endpoints, NULL, 0 }
};
static const struct libusb_interface fake_interfaces[] = {
    { &fake_altsettings[0], 1 },
    { &fake_altsettings[1], 1 }
};
static struct libusb_config_descriptor fake_config = {
    LIBUSB_DT_CONFIG_SIZE, LIBUSB_DT_CONFIG, 80, 2, 1, 4, 0xC0, 0xFA, fake_interfaces, NULL, 0
};

static int LIBUSB_CALL Fake_get_config_descriptor(libusb_device *dev, uint8_t config_index, struct libusb_config_descriptor **config)
{
    if (!UnitOfDevice(dev) || config_index != 0) {
        Fake_Unexpected("configuration descriptor read");
        return LIBUSB_ERROR_NOT_FOUND;
    }
    *config = &fake_config;
    ++fake.configs;
    return 0;
}

static void LIBUSB_CALL Fake_free_config_descriptor(struct libusb_config_descriptor *config)
{
    if (config != &fake_config || fake.configs < 1) {
        Fake_Unexpected("free of a configuration descriptor");
        return;
    }
    --fake.configs;
}

static int LIBUSB_CALL Fake_open(libusb_device *dev, libusb_device_handle **handle)
{
    FakeUnit *unit = UnitOfDevice(dev);

    if (!unit || unit->open) {
        Fake_Unexpected("open");
        return LIBUSB_ERROR_OTHER;
    }
    if (unit->unit.open_error) {
        return unit->unit.open_error;
    }
    unit->open = true;
    ++unit->opens;
    *handle = (libusb_device_handle *)(void *)&unit->handle_tag;
    return 0;
}

static void LIBUSB_CALL Fake_close(libusb_device_handle *handle)
{
    FakeUnit *unit = UnitOfHandle(handle);

    if (!unit || !unit->open || unit->claimed) {
        Fake_Unexpected("close");
        return;
    }
    unit->open = false;
    ++unit->closes;
}

static libusb_device *LIBUSB_CALL Fake_get_device(libusb_device_handle *handle)
{
    FakeUnit *unit = UnitOfHandle(handle);

    if (!unit || !unit->open) {
        Fake_Unexpected("device of a handle that is not open");
        return NULL;
    }
    return (libusb_device *)(void *)&unit->device_tag;
}

static int LIBUSB_CALL Fake_claim_interface(libusb_device_handle *handle, int interface_number)
{
    FakeUnit *unit = UnitOfHandle(handle);

    if (!unit || !unit->open || unit->claimed || interface_number != 1) {
        Fake_Unexpected("claim");
        return LIBUSB_ERROR_OTHER;
    }
    unit->claimed = true;
    ++unit->claims;
    return 0;
}

static int LIBUSB_CALL Fake_release_interface(libusb_device_handle *handle, int interface_number)
{
    FakeUnit *unit = UnitOfHandle(handle);

    if (!unit || !unit->open || !unit->claimed || interface_number != 1) {
        Fake_Unexpected("release");
        return LIBUSB_ERROR_OTHER;
    }
    unit->claimed = false;
    ++unit->releases;
    return 0;
}

/* libusb's Windows backend cannot detach a driver (libusb core.c:2280-2288) */
static int LIBUSB_CALL Fake_set_auto_detach_kernel_driver(libusb_device_handle *handle, int enable)
{
    if (!UnitOfHandle(handle) || !enable) {
        Fake_Unexpected("auto detach request");
    }
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

/* One bulk OUT payload and the reply it queues. The reply starts with the
   command, status 01, the transport, the subcommand, 10 78 00 00, as
   commands.md records for a controller. A Read Memory Block follows with
   0x40, three zero bytes, the address as asked and 0x40 bytes of flash,
   which read 0xFF where nothing was written. */
static void Fake_Command(FakeUnit *unit, const Uint8 *data, int length)
{
    Uint8 command, subcommand;

    if (length < 8) {
        Fake_Unexpected("command shorter than its header");
        return;
    }
    command = data[0];
    subcommand = data[3];
    if (unit->ncommands < FAKE_MAX_COMMANDS) {
        unit->commands[unit->ncommands] = (Uint16)((command << 8) | subcommand);
        unit->addresses[unit->ncommands] = 0;
    }

    SDL_zeroa(unit->reply);
    unit->reply[0] = command;
    unit->reply[1] = 0x01;
    unit->reply[2] = data[2];
    unit->reply[3] = subcommand;
    unit->reply[4] = 0x10;
    unit->reply[5] = 0x78;
    unit->reply_length = 8;
    unit->reply_offset = 0;

    if (command == 0x02 && subcommand == 0x01 && length >= 16) {
        const Uint32 address = (Uint32)data[12] | ((Uint32)data[13] << 8) | ((Uint32)data[14] << 16) | ((Uint32)data[15] << 24);

        if (unit->ncommands < FAKE_MAX_COMMANDS) {
            unit->addresses[unit->ncommands] = address;
        }
        unit->reply[8] = 0x40;
        SDL_memcpy(&unit->reply[12], &data[12], 4);
        SDL_memset(&unit->reply[16], 0xFF, 0x40);
        if (address == 0x13000) {
            const size_t serial_length = SDL_min(SDL_strlen(unit->unit.flash_serial), (size_t)16);

            unit->reply[16] = 0x01;
            unit->reply[17] = 0x00;
            SDL_memset(&unit->reply[18], 0, 16);
            SDL_memcpy(&unit->reply[18], unit->unit.flash_serial, serial_length);
        }
        unit->reply_length = FAKE_REPLY_SIZE;
    } else if (command == 0x03 && subcommand == 0x0D) {
        unit->started = true;
        unit->reply[8] = 0x01;
        unit->reply_length = 12;
    }
    ++unit->ncommands;
}

static int LIBUSB_CALL Fake_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint, unsigned char *data, int length, int *transferred, unsigned int timeout)
{
    FakeUnit *unit = UnitOfHandle(handle);

    (void)timeout;
    if (!unit || !unit->open || !unit->claimed || !data || length < 0 || !transferred) {
        Fake_Unexpected("bulk transfer on a device that is not claimed");
        return LIBUSB_ERROR_IO;
    }
    if (endpoint == 0x02) {
        ++unit->bulk_out;
        Fake_Command(unit, data, length);
        *transferred = length;
        return 0;
    }
    if (endpoint == 0x82) {
        int size = unit->reply_length - unit->reply_offset;

        ++unit->bulk_in;
        if (size <= 0) {
            *transferred = 0;
            return LIBUSB_ERROR_TIMEOUT;
        }
        /* One reply spans several reads, 64 bytes and then the rest */
        size = SDL_min(size, length);
        SDL_memcpy(data, &unit->reply[unit->reply_offset], (size_t)size);
        unit->reply_offset += size;
        *transferred = size;
        return 0;
    }
    Fake_Unexpected("bulk transfer on another endpoint");
    return LIBUSB_ERROR_NOT_FOUND;
}

static bool Fake_InitLibUSB(struct SDL_LibUSBContext **ctx)
{
    *ctx = &fake_libusb;
    ++fake.libusb_refs;
    return true;
}

static void Fake_QuitLibUSB(void)
{
    if (fake.libusb_refs < 1) {
        Fake_Unexpected("SDL_QuitLibUSB without SDL_InitLibUSB");
        return;
    }
    --fake.libusb_refs;
}

/* As hid_get_serial_number_string in the Windows HID backend, which copies
   the string it read when the device was opened */
static int Fake_hid_get_serial_number_string(SDL_hid_device *dev, wchar_t *string, size_t maxlen)
{
    ++fake.hid_serial_reads;
    if (dev != FakeHandle() || !string || !maxlen) {
        Fake_Unexpected("HID serial number read");
        return -1;
    }
    if (!fake.hid_serial) {
        return -1;
    }
    SDL_wcslcpy(string, fake.hid_serial, maxlen);
    return 0;
}

static int Fake_hid_read_timeout(SDL_hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
    (void)dev;
    (void)data;
    (void)length;
    (void)milliseconds;
    return 0;
}

/* As SDL_HidmaestroOwnsUsbSerial, which test/testhidmaestroserial.c covers:
   a persona with these IDs reports this serial on a HID interface the filter
   classifies. With the filter off nothing is classified. */
static int Fake_HidmaestroOwnsUsbSerial(unsigned short vendor_id, unsigned short product_id, const char *serial)
{
    int i;

    ++fake.hidmaestro_calls;
    if (!fake.filter) {
        return 0;
    }
    for (i = 0; i < fake.nunits; ++i) {
        const Unit *unit = &fake.units[i].unit;
        unsigned char ascii[128];

        if (!unit->persona || unit->vendor_id != vendor_id || unit->product_id != product_id || !unit->serial) {
            continue;
        }
        SerialToASCII(unit->serial, 0, ascii, sizeof(ascii));
        if (SDL_strcmp((const char *)ascii, serial) == 0) {
            return 1;
        }
    }
    return 0;
}

static bool Fake_LockRumble(void)
{
    return true;
}

static int Fake_SendRumbleAndUnlock(SDL_HIDAPI_Device *device, const Uint8 *data, int size)
{
    (void)device;
    (void)data;
    return size;
}

/* As HIDAPI_JoystickConnected, without SDL's joystick list */
static bool Fake_JoystickConnected(SDL_HIDAPI_Device *dev, SDL_JoystickID *pJoystickID)
{
    SDL_JoystickID *joysticks = (SDL_JoystickID *)SDL_realloc(dev->joysticks, (size_t)(dev->num_joysticks + 1) * sizeof(*joysticks));

    if (!joysticks) {
        return false;
    }
    dev->joysticks = joysticks;
    dev->joysticks[dev->num_joysticks] = (SDL_JoystickID)(2000 + dev->num_joysticks);
    if (pJoystickID) {
        *pJoystickID = dev->joysticks[dev->num_joysticks];
    }
    ++dev->num_joysticks;
    ++fake.connections;
    return true;
}

static void Fake_JoystickDisconnected(SDL_HIDAPI_Device *dev, SDL_JoystickID joystickID)
{
    (void)dev;
    (void)joystickID;
    Fake_Unexpected("joystick disconnection");
}

/* The units scenarios put on the bus */
#define REAL_SERIAL    L"00"
#define REAL_FLASH     "HEJ71001121247"
#define PERSONA_SERIAL L"HM0123456789AB"
#define PERSONA_FLASH  "HM0123456789AB"

static Unit RealController(Uint16 product_id)
{
    Unit unit;

    SDL_zero(unit);
    unit.vendor_id = USB_VENDOR_NINTENDO;
    unit.product_id = product_id;
    unit.serial_index = 3;
    unit.serial = REAL_SERIAL;
    unit.flash_serial = REAL_FLASH;
    return unit;
}

static Unit Persona(const wchar_t *serial, const char *flash_serial)
{
    Unit unit;

    SDL_zero(unit);
    unit.vendor_id = USB_VENDOR_NINTENDO;
    unit.product_id = PRODUCT_PRO;
    unit.serial_index = 3;
    unit.serial = serial;
    unit.flash_serial = flash_serial;
    unit.persona = true;
    return unit;
}

/* A unit with the controller's IDs and this serial string */
static Unit OtherUnit(const wchar_t *serial, const char *flash_serial)
{
    Unit unit = RealController(PRODUCT_PRO);

    unit.serial = serial;
    unit.flash_serial = flash_serial;
    return unit;
}

static void Bus(const Unit *units, int count)
{
    int i;

    SDL_zero(fake);
    fake.filter = true;
    fake.hid_serial = REAL_SERIAL;
    for (i = 0; i < count && i < FAKE_MAX_UNITS; ++i) {
        fake.units[i].unit = units[i];
    }
    fake.nunits = i;
}

/* The device record */
static SDL_HIDAPI_Device device;

/* As HIDAPI_AddDevice fills it from the HID enumeration */
static void AddDevice(Uint16 product_id, const char *serial)
{
    SDL_zero(device);
    device.dev = FakeHandle();
    device.name = SDL_strdup("Switch 2 Pro Controller");
    device.path = SDL_strdup("fake");
    device.vendor_id = USB_VENDOR_NINTENDO;
    device.product_id = product_id;
    device.serial = serial ? SDL_strdup(serial) : NULL;
    device.guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_USB, USB_VENDOR_NINTENDO, product_id, 0, NULL, device.name, 'h', 0);
}

/* As HIDAPI_CleanupDeviceDriver. The record and its serial stay. */
static void Stop(void)
{
    if (!device.driver) {
        return;
    }
    device.driver->FreeDevice(&device);
    device.driver = NULL;
    SDL_free(device.context);
    device.context = NULL;
    SDL_free(device.joysticks);
    device.joysticks = NULL;
    device.num_joysticks = 0;
}

/* As HIDAPI_SetupDeviceDriver: InitDevice under the joystick lock, and the
   cleanup when it fails. The counts restart so each start is read alone. */
static bool Start(void)
{
    bool result;
    int i;

    for (i = 0; i < fake.nunits; ++i) {
        const Unit unit = fake.units[i].unit;

        SDL_zero(fake.units[i]);
        fake.units[i].unit = unit;
    }
    fake.hid_serial_reads = 0;
    fake.hidmaestro_calls = 0;
    fake.connections = 0;
    SDL_ClearError();

    device.driver = &SDL_HIDAPI_DriverSwitch2;
    SDL_LockJoysticks();
    result = device.driver->InitDevice(&device);
    SDL_UnlockJoysticks();
    if (!result) {
        Stop();
    }
    return result;
}

static void RemoveDevice(void)
{
    Stop();
    SDL_free(device.serial);
    SDL_free(device.name);
    SDL_free(device.path);
    SDL_zero(device);
}

/* The unit took the driver's whole start: its interface claimed once, the
   serial block read first, and command 03/0D, which starts its input, last */
static void CheckStarted(int index, const char *what)
{
    const FakeUnit *unit = &fake.units[index];

    CHECK(unit->open && unit->claimed && unit->opens == 1 && unit->claims == 1,
          "%s: unit %d open %d claimed %d opens %d claims %d", what, index, unit->open, unit->claimed, unit->opens, unit->claims);
    CHECK(unit->started, "%s: unit %d never took the command that starts input", what, index);
    CHECK(unit->ncommands > 1 && unit->commands[0] == 0x0201 && unit->addresses[0] == 0x13000,
          "%s: unit %d's first command is %04x at %x", what, index, unit->commands[0], (unsigned int)unit->addresses[0]);
    CHECK(unit->ncommands > 1 && unit->ncommands <= FAKE_MAX_COMMANDS && unit->commands[unit->ncommands - 1] == 0x030D,
          "%s: unit %d took %d commands, the last not 03/0D", what, index, unit->ncommands);
    CHECK(unit->bulk_out == unit->ncommands && unit->bulk_in > 0, "%s: unit %d bulk OUT %d IN %d", what, index, unit->bulk_out, unit->bulk_in);
}

/* The unit took no command: never claimed, no bulk transfer, and closed */
static void CheckUntouched(int index, int opens, const char *what)
{
    const FakeUnit *unit = &fake.units[index];

    CHECK(unit->claims == 0 && unit->bulk_out == 0 && unit->bulk_in == 0 && !unit->started,
          "%s: unit %d claims %d bulk OUT %d IN %d started %d", what, index, unit->claims, unit->bulk_out, unit->bulk_in, unit->started);
    CHECK(unit->opens == opens && unit->closes == opens && !unit->open,
          "%s: unit %d opens %d closes %d, expected %d of each", what, index, unit->opens, unit->closes, opens);
}

/* After the driver is released nothing of libusb is left held */
static void CheckReleased(const char *what)
{
    int i;

    CHECK(fake.contexts == 0 && fake.lists == 0 && fake.configs == 0 && fake.libusb_refs == 0,
          "%s: contexts %d lists %d configs %d libusb references %d", what, fake.contexts, fake.lists, fake.configs, fake.libusb_refs);
    for (i = 0; i < fake.nunits; ++i) {
        const FakeUnit *unit = &fake.units[i];

        CHECK(!unit->open && !unit->claimed && unit->opens == unit->closes && unit->claims == unit->releases,
              "%s: unit %d open %d claimed %d opens %d closes %d claims %d releases %d", what, i,
              unit->open, unit->claimed, unit->opens, unit->closes, unit->claims, unit->releases);
    }
    CHECK(fake.unexpected == 0, "%s: %d requests no driver should make", what, fake.unexpected);
}

static bool SerialIs(const char *serial)
{
    return device.serial && SDL_strcmp(device.serial, serial) == 0;
}

/* 1. A controller alone, on its first start and on later ones */
static void TestControllerAlone(void)
{
    Unit units[3];
    int start;

    scenario = "a controller alone";

    /* Another vendor's device and a Joy-Con 2 share the bus and are never opened */
    units[0] = RealController(PRODUCT_PRO);
    units[0].vendor_id = 0x046D;
    units[1] = RealController(PRODUCT_JOYCON_R);
    units[2] = RealController(PRODUCT_PRO);
    Bus(units, 3);
    AddDevice(PRODUCT_PRO, "00");

    for (start = 1; start <= 3; ++start) {
        /* From the second start on, device->serial is the flash serial */
        CHECK(Start(), "start %d failed: %s", start, SDL_GetError());
        CheckStarted(2, "the controller");
        CheckUntouched(0, 0, "another vendor's device");
        CheckUntouched(1, 0, "another product");
        CHECK(fake.units[2].string_reads == 1, "start %d read the serial string %d times", start, fake.units[2].string_reads);
        CHECK(fake.hid_serial_reads == 1, "start %d read the HID serial %d times", start, fake.hid_serial_reads);
        CHECK(fake.hidmaestro_calls == 0, "start %d asked about HIDMaestro %d times", start, fake.hidmaestro_calls);
        CHECK(fake.connections == 1 && device.num_joysticks == 1, "start %d connected %d joysticks", start, fake.connections);
        CHECK(SerialIs(REAL_FLASH), "start %d left serial %s", start, device.serial ? device.serial : "(none)");
        Stop();
        CheckReleased("the controller alone");
    }
    RemoveDevice();

    /* The Joy-Con 2 takes its own product's unit */
    scenario = "a Joy-Con 2 beside a Pro Controller 2";
    Bus(units, 3);
    AddDevice(PRODUCT_JOYCON_R, "00");
    CHECK(Start(), "start failed: %s", SDL_GetError());
    CheckStarted(1, "the Joy-Con 2");
    CheckUntouched(0, 0, "another vendor's device");
    CheckUntouched(2, 0, "the Pro Controller 2");
    RemoveDevice();
    CheckReleased("the Joy-Con 2");
}

/* 2. A controller beside a HIDMaestro persona */
static void TestBesidePersona(void)
{
    int order, start;

    for (order = 0; order < 2; ++order) {
        Unit units[2];
        const int persona = order;
        const int controller = 1 - order;

        scenario = (order == 0) ? "a persona listed before the controller" : "a persona listed after the controller";
        units[persona] = Persona(PERSONA_SERIAL, PERSONA_FLASH);
        units[controller] = RealController(PRODUCT_PRO);
        Bus(units, 2);
        AddDevice(PRODUCT_PRO, "00");

        for (start = 1; start <= 2; ++start) {
            CHECK(Start(), "start %d failed: %s", start, SDL_GetError());
            CheckStarted(controller, "the controller");
            /* A persona listed first is opened for its serial string and
               closed. One listed after the match is never opened. */
            CheckUntouched(persona, (order == 0) ? 1 : 0, "the persona");
            CHECK(fake.units[persona].string_reads == ((order == 0) ? 1 : 0),
                  "start %d read the persona's serial %d times", start, fake.units[persona].string_reads);
            CHECK(fake.hidmaestro_calls == 0, "start %d asked about HIDMaestro %d times", start, fake.hidmaestro_calls);
            CHECK(SerialIs(REAL_FLASH), "start %d left serial %s", start, device.serial ? device.serial : "(none)");
            Stop();
            CheckReleased("beside a persona");
        }
        RemoveDevice();
    }
}

/* 3. Nothing opens in the controller's place */
static void TestNoSubstitute(void)
{
    Unit units[2];
    bool result;

    scenario = "the controller's USB device is not listed";
    units[0] = Persona(PERSONA_SERIAL, PERSONA_FLASH);
    Bus(units, 1);
    AddDevice(PRODUCT_PRO, "00");
    result = Start();
    CHECK(!result, "a persona opened in the controller's place");
    CHECK(SDL_strcmp(SDL_GetError(), "Couldn't open Switch 2 over libusb") == 0, "error: %s", SDL_GetError());
    CheckUntouched(0, 1, "the persona");
    CHECK(fake.connections == 0 && SerialIs("00"), "the failed start connected %d joysticks, serial %s", fake.connections, device.serial ? device.serial : "(none)");
    CheckReleased("no controller listed");
    RemoveDevice();

    /* Another application holds the controller, so libusb's open is denied */
    scenario = "the controller's USB device cannot be opened";
    units[0] = Persona(PERSONA_SERIAL, PERSONA_FLASH);
    units[1] = RealController(PRODUCT_PRO);
    units[1].open_error = LIBUSB_ERROR_ACCESS;
    Bus(units, 2);
    AddDevice(PRODUCT_PRO, "00");
    result = Start();
    CHECK(!result, "a persona opened in the controller's place");
    CheckUntouched(0, 1, "the persona");
    CheckUntouched(1, 0, "the controller");
    CheckReleased("the controller held elsewhere");
    RemoveDevice();

    /* A second controller whose string is not this one's */
    scenario = "only another unit is listed";
    units[0] = OtherUnit(L"01", "HEJ79999999999");
    Bus(units, 1);
    AddDevice(PRODUCT_PRO, "00");
    result = Start();
    CHECK(!result, "another unit opened in the controller's place");
    CheckUntouched(0, 1, "the other unit");
    CHECK(fake.hidmaestro_calls == 0, "asked about HIDMaestro %d times", fake.hidmaestro_calls);
    CheckReleased("another unit");
    RemoveDevice();

    scenario = "libusb lists no devices";
    units[0] = RealController(PRODUCT_PRO);
    Bus(units, 1);
    fake.list_error = LIBUSB_ERROR_NO_MEM;
    AddDevice(PRODUCT_PRO, "00");
    result = Start();
    CHECK(!result, "started without a device list");
    CheckUntouched(0, 0, "the controller");
    CheckReleased("no device list");
    RemoveDevice();
}

/* 4. The HID interface gives no serial to compare */
static void TestNoHIDSerial(void)
{
    static const wchar_t *const hid_serials[] = { NULL, L"" };
    int form, order;

    for (form = 0; form < 2; ++form) {
        for (order = 0; order < 2; ++order) {
            Unit units[2];
            const int persona = order;
            const int controller = 1 - order;

            scenario = (form == 0) ? "the HID serial read fails, beside a persona" : "the HID serial is empty, beside a persona";
            units[persona] = Persona(PERSONA_SERIAL, PERSONA_FLASH);
            units[controller] = RealController(PRODUCT_PRO);
            Bus(units, 2);
            fake.hid_serial = hid_serials[form];
            AddDevice(PRODUCT_PRO, NULL);
            CHECK(Start(), "order %d: start failed: %s", order, SDL_GetError());
            CheckStarted(controller, "the controller");
            CheckUntouched(persona, 1, "the persona");
            CHECK(fake.hidmaestro_calls == 2, "order %d: asked about HIDMaestro %d times", order, fake.hidmaestro_calls);
            CHECK(SerialIs(REAL_FLASH), "order %d left serial %s", order, device.serial ? device.serial : "(none)");
            RemoveDevice();
            CheckReleased("no HID serial, beside a persona");
        }

        scenario = (form == 0) ? "the HID serial read fails, a controller alone" : "the HID serial is empty, a controller alone";
        {
            Unit unit = RealController(PRODUCT_PRO);

            Bus(&unit, 1);
            fake.hid_serial = hid_serials[form];
            AddDevice(PRODUCT_PRO, NULL);
            CHECK(Start(), "the single-controller fallback failed: %s", SDL_GetError());
            CheckStarted(0, "the controller");
            CHECK(fake.hidmaestro_calls == 1, "asked about HIDMaestro %d times", fake.hidmaestro_calls);
            RemoveDevice();
            CheckReleased("no HID serial, a controller alone");
        }

        scenario = (form == 0) ? "the HID serial read fails, a persona alone" : "the HID serial is empty, a persona alone";
        {
            Unit unit = Persona(PERSONA_SERIAL, PERSONA_FLASH);
            bool result;

            Bus(&unit, 1);
            fake.hid_serial = hid_serials[form];
            AddDevice(PRODUCT_PRO, NULL);
            result = Start();
            CHECK(!result, "a persona opened as the fallback");
            CheckUntouched(0, 1, "the persona");
            CheckReleased("no HID serial, a persona alone");
            RemoveDevice();

            /* With nothing classified, as in HIDMaestro's unfiltered
               acceptance build, the same unit is the fallback */
            Bus(&unit, 1);
            fake.hid_serial = hid_serials[form];
            fake.filter = false;
            AddDevice(PRODUCT_PRO, NULL);
            CHECK(Start(), "the fallback failed with the filter off: %s", SDL_GetError());
            CheckStarted(0, "the persona, filter off");
            RemoveDevice();
            CheckReleased("no HID serial, filter off");
        }
    }
}

/* 5. A unit that cannot be compared */
static void TestUncomparable(void)
{
    Unit units[2];

    scenario = "a unit with no serial string before the controller";
    units[0] = RealController(PRODUCT_PRO);
    units[0].serial_index = 0;
    units[0].serial = NULL;
    units[1] = RealController(PRODUCT_PRO);
    Bus(units, 2);
    AddDevice(PRODUCT_PRO, "00");
    CHECK(Start(), "start failed: %s", SDL_GetError());
    CheckStarted(1, "the controller");
    CheckUntouched(0, 1, "the unit with no serial string");
    CHECK(fake.units[0].string_reads == 0, "read string 0 of the unit that declares none");
    RemoveDevice();
    CheckReleased("no serial string");

    scenario = "the only unit's serial string cannot be read";
    units[0] = RealController(PRODUCT_PRO);
    units[0].serial_error = LIBUSB_ERROR_PIPE;
    Bus(units, 1);
    AddDevice(PRODUCT_PRO, "00");
    CHECK(Start(), "the fallback failed: %s", SDL_GetError());
    CheckStarted(0, "the unit that cannot be compared");
    CHECK(fake.units[0].string_reads == 1 && fake.hidmaestro_calls == 0,
          "string reads %d, HIDMaestro asked %d times", fake.units[0].string_reads, fake.hidmaestro_calls);
    RemoveDevice();
    CheckReleased("unreadable serial string");

    scenario = "the only unit's serial string is empty";
    units[0] = OtherUnit(L"", REAL_FLASH);
    Bus(units, 1);
    AddDevice(PRODUCT_PRO, "00");
    CHECK(Start(), "the fallback failed: %s", SDL_GetError());
    CheckStarted(0, "the unit with an empty serial string");
    RemoveDevice();
    CheckReleased("empty serial string");

    /* The count libusb returns is above 0 and the string is still empty */
    scenario = "the only unit's serial string starts with a NUL";
    units[0] = OtherUnit(L"\0AB", REAL_FLASH);
    units[0].serial_length = 3;
    Bus(units, 1);
    AddDevice(PRODUCT_PRO, "00");
    CHECK(Start(), "the fallback failed: %s", SDL_GetError());
    CheckStarted(0, "the unit whose serial string starts with a NUL");
    RemoveDevice();
    CheckReleased("a serial string that starts with a NUL");

    scenario = "a persona before a unit that cannot be compared";
    units[0] = Persona(PERSONA_SERIAL, PERSONA_FLASH);
    units[1] = RealController(PRODUCT_PRO);
    units[1].serial_error = LIBUSB_ERROR_PIPE;
    Bus(units, 2);
    AddDevice(PRODUCT_PRO, "00");
    CHECK(Start(), "the fallback failed: %s", SDL_GetError());
    CheckStarted(1, "the unit that cannot be compared");
    CheckUntouched(0, 1, "the persona");
    RemoveDevice();
    CheckReleased("a persona and an unreadable unit");

    scenario = "two units that cannot be compared";
    units[0] = RealController(PRODUCT_PRO);
    units[0].serial_error = LIBUSB_ERROR_PIPE;
    units[1] = RealController(PRODUCT_PRO);
    units[1].serial_error = LIBUSB_ERROR_PIPE;
    Bus(units, 2);
    AddDevice(PRODUCT_PRO, "00");
    CHECK(Start(), "the fallback failed: %s", SDL_GetError());
    CheckStarted(0, "the first unit");
    CheckUntouched(1, 1, "the second unit");
    RemoveDevice();
    CheckReleased("two unreadable units");
}

/* 6. Two personas, nothing classified */
static void TestTwoPersonas(void)
{
    static const wchar_t *const serials[] = { L"HM00000000A001", L"HM00000000B002" };
    static const char *const flash[] = { "HM00000000A001", "HM00000000B002" };
    int own, start;

    for (own = 0; own < 2; ++own) {
        Unit units[2];

        scenario = (own == 0) ? "two personas, the first one's device" : "two personas, the second one's device";
        units[0] = Persona(serials[0], flash[0]);
        units[1] = Persona(serials[1], flash[1]);
        Bus(units, 2);
        fake.filter = false;
        fake.hid_serial = serials[own];
        AddDevice(PRODUCT_PRO, flash[own]);
        for (start = 1; start <= 2; ++start) {
            CHECK(Start(), "start %d failed: %s", start, SDL_GetError());
            CheckStarted(own, "its own persona");
            CheckUntouched(1 - own, (own == 1) ? 1 : 0, "the other persona");
            CHECK(SerialIs(flash[own]), "start %d left serial %s", start, device.serial ? device.serial : "(none)");
            CHECK(fake.hidmaestro_calls == 0, "start %d asked about HIDMaestro %d times", start, fake.hidmaestro_calls);
            Stop();
            CheckReleased("two personas");
        }
        RemoveDevice();
    }
}

/* 7. Serial strings libusb and the HID interface carry differently */
static void TestSerialForms(void)
{
    static wchar_t longest[2][127];
    Unit units[2];
    int i, own;

    /* E with an acute accent is one UTF-16 code unit, which libusb gives as '?' */
    for (own = 0; own < 2; ++own) {
        scenario = "a serial outside ASCII";
        units[0] = OtherUnit(L"S\x00C9" L"RIE-1", "FLASH-1");
        units[1] = OtherUnit(L"S\x00C9" L"RIE-2", "FLASH-2");
        Bus(units, 2);
        fake.hid_serial = units[own].serial;
        AddDevice(PRODUCT_PRO, NULL);
        CHECK(Start(), "unit %d: start failed: %s", own, SDL_GetError());
        CheckStarted(own, "its own unit");
        CheckUntouched(1 - own, (own == 1) ? 1 : 0, "the other unit");
        RemoveDevice();
        CheckReleased("a serial outside ASCII");
    }

    scenario = "a plain serial against one outside ASCII";
    units[0] = OtherUnit(L"S\x00C9" L"RIE-1", "FLASH-1");
    Bus(units, 1);
    fake.hid_serial = L"SERIE-1";
    AddDevice(PRODUCT_PRO, "SERIE-1");
    CHECK(!Start(), "a unit with another serial opened");
    CheckUntouched(0, 1, "the other unit");
    RemoveDevice();

    /* A string descriptor holds 126 code units. Two that differ in the last
       one are different units. */
    for (i = 0; i < 126; ++i) {
        longest[0][i] = (wchar_t)(L'A' + (i % 26));
        longest[1][i] = longest[0][i];
    }
    longest[1][125] = L'0';
    for (own = 0; own < 2; ++own) {
        scenario = "serials of 126 characters";
        units[0] = OtherUnit(longest[0], "FLASH-1");
        units[1] = OtherUnit(longest[1], "FLASH-2");
        Bus(units, 2);
        fake.hid_serial = longest[own];
        AddDevice(PRODUCT_PRO, NULL);
        CHECK(Start(), "unit %d: start failed: %s", own, SDL_GetError());
        CheckStarted(own, "its own unit");
        CheckUntouched(1 - own, (own == 1) ? 1 : 0, "the other unit");
        RemoveDevice();
        CheckReleased("serials of 126 characters");
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Only the functions the driver's start and release call. Any other
       entry stays NULL. */
    fake_libusb.init = Fake_init;
    fake_libusb.exit = Fake_exit;
    fake_libusb.get_device_list = Fake_get_device_list;
    fake_libusb.free_device_list = Fake_free_device_list;
    fake_libusb.get_device_descriptor = Fake_get_device_descriptor;
    fake_libusb.get_string_descriptor_ascii = Fake_get_string_descriptor_ascii;
    fake_libusb.get_config_descriptor = Fake_get_config_descriptor;
    fake_libusb.free_config_descriptor = Fake_free_config_descriptor;
    fake_libusb.open = Fake_open;
    fake_libusb.close = Fake_close;
    fake_libusb.get_device = Fake_get_device;
    fake_libusb.claim_interface = Fake_claim_interface;
    fake_libusb.release_interface = Fake_release_interface;
    fake_libusb.set_auto_detach_kernel_driver = Fake_set_auto_detach_kernel_driver;
    fake_libusb.bulk_transfer = Fake_bulk_transfer;

    TestControllerAlone();
    TestBesidePersona();
    TestNoSubstitute();
    TestNoHIDSerial();
    TestUncomparable();
    TestTwoPersonas();
    TestSerialForms();

    SDL_Quit();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
