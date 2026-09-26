# Bluetooth LE controllers

This fork reads controllers that send their input as notifications on a
vendor GATT service over Bluetooth LE instead of the HID service. Windows
loads no driver for such a service. The driver is Windows only and shares
its WinRT transport with the Switch 2 driver.

| Family | Found by | Pairing | Joystick |
|---|---|---|---|
| Nintendo Poke Ball Plus | The name "Pokemon PBP" | None | Gamepad, "Poke Ball Plus" |
| Google Daydream controller | The name "Daydream controller" | Before discovery, when Windows holds no bond | Gamepad, "Google Daydream Controller", VID 18D1, PID 9210 |
| Samsung Gear VR controller | Service 4f63756c-7573-2054-6872-65656d6f7465, or a name starting "Gear VR Controller" | When a subscription needs it | Gamepad, "Samsung Gear VR Controller" |
| Oculus Go controller | A name starting "OMVR", or service 81265652-3692-ae93-e711-270f223c83b3 | When a subscription needs it | Gamepad, "Oculus Go Controller" |
| Guitar Hero Live guitar for iOS | A name containing "Ble Guitar", or service 1523 | None | Guitar, "Guitar Hero Live Guitar (iOS)" |
| Zwift Play and Zwift Click | Zwift's company ID 0x094A with type 02, 03, 09 or 0E | None | Gamepad, "Zwift Play (L)", "Zwift Play (R)", "Zwift Play" or "Zwift Click" |
| Thalmic Myo armband | Service d5060001-a904-deb9-4748-2c7f4a124842 | None | Joystick, "Thalmic Myo Armband" |

`SDL_HINT_JOYSTICK_BLE` turns the driver on, default off, so no Bluetooth
scan runs unless the application asks for one.
`SDL_HINT_JOYSTICK_BLE_POKEBALL`, `_DAYDREAM`, `_GEARVR`, `_OCULUSGO`,
`_GHLIVE`, `_ZWIFT` and `_MYO` default to it and override it, as the HIDAPI
drivers' hints do. `SDL_HINT_JOYSTICK_BLE_SWITCH2` belongs to the Switch 2
driver and does not follow it. `SDL_HINT_JOYSTICK_BLE_PAIRING`, default off,
lets the driver pair a device that streams only over a bond, and remove a
Windows bond through which a subscription cannot reach a device. Pairing
writes a bond into Windows, and for an Oculus Go it can replace the
controller's pairing with its headset. With the hint off, pair the Daydream
and the Oculus Go in Windows Settings first. A device the driver tried
before it was paired appears at its next attempt, up to 5 minutes later.
Every hint can be set anytime. The driver applies a family hint at the next
joystick update, and the pairing hint to each connection it starts after
that. A family turned off loses its joysticks at once, and the driver
disconnects its devices after any closing writes. At `SDL_Quit` the driver
waits at most 1 second for closing writes and at most 3 seconds in all for
the links to close. A link still closing then is left to close on its own.

While a family is on, the driver watches advertisements. It connects to a
matching device, discovers its services without the cache, subscribes to
its characteristics and runs the device's start-up. The joystick appears
with the first input after the start-up. A device with no input 30 seconds
after its start-up began is disconnected, so no device stays connected for
long without a joystick. A lost link removes the joystick, and the next
advertisement connects again and repeats the start-up. After a failed
discovery, subscription, bond removal or start-up, or a Daydream without a
bond while pairing is off, the address waits 15 seconds before the next
attempt, doubling each time up to 5 minutes. A joystick that appeared resets
the wait. A failed pairing is tried again at the next advertisement twice,
then waits the same way. A failed connection waits 5 seconds, and so does
a link lost before the joystick appears, unless the link drops while the
driver, after a failed subscription, pairs the controller or removes its
bond.
Changing the pairing hint ends the wait of every address the driver is not
connected to. The joystick's serial is the Bluetooth address as 12 hex
digits.

Gamepads report their buttons and axes at SDL's gamepad positions, and the
driver's gamepad mapping names each one. A touchpad also moves the left
stick. Triggers rest at -32768 on the joystick axis, which the gamepad
reads as 0.

## Poke Ball Plus

Press the top button to make the ball advertise. SDL sends nothing. The
stick is the left stick, the stick press South and the top button East. The
battery comes from the Battery service. The motion data is not decoded,
because no source agrees on its layout.

## Daydream

Hold Home until the light pulses. The driver uses the controller only over a
Windows bond, since from firmware 1.2.11 it sends its record only over one.
The touchpad is SDL's touchpad and also moves the left stick. The touchpad
click is South, App Start, Home Guide, and the volume buttons the
shoulders. The accelerometer and gyroscope use the controller's own clock
and are registered at 40 Hz, one source's measured average. The controller
sends a record about every 16 ms. The battery comes from the Battery
service. The controller's orientation is not exposed.

## Gear VR

SDL writes 08 00, waits for the controller to echo it, then writes 01 00,
which streams 68 packets a second. Without the echo in 4 seconds it writes
01 00 alone, which streams about 30. It writes 04 00 every 10 seconds and
00 00 when SDL closes the connection. When 3 seconds pass without a packet,
counted from the 01 00 write or from the last packet, SDL releases every
control and runs the start-up again from 08 00, since an 01 00 that reaches
the controller before the echo can stop the stream for good. After two such
restarts in a row bring no packet, the driver disconnects the controller
and connects again later. The touchpad is SDL's touchpad and also moves the
left stick, the trigger is the right trigger, the touchpad click South,
Back Back, Home Guide and the volume buttons the shoulders. Once the echo
arrives, the accelerometer and gyroscope report at 206 Hz, three samples a
packet, with the controller's clock. SDL's X is the controller's X, SDL's Y
its Z, and SDL's Z its minus Y, so the controller lying flat reads +1 g on
SDL's Y. The battery is in every packet.

