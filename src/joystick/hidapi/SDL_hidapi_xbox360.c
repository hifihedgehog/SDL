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
#include "../../misc/SDL_libusb.h"
#include "../SDL_sysjoystick.h"
#include "SDL_hidapijoystick_c.h"
#include "SDL_hidapi_rumble.h"
#include "SDL_hidapi_xbox360.h"
#include "SDL_hidapi_xbox360acc_proto.h"

#ifdef SDL_JOYSTICK_HIDAPI_XBOX360

// Define this if you want to log all packets from the controller
// #define DEBUG_XBOX_PROTOCOL

#ifdef SDL_PLATFORM_MACOS
#include <IOKit/IOKitLib.h>
#endif

#ifdef HAVE_LIBUSB
/* The chatpad in the pad's expansion port (hifihedgehog/SDL#33 Part 15). It
 * reports on interface 2, which the driver claims on the handle the libusb
 * backend holds for interface 0, since WinUSB reaches a device's other
 * interfaces only through that handle. The start-up and keep-alives are
 * control transfers. The reports come from an interrupt transfer that
 * completes on the backend's event thread, where its callback submits it
 * again. The protocol lives in SDL_hidapi_xbox360acc_proto.c, where the
 * offline tests run it. */

#define SDL_XBOX360_CHATPAD_REPORTS 8

typedef struct SDL_Xbox360ChatpadReader
{
    SDL_Mutex *lock;
    SDL_LibUSBContext *libusb;
    struct libusb_transfer *transfer;
    SDL_AtomicInt finished; // 1 once the transfer is idle for good, set last
    bool stopping;          // Under the lock: the transfer is not submitted again
    int first;              // Under the lock: the oldest queued report
    int count;              // Under the lock
    int sizes[SDL_XBOX360_CHATPAD_REPORTS];
    Uint8 reports[SDL_XBOX360_CHATPAD_REPORTS][SDL_XBOX360ACC_CHATPAD_READ_MAX];
    Uint8 buffer[SDL_XBOX360ACC_CHATPAD_READ_MAX];
} SDL_Xbox360ChatpadReader;
#endif

typedef struct
{
    SDL_HIDAPI_Device *device;
    SDL_Joystick *joystick;
    int player_index;
    bool player_lights;
    SDL_xinput_capabilities capabilities;
    Uint8 last_state[USB_PACKET_LENGTH];
#ifdef SDL_PLATFORM_MACOS
    bool controlled_by_360controller;
    bool is_steam_virtual_gamepad;
#endif
    SDL_JoystickID pad_id;
#ifdef HAVE_LIBUSB
    bool chatpad_active; // Interface 2 is claimed and the start-up runs
    SDL_LibUSBContext *libusb;
    libusb_device_handle *handle;
    Uint8 chatpad_endpoint;
    Uint16 chatpad_packet_size;
    SDL_Xbox360ChatpadReader *reader;
    bool reads_started;
    SDL_Xbox360AccWired wired;
    SDL_JoystickID chatpad_id;
    bool chatpad_post_pending; // The joystick opened and has not had the state yet
    Uint64 chatpad_sent;       // The buttons the joystick has
#endif
} SDL_DriverXbox360_Context;

static bool IsChatpadJoystick(SDL_DriverXbox360_Context *ctx, SDL_Joystick *joystick)
{
#ifdef HAVE_LIBUSB
    return ctx->chatpad_id && joystick && joystick->instance_id == ctx->chatpad_id;
#else
    (void)ctx;
    (void)joystick;
    return false;
#endif
}

static void HIDAPI_DriverXbox360_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX, callback, userdata);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360, callback, userdata);
}

static void HIDAPI_DriverXbox360_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX, callback, userdata);
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360, callback, userdata);
}

static bool HIDAPI_DriverXbox360_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360,
                              SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT)));
}

#ifdef SDL_PLATFORM_MACOS
static bool IsControlledBy360ControllerDriverMacOS(SDL_HIDAPI_Device *device)
{
    bool controlled_by_360controller = false;
    if (device && device->path && SDL_strncmp("DevSrvsID:", device->path, 10) == 0) {
        uint64_t entry_id = SDL_strtoull(device->path + 10, NULL, 10);
        io_service_t service = IOServiceGetMatchingService(0, IORegistryEntryIDMatching(entry_id));
        if (service != MACH_PORT_NULL) {
            controlled_by_360controller = IOObjectConformsTo(service, "Xbox360ControllerClass");
            IOObjectRelease(service);
        }
    }
    return controlled_by_360controller;
}
#endif

