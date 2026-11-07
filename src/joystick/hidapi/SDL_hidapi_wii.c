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
#include "SDL_hidapi_wii_ext_proto.h"

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
/* A camera start that failed, or whose status report showed the camera off,
   runs again after 1, 2 and 4 seconds, then every 8 (hifihedgehog/SDL#39) */
static const Uint32 k_rgunIRStartDelaysMS[] = { 1000, 2000, 4000, 8000 };
/* A camera write the remote has not acknowledged 250 ms after the rumble
   thread's queue drained, the wait ReadInputSync gives a write it sent, goes
   again, four sends in all, before the start fails (hifihedgehog/SDL#40) */
#define WII_WRITE_PATIENCE_MS 250
#define WII_WRITE_MAX_SENDS   4

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

/* The writes the update loop waits on (hifihedgehog/SDL#40): Motion Plus
   writes, then the camera start in WiimoteLib's order, each camera write
   after the one before it is acknowledged */
typedef enum
{
    k_eWiiWriteStep_None,
    k_eWiiWriteStep_MotionPlus,         // the Motion Plus mode, or its deactivation
    k_eWiiWriteStep_CameraEnable,       // output report 0x13
    k_eWiiWriteStep_CameraEnable2,      // output report 0x1a
    k_eWiiWriteStep_CameraControl,      // 0x08 to the control register
    k_eWiiWriteStep_CameraSensitivity1, // the first sensitivity block
    k_eWiiWriteStep_CameraSensitivity2, // the second sensitivity block
    k_eWiiWriteStep_CameraMode,         // Basic or Extended mode
    k_eWiiWriteStep_CameraControl2,     // 0x08 to the control register again
} EWiiWriteStep;

typedef enum
{
    k_eWiiMotionPlusWrite_None,
    k_eWiiMotionPlusWrite_Activate,
    k_eWiiMotionPlusWrite_Deactivate,
} EWiiMotionPlusWrite;

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

/* SDL_hidapi_wii_ext_proto.c decodes the extensions the driver used to report
   as unknown. It builds without SDL's headers, so its values are checked
   against SDL's here. */
