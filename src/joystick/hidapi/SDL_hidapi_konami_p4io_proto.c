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

/* Konami's P4IO. See SDL_hidapi_konami_p4io_proto.h. */

#include "SDL_hidapi_konami_p4io_proto.h"

#include <string.h>

#define P4IO_TRANSFER_BULK      2
#define P4IO_TRANSFER_INTERRUPT 3

/* Test and Service sit on the same bits in the jubeat and DDR sources. Coin
   is the DDR harness's. */
#define P4IO_BIT_COIN    24
#define P4IO_BIT_SERVICE 25
#define P4IO_BIT_TEST    28

/* The word bit of each jubeat panel, panels 1 to 16 */
static const uint8_t P4IO_JubeatPanelBits[SDL_KONAMI_P4IO_JUBEAT_PANELS] = {
    5, 1, 13, 9,
    6, 2, 14, 10,
    7, 3, 15, 11,
    16, 4, 20, 12
};

static char P4IO_Lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool P4IO_SameName(const char *hint, const char *name)
{
    for (;;) {
        const char a = P4IO_Lower(*hint++);
        const char b = *name++;

        if (a != b) {
            return false;
        }
        if (!a) {
            return true;
        }
    }
}

int SDL_KonamiP4IO_ParseLayout(const char *hint)
{
    if (hint) {
        if (P4IO_SameName(hint, "jubeat")) {
            return SDL_KONAMI_P4IO_LAYOUT_JUBEAT;
        }
        if (P4IO_SameName(hint, "ddr")) {
            return SDL_KONAMI_P4IO_LAYOUT_DDR;
        }
    }
    return SDL_KONAMI_P4IO_LAYOUT_RAW;
}

bool SDL_KonamiP4IO_GetIdentity(int layout, SDL_KonamiP4IOIdentity *out)
{
    switch (layout) {
    case SDL_KONAMI_P4IO_LAYOUT_RAW:
        out->name = "Konami P4IO";
        out->nbuttons = SDL_KONAMI_P4IO_RAW_BUTTONS;
        return true;
    case SDL_KONAMI_P4IO_LAYOUT_JUBEAT:
        out->name = "Konami jubeat";
        out->nbuttons = SDL_KONAMI_P4IO_JUBEAT_BUTTONS;
        return true;
    case SDL_KONAMI_P4IO_LAYOUT_DDR:
        out->name = "Konami P4IO DDR";
        out->nbuttons = SDL_KONAMI_P4IO_DDR_BUTTONS;
        return true;
    default:
        return false;
    }
}

void SDL_KonamiP4IO_ResetState(SDL_KonamiP4IOState *state)
{
    memset(state, 0, sizeof(*state));
}

static bool P4IO_Bit(uint32_t word, int bit)
{
    return (word & ((uint32_t)1 << bit)) != 0;
}

bool SDL_KonamiP4IO_Decode(int layout, const uint8_t *report, size_t length, SDL_KonamiP4IOState *state)
{
    uint32_t word, buttons = 0;
    int i;

    if (!report || !state || length < SDL_KONAMI_P4IO_REPORT_LENGTH) {
        return false;
    }
    word = (uint32_t)report[0] | ((uint32_t)report[1] << 8) | ((uint32_t)report[2] << 16) | ((uint32_t)report[3] << 24);

    switch (layout) {
    case SDL_KONAMI_P4IO_LAYOUT_RAW:
        buttons = word;
        break;
    case SDL_KONAMI_P4IO_LAYOUT_JUBEAT:
        /* The panels are active low, Test and Service active high */
        for (i = 0; i < SDL_KONAMI_P4IO_JUBEAT_PANELS; ++i) {
            if (!P4IO_Bit(word, P4IO_JubeatPanelBits[i])) {
                buttons |= (uint32_t)1 << i;
            }
        }
        if (P4IO_Bit(word, P4IO_BIT_TEST)) {
            buttons |= (uint32_t)1 << SDL_KONAMI_P4IO_JUBEAT_TEST;
        }
        if (P4IO_Bit(word, P4IO_BIT_SERVICE)) {
            buttons |= (uint32_t)1 << SDL_KONAMI_P4IO_JUBEAT_SERVICE;
        }
        break;
    case SDL_KONAMI_P4IO_LAYOUT_DDR:
        /* Active high. Bits 0 to 4 of byte 0 are player 1's, of byte 1
           player 2's. */
        buttons = word & 0x1F;
        buttons |= ((word >> 8) & 0x1F) << SDL_KONAMI_P4IO_DDR_P2;
        if (P4IO_Bit(word, P4IO_BIT_COIN)) {
            buttons |= (uint32_t)1 << SDL_KONAMI_P4IO_DDR_COIN;
        }
        if (P4IO_Bit(word, P4IO_BIT_SERVICE)) {
            buttons |= (uint32_t)1 << SDL_KONAMI_P4IO_DDR_SERVICE;
        }
        if (P4IO_Bit(word, P4IO_BIT_TEST)) {
            buttons |= (uint32_t)1 << SDL_KONAMI_P4IO_DDR_TEST;
        }
        break;
    default:
        return false;
    }
    state->buttons = buttons;
    return true;
}

static bool P4IO_IsEndpoint(const SDL_KonamiP4IOEndpoint *endpoint, uint8_t transfer, bool in)
{
    return (endpoint->attributes & 0x03) == transfer && ((endpoint->address & 0x80) != 0) == in;
}

