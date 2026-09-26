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
 * PadForge fork: the WinRT Bluetooth LE GATT transport shared by the Switch 2
 * driver (SDL_ble_switch2joystick.c, hifihedgehog/SDL#5 and #9) and the
 * generic BLE GATT driver (SDL_blegattjoystick.c) of hifihedgehog/SDL#33
 * Part 12. The activation and await helpers, the buffers, the delegates, the
 * advertisement watcher and the open and uncached discovery were moved here
 * from the Switch 2 driver, which proved them on hardware. The Switch 2
 * driver keeps its own connect sequence and calls the lower-level helpers.
 * The generic driver calls the SDL_BLEGATTLink API.
 */

#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_BLE

#include "../../core/windows/SDL_windows.h"

// The GATT session, pairing and connection-parameter interfaces need a high API contract.
#ifndef WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION
#define WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION 0xe0000
#endif

#define COBJMACROS
#include <windows.devices.bluetooth.h>
#include <windows.devices.bluetooth.advertisement.h>
#include <windows.devices.bluetooth.genericattributeprofile.h>
#include <windows.devices.enumeration.h>
#include <windows.storage.streams.h>
#include <windows.foundation.h>
#include <roapi.h>
#include <objidlbase.h>

#include "SDL_ble_gatt.h"

#include <initguid.h>

// ---------------------------------------------------------------------------
// Short aliases for the very long WinRT-from-C symbol names.
// ---------------------------------------------------------------------------

typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher            BleWatcher;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher2           BleWatcher2;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementReceivedEventArgs  BleRecvArgs;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementReceivedEventArgs2 BleRecvArgs2;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisement                   BleAdvertisement;
typedef enum __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CBluetoothLEAdvertisementWatcherStatus   BleWatcherStatus;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEManufacturerData                BleMfgData;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDeviceStatics                    BleDeviceStatics;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice                           BleDevice;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice2                          BleDevice2;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3                          BleDevice3;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice4                          BleDevice4;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice6                          BleDevice6;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothDeviceId                           BleDeviceId;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEPreferredConnectionParametersStatics BleConnParamStatics;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEPreferredConnectionParameters    BleConnParam;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEPreferredConnectionParametersRequest BleConnParamReq;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServicesResult                  GattServicesResult;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService                         GattService;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3                        GattService3;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicsResult                 GattCharsResult;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic                        GattChar;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic3                       GattChar3;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattValueChangedEventArgs                 GattValueArgs;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattCommunicationStatus                    GattCommStatus;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattWriteResult                           GattWriteResult;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattReadResult                            GattReadResult;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattSession                               GattSession;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattSessionStatics                        GattSessionStatics;
typedef __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformation                          DeviceInfo;
typedef __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformation2                         DeviceInfo2;
typedef __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationPairing                   DevicePairing;
typedef __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationPairing2                  DevicePairing2;
typedef __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationCustomPairing             DeviceCustomPairing;
typedef __x_ABI_CWindows_CDevices_CEnumeration_CIDevicePairingRequestedEventArgs            PairingRequestedArgs;
typedef __x_ABI_CWindows_CDevices_CEnumeration_CIDevicePairingResult                        PairingResult;
typedef __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceUnpairingResult                      UnpairingResult;
typedef __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCommunicationStatus StatusOp;
typedef __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattWriteResult         WriteResultOp;
typedef __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattReadResult          ReadResultOp;
typedef __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattSession             SessionOp;
typedef __FIAsyncOperation_1_Windows__CDevices__CEnumeration__CDevicePairingResult                             PairingOp;
typedef __FIAsyncOperation_1_Windows__CDevices__CEnumeration__CDeviceUnpairingResult                           UnpairingOp;
typedef __x_ABI_CWindows_CStorage_CStreams_CIBuffer                                 Buffer;
typedef __x_ABI_CWindows_CStorage_CStreams_CIDataWriter                             DataWriter;
typedef __x_ABI_CWindows_CFoundation_CIClosable                                     Closable;

// ---------------------------------------------------------------------------
// IIDs. The WinRT C headers ship only declarations, so we define the GUIDs
// ourselves (same approach as SDL_windows_gaming_input.c). The file:line after
// each is where the Windows SDK 10.0.26100.0 header declares it (bt.h is
// windows.devices.bluetooth.h, adv.h the advertisement header, gatt.h the
// genericattributeprofile header, enum.h windows.devices.enumeration.h).
// ---------------------------------------------------------------------------
DEFINE_GUID(IID_BleWatcher,       0xa6ac336f, 0xf3d3, 0x4297, 0x8d, 0x6c, 0xc8, 0x1e, 0xa6, 0x62, 0x3f, 0x40); // adv.h:2483
DEFINE_GUID(IID_BleWatcher2,      0x01bf26bc, 0xb164, 0x5805, 0x90, 0xa3, 0xe8, 0xa7, 0x99, 0x7f, 0xf2, 0x25); // adv.h:2567
// ITypedEventHandler<BluetoothLEAdvertisementWatcher*, BluetoothLEAdvertisementReceivedEventArgs*>
DEFINE_GUID(IID_BleRecvHandler,   0x90eb4eca, 0xd465, 0x5ea0, 0xa6, 0x1c, 0x03, 0x3c, 0x8c, 0x5e, 0xce, 0xf2); // adv.h:1151
DEFINE_GUID(IID_BleRecvArgs,      0x27987ddf, 0xe596, 0x41be, 0x8d, 0x43, 0x9e, 0x67, 0x31, 0xd4, 0xa9, 0x13); // adv.h:2254
DEFINE_GUID(IID_BleRecvArgs2,     0x12d9c87b, 0x0399, 0x5f0e, 0xa3, 0x48, 0x53, 0xb0, 0x2b, 0x6b, 0x16, 0x2e); // adv.h:2304
DEFINE_GUID(IID_BleAdvertisement, 0x066fb2b7, 0x33d1, 0x4e7d, 0x83, 0x67, 0xcf, 0x81, 0xd0, 0xf7, 0x96, 0x53); // adv.h:1590
DEFINE_GUID(IID_BleMfgData,       0x912dba18, 0x6963, 0x4533, 0xb0, 0x61, 0x46, 0x94, 0xda, 0xfb, 0x34, 0xe5); // adv.h:2744
DEFINE_GUID(IID_BleDeviceStatics, 0xc8cf1a19, 0xf0b6, 0x4bf0, 0x86, 0x89, 0x41, 0x30, 0x3d, 0xe2, 0xd9, 0xf4); // bt.h:3657
DEFINE_GUID(IID_BleDevice,        0xb5ee2f7b, 0x4ad8, 0x4642, 0xac, 0x48, 0x80, 0xa0, 0xb5, 0x00, 0xe8, 0x87); // bt.h:3352
DEFINE_GUID(IID_BleDevice2,       0x26f062b3, 0x7aee, 0x4d31, 0xba, 0xba, 0xb1, 0xb9, 0x77, 0x5f, 0x59, 0x16); // bt.h:3431
DEFINE_GUID(IID_BleDevice3,       0xaee9e493, 0x44ac, 0x40dc, 0xaf, 0x33, 0xb2, 0xc1, 0x3c, 0x01, 0xca, 0x46); // bt.h:3473
DEFINE_GUID(IID_BleDevice4,       0x2b605031, 0x2248, 0x4b2f, 0xac, 0xf0, 0x7c, 0xee, 0x36, 0xfc, 0x58, 0x70); // bt.h:3528
DEFINE_GUID(IID_BleDevice6,       0xca7190ef, 0x0cae, 0x573c, 0xa1, 0xca, 0xe1, 0xfc, 0x5b, 0xfc, 0x39, 0xe2); // bt.h:3600
DEFINE_GUID(IID_BleConnParamStatics, 0x0e3e8edc, 0x2751, 0x55aa, 0xa8, 0x38, 0x8f, 0xae, 0xee, 0x81, 0x8d, 0x72); // bt.h:3845
DEFINE_GUID(IID_GattService3,     0xb293a950, 0x0c53, 0x437c, 0xa9, 0xb3, 0x5c, 0x32, 0x10, 0xc6, 0xe5, 0x69); // gatt.h:5147
DEFINE_GUID(IID_GattChar,         0x59cb50c1, 0x5934, 0x4f68, 0xa1, 0x98, 0xeb, 0x86, 0x4f, 0xa4, 0x4e, 0x6b); // gatt.h:4102
DEFINE_GUID(IID_GattChar3,        0x3f3c663e, 0x93d4, 0x406b, 0xb8, 0x17, 0xdb, 0x81, 0xf8, 0xed, 0x53, 0xb3); // gatt.h:4242
// ITypedEventHandler<GattCharacteristic*, GattValueChangedEventArgs*> (ValueChanged)
DEFINE_GUID(IID_GattValueHandler, 0xc1f420f6, 0x6292, 0x5760, 0xa2, 0xc9, 0x9d, 0xdf, 0x98, 0x68, 0x3c, 0xfc); // gatt.h:3087
// ITypedEventHandler<BluetoothLEDevice*, IInspectable*> (ConnectionStatusChanged),
// declspec(uuid) on the specialization, windows.devices.bluetooth.h:1597.
DEFINE_GUID(IID_BleStatusHandler, 0xa90661e2, 0x372e, 0x5d1e, 0xbb, 0xbb, 0xb8, 0xa2, 0xce, 0x0e, 0x7c, 0x4d);
DEFINE_GUID(IID_GattValueArgs,    0xd21bdb54, 0x06e3, 0x4ed8, 0xa2, 0x63, 0xac, 0xfa, 0xc8, 0xba, 0x73, 0x13); // gatt.h:7192
DEFINE_GUID(IID_GattSessionStatics, 0x2e65b95c, 0x539f, 0x4db7, 0x82, 0xa8, 0x73, 0xbd, 0xbb, 0xf7, 0x3e, 0xbf); // gatt.h:7064
DEFINE_GUID(IID_DeviceInfo2,      0xf156a638, 0x7997, 0x48d9, 0xa1, 0x0c, 0x26, 0x9d, 0x46, 0x53, 0x3f, 0x48); // enum.h:3043
DEFINE_GUID(IID_DevicePairing2,   0xf68612fd, 0x0aee, 0x4328, 0x85, 0xcc, 0x1c, 0x74, 0x2b, 0xb1, 0x79, 0x0d); // enum.h:3226
// ITypedEventHandler<DeviceInformationCustomPairing*, DevicePairingRequestedEventArgs*> (PairingRequested)
DEFINE_GUID(IID_PairingRequestedHandler, 0xfa65231f, 0x4178, 0x5de1, 0xb2, 0xcc, 0x03, 0xe2, 0x2d, 0x77, 0x02, 0xb4); // enum.h:1675
DEFINE_GUID(IID_DataWriter,       0x64b89265, 0xd341, 0x4922, 0xb3, 0x8a, 0xdd, 0x4a, 0xf8, 0x80, 0x8c, 0x4e); // windows.storage.streams.h:1663
DEFINE_GUID(IID_IBufferByteAccess, 0x905a0fef, 0xbc53, 0x11df, 0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda); // robuffer.h:27
DEFINE_GUID(IID_BleAsyncInfo,     0x00000036, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46); // asyncinfo.h:114
DEFINE_GUID(IID_Closable,         0x30d5a829, 0x7fa4, 0x4026, 0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e); // windows.foundation.h:1291
// Parameterized IAsyncOperationCompletedHandler<T> IIDs (PIIDs). The shared
// awaiter must QI-accept exactly the handler IID put_Completed asks for, like the
// WGI driver's per-delegate QI (SDL_windows_gaming_input.c:340-342). These are the
// CompletedHandler IIDs, NOT the IAsyncOperation<T> IIDs that sit on the adjacent
// near-identically-named struct in the SDK headers (using the operation IID makes
// put_Completed return E_NOINTERFACE).
DEFINE_GUID(IID_AsyncDeviceHandler,   0x9156b79f, 0xc54a, 0x5277, 0x8f, 0x8b, 0xd2, 0xcc, 0x43, 0xc7, 0xe0, 0x04); // <BluetoothLEDevice> bt.h:833
DEFINE_GUID(IID_AsyncServicesHandler, 0x74ab0892, 0xa631, 0x5d6c, 0xb1, 0xb4, 0xbd, 0x2e, 0x1a, 0x74, 0x1a, 0x9b); // <GattDeviceServicesResult> bt.h:916
DEFINE_GUID(IID_AsyncCharsHandler,    0xd6a15475, 0x1e72, 0x5c56, 0x98, 0xe8, 0x88, 0xf4, 0xbc, 0x3e, 0x03, 0x13); // <GattCharacteristicsResult> gatt.h:1167
DEFINE_GUID(IID_AsyncStatusHandler,   0x2154117a, 0x978d, 0x59db, 0x99, 0xcf, 0x6b, 0x69, 0x0c, 0xb3, 0x38, 0x9b); // <GattCommunicationStatus> gatt.h:1299
DEFINE_GUID(IID_AsyncReadHandler,     0xd8992aa0, 0xeac2, 0x55b7, 0x92, 0xc5, 0x89, 0x48, 0x86, 0xbe, 0xb0, 0xca); // <GattReadResult> gatt.h:1893
DEFINE_GUID(IID_AsyncSessionHandler,  0xcae01a28, 0xfd33, 0x542e, 0xa5, 0xad, 0x3d, 0x87, 0x8f, 0x73, 0xdb, 0x90); // <GattSession> gatt.h:2025
DEFINE_GUID(IID_AsyncWriteHandler,    0x6fa8c9c3, 0xff7e, 0x5fa1, 0xa2, 0xf3, 0x27, 0x14, 0xcf, 0x04, 0xb8, 0x99); // <GattWriteResult> gatt.h:2157
DEFINE_GUID(IID_AsyncPairingHandler,  0x7ee0247f, 0x5f57, 0x5cb2, 0xb4, 0x0e, 0x18, 0xb5, 0xa2, 0x11, 0xd6, 0xc3); // <DevicePairingResult> enum.h:962
DEFINE_GUID(IID_AsyncUnpairingHandler, 0x9bbe6eb9, 0xdb2d, 0x5160, 0xa2, 0x0c, 0xf0, 0xc2, 0x65, 0xf2, 0x0d, 0x8e); // <DeviceUnpairingResult> enum.h:1105

// Ceilings. The open keeps the Switch 2 driver's 20 s (SDL_BLEGATT_AwaitDevice),
// and a pairing or bond removal gets the same, since Windows runs the whole
// pairing exchange inside one operation. A cancelable wait checks its flag
// every SDL_BLEGATT_CANCEL_SLICE_MS. Services are closed
// SDL_BLEGATT_CLOSE_SETTLE_MS after their handlers go, the pause bleak takes
// because GattDeviceService.Close can hang otherwise (bleak
// backends/winrt/client.py:488-494 and :841-847).
#define SDL_BLEGATT_PAIR_TIMEOUT_MS  20000
#define SDL_BLEGATT_CANCEL_SLICE_MS  50
#define SDL_BLEGATT_CLOSE_SETTLE_MS  100
#define SDL_BLEGATT_MAX_LISTENERS    4

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
typedef PCWSTR(WINAPI *WindowsGetStringRawBuffer_t)(HSTRING string, UINT32 *length);

typedef struct SDL_BLEGATTListenerSlot
{
    SDL_BLEGATT_Listener listener;
    void *userdata;
} SDL_BLEGATTListenerSlot;

static struct
{
    // SDL_BLEGATT_Init calls not yet matched by SDL_BLEGATT_Quit. Both run
    // only from the drivers' Init and Quit, which SDL calls under the joystick
    // lock on the thread that initializes and quits the joystick subsystem, so
    // WIN_RoInitialize and WIN_RoUninitialize pair up on that thread
    // (SDL_ble_gatt.h).
    int refcount;
    CoIncrementMTAUsage_t CoIncrementMTAUsage;
    RoGetActivationFactory_t RoGetActivationFactory;
    RoActivateInstance_t RoActivateInstance;
    RoInitialize_t RoInitialize;
    RoUninitialize_t RoUninitialize;
    WindowsCreateStringReference_t WindowsCreateStringReference;
    WindowsDeleteString_t WindowsDeleteString;
    WindowsGetStringRawBuffer_t WindowsGetStringRawBuffer;

    // Serializes starting and stopping the watcher. Created by the first Init
    // and never destroyed, so a late caller never meets a destroyed mutex.
    SDL_Mutex *watcher_lock;
    BleWatcher *watcher;
    EventRegistrationToken received_token;
    // Under watcher_lock: the watcher's Start succeeded and did not leave it
    // Aborted, what Start returned, and the status read right after a Start
    // that succeeded, -1 when none was read. Start fails with the adapter
    // disabled (microsoft/WindowsAppSDK#3503) and leaves nothing that scans.
    bool watcher_started;
    HRESULT watcher_hr;
    int watcher_status;
    // Under watcher_lock: SDL_BLEGATT_CheckWatcher logged a failed restart,
    // so the retries after it stay quiet until one succeeds
    bool watcher_failing;

    // The Received callback copies the slots under this spin lock and calls
    // the listeners after releasing it. Nothing else runs under it, so a
    // listener can take the joystick lock.
    SDL_SpinLock listener_lock;
    SDL_BLEGATTListenerSlot listeners[SDL_BLEGATT_MAX_LISTENERS];

    // Advertisements handed to the listeners since the watcher started, a
    // count for the log with no device names
    SDL_AtomicInt dispatched;

    // Set once the last holder has quit. A connection thread that SDL_Quit
    // left behind, or a WinRT callback, can still run transport code after
    // that, and it logs nothing, since SDL_Quit goes on to shut the log
    // down.
    SDL_AtomicInt quiet;
} gatt;

// Every transport log line goes through here
#define SDL_BLEGATT_LOG(...) do { if (!SDL_GetAtomicInt(&gatt.quiet)) { SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, __VA_ARGS__); } } while (0)

typedef struct SDL_BLEGATTValueHandler SDL_BLEGATTValueHandler;
typedef struct SDL_BLEGATTStatusHandler SDL_BLEGATTStatusHandler;

// One connection. Only the thread that runs the connection touches it, and no
// callback does, so SDL_BLEGATT_Close can free it. The delegates it registers
// are never freed, since a callback can still be running after they are
// removed.
struct SDL_BLEGATTLink
{
    Uint64 address;
    BleDevice *device;
    GattSession *session;
    SDL_BLEGATTStatusHandler *status_handler;
    EventRegistrationToken status_token;
    bool status_registered;
    GattService3 *services[SDL_BLE_MAX_SERVICES];
    GattChar *characteristics[SDL_BLE_MAX_CHARS];
    SDL_BLEGATTValueHandler *value_handlers[SDL_BLE_MAX_CHARS];
    EventRegistrationToken value_tokens[SDL_BLE_MAX_CHARS];
};

// ---------------------------------------------------------------------------
// Activation helpers.
// ---------------------------------------------------------------------------
static HRESULT SDL_BLEGATT_GetActivationFactory(PCWSTR class_name, REFIID iid, void **out)
{
    HSTRING_HEADER header;
    HSTRING str;
    HRESULT hr = gatt.WindowsCreateStringReference(class_name, (UINT32)SDL_wcslen(class_name), &header, &str);
    if (SUCCEEDED(hr)) {
        hr = gatt.RoGetActivationFactory(str, iid, out);
    }
    return hr;
}

static HRESULT SDL_BLEGATT_ActivateInstance(PCWSTR class_name, REFIID iid, void **out)
{
    HSTRING_HEADER header;
    HSTRING str;
    IInspectable *inspectable = NULL;
    HRESULT hr = gatt.WindowsCreateStringReference(class_name, (UINT32)SDL_wcslen(class_name), &header, &str);
    if (SUCCEEDED(hr)) {
        hr = gatt.RoActivateInstance(str, &inspectable);
        if (SUCCEEDED(hr)) {
            hr = inspectable->lpVtbl->QueryInterface(inspectable, iid, out);
            inspectable->lpVtbl->Release(inspectable);
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Cancellation. A NULL flag is never set.
// ---------------------------------------------------------------------------
static bool SDL_BLEGATT_Canceled(SDL_AtomicInt *cancel)
{
    return cancel && SDL_GetAtomicInt(cancel) != 0;
}

// Cancel a running IAsyncOperation<T> through the IAsyncInfo every WinRT
// operation implements (asyncinfo.h:114). Microsoft documents only that
// Cancel cancels the operation, so the await does not wait for anything
// after it. The awaiter lives on its reference count: a completion that
// still comes finds it alive, and WinRT's release of its own reference
// frees it.
static void SDL_BLEGATT_CancelOperation(void *async_op)
{
    IUnknown *unknown = (IUnknown *)async_op;
    IAsyncInfo *info = NULL;

    if (SUCCEEDED(unknown->lpVtbl->QueryInterface(unknown, &IID_BleAsyncInfo, (void **)&info)) && info) {
        HRESULT hr = IAsyncInfo_Cancel(info);
        SDL_BLEGATT_LOG("BLE await canceled: Cancel hr=0x%08lX", (unsigned long)hr);
        IAsyncInfo_Release(info);
    }
}

// Sleep for ms, in short slices when there is a flag. False when the flag was
// set first.
static bool SDL_BLEGATT_Delay(Uint32 ms, SDL_AtomicInt *cancel)
{
    Uint64 start;

    if (!cancel) {
        SDL_Delay(ms);
        return true;
    }
    start = SDL_GetTicks();
    for (;;) {
        Uint64 elapsed;

        if (SDL_GetAtomicInt(cancel)) {
            return false;
        }
        elapsed = SDL_GetTicks() - start;
        if (elapsed >= ms) {
            return true;
        }
        SDL_Delay((Uint32)SDL_min((Uint64)SDL_BLEGATT_CANCEL_SLICE_MS, (Uint64)ms - elapsed));
    }
}

// ---------------------------------------------------------------------------
// Async-await: one reference-counted completed-handler serves every
// IAsyncOperation<T>. Its QueryInterface is strict and answers the
// completed-handler PIID of the operation it waits on (the IID_Async*Handler
// values above). put_Completed is vtbl slot 6 on every IAsyncOperation<T>, so
// it registers the same way for each, and the caller then calls the typed
// GetResults.
// ---------------------------------------------------------------------------
typedef struct SDL_BLEGATTAwaiter
{
    void *lpVtbl;
    SDL_AtomicInt refcount;
    SDL_Semaphore *sem;
    const GUID *handler_iid; // PIID of the IAsyncOperationCompletedHandler<T> we are
} SDL_BLEGATTAwaiter;

static ULONG STDMETHODCALLTYPE SDL_BLEGATT_Awaiter_AddRef(void *This);

// Strict QI, matching the WGI handler (SDL_windows_gaming_input.c:340-342): accept
// only IUnknown, IAgileObject, and this awaiter's specific completed-handler IID,
// AddRef on success, E_NOINTERFACE for everything else. The previous permissive
// version handed back self (and skipped AddRef) for every IID except IMarshal,
// including IInspectable, which a delegate is not. WinRT rejected that handler at
// put_Completed with CO_E_NOTSUPPORTED (0x80004021), so the open never waited
// (hifihedgehog/SDL#5).
static HRESULT STDMETHODCALLTYPE SDL_BLEGATT_Awaiter_QueryInterface(void *This, REFIID riid, void **ppv)
{
    SDL_BLEGATTAwaiter *self = (SDL_BLEGATTAwaiter *)This;
    if (!ppv) {
        return E_INVALIDARG;
    }
    if (WIN_IsEqualIID(riid, &IID_IUnknown) ||
        WIN_IsEqualIID(riid, &IID_IAgileObject) ||
        (self->handler_iid && WIN_IsEqualIID(riid, self->handler_iid))) {
        *ppv = This;
        SDL_BLEGATT_Awaiter_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE SDL_BLEGATT_Awaiter_AddRef(void *This)
{
    SDL_BLEGATTAwaiter *self = (SDL_BLEGATTAwaiter *)This;
    return (ULONG)(SDL_AddAtomicInt(&self->refcount, 1) + 1);
}
static ULONG STDMETHODCALLTYPE SDL_BLEGATT_Awaiter_Release(void *This)
{
    SDL_BLEGATTAwaiter *self = (SDL_BLEGATTAwaiter *)This;
    int rc = SDL_AddAtomicInt(&self->refcount, -1) - 1;
    if (rc == 0) {
        SDL_DestroySemaphore(self->sem);
        SDL_free(self);
    }
    return (ULONG)rc;
}
static HRESULT STDMETHODCALLTYPE SDL_BLEGATT_Awaiter_Invoke(void *This, void *op, int status)
{
    SDL_BLEGATTAwaiter *self = (SDL_BLEGATTAwaiter *)This;
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
} g_awaiter_vtbl = { (void *)SDL_BLEGATT_Awaiter_QueryInterface, (void *)SDL_BLEGATT_Awaiter_AddRef, (void *)SDL_BLEGATT_Awaiter_Release, (void *)SDL_BLEGATT_Awaiter_Invoke };

// Wait for the semaphore in slices, checking the flag before each. Once the
// flag is set, or the ceiling passes, the operation is canceled and the wait
// fails, so a caller that tries again never leaves the last try running, as
// Qt cancels at its await's deadline (qtconnectivity
// src/bluetooth/qlowenergycontroller_winrt.cpp:130-131). A negative timeout
// waits without a ceiling.
static bool SDL_BLEGATT_WaitCancelable(SDL_Semaphore *sem, Sint32 timeout_ms, void *async_op, SDL_AtomicInt *cancel)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        Sint32 slice = SDL_BLEGATT_CANCEL_SLICE_MS;

        if (SDL_GetAtomicInt(cancel)) {
            SDL_BLEGATT_CancelOperation(async_op);
            return false;
        }
        if (timeout_ms >= 0) {
            const Uint64 elapsed = SDL_GetTicks() - start;
            if (elapsed >= (Uint64)timeout_ms) {
                SDL_BLEGATT_CancelOperation(async_op);
                return false;
            }
            if ((Uint64)timeout_ms - elapsed < (Uint64)slice) {
                slice = (Sint32)((Uint64)timeout_ms - elapsed);
            }
        }
        if (SDL_WaitSemaphoreTimeout(sem, slice)) {
            return true;
        }
    }
}

// Block (up to timeout_ms) until the IAsyncOperation completes. The handler is
// heap-allocated and refcounted: WinRT holds a reference until it finishes, so a
// timeout here cannot free the handler out from under a later completion. Returns
// false on arm failure or timeout. With a cancel flag the wait runs in short
// slices, and once the flag is set or the ceiling passes the operation is
// canceled through IAsyncInfo::Cancel and the await fails. A NULL flag waits in
// one piece and leaves the operation running at its ceiling, as the Switch 2
// driver always has.
static bool SDL_BLEGATT_AwaitTimeout(void *async_op, Sint32 timeout_ms, const GUID *handler_iid, SDL_AtomicInt *cancel)
{
    typedef HRESULT(STDMETHODCALLTYPE * put_Completed_t)(void *This, void *handler);
    void ***vtbl;
    put_Completed_t put_Completed;
    SDL_BLEGATTAwaiter *awaiter;
    HRESULT hr;
    bool completed;

    if (!async_op) {
        return false;
    }
    awaiter = (SDL_BLEGATTAwaiter *)SDL_calloc(1, sizeof(*awaiter));
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
        if (cancel) {
            completed = SDL_BLEGATT_WaitCancelable(awaiter->sem, timeout_ms, async_op, cancel);
        } else {
            completed = SDL_WaitSemaphoreTimeout(awaiter->sem, timeout_ms);
        }
    } else {
        completed = false;
    }
    // Diagnostic (hifihedgehog/SDL#5): pin candidate (b). A put_Completed failure,
    // or a completed=0 that returns far sooner than timeout_ms, means the await is
    // not actually waiting on the async op.
    SDL_BLEGATT_LOG("BLE await: put_Completed hr=0x%08lX wait(%dms) completed=%d",
                 (unsigned long)hr, (int)timeout_ms, (int)completed);
    SDL_BLEGATT_Awaiter_Release(awaiter); // drop our reference. WinRT frees it when it is done
    return completed;
}

// Default await for post-link per-op reads and writes (link already up, so a
// short ceiling is fine and surfaces a wedged op quickly).
static bool SDL_BLEGATT_Await(void *async_op, const GUID *handler_iid, SDL_AtomicInt *cancel)
{
    return SDL_BLEGATT_AwaitTimeout(async_op, 3000, handler_iid, cancel);
}

// ---------------------------------------------------------------------------
// IBuffer helpers.
// ---------------------------------------------------------------------------
static Buffer *SDL_BLEGATT_BufferFromBytes(const Uint8 *bytes, UINT32 length)
{
    DataWriter *writer = NULL;
    Buffer *buffer = NULL;

    if (FAILED(SDL_BLEGATT_ActivateInstance(RuntimeClass_Windows_Storage_Streams_DataWriter, &IID_DataWriter, (void **)&writer))) {
        return NULL;
    }
    if (SUCCEEDED(__x_ABI_CWindows_CStorage_CStreams_CIDataWriter_WriteBytes(writer, length, (BYTE *)bytes))) {
        __x_ABI_CWindows_CStorage_CStreams_CIDataWriter_DetachBuffer(writer, &buffer);
    }
    __x_ABI_CWindows_CStorage_CStreams_CIDataWriter_Release(writer);
    return buffer;
}

// Copy an IBuffer's bytes into dst (up to dst_len). Returns the number copied.
static int SDL_BLEGATT_BufferToBytes(Buffer *buffer, Uint8 *dst, int dst_len)
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

// Reach an IBuffer's bytes in place, whole. The bytes stay valid while the
// caller holds its reference to the buffer. An empty buffer yields a pointer
// to a zero byte, never NULL. False when the bytes cannot be reached.
static bool SDL_BLEGATT_BufferBytes(Buffer *buffer, const Uint8 **data, UINT32 *length)
{
    static const Uint8 empty = 0;
    IBufferByteAccess *access = NULL;
    byte *raw = NULL;
    UINT32 size = 0;
    bool result = false;

    if (FAILED(__x_ABI_CWindows_CStorage_CStreams_CIBuffer_get_Length(buffer, &size))) {
        return false;
    }
    if (FAILED(__x_ABI_CWindows_CStorage_CStreams_CIBuffer_QueryInterface(buffer, &IID_IBufferByteAccess, (void **)&access)) || !access) {
        return false;
    }
    if (SUCCEEDED(access->lpVtbl->Buffer(access, &raw)) && (raw || size == 0)) {
        *data = raw ? (const Uint8 *)raw : &empty;
        *length = size;
        result = true;
    }
    access->lpVtbl->Release(access);
    return result;
}

// An SDL_BLEUUID holds the UUID in the order it is written (SDL_ble_proto.h:103-108),
// which is GUID.Data1, Data2 and Data3 most significant byte first, then Data4.
static void SDL_BLEGATT_UUIDFromGUID(const GUID *guid, SDL_BLEUUID *uuid)
{
    uuid->bytes[0] = (Uint8)(guid->Data1 >> 24);
    uuid->bytes[1] = (Uint8)(guid->Data1 >> 16);
    uuid->bytes[2] = (Uint8)(guid->Data1 >> 8);
    uuid->bytes[3] = (Uint8)guid->Data1;
    uuid->bytes[4] = (Uint8)(guid->Data2 >> 8);
    uuid->bytes[5] = (Uint8)guid->Data2;
    uuid->bytes[6] = (Uint8)(guid->Data3 >> 8);
    uuid->bytes[7] = (Uint8)guid->Data3;
    SDL_memcpy(&uuid->bytes[8], guid->Data4, 8);
}

static void SDL_BLEGATT_GUIDFromUUID(const SDL_BLEUUID *uuid, GUID *guid)
{
    guid->Data1 = ((unsigned long)uuid->bytes[0] << 24) | ((unsigned long)uuid->bytes[1] << 16) |
                  ((unsigned long)uuid->bytes[2] << 8) | (unsigned long)uuid->bytes[3];
    guid->Data2 = (unsigned short)((uuid->bytes[4] << 8) | uuid->bytes[5]);
    guid->Data3 = (unsigned short)((uuid->bytes[6] << 8) | uuid->bytes[7]);
    SDL_memcpy(guid->Data4, &uuid->bytes[8], 8);
}

// Write bytes to a characteristic. The caller's prefer_response hint asks for a
// reliable (acknowledged) write for the command channel, but the Switch 2 command
// characteristic only advertises write-without-response on real hardware
// (switch2-bt docs/HARDWARE-TEST.md: "command write ... props=write-without-
// response". joycon2cpp testapp.cpp:452-455 and controller.py via Bleak both write
// commands without response). Honoring a WriteWithResponse against a char that
// lacks the Write property fails at the ATT layer, so choose the option from the
// characteristic's actual properties: WriteWithResponse only when Write (0x8) is
// advertised, otherwise WriteWithoutResponse. The reply still arrives over the
// response notification, independent of the write acknowledgment.
// With check_status the write also has to return GattCommunicationStatus_Success,
// as the generic driver's session expects. Without it a completed write counts,
// as the Switch 2 driver has always counted it.
static bool SDL_BLEGATT_WriteValue(GattChar *characteristic, const Uint8 *bytes, int length, bool prefer_response,
                                   SDL_AtomicInt *cancel, bool check_status)
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
    buffer = SDL_BLEGATT_BufferFromBytes(bytes, (UINT32)length);
    if (!buffer) {
        return false;
    }
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_WriteValueWithOptionAsync(characteristic, buffer, option, (void *)&op)) && op) {
        result = SDL_BLEGATT_Await(op, &IID_AsyncStatusHandler, cancel);
        if (result && check_status) {
            GattCommStatus status = GattCommunicationStatus_Unreachable;
            if (FAILED(__FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCommunicationStatus_GetResults((StatusOp *)op, &status)) ||
                status != GattCommunicationStatus_Success) {
                result = false;
            }
        }
        ((Buffer *)op)->lpVtbl->Release((Buffer *)op);
    }
    __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release(buffer);
    return result;
}

