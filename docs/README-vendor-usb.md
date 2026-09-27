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
| DJI RC (RM330), Windows only | 2CA3:1023 | class 0xFF, subclass 0x43, any protocol | `USB\VID_2CA3&PID_1023&MI_01`, the DUML bulk interface | `SDL_HINT_JOYSTICK_HIDAPI_DJI_REMOTE` |
| I-Force wheels and joysticks, Windows only | the 14 IDs in [README-iforce.md](README-iforce.md) | 0, any class | the device | `SDL_HINT_JOYSTICK_HIDAPI_IFORCE` |
| Namco GunCon 2, and an EMS LCD TopGun whose interface has the GunCon 2's class 0xFF | 0B9A:016A | 0, any class. The driver reads class 0xFF only | the device | `SDL_HINT_JOYSTICK_HIDAPI_GUNCON` |
| Train controllers | 0AE4:0004, 0005, 0007, 0101 and 1C06:77A7 | 0, any class | the device | `SDL_HINT_JOYSTICK_HIDAPI_TRAIN` |
| Namco USIO, Windows only | 0B9A:0910, 0B9A:0900 | 0, any class | the device | `SDL_HINT_JOYSTICK_HIDAPI_USIO`, `SDL_HINT_JOYSTICK_HIDAPI_USIO_LAYOUT` |
| Konami P3IO, Windows only | 1CCF:8008 | 0, IN endpoint 0x83 | the device, or on a composite device the interface children that hold 0x83, 0x02 and 0x81 | `SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P3IO` |
| Konami P4IO, Windows only | 1CCF:8010 | 0, the first interrupt IN endpoint | the device, or `USB\VID_1CCF&PID_8010&MI_00` if Windows lists it as composite | `SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO`, `SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO_LAYOUT` |
| CH Products Multi-Function Panel, Windows only | 068E:00F0 | 0, any class, IN endpoint 0x81 of 8 bytes. The driver reads class 0xFF only | the device | `SDL_HINT_JOYSTICK_HIDAPI_CHMFP` |
| Ergodex DX1, Windows only | 1603:0002 | 1, any class, IN endpoint 0x82 and OUT endpoint 0x02 of 16 bytes each. The driver reads class 0xFF only | `USB\VID_1603&PID_0002&MI_01`, the vendor interface | `SDL_HINT_JOYSTICK_HIDAPI_ERGODEX` |
| NaturalPoint TrackIR 2 and TrackIR 3, Windows only | 131D:0150, 131D:0155 | 0, any class | the device | `SDL_HINT_JOYSTICK_HIDAPI_TRACKIR` |
| Tacx T1904 and T1932 head units, Windows only | 3561:1904 and 1932 | 0, any class | the device | `SDL_HINT_JOYSTICK_HIDAPI_TACX` |
| Creative Prodikeys PC-MIDI, Windows only | 041E:2801 | 1, IN endpoint 0x82, OUT endpoint 0x03 | `USB\VID_041E&PID_2801&MI_01`, the music keys | `SDL_HINT_JOYSTICK_HIDAPI_PRODIKEYS` |

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
  can open their device, or when this process already holds the device open
  on a handle that still reads. A pad that xusb22 or the GIP driver holds
  cannot be opened and does not appear twice. A composite device with a HID
  interface opens anyway: its Xbox interface is listed, fails to claim and
  gets no driver, and XInput keeps the pad. A pad bound to WinUSB is read by
  SDL's Xbox drivers.
- Original Xbox XID interfaces, class 0x58, follow the same rule. Windows has
  no driver for them, so one appears only once WinUSB is bound. See
  [README-xid.md](README-xid.md).
- On Windows a device SDL reads through libusb on an interface other than a
  HID interface is on WinUSB, libusbK or libusb0, which XInput, RawInput,
  Windows.Gaming.Input, DirectInput and GameInput never read. SDL does not
  take such a device for a pad those backends list, so a pad or receiver
  bound to WinUSB leaves the pads on xusb22 and the GIP driver listed.
- An open that fails is tried again at the next device change, so a WinUSB
  binding made while SDL runs takes effect once Windows reports it. The
  Intel base station's interface 0 fails that way until the binding, since
  Windows' keyboard driver holds it.
- The Xbox 360 wired pad, 045E:028E, and wireless receiver, 045E:0719, stay
  with xusb22 until WinUSB is bound to the whole device, which takes the pad,
  or every slot of the receiver, away from XInput. SDL's Xbox 360 drivers then
  read them with their chatpads and the Xbox 360 uDraw GameTablet. See
  [README-xbox360-accessories.md](README-xbox360-accessories.md).
