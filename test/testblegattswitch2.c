/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* The Switch 2 driver, SDL_BLE_JoystickDriver, for testblegattdriver: the
   tree's driver with every transport function renamed to the fake transport
   (testblegattfake.h). The driver table in the static SDL binds to this copy,
   so the Switch 2 driver in the test never reaches SDL's transport or a radio
   (see testblegattdriver.c).

   This file also holds the scripted Switch 2 that answers the driver's
   transport calls (testblegattswitch2.h), for hifihedgehog/SDL#38. It hands
   the driver fake WinRT objects whose vtables answer the calls the driver
   makes on them, counts every reference the driver takes and drops, records
   every write, and answers commands as the references describe the devices:

   - A command frame is [cmd] 91 01 [sub] 00 [len] 00 00 [data]
     (switch2_controller_research commands.md "Command Header"). Its reply
     is [cmd] 01 01 [sub] 10 78 00 00 [data], as the OEM Joy-Con 2 (R) and the
     NYXI Hyperion 3 Ultra answer in the captures of PadForge discussion #491.
     A memory read's reply data is the length, three zero bytes, the address
     and the bytes (switch2-controllers controller.py read_memory, :339-347).
   - A genuine Joy-Con 2 answers commands on 649d4ac9 with replies on
     c765a961, and streams on the unified input once its notifications are on
     (tarabdaar JoyConBLE.swift altFallbackDelay).
   - A NYXI Hyperion 3 never answers 649d4ac9 and never streams on the unified
     input. It answers the console channel, the side's command characteristic
     behind 17 zero bytes (joycon2android ConsoleSession.kt, protocol.md
     "Console-protocol controllers"). The script streams on the side's input
     once the session start, the report-rate descriptor, the input's
     notifications and the feature enable have all come, the steps
     joycon2android takes. Its replies arrive on the side's extended
     response after 15 zero bytes (bluetooth_interface.md handle 0x001E) or
     on c765a961.
   - A Hyperion 3 Ultra also answers 649d4ac9 and sends vendor frames that
     start EA 01 00 8B 00 78 00 00 0C on c765a961 (tarabdaar
     JoyConNYXIReport.swift).
   - A script can put decoys before each reply, notifications that each
     fail one of the driver's reply checks, and can answer one console read
     after SW2_SLOW_REPLY_MS (testblegattswitch2.h).

   Values reach the driver on a thread of the fake's own, as WinRT delivers
   them from its thread pool. It is a Win32 thread, so SDL_Quit never finds
   it among SDL's threads. Every UUID is parsed from its text here, so a
   wrong GUID in the driver finds nothing. */

#include "SDL_internal.h"

/* Every transport function, renamed to the fake */
#include "testblegattfake.h"

/* Always the tree's driver, by its path from this file */
#include "../src/joystick/windows/SDL_ble_switch2joystick.c"

#include "testblegattswitch2.h"

#include <process.h>
#include <stdlib.h>

typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDeviceVtbl SW2DeviceVtbl;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3Vtbl SW2Device3Vtbl;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3Vtbl SW2ServiceVtbl;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicVtbl SW2CharVtbl;

typedef struct SW2Device
{
    const SW2DeviceVtbl *lpVtbl;
    int refs;
} SW2Device;

typedef struct SW2Device3
{
    const SW2Device3Vtbl *lpVtbl;
    int refs;
} SW2Device3;

/* The Switch 2 service, and the session service */
typedef struct SW2Service
{
    const SW2ServiceVtbl *lpVtbl;
    bool session;
    int refs;
} SW2Service;

typedef struct SW2Char
{
    const SW2CharVtbl *lpVtbl;
    int role;
    int refs;
    SDL_BLEGATT_ValueCallback callback;
    void *userdata;
    int index;
    Sint64 token;
    bool notifying;
} SW2Char;

#define SW2_QUEUE_CAPACITY 128
#define SW2_VALUE_MAX      128

typedef struct SW2Delivery
{
    int role;
    Uint8 data[SW2_VALUE_MAX];
    int length;
    ULONGLONG due;
} SW2Delivery;

static struct
{
    bool ready;
    CRITICAL_SECTION lock;
    HANDLE wake;
    HANDLE radio;
    volatile LONG quit;
    volatile LONG helper_calls;

    bool armed;
    SW2Script script;
    GUID uuids[SW2_CHARACTERISTICS];
    GUID service_uuid;
    GUID session_uuid;
    GUID report_rate_uuid;
    GUID device3_iid;
    SW2Device device;
    SW2Device3 device3;
    SW2Service service;
    SW2Service session;
    SW2Char chars[SW2_CHARACTERISTICS];
    SDL_AtomicInt *lost;
    Sint64 next_token;