bool SDL_BLEGATT_WriteCharacteristic(GattChar *characteristic, const Uint8 *bytes, int length, bool prefer_response)
{
    return SDL_BLEGATT_WriteValue(characteristic, bytes, length, prefer_response, NULL, false);
}

// Subscribe a characteristic to notifications (register handler then write CCCD).
bool SDL_BLEGATT_EnableNotifications(GattChar *characteristic)
{
    void *op = NULL;
    bool result = false;

    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_WriteClientCharacteristicConfigurationDescriptorAsync(characteristic, GattClientCharacteristicConfigurationDescriptorValue_Notify, (void *)&op)) && op) {
        result = SDL_BLEGATT_Await(op, &IID_AsyncStatusHandler, NULL);
        ((Buffer *)op)->lpVtbl->Release((Buffer *)op);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Characteristic ValueChanged: hand the whole value, never cut short, to the
// callback with its context and characteristic index. Runs on a WinRT
// thread-pool (MTA) thread.
// ---------------------------------------------------------------------------
// vtbl + trailing context so Invoke can find its callback.
struct SDL_BLEGATTValueHandler
{
    void *vtbl;
    SDL_BLEGATT_ValueCallback callback;
    void *userdata;
    int characteristic;
};

static HRESULT STDMETHODCALLTYPE SDL_BLEGATT_ValueHandler_QueryInterface(void *This, REFIID riid, void **ppv)
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
static ULONG STDMETHODCALLTYPE SDL_BLEGATT_ValueHandler_AddRef(void *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE SDL_BLEGATT_ValueHandler_Release(void *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE SDL_BLEGATT_ValueHandler_Invoke(void *This, void *sender, GattValueArgs *args)
{
    SDL_BLEGATTValueHandler *handler = (SDL_BLEGATTValueHandler *)This;
    Buffer *buffer = NULL;
    (void)sender;

    if (!args || !handler) {
        return S_OK;
    }
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattValueChangedEventArgs_get_CharacteristicValue(args, &buffer)) && buffer) {
        const Uint8 *data = NULL;
        UINT32 length = 0;
        if (SDL_BLEGATT_BufferBytes(buffer, &data, &length)) {
            handler->callback(handler->userdata, handler->characteristic, data, (size_t)length);
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
} g_value_vtbl = { (void *)SDL_BLEGATT_ValueHandler_QueryInterface, (void *)SDL_BLEGATT_ValueHandler_AddRef, (void *)SDL_BLEGATT_ValueHandler_Release, (void *)SDL_BLEGATT_ValueHandler_Invoke };

static SDL_BLEGATTValueHandler *SDL_BLEGATT_NewValueHandler(SDL_BLEGATT_ValueCallback callback, void *userdata, int index)
{
    SDL_BLEGATTValueHandler *handler;

    if (!callback) {
        return NULL;
    }
    handler = (SDL_BLEGATTValueHandler *)SDL_calloc(1, sizeof(*handler));
    if (handler) {
        handler->vtbl = (void *)&g_value_vtbl;
        handler->callback = callback;
        handler->userdata = userdata;
        handler->characteristic = index;
    }
    return handler;
}

void *SDL_BLEGATT_AddValueHandler(GattChar *characteristic, SDL_BLEGATT_ValueCallback callback, void *userdata, int index,
                                  EventRegistrationToken *token)
{
    SDL_BLEGATTValueHandler *handler = SDL_BLEGATT_NewValueHandler(callback, userdata, index);
    if (handler) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_add_ValueChanged(characteristic, (void *)handler, token);
    }
    return handler;
}

// ---------------------------------------------------------------------------
// BluetoothLEDevice.ConnectionStatusChanged: link-loss detection (SDL#9
// follow-up). Fires on a WinRT thread-pool (MTA) thread when the link drops:
// pad powered off (the SendEffect shutdown), battery died, or out of range.
// Per the SDL#5 callback-pool rule, no work happens here: set the flag and
// return. The driver drains it on its own thread (the Switch 2 driver in
// BLE_JoystickDetect) and runs the actual teardown there.
// A Disconnected status counts as a loss only once the link has been seen up
// (armed). SDL_BLEGATT_Open registers before the connection exists, since
// Windows connects a device on its first GATT operation (qtconnectivity
// src/bluetooth/qlowenergycontroller_winrt.cpp:479-487 and :527-528), so
// there Disconnected first means not yet connected. The Switch 2 driver
// registers after its setup, on a link already up, so its delegate starts
// armed.
// ---------------------------------------------------------------------------
struct SDL_BLEGATTStatusHandler
{
    void *vtbl;
    SDL_AtomicInt *lost;
    SDL_AtomicInt armed;
};

static HRESULT STDMETHODCALLTYPE SDL_BLEGATT_StatusHandler_QueryInterface(void *This, REFIID riid, void **ppv)
{
    if (!ppv) {
        return E_INVALIDARG;
    }
    if (WIN_IsEqualIID(riid, &IID_IUnknown) || WIN_IsEqualIID(riid, &IID_IAgileObject) || WIN_IsEqualIID(riid, &IID_BleStatusHandler)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE SDL_BLEGATT_StatusHandler_AddRef(void *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE SDL_BLEGATT_StatusHandler_Release(void *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE SDL_BLEGATT_StatusHandler_Invoke(void *This, BleDevice *sender, void *args)
{
    SDL_BLEGATTStatusHandler *handler = (SDL_BLEGATTStatusHandler *)This;
    enum __x_ABI_CWindows_CDevices_CBluetooth_CBluetoothConnectionStatus status = BluetoothConnectionStatus_Connected;
    (void)args;
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_get_ConnectionStatus(sender, &status))) {
        if (status == BluetoothConnectionStatus_Disconnected) {
            if (SDL_GetAtomicInt(&handler->armed)) {
                SDL_SetAtomicInt(handler->lost, 1);
            }
        } else {
            SDL_SetAtomicInt(&handler->armed, 1);
        }
    }
    return S_OK;
}
static const struct
{
    void *QueryInterface;
    void *AddRef;
    void *Release;
    void *Invoke;
} g_status_vtbl = { (void *)SDL_BLEGATT_StatusHandler_QueryInterface, (void *)SDL_BLEGATT_StatusHandler_AddRef, (void *)SDL_BLEGATT_StatusHandler_Release, (void *)SDL_BLEGATT_StatusHandler_Invoke };

static SDL_BLEGATTStatusHandler *SDL_BLEGATT_NewStatusHandler(SDL_AtomicInt *lost, bool armed)
{
    SDL_BLEGATTStatusHandler *handler;

    if (!lost) {
        return NULL;
    }
    handler = (SDL_BLEGATTStatusHandler *)SDL_calloc(1, sizeof(*handler));
    if (handler) {
        handler->vtbl = (void *)&g_status_vtbl;
        handler->lost = lost;
        SDL_SetAtomicInt(&handler->armed, armed ? 1 : 0);
    }
    return handler;
}

void *SDL_BLEGATT_AddStatusHandler(BleDevice *device, SDL_AtomicInt *lost, EventRegistrationToken *token)
{
    SDL_BLEGATTStatusHandler *status_handler = SDL_BLEGATT_NewStatusHandler(lost, true);
    if (status_handler) {
        enum __x_ABI_CWindows_CDevices_CBluetooth_CBluetoothConnectionStatus status = BluetoothConnectionStatus_Connected;
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_add_ConnectionStatusChanged(device, (void *)status_handler, token);
        // The event fires only on transitions after registration, never as a
        // state replay. A pad that died during the caller's multi-second setup
        // would be appended already-dead with no event ever coming, so poll
        // the status once after subscribing to close that window.
        if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_get_ConnectionStatus(device, &status)) &&
            status == BluetoothConnectionStatus_Disconnected) {
            SDL_SetAtomicInt(lost, 1);
        }
    }
    return status_handler;
}

// ---------------------------------------------------------------------------
// DeviceInformationCustomPairing.PairingRequested: with ConfirmOnly the handler
// calls Accept for the pairing to complete (MicrosoftDocs/winrt-api
// windows.devices.enumeration/devicepairingkinds.md:22-23). Stateless, so one
// static delegate serves every pairing.
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE SDL_BLEGATT_PairingHandler_QueryInterface(void *This, REFIID riid, void **ppv)
{
    if (!ppv) {
        return E_INVALIDARG;
    }
    if (WIN_IsEqualIID(riid, &IID_IUnknown) || WIN_IsEqualIID(riid, &IID_IAgileObject) || WIN_IsEqualIID(riid, &IID_PairingRequestedHandler)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE SDL_BLEGATT_PairingHandler_AddRef(void *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE SDL_BLEGATT_PairingHandler_Release(void *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE SDL_BLEGATT_PairingHandler_Invoke(void *This, void *sender, PairingRequestedArgs *args)
{
    enum __x_ABI_CWindows_CDevices_CEnumeration_CDevicePairingKinds kind = DevicePairingKinds_None;
    (void)This;
    (void)sender;

    if (args && SUCCEEDED(__x_ABI_CWindows_CDevices_CEnumeration_CIDevicePairingRequestedEventArgs_get_PairingKind(args, &kind)) &&
        kind == DevicePairingKinds_ConfirmOnly) {
        HRESULT hr = __x_ABI_CWindows_CDevices_CEnumeration_CIDevicePairingRequestedEventArgs_Accept(args);
        SDL_BLEGATT_LOG("BLE GATT pairing requested: Accept hr=0x%08lX", (unsigned long)hr);
    }
    return S_OK;
}
static const struct
{
    void *QueryInterface;
    void *AddRef;
    void *Release;
    void *Invoke;
} g_pairing_vtbl = { (void *)SDL_BLEGATT_PairingHandler_QueryInterface, (void *)SDL_BLEGATT_PairingHandler_AddRef, (void *)SDL_BLEGATT_PairingHandler_Release, (void *)SDL_BLEGATT_PairingHandler_Invoke };
static struct { void *vtbl; } g_pairing_handler = { (void *)&g_pairing_vtbl };

// ---------------------------------------------------------------------------
// Advertisement Received: parse the event and hand it to every listener. Runs
// on a WinRT thread-pool (MTA) thread.
// ---------------------------------------------------------------------------

// The listeners as they stand, copied under the spin lock. False when there
// are none.
static bool SDL_BLEGATT_GetListeners(SDL_BLEGATTListenerSlot *slots)
{
    bool any = false;
    int i;

    SDL_LockSpinlock(&gatt.listener_lock);
    SDL_memcpy(slots, gatt.listeners, sizeof(gatt.listeners));
    SDL_UnlockSpinlock(&gatt.listener_lock);
    for (i = 0; i < SDL_BLEGATT_MAX_LISTENERS; ++i) {
        if (slots[i].listener) {
            any = true;
        }
    }
    return any;
}

// The local name as UTF-8, cut at a character boundary to fit
static void SDL_BLEGATT_ParseName(BleAdvertisement *advertisement, SDL_BLEAdvertisement *parsed)
{
    HSTRING name = NULL;

    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisement_get_LocalName(advertisement, &name)) && name) {
        UINT32 length = 0;
        PCWSTR raw = gatt.WindowsGetStringRawBuffer(name, &length);
        if (raw && length > 0) {
            char *utf8 = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)raw, (size_t)length * sizeof(WCHAR));
            if (utf8) {
                SDL_utf8strlcpy(parsed->name, utf8, sizeof(parsed->name));
                parsed->has_name = (parsed->name[0] != '\0');
                SDL_free(utf8);
            }
        }
        gatt.WindowsDeleteString(name);
    }
}

static void SDL_BLEGATT_ParseServices(BleAdvertisement *advertisement, SDL_BLEAdvertisement *parsed)
{
    __FIVector_1_GUID *uuids = NULL;

    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisement_get_ServiceUuids(advertisement, &uuids)) && uuids) {
        unsigned i, count = 0;
        __FIVector_1_GUID_get_Size(uuids, &count);
        for (i = 0; i < count && parsed->nservices < SDL_BLE_AD_SERVICES; ++i) {
            GUID uuid;
            if (SUCCEEDED(__FIVector_1_GUID_GetAt(uuids, i, &uuid))) {
                SDL_BLEGATT_UUIDFromGUID(&uuid, &parsed->services[parsed->nservices++]);
            }
        }
        __FIVector_1_GUID_Release(uuids);
    }
}

