# HID devices without a Windows controller

These devices open through hid.dll, but Windows gives no usable game
controller for them. Their descriptors declare no controller usage, they stay
silent until the host writes a start sequence, or their data sits in vendor
reports. Each has a small HIDAPI driver over a pure protocol module, which
`test/controller-protocols` replays. SDL loads no driver and no firmware.

| Device | ID | Hint `SDL_HINT_JOYSTICK_HIDAPI_...` | SDL device |
|---|---|---|---|
| Logitech Speed Force Wireless | 046D:C29C | `SPEEDFORCE` | Wheel: the wheel, two pedals, 11 buttons |
| PhoenixRC USB adapter | 1781:0898 | `RC_ADAPTER` | 8 axes, one per channel |
| Microsoft SideWinder Game Voice | 045E:003B | `GAMEVOICE` | 8 buttons |
| Essential Reality P5 glove | 0D7F:0100 | `P5GLOVE` | 5 finger axes, 4 buttons |
| Dream Cheeky roll-up drum kit | 1941:8021 | `DREAMCHEEKY` | Drum kit, 6 pads |
| OCZ Neural Impulse Actuator | 1234:0000 | `NIA` | 1 axis |
| In2Games Gametrak | 14B7:0982 | `GAMETRAK` | 6 axes, 12 buttons, a hat |
| Oculus Rift DK1 head tracker | 2833:0001 | `RIFT_DK1` | Yaw, pitch and roll axes, accelerometer, gyroscope |
| Windows Mixed Reality motion controllers | 045E:065B, 045E:065D, 045E:066A | `WMR` | 4 or 5 axes, 6 buttons, accelerometer, gyroscope |
| SteelSeries Nimbus | 0111:1420 | `NIMBUS` | Gamepad |
| Creative Prodikeys PC-MIDI | 041E:2801 | `PRODIKEYS` | 152 buttons, a velocity axis |

Each hint defaults to `SDL_HINT_JOYSTICK_HIDAPI`, except the drum kit's,
which defaults to off. Only the Nimbus gets a gamepad mapping, and the Speed
Force is a wheel. The rest stay joysticks.

## Enumeration

With `SDL_HINT_HIDAPI_ENUMERATE_ONLY_CONTROLLERS` on, its default, SDL drops
every collection that is not a joystick, gamepad or multi-axis controller.
The table in `src/hidapi/SDL_hidapi_collections.c` admits these devices'
collections anyway: the Game Voice's telephony headset collection, page 0x0B
usage 0x05, the P5's native collection, page 0x8C usage 1, the NIA's vendor
collection, page 0xFF00 usage 0xFF01, and every collection of the others,
whose usage no source records. The mouse, keyboard, pen and touch
collections that Windows opens for itself are never admitted.

The NIA and the Rift share their IDs with other devices, so their drivers
also need the manufacturer strings "Brain Actuated Technologies" and
"Oculus VR, Inc.". The drum kit's ID also belongs to a weather station and a
missile launcher, which is why its driver is off unless its hint turns it on.

## Devices that need a start

- Speed Force: the receiver pairs with the wheel only after feature AF and,
  40 ms later, feature B2 with two random bytes, as Linux sends them. The
  driver sends both at open and then turns autocentering off.
- Gametrak: the unit reports nothing until the host writes "Gametrak", then
  45 and a key byte, and after every 100 reports 46 and the next key byte.
  hid.dll cannot send these writes, so on Windows the Gametrak is read
  through libusb once WinUSB is bound to it, as
  [README-vendor-usb.md](README-vendor-usb.md) describes. Linux and macOS
  send them through their HID backends.
- Rift DK1: the driver clears the sensor-frame flag of feature 2, sets its
  packet interval to 1 and sends keep-alive feature 8 at open and every 3 s.
- Windows Mixed Reality controllers: paired to the PC over Bluetooth, not to
  a headset. At open the driver sends a reset and a quiesce, reads firmware
  blocks 0 and 3 and the configuration block, and turns on the status reports
  and the IMU, waiting up to 250 ms for each answer. The controller appears
  when that is done, after a second or two.
- Prodikeys: the driver serves the collection of interface 1 that declares
  input report 3 or 4 and sends output report 6, `01 C1`, at open. The typing
  keys and report 1 stay with Windows.

## Sensors

The Rift DK1 gives every accelerometer and gyroscope sample at 1000 Hz with
the Oculus SDK's timing, in the headset's frame, which is SDL's. Its axes 0-2
are yaw, pitch and roll from a port of OpenHMD's fusion, -180 to +180
degrees over the full axis range.

The Windows Mixed Reality controllers give their samples calibrated with the
controller's configuration block, as Monado applies it, and turned into
SDL's frame. The timestamps come from the controller's 100 ns clock. No
source gives the sample rate, so SDL reports it as 0.

## Known limits

- hid.dll hands every input report over at the length of the collection's
  longest input report. The Windows Mixed Reality driver accepts status
  reports and answers at their own length or longer on Windows, and the Rift
  DK1 driver takes any report 1 of 62 bytes or more, as the Oculus SDK does.
- Windows 11 24H2 and later have no Windows Mixed Reality. Controllers whose
  firmware Windows never updated may not answer the start-up, and a failed
  start-up leaves the controller without a joystick.
- If the Nimbus sends input reports other than its controls, the driver
  reads them as controls, as MFIGamepadFeeder does.
- Linux replaces the PhoenixRC adapter's descriptor. Whether Windows accepts
  the original is not on record.