#ifdef HAVE_LIBUSB
static void FetchXInputCapabilities(SDL_HIDAPI_Device *device)
{
    SDL_DriverXbox360_Context *ctx = (SDL_DriverXbox360_Context *)device->context;
    SDL_LibUSBContext *libusb_ctx;
    if (SDL_InitLibUSB(&libusb_ctx)) {
        libusb_device_handle *handle = (libusb_device_handle *)SDL_GetPointerProperty(SDL_hid_get_properties(device->dev), SDL_PROP_HIDAPI_LIBUSB_DEVICE_HANDLE_POINTER, NULL);
        if (handle == NULL) {
            SDL_QuitLibUSB();
            return;
        }
        libusb_device *dev = libusb_ctx->get_device(handle);
        if (dev == NULL) {
            SDL_QuitLibUSB();
            return;
        }
        struct libusb_config_descriptor *conf_desc = NULL;
        const struct libusb_interface_descriptor *intf_desc;
        libusb_ctx->get_active_config_descriptor(dev, &conf_desc);
        if (conf_desc == NULL || conf_desc->bNumInterfaces < device->interface_number) {
            SDL_QuitLibUSB();
            return;
        }
        const struct libusb_interface *intf = &conf_desc->interface[device->interface_number];
        intf_desc = &intf->altsetting[0];
        if (intf_desc->extra_length == 17 && intf_desc->extra[1] == 0x21) {
            ctx->capabilities.type = intf_desc->extra[3];
            ctx->capabilities.subType = intf_desc->extra[4];
            switch (ctx->capabilities.subType) {
                case 0x01: // XINPUT_DEVSUBTYPE_GAMEPAD
                    device->joystick_type = SDL_JOYSTICK_TYPE_GAMEPAD;
                    break;
                case 0x02: // XINPUT_DEVSUBTYPE_WHEEL
                    device->joystick_type = SDL_JOYSTICK_TYPE_WHEEL;
                    break;
                case 0x03: // XINPUT_DEVSUBTYPE_ARCADE_STICK
                    device->joystick_type = SDL_JOYSTICK_TYPE_ARCADE_STICK;
                    break;
                case 0x04: // XINPUT_DEVSUBTYPE_FLIGHT_STICK
                    device->joystick_type = SDL_JOYSTICK_TYPE_FLIGHT_STICK;
                    break;
                case 0x05: // XINPUT_DEVSUBTYPE_DANCE_PAD
                    device->joystick_type = SDL_JOYSTICK_TYPE_DANCE_PAD;
                    break;
                case 0x06: // XINPUT_DEVSUBTYPE_GUITAR
                case 0x07: // XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE
                case 0x0B: // XINPUT_DEVSUBTYPE_GUITAR_BASS
                    device->joystick_type = SDL_JOYSTICK_TYPE_GUITAR;
                    break;
                case 0x08: // XINPUT_DEVSUBTYPE_DRUM_KIT
                    device->joystick_type = SDL_JOYSTICK_TYPE_DRUM_KIT;
                    break;
                case 0x13: // XINPUT_DEVSUBTYPE_ARCADE_PAD
                    device->joystick_type = SDL_JOYSTICK_TYPE_ARCADE_PAD;
                    break;
                default:
                    break;
            }
            device->guid.data[15] = ctx->capabilities.subType;
            unsigned char buf[20];
            int ret = libusb_ctx->control_transfer(handle, 0xC1, 0x01, 0x100, 0x0, buf, sizeof(buf), 100);
            if (ret == sizeof(buf)) {
                ctx->capabilities.flags = LOAD16(buf[18], buf[19]);
                ctx->capabilities.gamepad.wButtons = LOAD16(buf[2], buf[3]);
                ctx->capabilities.gamepad.bLeftTrigger = buf[4];
                ctx->capabilities.gamepad.bRightTrigger = buf[5];
                ctx->capabilities.gamepad.sThumbLX = LOAD16(buf[6], buf[7]);
                ctx->capabilities.gamepad.sThumbLY = LOAD16(buf[8], buf[9]);
                ctx->capabilities.gamepad.sThumbRX = LOAD16(buf[10], buf[11]);
                ctx->capabilities.gamepad.sThumbRY = LOAD16(buf[12], buf[13]);
            }
            ret = libusb_ctx->control_transfer(handle, 0xC1, 0x01, 0x00, 0x0, buf, 8, 100);
            if (ret == 8) {
                ctx->capabilities.vibration.wLeftMotorSpeed = buf[3] << 8;
                ctx->capabilities.vibration.wRightMotorSpeed = buf[4] << 8;
            }
#ifdef DEBUG_XBOX_PROTOCOL
            SDL_Log("Xbox 360 capabilities:");
            SDL_Log("   type: %02x", ctx->capabilities.type);
            SDL_Log("   subType: %02x", ctx->capabilities.subType);
            SDL_Log("   flags: %04x", ctx->capabilities.flags);
            SDL_Log("   wButtons: %02x", ctx->capabilities.gamepad.wButtons);
            SDL_Log("   bLeftTrigger: %02x", ctx->capabilities.gamepad.bLeftTrigger);
            SDL_Log("   bRightTrigger: %02x", ctx->capabilities.gamepad.bRightTrigger);
            SDL_Log("   sThumbLX: %02x", ctx->capabilities.gamepad.sThumbLX);
            SDL_Log("   sThumbLY: %02x", ctx->capabilities.gamepad.sThumbLY);
            SDL_Log("   sThumbRX: %02x", ctx->capabilities.gamepad.sThumbRX);
            SDL_Log("   sThumbRY: %02x", ctx->capabilities.gamepad.sThumbRY);
            SDL_Log("   wLeftMotorSpeed: %02x", ctx->capabilities.vibration.wLeftMotorSpeed);
            SDL_Log("   wRightMotorSpeed: %02x", ctx->capabilities.vibration.wRightMotorSpeed);
#endif
        }
        SDL_QuitLibUSB();
    }
}

