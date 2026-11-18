/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/*
 * PadForge fork: WinRT BLE-GATT joystick driver for the wireless Nintendo
 * Switch 2 Pro Controller / Joy-Con 2. See SDL_ble_switch2joystick_design.md
 * and hifihedgehog/SDL#5. The Switch 2 advertises a custom 128-bit GATT
 * service over Bluetooth LE (not HID-over-GATT), so hidapi never sees it and
 * the upstream SDL_hidapi_switch2.c Bluetooth path stays a stub. This driver
 * owns a WinRT BLE connection, modeled on the WinRT-from-C mechanics of SDL's
 * Windows.Gaming.Input driver (SDL_windows_gaming_input.c, which upstream
 * 81b4769b0e removed). The WinRT transport it shares with
 * the generic BLE GATT driver lives in SDL_ble_gatt.c (hifihedgehog/SDL#33
 * Part 12): the runtime, the awaits, the delegates, the advertisement watcher,
 * the open, the uncached discovery and the writes.
 *
 * The report offsets come from the reference reimplementations (ndeadly,
 * Nadeflore, joycon2cpp) and, for a Joy-Con 2 (R), the OEM capture of
 * PadForge discussion #491. The console path of the Joy-Con 2 clones follows
 * joycon2android (hifihedgehog/SDL#38). test/ble-driver runs this driver
 * against a scripted Switch 2 (testblegattswitch2.c).
 */

#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_BLE

#include "../SDL_sysjoystick.h"
#include "../SDL_joystick_c.h"
#include "../usb_ids.h"
#include "../../core/windows/SDL_windows.h"

// The Switch 2 GATT/connection-parameter interfaces need a high API contract.
#ifndef WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION
#define WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION 0xe0000
#endif

#define COBJMACROS
#include <windows.devices.bluetooth.h>
#include <windows.devices.bluetooth.advertisement.h>
#include <windows.devices.bluetooth.genericattributeprofile.h>
#include <windows.storage.streams.h>
#include <windows.foundation.h>
#include <roapi.h>
#include <objidlbase.h>

#include "SDL_ble_gatt.h"

#include <initguid.h>

// ---------------------------------------------------------------------------
// Short aliases for the very long WinRT-from-C symbol names. The transport's
// own aliases live in SDL_ble_gatt.c.
// ---------------------------------------------------------------------------

typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice                           BleDevice;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3                          BleDevice3;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3                        GattService3;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic                        GattChar;

// ---------------------------------------------------------------------------
// IIDs. The WinRT C headers ship only declarations, so we define the GUIDs
// ourselves (the approach of SDL_windows_gaming_input.c, which upstream
// 81b4769b0e removed). The transport's
// IIDs live in SDL_ble_gatt.c.
// ---------------------------------------------------------------------------
DEFINE_GUID(IID_BleDevice3,       0xaee9e493, 0x44ac, 0x40dc, 0xaf, 0x33, 0xb2, 0xc1, 0x3c, 0x01, 0xca, 0x46);

// The custom Switch 2 GATT service and its characteristics (all controllers).
DEFINE_GUID(GUID_Switch2Service,    0xab7de9be, 0x89fe, 0x49ad, 0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd0);
DEFINE_GUID(GUID_Switch2Input,      0xab7de9be, 0x89fe, 0x49ad, 0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd2);
DEFINE_GUID(GUID_Switch2Command,    0x649d4ac9, 0x8eb7, 0x4e6c, 0xaf, 0x44, 0x1e, 0xa5, 0x4f, 0xe5, 0xf0, 0x05);
DEFINE_GUID(GUID_Switch2CmdResponse,0xc765a961, 0xd9d8, 0x4d36, 0xa2, 0x0a, 0x53, 0x15, 0xb1, 0x11, 0x83, 0x6a);
// Per-controller-type vibration characteristics (handle 0x0012).
DEFINE_GUID(GUID_Switch2VibePro,  0xcc483f51, 0x9258, 0x427d, 0xa9, 0x39, 0x63, 0x0c, 0x31, 0xf7, 0x2b, 0x05);
DEFINE_GUID(GUID_Switch2VibeJCL,  0x289326cb, 0xa471, 0x485d, 0xa8, 0xf4, 0x24, 0x0c, 0x14, 0xf1, 0x82, 0x41);
DEFINE_GUID(GUID_Switch2VibeJCR,  0xfa19b0fb, 0xcd1f, 0x46a7, 0x84, 0xa1, 0xbb, 0xb0, 0x9e, 0x00, 0xc1, 0x49);
DEFINE_GUID(GUID_Switch2VibeGC,   0x3f8fb670, 0xab25, 0x45bf, 0xb5, 0x40, 0x38, 0xc7, 0x28, 0x34, 0xd0, 0x64);

// The channel a Switch 2 console opens on a Joy-Con 2 (hifihedgehog/SDL#38):
// the session service and its start characteristic (handles 0x0001-0x0007),
// each side's command (0x0016), input (0x000E) and extended response
// (0x001E), and the report-rate descriptor of the input (0x0010), from the
// GATT table in switch2_controller_research bluetooth_interface.md and
// joycon2android ConsoleChannel.kt.
DEFINE_GUID(GUID_Switch2SessionService,     0x00c5af5d, 0x1964, 0x4e30, 0x8f, 0x51, 0x19, 0x56, 0xf9, 0x6b, 0xd2, 0x80);
DEFINE_GUID(GUID_Switch2SessionStart,       0x00c5af5d, 0x1964, 0x4e30, 0x8f, 0x51, 0x19, 0x56, 0xf9, 0x6b, 0xd2, 0x82);
DEFINE_GUID(GUID_Switch2ConsoleCommandJCL,  0xce49a830, 0xdced, 0x48ae, 0x93, 0x1e, 0xc8, 0xcf, 0x88, 0xaa, 0xdb, 0xea);
DEFINE_GUID(GUID_Switch2ConsoleCommandJCR,  0x65a724b3, 0xf1e7, 0x4a61, 0x80, 0x78, 0xa3, 0x42, 0x37, 0x6b, 0x27, 0xff);
DEFINE_GUID(GUID_Switch2NativeInputJCL,     0xcc1bbbb5, 0x7354, 0x4d32, 0xa7, 0x16, 0xa8, 0x1c, 0xb2, 0x41, 0xa3, 0x2a);
DEFINE_GUID(GUID_Switch2NativeInputJCR,     0xd5a9e01e, 0x2ffc, 0x4cca, 0xb2, 0x0c, 0x8b, 0x67, 0x14, 0x2b, 0xf4, 0x42);
DEFINE_GUID(GUID_Switch2ExtendedResponseJCL, 0x63a3810f, 0xaec7, 0x474b, 0x90, 0x10, 0x3d, 0x52, 0x40, 0x3c, 0xb9, 0x96);
DEFINE_GUID(GUID_Switch2ExtendedResponseJCR, 0x640ca58e, 0x0e88, 0x410c, 0xa7, 0xf3, 0x42, 0x6f, 0xaf, 0x2b, 0x69, 0x0b);
DEFINE_GUID(GUID_Switch2ReportRate,         0x679d5510, 0x5a24, 0x4dee, 0x95, 0x57, 0x95, 0xdf, 0x80, 0x48, 0x6e, 0xcb);

#define NINTENDO_BLE_COMPANY_ID 0x0553

// How long a Joy-Con 2 may stay silent on the unified input after its
// notifications are enabled before the console session starts. A genuine
// Joy-Con 2 streams within about 100 ms of the write (tarabdaar
// JoyConBLE.swift altFallbackDelay), and joycon2android waits 1.5 s
// (JoyconConnection.kt CONSOLE_FALLBACK_MS).
#define BLE_CONSOLE_FALLBACK_MS 1500

// A command for the console channel follows 17 zero bytes: report ID 00 and
// 16 bytes of HD rumble (bluetooth_interface.md handle 0x0016, joycon2android
// ConsoleChannel.COMMAND_PREFIX_LENGTH).
#define BLE_CONSOLE_PREFIX_LENGTH 17

// How long a command on the console channel waits for its reply, as
// joycon2android ConsoleTransport waits (REPLY_TIMEOUT_MS). The unified path
// keeps its 500 ms.
#define BLE_CONSOLE_REPLY_MS 700

// The longest command frame, header included. The 0x0A/0x08 vibration
// command is the longest the driver builds, 8 + 20 bytes.
#define BLE_COMMAND_FRAME_MAX 64

// The value handlers' characteristic index: the unified input, or the
// side's own input on the console channel
#define BLE_INPUT_UNIFIED 0
#define BLE_INPUT_NATIVE  1

// Keep-alive cadence for the continuous-drive rumble pump (about 100 Hz). The
// reference paces its ESP32 bridge at 7.5 ms; on a direct BLE link 10 ms sustains
// the effect without flooding the shared connection.
#define BLE_RUMBLE_INTERVAL_MS 10

// How often Detect has the transport check the shared watcher, the BLE GATT
// driver's cadence (BLEGATT_WATCHER_CHECK_MS in SDL_blegattjoystick.c)
#define BLE_WATCHER_CHECK_MS 2000

// The spacing of the console session's commands whose replies the driver
// does not use: FlexInput's INIT_CMD_GAP, the longest its write_quick waits
// after each (crates/joycon2/src/hub.rs:30-40 and :1432-1443 at 0905b2fe).
// Its earlier init waited up to 2 s on every reply, near 40 s in all, and the
// controller powered itself off partway through.
#define BLE_CONSOLE_COMMAND_GAP_MS 25

// How long a lost link may take to come back, and how often the status is
// read meanwhile: FlexInput's RECONNECT_WAIT and RECONNECT_POLL (hub.rs:709-714),
// for a link that Windows connects again by itself while a GattSession holds
// MaintainConnection (hifihedgehog/SDL#49)
#define BLE_RECOVERY_WAIT_MS 12000
#define BLE_RECOVERY_POLL_MS 250

// Discovery tries once the status reads Connected again. Windows fails
// discovery for a moment while it still holds the dropped link's GATT state,
// and FlexInput retries it three times (hub.rs:790-831).
#define BLE_RECOVERY_DISCOVERY_ATTEMPTS 3

// How long a controller may keep reporting after the app's shutdown command
// (command 0x06, subcommand 0x02) and still count as having obeyed it. No
// source times a controller's power-off. One that reports later than this
// ignored the command, as a Joy-Con 2 clone might, and its next loss is
// recovered like any other (hifihedgehog/SDL#50).
#define BLE_SHUTDOWN_GRACE_MS 2000

// BLE_Controller::recovery. While it is RUNNING, the recovery thread owns the
// controller's WinRT objects and the joystick thread touches none of them.
#define BLE_RECOVERY_IDLE      0 // no recovery runs
#define BLE_RECOVERY_RUNNING   1 // the recovery thread runs
#define BLE_RECOVERY_RESTORED  2 // it brought the link back
#define BLE_RECOVERY_FAILED    3 // the link did not come back, or Quit stopped it
#define BLE_RECOVERY_ABANDONED 4 // Quit let go of the controller while it ran

// How long SDL_Quit waits for the connect and recovery threads, the BLE GATT
// driver's BLEGATT_QUIT_WAIT_MS: under the 5 s after which Windows considers
// an application that reads no window messages not responding. Once Quit
// has told them to stop, every transport await of theirs ends within one
// 50 ms slice (SDL_BLEGATT_WaitCancelable), and the driver's own waits check
// the flag every 50 ms, so the threads end well inside the wait. One still
// running after it is blocked in a WinRT call that takes no flag.
#define BLE_QUIT_WAIT_MS 3000

// One Init-to-Quit run of the driver, for its connect and recovery threads.
// Quit sets cancel and waits for threads to reach 0, and none of those
// threads waits on the joystick lock, which Quit holds meanwhile
// (BLE_LockForThread). A thread still running after BLE_QUIT_WAIT_MS is left
// to end on its own with quiet set, so it logs nothing after SDL_Quit has
// shut the log down, as the BLE GATT driver leaves its connections
// (BLEGATT_AbandonConnection). Once cancel is set, a thread reads no clock,
// sleeps on no SDL timer and waits on no semaphore, since after SDL_Quit
// SDL_GetTicks would start SDL's tick base and timer resolution again, as
// the BLE GATT driver's ended sessions read none (BLEGATT_TakeEnds). Such a
// run is never freed.
typedef struct BLE_Run
{
    SDL_AtomicInt cancel;  // every await of the run's threads ends
    SDL_AtomicInt threads; // connect and recovery threads not yet ended
    SDL_AtomicInt quiet;   // Quit left threads running
} BLE_Run;

// Per-axis stick calibration (reimplemented; the wired versions are file-static).
typedef struct
{
    Uint16 neutral;
    Uint16 max;
    Uint16 min;
} Switch2_AxisCal;

typedef struct BLE_Controller
{
    SDL_JoystickID instance_id;
    Uint64 bluetooth_address;
    Uint16 vendor_id;
    Uint16 product_id;
    char *name;
    SDL_GUID guid;

    BleDevice *device;
    SDL_BLEGATTSession *session; // MaintainConnection, held for the controller's life
    GattChar *input_char;
    GattChar *command_char;
    GattChar *response_char;
    GattChar *vibration_char;
    EventRegistrationToken input_token;
    EventRegistrationToken response_token;
    void *input_handler;    // heap delegate, never freed, see BLE_FreeController
    void *response_handler; // heap delegate, never freed, see BLE_FreeController
    EventRegistrationToken status_token;
    void *status_handler;   // ConnectionStatusChanged delegate, leaked like the others
    SDL_AtomicInt link_lost; // set by the status callback (MTA), drained by Detect
    Uint64 connected_ms;    // when the link came up: the joystick's addition, or the last recovery

    // The recovery in place of a lost link (hifihedgehog/SDL#49), BLE_RECOVERY_*.
    // Detect starts it under the joystick lock, the recovery thread ends it,
    // and BLE_FreeController hands a running one its teardown.
    SDL_AtomicInt recovery;
    Uint64 lost_ms;        // when Detect saw the loss
    Uint64 restored_ms;    // when the recovery thread had the link back
    bool unified_answered; // a command on 649d4ac9 got its reply, connect thread only

    // A shutdown the app sent through SendEffect (hifihedgehog/SDL#50), under
    // the joystick lock. The controller powers off without a reply, so
    // Detect removes the joystick at the loss that follows, with no
    // recovery (BLE_LossAfterShutdown).
    bool shutdown_sent;
    Uint64 shutdown_counter; // SDL_GetPerformanceCounter once the frame was delivered

    // The run whose connect thread built the controller, whose cancel flag
    // its connect and recovery threads watch. quiet is set when the
    // controller is freed, after which its threads log nothing.
    BLE_Run *run;
    SDL_AtomicInt quiet;

    // A Hyperion 3 Ultra's first vendor frame, kept by the response callback
    // and logged by Detect on the joystick thread, since a callback can still
    // run after SDL_Quit has shut the log down. Under response_lock.
    Uint8 vendor_frame[64];  // its first 64 bytes
    int vendor_frame_length; // its own length
    bool vendor_frame_pending;

    // The console channel of a Joy-Con 2 clone that never notifies on the
    // unified input (hifihedgehog/SDL#38). console is written under
    // report_lock on the connect thread before the joystick is added, and
    // never changes after.
    bool console;
    GattChar *console_command_char;
    GattChar *native_input_char;
    GattChar *extended_response_char;
    EventRegistrationToken native_input_token;
    EventRegistrationToken extended_response_token;
    void *native_input_handler;      // leaked like the others
    void *extended_response_handler; // leaked like the others
    SDL_AtomicInt unified_reports;   // notifications on the unified input
    SDL_Semaphore *first_report_sem; // signaled by the first of them
    bool logged_unified_in_console;  // one-shot debug line, joystick thread
    SDL_AtomicInt logged_vendor_frame; // the first vendor frame was kept, MTA thread

    SDL_Joystick *joystick; // set in Open, NULL otherwise
    bool rumble_capable;    // a characteristic carries rumble: set by the connect, and by the joystick thread after a recovery

    // Latest input report, filled by the ValueChanged callback (MTA thread),
    // drained by Update (joystick thread).
    SDL_Mutex *report_lock;
    Uint8 report[64];
    int report_size;
    bool report_pending;
    Uint64 report_counter; // SDL_GetPerformanceCounter at the last input notification, under report_lock
    Uint8 last_state[64];
    bool have_last_state;
    bool logged_first_report; // one-shot debug dump of the first input notification

    // Command/response channel: a write to command_char produces a notification
    // on response_char. The ValueChanged handler keeps the reply to the command
    // the sender waits on, from its header on, and signals. A flash read reply
    // is 0x10 header + 0x40 data = 0x50 bytes, so size for that.
    SDL_Mutex *response_lock;
    SDL_Semaphore *response_sem;
    Uint8 response[128];
    int response_size;
    bool expecting;     // a sender waits for the reply to expect_cmd/expect_sub
    Uint8 expect_cmd;
    Uint8 expect_sub;
    bool expect_read;   // the reply must echo a memory read's length and address
    Uint8 expect_length;
    Uint32 expect_address;

    // Stick calibration (reimplemented from the wired driver; static there).
    Switch2_AxisCal left_x, left_y, right_x, right_y;
    bool calibrated; // the unified path read every stick's calibration, so a restart reads none again
    bool sensors_enabled; // IMU streams only after the enable command is sent
    bool mouse_enabled;   // Joy-Con 2 optical mouse counters stream (hint opt-in)
    bool magnetometer_enabled; // Switch 2 magnetometer sample stream (hint opt-in)
    bool vertical_mode;   // Joy-Con 2 held upright (SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS)

    // Rumble state. The actuator does not latch, so a sustained effect needs a
    // continuous packet stream. rumble_low/high are the last commanded amplitudes
    // (0/0 = idle), pumped from BLE_JoystickUpdate and rate-gated by rumble_last_ms.
    Uint32 rumble_seq;
    Uint16 rumble_low, rumble_high;
    Uint64 rumble_last_ms;
    // The GameCube motor is on/off only, so the pump runs a software PWM over the
    // command channel (command 0x0A/0x02) to fake variable strength. These are
    // touched only on the update thread (BLE_PumpGameCubeRumble), never off-thread.
    Uint8 gc_pwm_phase;
    bool gc_motor_on;
    bool gc_motor_known;
    int player_index;
} BLE_Controller;

