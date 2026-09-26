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

#include "SDL_hidapi_xbox360acc_proto.h"

#include <string.h>

#define XBOX360ACC_VENDOR_MICROSOFT           0x045E
#define XBOX360ACC_PRODUCT_WIRED              0x028E
#define XBOX360ACC_PRODUCT_RECEIVER           0x0719
#define XBOX360ACC_PRODUCT_RECEIVER_THIRD1    0x02A9
#define XBOX360ACC_PRODUCT_RECEIVER_THIRD2    0x0291

/* The key codes in button order from button 4, row by row and left to
 * right: 1 to 9 and 0, Q to P, A to L and comma, Z to M, period and Enter,
 * then Left, Space, Right and Backspace. The codes name physical positions
 * and stay the same under every regional printing of the keycaps. */
static const uint8_t xbox360acc_key_codes[SDL_XBOX360ACC_CHATPAD_BUTTONS - 4] = {
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x67, 0x66, 0x65,
    0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21, 0x76, 0x75, 0x64,
    0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31, 0x77, 0x72, 0x62,
    0x46, 0x45, 0x44, 0x43, 0x42, 0x41, 0x52, 0x53, 0x63,
    0x55, 0x54, 0x51, 0x71
};

int SDL_Xbox360Acc_ChatpadButton(uint8_t code)
{
    size_t i;

    /* 0x00 is the empty key slot and no key sends it */
    if (code == 0x00) {
        return -1;
    }
    for (i = 0; i < sizeof(xbox360acc_key_codes); ++i) {
        if (xbox360acc_key_codes[i] == code) {
            return (int)i + 4;
        }
    }
    return -1;
}

bool SDL_Xbox360Acc_IsAccessoryGUID(uint16_t vendor, uint16_t product, uint8_t guid_byte15)
{
    if (guid_byte15 == SDL_XBOX360ACC_GUID_CHATPAD) {
        return (vendor == XBOX360ACC_VENDOR_MICROSOFT && product == XBOX360ACC_PRODUCT_WIRED) ||
               SDL_Xbox360Acc_ReceiverSupported(vendor, product);
    }
    if (guid_byte15 == SDL_XBOX360ACC_GUID_UDRAW) {
        return SDL_Xbox360Acc_ReceiverSupported(vendor, product);
    }
    return false;
}

bool SDL_Xbox360Acc_ChatpadLampValid(uint8_t effect)
{
    return effect <= 0x04 || (effect >= 0x08 && effect <= 0x0C);
}

/* The held state of one key report: the modifier bits 0 to 3 as buttons 0
 * to 3, and each key code a key sends. Two keys at most are held at once. */
static uint64_t Xbox360Acc_KeyButtons(uint8_t modifiers, uint8_t key1, uint8_t key2)
{
    uint64_t buttons = (uint64_t)(modifiers & 0x0F);
    int button;

    button = SDL_Xbox360Acc_ChatpadButton(key1);
    if (button >= 0) {
        buttons |= (uint64_t)1 << button;
    }
    button = SDL_Xbox360Acc_ChatpadButton(key2);
    if (button >= 0) {
        buttons |= (uint64_t)1 << button;
    }
    return buttons;
}

/* The five chatpad bytes, in the wired report or bytes 24 to 28 of a
 * receiver packet. Returns true for F0 03, which asks for a 1B. */
static bool Xbox360Acc_ChatpadMessage(SDL_Xbox360AccChatpad *chatpad, const uint8_t *message)
{
    if (message[0] == 0x00) {
        chatpad->present = true;
        chatpad->buttons = Xbox360Acc_KeyButtons(message[1], message[2], message[3]);
        return false;
    }
    if (message[0] == 0xF0) {
        /* A status: 04 carries the lit lamps and a clock, 03 is the
           handshake. Neither holds a key. */
        chatpad->present = true;
        return message[1] == 0x03;
    }
    return false;
}

static void Xbox360Acc_ChatpadGone(SDL_Xbox360AccChatpad *chatpad)
{
    chatpad->present = false;
    chatpad->buttons = 0;
}

/* The wired chatpad */

bool SDL_Xbox360Acc_WiredSupported(uint16_t vendor, uint16_t product, uint16_t bcd_device)
{
    return vendor == XBOX360ACC_VENDOR_MICROSOFT && product == XBOX360ACC_PRODUCT_WIRED &&
           (bcd_device == 0x0110 || bcd_device == 0x0114);
}

