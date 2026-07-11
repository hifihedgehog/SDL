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
#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_HIDAPI

#include "../../SDL_hints_c.h"
#include "../SDL_sysjoystick.h"
#include "SDL_hidapijoystick_c.h"
#include "SDL_hidapi_rumble.h"
#include "SDL_hidapi_nintendo.h"

#ifdef SDL_JOYSTICK_HIDAPI_WII

// Define this if you want to log all packets from the controller
// #define DEBUG_WII_PROTOCOL

#define ENABLE_CONTINUOUS_REPORTING true

#define INPUT_WAIT_TIMEOUT_MS      (3 * 1000)
#define MOTION_PLUS_UPDATE_TIME_MS (8 * 1000)
/* A Motion Plus takes ~20 ms to activate or deactivate, during which the
   extension port is completely unresponsive and the detect pin pulses status
   reports (Dolphin MotionPlus.cpp:355, :367, :216-219). Dolphin's real-Wiimote
   backend waits 2 s after any M+ mode write before trusting identify results
   (WiimoteController.cpp:1476-1480). */
#define WII_MOTIONPLUS_SETTLE_TIME_MS 2000
#define STATUS_UPDATE_TIME_MS      (15 * 60 * 1000)

#define WII_EXTENSION_NONE            0x2E2E
#define WII_EXTENSION_UNINITIALIZED   0xFFFF
#define WII_EXTENSION_NUNCHUK         0x0000
#define WII_EXTENSION_GAMEPAD         0x0101
#define WII_EXTENSION_WIIUPRO         0x0120
#define WII_EXTENSION_BALANCEBOARD    0x0402
#define WII_EXTENSION_MOTIONPLUS_MASK 0xF0FF
#define WII_EXTENSION_MOTIONPLUS_ID   0x0005

#define WII_MOTIONPLUS_MODE_NONE     0x00
#define WII_MOTIONPLUS_MODE_STANDARD 0x04
#define WII_MOTIONPLUS_MODE_NUNCHUK  0x05
#define WII_MOTIONPLUS_MODE_GAMEPAD  0x07

/* IR camera registers (WiimoteLib-Trihy Wiimote.cs:57-60). The 0x04 register-space
   selector is carried in output-report byte1, so the low 24 bits are passed to
   WriteRegister. */
#define WII_IR_REGISTER_CONTROL      0xB00030 // 0x04b00030
#define WII_IR_REGISTER_SENSITIVITY1 0xB00000 // 0x04b00000
#define WII_IR_REGISTER_SENSITIVITY2 0xB0001A // 0x04b0001a
#define WII_IR_REGISTER_MODE         0xB00033 // 0x04b00033
#define WII_IR_MODE_EXTENDED         0x03     // 4 dots, 3 bytes/dot (12 IR bytes, report 0x33, bare remote)
#define WII_IR_MODE_BASIC            0x01     // 4 dots in two 5-byte groups (10 IR bytes, report 0x37, + extension)

/* The two IR dots are surfaced on dedicated joystick axes beyond the six standard
   gamepad axes, so the pointer never collides with an extension's stick axes (0-3)
   and the consumer reads IR from one stable location on every Wii Remote. */
#define WII_IR_AXIS_DOT0_X           (SDL_GAMEPAD_AXIS_COUNT + 0)
#define WII_IR_AXIS_DOT0_Y           (SDL_GAMEPAD_AXIS_COUNT + 1)
#define WII_IR_AXIS_DOT1_X           (SDL_GAMEPAD_AXIS_COUNT + 2)
#define WII_IR_AXIS_DOT1_Y           (SDL_GAMEPAD_AXIS_COUNT + 3)
#define WII_IR_AXIS_COUNT            4

/* Balance Board calibration register (WiimoteLib-Trihy Wiimote.cs:65, 0x04a40020).
   The Kg0/Kg17/Kg34 rows start 4 bytes in, at 0x04a40024. */
#define WII_BALANCE_CALIBRATION_ADDR 0xA40024

typedef enum
{
    k_eWiiInputReportIDs_Status = 0x20,
    k_eWiiInputReportIDs_ReadMemory = 0x21,
    k_eWiiInputReportIDs_Acknowledge = 0x22,
    k_eWiiInputReportIDs_ButtonData0 = 0x30,
    k_eWiiInputReportIDs_ButtonData1 = 0x31,
    k_eWiiInputReportIDs_ButtonData2 = 0x32,
    k_eWiiInputReportIDs_ButtonData3 = 0x33,
    k_eWiiInputReportIDs_ButtonData4 = 0x34,
    k_eWiiInputReportIDs_ButtonData5 = 0x35,
    k_eWiiInputReportIDs_ButtonData6 = 0x36,
    k_eWiiInputReportIDs_ButtonData7 = 0x37,
    k_eWiiInputReportIDs_ButtonDataD = 0x3D,
    k_eWiiInputReportIDs_ButtonDataE = 0x3E,
    k_eWiiInputReportIDs_ButtonDataF = 0x3F,
} EWiiInputReportIDs;

typedef enum
{
    k_eWiiOutputReportIDs_Rumble = 0x10,
    k_eWiiOutputReportIDs_LEDs = 0x11,
    k_eWiiOutputReportIDs_DataReportingMode = 0x12,
    k_eWiiOutputReportIDs_IRCameraEnable = 0x13,
    k_eWiiOutputReportIDs_SpeakerEnable = 0x14,
    k_eWiiOutputReportIDs_StatusRequest = 0x15,
    k_eWiiOutputReportIDs_WriteMemory = 0x16,
    k_eWiiOutputReportIDs_ReadMemory = 0x17,
    k_eWiiOutputReportIDs_SpeakerData = 0x18,
    k_eWiiOutputReportIDs_SpeakerMute = 0x19,
    k_eWiiOutputReportIDs_IRCameraEnable2 = 0x1a,
} EWiiOutputReportIDs;

typedef enum
{
    k_eWiiPlayerLEDs_P1 = 0x10,
    k_eWiiPlayerLEDs_P2 = 0x20,
    k_eWiiPlayerLEDs_P3 = 0x40,
    k_eWiiPlayerLEDs_P4 = 0x80,
} EWiiPlayerLEDs;

typedef enum
{
    k_eWiiCommunicationState_None,                  // No special communications happening
    k_eWiiCommunicationState_CheckMotionPlusStage1, // Sent standard extension identify request
    k_eWiiCommunicationState_CheckMotionPlusStage2, // Sent Motion Plus extension identify request
} EWiiCommunicationState;

typedef enum
{
    k_eWiiButtons_A = SDL_GAMEPAD_BUTTON_MISC1,
    k_eWiiButtons_B,
    k_eWiiButtons_One,
    k_eWiiButtons_Two,
    k_eWiiButtons_Plus,
    k_eWiiButtons_Minus,
    k_eWiiButtons_Home,
    k_eWiiButtons_DPad_Up,
    k_eWiiButtons_DPad_Down,
    k_eWiiButtons_DPad_Left,
    k_eWiiButtons_DPad_Right,
    k_eWiiButtons_Max
} EWiiButtons;

#define k_unWiiPacketDataLength 22

typedef struct
{
    Uint8 rgucBaseButtons[2];
    Uint8 rgucAccelerometer[3];
    Uint8 rgucExtension[21];
    bool hasBaseButtons;
    bool hasAccelerometer;
    Uint8 ucNExtensionBytes;
} WiiButtonData;

typedef struct
{
    Uint16 min;
    Uint16 max;
    Uint16 center;
    Uint16 deadzone;
} StickCalibrationData;

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_Joystick *joystick;
    Uint64 timestamp;
    EWiiCommunicationState m_eCommState;
    EWiiExtensionControllerType m_eExtensionControllerType;
    bool m_bPlayerLights;
    int m_nPlayerIndex;
    bool m_bRumbleActive;
    bool m_bMotionPlusPresent;
    Uint8 m_ucMotionPlusMode;
    bool m_bMotionPlusChildConnected;    // live passthrough-connected flag (ext[4] & 0x01) from the M+ data frames
    Uint64 m_ulMotionPlusSettleDeadline; // ticks until which M+ identify results are unreliable after a mode write
    bool m_bReportSensors;
    bool m_bIRActive;                          // IR camera enabled (bare Wii Remote, sensors on)
    bool m_bBalanceBoardCalibrationValid;
    Uint8 m_rgucBalanceBoardCalibration[24];   // Kg0/Kg17/Kg34 x 4 corners x int16, big-endian
    Uint8 m_rgucReadBuffer[k_unWiiPacketDataLength];
    Uint64 m_ulLastInput;
    Uint64 m_ulLastStatus;
    Uint64 m_ulNextMotionPlusCheck;
    bool m_bDisconnected;

    StickCalibrationData m_StickCalibrationData[6];
} SDL_DriverWii_Context;

static void HIDAPI_DriverWii_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_WII, callback, userdata);
}

static void HIDAPI_DriverWii_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_WII, callback, userdata);
}

static bool HIDAPI_DriverWii_IsEnabled(void)
{
#if 1 // This doesn't work with the dolphinbar, so don't enable by default right now
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_WII, false);
#else
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_WII,
                              SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI,
                                                 SDL_HIDAPI_DEFAULT));
#endif
}

static bool HIDAPI_DriverWii_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    if (vendor_id == USB_VENDOR_NINTENDO &&
        (product_id == USB_PRODUCT_NINTENDO_WII_REMOTE ||
         product_id == USB_PRODUCT_NINTENDO_WII_REMOTE2)) {
        return true;
    }
    return false;
}

static int ReadInput(SDL_DriverWii_Context *ctx)
{
    int size;

    // Make sure we don't try to read at the same time a write is happening
    if (SDL_GetAtomicInt(&ctx->device->rumble_pending) > 0) {
        return 0;
    }

    size = SDL_hid_read_timeout(ctx->device->dev, ctx->m_rgucReadBuffer, sizeof(ctx->m_rgucReadBuffer), 0);
#ifdef DEBUG_WII_PROTOCOL
    if (size > 0) {
        HIDAPI_DumpPacket("Wii packet: size = %d", ctx->m_rgucReadBuffer, size);
    }
#endif
    return size;
}

static bool WriteOutput(SDL_DriverWii_Context *ctx, const Uint8 *data, int size, bool sync)
{
#ifdef DEBUG_WII_PROTOCOL
    if (size > 0) {
        HIDAPI_DumpPacket("Wii write packet: size = %d", data, size);
    }
#endif
    if (sync) {
        return SDL_hid_write(ctx->device->dev, data, size) >= 0;
    } else {
        // Use the rumble thread for general asynchronous writes
        if (!SDL_HIDAPI_LockRumble()) {
            return false;
        }
        return SDL_HIDAPI_SendRumbleAndUnlock(ctx->device, data, size) >= 0;
    }
}

