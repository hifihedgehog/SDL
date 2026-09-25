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
#include "SDL_hidapi_riftdk1_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_RIFT_DK1

/* The Oculus Rift DK1 head tracker, 2833:0001 with the manufacturer string
 * "Oculus VR, Inc.". It sends report 1, up to three accelerometer and
 * gyroscope samples, only while the host writes keep-alive feature 8, which
 * goes out at open and every 3 s. The start-up clears the sensor-frame flag
 * of feature 2, so samples come in the headset's frame, and sets the packet
 * interval to 1. SDL_SENSOR_ACCEL and SDL_SENSOR_GYRO carry each sample with
 * the Oculus SDK's timing, and axes 0-2 are yaw, pitch and roll from
 * OpenHMD's fusion. The protocol lives in SDL_hidapi_riftdk1_proto.c, where
 * the offline tests run it. */

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_RiftDK1Session session;
} SDL_DriverRiftDK1_Context;

static void HIDAPI_DriverRiftDK1_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_RIFT_DK1, callback, userdata);
}

static void HIDAPI_DriverRiftDK1_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_RIFT_DK1, callback, userdata);
}

static bool HIDAPI_DriverRiftDK1_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_RIFT_DK1, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static int HIDAPI_DriverRiftDK1_GetFeature(void *userdata, uint8_t *data, size_t length)
{
    SDL_DriverRiftDK1_Context *ctx = (SDL_DriverRiftDK1_Context *)userdata;

    return SDL_hid_get_feature_report(ctx->device->dev, data, length);
}

static bool HIDAPI_DriverRiftDK1_SetFeature(void *userdata, const uint8_t *data, size_t length)
{
    SDL_DriverRiftDK1_Context *ctx = (SDL_DriverRiftDK1_Context *)userdata;

    return SDL_hid_send_feature_report(ctx->device->dev, data, length) >= 0;
}

static void HIDAPI_DriverRiftDK1_GetSink(SDL_DriverRiftDK1_Context *ctx, SDL_RiftDK1Sink *sink)
{
    sink->userdata = ctx;
    sink->get_feature = HIDAPI_DriverRiftDK1_GetFeature;
    sink->set_feature = HIDAPI_DriverRiftDK1_SetFeature;
}

static bool HIDAPI_DriverRiftDK1_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    if (vendor_id != USB_VENDOR_OCULUS || product_id != USB_PRODUCT_OCULUS_RIFT_DK1) {
        return false;
    }
    // Without a device the question is only whether the ID could be one
    return !device || SDL_RiftDK1_IsManufacturer(device->manufacturer_string);
}

static bool HIDAPI_DriverRiftDK1_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverRiftDK1_Context *ctx = (SDL_DriverRiftDK1_Context *)SDL_calloc(1, sizeof(*ctx));
    SDL_RiftDK1Sink sink;

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;

    HIDAPI_SetDeviceName(device, "Oculus Rift DK1");
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;

    HIDAPI_DriverRiftDK1_GetSink(ctx, &sink);
    SDL_RiftDK1_Start(&ctx->session, SDL_GetTicks(), &sink);
    return HIDAPI_JoystickConnected(device, NULL);
}

static bool HIDAPI_DriverRiftDK1_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverRiftDK1_Context *ctx = (SDL_DriverRiftDK1_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    SDL_RiftDK1Sink sink;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        SDL_RiftDK1Report report;
        SDL_RiftDK1Event events[SDL_RIFTDK1_MAX_SAMPLES + 1];
        int count, i;

        if (!SDL_RiftDK1_DecodeReport(data, (size_t)size, &report)) {
            continue;
        }
        // The fusion runs whether or not the joystick is open
        count = SDL_RiftDK1_HandleReport(&ctx->session, &report, events);
        if (joystick) {
            const Uint64 timestamp = SDL_GetTicksNS();

            for (i = 0; i < count; ++i) {
                const Uint64 sensor_timestamp = events[i].time_us * SDL_NS_PER_US;

                SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_ACCEL, sensor_timestamp, events[i].accel, 3);
                SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_GYRO, sensor_timestamp, events[i].gyro, 3);
            }
            for (i = 0; i < SDL_RIFTDK1_AXES; ++i) {
                SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, ctx->session.axes[i]);
            }
        }
    }

    if (size < 0) {
        // Read error, device is disconnected
        if (device->num_joysticks > 0) {
            HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        }
        return false;
    }

    HIDAPI_DriverRiftDK1_GetSink(ctx, &sink);
    SDL_RiftDK1_Update(&ctx->session, SDL_GetTicks(), &sink);
    return true;
}

static bool HIDAPI_DriverRiftDK1_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_AssertJoysticksLocked();

    joystick->naxes = SDL_RIFTDK1_AXES;
    // Samples are 1 ms apart
    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 1000.0f);
    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, 1000.0f);
    return true;
}

static bool HIDAPI_DriverRiftDK1_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    // The tracker streams while the keep-alive runs, and SDL passes on samples only for enabled sensors
    return true;
}

static int HIDAPI_DriverRiftDK1_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverRiftDK1_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverRiftDK1_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverRiftDK1_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverRiftDK1_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverRiftDK1_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverRiftDK1_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverRiftDK1_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverRiftDK1_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverRiftDK1 = {
    SDL_HINT_JOYSTICK_HIDAPI_RIFT_DK1,
    true,
    HIDAPI_DriverRiftDK1_RegisterHints,
    HIDAPI_DriverRiftDK1_UnregisterHints,
    HIDAPI_DriverRiftDK1_IsEnabled,
    HIDAPI_DriverRiftDK1_IsSupportedDevice,
    HIDAPI_DriverRiftDK1_InitDevice,
    HIDAPI_DriverRiftDK1_GetDevicePlayerIndex,
    HIDAPI_DriverRiftDK1_SetDevicePlayerIndex,
    HIDAPI_DriverRiftDK1_UpdateDevice,
    HIDAPI_DriverRiftDK1_OpenJoystick,
    HIDAPI_DriverRiftDK1_RumbleJoystick,
    HIDAPI_DriverRiftDK1_RumbleJoystickTriggers,
    HIDAPI_DriverRiftDK1_GetJoystickCapabilities,
    HIDAPI_DriverRiftDK1_SetJoystickLED,
    HIDAPI_DriverRiftDK1_SendJoystickEffect,
    HIDAPI_DriverRiftDK1_SetJoystickSensorsEnabled,
    HIDAPI_DriverRiftDK1_CloseJoystick,
    HIDAPI_DriverRiftDK1_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_RIFT_DK1

#endif // SDL_JOYSTICK_HIDAPI
