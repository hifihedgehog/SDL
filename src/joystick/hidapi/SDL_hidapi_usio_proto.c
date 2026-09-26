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

/* The Namco USIO. See SDL_hidapi_usio_proto.h. */

#include "SDL_hidapi_usio_proto.h"

#include <string.h>

/* The Taiko block at register 1080: the panel word at byte 0, the coin
 * counter at byte 16, and from byte 32 the four pad sensors of player 1,
 * then of player 2, each 16-bit little-endian (usio.cpp:211-299,
 * taiko_frame.c:109-160, usio_driver.c:320-379). */
#define USIO_TAIKO_PANEL          0
#define USIO_TAIKO_COINS          16
#define USIO_TAIKO_SENSORS        32
#define USIO_TAIKO_PLAYER_STRIDE  8
#define USIO_TAIKO_PLAYERS        2
#define USIO_TAIKO_PADS           4

/* Panel bits (usio.cpp:244-258, :286-287) */
#define USIO_TAIKO_TEST    0x0080
#define USIO_TAIKO_ENTER   0x0200
#define USIO_TAIKO_DOWN    0x1000
#define USIO_TAIKO_UP      0x2000
#define USIO_TAIKO_SERVICE 0x4000

/* The Tekken block at register 1000, from RPCS3 alone: one 64-bit
 * little-endian word per pair of players, players 1 and 2 at 0x100 and
 * players 3 and 4 at 0x080, each followed 0x10 bytes later by the pair's
 * coin counter (usio.cpp:428-432). The second player of a pair sits 24 bits
 * up (usio.cpp:312). Bytes 0 and 1 repeat player 1 in another order
 * (usio.cpp:344-386, :434) and byte 2 holds DIP switches (usio.cpp:436).
 * Neither is read. */
#define USIO_TEKKEN_PAIR0   0x100
#define USIO_TEKKEN_PAIR1   0x080
#define USIO_TEKKEN_COINS   0x10
#define USIO_TEKKEN_SHIFT   24
#define USIO_TEKKEN_PAIRS   2
#define USIO_TEKKEN_PLAYERS 4

/* Bits of one player's slot (usio.cpp:337-404). Test and Service are in the
 * first player's slot of each pair only (usio.cpp:337-340, :415-420). */
#define USIO_TEKKEN_TEST    0x00000080u
#define USIO_TEKKEN_SERVICE 0x00004000u
#define USIO_TEKKEN_BUTTON2 0x00010000u
#define USIO_TEKKEN_BUTTON1 0x00020000u
#define USIO_TEKKEN_RIGHT   0x00040000u
#define USIO_TEKKEN_LEFT    0x00080000u
#define USIO_TEKKEN_DOWN    0x00100000u
#define USIO_TEKKEN_UP      0x00200000u
#define USIO_TEKKEN_ENTER   0x00800000u
#define USIO_TEKKEN_BUTTON4 0x20000000u
#define USIO_TEKKEN_BUTTON3 0x40000000u
#define USIO_TEKKEN_BUTTON5 0x80000000u

/* A counter that moves up by this much or more went back, as after a reset.
 * Half the 16-bit range, as the JVS module takes half of its 14-bit one
 * (SDL_serial_jvs_proto.c:530-534). */
#define USIO_COIN_WENT_BACK 0x8000

static const SDL_USIOJoystickInfo USIO_TaikoJoysticks[USIO_TAIKO_PLAYERS] = {
    { "Namco USIO Taiko Drum P1", 4, 10, 0 },
    { "Namco USIO Taiko Drum P2", 4, 4, 0 },
};

static const SDL_USIOJoystickInfo USIO_TekkenJoysticks[USIO_TEKKEN_PLAYERS] = {
    { "Namco USIO Tekken P1", 0, 9, 1 },
    { "Namco USIO Tekken P2", 0, 6, 1 },
    { "Namco USIO Tekken P3", 0, 9, 1 },
    { "Namco USIO Tekken P4", 0, 6, 1 },
};

