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
/* This driver supports the Nintendo Switch Pro controller.
   Code and logic contributed by Valve Corporation under the SDL zlib license.
*/
#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_HIDAPI

#include "../../SDL_hints_c.h"
#include "../SDL_sysjoystick.h"
#include "SDL_hidapijoystick_c.h"
#include "SDL_hidapi_rumble.h"
#include "SDL_hidapi_nintendo.h"

#ifdef SDL_JOYSTICK_HIDAPI_SWITCH

// Define this if you want to log all packets from the controller
#if 0
#define DEBUG_SWITCH_PROTOCOL
#endif

// Define this to get log output for rumble logic
// #define DEBUG_RUMBLE

/* The initialization sequence doesn't appear to work correctly on Windows unless
   the reads and writes are on the same thread.

   ... and now I can't reproduce this, so I'm leaving it in, but disabled for now.
 */
// #define SWITCH_SYNCHRONOUS_WRITES

/* How often you can write rumble commands to the controller.
   If you send commands more frequently than this, you can turn off the controller
   in Bluetooth mode, or the motors can miss the command in USB mode.
 */
#define RUMBLE_WRITE_FREQUENCY_MS 30

// How often you have to refresh a long duration rumble to keep the motors running
#define RUMBLE_REFRESH_FREQUENCY_MS 50

#define SWITCH_GYRO_SCALE  14.2842f
#define SWITCH_ACCEL_SCALE 4096.f

#define SWITCH_GYRO_SCALE_MULT    936.0f
#define SWITCH_ACCEL_SCALE_MULT   4.0f

enum
{
    SDL_GAMEPAD_BUTTON_SWITCH_SHARE = 11,
    SDL_GAMEPAD_BUTTON_SWITCH_RIGHT_PADDLE1,
    SDL_GAMEPAD_BUTTON_SWITCH_LEFT_PADDLE1,
    SDL_GAMEPAD_BUTTON_SWITCH_RIGHT_PADDLE2,
    SDL_GAMEPAD_BUTTON_SWITCH_LEFT_PADDLE2,
    SDL_GAMEPAD_NUM_SWITCH_BUTTONS,
};

enum
{
    SDL_GAMEPAD_BUTTON_SWITCH_INPUT_ONLY_SHARE = 11,
    SDL_GAMEPAD_NUM_SWITCH_INPUT_ONLY_BUTTONS,
};

enum
{
    SDL_GAMEPAD_BUTTON_SWITCH2_SHARE = 11,
    SDL_GAMEPAD_BUTTON_SWITCH2_C,
    SDL_GAMEPAD_NUM_SWITCH2_BUTTONS,
};

typedef enum
{
    k_eSwitchInputReportIDs_SubcommandReply = 0x21,
    k_eSwitchInputReportIDs_FullControllerState = 0x30,
    k_eSwitchInputReportIDs_FullControllerAndMcuState = 0x31,
    k_eSwitchInputReportIDs_SimpleControllerState = 0x3F,
    k_eSwitchInputReportIDs_CommandAck = 0x81,
} ESwitchInputReportIDs;

typedef enum
{
    k_eSwitchOutputReportIDs_RumbleAndSubcommand = 0x01,
    k_eSwitchOutputReportIDs_Rumble = 0x10,
    k_eSwitchOutputReportIDs_McuData = 0x11,
    k_eSwitchOutputReportIDs_Proprietary = 0x80,
} ESwitchOutputReportIDs;

typedef enum
{
    k_eSwitchSubcommandIDs_BluetoothManualPair = 0x01,
    k_eSwitchSubcommandIDs_RequestDeviceInfo = 0x02,
    k_eSwitchSubcommandIDs_SetInputReportMode = 0x03,
    k_eSwitchSubcommandIDs_SetHCIState = 0x06,
    k_eSwitchSubcommandIDs_SPIFlashRead = 0x10,
    k_eSwitchSubcommandIDs_SetMCUConfig = 0x21,
    k_eSwitchSubcommandIDs_SetMCUState = 0x22,
    k_eSwitchSubcommandIDs_SetPlayerLights = 0x30,
    k_eSwitchSubcommandIDs_SetHomeLight = 0x38,
    k_eSwitchSubcommandIDs_EnableIMU = 0x40,
    k_eSwitchSubcommandIDs_SetIMUSensitivity = 0x41,
    k_eSwitchSubcommandIDs_EnableVibration = 0x48,
} ESwitchSubcommandIDs;

typedef enum
{
    k_eSwitchProprietaryCommandIDs_Status = 0x01,
    k_eSwitchProprietaryCommandIDs_Handshake = 0x02,
    k_eSwitchProprietaryCommandIDs_HighSpeed = 0x03,
    k_eSwitchProprietaryCommandIDs_ForceUSB = 0x04,
    k_eSwitchProprietaryCommandIDs_ClearUSB = 0x05,
    k_eSwitchProprietaryCommandIDs_ResetMCU = 0x06,
} ESwitchProprietaryCommandIDs;

#define k_unSwitchOutputPacketDataLength 49
#define k_unSwitchMaxOutputPacketLength  64
/* The Bluetooth NFC/IR input report 0x31 is 362 bytes (dekuNukem
   bluetooth_hid_notes.md); the read buffer must hold it so the NFC tag
   payload (UID through roughly byte 75) is not truncated at the old 64-byte
   size. jc_toolkit reads NFC replies into a 0x170-byte buffer. */
#define k_unSwitchMaxInputPacketLength   368
#define k_unSwitchBluetoothPacketLength  k_unSwitchOutputPacketDataLength
#define k_unSwitchUSBPacketLength        k_unSwitchMaxOutputPacketLength

#define k_unSPIStickFactoryCalibrationStartOffset 0x603D
#define k_unSPIStickFactoryCalibrationEndOffset   0x604E
#define k_unSPIStickFactoryCalibrationLength      (k_unSPIStickFactoryCalibrationEndOffset - k_unSPIStickFactoryCalibrationStartOffset + 1)

#define k_unSPIStickUserCalibrationStartOffset 0x8010
#define k_unSPIStickUserCalibrationEndOffset   0x8025
#define k_unSPIStickUserCalibrationLength      (k_unSPIStickUserCalibrationEndOffset - k_unSPIStickUserCalibrationStartOffset + 1)

#define k_unSPIIMUScaleStartOffset 0x6020
#define k_unSPIIMUScaleEndOffset   0x6037
#define k_unSPIIMUScaleLength      (k_unSPIIMUScaleEndOffset - k_unSPIIMUScaleStartOffset + 1)

#define k_unSPIIMUUserScaleStartOffset 0x8026
#define k_unSPIIMUUserScaleEndOffset   0x8039
#define k_unSPIIMUUserScaleLength      (k_unSPIIMUUserScaleEndOffset - k_unSPIIMUUserScaleStartOffset + 1)

#pragma pack(1)
typedef struct
{
    Uint8 rgucButtons[2];
    Uint8 ucStickHat;
    Uint8 rgucJoystickLeft[2];
    Uint8 rgucJoystickRight[2];
} SwitchInputOnlyControllerStatePacket_t;

typedef struct
{
    Uint8 rgucButtons[2];
    Uint8 ucStickHat;
    Sint16 sJoystickLeft[2];
    Sint16 sJoystickRight[2];
} SwitchSimpleStatePacket_t;

typedef struct
{
    Uint8 ucCounter;
    Uint8 ucBatteryAndConnection;
    Uint8 rgucButtons[3];
    Uint8 rgucJoystickLeft[3];
    Uint8 rgucJoystickRight[3];
    Uint8 ucVibrationCode;
} SwitchControllerStatePacket_t;

typedef struct
{
    Sint16 sAccelX;
    Sint16 sAccelY;
    Sint16 sAccelZ;

    Sint16 sGyroX;
    Sint16 sGyroY;
    Sint16 sGyroZ;
} SwitchControllerIMUState_t;

typedef struct
{
    SwitchControllerStatePacket_t controllerState;
    SwitchControllerIMUState_t imuState[3];
} SwitchStatePacket_t;

typedef struct
{
    Uint32 unAddress;
    Uint8 ucLength;
} SwitchSPIOpData_t;

typedef struct
{
    SwitchControllerStatePacket_t m_controllerState;

    Uint8 ucSubcommandAck;
    Uint8 ucSubcommandID;

#define k_unSubcommandDataBytes 35
    union
    {
        Uint8 rgucSubcommandData[k_unSubcommandDataBytes];

        struct
        {
            SwitchSPIOpData_t opData;
            Uint8 rgucReadData[k_unSubcommandDataBytes - sizeof(SwitchSPIOpData_t)];
        } spiReadData;

        struct
        {
            Uint8 rgucFirmwareVersion[2];
            Uint8 ucDeviceType;
            Uint8 ucFiller1;
            Uint8 rgucMACAddress[6];
            Uint8 ucFiller2;
            Uint8 ucColorLocation;
        } deviceInfo;

        struct
        {
            SwitchSPIOpData_t opData;
            Uint8 rgucLeftCalibration[9];
            Uint8 rgucRightCalibration[9];
        } stickFactoryCalibration;

        struct
        {
            SwitchSPIOpData_t opData;
            Uint8 rgucLeftMagic[2];
            Uint8 rgucLeftCalibration[9];
            Uint8 rgucRightMagic[2];
            Uint8 rgucRightCalibration[9];
        } stickUserCalibration;
    };
} SwitchSubcommandInputPacket_t;

typedef struct
{
    Uint8 ucPacketType;
    Uint8 ucCommandID;
    Uint8 ucFiller;

    Uint8 ucDeviceType;
    Uint8 rgucMACAddress[6];
} SwitchProprietaryStatusPacket_t;

typedef struct
{
    Uint8 rgucData[4];
} SwitchRumbleData_t;

typedef struct
{
    Uint8 ucPacketType;
    Uint8 ucPacketNumber;
    SwitchRumbleData_t rumbleData[2];
} SwitchCommonOutputPacket_t;

typedef struct
{
    SwitchCommonOutputPacket_t commonData;

    Uint8 ucSubcommandID;
    Uint8 rgucSubcommandData[k_unSwitchOutputPacketDataLength - sizeof(SwitchCommonOutputPacket_t) - 1];
} SwitchSubcommandOutputPacket_t;

typedef struct
{
    Uint8 ucPacketType;
    Uint8 ucProprietaryID;

    Uint8 rgucProprietaryData[k_unSwitchOutputPacketDataLength - 1 - 1];
} SwitchProprietaryOutputPacket_t;
#pragma pack()

/* Enhanced report hint mode:
 * "0": enhanced features are never used
 * "1": enhanced features are always used
 * "auto": enhanced features are advertised to the application, but SDL doesn't touch the controller state unless the application explicitly requests it.
 */
typedef enum
{
    SWITCH_ENHANCED_REPORT_HINT_OFF,
    SWITCH_ENHANCED_REPORT_HINT_ON,
    SWITCH_ENHANCED_REPORT_HINT_AUTO
} HIDAPI_Switch_EnhancedReportHint;

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_Joystick *joystick;
    bool m_bInputOnly;
    bool m_bSwitch2;
    bool m_bUseButtonLabels;
    bool m_bPlayerLights;
    int m_nPlayerIndex;
    bool m_bSyncWrite;
    int m_nMaxWriteAttempts;
    ESwitchDeviceInfoControllerType m_eControllerType;
    Uint8 m_nInitialInputMode;
    Uint8 m_nCurrentInputMode;
    Uint8 m_rgucMACAddress[6];
    Uint8 m_nCommandNumber;
    HIDAPI_Switch_EnhancedReportHint m_eEnhancedReportHint;
    bool m_bEnhancedMode;
    bool m_bEnhancedModeAvailable;
    SwitchCommonOutputPacket_t m_RumblePacket;
    Uint8 m_rgucReadBuffer[k_unSwitchMaxInputPacketLength];
    // Frequency-shaped rumble (fork issue #25): last target intensities, for
    // the per-motor attack/decay edges
    Uint16 m_usShapedPrevLow;
    Uint16 m_usShapedPrevHigh;
    bool m_bRumbleActive;
    Uint64 m_ulRumbleSent;
    bool m_bRumblePending;
    bool m_bRumbleZeroPending;
    Uint32 m_unRumblePending;
    bool m_bSensorsSupported;
    bool m_bReportSensors;
    bool m_bIRSensorActive; // right Joy-Con NIR camera streaming (input report 0x31)
    Uint64 m_ulIRModeFixTicks; // last report-mode-watchdog re-init (externally knocked input mode)
    // NIR camera async bring-up (fork issue #24): one machine step per update tick
    Uint8 m_ucIRState;          // ESwitchIRState
    Uint8 m_ucIRRounds;         // command re-send rounds in the current state
    Uint8 m_ucIRFails;          // consecutive failed bring-ups (retry backoff)
    Uint64 m_ulIRActionTicks;   // last state-machine send (retry / cooldown pacing)
    Uint64 m_ulIRLastFragTicks; // last image fragment while streaming (stall watchdog)
    // NFC tag reading (fork issue #15): MCU-driven, async off the update loop
    bool m_bNfcActive;          // machine running: input mode 0x31 held, MCU coming up or polling
    Uint8 m_ucNfcState;         // ESwitchNfcState
    Uint8 m_ucNfcRounds;        // command re-send rounds in the current state
    bool m_bNfcTagPresent;      // a tag UID is currently published
    Uint64 m_ulNfcActionTicks;  // last state-machine send (retry / cooldown / pacing)
    Uint64 m_ulNfcLastMcuTicks; // last NFC-shaped MCU packet seen (stream watchdog)
    Uint64 m_ulNfcLastTagTicks; // last UID-bearing status seen (stream-death backstop)
    Uint8 m_ucNfcStatusMisses;  // consecutive failed presence reads while a tag is published
    Uint64 m_ulNfcMissTicks;    // last counted failure, one count per outstanding read
    Uint64 m_ulNfcReadTicks;    // last timer-driven solicitation (0x04 scanning / 0x06 presence read)
    Uint64 m_ulForceUSBTicks;   // last ForceUSB keepalive nudge (rate bound)
    bool m_bHasSensorData;
    Uint64 m_ulLastInput;
    Uint64 m_ulLastIMUReset;
    Uint64 m_ulIMUSampleTimestampNS;
    Uint32 m_unIMUSamples;
    Uint64 m_ulIMUUpdateIntervalNS;
    Uint64 m_ulTimestampNS;
    bool m_bVerticalMode;
    SDL_PowerState m_ePowerState;
    int m_nPowerPercent;

    SwitchInputOnlyControllerStatePacket_t m_lastInputOnlyState;
    SwitchSimpleStatePacket_t m_lastSimpleState;
    SwitchStatePacket_t m_lastFullState;

    struct StickCalibrationData
    {
        struct
        {
            Sint16 sCenter;
            Sint16 sMin;
            Sint16 sMax;
        } axis[2];
    } m_StickCalData[2];

    struct StickExtents
    {
        struct
        {
            Sint16 sMin;
            Sint16 sMax;
        } axis[2];
    } m_StickExtents[2], m_SimpleStickExtents[2];

    struct IMUScaleData
    {
        float fAccelScaleX;
        float fAccelScaleY;
        float fAccelScaleZ;

        float fGyroScaleX;
        float fGyroScaleY;
        float fGyroScaleZ;

        Sint16 sGyroOffsetX;
        Sint16 sGyroOffsetY;
        Sint16 sGyroOffsetZ;
    } m_IMUScaleData;
} SDL_DriverSwitch_Context;

static int ReadInput(SDL_DriverSwitch_Context *ctx)
{
    int result;

    // Make sure we don't try to read at the same time a write is happening
    if (SDL_GetAtomicInt(&ctx->device->rumble_pending) > 0) {
        return 0;
    }

    result = SDL_hid_read_timeout(ctx->device->dev, ctx->m_rgucReadBuffer, sizeof(ctx->m_rgucReadBuffer), 0);

    // See if we can guess the initial input mode
    if (result > 0 && !ctx->m_bInputOnly && !ctx->m_nInitialInputMode) {
        switch (ctx->m_rgucReadBuffer[0]) {
        case k_eSwitchInputReportIDs_FullControllerState:
        case k_eSwitchInputReportIDs_FullControllerAndMcuState:
        case k_eSwitchInputReportIDs_SimpleControllerState:
            ctx->m_nInitialInputMode = ctx->m_rgucReadBuffer[0];
            break;
        default:
            break;
        }
    }
    return result;
}

static int WriteOutput(SDL_DriverSwitch_Context *ctx, const Uint8 *data, int size)
{
#ifdef SWITCH_SYNCHRONOUS_WRITES
    return SDL_hid_write(ctx->device->dev, data, size);
#else
    // Use the rumble thread for general asynchronous writes
    if (!SDL_HIDAPI_LockRumble()) {
        return -1;
    }
    return SDL_HIDAPI_SendRumbleAndUnlock(ctx->device, data, size);
#endif // SWITCH_SYNCHRONOUS_WRITES
}

static SwitchSubcommandInputPacket_t *ReadSubcommandReply(SDL_DriverSwitch_Context *ctx, ESwitchSubcommandIDs expectedID, const Uint8 *pBuf, Uint8 ucLen)
{
    // Average response time for messages is ~30ms
    Uint64 endTicks = SDL_GetTicks() + 100;

    int nRead = 0;
    while ((nRead = ReadInput(ctx)) != -1) {
        if (nRead > 0) {
            if (ctx->m_rgucReadBuffer[0] == k_eSwitchInputReportIDs_SubcommandReply) {
                SwitchSubcommandInputPacket_t *reply = (SwitchSubcommandInputPacket_t *)&ctx->m_rgucReadBuffer[1];
                if (reply->ucSubcommandID != expectedID || !(reply->ucSubcommandAck & 0x80)) {
                    continue;
                }
                if (reply->ucSubcommandID == k_eSwitchSubcommandIDs_SPIFlashRead) {
                    SDL_assert(ucLen == sizeof(reply->spiReadData.opData));
                    if (SDL_memcmp(&reply->spiReadData.opData, pBuf, ucLen) != 0) {
                        // This was a reply for another SPI read command
                        continue;
                    }
                }
                return reply;
            }
        } else {
            SDL_Delay(1);
        }

        if (SDL_GetTicks() >= endTicks) {
            break;
        }
    }
    return NULL;
}

static bool ReadProprietaryReply(SDL_DriverSwitch_Context *ctx, ESwitchProprietaryCommandIDs expectedID)
{
    // Average response time for messages is ~30ms
    Uint64 endTicks = SDL_GetTicks() + 100;

    int nRead = 0;
    while ((nRead = ReadInput(ctx)) != -1) {
        if (nRead > 0) {
            if (ctx->m_rgucReadBuffer[0] == k_eSwitchInputReportIDs_CommandAck && ctx->m_rgucReadBuffer[1] == expectedID) {
                return true;
            }
        } else {
            SDL_Delay(1);
        }

        if (SDL_GetTicks() >= endTicks) {
            break;
        }
    }
    return false;
}

static void ConstructSubcommand(SDL_DriverSwitch_Context *ctx, ESwitchSubcommandIDs ucCommandID, const Uint8 *pBuf, Uint8 ucLen, SwitchSubcommandOutputPacket_t *outPacket)
{
    SDL_zerop(outPacket);

    outPacket->commonData.ucPacketType = k_eSwitchOutputReportIDs_RumbleAndSubcommand;
    outPacket->commonData.ucPacketNumber = ctx->m_nCommandNumber;

    SDL_memcpy(outPacket->commonData.rumbleData, ctx->m_RumblePacket.rumbleData, sizeof(ctx->m_RumblePacket.rumbleData));

    outPacket->ucSubcommandID = ucCommandID;
    if (pBuf) {
        SDL_memcpy(outPacket->rgucSubcommandData, pBuf, ucLen);
    }

    ctx->m_nCommandNumber = (ctx->m_nCommandNumber + 1) & 0xF;
}

static bool WritePacket(SDL_DriverSwitch_Context *ctx, void *pBuf, Uint8 ucLen)
{
    Uint8 rgucBuf[k_unSwitchMaxOutputPacketLength];
    const size_t unWriteSize = ctx->device->is_bluetooth ? k_unSwitchBluetoothPacketLength : k_unSwitchUSBPacketLength;

    if (ucLen > k_unSwitchOutputPacketDataLength) {
        return false;
    }

    if (ucLen < unWriteSize) {
        SDL_memcpy(rgucBuf, pBuf, ucLen);
        SDL_memset(rgucBuf + ucLen, 0, unWriteSize - ucLen);
        pBuf = rgucBuf;
        ucLen = (Uint8)unWriteSize;
    }
    if (ctx->m_bSyncWrite) {
        return SDL_hid_write(ctx->device->dev, (Uint8 *)pBuf, ucLen) >= 0;
    } else {
        return WriteOutput(ctx, (Uint8 *)pBuf, ucLen) >= 0;
    }
}

static bool WriteSubcommand(SDL_DriverSwitch_Context *ctx, ESwitchSubcommandIDs ucCommandID, const Uint8 *pBuf, Uint8 ucLen, SwitchSubcommandInputPacket_t **ppReply)
{
    SwitchSubcommandInputPacket_t *reply = NULL;
    int nTries;

    for (nTries = 1; !reply && nTries <= ctx->m_nMaxWriteAttempts; ++nTries) {
        SwitchSubcommandOutputPacket_t commandPacket;
        ConstructSubcommand(ctx, ucCommandID, pBuf, ucLen, &commandPacket);

        if (!WritePacket(ctx, &commandPacket, sizeof(commandPacket))) {
            continue;
        }

        reply = ReadSubcommandReply(ctx, ucCommandID, pBuf, ucLen);
    }

    if (ppReply) {
        *ppReply = reply;
    }
    return reply != NULL;
}

static bool WriteProprietary(SDL_DriverSwitch_Context *ctx, ESwitchProprietaryCommandIDs ucCommand, Uint8 *pBuf, Uint8 ucLen, bool waitForReply)
{
    int nTries;

    for (nTries = 1; nTries <= ctx->m_nMaxWriteAttempts; ++nTries) {
        SwitchProprietaryOutputPacket_t packet;

        if ((!pBuf && ucLen > 0) || ucLen > sizeof(packet.rgucProprietaryData)) {
            return false;
        }

        SDL_zero(packet);
        packet.ucPacketType = k_eSwitchOutputReportIDs_Proprietary;
        packet.ucProprietaryID = ucCommand;
        if (pBuf) {
            SDL_memcpy(packet.rgucProprietaryData, pBuf, ucLen);
        }

        if (!WritePacket(ctx, &packet, sizeof(packet))) {
            continue;
        }

        if (!waitForReply || ReadProprietaryReply(ctx, ucCommand)) {
            // SDL_Log("Succeeded%s after %d tries", ctx->m_bSyncWrite ? " (sync)" : "", nTries);
            return true;
        }
    }
    // SDL_Log("Failed%s after %d tries", ctx->m_bSyncWrite ? " (sync)" : "", nTries);
    return false;
}

static Uint8 EncodeRumbleHighAmplitude(Uint16 amplitude)
{
    /* More information about these values can be found here:
     * https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/rumble_data_table.md
     */
    Uint16 hfa[101][2] = { { 0, 0x0 }, { 514, 0x2 }, { 775, 0x4 }, { 921, 0x6 }, { 1096, 0x8 }, { 1303, 0x0a }, { 1550, 0x0c }, { 1843, 0x0e }, { 2192, 0x10 }, { 2606, 0x12 }, { 3100, 0x14 }, { 3686, 0x16 }, { 4383, 0x18 }, { 5213, 0x1a }, { 6199, 0x1c }, { 7372, 0x1e }, { 7698, 0x20 }, { 8039, 0x22 }, { 8395, 0x24 }, { 8767, 0x26 }, { 9155, 0x28 }, { 9560, 0x2a }, { 9984, 0x2c }, { 10426, 0x2e }, { 10887, 0x30 }, { 11369, 0x32 }, { 11873, 0x34 }, { 12398, 0x36 }, { 12947, 0x38 }, { 13520, 0x3a }, { 14119, 0x3c }, { 14744, 0x3e }, { 15067, 0x40 }, { 15397, 0x42 }, { 15734, 0x44 }, { 16079, 0x46 }, { 16431, 0x48 }, { 16790, 0x4a }, { 17158, 0x4c }, { 17534, 0x4e }, { 17918, 0x50 }, { 18310, 0x52 }, { 18711, 0x54 }, { 19121, 0x56 }, { 19540, 0x58 }, { 19967, 0x5a }, { 20405, 0x5c }, { 20851, 0x5e }, { 21308, 0x60 }, { 21775, 0x62 }, { 22251, 0x64 }, { 22739, 0x66 }, { 23236, 0x68 }, { 23745, 0x6a }, { 24265, 0x6c }, { 24797, 0x6e }, { 25340, 0x70 }, { 25894, 0x72 }, { 26462, 0x74 }, { 27041, 0x76 }, { 27633, 0x78 }, { 28238, 0x7a }, { 28856, 0x7c }, { 29488, 0x7e }, { 30134, 0x80 }, { 30794, 0x82 }, { 31468, 0x84 }, { 32157, 0x86 }, { 32861, 0x88 }, { 33581, 0x8a }, { 34316, 0x8c }, { 35068, 0x8e }, { 35836, 0x90 }, { 36620, 0x92 }, { 37422, 0x94 }, { 38242, 0x96 }, { 39079, 0x98 }, { 39935, 0x9a }, { 40809, 0x9c }, { 41703, 0x9e }, { 42616, 0xa0 }, { 43549, 0xa2 }, { 44503, 0xa4 }, { 45477, 0xa6 }, { 46473, 0xa8 }, { 47491, 0xaa }, { 48531, 0xac }, { 49593, 0xae }, { 50679, 0xb0 }, { 51789, 0xb2 }, { 52923, 0xb4 }, { 54082, 0xb6 }, { 55266, 0xb8 }, { 56476, 0xba }, { 57713, 0xbc }, { 58977, 0xbe }, { 60268, 0xc0 }, { 61588, 0xc2 }, { 62936, 0xc4 }, { 64315, 0xc6 }, { 65535, 0xc8 } };
    int index = 0;
    for (; index < 101; index++) {
        if (amplitude <= hfa[index][0]) {
            return (Uint8)hfa[index][1];
        }
    }
    return (Uint8)hfa[100][1];
}