// A 9-byte block of all 0xFF holds no calibration, the Linux v13 rule
// (switch2_parse_stick_calibration compares against nine 0xFF bytes). Parsed,
// it would give neutral = max = min = 4095 and pin the stick (hifihedgehog/SDL#38),
// so the axes keep their zeros and BLE_MapStickAxis maps them linearly.
static bool BLE_ParseStickCalibration(Switch2_AxisCal *x, Switch2_AxisCal *y, const Uint8 *data)
{
    static const Uint8 blank[9] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    if (SDL_memcmp(data, blank, sizeof(blank)) == 0) {
        return false;
    }
    x->neutral = (Uint16)(data[0] | ((data[1] & 0x0F) << 8));
    y->neutral = (Uint16)((data[1] >> 4) | (data[2] << 4));
    x->max = (Uint16)(data[3] | ((data[4] & 0x0F) << 8));
    y->max = (Uint16)((data[4] >> 4) | (data[5] << 4));
    x->min = (Uint16)(data[6] | ((data[7] & 0x0F) << 8));
    y->min = (Uint16)((data[7] >> 4) | (data[8] << 4));
    return true;
}

static Sint16 BLE_MapStickAxis(const Switch2_AxisCal *calib, float value, bool invert)
{
    Sint16 mapped;
    if (calib && calib->neutral && calib->min && calib->max) {
        value -= calib->neutral;
        value /= (value < 0) ? calib->min : calib->max;
        mapped = (Sint16)SDL_clamp(value * SDL_MAX_SINT16, SDL_MIN_SINT16, SDL_MAX_SINT16);
    } else {
        // Uncalibrated linear map of the 12-bit range.
        int scaled = ((int)value - 2048) * 16;
        mapped = (Sint16)SDL_clamp(scaled, SDL_MIN_SINT16, SDL_MAX_SINT16);
    }
    return (Sint16)(invert ? ~mapped : mapped);
}

static struct
{
    bool initialized;
    bool transport; // holds a reference from SDL_BLEGATT_Init
    bool scanning;  // BLE_OnAdvertisement is added to the shared watcher
    Uint64 watcher_checked; // when Detect last had the transport check the watcher
    BLE_Run *run;   // between Init and Quit, under the joystick lock

    BLE_Controller **controllers;
    int controller_count;

    // Addresses with a connect in progress, so repeated advertisements during
    // the (multi-second) connect don't start a storm of duplicate connects.
    Uint64 connecting[16];
    int connecting_count;

    // Per-address backoff after a full GATT-discovery failure. A Switch 2 busy on
    // USB still advertises BLE but its GATT stays Unreachable, and the always-on
    // watcher's repeated 16 s connect attempts destabilize the wired link
    // (hifihedgehog/SDL#5). A full discovery failure (all 10 attempts Unreachable)
    // is the reliable "stop trying this address" signal, distinct from a real
    // wireless connect that flips to Success within a few attempts.
    struct {
        Uint64 address;
        Uint64 until_ms;   // skip connects to this address until this tick
        int fail_count;    // escalates the backoff
    } cooldowns[16];
    int cooldown_count;
} ble;

// The advertisement callback's way in and out. The shared watcher can still
// call a listener once after it was removed, with an advertisement it copied
// before (SDL_ble_gatt.h), and SDL_Quit destroys the joystick lock the
// callback reserves an address under. So each call counts itself before it
// reads ble_listening, and Quit clears ble_listening before it reads the
// count: Quit either finds the call and waits for it, or the call finds the
// listening ended and touches nothing of the driver's or of SDL's. The BLE
// GATT driver keeps a value callback out of a freed connection the same way
// (BLEGATT_ValueReceived and BLEGATT_DetachInbox in SDL_blegattjoystick.c).
static SDL_AtomicInt ble_listening;      // between Init and Quit
static SDL_AtomicInt ble_listener_calls; // calls of BLE_OnAdvertisement not yet returned

// Forward declarations.
static void BLE_ConnectAndSubscribe(BLE_Run *run, Uint64 bluetooth_address, Uint16 vendor_id, Uint16 product_id, char *name);
static void BLE_FreeController(BLE_Controller *ctrl);
static void BLE_ReleaseTransport(BLE_Controller *ctrl);
static BLE_Controller *BLE_GetControllerByAddress(Uint64 address);
static void BLE_ReleaseConnect(BLE_Run *run, Uint64 address);

// A log line from the connect or recovery threads, which can still run
// after SDL_Quit has given up waiting for them (BLE_Run). The joystick
// thread never runs after Quit, and a WinRT callback logs nothing.
#define BLE_RUN_LOG(run, ...)                                                   \
    do {                                                                        \
        if (!SDL_GetAtomicInt(&(run)->quiet)) {                                 \
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, __VA_ARGS__);                  \
        }                                                                       \
    } while (0)
#define BLE_LOG(ctrl, ...)                                                       \
    do {                                                                        \
        if (!SDL_GetAtomicInt(&(ctrl)->quiet) && !SDL_GetAtomicInt(&(ctrl)->run->quiet)) { \
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, __VA_ARGS__);                  \
        }                                                                       \
    } while (0)

// True once Quit has told the run's threads to stop
static bool BLE_Canceled(const BLE_Controller *ctrl)
{
    return SDL_GetAtomicInt(&ctrl->run->cancel) != 0;
}

// The joystick lock for a connect thread. Quit holds that lock while it
// waits for the run's threads, so a thread that cannot take it gives up once
// the run is told to stop, and one that took it gives it back then too.
// False when the run is canceled.
static bool BLE_LockForThread(BLE_Run *run)
{
    while (!SDL_GetAtomicInt(&run->cancel)) {
        if (SDL_TryLockJoysticks()) {
            if (!SDL_GetAtomicInt(&run->cancel)) {
                return true;
            }
            SDL_UnlockJoysticks();
            break;
        }
        SDL_Delay(1);
    }
    return false;
}

// The advertisement Received callback hands the connect to this worker thread so
// the WinRT thread-pool callback returns immediately. The connect blocks on
// multi-second async opens and discovery, and blocking the callback thread starves
// the same thread pool that must deliver those completions (hifihedgehog/SDL#5:
// device=NULL on every attempt). joycon2cpp opens on its own connect thread
// (testapp.cpp:1467 and :835) and bleak dispatches to a task (discoverer.py).
typedef struct
{
    BLE_Run *run;
    Uint64 address;
    Uint16 vendor;
    Uint16 product;
} BLE_ConnectRequest;

static int SDLCALL BLE_ConnectThread(void *data)
{
    BLE_ConnectRequest *req = (BLE_ConnectRequest *)data;
    BLE_Run *run = req->run;
    bool ro_inited = false;

    // Join the MTA explicitly. This is a fresh SDL thread, not a system thread-pool
    // thread, so it has no apartment of its own. RoGetActivationFactory can return
    // RO_E_UNINITIALIZED on an uninitialized thread even with the process-wide
    // implicit MTA from CoIncrementMTAUsage, so initialize it as MULTITHREADED
    // (not WIN_RoInitialize, which is STA-first and would risk a marshal-back
    // deadlock against the agile completion handlers this thread blocks on).
    ro_inited = SDL_BLEGATT_InitThread();
    BLE_ConnectAndSubscribe(run, req->address, req->vendor, req->product, NULL);
    BLE_ReleaseConnect(run, req->address); // address reserved by the caller before spawn
    if (ro_inited) {
        SDL_BLEGATT_QuitThread();
    }
    SDL_free(req);
    // Last, since Quit may go on once the run's threads reach 0
    SDL_AddAtomicInt(&run->threads, -1);
    return 0;
}

// The joystick lock for the advertisement callback. Quit holds that lock
// while it waits for the callbacks inside the driver (ble_listener_calls),
// so a callback that cannot take it gives up once Quit has ended the
// listening. False then. The wait is the Win32 Sleep, which needs nothing of
// SDL's on a WinRT thread.
static bool BLE_LockForListener(void)
{
    while (SDL_GetAtomicInt(&ble_listening)) {
        if (SDL_TryLockJoysticks()) {
            return true;
        }
        Sleep(1);
    }
    return false;
}

// Reserve an address for connecting, on the advertisement callback. Returns
// the run the connect thread belongs to, or NULL if the address is already
// connected, a connect is already in progress, or Quit has ended the
// listening. The reservation counts the thread in the run's threads, so Quit
// waits for a thread not yet created. Caller must BLE_ReleaseConnect on
// completion, and on a failed spawn give the count back.
static BLE_Run *BLE_TryReserveConnect(Uint64 address)
{
    int i;
    BLE_Run *run = NULL;

    if (!BLE_LockForListener()) {
        return NULL;
    }
    if (ble.initialized && ble.run && !BLE_GetControllerByAddress(address)) {
        bool pending = false;
        Uint64 now = SDL_GetTicks();
        for (i = 0; i < ble.connecting_count; ++i) {
            if (ble.connecting[i] == address) {
                pending = true;
                break;
            }
        }
        // Honor an active backoff from a prior full GATT-discovery failure.
        for (i = 0; i < ble.cooldown_count; ++i) {
            if (ble.cooldowns[i].address == address && now < ble.cooldowns[i].until_ms) {
                pending = true; // not really pending, but same effect: do not connect now
                break;
            }
        }
        if (!pending && ble.connecting_count < (int)SDL_arraysize(ble.connecting)) {
            ble.connecting[ble.connecting_count++] = address;
            run = ble.run;
            SDL_AddAtomicInt(&run->threads, 1);
        }
    }
    SDL_UnlockJoysticks();
    return run;
}

// Record a full GATT-discovery failure for an address and arm an escalating
// backoff (15 s, 30 s, 60 s, ... capped at 5 min), so the watcher stops hammering
// a controller that is busy on USB. Cleared on a successful connect. Nothing
// is recorded once Quit has told the run to stop.
static void BLE_NoteConnectFailure(BLE_Run *run, Uint64 address)
{
    int i;
    Uint64 now;

    // The lock first, which fails once the run is told to stop, then the
    // clock (BLE_Run)
    if (!BLE_LockForThread(run)) {
        return;
    }
    now = SDL_GetTicks();
    for (i = 0; i < ble.cooldown_count; ++i) {
        if (ble.cooldowns[i].address == address) {
            break;
        }
    }
    if (i == ble.cooldown_count && ble.cooldown_count < (int)SDL_arraysize(ble.cooldowns)) {
        ble.cooldowns[i].address = address;
        ble.cooldowns[i].fail_count = 0;
        ble.cooldown_count++;
    }
    if (i < ble.cooldown_count) {
        int shift = ble.cooldowns[i].fail_count;
        Uint64 secs;
        if (shift > 5) {
            shift = 5;
        }
        secs = (Uint64)15 << shift; // 15, 30, 60, 120, 240, 480
        if (secs > 300) {
            secs = 300; // cap at 5 minutes
        }
        ble.cooldowns[i].fail_count++;
        ble.cooldowns[i].until_ms = now + secs * 1000;
    }
    SDL_UnlockJoysticks();
}

// Gives the address back. Once Quit has told the run to stop, Quit clears
// every reservation itself.
static void BLE_ReleaseConnect(BLE_Run *run, Uint64 address)
{
    int i;

    if (!BLE_LockForThread(run)) {
        return;
    }
    for (i = 0; i < ble.connecting_count; ++i) {
        if (ble.connecting[i] == address) {
            ble.connecting[i] = ble.connecting[--ble.connecting_count];
            break;
        }
    }
    SDL_UnlockJoysticks();
}

// Debug hex dump (visible at SDL_LOG_PRIORITY_DEBUG). Used to capture the real
// on-wire layout so the transport-specific unknowns (input report offsets, flash
// reply offset) can be confirmed against the reference on first hardware contact.
static void BLE_LogBytes(BLE_Controller *ctrl, const char *label, const Uint8 *data, int len)
{
    char hex[3 * 64 + 1];
    int i, n = SDL_min(len, 64);
    for (i = 0; i < n; ++i) {
        (void)SDL_snprintf(&hex[i * 3], 4, "%02x ", data[i]);
    }
    hex[n > 0 ? n * 3 : 0] = '\0';
    BLE_LOG(ctrl, "BLE Switch2 %s (%d bytes): %s", label, len, hex);
}

// ---------------------------------------------------------------------------
// Controller array bookkeeping.
// ---------------------------------------------------------------------------
static BLE_Controller *BLE_GetControllerByInstance(SDL_JoystickID instance_id)
{
    int i;
    for (i = 0; i < ble.controller_count; ++i) {
        if (ble.controllers[i]->instance_id == instance_id) {
            return ble.controllers[i];
        }
    }
    return NULL;
}

static BLE_Controller *BLE_GetControllerByAddress(Uint64 address)
{
    int i;
    for (i = 0; i < ble.controller_count; ++i) {
        if (ble.controllers[i]->bluetooth_address == address) {
            return ble.controllers[i];
        }
    }
    return NULL;
}

// Publishes a controller's joystick under its own instance ID, under the
// joystick lock. A controller has no path.
static bool BLE_PublishController(BLE_Controller *ctrl)
{
    SDL_JoystickDevice *device = SDL_CreateJoystickDevice(&SDL_BLE_JoystickDriver, 0);

    if (!device) {
        return false;
    }
    device->name = SDL_strdup(ctrl->name);
    if (!device->name) {
        SDL_DestroyJoystickDevice(device);
        return false;
    }
    device->guid = ctrl->guid;
    device->instance_id = ctrl->instance_id;
    return SDL_AddJoystickDevice(device) != 0;
}

// ---------------------------------------------------------------------------
// Input characteristic ValueChanged: copy the raw report into the ring slot.
// Runs on a WinRT thread-pool (MTA) thread, called by the shared value delegate
// (SDL_ble_gatt.c) with the whole value. The report keeps its first 64 bytes.
// characteristic is BLE_INPUT_UNIFIED or BLE_INPUT_NATIVE, and only the stream
// of the path the connect chose reaches the slot, so a report of the other
// layout is never decoded. The first unified report wakes the connect thread
// waiting to choose the path.
// ---------------------------------------------------------------------------
static void BLE_OnInputValue(void *userdata, int characteristic, const Uint8 *data, size_t length)
{
    BLE_Controller *ctrl = (BLE_Controller *)userdata;
    int size;

    if (!ctrl) {
        return;
    }
    size = (int)SDL_min(length, sizeof(ctrl->report));
    if (size <= 0) {
        return;
    }
    SDL_LockMutex(ctrl->report_lock);
    // When the controller last reported on either input, for a loss after a
    // shutdown the app sent (BLE_LossAfterShutdown). The performance counter
    // keeps no state of SDL's, so a callback that runs after SDL_Quit starts
    // nothing of SDL's again.
    ctrl->report_counter = SDL_GetPerformanceCounter();
    if ((characteristic == BLE_INPUT_NATIVE) == ctrl->console) {
        SDL_memcpy(ctrl->report, data, size);
        ctrl->report_size = size;
        ctrl->report_pending = true;
    }
    SDL_UnlockMutex(ctrl->report_lock);
    if (characteristic == BLE_INPUT_UNIFIED && SDL_AddAtomicInt(&ctrl->unified_reports, 1) == 0 && ctrl->first_report_sem) {
        SDL_SignalSemaphore(ctrl->first_report_sem);
    }
}

// Where the reply the sender waits on starts in a notification, or -1. A
// reply echoes the command at [i], 0x01 at [i+1] and the subcommand at [i+3]
// (joycon2android ConsoleTransport.replyStart). Replies on the extended
// response start with a run of zero bytes, the header at 0xF
// (bluetooth_interface.md handle 0x001E), and those on c765a961 start with
// it (switch2-controllers controller.py:316 checks [0] and [1]). A memory
// read's reply also echoes the length at [i+8] and the address at [i+12],
// which controller.py read_memory checks (:345), so a late reply to an
// earlier read is not taken for this one. Called under response_lock.
static int BLE_FindReply(const BLE_Controller *ctrl, const Uint8 *data, int length)
{
    int i;

    for (i = 0; i + 8 <= length; ++i) {
        if (data[i] != ctrl->expect_cmd || data[i + 1] != 0x01 || data[i + 3] != ctrl->expect_sub) {
            continue;
        }
        if (ctrl->expect_read &&
            (i + 16 > length || data[i + 8] != ctrl->expect_length ||
             (Uint32)(data[i + 12] | (data[i + 13] << 8) | (data[i + 14] << 16) | ((Uint32)data[i + 15] << 24)) != ctrl->expect_address)) {
            continue;
        }
        return i;
    }
    return -1;
}

// NYXI Hyperion 3 Ultra vendor input on c765a961 (tarabdaar
// JoyConNYXIReport.swift: a frame of at least 16 bytes that starts
// EA 01 00 8B 00 78 00 00 0C). Not decoded, only logged.
static bool BLE_IsVendorFrame(const Uint8 *data, size_t length)
{
    static const Uint8 header[] = { 0xEA, 0x01, 0x00, 0x8B, 0x00, 0x78, 0x00, 0x00, 0x0C };

    return length >= 16 && SDL_memcmp(data, header, sizeof(header)) == 0;
}

// ---------------------------------------------------------------------------
// Command-response ValueChanged, for c765a961 and on the console channel the
// side's extended response: keep the reply to the command a sender waits on,
// from its header on, and signal. A notification that holds no such reply,
// such as a reply that came after its sender stopped waiting or a vendor
// frame, is not a reply (hifihedgehog/SDL#38). Called by the shared value
// delegate, like BLE_OnInputValue. The reply keeps its first 128 bytes. The
// first vendor frame is kept for Detect to log (BLE_Controller::vendor_frame).
// ---------------------------------------------------------------------------
static void BLE_OnResponseValue(void *userdata, int characteristic, const Uint8 *data, size_t length)
{
    BLE_Controller *ctrl = (BLE_Controller *)userdata;
    int size = (int)SDL_min(length, (size_t)SDL_MAX_SINT32);
    bool matched = false;
    (void)characteristic;

    if (!ctrl) {
        return;
    }
    SDL_LockMutex(ctrl->response_lock);
    if (ctrl->expecting) {
        const int start = BLE_FindReply(ctrl, data, size);
        if (start >= 0) {
            ctrl->response_size = SDL_min(size - start, (int)sizeof(ctrl->response));
            SDL_memcpy(ctrl->response, &data[start], ctrl->response_size);
            ctrl->expecting = false;
            matched = true;
        }
    }
    if (!matched && BLE_IsVendorFrame(data, length) && SDL_CompareAndSwapAtomicInt(&ctrl->logged_vendor_frame, 0, 1)) {
        // BLE_LogBytes dumps the first 64 bytes, all that is kept
        SDL_memcpy(ctrl->vendor_frame, data, SDL_min(size, (int)sizeof(ctrl->vendor_frame)));
        ctrl->vendor_frame_length = size;
        ctrl->vendor_frame_pending = true;
    }
    SDL_UnlockMutex(ctrl->response_lock);
    if (matched) {
        SDL_SignalSemaphore(ctrl->response_sem);
    }
}

