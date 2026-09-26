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
#include "SDL_hidapi_konami_p3io_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_KONAMI_P3IO

#include "../../misc/SDL_libusb.h"

// Define this if you want to log all packets from the board
// #define DEBUG_KONAMI_P3IO_PROTOCOL

/* Konami's P3IO of the DDR SuperNova 2 and DDR X cabinets, 1CCF:8008
 * (hifihedgehog/SDL#33 Part 14). The inputs come on interrupt IN 0x83,
 * which the libusb backend reads once WinUSB is bound. The board takes its
 * commands on bulk OUT 0x02 and answers on bulk IN 0x81, and this driver
 * runs those on the handle the libusb backend holds. A reply takes a
 * blocking read, and UpdateDevice runs on the thread that pumps events with
 * the joystick lock held, so the commands run on their own thread, as
 * OpenITG keeps them apart from its interrupt reads. The session and its
 * timers step there under the context lock. The two sides of the cabinet
 * become two joysticks once the board answers INIT and SET WATCHDOG, and
 * they leave when it stops answering. The protocol lives in
 * SDL_hidapi_konami_p3io_proto.c, where the offline tests run it. */

#define P3IO_WRITE_TIMEOUT_MS 100 /* A request is one short packet */
#define P3IO_READ_SLICE_MS    50  /* The longest a read blocks, and so the longest a stop waits */
#define P3IO_READ_LENGTH      64  /* As OpenITG reads, more than any reply the session asks for */
#define P3IO_MAX_ENDPOINTS    32

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_JoystickID joysticks[SDL_P3IO_PLAYERS]; /* 0 while not connected */
    bool connected;                             /* The joysticks belong to session number connected_ups */
    Uint32 connected_ups;
    bool gone;                                  /* A read failed, so the board is gone for this handle */
    bool post_pending[SDL_P3IO_PLAYERS];        /* Opened and not yet sent the state */
    SDL_P3IOInput input;
    SDL_P3IOState state;

    SDL_LibUSBContext *libusb;
    libusb_device_handle *handle;
    SDL_P3IOCommandEndpoints command;
    int claimed[2]; /* Interfaces this driver claimed beside the backend's */
    int num_claimed;

    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_Condition *wake;

    /* Under lock */
    SDL_P3IOSession session;
    bool stop;
} SDL_DriverKonamiP3IO_Context;

static void HIDAPI_DriverKonamiP3IO_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P3IO, callback, userdata);
}

static void HIDAPI_DriverKonamiP3IO_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P3IO, callback, userdata);
}

static bool HIDAPI_DriverKonamiP3IO_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P3IO, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverKonamiP3IO_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    /* Interface 0, the one the vendor rule enumerates. OpenITG also lists
       0000:5731 for a chimera build, which is not matched. */
    return vendor_id == USB_VENDOR_KONAMI && product_id == USB_PRODUCT_KONAMI_P3IO && interface_number == 0;
}

static int HIDAPI_DriverKonamiP3IO_Transfer(SDL_DriverKonamiP3IO_Context *ctx, Uint8 endpoint, bool interrupt,
                                            Uint8 *data, int length, int *transferred, unsigned int timeout)
{
    // libusb sets no count when a transfer fails to start
    *transferred = 0;
    if (interrupt) {
        return ctx->libusb->interrupt_transfer(ctx->handle, endpoint, data, length, transferred, timeout);
    }
    return ctx->libusb->bulk_transfer(ctx->handle, endpoint, data, length, transferred, timeout);
}

/* Sends each request the session asks for and reads its reply, until
   FreeDevice stops it. The lock is never held during a transfer. */