static bool ReadInputSync(SDL_DriverWii_Context *ctx, EWiiInputReportIDs expectedID, bool (*isMine)(const Uint8 *))
{
    Uint64 endTicks = SDL_GetTicks() + 250; // Seeing successful reads after about 200 ms

    int nRead = 0;
    while ((nRead = ReadInput(ctx)) != -1) {
        if (nRead > 0) {
            if (ctx->m_rgucReadBuffer[0] == expectedID && (!isMine || isMine(ctx->m_rgucReadBuffer))) {
                return true;
            }
        } else {
            if (SDL_GetTicks() >= endTicks) {
                break;
            }
            SDL_Delay(1);
        }
    }
    SDL_SetError("Read timed out");
    return false;
}

static bool IsWriteMemoryResponse(const Uint8 *data)
{
    return data[3] == k_eWiiOutputReportIDs_WriteMemory;
}

static bool WriteRegister(SDL_DriverWii_Context *ctx, Uint32 address, const Uint8 *data, int size, bool sync)
{
    Uint8 writeRequest[k_unWiiPacketDataLength];

    SDL_zeroa(writeRequest);
    writeRequest[0] = k_eWiiOutputReportIDs_WriteMemory;
    writeRequest[1] = (Uint8)(0x04 | (Uint8)ctx->m_bRumbleActive);
    writeRequest[2] = (address >> 16) & 0xff;
    writeRequest[3] = (address >> 8) & 0xff;
    writeRequest[4] = address & 0xff;
    writeRequest[5] = (Uint8)size;
    SDL_assert(size > 0 && size <= 16);
    SDL_memcpy(writeRequest + 6, data, size);

    if (!WriteOutput(ctx, writeRequest, sizeof(writeRequest), sync)) {
        return false;
    }
    if (sync) {
        // Wait for response
        if (!ReadInputSync(ctx, k_eWiiInputReportIDs_Acknowledge, IsWriteMemoryResponse)) {
            return false;
        }
        if (ctx->m_rgucReadBuffer[4]) {
            SDL_SetError("Write memory failed: %u", ctx->m_rgucReadBuffer[4]);
            return false;
        }
    }
    return true;
}

static bool ReadRegister(SDL_DriverWii_Context *ctx, Uint32 address, int size, bool sync)
{
    Uint8 readRequest[7];

    readRequest[0] = k_eWiiOutputReportIDs_ReadMemory;
    readRequest[1] = (Uint8)(0x04 | (Uint8)ctx->m_bRumbleActive);
    readRequest[2] = (address >> 16) & 0xff;
    readRequest[3] = (address >> 8) & 0xff;
    readRequest[4] = address & 0xff;
    readRequest[5] = (size >> 8) & 0xff;
    readRequest[6] = size & 0xff;

    SDL_assert(size > 0 && size <= 0xffff);

    if (!WriteOutput(ctx, readRequest, sizeof(readRequest), sync)) {
        return false;
    }
    if (sync) {
        SDL_assert(size <= 16); // Only waiting for one packet is supported right now
        // Wait for response
        if (!ReadInputSync(ctx, k_eWiiInputReportIDs_ReadMemory, NULL)) {
            return false;
        }
    }
    return true;
}

static bool SendExtensionIdentify(SDL_DriverWii_Context *ctx, bool sync)
{
    return ReadRegister(ctx, 0xA400FE, 2, sync);
}

/* An ACTIVE Motion Plus blocks i2c passthrough entirely (Dolphin
   MotionPlus.cpp:204-210), so the child behind it is identifiable only from
   the M+'s stored copy of the child ID: 0xF6 holds ID byte 4, 0xF7 the
   challenge state, 0xF8 ID byte 0, 0xF9 ID byte 5 (Dolphin MotionPlus.h
   register map). Dolphin's real-Wiimote backend reads these four bytes at the
   active address and maps the child from bytes (0xF8, 0xF6, 0xF9)
   (WiimoteController.cpp:541-564). Our 16-bit extension IDs are full-ID bytes
   4-5, so the stored pair is (data[0] << 8) | data[3]. */
static bool ReadStoredChildExtensionID(SDL_DriverWii_Context *ctx, Uint16 *extension)
{
    if (!ReadRegister(ctx, 0xA400F6, 4, true)) {
        return false;
    }
    if (ctx->m_rgucReadBuffer[0] != k_eWiiInputReportIDs_ReadMemory) {
        return false;
    }
    if (ctx->m_rgucReadBuffer[4] != 0x00 || ctx->m_rgucReadBuffer[5] != 0xF6) {
        return false;
    }
    if (ctx->m_rgucReadBuffer[3] != 0x30) {
        // Read error or short read: the register is unreadable right now
        return false;
    }
    *extension = (Uint16)((ctx->m_rgucReadBuffer[6] << 8) | ctx->m_rgucReadBuffer[9]);
    return true;
}

static bool ParseExtensionIdentifyResponse(SDL_DriverWii_Context *ctx, Uint16 *extension)
{
    int i;

    if (ctx->m_rgucReadBuffer[0] != k_eWiiInputReportIDs_ReadMemory) {
        SDL_SetError("Unexpected extension response type");
        return false;
    }

    if (ctx->m_rgucReadBuffer[4] != 0x00 || ctx->m_rgucReadBuffer[5] != 0xFE) {
        SDL_SetError("Unexpected extension response address");
        return false;
    }

    if (ctx->m_rgucReadBuffer[3] != 0x10) {
        Uint8 error = (ctx->m_rgucReadBuffer[3] & 0xF);

        if (error == 7) {
            // The extension memory isn't mapped
            *extension = WII_EXTENSION_NONE;
            return true;
        }

        if (error) {
            SDL_SetError("Failed to read extension type: %u", error);
        } else {
            SDL_SetError("Unexpected read length when reading extension type: %d", (ctx->m_rgucReadBuffer[3] >> 4) + 1);
        }
        return false;
    }

    *extension = 0;
    for (i = 6; i < 8; i++) {
        *extension = *extension << 8 | ctx->m_rgucReadBuffer[i];
    }
    return true;
}

static EWiiExtensionControllerType GetExtensionType(Uint16 extension_id)
{
    switch (extension_id) {
    case WII_EXTENSION_NONE:
        return k_eWiiExtensionControllerType_None;
    case WII_EXTENSION_NUNCHUK:
        return k_eWiiExtensionControllerType_Nunchuk;
    case WII_EXTENSION_GAMEPAD:
        return k_eWiiExtensionControllerType_Gamepad;
    case WII_EXTENSION_WIIUPRO:
        return k_eWiiExtensionControllerType_WiiUPro;
    case WII_EXTENSION_BALANCEBOARD:
        return k_eWiiExtensionControllerType_BalanceBoard;
    default:
        return k_eWiiExtensionControllerType_Unknown;
    }
}

static bool SendExtensionReset(SDL_DriverWii_Context *ctx, bool sync)
{
    bool result = true;
    {
        Uint8 data = 0x55;
        result = result && WriteRegister(ctx, 0xA400F0, &data, sizeof(data), sync);
    }
    // This write will fail if there is no extension connected, that's fine
    {
        Uint8 data = 0x00;
        (void)WriteRegister(ctx, 0xA400FB, &data, sizeof(data), sync);
    }
    return result;
}

/* Enable the IR camera, transcribed from WiimoteLib-Trihy EnableIR (Wiimote.cs,
   registers at :57-60). The sequence powers the camera (reports 0x13/0x1a), writes
   max-sensitivity blocks, and selects the data mode. A bare remote uses Extended
   mode (report 0x33); a remote with an extension uses Basic mode (report 0x37,
   which carries IR and the extension together, WiimoteLib IRExtensionAccel +
   EnableIR(Basic), Wiimote.cs:386-390/1034). Output-report byte1 must echo the
   rumble bit, which the driver also drives. The matching report mode is selected
   afterward via ResetButtonPacketType once m_bIRActive is set. */
static void EnableIR(SDL_DriverWii_Context *ctx)
{
    static const Uint8 sensitivity1[9] = { 0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x90, 0x00, 0x41 };
    static const Uint8 sensitivity2[2] = { 0x40, 0x00 };
    Uint8 data[2];
    Uint8 value;
    bool ok = true;

    // 1. IR camera enable (report 0x13), 2. IR logic enable (report 0x1a)
    data[0] = k_eWiiOutputReportIDs_IRCameraEnable;
    data[1] = (Uint8)(0x04 | (Uint8)ctx->m_bRumbleActive);
    WriteOutput(ctx, data, sizeof(data), false);

    data[0] = k_eWiiOutputReportIDs_IRCameraEnable2;
    data[1] = (Uint8)(0x04 | (Uint8)ctx->m_bRumbleActive);
    WriteOutput(ctx, data, sizeof(data), false);

    // 3. control register <- 0x08
    value = 0x08;
    ok = WriteRegister(ctx, WII_IR_REGISTER_CONTROL, &value, sizeof(value), true) && ok;

    // 4. sensitivity blocks
    ok = WriteRegister(ctx, WII_IR_REGISTER_SENSITIVITY1, sensitivity1, sizeof(sensitivity1), true) && ok;
    ok = WriteRegister(ctx, WII_IR_REGISTER_SENSITIVITY2, sensitivity2, sizeof(sensitivity2), true) && ok;

    // 5. data mode: Extended only for a bare remote without Motion Plus (report
    // 0x33, 12 IR bytes). With an extension OR an active Motion Plus the report
    // must be 0x37, whose span carries 10-byte Basic IR (Dolphin's real-Wiimote
    // backend uses Basic for the same reason, WiimoteController.cpp:991: "it's
    // all that fits"). 6. control <- 0x08
    value = (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_None &&
             !ctx->m_bMotionPlusPresent) ? WII_IR_MODE_EXTENDED : WII_IR_MODE_BASIC;
    ok = WriteRegister(ctx, WII_IR_REGISTER_MODE, &value, sizeof(value), true) && ok;
    value = 0x08;
    ok = WriteRegister(ctx, WII_IR_REGISTER_CONTROL, &value, sizeof(value), true) && ok;

    /* Only claim IR is active if every configuration write landed. If a sync write
       timed out, the camera is half-configured, so leaving m_bIRActive false keeps
       GetButtonPacketType from selecting report 0x33 against it; the next sensor
       toggle (or reopen) retries the full sequence. */
    ctx->m_bIRActive = ok;
}

static void DisableIR(SDL_DriverWii_Context *ctx)
{
    Uint8 data[2];

    data[0] = k_eWiiOutputReportIDs_IRCameraEnable;
    data[1] = (Uint8)(0x00 | (Uint8)ctx->m_bRumbleActive);
    WriteOutput(ctx, data, sizeof(data), false);

    data[0] = k_eWiiOutputReportIDs_IRCameraEnable2;
    data[1] = (Uint8)(0x00 | (Uint8)ctx->m_bRumbleActive);
    WriteOutput(ctx, data, sizeof(data), false);

    ctx->m_bIRActive = false;
}

