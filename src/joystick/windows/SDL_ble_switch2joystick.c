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
 * owns a WinRT BLE connection, modeled on the WGI driver's WinRT-from-C
 * mechanics (SDL_windows_gaming_input.c). The WinRT transport it shares with
 * the generic BLE GATT driver lives in SDL_ble_gatt.c (hifihedgehog/SDL#33
 * Part 12): the runtime, the awaits, the delegates, the advertisement watcher,
 * the open, the uncached discovery and the writes.
 *
 * Hardware-gated: the BLE report byte offsets and IMU scale are reasoned from
 * the reference reimplementations (ndeadly/Nadeflore/joycon2cpp) and must be
 * confirmed against a physical controller. The build is the only verification
 * available in the SDL-fork environment.
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
// ourselves (same approach as SDL_windows_gaming_input.c). The transport's
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

#define NINTENDO_BLE_COMPANY_ID 0x0553

// Keep-alive cadence for the continuous-drive rumble pump (about 100 Hz). The
// reference paces its ESP32 bridge at 7.5 ms; on a direct BLE link 10 ms sustains
// the effect without flooding the shared connection.
#define BLE_RUMBLE_INTERVAL_MS 10

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
    GattChar *input_char;
    GattChar *command_char;
    GattChar *response_char;
    GattChar *vibration_char;
    EventRegistrationToken input_token;
    EventRegistrationToken response_token;
    void *input_handler;    // heap delegate, freed in BLE_FreeController
    void *response_handler; // heap delegate, freed in BLE_FreeController
    EventRegistrationToken status_token;
    void *status_handler;   // ConnectionStatusChanged delegate, leaked like the others
    SDL_AtomicInt link_lost; // set by the status callback (MTA), drained by Detect

    SDL_Joystick *joystick; // set in Open, NULL otherwise

    // Latest input report, filled by the ValueChanged callback (MTA thread),
    // drained by Update (joystick thread).
    SDL_Mutex *report_lock;
    Uint8 report[64];
    int report_size;
    bool report_pending;
    Uint8 last_state[64];
    bool have_last_state;
    bool logged_first_report; // one-shot debug dump of the first input notification

    // Command/response channel: a write to command_char produces a notification
    // on response_char. The ValueChanged handler stashes it and signals. A flash
    // read reply is 0x10 header + 0x40 data = 0x50 bytes, so size for that.
    SDL_Mutex *response_lock;
    SDL_Semaphore *response_sem;
    Uint8 response[128];
    int response_size;

    // Stick calibration (reimplemented from the wired driver; static there).
    Switch2_AxisCal left_x, left_y, right_x, right_y;
    bool calibrated;
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

static void BLE_ParseStickCalibration(Switch2_AxisCal *x, Switch2_AxisCal *y, const Uint8 *data)
{
    x->neutral = (Uint16)(data[0] | ((data[1] & 0x0F) << 8));
    y->neutral = (Uint16)((data[1] >> 4) | (data[2] << 4));
    x->max = (Uint16)(data[3] | ((data[4] & 0x0F) << 8));
    y->max = (Uint16)((data[4] >> 4) | (data[5] << 4));
    x->min = (Uint16)(data[6] | ((data[7] & 0x0F) << 8));
    y->min = (Uint16)((data[7] >> 4) | (data[8] << 4));
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

// Forward declarations.
static void BLE_ConnectAndSubscribe(Uint64 bluetooth_address, Uint16 vendor_id, Uint16 product_id, char *name);
static void BLE_FreeController(BLE_Controller *ctrl);
static BLE_Controller *BLE_GetControllerByAddress(Uint64 address);
static void BLE_ReleaseConnect(Uint64 address);

// The advertisement Received callback hands the connect to this worker thread so
// the WinRT thread-pool callback returns immediately. The connect blocks on
// multi-second async opens and discovery, and blocking the callback thread starves
// the same thread pool that must deliver those completions (hifihedgehog/SDL#5:
// device=NULL on every attempt). joycon2cpp opens on its own connect thread
// (testapp.cpp:1467 and :835) and bleak dispatches to a task (discoverer.py).
typedef struct
{
    Uint64 address;
    Uint16 vendor;
    Uint16 product;
} BLE_ConnectRequest;

static int SDLCALL BLE_ConnectThread(void *data)
{
    BLE_ConnectRequest *req = (BLE_ConnectRequest *)data;
    bool ro_inited = false;

    // Join the MTA explicitly. This is a fresh SDL thread, not a system thread-pool
    // thread, so it has no apartment of its own. RoGetActivationFactory can return
    // RO_E_UNINITIALIZED on an uninitialized thread even with the process-wide
    // implicit MTA from CoIncrementMTAUsage, so initialize it as MULTITHREADED
    // (not WIN_RoInitialize, which is STA-first and would risk a marshal-back
    // deadlock against the agile completion handlers this thread blocks on).
    ro_inited = SDL_BLEGATT_InitThread();
    BLE_ConnectAndSubscribe(req->address, req->vendor, req->product, NULL);
    BLE_ReleaseConnect(req->address); // address reserved by the caller before spawn
    if (ro_inited) {
        SDL_BLEGATT_QuitThread();
    }
    SDL_free(req);
    return 0;
}

// Reserve an address for connecting. Returns false if it is already connected or
// a connect is already in progress. Caller must BLE_ReleaseConnect on completion.
// Also false once the driver has quit: the shared watcher can still deliver an
// advertisement that it copied before BLE_JoystickQuit removed the listener
// (SDL_ble_gatt.h), and ble.initialized is written under this same lock.
static bool BLE_TryReserveConnect(Uint64 address)
{
    int i;
    bool reserved = false;

    SDL_LockJoysticks();
    if (ble.initialized && !BLE_GetControllerByAddress(address)) {
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
            reserved = true;
        }
    }
    SDL_UnlockJoysticks();
    return reserved;
}

