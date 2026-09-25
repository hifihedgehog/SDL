# Original Xbox controllers (XID)

This fork reads the devices that speak the original Xbox XID protocol through
a passive Xbox-to-USB cable: the Duke and Controller S pads, the pads, wheels,
dance pads, arcade sticks and light guns that share their 20-byte report, and
the Steel Battalion controller. Windows has no driver for any XID interface.
SDL reads it through libusb once WinUSB is bound to it. The application or its
installer does the binding. SDL loads no driver and no firmware.

The protocol lives in `src/joystick/hidapi/SDL_hidapi_xid_proto.c`, which
`test/controller-protocols` replays. The driver is
`src/joystick/hidapi/SDL_hidapi_xid.c`.

## Binding

An XID interface is class 0x58, subclass 0x42, protocol 0, with one interrupt
IN and one interrupt OUT endpoint. SDL serves every such interface whatever its
IDs, as Linux xpad does. The DVD remote's XID interface has one endpoint and is
not served.

Bind WinUSB to the pad function by the compatible ID `USB\Class_58&SubClass_42`.
That ID follows the rule Windows uses for other single-interface devices, but
it has not been read from an XID device. If Windows lists other compatible IDs
for one, bind by hardware ID. An XID pad is a USB hub with the pad behind it,
so bind the pad, not the hub.

On Windows the libusb backend lists an XID interface only when it can open it,
so an unbound pad does not appear. On Linux the kernel's xpad driver keeps XID
devices, and the libusb backend leaves them to it unless
`SDL_HINT_HIDAPI_LIBUSB_WHITELIST` is "0".

## Hints

| Hint | Default | Devices |
|---|---|---|
| `SDL_HINT_JOYSTICK_HIDAPI_XBOX_ORIGINAL` | `SDL_HINT_JOYSTICK_HIDAPI_XBOX` | every XID interface except 0A7B:D000 |
| `SDL_HINT_JOYSTICK_HIDAPI_STEEL_BATTALION` | `SDL_HINT_JOYSTICK_HIDAPI_XBOX_ORIGINAL` | 0A7B:D000 |

The XID descriptor picks the decoder on any ID. ogx360 reports the Steel
Battalion descriptor on 045E:0289, and that adapter is read as a Steel
Battalion under the first hint.

## Open

The driver sends one vendor request, GET_DESCRIPTOR: `C1 06`, wValue 0x4200,
wIndex the interface, wLength 16, with a 100 ms timeout. bType 0x01 in the
reply is the gamepad family and 0x80 the Steel Battalion. A device with
bType 0x03, the DVD remote, is closed. Without a usable reply the ID decides:
0A7B:D000 is the Steel Battalion and any other ID the gamepad family.

The gamepad family then reads its current report once with GET_REPORT:
`A1 01`, wValue 0x0100, wLength 20. A pad sends a report only when something
changes, and this read catches a stick or button already held. Nothing else is
sent until the application asks for rumble or lamps.

Silence is not a disconnect. A read error is.

## Gamepad family

- Axes 0-5: left X, left Y, right X, right Y, left trigger, right trigger.
  Axes 6-11: the pressure of A, B, X, Y, White and Black, -32768 released.
- Buttons 0-10 in SDL gamepad order. Guide, button 5, never goes down. White
  is the left shoulder and Black the right. A face button is down above 0x20
  of pressure.
- Hat 0 is the D-pad. Opposite directions cancel.
- A dance pad has no hat. Its arrows are buttons 11 to 14: up, down, left,
  right, and opposite arrows can be held together. Dance-pad mode comes from
  bSubType 0x80 or from the IDs 0738:4540, 0738:45FF, 0738:4743, 0738:6040,
  0C12:8809, 0D2F:0002, 12AB:8809 and 1430:8888. 0C12:8809 reads its sticks
  as centered.
- A light gun, bSubType 0x50, adds button 11, down while the gun sees a
  bright screen.
- The joystick type comes from bSubType: 0x01 and 0x02 gamepad, 0x10 wheel,
  0x20 and 0x21 arcade stick, 0x30 flight stick, 0x80 dance pad, and unknown
  for the rest.
- The name is the USB product string, else the name in the identity table,
  else "Xbox Controller" for bSubType 0x01, "Xbox Controller S" for 0x02 and
  "Xbox Input Device" for the rest.
- The gamepad mapping is the standard layout without Guide. A dance pad maps
  buttons 11 to 14 to the D-pad. For an ID in the identity table the GUID
  alone decides the mapping. Any other XID device gets its mapping only while
  it is connected, since only its interface class marks it.
- `SDL_RumbleJoystick` writes `00 06 LL LH RL RH` to the OUT endpoint, low
  frequency on the left motor.

## Steel Battalion

- 9 axes: aiming lever X and Y, rotation lever, sight change X and Y, left,
  middle and right pedal, tuner dial. Each axis takes its report byte as its
  high byte. The pedals read -32768 released, and the tuner's 16 positions
  run from -32768 to 32767.
- 46 buttons: the 39 report bits in byte order, byte 2 bit 0 first, then gear
  R, N and 1 to 5. One gear button is down while the lever sits in a gear and
  none while it is between gears. The five toggles stay down while switched
  on.
- No gamepad mapping and no rumble.
- Until its first report the controller reads as at rest: levers centered,
  pedals released, tuner at 0, no gear.
- `SDL_SendJoystickEffect` with exactly 22 bytes sets the lamps. Lamp n is
  byte n / 2, the low nibble for even n and the high nibble for odd n, with a
  brightness from 0 to 15. The driver sets bytes 0 and 1 to `00 16` and writes
  32 bytes, the 22 followed by 10 zero bytes. Other sizes fail. SDL lights no
  lamp by itself.

| Lamps | Controls |
|---|---|
| 4-7 | Emergency eject, Cockpit hatch, Ignition, Start |
| 8-13 | Open/close, Map zoom in/out, Mode select, Sub monitor mode select, Main monitor zoom in, Main monitor zoom out |
| 14-22 | Forecast shooting system, Manipulator, Line color change, Washing, Extinguisher, Chaff, Tank detach, Override, Night scope |
| 23-25 | F1, F2, F3 |
| 26-28 | Main weapon control, Sub weapon control, Magazine change |
| 29-33 | Comm 1 to Comm 5 |
| 35-41 | Gear R, N, 1, 2, 3, 4, 5 |

Lamp 34 and byte 21 have no known lamp.

## Known limits

- None of this has run on hardware. The replay tests are the evidence.
- Sources disagree on which rumble byte drives the strong motor. Linux, xemu,
  Xb2XInput and tusb_xinput put it on the left, byte 3, and xboxdrv's notes put
  the weak motor there.
- Wheels, flight sticks, snowboards, fishing rods and radio flight controls
  use the pad layout, because no source documents their report bytes.
- Light guns need the video sync signal that passive adapters leave
  unconnected, and may report nothing without it.
- The Steel Battalion's even bytes 8 to 22, the low bytes of its axes in some
  layouts, are ignored.
