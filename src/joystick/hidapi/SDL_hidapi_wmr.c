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
#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_HIDAPI

#include "../../SDL_hints_c.h"
#include "../SDL_sysjoystick.h"
#include "SDL_hidapijoystick_c.h"
#include "../../hidapi/SDL_hidapi_c.h"
#include "SDL_hidapi_wmr_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_WMR

/* The Windows Mixed Reality motion controllers paired to the PC over
 * Bluetooth: the first generation 045E:065B, the Samsung Odyssey 045E:065D
 * and the HP Reverb G2 045E:066A. At open the driver runs Monado's start-up:
 * a reset, a quiesce, reads of firmware blocks 0 and 3 and of the
 * configuration block, and then the commands that turn on the status
 * reports and the IMU. The joystick connects once that start-up is done.
 * SDL_SENSOR_ACCEL and SDL_SENSOR_GYRO carry the samples calibrated with the
 * configuration block. The protocol lives in SDL_hidapi_wmr_proto.c, where
 * the offline tests run it. */

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_WMRSession session;
    SDL_WMRModel model;
    Uint8 *config; /* Block 2 during the start-up */
    bool reported; /* The start-up's end has been handled */
} SDL_DriverWMR_Context;

static void HIDAPI_DriverWMR_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_WMR, callback, userdata);
}

static void HIDAPI_DriverWMR_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_WMR, callback, userdata);
}

static bool HIDAPI_DriverWMR_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_WMR, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverWMR_Write(void *userdata, const uint8_t *data, size_t length)
{
    SDL_DriverWMR_Context *ctx = (SDL_DriverWMR_Context *)userdata;

    return SDL_hid_write(ctx->device->dev, data, length) >= 0;
}

static bool HIDAPI_DriverWMR_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return SDL_WMR_IsControllerID(vendor_id, product_id, NULL);
}

static const char *HIDAPI_DriverWMR_Name(SDL_WMRHand hand)
{
    switch (hand) {
    case SDL_WMR_HAND_LEFT:
        return "Windows Mixed Reality Motion Controller (Left)";
    case SDL_WMR_HAND_RIGHT:
        return "Windows Mixed Reality Motion Controller (Right)";
    default:
        return "Windows Mixed Reality Motion Controller";
    }
}

static bool HIDAPI_DriverWMR_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverWMR_Context *ctx = (SDL_DriverWMR_Context *)SDL_calloc(1, sizeof(*ctx));
    SDL_WMRSink sink;
    /* hid.dll returns every input report at the collection's longest input
       report length */
#if defined(SDL_PLATFORM_WIN32) || defined(SDL_PLATFORM_WINGDK)
    const bool padded = true;
#else
    const bool padded = false;
#endif

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;
    ctx->config = (Uint8 *)SDL_malloc(SDL_WMR_BLOCK_MAXIMUM);
    if (!ctx->config) {
        return false;
    }
    (void)SDL_WMR_IsControllerID(device->vendor_id, device->product_id, &ctx->model);

    // Monado tells the hands apart by the product string alone
    HIDAPI_SetDeviceName(device, HIDAPI_DriverWMR_Name(SDL_WMR_GetHand(device->product_string)));
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;

    sink.userdata = ctx;
    sink.write = HIDAPI_DriverWMR_Write;
    SDL_WMR_Open(&ctx->session, ctx->model, padded, ctx->config, SDL_WMR_BLOCK_MAXIMUM, SDL_GetTicks(), &sink);
    // The joystick connects when the start-up is done
    return true;
}

static const char *HIDAPI_DriverWMR_FailureText(SDL_WMRFailure failure)
{
    switch (failure) {
    case SDL_WMR_FAILURE_WRITE:
        return "a command could not be written";
    case SDL_WMR_FAILURE_TIMEOUT:
        return "no answer within 250 ms";
    case SDL_WMR_FAILURE_REPLY:
        return "an answer of the wrong length or command";
    case SDL_WMR_FAILURE_BLOCK:
        return "a block of the wrong size";
    case SDL_WMR_FAILURE_CONFIG:
        return "configuration JSON that Monado refuses";
    default:
        return "unknown";
    }
}

