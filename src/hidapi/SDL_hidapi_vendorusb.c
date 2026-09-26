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

#include "SDL_hidapi_vendorusb.h"

#include "../joystick/usb_ids.h"

#define VENDORUSB_ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))

/* Vendor interfaces. Every member here has no usable HID interface on
 * Windows, so libusb serves it there once WinUSB is bound.
 */
static const SDL_VendorUSBRule SDL_vendorusb_rules[] = {
    /* The Wii U GameCube adapter. Its interface is HID class, but Windows
     * gives no usable input through it, and the driver uses libusb. */
    { USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_GAMECUBE_ADAPTER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },

    /* The Xbox 360 Big Button receiver has no HID interface. Its one vendor
     * interface is class 0xFF, subclass 0x5D, protocol 4. */
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER, SDL_VENDORUSB_MATCH_CLASS, 0, 0xFF, 0x5D, 0x04, 0, 0, 0, 0, 0 },

    /* The Intel Wireless Series base station. Windows binds its keyboard
     * driver to interface 0 at alternate 0, and the pads report only on
     * alternate 1: interrupt IN 0x81 of 27 bytes and interrupt OUT 0x01 of
     * 25 bytes. Endpoint 0x02 of that alternate is a control endpoint and is
     * never opened. Interface 1, the boot mouse, is not enumerated. */
    { USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES, SDL_VENDORUSB_RAW_OUTPUT, 0, 0, 0, 0, 1, 0x81, 0x01, 27, 25 },

    /* The Gametrak for PlayStation. It stays silent until the host writes
     * "Gametrak" and then key bytes as output reports whose first byte is a
     * report ID its collection does not declare, which hid.dll refuses. On
     * Windows libusb reads it once WinUSB is bound: interrupt IN 0x81, no OUT
     * endpoint, so each write goes out as SET_REPORT with the first byte in
     * wValue, as Linux sends it. Linux and macOS send those writes through
     * their HID backends. */
    { USB_VENDOR_IN2GAMES, USB_PRODUCT_IN2GAMES_GAMETRAK, SDL_VENDORUSB_WINDOWS_ONLY, 0, 0, 0, 0, 0, 0, 0, 0, 0 },

    /* The DJI RC (RM330). Its configuration is MTP, a DUML bulk interface
     * and ADB. The bulk interface is class 0xFF, subclass 0x43, the way
     * dji-firmware-tools and DJI-RC-Emulator find it, whatever its number
     * or protocol: bulk IN 0x83 and bulk OUT 0x02. The DUML frames go out
     * unchanged. */
    { USB_VENDOR_DJI, USB_PRODUCT_DJI_RC_RM330, SDL_VENDORUSB_MATCH_CLASS | SDL_VENDORUSB_ANY_PROTOCOL | SDL_VENDORUSB_RAW_OUTPUT, 0, 0xFF, 0x43, 0, 0, 0x83, 0x02, 0, 0 },

    /* I-Force wheels and joysticks. Linux's iforce driver matches these IDs
     * whatever the interface class, and requires the interface's first
     * endpoint to be interrupt IN and its second interrupt OUT. The rule
     * takes interface 0's first IN and first OUT endpoint, and commands go
     * out unchanged. On Linux that driver serves them. */