SDL_COMPILE_TIME_ASSERT(wii_ext_none, SDL_WII_EXT_NONE == k_eWiiExtensionControllerType_None);
SDL_COMPILE_TIME_ASSERT(wii_ext_unknown, SDL_WII_EXT_UNKNOWN == k_eWiiExtensionControllerType_Unknown);
SDL_COMPILE_TIME_ASSERT(wii_ext_nunchuk, SDL_WII_EXT_NUNCHUK == k_eWiiExtensionControllerType_Nunchuk);
SDL_COMPILE_TIME_ASSERT(wii_ext_gamepad, SDL_WII_EXT_GAMEPAD == k_eWiiExtensionControllerType_Gamepad);
SDL_COMPILE_TIME_ASSERT(wii_ext_wiiupro, SDL_WII_EXT_WIIUPRO == k_eWiiExtensionControllerType_WiiUPro);
SDL_COMPILE_TIME_ASSERT(wii_ext_balanceboard, SDL_WII_EXT_BALANCEBOARD == k_eWiiExtensionControllerType_BalanceBoard);
SDL_COMPILE_TIME_ASSERT(wii_ext_guitar, SDL_WII_EXT_GUITAR == k_eWiiExtensionControllerType_Guitar);
SDL_COMPILE_TIME_ASSERT(wii_ext_drums, SDL_WII_EXT_DRUMS == k_eWiiExtensionControllerType_Drums);
SDL_COMPILE_TIME_ASSERT(wii_ext_turntable, SDL_WII_EXT_TURNTABLE == k_eWiiExtensionControllerType_Turntable);
SDL_COMPILE_TIME_ASSERT(wii_ext_taiko, SDL_WII_EXT_TAIKO == k_eWiiExtensionControllerType_Taiko);
SDL_COMPILE_TIME_ASSERT(wii_ext_udraw, SDL_WII_EXT_UDRAW == k_eWiiExtensionControllerType_UDraw);
SDL_COMPILE_TIME_ASSERT(wii_ext_drawsome, SDL_WII_EXT_DRAWSOME == k_eWiiExtensionControllerType_Drawsome);
SDL_COMPILE_TIME_ASSERT(wii_ext_shinkansen, SDL_WII_EXT_SHINKANSEN == k_eWiiExtensionControllerType_Shinkansen);
SDL_COMPILE_TIME_ASSERT(wii_ext_type_unknown, SDL_WII_EXT_JOYSTICK_TYPE_UNKNOWN == SDL_JOYSTICK_TYPE_UNKNOWN);
SDL_COMPILE_TIME_ASSERT(wii_ext_type_gamepad, SDL_WII_EXT_JOYSTICK_TYPE_GAMEPAD == SDL_JOYSTICK_TYPE_GAMEPAD);
SDL_COMPILE_TIME_ASSERT(wii_ext_type_guitar, SDL_WII_EXT_JOYSTICK_TYPE_GUITAR == SDL_JOYSTICK_TYPE_GUITAR);
SDL_COMPILE_TIME_ASSERT(wii_ext_type_drum_kit, SDL_WII_EXT_JOYSTICK_TYPE_DRUM_KIT == SDL_JOYSTICK_TYPE_DRUM_KIT);
SDL_COMPILE_TIME_ASSERT(wii_ext_south, SDL_WII_EXT_BUTTON_SOUTH == SDL_GAMEPAD_BUTTON_SOUTH);
SDL_COMPILE_TIME_ASSERT(wii_ext_east, SDL_WII_EXT_BUTTON_EAST == SDL_GAMEPAD_BUTTON_EAST);
SDL_COMPILE_TIME_ASSERT(wii_ext_west, SDL_WII_EXT_BUTTON_WEST == SDL_GAMEPAD_BUTTON_WEST);
SDL_COMPILE_TIME_ASSERT(wii_ext_north, SDL_WII_EXT_BUTTON_NORTH == SDL_GAMEPAD_BUTTON_NORTH);
SDL_COMPILE_TIME_ASSERT(wii_ext_back, SDL_WII_EXT_BUTTON_BACK == SDL_GAMEPAD_BUTTON_BACK);
SDL_COMPILE_TIME_ASSERT(wii_ext_start, SDL_WII_EXT_BUTTON_START == SDL_GAMEPAD_BUTTON_START);
SDL_COMPILE_TIME_ASSERT(wii_ext_left_stick, SDL_WII_EXT_BUTTON_LEFT_STICK == SDL_GAMEPAD_BUTTON_LEFT_STICK);
SDL_COMPILE_TIME_ASSERT(wii_ext_right_stick, SDL_WII_EXT_BUTTON_RIGHT_STICK == SDL_GAMEPAD_BUTTON_RIGHT_STICK);
SDL_COMPILE_TIME_ASSERT(wii_ext_left_shoulder, SDL_WII_EXT_BUTTON_LEFT_SHOULDER == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
SDL_COMPILE_TIME_ASSERT(wii_ext_right_shoulder, SDL_WII_EXT_BUTTON_RIGHT_SHOULDER == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
SDL_COMPILE_TIME_ASSERT(wii_ext_turntable_buttons, SDL_WII_EXT_BUTTON_TURNTABLE_LEFT_GREEN == k_eWiiButtons_Max);
SDL_COMPILE_TIME_ASSERT(wii_ext_leftx, SDL_WII_EXT_AXIS_LEFTX == SDL_GAMEPAD_AXIS_LEFTX);
SDL_COMPILE_TIME_ASSERT(wii_ext_lefty, SDL_WII_EXT_AXIS_LEFTY == SDL_GAMEPAD_AXIS_LEFTY);
SDL_COMPILE_TIME_ASSERT(wii_ext_rightx, SDL_WII_EXT_AXIS_RIGHTX == SDL_GAMEPAD_AXIS_RIGHTX);
SDL_COMPILE_TIME_ASSERT(wii_ext_righty, SDL_WII_EXT_AXIS_RIGHTY == SDL_GAMEPAD_AXIS_RIGHTY);
SDL_COMPILE_TIME_ASSERT(wii_ext_left_trigger, SDL_WII_EXT_AXIS_LEFT_TRIGGER == SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
SDL_COMPILE_TIME_ASSERT(wii_ext_right_trigger, SDL_WII_EXT_AXIS_RIGHT_TRIGGER == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
SDL_COMPILE_TIME_ASSERT(wii_ext_hat_up, SDL_WII_EXT_HAT_UP == SDL_HAT_UP);
SDL_COMPILE_TIME_ASSERT(wii_ext_hat_right, SDL_WII_EXT_HAT_RIGHT == SDL_HAT_RIGHT);
SDL_COMPILE_TIME_ASSERT(wii_ext_hat_down, SDL_WII_EXT_HAT_DOWN == SDL_HAT_DOWN);
SDL_COMPILE_TIME_ASSERT(wii_ext_hat_left, SDL_WII_EXT_HAT_LEFT == SDL_HAT_LEFT);
SDL_COMPILE_TIME_ASSERT(wii_ext_write_request, SDL_WII_EXT_WRITE_REQUEST_SIZE == k_unWiiPacketDataLength);

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
    Uint64 m_ulNextIRStart;                    // ticks of the next camera start after one that failed or did not hold, 0 for none
    Uint64 m_ulIRCheck;                        // ticks of the status request that checks a camera start, 0 for none
    int m_nIRStartFailures;                    // camera starts that failed or did not hold since the camera last held
    EWiiWriteStep m_eWriteStep;                // the write waiting for its acknowledgment, or none
    Uint64 m_ulWriteSent;                      // ticks it was last sent
    int m_nWriteSends;                         // times the camera write was sent, or the Motion Plus writes still to acknowledge
    bool m_bIRStartPending;                    // a camera start waiting for its turn
    EWiiMotionPlusWrite m_eMotionPlusWrite;    // a Motion Plus write waiting for its turn
    Uint8 m_ucMotionPlusWriteMode;             // the mode that activation writes
    bool m_bBalanceBoardCalibrationValid;
    Uint8 m_rgucBalanceBoardCalibration[24];   // Kg0/Kg17/Kg34 x 4 corners x int16, big-endian
    Uint8 m_rgucReadBuffer[k_unWiiPacketDataLength];
    Uint64 m_ulLastInput;
    Uint64 m_ulLastStatus;
    Uint64 m_ulNextMotionPlusCheck;
    bool m_bDisconnected;
    Uint64 m_ulNextConnect;      // without a joystick, input prompts no status request before this (hifihedgehog/SDL#47)
    int m_nUnansweredIdentifies; // identifies in a row that no reply answered, which lengthen that wait

    StickCalibrationData m_StickCalibrationData[6];
    SDL_WiiExtState m_ExtState; // The extensions SDL_hidapi_wii_ext_proto.c decodes
    int m_nReadSize;            // Length of the report in m_rgucReadBuffer
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
    if (size > 0) {
        ctx->m_nReadSize = size;
    }
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
        // The error names the report for the log lines that report a failed write
        if (SDL_hid_write(ctx->device->dev, data, size) < 0) {
            return SDL_SetError("Couldn't write output report 0x%.2x", data[0]);
        }
        return true;
    } else {
        // Use the rumble thread for general asynchronous writes
        if (!SDL_HIDAPI_LockRumble()) {
            return false;
        }
        return SDL_HIDAPI_SendRumbleAndUnlock(ctx->device, data, size) >= 0;
    }
}

static bool RequestStatus(SDL_DriverWii_Context *ctx, bool sync)
{
    Uint8 data[SDL_WII_EXT_STATUS_REQUEST_SIZE];

    SDL_WiiExt_BuildStatusRequest(data, ctx->m_bRumbleActive);
    return WriteOutput(ctx, data, sizeof(data), sync);
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
    Uint8 writeRequest[SDL_WII_EXT_WRITE_REQUEST_SIZE];

    SDL_assert(size > 0 && size <= 16);
    if (!SDL_WiiExt_BuildWriteRequest(writeRequest, ctx->m_bRumbleActive, address, data, (size_t)size)) {
        return false;
    }

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
    Uint8 readRequest[SDL_WII_EXT_READ_REQUEST_SIZE];

    SDL_assert(size > 0 && size <= 0xffff);
    SDL_WiiExt_BuildReadRequest(readRequest, ctx->m_bRumbleActive, address, (Uint16)size);

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

/* All six ID bytes at 0xA400FA, as Linux (hid-wiimote-core.c) and Dolphin's
   real-Wiimote backend read them. The guitar, drum kit and turntable share
   bytes 4-5 and differ in byte 0, which the old 2-byte read at 0xFE missed. */
static bool SendExtensionIdentify(SDL_DriverWii_Context *ctx, bool sync)
{
    return ReadRegister(ctx, SDL_WII_EXT_REG_IDENTIFY, 6, sync);
}

/* An ACTIVE Motion Plus blocks i2c passthrough entirely (Dolphin
   MotionPlus.cpp:204-210), so the child behind it is identifiable only from
   the M+'s stored copy of the child ID: 0xF6 holds ID byte 4, 0xF7 the
   challenge state, 0xF8 ID byte 0, 0xF9 ID byte 5 (Dolphin MotionPlus.h
   register map). Dolphin's real-Wiimote backend reads these four bytes at the
   active address and maps the child from bytes (0xF8, 0xF6, 0xF9)
   (WiimoteController.cpp:541-564). Our 16-bit extension IDs are full-ID bytes
   4-5, so the stored pair is (data[0] << 8) | data[3], and data[2] is ID byte
   0, which tells the guitar, drum kit and turntable apart. */
static bool ReadStoredChildExtensionID(SDL_DriverWii_Context *ctx, Uint16 *extension, Uint8 *id0)
{
    Uint8 i0, i4, i5;

    if (!ReadRegister(ctx, SDL_WII_EXT_REG_STORED_ID, 4, true)) {
        return false;
    }
    /* A read error, a short read or another reply: the register is
       unreadable right now */
    if (!SDL_WiiExt_ParseStoredIdReply(ctx->m_rgucReadBuffer, (size_t)ctx->m_nReadSize, &i0, &i4, &i5)) {
        return false;
    }
    *extension = (Uint16)((i4 << 8) | i5);
    *id0 = i0;
    return true;
}

/* The reply to a 6-byte identify at 0xA400FA or 0xA600FA: both echo address
   00 FA. The 16-bit value is ID bytes 4-5, which every Motion Plus test uses,
   and id0 is ID byte 0. */
static bool ParseExtensionIdentifyResponse(SDL_DriverWii_Context *ctx, Uint16 *extension, Uint8 *id0)
{
    Uint8 id[6];

    switch (SDL_WiiExt_ParseIdentifyReply(ctx->m_rgucReadBuffer, (size_t)ctx->m_nReadSize, SDL_WII_EXT_REG_IDENTIFY, id)) {
    case SDL_WII_EXT_REPLY_OK:
        *extension = (Uint16)((id[4] << 8) | id[5]);
        if (id0) {
            *id0 = id[0];
        }
        return true;
    case SDL_WII_EXT_REPLY_ABSENT:
        // The extension memory isn't mapped
        *extension = WII_EXTENSION_NONE;
        if (id0) {
            *id0 = 0;
        }
        return true;
    default:
        SDL_SetError("Unexpected extension identify response");
        return false;
    }
}

static EWiiExtensionControllerType GetExtensionType(Uint16 extension_id, Uint8 id0)
{
    if (extension_id == WII_EXTENSION_NONE) {
        return k_eWiiExtensionControllerType_None;
    }
    /* The Nunchuk, Classic, Wii U Pro and Balance Board classify on bytes
       4-5 as before. The extensions the module decodes classify only while
       their hint is on, and as unknown otherwise, as before. */
    return (EWiiExtensionControllerType)SDL_WiiExt_Classify(id0, (Uint8)(extension_id >> 8), (Uint8)extension_id,
                                                            SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_WII_EXTENSIONS, true));
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

/* The next start of a camera whose start failed or did not hold, at a growing
   delay (hifihedgehog/SDL#39). Dolphin's RunTasks repeats its camera steps on
   every pass until each lands (WiimoteController.cpp:434-438, :946-1018). The
   delay spaces out the starts of a camera that never answers. */
static void ScheduleIRStart(SDL_DriverWii_Context *ctx)
{
    const int last = (int)SDL_arraysize(k_rgunIRStartDelaysMS) - 1;

    ctx->m_ulNextIRStart = SDL_GetTicks() + k_rgunIRStartDelaysMS[SDL_min(ctx->m_nIRStartFailures, last)];
    if (ctx->m_nIRStartFailures < last) {
        ++ctx->m_nIRStartFailures;
    }
}

// The output report a step's acknowledgment names
static Uint8 GetWriteStepReport(EWiiWriteStep step)
{
    switch (step) {
    case k_eWiiWriteStep_None:
        return 0;
    case k_eWiiWriteStep_CameraEnable:
        return k_eWiiOutputReportIDs_IRCameraEnable;
    case k_eWiiWriteStep_CameraEnable2:
        return k_eWiiOutputReportIDs_IRCameraEnable2;
    default:
        return k_eWiiOutputReportIDs_WriteMemory;
    }
}

static bool IsCameraStep(EWiiWriteStep step)
{
    return step >= k_eWiiWriteStep_CameraEnable;
}

// The camera register a step writes, for the log, or 0 for an output report
static Uint32 GetWriteStepRegister(EWiiWriteStep step)
{
    switch (step) {
    case k_eWiiWriteStep_CameraControl:
    case k_eWiiWriteStep_CameraControl2:
        return WII_IR_REGISTER_CONTROL;
    case k_eWiiWriteStep_CameraSensitivity1:
        return WII_IR_REGISTER_SENSITIVITY1;
    case k_eWiiWriteStep_CameraSensitivity2:
        return WII_IR_REGISTER_SENSITIVITY2;
    case k_eWiiWriteStep_CameraMode:
        return WII_IR_REGISTER_MODE;
    default:
        return 0;
    }
}

/* Sends the camera step the machine waits on, again on each resend. The
   steps are WiimoteLib-Trihy's EnableIR (Wiimote.cs:1137-1183, registers at
   :57-60): power the camera (reports 0x13 and 0x1a), write the
   maximum-sensitivity blocks, select the data mode. A bare remote uses
   Extended mode (report 0x33). A remote with an extension uses Basic mode
   (report 0x37, which carries IR and the extension together, WiimoteLib
   IRExtensionAccel + EnableIR(Basic), Wiimote.cs:386-390/1034). Byte 1 of
   reports 0x13 and 0x1a carries the rumble bit, which the driver also drives,
   and asks for an acknowledgment, bit 1, as Dolphin's and Linux's do
   (WiimoteController.cpp:950-962, WiimoteReport.h:46-53,
   hid-wiimote-modules.c:640-661). The camera answers on its bus only once
   0x13 has enabled it (Dolphin EmuSubroutines.cpp:348-356), so each step goes
   after the one before it is acknowledged, as Linux waits for each
   (hid-wiimote-modules.c:640-706). All go through the rumble thread, so none
   waits here. */
static void SendCameraStep(SDL_DriverWii_Context *ctx, Uint64 now)
{
    static const Uint8 sensitivity1[9] = { 0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x90, 0x00, 0x41 };
    static const Uint8 sensitivity2[2] = { 0x40, 0x00 };
    Uint8 data[2];
    Uint8 value;

    ++ctx->m_nWriteSends;
    ctx->m_ulWriteSent = now;
    switch (ctx->m_eWriteStep) {
    case k_eWiiWriteStep_CameraEnable:
    case k_eWiiWriteStep_CameraEnable2:
        data[0] = GetWriteStepReport(ctx->m_eWriteStep);
        data[1] = (Uint8)(0x04 | 0x02 | (Uint8)ctx->m_bRumbleActive);
        WriteOutput(ctx, data, sizeof(data), false);
        break;
    case k_eWiiWriteStep_CameraControl:
    case k_eWiiWriteStep_CameraControl2:
        value = 0x08;
        WriteRegister(ctx, WII_IR_REGISTER_CONTROL, &value, sizeof(value), false);
        break;
    case k_eWiiWriteStep_CameraSensitivity1:
        WriteRegister(ctx, WII_IR_REGISTER_SENSITIVITY1, sensitivity1, sizeof(sensitivity1), false);
        break;
    case k_eWiiWriteStep_CameraSensitivity2:
        WriteRegister(ctx, WII_IR_REGISTER_SENSITIVITY2, sensitivity2, sizeof(sensitivity2), false);
        break;
    case k_eWiiWriteStep_CameraMode:
        /* Extended only for a bare remote without Motion Plus (report 0x33,
           12 IR bytes). With an extension OR an active Motion Plus the report
           must be 0x37, whose span carries 10-byte Basic IR (Dolphin's
           real-Wiimote backend uses Basic for the same reason,
           WiimoteController.cpp:991: "it's all that fits"). */
        value = (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_None &&
                 !ctx->m_bMotionPlusPresent) ? WII_IR_MODE_EXTENDED : WII_IR_MODE_BASIC;
        WriteRegister(ctx, WII_IR_REGISTER_MODE, &value, sizeof(value), false);
        break;
    default:
        break;
    }
}

static void StartCameraStep(SDL_DriverWii_Context *ctx, EWiiWriteStep step, Uint64 now)
{
    ctx->m_eWriteStep = step;
    ctx->m_nWriteSends = 0;
    SendCameraStep(ctx, now);
}

/* Reads size bytes, 1 to 16, at address in the registers or in EEPROM and
   waits for the reply. Only a clean ReadMemory reply counts: type 0x21, SE
   naming the size and no error, and the echoed address (bytes 4-5) matching
   the low 16 bits of the request (SDL_WiiExt_ParseMemoryReply), so an error
   packet is never published as data. ReadInputSync matches on report ID
   only, and a late reply to an earlier read can come first: a reply that
   echoes another address is passed over, up to four of them. */
static bool ReadMemoryBlock(SDL_DriverWii_Context *ctx, bool eeprom, Uint32 address, int size, Uint8 *dest)
{
    Uint8 request[SDL_WII_EXT_READ_REQUEST_SIZE];
    int replies;

    SDL_assert(size > 0 && size <= 16);
    if (eeprom) {
        SDL_WiiExt_BuildEepromReadRequest(request, ctx->m_bRumbleActive, address, (Uint16)size);
    } else {
        SDL_WiiExt_BuildReadRequest(request, ctx->m_bRumbleActive, address, (Uint16)size);
    }
    if (!WriteOutput(ctx, request, sizeof(request), true)) {
        return false;
    }
    for (replies = 0; replies < 4; ++replies) {
        if (!ReadInputSync(ctx, k_eWiiInputReportIDs_ReadMemory, NULL)) {
            return false;
        }
        if (ctx->m_nReadSize >= 6 &&
            ctx->m_rgucReadBuffer[4] == (Uint8)(address >> 8) && ctx->m_rgucReadBuffer[5] == (Uint8)address) {
            return SDL_WiiExt_ParseMemoryReply(ctx->m_rgucReadBuffer, (size_t)ctx->m_nReadSize, address, (size_t)size, dest);
        }
    }
    return false;
}

/* Read the 24-byte Balance Board calibration (Kg0/Kg17/Kg34 x 4 corners x int16
   big-endian) from register 0x04a40024 (WiimoteLib-Trihy Wiimote.cs:65,531-547).
   A read waits on one ReadMemory packet of <= 16 bytes, so the 24 bytes are
   read in two passes (16 + 8). Stored raw; PadForge does its own kg interpolation. */
static void ReadBalanceBoardCalibration(SDL_DriverWii_Context *ctx)
{
    ctx->m_bBalanceBoardCalibrationValid =
        ReadMemoryBlock(ctx, false, WII_BALANCE_CALIBRATION_ADDR, 16, ctx->m_rgucBalanceBoardCalibration) &&
        ReadMemoryBlock(ctx, false, WII_BALANCE_CALIBRATION_ADDR + 16, 8, ctx->m_rgucBalanceBoardCalibration + 16);
}

/* A remote through the Mayflash DolphinBar has no serial: the bar reports
   none for any slot and hands out its slots in no fixed order. The factory
   calibration in the remote's EEPROM, the camera's and the accelerometer's,
   is the only per-unit data a remote gives over HID (Dolphin WiimoteEmu.h
   UsableEEPROMData), so it becomes the serial, as the PS4, PS5 and Switch
   drivers pass HIDAPI_SetDeviceSerial what the controller reports. It is
   read at each connection, since another remote can sync into the
   slot. A device whose serial came from the system, the Bluetooth address,
   keeps it, and a read that fails leaves the device with no serial
   (hifihedgehog/SDL#47). */
static void UpdateCalibrationSerial(SDL_DriverWii_Context *ctx)
{
    SDL_HIDAPI_Device *device = ctx->device;
    Uint8 ir[SDL_WII_EXT_EEPROM_IR_CALIBRATION_SIZE];
    Uint8 accel[SDL_WII_EXT_EEPROM_ACCEL_CALIBRATION_SIZE];
    char serial[SDL_WII_EXT_CALIBRATION_SERIAL_SIZE];

    if (device->serial && *device->serial &&
        SDL_strncmp(device->serial, SDL_WII_EXT_CALIBRATION_SERIAL_PREFIX, SDL_WII_EXT_CALIBRATION_SERIAL_PREFIX_LEN) != 0) {
        return;
    }
    if (device->serial) {
        // The serial of the remote that was in the slot before
        SDL_free(device->serial);
        device->serial = NULL;
    }
    if (!ReadMemoryBlock(ctx, true, SDL_WII_EXT_EEPROM_IR_CALIBRATION, (int)sizeof(ir), ir) ||
        !ReadMemoryBlock(ctx, true, SDL_WII_EXT_EEPROM_ACCEL_CALIBRATION, (int)sizeof(accel), accel)) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: The Wii Remote at %s gave no calibration, so it has no serial", device->path);
        return;
    }
    SDL_WiiExt_FormatCalibrationSerial(serial, ir, accel);
    HIDAPI_SetDeviceSerial(device, serial);
}

static bool GetMotionPlusState(SDL_DriverWii_Context *ctx, bool *connected, Uint8 *mode)
{
    Uint16 extension;

    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_WiiUPro) {
        // The Wii U Pro controller never has the Motion Plus extension
        if (connected) {
            *connected = false;
        }
        if (mode) {
            *mode = 0;
        }
        return true;
    }

    if (SendExtensionIdentify(ctx, true) &&
        ParseExtensionIdentifyResponse(ctx, &extension, NULL)) {
        if ((extension & WII_EXTENSION_MOTIONPLUS_MASK) == WII_EXTENSION_MOTIONPLUS_ID) {
            // Motion Plus is currently active
            if (connected) {
                *connected = true;
            }
            if (mode) {
                *mode = (Uint8)(extension >> 8);
            }
            return true;
        }
    }

    if (ReadRegister(ctx, SDL_WII_EXT_REG_MOTIONPLUS_PROBE, 6, true) &&
        ParseExtensionIdentifyResponse(ctx, &extension, NULL)) {
        // Determined: connected exactly when the inactive M+ answered its ID
        if (connected) {
            *connected = ((extension & WII_EXTENSION_MOTIONPLUS_MASK) == WII_EXTENSION_MOTIONPLUS_ID);
        }
        if (mode) {
            *mode = 0;
        }
        return true;
    }

    /* Total read failure: leave the outputs untouched so a caller that
       stores them unconditionally keeps its last known-good state instead
       of a zeroed one (hifihedgehog/SDL#13). */
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

/* Writes the Motion Plus mode, or its deactivation, and records the result.
   Returns false when there was nothing to write. The identify path writes
   synchronously, since it reads the port right after, and waits for the
   deactivation's status report. Every other write goes through PumpWrites
   and waits for nothing (hifihedgehog/SDL#40). */
static bool WriteMotionPlus(SDL_DriverWii_Context *ctx, EWiiMotionPlusWrite write, Uint8 mode, bool sync)
{
    if (write == k_eWiiMotionPlusWrite_Activate) {
#ifdef SDL_PLATFORM_LINUX
        /* Linux drivers maintain a lot of state around the Motion Plus
         * extension, so don't mess with it here.
         */
        (void)mode;
        return false;
#else
        Uint32 address;

        /* Already running in the desired mode: rewriting it would only restart
           the M+'s ~20 ms reset for nothing. Dolphin changes the mode only when
           current differs from desired (WiimoteController.cpp:533-535). */
        if (ctx->m_ucMotionPlusMode == mode) {
            return false;
        }

        /* An ACTIVE M+ answers only at the active address and ignores writes to
           the inactive one, so a live mode switch must target 0xA4xxxx. Dolphin
           picks the address by activation state (WiimoteController.cpp:1056-1060). */
        address = (ctx->m_ucMotionPlusMode != WII_MOTIONPLUS_MODE_NONE) ? 0xA400FE : 0xA600FE;
        WriteRegister(ctx, address, &mode, sizeof(mode), sync);
        ctx->m_ucMotionPlusMode = mode;
#endif // LINUX
    } else {
        Uint8 data = 0x55;

        WriteRegister(ctx, 0xA400F0, &data, sizeof(data), sync);
        if (sync) {
            // Wait for the deactivation status message
            ReadInputSync(ctx, k_eWiiInputReportIDs_Status, NULL);
        }
        ctx->m_ucMotionPlusMode = WII_MOTIONPLUS_MODE_NONE;
    }

    /* Every mode write starts the M+'s ~20 ms reset, so stamp the settle
       window during which identify results are unreliable. */
    ctx->m_ulMotionPlusSettleDeadline = SDL_GetTicks() + WII_MOTIONPLUS_SETTLE_TIME_MS;
    return true;
}

/* Sends the writes waiting their turn (hifihedgehog/SDL#40). A Motion Plus
   write goes unless a camera write waits for its acknowledgment, since both
   are acknowledged as report 0x16. It can go beside other Motion Plus
   writes, whose acknowledgments come back in the order the remote took the
   writes. It goes before a camera start, in the order the sensor toggle has
   always sent them, so the camera's status check waits out the settle
   window the write opens (HandleWriteAck). A camera start goes when nothing
   waits, as Dolphin's RunTasks starts nothing while a report handler waits
   (WiimoteController.cpp:340-343, IsPerformingTask at :1685-1688). */
static void PumpWrites(SDL_DriverWii_Context *ctx)
{
    const Uint64 now = SDL_GetTicks();

    if (ctx->m_eMotionPlusWrite != k_eWiiMotionPlusWrite_None && !IsCameraStep(ctx->m_eWriteStep)) {
        const EWiiMotionPlusWrite write = ctx->m_eMotionPlusWrite;

        ctx->m_eMotionPlusWrite = k_eWiiMotionPlusWrite_None;
        if (WriteMotionPlus(ctx, write, ctx->m_ucMotionPlusWriteMode, false)) {
            if (ctx->m_eWriteStep == k_eWiiWriteStep_None) {
                ctx->m_eWriteStep = k_eWiiWriteStep_MotionPlus;
                ctx->m_nWriteSends = 0;
            }
            ++ctx->m_nWriteSends;
            ctx->m_ulWriteSent = now;
        }
    }
    if (ctx->m_eWriteStep == k_eWiiWriteStep_None && ctx->m_bIRStartPending) {
        ctx->m_bIRStartPending = false;
        StartCameraStep(ctx, k_eWiiWriteStep_CameraEnable, now);
    }
}

// Drops every write the machine holds, as when the joystick closes
static void ResetWrites(SDL_DriverWii_Context *ctx)
{
    ctx->m_eWriteStep = k_eWiiWriteStep_None;
    ctx->m_bIRStartPending = false;
    ctx->m_eMotionPlusWrite = k_eWiiMotionPlusWrite_None;
}

/* The mode write waits its turn behind a camera write the remote has not
   acknowledged yet, and a later request replaces one still waiting
   (hifihedgehog/SDL#40). */
static void ActivateMotionPlusWithMode(SDL_DriverWii_Context *ctx, Uint8 mode)
{
#ifdef SDL_PLATFORM_LINUX
    // WriteMotionPlus leaves the Motion Plus to the kernel driver here
    (void)ctx;
    (void)mode;
#else
    ctx->m_eMotionPlusWrite = k_eWiiMotionPlusWrite_Activate;
    ctx->m_ucMotionPlusWriteMode = mode;
    PumpWrites(ctx);
#endif
}

static void ActivateMotionPlus(SDL_DriverWii_Context *ctx)
{
    Uint8 mode = WII_MOTIONPLUS_MODE_STANDARD;

    /* The extensions the module decodes never run behind an active Motion
       Plus: passthrough drops or moves bits they use, halves their report
       rate, and cannot carry the Shinkansen's 8 bytes at all. */
    if (!SDL_WiiExt_AllowsMotionPlus(ctx->m_eExtensionControllerType)) {
        return;
    }

    // Pick the pass-through mode based on the connected controller
    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Nunchuk) {
        mode = WII_MOTIONPLUS_MODE_NUNCHUK;
    } else if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Gamepad) {
        mode = WII_MOTIONPLUS_MODE_GAMEPAD;
    }
    ActivateMotionPlusWithMode(ctx, mode);
}

