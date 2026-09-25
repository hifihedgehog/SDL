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

/* FlySky i-BUS from an FS-iA6B receiver, protocol token "ibus". 115200 baud
 * 8N1, receive only. Each 32-byte frame is 20 40, fourteen 16-bit
 * little-endian channels and a little-endian checksum, FFFF less every byte
 * before it. The facts are from vJoySerialFeeder (GPL, facts only) and
 * Linux's fsia6b driver (GPL, facts only). No code from them is copied.
 */

#ifndef SDL_serial_ibus_proto_h_
#define SDL_serial_ibus_proto_h_

#include "SDL_serial_proto.h"

#define SDL_IBUS_RATE       115200
#define SDL_IBUS_FRAME      32
#define SDL_IBUS_LENGTH     0x20 /* The first byte, the frame's length */
#define SDL_IBUS_COMMAND    0x40 /* Channel data */
#define SDL_IBUS_CHANNELS   14
#define SDL_IBUS_SILENCE_MS 500
#define SDL_IBUS_MIN        1000
#define SDL_IBUS_MAX        2000

typedef struct SDL_IBusState
{
    SDL_SerialBase base;
    uint8_t window[SDL_IBUS_FRAME];
    int length;
    bool timer;
    uint64_t deadline;
    int bad_frames;
} SDL_IBusState;

extern const SDL_SerialModule SDL_SerialIBusModule;

/* A whole frame: true when its checksum holds, with the channels masked to
 * 12 bits */
extern bool SDL_IBus_DecodeFrame(const uint8_t *frame, size_t length, uint16_t *channels);

#endif /* SDL_serial_ibus_proto_h_ */
