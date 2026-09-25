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

#include "SDL_hidapi_xid_proto.h"

#include "../usb_ids.h"

#include <string.h>

#define XID_ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))

/* The XID devices known by ID. The IDs and flags are the ones Linux xpad
   lists for the original Xbox, plus a Radica pad from Xb2XInput and two
   light guns from xboxdevwiki. The names are short product names written for
   this table. An ID here never admits an interface by itself: the interface
   class does. */
static const SDL_XIDKnownDevice xid_known_devices[] = {
    { 0x044F, 0x0F00, 0, "Thrustmaster Wheel" },
    { 0x044F, 0x0F03, 0, "Thrustmaster Wheel" },
    { 0x044F, 0x0F07, 0, "Thrustmaster Controller" },
    { 0x044F, 0x0F10, 0, "Thrustmaster Modena GT Wheel" },
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX_ORIGINAL_PAD_V1, 0, "Xbox Controller" },
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX_ORIGINAL_PAD_JAPAN, 0, "Xbox Controller" },
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX_ORIGINAL_CONTROLLER_S, 0, "Xbox Controller S" },
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX_ORIGINAL_CONTROLLER_S_V2, 0, "Xbox Controller S" },
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX_ORIGINAL_PAD_V2, 0, "Xbox Controller S" },
    { 0x046D, 0xCA84, 0, "Logitech Cordless Precision Controller" },
    { 0x046D, 0xCA88, 0, "Logitech Compact Controller" },
    { 0x046D, 0xCA8A, 0, "Logitech Precision Vibration Feedback Wheel" },
    { 0x05FD, 0x1007, 0, "Mad Catz Controller" },
    { 0x05FD, 0x107A, 0, "InterAct PowerPad Pro" },
    { 0x05FE, 0x3030, 0, "Chic Controller" },
    { 0x05FE, 0x3031, 0, "Chic Controller" },
    { 0x062A, 0x0020, 0, "Logic3 Xbox GamePad" },
    { 0x062A, 0x0033, 0, "Competition Pro Steering Wheel" },
    { 0x06A3, 0x0200, 0, "Saitek Racing Wheel" },
    { 0x06A3, 0x0201, 0, "Saitek Adrenalin" },
    { 0x0738, 0x4506, 0, "Mad Catz 4506 Wireless Controller" },
    { 0x0738, 0x4516, 0, "Mad Catz Control Pad" },
    { 0x0738, 0x4520, 0, "Mad Catz Control Pad Pro" },
    { 0x0738, 0x4522, 0, "Mad Catz LumiCON" },
    { 0x0738, 0x4526, 0, "Mad Catz Control Pad Pro" },
    { 0x0738, 0x4530, 0, "Mad Catz MC2 Racing Wheel" },
    { 0x0738, 0x4536, 0, "Mad Catz MicroCON" },
    { 0x0738, 0x4540, SDL_XID_FLAG_DPAD_BUTTONS, "Mad Catz Beat Pad" },
    { 0x0738, 0x4556, 0, "Mad Catz Lynx Wireless Controller" },
    { 0x0738, 0x4586, 0, "Mad Catz MicroCON Wireless Controller" },
    { 0x0738, 0x4588, 0, "Mad Catz Blaster" },
    { 0x0738, 0x45FF, SDL_XID_FLAG_DPAD_BUTTONS, "Mad Catz Beat Pad" },
    { 0x0738, 0x4743, SDL_XID_FLAG_DPAD_BUTTONS, "Mad Catz Beat Pad Pro" },
    { 0x0738, 0x6040, SDL_XID_FLAG_DPAD_BUTTONS, "Mad Catz Beat Pad Pro" },
    { 0x0B9A, 0x016B, 0, "EMS TopGun II" },
    { 0x0C12, 0x0005, 0, "Intec Wireless Controller" },
    { 0x0C12, 0x8801, 0, "Nyko Xbox Controller" },
    { 0x0C12, 0x8802, 0, "Zeroplus Xbox Controller" },
    { 0x0C12, 0x8809, SDL_XID_FLAG_DANCE_PAD, "RedOctane Dance Pad" },
    { 0x0C12, 0x880A, 0, "Pelican Eclipse PL-2023" },
    { 0x0C12, 0x8810, 0, "Zeroplus Xbox Controller" },
    { 0x0C12, 0x9902, 0, "HAMA VibraX" },
    { 0x0D2F, 0x0002, SDL_XID_FLAG_DPAD_BUTTONS, "Andamiro Pump It Up Pad" },
    { 0x0E4C, 0x1097, 0, "Radica Gamester Controller" },
    { 0x0E4C, 0x1103, SDL_XID_FLAG_TRIGGER_BUTTONS, "Radica Gamester Reflex" },
    { 0x0E4C, 0x2390, 0, "Radica Jtech Controller" },
    { 0x0E4C, 0x3240, 0, "Radica Gamester" },
    { 0x0E4C, 0x3510, 0, "Radica Gamester" },
    { 0x0E6F, 0x0003, 0, "Logic3 Freebird Wireless Controller" },
    { 0x0E6F, 0x0005, 0, "Eclipse Wireless Controller" },
    { 0x0E6F, 0x0006, 0, "Edge Wireless Controller" },
    { 0x0E6F, 0x0008, 0, "Afterglow Pro Controller" },
    { 0x0E8F, 0x0201, 0, "SmartJoy Frag Xpad" },
    { 0x0E8F, 0x3008, 0, "Xbox Controller" },
    { 0x0F30, 0x010B, 0, "Philips Recoil" },
    { 0x0F30, 0x0202, 0, "Joytech Advanced Controller" },
    { 0x0F30, 0x8888, 0, "BigBen XBMiniPad" },
    { 0x102C, 0xFF0C, 0, "Joytech Wireless Advanced Controller" },
    { 0x1292, 0x3006, 0, "Joytech Sharp Shooter" },
    { 0x12AB, 0x8809, SDL_XID_FLAG_DPAD_BUTTONS, "Xbox Dance Pad" },
    { 0x1430, 0x8888, SDL_XID_FLAG_DPAD_BUTTONS, "TX6500+ Dance Pad" },
    { 0x3767, 0x0101, 0, "Fanatec Speedster 3 Forceshock Wheel" },
    { 0xFFFF, 0xFFFF, 0, "Xbox Controller" },
};

