/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/*
 * PadForge fork: WinRT BLE-GATT joystick driver for the wireless Nintendo
 * Switch 2 Pro Controller / Joy-Con 2. See SDL_ble_switch2joystick_design.md
 * and hifihedgehog/SDL#5. The Switch 2 advertises a custom 128-bit GATT
 * service over Bluetooth LE (not HID-over-GATT), so hidapi never sees it and
 * the upstream SDL_hidapi_switch2.c Bluetooth path stays a stub. This driver
 * owns a WinRT BLE connection, modeled on the WGI driver's WinRT-from-C
 * mechanics (SDL_windows_gaming_input.c).
 *
 * Hardware-gated: the BLE report byte offsets and IMU scale are reasoned from
 * the reference reimplementations (ndeadly/Nadeflore/joycon2cpp) and must be
 * confirmed against a physical controller. The build is the only verification
 * available in the SDL-fork environment.
 */

#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_BLE

#include "../SDL_sysjoystick.h"
#include "../SDL_joystick_c.h"
#include "../usb_ids.h"
#include "../../core/windows/SDL_windows.h"

// The Switch 2 GATT/connection-parameter interfaces need a high API contract.
#ifndef WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION
#define WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION 0xe0000
#endif

#define COBJMACROS
#include <windows.devices.bluetooth.h>
#include <windows.devices.bluetooth.advertisement.h>
#include <windows.devices.bluetooth.genericattributeprofile.h>
#include <windows.storage.streams.h>
#include <windows.foundation.h>
#include <roapi.h>
#include <objidlbase.h>
#include <initguid.h>

// ---------------------------------------------------------------------------
// Short aliases for the very long WinRT-from-C symbol names.
// ---------------------------------------------------------------------------

typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher            BleWatcher;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher2           BleWatcher2;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementReceivedEventArgs  BleRecvArgs;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisement                   BleAdvertisement;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEManufacturerData                BleMfgData;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDeviceStatics                    BleDeviceStatics;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice                           BleDevice;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3                          BleDevice3;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice6                          BleDevice6;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEPreferredConnectionParametersStatics BleConnParamStatics;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEPreferredConnectionParameters    BleConnParam;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEPreferredConnectionParametersRequest BleConnParamReq;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServicesResult                  GattServicesResult;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService                         GattService;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3                        GattService3;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicsResult                 GattCharsResult;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic                        GattChar;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattValueChangedEventArgs                 GattValueArgs;
typedef __x_ABI_CWindows_CStorage_CStreams_CIBuffer                                 Buffer;
typedef __x_ABI_CWindows_CStorage_CStreams_CIDataWriter                             DataWriter;

// ---------------------------------------------------------------------------
// IIDs. The WinRT C headers ship only declarations, so we define the GUIDs
// ourselves (same approach as SDL_windows_gaming_input.c).
// ---------------------------------------------------------------------------
DEFINE_GUID(IID_BleWatcher,       0xa6ac336f, 0xf3d3, 0x4297, 0x8d, 0x6c, 0xc8, 0x1e, 0xa6, 0x62, 0x3f, 0x40);
DEFINE_GUID(IID_BleWatcher2,      0x01bf26bc, 0xb164, 0x5805, 0x90, 0xa3, 0xe8, 0xa7, 0x99, 0x7f, 0xf2, 0x25);
DEFINE_GUID(IID_BleRecvHandler,   0x90eb4eca, 0xd465, 0x5ea0, 0xa6, 0x1c, 0x03, 0x3c, 0x8c, 0x5e, 0xce, 0xf2);
DEFINE_GUID(IID_BleRecvArgs,      0x27987ddf, 0xe596, 0x41be, 0x8d, 0x43, 0x9e, 0x67, 0x31, 0xd4, 0xa9, 0x13);
DEFINE_GUID(IID_BleAdvertisement, 0x066fb2b7, 0x33d1, 0x4e7d, 0x83, 0x67, 0xcf, 0x81, 0xd0, 0xf7, 0x96, 0x53);
DEFINE_GUID(IID_BleMfgData,       0x912dba18, 0x6963, 0x4533, 0xb0, 0x61, 0x46, 0x94, 0xda, 0xfb, 0x34, 0xe5);
DEFINE_GUID(IID_BleDeviceStatics, 0xc8cf1a19, 0xf0b6, 0x4bf0, 0x86, 0x89, 0x41, 0x30, 0x3d, 0xe2, 0xd9, 0xf4);
DEFINE_GUID(IID_BleDevice,        0xb5ee2f7b, 0x4ad8, 0x4642, 0xac, 0x48, 0x80, 0xa0, 0xb5, 0x00, 0xe8, 0x87);
DEFINE_GUID(IID_BleDevice3,       0xaee9e493, 0x44ac, 0x40dc, 0xaf, 0x33, 0xb2, 0xc1, 0x3c, 0x01, 0xca, 0x46);
DEFINE_GUID(IID_BleDevice6,       0xca7190ef, 0x0cae, 0x573c, 0xa1, 0xca, 0xe1, 0xfc, 0x5b, 0xfc, 0x39, 0xe2);
DEFINE_GUID(IID_BleConnParamStatics, 0x0e3e8edc, 0x2751, 0x55aa, 0xa8, 0x38, 0x8f, 0xae, 0xee, 0x81, 0x8d, 0x72);
DEFINE_GUID(IID_GattService3,     0xb293a950, 0x0c53, 0x437c, 0xa9, 0xb3, 0x5c, 0x32, 0x10, 0xc6, 0xe5, 0x69);
DEFINE_GUID(IID_GattChar,         0x59cb50c1, 0x5934, 0x4f68, 0xa1, 0x98, 0xeb, 0x86, 0x4f, 0xa4, 0x4e, 0x6b);
DEFINE_GUID(IID_GattValueHandler, 0xc1f420f6, 0x6292, 0x5760, 0xa2, 0xc9, 0x9d, 0xdf, 0x98, 0x68, 0x3c, 0xfc);
DEFINE_GUID(IID_GattValueArgs,    0xd21bdb54, 0x06e3, 0x4ed8, 0xa2, 0x63, 0xac, 0xfa, 0xc8, 0xba, 0x73, 0x13);
DEFINE_GUID(IID_DataWriter,       0x64b89265, 0xd341, 0x4922, 0xb3, 0x8a, 0xdd, 0x4a, 0xf8, 0x80, 0x8c, 0x4e);
DEFINE_GUID(IID_IBufferByteAccess, 0x905a0fef, 0xbc53, 0x11df, 0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda);