- WinUSB lets one handle open a device, so every interface SDL opens on a
  device goes through the handle of the first one it opened. The four pad
  slots of an Xbox 360 receiver bound to WinUSB open this way. While SDL
  holds a device on a handle that still reads, a later enumeration still
  lists the device's Xbox interfaces, though it cannot open them again. The
  handle, with interface 0 claimed on it, stays open until the last of those
  interfaces closes, and a second open of an interface SDL already holds
  fails. The wired pad's chatpad, interface 2, is claimed on the same handle.
  An enumeration opens each interface it lists for its strings and closes it
  again, and it holds the lock of SDL's open devices from that open to the
  close, so an enumeration on one thread of the process never makes an open
  on another thread fail.
  When a later Xbox interface is the first to open the handle, SDL claims
  interface 0 before it. Otherwise libusb sets up interface 0's WinUSB handle
  without counting the claim, and libusb 1.0.29 sets it up a second time, and
  loses the first, when interface 0 opens afterward.
- The Switch 2 controllers keep their input on the platform HID backend. Their
  drivers open WinUSB separately for bulk I/O.
- The Prodikeys PC-MIDI's rule names interface 1, the music keys, and serves
  Windows only. Prodikeys64, the one Windows reader, found no piano keys
  through Windows' HID stack and reads the interface over WinUSB: interrupt
  IN 0x82, and output report 6 on interrupt OUT 0x03 with its ID byte. The
  platform backend skips the whole keyboard on Windows, as it skips every
  device with a rule. Without WinUSB, libusb reaches a HID interface only
  through its HID layer, which answers with a report descriptor that has no
  report IDs, so the driver takes no Prodikeys until WinUSB is bound. See
  [README-hid-devices.md](README-hid-devices.md).
- A rule can serve Windows only. The Gametrak's HID interface needs libusb
  on Windows alone, where hid.dll refuses its unlock writes. It has no OUT
  endpoint, so each write goes out as SET_REPORT with its first byte in
  wValue. On Linux and macOS the platform backend keeps it. See
  [README-hid-devices.md](README-hid-devices.md).
- A class rule can ignore the protocol. The DJI RC's DUML interface is found
  by class and subclass alone, as dji-firmware-tools and DJI-RC-Emulator find
  it, and its bulk writes go out unchanged. Its bulk IN endpoint is read one
  packet per transfer, 512 bytes at high speed, as DJI-RC-Emulator and
  dji-rc-joystick read it, and the driver's buffer also holds a SuperSpeed
  packet of 1024 bytes. Its rule serves Windows only, the one platform its
  driver builds on. See [README-dji-remotes.md](README-dji-remotes.md).
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
- A rule's `in_timeout` ends each IN transfer that many milliseconds after it
  starts, where the backend otherwise waits 5000 ms, and a transfer that
  times out then hands over the bytes it holds as one read. It serves a
  device whose reply can end on a full packet without filling the transfer,
  which ends no bulk transfer. Only the Tacx rules set it.
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
- The TrackIR 2 and 3 rules read 16384 bytes per transfer when IN 0x82 is
  bulk, linuxtrack's read size, so one read holds every packet the camera
  sent up to its next short packet. The driver reads with a buffer that
  size, since a read hands back no more than the buffer it is given. A read
  that fills the whole transfer can end inside a packet, and the next read
  finishes it.
- The Tacx rules read a bulk IN endpoint 64 bytes per transfer, as every
  Tacx source reads it, and end each transfer 50 ms after it starts, so a
  48-byte reply that ends on a full packet still comes back alone. The head
  units ask their brake only after a frame from the host, so the driver
  hands out the version request and then a stop frame every 100 ms from its
  updates, and the HIDAPI rumble thread writes them. The driver never sets
  a resistance.

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

1. Add its ID to `usb_ids.h` and its rule to `SDL_vendorusb_rules`. A rule
   that serves every platform on an interface a platform HID backend could
   list also needs the device in `SDL_vendorusb_libusb_required`, since
   outside Windows the platform backend skips only the devices on that list.
   A vendor-class interface needs no entry: no HID backend lists it, and its
   rule alone admits it to libusb.
2. Put the protocol in a pure module with no SDL runtime and no I/O, and keep
   the driver file to moving bytes between that module and SDL.
3. Add a replay test, `test/test<name>.c`, register it in
   `test/controller-protocols/CMakeLists.txt`, and run it in the normal and
   AddressSanitizer builds. Feed it every truncation of every packet with
   stale bytes past the received length, and drive its timing from an
   injected clock.