// A scan response or an advertisement. An extended advertisement need not be
// either kind of legacy packet, and its type says nothing more
// (MicrosoftDocs/winrt-api
// windows.devices.bluetooth.advertisement/bluetoothleadvertisementtype.md:42-45),
// so the IsScanResponse property of Windows 10 2004 and later decides when
// Windows has it (adv.h:6411). Before that only the ScanResponse type counts.
static SDL_BLEAdvertisementKind SDL_BLEGATT_AdvertisementKind(BleRecvArgs *args)
{
    enum __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CBluetoothLEAdvertisementType type = BluetoothLEAdvertisementType_ConnectableUndirected;
    BleRecvArgs2 *args2 = NULL;
    bool scan_response = false;

    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementReceivedEventArgs_get_AdvertisementType(args, &type))) {
        scan_response = (type == BluetoothLEAdvertisementType_ScanResponse);
    }
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementReceivedEventArgs_QueryInterface(args, &IID_BleRecvArgs2, (void **)&args2)) && args2) {
        boolean value = FALSE;
        if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementReceivedEventArgs2_get_IsScanResponse(args2, &value))) {
            scan_response = (value != FALSE);
        }
        __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementReceivedEventArgs2_Release(args2);
    }
    return scan_response ? SDL_BLE_AD_SCAN_RESPONSE : SDL_BLE_AD_ADVERTISEMENT;
}

