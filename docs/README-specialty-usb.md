# Specialty USB devices

These USB devices give Windows no game input without their makers' drivers,
which are gone or were replaced. This fork reads them through libusb once
WinUSB is bound, on Windows only. SDL loads no driver and no firmware.

| Device | ID | Bind WinUSB to | Hint | SDL device |
|---|---|---|---|---|
| CH Products Multi-Function Panel | 068E:00F0 | The device | `SDL_HINT_JOYSTICK_HIDAPI_CHMFP` | 102 buttons |
| Ergodex DX1 | 1603:0002 | `USB\VID_1603&PID_0002&MI_01`, interface 1 | `SDL_HINT_JOYSTICK_HIDAPI_ERGODEX` | 52 buttons |
| NaturalPoint TrackIR 2, TrackIR 3 | 131D:0150, 131D:0155 | The device | `SDL_HINT_JOYSTICK_HIDAPI_TRACKIR` | 3 axes, 1 button |
| Tacx T1904 and T1932 trainer head units | 3561:1904, 3561:1932 | The device | `SDL_HINT_JOYSTICK_HIDAPI_TACX` | 5 axes, 4 buttons |

Each hint defaults to `SDL_HINT_JOYSTICK_HIDAPI`. Each device is one joystick
of type `SDL_JOYSTICK_TYPE_UNKNOWN`, with no gamepad mapping and no player
index. On Linux and macOS SDL leaves these devices to tools such as chmfp,
ergodex-dx1-linux, linuxtrack and FortiusANT. See
[README-vendor-usb.md](README-vendor-usb.md) for the vendor USB path.

## CH Products Multi-Function Panel

The Multi-Function Panel takes up to 50 keys placed anywhere on its surface.
On Windows it works only through CH Control Manager, whose driver presents
it as a virtual keyboard and mouse. Binding WinUSB takes the panel from that
driver. Its only interface is vendor class, with one 8-byte interrupt IN
endpoint, 0x81, and the host never writes to it.

| Item | Value |
|---|---|
| ID | 068E:00F0 |
| Bind WinUSB to | `USB\VID_068E&PID_00F0`, the device, which has one interface |
| Hint | `SDL_HINT_JOYSTICK_HIDAPI_CHMFP` |
| Name | CH Products Multi-Function Panel |

SDL reads interface 0 when its class is 0xFF, and the joystick appears when
the panel connects. SDL sends the panel nothing.

| SDL | Control |
|---|---|
| Buttons 0 to 49 | Keys 1 to 50 in green mode |
| Buttons 50 to 99 | Keys 1 to 50 in red mode, while the panel's LED is red |
| Button 100 | Green button |
| Button 101 | Red button |

A report is 8 bytes. Key k is bit (k - 1) mod 8 of byte (k - 1) div 8, and
byte 7 holds the green button in bit 0, the red button in bit 1 and red mode
in bit 2. SDL sets both banks from each report, so a mode change releases a
held key in one bank and presses it in the other. As chmfp does, SDL drops a
read that is not 8 bytes, a report with any of byte 7's bits 3 to 7 set, and
a report that changes more than 6 of the 102 buttons from the last one it
accepted, the chord limit CH gives for the panel. A mode change counts each
held key twice. The panel's LED shows its mode, and the host cannot drive
it.

## Ergodex DX1

The DX1 is a pad of keys that sit anywhere on its surface. It is a composite
device: interface 0 is a boot keyboard that Windows binds, and interface 1 is
vendor class. The keys type nothing until software programs them over the
vendor interface, the pad forgets its programming at power-off, and its own
software ended with Windows XP.

| Item | Value |
|---|---|
| ID | 1603:0002, "DX1 Pad" |
| Bind WinUSB to | `USB\VID_1603&PID_0002&MI_01`, interface 1 only. Interface 0, the keyboard, stays with Windows. |
| Hint | `SDL_HINT_JOYSTICK_HIDAPI_ERGODEX` |
| Name | Ergodex DX1 |

SDL reads interface 1 when its class is 0xFF: interrupt IN 0x82 and
interrupt OUT 0x02, 16 bytes a transfer each way. The joystick appears when
the pad connects. Since the pad forgets its keys, every connection runs the
start-up again, one 16-byte packet per update, each at least 10 ms after the
last write, since the pad stops answering without the pause:

1. DEVICE, `0A` and 15 bytes of 00. The reply's serial becomes the device's
   serial, 16 hex digits, since the USB descriptor has none.
2. DISABLE, `02 00 00 01 00 01 01 01` and 8 bytes of 00.
3. Ten PROGRAM packets that make keys 1 to 50 macro keys, type 3, five keys
   a packet: `03 01 03 00 02 03 00 03 03 00 04 03 00 05 03 00` for keys 1 to
   5, and so on.