static Uint16 EncodeRumbleLowAmplitude(Uint16 amplitude)
{
    /* More information about these values can be found here:
     * https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/rumble_data_table.md
     */
    Uint16 lfa[101][2] = { { 0, 0x0040 }, { 514, 0x8040 }, { 775, 0x0041 }, { 921, 0x8041 }, { 1096, 0x0042 }, { 1303, 0x8042 }, { 1550, 0x0043 }, { 1843, 0x8043 }, { 2192, 0x0044 }, { 2606, 0x8044 }, { 3100, 0x0045 }, { 3686, 0x8045 }, { 4383, 0x0046 }, { 5213, 0x8046 }, { 6199, 0x0047 }, { 7372, 0x8047 }, { 7698, 0x0048 }, { 8039, 0x8048 }, { 8395, 0x0049 }, { 8767, 0x8049 }, { 9155, 0x004a }, { 9560, 0x804a }, { 9984, 0x004b }, { 10426, 0x804b }, { 10887, 0x004c }, { 11369, 0x804c }, { 11873, 0x004d }, { 12398, 0x804d }, { 12947, 0x004e }, { 13520, 0x804e }, { 14119, 0x004f }, { 14744, 0x804f }, { 15067, 0x0050 }, { 15397, 0x8050 }, { 15734, 0x0051 }, { 16079, 0x8051 }, { 16431, 0x0052 }, { 16790, 0x8052 }, { 17158, 0x0053 }, { 17534, 0x8053 }, { 17918, 0x0054 }, { 18310, 0x8054 }, { 18711, 0x0055 }, { 19121, 0x8055 }, { 19540, 0x0056 }, { 19967, 0x8056 }, { 20405, 0x0057 }, { 20851, 0x8057 }, { 21308, 0x0058 }, { 21775, 0x8058 }, { 22251, 0x0059 }, { 22739, 0x8059 }, { 23236, 0x005a }, { 23745, 0x805a }, { 24265, 0x005b }, { 24797, 0x805b }, { 25340, 0x005c }, { 25894, 0x805c }, { 26462, 0x005d }, { 27041, 0x805d }, { 27633, 0x005e }, { 28238, 0x805e }, { 28856, 0x005f }, { 29488, 0x805f }, { 30134, 0x0060 }, { 30794, 0x8060 }, { 31468, 0x0061 }, { 32157, 0x8061 }, { 32861, 0x0062 }, { 33581, 0x8062 }, { 34316, 0x0063 }, { 35068, 0x8063 }, { 35836, 0x0064 }, { 36620, 0x8064 }, { 37422, 0x0065 }, { 38242, 0x8065 }, { 39079, 0x0066 }, { 39935, 0x8066 }, { 40809, 0x0067 }, { 41703, 0x8067 }, { 42616, 0x0068 }, { 43549, 0x8068 }, { 44503, 0x0069 }, { 45477, 0x8069 }, { 46473, 0x006a }, { 47491, 0x806a }, { 48531, 0x006b }, { 49593, 0x806b }, { 50679, 0x006c }, { 51789, 0x806c }, { 52923, 0x006d }, { 54082, 0x806d }, { 55266, 0x006e }, { 56476, 0x806e }, { 57713, 0x006f }, { 58977, 0x806f }, { 60268, 0x0070 }, { 61588, 0x8070 }, { 62936, 0x0071 }, { 64315, 0x8071 }, { 65535, 0x0072 } };
    int index = 0;
    for (; index < 101; index++) {
        if (amplitude <= lfa[index][0]) {
            return lfa[index][1];
        }
    }
    return lfa[100][1];
}

static void SetNeutralRumble(SDL_HIDAPI_Device *device, SwitchRumbleData_t *pRumble)
{
    bool bStandardNeutralValue;
    if (device->vendor_id == USB_VENDOR_NINTENDO &&
        device->product_id == USB_PRODUCT_NINTENDO_N64_CONTROLLER) {
        // The 8BitDo 64 Bluetooth Controller rumbles at startup with the standard neutral value,
        // so we'll use a 0 amplitude value instead.
        bStandardNeutralValue = false;
    } else {
        // The KingKong2 PRO Controller doesn't initialize correctly with a 0 amplitude value
        // over Bluetooth, so we'll use the standard value in all other cases.
        bStandardNeutralValue = true;
    }
    if (bStandardNeutralValue) {
        pRumble->rgucData[0] = 0x00;
        pRumble->rgucData[1] = 0x01;
        pRumble->rgucData[2] = 0x40;
        pRumble->rgucData[3] = 0x40;
    } else {
        pRumble->rgucData[0] = 0x00;
        pRumble->rgucData[1] = 0x00;
        pRumble->rgucData[2] = 0x01;
        pRumble->rgucData[3] = 0x40;
    }
}

static void EncodeRumble(SDL_HIDAPI_Device *device, SwitchRumbleData_t *pRumble, Uint16 usHighFreq, Uint8 ucHighFreqAmp, Uint8 ucLowFreq, Uint16 usLowFreqAmp)
{
    if (ucHighFreqAmp > 0 || usLowFreqAmp > 0) {
        // High-band frequency and low-band amplitude are actually nine-bits each so they
        // take a bit from the high-band amplitude and low-band frequency bytes respectively
        pRumble->rgucData[0] = usHighFreq & 0xFF;
        pRumble->rgucData[1] = ucHighFreqAmp | ((usHighFreq >> 8) & 0x01);

        pRumble->rgucData[2] = ucLowFreq | ((usLowFreqAmp >> 8) & 0x80);
        pRumble->rgucData[3] = usLowFreqAmp & 0xFF;

#ifdef DEBUG_RUMBLE
        SDL_Log("Freq: %.2X %.2X  %.2X, Amp: %.2X  %.2X %.2X",
                usHighFreq & 0xFF, ((usHighFreq >> 8) & 0x01), ucLowFreq,
                ucHighFreqAmp, ((usLowFreqAmp >> 8) & 0x80), usLowFreqAmp & 0xFF);
#endif
    } else {
        SetNeutralRumble(device, pRumble);
    }
}

static bool WriteRumble(SDL_DriverSwitch_Context *ctx)
{
    /* Write into m_RumblePacket rather than a temporary buffer to allow the current rumble state
     * to be retained for subsequent rumble or subcommand packets sent to the controller
     */
    ctx->m_RumblePacket.ucPacketType = k_eSwitchOutputReportIDs_Rumble;
    ctx->m_RumblePacket.ucPacketNumber = ctx->m_nCommandNumber;
    ctx->m_nCommandNumber = (ctx->m_nCommandNumber + 1) & 0xF;

    // Refresh the rumble state periodically
    ctx->m_ulRumbleSent = SDL_GetTicks();

    return WritePacket(ctx, (Uint8 *)&ctx->m_RumblePacket, sizeof(ctx->m_RumblePacket));
}

static ESwitchDeviceInfoControllerType CalculateControllerType(SDL_DriverSwitch_Context *ctx, ESwitchDeviceInfoControllerType eControllerType)
{
    SDL_HIDAPI_Device *device = ctx->device;

    // The N64 controller reports as a Pro controller over USB
    if (eControllerType == k_eSwitchDeviceInfoControllerType_ProController &&
        device->product_id == USB_PRODUCT_NINTENDO_N64_CONTROLLER) {
        eControllerType = k_eSwitchDeviceInfoControllerType_N64;
    }

    if (eControllerType == k_eSwitchDeviceInfoControllerType_Unknown) {
        // This might be a Joy-Con that's missing from a charging grip slot
        if (device->product_id == USB_PRODUCT_NINTENDO_SWITCH_JOYCON_GRIP) {
            if (device->interface_number == 1) {
                eControllerType = k_eSwitchDeviceInfoControllerType_JoyConLeft;
            } else {
                eControllerType = k_eSwitchDeviceInfoControllerType_JoyConRight;
            }
        }
    }
    return eControllerType;
}

static bool BReadDeviceInfo(SDL_DriverSwitch_Context *ctx)
{
    SwitchSubcommandInputPacket_t *reply = NULL;

    if (ctx->device->is_bluetooth) {
        if (WriteSubcommand(ctx, k_eSwitchSubcommandIDs_RequestDeviceInfo, NULL, 0, &reply)) {
            // Byte 2: Controller ID (1=LJC, 2=RJC, 3=Pro)
            ctx->m_eControllerType = CalculateControllerType(ctx, (ESwitchDeviceInfoControllerType)reply->deviceInfo.ucDeviceType);

            // Bytes 4-9: MAC address (big-endian)
            SDL_memcpy(ctx->m_rgucMACAddress, reply->deviceInfo.rgucMACAddress, sizeof(ctx->m_rgucMACAddress));

            return true;
        }
    } else {
        if (WriteProprietary(ctx, k_eSwitchProprietaryCommandIDs_Status, NULL, 0, true)) {
            SwitchProprietaryStatusPacket_t *status = (SwitchProprietaryStatusPacket_t *)&ctx->m_rgucReadBuffer[0];
            size_t i;

            ctx->m_eControllerType = CalculateControllerType(ctx, (ESwitchDeviceInfoControllerType)status->ucDeviceType);

            for (i = 0; i < sizeof(ctx->m_rgucMACAddress); ++i) {
                ctx->m_rgucMACAddress[i] = status->rgucMACAddress[sizeof(ctx->m_rgucMACAddress) - i - 1];
            }

            return true;
        }
    }
    return false;
}

static bool BTrySetupUSB(SDL_DriverSwitch_Context *ctx)
{
    /* We have to send a connection handshake to the controller when communicating over USB
     * before we're able to send it other commands. Luckily this command is not supported
     * over Bluetooth, so we can use the controller's lack of response as a way to
     * determine if the connection is over USB or Bluetooth
     */
    if (!WriteProprietary(ctx, k_eSwitchProprietaryCommandIDs_Handshake, NULL, 0, true)) {
        return false;
    }
    if (!WriteProprietary(ctx, k_eSwitchProprietaryCommandIDs_HighSpeed, NULL, 0, true)) {
        // The 8BitDo M30 and SF30 Pro don't respond to this command, but otherwise work correctly
        // return false;
    }
    if (!WriteProprietary(ctx, k_eSwitchProprietaryCommandIDs_Handshake, NULL, 0, true)) {
        // This fails on the right Joy-Con when plugged into the charging grip
        // return false;
    }
    if (!WriteProprietary(ctx, k_eSwitchProprietaryCommandIDs_ForceUSB, NULL, 0, false)) {
        return false;
    }
    return true;
}

static bool SetVibrationEnabled(SDL_DriverSwitch_Context *ctx, Uint8 enabled)
{
    return WriteSubcommand(ctx, k_eSwitchSubcommandIDs_EnableVibration, &enabled, sizeof(enabled), NULL);
}
static bool SetInputMode(SDL_DriverSwitch_Context *ctx, Uint8 input_mode)
{
#ifdef FORCE_SIMPLE_REPORTS
    input_mode = k_eSwitchInputReportIDs_SimpleControllerState;
#endif
#ifdef FORCE_FULL_REPORTS
    input_mode = k_eSwitchInputReportIDs_FullControllerState;
#endif

    if (input_mode == ctx->m_nCurrentInputMode) {
        return true;
    } else {
        ctx->m_nCurrentInputMode = input_mode;

        return WriteSubcommand(ctx, k_eSwitchSubcommandIDs_SetInputReportMode, &input_mode, sizeof(input_mode), NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Right Joy-Con NIR camera: cover/proximity scalar (PadForge fork addition).
 *
 * The camera is behind the NFC/IR MCU. The enable sequence, register values,
 * packet framing, and CRC are transcribed from jc_toolkit (MIT), the proven
 * implementation: jctool.cpp ir_sensor() (:1672-2099), get_raw_ir_image()
 * (:1395-1502), mcu_crc8_calc() (:43-50), and the CRC-8-CCITT table from
 * ir_sensor.h (:4-21). dekuNukem's bluetooth_hid_subcommands_notes.md (:83)
 * documents that the 0x31 input report carries all-zero IR data unless an
 * 0x11 output report with subcmd 0x03 is sent first, so the poll/ACK writes
 * are mandatory, not optional.
 *
 * No image is assembled. The MCU computes an average-intensity statistic into
 * every IR fragment header (0x31 report byte 53, 0-255, jctool.cpp:1495-1499),
 * and that scalar is the whole feature: a covered NIR window floods bright,
 * an uncovered one reads dark. The fragment headers live at bytes 49-58, so
 * the driver's 64-byte read buffer keeps them even though the full 361-byte
 * report is truncated by the HID read (hid.c return_data copies
 * min(length, report)).
 * ------------------------------------------------------------------------- */

// crc-8-ccitt / polynomial 0x07 look-up table (jc_toolkit ir_sensor.h:4-21)
static const Uint8 k_rgucMCUCrc8Table[256] = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
    0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65, 0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
    0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
    0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
    0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2, 0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA,
    0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
    0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A,
    0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42, 0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
    0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
    0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
    0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C, 0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44,
    0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
    0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63,
    0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B, 0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
    0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
    0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3
};

// jc_toolkit mcu_crc8_calc (jctool.cpp:43-50)
static Uint8 MCUCrc8(const Uint8 *pBuf, int nLen)
{
    Uint8 crc8 = 0;
    int i;

    for (i = 0; i < nLen; ++i) {
        crc8 = k_rgucMCUCrc8Table[(Uint8)(crc8 ^ pBuf[i])];
    }
    return crc8;
}

/* Send an 0x11 output report (MCU data request). Framing from jc_toolkit:
 * byte 0 = 0x11, byte 1 = packet number, bytes 2-9 = current rumble data (the
 * same common header ConstructSubcommand builds), byte 10 = MCU subcommand.
 * For subcmd 0x03 (IR data poll / fragment ACK) byte 14 carries the fragment
 * number being ACKed, byte 47 = crc8 over bytes 11-46, byte 48 = 0xFF
 * (get_raw_ir_image, jctool.cpp:1425-1433). For subcmd 0x01 (MCU status poll)
 * no CRC is set (ir_sensor step 2, jctool.cpp:1738-1749). */
static bool WriteMcuDataRequest(SDL_DriverSwitch_Context *ctx, Uint8 ucMcuSubcommand, Uint8 ucArg1, Uint8 ucFragAck)
{
    Uint8 rgucPacket[k_unSwitchOutputPacketDataLength];

    SDL_zeroa(rgucPacket);
    rgucPacket[0] = k_eSwitchOutputReportIDs_McuData;
    rgucPacket[1] = ctx->m_nCommandNumber;
    SDL_memcpy(&rgucPacket[2], ctx->m_RumblePacket.rumbleData, sizeof(ctx->m_RumblePacket.rumbleData));
    rgucPacket[10] = ucMcuSubcommand;
    ctx->m_nCommandNumber = (ctx->m_nCommandNumber + 1) & 0xF;

    if (ucMcuSubcommand == 0x03) {
        rgucPacket[11] = ucArg1;
        rgucPacket[14] = ucFragAck;
        rgucPacket[47] = MCUCrc8(&rgucPacket[11], 36);
        rgucPacket[48] = 0xFF;
    }
    return WritePacket(ctx, rgucPacket, sizeof(rgucPacket));
}

static bool SetHomeLED(SDL_DriverSwitch_Context *ctx, Uint8 brightness)
{
    Uint8 ucLedIntensity = 0;
    Uint8 rgucBuffer[4];

    if (brightness > 0) {
        if (brightness < 65) {
            ucLedIntensity = (brightness + 5) / 10;
        } else {
            ucLedIntensity = (Uint8)SDL_ceilf(0xF * SDL_powf((float)brightness / 100.f, 2.13f));
        }
    }

    rgucBuffer[0] = (0x0 << 4) | 0x1;                    // 0 mini cycles (besides first), cycle duration 8ms
    rgucBuffer[1] = ((ucLedIntensity & 0xF) << 4) | 0x0; // LED start intensity (0x0-0xF), 0 cycles (LED stays on at start intensity after first cycle)
    rgucBuffer[2] = ((ucLedIntensity & 0xF) << 4) | 0x0; // First cycle LED intensity, 0x0 intensity for second cycle
    rgucBuffer[3] = (0x0 << 4) | 0x0;                    // 8ms fade transition to first cycle, 8ms first cycle LED duration

    return WriteSubcommand(ctx, k_eSwitchSubcommandIDs_SetHomeLight, rgucBuffer, sizeof(rgucBuffer), NULL);
}

static void SDLCALL SDL_HomeLEDHintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)userdata;

    if (hint && *hint) {
        int value;

        if (SDL_strchr(hint, '.') != NULL) {
            value = (int)(100.0f * SDL_atof(hint));
            if (value > 255) {
                value = 255;
            }
        } else if (SDL_GetStringBoolean(hint, true)) {
            value = 100;
        } else {
            value = 0;
        }
        SetHomeLED(ctx, (Uint8)value);
    }
}

static void UpdateSlotLED(SDL_DriverSwitch_Context *ctx)
{
    if (!ctx->m_bInputOnly) {
        Uint8 led_data = 0;
        const Uint8 player_pattern[] = { 0x1, 0x3, 0x7, 0xf, 0x9, 0x5, 0xd, 0x6 };

        if (ctx->m_bPlayerLights && ctx->m_nPlayerIndex >= 0) {
            led_data = player_pattern[ctx->m_nPlayerIndex % 8];
        }
        WriteSubcommand(ctx, k_eSwitchSubcommandIDs_SetPlayerLights, &led_data, sizeof(led_data), NULL);
    }
}

static void SDLCALL SDL_PlayerLEDHintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)userdata;
    bool bPlayerLights = SDL_GetStringBoolean(hint, true);

    if (bPlayerLights != ctx->m_bPlayerLights) {
        ctx->m_bPlayerLights = bPlayerLights;

        UpdateSlotLED(ctx);
        HIDAPI_UpdateDeviceProperties(ctx->device);
    }
}

static void GetInitialInputMode(SDL_DriverSwitch_Context *ctx)
{
    if (!ctx->m_nInitialInputMode) {
        // This will set the initial input mode if it can
        ReadInput(ctx);
    }
}

static Uint8 GetDefaultInputMode(SDL_DriverSwitch_Context *ctx)
{
    Uint8 input_mode;

    // Determine the desired input mode
    if (ctx->m_nInitialInputMode) {
        input_mode = ctx->m_nInitialInputMode;
    } else {
        if (ctx->device->is_bluetooth) {
            input_mode = k_eSwitchInputReportIDs_SimpleControllerState;
        } else {
            input_mode = k_eSwitchInputReportIDs_FullControllerState;
        }
    }

    switch (ctx->m_eEnhancedReportHint) {
    case SWITCH_ENHANCED_REPORT_HINT_OFF:
        input_mode = k_eSwitchInputReportIDs_SimpleControllerState;
        break;
    case SWITCH_ENHANCED_REPORT_HINT_ON:
        if (input_mode == k_eSwitchInputReportIDs_SimpleControllerState) {
            input_mode = k_eSwitchInputReportIDs_FullControllerState;
        }
        break;
    case SWITCH_ENHANCED_REPORT_HINT_AUTO:
        /* Joy-Con controllers switch their thumbsticks into D-pad mode in simple mode,
         * so let's enable full controller state for them.
         */
        if (ctx->device->vendor_id == USB_VENDOR_NINTENDO &&
            (ctx->device->product_id == USB_PRODUCT_NINTENDO_SWITCH_JOYCON_LEFT ||
             ctx->device->product_id == USB_PRODUCT_NINTENDO_SWITCH_JOYCON_RIGHT)) {
            input_mode = k_eSwitchInputReportIDs_FullControllerState;
        }
        break;
    }

    // Wired controllers break if they are put into simple controller state
    if (input_mode == k_eSwitchInputReportIDs_SimpleControllerState &&
        !ctx->device->is_bluetooth) {
        input_mode = k_eSwitchInputReportIDs_FullControllerState;
    }
    return input_mode;
}

static Uint8 GetSensorInputMode(SDL_DriverSwitch_Context *ctx)
{
    Uint8 input_mode;

    // Determine the desired input mode
    if (!ctx->m_nInitialInputMode ||
        ctx->m_nInitialInputMode == k_eSwitchInputReportIDs_SimpleControllerState) {
        input_mode = k_eSwitchInputReportIDs_FullControllerState;
    } else {
        input_mode = ctx->m_nInitialInputMode;
    }
    return input_mode;
}

// Defined with the NIR machine below: true while the IR bring-up or stream
// holds the MCU (any state but Idle and Failed)
static bool IsIROwningMcu(SDL_DriverSwitch_Context *ctx);

static void UpdateInputMode(SDL_DriverSwitch_Context *ctx)
{
    Uint8 input_mode;

    if (IsIROwningMcu(ctx) || ctx->m_bNfcActive) {
        // The NIR camera and the NFC reader stream over the NFC/IR report,
        // which also carries the full controller state, so buttons/sticks/IMU
        // keep flowing.
        input_mode = k_eSwitchInputReportIDs_FullControllerAndMcuState;
    } else if (ctx->m_bReportSensors) {
        input_mode = GetSensorInputMode(ctx);
    } else {
        input_mode = GetDefaultInputMode(ctx);
    }
    SetInputMode(ctx, input_mode);
}

/* ---------------------------------------------------------------------------
 * NFC tag reading (PadForge fork addition, hifihedgehog/SDL#15).
 *
 * The right Joy-Con and Pro Controller read NFC tags through the same NFC/IR
 * MCU the NIR camera uses. The flow is transcribed from jc_toolkit
 * nfc_tag_info() (jctool.cpp:2241 on, credited to bettse), grounded in
 * dekuNukem bluetooth_hid_subcommands_notes.md (subcommands 0x21/0x22 and
 * the 0x11 output's active-polling role) and bluetooth_hid_notes.md (report
 * 0x31: standard input in the head, MCU payload from byte 49).
 *
 * Unlike the synchronous NIR bring-up above, this machine runs one step per
 * UpdateDevice tick and never blocks: each state sends its command through
 * the normal write path, and the ack gates run on the reports the update
 * loop reads anyway (subcommand acks on 0x21, MCU payloads on 0x31). Retry
 * pacing mirrors jc_toolkit: a resend window worth ~8 report reads and 7
 * command rounds before giving up (nfc_tag_info retry/error_reading limits).
 * The standard input head of every 0x31 report keeps flowing through
 * HandleFullControllerState, so gamepad input never degrades while NFC mode
 * is active. */