// The custom Switch 2 GATT service and its characteristics (all controllers).
DEFINE_GUID(GUID_Switch2Service,    0xab7de9be, 0x89fe, 0x49ad, 0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd0);
DEFINE_GUID(GUID_Switch2Input,      0xab7de9be, 0x89fe, 0x49ad, 0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd2);
DEFINE_GUID(GUID_Switch2Command,    0x649d4ac9, 0x8eb7, 0x4e6c, 0xaf, 0x44, 0x1e, 0xa5, 0x4f, 0xe5, 0xf0, 0x05);
DEFINE_GUID(GUID_Switch2CmdResponse,0xc765a961, 0xd9d8, 0x4d36, 0xa2, 0x0a, 0x53, 0x15, 0xb1, 0x11, 0x83, 0x6a);

#define NINTENDO_BLE_COMPANY_ID 0x0553

// ---------------------------------------------------------------------------
// IBufferByteAccess: classic COM interface to reach an IBuffer's raw bytes.
// robuffer.h is C++-only, so declare the C-callable form here.
// ---------------------------------------------------------------------------
typedef struct IBufferByteAccess IBufferByteAccess;
typedef struct IBufferByteAccessVtbl
{
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(IBufferByteAccess *This, REFIID riid, void **ppv);
    ULONG(STDMETHODCALLTYPE *AddRef)(IBufferByteAccess *This);
    ULONG(STDMETHODCALLTYPE *Release)(IBufferByteAccess *This);
    HRESULT(STDMETHODCALLTYPE *Buffer)(IBufferByteAccess *This, byte **value);
} IBufferByteAccessVtbl;
struct IBufferByteAccess
{
    const IBufferByteAccessVtbl *lpVtbl;
};

// ---------------------------------------------------------------------------
// combase entrypoints, resolved at runtime (same as WGI).
// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI *CoIncrementMTAUsage_t)(HANDLE *pCookie);
typedef HRESULT(WINAPI *RoGetActivationFactory_t)(HSTRING activatableClassId, REFIID iid, void **factory);
typedef HRESULT(WINAPI *RoActivateInstance_t)(HSTRING activatableClassId, IInspectable **instance);
typedef HRESULT(WINAPI *WindowsCreateStringReference_t)(PCWSTR sourceString, UINT32 length, HSTRING_HEADER *header, HSTRING *string);
typedef HRESULT(WINAPI *WindowsDeleteString_t)(HSTRING string);

typedef struct BLE_Controller
{
    SDL_JoystickID instance_id;
    Uint64 bluetooth_address;
    Uint16 vendor_id;
    Uint16 product_id;
    char *name;
    SDL_GUID guid;

    BleDevice *device;
    GattChar *input_char;
    GattChar *command_char;
    GattChar *response_char;
    EventRegistrationToken input_token;
    EventRegistrationToken response_token;

    SDL_Joystick *joystick; // set in Open, NULL otherwise

    // Latest input report, filled by the ValueChanged callback (MTA thread),
    // drained by Update (joystick thread).
    SDL_Mutex *report_lock;
    Uint8 report[64];
    int report_size;
    bool report_pending;
    Uint8 last_state[64];
    bool have_last_state;
} BLE_Controller;

// Raw 12-bit stick value (0..4095, center ~2048) to Sint16 axis. M1 uses an
// uncalibrated linear map; calibration read over the command channel is M2.
static Sint16 BLE_StickAxis(int raw12, bool invert)
{
    int scaled = (raw12 - 2048) * 16;
    if (scaled > 32767) {
        scaled = 32767;
    } else if (scaled < -32768) {
        scaled = -32768;
    }
    return (Sint16)(invert ? -scaled : scaled);
}

static struct
{
    bool initialized;
    bool ro_initialized;
    bool scanning;
    CoIncrementMTAUsage_t CoIncrementMTAUsage;
    RoGetActivationFactory_t RoGetActivationFactory;
    RoActivateInstance_t RoActivateInstance;
    WindowsCreateStringReference_t WindowsCreateStringReference;
    WindowsDeleteString_t WindowsDeleteString;

