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

/* Namco's USB light guns. See SDL_hidapi_guncon_proto.h. */

#include "SDL_hidapi_guncon_proto.h"

#include <string.h>

void SDL_GunCon2_ModeRequest(uint8_t out[SDL_GUNCON2_MODE_LENGTH])
{
    memset(out, 0, SDL_GUNCON2_MODE_LENGTH);
    /* The mode, 0x0100 little-endian, in the last two of the 6 data bytes */
    out[6] = 0x01;
}

bool SDL_GunCon2_Open(const SDL_GunConSink *sink)
{
    uint8_t request[SDL_GUNCON2_MODE_LENGTH];

    SDL_GunCon2_ModeRequest(request);
    return sink && sink->write && sink->write(sink->userdata, request, sizeof(request));
}

void SDL_GunCon2_ResetState(SDL_GunCon2State *state)
{
    memset(state, 0, sizeof(*state));
}

static int16_t GunCon_Count(uint16_t raw)
{
    return (int16_t)((raw > 32767) ? 32767 : raw);
}

/* One axis of the D-pad: both directions together cancel */
static uint8_t GunCon_HatAxis(bool negative, bool positive, uint8_t negative_bit, uint8_t positive_bit)
{
    if (negative == positive) {
        return 0;
    }
    return negative ? negative_bit : positive_bit;
}

bool SDL_GunCon2_Decode(const uint8_t *report, size_t length, SDL_GunCon2State *state)
{
    uint16_t pressed;
    uint8_t buttons = 0;

    if (!report || !state || length < SDL_GUNCON2_REPORT_LENGTH) {
        return false;
    }
    /* Active low: a 0 bit is a pressed control */
    pressed = (uint16_t)~(report[0] | (report[1] << 8));

    if (pressed & 0x2000) {
        buttons |= 1u << 0; /* Trigger, byte 1 bit 5 */
    }
    if (pressed & 0x0008) {
        buttons |= 1u << 1; /* A, byte 0 bit 3 */
    }
    if (pressed & 0x0004) {
        buttons |= 1u << 2; /* B, byte 0 bit 2 */
    }
    if (pressed & 0x0002) {
        buttons |= 1u << 3; /* C, byte 0 bit 1 */
    }
    if (pressed & 0x8000) {
        buttons |= 1u << 4; /* Start, byte 1 bit 7 */
    }
    if (pressed & 0x4000) {
        buttons |= 1u << 5; /* Select, byte 1 bit 6 */
    }
    state->buttons = buttons;
    /* Byte 0: bit 4 up, 5 right, 6 down, 7 left */
    state->hat = (uint8_t)(GunCon_HatAxis((pressed & 0x0010) != 0, (pressed & 0x0040) != 0, SDL_GUNCON_HAT_UP, SDL_GUNCON_HAT_DOWN) |
                           GunCon_HatAxis((pressed & 0x0080) != 0, (pressed & 0x0020) != 0, SDL_GUNCON_HAT_LEFT, SDL_GUNCON_HAT_RIGHT));
    state->axes[0] = GunCon_Count((uint16_t)(report[2] | (report[3] << 8)));
    state->axes[1] = GunCon_Count((uint16_t)(report[4] | (report[5] << 8)));
    return true;
}

const uint8_t SDL_GunCon3_Key[SDL_GUNCON3_KEY_LENGTH] = { 0x01, 0x12, 0x6F, 0x32, 0x24, 0x60, 0x17, 0x21 };

