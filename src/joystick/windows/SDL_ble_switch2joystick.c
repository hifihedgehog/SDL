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
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattCommunicationStatus                    GattCommStatus;
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
// Parameterized IAsyncOperationCompletedHandler<T> IIDs (PIIDs). The shared
// awaiter must QI-accept exactly the handler IID for the op it is registered on,
// like the WGI driver's per-delegate QI (SDL_windows_gaming_input.c:340-342).
DEFINE_GUID(IID_AsyncDeviceHandler,   0x375f9d67, 0x74a2, 0x5f91, 0xa1, 0x1d, 0x16, 0x90, 0x93, 0x71, 0x8d, 0x41); // <BluetoothLEDevice>
DEFINE_GUID(IID_AsyncServicesHandler, 0xe7c667f6, 0xe874, 0x500f, 0x86, 0xff, 0x76, 0x0c, 0xa6, 0xf0, 0x7a, 0x58); // <GattDeviceServicesResult>
DEFINE_GUID(IID_AsyncCharsHandler,    0x0972194a, 0xac1c, 0x5536, 0x98, 0x86, 0x27, 0xe5, 0x8a, 0x18, 0xf2, 0x73); // <GattCharacteristicsResult>
DEFINE_GUID(IID_AsyncStatusHandler,   0x3ff69516, 0x1bfb, 0x52e9, 0x9e, 0xe6, 0xe5, 0xcd, 0xb7, 0x8e, 0x16, 0x83); // <GattCommunicationStatus>

// The custom Switch 2 GATT service and its characteristics (all controllers).
DEFINE_GUID(GUID_Switch2Service,    0xab7de9be, 0x89fe, 0x49ad, 0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd0);
DEFINE_GUID(GUID_Switch2Input,      0xab7de9be, 0x89fe, 0x49ad, 0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd2);
DEFINE_GUID(GUID_Switch2Command,    0x649d4ac9, 0x8eb7, 0x4e6c, 0xaf, 0x44, 0x1e, 0xa5, 0x4f, 0xe5, 0xf0, 0x05);
DEFINE_GUID(GUID_Switch2CmdResponse,0xc765a961, 0xd9d8, 0x4d36, 0xa2, 0x0a, 0x53, 0x15, 0xb1, 0x11, 0x83, 0x6a);
// Per-controller-type vibration characteristics (handle 0x0012).
DEFINE_GUID(GUID_Switch2VibePro,  0xcc483f51, 0x9258, 0x427d, 0xa9, 0x39, 0x63, 0x0c, 0x31, 0xf7, 0x2b, 0x05);
DEFINE_GUID(GUID_Switch2VibeJCL,  0x289326cb, 0xa471, 0x485d, 0xa8, 0xf4, 0x24, 0x0c, 0x14, 0xf1, 0x82, 0x41);
DEFINE_GUID(GUID_Switch2VibeJCR,  0xfa19b0fb, 0xcd1f, 0x46a7, 0x84, 0xa1, 0xbb, 0xb0, 0x9e, 0x00, 0xc1, 0x49);
DEFINE_GUID(GUID_Switch2VibeGC,   0x3f8fb670, 0xab25, 0x45bf, 0xb5, 0x40, 0x38, 0xc7, 0x28, 0x34, 0xd0, 0x64);

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
typedef HRESULT(WINAPI *RoInitialize_t)(RO_INIT_TYPE initType);
typedef void(WINAPI *RoUninitialize_t)(void);
typedef HRESULT(WINAPI *WindowsCreateStringReference_t)(PCWSTR sourceString, UINT32 length, HSTRING_HEADER *header, HSTRING *string);
typedef HRESULT(WINAPI *WindowsDeleteString_t)(HSTRING string);

// Per-axis stick calibration (reimplemented; the wired versions are file-static).
typedef struct
{
    Uint16 neutral;
    Uint16 max;
    Uint16 min;
} Switch2_AxisCal;

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
    GattChar *vibration_char;
    EventRegistrationToken input_token;
    EventRegistrationToken response_token;
    void *input_handler;    // heap delegate, freed in BLE_FreeController
    void *response_handler; // heap delegate, freed in BLE_FreeController

    SDL_Joystick *joystick; // set in Open, NULL otherwise

    // Latest input report, filled by the ValueChanged callback (MTA thread),
    // drained by Update (joystick thread).
    SDL_Mutex *report_lock;
    Uint8 report[64];
    int report_size;
    bool report_pending;
    Uint8 last_state[64];
    bool have_last_state;
    bool logged_first_report; // one-shot debug dump of the first input notification

    // Command/response channel: a write to command_char produces a notification
    // on response_char. The ValueChanged handler stashes it and signals. A flash
    // read reply is 0x10 header + 0x40 data = 0x50 bytes, so size for that.
    SDL_Mutex *response_lock;
    SDL_Semaphore *response_sem;
    Uint8 response[128];
    int response_size;

    // Stick calibration (reimplemented from the wired driver; static there).
    Switch2_AxisCal left_x, left_y, right_x, right_y;
    bool calibrated;
    bool sensors_enabled; // IMU streams only after the enable command is sent

    // Rumble state.
    Uint32 rumble_seq;
    int player_index;
} BLE_Controller;

static void BLE_ParseStickCalibration(Switch2_AxisCal *x, Switch2_AxisCal *y, const Uint8 *data)
{
    x->neutral = (Uint16)(data[0] | ((data[1] & 0x0F) << 8));
    y->neutral = (Uint16)((data[1] >> 4) | (data[2] << 4));
    x->max = (Uint16)(data[3] | ((data[4] & 0x0F) << 8));
    y->max = (Uint16)((data[4] >> 4) | (data[5] << 4));
    x->min = (Uint16)(data[6] | ((data[7] & 0x0F) << 8));
    y->min = (Uint16)((data[7] >> 4) | (data[8] << 4));
}

static Sint16 BLE_MapStickAxis(const Switch2_AxisCal *calib, float value, bool invert)
{
    Sint16 mapped;
    if (calib && calib->neutral && calib->min && calib->max) {
        value -= calib->neutral;
        value /= (value < 0) ? calib->min : calib->max;
        mapped = (Sint16)SDL_clamp(value * SDL_MAX_SINT16, SDL_MIN_SINT16, SDL_MAX_SINT16);
    } else {
        // Uncalibrated linear map of the 12-bit range.
        int scaled = ((int)value - 2048) * 16;
        mapped = (Sint16)SDL_clamp(scaled, SDL_MIN_SINT16, SDL_MAX_SINT16);
    }
    return (Sint16)(invert ? ~mapped : mapped);
}

