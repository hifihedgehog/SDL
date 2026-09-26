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

/* The WinRT Bluetooth LE GATT transport shared by the Switch 2 driver
 * (SDL_ble_switch2joystick.c) and the generic BLE GATT driver
 * (SDL_blegattjoystick.c) of hifihedgehog/SDL#33 Part 12. It holds the
 * WinRT-from-C plumbing that the Switch 2 driver proved: activation and
 * awaits, buffers, delegates, the advertisement watcher, and the open and
 * uncached discovery with its retries. Every call that awaits blocks its
 * thread, so it runs on a worker thread, never on a WinRT callback thread. */

#ifndef SDL_ble_gatt_h_
#define SDL_ble_gatt_h_

#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_BLE

#include "../ble/SDL_ble_session.h"

/* Runtime: resolves the combase entry points and pins the MTA. Counted, so
   both drivers can hold it. False when WinRT is not available.
   The first SDL_BLEGATT_Init runs WIN_RoInitialize on the calling thread, and
   the SDL_BLEGATT_Quit that drops the last reference runs WIN_RoUninitialize
   on its own. A driver therefore calls them only from its Init and its Quit,
   which SDL runs on the thread that initializes and quits the joystick
   subsystem, as the WGI driver pairs its own
   (SDL_windows_gaming_input.c:595-598 and :1007-1009). */
extern bool SDL_BLEGATT_Init(void);
extern void SDL_BLEGATT_Quit(void);

/* The advertisement watcher, active scanning with extended advertisements
   allowed. It runs while any listener is added. A listener runs on a WinRT
   thread-pool thread with the parsed event and must return at once. The
   listeners run one after another on that thread, so one that waits delays
   the next. After SDL_BLEGATT_RemoveListener returns, a call already running
   can still finish, so a listener checks its own driver's state. */
typedef void (*SDL_BLEGATT_Listener)(const SDL_BLEAdvertisement *advertisement, void *userdata);
extern bool SDL_BLEGATT_AddListener(SDL_BLEGATT_Listener listener, void *userdata);
extern void SDL_BLEGATT_RemoveListener(SDL_BLEGATT_Listener listener, void *userdata);

/* A watcher that aborts, as when the radio goes off, or one whose start
   failed or left it Aborted, never runs again by itself. While any listener
   is added, this reads the watcher's status and puts a new watcher in place
   of one that is Aborted or Stopped or never started. It logs each change
   of state and returns true when a new watcher started. */
extern bool SDL_BLEGATT_CheckWatcher(void);

/* One connection to one device, used by one thread at a time */
typedef struct SDL_BLEGATTLink SDL_BLEGATTLink;

/* A characteristic value, on a WinRT thread-pool thread. data is valid only
   during the call, and length is the whole value, never cut short. */
typedef void (*SDL_BLEGATT_ValueCallback)(void *userdata, int characteristic, const Uint8 *data, size_t length);

/* Every call below that takes cancel returns early, failing, once *cancel is
   not 0. With a flag, an operation still running at its ceiling is canceled
   too. NULL cannot be canceled. */

/* Opens the device by address with a 20 s ceiling, holds a GattSession with
   MaintainConnection set where Windows allows it, and sets *lost to 1 when
   ConnectionStatusChanged or a status poll reports the link down after it
   has been up. Windows connects on the first GATT operation, so a device
   not yet connected right after the open is no loss. The link counts as up
   once a status says so or discovery finds the first service, and
   discovery then polls the status once. *bonded is true when Windows holds
   a bond. NULL on failure. */
extern SDL_BLEGATTLink *SDL_BLEGATT_Open(Uint64 address, SDL_AtomicInt *lost, SDL_AtomicInt *cancel, bool *bonded);

/* DeviceInformation.Pairing.Custom.PairAsync with ConfirmOnly and minimum
   protection Encryption, accepting from PairingRequested. True when paired
   or already paired. */
extern bool SDL_BLEGATT_Pair(SDL_BLEGATTLink *link, SDL_AtomicInt *cancel);
/* DeviceInformation.Pairing.UnpairAsync. True when unpaired or already so. */
extern bool SDL_BLEGATT_RemoveBond(SDL_BLEGATTLink *link, SDL_AtomicInt *cancel);

/* Finds the family's services and characteristics, uncached. The first
   service is tried up to 10 times 500 ms apart while the link comes up, its
   alternate UUID first when the family has one. Once it is found the link
   counts as up, and a Disconnected status read then sets the flag that
   SDL_BLEGATT_Open received. Then it requests throughput-optimized
   connection parameters, best effort. found and properties get one entry
   per family characteristic. False when the first service is not found. */
extern bool SDL_BLEGATT_Discover(SDL_BLEGATTLink *link, const SDL_BLEFamily *family, bool *found, Uint8 *properties,
                                 SDL_AtomicInt *cancel);