    BleWatcher *watcher;
    EventRegistrationToken received_token;

    BLE_Controller **controllers;
    int controller_count;
} ble;

// Forward declarations.
static void BLE_ConnectAndSubscribe(Uint64 bluetooth_address, Uint16 vendor_id, Uint16 product_id, char *name);

// ---------------------------------------------------------------------------
// Activation helpers.
// ---------------------------------------------------------------------------
static HRESULT BLE_GetActivationFactory(PCWSTR class_name, REFIID iid, void **out)
{
    HSTRING_HEADER header;
    HSTRING str;
    HRESULT hr = ble.WindowsCreateStringReference(class_name, (UINT32)SDL_wcslen(class_name), &header, &str);
    if (SUCCEEDED(hr)) {
        hr = ble.RoGetActivationFactory(str, iid, out);
    }
    return hr;
}

static HRESULT BLE_ActivateInstance(PCWSTR class_name, REFIID iid, void **out)
{
    HSTRING_HEADER header;
    HSTRING str;
    IInspectable *inspectable = NULL;
    HRESULT hr = ble.WindowsCreateStringReference(class_name, (UINT32)SDL_wcslen(class_name), &header, &str);
    if (SUCCEEDED(hr)) {
        hr = ble.RoActivateInstance(str, &inspectable);
        if (SUCCEEDED(hr)) {
            hr = inspectable->lpVtbl->QueryInterface(inspectable, iid, out);
            inspectable->lpVtbl->Release(inspectable);
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Async-await: a permissive completed-handler avoids needing any parameterized
// IID. put_Completed is vtbl slot 6 on every IAsyncOperation<T>, so we register
// generically; the caller then calls the typed GetResults.
// ---------------------------------------------------------------------------
typedef struct BLE_Awaiter
{
    void *lpVtbl;
    SDL_Semaphore *sem;
} BLE_Awaiter;

static HRESULT STDMETHODCALLTYPE Awaiter_QueryInterface(void *This, REFIID riid, void **ppv)
{
    if (!ppv) {
        return E_INVALIDARG;
    }
    if (WIN_IsEqualIID(riid, &IID_IMarshal)) {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    // Permissive: hand back self for IUnknown, IAgileObject, and the (unnamed)
    // parameterized completed-handler IID. We never marshal across apartments.
    *ppv = This;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE Awaiter_AddRef(void *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE Awaiter_Release(void *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE Awaiter_Invoke(void *This, void *op, int status)
{
    BLE_Awaiter *self = (BLE_Awaiter *)This;
    (void)op;
    (void)status;
    SDL_SignalSemaphore(self->sem);
    return S_OK;
}
static const struct
{
    void *QueryInterface;
    void *AddRef;
    void *Release;
    void *Invoke;
} g_awaiter_vtbl = { (void *)Awaiter_QueryInterface, (void *)Awaiter_AddRef, (void *)Awaiter_Release, (void *)Awaiter_Invoke };

// Block until the IAsyncOperation completes. Returns false on failure to arm.
static bool BLE_Await(void *async_op)
{
    typedef HRESULT(STDMETHODCALLTYPE * put_Completed_t)(void *This, void *handler);
    void ***vtbl;
    put_Completed_t put_Completed;
    BLE_Awaiter awaiter;
    HRESULT hr;

    if (!async_op) {
        return false;
    }
    awaiter.lpVtbl = (void *)&g_awaiter_vtbl;
    awaiter.sem = SDL_CreateSemaphore(0);
    if (!awaiter.sem) {
        return false;
    }
    vtbl = (void ***)async_op;
    put_Completed = (put_Completed_t)(*vtbl)[6]; // IInspectable(0..5) then put_Completed(6)
    hr = put_Completed(async_op, &awaiter);
    if (SUCCEEDED(hr)) {
        SDL_WaitSemaphore(awaiter.sem);
    }
    SDL_DestroySemaphore(awaiter.sem);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// IBuffer helpers.
// ---------------------------------------------------------------------------
static Buffer *BLE_BufferFromBytes(const Uint8 *bytes, UINT32 length)
{
    DataWriter *writer = NULL;
    Buffer *buffer = NULL;

    if (FAILED(BLE_ActivateInstance(RuntimeClass_Windows_Storage_Streams_DataWriter, &IID_DataWriter, (void **)&writer))) {
        return NULL;
    }
    if (SUCCEEDED(__x_ABI_CWindows_CStorage_CStreams_CIDataWriter_WriteBytes(writer, length, (BYTE *)bytes))) {
        __x_ABI_CWindows_CStorage_CStreams_CIDataWriter_DetachBuffer(writer, &buffer);
    }
    __x_ABI_CWindows_CStorage_CStreams_CIDataWriter_Release(writer);
    return buffer;
}

// Copy an IBuffer's bytes into dst (up to dst_len). Returns the number copied.
static int BLE_BufferToBytes(Buffer *buffer, Uint8 *dst, int dst_len)
{
    IBufferByteAccess *access = NULL;
    UINT32 length = 0;
    byte *raw = NULL;
    int copied = 0;

    if (!buffer) {
        return 0;
    }
    __x_ABI_CWindows_CStorage_CStreams_CIBuffer_get_Length(buffer, &length);
    if (FAILED(__x_ABI_CWindows_CStorage_CStreams_CIBuffer_QueryInterface(buffer, &IID_IBufferByteAccess, (void **)&access))) {
        return 0;
    }
    if (SUCCEEDED(access->lpVtbl->Buffer(access, &raw)) && raw) {
        copied = (int)SDL_min((int)length, dst_len);
        SDL_memcpy(dst, raw, copied);
    }
    access->lpVtbl->Release(access);
    return copied;
}

// Write bytes to a characteristic (fire-and-forget, response-less).
static bool BLE_WriteCharacteristic(GattChar *characteristic, const Uint8 *bytes, int length)
{
    Buffer *buffer;
    void *op = NULL;
    bool result = false;

    if (!characteristic) {
        return false;
    }
    buffer = BLE_BufferFromBytes(bytes, (UINT32)length);
    if (!buffer) {
        return false;
    }
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_WriteValueWithOptionAsync(characteristic, buffer, GattWriteOption_WriteWithoutResponse, (void *)&op)) && op) {
        result = BLE_Await(op);
        ((Buffer *)op)->lpVtbl->Release((Buffer *)op);
    }
    __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release(buffer);
    return result;
}

// Subscribe a characteristic to notifications (register handler then write CCCD).
static bool BLE_EnableNotifications(GattChar *characteristic)
{
    void *op = NULL;
    bool result = false;

    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_WriteClientCharacteristicConfigurationDescriptorAsync(characteristic, GattClientCharacteristicConfigurationDescriptorValue_Notify, (void *)&op)) && op) {
        result = BLE_Await(op);
        ((Buffer *)op)->lpVtbl->Release((Buffer *)op);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Controller array bookkeeping.
// ---------------------------------------------------------------------------
static BLE_Controller *BLE_GetControllerByInstance(SDL_JoystickID instance_id)
{
    int i;
    for (i = 0; i < ble.controller_count; ++i) {
        if (ble.controllers[i]->instance_id == instance_id) {
            return ble.controllers[i];
        }
    }
    return NULL;
}

static BLE_Controller *BLE_GetControllerByAddress(Uint64 address)
{
    int i;
    for (i = 0; i < ble.controller_count; ++i) {
        if (ble.controllers[i]->bluetooth_address == address) {
            return ble.controllers[i];
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Input characteristic ValueChanged: copy the raw report into the ring slot.
// Runs on a WinRT thread-pool (MTA) thread.
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE InputHandler_QueryInterface(void *This, REFIID riid, void **ppv)
{
    if (!ppv) {
        return E_INVALIDARG;
    }
    if (WIN_IsEqualIID(riid, &IID_IUnknown) || WIN_IsEqualIID(riid, &IID_IAgileObject) || WIN_IsEqualIID(riid, &IID_GattValueHandler)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return WIN_IsEqualIID(riid, &IID_IMarshal) ? E_NOINTERFACE : E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE InputHandler_AddRef(void *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE InputHandler_Release(void *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE InputHandler_Invoke(void *This, void *sender, GattValueArgs *args)
{
    BLE_Controller *ctrl = (BLE_Controller *)((void **)This)[1];
    Buffer *buffer = NULL;
    (void)sender;

    if (!args || !ctrl) {
        return S_OK;
    }
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattValueChangedEventArgs_get_CharacteristicValue(args, &buffer)) && buffer) {
        Uint8 tmp[64];
        int size = BLE_BufferToBytes(buffer, tmp, sizeof(tmp));
        if (size > 0) {
            SDL_LockMutex(ctrl->report_lock);
            SDL_memcpy(ctrl->report, tmp, size);
            ctrl->report_size = size;
            ctrl->report_pending = true;
            SDL_UnlockMutex(ctrl->report_lock);
        }
        __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release(buffer);
    }
    return S_OK;
}
// vtbl + a trailing context slot so Invoke can find its controller.
typedef struct
{
    void *vtbl;
    BLE_Controller *ctrl;
} InputHandlerObj;
static const struct
{
    void *QueryInterface;
    void *AddRef;
    void *Release;
    void *Invoke;
} g_input_vtbl = { (void *)InputHandler_QueryInterface, (void *)InputHandler_AddRef, (void *)InputHandler_Release, (void *)InputHandler_Invoke };

// ---------------------------------------------------------------------------
// Advertisement Received: match Nintendo BLE company id, parse VID/PID, connect.
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE Received_QueryInterface(void *This, REFIID riid, void **ppv)
{
    if (!ppv) {
        return E_INVALIDARG;
    }
    if (WIN_IsEqualIID(riid, &IID_IUnknown) || WIN_IsEqualIID(riid, &IID_IAgileObject) || WIN_IsEqualIID(riid, &IID_BleRecvHandler)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Received_AddRef(void *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE Received_Release(void *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE Received_Invoke(void *This, void *sender, BleRecvArgs *args)
{
    UINT64 address = 0;
    BleAdvertisement *advertisement = NULL;
    __FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData *mfg_list = NULL;
    (void)This;
    (void)sender;

    if (!args) {
        return S_OK;
    }
    __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementReceivedEventArgs_get_BluetoothAddress(args, &address);
    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementReceivedEventArgs_get_Advertisement(args, &advertisement)) || !advertisement) {
        return S_OK;
    }

    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisement_get_ManufacturerData(advertisement, &mfg_list)) && mfg_list) {
        unsigned i, count = 0;
        __FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData_get_Size(mfg_list, &count);
        for (i = 0; i < count; ++i) {
            BleMfgData *mfg = NULL;
            if (SUCCEEDED(__FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData_GetAt(mfg_list, i, &mfg)) && mfg) {
                UINT16 company = 0;
                Buffer *payload = NULL;
                __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEManufacturerData_get_CompanyId(mfg, &company);
                if (company == NINTENDO_BLE_COMPANY_ID &&
                    SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEManufacturerData_get_Data(mfg, &payload)) && payload) {
                    Uint8 data[32];
                    int n = BLE_BufferToBytes(payload, data, sizeof(data));
                    // Company id already stripped: vendor at [3:5], product at [5:7] LE.
                    if (n >= 7) {
                        Uint16 vendor = (Uint16)(data[3] | (data[4] << 8));
                        Uint16 product = (Uint16)(data[5] | (data[6] << 8));
                        if (vendor == USB_VENDOR_NINTENDO && !BLE_GetControllerByAddress((Uint64)address)) {
                            switch (product) {
                            case USB_PRODUCT_NINTENDO_SWITCH2_PRO:
                            case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT:
                            case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT:
                                BLE_ConnectAndSubscribe((Uint64)address, vendor, product, NULL);
                                break;
                            default:
                                break;
                            }
                        }
                    }
                    __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release(payload);
                }
                __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEManufacturerData_Release(mfg);
            }
        }
        __FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData_Release(mfg_list);
    }
    __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisement_Release(advertisement);
    return S_OK;
}
static const struct
{
    void *QueryInterface;
    void *AddRef;
    void *Release;
    void *Invoke;
} g_received_vtbl = { (void *)Received_QueryInterface, (void *)Received_AddRef, (void *)Received_Release, (void *)Received_Invoke };
static struct { void *vtbl; } g_received_handler = { (void *)&g_received_vtbl };

// ---------------------------------------------------------------------------
// Connect + GATT discovery + subscribe (called from the Received callback).
// ---------------------------------------------------------------------------
static GattChar *BLE_FindCharacteristic(GattService3 *service3, const GUID *uuid)
{
    void *op = NULL;
    GattCharsResult *result = NULL;
    GattChar *found = NULL;

    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3_GetCharacteristicsForUuidAsync(service3, *uuid, (void *)&op)) || !op) {
        return NULL;
    }
    if (BLE_Await(op)) {
        // GetResults is vtbl slot 8 on every IAsyncOperation<T>.
        typedef HRESULT(STDMETHODCALLTYPE * GetResults_t)(void *This, GattCharsResult **out);
        void ***vt = (void ***)op;
        ((GetResults_t)(*vt)[8])(op, &result);
    }
    if (result) {
        __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic *chars = NULL;
        if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicsResult_get_Characteristics(result, &chars)) && chars) {
            unsigned size = 0;
            __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic_get_Size(chars, &size);
            if (size > 0) {
                __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic_GetAt(chars, 0, &found);
            }
            __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic_Release(chars);
        }
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicsResult_Release(result);
    }
    ((GattCharsResult *)op)->lpVtbl->Release((GattCharsResult *)op);
    return found;
}

static void BLE_ConnectAndSubscribe(Uint64 bluetooth_address, Uint16 vendor_id, Uint16 product_id, char *name)
{
    BleDeviceStatics *statics = NULL;
    void *op = NULL;
    BleDevice *device = NULL;
    BleDevice3 *device3 = NULL;
    GattServicesResult *services_result = NULL;
    GattService *service = NULL;
    GattService3 *service3 = NULL;
    BLE_Controller *ctrl = NULL;
    InputHandlerObj *input_handler = NULL;

    if (FAILED(BLE_GetActivationFactory(RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice, &IID_BleDeviceStatics, (void **)&statics))) {
        return;
    }
    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDeviceStatics_FromBluetoothAddressAsync(statics, bluetooth_address, (void *)&op)) || !op) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDeviceStatics_Release(statics);
        return;
    }
    if (BLE_Await(op)) {
        typedef HRESULT(STDMETHODCALLTYPE * GetResults_t)(void *This, BleDevice **out);
        void ***vt = (void ***)op;
        ((GetResults_t)(*vt)[8])(op, &device);
    }
    ((BleDevice *)op)->lpVtbl->Release((BleDevice *)op);
    __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDeviceStatics_Release(statics);
    if (!device) {
        return;
    }

    // Best-effort throughput bump (Win10 1809+; ignore failure).
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_QueryInterface(device, &IID_BleDevice6, (void **)&device3) /* reuse var below */)) {
        // device3 currently holds an IBluetoothLEDevice6*; request params then release.
        BleConnParamStatics *cp_statics = NULL;
        if (SUCCEEDED(BLE_GetActivationFactory(RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEPreferredConnectionParameters, &IID_BleConnParamStatics, (void **)&cp_statics))) {
            BleConnParam *params = NULL;
            if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEPreferredConnectionParametersStatics_get_ThroughputOptimized(cp_statics, &params)) && params) {
                BleConnParamReq *req = NULL;
                __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice6_RequestPreferredConnectionParameters((BleDevice6 *)device3, params, &req);
                if (req) {
                    ((BleConnParamReq *)req)->lpVtbl->Release((BleConnParamReq *)req);
                }
                __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEPreferredConnectionParameters_Release(params);
            }
            __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEPreferredConnectionParametersStatics_Release(cp_statics);
        }
        ((BleDevice6 *)device3)->lpVtbl->Release((BleDevice6 *)device3);
        device3 = NULL;
    }

    // Discover the Switch 2 service.
    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_QueryInterface(device, &IID_BleDevice3, (void **)&device3)) || !device3) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(device);
        return;
    }
    op = NULL;
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3_GetGattServicesForUuidAsync(device3, GUID_Switch2Service, (void *)&op)) && op) {
        if (BLE_Await(op)) {
            typedef HRESULT(STDMETHODCALLTYPE * GetResults_t)(void *This, GattServicesResult **out);
            void ***vt = (void ***)op;
            ((GetResults_t)(*vt)[8])(op, &services_result);
        }
        ((GattServicesResult *)op)->lpVtbl->Release((GattServicesResult *)op);
    }
    if (services_result) {
        __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService *list = NULL;
        if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServicesResult_get_Services(services_result, &list)) && list) {
            unsigned size = 0;
            __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService_get_Size(list, &size);
            if (size > 0) {
                __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService_GetAt(list, 0, &service);
            }
            __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService_Release(list);
        }
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServicesResult_Release(services_result);
    }
    if (service) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService_QueryInterface(service, &IID_GattService3, (void **)&service3);
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService_Release(service);
    }
    if (!service3) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3_Release(device3);
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(device);
        return;
    }

    // Build the controller record.
    ctrl = (BLE_Controller *)SDL_calloc(1, sizeof(*ctrl));
    if (!ctrl) {
        goto cleanup;
    }
    ctrl->report_lock = SDL_CreateMutex();
    ctrl->bluetooth_address = bluetooth_address;
    ctrl->vendor_id = vendor_id;
    ctrl->product_id = product_id;
    ctrl->name = name ? name : SDL_strdup("Nintendo Switch 2 Pro Controller");
    ctrl->device = device;
    ctrl->instance_id = SDL_GetNextObjectID();
    ctrl->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, vendor_id, product_id, 0, NULL, ctrl->name, 'h', 0);

    ctrl->input_char = BLE_FindCharacteristic(service3, &GUID_Switch2Input);
    ctrl->command_char = BLE_FindCharacteristic(service3, &GUID_Switch2Command);
    ctrl->response_char = BLE_FindCharacteristic(service3, &GUID_Switch2CmdResponse);

    if (ctrl->input_char) {
        input_handler = (InputHandlerObj *)SDL_calloc(1, sizeof(*input_handler));
        if (input_handler) {
            input_handler->vtbl = (void *)&g_input_vtbl;
            input_handler->ctrl = ctrl;
            // Register the handler before enabling notifications (spec ordering).
            __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_add_ValueChanged(ctrl->input_char, (void *)input_handler, &ctrl->input_token);
            BLE_EnableNotifications(ctrl->input_char);
        }
    }

    SDL_LockJoysticks();
    {
        BLE_Controller **grown = (BLE_Controller **)SDL_realloc(ble.controllers, sizeof(ble.controllers[0]) * (ble.controller_count + 1));
        if (grown) {
            ble.controllers = grown;
            ble.controllers[ble.controller_count++] = ctrl;
            SDL_PrivateJoystickAdded(ctrl->instance_id);
            ctrl = NULL; // owned by the array now
        }
    }
    SDL_UnlockJoysticks();

