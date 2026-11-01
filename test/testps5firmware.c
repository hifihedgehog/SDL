/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_HIDAPI_DriverPS5, the DualSense driver, against a scripted
   DualSense, for hifihedgehog/SDL#43. A DualSense that connects over
   Bluetooth in simple mode skips the firmware information read at init,
   since the read itself switches the controller to enhanced reports. Once
   the driver switches it, the next update reads report 0x20 and the joystick
   reports the update version, bytes 44 and 45, as the init read does.

   The scripted DualSense answers feature report 0x20 with 64 bytes laid out
   as Linux reads them (hid-playstation.c, dualsense_get_firmware_info) and
   duaLib declares them (dataStructures.h, ReportFeatureInVersion). The last
   four bytes hold a CRC-32 of the hidp header 0xA3 and the first 60 bytes,
   as Linux checks it (ps_get_report and ps_check_crc32) and DS4Windows does
   (DualSenseDevice.cs, ReadBTFeatureReport). Its enhanced input report 0x31
   is 78 bytes and ends with a CRC-32 seeded with 0xA1. The test computes both
   with its own CRC-32, checked against the standard check value.

   1. Bluetooth in simple mode: no feature request at init or before the
      switch, version 0 at open, and the version once the application turns
      on a sensor and an update runs. The same for a DualSense Edge.
   2. The same with an enhanced report in auto mode, read in the same update.
   3. The enhanced reports hint set by the application afterward: the read
      waits for the update, which holds the joystick lock.
   4. The hint left at its default switches the controller at open, and the
      first update reads the version.
   5. A firmware older than 2.24 read late leaves enhanced rumble as init set
      it.
   6. A joystick closed before the read: the read still runs, and the next
      open reports the version.
   7. A bad CRC or a reply that names another report is tried again on the
      next update, five tries for each switch into enhanced mode.
   8. USB, and Bluetooth in enhanced mode at connect: the version read at
      init, and no second read of report 0x20. A read that fails at init is
      tried again after the switch on Bluetooth, never on USB.
   9. A controller from another vendor gets no read of report 0x20, as init
      reads it only from Sony controllers.
   10. A controller left in simple mode gets no feature request. */

#define SDL_MAIN_HANDLED
#include "testfirmwarefake.h"

#include "../src/joystick/hidapi/SDL_hidapi_ps5.c"

#define SCRIPTED_VERSION 0x0630

static int requests_by_id[256];
static Uint16 scripted_version;  /* Bytes 44 and 45 of report 0x20 */
static bool firmware_unanswered; /* Report 0x20 goes unanswered */
static int bad_crc_replies;      /* Firmware replies to corrupt, counting down */
static int wrong_id_replies;     /* Firmware replies that name another report */

/* CRC-32 as zlib computes it, bit by bit, so the reports do not depend on
   the CRC code under test. Chained calls continue one CRC. */
