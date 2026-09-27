/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Tests SDL's libusb HIDAPI backend, src/hidapi/libusb/hid.c, and the Intel
   Wireless Series and DJI RC drivers on it, against the fake
   libusb-1.0.dll of testlibusbfake.c. test/libusb-backend builds and runs
   it.

   testlibusbbackend <fake libusb-1.0.dll>

   The test loads the fake by its full path before SDL loads libusb by name,
   and Windows then hands SDL the module already loaded, so no real USB
   device is reached. The HIDAPI joystick backend is never started: each
   driver runs on a device record built here, the way
   HIDAPI_SetupDeviceDriver and HIDAPI_UpdateDevices run it.

   1. An open of the base station that fails after the claim is tried once,
      though interface 0 has two alternate settings on one path.
   2. The same open succeeds when SET_INTERFACE does, and the close selects
      alternate 0 again.
   3. The driver answers an activation on interrupt OUT 0x01 with a timeout
      of at most 10 ms, and a failed answer goes out again after the
      protocol's retry delay.
   4. Without the libusb handle, the driver answers through SDL_hid_write.
   5. While the DJI RC's bulk OUT 0x02 refuses data, the DJI RC driver's
      InitDevice and UpdateDevice return at once, the module goes on from
      the write's completion once the endpoint takes it, and a close waits
      for a write still running.
*/

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"
#include "core/windows/SDL_windows.h"
#include "joystick/hidapi/SDL_hidapijoystick_c.h"

#include <stdio.h>
#include <string.h>

#include "testlibusbfake.h"

/* libusb's error codes, as libusb.h numbers them */
#define TEST_LIBUSB_ERROR_IO            (-1)
#define TEST_LIBUSB_ERROR_NOT_SUPPORTED (-12)

#define TEST_UPDATE_LIMIT_MS 2000 /* The longest the driver gets to write */
#define TEST_CALL_LIMIT_MS   100  /* The longest one driver call may take while a write waits */

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
    FakeLibUSB_ResetFunc Reset;
    FakeLibUSB_SetAlternateResultFunc SetAlternateResult;
    FakeLibUSB_FailNextWriteFunc FailNextWrite;
    FakeLibUSB_HoldWritesFunc HoldWrites;
    FakeLibUSB_QueueInputFunc QueueInput;
    FakeLibUSB_GetCountsFunc GetCounts;
    FakeLibUSB_GetWriteFunc GetWrite;
    FakeLibUSB_GetUnexpectedFunc GetUnexpected;
} fake;

/* The JoypadOS capture (joypad-os docs/INTEL_WIRELESS_SERIES.md): a gamepad
   activating in slot 3, and the reply that activated it */
static const Uint8 activate_slot3[] = { 0x03, 0x03, 0x01, 0x02, 0xFF, 0x00, 0x00 };
static const Uint8 reply_slot3[] = { 0x03, 0x01, 0xFF, 0x00, 0x01, 0x63 };

/* The DJI RC module's 06/24 with payload 01 and its 06/01 poll, as
   testdjiremote.c has them */
