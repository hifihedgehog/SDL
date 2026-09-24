# Wii Remote extensions

The HIDAPI Wii driver (`SDL_HINT_JOYSTICK_HIDAPI_WII`, off by default) reads
these extensions through a Bluetooth-paired Wii Remote, beyond the Nunchuk,
Classic Controller, Wii U Pro Controller and Balance Board. The hint
`SDL_HINT_JOYSTICK_HIDAPI_WII_EXTENSIONS`, on by default, turns them on. With
it off they report as an unknown extension, as before.

The protocol lives in `src/joystick/hidapi/SDL_hidapi_wii_ext_proto.c`, and
`test/controller-protocols` runs its replay tests.

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
type, so each configuration has its own GUID.

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
  buttons 0 and 1 are the pen's lower and upper buttons.
- Shinkansen: the brake and power levers on LEFT_TRIGGER and RIGHT_TRIGGER by
  notch, their raw bytes on axes 6 and 7. The face buttons follow the Classic
  Controller's bits, the D-pad is hat 0.

None of these runs behind an active Motion Plus. The driver leaves the Motion
Plus inactive, so no gyro is reported with them.
