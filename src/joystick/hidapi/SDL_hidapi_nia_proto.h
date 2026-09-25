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

/* The OCZ Neural Impulse Actuator, 1234:0000 with the manufacturer string
 * "Brain Actuated Technologies". Pure C99: no SDL runtime and no I/O.
 *
 * Each 55-byte report, no report ID, holds up to 16 samples of 3 bytes,
 * little-endian, the count in byte 54 and a counter in bytes 52-53 that
 * advances by that count. A sample is offset binary around 0x800000, as the
 * headband-on logs of nia4linux show. The facts are from pynia,
 * nia_reaction_lab, nia4linux and drwho.virtadpt.net. No code is copied.
 */

#ifndef SDL_hidapi_nia_proto_h_
#define SDL_hidapi_nia_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_NIA_REPORT_LENGTH 55
#define SDL_NIA_MAX_SAMPLES   16
#define SDL_NIA_MANUFACTURER  "Brain Actuated Technologies"

typedef struct SDL_NIAReport
{
    int count;                             /* The valid samples, byte 54 clamped to 16 */
    uint8_t raw_count;                     /* Byte 54, what the counter advances by */
    int32_t samples[SDL_NIA_MAX_SAMPLES];  /* Each value minus 0x800000 */
    uint16_t counter;
} SDL_NIAReport;

typedef struct SDL_NIAState
{
    bool have_counter;
    uint16_t counter;
    bool gap;     /* The last report's counter did not advance by its count */
    int16_t axis; /* Axis 0: the last valid sample */
} SDL_NIAState;

/* Whether the manufacturer string names the NIA's maker */
extern bool SDL_NIA_IsManufacturer(const char *manufacturer);

/* One report of exactly 55 bytes. Samples past the count are never read. */
extern bool SDL_NIA_DecodeReport(const uint8_t *report, size_t length, SDL_NIAReport *out);

/* A sample as an axis: the offset divided by 256, clamped */
extern int16_t SDL_NIA_SampleAxis(int32_t offset);

/* No samples, no counter, the axis centered */
extern void SDL_NIA_Init(SDL_NIAState *state);

/* Decodes one report into out and moves the state: the gap flag, and the
 * axis when the report holds a sample */
extern bool SDL_NIA_HandleReport(SDL_NIAState *state, const uint8_t *report, size_t length, SDL_NIAReport *out);

#endif /* SDL_hidapi_nia_proto_h_ */
