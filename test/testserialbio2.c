/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Replay tests for src/joystick/serial/SDL_serial_bio2_proto.c, the Konami
   BIO2's BI2A node of hifihedgehog/SDL#33 Part 14. The vectors are the
   ticket's, constructed from bemanitools' bio2drv, iidxio-bio2 and
   sdvxio-bio2. Test numbers follow the ticket. */

#include "testserialharness.h"
#include "../src/joystick/serial/SDL_serial_bio2_proto.h"

/* The ticket's bytes */
static const uint8_t aa[1] = { 0xAA };
static const uint8_t enumerate_request[8] = { 0xAA, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x03 };
static const uint8_t enumerate_reply[8] = { 0xAA, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x04 };
static const uint8_t version_request[7] = { 0xAA, 0x01, 0x00, 0x02, 0x02, 0x00, 0x05 };
static const uint8_t version_reply[51] = {
    0xAA, 0x81, 0x00, 0x02, 0x02, 0x2C, 0x0D, 0x06, 0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x42, 0x49,
    0x32, 0x41, 0x4E, 0x6F, 0x76, 0x20, 0x32, 0x37, 0x20, 0x32, 0x30, 0x31, 0x37, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x31, 0x34, 0x3A, 0x34, 0x38, 0x3A, 0x35, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x18
};
static const uint8_t start_request[7] = { 0xAA, 0x01, 0x00, 0x03, 0x03, 0x00, 0x07 };
static const uint8_t start_reply[8] = { 0xAA, 0x81, 0x00, 0x03, 0x03, 0x01, 0x00, 0x88 };
static const uint8_t iidx_init[8] = { 0xAA, 0x01, 0x01, 0x00, 0x04, 0x01, 0x2D, 0x34 };
static const uint8_t init_reply[8] = { 0xAA, 0x81, 0x01, 0x00, 0x04, 0x01, 0x00, 0x87 };
static const uint8_t sdvx_init[8] = { 0xAA, 0x01, 0x01, 0x00, 0x04, 0x01, 0x3B, 0x42 };
static const uint8_t sdvx_amp[11] = { 0xAA, 0x01, 0x01, 0x28, 0x05, 0x04, 0x00, 0x00, 0x00, 0x00, 0x33 };
static const uint8_t amp_reply[8] = { 0xAA, 0x81, 0x01, 0x28, 0x05, 0x01, 0x00, 0xB0 };
static uint8_t iidx_poll[55];  /* Sequence 5: AA 01 01 52 05 30, 48 bytes of 00, 89 */
static uint8_t iidx_idle[53];  /* AA 81 01 52 05 2E, 46 bytes of 00, 07 */
static uint8_t sdvx_poll[47];  /* Sequence 6 */
static uint8_t sdvx_idle[23];

/* An encoder written apart from the modules', from aciodrv/device.c */
static size_t Encode(uint8_t address, uint16_t command, uint8_t sequence, const uint8_t *payload, size_t length, uint8_t *out)
{
    uint8_t body[300];
    size_t n = 0, o = 0, i;
    uint8_t sum = 0;

    body[n++] = address;
    body[n++] = (uint8_t)(command >> 8);
    body[n++] = (uint8_t)(command & 0xFF);
    body[n++] = sequence;
    body[n++] = (uint8_t)length;
    if (length) {
        memcpy(body + n, payload, length);
    }
    n += length;
    for (i = 0; i < n; ++i) {
        sum = (uint8_t)(sum + body[i]);
    }
    body[n++] = sum;
    out[o++] = 0xAA;
    for (i = 0; i < n; ++i) {
        if (body[i] == 0xAA || body[i] == 0xFF) {
            out[o++] = 0xFF;
            out[o++] = (uint8_t)~body[i];
        } else {
            out[o++] = body[i];
        }
    }
    return o;
}

static SDL_BIO2State *State(Harness *h)
{
    return (SDL_BIO2State *)h->state;
}

static void BuildVectors(void)
{
    static const uint8_t zeros[48] = { 0 };

    CHECK(Encode(0x01, 0x0152, 5, zeros, 48, iidx_poll) == sizeof(iidx_poll) && iidx_poll[5] == 0x30 && iidx_poll[54] == 0x89);
    CHECK(Encode(0x81, 0x0152, 5, zeros, 46, iidx_idle) == sizeof(iidx_idle) && iidx_idle[5] == 0x2E && iidx_idle[52] == 0x07);
    CHECK(Encode(0x01, 0x0113, 6, zeros, 40, sdvx_poll) == sizeof(sdvx_poll) && sdvx_poll[5] == 0x28 && sdvx_poll[46] == 0x43);
    CHECK(Encode(0x81, 0x0113, 6, zeros, 16, sdvx_idle) == sizeof(sdvx_idle) && sdvx_idle[22] == 0xAB);
}

/* A poll reply with the given payload */
static size_t Reply(SDL_BIO2Mode mode, const uint8_t *payload, uint8_t *out)
{
    if (mode == SDL_BIO2_MODE_IIDX) {
        return Encode(0x81, SDL_BIO2_CMD_POLL_IIDX, 5, payload, SDL_BIO2_INPUT_IIDX, out);
    }
    return Encode(0x81, SDL_BIO2_CMD_POLL_SDVX, 6, payload, SDL_BIO2_INPUT_SDVX, out);
}

static void FeedReply(Harness *h, SDL_BIO2Mode mode, const uint8_t *payload)
{
    uint8_t frame[128];

    H_Feed(h, frame, Reply(mode, payload, frame));
}

static const SDL_SerialModule *Module(SDL_BIO2Mode mode)
{
    return (mode == SDL_BIO2_MODE_IIDX) ? &SDL_SerialBIO2IIDXModule : (mode == SDL_BIO2_MODE_SDVX) ? &SDL_SerialBIO2SDVXModule : &SDL_SerialBIO2Module;
}

/* From an open or a restart at time t: the reset, the bus bring-up with one
   BI2A node, and in a mode the init, the amplifier command and the first
   poll's idle reply */
