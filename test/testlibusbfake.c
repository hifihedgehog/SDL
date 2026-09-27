/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* A fake libusb-1.0.dll for test/libusb-backend. It exports every function
   SDL_InitLibUSB loads (src/misc/SDL_libusb.c) and serves the two devices
   that testlibusbfake.h describes. It never reaches a real device. Transfers
   submitted for an IN endpoint complete from libusb_handle_events with the
   packets the test queues, and a canceled transfer completes as canceled
   there, as libusb reports both. Interrupt and bulk OUT transfers are
   synchronous and recorded, and while the test holds writes each one waits,
   as on an endpoint that refuses data, until the test lets writes go or its
   timeout passes. Control transfers are synchronous, recorded, and answered
   as the test sets them. Calls the model does not serve count as
   unexpected. */

#include <libusb.h>

#include <stdlib.h>
#include <string.h>

#include "testlibusbfake.h"

struct libusb_context
{
    int unused;
};

struct libusb_device
{
    int unused;
};

struct libusb_device_handle
{
    libusb_device *device;
};

#define FAKE_MAX_PENDING 8
#define FAKE_MAX_INPUTS  16
#define FAKE_INPUT_SIZE  64
#define FAKE_WAIT_MS     10 /* The longest libusb_handle_events waits for work */
#define FAKE_MAX_WRITERS 4  /* OUT transfers running at once */
#define FAKE_MAX_ANSWERS 16
#define FAKE_ANSWER_SIZE 16

/* The base station as JoypadOS read it (joypad-os 969c232c,
   docs/INTEL_WIRELESS_SERIES.md): one configuration, value 1, of 89 bytes.
   Fields the record does not give, such as bcdDevice and MaxPower, are
   placeholders this test does not depend on. */
static const struct libusb_endpoint_descriptor fake_keyboard_endpoints[] = {
    { 7, LIBUSB_DT_ENDPOINT, 0x81, LIBUSB_TRANSFER_TYPE_INTERRUPT, 8, 10, 0, 0, NULL, 0 },
};

static const struct libusb_endpoint_descriptor fake_pad_endpoints[] = {
    { 7, LIBUSB_DT_ENDPOINT, 0x81, LIBUSB_TRANSFER_TYPE_INTERRUPT, 27, 1, 0, 0, NULL, 0 },
    { 7, LIBUSB_DT_ENDPOINT, 0x01, LIBUSB_TRANSFER_TYPE_INTERRUPT, 25, 1, 0, 0, NULL, 0 },
    { 7, LIBUSB_DT_ENDPOINT, 0x02, LIBUSB_TRANSFER_TYPE_CONTROL, 8, 0, 0, 0, NULL, 0 },
};

static const struct libusb_endpoint_descriptor fake_mouse_endpoints[] = {
    { 7, LIBUSB_DT_ENDPOINT, 0x82, LIBUSB_TRANSFER_TYPE_INTERRUPT, 4, 10, 0, 0, NULL, 0 },
};

static const struct libusb_interface_descriptor fake_interface0[] = {
    { 9, LIBUSB_DT_INTERFACE, 0, 0, 1, LIBUSB_CLASS_HID, 1, 1, 0, fake_keyboard_endpoints, NULL, 0 },
    { 9, LIBUSB_DT_INTERFACE, 0, 1, 3, 0, 0, 0, 0, fake_pad_endpoints, NULL, 0 },
};

static const struct libusb_interface_descriptor fake_interface1[] = {
    { 9, LIBUSB_DT_INTERFACE, 1, 0, 1, LIBUSB_CLASS_HID, 1, 2, 0, fake_mouse_endpoints, NULL, 0 },
};

static const struct libusb_interface fake_interfaces[] = {
    { fake_interface0, 2 },
    { fake_interface1, 1 },
};

static const struct libusb_config_descriptor fake_config = {
    9, LIBUSB_DT_CONFIG, 89, 2, 1, 0, 0x80, 50, fake_interfaces, NULL, 0
};

static const struct libusb_device_descriptor fake_device_descriptor = {
    18, LIBUSB_DT_DEVICE, 0x0110, 0, 0, 0, 8, 0x8086, 0xC013, 0x0100, 0, 0, 0, 1
};