static struct
{
    bool initialized;
    bool ro_initialized;
    bool scanning;
    CoIncrementMTAUsage_t CoIncrementMTAUsage;
    RoGetActivationFactory_t RoGetActivationFactory;
    RoActivateInstance_t RoActivateInstance;
    RoInitialize_t RoInitialize;
    RoUninitialize_t RoUninitialize;
    WindowsCreateStringReference_t WindowsCreateStringReference;
    WindowsDeleteString_t WindowsDeleteString;

    BleWatcher *watcher;
    EventRegistrationToken received_token;

    BLE_Controller **controllers;
    int controller_count;

    // Addresses with a connect in progress, so repeated advertisements during
    // the (multi-second) connect don't start a storm of duplicate connects.
    Uint64 connecting[16];
    int connecting_count;
} ble;

// Forward declarations.
static void BLE_ConnectAndSubscribe(Uint64 bluetooth_address, Uint16 vendor_id, Uint16 product_id, char *name);
static void BLE_FreeController(BLE_Controller *ctrl);
static BLE_Controller *BLE_GetControllerByAddress(Uint64 address);
static void BLE_ReleaseConnect(Uint64 address);

// The advertisement Received callback hands the connect to this worker thread so
// the WinRT thread-pool callback returns immediately. The connect blocks on
// multi-second async opens and discovery, and blocking the callback thread starves
// the same thread pool that must deliver those completions (hifihedgehog/SDL#5:
// device=NULL on every attempt). joycon2cpp opens on its own connect thread
// (testapp.cpp:812-827) and bleak dispatches to a task (discoverer.py).
typedef struct
{
    Uint64 address;
    Uint16 vendor;
    Uint16 product;
} BLE_ConnectRequest;

static int SDLCALL BLE_ConnectThread(void *data)
{
    BLE_ConnectRequest *req = (BLE_ConnectRequest *)data;
    bool ro_inited = false;

    // Join the MTA explicitly. This is a fresh SDL thread, not a system thread-pool
    // thread, so it has no apartment of its own. RoGetActivationFactory can return
    // RO_E_UNINITIALIZED on an uninitialized thread even with the process-wide
    // implicit MTA from CoIncrementMTAUsage, so initialize it as MULTITHREADED
    // (not WIN_RoInitialize, which is STA-first and would risk a marshal-back
    // deadlock against the agile completion handlers this thread blocks on).
    if (ble.RoInitialize) {
        ro_inited = SUCCEEDED(ble.RoInitialize(RO_INIT_MULTITHREADED));
    }
    BLE_ConnectAndSubscribe(req->address, req->vendor, req->product, NULL);
    BLE_ReleaseConnect(req->address); // address reserved by the caller before spawn
    if (ro_inited && ble.RoUninitialize) {
        ble.RoUninitialize();
    }
    SDL_free(req);
    return 0;
}

// Reserve an address for connecting. Returns false if it is already connected or
// a connect is already in progress. Caller must BLE_ReleaseConnect on completion.
static bool BLE_TryReserveConnect(Uint64 address)
{
    int i;
    bool reserved = false;

    SDL_LockJoysticks();
    if (!BLE_GetControllerByAddress(address)) {
        bool pending = false;
        for (i = 0; i < ble.connecting_count; ++i) {
            if (ble.connecting[i] == address) {
                pending = true;
                break;
            }
        }
        if (!pending && ble.connecting_count < (int)SDL_arraysize(ble.connecting)) {
            ble.connecting[ble.connecting_count++] = address;
            reserved = true;
        }
    }
    SDL_UnlockJoysticks();
    return reserved;
}

static void BLE_ReleaseConnect(Uint64 address)
{
    int i;

    SDL_LockJoysticks();
    for (i = 0; i < ble.connecting_count; ++i) {
        if (ble.connecting[i] == address) {
            ble.connecting[i] = ble.connecting[--ble.connecting_count];
            break;
        }
    }
    SDL_UnlockJoysticks();
}

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
    SDL_AtomicInt refcount;
    SDL_Semaphore *sem;
    const GUID *handler_iid; // PIID of the IAsyncOperationCompletedHandler<T> we are
} BLE_Awaiter;

static ULONG STDMETHODCALLTYPE Awaiter_AddRef(void *This);