bool SDL_XID_IsSupportedInterface(int interface_class, int interface_subclass, int interface_protocol)
{
    return interface_class == SDL_XID_INTERFACE_CLASS &&
           interface_subclass == SDL_XID_INTERFACE_SUBCLASS &&
           interface_protocol == SDL_XID_INTERFACE_PROTOCOL;
}

bool SDL_XID_IsSteelBattalionID(uint16_t vendor, uint16_t product)
{
    return vendor == USB_VENDOR_CAPCOM && product == USB_PRODUCT_CAPCOM_STEEL_BATTALION;
}

bool SDL_XID_ParseDescriptor(const uint8_t *data, size_t length, SDL_XIDDescriptor *out)
{
    if (!data || !out || length < 8) {
        return false;
    }
    if (data[1] != SDL_XID_DESCRIPTOR_TYPE || data[0] < 8 || data[0] > length) {
        return false;
    }
    out->type = data[4];
    out->subtype = data[5];
    out->max_input = data[6];
    out->max_output = data[7];
    return true;
}

const SDL_XIDKnownDevice *SDL_XID_FindKnownDevice(uint16_t vendor, uint16_t product)
{
    size_t i;

    for (i = 0; i < XID_ARRAYSIZE(xid_known_devices); ++i) {
        if (xid_known_devices[i].vendor == vendor && xid_known_devices[i].product == product) {
            return &xid_known_devices[i];
        }
    }
    return NULL;
}

// The joystick type a gamepad-family subtype gives, or -1 when the subtype is not known
static int XID_TypeForSubtype(uint8_t subtype)
{
    switch (subtype) {
    case SDL_XID_SUBTYPE_DUKE:
    case SDL_XID_SUBTYPE_CONTROLLER_S:
        return SDL_XID_JOYSTICK_GAMEPAD;
    case SDL_XID_SUBTYPE_WHEEL:
        return SDL_XID_JOYSTICK_WHEEL;
    case SDL_XID_SUBTYPE_ARCADE_STICK:
    case SDL_XID_SUBTYPE_DIGITAL_STICK:
        return SDL_XID_JOYSTICK_ARCADE_STICK;
    case SDL_XID_SUBTYPE_FLIGHT_STICK:
        return SDL_XID_JOYSTICK_FLIGHT_STICK;
    case SDL_XID_SUBTYPE_SNOWBOARD:
    case SDL_XID_SUBTYPE_LIGHT_GUN:
    case SDL_XID_SUBTYPE_RADIO_FLIGHT:
    case SDL_XID_SUBTYPE_FISHING_ROD:
        return SDL_XID_JOYSTICK_UNKNOWN;
    case SDL_XID_SUBTYPE_DANCE_PAD:
        return SDL_XID_JOYSTICK_DANCE_PAD;
    default:
        return -1;
    }
}

