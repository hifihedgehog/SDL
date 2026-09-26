# iCade

This fork reads the ION iCade arcade cabinet and pads in iCade mode on
Windows. They pair as Bluetooth HID keyboards and type one letter when a
control is pressed and another when it is released, so Windows sees typing
and no game sees a controller. The driver,
`src/joystick/windows/SDL_icadejoystick.c`, decodes those keyboards into
joysticks. The letter protocol lives in
`src/joystick/windows/SDL_icade_proto.c`, which `test/controller-protocols`
replays, and `test/icade-driver` runs the driver against a fake Win32 system.

| Device | How the driver knows it | SDL joystick | Type |
|---|---|---|---|
| ION iCade | Bluetooth HID keyboard 15E4:0132 | ION iCade, 8 buttons, 1 hat | `SDL_JOYSTICK_TYPE_ARCADE_STICK` |
| A pad in iCade mode | its IDs listed in `SDL_HINT_JOYSTICK_ICADE_DEVICES` | iCade Controller (0xVVVV/0xPPPP), 8 buttons, 1 hat | `SDL_JOYSTICK_TYPE_GAMEPAD` |

Both have a gamepad mapping, which the driver supplies.

## The letters

| Control | Press letter | Release letter | Joystick control |
|---|---|---|---|
| Stick up | W | E | Hat up |
| Stick right | D | C | Hat right |
| Stick down | X | Z | Hat down |
| Stick left | A | Q | Hat left |
| Button A | Y | T | Button 0 |
| Button B | H | R | Button 1 |
| Button C | U | F | Button 2 |
| Button D | J | N | Button 3 |
| Button E | I | M | Button 4 |
| Button F | K | P | Button 5 |
| Button G | O | G | Button 6 |
| Button H | L | V | Button 7 |

The cabinet's buttons sit in two rows, A C E G over B D F H. Opposite stick
directions held together cancel. The letters B and S mean nothing.

The driver reads scan codes, so the keyboard layout does not matter. It acts
on key-down records only and ignores key-ups, records with the E0 or E1
prefix, and the overrun code FF. A lost release letter leaves its control
held until the next letter for that control. When the keyboard leaves, every
control is released.

## Gamepad mappings

| Gamepad control | Cabinet | Pad in iCade mode |
|---|---|---|
| South | B | F |
| East | D | G |
| West | A | H |
| North | C | E |
| Back | | A |
| Start | | C |
| Left shoulder | G | B |
| Right shoulder | E | D |
| Left trigger | H | |
| Right trigger | F | |
| D-pad | the stick | the stick |

The pad column is iCade-iOS's table of what a pad's letters mean, with the
pad's Y, A, B and X read as on an Xbox pad. No source maps the cabinet, so its
column is a proposal.

## Raw Input

The driver runs a thread with a message-only window of class `SDL_ICade`. It
lists the keyboards with `GetRawInputDeviceList`, opens each keyboard's
interface path with no access, since Windows keeps keyboards for its own use,
and reads the IDs with `HidD_GetAttributes`. It lists them again 300 ms and
2 s after each HID interface notification, and tries a keyboard whose IDs
could not be read again on every list. At most eight iCade devices are
decoded at once, and each keeps a queue of 128 state changes that drops the
oldest when full.

Windows gives a device class's Raw Input to one window per process, the one
registered last. With `SDL_HINT_JOYSTICK_ICADE_RAWINPUT` on, the default, the
driver registers keyboards on its window, with `RIDEV_INPUTSINK` and
`RIDEV_DEVNOTIFY`, only while at least one iCade is connected and no other
window of the process holds the keyboard class. With no iCade, SDL leaves the
process's keyboard registration alone. SDL removes its registration when the
last iCade leaves, when the hint turns off and at `SDL_Quit`, and only if its
window still holds it. A window that registers keyboards while SDL holds the
class takes the input from SDL.

SDL's own raw keyboard input, `SDL_HINT_WINDOWS_RAW_KEYBOARD`, passes every
keyboard record it receives to the driver, so the iCade keeps working while
SDL video holds the keyboard class. Without
`SDL_HINT_WINDOWS_RAW_KEYBOARD_INPUTSINK`, SDL video receives keyboard
records only while one of its windows has the focus.

A host that registers keyboards itself sets `SDL_HINT_JOYSTICK_ICADE_RAWINPUT`
to "0" before `SDL_Init`, and passes each keyboard record its window receives
to `SDL_ICadeProcessRawKeyboard()` with the record's `hDevice`, `MakeCode` and
`Flags`. SDL then registers nothing and still lists and identifies the
keyboards itself. The function returns true for a keyboard the driver decodes
as an iCade, so the host can keep that keyboard out of its own keyboard
handling. It takes only the driver's lock and can be called from any thread.
It returns false before `SDL_Init`, after `SDL_Quit`, and in a build without
the driver.

The letters still reach the window with the keyboard focus. Raw Input
observes keystrokes and removes none, and SDL suppresses nothing. A host that
wants to stop them, with a keyboard hook or a filter driver, finds the device
two ways. The joystick's path, `SDL_GetJoystickPathForID()`, is the
keyboard's Raw Input device name (`RIDI_DEVICENAME`). A record with make code
0 and `RI_KEY_BREAK` set asks `SDL_ICadeProcessRawKeyboard()` whether a
handle is an iCade without changing any control.

## Hints

| Hint | Default | Effect |
|---|---|---|
| `SDL_HINT_JOYSTICK_ICADE` | "1" | "0" turns the driver off |
| `SDL_HINT_JOYSTICK_ICADE_RAWINPUT` | "1" | "0" makes SDL register nothing and take the records from the host |
| `SDL_HINT_JOYSTICK_ICADE_DEVICES` | empty | More keyboards to decode as pads, as `0xVVVV/0xPPPP` pairs separated by commas |

All three can change at any time. In the devices hint each number is "0x" and
one to four hex digits, spaces and tabs around the numbers are ignored, a
malformed entry is skipped with a warning, vendor 0x0000 is refused, and at
most 32 pairs are read.

## Known limits

- No iCade ran here. No source shows whether Windows reports the cabinet's
  keyboard as 15E4:0132, and the cabinet's report descriptor is not
  published.
- The Bluetooth IDs of the iCade 8-Bitty, the iCade Mobile and pads in iCade
  mode are not published, so each needs the devices hint.
- A pad in iCade mode is still a keyboard to every other application.
- When another window gives up the keyboard class, as SDL video does when
  its raw keyboard input turns off, the driver registers again only at its
  next device change or hint change, and the iCade's letters are lost until
  then.
- The labels printed on the cabinet are not recorded. iCade-iOS, Linux and
  android-bluez-ime name the eight positions three ways.
- Windows only.

## Tests

`test/controller-protocols` runs `testicadeproto`: the part's replay tests 1
to 4 and 6, every make code, every flag word on a press and a release letter,
the hat, both layouts, the devices hint parser and the queue. `test/icade-driver` runs the driver inside
a static SDL against a fake Win32 system and fails its build if the test
still imports a Win32 function that reaches a device or the Raw Input
registration. It covers the part's tests 5 and 7 to 9, the device list, the
driver switched off and on, a full queue, keyboard traffic that never waits
for the joystick lock, and a registration that exists only while an iCade
does. Both run in the normal and AddressSanitizer builds.