// Strict QI, matching the WGI handler (SDL_windows_gaming_input.c:340-342): accept
// only IUnknown, IAgileObject, and this awaiter's specific completed-handler IID,
// AddRef on success, E_NOINTERFACE for everything else. The previous permissive
// version handed back self (and skipped AddRef) for every IID except IMarshal,
// including IInspectable, which a delegate is not. WinRT rejected that handler at
// put_Completed with CO_E_NOTSUPPORTED (0x80004021), so the open never waited
// (hifihedgehog/SDL#5).
static HRESULT STDMETHODCALLTYPE Awaiter_QueryInterface(void *This, REFIID riid, void **ppv)
{
    BLE_Awaiter *self = (BLE_Awaiter *)This;
    if (!ppv) {
        return E_INVALIDARG;
    }
    if (WIN_IsEqualIID(riid, &IID_IUnknown) ||
        WIN_IsEqualIID(riid, &IID_IAgileObject) ||
        (self->handler_iid && WIN_IsEqualIID(riid, self->handler_iid))) {
        *ppv = This;
        Awaiter_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Awaiter_AddRef(void *This)
{
    BLE_Awaiter *self = (BLE_Awaiter *)This;
    return (ULONG)(SDL_AddAtomicInt(&self->refcount, 1) + 1);
}
static ULONG STDMETHODCALLTYPE Awaiter_Release(void *This)
{
    BLE_Awaiter *self = (BLE_Awaiter *)This;
    int rc = SDL_AddAtomicInt(&self->refcount, -1) - 1;
    if (rc == 0) {
        SDL_DestroySemaphore(self->sem);
        SDL_free(self);
    }
    return (ULONG)rc;
}
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

// Block (up to timeout_ms) until the IAsyncOperation completes. The handler is
// heap-allocated and refcounted: WinRT holds a reference until it finishes, so a
// timeout here cannot free the handler out from under a later completion. Returns
// false on arm failure or timeout.
static bool BLE_AwaitTimeout(void *async_op, Sint32 timeout_ms, const GUID *handler_iid)
{
    typedef HRESULT(STDMETHODCALLTYPE * put_Completed_t)(void *This, void *handler);
    void ***vtbl;
    put_Completed_t put_Completed;
    BLE_Awaiter *awaiter;
    HRESULT hr;
    bool completed;

    if (!async_op) {
        return false;
    }
    awaiter = (BLE_Awaiter *)SDL_calloc(1, sizeof(*awaiter));
    if (!awaiter) {
        return false;
    }
    awaiter->lpVtbl = (void *)&g_awaiter_vtbl;
    awaiter->handler_iid = handler_iid;
    awaiter->sem = SDL_CreateSemaphore(0);
    if (!awaiter->sem) {
        SDL_free(awaiter);
        return false;
    }
    SDL_SetAtomicInt(&awaiter->refcount, 1); // our reference

    vtbl = (void ***)async_op;
    put_Completed = (put_Completed_t)(*vtbl)[6]; // IInspectable(0..5) then put_Completed(6)
    hr = put_Completed(async_op, awaiter);      // WinRT takes its own reference
    if (SUCCEEDED(hr)) {
        completed = SDL_WaitSemaphoreTimeout(awaiter->sem, timeout_ms);
    } else {
        completed = false;
    }
    // Diagnostic (hifihedgehog/SDL#5): pin candidate (b). A put_Completed failure,
    // or a completed=0 that returns far sooner than timeout_ms, means the await is
    // not actually waiting on the async op.
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE await: put_Completed hr=0x%08lX wait(%dms) completed=%d",
                 (unsigned long)hr, (int)timeout_ms, (int)completed);
    Awaiter_Release(awaiter); // drop our reference; WinRT frees it when it is done
    return completed;
}

// Default await for post-link per-op reads and writes (link already up, so a
// short ceiling is fine and surfaces a wedged op quickly).
static bool BLE_Await(void *async_op, const GUID *handler_iid)
{
    return BLE_AwaitTimeout(async_op, 3000, handler_iid);
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

// Debug hex dump (visible at SDL_LOG_PRIORITY_DEBUG). Used to capture the real
// on-wire layout so the transport-specific unknowns (input report offsets, flash
// reply offset) can be confirmed against the reference on first hardware contact.
static void BLE_LogBytes(const char *label, const Uint8 *data, int len)
{
    char hex[3 * 64 + 1];
    int i, n = SDL_min(len, 64);
    for (i = 0; i < n; ++i) {
        (void)SDL_snprintf(&hex[i * 3], 4, "%02x ", data[i]);
    }
    hex[n > 0 ? n * 3 : 0] = '\0';
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 %s (%d bytes): %s", label, len, hex);
}

// Write bytes to a characteristic. The caller's prefer_response hint asks for a
// reliable (acknowledged) write for the command channel, but the Switch 2 command
// characteristic only advertises write-without-response on real hardware
// (switch2-bt docs/HARDWARE-TEST.md: "command write ... props=write-without-
// response"; joycon2cpp testapp.cpp:446 and controller.py via Bleak both write
// commands without response). Honoring a WriteWithResponse against a char that
// lacks the Write property fails at the ATT layer, so choose the option from the
// characteristic's actual properties: WriteWithResponse only when Write (0x8) is
// advertised, otherwise WriteWithoutResponse. The reply still arrives over the
// response notification, independent of the write acknowledgment.
static bool BLE_WriteCharacteristic(GattChar *characteristic, const Uint8 *bytes, int length, bool prefer_response)
{
    Buffer *buffer;
    void *op = NULL;
    bool result = false;
    int option = GattWriteOption_WriteWithoutResponse;

    if (!characteristic) {
        return false;
    }
    if (prefer_response) {
        enum __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattCharacteristicProperties props = 0;
        if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_get_CharacteristicProperties(characteristic, &props)) &&
            (props & GattCharacteristicProperties_Write)) {
            option = GattWriteOption_WriteWithResponse;
        }
    }
    buffer = BLE_BufferFromBytes(bytes, (UINT32)length);
    if (!buffer) {
        return false;
    }
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_WriteValueWithOptionAsync(characteristic, buffer, option, (void *)&op)) && op) {
        result = BLE_Await(op, &IID_AsyncStatusHandler);
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
        result = BLE_Await(op, &IID_AsyncStatusHandler);
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
    return E_NOINTERFACE;
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
// Command-response characteristic ValueChanged: stash the reply and signal.
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE ResponseHandler_Invoke(void *This, void *sender, GattValueArgs *args)
{
    BLE_Controller *ctrl = (BLE_Controller *)((void **)This)[1];
    Buffer *buffer = NULL;
    (void)sender;

    if (!args || !ctrl) {
        return S_OK;
    }
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattValueChangedEventArgs_get_CharacteristicValue(args, &buffer)) && buffer) {
        SDL_LockMutex(ctrl->response_lock);
        ctrl->response_size = BLE_BufferToBytes(buffer, ctrl->response, sizeof(ctrl->response));
        SDL_UnlockMutex(ctrl->response_lock);
        if (ctrl->response_size > 0) {
            SDL_SignalSemaphore(ctrl->response_sem);
        }
        __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release(buffer);
    }
    return S_OK;
}
static const struct
{
    void *QueryInterface;
    void *AddRef;
    void *Release;
    void *Invoke;
} g_response_vtbl = { (void *)InputHandler_QueryInterface, (void *)InputHandler_AddRef, (void *)InputHandler_Release, (void *)ResponseHandler_Invoke };

// Send a command and wait (briefly) for the reply on the response characteristic.
// Frame: [cmd] 0x91 0x01 [subcmd] 0x00 [data_len] 0x00 0x00 [data...]. Returns
// the number of reply bytes copied, or 0 on timeout.
static int BLE_SendCommand(BLE_Controller *ctrl, Uint8 cmd, Uint8 subcmd, const Uint8 *data, int data_len, Uint8 *reply, int reply_len)
{
    Uint8 frame[64];
    int got = 0;

    if (!ctrl->command_char || data_len < 0 || data_len + 8 > (int)sizeof(frame)) {
        return 0;
    }
    frame[0] = cmd;
    frame[1] = 0x91;
    frame[2] = 0x01; // Bluetooth transport
    frame[3] = subcmd;
    frame[4] = 0x00;
    frame[5] = (Uint8)data_len;
    frame[6] = 0x00;
    frame[7] = 0x00;
    if (data_len > 0) {
        SDL_memcpy(&frame[8], data, data_len);
    }
    while (SDL_TryWaitSemaphore(ctrl->response_sem)) {
        // drain stale replies
    }
    if (!BLE_WriteCharacteristic(ctrl->command_char, frame, 8 + data_len, true)) {
        return 0;
    }
    if (ctrl->response_sem && SDL_WaitSemaphoreTimeout(ctrl->response_sem, 500)) {
        SDL_LockMutex(ctrl->response_lock);
        got = ctrl->response_size;
        if (reply && reply_len > 0) {
            got = SDL_min(got, reply_len);
            SDL_memcpy(reply, ctrl->response, got);
        }
        SDL_UnlockMutex(ctrl->response_lock);
    }
    return got;
}