#define SDL_VENDORUSB_IFORCE(vendor, product) { vendor, product, SDL_VENDORUSB_RAW_OUTPUT | SDL_VENDORUSB_WINDOWS_ONLY, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
    SDL_VENDORUSB_IFORCE(USB_VENDOR_THRUSTMASTER, USB_PRODUCT_THRUSTMASTER_MOTOR_SPORT_GT),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_LOGITECH, USB_PRODUCT_LOGITECH_WINGMAN_FORCE),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_LOGITECH, USB_PRODUCT_LOGITECH_WINGMAN_FORMULA_FORCE),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_AVB, USB_PRODUCT_AVB_TOP_SHOT_PEGASUS),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_AVB, USB_PRODUCT_AVB_MAG_TURBO_FORCE),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_AVB, USB_PRODUCT_AVB_TOP_SHOT_FFB_WHEEL),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_ACTLABS, USB_PRODUCT_ACTLABS_FORCE_RS_C084),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_ACTLABS, USB_PRODUCT_ACTLABS_FORCE_RS_C094),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_ACTLABS, USB_PRODUCT_ACTLABS_FORCE_RS_C0A4),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_SAITEK, USB_PRODUCT_SAITEK_R440_FORCE_WHEEL),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_GUILLEMOT, USB_PRODUCT_GUILLEMOT_RACE_LEADER_FFB),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_GUILLEMOT, USB_PRODUCT_GUILLEMOT_JET_LEADER_FFB),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_GUILLEMOT, USB_PRODUCT_GUILLEMOT_FFB_RACING_WHEEL),
    SDL_VENDORUSB_IFORCE(USB_VENDOR_GUILLEMOT, USB_PRODUCT_GUILLEMOT_JET_LEADER_3D),
