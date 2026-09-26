# Arcade I/O boards

The I/O boards of these Namco and Konami arcade cabinets give Windows no game
input. The Namco USIO, the Konami P3IO and the Konami P4IO are USB boards
that no stock Windows driver serves, and this fork reads them through libusb
once WinUSB is bound, on Windows only. The Konami BIO2 is a COM port on
Windows' own USB serial driver, and Konami's older ACIO boards sit on RS-232
ports. The serial joystick driver reads both. SDL loads no driver and no
firmware.

| Board | Cabinets | ID | Hint | SDL devices |
|---|---|---|---|---|
| Namco USIO | System 357 and 369: Taiko no Tatsujin, Tekken | 0B9A:0910, 0B9A:0900 | `SDL_HINT_JOYSTICK_HIDAPI_USIO` | Two drum kits, or four arcade sticks |
| Konami BIO2 | beatmania IIDX, SOUND VOLTEX | 1CCF:804C, 1CCF:8040 | `SDL_HINT_JOYSTICK_KONAMI_ACIO`, `SDL_HINT_JOYSTICK_KONAMI_BIO2_MODE` | One joystick, once the mode hint names the cabinet |
| Konami KFCA, PANB, RVOL, MDXF | SOUND VOLTEX before the BIO2, Nostalgia, MUSECA, DDR A | RS-232, none | `SDL_HINT_JOYSTICK_SERIAL`, `SDL_HINT_JOYSTICK_KONAMI_ACIO` | One joystick, or two dance pads for the MDXF |
| Konami P3IO | DDR SuperNova 2, DDR X | 1CCF:8008 | `SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P3IO` | Two dance pads |
| Konami P4IO | jubeat, DDR White | 1CCF:8010 | `SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO` | One joystick of buttons |

The three HIDAPI hints default to `SDL_HINT_JOYSTICK_HIDAPI`, and the ACIO
hint defaults to on. On Linux and macOS SDL leaves the USB boards alone. See
[README-vendor-usb.md](README-vendor-usb.md) for the vendor USB path and
[README-serial-joysticks.md](README-serial-joysticks.md) for the COM port
driver.

SDL does not read Konami's EZ-USB boards, the C02/D01 IO and the USBIO2 of
beatmania IIDX and pop'n music, because they run only after the host loads a
Konami firmware image.

## Namco USIO

The USIO is the I/O board of Namco's System 357 and 369 cabinets. The host
reads its inputs as registers, through a command protocol on vendor
interface 0. RPCS3, TaikoZucchini and ITAIKO-firmware implement the board's
side of that protocol for the games, and SDL reads the layouts they serve.

| Item | Value |
|---|---|
| ID | 0B9A:0910, "USIO PCB rev00". 0B9A:0900, "H050 USJ(C) PCB rev00", only when its identification block names a USIO |
| Bind WinUSB to | The device, `USB\VID_0B9A&PID_0910` or `USB\VID_0B9A&PID_0900` |
| Hints | `SDL_HINT_JOYSTICK_HIDAPI_USIO`, and `SDL_HINT_JOYSTICK_HIDAPI_USIO_LAYOUT`: `taiko` (default) or `tekken` |
| Names | Namco USIO Taiko Drum P1 and P2, or Namco USIO Tekken P1 to P4 |
| Type | Taiko: `SDL_JOYSTICK_TYPE_DRUM_KIT`, no gamepad mapping. Tekken: `SDL_JOYSTICK_TYPE_ARCADE_STICK`, each a gamepad |

The layout hint names the game whose register map SDL reads. `tekken`, in any
case, selects Tekken, and any other value selects Taiko. SDL reads the hint
when the board opens, so a change applies when the board next connects, or
after `SDL_HINT_JOYSTICK_HIDAPI_USIO` is turned off and on again. Player 1 is
player index 0, player 2 index 1, and so on.