static uint16_t USIO_Read16(const uint8_t *data)
{
    return (uint16_t)(data[0] | (data[1] << 8));
}

static uint64_t USIO_Read64(const uint8_t *data)
{
    uint64_t value = 0;
    int i;

    for (i = 7; i >= 0; --i) {
        value = (value << 8) | data[i];
    }
    return value;
}

uint8_t SDL_USIO_CheckByte(uint16_t reg)
{
    return (uint8_t)(~(reg >> 8) & 0xF0);
}

bool SDL_USIO_EncodeCommand(uint8_t operation, uint8_t channel, uint16_t reg, uint16_t length,
                            uint8_t command[SDL_USIO_COMMAND_SIZE])
{
    /* Reads of channel 0 alone. A write or an init changes the board,
     * channel 1 takes firmware, and channels 2 and up are backup SRAM pages
     * (usio.cpp:629-709, :784-812). */
    if (!command || operation != SDL_USIO_READ || channel != 0 || length == 0 || length > SDL_USIO_MAX_LENGTH) {
        return false;
    }
    command[0] = (uint8_t)(operation | channel);
    command[1] = SDL_USIO_CheckByte(reg);
    command[2] = (uint8_t)(reg & 0xFF);
    command[3] = (uint8_t)(reg >> 8);
    command[4] = (uint8_t)(length & 0xFF);
    command[5] = (uint8_t)(length >> 8);
    return true;
}

bool SDL_USIO_IsIdentified(const uint8_t *block, size_t length)
{
    static const uint8_t prefix[5] = { 0x4E, 0x42, 0x47, 0x49, 0x2E };

    return block && length >= sizeof(prefix) && memcmp(block, prefix, sizeof(prefix)) == 0;
}

SDL_USIOLayout SDL_USIO_LayoutFromHint(const char *value)
{
    static const char tekken[] = "tekken";
    size_t i;

    if (!value) {
        return SDL_USIO_LAYOUT_TAIKO;
    }
    for (i = 0; i < sizeof(tekken) - 1; ++i) {
        char c = value[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != tekken[i]) {
            return SDL_USIO_LAYOUT_TAIKO;
        }
    }
    return (value[i] == '\0') ? SDL_USIO_LAYOUT_TEKKEN : SDL_USIO_LAYOUT_TAIKO;
}

int SDL_USIO_PlayerCount(SDL_USIOLayout layout)
{
    return (layout == SDL_USIO_LAYOUT_TEKKEN) ? USIO_TEKKEN_PLAYERS : USIO_TAIKO_PLAYERS;
}

const char *SDL_USIO_DeviceName(SDL_USIOLayout layout)
{
    return (layout == SDL_USIO_LAYOUT_TEKKEN) ? "Namco USIO Tekken" : "Namco USIO Taiko Drum";
}

uint8_t SDL_USIO_JoystickType(SDL_USIOLayout layout)
{
    return (layout == SDL_USIO_LAYOUT_TEKKEN) ? SDL_USIO_JOYSTICK_ARCADE_STICK : SDL_USIO_JOYSTICK_DRUM_KIT;
}

uint8_t SDL_USIO_GUIDByte(SDL_USIOLayout layout)
{
    return (layout == SDL_USIO_LAYOUT_TEKKEN) ? SDL_USIO_GUID_TEKKEN : SDL_USIO_GUID_TAIKO;
}

bool SDL_USIO_GetJoystickInfo(SDL_USIOLayout layout, int player, SDL_USIOJoystickInfo *info)
{
    if (!info || player < 0 || player >= SDL_USIO_PlayerCount(layout)) {
        return false;
    }
    *info = (layout == SDL_USIO_LAYOUT_TEKKEN) ? USIO_TekkenJoysticks[player] : USIO_TaikoJoysticks[player];
    return true;
}

const char *SDL_USIO_GetMapping(uint8_t guid_byte)
{
    return (guid_byte == SDL_USIO_GUID_TEKKEN) ? SDL_USIO_TEKKEN_MAPPING : NULL;
}

