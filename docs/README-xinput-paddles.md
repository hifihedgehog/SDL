# XInput Elite paddles

This fork adds four paddle buttons to a qualified Xbox Elite controller's
existing XInput joystick. OpenXInput supplies the slot's cached interface path,
channel, and attachment generation through `OpenXInputGetDeviceIdentityV1`.
The supplement resolves that path to the physical device before acquiring
additional reports.

USB eligibility uses the physical parent's VID/PID. Windows can expose an
Elite through the generic `045E:02FF` XUSB identity. That published SDL identity
is preserved.

Xbox Wireless Adapter eligibility uses the controller's primary synthetic
device instance. The receiver's VID/PID identifies the transport, not the
controller model. The controller and receiver can have different containers.
The native controller ID joins the XInput attachment to its GIP provider and
service view. The host index in the device path is not used as controller identity.

The build option `SDL_XINPUT_PADDLES` enables the supplement on native MSVC x64
and native MSVC ARM64 Windows builds with XInput and the C runtime enabled.
ARM64EC is excluded. Both routes run on both architectures. Set the private hint
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

The client never trusts an inherited Notify. Windows caches the descriptor per
bond and can satisfy a same-value write from that cache, and a subscribed
client can then receive nothing while the descriptor still reads Notify. After
reading the descriptor, the client writes None and then Notify, so the
controller sees a real transition. Retirement restores the value it found when
the descriptor holds None or Notify, and leaves any other value alone.

A transient failure (the service still held by a killed process, a lost link,
a timeout, a cleanup or resource error) does not retire the route. The client
is recreated after a backoff that starts at 1 s, doubles, caps at 8 s, and
resets once a client delivers. Identity, contract, thread, and apartment
failures stay permanent. Only SDL sees the ordinary XInput state, so it
requests a new client when the state has changed for 1 s with no vendor
payload while the client reports streaming. That request recreates the client
at once, at most once per 3 s, and the new client's transition revives the
stream. An idle controller never trips it.

USB and Xbox Wireless Adapter connections use one process-level client of the
existing input service. Each selected
device has a separate view generation and owned copies of its input packets.
The WGI provider with the same native GIP ID must supply its own input-readiness
observation before it receives the existing extra-data command. Its normal
callback is LowLatency class 1, message 0, with a known normal payload length
of 14, 29, 34, 46, or 47 bytes. A service report alone
does not satisfy that precondition. Resume starts a new input epoch. Suspend
invalidates pending command readiness, while the service remains the USB input
publisher. A new epoch can rearm after its own normal frame. Series 1 paddles
use ordinary reports and do not receive the Series 2 extra-data command. A completed command
is recorded separately from report receipt. The decoder accepts the known 29-, 34-, 46-,
and 47-byte in-band layouts and the separate 17-byte `0x0C` report. Once a
separate report arrives, ordinary reports cannot replace its paddle state.

The command is re-sent inside the same input epoch when the separate report
stops. XboxGIP broadcasts a quiesce to every controller whenever the
foreground process changes, and the Elite then stops its `0x0C` report. An
elevated WGI client receives no suspend or resume for that switch, so the
epoch never changes. Report pairing is the ground truth: after a completed
command the service delivers each `0x0C` right after its `0x20` with the same
source timestamp. A `0x20` that no `0x0C` follows before the next `0x20`, or
within 50 ms, releases the paddle state and schedules the next attempt. A
foreground process change observed from the owner thread schedules one sooner,
after a 100 ms settle, because the driver's quiesce is sent inside its own
focus callback. Re-sends keep one command in flight and at most one dispatch
per 250 ms. Each attempt within an epoch is numbered. There is no foreground
requirement and no periodic timer: the service publishes both reports to a
background client, and a re-send from the background restores them. A
background client whose provider is suspended by the same switch can still
re-send, because the command needs only a provider that was ready once. A
resume starts a new epoch and the first command again follows that provider's
own normal frame.

The input service can stop publishing readings to every client after standby
while the WGI provider in the same process still receives every normal frame
and every `0x0C`. The WGI module keeps a bounded copy of the provider's own
reports, 64 per provider. The route publishes from those copies once the
provider has delivered 16 normal frames with no service reading for 250 ms,
and it switches back on the first live service reading, releasing the paddle
state at each switch. The service is preferred because it keeps delivering to
a background client whose provider WGI has suspended. A device the service
never lists starts in provider mode at the 10 s deadline when the provider has
delivered 16 normal frames, and adopts the view if it appears later. Pairing
detection, the focus poll, and the re-send run in both modes.

The private GIP layout is qualified against the installed file families in
`SDL_xinput_paddle_runtime.cpp`: the service executable, the inbox GameInput
DLL, the WGI DLL, and the XboxGIP driver by version family (major, minor and
build, with a revision floor), and the redistributable GameInput DLL by major
version when it is present in System32. A servicing update inside a family
keeps the route on, on x64 and on ARM64 alike, because the version resource
carries no architecture. A file outside its family, a missing required file,
or a file that changes during the check disables this GIP supplement, and the
`error` property names the file and the version it found. The Bluetooth route
has no such check, because it uses the public WinRT GATT contract alone.
The family check assumes Windows loaded a member of that family. It does not
establish the exact bytes already mapped in the service process, and it does
not prove that a new build inside the family kept the format. The reader's
layout validation on every view is what rejects such a build, before any write.

The service connection also checks its peer against the service manager's
session child. Every view is bounded and its native ID must agree with its
provider string. Descriptor metadata alone does not establish received input.
The adapter path supports the Microsoft receiver IDs `02E6`, `02FE`, `02F9`,
and `091E` with a qualified XboxGIP binding. Device-ID construction and the
XInput interface count were checked against the Windows driver instructions.
Offline instruction replay and controller-association tests cover the adapter
path. Physical adapter receipt and firmware 5.23.6.0 are not hardware-tested.

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
`received_0c`, `received_gatt`, `gaps`, `enable_status`, `command_attempt`,
`resends`, `pairing_losses`, `focus_changes`, `pairing_armed`, `source` (0 none,
1 service, 2 provider), `provider_20`, `provider_0c`, `provider_discarded`,
`source_switches`, `gatt_phase`, `gatt_streaming`, `gatt_error`, `gatt_hresult`,
`gatt_retries`, `gatt_rearms`, `data_available`, and `error`. When Open
declines the route, `data_available` is false and `error` says why, for
example which installed file sits outside its qualified family. The private
`SDL.joystick.xinput.paddle_mask` property describes capability and is absent
on a declined route. These properties are diagnostic, not a public SDL API
contract. Trace records add
`source-changed`, `gatt-retry`, and `gatt-rearm`, and raw records carry their
origin: `service`, `wgi`, or `gatt`.

Set `SDL_JOYSTICK_XINPUT_PADDLE_TRACE` to `1` before Open and enable input debug
logging for bounded passive traces. Records include WGI resume/suspend and raw
messages, service reading generations and raw sequences, payload bytes, decoded
masks, queue/drain transitions, command times and HRESULTs, pairing loss,
foreground changes, and re-arm decisions with their attempt numbers. The library
keeps owned bounded records and logs them on SDL's thread. It writes no files.
Overflow is reported. Source timestamps and host QPC values are separate fields.

Offline regression tests are in `test/testxinputpaddle*`. They cover the decoded
capture cases, mapping precedence, identity, retained history, queue ordering,
and teardown. They do not substitute for transport receipt evidence.
