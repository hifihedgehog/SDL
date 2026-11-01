/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_HIDAPI_DriverSwitch, the Nintendo Switch driver, against a
   scripted Pro Controller, for hifihedgehog/SDL#42. Over Bluetooth the
   joystick reports the firmware version the device info reply starts with,
   major then minor (dekuNukem Nintendo_Switch_Reverse_Engineering,
   bluetooth_hid_subcommands_notes.md, subcommand 0x02: sample 03 48 for
   3.72). Over USB the driver reads the proprietary status packet instead,
   which carries no version, and the joystick reports 0.

   The scripted controller answers each subcommand with report 0x21: the
   acknowledgment in byte 13 with bit 7 set, the subcommand in byte 14 and
   the data from byte 15 (bluetooth_hid_notes.md, the subcommand reply). An
   SPI flash read echoes its address and length and returns erased flash,
   0xFF, which the driver reads as no calibration. Over USB it answers each
   proprietary command with report 0x81, and the status command with the
   device type in byte 3 and the MAC address from byte 4.

   1. A Pro Controller over Bluetooth with firmware 3.72 opens with 0x0348.
   2. As the second half of a combined pair opens the shared joystick, a
      half that read a version replaces the first half's, and a half that
      read none leaves it.
   3. A Pro Controller over USB opens with 0. */

#define SDL_MAIN_HANDLED
#include "testfirmwarefake.h"

#include "../src/joystick/hidapi/SDL_hidapi_switch.c"

#define DEVICE_INFO_MAJOR 0x03
#define DEVICE_INFO_MINOR 0x48

static int subcommands;
static int device_info_requests;
static int proprietary_commands;

static void QueueSubcommandReply(Uint8 subcommand, const Uint8 *request_data)
{
    Uint8 report[64];
    SwitchSubcommandInputPacket_t *reply = (SwitchSubcommandInputPacket_t *)&report[1];

    SDL_zeroa(report);
    report[0] = k_eSwitchInputReportIDs_SubcommandReply;
    reply->m_controllerState.ucBatteryAndConnection = 0x8E; /* full, powered */
    reply->ucSubcommandAck = 0x80;
    reply->ucSubcommandID = subcommand;
    switch (subcommand) {
    case k_eSwitchSubcommandIDs_RequestDeviceInfo:
        ++device_info_requests;
        reply->ucSubcommandAck = 0x82;
        reply->deviceInfo.rgucFirmwareVersion[0] = DEVICE_INFO_MAJOR;
        reply->deviceInfo.rgucFirmwareVersion[1] = DEVICE_INFO_MINOR;
        reply->deviceInfo.ucDeviceType = k_eSwitchDeviceInfoControllerType_ProController;
        reply->deviceInfo.ucFiller1 = 0x02;
        reply->deviceInfo.rgucMACAddress[0] = 0x7C;
        reply->deviceInfo.rgucMACAddress[5] = 0x57;
        reply->deviceInfo.ucFiller2 = 0x01;
        reply->deviceInfo.ucColorLocation = 0x01;
        break;
    case k_eSwitchSubcommandIDs_SPIFlashRead:
        reply->ucSubcommandAck = 0x90;
        SDL_memcpy(&reply->spiReadData.opData, request_data, sizeof(reply->spiReadData.opData));
        SDL_memset(reply->spiReadData.rgucReadData, 0xFF, sizeof(reply->spiReadData.rgucReadData));
        break;
    default:
        break;
    }
    Fake_Queue(report, sizeof(report));
}

static void QueueProprietaryAck(Uint8 command)
{
    Uint8 report[64];

    SDL_zeroa(report);
    report[0] = k_eSwitchInputReportIDs_CommandAck;
    report[1] = command;
    if (command == k_eSwitchProprietaryCommandIDs_Status) {
        SwitchProprietaryStatusPacket_t *status = (SwitchProprietaryStatusPacket_t *)report;

        status->ucDeviceType = k_eSwitchDeviceInfoControllerType_ProController;
        status->rgucMACAddress[0] = 0x57;
        status->rgucMACAddress[5] = 0x7C;
    }
    Fake_Queue(report, sizeof(report));
}