cleanup:
    if (service3) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3_Release(service3);
    }
    if (device3) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3_Release(device3);
    }
    if (ctrl) {
        // Failed to publish; tear down.
        if (ctrl->report_lock) {
            SDL_DestroyMutex(ctrl->report_lock);
        }
        SDL_free(ctrl->name);
        SDL_free(ctrl);
        if (device) {
            __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(device);
        }
    }
}

// ---------------------------------------------------------------------------
// Input decode. The BLE report is the wired report minus the 1-byte report-ID
// prefix, so the wired HandleSwitchProState layout applies at offset -1. Button
// bytes: u32 LE at [4:8]; sticks at [10:16]; IMU accel [48:54] / gyro [54:60].
// ---------------------------------------------------------------------------
static void BLE_DecodeProReport(BLE_Controller *ctrl, SDL_Joystick *joystick, Uint8 *data, int size)
{
    Uint64 timestamp = SDL_GetTicksNS();
    Sint16 axis;

    if (size < 16) {
        return;
    }

    // data[4..7] are the four button bytes (== wired data[5..8]).
    if (!ctrl->have_last_state || data[4] != ctrl->last_state[4]) {
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[4] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[4] & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[4] & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[4] & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data[4] & 0x40) != 0));
    }
    if (!ctrl->have_last_state || data[5] != ctrl->last_state[5]) {
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data[5] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data[5] & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, ((data[5] & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data[5] & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[5] & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_MISC1, ((data[5] & 0x20) != 0)); // Share
        SDL_SendJoystickButton(timestamp, joystick, 12 /* C */, ((data[5] & 0x40) != 0));
    }
    if (!ctrl->have_last_state || data[6] != ctrl->last_state[6]) {
        Uint8 hat = 0;
        if (data[6] & 0x01) {
            hat |= SDL_HAT_DOWN;
        }
        if (data[6] & 0x02) {
            hat |= SDL_HAT_UP;
        }
        if (data[6] & 0x04) {
            hat |= SDL_HAT_RIGHT;
        }
        if (data[6] & 0x08) {
            hat |= SDL_HAT_LEFT;
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[6] & 0x40) != 0));
    }
    if (!ctrl->have_last_state || data[7] != ctrl->last_state[7]) {
        SDL_SendJoystickButton(timestamp, joystick, 13 /* right paddle */, ((data[7] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, 14 /* left paddle */, ((data[7] & 0x02) != 0));
    }

    axis = (data[4] & 0x80) ? 32767 : -32768; // ZR
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis);
    axis = (data[6] & 0x80) ? 32767 : -32768; // ZL
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis);

    // Sticks: 12-bit packed at [10:13] (left) and [13:16] (right).
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX,
                         BLE_StickAxis(data[10] | ((data[11] & 0x0F) << 8), false));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY,
                         BLE_StickAxis((data[11] >> 4) | (data[12] << 4), true));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX,
                         BLE_StickAxis(data[13] | ((data[14] & 0x0F) << 8), false));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY,
                         BLE_StickAxis((data[14] >> 4) | (data[15] << 4), true));

    // IMU (scale reused from the wired driver; verify on hardware).
    if (size >= 60 && ctrl->joystick) {
        Sint16 ax = (Sint16)(data[48] | (data[49] << 8));
        Sint16 ay = (Sint16)(data[50] | (data[51] << 8));
        Sint16 az = (Sint16)(data[52] | (data[53] << 8));
        Sint16 gx = (Sint16)(data[54] | (data[55] << 8));
        Sint16 gy = (Sint16)(data[56] | (data[57] << 8));
        Sint16 gz = (Sint16)(data[58] | (data[59] << 8));
        const float accel_scale = SDL_STANDARD_GRAVITY * 8.0f / 32767.0f;
        const float gyro_scale = 34.8f * (SDL_PI_F / 180.0f) / 1000.0f;
        float accel[3], gyro[3];
        accel[0] = ax * accel_scale;
        accel[1] = ay * accel_scale;
        accel[2] = az * accel_scale;
        gyro[0] = gx * gyro_scale;
        gyro[1] = gy * gyro_scale;
        gyro[2] = gz * gyro_scale;
        SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_ACCEL, timestamp, accel, 3);
        SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_GYRO, timestamp, gyro, 3);
    }

    SDL_memcpy(ctrl->last_state, data, SDL_min(size, (int)sizeof(ctrl->last_state)));
    ctrl->have_last_state = true;
}