// Runs on the libusb backend's event thread
static void LIBUSB_CALL HIDAPI_DriverXbox360_ChatpadRead(struct libusb_transfer *transfer)
{
    SDL_Xbox360ChatpadReader *reader = (SDL_Xbox360ChatpadReader *)transfer->user_data;
    bool finished;

    SDL_LockMutex(reader->lock);
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED && transfer->actual_length > 0) {
        int slot;

        if (reader->count == SDL_XBOX360_CHATPAD_REPORTS) {
            // A full queue drops its oldest report
            reader->first = (reader->first + 1) % SDL_XBOX360_CHATPAD_REPORTS;
            --reader->count;
        }
        slot = (reader->first + reader->count) % SDL_XBOX360_CHATPAD_REPORTS;
        reader->sizes[slot] = SDL_min(transfer->actual_length, (int)sizeof(reader->reports[slot]));
        SDL_memcpy(reader->reports[slot], transfer->buffer, reader->sizes[slot]);
        ++reader->count;
    }
    // Only a completed read goes out again, as in xboxdrv. A cancel, a stall
    // or a lost device ends the reads.
    finished = (reader->stopping || transfer->status != LIBUSB_TRANSFER_COMPLETED ||
                reader->libusb->submit_transfer(transfer) < 0);
    SDL_UnlockMutex(reader->lock);

    // The last access to the reader, which may be freed once this is set
    if (finished) {
        SDL_SetAtomicInt(&reader->finished, 1);
    }
}

static void HIDAPI_DriverXbox360_StartChatpadReads(SDL_DriverXbox360_Context *ctx)
{
    SDL_Xbox360ChatpadReader *reader = ctx->reader;

    reader->transfer = ctx->libusb->alloc_transfer(0);
    if (!reader->transfer) {
        return;
    }
    // No timeout: the chatpad reports only when a key or its status changes
    libusb_fill_interrupt_transfer(reader->transfer, ctx->handle, ctx->chatpad_endpoint, reader->buffer,
                                   ctx->chatpad_packet_size, HIDAPI_DriverXbox360_ChatpadRead, reader, 0);
    SDL_SetAtomicInt(&reader->finished, 0);
    if (ctx->libusb->submit_transfer(reader->transfer) < 0) {
        SDL_SetAtomicInt(&reader->finished, 1);
    }
}

static bool HIDAPI_DriverXbox360_TakeChatpadReport(SDL_Xbox360ChatpadReader *reader, Uint8 *report, int *size)
{
    bool taken = false;

    SDL_LockMutex(reader->lock);
    if (reader->count > 0) {
        *size = reader->sizes[reader->first];
        SDL_memcpy(report, reader->reports[reader->first], *size);
        reader->first = (reader->first + 1) % SDL_XBOX360_CHATPAD_REPORTS;
        --reader->count;
        taken = true;
    }
    SDL_UnlockMutex(reader->lock);
    return taken;
}

static void HIDAPI_DriverXbox360_PostChatpad(SDL_Joystick *joystick, Uint64 buttons)
{
    Uint64 timestamp = SDL_GetTicksNS();
    int i;

    for (i = 0; i < SDL_XBOX360ACC_CHATPAD_BUTTONS; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, ((buttons >> i) & 1) != 0);
    }
}

// The chatpad joystick's own name, which the core asks for by instance
static const char *HIDAPI_DriverXbox360_GetJoystickName(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    const SDL_DriverXbox360_Context *ctx = (const SDL_DriverXbox360_Context *)device->context;

    if (ctx && ctx->chatpad_id && instance_id == ctx->chatpad_id) {
        return SDL_XBOX360ACC_CHATPAD_NAME;
    }
    return NULL;
}