// A command frame: [cmd] 0x91 0x01 [subcmd] 0x00 [data_len] 0x00 0x00 [data...]
// (commands.md "Command Header", 0x01 the Bluetooth transport). Returns its
// length, or 0 when it would not fit BLE_COMMAND_FRAME_MAX.
static int BLE_BuildCommand(Uint8 *frame, Uint8 cmd, Uint8 subcmd, const Uint8 *data, int data_len)
{
    if (data_len < 0 || 8 + data_len > BLE_COMMAND_FRAME_MAX) {
        return 0;
    }
    frame[0] = cmd;
    frame[1] = 0x91;
    frame[2] = 0x01;
    frame[3] = subcmd;
    frame[4] = 0x00;
    frame[5] = (Uint8)data_len;
    frame[6] = 0x00;
    frame[7] = 0x00;
    if (data_len > 0) {
        SDL_memcpy(&frame[8], data, data_len);
    }
    return 8 + data_len;
}

// The one write path for command frames (hifihedgehog/SDL#38). On the
// unified path the frame goes to 649d4ac9. On the console path it goes to the
// side's command characteristic behind BLE_CONSOLE_PREFIX_LENGTH zero bytes,
// as joycon2android ConsoleTransport.send writes it. Both characteristics
// take only writes without response (bluetooth_interface.md, handles 0x0014
// and 0x0016), so SDL_BLEGATT_WriteCharacteristic writes them that way
// whatever prefer_response asks, as joycon2android and tarabdaar do.
static bool BLE_WriteCommandFrame(BLE_Controller *ctrl, const Uint8 *frame, int length, bool prefer_response)
{
    Uint8 prefixed[BLE_CONSOLE_PREFIX_LENGTH + BLE_COMMAND_FRAME_MAX];

    if (length <= 0 || length > BLE_COMMAND_FRAME_MAX) {
        return false;
    }
    if (ctrl->console) {
        if (!ctrl->console_command_char) {
            return false;
        }
        SDL_memset(prefixed, 0, BLE_CONSOLE_PREFIX_LENGTH);
        SDL_memcpy(&prefixed[BLE_CONSOLE_PREFIX_LENGTH], frame, length);
        return SDL_BLEGATT_WriteCharacteristic(ctrl->console_command_char, prefixed, BLE_CONSOLE_PREFIX_LENGTH + length, prefer_response,
                                               &ctrl->run->cancel);
    }
    if (!ctrl->command_char) {
        return false;
    }
    return SDL_BLEGATT_WriteCharacteristic(ctrl->command_char, frame, length, prefer_response, &ctrl->run->cancel);
}

static bool BLE_HasCommandChannel(const BLE_Controller *ctrl)
{
    return ctrl->console ? (ctrl->console_command_char != NULL) : (ctrl->command_char != NULL);
}

// Waits up to timeout_ms for a reply's signal, in 50 ms slices, so Quit's
// stop ends the wait. The slices are counted, so the wait reads no clock
// (BLE_Run).
static bool BLE_WaitReply(BLE_Controller *ctrl, Sint32 timeout_ms)
{
    Sint32 waited = 0;

    while (waited < timeout_ms) {
        const Sint32 slice = SDL_min(50, timeout_ms - waited);

        if (BLE_Canceled(ctrl)) {
            return false;
        }
        if (SDL_WaitSemaphoreTimeout(ctrl->response_sem, slice)) {
            return true;
        }
        waited += slice;
    }
    return false;
}

// Send a command and wait (briefly) for its reply. The reply is the
// notification that echoes the command (BLE_FindReply), copied from its
// header on. read asks the reply to echo a memory read of read_length bytes
// at read_address. Returns the number of reply bytes copied, or 0 on timeout
// or once Quit has told the run to stop.
static int BLE_SendCommandFor(BLE_Controller *ctrl, Uint8 cmd, Uint8 subcmd, const Uint8 *data, int data_len, Uint8 *reply, int reply_len,
                              bool read, Uint8 read_length, Uint32 read_address)
{
    Uint8 frame[BLE_COMMAND_FRAME_MAX];
    const int length = BLE_BuildCommand(frame, cmd, subcmd, data, data_len);
    int got = 0;

    if (length == 0 || !BLE_HasCommandChannel(ctrl) || !ctrl->response_sem || BLE_Canceled(ctrl)) {
        return 0;
    }
    // A signal left by a reply that matched after an earlier sender gave up.
    // Nothing matches between this and the expectation below, since none is
    // set.
    while (SDL_TryWaitSemaphore(ctrl->response_sem)) {
    }
    SDL_LockMutex(ctrl->response_lock);
    ctrl->expect_cmd = cmd;
    ctrl->expect_sub = subcmd;
    ctrl->expect_read = read;
    ctrl->expect_length = read_length;
    ctrl->expect_address = read_address;
    ctrl->expecting = true;
    ctrl->response_size = 0;
    SDL_UnlockMutex(ctrl->response_lock);
    if (BLE_WriteCommandFrame(ctrl, frame, length, true) && BLE_WaitReply(ctrl, ctrl->console ? BLE_CONSOLE_REPLY_MS : 500)) {
        SDL_LockMutex(ctrl->response_lock);
        got = ctrl->response_size;
        if (reply && reply_len > 0) {
            got = SDL_min(got, reply_len);
            SDL_memcpy(reply, ctrl->response, got);
        }
        SDL_UnlockMutex(ctrl->response_lock);
    }
    SDL_LockMutex(ctrl->response_lock);
    ctrl->expecting = false;
    SDL_UnlockMutex(ctrl->response_lock);
    return got;
}

static int BLE_SendCommand(BLE_Controller *ctrl, Uint8 cmd, Uint8 subcmd, const Uint8 *data, int data_len, Uint8 *reply, int reply_len)
{
    return BLE_SendCommandFor(ctrl, cmd, subcmd, data, data_len, reply, reply_len, false, 0, 0);
}

// Read controller memory, transcribed from the BLE reference controller.py
// read_memory (switch2-controllers/controller.py:339-347): command 0x02/0x04
// with data {length, 0x7e, 0, 0, addr LE}. controller.py strips an 8-byte header
// in write_command and another 8 in read_memory, so the payload sits at raw reply
// offset 0x10. The reply taken echoes the length and the address
// (BLE_FindReply). Returns the number of payload bytes copied (0 on failure).
static int BLE_ReadMemory(BLE_Controller *ctrl, Uint8 length, Uint32 addr, Uint8 *out, int out_len)
{
    Uint8 req[8] = { length, 0x7e, 0x00, 0x00, (Uint8)addr, (Uint8)(addr >> 8), (Uint8)(addr >> 16), (Uint8)(addr >> 24) };
    Uint8 reply[128];
    int got = BLE_SendCommandFor(ctrl, 0x02, 0x04, req, (int)sizeof(req), reply, (int)sizeof(reply), true, length, addr);
    int n;

    BLE_LOG(ctrl, "BLE Switch2 memory read addr 0x%06x len %d: %d reply bytes", (unsigned)addr, length, got);
    if (got > 0) {
        BLE_LogBytes(ctrl, "memory reply", reply, got); // confirms the 0x10 payload offset + MTU
    }
    if (got < 0x10) {
        return 0;
    }
    n = SDL_min(got - 0x10, (int)length);
    n = SDL_min(n, out_len);
    SDL_memcpy(out, &reply[0x10], n);
    return n;
}

// A user calibration block: the B2 A1 magic, then the 9-byte stick block
// (Linux v13 NS2_USER_CALIB_MAGIC 0xa1b2 read little-endian, and the wired
// driver, SDL_hidapi_switch2.c:614-626). False when the magic is missing or
// the block is blank.
static bool BLE_ParseUserCalibration(Switch2_AxisCal *x, Switch2_AxisCal *y, const Uint8 *data, int length)
{
    return length >= 0x0b && data[0] == 0xB2 && data[1] == 0xA1 && BLE_ParseStickCalibration(x, y, &data[2]);
}

// Read one stick's calibration over the command channel: the user block
// (0x0B bytes at 0x1FC040 or 0x1FC080), else the factory block (0x0130A8 or
// 0x0130E8), as Linux v13 and the wired driver pick them, a user block
// replacing the factory one. The addresses are those of Linux
// (NS2_FLASH_ADDR_*_CALIB) and the wired driver, and replace the 0x1FC042 and
// 0x1FC062 of controller.py read_calibration_data
// (switch2-controllers/controller.py:353-368), which tested only the first
// three bytes of a user block (hifihedgehog/SDL#38). A Joy-Con stores its
// single-stick calibration in the first slot. The decoders use left_x/left_y
// for the single Joy-Con stick, so both L and R store there. Best-effort: on
// failure BLE_MapStickAxis falls back to a linear map. A reply to either read
// marks a controller that answers 649d4ac9 (BLE_ConnectAndSubscribe).
static bool BLE_ReadCalibSlot(BLE_Controller *ctrl, Uint32 user_addr, Uint32 factory_addr, Switch2_AxisCal *x, Switch2_AxisCal *y)
{
    Uint8 cal[16];
    int got = BLE_ReadMemory(ctrl, 0x0b, user_addr, cal, sizeof(cal));

    if (got > 0) {
        ctrl->unified_answered = true;
    }
    if (BLE_ParseUserCalibration(x, y, cal, got)) {
        return true;
    }
    got = BLE_ReadMemory(ctrl, 0x0b, factory_addr, cal, sizeof(cal));
    if (got > 0) {
        ctrl->unified_answered = true;
    }
    return got >= 9 && BLE_ParseStickCalibration(x, y, cal);
}

// Every stick's calibration, and whether each was read. A slot that gives
// none keeps the calibration it had (BLE_ReadCalibSlot).
static void BLE_ReadCalibration(BLE_Controller *ctrl)
{
    bool complete = BLE_ReadCalibSlot(ctrl, 0x1FC040, 0x0130A8, &ctrl->left_x, &ctrl->left_y);

    // Pro/GameCube have a second stick; a Joy-Con uses only the first slot.
    if (ctrl->product_id != USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT &&
        ctrl->product_id != USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT) {
        if (!BLE_ReadCalibSlot(ctrl, 0x1FC080, 0x0130E8, &ctrl->right_x, &ctrl->right_y)) {
            complete = false;
        }
    }
    ctrl->calibrated = complete;
}