bool SDL_KonamiP4IO_FindEndpoints(const SDL_KonamiP4IOEndpoint *endpoints, int count,
                                  SDL_KonamiP4IOEndpoints *out)
{
    if (!endpoints || !out || count != SDL_KONAMI_P4IO_ENDPOINTS) {
        return false;
    }
    if (!P4IO_IsEndpoint(&endpoints[0], P4IO_TRANSFER_BULK, false) ||
        !P4IO_IsEndpoint(&endpoints[1], P4IO_TRANSFER_BULK, true) ||
        !P4IO_IsEndpoint(&endpoints[2], P4IO_TRANSFER_INTERRUPT, true)) {
        return false;
    }
    out->bulk_out = endpoints[0].address;
    out->bulk_in = endpoints[1].address;
    out->interrupt_in = endpoints[2].address;
    return true;
}

size_t SDL_KonamiP4IO_BuildRequest(uint8_t command, uint8_t sequence, const uint8_t *payload, size_t length,
                                   uint8_t out[SDL_KONAMI_P4IO_REQUEST_MAX])
{
    if (length > SDL_KONAMI_P4IO_MAX_PAYLOAD || (length && !payload)) {
        return 0;
    }
    out[0] = SDL_KONAMI_P4IO_SOF;
    out[1] = command;
    out[2] = sequence;
    out[3] = (uint8_t)length;
    if (length) {
        memcpy(&out[SDL_KONAMI_P4IO_HEADER_LENGTH], payload, length);
    }
    return SDL_KONAMI_P4IO_HEADER_LENGTH + length;
}

bool SDL_KonamiP4IO_ParseReply(const uint8_t *reply, size_t length, uint8_t sequence,
                               const uint8_t **payload, size_t *payload_length)
{
    size_t declared;

    if (!reply || length < SDL_KONAMI_P4IO_HEADER_LENGTH) {
        return false;
    }
    if (reply[0] != SDL_KONAMI_P4IO_SOF || reply[2] != sequence) {
        return false;
    }
    declared = reply[3];
    if (declared > SDL_KONAMI_P4IO_MAX_PAYLOAD || declared > length - SDL_KONAMI_P4IO_HEADER_LENGTH) {
        return false;
    }
    if (payload) {
        *payload = &reply[SDL_KONAMI_P4IO_HEADER_LENGTH];
    }
    if (payload_length) {
        *payload_length = declared;
    }
    return true;
}

/* Up to size characters, ending at the first NUL */
static void P4IO_CopyText(char *out, const uint8_t *text, size_t size)
{
    size_t i;

    for (i = 0; i < size && text[i]; ++i) {
        out[i] = (text[i] >= 0x20 && text[i] <= 0x7E) ? (char)text[i] : '?';
    }
    out[i] = '\0';
}

bool SDL_KonamiP4IO_ParseDeviceInfo(const uint8_t *payload, size_t length, SDL_KonamiP4IODeviceInfo *out)
{
    if (!payload || !out || length != SDL_KONAMI_P4IO_DEVICE_INFO_LENGTH) {
        return false;
    }
    memcpy(out->type, &payload[0], sizeof(out->type));
    /* Byte 4 pads */
    out->major = payload[5];
    out->minor = payload[6];
    out->revision = payload[7];
    P4IO_CopyText(out->product, &payload[8], 4);
    P4IO_CopyText(out->date, &payload[12], 16);
    P4IO_CopyText(out->time, &payload[28], 16);
    return true;
}

void SDL_KonamiP4IO_StartupInit(SDL_KonamiP4IOStartup *startup)
{
    memset(startup, 0, sizeof(*startup));
    startup->step = SDL_KONAMI_P4IO_STARTUP_INIT;
}

size_t SDL_KonamiP4IO_StartupNext(const SDL_KonamiP4IOStartup *startup, uint8_t out[SDL_KONAMI_P4IO_REQUEST_MAX])
{
    switch (startup->step) {
    case SDL_KONAMI_P4IO_STARTUP_INIT:
        return SDL_KonamiP4IO_BuildRequest(SDL_KONAMI_P4IO_CMD_INIT, startup->sequence, NULL, 0, out);
    case SDL_KONAMI_P4IO_STARTUP_DEVICE_INFO:
        return SDL_KonamiP4IO_BuildRequest(SDL_KONAMI_P4IO_CMD_GET_DEVICE_INFO, startup->sequence, NULL, 0, out);
    default:
        return 0;
    }
}

void SDL_KonamiP4IO_StartupResult(SDL_KonamiP4IOStartup *startup, bool exchanged, const uint8_t *reply,
                                  size_t length)
{
    const uint8_t *payload;
    size_t payload_length;

    switch (startup->step) {
    case SDL_KONAMI_P4IO_STARTUP_INIT:
        /* bemanitools ignores how INIT went and describes its reply as zero
           bytes, so nothing depends on it */
        startup->step = SDL_KONAMI_P4IO_STARTUP_DEVICE_INFO;
        break;
    case SDL_KONAMI_P4IO_STARTUP_DEVICE_INFO:
        if (exchanged &&
            SDL_KonamiP4IO_ParseReply(reply, length, startup->sequence, &payload, &payload_length) &&
            SDL_KonamiP4IO_ParseDeviceInfo(payload, payload_length, &startup->info)) {
            startup->identified = true;
        }
        startup->step = SDL_KONAMI_P4IO_STARTUP_DONE;
        break;
    default:
        return;
    }
    ++startup->sequence;
}