/* Read the 24-byte Balance Board calibration (Kg0/Kg17/Kg34 x 4 corners x int16
   big-endian) from register 0x04a40024 (WiimoteLib-Trihy Wiimote.cs:65,531-547).
   ReadRegister waits on one ReadMemory packet of <= 16 bytes, so the 24 bytes are
   read in two passes (16 + 8). Stored raw; PadForge does its own kg interpolation. */
static bool ReadCalibrationBlock(SDL_DriverWii_Context *ctx, Uint32 address, int size, Uint8 *dest)
{
    if (!ReadRegister(ctx, address, size, true)) {
        return false;
    }
    /* Accept only a clean ReadMemory reply: type 0x21, the low nibble of byte 3
       (the error code) clear, and the echoed address (bytes 4-5) matching the low
       16 bits of the request. This rejects ReadMemory error packets and a stale
       reply from an earlier request, which ReadInputSync would otherwise hand back
       (it matches on report id only), so garbage is never published as calibration. */
    if (ctx->m_rgucReadBuffer[0] != k_eWiiInputReportIDs_ReadMemory ||
        (ctx->m_rgucReadBuffer[3] & 0x0F) != 0 ||
        (Uint16)((ctx->m_rgucReadBuffer[4] << 8) | ctx->m_rgucReadBuffer[5]) != (Uint16)(address & 0xFFFF)) {
        return false;
    }
    SDL_memcpy(dest, ctx->m_rgucReadBuffer + 6, size);
    return true;
}

static void ReadBalanceBoardCalibration(SDL_DriverWii_Context *ctx)
{
    ctx->m_bBalanceBoardCalibrationValid =
        ReadCalibrationBlock(ctx, WII_BALANCE_CALIBRATION_ADDR, 16, ctx->m_rgucBalanceBoardCalibration) &&
        ReadCalibrationBlock(ctx, WII_BALANCE_CALIBRATION_ADDR + 16, 8, ctx->m_rgucBalanceBoardCalibration + 16);
}

static bool GetMotionPlusState(SDL_DriverWii_Context *ctx, bool *connected, Uint8 *mode)
{
    Uint16 extension;

    if (connected) {
        *connected = false;
    }
    if (mode) {
        *mode = 0;
    }

    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_WiiUPro) {
        // The Wii U Pro controller never has the Motion Plus extension
        return true;
    }

    if (SendExtensionIdentify(ctx, true) &&
        ParseExtensionIdentifyResponse(ctx, &extension)) {
        if ((extension & WII_EXTENSION_MOTIONPLUS_MASK) == WII_EXTENSION_MOTIONPLUS_ID) {
            // Motion Plus is currently active
            if (connected) {
                *connected = true;
            }
            if (mode) {
                *mode = (extension >> 8);
            }
            return true;
        }
    }

    if (ReadRegister(ctx, 0xA600FE, 2, true) &&
        ParseExtensionIdentifyResponse(ctx, &extension)) {
        if ((extension & WII_EXTENSION_MOTIONPLUS_MASK) == WII_EXTENSION_MOTIONPLUS_ID) {
            // Motion Plus is currently connected
            if (connected) {
                *connected = true;
            }
        }
        return true;
    }

    // Failed to read the register or parse the response
    return false;
}

static bool NeedsPeriodicMotionPlusCheck(SDL_DriverWii_Context *ctx, bool status_update)
{
    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_WiiUPro) {
        // The Wii U Pro controller never has the Motion Plus extension
        return false;
    }

    if (ctx->m_ucMotionPlusMode != WII_MOTIONPLUS_MODE_NONE && !status_update) {
        // We'll get a status update when Motion Plus is disconnected
        return false;
    }

    return true;
}

static void SchedulePeriodicMotionPlusCheck(SDL_DriverWii_Context *ctx)
{
    ctx->m_ulNextMotionPlusCheck = SDL_GetTicks() + MOTION_PLUS_UPDATE_TIME_MS;
}

static void CheckMotionPlusConnection(SDL_DriverWii_Context *ctx)
{
    SendExtensionIdentify(ctx, false);

    ctx->m_eCommState = k_eWiiCommunicationState_CheckMotionPlusStage1;
}

static void ActivateMotionPlusWithMode(SDL_DriverWii_Context *ctx, Uint8 mode)
{
#ifdef SDL_PLATFORM_LINUX
    /* Linux drivers maintain a lot of state around the Motion Plus
     * extension, so don't mess with it here.
     */
#else
    /* Already running in the desired mode: rewriting it would only restart
       the M+'s ~20 ms reset for nothing. Dolphin changes the mode only when
       current differs from desired (WiimoteController.cpp:533-535). */
    if (ctx->m_ucMotionPlusMode == mode) {
        return;
    }

    /* An ACTIVE M+ answers only at the active address and ignores writes to
       the inactive one, so a live mode switch must target 0xA4xxxx; Dolphin
       picks the address by activation state (WiimoteController.cpp:1056-1060).
       Every mode write starts the M+'s ~20 ms reset, so stamp the settle
       window during which identify results are unreliable. */
    Uint32 address = (ctx->m_ucMotionPlusMode != WII_MOTIONPLUS_MODE_NONE) ? 0xA400FE : 0xA600FE;
    WriteRegister(ctx, address, &mode, sizeof(mode), true);

    ctx->m_ucMotionPlusMode = mode;
    ctx->m_ulMotionPlusSettleDeadline = SDL_GetTicks() + WII_MOTIONPLUS_SETTLE_TIME_MS;
#endif // LINUX
}

static void ActivateMotionPlus(SDL_DriverWii_Context *ctx)
{
    Uint8 mode = WII_MOTIONPLUS_MODE_STANDARD;

    // Pick the pass-through mode based on the connected controller
    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Nunchuk) {
        mode = WII_MOTIONPLUS_MODE_NUNCHUK;
    } else if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Gamepad) {
        mode = WII_MOTIONPLUS_MODE_GAMEPAD;
    }
    ActivateMotionPlusWithMode(ctx, mode);
}

static void DeactivateMotionPlus(SDL_DriverWii_Context *ctx)
{
    Uint8 data = 0x55;
    WriteRegister(ctx, 0xA400F0, &data, sizeof(data), true);

    // Wait for the deactivation status message
    ReadInputSync(ctx, k_eWiiInputReportIDs_Status, NULL);

    ctx->m_ucMotionPlusMode = WII_MOTIONPLUS_MODE_NONE;
    ctx->m_ulMotionPlusSettleDeadline = SDL_GetTicks() + WII_MOTIONPLUS_SETTLE_TIME_MS;
}

static void UpdatePowerLevelWii(SDL_Joystick *joystick, Uint8 batteryLevelByte)
{
    int percent;
    if (batteryLevelByte > 178) {
        percent = 100;
    } else if (batteryLevelByte > 51) {
        percent = 70;
    } else if (batteryLevelByte > 13) {
        percent = 20;
    } else {
        percent = 5;
    }
    SDL_SendJoystickPowerInfo(joystick, SDL_POWERSTATE_ON_BATTERY, percent);
}

static void UpdatePowerLevelWiiU(SDL_Joystick *joystick, Uint8 extensionBatteryByte)
{
    bool charging = !(extensionBatteryByte & 0x08);
    bool pluggedIn = !(extensionBatteryByte & 0x04);
    Uint8 batteryLevel = extensionBatteryByte >> 4;

    SDL_AssertJoysticksLocked();

    if (pluggedIn) {
        joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRED;
    } else {
        joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
    }

    /* Not sure if all Wii U Pro controllers act like this, but on mine
     * 4, 3, and 2 are held for about 20 hours each
     * 1 is held for about 6 hours
     * 0 is held for about 2 hours
     * No value above 4 has been observed.
     */
    SDL_PowerState state;
    int percent;
    if (charging) {
        state = SDL_POWERSTATE_CHARGING;
    } else if (pluggedIn) {
        state = SDL_POWERSTATE_CHARGED;
    } else {
        state = SDL_POWERSTATE_ON_BATTERY;
    }
    if (batteryLevel >= 4) {
        percent = 100;
    } else if (batteryLevel == 3) {
        percent = 70;
    } else if (batteryLevel == 2) {
        percent = 40;
    } else if (batteryLevel == 1) {
        percent = 10;
    } else {
        percent = 3;
    }
    SDL_SendJoystickPowerInfo(joystick, state, percent);
}

/* The IR camera lives on the Wii Remote itself, so it is available whenever the
   device is a Wii Remote: bare, or with a Nunchuk or Classic Controller. The Wii U
   Pro Controller and the Balance Board are not remotes and have no camera. Motion
   Plus coexists with IR: its passthrough frame is 6 bytes and fits report 0x37's
   extension span beside 10-byte Basic IR, the same simultaneous IR + M+ + Nunchuk
   arrangement Dolphin's real-Wiimote backend runs (WiimoteController.cpp:382,
   ReportCoreAccelIR10Ext6) and every WM+ sensor-bar game used. */
static bool WiiRemoteHasIRCamera(EWiiExtensionControllerType type)
{
    return type == k_eWiiExtensionControllerType_None ||
           type == k_eWiiExtensionControllerType_Nunchuk ||
           type == k_eWiiExtensionControllerType_Gamepad;
}

static EWiiInputReportIDs GetButtonPacketType(SDL_DriverWii_Context *ctx)
{
    switch (ctx->m_eExtensionControllerType) {
    case k_eWiiExtensionControllerType_WiiUPro:
        return k_eWiiInputReportIDs_ButtonDataD;
    case k_eWiiExtensionControllerType_BalanceBoard:
        // 34 BB BB EE*19: core + the four weight sensors in the extension span.
        return k_eWiiInputReportIDs_ButtonData4;
    case k_eWiiExtensionControllerType_Nunchuk:
    case k_eWiiExtensionControllerType_Gamepad:
        if (ctx->m_bIRActive) {
            // 37 BB BB AA AA AA II*10 EE*6: core + accel + basic IR + the extension.
            // The Nunchuk and Classic are 6-byte extensions, so they fit the span.
            return k_eWiiInputReportIDs_ButtonData7;
        } else if (ctx->m_bReportSensors) {
            return k_eWiiInputReportIDs_ButtonData5;
        } else {
            return k_eWiiInputReportIDs_ButtonData2;
        }
    default:
        if (ctx->m_bIRActive) {
            /* Bare remote: 33 BB BB AA AA AA II*12 (extended IR, no extension
               span). With Motion Plus active the gyro needs the extension span,
               so 0x37 (basic IR + 6-byte M+ interleave) instead; the camera's
               programmed mode (EnableIR) and this selection must agree or the
               re-request loop fights itself. */
            return ctx->m_bMotionPlusPresent ? k_eWiiInputReportIDs_ButtonData7
                                             : k_eWiiInputReportIDs_ButtonData3;
        } else if (ctx->m_bReportSensors) {
            return k_eWiiInputReportIDs_ButtonData5;
        } else {
            return k_eWiiInputReportIDs_ButtonData0;
        }
    }
}