// Player LED (command 0x09 / subcmd 0x07). The wired UpdateSlotLED
// (SDL_hidapi_switch2.c:248-265) sends 8 data bytes with the pattern in data[0].
static void BLE_SetPlayerLED(BLE_Controller *ctrl, int player_index)
{
    static const Uint8 pattern[] = { 0x1, 0x3, 0x7, 0xf, 0x9, 0x5, 0xd, 0x6 };
    Uint8 data[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (player_index >= 0) {
        data[0] = pattern[player_index % 8];
    }
    BLE_SendCommand(ctrl, 0x09, 0x07, data, sizeof(data), NULL, 0);
}

// The unified path's feature flags, through the BLE reference's
// enableFeatures (controller.py:370-373): command 0x0c/0x02 (init) then
// 0x0c/0x04 (enable), both carrying the mask. FEATURE_MOTION = 0x04
// (controller.py:59). The wired driver's 0x27 is a USB-path value and sets
// unrelated bits here. The mask is the union of every feature this driver
// keeps active: the proven pattern for multiple features is one combined
// init+enable (windows10-gyro controller.py:802 sends MOTION | MOUSE |
// MAGNETOMETER in a single call), so a motion-only toggle must not drop the
// mouse or magnetometer bits.
static void BLE_SendFeatures(BLE_Controller *ctrl)
{
    const Uint8 flags[4] = { (Uint8)((ctrl->sensors_enabled ? 0x04 : 0x00) |
                                     (ctrl->mouse_enabled ? 0x10 : 0x00) |
                                     (ctrl->magnetometer_enabled ? 0x80 : 0x00)),
                             0x00, 0x00, 0x00 };

    BLE_SendCommand(ctrl, 0x0c, 0x02, flags, sizeof(flags), NULL, 0);
    BLE_SendCommand(ctrl, 0x0c, 0x04, flags, sizeof(flags), NULL, 0);
}

// VibrationData 5-byte LE bit-pack, transcribed from controller.py
// VibrationData.get_bytes (switch2-controllers/controller.py:196-209): lf_freq
// at bits 0-8, en_lf at 9, lf_amp at 10-19, hf_freq at 20-28, en_hf at 29, hf_amp
// at 30-39. The BLE reference (virtual_controller.py:29-31) scales amplitude as
// 800*motor/256 (so ~0..800 of the 10-bit field) and leaves the en_tone bits 0.
// Fire-and-forget command write (no reply wait). The GameCube rumble PWM toggles
// faster than a request/response round-trip allows, so it cannot use BLE_SendCommand
// (which blocks on the response). Same frame layout. Mirrors the gyro reference's
// write_gatt_char(..., response=False) (switch2-controllers-windows10-gyro
// controller.py _gc_pwm_loop).
static bool BLE_WriteCommandNoReply(BLE_Controller *ctrl, Uint8 cmd, Uint8 subcmd, const Uint8 *data, int data_len)
{
    Uint8 frame[BLE_COMMAND_FRAME_MAX];
    const int length = BLE_BuildCommand(frame, cmd, subcmd, data, data_len);

    return length != 0 && !BLE_Canceled(ctrl) && BLE_WriteCommandFrame(ctrl, frame, length, false);
}

// A console-session command whose reply the driver does not use, written
// without waiting for it and followed by BLE_CONSOLE_COMMAND_GAP_MS, the
// longest FlexInput's write_quick waits after each (hub.rs:1432-1443). A
// reply that comes later matches no sender, since a reply must echo the
// command and, for a read, its length and address (BLE_FindReply). Nothing
// is written once Quit has told the run to stop.
static void BLE_SendConsoleCommand(BLE_Controller *ctrl, Uint8 cmd, Uint8 subcmd, const Uint8 *data, int data_len)
{
    if (BLE_WriteCommandNoReply(ctrl, cmd, subcmd, data, data_len)) {
        SDL_Delay(BLE_CONSOLE_COMMAND_GAP_MS);
    }
}

// A console-session memory read whose data the driver does not use, as
// BLE_ReadMemory frames it
static void BLE_RequestConsoleRead(BLE_Controller *ctrl, Uint8 length, Uint32 addr)
{
    const Uint8 req[8] = { length, 0x7e, 0x00, 0x00, (Uint8)addr, (Uint8)(addr >> 8), (Uint8)(addr >> 16), (Uint8)(addr >> 24) };

    BLE_SendConsoleCommand(ctrl, 0x02, 0x04, req, (int)sizeof(req));
}

// GameCube motor on/off, via command 0x0A subcommand 0x02. Motor on = data byte
// 0x01, off = 0x00 (switch2-controllers-windows10-gyro controller.py _gc_pwm_loop:
// payload 0A 91 01 02 00 04 00 00 [01|00] 00 00 00).
static void BLE_SetGameCubeMotor(BLE_Controller *ctrl, bool on)
{
    Uint8 data[4] = { (Uint8)(on ? 0x01 : 0x00), 0x00, 0x00, 0x00 };
    BLE_WriteCommandNoReply(ctrl, 0x0A, 0x02, data, sizeof(data));
}

// The GameCube controller's motor only supports on/off natively, so the gyro
// reference simulates variable strength with a ~25 Hz software PWM over the command
// channel (controller.py _gc_pwm_loop: "it only supports ON/OFF natively"). Run the
// same PWM from the keep-alive pump: each ~10 ms tick is one of 4 slots in a 40 ms
// period, the motor is on for round(amp * 4) of them. GC amplitude scales lf*0.5,
// hf*0.1 (controller.py gc_lf_scale / gc_hf_scale). Only writes on a state change,
// like the reference's last_is_on, so it does not flood the command channel.
static void BLE_PumpGameCubeRumble(BLE_Controller *ctrl, Uint16 low, Uint16 high)
{
    float amp = SDL_max((float)low * 0.5f, (float)high * 0.1f) / 65535.0f;
    bool desired;

    if (amp <= 0.02f) {
        desired = false;
    } else if (amp >= 0.98f) {
        desired = true;
    } else {
        int on_slots = (int)(amp * 4.0f + 0.5f);
        desired = ((int)ctrl->gc_pwm_phase < on_slots);
    }
    ctrl->gc_pwm_phase = (Uint8)((ctrl->gc_pwm_phase + 1) & 0x03);
    if (!ctrl->gc_motor_known || desired != ctrl->gc_motor_on) {
        BLE_SetGameCubeMotor(ctrl, desired);
        ctrl->gc_motor_on = desired;
        ctrl->gc_motor_known = true;
    }
}

static void BLE_EncodeVibration(Uint16 low, Uint16 high, Uint8 out[5])
{
    Uint32 lf_amp = (Uint32)((int)low * 800 / 65535) & 0x3FF;
    Uint32 hf_amp = (Uint32)((int)high * 800 / 65535) & 0x3FF;
    Uint64 v = (0x0E1ULL & 0x1FF) |        // lf_freq default (en_lf bit 9 stays 0)
               ((Uint64)lf_amp << 10) |    // lf_amp
               ((0x1E1ULL & 0x1FF) << 20) | // hf_freq default (en_hf bit 29 stays 0)
               ((Uint64)hf_amp << 30);     // hf_amp
    out[0] = (Uint8)v;
    out[1] = (Uint8)(v >> 8);
    out[2] = (Uint8)(v >> 16);
    out[3] = (Uint8)(v >> 24);
    out[4] = (Uint8)(v >> 32);
}

// Write a vibration packet, transcribed from controller.py set_vibration
// (switch2-controllers/controller.py:288-302): 0x00 + packet_id + the amplitude
// VibrationData + two default (zero-amplitude) VibrationData blocks. Pro repeats
// the 16-byte motor group (L then R). SDL re-calls Rumble periodically
// (SDL_RUMBLE_RESEND_MS), which sustains the effect.
static bool BLE_WriteRumble(BLE_Controller *ctrl, Uint16 low, Uint16 high)
{
    Uint8 group[16];
    Uint8 packet[33];
    Uint8 vib[5], zero[5];
    int len;

    // The NSO GameCube controller does not take the VibrationData motor group. Its
    // motor is on/off only, driven over the command channel with a software PWM for
    // strength (see BLE_PumpGameCubeRumble), so route it there before the
    // vibration-characteristic check (which the GC path does not use).
    if (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER) {
        BLE_PumpGameCubeRumble(ctrl, low, high);
        return true;
    }
    if (!ctrl->vibration_char) {
        return false;
    }
    BLE_EncodeVibration(low, high, vib);
    BLE_EncodeVibration(0, 0, zero); // default VibrationData (default freq, 0 amp)
    group[0] = (Uint8)(0x50 | (ctrl->rumble_seq & 0x0F));
    SDL_memcpy(&group[1], vib, 5);
    SDL_memcpy(&group[6], zero, 5);
    SDL_memcpy(&group[11], zero, 5);
    ctrl->rumble_seq++;

    packet[0] = 0x00;
    SDL_memcpy(&packet[1], group, 16);
    len = 17;
    if (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_PRO) {
        SDL_memcpy(&packet[17], group, 16);
        len = 33;
    }
    return SDL_BLEGATT_WriteCharacteristic(ctrl->vibration_char, packet, len, false, &ctrl->run->cancel);
}

// ---------------------------------------------------------------------------
// Advertisement listener: match Nintendo BLE company id, parse VID/PID, connect.
// The shared watcher (SDL_ble_gatt.c) calls it on a WinRT thread-pool thread
// with the parsed event, whose manufacturer data holds up to 32 bytes after the
// company id, as the copy here always has. The event carries at most 4
// manufacturer entries (SDL_BLE_AD_MANUFACTURER), and the Switch 2 advertises
// one 0x0553 section beside its Flags (switch2-bt RESEARCH.md:285-286, and
// btstack_build/sw2d_btstack.c:152-166 finds the controller by that section).
// ---------------------------------------------------------------------------
static void BLE_TakeAdvertisement(const SDL_BLEAdvertisement *advertisement)
{
    int i;

    for (i = 0; i < advertisement->nmanufacturer; ++i) {
        const SDL_BLEManufacturerData *mfg = &advertisement->manufacturer[i];
        if (mfg->company == NINTENDO_BLE_COMPANY_ID) {
            const Uint8 *data = mfg->data;
            int n = mfg->length;
            // Company id already stripped: vendor at [3:5], product at [5:7] LE.
            if (n >= 7) {
                Uint16 vendor = (Uint16)(data[3] | (data[4] << 8));
                Uint16 product = (Uint16)(data[5] | (data[6] << 8));
                bool supported = (vendor == USB_VENDOR_NINTENDO) &&
                                 (product == USB_PRODUCT_NINTENDO_SWITCH2_PRO ||
                                  product == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT ||
                                  product == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT ||
                                  product == USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER);
                // Reserve the address so repeated advertisements during the
                // multi-second connect don't start duplicate connects, then
                // hand the connect to a worker thread so this WinRT callback
                // returns immediately and the thread pool stays free to
                // deliver the open/discovery completions. The reservation
                // counts the thread in its run, which Quit waits for, so the
                // line below cannot come after Quit, which keeps waiting.
                BLE_Run *run = supported ? BLE_TryReserveConnect(advertisement->address) : NULL;
                if (run) {
                    BLE_ConnectRequest *req = (BLE_ConnectRequest *)SDL_malloc(sizeof(*req));
                    BLE_RUN_LOG(run, "BLE Switch2 advertisement matched: VID %04x PID %04x addr %012llx", vendor, product, (unsigned long long)advertisement->address);
                    if (req) {
                        SDL_Thread *thread;
                        req->run = run;
                        req->address = advertisement->address;
                        req->vendor = vendor;
                        req->product = product;
                        thread = SDL_CreateThread(BLE_ConnectThread, "BLESwitch2Connect", req);
                        if (thread) {
                            SDL_DetachThread(thread);
                        } else {
                            SDL_free(req);
                            BLE_ReleaseConnect(run, advertisement->address);
                            SDL_AddAtomicInt(&run->threads, -1);
                        }
                    } else {
                        BLE_ReleaseConnect(run, advertisement->address);
                        SDL_AddAtomicInt(&run->threads, -1);
                    }
                }
            }
        }
    }
}

// The listener. Counted before the flag is read (ble_listener_calls).
static void BLE_OnAdvertisement(const SDL_BLEAdvertisement *advertisement, void *userdata)
{
    (void)userdata;

    SDL_AddAtomicInt(&ble_listener_calls, 1);
    if (SDL_GetAtomicInt(&ble_listening)) {
        BLE_TakeAdvertisement(advertisement);
    }
    SDL_AddAtomicInt(&ble_listener_calls, -1);
}

// True once the unified input has reported, waiting up to timeout_ms. The
// choice is made once: a genuine pad that missed the wait keeps the console
// path until it connects anew, and a recovery in place keeps it too.
static bool BLE_AwaitUnifiedReport(BLE_Controller *ctrl, Sint32 timeout_ms)
{
    Sint32 waited = 0;

    if (!ctrl->input_handler || !ctrl->first_report_sem) {
        return false;
    }
    // In 50 ms slices, so Quit's stop ends the wait (BLE_QUIT_WAIT_MS). The
    // slices are counted, so the wait reads no clock (BLE_Run).
    while (SDL_GetAtomicInt(&ctrl->unified_reports) == 0) {
        Sint32 slice;

        if (BLE_Canceled(ctrl) || waited >= timeout_ms) {
            return false;
        }
        slice = SDL_min(50, timeout_ms - waited);
        (void)SDL_WaitSemaphoreTimeout(ctrl->first_report_sem, slice);
        waited += slice;
    }
    return true;
}

// The unified path of a Joy-Con 2, once its first report has arrived.
// The connect reads the sensor hints, and a recovery sends what the connect
// chose (read_hints false), since Open has already sized the joystick by it.
static void BLE_StartUnifiedJoyCon(BLE_Controller *ctrl, bool read_hints)
{
    // Joy-Con 2 input-mode write (Format 3 / 0x30). windows10-gyro sends this
    // raw 11-byte buffer to the command characteristic for the Joy-Cons
    // before it subscribes their input (src/controller.py:2426-2441). Its
    // comment says that without it the high status byte and the Left's bit 23
    // leak into the button field as phantom ZL and ZR. Joy-Cons stay on the
    // unified Report 0x05 layout that BLE_DecodeJoyConLeft/Right read. The
    // write now follows the first unified report, so a Joy-Con 2 clone never
    // receives it (hifihedgehog/SDL#38). The GameCube is deliberately
    // excluded: Format 3 switches it to the native Report 0x0A layout,
    // buttons at [2:5] and triggers at [12] and [13] (src/controller.py
    // :1317-1370), which this driver's GameCube decoder does not parse.
    static const Uint8 set_input_mode[] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x30 };
    if (BLE_Canceled(ctrl)) {
        return;
    }
    (void)BLE_WriteCommandFrame(ctrl, set_input_mode, (int)sizeof(set_input_mode), false);

    /* Opt-in side-channel sensors (Joy-Con only, both L and R). Feature
       flags command 0x0C: subcommand 0x02 (init) then 0x04 (enable), u32 LE
       payload, mouse = bit 4 (controller.py:55-61/370-373, enabled at :268;
       the report 0x05 Mouse Data block is "Activated via feature bit 4",
       hid_reports.md), magnetometer = bit 7 (windows10-gyro
       controller.py:713's FEATSEL 0x94 = motion | mouse | magnetometer).
       Both ride one combined init+enable, the reference's own multi-feature
       pattern, fire-and-forget like the Format-3 write above. */
    if (read_hints && SDL_GetHintBoolean(SDL_HINT_JOYSTICK_BLE_SWITCH2_MOUSE, false)) {
        ctrl->mouse_enabled = true;
    }
    if (read_hints && SDL_GetHintBoolean(SDL_HINT_JOYSTICK_BLE_SWITCH2_MAGNETOMETER, false)) {
        ctrl->magnetometer_enabled = true;
    }
    if (ctrl->mouse_enabled || ctrl->magnetometer_enabled) {
        const Uint8 feature_mask[4] = { (Uint8)((ctrl->mouse_enabled ? 0x10 : 0x00) | (ctrl->magnetometer_enabled ? 0x80 : 0x00)), 0x00, 0x00, 0x00 };
        (void)BLE_WriteCommandNoReply(ctrl, 0x0C, 0x02, feature_mask, (int)sizeof(feature_mask));
        (void)BLE_WriteCommandNoReply(ctrl, 0x0C, 0x04, feature_mask, (int)sizeof(feature_mask));
    }
}

// A value delegate on the characteristic, then its notifications. The
// controller's own delegate is registered again after a recovery, on the
// characteristic found anew, so a recovery allocates none
// (SDL_BLEGATT_ReAddValueHandler). False when no delegate is registered or
// the characteristic is missing.
static bool BLE_Subscribe(BLE_Controller *ctrl, GattChar *characteristic, void **handler, EventRegistrationToken *token,
                          SDL_BLEGATT_ValueCallback callback, int index)
{
    if (!characteristic || BLE_Canceled(ctrl)) {
        return false;
    }
    if (*handler) {
        if (!SDL_BLEGATT_ReAddValueHandler(characteristic, *handler, token)) {
            return false;
        }
    } else {
        *handler = SDL_BLEGATT_AddValueHandler(characteristic, callback, ctrl, index, token);
        if (!*handler) {
            return false;
        }
    }
    return SDL_BLEGATT_EnableNotifications(characteristic, &ctrl->run->cancel);
}

// The Switch 2 service's characteristics of the unified path, the
// vibration output the type's own
static void BLE_FindUnifiedCharacteristics(Uint16 product_id, GattService3 *service3, SDL_AtomicInt *cancel, GattChar **input,
                                           GattChar **command, GattChar **response, GattChar **vibration)
{
    const GUID *vibration_uuid;

    switch (product_id) {
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT:
        vibration_uuid = &GUID_Switch2VibeJCL;
        break;
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT:
        vibration_uuid = &GUID_Switch2VibeJCR;
        break;
    case USB_PRODUCT_NINTENDO_SWITCH2_PRO:
        vibration_uuid = &GUID_Switch2VibePro;
        break;
    default:
        vibration_uuid = &GUID_Switch2VibeGC;
        break;
    }
    *input = SDL_BLEGATT_FindCharacteristic(service3, &GUID_Switch2Input, cancel);
    *command = SDL_BLEGATT_FindCharacteristic(service3, &GUID_Switch2Command, cancel);
    *response = SDL_BLEGATT_FindCharacteristic(service3, &GUID_Switch2CmdResponse, cancel);
    *vibration = SDL_BLEGATT_FindCharacteristic(service3, vibration_uuid, cancel);
}

// Rumble goes over the vibration output, or the command channel for the
// GameCube (BLE_PumpGameCubeRumble)
static bool BLE_CanRumble(const BLE_Controller *ctrl)
{
    return (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER) ? (ctrl->command_char != NULL)
                                                                                 : (ctrl->vibration_char != NULL);
}

// The side's console channel of a Joy-Con 2 (hifihedgehog/SDL#38)
static void BLE_FindConsoleCharacteristics(Uint16 product_id, GattService3 *service3, SDL_AtomicInt *cancel, GattChar **command,
                                           GattChar **input, GattChar **extended_response)
{
    const bool left = (product_id == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT);

    *command = SDL_BLEGATT_FindCharacteristic(service3, left ? &GUID_Switch2ConsoleCommandJCL : &GUID_Switch2ConsoleCommandJCR, cancel);
    *input = SDL_BLEGATT_FindCharacteristic(service3, left ? &GUID_Switch2NativeInputJCL : &GUID_Switch2NativeInputJCR, cancel);
    *extended_response = SDL_BLEGATT_FindCharacteristic(service3, left ? &GUID_Switch2ExtendedResponseJCL : &GUID_Switch2ExtendedResponseJCR,
                                                        cancel);
}

