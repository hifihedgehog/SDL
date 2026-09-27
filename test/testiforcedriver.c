/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Tests the I-Force drivers of hifihedgehog/SDL#33 Part 8 on the static SDL
   of test/libusb-backend, which builds and runs it:

   testiforcedriver <fake libusb-1.0.dll>

   The joystick driver runs on the fake's libusb handle with an I-Force
   device record, the way testlibusbbackend.c runs the Intel driver.
   InitDevice reads only the record's IDs and the handle, and every query is
   a control transfer the fake answers as each case sets it. The haptic
   driver runs on a joystick record built here, whose effects this test
   records, under SDL's real joystick lock. The HIDAPI joystick backend is
   never started, and no real USB device is reached.

   1. Through Windows' HID driver, libusb refuses GET_STATUS and every vendor
      request with LIBUSB_ERROR_NOT_SUPPORTED. The driver declines such a
      device before any query. Through WinUSB, which passes GET_STATUS to the
      device, the same device is taken and asked O.
   2. Opening the joystick publishes the effect count and the memory end
      once N is answered, while C is still unanswered.
   3. An M that stalls does not end the open's wait for N.
   4. An effect update whose send fails reports the failure, and the same
      update sent again goes out.
   5. A waiting update whose send fails on the haptic thread goes out on a
      later try.
   6. An effect update under the joystick lock, as from an event watcher,
      returns while a waiting update is due. A waiting update that falls due
      under the lock goes out once the lock is free.
   7. Closing the haptic device under the joystick lock returns while a
      waiting update is due.
   8. On an OUT endpoint that refuses data, the haptic thread tries a
      waiting update three times, each for the write's timeout, and then
      drops it. The sends go through the I-Force joystick driver to the fake.
      A later update from the application goes out.
*/

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"
#include "core/windows/SDL_windows.h"
#include "joystick/hidapi/SDL_hidapijoystick_c.h"
#include "haptic/hidapi/SDL_hidapihaptic_c.h"
#include "joystick/SDL_iforce_proto.h"

#include <stdio.h>
#include <string.h>

#include "testlibusbfake.h"

/* libusb's error codes and the standard GET_STATUS request, as libusb.h
   numbers them */
#define TEST_LIBUSB_ERROR_TIMEOUT       (-7)
#define TEST_LIBUSB_ERROR_PIPE          (-9)
#define TEST_LIBUSB_ERROR_NOT_SUPPORTED (-12)
#define TEST_GET_STATUS_TYPE            0x80
#define TEST_GET_STATUS                 0x00

#define TEST_OPEN_LIMIT_MS 500  /* Well under the open's 1000 ms wait */
#define TEST_C_DELAY_MS    1500 /* Longer than that wait */
#define TEST_N_DELAY_MS    100
#define TEST_DUE_MS        300  /* A waiting update falls due after 20 ms */
#define TEST_HANG_MS       3000 /* A call under the joystick lock that takes this long is stuck */
#define TEST_WRITE_MS      1000 /* hid_write's timeout on the interrupt OUT endpoint */
#define TEST_QUIET_MS      600  /* The haptic thread's silence after its last try */

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
    FakeLibUSB_GetCountsFunc GetCounts;
    FakeLibUSB_HoldWritesFunc HoldWrites;
    FakeLibUSB_GetWriteFunc GetWrite;
    FakeLibUSB_SetControlFunc SetControl;
    FakeLibUSB_GetControlFunc GetControl;
    FakeLibUSB_GetUnexpectedFunc GetUnexpected;
} fake;

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
    fake.GetCounts = (FakeLibUSB_GetCountsFunc)GetProcAddress(module, "FakeLibUSB_GetCounts");
    fake.HoldWrites = (FakeLibUSB_HoldWritesFunc)GetProcAddress(module, "FakeLibUSB_HoldWrites");
    fake.GetWrite = (FakeLibUSB_GetWriteFunc)GetProcAddress(module, "FakeLibUSB_GetWrite");
    fake.SetControl = (FakeLibUSB_SetControlFunc)GetProcAddress(module, "FakeLibUSB_SetControl");
    fake.GetControl = (FakeLibUSB_GetControlFunc)GetProcAddress(module, "FakeLibUSB_GetControl");
    fake.GetUnexpected = (FakeLibUSB_GetUnexpectedFunc)GetProcAddress(module, "FakeLibUSB_GetUnexpected");

    /* SDL_InitLibUSB loads "libusb-1.0.dll" by name, which must be this module */
    return fake.Reset && fake.GetCounts && fake.HoldWrites && fake.GetWrite && fake.SetControl && fake.GetControl &&
           fake.GetUnexpected && GetModuleHandleW(L"libusb-1.0.dll") == module;
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

