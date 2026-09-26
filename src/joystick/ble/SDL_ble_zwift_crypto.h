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

/* The key exchange of older Zwift firmware (hifihedgehog/SDL#33 Part 12)
 * with Windows' CNG, which bcrypt.dll provides and which is loaded at run
 * time: a P-256 key pair, ECDH, HKDF with SHA-256 and AES-CCM with a 4-byte
 * tag. One per connection. */

#ifndef SDL_ble_zwift_crypto_h_
#define SDL_ble_zwift_crypto_h_

#include "SDL_ble_zwift_proto.h"

#define SDL_ZWIFT_HKDF_SIZE 36 /* A 32-byte AES-256 key, then a 4-byte nonce prefix */

typedef struct SDL_ZwiftKeys SDL_ZwiftKeys;

/* A key exchange with its entry points in crypto. NULL when CNG cannot be
   loaded or lacks an algorithm. */
extern SDL_ZwiftKeys *SDL_ZwiftCrypto_Create(SDL_ZwiftCrypto *crypto);
extern void SDL_ZwiftCrypto_Destroy(SDL_ZwiftKeys *keys);

/* For tests: a fixed key pair in place of MakeKey's, the private scalar and
   the public X and Y, each 32 bytes big-endian */
extern bool SDL_ZwiftCrypto_SetKeyPair(SDL_ZwiftKeys *keys, const uint8_t *d, const uint8_t *x, const uint8_t *y);
/* For tests: the HKDF output of the last Derive */
extern bool SDL_ZwiftCrypto_GetDerived(const SDL_ZwiftKeys *keys, uint8_t *hkdf);

#endif /* SDL_ble_zwift_crypto_h_ */