static int SDLCALL HIDAPI_DriverKonamiP3IO_CommandThread(void *data)
{
    SDL_DriverKonamiP3IO_Context *ctx = (SDL_DriverKonamiP3IO_Context *)data;
    Uint8 frame[SDL_P3IO_REQUEST_SIZE];
    Uint8 reply[P3IO_READ_LENGTH];

    SDL_LockMutex(ctx->lock);
    while (!ctx->stop) {
        const Uint64 now = SDL_GetTicks();
        const size_t length = SDL_P3IO_SessionPoll(&ctx->session, now, frame, sizeof(frame));
        int result, transferred;

        if (length > 0) {
            SDL_UnlockMutex(ctx->lock);
            result = HIDAPI_DriverKonamiP3IO_Transfer(ctx, SDL_P3IO_COMMAND_OUT, ctx->command.out_interrupt,
                                                      frame, (int)length, &transferred, P3IO_WRITE_TIMEOUT_MS);
            SDL_LockMutex(ctx->lock);
            if (result < 0 || transferred != (int)length) {
                SDL_P3IO_SessionFailed(&ctx->session, SDL_GetTicks());
            }
        } else if (SDL_P3IO_SessionAwaiting(&ctx->session)) {
            SDL_UnlockMutex(ctx->lock);
            result = HIDAPI_DriverKonamiP3IO_Transfer(ctx, SDL_P3IO_COMMAND_IN, ctx->command.in_interrupt,
                                                      reply, (int)sizeof(reply), &transferred, P3IO_READ_SLICE_MS);
            SDL_LockMutex(ctx->lock);
            // A read that timed out can still carry bytes
            if (transferred > 0) {
                SDL_P3IO_SessionReceive(&ctx->session, reply, (size_t)transferred, SDL_GetTicks());
            }
            if (result < 0 && result != LIBUSB_ERROR_TIMEOUT) {
                SDL_P3IO_SessionFailed(&ctx->session, SDL_GetTicks());
            }
        } else {
            // Nothing to send or read before the next deadline, unless FreeDevice wakes it
            const Uint64 deadline = SDL_P3IO_SessionDeadline(&ctx->session);
            const Uint64 wait = (deadline > now) ? (deadline - now) : 1;

            SDL_WaitConditionTimeout(ctx->wake, ctx->lock, (Sint32)SDL_min(wait, (Uint64)SDL_MAX_SINT32));
        }
    }
    SDL_UnlockMutex(ctx->lock);
    return 0;
}

/* Every endpoint of alternate setting 0 in the active configuration */
static bool HIDAPI_DriverKonamiP3IO_FindEndpoints(SDL_DriverKonamiP3IO_Context *ctx)
{
    struct libusb_config_descriptor *config = NULL;
    SDL_P3IOEndpoint endpoints[P3IO_MAX_ENDPOINTS];
    libusb_device *usb = ctx->libusb->get_device(ctx->handle);
    size_t count = 0;
    int i, j, k;

    if (ctx->libusb->get_active_config_descriptor(usb, &config) < 0) {
        config = NULL;
        ctx->libusb->get_config_descriptor(usb, 0, &config);
    }
    if (!config) {
        return false;
    }
    for (i = 0; i < config->bNumInterfaces; ++i) {
        const struct libusb_interface *intf = &config->interface[i];

        for (j = 0; j < intf->num_altsetting; ++j) {
            const struct libusb_interface_descriptor *alt = &intf->altsetting[j];

            if (alt->bAlternateSetting != 0) {
                continue;
            }
            for (k = 0; k < alt->bNumEndpoints && count < SDL_arraysize(endpoints); ++k) {
                endpoints[count].interface_number = alt->bInterfaceNumber;
                endpoints[count].address = alt->endpoint[k].bEndpointAddress;
                endpoints[count].attributes = alt->endpoint[k].bmAttributes;
                ++count;
            }
        }
    }
    ctx->libusb->free_config_descriptor(config);
    return SDL_P3IO_FindCommandEndpoints(endpoints, count, &ctx->command);
}

/* The libusb backend claimed the interface the vendor rule names, and that
   claim covers every endpoint of the interface. A command endpoint on
   another interface needs that interface claimed here, as the Switch 2
   driver claims its bulk interface. */
static bool HIDAPI_DriverKonamiP3IO_Claim(SDL_DriverKonamiP3IO_Context *ctx, Uint8 interface_number)
{
    int i;

    if ((int)interface_number == ctx->device->interface_number) {
        return true;
    }
    for (i = 0; i < ctx->num_claimed; ++i) {
        if (ctx->claimed[i] == (int)interface_number) {
            return true;
        }
    }
    ctx->libusb->set_auto_detach_kernel_driver(ctx->handle, 1);
    if (ctx->libusb->claim_interface(ctx->handle, interface_number) < 0) {
        return false;
    }
    ctx->claimed[ctx->num_claimed++] = interface_number;
    return true;
}

/* Each cabinet side is named for its player, P1 at player index 0 and P2
   at player index 1 */
static const char *HIDAPI_DriverKonamiP3IO_GetJoystickName(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    static const char *const names[SDL_P3IO_PLAYERS] = { "Konami P3IO DDR P1", "Konami P3IO DDR P2" };
    const SDL_DriverKonamiP3IO_Context *ctx = (const SDL_DriverKonamiP3IO_Context *)device->context;
    int player;

    for (player = 0; ctx && instance_id != 0 && player < SDL_P3IO_PLAYERS; ++player) {
        if (instance_id == ctx->joysticks[player]) {
            return names[player];
        }
    }
    return NULL;
}