static void BringUpMode(Harness *h, SDL_BIO2Mode mode)
{
    const uint64_t t = h->now;

    H_Advance(h, t + 2650);
    H_Feed(h, aa, 1);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply, sizeof(version_reply));
    H_Feed(h, start_reply, sizeof(start_reply));
    if (mode == SDL_BIO2_MODE_NONE) {
        return;
    }
    H_Feed(h, init_reply, sizeof(init_reply));
    if (mode == SDL_BIO2_MODE_SDVX) {
        H_Feed(h, amp_reply, sizeof(amp_reply));
        H_Feed(h, sdvx_idle, sizeof(sdvx_idle));
    } else {
        H_Feed(h, iidx_idle, sizeof(iidx_idle));
    }
}

static void BringUpIIDX(Harness *h)
{
    BringUpMode(h, SDL_BIO2_MODE_IIDX);
}

static void BringUpSDVX(Harness *h)
{
    BringUpMode(h, SDL_BIO2_MODE_SDVX);
}

static Harness *Present(SDL_BIO2Mode mode)
{
    Harness *h = H_Create(Module(mode));

    H_Start(h);
    BringUpMode(h, mode);
    CHECK(h->presence[0] == 1 && State(h)->step == SDL_BIO2_STEP_POLL);
    H_SkipCalls(h);
    return h;
}

/* The zeros and the break of a restart at time t */
static bool ExpectRestart(Harness *h, uint64_t t)
{
    static const uint8_t zeros[64] = { 0 };
    int i;

    for (i = 0; i < 8; ++i) {
        if (!H_IsWrite(H_NextCall(h), zeros, 64, t)) {
            return false;
        }
    }
    return H_IsWrite(H_NextCall(h), zeros, 13, t) && H_IsEscape(H_NextCall(h), SDL_SERIAL_SETBREAK, t);
}

/* Tests 1 and 2 */
static void TestStartup(void)
{
    Harness *h = H_Create(&SDL_SerialBIO2IIDXModule);
    const SDL_SerialSnapshot *snapshot;
    uint8_t expected[64];
    int i;

    /* The line: 115200 8N1, DTR and RTS on, then the reset */
    H_Start(h);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(ExpectRestart(h, 0));
    H_Advance(h, 1450);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 1450));
    H_Advance(h, 2650);
    CHECK(H_IsWrite(H_NextCall(h), aa, 1, 2650));
    H_Feed(h, aa, 1);
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, sizeof(enumerate_request), 2650));
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    CHECK(H_IsWrite(H_NextCall(h), version_request, sizeof(version_request), 2650));
    H_Feed(h, version_reply, sizeof(version_reply));
    CHECK(H_IsWrite(H_NextCall(h), start_request, sizeof(start_request), 2650));
    CHECK(State(h)->step == SDL_BIO2_STEP_BUS);
    /* 1: the IIDX init */
    H_Feed(h, start_reply, sizeof(start_reply));
    CHECK(H_IsWrite(H_NextCall(h), iidx_init, sizeof(iidx_init), 2650));
    CHECK(State(h)->step == SDL_BIO2_STEP_INIT && State(h)->node == 1 && State(h)->mode == SDL_BIO2_MODE_IIDX);
    /* 2: the poll, sequence 5, and no joystick before its reply */
    H_Feed(h, init_reply, sizeof(init_reply));
    CHECK(H_IsWrite(H_NextCall(h), iidx_poll, sizeof(iidx_poll), 2650));
    CHECK(h->npresence == 0 && State(h)->step == SDL_BIO2_STEP_POLL);
    H_Feed(h, iidx_idle, sizeof(iidx_idle));
    CHECK(h->presence[0] == 1 && h->npresence == 1 && h->npublished == 1);
    CHECK(strcmp(h->identity[0].name, "Konami BIO2 IIDX") == 0 && h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN);
    CHECK(h->identity[0].naxes == 7 && h->identity[0].nbuttons == 21 && h->identity[0].nhats == 0 && h->identity[0].nballs == 0);
    CHECK(!h->identity[0].has_mapping);
    /* Idle: nothing pressed, sliders and turntables at 0 */
    snapshot = H_Snapshot(h, 0);
    for (i = 0; i < 7; ++i) {
        CHECK(snapshot->controls.axes[i] == -32768);
    }
    CHECK(H_NoButtons(h, 0) && H_Last(h)->controls.axes[0] == -32768);
    /* The next poll goes out at once, sequence 6 */
    {
        static const uint8_t zeros[48] = { 0 };
        const size_t n = Encode(0x01, 0x0152, 6, zeros, 48, expected);

        CHECK(H_IsWrite(H_NextCall(h), expected, n, 2650) && H_NextCall(h) == NULL);
    }
    H_Destroy(h);

    /* 1: SOUND VOLTEX, the init with 3B, then the amplifier command */
    h = H_Create(&SDL_SerialBIO2SDVXModule);
    H_Start(h);
    H_Advance(h, 2650);
    H_Feed(h, aa, 1);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply, sizeof(version_reply));
    H_SkipCalls(h);
    H_Feed(h, start_reply, sizeof(start_reply));
    CHECK(H_IsWrite(H_NextCall(h), sdvx_init, sizeof(sdvx_init), 2650));
    H_Feed(h, init_reply, sizeof(init_reply));
    CHECK(H_IsWrite(H_NextCall(h), sdvx_amp, sizeof(sdvx_amp), 2650) && State(h)->step == SDL_BIO2_STEP_AMP);
    H_Feed(h, amp_reply, sizeof(amp_reply));
    CHECK(H_IsWrite(H_NextCall(h), sdvx_poll, sizeof(sdvx_poll), 2650) && h->npresence == 0);
    H_Feed(h, sdvx_idle, sizeof(sdvx_idle));
    CHECK(h->presence[0] == 1 && strcmp(h->identity[0].name, "Konami SOUND VOLTEX") == 0 && h->identity[0].type == SDL_SERIAL_TYPE_UNKNOWN);
    CHECK(h->identity[0].naxes == 2 && h->identity[0].nbuttons == 14 && !h->identity[0].has_mapping);
    CHECK(H_Axis(h, 0, 0) == -32768 && H_Axis(h, 0, 1) == -32768 && H_NoButtons(h, 0));
    H_Destroy(h);

    /* Every truncation of the init and amplifier replies, with stale bytes
       past the count, sends nothing, and every split of them decodes as one
       read does */
    for (i = 0; i < 2; ++i) {
        const uint8_t *reply = (i == 0) ? init_reply : amp_reply;
        const uint8_t *next = (i == 0) ? sdvx_amp : sdvx_poll;
        const size_t next_length = (i == 0) ? sizeof(sdvx_amp) : sizeof(sdvx_poll);
        size_t k;

        for (k = 0; k < sizeof(init_reply); ++k) {
            int split;

            for (split = 0; split < 2; ++split) {
                h = H_Create(&SDL_SerialBIO2SDVXModule);
                H_Start(h);
                H_Advance(h, 2650);
                H_Feed(h, aa, 1);
                H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
                H_Feed(h, version_reply, sizeof(version_reply));
                H_Feed(h, start_reply, sizeof(start_reply));
                if (i == 1) {
                    H_Feed(h, init_reply, sizeof(init_reply));
                }
                H_SkipCalls(h);
                if (split == 0) {
                    H_FeedPadded(h, reply, k);
                    CHECK(H_NextCall(h) == NULL);
                    H_Feed(h, reply, sizeof(init_reply));
                } else if (k > 0) {
                    H_Feed(h, reply, k);
                    H_Feed(h, reply + k, sizeof(init_reply) - k);
                } else {
                    size_t b;

                    for (b = 0; b < sizeof(init_reply); ++b) {
                        H_Feed(h, reply + b, 1);
                    }
                }
                CHECK(H_IsWrite(H_NextCall(h), next, next_length, 2650));
                H_Destroy(h);
            }
        }
    }

    /* The init and amplifier replies may carry any status length */
    {
        static const uint8_t lengths[3] = { 0, 4, 200 };
        uint8_t status[200];

        memset(status, 0x01, sizeof(status));
        for (i = 0; i < 3; ++i) {
            uint8_t frame[256];

            h = H_Create(&SDL_SerialBIO2SDVXModule);
            H_Start(h);
            H_Advance(h, 2650);
            H_Feed(h, aa, 1);
            H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
            H_Feed(h, version_reply, sizeof(version_reply));
            H_Feed(h, start_reply, sizeof(start_reply));
            H_Feed(h, frame, Encode(0x81, 0x0100, 4, status, lengths[i], frame));
            CHECK(State(h)->step == SDL_BIO2_STEP_AMP);
            H_Feed(h, frame, Encode(0x81, 0x0128, 5, status, lengths[i], frame));
            CHECK(State(h)->step == SDL_BIO2_STEP_POLL);
            H_Destroy(h);
        }
    }
}