// Read controller memory, transcribed from the BLE reference controller.py
// read_memory (switch2-controllers/controller.py:339-347): command 0x02/0x04
// with data {length, 0x7e, 0, 0, addr LE}. controller.py strips an 8-byte header
// in write_command and another 8 in read_memory, so the payload sits at raw reply
// offset 0x10. Returns the number of payload bytes copied (0 on failure).
static int BLE_ReadMemory(BLE_Controller *ctrl, Uint8 length, Uint32 addr, Uint8 *out, int out_len)
{
    Uint8 req[8] = { length, 0x7e, 0x00, 0x00, (Uint8)addr, (Uint8)(addr >> 8), (Uint8)(addr >> 16), (Uint8)(addr >> 24) };
    Uint8 reply[128];
    int got = BLE_SendCommand(ctrl, 0x02, 0x04, req, (int)sizeof(req), reply, (int)sizeof(reply));
    int n;

    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 memory read addr 0x%06x len %d: %d reply bytes", (unsigned)addr, length, got);
    if (got > 0) {
        BLE_LogBytes("memory reply", reply, got); // confirms the 0x10 payload offset + MTU
    }
    if (got < 0x10) {
        return 0;
    }
    n = SDL_min(got - 0x10, (int)length);
    n = SDL_min(n, out_len);
    SDL_memcpy(out, &reply[0x10], n);
    return n;
}

// Read stick calibration, transcribed from controller.py read_calibration_data
// (switch2-controllers/controller.py:353-368): user slot first (0x1FC042 /
// 0x1FC062), falling back to factory (0x0130A8 / 0x0130E8) when the first 3 bytes
// read 0xFFFFFF; parse StickCalibrationData directly from the 9-byte payload. A
// Joy-Con stores its single-stick calibration in the first slot. The decoders use
// left_x/left_y for the single Joy-Con stick, so both L and R store there. Best-
// effort: on failure BLE_MapStickAxis falls back to a linear map.
static bool BLE_ReadCalibSlot(BLE_Controller *ctrl, Uint32 user_addr, Uint32 factory_addr, Uint8 *out9)
{
    Uint8 cal[16];
    if (BLE_ReadMemory(ctrl, 0x0b, user_addr, cal, sizeof(cal)) >= 9 &&
        !(cal[0] == 0xFF && cal[1] == 0xFF && cal[2] == 0xFF)) {
        SDL_memcpy(out9, cal, 9);
        return true;
    }
    if (BLE_ReadMemory(ctrl, 0x0b, factory_addr, cal, sizeof(cal)) >= 9) {
        SDL_memcpy(out9, cal, 9);
        return true;
    }
    return false;
}

static void BLE_ReadCalibration(BLE_Controller *ctrl)
{
    Uint8 slot1[9], slot2[9];

    if (BLE_ReadCalibSlot(ctrl, 0x1FC042, 0x0130A8, slot1)) {
        BLE_ParseStickCalibration(&ctrl->left_x, &ctrl->left_y, slot1);
    }
    // Pro/GameCube have a second stick; a Joy-Con uses only the first slot.
    if (ctrl->product_id != USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT &&
        ctrl->product_id != USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT &&
        BLE_ReadCalibSlot(ctrl, 0x1FC062, 0x0130E8, slot2)) {
        BLE_ParseStickCalibration(&ctrl->right_x, &ctrl->right_y, slot2);
    }
    ctrl->calibrated = true;
}

