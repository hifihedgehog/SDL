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
#include "SDL_hidapi_konami_p4io_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_KONAMI_P4IO

#include "../../misc/SDL_libusb.h"

// Define this if you want to log all packets from the board
// #define DEBUG_KONAMI_P4IO_PROTOCOL

/* Konami's P4IO of jubeat and the DDR White cabinet, 1CCF:8010
 * (hifihedgehog/SDL#33 Part 14). Its input reports come on the interrupt IN
 * endpoint with no request, and the libusb backend reads them once WinUSB
 * is bound. The board takes commands on its bulk OUT endpoint and answers
 * on bulk IN, and this driver sends INIT and GET DEVICE INFO there once,
 * as bemanitools does, on the handle the libusb backend holds. A reply
 * takes a blocking read, and InitDevice and UpdateDevice run with the
 * joystick lock held, so the two commands run on their own thread, as the
 * I-Force queries do. Their outcome is only logged, since p4io-mdxfdrv
 * reads the inputs without them. No keep-alive is documented, so none is
 * sent. Only the cabinet's wiring gives the input bits a meaning, and
 * SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO_LAYOUT picks the layout. The protocol
 * lives in SDL_hidapi_konami_p4io_proto.c, where the offline tests run it. */

#define P4IO_TIMEOUT_MS 1000 /* Each command transfer, as long as an I-Force query */

typedef struct
{
    SDL_HIDAPI_Device *device;
    int layout;
    SDL_KonamiP4IOIdentity identity;
    SDL_KonamiP4IOState state;
    bool post_pending; /* The joystick opened and has not had the state yet */

    SDL_Thread *thread;
    SDL_AtomicInt stop;
    SDL_AtomicInt finished;
    SDL_LibUSBContext *libusb;
    libusb_device_handle *handle;
    SDL_KonamiP4IOEndpoints endpoints;

    /* Only the thread touches this until it has finished */
    SDL_KonamiP4IOStartup startup;
} SDL_DriverKonamiP4IO_Context;

static void HIDAPI_DriverKonamiP4IO_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO, callback, userdata);
}

static void HIDAPI_DriverKonamiP4IO_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO, callback, userdata);
}

static bool HIDAPI_DriverKonamiP4IO_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverKonamiP4IO_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    /* Interface 0, the one the vendor rule enumerates. No source records its class. */
    return vendor_id == USB_VENDOR_KONAMI && product_id == USB_PRODUCT_KONAMI_P4IO && interface_number == 0;
}

/* Sends INIT, then GET DEVICE INFO, each followed by one read of its reply */
static int SDLCALL HIDAPI_DriverKonamiP4IO_StartupThread(void *data)
{
    SDL_DriverKonamiP4IO_Context *ctx = (SDL_DriverKonamiP4IO_Context *)data;
    Uint8 request[SDL_KONAMI_P4IO_REQUEST_MAX];
    Uint8 reply[SDL_KONAMI_P4IO_READ_LENGTH];
    size_t length;

    while (!SDL_GetAtomicInt(&ctx->stop) &&
           (length = SDL_KonamiP4IO_StartupNext(&ctx->startup, request)) > 0) {
        int written = 0, received = 0, result;
        bool exchanged = false;

        result = ctx->libusb->bulk_transfer(ctx->handle, ctx->endpoints.bulk_out, request, (int)length, &written, P4IO_TIMEOUT_MS);
        if (result == 0 && written == (int)length && !SDL_GetAtomicInt(&ctx->stop)) {
            // A read that timed out can still hold bytes, which libusb keeps
            result = ctx->libusb->bulk_transfer(ctx->handle, ctx->endpoints.bulk_in, reply, (int)sizeof(reply), &received, P4IO_TIMEOUT_MS);
            exchanged = (result == 0 || (result == LIBUSB_ERROR_TIMEOUT && received > 0));
        }
#ifdef DEBUG_KONAMI_P4IO_PROTOCOL
        HIDAPI_DumpPacket("Konami P4IO request: size = %d", request, (int)length);
        if (exchanged) {
            HIDAPI_DumpPacket("Konami P4IO reply: size = %d", reply, received);
        }
#endif
        SDL_KonamiP4IO_StartupResult(&ctx->startup, exchanged, reply, exchanged ? (size_t)received : 0);
    }
    SDL_SetAtomicInt(&ctx->finished, 1);
    return 0;
}

