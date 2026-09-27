/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Tests the Namco USIO and Tacx drivers on SDL's libusb HIDAPI backend,
   src/hidapi/libusb/hid.c, against the fake libusb-1.0.dll of
   testlibusbspecialtyfake.c. test/libusb-backend builds and runs it.

   testlibusbspecialty <fake libusb-1.0.dll>

   The test loads the fake by its full path before SDL loads libusb by name,
   and Windows then hands SDL the module already loaded, so no real USB
   device is reached. The HIDAPI joystick backend is never started: each
   driver runs on a device record built here, the way
   HIDAPI_SetupDeviceDriver and HIDAPI_UpdateDevices run it.

   1. The USIO driver's update returns at once while its command waits on an
      OUT endpoint that NAKs, and the close waits for that write.
   2. The same for the Tacx driver's version request.
   3. Tacx replies of 48 bytes in 16-byte packets, which end no 64-byte bulk
      transfer, come back one reply a read.
*/

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"
#include "core/windows/SDL_windows.h"
#include "joystick/hidapi/SDL_hidapijoystick_c.h"
#include "joystick/hidapi/SDL_hidapi_rumble.h"

#include <stdio.h>
#include <string.h>

#include "testlibusbspecialtyfake.h"

/* libusb's error code, as libusb.h numbers it */
#define TEST_LIBUSB_ERROR_TIMEOUT (-7)

#define TEST_UPDATE_LIMIT_MS  2000 /* The longest the driver gets to write */
#define TEST_WRITE_TIMEOUT_MS 1000 /* hid_write's timeout, which a NAKed write waits out */
/* An update that waited on a NAKed write takes the write's whole timeout. One
   that did not returns in far less than this. */
#define TEST_UPDATE_BLOCKED_MS 250

static int checks;
static int failures;

#define CHECK(condition, ...)                         \
    do {                                              \
        ++checks;                                     \
        if (!(condition)) {                           \
            ++failures;                               \
            printf("FAILED line %d: ", __LINE__);     \
            printf(__VA_ARGS__);                      \
            printf("\n");                             \
        }                                             \
    } while (0)

static struct
{
    FakeSpecialty_ResetFunc Reset;
    FakeSpecialty_SelectDeviceFunc SelectDevice;
    FakeSpecialty_NakWritesFunc NakWrites;
    FakeSpecialty_QueueReplyFunc QueueReply;
    FakeSpecialty_GetCountsFunc GetCounts;
    FakeSpecialty_GetWriteFunc GetWrite;
    FakeSpecialty_GetUnexpectedFunc GetUnexpected;
} fake;

/* The USIO's identification read, register 1800 for 0x180 bytes */
static const Uint8 usio_ident_read[] = { 0x10, 0xE0, 0x00, 0x18, 0x80, 0x01 };

/* The Tacx's version request, and its stop frame: 01 08 01 00 and eight
   bytes of 00 */
static const Uint8 tacx_version_request[] = { 0x02, 0x00, 0x00, 0x00 };
static const Uint8 tacx_stop_frame[] = { 0x01, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static bool LoadFake(const char *path)
{
    WCHAR wide[MAX_PATH];
    HMODULE module;

    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH)) {
        return false;
    }
    module = LoadLibraryW(wide);
    if (!module) {
        return false;
    }
    fake.Reset = (FakeSpecialty_ResetFunc)GetProcAddress(module, "FakeSpecialty_Reset");
    fake.SelectDevice = (FakeSpecialty_SelectDeviceFunc)GetProcAddress(module, "FakeSpecialty_SelectDevice");
    fake.NakWrites = (FakeSpecialty_NakWritesFunc)GetProcAddress(module, "FakeSpecialty_NakWrites");
    fake.QueueReply = (FakeSpecialty_QueueReplyFunc)GetProcAddress(module, "FakeSpecialty_QueueReply");
    fake.GetCounts = (FakeSpecialty_GetCountsFunc)GetProcAddress(module, "FakeSpecialty_GetCounts");
    fake.GetWrite = (FakeSpecialty_GetWriteFunc)GetProcAddress(module, "FakeSpecialty_GetWrite");
    fake.GetUnexpected = (FakeSpecialty_GetUnexpectedFunc)GetProcAddress(module, "FakeSpecialty_GetUnexpected");

    /* SDL_InitLibUSB loads "libusb-1.0.dll" by name, which must be this module */
    return fake.Reset && fake.SelectDevice && fake.NakWrites && fake.QueueReply && fake.GetCounts &&
           fake.GetWrite && fake.GetUnexpected && GetModuleHandleW(L"libusb-1.0.dll") == module;
}

