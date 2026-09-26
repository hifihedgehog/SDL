# Xbox 360 chatpads and the uDraw GameTablet

This fork reads three Xbox 360 accessories whose input Windows never
delivers: the chatpad in the expansion port of a wired pad, the chatpad on a
wireless pad paired to the Xbox 360 wireless receiver, and the Xbox 360 uDraw
GameTablet paired to the same receiver. SDL's wired and wireless Xbox 360
HIDAPI drivers read them over libusb, beside the pads they already read. On
Windows that takes WinUSB on the whole pad or the whole receiver. SDL loads no
driver and no firmware, and it binds nothing.

The protocol lives in `src/joystick/hidapi/SDL_hidapi_xbox360acc_proto.c`,
which `test/controller-protocols` replays. The drivers are
`src/joystick/hidapi/SDL_hidapi_xbox360.c` for the wired pad and
`src/joystick/hidapi/SDL_hidapi_xbox360w.c` for the receiver.

| Device | ID | SDL joystick | Hint |
|---|---|---|---|
| Chatpad on a wired pad | 045E:028E at bcdDevice 1.10 or 1.14 | Xbox 360 Chatpad, 47 buttons | `SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_CHATPAD` |
| Chatpad on a wireless pad | receiver 045E:0719, 045E:0291 or 045E:02A9 | Xbox 360 Chatpad, 47 buttons | `SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_CHATPAD` |
| Xbox 360 uDraw GameTablet | the same receivers, subtype 0x23 | Xbox 360 uDraw GameTablet, 7 axes, 10 buttons, 1 hat | `SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_UDRAW` |

None of the three has a gamepad mapping, and each reads as
`SDL_JOYSTICK_TYPE_UNKNOWN`. A chatpad appears as a second joystick beside its
pad. A tablet takes its receiver slot in place of a pad.

## What Windows does today

Windows binds its Xbox 360 driver, xusb22, to the whole wired pad and to the
whole receiver by device ID, and games see the pads through XInput. Nothing
carries chatpad keys. XInput's keystroke API leaves its Unicode field at zero
and documents only the `VK_PAD_` gamepad keys, and Microsoft's XUSB
specification, [MS-XUSBI], says the host ignores the chatpad bytes in a
wireless pad's report. No source shows tablet data reaching any Windows API.

SDL cannot reach the raw data while xusb22 holds a device. Its Windows HID
backend skips the synthetic HID collections xusb22 creates, libusb cannot
open a device that xusb22 owns, and the libusb backend skips an Xbox interface
it cannot open. Such a pad reaches SDL through its other Windows backends, as
before, with no chatpad.

## Binding WinUSB

SDL reads these devices on Windows once WinUSB owns the whole device. Neither
device splits into per-interface nodes. Both report device class FF/FF/FF with
one configuration, and Windows loads its composite driver for such a device
only when an INF asks for it, so the driver on the device node owns every
interface.

The binding names revision-qualified hardware IDs:

| Device | Hardware ID |
|---|---|
| Wired pad, bcdDevice 1.14 | `USB\VID_045E&PID_028E&REV_0114` |
| Wired pad, bcdDevice 1.10 | `USB\VID_045E&PID_028E&REV_0110` |
| Wireless receiver | `USB\VID_045E&PID_0719&REV_0100` |

xboxdrv accepts chatpads on pad revisions 1.10 and 1.14 only, and SDL starts
a wired chatpad on no other. The binding names no ID for 045E:0291 or
045E:02A9, which SDL's receiver driver treats as the same receiver.

Windows lists a USB device's revision-qualified ID first, and xusb22.inf
matches the ID without the revision. By Microsoft's ranking rules an
Authenticode-signed package that names these IDs outranks xusb22 while the
AllSignersEqual policy is on, the default since Windows 7. With the policy
off, xusb22's Microsoft signature wins. This comes from Microsoft's
documents, and no machine has tested it.

The binding has these costs:

- The wired pad, or all four pad slots and all four headset slots of the
  receiver, leave XInput, DirectInput and Windows.Gaming.Input. They give
  input only to programs that open them through WinUSB, as SDL's libusb
  backend does. A host such as PadForge reads them through SDL and gives
  games virtual controllers in their place. With the host closed they give
  no input.
- xusb22 no longer serves the other interfaces, among them the wired pad's
  interface 1, FF/5D/03, and the receiver's headset interfaces.
- A pad revision the binding does not name stays on xusb22 with no chatpad.
- Undoing the binding is a driver rollback on the device node.