static bool RequestButtonPacketType(SDL_DriverWii_Context *ctx, EWiiInputReportIDs type)
{
    Uint8 data[3];
    Uint8 tt = (Uint8)ctx->m_bRumbleActive;

    // Continuous reporting off, tt & 4 == 0
    if (ENABLE_CONTINUOUS_REPORTING) {
        tt |= 4;
    }

    data[0] = k_eWiiOutputReportIDs_DataReportingMode;
    data[1] = tt;
    data[2] = type;
    return WriteOutput(ctx, data, sizeof(data), false);
}

static void ResetButtonPacketType(SDL_DriverWii_Context *ctx)
{
    RequestButtonPacketType(ctx, GetButtonPacketType(ctx));
}

static void InitStickCalibrationData(SDL_DriverWii_Context *ctx)
{
    int i;
    switch (ctx->m_eExtensionControllerType) {
    case k_eWiiExtensionControllerType_WiiUPro:
        for (i = 0; i < 4; i++) {
            ctx->m_StickCalibrationData[i].min = 1000;
            ctx->m_StickCalibrationData[i].max = 3000;
            ctx->m_StickCalibrationData[i].center = 0;
            ctx->m_StickCalibrationData[i].deadzone = 100;
        }
        break;
    case k_eWiiExtensionControllerType_Gamepad:
        for (i = 0; i < 4; i++) {
            ctx->m_StickCalibrationData[i].min = i < 2 ? 9 : 5;
            ctx->m_StickCalibrationData[i].max = i < 2 ? 54 : 26;
            ctx->m_StickCalibrationData[i].center = 0;
            ctx->m_StickCalibrationData[i].deadzone = i < 2 ? 4 : 2;
        }
        break;
    case k_eWiiExtensionControllerType_Nunchuk:
        for (i = 0; i < 2; i++) {
            ctx->m_StickCalibrationData[i].min = 40;
            ctx->m_StickCalibrationData[i].max = 215;
            ctx->m_StickCalibrationData[i].center = 0;
            ctx->m_StickCalibrationData[i].deadzone = 10;
        }
        break;
    default:
        break;
    }
}

static void InitializeExtension(SDL_DriverWii_Context *ctx)
{
    /* Resetting the port with an ACTIVE Motion Plus would deactivate it (a
       write of any value to 0xf0 is the deactivation signal, Dolphin
       MotionPlus.cpp:262-268) and open its ~20 ms dead window. The child
       behind an active M+ is unreachable for a reset anyway. */
    if (ctx->m_ucMotionPlusMode == WII_MOTIONPLUS_MODE_NONE) {
        SendExtensionReset(ctx, true);
    }
    InitStickCalibrationData(ctx);
    ResetButtonPacketType(ctx);
}

static void UpdateSlotLED(SDL_DriverWii_Context *ctx)
{
    Uint8 leds;
    Uint8 data[2];

    // The lowest bit needs to have the rumble status
    leds = (Uint8)ctx->m_bRumbleActive;

    if (ctx->m_bPlayerLights) {
        // Use the same LED codes as Smash 8-player for 5-7
        if (ctx->m_nPlayerIndex == 0 || ctx->m_nPlayerIndex > 3) {
            leds |= k_eWiiPlayerLEDs_P1;
        }
        if (ctx->m_nPlayerIndex == 1 || ctx->m_nPlayerIndex == 4) {
            leds |= k_eWiiPlayerLEDs_P2;
        }
        if (ctx->m_nPlayerIndex == 2 || ctx->m_nPlayerIndex == 5) {
            leds |= k_eWiiPlayerLEDs_P3;
        }
        if (ctx->m_nPlayerIndex == 3 || ctx->m_nPlayerIndex == 6) {
            leds |= k_eWiiPlayerLEDs_P4;
        }
        // Turn on all lights for other player indexes
        if (ctx->m_nPlayerIndex < 0 || ctx->m_nPlayerIndex > 6) {
            leds |= k_eWiiPlayerLEDs_P1 | k_eWiiPlayerLEDs_P2 | k_eWiiPlayerLEDs_P3 | k_eWiiPlayerLEDs_P4;
        }
    }

    data[0] = k_eWiiOutputReportIDs_LEDs;
    data[1] = leds;
    WriteOutput(ctx, data, sizeof(data), false);
}

static void SDLCALL SDL_PlayerLEDHintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)userdata;
    bool bPlayerLights = SDL_GetStringBoolean(hint, true);

    if (bPlayerLights != ctx->m_bPlayerLights) {
        ctx->m_bPlayerLights = bPlayerLights;

        UpdateSlotLED(ctx);
    }
}

static EWiiExtensionControllerType ReadExtensionControllerType(SDL_HIDAPI_Device *device)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    EWiiExtensionControllerType eExtensionControllerType = k_eWiiExtensionControllerType_Unknown;
    const int MAX_ATTEMPTS = 20;
    int attempts = 0;

    // Create enough of a context to read the controller type from the device
    for (attempts = 0; attempts < MAX_ATTEMPTS; ++attempts) {
        Uint16 extension;
        if (SendExtensionIdentify(ctx, true) &&
            ParseExtensionIdentifyResponse(ctx, &extension)) {
            Uint8 motion_plus_mode = 0;
            if ((extension & WII_EXTENSION_MOTIONPLUS_MASK) == WII_EXTENSION_MOTIONPLUS_ID) {
                motion_plus_mode = (Uint8)(extension >> 8);
            }

            if (motion_plus_mode) {
                /* The port answered with an ACTIVE Motion Plus. Deactivating
                   it to identify the child opens its ~20 ms dead window,
                   where identify reads return error 7 and parse as "no
                   extension" (the misread behind hifihedgehog/SDL#12), so
                   identify the child from the M+'s stored ID registers and
                   leave the M+ running, as Dolphin's real-Wiimote backend
                   does (WiimoteController.cpp:541-564). The stored registers
                   are stale when no child is attached, so gate on the live
                   passthrough-connected flag from the M+ data frames. */
                Uint16 stored;
                if (ctx->m_bMotionPlusChildConnected &&
                    ReadStoredChildExtensionID(ctx, &stored)) {
                    eExtensionControllerType = GetExtensionType(stored);
                    ctx->m_ucMotionPlusMode = motion_plus_mode;
                    break;
                }

                /* Fallback (no live child flag, or the stored read failed):
                   deactivate the M+, consuming its status pulse so the dead
                   window passes, re-init the now-visible port, and identify
                   with retries while it settles. Error 7 here means
                   "settling", not "removed", for at least 50 ms. */
                DeactivateMotionPlus(ctx);
                SendExtensionReset(ctx, true);
                {
                    const Uint64 retry_deadline = SDL_GetTicks() + 50;
                    for (;;) {
                        extension = WII_EXTENSION_NONE;
                        if (SendExtensionIdentify(ctx, true) &&
                            ParseExtensionIdentifyResponse(ctx, &extension) &&
                            extension != WII_EXTENSION_NONE) {
                            break;
                        }
                        if (SDL_GetTicks() >= retry_deadline) {
                            break;
                        }
                        SDL_Delay(5);
                    }
                }
                eExtensionControllerType = GetExtensionType(extension);

                // Restore the Motion Plus to the mode it was running
                ActivateMotionPlusWithMode(ctx, motion_plus_mode);
                break;
            }

            if (extension == WII_EXTENSION_UNINITIALIZED) {
                /* Uninitialized child with the M+ inactive: the 0xf0 reset
                   write passes through as plain child init (Dolphin
                   MotionPlus.cpp:227-244, "The M+ deactivation signal is
                   cleverly the same as EXT initialization"). */
                SendExtensionReset(ctx, true);
                if (SendExtensionIdentify(ctx, true)) {
                    ParseExtensionIdentifyResponse(ctx, &extension);
                }
            }

            eExtensionControllerType = GetExtensionType(extension);
            break;
        }
    }
    return eExtensionControllerType;
}

static void UpdateDeviceIdentity(SDL_HIDAPI_Device *device)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;

    switch (ctx->m_eExtensionControllerType) {
    case k_eWiiExtensionControllerType_None:
        HIDAPI_SetDeviceName(device, "Nintendo Wii Remote");
        break;
    case k_eWiiExtensionControllerType_Nunchuk:
        HIDAPI_SetDeviceName(device, "Nintendo Wii Remote with Nunchuk");
        break;
    case k_eWiiExtensionControllerType_Gamepad:
        HIDAPI_SetDeviceName(device, "Nintendo Wii Remote with Classic Controller");
        break;
    case k_eWiiExtensionControllerType_WiiUPro:
        HIDAPI_SetDeviceName(device, "Nintendo Wii U Pro Controller");
        break;
    case k_eWiiExtensionControllerType_BalanceBoard:
        HIDAPI_SetDeviceName(device, "Nintendo Wii Balance Board");
        break;
    default:
        HIDAPI_SetDeviceName(device, "Nintendo Wii Remote with Unknown Extension");
        break;
    }
    device->guid.data[15] = ctx->m_eExtensionControllerType;
}

static bool HIDAPI_DriverWii_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverWii_Context *ctx;

    ctx = (SDL_DriverWii_Context *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;

    /* Start the no-input grace period from when the device is added, not from 0.
     * A Wii Remote does not stream until OpenJoystick sets the reporting mode,
     * so no input arrives between here and OpenJoystick. With m_ulLastInput left
     * at 0, UpdateDevice's "now >= m_ulLastInput + INPUT_WAIT_TIMEOUT_MS" check
     * disconnects the device the moment app uptime passes 3s, before the app can
     * open it (the "only works right after a restart" bug). Seeding it with the
     * current tick gives every freshly-added Wii Remote the full timeout window
     * to be opened, regardless of app uptime. */
    ctx->m_ulLastInput = SDL_GetTicks();

    if (device->vendor_id == USB_VENDOR_NINTENDO) {
        ctx->m_eExtensionControllerType = ReadExtensionControllerType(device);

        UpdateDeviceIdentity(device);
    }
    return HIDAPI_JoystickConnected(device, NULL);
}

static int HIDAPI_DriverWii_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverWii_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;

    if (!ctx->joystick) {
        return;
    }

    ctx->m_nPlayerIndex = player_index;

    UpdateSlotLED(ctx);
}