/* Every IIDX payload bit alone, every slider value and turntable position */
static void TestIIDXDecode(void)
{
    uint8_t payload[SDL_BIO2_INPUT_IIDX];
    SDL_SerialControls controls, idle;
    int byte, bit, value, i;

    memset(payload, 0, sizeof(payload));
    SDL_BIO2_DecodeIIDX(payload, &idle);
    for (i = 0; i < 7; ++i) {
        CHECK(idle.axes[i] == -32768);
    }
    for (i = 7; i < SDL_SERIAL_MAX_AXES; ++i) {
        CHECK(idle.axes[i] == 0);
    }
    for (byte = 0; byte < SDL_BIO2_INPUT_IIDX; ++byte) {
        for (bit = 0; bit < 8; ++bit) {
            int expected = -1;

            memset(payload, 0, sizeof(payload));
            payload[byte] = (uint8_t)(1 << bit);
            SDL_BIO2_DecodeIIDX(payload, &controls);
            if (byte >= 18 && byte <= 30 && (byte % 2) == 0 && bit == 7) {
                expected = SDL_BIO2_IIDX_BUTTON_P1_KEY1 + (byte - 18) / 2;
            } else if (byte >= 32 && byte <= 44 && (byte % 2) == 0 && bit == 7) {
                expected = SDL_BIO2_IIDX_BUTTON_P2_KEY1 + (byte - 32) / 2;
            } else if (byte == 9 && bit >= 4) {
                static const int panel[4] = { SDL_BIO2_IIDX_BUTTON_EFFECT, SDL_BIO2_IIDX_BUTTON_VEFX, SDL_BIO2_IIDX_BUTTON_P2_START, SDL_BIO2_IIDX_BUTTON_P1_START };
                expected = panel[bit - 4];
            } else if (byte == 1 && bit >= 1 && bit <= 3) {
                static const int system[3] = { SDL_BIO2_IIDX_BUTTON_COIN, SDL_BIO2_IIDX_BUTTON_SERVICE, SDL_BIO2_IIDX_BUTTON_TEST };
                expected = system[bit - 1];
            }
            if (expected >= 0) {
                SDL_SerialControls want = idle;

                SDL_Serial_SetButton(&want, expected, true);
                CHECK(SDL_Serial_ControlsEqual(&controls, &want));
            } else if ((byte == 0 || byte == 2 || byte == 4 || byte == 6 || byte == 7) && bit >= 4) {
                /* A slider's high nibble */
                const int axis = SDL_BIO2_IIDX_AXIS_SLIDER1 + ((byte == 7) ? 4 : byte / 2);
                SDL_SerialControls want = idle;

                want.axes[axis] = (int16_t)((1 << (bit - 4)) * 4369 - 32768);
                CHECK(SDL_Serial_ControlsEqual(&controls, &want));
            } else if (byte == 16 || byte == 17) {
                SDL_SerialControls want = idle;

                want.axes[byte - 16] = (int16_t)(((1 << bit) - 128) * 256);
                CHECK(SDL_Serial_ControlsEqual(&controls, &want));
            } else {
                /* Unmapped */
                CHECK(SDL_Serial_ControlsEqual(&controls, &idle));
            }
        }
    }
    /* Every slider value, the low nibble ignored */
    for (i = 0; i < 5; ++i) {
        static const int bytes[5] = { 0, 2, 4, 6, 7 };

        for (value = 0; value < 256; ++value) {
            memset(payload, 0, sizeof(payload));
            payload[bytes[i]] = (uint8_t)value;
            SDL_BIO2_DecodeIIDX(payload, &controls);
            CHECK(controls.axes[SDL_BIO2_IIDX_AXIS_SLIDER1 + i] == (int16_t)((value >> 4) * 4369 - 32768));
        }
    }
    payload[0] = 0xF0;
    SDL_BIO2_DecodeIIDX(payload, &controls);
    CHECK(controls.axes[SDL_BIO2_IIDX_AXIS_SLIDER1] == 32767);
    /* Every turntable position */
    for (value = 0; value < 256; ++value) {
        memset(payload, 0, sizeof(payload));
        payload[16] = (uint8_t)value;
        payload[17] = (uint8_t)(255 - value);
        SDL_BIO2_DecodeIIDX(payload, &controls);
        CHECK(controls.axes[SDL_BIO2_IIDX_AXIS_TT1] == (value - 128) * 256);
        CHECK(controls.axes[SDL_BIO2_IIDX_AXIS_TT2] == (127 - value) * 256);
    }
    /* Everything at once */
    memset(payload, 0xFF, sizeof(payload));
    SDL_BIO2_DecodeIIDX(payload, &controls);
    for (i = 0; i < SDL_BIO2_IIDX_BUTTONS; ++i) {
        CHECK(SDL_Serial_GetButton(&controls, i));
    }
    CHECK(!SDL_Serial_GetButton(&controls, SDL_BIO2_IIDX_BUTTONS));
    CHECK(controls.axes[0] == 32512 && controls.axes[1] == 32512 && controls.axes[2] == 32767 && controls.axes[6] == 32767);
}

