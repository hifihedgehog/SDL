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

/* The Zeemote JS1 in joystick mode. See SDL_rfcomm_zeemote_proto.h. */

#include "SDL_rfcomm_zeemote_proto.h"

#include <string.h>

static void Zeemote_Identity(const SDL_ZeemoteState *s, SDL_SerialIdentity *identity)
{
    SDL_Serial_SetIdentity(identity, s->name, SDL_SERIAL_TYPE_GAMEPAD, SDL_ZEEMOTE_AXES, SDL_ZEEMOTE_BUTTONS, 0, 0);
    identity->has_mapping = true;
    /* C and D as Select and Start, as zeemouse gives them */
    identity->mapping.a = SDL_Serial_MapButton(SDL_ZEEMOTE_BUTTON_A);
    identity->mapping.b = SDL_Serial_MapButton(SDL_ZEEMOTE_BUTTON_B);
    identity->mapping.back = SDL_Serial_MapButton(SDL_ZEEMOTE_BUTTON_C);
    identity->mapping.start = SDL_Serial_MapButton(SDL_ZEEMOTE_BUTTON_D);
    identity->mapping.leftx = SDL_Serial_MapAxis(SDL_ZEEMOTE_AXIS_X);
    identity->mapping.lefty = SDL_Serial_MapAxis(SDL_ZEEMOTE_AXIS_Y);
}

/* A whole frame. True when it is one of the documented types with its
   documented length. */
static bool Zeemote_Frame(SDL_ZeemoteState *s, const uint8_t *frame, size_t total)
{
    const uint8_t *payload = frame + 3;
    const size_t size = total - 3;
    size_t i;

    switch (frame[2]) {
    case SDL_ZEEMOTE_TYPE_STATUS:
        return true;
    case SDL_ZEEMOTE_TYPE_BUTTONS:
        if (size != 6) {
            return false;
        }
        /* The frame is the whole pressed set. Slots other than 0 to 3 and
           FE name no button. */
        for (i = 0; i < SDL_ZEEMOTE_BUTTONS; ++i) {
            SDL_Serial_SetButton(&s->controls, (int)i, false);
        }
        for (i = 0; i < size; ++i) {
            if (payload[i] < SDL_ZEEMOTE_BUTTONS) {
                SDL_Serial_SetButton(&s->controls, payload[i], true);
            }
        }
        return true;
    case SDL_ZEEMOTE_TYPE_STICK:
        /* The JS1 has stick 0 only */
        if (size != 3 || payload[0] != 0) {
            return false;
        }
        s->controls.axes[SDL_ZEEMOTE_AXIS_X] = SDL_RFCOMM_AxisFromS8(payload[1]);
        s->controls.axes[SDL_ZEEMOTE_AXIS_Y] = SDL_RFCOMM_AxisFromS8(payload[2]);
        return true;
    case SDL_ZEEMOTE_TYPE_BATTERY:
        if (size != 2) {
            return false;
        }
        s->base.battery_mv = (payload[0] << 8) | payload[1];
        return true;
    default:
        return false;
    }
}

static void Zeemote_Drop(SDL_ZeemoteState *s, size_t count)
{
    if (count >= s->length) {
        s->length = 0;
        return;
    }
    memmove(s->frame, s->frame + count, s->length - count);
    s->length -= count;
}

static void Zeemote_Scan(SDL_ZeemoteState *s)
{
    while (s->length >= 2) {
        const size_t total = (size_t)s->frame[0] + 1;

        if (s->frame[1] != SDL_ZEEMOTE_MAGIC || s->frame[0] < 2 || total > SDL_ZEEMOTE_MAX_FRAME) {
            Zeemote_Drop(s, 1);
            continue;
        }
        if (s->length < total) {
            return;
        }
        if (Zeemote_Frame(s, s->frame, total)) {
            if (!SDL_Serial_IsPresent(&s->base.serial, 0)) {
                SDL_SerialIdentity identity;

                Zeemote_Identity(s, &identity);
                SDL_Serial_PresentWith(&s->base.serial, 0, &identity, &s->controls);
            } else {
                SDL_Serial_Commit(&s->base.serial, 0, &s->controls);
            }
        }
        Zeemote_Drop(s, total);
    }
}

static void Zeemote_Reset(void *state, const SDL_SerialSink *sink, const char *name, int player_index, uint64_t now)
{
    SDL_ZeemoteState *s = (SDL_ZeemoteState *)state;

    (void)player_index;
    (void)now;
    SDL_RFCOMM_ResetBase(&s->base, sink);
    memset((uint8_t *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    SDL_RFCOMM_CopyText(s->name, sizeof(s->name), name);
}

static void Zeemote_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    SDL_ZeemoteState *s = (SDL_ZeemoteState *)state;
    size_t i;

    (void)now;
    for (i = 0; i < length; ++i) {
        /* A scan leaves at most 63 bytes */
        s->frame[s->length++] = data[i];
        Zeemote_Scan(s);
    }
}

/* No keep-alive is documented */
static void Zeemote_Tick(void *state, uint64_t now)
{
    (void)state;
    (void)now;
}

static bool Zeemote_GetDeadline(const void *state, uint64_t *deadline)
{
    (void)state;
    (void)deadline;
    return false;
}

const SDL_RFCOMMModule SDL_RFCOMMZeemoteModule = {
    SDL_RFCOMM_FAMILY_ZEEMOTE,
    true,
    /* 8E1F0CF7-508F-4875-B62C-FBB67FD34812 */
    { 0x8E, 0x1F, 0x0C, 0xF7, 0x50, 0x8F, 0x48, 0x75, 0xB6, 0x2C, 0xFB, 0xB6, 0x7F, 0xD3, 0x48, 0x12 },
    sizeof(SDL_ZeemoteState),
    Zeemote_Reset,
    Zeemote_Feed,
    Zeemote_Tick,
    Zeemote_GetDeadline,
    NULL
};