typedef enum
{
    k_eSwitchNfcState_Idle,
    k_eSwitchNfcState_SetInputMode, // subcmd 0x03 arg 0x31; ack bytes 13/14 == 0x80/0x03 (jctool.cpp:2264-2290)
    k_eSwitchNfcState_EnableMCU,    // subcmd 0x22 arg 0x01; ack 0x80/0x22 (jctool.cpp:2292-2320)
    k_eSwitchNfcState_AwaitStandby, // 0x11/0x01 poll until bytes 49/56 == 0x01/0x01 (jctool.cpp:2322-2354)
    k_eSwitchNfcState_SetModeNFC,   // subcmd 0x21, MCU cmd 0x21/0x00 mode 0x04; ack bytes 15/22 == 0x01/0x01 (jctool.cpp:2356-2396)
    k_eSwitchNfcState_AwaitNFC,     // 0x11/0x01 poll until byte 56 == 0x04 (jctool.cpp:2398-2424)
    k_eSwitchNfcState_WaitReceive,  // 0x11 NFC cmd 0x04; reply 0x2a/0x0500/0x31 state 0x00 (jctool.cpp:2426-2465)
    k_eSwitchNfcState_Polling,      // steady state: 0x01 once on entry, then presence check by paced 0x06 reads (a selected tag serves reads, a removed one fails them)
    k_eSwitchNfcState_CloseSession, // 0x02 sent after a read-failure clear (no response exists, mcu.md:95-99); dwell to absorb the 3-echo tail (mcu.md:73), then rediscover
    k_eSwitchNfcState_Failed        // gave up; retried after a cooldown while the hint stays on
} ESwitchNfcState;

#define SWITCH_NFC_RESEND_MS         600  // ~8 reads at the report cadence (jc_toolkit retries > 8 at 64 ms)
#define SWITCH_NFC_MAX_ROUNDS        7    // jc_toolkit error_reading > 7
#define SWITCH_NFC_TAG_GONE_MS       3500 // stream-death insurance only: removal is detected by failed presence reads long before this
#define SWITCH_NFC_READ_FAILS        2    // consecutive failed presence reads before clearing
#define SWITCH_NFC_PRESENCE_READ_MS  100  // presence-check read cadence, 3-4x above the measured 25-30 ms round trip. Removal detection is floored by silicon regardless: ~1.9 s until the MCU's own departure cycle serves the first error answer, plus the two-probe confirm, ~2.1 s total (bench-measured)
#define SWITCH_NFC_STATUS_PACE_MS    50   // pre-acquisition status-request cadence, the dump-proven detection vehicle (every request answered, tap surfaces as 01+UID)
#define SWITCH_NFC_CLOSE_DWELL_MS    200  // command-quiet dwell after the session-close 0x02, ample for the 3-echo stale tail (mcu.md:73) before rediscovery
#define SWITCH_NFC_POLL_PACE_MS      100  // minimum spacing between StartPolling re-issues
#define SWITCH_NFC_RETRY_COOLDOWN_MS 5000 // wait after a failed bring-up before trying again

static bool IsNfcSupported(SDL_DriverSwitch_Context *ctx)
{
    if (ctx->m_bInputOnly) {
        return false;
    }
    if (!ctx->device->is_bluetooth) {
        /* Bluetooth only, as a capability verdict, not a tuning gap
           (issue #15, fifteen rounds of evidence): NFC on these
           controllers is a Bluetooth capability. No reference reads NFC
           over USB (scan_amiibo.log's ~313-byte MCU frames only fit
           BT's 362-byte report, jc_toolkit's USB block is NFC-free dead
           code, dekuNukem's USB notes never mention the MCU), and on
           the wire the USB firmware acks the switch to report mode
           0x31 and then never streams a single 0x31 frame. Do not
           reattempt USB NFC without new hardware evidence. */
        return false;
    }
    /* The reader is in the right Joy-Con, paired or standalone, and the
       Pro Controller. A combined pair's right child runs this machine on
       its own HID handle and posts to the pair's joystick, the same
       plumbing that already delivers the child's input and GYRO_R/ACCEL_R
       (PadForge#248). The left child has no reader and stays excluded by
       the type check. */
    if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
        return true;
    }
    return !ctx->device->parent &&
           ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_ProController;
}

/* Send a subcommand without waiting for the reply: the async machine reads
 * its acks from the update loop instead. The framing is WriteSubcommand's
 * send half. */
static bool SendSubcommandAsync(SDL_DriverSwitch_Context *ctx, ESwitchSubcommandIDs ucCommandID, const Uint8 *pBuf, Uint8 ucLen)
{
    SwitchSubcommandOutputPacket_t commandPacket;

    ConstructSubcommand(ctx, ucCommandID, pBuf, ucLen, &commandPacket);
    return WritePacket(ctx, &commandPacket, sizeof(commandPacket));
}

/* Send an 0x11 output carrying an MCU NFC command (MCU subcommand 0x02).
 * Framing from jc_toolkit steps 5-6 (jctool.cpp:2432-2447, 2478-2494):
 * byte 10 = 0x02, byte 11 = NFC command (0x04 StartWaitingReceive, 0x01
 * StartPolling, 0x02 StopPolling, 0x00 CancelAll), byte 14 = 0x08 (last
 * packet of the command), byte 15 = payload length, payload from byte 16,
 * crc8 over bytes 11-46 at byte 47. */
static bool WriteNfcCommand(SDL_DriverSwitch_Context *ctx, Uint8 ucNfcCommand, const Uint8 *pPayload, Uint8 ucPayloadLen)
{
    Uint8 rgucPacket[k_unSwitchOutputPacketDataLength];

    SDL_zeroa(rgucPacket);
    rgucPacket[0] = k_eSwitchOutputReportIDs_McuData;
    rgucPacket[1] = ctx->m_nCommandNumber;
    SDL_memcpy(&rgucPacket[2], ctx->m_RumblePacket.rumbleData, sizeof(ctx->m_RumblePacket.rumbleData));
    ctx->m_nCommandNumber = (ctx->m_nCommandNumber + 1) & 0xF;

    rgucPacket[10] = 0x02;
    rgucPacket[11] = ucNfcCommand;
    rgucPacket[12] = 0x00; // packet number in a multi-packet command
    rgucPacket[13] = 0x00;
    rgucPacket[14] = 0x08; // last command packet
    rgucPacket[15] = ucPayloadLen;
    if (pPayload && ucPayloadLen > 0) {
        SDL_memcpy(&rgucPacket[16], pPayload, ucPayloadLen);
    }
    rgucPacket[47] = MCUCrc8(&rgucPacket[11], 36);
    return WritePacket(ctx, rgucPacket, sizeof(rgucPacket));
}

static bool SendNfcStartPolling(SDL_DriverSwitch_Context *ctx)
{
    /* jc_toolkit step 6 payload (jctool.cpp:2484-2489): enable Mifare
       support, two unknown zeros, 0x2c (some other values fail), and a
       trailing 0x01 the Switch itself sends. */
    static const Uint8 rgucPayload[] = { 0x01, 0x00, 0x00, 0x2c, 0x01 };

    return WriteNfcCommand(ctx, 0x01, rgucPayload, sizeof(rgucPayload));
}

static bool SendNfcPresenceRead(SDL_DriverSwitch_Context *ctx)
{
    /* NTAG read request (0x06), jc_toolkit step7's frame: 0xd0 0x07, a
       zeroed 7-byte UID slot (read whatever is selected), 0x00 = all tag
       types (0x01 is NTAG215-only and errors 0x48 on anything else,
       jctool.cpp:2571), then one block covering page 0 only, the probe
       shape jc_toolkit itself sends when the tag type is still unknown
       (jctool.cpp:2577-2580). The probed page is immaterial to removal
       latency: status requests, cached-page reads, and uncached-page
       reads all measure the same ~1.9 s to the first error answer,
       which is the MCU's own internal departure cycle, so the simplest
       fixed-page frame stays. */
    static const Uint8 rgucPayload[] = {
        0xd0, 0x07, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // UID slot: any
        0x00,                               // all tag types
        0x01,                               // one page block
        0x00, 0x00,                         // pages 0..0
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    return WriteNfcCommand(ctx, 0x06, rgucPayload, sizeof(rgucPayload));
}

static void SetNfcTagUid(SDL_DriverSwitch_Context *ctx, SDL_Joystick *joystick, const char *pchUid)
{
    if (joystick) {
        SDL_SendJoystickNfcTagUid(joystick, pchUid);
    }
    ctx->m_bNfcTagPresent = (pchUid && *pchUid);
}

/* Enter a state and send its command. Also used to re-send the current
 * state's command on a retry round. */
static void EnterNfcState(SDL_DriverSwitch_Context *ctx, Uint8 ucState, Uint64 now)
{
    ctx->m_ucNfcState = ucState;
    ctx->m_ulNfcActionTicks = now;
    ctx->m_bNfcActive = true;

    switch (ucState) {
    case k_eSwitchNfcState_SetInputMode:
    {
        Uint8 ucMode = k_eSwitchInputReportIDs_FullControllerAndMcuState;
        // Keep the mode bookkeeping in agreement so nothing reverts it
        ctx->m_nCurrentInputMode = ucMode;
        SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetInputReportMode, &ucMode, sizeof(ucMode));
        break;
    }
    case k_eSwitchNfcState_EnableMCU:
    {
        Uint8 ucOn = 0x01;
        SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetMCUState, &ucOn, sizeof(ucOn));
        break;
    }
    case k_eSwitchNfcState_AwaitStandby:
    case k_eSwitchNfcState_AwaitNFC:
        WriteMcuDataRequest(ctx, 0x01, 0, 0); // MCU status poll
        break;
    case k_eSwitchNfcState_SetModeNFC:
    {
        /* Subcommand 0x21 with the 38-byte MCU payload: mcu_cmd 0x21,
           mcu_subcmd 0x00, mode 0x04 (NFC), crc8 over payload[1..36] at
           payload[37], the same layout the IR machine's SetModeIR state
           builds for the camera (jctool.cpp:2363-2374). Sent async; the
           ack gate runs in HandleNfcSubcommandReply. */
        Uint8 rgucPayload[38];
        SDL_zeroa(rgucPayload);
        rgucPayload[0] = 0x21;
        rgucPayload[1] = 0x00;
        rgucPayload[2] = 0x04; // MCU mode 4: NFC
        rgucPayload[37] = MCUCrc8(&rgucPayload[1], 36);
        SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetMCUConfig, rgucPayload, sizeof(rgucPayload));
        break;
    }
    case k_eSwitchNfcState_WaitReceive:
        WriteNfcCommand(ctx, 0x04, NULL, 0); // StartWaitingReceive
        break;
    case k_eSwitchNfcState_Polling:
        /* One StartPolling per session (mcu.py:273-275): a re-issue into
           an active session produces sequence errors (CTCaer's raw
           capture). The solicitation clock is stamped NOW, never zeroed:
           a zeroed clock let the status timer fire in the same tick as
           this 0x01 and the 0x04 mode-arm stomped the just-started poll
           (dump: state-00 answers for 69+ s). The poll's first answer
           gets a full timer slot before any other command. */
        ctx->m_ucNfcStatusMisses = 0;
        ctx->m_ulNfcReadTicks = now;
        SendNfcStartPolling(ctx);
        break;
    case k_eSwitchNfcState_CloseSession:
        WriteNfcCommand(ctx, 0x02, NULL, 0); // stop polling: closes the errored session, no response to wait for (mcu.md:95-99)
        break;
    default:
        break;
    }
}

/* Drop the machine without touching the bus: used when the IR path takes
 * MCU ownership (suspending the MCU here would kill the camera it just
 * configured). */
static void AbandonNfc(SDL_DriverSwitch_Context *ctx, SDL_Joystick *joystick)
{
    SetNfcTagUid(ctx, joystick, NULL);
    ctx->m_bNfcActive = false;
    ctx->m_ucNfcState = k_eSwitchNfcState_Idle;
    ctx->m_ucNfcRounds = 0;
}

/* Power the MCU back down and (optionally) restore the input report mode,
 * mirroring jc_toolkit's teardown (step 10, jctool.cpp:2048-2095). The
 * suspend is sent unconditionally when the machine was up: suspending an
 * MCU that never resumed is harmless, and inferring power from the state
 * enum is how a watchdog re-init would leak a running MCU. From the update
 * loop this is fire-and-forget (a lost suspend costs battery, not
 * correctness, and the hint machinery re-arms cleanly); the close path uses
 * a synchronous variant below since no later tick is guaranteed. */
static void TeardownNfc(SDL_DriverSwitch_Context *ctx, SDL_Joystick *joystick, bool bRestoreInputMode)
{
    bool bWasUp = (ctx->m_ucNfcState != k_eSwitchNfcState_Idle &&
                   ctx->m_ucNfcState != k_eSwitchNfcState_Failed);

    if (bWasUp) {
        Uint8 ucOff = 0x00;
        SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetMCUState, &ucOff, sizeof(ucOff));
    }
    if (bWasUp && bRestoreInputMode && !IsIROwningMcu(ctx)) {
        Uint8 ucMode = ctx->m_bReportSensors ? GetSensorInputMode(ctx) : GetDefaultInputMode(ctx);
        ctx->m_nCurrentInputMode = ucMode;
        SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetInputReportMode, &ucMode, sizeof(ucMode));
    }
    AbandonNfc(ctx, joystick);
}

static void FailNfc(SDL_DriverSwitch_Context *ctx, SDL_Joystick *joystick, Uint64 now)
{
    TeardownNfc(ctx, joystick, true);
    ctx->m_ucNfcState = k_eSwitchNfcState_Failed;
    ctx->m_ulNfcActionTicks = now;
}

/* Ack gates for the subcommand-reply steps, on the raw 0x21 report bytes
 * jc_toolkit checks: byte 13 = ack, byte 14 = replied subcommand, and for
 * the MCU-config reply the standby state echo at bytes 15 and 22. */
static void HandleNfcSubcommandReply(SDL_DriverSwitch_Context *ctx, SDL_Joystick *joystick, Uint64 now, int size)
{
    const Uint8 *buf = ctx->m_rgucReadBuffer;

    (void)joystick;
    /* Byte 13's MSB is the ACK/NACK discriminator (dekuNukem
       bluetooth_hid_notes.md); a truncated report or a NACK must not
       advance the machine on stale payload bytes. */
    if (size < 23 || !(buf[13] & 0x80)) {
        return;
    }
    switch (ctx->m_ucNfcState) {
    case k_eSwitchNfcState_SetInputMode:
        if (buf[13] == 0x80 && buf[14] == 0x03) {
            ctx->m_ucNfcRounds = 0;
            EnterNfcState(ctx, k_eSwitchNfcState_EnableMCU, now);
        }
        break;
    case k_eSwitchNfcState_EnableMCU:
        if (buf[13] == 0x80 && buf[14] == 0x22) {
            ctx->m_ucNfcRounds = 0;
            EnterNfcState(ctx, k_eSwitchNfcState_AwaitStandby, now);
        }
        break;
    case k_eSwitchNfcState_SetModeNFC:
        if (buf[14] == 0x21 && buf[15] == 0x01 && buf[22] == 0x01) {
            ctx->m_ucNfcRounds = 0;
            EnterNfcState(ctx, k_eSwitchNfcState_AwaitNFC, now);
        }
        break;
    default:
        break;
    }
}

/* MCU payload gates on 0x31 reports: status polls during bring-up, NFC
 * responses and tag packets while polling. Byte layout per jc_toolkit:
 * 49 = MCU report type (0x01 status, 0x2a NFC), 50-51 = 0x0500 for NFC,
 * 56 = state, and for a detected tag IC at 62, type at 63, UID length at
 * 64, UID bytes from 65 (jctool.cpp:2503-2527). */
static void HandleNfcMcuReport(SDL_DriverSwitch_Context *ctx, SDL_Joystick *joystick, Uint64 now, int size)
{
    const Uint8 *buf = ctx->m_rgucReadBuffer;

    if (size < 57) {
        return;
    }
    /* Heartbeat only on NFC-shaped payloads: report 0x31 streams at 60 Hz
       with an all-zero MCU tail when no MCU activity is pending, and letting
       those refresh the watchdog would let a lost polling command livelock
       forever. */
    if (buf[49] != 0x01 && buf[49] != 0x2a && buf[49] != 0x3a) {
        return;
    }
    /* Error answers (nonzero error byte) do not feed the heartbeat: the
       bench showed a dead session answering every request with the same
       2a 47 error frame for 40+ seconds, which would hold the watchdog
       off forever. */
    if (buf[49] != 0x2a || buf[50] == 0x00) {
        ctx->m_ulNfcLastMcuTicks = now;
    }

    switch (ctx->m_ucNfcState) {
    case k_eSwitchNfcState_AwaitStandby:
        if (buf[49] == 0x01 && buf[56] == 0x01) {
            ctx->m_ucNfcRounds = 0;
            EnterNfcState(ctx, k_eSwitchNfcState_SetModeNFC, now);
        }
        break;
    case k_eSwitchNfcState_AwaitNFC:
        if (buf[49] == 0x01 && buf[56] == 0x04) {
            ctx->m_ucNfcRounds = 0;
            EnterNfcState(ctx, k_eSwitchNfcState_WaitReceive, now);
        }
        break;
    case k_eSwitchNfcState_WaitReceive:
        if (buf[49] == 0x2a && buf[50] == 0x00 && buf[51] == 0x05 && buf[55] == 0x31) {
            if (buf[56] == 0x00) {
                ctx->m_ucNfcRounds = 0;
                ctx->m_ulNfcLastTagTicks = 0;
                EnterNfcState(ctx, k_eSwitchNfcState_Polling, now);
            } else if (buf[56] == 0x0b &&
                       now >= ctx->m_ulNfcActionTicks + SWITCH_NFC_POLL_PACE_MS) {
                /* Busy: reissue promptly, matching jc_toolkit's re-send-on-
                   busy loop, but bounded by the round counter (their
                   error_reading equivalent): each reissue refreshes the
                   action clock the 600 ms round timer keys on, so an
                   unbounded busy storm would otherwise pin this state
                   forever. */
                if (++ctx->m_ucNfcRounds > SWITCH_NFC_MAX_ROUNDS) {
                    FailNfc(ctx, joystick, now);
                } else {
                    ctx->m_ulNfcActionTicks = now;
                    WriteNfcCommand(ctx, 0x04, NULL, 0);
                }
            }
        }
        break;
    case k_eSwitchNfcState_Polling:
        if (buf[49] == 0x2a && buf[50] == 0x00 && buf[51] == 0x05) {
            if (buf[56] == 0x01 || buf[56] == 0x02 || buf[56] == 0x04 || buf[56] == 0x09) {
                /* UID-bearing session answers refresh presence: POLL
                   (0x01) for a newly seen tag, POLL_AGAIN (0x09) for the
                   same tag (mcu.py:71-78, 204-217), PENDING_READ (0x02,
                   mcu.py:70) and the read trailer (0x04, mcu.md:93)
                   during a presence read. */
                int nUidLen = buf[64];
                if (nUidLen > 10) {
                    nUidLen = 10;
                }
                if (nUidLen > 0 && size >= 65 + nUidLen) {
                    char rgchUid[21];
                    int i;
                    for (i = 0; i < nUidLen; ++i) {
                        (void)SDL_snprintf(&rgchUid[i * 2], 3, "%02x", buf[65 + i]);
                    }
                    rgchUid[nUidLen * 2] = '\0';
                    SetNfcTagUid(ctx, joystick, rgchUid);
                    ctx->m_ulNfcLastTagTicks = now;
                    ctx->m_ucNfcStatusMisses = 0;
                }
            } else if (buf[56] == 0x00 && !ctx->m_bNfcTagPresent &&
                       now >= ctx->m_ulNfcReadTicks + SWITCH_NFC_POLL_PACE_MS) {
                /* Pre-acquisition only: a state-00 answer means the MCU
                   is awaiting command, not polling (the dump showed 69+ s
                   of state-00 answers after a stomped poll, and no tap
                   could ever detect). Re-arm with a paced StartPolling,
                   CTCaer's acquisition loop. The mid-session re-issue ban
                   stands untouched: this branch requires no published
                   tag. */
                ctx->m_ulNfcReadTicks = now;
                SendNfcStartPolling(ctx);
            }
            ctx->m_ucNfcRounds = 0;
        } else if (buf[49] == 0x2a && buf[51] == 0x05 && buf[50] != 0x00 &&
                   ctx->m_bNfcTagPresent &&
                   ctx->m_ulNfcMissTicks < ctx->m_ulNfcReadTicks) {
            /* Error answer: buf[50] is the error byte (CTCaer's
               annotation; the bench pinned a removed tag's read answer
               as 2a 47 05 with stale UID in the tail; read errors
               0x3e/0x40/0x48 documented at jctool.cpp:2571-2576). Any
               nonzero error counts, one per outstanding presence read.
               The round accounting in UpdateNfc catches silent failures
               the same way. */
            ctx->m_ulNfcMissTicks = now;
            ++ctx->m_ucNfcStatusMisses;
        }
        break;
    case k_eSwitchNfcState_CloseSession:
        // Absorb everything: stale echoes and error frames carry no information here
        break;
    default:
        break;
    }
}

/* One machine step per update tick: start, retry, watchdog, debounce,
 * teardown. Never blocks. */
static void UpdateNfc(SDL_DriverSwitch_Context *ctx, SDL_Joystick *joystick, Uint64 now)
{
    if (!IsNfcSupported(ctx)) {
        return;
    }

    /* The NIR camera owns the MCU while its machine runs, bring-up included
       (it programs MCU mode 5, NFC needs mode 4). Abandon without touching
       the bus: a teardown's MCU suspend would kill the camera the IR path is
       configuring. The machine re-arms automatically when the camera stops. */
    if (IsIROwningMcu(ctx)) {
        if (ctx->m_ucNfcState != k_eSwitchNfcState_Idle &&
            ctx->m_ucNfcState != k_eSwitchNfcState_Failed) {
            AbandonNfc(ctx, joystick);
        }
        return;
    }

    /* The hint is read here, on the update loop, because SDL's hint storage
       is thread-safe and a callback-written flag would be a cross-thread
       data race (the callback runs on whichever thread changes the hint). */
    if (!SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_SWITCH_NFC, false)) {
        if (ctx->m_ucNfcState == k_eSwitchNfcState_Failed) {
            ctx->m_ucNfcState = k_eSwitchNfcState_Idle;
        } else if (ctx->m_ucNfcState != k_eSwitchNfcState_Idle) {
            TeardownNfc(ctx, joystick, true);
        }
        return;
    }

    /* Stream-death backstop, in every active state: removal is normally
       detected by failed presence reads in the Polling machinery, so this
       only clears a published tag when answers stop arriving at all. The
       Idle and Failed paths clear the tag on entry. */
    if (ctx->m_bNfcTagPresent && now >= ctx->m_ulNfcLastTagTicks + SWITCH_NFC_TAG_GONE_MS) {
        SetNfcTagUid(ctx, joystick, NULL);
    }

    switch (ctx->m_ucNfcState) {
    case k_eSwitchNfcState_Idle:
        ctx->m_ucNfcRounds = 0;
        EnterNfcState(ctx, k_eSwitchNfcState_SetInputMode, now);
        break;
    case k_eSwitchNfcState_Failed:
        if (now >= ctx->m_ulNfcActionTicks + SWITCH_NFC_RETRY_COOLDOWN_MS) {
            ctx->m_ucNfcRounds = 0;
            EnterNfcState(ctx, k_eSwitchNfcState_SetInputMode, now);
        }
        break;
    case k_eSwitchNfcState_CloseSession:
        /* No response exists for the stop (mcu.md:95-99): advance on a
           short command-quiet dwell that absorbs the stale-echo tail
           (mcu.md:73), then rediscover through the leg-0-proven
           bring-up tail. */
        if (now >= ctx->m_ulNfcActionTicks + SWITCH_NFC_CLOSE_DWELL_MS) {
            ctx->m_ucNfcRounds = 0;
            EnterNfcState(ctx, k_eSwitchNfcState_WaitReceive, now);
        }
        break;
    case k_eSwitchNfcState_Polling:
        /* Presence check by re-read, the ISO14443/T2T mechanism: a
           selected tag serves reads and stays awake, a removed one
           fails within a round. The dump proved every polling and
           status answer serves a frozen session latch forever (87+ s
           past removal), so no polling command can report departure.
           Timer-driven on schedule, never response-triggered. */
        if (!ctx->m_bNfcTagPresent) {
            /* Pre-acquisition: timer-driven status requests, the
               dump-proven detection vehicle (every request answered at
               cadence, a tap surfaces as the first UID-bearing answer).
               There is no latch to freeze before a tag is acquired. */
            if (now >= ctx->m_ulNfcReadTicks + SWITCH_NFC_STATUS_PACE_MS) {
                ctx->m_ulNfcReadTicks = now;
                WriteNfcCommand(ctx, 0x04, NULL, 0);
            }
        } else if (now >= ctx->m_ulNfcReadTicks + SWITCH_NFC_PRESENCE_READ_MS) {
            /* Round accounting: a read that produced neither a
               UID-bearing answer nor an already-counted error frame is
               a silent failure. */
            if (ctx->m_ulNfcReadTicks > 0 &&
                ctx->m_ulNfcLastTagTicks < ctx->m_ulNfcReadTicks &&
                ctx->m_ulNfcMissTicks < ctx->m_ulNfcReadTicks) {
                ctx->m_ulNfcMissTicks = now;
                ++ctx->m_ucNfcStatusMisses;
            }
            if (ctx->m_ucNfcStatusMisses >= SWITCH_NFC_READ_FAILS) {
                ctx->m_ucNfcStatusMisses = 0;
                ctx->m_ulNfcReadTicks = 0;
                SetNfcTagUid(ctx, joystick, NULL);
                /* The failed session is dead (bench: error state answers
                   every request from here on and the next tap cannot
                   detect). Close it and re-run the proven discovery
                   tail. */
                ctx->m_ucNfcRounds = 0;
                EnterNfcState(ctx, k_eSwitchNfcState_CloseSession, now);
            } else {
                ctx->m_ulNfcReadTicks = now;
                SendNfcPresenceRead(ctx);
            }
        }
        // Stream watchdog: no clean NFC-shaped packet despite the timers
        if (now >= ctx->m_ulNfcLastMcuTicks + (2 * SWITCH_NFC_RESEND_MS) &&
            now >= ctx->m_ulNfcActionTicks + SWITCH_NFC_RESEND_MS) {
            ctx->m_ucNfcRounds++;
            if (ctx->m_ucNfcRounds > SWITCH_NFC_MAX_ROUNDS) {
                /* Full re-init. Suspend the MCU first: it is known powered
                   here, and the bring-up expects to resume from suspended
                   into Standby. */
                Uint8 ucOff = 0x00;
                SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetMCUState, &ucOff, sizeof(ucOff));
                ctx->m_ucNfcRounds = 0;
                SetNfcTagUid(ctx, joystick, NULL);
                EnterNfcState(ctx, k_eSwitchNfcState_SetInputMode, now);
            } else {
                /* No separate nudge command: the status timer is already
                   the constant stimulus, a mid-session 0x01 produces
                   sequence errors (CTCaer capture), and a discovery
                   restart around a resting tag hits the HALT-blind
                   window (dump: 3.7 s). Just arm the next round. */
                ctx->m_ulNfcActionTicks = now;
            }
        }
        break;
    default:
        // Bring-up states: re-send on timeout, give up after the round limit
        if (now >= ctx->m_ulNfcActionTicks + SWITCH_NFC_RESEND_MS) {
            ctx->m_ucNfcRounds++;
            if (ctx->m_ucNfcRounds > SWITCH_NFC_MAX_ROUNDS) {
                FailNfc(ctx, joystick, now);
            } else {
                EnterNfcState(ctx, ctx->m_ucNfcState, now);
            }
        }
        break;
    }
}