static HRESULT STDMETHODCALLTYPE SDL_BLEGATT_Received_QueryInterface(void *This, REFIID riid, void **ppv)
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
static ULONG STDMETHODCALLTYPE SDL_BLEGATT_Received_AddRef(void *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE SDL_BLEGATT_Received_Release(void *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE SDL_BLEGATT_Received_Invoke(void *This, void *sender, BleRecvArgs *args)
{
    UINT64 address = 0;
    BleAdvertisement *advertisement = NULL;
    __FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData *mfg_list = NULL;
    SDL_BLEGATTListenerSlot slots[SDL_BLEGATT_MAX_LISTENERS];
    SDL_BLEAdvertisement parsed;
    int l;
    (void)This;
    (void)sender;

    if (!args || !SDL_BLEGATT_GetListeners(slots)) {
        return S_OK;
    }
    __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementReceivedEventArgs_get_BluetoothAddress(args, &address);
    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementReceivedEventArgs_get_Advertisement(args, &advertisement)) || !advertisement) {
        return S_OK;
    }
    SDL_zero(parsed);
    parsed.address = address;
    parsed.kind = (Uint8)SDL_BLEGATT_AdvertisementKind(args);
    SDL_BLEGATT_ParseName(advertisement, &parsed);
    SDL_BLEGATT_ParseServices(advertisement, &parsed);

    // Up to SDL_BLE_AD_MANUFACTURER entries in their order, each with up to
    // SDL_BLE_AD_MANUFACTURER_DATA bytes after the company ID
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisement_get_ManufacturerData(advertisement, &mfg_list)) && mfg_list) {
        unsigned i, count = 0;
        __FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData_get_Size(mfg_list, &count);
        for (i = 0; i < count && parsed.nmanufacturer < SDL_BLE_AD_MANUFACTURER; ++i) {
            BleMfgData *mfg = NULL;
            if (SUCCEEDED(__FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData_GetAt(mfg_list, i, &mfg)) && mfg) {
                SDL_BLEManufacturerData *entry = &parsed.manufacturer[parsed.nmanufacturer++];
                UINT16 company = 0;
                Buffer *payload = NULL;
                __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEManufacturerData_get_CompanyId(mfg, &company);
                entry->company = company;
                if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEManufacturerData_get_Data(mfg, &payload)) && payload) {
                    // Company id already stripped. The full length clamped to the copy.
                    entry->length = (Uint8)SDL_BLEGATT_BufferToBytes(payload, entry->data, (int)sizeof(entry->data));
                    __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release(payload);
                }
                __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEManufacturerData_Release(mfg);
            }
        }
        __FIVector_1_Windows__CDevices__CBluetooth__CAdvertisement__CBluetoothLEManufacturerData_Release(mfg_list);
    }
    __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisement_Release(advertisement);

    // Outside every lock. A listener removed after the copy can still be
    // called once, and checks its own driver's state (SDL_ble_gatt.h).
    // The listeners run one after another on this thread. The Switch 2
    // listener takes SDL_LockJoysticks to reserve the address of a Switch 2
    // advertisement (BLE_TryReserveConnect in SDL_ble_switch2joystick.c), so
    // while another thread holds the joystick lock, the listeners after it
    // wait as well. This known limit is left as it is: the wait only delays
    // the next listener, and the generic driver's listener, which takes only
    // its own lock, then returns at once.
    SDL_AddAtomicInt(&gatt.dispatched, 1);
    for (l = 0; l < SDL_BLEGATT_MAX_LISTENERS; ++l) {
        if (slots[l].listener) {
            slots[l].listener(&parsed, slots[l].userdata);
        }
    }
    return S_OK;
}
static const struct
{
    void *QueryInterface;
    void *AddRef;
    void *Release;
    void *Invoke;
} g_received_vtbl = { (void *)SDL_BLEGATT_Received_QueryInterface, (void *)SDL_BLEGATT_Received_AddRef, (void *)SDL_BLEGATT_Received_Release, (void *)SDL_BLEGATT_Received_Invoke };
static struct { void *vtbl; } g_received_handler = { (void *)&g_received_vtbl };

