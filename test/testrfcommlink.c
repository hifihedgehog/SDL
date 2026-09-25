/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/windows/SDL_rfcomm_proto.c, the transport of
   the Bluetooth RFCOMM controllers of hifihedgehog/SDL#33 Part 11: which
   paired devices qualify, how the running set changes, and the link that
   connects, retries and hands bytes to the family module. Test numbers
   follow the part's transport tests. */

#include "testrfcommharness.h"

static void TestMatch(void)
{
    static const struct
    {
        const char *name;
        SDL_RFCOMMFamily family;
    } names[] = {
        { "BD&A", SDL_RFCOMM_FAMILY_MOGA },
        { "BD&A Pocket", SDL_RFCOMM_FAMILY_MOGA },
        { "bd&a", SDL_RFCOMM_FAMILY_MOGA },
        { "BDA 1234", SDL_RFCOMM_FAMILY_MOGA },
        { "Bda", SDL_RFCOMM_FAMILY_MOGA },
        { "MOGA 2", SDL_RFCOMM_FAMILY_MOGA },
        { "Moga Pro", SDL_RFCOMM_FAMILY_MOGA },
        { "Moga Pro 2", SDL_RFCOMM_FAMILY_MOGA },
        { "moga", SDL_RFCOMM_FAMILY_MOGA },
        { "MOGA Pro HID", SDL_RFCOMM_FAMILY_NONE },
        { "MOGA 2 HID", SDL_RFCOMM_FAMILY_NONE },
        { "Moga hid", SDL_RFCOMM_FAMILY_NONE },
        { "MOGAHid", SDL_RFCOMM_FAMILY_NONE },
        { "A MOGA", SDL_RFCOMM_FAMILY_NONE },
        { "MOG", SDL_RFCOMM_FAMILY_NONE },
        { "BD", SDL_RFCOMM_FAMILY_NONE },
        { "Zeemote JS1", SDL_RFCOMM_FAMILY_ZEEMOTE },
        { "Zeemote JS1 H", SDL_RFCOMM_FAMILY_ZEEMOTE },
        { "zeemote js1", SDL_RFCOMM_FAMILY_NONE },
        { "Zeemote JS", SDL_RFCOMM_FAMILY_NONE },
        { "Zeemote: SteelSeries FREE", SDL_RFCOMM_FAMILY_NONE },
        { "Zeemote: SteelSeries Free", SDL_RFCOMM_FAMILY_NONE },
        { "ZEEMOTE: STEELSERIES", SDL_RFCOMM_FAMILY_NONE },
        { "GAMEPAD", SDL_RFCOMM_FAMILY_BGP100 },
        { "GAMEPAD 2", SDL_RFCOMM_FAMILY_BGP100 },
        { "Gamepad", SDL_RFCOMM_FAMILY_NONE },
        { "gamepad", SDL_RFCOMM_FAMILY_NONE },
        { "GAMEPA", SDL_RFCOMM_FAMILY_NONE },
        { "Phonejoy", SDL_RFCOMM_FAMILY_PHONEJOY },
        { "PHONEJOY", SDL_RFCOMM_FAMILY_PHONEJOY },
        { "phonejoy 7", SDL_RFCOMM_FAMILY_PHONEJOY },
        { "Phone", SDL_RFCOMM_FAMILY_NONE },
        { "Xbox Wireless Controller", SDL_RFCOMM_FAMILY_NONE },
        { "", SDL_RFCOMM_FAMILY_NONE },
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        CHECK(SDL_RFCOMM_MatchName(names[i].name) == names[i].family);
    }
    CHECK(SDL_RFCOMM_MatchName(NULL) == SDL_RFCOMM_FAMILY_NONE);
    CHECK(SDL_RFCOMM_StartsWith("abc", "", false) && !SDL_RFCOMM_StartsWith(NULL, "a", true) && !SDL_RFCOMM_StartsWith("a", NULL, true));
}