static void TestSDVXDecode(void)
{
    uint8_t payload[SDL_BIO2_INPUT_SDVX];
    SDL_SerialControls controls, idle;
    int byte, bit, value;

    memset(payload, 0, sizeof(payload));
    SDL_BIO2_DecodeSDVX(payload, &idle);
    CHECK(idle.axes[0] == -32768 && idle.axes[1] == -32768 && idle.axes[2] == 0);
    for (byte = 0; byte < SDL_BIO2_INPUT_SDVX; ++byte) {
        for (bit = 0; bit < 8; ++bit) {
            SDL_SerialControls want = idle;
            int expected = -1;

            memset(payload, 0, sizeof(payload));
            payload[byte] = (uint8_t)(1 << bit);
            SDL_BIO2_DecodeSDVX(payload, &controls);
            if (byte == 9) {
                static const int buttons[8] = {
                    SDL_BIO2_SDVX_BUTTON_FX_L, SDL_BIO2_SDVX_BUTTON_BT_D, SDL_BIO2_SDVX_BUTTON_BT_C, SDL_BIO2_SDVX_BUTTON_BT_B,
                    SDL_BIO2_SDVX_BUTTON_BT_A, SDL_BIO2_SDVX_BUTTON_START, SDL_BIO2_SDVX_BUTTON_RECORDER, SDL_BIO2_SDVX_BUTTON_HEADPHONE
                };
                expected = buttons[bit];
            } else if (byte == 10 && bit >= 5) {
                static const int buttons[3] = { SDL_BIO2_SDVX_BUTTON_EX2, SDL_BIO2_SDVX_BUTTON_EX1, SDL_BIO2_SDVX_BUTTON_FX_R };
                expected = buttons[bit - 5];
            } else if (byte == 1 && bit >= 1 && bit <= 3) {
                static const int system[3] = { SDL_BIO2_SDVX_BUTTON_COIN, SDL_BIO2_SDVX_BUTTON_SERVICE, SDL_BIO2_SDVX_BUTTON_TEST };
                expected = system[bit - 1];
            }
            if (expected >= 0) {
                SDL_Serial_SetButton(&want, expected, true);
            } else if (byte == 0 || byte == 2) {
                /* Knob bits 9 to 2 */
                want.axes[byte / 2] = (int16_t)((1 << (bit + 2)) * 64 - 32768);
            } else if ((byte == 1 || byte == 3) && bit >= 6) {
                /* Knob bits 1 and 0 */
                want.axes[byte / 2] = (int16_t)((1 << (bit - 6)) * 64 - 32768);
            }
            CHECK(SDL_Serial_ControlsEqual(&controls, &want));
        }
    }
    /* Every knob position, the low six bits ignored */
    for (value = 0; value < 65536; value += 7) {
        memset(payload, 0, sizeof(payload));
        payload[0] = (uint8_t)(value >> 8);
        payload[1] = (uint8_t)value;
        payload[2] = (uint8_t)((65535 - value) >> 8);
        payload[3] = (uint8_t)(65535 - value);
        SDL_BIO2_DecodeSDVX(payload, &controls);
        CHECK(controls.axes[SDL_BIO2_SDVX_AXIS_KNOB_L] == (value >> 6) * 64 - 32768);
        CHECK(controls.axes[SDL_BIO2_SDVX_AXIS_KNOB_R] == ((65535 - value) >> 6) * 64 - 32768);
    }
    /* 4 */
    memset(payload, 0, sizeof(payload));
    payload[0] = 0xFF;
    payload[1] = 0xC2;
    SDL_BIO2_DecodeSDVX(payload, &controls);
    CHECK(controls.axes[0] == 1023 * 64 - 32768 && SDL_Serial_GetButton(&controls, SDL_BIO2_SDVX_BUTTON_COIN));
    payload[0] = 0x80;
    payload[1] = 0x00;
    SDL_BIO2_DecodeSDVX(payload, &controls);
    CHECK(controls.axes[0] == 0 && !SDL_Serial_GetButton(&controls, SDL_BIO2_SDVX_BUTTON_COIN));
    /* Everything at once */
    memset(payload, 0xFF, sizeof(payload));
    SDL_BIO2_DecodeSDVX(payload, &controls);
    for (bit = 0; bit < SDL_BIO2_SDVX_BUTTONS; ++bit) {
        CHECK(SDL_Serial_GetButton(&controls, bit));
    }
    CHECK(!SDL_Serial_GetButton(&controls, SDL_BIO2_SDVX_BUTTONS) && controls.axes[0] == 32704 && controls.axes[1] == 32704);
}