void SDL_USIO_Init(SDL_USIOState *state, SDL_USIOLayout layout, bool require_ident)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->layout = (layout == SDL_USIO_LAYOUT_TEKKEN) ? SDL_USIO_LAYOUT_TEKKEN : SDL_USIO_LAYOUT_TAIKO;
    state->require_ident = require_ident;
    state->phase = SDL_USIO_PHASE_IDENTIFY;
}

bool SDL_USIO_NextCommand(SDL_USIOState *state, uint64_t now, uint8_t command[SDL_USIO_COMMAND_SIZE])
{
    uint16_t reg, length;

    if (!state || !command || state->waiting || state->phase == SDL_USIO_PHASE_REJECTED) {
        return false;
    }
    if (state->quiet) {
        if (now < state->quiet_until) {
            return false;
        }
        state->quiet = false;
    }
    if (state->phase == SDL_USIO_PHASE_IDENTIFY) {
        reg = SDL_USIO_REGISTER_IDENT;
        length = SDL_USIO_IDENT_LENGTH;
    } else if (state->layout == SDL_USIO_LAYOUT_TEKKEN) {
        reg = SDL_USIO_REGISTER_TEKKEN;
        length = SDL_USIO_TEKKEN_LENGTH;
    } else {
        reg = SDL_USIO_REGISTER_TAIKO;
        length = SDL_USIO_TAIKO_LENGTH;
    }
    if (!SDL_USIO_EncodeCommand(SDL_USIO_READ, 0, reg, length, command)) {
        return false;
    }
    state->waiting = true;
    state->length = length;
    state->received = 0;
    state->deadline = now + SDL_USIO_REPLY_TIMEOUT_NS;
    return true;
}

/* Everything the board told the module goes, and the next read is the
 * identification block again. The layout and the quiet wait stay. */
static void USIO_Restart(SDL_USIOState *state, const SDL_USIOSink *sink)
{
    const bool connected = state->connected;

    state->phase = SDL_USIO_PHASE_IDENTIFY;
    state->connected = false;
    state->failures = 0;
    state->baseline = false;
    memset(state->pending_coins, 0, sizeof(state->pending_coins));
    memset(state->coin_pressed, 0, sizeof(state->coin_pressed));
    memset(state->controls, 0, sizeof(state->controls));
    if (connected) {
        sink->disconnect(sink->userdata);
    }
}

static void USIO_Abandon(SDL_USIOState *state, uint64_t now, const SDL_USIOSink *sink)
{
    state->waiting = false;
    state->received = 0;
    state->quiet = true;
    state->quiet_until = now + SDL_USIO_QUIET_NS;
    if (state->failures >= SDL_USIO_MAX_FAILURES) {
        return; /* A board that never answered its identification: no more log lines */
    }
    ++state->failures;
    if (state->failures == 1) {
        sink->log(sink->userdata, "a read went unanswered and will be sent again");
    }
    if (state->failures == SDL_USIO_MAX_FAILURES && state->phase == SDL_USIO_PHASE_POLL) {
        sink->log(sink->userdata, "the board stopped answering and will be identified again");
        USIO_Restart(state, sink);
    }
}

void SDL_USIO_CommandDone(SDL_USIOState *state, int written, uint64_t now, const SDL_USIOSink *sink)
{
    if (!state || !sink || !state->waiting) {
        return;
    }
    if (written == SDL_USIO_COMMAND_SIZE) {
        /* The board takes a command only as one 6-byte transfer
         * (usio.cpp:855-859, usio_driver.c:646), and the reply wait starts
         * once it is out */
        state->deadline = now + SDL_USIO_REPLY_TIMEOUT_NS;
        return;
    }
    USIO_Abandon(state, now, sink);
}