// The chatpad joystick's GUID: the pad's, with the CRC of the chatpad's name
// and the byte 15 that keeps it off the gamepad mappings
static bool HIDAPI_DriverXbox360_GetJoystickGUID(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, SDL_GUID *guid)
{
    const SDL_DriverXbox360_Context *ctx = (const SDL_DriverXbox360_Context *)device->context;

    if (!ctx || !ctx->chatpad_id || instance_id != ctx->chatpad_id) {
        return false;
    }
    *guid = device->guid;
    SDL_SetJoystickGUIDCRC(guid, SDL_crc16(0, SDL_XBOX360ACC_CHATPAD_NAME, SDL_strlen(SDL_XBOX360ACC_CHATPAD_NAME)));
    guid->data[15] = SDL_XBOX360ACC_GUID_CHATPAD;
    return true;
}

// Connects and disconnects the chatpad joystick and sends it what changed
static void HIDAPI_DriverXbox360_SyncChatpad(SDL_HIDAPI_Device *device, SDL_DriverXbox360_Context *ctx)
{
    const SDL_Xbox360AccChatpad *chatpad = &ctx->wired.chatpad;

    if (chatpad->present && !ctx->chatpad_id) {
        ctx->chatpad_sent = 0;
        // The ID is stored before the joystick is announced, so the name and
        // GUID callbacks know it from the first event
        HIDAPI_JoystickConnected(device, &ctx->chatpad_id);
    } else if (!chatpad->present && ctx->chatpad_id) {
        HIDAPI_JoystickDisconnected(device, ctx->chatpad_id);
        ctx->chatpad_id = 0;
    }

    if (ctx->chatpad_id && (ctx->chatpad_post_pending || chatpad->buttons != ctx->chatpad_sent)) {
        SDL_Joystick *joystick = SDL_GetJoystickFromID(ctx->chatpad_id);

        if (joystick) {
            HIDAPI_DriverXbox360_PostChatpad(joystick, chatpad->buttons);
            ctx->chatpad_sent = chatpad->buttons;
            ctx->chatpad_post_pending = false;
        }
    }
}

static void HIDAPI_DriverXbox360_UpdateChatpad(SDL_HIDAPI_Device *device, SDL_DriverXbox360_Context *ctx)
{
    SDL_Xbox360AccControl control;
    Uint8 report[SDL_XBOX360ACC_CHATPAD_READ_MAX];
    int size = 0;
    int sent = 0;

    while (HIDAPI_DriverXbox360_TakeChatpadReport(ctx->reader, report, &size)) {
        SDL_Xbox360Acc_WiredChatpadReport(&ctx->wired, report, (size_t)size);
    }

    // The transfers that are due, each after the one before completes. A
    // stall does not stop the start-up, and the next SETUP clears it.
    while (sent < 16 && SDL_Xbox360Acc_WiredNext(&ctx->wired, SDL_GetTicks(), &control)) {
        Uint8 data[sizeof(control.data)];
        int result;

        SDL_memcpy(data, control.data, sizeof(data));
        result = ctx->libusb->control_transfer(ctx->handle, control.request_type, control.request, control.value, control.index,
                                               control.length ? data : NULL, control.length, 100);
        SDL_Xbox360Acc_WiredDone(&ctx->wired, SDL_GetTicks(), result >= 0);
        ++sent;
    }

    // The reports start once step 9, the 1B, has completed
    if (!ctx->reads_started && SDL_Xbox360Acc_WiredReading(&ctx->wired)) {
        ctx->reads_started = true;
        HIDAPI_DriverXbox360_StartChatpadReads(ctx);
    }

    HIDAPI_DriverXbox360_SyncChatpad(device, ctx);
}