/* The DJI RC (RM330), 2CA3:1023: MTP, the DUML bulk interface and ADB, as
   testvendorusb.c lists them. Interface 1 has dji-rc-joystick's endpoints,
   bulk OUT 0x02 and bulk IN 0x83 of 512 bytes at high speed. SDL never opens
   interfaces 0 and 2, whose endpoints no record gives, so they have none
   here. bcdDevice and MaxPower are placeholders. */
static const struct libusb_endpoint_descriptor fake_dji_endpoints[] = {
    { 7, LIBUSB_DT_ENDPOINT, 0x02, LIBUSB_TRANSFER_TYPE_BULK, 512, 0, 0, 0, NULL, 0 },
    { 7, LIBUSB_DT_ENDPOINT, 0x83, LIBUSB_TRANSFER_TYPE_BULK, 512, 0, 0, 0, NULL, 0 },
};

static const struct libusb_interface_descriptor fake_dji_mtp[] = {
    { 9, LIBUSB_DT_INTERFACE, 0, 0, 0, LIBUSB_CLASS_IMAGE, 1, 1, 0, NULL, NULL, 0 },
};

static const struct libusb_interface_descriptor fake_dji_duml[] = {
    { 9, LIBUSB_DT_INTERFACE, 1, 0, 2, LIBUSB_CLASS_VENDOR_SPEC, 0x43, 1, 0, fake_dji_endpoints, NULL, 0 },
};

static const struct libusb_interface_descriptor fake_dji_adb[] = {
    { 9, LIBUSB_DT_INTERFACE, 2, 0, 0, LIBUSB_CLASS_VENDOR_SPEC, 0x42, 1, 0, NULL, NULL, 0 },
};

static const struct libusb_interface fake_dji_interfaces[] = {
    { fake_dji_mtp, 1 },
    { fake_dji_duml, 1 },
    { fake_dji_adb, 1 },
};

static const struct libusb_config_descriptor fake_dji_config = {
    9, LIBUSB_DT_CONFIG, 50, 3, 1, 0, 0x80, 250, fake_dji_interfaces, NULL, 0
};

static const struct libusb_device_descriptor fake_dji_device_descriptor = {
    18, LIBUSB_DT_DEVICE, 0x0200, 0, 0, 0, 64, 0x2CA3, 0x1023, 0x0100, 0, 0, 0, 1
};

static struct libusb_context fake_context;
static struct libusb_device fake_device;
static struct libusb_device fake_dji_device;

typedef struct FakeInput
{
    int length;
    unsigned char data[FAKE_INPUT_SIZE];
} FakeInput;

/* The test's answer to one control request */
typedef struct FakeAnswer
{
    unsigned char request_type;
    unsigned char request;
    int result;
    int delay_ms;
    unsigned char reply[FAKE_ANSWER_SIZE];
} FakeAnswer;

/* Everything below is guarded by fake_lock */
static SRWLOCK fake_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE fake_wake = CONDITION_VARIABLE_INIT;
static struct libusb_transfer *fake_pending[FAKE_MAX_PENDING];
static int fake_canceled[FAKE_MAX_PENDING];
static FakeInput fake_inputs[FAKE_MAX_INPUTS];
static int fake_input_count;
static int fake_interrupted;
static int fake_alternate_result;
static int fake_next_write_result;
static int fake_hold_writes;
static libusb_device_handle *fake_writing[FAKE_MAX_WRITERS]; /* The handle of each OUT transfer that runs */
static FakeLibUSB_Counts fake_counts;
static FakeLibUSB_Write fake_writes[FAKE_LIBUSB_MAX_WRITES];
static FakeAnswer fake_answers[FAKE_MAX_ANSWERS];
static int fake_answer_count;
static FakeLibUSB_Control fake_controls[FAKE_LIBUSB_MAX_CONTROLS];
static int fake_unexpected;

