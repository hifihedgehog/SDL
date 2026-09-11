# XInput Elite paddles

This fork adds four paddle buttons to a qualified Xbox Elite controller's
existing XInput joystick. OpenXInput supplies the slot's cached interface path,
channel, and attachment generation through `OpenXInputGetDeviceIdentityV1`.
The supplement resolves that path to the physical device before acquiring
additional reports.

USB eligibility uses the physical parent's VID/PID. Windows can expose an
Elite through the generic `045E:02FF` XUSB identity. That published SDL identity
is preserved.

The build option `SDL_XINPUT_PADDLES` enables the supplement on native MSVC x64
Windows builds with XInput and the C runtime enabled. Set the private hint
`SDL_JOYSTICK_XINPUT_PADDLES` to `0` before opening the joystick to disable it.
The Windows SDK GameInput selection remains independent of this option.
This build adds Windows 10 WinRT API-set and Microsoft C++ runtime dependencies
to the DLL. Build with the option off for Windows targets without them.

## Input and mappings

The original XInput driver owns ordinary controls, Guide, Share, and rumble.
The supplement publishes only raw buttons 12 through 15. Raw button 11 retains
its Share reservation.

| Physical paddle | Raw button | Gamepad mapping field |
| --- | --- | --- |
| Upper right | 12 | `paddle1` |
| Lower right | 13 | `paddle3` |
| Upper left | 14 | `paddle2` |
| Lower left | 15 | `paddle4` |

A qualified joystick reserves these buttons during Open. Its gamepad mappings
are therefore available when the application first opens it. These bindings
belong to that instance. The GUID mapping cache is unchanged. Explicit paddle
fields, including empty fields, and existing raw-button bindings take precedence.

Paddle values start at zero. Invalid identity, source retirement, and missing
history release the paddle state. Acquisition failures leave the capability
mapping stable. The decoder preserves the raw paddle mask independently of
the onboard profile and leaves ordinary XInput buttons untouched.

## Transports

Bluetooth uses service `00000001-5f60-4c4f-9c83-a7953298d40d`, characteristic
`00000005-5f60-4c4f-9c83-a7953298d40d`, and standard CCCD Notify. Its 17-byte
payload stores the paddle mask at byte 14. Byte 15 is profile metadata and
byte 16 is opaque. Endpoint association uses checked Windows device metadata.
Cleanup restores a CCCD value changed by this client after checking its current
value.

USB uses one process-level client of the existing input service. Each selected
device has a separate view generation and owned copies of its input packets.
The WGI provider with the same native GIP ID must supply its own input-readiness
observation before it receives the existing extra-data command. Its normal
callback is LowLatency class 1, message 0, length 46. A service report alone
does not satisfy that precondition. Resume starts a new input epoch. Suspend
invalidates pending command readiness, while the service remains the USB input
publisher. A new epoch can rearm after its own normal frame. A completed command
is recorded separately from report receipt. The decoder accepts the known 29-, 34-, 46-,
and 47-byte in-band layouts and the separate 17-byte `0x0C` report. Once a
separate report arrives, ordinary reports cannot replace its paddle state.

The private USB layout is qualified against the complete file profile in
`SDL_xinput_paddle_runtime.cpp`: the service executable, inbox and redistributable
GameInput DLLs, WGI DLL, and XboxGIP driver. File identities and digests are
checked together. A missing file, including an absent redistributable DLL,
or an unknown or changing profile disables this USB supplement.
The check assumes Windows loaded a member of that supported family. It does
not establish the exact bytes already mapped in the service process.

The service connection also checks its peer against the service manager's
session child. Every view is bounded and its native ID must agree with its
provider string. Descriptor metadata alone does not establish received input.
Delivery through the Xbox Wireless Adapter and firmware 5.23.6.0 remain
separate coverage targets.

The September 11, 2026 hardware test passed on an Elite Series 2 over USB and
Bluetooth. Both runs recorded all four raw and mapped paddle presses and
releases. The Bluetooth capture contained 260 notifications, two press/release
cycles per paddle, and an ordinary A-button press/release with no state
mismatches. Its saved payloads agreed with every decoded, queued, drained,
and mapped paddle event. The controller reported Bluetooth revision 0513.

## Lifetime and diagnostics

The owner copies queued reports before SDL consumes them. Attachment and view
tokens reject stale publications. Bounded queue overflow releases state and
resynchronizes from a validated snapshot.

WGI has no custom-factory unregister API and its command call has no timeout
parameter. The module stays loaded once its worker starts. Worker and callback
storage is bounded and retained for calls whose completion is still unknown.
Close and Quit retire delivery without waiting under SDL's joystick lock.

Joystick properties under `SDL.joystick.xinput.paddle.` expose `received_20`,
`received_0c`, `received_gatt`, `gaps`, `enable_status`, `data_available`, and
`error`. The private `SDL.joystick.xinput.paddle_mask` property describes
capability. These properties are diagnostic, not a public SDL API contract.

Set `SDL_JOYSTICK_XINPUT_PADDLE_TRACE` to `1` before Open and enable input debug
logging for bounded passive traces. Records include WGI resume/suspend and raw
messages, service reading generations and raw sequences, payload bytes, decoded
masks, queue/drain transitions, and command times and HRESULTs. The library
keeps owned bounded records and logs them on SDL's thread. It writes no files.
Overflow is reported. Source timestamps and host QPC values are separate fields.

Offline regression tests are in `test/testxinputpaddle*`. They cover the decoded
capture cases, mapping precedence, identity, retained history, queue ordering,
and teardown. They do not substitute for transport receipt evidence.