/* The deactivation waits its turn the same way, and DisableIR, which the
   sensor toggle calls next, stops a camera start, so the write is on its way
   when the toggle returns, as when it waited for its status report. That
   report now reaches HandleStatus, which asks for the data report again. */
static void DeactivateMotionPlus(SDL_DriverWii_Context *ctx)
{
    ctx->m_eMotionPlusWrite = k_eWiiMotionPlusWrite_Deactivate;
    PumpWrites(ctx);
}

/* Arms the camera start (hifihedgehog/SDL#40). The update loop sends its
   writes one at a time, each after the one before it is acknowledged, the way
   Dolphin queues each camera step behind the last one's acknowledgment
   (WiimoteController.cpp:946-1018) and the Joy-Con camera's machine in
   SDL_hidapi_switch.c takes a step per acknowledgment. Nothing here waits on
   the remote, and reports keep reaching the joystick meanwhile. m_bIRActive
   is set when the last write is acknowledged. */
static void EnableIR(SDL_DriverWii_Context *ctx)
{
    ctx->m_ulNextIRStart = 0;
    ctx->m_bIRStartPending = true;
    PumpWrites(ctx);
}

/* Stops a start under way, turns the camera off, and lets a Motion Plus
   write waiting behind the start go */
static void DisableIR(SDL_DriverWii_Context *ctx)
{
    Uint8 data[2];

    ctx->m_bIRStartPending = false;
    if (IsCameraStep(ctx->m_eWriteStep)) {
        ctx->m_eWriteStep = k_eWiiWriteStep_None;
    }

    data[0] = k_eWiiOutputReportIDs_IRCameraEnable;
    data[1] = (Uint8)(0x00 | (Uint8)ctx->m_bRumbleActive);
    WriteOutput(ctx, data, sizeof(data), false);

    data[0] = k_eWiiOutputReportIDs_IRCameraEnable2;
    data[1] = (Uint8)(0x00 | (Uint8)ctx->m_bRumbleActive);
    WriteOutput(ctx, data, sizeof(data), false);

    ctx->m_bIRActive = false;
    ctx->m_ulNextIRStart = 0;
    ctx->m_ulIRCheck = 0;
    PumpWrites(ctx);
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
        if (SDL_WiiExt_IsDecodedType(ctx->m_eExtensionControllerType)) {
            // 0x32, or 0x35 for the guitar's tilt while sensors are on
            return (EWiiInputReportIDs)SDL_WiiExt_ReportMode(ctx->m_eExtensionControllerType, ctx->m_bReportSensors);
        }
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
    Uint8 data[SDL_WII_EXT_MODE_REQUEST_SIZE];

    // Continuous reporting on, byte 1 bit 2, with the rumble bit in bit 0
    SDL_COMPILE_TIME_ASSERT(continuous_reporting, ENABLE_CONTINUOUS_REPORTING);
    SDL_WiiExt_BuildModeRequest(data, ctx->m_bRumbleActive, (Uint8)type);
    return WriteOutput(ctx, data, sizeof(data), false);
}