## Oculus Go

The controller streams only over a bond: hold Oculus and Back until the
light blinks. The touchpad is SDL's touchpad and also moves the left stick,
the trigger is the right trigger, the touchpad click South, Back Back and
Oculus Guide. No sensor is registered. The only source treats the
accelerometer's raw Y as the axis along the controller, so the frame SDL
needs is unknown, and no source gives the gyroscope's scale. After 3 seconds
without data the driver releases every control and writes the
subscriptions again, then again every second until data returns, as the
macOS tool does.

## Guitar Hero Live for iOS

SDL sends nothing. Black 1, 2 and 3 are South, East and North, White 1, 2
and 3 West and the two shoulders. Strum up and down are the D-pad's up and
down, with the left stick's Y at its ends, and the guitar's D-pad joins
them. Pause is Start, Hero Power Back, GHTV the left stick click and the
sync button Guide. The whammy is the right stick's Y from 0 to -32768, and
the tilt its X, as SDL reports the Xbox 360 guitar that GHLtarUtility makes
of this one. The fork's driver for the guitar's USB dongles
(README-instruments.md) reports the whammy from 128 at rest to 32767
instead.

## Zwift Play and Zwift Click

SDL writes "RideOn" to the controller and reads plain messages. A
controller that sends no message within 10 seconds of that write gets the
key exchange Zwift's app performs: a P-256 key pair, HKDF with SHA-256 and
AES-CCM from Windows' CNG. Each half of a Play on firmware 1.x is its own
gamepad: the left half's pad is the D-pad, its side button the left
shoulder, its on/off button Back, its lever the left stick's X and its pull
the left trigger. The right half's Y, Z, A and B are North, West, East and
South, its side button the right shoulder, its on/off button Start, its
lever the right stick's X and its pull the right trigger. A Play on
firmware 2.0.1 is one gamepad with the same buttons and both levers. The
Click's plus and minus are the right and left shoulders. The battery is in
the controller's messages. The Zwift Ride and the Click v2 are not
supported. Ride firmware after 1.2.0 hides its service until Zwift's app
unlocks it, and the Click v2 needs an unlock from Zwift's app or keep-awake
logic that is not public.

## Myo

SDL turns on the IMU data and events and the pose classifier, keeps the
band from sleeping and unlocks it. On close it lets it sleep and locks it.
Fist, wave in, wave out, fingers spread and double tap are buttons 0 to 4.
A pose holds its button, and rest, an unknown pose, a lost arm sync or a
lock release all five. Roll, pitch and yaw from the band's orientation are
axes 0 to 2. The accelerometer and gyroscope report at 50 Hz, and since SDL
reads sensors through its gamepad API, an application sees them once it
gives the Myo a gamepad mapping. The battery comes from the Battery
service. This mapping is the fork's own.

## Known limits

- No capture of the Poke Ball Plus, the Oculus Go or the guitar exists as
  text. The Zwift controllers decode against the messages quoted in
  Makinolo's posts, the Myo against device values in dl-myo's tests, the
  Daydream against the captures its sources quote, and the Gear VR against
  gearvr-controller's recordings from an ET-YO324.
- Whether Windows accepts each device's connection-parameter request is
  not known. The Gear VR drops the link when its request is not met within
  30 seconds.
- Whether Windows' confirm-only pairing suits the Daydream, the Gear VR and
  the Oculus Go is not known.
- The driver removes a Windows bond the controller has lost only with the
  pairing hint on, and only when a subscription reports it: as unreachable
  for any family, and also with an authentication or encryption error for
  the Daydream, the Gear VR and the Oculus Go. A controller that stays
  silent instead is disconnected within 30 seconds each time. Remove it in
  Windows Settings and pair it again.
- The Gear VR's axes match gearvr-controller's recordings of known poses and
  motions on an ET-YO324. The Daydream's axes follow its sources, and two
  pointer tools agree on its pitch and yaw signs. Neither was tested through
  SDL on hardware. The Myo's axes relative to the arm are not documented.
- After a gap longer than 512 ms in the Daydream's stream, its sensor times
  fall behind by whole 512 ms periods, as they do in its sources.
- Which Zwift firmware accepts the plain handshake, and whether a
  firmware 2.0.1 Play answers on service FC82 or on the 128-bit service, are
  not documented. The driver tries FC82 first, as SwiftControl does.
- The Poke Ball Plus's stick center comes from one tool's range. A ball
  that rests elsewhere reads slightly off center.
- A device with a rotating address gets a new serial for each connection.

## Tests

The session and the device modules are C99 with no SDL runtime and no I/O.
`test/ble` runs `testblesession` and one test per device, and
`testblezwiftcrypto` runs the Zwift key exchange through Windows' CNG, in
normal and AddressSanitizer builds. `test/ble-driver` runs
`testblegattdriver`, the driver inside a static SDL against a fake transport,
in both builds. It and `testblegearvr` read the Gear VR packets from the
gearvr-controller clone.