/* The clock of the fake's records */
static double NowMs(void)
{
    LARGE_INTEGER counter, frequency;

    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

/* A joystick record for 06F8:0004 whose effects the test records */
#define TEST_MAX_EFFECTS 64

typedef struct TestEffect
{
    int length;
    Uint8 data[SDL_IFORCE_MAX_COMMAND];
    SDL_ThreadID thread;
    bool failed;
} TestEffect;

static SDL_Mutex *effects_lock;
static TestEffect effects[TEST_MAX_EFFECTS];
static int effect_count;
static int effects_to_fail;
static SDL_HIDAPI_Device *effects_device; /* When set, sends also go to the I-Force driver on this device */

static bool TestSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    SDL_HIDAPI_Device *device;
    bool failed = false;

    SDL_LockMutex(effects_lock);
    if (effects_to_fail > 0) {
        --effects_to_fail;
        failed = true;
    }
    if (effect_count < TEST_MAX_EFFECTS && size > 0 && size <= SDL_IFORCE_MAX_COMMAND) {
        effects[effect_count].length = size;
        SDL_memcpy(effects[effect_count].data, data, (size_t)size);
        effects[effect_count].thread = SDL_GetCurrentThreadID();
        effects[effect_count].failed = failed;
        ++effect_count;
    }
    device = effects_device;
    SDL_UnlockMutex(effects_lock);
    if (failed) {
        return SDL_SetError("The test failed this send");
    }
    if (device) {
        /* HIDAPI_JoystickSendEffect's call, whose write the fake records */
        return device->driver->SendJoystickEffect(device, joystick, data, size);
    }
    return true;
}

static void SetEffectsDevice(SDL_HIDAPI_Device *device)
{
    SDL_LockMutex(effects_lock);
    effects_device = device;
    SDL_UnlockMutex(effects_lock);
}

/* The next sends fail, as a write that the device does not take */
static void FailSends(int count)
{
    SDL_LockMutex(effects_lock);
    effects_to_fail = count;
    SDL_UnlockMutex(effects_lock);
}

static SDL_JoystickDriver test_joystick_driver;

static SDL_Joystick *CreateJoystick(void)
{
    SDL_Joystick *joystick = (SDL_Joystick *)SDL_calloc(1, sizeof(*joystick));

    if (!joystick) {
        return NULL;
    }
    test_joystick_driver.SendEffect = TestSendEffect;
    joystick->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_USB, USB_VENDOR_GUILLEMOT, USB_PRODUCT_GUILLEMOT_FFB_RACING_WHEEL,
                                            0, NULL, "Guillemot Force Feedback Racing Wheel", 0, 0);
    joystick->driver = &test_joystick_driver;
    joystick->ref_count = 1;
    SDL_SetObjectValid(joystick, SDL_OBJECT_TYPE_JOYSTICK, true);
    return joystick;
}

static void DestroyJoystick(SDL_Joystick *joystick)
{
    SDL_SetObjectValid(joystick, SDL_OBJECT_TYPE_JOYSTICK, false);
    if (joystick->props) {
        SDL_DestroyProperties(joystick->props);
    }
    SDL_free(joystick);
}

static void ResetEffects(void)
{
    SDL_LockMutex(effects_lock);
    SDL_zeroa(effects);
    effect_count = 0;
    effects_to_fail = 0;
    SDL_UnlockMutex(effects_lock);
}

/* How many 03 commands carried this level byte and went out, or failed,
   and from which thread the last came */
static int CountMagnitudeSends(Uint8 level, bool failed, SDL_ThreadID *thread)
{
    int i, count = 0;

    SDL_LockMutex(effects_lock);
    for (i = 0; i < effect_count; ++i) {
        if (effects[i].length == 4 && effects[i].data[0] == SDL_IFORCE_CMD_MAGNITUDE && effects[i].data[3] == level &&
            effects[i].failed == failed) {
            ++count;
            if (thread) {
                *thread = effects[i].thread;
            }
        }
    }
    SDL_UnlockMutex(effects_lock);
    return count;
}

static int CountMagnitude(Uint8 level, SDL_ThreadID *thread)
{
    return CountMagnitudeSends(level, false, thread);
}

