/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_HIDAPI_DriverGIP, the GIP driver for wired Xbox One and Series
   pads, against a scripted pad, for hifihedgehog/SDL#42. The joystick
   reports the firmware version the hello message and the firmware reply
   carry: the major version in the high byte and the minor in the low one,
   as xone sets its input device's version (bus/protocol.c,
   gip_handle_pkt_announce).

   The scripted pad sends a hello message with its firmware quartet in
   bytes 12 to 19 (src/joystick/hidapi/SDL_hidapi_gip.c,
   GIP_HandleCommandHelloDevice) and answers nothing else. After its
   metadata request and three retries go unanswered, the driver takes
   default metadata and adds the joystick, with the hint
   SDL_JOYSTICK_HIDAPI_GIP_RESET_FOR_METADATA off. The driver asks an Elite
   Series 2 for its firmware version, whose quirk declares the firmware
   message both ways, and the reply carries the quartet in bytes 6 to 13
   (GIP_HandleCommandFirmware).

   1. An Elite Series 2 with firmware 5.17.3202.0 opens with 0x0511, and a
      firmware reply for 5.13.3143.0 after the open changes it to 0x050D and
      the Elite button layout with it.
   2. A reply while the joystick is closed changes nothing until the next
      open, which reports it.
   3. An Xbox Series X pad with firmware 5.9 opens with 0x0509. It declares
      no firmware message, so the driver refuses a firmware reply and the
      version stays.
   4. A hello while the joystick is open, as after a reset of the pad,
      changes the version at once. */

#define SDL_MAIN_HANDLED
#include "testfirmwarefake.h"

#include "../src/joystick/hidapi/SDL_hidapi_gip.c"

/* The messages the driver wrote, by type and flags */
#define MAX_LOGGED_WRITES 64
static Uint8 written_types[MAX_LOGGED_WRITES];
static int num_written;

void Firmware_Respond(const Uint8 *data, int size, bool async)
{
    (void)size;
    (void)async;
    if (num_written < MAX_LOGGED_WRITES) {
        written_types[num_written++] = data[0];
    }
}

static int WritesOfType(Uint8 type)
{
    int i, count = 0;

    for (i = 0; i < num_written; ++i) {
        if (written_types[i] == type) {
            ++count;
        }
    }
    return count;
}

/* A system message from the pad's own attachment, header then payload */
static void QueueSystemMessage(Uint8 type, Uint8 sequence, const Uint8 *payload, int length)
{
    Uint8 packet[64];
    int offset = 0;

    packet[offset++] = type;
    packet[offset++] = GIP_FLAG_SYSTEM;
    packet[offset++] = sequence;
    offset += GIP_EncodeLength((Uint64)length, &packet[offset], (int)sizeof(packet) - offset);
    SDL_memcpy(&packet[offset], payload, (size_t)length);
    Fake_Queue(packet, offset + length);
}

static void Put16(Uint8 *data, Uint16 value)
{
    data[0] = (Uint8)(value & 0xFF);
    data[1] = (Uint8)(value >> 8);
}

/* GIP_CMD_HELLO_DEVICE, 28 bytes */
static void QueueHello(Uint16 vendor, Uint16 product, Uint16 major, Uint16 minor, Uint16 build, Uint16 revision)
{
    Uint8 hello[28];

    SDL_zeroa(hello);
    hello[0] = 0x7E; /* device ID */
    hello[1] = 0xED;
    Put16(&hello[8], vendor);
    Put16(&hello[10], product);
    Put16(&hello[12], major);
    Put16(&hello[14], minor);
    Put16(&hello[16], build);
    Put16(&hello[18], revision);
    hello[20] = 1; /* hardware version */
    hello[22] = 1; /* RF protocol 1.0 */
    hello[24] = 1; /* security protocol 1.0 */
    hello[26] = 1; /* GIP 1.0 */
    QueueSystemMessage(GIP_CMD_HELLO_DEVICE, 1, hello, sizeof(hello));
}

/* GIP_CMD_FIRMWARE, the reply to the query for slot 2 */
static void QueueFirmwareReply(Uint8 sequence, Uint16 major, Uint16 minor, Uint16 build, Uint16 revision)
{
    Uint8 reply[14];

    SDL_zeroa(reply);
    reply[0] = 1;
    reply[1] = 2;
    Put16(&reply[6], major);
    Put16(&reply[8], minor);
    Put16(&reply[10], build);
    Put16(&reply[12], revision);
    QueueSystemMessage(GIP_CMD_FIRMWARE, sequence, reply, sizeof(reply));
}

static GIP_Attachment *Attachment(void)
{
    GIP_Device *ctx = (GIP_Device *)fake_device.context;

    return ctx ? ctx->attachments[0] : NULL;
}

/* A pad on USB, through the hello and three unanswered metadata requests to
   default metadata and its joystick */