static void ResetButtonPacketType(SDL_DriverWii_Context *ctx)
{
    RequestButtonPacketType(ctx, GetButtonPacketType(ctx));
}

/* A camera step the remote refused, error nonzero, or left unacknowledged
   after its last send ends the start: logged with the register or report and
   the error, then retried on the schedule (hifihedgehog/SDL#39). */
static void FailIRStart(SDL_DriverWii_Context *ctx, Uint8 error)
{
    const Uint32 address = GetWriteStepRegister(ctx->m_eWriteStep);
    const unsigned int report = GetWriteStepReport(ctx->m_eWriteStep);

    if (address && error) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: IR camera register 0x%.6x not written: Write memory failed: %u", (unsigned int)address, (unsigned int)error);
    } else if (address) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: IR camera register 0x%.6x not written: no acknowledgment after %d sends", (unsigned int)address, ctx->m_nWriteSends);
    } else if (error) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: IR camera report 0x%.2x refused: error %u", report, (unsigned int)error);
    } else {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: IR camera report 0x%.2x not acknowledged after %d sends", report, ctx->m_nWriteSends);
    }
    ctx->m_eWriteStep = k_eWiiWriteStep_None;
    ScheduleIRStart(ctx);
    PumpWrites(ctx);
}

/* An acknowledgment, report 0x22, that names the report of the write the
   machine waits on moves it on (hifihedgehog/SDL#40), as Dolphin's handler
   takes only the acknowledgment for its own report (MakeAckHandler,
   WiimoteController.cpp:1672-1683) and Linux's only the one for its pending
   request, after posting the buttons it carries (handler_return,
   hid-wiimote-core.c:1479-1492). Any other report is left to the caller.
   Once the last camera write lands, the camera is configured: the IR report
   is selected, and the camera is checked with a status request that waits out
   a Motion Plus mode write, whose pulsed status reports can predate the start
   (HandleStatus). A Motion Plus write that waited behind the start goes
   first, so the check waits out its window too. */