// Player LED (command 0x09 / subcmd 0x07). The wired UpdateSlotLED
// (SDL_hidapi_switch2.c:327-337) sends 8 data bytes with the pattern in data[0].
static void BLE_SetPlayerLED(BLE_Controller *ctrl, int player_index)
{
    static const Uint8 pattern[] = { 0x1, 0x3, 0x7, 0xf, 0x9, 0x5, 0xd, 0x6 };
    Uint8 data[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (player_index >= 0) {
        data[0] = pattern[player_index % 8];
    }
    BLE_SendCommand(ctrl, 0x09, 0x07, data, sizeof(data), NULL, 0);
}

// VibrationData 5-byte LE bit-pack, transcribed from controller.py
// VibrationData.get_bytes (switch2-controllers/controller.py:196-209): lf_freq
// at bits 0-8, en_lf at 9, lf_amp at 10-19, hf_freq at 20-28, en_hf at 29, hf_amp
// at 30-39. The BLE reference (virtual_controller.py:29-31) scales amplitude as
// 800*motor/256 (so ~0..800 of the 10-bit field) and leaves the en_tone bits 0.
static void BLE_EncodeVibration(Uint16 low, Uint16 high, Uint8 out[5])
{
    Uint32 lf_amp = (Uint32)((int)low * 800 / 65535) & 0x3FF;
    Uint32 hf_amp = (Uint32)((int)high * 800 / 65535) & 0x3FF;
    Uint64 v = (0x0E1ULL & 0x1FF) |        // lf_freq default (en_lf bit 9 stays 0)
               ((Uint64)lf_amp << 10) |    // lf_amp
               ((0x1E1ULL & 0x1FF) << 20) | // hf_freq default (en_hf bit 29 stays 0)
               ((Uint64)hf_amp << 30);     // hf_amp
    out[0] = (Uint8)v;
    out[1] = (Uint8)(v >> 8);
    out[2] = (Uint8)(v >> 16);
    out[3] = (Uint8)(v >> 24);
    out[4] = (Uint8)(v >> 32);
}

// Write a vibration packet, transcribed from controller.py set_vibration
// (switch2-controllers/controller.py:288-302): 0x00 + packet_id + the amplitude
// VibrationData + two default (zero-amplitude) VibrationData blocks. Pro repeats
// the 16-byte motor group (L then R). SDL re-calls Rumble periodically
// (SDL_RUMBLE_RESEND_MS), which sustains the effect.
static bool BLE_WriteRumble(BLE_Controller *ctrl, Uint16 low, Uint16 high)
{
    Uint8 group[16];
    Uint8 packet[33];
    Uint8 vib[5], zero[5];
    int len;

    if (!ctrl->vibration_char) {
        return false;
    }
    // The NSO GameCube controller uses a different 4-byte packed rumble format
    // (hid_reports.md:250-255), not the VibrationData motor group. Skip it rather
    // than send a malformed packet; GC rumble is a separate TODO.
    if (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER) {
        return false;
    }
    BLE_EncodeVibration(low, high, vib);
    BLE_EncodeVibration(0, 0, zero); // default VibrationData (default freq, 0 amp)
    group[0] = (Uint8)(0x50 | (ctrl->rumble_seq & 0x0F));
    SDL_memcpy(&group[1], vib, 5);
    SDL_memcpy(&group[6], zero, 5);
    SDL_memcpy(&group[11], zero, 5);
    ctrl->rumble_seq++;

    packet[0] = 0x00;
    SDL_memcpy(&packet[1], group, 16);
    len = 17;
    if (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_PRO) {
        SDL_memcpy(&packet[17], group, 16);
        len = 33;
    }
    return BLE_WriteCharacteristic(ctrl->vibration_char, packet, len, false);
}

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
                        bool supported = (vendor == USB_VENDOR_NINTENDO) &&
                                         (product == USB_PRODUCT_NINTENDO_SWITCH2_PRO ||
                                          product == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT ||
                                          product == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT ||
                                          product == USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER);
                        // Reserve the address so repeated advertisements during the
                        // multi-second connect don't start duplicate connects, then
                        // hand the connect to a worker thread so this WinRT callback
                        // returns immediately and the thread pool stays free to
                        // deliver the open/discovery completions.
                        if (supported && BLE_TryReserveConnect((Uint64)address)) {
                            BLE_ConnectRequest *req = (BLE_ConnectRequest *)SDL_malloc(sizeof(*req));
                            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 advertisement matched: VID %04x PID %04x addr %012llx", vendor, product, (unsigned long long)address);
                            if (req) {
                                SDL_Thread *thread;
                                req->address = (Uint64)address;
                                req->vendor = vendor;
                                req->product = product;
                                thread = SDL_CreateThread(BLE_ConnectThread, "BLESwitch2Connect", req);
                                if (thread) {
                                    SDL_DetachThread(thread);
                                } else {
                                    SDL_free(req);
                                    BLE_ReleaseConnect((Uint64)address);
                                }
                            } else {
                                BLE_ReleaseConnect((Uint64)address);
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

    // Uncached, so the read goes to the device rather than a stale OS cache, and
    // only trust the result when GattCommunicationStatus_Success (joycon2cpp checks
    // cr.Status() before reading characteristics, testapp.cpp:854). The link is up
    // by now (service discovery already succeeded), so a single attempt suffices.
    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3_GetCharacteristicsForUuidWithCacheModeAsync(service3, *uuid, BluetoothCacheMode_Uncached, (void *)&op)) || !op) {
        return NULL;
    }
    if (BLE_Await(op, &IID_AsyncCharsHandler)) {
        // GetResults is vtbl slot 8 on every IAsyncOperation<T>.
        typedef HRESULT(STDMETHODCALLTYPE * GetResults_t)(void *This, GattCharsResult **out);
        void ***vt = (void ***)op;
        ((GetResults_t)(*vt)[8])(op, &result);
    }
    if (result) {
        GattCommStatus status = GattCommunicationStatus_Unreachable;
        __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCharacteristic *chars = NULL;
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicsResult_get_Status(result, &status);
        if (status == GattCommunicationStatus_Success &&
            SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicsResult_get_Characteristics(result, &chars)) && chars) {
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

// Await an IAsyncOperation<BluetoothLEDevice>, return the device (NULL on
// timeout/failure), and release the operation. GetResults is vtbl slot 8. The
// cold device open can take several seconds, so it gets a generous 20 s ceiling
// (matching windows10-gyro controller.py connect(timeout=20.0)), not the 3 s
// per-op default.
static BleDevice *BLE_AwaitDevice(void *op)
{
    BleDevice *device = NULL;
    if (!op) {
        return NULL;
    }
    if (BLE_AwaitTimeout(op, 20000, &IID_AsyncDeviceHandler)) {
        typedef HRESULT(STDMETHODCALLTYPE * GetResults_t)(void *This, BleDevice **out);
        void ***vt = (void ***)op;
        HRESULT gr = ((GetResults_t)(*vt)[8])(op, &device);
        // Diagnostic (hifihedgehog/SDL#5): pin candidate (c). A returning-fast wait
        // with GetResults S_OK but device NULL means a synchronous null completion.
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE device open: GetResults hr=0x%08lX device=%p",
                     (unsigned long)gr, (void *)device);
    }
    ((BleDevice *)op)->lpVtbl->Release((BleDevice *)op);
    return device;
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
    InputHandlerObj *response_handler = NULL;

    if (FAILED(BLE_GetActivationFactory(RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice, &IID_BleDeviceStatics, (void **)&statics))) {
        return;
    }
    // Open by raw address and let the OS resolve the address type. The Switch 2 Pro
    // Controller advertises a Public address (switch2-bt PHASE_B_LOG.md:48, and the
    // live trace reported type=0), and this single-arg open is exactly joycon2cpp's
    // working call (testapp.cpp:827). This runs on a dedicated worker thread (see
    // Received_Invoke), so blocking the 20 s await here does not starve the WinRT
    // thread pool that has to deliver the completion.
    op = NULL;
    {
        // Diagnostic (hifihedgehog/SDL#5): pin candidate (a). A failed HRESULT here
        // skips the await entirely, so device stays NULL with no wait.
        HRESULT open_hr = __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDeviceStatics_FromBluetoothAddressAsync(statics, bluetooth_address, (void *)&op);
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE device open: FromBluetoothAddressAsync hr=0x%08lX op=%p",
                     (unsigned long)open_hr, (void *)op);
        if (SUCCEEDED(open_hr)) {
            device = BLE_AwaitDevice(op);
        }
    }
    __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDeviceStatics_Release(statics);
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 open addr %012llx: device=%p",
                 (unsigned long long)bluetooth_address, (void *)device);
    if (!device) {
        return;
    }

    // Discover the Switch 2 service. This driver connects bond-free (no SMP, no OS
    // pairing), so the first GATT query returns before the ACL/GATT link is up and
    // a Cached read sees an empty table. Query Uncached and retry until the result
    // is GattCommunicationStatus_Success with the service present, matching the
    // proven joycon2cpp connect, which loops GetGattServicesAsync(Uncached) up to
    // 10x at 500 ms checking Success (joycon2cpp/testapp/src/testapp.cpp:841-851).
    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_QueryInterface(device, &IID_BleDevice3, (void **)&device3)) || !device3) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(device);
        return;
    }
    {
        int attempt;
        for (attempt = 1; attempt <= 10 && !service; ++attempt) {
            GattCommStatus status = GattCommunicationStatus_Unreachable;
            unsigned size = 0;
            op = NULL;
            services_result = NULL;
            if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3_GetGattServicesForUuidWithCacheModeAsync(device3, GUID_Switch2Service, BluetoothCacheMode_Uncached, (void *)&op)) && op) {
                if (BLE_Await(op, &IID_AsyncServicesHandler)) {
                    typedef HRESULT(STDMETHODCALLTYPE * GetResults_t)(void *This, GattServicesResult **out);
                    void ***vt = (void ***)op;
                    ((GetResults_t)(*vt)[8])(op, &services_result);
                }
                ((GattServicesResult *)op)->lpVtbl->Release((GattServicesResult *)op);
            }
            if (services_result) {
                __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServicesResult_get_Status(services_result, &status);
                if (status == GattCommunicationStatus_Success) {
                    __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService *list = NULL;
                    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServicesResult_get_Services(services_result, &list)) && list) {
                        __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService_get_Size(list, &size);
                        if (size > 0) {
                            __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService_GetAt(list, 0, &service);
                        }
                        __FIVectorView_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceService_Release(list);
                    }
                }
                __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServicesResult_Release(services_result);
                services_result = NULL;
            }
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 GATT discovery attempt %d/10: status=%d services=%u",
                         attempt, (int)status, size);
            if (!service && attempt < 10) {
                SDL_Delay(500); // ride out the bond-free link-up window
            }
        }
    }
    if (service) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService_QueryInterface(service, &IID_GattService3, (void **)&service3);
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService_Release(service);
    }
    if (!service3) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 GATT service discovery failed after 10 attempts");
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3_Release(device3);
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(device);
        return;
    }

    // Best-effort throughput bump, now that the link is up. joycon2cpp requests it
    // after discovery (testapp.cpp:891). Win10 1809+; ignore failure.
    {
        BleDevice6 *device6 = NULL;
        if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_QueryInterface(device, &IID_BleDevice6, (void **)&device6))) {
            BleConnParamStatics *cp_statics = NULL;
            if (SUCCEEDED(BLE_GetActivationFactory(RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEPreferredConnectionParameters, &IID_BleConnParamStatics, (void **)&cp_statics))) {
                BleConnParam *params = NULL;
                if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEPreferredConnectionParametersStatics_get_ThroughputOptimized(cp_statics, &params)) && params) {
                    BleConnParamReq *req = NULL;
                    __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice6_RequestPreferredConnectionParameters(device6, params, &req);
                    if (req) {
                        ((BleConnParamReq *)req)->lpVtbl->Release((BleConnParamReq *)req);
                    }
                    __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEPreferredConnectionParameters_Release(params);
                }
                __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEPreferredConnectionParametersStatics_Release(cp_statics);
            }
            __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice6_Release(device6);
        }
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
    if (name) {
        ctrl->name = name;
    } else {
        const char *type_name;
        switch (product_id) {
        case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT:
            type_name = "Nintendo Switch 2 Joy-Con (L)";
            break;
        case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT:
            type_name = "Nintendo Switch 2 Joy-Con (R)";
            break;
        case USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER:
            type_name = "Nintendo Switch 2 GameCube Controller";
            break;
        default:
            type_name = "Nintendo Switch 2 Pro Controller";
            break;
        }
        ctrl->name = SDL_strdup(type_name);
    }
    ctrl->device = device;
    device = NULL; // ownership transferred to ctrl; cleanup releases via ctrl
    ctrl->instance_id = SDL_GetNextObjectID();
    ctrl->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, vendor_id, product_id, 0, NULL, ctrl->name, 'h', 0);

    ctrl->response_lock = SDL_CreateMutex();
    ctrl->response_sem = SDL_CreateSemaphore(0);
    ctrl->player_index = -1;

    ctrl->input_char = BLE_FindCharacteristic(service3, &GUID_Switch2Input);
    ctrl->command_char = BLE_FindCharacteristic(service3, &GUID_Switch2Command);
    ctrl->response_char = BLE_FindCharacteristic(service3, &GUID_Switch2CmdResponse);
    switch (product_id) {
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT:
        ctrl->vibration_char = BLE_FindCharacteristic(service3, &GUID_Switch2VibeJCL);
        break;
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT:
        ctrl->vibration_char = BLE_FindCharacteristic(service3, &GUID_Switch2VibeJCR);
        break;
    default:
        ctrl->vibration_char = BLE_FindCharacteristic(service3,
            (product_id == USB_PRODUCT_NINTENDO_SWITCH2_PRO) ? &GUID_Switch2VibePro : &GUID_Switch2VibeGC);
        break;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 connected: chars input=%d command=%d response=%d vibration=%d",
                 ctrl->input_char != NULL, ctrl->command_char != NULL, ctrl->response_char != NULL, ctrl->vibration_char != NULL);

    // Connect sequence per controller.py connect() (switch2-controllers/
    // controller.py:253-265): enable the command-response notification first, then
    // read calibration over the command channel, and only THEN enable the input
    // report notification.
    if (ctrl->response_char) {
        response_handler = (InputHandlerObj *)SDL_calloc(1, sizeof(*response_handler));
        if (response_handler) {
            response_handler->vtbl = (void *)&g_response_vtbl;
            response_handler->ctrl = ctrl;
            ctrl->response_handler = response_handler;
            __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_add_ValueChanged(ctrl->response_char, (void *)response_handler, &ctrl->response_token);
            BLE_EnableNotifications(ctrl->response_char);
        }
    }

    // Read stick calibration over the command channel (best-effort).
    BLE_ReadCalibration(ctrl);

    if (ctrl->input_char) {
        input_handler = (InputHandlerObj *)SDL_calloc(1, sizeof(*input_handler));
        if (input_handler) {
            input_handler->vtbl = (void *)&g_input_vtbl;
            input_handler->ctrl = ctrl;
            ctrl->input_handler = input_handler;
            // Register the handler before enabling notifications (controller.py order).
            __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_add_ValueChanged(ctrl->input_char, (void *)input_handler, &ctrl->input_token);
            BLE_EnableNotifications(ctrl->input_char);
        }
    }

    SDL_LockJoysticks();
    {
        // Bail if a teardown ran while this connect was in WinRT discovery.
        // SDL_QuitJoysticks holds SDL_LockJoysticks across driver->Quit(), and
        // BLE_JoystickQuit clears ble.initialized last under that same lock
        // (SDL_joystick.c:2275-2294). A connect callback that was blocked here
        // during shutdown must not re-grow the freed ble.controllers array or
        // register a joystick after SDL_joysticks_quitting is set. ctrl stays
        // non-NULL and is torn down via the cleanup path below.
        BLE_Controller **grown = ble.initialized
            ? (BLE_Controller **)SDL_realloc(ble.controllers, sizeof(ble.controllers[0]) * (ble.controller_count + 1))
            : NULL;
        // Re-check under the lock: another advertisement callback on a second
        // thread-pool thread may have connected the same device concurrently.
        if (grown) {
            ble.controllers = grown;
            if (!BLE_GetControllerByAddress(bluetooth_address)) {
                ble.controllers[ble.controller_count++] = ctrl;
                SDL_PrivateJoystickAdded(ctrl->instance_id);
                ctrl = NULL; // owned by the array now
            }
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
        // Failed to publish (calloc failure or a concurrent duplicate connect).
        // Full teardown: unregister the ValueChanged handlers, release the
        // characteristics and device, free the handlers/locks. ctrl->device == device
        // at this point, so this also releases device.
        BLE_FreeController(ctrl);
    } else if (device) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(device);
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
        SDL_SendJoystickButton(timestamp, joystick, 11 /* Share */, ((data[5] & 0x20) != 0));
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
                         BLE_MapStickAxis(&ctrl->left_x, (float)(data[10] | ((data[11] & 0x0F) << 8)), false));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY,
                         BLE_MapStickAxis(&ctrl->left_y, (float)((data[11] >> 4) | (data[12] << 4)), true));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX,
                         BLE_MapStickAxis(&ctrl->right_x, (float)(data[13] | ((data[14] & 0x0F) << 8)), false));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY,
                         BLE_MapStickAxis(&ctrl->right_y, (float)((data[14] >> 4) | (data[15] << 4)), true));

    // IMU, transcribed from the wired HandleStatePacket (SDL_hidapi_switch2.c
    // :1379-1392) at BLE offset -1. The axes are NOT sequential: accel maps
    // X<-0x31, Y<-0x35, Z<-0x33(neg); gyro X<-0x37, Y<-0x3b, Z<-0x39(neg). The
    // wired gyro_coeff is 34.8 (its dynamic 34.8-vs-40 calibration is not ported;
    // 34.8 is the nominal case, flagged for hardware confirmation). Biases are 0.
    if (size >= 60 && ctrl->joystick && ctrl->sensors_enabled) {
        const float accel_scale = SDL_STANDARD_GRAVITY * 8.0f / 32767.0f;
        const float gyro_scale = 34.8f / 32767.0f;
        float accel[3], gyro[3];
        accel[0] = (Sint16)(data[48] | (data[49] << 8)) * accel_scale;
        accel[1] = (Sint16)(data[52] | (data[53] << 8)) * accel_scale;
        accel[2] = (Sint16)(data[50] | (data[51] << 8)) * -accel_scale;
        gyro[0] = (Sint16)(data[54] | (data[55] << 8)) * gyro_scale;
        gyro[1] = (Sint16)(data[58] | (data[59] << 8)) * gyro_scale;
        gyro[2] = (Sint16)(data[56] | (data[57] << 8)) * -gyro_scale;
        SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_ACCEL, timestamp, accel, 3);
        SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_GYRO, timestamp, gyro, 3);
    }

    SDL_memcpy(ctrl->last_state, data, SDL_min(size, (int)sizeof(ctrl->last_state)));
    ctrl->have_last_state = true;
}