// ---------------------------------------------------------------------------
// SDL_JoystickDriver entry points.
// ---------------------------------------------------------------------------
static bool BLE_JoystickInit(void)
{
    if (!SDL_GetHintBoolean(SDL_HINT_JOYSTICK_BLE_SWITCH2, false)) {
        return true; // disabled, but the driver still loads cleanly
    }
    if (ble.initialized) {
        return true;
    }

    if (FAILED(WIN_RoInitialize())) {
        return SDL_SetError("RoInitialize() failed");
    }
    ble.ro_initialized = true;

#define RESOLVE(x) ble.x = (x##_t)WIN_LoadComBaseFunction(#x); if (!ble.x) return SDL_SetError("GetProcAddress failed for " #x)
    RESOLVE(CoIncrementMTAUsage);
    RESOLVE(RoGetActivationFactory);
    RESOLVE(RoActivateInstance);
    RESOLVE(WindowsCreateStringReference);
    RESOLVE(WindowsDeleteString);
#undef RESOLVE

    {
        static HANDLE cookie = NULL;
        if (!cookie) {
            ble.CoIncrementMTAUsage(&cookie); // pin MTA for BLE callback threads
        }
    }

    // Start the advertisement watcher.
    if (SUCCEEDED(BLE_ActivateInstance(RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisementWatcher, &IID_BleWatcher, (void **)&ble.watcher))) {
        BleWatcher2 *watcher2 = NULL;
        __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_put_ScanningMode(ble.watcher, BluetoothLEScanningMode_Active);
        if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_QueryInterface(ble.watcher, &IID_BleWatcher2, (void **)&watcher2))) {
            __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher2_put_AllowExtendedAdvertisements(watcher2, TRUE);
            __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher2_Release(watcher2);
        }
        __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_add_Received(ble.watcher, (void *)&g_received_handler, &ble.received_token);
        __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_Start(ble.watcher);
        ble.scanning = true;
    }

    ble.initialized = true;
    return true;
}