/* Test 3 through the port */
static void TestIIDXReplies(void)
{
    Harness *h = Present(SDL_BIO2_MODE_IIDX);
    uint8_t payload[SDL_BIO2_INPUT_IIDX], frame[128];
    size_t n;
    int i, value;

    /* Byte 18 = 80 is P1 key 1, checksum 87 */
    memset(payload, 0, sizeof(payload));
    payload[18] = 0x80;
    n = Reply(SDL_BIO2_MODE_IIDX, payload, frame);
    CHECK(frame[n - 1] == 0x87);
    H_Feed(h, frame, n);
    CHECK(H_OnlyButton(h, 0, SDL_BIO2_IIDX_BUTTON_P1_KEY1));
    /* Byte 44 = 80 is P2 key 7 */
    memset(payload, 0, sizeof(payload));
    payload[44] = 0x80;
    FeedReply(h, SDL_BIO2_MODE_IIDX, payload);
    CHECK(H_OnlyButton(h, 0, SDL_BIO2_IIDX_BUTTON_P2_KEY1 + 6));
    /* Byte 9 = 80 is P1 START, byte 1 = 08 Test */
    memset(payload, 0, sizeof(payload));
    payload[9] = 0x80;
    FeedReply(h, SDL_BIO2_MODE_IIDX, payload);
    CHECK(H_OnlyButton(h, 0, SDL_BIO2_IIDX_BUTTON_P1_START));
    memset(payload, 0, sizeof(payload));
    payload[1] = 0x08;
    FeedReply(h, SDL_BIO2_MODE_IIDX, payload);
    CHECK(H_OnlyButton(h, 0, SDL_BIO2_IIDX_BUTTON_TEST));
    /* Byte 0 = F0 is slider 1 at 15 */
    memset(payload, 0, sizeof(payload));
    payload[0] = 0xF0;
    FeedReply(h, SDL_BIO2_MODE_IIDX, payload);
    CHECK(H_NoButtons(h, 0) && H_Axis(h, 0, SDL_BIO2_IIDX_AXIS_SLIDER1) == 32767);
    /* Byte 16 = AA arrives as FF 55, and FF as FF 00 */
    memset(payload, 0, sizeof(payload));
    payload[16] = 0xAA;
    n = Reply(SDL_BIO2_MODE_IIDX, payload, frame);
    CHECK(frame[22] == 0xFF && frame[23] == 0x55);
    H_Feed(h, frame, n);
    CHECK(H_Axis(h, 0, SDL_BIO2_IIDX_AXIS_TT1) == (170 - 128) * 256);
    payload[16] = 0xFF;
    n = Reply(SDL_BIO2_MODE_IIDX, payload, frame);
    CHECK(frame[22] == 0xFF && frame[23] == 0x00);
    H_Feed(h, frame, n);
    CHECK(H_Axis(h, 0, SDL_BIO2_IIDX_AXIS_TT1) == 32512);
    /* Every turntable position through the port, both turntables */
    for (value = 0; value < 256; ++value) {
        memset(payload, 0, sizeof(payload));
        payload[16] = (uint8_t)value;
        payload[17] = (uint8_t)(value ^ 0x55);
        FeedReply(h, SDL_BIO2_MODE_IIDX, payload);
        CHECK(H_Axis(h, 0, SDL_BIO2_IIDX_AXIS_TT1) == (value - 128) * 256);
        CHECK(H_Axis(h, 0, SDL_BIO2_IIDX_AXIS_TT2) == ((value ^ 0x55) - 128) * 256);
    }
    /* Every mapped bit alone through the port */
    for (i = 0; i < 7; ++i) {
        memset(payload, 0, sizeof(payload));
        payload[18 + 2 * i] = 0x80;
        FeedReply(h, SDL_BIO2_MODE_IIDX, payload);
        CHECK(H_OnlyButton(h, 0, SDL_BIO2_IIDX_BUTTON_P1_KEY1 + i));
        memset(payload, 0, sizeof(payload));
        payload[32 + 2 * i] = 0x80;
        FeedReply(h, SDL_BIO2_MODE_IIDX, payload);
        CHECK(H_OnlyButton(h, 0, SDL_BIO2_IIDX_BUTTON_P2_KEY1 + i));
    }
    /* Every slider through the port */
    for (i = 0; i < 5; ++i) {
        static const int bytes[5] = { 0, 2, 4, 6, 7 };

        for (value = 0; value < 16; ++value) {
            memset(payload, 0, sizeof(payload));
            payload[bytes[i]] = (uint8_t)(value << 4);
            FeedReply(h, SDL_BIO2_MODE_IIDX, payload);
            CHECK(H_Axis(h, 0, SDL_BIO2_IIDX_AXIS_SLIDER1 + i) == value * 4369 - 32768);
        }
    }
    CHECK(State(h)->failures == 0 && h->presence[0] == 1);
    H_Destroy(h);
}

/* Test 4 through the port */
static void TestSDVXReplies(void)
{
    Harness *h = Present(SDL_BIO2_MODE_SDVX);
    uint8_t payload[SDL_BIO2_INPUT_SDVX];

    memset(payload, 0, sizeof(payload));
    payload[0] = 0xFF;
    payload[1] = 0xC2;
    FeedReply(h, SDL_BIO2_MODE_SDVX, payload);
    CHECK(H_Axis(h, 0, SDL_BIO2_SDVX_AXIS_KNOB_L) == 32704 && H_OnlyButton(h, 0, SDL_BIO2_SDVX_BUTTON_COIN));
    payload[0] = 0x80;
    payload[1] = 0x00;
    FeedReply(h, SDL_BIO2_MODE_SDVX, payload);
    CHECK(H_Axis(h, 0, SDL_BIO2_SDVX_AXIS_KNOB_L) == 0 && H_NoButtons(h, 0));
    memset(payload, 0, sizeof(payload));
    payload[9] = 0x10;
    FeedReply(h, SDL_BIO2_MODE_SDVX, payload);
    CHECK(H_OnlyButton(h, 0, SDL_BIO2_SDVX_BUTTON_BT_A) && H_Axis(h, 0, SDL_BIO2_SDVX_AXIS_KNOB_L) == -32768);
    payload[9] = 0x00;
    payload[10] = 0x80;
    FeedReply(h, SDL_BIO2_MODE_SDVX, payload);
    CHECK(H_OnlyButton(h, 0, SDL_BIO2_SDVX_BUTTON_FX_R));
    /* Knob R, the escaped bytes included */
    memset(payload, 0, sizeof(payload));
    payload[2] = 0xAA;
    payload[3] = 0xFF;
    FeedReply(h, SDL_BIO2_MODE_SDVX, payload);
    CHECK(H_Axis(h, 0, SDL_BIO2_SDVX_AXIS_KNOB_R) == (0xAAFF >> 6) * 64 - 32768 && H_NoButtons(h, 0));
    CHECK(State(h)->failures == 0);
    H_Destroy(h);
}

