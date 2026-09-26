# Joysticks on COM ports

Some controllers reach Windows only as a COM port: RS-232 devices behind a
USB-serial adapter, an RC receiver's serial pin on a USB-UART cable, JVS
arcade boards on a USB-RS485 adapter, and flight panels with a USB-serial
chip inside. Windows loads a port driver and nothing reads the protocol. The
serial joystick driver, for Windows, reads them. It opens only the ports the
application names, because a USB-serial adapter carries its own maker's IDs
and says nothing about the device behind it.

## Naming the ports

`SDL_HINT_JOYSTICK_SERIAL` holds a comma separated list of `PORT=PROTOCOL`
entries:

    COM3=spaceball,COM4=magellan

`PORT` is `COMn`, or the start of a device instance ID as Device Manager
shows it under "Device instance path", for example
`FTDIBUS\VID_0403+PID_6001+A1B2C3D4A`. An instance ID follows an adapter to a
new COM number. Spaces around entries are ignored, an entry that cannot be
used is skipped with a log message, and a port named twice keeps its last
entry. The hint can change at any time: ports that leave the list close and
their joysticks go, new ports open, and a port whose protocol changed starts
over.

The driver holds each port exclusively. A port that is absent or held by
another program is retried every second, and at once when Windows reports a
COM port arriving.

| Token | Devices | Line | SDL device |
|---|---|---|---|
| `spaceball` | SpaceTec Spaceball 1003, 2003, 2003B, 2003C, 3003, 3003C, 4000 FLX | 9600 8N1 | 6 axes, 12 buttons |
| `spaceorb` | SpaceTec SpaceOrb 360, SpaceBall Avenger | 9600 8N1 | 6 axes, 7 buttons |
| `magellan` | Magellan and SpaceMouse, Spaceball 5000, CadMan | 9600 8N2 | 6 axes, 12 buttons |
| `stinger` | Gravis Stinger | 1200 8N1 | Gamepad |
| `warrior` | Logitech WingMan Warrior | 1200 7N2, then 4800 8N1 | Flight stick: 3 axes, 4 buttons, a hat, the dial as a ball |
| `cyberman` | Logitech CyberMan | 1200 7N1, then 4800 8N1 | 6 axes, 3 buttons, rumble |
| `zhenhua` | Zhen Hua 5-byte RC transmitters | 19200 8N1 | 4 axes |
| `ibus` | FlySky FS-iA6B receiver's i-BUS output | 115200 8N1 | 14 axes |
| `jvs` | JVS I/O boards | 115200 8N1 | Arcade stick per player, up to 4 |
| `vrinsight` | VRinsight CDU II, MCP Combo I | 115200 8N1 | 70 or 72 buttons |
| `kettler` | Kettler ergometers with an RS-232 port | 9600 8N1 | 5 axes |
| `iforce` | I-Force wheels and joysticks: the Boeder Force Feedback Wheel, the Trust Force Feedback Race Master, and others by the IDs they report | 38400 8N1 | Wheel or flight stick |
| `mastercontroller` | Pony Canyon Master Controller and Master Controller II | 19200 8N1 | Gamepad: the lever and the reverser |
| `dji` | DJI RC-N1 family | 115200 8N1 | Gamepad |
| `djimavicmini` | DJI Mavic Mini remote | 115200 8N1 | Gamepad |
| `djiphantom3` | DJI Phantom 3 remote | 115200 8N1 | Gamepad |
| `djiphantom2` | DJI Phantom 2 remote | 115200 8N1 | Gamepad |
| `bio2` | Konami BIO2 board, as `SDL_HINT_JOYSTICK_KONAMI_BIO2_MODE` names its cabinet | 115200 8N1 | As `bio2iidx` or `bio2sdvx`, no joystick without a mode |
| `bio2iidx` | Konami BIO2 board of a beatmania IIDX cabinet | 115200 8N1 | 7 axes, 21 buttons |
| `bio2sdvx` | Konami BIO2 board of a SOUND VOLTEX cabinet | 115200 8N1 | 2 axes, 14 buttons |
| `kfca` | Konami KFCA board of SOUND VOLTEX cabinets before the BIO2 | 57600 8N1 | As `bio2sdvx`: 2 axes, 14 buttons |
| `panb` | Konami PANB key panel of Nostalgia cabinets | 57600 8N1, or 115200 | 28 buttons |
| `rvol` | Konami RVOL board of MUSECA cabinets | 57600 8N1, or 115200 | 5 axes, 6 buttons |
| `mdxf` | Konami MDXF stage of DDR A cabinets | 115200 8N1 | A dance pad per player, up to 2 |