bool SDL_Xbox360Acc_IsChatpadInterface(uint8_t number, uint8_t interface_class,
                                       uint8_t interface_subclass, uint8_t interface_protocol)
{
    return number == 2 && interface_class == 0xFF && interface_subclass == 0x5D && interface_protocol == 0x02;
}

uint8_t SDL_Xbox360Acc_ChatpadEndpoint(const SDL_Xbox360AccEndpoint *endpoints, int count, uint16_t *packet_size)
{
    int i;

    if (!endpoints || !packet_size) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        const uint16_t size = (uint16_t)(endpoints[i].max_packet_size & 0x07FF);

        if ((endpoints[i].attributes & 0x03) != 0x03 || !(endpoints[i].address & 0x80)) {
            continue;
        }
        /* The first interrupt IN endpoint is the chatpad's, and a 5-byte
           report must fit in one packet */
        if (size < 5 || size > SDL_XBOX360ACC_CHATPAD_READ_MAX) {
            return 0;
        }
        *packet_size = size;
        return endpoints[i].address;
    }
    return 0;
}

void SDL_Xbox360Acc_ControlSetup(const SDL_Xbox360AccControl *control, uint8_t setup[8])
{
    setup[0] = control->request_type;
    setup[1] = control->request;
    setup[2] = (uint8_t)(control->value & 0xFF);
    setup[3] = (uint8_t)(control->value >> 8);
    setup[4] = (uint8_t)(control->index & 0xFF);
    setup[5] = (uint8_t)(control->index >> 8);
    setup[6] = (uint8_t)(control->length & 0xFF);
    setup[7] = (uint8_t)(control->length >> 8);
}

static void Xbox360Acc_SetControl(SDL_Xbox360AccControl *control, uint8_t request_type, uint8_t request,
                                  uint16_t value, uint16_t index, uint16_t length)
{
    memset(control, 0, sizeof(*control));
    control->request_type = request_type;
    control->request = request;
    control->value = value;
    control->index = index;
    control->length = length;
}

/* A request on the chatpad interface, 41 00 with the command in wValue */
static void Xbox360Acc_ChatpadCommand(SDL_Xbox360AccControl *control, uint8_t command)
{
    Xbox360Acc_SetControl(control, 0x41, 0x00, command, 0x0002, 0);
}

void SDL_Xbox360Acc_WiredStart(SDL_Xbox360AccWired *wired, uint16_t bcd_device, uint64_t now_ms)
{
    if (!wired) {
        return;
    }
    memset(wired, 0, sizeof(*wired));
    wired->bcd_device = bcd_device;
    wired->step = 1;
    wired->due_ms = now_ms;
}

void SDL_Xbox360Acc_WiredStop(SDL_Xbox360AccWired *wired)
{
    if (!wired) {
        return;
    }
    wired->step = SDL_XBOX360ACC_WIRED_STOPPED;
    wired->in_flight = false;
    wired->in_flight_extra = false;
    wired->send_1b = false;
    Xbox360Acc_ChatpadGone(&wired->chatpad);
}

bool SDL_Xbox360Acc_WiredNext(SDL_Xbox360AccWired *wired, uint64_t now_ms, SDL_Xbox360AccControl *control)
{
    if (!wired || !control || wired->step == SDL_XBOX360ACC_WIRED_STOPPED || wired->in_flight) {
        return false;
    }
    if (wired->send_1b) {
        wired->send_1b = false;
        wired->in_flight = true;
        wired->in_flight_extra = true;
        Xbox360Acc_ChatpadCommand(control, 0x1B);
        return true;
    }
    if (now_ms < wired->due_ms) {
        return false;
    }

    switch (wired->step) {
    case 1:
        Xbox360Acc_SetControl(control, 0x40, 0xA9, 0xA30C, 0x4423, 0);
        break;
    case 2:
        Xbox360Acc_SetControl(control, 0x40, 0xA9, 0x2344, 0x7F03, 0);
        break;
    case 3:
        Xbox360Acc_SetControl(control, 0x40, 0xA9, 0x5839, 0x6832, 0);
        break;
    case 4:
    case 6:
        Xbox360Acc_SetControl(control, 0xC0, 0xA1, 0x0000, 0xE416, 2);
        break;
    case 5:
        Xbox360Acc_SetControl(control, 0x40, 0xA1, 0x0000, 0xE416, 2);
        if (wired->bcd_device == 0x0114) {
            control->data[0] = 0x09;
            control->data[1] = 0x00;
        } else {
            control->data[0] = 0x01;
            control->data[1] = 0x02;
        }
        break;
    case 7:
        Xbox360Acc_ChatpadCommand(control, 0x1F);
        break;
    case 8:
        Xbox360Acc_ChatpadCommand(control, 0x1E);
        break;
    case 9:
        Xbox360Acc_ChatpadCommand(control, 0x1B);
        break;
    default:
        Xbox360Acc_ChatpadCommand(control, wired->keepalive_1e ? 0x1E : 0x1F);
        break;
    }
    wired->in_flight = true;
    wired->in_flight_extra = false;
    return true;
}