static bool HandleWriteAck(SDL_DriverWii_Context *ctx)
{
    const EWiiWriteStep step = ctx->m_eWriteStep;
    Uint8 error;

    if (step == k_eWiiWriteStep_None || ctx->m_nReadSize < 5 ||
        ctx->m_rgucReadBuffer[3] != GetWriteStepReport(step)) {
        return false;
    }
    error = ctx->m_rgucReadBuffer[4];
    if (step == k_eWiiWriteStep_MotionPlus) {
        /* The mode stays recorded either way, as when the write waited, and
           the Motion Plus check verifies it. The step ends with the
           acknowledgment of its last write. */
        if (error) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Motion Plus write refused: error %u", (unsigned int)error);
        }
        if (--ctx->m_nWriteSends <= 0) {
            ctx->m_eWriteStep = k_eWiiWriteStep_None;
            PumpWrites(ctx);
        }
    } else if (error) {
        FailIRStart(ctx, error);
    } else if (step == k_eWiiWriteStep_CameraControl2) {
        ctx->m_eWriteStep = k_eWiiWriteStep_None;
        ctx->m_bIRActive = true;
        ctx->m_ulNextIRStart = 0;
        ResetButtonPacketType(ctx);
        PumpWrites(ctx);
        ctx->m_ulIRCheck = SDL_max(SDL_GetTicks(), ctx->m_ulMotionPlusSettleDeadline);
    } else {
        StartCameraStep(ctx, (EWiiWriteStep)(step + 1), SDL_GetTicks());
    }
    return true;
}

/* A write still without its acknowledgment after the patience: a camera step
   goes again, up to the send limit, and then the start fails. A Motion Plus
   write is sent once, as it was when it waited, since the Motion Plus check
   verifies it. The patience starts once the rumble thread has sent every
   queued write, since ReadInput reads nothing before that. */
static void UpdateWrites(SDL_DriverWii_Context *ctx, Uint64 now)
{
    if (ctx->m_eWriteStep == k_eWiiWriteStep_None) {
        return;
    }
    if (SDL_GetAtomicInt(&ctx->device->rumble_pending) > 0) {
        ctx->m_ulWriteSent = now;
        return;
    }
    if (now < ctx->m_ulWriteSent + WII_WRITE_PATIENCE_MS) {
        return;
    }
    if (ctx->m_eWriteStep == k_eWiiWriteStep_MotionPlus) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Motion Plus write not acknowledged");
        ctx->m_eWriteStep = k_eWiiWriteStep_None;
        PumpWrites(ctx);
    } else if (ctx->m_nWriteSends < WII_WRITE_MAX_SENDS) {
        SendCameraStep(ctx, now);
    } else {
        FailIRStart(ctx, 0);
    }
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
    // The decoded extensions start from scratch: stick seeds, whammy, drum pads
    SDL_WiiExt_Reset(&ctx->m_ExtState, ctx->m_eExtensionControllerType);
}

static bool WriteStartupRegister(void *userdata, Uint32 address, Uint8 value)
{
    return WriteRegister((SDL_DriverWii_Context *)userdata, address, &value, sizeof(value), true);
}

/* Writes a decoded extension needs after the standard init, each after the
   previous one's acknowledge. Only the Drawsome has any, and it streams no pen
   data until they land, so a missing acknowledge leaves it unenabled. */
static void SendExtensionStartup(SDL_DriverWii_Context *ctx)
{
    if (SDL_WiiExt_IsDecodedType(ctx->m_eExtensionControllerType)) {
        ctx->m_ExtState.enabled = SDL_WiiExt_RunStartup(ctx->m_eExtensionControllerType, WriteStartupRegister, ctx);
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
    SendExtensionStartup(ctx);
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

/* Identifies the extension into *type. Returns false when no attempt got a
   reply: no remote answers at the path, as at an empty Mayflash DolphinBar
   slot. A remote that replies with an ID the driver does not know is
   Unknown (hifihedgehog/SDL#47). */
static bool ReadExtensionControllerType(SDL_HIDAPI_Device *device, EWiiExtensionControllerType *type)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    EWiiExtensionControllerType eExtensionControllerType = k_eWiiExtensionControllerType_Unknown;
    const int MAX_ATTEMPTS = 20;
    int attempts = 0;
    bool replied = false;

    // Create enough of a context to read the controller type from the device
    for (attempts = 0; attempts < MAX_ATTEMPTS; ++attempts) {
        Uint16 extension;
        Uint8 id0 = 0;
        if (!SendExtensionIdentify(ctx, true)) {
            continue;
        }
        replied = true;
        if (ParseExtensionIdentifyResponse(ctx, &extension, &id0)) {
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
                Uint8 stored_id0 = 0;
                bool stored_ok;

                if (!ctx->m_bMotionPlusChildConnected) {
                    /* No child, per the live passthrough flag: Dolphin
                       derives child absence from the flag alone and does no
                       bus work for it ("With no passthrough extension we'll
                       be happy with the current mode",
                       WiimoteController.cpp:1369-1371, :709-720). Conclude
                       None with zero further sync I/O and leave the M+
                       running (hifihedgehog/SDL#13 R4). */
                    eExtensionControllerType = k_eWiiExtensionControllerType_None;
                    ctx->m_ucMotionPlusMode = motion_plus_mode;
                    ctx->m_bMotionPlusPresent = true;
                    break;
                }

                stored_ok = ReadStoredChildExtensionID(ctx, &stored, &stored_id0);
                if (!stored_ok) {
                    /* One retry: Dolphin's response to a failed stored-ID
                       read is retry, never deactivate
                       (WiimoteController.cpp:549-551). */
                    stored_ok = ReadStoredChildExtensionID(ctx, &stored, &stored_id0);
                }
                if (stored_ok) {
                    EWiiExtensionControllerType stored_type = GetExtensionType(stored, stored_id0);
                    if (SDL_WiiExt_AllowsMotionPlus(stored_type)) {
                        eExtensionControllerType = stored_type;
                        ctx->m_ucMotionPlusMode = motion_plus_mode;
                        /* The port just answered with an active M+, the
                           strongest presence evidence there is */
                        ctx->m_bMotionPlusPresent = true;
                        break;
                    }
                    /* An extension the module decodes never rides in
                       passthrough: take the fallback below, which leaves
                       the M+ inactive and the child answering directly. */
                }

                /* Fallback (the child flag is set but the stored read failed
                   twice, or the child is one the module decodes):
                   deactivate the M+, consuming its status pulse so the dead
                   window passes, re-init the now-visible port, and identify
                   with retries while it settles. Error 7 here means
                   "settling", not "removed", for at least 50 ms. */
                WriteMotionPlus(ctx, k_eWiiMotionPlusWrite_Deactivate, 0, true);
                SendExtensionReset(ctx, true);
                {
                    const Uint64 retry_deadline = SDL_GetTicks() + 50;
                    for (;;) {
                        extension = WII_EXTENSION_NONE;
                        if (SendExtensionIdentify(ctx, true) &&
                            ParseExtensionIdentifyResponse(ctx, &extension, &id0) &&
                            extension != WII_EXTENSION_NONE) {
                            break;
                        }
                        if (SDL_GetTicks() >= retry_deadline) {
                            break;
                        }
                        SDL_Delay(5);
                    }
                }
                eExtensionControllerType = GetExtensionType(extension, id0);

                /* Restore the Motion Plus to the mode it was running, unless
                   the child is one that never rides in passthrough */
                if (SDL_WiiExt_AllowsMotionPlus(eExtensionControllerType)) {
                    WriteMotionPlus(ctx, k_eWiiMotionPlusWrite_Activate, motion_plus_mode, true);
                }
                ctx->m_bMotionPlusPresent = true;
                break;
            }

            if (extension == WII_EXTENSION_UNINITIALIZED) {
                /* Uninitialized child with the M+ inactive: the 0xf0 reset
                   write passes through as plain child init (Dolphin
                   MotionPlus.cpp:227-244, "The M+ deactivation signal is
                   cleverly the same as EXT initialization"). */
                SendExtensionReset(ctx, true);
                if (SendExtensionIdentify(ctx, true)) {
                    ParseExtensionIdentifyResponse(ctx, &extension, &id0);
                }
            }

            eExtensionControllerType = GetExtensionType(extension, id0);
            break;
        }
    }
    *type = eExtensionControllerType;
    return replied;
}

/* No identify got a reply, so the path has no remote. Input from it prompts
   no status request for 3 s, twice that for each further identify in a row
   that goes unanswered, up to 48 s, so a path that takes writes and answers
   no read rarely costs the update the 20 waits of an identify
   (hifihedgehog/SDL#47). */
static void WaitAfterUnansweredIdentify(SDL_DriverWii_Context *ctx, int unanswered)
{
    unanswered = SDL_min(unanswered, 5);
    ctx->m_nUnansweredIdentifies = unanswered;
    ctx->m_ulNextConnect = SDL_GetTicks() + ((Uint64)INPUT_WAIT_TIMEOUT_MS << (unanswered - 1));
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Nothing answered the extension identify at %s", ctx->device->path);
}

static void UpdateDeviceIdentity(SDL_HIDAPI_Device *device)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;

    HIDAPI_SetDeviceName(device, SDL_WiiExt_TypeName(ctx->m_eExtensionControllerType));
    /* A guitar or drum kit is typed as one, as the PS3 driver types its
       guitars and kits. Every configuration that existed before stays a
       gamepad. */
    device->joystick_type = (SDL_JoystickType)SDL_WiiExt_JoystickType(ctx->m_eExtensionControllerType);
    device->guid.data[15] = ctx->m_eExtensionControllerType;
}

/* Identifies the remote and adds its joystick, from a fresh context. A
   remote that answers again after it refused a write has connected again,
   which starts its camera, Motion Plus and data report over, and the
   joystick that had its sensors enabled is gone (hifihedgehog/SDL#39). A
   path where no identify gets a reply gets no joystick and waits in
   UpdateWithoutJoystick, and a remote with no system serial gets one from
   its calibration before its joystick is added (hifihedgehog/SDL#47). */