static bool HIDAPI_DriverWii_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;

    SDL_AssertJoysticksLocked();

    ctx->joystick = joystick;

    InitializeExtension(ctx);

    GetMotionPlusState(ctx, &ctx->m_bMotionPlusPresent, &ctx->m_ucMotionPlusMode);

    /* If sensors stayed enabled across an extension hot-plug, resync the M+
       passthrough mode to the (possibly new) extension type: after a child is
       identified behind an ACTIVE M+ from its stored ID, the M+ is still
       running the old mode and would never carry the child's data. The
       equal-mode guard in ActivateMotionPlusWithMode makes this free when
       the mode already matches. */
    if (ctx->m_bReportSensors && ctx->m_bMotionPlusPresent) {
        ActivateMotionPlus(ctx);
    }

    if (NeedsPeriodicMotionPlusCheck(ctx, false)) {
        SchedulePeriodicMotionPlusCheck(ctx);
    }

    /* If sensors were left enabled across an extension hot-plug that cleared
       m_bIRActive, re-power the IR camera and reselect the IR report so the pointer
       resumes without the app re-toggling sensors. Motion Plus does not gate this:
       IR and the M+ interleave share report 0x37 (hifihedgehog/SDL#11). Placed
       after GetMotionPlusState so EnableIR picks the right camera mode. */
    if (WiiRemoteHasIRCamera(ctx->m_eExtensionControllerType) &&
        ctx->m_bReportSensors && !ctx->m_bIRActive) {
        EnableIR(ctx);
        ResetButtonPacketType(ctx);
    }

    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_None ||
        ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Nunchuk) {
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 100.0f);
        if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Nunchuk) {
            SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL_L, 100.0f);
        }

        if (ctx->m_bMotionPlusPresent) {
            SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, 100.0f);
        }
    }

    /* Read the Balance Board's per-unit load-cell calibration and expose it as a
       hex-string property. The live corner sensors come through as axes; PadForge
       reads this calibration to convert the raw corners into kilograms itself,
       which it cannot read directly while SDL owns the device. */
    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_BalanceBoard) {
        ReadBalanceBoardCalibration(ctx);
        if (ctx->m_bBalanceBoardCalibrationValid) {
            char hex[sizeof(ctx->m_rgucBalanceBoardCalibration) * 2 + 1];
            int i;
            for (i = 0; i < (int)sizeof(ctx->m_rgucBalanceBoardCalibration); ++i) {
                (void)SDL_snprintf(hex + (i * 2), 3, "%02x", ctx->m_rgucBalanceBoardCalibration[i]);
            }
            SDL_SetStringProperty(SDL_GetJoystickProperties(joystick),
                                  "SDL.joystick.wii.balance_board_calibration", hex);
        }
    }

    // Initialize player index (needed for setting LEDs)
    ctx->m_nPlayerIndex = SDL_GetJoystickPlayerIndex(joystick);
    ctx->m_bPlayerLights = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_WII_PLAYER_LED, true);
    UpdateSlotLED(ctx);

    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_WII_PLAYER_LED,
                        SDL_PlayerLEDHintChanged, ctx);

    // Initialize the joystick capabilities
    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_WiiUPro) {
        joystick->nbuttons = 15;
    } else if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_BalanceBoard) {
        // The board has no buttons; HandleBalanceBoardData only posts the four axes.
        joystick->nbuttons = 0;
    } else if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_None ||
               ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Nunchuk) {
        /* Every remote input surfaces exactly once through the gamepad layer
           (positions 0-6 plus the hat), so the raw remote-button block
           (15-25) is not declared. 11-14 are the retired dead slots. */
        joystick->nbuttons = 15;
    } else {
        /* Classic Controller and unrecognized extensions: the gamepad layer
           carries the Classic's controls, or nothing at all when the
           extension is unrecognized, so either way the raw block (15-25) is
           the remote's only button surface and stays. */
        joystick->nbuttons = k_eWiiButtons_Max;
    }
    /* Every Wii Remote (bare or with an extension) exposes four extra axes beyond
       the six gamepad axes for the two IR dots, so the pointer is read from one
       stable location and never collides with an extension's stick axes. Devices
       with no camera (Wii U Pro, Balance Board) keep just the gamepad axes. */
    if (WiiRemoteHasIRCamera(ctx->m_eExtensionControllerType)) {
        joystick->naxes = SDL_GAMEPAD_AXIS_COUNT + WII_IR_AXIS_COUNT;
    } else {
        joystick->naxes = SDL_GAMEPAD_AXIS_COUNT;
    }
    /* Every recognized D-pad-bearing configuration reports its D-pad as a
       hat only, the canonical HIDAPI shape (upstream 70ba3f2830), and the
       auto-mappings bind h0.x. The gamepad-position D-pad button indices
       (11-14) are retired in place and stay silent so the raw remote
       buttons above them keep their positions in the configurations that
       still post them (Classic and unrecognized extensions). The Balance
       Board has no D-pad. */
    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_None ||
        ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Nunchuk ||
        ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Gamepad ||
        ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_WiiUPro) {
        joystick->nhats = 1;
    }

    ctx->m_ulLastInput = SDL_GetTicks();

    return true;
}

static bool HIDAPI_DriverWii_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    bool active = (low_frequency_rumble || high_frequency_rumble);

    if (active != ctx->m_bRumbleActive) {
        Uint8 data[2];

        data[0] = k_eWiiOutputReportIDs_Rumble;
        data[1] = (Uint8)active;
        WriteOutput(ctx, data, sizeof(data), false);

        ctx->m_bRumbleActive = active;
    }
    return true;
}

static bool HIDAPI_DriverWii_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverWii_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return SDL_JOYSTICK_CAP_RUMBLE;
}

static bool HIDAPI_DriverWii_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverWii_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverWii_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;

    if (enabled != ctx->m_bReportSensors) {
        ctx->m_bReportSensors = enabled;

        if (ctx->m_bMotionPlusPresent) {
            if (enabled) {
                ActivateMotionPlus(ctx);
            } else {
                DeactivateMotionPlus(ctx);
            }
        }

        /* On any Wii Remote (bare, or with a Nunchuk or Classic Controller),
           enabling sensors also powers the IR camera and switches to the IR report:
           0x33 (core + accel + extended IR) on a bare remote without Motion Plus,
           or 0x37 (core + accel + basic IR + the 6-byte extension span) otherwise.
           The camera stays off until the app opts in this way, so default behavior
           is unchanged. Motion Plus coexists rather than gating IR off: its
           passthrough frame rides 0x37's extension span beside the IR bytes, the
           arrangement Dolphin's real-Wiimote backend runs and WM+ sensor-bar games
           used (hifihedgehog/SDL#11). This runs after the Motion Plus activation
           above so EnableIR sees the current M+ state. */
        if (WiiRemoteHasIRCamera(ctx->m_eExtensionControllerType)) {
            if (enabled) {
                EnableIR(ctx);
            } else {
                DisableIR(ctx);
            }
        }

        ResetButtonPacketType(ctx);
    }
    return true;
}

static void PostStickCalibrated(Uint64 timestamp, SDL_Joystick *joystick, StickCalibrationData *calibration, Uint8 axis, Uint16 data)
{
    Sint16 value = 0;
    if (!calibration->center) {
        // Center on first read
        calibration->center = data;
        return;
    }
    if (data < calibration->min) {
        calibration->min = data;
    }
    if (data > calibration->max) {
        calibration->max = data;
    }
    if (data < calibration->center - calibration->deadzone) {
        Uint16 zero = calibration->center - calibration->deadzone;
        Uint16 range = zero - calibration->min;
        Uint16 distance = zero - data;
        float fvalue = (float)distance / (float)range;
        value = (Sint16)(fvalue * SDL_JOYSTICK_AXIS_MIN);
    } else if (data > calibration->center + calibration->deadzone) {
        Uint16 zero = calibration->center + calibration->deadzone;
        Uint16 range = calibration->max - zero;
        Uint16 distance = data - zero;
        float fvalue = (float)distance / (float)range;
        value = (Sint16)(fvalue * SDL_JOYSTICK_AXIS_MAX);
    }
    if (axis == SDL_GAMEPAD_AXIS_LEFTY || axis == SDL_GAMEPAD_AXIS_RIGHTY) {
        if (value) {
            value = ~value;
        }
    }
    SDL_SendJoystickAxis(timestamp, joystick, axis, value);
}

/* Send button data to SDL
 *`defs` is a mapping for each bit to which button it represents.  0xFF indicates an unused bit
 *`data` is the button data from the controller
 *`size` is the number of bytes in `data` and the number of arrays of 8 mappings in `defs`
 *`on` is the joystick value to be sent if a bit is on
 *`off` is the joystick value to be sent if a bit is off
 */
static void PostPackedButtonData(Uint64 timestamp, SDL_Joystick *joystick, const Uint8 defs[][8], const Uint8 *data, int size, bool on, bool off)
{
    int i, j;

    for (i = 0; i < size; i++) {
        for (j = 0; j < 8; j++) {
            Uint8 button = defs[i][j];
            if (button != 0xFF) {
                bool down = (data[i] >> j) & 1 ? on : off;
                SDL_SendJoystickButton(timestamp, joystick, button, down);
            }
        }
    }
}

static void PostDpadHat(Uint64 timestamp, SDL_Joystick *joystick, bool up, bool down, bool left, bool right)
{
    Uint8 hat = 0;

    if (up) {
        hat |= SDL_HAT_UP;
    }
    if (down) {
        hat |= SDL_HAT_DOWN;
    }
    if (left) {
        hat |= SDL_HAT_LEFT;
    }
    if (right) {
        hat |= SDL_HAT_RIGHT;
    }
    SDL_SendJoystickHat(timestamp, joystick, 0, hat);
}

/* Shared by the Classic Controller (bytes 0-1, at rgucExtension + 4) and the
   Wii U Pro (bytes 0-2, at rgucExtension + 8). The D-pad bits (byte 0 bits
   6-7, byte 1 bits 0-1) are reported as the hat, not as buttons, so with the
   D-pad and Motion Plus interleave bits both unmapped this single table
   serves the Motion Plus passthrough frames too. */
static const Uint8 GAMEPAD_BUTTON_DEFS[3][8] = {
    {
        0xFF /* Unused */,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_GUIDE,
        SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        0xFF /* D-pad Down (hat) */,
        0xFF /* D-pad Right (hat) */,
    },
    {
        0xFF /* D-pad Up (hat); Motion Plus data in passthrough */,
        0xFF /* D-pad Left (hat); Motion Plus data in passthrough */,
        0xFF /* ZR */,
        SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_SOUTH,
        0xFF /*ZL*/,
    },
    {
        SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK,
        0xFF /* Charging */,
        0xFF /* Plugged In */,
        0xFF /* Unused */,
        0xFF /* Unused */,
        0xFF /* Unused */,
        0xFF /* Unused */,
    }
};