static Uint32 Crc32(Uint32 crc, const Uint8 *data, size_t length)
{
    size_t i;
    int bit;

    crc = ~crc;
    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void PutCrc(Uint8 seed, Uint8 *report, size_t length)
{
    const Uint32 crc = Crc32(Crc32(0, &seed, 1), report, length - 4);

    report[length - 4] = (Uint8)(crc & 0xFF);
    report[length - 3] = (Uint8)((crc >> 8) & 0xFF);
    report[length - 2] = (Uint8)((crc >> 16) & 0xFF);
    report[length - 1] = (Uint8)(crc >> 24);
}

static int Feature(unsigned char *data, size_t length)
{
    const Uint8 id = data[0];

    ++requests_by_id[id];
    switch (id) {
    case k_EPS5FeatureReportIdFirmwareInfo:
    {
        Uint8 report[64];

        if (firmware_unanswered || length < sizeof(report)) {
            return -1;
        }
        SDL_zeroa(report);
        report[0] = id;
        if (wrong_id_replies > 0) {
            /* Another report, whole and with a good CRC */
            --wrong_id_replies;
            report[0] = (Uint8)(id + 1);
        }
        SDL_memcpy(&report[1], "Jun 20 2024", 11);
        SDL_memcpy(&report[12], "10:53:46", 8);
        report[44] = (Uint8)(scripted_version & 0xFF);
        report[45] = (Uint8)(scripted_version >> 8);
        PutCrc(0xA3, report, sizeof(report));
        if (bad_crc_replies > 0) {
            --bad_crc_replies;
            report[60] ^= 0xFF;
        }
        SDL_memcpy(data, report, sizeof(report));
        return (int)sizeof(report);
    }
    case k_EPS5FeatureReportIdSerialNumber:
        if (length < 20) {
            return -1;
        }
        SDL_memset(data + 1, 0, 19);
        data[1] = 0x57;
        data[6] = 0x7C;
        return 20;
    default:
        /* No calibration or capabilities: the driver ignores a short read */
        return -1;
    }
}

void Firmware_Respond(const Uint8 *data, int size, bool async)
{
    (void)data;
    (void)size;
    (void)async;
}

static void QueueSimpleReport(void)
{
    Uint8 report[10];

    SDL_zeroa(report);
    report[0] = k_EPS5ReportIdState;
    report[1] = 0x80; /* sticks centered */
    report[2] = 0x80;
    report[3] = 0x80;
    report[4] = 0x80;
    report[5] = 0x08; /* hat centered */
    Fake_Queue(report, sizeof(report));
}

static void QueueEnhancedReport(void)
{
    Uint8 report[78];

    SDL_zeroa(report);
    report[0] = k_EPS5ReportIdBluetoothState;
    report[1] = 0x01; /* controller state, no microphone frame */
    report[2] = 0x80;
    report[3] = 0x80;
    report[4] = 0x80;
    report[5] = 0x80;
    report[9] = 0x08;
    PutCrc(0xA1, report, sizeof(report));
    Fake_Queue(report, sizeof(report));
}

static void QueueUsbReport(void)
{
    Uint8 report[64];

    SDL_zeroa(report);
    report[0] = k_EPS5ReportIdState;
    report[1] = 0x80;
    report[2] = 0x80;
    report[3] = 0x80;
    report[4] = 0x80;
    report[8] = 0x08;
    Fake_Queue(report, sizeof(report));
}

typedef enum
{
    FIRST_REPORT_SIMPLE,
    FIRST_REPORT_ENHANCED,
    FIRST_REPORT_USB
} FirstReport;

/* hint: the enhanced reports hint, NULL for its default. unanswered: report
   0x20 goes unanswered from the start. */
static bool StartPadFrom(FirstReport first, const char *hint, Uint16 vendor_id, Uint16 product_id, bool unanswered)
{
    const bool bluetooth = (first != FIRST_REPORT_USB);

    Fake_Reset();
    SDL_zeroa(requests_by_id);
    scripted_version = SCRIPTED_VERSION;
    firmware_unanswered = unanswered;
    bad_crc_replies = 0;
    wrong_id_replies = 0;
    fake_feature_report = Feature;
    fake_now = 1000;
    if (hint) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, hint);
    } else {
        SDL_ResetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS);
    }
    SDL_zero(fake_device);
    fake_device.dev = Fake_Handle();
    fake_device.name = SDL_strdup("Wireless Controller");
    fake_device.path = SDL_strdup("fake");
    fake_device.serial = SDL_strdup("7cbb8aea3057");
    fake_device.vendor_id = vendor_id;
    fake_device.product_id = product_id;
    fake_device.is_bluetooth = bluetooth;
    fake_device.guid = SDL_CreateJoystickGUID(bluetooth ? SDL_HARDWARE_BUS_BLUETOOTH : SDL_HARDWARE_BUS_USB,
                                              vendor_id, product_id, 0, NULL, fake_device.name, 'h', 0);
    fake_device.driver = &SDL_HIDAPI_DriverPS5;

    switch (first) {
    case FIRST_REPORT_SIMPLE:
        QueueSimpleReport();
        break;
    case FIRST_REPORT_ENHANCED:
        QueueEnhancedReport();
        break;
    case FIRST_REPORT_USB:
        QueueUsbReport();
        break;
    }
    return Fake_InitDevice() && fake_device.num_joysticks == 1;
}

static bool StartPad(FirstReport first, const char *hint)
{
    return StartPadFrom(first, hint, USB_VENDOR_SONY, USB_PRODUCT_SONY_DS5, false);
}

static SDL_DriverPS5_Context *Ctx(void)
{
    return (SDL_DriverPS5_Context *)fake_device.context;
}