SDL only reads. The vendor rule names bulk OUT 0x01 and bulk IN 0x82 of
interface 0, and the interrupt endpoint 0x83 that the emulators describe is
never read. A command is 6 bytes: 10 for a read of channel 0, a check byte,
E0 for every register SDL reads, then the register and the length, each
16-bit little-endian. The first command reads the identification block,
register 1800 for 0x180 bytes, `10 E0 00 18 80 01`. After it SDL reads the
layout's input block over and over, register 1080 for 0x60 bytes for Taiko,
`10 E0 80 10 60 00`, or register 1000 for 0x180 bytes for Tekken,
`10 E0 00 10 80 01`. Each command goes out from SDL's update as soon as the
last reply is whole, whether or not a joystick is open. SDL never writes a
register and never addresses another channel.

The joysticks appear once the identification block has arrived. A block that
does not begin with `NBGI.` is logged. SDL uses a 0910 board anyway, but
sends a 0900 board nothing more and opens no joystick for it.

A reply is due 100 ms after its command went out. After a missed reply, a
failed write, or bytes that no command asked for, the next command waits
until no byte has come for 100 ms. Three failed reads in a row while polling,
about half a second without an answer, remove the joysticks, and SDL starts
over with the identification block and a new coin baseline.

Taiko gives one joystick per drum:

| SDL | Control |
|---|---|
| Axes 0 to 3 | Side left (Ka), center left (Don), center right (Don) and side right (Ka) pads: the 16-bit sensor value halved, 0 to 32767 |
| Buttons 0 to 3 | The same four pads, pressed from sensor value 0C00 up |
| Buttons 4 to 9, P1 only | Enter, Up, Down, Service, Test, Coin |

Coin is pressed for one poll per coin, with a released poll between two
coins, and at most 16 coins wait their turn. The first block after the start
sets the counter's baseline and presses nothing. A counter that goes back, as
after a reset, presses nothing either. Test is the level the block carries.

Tekken gives one joystick per player:

| SDL | Control |
|---|---|
| Hat 0 | The stick. Opposite directions held together cancel. |
| Buttons 0 to 4 | Buttons 1 to 5 |
| Button 5 | Enter |
| Buttons 6 to 8, P1 and P3 only | Test, Service, Coin |

P1 carries the coin counter of players 1 and 2, and P3 that of players 3 and
4. The gamepad mapping follows RPCS3's default pad assignment: the stick on
the D-pad, button 1 on West, button 2 on North, button 3 on South, button 4
on East, button 5 on the right shoulder and Enter on Start. Test, Service and
Coin stay unmapped. The two layouts share one USB ID, so byte 15 of the
joystick GUID carries the layout, 01 for Taiko and 02 for Tekken, and the
drums keep no mapping.

SDL drives no output of the USIO.

## Konami ACIO bus

ACIO is the node bus of Konami's arcade I/O boards. The BIO2 carries it over
USB, and older boards over RS-232. The serial joystick driver runs the bus on
the board's COM port, 8N1 with DTR and RTS on, at the board's rate.

A frame is AA, then the node address, the command as a big-endian 16-bit
word, a sequence number, the payload length, the payload and a checksum, the
low byte of the sum of the bytes from the address on. A byte of AA or FF
inside a frame, the checksum included, travels as FF and its complement. A
reply sets bit 7 of the address.

The driver brings the bus up as bemanitools does. It writes 525 bytes of 00,
holds a line break for 1450 ms, drops what it reads for 1200 ms, then sends a
single AA every 10 ms until an AA comes back, and starts over after 500 with
no answer. Once an AA comes back, it enumerates the nodes, reads each node's
version and logs it, and starts each node. The sequence counts from 1, and a
reply is due 200 ms after its request left. A failure before the bus is up
starts it over, and so does a bus with no node or more than 16. A board
answers about 3 seconds after its port opens.

Each node's line in the debug log follows the port's name and token. For the
BIO2's node, with the version arcade-docs lists, it reads:

    ACIO node 1: BI2A, type 0D060000, version 1.2.1, Nov 27 2017 14:48:52

Setting `SDL_HINT_JOYSTICK_KONAMI_ACIO` to 0 stops every ACIO protocol, on
every port, named or matched. The hint can change at any time.