/* Joins a start-up thread that has finished, or stops it and waits when
   wait is true, logs what the board said, and lets go of libusb */
static void HIDAPI_DriverKonamiP4IO_EndStartup(SDL_DriverKonamiP4IO_Context *ctx, bool wait)
{
    const SDL_KonamiP4IODeviceInfo *info = &ctx->startup.info;

    if (!ctx->thread) {
        return;
    }
    if (!wait && !SDL_GetAtomicInt(&ctx->finished)) {
        return;
    }
    SDL_SetAtomicInt(&ctx->stop, 1);
    SDL_WaitThread(ctx->thread, NULL);
    ctx->thread = NULL;
    if (ctx->startup.identified) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Konami P4IO: %s %d.%d.%d, built %s %s",
                     info->product, info->major, info->minor, info->revision, info->date, info->time);
    } else if (ctx->startup.step == SDL_KONAMI_P4IO_STARTUP_DONE) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Konami P4IO: no answer to GET DEVICE INFO, the inputs are read without it");
    }
    SDL_QuitLibUSB();
    ctx->libusb = NULL;
    ctx->handle = NULL;
}

/* The endpoints of alternate setting 0 of the interface the libusb backend
   opened, in descriptor order, from configuration index 0 as p4io-mdxfdrv
   reads it */
static bool HIDAPI_DriverKonamiP4IO_FindEndpoints(SDL_DriverKonamiP4IO_Context *ctx)
{
    struct libusb_config_descriptor *config = NULL;
    SDL_KonamiP4IOEndpoint endpoints[SDL_KONAMI_P4IO_ENDPOINTS];
    bool found = false;
    int i, j, k;

    if (ctx->libusb->get_config_descriptor(ctx->libusb->get_device(ctx->handle), 0, &config) != 0 || !config) {
        return false;
    }
    for (i = 0; i < config->bNumInterfaces && !found; ++i) {
        const struct libusb_interface *intf = &config->interface[i];

        for (j = 0; j < intf->num_altsetting && !found; ++j) {
            const struct libusb_interface_descriptor *alt = &intf->altsetting[j];

            if (alt->bInterfaceNumber != ctx->device->interface_number || alt->bAlternateSetting != 0) {
                continue;
            }
            // The module takes exactly three, so a fourth is never read
            for (k = 0; k < alt->bNumEndpoints && k < SDL_KONAMI_P4IO_ENDPOINTS; ++k) {
                endpoints[k].address = alt->endpoint[k].bEndpointAddress;
                endpoints[k].attributes = alt->endpoint[k].bmAttributes;
            }
            found = SDL_KonamiP4IO_FindEndpoints(endpoints, alt->bNumEndpoints, &ctx->endpoints);
        }
    }
    ctx->libusb->free_config_descriptor(config);
    return found;
}

static bool HIDAPI_DriverKonamiP4IO_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverKonamiP4IO_Context *ctx = (SDL_DriverKonamiP4IO_Context *)SDL_calloc(1, sizeof(*ctx));
    libusb_device_handle *handle;

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;
    ctx->layout = SDL_KonamiP4IO_ParseLayout(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO_LAYOUT));
    if (!SDL_KonamiP4IO_GetIdentity(ctx->layout, &ctx->identity)) {
        return SDL_SetError("Unknown Konami P4IO layout");
    }
    SDL_KonamiP4IO_ResetState(&ctx->state);
    SDL_KonamiP4IO_StartupInit(&ctx->startup);

    /* The commands need the libusb handle, and a platform HID backend would
       hand over no reports from this board */
    handle = (libusb_device_handle *)SDL_GetPointerProperty(SDL_hid_get_properties(device->dev), SDL_PROP_HIDAPI_LIBUSB_DEVICE_HANDLE_POINTER, NULL);
    if (!handle) {
        return SDL_SetError("The Konami P4IO is read through libusb");
    }

    HIDAPI_SetDeviceName(device, ctx->identity.name);
    device->joystick_type = SDL_JOYSTICK_TYPE_UNKNOWN;

    if (SDL_InitLibUSB(&ctx->libusb)) {
        ctx->handle = handle;
        if (HIDAPI_DriverKonamiP4IO_FindEndpoints(ctx)) {
            ctx->thread = SDL_CreateThread(HIDAPI_DriverKonamiP4IO_StartupThread, "SDL Konami P4IO start-up", ctx);
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Konami P4IO: the interface is not bulk OUT, bulk IN and interrupt IN, so no command is sent");
        }
        if (!ctx->thread) {
            SDL_QuitLibUSB();
            ctx->libusb = NULL;
            ctx->handle = NULL;
        }
    } else {
        ctx->libusb = NULL;
    }

    /* The inputs need no command, so the joystick is there from the start */
    return HIDAPI_JoystickConnected(device, NULL);
}