/* ---------------------------------------------------------------------------
 * NIR camera bring-up (fork issues #7/#151, rebuilt async as #24).
 *
 * The right Joy-Con's NIR camera enable is jc_toolkit ir_sensor()
 * (jctool.cpp:1672-2099) rebuilt on the NFC machine's pattern above: one
 * step per UpdateDevice tick, commands sent async, acks read from the
 * reports the update loop reads anyway (subcommand acks on 0x21, MCU
 * payloads on 0x31), a per-state resend/deadline budget matching
 * jc_toolkit's (8 reads x 64 ms per write, 8 writes per step, roughly four
 * seconds of patience per step), and a Failed state retried on a backoff
 * while demand persists, so one bad bring-up at connect no longer kills the
 * feature for the session. Every transition and failure logs through
 * SDL_LogDebug, which is the acceptance mechanism: the maintainers have no
 * IR hardware, so the log is what names a refusing step in the field.
 * The old enable was synchronous on the caller's thread and held SDL's
 * joystick lock for its full duration; this machine never blocks. */

typedef enum
{
    k_eSwitchIRState_Idle,
    k_eSwitchIRState_SetInputMode, // subcmd 0x03 arg 0x31; ack bytes 13/14 == 0x80/0x03
    k_eSwitchIRState_EnableMCU,    // subcmd 0x22 arg 0x01; ack 0x80/0x22 (jctool.cpp:1706-1734)
    k_eSwitchIRState_AwaitStandby, // 0x11/0x01 poll until bytes 49/56 == 0x01/0x01 (jctool.cpp:1738-1766)
    k_eSwitchIRState_SetModeIR,    // subcmd 0x21, MCU cmd 0x21/0x00 mode 0x05; ack = pre-switch Standby echo, raw bytes 15 and 22-25 = 0x01 and u32 0x01 (jctool.cpp:1770-1806)
    k_eSwitchIRState_AwaitIR,      // 0x11/0x01 poll until byte 56 == 0x05 (jctool.cpp:1810-1838)
    k_eSwitchIRState_SetIRMode7,   // subcmd 0x21, MCU 0x23/0x01: IR mode 7 (image transfer), 30x40, FW 5.18; ack byte 15 == 0x0b (jctool.cpp:1841-1882)
    k_eSwitchIRState_Registers1,   // first register block + the status nudge; ack bytes 15-17 == 0x13/0x00/0x07 (jctool.cpp:1925-1978)
    k_eSwitchIRState_Registers2,   // second register block; same ack or the 0x23 alternate (jctool.cpp:1986-2038)
    k_eSwitchIRState_FirstAck,     // 0x11/0x03 starts the stream (dekuNukem notes:83); advance on the first image fragment
    k_eSwitchIRState_Streaming,    // m_bIRSensorActive: fragments post + ACK, watchdogs live
    k_eSwitchIRState_Failed        // gave up; retried on a backoff while demand persists
} ESwitchIRState;

#define SWITCH_IR_RESEND_MS         600   // per-round ack patience, ~8 reads at the report cadence (jc_toolkit reads 8 x 64 ms per write)
#define SWITCH_IR_MAX_ROUNDS        7     // fresh re-writes per state before failing (jc_toolkit error_reading > 7)
#define SWITCH_IR_RETRY_COOLDOWN_MS 5000  // base retry delay after a failed bring-up; doubles per consecutive failure
#define SWITCH_IR_STREAM_GONE_MS    2000  // no image fragment while streaming: restart the bring-up
#define SWITCH_IR_MODE_FIX_PACE_MS  1000  // report-mode watchdog pacing (a 0x30 report while streaming)
#define SWITCH_IR_FRAG_MAX          0x03  // final fragment number at 30x40 (ir_max_frag_no, FormJoy.h:6127-6131)

static const char *GetIRStateName(Uint8 ucState)
{
    static const char *const k_rgpszIRStates[] = {
        "Idle", "SetInputMode", "EnableMCU", "AwaitStandby", "SetModeIR",
        "AwaitIR", "SetIRMode7", "Registers1", "Registers2", "FirstAck",
        "Streaming", "Failed"
    };

    if (ucState < SDL_arraysize(k_rgpszIRStates)) {
        return k_rgpszIRStates[ucState];
    }
    return "Unknown";
}

/* The gating the sync enable used, unchanged (fork #151): opt-in by hint
 * because powering the MCU costs battery, standalone right Joy-Con only
 * because a combined pair's shared joystick has no IR axis to deliver to. */
static bool IsIRSupported(SDL_DriverSwitch_Context *ctx)
{
    return !ctx->m_bInputOnly &&
           ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight &&
           !ctx->device->parent;
}

static bool IsIROwningMcu(SDL_DriverSwitch_Context *ctx)
{
    return ctx->m_ucIRState != k_eSwitchIRState_Idle &&
           ctx->m_ucIRState != k_eSwitchIRState_Failed;
}

/* Power the MCU back down (subcmd 0x22 arg 0x00, ir_sensor step 10,
 * jctool.cpp:2064-2075), so the camera does not drain the battery or leave
 * the Joy-Con stuck in IR mode. Synchronous: used from the close path and
 * the sensors-off edge, where no later update tick is guaranteed. Safe to
 * send even if the MCU never resumed (jc_toolkit likewise jumps to its
 * disable step on every enable failure). Also resets the machine. */
static void DisableIRSensor(SDL_DriverSwitch_Context *ctx)
{
    Uint8 ucState = 0x00;

    WriteSubcommand(ctx, k_eSwitchSubcommandIDs_SetMCUState, &ucState, sizeof(ucState), NULL);
    if (ctx->m_bIRSensorActive && ctx->joystick) {
        /* Park the IR axis at its floor: it otherwise keeps the last
           frame's value, and a bright last frame false-fires brightness
           bindings when the camera later re-enables, until a fresh frame
           arrives (PadForge#248 closure audit). */
        SDL_SendJoystickAxis(SDL_GetTicksNS(), ctx->joystick, SDL_GAMEPAD_AXIS_COUNT, SDL_MIN_SINT16);
    }
    ctx->m_bIRSensorActive = false;
    if (ctx->m_ucIRState != k_eSwitchIRState_Idle) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Joy-Con IR: torn down from step %d (%s)",
                     ctx->m_ucIRState, GetIRStateName(ctx->m_ucIRState));
    }
    ctx->m_ucIRState = k_eSwitchIRState_Idle;
    ctx->m_ucIRRounds = 0;
}

/* Enter a state and send its command. Also used to re-send the current
 * state's command on a retry round. The payloads are the sync enable's
 * bytes, byte-faithful to jc_toolkit (re-verified in fork #24). */
static void EnterIRState(SDL_DriverSwitch_Context *ctx, Uint8 ucState, Uint64 now)
{
    if (ctx->m_ucIRState != ucState) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Joy-Con IR: step %d (%s) -> step %d (%s)",
                     ctx->m_ucIRState, GetIRStateName(ctx->m_ucIRState),
                     ucState, GetIRStateName(ucState));
    }
    ctx->m_ucIRState = ucState;
    ctx->m_ulIRActionTicks = now;

    switch (ucState) {
    case k_eSwitchIRState_SetInputMode:
    {
        Uint8 ucMode = k_eSwitchInputReportIDs_FullControllerAndMcuState;
        // Keep the mode bookkeeping in agreement so nothing reverts it
        ctx->m_nCurrentInputMode = ucMode;
        SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetInputReportMode, &ucMode, sizeof(ucMode));
        break;
    }
    case k_eSwitchIRState_EnableMCU:
    {
        Uint8 ucOn = 0x01;
        SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetMCUState, &ucOn, sizeof(ucOn));
        break;
    }
    case k_eSwitchIRState_AwaitStandby:
    case k_eSwitchIRState_AwaitIR:
        WriteMcuDataRequest(ctx, 0x01, 0, 0); // MCU status poll
        break;
    case k_eSwitchIRState_SetModeIR:
    {
        Uint8 rgucPayload[38];
        SDL_zeroa(rgucPayload);
        rgucPayload[0] = 0x21; // Set MCU mode cmd
        rgucPayload[1] = 0x00;
        rgucPayload[2] = 0x05; // MCU mode 5: IR
        rgucPayload[37] = MCUCrc8(&rgucPayload[1], 36);
        SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetMCUConfig, rgucPayload, sizeof(rgucPayload));
        break;
    }
    case k_eSwitchIRState_SetIRMode7:
    {
        Uint8 rgucPayload[38];
        SDL_zeroa(rgucPayload);
        rgucPayload[0] = 0x23; // MCU write cmd
        rgucPayload[1] = 0x01; // Set IR mode subcmd
        rgucPayload[2] = 0x07; // IR mode 7: image transfer
        rgucPayload[3] = 0x03; // fragments per blob: 0-3 (30x40)
        rgucPayload[4] = 0x00; // required major FW 0x0005 (u16 LE 0x0500)
        rgucPayload[5] = 0x05;
        rgucPayload[6] = 0x00; // required minor FW 0x0018 (u16 LE 0x1800)
        rgucPayload[7] = 0x18;
        rgucPayload[37] = MCUCrc8(&rgucPayload[1], 36);
        SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetMCUConfig, rgucPayload, sizeof(rgucPayload));
        break;
    }
    case k_eSwitchIRState_Registers1:
    {
        /* First register block, 9 registers (jctool.cpp:1925-1951):
           resolution 0x002e = 0x69 (30x40), exposure 0x0130/0x0131 = 300us
           (31200 * 300 / 1000 = 0x2490), manual exposure 0x0132 = 0, both LED
           groups 0x0010 = 0, digital gain 0x012e/0x012f = 1, external light
           filter 0x00e0 = 0x03, white-pixel threshold 0x0143 = 0xc8. */
        static const Uint8 rgucRegisters1[] = {
            0x23, 0x04, 0x09,
            0x00, 0x2e, 0x69,
            0x01, 0x30, 0x90,
            0x01, 0x31, 0x24,
            0x01, 0x32, 0x00,
            0x00, 0x10, 0x00,
            0x01, 0x2e, 0x10,
            0x01, 0x2f, 0x00,
            0x00, 0x0e, 0x03,
            0x01, 0x43, 0xc8
        };
        Uint8 rgucPayload[38];
        SDL_zeroa(rgucPayload);
        SDL_memcpy(rgucPayload, rgucRegisters1, sizeof(rgucRegisters1));
        rgucPayload[37] = MCUCrc8(&rgucPayload[1], 36);
        SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetMCUConfig, rgucPayload, sizeof(rgucPayload));
        /* jc_toolkit's step 7 interleaves an 0x11/0x03 arg-0x02 IR status
           request between the register write and the ack read
           (jctool.cpp:1954-1965); without it the ack can arrive in its
           alternate 0x23 form, which the gates also accept. */
        WriteMcuDataRequest(ctx, 0x03, 0x02, 0x00);
        break;
    }
    case k_eSwitchIRState_Registers2:
    {
        /* Second register block, 8 registers (jctool.cpp:1986-2024):
           LED 1/2 intensity 0x0011 = 0x0f (max), LED 3/4 intensity 0x0012 =
           0x10 (max), no flip 0x002d = 0, denoise on 0x0167 = 1 with edge
           0x0168 = 0x23 and color 0x0169 = 0x44 defaults, buffer update time
           0x0004 = 0x2d (the 30x40 value), finalize 0x0007 = 0x01 (without
           it nothing takes effect). */
        static const Uint8 rgucRegisters2[] = {
            0x23, 0x04, 0x08,
            0x00, 0x11, 0x0f,
            0x00, 0x12, 0x10,
            0x00, 0x2d, 0x00,
            0x01, 0x67, 0x01,
            0x01, 0x68, 0x23,
            0x01, 0x69, 0x44,
            0x00, 0x04, 0x2d,
            0x00, 0x07, 0x01
        };
        Uint8 rgucPayload[38];
        SDL_zeroa(rgucPayload);
        SDL_memcpy(rgucPayload, rgucRegisters2, sizeof(rgucRegisters2));
        rgucPayload[37] = MCUCrc8(&rgucPayload[1], 36);
        SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetMCUConfig, rgucPayload, sizeof(rgucPayload));
        break;
    }
    case k_eSwitchIRState_FirstAck:
        /* The 0x31 report carries all-zero IR data until an 0x11 output with
           subcmd 0x03 is sent (dekuNukem notes:83; jc_toolkit's "first ack",
           jctool.cpp:1425-1433). */
        WriteMcuDataRequest(ctx, 0x03, 0x00, 0x00);
        break;
    case k_eSwitchIRState_Streaming:
        ctx->m_bIRSensorActive = true;
        ctx->m_ulIRLastFragTicks = now;
        ctx->m_ucIRFails = 0;
        break;
    default:
        break;
    }
}

/* Async teardown for machine-internal paths (failure, demand drop on the
 * update loop): power the MCU down, park the axis, restore the input mode,
 * reset to Idle. The sync DisableIRSensor above serves the paths where no
 * later tick is guaranteed. */
static void TeardownIRAsync(SDL_DriverSwitch_Context *ctx)
{
    Uint8 ucOff = 0x00;

    SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetMCUState, &ucOff, sizeof(ucOff));
    if (ctx->m_bIRSensorActive && ctx->joystick) {
        // Park the IR axis at its floor (see DisableIRSensor)
        SDL_SendJoystickAxis(SDL_GetTicksNS(), ctx->joystick, SDL_GAMEPAD_AXIS_COUNT, SDL_MIN_SINT16);
    }
    ctx->m_bIRSensorActive = false;
    if (!ctx->m_bNfcActive) {
        Uint8 ucMode = ctx->m_bReportSensors ? GetSensorInputMode(ctx) : GetDefaultInputMode(ctx);
        ctx->m_nCurrentInputMode = ucMode;
        SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetInputReportMode, &ucMode, sizeof(ucMode));
    }
    ctx->m_ucIRState = k_eSwitchIRState_Idle;
    ctx->m_ucIRRounds = 0;
}

static Uint64 GetIRRetryCooldown(SDL_DriverSwitch_Context *ctx)
{
    // 5 s, 10 s, 20 s, 40 s, capped at 80 s
    Uint8 ucShift = (ctx->m_ucIRFails > 4) ? 4 : (Uint8)(ctx->m_ucIRFails - 1);

    return (Uint64)SWITCH_IR_RETRY_COOLDOWN_MS << ucShift;
}

static void FailIR(SDL_DriverSwitch_Context *ctx, Uint64 now)
{
    if (ctx->m_ucIRFails < 255) {
        ctx->m_ucIRFails++;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Joy-Con IR: bring-up failed at step %d (%s), failure %d, retry in %u ms",
                 ctx->m_ucIRState, GetIRStateName(ctx->m_ucIRState),
                 ctx->m_ucIRFails, (Uint32)GetIRRetryCooldown(ctx));
    TeardownIRAsync(ctx);
    ctx->m_ucIRState = k_eSwitchIRState_Failed;
    ctx->m_ulIRActionTicks = now;
}

/* Restart the bring-up in place: used by the stream watchdogs, where the MCU
 * is known powered, so it is suspended first (the bring-up expects to resume
 * from suspended into Standby, the NFC watchdog's own precedent). */
static void RestartIR(SDL_DriverSwitch_Context *ctx, Uint64 now, const char *pszReason)
{
    Uint8 ucOff = 0x00;

    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Joy-Con IR: %s, restarting bring-up", pszReason);
    if (ctx->m_bIRSensorActive && ctx->joystick) {
        // Park the IR axis at its floor (see DisableIRSensor)
        SDL_SendJoystickAxis(SDL_GetTicksNS(), ctx->joystick, SDL_GAMEPAD_AXIS_COUNT, SDL_MIN_SINT16);
    }
    ctx->m_bIRSensorActive = false;
    SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetMCUState, &ucOff, sizeof(ucOff));
    ctx->m_ucIRRounds = 0;
    EnterIRState(ctx, k_eSwitchIRState_SetInputMode, now);
}

/* Ack gates for the subcommand-reply steps, on the raw 0x21 report bytes the
 * sync enable checked: byte 13 = ack, byte 14 = replied subcommand, MCU
 * config reply payload from byte 15 (rgucSubcommandData[i] = raw byte
 * 15 + i). */
static void HandleIRSubcommandReply(SDL_DriverSwitch_Context *ctx, Uint64 now, int size)
{
    const Uint8 *buf = ctx->m_rgucReadBuffer;

    /* Byte 13's MSB is the ACK/NACK discriminator (dekuNukem
       bluetooth_hid_notes.md); a truncated report or a NACK must not
       advance the machine on stale payload bytes. */
    if (size < 23 || !(buf[13] & 0x80)) {
        return;
    }
    switch (ctx->m_ucIRState) {
    case k_eSwitchIRState_SetInputMode:
        if (buf[13] == 0x80 && buf[14] == 0x03) {
            ctx->m_ucIRRounds = 0;
            EnterIRState(ctx, k_eSwitchIRState_EnableMCU, now);
        }
        break;
    case k_eSwitchIRState_EnableMCU:
        if (buf[13] == 0x80 && buf[14] == 0x22) {
            ctx->m_ucIRRounds = 0;
            EnterIRState(ctx, k_eSwitchIRState_AwaitStandby, now);
        }
        break;
    case k_eSwitchIRState_SetModeIR:
        // Pre-switch Standby echo: bytes 15 and 22-25 = 0x01 and u32 0x01
        if (size >= 26 && buf[14] == 0x21 && buf[15] == 0x01 &&
            buf[22] == 0x01 && buf[23] == 0x00 && buf[24] == 0x00 && buf[25] == 0x00) {
            ctx->m_ucIRRounds = 0;
            EnterIRState(ctx, k_eSwitchIRState_AwaitIR, now);
        }
        break;
    case k_eSwitchIRState_SetIRMode7:
        if (buf[14] == 0x21 && buf[15] == 0x0b) {
            ctx->m_ucIRRounds = 0;
            EnterIRState(ctx, k_eSwitchIRState_Registers1, now);
        }
        break;
    case k_eSwitchIRState_Registers1:
        if (buf[14] == 0x21 &&
            buf[15] == 0x13 && buf[16] == 0x00 && buf[17] == 0x07) {
            ctx->m_ucIRRounds = 0;
            EnterIRState(ctx, k_eSwitchIRState_Registers2, now);
        }
        break;
    case k_eSwitchIRState_Registers2:
        // The 0x23 alternate arrives when no status nudge interleaved (jctool.cpp:2029-2038)
        if (buf[14] == 0x21 &&
            ((buf[15] == 0x13 && buf[16] == 0x00 && buf[17] == 0x07) ||
             buf[15] == 0x23)) {
            ctx->m_ucIRRounds = 0;
            EnterIRState(ctx, k_eSwitchIRState_FirstAck, now);
        }
        break;
    default:
        break;
    }
}

/* MCU payload gates on 0x31 reports: status polls during bring-up, image
 * fragments from FirstAck on. Byte 49 = MCU report type (0x01 status, 0x03
 * IR image data), byte 52 = fragment number, byte 53 = the MCU-computed
 * average intensity 0-255. jc_toolkit parses the stats header ONLY on a
 * frame's final fragment (got_frag_no == ir_max_frag_no,
 * jctool.cpp:1485-1499), so the intensity posts only there; earlier
 * fragments would read whatever the field holds mid-frame, which fed the
 * dead-zero symptom in PadForge#259. Every fragment is still ACKed
 * (0x11/0x03 with byte 14 = fragment number) so the MCU keeps streaming,
 * repeats included (jctool.cpp:1516-1528), so a dropped ACK self-heals. */
static void HandleIRMcuReport(SDL_DriverSwitch_Context *ctx, SDL_Joystick *joystick, Uint64 now, int size)
{
    const Uint8 *buf = ctx->m_rgucReadBuffer;

    if (buf[49] == 0x01 && size > 56) { // MCU status report
        switch (ctx->m_ucIRState) {
        case k_eSwitchIRState_AwaitStandby:
            if (buf[56] == 0x01) {
                ctx->m_ucIRRounds = 0;
                EnterIRState(ctx, k_eSwitchIRState_SetModeIR, now);
            }
            break;
        case k_eSwitchIRState_AwaitIR:
            if (buf[56] == 0x05) {
                ctx->m_ucIRRounds = 0;
                EnterIRState(ctx, k_eSwitchIRState_SetIRMode7, now);
            }
            break;
        default:
            break;
        }
    } else if (buf[49] == 0x03 && size >= 54) { // IR image-data fragment
        Uint8 ucFrag = buf[52];

        if (ctx->m_ucIRState == k_eSwitchIRState_FirstAck) {
            ctx->m_ucIRRounds = 0;
            EnterIRState(ctx, k_eSwitchIRState_Streaming, now);
        }
        if (ctx->m_ucIRState == k_eSwitchIRState_Streaming) {
            ctx->m_ulIRLastFragTicks = now;
            if (ucFrag == SWITCH_IR_FRAG_MAX) {
                Uint64 timestamp = SDL_GetTicksNS();
                Sint16 sValue = (Sint16)((buf[53] * 32767) / 255);

                /* Data axis: seed past the analog anti-jitter gate so low
                   intensities at connect are not withheld inside the band
                   (hifihedgehog/SDL#14). The axis keeps the last final-
                   fragment value between frames. */
                SDL_SeedJoystickDataAxis(joystick, SDL_GAMEPAD_AXIS_COUNT, sValue);
                SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_COUNT, sValue);
            }
            WriteMcuDataRequest(ctx, 0x03, 0x00, ucFrag);
        }
    }
}

/* One machine step per update tick: start, retry, watchdog, backoff,
 * teardown. Never blocks and never holds a wait under the joystick lock. */
