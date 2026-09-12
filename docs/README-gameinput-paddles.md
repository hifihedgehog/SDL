# GameInput paddle rollout withdrawn

The rollout from `cc7daf166d` and its documentation follow-up `f65df9586f`
were removed from this branch on September 10, 2026. That rollback restored
SDL's source and backend behavior to `d98c5804a9`, including the Windows SDK
header selection.

The [September 10 report](https://github.com/hifihedgehog/SDL/issues/28#issuecomment-5625248719)
records duplicate GameInput and DirectInput entries for two physical Bluetooth
controllers. It also records no independent paddle capability on the tested
Elite Series 2 with firmware 5.13.3146.0 and GameInput 3.3.221.0.

The replacement is an [XInput paddle supplement](README-xinput-paddles.md).
It adds reports to the existing controller entry and does not change the
global GameInput backend selection. The withdrawn rollout stays removed.

Upstream now vendors a GameInput API version 3 header and turns the backend
on by default when that version is available. This fork keeps the desktop
default off in `src/core/windows/SDL_gameinput.h`. The backend still compiles
and loads `GameInput.dll` dynamically, and an application enables it only by
setting `SDL_JOYSTICK_GAMEINPUT`. The GDK default is unchanged.
