# Wii Remote extensions

The HIDAPI Wii driver (`SDL_HINT_JOYSTICK_HIDAPI_WII`, off by default) reads
these extensions through a Bluetooth-paired Wii Remote, beyond the Nunchuk,
Classic Controller, Wii U Pro Controller and Balance Board. The hint
`SDL_HINT_JOYSTICK_HIDAPI_WII_EXTENSIONS`, on by default, turns them on. With
it off they report as an unknown extension, as before.

The protocol lives in `src/joystick/hidapi/SDL_hidapi_wii_ext_proto.c`, and
`test/controller-protocols` runs its replay tests. `test/community-mappings`
checks the gamepad mappings SDL generates from it.

| Extension | ID bytes 0, 4, 5 | Joystick type | Buttons, axes, hats |
|---|---|---|---|
| Guitar Hero guitar | 00, 01 03 | Guitar | 26, 6, 1 |
| World Tour drum kit | 01, 01 03 | Drum kit | 26, 13, 0 |
| DJ Hero turntable | 03, 01 03 | Gamepad | 32, 8, 0 |
| Taiko TaTaCon | any, 01 11 | Gamepad | 26, 6, 0 |
| uDraw GameTablet | any, 01 12 | Unknown, no mapping | 26, 3, 0 |
| Drawsome tablet | any, 00 13 | Unknown, no mapping | 26, 3, 0 |
| Densha de GO! Shinkansen | any, 03 10 | Gamepad | 26, 8, 1 |

Every one keeps the remote's own buttons at 15-25, as the Classic Controller
configuration does, and none has a GUIDE button. GUID byte 15 is the extension
type, so each configuration has its own GUID. Each gamepad mapping binds only
the controls its extension reports: the drum kit, turntable and TaTaCon have
no D-pad, the guitar's strum bar is D-pad up and down, and only the TaTaCon
binds the stick buttons, to its faces.

- Guitar: green, red, yellow and blue frets on SOUTH, EAST, WEST and NORTH,
  orange on LEFT_SHOULDER, the pedal on RIGHT_SHOULDER, minus on BACK, plus on
  START, strum on hat 0 up and down. The stick is LEFTX and LEFTY, whammy is
  RIGHTX from 0 at rest, and the touch bar is RIGHTY with the values a PS3
  Guitar Hero guitar reports. With sensors on, the remote's accelerometer is
  `SDL_SENSOR_ACCEL` for tilt.
- Drum kit: green, red, blue and yellow on SOUTH, EAST, WEST and NORTH, the
  bass pedal on LEFT_SHOULDER, orange on RIGHT_SHOULDER. Axes 6 to 11 carry
  each pad's hit velocity in that order while the pad is down. Axis 12 holds
  the last hi-hat pedal value.
- Turntable: either green on SOUTH, either red on EAST, either blue on WEST,
  Euphoria on NORTH, and each platter's buttons apart at 26-31 (left green, red,
  blue, right green, red, blue). RIGHTX and RIGHTY are the left and right
  platter rates, axis 6 the crossfader, axis 7 the effects dial. The Euphoria
  LED is a mono LED.
- TaTaCon: the faces on LEFT_STICK and RIGHT_STICK, the rims on the triggers.
- Tablets: axis 0 and 1 are the raw pen coordinates, -1 while the pen is out of
  range, axis 2 the pressure. Button 2 is pen in range, and on the uDraw
  buttons 0 and 1 are the pen's lower and upper buttons. The uDraw sends
  12-bit coordinates, X left to right and Y bottom to top, about 80-1960 by
  95-1450 on hardware, and 9-bit pressure. The Drawsome sends 16-bit
  coordinates, X left to right and Y top to bottom, 0-10206 by 0-7422
  measured, and 12-bit pressure.
- Shinkansen: the brake and power levers on LEFT_TRIGGER and RIGHT_TRIGGER by
  notch, their raw bytes on axes 6 and 7. The face buttons follow the Classic
  Controller's bits, the D-pad is hat 0.

None of these runs behind an active Motion Plus. The driver leaves the Motion
Plus inactive, so no gyro is reported with them.

## Sensors and the IR camera

The remote alone, with a Nunchuk and with a Classic Controller registers the
remote's accelerometer as `SDL_SENSOR_ACCEL`, and the gyro of a Motion Plus,
built into the Wii Remote Plus, as `SDL_SENSOR_GYRO`. The Nunchuk adds its own
accelerometer as `SDL_SENSOR_ACCEL_L`. Enabling the first of them powers the
IR camera, whose first two dots are axes 6 to 9, the X and Y of each, -1 for
a dot the camera does not see. Disabling the last one turns the camera off.

In these configurations, enabling a sensor also activates a Motion Plus, in
the pass-through mode of an attached Nunchuk or Classic Controller. Its
reports then alternate with the extension's, the Nunchuk's accelerometer
values lose their lowest bit, and the Classic Controller's left stick loses
its lowest bit. The Wii U Pro Controller, the Balance Board, the extensions
above and an extension SDL does not recognize power no camera.

No sensor call and no joystick update waits on the remote for the camera or
the Motion Plus. The camera's writes go one at a time, each once the remote
has acknowledged the one before it, and the remote's input keeps arriving
meanwhile. A camera write the remote has not acknowledged 250 ms after it
was sent goes again, up to four sends in all.

While a sensor stays on, a camera start the remote does not complete runs
again after 1, 2 and 4 seconds, then every 8, and so does one whose status
report shows the camera off. Each camera write the remote refuses or leaves
unacknowledged is logged in `SDL_LOG_CATEGORY_INPUT` at debug priority, with
the register or report and the error.

## A remote that goes quiet

A remote that sends nothing for 3 seconds gets a status request. A remote
that takes it keeps its joystick. One that refuses it loses the joystick
until it answers again. Windows refuses the write for a remote that powered
off, and the Mayflash DolphinBar for an empty slot. A path that refuses the
request when SDL first opens it gets no joystick either. SDL keeps such a
path, so no other backend lists the remote, and asks again every 3 seconds.
The remote's first report after that brings a new joystick, which starts
with its sensors off. A read error releases the path, and SDL opens it again
at the next device change.