The Stinger, the JVS players, the DJI remotes, the Master Controllers and the
MDXF's dance pads get gamepad mappings. The rest stay joysticks. Every port
runs with DTR and RTS on, except that a JVS port raises RTS only while it
sends and a Master Controller port runs with both off. The DJI remotes are
described in [README-dji-remotes.md](README-dji-remotes.md), the I-Force
devices in [README-iforce.md](README-iforce.md), the Master Controllers in
[README-train.md](README-train.md), and the Konami boards in
[README-arcade-io.md](README-arcade-io.md).

## Devices that need care

- CyberMan: after a reset it is a serial mouse, so Windows may install its
  serial mouse driver on that port. Disable that device first. The driver
  resets the CyberMan with RTS and switches it to its 3D mode. Only the
  tactile motor needs batteries or the AC adapter.
- JVS: the driver is the bus master. It resets the boards, assigns addresses
  until one goes unanswered, identifies each board and polls them in turn.
  The adapter must switch direction on RTS or by itself, and the bus needs
  120 ohms across A and B at the host end if the adapter has none. A coin
  pulses the player's coin button for 100 ms per coin, and TEST is player 1's
  guide button.
- i-BUS: wire the cable's RX, ground and +5 V to the receiver's i-BUS, ground
  and power pins. Channels are masked to 12 bits and scaled from 1000 to
  2000. Switch channels are axes, for the application to threshold.
- VRinsight: every panel message presses its button for 100 ms. The MCP
  Combo II and the M-Panel are identified and logged but get no joystick,
  because their messages are not published.
- Kettler: only RX, TX and ground are wired. The axes are the raw readings:
  cadence, power on the brake, speed in 0.1 km/h, heart rate and target
  power.
- I-Force: the driver asks `O` once a second, 20 times at most, and with no
  answer logs it and opens no joystick. The `M` and `P` replies name the
  device and pick its layout.
- Master Controller: the driver writes one 00 byte after opening. The
  controller sends an event when a control moves, so its joystick appears
  with the first event.
- BIO2: Windows binds its own USB serial driver to the board, which gives a
  COM port and no game input. The driver resets the board's ACIO bus: 525
  bytes of 00, a line break for 1450 ms, 1200 ms in which it drops what it
  reads, then a single AA byte every 10 ms until one comes back, starting
  over after 500. It enumerates the nodes, logs each node's version, starts
  each node and takes the BI2A node, the highest if there are several. The
  node does not say which cabinet it serves, so
  `SDL_HINT_JOYSTICK_KONAMI_BIO2_MODE`, or the token `bio2iidx` or
  `bio2sdvx`, names it, and without a mode no joystick opens. The driver
  sends the node its init byte, 2D for IIDX and 3B for SOUND VOLTEX, then
  polls it with every lamp off. The IIDX joystick's axes are the two
  turntables, as (position - 128) x 256, and sliders 1 to 5, as value x
  4369 - 32768. Its buttons are P1 keys 1 to 7, P2 keys 1 to 7, P1 START, P2
  START, VEFX, EFFECT, Test, Service and Coin. The SOUND VOLTEX joystick's
  axes are knobs L and R, and its buttons BT-A, BT-B, BT-C, BT-D, FX-L, FX-R,
  START, EX1, EX2, headphone, recorder, Test, Service and Coin. SOUND VOLTEX
  also gets the amplifier command with four bytes of 00, as bemanitools
  sends at start-up. In bemanitools' API 0 is the loudest amplifier level,
  and its BIO2 driver notes that the BIO2 does not read these bytes as the
  KFCA does. The bit positions come from bemanitools' structures, and no
  capture confirms them.