## Konami BIO2

The BIO2 of beatmania IIDX and SOUND VOLTEX cabinets connects over USB, and
Windows installs its own serial driver for it, which gives a COM port and no
game input. SDL opens that port by the board's IDs, through the serial
joystick driver's table of ports that identify themselves.

| Item | Value |
|---|---|
| ID | 1CCF:804C, 1CCF:8040. Which board revision takes which ID is not recorded. |
| Port | The COM port of Windows' USB serial driver, matched by the device instance ID `USB\VID_1CCF&PID_804C` or `USB\VID_1CCF&PID_8040`, of the device or any interface of it |
| Line | 115200 baud, 8N1, DTR and RTS on |
| Hints | `SDL_HINT_JOYSTICK_KONAMI_ACIO`, and `SDL_HINT_JOYSTICK_KONAMI_BIO2_MODE`: `iidx` or `sdvx` |
| Tokens | `bio2`, `bio2iidx` and `bio2sdvx` in `SDL_HINT_JOYSTICK_SERIAL` |
| Names | Konami BIO2 IIDX, Konami SOUND VOLTEX |
| Type | `SDL_JOYSTICK_TYPE_UNKNOWN`, no gamepad mapping |

The BI2A node on the board does not report which cabinet it serves, so
`SDL_HINT_JOYSTICK_KONAMI_BIO2_MODE` names it, `iidx` or `sdvx` in any case.
Without a mode the driver brings the bus up, logs each node and opens no
joystick. The mode applies to the board's own port and to any port that
`SDL_HINT_JOYSTICK_SERIAL` names with `bio2`, and a change restarts those
ports. The tokens `bio2iidx` and `bio2sdvx` name the cabinet themselves. A
port that `SDL_HINT_JOYSTICK_SERIAL` names keeps that hint's protocol. With
`SDL_HINT_JOYSTICK_SERIAL_AUTO` set to 0, SDL opens the board's port only when
that hint names it. SDL holds the port it opens, so a game cannot use the
BIO2 while SDL does.

Once the bus is up, the driver takes the node whose product code is BI2A, the
highest address if several are. It sends the node its init command, 0100
with one byte, 2D for IIDX or 3B for SOUND VOLTEX. SOUND VOLTEX then gets the
amplifier command 0128 with four bytes of 00, as bemanitools sends at
start-up. Then the driver polls: 0152 with 48 bytes of 00 for IIDX, answered
with 46 bytes, or 0113 with 40 bytes of 00 for SOUND VOLTEX, answered with
16. The output bytes of every poll stay 00, with every lamp off. Each reply
sends the next poll, and the joystick appears with the first reply. An init
or amplifier command that gets no answer starts the bus over, and so do three
failed polls in a row, which also remove the joystick. A board with no BI2A
node gets no joystick, and the driver logs why.

beatmania IIDX:

| SDL | Control |
|---|---|
| Axis 0 | P1 turntable, (position - 128) x 256, the position in 1/256 turn |
| Axis 1 | P2 turntable, the same |
| Axes 2 to 6 | Sliders 1 to 5, value x 4369 - 32768, the value from 0 at the bottom to 15 at the top |
| Buttons 0 to 6 | P1 keys 1 to 7 |
| Buttons 7 to 13 | P2 keys 1 to 7 |
| Buttons 14 to 20 | P1 START, P2 START, VEFX, EFFECT, Test, Service, Coin |

SOUND VOLTEX:

| SDL | Control |
|---|---|
| Axes 0, 1 | Knob L and knob R, the 10-bit position x 64 - 32768 |
| Buttons 0 to 13 | BT-A, BT-B, BT-C, BT-D, FX-L, FX-R, START, EX1, EX2, headphone, recorder, Test, Service, Coin |

`SDL_RumbleJoystick` and `SDL_SendJoystickEffect` are unsupported.

## ACIO boards on RS-232 (KFCA, PANB, RVOL, MDXF)

