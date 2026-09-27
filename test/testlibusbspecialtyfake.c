/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* A fake libusb-1.0.dll for testlibusbspecialty.c in test/libusb-backend.
   It exports every function SDL_InitLibUSB loads (src/misc/SDL_libusb.c)
   and serves the bulk devices that testlibusbspecialtyfake.h describes, one
   at a time. It never reaches a real device. Transfers submitted for the IN
   endpoint complete from libusb_handle_events: the packets the test queues
   fill them as a host controller fills a transfer, a transfer whose timeout
   passes completes as timed out with the bytes it holds, and a canceled
   transfer completes as canceled, as libusb reports each. OUT transfers are
   synchronous and recorded. Calls the model does not serve count as
   unexpected. */

#include <libusb.h>

#include <stdlib.h>
#include <string.h>

#include "testlibusbspecialtyfake.h"

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

/* A Tacx T1904 head unit, constructed: bulk IN 0x82 and bulk OUT 0x02, the
   endpoints FortiusANT and antifier use, with 16-byte packets. The class,
   bcdDevice and MaxPower are placeholders. */
static const struct libusb_endpoint_descriptor fake_tacx_endpoints[] = {
    { 7, LIBUSB_DT_ENDPOINT, 0x82, LIBUSB_TRANSFER_TYPE_BULK, 16, 0, 0, 0, NULL, 0 },
    { 7, LIBUSB_DT_ENDPOINT, 0x02, LIBUSB_TRANSFER_TYPE_BULK, 16, 0, 0, 0, NULL, 0 },
};

static const struct libusb_interface_descriptor fake_tacx_interface0[] = {
    { 9, LIBUSB_DT_INTERFACE, 0, 0, 2, LIBUSB_CLASS_VENDOR_SPEC, 0, 0, 0, fake_tacx_endpoints, NULL, 0 },
};

static const struct libusb_interface fake_tacx_interfaces[] = {
    { fake_tacx_interface0, 1 },
};

static const struct libusb_config_descriptor fake_tacx_config = {
    9, LIBUSB_DT_CONFIG, 32, 1, 1, 0, 0x80, 50, fake_tacx_interfaces, NULL, 0
};

static const struct libusb_device_descriptor fake_tacx_device_descriptor = {
    18, LIBUSB_DT_DEVICE, 0x0110, 0, 0, 0, 8, 0x3561, 0x1904, 0x0100, 0, 0, 0, 1
};

/* The Namco USIO as RPCS3 describes it (rpcs3/Emu/Io/usio.cpp:68-122),
   without its two string descriptors */
static const struct libusb_endpoint_descriptor fake_usio_endpoints[] = {
    { 7, LIBUSB_DT_ENDPOINT, 0x01, LIBUSB_TRANSFER_TYPE_BULK, 64, 0, 0, 0, NULL, 0 },
    { 7, LIBUSB_DT_ENDPOINT, 0x82, LIBUSB_TRANSFER_TYPE_BULK, 64, 0, 0, 0, NULL, 0 },
    { 7, LIBUSB_DT_ENDPOINT, 0x83, LIBUSB_TRANSFER_TYPE_INTERRUPT, 8, 16, 0, 0, NULL, 0 },
};

static const struct libusb_interface_descriptor fake_usio_interface0[] = {
    { 9, LIBUSB_DT_INTERFACE, 0, 0, 3, 0, 0, 0, 0, fake_usio_endpoints, NULL, 0 },
};

static const struct libusb_interface fake_usio_interfaces[] = {
    { fake_usio_interface0, 1 },
};

static const struct libusb_config_descriptor fake_usio_config = {
    9, LIBUSB_DT_CONFIG, 39, 1, 1, 0, 0xC0, 0x32, fake_usio_interfaces, NULL, 0
};