// Under gatt.watcher_lock. True when the watcher object was created, as the
// Switch 2 driver counted its watcher as scanning. gatt.watcher_started says
// whether it runs: Start moves the watcher to Started, or straight to
// Aborted when the request fails at once (MicrosoftDocs/winrt-api
// windows.devices.bluetooth.advertisement/bluetoothleadvertisementwatcher_start_1587696324.md:26),
// so a watcher Aborted after a Start that succeeded counts as not started,
// and SDL_BLEGATT_CheckWatcher puts a new one in its place.
static bool SDL_BLEGATT_StartWatcher(void)
{
    BleWatcher2 *watcher2 = NULL;
    BleWatcherStatus status = BluetoothLEAdvertisementWatcherStatus_Started;
    HRESULT hr;

    // Start the advertisement watcher.
    gatt.watcher_started = false;
    gatt.watcher_status = -1;
    hr = SDL_BLEGATT_ActivateInstance(RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisementWatcher, &IID_BleWatcher, (void **)&gatt.watcher);
    if (FAILED(hr) || !gatt.watcher) {
        gatt.watcher = NULL;
        gatt.watcher_hr = FAILED(hr) ? hr : E_POINTER;
        return false;
    }
    __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_put_ScanningMode(gatt.watcher, BluetoothLEScanningMode_Active);
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_QueryInterface(gatt.watcher, &IID_BleWatcher2, (void **)&watcher2))) {
        __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher2_put_AllowExtendedAdvertisements(watcher2, TRUE);
        __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher2_Release(watcher2);
    }
    SDL_SetAtomicInt(&gatt.dispatched, 0);
    __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_add_Received(gatt.watcher, (void *)&g_received_handler, &gatt.received_token);
    gatt.watcher_hr = __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_Start(gatt.watcher);
    if (SUCCEEDED(gatt.watcher_hr)) {
        gatt.watcher_started = true;
        if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_get_Status(gatt.watcher, &status))) {
            gatt.watcher_status = (int)status;
            if (status == BluetoothLEAdvertisementWatcherStatus_Aborted) {
                gatt.watcher_started = false;
            }
        }
    }
    return true;
}

// Under gatt.watcher_lock
static void SDL_BLEGATT_StopWatcher(void)
{
    if (gatt.watcher) {
        __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_Stop(gatt.watcher);
        if (gatt.received_token.value) {
            __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_remove_Received(gatt.watcher, gatt.received_token);
        }
        __x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_Release(gatt.watcher);
        gatt.watcher = NULL;
        gatt.received_token.value = 0;
    }
    gatt.watcher_started = false;
}

// ---------------------------------------------------------------------------
// Runtime.
// ---------------------------------------------------------------------------
static bool SDL_BLEGATT_ResolveRuntime(void)
{
#define RESOLVE(x) gatt.x = (x##_t)WIN_LoadComBaseFunction(#x); if (!gatt.x) return SDL_SetError("GetProcAddress failed for " #x)
    RESOLVE(CoIncrementMTAUsage);
    RESOLVE(RoGetActivationFactory);
    RESOLVE(RoActivateInstance);
    RESOLVE(RoInitialize);
    RESOLVE(RoUninitialize);
    RESOLVE(WindowsCreateStringReference);
    RESOLVE(WindowsDeleteString);
    RESOLVE(WindowsGetStringRawBuffer);
#undef RESOLVE
    return true;
}

bool SDL_BLEGATT_Init(void)
{
    if (gatt.refcount > 0) {
        ++gatt.refcount;
        return true;
    }

    // The lock comes first, so a failure leaves nothing to undo. Without it
    // the watcher could start and stop on two threads at once.
    if (!gatt.watcher_lock) {
        gatt.watcher_lock = SDL_CreateMutex();
        if (!gatt.watcher_lock) {
            return false;
        }
    }
    if (FAILED(WIN_RoInitialize())) {
        return SDL_SetError("RoInitialize() failed");
    }
    if (!SDL_BLEGATT_ResolveRuntime()) {
        WIN_RoUninitialize();
        return false;
    }

    {
        static HANDLE cookie = NULL;
        if (!cookie) {
            gatt.CoIncrementMTAUsage(&cookie); // pin MTA for BLE callback threads
        }
    }

    SDL_SetAtomicInt(&gatt.quiet, 0);
    gatt.refcount = 1;
    return true;
}

void SDL_BLEGATT_Quit(void)
{
    if (gatt.refcount <= 0) {
        return;
    }
    if (--gatt.refcount > 0) {
        return;
    }

    // The last holder is gone. Every driver removes its listener before it
    // lets go, so this only stops a watcher left behind.
    SDL_LockMutex(gatt.watcher_lock);
    SDL_LockSpinlock(&gatt.listener_lock);
    SDL_zeroa(gatt.listeners);
    SDL_UnlockSpinlock(&gatt.listener_lock);
    SDL_BLEGATT_StopWatcher();
    SDL_UnlockMutex(gatt.watcher_lock);

    SDL_SetAtomicInt(&gatt.quiet, 1);
    WIN_RoUninitialize();

    // The combase entry points stay resolved. combase stays loaded
    // (WIN_LoadComBaseFunction never frees it), and a connect thread or a
    // WinRT callback that is still running keeps calling them.
}

// The Switch 2 driver's connect thread joins the MTA this way (see its
// BLE_ConnectThread for why MULTITHREADED and not WIN_RoInitialize).
bool SDL_BLEGATT_InitThread(void)
{
    if (gatt.RoInitialize) {
        return SUCCEEDED(gatt.RoInitialize(RO_INIT_MULTITHREADED));
    }
    return false;
}

void SDL_BLEGATT_QuitThread(void)
{
    if (gatt.RoUninitialize) {
        gatt.RoUninitialize();
    }
}

bool SDL_BLEGATT_AddListener(SDL_BLEGATT_Listener listener, void *userdata)
{
    bool result = false;
    bool started = false;
    bool running = false;
    HRESULT hr = S_OK;
    int status = -1;
    int slot = -1;
    int i;

    if (!listener) {
        return SDL_InvalidParamError("listener");
    }
    if (gatt.refcount <= 0) {
        return SDL_SetError("The BLE GATT transport is not initialized");
    }
    SDL_LockMutex(gatt.watcher_lock);
    SDL_LockSpinlock(&gatt.listener_lock);
    for (i = 0; i < SDL_BLEGATT_MAX_LISTENERS; ++i) {
        if (!gatt.listeners[i].listener) {
            slot = i;
            gatt.listeners[i].listener = listener;
            gatt.listeners[i].userdata = userdata;
            break;
        }
    }
    SDL_UnlockSpinlock(&gatt.listener_lock);
    if (slot < 0) {
        SDL_SetError("Too many BLE advertisement listeners");
    } else if (gatt.watcher) {
        result = true;
    } else if (SDL_BLEGATT_StartWatcher()) {
        // A watcher whose Start failed still counts, and
        // SDL_BLEGATT_CheckWatcher starts a new one later
        result = true;
        started = true;
        running = gatt.watcher_started;
        hr = gatt.watcher_hr;
        status = gatt.watcher_status;
        gatt.watcher_failing = !running;
    } else {
        // No watcher runs, so no callback can have copied the slot
        SDL_LockSpinlock(&gatt.listener_lock);
        gatt.listeners[slot].listener = NULL;
        gatt.listeners[slot].userdata = NULL;
        SDL_UnlockSpinlock(&gatt.listener_lock);
        SDL_SetError("Could not create the BLE advertisement watcher");
    }
    SDL_UnlockMutex(gatt.watcher_lock);
    // Logged outside the lock, so an app's log callback runs under no lock
    // of the transport
    if (started && running) {
        SDL_BLEGATT_LOG("BLE: watching advertisements");
    } else if (started) {
        SDL_BLEGATT_LOG("BLE: the advertisement watcher did not start, hr=0x%08lX, status %d",
                     (unsigned long)hr, status);
    }
    return result;
}