static bool StartPad(Uint16 product, Uint16 major, Uint16 minor, Uint16 build)
{
    int i;

    Fake_Reset();
    num_written = 0;
    fake_now = 1000;
    SDL_zero(fake_device);
    fake_device.dev = Fake_Handle();
    fake_device.name = SDL_strdup("Xbox Controller");
    fake_device.path = SDL_strdup("fake");
    fake_device.vendor_id = USB_VENDOR_MICROSOFT;
    fake_device.product_id = product;
    fake_device.guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_USB, USB_VENDOR_MICROSOFT, product, 0, NULL, fake_device.name, 'h', 0);
    fake_device.driver = &SDL_HIDAPI_DriverGIP;

    if (!Fake_InitDevice()) {
        return false;
    }
    QueueHello(USB_VENDOR_MICROSOFT, product, major, minor, build, 0);
    Fake_UpdateDevice();
    CHECK(Attachment() && Attachment()->firmware_major_version == major && Attachment()->firmware_minor_version == minor,
          "the hello stored %d.%d", Attachment() ? Attachment()->firmware_major_version : -1, Attachment() ? Attachment()->firmware_minor_version : -1);
    CHECK(WritesOfType(GIP_CMD_METADATA) == 1, "the hello sent %d metadata requests", WritesOfType(GIP_CMD_METADATA));

    for (i = 0; i < 4 && fake_device.num_joysticks == 0; ++i) {
        fake_now += 500;
        Fake_UpdateDevice();
    }
    CHECK(WritesOfType(GIP_CMD_METADATA) == 4, "%d metadata requests went", WritesOfType(GIP_CMD_METADATA));
    return fake_device.num_joysticks == 1;
}

static Uint16 Reported(void)
{
    return SDL_GetJoystickFirmwareVersion(fake_joystick);
}

static void TestEliteSeries2(void)
{
    fake_scenario = "Elite Series 2";
    if (!StartPad(USB_PRODUCT_XBOX_ONE_ELITE_SERIES_2, 5, 17, 3202)) {
        CHECK(false, "no joystick");
        Fake_FreeDevice();
        return;
    }
    CHECK(WritesOfType(GIP_CMD_FIRMWARE) == 1, "%d firmware queries went", WritesOfType(GIP_CMD_FIRMWARE));

    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(Reported() == 0x0511, "the open reported 0x%04x", Reported());
    CHECK(Attachment()->xbe_format == GIP_BTN_FMT_XBE2_5, "5.17 took layout %d", (int)Attachment()->xbe_format);

    /* A firmware reply while the joystick is open */
    QueueFirmwareReply(2, 5, 13, 3143, 0);
    Fake_UpdateDevice();
    CHECK(Reported() == 0x050D, "the reply left 0x%04x", Reported());
    CHECK(Attachment()->xbe_format == GIP_BTN_FMT_XBE2_RAW, "5.13 took layout %d", (int)Attachment()->xbe_format);

    fake_scenario = "Elite Series 2, reply while closed";
    Fake_CloseRecord();
    QueueFirmwareReply(3, 5, 21, 3312, 0);
    Fake_UpdateDevice();
    CHECK(Attachment()->firmware_minor_version == 21, "the reply stored minor %d", Attachment()->firmware_minor_version);
    CHECK(Fake_OpenRecord(), "the second open failed: %s", SDL_GetError());
    CHECK(Reported() == 0x0515, "the second open reported 0x%04x", Reported());

    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestSeriesX(void)
{
    fake_scenario = "Xbox Series X";
    if (!StartPad(USB_PRODUCT_XBOX_SERIES_X, 5, 9, 2641)) {
        CHECK(false, "no joystick");
        Fake_FreeDevice();
        return;
    }
    CHECK(WritesOfType(GIP_CMD_FIRMWARE) == 0, "%d firmware queries went", WritesOfType(GIP_CMD_FIRMWARE));
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(Reported() == 0x0509, "the open reported 0x%04x", Reported());

    /* The pad declares no firmware message, so the driver refuses one */
    QueueFirmwareReply(2, 5, 23, 3000, 0);
    Fake_UpdateDevice();
    CHECK(Reported() == 0x0509, "a refused reply changed the version to 0x%04x", Reported());

    fake_scenario = "Xbox Series X, hello while open";
    QueueHello(USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX_SERIES_X, 5, 11, 2700, 0);
    Fake_UpdateDevice();
    CHECK(Reported() == 0x050B, "the hello left 0x%04x", Reported());

    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Default metadata after the third unanswered request, rather than a
       reset of the pad */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GIP_RESET_FOR_METADATA, "0");
    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    TestEliteSeries2();
    TestSeriesX();

    SDL_Quit();
    return Fake_Finish("testgipfirmware");
}