static const Uint8 dji_simulator[] = { 0x55, 0x0E, 0x04, 0x66, 0x0A, 0x06, 0xEB, 0x34, 0x40, 0x06, 0x24, 0x01, 0xD9, 0xEC };
static const Uint8 dji_poll[] = { 0x55, 0x0D, 0x04, 0x33, 0x0A, 0x06, 0xEB, 0x34, 0x40, 0x06, 0x01, 0x74, 0x24 };

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
    fake.Reset = (FakeLibUSB_ResetFunc)GetProcAddress(module, "FakeLibUSB_Reset");
    fake.SetAlternateResult = (FakeLibUSB_SetAlternateResultFunc)GetProcAddress(module, "FakeLibUSB_SetAlternateResult");
    fake.FailNextWrite = (FakeLibUSB_FailNextWriteFunc)GetProcAddress(module, "FakeLibUSB_FailNextWrite");
    fake.HoldWrites = (FakeLibUSB_HoldWritesFunc)GetProcAddress(module, "FakeLibUSB_HoldWrites");
    fake.QueueInput = (FakeLibUSB_QueueInputFunc)GetProcAddress(module, "FakeLibUSB_QueueInput");
    fake.GetCounts = (FakeLibUSB_GetCountsFunc)GetProcAddress(module, "FakeLibUSB_GetCounts");
    fake.GetWrite = (FakeLibUSB_GetWriteFunc)GetProcAddress(module, "FakeLibUSB_GetWrite");
    fake.GetUnexpected = (FakeLibUSB_GetUnexpectedFunc)GetProcAddress(module, "FakeLibUSB_GetUnexpected");

    /* SDL_InitLibUSB loads "libusb-1.0.dll" by name, which must be this module */
    return fake.Reset && fake.SetAlternateResult && fake.FailNextWrite && fake.HoldWrites && fake.QueueInput &&
           fake.GetCounts && fake.GetWrite && fake.GetUnexpected &&
           GetModuleHandleW(L"libusb-1.0.dll") == module;
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

/* 1. libusb 1.0.29's HID backend refuses SET_INTERFACE to any alternate but
   0, so this open fails after the claim. Interface 0's two alternate
   settings share one path, and an attempt is one open, one claim, one
   SET_INTERFACE, one release and one close. */
static void TestFailedOpen(void)
{
    FakeLibUSB_Counts counts;
    SDL_hid_device *dev;

    fake.Reset();
    fake.SetAlternateResult(TEST_LIBUSB_ERROR_NOT_SUPPORTED);
    dev = SDL_hid_open_path(FAKE_LIBUSB_INTEL_PATH);
    fake.GetCounts(&counts);
    CHECK(dev == NULL, "the base station opened although SET_INTERFACE 1 failed");

    /* Positive control: the open reached SET_INTERFACE 1 on this path */
    CHECK(counts.opens >= 1 && counts.alternates >= 1 && counts.last_alternate == 1,
          "the open never asked for alternate 1: %d opens, %d SET_INTERFACE requests", counts.opens, counts.alternates);
    CHECK(counts.opens == 1 && counts.claims == 1 && counts.alternates == 1 && counts.releases == 1 && counts.closes == 1,
          "one failed open made %d opens, %d claims, %d SET_INTERFACE requests, %d releases and %d closes",
          counts.opens, counts.claims, counts.alternates, counts.releases, counts.closes);
    if (dev) {
        SDL_hid_close(dev);
    }
}

/* 2. The same open succeeds when SET_INTERFACE does, and hands its libusb
   handle to the drivers. The close selects alternate 0 again, releases the
   interface and closes the handle. */
static void TestOpenClose(void)
{
    FakeLibUSB_Counts counts;
    SDL_hid_device *dev;

    fake.Reset();
    dev = SDL_hid_open_path(FAKE_LIBUSB_INTEL_PATH);
    fake.GetCounts(&counts);
    CHECK(dev != NULL, "the base station did not open: %s", SDL_GetError());
    CHECK(counts.opens == 1 && counts.claims == 1 && counts.alternates == 1 && counts.last_alternate == 1 && counts.submits >= 1,
          "the open made %d opens, %d claims, %d SET_INTERFACE requests (the last for alternate %d) and %d submits",
          counts.opens, counts.claims, counts.alternates, counts.last_alternate, counts.submits);
    if (!dev) {
        return;
    }
    CHECK(SDL_GetPointerProperty(SDL_hid_get_properties(dev), SDL_PROP_HIDAPI_LIBUSB_DEVICE_HANDLE_POINTER, NULL) != NULL,
          "the open device has no libusb handle property");

    SDL_hid_close(dev);
    fake.GetCounts(&counts);
    CHECK(counts.alternates == 2 && counts.last_alternate == 0 && counts.releases == 1 && counts.closes == 1,
          "the close made %d SET_INTERFACE requests in all (the last for alternate %d), %d releases and %d closes",
          counts.alternates, counts.last_alternate, counts.releases, counts.closes);
}