bool SDL_XID_Identify(uint16_t vendor, uint16_t product, const SDL_XIDDescriptor *descriptor,
                      const char *product_string, SDL_XIDIdentity *out)
{
    const SDL_XIDKnownDevice *known = SDL_XID_FindKnownDevice(vendor, product);
    const uint8_t flags = known ? known->flags : 0;
    bool steel_battalion;
    int type = -1;

    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (descriptor) {
        if (descriptor->type == SDL_XID_TYPE_DVD_REMOTE) {
            return false;
        }
        steel_battalion = (descriptor->type == SDL_XID_TYPE_STEEL_BATTALION);
    } else {
        steel_battalion = SDL_XID_IsSteelBattalionID(vendor, product);
    }

    if (steel_battalion) {
        out->kind = SDL_XID_KIND_STEEL_BATTALION;
        out->joystick_type = SDL_XID_JOYSTICK_UNKNOWN;
        out->guid_byte = SDL_XID_GUID_STEEL_BATTALION;
        out->name = "Steel Battalion Controller";
        out->naxes = SDL_XID_SB_AXES;
        out->nbuttons = SDL_XID_SB_BUTTONS;
        out->nhats = 0;
        return true;
    }

    out->kind = SDL_XID_KIND_GAMEPAD;
    if (descriptor && descriptor->type == SDL_XID_TYPE_GAMEPAD) {
        type = XID_TypeForSubtype(descriptor->subtype);
        out->light_gun = (descriptor->subtype == SDL_XID_SUBTYPE_LIGHT_GUN);
        out->dance = (descriptor->subtype == SDL_XID_SUBTYPE_DANCE_PAD);
        out->guid_byte = descriptor->subtype;
    }
    /* Linux applies these per ID, whatever the pad reports: a dance pad's
       arrows are four independent buttons, and its sticks are not read. */
    if (flags & SDL_XID_FLAG_DPAD_BUTTONS) {
        out->dance = true;
    }
    out->null_sticks = ((flags & SDL_XID_FLAG_NULL_STICKS) != 0);
    if (out->dance) {
        type = SDL_XID_JOYSTICK_DANCE_PAD;
        out->light_gun = false;
        out->guid_byte = SDL_XID_GUID_DANCE_PAD;
    } else if (type < 0) {
        type = SDL_XID_JOYSTICK_GAMEPAD;
    }
    out->joystick_type = type;

    if (product_string && *product_string) {
        out->name = product_string;
    } else if (known) {
        out->name = known->name;
    } else if (descriptor && descriptor->type == SDL_XID_TYPE_GAMEPAD && descriptor->subtype == SDL_XID_SUBTYPE_DUKE) {
        out->name = "Xbox Controller";
    } else if (descriptor && descriptor->type == SDL_XID_TYPE_GAMEPAD && descriptor->subtype == SDL_XID_SUBTYPE_CONTROLLER_S) {
        out->name = "Xbox Controller S";
    } else {
        out->name = "Xbox Input Device";
    }

    out->naxes = SDL_XID_GAMEPAD_AXES;
    if (out->dance) {
        out->nbuttons = SDL_XID_DANCE_BUTTONS;
        out->nhats = 0;
    } else if (out->light_gun) {
        out->nbuttons = SDL_XID_LIGHT_GUN_BUTTONS;
        out->nhats = 1;
    } else {
        out->nbuttons = SDL_XID_GAMEPAD_BUTTONS;
        out->nhats = 1;
    }
    return true;
}

static int16_t XID_Byte(uint8_t value)
{
    return (int16_t)((int)value * 257 - 32768);
}

static int16_t XID_Word(const uint8_t *data)
{
    const int value = data[0] | (data[1] << 8);

    return (int16_t)((value >= 0x8000) ? (value - 0x10000) : value);
}

