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

/* Zhen Hua 5-byte RC transmitters, protocol token "zhenhua". 19200 baud
 * 8N1, receive only. A raw EF starts each 5-byte frame, every byte arrives
 * bit-reversed, and the four channel bytes lie in 50-200 once reversed. The
 * facts are from inputattach and Linux's zhenhua driver (GPL, facts only).
 * No code from them is copied.
 */

#ifndef SDL_serial_zhenhua_proto_h_
#define SDL_serial_zhenhua_proto_h_

#include "SDL_serial_proto.h"

#define SDL_ZHENHUA_RATE       19200
#define SDL_ZHENHUA_SYNC       0xEF /* Raw, F7 once reversed */
#define SDL_ZHENHUA_FRAME      5
#define SDL_ZHENHUA_BYTE_MS    500  /* Detection: each byte. Presence: each frame. */
#define SDL_ZHENHUA_CHANNELS   4
#define SDL_ZHENHUA_MIN        50
#define SDL_ZHENHUA_MAX        200

typedef struct SDL_ZhenHuaState
{
    SDL_SerialBase base;
    bool detecting;
    int detect_length;  /* Bytes of the two frames so far, the EF included */
    bool timer;
    uint64_t deadline;
    uint8_t frame[SDL_ZHENHUA_FRAME]; /* Reversed */
    int frame_length;   /* 0 while no frame is open */
} SDL_ZhenHuaState;

extern const SDL_SerialModule SDL_SerialZhenHuaModule;

extern uint8_t SDL_ZhenHua_Reverse(uint8_t byte);

#endif /* SDL_serial_zhenhua_proto_h_ */