/* The Intel Wireless Series driver on the fake base station */
static bool IntelOpen(SDL_HIDAPI_Device *device, bool libusb_handle)
{
    SDL_zerop(device);
    device->dev = SDL_hid_open_path(FAKE_LIBUSB_INTEL_PATH);
    if (!device->dev) {
        return false;
    }
    SDL_hid_set_nonblocking(device->dev, 1);
    if (!libusb_handle) {
        SDL_ClearProperty(SDL_hid_get_properties(device->dev), SDL_PROP_HIDAPI_LIBUSB_DEVICE_HANDLE_POINTER);
    }
    device->name = SDL_strdup("Intel Wireless Series");
    device->path = SDL_strdup(FAKE_LIBUSB_INTEL_PATH);
    device->vendor_id = USB_VENDOR_INTEL;
    device->product_id = USB_PRODUCT_INTEL_WIRELESS_SERIES;
    device->interface_number = 0;
    device->driver = &SDL_HIDAPI_DriverIntelWireless;
    return device->driver->InitDevice(device);
}

/* Updates the driver until the fake holds this many writes or the limit
   passes, and returns the count */
static int IntelUpdateUntil(SDL_HIDAPI_Device *device, int writes)
{
    const Uint64 start = SDL_GetTicks();
    FakeLibUSB_Counts counts;

    for (;;) {
        device->driver->UpdateDevice(device);
        fake.GetCounts(&counts);
        if (counts.writes >= writes || SDL_GetTicks() - start > TEST_UPDATE_LIMIT_MS) {
            return counts.writes;
        }
        SDL_Delay(1);
    }
}

/* In the order of HIDAPI_CleanupDeviceDriver */
static void DriverClose(SDL_HIDAPI_Device *device)
{
    if (device->driver) {
        device->driver->FreeDevice(device);
    }
    if (device->dev) {
        SDL_hid_close(device->dev);
    }
    SDL_free(device->context);
    SDL_free(device->name);
    SDL_free(device->path);
    SDL_zerop(device);
}

static bool IsReply(const FakeLibUSB_Write *write)
{
    return write->endpoint == 0x01 && write->length == (int)sizeof(reply_slot3) &&
           SDL_memcmp(write->data, reply_slot3, sizeof(reply_slot3)) == 0;
}

/* 3. The driver's updates run under SDL's joystick lock, so the reply may
   wait at most 10 ms, ten intervals of interrupt OUT 0x01. A failed reply
   goes out again after the protocol's 100 ms retry delay. */
static void TestActivationReply(void)
{
    SDL_HIDAPI_Device device;
    FakeLibUSB_Write first, failed, retry;
    int writes;

    fake.Reset();
    CHECK(IntelOpen(&device, true), "the driver did not start: %s", SDL_GetError());
    if (!device.context) {
        DriverClose(&device);
        return;
    }
    CHECK(fake.QueueInput(activate_slot3, sizeof(activate_slot3)), "the fake did not queue the activation");
    writes = IntelUpdateUntil(&device, 1);
    CHECK(writes == 1, "%d replies to one activation", writes);
    if (fake.GetWrite(0, &first)) {
        /* Positive control: the reply of the capture, on the pads' OUT endpoint */
        CHECK(IsReply(&first), "the reply went to endpoint 0x%02X with %d bytes, first byte 0x%02X",
              first.endpoint, first.length, first.data[0]);
        CHECK(first.timeout <= 10, "the activation reply may wait %u ms for the endpoint", first.timeout);
    }

    /* A fresh activation of slot 3, whose reply fails once */
    fake.FailNextWrite(TEST_LIBUSB_ERROR_IO);
    CHECK(fake.QueueInput(activate_slot3, sizeof(activate_slot3)), "the fake did not queue the second activation");
    writes = IntelUpdateUntil(&device, 3);
    CHECK(writes == 3, "%d writes where the first reply, a failed one and its retry make 3", writes);
    if (fake.GetWrite(1, &failed) && fake.GetWrite(2, &retry)) {
        CHECK(IsReply(&failed) && failed.result == TEST_LIBUSB_ERROR_IO, "the second write was not the failed reply");
        CHECK(IsReply(&retry) && retry.result == 0 && retry.timeout <= 10,
              "the retry was not the reply with a timeout of at most 10 ms, but %u ms", retry.timeout);
        CHECK(retry.ms - failed.ms >= 99.0, "the reply went out again %.1f ms after the failed write", retry.ms - failed.ms);
    }
    DriverClose(&device);
}