These boards sit on a PC serial port or a USB-serial adapter, which says
nothing about the board behind it, so only `SDL_HINT_JOYSTICK_SERIAL` opens
them, with the tokens below. Each gets the bus reset and bring-up above, and
`SDL_HINT_JOYSTICK_KONAMI_ACIO` turns them off as it turns off the BIO2. The
driver finds each board's node by the node type in its version reply.

| Token | Board | Node type | Cabinet | Line | Name |
|---|---|---|---|---|---|
| `kfca` | KFCA | 09060000 | SOUND VOLTEX before the BIO2 | 57600 8N1 | Konami SOUND VOLTEX |
| `panb` | PANB key panel | 090E0000 | Nostalgia | 57600 8N1, or 115200 | Konami Nostalgia Panel |
| `rvol` | RVOL | 09060001 | MUSECA | 57600 8N1, or 115200 | Konami MUSECA |
| `mdxf` | MDXF stage | 09070000 | DDR A | 115200 8N1 | Konami DDR Stage P1 and P2 |

Every line runs with DTR and RTS on. The joysticks are
`SDL_JOYSTICK_TYPE_UNKNOWN` with no gamepad mapping, except the MDXF's two,
which are `SDL_JOYSTICK_TYPE_DANCE_PAD` with a mapping. None has a player
index. The driver takes no output request, and every poll carries its output
bytes as 00, with every lamp off. An ICCA card reader on the same bus is
enumerated and started with the other nodes, and SDL reads nothing from it.

No source records the PANB's or the RVOL's rate. The driver starts at 57600,
the KFCA's rate, and after each bring-up that fails it switches between 57600
and 115200 and logs the change. A rate that brought the bus up stays through
later restarts, and a port that opens again starts at 57600.

The KFCA and the RVOL start the bus over after three failed polls in a row,
and the MDXF after three failed polls of one side. Each restart removes the
board's joysticks. A start-up command that gets no answer starts the bus over
too. A board with no node of its type gets no joystick, and the driver logs
why.

### KFCA

The driver takes the highest node of type 09060000, as bemanitools takes the
last KFCA. It arms the node's watchdog with 0120 and the big-endian word
6000, a value no source gives a unit for, and sets the amplifier with 0128
and four bytes of 00. Then it polls with 0113 and 24 bytes of 00, answered
with 16 bytes, each reply sending the next poll. The joystick appears with
the first reply.

The KFCA gives the joystick of `bio2sdvx`, with the same axes and buttons, so
both boards of the game read alike. It has no EX buttons, and buttons 7 and 8
stay released. Knobs L and R are reply bytes 0-1 and 2-3, bits 15-6 of each
big-endian word. Byte 9 holds BT-C, BT-B, BT-A, START, recorder and
headphone in bits 0 to 5, byte 11 FX-R, FX-L and BT-D in bits 3 to 5, and
byte 1 Coin in bit 2, Service in bit 4 and Test in bit 5.

### PANB

The Nostalgia panel is four nodes, and the first node of type 090E0000 takes
the commands. After bring-up the driver sends it the start, 0115 with 04,
which nothing answers. The panel then streams 0110 frames of 16 bytes without
being asked, until a reset stops it. The driver takes them from any address
and with any sequence, as bemanitools does, and the joystick appears with the
first frame.

| SDL | Control |
|---|---|
| Buttons 0 to 27 | Keys 1 to 28 |

Frame bytes 2 to 15 hold the keys as nibbles: key 2i+1 in the high nibble of
byte 2+i, and key 2i+2 in the low nibble. A key reads 0 to 15, which
bemanitools calls its velocity, and is pressed when not 0.

A panel whose stream goes unread stops sending and then ignores a reset until
it is power cycled, so the driver reads the port the whole time it is open. A
stream silent for 600 ms starts the bus over, which resets the panel and
sends the start again. Once the start has gone out, the driver resets the
panel again before the port closes: the zeros and the 1450 ms break go out,
and the port closes when the break ends. Closing the port waits for that,
about 1.5 seconds and never more than 3.

### RVOL