bool SDL_XID_DecodeGamepad(const SDL_XIDIdentity *identity, const uint8_t *report, size_t length,
                           SDL_XIDGamepadState *out)
{
    // A face button's byte: A, B, X, Y, White, Black in SDL button order
    static const struct
    {
        uint8_t offset;
        uint8_t button;
    } faces[] = {
        { 4, 0 }, // A, south
        { 5, 1 }, // B, east
        { 6, 2 }, // X, west
        { 7, 3 }, // Y, north
        { 9, 9 }, // White, left shoulder
        { 8, 10 }, // Black, right shoulder
    };
    uint8_t digital;
    size_t i;

    if (!identity || !report || !out || identity->kind != SDL_XID_KIND_GAMEPAD) {
        return false;
    }
    if (length < SDL_XID_GAMEPAD_REPORT_LENGTH || report[0] != 0x00 || report[1] != SDL_XID_GAMEPAD_REPORT_LENGTH) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    digital = report[2];
    if (digital & 0x20) {
        out->buttons |= (uint16_t)(1u << 4); // Back
    }
    if (digital & 0x10) {
        out->buttons |= (uint16_t)(1u << 6); // Start
    }
    if (digital & 0x40) {
        out->buttons |= (uint16_t)(1u << 7); // left stick
    }
    if (digital & 0x80) {
        out->buttons |= (uint16_t)(1u << 8); // right stick
    }
    for (i = 0; i < XID_ARRAYSIZE(faces); ++i) {
        const uint8_t value = report[faces[i].offset];
        if (value > SDL_XID_PRESS_THRESHOLD) {
            out->buttons |= (uint16_t)(1u << faces[i].button);
        }
        out->axes[6 + i] = XID_Byte(value);
    }

    if (identity->dance) {
        // Four independent arrows, so opposite ones can be held together
        if (digital & 0x01) {
            out->buttons |= (uint16_t)(1u << (SDL_XID_BUTTON_EXTRA + 0));
        }
        if (digital & 0x02) {
            out->buttons |= (uint16_t)(1u << (SDL_XID_BUTTON_EXTRA + 1));
        }
        if (digital & 0x04) {
            out->buttons |= (uint16_t)(1u << (SDL_XID_BUTTON_EXTRA + 2));
        }
        if (digital & 0x08) {
            out->buttons |= (uint16_t)(1u << (SDL_XID_BUTTON_EXTRA + 3));
        }
    } else {
        // Opposite directions cancel, since a hat cannot hold both
        if ((digital & 0x01) && !(digital & 0x02)) {
            out->hat |= SDL_XID_HAT_UP;
        }
        if ((digital & 0x02) && !(digital & 0x01)) {
            out->hat |= SDL_XID_HAT_DOWN;
        }
        if ((digital & 0x04) && !(digital & 0x08)) {
            out->hat |= SDL_XID_HAT_LEFT;
        }
        if ((digital & 0x08) && !(digital & 0x04)) {
            out->hat |= SDL_XID_HAT_RIGHT;
        }
    }
    if (identity->light_gun && (report[3] & 0x20)) {
        out->buttons |= (uint16_t)(1u << SDL_XID_BUTTON_EXTRA);
    }

    if (!identity->null_sticks) {
        // Y is positive up in the report and negative up in SDL
        out->axes[0] = XID_Word(&report[12]);
        out->axes[1] = (int16_t)~XID_Word(&report[14]);
        out->axes[2] = XID_Word(&report[16]);
        out->axes[3] = (int16_t)~XID_Word(&report[18]);
    }
    out->axes[4] = XID_Byte(report[10]);
    out->axes[5] = XID_Byte(report[11]);
    return true;
}

bool SDL_XID_DecodeSteelBattalion(const uint8_t *report, size_t length, SDL_XIDSteelBattalionState *out)
{
    uint8_t gear;
    int bit;

    if (!report || !out) {
        return false;
    }
    if (length < SDL_XID_SB_REPORT_LENGTH || report[0] != 0x00 || report[1] != SDL_XID_SB_REPORT_LENGTH) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    // Buttons 0-38: bytes 2 to 6, byte 2 bit 0 first. Byte 6 bit 7 is unused.
    for (bit = 0; bit < 39; ++bit) {
        if (report[2 + bit / 8] & (1 << (bit % 8))) {
            out->buttons |= (uint64_t)1 << bit;
        }
    }

    // Each axis is the odd byte, in the high byte of the SDL value
    out->axes[0] = (int16_t)(((int)report[9] << 8) - 32768);  // aiming X
    out->axes[1] = (int16_t)(((int)report[11] << 8) - 32768); // aiming Y
    out->axes[2] = (int16_t)((int)(int8_t)report[13] * 256);  // rotation
    out->axes[3] = (int16_t)((int)(int8_t)report[15] * 256);  // sight X
    out->axes[4] = (int16_t)((int)(int8_t)report[17] * 256);  // sight Y
    out->axes[5] = (int16_t)(((int)report[19] << 8) - 32768); // left pedal
    out->axes[6] = (int16_t)(((int)report[21] << 8) - 32768); // middle pedal
    out->axes[7] = (int16_t)(((int)report[23] << 8) - 32768); // right pedal
    out->axes[8] = (int16_t)(-32768 + (int)(report[24] & 0x0F) * 4369); // tuner dial

    /* The gear lever: R -2, N -1 and 1 to 5 from the hardware, 7 to 13 from
       the emulators, 0 between gears. */
    gear = report[25];
    if (gear == 0xFE || gear == 0x07) {
        out->buttons |= (uint64_t)1 << (SDL_XID_SB_GEAR_BUTTON + 0);
    } else if (gear == 0xFF || gear == 0x08) {
        out->buttons |= (uint64_t)1 << (SDL_XID_SB_GEAR_BUTTON + 1);
    } else if (gear >= 0x01 && gear <= 0x05) {
        out->buttons |= (uint64_t)1 << (SDL_XID_SB_GEAR_BUTTON + 1 + gear);
    } else if (gear >= 0x09 && gear <= 0x0D) {
        out->buttons |= (uint64_t)1 << (SDL_XID_SB_GEAR_BUTTON + 1 + (gear - 0x08));
    }
    return true;
}