/* Waits up to a second for a level to go out */
static bool WaitForMagnitude(Uint8 level)
{
    const Uint64 start = SDL_GetTicks();

    while (CountMagnitude(level, NULL) == 0) {
        if (SDL_GetTicks() - start > 1000) {
            return false;
        }
        SDL_Delay(1);
    }
    return true;
}

static bool LastEffectIs(const Uint8 *data, int length)
{
    bool result;

    SDL_LockMutex(effects_lock);
    result = effect_count > 0 && effects[effect_count - 1].length == length &&
             SDL_memcmp(effects[effect_count - 1].data, data, (size_t)length) == 0;
    SDL_UnlockMutex(effects_lock);
    return result;
}

/* The I-Force joystick driver on the fake's handle */
static bool IForceInit(SDL_HIDAPI_Device *device)
{
    bool result;

    SDL_zerop(device);
    device->dev = SDL_hid_open_path(FAKE_LIBUSB_INTEL_PATH);
    if (!device->dev) {
        return false;
    }
    SDL_hid_set_nonblocking(device->dev, 1);
    device->name = SDL_strdup("Guillemot Force Feedback Racing Wheel");
    device->path = SDL_strdup(FAKE_LIBUSB_INTEL_PATH);
    device->vendor_id = USB_VENDOR_GUILLEMOT;
    device->product_id = USB_PRODUCT_GUILLEMOT_FFB_RACING_WHEEL;
    device->interface_number = 0;
    device->driver = &SDL_HIDAPI_DriverIForce;
    /* HIDAPI_UpdateDeviceList sets a driver up under the joystick lock */
    SDL_LockJoysticks();
    result = device->driver->InitDevice(device);
    SDL_UnlockJoysticks();
    return result;
}

/* In the order of HIDAPI_CleanupDeviceDriver. FreeDevice waits for the
   query thread. */
static void IForceFree(SDL_HIDAPI_Device *device)
{
    if (device->driver) {
        device->driver->FreeDevice(device);
    }
    if (device->dev) {
        SDL_hid_close(device->dev);
    }
    SDL_free(device->context);
    SDL_free(device->joysticks);
    SDL_free(device->name);
    SDL_free(device->path);
    SDL_zerop(device);
}

/* GET_STATUS answered, as through WinUSB, and every query answered at once
   unless a case changes it: vendor 06F8, product 0004, memory end 500 and
   12 effects */
static void AnswerAsWinUSB(void)
{
    static const Uint8 status[] = { 0x00, 0x00 };
    static const struct
    {
        Uint8 letter;
        Uint8 reply[3];
        int length;
    } queries[] = {
        { 'O', { 'O' }, 1 },
        { 'M', { 'M', 0xF8, 0x06 }, 3 },
        { 'P', { 'P', 0x04, 0x00 }, 3 },
        { 'B', { 'B', 0xF4, 0x01 }, 3 },
        { 'N', { 'N', 0x0C }, 2 },
        { 'C', { 'C' }, 1 },
        { 'E', { 'E' }, 1 },
        { 'V', { 'V', 0x01 }, 2 },
    };
    size_t i;

    CHECK(fake.SetControl(TEST_GET_STATUS_TYPE, TEST_GET_STATUS, (int)sizeof(status), status, 0), "the fake took no GET_STATUS answer");
    for (i = 0; i < SDL_arraysize(queries); ++i) {
        CHECK(fake.SetControl(SDL_IFORCE_QUERY_REQUEST_TYPE, queries[i].letter, queries[i].length, queries[i].reply, 0),
              "the fake took no answer for %c", queries[i].letter);
    }
}

/* The control transfer that asked this request first, or -1 */
static int FindControl(Uint8 request_type, Uint8 request, FakeLibUSB_Control *control)
{
    FakeLibUSB_Control candidate;
    int i;

    for (i = 0; fake.GetControl(i, &candidate); ++i) {
        if (candidate.request_type == request_type && candidate.request == request) {
            *control = candidate;
            return i;
        }
    }
    return -1;
}

static int CountVendorRequests(void)
{
    FakeLibUSB_Control control;
    int i, count = 0;

    for (i = 0; fake.GetControl(i, &control); ++i) {
        if (control.request_type == SDL_IFORCE_QUERY_REQUEST_TYPE) {
            ++count;
        }
    }
    return count;
}

/* Waits up to a second for the query thread to make this many control
   transfers, since FreeDevice stops it after the query it is on */
