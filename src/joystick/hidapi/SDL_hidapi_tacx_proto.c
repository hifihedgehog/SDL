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

/* Tacx USB trainer head units. See SDL_hidapi_tacx_proto.h. */

#include "SDL_hidapi_tacx_proto.h"

#include <string.h>

SDL_TacxModel SDL_Tacx_Identify(uint16_t vendor, uint16_t product)
{
    if (vendor != SDL_TACX_VENDOR) {
        return SDL_TACX_NONE;
    }
    switch (product) {
    case SDL_TACX_PRODUCT_T1904:
        return SDL_TACX_T1904;
    case SDL_TACX_PRODUCT_T1932:
        return SDL_TACX_T1932;
    default:
        return SDL_TACX_NONE;
    }
}

const char *SDL_Tacx_Name(SDL_TacxModel model)
{
    switch (model) {
    case SDL_TACX_T1904:
        return "Tacx T1904";
    case SDL_TACX_T1932:
        return "Tacx T1932";
    default:
        return NULL;
    }
}

void SDL_Tacx_EncodeFrame(int16_t target, uint8_t pedal_echo, uint8_t mode, uint8_t weight,
                          uint16_t calibration, uint8_t out[SDL_TACX_FRAME_LENGTH])
{
    const uint16_t load = (uint16_t)target;

    /* The control command, 00010801 little-endian */
    out[0] = 0x01;
    out[1] = 0x08;
    out[2] = 0x01;
    out[3] = 0x00;
    out[4] = (uint8_t)(load & 0xFF);
    out[5] = (uint8_t)(load >> 8);
    out[6] = pedal_echo;
    out[7] = 0x00;
    out[8] = mode;
    out[9] = weight;
    out[10] = (uint8_t)(calibration & 0xFF);
    out[11] = (uint8_t)(calibration >> 8);
}

void SDL_Tacx_VersionRequest(uint8_t out[SDL_TACX_VERSION_LENGTH])
{
    /* The version command, 00000002 little-endian */
    out[0] = 0x02;
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x00;
}

static uint16_t Tacx_U16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint32_t Tacx_U32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* The header of a reply long enough to decode, or 0 */
static uint32_t Tacx_Header(const uint8_t *reply, size_t length)
{
    if (!reply || length < SDL_TACX_REPLY_MINIMUM) {
        return 0;
    }
    return Tacx_U32(&reply[24]);
}

bool SDL_Tacx_DecodeData(const uint8_t *reply, size_t length, SDL_TacxData *data)
{
    int i;

    if (!data || Tacx_Header(reply, length) != SDL_TACX_HEADER_DATA) {
        return false;
    }
    data->unit_serial = Tacx_U16(&reply[0]);
    data->unit_year = reply[8];
    data->heart_rate = reply[12];
    data->buttons = reply[13];
    for (i = 0; i < 4; ++i) {
        data->axes[i] = Tacx_U16(&reply[16 + 2 * i]);
    }
    data->distance = Tacx_U32(&reply[28]);
    data->wheel_speed = Tacx_U16(&reply[32]);
    data->resistance = (int16_t)Tacx_U16(&reply[38]);
    data->target = (int16_t)Tacx_U16(&reply[40]);
    data->events = reply[42];
    data->cadence = reply[44];
    data->mode = reply[46];
    return true;
}

/* The serial's decimal digits as FortiusANT splits them: the first two are
 * the type, the next two the year, and the rest the number. A digit a short
 * serial lacks reads as 0, and serial 0 has one digit, 0. */
static void Tacx_SplitSerial(uint32_t serial, SDL_TacxBrake *brake)
{
    uint8_t digits[10]; /* Least significant first. 4294967295 has 10. */
    int count = 0;
    int i;

    do {
        digits[count++] = (uint8_t)(serial % 10);
        serial /= 10;
    } while (serial != 0);

    brake->type = 0;
    brake->year = 0;
    brake->number = 0;
    for (i = 0; i < count; ++i) {
        const uint8_t digit = digits[count - 1 - i];

        if (i < 2) {
            brake->type = (uint8_t)(brake->type * 10 + digit);
        } else if (i < 4) {
            brake->year = (uint8_t)(brake->year * 10 + digit);
        } else {
            brake->number = brake->number * 10 + digit;
        }
    }
}

