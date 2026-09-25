# Bluetooth serial (RFCOMM) controllers

This fork reads controllers that pair as classic Bluetooth devices but send
their input over an RFCOMM channel instead of HID. Windows pairs them and
loads no input driver for them. The driver is Windows only.

| Family | Bluetooth name | Connection | Joystick |
|---|---|---|---|
| PowerA MOGA in Mode A (Pocket, Pro, Pro Power, Hero Power) | Starts with "BD&A" or "BDA", or with "MOGA" and holds no "HID", in any case | Serial Port service, then channel 1 | Gamepad, "PowerA MOGA" or the Bluetooth name |
| Zeemote JS1 and JS1 H in joystick mode | Starts with "Zeemote JS1" | Service 8E1F0CF7-508F-4875-B62C-FBB67FD34812, then channel 1 | Gamepad, the Bluetooth name |
| Chainpus BGP100 | Starts with "GAMEPAD" in capitals | Channel 1 | Gamepad, "Chainpus BGP100" |
| Phonejoy Digital and Analog of 2011 | Starts with "Phonejoy", in any case | Channel 1 | Gamepad, "Phonejoy" |

Pair the controller in Windows Settings first. The driver never searches
for new devices. It reads the list of paired devices every 3 seconds,
connects to each one whose name matches, and after a failed connect or a
lost link connects again 3 seconds later, until the device leaves the list.
A device counts as paired when Windows remembers or authenticated it, not
because it is connected. Names starting "Zeemote: SteelSeries", in any case,
are left alone: the SteelSeries FREE works as a HID gamepad.

`SDL_HINT_JOYSTICK_RFCOMM` turns the driver on or off, default on.
`SDL_HINT_JOYSTICK_RFCOMM_MOGA`, `SDL_HINT_JOYSTICK_RFCOMM_ZEEMOTE`,
`SDL_HINT_JOYSTICK_RFCOMM_BGP100` and `SDL_HINT_JOYSTICK_RFCOMM_PHONEJOY`
default to it and override it, as the HIDAPI drivers' hints do.

A joystick appears with the first valid report or key event, so a device
with a matching name that sends something else shows nothing. A lost link
removes the joystick.

## MOGA

At connect SDL sends 43 with the controller id, a poll and the listen
command: 45 and 46 when the name starts with "MOGA", for 14-byte reports
with analog triggers, and 41 and 44 for "BD&A" or "BDA", for 12-byte
reports. After 2 seconds without a report SDL polls. A report after the poll
brings the listen command again, and 2 more seconds of silence end the link.

The player index lights the blue LED: players 0 to 3 are ids 1 to 4, and no
player is id 5, which lights none. A, B, X and Y are South, East, West and
North, L1 and R1 the shoulders, Select Back, Start Start, L3 and R3 the
stick clicks, and the D-pad the D-pad. The triggers are analog in 14-byte
reports and full or released in 12-byte ones. Mode A carries no battery
level.

## Zeemote

SDL sends nothing. A and B are South and East, C is Back and D Start, and
the stick is the left stick. Once the Zeemote reports its battery, the
joystick is on battery with an unknown percentage, since no source gives the
charge curve of its millivolt reading.

## BGP100 and Phonejoy

SDL sends nothing. On the BGP100 A, B, D and C are South, East, West and
North, L and R the shoulders, Start Start, and the D-pad the D-pad. On the
Phonejoy buttons 1, 2, 3 and 4 are North, East, South and West, L1 and R1
the shoulders, L2 and R2 triggers that are full or released, Select Back and
Start Start. Its sticks follow its position frames, and a direction event
holds its stick at full until released.

## Known limits

- No capture of any family's traffic exists. The layouts follow MogaSerial,
  moga-uinput, ZeeClient, zeemouse, zeemoted and android-bluez-ime.
- Whether the MOGA Hero Power and the Pocket answer 45 and 46 is not
  documented.
- The BGP100's L and R labels, the place of its C button and the Phonejoy's
  button positions are not documented. The Phonejoy's R2 pair and its stick
  frames come from untested code.
- The Phonejoy Play of 2013 is a HID gamepad. Its Bluetooth name is not
  documented, and a name starting "Phonejoy" gets connect attempts that
  show no joystick without valid key events.
- Whether a service lookup inside a connect can block is not documented. If
  it does, stopping a device waits for it.

## Tests

The family modules and the link are C99 with no SDL runtime and no I/O.
`test/serial-joystick` runs `testrfcommlink`, `testrfcommmoga`,
`testrfcommzeemote` and `testrfcommbgp100` against them, in normal and
AddressSanitizer builds.