void SDL_BLEGATT_RemoveListener(SDL_BLEGATT_Listener listener, void *userdata)
{
    bool removed = false;
    bool stopped = false;
    int remaining = 0;
    int i;

    if (gatt.refcount <= 0) {
        return;
    }
    SDL_LockMutex(gatt.watcher_lock);
    SDL_LockSpinlock(&gatt.listener_lock);
    for (i = 0; i < SDL_BLEGATT_MAX_LISTENERS; ++i) {
        if (!removed && gatt.listeners[i].listener == listener && gatt.listeners[i].userdata == userdata) {
            gatt.listeners[i].listener = NULL;
            gatt.listeners[i].userdata = NULL;
            removed = true;
        } else if (gatt.listeners[i].listener) {
            ++remaining;
        }
    }
    SDL_UnlockSpinlock(&gatt.listener_lock);
    if (removed && remaining == 0 && gatt.watcher) {
        SDL_BLEGATT_StopWatcher();
        stopped = true;
    }
    SDL_UnlockMutex(gatt.watcher_lock);
    if (stopped) {
        SDL_BLEGATT_LOG("BLE: stopped watching after %d advertisements",
                     SDL_GetAtomicInt(&gatt.dispatched));
    }
}

// A watcher that aborts stays Aborted or Stopped, and apart from its status
// only its Stopped event reports that (MicrosoftDocs/winrt-api
// windows.devices.bluetooth.advertisement/bluetoothleadvertisementwatcherstatus.md:17-30
// and bluetoothleadvertisementwatcher_stopped.md:13-14). Polling the status
// needs no delegate. Nothing here stops a watcher that still runs.
bool SDL_BLEGATT_CheckWatcher(void)
{
    BleWatcherStatus status = BluetoothLEAdvertisementWatcherStatus_Created;
    bool listening = false;
    bool had_status = false;
    bool restarted = false;
    bool failed = false;
    HRESULT hr = S_OK;
    int new_status = -1;
    int i;

    if (gatt.refcount <= 0) {
        return false;
    }
    SDL_LockMutex(gatt.watcher_lock);
    SDL_LockSpinlock(&gatt.listener_lock);
    for (i = 0; i < SDL_BLEGATT_MAX_LISTENERS; ++i) {
        if (gatt.listeners[i].listener) {
            listening = true;
        }
    }
    SDL_UnlockSpinlock(&gatt.listener_lock);
    if (listening) {
        bool stale = !gatt.watcher || !gatt.watcher_started;

        if (!stale && SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CAdvertisement_CIBluetoothLEAdvertisementWatcher_get_Status(gatt.watcher, &status))) {
            had_status = true;
            stale = (status == BluetoothLEAdvertisementWatcherStatus_Aborted || status == BluetoothLEAdvertisementWatcherStatus_Stopped);
        }
        if (stale) {
            SDL_BLEGATT_StopWatcher();
            if (SDL_BLEGATT_StartWatcher() && gatt.watcher_started) {
                restarted = true;
                gatt.watcher_failing = false;
            } else if (!gatt.watcher_failing) {
                failed = true;
                gatt.watcher_failing = true;
            }
            hr = gatt.watcher_hr;
            new_status = gatt.watcher_status;
        }
    }
    SDL_UnlockMutex(gatt.watcher_lock);
    // One line for each change, outside the lock. A watcher that keeps
    // failing to start is retried without a line each time.
    if (restarted && had_status) {
        SDL_BLEGATT_LOG("BLE: the advertisement watcher had stopped with status %d, started a new one", (int)status);
    } else if (restarted) {
        SDL_BLEGATT_LOG("BLE: the advertisement watcher started");
    } else if (failed) {
        SDL_BLEGATT_LOG("BLE: the advertisement watcher stopped, and a new one did not start, hr=0x%08lX, status %d",
                     (unsigned long)hr, new_status);
    }
    return restarted;
}

// ---------------------------------------------------------------------------
// Open, uncached discovery and characteristic lookup, called from the drivers'
// connect threads.
// ---------------------------------------------------------------------------
GattChar *SDL_BLEGATT_FindCharacteristic(GattService3 *service3, const GUID *uuid, SDL_AtomicInt *cancel)
{
    void *op = NULL;
    GattCharsResult *result = NULL;
    GattChar *found = NULL;

    // Uncached, so the read goes to the device rather than a stale OS cache, and
    // only trust the result when GattCommunicationStatus_Success (joycon2cpp checks
    // cr.Status() before reading characteristics, testapp.cpp:862). The link is up
    // by now (service discovery already succeeded), so a single attempt suffices.
    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3_GetCharacteristicsForUuidWithCacheModeAsync(service3, *uuid, BluetoothCacheMode_Uncached, (void *)&op)) || !op) {
        return NULL;
    }
    if (SDL_BLEGATT_Await(op, &IID_AsyncCharsHandler, cancel)) {
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
static BleDevice *SDL_BLEGATT_AwaitDevice(void *op, SDL_AtomicInt *cancel)
{
    BleDevice *device = NULL;
    if (!op) {
        return NULL;
    }
    if (SDL_BLEGATT_AwaitTimeout(op, 20000, &IID_AsyncDeviceHandler, cancel)) {
        typedef HRESULT(STDMETHODCALLTYPE * GetResults_t)(void *This, BleDevice **out);
        void ***vt = (void ***)op;
        HRESULT gr = ((GetResults_t)(*vt)[8])(op, &device);
        // Diagnostic (hifihedgehog/SDL#5): pin candidate (c). A returning-fast wait
        // with GetResults S_OK but device NULL means a synchronous null completion.
        SDL_BLEGATT_LOG("BLE device open: GetResults hr=0x%08lX device=%p",
                     (unsigned long)gr, (void *)device);
    }
    ((BleDevice *)op)->lpVtbl->Release((BleDevice *)op);
    return device;
}

BleDevice *SDL_BLEGATT_OpenDevice(Uint64 bluetooth_address, SDL_AtomicInt *cancel)
{
    BleDeviceStatics *statics = NULL;
    void *op = NULL;
    BleDevice *device = NULL;

    if (FAILED(SDL_BLEGATT_GetActivationFactory(RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice, &IID_BleDeviceStatics, (void **)&statics))) {
        return NULL;
    }
    // Open by raw address and let the OS resolve the address type. The Switch 2 Pro
    // Controller advertises a Public address (switch2-bt PHASE_B_LOG.md:48, and the
    // live trace reported type=0), and this single-arg open is exactly joycon2cpp's
    // working call (testapp.cpp:835). This runs on a dedicated worker thread (the
    // driver's connect thread), so blocking the 20 s await here does not starve the
    // WinRT thread pool that has to deliver the completion.
    op = NULL;
    {
        // Diagnostic (hifihedgehog/SDL#5): pin candidate (a). A failed HRESULT here
        // skips the await entirely, so device stays NULL with no wait.
        HRESULT open_hr = __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDeviceStatics_FromBluetoothAddressAsync(statics, bluetooth_address, (void *)&op);
        SDL_BLEGATT_LOG("BLE device open: FromBluetoothAddressAsync hr=0x%08lX op=%p",
                     (unsigned long)open_hr, (void *)op);
        if (SUCCEEDED(open_hr)) {
            device = SDL_BLEGATT_AwaitDevice(op, cancel);
        }
    }
    __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDeviceStatics_Release(statics);
    return device;
}

GattService3 *SDL_BLEGATT_FindService(BleDevice3 *device3, const GUID *uuids, int nuuids, int attempts,
                                      const char *label, SDL_AtomicInt *cancel)
{
    void *op = NULL;
    GattServicesResult *services_result = NULL;
    GattService *service = NULL;
    GattService3 *service3 = NULL;

    if (!device3 || !uuids || nuuids <= 0) {
        return NULL;
    }
    // Discover the service. The Switch 2 driver connects bond-free (no SMP, no OS
    // pairing), so the first GATT query returns before the ACL/GATT link is up and
    // a Cached read sees an empty table. Query Uncached and retry until the result
    // is GattCommunicationStatus_Success with the service present, matching the
    // proven joycon2cpp connect, which loops GetGattServicesAsync(Uncached) up to
    // 10x at 500 ms checking Success (joycon2cpp/testapp/src/testapp.cpp:850-855).
    // Each try asks for the UUIDs in order, for a family whose first service can
    // carry an alternate UUID.
    {
        int attempt;
        for (attempt = 1; attempt <= attempts && !service; ++attempt) {
            GattCommStatus status = GattCommunicationStatus_Unreachable;
            unsigned size = 0;
            int u;
            if (SDL_BLEGATT_Canceled(cancel)) {
                break;
            }
            for (u = 0; u < nuuids && !service && !SDL_BLEGATT_Canceled(cancel); ++u) {
                status = GattCommunicationStatus_Unreachable;
                size = 0;
                op = NULL;
                services_result = NULL;
                if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3_GetGattServicesForUuidWithCacheModeAsync(device3, uuids[u], BluetoothCacheMode_Uncached, (void *)&op)) && op) {
                    if (SDL_BLEGATT_Await(op, &IID_AsyncServicesHandler, cancel)) {
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
            }
            SDL_BLEGATT_LOG("BLE %s GATT discovery attempt %d/%d: status=%d services=%u",
                         label, attempt, attempts, (int)status, size);
            if (!service && attempt < attempts) {
                if (!SDL_BLEGATT_Delay(500, cancel)) { // ride out the bond-free link-up window
                    break;
                }
            }
        }
    }
    if (service) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService_QueryInterface(service, &IID_GattService3, (void **)&service3);
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService_Release(service);
    }
    return service3;
}

void SDL_BLEGATT_RequestThroughput(BleDevice *device)
{
    // Best-effort throughput bump, now that the link is up. joycon2cpp requests it
    // after discovery (testapp.cpp:899). Win10 1809+. Failure is ignored.
    BleDevice6 *device6 = NULL;
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_QueryInterface(device, &IID_BleDevice6, (void **)&device6))) {
        BleConnParamStatics *cp_statics = NULL;
        if (SUCCEEDED(SDL_BLEGATT_GetActivationFactory(RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEPreferredConnectionParameters, &IID_BleConnParamStatics, (void **)&cp_statics))) {
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

// ---------------------------------------------------------------------------
// The SDL_BLEGATTLink API of the generic driver.
// ---------------------------------------------------------------------------

// A GattSession for the device with MaintainConnection set, best effort, as the
// Windows Gear VR driver does (gear_vr_controller
// src/infrastructure/bluetooth/connection/pairing.rs:17-26: BluetoothDeviceId,
// GattSession::FromDeviceIdAsync, SetMaintainConnection(true)). SDK
// windows.devices.bluetooth.genericattributeprofile.h:7068 FromDeviceIdAsync,
// :7007 put_MaintainConnection. NULL when Windows gives none.
static GattSession *SDL_BLEGATT_OpenSession(BleDevice *device, SDL_AtomicInt *cancel)
{
    BleDevice4 *device4 = NULL;
    BleDeviceId *device_id = NULL;
    GattSessionStatics *statics = NULL;
    SessionOp *op = NULL;
    GattSession *session = NULL;

    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_QueryInterface(device, &IID_BleDevice4, (void **)&device4)) || !device4) {
        return NULL;
    }
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice4_get_BluetoothDeviceId(device4, &device_id)) && device_id) {
        if (SUCCEEDED(SDL_BLEGATT_GetActivationFactory(RuntimeClass_Windows_Devices_Bluetooth_GenericAttributeProfile_GattSession, &IID_GattSessionStatics, (void **)&statics))) {
            if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattSessionStatics_FromDeviceIdAsync(statics, device_id, &op)) && op) {
                if (SDL_BLEGATT_Await(op, &IID_AsyncSessionHandler, cancel)) {
                    __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattSession_GetResults(op, &session);
                }
                __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattSession_Release(op);
            }
            __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattSessionStatics_Release(statics);
        }
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothDeviceId_Release(device_id);
    }
    __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice4_Release(device4);
    if (session) {
        HRESULT hr = __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattSession_put_MaintainConnection(session, TRUE);
        SDL_BLEGATT_LOG("BLE GATT session: MaintainConnection hr=0x%08lX", (unsigned long)hr);
    }
    return session;
}