// The console path of a Joy-Con 2 (hifihedgehog/SDL#38). Third-party
// Joy-Con 2s such as the NYXI Hyperion 3 and Hyperion 3 Ultra copy a Joy-Con 2's
// identity and GATT table and never notify on the unified input. They stream
// only on the channel a Switch 2 console opens. This opens that channel as
// joycon2android ConsoleSession does, tested on a Hyperion 3: the session
// start, the side's characteristics, the console's command sequence, the
// report-rate descriptor, then the side's input. The sequence is the
// console's own (switch2_controller_research bluetooth_interface.md
// "JoyCon 2") without the pairing commands 0x15 and the two 0x03 commands
// after them, which carry the host's address, as joycon2android sends it.
// Pairing would store this host on the controller and unpair it from its
// owner's console. The stick calibration comes from the console's reads:
// the factory block at +0x28 in the 0x13080 read, where the wired driver
// parses it (SDL_hidapi_switch2.c:581-586), and the user block at 0x1FC040.
// Those two reads wait for their replies. Every other command goes out
// without waiting, BLE_CONSOLE_COMMAND_GAP_MS apart, in the same order
// (hifihedgehog/SDL#49). The side's characteristics are found by the
// caller. A recovery runs the session again on the characteristics it found
// anew, and keeps the calibration it had when neither read gives one. True
// once the side's input is subscribed.
static bool BLE_StartConsoleSession(BLE_Controller *ctrl, BleDevice3 *device3)
{
    static const Uint8 session_start[] = { 0x01, 0x00 };
    static const Uint8 report_rate[] = { 0x85, 0x00 };
    static const Uint8 vibration_sample[] = { 0x03, 0x00, 0x00, 0x00 };
    static const Uint8 features[] = { 0x37, 0x00, 0x00, 0x00 };
    static const Uint8 vibration_data[] = {
        0x01, 0x59, 0x09, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x35,
        0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    // All four player LEDs while no player is assigned, as joycon2android
    // starts its session (JoyconConnection.kt LED_ALL_ON). Open sets the slot.
    static const Uint8 led[8] = { 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    const bool left = (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT);
    const bool replay = ctrl->console;
    GattService3 *session_service;
    bool session_started = false, extended = false, parsed = false;
    Switch2_AxisCal x, y;
    Uint8 block[0x40];
    int got;

    // Open the session: 01 00 to 00c5af5d-...-bd282 (handle 0x0005), with
    // response as tarabdaar writes it (JoyConBLE.swift beginNYXISession)
    session_service = SDL_BLEGATT_FindService(device3, &GUID_Switch2SessionService, 1, 1, "Switch2 session", &ctrl->run->cancel);
    if (session_service) {
        GattChar *start = SDL_BLEGATT_FindCharacteristic(session_service, &GUID_Switch2SessionStart, &ctrl->run->cancel);
        if (start) {
            session_started = SDL_BLEGATT_WriteCharacteristic(start, session_start, (int)sizeof(session_start), true, &ctrl->run->cancel);
            __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(start);
        }
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3_Release(session_service);
    }

    // The replies arrive on the side's extended response, or on c765a961,
    // which is already subscribed
    extended = BLE_Subscribe(ctrl, ctrl->extended_response_char, &ctrl->extended_response_handler, &ctrl->extended_response_token,
                             BLE_OnResponseValue, 1);

    // From here every command frame goes to the side's command
    // characteristic, and only the side's input reaches the report slot. A
    // restart finds the path set, and Open reads it on the joystick thread.
    if (!replay) {
        SDL_LockMutex(ctrl->report_lock);
        ctrl->console = true;
        ctrl->report_pending = false;
        SDL_UnlockMutex(ctrl->report_lock);
    }
    BLE_LOG(ctrl, "BLE Switch2 console session: start %s, extended response %s", session_started ? "written" : "not written",
            extended ? "subscribed" : "absent");

    BLE_SendConsoleCommand(ctrl, 0x07, 0x01, NULL, 0);
    BLE_RequestConsoleRead(ctrl, 0x40, 0x13000);
    BLE_SendConsoleCommand(ctrl, 0x10, 0x01, NULL, 0);
    BLE_SendConsoleCommand(ctrl, 0x16, 0x01, NULL, 0);
    BLE_SendConsoleCommand(ctrl, 0x0A, 0x02, vibration_sample, (int)sizeof(vibration_sample));
    BLE_SendConsoleCommand(ctrl, 0x09, 0x07, led, (int)sizeof(led));
    BLE_SendConsoleCommand(ctrl, 0x0C, 0x02, features, (int)sizeof(features));
    SDL_zero(x);
    SDL_zero(y);
    got = BLE_ReadMemory(ctrl, 0x40, 0x13080, block, (int)sizeof(block));
    if (got >= 0x28 + 9) {
        parsed = BLE_ParseStickCalibration(&x, &y, &block[0x28]);
    }
    got = BLE_ReadMemory(ctrl, 0x40, 0x1FC040, block, (int)sizeof(block));
    if (BLE_ParseUserCalibration(&x, &y, block, got)) {
        parsed = true;
    }
    if (parsed || !replay) {
        ctrl->left_x = x;
        ctrl->left_y = y;
    }
    BLE_RequestConsoleRead(ctrl, 0x10, 0x13040);
    BLE_RequestConsoleRead(ctrl, 0x18, 0x13100);
    BLE_SendConsoleCommand(ctrl, 0x11, 0x03, NULL, 0);
    BLE_RequestConsoleRead(ctrl, 0x20, 0x13060);
    BLE_SendConsoleCommand(ctrl, 0x0A, 0x08, vibration_data, (int)sizeof(vibration_data));
    BLE_SendConsoleCommand(ctrl, 0x11, 0x01, NULL, 0);
    BLE_SendConsoleCommand(ctrl, 0x0C, 0x04, features, (int)sizeof(features));

    // The report rate, then the side's input (handles 0x0010 and 0x000F),
    // the console's last two writes
    if (!SDL_BLEGATT_WriteDescriptorValue(ctrl->native_input_char, &GUID_Switch2ReportRate, report_rate, (int)sizeof(report_rate),
                                          &ctrl->run->cancel)) {
        BLE_LOG(ctrl, "BLE Switch2 console session: report-rate descriptor not written");
    }
    if (!BLE_Subscribe(ctrl, ctrl->native_input_char, &ctrl->native_input_handler, &ctrl->native_input_token, BLE_OnInputValue,
                       BLE_INPUT_NATIVE)) {
        BLE_LOG(ctrl, "BLE Switch2 console session: the %s input could not be subscribed", left ? "left" : "right");
        return false;
    }
    BLE_LOG(ctrl, "BLE Switch2 console session: %s input subscribed", left ? "left" : "right");
    return true;
}

// ---------------------------------------------------------------------------
// Connect + GATT discovery + subscribe (called from the connect thread). The
// open, the uncached discovery and the lookups are the shared transport's
// (SDL_ble_gatt.c). Every await takes the run's cancel flag, so Quit's stop
// ends the connect within BLE_QUIT_WAIT_MS.
// ---------------------------------------------------------------------------
static void BLE_ConnectAndSubscribe(BLE_Run *run, Uint64 bluetooth_address, Uint16 vendor_id, Uint16 product_id, char *name)
{
    BleDevice *device = NULL;
    BleDevice3 *device3 = NULL;
    GattService3 *service3 = NULL;
    SDL_BLEGATTSession *session = NULL;
    BLE_Controller *ctrl = NULL;
    bool input = false, paired;

    // Open by raw address with the 20 s ceiling (SDL_BLEGATT_OpenDevice). This
    // runs on the connect thread, so the wait does not starve the WinRT thread
    // pool that has to deliver the completion.
    device = SDL_BLEGATT_OpenDevice(bluetooth_address, &run->cancel);
    BLE_RUN_LOG(run, "BLE Switch2 open addr %012llx: device=%p", (unsigned long long)bluetooth_address, (void *)device);
    if (!device) {
        return;
    }

    // A GattSession with MaintainConnection, held for the controller's life,
    // so Windows connects the link again by itself after it drops, as it does
    // for FlexInput (commit d963276d6, CHANGELOG 0.13.5). It does not stop a
    // Joy-Con 2 clone's drop (hifihedgehog/SDL#49). The pairing state tells a
    // pairing started in Windows settings, which the controller ends by
    // hanging up (FlexInput crates/joycon2/src/win_pair.rs:5-13), from the
    // timeout of the pairing a clone asks for.
    session = SDL_BLEGATT_OpenSession(device, &run->cancel);
    // Read before the log line checks whether Quit has left this thread
    // running, since the read takes no cancel flag
    paired = SDL_BLEGATT_IsPaired(device);
    BLE_RUN_LOG(run, "BLE Switch2 Windows pairing: %s", paired ? "paired" : "not paired");

    // Discover the Switch 2 service uncached, up to 10 tries 500 ms apart while
    // the bond-free link comes up (SDL_BLEGATT_FindService).
    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_QueryInterface(device, &IID_BleDevice3, (void **)&device3)) || !device3) {
        goto cleanup;
    }
    service3 = SDL_BLEGATT_FindService(device3, &GUID_Switch2Service, 1, 10, "Switch2", &run->cancel);
    if (!service3) {
        BLE_RUN_LOG(run, "BLE Switch2 GATT service discovery failed after 10 attempts");
        BLE_NoteConnectFailure(run, bluetooth_address); // back off; likely busy on USB
        goto cleanup;
    }

    // Best-effort throughput bump, now that the link is up
    // (SDL_BLEGATT_RequestThroughput).
    SDL_BLEGATT_RequestThroughput(device);

    // Build the controller record.
    ctrl = (BLE_Controller *)SDL_calloc(1, sizeof(*ctrl));
    if (!ctrl) {
        goto cleanup;
    }
    ctrl->run = run;
    ctrl->report_lock = SDL_CreateMutex();
    ctrl->bluetooth_address = bluetooth_address;
    ctrl->vendor_id = vendor_id;
    ctrl->product_id = product_id;
    if (name) {
        ctrl->name = name;
    } else {
        const char *type_name;
        switch (product_id) {
        case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT:
            type_name = "Nintendo Switch 2 Joy-Con (L)";
            break;
        case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT:
            type_name = "Nintendo Switch 2 Joy-Con (R)";
            break;
        case USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER:
            type_name = "Nintendo Switch 2 GameCube Controller";
            break;
        default:
            type_name = "Nintendo Switch 2 Pro Controller";
            break;
        }
        ctrl->name = SDL_strdup(type_name);
    }
    // Ownership of the device and the session goes to ctrl, and the cleanup
    // releases both through it
    ctrl->device = device;
    device = NULL;
    ctrl->session = session;
    session = NULL;
    ctrl->instance_id = SDL_GetNextObjectID();
    ctrl->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, vendor_id, product_id, 0, NULL, ctrl->name, 'h', 0);

    ctrl->response_lock = SDL_CreateMutex();
    ctrl->response_sem = SDL_CreateSemaphore(0);
    ctrl->first_report_sem = SDL_CreateSemaphore(0);
    ctrl->player_index = -1;

    // Link-loss detection (SDL#9 follow-up): without it a pad that powers off
    // (the SendEffect shutdown, a dead battery, out of range) stays registered
    // forever, and its address stays claimed in ble.controllers so every
    // reconnection advertisement is dropped by BLE_TryReserveConnect. The
    // callback only sets link_lost, and BLE_JoystickDetectDevices runs the
    // recovery. It is registered as soon as the discovery has the link up, so
    // a drop during the rest of the setup is recorded too: while the session
    // holds MaintainConnection, Windows can connect the link again before the
    // setup ends, with every characteristic found before the drop closed, and
    // the recovery after the publish finds them anew (hifihedgehog/SDL#49).
    // If the append below is skipped (duplicate/teardown race),
    // BLE_FreeController removes this subscription on the cleanup path like
    // the others. SDL_BLEGATT_AddStatusHandler polls the status once after
    // registering.
    ctrl->status_handler = SDL_BLEGATT_AddStatusHandler(ctrl->device, &ctrl->link_lost, &ctrl->status_token);

    BLE_FindUnifiedCharacteristics(product_id, service3, &run->cancel, &ctrl->input_char, &ctrl->command_char, &ctrl->response_char,
                                   &ctrl->vibration_char);
    // Open reads this while a recovery runs, and only the joystick thread
    // changes it, once a recovery has found a rumble characteristic the
    // connect could not (BLE_FinishRestore)
    ctrl->rumble_capable = BLE_CanRumble(ctrl);

    BLE_LOG(ctrl, "BLE Switch2 connected: chars input=%d command=%d response=%d vibration=%d", ctrl->input_char != NULL,
            ctrl->command_char != NULL, ctrl->response_char != NULL, ctrl->vibration_char != NULL);

    // Connect sequence per controller.py connect() (switch2-controllers/
    // controller.py:253-265): enable the command-response notification first, then
    // read calibration over the command channel, and only THEN enable the input
    // report notification. Handler before notifications (controller.py order).
    (void)BLE_Subscribe(ctrl, ctrl->response_char, &ctrl->response_handler, &ctrl->response_token, BLE_OnResponseValue, 0);

    // Read stick calibration over the command channel (best-effort).
    BLE_ReadCalibration(ctrl);

    input = BLE_Subscribe(ctrl, ctrl->input_char, &ctrl->input_handler, &ctrl->input_token, BLE_OnInputValue, BLE_INPUT_UNIFIED);

    // A Joy-Con 2 that reports on the unified input takes the unified path.
    // One that stays silent, as the Joy-Con 2 clones do, takes the console
    // path, and one that has neither is not added (hifihedgehog/SDL#38). A
    // genuine Joy-Con 2 answers the calibration reads on 649d4ac9 (handle
    // 0x0014). The Mobapad M12-S answers no command there (FlexInput
    // docs/BLUETOOTH_TRANSPORTS.md:132-138), and the NYXI Hyperion 3 ignores
    // that characteristic (joycon2android docs/protocol.md "Console-protocol
    // controllers"), so a Joy-Con 2 that answered neither read starts the
    // console session without the wait for the unified input
    // (hifihedgehog/SDL#49). A clone that answers them, such as the
    // Hyperion 3 Ultra, still waits.
    if (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT ||
        ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT) {
        if (BLE_AwaitUnifiedReport(ctrl, ctrl->unified_answered ? BLE_CONSOLE_FALLBACK_MS : 0)) {
            BLE_StartUnifiedJoyCon(ctrl, true);
        } else {
            if (BLE_Canceled(ctrl)) {
                goto cleanup;
            }
            if (ctrl->unified_answered) {
                BLE_LOG(ctrl, "BLE Switch2 no report on the unified input within %d ms: starting the console session", BLE_CONSOLE_FALLBACK_MS);
            } else {
                BLE_LOG(ctrl, "BLE Switch2 no reply to the calibration reads on 649d4ac9: starting the console session");
            }
            BLE_FindConsoleCharacteristics(ctrl->product_id, service3, &run->cancel, &ctrl->console_command_char, &ctrl->native_input_char,
                                           &ctrl->extended_response_char);
            if (!ctrl->console_command_char || !ctrl->native_input_char) {
                BLE_LOG(ctrl, "BLE Switch2 console channel missing: command=%d input=%d", ctrl->console_command_char != NULL,
                        ctrl->native_input_char != NULL);
                BLE_NoteConnectFailure(run, bluetooth_address);
                goto cleanup;
            }
            if (!BLE_StartConsoleSession(ctrl, device3)) {
                BLE_NoteConnectFailure(run, bluetooth_address);
                goto cleanup;
            }
        }
    } else if (!input && !SDL_GetAtomicInt(&ctrl->link_lost)) {
        // A Pro or GameCube controller whose input cannot be subscribed on a
        // link that held would stay silent. One whose link dropped during
        // the setup is published, and its recovery subscribes the input anew.
        BLE_LOG(ctrl, "BLE Switch2 input could not be subscribed");
        BLE_NoteConnectFailure(run, bluetooth_address);
        goto cleanup;
    }

    // The joystick lock, unless Quit has told the run to stop meanwhile
    // (BLE_LockForThread). ctrl then stays non-NULL and is torn down via the
    // cleanup path below.
    if (BLE_LockForThread(run)) {
        // Bail if a teardown ran while this connect was in WinRT discovery.
        // SDL_QuitJoysticks holds SDL_LockJoysticks from setting
        // SDL_joysticks_quitting through every driver->Quit(), and
        // BLE_JoystickQuit clears ble.initialized last under that same lock.
        // A connect thread that took the lock after the quit must not
        // re-grow the freed ble.controllers array or publish a joystick.
        // ctrl stays non-NULL and is torn down via the cleanup path below.
        BLE_Controller **grown = ble.initialized
            ? (BLE_Controller **)SDL_realloc(ble.controllers, sizeof(ble.controllers[0]) * (ble.controller_count + 1))
            : NULL;
        // Re-check under the lock: another advertisement callback on a second
        // thread-pool thread may have connected the same device concurrently.
        if (grown) {
            ble.controllers = grown;
            if (!BLE_GetControllerByAddress(bluetooth_address)) {
                int c;
                ctrl->connected_ms = SDL_GetTicks();
                ble.controllers[ble.controller_count++] = ctrl;
                if (BLE_PublishController(ctrl)) {
                    ctrl = NULL; // owned by the array now
                    // Genuine connect: clear any backoff so a later wireless reconnect
                    // is immediate. (Already under SDL_LockJoysticks here.)
                    for (c = 0; c < ble.cooldown_count; ++c) {
                        if (ble.cooldowns[c].address == bluetooth_address) {
                            ble.cooldowns[c] = ble.cooldowns[--ble.cooldown_count];
                            break;
                        }
                    }
                } else {
                    // No joystick, so the cleanup path below tears the controller down
                    --ble.controller_count;
                }
            }
        }
        SDL_UnlockJoysticks();
    }

cleanup:
    if (service3) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3_Release(service3);
    }
    if (device3) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3_Release(device3);
    }
    if (ctrl) {
        // Failed to publish (a failed console start or a concurrent duplicate
        // connect). Full teardown: unregister the ValueChanged handlers,
        // release the characteristics, close the session and release the
        // device, which ctrl holds by now.
        BLE_FreeController(ctrl);
    } else {
        // Failed before ctrl took the session and the device
        if (session) {
            SDL_BLEGATT_CloseSession(session);
        }
        if (device) {
            __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(device);
        }
    }
}

// ---------------------------------------------------------------------------
// Input decode. The BLE report is the wired report minus the 1-byte report-ID
// prefix, so the wired HandleSwitchProState layout applies at offset -1. Button
// bytes: u32 LE at [4:8]; sticks at [10:16]; IMU accel [48:54] / gyro [54:60].
// ---------------------------------------------------------------------------
// The unified report's motion block, the same for every controller type
// (hid_reports.md report 0x05, Motion Data at 0x2A: accelerometer at 0x30,
// gyroscope at 0x36), transcribed from the wired HandleStatePacket
// (SDL_hidapi_switch2.c:1321-1327) at BLE offset -1. The axes are not in
// order: accel maps X<-0x30, Y<-0x34, Z<-0x32(neg), and the gyro X<-0x36,
// Y<-0x3A, Z<-0x38(neg). The wired driver picks a gyro_coeff of 34.8 or 40
// from the rate of its first 100 sensor timestamps (:1278-1301). That choice
// is not ported, and 34.8, the nominal case, is used. Biases are 0. The
// caller checks size >= 60.
static void BLE_ReadMotion(const Uint8 *data, float accel[3], float gyro[3])
{
    const float accel_scale = SDL_STANDARD_GRAVITY * 8.0f / 32767.0f;
    const float gyro_scale = 34.8f / 32767.0f;

    accel[0] = (Sint16)(data[48] | (data[49] << 8)) * accel_scale;
    accel[1] = (Sint16)(data[52] | (data[53] << 8)) * accel_scale;
    accel[2] = (Sint16)(data[50] | (data[51] << 8)) * -accel_scale;
    gyro[0] = (Sint16)(data[54] | (data[55] << 8)) * gyro_scale;
    gyro[1] = (Sint16)(data[58] | (data[59] << 8)) * gyro_scale;
    gyro[2] = (Sint16)(data[56] | (data[57] << 8)) * -gyro_scale;
}

// A Joy-Con held sideways, the wired driver's rotation for a single Joy-Con
// (SDL_hidapi_switch2.c:1335-1341 left, :1354-1360 right). A Joy-Con held
// upright keeps the axes of a Joy-Con in a pair, as the Switch 1 driver
// rotates only its mini-gamepad mode (SDL_hidapi_switch.c SendSensorUpdate,
// the !m_bVerticalMode tests).
static void BLE_RotateSideways(bool left, float v[3])
{
    float tmp;

    if (left) {
        tmp = -v[0];
        v[0] = v[2];
        v[2] = tmp;
    } else {
        tmp = v[0];
        v[0] = -v[2];
        v[2] = tmp;
    }
}

// Joy-Con 2 motion on the unified path (hifihedgehog/SDL#38): the Pro
// decoder's offsets and scales, with the single Joy-Con's rotation
static void BLE_PostJoyConMotion(BLE_Controller *ctrl, SDL_Joystick *joystick, const Uint8 *data, int size, Uint64 ts, bool left)
{
    float accel[3], gyro[3];

    if (size < 60 || !ctrl->joystick || !ctrl->sensors_enabled) {
        return;
    }
    BLE_ReadMotion(data, accel, gyro);
    if (!ctrl->vertical_mode) {
        BLE_RotateSideways(left, accel);
        BLE_RotateSideways(left, gyro);
    }
    SDL_SendJoystickSensor(ts, joystick, SDL_SENSOR_GYRO, ts, gyro, 3);
    SDL_SendJoystickSensor(ts, joystick, SDL_SENSOR_ACCEL, ts, accel, 3);
}

static void BLE_DecodeProReport(BLE_Controller *ctrl, SDL_Joystick *joystick, Uint8 *data, int size)
{
    Uint64 timestamp = SDL_GetTicksNS();
    Sint16 axis;

    if (size < 16) {
        return;
    }

    // data[4..7] are the four button bytes (== wired data[5..8]).
    if (!ctrl->have_last_state || data[4] != ctrl->last_state[4]) {
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[4] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[4] & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[4] & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[4] & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data[4] & 0x40) != 0));
    }
    if (!ctrl->have_last_state || data[5] != ctrl->last_state[5]) {
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data[5] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data[5] & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, ((data[5] & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data[5] & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[5] & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, 11 /* Share */, ((data[5] & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, 12 /* C */, ((data[5] & 0x40) != 0));
    }
    if (!ctrl->have_last_state || data[6] != ctrl->last_state[6]) {
        Uint8 hat = 0;
        if (data[6] & 0x01) {
            hat |= SDL_HAT_DOWN;
        }
        if (data[6] & 0x02) {
            hat |= SDL_HAT_UP;
        }
        if (data[6] & 0x04) {
            hat |= SDL_HAT_RIGHT;
        }
        if (data[6] & 0x08) {
            hat |= SDL_HAT_LEFT;
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[6] & 0x40) != 0));
    }
    if (!ctrl->have_last_state || data[7] != ctrl->last_state[7]) {
        SDL_SendJoystickButton(timestamp, joystick, 13 /* right paddle */, ((data[7] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, 14 /* left paddle */, ((data[7] & 0x02) != 0));
    }

    axis = (data[4] & 0x80) ? 32767 : -32768; // ZR
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis);
    axis = (data[6] & 0x80) ? 32767 : -32768; // ZL
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis);

    // Sticks: 12-bit packed at [10:13] (left) and [13:16] (right).
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX,
                         BLE_MapStickAxis(&ctrl->left_x, (float)(data[10] | ((data[11] & 0x0F) << 8)), false));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY,
                         BLE_MapStickAxis(&ctrl->left_y, (float)((data[11] >> 4) | (data[12] << 4)), true));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX,
                         BLE_MapStickAxis(&ctrl->right_x, (float)(data[13] | ((data[14] & 0x0F) << 8)), false));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY,
                         BLE_MapStickAxis(&ctrl->right_y, (float)((data[14] >> 4) | (data[15] << 4)), true));

    // IMU (BLE_ReadMotion)
    if (size >= 60 && ctrl->joystick && ctrl->sensors_enabled) {
        float accel[3], gyro[3];
        BLE_ReadMotion(data, accel, gyro);
        SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_ACCEL, timestamp, accel, 3);
        SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_GYRO, timestamp, gyro, 3);
    }

    SDL_memcpy(ctrl->last_state, data, SDL_min(size, (int)sizeof(ctrl->last_state)));
    ctrl->have_last_state = true;
}