bool SDL_Tacx_DecodeBrake(const uint8_t *reply, size_t length, SDL_TacxBrake *brake)
{
    if (!brake || Tacx_Header(reply, length) != SDL_TACX_HEADER_VERSION) {
        return false;
    }
    brake->firmware = Tacx_U32(&reply[28]);
    brake->serial = Tacx_U32(&reply[32]);
    brake->version2 = Tacx_U16(&reply[36]);
    Tacx_SplitSerial(brake->serial, brake);
    brake->magnetic = (brake->serial == 0);
    return true;
}

void SDL_Tacx_GetControls(const SDL_TacxData *data, SDL_TacxControls *controls)
{
    controls->axes[SDL_TACX_AXIS_STEERING] = (int16_t)((int32_t)data->axes[1] - 32768);
    controls->axes[SDL_TACX_AXIS_WHEEL_SPEED] = (int16_t)((data->wheel_speed > 32767) ? 32767 : data->wheel_speed);
    controls->axes[SDL_TACX_AXIS_CADENCE] = (int16_t)(data->cadence * 128);
    controls->axes[SDL_TACX_AXIS_HEART_RATE] = (int16_t)(data->heart_rate * 128);
    controls->axes[SDL_TACX_AXIS_RESISTANCE] = data->resistance;
    /* Enter, Down, Up and Cancel are bits 0 to 3, in button order */
    controls->buttons = (uint8_t)(data->buttons & 0x0F);
}

void SDL_Tacx_Init(SDL_TacxState *state, uint64_t now)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->frame_at = now;
    state->version_at = now;
}

bool SDL_Tacx_NextFrame(SDL_TacxState *state, uint64_t now, uint8_t out[SDL_TACX_FRAME_LENGTH], size_t *length)
{
    if (!state || !out || !length || state->in_flight || now < state->frame_at) {
        return false;
    }
    if (!state->version_known && state->version_requests < SDL_TACX_VERSION_REQUESTS && now >= state->version_at) {
        SDL_Tacx_VersionRequest(out);
        *length = SDL_TACX_VERSION_LENGTH;
        state->in_flight_version = true;
    } else {
        /* Mode 00 with target 0, and no echo, weight or calibration */
        SDL_Tacx_EncodeFrame(0, 0, SDL_TACX_MODE_STOP, 0, 0, out);
        *length = SDL_TACX_FRAME_LENGTH;
        state->in_flight_version = false;
    }
    state->in_flight = true;
    return true;
}

void SDL_Tacx_FrameDone(SDL_TacxState *state, int written, uint64_t now)
{
    if (!state || !state->in_flight) {
        return;
    }
    state->in_flight = false;
    state->frame_at = now + SDL_TACX_FRAME_INTERVAL_NS;
    if (state->in_flight_version && written == SDL_TACX_VERSION_LENGTH) {
        ++state->version_requests;
        state->version_at = now + SDL_TACX_VERSION_RETRY_NS;
    }
    state->in_flight_version = false;
}

static bool Tacx_SameBrake(const SDL_TacxBrake *a, const SDL_TacxBrake *b)
{
    return a->firmware == b->firmware && a->serial == b->serial && a->version2 == b->version2;
}

int SDL_Tacx_HandleReply(SDL_TacxState *state, const uint8_t *reply, size_t length, uint64_t now)
{
    SDL_TacxBrake brake;
    int changed = 0;

    if (!state) {
        return 0;
    }
    if (SDL_Tacx_DecodeData(reply, length, &state->data)) {
        state->data_at = now;
        changed |= SDL_TACX_CHANGED_CONTROLS;
        if (!state->present) {
            state->present = true;
            changed |= SDL_TACX_CHANGED_PRESENT;
            /* The brake answers now, and a round of version requests went
               unanswered, so cadence would stay off without another */
            if (!state->version_known && state->version_requests >= SDL_TACX_VERSION_REQUESTS) {
                state->version_requests = 0;
                state->version_at = now;
            }
        }
    } else if (SDL_Tacx_DecodeBrake(reply, length, &brake)) {
        if (!state->version_known || !Tacx_SameBrake(&brake, &state->brake)) {
            changed |= SDL_TACX_CHANGED_BRAKE;
        }
        state->brake = brake;
        state->version_known = true;
    }
    return changed;
}

int SDL_Tacx_Tick(SDL_TacxState *state, uint64_t now)
{
    if (!state || !state->present || now < state->data_at + SDL_TACX_TIMEOUT_NS) {
        return 0;
    }
    state->present = false;
    /* Whatever stopped the replies may have restarted the brake, which then
       reports cadence only after another version request */
    state->version_known = false;
    state->version_requests = 0;
    state->version_at = now;
    return SDL_TACX_CHANGED_PRESENT;
}
