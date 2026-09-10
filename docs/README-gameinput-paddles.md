# GameInput paddle rollout withdrawn

The rollout from `cc7daf166d` and its documentation follow-up `f65df9586f`
were removed from this branch. SDL's source and backend behavior are restored
to `d98c5804a9`. The new SDK selection, paddle mapping, ownership checks,
callbacks, properties and test harnesses are removed.

The [September 10 report](https://github.com/hifihedgehog/SDL/issues/28#issuecomment-5625248719)
records duplicate GameInput and DirectInput entries for two physical Bluetooth
controllers. It also records no independent paddle capability on the tested
Elite Series 2 with firmware 5.13.3146.0 and GameInput 3.3.221.0.

The SDL task builds the restored source with the Windows SDK header and copies
the verified DLL to `PadForge.App/Resources/SDL3/x64/SDL3.dll`. The PadForge task
must remove the unconditional GameInput hint from `217aad5f`, publish its
executable and verify saved-device behavior. The independent string-property
marshalling correction and existing device-adoption policy must remain.

Issue [28](https://github.com/hifihedgehog/SDL/issues/28) remains open for a
replacement. No replacement implementation or runtime upgrade is part of this
rollback.