// Record a full GATT-discovery failure for an address and arm an escalating
// backoff (15 s, 30 s, 60 s, ... capped at 5 min), so the watcher stops hammering
// a controller that is busy on USB. Cleared on a successful connect.
static void BLE_NoteConnectFailure(Uint64 address)
{
    int i;
    Uint64 now = SDL_GetTicks();

    SDL_LockJoysticks();
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

static void BLE_ReleaseConnect(Uint64 address)
{
    int i;

    SDL_LockJoysticks();
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
static void BLE_LogBytes(const char *label, const Uint8 *data, int len)
{
    char hex[3 * 64 + 1];
    int i, n = SDL_min(len, 64);
    for (i = 0; i < n; ++i) {
        (void)SDL_snprintf(&hex[i * 3], 4, "%02x ", data[i]);
    }
    hex[n > 0 ? n * 3 : 0] = '\0';
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 %s (%d bytes): %s", label, len, hex);
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

// ---------------------------------------------------------------------------
// Input characteristic ValueChanged: copy the raw report into the ring slot.
// Runs on a WinRT thread-pool (MTA) thread, called by the shared value delegate
// (SDL_ble_gatt.c) with the whole value. The report keeps its first 64 bytes.
// ---------------------------------------------------------------------------
static void BLE_OnInputValue(void *userdata, int characteristic, const Uint8 *data, size_t length)
{
    BLE_Controller *ctrl = (BLE_Controller *)userdata;
    int size = (int)SDL_min(length, sizeof(ctrl->report));
    (void)characteristic;

    if (!ctrl) {
        return;
    }
    if (size > 0) {
        SDL_LockMutex(ctrl->report_lock);
        SDL_memcpy(ctrl->report, data, size);
        ctrl->report_size = size;
        ctrl->report_pending = true;
        SDL_UnlockMutex(ctrl->report_lock);
    }
}

// ---------------------------------------------------------------------------
// Command-response characteristic ValueChanged: stash the reply and signal.
// Called by the shared value delegate, like BLE_OnInputValue. The reply keeps
// its first 128 bytes.
// ---------------------------------------------------------------------------
static void BLE_OnResponseValue(void *userdata, int characteristic, const Uint8 *data, size_t length)
{
    BLE_Controller *ctrl = (BLE_Controller *)userdata;
    (void)characteristic;

    if (!ctrl) {
        return;
    }
    SDL_LockMutex(ctrl->response_lock);
    ctrl->response_size = (int)SDL_min(length, sizeof(ctrl->response));
    SDL_memcpy(ctrl->response, data, ctrl->response_size);
    SDL_UnlockMutex(ctrl->response_lock);
    if (ctrl->response_size > 0) {
        SDL_SignalSemaphore(ctrl->response_sem);
    }
}

// Send a command and wait (briefly) for the reply on the response characteristic.
// Frame: [cmd] 0x91 0x01 [subcmd] 0x00 [data_len] 0x00 0x00 [data...]. Returns
// the number of reply bytes copied, or 0 on timeout.
static int BLE_SendCommand(BLE_Controller *ctrl, Uint8 cmd, Uint8 subcmd, const Uint8 *data, int data_len, Uint8 *reply, int reply_len)
{
    Uint8 frame[64];
    int got = 0;

    if (!ctrl->command_char || data_len < 0 || data_len + 8 > (int)sizeof(frame)) {
        return 0;
    }
    frame[0] = cmd;
    frame[1] = 0x91;
    frame[2] = 0x01; // Bluetooth transport
    frame[3] = subcmd;
    frame[4] = 0x00;
    frame[5] = (Uint8)data_len;
    frame[6] = 0x00;
    frame[7] = 0x00;
    if (data_len > 0) {
        SDL_memcpy(&frame[8], data, data_len);
    }
    while (SDL_TryWaitSemaphore(ctrl->response_sem)) {
        // drain stale replies
    }
    if (!SDL_BLEGATT_WriteCharacteristic(ctrl->command_char, frame, 8 + data_len, true)) {
        return 0;
    }
    if (ctrl->response_sem && SDL_WaitSemaphoreTimeout(ctrl->response_sem, 500)) {
        SDL_LockMutex(ctrl->response_lock);
        got = ctrl->response_size;
        if (reply && reply_len > 0) {
            got = SDL_min(got, reply_len);
            SDL_memcpy(reply, ctrl->response, got);
        }
        SDL_UnlockMutex(ctrl->response_lock);
    }
    return got;
}

// Read controller memory, transcribed from the BLE reference controller.py
// read_memory (switch2-controllers/controller.py:339-347): command 0x02/0x04
// with data {length, 0x7e, 0, 0, addr LE}. controller.py strips an 8-byte header
// in write_command and another 8 in read_memory, so the payload sits at raw reply
// offset 0x10. Returns the number of payload bytes copied (0 on failure).
static int BLE_ReadMemory(BLE_Controller *ctrl, Uint8 length, Uint32 addr, Uint8 *out, int out_len)
{
    Uint8 req[8] = { length, 0x7e, 0x00, 0x00, (Uint8)addr, (Uint8)(addr >> 8), (Uint8)(addr >> 16), (Uint8)(addr >> 24) };
    Uint8 reply[128];
    int got = BLE_SendCommand(ctrl, 0x02, 0x04, req, (int)sizeof(req), reply, (int)sizeof(reply));
    int n;

    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 memory read addr 0x%06x len %d: %d reply bytes", (unsigned)addr, length, got);
    if (got > 0) {
        BLE_LogBytes("memory reply", reply, got); // confirms the 0x10 payload offset + MTU
    }
    if (got < 0x10) {
        return 0;
    }
    n = SDL_min(got - 0x10, (int)length);
    n = SDL_min(n, out_len);
    SDL_memcpy(out, &reply[0x10], n);
    return n;
}

// Read stick calibration, transcribed from controller.py read_calibration_data
// (switch2-controllers/controller.py:353-368): user slot first (0x1FC042 /
// 0x1FC062), falling back to factory (0x0130A8 / 0x0130E8) when the first 3 bytes
// read 0xFFFFFF; parse StickCalibrationData directly from the 9-byte payload. A
// Joy-Con stores its single-stick calibration in the first slot. The decoders use
// left_x/left_y for the single Joy-Con stick, so both L and R store there. Best-
// effort: on failure BLE_MapStickAxis falls back to a linear map.
static bool BLE_ReadCalibSlot(BLE_Controller *ctrl, Uint32 user_addr, Uint32 factory_addr, Uint8 *out9)
{
    Uint8 cal[16];
    if (BLE_ReadMemory(ctrl, 0x0b, user_addr, cal, sizeof(cal)) >= 9 &&
        !(cal[0] == 0xFF && cal[1] == 0xFF && cal[2] == 0xFF)) {
        SDL_memcpy(out9, cal, 9);
        return true;
    }
    if (BLE_ReadMemory(ctrl, 0x0b, factory_addr, cal, sizeof(cal)) >= 9) {
        SDL_memcpy(out9, cal, 9);
        return true;
    }
    return false;
}

static void BLE_ReadCalibration(BLE_Controller *ctrl)
{
    Uint8 slot1[9], slot2[9];

    if (BLE_ReadCalibSlot(ctrl, 0x1FC042, 0x0130A8, slot1)) {
        BLE_ParseStickCalibration(&ctrl->left_x, &ctrl->left_y, slot1);
    }
    // Pro/GameCube have a second stick; a Joy-Con uses only the first slot.
    if (ctrl->product_id != USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT &&
        ctrl->product_id != USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT &&
        BLE_ReadCalibSlot(ctrl, 0x1FC062, 0x0130E8, slot2)) {
        BLE_ParseStickCalibration(&ctrl->right_x, &ctrl->right_y, slot2);
    }
    ctrl->calibrated = true;
}

// Player LED (command 0x09 / subcmd 0x07). The wired UpdateSlotLED
// (SDL_hidapi_switch2.c:327-337) sends 8 data bytes with the pattern in data[0].
static void BLE_SetPlayerLED(BLE_Controller *ctrl, int player_index)
{
    static const Uint8 pattern[] = { 0x1, 0x3, 0x7, 0xf, 0x9, 0x5, 0xd, 0x6 };
    Uint8 data[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (player_index >= 0) {
        data[0] = pattern[player_index % 8];
    }
    BLE_SendCommand(ctrl, 0x09, 0x07, data, sizeof(data), NULL, 0);
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
    Uint8 frame[64];

    if (!ctrl->command_char || data_len < 0 || data_len + 8 > (int)sizeof(frame)) {
        return false;
    }
    frame[0] = cmd;
    frame[1] = 0x91;
    frame[2] = 0x01; // Bluetooth transport
    frame[3] = subcmd;
    frame[4] = 0x00;
    frame[5] = (Uint8)data_len;
    frame[6] = 0x00;
    frame[7] = 0x00;
    if (data_len > 0) {
        SDL_memcpy(&frame[8], data, data_len);
    }
    return SDL_BLEGATT_WriteCharacteristic(ctrl->command_char, frame, 8 + data_len, false);
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
    return SDL_BLEGATT_WriteCharacteristic(ctrl->vibration_char, packet, len, false);
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
static void BLE_OnAdvertisement(const SDL_BLEAdvertisement *advertisement, void *userdata)
{
    int i;
    (void)userdata;

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
                // deliver the open/discovery completions.
                if (supported && BLE_TryReserveConnect(advertisement->address)) {
                    BLE_ConnectRequest *req = (BLE_ConnectRequest *)SDL_malloc(sizeof(*req));
                    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 advertisement matched: VID %04x PID %04x addr %012llx", vendor, product, (unsigned long long)advertisement->address);
                    if (req) {
                        SDL_Thread *thread;
                        req->address = advertisement->address;
                        req->vendor = vendor;
                        req->product = product;
                        thread = SDL_CreateThread(BLE_ConnectThread, "BLESwitch2Connect", req);
                        if (thread) {
                            SDL_DetachThread(thread);
                        } else {
                            SDL_free(req);
                            BLE_ReleaseConnect(advertisement->address);
                        }
                    } else {
                        BLE_ReleaseConnect(advertisement->address);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Connect + GATT discovery + subscribe (called from the connect thread). The
// open, the uncached discovery and the lookups are the shared transport's
// (SDL_ble_gatt.c).
// ---------------------------------------------------------------------------
static void BLE_ConnectAndSubscribe(Uint64 bluetooth_address, Uint16 vendor_id, Uint16 product_id, char *name)
{
    BleDevice *device = NULL;
    BleDevice3 *device3 = NULL;
    GattService3 *service3 = NULL;
    BLE_Controller *ctrl = NULL;

    // Open by raw address with the 20 s ceiling (SDL_BLEGATT_OpenDevice). This
    // runs on the connect thread, so the wait does not starve the WinRT thread
    // pool that has to deliver the completion.
    device = SDL_BLEGATT_OpenDevice(bluetooth_address, NULL);
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 open addr %012llx: device=%p",
                 (unsigned long long)bluetooth_address, (void *)device);
    if (!device) {
        return;
    }

    // Discover the Switch 2 service uncached, up to 10 tries 500 ms apart while
    // the bond-free link comes up (SDL_BLEGATT_FindService).
    if (FAILED(__x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_QueryInterface(device, &IID_BleDevice3, (void **)&device3)) || !device3) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(device);
        return;
    }
    service3 = SDL_BLEGATT_FindService(device3, &GUID_Switch2Service, 1, 10, "Switch2", NULL);
    if (!service3) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 GATT service discovery failed after 10 attempts");
        BLE_NoteConnectFailure(bluetooth_address); // back off; likely busy on USB
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3_Release(device3);
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(device);
        return;
    }

    // Best-effort throughput bump, now that the link is up
    // (SDL_BLEGATT_RequestThroughput).
    SDL_BLEGATT_RequestThroughput(device);

    // Build the controller record.
    ctrl = (BLE_Controller *)SDL_calloc(1, sizeof(*ctrl));
    if (!ctrl) {
        goto cleanup;
    }
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
    ctrl->device = device;
    device = NULL; // ownership transferred to ctrl; cleanup releases via ctrl
    ctrl->instance_id = SDL_GetNextObjectID();
    ctrl->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, vendor_id, product_id, 0, NULL, ctrl->name, 'h', 0);

    ctrl->response_lock = SDL_CreateMutex();
    ctrl->response_sem = SDL_CreateSemaphore(0);
    ctrl->player_index = -1;

    ctrl->input_char = SDL_BLEGATT_FindCharacteristic(service3, &GUID_Switch2Input, NULL);
    ctrl->command_char = SDL_BLEGATT_FindCharacteristic(service3, &GUID_Switch2Command, NULL);
    ctrl->response_char = SDL_BLEGATT_FindCharacteristic(service3, &GUID_Switch2CmdResponse, NULL);
    switch (product_id) {
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT:
        ctrl->vibration_char = SDL_BLEGATT_FindCharacteristic(service3, &GUID_Switch2VibeJCL, NULL);
        break;
    case USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT:
        ctrl->vibration_char = SDL_BLEGATT_FindCharacteristic(service3, &GUID_Switch2VibeJCR, NULL);
        break;
    default:
        ctrl->vibration_char = SDL_BLEGATT_FindCharacteristic(service3,
            (product_id == USB_PRODUCT_NINTENDO_SWITCH2_PRO) ? &GUID_Switch2VibePro : &GUID_Switch2VibeGC, NULL);
        break;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "BLE Switch2 connected: chars input=%d command=%d response=%d vibration=%d",
                 ctrl->input_char != NULL, ctrl->command_char != NULL, ctrl->response_char != NULL, ctrl->vibration_char != NULL);

    // Connect sequence per controller.py connect() (switch2-controllers/
    // controller.py:253-265): enable the command-response notification first, then
    // read calibration over the command channel, and only THEN enable the input
    // report notification.
    if (ctrl->response_char) {
        ctrl->response_handler = SDL_BLEGATT_AddValueHandler(ctrl->response_char, BLE_OnResponseValue, ctrl, 0, &ctrl->response_token);
        if (ctrl->response_handler) {
            SDL_BLEGATT_EnableNotifications(ctrl->response_char);
        }
    }

    // Read stick calibration over the command channel (best-effort).
    BLE_ReadCalibration(ctrl);

    // Joy-Con 2 input-mode write (Format 3 / 0x30) before input notifications start.
    // Without it the default report leaks the high status byte and the Left's
    // bit-23 into the button field as phantom ZL/ZR. windows10-gyro sends this raw
    // 11-byte buffer to the command characteristic for the Joy-Cons (controller.py
    // :758-766). Joy-Cons stay on the unified Report 0x05 layout that
    // BLE_DecodeJoyConLeft/Right read (controller.py:276-289 parses Joy-Cons there
    // even with Format 3 set), so this only cleans the phantom bits. The GameCube is
    // deliberately excluded: Format 3 switches it to the native Report 0x0A layout
    // (controller.py:173-270, buttons at data[2:5], triggers data[12:13], IMU
    // data[34:46]), which this driver's GameCube decoder does not parse. Sending it
    // to a GameCube would break that decode, so GC stays on the unified layout and
    // its Format-3 + native-decoder rework is a separate, hardware-gated task.
    if (ctrl->command_char &&
        (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT ||
         ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT)) {
        static const Uint8 set_input_mode[] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x30 };
        SDL_BLEGATT_WriteCharacteristic(ctrl->command_char, set_input_mode, (int)sizeof(set_input_mode), false);

        /* Opt-in side-channel sensors (Joy-Con only, both L and R). Feature
           flags command 0x0C: subcommand 0x02 (init) then 0x04 (enable), u32 LE
           payload, mouse = bit 4 (controller.py:55-61/370-373, enabled at :268;
           the report 0x05 Mouse Data block is "Activated via feature bit 4",
           hid_reports.md), magnetometer = bit 7 (windows10-gyro
           controller.py:713's FEATSEL 0x94 = motion | mouse | magnetometer).
           Both ride one combined init+enable, the reference's own multi-feature
           pattern. The frames use the command channel convention
           <cmd> 91 01 <subcmd> 00 <len> 00 00 <payload> that BLE_SendCommand
           also builds; fire-and-forget like the Format-3 write above. */
        if (SDL_GetHintBoolean(SDL_HINT_JOYSTICK_BLE_SWITCH2_MOUSE, false)) {
            ctrl->mouse_enabled = true;
        }
        if (SDL_GetHintBoolean(SDL_HINT_JOYSTICK_BLE_SWITCH2_MAGNETOMETER, false)) {
            ctrl->magnetometer_enabled = true;
        }
        if (ctrl->mouse_enabled || ctrl->magnetometer_enabled) {
            Uint8 feature_init[12] = { 0x0C, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            Uint8 feature_enable[12] = { 0x0C, 0x91, 0x01, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            Uint8 feature_mask = (Uint8)((ctrl->mouse_enabled ? 0x10 : 0x00) |
                                         (ctrl->magnetometer_enabled ? 0x80 : 0x00));
            feature_init[8] = feature_mask;
            feature_enable[8] = feature_mask;
            SDL_BLEGATT_WriteCharacteristic(ctrl->command_char, feature_init, (int)sizeof(feature_init), false);
            SDL_BLEGATT_WriteCharacteristic(ctrl->command_char, feature_enable, (int)sizeof(feature_enable), false);
        }
    }

    if (ctrl->input_char) {
        // Register the handler before enabling notifications (controller.py order).
        ctrl->input_handler = SDL_BLEGATT_AddValueHandler(ctrl->input_char, BLE_OnInputValue, ctrl, 0, &ctrl->input_token);
        if (ctrl->input_handler) {
            SDL_BLEGATT_EnableNotifications(ctrl->input_char);
        }
    }

    // Link-loss detection (SDL#9 follow-up): without it a pad that powers off
    // (the SendEffect shutdown, a dead battery, out of range) stays registered
    // forever, and its address stays claimed in ble.controllers so every
    // reconnection advertisement is dropped by BLE_TryReserveConnect. The
    // callback only sets link_lost; BLE_JoystickDetect runs the teardown. If
    // the append below is skipped (duplicate/teardown race), BLE_FreeController
    // removes this subscription on the cleanup path like the others.
    // SDL_BLEGATT_AddStatusHandler polls the status once after registering.
    ctrl->status_handler = SDL_BLEGATT_AddStatusHandler(ctrl->device, &ctrl->link_lost, &ctrl->status_token);

    SDL_LockJoysticks();
    {
        // Bail if a teardown ran while this connect was in WinRT discovery.
        // SDL_QuitJoysticks holds SDL_LockJoysticks across driver->Quit(), and
        // BLE_JoystickQuit clears ble.initialized last under that same lock
        // (SDL_joystick.c:2275-2294). A connect callback that was blocked here
        // during shutdown must not re-grow the freed ble.controllers array or
        // register a joystick after SDL_joysticks_quitting is set. ctrl stays
        // non-NULL and is torn down via the cleanup path below.
        BLE_Controller **grown = ble.initialized
            ? (BLE_Controller **)SDL_realloc(ble.controllers, sizeof(ble.controllers[0]) * (ble.controller_count + 1))
            : NULL;
        // Re-check under the lock: another advertisement callback on a second
        // thread-pool thread may have connected the same device concurrently.
        if (grown) {
            ble.controllers = grown;
            if (!BLE_GetControllerByAddress(bluetooth_address)) {
                int c;
                ble.controllers[ble.controller_count++] = ctrl;
                SDL_PrivateJoystickAdded(ctrl->instance_id);
                ctrl = NULL; // owned by the array now
                // Genuine connect: clear any backoff so a later wireless reconnect
                // is immediate. (Already under SDL_LockJoysticks here.)
                for (c = 0; c < ble.cooldown_count; ++c) {
                    if (ble.cooldowns[c].address == bluetooth_address) {
                        ble.cooldowns[c] = ble.cooldowns[--ble.cooldown_count];
                        break;
                    }
                }
            }
        }
    }
    SDL_UnlockJoysticks();

cleanup:
    if (service3) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3_Release(service3);
    }
    if (device3) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice3_Release(device3);
    }
    if (ctrl) {
        // Failed to publish (calloc failure or a concurrent duplicate connect).
        // Full teardown: unregister the ValueChanged handlers, release the
        // characteristics and device, free the handlers/locks. ctrl->device == device
        // at this point, so this also releases device.
        BLE_FreeController(ctrl);
    } else if (device) {
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(device);
    }
}

// ---------------------------------------------------------------------------
// Input decode. The BLE report is the wired report minus the 1-byte report-ID
// prefix, so the wired HandleSwitchProState layout applies at offset -1. Button
// bytes: u32 LE at [4:8]; sticks at [10:16]; IMU accel [48:54] / gyro [54:60].
// ---------------------------------------------------------------------------
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

    // IMU, transcribed from the wired HandleStatePacket (SDL_hidapi_switch2.c
    // :1379-1392) at BLE offset -1. The axes are NOT sequential: accel maps
    // X<-0x31, Y<-0x35, Z<-0x33(neg); gyro X<-0x37, Y<-0x3b, Z<-0x39(neg). The
    // wired gyro_coeff is 34.8 (its dynamic 34.8-vs-40 calibration is not ported;
    // 34.8 is the nominal case, flagged for hardware confirmation). Biases are 0.
    if (size >= 60 && ctrl->joystick && ctrl->sensors_enabled) {
        const float accel_scale = SDL_STANDARD_GRAVITY * 8.0f / 32767.0f;
        const float gyro_scale = 34.8f / 32767.0f;
        float accel[3], gyro[3];
        accel[0] = (Sint16)(data[48] | (data[49] << 8)) * accel_scale;
        accel[1] = (Sint16)(data[52] | (data[53] << 8)) * accel_scale;
        accel[2] = (Sint16)(data[50] | (data[51] << 8)) * -accel_scale;
        gyro[0] = (Sint16)(data[54] | (data[55] << 8)) * gyro_scale;
        gyro[1] = (Sint16)(data[58] | (data[59] << 8)) * gyro_scale;
        gyro[2] = (Sint16)(data[56] | (data[57] << 8)) * -gyro_scale;
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

/* Switch 2 magnetometer: three int16 LE at report offsets 0x16/0x18/0x1A
   (joycon2cpp README report layout, fork issue #25), streamed when feature
   bit 7 is enabled (windows10-gyro controller.py:713's FEATSEL 0x94).
   Raw samples on three dedicated axes, no fusion: the consumer owns
   orientation math. The axes follow the mouse counters when both are
   enabled, and the raw axis count is the availability contract
   (6 = neither, 8 = mouse, 9 = magnetometer, 11 = both). */
static void BLE_PostMagnetometerAxes(BLE_Controller *ctrl, SDL_Joystick *joystick, const Uint8 *data, int size, Uint64 ts)
{
    int base;
    Sint16 mag[3];
    int i;

    if (!ctrl->magnetometer_enabled || size < 0x1C) {
        return;
    }
    base = SDL_GAMEPAD_AXIS_COUNT + (ctrl->mouse_enabled ? 2 : 0);
    mag[0] = (Sint16)(data[0x16] | (data[0x17] << 8));
    mag[1] = (Sint16)(data[0x18] | (data[0x19] << 8));
    mag[2] = (Sint16)(data[0x1A] | (data[0x1B] << 8));
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
// SDL_JoystickDriver entry points.
// ---------------------------------------------------------------------------
static bool BLE_JoystickInit(void)
{
    if (!SDL_GetHintBoolean(SDL_HINT_JOYSTICK_BLE_SWITCH2, false)) {
        return true; // disabled, but the driver still loads cleanly
    }
    if (ble.initialized) {
        return true;
    }

    // RoInitialize, the combase entry points and the MTA pin are the shared
    // transport's, counted across both BLE drivers.
    if (!SDL_BLEGATT_Init()) {
        return false;
    }
    ble.transport = true;

    // Start the advertisement watcher, or join it when the generic BLE GATT
    // driver started it first.
    if (SDL_BLEGATT_AddListener(BLE_OnAdvertisement, NULL)) {
        ble.scanning = true;
    }

    ble.initialized = true;
    return true;
}

static int BLE_JoystickGetCount(void)
{
    return ble.controller_count;
}

static void BLE_JoystickDetect(void)
{
    // Drain link-loss flags set by ConnectionStatusChanged (MTA callback) and
    // tear the dead controllers down on the joystick thread. Detect runs under
    // SDL_LockJoysticks (SDL_joystick.c:2867-2869, after the per-joystick update
    // walk, the documented place to free removed-device data), the same lock the
    // connect append and Quit hold, so the array walk is safe. Removing the
    // controller posts the removal to the app (fixes the stale registration) and
    // makes BLE_GetControllerByAddress return NULL for the address, so
    // BLE_TryReserveConnect accepts the pad's next advertisement and it can
    // reconnect (SDL#9 follow-up). BLE_FreeController deliberately leaks the
    // callback-touched state, so an in-flight callback on the dead controller
    // stays safe.
    int i;
    for (i = 0; i < ble.controller_count; ) {
        BLE_Controller *ctrl = ble.controllers[i];
        if (SDL_GetAtomicInt(&ctrl->link_lost)) {
            SDL_PrivateJoystickRemoved(ctrl->instance_id);
            ble.controllers[i] = ble.controllers[--ble.controller_count];
            BLE_FreeController(ctrl);
        } else {
            ++i;
        }
    }
}

static bool BLE_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    (void)version;
    (void)name;
    int i;
    for (i = 0; i < ble.controller_count; ++i) {
        if (ble.controllers[i]->vendor_id == vendor_id && ble.controllers[i]->product_id == product_id) {
            return true;
        }
    }
    return false;
}

static const char *BLE_JoystickGetDeviceName(int device_index)
{
    if (device_index >= 0 && device_index < ble.controller_count) {
        return ble.controllers[device_index]->name;
    }
    return NULL;
}

static const char *BLE_JoystickGetDevicePath(int device_index)
{
    (void)device_index;
    return NULL;
}

static int BLE_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    (void)device_index;
    return -1;
}

static int BLE_JoystickGetDevicePlayerIndex(int device_index)
{
    (void)device_index;
    return -1;
}

static void BLE_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
    if (device_index >= 0 && device_index < ble.controller_count) {
        BLE_Controller *ctrl = ble.controllers[device_index];
        ctrl->player_index = player_index;
        BLE_SetPlayerLED(ctrl, player_index);
    }
}

static SDL_GUID BLE_JoystickGetDeviceGUID(int device_index)
{
    SDL_GUID guid;
    if (device_index >= 0 && device_index < ble.controller_count) {
        return ble.controllers[device_index]->guid;
    }
    SDL_zero(guid);
    return guid;
}

static SDL_JoystickID BLE_JoystickGetDeviceInstanceID(int device_index)
{
    if (device_index >= 0 && device_index < ble.controller_count) {
        return ble.controllers[device_index]->instance_id;
    }
    return 0;
}

static bool BLE_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    BLE_Controller *ctrl;
    if (device_index < 0 || device_index >= ble.controller_count) {
        return SDL_SetError("BLE joystick index out of range");
    }
    ctrl = ble.controllers[device_index];
    ctrl->joystick = joystick;

    /* Stable identity across reconnects (SDL#9 follow-up): expose the pad's
       Bluetooth address as the joystick serial, twelve bare lowercase hex
       digits, the same shape hidapi reports for Bluetooth pads. Without it
       every consumer identity falls back to the SDL instance ID, which mints
       a new identity on every power-cycle and orphans per-device settings.
       Core owns and frees the string (SDL_joystick.c:2254), matching the
       hidapi open's SDL_strdup assignment (SDL_hidapijoystick.c:1587). */
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

    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, 250.0f);
    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 250.0f);

    // Advertise the rumble capability so apps that gate on it (PadForge's FFB
    // passthrough reads SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN into HasRumble) will
    // drive it, mirroring SDL_hidapijoystick.c:860. The cap must track actual
    // delivery: the GameCube rumbles over the command channel (BLE_PumpGameCubeRumble),
    // every other type over the vibration characteristic.
    {
        bool can_rumble = (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER)
            ? (ctrl->command_char != NULL)
            : (ctrl->vibration_char != NULL);
        if (can_rumble) {
            SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick),
                                   SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, true);
        }
    }

    // Light the player LED for this slot.
    ctrl->player_index = SDL_GetJoystickPlayerIndex(joystick);
    BLE_SetPlayerLED(ctrl, ctrl->player_index);
    return true;
}