/* Registers the value callback for the characteristic once, then writes its
   descriptor with WriteClientCharacteristicConfigurationDescriptorWithResultAsync.
   *protocol_error gets the ATT error of a ProtocolError status. */
extern SDL_BLEStatus SDL_BLEGATT_Subscribe(SDL_BLEGATTLink *link, int characteristic, Uint8 cccd, Uint8 *protocol_error,
                                           SDL_BLEGATT_ValueCallback callback, void *userdata, SDL_AtomicInt *cancel);

/* ReadValueAsync, uncached. *length holds the capacity and gets the length.
   False when the read fails or the value does not fit. */
extern bool SDL_BLEGATT_Read(SDL_BLEGATTLink *link, int characteristic, Uint8 *data, size_t *length, SDL_AtomicInt *cancel);

/* Writes with response when asked and the characteristic has Write, else
   without response, as the Switch 2 driver chooses */
extern bool SDL_BLEGATT_Write(SDL_BLEGATTLink *link, int characteristic, const Uint8 *data, size_t length, bool response,
                              SDL_AtomicInt *cancel);

/* Removes the handlers, then closes the services, the GattSession and the
   device through IClosable and releases every WinRT object. Memory a
   callback can still touch is kept, as the Switch 2 driver keeps its. */
extern void SDL_BLEGATT_Close(SDL_BLEGATTLink *link);

/* Lower-level helpers for the Switch 2 driver (SDL_ble_switch2joystick.c),
   which keeps its own connect sequence on its own connect thread. The WinRT
   interfaces are declared by their struct tags, so this header needs no WinRT
   header. Every helper that awaits blocks its thread. A NULL cancel waits for
   the whole ceiling, as the Switch 2 driver always has. */

struct __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice;
struct __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3;
struct __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3;
struct __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic;
struct _GUID;
struct EventRegistrationToken;

typedef struct __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice SDL_BLEGATTDevice;
typedef struct __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3 SDL_BLEGATTDevice3;
typedef struct __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3 SDL_BLEGATTService;
typedef struct __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic SDL_BLEGATTCharacteristic;

/* Joins the calling worker thread to the MTA with RoInitialize. True when
   SDL_BLEGATT_QuitThread is owed. */
extern bool SDL_BLEGATT_InitThread(void);
extern void SDL_BLEGATT_QuitThread(void);

/* BluetoothLEDevice.FromBluetoothAddressAsync with the 20 s ceiling. NULL on
   failure. */
extern SDL_BLEGATTDevice *SDL_BLEGATT_OpenDevice(Uint64 address, SDL_AtomicInt *cancel);

/* The uncached service lookup: up to attempts tries 500 ms apart, each try
   asking for uuids in order, until one returns Success with the service.
   label names the device in the log. NULL when no try finds it. */
extern SDL_BLEGATTService *SDL_BLEGATT_FindService(SDL_BLEGATTDevice3 *device3, const struct _GUID *uuids, int nuuids,
                                                   int attempts, const char *label, SDL_AtomicInt *cancel);

/* Requests throughput-optimized connection parameters, best effort */
extern void SDL_BLEGATT_RequestThroughput(SDL_BLEGATTDevice *device);

/* The uncached characteristic lookup, one try. NULL when it is missing. */
extern SDL_BLEGATTCharacteristic *SDL_BLEGATT_FindCharacteristic(SDL_BLEGATTService *service3, const struct _GUID *uuid,
                                                                 SDL_AtomicInt *cancel);

/* Writes with response only when prefer_response is set and the
   characteristic has Write. True when the write completed within 3 s,
   whatever its status. */
extern bool SDL_BLEGATT_WriteCharacteristic(SDL_BLEGATTCharacteristic *characteristic, const Uint8 *bytes, int length,
                                            bool prefer_response);

/* Writes the descriptor to Notify. True when the write completed within
   3 s, whatever its status. */
extern bool SDL_BLEGATT_EnableNotifications(SDL_BLEGATTCharacteristic *characteristic);

/* Registers a ValueChanged delegate that calls callback with userdata and
   index. Returns the delegate, which is never freed, or NULL when it cannot
   be allocated. */
extern void *SDL_BLEGATT_AddValueHandler(SDL_BLEGATTCharacteristic *characteristic, SDL_BLEGATT_ValueCallback callback,
                                         void *userdata, int index, struct EventRegistrationToken *token);

/* Registers a ConnectionStatusChanged delegate on a link that is already
   up, then polls the status once. *lost gets 1 whenever the status reads
   Disconnected. Returns the delegate, which is never freed, or NULL when it
   cannot be allocated. */
extern void *SDL_BLEGATT_AddStatusHandler(SDL_BLEGATTDevice *device, SDL_AtomicInt *lost,
                                          struct EventRegistrationToken *token);

#endif /* SDL_JOYSTICK_BLE */

#endif /* SDL_ble_gatt_h_ */