    bool session_started;
    bool rate_written;
    bool features_enabled;
    int reads;
    int bad_calls;

    SW2Event events[SW2_MAX_EVENTS];
    int nevents;

    SW2Delivery queue[SW2_QUEUE_CAPACITY];
    int head;
    int count;
} sw2;

/* The text form of a UUID, as the references print it */
static GUID SW2_GUID(const char *text)
{
    GUID guid;
    Uint8 bytes[16];
    int n = 0;

    SDL_zero(guid);
    while (*text && n < 32) {
        const char c = *text++;
        int digit;

        if (c == '-') {
            continue;
        }
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return guid;
        }
        if (n % 2 == 0) {
            bytes[n / 2] = (Uint8)(digit << 4);
        } else {
            bytes[n / 2] |= (Uint8)digit;
        }
        ++n;
    }
    guid.Data1 = ((unsigned long)bytes[0] << 24) | ((unsigned long)bytes[1] << 16) | ((unsigned long)bytes[2] << 8) | bytes[3];
    guid.Data2 = (unsigned short)((bytes[4] << 8) | bytes[5]);
    guid.Data3 = (unsigned short)((bytes[6] << 8) | bytes[7]);
    SDL_memcpy(guid.Data4, &bytes[8], 8);
    return guid;
}

static bool SW2_SameGUID(const GUID *a, const GUID *b)
{
    return SDL_memcmp(a, b, sizeof(*a)) == 0;
}

static void SW2_Event(SW2EventKind kind, int characteristic, bool response, bool report_rate, const Uint8 *data, int length)
{
    SW2Event *event;

    if (sw2.nevents >= SW2_MAX_EVENTS) {
        ++sw2.bad_calls;
        return;
    }
    event = &sw2.events[sw2.nevents++];
    SDL_zerop(event);
    event->kind = kind;
    event->characteristic = characteristic;
    event->response = response;
    event->report_rate = report_rate;
    event->ms = SDL_GetTicks();
    if (data && length > 0) {
        event->length = SDL_min(length, (int)sizeof(event->data));
        SDL_memcpy(event->data, data, event->length);
    }
}

/* Queues a value for the radio thread. Called under the lock. */
static void SW2_Queue(int role, const Uint8 *data, int length, int delay_ms)
{
    SW2Delivery *delivery;

    if (sw2.count >= SW2_QUEUE_CAPACITY || length > SW2_VALUE_MAX) {
        ++sw2.bad_calls;
        return;
    }
    delivery = &sw2.queue[(sw2.head + sw2.count) % SW2_QUEUE_CAPACITY];
    delivery->role = role;
    SDL_memcpy(delivery->data, data, length);
    delivery->length = length;
    delivery->due = GetTickCount64() + (ULONGLONG)delay_ms;
    ++sw2.count;
    SetEvent(sw2.wake);
}

/* Delivers the queue in order, each value no sooner than it is due. The
   value is an exact-size copy from the C runtime, so AddressSanitizer sees a
   read past it, as in testblegattdriver.c's DT_Push. */
static unsigned __stdcall SW2_RadioThread(void *unused)
{
    (void)unused;
    for (;;) {
        DWORD wait = INFINITE;

        EnterCriticalSection(&sw2.lock);
        if (sw2.count > 0) {
            SW2Delivery delivery = sw2.queue[sw2.head];
            const ULONGLONG now = GetTickCount64();

            if (now >= delivery.due) {
                SW2Char *characteristic = &sw2.chars[delivery.role];
                SDL_BLEGATT_ValueCallback callback = characteristic->callback;
                void *userdata = characteristic->userdata;
                const int index = characteristic->index;

                sw2.head = (sw2.head + 1) % SW2_QUEUE_CAPACITY;
                --sw2.count;
                LeaveCriticalSection(&sw2.lock);
                if (callback) {
                    Uint8 *copy = (Uint8 *)malloc((size_t)delivery.length);
                    if (copy) {
                        SDL_memcpy(copy, delivery.data, delivery.length);
                        callback(userdata, index, copy, (size_t)delivery.length);
                        free(copy);
                    }
                }
                continue;
            }
            wait = (DWORD)(delivery.due - now);
        } else if (sw2.quit) {
            LeaveCriticalSection(&sw2.lock);
            break;
        }
        LeaveCriticalSection(&sw2.lock);
        WaitForSingleObject(sw2.wake, wait);
    }
    return 0;
}

/* Flash as the scripts hold it: erased, 0xFF, but for the device info
   block of each capture and the stick blocks a script gives */
