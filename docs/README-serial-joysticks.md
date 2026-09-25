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
| `dji` | DJI RC-N1 family | 115200 8N1 | Gamepad |
| `djimavicmini` | DJI Mavic Mini remote | 115200 8N1 | Gamepad |
| `djiphantom3` | DJI Phantom 3 remote | 115200 8N1 | Gamepad |
| `djiphantom2` | DJI Phantom 2 remote | 115200 8N1 | Gamepad |

The Stinger, the JVS players and the DJI remotes get gamepad mappings. The
rest stay joysticks. Every port runs with DTR and RTS on, except that a JVS
port raises RTS only while it sends. The DJI remotes are described in
[README-dji-remotes.md](README-dji-remotes.md), and the I-Force devices in
[README-iforce.md](README-iforce.md).

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
consoles, also disappear after their silence limit. The others have no
silence limit, because they can be silent at rest, so unplugging one from an
adapter that stays connected goes unnoticed.

## Self-identifying ports

A device whose COM port is one of its own USB interfaces is opened without
the hint, by matching its device instance ID against a table in the driver.
In a table row, `?` matches any one character. `SDL_HINT_JOYSTICK_SERIAL_AUTO`
turns that matching off, and a port the hint names keeps the hint's protocol.

| Instance ID | Token | Device |
|---|---|---|
| `USB\VID_2CA3&PID_????&MI_02\` | `dji` | The protocol port of a DJI RC-N1 family remote |

Such a port's joystick GUID carries the USB vendor and product IDs of its
device, the product ID taken from the instance ID.

## Tests

The engine and every protocol module are C99 with no SDL runtime and no I/O.
`test/serial-joystick` replays each module's byte streams against a scripted
port and runs in normal and AddressSanitizer builds.