size_t SDL_XID_BuildRumble(uint16_t low, uint16_t high, uint8_t out[SDL_XID_RUMBLE_LENGTH])
{
    out[0] = 0x00;
    out[1] = SDL_XID_RUMBLE_LENGTH;
    out[2] = (uint8_t)(low & 0xFF);
    out[3] = (uint8_t)(low >> 8);
    out[4] = (uint8_t)(high & 0xFF);
    out[5] = (uint8_t)(high >> 8);
    return SDL_XID_RUMBLE_LENGTH;
}

bool SDL_XID_BuildLamps(const uint8_t *effect, size_t length, uint8_t out[SDL_XID_SB_LAMP_WRITE_LENGTH])
{
    if (!effect || !out || length != SDL_XID_SB_LAMP_EFFECT_LENGTH) {
        return false;
    }
    memset(out, 0, SDL_XID_SB_LAMP_WRITE_LENGTH);
    memcpy(out, effect, SDL_XID_SB_LAMP_EFFECT_LENGTH);
    out[0] = 0x00;
    out[1] = SDL_XID_SB_LAMP_EFFECT_LENGTH;
    return true;
}

const char *SDL_XID_GetMapping(uint8_t guid_byte)
{
    switch (guid_byte) {
    case SDL_XID_GUID_STEEL_BATTALION:
        return NULL;
    case SDL_XID_GUID_DANCE_PAD:
        return SDL_XID_MAPPING_DANCE_PAD;
    default:
        return SDL_XID_MAPPING_GAMEPAD;
    }
}

/* The state at rest, decoded from a report at rest so that it matches what
   the device sends: levers centered, pedals and triggers released. The
   Steel Battalion's gear lever reads between gears and its tuner at 0 until
   its first report. */
static void XID_SetRest(SDL_XIDSession *session)
{
    uint8_t report[SDL_XID_SB_REPORT_LENGTH];

    memset(report, 0, sizeof(report));
    if (session->identity.kind == SDL_XID_KIND_STEEL_BATTALION) {
        report[1] = SDL_XID_SB_REPORT_LENGTH;
        report[9] = 0x80;
        report[11] = 0x80;
        SDL_XID_DecodeSteelBattalion(report, sizeof(report), &session->steel_battalion);
    } else {
        report[1] = SDL_XID_GAMEPAD_REPORT_LENGTH;
        SDL_XID_DecodeGamepad(&session->identity, report, SDL_XID_GAMEPAD_REPORT_LENGTH, &session->gamepad);
    }
}

// Applies one transfer to the state. Returns false for anything that is not a report of this device.
static bool XID_Apply(SDL_XIDSession *session, const uint8_t *data, size_t length)
{
    if (session->identity.kind == SDL_XID_KIND_STEEL_BATTALION) {
        SDL_XIDSteelBattalionState state;
        if (!SDL_XID_DecodeSteelBattalion(data, length, &state)) {
            return false;
        }
        session->steel_battalion = state;
    } else {
        SDL_XIDGamepadState state;
        if (!SDL_XID_DecodeGamepad(&session->identity, data, length, &state)) {
            return false;
        }
        session->gamepad = state;
    }
    return true;
}