/* A bounded copy that always ends the string */
static void CopyName(char *dst, size_t size, const char *src)
{
    size_t length = strlen(src);

    if (length > size - 1) {
        length = size - 1;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static SDL_RFCOMMPaired Paired(uint64_t address, const char *name, bool paired)
{
    SDL_RFCOMMPaired p;

    memset(&p, 0, sizeof(p));
    p.address = address;
    CopyName(p.name, sizeof(p.name), name);
    p.paired = paired;
    return p;
}

/* 1, first half: a device with another name is never selected, so never
   connected */
static void TestSelect(void)
{
    bool enabled[SDL_RFCOMM_FAMILY_COUNT] = { false, true, true, true, true };
    SDL_RFCOMMPaired paired[8];
    SDL_RFCOMMDevice devices[SDL_RFCOMM_MAX_DEVICES];
    int count;

    paired[0] = Paired(0x111111111111ull, "MOGA Pro", true);
    paired[1] = Paired(0x222222222222ull, "Xbox Wireless Controller", true);
    paired[2] = Paired(0x333333333333ull, "Zeemote JS1", true);
    paired[3] = Paired(0x444444444444ull, "GAMEPAD", false); /* Only connected */
    paired[4] = Paired(0x555555555555ull, "Phonejoy", true);
    paired[5] = Paired(0x111111111111ull, "MOGA Pro", true); /* Repeated */
    paired[6] = Paired(0x666666666666ull, "Zeemote: SteelSeries FREE", true);
    paired[7] = Paired(0x777777777777ull, "BD&A", true);

    count = SDL_RFCOMM_SelectDevices(paired, 8, enabled, devices, SDL_RFCOMM_MAX_DEVICES);
    CHECK(count == 4);
    CHECK(devices[0].address == 0x111111111111ull && devices[0].family == SDL_RFCOMM_FAMILY_MOGA && strcmp(devices[0].name, "MOGA Pro") == 0);
    CHECK(devices[1].address == 0x333333333333ull && devices[1].family == SDL_RFCOMM_FAMILY_ZEEMOTE);
    CHECK(devices[2].address == 0x555555555555ull && devices[2].family == SDL_RFCOMM_FAMILY_PHONEJOY);
    CHECK(devices[3].address == 0x777777777777ull && devices[3].family == SDL_RFCOMM_FAMILY_MOGA);

    /* A family turned off is left out */
    enabled[SDL_RFCOMM_FAMILY_MOGA] = false;
    count = SDL_RFCOMM_SelectDevices(paired, 8, enabled, devices, SDL_RFCOMM_MAX_DEVICES);
    CHECK(count == 2 && devices[0].family == SDL_RFCOMM_FAMILY_ZEEMOTE && devices[1].family == SDL_RFCOMM_FAMILY_PHONEJOY);
    /* NONE is never enabled, whatever the table says */
    enabled[SDL_RFCOMM_FAMILY_NONE] = true;
    count = SDL_RFCOMM_SelectDevices(paired, 8, enabled, devices, SDL_RFCOMM_MAX_DEVICES);
    CHECK(count == 2);
    /* The limit */
    enabled[SDL_RFCOMM_FAMILY_MOGA] = true;
    count = SDL_RFCOMM_SelectDevices(paired, 8, enabled, devices, 2);
    CHECK(count == 2 && devices[1].family == SDL_RFCOMM_FAMILY_ZEEMOTE);
    CHECK(SDL_RFCOMM_SelectDevices(paired, 0, enabled, devices, 2) == 0);
    CHECK(SDL_RFCOMM_SelectDevices(NULL, 8, enabled, devices, 2) == 0);
    CHECK(SDL_RFCOMM_SelectDevices(paired, 8, NULL, devices, 2) == 0);
    CHECK(SDL_RFCOMM_SelectDevices(paired, 8, enabled, devices, 0) == 0);

    /* A name that fills the field is cut, and still matched */
    {
        SDL_RFCOMMPaired longname;

        memset(&longname, 'x', sizeof(longname));
        memcpy(longname.name, "MOGA", 4);
        longname.address = 1;
        longname.paired = true;
        count = SDL_RFCOMM_SelectDevices(&longname, 1, enabled, devices, 1);
        CHECK(count == 1 && strlen(devices[0].name) == SDL_RFCOMM_NAME_LENGTH - 1);
    }
}

static SDL_RFCOMMDevice Device(uint64_t address, SDL_RFCOMMFamily family, const char *name)
{
    SDL_RFCOMMDevice d;

    memset(&d, 0, sizeof(d));
    d.address = address;
    d.family = family;
    CopyName(d.name, sizeof(d.name), name);
    return d;
}

/* 6, first half: a device that left the list stops */
static void TestDiff(void)
{
    SDL_RFCOMMDevice before[3], after[3];
    SDL_RFCOMMChange old_changes[3], new_changes[3];

    before[0] = Device(1, SDL_RFCOMM_FAMILY_MOGA, "MOGA 2");
    before[1] = Device(2, SDL_RFCOMM_FAMILY_ZEEMOTE, "Zeemote JS1");
    before[2] = Device(3, SDL_RFCOMM_FAMILY_BGP100, "GAMEPAD");
    after[0] = Device(2, SDL_RFCOMM_FAMILY_ZEEMOTE, "Zeemote JS1");
    after[1] = Device(4, SDL_RFCOMM_FAMILY_PHONEJOY, "Phonejoy");
    after[2] = Device(3, SDL_RFCOMM_FAMILY_BGP100, "GAMEPAD 2"); /* Renamed */
    SDL_RFCOMM_DiffDevices(before, 3, after, 3, old_changes, new_changes);
    CHECK(old_changes[0] == SDL_RFCOMM_STOP && old_changes[1] == SDL_RFCOMM_KEEP && old_changes[2] == SDL_RFCOMM_STOP);
    CHECK(new_changes[0] == SDL_RFCOMM_KEEP && new_changes[1] == SDL_RFCOMM_START && new_changes[2] == SDL_RFCOMM_START);

    /* A change of family alone restarts too */
    after[0] = Device(2, SDL_RFCOMM_FAMILY_MOGA, "Zeemote JS1");
    SDL_RFCOMM_DiffDevices(before + 1, 1, after, 1, old_changes, new_changes);
    CHECK(old_changes[0] == SDL_RFCOMM_STOP && new_changes[0] == SDL_RFCOMM_START);
    /* Nothing before, nothing after */
    SDL_RFCOMM_DiffDevices(before, 0, after, 2, old_changes, new_changes);
    CHECK(new_changes[0] == SDL_RFCOMM_START && new_changes[1] == SDL_RFCOMM_START);
    SDL_RFCOMM_DiffDevices(before, 2, after, 0, old_changes, new_changes);
    CHECK(old_changes[0] == SDL_RFCOMM_STOP && old_changes[1] == SDL_RFCOMM_STOP);
}

/* 1, second half: one connect at a time */
static void TestConnectOnce(void)
{
    RHarness *r = R_Create(&SDL_RFCOMMMogaModule, "MOGA 2");

    CHECK(R_IsConnect(R_Next(r), 0, 0) && R_Next(r) == NULL && r->link.phase == SDL_RFCOMM_CONNECTING);
    /* No deadline while the connect is out, however long it takes */
    R_Advance(r, 600000);
    CHECK(R_Next(r) == NULL && r->link.connects == 1);
    /* Bytes before the link is up change nothing */
    R_Receive(r, (const uint8_t *)"\x7A\x0E\x66\x01\x00\x00\x00\x00\x00\x00\x00\x00\x10\x03", 14);
    CHECK(r->nsnapshots == 0);
    /* A result for a connect that is not out is ignored */
    R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    CHECK(r->link.phase == SDL_RFCOMM_UP);
    R_Skip(r);
    R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    CHECK(R_Next(r) == NULL && r->link.phase == SDL_RFCOMM_UP);
    R_Destroy(r);

    /* The channel families go to channel 1 */
    r = R_Create(&SDL_RFCOMMPhonejoyModule, "Phonejoy");
    CHECK(R_IsConnect(R_Next(r), SDL_RFCOMM_CHANNEL, 0) && R_Next(r) == NULL);
    R_Destroy(r);
}

/* The service connect's fallback and the retry schedule */
static void TestFallback(void)
{
    RHarness *r = R_Create(&SDL_RFCOMMMogaModule, "MOGA 2");

    R_Skip(r);
    /* A failure other than a page timeout tries channel 1 at once */
    r->now = 700;
    R_Connected(r, SDL_RFCOMM_CONNECT_FAILED);
    CHECK(R_IsConnect(R_Next(r), SDL_RFCOMM_CHANNEL, 700) && R_Next(r) == NULL);
    /* The channel failing waits 3000 ms, then the service UUID again */
    r->now = 900;
    R_Connected(r, SDL_RFCOMM_CONNECT_FAILED);
    CHECK(R_Next(r) == NULL && r->link.phase == SDL_RFCOMM_WAITING);
    R_Advance(r, 3899);
    CHECK(R_Next(r) == NULL);
    R_Advance(r, 3900);
    CHECK(R_IsConnect(R_Next(r), 0, 3900));
    /* A page timeout means the device is away: no channel attempt */
    r->now = 9000;
    R_Connected(r, SDL_RFCOMM_CONNECT_TIMED_OUT);
    CHECK(R_Next(r) == NULL);
    R_Advance(r, 12000);
    CHECK(R_IsConnect(R_Next(r), 0, 12000));
    /* The channel connect that works brings the start-up */
    R_Connected(r, SDL_RFCOMM_CONNECT_FAILED);
    CHECK(R_IsConnect(R_Next(r), SDL_RFCOMM_CHANNEL, 12000));
    R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    CHECK(R_IsSend(R_Next(r), (const uint8_t *)"\x5A\x05\x43\x01\x1D", 5, 12000));
    CHECK(r->link.connects == 5);
    R_Destroy(r);

    /* A channel family that fails waits, with no second attempt */
    r = R_Create(&SDL_RFCOMMBGP100Module, "GAMEPAD");
    R_Skip(r);
    R_Connected(r, SDL_RFCOMM_CONNECT_FAILED);
    CHECK(R_Next(r) == NULL);
    R_Advance(r, SDL_RFCOMM_RETRY_MS);
    CHECK(R_IsConnect(R_Next(r), SDL_RFCOMM_CHANNEL, SDL_RFCOMM_RETRY_MS));
    R_Destroy(r);
}

/* 2: the quiet link runs the family's rule and nothing else */
static void TestQuiet(void)
{
    RHarness *r = R_Up(&SDL_RFCOMMMogaModule, "MOGA 2");

    R_Advance(r, 2000);
    CHECK(R_IsSend(R_Next(r), (const uint8_t *)"\x5A\x05\x45\x01\x1B", 5, 2000));
    CHECK(R_Next(r) == NULL && r->link.phase == SDL_RFCOMM_UP);
    R_Destroy(r);

    r = R_Up(&SDL_RFCOMMZeemoteModule, "Zeemote JS1");
    R_Advance(r, 2000);
    CHECK(R_Next(r) == NULL && r->link.phase == SDL_RFCOMM_UP);
    R_Destroy(r);

    r = R_Up(&SDL_RFCOMMBGP100Module, "GAMEPAD");
    R_Advance(r, 2000);
    CHECK(R_Next(r) == NULL && r->link.phase == SDL_RFCOMM_UP);
    R_Destroy(r);
}

/* 3: recv returning 0 and then WSAECONNRESET both reach the link as loss */
static void TestLoss(void)
{
    RHarness *r = R_Up(&SDL_RFCOMMBGP100Module, "GAMEPAD");
    int i;

    for (i = 0; i < 2; ++i) {
        const uint64_t start = r->now;

        R_Receive(r, (const uint8_t *)"\xB6\x49\xBA\x45", 4);
        CHECK(R_Present(r) && R_Buttons(r) != 0);
        r->now = start + 250;
        R_Lost(r);
        /* The joystick goes with every control released, and no close is
           asked for, since the socket is closed already */
        CHECK(!R_Present(r) && R_Buttons(r) == 0 && R_Next(r) == NULL);
        R_Advance(r, start + 250 + SDL_RFCOMM_RETRY_MS - 1);
        CHECK(R_Next(r) == NULL);
        R_Advance(r, start + 250 + SDL_RFCOMM_RETRY_MS);
        CHECK(R_IsConnect(R_Next(r), SDL_RFCOMM_CHANNEL, start + 250 + SDL_RFCOMM_RETRY_MS));
        R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    }
    /* A loss during the connect counts the same */
    R_Lost(r);
    R_Advance(r, r->now + SDL_RFCOMM_RETRY_MS);
    CHECK(R_IsConnect(R_Next(r), SDL_RFCOMM_CHANNEL, r->now));
    R_Lost(r);
    CHECK(r->link.phase == SDL_RFCOMM_WAITING && r->link.connect_at == r->now + SDL_RFCOMM_RETRY_MS);
    /* A later loss report changes nothing */
    {
        const uint64_t due = r->link.connect_at;

        r->now += 1000;
        R_Lost(r);
        CHECK(r->link.connect_at == due && R_Next(r) == NULL);
        /* A tick before the retry is due asks for nothing */
        SDL_RFCOMMLink_Tick(&r->link, due - 1);
        R_Pump(r);
        CHECK(R_Next(r) == NULL && r->link.phase == SDL_RFCOMM_WAITING);
        SDL_RFCOMMLink_Tick(&r->link, due);
        r->now = due;
        R_Pump(r);
        CHECK(R_IsConnect(R_Next(r), SDL_RFCOMM_CHANNEL, due));
    }
    R_Destroy(r);
}

/* 4: splits, on each family */
static void TestSplits(void)
{
    static const uint8_t moga[] = {
        0x00, 0x7A, 0x0E, 0x66, 0x01, 0x04, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x10, 0x87,
        0x7A, 0x0C, 0x64, 0x01, 0x00, 0x11, 0x00, 0x80, 0x00, 0x00, 0x10, 0x92
    };
    static const uint8_t zeemote[] = {
        0x07, 0xA1, 0x05, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x08, 0xA1, 0x07, 0x03, 0x01, 0xFE, 0xFE, 0xFE, 0xFE, 0x05, 0xA1, 0x08, 0x00, 0x7F, 0x80
    };
    static const uint8_t bgp100[] = { 0xB6, 0x49, 0xBA, 0x45, 0xBB, 0x44, 0xFA, 0x05, 0xFB, 0x04, 0xF6, 0x09 };

    R_CheckSplits(&SDL_RFCOMMMogaModule, "MOGA 2", moga, sizeof(moga));
    R_CheckSplits(&SDL_RFCOMMZeemoteModule, "Zeemote JS1", zeemote, sizeof(zeemote));
    R_CheckSplits(&SDL_RFCOMMBGP100Module, "GAMEPAD", bgp100, sizeof(bgp100));
}

/* 5: the hand-off to the joystick thread keeps a press and its release */
typedef struct QueueSink
{
    SDL_SerialSnapshotQueue queue;
    uint64_t sequence;
} QueueSink;

static void QueueChanged(void *userdata, int sub, const SDL_SerialSnapshot *snapshot)
{
    QueueSink *q = (QueueSink *)userdata;
    SDL_SerialQueueEntry entry;

    memset(&entry, 0, sizeof(entry));
    entry.sub = sub;
    entry.generation = snapshot->present ? 1 : 0;
    entry.sequence = ++q->sequence;
    entry.controls = snapshot->controls;
    SDL_Serial_PushQueue(&q->queue, &entry);
}

static void TestHandOff(void)
{
    static SDL_BGP100State state;
    QueueSink *q = (QueueSink *)calloc(1, sizeof(*q));
    SDL_RFCOMMLink link;
    SDL_SerialSink sink;
    SDL_SerialQueueEntry entry;
    SDL_RFCOMMAction action;

    SDL_Serial_ClearQueue(&q->queue);
    sink.userdata = q;
    sink.changed = QueueChanged;
    sink.log = NULL;
    SDL_RFCOMMLink_Init(&link, &SDL_RFCOMMBGP100Module, &state, &sink, "GAMEPAD", 0);
    SDL_RFCOMMLink_Tick(&link, 0);
    CHECK(SDL_RFCOMMLink_NextAction(&link, &action) && action.kind == SDL_RFCOMM_ACTION_CONNECT);
    SDL_RFCOMMLink_Connected(&link, SDL_RFCOMM_CONNECT_OK, 0);
    /* A press and its release in one read, between two updates */
    SDL_RFCOMMLink_Received(&link, (const uint8_t *)"\xB6\x49", 2, 10);
    SDL_RFCOMMLink_Received(&link, (const uint8_t *)"\xF6\x09", 2, 12);
    CHECK(SDL_Serial_PopQueue(&q->queue, &entry) && entry.sequence == 1 && SDL_Serial_GetButton(&entry.controls, SDL_BGP100_BUTTON_A));
    CHECK(SDL_Serial_PopQueue(&q->queue, &entry) && entry.sequence == 2 && !SDL_Serial_GetButton(&entry.controls, SDL_BGP100_BUTTON_A));
    CHECK(!SDL_Serial_PopQueue(&q->queue, &entry));
    free(q);
}

/* 6: unpairing while connected stops the link for good */
static void TestStop(void)
{
    RHarness *r = R_Up(&SDL_RFCOMMZeemoteModule, "Zeemote JS1");
    uint64_t deadline;

    R_Receive(r, (const uint8_t *)"\x08\xA1\x07\x00\xFE\xFE\xFE\xFE\xFE", 9);
    CHECK(R_Present(r) && R_Buttons(r) != 0);
    r->now = 1000;
    SDL_RFCOMMLink_Stop(&r->link);
    R_Pump(r);
    CHECK(R_IsClose(R_Next(r), 1000) && R_Next(r) == NULL);
    CHECK(!R_Present(r) && R_Buttons(r) == 0 && r->link.phase == SDL_RFCOMM_STOPPED);
    CHECK(!SDL_RFCOMMLink_GetDeadline(&r->link, &deadline));
    R_Advance(r, 600000);
    CHECK(R_Next(r) == NULL);
    /* Nothing brings it back */
    R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    R_Receive(r, (const uint8_t *)"\x08\xA1\x07\x00\xFE\xFE\xFE\xFE\xFE", 9);
    R_Lost(r);
    CHECK(R_Next(r) == NULL && !R_Present(r) && r->link.phase == SDL_RFCOMM_STOPPED);
    R_Destroy(r);

    /* Stopping a connect in progress closes its socket */
    r = R_Create(&SDL_RFCOMMMogaModule, "MOGA 2");
    R_Skip(r);
    SDL_RFCOMMLink_Stop(&r->link);
    R_Pump(r);
    CHECK(R_IsClose(R_Next(r), 0) && R_Next(r) == NULL);
    R_Destroy(r);

    /* Stopping a waiting link asks for nothing, and queued sends go */
    r = R_Up(&SDL_RFCOMMMogaModule, "MOGA 2");
    R_Lost(r);
    SDL_RFCOMMLink_Stop(&r->link);
    R_Pump(r);
    CHECK(R_Next(r) == NULL && r->link.phase == SDL_RFCOMM_STOPPED);
    R_Destroy(r);
}

/* The module's send queue and the conversions */
static void TestHelpers(void)
{
    static SDL_MOGAState state;
    SDL_SerialSink sink;
    int i;

    memset(&sink, 0, sizeof(sink));
    SDL_RFCOMM_ResetBase(&state.base, &sink);
    CHECK(state.base.battery_mv == -1 && state.base.out_count == 0 && !state.base.end);
    CHECK(!SDL_RFCOMM_Send(&state.base, (const uint8_t *)"123456789", 9));
    CHECK(!SDL_RFCOMM_Send(&state.base, (const uint8_t *)"1", 0) && !SDL_RFCOMM_Send(&state.base, NULL, 1));
    for (i = 0; i < SDL_RFCOMM_MAX_OUT; ++i) {
        CHECK(SDL_RFCOMM_Send(&state.base, (const uint8_t *)"12345678", 8));
    }
    CHECK(!SDL_RFCOMM_Send(&state.base, (const uint8_t *)"1", 1) && state.base.out_count == SDL_RFCOMM_MAX_OUT);
    SDL_RFCOMM_End(&state.base);
    CHECK(state.base.end);

    for (i = 0; i < 256; ++i) {
        const int value = (i < 128) ? i : i - 256;
        const int16_t x = SDL_RFCOMM_AxisFromS8((uint8_t)i);
        const int16_t y = SDL_RFCOMM_AxisFromS8Negated((uint8_t)i);
        const int16_t t = SDL_RFCOMM_TriggerFromU8((uint8_t)i);

        CHECK(x == ((value >= 0) ? value * 32767 / 127 : value * 32768 / 128));
        CHECK(y == ((-value >= 0) ? -value * 32767 / 128 : -value * 32768 / 127));
        CHECK(t == i * 257 - 32768);
    }
    /* Text cut only at a character boundary */
    {
        char text[8];

        SDL_RFCOMM_CopyText(text, sizeof(text), "abc");
        CHECK(strcmp(text, "abc") == 0);
        SDL_RFCOMM_CopyText(text, 4, "abcdef");
        CHECK(strcmp(text, "abc") == 0);
        SDL_RFCOMM_CopyText(text, 4, "ab\xC3\xA9");
        CHECK(strcmp(text, "ab") == 0);
        SDL_RFCOMM_CopyText(text, 3, "a\xE2\x82\xAC");
        CHECK(strcmp(text, "a") == 0);
        SDL_RFCOMM_CopyText(text, 5, "a\xE2\x82\xAC");
        CHECK(strcmp(text, "a\xE2\x82\xAC") == 0);
        SDL_RFCOMM_CopyText(text, 5, "\xF0\x9F\x8E\xAE" "b");
        CHECK(strcmp(text, "\xF0\x9F\x8E\xAE") == 0);
        SDL_RFCOMM_CopyText(text, 4, "\xF0\x9F\x8E\xAE");
        CHECK(text[0] == '\0');
        SDL_RFCOMM_CopyText(text, sizeof(text), NULL);
        CHECK(text[0] == '\0');
        SDL_RFCOMM_CopyText(text, 1, "abc");
        CHECK(text[0] == '\0');
        SDL_RFCOMM_CopyText(NULL, 4, "abc");
    }
    /* A MOGA name cut to the identity keeps whole characters */
    {
        static SDL_MOGAState moga;
        char name[80];
        SDL_SerialSink sink2;

        memset(&sink2, 0, sizeof(sink2));
        memset(name, 'a', sizeof(name));
        memcpy(name, "MOGA ", 5);
        name[62] = '\xC3';
        name[63] = '\xA9';
        name[79] = '\0';
        SDL_RFCOMMMogaModule.Reset(&moga, &sink2, name, -1, 0);
        CHECK(strlen(moga.name) == 62 && moga.name[61] == 'a');
    }
    CHECK(SDL_RFCOMM_AxisFromS8(0x7F) == 32767 && SDL_RFCOMM_AxisFromS8(0x80) == -32768);
    CHECK(SDL_RFCOMM_AxisFromS8Negated(0x7F) == -32768 && SDL_RFCOMM_AxisFromS8Negated(0x80) == 32767);
    CHECK(SDL_RFCOMM_TriggerFromU8(0) == -32768 && SDL_RFCOMM_TriggerFromU8(255) == 32767);
}

int main(void)
{
    TestMatch();
    TestSelect();
    TestDiff();
    TestConnectOnce();
    TestFallback();
    TestQuiet();
    TestLoss();
    TestSplits();
    TestHandOff();
    TestStop();
    TestHelpers();
    return H_Finish();
}