- KFCA, PANB, RVOL and MDXF: Konami's ACIO boards on RS-232 get the BIO2's
  bus reset and bring-up on the port the hint names. The KFCA's watchdog is
  armed with 6000, a value no source gives a unit for, and its amplifier
  gets four bytes of 00, the level bemanitools' KFCA driver calls the
  loudest.
  It gives the SOUND VOLTEX joystick of `bio2sdvx`, whose EX buttons stay
  released, since the KFCA has none. The RVOL gets the expand mode C0 and
  gives MUSECA's five spinners as axes, position x 256 - 32768, then their
  presses and the pedal as buttons. The MDXF stage gives a dance pad per
  side, player 1 on the first MDXF node, with the arrows as buttons that the
  gamepad mapping puts on the D-pad, so a jump holds two opposite arrows. No
  source records the PANB's or the RVOL's rate: the driver tries 57600, and
  after each bring-up that fails switches between it and 115200. Test and
  Service on the KFCA follow bemanitools' SOUND VOLTEX API, Test on bit 5
  and Service on bit 4, and its ACIO header has them the other way around.
- PANB: after its start command the panel streams its keys without being
  asked, and a panel left unread stops, then ignores a reset until it is
  power cycled. The driver reads the port the whole time it is open, and
  resets the panel before the port closes, which holds the close for about
  1.5 seconds. A stream silent for 600 ms starts the bus over. Each key reads
  0 to 15, which bemanitools calls its velocity, and is pressed when not 0.

## Output

`SDL_RumbleJoystick` drives the CyberMan's tactile motor.
`SDL_SendJoystickEffect` sends, for a JVS player, up to 8 bytes of general
purpose outputs to that player's board, for a VRinsight panel one 8-byte
message, such as `SPD250` and two 00 bytes for the speed display, for a
Kettler a 2-byte little-endian power target in watts, and for an I-Force
device one whole force feedback command, which the driver frames.

## Losing and finding devices

A read or write error closes the port, removes its joysticks and retries the
open every second. When the device answers its start-up again, it appears as
a new joystick. Devices that stream or answer polls, the Zhen Hua
transmitters, the i-BUS receiver, JVS boards, VRinsight panels and Kettler
consoles, also disappear after their silence limit. A BIO2, KFCA or RVOL
disappears after three failed polls in a row, an MDXF after three failed
polls of one side, and a PANB after 600 ms without a frame, and the driver
then resets its bus. The others have no silence limit, because they can be
silent at rest, so unplugging one from an adapter that stays connected goes
unnoticed.

## Self-identifying ports

A device whose COM port is one of its own USB interfaces is opened without
the hint, by matching its device instance ID against a table in the driver.
In a table row, `?` matches any one character. `SDL_HINT_JOYSTICK_SERIAL_AUTO`
turns that matching off, and a port the hint names keeps the hint's protocol.

| Instance ID | Token | Device |
|---|---|---|
| `USB\VID_2CA3&PID_????&MI_02\` | `dji` | The protocol port of a DJI RC-N1 family remote |
| `USB\VID_1CCF&PID_804C` | `bio2` | Konami BIO2 board |
| `USB\VID_1CCF&PID_8040` | `bio2` | Konami BIO2 board |

The BIO2 rows match the device and any interface of it, as bemanitools'
search by ID does. Such a port's joystick GUID carries the USB vendor and
product IDs of its device, the product ID taken from the instance ID when
its row gives none.

`SDL_HINT_JOYSTICK_KONAMI_ACIO` turns the ACIO protocols off on every port,
named or matched.

## Tests

The engine and every protocol module are C99 with no SDL runtime and no I/O.
`test/serial-joystick` replays each module's byte streams against a scripted
port and runs in normal and AddressSanitizer builds.