static void UpdateIR(SDL_DriverSwitch_Context *ctx, SDL_Joystick *joystick, Uint64 now)
{
    if (!IsIRSupported(ctx)) {
        return;
    }

    /* Demand = sensors enabled + the app hint, read here on the update loop
       because SDL's hint storage is thread-safe and a callback-written flag
       would be a cross-thread data race (the NFC machine's precedent). */
    if (!ctx->m_bReportSensors ||
        !SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_JOYCON_IR_SENSOR, false)) {
        if (ctx->m_ucIRState == k_eSwitchIRState_Failed) {
            ctx->m_ucIRState = k_eSwitchIRState_Idle;
            ctx->m_ucIRFails = 0;
        } else if (ctx->m_ucIRState != k_eSwitchIRState_Idle) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Joy-Con IR: demand dropped, tearing down from step %d (%s)",
                         ctx->m_ucIRState, GetIRStateName(ctx->m_ucIRState));
            TeardownIRAsync(ctx);
            ctx->m_ucIRFails = 0;
        }
        return;
    }

    switch (ctx->m_ucIRState) {
    case k_eSwitchIRState_Idle:
        /* Take the MCU. If the NFC machine holds it powered, suspend first:
           the bring-up expects to resume from suspended into Standby (the
           NFC watchdog's own re-init precedent). NFC re-arms automatically
           when the camera stops. */
        if (ctx->m_bNfcActive) {
            Uint8 ucOff = 0x00;
            SendSubcommandAsync(ctx, k_eSwitchSubcommandIDs_SetMCUState, &ucOff, sizeof(ucOff));
            AbandonNfc(ctx, joystick);
        }
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Joy-Con IR: arming bring-up");
        ctx->m_ucIRRounds = 0;
        EnterIRState(ctx, k_eSwitchIRState_SetInputMode, now);
        break;
    case k_eSwitchIRState_Failed:
        if (now >= ctx->m_ulIRActionTicks + GetIRRetryCooldown(ctx)) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Joy-Con IR: retrying bring-up after failure %d", ctx->m_ucIRFails);
            ctx->m_ucIRRounds = 0;
            EnterIRState(ctx, k_eSwitchIRState_SetInputMode, now);
        }
        break;
    case k_eSwitchIRState_Streaming:
        if (now >= ctx->m_ulIRLastFragTicks + SWITCH_IR_STREAM_GONE_MS) {
            RestartIR(ctx, now, "no image fragment within the stall window");
        }
        break;
    default:
        // Bring-up states: fresh re-write on timeout, fail after the round limit
        if (now >= ctx->m_ulIRActionTicks + SWITCH_IR_RESEND_MS) {
            ctx->m_ucIRRounds++;
            if (ctx->m_ucIRRounds > SWITCH_IR_MAX_ROUNDS) {
                FailIR(ctx, now);
            } else {
                SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Joy-Con IR: step %d (%s) unanswered, resend round %d",
                             ctx->m_ucIRState, GetIRStateName(ctx->m_ucIRState), ctx->m_ucIRRounds);
                EnterIRState(ctx, ctx->m_ucIRState, now);
            }
        }
        break;
    }
}

static void SetEnhancedModeAvailable(SDL_DriverSwitch_Context *ctx)
{
    if (ctx->m_bEnhancedModeAvailable) {
        return;
    }
    ctx->m_bEnhancedModeAvailable = true;

    if (ctx->m_bSensorsSupported) {
        // Use the right sensor in the combined Joy-Con pair
        if (!ctx->device->parent ||
            ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
            SDL_PrivateJoystickAddSensor(ctx->joystick, SDL_SENSOR_GYRO, 200.0f);
            SDL_PrivateJoystickAddSensor(ctx->joystick, SDL_SENSOR_ACCEL, 200.0f);
        }
        if (ctx->device->parent &&
            ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConLeft) {
            SDL_PrivateJoystickAddSensor(ctx->joystick, SDL_SENSOR_GYRO_L, 200.0f);
            SDL_PrivateJoystickAddSensor(ctx->joystick, SDL_SENSOR_ACCEL_L, 200.0f);
        }
        if (ctx->device->parent &&
            ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
            SDL_PrivateJoystickAddSensor(ctx->joystick, SDL_SENSOR_GYRO_R, 200.0f);
            SDL_PrivateJoystickAddSensor(ctx->joystick, SDL_SENSOR_ACCEL_R, 200.0f);
        }
    }
}

static void SetEnhancedReportHint(SDL_DriverSwitch_Context *ctx, HIDAPI_Switch_EnhancedReportHint eEnhancedReportHint)
{
    ctx->m_eEnhancedReportHint = eEnhancedReportHint;

    switch (eEnhancedReportHint) {
    case SWITCH_ENHANCED_REPORT_HINT_OFF:
        ctx->m_bEnhancedMode = false;
        break;
    case SWITCH_ENHANCED_REPORT_HINT_ON:
        SetEnhancedModeAvailable(ctx);
        ctx->m_bEnhancedMode = true;
        break;
    case SWITCH_ENHANCED_REPORT_HINT_AUTO:
        SetEnhancedModeAvailable(ctx);
        break;
    }

    UpdateInputMode(ctx);
}

static void UpdateEnhancedModeOnEnhancedReport(SDL_DriverSwitch_Context *ctx)
{
    if (ctx->m_eEnhancedReportHint == SWITCH_ENHANCED_REPORT_HINT_AUTO) {
        SetEnhancedReportHint(ctx, SWITCH_ENHANCED_REPORT_HINT_ON);
    }
}

static void UpdateEnhancedModeOnApplicationUsage(SDL_DriverSwitch_Context *ctx)
{
    if (ctx->m_eEnhancedReportHint == SWITCH_ENHANCED_REPORT_HINT_AUTO) {
        SetEnhancedReportHint(ctx, SWITCH_ENHANCED_REPORT_HINT_ON);
    }
}

static void SDLCALL SDL_EnhancedReportsChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)userdata;

    if (hint && SDL_strcasecmp(hint, "auto") == 0) {
        SetEnhancedReportHint(ctx, SWITCH_ENHANCED_REPORT_HINT_AUTO);
    } else if (SDL_GetStringBoolean(hint, true)) {
        SetEnhancedReportHint(ctx, SWITCH_ENHANCED_REPORT_HINT_ON);
    } else {
        SetEnhancedReportHint(ctx, SWITCH_ENHANCED_REPORT_HINT_OFF);
    }
}

static bool SetIMUEnabled(SDL_DriverSwitch_Context *ctx, bool enabled)
{
    Uint8 imu_data = enabled ? 1 : 0;
    return WriteSubcommand(ctx, k_eSwitchSubcommandIDs_EnableIMU, &imu_data, sizeof(imu_data), NULL);
}

static bool LoadStickCalibration(SDL_DriverSwitch_Context *ctx)
{
    Uint8 *pLeftStickCal = NULL;
    Uint8 *pRightStickCal = NULL;
    size_t stick, axis;
    SwitchSubcommandInputPacket_t *user_reply = NULL;
    SwitchSubcommandInputPacket_t *factory_reply = NULL;
    SwitchSPIOpData_t readUserParams;
    SwitchSPIOpData_t readFactoryParams;
    Uint8 userParamsReadSuccessCount = 0;

    // Read User Calibration Info
    readUserParams.unAddress = k_unSPIStickUserCalibrationStartOffset;
    readUserParams.ucLength = k_unSPIStickUserCalibrationLength;

    // This isn't readable on all controllers, so ignore failure
    WriteSubcommand(ctx, k_eSwitchSubcommandIDs_SPIFlashRead, (uint8_t *)&readUserParams, sizeof(readUserParams), &user_reply);

    // Read Factory Calibration Info
    readFactoryParams.unAddress = k_unSPIStickFactoryCalibrationStartOffset;
    readFactoryParams.ucLength = k_unSPIStickFactoryCalibrationLength;

    // Automatically select the user calibration if magic bytes are set
    if (user_reply && user_reply->stickUserCalibration.rgucLeftMagic[0] == 0xB2 && user_reply->stickUserCalibration.rgucLeftMagic[1] == 0xA1) {
        userParamsReadSuccessCount += 1;
        pLeftStickCal = user_reply->stickUserCalibration.rgucLeftCalibration;
    }

    if (user_reply && user_reply->stickUserCalibration.rgucRightMagic[0] == 0xB2 && user_reply->stickUserCalibration.rgucRightMagic[1] == 0xA1) {
        userParamsReadSuccessCount += 1;
        pRightStickCal = user_reply->stickUserCalibration.rgucRightCalibration;
    }

    // Only read the factory calibration info if we failed to receive the correct magic bytes
    if (userParamsReadSuccessCount < 2) {
        // Read Factory Calibration Info
        readFactoryParams.unAddress = k_unSPIStickFactoryCalibrationStartOffset;
        readFactoryParams.ucLength = k_unSPIStickFactoryCalibrationLength;

        const int MAX_ATTEMPTS = 3;
        for (int attempt = 0;; ++attempt) {
            if (!WriteSubcommand(ctx, k_eSwitchSubcommandIDs_SPIFlashRead, (uint8_t *)&readFactoryParams, sizeof(readFactoryParams), &factory_reply)) {
                return false;
            }

            if (factory_reply->stickFactoryCalibration.opData.unAddress == k_unSPIStickFactoryCalibrationStartOffset) {
                // We successfully read the calibration data
                pLeftStickCal = factory_reply->stickFactoryCalibration.rgucLeftCalibration;
                pRightStickCal = factory_reply->stickFactoryCalibration.rgucRightCalibration;
                break;
            }

            if (attempt == MAX_ATTEMPTS) {
                return false;
            }
        }
    }

    // If we still don't have calibration data, return false
    if (pLeftStickCal == NULL || pRightStickCal == NULL)
    {
        return false;
    }

    /* Stick calibration values are 12-bits each and are packed by bit
     * For whatever reason the fields are in a different order for each stick
     * Left:  X-Max, Y-Max, X-Center, Y-Center, X-Min, Y-Min
     * Right: X-Center, Y-Center, X-Min, Y-Min, X-Max, Y-Max
     */

    // Left stick
    ctx->m_StickCalData[0].axis[0].sMax = ((pLeftStickCal[1] << 8) & 0xF00) | pLeftStickCal[0];    // X Axis max above center
    ctx->m_StickCalData[0].axis[1].sMax = (pLeftStickCal[2] << 4) | (pLeftStickCal[1] >> 4);       // Y Axis max above center
    ctx->m_StickCalData[0].axis[0].sCenter = ((pLeftStickCal[4] << 8) & 0xF00) | pLeftStickCal[3]; // X Axis center
    ctx->m_StickCalData[0].axis[1].sCenter = (pLeftStickCal[5] << 4) | (pLeftStickCal[4] >> 4);    // Y Axis center
    ctx->m_StickCalData[0].axis[0].sMin = ((pLeftStickCal[7] << 8) & 0xF00) | pLeftStickCal[6];    // X Axis min below center
    ctx->m_StickCalData[0].axis[1].sMin = (pLeftStickCal[8] << 4) | (pLeftStickCal[7] >> 4);       // Y Axis min below center

    // Right stick
    ctx->m_StickCalData[1].axis[0].sCenter = ((pRightStickCal[1] << 8) & 0xF00) | pRightStickCal[0]; // X Axis center
    ctx->m_StickCalData[1].axis[1].sCenter = (pRightStickCal[2] << 4) | (pRightStickCal[1] >> 4);    // Y Axis center
    ctx->m_StickCalData[1].axis[0].sMin = ((pRightStickCal[4] << 8) & 0xF00) | pRightStickCal[3];    // X Axis min below center
    ctx->m_StickCalData[1].axis[1].sMin = (pRightStickCal[5] << 4) | (pRightStickCal[4] >> 4);       // Y Axis min below center
    ctx->m_StickCalData[1].axis[0].sMax = ((pRightStickCal[7] << 8) & 0xF00) | pRightStickCal[6];    // X Axis max above center
    ctx->m_StickCalData[1].axis[1].sMax = (pRightStickCal[8] << 4) | (pRightStickCal[7] >> 4);       // Y Axis max above center

    // Filter out any values that were uninitialized (0xFFF) in the SPI read
    for (stick = 0; stick < 2; ++stick) {
        for (axis = 0; axis < 2; ++axis) {
            if (ctx->m_StickCalData[stick].axis[axis].sCenter == 0xFFF) {
                ctx->m_StickCalData[stick].axis[axis].sCenter = 2048;
            }
            if (ctx->m_StickCalData[stick].axis[axis].sMax == 0xFFF) {
                ctx->m_StickCalData[stick].axis[axis].sMax = (Sint16)(ctx->m_StickCalData[stick].axis[axis].sCenter * 0.7f);
            }
            if (ctx->m_StickCalData[stick].axis[axis].sMin == 0xFFF) {
                ctx->m_StickCalData[stick].axis[axis].sMin = (Sint16)(ctx->m_StickCalData[stick].axis[axis].sCenter * 0.7f);
            }
        }
    }

    for (stick = 0; stick < 2; ++stick) {
        for (axis = 0; axis < 2; ++axis) {
            ctx->m_StickExtents[stick].axis[axis].sMin = -(Sint16)(ctx->m_StickCalData[stick].axis[axis].sMin * 0.7f);
            ctx->m_StickExtents[stick].axis[axis].sMax = (Sint16)(ctx->m_StickCalData[stick].axis[axis].sMax * 0.7f);
        }
    }

    for (stick = 0; stick < 2; ++stick) {
        for (axis = 0; axis < 2; ++axis) {
            ctx->m_SimpleStickExtents[stick].axis[axis].sMin = (Sint16)(SDL_MIN_SINT16 * 0.5f);
            ctx->m_SimpleStickExtents[stick].axis[axis].sMax = (Sint16)(SDL_MAX_SINT16 * 0.5f);
        }
    }

    return true;
}

static bool LoadIMUCalibration(SDL_DriverSwitch_Context *ctx)
{
    SwitchSubcommandInputPacket_t *reply = NULL;

    // Read Calibration Info
    SwitchSPIOpData_t readParams;
    readParams.unAddress = k_unSPIIMUScaleStartOffset;
    readParams.ucLength = k_unSPIIMUScaleLength;

    if (WriteSubcommand(ctx, k_eSwitchSubcommandIDs_SPIFlashRead, (uint8_t *)&readParams, sizeof(readParams), &reply)) {
        Uint8 *pIMUScale;
        Sint16 sAccelRawX, sAccelRawY, sAccelRawZ, sGyroRawX, sGyroRawY, sGyroRawZ;
        Sint16 sAccelSensCoeffX, sAccelSensCoeffY, sAccelSensCoeffZ;
        Sint16 sGyroSensCoeffX, sGyroSensCoeffY, sGyroSensCoeffZ;

        // IMU scale gives us multipliers for converting raw values to real world values
        pIMUScale = reply->spiReadData.rgucReadData;

        sAccelRawX = (pIMUScale[1] << 8) | pIMUScale[0];
        sAccelRawY = (pIMUScale[3] << 8) | pIMUScale[2];
        sAccelRawZ = (pIMUScale[5] << 8) | pIMUScale[4];

        sAccelSensCoeffX = (pIMUScale[7] << 8) | pIMUScale[6];
        sAccelSensCoeffY = (pIMUScale[9] << 8) | pIMUScale[8];
        sAccelSensCoeffZ = (pIMUScale[11] << 8) | pIMUScale[10];

        sGyroRawX = (pIMUScale[13] << 8) | pIMUScale[12];
        sGyroRawY = (pIMUScale[15] << 8) | pIMUScale[14];
        sGyroRawZ = (pIMUScale[17] << 8) | pIMUScale[16];

        sGyroSensCoeffX = (pIMUScale[19] << 8) | pIMUScale[18];
        sGyroSensCoeffY = (pIMUScale[21] << 8) | pIMUScale[20];
        sGyroSensCoeffZ = (pIMUScale[23] << 8) | pIMUScale[22];

        // Check for user calibration data. If it's present and set, it'll override the factory settings
        readParams.unAddress = k_unSPIIMUUserScaleStartOffset;
        readParams.ucLength = k_unSPIIMUUserScaleLength;
        if (WriteSubcommand(ctx, k_eSwitchSubcommandIDs_SPIFlashRead, (uint8_t *)&readParams, sizeof(readParams), &reply)) {
            Uint8 *pUserIMUScale = reply->spiReadData.rgucReadData;

            if ((pUserIMUScale[0] | (pUserIMUScale[1] << 8)) == 0xA1B2) {
                pIMUScale = pUserIMUScale;

                sAccelRawX = (pIMUScale[3] << 8) | pIMUScale[2];
                sAccelRawY = (pIMUScale[5] << 8) | pIMUScale[4];
                sAccelRawZ = (pIMUScale[7] << 8) | pIMUScale[6];

                sGyroRawX = (pIMUScale[15] << 8) | pIMUScale[14];
                sGyroRawY = (pIMUScale[17] << 8) | pIMUScale[16];
                sGyroRawZ = (pIMUScale[19] << 8) | pIMUScale[18];
            }
        }

        // Gyro zero-rate offset
        ctx->m_IMUScaleData.sGyroOffsetX = sGyroRawX;
        ctx->m_IMUScaleData.sGyroOffsetY = sGyroRawY;
        ctx->m_IMUScaleData.sGyroOffsetZ = sGyroRawZ;

        // Accelerometer scale
        ctx->m_IMUScaleData.fAccelScaleX = SWITCH_ACCEL_SCALE_MULT / ((float)sAccelSensCoeffX - (float)sAccelRawX) * SDL_STANDARD_GRAVITY;
        ctx->m_IMUScaleData.fAccelScaleY = SWITCH_ACCEL_SCALE_MULT / ((float)sAccelSensCoeffY - (float)sAccelRawY) * SDL_STANDARD_GRAVITY;
        ctx->m_IMUScaleData.fAccelScaleZ = SWITCH_ACCEL_SCALE_MULT / ((float)sAccelSensCoeffZ - (float)sAccelRawZ) * SDL_STANDARD_GRAVITY;

        // Gyro scale
        ctx->m_IMUScaleData.fGyroScaleX = SWITCH_GYRO_SCALE_MULT / ((float)sGyroSensCoeffX - (float)sGyroRawX) * SDL_PI_F / 180.0f;
        ctx->m_IMUScaleData.fGyroScaleY = SWITCH_GYRO_SCALE_MULT / ((float)sGyroSensCoeffY - (float)sGyroRawY) * SDL_PI_F / 180.0f;
        ctx->m_IMUScaleData.fGyroScaleZ = SWITCH_GYRO_SCALE_MULT / ((float)sGyroSensCoeffZ - (float)sGyroRawZ) * SDL_PI_F / 180.0f;

    } else {
        // Use default values
        const float accelScale = SDL_STANDARD_GRAVITY / SWITCH_ACCEL_SCALE;
        const float gyroScale = SDL_PI_F / 180.0f / SWITCH_GYRO_SCALE;

        ctx->m_IMUScaleData.fAccelScaleX = accelScale;
        ctx->m_IMUScaleData.fAccelScaleY = accelScale;
        ctx->m_IMUScaleData.fAccelScaleZ = accelScale;

        ctx->m_IMUScaleData.fGyroScaleX = gyroScale;
        ctx->m_IMUScaleData.fGyroScaleY = gyroScale;
        ctx->m_IMUScaleData.fGyroScaleZ = gyroScale;

        ctx->m_IMUScaleData.sGyroOffsetX = 0;
        ctx->m_IMUScaleData.sGyroOffsetY = 0;
        ctx->m_IMUScaleData.sGyroOffsetZ = 0;
    }
    return true;
}

static Sint16 ApplyStickCalibration(SDL_DriverSwitch_Context *ctx, int nStick, int nAxis, Sint16 sRawValue)
{
    sRawValue -= ctx->m_StickCalData[nStick].axis[nAxis].sCenter;

    if (sRawValue >= 0) {
        if (sRawValue > ctx->m_StickExtents[nStick].axis[nAxis].sMax) {
            ctx->m_StickExtents[nStick].axis[nAxis].sMax = sRawValue;
        }
        return (Sint16)HIDAPI_RemapVal(sRawValue, 0, ctx->m_StickExtents[nStick].axis[nAxis].sMax, 0, SDL_MAX_SINT16);
    } else {
        if (sRawValue < ctx->m_StickExtents[nStick].axis[nAxis].sMin) {
            ctx->m_StickExtents[nStick].axis[nAxis].sMin = sRawValue;
        }
        return (Sint16)HIDAPI_RemapVal(sRawValue, ctx->m_StickExtents[nStick].axis[nAxis].sMin, 0, SDL_MIN_SINT16, 0);
    }
}

static Sint16 ApplySimpleStickCalibration(SDL_DriverSwitch_Context *ctx, int nStick, int nAxis, Sint16 sRawValue)
{
    // 0x8000 is the neutral value for all joystick axes
    const Uint16 usJoystickCenter = 0x8000;

    sRawValue -= usJoystickCenter;

    if (sRawValue >= 0) {
        if (sRawValue > ctx->m_SimpleStickExtents[nStick].axis[nAxis].sMax) {
            ctx->m_SimpleStickExtents[nStick].axis[nAxis].sMax = sRawValue;
        }
        return (Sint16)HIDAPI_RemapVal(sRawValue, 0, ctx->m_SimpleStickExtents[nStick].axis[nAxis].sMax, 0, SDL_MAX_SINT16);
    } else {
        if (sRawValue < ctx->m_SimpleStickExtents[nStick].axis[nAxis].sMin) {
            ctx->m_SimpleStickExtents[nStick].axis[nAxis].sMin = sRawValue;
        }
        return (Sint16)HIDAPI_RemapVal(sRawValue, ctx->m_SimpleStickExtents[nStick].axis[nAxis].sMin, 0, SDL_MIN_SINT16, 0);
    }
}

static Uint8 RemapButton(SDL_DriverSwitch_Context *ctx, Uint8 button)
{
    if (ctx->m_bUseButtonLabels) {
        // Use button labels instead of positions, e.g. Nintendo Online Classic controllers
        switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH:
            return SDL_GAMEPAD_BUTTON_EAST;
        case SDL_GAMEPAD_BUTTON_EAST:
            return SDL_GAMEPAD_BUTTON_SOUTH;
        case SDL_GAMEPAD_BUTTON_WEST:
            return SDL_GAMEPAD_BUTTON_NORTH;
        case SDL_GAMEPAD_BUTTON_NORTH:
            return SDL_GAMEPAD_BUTTON_WEST;
        default:
            break;
        }
    }
    return button;
}

static int GetMaxWriteAttempts(SDL_HIDAPI_Device *device)
{
    if (device->vendor_id == USB_VENDOR_NINTENDO &&
        device->product_id == USB_PRODUCT_NINTENDO_SWITCH_JOYCON_GRIP) {
        // This device is a little slow and we know we're always on USB
        return 20;
    } else {
        return 5;
    }
}

static ESwitchDeviceInfoControllerType ReadJoyConControllerType(SDL_HIDAPI_Device *device)
{
    ESwitchDeviceInfoControllerType eControllerType = k_eSwitchDeviceInfoControllerType_Unknown;
    const int MAX_ATTEMPTS = 1; // Don't try too long, in case this is a zombie Bluetooth controller
    int attempts = 0;

    // Create enough of a context to read the controller type from the device
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)SDL_calloc(1, sizeof(*ctx));
    if (ctx) {
        ctx->device = device;
        ctx->m_bSyncWrite = true;
        ctx->m_nMaxWriteAttempts = GetMaxWriteAttempts(device);

        for ( ; ; ) {
            ++attempts;
            if (device->is_bluetooth) {
                SwitchSubcommandInputPacket_t *reply = NULL;

                if (WriteSubcommand(ctx, k_eSwitchSubcommandIDs_RequestDeviceInfo, NULL, 0, &reply)) {
                    eControllerType = CalculateControllerType(ctx, (ESwitchDeviceInfoControllerType)reply->deviceInfo.ucDeviceType);
                }
            } else {
                if (WriteProprietary(ctx, k_eSwitchProprietaryCommandIDs_Status, NULL, 0, true)) {
                    SwitchProprietaryStatusPacket_t *status = (SwitchProprietaryStatusPacket_t *)&ctx->m_rgucReadBuffer[0];

                    eControllerType = CalculateControllerType(ctx, (ESwitchDeviceInfoControllerType)status->ucDeviceType);
                }
            }
            if (eControllerType == k_eSwitchDeviceInfoControllerType_Unknown && attempts < MAX_ATTEMPTS) {
                // Wait a bit and try again
                SDL_Delay(100);
                continue;
            }
            break;
        }
        SDL_free(ctx);
    }
    return eControllerType;
}

static bool HasHomeLED(SDL_DriverSwitch_Context *ctx)
{
    Uint16 vendor_id = ctx->device->vendor_id;
    Uint16 product_id = ctx->device->product_id;

    // The Power A Nintendo Switch Pro controllers don't have a Home LED
    if (vendor_id == 0 && product_id == 0) {
        return false;
    }

    // HORI Wireless Switch Pad
    if (vendor_id == 0x0f0d && product_id == 0x00f6) {
        return false;
    }

    // Third party controllers don't have a home LED and will shut off if we try to set it
    if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_Unknown ||
        ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_LicProController) {
        return false;
    }

    // The Nintendo Online classic controllers don't have a Home LED
    if (vendor_id == USB_VENDOR_NINTENDO &&
        ctx->m_eControllerType > k_eSwitchDeviceInfoControllerType_ProController) {
        return false;
    }

    return true;
}

