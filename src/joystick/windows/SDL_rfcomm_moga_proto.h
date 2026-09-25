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

/* PowerA MOGA controllers in Mode A, on the Serial Port service.
 * hifihedgehog/SDL#33 Part 11.
 *
 * Commands are 5A 05 command id checksum, the checksum the XOR of the first
 * four bytes. At connect the host sends 43 with the controller id, then a
 * poll and the listen command: 45 and 46 for the second generation, whose
 * 14-byte reports carry analog triggers, and 41 and 44 for the first
 * generation and the Pocket, whose reports are 12 bytes. Reports are 7A,
 * the length, a code, the id, the controls, a byte of 10 and the XOR of all
 * earlier bytes. After 2000 ms without a report the host polls, a report
 * after the poll brings the listen command again, and 2000 ms more without
 * one ends the link. The ids 1 to 4 light one of the blue LEDs and 5 lights
 * none. The facts are from MogaSerial and moga-uinput (MIT) and zeemouse
 * (no license, facts only). No code from them is copied.
 */

#ifndef SDL_rfcomm_moga_proto_h_
#define SDL_rfcomm_moga_proto_h_

#include "SDL_rfcomm_proto.h"

#define SDL_MOGA_COMMAND_LENGTH 5
#define SDL_MOGA_QUIET_MS       2000
#define SDL_MOGA_MAX_REPORT     14

#define SDL_MOGA_SET_ID         0x43
#define SDL_MOGA_POLL_DIGITAL   0x41
#define SDL_MOGA_LISTEN_DIGITAL 0x44
#define SDL_MOGA_POLL_ANALOG    0x45
#define SDL_MOGA_LISTEN_ANALOG  0x46
#define SDL_MOGA_ID_NONE        5 /* No LED lit. The pad still works. */

typedef enum SDL_MOGAAxis
{
    SDL_MOGA_AXIS_LEFT_X,
    SDL_MOGA_AXIS_LEFT_Y,
    SDL_MOGA_AXIS_RIGHT_X,
    SDL_MOGA_AXIS_RIGHT_Y,
    SDL_MOGA_AXIS_LEFT_TRIGGER,
    SDL_MOGA_AXIS_RIGHT_TRIGGER,
    SDL_MOGA_AXES
} SDL_MOGAAxis;

typedef enum SDL_MOGAButton
{
    SDL_MOGA_BUTTON_A,
    SDL_MOGA_BUTTON_B,
    SDL_MOGA_BUTTON_X,
    SDL_MOGA_BUTTON_Y,
    SDL_MOGA_BUTTON_SELECT,
    SDL_MOGA_BUTTON_START,
    SDL_MOGA_BUTTON_L3,
    SDL_MOGA_BUTTON_R3,
    SDL_MOGA_BUTTON_L1,
    SDL_MOGA_BUTTON_R1,
    SDL_MOGA_BUTTON_UP,
    SDL_MOGA_BUTTON_DOWN,
    SDL_MOGA_BUTTON_LEFT,
    SDL_MOGA_BUTTON_RIGHT,
    SDL_MOGA_BUTTONS
} SDL_MOGAButton;

typedef struct SDL_MOGAState
{
    SDL_RFCOMMBase base;
    bool analog;         /* Second generation: 45, 46 and 14-byte reports */
    uint8_t id;          /* The controller id of every command */
    char name[SDL_SERIAL_NAME_LENGTH];
    uint8_t report[SDL_MOGA_MAX_REPORT];
    size_t length;       /* Bytes of a report held from earlier reads */
    bool polling;        /* A poll went out after a quiet period */
    uint64_t quiet_at;   /* The quiet-link rule runs then */
    SDL_SerialControls controls;
} SDL_MOGAState;

/* A MOGA name of the second generation. The first generation's names start
   "BD&A" or "BDA", in any case. */
extern bool SDL_MOGA_IsAnalogName(const char *name);
/* Player indexes 0 to 3 are ids 1 to 4. Any other index is 5. */
extern uint8_t SDL_MOGA_IdForPlayerIndex(int player_index);
/* Five bytes: 5A 05 command id checksum */
extern void SDL_MOGA_BuildCommand(uint8_t command, uint8_t id, uint8_t out[SDL_MOGA_COMMAND_LENGTH]);
/* A whole report of 12 or 14 bytes into the controls */
extern bool SDL_MOGA_DecodeReport(const uint8_t *report, size_t length, SDL_SerialControls *controls);

extern const SDL_RFCOMMModule SDL_RFCOMMMogaModule;

#endif /* SDL_rfcomm_moga_proto_h_ */