static double FakeNowMs(void)
{
    LARGE_INTEGER counter, frequency;

    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

static void FakeUnexpected(void)
{
    AcquireSRWLockExclusive(&fake_lock);
    ++fake_unexpected;
    ReleaseSRWLockExclusive(&fake_lock);
}

/* The next finished transfer, taken off the pending list: a canceled one
   first, then an IN transfer with a packet to fill it. Called locked. */
static struct libusb_transfer *FakeTakeFinished(void)
{
    int i;

    for (i = 0; i < FAKE_MAX_PENDING; ++i) {
        if (fake_pending[i] && fake_canceled[i]) {
            struct libusb_transfer *transfer = fake_pending[i];
            fake_pending[i] = NULL;
            fake_canceled[i] = 0;
            transfer->status = LIBUSB_TRANSFER_CANCELLED;
            transfer->actual_length = 0;
            return transfer;
        }
    }
    if (fake_input_count == 0) {
        return NULL;
    }
    for (i = 0; i < FAKE_MAX_PENDING; ++i) {
        struct libusb_transfer *transfer = fake_pending[i];
        if (transfer && (transfer->endpoint & LIBUSB_ENDPOINT_IN)) {
            const int length = (fake_inputs[0].length < transfer->length) ? fake_inputs[0].length : transfer->length;

            memcpy(transfer->buffer, fake_inputs[0].data, (size_t)length);
            transfer->actual_length = length;
            transfer->status = LIBUSB_TRANSFER_COMPLETED;
            fake_pending[i] = NULL;
            --fake_input_count;
            memmove(&fake_inputs[0], &fake_inputs[1], (size_t)fake_input_count * sizeof(fake_inputs[0]));
            return transfer;
        }
    }
    return NULL;
}

/* One round of event handling: at most one callback, run unlocked, after a
   wait of at most FAKE_WAIT_MS for work */
static int FakeHandleEvents(int *completed)
{
    struct libusb_transfer *finished;

    AcquireSRWLockExclusive(&fake_lock);
    finished = FakeTakeFinished();
    if (!finished && !fake_interrupted && !(completed && *completed)) {
        SleepConditionVariableSRW(&fake_wake, &fake_lock, FAKE_WAIT_MS, 0);
        finished = FakeTakeFinished();
    }
    fake_interrupted = 0;
    ReleaseSRWLockExclusive(&fake_lock);

    if (finished) {
        finished->callback(finished);
    }
    return 0;
}

/* The test's controls */

void FakeLibUSB_Reset(void)
{
    AcquireSRWLockExclusive(&fake_lock);
    fake_input_count = 0;
    fake_alternate_result = 0;
    fake_next_write_result = 0;
    fake_hold_writes = 0;
    fake_unexpected = 0;
    fake_answer_count = 0;
    memset(&fake_counts, 0, sizeof(fake_counts));
    memset(fake_writes, 0, sizeof(fake_writes));
    memset(fake_controls, 0, sizeof(fake_controls));
    WakeAllConditionVariable(&fake_wake);
    ReleaseSRWLockExclusive(&fake_lock);
}

void FakeLibUSB_HoldWrites(int hold)
{
    AcquireSRWLockExclusive(&fake_lock);
    fake_hold_writes = hold;
    WakeAllConditionVariable(&fake_wake);
    ReleaseSRWLockExclusive(&fake_lock);
}

void FakeLibUSB_SetAlternateResult(int result)
{
    AcquireSRWLockExclusive(&fake_lock);
    fake_alternate_result = result;
    ReleaseSRWLockExclusive(&fake_lock);
}

void FakeLibUSB_FailNextWrite(int result)
{
    AcquireSRWLockExclusive(&fake_lock);
    fake_next_write_result = result;
    ReleaseSRWLockExclusive(&fake_lock);
}

int FakeLibUSB_QueueInput(const unsigned char *data, int length)
{
    int queued = 0;

    AcquireSRWLockExclusive(&fake_lock);
    if (fake_input_count < FAKE_MAX_INPUTS && length > 0 && length <= FAKE_INPUT_SIZE) {
        fake_inputs[fake_input_count].length = length;
        memcpy(fake_inputs[fake_input_count].data, data, (size_t)length);
        ++fake_input_count;
        queued = 1;
        WakeAllConditionVariable(&fake_wake);
    }
    ReleaseSRWLockExclusive(&fake_lock);
    return queued;
}

void FakeLibUSB_GetCounts(FakeLibUSB_Counts *counts)
{
    int i;

    AcquireSRWLockExclusive(&fake_lock);
    *counts = fake_counts;
    counts->inputs = fake_input_count;
    counts->writing = 0;
    for (i = 0; i < FAKE_MAX_WRITERS; ++i) {
        if (fake_writing[i]) {
            ++counts->writing;
        }
    }
    ReleaseSRWLockExclusive(&fake_lock);
}

int FakeLibUSB_GetWrite(int index, FakeLibUSB_Write *write)
{
    int found = 0;

    AcquireSRWLockExclusive(&fake_lock);
    if (index >= 0 && index < fake_counts.writes && index < FAKE_LIBUSB_MAX_WRITES) {
        *write = fake_writes[index];
        found = 1;
    }
    ReleaseSRWLockExclusive(&fake_lock);
    return found;
}

int FakeLibUSB_SetControl(unsigned char request_type, unsigned char request, int result, const unsigned char *reply, int delay_ms)
{
    int i, set = 0;

    if (result > FAKE_ANSWER_SIZE || (result > 0 && !reply)) {
        return 0;
    }
    AcquireSRWLockExclusive(&fake_lock);
    for (i = 0; i < fake_answer_count; ++i) {
        if (fake_answers[i].request_type == request_type && fake_answers[i].request == request) {
            break;
        }
    }
    if (i < FAKE_MAX_ANSWERS) {
        FakeAnswer *answer = &fake_answers[i];

        memset(answer, 0, sizeof(*answer));
        answer->request_type = request_type;
        answer->request = request;
        answer->result = result;
        answer->delay_ms = delay_ms;
        if (result > 0) {
            memcpy(answer->reply, reply, (size_t)result);
        }
        if (i == fake_answer_count) {
            ++fake_answer_count;
        }
        set = 1;
    }
    ReleaseSRWLockExclusive(&fake_lock);
    return set;
}

int FakeLibUSB_GetControl(int index, FakeLibUSB_Control *control)
{
    int found = 0;

    AcquireSRWLockExclusive(&fake_lock);
    if (index >= 0 && index < fake_counts.controls && index < FAKE_LIBUSB_MAX_CONTROLS) {
        *control = fake_controls[index];
        found = 1;
    }
    ReleaseSRWLockExclusive(&fake_lock);
    return found;
}

int FakeLibUSB_GetUnexpected(void)
{
    int unexpected;

    AcquireSRWLockExclusive(&fake_lock);
    unexpected = fake_unexpected;
    ReleaseSRWLockExclusive(&fake_lock);
    return unexpected;
}

/* libusb */

int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
    if (ctx) {
        *ctx = &fake_context;
    }
    return LIBUSB_SUCCESS;
}