/* 4. A device another backend opened has no libusb handle, and the driver
   answers through SDL_hid_write, here the libusb backend's hid_write with
   its own 1000 ms. */
static void TestReplyWithoutHandle(void)
{
    SDL_HIDAPI_Device device;
    FakeLibUSB_Write write;
    int writes;

    fake.Reset();
    CHECK(IntelOpen(&device, false), "the driver did not start: %s", SDL_GetError());
    if (!device.context) {
        DriverClose(&device);
        return;
    }
    CHECK(fake.QueueInput(activate_slot3, sizeof(activate_slot3)), "the fake did not queue the activation");
    writes = IntelUpdateUntil(&device, 1);
    CHECK(writes == 1, "%d replies to one activation", writes);
    if (fake.GetWrite(0, &write)) {
        CHECK(IsReply(&write) && write.timeout == 1000,
              "the reply through SDL_hid_write went to endpoint 0x%02X with %d bytes and a timeout of %u ms",
              write.endpoint, write.length, write.timeout);
    }
    DriverClose(&device);
}

/* The performance counter in milliseconds, the clock of the fake's records */
static double NowMs(void)
{
    LARGE_INTEGER counter, frequency;

    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

/* The DJI RC driver on the fake remote's DUML interface. The test runs
   InitDevice itself, to time it. */
static bool DJIOpen(SDL_HIDAPI_Device *device)
{
    SDL_zerop(device);
    device->dev = SDL_hid_open_path(FAKE_LIBUSB_DJI_PATH);
    if (!device->dev) {
        return false;
    }
    SDL_hid_set_nonblocking(device->dev, 1);
    device->name = SDL_strdup("DJI RC (RM330)");
    device->path = SDL_strdup(FAKE_LIBUSB_DJI_PATH);
    device->vendor_id = USB_VENDOR_DJI;
    device->product_id = USB_PRODUCT_DJI_RC_RM330;
    device->interface_number = 1;
    device->driver = &SDL_HIDAPI_DriverDJIRemote;
    return true;
}

/* Updates the driver every millisecond until the fake has seen at least
   writes OUT transfers with running of them running, any number when
   running is negative, or until limit_ms passes. Returns the longest
   update. */
static double DJIUpdateUntil(SDL_HIDAPI_Device *device, int writes, int running, double limit_ms)
{
    const double start = NowMs();
    double longest = 0.0;

    for (;;) {
        FakeLibUSB_Counts counts;
        const double before = NowMs();
        double took;

        device->driver->UpdateDevice(device);
        took = NowMs() - before;
        if (took > longest) {
            longest = took;
        }
        fake.GetCounts(&counts);
        if ((counts.writes >= writes && (running < 0 || counts.writing == running)) || NowMs() - start > limit_ms) {
            return longest;
        }
        SDL_Delay(1);
    }
}

static bool IsFrame(const FakeLibUSB_Write *write, const Uint8 *frame, int length)
{
    return write->endpoint == 0x02 && write->length == length && SDL_memcmp(write->data, frame, (size_t)length) == 0;
}

/* 5. The DJI RC's updates run under SDL's joystick lock, and its writes are
   bulk transfers that SDL_hid_write lets wait up to 1000 ms. While OUT 0x02
   refuses data, InitDevice and every UpdateDevice return at once and the
   write waits on the driver's own thread. Once the endpoint takes the
   06/24, an update hands the module the completion, and the 06/01 poll
   follows it by the 100 ms settle. A close waits for a write still
   running, so the handle closes after it. */
static void TestDJIWrites(void)
{
    SDL_HIDAPI_Device device;
    FakeLibUSB_Counts counts;
    FakeLibUSB_Write simulator, poll;
    double start, took;
    bool started;

    fake.Reset();
    fake.HoldWrites(1);
    if (!DJIOpen(&device)) {
        CHECK(false, "the remote did not open: %s", SDL_GetError());
        fake.HoldWrites(0);
        DriverClose(&device);
        return;
    }
    start = NowMs();
    started = device.driver->InitDevice(&device);
    took = NowMs() - start;
    CHECK(started, "the driver did not start: %s", SDL_GetError());
    if (!started) {
        fake.HoldWrites(0);
        DriverClose(&device);
        return;
    }
    CHECK(took < TEST_CALL_LIMIT_MS, "InitDevice took %.0f ms while OUT 0x02 refused data", took);

    /* 150 ms of updates while the 06/24 waits */
    took = DJIUpdateUntil(&device, SDL_MAX_SINT32, -1, 150.0);
    CHECK(took < TEST_CALL_LIMIT_MS, "an UpdateDevice took %.0f ms while OUT 0x02 refused data", took);
    /* Positive control: the 06/24 reached OUT 0x02 and still waits there,
       and nothing else went out */
    fake.GetCounts(&counts);
    CHECK(counts.writes == 1 && counts.writing == 1 && counts.held == 1,
          "%d writes made, %d running and %d held while the 06/24 waited", counts.writes, counts.writing, counts.held);
    CHECK(fake.GetWrite(0, &simulator) && IsFrame(&simulator, dji_simulator, (int)sizeof(dji_simulator)),
          "the first write was not the 06/24 on OUT 0x02");

    /* The endpoint takes it, and the poll follows the settle */
    fake.HoldWrites(0);
    DJIUpdateUntil(&device, 2, -1, TEST_UPDATE_LIMIT_MS);
    if (fake.GetWrite(0, &simulator) && fake.GetWrite(1, &poll)) {
        CHECK(simulator.result == 0, "the 06/24 returned %d once the endpoint took it", simulator.result);
        CHECK(IsFrame(&poll, dji_poll, (int)sizeof(dji_poll)), "the second write was not the 06/01 poll on OUT 0x02");
        CHECK(poll.ms - simulator.done_ms >= 99.0, "the poll went out %.1f ms after the 06/24 finished", poll.ms - simulator.done_ms);
    } else {
        CHECK(false, "no poll followed the 06/24");
    }

    /* A close while a poll waits */
    fake.HoldWrites(1);
    DJIUpdateUntil(&device, 3, 1, TEST_UPDATE_LIMIT_MS);
    fake.GetCounts(&counts);
    CHECK(counts.writing == 1, "no poll was running when the close began");
    DriverClose(&device);
    fake.GetCounts(&counts);
    CHECK(counts.writing == 0 && counts.writing_closes == 0,
          "the handle closed with a write running %d times, and %d still run", counts.writing_closes, counts.writing);
    fake.HoldWrites(0);
}

int main(int argc, char *argv[])
{
    /* Unbuffered, so a crash still leaves every failed check on the screen */
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2) {
        printf("Usage: %s <fake libusb-1.0.dll>\n", argv[0]);
        return 2;
    }

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

    TestFailedOpen();
    TestOpenClose();
    TestActivationReply();
    TestReplyWithoutHandle();
    TestDJIWrites();
    CHECK(fake.GetUnexpected() == 0, "SDL made %d libusb calls the fake does not serve", fake.GetUnexpected());

    SDL_hid_exit();
    SDL_Quit();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