static void ClearSDLEnvironment(void)
{
    SDL_Environment *environment = SDL_GetEnvironment();
    char **variables = SDL_GetEnvironmentVariables(environment);
    int i;

    if (!variables) {
        return;
    }
    for (i = 0; variables[i]; ++i) {
        if (SDL_strncmp(variables[i], "SDL_", 4) == 0) {
            char *equals = SDL_strchr(variables[i], '=');
            if (equals) {
                *equals = '\0';
            }
            SDL_UnsetEnvironmentVariable(environment, variables[i]);
        }
    }
    SDL_free(variables);
}

/* A driver on the device the fake serves, started as
   HIDAPI_SetupDeviceDriver starts it */
static bool DriverOpen(SDL_HIDAPI_Device *device, SDL_HIDAPI_DeviceDriver *driver, const char *name,
                       Uint16 vendor_id, Uint16 product_id)
{
    SDL_zerop(device);
    device->dev = SDL_hid_open_path(FAKE_SPECIALTY_PATH);
    if (!device->dev) {
        return false;
    }
    SDL_hid_set_nonblocking(device->dev, 1);
    device->name = SDL_strdup(name);
    device->path = SDL_strdup(FAKE_SPECIALTY_PATH);
    device->vendor_id = vendor_id;
    device->product_id = product_id;
    device->interface_number = 0;
    device->driver = driver;
    return device->driver->InitDevice(device);
}

/* In the order of HIDAPI_CleanupDeviceDriver, then HIDAPI_DelDevice's wait
   for the rumble thread, which can still hold a request for this record */
static void DriverClose(SDL_HIDAPI_Device *device)
{
    if (device->driver) {
        device->driver->FreeDevice(device);
    }
    if (device->dev) {
        SDL_hid_close(device->dev);
        device->dev = NULL;
    }
    while (SDL_GetAtomicInt(&device->rumble_pending) > 0) {
        SDL_Delay(10);
    }
    SDL_free(device->context);
    SDL_free(device->name);
    SDL_free(device->path);
    SDL_zerop(device);
}

/* The milliseconds one update of the driver takes */
static Uint64 TimedUpdate(SDL_HIDAPI_Device *device)
{
    const Uint64 start = SDL_GetTicks();

    device->driver->UpdateDevice(device);
    return SDL_GetTicks() - start;
}

/* Updates the driver until the fake holds this many writes or the limit
   passes, and returns the count */
static int UpdateUntil(SDL_HIDAPI_Device *device, int writes)
{
    const Uint64 start = SDL_GetTicks();
    FakeSpecialty_Counts counts;

    for (;;) {
        device->driver->UpdateDevice(device);
        fake.GetCounts(&counts);
        if (counts.writes >= writes || SDL_GetTicks() - start > TEST_UPDATE_LIMIT_MS) {
            return counts.writes;
        }
        SDL_Delay(1);
    }
}

static bool IsTacxFrame(const FakeSpecialty_Write *write)
{
    return (write->length == (int)sizeof(tacx_version_request) &&
            SDL_memcmp(write->data, tacx_version_request, sizeof(tacx_version_request)) == 0) ||
           (write->length == (int)sizeof(tacx_stop_frame) &&
            SDL_memcmp(write->data, tacx_stop_frame, sizeof(tacx_stop_frame)) == 0);
}

/* 1. The driver's updates run with SDL's joystick lock held, and a write to
   an OUT endpoint that NAKs every packet blocks for its 1000 ms timeout.
   The update that hands out the USIO's first command returns at once, the
   command still reaches bulk OUT 0x01, and the read goes out again once
   that write has failed. The close comes while the second try is blocked
   too, and waits for that write before the handle closes. */