static void HandleWiiUProButtonData(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, const WiiButtonData *data)
{
    static const Uint8 axes[] = { SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTY };
    const Uint8(*buttons)[8] = GAMEPAD_BUTTON_DEFS;
    Uint8 zl, zr;
    int i;

    if (data->ucNExtensionBytes < 11) {
        return;
    }

    // Buttons
    PostPackedButtonData(ctx->timestamp, joystick, buttons, data->rgucExtension + 8, 3, false, true);

    // D-pad as a hat, from the extension's active-low D-pad bits
    PostDpadHat(ctx->timestamp, joystick,
                !(data->rgucExtension[9] & 0x01),
                !(data->rgucExtension[8] & 0x40),
                !(data->rgucExtension[9] & 0x02),
                !(data->rgucExtension[8] & 0x80));

    // Triggers
    zl = data->rgucExtension[9] & 0x80;
    zr = data->rgucExtension[9] & 0x04;
    SDL_SendJoystickAxis(ctx->timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, zl ? SDL_JOYSTICK_AXIS_MIN : SDL_JOYSTICK_AXIS_MAX);
    SDL_SendJoystickAxis(ctx->timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, zr ? SDL_JOYSTICK_AXIS_MIN : SDL_JOYSTICK_AXIS_MAX);

    // Sticks
    for (i = 0; i < 4; i++) {
        Uint16 value = data->rgucExtension[i * 2] | (data->rgucExtension[i * 2 + 1] << 8);
        PostStickCalibrated(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[i], axes[i], value);
    }

    // Power
    UpdatePowerLevelWiiU(joystick, data->rgucExtension[10]);
}

static void HandleGamepadControllerButtonData(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, const WiiButtonData *data)
{
    Uint8 lx, ly, rx, ry, zl, zr;

    if (data->ucNExtensionBytes < 6) {
        return;
    }

    // Buttons
    PostPackedButtonData(ctx->timestamp, joystick, GAMEPAD_BUTTON_DEFS, data->rgucExtension + 4, 2, false, true);

    /* D-pad as a hat, from the extension's active-low D-pad bits. In Motion
       Plus passthrough the up/left bits relocate to bit 0 of bytes 0 and 1
       (Dolphin MotionPlus.cpp:669-678, verified on real hardware). */
    if (ctx->m_ucMotionPlusMode == WII_MOTIONPLUS_MODE_GAMEPAD) {
        PostDpadHat(ctx->timestamp, joystick,
                    !(data->rgucExtension[0] & 0x01),
                    !(data->rgucExtension[4] & 0x40),
                    !(data->rgucExtension[1] & 0x01),
                    !(data->rgucExtension[4] & 0x80));
    } else {
        PostDpadHat(ctx->timestamp, joystick,
                    !(data->rgucExtension[5] & 0x01),
                    !(data->rgucExtension[4] & 0x40),
                    !(data->rgucExtension[5] & 0x02),
                    !(data->rgucExtension[4] & 0x80));
    }

    // Triggers
    zl = data->rgucExtension[5] & 0x80;
    zr = data->rgucExtension[5] & 0x04;
    SDL_SendJoystickAxis(ctx->timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, zl ? SDL_JOYSTICK_AXIS_MIN : SDL_JOYSTICK_AXIS_MAX);
    SDL_SendJoystickAxis(ctx->timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, zr ? SDL_JOYSTICK_AXIS_MIN : SDL_JOYSTICK_AXIS_MAX);

    // Sticks
    if (ctx->m_ucMotionPlusMode == WII_MOTIONPLUS_MODE_GAMEPAD) {
        lx = data->rgucExtension[0] & 0x3E;
        ly = data->rgucExtension[1] & 0x3E;
    } else {
        lx = data->rgucExtension[0] & 0x3F;
        ly = data->rgucExtension[1] & 0x3F;
    }
    rx = (data->rgucExtension[2] >> 7) | ((data->rgucExtension[1] >> 5) & 0x06) | ((data->rgucExtension[0] >> 3) & 0x18);
    ry = data->rgucExtension[2] & 0x1F;
    PostStickCalibrated(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[0], SDL_GAMEPAD_AXIS_LEFTX, lx);
    PostStickCalibrated(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[1], SDL_GAMEPAD_AXIS_LEFTY, ly);
    PostStickCalibrated(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[2], SDL_GAMEPAD_AXIS_RIGHTX, rx);
    PostStickCalibrated(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[3], SDL_GAMEPAD_AXIS_RIGHTY, ry);
}

static void HandleWiiRemoteButtonData(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, const WiiButtonData *data)
{
    static const Uint8 buttons[2][8] = {
        {
            k_eWiiButtons_DPad_Left,
            k_eWiiButtons_DPad_Right,
            k_eWiiButtons_DPad_Down,
            k_eWiiButtons_DPad_Up,
            k_eWiiButtons_Plus,
            0xFF /* Unused */,
            0xFF /* Unused */,
            0xFF /* Unused */,
        },
        {
            k_eWiiButtons_Two,
            k_eWiiButtons_One,
            k_eWiiButtons_B,
            k_eWiiButtons_A,
            k_eWiiButtons_Minus,
            0xFF /* Unused */,
            0xFF /* Unused */,
            k_eWiiButtons_Home,
        }
    };
    if (data->hasBaseButtons) {
        PostPackedButtonData(ctx->timestamp, joystick, buttons, data->rgucBaseButtons, 2, true, false);
    }
}

static void HandleWiiRemoteButtonDataAsMainController(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, const WiiButtonData *data)
{
    /* Wii remote maps really badly to a normal controller
     * Mapped 1 and 2 as X and Y
     * Not going to attempt positional mapping
     */
    static const Uint8 buttons[2][8] = {
        {
            0xFF /* D-pad Left (hat) */,
            0xFF /* D-pad Right (hat) */,
            0xFF /* D-pad Down (hat) */,
            0xFF /* D-pad Up (hat) */,
            SDL_GAMEPAD_BUTTON_START,
            0xFF /* Unused */,
            0xFF /* Unused */,
            0xFF /* Unused */,
        },
        {
            SDL_GAMEPAD_BUTTON_NORTH,
            SDL_GAMEPAD_BUTTON_WEST,
            SDL_GAMEPAD_BUTTON_SOUTH,
            SDL_GAMEPAD_BUTTON_EAST,
            SDL_GAMEPAD_BUTTON_BACK,
            0xFF /* Unused */,
            0xFF /* Unused */,
            SDL_GAMEPAD_BUTTON_GUIDE,
        }
    };
    if (data->hasBaseButtons) {
        PostPackedButtonData(ctx->timestamp, joystick, buttons, data->rgucBaseButtons, 2, true, false);
        PostDpadHat(ctx->timestamp, joystick,
                    (data->rgucBaseButtons[0] & 0x08) != 0,
                    (data->rgucBaseButtons[0] & 0x04) != 0,
                    (data->rgucBaseButtons[0] & 0x01) != 0,
                    (data->rgucBaseButtons[0] & 0x02) != 0);
    }
}

