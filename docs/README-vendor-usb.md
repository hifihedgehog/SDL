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
| Gametrak, Windows only | 14B7:0982 | 0 | the device | `SDL_HINT_JOYSTICK_HIDAPI_GAMETRAK` |
| DJI RC (RM330) | 2CA3:1023 | class 0xFF, subclass 0x43, any protocol | `USB\VID_2CA3&PID_1023&MI_01`, the DUML bulk interface | `SDL_HINT_JOYSTICK_HIDAPI_DJI_REMOTE` |
| I-Force wheels and joysticks, Windows only | the 14 IDs in [README-iforce.md](README-iforce.md) | 0, any class | the device | `SDL_HINT_JOYSTICK_HIDAPI_IFORCE` |
| Namco GunCon 2 and EMS LCD TopGun | 0B9A:016A | 0, any class | the device | `SDL_HINT_JOYSTICK_HIDAPI_GUNCON` |
| Train controllers | 0AE4:0004, 0005, 0007, 0101 and 1C06:77A7 | 0, any class | the device | `SDL_HINT_JOYSTICK_HIDAPI_TRAIN` |
| Namco USIO, Windows only | 0B9A:0910, 0B9A:0900 | 0, any class | the device | `SDL_HINT_JOYSTICK_HIDAPI_USIO`, `SDL_HINT_JOYSTICK_HIDAPI_USIO_LAYOUT` |
| Konami P3IO, Windows only | 1CCF:8008 | 0, IN endpoint 0x83 | the device, or on a composite device the interface children that hold 0x83, 0x02 and 0x81 | `SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P3IO` |
| Konami P4IO, Windows only | 1CCF:8010 | 0, the first interrupt IN endpoint | the device, or `USB\VID_1CCF&PID_8010&MI_00` if Windows lists it as composite | `SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO`, `SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO_LAYOUT` |
| CH Products Multi-Function Panel, Windows only | 068E:00F0 | 0, class 0xFF | the device | `SDL_HINT_JOYSTICK_HIDAPI_CHMFP` |
| Ergodex DX1, Windows only | 1603:0002 | 1, class 0xFF | `USB\VID_1603&PID_0002&MI_01`, the vendor interface | `SDL_HINT_JOYSTICK_HIDAPI_ERGODEX` |
| NaturalPoint TrackIR 2 and TrackIR 3, Windows only | 131D:0150, 131D:0155 | 0, any class | the device | `SDL_HINT_JOYSTICK_HIDAPI_TRACKIR` |
| Tacx T1904 and T1932 head units, Windows only | 3561:1904 and 1932 | 0, any class | the device | `SDL_HINT_JOYSTICK_HIDAPI_TACX` |

## How the path works

- A rule names a vendor and product, then the interface by number or by class,
  subclass and protocol. It can name the alternate setting to select on open
  and the endpoints and packet sizes to use.
- Other interfaces of a device with a rule are not enumerated. The Intel base
  station's boot mouse on interface 1 and the Ergodex DX1's keyboard on
  interface 0 stay with Windows.
- Endpoints are bulk or interrupt, as the descriptor says. Control and
  isochronous endpoints are never used. The alternate setting goes back to 0
  when the device closes.
- Xbox 360 and Xbox One interfaces on Windows are enumerated only when libusb
  can open them. A pad that xusb22 or the GIP driver holds cannot be opened and
  does not appear twice. A pad bound to WinUSB is read by SDL's Xbox drivers.
- Original Xbox XID interfaces, class 0x58, follow the same rule. Windows has
  no driver for them, so one appears only once WinUSB is bound. See
  [README-xid.md](README-xid.md).
- The Switch 2 controllers keep their input on the platform HID backend. Their
  drivers open WinUSB separately for bulk I/O.
- A rule can serve Windows only. The Gametrak's HID interface needs libusb
  on Windows alone, where hid.dll refuses its unlock writes. It has no OUT
  endpoint, so each write goes out as SET_REPORT with its first byte in
  wValue. On Linux and macOS the platform backend keeps it. See
  [README-hid-devices.md](README-hid-devices.md).
- A class rule can ignore the protocol. The DJI RC's DUML interface is found
  by class and subclass alone, as dji-firmware-tools and DJI-RC-Emulator find
  it, and its bulk writes go out unchanged. See
  [README-dji-remotes.md](README-dji-remotes.md).