static void WaitForControls(int count)
{
    const Uint64 start = SDL_GetTicks();
    FakeLibUSB_Counts counts;

    for (;;) {
        fake.GetCounts(&counts);
        if (counts.controls >= count || SDL_GetTicks() - start > 1000) {
            return;
        }
        SDL_Delay(1);
    }
}

static void CheckAllServed(const char *what)
{
    CHECK(fake.GetUnexpected() == 0, "%s: SDL made %d libusb calls the fake does not serve", what, fake.GetUnexpected());
}

/* 1. libusb reaches a HID-class device that WinUSB is not bound to through
   Windows' HID driver. That path answers GET_STATUS and every vendor
   request with LIBUSB_ERROR_NOT_SUPPORTED (windows_winusb.c
   hid_submit_control_transfer), so no query could be answered. */
static void TestHIDDriverPath(void)
{
    SDL_HIDAPI_Device device;
    FakeLibUSB_Control status;
    bool taken;

    fake.Reset();
    CHECK(fake.SetControl(TEST_GET_STATUS_TYPE, TEST_GET_STATUS, TEST_LIBUSB_ERROR_NOT_SUPPORTED, NULL, 0) &&
          fake.SetControl(SDL_IFORCE_QUERY_REQUEST_TYPE, 'O', TEST_LIBUSB_ERROR_NOT_SUPPORTED, NULL, 0),
          "the fake took no answers");
    taken = IForceInit(&device);
    CHECK(!taken, "the driver took a device whose GET_STATUS libusb refuses, as through the HID driver");
    CHECK(taken || SDL_strstr(SDL_GetError(), "WinUSB") != NULL, "the driver declined with \"%s\"", SDL_GetError());
    if (taken) {
        /* The query thread's first O, before FreeDevice stops it */
        const Uint64 start = SDL_GetTicks();

        while (CountVendorRequests() == 0 && SDL_GetTicks() - start < 1000) {
            SDL_Delay(1);
        }
    }
    IForceFree(&device);

    /* Positive control: the driver asked GET_STATUS, of the device, for 2 bytes */
    CHECK(FindControl(TEST_GET_STATUS_TYPE, TEST_GET_STATUS, &status) == 0 && status.value == 0 && status.index == 0 &&
              status.length == 2 && status.timeout > 0 && status.timeout <= 100,
          "the first control transfer was not GET_STATUS with 2 bytes and a timeout of at most 100 ms");
    CHECK(CountVendorRequests() == 0, "the driver asked %d queries through the HID driver's path", CountVendorRequests());
    CheckAllServed("the HID driver's path");

    /* The same device through WinUSB is taken and asked O */
    fake.Reset();
    AnswerAsWinUSB();
    taken = IForceInit(&device);
    CHECK(taken, "the driver declined a device whose GET_STATUS WinUSB answers: %s", SDL_GetError());
    WaitForControls(10);
    IForceFree(&device);
    {
        FakeLibUSB_Control first;

        CHECK(FindControl(TEST_GET_STATUS_TYPE, TEST_GET_STATUS, &status) == 0 &&
                  FindControl(SDL_IFORCE_QUERY_REQUEST_TYPE, 'O', &first) == 1,
              "through WinUSB the driver did not ask GET_STATUS and then O");
        CHECK(CountVendorRequests() == 9, "through WinUSB the driver asked %d queries, not the 9 of the sequence", CountVendorRequests());
    }
    CheckAllServed("WinUSB");
}

/* Opens the joystick as SDL_OpenJoystick does, under the joystick lock,
   and returns the published effect count and memory end */
static void OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, int *effect_number, int *memory_end)
{
    SDL_PropertiesID props;

    SDL_LockJoysticks();
    device->driver->OpenJoystick(device, joystick);
    SDL_UnlockJoysticks();
    props = SDL_GetJoystickProperties(joystick);
    *effect_number = (int)SDL_GetNumberProperty(props, SDL_IFORCE_PROP_EFFECTS_NUMBER, 0);
    *memory_end = (int)SDL_GetNumberProperty(props, SDL_IFORCE_PROP_MEMORY_NUMBER, 0);
}

/* 2. N and B are final once N is answered. C, E, O and V change neither,
   and C may take up to its 1000 ms timeout. */
