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

/* VRinsight serial panels, protocol token "vrinsight". 115200 baud 8N1.
 * Every message is 8 bytes of ASCII padded with 00, in both directions. The
 * host writes CMDRST and CMDCON, then CMDFUN, whose answer names the panel,
 * then CMDVER. Panel messages are events, each pressing its button for
 * 100 ms. The facts are from a SerialFP2 capture with a CDU II, the CDU II
 * MSFS driver, VRInsight-Xplane-Interface and XPComboTest.
 */

#ifndef SDL_serial_vrinsight_proto_h_
#define SDL_serial_vrinsight_proto_h_

#include "SDL_serial_proto.h"

#define SDL_VRINSIGHT_RATE          115200
#define SDL_VRINSIGHT_MESSAGE       8
#define SDL_VRINSIGHT_REPLY_MS      3000
#define SDL_VRINSIGHT_KEEPALIVE_MS  60000
#define SDL_VRINSIGHT_CDU_BUTTONS   70
#define SDL_VRINSIGHT_COMBO_BUTTONS 72

typedef enum SDL_VRinsightPanel
{
    SDL_VRINSIGHT_PANEL_NONE,
    SDL_VRINSIGHT_PANEL_CDU2,
    SDL_VRINSIGHT_PANEL_COMBO1,
    SDL_VRINSIGHT_PANEL_COMBO2_AIRBUS,
    SDL_VRINSIGHT_PANEL_COMBO2_BOEING,
    SDL_VRINSIGHT_PANEL_MPANEL,
    SDL_VRINSIGHT_PANEL_OTHER
} SDL_VRinsightPanel;

typedef enum SDL_VRinsightStep
{
    SDL_VRINSIGHT_STEP_CONNECT,    /* CMDRST and CMDCON written, the CMDCON answer */
    SDL_VRINSIGHT_STEP_FUNCTION,   /* CMDFUN written, the panel's name */
    SDL_VRINSIGHT_STEP_PRESENT,
    SDL_VRINSIGHT_STEP_KEEPALIVE,  /* CMDCON written after 60 s of quiet, any answer */
    SDL_VRINSIGHT_STEP_UNSUPPORTED /* Identified, messages not known */
} SDL_VRinsightStep;

typedef struct SDL_VRinsightState
{
    SDL_SerialBase base;
    SDL_VRinsightStep step;
    SDL_VRinsightPanel panel;
    bool timer;
    uint64_t deadline;
    uint8_t action_seq;
    uint8_t waiting_seq;
    uint8_t message[SDL_VRINSIGHT_MESSAGE];
    int message_length;
    char version[SDL_VRINSIGHT_MESSAGE + 1];
    int dropped; /* Bytes dropped to resynchronize */
} SDL_VRinsightState;

extern const SDL_SerialModule SDL_SerialVRinsightModule;

/* The panel an answer to CMDFUN names */
extern SDL_VRinsightPanel SDL_VRinsight_Panel(const uint8_t *message);
/* The CDU II button of a message, -1 when none */
extern int SDL_VRinsight_CduButton(const uint8_t *message);
/* The MCP Combo I button of a message, -1 for values and unknown messages */
extern int SDL_VRinsight_ComboButton(const uint8_t *message);

#endif /* SDL_serial_vrinsight_proto_h_ */