static bool BLE_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    bool is_gc;
    if (!ctrl) {
        return SDL_Unsupported();
    }
    is_gc = (ctrl->product_id == USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER);
    if (is_gc ? !ctrl->command_char : !ctrl->vibration_char) {
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
    if (is_gc) {
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

   The frame layout matches BLE_SendCommand (:794-801). The write is
   fire-and-forget rather than routed through BLE_SendCommand because that
   helper always waits up to 500 ms on the response semaphore and returns the
   reply byte count. A shutdown powers the pad off instead of replying
   (commands.md documents an empty response), so the helper would stall and
   then read a successful shutdown as a failure. Success here means the write
   was delivered. A stale reply from a command that does respond is drained by
   BLE_SendCommand before its next send (:812-814). */
static bool BLE_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    const Uint8 *bytes = (const Uint8 *)data;
    Uint8 frame[64];
    int data_len;

    if (!ctrl || !ctrl->command_char) {
        return SDL_SetError("No BLE command channel for joystick");
    }
    if (size < 2) {
        return SDL_SetError("Switch 2 effect needs at least [cmd, subcmd]");
    }
    data_len = size - 2;
    if (data_len + 8 > (int)sizeof(frame)) {
        return SDL_SetError("Switch 2 effect payload too large");
    }
    frame[0] = bytes[0];
    frame[1] = 0x91;
    frame[2] = 0x01; // Bluetooth transport
    frame[3] = bytes[1];
    frame[4] = 0x00;
    frame[5] = (Uint8)data_len;
    frame[6] = 0x00;
    frame[7] = 0x00;
    if (data_len > 0) {
        SDL_memcpy(&frame[8], bytes + 2, data_len);
    }
    if (!SDL_BLEGATT_WriteCharacteristic(ctrl->command_char, frame, 8 + data_len, true)) {
        return SDL_SetError("Switch 2 command write failed");
    }
    return true;
}

static bool BLE_JoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    // Enable/disable the IMU via the BLE reference's enableFeatures
    // (controller.py:370-373): command 0x0c/0x02 (init) then 0x0c/0x04 (enable),
    // both carrying the feature mask. FEATURE_MOTION = 0x04 (controller.py:59).
    // The wired driver's 0x27 is a USB-path value and sets unrelated bits here.
    // The mask is the union of every feature this driver keeps active: the
    // proven pattern for multiple features is one combined init+enable
    // (windows10-gyro controller.py:802 sends MOTION | MOUSE | MAGNETOMETER in
    // a single call), so a motion-only toggle must not drop the mouse or
    // magnetometer bits.
    Uint8 flags[4] = { 0x00, 0x00, 0x00, 0x00 };
    if (!ctrl) {
        return SDL_SetError("No BLE controller for joystick");
    }
    flags[0] = (Uint8)((enabled ? 0x04 : 0x00) |
                       (ctrl->mouse_enabled ? 0x10 : 0x00) |
                       (ctrl->magnetometer_enabled ? 0x80 : 0x00));
    ctrl->sensors_enabled = enabled;
    BLE_SendCommand(ctrl, 0x0c, 0x02, flags, sizeof(flags), NULL, 0);
    BLE_SendCommand(ctrl, 0x0c, 0x04, flags, sizeof(flags), NULL, 0);
    return true;
}