static void HandleNunchuckButtonData(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, const WiiButtonData *data)
{
    bool c_button, z_button;

    if (data->ucNExtensionBytes < 6) {
        return;
    }

    if (ctx->m_ucMotionPlusMode == WII_MOTIONPLUS_MODE_NUNCHUK) {
        c_button = (data->rgucExtension[5] & 0x08) ? false : true;
        z_button = (data->rgucExtension[5] & 0x04) ? false : true;
    } else {
        c_button = (data->rgucExtension[5] & 0x02) ? false : true;
        z_button = (data->rgucExtension[5] & 0x01) ? false : true;
    }
    SDL_SendJoystickButton(ctx->timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, c_button);
    SDL_SendJoystickAxis(ctx->timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, z_button ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    PostStickCalibrated(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[0], SDL_GAMEPAD_AXIS_LEFTX, data->rgucExtension[0]);
    PostStickCalibrated(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[1], SDL_GAMEPAD_AXIS_LEFTY, data->rgucExtension[1]);

    if (ctx->m_bReportSensors) {
        const float ACCEL_RES_PER_G = 200.0f;
        Sint16 x, y, z;
        float values[3];

        x = (data->rgucExtension[2] << 2);
        y = (data->rgucExtension[3] << 2);
        z = (data->rgucExtension[4] << 2);

        if (ctx->m_ucMotionPlusMode == WII_MOTIONPLUS_MODE_NUNCHUK) {
            x |= ((data->rgucExtension[5] >> 3) & 0x02);
            y |= ((data->rgucExtension[5] >> 4) & 0x02);
            z &= ~0x04;
            z |= ((data->rgucExtension[5] >> 5) & 0x06);
        } else {
            x |= ((data->rgucExtension[5] >> 2) & 0x03);
            y |= ((data->rgucExtension[5] >> 4) & 0x03);
            z |= ((data->rgucExtension[5] >> 6) & 0x03);
        }

        x -= 0x200;
        y -= 0x200;
        z -= 0x200;

        values[0] = -((float)x / ACCEL_RES_PER_G) * SDL_STANDARD_GRAVITY;
        values[1] = ((float)z / ACCEL_RES_PER_G) * SDL_STANDARD_GRAVITY;
        values[2] = ((float)y / ACCEL_RES_PER_G) * SDL_STANDARD_GRAVITY;
        SDL_SendJoystickSensor(ctx->timestamp, joystick, SDL_SENSOR_ACCEL_L, ctx->timestamp, values, 3);
    }
}

static void HandleMotionPlusData(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, const WiiButtonData *data)
{
    if (ctx->m_bReportSensors) {
        const float GYRO_RES_PER_DEGREE = 8192.0f;
        int x, y, z;
        float values[3];

        x = (data->rgucExtension[0] | ((data->rgucExtension[3] << 6) & 0xFF00)) - 8192;
        y = (data->rgucExtension[1] | ((data->rgucExtension[4] << 6) & 0xFF00)) - 8192;
        z = (data->rgucExtension[2] | ((data->rgucExtension[5] << 6) & 0xFF00)) - 8192;

        if (data->rgucExtension[3] & 0x02) {
            // Slow rotation rate: 8192/440 units per deg/s
            x *= 440;
        } else {
            // Fast rotation rate: 8192/2000 units per deg/s
            x *= 2000;
        }
        if (data->rgucExtension[4] & 0x02) {
            // Slow rotation rate: 8192/440 units per deg/s
            y *= 440;
        } else {
            // Fast rotation rate: 8192/2000 units per deg/s
            y *= 2000;
        }
        if (data->rgucExtension[3] & 0x01) {
            // Slow rotation rate: 8192/440 units per deg/s
            z *= 440;
        } else {
            // Fast rotation rate: 8192/2000 units per deg/s
            z *= 2000;
        }

        values[0] = -((float)z / GYRO_RES_PER_DEGREE) * SDL_PI_F / 180.0f;
        values[1] = ((float)x / GYRO_RES_PER_DEGREE) * SDL_PI_F / 180.0f;
        values[2] = ((float)y / GYRO_RES_PER_DEGREE) * SDL_PI_F / 180.0f;
        SDL_SendJoystickSensor(ctx->timestamp, joystick, SDL_SENSOR_GYRO, ctx->timestamp, values, 3);
    }
}

static void HandleWiiRemoteAccelData(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, const WiiButtonData *data)
{
    const float ACCEL_RES_PER_G = 100.0f;
    Sint16 x, y, z;
    float values[3];

    if (!ctx->m_bReportSensors) {
        return;
    }

    x = ((data->rgucAccelerometer[0] << 2) | ((data->rgucBaseButtons[0] >> 5) & 0x03)) - 0x200;
    y = ((data->rgucAccelerometer[1] << 2) | ((data->rgucBaseButtons[1] >> 4) & 0x02)) - 0x200;
    z = ((data->rgucAccelerometer[2] << 2) | ((data->rgucBaseButtons[1] >> 5) & 0x02)) - 0x200;

    values[0] = -((float)x / ACCEL_RES_PER_G) * SDL_STANDARD_GRAVITY;
    values[1] = ((float)z / ACCEL_RES_PER_G) * SDL_STANDARD_GRAVITY;
    values[2] = ((float)y / ACCEL_RES_PER_G) * SDL_STANDARD_GRAVITY;
    SDL_SendJoystickSensor(ctx->timestamp, joystick, SDL_SENSOR_ACCEL, ctx->timestamp, values, 3);
}

static void HandleIRData(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, const Uint8 *buff, bool basic)
{
    /* The IR span starts at buff[6]. The first two dots are the two sensor-bar LEDs,
       all a pointer needs (WiimoteLib-Trihy ParseIR, Wiimote.cs:608-649). Surface
       their raw coordinates (X 0..1023, Y 0..767) on dedicated joystick axes (beyond
       the gamepad sticks), with -1 meaning "dot not detected". PadForge runs its own
       homography and cursor mapping on the raw points, so no normalization is done.

       Extended mode (report 0x33, bare remote) gives each dot its own high-bit byte,
       so dot1's high bits live in buff[11]. Basic mode (report 0x37, with extension)
       packs both dots into 5 bytes, so dot1's high bits share buff[8]. */
    int dot0_x = buff[6] | (((buff[8] >> 4) & 0x03) << 8);
    int dot0_y = buff[7] | (((buff[8] >> 6) & 0x03) << 8);
    int dot1_x, dot1_y;
    bool dot0_found, dot1_found;

    if (basic) {
        dot1_x = buff[9] | (((buff[8] >> 0) & 0x03) << 8);
        dot1_y = buff[10] | (((buff[8] >> 2) & 0x03) << 8);
        dot0_found = !(buff[6] == 0xFF && buff[7] == 0xFF);
        dot1_found = !(buff[9] == 0xFF && buff[10] == 0xFF);
    } else {
        dot1_x = buff[9] | (((buff[11] >> 4) & 0x03) << 8);
        dot1_y = buff[10] | (((buff[11] >> 6) & 0x03) << 8);
        dot0_found = !(buff[6] == 0xFF && buff[7] == 0xFF && buff[8] == 0xFF);
        dot1_found = !(buff[9] == 0xFF && buff[10] == 0xFF && buff[11] == 0xFF);
    }

    SDL_SendJoystickAxis(ctx->timestamp, joystick, WII_IR_AXIS_DOT0_X, dot0_found ? (Sint16)dot0_x : (Sint16)-1);
    SDL_SendJoystickAxis(ctx->timestamp, joystick, WII_IR_AXIS_DOT0_Y, dot0_found ? (Sint16)dot0_y : (Sint16)-1);
    SDL_SendJoystickAxis(ctx->timestamp, joystick, WII_IR_AXIS_DOT1_X, dot1_found ? (Sint16)dot1_x : (Sint16)-1);
    SDL_SendJoystickAxis(ctx->timestamp, joystick, WII_IR_AXIS_DOT1_Y, dot1_found ? (Sint16)dot1_y : (Sint16)-1);
}

static void HandleBalanceBoardData(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, const WiiButtonData *data)
{
    Sint16 top_right, bottom_right, top_left, bottom_left;

    if (data->ucNExtensionBytes < 8) {
        return;
    }
    /* Four corner load cells, raw int16 big-endian at the start of the extension
       span (WiimoteLib-Trihy ParseBalanceBoard, Wiimote.cs:898-902). Surfaced raw
       on the four stick axes; PadForge derives weight and center-of-gravity with
       its own kg interpolation from these and the exposed calibration. */
    top_right    = (Sint16)((data->rgucExtension[0] << 8) | data->rgucExtension[1]);
    bottom_right = (Sint16)((data->rgucExtension[2] << 8) | data->rgucExtension[3]);
    top_left     = (Sint16)((data->rgucExtension[4] << 8) | data->rgucExtension[5]);
    bottom_left  = (Sint16)((data->rgucExtension[6] << 8) | data->rgucExtension[7]);

    SDL_SendJoystickAxis(ctx->timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, top_left);
    SDL_SendJoystickAxis(ctx->timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, bottom_left);
    SDL_SendJoystickAxis(ctx->timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, top_right);
    SDL_SendJoystickAxis(ctx->timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, bottom_right);
}

static void HandleButtonData(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, WiiButtonData *data)
{
    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_BalanceBoard) {
        HandleBalanceBoardData(ctx, joystick, data);
        return;
    }

    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_WiiUPro) {
        HandleWiiUProButtonData(ctx, joystick, data);
        return;
    }

    if (ctx->m_ucMotionPlusMode != WII_MOTIONPLUS_MODE_NONE &&
        data->ucNExtensionBytes > 5) {
        if (data->rgucExtension[5] & 0x01) {
            // The data is invalid, possibly during a hotplug
            return;
        }

        /* Record the live passthrough-connected flag: it gates the stored
           child-ID identify when the extension type is re-read behind an
           ACTIVE Motion Plus (the stored registers are stale without it). */
        ctx->m_bMotionPlusChildConnected = ((data->rgucExtension[4] & 0x01) != 0);

        if (data->rgucExtension[4] & 0x01) {
            if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_None) {
                // Something was plugged into the extension port, reinitialize to get new state
                ctx->m_bDisconnected = true;
            }
        } else {
            if (ctx->m_eExtensionControllerType != k_eWiiExtensionControllerType_None) {
                // Something was removed from the extension port, reinitialize to get new state
                ctx->m_bDisconnected = true;
            }
        }

        if (data->rgucExtension[5] & 0x02) {
            HandleMotionPlusData(ctx, joystick, data);

            // The extension data is consumed
            data->ucNExtensionBytes = 0;
        }
    }

    /* Every input surfaces once. In the bare and Nunchuk configurations the
       remote's buttons reach the gamepad layer through the main-controller
       handler (positions 0-6 plus the hat), so the raw remote-button block
       (15-25) is not posted there. With a Classic attached the gamepad
       layer carries the Classic's controls, and with an unrecognized
       extension it carries nothing, so in both states the raw block is the
       remote's only button surface. */
    switch (ctx->m_eExtensionControllerType) {
    case k_eWiiExtensionControllerType_Nunchuk:
        HandleNunchuckButtonData(ctx, joystick, data);
        SDL_FALLTHROUGH;
    case k_eWiiExtensionControllerType_None:
        HandleWiiRemoteButtonDataAsMainController(ctx, joystick, data);
        break;
    case k_eWiiExtensionControllerType_Gamepad:
        HandleWiiRemoteButtonData(ctx, joystick, data);
        HandleGamepadControllerButtonData(ctx, joystick, data);
        break;
    default:
        HandleWiiRemoteButtonData(ctx, joystick, data);
        break;
    }
    HandleWiiRemoteAccelData(ctx, joystick, data);
}

static void GetBaseButtons(WiiButtonData *dst, const Uint8 *src)
{
    SDL_memcpy(dst->rgucBaseButtons, src, 2);
    dst->hasBaseButtons = true;
}

static void GetAccelerometer(WiiButtonData *dst, const Uint8 *src)
{
    SDL_memcpy(dst->rgucAccelerometer, src, 3);
    dst->hasAccelerometer = true;
}

static void GetExtensionData(WiiButtonData *dst, const Uint8 *src, int size)
{
    bool valid_data = false;
    int i;

    if (size > sizeof(dst->rgucExtension)) {
        size = sizeof(dst->rgucExtension);
    }

    for (i = 0; i < size; ++i) {
        if (src[i] != 0xFF) {
            valid_data = true;
            break;
        }
    }
    if (valid_data) {
        SDL_memcpy(dst->rgucExtension, src, size);
        dst->ucNExtensionBytes = (Uint8)size;
    }
}

static void HandleStatus(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick)
{
    bool hadExtension = ctx->m_eExtensionControllerType != k_eWiiExtensionControllerType_None;
    bool hasExtension = (ctx->m_rgucReadBuffer[3] & 2) ? true : false;
    WiiButtonData data;
    SDL_zero(data);
    GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
    HandleButtonData(ctx, joystick, &data);

    if (ctx->m_eExtensionControllerType != k_eWiiExtensionControllerType_WiiUPro) {
        // Wii U has separate battery level tracking
        UpdatePowerLevelWii(joystick, ctx->m_rgucReadBuffer[6]);
    }

    // The report data format has been reset, need to update it
    ResetButtonPacketType(ctx);

    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Status update, extension %s", hasExtension ? "CONNECTED" : "DISCONNECTED");

    /* When Motion Plus is active, we get extension connect/disconnect status
     * through the Motion Plus packets. Otherwise we can use the status here.
     */
    if (ctx->m_ucMotionPlusMode != WII_MOTIONPLUS_MODE_NONE) {
        /* Check to make sure the Motion Plus extension state hasn't changed,
         * otherwise we'll get extension connect/disconnect status through
         * Motion Plus packets.
         */
        if (NeedsPeriodicMotionPlusCheck(ctx, true)) {
            /* Not NOW: this status is usually the M+'s own activation or
               deactivation pulse, and probing inside the ~20 ms dead window
               reads error 7, which parses as "no extension" and tears the
               device down (hifihedgehog/SDL#12). Check once the mode write
               has settled. */
            Uint64 now = SDL_GetTicks();
            ctx->m_ulNextMotionPlusCheck = SDL_max(now, ctx->m_ulMotionPlusSettleDeadline);
        }

    } else if (hadExtension != hasExtension) {
        // Reinitialize to get new state
        ctx->m_bDisconnected = true;
    }
}