The RVOL is a KFCA variant, whose product code reads KFCA and whose type is
09060001. The driver takes the highest node of that type, sets its expand
mode with 0114 and C0, then polls with 0112 and 32 bytes of 00, answered with
23 bytes, each reply sending the next poll. The joystick appears with the
first reply.

| SDL | Control |
|---|---|
| Axes 0 to 4 | Spinners 1 to 5, reply bytes 16 to 20, position x 256 - 32768 |
| Buttons 0 to 4 | Spinner 1 to 5 presses |
| Button 5 | Pedal |

The presses are active high and the pedal active low. Byte 9 bit 3 is
spinner 1's press and bit 0 spinner 2's, byte 11 bit 3 spinner 3's, and
byte 1 bits 2 and 4 spinners 4 and 5. Byte 1 bit 3 is the pedal.

### MDXF

Each side of the DDR A stage is an MDXF node. The driver takes the nodes of
type 09070000 in address order, the first for player 1 and the second for
player 2, as p4io-mdxfdrv reads them, and logs a third without reading it. It
polls the two in turn, 0110 with no payload, answered with 3 bytes, each
reply sending the next poll. The board also has an automatic mode that sends
frames without pause, which overran p4io-mdxfdrv's reader, and SDL never
starts it. Each player's joystick appears with its node's first reply.

| Button | Control |
|---|---|
| 0 | Up |
| 1 | Down |
| 2 | Left |
| 3 | Right |

Byte 0 holds the four down sensors in its low nibble and the four up sensors
in its high nibble, byte 1 right and left the same way, and an arrow is
pressed when any of its sensors is. The arrows are buttons, so a jump holds
two opposite arrows at once, and the gamepad mapping puts them on the D-pad.

## Konami P3IO

The P3IO of the DDR SuperNova 2 and DDR X cabinets is a USB board for which
Windows has no driver without Konami's own. SDL reads it through libusb once
WinUSB is bound.

| Item | Value |
|---|---|
| ID | 1CCF:8008 |
| Bind WinUSB to | The device, or on a composite device the interface children that hold endpoints 0x83, 0x02 and 0x81 |
| Hint | `SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P3IO` |
| Names | Konami P3IO DDR P1 at player index 0, Konami P3IO DDR P2 at player index 1 |
| Type | `SDL_JOYSTICK_TYPE_DANCE_PAD`, each a gamepad |

The inputs come on interrupt IN 0x83 of interface 0, which the libusb backend
reads. The board takes commands on bulk OUT 0x02 and answers on bulk IN 0x81.
The driver runs those itself, from a thread of its own, on the handle the
libusb backend holds. It finds 0x02 and 0x81 on any interface of the active
configuration and claims that interface when it is not interface 0.

A command frame is AA, then a length byte that counts the bytes after it, a
4-bit sequence, the command and its payload. AA and FF inside a frame travel
as FF and the byte's complement, and there is no checksum. The driver sends
INIT, `AA 02 00 2F`, then SET WATCHDOG with 00, `AA 03 01 05 00`, which
turns the board's watchdog off. Then it sends GET VERSION, `AA 02 02 01`
with the sequence counting on, 1000 ms after each reply. A reply must echo its
request's sequence and command. No reply within 1000 ms, a reply of the
wrong length, an INIT status other than 00 or a failed transfer ends the
session, and 1000 ms later INIT goes out again with sequence 0. While the
session is up the board gets a command at least every 2 seconds, and after
a failure INIT follows its last command within 3 seconds. A board whose
watchdog stayed on resets 5 to 7 seconds after its last command, so it does
not reset while its commands go through.

The two joysticks appear once the board has answered INIT and SET WATCHDOG,
and they leave when an exchange fails. They come back as new joysticks when
the board answers again.

| Button | Control |
|---|---|
| 0 | Menu start |
| 1 to 4 | Pad up, down, left, right |
| 5, 6 | Menu left, menu right |
| 7, 8 | Menu up, menu down, on the HD cabinet |
| 9 to 11, P1 only | Test, Service, Coin |