static void USIO_Drop(SDL_USIOState *state, size_t count, uint64_t now, const SDL_USIOSink *sink)
{
    if (!state->quiet) {
        sink->log(sink->userdata, "dropped bytes that no read asked for");
    }
    state->dropped += (uint32_t)count;
    state->quiet = true;
    state->quiet_until = now + SDL_USIO_QUIET_NS;
}

static void USIO_Press(SDL_USIOControls *controls, bool pressed, int button)
{
    if (pressed) {
        controls->buttons = (uint16_t)(controls->buttons | (1u << button));
    }
}

/* One axis of the stick. Both directions together cancel, since a hat
 * cannot hold both (SDL_hidapi_guncon_proto.c:53, SDL_hidapi_xid_proto.c:327). */
static uint8_t USIO_HatAxis(bool negative, bool positive, uint8_t negative_bit, uint8_t positive_bit)
{
    if (negative == positive) {
        return 0;
    }
    return negative ? negative_bit : positive_bit;
}

/* Whether a coin slot's button is pressed in this block. The counter rises
 * by one per coin (usio.cpp:238-243, taiko_frame.c:100-104,
 * usio_driver.c:323-326). The first block after the start sets the baseline
 * and presses nothing. Each coin presses the button for one poll, with one
 * released poll before the next, so two coins read as two presses. */
static bool USIO_Coin(SDL_USIOState *state, int slot, uint16_t counter)
{
    if (state->baseline) {
        /* FFFF to 0000 is a rise of one */
        const uint16_t rise = (uint16_t)(counter - state->coins[slot]);

        if (rise < USIO_COIN_WENT_BACK) {
            const unsigned int pending = state->pending_coins[slot] + (unsigned int)rise;

            state->pending_coins[slot] = (uint8_t)((pending > SDL_USIO_MAX_PENDING_COINS) ? SDL_USIO_MAX_PENDING_COINS : pending);
        }
    }
    state->coins[slot] = counter;

    if (state->coin_pressed[slot]) {
        state->coin_pressed[slot] = false;
    } else if (state->pending_coins[slot] > 0) {
        --state->pending_coins[slot];
        state->coin_pressed[slot] = true;
    }
    return state->coin_pressed[slot];
}

static bool USIO_SameControls(const SDL_USIOControls *a, const SDL_USIOControls *b)
{
    int i;

    for (i = 0; i < SDL_USIO_MAX_AXES; ++i) {
        if (a->axes[i] != b->axes[i]) {
            return false;
        }
    }
    return a->buttons == b->buttons && a->hat == b->hat;
}

static void USIO_Publish(SDL_USIOState *state, const SDL_USIOControls *decoded, int players, const SDL_USIOSink *sink)
{
    int player;

    for (player = 0; player < players; ++player) {
        if (!USIO_SameControls(&state->controls[player], &decoded[player])) {
            state->controls[player] = decoded[player];
            sink->controls(sink->userdata, player, &state->controls[player]);
        }
    }
}