// GameCube button indices (the wired SDL_GAMEPAD_BUTTON_SWITCH2_GAMECUBE_* enums
// are file-static there). Joy-Con extras: SHARE=11, C=12, paddles 13..16.
enum { GC_GUIDE = 4, GC_START, GC_LSHOULDER, GC_RSHOULDER, GC_SHARE, GC_C, GC_LTRIGGER, GC_RTRIGGER };

static Sint16 BLE_RemapTrigger(Uint8 value)
{
    float t = SDL_clamp((float)value / 232.0f, 0.0f, 1.0f);
    return (Sint16)(SDL_MIN_SINT16 + t * ((float)SDL_MAX_SINT16 - (float)SDL_MIN_SINT16));
}

// All BLE decoders run at offset -1 vs the wired report (BLE omits the report-ID
// prefix). SDL_SendJoystick* filters unchanged values, so we emit every report.
static void BLE_DecodeGameCube(BLE_Controller *ctrl, SDL_Joystick *joystick, Uint8 *data, int size)
{
    Uint64 ts = SDL_GetTicksNS();
    Uint8 hat = 0;
    if (size < 62) {
        return;
    }
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[4] & 0x01) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[4] & 0x02) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[4] & 0x04) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[4] & 0x08) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_RTRIGGER, ((data[4] & 0x40) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_RSHOULDER, ((data[4] & 0x80) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_START, ((data[5] & 0x02) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_GUIDE, ((data[5] & 0x10) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_SHARE, ((data[5] & 0x20) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_C, ((data[5] & 0x40) != 0));
    if (data[6] & 0x01) { hat |= SDL_HAT_DOWN; }
    if (data[6] & 0x02) { hat |= SDL_HAT_UP; }
    if (data[6] & 0x04) { hat |= SDL_HAT_RIGHT; }
    if (data[6] & 0x08) { hat |= SDL_HAT_LEFT; }
    SDL_SendJoystickHat(ts, joystick, 0, hat);
    SDL_SendJoystickButton(ts, joystick, GC_LTRIGGER, ((data[6] & 0x40) != 0));
    SDL_SendJoystickButton(ts, joystick, GC_LSHOULDER, ((data[6] & 0x80) != 0));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, BLE_RemapTrigger(data[60]));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, BLE_RemapTrigger(data[61]));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTX, BLE_MapStickAxis(&ctrl->left_x, (float)(data[10] | ((data[11] & 0x0F) << 8)), false));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTY, BLE_MapStickAxis(&ctrl->left_y, (float)((data[11] >> 4) | (data[12] << 4)), true));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_RIGHTX, BLE_MapStickAxis(&ctrl->right_x, (float)(data[13] | ((data[14] & 0x0F) << 8)), false));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_RIGHTY, BLE_MapStickAxis(&ctrl->right_y, (float)((data[14] >> 4) | (data[15] << 4)), true));
}

// Standalone (mini) Joy-Con 2 Left, held sideways.
/* Joy-Con 2 optical mouse: the report 0x05 Mouse Data block at offset 0x10 is
   Position X (u16 LE) then Position Y (u16 LE), absolute accumulating counters
   (hid_reports.md "Mouse Data (absolute)"; joycon2cpp GetRawOpticalMouse reads
   signed 16 at buffer[0x10]/[0x12] with the same size >= 0x18 guard,
   JoyConDecoder.cpp:741-747; jc2mouse reads the identical bytes at window
   0x0F+1, driver.py:71-74). Posted raw and bit-preserved as Sint16 on the two
   dedicated axes; the consumer derives wraparound deltas (the jc2mouse
   delta_u16 idiom, driver.py:174-176), the same raw-in/derive-downstream
   contract as the Wii IR dots. The +0x14/+0x16 unknown fields stay unsurfaced
   until their semantics are pinned. */
static void BLE_PostMouseAxes(BLE_Controller *ctrl, SDL_Joystick *joystick, const Uint8 *data, int size, Uint64 ts)
{
    Sint16 counter_x, counter_y;

    if (!ctrl->mouse_enabled || size < 0x18) {
        return;
    }
    counter_x = (Sint16)(data[0x10] | (data[0x11] << 8));
    counter_y = (Sint16)(data[0x12] | (data[0x13] << 8));

    /* Data axes: seed past the analog anti-jitter gate so the first ~409
       counts of motion per connection are not eaten (hifihedgehog/SDL#14) */
    SDL_SeedJoystickDataAxis(joystick, SDL_GAMEPAD_AXIS_COUNT + 0, counter_x);
    SDL_SeedJoystickDataAxis(joystick, SDL_GAMEPAD_AXIS_COUNT + 1, counter_y);

    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_COUNT + 0, counter_x);
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_COUNT + 1, counter_y);
}

/* Switch 2 magnetometer: three int16 LE at report offsets 0x19/0x1B/0x1D
   (hid_reports.md report 0x05 Magnetometer Data at 0x19, and
   switch2-controllers controller.py:136 reads data[25:31]), streamed when
   feature bit 7 is enabled (windows10-gyro controller.py:713's FEATSEL
   0x94). Two OEM Joy-Con 2 (R) reports in the capture of PadForge discussion
   #491 read (102, 299, -26) and (100, 301, -24) there (hifihedgehog/SDL#38).
   Raw samples on three dedicated axes, no fusion: the consumer owns
   orientation math. The axes follow the mouse counters when both are
   enabled, and the raw axis count is the availability contract
   (6 = neither, 8 = mouse, 9 = magnetometer, 11 = both). */
static void BLE_PostMagnetometerAxes(BLE_Controller *ctrl, SDL_Joystick *joystick, const Uint8 *data, int size, Uint64 ts)
{
    int base;
    Sint16 mag[3];
    int i;

    if (!ctrl->magnetometer_enabled || size < 0x1F) {
        return;
    }
    base = SDL_GAMEPAD_AXIS_COUNT + (ctrl->mouse_enabled ? 2 : 0);
    mag[0] = (Sint16)(data[0x19] | (data[0x1A] << 8));
    mag[1] = (Sint16)(data[0x1B] | (data[0x1C] << 8));
    mag[2] = (Sint16)(data[0x1D] | (data[0x1E] << 8));
    for (i = 0; i < 3; ++i) {
        /* Data axes: seed past the analog anti-jitter gate (hifihedgehog/SDL#14) */
        SDL_SeedJoystickDataAxis(joystick, base + i, mag[i]);
        SDL_SendJoystickAxis(ts, joystick, base + i, mag[i]);
    }
}

static void BLE_DecodeJoyConLeft(BLE_Controller *ctrl, SDL_Joystick *joystick, Uint8 *data, int size)
{
    Uint64 ts = SDL_GetTicksNS();
    if (size < 14) {
        return;
    }
    if (ctrl->vertical_mode) {
        /* Upright orientation: mirror the wired driver's
           HandleCombinedControllerStateL (SDL_hidapi_switch2.c). The BLE
           report is the wired report shifted down one byte, the same
           correspondence the mini decode below already uses. */
        Uint8 hat = 0;
        Sint16 axis;

        SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data[5] & 0x01) != 0));
        SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data[5] & 0x08) != 0));
        SDL_SendJoystickButton(ts, joystick, 11 /* JoyCon Share */, ((data[5] & 0x20) != 0));

        if (data[6] & 0x01) {
            hat |= SDL_HAT_DOWN;
        }
        if (data[6] & 0x02) {
            hat |= SDL_HAT_UP;
        }
        if (data[6] & 0x04) {
            hat |= SDL_HAT_RIGHT;
        }
        if (data[6] & 0x08) {
            hat |= SDL_HAT_LEFT;
        }
        SDL_SendJoystickHat(ts, joystick, 0, hat);
        SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[6] & 0x40) != 0));
        SDL_SendJoystickButton(ts, joystick, 14 /* JoyCon left paddle 1 */, ((data[7] & 0x02) != 0));

        axis = (data[6] & 0x80) ? 32767 : -32768;
        SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis);

        SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTX, BLE_MapStickAxis(&ctrl->left_x, (float)(data[10] | ((data[11] & 0x0F) << 8)), false));
        SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTY, BLE_MapStickAxis(&ctrl->left_y, (float)((data[11] >> 4) | (data[12] << 4)), true));
        BLE_PostMouseAxes(ctrl, joystick, data, size, ts);
        BLE_PostMagnetometerAxes(ctrl, joystick, data, size, ts);
        BLE_PostJoyConMotion(ctrl, joystick, data, size, ts, true);
        return;
    }
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_START, ((data[5] & 0x01) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data[5] & 0x08) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[5] & 0x20) != 0));
    SDL_SendJoystickButton(ts, joystick, 11 /* JoyCon Share */, ((data[5] & 0x10) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[6] & 0x01) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[6] & 0x02) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[6] & 0x04) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[6] & 0x08) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data[6] & 0x10) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[6] & 0x20) != 0));
    SDL_SendJoystickButton(ts, joystick, 14 /* JoyCon left paddle 1 */, ((data[6] & 0x40) != 0));
    SDL_SendJoystickButton(ts, joystick, 16 /* JoyCon left paddle 2 */, ((data[6] & 0x80) != 0));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTX, BLE_MapStickAxis(&ctrl->left_y, (float)((data[11] >> 4) | (data[12] << 4)), true));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTY, BLE_MapStickAxis(&ctrl->left_x, (float)(data[10] | ((data[11] & 0x0F) << 8)), true));
    BLE_PostMouseAxes(ctrl, joystick, data, size, ts);
    BLE_PostMagnetometerAxes(ctrl, joystick, data, size, ts);
    BLE_PostJoyConMotion(ctrl, joystick, data, size, ts, true);
}

// Standalone (mini) Joy-Con 2 Right, held sideways.
static void BLE_DecodeJoyConRight(BLE_Controller *ctrl, SDL_Joystick *joystick, Uint8 *data, int size)
{
    Uint64 ts = SDL_GetTicksNS();
    if (size < 16) {
        return;
    }
    if (ctrl->vertical_mode) {
        /* Upright orientation: mirror the wired driver's
           HandleCombinedControllerStateR (SDL_hidapi_switch2.c), report
           shifted down one byte as in the mini decode below. */
        Sint16 axis;

        SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[4] & 0x01) != 0));
        SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[4] & 0x02) != 0));
        SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[4] & 0x04) != 0));
        SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[4] & 0x08) != 0));
        SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data[4] & 0x40) != 0));
        SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_START, ((data[5] & 0x02) != 0));
        SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, ((data[5] & 0x04) != 0));
        SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[5] & 0x10) != 0));
        SDL_SendJoystickButton(ts, joystick, 12 /* JoyCon C */, ((data[5] & 0x40) != 0));
        SDL_SendJoystickButton(ts, joystick, 13 /* JoyCon right paddle 1 */, ((data[7] & 0x01) != 0));

        axis = (data[4] & 0x80) ? 32767 : -32768;
        SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis);

        SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_RIGHTX, BLE_MapStickAxis(&ctrl->left_x, (float)(data[13] | ((data[14] & 0x0F) << 8)), false));
        SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_RIGHTY, BLE_MapStickAxis(&ctrl->left_y, (float)((data[14] >> 4) | (data[15] << 4)), true));
        BLE_PostMouseAxes(ctrl, joystick, data, size, ts);
        BLE_PostMagnetometerAxes(ctrl, joystick, data, size, ts);
        BLE_PostJoyConMotion(ctrl, joystick, data, size, ts, false);
        return;
    }
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[4] & 0x01) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[4] & 0x02) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[4] & 0x04) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[4] & 0x08) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data[4] & 0x10) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[4] & 0x20) != 0));
    SDL_SendJoystickButton(ts, joystick, 13 /* JoyCon right paddle 1 */, ((data[4] & 0x40) != 0));
    SDL_SendJoystickButton(ts, joystick, 15 /* JoyCon right paddle 2 */, ((data[4] & 0x80) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_START, ((data[5] & 0x02) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data[5] & 0x04) != 0));
    SDL_SendJoystickButton(ts, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[5] & 0x10) != 0));
    SDL_SendJoystickButton(ts, joystick, 12 /* JoyCon C */, ((data[5] & 0x40) != 0));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTX, BLE_MapStickAxis(&ctrl->left_y, (float)((data[14] >> 4) | (data[15] << 4)), false));
    SDL_SendJoystickAxis(ts, joystick, SDL_GAMEPAD_AXIS_LEFTY, BLE_MapStickAxis(&ctrl->left_x, (float)(data[13] | ((data[14] & 0x0F) << 8)), false));
    BLE_PostMouseAxes(ctrl, joystick, data, size, ts);
    BLE_PostMagnetometerAxes(ctrl, joystick, data, size, ts);
    BLE_PostJoyConMotion(ctrl, joystick, data, size, ts, false);
}

// The console path's report, 0x07 from a left Joy-Con 2 and 0x08 from a right
// one (hid_reports.md, joycon2android ConsolePacketParser.kt, Linux v13
// NS2_REPORT_JCL and NS2_REPORT_JCR). Its buttons, u16 LE at 2, and its stick,
// packed 12-bit at 5, are put where the unified report holds them, so
// BLE_DecodeJoyConLeft/Right post them in both layouts with the axis swaps
// they apply to a unified report, as ConsolePacketParser translates them
// into its common bitmask. The motion block follows its length byte, which
// is 0x0E on the left and 0x0F on the right. The accelerometer is an int16
// in the high half of each of three 4-byte words from the block's start
// plus 0x12, low halves zero, 4096 = 1 g, and y running opposite a genuine
// Joy-Con 2 (joycon2android protocol.md "Motion block", measured on a NYXI
// Hyperion 3 against gravity). No gyroscope reaches it.
static void BLE_DecodeConsoleReport(BLE_Controller *ctrl, SDL_Joystick *joystick, const Uint8 *data, int size)
{
    typedef struct
    {
        Uint8 bit;  // in the console report's u16
        Uint8 byte; // of the unified report
        Uint8 mask;
    } BLE_ConsoleButton;
    static const BLE_ConsoleButton right_buttons[] = {
        { 0, 4, 0x04 },  // B
        { 1, 4, 0x08 },  // A
        { 2, 4, 0x01 },  // Y
        { 3, 4, 0x02 },  // X
        { 4, 4, 0x40 },  // R
        { 5, 4, 0x80 },  // ZR
        { 6, 5, 0x02 },  // Plus
        { 7, 5, 0x04 },  // stick
        { 8, 5, 0x10 },  // Home
        { 12, 5, 0x40 }, // C
        { 14, 4, 0x10 }, // SR
        { 15, 4, 0x20 }  // SL
    };
    static const BLE_ConsoleButton left_buttons[] = {
        { 0, 6, 0x01 },  // Down
        { 1, 6, 0x04 },  // Right
        { 2, 6, 0x08 },  // Left
        { 3, 6, 0x02 },  // Up
        { 4, 6, 0x40 },  // L
        { 5, 6, 0x80 },  // ZL
        { 6, 5, 0x01 },  // Minus
        { 7, 5, 0x08 },  // stick
        { 8, 5, 0x20 },  // Capture
        { 14, 6, 0x10 }, // SR
        { 15, 6, 0x20 }  // SL
    };
    const bool left = (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT);
    const BLE_ConsoleButton *table = left ? left_buttons : right_buttons;
    const int count = left ? (int)SDL_arraysize(left_buttons) : (int)SDL_arraysize(right_buttons);
    Uint8 unified[16];
    int buttons, i;

    if (size < 8) {
        return;
    }
    SDL_zeroa(unified);
    buttons = data[2] | (data[3] << 8);
    for (i = 0; i < count; ++i) {
        if (buttons & (1 << table[i].bit)) {
            unified[table[i].byte] |= table[i].mask;
        }
    }
    SDL_memcpy(&unified[left ? 10 : 13], &data[5], 3);
    if (left) {
        BLE_DecodeJoyConLeft(ctrl, joystick, unified, (int)sizeof(unified));
    } else {
        BLE_DecodeJoyConRight(ctrl, joystick, unified, (int)sizeof(unified));
    }

    if (ctrl->joystick && ctrl->sensors_enabled) {
        const int block = left ? 0x0F : 0x10;
        const int first = block + 0x12;

        if (size >= first + 10 && data[block - 1] >= 0x1C &&
            (data[first - 2] | data[first - 1] | data[first + 2] | data[first + 3] | data[first + 6] | data[first + 7]) == 0) {
            const float scale = SDL_STANDARD_GRAVITY * 8.0f / 32767.0f;
            const Uint64 ts = SDL_GetTicksNS();
            // y negated into a genuine Joy-Con 2's frame, then the order
            // BLE_ReadMotion gives the unified report's X, Y and Z
            const int x = (Sint16)(data[first] | (data[first + 1] << 8));
            const int y = -(Sint16)(data[first + 4] | (data[first + 5] << 8));
            const int z = (Sint16)(data[first + 8] | (data[first + 9] << 8));
            float accel[3];

            accel[0] = (float)x * scale;
            accel[1] = (float)z * scale;
            accel[2] = (float)-y * scale;
            if (!ctrl->vertical_mode) {
                BLE_RotateSideways(left, accel);
            }
            SDL_SendJoystickSensor(ts, joystick, SDL_SENSOR_ACCEL, ts, accel, 3);
        }
    }
}