SDL binds nothing. The binding is the host's job, and the fork's decision for
PadForge makes it opt-in and reversible: bind a device only when the user
asks, after a prompt that states the cost, leave the binding on the device
node when the host exits or crashes, check it at every start and tell the
user when Windows has moved the device back to xusb22, which Windows can do
with a better-ranked match from its INF directory or Windows Update, and
offer a one-click restore that uninstalls the host's package, after which
Windows installs xusb22 again as the best remaining match.

WinUSB lets one handle open a device, so on Windows SDL opens the receiver's
four slots, and claims the wired pad's chatpad interface, through the one
handle it holds for the device. See [README-vendor-usb.md](README-vendor-usb.md).

## Chatpad keys

Both chatpads send the same key report: a modifier byte and two key codes. The
codes name physical positions and stay the same under every regional printing
of the keycaps. The names below are the US legends.

The chatpad joystick has 47 buttons, no axes and no hats. Within a row, the
buttons, keys and codes run in the same order.

| Buttons | Keys | Codes (hex) |
|---|---|---|
| 0 to 3 | Shift, Green, Orange, People | bits 0 to 3 of the modifier byte |
| 4 to 13 | 1, 2, 3, 4, 5, 6, 7, 8, 9, 0 | 17, 16, 15, 14, 13, 12, 11, 67, 66, 65 |
| 14 to 23 | Q, W, E, R, T, Y, U, I, O, P | 27, 26, 25, 24, 23, 22, 21, 76, 75, 64 |
| 24 to 33 | A, S, D, F, G, H, J, K, L, comma | 37, 36, 35, 34, 33, 32, 31, 77, 72, 62 |
| 34 to 42 | Z, X, C, V, B, N, M, period, Enter | 46, 45, 44, 43, 42, 41, 52, 53, 63 |
| 43 to 46 | Left, Space, Right, Backspace | 55, 54, 51, 71 |

A button is down while its modifier bit is set or while its code sits in either
key slot. The report holds two keys at most, and a code outside the table holds
nothing. The report carries held state only. On the console, by xboxdrv's
reading of the console manual, Shift, Green, Orange and People are one-shot
latches: a tap arms the mode and lights its lamp, and the next key consumes
it. Orange with Shift is caps lock. SDL reports held state and leaves latching
and characters to the application. The Green and Orange legends differ by
region.

The chatpad joystick has no rumble. `SDL_SendJoystickEffect` sets its lamps
with exactly one byte:

| Byte | Lamp |
|---|---|
| 00, 01, 02, 03 | Shift, Green, Orange, People off |
| 04 | Backlight off |
| 08, 09, 0A, 0B | Shift, Green, Orange, People on |
| 0C | Backlight on |

Any other byte or size fails. SDL lights no lamp by itself.

The chatpad joystick's GUID is its pad's, with the CRC of "Xbox 360 Chatpad"
and byte 15 at 0x82, a value that keeps it off the gamepad mappings.

## Wired chatpad

The chatpad reports on the wired pad's interface 2, class 0xFF, subclass
0x5D, protocol 2, through one interrupt IN endpoint: 0x84 with 32-byte packets
in a published descriptor of bcdDevice 1.14. [MS-XUSBI] shows 0x87 and leaves
the address to the chip, so SDL takes the first interrupt IN endpoint of
interface 2, whatever its address, and needs a packet size of 5 to 64 bytes.

When the pad opens, the driver sets the chatpad up if all of these hold:

- `SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_CHATPAD` is on.
- The pad is 045E:028E at bcdDevice 1.10 or 1.14.
- The pad is open through SDL's libusb backend.
- Interface 2 and its endpoint are there, and the driver can claim the
  interface.

The driver claims interface 2 on the handle the libusb backend holds for
interface 0, since WinUSB reaches a device's other interfaces only through
that handle. If a condition fails, the pad works as before with no chatpad
joystick. The start-up and the keep-alives go out whether or not a chatpad is
plugged in.

SDL sends these control transfers from the driver's updates, each after the
previous one completes, with a 100 ms timeout:

| Step | bmRequestType | bRequest | wValue | wIndex | wLength | Data | Due |
|---|---|---|---|---|---|---|---|
| 1 | 40 | A9 | A30C | 4423 | 0 | none | at the pad's first update |
| 2 | 40 | A9 | 2344 | 7F03 | 0 | none | at once |
| 3 | 40 | A9 | 5839 | 6832 | 0 | none | at once |
| 4 | C0 | A1 | 0000 | E416 | 2 | 2 bytes read | at once |
| 5 | 40 | A1 | 0000 | E416 | 2 | `09 00` at bcdDevice 1.14, `01 02` at 1.10 | at once |
| 6 | C0 | A1 | 0000 | E416 | 2 | 2 bytes read | at once |
| 7 | 41 | 00 | 001F | 0002 | 0 | none | 1000 ms after step 6 |
| 8 | 41 | 00 | 001E | 0002 | 0 | none | 1000 ms after step 7 |
| 9 | 41 | 00 | 001B | 0002 | 0 | none | at once |

The keep-alives follow step 9: `41 00` with wIndex 0002 and wValue 001F, then
001E, in turn, the first 1000 ms after step 9 and each later one 1000 ms after
the one before. A stall or any other failure counts as a completion, so the
sequence always goes on. Steps 1 to 3 can stall, as xboxdrv and the Chatpad
Super Driver record. The Chatpad Super Driver found that steps 4 to 6 fail
without them.

Once step 9 completes, SDL reads the chatpad endpoint through one interrupt
transfer with no timeout, which it submits again after each completed read,
on the libusb backend's event thread. SDL reads bytes 0 to 3 of a message of
5 bytes or more:

| Byte 0 | Message | Bytes 1 to 3 |
|---|---|---|
| 00 | Key report | modifiers, key 1, key 2 |
| F0 | Status | Byte 1 is 04 for the lamp status, or 03 to ask for a `1B`, which SDL sends at once, outside the schedule |

Any other first byte, or a shorter message, changes nothing.

The chatpad joystick connects on the first key report or status after step 9.
It disconnects with the pad, or when the pad reports on interface 0 that the
chatpad is gone: report `08 03` with bit 0 of byte 2 clear. A lamp byte goes
out as the wValue of a `41 00` request with wIndex 0002 and no data.

## Wireless chatpad

A wireless pad passes its chatpad's bytes through the receiver on its own
slot. The slots are the receiver's even interfaces, class 0xFF, subclass 0x5D,
protocol 0x81, and SDL opens each as its own device. The odd interfaces are
headset slots, and SDL never opens them.

SDL reads a receiver packet of 28 bytes or more that starts `00 02 00 F0` as
chatpad data. Bytes 24 to 27 hold a key report or a status in the wired
chatpad's layout, with byte 24 at 00 or F0. Packets of type 0x03, which carry
pad state and chatpad bytes together, decode as pad state only.

The chatpad joystick connects on the slot's first chatpad packet, even before
the slot has a type. It disconnects when the slot empties, when the receiver
goes, or when a tablet takes the slot.

Each command is 12 bytes on the slot's OUT endpoint: `00 00 0C`, the command
byte and 8 zero bytes. While the chatpad hint is on, SDL sends them to every
slot that shows a device, pads with or without a chatpad and tablets alike:

| Sent | Command |
|---|---|
| 100 ms after the slot shows a device | `1B` |
| every 1000 ms after that | `1E`, then `1F`, in turn |
| at the next scheduled command after a status `F0 03` | `1B`, in place of the keep-alive |
| after each scheduled command | each Shift, Green, Orange or People lamp the application lit |
| on `SDL_SendJoystickEffect` | the lamp byte, at once |

A slot shows a device on the connection status, `08` with bit 7 of byte 1
set, on the link control packet, or on a pad or tablet state packet. The
commands stop when the slot empties or the receiver goes, and the next device
on the slot starts again with `1B` after 100 ms. SDL queues the scheduled
commands during the driver's updates and sends them at the end of each update.

## Xbox 360 uDraw GameTablet

The tablet pairs with the receiver like a pad: hold its Guide button until the
lights flash, and press the sync buttons on the tablet and the receiver if
needed. Once paired, it lights one corner of its Guide button. It names
subtype 0x23 in its link control packet, and with
`SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_UDRAW` on, SDL gives its slot a joystick
named "Xbox 360 uDraw GameTablet" in place of a pad.

The tablet reports its pen, pressure, touch state and tilt in the bytes a pad
uses for its triggers and sticks. SDL reads bytes 6 to 17 of a packet of 18
bytes or more that starts `00 01 00 F0 00 13`:

| SDL | Control | Range |
|---|---|---|
| Axis 0 | Pen X, byte 11 x 256 + byte 10 | X x 65535 / 1919 - 32768, from -32768 at the left edge to 32767 at the right |
| Axis 1 | Pen Y, byte 13 x 256 + byte 12 | Y x 65535 / 1079 - 32768, from -32768 at the top edge to 32767 at the bottom |
| Axis 2 | Pressure, byte 8 | byte x 257 - 32768. About 0x72 to 0x74 at rest, up to 0xFF |
| Axis 3 | Finger spread, byte 15 | byte x 257 - 32768. 0x00 except while two fingers pinch |
| Axis 4 | Tilt X, byte 16 | byte x 257 - 32768. About 0x20 at rest, 0x35 tilted left, 0x0C tilted right |
| Axis 5 | Tilt Y, byte 17 | byte x 257 - 32768. About 0x24 at rest, 0x35 tilted back, 0x0A tilted forward |
| Axis 6 | Tilt Z, byte 9 | byte x 257 - 32768. About 0x0C at rest, 0x35 upside down |
| Buttons 0 to 3 | A, B, X, Y | byte 7 bits 4 to 7 |
| Button 4 | Back | byte 6 bit 5 |
| Button 5 | Start | byte 6 bit 4 |
| Button 6 | Guide | byte 7 bit 2 |
| Button 7 | Pen down | byte 14 at 0x40 |
| Button 8 | Finger down | byte 14 at 0x80 |
| Button 9 | Multi-touch | byte 14 at any other nonzero value |
| Hat 0 | D-pad | byte 6 bits 0 to 3: up, down, left, right |

The surface is 1920 by 1080, an 8 by 5 grid of 256 by 256 cells whose last
column is 128 wide and last row 56 tall. X and Y follow the pen or finger only
while byte 14 is nonzero and both cells are below 0x0F, and they hold their
last values otherwise. SDL clamps X to 1919 and Y to 1079 before scaling. The
tablet joystick starts with every axis at 0, no button down and the hat
centered, and X and Y stay at 0, the center, until the first touch.

The tablet has no rumble. Its GUID is its slot's, with the CRC of its name and
byte 15 at 0xA3. With `SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_PLAYER_LED` on, the
driver lights the corner of the tablet's Guide button for its player index.
SDL assigns player indexes on its own to gamepads only, so unless the
application sets one, the tablet lights the corner for its receiver slot.

## Receiver slots

Each of the receiver's four pad slots carries a pad or a tablet. With
`SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_UDRAW` on, the default, a slot's joystick
waits for the slot's type:

- The connection status, `08` with bit 7 of byte 1 set, marks a device on the
  slot and connects no joystick.
- The link control packet, 29 bytes starting `00 0F 00 F0`, types the slot by
  byte 25 without bit 7: 0x23 makes a tablet, any other value a pad. The
  receiver sends it with the connection status when the driver asks for the
  slot's status, which the driver does when it opens.
- A pad state packet, 29 bytes with byte 0 at 00 and an odd byte 1, other
  than the reply to the driver's capabilities request, types as a pad a slot
  that no link control packet has typed.
- A later link control packet that names the other kind replaces the slot's
  joystick. The pad and its chatpad go and the tablet comes, or the tablet
  goes and a pad comes.
- The connection status with bit 7 clear empties the slot and disconnects its
  joysticks.

Until the slot has a type it has no pad or tablet joystick. A pad slot typed
by its link control packet connects with its subtype already in byte 15 of its
GUID and in its joystick type.

With the hint off, a slot connects as a pad on its connection status, as
before, and the link control packet types nothing. A tablet is then an Xbox
360 gamepad whose bytes decode as a pad's, with its pen position on the left
stick.

The slot typing, the chatpad and the tablet apply to 045E:0719, 045E:0291 and
045E:02A9. Any other adapter the receiver driver serves keeps its slots as
before.

## Hints

| Hint | Default | Off |
|---|---|---|
| `SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_CHATPAD` | The value of `SDL_HINT_JOYSTICK_HIDAPI_XBOX_360`, which follows `SDL_HINT_JOYSTICK_HIDAPI_XBOX` when unset | No chatpad is read, no chatpad command goes out, and the wired driver leaves interface 2 alone |
| `SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_UDRAW` | The value of `SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_WIRELESS`, which follows `SDL_HINT_JOYSTICK_HIDAPI_XBOX_360` when unset | A tablet is an Xbox 360 gamepad, and a slot connects on its connection status |

SDL reads both when it opens a wired pad or a receiver, so a change applies
when the device is next plugged in, or after the driver's own hint,
`SDL_HINT_JOYSTICK_HIDAPI_XBOX_360` for the wired pad or
`SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_WIRELESS` for the receiver, is turned off
and on again. With both hints "0" the two drivers do what they did before.