void Firmware_Respond(const Uint8 *data, int size, bool async)
{
    (void)async;
    if (data[0] == k_eSwitchOutputReportIDs_RumbleAndSubcommand && size >= (int)sizeof(SwitchSubcommandOutputPacket_t)) {
        const SwitchSubcommandOutputPacket_t *packet = (const SwitchSubcommandOutputPacket_t *)data;

        ++subcommands;
        QueueSubcommandReply(packet->ucSubcommandID, packet->rgucSubcommandData);
    } else if (data[0] == k_eSwitchOutputReportIDs_Proprietary && size >= 2) {
        ++proprietary_commands;
        /* ForceUSB and the commands no reply is awaited for get none */
        if (data[1] != k_eSwitchProprietaryCommandIDs_ForceUSB) {
            QueueProprietaryAck(data[1]);
        }
    }
    /* Rumble alone, report 0x10, gets no answer */
}

static SDL_DriverSwitch_Context *Ctx(void)
{
    return (SDL_DriverSwitch_Context *)fake_device.context;
}

static bool StartPad(bool bluetooth)
{
    Fake_Reset();
    subcommands = 0;
    device_info_requests = 0;
    proprietary_commands = 0;
    fake_now = 1000;
    SDL_zero(fake_device);
    fake_device.dev = Fake_Handle();
    fake_device.name = SDL_strdup("Pro Controller");
    fake_device.path = SDL_strdup("fake");
    fake_device.serial = SDL_strdup("7c-bb-8a-ea-30-57");
    fake_device.vendor_id = USB_VENDOR_NINTENDO;
    fake_device.product_id = USB_PRODUCT_NINTENDO_SWITCH_PRO;
    fake_device.is_bluetooth = bluetooth;
    fake_device.guid = SDL_CreateJoystickGUID(bluetooth ? SDL_HARDWARE_BUS_BLUETOOTH : SDL_HARDWARE_BUS_USB,
                                              USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH_PRO, 0, NULL, fake_device.name, 'h', 0);
    fake_device.driver = &SDL_HIDAPI_DriverSwitch;
    return Fake_InitDevice() && fake_device.num_joysticks == 1;
}

static Uint16 Reported(void)
{
    return SDL_GetJoystickFirmwareVersion(fake_joystick);
}

static void TestBluetooth(void)
{
    bool opened;

    fake_scenario = "Bluetooth";
    if (!StartPad(true)) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    CHECK(device_info_requests == 1, "%d device info requests", device_info_requests);
    CHECK(Ctx()->m_eControllerType == k_eSwitchDeviceInfoControllerType_ProController, "type %d", (int)Ctx()->m_eControllerType);
    CHECK(Ctx()->m_usFirmwareVersion == 0x0348, "the reply kept 0x%04x", Ctx()->m_usFirmwareVersion);
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(Reported() == 0x0348, "the open reported 0x%04x", Reported());
    CHECK(proprietary_commands == 0, "%d proprietary commands over Bluetooth", proprietary_commands);

    /* The second half of a combined pair runs its open on the joystick the
       first half opened (HIDAPI_DriverCombined_OpenJoystick) */
    fake_scenario = "second half of a pair";
    Ctx()->m_usFirmwareVersion = 0x0349;
    SDL_LockJoysticks();
    opened = SDL_HIDAPI_DriverSwitch.OpenJoystick(&fake_device, fake_joystick);
    SDL_UnlockJoysticks();
    CHECK(opened, "the second open failed: %s", SDL_GetError());
    CHECK(Reported() == 0x0349, "a half with a version left 0x%04x", Reported());

    Ctx()->m_usFirmwareVersion = 0;
    SDL_LockJoysticks();
    opened = SDL_HIDAPI_DriverSwitch.OpenJoystick(&fake_device, fake_joystick);
    SDL_UnlockJoysticks();
    CHECK(opened, "the third open failed: %s", SDL_GetError());
    CHECK(Reported() == 0x0349, "a half without a version changed it to 0x%04x", Reported());

    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestUSB(void)
{
    fake_scenario = "USB";
    if (!StartPad(false)) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    CHECK(device_info_requests == 0, "%d device info requests over USB", device_info_requests);
    CHECK(Ctx()->m_eControllerType == k_eSwitchDeviceInfoControllerType_ProController, "type %d", (int)Ctx()->m_eControllerType);
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(Reported() == 0, "the USB open reported 0x%04x", Reported());
    CHECK(subcommands > 0, "the USB open sent no subcommand");
    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    TestBluetooth();
    TestUSB();

    SDL_Quit();
    return Fake_Finish("testswitchfirmware");
}