// Claims the chatpad interface and starts the start-up, when the pad is one
// whose chatpad the sources serve and libusb holds it
static void HIDAPI_DriverXbox360_InitChatpad(SDL_HIDAPI_Device *device)
{
    SDL_DriverXbox360_Context *ctx = (SDL_DriverXbox360_Context *)device->context;
    SDL_Xbox360AccEndpoint endpoints[4];
    struct libusb_config_descriptor *config = NULL;
    libusb_device_handle *handle;
    Uint8 address = 0;
    Uint16 packet_size = 0;
    int i, j, count = 0;

    if (!SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_CHATPAD, HIDAPI_DriverXbox360_IsEnabled())) {
        return;
    }
    if (!SDL_Xbox360Acc_WiredSupported(device->vendor_id, device->product_id, device->version)) {
        return;
    }
    handle = (libusb_device_handle *)SDL_GetPointerProperty(SDL_hid_get_properties(device->dev), SDL_PROP_HIDAPI_LIBUSB_DEVICE_HANDLE_POINTER, NULL);
    if (!handle || !SDL_InitLibUSB(&ctx->libusb)) {
        return;
    }

    // The endpoint comes from the interface 2 descriptor, whatever its address
    if (ctx->libusb->get_active_config_descriptor(ctx->libusb->get_device(handle), &config) == 0 && config) {
        for (i = 0; i < config->bNumInterfaces && !address; ++i) {
            const struct libusb_interface *intf = &config->interface[i];

            for (j = 0; j < intf->num_altsetting; ++j) {
                const struct libusb_interface_descriptor *alt = &intf->altsetting[j];

                if (alt->bAlternateSetting != 0 ||
                    !SDL_Xbox360Acc_IsChatpadInterface(alt->bInterfaceNumber, alt->bInterfaceClass, alt->bInterfaceSubClass, alt->bInterfaceProtocol)) {
                    continue;
                }
                for (count = 0; count < alt->bNumEndpoints && count < (int)SDL_arraysize(endpoints); ++count) {
                    endpoints[count].address = alt->endpoint[count].bEndpointAddress;
                    endpoints[count].attributes = alt->endpoint[count].bmAttributes;
                    endpoints[count].max_packet_size = alt->endpoint[count].wMaxPacketSize;
                }
                address = SDL_Xbox360Acc_ChatpadEndpoint(endpoints, count, &packet_size);
                break;
            }
        }
        ctx->libusb->free_config_descriptor(config);
    }
    if (!address) {
        SDL_QuitLibUSB();
        ctx->libusb = NULL;
        return;
    }

    ctx->reader = (SDL_Xbox360ChatpadReader *)SDL_calloc(1, sizeof(*ctx->reader));
    if (ctx->reader) {
        ctx->reader->lock = SDL_CreateMutex();
    }
    if (!ctx->reader || !ctx->reader->lock) {
        SDL_free(ctx->reader);
        ctx->reader = NULL;
        SDL_QuitLibUSB();
        ctx->libusb = NULL;
        return;
    }
    ctx->reader->libusb = ctx->libusb;
    SDL_SetAtomicInt(&ctx->reader->finished, 1);

    ctx->libusb->set_auto_detach_kernel_driver(handle, true);
    if (ctx->libusb->claim_interface(handle, 2) < 0) {
        SDL_DestroyMutex(ctx->reader->lock);
        SDL_free(ctx->reader);
        ctx->reader = NULL;
        SDL_QuitLibUSB();
        ctx->libusb = NULL;
        return;
    }
    ctx->handle = handle;
    ctx->chatpad_endpoint = address;
    ctx->chatpad_packet_size = packet_size;
    SDL_Xbox360Acc_WiredStart(&ctx->wired, device->version, SDL_GetTicks());
    ctx->chatpad_active = true;
    device->GetJoystickName = HIDAPI_DriverXbox360_GetJoystickName;
    device->GetJoystickGUID = HIDAPI_DriverXbox360_GetJoystickGUID;
}

static void HIDAPI_DriverXbox360_FreeChatpad(SDL_DriverXbox360_Context *ctx)
{
    SDL_Xbox360ChatpadReader *reader = ctx->reader;
    Uint64 deadline;
    bool finished;

    if (!ctx->chatpad_active) {
        return;
    }
    ctx->chatpad_active = false;

    // The event thread completes the canceled transfer
    SDL_LockMutex(reader->lock);
    reader->stopping = true;
    if (!SDL_GetAtomicInt(&reader->finished)) {
        ctx->libusb->cancel_transfer(reader->transfer);
    }
    SDL_UnlockMutex(reader->lock);
    deadline = SDL_GetTicks() + 1000;
    for (;;) {
        finished = (SDL_GetAtomicInt(&reader->finished) != 0);
        if (finished || SDL_GetTicks() >= deadline) {
            break;
        }
        SDL_Delay(1);
    }

    ctx->libusb->release_interface(ctx->handle, 2);
    if (finished) {
        if (reader->transfer) {
            ctx->libusb->free_transfer(reader->transfer);
        }
        SDL_DestroyMutex(reader->lock);
        SDL_free(reader);
    } else {
        // A transfer still in flight keeps its reader, which it may yet complete into
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Xbox 360 chatpad: read still pending after cancel");
    }
    ctx->reader = NULL;
    ctx->handle = NULL;
    SDL_QuitLibUSB();
    ctx->libusb = NULL;
}
#endif

static bool HIDAPI_DriverXbox360_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    const int XB360W_IFACE_PROTOCOL = 129; // Wireless

    if (vendor_id == USB_VENDOR_ASTRO && product_id == USB_PRODUCT_ASTRO_C40_XBOX360) {
        // This is the ASTRO C40 in Xbox 360 mode
        return true;
    }
    if (vendor_id == USB_VENDOR_NVIDIA) {
        // This is the NVIDIA Shield controller which doesn't talk Xbox controller protocol
        return false;
    }
    if ((vendor_id == USB_VENDOR_MICROSOFT && (product_id == USB_PRODUCT_XBOX360_WIRELESS_RECEIVER_THIRDPARTY2 || product_id == USB_PRODUCT_XBOX360_WIRELESS_RECEIVER)) ||
        (type == SDL_GAMEPAD_TYPE_XBOX360 && interface_protocol == XB360W_IFACE_PROTOCOL)) {
        // This is the wireless dongle, which talks a different protocol
        return false;
    }
    if ((vendor_id == USB_VENDOR_MICROSOFT) && (product_id == USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER)) {
        // This is the BigButton wireless receiver, which talks a different protocol
        return false;
    }
    if (interface_number > 0) {
        // This is the chatpad or other input interface, not the Xbox 360 interface
        return false;
    }