void SDL_Xbox360Acc_WiredDone(SDL_Xbox360AccWired *wired, uint64_t now_ms, bool completed)
{
    /* Steps 1 to 3 stall on every pad the sources show, and xboxdrv goes on
       after a stall at any step */
    (void)completed;

    if (!wired || !wired->in_flight) {
        return;
    }
    wired->in_flight = false;
    if (wired->in_flight_extra) {
        wired->in_flight_extra = false;
        return;
    }

    switch (wired->step) {
    case 6:
    case 7:
        /* 1F comes 1000 ms after step 6, and 1E 1000 ms after 1F */
        ++wired->step;
        wired->due_ms = now_ms + SDL_XBOX360ACC_WIRED_KEEPALIVE_MS;
        break;
    case 9:
        /* 1B enables the key reports, and the keep-alives start with 1F */
        wired->step = SDL_XBOX360ACC_WIRED_KEEPALIVE;
        wired->keepalive_1e = false;
        wired->due_ms = now_ms + SDL_XBOX360ACC_WIRED_KEEPALIVE_MS;
        break;
    case SDL_XBOX360ACC_WIRED_KEEPALIVE:
        wired->keepalive_1e = !wired->keepalive_1e;
        wired->due_ms = now_ms + SDL_XBOX360ACC_WIRED_KEEPALIVE_MS;
        break;
    default:
        /* Steps 1 to 5 and 8: the next one goes out at once */
        ++wired->step;
        wired->due_ms = now_ms;
        break;
    }
}

bool SDL_Xbox360Acc_WiredReading(const SDL_Xbox360AccWired *wired)
{
    return wired && wired->step == SDL_XBOX360ACC_WIRED_KEEPALIVE;
}

void SDL_Xbox360Acc_WiredChatpadReport(SDL_Xbox360AccWired *wired, const uint8_t *data, size_t length)
{
    if (!wired || !data || length < 5 || !SDL_Xbox360Acc_WiredReading(wired)) {
        return;
    }
    if (Xbox360Acc_ChatpadMessage(&wired->chatpad, data)) {
        wired->send_1b = true;
    }
}

void SDL_Xbox360Acc_WiredPadReport(SDL_Xbox360AccWired *wired, const uint8_t *data, size_t length)
{
    if (!wired || !data || length < 3) {
        return;
    }
    /* Report 08, size 03: bit 0 of the status is the text input device */
    if (data[0] == 0x08 && data[1] == 0x03 && !(data[2] & 0x01)) {
        Xbox360Acc_ChatpadGone(&wired->chatpad);
    }
}

bool SDL_Xbox360Acc_WiredLamp(const uint8_t *effect, size_t size, SDL_Xbox360AccControl *control)
{
    if (!effect || size != 1 || !control || !SDL_Xbox360Acc_ChatpadLampValid(effect[0])) {
        return false;
    }
    Xbox360Acc_ChatpadCommand(control, effect[0]);
    return true;
}

/* The wireless chatpad and the uDraw */

bool SDL_Xbox360Acc_ReceiverSupported(uint16_t vendor, uint16_t product)
{
    return vendor == XBOX360ACC_VENDOR_MICROSOFT &&
           (product == XBOX360ACC_PRODUCT_RECEIVER || product == XBOX360ACC_PRODUCT_RECEIVER_THIRD1 ||
            product == XBOX360ACC_PRODUCT_RECEIVER_THIRD2);
}

void SDL_Xbox360Acc_SlotInit(SDL_Xbox360AccSlot *slot, bool chatpad, bool udraw)
{
    if (!slot) {
        return;
    }
    memset(slot, 0, sizeof(*slot));
    slot->chatpad_enabled = chatpad;
    slot->udraw_enabled = udraw;
}

