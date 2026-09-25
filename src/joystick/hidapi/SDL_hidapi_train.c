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
#include "../SDL_train_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_TRAIN

#include "../../misc/SDL_libusb.h"

/* Train controllers on USB (hifihedgehog/SDL#33 Part 10): Taito's Type 2,
 * Shinkansen and Ryojohen controllers, the Multi Train Controller and the
 * Train Mascon. Each has one interface with one interrupt IN endpoint and
 * no HID class descriptor, read through libusb once WinUSB is bound. The
 * reports need no request, and the driver asks for no report descriptor.
 * Every output is a vendor control transfer on the handle the libusb
 * backend holds. The protocol lives in SDL_train_proto.c, where the
 * offline tests run it. */

SDL_COMPILE_TIME_ASSERT(train_hat_up, SDL_TRAIN_HAT_UP == SDL_HAT_UP);
SDL_COMPILE_TIME_ASSERT(train_hat_right, SDL_TRAIN_HAT_RIGHT == SDL_HAT_RIGHT);
SDL_COMPILE_TIME_ASSERT(train_hat_down, SDL_TRAIN_HAT_DOWN == SDL_HAT_DOWN);
SDL_COMPILE_TIME_ASSERT(train_hat_left, SDL_TRAIN_HAT_LEFT == SDL_HAT_LEFT);

typedef struct
{
    SDL_HIDAPI_Device *device;
    int model;
    SDL_TrainIdentity identity;
    SDL_TrainState state;
    SDL_TrainOutputs outputs;
    bool post_pending; /* The joystick opened and has not had the state yet */
    SDL_LibUSBContext *libusb;
    libusb_device_handle *handle;
} SDL_DriverTrain_Context;

static void HIDAPI_DriverTrain_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_TRAIN, callback, userdata);
}

static void HIDAPI_DriverTrain_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_TRAIN, callback, userdata);
}

static bool HIDAPI_DriverTrain_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_TRAIN, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverTrain_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return interface_number == 0 && SDL_Train_Identify(vendor_id, product_id, version) != SDL_TRAIN_NONE;
}

static bool HIDAPI_DriverTrain_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverTrain_Context *ctx = (SDL_DriverTrain_Context *)SDL_calloc(1, sizeof(*ctx));
    libusb_device_handle *handle;

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;
    ctx->model = SDL_Train_Identify(device->vendor_id, device->product_id, device->version);
    if (!SDL_Train_GetIdentity(ctx->model, &ctx->identity)) {
        return SDL_SetError("Not a train controller");
    }
    /* The outputs need the libusb handle, and a platform HID backend would
       hand over no reports from these devices */
    handle = (libusb_device_handle *)SDL_GetPointerProperty(SDL_hid_get_properties(device->dev), SDL_PROP_HIDAPI_LIBUSB_DEVICE_HANDLE_POINTER, NULL);
    if (!handle) {
        return SDL_SetError("Train controllers are read through libusb");
    }
    if (SDL_InitLibUSB(&ctx->libusb)) {
        ctx->handle = handle;
    } else {
        ctx->libusb = NULL;
    }
    SDL_Train_ResetState(ctx->model, &ctx->state);
    SDL_Train_InitOutputs(&ctx->outputs);

    HIDAPI_SetDeviceName(device, SDL_Train_Name(ctx->model));
    device->joystick_type = SDL_JOYSTICK_TYPE_GAMEPAD;
    return HIDAPI_JoystickConnected(device, NULL);
}

static bool HIDAPI_DriverTrain_Send(SDL_DriverTrain_Context *ctx, const SDL_TrainControl *control)
{
    Uint8 data[sizeof(control->data)];
    int result;

    if (!ctx->libusb || !ctx->handle) {
        return SDL_SetError("No libusb handle for the train controller");
    }
    SDL_memcpy(data, control->data, sizeof(data));
    result = ctx->libusb->control_transfer(ctx->handle, control->request_type, control->request, control->value, control->index,
                                           control->length ? data : NULL, control->length, 1000);
    if (result < 0 || result != control->length) {
        return SDL_SetError("Couldn't send the train controller's output");
    }
    return true;
}

static void HIDAPI_DriverTrain_Post(SDL_DriverTrain_Context *ctx, SDL_Joystick *joystick)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    int i;

    for (i = 0; i < ctx->identity.naxes; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, ctx->state.axes[i]);
    }
    for (i = 0; i < ctx->identity.nbuttons; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, (ctx->state.buttons & (1u << i)) != 0);
    }
    for (i = 0; i < ctx->identity.nhats; ++i) {
        SDL_SendJoystickHat(timestamp, joystick, (Uint8)i, ctx->state.hat);
    }
}