static const struct libusb_device_descriptor fake_usio_device_descriptor = {
    18, LIBUSB_DT_DEVICE, 0x0110, 0xFF, 0x00, 0xFF, 8, 0x0B9A, 0x0910, 0x0910, 0, 0, 0, 1
};

typedef struct FakeDevice
{
    const struct libusb_device_descriptor *descriptor;
    const struct libusb_config_descriptor *config;
    int in_packet_size; /* wMaxPacketSize of the IN endpoint SDL reads */
} FakeDevice;

/* Indexed by FAKE_SPECIALTY_* */
static const FakeDevice fake_devices[] = {
    { &fake_tacx_device_descriptor, &fake_tacx_config, 16 },
    { &fake_usio_device_descriptor, &fake_usio_config, 64 },
};

static struct libusb_context fake_context;
static struct libusb_device fake_device;

typedef struct FakeInput
{
    int length;
    unsigned char data[FAKE_INPUT_SIZE];
} FakeInput;

/* Everything below is guarded by fake_lock */
static SRWLOCK fake_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE fake_wake = CONDITION_VARIABLE_INIT;
static struct libusb_transfer *fake_pending[FAKE_MAX_PENDING];
static int fake_canceled[FAKE_MAX_PENDING];
static double fake_submitted[FAKE_MAX_PENDING]; /* When each pending transfer was submitted */
static FakeInput fake_inputs[FAKE_MAX_INPUTS];
static int fake_input_count;
static int fake_interrupted;
static int fake_selected;
static int fake_naks;
static int fake_writing; /* OUT transfers in progress */
static FakeSpecialty_Counts fake_counts;
static FakeSpecialty_Write fake_writes[FAKE_SPECIALTY_MAX_WRITES];
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
   first, then an IN transfer that the queued packets end, then an IN
   transfer whose timeout passed. A packet shorter than the endpoint's
   packet size, or one that fills the buffer, ends the transfer it lands
   in. The packets queued before the timeout land first, so a timed-out
   transfer holds every byte that came before it. libusb cancels a transfer
   whose timeout passed and reports the bytes that arrived
   (os/windows_common.c, ERROR_OPERATION_ABORTED, and io.c,
   usbi_handle_transfer_cancellation). Called locked. */