static void Xbox360Acc_SlotQueue(SDL_Xbox360AccSlot *slot, uint8_t command)
{
    /* The driver sends every command after each update, so the queue holds
       a few at most. A full queue drops the new one. */
    if (slot->command_count < SDL_XBOX360ACC_COMMAND_QUEUE) {
        slot->commands[slot->command_count++] = command;
    }
}

/* The slot shows a device: the chatpad commands start 100 ms from now,
   unless they run already */
static void Xbox360Acc_SlotShowsDevice(SDL_Xbox360AccSlot *slot, uint64_t now_ms)
{
    slot->link = true;
    if (slot->chatpad_enabled && !slot->scheduled) {
        slot->scheduled = true;
        slot->need_init = true;
        slot->keepalive_1f = false;
        slot->due_ms = now_ms + SDL_XBOX360ACC_SLOT_START_MS;
    }
}

static void Xbox360Acc_TabletReset(SDL_Xbox360AccTablet *tablet)
{
    memset(tablet, 0, sizeof(*tablet));
}

static void Xbox360Acc_SlotEmpty(SDL_Xbox360AccSlot *slot)
{
    slot->link = false;
    slot->kind = SDL_XBOX360ACC_SLOT_NONE;
    slot->scheduled = false;
    slot->need_init = false;
    slot->keepalive_1f = false;
    slot->lamps = 0;
    slot->command_count = 0;
    Xbox360Acc_ChatpadGone(&slot->chatpad);
    Xbox360Acc_TabletReset(&slot->tablet);
}

/* A new kind of device on the slot. A chatpad belongs to a pad, so a tablet
   takes the slot without one, whether a pad or no type came before it. A
   tablet starts at rest with X and Y at 0. */
static void Xbox360Acc_SlotBecomes(SDL_Xbox360AccSlot *slot, uint8_t kind)
{
    if (slot->kind == kind) {
        return;
    }
    if (kind == SDL_XBOX360ACC_SLOT_TABLET) {
        Xbox360Acc_ChatpadGone(&slot->chatpad);
        slot->lamps = 0;
        Xbox360Acc_TabletReset(&slot->tablet);
    }
    slot->kind = kind;
}

static bool Xbox360Acc_IsTabletFraming(const uint8_t *data, size_t length)
{
    return length >= 18 && data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 &&
           data[3] == 0xF0 && data[4] == 0x00 && data[5] == 0x13;
}

int SDL_Xbox360Acc_SlotPacket(SDL_Xbox360AccSlot *slot, uint64_t now_ms, const uint8_t *data, size_t length)
{
    if (!slot || !data) {
        return SDL_XBOX360ACC_PACKET_OTHER;
    }

    /* The connection status: bit 7 of byte 1 is the data device */
    if (length == 2 && data[0] == 0x08) {
        if (data[1] & 0x80) {
            Xbox360Acc_SlotShowsDevice(slot, now_ms);
            if (!slot->udraw_enabled) {
                Xbox360Acc_SlotBecomes(slot, SDL_XBOX360ACC_SLOT_PAD);
            }
        } else {
            Xbox360Acc_SlotEmpty(slot);
        }
        return SDL_XBOX360ACC_PACKET_STATUS;
    }

    /* The link control packet: the subtype is byte 25, whose bit 7 is the
       force feedback flag */
    if (length == 29 && data[0] == 0x00 && data[1] == 0x0F && data[2] == 0x00 && data[3] == 0xF0) {
        Xbox360Acc_SlotShowsDevice(slot, now_ms);
        if (slot->udraw_enabled) {
            if ((data[25] & 0x7F) == SDL_XBOX360ACC_SUBTYPE_UDRAW) {
                Xbox360Acc_SlotBecomes(slot, SDL_XBOX360ACC_SLOT_TABLET);
            } else {
                Xbox360Acc_SlotBecomes(slot, SDL_XBOX360ACC_SLOT_PAD);
            }
        }
        return SDL_XBOX360ACC_PACKET_INFO;
    }

    /* A tablet sends its pen and buttons in the pad state packet */
    if (slot->kind == SDL_XBOX360ACC_SLOT_TABLET && Xbox360Acc_IsTabletFraming(data, length)) {
        Xbox360Acc_SlotShowsDevice(slot, now_ms);
        SDL_Xbox360Acc_TabletDecode(&slot->tablet, data, length);
        return SDL_XBOX360ACC_PACKET_TABLET;
    }

    /* Pad state: types 0x01 and 0x03. Type 0x05 with byte 5 at 0x12 is the
       capabilities reply, which the driver reads first. */
    if (length == 29 && data[0] == 0x00 && (data[1] & 0x01) &&
        !(data[1] == 0x05 && data[5] == 0x12)) {
        Xbox360Acc_SlotShowsDevice(slot, now_ms);
        if (slot->udraw_enabled && slot->kind == SDL_XBOX360ACC_SLOT_NONE) {
            Xbox360Acc_SlotBecomes(slot, SDL_XBOX360ACC_SLOT_PAD);
        }
        return SDL_XBOX360ACC_PACKET_PAD;
    }

    /* Type 0x02 carries only the plug-in bytes 24 to 28 */
    if (length >= 28 && data[0] == 0x00 && data[1] == 0x02 && data[2] == 0x00 && data[3] == 0xF0) {
        if (slot->chatpad_enabled && slot->kind != SDL_XBOX360ACC_SLOT_TABLET) {
            if (Xbox360Acc_ChatpadMessage(&slot->chatpad, &data[24])) {
                slot->need_init = true;
            }
        }
        return SDL_XBOX360ACC_PACKET_CHATPAD;
    }
    return SDL_XBOX360ACC_PACKET_OTHER;
}