static void SW2_ReadFlash(Uint32 address, Uint8 *out, int length)
{
    /* The DeviceInfo blocks the captures of PadForge discussion #491 show at
       0x13000: the OEM Joy-Con 2 (R) and the Hyperion 3 Ultra right half */
    static const Uint8 oem_info[] = {
        0x01, 0x00, 0x48, 0x43, 0x57, 0x35, 0x31, 0x30, 0x34, 0x35, 0x35, 0x37, 0x37, 0x36, 0x31, 0x33,
        0x00, 0x00, 0x7e, 0x05, 0x66, 0x20, 0x01, 0x08, 0x02, 0x32, 0x32, 0x32, 0xaa, 0xaa, 0xaa, 0xff,
        0x8c, 0x5f, 0x32, 0x32, 0x32
    };
    static const Uint8 nyxi_info[] = {
        0x01, 0x00, 0x48, 0x43, 0x57, 0x37, 0x31, 0x33, 0x35, 0x37, 0x38, 0x37, 0x30, 0x35, 0x35, 0x36,
        0x00, 0x00, 0x7e, 0x05, 0x66, 0x20, 0x01, 0x08, 0x02, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00
    };
    const struct
    {
        Uint32 address;
        const Uint8 *bytes;
        int length;
    } blocks[] = {
        { 0x13000, sw2.script.kind == SW2_GENUINE ? oem_info : nyxi_info, (int)sizeof(oem_info) },
        { 0x130A8, sw2.script.factory1, 9 },
        { 0x130E8, sw2.script.factory2, 9 },
        { 0x1FC040, sw2.script.user1, 11 },
        { 0x1FC080, sw2.script.user2, 11 }
    };
    int i, b;

    SDL_memset(out, 0xFF, length);
    for (b = 0; b < (int)SDL_arraysize(blocks); ++b) {
        if (!blocks[b].bytes) {
            continue;
        }
        for (i = 0; i < length; ++i) {
            const Uint32 at = address + (Uint32)i;
            if (at >= blocks[b].address && at < blocks[b].address + (Uint32)blocks[b].length) {
                out[i] = blocks[b].bytes[at - blocks[b].address];
            }
        }
    }
}

/* Before the reply to frame, on the reply's characteristic, a notification
   for each thing the driver must check before it takes one as the reply
   (BLE_FindReply), each differing from the reply in that thing alone: the
   command, the status, the subcommand, and for a memory read the length and
   the address. A read's decoys hold 0x5A, which no script's stick block
   holds. Called under the lock. */
static void SW2_QueueDecoys(int target, int prefix, const Uint8 *frame, int length)
{
    const bool read = (frame[0] == 0x02 && frame[3] == 0x04 && length >= 16);
    const Uint8 read_length = read ? frame[8] : 0;
    const Uint32 address = read ? ((Uint32)frame[12] | ((Uint32)frame[13] << 8) | ((Uint32)frame[14] << 16) | ((Uint32)frame[15] << 24)) : 0;
    Uint8 decoy[16 + 0x40 + 16];
    int which;

    for (which = 0; which < (read ? 5 : 3); ++which) {
        Uint8 cmd = frame[0], status = 0x01, sub = frame[3], decoy_length = read_length;
        Uint32 decoy_address = address;
        int size = prefix + 8;

        switch (which) {
        case 0:
            cmd = (Uint8)(cmd ^ 0x01);
            break;
        case 1:
            status = 0x02; /* a refusal, as the OEM Joy-Con 2 (R) answers 03/0A */
            break;
        case 2:
            sub = (Uint8)(sub ^ 0x01);
            break;
        case 3:
            decoy_length = (Uint8)((read_length == 0x40) ? 0x20 : 0x40);
            break;
        default:
            decoy_address += 0x100;
            break;
        }
        SDL_memset(decoy, 0, sizeof(decoy));
        decoy[prefix + 0] = cmd;
        decoy[prefix + 1] = status;
        decoy[prefix + 2] = 0x01;
        decoy[prefix + 3] = sub;
        decoy[prefix + 4] = 0x10;
        decoy[prefix + 5] = 0x78;
        if (read) {
            decoy[prefix + 8] = decoy_length;
            decoy[prefix + 12] = (Uint8)decoy_address;
            decoy[prefix + 13] = (Uint8)(decoy_address >> 8);
            decoy[prefix + 14] = (Uint8)(decoy_address >> 16);
            decoy[prefix + 15] = (Uint8)(decoy_address >> 24);
            SDL_memset(&decoy[prefix + 16], 0x5A, decoy_length);
            size = prefix + 16 + decoy_length;
        }
        SW2_Queue(target, decoy, size, 2);
    }
}