static bool ConnectRemote(SDL_HIDAPI_Device *device)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    const int unanswered = ctx->m_nUnansweredIdentifies;
    EWiiExtensionControllerType type;

    SDL_zerop(ctx);
    ctx->device = device;
    // The no-input grace period starts now, as at InitDevice
    ctx->m_ulLastInput = SDL_GetTicks();

    if (device->vendor_id == USB_VENDOR_NINTENDO) {
        if (!ReadExtensionControllerType(device, &type)) {
            WaitAfterUnansweredIdentify(ctx, unanswered + 1);
            return true;
        }
        ctx->m_eExtensionControllerType = type;

        UpdateDeviceIdentity(device);
        UpdateCalibrationSerial(ctx);
    }
    return HIDAPI_JoystickConnected(device, NULL);
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

    /* Windows keeps listing a Wii Remote that powered off, and the Mayflash
       DolphinBar lists its empty slots. Only a write tells either from a
       remote, since it fails (Dolphin IOWin.cpp:428-449, :724-727). A path
       that refuses a status request gets no joystick until its remote
       answers (UpdateWithoutJoystick, hifihedgehog/SDL#39), and input from
       it prompts no request for 3 s (hifihedgehog/SDL#47). */
    if (!RequestStatus(ctx, true)) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: No Wii Remote answers at %s: %s", device->path, SDL_GetError());
        ctx->m_ulNextConnect = ctx->m_ulLastInput + INPUT_WAIT_TIMEOUT_MS;
        return true;
    }
    return ConnectRemote(device);
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
    SDL_WiiExtCaps ext_caps;

    SDL_AssertJoysticksLocked();

    ctx->joystick = joystick;

    if (SDL_GetTicks() < ctx->m_ulMotionPlusSettleDeadline) {
        /* A Motion Plus mode write is still settling: the port reset would
           deactivate the M+ again, and a probe inside the ~20 ms dead window
           would misread error 7 as "not present" and re-arm the churn loop
           (hifihedgehog/SDL#13). The paths that stamp this deadline leave
           m_bMotionPlusPresent and m_ucMotionPlusMode recorded, so keep them
           and verify once the M+ has settled. Dolphin bars all extension
           work until the M+ settles (WiimoteController.cpp:1476-1480). */
        InitStickCalibrationData(ctx);
        SendExtensionStartup(ctx);
        ResetButtonPacketType(ctx);

        /* The sensors-on mode resync still applies on a settle-gated reopen:
           free when the stamp matches (equal-mode guard), and a lost write
           is repaired by the armed check below. */
        if (ctx->m_bReportSensors && ctx->m_bMotionPlusPresent) {
            ActivateMotionPlus(ctx);
        }
        ctx->m_ulNextMotionPlusCheck = ctx->m_ulMotionPlusSettleDeadline;
    } else {
        bool probe_ok;

        InitializeExtension(ctx);

        probe_ok = GetMotionPlusState(ctx, &ctx->m_bMotionPlusPresent, &ctx->m_ucMotionPlusMode);

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
        if (!probe_ok) {
            /* Total probe failure: the preserved state is unverified, so
               leave verification armed regardless of the mode gate, the same
               principle the settle-gated branch applies
               (hifihedgehog/SDL#13 R2). */
            Uint64 now = SDL_GetTicks();
            ctx->m_ulNextMotionPlusCheck = SDL_max(now, ctx->m_ulMotionPlusSettleDeadline);
        }
    }

    /* If sensors were left enabled across an extension hot-plug that cleared
       m_bIRActive, re-power the IR camera so the pointer resumes without the app
       re-toggling sensors. EnableIR arms the start, and the IR report follows
       once the remote acknowledges its last write (hifihedgehog/SDL#40). Motion
       Plus does not gate this: IR and the M+ interleave share report 0x37
       (hifihedgehog/SDL#11). Placed after the M+ presence state is established
       (probed on a normal open, preserved by the settle gate on a gated reopen)
       so the start picks the right camera mode. */
    if (WiiRemoteHasIRCamera(ctx->m_eExtensionControllerType) &&
        ctx->m_bReportSensors && !ctx->m_bIRActive) {
        EnableIR(ctx);
        ResetButtonPacketType(ctx);
    }

    /* The remote's accelerometer, and the gyro of a Motion Plus, serve every
       configuration with a camera. Enabling a sensor is the only way the camera
       powers on, and SDL passes that request only for a registered sensor, so
       the Classic Controller needs one too (hifihedgehog/SDL#36). With a Motion
       Plus present, enabling sensors runs the Classic in the M+'s Classic
       passthrough mode, as Dolphin's real-Wiimote backend does
       (WiimoteController.cpp:731-737), so its gyro is registered as well. */
    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_None ||
        ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Nunchuk ||
        ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Gamepad) {
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 100.0f);
        if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Nunchuk) {
            SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL_L, 100.0f);
        }

        if (ctx->m_bMotionPlusPresent) {
            SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, 100.0f);
        }
    }
    if (SDL_WiiExt_GetCaps(ctx->m_eExtensionControllerType, &ext_caps) && ext_caps.accel) {
        // The guitar's tilt is the remote's accelerometer. No gyro: the M+ stays inactive.
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 100.0f);
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
    if (SDL_WiiExt_GetCaps(ctx->m_eExtensionControllerType, &ext_caps)) {
        /* The decoded extensions: their controls at the gamepad positions
           (26-31 for the turntable's split buttons), the remote's raw block
           at 15-25 as in the Classic configuration, no IR */
        joystick->nbuttons = ext_caps.nbuttons;
        joystick->naxes = ext_caps.naxes;
        joystick->nhats = ext_caps.nhats;
        ctx->m_ulLastInput = SDL_GetTicks();
        return true;
    }
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

static Uint32 HIDAPI_DriverWii_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    SDL_WiiExtCaps ext_caps;
    Uint32 result = SDL_JOYSTICK_CAP_RUMBLE;

    if (SDL_WiiExt_GetCaps(ctx->m_eExtensionControllerType, &ext_caps) && ext_caps.mono_led) {
        // The turntable's Euphoria LED, as the Switch driver offers the HOME LED
        result |= SDL_JOYSTICK_CAP_MONO_LED;
    }
    return result;
}

static bool HIDAPI_DriverWii_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    SDL_WiiExtCaps ext_caps;

    if (SDL_WiiExt_GetCaps(ctx->m_eExtensionControllerType, &ext_caps) && ext_caps.mono_led) {
        Uint8 request[SDL_WII_EXT_WRITE_REQUEST_SIZE];

        // Any color lights it, all zero turns it off
        SDL_WiiExt_BuildLEDWrite(request, ctx->m_bRumbleActive, (red || green || blue));
        return WriteOutput(ctx, request, sizeof(request), false);
    }
    return SDL_Unsupported();
}

