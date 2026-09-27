# I-Force wheels and joysticks

Immersion's I-Force controller runs the force feedback wheels and joysticks
below. On current Windows only Saitek's R440 driver of 2007 and two community
bridges read them. This fork reads their input over USB and RS-232 and drives
their force feedback through SDL's haptic API. It loads no firmware. The protocol facts come from Linux's `iforce` driver and protocol
document and from the two bridges.

## Devices

| ID | Name | SDL device |
|---|---|---|
| 044F:A01C | Thrustmaster Motor Sport GT | Wheel |
| 046D:C281 | Logitech WingMan Force | Flight stick |
| 046D:C291 | Logitech WingMan Formula Force | Wheel |
| 05EF:020A | AVB Top Shot Pegasus | Flight stick with rudder |
| 05EF:8884 | AVB Mag Turbo Force | Wheel |
| 05EF:8888 | AVB Top Shot Force Feedback Racing Wheel | Wheel |
| 061C:C084, C094, C0A4 | ACT LABS Force RS | Wheel |
| 06A3:FF04 | Saitek R440 Force Wheel | Wheel |
| 06F8:0001 | Guillemot Race Leader Force Feedback | Wheel |
| 06F8:0003 | Guillemot Jet Leader Force Feedback | Flight stick with rudder |
| 06F8:0004 | Guillemot Force Feedback Racing Wheel | Wheel with two hats |
| 06F8:A302 | Guillemot Jet Leader 3D | Flight stick |
| 05EF:8886, serial | Boeder Force Feedback Wheel | Wheel |
| 06D6:29BC, serial | Trust Force Feedback Race Master | Wheel |

`061C:C094` is not in Linux. The forcers INF package binds the Saitek driver
to it. Linux files the Jet Leader Force Feedback under `0001`, which its USB
table gives to the Race Leader, so a real `0003` gets its rudder here and not
in Linux.

## USB

Bind WinUSB to the whole device, `USB\VID_xxxx&PID_yyyy`, as the community
packages bind the Saitek driver. SDL reads interface 0's first IN endpoint
and writes its first OUT endpoint, whatever the interface class. See
[README-vendor-usb.md](README-vendor-usb.md). Setting
`SDL_HINT_JOYSTICK_HIDAPI_IFORCE` to 0 turns the driver off.

Input needs no command. The device's ID picks the layout. A thread asks the
queries Linux asks, `O` until the device answers, 20 times at most, then `M`,
`P`, `B`, `N`, `C`, `E`, `O` and `V`, each a vendor control request with a
1000 ms timeout. A device that never answers `O` keeps its input and has no
force feedback. Opening the joystick waits until `N` has been asked, up to
1000 ms, so that a haptic device opened next knows the effect count and
memory size. An unanswered `O` ends the wait, and `C`, `E`, the second `O`
and `V` never hold it up.

The driver serves only devices that libusb opens through WinUSB. A device
that a platform HID backend opens stays with the other joystick backends, and
so does one that libusb reaches through Windows' HID driver before WinUSB is
bound, since that driver refuses the queries.

## RS-232

Name the port with `iforce` in `SDL_HINT_JOYSTICK_SERIAL`, for example
`COM3=iforce`. The line is 38400 baud, 8N1, with DTR and RTS on. The driver
sends the same queries as frames and waits up to 1 s after each one leaves.
With no answer to `O` after 20 tries it logs `no answer to O after 20 tries`
and opens no joystick. The `M` and `P` replies pick the layout. A serial
device that reports a USB model's IDs gets that model's layout, and any other
gets the joystick layout, named `Unknown I-Force Device [vvvv:pppp]`. Input
that arrives before the answers is dropped, as Linux drops it.

The joystick appears once the queries are over, with their results as
properties: `SDL.joystick.iforce.vendor` and `SDL.joystick.iforce.product`
from `M` and `P`, and, when `N` reported effects,
`SDL.joystick.iforce.effects` and `SDL.joystick.iforce.memory` from `N` and
`B`. Its GUID keeps vendor 0, as for every port the hint names. SDL's haptic
API serves a serial device whose `N` reported effects, with the model its `M`
and `P` replies pick. A device that no table lists gets force feedback too,
as Linux gives it.

`SDL_SendJoystickEffect` takes one whole I-Force command, the command byte and
exactly its data, and the driver frames it. Any other bytes fail the call.
Commands go out in the order sent, and the call fails while 16 wait. A burst
beyond 16, such as six effects created in a row, can fail the call that finds
the queue full, and the haptic driver then undoes that update.

## Controls

Wheels:

| Control | SDL |
|---|---|
| Wheel | Axis 0, `raw * 32767 / 1920` clamped |
| Gas, brake | Axes 1 and 2, `(255 - raw) * 257 - 32768`, released at -32768 |
| Buttons 0-7 | Data byte 5, bits 0-7: gear down, gear up, then base 1 to 6 in Linux's order |
| Hat 0 | High nibble of data byte 6 |
| Grip sensor | Button 8 |

`06F8:0004`, as measured on the wheel: buttons 0-5 are the right paddle, the
left paddle, the right face button, the left face button, gear up and gear
down. Hat 0 is the left hat. Hat 1 is the right hat, from data byte 5 bits 6
and 7 and data byte 6 bits 0 and 1. The grip sensor is button 6.

Joysticks:

| Control | SDL |
|---|---|
| X, Y | Axes 0 and 1, scaled like the wheel |
| Throttle | Axis 2, released at -32768 |
| Rudder | Axis 3 on the Pegasus and the Jet Leader Force Feedback, `(raw + 128) * 257 - 32768` with raw signed |
| Buttons | 0-11 from data byte 5 and the low nibble of data byte 6. The Pegasus has buttons 0-8. |
| Hat 0 | High nibble of data byte 6 |
| Grip sensor | The button after the last one |

The grip sensor is data byte 0 bit 1 of the device's status packets. It reads
released until the first status packet. Wheels and joysticks get no gamepad
mapping.

## Force feedback

Opening the haptic device sends `40 03 00` and `40 04 01`, which turn the
centering spring off, then `42 04`, which enables force feedback. Closing it
sends `42 01`, which stops every effect. A wheel used for input alone keeps
the centering it powers up with. The commands are the same over USB and
RS-232, where each goes out in a frame. `SDL_Quit` closes the joysticks
before the haptic devices, so a haptic device still open at `SDL_Quit` sends
no `42 01`. Close it first.

- Effects: constant, sine, square, triangle, sawtooth up and down, spring and
  damper, with gain and autocenter. The device's `N` reply gives the effect
  count, 32 at most, and its `B` reply the effect memory, 200 bytes when it
  does not answer.
- Parameter blocks go in the device's memory at the first address that fits.
  An update sends only the blocks and core that changed. A block is written at
  most every 20 ms, the spacing jsmolina's Windows port gives periodic
  updates, and an update that comes sooner waits for its turn, replaced by
  any later one. Linux instead waits for a status packet that names the
  block.
- An update whose commands fail to send is undone, and the call fails. The
  same update sent again goes out, its parameter blocks no sooner than 20 ms
  after the failed try, since the device may hold them. A waiting update
  whose send fails waits again, three times in a row at most. Each try holds
  the joystick lock for as long as the write takes, up to 1000 ms on an
  endpoint that refuses data. After the third failed send the update is
  dropped with a debug log line, and the next update of that effect is built
  against the parameters the device held. A send that goes through starts
  the count again.
- The haptic functions can be called under the joystick lock, from an event
  watcher or after `SDL_LockJoysticks`.
- A trigger button n is the device's button n - 1, as SDL's DirectInput
  backend numbers them. 15 is the highest.
- More than 255 iterations play 255 times.
- Effect status, pause and resume are not supported.

## Known limits

- No I-Force device has run this code. The tests replay constructed packets,
  the `06F8:0004` button bytes from jsmolina's notes, and the WingMan Force
  replies a bridge recorded.
- No source gives the endpoint packet sizes. SDL reads the endpoint's own max
  packet size, so a size below 9 would cut the rudder byte.
- The Force RS's buttons 9 to 12 and the `C0A4`'s extra axis have no known
  wire position. These wheels get the wheel layout.
- The Pegasus's data byte 6 bits 0 to 3 are button 8 and spare bits, as
  Linux's button table decodes them. Its Linux axis table declares a second
  hat that Linux never reads, and SDL reads none.
- The enable byte is Linux's `42 04`. A Windows XP driver sent `40 05 00 04`
  and then `42 01` to a WingMan Force, whose motors then ran only with a hand
  on the grip.
- On RS-232 no device frame is recorded. Whether a device sends the XOR
  checksum or needs DTR or RTS is not known. The driver counts checksum
  mismatches and ignores them, as Linux does.

## Tests

The protocol module is C99 with no SDL runtime and no I/O, and both drivers
share it. `test/controller-protocols` builds and runs `test/testiforce.c`
against the packets and commands, and `test/serial-joystick` runs
`testserialiforce` against the framing and the query sequence on a scripted
port. `test/libusb-backend` runs `testiforcedriver`: the USB joystick driver
against a fake libusb, and the haptic driver against a joystick that records
its commands. `test/serial-driver` runs `testserialdriver`: the serial driver
in a static SDL against scripted COM ports, with SDL's haptic API on a serial
wheel, whose frames it compares with the commands the haptic driver sends a
USB device. All four run in normal and AddressSanitizer builds.
