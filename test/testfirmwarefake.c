/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* The scripted controller of the firmware version tests. See
   testfirmwarefake.h. This file calls SDL itself only to lock the joystick
   list, mark the joystick record valid and free memory. */

#define TESTFIRMWAREFAKE_IMPLEMENTATION
#include "testfirmwarefake.h"

int fake_checks;
int fake_failures;
const char *fake_scenario = "";

SDL_HIDAPI_Device fake_device;
SDL_Joystick *fake_joystick;
int fake_writes;
int fake_unexpected;
Uint64 fake_now;
int (*fake_feature_report)(unsigned char *data, size_t length);
int fake_feature_requests;

#define FAKE_MAX_CALLBACKS 16

static struct
{
    Uint8 queue[FAKE_MAX_REPORTS][FAKE_REPORT_SIZE];
    int queue_length[FAKE_MAX_REPORTS];
    int queue_head;
    int queue_count;
    int locks; /* SDL_HIDAPI_LockRumble without the send that releases it */
    void (*callbacks[FAKE_MAX_CALLBACKS])(void *userdata);
    void *userdata[FAKE_MAX_CALLBACKS];
    int num_callbacks;
} fake;

static bool record_closed; /* The driver closed the record when its joystick went */
static SDL_JoystickID next_instance_id = 1000;

SDL_hid_device *Fake_Handle(void)
{
    return (SDL_hid_device *)(void *)&fake;
}

void Fake_Reset(void)
{
    SDL_zero(fake);
    fake_writes = 0;
    fake_unexpected = 0;
    fake_feature_requests = 0;
}

void Fake_Unexpected(const char *what)
{
    ++fake_unexpected;
    printf("fake: unexpected %s\n", what);
}

/* A report the controller sends */
void Fake_Queue(const Uint8 *report, int length)
{
    int slot;

    if (fake.queue_count == FAKE_MAX_REPORTS || length < 1 || length > FAKE_REPORT_SIZE) {
        Fake_Unexpected("report beyond the queue");
        return;
    }
    slot = (fake.queue_head + fake.queue_count) % FAKE_MAX_REPORTS;
    SDL_memcpy(fake.queue[slot], report, (size_t)length);
    fake.queue_length[slot] = length;
    ++fake.queue_count;
}