static bool HIDAPI_DriverWii_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;

    if (enabled != ctx->m_bReportSensors) {
        ctx->m_bReportSensors = enabled;

        /* The decoded extensions keep the M+ inactive, so there is nothing
           to activate and nothing to deactivate: a deactivation write would
           reach the child itself. */
        if (ctx->m_bMotionPlusPresent && SDL_WiiExt_AllowsMotionPlus(ctx->m_eExtensionControllerType)) {
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
           used (hifihedgehog/SDL#11). The Motion Plus write above goes before the
           camera's writes, so the start sees the current M+ state, and neither
           waits on the remote here (hifihedgehog/SDL#40). Either change starts
           the camera's retry delays over. */
        if (WiiRemoteHasIRCamera(ctx->m_eExtensionControllerType)) {
            ctx->m_nIRStartFailures = 0;
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

    /* A status report or a report in a mode without the accelerometer
       carries no sample */
    if (!ctx->m_bReportSensors || !data->hasAccelerometer) {
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

    {
        /* Data axes bypass the analog anti-jitter gate: seed each with its
           first real value so a dot appearing in the low camera range, or
           the -1 lost-dot sentinel, is never withheld inside the connect
           band (hifihedgehog/SDL#14). No-op after the first post. */
        const Sint16 v0x = dot0_found ? (Sint16)dot0_x : (Sint16)-1;
        const Sint16 v0y = dot0_found ? (Sint16)dot0_y : (Sint16)-1;
        const Sint16 v1x = dot1_found ? (Sint16)dot1_x : (Sint16)-1;
        const Sint16 v1y = dot1_found ? (Sint16)dot1_y : (Sint16)-1;

        SDL_SeedJoystickDataAxis(joystick, WII_IR_AXIS_DOT0_X, v0x);
        SDL_SeedJoystickDataAxis(joystick, WII_IR_AXIS_DOT0_Y, v0y);
        SDL_SeedJoystickDataAxis(joystick, WII_IR_AXIS_DOT1_X, v1x);
        SDL_SeedJoystickDataAxis(joystick, WII_IR_AXIS_DOT1_Y, v1y);

        SDL_SendJoystickAxis(ctx->timestamp, joystick, WII_IR_AXIS_DOT0_X, v0x);
        SDL_SendJoystickAxis(ctx->timestamp, joystick, WII_IR_AXIS_DOT0_Y, v0y);
        SDL_SendJoystickAxis(ctx->timestamp, joystick, WII_IR_AXIS_DOT1_X, v1x);
        SDL_SendJoystickAxis(ctx->timestamp, joystick, WII_IR_AXIS_DOT1_Y, v1y);
    }
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

    /* The corner cells are exact data on the stick-axis numbers, so they get
       the same anti-jitter bypass as the other data axes: 409 withheld raw
       counts is on the order of kilograms against a typical unit's 17 kg
       calibration span (hifihedgehog/SDL#14). */
    SDL_SeedJoystickDataAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, top_left);
    SDL_SeedJoystickDataAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, bottom_left);
    SDL_SeedJoystickDataAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX, top_right);
    SDL_SeedJoystickDataAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTY, bottom_right);

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

    /* Motion Plus frames always have byte 5 bit 0 clear (Dolphin
       MotionPlus.h:109, MotionPlus.cpp:645-646). A frame with it set is
       invalid, as during a hotplug, or is the extension's own frame from a
       Motion Plus that did not take its activation write, as a Nunchuk's with
       Z released (Nunchuk.h:44-45). Its extension bytes are dropped, and the
       remote's buttons and accelerometer, which the report carries outside
       them, still post (hifihedgehog/SDL#39). */
    if (ctx->m_ucMotionPlusMode != WII_MOTIONPLUS_MODE_NONE &&
        data->ucNExtensionBytes > 5 && (data->rgucExtension[5] & 0x01)) {
        data->ucNExtensionBytes = 0;
    }

    if (ctx->m_ucMotionPlusMode != WII_MOTIONPLUS_MODE_NONE &&
        data->ucNExtensionBytes > 5) {
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

    /* Bit 3 of the flags byte is the camera, which report 0x13 turns on
       (Dolphin WiimoteReport.h:198-202, EmuSubroutines.cpp:348-356). A camera
       found off while sensors are on starts again, as Dolphin resets its camera
       state on the same bit (WiimoteController.cpp:1698-1699). Inside a Motion
       Plus settle window the report can be one of its pulses from before the
       camera start, so it does not count (hifihedgehog/SDL#39). This comes
       before the data report request below, which must not select IR for a
       camera that is off. */
    if (ctx->m_bReportSensors && ctx->m_bIRActive &&
        WiiRemoteHasIRCamera(ctx->m_eExtensionControllerType) &&
        SDL_GetTicks() >= ctx->m_ulMotionPlusSettleDeadline) {
        if (ctx->m_rgucReadBuffer[3] & 0x08) {
            ctx->m_nIRStartFailures = 0;
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Status shows the IR camera off, starting it again");
            ctx->m_bIRActive = false;
            ScheduleIRStart(ctx);
        }
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

    } else if (hadExtension != hasExtension && ctx->m_eWriteStep != k_eWiiWriteStep_MotionPlus) {
        /* Reinitialize to get new state. Not while a Motion Plus write waits
           for its acknowledgment: a status report the remote sent before it
           took a deactivation still counts the active Motion Plus as an
           extension, and the deactivation's own status report follows the
           acknowledgment (hifihedgehog/SDL#40). */
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

    // The acknowledgment a queued write waits on (hifihedgehog/SDL#40)
    if (type == k_eWiiInputReportIDs_Acknowledge && HandleWriteAck(ctx)) {
        return;
    }

    switch (ctx->m_eCommState) {
    case k_eWiiCommunicationState_None:
        break;

    case k_eWiiCommunicationState_CheckMotionPlusStage1:
    case k_eWiiCommunicationState_CheckMotionPlusStage2:
    {
        Uint16 extension = 0;
        if (ParseExtensionIdentifyResponse(ctx, &extension, NULL)) {
            if ((extension & WII_EXTENSION_MOTIONPLUS_MASK) == WII_EXTENSION_MOTIONPLUS_ID) {
                bool stage1 = (ctx->m_eCommState == k_eWiiCommunicationState_CheckMotionPlusStage1);
                Uint8 running_mode = (Uint8)(extension >> 8);

                // Motion Plus is currently active
                SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Motion Plus CONNECTED (stage %d)", stage1 ? 1 : 2);

                if (stage1 &&
                    running_mode != WII_MOTIONPLUS_MODE_STANDARD &&
                    running_mode != WII_MOTIONPLUS_MODE_NUNCHUK &&
                    running_mode != WII_MOTIONPLUS_MODE_GAMEPAD) {
                    /* A stage 1 request went to the active address, and an
                       active M+ always reports its running mode, so a zero
                       high byte here is by construction a mis-attributed
                       dormant-M+ reply (both identify replies echo address
                       0x00FA), and any other unknown value is garbage.
                       Neither is adoptable: doing so would poison the mode
                       while M+ frames stream (hifihedgehog/SDL#13 R3). Leave
                       the state alone and retry shortly. */
                    ctx->m_ulNextMotionPlusCheck = SDL_max(SDL_GetTicks() + 500, ctx->m_ulMotionPlusSettleDeadline);
                    ctx->m_eCommState = k_eWiiCommunicationState_None;

                } else {
                    /* A successful M+ ID read is the state source, never a
                       disconnect (Dolphin WiimoteController.cpp:451-456).
                       Adopt the presence and the running mode from the reply
                       (the ID's high byte carries it, 0 when dormant at
                       stage 2). The teardown stays only for the genuine
                       capability change: an M+ that newly appeared, so the
                       sensor set needs rebuilding. */
                    bool was_present = ctx->m_bMotionPlusPresent;

                    if (!was_present) {
                        // Reinitialize to get new sensor availability
                        ctx->m_bDisconnected = true;
                    }
                    ctx->m_bMotionPlusPresent = true;
                    ctx->m_ucMotionPlusMode = stage1 ? running_mode : WII_MOTIONPLUS_MODE_NONE;

                    if (!stage1 && was_present && ctx->m_bReportSensors) {
                        /* The M+ is genuinely dormant while sensors are on
                           (a contact bounce or a lost activation write left
                           it powered down): reactivate it right here,
                           mirroring the resync blocks in OpenJoystick and
                           SetJoystickSensorsEnabled. The write stamps a
                           settle deadline the armed check verifies, so a
                           lost write becomes a bounded ~2 s retry loop
                           rather than a parked dead gyro
                           (hifihedgehog/SDL#13 R1). */
                        ActivateMotionPlus(ctx);
                        if (ctx->m_ucMotionPlusMode != WII_MOTIONPLUS_MODE_NONE ||
                            ctx->m_eMotionPlusWrite == k_eWiiMotionPlusWrite_Activate) {
                            /* The mode stamp is the evidence the write went
                               out, and an activation waiting behind a camera
                               write goes once that write is acknowledged
                               (hifihedgehog/SDL#40): verify it at the settle
                               deadline, floored to the future. Where the
                               activation is a deliberate no-op (Linux leaves
                               M+ state to the kernel driver) the mode stays
                               NONE, nothing waits, this arm is skipped, and
                               the re-arm below restores the 8 s periodic
                               watch instead of a tight retry cycle. */
                            ctx->m_ulNextMotionPlusCheck = SDL_max(SDL_GetTicks() + 500, ctx->m_ulMotionPlusSettleDeadline);
                        }
                    }
                    ctx->m_eCommState = k_eWiiCommunicationState_None;
                }

            } else if (ctx->m_eCommState == k_eWiiCommunicationState_CheckMotionPlusStage1) {
                // Check to see if Motion Plus is present
                ReadRegister(ctx, SDL_WII_EXT_REG_MOTIONPLUS_PROBE, 6, false);

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

                /* Neither register answered as M+ outside the settle window,
                   and an active M+ always answers its identify at the active
                   address, so NONE is the physical state. Adopt it so the
                   state that the total-failure path preserves stays truthful
                   and the equal-mode guard can never block a needed resync
                   (hifihedgehog/SDL#13 R2). */
                if (ctx->m_bMotionPlusPresent) {
                    // Reinitialize to get new sensor availability
                    ctx->m_bDisconnected = true;
                }
                ctx->m_bMotionPlusPresent = false;
                ctx->m_ucMotionPlusMode = WII_MOTIONPLUS_MODE_NONE;
                ctx->m_eCommState = k_eWiiCommunicationState_None;
            }

            /* Any resolution re-arms the watch it needs: the fire-time
               disarm used the pre-adoption mode, so an adoption landing in
               mode NONE would otherwise strand the machine without the
               dormant-M+ watch (hifihedgehog/SDL#13). */
            if (ctx->m_eCommState == k_eWiiCommunicationState_None &&
                !ctx->m_ulNextMotionPlusCheck &&
                NeedsPeriodicMotionPlusCheck(ctx, false)) {
                SchedulePeriodicMotionPlusCheck(ctx);
            }
        }
    } break;
    default:
        // Should never happen
        break;
    }
}

static void PostExtensionOutput(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, const SDL_WiiExtOutput *output)
{
    int i;

    for (i = 0; i < 32; ++i) {
        if (output->button_mask & (1u << i)) {
            SDL_SendJoystickButton(ctx->timestamp, joystick, (Uint8)i, ((output->buttons >> i) & 1) != 0);
        }
    }
    for (i = 0; i < SDL_WII_EXT_MAX_AXES; ++i) {
        if (output->axis_mask & (1u << i)) {
            if (output->data_axis_mask & (1u << i)) {
                /* Pen coordinates, lever bytes, drum velocities and the
                   turntable's crossfader and dial are data: seed past the
                   anti-jitter gate as the IR axes are */
                SDL_SeedJoystickDataAxis(joystick, (Uint8)i, output->axes[i]);
            }
            SDL_SendJoystickAxis(ctx->timestamp, joystick, (Uint8)i, output->axes[i]);
        }
    }
    if (output->has_hat) {
        SDL_SendJoystickHat(ctx->timestamp, joystick, 0, output->hat);
    }
}

/* A data report while a decoded extension is attached. A report shorter than
   its full length changes nothing, and only the span its mode defines is
   read. The remote's own buttons stay in the raw block. */
static void HandleExtensionPacket(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick)
{
    const Uint8 *report = ctx->m_rgucReadBuffer;
    const size_t length = (ctx->m_nReadSize > 0) ? (size_t)ctx->m_nReadSize : 0;
    SDL_WiiExtLayout layout;
    const Uint8 *span = NULL;
    size_t span_length = 0;
    SDL_WiiExtOutput output;
    WiiButtonData data;

    if (!SDL_WiiExt_GetLayout(report[0], &layout) || length < layout.length) {
        return;
    }
    SDL_zero(data);
    if (layout.buttons) {
        GetBaseButtons(&data, report + 1);
    }
    if (layout.accel) {
        GetAccelerometer(&data, report + 3);
    }
    HandleWiiRemoteButtonData(ctx, joystick, &data);
    HandleWiiRemoteAccelData(ctx, joystick, &data);

    if (SDL_WiiExt_GetSpan(report, length, &span, &span_length, NULL) &&
        SDL_WiiExt_Decode(&ctx->m_ExtState, span, span_length, &output)) {
        PostExtensionOutput(ctx, joystick, &output);
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

    if (SDL_WiiExt_IsDecodedType(ctx->m_eExtensionControllerType)) {
        HandleExtensionPacket(ctx, joystick);
        return;
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

/* A device without a joystick: its path refused a status request at
   InitDevice or after 3 s without input, or no identify got a reply. The
   device keeps its path and handle, so no other joystick backend lists the
   remote as its own: they ask HIDAPI_IsDevicePresent, which counts a device
   that has a driver (SDL_JoystickHandledByAnotherDriver). It gets a status
   request every 3 s, as Dolphin's scanner writes to every listed interface
   (IOWin.cpp:703-741, WiimoteReal.cpp:672-674). That request goes through
   the rumble thread, so a write that waits out the HID layer's timeout does
   not hold the update (hifihedgehog/SDL#39).

   Input from the path is no proof of a remote: an empty Mayflash DolphinBar
   slot can send some, and fails every write (Dolphin IOWin.cpp:428-438,
   :599). Input prompts a status request on this thread, and only a request
   the path takes brings the joystick back, as at InitDevice and in Dolphin's
   scanner (IOWin.cpp:440-449). Input that arrives while a refused request or
   an unanswered identify holds the path back is dropped
   (hifihedgehog/SDL#47). A read error means the handle broke: the core
   releases the device and opens its path again at the next device change
   (hifihedgehog/SDL#39). */
static bool UpdateWithoutJoystick(SDL_HIDAPI_Device *device)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    Uint64 now = SDL_GetTicks();
    bool input = false;
    int size;

    while ((size = ReadInput(ctx)) > 0) {
        input = true;
    }
    if (size < 0) {
        device->reads_ended = true;
        return false;
    }
    if (input && now >= ctx->m_ulNextConnect && SDL_GetAtomicInt(&device->rumble_pending) == 0) {
        if (RequestStatus(ctx, true)) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: The Wii Remote at %s answers again", device->path);
            return ConnectRemote(device);
        }
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Input from %s, and the status request failed: %s", device->path, SDL_GetError());
        ctx->m_ulNextConnect = now + INPUT_WAIT_TIMEOUT_MS;
        ctx->m_ulLastInput = now;
    }
    if (now >= (ctx->m_ulLastInput + INPUT_WAIT_TIMEOUT_MS) &&
        SDL_GetAtomicInt(&device->rumble_pending) == 0) {
        RequestStatus(ctx, false);
        ctx->m_ulLastInput = now;
    }
    return true;
}

static bool HIDAPI_DriverWii_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    bool refused = false;
    int size;
    Uint64 now;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    } else {
        return UpdateWithoutJoystick(device);
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
    if (size >= 0 && now >= (ctx->m_ulLastInput + INPUT_WAIT_TIMEOUT_MS) &&
        SDL_GetAtomicInt(&device->rumble_pending) == 0) {
        /* Bluetooth may have disconnected. A Wii Remote that powered off reads
           nothing on Windows, where only a write fails (Dolphin
           IOWin.cpp:428-449), so ask for its status. A remote that takes the
           request is quiet, not gone, and stays, and once the joystick is open
           the status report it sends restarts its data reports (HandleStatus).
           One that refuses it loses its joystick until it answers again
           (UpdateWithoutJoystick). A write still queued on the rumble thread
           goes first, as ReadInput waits for it too (hifihedgehog/SDL#39). */
        if (!RequestStatus(ctx, true)) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: No input for %d ms, and the status request failed: %s", INPUT_WAIT_TIMEOUT_MS, SDL_GetError());
            refused = true;
            size = -1;
            ctx->m_ulNextConnect = now + INPUT_WAIT_TIMEOUT_MS;
        }
        ctx->m_ulLastInput = now;
    }

    if (joystick) {
        // These checks aren't needed on the Wii U Pro Controller
        if (ctx->m_eExtensionControllerType != k_eWiiExtensionControllerType_WiiUPro) {

            // A queued write still without its acknowledgment (hifihedgehog/SDL#40)
            if (size >= 0) {
                UpdateWrites(ctx, now);
            }

            /* A camera start that failed, or whose status report showed the
               camera off, is armed again once its delay passes
               (hifihedgehog/SDL#39). It waits for a Motion Plus mode write to
               settle. An identify in flight no longer holds it back, since the
               start drops no report (hifihedgehog/SDL#40). */
            if (size >= 0 && ctx->m_ulNextIRStart && now >= ctx->m_ulNextIRStart &&
                now >= ctx->m_ulMotionPlusSettleDeadline) {
                ctx->m_ulNextIRStart = 0;
                if (ctx->m_bReportSensors && !ctx->m_bIRActive &&
                    WiiRemoteHasIRCamera(ctx->m_eExtensionControllerType)) {
                    EnableIR(ctx);
                }
            }

            // The status request that checks a camera start (EnableIR)
            if (size >= 0 && ctx->m_ulIRCheck && now >= ctx->m_ulIRCheck) {
                ctx->m_ulIRCheck = 0;
                RequestStatus(ctx, false);
                ctx->m_ulLastStatus = now;
            }

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
                Uint8 data[SDL_WII_EXT_STATUS_REQUEST_SIZE];

                SDL_WiiExt_BuildStatusRequest(data, ctx->m_bRumbleActive);
                WriteOutput(ctx, data, sizeof(data), false);

                ctx->m_ulLastStatus = now;
            }
        }
    }

    if (size < 0) {
        /* Read error, or a remote that refused a write: device is disconnected.
           A remote that refused the write keeps the device and its handle
           (UpdateWithoutJoystick). A read error means the handle broke: the
           core releases the device and opens its path again at the next
           device change (hifihedgehog/SDL#39). */
        HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        if (!refused) {
            device->reads_ended = true;
        }
    } else if (ctx->m_bDisconnected) {
        // An extension was hot-plugged (Nunchuk/Classic attach or detach, or a
        // Motion Plus state change). The device stays physically present, so the
        // HIDAPI core never tears it down and re-runs InitDevice. Re-identify in
        // place: drop the old joystick, re-read the extension type, refresh the
        // name/GUID, then re-add with the new capabilities. OpenJoystick re-applies
        // nbuttons/naxes and per-extension sensors when the app reopens it.
        EWiiExtensionControllerType type;
        bool replied;

        HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        replied = ReadExtensionControllerType(device, &type);
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
        if (replied) {
            ctx->m_eExtensionControllerType = type;
            UpdateDeviceIdentity(device);
            HIDAPI_JoystickConnected(device, NULL);
        } else {
            /* The remote left during the identify: no joystick, and the
               device waits in UpdateWithoutJoystick (hifihedgehog/SDL#47) */
            WaitAfterUnansweredIdentify(ctx, ctx->m_nUnansweredIdentifies + 1);
        }
    }
    return (size >= 0);
}

static void HIDAPI_DriverWii_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;

    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_WII_PLAYER_LED,
                        SDL_PlayerLEDHintChanged, ctx);

    /* A closed joystick reads no acknowledgment, so the queued writes stop,
       and OpenJoystick arms them again (hifihedgehog/SDL#40). The core
       closes an open joystick before the extension identify that follows a
       hot-plug (HIDAPI_JoystickDisconnected), so no acknowledgment that
       identify's synchronous reads take moves a write on. */
    ResetWrites(ctx);
    ctx->joystick = NULL;
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverWii = {
    .name = SDL_HINT_JOYSTICK_HIDAPI_WII,
    .enabled = true,
    .RegisterHints = HIDAPI_DriverWii_RegisterHints,
    .UnregisterHints = HIDAPI_DriverWii_UnregisterHints,
    .IsEnabled = HIDAPI_DriverWii_IsEnabled,
    .IsSupportedDevice = HIDAPI_DriverWii_IsSupportedDevice,
    .InitDevice = HIDAPI_DriverWii_InitDevice,
    .SetDevicePlayerIndex = HIDAPI_DriverWii_SetDevicePlayerIndex,
    .UpdateDevice = HIDAPI_DriverWii_UpdateDevice,
    .OpenJoystick = HIDAPI_DriverWii_OpenJoystick,
    .RumbleJoystick = HIDAPI_DriverWii_RumbleJoystick,
    .GetJoystickCapabilities = HIDAPI_DriverWii_GetJoystickCapabilities,
    .SetJoystickLED = HIDAPI_DriverWii_SetJoystickLED,
    .SetJoystickSensorsEnabled = HIDAPI_DriverWii_SetJoystickSensorsEnabled,
    .CloseJoystick = HIDAPI_DriverWii_CloseJoystick,
};

#endif // SDL_JOYSTICK_HIDAPI_WII

#endif // SDL_JOYSTICK_HIDAPI
