/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_HIDAPI_DriverXboxOne, the Xbox One driver, against a scripted pad
   on USB, for hifihedgehog/SDL#42. The joystick reports the firmware version
   the announce packet carries: the major version in the high byte and the
   minor in the low one, as xone sets its input device's version from the
   same packet (bus/protocol.c, gip_pkt_announce and
   gip_handle_pkt_announce), so firmware 5.5.2641.0 is 0x0505, the example in
   the driver's comment on GIP_CMD_ANNOUNCE.

   The announce payload is 28 bytes: the MAC address, two bytes of padding,
   the vendor and product IDs, then the firmware and hardware versions, four
   16-bit little-endian fields each (xone bus/protocol.c, gip_pkt_announce).

   1. An announce before the open: the open reports 0x0505.
   2. An announce while the joystick is open reports at once.
   3. An announce shorter than the firmware minor version changes nothing.
   4. An announce for a device plugged into the pad changes nothing.
   5. A pad that never announces reports 0. */

#define SDL_MAIN_HANDLED
#include "testfirmwarefake.h"

#include "../src/joystick/hidapi/SDL_hidapi_xboxone.c"

void Firmware_Respond(const Uint8 *data, int size, bool async)
{
    (void)data;
    (void)size;
    (void)async;
}

static void Put16(Uint8 *data, Uint16 value)
{
    data[0] = (Uint8)(value & 0xFF);
    data[1] = (Uint8)(value >> 8);
}

/* GIP_CMD_ANNOUNCE with the given payload length and options */
static void QueueAnnounce(Uint8 options, Uint8 sequence, int length, Uint16 major, Uint16 minor, Uint16 build)
{
    Uint8 packet[64];
    Uint8 *payload = &packet[4];

    SDL_zeroa(packet);
    packet[0] = GIP_CMD_ANNOUNCE;
    packet[1] = options;
    packet[2] = sequence;
    packet[3] = (Uint8)length;
    payload[0] = 0x7C; /* MAC address */
    payload[1] = 0xED;
    Put16(&payload[8], USB_VENDOR_MICROSOFT);
    Put16(&payload[10], USB_PRODUCT_XBOX_SERIES_X);
    Put16(&payload[12], major);
    Put16(&payload[14], minor);
    Put16(&payload[16], build);
    Put16(&payload[20], 1); /* hardware version */
    Fake_Queue(packet, 4 + length);
}

static SDL_DriverXboxOne_Context *Ctx(void)
{
    return (SDL_DriverXboxOne_Context *)fake_device.context;
}

static bool StartPad(void)
{
    Fake_Reset();
    fake_now = 1000;
    SDL_zero(fake_device);
    fake_device.dev = Fake_Handle();
    fake_device.name = SDL_strdup("Xbox Series X Controller");
    fake_device.path = SDL_strdup("fake");
    fake_device.vendor_id = USB_VENDOR_MICROSOFT;
    fake_device.product_id = USB_PRODUCT_XBOX_SERIES_X;
    fake_device.guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_USB, USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX_SERIES_X, 0, NULL, fake_device.name, 'h', 0);
    fake_device.driver = &SDL_HIDAPI_DriverXboxOne;
    return Fake_InitDevice() && fake_device.num_joysticks == 1;
}

static Uint16 Reported(void)
{
    return SDL_GetJoystickFirmwareVersion(fake_joystick);
}

static void TestAnnounce(void)
{
    fake_scenario = "announce before the open";
    if (!StartPad()) {
        CHECK(false, "the pad did not start");
        Fake_FreeDevice();
        return;
    }
    QueueAnnounce(GIP_OPT_INTERNAL, 1, 28, 5, 5, 2641);
    Fake_UpdateDevice();
    CHECK(Ctx()->firmware_version == 0x0505, "the announce kept 0x%04x", Ctx()->firmware_version);
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(Reported() == 0x0505, "the open reported 0x%04x", Reported());

    fake_scenario = "announce while open";
    QueueAnnounce(GIP_OPT_INTERNAL, 2, 28, 5, 17, 3202);
    Fake_UpdateDevice();
    CHECK(Reported() == 0x0511, "the announce left 0x%04x", Reported());

    fake_scenario = "short announce";
    /* Up to the major version only. Its major differs, so a read past the
       end would show. */
    QueueAnnounce(GIP_OPT_INTERNAL, 3, 14, 6, 23, 3500);
    Fake_UpdateDevice();
    CHECK(Reported() == 0x0511, "a short announce changed the version to 0x%04x", Reported());
    CHECK(Ctx()->firmware_version == 0x0511, "a short announce changed the kept version to 0x%04x", Ctx()->firmware_version);

    fake_scenario = "announce from a device plugged into the pad";
    QueueAnnounce(GIP_OPT_INTERNAL | 0x01, 4, 28, 1, 2, 3);
    Fake_UpdateDevice();
    CHECK(Reported() == 0x0511, "an attached device changed the version to 0x%04x", Reported());

    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestNoAnnounce(void)
{
    fake_scenario = "no announce";
    if (!StartPad()) {
        CHECK(false, "the pad did not start");
        Fake_FreeDevice();
        return;
    }
    Fake_UpdateDevice();
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(Reported() == 0, "a pad without an announce reported 0x%04x", Reported());
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

    TestAnnounce();
    TestNoAnnounce();

    SDL_Quit();
    return Fake_Finish("testxboxonefirmware");
}
