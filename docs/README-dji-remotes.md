# DJI drone remotes

DJI's drone remotes give Windows no game controller. This fork reads them as
gamepads over the three links DJI's own DUML protocol runs on: the remote's USB
serial port, the USB bulk interface of the screen remotes, and TCP port 40007.
It sends only what the community bridges send. That includes `06/24` with
payload 01, which turns on the remote's simulator mode and which the bridges
send before they poll. Over TCP it also answers each request that asks for
an acknowledgment with an empty response. It loads no firmware.

## Remotes

| Remote | Link | How SDL finds it | Name |
|---|---|---|---|
| RC-N1 family (RC231 and the remotes of the Mini 2 and Mavic 3) | USB serial | Automatically on interface 2 of 2CA3:1010, 2CA3:1020 and 2CA3:1030, or `dji` in `SDL_HINT_JOYSTICK_SERIAL` | DJI RC-N1 |
| Mavic Mini remote | USB serial | `djimavicmini` in `SDL_HINT_JOYSTICK_SERIAL` | DJI Mavic Mini Remote |
| Phantom 3 remote | USB serial | `djiphantom3` in `SDL_HINT_JOYSTICK_SERIAL` | DJI Phantom 3 Remote |
| Phantom 2 remote | USB serial | `djiphantom2` in `SDL_HINT_JOYSTICK_SERIAL` | DJI Phantom 2 Remote |
| DJI RC (RM330) | USB bulk, 2CA3:1023 interface 1 | HIDAPI once WinUSB is bound, `SDL_HINT_JOYSTICK_HIDAPI_DJI_REMOTE` | DJI RC (RM330) |
| DJI RC, DJI RC 2 | TCP 40007 | `SDL_HINT_JOYSTICK_DJI_REMOTE_TCP_HOSTS` | DJI RC (RM330), DJI RC 2, or DJI Remote |

The serial ports appear once DJI Assistant 2 has installed DJI's VCOM driver.
That driver names interface 2 of those three product IDs "DJI USB VCOM For
Protocol", and SDL opens no other DJI product's port by itself, so an RC-N1
family remote on another product ID needs `dji` in the hint. SDL holds a port
it opens, so DJI Assistant 2 cannot use the remote while SDL does. Turn
`SDL_HINT_JOYSTICK_SERIAL_AUTO` off to leave RC-N1 ports alone. The RC-N1
port must be the remote's bottom USB-C port.

The DJI RC's bulk interface needs WinUSB bound to
`USB\VID_2CA3&PID_1023&MI_01`. See [README-vendor-usb.md](README-vendor-usb.md).

Over the network, name each remote's IPv4 address, with a port when it is
not 40007:

    192.168.7.251,192.168.42.2

SDL opens no connection unless this hint names a host. DJI's latest firmware
closed port 40007, so the network link serves remotes on older firmware, or
the port reached through adb, whose steps no source gives.

SDL's device lists, `SDL_HINT_JOYSTICK_BLACKLIST_DEVICES`,
`SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES` and
`SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT`, check a network remote as
0x0000/0x0000 with its joystick name when it would become a joystick, so an
allow list leaves every network remote out. A remote left out keeps its
links and gets no joystick.

## The gamepad

The axes follow the gimbals' physical positions on every remote. Mode 1 and
Mode 2 remain a simulator setting. Up is negative on both vertical axes.
The dial reads positive when turned right on the RC-N1 family and the
Phantom 2, as DJI_RCNx_for_drone_simulators and mDjiController read it. No
source gives the direction on the other remotes, whose dials keep the sign
the remote sends.

| Axis | Control |
|---|---|
| 0, 1 | Left gimbal, horizontal and vertical |
| 2, 3 | Right gimbal, horizontal and vertical |
| 4 | Gimbal dial |
| 5 | Second dial, on the bulk and network remotes |

| Button | Control | Remotes |
|---|---|---|
| 0 | Return to home or pause | Phantom 3, network |
| 1 | Record | Phantom 3, network |
| 2 | Shutter, full press | Phantom 3, network |
| 3 | Shutter, half press | Network, DJI RC only |
| 4, 5 | C1, C2 | Phantom 3, network |
| 7 | Playback | Phantom 3 |
| 8 | Dial press | Phantom 3 |
| 9 to 11 | Flight mode position, one held | RC-N1, Phantom 3, network. Phantom 2: the left lever |
| 12 to 14 | Right lever position | Phantom 2 |
| 15 to 18 | Status bits 1, 2, 5 with 6, and 7, names not known | RC-N1 |
| 19, 20 | Right dial counter rising and falling, which the Phantom 3 mDjiController reports as the dial turned left and right, pressed 100 ms | Phantom 3 |

The mapping puts the gimbals on the sticks, the dial's positive half on Y and
its negative half on B. On the remotes that have them, the shutter goes on A,
record on X, C1 and C2 on the shoulders and return to home on Guide.

A network remote reports its battery: a `06/1E` push carries the percentage
in payload byte 4, and the joystick reports it as on battery.

## Timing

- RC-N1: `06/24` once, then after 50 ms a poll of `06/01` and `06/27`. A
  `06/01` answer asks for the next poll at once, and 25 ms without one sends
  it again. The Mavic Mini remote gets `06/01` alone, addressed to 0E.
- Phantom 3: two frames every 10 ms. Phantom 2: an init frame, 2000 ms, then a
  ping every 10 ms. The bytes of each 10 ms count as one read, which must be
  76 bytes starting with 55.
- DJI RC over USB: `06/24`, then after 100 ms `06/01` polls, again after each
  answer or 10 ms, and `06/24` every 3000 ms. With no 32-byte `06/01` answer
  within 200 ms it polls `06/F5` every 12 ms instead. The writes go out one at
  a time on a thread of the driver's own, so a remote that stops taking data
  holds up no application thread. Closing the remote waits for a write still
  going out, up to its 1000 ms timeout.
- On the RC-N1, the Mavic Mini remote and the DJI RC over USB, an answer
  that arrives before its poll has finished going out sends the next poll as
  soon as that poll has.
- Over TCP each frame travels behind `55 CC 30 75` and its length. The DJI RC
  gets a keepalive at connect and after 40 ms without data. A DJI RC 2 gets
  the same keepalives, one at the first connect, until its `rc331` beacon
  names it, as voluminor's automatic profile does. After the beacon it gets
  none, and five links start 500 ms apart. Any request asking for an
  acknowledgment gets an empty response.
- A serial remote goes after 2000 ms without a stick report, a USB remote
  after 1000 ms, a network remote after 1000 ms without a `06/AE` frame. Each
  comes back as a new joystick.

## Known limits

- The RC-N1's `06/27` bits have no known names, and the gamepad mapping
  leaves them out. Which switch end is which flight mode is not known.
- The DJI RC over USB reports no buttons. No capture shows which request
  carries them.
- After the `06/F5` fallback the DJI RC over USB reports only its sticks. Its
  dials rest at 0 unless a `06/01` answer set them in that start-up, since no
  source decodes the rest of the `06/F5` reply.
- The Phantom 2's replies cannot be validated, so two replies in one 10 ms
  read are lost.
- On the network no source ties a raw value to a stick direction. The fork
  reads the values as the serial and USB remotes send them.
- No DJI remote documents rumble, LEDs or displays.

## Tests

The codec, the remote modules and the network session are C99 with no SDL
runtime and no I/O. `test/serial-joystick` runs them against constructed
frames and three hardware captures read from the reference clones, in normal
and AddressSanitizer builds. `test/libusb-backend` runs the USB driver
against a fake libusb whose bulk OUT endpoint can refuse data.