uint8_t SDL_GunCon3_Checksum(const uint8_t *encrypted, uint8_t key7)
{
    uint8_t sum = key7;

    sum = (uint8_t)(sum + encrypted[0]);
    sum = (uint8_t)(sum - encrypted[1]);
    sum = (uint8_t)(sum - encrypted[2]);
    sum = (uint8_t)(sum ^ encrypted[3]);
    sum = (uint8_t)(sum + encrypted[4]);
    sum = (uint8_t)(sum + encrypted[5]);
    sum = (uint8_t)(sum ^ encrypted[6]);
    sum = (uint8_t)(sum ^ encrypted[7]);
    sum = (uint8_t)(sum + encrypted[8]);
    sum = (uint8_t)(sum + encrypted[9]);
    sum = (uint8_t)(sum - encrypted[10]);
    sum = (uint8_t)(sum - encrypted[11]);
    sum = (uint8_t)(sum ^ encrypted[12]);
    return sum;
}

bool SDL_GunCon3_ChecksumValid(const uint8_t *report, size_t length, const uint8_t *key)
{
    if (!report || !key || length < SDL_GUNCON3_REPORT_LENGTH) {
        return false;
    }
    return SDL_GunCon3_Checksum(report, key[7]) == report[13];
}

static int16_t GunCon_Stick(uint8_t raw)
{
    return (int16_t)(raw * 257 - 32768);
}

void SDL_GunCon3_DecodePlain(const uint8_t *plain, SDL_GunCon3State *state)
{
    static const struct
    {
        uint8_t byte;
        uint8_t mask;
    } buttons[SDL_GUNCON3_BUTTONS] = {
        { 1, 0x20 }, /* Trigger */
        { 0, 0x04 }, /* A1 */
        { 0, 0x02 }, /* A2 */
        { 2, 0x80 }, /* A3, the A stick press */
        { 1, 0x04 }, /* B1 */
        { 1, 0x02 }, /* B2 */
        { 2, 0x40 }, /* B3, the B stick press */
        { 1, 0x80 }, /* C1 */
        { 0, 0x08 }, /* C2 */
        { 1, 0x08 }, /* Both markers out of view */
        { 1, 0x10 }  /* Only one marker in view */
    };
    int32_t y;
    int i;

    state->buttons = 0;
    for (i = 0; i < SDL_GUNCON3_BUTTONS; ++i) {
        if (plain[buttons[i].byte] & buttons[i].mask) {
            state->buttons = (uint16_t)(state->buttons | (1u << i));
        }
    }
    state->axes[0] = (int16_t)(uint16_t)(plain[3] | (plain[4] << 8));
    /* The gun counts up toward the top of the screen, and SDL down */
    y = -(int32_t)(int16_t)(uint16_t)(plain[5] | (plain[6] << 8));
    state->axes[1] = (int16_t)((y > 32767) ? 32767 : y);
    state->axes[2] = (int16_t)(uint16_t)(plain[7] | (plain[8] << 8));
    state->axes[3] = GunCon_Stick(plain[11]); /* A stick X */
    state->axes[4] = GunCon_Stick(plain[12]); /* A stick Y */
    state->axes[5] = GunCon_Stick(plain[9]);  /* B stick X */
    state->axes[6] = GunCon_Stick(plain[10]); /* B stick Y */
}

bool SDL_GunCon3_Open(SDL_GunCon3Session *session, const SDL_GunConSink *sink)
{
    session->failures = 0;
    return sink && sink->write && sink->write(sink->userdata, SDL_GunCon3_Key, SDL_GUNCON3_KEY_LENGTH);
}

int SDL_GunCon3_HandleReport(SDL_GunCon3Session *session, const uint8_t *report, size_t length,
                             const SDL_GunConSink *sink)
{
    if (!report || length < SDL_GUNCON3_REPORT_LENGTH) {
        return SDL_GUNCON3_REPORT_SHORT;
    }
    if (!SDL_GunCon3_ChecksumValid(report, length, SDL_GunCon3_Key)) {
        if (++session->failures >= SDL_GUNCON3_RETRY_FAILURES) {
            SDL_GunCon3_Open(session, sink);
        }
        return SDL_GUNCON3_REPORT_FAILED;
    }
    session->failures = 0;
    return SDL_GUNCON3_REPORT_VALID;
}
