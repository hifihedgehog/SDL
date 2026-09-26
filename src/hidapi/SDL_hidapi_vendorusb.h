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

/* The shared path for USB devices whose input has no usable HID interface
 * on Windows. Pure C99: no SDL runtime and no I/O, so every decision here
 * runs in the offline tests exactly as it runs in the library.
 *
 * libusb reaches these devices on Windows only after WinUSB is bound to the
 * interface. The application or its installer does the binding.
 */

#ifndef SDL_hidapi_vendorusb_h_
#define SDL_hidapi_vendorusb_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum SDL_VendorUSBPlatform
{
    SDL_VENDORUSB_PLATFORM_OTHER,
    SDL_VENDORUSB_PLATFORM_WINDOWS,
    SDL_VENDORUSB_PLATFORM_MACOS
} SDL_VendorUSBPlatform;

/* Match the interface by class, subclass and protocol, on any interface
 * number, instead of by interface number. */
#define SDL_VENDORUSB_MATCH_CLASS 0x01
/* Output reports carry no report ID, so a first byte of 0x00 is data. */
#define SDL_VENDORUSB_RAW_OUTPUT  0x02
/* The rule serves Windows only. Elsewhere the platform HID backend keeps
 * the device, and libusb treats the interface as it treats any other. */
#define SDL_VENDORUSB_WINDOWS_ONLY 0x04
/* With SDL_VENDORUSB_MATCH_CLASS: any protocol matches */
#define SDL_VENDORUSB_ANY_PROTOCOL 0x08
/* Input comes on an interrupt endpoint. With in_endpoint 0 the first
 * interrupt IN endpoint is taken and bulk IN endpoints are passed over. A
 * named in_endpoint must be an interrupt endpoint too, or nothing is
 * selected. */
#define SDL_VENDORUSB_IN_INTERRUPT 0x10

/* One vendor interface of one device. */
typedef struct SDL_VendorUSBRule
{
    uint16_t vendor;
    uint16_t product;
    uint8_t flags;
    uint8_t interface_number;   /* Without SDL_VENDORUSB_MATCH_CLASS */
    uint8_t interface_class;    /* With SDL_VENDORUSB_MATCH_CLASS */
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t alternate;    /* Selected on open. 0 keeps the default setting. */
    uint8_t in_endpoint;  /* 0 takes the first interrupt or bulk IN endpoint */
    uint8_t out_endpoint; /* 0 takes the first interrupt or bulk OUT endpoint, which is then optional */
    uint16_t in_size;     /* Required packet size, bits 0 to 10 of wMaxPacketSize, or 0 for any */
    uint16_t out_size;
    /* Bytes read per transfer on a bulk IN endpoint, or 0 to read
     * wMaxPacketSize as every other interface is read. A bulk transfer ends
     * at a short packet or a full buffer (USB 2.0 section 5.8.3), so one read
     * returns what the device sent up to its next short packet. The size must
     * be a whole number of packets, or the device's last packet can overrun
     * the read, which libusb reports as an overflow (libusb io.c, "Packets
     * and overflows") and WinUSB carries into the next read. Nothing is
     * selected when it is not. An interrupt IN endpoint ignores the size. Its
     * transfers end the same way but move their packets at the pipe's polling
     * interval (USB 2.0 section 5.7.3), so a longer read would hold each
     * full-size report back and return several at once. */
    uint16_t read_size;
} SDL_VendorUSBRule;

/* The rule for this interface, or NULL. */
extern const SDL_VendorUSBRule *SDL_VendorUSB_FindRule(uint16_t vendor, uint16_t product,
                                                       uint8_t interface_number, uint8_t interface_class,
                                                       uint8_t interface_subclass, uint8_t interface_protocol);

/* Whether any rule names this vendor and product. A device with a rule
 * enumerates only the interfaces its rules match. */
extern bool SDL_VendorUSB_IsVendorDevice(uint16_t vendor, uint16_t product);

/* Whether a rule serves this platform. NULL serves none. The libusb backend
 * applies a rule only where it serves. */
extern bool SDL_VendorUSB_RuleApplies(const SDL_VendorUSBRule *rule, SDL_VendorUSBPlatform platform);

/* The devices libusb serves on this platform. An ID on the list, an Xbox
 * interface on macOS and Windows, and any vendor interface. */
extern bool SDL_VendorUSB_RequiresLibUSB(SDL_VendorUSBPlatform platform, uint16_t vendor, uint16_t product,
                                         bool xbox, bool vendor_interface);