void SDL_Xbox360Acc_SlotUpdate(SDL_Xbox360AccSlot *slot, uint64_t now_ms)
{
    if (!slot || !slot->scheduled || now_ms < slot->due_ms) {
        return;
    }
    if (slot->need_init) {
        slot->need_init = false;
        Xbox360Acc_SlotQueue(slot, 0x1B);
    } else {
        Xbox360Acc_SlotQueue(slot, slot->keepalive_1f ? 0x1F : 0x1E);
        slot->keepalive_1f = !slot->keepalive_1f;
    }
    /* The lamps that are lit, again, in case the command before was lost */
    if (slot->lamps & 0x01) {
        Xbox360Acc_SlotQueue(slot, 0x08);
    }
    if (slot->lamps & 0x02) {
        Xbox360Acc_SlotQueue(slot, 0x09);
    }
    if (slot->lamps & 0x04) {
        Xbox360Acc_SlotQueue(slot, 0x0A);
    }
    if (slot->lamps & 0x08) {
        Xbox360Acc_SlotQueue(slot, 0x0B);
    }
    slot->due_ms = now_ms + SDL_XBOX360ACC_SLOT_KEEPALIVE_MS;
}

bool SDL_Xbox360Acc_SlotLamp(SDL_Xbox360AccSlot *slot, const uint8_t *effect, size_t size)
{
    if (!slot || !slot->chatpad_enabled || !effect || size != 1 || !SDL_Xbox360Acc_ChatpadLampValid(effect[0])) {
        return false;
    }
    /* 08 to 0B light Shift, Green, Orange and People, 00 to 03 put them out.
       The backlight's 04 and 0C are not sent again. */
    if (effect[0] >= 0x08 && effect[0] <= 0x0B) {
        slot->lamps |= (uint8_t)(1u << (effect[0] - 0x08));
    } else if (effect[0] <= 0x03) {
        slot->lamps &= (uint8_t)~(1u << effect[0]);
    }
    Xbox360Acc_SlotQueue(slot, effect[0]);
    return true;
}

bool SDL_Xbox360Acc_SlotTakeCommand(SDL_Xbox360AccSlot *slot, uint8_t packet[SDL_XBOX360ACC_COMMAND_SIZE])
{
    if (!slot || !packet || slot->command_count <= 0) {
        return false;
    }
    memset(packet, 0, SDL_XBOX360ACC_COMMAND_SIZE);
    packet[2] = 0x0C;
    packet[3] = slot->commands[0];
    --slot->command_count;
    memmove(&slot->commands[0], &slot->commands[1], (size_t)slot->command_count);
    return true;
}

void SDL_Xbox360Acc_SlotLost(SDL_Xbox360AccSlot *slot)
{
    if (slot) {
        Xbox360Acc_SlotEmpty(slot);
    }
}