4. ENABLE in test mode, `02 00 01 01 00 00 01 01` and 8 bytes of 00.

A write that fails goes out again 50 ms later, and a second failure of the
same packet ends the start-up. The two sources that drive the pad disagree on
which packets report keys. On one pad, keys programmed as macro keys reported
on the vendor interface in normal mode. On another, no key reported outside
test mode. The start-up covers both, and SDL decodes the two kinds of key
packet alike.

| SDL | Control |
|---|---|
| Buttons 0 to 49 | Key IDs 1 to 50 |
| Button 50 | Top ("power") button |
| Button 51 | Lower ("record") button |

A key ID belongs to the key puck and stays the same wherever the puck sits. A
macro packet, type 02, or a test packet, type 03, lists up to six held key
IDs, and a key it leaves out is released. IDs 00 and above 50 hold nothing. A
STATUS packet, type 01, carries the two pad buttons in byte 4, active low.
SDL ignores a packet that is not 16 bytes or has another type.
`SDL_SetJoystickLED` is unsupported, since the two sources disagree on what
the LED bits light.

## NaturalPoint TrackIR 2 and TrackIR 3

The TrackIR 2 and 3 are USB infrared cameras that send the stripes of bright
pixels they see.

| Item | Value |
|---|---|
| ID | 131D:0150 TrackIR 2, 131D:0155 TrackIR 3 |
| Bind WinUSB to | The device |
| Hint | `SDL_HINT_JOYSTICK_HIDAPI_TRACKIR` |
| Names | NaturalPoint TrackIR 2, NaturalPoint TrackIR 3 |
| Sensor | 256 x 256 pixels on the TrackIR 2, 440 x 314 on the TrackIR 3 |

Binding WinUSB to a camera takes it away from NaturalPoint's driver.

Interface 0 takes commands on bulk OUT 0x02 and streams packets on bulk IN
0x82, which SDL reads 16384 bytes at a time, as linuxtrack reads it. The
joystick appears when the camera connects, and SDL starts the camera from its
updates with linuxtrack's commands and delays. It turns the lights and the
video off, flushes the stream with three reads, and sends the device
information request `17 01` until a read starts `09 40`, 20 tries at most,
after which it goes on without an answer. Then it sets the threshold and
turns the video and the infrared lights on. The TrackIR 3 also gets three
commands that linuxtrack names unknown, sent as linuxtrack sends them. A
light command is `10`, the lights to turn on, then the lights it sets: 10
red, 20 green, 40 blue, 80 infrared. The red and green status lights stay
off. The threshold ends at 140, `15 8C 01` on the TrackIR 2 and
`15 8C 01 00` on the TrackIR 3.

When SDL lets the camera go, it sends linuxtrack's stop sequence, which turns
the infrared lights off, and waits out its delays, 401 ms on the TrackIR 2
and 109 ms on the TrackIR 3. It skips the stop sequence when the camera is
gone, or when the start-up failed or never began.

| SDL | Control |
|---|---|
| Axis 0 | ((width - 1) / 2 - x) x 65535 / width, where x is the mean column of the dot's pixels |
| Axis 1 | The same with the mean line of the dot's pixels and the height |
| Axis 2 | The dot's size in pixels, up to 32767 |
| Button 0 | Held while a dot is in view |

A read holds one or more packets, each starting with its size and a type: 1C
carries stripes, 20 status and 40 device information. SDL skips status and
information packets, and an unknown type drops the rest of the read. A read
that fills all 16384 bytes can end inside a packet, and the next read
finishes it. A frame ends at an all-zero stripe or at a line lower than the
one before. A stripe joins every blob that has a stripe it overlaps or
touches, diagonally too, on the line before. A stripe outside the sensor, or
one that ends before it starts, is ignored. The dot is the largest blob of
the frame, the later one on a tie. The axes keep their last values while no
dot is in view. Recentering and scaling into yaw and pitch, which
linuxtrack's pose code does, are left to the application.

## Tacx trainer head units

The T1904 and T1932 head units connect a Tacx trainer's brake to the
PC. On Windows they need Tacx's Jungo drivers or a libusb-win32 replacement.

| Item | Value |
|---|---|
| ID | 3561:1904, the white and green i-Magic head unit. 3561:1932, the white and blue head unit of the Flow, Fortius and VR trainers. |
| Bind WinUSB to | The device, `USB\VID_3561&PID_1904` or `USB\VID_3561&PID_1932` |
| Hint | `SDL_HINT_JOYSTICK_HIDAPI_TACX` |
| Names | Tacx T1904, Tacx T1932 |