/* Answers one command frame that arrived on channel. Called under the lock. */
static void SW2_Command(int channel, const Uint8 *frame, int length)
{
    static const Uint8 vendor[16] = { 0xEA, 0x01, 0x00, 0x8B, 0x00, 0x78, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x06, 0xCB, 0x1D, 0xF0, 0x00 };
    Uint8 reply[16 + 0x40 + 16];
    int reply_length, prefix, target, delay = 5;
    Uint8 cmd, sub;

    if (length < 8 || frame[1] != 0x91) {
        return; /* not a command frame, such as the raw input-mode write */
    }
    if (frame[5] != length - 8) {
        ++sw2.bad_calls;
        return;
    }
    cmd = frame[0];
    sub = frame[3];
    if (cmd == 0x0C && sub == 0x04) {
        sw2.features_enabled = true;
    }

    target = (channel == SW2_CONSOLE_COMMAND && sw2.script.extended_replies) ? SW2_EXTENDED_RESPONSE : SW2_RESPONSE;
    prefix = (target == SW2_EXTENDED_RESPONSE) ? 15 : 0;
    SDL_memset(reply, 0, sizeof(reply));
    reply[prefix + 0] = cmd;
    reply[prefix + 1] = 0x01;
    reply[prefix + 2] = 0x01;
    reply[prefix + 3] = sub;
    reply[prefix + 4] = 0x10;
    reply[prefix + 5] = 0x78;
    if (cmd == 0x02 && sub == 0x04 && length >= 16) {
        const Uint8 read_length = frame[8];
        const Uint32 address = (Uint32)frame[12] | ((Uint32)frame[13] << 8) | ((Uint32)frame[14] << 16) | ((Uint32)frame[15] << 24);

        if (read_length > 0x40) {
            ++sw2.bad_calls;
            return;
        }
        reply[prefix + 8] = read_length;
        SDL_memcpy(&reply[prefix + 12], &frame[12], 4);
        SW2_ReadFlash(address, &reply[prefix + 16], read_length);
        reply_length = prefix + 16 + read_length;
        ++sw2.reads;
        if (channel == SW2_CONSOLE_COMMAND && sw2.script.slow_console_read && address == sw2.script.slow_console_read) {
            delay = SW2_SLOW_REPLY_MS;
        }
    } else {
        /* A reply with no data is its header alone, as the OEM Joy-Con 2 (R)
           answers 0A/08 in the capture */
        reply_length = prefix + 8;
    }

    if (sw2.script.vendor_before_replies) {
        SW2_Queue(SW2_RESPONSE, vendor, (int)sizeof(vendor), 2);
    }
    if (sw2.script.decoys_before_replies) {
        SW2_QueueDecoys(target, prefix, frame, length);
    }
    SW2_Queue(target, reply, reply_length, delay);
}

/* The vtables of the fake objects. Only the calls the driver makes are
   filled in. */

