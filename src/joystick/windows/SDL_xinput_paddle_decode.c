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

#include "SDL_xinput_paddle_decode.h"

#include <string.h>

void SDL_XInputPaddleReset(SDL_XInputPaddleState *state,
                         SDL_XInputPaddleTransport transport, uint64_t generation)
{
    if (state) {
        memset(state, 0, sizeof(*state));
        state->transport = transport;
        state->generation = generation;
    }
}

static SDL_XInputPaddleComparison SDL_XInputPaddleCompare(const uint8_t *a, const uint8_t *b)
{
    /* Only the digital word is compared. Analog drift and sampling order do
     * not establish whether a paddle is mapped. This result is metadata.
     */
    return (a[0] == b[0] && a[1] == b[1]) ? SDL_XINPUT_PADDLE_COMPARE_EQUAL : SDL_XINPUT_PADDLE_COMPARE_DIFFERENT;
}

SDL_XInputPaddleResult SDL_XInputPaddleDecode(SDL_XInputPaddleState *state,
                                           SDL_XInputPaddleTransport transport,
                                           uint64_t generation, uint32_t report_id,
                                           const uint8_t *data, size_t size)
{
    SDL_XInputPaddleResult result = { 0 };
    size_t paddle_index;
    size_t profile_index;
    int ordinary = 0;

    if (!state || !data || state->transport != transport || state->generation != generation) {
        return result;
    }

    if (transport == SDL_XINPUT_PADDLE_VENDOR_GATT) {
        if (report_id != 0 || size != 17) {
            return result;
        }
        paddle_index = 14;
        profile_index = 15;
        result.sample.delivery = SDL_XINPUT_PADDLE_DELIVERY_GATT;
    } else if (transport == SDL_XINPUT_PADDLE_SERVICE_GIP) {
        if (report_id == 0x0C && size == 17) {
            /* SDL_hidapi_xboxone.c:981-1037, xone gamepad.c:86-90,358-376.
             * Unlike xpad, these offsets exclude the four-byte wire header.
             */
            paddle_index = 14;
            profile_index = 15;
            result.flags = SDL_XINPUT_PADDLE_SEPARATE_FRAME;
            result.sample.delivery = SDL_XINPUT_PADDLE_DELIVERY_SEPARATE;
            if (state->have_normal) {
                result.sample.digital_comparison = SDL_XInputPaddleCompare(data, state->normal);
            }
        } else if (report_id == 0x20) {
            if (!SDL_XInputPaddleNormalSizeValid(size)) {
                return result;
            }
            /* SDL_hidapi_xboxone.c:1119-1180 defines these payload layouts.
             * xpad.c:1256-1288 and xone gamepad.c:430-462 agree on the
             * Series 1 / 4.x / early 5.x fields and physical bit order.
             */
            switch (size) {
            case 14:
                memcpy(state->normal, data, sizeof(state->normal));
                state->have_normal = 1;
                result.flags = SDL_XINPUT_PADDLE_NORMAL_READY;
                result.sample.button_word = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
                return result;
            case 29:
                paddle_index = 28;
                profile_index = 28;
                result.sample.digital_comparison = SDL_XInputPaddleCompare(data, data + 14);
                break;
            case 34:
                paddle_index = 14;
                profile_index = 15;
                break;
            case 46:
                paddle_index = 18;
                profile_index = 19;
                break;
            case 47:
                paddle_index = 14;
                profile_index = 20;
                break;
            default:
                return result;
            }
            ordinary = 1;
            result.flags = SDL_XINPUT_PADDLE_NORMAL_READY;
            result.sample.delivery = SDL_XINPUT_PADDLE_DELIVERY_IN_BAND;
        } else {
            return result;
        }
    } else {
        return result;
    }

    result.sample.button_word = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
    result.sample.raw_mask = data[paddle_index] & 0x0F;
    result.sample.physical_mask = result.sample.raw_mask;
    result.sample.profile = data[profile_index];
    result.sample.profile_kind = SDL_XINPUT_PADDLE_PROFILE_SLOT;
    if (ordinary && size == 29) {
        const uint8_t raw = result.sample.raw_mask;
        result.sample.physical_mask = (uint8_t)(((raw & 0x02) >> 1) |
                                               ((raw & 0x08) >> 2) |
                                               ((raw & 0x01) << 2) |
                                               ((raw & 0x04) << 1));
        result.sample.profile = (data[28] & 0x10) >> 4;
        result.sample.profile_kind = SDL_XINPUT_PADDLE_PROFILE_MODE;
    }
    result.flags |= SDL_XINPUT_PADDLE_RAW_NIBBLE;

    if (ordinary) {
        memcpy(state->normal, data, sizeof(state->normal));
        state->have_normal = 1;
    }
    if (!ordinary || !state->have_separate) {
        /* Raw publication follows xone's policy. No profile suppression or
         * mapped face-button clearing occurs here. SDL's has_unmapped_state
         * rule prevents later ordinary frames from replacing a split sample.
         */
        state->paddles = result.sample;
        state->have_paddles = 1;
        result.flags |= SDL_XINPUT_PADDLE_STATE_READY;
    }
    if (result.flags & SDL_XINPUT_PADDLE_SEPARATE_FRAME) {
        state->have_separate = 1;
    }
    return result;
}