static void BLE_JoystickUpdate(SDL_Joystick *joystick)
{
    BLE_Controller *ctrl = BLE_GetControllerByInstance(joystick->instance_id);
    Uint8 data[64];
    int size = 0;

    if (!ctrl) {
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
            BLE_LogBytes("first input report", data, size); // confirms the byte offsets
        }
        BLE_DecodeReport(ctrl, joystick, data, size);
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

static void BLE_FreeController(BLE_Controller *ctrl)
{
    // Stop new notifications and release the WinRT COM objects. An in-flight
    // ValueChanged callback uses only its event args and the controller's own
    // buffers, not these objects, so releasing them here is safe. (WinRT holds
    // its own reference to the characteristic for the duration of a callback.)
    if (ctrl->input_char) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_remove_ValueChanged(ctrl->input_char, ctrl->input_token);
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(ctrl->input_char);
    }
    if (ctrl->command_char) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(ctrl->command_char);
    }
    if (ctrl->response_char) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_remove_ValueChanged(ctrl->response_char, ctrl->response_token);
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(ctrl->response_char);
    }
    if (ctrl->vibration_char) {
        __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic_Release(ctrl->vibration_char);
    }
    if (ctrl->device) {
        if (ctrl->status_handler) {
            __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_remove_ConnectionStatusChanged(ctrl->device, ctrl->status_token);
        }
        __x_ABI_CWindows_CDevices_CBluetooth_CIBluetoothLEDevice_Release(ctrl->device);
    }

    // Deliberately leak the controller struct, its locks/semaphore, buffers, and
    // the delegates. A GATT ValueChanged callback may still be in-flight on a
    // WinRT thread-pool thread and touches exactly these, and GATT
    // remove_ValueChanged does not drain in-flight invocations. Destroying the
    // mutex or freeing the struct here would be a use-after-free. The leak is
    // bounded (one controller per connection attempt, reclaimed at process exit);
    // a future refcounted-controller design could free it deterministically.
    (void)ctrl;
}