/* value * 257 - 32768, so 0x00 is -32768 and 0xFF is 32767 */
static int16_t Xbox360Acc_ByteAxis(uint8_t value)
{
    return (int16_t)((int32_t)value * 257 - 32768);
}

bool SDL_Xbox360Acc_TabletDecode(SDL_Xbox360AccTablet *tablet, const uint8_t *data, size_t length)
{
    uint16_t buttons = 0;
    uint8_t hat = 0;
    uint8_t touch;

    if (!tablet || !data || !Xbox360Acc_IsTabletFraming(data, length)) {
        return false;
    }

    /* Byte 6: D-pad up, down, left and right, Start and Back */
    if (data[6] & 0x01) {
        hat |= SDL_XBOX360ACC_HAT_UP;
    }
    if (data[6] & 0x02) {
        hat |= SDL_XBOX360ACC_HAT_DOWN;
    }
    if (data[6] & 0x04) {
        hat |= SDL_XBOX360ACC_HAT_LEFT;
    }
    if (data[6] & 0x08) {
        hat |= SDL_XBOX360ACC_HAT_RIGHT;
    }
    if (data[6] & 0x10) {
        buttons |= 1u << SDL_XBOX360ACC_TABLET_BUTTON_START;
    }
    if (data[6] & 0x20) {
        buttons |= 1u << SDL_XBOX360ACC_TABLET_BUTTON_BACK;
    }
    /* Byte 7: Guide, then A, B, X and Y. Bits 0, 1 and 3 are unused. */
    if (data[7] & 0x04) {
        buttons |= 1u << SDL_XBOX360ACC_TABLET_BUTTON_GUIDE;
    }
    if (data[7] & 0x10) {
        buttons |= 1u << SDL_XBOX360ACC_TABLET_BUTTON_A;
    }
    if (data[7] & 0x20) {
        buttons |= 1u << SDL_XBOX360ACC_TABLET_BUTTON_B;
    }
    if (data[7] & 0x40) {
        buttons |= 1u << SDL_XBOX360ACC_TABLET_BUTTON_X;
    }
    if (data[7] & 0x80) {
        buttons |= 1u << SDL_XBOX360ACC_TABLET_BUTTON_Y;
    }

    /* Byte 14: 0x40 the pen, 0x80 a finger, any other nonzero value two
       fingers pinching */
    touch = data[14];
    if (touch == 0x40) {
        buttons |= 1u << SDL_XBOX360ACC_TABLET_BUTTON_PEN;
    } else if (touch == 0x80) {
        buttons |= 1u << SDL_XBOX360ACC_TABLET_BUTTON_FINGER;
    } else if (touch != 0x00) {
        buttons |= 1u << SDL_XBOX360ACC_TABLET_BUTTON_MULTI;
    }

    /* The surface is 8 by 5 cells of 256 by 256 over 1920 by 1080, the last
       column 128 wide and the last row 56 tall. A cell of 0x0F means nothing
       touches it, and the position holds. */
    if (touch != 0x00 && data[11] < 0x0F && data[13] < 0x0F) {
        int32_t x = (int32_t)data[11] * 256 + data[10];
        int32_t y = (int32_t)data[13] * 256 + data[12];

        if (x > 1919) {
            x = 1919;
        }
        if (y > 1079) {
            y = 1079;
        }
        tablet->axes[SDL_XBOX360ACC_TABLET_AXIS_X] = (int16_t)(x * 65535 / 1919 - 32768);
        tablet->axes[SDL_XBOX360ACC_TABLET_AXIS_Y] = (int16_t)(y * 65535 / 1079 - 32768);
    }
    tablet->axes[SDL_XBOX360ACC_TABLET_AXIS_PRESSURE] = Xbox360Acc_ByteAxis(data[8]);
    tablet->axes[SDL_XBOX360ACC_TABLET_AXIS_SPREAD] = Xbox360Acc_ByteAxis(data[15]);
    tablet->axes[SDL_XBOX360ACC_TABLET_AXIS_TILT_X] = Xbox360Acc_ByteAxis(data[16]);
    tablet->axes[SDL_XBOX360ACC_TABLET_AXIS_TILT_Y] = Xbox360Acc_ByteAxis(data[17]);
    tablet->axes[SDL_XBOX360ACC_TABLET_AXIS_TILT_Z] = Xbox360Acc_ByteAxis(data[9]);
    tablet->buttons = buttons;
    tablet->hat = hat;
    return true;
}