static void TestUSIOWrite(void)
{
    SDL_HIDAPI_Device device;
    FakeSpecialty_Counts counts;
    FakeSpecialty_Write first, again;
    Uint64 elapsed;
    int writes;

    fake.Reset();
    fake.SelectDevice(FAKE_SPECIALTY_USIO);
    CHECK(DriverOpen(&device, &SDL_HIDAPI_DriverUSIO, "Namco USIO", USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_USIO),
          "the USIO driver did not start: %s", SDL_GetError());
    if (!device.context) {
        DriverClose(&device);
        return;
    }
    fake.NakWrites(2);
    elapsed = TimedUpdate(&device);
    CHECK(elapsed < TEST_UPDATE_BLOCKED_MS, "the update that handed out the first command took %u ms while OUT 0x01 NAKed",
          (unsigned int)elapsed);
    writes = UpdateUntil(&device, 2);
    CHECK(writes == 2, "%d writes where the NAKed identification read and its second try make 2", writes);
    if (fake.GetWrite(0, &first) && fake.GetWrite(1, &again)) {
        /* Positive control: the read reached the USIO's OUT endpoint, and
           the fake NAKed it for hid_write's whole timeout */
        CHECK(first.endpoint == 0x01 && first.length == (int)sizeof(usio_ident_read) &&
              SDL_memcmp(first.data, usio_ident_read, sizeof(usio_ident_read)) == 0 &&
              first.timeout == TEST_WRITE_TIMEOUT_MS && first.result == TEST_LIBUSB_ERROR_TIMEOUT,
              "the first write went to endpoint 0x%02X with %d bytes, a timeout of %u ms and result %d",
              first.endpoint, first.length, first.timeout, first.result);
        CHECK(again.endpoint == 0x01 && again.length == (int)sizeof(usio_ident_read) &&
              SDL_memcmp(again.data, usio_ident_read, sizeof(usio_ident_read)) == 0 &&
              again.result == TEST_LIBUSB_ERROR_TIMEOUT && again.ms - first.ms >= TEST_WRITE_TIMEOUT_MS * 0.9,
              "the second try went out %.1f ms after the first, with result %d", again.ms - first.ms, again.result);
    }
    DriverClose(&device);
    fake.GetCounts(&counts);
    CHECK(counts.closes == 1 && counts.closes_during_writes == 0,
          "%d of %d closes came while a write ran", counts.closes_during_writes, counts.closes);
    CHECK(fake.GetUnexpected() == 0, "SDL made %d libusb calls the fake does not serve", fake.GetUnexpected());
}

/* 2. The Tacx writes a frame every 100 ms from its updates. The update that
   hands out the version request returns at once while OUT 0x02 NAKs, the
   request still reaches bulk OUT 0x02, and the frames go on once that write
   has failed. The close comes while the next frame is blocked too, and
   waits for that write before the handle closes. */
static void TestTacxWrite(void)
{
    SDL_HIDAPI_Device device;
    FakeSpecialty_Counts counts;
    FakeSpecialty_Write first, next;
    Uint64 elapsed;
    int writes;

    fake.Reset();
    fake.SelectDevice(FAKE_SPECIALTY_TACX);
    CHECK(DriverOpen(&device, &SDL_HIDAPI_DriverTacx, "Tacx T1904", USB_VENDOR_TACX, USB_PRODUCT_TACX_T1904),
          "the Tacx driver did not start: %s", SDL_GetError());
    if (!device.context) {
        DriverClose(&device);
        return;
    }
    fake.NakWrites(2);
    elapsed = TimedUpdate(&device);
    CHECK(elapsed < TEST_UPDATE_BLOCKED_MS, "the update that handed out the version request took %u ms while OUT 0x02 NAKed",
          (unsigned int)elapsed);
    writes = UpdateUntil(&device, 2);
    CHECK(writes >= 2, "%d writes where the NAKed version request and a later frame make 2", writes);
    if (fake.GetWrite(0, &first) && fake.GetWrite(1, &next)) {
        /* Positive control: the request reached the head unit's OUT
           endpoint, and the fake NAKed it for hid_write's whole timeout */
        CHECK(first.endpoint == 0x02 && first.length == (int)sizeof(tacx_version_request) &&
              SDL_memcmp(first.data, tacx_version_request, sizeof(tacx_version_request)) == 0 &&
              first.timeout == TEST_WRITE_TIMEOUT_MS && first.result == TEST_LIBUSB_ERROR_TIMEOUT,
              "the first write went to endpoint 0x%02X with %d bytes, a timeout of %u ms and result %d",
              first.endpoint, first.length, first.timeout, first.result);
        CHECK(next.endpoint == 0x02 && IsTacxFrame(&next) && next.result == TEST_LIBUSB_ERROR_TIMEOUT &&
              next.ms - first.ms >= TEST_WRITE_TIMEOUT_MS * 0.9,
              "the next write, %d bytes from 0x%02X, went out %.1f ms after the first with result %d",
              next.length, next.data[0], next.ms - first.ms, next.result);
    }
    DriverClose(&device);
    fake.GetCounts(&counts);
    CHECK(counts.closes == 1 && counts.closes_during_writes == 0,
          "%d of %d closes came while a write ran", counts.closes_during_writes, counts.closes);
    CHECK(fake.GetUnexpected() == 0, "SDL made %d libusb calls the fake does not serve", fake.GetUnexpected());
}

