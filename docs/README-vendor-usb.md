# Vendor USB devices on Windows

Some controllers carry their input on a USB interface that no Windows driver
serves. This fork reads them through libusb once WinUSB is bound to that
interface. The application or its installer does the binding. SDL loads no
driver and no firmware.

The rules live in `src/hidapi/SDL_hidapi_vendorusb.c`. On Windows a device with
a rule is left to the libusb backend, and the platform HID backend skips it.

| Device | ID | Interface | Bind WinUSB to | Hint |
|---|---|---|---|---|
| Wii U GameCube adapter | 057E:0337 | 0 | the device | `SDL_HINT_HIDAPI_LIBUSB_GAMECUBE`, `SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE` |
| Intel Wireless Series base station | 8086:C013 | 0, alternate setting 1 | `USB\VID_8086&PID_C013&MI_00` if Windows lists the device as composite, otherwise the device | `SDL_HINT_JOYSTICK_HIDAPI_INTEL_WIRELESS` |
| Xbox 360 Big Button receiver | 045E:02A0 | class 0xFF, subclass 0x5D, protocol 4 | the device or the interface child that carries that interface | `SDL_HINT_JOYSTICK_HIDAPI_XBOX_360` |

## How the path works

- A rule names a vendor and product, then the interface by number or by class,
  subclass and protocol. It can name the alternate setting to select on open
  and the endpoints and packet sizes to use.
- Other interfaces of a device with a rule are not enumerated. The Intel base
  station's boot mouse on interface 1 stays with Windows.
- Endpoints are bulk or interrupt, as the descriptor says. Control and
  isochronous endpoints are never used. The alternate setting goes back to 0
  when the device closes.
- Xbox 360 and Xbox One interfaces on Windows are enumerated only when libusb
  can open them. A pad that xusb22 or the GIP driver holds cannot be opened and
  does not appear twice. A pad bound to WinUSB is read by SDL's Xbox drivers.
- The Switch 2 controllers keep their input on the platform HID backend. Their
  drivers open WinUSB separately for bulk I/O.

## Known limits

- While WinUSB owns the Intel base station's interface 0, the Intel wireless
  keyboard on that base station stops working as a Windows keyboard.
- The Intel base station sends no message when a pad powers off. The pad stays
  connected until its slot is reassigned or the base station is unplugged.
- The Big Button receiver sends packets only while a button is held. A pad
  releases its controls 120 ms after its last packet.

## Adding a device

1. Add its ID to `usb_ids.h`, its rule to `SDL_vendorusb_rules`, and its ID to
   `SDL_vendorusb_libusb_required`.
2. Put the protocol in a pure module with no SDL runtime and no I/O, and keep
   the driver file to moving bytes between that module and SDL.
3. Add a replay test under `test/controller-protocols` and run it in the
   normal and AddressSanitizer builds. Feed it every truncation of every packet
   with stale bytes past the received length, and drive its timing from an
   injected clock.