WinUSB replaces Tacx's Jungo driver, so software that uses the Jungo driver
cannot reach the head unit until that driver is bound again.

Interface 0 takes frames on OUT 0x02 and answers on IN 0x82. The head unit
asks the brake only after a frame from the host, so SDL writes from its
updates, whether or not a joystick is open: the version request
`02 00 00 00` first, without which the head unit reports no cadence, then a
stop frame every 100 ms, `01 08 01 00` and eight bytes of 00, mode 00 with
target 0. Until the brake's version reply arrives, the version request takes
a frame's place every 500 ms, six times at most, and a head unit that starts
answering after that gets a new round. SDL never sets a resistance.
`SDL_SendJoystickEffect` is unsupported, and resistance control belongs to
the application's trainer code.

A reply is 64 bytes. Its header is bytes 24 to 27, a little-endian word. SDL
decodes a reply of 48 bytes or more whose header is 00021303 as data, the
length of the T1942's replies, which share the protocol. The joystick
appears with the first data reply and goes 1000 ms after the last one, and a
new round of version requests begins then. The brake's version reply, header
00000C03, is logged at debug level with the brake's firmware version and
serial, and for a motor brake its model, year and unit number, which the
serial encodes. A magnetic brake reports serial 0, and its line ends
`serial 0, a magnetic brake`. The head unit reports no power, and SDL
computes none.

| SDL | Control |
|---|---|
| Axis 0 | Steering, the head unit's axis 1 in reply bytes 18 and 19, minus 32768 |
| Axis 1 | Wheel speed as the head unit reports it, up to 32767, about 113 km/h by FortiusANT's factor |
| Axis 2 | Cadence in rpm, x 128 |
| Axis 3 | Heart rate in bpm, x 128 |
| Axis 4 | Current resistance, signed, as the head unit reports it |
| Buttons 0 to 3 | Enter, Down, Up, Cancel |

## Devices SDL does not read

- The Novint Falcon, 0403:CB48, answers only after the host loads Novint's
  firmware, and Novint's license forbids changing or emulating its SDK's
  communication protocol, so SDL does not read it.
- SDL does not read the Tacx T1902, 3561:1902, which needs a Tacx firmware
  image from the host at every power-up.
- SDL does not read 0547:2131, a T1902 before its firmware is loaded, since
  only a Tacx firmware image from the host brings it up.
- SDL does not read the T1942, 3561:E6BE before its firmware is loaded and
  3561:1942 after, since the host must load a Tacx firmware image into it
  at every power-up.

## Known limits

- The panel's report layout comes from chmfp, whose author built it by
  listening to the USB traffic of a green-logo panel. Whether white-logo
  panels send the same report, and what the 21-byte report descriptor that
  the interface names holds, are not recorded.
- The DX1's two sources disagree on which packets report keys, and no source
  tried macro keys and test mode together. A packet lists at most six held
  keys.
- No capture of TrackIR 2 or 3 traffic exists. Every decode follows
  linuxtrack's code. The meaning of `23 40 1C 5E 00 00`, `23 40 1D 01 00 00`
  and `19 03 03 00 00` is not recorded.
- When the TrackIR's `17 01` request cannot be written, the start-up stops,
  as linuxtrack stops, and the joystick does not move until the camera
  connects again.
- A Tacx head unit whose brake does not answer sends no data reply, so it
  shows no joystick. The 24-byte replies that FortiusANT sees at times hold
  the head unit's own fields, the buttons and the steering among them, but
  no brake answer, and SDL ignores them as FortiusANT does.
- FortiusANT reads km/h as the wheel speed / 289.75, and antifier as the
  wheel speed / 280.54. SDL reports the raw value.
- No Tacx capture shows a button press, so the buttons follow FortiusANT's
  code. The T1932 capture's steering reads 0A0D, which FortiusANT treats as
  no steering unit, and the T1942 capture's reads near 03D0. Whether a
  steering unit was attached to that T1942 is not recorded.
- No source records the Tacx endpoint types. The transfer type comes from
  the descriptor, and a bulk IN endpoint is read 64 bytes a transfer, so one
  whose packet size does not divide 64, such as a high-speed one of 512
  bytes, does not open.

## Tests

The protocol modules are C99 with no SDL runtime and no I/O.
`test/controller-protocols` runs `testchmfp` against chmfp's byte table and
constructed reports, `testergodex` against the packets dx1-studio and
ergodex-dx1-linux captured from hardware and the start-up on an injected
clock, `testtrackir` against streams constructed from linuxtrack's parser and
blob code, and `testtacx` against antifier's T1942 and T1932 captures,
replayed whole, in normal and AddressSanitizer builds. Each of the four also
checks its device's vendor rule.