P1 has 12 buttons and P2 has 9, and neither has an axis or a hat. The pad
arrows are buttons, so a jump holds two opposite arrows at once. Both
joysticks share the device's GUID and one mapping: the pad arrows on the
D-pad, menu start on Start, menu left and right on the shoulders, menu up on
North and menu down on South. Test, Service and Coin stay unmapped.

A report is 12 bytes. Byte 0 is 80, and bytes 1 to 3 are active low. It comes
whole, or as three 4-byte transfers of which the first starts with 80. The HD
cabinet's menu up and down bits press nothing until a report shows them
released, since OpenITG saw them read as held through libusbK and only the HD
cabinet has those buttons. The driver sends no lamp output.

## Konami P4IO

The P4IO of jubeat and the DDR White cabinet is a USB board that bemanitools
reaches only through Konami's driver. p4io-mdxfdrv drives it through libusb on
Linux, tested with the ADE-704A and ADE-6291 systems. SDL reads it through
libusb once WinUSB is bound.

| Item | Value |
|---|---|
| ID | 1CCF:8010 |
| Bind WinUSB to | The device, or `USB\VID_1CCF&PID_8010&MI_00` if Windows lists it as composite |
| Hints | `SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO`, and `SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO_LAYOUT`: `raw` (default), `jubeat` or `ddr` |
| Names | Konami P4IO, Konami jubeat or Konami P4IO DDR, by layout |
| Type | `SDL_JOYSTICK_TYPE_UNKNOWN`, no gamepad mapping, no player index |

Interface 0 holds three endpoints in this order: bulk OUT and bulk IN, which
carry commands, and interrupt IN, which sends a 16-byte input report with no
request. No source records their addresses, so the vendor rule takes the
first interrupt IN endpoint and passes over the bulk one. The joystick
appears as soon as the board connects.

From a thread of its own, the driver sends INIT, `AA 00 00 00`, and GET
DEVICE INFO, `AA 01 01 00`, once each, as bemanitools does, and reads one
reply after each, with 1000 ms for every transfer. It only logs the answer,
the product code, the version and when the firmware was built, since the
input reports need neither command. It sends the commands only when
interface 0 holds exactly those three endpoints in that order. No keep-alive
is documented, and SDL sends none. SDL sends the board no output.

The layout hint is matched in any case, and any other value selects raw. SDL
reads it when the board connects, so a change applies when the board next
connects, or after `SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO` is turned off and on
again. The first 4 bytes of a report are a 32-bit little-endian word, and a
read shorter than 16 bytes changes nothing.

raw: 32 buttons, button N for bit N of the word as the board sends it.

jubeat: 18 buttons. Panel 1 is at the top left and panel 16 at the bottom
right. The panels are active low, Test and Service active high.

| Buttons | Control | Word bits |
|---|---|---|
| 0 to 3 | Panels 1 to 4 | 5, 1, 13, 9 |
| 4 to 7 | Panels 5 to 8 | 6, 2, 14, 10 |
| 8 to 11 | Panels 9 to 12 | 7, 3, 15, 11 |
| 12 to 15 | Panels 13 to 16 | 16, 4, 20, 12 |
| 16, 17 | Test, Service | 28, 25 |

ddr: 13 buttons, all active high. The stage arrows are on another board, the
MDXF.

| Buttons | Control | Word bits |
|---|---|---|
| 0 to 4 | P1 menu OK, up, down, left, right | 0 to 4 |
| 5 to 9 | P2 menu OK, up, down, left, right | 8 to 12 |
| 10 to 12 | Coin, Service, Test | 24, 25, 28 |

## Known limits

- No capture of a real USIO exists. Every layout, the reply lengths and the
  endpoints are what the emulators serve to the games, and their register
  maps overlap, so one of them may not match a real board. SDL uses a reply
  only once it holds every byte its read asked for. A board that answers
  with fewer bytes gets no joysticks, or loses them after three reads.
- The USIO's hit threshold, 0C00, lies midway between the emulators' idle
  value 0 and the weakest hit any of them sends, 1800. A real sensor's scale
  and noise are not recorded. The emulators latch Test and turn it over on
  each press, and no source says what a real board sends.