static void BLE_JoystickQuit(void)
{
    int i;

    // The watcher stops when the last listener goes
    if (ble.scanning) {
        SDL_BLEGATT_RemoveListener(BLE_OnAdvertisement, NULL);
    }
    for (i = 0; i < ble.controller_count; ++i) {
        BLE_FreeController(ble.controllers[i]);
    }
    SDL_free(ble.controllers);
    ble.controllers = NULL;
    ble.controller_count = 0;

    if (ble.transport) {
        SDL_BLEGATT_Quit();
        ble.transport = false;
    }
    ble.scanning = false;
    ble.initialized = false;
}

static bool BLE_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    (void)device_index;
    (void)out;
    return false; // resolved by the 'h'+VID/PID fabricated mapping
}

SDL_JoystickDriver SDL_BLE_JoystickDriver = {
    BLE_JoystickInit,
    BLE_JoystickGetCount,
    BLE_JoystickDetect,
    BLE_JoystickIsDevicePresent,
    BLE_JoystickGetDeviceName,
    BLE_JoystickGetDevicePath,
    BLE_JoystickGetDeviceSteamVirtualGamepadSlot,
    BLE_JoystickGetDevicePlayerIndex,
    BLE_JoystickSetDevicePlayerIndex,
    BLE_JoystickGetDeviceGUID,
    BLE_JoystickGetDeviceInstanceID,
    BLE_JoystickOpen,
    BLE_JoystickRumble,
    BLE_JoystickRumbleTriggers,
    BLE_JoystickSetLED,
    BLE_JoystickSendEffect,
    BLE_JoystickSetSensorsEnabled,
    BLE_JoystickUpdate,
    BLE_JoystickClose,
    BLE_JoystickQuit,
    BLE_JoystickGetGamepadMapping
};

#endif // SDL_JOYSTICK_BLE