// GameCube button indices (the wired SDL_GAMEPAD_BUTTON_SWITCH2_GAMECUBE_* enums
// are file-static there). Joy-Con extras: SHARE=11, C=12, paddles 13..16.
enum { GC_GUIDE = 4, GC_START, GC_LSHOULDER, GC_RSHOULDER, GC_SHARE, GC_C, GC_LTRIGGER, GC_RTRIGGER };

static Sint16 BLE_RemapTrigger(Uint8 value)
{
    float t = SDL_clamp((float)value / 232.0f, 0.0f, 1.0f);
    return (Sint16)(SDL_MIN_SINT16 + t * ((float)SDL_MAX_SINT16 - (float)SDL_MIN_SINT16));
}

// All BLE decoders run at offset -1 vs the wired report (BLE omits the report-ID
// prefix). SDL_SendJoystick* filters unchanged values, so we emit every report.
static void BLE_DecodeGameCube(BLE_Controller *ctrl, SDL_Joystick *joystick, Uint8 *data, int size)
{
    Uint64 ts = SDL_GetTicksNS();
    Uint8 hat = 0;
    if (size < 62) {
        return;
    }
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[4] & 0x01) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[4] & 0x02) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[4] & 0x04) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[4] & 0x08) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_RTRIGGER, ((data[4] & 0x40) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_RSHOULDER, ((data[4] & 0x80) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_START, ((data[5] & 0x02) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_GUIDE, ((data[5] & 0x10) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_SHARE, ((data[5] & 0x20) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_C, ((data[5] & 0x40) != 0));
    if (data[6] & 0x01) { hat |= SDL_HAT_DOWN; }
    if (data[6] & 0x02) { hat |= SDL_HAT_UP; }
    if (data[6] & 0x04) { hat |= SDL_HAT_RIGHT; }
    if (data[6] & 0x08) { hat |= SDL_HAT_LEFT; }
    SDL_SendJoystickHat(ts, joystick, 0, hat);
    SDL_SendJoystickButton(ts, joystick, GC_LTRIGGER, ((data[6] & 0x40) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_LSHOULDER, ((data[6] & 0x80) != 0));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, BLE_RemapTrigger(data[60]));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, BLE_RemapTrigger(data[61]));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTX, BLE_MapStickAxis(&ctrl->left_x, (float)(data[10] | ((data[11] & 0x0F) << 8)), false));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTY, BLE_MapStickAxis(&ctrl->left_y, (float)((data[11] >> 4) | (data[12] << 4)), true));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_RIGHTX, BLE_MapStickAxis(&ctrl->right_x, (float)(data[13] | ((data[14] & 0x0F) << 8)), false));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_RIGHTY, BLE_MapStickAxis(&ctrl->right_y, (float)((data[14] >> 4) | (data[15] << 4)), true));
}