/* Test 5 */
static void TestBatteryA(void)
{
    uint8_t payload[SDL_BIO2_INPUT_IIDX];
    uint8_t key[64], tt_aa[64], tt_ff[64], sum_ff[64], idle_again[64], bad_sum[64], bad_command[64], short_reply[64], cut_ff[64];
    size_t n_key, n_tt_aa, n_tt_ff, n_sum_ff, n_idle, n_bad_sum, n_bad_command, n_short, n_cut_ff;
    H_Vector vectors[8];
    Harness *h;
    int i, count = 0;

    memset(payload, 0, sizeof(payload));
    payload[18] = 0x80;
    n_key = Reply(SDL_BIO2_MODE_IIDX, payload, key);
    memset(payload, 0, sizeof(payload));
    payload[16] = 0xAA;
    n_tt_aa = Reply(SDL_BIO2_MODE_IIDX, payload, tt_aa);
    payload[16] = 0xFF;
    n_tt_ff = Reply(SDL_BIO2_MODE_IIDX, payload, tt_ff);
    /* 07 + F8 = FF: the checksum travels escaped */
    memset(payload, 0, sizeof(payload));
    payload[2] = 0xF8;
    n_sum_ff = Reply(SDL_BIO2_MODE_IIDX, payload, sum_ff);
    CHECK(sum_ff[n_sum_ff - 2] == 0xFF && sum_ff[n_sum_ff - 1] == 0x00);
    memset(payload, 0, sizeof(payload));
    n_idle = Reply(SDL_BIO2_MODE_IIDX, payload, idle_again);
    payload[18] = 0x80;
    n_bad_sum = Reply(SDL_BIO2_MODE_IIDX, payload, bad_sum);
    bad_sum[n_bad_sum - 1] = 0x88;
    n_bad_command = Encode(0x81, 0x0153, 5, payload, SDL_BIO2_INPUT_IIDX, bad_command);
    n_short = Encode(0x81, 0x0152, 5, payload, SDL_BIO2_INPUT_IIDX - 1, short_reply);
    memset(payload, 0, sizeof(payload));
    payload[16] = 0xFF;
    Reply(SDL_BIO2_MODE_IIDX, payload, cut_ff);
    CHECK(cut_ff[22] == 0xFF);
    n_cut_ff = 23;

    h = Present(SDL_BIO2_MODE_IIDX);
    H_SetVector(&vectors[count++], "3 P1 key 1", key, n_key, 1, 0);
    H_SetVector(&vectors[count++], "3 TT1 AA escaped", tt_aa, n_tt_aa, 1, 0);
    H_SetVector(&vectors[count++], "3 TT1 FF escaped", tt_ff, n_tt_ff, 1, 0);
    H_SetVector(&vectors[count++], "checksum FF escaped", sum_ff, n_sum_ff, 1, 0);
    H_SetVector(&vectors[count++], "2 idle again", idle_again, n_idle, 0, 0);
    for (i = 0; i < count; ++i) {
        H_BatteryA(h, &vectors[i], BringUpIIDX);
    }
    H_BatteryA6(h, "wrong checksum", bad_sum, n_bad_sum, &vectors[0]);
    H_BatteryA6(h, "command 0153", bad_command, n_bad_command, &vectors[0]);
    H_BatteryA6(h, "45 bytes", short_reply, n_short, &vectors[0]);
    H_BatteryA6(h, "cut after FF", cut_ff, n_cut_ff, &vectors[0]);
    H_BatteryA6(h, "cut after FF, TT1 FF", cut_ff, n_cut_ff, &vectors[2]);
    for (i = 1; i < (int)n_key; ++i) {
        H_BatteryA6(h, "every truncation", key, (size_t)i, &vectors[0]);
    }
    H_Destroy(h);

    /* SOUND VOLTEX */
    {
        uint8_t sdvx[SDL_BIO2_INPUT_SDVX], knob[40], button[40];
        size_t n_knob, n_button;

        memset(sdvx, 0, sizeof(sdvx));
        sdvx[0] = 0xFF;
        sdvx[1] = 0xC2;
        n_knob = Reply(SDL_BIO2_MODE_SDVX, sdvx, knob);
        memset(sdvx, 0, sizeof(sdvx));
        sdvx[10] = 0x80;
        n_button = Reply(SDL_BIO2_MODE_SDVX, sdvx, button);
        h = Present(SDL_BIO2_MODE_SDVX);
        H_SetVector(&vectors[0], "4 knob L and Coin", knob, n_knob, 1, 0);
        H_SetVector(&vectors[1], "4 FX-R", button, n_button, 1, 0);
        H_SetVector(&vectors[2], "SDVX idle", sdvx_idle, sizeof(sdvx_idle), 0, 0);
        for (i = 0; i < 3; ++i) {
            H_BatteryA(h, &vectors[i], BringUpSDVX);
        }
        /* An IIDX reply is another command */
        H_BatteryA6(h, "IIDX reply", key, n_key, &vectors[0]);
        H_Destroy(h);
    }
}