static void BLE_DecodeReport(BLE_Controller *ctrl, SDL_Joystick *joystick, Uint8 *data, int size)
{
    switch (ctrl->product_id) {
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT:
        BLE_DecodeJoyConLeft(ctrl, joystick, data, size);
        break;
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT:
        BLE_DecodeJoyConRight(ctrl, joystick, data, size);
        break;
    default: // Pro Controller and GameCube share the full layout
        if (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_PRO) {
            BLE_DecodeProReport(ctrl, joystick, data, size);
        } else {
            BLE_DecodeGameCube(ctrl, joystick, data, size);
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// The recovery in place of a lost link (hifihedgehog/SDL#49). A Joy-Con 2
// clone such as the Mobapad M12-S sends an SMP Security Request about 30 ms
// after each connect. Windows answers with a pairing it never completes and
// drops the link at the SMP timeout, 30 s later (FlexInput commit
// f395e3511). While the controller's GattSession holds MaintainConnection,
// Windows connects the link again by itself, and every characteristic of
// the old link is closed (RO_E_CLOSED, crates/joycon2/src/hub.rs:578-582
// and :649-652 at 0905b2fe). So the driver keeps the device and the session,
// as FlexInput keeps them while it waits, since its first version let them
// go and every reconnect then failed with Device not found (hub.rs:631-638).
// It reads the status every BLE_RECOVERY_POLL_MS for up to
// BLE_RECOVERY_WAIT_MS, as FlexInput waits for Windows (hub.rs:709-742),
// then finds the service, uncached, and the characteristics again and starts
// the controller once more. FlexInput disconnects and connects anew at that
// point, since its btleplug clears the services it holds only on a
// disconnect (hub.rs:649-657). The driver keeps the device and the session
// through the restart too, so MaintainConnection holds throughout
// (hifihedgehog/SDL#49). The joystick stays published, and its controls rest
// for the gap.
// The recovery thread never takes the joystick lock. While it runs, the
// joystick thread touches none of the controller's WinRT objects
// (BLE_LinkReady), and the thread hands the joystick thread the end through
// recovery, which Detect reads.
// ---------------------------------------------------------------------------

// The joystick thread may use the controller's WinRT objects
static bool BLE_LinkReady(BLE_Controller *ctrl)
{
    return SDL_GetAtomicInt(&ctrl->recovery) == BLE_RECOVERY_IDLE;
}

// Sleeps ms in slices of at most 50 ms. False once Quit has told the run to
// stop.
static bool BLE_RecoveryDelay(BLE_Controller *ctrl, Uint32 ms)
{
    Uint32 slept = 0;

    while (slept < ms) {
        const Uint32 slice = SDL_min(50u, ms - slept);

        if (BLE_Canceled(ctrl)) {
            return false;
        }
        SDL_Delay(slice);
        slept += slice;
    }
    return !BLE_Canceled(ctrl);
}

// The connect's steps in its order, then the controller's start: the
// console session on the console path, the unified start of a Joy-Con 2 on
// the unified path. The path stays the one the connect chose. Each delegate
// is the controller's own, registered again on the characteristic found
// anew, and one the connect could not register, as when the link dropped
// during the setup, is added now (BLE_Subscribe). The unified path reads the
// stick calibration again only when the connect read not all of it. The
// player LED and the features an app turned on are the joystick thread's to
// set again (BLE_FinishRestore). True once the start ran, which on the
// unified path needs the input subscribed.
static bool BLE_StartAgain(BLE_Controller *ctrl, BleDevice3 *device3)
{
    bool input;

    (void)BLE_Subscribe(ctrl, ctrl->response_char, &ctrl->response_handler, &ctrl->response_token, BLE_OnResponseValue, 0);
    if (!ctrl->console && !ctrl->calibrated) {
        BLE_ReadCalibration(ctrl);
    }
    input = BLE_Subscribe(ctrl, ctrl->input_char, &ctrl->input_handler, &ctrl->input_token, BLE_OnInputValue, BLE_INPUT_UNIFIED);
    if (BLE_Canceled(ctrl)) {
        return false;
    }
    if (ctrl->console) {
        return BLE_StartConsoleSession(ctrl, device3);
    }
    if (!input) {
        return false;
    }
    if (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT ||
        ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT) {
        BLE_StartUnifiedJoyCon(ctrl, false);
    }
    return true;
}

// Finds the Switch 2 service and each characteristic of the controller's
// path, uncached, and needs every one the controller had. Each one found
// moves into the controller: one the controller had replaces the old link's,
// whose delegate is removed and which is released, and one the connect could
// not find, as when the link dropped during its lookups, is filled in. Then
// the controller starts again. False when the service or a characteristic
// the controller had is missing, as while Windows still holds the dropped
// link's GATT state (FlexInput hub.rs:790-805), or when the start fails, so
// the next poll tries again.
static bool BLE_RestoreLink(BLE_Controller *ctrl)
{
    GattChar **const fields[] = {
        &ctrl->input_char, &ctrl->command_char, &ctrl->response_char, &ctrl->vibration_char,
        &ctrl->console_command_char, &ctrl->native_input_char, &ctrl->extended_response_char
    };
    // The delegate registered on each, and its registration
    void *const handlers[SDL_arraysize(fields)] = {
        ctrl->input_handler, NULL, ctrl->response_handler, NULL,
        NULL, ctrl->native_input_handler, ctrl->extended_response_handler
    };
    const EventRegistrationToken tokens[SDL_arraysize(fields)] = {
        ctrl->input_token, { 0 }, ctrl->response_token, { 0 },
        { 0 }, ctrl->native_input_token, ctrl->extended_response_token
    };
    GattChar *found[SDL_arraysize(fields)];
    BleDevice3 *device3 = NULL;
    GattService3 *service3;
    bool complete = false, started = false;
    int i;

    SDL_zeroa(found);
    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_QueryInterface(ctrl->device, &IID_BleDevice3, (void **)&device3)) || !device3) {
        return false;
    }
    service3 = SDL_BLEGATT_FindService(device3, &GUID_Switch2Service, 1, BLE_RECOVERY_DISCOVERY_ATTEMPTS, "Switch2", &ctrl->run->cancel);
    if (service3) {
        BLE_FindUnifiedCharacteristics(ctrl->product_id, service3, &ctrl->run->cancel, &found[0], &found[1], &found[2], &found[3]);
        if (ctrl->console) {
            BLE_FindConsoleCharacteristics(ctrl->product_id, service3, &ctrl->run->cancel, &found[4], &found[5], &found[6]);
        }
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3_Release(service3);
        complete = true;
        for (i = 0; i < (int)SDL_arraysize(fields); ++i) {
            if (*fields[i] && !found[i]) {
                complete = false;
            }
        }
    }
    if (complete) {
        // The joystick thread takes what this adds of rumble once the
        // recovery has ended (BLE_FinishRestore)
        for (i = 0; i < (int)SDL_arraysize(fields); ++i) {
            GattChar *old = *fields[i];

            if (!found[i]) {
                continue; // complete, so the controller had none here either
            }
            *fields[i] = found[i];
            found[i] = NULL;
            if (old) {
                if (handlers[i]) {
                    __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_remove_ValueChanged(old, tokens[i]);
                }
                __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(old);
            }
        }
        started = BLE_StartAgain(ctrl, device3);
    }
    for (i = 0; i < (int)SDL_arraysize(fields); ++i) {
        if (found[i]) {
            __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(found[i]);
        }
    }
    __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3_Release(device3);
    return started;
}

// Reads the status every BLE_RECOVERY_POLL_MS until BLE_RECOVERY_WAIT_MS has
// passed since the loss, as FlexInput's await_windows_reconnect does
// (hub.rs:721-742), and restores the link each time it reads Connected. A
// loss during the start leaves the link down, so the status is read once
// more after link_lost is cleared, and a loss after that read starts the
// next recovery.
static int SDLCALL BLE_RecoveryThread(void *data)
{
    BLE_Controller *ctrl = (BLE_Controller *)data;
    BLE_Run *run = ctrl->run;
    const bool ro_inited = SDL_BLEGATT_InitThread();
    bool restored = false;

    // The stop before the clock, so a thread Quit left to end on its own
    // reads none (BLE_Run)
    while (!restored && !BLE_Canceled(ctrl) && SDL_GetTicks() - ctrl->lost_ms < BLE_RECOVERY_WAIT_MS &&
           BLE_RecoveryDelay(ctrl, BLE_RECOVERY_POLL_MS)) {
        if (SDL_BLEGATT_IsConnected(ctrl->device) && BLE_RestoreLink(ctrl)) {
            SDL_SetAtomicInt(&ctrl->link_lost, 0);
            restored = SDL_BLEGATT_IsConnected(ctrl->device);
        }
    }
    if (restored && !BLE_Canceled(ctrl)) {
        ctrl->restored_ms = SDL_GetTicks();
    }
    if (!SDL_CompareAndSwapAtomicInt(&ctrl->recovery, BLE_RECOVERY_RUNNING, restored ? BLE_RECOVERY_RESTORED : BLE_RECOVERY_FAILED)) {
        // Quit let go of the controller while this ran (BLE_FreeController)
        BLE_ReleaseTransport(ctrl);
    }
    if (ro_inited) {
        SDL_BLEGATT_QuitThread();
    }
    // Last, since Quit may go on once the run's threads reach 0
    SDL_AddAtomicInt(&run->threads, -1);
    return 0;
}

// Starts the recovery of a lost link on its own thread, under the joystick
// lock. The joystick rests for the gap, as SDL recenters a joystick it
// removes (SDL_PrivateJoystickRemoved), and the first report after the gap
// posts every control again. False when the thread cannot start.
static bool BLE_StartRecovery(BLE_Controller *ctrl)
{
    SDL_Thread *thread;

    if (ctrl->joystick) {
        SDL_PrivateJoystickForceRecentering(ctrl->joystick);
    }
    SDL_LockMutex(ctrl->report_lock);
    ctrl->report_pending = false;
    SDL_UnlockMutex(ctrl->report_lock);
    ctrl->have_last_state = false;
    ctrl->lost_ms = SDL_GetTicks();
    SDL_SetAtomicInt(&ctrl->link_lost, 0);
    SDL_SetAtomicInt(&ctrl->recovery, BLE_RECOVERY_RUNNING);
    SDL_AddAtomicInt(&ctrl->run->threads, 1);
    thread = SDL_CreateThread(BLE_RecoveryThread, "BLESwitch2Recover", ctrl);
    if (!thread) {
        SDL_AddAtomicInt(&ctrl->run->threads, -1);
        SDL_SetAtomicInt(&ctrl->recovery, BLE_RECOVERY_IDLE);
        return false;
    }
    SDL_DetachThread(thread);
    return true;
}

// The joystick thread's end of a recovery that brought the link back, under
// the joystick lock: the player LED of an open joystick, and the features an
// app turned on, both of which the controller lost with the link, and the
// GameCube motor, which the rumble pump writes again
static void BLE_FinishRestore(BLE_Controller *ctrl)
{
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 link restored after %" SDL_PRIu64 " ms (%s path)",
                 ctrl->restored_ms - ctrl->lost_ms, ctrl->console ? "console" : "unified");
    ctrl->connected_ms = ctrl->restored_ms;
    SDL_SetAtomicInt(&ctrl->recovery, BLE_RECOVERY_IDLE);
    ctrl->gc_motor_known = false;
    // A restart can find the rumble characteristic the connect could not
    // (BLE_RestoreLink)
    if (!ctrl->rumble_capable && BLE_CanRumble(ctrl)) {
        ctrl->rumble_capable = true;
        if (ctrl->joystick) {
            SDL_SetBooleanProperty(SDL_GetJoystickProperties(ctrl->joystick), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, true);
        }
    }
    if (ctrl->joystick) {
        BLE_SetPlayerLED(ctrl, ctrl->player_index);
        if (!ctrl->console && ctrl->sensors_enabled) {
            BLE_SendFeatures(ctrl);
        }
    }
}

// True when a lost link follows a shutdown the app sent and the controller
// obeyed it: no input arrived later than BLE_SHUTDOWN_GRACE_MS after the
// write. *elapsed_ms gets the time since the write. A controller that
// ignored the command has reported since, so its loss is recovered like any
// other. FlexInput draws the same line: SessionEnd::Shutdown returns without
// a reconnect, and only SessionEnd::LinkLost waits for Windows
// (crates/joycon2/src/hub.rs:565-571 and :626-647 at 0905b2fe). Under the
// joystick lock.
static bool BLE_LossAfterShutdown(BLE_Controller *ctrl, Uint64 *elapsed_ms)
{
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    Uint64 last_report;

    if (!ctrl->shutdown_sent) {
        return false;
    }
    SDL_LockMutex(ctrl->report_lock);
    last_report = ctrl->report_counter;
    SDL_UnlockMutex(ctrl->report_lock);
    if (last_report > ctrl->shutdown_counter + (Uint64)BLE_SHUTDOWN_GRACE_MS * frequency / 1000) {
        return false;
    }
    *elapsed_ms = (SDL_GetPerformanceCounter() - ctrl->shutdown_counter) * 1000 / frequency;
    return true;
}

// ---------------------------------------------------------------------------
// SDL_JoystickDriver entry points.
// ---------------------------------------------------------------------------
static bool BLE_JoystickInit(void)
{
    BLE_Run *run;

    if (!SDL_GetHintBoolean(SDL_HINT_JOYSTICK_BLE_SWITCH2, false)) {
        return true; // disabled, but the driver still loads cleanly
    }
    if (ble.initialized) {
        return true;
    }
    // A new run, since a thread that the last Quit left running still
    // holds the last one (BLE_Run)
    run = (BLE_Run *)SDL_calloc(1, sizeof(*run));
    if (!run) {
        return false;
    }

    // RoInitialize, the combase entry points and the MTA pin are the shared
    // transport's, counted across both BLE drivers.
    if (!SDL_BLEGATT_Init()) {
        SDL_free(run);
        return false;
    }
    ble.transport = true;
    ble.run = run;

    // Start the advertisement watcher, or join it when the generic BLE GATT
    // driver started it first. A watcher whose start failed still counts,
    // and Detect has it started again.
    SDL_SetAtomicInt(&ble_listening, 1);
    if (SDL_BLEGATT_AddListener(BLE_OnAdvertisement, NULL)) {
        ble.scanning = true;
        ble.watcher_checked = SDL_GetTicks();
    }

    ble.initialized = true;
    return true;
}

static void BLE_JoystickDetectDevices(void)
{
    // Drain link-loss flags set by ConnectionStatusChanged (MTA callback) and
    // the ends of the recoveries, on the joystick thread. DetectDevices runs
    // under SDL_LockJoysticks (SDL_UpdateJoystickDevices, after
    // RunJoystickUpdates' per-joystick update walk), the same lock the connect
    // append and Quit hold, so the array walk is safe. A lost link starts a
    // recovery in place (BLE_StartRecovery), and the joystick stays. Only a
    // link that did not come back, or one lost after a shutdown the app sent
    // (BLE_LossAfterShutdown), removes the controller, which posts the
    // removal to the app and makes BLE_GetControllerByAddress return NULL for
    // the address, so BLE_TryReserveConnect accepts the pad's next
    // advertisement and it can reconnect (SDL#9 follow-up). BLE_FreeController
    // deliberately leaks the callback-touched state, so an in-flight callback
    // on the dead controller stays safe.
    int i;

    // A watcher whose start failed, as with the radio off at Init, or one
    // that aborted, as when the radio is turned off, never runs again by
    // itself (SDL_ble_gatt.h). The transport puts a new one in place, as the
    // BLE GATT driver's Detect has it do, since that driver checks the
    // watcher only while one of its own families is on.
    if (ble.scanning && SDL_GetTicks() - ble.watcher_checked >= BLE_WATCHER_CHECK_MS) {
        ble.watcher_checked = SDL_GetTicks();
        (void)SDL_BLEGATT_CheckWatcher();
    }
    for (i = 0; i < ble.controller_count; ) {
        BLE_Controller *ctrl = ble.controllers[i];
        const int recovery = SDL_GetAtomicInt(&ctrl->recovery);
        const char *path = ctrl->console ? "console" : "unified";
        Uint64 since_shutdown = 0;
        bool remove = false;

        // The first vendor frame the response callback kept, logged here,
        // since a callback can still run after SDL_Quit has shut the log down
        {
            Uint8 frame[sizeof(ctrl->vendor_frame)];
            int length = 0;

            SDL_LockMutex(ctrl->response_lock);
            if (ctrl->vendor_frame_pending) {
                ctrl->vendor_frame_pending = false;
                length = ctrl->vendor_frame_length;
                SDL_memcpy(frame, ctrl->vendor_frame, sizeof(frame));
            }
            SDL_UnlockMutex(ctrl->response_lock);
            if (length > 0) {
                BLE_LogBytes(ctrl, "first vendor frame", frame, length);
            }
        }

        if (recovery == BLE_RECOVERY_RESTORED) {
            BLE_FinishRestore(ctrl);
        } else if (recovery == BLE_RECOVERY_FAILED) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 link not restored after %d s (%s path): removing the joystick",
                         BLE_RECOVERY_WAIT_MS / 1000, path);
            remove = true;
        } else if (recovery == BLE_RECOVERY_IDLE && SDL_GetAtomicInt(&ctrl->link_lost) && BLE_LossAfterShutdown(ctrl, &since_shutdown)) {
            // The app turned the controller off, so the link is not coming
            // back. The joystick goes now, and the session closes with it,
            // so Windows stops connecting a controller that is off
            // (hifihedgehog/SDL#50).
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 link lost %" SDL_PRIu64 " ms after a shutdown the app sent: removing the joystick",
                         since_shutdown);
            remove = true;
        } else if (recovery == BLE_RECOVERY_IDLE && SDL_GetAtomicInt(&ctrl->link_lost)) {
            // How long the link held. A Joy-Con 2 clone such as the Mobapad
            // M12-S sends an SMP Security Request about 30 ms after each
            // connect. Windows answers with a pairing it never completes and
            // drops the link at the SMP timeout, 30 s later (FlexInput
            // commit f395e3511 and crates/joycon2/src/win_pair.rs:5-13,
            // Bluetooth Core 5.4 Vol 3 Part H 3.4). A genuine Joy-Con 2 sends
            // no Security Request (joycon2android docs/protocol.md:440-441 at
            // 8a1ed94).
            // A clone also drops its link after 60 s without the console
            // handshake (tarabdaar JoyConBLE.swift maybeBeginInit,
            // hifihedgehog/SDL#38).
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 link lost %" SDL_PRIu64 " ms after it came up (%s path): waiting up to %d s for it",
                         SDL_GetTicks() - ctrl->connected_ms, path, BLE_RECOVERY_WAIT_MS / 1000);
            remove = !BLE_StartRecovery(ctrl);
        }
        if (remove) {
            SDL_RemoveJoystickDevice(ctrl->instance_id);
            ble.controllers[i] = ble.controllers[--ble.controller_count];
            BLE_FreeController(ctrl);
        } else {
            ++i;
        }
    }
}