typedef struct SDL_VendorUSBRouting
{
    SDL_VendorUSBPlatform platform;
    bool libusb;           /* The libusb backend asks. false for the platform HID backend. */
    bool whitelist;        /* SDL_HINT_HIDAPI_LIBUSB_WHITELIST */
    bool gamecube;         /* SDL_HINT_HIDAPI_LIBUSB_GAMECUBE */
    uint16_t vendor;
    uint16_t product;
    bool xbox;             /* libusb only: an Xbox 360 or Xbox One interface */
    bool vendor_interface; /* libusb only: the interface matches a rule */
} SDL_VendorUSBRouting;

/* Whether the backend that asks leaves the device to the other backend. */
extern bool SDL_VendorUSB_Ignore(const SDL_VendorUSBRouting *routing);

/* Whether the libusb enumeration considers this interface before the ignore
 * rules: an interface a rule serving this platform matches, never another
 * interface of a device that has such a rule, and otherwise an Xbox or HID
 * interface. A rule that serves another platform leaves the device as if it
 * had none. */
extern bool SDL_VendorUSB_IsCandidate(SDL_VendorUSBPlatform platform, uint16_t vendor, uint16_t product,
                                      uint8_t interface_number, uint8_t interface_class,
                                      uint8_t interface_subclass, uint8_t interface_protocol, bool xbox);

typedef struct SDL_VendorUSBEndpointInfo
{
    uint8_t address;    /* bEndpointAddress */
    uint8_t attributes; /* bmAttributes */
} SDL_VendorUSBEndpointInfo;

/* An original Xbox XID interface: class 0x58, subclass 0x42, protocol 0, and
 * exactly two endpoints, one interrupt IN and one interrupt OUT. The class
 * alone admits it, whatever the vendor and product. The DVD remote's XID
 * interface has one endpoint and fails the test. */
extern bool SDL_VendorUSB_IsXIDInterface(uint8_t interface_class, uint8_t interface_subclass,
                                         uint8_t interface_protocol,
                                         const SDL_VendorUSBEndpointInfo *endpoints, int count);

/* Whether the libusb enumeration skips an interface it could not open.
 * On Windows an Xbox interface that xusb22 or the GIP driver holds cannot be
 * opened, and skipping it keeps a pad Windows serves from appearing twice.
 * One bound to WinUSB opens and is enumerated. */
extern bool SDL_VendorUSB_SkipUnopened(SDL_VendorUSBPlatform platform, bool xbox, bool opened);

#define SDL_VENDORUSB_TRANSFER_BULK      2
#define SDL_VENDORUSB_TRANSFER_INTERRUPT 3

typedef struct SDL_VendorUSBEndpoint
{
    uint8_t address;          /* 0 when absent */
    uint8_t transfer;         /* SDL_VENDORUSB_TRANSFER_BULK or _INTERRUPT */
    uint16_t max_packet_size; /* Bits 0 to 10 of wMaxPacketSize, which a rule's size matches */
    uint16_t read_size;       /* The bytes the backend reads per IN transfer: the rule's read_size
                                 on a bulk IN endpoint, otherwise wMaxPacketSize as the descriptor
                                 gives it, as the backend reads every other interface. */
} SDL_VendorUSBEndpoint;

typedef struct SDL_VendorUSBSelection
{
    uint8_t alternate;
    SDL_VendorUSBEndpoint in;
    SDL_VendorUSBEndpoint out;
} SDL_VendorUSBSelection;

/* Walks one interface's descriptors, every alternate setting in order, the
 * way a configuration descriptor carries them. Selects the rule's alternate
 * and its endpoints. Control and isochronous endpoints are never selected,
 * and bulk IN endpoints are not under SDL_VENDORUSB_IN_INTERRUPT. Fails on a
 * descriptor that is shorter than its type needs, has a length below 2, or
 * runs past the given length, when the alternate or a required endpoint is
 * missing, and when the rule's read_size is not a whole number of the bulk
 * IN endpoint's packets. Reads nothing at or past the given length. */
extern bool SDL_VendorUSB_SelectEndpoints(const SDL_VendorUSBRule *rule, uint8_t interface_number,
                                          const uint8_t *descriptors, size_t length,
                                          SDL_VendorUSBSelection *selection);

#endif /* SDL_hidapi_vendorusb_h_ */