static bool HIDAPI_DriverKonamiP3IO_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverKonamiP3IO_Context *ctx = (SDL_DriverKonamiP3IO_Context *)SDL_calloc(1, sizeof(*ctx));
    libusb_device_handle *handle;

    if (!ctx) {
        return false;
    }
    ctx->device = device;
    device->context = ctx;
    SDL_P3IO_InputInit(&ctx->input, &ctx->state);

    /* The device's name. Each joystick takes its side's name from
       HIDAPI_DriverKonamiP3IO_GetJoystickName. */
    HIDAPI_SetDeviceName(device, "Konami P3IO DDR");
    device->GetJoystickName = HIDAPI_DriverKonamiP3IO_GetJoystickName;
    device->joystick_type = SDL_JOYSTICK_TYPE_DANCE_PAD;

    /* The commands need the libusb handle, and a platform HID backend would
       hand over no reports from this board */
    handle = (libusb_device_handle *)SDL_GetPointerProperty(SDL_hid_get_properties(device->dev), SDL_PROP_HIDAPI_LIBUSB_DEVICE_HANDLE_POINTER, NULL);
    if (!handle) {
        return SDL_SetError("The P3IO is read through libusb");
    }
    if (!SDL_InitLibUSB(&ctx->libusb)) {
        ctx->libusb = NULL;
        return false;
    }
    ctx->handle = handle;
    if (!HIDAPI_DriverKonamiP3IO_FindEndpoints(ctx)) {
        return SDL_SetError("The P3IO has no command endpoints 0x02 and 0x81");
    }
    if (!HIDAPI_DriverKonamiP3IO_Claim(ctx, ctx->command.out_interface) ||
        !HIDAPI_DriverKonamiP3IO_Claim(ctx, ctx->command.in_interface)) {
        return SDL_SetError("Couldn't claim the P3IO command interface");
    }

    ctx->lock = SDL_CreateMutex();
    ctx->wake = SDL_CreateCondition();
    if (!ctx->lock || !ctx->wake) {
        return false;
    }
    SDL_P3IO_SessionInit(&ctx->session, SDL_GetTicks());
    ctx->thread = SDL_CreateThread(HIDAPI_DriverKonamiP3IO_CommandThread, "SDL P3IO commands", ctx);
    if (!ctx->thread) {
        return false;
    }

    /* The joysticks connect once the board answers INIT and SET WATCHDOG */
    return true;
}

/* The cabinet side decides: P1 is player index 0 and P2 player index 1 */
static int HIDAPI_DriverKonamiP3IO_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    SDL_DriverKonamiP3IO_Context *ctx = (SDL_DriverKonamiP3IO_Context *)device->context;
    int player;

    for (player = 0; player < SDL_P3IO_PLAYERS; ++player) {
        if (instance_id == ctx->joysticks[player]) {
            return player;
        }
    }
    return -1;
}

static void HIDAPI_DriverKonamiP3IO_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static void HIDAPI_DriverKonamiP3IO_Connect(SDL_DriverKonamiP3IO_Context *ctx, Uint32 ups)
{
    int player;

    for (player = 0; player < SDL_P3IO_PLAYERS; ++player) {
        ctx->post_pending[player] = false;
        HIDAPI_JoystickConnected(ctx->device, &ctx->joysticks[player]);
    }
    ctx->connected = true;
    ctx->connected_ups = ups;
}

static void HIDAPI_DriverKonamiP3IO_Disconnect(SDL_DriverKonamiP3IO_Context *ctx)
{
    int player;

    for (player = 0; player < SDL_P3IO_PLAYERS; ++player) {
        if (ctx->joysticks[player] != 0) {
            HIDAPI_JoystickDisconnected(ctx->device, ctx->joysticks[player]);
            ctx->joysticks[player] = 0;
        }
        ctx->post_pending[player] = false;
    }
    ctx->connected = false;
}

static void HIDAPI_DriverKonamiP3IO_Post(SDL_DriverKonamiP3IO_Context *ctx, int player, SDL_Joystick *joystick, Uint64 timestamp)
{
    const int nbuttons = (player == 0) ? SDL_P3IO_P1_BUTTONS : SDL_P3IO_P2_BUTTONS;
    int i;

    for (i = 0; i < nbuttons; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, (ctx->state.buttons[player] & (1u << i)) != 0);
    }
}