// DeviceInformation.Pairing of the device, the path the Windows Gear VR driver
// reads IsPaired through (gear_vr_controller connection/pairing.rs:32-34).
// NULL when Windows gives none.
static DevicePairing *SDL_BLEGATT_GetPairing(BleDevice *device)
{
    BleDevice2 *device2 = NULL;
    DeviceInfo *info = NULL;
    DeviceInfo2 *info2 = NULL;
    DevicePairing *pairing = NULL;

    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_QueryInterface(device, &IID_BleDevice2, (void **)&device2)) || !device2) {
        return NULL;
    }
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice2_get_DeviceInformation(device2, &info)) && info) {
        if (SUCCEEDED(__x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformation_QueryInterface(info, &IID_DeviceInfo2, (void **)&info2)) && info2) {
            __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformation2_get_Pairing(info2, &pairing);
            __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformation2_Release(info2);
        }
        __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformation_Release(info);
    }
    __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice2_Release(device2);
    return pairing;
}

// IDeviceInformationPairing2, which carries Custom and UnpairAsync
static DevicePairing2 *SDL_BLEGATT_GetPairing2(BleDevice *device)
{
    DevicePairing *pairing = SDL_BLEGATT_GetPairing(device);
    DevicePairing2 *pairing2 = NULL;

    if (pairing) {
        if (FAILED(__x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationPairing_QueryInterface(pairing, &IID_DevicePairing2, (void **)&pairing2))) {
            pairing2 = NULL;
        }
        __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationPairing_Release(pairing);
    }
    return pairing2;
}

static bool SDL_BLEGATT_IsPaired(BleDevice *device)
{
    DevicePairing *pairing = SDL_BLEGATT_GetPairing(device);
    boolean paired = FALSE;

    if (pairing) {
        if (FAILED(__x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationPairing_get_IsPaired(pairing, &paired))) {
            paired = FALSE;
        }
        __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationPairing_Release(pairing);
    }
    return paired != FALSE;
}

static bool SDL_BLEGATT_ValidCharacteristic(const SDL_BLEGATTLink *link, int characteristic)
{
    return link && characteristic >= 0 && characteristic < SDL_BLE_MAX_CHARS && link->characteristics[characteristic];
}

static SDL_BLEStatus SDL_BLEGATT_MapStatus(GattCommStatus status)
{
    switch (status) {
    case GattCommunicationStatus_Success:
        return SDL_BLE_STATUS_SUCCESS;
    case GattCommunicationStatus_Unreachable:
        return SDL_BLE_STATUS_UNREACHABLE;
    case GattCommunicationStatus_ProtocolError:
        return SDL_BLE_STATUS_PROTOCOL_ERROR;
    case GattCommunicationStatus_AccessDenied:
        return SDL_BLE_STATUS_ACCESS_DENIED;
    default:
        return SDL_BLE_STATUS_FAILED;
    }
}

// IClosable.Close before the last Release. BluetoothLEDevice, GattSession and
// GattDeviceService implement Windows.Foundation.IClosable, and bleak
// (backends/winrt/client.py:486-503), Qt
// (src/bluetooth/qlowenergycontroller_winrt.cpp:1122-1123), the Windows Gear
// VR driver (gear_vr_controller src/infrastructure/bluetooth/service.rs:159)
// and the paddle reader (SDL_xinput_paddle_gatt.cpp:531-537) close theirs.
// Returns what Close returned, or the failed QueryInterface.
static HRESULT SDL_BLEGATT_CloseObject(void *object)
{
    IUnknown *unknown = (IUnknown *)object;
    Closable *closable = NULL;
    HRESULT hr = unknown->lpVtbl->QueryInterface(unknown, &IID_Closable, (void **)&closable);

    if (SUCCEEDED(hr) && closable) {
        hr = __x_ABI_CWindows_CFoundation_CIClosable_Close(closable);
        __x_ABI_CWindows_CFoundation_CIClosable_Release(closable);
    }
    return hr;
}

// Removes the value delegates and releases the characteristics, then closes
// and releases the services of the last discovery, after the pause bleak
// takes (SDL_BLEGATT_CLOSE_SETTLE_MS). The delegates stay allocated.
static void SDL_BLEGATT_ReleaseDiscovery(SDL_BLEGATTLink *link)
{
    bool services = false;
    int i;

    for (i = 0; i < SDL_BLE_MAX_CHARS; ++i) {
        if (link->characteristics[i]) {
            if (link->value_handlers[i]) {
                __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_remove_ValueChanged(link->characteristics[i], link->value_tokens[i]);
            }
            __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(link->characteristics[i]);
            link->characteristics[i] = NULL;
        }
        link->value_handlers[i] = NULL;
        SDL_zero(link->value_tokens[i]);
    }
    for (i = 0; i < SDL_BLE_MAX_SERVICES; ++i) {
        if (link->services[i]) {
            services = true;
        }
    }
    if (services) {
        SDL_Delay(SDL_BLEGATT_CLOSE_SETTLE_MS);
    }
    for (i = 0; i < SDL_BLE_MAX_SERVICES; ++i) {
        if (link->services[i]) {
            (void)SDL_BLEGATT_CloseObject(link->services[i]);
            __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3_Release(link->services[i]);
            link->services[i] = NULL;
        }
    }
}

// Arms the status delegate, since the link has been up, then reads the status
// once, as SDL_BLEGATT_AddStatusHandler does after registering. The event
// fires only on a change, so a link that dropped while the delegate was
// disarmed is reported here or never.
static void SDL_BLEGATT_ArmStatus(SDL_BLEGATTLink *link)
{
    enum __x_ABI_CWindows_CDevices_CBluetooth_CBluetoothConnectionStatus status = BluetoothConnectionStatus_Connected;

    if (!link->status_handler) {
        return;
    }
    SDL_SetAtomicInt(&link->status_handler->armed, 1);
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_get_ConnectionStatus(link->device, &status)) &&
        status == BluetoothConnectionStatus_Disconnected) {
        SDL_SetAtomicInt(link->status_handler->lost, 1);
    }
}

SDL_BLEGATTLink *SDL_BLEGATT_Open(Uint64 address, SDL_AtomicInt *lost, SDL_AtomicInt *cancel, bool *bonded)
{
    SDL_BLEGATTLink *link;
    BleDevice *device;

    if (bonded) {
        *bonded = false;
    }
    if (!gatt.RoGetActivationFactory || SDL_BLEGATT_Canceled(cancel)) {
        return NULL;
    }
    device = SDL_BLEGATT_OpenDevice(address, cancel);
    SDL_BLEGATT_LOG("BLE GATT open addr %012llx: device=%p", (unsigned long long)address, (void *)device);
    if (!device) {
        return NULL;
    }
    link = (SDL_BLEGATTLink *)SDL_calloc(1, sizeof(*link));
    if (!link) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(device);
        return NULL;
    }
    link->address = address;
    link->device = device;
    link->session = SDL_BLEGATT_OpenSession(device, cancel);

    // Register ConnectionStatusChanged and poll the status once, as the Switch
    // 2 driver does. The delegate starts disarmed (see SDL_BLEGATT_StatusHandler),
    // so a Connected status here arms it and a Disconnected one means Windows
    // has not connected yet. SDL_BLEGATT_Discover arms it once the service is
    // found, since the link was up then.
    if (lost && !SDL_BLEGATT_Canceled(cancel)) {
        link->status_handler = SDL_BLEGATT_NewStatusHandler(lost, false);
        if (link->status_handler) {
            if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_add_ConnectionStatusChanged(device, (void *)link->status_handler, &link->status_token))) {
                enum __x_ABI_CWindows_CDevices_CBluetooth_CBluetoothConnectionStatus status = BluetoothConnectionStatus_Disconnected;
                link->status_registered = true;
                if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_get_ConnectionStatus(device, &status)) &&
                    status == BluetoothConnectionStatus_Connected) {
                    SDL_SetAtomicInt(&link->status_handler->armed, 1);
                }
            } else {
                // Never registered, so no callback can reach it
                SDL_free(link->status_handler);
                link->status_handler = NULL;
            }
        }
    }
    if (SDL_BLEGATT_Canceled(cancel) || (lost && !link->status_registered)) {
        SDL_BLEGATT_Close(link);
        return NULL;
    }
    if (bonded) {
        *bonded = SDL_BLEGATT_IsPaired(device);
    }
    return link;
}

bool SDL_BLEGATT_Pair(SDL_BLEGATTLink *link, SDL_AtomicInt *cancel)
{
    DevicePairing2 *pairing2;
    DeviceCustomPairing *custom = NULL;
    bool paired = false;

    if (!link || SDL_BLEGATT_Canceled(cancel)) {
        return false;
    }
    pairing2 = SDL_BLEGATT_GetPairing2(link->device);
    if (!pairing2) {
        return false;
    }
    // DeviceInformation.Pairing.Custom.PairAsync(ConfirmOnly, Encryption), which
    // the ABI names PairWithProtectionLevelAsync (MicrosoftDocs/winrt-api
    // windows.devices.enumeration/deviceinformationcustompairing_pairasync_991868827.md:6-13,
    // devicepairingprotectionlevel.md:22-23). PairingRequested must be handled
    // first, or Windows refuses a ConfirmOnly pairing.
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationPairing2_get_Custom(pairing2, &custom)) && custom) {
        EventRegistrationToken token;
        SDL_zero(token);
        if (SUCCEEDED(__x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationCustomPairing_add_PairingRequested(custom, (void *)&g_pairing_handler, &token))) {
            PairingOp *op = NULL;
            if (SUCCEEDED(__x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationCustomPairing_PairWithProtectionLevelAsync(custom, DevicePairingKinds_ConfirmOnly, DevicePairingProtectionLevel_Encryption, &op)) && op) {
                if (SDL_BLEGATT_AwaitTimeout(op, SDL_BLEGATT_PAIR_TIMEOUT_MS, &IID_AsyncPairingHandler, cancel)) {
                    PairingResult *result = NULL;
                    if (SUCCEEDED(__FIAsyncOperation_1_Windows__CDevices__CEnumeration__CDevicePairingResult_GetResults(op, &result)) && result) {
                        enum __x_ABI_CWindows_CDevices_CEnumeration_CDevicePairingResultStatus status = DevicePairingResultStatus_NotPaired;
                        __x_ABI_CWindows_CDevices_CEnumeration_CIDevicePairingResult_get_Status(result, &status);
                        paired = (status == DevicePairingResultStatus_Paired || status == DevicePairingResultStatus_AlreadyPaired);
                        SDL_BLEGATT_LOG("BLE GATT pair addr %012llx: status=%d", (unsigned long long)link->address, (int)status);
                        __x_ABI_CWindows_CDevices_CEnumeration_CIDevicePairingResult_Release(result);
                    }
                }
                __FIAsyncOperation_1_Windows__CDevices__CEnumeration__CDevicePairingResult_Release(op);
            }
            __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationCustomPairing_remove_PairingRequested(custom, token);
        }
        __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationCustomPairing_Release(custom);
    }
    __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationPairing2_Release(pairing2);
    return paired;
}

bool SDL_BLEGATT_RemoveBond(SDL_BLEGATTLink *link, SDL_AtomicInt *cancel)
{
    DevicePairing2 *pairing2;
    UnpairingOp *op = NULL;
    bool unpaired = false;

    if (!link || SDL_BLEGATT_Canceled(cancel)) {
        return false;
    }
    pairing2 = SDL_BLEGATT_GetPairing2(link->device);
    if (!pairing2) {
        return false;
    }
    // Unpaired or AlreadyUnpaired both leave no bond, as the Windows Gear VR
    // driver reads them (gear_vr_controller connection/pairing.rs:124-130)
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationPairing2_UnpairAsync(pairing2, &op)) && op) {
        if (SDL_BLEGATT_AwaitTimeout(op, SDL_BLEGATT_PAIR_TIMEOUT_MS, &IID_AsyncUnpairingHandler, cancel)) {
            UnpairingResult *result = NULL;
            if (SUCCEEDED(__FIAsyncOperation_1_Windows__CDevices__CEnumeration__CDeviceUnpairingResult_GetResults(op, &result)) && result) {
                enum __x_ABI_CWindows_CDevices_CEnumeration_CDeviceUnpairingResultStatus status = DeviceUnpairingResultStatus_Failed;
                __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceUnpairingResult_get_Status(result, &status);
                unpaired = (status == DeviceUnpairingResultStatus_Unpaired || status == DeviceUnpairingResultStatus_AlreadyUnpaired);
                SDL_BLEGATT_LOG("BLE GATT unpair addr %012llx: status=%d", (unsigned long long)link->address, (int)status);
                __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceUnpairingResult_Release(result);
            }
        }
        __FIAsyncOperation_1_Windows__CDevices__CEnumeration__CDeviceUnpairingResult_Release(op);
    }
    __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceInformationPairing2_Release(pairing2);
    return unpaired;
}