static bool AlwaysUsesLabels(Uint16 vendor_id, Uint16 product_id, ESwitchDeviceInfoControllerType eControllerType)
{
    // Some controllers don't have a diamond button configuration, so should always use labels
    if (SDL_IsJoystickGameCube(vendor_id, product_id)) {
        return true;
    }
    switch (eControllerType) {
    case k_eSwitchDeviceInfoControllerType_HVCLeft:
    case k_eSwitchDeviceInfoControllerType_HVCRight:
    case k_eSwitchDeviceInfoControllerType_NESLeft:
    case k_eSwitchDeviceInfoControllerType_NESRight:
    case k_eSwitchDeviceInfoControllerType_N64:
    case k_eSwitchDeviceInfoControllerType_SEGA_Genesis:
        return true;
    default:
        return false;
    }
}

static void HIDAPI_DriverNintendoClassic_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_NINTENDO_CLASSIC, callback, userdata);
}

static void HIDAPI_DriverNintendoClassic_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_NINTENDO_CLASSIC, callback, userdata);
}

static bool HIDAPI_DriverNintendoClassic_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_NINTENDO_CLASSIC, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverNintendoClassic_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    if (vendor_id == USB_VENDOR_NINTENDO) {
        if (product_id == USB_PRODUCT_NINTENDO_SWITCH_JOYCON_RIGHT) {
            if (SDL_strncmp(name, "NES Controller", 14) == 0 ||
                SDL_strncmp(name, "HVC Controller", 14) == 0) {
                return true;
            }
        }

        if (product_id == USB_PRODUCT_NINTENDO_N64_CONTROLLER) {
            return true;
        }

        if (product_id == USB_PRODUCT_NINTENDO_SEGA_GENESIS_CONTROLLER) {
            return true;
        }

        if (product_id == USB_PRODUCT_NINTENDO_SNES_CONTROLLER) {
            return true;
        }
    }

    return false;
}

static void HIDAPI_DriverJoyCons_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, callback, userdata);
}

static void HIDAPI_DriverJoyCons_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, callback, userdata);
}

static bool HIDAPI_DriverJoyCons_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverJoyCons_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    if (vendor_id == USB_VENDOR_NINTENDO) {
        if (product_id == USB_PRODUCT_NINTENDO_SWITCH_PRO && device && device->dev) {
            // This might be a Kinvoca Joy-Con that reports VID/PID as a Switch Pro controller
            ESwitchDeviceInfoControllerType eControllerType = ReadJoyConControllerType(device);
            if (eControllerType == k_eSwitchDeviceInfoControllerType_JoyConLeft ||
                eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
                return true;
            }
        }

        if (product_id == USB_PRODUCT_NINTENDO_SWITCH_JOYCON_LEFT ||
            product_id == USB_PRODUCT_NINTENDO_SWITCH_JOYCON_RIGHT ||
            product_id == USB_PRODUCT_NINTENDO_SWITCH_JOYCON_GRIP) {
            return true;
        }
    }
    return false;
}

static void HIDAPI_DriverSwitch_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, callback, userdata);
}

static void HIDAPI_DriverSwitch_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, callback, userdata);
}

static bool HIDAPI_DriverSwitch_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverSwitch_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    /* The HORI Wireless Switch Pad enumerates as a HID device when connected via USB
       with the same VID/PID as when connected over Bluetooth but doesn't actually
       support communication over USB. The most reliable way to block this without allowing the
       controller to continually attempt to reconnect is to filter it out by manufacturer/product string.
       Note that the controller does have a different product string when connected over Bluetooth.
     */
    if (name && SDL_strcmp(name, "HORI Wireless Switch Pad") == 0) {
        return false;
    }

    // If it's handled by another driver, it's not handled here
    if (HIDAPI_DriverNintendoClassic_IsSupportedDevice(device, name, type, vendor_id, product_id, version, interface_number, interface_class, interface_subclass, interface_protocol) ||
        HIDAPI_DriverJoyCons_IsSupportedDevice(device, name, type, vendor_id, product_id, version, interface_number, interface_class, interface_subclass, interface_protocol)) {
        return false;
    }

    if (type != SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO) {
        return false;
    }

    // The Nintendo Switch 2 Pro uses another driver
    if (vendor_id == USB_VENDOR_NINTENDO && product_id == USB_PRODUCT_NINTENDO_SWITCH2_PRO) {
        return false;
    }
    return true;
}

static void UpdateDeviceIdentity(SDL_HIDAPI_Device *device)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)device->context;

    if (ctx->m_bInputOnly) {
        if (SDL_IsJoystickGameCube(device->vendor_id, device->product_id)) {
            device->type = SDL_GAMEPAD_TYPE_GAMECUBE;
        }
    } else {
        char serial[18];

        switch (ctx->m_eControllerType) {
        case k_eSwitchDeviceInfoControllerType_JoyConLeft:
            HIDAPI_SetDeviceName(device, "Nintendo Switch Joy-Con (L)");
            HIDAPI_SetDeviceProduct(device, USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH_JOYCON_LEFT);
            device->type = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT;
            break;
        case k_eSwitchDeviceInfoControllerType_JoyConRight:
            HIDAPI_SetDeviceName(device, "Nintendo Switch Joy-Con (R)");
            HIDAPI_SetDeviceProduct(device, USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH_JOYCON_RIGHT);
            device->type = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT;
            break;
        case k_eSwitchDeviceInfoControllerType_ProController:
        case k_eSwitchDeviceInfoControllerType_LicProController:
            HIDAPI_SetDeviceName(device, "Nintendo Switch Pro Controller");
            HIDAPI_SetDeviceProduct(device, USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH_PRO);
            device->type = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
            break;
        case k_eSwitchDeviceInfoControllerType_HVCLeft:
            HIDAPI_SetDeviceName(device, "Nintendo Family Computer Controller (1)");
            device->type = SDL_GAMEPAD_TYPE_STANDARD;
            break;
        case k_eSwitchDeviceInfoControllerType_HVCRight:
            HIDAPI_SetDeviceName(device, "Nintendo Family Computer Controller (2)");
            device->type = SDL_GAMEPAD_TYPE_STANDARD;
            break;
        case k_eSwitchDeviceInfoControllerType_NESLeft:
            HIDAPI_SetDeviceName(device, "Nintendo NES Controller (L)");
            device->type = SDL_GAMEPAD_TYPE_STANDARD;
            break;
        case k_eSwitchDeviceInfoControllerType_NESRight:
            HIDAPI_SetDeviceName(device, "Nintendo NES Controller (R)");
            device->type = SDL_GAMEPAD_TYPE_STANDARD;
            break;
        case k_eSwitchDeviceInfoControllerType_SNES:
            HIDAPI_SetDeviceName(device, "Nintendo SNES Controller");
            HIDAPI_SetDeviceProduct(device, USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SNES_CONTROLLER);
            device->type = SDL_GAMEPAD_TYPE_STANDARD;
            break;
        case k_eSwitchDeviceInfoControllerType_N64:
            HIDAPI_SetDeviceName(device, "Nintendo N64 Controller");
            HIDAPI_SetDeviceProduct(device, USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_N64_CONTROLLER);
            device->type = SDL_GAMEPAD_TYPE_STANDARD;
            break;
        case k_eSwitchDeviceInfoControllerType_SEGA_Genesis:
            HIDAPI_SetDeviceName(device, "Nintendo SEGA Genesis Controller");
            HIDAPI_SetDeviceProduct(device, USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SEGA_GENESIS_CONTROLLER);
            device->type = SDL_GAMEPAD_TYPE_STANDARD;
            break;
        case k_eSwitchDeviceInfoControllerType_Unknown:
            // We couldn't read the device info for this controller, might not be fully compliant
            if (device->vendor_id == USB_VENDOR_NINTENDO) {
                switch (device->product_id) {
                case USB_PRODUCT_NINTENDO_SWITCH_JOYCON_LEFT:
                    ctx->m_eControllerType = k_eSwitchDeviceInfoControllerType_JoyConLeft;
                    HIDAPI_SetDeviceName(device, "Nintendo Switch Joy-Con (L)");
                    device->type = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT;
                    break;
                case USB_PRODUCT_NINTENDO_SWITCH_JOYCON_RIGHT:
                    ctx->m_eControllerType = k_eSwitchDeviceInfoControllerType_JoyConRight;
                    HIDAPI_SetDeviceName(device, "Nintendo Switch Joy-Con (R)");
                    device->type = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT;
                    break;
                case USB_PRODUCT_NINTENDO_SWITCH_PRO:
                    ctx->m_eControllerType = k_eSwitchDeviceInfoControllerType_ProController;
                    HIDAPI_SetDeviceName(device, "Nintendo Switch Pro Controller");
                    device->type = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
                    break;
                default:
                    break;
                }
            }
            return;
        default:
            device->type = SDL_GAMEPAD_TYPE_STANDARD;
            break;
        }
        device->guid.data[15] = ctx->m_eControllerType;

        (void)SDL_snprintf(serial, sizeof(serial), "%.2x-%.2x-%.2x-%.2x-%.2x-%.2x",
                           ctx->m_rgucMACAddress[0],
                           ctx->m_rgucMACAddress[1],
                           ctx->m_rgucMACAddress[2],
                           ctx->m_rgucMACAddress[3],
                           ctx->m_rgucMACAddress[4],
                           ctx->m_rgucMACAddress[5]);
        HIDAPI_SetDeviceSerial(device, serial);
    }
}

static bool HIDAPI_DriverSwitch_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverSwitch_Context *ctx;

    ctx = (SDL_DriverSwitch_Context *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;

    ctx->m_nMaxWriteAttempts = GetMaxWriteAttempts(device);
    ctx->m_bSyncWrite = true;

    // Find out whether or not we can send output reports
    // Third party controllers use the full Switch Pro wireless protocol over Bluetooth
    if (!device->is_bluetooth) {
        ctx->m_bInputOnly = SDL_IsJoystickNintendoSwitchProInputOnly(device->vendor_id, device->product_id) ||
                            SDL_IsJoystickNintendoSwitch2ProInputOnly(device->vendor_id, device->product_id);
    }
    ctx->m_bSwitch2 = SDL_IsJoystickNintendoSwitch2Pro(device->vendor_id, device->product_id);

    if (!ctx->m_bInputOnly) {
        // Initialize rumble data, important for reading device info on the MOBAPAD M073
        SetNeutralRumble(device, &ctx->m_RumblePacket.rumbleData[0]);
        SetNeutralRumble(device, &ctx->m_RumblePacket.rumbleData[1]);

        BReadDeviceInfo(ctx);
    }
    UpdateDeviceIdentity(device);

    // Prefer the USB device over the Bluetooth device
    if (device->is_bluetooth) {
        if (HIDAPI_HasConnectedUSBDevice(device->serial)) {
            return true;
        }
    } else {
        HIDAPI_DisconnectBluetoothDevice(device->serial);
    }
    return HIDAPI_JoystickConnected(device, NULL);
}

static int HIDAPI_DriverSwitch_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverSwitch_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)device->context;

    if (!ctx->joystick) {
        return;
    }

    ctx->m_nPlayerIndex = player_index;

    UpdateSlotLED(ctx);
}

static bool HIDAPI_DriverSwitch_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)device->context;

    SDL_AssertJoysticksLocked();

    ctx->joystick = joystick;

    ctx->m_bSyncWrite = true;

    if (!ctx->m_bInputOnly) {
#ifdef SDL_PLATFORM_MACOS
        // Wait for the OS to finish its handshake with the controller
        SDL_Delay(250);
#endif
        GetInitialInputMode(ctx);
        ctx->m_nCurrentInputMode = ctx->m_nInitialInputMode;

        // Initialize rumble data
        SetNeutralRumble(device, &ctx->m_RumblePacket.rumbleData[0]);
        SetNeutralRumble(device, &ctx->m_RumblePacket.rumbleData[1]);

        if (!device->is_bluetooth) {
            if (!BTrySetupUSB(ctx)) {
                SDL_SetError("Couldn't setup USB mode");
                return false;
            }
        }

        if (!LoadStickCalibration(ctx)) {
            SDL_SetError("Couldn't load stick calibration");
            return false;
        }

        if (ctx->m_eControllerType != k_eSwitchDeviceInfoControllerType_HVCLeft &&
            ctx->m_eControllerType != k_eSwitchDeviceInfoControllerType_HVCRight &&
            ctx->m_eControllerType != k_eSwitchDeviceInfoControllerType_NESLeft &&
            ctx->m_eControllerType != k_eSwitchDeviceInfoControllerType_NESRight &&
            ctx->m_eControllerType != k_eSwitchDeviceInfoControllerType_SNES &&
            ctx->m_eControllerType != k_eSwitchDeviceInfoControllerType_N64 &&
            ctx->m_eControllerType != k_eSwitchDeviceInfoControllerType_SEGA_Genesis &&
            !(device->vendor_id == USB_VENDOR_PDP && device->product_id == USB_PRODUCT_PDP_REALMZ_WIRELESS)) {
            if (LoadIMUCalibration(ctx)) {
                ctx->m_bSensorsSupported = true;
            }
        }

        // Enable vibration
        SetVibrationEnabled(ctx, 1);

        // Set desired input mode
        SDL_AddHintCallback(SDL_HINT_JOYSTICK_ENHANCED_REPORTS,
                            SDL_EnhancedReportsChanged, ctx);

        // Start sending USB reports
        if (!device->is_bluetooth) {
            // ForceUSB doesn't generate an ACK, so don't wait for a reply
            if (!WriteProprietary(ctx, k_eSwitchProprietaryCommandIDs_ForceUSB, NULL, 0, false)) {
                SDL_SetError("Couldn't start USB reports");
                return false;
            }
        }

        // Set the LED state
        if (HasHomeLED(ctx)) {
            if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConLeft ||
                ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
                SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_JOYCON_HOME_LED,
                                    SDL_HomeLEDHintChanged, ctx);
            } else {
                SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_SWITCH_HOME_LED,
                                    SDL_HomeLEDHintChanged, ctx);
            }
        }
    }

    if (AlwaysUsesLabels(device->vendor_id, device->product_id, ctx->m_eControllerType)) {
        ctx->m_bUseButtonLabels = true;
    }

    // Initialize player index (needed for setting LEDs)
    ctx->m_nPlayerIndex = SDL_GetJoystickPlayerIndex(joystick);
    ctx->m_bPlayerLights = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_SWITCH_PLAYER_LED, true);
    UpdateSlotLED(ctx);

    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_SWITCH_PLAYER_LED,
                        SDL_PlayerLEDHintChanged, ctx);

    // NFC machine: fresh per open (full re-init on reconnect). The hint is
    // polled on the update loop, so there is no callback to register and no
    // callback lifetime to manage across device recombination.
    ctx->m_ucNfcState = k_eSwitchNfcState_Idle;
    ctx->m_bNfcActive = false;
    ctx->m_bNfcTagPresent = false;
    ctx->m_ucNfcRounds = 0;

    // Initialize the joystick capabilities
    if (ctx->m_bSwitch2) {
        joystick->nbuttons = SDL_GAMEPAD_NUM_SWITCH2_BUTTONS;
    } else if (ctx->m_bInputOnly) {
        joystick->nbuttons = SDL_GAMEPAD_NUM_SWITCH_INPUT_ONLY_BUTTONS;
    } else {
        joystick->nbuttons = SDL_GAMEPAD_NUM_SWITCH_BUTTONS;
    }
    /* A standalone right Joy-Con exposes one extra axis beyond the gamepad
       axes for the NIR camera's average-intensity scalar (0 until the camera
       is enabled via SDL_HINT_JOYSTICK_HIDAPI_JOYCON_IR_SENSOR + sensors). */
    if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight &&
        !device->parent) {
        joystick->naxes = SDL_GAMEPAD_AXIS_COUNT + 1;
    } else {
        joystick->naxes = SDL_GAMEPAD_AXIS_COUNT;
    }
    joystick->nhats = 1;

    // Set up for input
    ctx->m_bSyncWrite = false;
    ctx->m_ulLastIMUReset = ctx->m_ulLastInput = SDL_GetTicks();
    ctx->m_ulIMUUpdateIntervalNS = SDL_MS_TO_NS(5); // Start off at 5 ms update rate

    // Set up for vertical mode
    ctx->m_bVerticalMode = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, false);

    return true;
}

/* Frequency-shaped rumble (fork issue #25, opt-in via
 * SDL_HINT_JOYSTICK_HIDAPI_SWITCH_SHAPED_RUMBLE, default off): sweep LRA
 * frequency with intensity instead of holding the fixed carriers, which is
 * where native HD-rumble texture lives. The constants are ENCODED center
 * bytes per dekuNukem rumble_data_table.md (freq = 10 * 2^(enc/32) Hz), so a
 * linear sweep in encoded space is a logarithmic sweep in Hz, matching the
 * LRA's perceptual scale with no float math. The low motor sweeps the low
 * band and the high motor the high band. Band edges, the attack boost, and
 * the decay divisor are bench-tunable; the structure is not. */
#define SHAPED_RUMBLE_LOW_ENC_MIN  0x41 // ~40.9 Hz
#define SHAPED_RUMBLE_LOW_ENC_MAX  0x80 // 160 Hz (the fixed low carrier upstream uses)
#define SHAPED_RUMBLE_HIGH_ENC_MIN 0x80 // 160 Hz
#define SHAPED_RUMBLE_HIGH_ENC_MAX 0xA0 // 320 Hz (hf 0x0100, the table's canonical row)

static Uint8 ShapedRumbleEncByte(Uint8 ucEncMin, Uint8 ucEncMax, Uint16 usIntensity)
{
    return (Uint8)(ucEncMin + (((Uint32)(ucEncMax - ucEncMin) * usIntensity) / 65535));
}

/* The shaped encode path. Envelopes ride the driver's existing pending
 * machinery rather than new timers: a rising edge writes one attack frame
 * (amplitude boosted 1.5x, capped) and schedules the sustain re-encode by
 * setting the rumble-pending flags, which the update loop flushes one write
 * window (~30 ms) later. A fall to zero writes one decay frame (half the
 * previous intensity at the band floor) and schedules the true zero the same
 * way, so pulsed rumble reads as attack/body/tail texture instead of gated
 * tone. */
static bool HIDAPI_DriverSwitch_ShapedRumbleJoystick(SDL_DriverSwitch_Context *ctx, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    Uint16 usPrevLow = ctx->m_usShapedPrevLow;
    Uint16 usPrevHigh = ctx->m_usShapedPrevHigh;
    Uint16 usAmpLow, usAmpHigh;
    Uint8 ucLowEnc, ucHighEnc;
    Uint16 usHighFreq;
    Uint8 ucLowFreq;

    if (low_frequency_rumble || high_frequency_rumble) {
        bool bAttack = (low_frequency_rumble && !usPrevLow) || (high_frequency_rumble && !usPrevHigh);

        usAmpLow = low_frequency_rumble;
        usAmpHigh = high_frequency_rumble;
        if (low_frequency_rumble && !usPrevLow) {
            usAmpLow = (Uint16)SDL_min(65535, ((Uint32)low_frequency_rumble * 3) / 2);
        }
        if (high_frequency_rumble && !usPrevHigh) {
            usAmpHigh = (Uint16)SDL_min(65535, ((Uint32)high_frequency_rumble * 3) / 2);
        }
        ucLowEnc = ShapedRumbleEncByte(SHAPED_RUMBLE_LOW_ENC_MIN, SHAPED_RUMBLE_LOW_ENC_MAX, low_frequency_rumble);
        ucHighEnc = ShapedRumbleEncByte(SHAPED_RUMBLE_HIGH_ENC_MIN, SHAPED_RUMBLE_HIGH_ENC_MAX, high_frequency_rumble);
        if (bAttack) {
            // Settle to sustain amplitude one write window later
            Uint32 unRumblePending = ((Uint32)low_frequency_rumble << 16) | high_frequency_rumble;
            if (unRumblePending > ctx->m_unRumblePending) {
                ctx->m_unRumblePending = unRumblePending;
            }
            ctx->m_bRumblePending = true;
            ctx->m_bRumbleZeroPending = false;
        }
        ctx->m_usShapedPrevLow = low_frequency_rumble;
        ctx->m_usShapedPrevHigh = high_frequency_rumble;
        ctx->m_bRumbleActive = true;
    } else if (usPrevLow || usPrevHigh) {
        // Decay tail, then the true zero via the zero-pending flag
        usAmpLow = (Uint16)(usPrevLow / 2);
        usAmpHigh = (Uint16)(usPrevHigh / 2);
        ucLowEnc = SHAPED_RUMBLE_LOW_ENC_MIN;
        ucHighEnc = SHAPED_RUMBLE_HIGH_ENC_MIN;
        ctx->m_bRumbleZeroPending = true;
        ctx->m_usShapedPrevLow = 0;
        ctx->m_usShapedPrevHigh = 0;
        ctx->m_bRumbleActive = true;
    } else {
        SetNeutralRumble(ctx->device, &ctx->m_RumblePacket.rumbleData[0]);
        SetNeutralRumble(ctx->device, &ctx->m_RumblePacket.rumbleData[1]);
        ctx->m_bRumbleActive = false;
        if (!WriteRumble(ctx)) {
            return SDL_SetError("Couldn't send rumble packet");
        }
        return true;
    }

    // Pack per dekuNukem: high-band 16-bit = (enc - 0x60) << 2, low-band
    // 8-bit = enc - 0x40 (EncodeRumble borrows the spare bits)
    ucLowFreq = (Uint8)(ucLowEnc - 0x40);
    usHighFreq = (Uint16)((ucHighEnc - 0x60) << 2);
    EncodeRumble(ctx->device, &ctx->m_RumblePacket.rumbleData[0], usHighFreq, EncodeRumbleHighAmplitude(usAmpHigh), ucLowFreq, EncodeRumbleLowAmplitude(usAmpLow));
    EncodeRumble(ctx->device, &ctx->m_RumblePacket.rumbleData[1], usHighFreq, EncodeRumbleHighAmplitude(usAmpHigh), ucLowFreq, EncodeRumbleLowAmplitude(usAmpLow));

    if (!WriteRumble(ctx)) {
        return SDL_SetError("Couldn't send rumble packet");
    }
    return true;
}

static bool HIDAPI_DriverSwitch_ActuallyRumbleJoystick(SDL_DriverSwitch_Context *ctx, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    /* Experimentally determined rumble values. These will only matter on some controllers as tested ones
     * seem to disregard these and just use any non-zero rumble values as a binary flag for constant rumble
     *
     * More information about these values can be found here:
     * https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/rumble_data_table.md
     */
    const Uint16 k_usHighFreq = 0x0074;
    const Uint8 k_ucHighFreqAmp = EncodeRumbleHighAmplitude(high_frequency_rumble);
    const Uint8 k_ucLowFreq = 0x3D;
    const Uint16 k_usLowFreqAmp = EncodeRumbleLowAmplitude(low_frequency_rumble);

    /* Shaped mode targets the classic LRA packet only: Switch 2 report
       encoding is its own path and stays untouched (fork issue #25). */
    if (!ctx->m_bSwitch2 &&
        SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_SWITCH_SHAPED_RUMBLE, false)) {
        return HIDAPI_DriverSwitch_ShapedRumbleJoystick(ctx, low_frequency_rumble, high_frequency_rumble);
    }
    ctx->m_usShapedPrevLow = 0;
    ctx->m_usShapedPrevHigh = 0;

    if (low_frequency_rumble || high_frequency_rumble) {
        EncodeRumble(ctx->device, &ctx->m_RumblePacket.rumbleData[0], k_usHighFreq, k_ucHighFreqAmp, k_ucLowFreq, k_usLowFreqAmp);
        EncodeRumble(ctx->device, &ctx->m_RumblePacket.rumbleData[1], k_usHighFreq, k_ucHighFreqAmp, k_ucLowFreq, k_usLowFreqAmp);
    } else {
        SetNeutralRumble(ctx->device, &ctx->m_RumblePacket.rumbleData[0]);
        SetNeutralRumble(ctx->device, &ctx->m_RumblePacket.rumbleData[1]);
    }

    ctx->m_bRumbleActive = (low_frequency_rumble || high_frequency_rumble);

    if (!WriteRumble(ctx)) {
        return SDL_SetError("Couldn't send rumble packet");
    }
    return true;
}