- Byte 1 of a USIO read follows the rule RPCS3 and ITAIKO-firmware check on
  writes, since no source checks it on reads. Whether the board needs an init
  command before its inputs update is not recorded, and SDL sends none.
- Only the game's probe ties 0B9A:0900 to the USIO protocol, so SDL reads a
  0900 board only when its identification block begins with `NBGI.`.
- No source contains a capture of a BI2A exchange. The BIO2's bit positions
  follow the declared order of bemanitools' bit-fields, as its MinGW build
  lays them out.
- What the BIO2's init bytes 2D and 3B mean is not recorded. bemanitools says
  only that without 2D nothing but the 14 keys of an IIDX cabinet works.
- In bemanitools' amplifier API 0 is the loudest level, and its BIO2 driver
  notes that the BIO2 does not read the amplifier bytes as the KFCA does. What
  the BIO2 does with four bytes of 00 is not recorded.
- Whether the BI2A node's version data can tell IIDX from SOUND VOLTEX is not
  recorded. bemanitools also defines a DDR layout for the BI2A node that no
  code uses, and SDL has no mode for it. arcade-docs lists a second BIO2
  firmware family, BI2X, whose protocol no source documents.
- No source contains a capture of the KFCA, the PANB, the RVOL or the MDXF,
  and no source records the PANB's or the RVOL's rate.
- The KFCA's Test and Service follow bemanitools' SOUND VOLTEX API, Test on
  bit 5 and Service on bit 4 of byte 1. bemanitools' ACIO header has them
  the other way around. One of the two buttons is swapped until a capture
  settles it.
- SDL arms the KFCA's watchdog, as bemanitools' generic KFCA driver does.
  bemanitools' SOUND VOLTEX driver for real boards sends only the amplifier
  command. SDL sends the amplifier four bytes of 00, as both do unless
  configured otherwise, and bemanitools' KFCA driver calls 0 the highest
  volume.
- bemanitools calls a PANB key's value its velocity and gives it no scale.
  SDL reports any value from 1 to 15 as a press. Which way the RVOL's
  spinners count, and whether their values wrap, is not recorded, and SDL
  reports the byte as it comes. The MDXF's byte 2 is not known.
- The P3IO's Test and Service follow bemanitools: Test is bit 4 and Service
  bit 6 of byte 3. OpenITG has them the other way around. One of the two
  buttons is swapped until a capture settles it.
- The P3IO's descriptor is not recorded, and the rule assumes 0x83 is on
  interface 0. Whether its inputs flow before INIT, and whether they need
  OpenITG's security plug reads, its watchdog value 30 or the EXTIO board
  written first, is not recorded. The EXTIO is a separate board on a serial
  port, and SDL never touches it. The 1000 ms reply limit is a budget, not a
  measurement.
- No capture of a P4IO exchange exists. The endpoint order and the report
  size follow p4io-mdxfdrv, and no source records the endpoint addresses or
  the interface class. Each layout's polarity follows the source it comes
  from, and the raw layout does not depend on it.
- Whether the jubeat panels report before the game sets the panel mode with
  SET PORTOUT is not recorded, and SDL sends no output. The jubeat layout has
  no Coin button, as bemanitools' jubeat driver has none. bemanitools'
  jubeat emulator puts Coin on bit 24, which the raw layout carries.

## Tests

The protocol modules are C99 with no SDL runtime and no I/O.
`test/controller-protocols` runs `testusio`, `testkonamip3io` and
`testkonamip4io` against blocks, frames and reports constructed from RPCS3,
TaikoZucchini, ITAIKO-firmware, bemanitools, OpenITG, p4io-mdxfdrv and
arcade-docs, and `testvendorusb` checks where each board's rule applies on
Windows, Linux and macOS. `test/serial-joystick` runs `testserialacio`
against the bus inside the serial engine, `testserialbio2` against the BI2A
node, and `testserialkfca`, `testserialpanb`, `testserialrvol` and
`testserialmdxf` against the RS-232 boards' frames, in normal and
AddressSanitizer builds.