#ifdef SDL_PLATFORM_MACOS
    if (IsControlledBy360ControllerDriverMacOS(device)) {
        // Wired Xbox controllers are handled by this driver, when they are
        // controlled by the 360Controller driver available from:
        // https://github.com/360Controller/360Controller/releases
        return true;
    }
#endif
#if defined(SDL_PLATFORM_MACOS) && defined(SDL_JOYSTICK_MFI)
    if (SDL_GetHintBoolean(SDL_HINT_JOYSTICK_MFI, true)) {
        if (SDL_IsJoystickSteamVirtualGamepad(vendor_id, product_id, version)) {
            // GCController support doesn't work with the Steam Virtual Gamepad
            return true;
        }
        if (device && SDL_strncmp(device->path, "DevSrvsID", 9) == 0) {
            // On macOS when it isn't controlled by the 360Controller driver and
            // it doesn't look like a Steam virtual gamepad and it's not
            // available via libusb we should rely on GCController support.
            return false;
        }
    }
#endif
    return (type == SDL_GAMEPAD_TYPE_XBOX360);
}

static bool SetSlotLED(SDL_hid_device *dev, Uint8 slot, bool on)
{
    const bool blink = false;
    Uint8 mode = on ? ((blink ? 0x02 : 0x06) + slot) : 0;
    Uint8 led_packet[] = { 0x01, 0x03, 0x00 };

    led_packet[2] = mode;
    if (SDL_hid_write(dev, led_packet, sizeof(led_packet)) != sizeof(led_packet)) {
        return false;
    }
    return true;
}

static void UpdateSlotLED(SDL_DriverXbox360_Context *ctx)
{
    if (ctx->player_lights && ctx->player_index >= 0) {
        SetSlotLED(ctx->device->dev, (ctx->player_index % 4), true);
    } else {
        SetSlotLED(ctx->device->dev, 0, false);
    }
}

static void SDLCALL SDL_PlayerLEDHintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    SDL_DriverXbox360_Context *ctx = (SDL_DriverXbox360_Context *)userdata;
    bool player_lights = SDL_GetStringBoolean(hint, true);

    if (player_lights != ctx->player_lights) {
        ctx->player_lights = player_lights;

        UpdateSlotLED(ctx);
        HIDAPI_UpdateDeviceProperties(ctx->device);
    }
}

static bool HIDAPI_DriverXbox360_InitDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverXbox360_Context *ctx;

    ctx = (SDL_DriverXbox360_Context *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return false;
    }
    ctx->device = device;
#ifdef SDL_PLATFORM_MACOS
    ctx->controlled_by_360controller = IsControlledBy360ControllerDriverMacOS(device);
    ctx->is_steam_virtual_gamepad = SDL_IsJoystickSteamVirtualGamepad(device->vendor_id, device->product_id, device->version);
#endif

    device->context = ctx;

    device->type = SDL_GAMEPAD_TYPE_XBOX360;

    if (SDL_IsJoystickSteamVirtualGamepad(device->vendor_id, device->product_id, device->version) &&
        device->product_string && SDL_strncmp(device->product_string, "GamePad-", 8) == 0) {
        int slot = 0;
        SDL_sscanf(device->product_string, "GamePad-%d", &slot);
        device->steam_virtual_gamepad_slot = (slot - 1);
    }

#ifdef HAVE_LIBUSB
    HIDAPI_DriverXbox360_InitChatpad(device);
#endif

    return HIDAPI_JoystickConnected(device, &ctx->pad_id);
}

static int HIDAPI_DriverXbox360_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverXbox360_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
    SDL_DriverXbox360_Context *ctx = (SDL_DriverXbox360_Context *)device->context;

    if (!ctx->joystick || instance_id != ctx->pad_id) {
        return;
    }

    ctx->player_index = player_index;

    UpdateSlotLED(ctx);
}