static HRESULT STDMETHODCALLTYPE SW2_DeviceQueryInterface(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice *This, REFIID riid, void **ppv)
{
    (void)This;
    EnterCriticalSection(&sw2.lock);
    if (SW2_SameGUID(riid, &sw2.device3_iid)) {
        ++sw2.device3.refs;
        *ppv = &sw2.device3;
        LeaveCriticalSection(&sw2.lock);
        return S_OK;
    }
    LeaveCriticalSection(&sw2.lock);
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE SW2_DeviceRelease(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice *This)
{
    ULONG refs;

    (void)This;
    EnterCriticalSection(&sw2.lock);
    if (sw2.device.refs <= 0) {
        ++sw2.bad_calls;
    } else {
        --sw2.device.refs;
    }
    refs = (ULONG)sw2.device.refs;
    LeaveCriticalSection(&sw2.lock);
    return refs;
}

static HRESULT STDMETHODCALLTYPE SW2_DeviceRemoveStatus(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice *This, EventRegistrationToken token)
{
    (void)This;
    (void)token;
    EnterCriticalSection(&sw2.lock);
    sw2.lost = NULL;
    LeaveCriticalSection(&sw2.lock);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE SW2_Device3Release(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3 *This)
{
    ULONG refs;

    (void)This;
    EnterCriticalSection(&sw2.lock);
    if (sw2.device3.refs <= 0) {
        ++sw2.bad_calls;
    } else {
        --sw2.device3.refs;
    }
    refs = (ULONG)sw2.device3.refs;
    LeaveCriticalSection(&sw2.lock);
    return refs;
}

static ULONG STDMETHODCALLTYPE SW2_ServiceRelease(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3 *This)
{
    SW2Service *service = (SW2Service *)This;
    ULONG refs;

    EnterCriticalSection(&sw2.lock);
    if (service->refs <= 0) {
        ++sw2.bad_calls;
    } else {
        --service->refs;
    }
    refs = (ULONG)service->refs;
    LeaveCriticalSection(&sw2.lock);
    return refs;
}

static ULONG STDMETHODCALLTYPE SW2_CharRelease(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic *This)
{
    SW2Char *characteristic = (SW2Char *)This;
    ULONG refs;

    EnterCriticalSection(&sw2.lock);
    if (characteristic->refs <= 0) {
        ++sw2.bad_calls;
    } else {
        --characteristic->refs;
    }
    refs = (ULONG)characteristic->refs;
    LeaveCriticalSection(&sw2.lock);
    return refs;
}

static HRESULT STDMETHODCALLTYPE SW2_CharRemoveValueChanged(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic *This,
                                                            EventRegistrationToken token)
{
    SW2Char *characteristic = (SW2Char *)This;

    EnterCriticalSection(&sw2.lock);
    if (characteristic->callback && characteristic->token == token.value) {
        characteristic->callback = NULL;
        characteristic->userdata = NULL;
    }
    LeaveCriticalSection(&sw2.lock);
    return S_OK;
}

static SW2DeviceVtbl sw2_device_vtbl;
static SW2Device3Vtbl sw2_device3_vtbl;
static SW2ServiceVtbl sw2_service_vtbl;
static SW2CharVtbl sw2_char_vtbl;

void SW2_Setup(void)
{
    InitializeCriticalSection(&sw2.lock);
    sw2.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    sw2_device_vtbl.QueryInterface = SW2_DeviceQueryInterface;
    sw2_device_vtbl.Release = SW2_DeviceRelease;
    sw2_device_vtbl.remove_ConnectionStatusChanged = SW2_DeviceRemoveStatus;
    sw2_device3_vtbl.Release = SW2_Device3Release;
    sw2_service_vtbl.Release = SW2_ServiceRelease;
    sw2_char_vtbl.Release = SW2_CharRelease;
    sw2_char_vtbl.remove_ValueChanged = SW2_CharRemoveValueChanged;
    sw2.ready = true;
}

void SW2_Cleanup(void)
{
    if (sw2.ready) {
        SW2_Disarm();
        CloseHandle(sw2.wake);
        DeleteCriticalSection(&sw2.lock);
        sw2.ready = false;
    }
}

void SW2_Arm(const SW2Script *script)
{
    const bool left = (script->product == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT);
    int i;

    SW2_Disarm();
    EnterCriticalSection(&sw2.lock);
    sw2.script = *script;
    sw2.uuids[SW2_UNIFIED_INPUT] = SW2_GUID("ab7de9be-89fe-49ad-828f-118f09df7fd2");
    sw2.uuids[SW2_COMMAND] = SW2_GUID("649d4ac9-8eb7-4e6c-af44-1ea54fe5f005");
    sw2.uuids[SW2_RESPONSE] = SW2_GUID("c765a961-d9d8-4d36-a20a-5315b111836a");
    switch (script->product) {
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT:
        sw2.uuids[SW2_VIBRATION] = SW2_GUID("289326cb-a471-485d-a8f4-240c14f18241");
        break;
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT:
        sw2.uuids[SW2_VIBRATION] = SW2_GUID("fa19b0fb-cd1f-46a7-84a1-bbb09e00c149");
        break;
    case USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER:
        sw2.uuids[SW2_VIBRATION] = SW2_GUID("3f8fb670-ab25-45bf-b540-38c72834d064");
        break;
    default:
        sw2.uuids[SW2_VIBRATION] = SW2_GUID("cc483f51-9258-427d-a939-630c31f72b05");
        break;
    }
    /* The side's console channel (joycon2android ConsoleChannel.kt) */
    sw2.uuids[SW2_CONSOLE_COMMAND] = SW2_GUID(left ? "ce49a830-dced-48ae-931e-c8cf88aadbea" : "65a724b3-f1e7-4a61-8078-a342376b27ff");
    sw2.uuids[SW2_NATIVE_INPUT] = SW2_GUID(left ? "cc1bbbb5-7354-4d32-a716-a81cb241a32a" : "d5a9e01e-2ffc-4cca-b20c-8b67142bf442");
    sw2.uuids[SW2_EXTENDED_RESPONSE] = SW2_GUID(left ? "63a3810f-aec7-474b-9010-3d52403cb996" : "640ca58e-0e88-410c-a7f3-426faf2b690b");
    sw2.uuids[SW2_SESSION_START] = SW2_GUID("00c5af5d-1964-4e30-8f51-1956f96bd282");
    sw2.service_uuid = SW2_GUID("ab7de9be-89fe-49ad-828f-118f09df7fd0");
    sw2.session_uuid = SW2_GUID("00c5af5d-1964-4e30-8f51-1956f96bd280");
    sw2.report_rate_uuid = SW2_GUID("679d5510-5a24-4dee-9557-95df80486ecb");
    /* IBluetoothLEDevice3 (windows.devices.bluetooth.h:3473) */
    sw2.device3_iid = SW2_GUID("aee9e493-44ac-40dc-af33-b2c13c01ca46");

    SDL_zero(sw2.device);
    sw2.device.lpVtbl = &sw2_device_vtbl;
    SDL_zero(sw2.device3);
    sw2.device3.lpVtbl = &sw2_device3_vtbl;
    SDL_zero(sw2.service);
    sw2.service.lpVtbl = &sw2_service_vtbl;
    SDL_zero(sw2.session);
    sw2.session.lpVtbl = &sw2_service_vtbl;
    sw2.session.session = true;
    for (i = 0; i < SW2_CHARACTERISTICS; ++i) {
        SDL_zero(sw2.chars[i]);
        sw2.chars[i].lpVtbl = &sw2_char_vtbl;
        sw2.chars[i].role = i;
    }
    sw2.lost = NULL;
    sw2.next_token = 100;
    sw2.session_started = false;
    sw2.rate_written = false;
    sw2.features_enabled = false;
    sw2.reads = 0;
    sw2.bad_calls = 0;
    sw2.nevents = 0;
    sw2.head = 0;
    sw2.count = 0;
    sw2.quit = 0;
    sw2.armed = true;
    LeaveCriticalSection(&sw2.lock);
    sw2.radio = (HANDLE)_beginthreadex(NULL, 0, SW2_RadioThread, NULL, 0, NULL);
}

void SW2_Disarm(void)
{
    if (!sw2.ready) {
        return;
    }
    if (sw2.radio) {
        InterlockedExchange(&sw2.quit, 1);
        SetEvent(sw2.wake);
        WaitForSingleObject(sw2.radio, INFINITE);
        CloseHandle(sw2.radio);
        sw2.radio = NULL;
    }
    EnterCriticalSection(&sw2.lock);
    sw2.armed = false;
    sw2.count = 0;
    LeaveCriticalSection(&sw2.lock);
}

int SW2_HelperCalls(void)
{
    return (int)InterlockedCompareExchange(&sw2.helper_calls, 0, 0);
}

void SW2_ResetHelperCalls(void)
{
    InterlockedExchange(&sw2.helper_calls, 0);
}

int SW2_Events(SW2Event *events, int max)
{
    int count;

    EnterCriticalSection(&sw2.lock);
    count = sw2.nevents;
    if (events && max > 0) {
        SDL_memcpy(events, sw2.events, sizeof(*events) * (size_t)SDL_min(count, max));
    }
    LeaveCriticalSection(&sw2.lock);
    return count;
}

bool SW2_Push(int characteristic, const Uint8 *data, int length)
{
    bool sent = false;

    if (characteristic < 0 || characteristic >= SW2_CHARACTERISTICS || length > SW2_VALUE_MAX) {
        return false;
    }
    EnterCriticalSection(&sw2.lock);
    if (sw2.armed && sw2.chars[characteristic].callback && sw2.chars[characteristic].notifying &&
        (characteristic != SW2_NATIVE_INPUT || (sw2.session_started && sw2.rate_written && sw2.features_enabled))) {
        SW2_Queue(characteristic, data, length, 0);
        sent = true;
    }
    LeaveCriticalSection(&sw2.lock);
    return sent;
}

bool SW2_LoseLink(void)
{
    bool lost = false;

    EnterCriticalSection(&sw2.lock);
    if (sw2.lost) {
        SDL_SetAtomicInt(sw2.lost, 1);
        lost = true;
    }
    LeaveCriticalSection(&sw2.lock);
    return lost;
}

int SW2_References(void)
{
    int refs, i;

    EnterCriticalSection(&sw2.lock);
    refs = sw2.device.refs + sw2.device3.refs + sw2.service.refs + sw2.session.refs;
    for (i = 0; i < SW2_CHARACTERISTICS; ++i) {
        refs += sw2.chars[i].refs;
    }
    LeaveCriticalSection(&sw2.lock);
    return refs;
}

int SW2_BadCalls(void)
{
    int bad;

    EnterCriticalSection(&sw2.lock);
    bad = sw2.bad_calls;
    LeaveCriticalSection(&sw2.lock);
    return bad;
}

int SW2_Reads(void)
{
    int reads;

    EnterCriticalSection(&sw2.lock);
    reads = sw2.reads;
    LeaveCriticalSection(&sw2.lock);
    return reads;
}

/* The Switch 2 driver's own helpers. Each is counted, and each answers for
   the armed device only. */

static int SW2_RoleOf(const SDL_BLEGATTCharacteristic *characteristic)
{
    const SW2Char *found = (const SW2Char *)characteristic;

    if (found >= &sw2.chars[0] && found < &sw2.chars[SW2_CHARACTERISTICS]) {
        return found->role;
    }
    return -1;
}

SDL_BLEGATTDevice *Fake_BLEGATT_OpenDevice(Uint64 address, SDL_AtomicInt *cancel)
{
    SDL_BLEGATTDevice *device = NULL;

    (void)cancel;
    InterlockedIncrement(&sw2.helper_calls);
    if (!sw2.ready) {
        return NULL;
    }
    EnterCriticalSection(&sw2.lock);
    if (sw2.armed && address == sw2.script.address) {
        ++sw2.device.refs;
        device = (SDL_BLEGATTDevice *)&sw2.device;
    }
    LeaveCriticalSection(&sw2.lock);
    return device;
}

SDL_BLEGATTService *Fake_BLEGATT_FindService(SDL_BLEGATTDevice3 *device3, const struct _GUID *uuids, int nuuids,
                                             int attempts, const char *label, SDL_AtomicInt *cancel)
{
    SDL_BLEGATTService *service = NULL;

    (void)attempts;
    (void)label;
    (void)cancel;
    InterlockedIncrement(&sw2.helper_calls);
    if (!sw2.ready || nuuids < 1) {
        return NULL;
    }
    EnterCriticalSection(&sw2.lock);
    if (sw2.armed && device3 == (SDL_BLEGATTDevice3 *)&sw2.device3 && sw2.device3.refs > 0) {
        if (SW2_SameGUID(&uuids[0], &sw2.service_uuid)) {
            ++sw2.service.refs;
            service = (SDL_BLEGATTService *)&sw2.service;
        } else if (SW2_SameGUID(&uuids[0], &sw2.session_uuid)) {
            ++sw2.session.refs;
            service = (SDL_BLEGATTService *)&sw2.session;
        }
    } else if (sw2.armed) {
        ++sw2.bad_calls;
    }
    LeaveCriticalSection(&sw2.lock);
    return service;
}

void Fake_BLEGATT_RequestThroughput(SDL_BLEGATTDevice *device)
{
    (void)device;
    InterlockedIncrement(&sw2.helper_calls);
}

SDL_BLEGATTCharacteristic *Fake_BLEGATT_FindCharacteristic(SDL_BLEGATTService *service3, const struct _GUID *uuid,
                                                           SDL_AtomicInt *cancel)
{
    SDL_BLEGATTCharacteristic *found = NULL;
    const SW2Service *service = (const SW2Service *)service3;
    int i;

    (void)cancel;
    InterlockedIncrement(&sw2.helper_calls);
    if (!sw2.ready) {
        return NULL;
    }
    EnterCriticalSection(&sw2.lock);
    if (sw2.armed && (service == &sw2.service || service == &sw2.session) && service->refs > 0) {
        for (i = 0; i < SW2_CHARACTERISTICS; ++i) {
            const bool in_session = (i == SW2_SESSION_START);
            if (in_session != service->session || !SW2_SameGUID(uuid, &sw2.uuids[i])) {
                continue;
            }
            if (i == SW2_NATIVE_INPUT && sw2.script.no_native_input) {
                break;
            }
            /* Only a Joy-Con has the side's console channel */
            if ((i == SW2_CONSOLE_COMMAND || i == SW2_NATIVE_INPUT || i == SW2_EXTENDED_RESPONSE) &&
                sw2.script.product != USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT &&
                sw2.script.product != USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT) {
                break;
            }
            ++sw2.chars[i].refs;
            found = (SDL_BLEGATTCharacteristic *)&sw2.chars[i];
            break;
        }
    } else if (sw2.armed) {
        ++sw2.bad_calls;
    }
    LeaveCriticalSection(&sw2.lock);
    return found;
}

bool Fake_BLEGATT_WriteCharacteristic(SDL_BLEGATTCharacteristic *characteristic, const Uint8 *bytes, int length,
                                      bool prefer_response)
{
    int role, i;

    InterlockedIncrement(&sw2.helper_calls);
    if (!sw2.ready) {
        return false;
    }
    EnterCriticalSection(&sw2.lock);
    role = SW2_RoleOf(characteristic);
    if (!sw2.armed || role < 0 || sw2.chars[role].refs <= 0 || length <= 0) {
        ++sw2.bad_calls;
        LeaveCriticalSection(&sw2.lock);
        return false;
    }
    SW2_Event(SW2_EVENT_WRITE, role, prefer_response, false, bytes, length);
    switch (role) {
    case SW2_SESSION_START:
        if (length == 2 && bytes[0] == 0x01 && bytes[1] == 0x00) {
            sw2.session_started = true;
        }
        break;
    case SW2_COMMAND:
        if (sw2.script.kind != SW2_CLONE) {
            SW2_Command(SW2_COMMAND, bytes, length);
        }
        break;
    case SW2_CONSOLE_COMMAND:
        for (i = 0; i < 17 && i < length; ++i) {
            if (bytes[i] != 0) {
                break;
            }
        }
        if (i < 17) {
            ++sw2.bad_calls;
        } else {
            SW2_Command(SW2_CONSOLE_COMMAND, &bytes[17], length - 17);
        }
        break;
    default:
        break;
    }
    LeaveCriticalSection(&sw2.lock);
    return true;
}

bool Fake_BLEGATT_EnableNotifications(SDL_BLEGATTCharacteristic *characteristic)
{
    bool enabled = false;
    int role;

    InterlockedIncrement(&sw2.helper_calls);
    if (!sw2.ready) {
        return false;
    }
    EnterCriticalSection(&sw2.lock);
    role = SW2_RoleOf(characteristic);
    if (sw2.armed && role >= 0 && sw2.chars[role].refs > 0) {
        SW2_Event(SW2_EVENT_NOTIFY, role, false, false, NULL, 0);
        sw2.chars[role].notifying = true;
        enabled = true;
        if (role == SW2_UNIFIED_INPUT && sw2.script.kind == SW2_GENUINE && sw2.script.auto_report) {
            SW2_Queue(SW2_UNIFIED_INPUT, sw2.script.auto_report, sw2.script.auto_report_length, 30);
        }
    } else if (sw2.armed) {
        ++sw2.bad_calls;
    }
    LeaveCriticalSection(&sw2.lock);
    return enabled;
}

bool Fake_BLEGATT_WriteDescriptorValue(SDL_BLEGATTCharacteristic *characteristic, const struct _GUID *descriptor_uuid,
                                       const Uint8 *bytes, int length)
{
    bool written = false;
    int role;

    InterlockedIncrement(&sw2.helper_calls);
    if (!sw2.ready) {
        return false;
    }
    EnterCriticalSection(&sw2.lock);
    role = SW2_RoleOf(characteristic);
    if (sw2.armed && role >= 0 && sw2.chars[role].refs > 0) {
        const bool report_rate = SW2_SameGUID(descriptor_uuid, &sw2.report_rate_uuid);

        SW2_Event(SW2_EVENT_DESCRIPTOR, role, false, report_rate, bytes, length);
        /* The input characteristics carry the descriptor (bluetooth_interface.md handles 0x000C and 0x0010) */
        if (report_rate && (role == SW2_NATIVE_INPUT || role == SW2_UNIFIED_INPUT)) {
            if (role == SW2_NATIVE_INPUT && length == 2 && bytes[0] == 0x85 && bytes[1] == 0x00) {
                sw2.rate_written = true;
            }
            written = true;
        }
    } else if (sw2.armed) {
        ++sw2.bad_calls;
    }
    LeaveCriticalSection(&sw2.lock);
    return written;
}

void *Fake_BLEGATT_AddValueHandler(SDL_BLEGATTCharacteristic *characteristic, SDL_BLEGATT_ValueCallback callback,
                                   void *userdata, int index, struct EventRegistrationToken *token)
{
    void *handler = NULL;
    int role;

    InterlockedIncrement(&sw2.helper_calls);
    if (!sw2.ready) {
        return NULL;
    }
    EnterCriticalSection(&sw2.lock);
    role = SW2_RoleOf(characteristic);
    if (sw2.armed && role >= 0 && sw2.chars[role].refs > 0 && callback) {
        SW2_Event(SW2_EVENT_HANDLER, role, false, false, NULL, 0);
        sw2.chars[role].callback = callback;
        sw2.chars[role].userdata = userdata;
        sw2.chars[role].index = index;
        sw2.chars[role].token = ++sw2.next_token;
        token->value = sw2.chars[role].token;
        handler = &sw2.chars[role];
    } else if (sw2.armed) {
        ++sw2.bad_calls;
    }
    LeaveCriticalSection(&sw2.lock);
    return handler;
}

void *Fake_BLEGATT_AddStatusHandler(SDL_BLEGATTDevice *device, SDL_AtomicInt *lost, struct EventRegistrationToken *token)
{
    void *handler = NULL;

    InterlockedIncrement(&sw2.helper_calls);
    if (!sw2.ready) {
        return NULL;
    }
    EnterCriticalSection(&sw2.lock);
    if (sw2.armed && device == (SDL_BLEGATTDevice *)&sw2.device && sw2.device.refs > 0) {
        SW2_Event(SW2_EVENT_STATUS, -1, false, false, NULL, 0);
        sw2.lost = lost;
        token->value = ++sw2.next_token;
        handler = &sw2.device;
    } else if (sw2.armed) {
        ++sw2.bad_calls;
    }
    LeaveCriticalSection(&sw2.lock);
    return handler;
}
