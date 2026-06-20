# BLE-GATT Switch 2 joystick driver: implementation design

Implementation plan for [hifihedgehog/SDL#5](https://github.com/hifihedgehog/SDL/issues/5). This is the SDL-fork-session design doc that the issue #5 spec asked for. It pins the architecture and the exact WinRT-from-C symbols resolved from the installed SDK (`10.0.26100.0`) so the implementation compiles, then layers the BLE flow on top.

Fork-only file. Bundled into PadForge. Not an upstream contribution. The upstream `SDL_hidapi_switch2.c` Bluetooth path stays the `// FIXME` stub. This driver claims the device at the BLE-GATT transport, which HIDAPI never sees.

## 1. Shape

New peer `SDL_JoystickDriver` `SDL_BLE_JoystickDriver` in `src/joystick/windows/SDL_ble_switch2joystick.c`, modeled byte-for-byte on the WGI driver (`SDL_windows_gaming_input.c`), SDL's existing WinRT-from-C joystick backend. WGI talks to `Windows.Gaming.Input`. This talks to `Windows.Devices.Bluetooth(.Advertisement|.GenericAttributeProfile)` plus `Windows.Storage.Streams`.

Gated by:
- build define `SDL_JOYSTICK_BLE` (whole `.c` wrapped in `#ifdef`),
- hint `SDL_HINT_JOYSTICK_BLE_SWITCH2`, default OFF. `Init` returns `true` early when unset so the driver loads but never scans.

The Switch 2 over BT is NOT HID-over-GATT. It advertises a custom 128-bit GATT service `ab7de9be-89fe-49ad-828f-118f09df7fd0`, so Windows never enumerates it as HID and hidapi cannot open it. This driver owns a WinRT BLE-GATT connection instead.

## 2. Build wiring

- `include/build_config/SDL_build_config.h.cmake`: add `#cmakedefine SDL_JOYSTICK_BLE 1` near `SDL_JOYSTICK_WGI`.
- `include/build_config/SDL_build_config_windows.h`: `#define SDL_JOYSTICK_BLE 1` near the WGI define.
- `CMakeLists.txt`: the Windows joystick `*.c` glob already picks up the new file. Set the feature flag next to `SDL_JOYSTICK_WGI`. No new link libs. Combase entrypoints are `GetProcAddress`'d at runtime (same as WGI), and all WinRT IIDs are `DEFINE_GUID`'d in-file (no `windowsapp.lib`).
- `include/SDL3/SDL_hints.h`: define `SDL_HINT_JOYSTICK_BLE_SWITCH2 "SDL_JOYSTICK_BLE_SWITCH2"` with a doc comment mirroring `SDL_HINT_JOYSTICK_HIDAPI_SWITCH2`.
- `src/joystick/SDL_sysjoystick.h`: add `extern SDL_JoystickDriver SDL_BLE_JoystickDriver`.
- `src/joystick/SDL_joystick.c`: register under `#ifdef SDL_JOYSTICK_BLE`, after the other Windows drivers (BLE is a transport nothing else enumerates, so ordering is low-risk).

## 3. WinRT-from-C strategy (the decisions that make it compile)

Three problems and the chosen resolutions, all verified against the SDK headers.

### 3a. Async without parameterized IIDs: permissive completed-handler
Every BLE call returns `IAsyncOperation<T>`. The parameterized PIIDs for `__FIAsyncOperationCompletedHandler_1_T` are NOT literal in the headers (MIDL-computed). Resolution: a hand-rolled completed-handler delegate whose `QueryInterface` is permissive. It returns `self`+AddRef for `IUnknown`/`IAgileObject`/anything except `IMarshal`, which returns `E_NOINTERFACE`. `put_Completed(op, &handler)` then wait on a semaphore, then `GetResults(op, &out)`. Both `put_Completed` and `GetResults` are COBJMACRO vtbl calls on the typed `__FIAsyncOperation_1_T*` the method already returns, so no PIID is ever needed. One reusable helper struct plus vtbl plus an `SDL_Semaphore`, used for every await. All awaits run on the driver's worker thread, never on a WinRT callback thread.

### 3b. IBufferByteAccess: hand-declared
`robuffer.h` is C++-only. Declare the classic COM interface in-file:
```c
// IID_IBufferByteAccess 905a0fef-bc53-11df-8c49-001e4fc686da
typedef struct IBufferByteAccessVtbl { /* QI, AddRef, Release, */ HRESULT (STDMETHODCALLTYPE *Buffer)(void *This, byte **value); } IBufferByteAccessVtbl;
```
QI an `IBuffer*` to it to read raw notification/read bytes.

### 3c. Build IBuffer from bytes: DataWriter
For GATT writes of commands and vibration (CCCD is an enum, not a buffer, so it skips this path): `RoActivateInstance(RuntimeClass_Windows_Storage_Streams_DataWriter)` then `IDataWriter` then `WriteBytes(n, bytes)` then `DetachBuffer(&ibuffer)`, pass to `WriteValueAsync`/`WriteValueWithOptionAsync`, then Release. `RoActivateInstance` is a combase export resolved like the other entrypoints.

### 3d. Delegates (event handlers)
Same hand-rolled-vtbl pattern as WGI (`SDL_windows_gaming_input.c:327-365`). For the two TypedEventHandlers the IIDs ARE in the headers, so strict QI like WGI:
- Advertisement `Received`: `90eb4eca-d465-5ea0-a61c-033c8c5ecef2`.
- GATT `ValueChanged`: `c1f420f6-6292-5760-a2c9-9ddf98683cfc`.

### 3e. Apartment / threading
`WIN_RoInitialize()` in `Init`, pin MTA with `CoIncrementMTAUsage`. BLE advertisement and ValueChanged fire on MTA thread-pool threads, exactly why WGI pins it (libsdl-org/SDL#5552). Connect/discover on a worker thread (await via 3a). `ValueChanged` pushes raw report bytes into a lock-protected ring buffer. The `Update` driver callback (SDL joystick thread) drains it and emits `SDL_SendJoystick*`. Never call `joystick->...` directly.

## 4. Resolved WinRT symbols (SDK 10.0.26100.0)

All C symbols prefixed `__x_ABI_CWindows_C...`. IIDs `DEFINE_GUID`'d in-file from these dashed strings. Contract-version gate: set `WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION` high enough (Device6/PreferredConnectionParameters need at least `0xd0000`) before the includes, or those macros compile out.

### Advertisement (`windows.devices.bluetooth.advertisement.h`)
- `...Advertisement_CIBluetoothLEAdvertisementWatcher` (`a6ac336f-f3d3-4297-8d6c-c81ea6623f40`): `put_ScanningMode`, `add_Received`, `remove_Received`, `Start`, `Stop`. Activatable via `RoActivateInstance(RuntimeClass_..._BluetoothLEAdvertisementWatcher)` then QI.
- `...CIBluetoothLEAdvertisementWatcher2` (`01bf26bc-b164-5805-90a3-e8a7997ff225`): `put_AllowExtendedAdvertisements` (QI from base).
- Received TypedEventHandler IID `90eb4eca-d465-5ea0-a61c-033c8c5ecef2`, `Invoke(This, sender, args)`.
- `...CIBluetoothLEAdvertisementReceivedEventArgs` (`27987ddf-e596-41be-8d43-9e6731d4a913`): `get_BluetoothAddress(UINT64*)`, `get_Advertisement`.
- `...CIBluetoothLEAdvertisement` (`066fb2b7-33d1-4e7d-8367-cf81d0f79653`): `get_ManufacturerData` returns `__FIVector_1_...ManufacturerData**`, iterate `get_Size`/`GetAt`.
- `...CIBluetoothLEManufacturerData` (`912dba18-6963-4533-b061-4694dafb34e5`): `get_CompanyId(UINT16*)`, `get_Data(IBuffer**)`.
- enum `BluetoothLEScanningMode_Active = 1`.

### Device (`windows.devices.bluetooth.h`)
- `CIBluetoothLEDeviceStatics` (`c8cf1a19-f0b6-4bf0-8689-41303de2d9f4`): `FromBluetoothAddressAsync(UINT64, IAsyncOperation<BluetoothLEDevice>**)`. RoGetActivationFactory(`RuntimeClass_..._BluetoothLEDevice`).
- `CIBluetoothLEDevice` (`b5ee2f7b-4ad8-4642-ac48-80a0b500e887`): `get_ConnectionStatus`, `get_BluetoothAddress`, `add/remove_ConnectionStatusChanged`.
- `CIBluetoothLEDevice3` (`aee9e493-44ac-40dc-af33-b2c13c01ca46`): `GetGattServicesAsync`, `GetGattServicesForUuidAsync(GUID, op**)`.
- `CIBluetoothLEDevice6` (`ca7190ef-0cae-573c-a1ca-e1fc5bfc39e2`): `RequestPreferredConnectionParameters(params*, request**)`, sync, best-effort, try/catch. NOTE: on 6, not 5.
- `CIBluetoothLEPreferredConnectionParametersStatics` (`0e3e8edc-2751-55aa-a838-8faeee818d72`): `get_ThroughputOptimized(params**)`. RoGetActivationFactory(`RuntimeClass_..._BluetoothLEPreferredConnectionParameters`).
- `CIBluetoothAdapterStatics` (`8b02fb6a-ac4c-4741-8661-8eab7d17ea9f`): `GetDefaultAsync` (host MAC for optional pairing). `CIBluetoothAdapter` (`7974f04c-...`): `get_BluetoothAddress`.
- enum `BluetoothConnectionStatus_Connected = 1`.

### GATT (`windows.devices.bluetooth.genericattributeprofile.h`)
- `...GenericAttributeProfile_CIGattDeviceServicesResult` (`171dd3ee-016d-419d-838a-576cf475a3d8`): `get_Status`, `get_Services(IVectorView<GattDeviceService>**)`.
- `CIGattDeviceService` (`ac7b7c05-...`): `get_Uuid`. QI to `CIGattDeviceService3` (`b293a950-0c53-437c-a9b3-5c3210c6e569`): `GetCharacteristicsForUuidAsync(GUID, op**)`.
- `CIGattCharacteristicsResult` (`1194945c-b257-4f3e-9db7-f68bc9a9aef2`): `get_Status`, `get_Characteristics(IVectorView<GattCharacteristic>**)`.
- `CIGattCharacteristic` (`59cb50c1-5934-4f68-a198-eb864fa44e6b`): `get_Uuid`, `WriteValueAsync(IBuffer, op<GattCommunicationStatus>**)`, `WriteValueWithOptionAsync(IBuffer, GattWriteOption, op**)`, `WriteClientCharacteristicConfigurationDescriptorAsync(cccdEnum, op<GattCommunicationStatus>**)`, `add/remove_ValueChanged`.
- ValueChanged TypedEventHandler IID `c1f420f6-6292-5760-a2c9-9ddf98683cfc`, `Invoke(This, sender, args)`.
- `CIGattValueChangedEventArgs` (`d21bdb54-06e3-4ed8-a263-acfac8ba7313`): `get_CharacteristicValue(IBuffer**)`, `get_Timestamp`.
- `CIGattReadResult` (`63a66f08-1aea-4c4c-a50f-97bae474b348`): `get_Status`, `get_Value(IBuffer**)`. Reads come back over the command/response GATT chars (see §6), so this is mainly for completeness.
- enums: `GattClientCharacteristicConfigurationDescriptorValue_Notify = 1`, `GattCommunicationStatus_Success = 0`, `GattWriteOption_WriteWithResponse = 0`, `GattWriteOption_WriteWithoutResponse = 1`.
- IAsyncOperation result PIIDs (literal in headers if ever needed, but unused given §3a): GattDeviceServicesResult `e7c667f6-e874-500f-86ff-760ca6f07a58`, GattCharacteristicsResult `0972194a-ac1c-5536-9886-27e58a18f273`, GattCommunicationStatus `3ff69516-1bfb-52e9-9ee6-e5cdb78e1683`, GattReadResult `d40432a8-1e14-51d0-b49b-ae2ce1aa05e5`, GattWriteResult `e83b4534-bd14-5a9b-a53b-17cc02a2a8a8`.

### Storage.Streams (`windows.storage.streams.h`) plus robuffer
- `CIBuffer`: `get_Length`, `get_Capacity`, `put_Length`.
- `CIDataWriter`: `WriteBytes(UINT32, BYTE*)`, `DetachBuffer(IBuffer**)`. RoActivateInstance(`RuntimeClass_..._DataWriter`). IDataWriter IID: grep `IDataWriter :`/MIDL at implementation.
- `IBufferByteAccess` (`905a0fef-bc53-11df-8c49-001e4fc686da`): `Buffer(byte**)`, hand-declared per §3b.

## 5. BLE flow (the driver lifecycle)

`Init`: hint gate, then `WIN_RoInitialize` plus resolve combase entrypoints (`CoIncrementMTAUsage`, `RoGetActivationFactory`, `RoActivateInstance`, `WindowsCreateStringReference`, `WindowsDeleteString`, `WindowsGetStringRawBuffer`), then pin MTA, then start the advertisement watcher on a worker thread. Return true even if disabled.

Discovery (`Received` handler): walk `ManufacturerData`, match `CompanyId == 0x0553` (Nintendo BLE company id, NOT the USB VID). Payload (company id stripped): `vendor=data[3:5]` (==0x057E), `product=data[5:7]` LE (Pro `0x2069`, JC2-R `0x2066`, JC2-L `0x2067`, GC `0x2073`), `reconnect_mac=data[10:16]`. `e.BluetoothAddress` is the connect handle. Cooperate via `SDL_ShouldIgnoreJoystick` plus `SDL_JoystickHandledByAnotherDriver(&SDL_BLE_JoystickDriver,...)`, take `SDL_LockJoysticks`, bail if quitting.

Connect (NO SMP / NO OS pairing): `BluetoothLEDeviceStatics.FromBluetoothAddressAsync(addr)` then await then `IBluetoothLEDevice`. Do NOT `PairAsync` (SMP terminates the link). Best-effort `RequestPreferredConnectionParameters(ThroughputOptimized)` on `IBluetoothLEDevice6` (try/catch, roughly 15ms/66Hz vs default 60ms).

GATT subscribe (order matters): `GetGattServicesForUuidAsync(service)`, then `GetCharacteristicsForUuidAsync` per char, then register `ValueChanged` on command-response `c765a961-…` THEN input `…fd2`, THEN write each CCCD to `Notify`. Handler before CCCD.

`Open`: set counts (§6), add gyro and accel sensors, `connection_state = WIRELESS`. `Update`: drain ring buffer, decode latest, emit. `Rumble`: stash amplitudes for the next vibration write. `Close`/`Quit`: Release every interface, stop watcher, RoUninitialize.

GUID for free mapping: `SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_PRO, version, NULL, name, 'h', type)`. Signature `'h'` makes `SDL_CreateMappingForHIDAPIGamepad` fabricate the existing Switch2-Pro mapping (`SDL_gamepad.c:1135`). Emit buttons/axes/hat in the indices `HandleSwitchProState` uses (`SDL_hidapi_switch2.c:1139-1224`), reusing the `SDL_GAMEPAD_BUTTON_SWITCH2_PRO_*` enums for parity. Zero gamepad-DB edits.

## 6. Per-type characteristics, decode, output

Service `ab7de9be-89fe-49ad-828f-118f09df7fd0`. Input report 0x05 NOTIFY `…fd2` (handle 0x000A). Command write `649d4ac9-…f005` (0x0014). Command response NOTIFY `c765a961-…836a` (0x001A). Vibration write (per type, handle 0x0012): Pro `cc483f51-…2b05`, JC2-L `289326cb-…8241`, JC2-R `fa19b0fb-…c149`, GC `3f8fb670-…d064`.

Input decode (BLE reports omit the leading report-ID, offsets per ndeadly/Nadeflore/joycon2cpp, confirm on hardware): `[4:8]` buttons u32 LE, `[10:13]` left stick 12-bit packed (`v=u24LE`, then `x=v&0xFFF`, `y=v>>12`), `[13:16]` right stick, `[48:54]` accel s16x3, `[54:60]` gyro s16x3, `[0x1F:0x21]` battery mV, `[0x29]==0x01` marker. Button bits: `Y=0x1 X=0x2 B=0x4 A=0x8 R=0x40 ZR=0x80 Minus=0x100 Plus=0x200 RStick=0x400 LStick=0x800 Home=0x1000 Capture=0x2000 C=0x4000 Down=0x10000 Up=0x20000 Right=0x40000 Left=0x80000 L=0x400000 ZL=0x800000`.

Sticks reuse the wired driver's `ParseStickCalibration` (`SDL_hidapi_switch2.c:157`) plus `MapJoystickAxis` (:295), both transport-independent byte math. Calib read over command channel: user (L `0x1FC042`, R `0x1FC062`). If first 3 bytes are `0xFFFFFF`, fall back to factory (L `0x0130A8`, R `0x0130E8`).

IMU: add sensors at 250 Hz. Scale reuses wired IMU decode (:1343-1436) but verify the BLE raw-to-rad/s factor on hardware (`gyro_coeff=34.8f` may differ).

Command framing: `[cmd] 0x91 0x01 [subcmd] 0x00 [len] 0x00 0x00 [data…]` to `…f005`, response on `c765a961` at byte 8. Read-memory cmd `0x02/0x04`: data `[len] 7E 00 00 [addr u32 LE]` (max 0x4F). Player LED `0x09/0x07`: `[bitmask]` at least 4 bytes. Rumble (vibration char): packet-id byte plus 3x 5-byte VibrationData. Pro writes the group twice (L then R), packet_id high nibble `0x50|counter` wraps at 0x0F. VibrationData LE pack: `(lf_freq&0x1FF)|(en_lf<<9)|((lf_amp&0x3FF)<<10)|((hf_freq&0x1FF)<<20)|(en_hf<<29)|((hf_amp&0x3FF)<<30)`. SDL `Rumble(low,high)` maps low to lf_amp, high to hf_amp (scale 0-65535 to the 10-bit field).

Optional persistence pairing (command `0x15`, NOT SMP) is for auto-reconnect only, out of M1 scope.

## 7. Milestones

- **M1 (Pro Controller):** watcher, connect (no SMP), ThroughputOptimized, calib read, subscribe `…fd2`, decode buttons/sticks/gyro, surface via `'h'`+Switch2-Pro GUID (mapping fires free), reconnect. No rumble.
- **M2:** rumble (vibration char), player LED, optional `0x15` persistence ceremony.
- **M3:** Joy-Con 2 (L/R single plus merged), NSO GameCube (analog triggers `[0x3C]/[0x3D]`).
- **Out of scope:** emulating a controller to a console, the 200 Hz vendor-HCI interval, headset audio chars.

## 8. Hardware-gated unknowns (cannot verify without the controller)

1. BLE report byte offsets (§6). Confirm against one real `…fd2` capture. Wired offsets differ.
2. Exact `__x_ABI_C…` symbol spellings compiling clean. The build is the gate. Some contract-version-gated interfaces may need the version bump.
3. IMU raw-to-rad/s scale for BLE.
4. Whether any Joy-Con 2 form factor needs the OS bond vs GATT-only. Prefer GATT-only, never trigger SMP.
5. OS-version gates: ThroughputOptimized/`IBluetoothLEDevice6` need build 17763 or newer, `AllowExtendedAdvertisements` needs 1903 or newer. Degrade gracefully.

The compile is the only verification available in the SDL-fork environment. Everything in §5 and §6 is wire-format reasoning from the reference reimplementations until PadForge deploy-tests against hardware.
