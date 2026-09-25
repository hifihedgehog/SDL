# Ring Fit Adventure Ring-Con

The Ring-Con carries a right Joy-Con (057E:2007) on its rail and reports one
strain value through it. The Switch driver reads it when
`SDL_HINT_JOYSTICK_HIDAPI_JOYCON_RINGCON` is "1". The hint defaults to "0",
because the Ring-Con is read through the Joy-Con's NFC/IR MCU, which costs
battery. The protocol lives in `src/joystick/hidapi/SDL_hidapi_switch_ringcon_proto.c`,
which `test/controller-protocols` replays.

## What the application sees

- Axis `SDL_GAMEPAD_AXIS_COUNT + 1` (7) of the right Joy-Con's joystick, alone
  or as the right half of a pair, is the raw strain: signed 16-bit, rising as
  the ring is pressed and falling as it is pulled. It reads 0 while no
  Ring-Con polls. The axis stays out of the gamepad mapping.
- Property `SDL.joystick.switch.ringcon` (boolean) is true while the strain
  flows.
- Property `SDL.joystick.switch.ringcon_rest` (number) holds the first strain
  after the strain starts to flow. No source reads a calibration from the
  ring, and rest values differ from ring to ring.

Axis 6 is the NIR camera's value, so every right Joy-Con now has 8 axes.

## Start

Only a right Joy-Con on Bluetooth qualifies. The driver looks for a Ring-Con
when the joystick opens and when the hint turns on, never on a timer. It sends
six subcommands, each 50 ms after the previous one's reply:

| Step | Subcommand | Reply that passes |
|---|---|---|
| 1 | `03 30`, input mode 0x30 | `80 03` |
| 2 | `22 01`, MCU resume | `80 22` |
| 3 | `21 21 01 01`, MCU standby, CRC-8 F3 at byte 48 | byte 14 = 21 |
| 4 | `59`, external device query | `59 00 20` at bytes 14-16 |
| 5 | `5C 06 03 25 06 ...`, external format config | byte 14 = 5C |
| 6 | `5A 04 01 01 02`, external polling | byte 14 = 5A |

Every reply needs byte 13 bit 7. A command without its reply goes again after
600 ms, and 600 ms after the eighth send the start fails. The query is
different: another device ID sends it again after 50 ms, no reply after
150 ms, and after 42 unmatched sends the Ring-Con counts as absent. A Joy-Con
can take several seconds to answer the query.

## Stop

A stop undoes what the start sent, in the order the Switch uses: `5B` if `5A`
went out, a `5C` with format 0 if `5C` went out, then `22 00`. After a failed
start, an absent Ring-Con or the hint turning off, the input mode the driver
uses without the Ring-Con follows. Closing the joystick writes the same
commands and waits for each reply.

From the `5C` of the start until the stop ends, bytes 37-48 of report 0x30 hold
the Ring-Con's data in place of the oldest IMU sample, so the driver posts
two IMU samples per report instead of three.

If no nonzero strain arrives for 2 s while polling, the driver stops and
starts once more. A second silent stretch without any strain in between ends
as absent.

## The MCU

The NIR camera and NFC share the MCU and come first: the Ring-Con stops when
either wants the MCU, and they wait for its stop to finish. It starts again
once they are done.