static void XID_Post(SDL_XIDSession *session, void *joystick, const SDL_XIDSink *sink)
{
    if (session->identity.kind == SDL_XID_KIND_STEEL_BATTALION) {
        sink->steel_battalion(sink->userdata, joystick, &session->steel_battalion);
    } else {
        sink->gamepad(sink->userdata, joystick, &session->gamepad);
    }
    session->post_pending = false;
}

bool SDL_XID_Open(SDL_XIDSession *session, uint16_t vendor, uint16_t product, uint8_t interface_number,
                  const char *product_string, const SDL_XIDSink *sink)
{
    uint8_t reply[SDL_XID_READ_LENGTH];
    SDL_XIDDescriptor descriptor;
    const SDL_XIDDescriptor *parsed = NULL;
    int received;

    if (!session || !sink) {
        return false;
    }
    memset(session, 0, sizeof(*session));
    session->interface_number = interface_number;

    memset(reply, 0, sizeof(reply));
    received = sink->control_in(sink->userdata, SDL_XID_REQUEST_TYPE_VENDOR_IN, SDL_XID_REQUEST_GET_DESCRIPTOR,
                                SDL_XID_DESCRIPTOR_VALUE, interface_number, reply, SDL_XID_DESCRIPTOR_LENGTH,
                                SDL_XID_REQUEST_TIMEOUT_MS);
    if (received > 0 && received <= SDL_XID_DESCRIPTOR_LENGTH &&
        SDL_XID_ParseDescriptor(reply, (size_t)received, &descriptor)) {
        parsed = &descriptor;
    }
    if (!SDL_XID_Identify(vendor, product, parsed, product_string, &session->identity)) {
        return false;
    }
    XID_SetRest(session);

    if (session->identity.kind == SDL_XID_KIND_GAMEPAD) {
        memset(reply, 0, sizeof(reply));
        received = sink->control_in(sink->userdata, SDL_XID_REQUEST_TYPE_CLASS_IN, SDL_XID_REQUEST_GET_REPORT,
                                    SDL_XID_INPUT_REPORT_VALUE, interface_number, reply, SDL_XID_GAMEPAD_REPORT_LENGTH,
                                    SDL_XID_REQUEST_TIMEOUT_MS);
        if (received > 0 && received <= SDL_XID_GAMEPAD_REPORT_LENGTH) {
            XID_Apply(session, reply, (size_t)received);
        }
    }
    return true;
}

void SDL_XID_Start(SDL_XIDSession *session)
{
    if (session) {
        session->post_pending = true;
    }
}

bool SDL_XID_Update(SDL_XIDSession *session, const SDL_XIDSink *sink)
{
    uint8_t data[SDL_XID_READ_LENGTH];
    void *joystick;
    int size;

    if (!session || !sink) {
        return false;
    }
    joystick = sink->joystick(sink->userdata);
    if (session->post_pending && joystick) {
        XID_Post(session, joystick, sink);
    }
    while ((size = sink->read(sink->userdata, data, sizeof(data))) > 0) {
        if ((size_t)size > sizeof(data)) {
            size = (int)sizeof(data);
        }
        if (XID_Apply(session, data, (size_t)size) && joystick) {
            XID_Post(session, joystick, sink);
        }
    }
    return size == 0;
}

int SDL_XID_Rumble(SDL_XIDSession *session, uint16_t low, uint16_t high, const SDL_XIDSink *sink)
{
    uint8_t report[SDL_XID_RUMBLE_LENGTH];

    if (!session || !sink || session->identity.kind != SDL_XID_KIND_GAMEPAD) {
        return SDL_XID_OUTPUT_UNSUPPORTED;
    }
    SDL_XID_BuildRumble(low, high, report);
    return sink->write(sink->userdata, report, sizeof(report)) ? SDL_XID_OUTPUT_SENT : SDL_XID_OUTPUT_FAILED;
}

int SDL_XID_SendEffect(SDL_XIDSession *session, const uint8_t *effect, size_t length, const SDL_XIDSink *sink)
{
    uint8_t report[SDL_XID_SB_LAMP_WRITE_LENGTH];

    if (!session || !sink || session->identity.kind != SDL_XID_KIND_STEEL_BATTALION) {
        return SDL_XID_OUTPUT_UNSUPPORTED;
    }
    if (!SDL_XID_BuildLamps(effect, length, report)) {
        return SDL_XID_OUTPUT_INVALID;
    }
    return sink->write(sink->userdata, report, sizeof(report)) ? SDL_XID_OUTPUT_SENT : SDL_XID_OUTPUT_FAILED;
}