static void HIDAPI_DriverWMR_Post(SDL_Joystick *joystick, const SDL_WMREvent *event)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    Uint8 i;

    for (i = 0; i < event->state.num_axes; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, i, event->state.axes[i]);
    }
    for (i = 0; i < SDL_WMR_BUTTONS; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, i, event->state.buttons[i]);
    }
    SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_ACCEL, event->timestamp_ns, event->accel, 3);
    SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_GYRO, event->timestamp_ns, event->gyro, 3);
}

static bool HIDAPI_DriverWMR_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverWMR_Context *ctx = (SDL_DriverWMR_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    SDL_WMRSink sink;
    Uint8 data[256];
    int size;

    sink.userdata = ctx;
    sink.write = HIDAPI_DriverWMR_Write;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        SDL_WMREvent event;

        if (SDL_WMR_HandleReport(&ctx->session, SDL_GetTicks(), data, (size_t)size, &sink, &event) == SDL_WMR_REPORT_STATUS &&
            joystick) {
            HIDAPI_DriverWMR_Post(joystick, &event);
        }
    }

    if (size < 0) {
        // Read error, device is disconnected
        if (device->num_joysticks > 0) {
            HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        }
        return false;
    }

    SDL_WMR_Update(&ctx->session, SDL_GetTicks(), &sink);
    if (!ctx->reported) {
        if (ctx->session.phase == SDL_WMR_PHASE_READY) {
            ctx->reported = true;
            SDL_free(ctx->config);
            ctx->config = NULL;
            HIDAPI_JoystickConnected(device, NULL);
        } else if (ctx->session.phase == SDL_WMR_PHASE_FAILED) {
            ctx->reported = true;
            SDL_free(ctx->config);
            ctx->config = NULL;
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Windows Mixed Reality controller start-up failed: %s",
                         HIDAPI_DriverWMR_FailureText(ctx->session.failure));
        }
    }
    return true;
}

static bool HIDAPI_DriverWMR_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverWMR_Context *ctx = (SDL_DriverWMR_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->naxes = SDL_WMR_GetNumAxes(ctx->model);
    joystick->nbuttons = SDL_WMR_BUTTONS;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;
    // No source gives the controllers' sample rate
    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 0.0f);
    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, 0.0f);
    return true;
}

static bool HIDAPI_DriverWMR_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    // The IMU runs from the start-up on, and SDL passes on samples only for enabled sensors
    return true;
}

static void HIDAPI_DriverWMR_FreeDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverWMR_Context *ctx = (SDL_DriverWMR_Context *)device->context;

    if (ctx) {
        SDL_free(ctx->config);
        ctx->config = NULL;
    }
}

static int HIDAPI_DriverWMR_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverWMR_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverWMR_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverWMR_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverWMR_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverWMR_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverWMR_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverWMR_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverWMR = {
    SDL_HINT_JOYSTICK_HIDAPI_WMR,
    true,
    HIDAPI_DriverWMR_RegisterHints,
    HIDAPI_DriverWMR_UnregisterHints,
    HIDAPI_DriverWMR_IsEnabled,
    HIDAPI_DriverWMR_IsSupportedDevice,
    HIDAPI_DriverWMR_InitDevice,
    HIDAPI_DriverWMR_GetDevicePlayerIndex,
    HIDAPI_DriverWMR_SetDevicePlayerIndex,
    HIDAPI_DriverWMR_UpdateDevice,
    HIDAPI_DriverWMR_OpenJoystick,
    HIDAPI_DriverWMR_RumbleJoystick,
    HIDAPI_DriverWMR_RumbleJoystickTriggers,
    HIDAPI_DriverWMR_GetJoystickCapabilities,
    HIDAPI_DriverWMR_SetJoystickLED,
    HIDAPI_DriverWMR_SendJoystickEffect,
    HIDAPI_DriverWMR_SetJoystickSensorsEnabled,
    HIDAPI_DriverWMR_CloseJoystick,
    HIDAPI_DriverWMR_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_WMR

#endif // SDL_JOYSTICK_HIDAPI