static struct libusb_transfer *FakeTakeFinished(void)
{
    const double now = FakeNowMs();
    const int packet_size = fake_devices[fake_selected].in_packet_size;
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
    for (i = 0; i < FAKE_MAX_PENDING; ++i) {
        struct libusb_transfer *transfer = fake_pending[i];

        if (!transfer || !(transfer->endpoint & LIBUSB_ENDPOINT_IN)) {
            continue;
        }
        while (fake_input_count > 0) {
            const int room = transfer->length - transfer->actual_length;
            const int length = fake_inputs[0].length;
            const int taken = (length < room) ? length : room;
            const int ends = (length < packet_size || length >= room);

            if (length > room) {
                /* libusb reports an overflow, which the model does not serve */
                ++fake_unexpected;
            }
            memcpy(transfer->buffer + transfer->actual_length, fake_inputs[0].data, (size_t)taken);
            transfer->actual_length += taken;
            --fake_input_count;
            memmove(&fake_inputs[0], &fake_inputs[1], (size_t)fake_input_count * sizeof(fake_inputs[0]));
            if (ends) {
                transfer->status = LIBUSB_TRANSFER_COMPLETED;
                fake_pending[i] = NULL;
                return transfer;
            }
        }
        if (transfer->timeout && now - fake_submitted[i] >= (double)transfer->timeout) {
            transfer->status = LIBUSB_TRANSFER_TIMED_OUT;
            fake_pending[i] = NULL;
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

/* An OUT transfer, interrupt or bulk, made synchronous and recorded. A
   NAKed one waits out its timeout outside the lock, as libusb's synchronous
   transfers wait on an endpoint that NAKs every packet. */
static int FakeWrite(unsigned char endpoint, const unsigned char *data, int length, int *actual_length, unsigned int timeout)
{
    int result, nak;

    AcquireSRWLockExclusive(&fake_lock);
    nak = (fake_naks > 0);
    if (nak) {
        --fake_naks;
    }
    result = nak ? LIBUSB_ERROR_TIMEOUT : LIBUSB_SUCCESS;
    if (fake_counts.writes < FAKE_SPECIALTY_MAX_WRITES) {
        FakeSpecialty_Write *write = &fake_writes[fake_counts.writes];
        write->endpoint = endpoint;
        write->length = length;
        write->timeout = timeout;
        write->result = result;
        memcpy(write->data, data, (size_t)((length < FAKE_SPECIALTY_MAX_DATA) ? length : FAKE_SPECIALTY_MAX_DATA));
        write->ms = FakeNowMs();
    }
    ++fake_counts.writes;
    ++fake_writing;
    ReleaseSRWLockExclusive(&fake_lock);

    if (nak) {
        Sleep(timeout);
    }

    AcquireSRWLockExclusive(&fake_lock);
    --fake_writing;
    ReleaseSRWLockExclusive(&fake_lock);
    if (actual_length) {
        *actual_length = (result == LIBUSB_SUCCESS) ? length : 0;
    }
    return result;
}

/* The test's controls */

void FakeSpecialty_Reset(void)
{
    AcquireSRWLockExclusive(&fake_lock);
    fake_input_count = 0;
    fake_selected = FAKE_SPECIALTY_TACX;
    fake_naks = 0;
    fake_unexpected = 0;
    memset(&fake_counts, 0, sizeof(fake_counts));
    memset(fake_writes, 0, sizeof(fake_writes));
    ReleaseSRWLockExclusive(&fake_lock);
}

void FakeSpecialty_SelectDevice(int device)
{
    AcquireSRWLockExclusive(&fake_lock);
    if (device >= 0 && device < (int)(sizeof(fake_devices) / sizeof(fake_devices[0]))) {
        fake_selected = device;
    }
    ReleaseSRWLockExclusive(&fake_lock);
}

void FakeSpecialty_NakWrites(int count)
{
    AcquireSRWLockExclusive(&fake_lock);
    fake_naks = count;
    ReleaseSRWLockExclusive(&fake_lock);
}

int FakeSpecialty_QueueReply(const unsigned char *data, int length)
{
    int queued = 0;

    AcquireSRWLockExclusive(&fake_lock);
    {
        const int packet_size = fake_devices[fake_selected].in_packet_size;
        const int packets = (length + packet_size - 1) / packet_size;

        if (length > 0 && packet_size <= FAKE_INPUT_SIZE && fake_input_count + packets <= FAKE_MAX_INPUTS) {
            int offset;

            for (offset = 0; offset < length; offset += packet_size) {
                const int size = (length - offset < packet_size) ? length - offset : packet_size;

                fake_inputs[fake_input_count].length = size;
                memcpy(fake_inputs[fake_input_count].data, data + offset, (size_t)size);
                ++fake_input_count;
            }
            queued = 1;
            WakeAllConditionVariable(&fake_wake);
        }
    }
    ReleaseSRWLockExclusive(&fake_lock);
    return queued;
}

void FakeSpecialty_GetCounts(FakeSpecialty_Counts *counts)
{
    AcquireSRWLockExclusive(&fake_lock);
    *counts = fake_counts;
    counts->inputs = fake_input_count;
    ReleaseSRWLockExclusive(&fake_lock);
}

int FakeSpecialty_GetWrite(int index, FakeSpecialty_Write *write)
{
    int found = 0;

    AcquireSRWLockExclusive(&fake_lock);
    if (index >= 0 && index < fake_counts.writes && index < FAKE_SPECIALTY_MAX_WRITES) {
        *write = fake_writes[index];
        found = 1;
    }
    ReleaseSRWLockExclusive(&fake_lock);
    return found;
}

int FakeSpecialty_GetUnexpected(void)
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
    libusb_device **devices = (libusb_device **)calloc(2, sizeof(*devices));

    (void)ctx;
    if (!devices) {
        return LIBUSB_ERROR_NO_MEM;
    }
    devices[0] = &fake_device;
    *list = devices;
    return 1;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref_devices)
{
    (void)unref_devices;
    free(list);
}

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc)
{
    (void)dev;
    AcquireSRWLockExclusive(&fake_lock);
    *desc = *fake_devices[fake_selected].descriptor;
    ReleaseSRWLockExclusive(&fake_lock);
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
    (void)dev;
    AcquireSRWLockExclusive(&fake_lock);
    *config = (struct libusb_config_descriptor *)fake_devices[fake_selected].config;
    ReleaseSRWLockExclusive(&fake_lock);
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_get_config_descriptor(libusb_device *dev, uint8_t config_index, struct libusb_config_descriptor **config)
{
    (void)dev;
    if (config_index != 0) {
        return LIBUSB_ERROR_NOT_FOUND;
    }
    AcquireSRWLockExclusive(&fake_lock);
    *config = (struct libusb_config_descriptor *)fake_devices[fake_selected].config;
    ReleaseSRWLockExclusive(&fake_lock);
    return LIBUSB_SUCCESS;
}

void LIBUSB_CALL libusb_free_config_descriptor(struct libusb_config_descriptor *config)
{
    /* The configurations are static */
    (void)config;
}

uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *dev)
{
    (void)dev;
    return 1;
}

int LIBUSB_CALL libusb_get_port_numbers(libusb_device *dev, uint8_t *port_numbers, int port_numbers_len)
{
    (void)dev;
    if (port_numbers_len < 1) {
        return LIBUSB_ERROR_OVERFLOW;
    }
    port_numbers[0] = 3;
    return 1;
}

uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *dev)
{
    (void)dev;
    return 5;
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
    AcquireSRWLockExclusive(&fake_lock);
    ++fake_counts.closes;
    if (fake_writing > 0) {
        /* libusb forbids closing a handle that a transfer still uses */
        ++fake_counts.closes_during_writes;
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
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *dev_handle, int interface_number)
{
    (void)dev_handle;
    (void)interface_number;
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
    /* Both devices have one alternate setting, which SDL never selects */
    (void)dev_handle;
    (void)interface_number;
    (void)alternate_setting;
    FakeUnexpected();
    return LIBUSB_SUCCESS;
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
                fake_submitted[i] = FakeNowMs();
                transfer->actual_length = 0;
                if (transfer->endpoint & LIBUSB_ENDPOINT_IN) {
                    fake_counts.in_timeout = (int)transfer->timeout;
                }
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

int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *dev_handle, uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                                        unsigned char *data, uint16_t wLength, unsigned int timeout)
{
    (void)dev_handle;
    (void)request_type;
    (void)bRequest;
    (void)wValue;
    (void)wIndex;
    (void)data;
    (void)wLength;
    (void)timeout;
    FakeUnexpected();
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_interrupt_transfer(libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *data, int length,
                                          int *actual_length, unsigned int timeout)
{
    (void)dev_handle;
    if (endpoint & LIBUSB_ENDPOINT_IN) {
        /* The model reads only through submitted transfers */
        if (actual_length) {
            *actual_length = 0;
        }
        FakeUnexpected();
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    return FakeWrite(endpoint, data, length, actual_length, timeout);
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *data, int length,
                                     int *actual_length, unsigned int timeout)
{
    (void)dev_handle;
    if (endpoint & LIBUSB_ENDPOINT_IN) {
        /* The model reads only through submitted transfers */
        if (actual_length) {
            *actual_length = 0;
        }
        FakeUnexpected();
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    return FakeWrite(endpoint, data, length, actual_length, timeout);
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