static bool HIDAPI_DriverKonamiP3IO_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverKonamiP3IO_Context *ctx = (SDL_DriverKonamiP3IO_Context *)device->context;
    Uint8 data[USB_PACKET_LENGTH];
    bool up, decoded = false;
    Uint32 ups;
    Uint64 timestamp;
    int size, player;

    /* Once a read fails, every later read fails too, and a session that is
       still up must not bring the joysticks back */
    if (ctx->gone) {
        return false;
    }

    SDL_LockMutex(ctx->lock);
    up = SDL_P3IO_SessionUp(&ctx->session);
    ups = ctx->session.ups;
    SDL_UnlockMutex(ctx->lock);

    /* A session that failed takes the joysticks with it, even one that is
       already up again */
    if (ctx->connected && (!up || ups != ctx->connected_ups)) {
        HIDAPI_DriverKonamiP3IO_Disconnect(ctx);
    }
    if (!ctx->connected && up) {
        HIDAPI_DriverKonamiP3IO_Connect(ctx, ups);
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
#ifdef DEBUG_KONAMI_P3IO_PROTOCOL
        HIDAPI_DumpPacket("Konami P3IO packet: size = %d", data, size);
#endif
        if (SDL_P3IO_InputFeed(&ctx->input, data, (size_t)size, &ctx->state)) {
            decoded = true;
        }
    }
    if (size < 0) {
        // The board went away
        ctx->gone = true;
        HIDAPI_DriverKonamiP3IO_Disconnect(ctx);
        return false;
    }

    timestamp = SDL_GetTicksNS();
    for (player = 0; player < SDL_P3IO_PLAYERS; ++player) {
        SDL_Joystick *joystick;

        if (ctx->joysticks[player] == 0 || (!decoded && !ctx->post_pending[player])) {
            continue;
        }
        joystick = SDL_GetJoystickFromID(ctx->joysticks[player]);
        if (joystick) {
            HIDAPI_DriverKonamiP3IO_Post(ctx, player, joystick, timestamp);
            ctx->post_pending[player] = false;
        }
    }
    return true;
}

static bool HIDAPI_DriverKonamiP3IO_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverKonamiP3IO_Context *ctx = (SDL_DriverKonamiP3IO_Context *)device->context;
    int player;

    SDL_AssertJoysticksLocked();

    for (player = 0; player < SDL_P3IO_PLAYERS; ++player) {
        if (joystick->instance_id == ctx->joysticks[player]) {
            joystick->nbuttons = (player == 0) ? SDL_P3IO_P1_BUTTONS : SDL_P3IO_P2_BUTTONS;
            // The buttons are allocated after this returns, so the next update sends the state
            ctx->post_pending[player] = true;
            return true;
        }
    }
    return false; // Should never get here!
}

static bool HIDAPI_DriverKonamiP3IO_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverKonamiP3IO_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverKonamiP3IO_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return 0;
}

static bool HIDAPI_DriverKonamiP3IO_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverKonamiP3IO_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverKonamiP3IO_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverKonamiP3IO_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverKonamiP3IO_FreeDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverKonamiP3IO_Context *ctx = (SDL_DriverKonamiP3IO_Context *)device->context;
    int i;

    if (!ctx) {
        return;
    }
    if (ctx->thread) {
        SDL_LockMutex(ctx->lock);
        ctx->stop = true;
        SDL_BroadcastCondition(ctx->wake);
        SDL_UnlockMutex(ctx->lock);
        SDL_WaitThread(ctx->thread, NULL);
        ctx->thread = NULL;
    }
    // The backend closes the handle after this returns
    for (i = 0; i < ctx->num_claimed; ++i) {
        ctx->libusb->release_interface(ctx->handle, ctx->claimed[i]);
    }
    ctx->num_claimed = 0;
    if (ctx->libusb) {
        SDL_QuitLibUSB();
        ctx->libusb = NULL;
        ctx->handle = NULL;
    }
    if (ctx->wake) {
        SDL_DestroyCondition(ctx->wake);
        ctx->wake = NULL;
    }
    if (ctx->lock) {
        SDL_DestroyMutex(ctx->lock);
        ctx->lock = NULL;
    }
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverKonamiP3IO = {
    SDL_HINT_JOYSTICK_HIDAPI_KONAMI_P3IO,
    true,
    HIDAPI_DriverKonamiP3IO_RegisterHints,
    HIDAPI_DriverKonamiP3IO_UnregisterHints,
    HIDAPI_DriverKonamiP3IO_IsEnabled,
    HIDAPI_DriverKonamiP3IO_IsSupportedDevice,
    HIDAPI_DriverKonamiP3IO_InitDevice,
    HIDAPI_DriverKonamiP3IO_GetDevicePlayerIndex,
    HIDAPI_DriverKonamiP3IO_SetDevicePlayerIndex,
    HIDAPI_DriverKonamiP3IO_UpdateDevice,
    HIDAPI_DriverKonamiP3IO_OpenJoystick,
    HIDAPI_DriverKonamiP3IO_RumbleJoystick,
    HIDAPI_DriverKonamiP3IO_RumbleJoystickTriggers,
    HIDAPI_DriverKonamiP3IO_GetJoystickCapabilities,
    HIDAPI_DriverKonamiP3IO_SetJoystickLED,
    HIDAPI_DriverKonamiP3IO_SendJoystickEffect,
    HIDAPI_DriverKonamiP3IO_SetJoystickSensorsEnabled,
    HIDAPI_DriverKonamiP3IO_CloseJoystick,
    HIDAPI_DriverKonamiP3IO_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_KONAMI_P3IO

#endif // SDL_JOYSTICK_HIDAPI