static int HIDAPI_DriverKonamiP4IO_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverKonamiP4IO_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static void HIDAPI_DriverKonamiP4IO_Post(SDL_DriverKonamiP4IO_Context *ctx, SDL_Joystick *joystick)
{
    const Uint64 timestamp = SDL_GetTicksNS();
    int i;

    for (i = 0; i < ctx->identity.nbuttons; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, (ctx->state.buttons & ((Uint32)1 << i)) != 0);
    }
}

static bool HIDAPI_DriverKonamiP4IO_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverKonamiP4IO_Context *ctx = (SDL_DriverKonamiP4IO_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size;

    HIDAPI_DriverKonamiP4IO_EndStartup(ctx, false);
    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    }
    if (joystick && ctx->post_pending) {
        HIDAPI_DriverKonamiP4IO_Post(ctx, joystick);
        ctx->post_pending = false;
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
#ifdef DEBUG_KONAMI_P4IO_PROTOCOL
        HIDAPI_DumpPacket("Konami P4IO packet: size = %d", data, size);
#endif
        if (SDL_KonamiP4IO_Decode(ctx->layout, data, (size_t)size, &ctx->state) && joystick) {
            HIDAPI_DriverKonamiP4IO_Post(ctx, joystick);
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

static bool HIDAPI_DriverKonamiP4IO_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverKonamiP4IO_Context *ctx = (SDL_DriverKonamiP4IO_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->nbuttons = ctx->identity.nbuttons;
    // The buttons are allocated after this returns, so the next update sends the state
    ctx->post_pending = true;
    return true;
}

static bool HIDAPI_DriverKonamiP4IO_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverKonamiP4IO_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverKonamiP4IO_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverKonamiP4IO_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverKonamiP4IO_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverKonamiP4IO_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverKonamiP4IO_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverKonamiP4IO_FreeDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverKonamiP4IO_Context *ctx = (SDL_DriverKonamiP4IO_Context *)device->context;

    if (ctx) {
        // The backend closes the handle after this returns, so the thread ends first
        HIDAPI_DriverKonamiP4IO_EndStartup(ctx, true);
    }
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverKonamiP4IO = {
    SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P4IO,
    true,
    HIDAPI_DriverKonamiP4IO_RegisterHints,
    HIDAPI_DriverKonamiP4IO_UnregisterHints,
    HIDAPI_DriverKonamiP4IO_IsEnabled,
    HIDAPI_DriverKonamiP4IO_IsSupportedDevice,
    HIDAPI_DriverKonamiP4IO_InitDevice,
    HIDAPI_DriverKonamiP4IO_GetDevicePlayerIndex,
    HIDAPI_DriverKonamiP4IO_SetDevicePlayerIndex,
    HIDAPI_DriverKonamiP4IO_UpdateDevice,
    HIDAPI_DriverKonamiP4IO_OpenJoystick,
    HIDAPI_DriverKonamiP4IO_RumbleJoystick,
    HIDAPI_DriverKonamiP4IO_RumbleJoystickTriggers,
    HIDAPI_DriverKonamiP4IO_GetJoystickCapabilities,
    HIDAPI_DriverKonamiP4IO_SetJoystickLED,
    HIDAPI_DriverKonamiP4IO_SendJoystickEffect,
    HIDAPI_DriverKonamiP4IO_SetJoystickSensorsEnabled,
    HIDAPI_DriverKonamiP4IO_CloseJoystick,
    HIDAPI_DriverKonamiP4IO_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_KONAMI_P4IO

#endif // SDL_JOYSTICK_HIDAPI
