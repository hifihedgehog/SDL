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

/* The Zwift Play and Zwift Click: protocol buffers messages on a vendor
 * service after a RideOn handshake, plain on current firmware and encrypted
 * on older firmware. See hifihedgehog/SDL#33 Part 12. */

#ifndef SDL_ble_zwift_proto_h_
#define SDL_ble_zwift_proto_h_

#include "SDL_ble_proto.h"

/* Characteristic indices */
#define SDL_ZWIFT_ASYNC   0
#define SDL_ZWIFT_SYNC_TX 1
#define SDL_ZWIFT_SYNC_RX 2

/* The type byte after company 0x094A in the manufacturer data */
#define SDL_ZWIFT_PLAY_RIGHT 0x02
#define SDL_ZWIFT_PLAY_LEFT  0x03
#define SDL_ZWIFT_CLICK      0x09
#define SDL_ZWIFT_PLAY_FW2   0x0E

/* Message types */
#define SDL_ZWIFT_MESSAGE_PLAY       0x07
#define SDL_ZWIFT_MESSAGE_IDLE       0x15
#define SDL_ZWIFT_MESSAGE_BATTERY    0x19
#define SDL_ZWIFT_MESSAGE_RIDE       0x23
#define SDL_ZWIFT_MESSAGE_CLICK      0x37
#define SDL_ZWIFT_MESSAGE_INFO       0x3C
#define SDL_ZWIFT_MESSAGE_DISCONNECT 0xFE

#define SDL_ZWIFT_HANDSHAKE_MS 10000 /* qdomyos-zwift zwiftclickremote.cpp:31-48 */
#define SDL_ZWIFT_KEY_SIZE     64
#define SDL_ZWIFT_FRAME_EXTRA  8     /* 4 counter bytes and a 4-byte tag */

/* The key exchange of older firmware, which SDL_ble_zwift_crypto.c provides
   with Windows' CNG. One per connection. */
typedef struct SDL_ZwiftCrypto
{
    void *userdata;
    /* Makes a P-256 key pair. public_key receives X then Y, big-endian. */
    bool (*MakeKey)(void *userdata, uint8_t *public_key);
    /* Derives the session key from the device's 64-byte public key */
    bool (*Derive)(void *userdata, const uint8_t *device_key);
    /* Decrypts 4 counter bytes, the ciphertext and a 4-byte tag into
       length - 8 bytes of plain. False when the tag does not match. */
    bool (*Decrypt)(void *userdata, const uint8_t *frame, size_t length, uint8_t *plain);
} SDL_ZwiftCrypto;

typedef enum SDL_ZwiftPhase
{
    SDL_ZWIFT_IDLE,
    SDL_ZWIFT_PLAIN,     /* RideOn written, a decoded ASYNC message awaited */
    SDL_ZWIFT_KEY_SENT,  /* The host key written, the device key awaited */
    SDL_ZWIFT_ENCRYPTED
} SDL_ZwiftPhase;

typedef struct SDL_ZwiftState
{
    SDL_BLEBase base;
    uint8_t variant;
    const SDL_ZwiftCrypto *crypto;
    uint8_t phase; /* SDL_ZwiftPhase */
    bool started;
    bool timing;   /* The handshake write is out: its completion starts the timer */
    bool armed;    /* The handshake's deadline counts */
    uint64_t deadline;
} SDL_ZwiftState;

/* Protocol buffers zigzag of a sint32 */
extern int32_t SDL_Zwift_ZigZag(uint64_t value);

extern const SDL_BLEModule SDL_BLEZwiftModule;
extern const SDL_BLEFamily SDL_BLEZwiftFamily;

#endif /* SDL_ble_zwift_proto_h_ */