static Uint16 Reported(void)
{
    return SDL_GetJoystickFirmwareVersion(fake_joystick);
}

static int FirmwareReads(void)
{
    return requests_by_id[k_EPS5FeatureReportIdFirmwareInfo];
}

static bool EnableSensors(void)
{
    bool result;

    SDL_LockJoysticks();
    result = SDL_HIDAPI_DriverPS5.SetJoystickSensorsEnabled(&fake_device, fake_joystick, true);
    SDL_UnlockJoysticks();
    return result;
}

/* Opens the joystick on a pad StartPad started, and turns on its sensors,
   which switches it to enhanced mode */
static bool OpenAndSwitch(void)
{
    if (!Fake_OpenRecord()) {
        CHECK(false, "the open failed: %s", SDL_GetError());
        return false;
    }
    if (!EnableSensors()) {
        CHECK(false, "the sensors did not turn on: %s", SDL_GetError());
        return false;
    }
    return true;
}

static void TestSensors(Uint16 product_id, const char *name)
{
    fake_scenario = name;
    if (!StartPadFrom(FIRST_REPORT_SIMPLE, "auto", USB_VENDOR_SONY, product_id, false)) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    CHECK(!Ctx()->enhanced_reports, "a simple first report counted as enhanced");
    CHECK(fake_feature_requests == 0, "%d feature requests at init", fake_feature_requests);
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(Reported() == 0, "the open reported 0x%04x", Reported());
    QueueSimpleReport();
    Fake_UpdateDevice();
    CHECK(fake_feature_requests == 0, "%d feature requests before the switch", fake_feature_requests);
    CHECK(Reported() == 0, "simple mode reported 0x%04x", Reported());

    CHECK(EnableSensors(), "the sensors did not turn on: %s", SDL_GetError());
    CHECK(Ctx()->enhanced_mode, "the sensors left simple mode on");
    CHECK(FirmwareReads() == 0, "report 0x20 was read before the update");
    Fake_UpdateDevice();
    CHECK(FirmwareReads() == 1, "report 0x20 was read %d times", FirmwareReads());
    CHECK(Reported() == SCRIPTED_VERSION, "the update reported 0x%04x", Reported());

    /* Read once: later updates leave it */
    Fake_UpdateDevice();
    CHECK(FirmwareReads() == 1, "report 0x20 was read again, %d times", FirmwareReads());
    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestAutoReport(void)
{
    fake_scenario = "Bluetooth, simple at connect, enhanced report in auto mode";
    if (!StartPad(FIRST_REPORT_SIMPLE, "auto")) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(Reported() == 0, "the open reported 0x%04x", Reported());
    QueueEnhancedReport();
    Fake_UpdateDevice();
    CHECK(Ctx()->enhanced_mode, "the enhanced report left simple mode on");
    CHECK(fake_feature_requests == 1 && FirmwareReads() == 1,
          "%d feature requests, %d of report 0x20", fake_feature_requests, FirmwareReads());
    CHECK(Reported() == SCRIPTED_VERSION, "the update reported 0x%04x", Reported());
    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestHintLater(void)
{
    fake_scenario = "Bluetooth, enhanced reports hint set after the open";
    if (!StartPad(FIRST_REPORT_SIMPLE, "auto")) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    /* The application's thread, without the joystick lock */
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
    CHECK(Ctx()->enhanced_mode, "the hint left simple mode on");
    CHECK(fake_feature_requests == 0, "the hint callback read %d feature reports", fake_feature_requests);
    CHECK(Reported() == 0, "the hint callback reported 0x%04x", Reported());
    Fake_UpdateDevice();
    CHECK(Reported() == SCRIPTED_VERSION, "the update reported 0x%04x", Reported());
    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestDefaultHint(void)
{
    fake_scenario = "Bluetooth, enhanced reports hint at its default";
    if (!StartPad(FIRST_REPORT_SIMPLE, NULL)) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    CHECK(fake_feature_requests == 0, "%d feature requests at init", fake_feature_requests);
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(Ctx()->enhanced_mode, "the open left simple mode on");
    CHECK(Reported() == 0, "the open reported 0x%04x", Reported());
    Fake_UpdateDevice();
    CHECK(Reported() == SCRIPTED_VERSION, "the first update reported 0x%04x", Reported());
    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestOldFirmware(void)
{
    fake_scenario = "Bluetooth, firmware older than 2.24 read late";
    if (!StartPad(FIRST_REPORT_SIMPLE, "auto")) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    scripted_version = 0x0110;
    /* Init knew no version and took the firmware for 2.24 or newer */
    CHECK(Ctx()->enhanced_rumble, "init left enhanced rumble off");
    if (OpenAndSwitch()) {
        Fake_UpdateDevice();
        CHECK(Reported() == 0x0110, "the update reported 0x%04x", Reported());
        CHECK(Ctx()->enhanced_rumble, "the late read turned enhanced rumble off");
    }
    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestClosedBeforeRead(void)
{
    fake_scenario = "Bluetooth, joystick closed before the read";
    if (!StartPad(FIRST_REPORT_SIMPLE, "auto")) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    if (OpenAndSwitch()) {
        Fake_CloseRecord();
        Fake_UpdateDevice();
        CHECK(FirmwareReads() == 1, "report 0x20 was read %d times", FirmwareReads());
        CHECK(Ctx()->firmware_version == SCRIPTED_VERSION, "the read kept 0x%04x", Ctx()->firmware_version);
        CHECK(Fake_OpenRecord(), "the second open failed: %s", SDL_GetError());
        CHECK(Reported() == SCRIPTED_VERSION, "the second open reported 0x%04x", Reported());
        CHECK(EnableSensors(), "the sensors did not turn on: %s", SDL_GetError());
        Fake_UpdateDevice();
        CHECK(FirmwareReads() == 1, "the second switch read report 0x20 again, %d times", FirmwareReads());
    }
    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestRetries(void)
{
    int i;

    fake_scenario = "Bluetooth, one bad CRC";
    if (!StartPad(FIRST_REPORT_SIMPLE, "auto")) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    bad_crc_replies = 1;
    if (OpenAndSwitch()) {
        Fake_UpdateDevice();
        CHECK(Reported() == 0, "a bad CRC reported 0x%04x", Reported());
        Fake_UpdateDevice();
        CHECK(FirmwareReads() == 2, "report 0x20 was read %d times", FirmwareReads());
        CHECK(Reported() == SCRIPTED_VERSION, "the second try reported 0x%04x", Reported());
    }
    Fake_FreeDevice();

    fake_scenario = "Bluetooth, a reply that names another report";
    if (!StartPad(FIRST_REPORT_SIMPLE, "auto")) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    wrong_id_replies = 1;
    if (OpenAndSwitch()) {
        Fake_UpdateDevice();
        CHECK(Reported() == 0, "another report's bytes reported 0x%04x", Reported());
        Fake_UpdateDevice();
        CHECK(FirmwareReads() == 2, "report 0x20 was read %d times", FirmwareReads());
        CHECK(Reported() == SCRIPTED_VERSION, "the second try reported 0x%04x", Reported());
    }
    Fake_FreeDevice();

    fake_scenario = "Bluetooth, every CRC bad";
    if (!StartPad(FIRST_REPORT_SIMPLE, "auto")) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    bad_crc_replies = 100;
    if (OpenAndSwitch()) {
        for (i = 0; i < 8; ++i) {
            Fake_UpdateDevice();
        }
        CHECK(FirmwareReads() == 5, "report 0x20 was read %d times", FirmwareReads());
        CHECK(Reported() == 0, "bad CRCs reported 0x%04x", Reported());

        /* The next open's switch tries five times again */
        Fake_CloseRecord();
        bad_crc_replies = 1;
        if (OpenAndSwitch()) {
            Fake_UpdateDevice();
            Fake_UpdateDevice();
            CHECK(FirmwareReads() == 7, "report 0x20 was read %d times in all", FirmwareReads());
            CHECK(Reported() == SCRIPTED_VERSION, "the next open's second try reported 0x%04x", Reported());
        }
    }
    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestReadAtInit(FirstReport first, bool unanswered, const char *name)
{
    const Uint16 expected = (Uint16)(unanswered ? 0 : SCRIPTED_VERSION);

    fake_scenario = name;
    if (!StartPadFrom(first, "auto", USB_VENDOR_SONY, USB_PRODUCT_SONY_DS5, unanswered)) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    CHECK(FirmwareReads() == 1, "init read report 0x20 %d times", FirmwareReads());
    if (OpenAndSwitch()) {
        CHECK(Reported() == expected, "the open reported 0x%04x", Reported());
        Fake_UpdateDevice();
        Fake_UpdateDevice();
        CHECK(FirmwareReads() == 1, "report 0x20 was read again, %d times", FirmwareReads());
        CHECK(Reported() == expected, "the version changed to 0x%04x", Reported());
    }
    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestFailedInitRead(void)
{
    fake_scenario = "Bluetooth, enhanced at connect, report 0x20 unanswered at init";
    if (!StartPadFrom(FIRST_REPORT_ENHANCED, "auto", USB_VENDOR_SONY, USB_PRODUCT_SONY_DS5, true)) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    CHECK(FirmwareReads() == 1, "init read report 0x20 %d times", FirmwareReads());
    CHECK(Ctx()->firmware_version == 0, "init kept 0x%04x", Ctx()->firmware_version);
    firmware_unanswered = false;
    if (OpenAndSwitch()) {
        CHECK(Reported() == 0, "the open reported 0x%04x", Reported());
        Fake_UpdateDevice();
        CHECK(FirmwareReads() == 2, "report 0x20 was read %d times", FirmwareReads());
        CHECK(Reported() == SCRIPTED_VERSION, "the update reported 0x%04x", Reported());
    }
    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestOtherVendor(void)
{
    int i;

    fake_scenario = "Bluetooth, a controller from another vendor";
    if (!StartPadFrom(FIRST_REPORT_SIMPLE, "1", USB_VENDOR_NACON_ALT, USB_PRODUCT_NACON_REVOLUTION_5_PRO_PS5_WIRED, false)) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(Ctx()->enhanced_mode, "the hint left simple mode on");
    for (i = 0; i < 3; ++i) {
        Fake_UpdateDevice();
    }
    CHECK(FirmwareReads() == 0, "report 0x20 was read %d times", FirmwareReads());
    CHECK(Reported() == 0, "the update reported 0x%04x", Reported());
    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

static void TestSimpleForever(void)
{
    int i;

    fake_scenario = "Bluetooth, left in simple mode";
    if (!StartPad(FIRST_REPORT_SIMPLE, "0")) {
        CHECK(false, "the pad did not start: %s", SDL_GetError());
        Fake_FreeDevice();
        return;
    }
    CHECK(Fake_OpenRecord(), "the open failed: %s", SDL_GetError());
    CHECK(!EnableSensors(), "the sensors turned on with enhanced reports off");
    for (i = 0; i < 4; ++i) {
        QueueSimpleReport();
        Fake_UpdateDevice();
    }
    CHECK(!Ctx()->enhanced_mode, "simple mode switched to enhanced");
    CHECK(fake_feature_requests == 0, "%d feature requests in simple mode", fake_feature_requests);
    CHECK(Reported() == 0, "simple mode reported 0x%04x", Reported());
    CHECK(fake_unexpected == 0, "%d unexpected requests", fake_unexpected);
    Fake_FreeDevice();
}

int main(int argc, char *argv[])
{
    static const Uint8 check[] = "123456789";

    (void)argc;
    (void)argv;

    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    fake_scenario = "CRC-32";
    CHECK(Crc32(0, check, 9) == 0xCBF43926u, "the test's CRC-32 of 123456789 is 0x%08x", Crc32(0, check, 9));

    TestSensors(USB_PRODUCT_SONY_DS5, "Bluetooth, simple at connect, sensors");
    TestSensors(USB_PRODUCT_SONY_DS5_EDGE, "Bluetooth DualSense Edge, simple at connect, sensors");
    TestAutoReport();
    TestHintLater();
    TestDefaultHint();
    TestOldFirmware();
    TestClosedBeforeRead();
    TestRetries();
    TestReadAtInit(FIRST_REPORT_USB, false, "USB");
    TestReadAtInit(FIRST_REPORT_USB, true, "USB, report 0x20 unanswered at init");
    TestReadAtInit(FIRST_REPORT_ENHANCED, false, "Bluetooth, enhanced at connect");
    TestFailedInitRead();
    TestOtherVendor();
    TestSimpleForever();

    SDL_Quit();
    return Fake_Finish("testps5firmware");
}