static bool HIDAPI_DriverSwitch_SendPendingRumble(SDL_DriverSwitch_Context *ctx)
{
    if (SDL_GetTicks() < (ctx->m_ulRumbleSent + RUMBLE_WRITE_FREQUENCY_MS)) {
        return true;
    }

    if (ctx->m_bRumblePending) {
        Uint16 low_frequency_rumble = (Uint16)(ctx->m_unRumblePending >> 16);
        Uint16 high_frequency_rumble = (Uint16)ctx->m_unRumblePending;

#ifdef DEBUG_RUMBLE
        SDL_Log("Sent pending rumble %d/%d, %d ms after previous rumble", low_frequency_rumble, high_frequency_rumble, SDL_GetTicks() - ctx->m_ulRumbleSent);
#endif
        ctx->m_bRumblePending = false;
        ctx->m_unRumblePending = 0;

        return HIDAPI_DriverSwitch_ActuallyRumbleJoystick(ctx, low_frequency_rumble, high_frequency_rumble);
    }

    if (ctx->m_bRumbleZeroPending) {
        ctx->m_bRumbleZeroPending = false;

#ifdef DEBUG_RUMBLE
        SDL_Log("Sent pending zero rumble, %d ms after previous rumble", SDL_GetTicks() - ctx->m_ulRumbleSent);
#endif
        return HIDAPI_DriverSwitch_ActuallyRumbleJoystick(ctx, 0, 0);
    }

    return true;
}

static bool HIDAPI_DriverSwitch_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)device->context;

    if (ctx->m_bInputOnly) {
        return SDL_Unsupported();
    }

    if (device->parent) {
        if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConLeft) {
            // Just handle low frequency rumble
            high_frequency_rumble = 0;
        } else if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
            // Just handle high frequency rumble
            low_frequency_rumble = 0;
        }
    }

    if (ctx->m_bRumblePending) {
        if (!HIDAPI_DriverSwitch_SendPendingRumble(ctx)) {
            return false;
        }
    }

    if (SDL_GetTicks() < (ctx->m_ulRumbleSent + RUMBLE_WRITE_FREQUENCY_MS)) {
        if (low_frequency_rumble || high_frequency_rumble) {
            Uint32 unRumblePending = ((Uint32)low_frequency_rumble << 16) | high_frequency_rumble;

            // Keep the highest rumble intensity in the given interval
            if (unRumblePending > ctx->m_unRumblePending) {
                ctx->m_unRumblePending = unRumblePending;
            }
            ctx->m_bRumblePending = true;
            ctx->m_bRumbleZeroPending = false;
        } else {
            // When rumble is complete, turn it off
            ctx->m_bRumbleZeroPending = true;
        }
        return true;
    }

#ifdef DEBUG_RUMBLE
    SDL_Log("Sent rumble %d/%d", low_frequency_rumble, high_frequency_rumble);
#endif

    return HIDAPI_DriverSwitch_ActuallyRumbleJoystick(ctx, low_frequency_rumble, high_frequency_rumble);
}

static bool HIDAPI_DriverSwitch_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverSwitch_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)device->context;
    Uint32 result = 0;

    if (ctx->m_bPlayerLights && !ctx->m_bInputOnly) {
        result |= SDL_JOYSTICK_CAP_PLAYER_LED;
    }

    if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_ProController && !ctx->m_bInputOnly) {
        // Doesn't have an RGB LED, so don't return SDL_JOYSTICK_CAP_RGB_LED here
        result |= SDL_JOYSTICK_CAP_RUMBLE;
        // But has the HOME LED, so treat it like a mono LED
        result |= SDL_JOYSTICK_CAP_MONO_LED;
    } else if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConLeft ||
               ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
        result |= SDL_JOYSTICK_CAP_RUMBLE;
        if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
            result |= SDL_JOYSTICK_CAP_MONO_LED; // Right JoyCon also have the HOME LED
        }
    }
    return result;
}

static bool HIDAPI_DriverSwitch_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)device->context;

    if (!(ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_ProController && !ctx->m_bInputOnly) &&
        ctx->m_eControllerType != k_eSwitchDeviceInfoControllerType_JoyConRight) {
        return SDL_Unsupported();
    }

    int value = (int)((SDL_max(red, SDL_max(green, blue)) / 255.0f) * 100.0f); // The colors are received between 0-255 and we need them to be 0-100
    return SetHomeLED(ctx, (Uint8)value);
}

static bool HIDAPI_DriverSwitch_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)device->context;

    if (size == sizeof(SwitchCommonOutputPacket_t)) {
        const SwitchCommonOutputPacket_t *packet = (SwitchCommonOutputPacket_t *)data;

        if (packet->ucPacketType != k_eSwitchOutputReportIDs_Rumble) {
            return SDL_SetError("Unknown Nintendo Switch Pro effect type");
        }

        SDL_copyp(&ctx->m_RumblePacket.rumbleData[0], &packet->rumbleData[0]);
        SDL_copyp(&ctx->m_RumblePacket.rumbleData[1], &packet->rumbleData[1]);
        if (!WriteRumble(ctx)) {
            return false;
        }

        // This overwrites any internal rumble
        ctx->m_bRumblePending = false;
        ctx->m_bRumbleZeroPending = false;
        return true;
    } else if (size >= 2 && size <= 256) {
        const Uint8 *payload = (const Uint8 *)data;
        ESwitchSubcommandIDs cmd = (ESwitchSubcommandIDs)payload[0];

        if (cmd == k_eSwitchSubcommandIDs_SetInputReportMode && !device->is_bluetooth) {
            // Going into simple mode over USB disables input reports, so don't do that
            return true;
        }
        if (cmd == k_eSwitchSubcommandIDs_SetHomeLight && !HasHomeLED(ctx)) {
            // Setting the home LED when it's not supported can cause the controller to reset
            return true;
        }

        if (!WriteSubcommand(ctx, cmd, &payload[1], (Uint8)(size - 1), NULL)) {
            return false;
        }
        return true;
    }
    return SDL_Unsupported();
}

static bool HIDAPI_DriverSwitch_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)device->context;

    UpdateEnhancedModeOnApplicationUsage(ctx);

    if (!ctx->m_bSensorsSupported || (enabled && !ctx->m_bEnhancedMode)) {
        return SDL_Unsupported();
    }

    ctx->m_bReportSensors = enabled;
    ctx->m_unIMUSamples = 0;
    ctx->m_ulIMUSampleTimestampNS = SDL_GetTicksNS();

    /* The NIR machine arms itself from the update loop when sensors + the
       app hint demand it (UpdateIR), so there is nothing to start here.
       Disable is immediate and synchronous so the camera powers down and
       the axis parks on this edge rather than a tick later; it keys on MCU
       ownership, not the hint, so clearing the hint mid-session cannot
       strand a powered camera, and it also stops a bring-up in flight. */
    if (!enabled && IsIROwningMcu(ctx)) {
        DisableIRSensor(ctx);
    }

    UpdateInputMode(ctx);
    SetIMUEnabled(ctx, enabled);

    return true;
}

static void HandleInputOnlyControllerState(SDL_Joystick *joystick, SDL_DriverSwitch_Context *ctx, SwitchInputOnlyControllerStatePacket_t *packet)
{
    Sint16 axis;
    Uint64 timestamp = SDL_GetTicksNS();

    if (packet->rgucButtons[0] != ctx->m_lastInputOnlyState.rgucButtons[0]) {
        Uint8 data = packet->rgucButtons[0];
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_SOUTH), ((data & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_EAST), ((data & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_WEST), ((data & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_NORTH), ((data & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data & 0x20) != 0));
    }

    if (packet->rgucButtons[1] != ctx->m_lastInputOnlyState.rgucButtons[1]) {
        Uint8 data = packet->rgucButtons[1];
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, ((data & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_SHARE, ((data & 0x20) != 0));
    }

    if (packet->ucStickHat != ctx->m_lastInputOnlyState.ucStickHat) {
        Uint8 hat;

        if (ctx->m_bSwitch2) {
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH2_C, ((packet->ucStickHat & 0x80) != 0));
        }

        switch (packet->ucStickHat & 0x0F) {
        case 0:
            hat = SDL_HAT_UP;
            break;
        case 1:
            hat = SDL_HAT_RIGHTUP;
            break;
        case 2:
            hat = SDL_HAT_RIGHT;
            break;
        case 3:
            hat = SDL_HAT_RIGHTDOWN;
            break;
        case 4:
            hat = SDL_HAT_DOWN;
            break;
        case 5:
            hat = SDL_HAT_LEFTDOWN;
            break;
        case 6:
            hat = SDL_HAT_LEFT;
            break;
        case 7:
            hat = SDL_HAT_LEFTUP;
            break;
        default:
            hat = SDL_HAT_CENTERED;
            break;
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);
    }

    axis = (packet->rgucButtons[0] & 0x40) ? 32767 : -32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis);

    axis = (packet->rgucButtons[0] & 0x80) ? 32767 : -32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis);

    if (packet->rgucJoystickLeft[0] != ctx->m_lastInputOnlyState.rgucJoystickLeft[0]) {
        axis = (Sint16)HIDAPI_RemapVal(packet->rgucJoystickLeft[0], SDL_MIN_UINT8, SDL_MAX_UINT8, SDL_MIN_SINT16, SDL_MAX_SINT16);
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, axis);
    }

    if (packet->rgucJoystickLeft[1] != ctx->m_lastInputOnlyState.rgucJoystickLeft[1]) {
        axis = (Sint16)HIDAPI_RemapVal(packet->rgucJoystickLeft[1], SDL_MIN_UINT8, SDL_MAX_UINT8, SDL_MIN_SINT16, SDL_MAX_SINT16);
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, axis);
    }

    if (packet->rgucJoystickRight[0] != ctx->m_lastInputOnlyState.rgucJoystickRight[0]) {
        axis = (Sint16)HIDAPI_RemapVal(packet->rgucJoystickRight[0], SDL_MIN_UINT8, SDL_MAX_UINT8, SDL_MIN_SINT16, SDL_MAX_SINT16);
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis);
    }

    if (packet->rgucJoystickRight[1] != ctx->m_lastInputOnlyState.rgucJoystickRight[1]) {
        axis = (Sint16)HIDAPI_RemapVal(packet->rgucJoystickRight[1], SDL_MIN_UINT8, SDL_MAX_UINT8, SDL_MIN_SINT16, SDL_MAX_SINT16);
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, axis);
    }

    ctx->m_lastInputOnlyState = *packet;
}

static void HandleCombinedSimpleControllerStateL(Uint64 timestamp, SDL_Joystick *joystick, SDL_DriverSwitch_Context *ctx, SwitchSimpleStatePacket_t *packet)
{
    if (packet->rgucButtons[0] != ctx->m_lastSimpleState.rgucButtons[0]) {
        Uint8 data = packet->rgucButtons[0];
        Uint8 hat = 0;

        if (data & 0x01) {
            hat |= SDL_HAT_LEFT;
        }
        if (data & 0x02) {
            hat |= SDL_HAT_DOWN;
        }
        if (data & 0x04) {
            hat |= SDL_HAT_UP;
        }
        if (data & 0x08) {
            hat |= SDL_HAT_RIGHT;
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);

        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_LEFT_PADDLE1, ((data & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_LEFT_PADDLE2, ((data & 0x20) != 0));
    }

    if (packet->rgucButtons[1] != ctx->m_lastSimpleState.rgucButtons[1]) {
        Uint8 data = packet->rgucButtons[1];
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_SHARE, ((data & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data & 0x40) != 0));
    }

    Sint16 axis = (packet->rgucButtons[1] & 0x80) ? 32767 : -32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis);

    if (packet->ucStickHat != ctx->m_lastSimpleState.ucStickHat) {
        switch (packet->ucStickHat) {
        case 0:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, 0);
            break;
        case 1:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX);
            break;
        case 2:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX);
            break;
        case 3:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX);
            break;
        case 4:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, 0);
            break;
        case 5:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);
            break;
        case 6:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);
            break;
        case 7:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);
            break;
        default:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, 0);
            break;
        }
    }
}

static void HandleCombinedSimpleControllerStateR(Uint64 timestamp, SDL_Joystick *joystick, SDL_DriverSwitch_Context *ctx, SwitchSimpleStatePacket_t *packet)
{
    if (packet->rgucButtons[0] != ctx->m_lastSimpleState.rgucButtons[0]) {
        Uint8 data = packet->rgucButtons[0];
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_EAST), ((data & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_NORTH), ((data & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_SOUTH), ((data & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_WEST), ((data & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_RIGHT_PADDLE2, ((data & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_RIGHT_PADDLE1, ((data & 0x20) != 0));
    }

    if (packet->rgucButtons[1] != ctx->m_lastSimpleState.rgucButtons[1]) {
        Uint8 data = packet->rgucButtons[1];
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, ((data & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data & 0x40) != 0));
    }

    Sint16 axis = (packet->rgucButtons[1] & 0x80) ? 32767 : -32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis);

    if (packet->ucStickHat != ctx->m_lastSimpleState.ucStickHat) {
        switch (packet->ucStickHat) {
        case 0:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, SDL_JOYSTICK_AXIS_MIN);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, 0);
            break;
        case 1:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, SDL_JOYSTICK_AXIS_MIN);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, SDL_JOYSTICK_AXIS_MIN);
            break;
        case 2:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, 0);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, SDL_JOYSTICK_AXIS_MIN);
            break;
        case 3:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, SDL_JOYSTICK_AXIS_MAX);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, SDL_JOYSTICK_AXIS_MIN);
            break;
        case 4:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, SDL_JOYSTICK_AXIS_MAX);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, 0);
            break;
        case 5:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, SDL_JOYSTICK_AXIS_MAX);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, SDL_JOYSTICK_AXIS_MAX);
            break;
        case 6:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, 0);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, SDL_JOYSTICK_AXIS_MAX);
            break;
        case 7:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, SDL_JOYSTICK_AXIS_MIN);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, SDL_JOYSTICK_AXIS_MAX);
            break;
        default:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, 0);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, 0);
            break;
        }
    }
}

static void HandleMiniSimpleControllerStateL(Uint64 timestamp, SDL_Joystick *joystick, SDL_DriverSwitch_Context *ctx, SwitchSimpleStatePacket_t *packet)
{
    if (packet->rgucButtons[0] != ctx->m_lastSimpleState.rgucButtons[0]) {
        Uint8 data = packet->rgucButtons[0];
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_SOUTH), ((data & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_EAST), ((data & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_WEST), ((data & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_NORTH), ((data & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data & 0x20) != 0));
    }

    if (packet->rgucButtons[1] != ctx->m_lastSimpleState.rgucButtons[1]) {
        Uint8 data = packet->rgucButtons[1];
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_LEFT_PADDLE1, ((data & 0x40) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_LEFT_PADDLE2, ((data & 0x80) != 0));
    }

    if (packet->ucStickHat != ctx->m_lastSimpleState.ucStickHat) {
        switch (packet->ucStickHat) {
        case 0:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);
            break;
        case 1:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);
            break;
        case 2:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, 0);
            break;
        case 3:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX);
            break;
        case 4:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX);
            break;
        case 5:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX);
            break;
        case 6:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, 0);
            break;
        case 7:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);
            break;
        default:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, 0);
            break;
        }
    }
}

static void HandleMiniSimpleControllerStateR(Uint64 timestamp, SDL_Joystick *joystick, SDL_DriverSwitch_Context *ctx, SwitchSimpleStatePacket_t *packet)
{
    if (packet->rgucButtons[0] != ctx->m_lastSimpleState.rgucButtons[0]) {
        Uint8 data = packet->rgucButtons[0];
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_SOUTH), ((data & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_EAST), ((data & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_WEST), ((data & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_NORTH), ((data & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data & 0x20) != 0));
    }

    if (packet->rgucButtons[1] != ctx->m_lastSimpleState.rgucButtons[1]) {
        Uint8 data = packet->rgucButtons[1];
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_SHARE, ((data & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_RIGHT_PADDLE1, ((data & 0x40) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_RIGHT_PADDLE2, ((data & 0x80) != 0));
    }

    if (packet->ucStickHat != ctx->m_lastSimpleState.ucStickHat) {
        switch (packet->ucStickHat) {
        case 0:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);
            break;
        case 1:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);
            break;
        case 2:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, 0);
            break;
        case 3:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX);
            break;
        case 4:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX);
            break;
        case 5:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX);
            break;
        case 6:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, 0);
            break;
        case 7:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);
            break;
        default:
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, 0);
            break;
        }
    }
}

static void HandleSimpleControllerState(SDL_Joystick *joystick, SDL_DriverSwitch_Context *ctx, SwitchSimpleStatePacket_t *packet)
{
    Uint64 timestamp = SDL_GetTicksNS();

    if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConLeft) {
        if (ctx->device->parent || ctx->m_bVerticalMode) {
            HandleCombinedSimpleControllerStateL(timestamp, joystick, ctx, packet);
        } else {
            HandleMiniSimpleControllerStateL(timestamp, joystick, ctx, packet);
        }
    } else if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
        if (ctx->device->parent || ctx->m_bVerticalMode) {
            HandleCombinedSimpleControllerStateR(timestamp, joystick, ctx, packet);
        } else {
            HandleMiniSimpleControllerStateR(timestamp, joystick, ctx, packet);
        }
    } else {
        Sint16 axis;

        if (packet->rgucButtons[0] != ctx->m_lastSimpleState.rgucButtons[0]) {
            Uint8 data = packet->rgucButtons[0];
            SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_SOUTH), ((data & 0x01) != 0));
            SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_EAST), ((data & 0x02) != 0));
            SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_WEST), ((data & 0x04) != 0));
            SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_NORTH), ((data & 0x08) != 0));
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data & 0x10) != 0));
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data & 0x20) != 0));
        }

        if (packet->rgucButtons[1] != ctx->m_lastSimpleState.rgucButtons[1]) {
            Uint8 data = packet->rgucButtons[1];
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data & 0x01) != 0));
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data & 0x02) != 0));
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data & 0x04) != 0));
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, ((data & 0x08) != 0));
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data & 0x10) != 0));
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_SHARE, ((data & 0x20) != 0));
        }

        if (packet->ucStickHat != ctx->m_lastSimpleState.ucStickHat) {
            Uint8 hat;

            switch (packet->ucStickHat) {
            case 0:
                hat = SDL_HAT_UP;
                break;
            case 1:
                hat = SDL_HAT_RIGHTUP;
                break;
            case 2:
                hat = SDL_HAT_RIGHT;
                break;
            case 3:
                hat = SDL_HAT_RIGHTDOWN;
                break;
            case 4:
                hat = SDL_HAT_DOWN;
                break;
            case 5:
                hat = SDL_HAT_LEFTDOWN;
                break;
            case 6:
                hat = SDL_HAT_LEFT;
                break;
            case 7:
                hat = SDL_HAT_LEFTUP;
                break;
            default:
                hat = SDL_HAT_CENTERED;
                break;
            }
            SDL_SendJoystickHat(timestamp, joystick, 0, hat);
        }

        axis = (packet->rgucButtons[0] & 0x40) ? 32767 : -32768;
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis);

        axis = ((packet->rgucButtons[0] & 0x80) || (packet->rgucButtons[1] & 0x80)) ? 32767 : -32768;
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis);

        axis = ApplySimpleStickCalibration(ctx, 0, 0, packet->sJoystickLeft[0]);
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, axis);

        axis = ApplySimpleStickCalibration(ctx, 0, 1, packet->sJoystickLeft[1]);
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, axis);

        axis = ApplySimpleStickCalibration(ctx, 1, 0, packet->sJoystickRight[0]);
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis);

        axis = ApplySimpleStickCalibration(ctx, 1, 1, packet->sJoystickRight[1]);
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, axis);
    }

    ctx->m_lastSimpleState = *packet;
}

static void SendSensorUpdate(Uint64 timestamp, SDL_Joystick *joystick, SDL_DriverSwitch_Context *ctx, SDL_SensorType type, Uint64 sensor_timestamp, const Sint16 *values)
{
    float data[3];

    /* Note the order of components has been shuffled to match PlayStation controllers,
     * since that's our de facto standard from already supporting those controllers, and
     * users will want consistent axis mappings across devices.
     */
    if (type == SDL_SENSOR_GYRO || type == SDL_SENSOR_GYRO_L || type == SDL_SENSOR_GYRO_R) {
        const float gyroX = (float)(values[0] - ctx->m_IMUScaleData.sGyroOffsetX);
        const float gyroY = (float)(values[1] - ctx->m_IMUScaleData.sGyroOffsetY);
        const float gyroZ = (float)(values[2] - ctx->m_IMUScaleData.sGyroOffsetZ);

        data[0] = -(ctx->m_IMUScaleData.fGyroScaleY * gyroY);
        data[1] = ctx->m_IMUScaleData.fGyroScaleZ * gyroZ;
        data[2] = -(ctx->m_IMUScaleData.fGyroScaleX * gyroX);
    } else {
        data[0] = -(ctx->m_IMUScaleData.fAccelScaleY * (float)values[1]);
        data[1] = ctx->m_IMUScaleData.fAccelScaleZ * (float)values[2];
        data[2] = -(ctx->m_IMUScaleData.fAccelScaleX * (float)values[0]);
    }

    // Right Joy-Con flips some axes, so let's flip them back for consistency
    if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
        data[0] = -data[0];
        data[1] = -data[1];
    }

    if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConLeft &&
        !ctx->device->parent && !ctx->m_bVerticalMode) {
        // Mini-gamepad mode, swap some axes around
        float tmp = data[2];
        data[2] = -data[0];
        data[0] = tmp;
    }

    if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight &&
        !ctx->device->parent && !ctx->m_bVerticalMode) {
        // Mini-gamepad mode, swap some axes around
        float tmp = data[2];
        data[2] = data[0];
        data[0] = -tmp;
    }

    SDL_SendJoystickSensor(timestamp, joystick, type, sensor_timestamp, data, 3);
}

static void HandleCombinedControllerStateL(Uint64 timestamp, SDL_Joystick *joystick, SDL_DriverSwitch_Context *ctx, SwitchStatePacket_t *packet)
{
    Sint16 axis;

    if (packet->controllerState.rgucButtons[1] != ctx->m_lastFullState.controllerState.rgucButtons[1]) {
        Uint8 data = packet->controllerState.rgucButtons[1];
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_SHARE, ((data & 0x20) != 0));
    }

    if (packet->controllerState.rgucButtons[2] != ctx->m_lastFullState.controllerState.rgucButtons[2]) {
        Uint8 data = packet->controllerState.rgucButtons[2];
        Uint8 hat = 0;

        if (data & 0x01) {
            hat |= SDL_HAT_DOWN;
        }
        if (data & 0x02) {
            hat |= SDL_HAT_UP;
        }
        if (data & 0x04) {
            hat |= SDL_HAT_RIGHT;
        }
        if (data & 0x08) {
            hat |= SDL_HAT_LEFT;
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);

        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_LEFT_PADDLE2, ((data & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_LEFT_PADDLE1, ((data & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data & 0x40) != 0));
        axis = (data & 0x80) ? 32767 : -32768;
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis);
    }

    axis = packet->controllerState.rgucJoystickLeft[0] | ((packet->controllerState.rgucJoystickLeft[1] & 0xF) << 8);
    axis = ApplyStickCalibration(ctx, 0, 0, axis);
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, axis);

    axis = ((packet->controllerState.rgucJoystickLeft[1] & 0xF0) >> 4) | (packet->controllerState.rgucJoystickLeft[2] << 4);
    axis = ApplyStickCalibration(ctx, 0, 1, axis);
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, ~axis);
}