// Standalone (mini) Joy-Con 2 Left, held sideways.
static void BLE_DecodeJoyConLeft(BLE_Controller *ctrl, SDL_Joystick *joystick, Uint8 *data, int size)
{
    Uint64 ts = SDL_GetTicksNS();
    if (size < 14) {
        return;
    }
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_START, ((data[5] & 0x01) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data[5] & 0x08) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[5] & 0x20) != 0));
    SDL_SendJoystickButton(ts, joystick, 11 /* JoyCon Share */, ((data[5] & 0x10) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[6] & 0x01) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[6] & 0x02) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[6] & 0x04) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[6] & 0x08) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data[6] & 0x10) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[6] & 0x20) != 0));
    SDL_SendJoystickButton(ts, joystick, 14 /* JoyCon left paddle 1 */, ((data[6] & 0x40) != 0));
    SDL_SendJoystickButton(ts, joystick, 16 /* JoyCon left paddle 2 */, ((data[6] & 0x80) != 0));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTX, BLE_MapStickAxis(&ctrl->left_y, (float)((data[11] >> 4) | (data[12] << 4)), true));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTY, BLE_MapStickAxis(&ctrl->left_x, (float)(data[10] | ((data[11] & 0x0F) << 8)), true));
}

// Standalone (mini) Joy-Con 2 Right, held sideways.
static void BLE_DecodeJoyConRight(BLE_Controller *ctrl, SDL_Joystick *joystick, Uint8 *data, int size)
{
    Uint64 ts = SDL_GetTicksNS();
    if (size < 16) {
        return;
    }
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[4] & 0x01) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[4] & 0x02) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[4] & 0x04) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[4] & 0x08) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data[4] & 0x10) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[4] & 0x20) != 0));
    SDL_SendJoystickButton(ts, joystick, 13 /* JoyCon right paddle 1 */, ((data[4] & 0x40) != 0));
    SDL_SendJoystickButton(ts, joystick, 15 /* JoyCon right paddle 2 */, ((data[4] & 0x80) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_START, ((data[5] & 0x02) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data[5] & 0x04) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[5] & 0x10) != 0));
    SDL_SendJoystickButton(ts, joystick, 12 /* JoyCon C */, ((data[5] & 0x40) != 0));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTX, BLE_MapStickAxis(&ctrl->left_y, (float)((data[14] >> 4) | (data[15] << 4)), false));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTY, BLE_MapStickAxis(&ctrl->left_x, (float)(data[13] | ((data[14] & 0x0F) << 8)), false));
}

