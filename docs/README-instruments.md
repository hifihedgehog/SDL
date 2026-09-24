# Guitar Hero Live, Rock Band 3 Pro and PS3 peripherals

These devices reach Windows with a driver bound, but their main input needs a
command or a decoder that no Windows component provides. The protocols live in
pure modules that `test/controller-protocols` replays:
`src/joystick/SDL_ghl_proto.c`, `src/joystick/SDL_rb3pro_proto.c` and
`src/joystick/hidapi/SDL_hidapi_ps3ext_proto.c`. The GameInput raw types are in
`src/joystick/gdk/SDL_gameinput_rawtype.h`.

## Guitar Hero Live guitars

| Dongle | ID | Read by | Keep-alive |
|---|---|---|---|
| PS3 and Wii U | 12BA:074B | HIDAPI GHL driver | output report ID 0: 02 08 20 00 00 00 00 00 |
| PS4 | 1430:07BB | HIDAPI GHL driver | output report 0x30: 02 08 0A 00 00 00 00 00 |
| Xbox One | 1430:079B | GameInput raw reports, or the GIP driver over WinUSB | GIP message 0x22: 02 08 0A 00 00 00 00 00 |

Without the keep-alive the strum bar cuts out held frets. Each path sends it at
open and every 8 seconds, on the control pipe for the HID dongles. A failed
send is retried on the next update. `SDL_HINT_JOYSTICK_HIDAPI_GHL`, on by
default, turns the HIDAPI driver on. With it off the PS3/Wii U dongle falls
back to the PS3 driver.

All three are `SDL_JOYSTICK_TYPE_GUITAR` with 11 buttons in SDL gamepad order,
6 axes and a hat, so the default mapping applies. Black 1, 2 and 3 are SOUTH,
EAST and NORTH, White 1 is WEST, White 2 and 3 are the shoulders, Hero Power is
BACK, Pause is START, GHTV is LEFT_STICK and the d-pad center is GUIDE. The
strum bar is LEFTY and adds hat up at 0x00 and down at 0xFF. Whammy is RIGHTY
and tilt RIGHTX. The PS4 guitar joins and leaves with byte 25 of its report.
On Xbox One the guide message carries the d-pad center, and message 0x20, the
gamepad-shaped copy, is not read.

The Xbox One key was 1430:0170, the PDP Jaguar guitar's product ID. It is now
1430:079B.

## Rock Band 3 Pro instruments

| Model | PS3 | Wii | Xbox 360 |
|---|---|---|---|
| Keyboard | 12BA:2330 | 1BAD:3330 | XInput subtype 15 |
| MIDI Pro Adapter, keyboard mode | 12BA:2338 | 1BAD:3338 | XInput subtype 15 |
| Mustang Pro guitar | 12BA:2430 | 1BAD:3430 | XInput subtype 25 |
| MIDI Pro Adapter, Mustang mode | 12BA:2438 | 1BAD:3438 | XInput subtype 25 |
| Squier Pro guitar | 12BA:2530 | 1BAD:3530 | XInput subtype 25 |
| MIDI Pro Adapter, Squier mode | 12BA:2538 | 1BAD:3538 | XInput subtype 25 |

The PS3 third-party driver accepts the PS3 and Wii IDs without its feature
probe. The PS3 models send keys and frets only after a 40-byte feature
SET_REPORT. On Windows, where `HidD_SetFeature` cuts a report to its declared
8 bytes, it goes as five reports of report ID 0 and one row each, with 1473 us
after each of rows 2 to 5. Elsewhere it goes as one report. It is sent at open,
and again when a report reads 0x02 in byte 24, the navigation-only marker,
more than 8 seconds after the last one. The Wii models need nothing.

Both layouts put the gamepad buttons first: cross, circle, square and triangle
on 0 to 3, select 4, PS 5, start 6, and 7 to 10 never pressed. Hat 0 is the
d-pad. The mapping has those buttons and the hat only.

- Keyboard, `SDL_JOYSTICK_TYPE_UNKNOWN`: button 11 overdrive, 12 the digital
  pedal, 13 to 37 the keys C1 to C3, 38 pedal connected. Axes 0 to 4 are the
  five velocity slots, 5 the analog pedal, 6 the touch strip, each 0 to 127
  scaled to 0 to 32767.
- Pro guitar, `SDL_JOYSTICK_TYPE_GUITAR`: button 11 the solo flag, 12 the
  pedal, 13 to 17 the green, red, yellow, blue and orange flags, 18 pedal
  connected. Axes 0 to 5 are the fret numbers from low E to high E, 0 to 31
  scaled to 0 to 32767, axes 6 to 11 the string velocities, 12 the tilt, 13 the
  microphone sensor and 14 the light sensor.

On Xbox 360 the XInput backend decodes the same layout from the raw
`XINPUT_STATE`. The touch strip, the tilt, both sensors, the guitar's pedal and
the pedal connection sit in six XUSB report bytes that `XInputGetState` leaves
out. The backend reads them through OpenXInput's
`OpenXInputGetStateExtendedV1` when that export exists. Without it they read as
zero and released. These subtypes have no Share button.

## uDraw GameTablet, Top Shot guns and Tony Hawk boards

The PS3 third-party driver accepts these by ID. None has a gamepad joystick
type.

| Device | ID | Buttons, axes, hats |
|---|---|---|
| THQ uDraw Game Tablet for PS3 | 20D6:CB17 | 14, 4, 1, and an accelerometer |
| Top Shot Elite | 12BA:04A0 | 16, 8, 1 |
| Top Shot Fearmaster | 12BA:04A1 | 16, 7, 1 |
| Tony Hawk RIDE Skateboard | 12BA:0400 | 7, 6, 1 |
| Tony Hawk Skateboard | 1430:0100 | 7, 6, 1 |

- uDraw: button 11 pen touching, 12 one finger, 13 two fingers. Axes 0 and 1
  are X and Y from 0 to 1919 and 0 to 1079, updated only while something
  touches. Axis 2 is pen pressure, the pressure byte less 113 and clamped to 0
  to 142, and axis 3 the two-finger distance. The accelerometer counts from
  0x200 at 22 counts per g, an estimate from brandonw.net's readings. Only
  27-byte reports are read.
- Top Shot: the gun reports the sensor bar's two LEDs only after a SET_REPORT
  of 82 00 01 00 00 00 00 00. It goes as an output report at open. While bytes
  7 to 12 read zero for a second it goes again, at most once a second, as a
  feature report first and then alternating, until one brings data. Button 7
  is L3, 8 R3 on the Elite, 11 the trigger, 12 reload on the Elite, 13 and 14
  the left and right LED in view, 15 the Fearmaster's heart-rate sensor
  touched. The LED coordinates run 0 to 1023. The Fearmaster's axis 6 is its
  heart-rate word, unscaled.
- Tony Hawk: axes 0 to 3 are the nose, tail, left and right IR sensors, 0 to
  0x38, and axes 4 and 5 the nose and tail accelerometer bytes, signed. The
  board's other accelerometer words are encrypted and not read. The board
  joins on a report that differs from both of the dongle's idle reports and
  leaves on either. The driver sends an output report of zeros at open, as
  RPCS3 does.

## Output reports on the control pipe

`SDL_hid_write` sends on the first interrupt OUT endpoint when one exists. The
internal `SDL_hid_send_output_report` sends a SET_REPORT with report type
output on the control pipe instead: `HidD_SetOutputReport` on Windows, the
control transfer on libusb, `HIDIOCSOUTPUT` on Linux, `IOHIDDeviceSetReport`
on macOS and the output report on NetBSD. Android and iOS return -1.