static void HandleCombinedControllerStateR(Uint64 timestamp, SDL_Joystick *joystick, SDL_DriverSwitch_Context *ctx, SwitchStatePacket_t *packet)
{
    Sint16 axis;

    if (packet->controllerState.rgucButtons[0] != ctx->m_lastFullState.controllerState.rgucButtons[0]) {
        Uint8 data = packet->controllerState.rgucButtons[0];
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_SOUTH), ((data & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_EAST), ((data & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_WEST), ((data & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_NORTH), ((data & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_RIGHT_PADDLE1, ((data & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_RIGHT_PADDLE2, ((data & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data & 0x40) != 0));
        axis = (data & 0x80) ? 32767 : -32768;
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis);
    }

    if (packet->controllerState.rgucButtons[1] != ctx->m_lastFullState.controllerState.rgucButtons[1]) {
        Uint8 data = packet->controllerState.rgucButtons[1];
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, ((data & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data & 0x10) != 0));
    }

    axis = packet->controllerState.rgucJoystickRight[0] | ((packet->controllerState.rgucJoystickRight[1] & 0xF) << 8);
    axis = ApplyStickCalibration(ctx, 1, 0, axis);
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis);

    axis = ((packet->controllerState.rgucJoystickRight[1] & 0xF0) >> 4) | (packet->controllerState.rgucJoystickRight[2] << 4);
    axis = ApplyStickCalibration(ctx, 1, 1, axis);
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, ~axis);
}

static void HandleMiniControllerStateL(Uint64 timestamp, SDL_Joystick *joystick, SDL_DriverSwitch_Context *ctx, SwitchStatePacket_t *packet)
{
    Sint16 axis;

    if (packet->controllerState.rgucButtons[1] != ctx->m_lastFullState.controllerState.rgucButtons[1]) {
        Uint8 data = packet->controllerState.rgucButtons[1];
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data & 0x20) != 0));
    }

    if (packet->controllerState.rgucButtons[2] != ctx->m_lastFullState.controllerState.rgucButtons[2]) {
        Uint8 data = packet->controllerState.rgucButtons[2];
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_SOUTH), ((data & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_EAST), ((data & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_WEST), ((data & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_NORTH), ((data & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_LEFT_PADDLE1, ((data & 0x40) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_LEFT_PADDLE2, ((data & 0x80) != 0));
    }

    axis = packet->controllerState.rgucJoystickLeft[0] | ((packet->controllerState.rgucJoystickLeft[1] & 0xF) << 8);
    axis = ApplyStickCalibration(ctx, 0, 0, axis);
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, ~axis);

    axis = ((packet->controllerState.rgucJoystickLeft[1] & 0xF0) >> 4) | (packet->controllerState.rgucJoystickLeft[2] << 4);
    axis = ApplyStickCalibration(ctx, 0, 1, axis);
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, ~axis);
}

static void HandleMiniControllerStateR(Uint64 timestamp, SDL_Joystick *joystick, SDL_DriverSwitch_Context *ctx, SwitchStatePacket_t *packet)
{
    Sint16 axis;

    if (packet->controllerState.rgucButtons[0] != ctx->m_lastFullState.controllerState.rgucButtons[0]) {
        Uint8 data = packet->controllerState.rgucButtons[0];
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_SOUTH), ((data & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_EAST), ((data & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_WEST), ((data & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_NORTH), ((data & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_RIGHT_PADDLE1, ((data & 0x40) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_RIGHT_PADDLE2, ((data & 0x80) != 0));
    }

    if (packet->controllerState.rgucButtons[1] != ctx->m_lastFullState.controllerState.rgucButtons[1]) {
        Uint8 data = packet->controllerState.rgucButtons[1];
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data & 0x10) != 0));
    }

    axis = packet->controllerState.rgucJoystickRight[0] | ((packet->controllerState.rgucJoystickRight[1] & 0xF) << 8);
    axis = ApplyStickCalibration(ctx, 1, 0, axis);
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, axis);

    axis = ((packet->controllerState.rgucJoystickRight[1] & 0xF0) >> 4) | (packet->controllerState.rgucJoystickRight[2] << 4);
    axis = ApplyStickCalibration(ctx, 1, 1, axis);
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, axis);
}

static void HandleFullControllerState(SDL_Joystick *joystick, SDL_DriverSwitch_Context *ctx, SwitchStatePacket_t *packet) SDL_NO_THREAD_SAFETY_ANALYSIS // We unlock and lock the device lock to be able to change IMU state
{
    Uint64 timestamp = SDL_GetTicksNS();

    if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConLeft) {
        if (ctx->device->parent || ctx->m_bVerticalMode) {
            HandleCombinedControllerStateL(timestamp, joystick, ctx, packet);
        } else {
            HandleMiniControllerStateL(timestamp, joystick, ctx, packet);
        }
    } else if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
        if (ctx->device->parent || ctx->m_bVerticalMode) {
            HandleCombinedControllerStateR(timestamp, joystick, ctx, packet);
        } else {
            HandleMiniControllerStateR(timestamp, joystick, ctx, packet);
        }
    } else {
        Sint16 axis;

        if (packet->controllerState.rgucButtons[0] != ctx->m_lastFullState.controllerState.rgucButtons[0]) {
            Uint8 data = packet->controllerState.rgucButtons[0];
            SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_SOUTH), ((data & 0x04) != 0));
            SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_EAST), ((data & 0x08) != 0));
            SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_WEST), ((data & 0x01) != 0));
            SDL_SendJoystickButton(timestamp, joystick, RemapButton(ctx, SDL_GAMEPAD_BUTTON_NORTH), ((data & 0x02) != 0));
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data & 0x40) != 0));
        }

        if (packet->controllerState.rgucButtons[1] != ctx->m_lastFullState.controllerState.rgucButtons[1]) {
            Uint8 data = packet->controllerState.rgucButtons[1];
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data & 0x01) != 0));
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data & 0x02) != 0));
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, ((data & 0x04) != 0));
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data & 0x08) != 0));

            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data & 0x10) != 0));
            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH_SHARE, ((data & 0x20) != 0));
            if (ctx->m_bSwitch2) {
                SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SWITCH2_C, ((data & 0x40) != 0));
            }
        }

        if (packet->controllerState.rgucButtons[2] != ctx->m_lastFullState.controllerState.rgucButtons[2]) {
            Uint8 data = packet->controllerState.rgucButtons[2];
            Uint8 hat = 0;

            if (data & 0x01) {
                hat |= SDL_HAT_DOWN;
            }
            if (data & 0x02) {
                hat |= SDL_HAT_UP;
            }
            if (data & 0x04) {
                hat |= SDL_HAT_RIGHT;
            }
            if (data & 0x08) {
                hat |= SDL_HAT_LEFT;
            }
            SDL_SendJoystickHat(timestamp, joystick, 0, hat);

            SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data & 0x40) != 0));
        }

        axis = (packet->controllerState.rgucButtons[0] & 0x80) ? 32767 : -32768;
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis);

        axis = (packet->controllerState.rgucButtons[2] & 0x80) ? 32767 : -32768;
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis);

        axis = packet->controllerState.rgucJoystickLeft[0] | ((packet->controllerState.rgucJoystickLeft[1] & 0xF) << 8);
        axis = ApplyStickCalibration(ctx, 0, 0, axis);
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, axis);

        axis = ((packet->controllerState.rgucJoystickLeft[1] & 0xF0) >> 4) | (packet->controllerState.rgucJoystickLeft[2] << 4);
        axis = ApplyStickCalibration(ctx, 0, 1, axis);
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, ~axis);

        axis = packet->controllerState.rgucJoystickRight[0] | ((packet->controllerState.rgucJoystickRight[1] & 0xF) << 8);
        axis = ApplyStickCalibration(ctx, 1, 0, axis);
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis);

        axis = ((packet->controllerState.rgucJoystickRight[1] & 0xF0) >> 4) | (packet->controllerState.rgucJoystickRight[2] << 4);
        axis = ApplyStickCalibration(ctx, 1, 1, axis);
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, ~axis);
    }

    /* High nibble of battery/connection byte is battery level, low nibble is connection status (always 0 on 8BitDo Pro 2)
     * LSB of connection nibble is USB/Switch connection status
     * LSB of the battery nibble is used to report charging.
     * The battery level is reported from 0(empty)-8(full)
     */
    int charging = (packet->controllerState.ucBatteryAndConnection & 0x10);
    int level = (packet->controllerState.ucBatteryAndConnection & 0xE0) >> 4;
    if (charging) {
        if (level == 8) {
            ctx->m_ePowerState = SDL_POWERSTATE_CHARGED;
        } else {
            ctx->m_ePowerState = SDL_POWERSTATE_CHARGING;
        }
    } else {
        ctx->m_ePowerState = SDL_POWERSTATE_ON_BATTERY;
    }
    ctx->m_nPowerPercent = (int)SDL_roundf((level / 8.0f) * 100.0f);

    if (!ctx->device->parent) {
        SDL_PowerState state = ctx->m_ePowerState;
        int percent = ctx->m_nPowerPercent;
        SDL_SendJoystickPowerInfo(joystick, state, percent);
    } else if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
        SDL_DriverSwitch_Context *other = (SDL_DriverSwitch_Context *)ctx->device->parent->children[0]->context;
        SDL_PowerState state = (SDL_PowerState)SDL_min(ctx->m_ePowerState, other->m_ePowerState);
        int percent = SDL_min(ctx->m_nPowerPercent, other->m_nPowerPercent);
        SDL_SendJoystickPowerInfo(joystick, state, percent);
    }

    if (ctx->m_bReportSensors) {
        // Need to copy the imuState to an aligned variable
        SwitchControllerIMUState_t imuState[3];
        SDL_COMPILE_TIME_ASSERT(imuState_size, sizeof(imuState) == sizeof(packet->imuState));
        SDL_memcpy(imuState, packet->imuState, sizeof(packet->imuState));

        bool bHasSensorData = (imuState[0].sAccelZ != 0 ||
                               imuState[0].sAccelY != 0 ||
                               imuState[0].sAccelX != 0);
        if (bHasSensorData) {
            const Uint32 IMU_UPDATE_RATE_SAMPLE_FREQUENCY = 1000;
            Uint64 sensor_timestamp[3];

            ctx->m_bHasSensorData = true;

            // We got three IMU samples, calculate the IMU update rate and timestamps
            ctx->m_unIMUSamples += 3;
            if (ctx->m_unIMUSamples >= IMU_UPDATE_RATE_SAMPLE_FREQUENCY) {
                Uint64 now = SDL_GetTicksNS();
                Uint64 elapsed = (now - ctx->m_ulIMUSampleTimestampNS);

                if (elapsed > 0) {
                    ctx->m_ulIMUUpdateIntervalNS = elapsed / ctx->m_unIMUSamples;
                }
                ctx->m_unIMUSamples = 0;
                ctx->m_ulIMUSampleTimestampNS = now;
            }

            ctx->m_ulTimestampNS += ctx->m_ulIMUUpdateIntervalNS;
            sensor_timestamp[0] = ctx->m_ulTimestampNS;
            ctx->m_ulTimestampNS += ctx->m_ulIMUUpdateIntervalNS;
            sensor_timestamp[1] = ctx->m_ulTimestampNS;
            ctx->m_ulTimestampNS += ctx->m_ulIMUUpdateIntervalNS;
            sensor_timestamp[2] = ctx->m_ulTimestampNS;

            if (!ctx->device->parent ||
                ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_GYRO, sensor_timestamp[0], &imuState[2].sGyroX);
                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_ACCEL, sensor_timestamp[0], &imuState[2].sAccelX);

                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_GYRO, sensor_timestamp[1], &imuState[1].sGyroX);
                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_ACCEL, sensor_timestamp[1], &imuState[1].sAccelX);

                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_GYRO, sensor_timestamp[2], &imuState[0].sGyroX);
                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_ACCEL, sensor_timestamp[2], &imuState[0].sAccelX);
            }

            if (ctx->device->parent &&
                ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConLeft) {
                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_GYRO_L, sensor_timestamp[0], &imuState[2].sGyroX);
                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_ACCEL_L, sensor_timestamp[0], &imuState[2].sAccelX);

                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_GYRO_L, sensor_timestamp[1], &imuState[1].sGyroX);
                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_ACCEL_L, sensor_timestamp[1], &imuState[1].sAccelX);

                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_GYRO_L, sensor_timestamp[2], &imuState[0].sGyroX);
                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_ACCEL_L, sensor_timestamp[2], &imuState[0].sAccelX);
            }
            if (ctx->device->parent &&
                ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_GYRO_R, sensor_timestamp[0], &imuState[2].sGyroX);
                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_ACCEL_R, sensor_timestamp[0], &imuState[2].sAccelX);

                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_GYRO_R, sensor_timestamp[1], &imuState[1].sGyroX);
                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_ACCEL_R, sensor_timestamp[1], &imuState[1].sAccelX);

                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_GYRO_R, sensor_timestamp[2], &imuState[0].sGyroX);
                SendSensorUpdate(timestamp, joystick, ctx, SDL_SENSOR_ACCEL_R, sensor_timestamp[2], &imuState[0].sAccelX);
            }

        } else if (ctx->m_bHasSensorData) {
            // Uh oh, someone turned off the IMU?
            const int IMU_RESET_DELAY_MS = 3000;
            Uint64 now = SDL_GetTicks();

            if (now >= (ctx->m_ulLastIMUReset + IMU_RESET_DELAY_MS)) {
                SetIMUEnabled(ctx, true);
                ctx->m_ulLastIMUReset = now;
            }

        } else {
            // We have never gotten IMU data, probably not supported on this device
        }
    }

    ctx->m_lastFullState = *packet;
}

static bool HIDAPI_DriverSwitch_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    int size;
    int packet_count = 0;
    Uint64 now = SDL_GetTicks();

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }

    while ((size = ReadInput(ctx)) > 0) {
#ifdef DEBUG_SWITCH_PROTOCOL
        HIDAPI_DumpPacket("Nintendo Switch packet: size = %d", ctx->m_rgucReadBuffer, size);
#endif
        ++packet_count;
        ctx->m_ulLastInput = now;

        if (!joystick) {
            continue;
        }

        if (ctx->m_bInputOnly) {
            HandleInputOnlyControllerState(joystick, ctx, (SwitchInputOnlyControllerStatePacket_t *)&ctx->m_rgucReadBuffer[0]);
        } else {
            if (ctx->m_rgucReadBuffer[0] == k_eSwitchInputReportIDs_SubcommandReply) {
                if (ctx->m_bNfcActive) {
                    HandleNfcSubcommandReply(ctx, joystick, now, size);
                } else if (IsIROwningMcu(ctx)) {
                    HandleIRSubcommandReply(ctx, now, size);
                }
                continue;
            }

            ctx->m_nCurrentInputMode = ctx->m_rgucReadBuffer[0];

            switch (ctx->m_rgucReadBuffer[0]) {
            case k_eSwitchInputReportIDs_SimpleControllerState:
                HandleSimpleControllerState(joystick, ctx, (SwitchSimpleStatePacket_t *)&ctx->m_rgucReadBuffer[1]);
                break;
            case k_eSwitchInputReportIDs_FullControllerState:
            case k_eSwitchInputReportIDs_FullControllerAndMcuState:
                // This is the extended report, we can enable sensors now in auto mode
                UpdateEnhancedModeOnEnhancedReport(ctx);

                HandleFullControllerState(joystick, ctx, (SwitchStatePacket_t *)&ctx->m_rgucReadBuffer[1]);

                /* The NFC/IR report's MCU tail: byte 49 = the MCU report type
                   (0x01 status, 0x03 IR image data). The IR stats header ends
                   by byte 58, within the 64-byte read buffer even though the
                   full 361-byte report is truncated. */
                if (IsIROwningMcu(ctx) &&
                    ctx->m_rgucReadBuffer[0] == k_eSwitchInputReportIDs_FullControllerAndMcuState) {
                    HandleIRMcuReport(ctx, joystick, now, size);
                }
                if (ctx->m_bNfcActive &&
                    ctx->m_rgucReadBuffer[0] == k_eSwitchInputReportIDs_FullControllerAndMcuState) {
                    HandleNfcMcuReport(ctx, joystick, now, size);
                }
                if (ctx->m_ucIRState == k_eSwitchIRState_Streaming &&
                    ctx->m_rgucReadBuffer[0] == k_eSwitchInputReportIDs_FullControllerState &&
                    now >= ctx->m_ulIRModeFixTicks + SWITCH_IR_MODE_FIX_PACE_MS) {
                    /* Report-mode watchdog, mirroring the NFC one: a 0x30
                       report while the camera streams means an external
                       raw writer knocked the input mode, and without this
                       the camera strands until reconnect (PadForge#248:
                       its haptic-writer guard has an unavoidable TOCTOU
                       window). Restart the bring-up, paced to one attempt
                       per second; a bring-up that keeps failing lands in
                       the Failed backoff rather than looping here. */
                    ctx->m_ulIRModeFixTicks = now;
                    RestartIR(ctx, now, "input mode knocked to 0x30");
                }
                break;
            default:
                break;
            }
        }
    }

    if (joystick) {
        if (packet_count == 0) {
            if (!ctx->m_bInputOnly && !device->is_bluetooth &&
                ctx->device->product_id != USB_PRODUCT_NINTENDO_SWITCH_JOYCON_GRIP) {
                const int INPUT_WAIT_TIMEOUT_MS = 100;
                const int FORCE_USB_MIN_INTERVAL_MS = 1000;
                /* Steam may have put the controller back into non-reporting
                   mode. Nudge it ONE shot per interval, never once per tick:
                   unbounded, this fired every tick once input stalled, a
                   continuous 80 04 stream at the ~8 ms endpoint interval
                   that suppressed the very reporting it was checking for
                   and convoyed a 1 kHz consumer to 110-120 Hz
                   (issue #15, named by a write-discriminator capture). */
                if (now >= (ctx->m_ulLastInput + INPUT_WAIT_TIMEOUT_MS) &&
                    now >= (ctx->m_ulForceUSBTicks + FORCE_USB_MIN_INTERVAL_MS)) {
                    bool wasSyncWrite = ctx->m_bSyncWrite;

                    ctx->m_ulForceUSBTicks = now;
                    ctx->m_bSyncWrite = true;
                    WriteProprietary(ctx, k_eSwitchProprietaryCommandIDs_ForceUSB, NULL, 0, false);
                    ctx->m_bSyncWrite = wasSyncWrite;
                }
            } else if (device->is_bluetooth &&
                       ctx->m_nCurrentInputMode != k_eSwitchInputReportIDs_SimpleControllerState) {
                const int INPUT_WAIT_TIMEOUT_MS = 3000;
                if (now >= (ctx->m_ulLastInput + INPUT_WAIT_TIMEOUT_MS)) {
                    // Bluetooth may have disconnected, try reopening the controller
                    size = -1;
                }
            }
        }

        UpdateIR(ctx, joystick, now);
        UpdateNfc(ctx, joystick, now);

        if (ctx->m_bRumblePending || ctx->m_bRumbleZeroPending) {
            HIDAPI_DriverSwitch_SendPendingRumble(ctx);
        } else if (ctx->m_bRumbleActive &&
                   now >= (ctx->m_ulRumbleSent + RUMBLE_REFRESH_FREQUENCY_MS)) {
#ifdef DEBUG_RUMBLE
            SDL_Log("Sent continuing rumble, %d ms after previous rumble", now - ctx->m_ulRumbleSent);
#endif
            WriteRumble(ctx);
        }
    }

    // Reconnect the Bluetooth device once the USB device is gone
    if (device->num_joysticks == 0 && device->is_bluetooth && packet_count > 0 &&
        !device->parent &&
        !HIDAPI_HasConnectedUSBDevice(device->serial)) {
        HIDAPI_JoystickConnected(device, NULL);
    }

    if (size < 0 && device->num_joysticks > 0) {
        // Read error, device is disconnected
        HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
    }
    return (size >= 0);
}

static void HIDAPI_DriverSwitch_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverSwitch_Context *ctx = (SDL_DriverSwitch_Context *)device->context;

    if (!ctx->m_bInputOnly) {
        // Power the NFC/IR MCU back down so the camera does not drain the
        // battery, whether it was streaming or still mid-bring-up
        if (IsIROwningMcu(ctx)) {
            DisableIRSensor(ctx);
            /* If the controller started in full mode, the simple-mode restore
               below will not run, so restore full mode here rather than leave
               the controller parked on the NFC/IR report. */
            if (ctx->m_nInitialInputMode == k_eSwitchInputReportIDs_FullControllerState) {
                SetInputMode(ctx, k_eSwitchInputReportIDs_FullControllerState);
            }
        }

        // Restore simple input mode for other applications
        if (!ctx->m_nInitialInputMode ||
            ctx->m_nInitialInputMode == k_eSwitchInputReportIDs_SimpleControllerState) {
            SetInputMode(ctx, k_eSwitchInputReportIDs_SimpleControllerState);
        }
    }

    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_ENHANCED_REPORTS,
                        SDL_EnhancedReportsChanged, ctx);

    if (ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConLeft ||
        ctx->m_eControllerType == k_eSwitchDeviceInfoControllerType_JoyConRight) {
        SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_JOYCON_HOME_LED,
                            SDL_HomeLEDHintChanged, ctx);
    } else {
        SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_SWITCH_HOME_LED,
                            SDL_HomeLEDHintChanged, ctx);
    }

    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_SWITCH_PLAYER_LED,
                        SDL_PlayerLEDHintChanged, ctx);

    if (ctx->m_ucNfcState != k_eSwitchNfcState_Idle &&
        ctx->m_ucNfcState != k_eSwitchNfcState_Failed) {
        /* Synchronous MCU suspend, like DisableIRSensor: no later update
           tick is guaranteed after close. The input report mode is NOT
           restored here; close's own mode restoration owns the final mode
           and runs before this point. */
        Uint8 ucOff = 0x00;
        WriteSubcommand(ctx, k_eSwitchSubcommandIDs_SetMCUState, &ucOff, sizeof(ucOff), NULL);
    }
    AbandonNfc(ctx, joystick);

    ctx->joystick = NULL;

    ctx->m_bReportSensors = false;
    ctx->m_bEnhancedMode = false;
    ctx->m_bEnhancedModeAvailable = false;
}

static void HIDAPI_DriverSwitch_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverNintendoClassic = {
    SDL_HINT_JOYSTICK_HIDAPI_NINTENDO_CLASSIC,
    true,
    HIDAPI_DriverNintendoClassic_RegisterHints,
    HIDAPI_DriverNintendoClassic_UnregisterHints,
    HIDAPI_DriverNintendoClassic_IsEnabled,
    HIDAPI_DriverNintendoClassic_IsSupportedDevice,
    HIDAPI_DriverSwitch_InitDevice,
    HIDAPI_DriverSwitch_GetDevicePlayerIndex,
    HIDAPI_DriverSwitch_SetDevicePlayerIndex,
    HIDAPI_DriverSwitch_UpdateDevice,
    HIDAPI_DriverSwitch_OpenJoystick,
    HIDAPI_DriverSwitch_RumbleJoystick,
    HIDAPI_DriverSwitch_RumbleJoystickTriggers,
    HIDAPI_DriverSwitch_GetJoystickCapabilities,
    HIDAPI_DriverSwitch_SetJoystickLED,
    HIDAPI_DriverSwitch_SendJoystickEffect,
    HIDAPI_DriverSwitch_SetJoystickSensorsEnabled,
    HIDAPI_DriverSwitch_CloseJoystick,
    HIDAPI_DriverSwitch_FreeDevice,
};

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverJoyCons = {
    SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS,
    true,
    HIDAPI_DriverJoyCons_RegisterHints,
    HIDAPI_DriverJoyCons_UnregisterHints,
    HIDAPI_DriverJoyCons_IsEnabled,
    HIDAPI_DriverJoyCons_IsSupportedDevice,
    HIDAPI_DriverSwitch_InitDevice,
    HIDAPI_DriverSwitch_GetDevicePlayerIndex,
    HIDAPI_DriverSwitch_SetDevicePlayerIndex,
    HIDAPI_DriverSwitch_UpdateDevice,
    HIDAPI_DriverSwitch_OpenJoystick,
    HIDAPI_DriverSwitch_RumbleJoystick,
    HIDAPI_DriverSwitch_RumbleJoystickTriggers,
    HIDAPI_DriverSwitch_GetJoystickCapabilities,
    HIDAPI_DriverSwitch_SetJoystickLED,
    HIDAPI_DriverSwitch_SendJoystickEffect,
    HIDAPI_DriverSwitch_SetJoystickSensorsEnabled,
    HIDAPI_DriverSwitch_CloseJoystick,
    HIDAPI_DriverSwitch_FreeDevice,
};

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverSwitch = {
    SDL_HINT_JOYSTICK_HIDAPI_SWITCH,
    true,
    HIDAPI_DriverSwitch_RegisterHints,
    HIDAPI_DriverSwitch_UnregisterHints,
    HIDAPI_DriverSwitch_IsEnabled,
    HIDAPI_DriverSwitch_IsSupportedDevice,
    HIDAPI_DriverSwitch_InitDevice,
    HIDAPI_DriverSwitch_GetDevicePlayerIndex,
    HIDAPI_DriverSwitch_SetDevicePlayerIndex,
    HIDAPI_DriverSwitch_UpdateDevice,
    HIDAPI_DriverSwitch_OpenJoystick,
    HIDAPI_DriverSwitch_RumbleJoystick,
    HIDAPI_DriverSwitch_RumbleJoystickTriggers,
    HIDAPI_DriverSwitch_GetJoystickCapabilities,
    HIDAPI_DriverSwitch_SetJoystickLED,
    HIDAPI_DriverSwitch_SendJoystickEffect,
    HIDAPI_DriverSwitch_SetJoystickSensorsEnabled,
    HIDAPI_DriverSwitch_CloseJoystick,
    HIDAPI_DriverSwitch_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_SWITCH

#endif // SDL_JOYSTICK_HIDAPI