static bool HIDAPI_DriverTrain_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverTrain_Context *ctx = (SDL_DriverTrain_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }
    if (joystick && ctx->post_pending) {
        // The triggers and the reverser rest at -32768, which the joystick learns only from an event
        HIDAPI_DriverTrain_Post(ctx, joystick);
        ctx->post_pending = false;
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
        if (SDL_Train_Decode(ctx->model, data, (size_t)size, &ctx->state) && joystick) {
            HIDAPI_DriverTrain_Post(ctx, joystick);
        }
    }

    if (size < 0) {
        // Read error, device is disconnected
        if (device->num_joysticks > 0) {
            HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
        }
        return false;
    }
    return true;
}

static bool HIDAPI_DriverTrain_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverTrain_Context *ctx = (SDL_DriverTrain_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->naxes = ctx->identity.naxes;
    joystick->nbuttons = ctx->identity.nbuttons;
    joystick->nhats = ctx->identity.nhats;
    // The axes are allocated after this returns, so the next update sends the state
    ctx->post_pending = true;
    return true;
}

static int HIDAPI_DriverTrain_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverTrain_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static bool HIDAPI_DriverTrain_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    SDL_DriverTrain_Context *ctx = (SDL_DriverTrain_Context *)device->context;
    SDL_TrainControl controls[2];
    int count, i;

    count = SDL_Train_Rumble(ctx->model, &ctx->outputs, low_frequency_rumble, high_frequency_rumble, controls);
    if (count < 0) {
        return SDL_Unsupported();
    }
    for (i = 0; i < count; ++i) {
        if (!HIDAPI_DriverTrain_Send(ctx, &controls[i])) {
            return false;
        }
    }
    return true;
}

static bool HIDAPI_DriverTrain_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverTrain_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverTrain_Context *ctx = (SDL_DriverTrain_Context *)device->context;

    if (ctx->model == SDL_TRAIN_TYPE2 || ctx->model == SDL_TRAIN_SHINKANSEN) {
        return SDL_JOYSTICK_CAP_RUMBLE;
    }
    return 0;
}

static bool HIDAPI_DriverTrain_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

/* The raw output: 2 bytes on the Type 2, the 8-byte output payload on the
   Shinkansen, one lamp byte on the Multi Train Controller and the Train
   Mascon */
static bool HIDAPI_DriverTrain_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    SDL_DriverTrain_Context *ctx = (SDL_DriverTrain_Context *)device->context;
    SDL_TrainControl control;

    if (size <= 0 || SDL_Train_Effect(ctx->model, &ctx->outputs, (const Uint8 *)data, (size_t)size, &control) < 0) {
        return SDL_SetError("The train controller takes no effect of %d bytes", size);
    }
    return HIDAPI_DriverTrain_Send(ctx, &control);
}

static bool HIDAPI_DriverTrain_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverTrain_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverTrain_Context *ctx = (SDL_DriverTrain_Context *)device->context;
    SDL_TrainControl controls[3];
    int count, i;

    /* The motors off and the displays and lamps blank */
    count = SDL_Train_Close(ctx->model, &ctx->outputs, controls);
    for (i = 0; i < count; ++i) {
        HIDAPI_DriverTrain_Send(ctx, &controls[i]);
    }
}

static void HIDAPI_DriverTrain_FreeDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverTrain_Context *ctx = (SDL_DriverTrain_Context *)device->context;

    if (ctx && ctx->libusb) {
        SDL_QuitLibUSB();
        ctx->libusb = NULL;
        ctx->handle = NULL;
    }
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverTrain = {
    SDL_HINT_JOYSTICK_HIDAPI_TRAIN,
    true,
    HIDAPI_DriverTrain_RegisterHints,
    HIDAPI_DriverTrain_UnregisterHints,
    HIDAPI_DriverTrain_IsEnabled,
    HIDAPI_DriverTrain_IsSupportedDevice,
    HIDAPI_DriverTrain_InitDevice,
    HIDAPI_DriverTrain_GetDevicePlayerIndex,
    HIDAPI_DriverTrain_SetDevicePlayerIndex,
    HIDAPI_DriverTrain_UpdateDevice,
    HIDAPI_DriverTrain_OpenJoystick,
    HIDAPI_DriverTrain_RumbleJoystick,
    HIDAPI_DriverTrain_RumbleJoystickTriggers,
    HIDAPI_DriverTrain_GetJoystickCapabilities,
    HIDAPI_DriverTrain_SetJoystickLED,
    HIDAPI_DriverTrain_SendJoystickEffect,
    HIDAPI_DriverTrain_SetJoystickSensorsEnabled,
    HIDAPI_DriverTrain_CloseJoystick,
    HIDAPI_DriverTrain_FreeDevice,
};

#endif /* SDL_JOYSTICK_HIDAPI_TRAIN */

#endif /* SDL_JOYSTICK_HIDAPI */