static int BLE_JoystickGetCount(void)
{
    return ble.controller_count;
}

static void BLE_JoystickDetect(void)
{
}

static bool BLE_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    (void)version;
    (void)name;
    int i;
    for (i = 0; i < ble.controller_count; ++i) {
        if (ble.controllers[i]->vendor_id == vendor_id && ble.controllers[i]->product_id == product_id) {
            return true;
        }
    }
    return false;
}

static const char *BLE_JoystickGetDeviceName(int device_index)
{
    if (device_index >= 0 && device_index < ble.controller_count) {
        return ble.controllers[device_index]->name;
    }
    return NULL;
}

static const char *BLE_JoystickGetDevicePath(int device_index)
{
    (void)device_index;
    return NULL;
}

static int BLE_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    (void)device_index;
    return -1;
}

static int BLE_JoystickGetDevicePlayerIndex(int device_index)
{
    (void)device_index;
    return -1;
}

static void BLE_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
    (void)device_index;
    (void)player_index;
}

static SDL_GUID BLE_JoystickGetDeviceGUID(int device_index)
{
    SDL_GUID guid;
    if (device_index >= 0 && device_index < ble.controller_count) {
        return ble.controllers[device_index]->guid;
    }
    SDL_zero(guid);
    return guid;
}

static SDL_JoystickID BLE_JoystickGetDeviceInstanceID(int device_index)
{
    if (device_index >= 0 && device_index < ble.controller_count) {
        return ble.controllers[device_index]->instance_id;
    }
    return 0;
}