/* 3. A head unit's reply ends a 64-byte bulk transfer only when it fills
   it or ends with a short packet, and a 48-byte reply in 16-byte packets
   does neither. The Tacx rule ends each IN transfer 50 ms after it starts,
   and the backend hands over the bytes a timed-out transfer holds, so each
   such reply is one read. A 64-byte reply fills the transfer, the positive
   control. The USIO, whose rule sets no timeout, keeps the backend's
   5000 ms. */
static void TestTacxReplies(void)
{
    Uint8 reply64[64], first[48], second[48], buffer[64];
    FakeSpecialty_Counts counts;
    SDL_hid_device *dev;
    int i, size;

    for (i = 0; i < 64; ++i) {
        reply64[i] = (Uint8)(0x40 + i);
    }
    for (i = 0; i < 48; ++i) {
        first[i] = (Uint8)(0x80 + i);
        second[i] = (Uint8)(0xC0 + i);
    }

    fake.Reset();
    fake.SelectDevice(FAKE_SPECIALTY_USIO);
    dev = SDL_hid_open_path(FAKE_SPECIALTY_PATH);
    fake.GetCounts(&counts);
    CHECK(dev != NULL && counts.in_timeout == 5000, "the USIO's IN transfers time out after %d ms", counts.in_timeout);
    if (dev) {
        SDL_hid_close(dev);
    }

    fake.Reset();
    fake.SelectDevice(FAKE_SPECIALTY_TACX);
    dev = SDL_hid_open_path(FAKE_SPECIALTY_PATH);
    CHECK(dev != NULL, "the head unit did not open: %s", SDL_GetError());
    if (!dev) {
        return;
    }
    fake.GetCounts(&counts);
    CHECK(counts.in_timeout == 50, "the head unit's IN transfers time out after %d ms", counts.in_timeout);

    CHECK(fake.QueueReply(reply64, sizeof(reply64)), "the fake did not queue the 64-byte reply");
    size = SDL_hid_read_timeout(dev, buffer, sizeof(buffer), 1000);
    CHECK(size == 64 && SDL_memcmp(buffer, reply64, sizeof(reply64)) == 0, "a 64-byte reply came back as %d bytes", size);

    CHECK(fake.QueueReply(first, sizeof(first)), "the fake did not queue the first 48-byte reply");
    size = SDL_hid_read_timeout(dev, buffer, sizeof(buffer), 1000);
    CHECK(size == 48 && SDL_memcmp(buffer, first, sizeof(first)) == 0, "the first 48-byte reply came back as %d bytes", size);

    CHECK(fake.QueueReply(second, sizeof(second)), "the fake did not queue the second 48-byte reply");
    size = SDL_hid_read_timeout(dev, buffer, sizeof(buffer), 1000);
    CHECK(size == 48 && SDL_memcmp(buffer, second, sizeof(second)) == 0,
          "the second 48-byte reply came back as %d bytes, the first of them 0x%02X", size, (size > 0) ? buffer[0] : 0);
    SDL_hid_close(dev);
    CHECK(fake.GetUnexpected() == 0, "SDL made %d libusb calls the fake does not serve", fake.GetUnexpected());
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("Usage: %s <fake libusb-1.0.dll>\n", argv[0]);
        return 2;
    }
    /* A crash after a failed check still leaves the check's line */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* No error dialog from a failed load, only the result */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    if (!LoadFake(argv[1])) {
        printf("%s is not the fake libusb this test needs. Nothing ran.\n", argv[1]);
        return 1;
    }

    ClearSDLEnvironment();
    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (SDL_hid_init() != 0) {
        printf("SDL_hid_init failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    TestUSIOWrite();
    TestTacxWrite();
    TestTacxReplies();
    /* HIDAPI_JoystickQuit stops the rumble thread the same way */
    SDL_HIDAPI_QuitRumble();

    SDL_hid_exit();
    SDL_Quit();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