/* Test 6: no mode, bring-up runs and no joystick opens */
static void TestNoMode(void)
{
    Harness *h = H_Create(&SDL_SerialBIO2Module);

    H_Start(h);
    H_Advance(h, 2650);
    H_SkipCalls(h);
    H_Feed(h, aa, 1);
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, sizeof(enumerate_request), 2650));
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    CHECK(H_IsWrite(H_NextCall(h), version_request, sizeof(version_request), 2650));
    H_Feed(h, version_reply, sizeof(version_reply));
    CHECK(H_IsWrite(H_NextCall(h), start_request, sizeof(start_request), 2650));
    H_Feed(h, start_reply, sizeof(start_reply));
    CHECK(H_NextCall(h) == NULL && State(h)->step == SDL_BIO2_STEP_IDLE && State(h)->node == 1);
    CHECK(strcmp(h->last_log, "SDL_JOYSTICK_KONAMI_BIO2_MODE names no cabinet, so no joystick opens") == 0);
    /* Nothing more goes out, and nothing opens */
    H_Advance(h, 60000);
    H_Feed(h, iidx_idle, sizeof(iidx_idle));
    CHECK(H_NextCall(h) == NULL && h->npresence == 0 && h->npublished == 0);
    CHECK(strcmp(SDL_SerialBIO2Module.token, "bio2") == 0 && strcmp(SDL_SerialBIO2IIDXModule.token, "bio2iidx") == 0);
    CHECK(strcmp(SDL_SerialBIO2SDVXModule.token, "bio2sdvx") == 0);
    CHECK(SDL_SerialBIO2Module.state_size == sizeof(SDL_BIO2State) && !SDL_SerialBIO2IIDXModule.rumble && SDL_SerialBIO2SDVXModule.effect_max == 0);
    H_Destroy(h);
}

/* Bring-up with a list of node products, up to Ready */
static void BringUpNodes(Harness *h, const char *const *products, int count)
{
    uint8_t payload[SDL_ACIO_VERSION_LENGTH], frame[64];
    uint8_t seq = 1;
    int i;

    H_Advance(h, h->now + 2650);
    H_Feed(h, aa, 1);
    payload[0] = (uint8_t)count;
    H_Feed(h, frame, Encode(0x00, 0x0001, seq++, payload, 1, frame));
    for (i = 0; i < count; ++i) {
        memset(payload, 0, sizeof(payload));
        memcpy(payload + 8, products[i], 4);
        H_Feed(h, frame, Encode((uint8_t)(0x81 + i), 0x0002, seq++, payload, SDL_ACIO_VERSION_LENGTH, frame));
    }
    for (i = 0; i < count; ++i) {
        payload[0] = 0x00;
        H_Feed(h, frame, Encode((uint8_t)(0x81 + i), 0x0003, seq++, payload, 1, frame));
    }
}

static void TestNodes(void)
{
    uint8_t expected[16];
    Harness *h;

    /* No BI2A node: nothing but the bring-up */
    {
        static const char *const products[2] = { "KFCA", "ICCA" };

        h = H_Create(&SDL_SerialBIO2IIDXModule);
        H_Start(h);
        BringUpNodes(h, products, 2);
        CHECK(State(h)->step == SDL_BIO2_STEP_IDLE && State(h)->node == 0);
        CHECK(strcmp(h->last_log, "No BI2A node on the BIO2's bus, so no joystick opens") == 0);
        H_SkipCalls(h);
        H_Advance(h, 60000);
        CHECK(H_NextCall(h) == NULL && h->npresence == 0);
        H_Destroy(h);
    }
    /* Several: the highest takes the init */
    {
        static const char *const products[3] = { "BI2A", "BI2A", "ICCA" };
        const uint8_t init = SDL_BIO2_INIT_IIDX;

        h = H_Create(&SDL_SerialBIO2IIDXModule);
        H_Start(h);
        H_SkipCalls(h);
        BringUpNodes(h, products, 3);
        h->cursor = h->ncalls - 1;
        CHECK(H_IsWrite(H_NextCall(h), expected, Encode(0x02, 0x0100, 8, &init, 1, expected), 2650));
        CHECK(State(h)->node == 2 && State(h)->step == SDL_BIO2_STEP_INIT);
        H_Destroy(h);
    }
    {
        static const char *const products[2] = { "BI2A", "ICCA" };

        h = H_Create(&SDL_SerialBIO2SDVXModule);
        H_Start(h);
        BringUpNodes(h, products, 2);
        CHECK(State(h)->node == 1 && State(h)->step == SDL_BIO2_STEP_INIT);
        H_Destroy(h);
    }
}