static void BLE_JoystickSetPlayerIndex(SDL_Joystick *joystick, int player_index)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    if (ctrl) {
        ctrl->player_index = player_index;
        // A recovery lights it once the link is back (BLE_FinishRestore)
        if (BLE_LinkReady(ctrl)) {
            BLE_SetPlayerLED(ctrl, player_index);
        }
    }
}

static bool BLE_JoystickOpen(SDL_Joystick *joystick, SDL_JoystickDevice *device)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(device->instance_id);
    if (!ctrl) {
        return SDL_SetError("BLE joystick disconnected");
    }
    ctrl->joystick = joystick;

    /* Stable identity across reconnects (SDL#9 follow-up): expose the pad's
       Bluetooth address as the joystick serial, twelve bare lowercase hex
       digits, the same shape hidapi reports for Bluetooth pads. Without it
       every consumer identity falls back to the SDL instance ID, which mints
       a new identity on every power-cycle and orphans per-device settings.
       Core owns and frees the string (SDL_CloseJoystick), matching the
       hidapi open's SDL_strdup assignment (HIDAPI_JoystickOpen). */
    {
        char serial[13];
        (void)SDL_snprintf(serial, sizeof(serial), "%012llx", (unsigned long long)ctrl->bluetooth_address);
        joystick->serial = SDL_strdup(serial);
    }

    /* Snapshot the vertical-orientation hint at open, mirroring the wired
       driver (SDL_hidapi_switch2.c OpenJoystick). The fabricated gamepad
       mapping keys on the same hint, so the decoder and the mapping agree. */
    ctrl->vertical_mode = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, false);

    switch (ctrl->product_id) {
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT:
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT:
        joystick->nbuttons = 17; // SDL_GAMEPAD_NUM_SWITCH2_JOYCON_BUTTONS
        break;
    default:
        joystick->nbuttons = (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_PRO) ? 15 : 12;
        break;
    }
    /* Extra raw axes beyond the gamepad set, each behind its hint: the
       optical mouse's absolute counters (axis 6 = X, 7 = Y) and the
       magnetometer sample (the next three, following whichever of the mouse
       axes exist). Raw axis count is the consumer's availability contract:
       6 = neither, 8 = mouse, 9 = magnetometer, 11 = both. */
    joystick->naxes = SDL_GAMEPAD_AXIS_COUNT +
                      (ctrl->mouse_enabled ? 2 : 0) +
                      (ctrl->magnetometer_enabled ? 3 : 0);
    joystick->nhats = 1;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;

    // The console report carries no gyroscope (hifihedgehog/SDL#38)
    if (!ctrl->console) {
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, 250.0f);
    }
    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 250.0f);

    // Advertise the rumble capability so apps that gate on it (PadForge's FFB
    // passthrough reads SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN into HasRumble) will
    // drive it, mirroring SDL_hidapijoystick.c:860. The cap must track actual
    // delivery: the GameCube rumbles over the command channel (BLE_PumpGameCubeRumble),
    // every other type over the vibration characteristic. The connect decided
    // it, and only the joystick thread changes it, when a recovery found the
    // characteristic the connect could not (BLE_FinishRestore).
    if (ctrl->rumble_capable) {
        SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, true);
    }

    // SDL_OpenJoystick calls SetPlayerIndex right after Open for a joystick
    // with a player index, and that call lights the slot's LED. Open turns
    // the LEDs off only for a joystick without one, so the LED command, which
    // waits for its response, goes out once. During a recovery the LED waits
    // for the link (BLE_FinishRestore).
    ctrl->player_index = SDL_GetJoystickPlayerIndex(joystick);
    if (ctrl->player_index < 0 && BLE_LinkReady(ctrl)) {
        BLE_SetPlayerLED(ctrl, ctrl->player_index);
    }
    return true;
}

static bool BLE_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    if (!ctrl || !ctrl->rumble_capable) {
        return SDL_Unsupported();
    }
    // Store the commanded amplitudes for the keep-alive pump (BLE_JoystickUpdate),
    // and write once now to keep the initial latency low. SDL only resends rumble
    // every SDL_RUMBLE_RESEND_MS (2 s), which the non-latching actuator reads as a
    // single pulse, so the pump is what sustains a continuous effect.
    SDL_LockMutex(ctrl->report_lock);
    ctrl->rumble_low = low_frequency_rumble;
    ctrl->rumble_high = high_frequency_rumble;
    ctrl->rumble_last_ms = SDL_GetTicks();
    SDL_UnlockMutex(ctrl->report_lock);
    // The GameCube PWM state is owned by the update thread, so do not write inline
    // here (a different thread); the pump picks up the stored amplitudes next tick.
    // During a recovery the pump writes once the link is back.
    if (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER || !BLE_LinkReady(ctrl)) {
        return true;
    }
    return BLE_WriteRumble(ctrl, low_frequency_rumble, high_frequency_rumble);
}

static bool BLE_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    (void)joystick;
    (void)left_rumble;
    (void)right_rumble;
    return SDL_Unsupported();
}

static bool BLE_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)joystick;
    (void)red;
    (void)green;
    (void)blue;
    return SDL_Unsupported(); // No RGB; player LED is set from the player index
}

/* Raw Switch 2 command passthrough (hifihedgehog/SDL#9): bytes[0] = command id,
   bytes[1] = subcommand id, bytes[2..] = payload. Windows GATT sessions must all
   share a service for a second opener to reach it, and this driver's session is
   exclusive, so the driver itself is the only writer that can deliver vendor
   commands like the shutdown (command 0x06 subcommand 0x02 + 12 zero bytes,
   commands.md "Shutdown Controller?"). Mirrors the upstream hidapi Switch
   driver's SendEffect-as-raw-passthrough philosophy.

   The frame is BLE_BuildCommand's and goes out through BLE_WriteCommandFrame,
   so on the console path it reaches the side's command characteristic. The
   write is fire-and-forget rather than routed through BLE_SendCommand because
   that helper waits for the reply and returns its byte count. A shutdown
   powers the pad off instead of replying (commands.md documents an empty
   response), so the helper would stall and then read a successful shutdown as
   a failure. Success here means the write was delivered. A reply to an effect
   can count only for a later sender of the same command and subcommand,
   since a reply must echo both (BLE_FindReply). No effect goes out while a
   recovery runs, since the link is down.

   A shutdown that was delivered is remembered (hifihedgehog/SDL#50). The
   controller powers off without a reply, and Detect then removes its
   joystick at the loss that follows instead of waiting for the link to come
   back (BLE_LossAfterShutdown). */
static bool BLE_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    const Uint8 *bytes = (const Uint8 *)data;
    Uint8 frame[BLE_COMMAND_FRAME_MAX];
    int length;

    if (ctrl && !BLE_LinkReady(ctrl)) {
        return SDL_SetError("The Switch 2 controller's link is down");
    }
    if (!ctrl || !BLE_HasCommandChannel(ctrl)) {
        return SDL_SetError("No BLE command channel for joystick");
    }
    if (size < 2) {
        return SDL_SetError("Switch 2 effect needs at least [cmd, subcmd]");
    }
    length = BLE_BuildCommand(frame, bytes[0], bytes[1], bytes + 2, size - 2);
    if (length == 0) {
        return SDL_SetError("Switch 2 effect payload too large");
    }
    // On the console path the command goes to the side's command
    // characteristic (BLE_WriteCommandFrame)
    if (!BLE_WriteCommandFrame(ctrl, frame, length, true)) {
        return SDL_SetError("Switch 2 command write failed");
    }
    if (bytes[0] == 0x06 && bytes[1] == 0x02) {
        ctrl->shutdown_sent = true;
        ctrl->shutdown_counter = SDL_GetPerformanceCounter();
    }
    return true;
}

static bool BLE_JoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    if (!ctrl) {
        return SDL_SetError("No BLE controller for joystick");
    }
    ctrl->sensors_enabled = enabled;
    if (ctrl->console) {
        // The console session's feature mask 0x37 already runs motion, and
        // no feature-select reply changes the report on this hardware
        // (joycon2android protocol.md "Motion block"), so only the posting
        // follows the request (hifihedgehog/SDL#38).
        return true;
    }
    // A recovery sends them once the link is back (BLE_FinishRestore)
    if (BLE_LinkReady(ctrl)) {
        BLE_SendFeatures(ctrl);
    }
    return true;
}

static void BLE_JoystickUpdate(SDL_Joystick *joystick)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    Uint8 data[64];
    int size = 0;

    // While a recovery runs, the joystick holds the rest BLE_StartRecovery
    // sent, and no rumble goes out
    if (!ctrl || !BLE_LinkReady(ctrl)) {
        return;
    }
    SDL_LockMutex(ctrl->report_lock);
    if (ctrl->report_pending) {
        size = ctrl->report_size;
        SDL_memcpy(data, ctrl->report, size);
        ctrl->report_pending = false;
    }
    SDL_UnlockMutex(ctrl->report_lock);

    if (size > 0) {
        // The BLE notification is the raw report (no report-ID prefix); the
        // decoders use absolute BLE offsets. Dispatch by controller type.
        if (!ctrl->logged_first_report) {
            ctrl->logged_first_report = true;
            BLE_LogBytes(ctrl, ctrl->console ? "first console report" : "first input report", data, size); // confirms the byte offsets
        }
        if (ctrl->console) {
            BLE_DecodeConsoleReport(ctrl, joystick, data, size);
        } else {
            BLE_DecodeReport(ctrl, joystick, data, size);
        }
    }
    if (ctrl->console && !ctrl->logged_unified_in_console && SDL_GetAtomicInt(&ctrl->unified_reports) > 0) {
        // A genuine pad that missed BLE_CONSOLE_FALLBACK_MS keeps the console
        // path until it reconnects
        ctrl->logged_unified_in_console = true;
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 unified input reported after the console path was chosen");
    }

    // Rumble keep-alive: re-send the last commanded amplitudes, rate-gated, so a
    // single SDL_RumbleJoystick call sustains on the non-latching actuator. The
    // write is done outside report_lock since it can block briefly.
    {
        bool pump = false;
        Uint16 rlow = 0, rhigh = 0;
        Uint64 now = SDL_GetTicks();
        // The GameCube keeps ticking one extra cycle after the amplitudes hit zero so
        // the PWM can send the final motor-off; gc_motor_on is update-thread only.
        bool gc_active = (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER) && ctrl->gc_motor_on;
        SDL_LockMutex(ctrl->report_lock);
        if ((ctrl->rumble_low || ctrl->rumble_high || gc_active) && (now - ctrl->rumble_last_ms >= BLE_RUMBLE_INTERVAL_MS)) {
            rlow = ctrl->rumble_low;
            rhigh = ctrl->rumble_high;
            ctrl->rumble_last_ms = now;
            pump = true;
        }
        SDL_UnlockMutex(ctrl->report_lock);
        if (pump) {
            BLE_WriteRumble(ctrl, rlow, rhigh);
        }
    }
}

static void BLE_JoystickClose(SDL_Joystick *joystick)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    if (ctrl) {
        ctrl->joystick = NULL;
    }
}

// Removes the characteristic's value delegate when one is registered (a
// handler and its token), then releases the characteristic
static void BLE_ReleaseCharacteristic(GattChar *characteristic, const void *handler, const EventRegistrationToken *token)
{
    if (!characteristic) {
        return;
    }
    if (handler && token) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_remove_ValueChanged(characteristic, *token);
    }
    __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(characteristic);
}

// Stop new notifications and release the WinRT COM objects. An in-flight
// ValueChanged callback uses only its event args and the controller's own
// buffers, not these objects, so releasing them here is safe. (WinRT holds
// its own reference to the characteristic for the duration of a callback.)
// The session goes before the device, as bleak closes them
// (backends/winrt/client.py:486-503), with MaintainConnection cleared, so
// Windows stops connecting a controller the driver has let go of
// (hifihedgehog/SDL#49).
static void BLE_ReleaseTransport(BLE_Controller *ctrl)
{
    BLE_ReleaseCharacteristic(ctrl->input_char, ctrl->input_handler, &ctrl->input_token);
    BLE_ReleaseCharacteristic(ctrl->command_char, NULL, NULL);
    BLE_ReleaseCharacteristic(ctrl->response_char, ctrl->response_handler, &ctrl->response_token);
    BLE_ReleaseCharacteristic(ctrl->vibration_char, NULL, NULL);
    BLE_ReleaseCharacteristic(ctrl->console_command_char, NULL, NULL);
    BLE_ReleaseCharacteristic(ctrl->native_input_char, ctrl->native_input_handler, &ctrl->native_input_token);
    BLE_ReleaseCharacteristic(ctrl->extended_response_char, ctrl->extended_response_handler, &ctrl->extended_response_token);
    ctrl->input_char = NULL;
    ctrl->command_char = NULL;
    ctrl->response_char = NULL;
    ctrl->vibration_char = NULL;
    ctrl->console_command_char = NULL;
    ctrl->native_input_char = NULL;
    ctrl->extended_response_char = NULL;
    if (ctrl->device && ctrl->status_handler) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_remove_ConnectionStatusChanged(ctrl->device, ctrl->status_token);
    }
    if (ctrl->session) {
        SDL_BLEGATT_CloseSession(ctrl->session);
        ctrl->session = NULL;
    }
    if (ctrl->device) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(ctrl->device);
        ctrl->device = NULL;
    }
}

static void BLE_FreeController(BLE_Controller *ctrl)
{
    // The controller's threads log nothing more
    SDL_SetAtomicInt(&ctrl->quiet, 1);

    // A recovery still running owns the WinRT objects, and its thread
    // releases them when it ends (BLE_RecoveryThread). That happens only when
    // Quit gave up waiting for it (BLE_QUIT_WAIT_MS).
    if (SDL_CompareAndSwapAtomicInt(&ctrl->recovery, BLE_RECOVERY_RUNNING, BLE_RECOVERY_ABANDONED)) {
        return;
    }
    BLE_ReleaseTransport(ctrl);

    // Deliberately leak the controller struct, its locks/semaphore, buffers, and
    // the delegates. A GATT ValueChanged callback may still be in-flight on a
    // WinRT thread-pool thread and touches exactly these, and GATT
    // remove_ValueChanged does not drain in-flight invocations. Destroying the
    // mutex or freeing the struct here would be a use-after-free. The leak is
    // bounded (one controller per connection attempt, reclaimed at process exit);
    // a future refcounted-controller design could free it deterministically.
}

static void BLE_JoystickQuit(void)
{
    BLE_Run *run = ble.run;
    const Uint64 start = SDL_GetTicks();
    int i;

    // The watcher stops when the last listener goes
    if (ble.scanning) {
        SDL_BLEGATT_RemoveListener(BLE_OnAdvertisement, NULL);
    }

    // The stop. Every connect and recovery thread ends at its next step, and
    // an advertisement callback takes nothing more. None of them waits on
    // the joystick lock, which Quit holds (BLE_LockForThread,
    // BLE_LockForListener).
    if (run) {
        SDL_SetAtomicInt(&run->cancel, 1);
    }
    SDL_SetAtomicInt(&ble_listening, 0);

    // A callback the watcher began before the listener was removed leaves
    // within a few ms of the stop: it gives up its wait for the joystick
    // lock, and one that had reserved an address starts its connect thread,
    // which the wait below counts
    while (SDL_GetAtomicInt(&ble_listener_calls) > 0) {
        SDL_Delay(1);
    }

    // Quit waits for the threads, as the BLE GATT driver waits for its
    // connections (BLEGATT_JoystickQuit). One still running after
    // BLE_QUIT_WAIT_MS is left to end on its own and logs nothing more.
    if (run) {
        int left;

        while (SDL_GetAtomicInt(&run->threads) > 0 && SDL_GetTicks() - start < BLE_QUIT_WAIT_MS) {
            SDL_Delay(1);
        }
        left = SDL_GetAtomicInt(&run->threads);
        if (left > 0) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 quit: %d threads still running after %d ms, left to end on their own", left,
                         BLE_QUIT_WAIT_MS);
            SDL_SetAtomicInt(&run->quiet, 1);
        }
    }
    for (i = 0; i < ble.controller_count; ++i) {
        BLE_FreeController(ble.controllers[i]);
    }
    SDL_free(ble.controllers);
    ble.controllers = NULL;
    ble.controller_count = 0;
    // A connect thread that ended gave its address back, and one left
    // running gives nothing back after the stop (BLE_ReleaseConnect)
    ble.connecting_count = 0;

    if (ble.transport) {
        SDL_BLEGATT_Quit();
        ble.transport = false;
    }
    // A run whose threads have all ended is freed. One with a thread left
    // running is kept, since that thread still reads it.
    if (run && SDL_GetAtomicInt(&run->threads) == 0) {
        SDL_free(run);
    }
    ble.run = NULL;
    ble.scanning = false;
    ble.initialized = false;
}

static bool BLE_JoystickGetGamepadMapping(SDL_JoystickDevice *device, SDL_GamepadMapping *out)
{
    (void)device;
    (void)out;
    return false; // resolved by the 'h'+VID/PID fabricated mapping
}

// A controller is published with SDL_AddJoystickDevice by its connect
// worker, so the driver has no QueryDevices or PrepareDevice
SDL_JoystickDriver SDL_BLE_JoystickDriver = {
    .Init = BLE_JoystickInit,
    .DetectDevices = BLE_JoystickDetectDevices,
    .GetGamepadMapping = BLE_JoystickGetGamepadMapping,
    .Open = BLE_JoystickOpen,
    .Rumble = BLE_JoystickRumble,
    .RumbleTriggers = BLE_JoystickRumbleTriggers,
    .SetLED = BLE_JoystickSetLED,
    .SendEffect = BLE_JoystickSendEffect,
    .SetSensorsEnabled = BLE_JoystickSetSensorsEnabled,
    .SetPlayerIndex = BLE_JoystickSetPlayerIndex,
    .Update = BLE_JoystickUpdate,
    .Close = BLE_JoystickClose,
    .Quit = BLE_JoystickQuit
};

#endif // SDL_JOYSTICK_BLE