static void TestPublishAfterN(void)
{
    static const Uint8 c_reply[] = { 'C' };
    SDL_HIDAPI_Device device;
    SDL_Joystick *joystick = CreateJoystick();
    FakeLibUSB_Control n, c;
    int effect_number = 0, memory_end = 0;
    double start, opened;

    if (!joystick) {
        CHECK(false, "no joystick record");
        return;
    }
    fake.Reset();
    AnswerAsWinUSB();
    fake.SetControl(SDL_IFORCE_QUERY_REQUEST_TYPE, 'C', 1, c_reply, TEST_C_DELAY_MS);
    start = NowMs();
    CHECK(IForceInit(&device), "the driver did not start: %s", SDL_GetError());
    if (device.context) {
        OpenJoystick(&device, joystick, &effect_number, &memory_end);
        opened = NowMs();
        CHECK(effect_number == 12 && memory_end == 500,
              "the joystick opened after %.0f ms with %d effects and memory end %d, where N gave 12 and B 500",
              opened - start, effect_number, memory_end);
        CHECK(opened - start < TEST_OPEN_LIMIT_MS, "the open waited %.0f ms for the queries after N", opened - start);
        IForceFree(&device);

        /* Positive control: N was answered before the open returned, and C after */
        CHECK(FindControl(SDL_IFORCE_QUERY_REQUEST_TYPE, 'N', &n) >= 0 && n.result == 2 && n.done_ms <= opened,
              "N was not answered before the open returned");
        CHECK(FindControl(SDL_IFORCE_QUERY_REQUEST_TYPE, 'C', &c) >= 0 && c.done_ms > opened,
              "C was answered before the open returned, so this case never arose");
    } else {
        IForceFree(&device);
    }
    CheckAllServed("a slow C");
    DestroyJoystick(joystick);
}

/* 3. Only a device that leaves O unanswered can take 20 timeouts. An M that
   stalls leaves the open waiting for N, which comes 100 ms later here. */
static void TestStalledM(void)
{
    static const Uint8 n_reply[] = { 'N', 0x0C };
    SDL_HIDAPI_Device device;
    SDL_Joystick *joystick = CreateJoystick();
    FakeLibUSB_Control m;
    int effect_number = 0, memory_end = 0;

    if (!joystick) {
        CHECK(false, "no joystick record");
        return;
    }
    fake.Reset();
    AnswerAsWinUSB();
    fake.SetControl(SDL_IFORCE_QUERY_REQUEST_TYPE, 'M', TEST_LIBUSB_ERROR_PIPE, NULL, 0);
    fake.SetControl(SDL_IFORCE_QUERY_REQUEST_TYPE, 'N', (int)sizeof(n_reply), n_reply, TEST_N_DELAY_MS);
    CHECK(IForceInit(&device), "the driver did not start: %s", SDL_GetError());
    if (device.context) {
        OpenJoystick(&device, joystick, &effect_number, &memory_end);
        CHECK(effect_number == 12 && memory_end == 500,
              "after a stalled M the joystick opened with %d effects and memory end %d, where N gave 12 and B 500",
              effect_number, memory_end);
    }
    IForceFree(&device);
    /* Positive control: M was asked and stalled */
    CHECK(FindControl(SDL_IFORCE_QUERY_REQUEST_TYPE, 'M', &m) >= 0 && m.result == TEST_LIBUSB_ERROR_PIPE, "M was not asked, or did not stall");
    CheckAllServed("a stalled M");
    DestroyJoystick(joystick);
}

/* The haptic driver on a joystick record, with one application thread that
   holds the joystick lock the way SDL_UpdateJoysticks does while it delivers
   an event to a watcher */
typedef enum HapticAction
{
    HAPTIC_HOLD,   /* Hold the lock and call nothing */
    HAPTIC_UPDATE, /* Update the effect again */
    HAPTIC_CLOSE   /* Close the haptic device */
} HapticAction;

typedef struct HapticCase
{
    SDL_HIDAPI_HapticDevice device;
    HapticAction action;
    SDL_HapticEffectID id;
    bool waited; /* The first update waited, as the case needs */
    bool result; /* The second update's */
    SDL_ThreadID thread;
    SDL_AtomicInt done;
} HapticCase;

static void ConstantEffect(SDL_HapticEffect *effect, Sint16 level)
{
    SDL_zerop(effect);
    effect->type = SDL_HAPTIC_CONSTANT;
    effect->constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect->constant.length = 1000;
    effect->constant.level = level;
}