/* Failed polls, a failed init and amplifier command, and reconnect */
static void TestFailures(void)
{
    uint8_t payload[SDL_BIO2_INPUT_IIDX + 1], frame[128];
    Harness *h;
    int i;

    /* Two failed polls in a row keep the joystick, and each polls again at
       once with the next sequence */
    h = Present(SDL_BIO2_MODE_IIDX);
    for (i = 1; i <= 2; ++i) {
        static const uint8_t zeros[SDL_BIO2_OUTPUT_IIDX] = { 0 };
        const uint64_t at = 2650 + 200 * (uint64_t)i;

        H_Advance(h, at - 1);
        CHECK(State(h)->failures == i - 1 && H_NextCall(h) == NULL);
        H_Advance(h, at);
        CHECK(State(h)->failures == i && h->presence[0] == 1);
        CHECK(H_IsWrite(H_NextCall(h), frame, Encode(0x01, 0x0152, (uint8_t)(6 + i), zeros, SDL_BIO2_OUTPUT_IIDX, frame), at));
    }
    memset(payload, 0, sizeof(payload));
    payload[18] = 0x80;
    FeedReply(h, SDL_BIO2_MODE_IIDX, payload);
    CHECK(State(h)->failures == 0 && H_OnlyButton(h, 0, 0));
    H_Destroy(h);

    /* Three start the bus over and the joystick goes */
    h = Present(SDL_BIO2_MODE_IIDX);
    H_Advance(h, 2650 + 400);
    H_SkipCalls(h);
    H_Advance(h, 2650 + 600 - 1);
    CHECK(h->presence[0] == 1 && State(h)->failures == 2);
    H_Advance(h, 2650 + 600);
    CHECK(h->presence[0] == 0 && State(h)->step == SDL_BIO2_STEP_BUS && State(h)->failures == 0 && State(h)->node == 0);
    CHECK(ExpectRestart(h, 3250));
    CHECK(strcmp(h->last_log, "The BI2A node failed three polls, so the bus starts over") == 0);
    /* The bus comes back, the sequence from 1, as a new joystick */
    BringUpMode(h, SDL_BIO2_MODE_IIDX);
    CHECK(h->presence[0] == 2 && State(h)->acio.bus.restarts == 1);
    H_Destroy(h);

    /* A reopened port starts its module over: failures count from 0, so a
       first poll that fails does not reset the bus */
    h = Present(SDL_BIO2_MODE_IIDX);
    H_Advance(h, 3050);
    CHECK(State(h)->failures == 2);
    H_LosePort(h);
    H_Advance(h, 4050);
    CHECK(State(h)->failures == 0 && State(h)->step == SDL_BIO2_STEP_BUS && State(h)->node == 0);
    H_Advance(h, 4050 + 2650);
    H_Feed(h, aa, 1);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply, sizeof(version_reply));
    H_Feed(h, start_reply, sizeof(start_reply));
    H_Feed(h, init_reply, sizeof(init_reply));
    CHECK(State(h)->step == SDL_BIO2_STEP_POLL);
    H_Advance(h, 4050 + 2650 + 200);
    CHECK(State(h)->failures == 1 && State(h)->acio.bus.restarts == 0 && State(h)->step == SDL_BIO2_STEP_POLL);
    H_Feed(h, iidx_idle, sizeof(iidx_idle));
    CHECK(h->presence[0] == 2 && State(h)->failures == 0);
    H_Destroy(h);

    /* Replies of another command or length fail a poll too */
    h = Present(SDL_BIO2_MODE_IIDX);
    memset(payload, 0, sizeof(payload));
    H_Feed(h, frame, Encode(0x81, 0x0153, 5, payload, SDL_BIO2_INPUT_IIDX, frame));
    H_Feed(h, frame, Encode(0x81, 0x0152, 5, payload, SDL_BIO2_INPUT_IIDX + 1, frame));
    CHECK(State(h)->failures == 2 && h->presence[0] == 1);
    H_Feed(h, frame, Encode(0x81, 0x0152, 5, payload, SDL_BIO2_INPUT_IIDX - 1, frame));
    CHECK(State(h)->failures == 0 && h->presence[0] == 0 && State(h)->acio.bus.restarts == 1);
    H_Destroy(h);

    /* An init with no reply, or another command, starts the bus over */
    for (i = 0; i < 2; ++i) {
        h = H_Create(&SDL_SerialBIO2IIDXModule);
        H_Start(h);
        H_Advance(h, 2650);
        H_Feed(h, aa, 1);
        H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
        H_Feed(h, version_reply, sizeof(version_reply));
        H_Feed(h, start_reply, sizeof(start_reply));
        CHECK(State(h)->step == SDL_BIO2_STEP_INIT);
        H_SkipCalls(h);
        if (i == 0) {
            H_Advance(h, 2849);
            CHECK(State(h)->step == SDL_BIO2_STEP_INIT);
            H_Advance(h, 2850);
        } else {
            H_Feed(h, amp_reply, sizeof(amp_reply));
        }
        CHECK(State(h)->step == SDL_BIO2_STEP_BUS && h->npresence == 0 && State(h)->acio.bus.restarts == 1);
        CHECK(ExpectRestart(h, h->now));
        CHECK(strcmp(h->last_log, "The BI2A node did not answer its init, so the bus starts over") == 0);
        H_Destroy(h);
    }

    /* An amplifier command with no reply starts the bus over */
    h = H_Create(&SDL_SerialBIO2SDVXModule);
    H_Start(h);
    H_Advance(h, 2650);
    H_Feed(h, aa, 1);
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply, sizeof(version_reply));
    H_Feed(h, start_reply, sizeof(start_reply));
    H_Feed(h, init_reply, sizeof(init_reply));
    CHECK(State(h)->step == SDL_BIO2_STEP_AMP);
    H_SkipCalls(h);
    H_Advance(h, 2850);
    CHECK(State(h)->step == SDL_BIO2_STEP_BUS && State(h)->acio.bus.restarts == 1 && ExpectRestart(h, 2850));
    CHECK(strcmp(h->last_log, "The BI2A node did not answer the amplifier command, so the bus starts over") == 0);
    H_Destroy(h);

    /* Output requests change nothing */
    h = Present(SDL_BIO2_MODE_SDVX);
    {
        SDL_SerialOutput request;

        memset(&request, 0, sizeof(request));
        request.kind = SDL_SERIAL_OUTPUT_RUMBLE;
        request.low_frequency_rumble = 0xFFFF;
        SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
        request.kind = SDL_SERIAL_OUTPUT_EFFECT;
        request.length = 1;
        SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
        CHECK(H_NextCall(h) == NULL);
    }
    H_Destroy(h);
}

/* Battery B: the port goes and comes back as a new joystick, the sequence
   from 1 */
static void TestBatteryB(void)
{
    Harness *h = Present(SDL_BIO2_MODE_IIDX);
    uint8_t payload[SDL_BIO2_INPUT_IIDX];

    memset(payload, 0, sizeof(payload));
    payload[9] = 0x80;
    FeedReply(h, SDL_BIO2_MODE_IIDX, payload);
    CHECK(H_Button(h, 0, SDL_BIO2_IIDX_BUTTON_P1_START));
    H_SkipCalls(h);
    H_Advance(h, 2700);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && H_IsCall(H_NextCall(h), 'C', 2700));
    H_Advance(h, 3700);
    CHECK(H_ExpectOpened(h, 115200, 8, SDL_SERIAL_NOPARITY, 1, 3700) && ExpectRestart(h, 3700));
    H_Advance(h, 3700 + 2650);
    H_SkipCalls(h);
    H_Feed(h, aa, 1);
    CHECK(H_IsWrite(H_NextCall(h), enumerate_request, sizeof(enumerate_request), 6350));
    H_Feed(h, enumerate_reply, sizeof(enumerate_reply));
    H_Feed(h, version_reply, sizeof(version_reply));
    H_Feed(h, start_reply, sizeof(start_reply));
    H_Feed(h, init_reply, sizeof(init_reply));
    h->cursor = h->ncalls - 1;
    CHECK(H_IsWrite(H_NextCall(h), iidx_poll, sizeof(iidx_poll), 6350));
    H_Feed(h, iidx_idle, sizeof(iidx_idle));
    CHECK(h->presence[0] == 2 && H_NoButtons(h, 0));
    H_Destroy(h);
}

int main(void)
{
    BuildVectors();
    TestStartup();
    TestIIDXDecode();
    TestSDVXDecode();
    TestIIDXReplies();
    TestSDVXReplies();
    TestBatteryA();
    TestNoMode();
    TestNodes();
    TestFailures();
    TestBatteryB();
    return H_Finish();
}