bool SDL_BLEGATT_Discover(SDL_BLEGATTLink *link, const SDL_BLEFamily *family, bool *found, Uint8 *properties,
                          SDL_AtomicInt *cancel)
{
    BleDevice3 *device3 = NULL;
    GUID candidates[2];
    int ncandidates = 0;
    int ncharacteristics;
    int nservices;
    int i;

    if (!family || !found || !properties) {
        return false;
    }
    ncharacteristics = SDL_clamp(family->ncharacteristics, 0, SDL_BLE_MAX_CHARS);
    nservices = SDL_clamp(family->nservices, 0, SDL_BLE_MAX_SERVICES);
    for (i = 0; i < ncharacteristics; ++i) {
        found[i] = false;
        properties[i] = 0;
    }
    if (!link || nservices < 1 || SDL_BLEGATT_Canceled(cancel)) {
        return false;
    }
    // A repeated discovery replaces the last one
    SDL_BLEGATT_ReleaseDiscovery(link);

    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_QueryInterface(link->device, &IID_BleDevice3, (void **)&device3)) || !device3) {
        return false;
    }
    if (family->has_alternate) {
        SDL_BLEGATT_GUIDFromUUID(&family->alternate, &candidates[ncandidates++]);
    }
    SDL_BLEGATT_GUIDFromUUID(&family->services[0], &candidates[ncandidates++]);
    link->services[0] = SDL_BLEGATT_FindService(device3, candidates, ncandidates, 10, family->name, cancel);
    if (!link->services[0]) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3_Release(device3);
        return false;
    }
    // The service came back uncached, so the link was up
    SDL_BLEGATT_ArmStatus(link);
    // The other services, once each, now that the link is up
    for (i = 1; i < nservices && !SDL_BLEGATT_Canceled(cancel); ++i) {
        GUID uuid;
        SDL_BLEGATT_GUIDFromUUID(&family->services[i], &uuid);
        link->services[i] = SDL_BLEGATT_FindService(device3, &uuid, 1, 1, family->name, cancel);
    }
    __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3_Release(device3);

    for (i = 0; i < ncharacteristics && !SDL_BLEGATT_Canceled(cancel); ++i) {
        const SDL_BLECharacteristic *characteristic = &family->characteristics[i];
        if (characteristic->service < nservices && link->services[characteristic->service]) {
            GUID uuid;
            SDL_BLEGATT_GUIDFromUUID(&characteristic->uuid, &uuid);
            link->characteristics[i] = SDL_BLEGATT_FindCharacteristic(link->services[characteristic->service], &uuid, cancel);
        }
        if (link->characteristics[i]) {
            enum __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattCharacteristicProperties props = 0;
            __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_get_CharacteristicProperties(link->characteristics[i], &props);
            found[i] = true;
            properties[i] = (Uint8)props; // the SDL_BLE_PROPERTY_* bits are the low byte
        }
        SDL_BLEGATT_LOG("BLE %s characteristic %d: found=%d properties=0x%02x",
                     family->name, i, (int)found[i], (unsigned)properties[i]);
    }
    if (SDL_BLEGATT_Canceled(cancel)) {
        return false;
    }
    SDL_BLEGATT_RequestThroughput(link->device);
    return true;
}

// WriteClientCharacteristicConfigurationDescriptorWithResultAsync
// (windows.devices.bluetooth.genericattributeprofile.h:4271-4274), whose
// GattWriteResult carries the ATT error of a ProtocolError. Before
// IGattCharacteristic3 only the status comes back, through the call the
// Switch 2 driver makes.
static SDL_BLEStatus SDL_BLEGATT_WriteDescriptor(GattChar *characteristic,
                                                 enum __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattClientCharacteristicConfigurationDescriptorValue value,
                                                 Uint8 *protocol_error, SDL_AtomicInt *cancel)
{
    GattChar3 *characteristic3 = NULL;
    SDL_BLEStatus result = SDL_BLE_STATUS_FAILED;

    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_QueryInterface(characteristic, &IID_GattChar3, (void **)&characteristic3)) && characteristic3) {
        WriteResultOp *op = NULL;
        if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic3_WriteClientCharacteristicConfigurationDescriptorWithResultAsync(characteristic3, value, &op)) && op) {
            if (SDL_BLEGATT_Await(op, &IID_AsyncWriteHandler, cancel)) {
                GattWriteResult *write_result = NULL;
                if (SUCCEEDED(__FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattWriteResult_GetResults(op, &write_result)) && write_result) {
                    GattCommStatus status = GattCommunicationStatus_Unreachable;
                    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattWriteResult_get_Status(write_result, &status))) {
                        result = SDL_BLEGATT_MapStatus(status);
                        if (status == GattCommunicationStatus_ProtocolError) {
                            __FIReference_1_byte *error = NULL;
                            if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattWriteResult_get_ProtocolError(write_result, &error)) && error) {
                                BYTE code = 0;
                                if (SUCCEEDED(__FIReference_1_byte_get_Value(error, &code))) {
                                    *protocol_error = code;
                                }
                                __FIReference_1_byte_Release(error);
                            }
                        }
                    }
                    __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattWriteResult_Release(write_result);
                }
            }
            __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattWriteResult_Release(op);
        }
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic3_Release(characteristic3);
    } else {
        StatusOp *op = NULL;
        if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_WriteClientCharacteristicConfigurationDescriptorAsync(characteristic, value, &op)) && op) {
            if (SDL_BLEGATT_Await(op, &IID_AsyncStatusHandler, cancel)) {
                GattCommStatus status = GattCommunicationStatus_Unreachable;
                if (SUCCEEDED(__FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCommunicationStatus_GetResults(op, &status))) {
                    result = SDL_BLEGATT_MapStatus(status);
                }
            }
            __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattCommunicationStatus_Release(op);
        }
    }
    return result;
}

SDL_BLEStatus SDL_BLEGATT_Subscribe(SDL_BLEGATTLink *link, int characteristic, Uint8 cccd, Uint8 *protocol_error,
                                    SDL_BLEGATT_ValueCallback callback, void *userdata, SDL_AtomicInt *cancel)
{
    enum __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattClientCharacteristicConfigurationDescriptorValue value;
    Uint8 error = 0;
    SDL_BLEStatus status;

    if (protocol_error) {
        *protocol_error = 0;
    }
    if (!SDL_BLEGATT_ValidCharacteristic(link, characteristic) || SDL_BLEGATT_Canceled(cancel)) {
        return SDL_BLE_STATUS_FAILED;
    }
    // GattClientCharacteristicConfigurationDescriptorValue (SDK header :3853-3855)
    switch (cccd) {
    case SDL_BLE_CCCD_NONE:
        value = GattClientCharacteristicConfigurationDescriptorValue_None;
        break;
    case SDL_BLE_CCCD_NOTIFY:
        value = GattClientCharacteristicConfigurationDescriptorValue_Notify;
        break;
    case SDL_BLE_CCCD_INDICATE:
        value = GattClientCharacteristicConfigurationDescriptorValue_Indicate;
        break;
    default:
        return SDL_BLE_STATUS_FAILED;
    }

    // The value delegate goes in before the descriptor write, as the Switch 2
    // driver orders them (controller.py order), and only once per discovery
    if (!link->value_handlers[characteristic] && callback) {
        SDL_BLEGATTValueHandler *handler = SDL_BLEGATT_NewValueHandler(callback, userdata, characteristic);
        if (!handler) {
            return SDL_BLE_STATUS_FAILED;
        }
        if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_add_ValueChanged(link->characteristics[characteristic], (void *)handler, &link->value_tokens[characteristic]))) {
            // Never registered, so no callback can reach it
            SDL_free(handler);
            return SDL_BLE_STATUS_FAILED;
        }
        link->value_handlers[characteristic] = handler;
    }

    status = SDL_BLEGATT_WriteDescriptor(link->characteristics[characteristic], value, &error, cancel);
    SDL_BLEGATT_LOG("BLE GATT subscribe characteristic %d cccd %d: status=%d protocol_error=0x%02x",
                 characteristic, (int)cccd, (int)status, (unsigned)error);
    if (protocol_error) {
        *protocol_error = error;
    }
    return status;
}

bool SDL_BLEGATT_Read(SDL_BLEGATTLink *link, int characteristic, Uint8 *data, size_t *length, SDL_AtomicInt *cancel)
{
    ReadResultOp *op = NULL;
    bool result = false;

    if (!data || !length || !SDL_BLEGATT_ValidCharacteristic(link, characteristic) || SDL_BLEGATT_Canceled(cancel)) {
        return false;
    }
    if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_ReadValueWithCacheModeAsync(link->characteristics[characteristic], BluetoothCacheMode_Uncached, &op)) && op) {
        if (SDL_BLEGATT_Await(op, &IID_AsyncReadHandler, cancel)) {
            GattReadResult *read_result = NULL;
            if (SUCCEEDED(__FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattReadResult_GetResults(op, &read_result)) && read_result) {
                GattCommStatus status = GattCommunicationStatus_Unreachable;
                Buffer *value = NULL;
                if (SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattReadResult_get_Status(read_result, &status)) &&
                    status == GattCommunicationStatus_Success &&
                    SUCCEEDED(__x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattReadResult_get_Value(read_result, &value)) && value) {
                    const Uint8 *bytes = NULL;
                    UINT32 size = 0;
                    if (SDL_BLEGATT_BufferBytes(value, &bytes, &size) && size <= *length) {
                        SDL_memcpy(data, bytes, size);
                        *length = size;
                        result = true;
                    }
                    __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release(value);
                }
                __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattReadResult_Release(read_result);
            }
        }
        __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattReadResult_Release(op);
    }
    return result;
}

bool SDL_BLEGATT_Write(SDL_BLEGATTLink *link, int characteristic, const Uint8 *data, size_t length, bool response,
                       SDL_AtomicInt *cancel)
{
    if (!data || length > (size_t)SDL_MAX_SINT32 || !SDL_BLEGATT_ValidCharacteristic(link, characteristic) ||
        SDL_BLEGATT_Canceled(cancel)) {
        return false;
    }
    return SDL_BLEGATT_WriteValue(link->characteristics[characteristic], data, (int)length, response, cancel, true);
}

void SDL_BLEGATT_Close(SDL_BLEGATTLink *link)
{
    HRESULT session_hr = S_FALSE;
    HRESULT device_hr = S_FALSE;

    if (!link) {
        return;
    }
    // Every handler goes before anything closes, as bleak removes its
    // handlers (backends/winrt/client.py:467-481) before it closes the
    // services, the session and the device (:486-503). This path is the
    // generic driver's. The Switch 2 driver releases its objects in
    // BLE_FreeController, which closes nothing.
    if (link->status_registered) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_remove_ConnectionStatusChanged(link->device, link->status_token);
        link->status_registered = false;
    }
    SDL_BLEGATT_ReleaseDiscovery(link);
    if (link->session) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattSession_put_MaintainConnection(link->session, FALSE);
        session_hr = SDL_BLEGATT_CloseObject(link->session);
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattSession_Release(link->session);
    }
    if (link->device) {
        device_hr = SDL_BLEGATT_CloseObject(link->device);
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(link->device);
    }
    SDL_BLEGATT_LOG("BLE GATT close addr %012llx: session Close hr=0x%08lX, device Close hr=0x%08lX",
                 (unsigned long long)link->address, (unsigned long)session_hr, (unsigned long)device_hr);
    // The delegates are kept, as BLE_FreeController keeps the Switch 2 driver's:
    // a callback may still be in flight on a WinRT thread-pool thread, and
    // remove_ValueChanged does not drain in-flight invocations. The link itself
    // is freed, since no callback touches it. The memory behind lost and each
    // callback's userdata stays the caller's to keep.
    SDL_free(link);
}

#endif // SDL_JOYSTICK_BLE