void LIBUSB_CALL libusb_exit(libusb_context *ctx)
{
    (void)ctx;
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    libusb_device **devices = (libusb_device **)calloc(3, sizeof(*devices));

    (void)ctx;
    if (!devices) {
        return LIBUSB_ERROR_NO_MEM;
    }
    devices[0] = &fake_device;
    devices[1] = &fake_dji_device;
    *list = devices;
    return 2;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref_devices)
{
    (void)unref_devices;
    free(list);
}

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc)
{
    *desc = (dev == &fake_dji_device) ? fake_dji_device_descriptor : fake_device_descriptor;
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_get_string_descriptor_ascii(libusb_device_handle *dev_handle, uint8_t desc_index, unsigned char *data, int length)
{
    (void)dev_handle;
    (void)desc_index;
    (void)data;
    (void)length;
    FakeUnexpected();
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device *dev, struct libusb_config_descriptor **config)
{
    *config = (struct libusb_config_descriptor *)((dev == &fake_dji_device) ? &fake_dji_config : &fake_config);
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_get_config_descriptor(libusb_device *dev, uint8_t config_index, struct libusb_config_descriptor **config)
{
    if (config_index != 0) {
        return LIBUSB_ERROR_NOT_FOUND;
    }
    *config = (struct libusb_config_descriptor *)((dev == &fake_dji_device) ? &fake_dji_config : &fake_config);
    return LIBUSB_SUCCESS;
}

void LIBUSB_CALL libusb_free_config_descriptor(struct libusb_config_descriptor *config)
{
    /* The configuration is static */
    (void)config;
}

uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *dev)
{
    (void)dev;
    return 1;
}

int LIBUSB_CALL libusb_get_port_numbers(libusb_device *dev, uint8_t *port_numbers, int port_numbers_len)
{
    if (port_numbers_len < 1) {
        return LIBUSB_ERROR_OVERFLOW;
    }
    port_numbers[0] = (dev == &fake_dji_device) ? 4 : 3;
    return 1;
}

uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *dev)
{
    return (dev == &fake_dji_device) ? 6 : 5;
}