static bool BLE_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    BLE_Controller *ctrl;
    if (device_index < 0 || device_index >= ble.controller_count) {
        return SDL_SetError("BLE joystick index out of range");
    }
    ctrl = ble.controllers[device_index];
    ctrl->joystick = joystick;

    joystick->nbuttons = 15; // a..y, back, guide, start, sticks, shoulders, share, C, paddles
    joystick->naxes = SDL_GAMEPAD_AXIS_COUNT;
    joystick->nhats = 1;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;

    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, 250.0f);
    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 250.0f);
    return true;
}

static bool BLE_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    (void)joystick;
    (void)low_frequency_rumble;
    (void)high_frequency_rumble;
    return SDL_Unsupported(); // M2
}

static bool BLE_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    (void)joystick;
    (void)left_rumble;
    (void)right_rumble;
    return SDL_Unsupported();
}

static bool BLE_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)joystick;
    (void)red;
    (void)green;
    (void)blue;
    return SDL_Unsupported();
}

static bool BLE_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    (void)joystick;
    (void)data;
    (void)size;
    return SDL_Unsupported();
}

static bool BLE_JoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    (void)joystick;
    (void)enabled;
    return true; // IMU is always streamed in report 0x05
}

static void BLE_JoystickUpdate(SDL_Joystick *joystick)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    Uint8 data[64];
    int size = 0;

    if (!ctrl) {
        return;
    }
    SDL_LockMutex(ctrl->report_lock);
    if (ctrl->report_pending) {
        size = ctrl->report_size;
        SDL_memcpy(data, ctrl->report, size);
        ctrl->report_pending = false;
    }
    SDL_UnlockMutex(ctrl->report_lock);

    if (size > 0) {
        // report[0] is the report id 0x05; pass the full buffer (decode uses
        // absolute BLE offsets that already account for the missing prefix).
        BLE_DecodeProReport(ctrl, joystick, data, size);
    }
}