static bool HapticOpen(HapticCase *c)
{
    static const Uint8 enable[] = { SDL_IFORCE_CMD_STATE, SDL_IFORCE_STATE_ENABLE };
    SDL_Joystick *joystick = CreateJoystick();
    SDL_PropertiesID props;
    void *ctx;

    SDL_zerop(c);
    if (!joystick) {
        CHECK(false, "no joystick record");
        return false;
    }
    props = SDL_GetJoystickProperties(joystick);
    SDL_SetNumberProperty(props, SDL_IFORCE_PROP_EFFECTS_NUMBER, 10);
    SDL_SetNumberProperty(props, SDL_IFORCE_PROP_MEMORY_NUMBER, 200);
    ResetEffects();
    /* SDL_OpenHapticFromJoystick opens it under the joystick lock */
    SDL_LockJoysticks();
    ctx = SDL_HIDAPI_HapticDriverIForce.Open(joystick);
    SDL_UnlockJoysticks();
    CHECK(ctx != NULL, "the haptic driver did not open: %s", SDL_GetError());
    if (!ctx) {
        DestroyJoystick(joystick);
        return false;
    }
    c->device.joystick = joystick;
    c->device.driver = &SDL_HIDAPI_HapticDriverIForce;
    c->device.ctx = ctx;
    /* Positive control: the start went out through the joystick */
    CHECK(LastEffectIs(enable, sizeof(enable)), "the haptic open did not send 42 04 last");
    return true;
}

/* As SDL_HIDAPI_HapticClose, which frees the context after the close */
static void HapticFree(HapticCase *c, bool closed)
{
    if (!closed) {
        c->device.driver->Close(&c->device);
    }
    SDL_free(c->device.ctx);
    DestroyJoystick(c->device.joystick);
}

static int SDLCALL HapticUnderLock(void *data)
{
    HapticCase *c = (HapticCase *)data;
    SDL_HapticEffect effect;

    c->thread = SDL_GetCurrentThreadID();
    SDL_LockJoysticks();
    ConstantEffect(&effect, 0x2000);
    c->id = c->device.driver->CreateEffect(&c->device, &effect);
    /* Within 20 ms of the block's first write, the new level waits */
    ConstantEffect(&effect, 0x3000);
    if (c->id >= 0 && c->device.driver->UpdateEffect(&c->device, c->id, &effect)) {
        c->waited = (CountMagnitude(0x30, NULL) == 0);
    }
    SDL_Delay(TEST_DUE_MS);
    if (c->action == HAPTIC_UPDATE) {
        ConstantEffect(&effect, 0x4000);
        c->result = c->device.driver->UpdateEffect(&c->device, c->id, &effect);
    } else if (c->action == HAPTIC_CLOSE) {
        c->device.driver->Close(&c->device);
    }
    SDL_UnlockJoysticks();
    SDL_SetAtomicInt(&c->done, 1);
    return 0;
}

/* Runs the application thread and waits for it. A thread stuck under the
   joystick lock cannot be joined or cleaned up, so the test ends there. */
static void RunUnderLock(HapticCase *c, const char *what)
{
    SDL_Thread *thread = SDL_CreateThread(HapticUnderLock, "test application", c);
    const Uint64 start = SDL_GetTicks();

    if (!thread) {
        CHECK(false, "no application thread: %s", SDL_GetError());
        SDL_SetAtomicInt(&c->done, 1);
        return;
    }
    while (!SDL_GetAtomicInt(&c->done)) {
        if (SDL_GetTicks() - start > TEST_HANG_MS) {
            CHECK(false, "%s under the joystick lock did not return within %d ms while a waiting update was due", what, TEST_HANG_MS);
            printf("FAILED: %d of %d checks\n", failures, checks);
            fflush(stdout);
            _exit(1);
        }
        SDL_Delay(1);
    }
    SDL_WaitThread(thread, NULL);
    CHECK(c->id >= 0 && c->waited, "the first update did not wait, so this case never arose");
}

/* 4. The effect model must match what the device holds, so a failed send
   leaves it as before and the same update goes out again, here 20 ms after
   the failed try */