static void HandleResponse(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick)
{
    EWiiInputReportIDs type = (EWiiInputReportIDs)ctx->m_rgucReadBuffer[0];
    WiiButtonData data;
    SDL_assert(type == k_eWiiInputReportIDs_Acknowledge || type == k_eWiiInputReportIDs_ReadMemory);
    SDL_zero(data);
    GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
    HandleButtonData(ctx, joystick, &data);

    switch (ctx->m_eCommState) {
    case k_eWiiCommunicationState_None:
        break;

    case k_eWiiCommunicationState_CheckMotionPlusStage1:
    case k_eWiiCommunicationState_CheckMotionPlusStage2:
    {
        Uint16 extension = 0;
        if (ParseExtensionIdentifyResponse(ctx, &extension)) {
            if ((extension & WII_EXTENSION_MOTIONPLUS_MASK) == WII_EXTENSION_MOTIONPLUS_ID) {
                // Motion Plus is currently active
                SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Motion Plus CONNECTED (stage %d)", ctx->m_eCommState == k_eWiiCommunicationState_CheckMotionPlusStage1 ? 1 : 2);

                if (!ctx->m_bMotionPlusPresent) {
                    // Reinitialize to get new sensor availability
                    ctx->m_bDisconnected = true;
                }
                ctx->m_eCommState = k_eWiiCommunicationState_None;

            } else if (ctx->m_eCommState == k_eWiiCommunicationState_CheckMotionPlusStage1) {
                // Check to see if Motion Plus is present
                ReadRegister(ctx, 0xA600FE, 2, false);

                ctx->m_eCommState = k_eWiiCommunicationState_CheckMotionPlusStage2;

            } else if (SDL_GetTicks() < ctx->m_ulMotionPlusSettleDeadline) {
                /* Non-response while a mode write settles means the M+ is
                   mid-reset, not removed. Dolphin maps non-response to wait
                   and retry, never to a state change (WiimoteController.cpp:
                   660-668). Check again after the window. */
                ctx->m_ulNextMotionPlusCheck = ctx->m_ulMotionPlusSettleDeadline;
                ctx->m_eCommState = k_eWiiCommunicationState_None;

            } else {
                // Motion Plus is not present
                SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Motion Plus DISCONNECTED (stage %d)", ctx->m_eCommState == k_eWiiCommunicationState_CheckMotionPlusStage1 ? 1 : 2);

                if (ctx->m_bMotionPlusPresent) {
                    // Reinitialize to get new sensor availability
                    ctx->m_bDisconnected = true;
                }
                ctx->m_eCommState = k_eWiiCommunicationState_None;
            }
        }
    } break;
    default:
        // Should never happen
        break;
    }
}

static void HandleButtonPacket(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick)
{
    EWiiInputReportIDs eExpectedReport = GetButtonPacketType(ctx);
    WiiButtonData data;

    // FIXME: This should see if the data format is compatible rather than equal
    if (eExpectedReport != ctx->m_rgucReadBuffer[0]) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Resetting report mode to %d", eExpectedReport);
        RequestButtonPacketType(ctx, eExpectedReport);
    }

    SDL_zero(data);
    switch (ctx->m_rgucReadBuffer[0]) {
    case k_eWiiInputReportIDs_ButtonData0: // 30 BB BB
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        break;
    case k_eWiiInputReportIDs_ButtonData1: // 31 BB BB AA AA AA
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        GetAccelerometer(&data, ctx->m_rgucReadBuffer + 3);
        break;
    case k_eWiiInputReportIDs_ButtonData3: // 33 BB BB AA AA AA II II II II II II II II II II II II
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        GetAccelerometer(&data, ctx->m_rgucReadBuffer + 3);
        // The 12 IR bytes at offset 6 are the extended-mode camera dots.
        HandleIRData(ctx, joystick, ctx->m_rgucReadBuffer, false);
        break;
    case k_eWiiInputReportIDs_ButtonData2: // 32 BB BB EE EE EE EE EE EE EE EE
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        GetExtensionData(&data, ctx->m_rgucReadBuffer + 3, 8);
        break;
    case k_eWiiInputReportIDs_ButtonData4: // 34 BB BB EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        GetExtensionData(&data, ctx->m_rgucReadBuffer + 3, 19);
        break;
    case k_eWiiInputReportIDs_ButtonData5: // 35 BB BB AA AA AA EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        GetAccelerometer(&data, ctx->m_rgucReadBuffer + 3);
        GetExtensionData(&data, ctx->m_rgucReadBuffer + 6, 16);
        break;
    case k_eWiiInputReportIDs_ButtonData6: // 36 BB BB II II II II II II II II II II EE EE EE EE EE EE EE EE EE
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        GetExtensionData(&data, ctx->m_rgucReadBuffer + 13, 9);
        break;
    case k_eWiiInputReportIDs_ButtonData7: // 37 BB BB AA AA AA II II II II II II II II II II EE EE EE EE EE EE
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        GetAccelerometer(&data, ctx->m_rgucReadBuffer + 3);
        // 10 basic-mode IR bytes at offset 6, then the 6-byte extension at offset 16.
        HandleIRData(ctx, joystick, ctx->m_rgucReadBuffer, true);
        GetExtensionData(&data, ctx->m_rgucReadBuffer + 16, 6);
        break;
    case k_eWiiInputReportIDs_ButtonDataD: // 3d EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE
        GetExtensionData(&data, ctx->m_rgucReadBuffer + 1, 21);
        break;
    case k_eWiiInputReportIDs_ButtonDataE:
    case k_eWiiInputReportIDs_ButtonDataF:
    default:
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Unsupported button data type %02x", ctx->m_rgucReadBuffer[0]);
        return;
    }
    HandleButtonData(ctx, joystick, &data);
}

static void HandleInput(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick)
{
    EWiiInputReportIDs type = (EWiiInputReportIDs)ctx->m_rgucReadBuffer[0];

    // Set up for handling input
    ctx->timestamp = SDL_GetTicksNS();

    if (type == k_eWiiInputReportIDs_Status) {
        HandleStatus(ctx, joystick);
    } else if (type == k_eWiiInputReportIDs_Acknowledge || type == k_eWiiInputReportIDs_ReadMemory) {
        HandleResponse(ctx, joystick);
    } else if (type >= k_eWiiInputReportIDs_ButtonData0 && type <= k_eWiiInputReportIDs_ButtonDataF) {
        HandleButtonPacket(ctx, joystick);
    } else {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Unexpected input packet of type %x", type);
    }
}

static bool HIDAPI_DriverWii_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    int size;
    Uint64 now;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    } else {
        return false;
    }

    now = SDL_GetTicks();

    while ((size = ReadInput(ctx)) > 0) {
        if (joystick) {
            HandleInput(ctx, joystick);
        }
        ctx->m_ulLastInput = now;
    }

    /* Check to see if we've lost connection to the controller.
     * We have continuous reporting enabled, so this should be reliable now.
     */
    {
        SDL_COMPILE_TIME_ASSERT(ENABLE_CONTINUOUS_REPORTING, ENABLE_CONTINUOUS_REPORTING);
    }
    if (now >= (ctx->m_ulLastInput + INPUT_WAIT_TIMEOUT_MS)) {
        // Bluetooth may have disconnected, try reopening the controller
        size = -1;
    }

    if (joystick) {
        // These checks aren't needed on the Wii U Pro Controller
        if (ctx->m_eExtensionControllerType != k_eWiiExtensionControllerType_WiiUPro) {

            // Check to see if the Motion Plus extension status has changed
            if (ctx->m_ulNextMotionPlusCheck && now >= ctx->m_ulNextMotionPlusCheck) {
                CheckMotionPlusConnection(ctx);
                if (NeedsPeriodicMotionPlusCheck(ctx, false)) {
                    SchedulePeriodicMotionPlusCheck(ctx);
                } else {
                    ctx->m_ulNextMotionPlusCheck = 0;
                }
            }

            // Request a status update periodically to make sure our battery value is up to date
            if (!ctx->m_ulLastStatus || now >= (ctx->m_ulLastStatus + STATUS_UPDATE_TIME_MS)) {
                Uint8 data[2];

                data[0] = k_eWiiOutputReportIDs_StatusRequest;
                data[1] = (Uint8)ctx->m_bRumbleActive;
                WriteOutput(ctx, data, sizeof(data), false);

                ctx->m_ulLastStatus = now;
            }
        }
    }

    if (size < 0) {
        // Read error, device is disconnected
        HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
    } else if (ctx->m_bDisconnected) {
        // An extension was hot-plugged (Nunchuk/Classic attach or detach, or a
        // Motion Plus state change). The device stays physically present, so the
        // HIDAPI core never tears it down and re-runs InitDevice. Re-identify in
        // place: drop the old joystick, re-read the extension type, refresh the
        // name/GUID, then re-add with the new capabilities. OpenJoystick re-applies
        // nbuttons/naxes and per-extension sensors when the app reopens it.
        HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        ctx->m_eExtensionControllerType = ReadExtensionControllerType(device);
        UpdateDeviceIdentity(device);
        ctx->m_bDisconnected = false;
        /* The extension churn reset the IR camera, so the active flag is now stale.
         * Clear it; OpenJoystick re-powers IR on reopen if sensors stay enabled. */
        ctx->m_bIRActive = false;
        /* Re-seed the idle clock after the blocking identify (see the InitDevice
         * seed for issue #3). ReadExtensionControllerType can block across several
         * read attempts, and the no-input timeout check above is not gated on an
         * open joystick, so a stale m_ulLastInput would disconnect the re-added
         * joystick before the app can reopen it. */
        ctx->m_ulLastInput = SDL_GetTicks();
        HIDAPI_JoystickConnected(device, NULL);
    }
    return (size >= 0);
}

static void HIDAPI_DriverWii_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;

    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_WII_PLAYER_LED,
                        SDL_PlayerLEDHintChanged, ctx);

    ctx->joystick = NULL;
}

static void HIDAPI_DriverWii_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverWii = {
    SDL_HINT_JOYSTICK_HIDAPI_WII,
    true,
    HIDAPI_DriverWii_RegisterHints,
    HIDAPI_DriverWii_UnregisterHints,
    HIDAPI_DriverWii_IsEnabled,
    HIDAPI_DriverWii_IsSupportedDevice,
    HIDAPI_DriverWii_InitDevice,
    HIDAPI_DriverWii_GetDevicePlayerIndex,
    HIDAPI_DriverWii_SetDevicePlayerIndex,
    HIDAPI_DriverWii_UpdateDevice,
    HIDAPI_DriverWii_OpenJoystick,
    HIDAPI_DriverWii_RumbleJoystick,
    HIDAPI_DriverWii_RumbleJoystickTriggers,
    HIDAPI_DriverWii_GetJoystickCapabilities,
    HIDAPI_DriverWii_SetJoystickLED,
    HIDAPI_DriverWii_SendJoystickEffect,
    HIDAPI_DriverWii_SetJoystickSensorsEnabled,
    HIDAPI_DriverWii_CloseJoystick,
    HIDAPI_DriverWii_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_WII

#endif // SDL_JOYSTICK_HIDAPI