- A rule by interface number matches whatever the class. The I-Force rules
  name interface 0 alone, since Linux's `iforce` driver takes those IDs by
  vendor and product, and their commands go out unchanged. They serve
  Windows only. Where a rule does not serve the platform, libusb treats the
  device as if it had no rule, so on Linux the kernel's driver keeps it. See
  [README-iforce.md](README-iforce.md).
- The GunCon 2 has no OUT endpoint, so its mode request goes out as
  SET_REPORT on the control pipe, as the Gametrak's writes do. No platform
  has a driver for it, so its rule serves every platform. See
  [README-guncon.md](README-guncon.md).
- The train controllers send their outputs as vendor control transfers on
  the handle the libusb backend holds. The Taito units declare HID class
  with no HID class descriptor, and SDL asks them for no report descriptor.
  See [README-train.md](README-train.md).
- A rule with `SDL_VENDORUSB_IN_INTERRUPT` reads its input from an interrupt
  IN endpoint and passes over bulk IN endpoints, for a device whose input
  endpoint follows a bulk IN endpoint that carries command replies.
- A rule's `read_size` sets the bytes read per transfer on a bulk IN
  endpoint, for a device that sends several packets in one transfer. A read
  then returns what the device sent up to its next short packet. The size
  must be a whole number of the endpoint's packets, or the interface does
  not open. An interrupt IN endpoint ignores it and is read one
  `wMaxPacketSize` at a time, as before.
- The rules of the arcade boards and the specialty devices serve Windows
  only, the platform they were added for, and none of these devices is on
  the list of devices that need libusb on every platform. On Linux and macOS
  SDL leaves them to tools such as linuxtrack, chmfp, ergodex-dx1-linux,
  p4io-mdxfdrv and FortiusANT. See [README-arcade-io.md](README-arcade-io.md)
  and [README-specialty-usb.md](README-specialty-usb.md).
- The P3IO's rule names its IN endpoint 0x83, since bulk IN 0x81 on the same
  board carries the replies to its commands. The driver runs those commands
  on bulk OUT 0x02 and bulk IN 0x81 itself, from a thread of its own, on the
  handle the libusb backend holds. It claims the interface that holds them
  when that is not interface 0.
- The P4IO's rule takes the first interrupt IN endpoint with
  `SDL_VENDORUSB_IN_INTERRUPT`, since no source records the board's
  addresses and a bulk IN endpoint for command replies comes first. The
  driver sends INIT and GET DEVICE INFO once, from a thread of its own,
  through libusb bulk transfers on the backend's handle, and only logs the
  answer, since the input reports need no command.
- The TrackIR 2 and 3 rules read 16384 bytes per bulk transfer, linuxtrack's
  read size, so one read holds every packet the camera sent up to its next
  short packet. The driver reads with a buffer that size, since a read
  hands back no more than the buffer it is given. A read that fills the
  whole transfer can end inside a packet, and the next read finishes it.
- The Tacx rules read a bulk IN endpoint 64 bytes per transfer, as every
  Tacx source reads it, so a reply spread over smaller packets arrives
  whole. The head units ask their brake only after a frame from the host, so
  the driver writes the version request and then a stop frame every 100 ms
  from its updates, and never sets a resistance.

## Known limits

- While WinUSB owns the Intel base station's interface 0, the Intel wireless
  keyboard on that base station stops working as a Windows keyboard.
- The Intel base station sends no message when a pad powers off. The pad stays
  connected until its slot is reassigned or the base station is unplugged.
- The Big Button receiver sends packets only while a button is held. A pad
  releases its controls 120 ms after its last packet.
- A Tacx reply of 24 bytes, which FortiusANT also sees at times and
  ignores, holds the head unit's buttons and steering but no brake answer.
  The driver decodes only replies of 48 bytes or more, and a head unit whose
  brake does not answer sends none, so it shows no joystick.

## Adding a device

1. Add its ID to `usb_ids.h` and its rule to `SDL_vendorusb_rules`. A device
   that needs libusb on every platform also goes in
   `SDL_vendorusb_libusb_required`.
2. Put the protocol in a pure module with no SDL runtime and no I/O, and keep
   the driver file to moving bytes between that module and SDL.
3. Add a replay test under `test/controller-protocols` and run it in the
   normal and AddressSanitizer builds. Feed it every truncation of every packet
   with stale bytes past the received length, and drive its timing from an
   injected clock.