#undef SDL_VENDORUSB_IFORCE

    /* The Namco GunCon 2, and the EMS LCD TopGun that shares its ID: interface
     * 0, class FF, with one interrupt IN endpoint and no OUT endpoint, so the
     * mode request goes out as SET_REPORT on the control pipe. No platform
     * has a driver for it, so libusb serves it everywhere. */
    { USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_GUNCON2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },

    /* Train controllers: interface 0 with one interrupt IN endpoint and no
     * HID class descriptor, class 03 on the Taito units and 00 on the
     * others. Every output is a vendor control transfer. No platform has a
     * driver for them, so libusb serves them everywhere. */
    { USB_VENDOR_TAITO, USB_PRODUCT_TAITO_DENSHA_TYPE2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { USB_VENDOR_TAITO, USB_PRODUCT_TAITO_DENSHA_SHINKANSEN, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { USB_VENDOR_TAITO, USB_PRODUCT_TAITO_DENSHA_RYOJOHEN, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { USB_VENDOR_TAITO, USB_PRODUCT_TAITO_MULTI_TRAIN_CONTROLLER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { USB_VENDOR_TRAIN_MASCON, USB_PRODUCT_TRAIN_MASCON, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },

    /* The Namco USIO and the H050 USJ(C): interface 0 with bulk OUT 0x01,
     * bulk IN 0x82 and interrupt IN 0x83, as the emulators describe it
     * (rpcs3/Emu/Io/usio.cpp:93-122). The command protocol runs on the two
     * bulk endpoints, and 0x83 is never read. Byte 0 of every command is its
     * operation, never a report ID, so commands go out unchanged. Like every
     * rule of Part 14 it serves Windows only, the platform the part covers. */
    { USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_USIO, SDL_VENDORUSB_RAW_OUTPUT | SDL_VENDORUSB_WINDOWS_ONLY, 0, 0, 0, 0, 0, 0x82, 0x01, 0, 0 },
    { USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_H050_USJC, SDL_VENDORUSB_RAW_OUTPUT | SDL_VENDORUSB_WINDOWS_ONLY, 0, 0, 0, 0, 0, 0x82, 0x01, 0, 0 },

    /* Konami's P3IO of the DDR SuperNova 2 and DDR X cabinets. The inputs
     * come on interrupt IN 0x83 of interface 0, which the rule names because
     * bulk IN 0x81 carries the replies to the board's commands. The driver
     * sends those on bulk OUT 0x02 and reads 0x81 itself on the libusb
     * handle. Like every rule of Part 14 it serves Windows only. */
    { USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P3IO, SDL_VENDORUSB_WINDOWS_ONLY, 0, 0, 0, 0, 0, 0x83, 0, 0, 0 },

    /* Konami's P4IO: interface 0 with bulk OUT, bulk IN and interrupt IN in
     * that order, as p4io-mdxfdrv reads it, and no source records the
     * addresses. The input reports come on the interrupt endpoint. Bulk IN
     * carries the replies to commands, which the driver reads itself. Like
     * every rule of Part 14 it serves Windows only. */
    { USB_VENDOR_KONAMI, USB_PRODUCT_KONAMI_P4IO, SDL_VENDORUSB_IN_INTERRUPT | SDL_VENDORUSB_WINDOWS_ONLY, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },

    /* The CH Products Multi-Function Panel: its only interface, 0, is class
     * FF with one interrupt IN endpoint, 0x81 of 8 bytes, and no OUT
     * endpoint. The host never writes to it. Like every rule of Part 14 it
     * serves Windows only. */
    { USB_VENDOR_CH_PRODUCTS, USB_PRODUCT_CH_PRODUCTS_MFP, SDL_VENDORUSB_WINDOWS_ONLY, 0, 0, 0, 0, 0, 0x81, 0, 8, 0 },

    /* The Ergodex DX1. Interface 0 is a boot keyboard and is not enumerated.
     * Interface 1 is class FF, subclass 00, protocol 01: interrupt IN 0x82
     * and interrupt OUT 0x02, 16 bytes each. Byte 0 of every request is its
     * type, never a report ID, so requests go out unchanged. Like every rule
     * of Part 14 it serves Windows only. */
    { USB_VENDOR_ERGODEX, USB_PRODUCT_ERGODEX_DX1, SDL_VENDORUSB_RAW_OUTPUT | SDL_VENDORUSB_WINDOWS_ONLY, 1, 0, 0, 0, 0, 0x82, 0x02, 16, 16 },

    /* The NaturalPoint TrackIR 2 and TrackIR 3: interface 0 with commands on
     * bulk OUT 0x02 and a stream of packets on bulk IN 0x82. A read takes
     * 16384 bytes, as linuxtrack reads them, so it holds every packet the
     * camera sent up to its next short packet. The commands go out
     * unchanged. Like every rule of Part 14 they serve Windows only, and
     * elsewhere linuxtrack keeps the cameras. */
    { USB_VENDOR_NATURALPOINT, USB_PRODUCT_NATURALPOINT_TRACKIR2, SDL_VENDORUSB_RAW_OUTPUT | SDL_VENDORUSB_WINDOWS_ONLY, 0, 0, 0, 0, 0, 0x82, 0x02, 0, 0, 16384 },
    { USB_VENDOR_NATURALPOINT, USB_PRODUCT_NATURALPOINT_TRACKIR3, SDL_VENDORUSB_RAW_OUTPUT | SDL_VENDORUSB_WINDOWS_ONLY, 0, 0, 0, 0, 0, 0x82, 0x02, 0, 0, 16384 },

    /* Tacx trainer head units, the T1904 and the T1932. Interface 0 takes
     * frames on OUT 0x02 and answers on IN 0x82, the endpoints FortiusANT,
     * antifier and the fortius_1942 Linux driver use. No source records the
     * class, and the descriptors give the transfer type. A reply is 64 bytes,
     * and a bulk IN endpoint is read 64 bytes at a time, as every source
     * reads it, so a reply spread over smaller packets still arrives whole.
     * The first byte of a frame is a command number, never a report ID. Like
     * every rule of Part 14 they serve Windows only, and elsewhere FortiusANT
     * and the Linux driver keep the head units. */
#define SDL_VENDORUSB_TACX(product) { USB_VENDOR_TACX, product, SDL_VENDORUSB_RAW_OUTPUT | SDL_VENDORUSB_WINDOWS_ONLY, 0, 0, 0, 0, 0, 0x82, 0x02, 0, 0, 64 }
    SDL_VENDORUSB_TACX(USB_PRODUCT_TACX_T1904),
    SDL_VENDORUSB_TACX(USB_PRODUCT_TACX_T1932),
#undef SDL_VENDORUSB_TACX
};

/* Devices that need libusb on every platform. The Switch 2 devices carry
 * their input on a HID interface, so on Windows the platform backend reads
 * them and their drivers open WinUSB separately for bulk I/O. */
static const struct
{
    uint16_t vendor;
    uint16_t product;
} SDL_vendorusb_libusb_required[] = {
    { USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_GAMECUBE_ADAPTER },
    { USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_GAMECUBE_CONTROLLER },
    { USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_LEFT },
    { USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_JOYCON_RIGHT },
    { USB_VENDOR_NINTENDO, USB_PRODUCT_NINTENDO_SWITCH2_PRO },
    { USB_VENDOR_MICROSOFT, USB_PRODUCT_XBOX360_BIGBUTTON_RECEIVER },
    { USB_VENDOR_INTEL, USB_PRODUCT_INTEL_WIRELESS_SERIES },
    { USB_VENDOR_NAMCO, USB_PRODUCT_NAMCO_GUNCON2 },
    { USB_VENDOR_TAITO, USB_PRODUCT_TAITO_DENSHA_TYPE2 },
    { USB_VENDOR_TAITO, USB_PRODUCT_TAITO_DENSHA_SHINKANSEN },
    { USB_VENDOR_TAITO, USB_PRODUCT_TAITO_DENSHA_RYOJOHEN },
    { USB_VENDOR_TAITO, USB_PRODUCT_TAITO_MULTI_TRAIN_CONTROLLER },
    { USB_VENDOR_TRAIN_MASCON, USB_PRODUCT_TRAIN_MASCON },
};

const SDL_VendorUSBRule *SDL_VendorUSB_FindRule(uint16_t vendor, uint16_t product,
                                                uint8_t interface_number, uint8_t interface_class,
                                                uint8_t interface_subclass, uint8_t interface_protocol)
{
    size_t i;

    for (i = 0; i < VENDORUSB_ARRAYSIZE(SDL_vendorusb_rules); ++i) {
        const SDL_VendorUSBRule *rule = &SDL_vendorusb_rules[i];
        if (rule->vendor != vendor || rule->product != product) {
            continue;
        }
        if (rule->flags & SDL_VENDORUSB_MATCH_CLASS) {
            if (rule->interface_class == interface_class &&
                rule->interface_subclass == interface_subclass &&
                ((rule->flags & SDL_VENDORUSB_ANY_PROTOCOL) || rule->interface_protocol == interface_protocol)) {
                return rule;
            }
        } else if (rule->interface_number == interface_number) {
            return rule;
        }
    }
    return NULL;
}

bool SDL_VendorUSB_IsVendorDevice(uint16_t vendor, uint16_t product)
{
    size_t i;

    for (i = 0; i < VENDORUSB_ARRAYSIZE(SDL_vendorusb_rules); ++i) {
        if (SDL_vendorusb_rules[i].vendor == vendor && SDL_vendorusb_rules[i].product == product) {
            return true;
        }
    }
    return false;
}

bool SDL_VendorUSB_RuleApplies(const SDL_VendorUSBRule *rule, SDL_VendorUSBPlatform platform)
{
    return rule && (!(rule->flags & SDL_VENDORUSB_WINDOWS_ONLY) || platform == SDL_VENDORUSB_PLATFORM_WINDOWS);
}

bool SDL_VendorUSB_RequiresLibUSB(SDL_VendorUSBPlatform platform, uint16_t vendor, uint16_t product,
                                  bool xbox, bool vendor_interface)
{
    size_t i;

    for (i = 0; i < VENDORUSB_ARRAYSIZE(SDL_vendorusb_libusb_required); ++i) {
        if (SDL_vendorusb_libusb_required[i].vendor == vendor &&
            SDL_vendorusb_libusb_required[i].product == product) {
            return true;
        }
    }
    if (vendor_interface) {
        return true;
    }
    /* On macOS and Windows an Xbox interface the system serves cannot be
     * opened through libusb, so any Xbox interface libusb can open is one
     * the system does not serve. */
    if (xbox && (platform == SDL_VENDORUSB_PLATFORM_MACOS || platform == SDL_VENDORUSB_PLATFORM_WINDOWS)) {
        return true;
    }
    return false;
}

bool SDL_VendorUSB_Ignore(const SDL_VendorUSBRouting *routing)
{
    const bool gamecube_adapter = (routing->vendor == USB_VENDOR_NINTENDO &&
                                   routing->product == USB_PRODUCT_NINTENDO_GAMECUBE_ADAPTER);

    if (routing->libusb) {
        const bool required = SDL_VendorUSB_RequiresLibUSB(routing->platform, routing->vendor, routing->product,
                                                           routing->xbox, routing->vendor_interface);
        if (routing->whitelist && !required) {
            return true;
        }
        if (!routing->gamecube && gamecube_adapter) {
            return true;
        }
        /* On Windows libusb cannot claim a HID interface that the Windows
         * HID driver holds, so a device on the list that is neither a vendor
         * interface nor an Xbox interface, such as a Switch 2 controller,
         * belongs to the platform backend. */
        if (routing->platform == SDL_VENDORUSB_PLATFORM_WINDOWS &&
            required && !routing->vendor_interface && !routing->xbox) {
            return true;
        }
        return false;
    }

    if (routing->platform == SDL_VENDORUSB_PLATFORM_WINDOWS) {
        /* A vendor device belongs to libusb. Only the rest of the list stays
         * with the platform backend on Windows. */
        return SDL_VendorUSB_IsVendorDevice(routing->vendor, routing->product);
    }
    return SDL_VendorUSB_RequiresLibUSB(routing->platform, routing->vendor, routing->product, false, false);
}

bool SDL_VendorUSB_IsCandidate(SDL_VendorUSBPlatform platform, uint16_t vendor, uint16_t product,
                               uint8_t interface_number, uint8_t interface_class,
                               uint8_t interface_subclass, uint8_t interface_protocol, bool xbox)
{
    size_t i;

    if (SDL_VendorUSB_RuleApplies(SDL_VendorUSB_FindRule(vendor, product, interface_number, interface_class,
                                                         interface_subclass, interface_protocol),
                                  platform)) {
        return true;
    }
    for (i = 0; i < VENDORUSB_ARRAYSIZE(SDL_vendorusb_rules); ++i) {
        const SDL_VendorUSBRule *rule = &SDL_vendorusb_rules[i];

        if (rule->vendor == vendor && rule->product == product && SDL_VendorUSB_RuleApplies(rule, platform)) {
            return false;
        }
    }
    return xbox || interface_class == 0x03; /* HID */
}

bool SDL_VendorUSB_IsXIDInterface(uint8_t interface_class, uint8_t interface_subclass,
                                  uint8_t interface_protocol,
                                  const SDL_VendorUSBEndpointInfo *endpoints, int count)
{
    int i, in = 0, out = 0;

    /* Linux xpad matches the class, subclass and protocol and requires two
       endpoints, both interrupt, one in each direction. */
    if (interface_class != 0x58 || interface_subclass != 0x42 || interface_protocol != 0x00) {
        return false;
    }
    if (count != 2 || !endpoints) {
        return false;
    }
    for (i = 0; i < count; ++i) {
        if ((endpoints[i].attributes & 0x03) != SDL_VENDORUSB_TRANSFER_INTERRUPT) {
            return false;
        }
        if (endpoints[i].address & 0x80) {
            ++in;
        } else {
            ++out;
        }
    }
    return in == 1 && out == 1;
}

bool SDL_VendorUSB_SkipUnopened(SDL_VendorUSBPlatform platform, bool xbox, bool opened)
{
    return platform == SDL_VENDORUSB_PLATFORM_WINDOWS && xbox && !opened;
}

#define VENDORUSB_DT_INTERFACE 0x04
#define VENDORUSB_DT_ENDPOINT  0x05

static void VendorUSB_TakeEndpoint(uint8_t wanted, uint16_t wanted_size, uint8_t address,
                                   uint8_t transfer, uint16_t w_max_packet_size, SDL_VendorUSBEndpoint *endpoint)
{
    /* Bits 11 and 12 count the extra transactions of a high-speed
     * high-bandwidth endpoint. The packet size is below them. */
    const uint16_t max_packet_size = (uint16_t)(w_max_packet_size & 0x07FF);

    if (endpoint->address) {
        return; /* The first match wins */
    }
    if (wanted && address != wanted) {
        return;
    }
    if (wanted_size && max_packet_size != wanted_size) {
        return;
    }
    endpoint->address = address;
    endpoint->transfer = transfer;
    endpoint->max_packet_size = max_packet_size;
    endpoint->read_size = w_max_packet_size;
}

bool SDL_VendorUSB_SelectEndpoints(const SDL_VendorUSBRule *rule, uint8_t interface_number,
                                   const uint8_t *descriptors, size_t length,
                                   SDL_VendorUSBSelection *selection)
{
    SDL_VendorUSBSelection result;
    bool found_alternate = false;
    bool in_alternate = false;
    size_t position = 0;

    if (!rule || !selection || (length && !descriptors)) {
        return false;
    }

    result.alternate = rule->alternate;
    result.in.address = result.in.transfer = 0;
    result.in.max_packet_size = result.in.read_size = 0;
    result.out = result.in;

    while (position < length) {
        const size_t remaining = length - position;
        uint8_t descriptor_length;
        uint8_t type;

        if (remaining < 2) {
            return false;
        }
        descriptor_length = descriptors[position];
        type = descriptors[position + 1];
        if (descriptor_length < 2 || descriptor_length > remaining) {
            return false;
        }

        if (type == VENDORUSB_DT_INTERFACE) {
            if (descriptor_length < 9) {
                return false;
            }
            in_alternate = (descriptors[position + 2] == interface_number &&
                            descriptors[position + 3] == rule->alternate);
            if (in_alternate) {
                found_alternate = true;
            }
        } else if (type == VENDORUSB_DT_ENDPOINT) {
            uint8_t address, transfer;
            uint16_t w_max_packet_size;

            if (descriptor_length < 7) {
                return false;
            }
            address = descriptors[position + 2];
            transfer = (uint8_t)(descriptors[position + 3] & 0x03);
            w_max_packet_size = (uint16_t)(descriptors[position + 4] | (descriptors[position + 5] << 8));

            if (in_alternate &&
                (transfer == SDL_VENDORUSB_TRANSFER_BULK || transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT)) {
                if (address & 0x80) {
                    /* A rule that reads an interrupt endpoint passes over bulk ones */
                    if (transfer == SDL_VENDORUSB_TRANSFER_INTERRUPT || !(rule->flags & SDL_VENDORUSB_IN_INTERRUPT)) {
                        VendorUSB_TakeEndpoint(rule->in_endpoint, rule->in_size, address, transfer, w_max_packet_size, &result.in);
                    }
                } else {
                    VendorUSB_TakeEndpoint(rule->out_endpoint, rule->out_size, address, transfer, w_max_packet_size, &result.out);
                }
            }
        }
        position += descriptor_length;
    }

    if (!found_alternate || !result.in.address) {
        return false;
    }
    if (rule->out_endpoint && !result.out.address) {
        return false;
    }
    if (rule->read_size && result.in.transfer == SDL_VENDORUSB_TRANSFER_BULK) {
        /* A whole number of packets, so the device's last packet always fits */
        if (!result.in.max_packet_size || (rule->read_size % result.in.max_packet_size) != 0) {
            return false;
        }
        result.in.read_size = rule->read_size;
    }
    *selection = result;
    return true;
}