static void USIO_DecodeTaiko(SDL_USIOState *state, const SDL_USIOSink *sink)
{
    const uint8_t *block = state->reply;
    const uint16_t panel = USIO_Read16(&block[USIO_TAIKO_PANEL]);
    SDL_USIOControls decoded[USIO_TAIKO_PLAYERS];
    int player, pad;

    memset(decoded, 0, sizeof(decoded));
    for (player = 0; player < USIO_TAIKO_PLAYERS; ++player) {
        /* Side left, center left, center right, side right (usio.cpp:260-275,
         * taiko_frame.c:122-124). An axis is the sensor value halved, so the
         * whole 16-bit range fits the positive half of an SDL axis. */
        for (pad = 0; pad < USIO_TAIKO_PADS; ++pad) {
            const uint16_t value = USIO_Read16(&block[USIO_TAIKO_SENSORS + player * USIO_TAIKO_PLAYER_STRIDE + pad * 2]);

            decoded[player].axes[pad] = (int16_t)(value / 2);
            USIO_Press(&decoded[player], value >= SDL_USIO_TAIKO_HIT_THRESHOLD, pad);
        }
    }

    /* Player 1's panel. Test is the level the block carries. The emulators
     * latch it and turn it over on each press (usio.cpp:232-236,
     * taiko_frame.c:105-106, usio_driver.c:331-337), and no source says what
     * a real board sends. */
    USIO_Press(&decoded[0], (panel & USIO_TAIKO_ENTER) != 0, SDL_USIO_TAIKO_ENTER);
    USIO_Press(&decoded[0], (panel & USIO_TAIKO_UP) != 0, SDL_USIO_TAIKO_UP);
    USIO_Press(&decoded[0], (panel & USIO_TAIKO_DOWN) != 0, SDL_USIO_TAIKO_DOWN);
    USIO_Press(&decoded[0], (panel & USIO_TAIKO_SERVICE) != 0, SDL_USIO_TAIKO_SERVICE);
    USIO_Press(&decoded[0], (panel & USIO_TAIKO_TEST) != 0, SDL_USIO_TAIKO_TEST);
    USIO_Press(&decoded[0], USIO_Coin(state, 0, USIO_Read16(&block[USIO_TAIKO_COINS])), SDL_USIO_TAIKO_COIN);
    state->baseline = true;

    USIO_Publish(state, decoded, USIO_TAIKO_PLAYERS, sink);
}

static void USIO_DecodeTekken(SDL_USIOState *state, const SDL_USIOSink *sink)
{
    static const uint16_t pair_offset[USIO_TEKKEN_PAIRS] = { USIO_TEKKEN_PAIR0, USIO_TEKKEN_PAIR1 };
    const uint8_t *block = state->reply;
    SDL_USIOControls decoded[USIO_TEKKEN_PLAYERS];
    int pair, half;

    memset(decoded, 0, sizeof(decoded));
    for (pair = 0; pair < USIO_TEKKEN_PAIRS; ++pair) {
        const uint64_t word = USIO_Read64(&block[pair_offset[pair]]);
        const bool coin = USIO_Coin(state, pair, USIO_Read16(&block[pair_offset[pair] + USIO_TEKKEN_COINS]));

        for (half = 0; half < 2; ++half) {
            SDL_USIOControls *controls = &decoded[pair * 2 + half];
            const uint32_t slot = (uint32_t)(word >> (half * USIO_TEKKEN_SHIFT));

            USIO_Press(controls, (slot & USIO_TEKKEN_BUTTON1) != 0, SDL_USIO_TEKKEN_BUTTON1);
            USIO_Press(controls, (slot & USIO_TEKKEN_BUTTON2) != 0, SDL_USIO_TEKKEN_BUTTON2);
            USIO_Press(controls, (slot & USIO_TEKKEN_BUTTON3) != 0, SDL_USIO_TEKKEN_BUTTON3);
            USIO_Press(controls, (slot & USIO_TEKKEN_BUTTON4) != 0, SDL_USIO_TEKKEN_BUTTON4);
            USIO_Press(controls, (slot & USIO_TEKKEN_BUTTON5) != 0, SDL_USIO_TEKKEN_BUTTON5);
            USIO_Press(controls, (slot & USIO_TEKKEN_ENTER) != 0, SDL_USIO_TEKKEN_ENTER);
            controls->hat = (uint8_t)(USIO_HatAxis((slot & USIO_TEKKEN_UP) != 0, (slot & USIO_TEKKEN_DOWN) != 0, SDL_USIO_HAT_UP, SDL_USIO_HAT_DOWN) |
                                      USIO_HatAxis((slot & USIO_TEKKEN_LEFT) != 0, (slot & USIO_TEKKEN_RIGHT) != 0, SDL_USIO_HAT_LEFT, SDL_USIO_HAT_RIGHT));
            if (half == 0) {
                /* Test is the level the block carries, as for the Taiko
                 * block. RPCS3 latches it (usio.cpp:323-329). */
                USIO_Press(controls, (slot & USIO_TEKKEN_TEST) != 0, SDL_USIO_TEKKEN_TEST);
                USIO_Press(controls, (slot & USIO_TEKKEN_SERVICE) != 0, SDL_USIO_TEKKEN_SERVICE);
                USIO_Press(controls, coin, SDL_USIO_TEKKEN_COIN);
            }
        }
    }
    state->baseline = true;

    USIO_Publish(state, decoded, USIO_TEKKEN_PLAYERS, sink);
}