static void TestHapticRetry(void)
{
    HapticCase c;
    SDL_HapticEffect effect;

    if (!HapticOpen(&c)) {
        return;
    }
    ConstantEffect(&effect, 0x2000);
    c.id = c.device.driver->CreateEffect(&c.device, &effect);
    CHECK(c.id >= 0, "no effect: %s", SDL_GetError());
    /* Past the block's 20 ms, a new level goes at once */
    SDL_Delay(30);
    FailSends(1);
    ConstantEffect(&effect, 0x5000);
    CHECK(!c.device.driver->UpdateEffect(&c.device, c.id, &effect), "an update whose send failed reported success");
    /* Positive control: the new level was tried */
    CHECK(CountMagnitudeSends(0x50, true, NULL) == 1, "the new level was not tried");
    CHECK(c.device.driver->UpdateEffect(&c.device, c.id, &effect), "the same update again failed: %s", SDL_GetError());
    CHECK(WaitForMagnitude(0x50), "the same update again never went out");
    HapticFree(&c, false);
}

/* 5. The haptic thread's failed send is undone too, and the update waits
   for another try */
static void TestHapticThreadRetry(void)
{
    HapticCase c;
    SDL_HapticEffect effect;
    bool waited;

    if (!HapticOpen(&c)) {
        return;
    }
    ConstantEffect(&effect, 0x2000);
    c.id = c.device.driver->CreateEffect(&c.device, &effect);
    CHECK(c.id >= 0, "no effect: %s", SDL_GetError());
    /* The joystick lock keeps the thread from sending until the failure is set */
    SDL_LockJoysticks();
    ConstantEffect(&effect, 0x6000);
    CHECK(c.device.driver->UpdateEffect(&c.device, c.id, &effect), "the update failed: %s", SDL_GetError());
    waited = (CountMagnitudeSends(0x60, false, NULL) == 0 && CountMagnitudeSends(0x60, true, NULL) == 0);
    FailSends(1);
    SDL_UnlockJoysticks();
    CHECK(waited, "the update did not wait, so this case never arose");
    CHECK(WaitForMagnitude(0x60), "the waiting update never went out after its failed try");
    /* Positive control: the thread's first try failed */
    CHECK(CountMagnitudeSends(0x60, true, NULL) == 1, "the thread's first try did not fail");
    HapticFree(&c, false);
}

/* 6. The haptic thread sends a waiting update through
   SDL_SendJoystickEffect, which takes the joystick lock, and the effect
   functions take the driver's lock. Both must come in one order. */
static void TestHapticUpdateUnderLock(void)
{
    HapticCase c;
    SDL_ThreadID thread = 0;

    if (!HapticOpen(&c)) {
        return;
    }
    c.action = HAPTIC_UPDATE;
    RunUnderLock(&c, "an effect update");
    CHECK(c.result, "the effect update under the joystick lock failed: %s", SDL_GetError());
    CHECK(CountMagnitude(0x40, &thread) == 1 && thread == c.thread, "the new level did not go out from the updating thread");
    HapticFree(&c, false);
    /* It went out at once and dropped the waiting level */
    CHECK(CountMagnitude(0x30, NULL) == 0, "the dropped level went out after all");

    /* A waiting update that fell due under the lock goes out once it is free */
    if (!HapticOpen(&c)) {
        return;
    }
    c.action = HAPTIC_HOLD;
    RunUnderLock(&c, "holding the lock");
    CHECK(WaitForMagnitude(0x30) && CountMagnitude(0x30, &thread) == 1 && thread != c.thread && thread != SDL_GetCurrentThreadID(),
          "the waiting level did not go out from the haptic thread once the joystick lock was free");
    HapticFree(&c, false);
}

/* 7. The close stops the haptic thread and waits for it */
static void TestHapticCloseUnderLock(void)
{
    static const Uint8 stop[] = { SDL_IFORCE_CMD_STATE, SDL_IFORCE_STATE_STOP_ALL };
    HapticCase c;

    if (!HapticOpen(&c)) {
        return;
    }
    c.action = HAPTIC_CLOSE;
    RunUnderLock(&c, "the haptic close");
    CHECK(LastEffectIs(stop, sizeof(stop)), "the close did not end with 42 01");
    CHECK(CountMagnitude(0x30, NULL) == 0, "the waiting level went out during the close");
    HapticFree(&c, true);
}

/* The writes of one 03 command's level byte that reached the fake */
typedef struct FakeLevel
{
    int writes;    /* That reached the fake */
    int returned;  /* Of those, the ones that returned */
    int sent;      /* Of those, the ones that went out */
    int timed_out; /* Of those, the ones the held endpoint timed out */
} FakeLevel;

