# Namco GunCon light guns

Namco's GunCon 2 and GunCon 3 each present one vendor-class USB interface
that no Windows driver claims. This fork reads the GunCon 2, and the EMS LCD
TopGun that shares its ID, through libusb once WinUSB is bound. It does not
read the GunCon 3.

## GunCon 2

| Item | Value |
|---|---|
| ID | 0B9A:016A |
| Bind WinUSB to | `USB\VID_0B9A&PID_016A`, the device |
| Hint | `SDL_HINT_JOYSTICK_HIDAPI_GUNCON` |
| Name | Namco GunCon 2 |
| Type | `SDL_JOYSTICK_TYPE_UNKNOWN`, no gamepad mapping |

SDL sends the mode request the PC tools send, once per connection, before it
reads a report: SET_REPORT to interface 0 with wValue `0200` and the data
`00 00 00 00 00 01`. The gun has no OUT endpoint, so the request goes out on
the control pipe.

| Control | SDL |
|---|---|
| X, Y | Axes 0 and 1, the raw beam counts, clamped to 32767 |
| Trigger, A, B, C, Start, Select | Buttons 0 to 5 |
| D-pad | Hat 0. Opposite directions held together cancel. |

The gun times the picture of a CRT. It needs a 15 kHz CRT and composite sync
connected to the gun, and it cannot see dark areas of the screen. The usable
window of the axes depends on the display, the video mode and the game, so
the application calibrates them. The PC tools start from X 175 to 720 and
Y 20 to 240.

## GunCon 3

The GunCon 3 encrypts its reports with a 256-byte table that only GPL and
unlicensed sources carry, so the fork cannot decode them. SDL does not claim
the gun, and a tool that reads it today keeps it. The protocol module holds
the 8-byte key the PC tools write, the checksum and the layout of a decrypted
report, ready for a table from a source the fork can use.

## Known limits

- No source records what the GunCon 2 reports off screen or with no video
  signal. PCSX2 emulates X 0 and Y 0.
- The TopGun's Linux driver swaps A and B and orders the D-pad differently.
  SDL cannot tell a TopGun from a GunCon 2, so both get the GunCon 2 layout.
- Whether the gun reports before the mode request, what the mode bytes mean,
  and the report rate are not recorded.

## Tests

The protocol module is C99 with no SDL runtime and no I/O.
`test/controller-protocols` runs `testguncon` against constructed GunCon 2
reports, every button word, the GunCon 3 key and three GunCon 3 packets
captured from hardware, in normal and AddressSanitizer builds.