## macOS and Linux

On macOS the libusb backend reaches Xbox 360 pads and receivers with no
binding, as before, and skips an Xbox interface that a kernel driver already
holds. With the hints on, the default, the changes above apply there. A wired
pad at bcdDevice 1.10 or 1.14 has interface 2 claimed and gets the start-up
and keep-alives. Receiver slots get the chatpad commands, a slot's joystick
connects on its link control packet or first state packet in place of its
connection status, and a tablet gets a tablet joystick. A pad that another
driver holds, such as 360Controller, gets no chatpad joystick. The receiver
driver types slots and sends the chatpad commands on every slot of the three
receivers it serves, whichever backend delivers the slot.

On Linux the kernel's xpad driver keeps Xbox 360 pads and receivers, and
SDL's libusb backend leaves them to it unless
`SDL_HINT_HIDAPI_LIBUSB_WHITELIST` is "0". Nothing changes there by default.

## Known limits

- None of this has run on hardware. The replay tests are the evidence.
- The binding's rank over xusb22 rests on Microsoft's documents, not a test.
  Sources show a whole-device libusb driver working on Windows for the
  receiver only, with libusb-win32 or an unnamed libusb driver. None names
  WinUSB on either device or shows the wired pad under such a binding.
- If the receiver's first slot fails to open while a later slot opens, the
  first slot's next open claims interface 0 after libusb has already set it
  up for the later slot. libusb 1.0.29 then calls WinUSB's Initialize a
  second time. libusb's source at a45bb16 skips that call and notes that it
  leaks the first handle.
- The wired start-up follows xboxdrv, and the sources disagree on its details.
  The Chatpad Super Driver always writes `09 00` at step 5 and notes that
  `01 02` may be needed on some pads. It also waits 12 ms after every request
  and sends `1B` once, after the first chatpad message. Whether those
  differences matter is not recorded.
- xboxdrv refuses pad revisions other than 1.10 and 1.14, and their chatpad
  endpoint and start-up data are not recorded. SDL starts no chatpad on them.
- The wired driver runs the start-up once per pad connection. It does not run
  it again, or restart a chatpad read that ended in an error, while the pad
  stays connected.
- How often the wired pad relays the chatpad's status is not recorded, so an
  idle chatpad's joystick can wait for the first key.
- The wired driver answers a status `F0 03` with a `1B`. Only disabled
  JoypadOS code gives that status a meaning on the wired endpoint.
- No source shows chatpad keys in a receiver packet of type 0x03, and SDL
  reads none there. ogx360 and the JoypadOS tusb_xinput fork read chatpad
  bytes from such packets, and a receiver that sends keys that way would lose
  them.
- The wireless sources disagree on the keep-alive order, on repeating `1B`
  and on the command length, 12 bytes or 4. SDL sends 12 bytes and starts
  with `1E`, as xboxdrv does. No variant is shown to fail.
- The sources give `04`, backlight off, for the wired chatpad only. SDL sends
  it to the wireless chatpad as well. [MS-XUSBI] defines a different wired
  control, the backlight alone, with wValue 0000 off and 0001 on. SDL sends
  xboxdrv's codes.
- Subtype 0x23 comes from uDrawTablet alone, and the subtype table of
  [MS-XUSBI] does not list it. Whether the tablet's packets are 29 bytes like
  a pad's is not confirmed, and SDL takes 18 bytes or more.
- The scale of the tilt values and the meaning of the pinch values are not
  documented, so tilt stays on raw axes and not SDL sensors.
- No source tests whether xusb22 gives a tablet an XInput slot today. If it
  does, the pen position already reaches XInput's left stick as raw numbers.

## Tests

The protocol module is C99 with no SDL runtime and no I/O.
`test/controller-protocols` runs `testxbox360acc` against key reports built
from Spivey's serial capture in xboxdrv's USB framing, receiver packets built
from [MS-XUSBI] and tablet packets built from brandonw.net's table: every key
code, the wired start-up and keep-alives at both revisions, the wireless
commands, four slots at once, slot typing, the tablet's corners, both hints
off, and every truncation of every packet with stale bytes past its length,
all on an injected clock. `testvendorusb` checks the enumeration rule that
keeps a wired pad or receiver this process holds, and that the pad's other
interfaces and the receiver's headset interfaces are never listed. Both run
in normal and AddressSanitizer builds.