static bool HIDAPI_DriverXbox360_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverXbox360_Context *ctx = (SDL_DriverXbox360_Context *)device->context;

    SDL_AssertJoysticksLocked();

    if (IsChatpadJoystick(ctx, joystick)) {
        joystick->nbuttons = SDL_XBOX360ACC_CHATPAD_BUTTONS;
        joystick->naxes = 0;
        joystick->nhats = 0;
#ifdef HAVE_LIBUSB
        // The buttons are allocated after this returns, so the next update sends them
        ctx->chatpad_post_pending = true;
#endif
        return true;
    }
    if (joystick->instance_id != ctx->pad_id) {
        // A chatpad that the update before this open removed
        return false;
    }

    ctx->joystick = joystick;
    SDL_zeroa(ctx->last_state);

    // Initialize player index (needed for setting LEDs)
    ctx->player_index = SDL_GetJoystickPlayerIndex(joystick);
    ctx->player_lights = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_PLAYER_LED, true);
    UpdateSlotLED(ctx);

    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_PLAYER_LED,
                        SDL_PlayerLEDHintChanged, ctx);

    // Initialize the joystick capabilities
    joystick->nbuttons = 11;
    joystick->naxes = SDL_GAMEPAD_AXIS_COUNT;
    joystick->nhats = 1;
#ifdef HAVE_LIBUSB
    FetchXInputCapabilities(device);
#endif
    return true;
}

static bool HIDAPI_DriverXbox360_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    if (IsChatpadJoystick((SDL_DriverXbox360_Context *)device->context, joystick)) {
        return SDL_Unsupported();
    }
#ifdef SDL_PLATFORM_MACOS
    if (((SDL_DriverXbox360_Context *)device->context)->controlled_by_360controller) {
        // On macOS the 360Controller driver uses this short report,
        // and we need to prefix it with a magic token so hidapi passes it through untouched
        Uint8 rumble_packet[] = { 'M', 'A', 'G', 'I', 'C', '0', 0x00, 0x04, 0x00, 0x00 };
        rumble_packet[6 + 2] = (low_frequency_rumble >> 8);
        rumble_packet[6 + 3] = (high_frequency_rumble >> 8);
        if (SDL_HIDAPI_SendRumble(device, rumble_packet, sizeof(rumble_packet)) != sizeof(rumble_packet)) {
            return SDL_SetError("Couldn't send rumble packet");
        }
        return true;
    }
#endif
    Uint8 rumble_packet[] = { 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    rumble_packet[3] = (low_frequency_rumble >> 8);
    rumble_packet[4] = (high_frequency_rumble >> 8);

    if (SDL_HIDAPI_SendRumble(device, rumble_packet, sizeof(rumble_packet)) != sizeof(rumble_packet)) {
        return SDL_SetError("Couldn't send rumble packet");
    }
    return true;
}

static bool HIDAPI_DriverXbox360_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverXbox360_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverXbox360_Context *ctx = (SDL_DriverXbox360_Context *)device->context;
    Uint32 result = SDL_JOYSTICK_CAP_RUMBLE;

    if (IsChatpadJoystick(ctx, joystick)) {
        return 0;
    }
    if (ctx->player_lights) {
        result |= SDL_JOYSTICK_CAP_PLAYER_LED;
    }
    return result;
}

static bool HIDAPI_DriverXbox360_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

/* The chatpad's lamps: one byte, 00 to 03 to put out Shift, Green, Orange
   and People, 08 to 0B to light them, 04 to put out the backlight and 0C to
   light it */
static bool HIDAPI_DriverXbox360_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
#ifdef HAVE_LIBUSB
    SDL_DriverXbox360_Context *ctx = (SDL_DriverXbox360_Context *)device->context;
    SDL_Xbox360AccControl control;

    if (IsChatpadJoystick(ctx, joystick)) {
        if (size <= 0 || !SDL_Xbox360Acc_WiredLamp((const Uint8 *)data, (size_t)size, &control)) {
            return SDL_SetError("The Xbox 360 chatpad takes one lamp byte, 00 to 04 or 08 to 0C");
        }
        if (ctx->libusb->control_transfer(ctx->handle, control.request_type, control.request, control.value, control.index, NULL, 0, 100) < 0) {
            return SDL_SetError("Couldn't set the Xbox 360 chatpad lamp");
        }
        return true;
    }
#endif
    return SDL_Unsupported();
}

