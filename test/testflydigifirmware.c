/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_HIDAPI_DriverFlydigi, the Flydigi driver, against scripted pads,
   for hifihedgehog/SDL#42. The joystick reports the firmware version the
   driver reads at init, as the PS5 driver reports its own
   (SDL_hidapi_ps5.c, HIDAPI_DriverPS5_OpenJoystick).

   The layouts are the driver's own (src/joystick/hidapi/SDL_hidapi_flydigi.c):
   a V1 pad answers command 0xEC with a 32-byte report whose byte 15 is 0xEC,
   with the device ID in byte 3 and the version little-endian in bytes 9 and
   10 (HIDAPI_DriverFlydigi_InitControllerV1). A V2 pad answers each command
   with a 32-byte report, a report ID, then 0x5A 0xA5 and the command. The
   reply to GET_INFO carries the version big-endian in bytes 15 and 16 after
   the report ID (HIDAPI_DriverFlydigi_InitControllerV2), and the reply to
   GET_STATUS makes the pad available with byte 9 set
   (HIDAPI_DriverFlydigi_HandleStatusResponse).

   1. A Vader 5 Pro on V2 with firmware 0x7150 opens with 0x7150.
   2. A V1 pad with firmware 0x6409 opens with 0x6409. */

#define SDL_MAIN_HANDLED
#include "testfirmwarefake.h"

#include "../src/joystick/hidapi/SDL_hidapi_flydigi.c"

#define V2_REPLY_REPORT_ID 0x04

static Uint16 scripted_version;
static int info_requests;

/* A V2 reply, the report ID first: after it, data[i] is reply[i + 1] */
static void QueueV2Reply(Uint8 command)
{
    Uint8 reply[32];

    SDL_zeroa(reply);
    reply[0] = V2_REPLY_REPORT_ID;
    reply[1] = FLYDIGI_V2_MAGIC1;
    reply[2] = FLYDIGI_V2_MAGIC2;
    reply[3] = command;
    switch (command) {
    case FLYDIGI_V2_CHECK_ARCHITECTURE_COMMAND:
        reply[1 + 3] = 1; /* the new architecture */
        reply[1 + 4] = 0;
        break;
    case FLYDIGI_V2_GET_INFO_COMMAND:
        ++info_requests;
        reply[1 + 5] = 0x80;  /* device ID */
        reply[1 + 6] = 0;     /* wired */
        reply[1 + 11] = 0x05; /* on battery, level 5 */
        reply[1 + 15] = (Uint8)(scripted_version >> 8);
        reply[1 + 16] = (Uint8)(scripted_version & 0xFF);
        break;
    case FLYDIGI_V2_GET_STATUS_COMMAND:
        reply[1 + 9] = 1; /* third-party apps may take the pad */
        break;
    default:
        return;
    }
    Fake_Queue(reply, sizeof(reply));
}

static void QueueV1Info(void)
{
    Uint8 reply[32];

    SDL_zeroa(reply);
    reply[0] = 0x04;
    reply[3] = 0x13; /* device ID */
    reply[5] = 0x12;  /* serial */
    reply[6] = 0x34;
    reply[7] = 0x56;
    reply[8] = 0x78;
    reply[9] = (Uint8)(scripted_version & 0xFF);
    reply[10] = (Uint8)(scripted_version >> 8);
    reply[13] = 1; /* wired */
    reply[15] = FLYDIGI_V1_GET_INFO_COMMAND;
    ++info_requests;
    Fake_Queue(reply, sizeof(reply));
}

void Firmware_Respond(const Uint8 *data, int size, bool async)
{
    (void)async;
    if (fake_device.vendor_id == USB_VENDOR_FLYDIGI_V1) {
        if (size >= 2 && data[0] == FLYDIGI_V1_CMD_REPORT_ID && data[1] == FLYDIGI_V1_GET_INFO_COMMAND) {
            QueueV1Info();
        }
        return;
    }
    /* The Vader sends commands as unnumbered reports, with byte 0 zeroed
       (HIDAPI_DriverFlydigi_WritePacket) */
    if (size >= 4 && data[1] == FLYDIGI_V2_MAGIC1 && data[2] == FLYDIGI_V2_MAGIC2) {
        QueueV2Reply(data[3]);
    }
}

static bool StartPad(Uint16 vendor, Uint16 product, Uint16 version)
{
    Fake_Reset();
    scripted_version = version;
    info_requests = 0;
    fake_now = 1000;
    SDL_zero(fake_device);
    fake_device.dev = Fake_Handle();
    fake_device.name = SDL_strdup("Flydigi Controller");
    fake_device.path = SDL_strdup("fake");
    fake_device.vendor_id = vendor;
    fake_device.product_id = product;
    fake_device.interface_number = (vendor == USB_VENDOR_FLYDIGI_V1) ? 2 : 1;
    fake_device.guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_USB, vendor, product, 0, NULL, fake_device.name, 'h', 0);
    fake_device.driver = &SDL_HIDAPI_DriverFlydigi;
    return Fake_InitDevice();
}

static Uint16 Reported(void)
{
    return SDL_GetJoystickFirmwareVersion(fake_joystick);
}

static void TestV2(void)
{
    fake_scenario = "Vader 5 Pro, V2";
    if (!StartPad(USB_VENDOR_FLYDIGI_V2, USB_PRODUCT_FLYDIGI_V2_VADER, 0x7150)) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    CHECK(info_requests == 1, "%d info requests at init", info_requests);
    /* The status reply arrives with the next update and makes the pad
       available */
    Fake_UpdateDevice();
    CHECK(fake_device.num_joysticks == 1, "%d joysticks", fake_device.num_joysticks);
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(Reported() == 0x7150, "the open reported 0x%04x", Reported());

    /* The heartbeat's info reply after the open leaves the version */
    fake_now += 1000;
    Fake_UpdateDevice();
    Fake_UpdateDevice();
    CHECK(info_requests >= 2, "%d info requests after the open", info_requests);
    CHECK(Reported() == 0x7150, "the heartbeat changed the version to 0x%04x", Reported());

    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestV1(void)
{
    fake_scenario = "V1";
    if (!StartPad(USB_VENDOR_FLYDIGI_V1, USB_PRODUCT_FLYDIGI_V1_GAMEPAD, 0x6409)) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    CHECK(info_requests == 1, "%d info requests at init", info_requests);
    CHECK(fake_device.num_joysticks == 1, "%d joysticks", fake_device.num_joysticks);
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(Reported() == 0x6409, "the open reported 0x%04x", Reported());
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

    TestV2();
    TestV1();

    SDL_Quit();
    return Fake_Finish("testflydigifirmware");
}