static void BLE_DecodeReport(BLE_Controller *ctrl, SDL_Joystick *joystick, Uint8 *data, int size)
{
    switch (ctrl->product_id) {
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT:
        BLE_DecodeJoyConLeft(ctrl, joystick, data, size);
        break;
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT:
        BLE_DecodeJoyConRight(ctrl, joystick, data, size);
        break;
    default: // Pro Controller and GameCube share the full layout
        if (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_PRO) {
            BLE_DecodeProReport(ctrl, joystick, data, size);
        } else {
            BLE_DecodeGameCube(ctrl, joystick, data, size);
        }
        break;
    }
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
    RESOLVE(RoInitialize);
    RESOLVE(RoUninitialize);
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
    if (device_index >= 0 && device_index < ble.controller_count) {
        BLE_Controller *ctrl = ble.controllers[device_index];
        ctrl->player_index = player_index;
        BLE_SetPlayerLED(ctrl, player_index);
    }
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

    switch (ctrl->product_id) {
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT:
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT:
        joystick->nbuttons = 17; // SDL_GAMEPAD_NUM_SWITCH2_JOYCON_BUTTONS
        break;
    default:
        joystick->nbuttons = (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_PRO) ? 15 : 12;
        break;
    }
    joystick->naxes = SDL_GAMEPAD_AXIS_COUNT;
    joystick->nhats = 1;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;

    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, 250.0f);
    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 250.0f);

    // Light the player LED for this slot.
    ctrl->player_index = SDL_GetJoystickPlayerIndex(joystick);
    BLE_SetPlayerLED(ctrl, ctrl->player_index);
    return true;
}

static bool BLE_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    if (!ctrl || !ctrl->vibration_char) {
        return SDL_Unsupported();
    }
    return BLE_WriteRumble(ctrl, low_frequency_rumble, high_frequency_rumble);
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
    return SDL_Unsupported(); // No RGB; player LED is set from the player index
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
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    // Enable/disable the IMU via the BLE reference's enableFeatures
    // (controller.py:370-373): command 0x0c/0x02 (init) then 0x0c/0x04 (enable),
    // both carrying the feature mask. FEATURE_MOTION = 0x04 (controller.py:59).
    // The wired driver's 0x27 is a USB-path value and sets unrelated bits here.
    Uint8 flags[4] = { enabled ? (Uint8)0x04 : (Uint8)0x00, 0x00, 0x00, 0x00 };
    if (!ctrl) {
        return SDL_SetError("No BLE controller for joystick");
    }
    ctrl->sensors_enabled = enabled;
    BLE_SendCommand(ctrl, 0x0c, 0x02, flags, sizeof(flags), NULL, 0);
    BLE_SendCommand(ctrl, 0x0c, 0x04, flags, sizeof(flags), NULL, 0);
    return true;
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
        // The BLE notification is the raw report (no report-ID prefix); the
        // decoders use absolute BLE offsets. Dispatch by controller type.
        if (!ctrl->logged_first_report) {
            ctrl->logged_first_report = true;
            BLE_LogBytes("first input report", data, size); // confirms the byte offsets
        }
        BLE_DecodeReport(ctrl, joystick, data, size);
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
    // Stop new notifications and release the WinRT COM objects. An in-flight
    // ValueChanged callback uses only its event args and the controller's own
    // buffers, not these objects, so releasing them here is safe. (WinRT holds
    // its own reference to the characteristic for the duration of a callback.)
    if (ctrl->input_char) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_remove_ValueChanged(ctrl->input_char, ctrl->input_token);
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(ctrl->input_char);
    }
    if (ctrl->command_char) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(ctrl->command_char);
    }
    if (ctrl->response_char) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_remove_ValueChanged(ctrl->response_char, ctrl->response_token);
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(ctrl->response_char);
    }
    if (ctrl->vibration_char) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(ctrl->vibration_char);
    }
    if (ctrl->device) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(ctrl->device);
    }

    // Deliberately leak the controller struct, its locks/semaphore, buffers, and
    // the delegates. A GATT ValueChanged callback may still be in-flight on a
    // WinRT thread-pool thread and touches exactly these, and GATT
    // remove_ValueChanged does not drain in-flight invocations. Destroying the
    // mutex or freeing the struct here would be a use-after-free. The leak is
    // bounded (one controller per connection attempt, reclaimed at process exit);
    // a future refcounted-controller design could free it deterministically.
    (void)ctrl;
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