static bool HIDAPI_DriverXbox360_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverXbox360_HandleStatePacket(SDL_Joystick *joystick, SDL_DriverXbox360_Context *ctx, Uint8 *data, int size)
{
    Sint16 axis;
#ifdef SDL_PLATFORM_MACOS
    // For backwards compatibility reasons, the 360Controller driver and the Steam Virtual
    // Gamepad require opposite Y axis inversion on macOS
    const bool invert_y_axes = (ctx->controlled_by_360controller ||
                                ctx->is_steam_virtual_gamepad) ? false : true;
#else
    const bool invert_y_axes = true;
#endif
    Uint64 timestamp = SDL_GetTicksNS();

    if (ctx->last_state[2] != data[2]) {
        Uint8 hat = 0;

        if (data[2] & 0x01) {
            hat |= SDL_HAT_UP;
        }
        if (data[2] & 0x02) {
            hat |= SDL_HAT_DOWN;
        }
        if (data[2] & 0x04) {
            hat |= SDL_HAT_LEFT;
        }
        if (data[2] & 0x08) {
            hat |= SDL_HAT_RIGHT;
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);

        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data[2] & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data[2] & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data[2] & 0x40) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, ((data[2] & 0x80) != 0));
    }

    if (ctx->last_state[3] != data[3]) {
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[3] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data[3] & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[3] & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[3] & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[3] & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[3] & 0x40) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[3] & 0x80) != 0));
    }

    axis = ((int)data[4] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis);
    axis = ((int)data[5] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis);
    axis = SDL_Swap16LE(*(Sint16 *)(&data[6]));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, axis);
    axis = SDL_Swap16LE(*(Sint16 *)(&data[8]));
    if (invert_y_axes) {
        axis = ~axis;
    }
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, axis);
    axis = SDL_Swap16LE(*(Sint16 *)(&data[10]));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis);
    axis = SDL_Swap16LE(*(Sint16 *)(&data[12]));
    if (invert_y_axes) {
        axis = ~axis;
    }
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, axis);

    SDL_memcpy(ctx->last_state, data, SDL_min((size_t)size, sizeof(ctx->last_state)));
}

static bool HIDAPI_DriverXbox360_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverXbox360_Context *ctx = (SDL_DriverXbox360_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size = 0;

    if (!ctx->pad_id) {
        return false;
    }
    joystick = SDL_GetJoystickFromID(ctx->pad_id);

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
#ifdef DEBUG_XBOX_PROTOCOL
        HIDAPI_DumpPacket("Xbox 360 packet: size = %d", data, size);
#endif
#ifdef HAVE_LIBUSB
        if (ctx->chatpad_active) {
            // 08 03 with bit 0 of byte 2 clear: the chatpad was taken out
            SDL_Xbox360Acc_WiredPadReport(&ctx->wired, data, (size_t)size);
        }
#endif
        if (!joystick) {
            continue;
        }

        if (data[0] == 0x00) {
            HIDAPI_DriverXbox360_HandleStatePacket(joystick, ctx, data, size);
        }
    }

    if (size < 0) {
        // Read error, device is disconnected
#ifdef HAVE_LIBUSB
        if (ctx->chatpad_active) {
            SDL_Xbox360Acc_WiredStop(&ctx->wired);
            HIDAPI_DriverXbox360_SyncChatpad(device, ctx);
        }
#endif
        HIDAPI_JoystickDisconnected(device, ctx->pad_id);
        ctx->pad_id = 0;
    }
#ifdef HAVE_LIBUSB
    else if (ctx->chatpad_active) {
        HIDAPI_DriverXbox360_UpdateChatpad(device, ctx);
    }
#endif
    return (size >= 0);
}

static void HIDAPI_DriverXbox360_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverXbox360_Context *ctx = (SDL_DriverXbox360_Context *)device->context;

    if (IsChatpadJoystick(ctx, joystick)) {
        return;
    }

    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_PLAYER_LED,
                        SDL_PlayerLEDHintChanged, ctx);

    ctx->joystick = NULL;
}

static void HIDAPI_DriverXbox360_FreeDevice(SDL_HIDAPI_Device *device)
{
#ifdef HAVE_LIBUSB
    SDL_DriverXbox360_Context *ctx = (SDL_DriverXbox360_Context *)device->context;

    if (ctx) {
        HIDAPI_DriverXbox360_FreeChatpad(ctx);
    }
#endif
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverXbox360 = {
    SDL_HINT_JOYSTICK_HIDAPI_XBOX_360,
    true,
    HIDAPI_DriverXbox360_RegisterHints,
    HIDAPI_DriverXbox360_UnregisterHints,
    HIDAPI_DriverXbox360_IsEnabled,
    HIDAPI_DriverXbox360_IsSupportedDevice,
    HIDAPI_DriverXbox360_InitDevice,
    HIDAPI_DriverXbox360_GetDevicePlayerIndex,
    HIDAPI_DriverXbox360_SetDevicePlayerIndex,
    HIDAPI_DriverXbox360_UpdateDevice,
    HIDAPI_DriverXbox360_OpenJoystick,
    HIDAPI_DriverXbox360_RumbleJoystick,
    HIDAPI_DriverXbox360_RumbleJoystickTriggers,
    HIDAPI_DriverXbox360_GetJoystickCapabilities,
    HIDAPI_DriverXbox360_SetJoystickLED,
    HIDAPI_DriverXbox360_SendJoystickEffect,
    HIDAPI_DriverXbox360_SetJoystickSensorsEnabled,
    HIDAPI_DriverXbox360_CloseJoystick,
    HIDAPI_DriverXbox360_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_XBOX360

#endif // SDL_JOYSTICK_HIDAPI
