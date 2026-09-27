# HID devices without a Windows controller

On Windows these devices open through hid.dll, all but the Gametrak and the
Prodikeys, which SDL reads through libusb. Windows gives no usable game
controller for any of them. Their descriptors declare no controller usage,
they stay silent until the host writes a start sequence, or their data sits in
vendor reports. Each has a small HIDAPI driver over a pure protocol module,
which `test/controller-protocols` replays. SDL loads no driver and no
firmware.

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

The drivers run on every platform with HIDAPI, with two exceptions. On Apple
platforms the Nimbus driver claims nothing, since GCController presents the
Nimbus through SDL's MFi backend. On Linux the Speed Force, Rift DK1 and
Windows Mixed Reality drivers claim nothing. There hid-lg and lg4ff drive
the Speed Force with force feedback through evdev, OpenHMD reads the Rift
DK1 through hidraw, and Monado reads the controllers the same way. The
drivers' start-up writes would disturb both.

## Enumeration

With `SDL_HINT_HIDAPI_ENUMERATE_ONLY_CONTROLLERS` on, its default, SDL drops
every collection that is not a joystick, gamepad or multi-axis controller.
The table in `src/hidapi/SDL_hidapi_collections.c` admits these devices'
collections anyway: the Game Voice's telephony headset collection, page 0x0B
usage 0x05, the P5's native collection, page 0x8C usage 1, the NIA's vendor
collection, page 0xFF00 usage 0xFF01, and every collection of the PhoenixRC
adapter, the drum kit, the Rift DK1, the Windows Mixed Reality controllers,
the Nimbus and the Prodikeys. No source records those six devices' usages,
except one log of the Rift DK1's. Their drivers then open only a collection
whose descriptor declares the reports they use: input 1 and features 2 and 8
for the Rift DK1, input 1 and output 6 for the Windows Mixed Reality
controllers, input 3 or 4 for the Prodikeys, and any input report for the
others. The Speed Force's and the Gametrak's collections are Generic Desktop
joysticks and pass without a row. The mouse, keyboard, pen and touch
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
  When no report arrives within a second of the key byte, the driver starts
  the unlock over, three unlocks in all. hid.dll cannot send these writes,
  so on Windows the Gametrak is read through libusb once WinUSB is bound to
  it, as [README-vendor-usb.md](README-vendor-usb.md) describes. Linux and
  macOS send them through their HID backends.
- Rift DK1: the driver clears the sensor-frame flag of feature 2, sets its
  packet interval to 1 and sends keep-alive feature 8 at open and every 3 s.
- Windows Mixed Reality controllers: paired to the PC over Bluetooth, not to
  a headset. At open the driver sends a reset and a quiesce, reads firmware
  blocks 0 and 3 and the configuration block, and turns on the status reports
  and the IMU, waiting up to 250 ms for each answer. The controller appears
  when that is done, after a second or two.
- Prodikeys: the driver serves the collection of interface 1 that declares
  input report 3 or 4 and sends output report 6, `01 C1`, at open. The typing
  keys on interface 0 stay with the system. On Windows the driver reads
  interface 1 through libusb, as Prodikeys64 does, so WinUSB must be bound to
  `USB\VID_041E&PID_2801&MI_01`. That takes reports 1 and 2, the media and
  sleep keys, away from Windows, and without it SDL shows no Prodikeys
  joystick on Windows. On Linux and macOS report 1 stays with the system.

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
  longest input report, as libusb/hidapi issue 210 records. The Windows Mixed
  Reality driver accepts status reports and answers at their own length or
  longer on Windows, and the Rift DK1 driver takes any report 1 of 62 bytes or
  more, as the Oculus SDK does.
- Windows 11 24H2 and later have no Windows Mixed Reality. Controllers whose
  firmware Windows never updated may not answer the start-up, and a failed
  start-up leaves the controller without a joystick.
- If the Nimbus sends input reports other than its controls, the driver
  reads them as controls, as MFIGamepadFeeder does.
- Linux replaces the PhoenixRC adapter's descriptor. Whether Windows accepts
  the original is not on record.
- The Prodikeys descriptor declares 16 bits for report 4 where the keyboard
  sends 24, which Linux corrects. When a HID backend hands report 4 over at
  its declared 3 bytes, the driver takes mask bits 8-23 from it, and buttons
  128-135, Fn lock among them, stay up. Over WinUSB the report arrives whole.
