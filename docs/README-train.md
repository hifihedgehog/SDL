# Train controllers

This fork reads the USB train controllers made for PlayStation 2 train games
and Pony Canyon's RS-232 Master Controllers for PC train simulators. Windows
ships no driver for any of them. The HID train controllers, such as the
ZUIKI Mascon and the DGOC-44U, already work as ordinary joysticks and are not
covered here.

## USB

| ID | bcdDevice | Name | Layout |
|---|---|---|---|
| 0AE4:0004 | any but 0100 | Taito Densha de GO! Type 2 Controller | Two handles |
| 0AE4:0004 | 0100 | Multi Train Controller (P5/B8) | Two handles, as a Type 2 |
| 0AE4:0005 | any | Taito Densha de GO! Shinkansen Controller | Two handles |
| 0AE4:0007 | any | Taito Densha de GO! Ryojohen Controller | Two handles, analog brake |
| 0AE4:0101 | 0300, 0400, 0800, 0A00 | Multi Train Controller (P4/B7), (P4/B2-B7), (P5/B7), (P13/B7) | One lever |
| 1C06:77A7 | any | Train Mascon | One lever |

Bind WinUSB to the whole device, `USB\VID_0AE4&PID_0004`,
`USB\VID_0AE4&PID_0005`, `USB\VID_0AE4&PID_0007`, `USB\VID_0AE4&PID_0101` and
`USB\VID_1C06&PID_77A7`. Each has one interface, so no `&MI_00` child exists.
Setting `SDL_HINT_JOYSTICK_HIDAPI_TRAIN` to 0 turns the driver off. The
devices need no request before they report, and SDL asks none of them for a
report descriptor, which they lack.

Every controller is a gamepad. Each handle gets one axis with its notches
spread evenly, so an application gets the notch back from the axis and the
notch layout from the name.

### Two handles

The brake goes to the left trigger, axis 4, and the power to the right
trigger, axis 5, each from -32768 at rest to 32767 at its last notch.
Emergency is the brake's last notch. A handle between notches reports a value
in no table, and the axis keeps its last notch. The Ryojohen's brake is
analog, from 23 at release to D7 at emergency.

A goes to West, B to South, C to East and D to North, Select to Back and
Start to Start. The Ryojohen's Camera goes to West, Announce to South and
Horn to East, with the left doors on the left shoulder and the right doors
on the right one. The horn pedal is Misc1 and the D-pad is hat 0.

### One lever

The lever goes to the left stick's Y, axis 1: -32768 at emergency, 0 at N
and 32767 at the top power notch. The reverser goes to the right stick's Y,
axis 3: -32768 Neutral, 128 Forward and 32767 Backward. The P13/B7 cartridge
has no reverser. A soft goes to West, B to South, C to East, D (Close on the
Train Mascon) to North, Select to Back, Start to Start, ATS to Misc1 and a
hard press of A to Misc2. The D-pad is hat 0.

### Outputs

- `SDL_RumbleJoystick` turns the Type 2's and the Shinkansen's motors on and
  off, the low frequency the left one and the high frequency the right one.
- `SDL_SendJoystickEffect` sends a raw payload: status and function on the
  Type 2 (function 1 and 2 the motors, 3 the door lamp), the 8 output bytes
  on the Shinkansen, and one lamp byte on the Multi Train Controller and the
  Train Mascon (bit 4 the door lamp, the low nibble the signal lamp).
- Closing the joystick turns the motors off and blanks the displays and
  lamps.

## RS-232

Name the port with `mastercontroller` in `SDL_HINT_JOYSTICK_SERIAL`, for
example `COM3=mastercontroller`. The line is 19200 baud, 8N1, with no flow
control and DTR and RTS off, as the PC readers open it. SDL writes one 00
byte after opening, as BVE Trainsim's interface does. The controller sends
an event when a control moves, so the joystick appears with the first event.

The controller's notch settings stay inside it, so the lever uses the fixed
scale of its codes: 9 brake steps with emergency and 8 power steps, on the
left stick's Y. The reverser goes to the right stick's Y as above. B goes to
South, C to East, A to West and S to North. Until the first lever event the
lever reads 0, and until the first reverser event the reverser reads Neutral.
The Master Controller and the Master Controller II send the same events and
share the name "Pony Canyon Master Controller".

## Known limits

- No capture of any report exists. The layouts follow the Train Controller
  Database's tables and OpenBVE's decoders, which agree.
- The Ryojohen's door bits: the database and OpenBVE put the right doors on
  bit 3, and PCSX2 puts the left doors there.
- The Shinkansen's output setup: the database gives 40 09 0301, OpenBVE
  41 09 0201. SDL sends the database's.
- The Train Mascon's brake notches: 5 in OpenBVE and on a modification page,
  8 on its database page. SDL uses 5.
- Whether a hard press of A also sets the soft bit is not documented.
- What the Master Controller does with the 00 byte is not documented, nor
  what it sends at power-on or whether it ever repeats its state.

## Tests

The protocol module is C99 with no SDL runtime and no I/O, and the USB and
serial drivers share it. `test/controller-protocols` runs `testtrain` against
every notch of every table, the buttons, the outputs and the event decoder,
and `test/serial-joystick` runs `testserialmastercontroller` against the
port, in normal and AddressSanitizer builds.