int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **dev_handle)
{
    libusb_device_handle *handle = (libusb_device_handle *)calloc(1, sizeof(*handle));

    if (!handle) {
        return LIBUSB_ERROR_NO_MEM;
    }
    handle->device = dev;
    AcquireSRWLockExclusive(&fake_lock);
    ++fake_counts.opens;
    ReleaseSRWLockExclusive(&fake_lock);
    *dev_handle = handle;
    return LIBUSB_SUCCESS;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *dev_handle)
{
    int i;

    AcquireSRWLockExclusive(&fake_lock);
    ++fake_counts.closes;
    /* libusb forbids closing a handle while a transfer on it runs. The
       transfer only compares the handle afterward, so freeing it here
       stays safe inside the fake. */
    for (i = 0; i < FAKE_MAX_WRITERS; ++i) {
        if (fake_writing[i] == dev_handle) {
            ++fake_counts.writing_closes;
            ++fake_unexpected;
        }
    }
    ReleaseSRWLockExclusive(&fake_lock);
    free(dev_handle);
}

libusb_device *LIBUSB_CALL libusb_get_device(libusb_device_handle *dev_handle)
{
    return dev_handle->device;
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *dev_handle, int interface_number)
{
    (void)dev_handle;
    (void)interface_number;
    AcquireSRWLockExclusive(&fake_lock);
    ++fake_counts.claims;
    ReleaseSRWLockExclusive(&fake_lock);
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *dev_handle, int interface_number)
{
    (void)dev_handle;
    (void)interface_number;
    AcquireSRWLockExclusive(&fake_lock);
    ++fake_counts.releases;
    ReleaseSRWLockExclusive(&fake_lock);
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_kernel_driver_active(libusb_device_handle *dev_handle, int interface_number)
{
    /* No kernel driver holds an interface, so nothing is detached */
    (void)dev_handle;
    (void)interface_number;
    return 0;
}

int LIBUSB_CALL libusb_detach_kernel_driver(libusb_device_handle *dev_handle, int interface_number)
{
    (void)dev_handle;
    (void)interface_number;
    FakeUnexpected();
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_attach_kernel_driver(libusb_device_handle *dev_handle, int interface_number)
{
    (void)dev_handle;
    (void)interface_number;
    FakeUnexpected();
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(libusb_device_handle *dev_handle, int enable)
{
    (void)dev_handle;
    (void)enable;
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *dev_handle, int interface_number, int alternate_setting)
{
    int result;

    (void)dev_handle;
    (void)interface_number;
    AcquireSRWLockExclusive(&fake_lock);
    ++fake_counts.alternates;
    fake_counts.last_alternate = alternate_setting;
    result = (alternate_setting != 0) ? fake_alternate_result : LIBUSB_SUCCESS;
    ReleaseSRWLockExclusive(&fake_lock);
    return result;
}

struct libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    const size_t size = sizeof(struct libusb_transfer) + (size_t)iso_packets * sizeof(struct libusb_iso_packet_descriptor);
    struct libusb_transfer *transfer = (struct libusb_transfer *)calloc(1, size);

    if (transfer) {
        transfer->num_iso_packets = iso_packets;
    }
    return transfer;
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
    int i, result = LIBUSB_ERROR_NO_MEM;

    AcquireSRWLockExclusive(&fake_lock);
    for (i = 0; i < FAKE_MAX_PENDING; ++i) {
        if (fake_pending[i] == transfer) {
            result = LIBUSB_ERROR_BUSY;
            break;
        }
    }
    if (i == FAKE_MAX_PENDING) {
        for (i = 0; i < FAKE_MAX_PENDING; ++i) {
            if (!fake_pending[i]) {
                fake_pending[i] = transfer;
                fake_canceled[i] = 0;
                ++fake_counts.submits;
                result = LIBUSB_SUCCESS;
                WakeAllConditionVariable(&fake_wake);
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&fake_lock);
    return result;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    int i, result = LIBUSB_ERROR_NOT_FOUND;

    AcquireSRWLockExclusive(&fake_lock);
    for (i = 0; i < FAKE_MAX_PENDING; ++i) {
        if (fake_pending[i] == transfer) {
            fake_canceled[i] = 1;
            result = LIBUSB_SUCCESS;
            WakeAllConditionVariable(&fake_wake);
            break;
        }
    }
    ReleaseSRWLockExclusive(&fake_lock);
    return result;
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{
    int i;

    AcquireSRWLockExclusive(&fake_lock);
    for (i = 0; i < FAKE_MAX_PENDING; ++i) {
        if (fake_pending[i] == transfer) {
            /* libusb forbids freeing a pending transfer */
            fake_pending[i] = NULL;
            ++fake_unexpected;
        }
    }
    ReleaseSRWLockExclusive(&fake_lock);
    free(transfer);
}

/* Answers after the answer's delay, with the lock released meanwhile, as a
   device takes its time on the control pipe. A request the test did not set
   fails as unsupported. */
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *dev_handle, uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                                        unsigned char *data, uint16_t wLength, unsigned int timeout)
{
    FakeAnswer answer = { 0 };
    const double arrived = FakeNowMs();
    int i, record, result, answered = 0;

    (void)dev_handle;
    AcquireSRWLockExclusive(&fake_lock);
    for (i = 0; i < fake_answer_count; ++i) {
        if (fake_answers[i].request_type == request_type && fake_answers[i].request == bRequest) {
            answer = fake_answers[i];
            answered = 1;
            break;
        }
    }
    if (!answered) {
        ++fake_unexpected;
    }
    record = fake_counts.controls++;
    if (record < FAKE_LIBUSB_MAX_CONTROLS) {
        FakeLibUSB_Control *control = &fake_controls[record];

        control->request_type = request_type;
        control->request = bRequest;
        control->value = wValue;
        control->index = wIndex;
        control->length = wLength;
        control->timeout = timeout;
        control->ms = arrived;
    }
    ReleaseSRWLockExclusive(&fake_lock);

    if (!answered) {
        result = LIBUSB_ERROR_NOT_SUPPORTED;
    } else {
        if (answer.delay_ms > 0) {
            Sleep((DWORD)answer.delay_ms);
        }
        result = answer.result;
        if (result > wLength) {
            result = wLength;
        }
        if (result > 0 && (request_type & LIBUSB_ENDPOINT_IN) && data) {
            memcpy(data, answer.reply, (size_t)result);
        }
    }

    AcquireSRWLockExclusive(&fake_lock);
    /* A reset meanwhile dropped the record */
    if (record < fake_counts.controls && record < FAKE_LIBUSB_MAX_CONTROLS) {
        fake_controls[record].result = result;
        fake_controls[record].done_ms = FakeNowMs();
    }
    ReleaseSRWLockExclusive(&fake_lock);
    return result;
}

/* An interrupt or bulk OUT transfer, recorded in order. While the test
   holds writes it waits, as on an endpoint that refuses data, until the test
   lets writes go or its timeout passes. A timeout moves no bytes and returns
   LIBUSB_ERROR_TIMEOUT, and a timeout of 0 waits without limit, as in
   libusb. */
static int FakeWrite(libusb_device_handle *dev_handle, unsigned char endpoint, const unsigned char *data, int length,
                     int *actual_length, unsigned int timeout)
{
    const double start = FakeNowMs();
    FakeLibUSB_Write *write = NULL;
    int slot = -1;
    int result, i;

    AcquireSRWLockExclusive(&fake_lock);
    result = fake_next_write_result;
    fake_next_write_result = 0;
    if (fake_counts.writes < FAKE_LIBUSB_MAX_WRITES) {
        write = &fake_writes[fake_counts.writes];
        write->endpoint = endpoint;
        write->length = length;
        write->timeout = timeout;
        memcpy(write->data, data, (size_t)((length < FAKE_LIBUSB_MAX_DATA) ? length : FAKE_LIBUSB_MAX_DATA));
        write->ms = start;
    }
    ++fake_counts.writes;
    for (i = 0; i < FAKE_MAX_WRITERS; ++i) {
        if (!fake_writing[i]) {
            fake_writing[i] = dev_handle;
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ++fake_unexpected;
    }
    if (result == LIBUSB_SUCCESS && fake_hold_writes) {
        ++fake_counts.held;
        while (fake_hold_writes) {
            const double waited = FakeNowMs() - start;

            if (timeout && waited >= (double)timeout) {
                result = LIBUSB_ERROR_TIMEOUT;
                break;
            }
            SleepConditionVariableSRW(&fake_wake, &fake_lock, timeout ? (DWORD)((double)timeout - waited) + 1 : INFINITE, 0);
        }
    }
    if (write) {
        write->result = result;
        write->done_ms = FakeNowMs();
    }
    if (slot >= 0) {
        fake_writing[slot] = NULL;
    }
    ReleaseSRWLockExclusive(&fake_lock);
    if (actual_length) {
        *actual_length = (result == LIBUSB_SUCCESS) ? length : 0;
    }
    return result;
}

int LIBUSB_CALL libusb_interrupt_transfer(libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *data, int length,
                                          int *actual_length, unsigned int timeout)
{
    if (endpoint & LIBUSB_ENDPOINT_IN) {
        /* The model reads only through submitted transfers */
        FakeUnexpected();
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    return FakeWrite(dev_handle, endpoint, data, length, actual_length, timeout);
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *data, int length,
                                     int *actual_length, unsigned int timeout)
{
    if (endpoint & LIBUSB_ENDPOINT_IN) {
        /* The model reads only through submitted transfers */
        if (actual_length) {
            *actual_length = 0;
        }
        FakeUnexpected();
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    return FakeWrite(dev_handle, endpoint, data, length, actual_length, timeout);
}

int LIBUSB_CALL libusb_handle_events(libusb_context *ctx)
{
    (void)ctx;
    return FakeHandleEvents(NULL);
}

int LIBUSB_CALL libusb_handle_events_completed(libusb_context *ctx, int *completed)
{
    (void)ctx;
    return FakeHandleEvents(completed);
}

void LIBUSB_CALL libusb_interrupt_event_handler(libusb_context *ctx)
{
    (void)ctx;
    AcquireSRWLockExclusive(&fake_lock);
    fake_interrupted = 1;
    WakeAllConditionVariable(&fake_wake);
    ReleaseSRWLockExclusive(&fake_lock);
}

int LIBUSB_CALL libusb_has_capability(uint32_t capability)
{
    /* No hotplug, so SDL registers no hotplug callback */
    (void)capability;
    return 0;
}

int LIBUSB_CALL libusb_hotplug_register_callback(libusb_context *ctx, int events, int flags, int vendor_id, int product_id, int dev_class,
                                                 libusb_hotplug_callback_fn cb_fn, void *user_data, libusb_hotplug_callback_handle *callback_handle)
{
    (void)ctx;
    (void)events;
    (void)flags;
    (void)vendor_id;
    (void)product_id;
    (void)dev_class;
    (void)cb_fn;
    (void)user_data;
    (void)callback_handle;
    FakeUnexpected();
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

void LIBUSB_CALL libusb_hotplug_deregister_callback(libusb_context *ctx, libusb_hotplug_callback_handle callback_handle)
{
    (void)ctx;
    (void)callback_handle;
}

const char *LIBUSB_CALL libusb_error_name(int errcode)
{
    (void)errcode;
    return "fake libusb error";
}