static void CountFakeLevel(Uint8 level, FakeLevel *count)
{
    FakeLibUSB_Counts counts;
    FakeLibUSB_Write write;
    int i;

    SDL_zerop(count);
    fake.GetCounts(&counts);
    for (i = 0; i < counts.writes && fake.GetWrite(i, &write); ++i) {
        if (write.length != 4 || write.data[0] != SDL_IFORCE_CMD_MAGNITUDE || write.data[3] != level) {
            continue;
        }
        ++count->writes;
        if (write.done_ms != 0.0) {
            ++count->returned;
            if (write.result == 0) {
                ++count->sent;
            } else if (write.result == TEST_LIBUSB_ERROR_TIMEOUT) {
                ++count->timed_out;
            }
        }
    }
}

/* 8. On an OUT endpoint that refuses data every try holds the joystick lock
   for the write's 1000 ms, so the thread's tries of one waiting update are
   bounded. The joystick record hands each send to the I-Force joystick
   driver, whose SendJoystickEffect uses only the device's handle, here the
   fake's. */
static void TestHapticTriesBound(void)
{
    SDL_HIDAPI_Device device;
    HapticCase c;
    SDL_HapticEffect effect;
    FakeLevel level;
    Uint64 start;

    fake.Reset();
    SDL_zero(device);
    device.dev = SDL_hid_open_path(FAKE_LIBUSB_INTEL_PATH);
    CHECK(device.dev != NULL, "the fake device did not open: %s", SDL_GetError());
    if (!device.dev) {
        return;
    }
    device.driver = &SDL_HIDAPI_DriverIForce;
    SetEffectsDevice(&device);
    if (HapticOpen(&c)) {
        ConstantEffect(&effect, 0x2000);
        c.id = c.device.driver->CreateEffect(&c.device, &effect);
        CHECK(c.id >= 0, "no effect: %s", SDL_GetError());
        /* Positive control: the effect's level went out through the fake */
        CountFakeLevel(0x20, &level);
        CHECK(level.sent == 1, "the effect's level went out %d times through the fake", level.sent);

        /* The endpoint stops taking data, and a new level waits for its
           block's 20 ms */
        fake.HoldWrites(1);
        ConstantEffect(&effect, 0x7000);
        CHECK(c.device.driver->UpdateEffect(&c.device, c.id, &effect), "the update failed: %s", SDL_GetError());
        CountFakeLevel(0x70, &level);
        CHECK(level.writes == 0, "the new level did not wait, so this case never arose");

        /* The tries, then a quiet time */
        start = SDL_GetTicks();
        do {
            SDL_Delay(5);
            CountFakeLevel(0x70, &level);
        } while (level.returned < SDL_IFORCE_SEND_TRIES && SDL_GetTicks() - start < (SDL_IFORCE_SEND_TRIES + 2) * TEST_WRITE_MS);
        SDL_Delay(TEST_QUIET_MS);
        CountFakeLevel(0x70, &level);
        CHECK(level.writes == SDL_IFORCE_SEND_TRIES, "the haptic thread sent the waiting level %d times, where %d tries are the most",
              level.writes, SDL_IFORCE_SEND_TRIES);
        /* Positive control: each try reached the fake and timed out */
        CHECK(level.timed_out == SDL_IFORCE_SEND_TRIES, "%d of the tries timed out at the fake", level.timed_out);

        /* A later update from the application is built against the level
           the device holds, so the dropped level goes out again */
        fake.HoldWrites(0);
        CHECK(c.device.driver->UpdateEffect(&c.device, c.id, &effect), "the later update failed: %s", SDL_GetError());
        CountFakeLevel(0x70, &level);
        CHECK(level.sent == 1 && level.writes == SDL_IFORCE_SEND_TRIES + 1,
              "the later update of the dropped level went out %d times in %d writes", level.sent, level.writes);
        HapticFree(&c, false);
    }
    SetEffectsDevice(NULL);
    SDL_hid_close(device.dev);
    CheckAllServed("an endpoint that refuses data");
}

int main(int argc, char *argv[])
{
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
    effects_lock = SDL_CreateMutex();
    if (!effects_lock) {
        printf("SDL_CreateMutex failed: %s\n", SDL_GetError());
        SDL_hid_exit();
        SDL_Quit();
        return 1;
    }

    TestHIDDriverPath();
    TestPublishAfterN();
    TestStalledM();
    TestHapticRetry();
    TestHapticThreadRetry();
    TestHapticUpdateUnderLock();
    TestHapticCloseUnderLock();
    TestHapticTriesBound();

    SDL_DestroyMutex(effects_lock);
    SDL_hid_exit();
    SDL_Quit();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