/* A whole reply to the read that was out */
static void USIO_Complete(SDL_USIOState *state, const SDL_USIOSink *sink)
{
    if (state->phase == SDL_USIO_PHASE_IDENTIFY) {
        if (!SDL_USIO_IsIdentified(state->reply, state->length)) {
            sink->log(sink->userdata, "the identification block does not begin with NBGI.");
            if (state->require_ident) {
                /* 0900 is used only when its block says USIO. The only sign
                 * that 0900 speaks this protocol is the game's probe, which
                 * checks the vendor ID and the product ID's high byte, 09,
                 * alone (bpreader_hook.c:95-98, :110), and RPCS3 names the
                 * range 0900 to 0910 as one device (sys_usbd.cpp:270-271). */
                state->phase = SDL_USIO_PHASE_REJECTED;
                sink->log(sink->userdata, "0B9A:0900 left alone, not a USIO");
                return;
            }
        }
        /* The first input block sets the coin baseline, since every way
           into this phase clears it */
        state->phase = SDL_USIO_PHASE_POLL;
        state->connected = true;
        sink->connect(sink->userdata);
        return;
    }
    if (state->layout == SDL_USIO_LAYOUT_TEKKEN) {
        USIO_DecodeTekken(state, sink);
    } else {
        USIO_DecodeTaiko(state, sink);
    }
}

void SDL_USIO_Feed(SDL_USIOState *state, const uint8_t *data, size_t length, uint64_t now,
                   const SDL_USIOSink *sink)
{
    size_t take;

    /* A zero-length transfer changes nothing. ITAIKO-firmware sends one
     * before the first command and after a reply of whole packets
     * (usio_driver.c:486-519), and the host's read of it completes with 0
     * bytes (RPCS3 a4f7edaa5e). */
    if (!state || !sink || !data || length == 0) {
        return;
    }
    if (!state->waiting) {
        USIO_Drop(state, length, now, sink);
        return;
    }

    /* The reply comes in transfers of up to 64 bytes and is exactly as long
     * as the read asked, cut or padded by the board (usio.cpp:781,
     * usio_driver.c:385-395). It is used only when whole. */
    take = state->length - state->received;
    if (take > length) {
        take = length;
    }
    memcpy(&state->reply[state->received], data, take);
    state->received += take;
    if (state->received < state->length) {
        return;
    }
    state->waiting = false;
    state->failures = 0;
    USIO_Complete(state, sink);
    if (length > take) {
        USIO_Drop(state, length - take, now, sink);
    }
}

void SDL_USIO_Tick(SDL_USIOState *state, uint64_t now, const SDL_USIOSink *sink)
{
    if (!state || !sink) {
        return;
    }
    if (state->waiting && now >= state->deadline) {
        USIO_Abandon(state, now, sink);
    }
}

const SDL_USIOControls *SDL_USIO_GetControls(const SDL_USIOState *state, int player)
{
    if (!state || player < 0 || player >= SDL_USIO_PlayerCount(state->layout)) {
        return NULL;
    }
    return &state->controls[player];
}

void SDL_USIO_Detach(SDL_USIOState *state, const SDL_USIOSink *sink)
{
    SDL_USIOLayout layout;
    bool require_ident;

    if (!state || !sink) {
        return;
    }
    if (state->connected) {
        state->connected = false;
        sink->disconnect(sink->userdata);
    }
    layout = state->layout;
    require_ident = state->require_ident;
    SDL_USIO_Init(state, layout, require_ident);
}