static void BLE_JoystickClose(SDL_Joystick *joystick)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    if (ctrl) {
        ctrl->joystick = NULL;
    }
}

static void BLE_FreeController(BLE_Controller *ctrl)
{
    if (ctrl->input_char) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_remove_ValueChanged(ctrl->input_char, ctrl->input_token);
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(ctrl->input_char);
    }
    if (ctrl->command_char) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(ctrl->command_char);
    }
    if (ctrl->response_char) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(ctrl->response_char);
    }
    if (ctrl->device) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(ctrl->device);
    }
    if (ctrl->report_lock) {
        SDL_DestroyMutex(ctrl->report_lock);
    }
    SDL_free(ctrl->name);
    SDL_free(ctrl);
}

static void BLE_JoystickQuit(void)
{
    int i;

    if (ble.watcher) {
        __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_Stop(ble.watcher);
        if (ble.received_token.value) {
            __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_remove_Received(ble.watcher, ble.received_token);
        }
        __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_Release(ble.watcher);
        ble.watcher = NULL;
    }
    for (i = 0; i < ble.controller_count; ++i) {
        BLE_FreeController(ble.controllers[i]);
    }
    SDL_free(ble.controllers);
    ble.controllers = NULL;
    ble.controller_count = 0;

    if (ble.ro_initialized) {
        WIN_RoUninitialize();
        ble.ro_initialized = false;
    }
    ble.scanning = false;
    ble.initialized = false;
}

static bool BLE_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    (void)device_index;
    (void)out;
    return false; // resolved by the 'h'+VID/PID fabricated mapping
}

SDL_JoystickDriver SDL_BLE_JoystickDriver = {
    BLE_JoystickInit,
    BLE_JoystickGetCount,
    BLE_JoystickDetect,
    BLE_JoystickIsDevicePresent,
    BLE_JoystickGetDeviceName,
    BLE_JoystickGetDevicePath,
    BLE_JoystickGetDeviceSteamVirtualGamepadSlot,
    BLE_JoystickGetDevicePlayerIndex,
    BLE_JoystickSetDevicePlayerIndex,
    BLE_JoystickGetDeviceGUID,
    BLE_JoystickGetDeviceInstanceID,
    BLE_JoystickOpen,
    BLE_JoystickRumble,
    BLE_JoystickRumbleTriggers,
    BLE_JoystickSetLED,
    BLE_JoystickSendEffect,
    BLE_JoystickSetSensorsEnabled,
    BLE_JoystickUpdate,
    BLE_JoystickClose,
    BLE_JoystickQuit,
    BLE_JoystickGetGamepadMapping
};

#endif // SDL_JOYSTICK_BLE