/* As hidapi reads: the next report at its own length, cut to the buffer */
int Fake_hid_read_timeout(SDL_hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
    int size;

    (void)milliseconds;
    if (dev != Fake_Handle()) {
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

static int Fake_Written(const Uint8 *data, int size, bool async)
{
    if (size < 1 || size > FAKE_REPORT_SIZE) {
        Fake_Unexpected("write size");
        return -1;
    }
    ++fake_writes;
    Firmware_Respond(data, size, async);
    return size;
}

int Fake_hid_write(SDL_hid_device *dev, const unsigned char *data, size_t length)
{
    if (dev != Fake_Handle()) {
        Fake_Unexpected("write handle");
        return -1;
    }
    return Fake_Written(data, (int)length, false);
}

int Fake_hid_get_feature_report(SDL_hid_device *dev, unsigned char *data, size_t length)
{
    if (dev != Fake_Handle()) {
        Fake_Unexpected("feature report handle");
        return -1;
    }
    ++fake_feature_requests;
    if (fake_feature_report) {
        return fake_feature_report(data, length);
    }
    Fake_Unexpected("feature report");
    return -1;
}

/* Only Linux synthesizes a report descriptor for an Xbox controller */
int Fake_hid_get_report_descriptor(SDL_hid_device *dev, unsigned char *buf, size_t buf_size)
{
    (void)dev;
    (void)buf;
    (void)buf_size;
    return -1;
}

bool Fake_LockRumble(void)
{
    ++fake.locks;
    return true;
}

void Fake_UnlockRumble(void)
{
    if (fake.locks != 1) {
        Fake_Unexpected("unlock without the lock");
    }
    fake.locks = 0;
}

int Fake_SendRumbleAndUnlock(SDL_HIDAPI_Device *device, const Uint8 *data, int size)
{
    if (fake.locks != 1) {
        Fake_Unexpected("queued write without the lock");
    }
    fake.locks = 0;
    if (device != &fake_device) {
        Fake_Unexpected("queued write device");
        return -1;
    }
    return Fake_Written(data, size, true);
}

/* The rumble thread calls the callback after the write, so the fake calls it
   at the start of the next update */
int Fake_SendRumbleWithCallbackAndUnlock(SDL_HIDAPI_Device *device, const Uint8 *data, int size, void (*callback)(void *userdata), void *userdata)
{
    const int result = Fake_SendRumbleAndUnlock(device, data, size);

    if (callback && result == size) {
        if (fake.num_callbacks == FAKE_MAX_CALLBACKS) {
            Fake_Unexpected("callback beyond the queue");
        } else {
            fake.callbacks[fake.num_callbacks] = callback;
            fake.userdata[fake.num_callbacks] = userdata;
            ++fake.num_callbacks;
        }
    }
    return result;
}

int Fake_SendRumble(SDL_HIDAPI_Device *device, const Uint8 *data, int size)
{
    if (fake.locks != 0) {
        Fake_Unexpected("rumble write under the lock");
    }
    if (device != &fake_device) {
        Fake_Unexpected("rumble device");
        return -1;
    }
    return Fake_Written(data, size, true);
}

/* As HIDAPI_JoystickConnected, without SDL's joystick list */
bool Fake_JoystickConnected(SDL_HIDAPI_Device *device, SDL_JoystickID *pJoystickID)
{
    SDL_JoystickID *joysticks = (SDL_JoystickID *)SDL_realloc(device->joysticks, (size_t)(device->num_joysticks + 1) * sizeof(*joysticks));

    if (!joysticks) {
        return false;
    }
    device->joysticks = joysticks;
    device->joysticks[device->num_joysticks++] = ++next_instance_id;
    if (pJoystickID) {
        *pJoystickID = next_instance_id;
    }
    return true;
}

/* As HIDAPI_JoystickDisconnected, which closes the joystick first when the
   app has it open */
void Fake_JoystickDisconnected(SDL_HIDAPI_Device *device, SDL_JoystickID joystickID)
{
    int i;

    for (i = 0; i < device->num_joysticks; ++i) {
        if (device->joysticks[i] == joystickID) {
            if (fake_joystick && fake_joystick->instance_id == joystickID && !record_closed) {
                if (device->driver->CloseJoystick) {
                    device->driver->CloseJoystick(device, fake_joystick);
                }
                record_closed = true;
            }
            SDL_memmove(&device->joysticks[i], &device->joysticks[i + 1], (size_t)(device->num_joysticks - i - 1) * sizeof(*device->joysticks));
            --device->num_joysticks;
            break;
        }
    }
}

/* SDL's list holds only joysticks the app opened, so the record is found
   only while it is open */
SDL_Joystick *Fake_GetJoystickFromID(SDL_JoystickID instance_id)
{
    return (fake_joystick && !record_closed && fake_joystick->instance_id == instance_id) ? fake_joystick : NULL;
}

Uint64 Fake_GetTicks(void)
{
    return fake_now;
}

void Fake_Delay(Uint32 ms)
{
    fake_now += ms;
}

bool Fake_InitDevice(void)
{
    bool result;

    SDL_LockJoysticks();
    result = fake_device.driver->InitDevice(&fake_device);
    SDL_UnlockJoysticks();
    return result;
}

void Fake_UpdateDevice(void)
{
    int i, count = fake.num_callbacks;

    fake.num_callbacks = 0;
    for (i = 0; i < count; ++i) {
        fake.callbacks[i](fake.userdata[i]);
    }
    SDL_LockJoysticks();
    fake_device.driver->UpdateDevice(&fake_device);
    SDL_UnlockJoysticks();
}

/* As SDL_OpenJoystick: the driver's open on a new record, for the device's
   first joystick */
bool Fake_OpenRecord(void)
{
    bool opened;

    if (fake_device.num_joysticks < 1 || fake_joystick) {
        return false;
    }
    fake_joystick = (SDL_Joystick *)SDL_calloc(1, sizeof(*fake_joystick));
    if (!fake_joystick) {
        return false;
    }
    fake_joystick->instance_id = fake_device.joysticks[0];
    fake_joystick->guid = fake_device.guid;
    fake_joystick->attached = true;
    fake_joystick->ref_count = 1;
    SDL_SetObjectValid(fake_joystick, SDL_OBJECT_TYPE_JOYSTICK, true);
    record_closed = false;

    SDL_LockJoysticks();
    opened = fake_device.driver->OpenJoystick(&fake_device, fake_joystick);
    SDL_UnlockJoysticks();
    if (!opened) {
        record_closed = true;
        Fake_CloseRecord();
        return false;
    }
    /* The arrays SDL_OpenJoystick allocates after the driver's open */
    if (fake_joystick->naxes > 0) {
        fake_joystick->axes = (SDL_JoystickAxisInfo *)SDL_calloc((size_t)fake_joystick->naxes, sizeof(*fake_joystick->axes));
    }
    if (fake_joystick->nhats > 0) {
        fake_joystick->hats = (Uint8 *)SDL_calloc((size_t)fake_joystick->nhats, sizeof(*fake_joystick->hats));
    }
    if (fake_joystick->nbuttons > 0) {
        fake_joystick->buttons = (bool *)SDL_calloc((size_t)fake_joystick->nbuttons, sizeof(*fake_joystick->buttons));
    }
    return (fake_joystick->naxes <= 0 || fake_joystick->axes) &&
           (fake_joystick->nhats <= 0 || fake_joystick->hats) &&
           (fake_joystick->nbuttons <= 0 || fake_joystick->buttons);
}

void Fake_CloseRecord(void)
{
    int i;

    if (!fake_joystick) {
        return;
    }
    /* A joystick the driver disconnected was closed then, and SDL closes it
       only once */
    if (!record_closed && fake_device.driver->CloseJoystick) {
        SDL_LockJoysticks();
        fake_device.driver->CloseJoystick(&fake_device, fake_joystick);
        SDL_UnlockJoysticks();
    }
    record_closed = false;
    SDL_SetObjectValid(fake_joystick, SDL_OBJECT_TYPE_JOYSTICK, false);
    if (fake_joystick->props) {
        SDL_DestroyProperties(fake_joystick->props);
    }
    SDL_free(fake_joystick->axes);
    SDL_free(fake_joystick->hats);
    SDL_free(fake_joystick->buttons);
    /* As SDL_CloseJoystick frees the touchpads a driver adds */
    for (i = 0; i < fake_joystick->ntouchpads; ++i) {
        SDL_free(fake_joystick->touchpads[i].fingers);
    }
    SDL_free(fake_joystick->touchpads);
    SDL_free(fake_joystick->sensors);
    SDL_free(fake_joystick->serial);
    SDL_free(fake_joystick);
    fake_joystick = NULL;
}

/* As HIDAPI_CleanupDeviceDriver: FreeDevice when the driver has one */
void Fake_FreeDevice(void)
{
    Fake_CloseRecord();
    if (fake_device.driver && fake_device.driver->FreeDevice) {
        SDL_LockJoysticks();
        fake_device.driver->FreeDevice(&fake_device);
        SDL_UnlockJoysticks();
    }
    SDL_free(fake_device.context);
    SDL_free(fake_device.joysticks);
    SDL_free(fake_device.name);
    SDL_free(fake_device.manufacturer_string);
    SDL_free(fake_device.product_string);
    SDL_free(fake_device.serial);
    SDL_free(fake_device.path);
    SDL_zero(fake_device);
}

int Fake_Finish(const char *name)
{
    if (fake_failures) {
        printf("%s FAILED: %d of %d checks\n", name, fake_failures, fake_checks);
        return 1;
    }
    printf("%s PASSED: %d checks, 0 failures\n", name, fake_checks);
    return 0;
}
