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

/* Original Xbox XID devices behind a passive Xbox-to-USB cable: the Duke and
 * Controller S pads, the pads, wheels, dance pads, arcade sticks and light
 * guns that share the 20-byte gamepad report, and the Steel Battalion
 * controller with its 26-byte report and 37 lamps. Pure C99: no SDL runtime
 * and no I/O. The XID driver sends what this builds and posts what it
 * decodes.
 *
 * An XID interface is class 0x58, subclass 0x42, protocol 0, and its XID
 * descriptor, read with a vendor request, names the device type. The layouts
 * are facts from Linux xpad, tusb_xinput, JoypadOS, xemu, xboxdrv,
 * Xb2XInput, Cxbx-Reloaded, 360Controller, the USB Host Shield library,
 * XboxControllerAnalyser, ogx360, the Steel Battalion drivers and xboxdevwiki.
 * No code from any of them is copied.
 */

#ifndef SDL_hidapi_xid_proto_h_
#define SDL_hidapi_xid_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The XID interface: class, subclass and protocol
#define SDL_XID_INTERFACE_CLASS    0x58
#define SDL_XID_INTERFACE_SUBCLASS 0x42
#define SDL_XID_INTERFACE_PROTOCOL 0x00

#define SDL_XID_DESCRIPTOR_TYPE   0x42
#define SDL_XID_DESCRIPTOR_LENGTH 16

// bType
#define SDL_XID_TYPE_GAMEPAD         0x01
#define SDL_XID_TYPE_DVD_REMOTE      0x03
#define SDL_XID_TYPE_STEEL_BATTALION 0x80

// bSubType of the gamepad family
#define SDL_XID_SUBTYPE_DUKE          0x01
#define SDL_XID_SUBTYPE_CONTROLLER_S  0x02
#define SDL_XID_SUBTYPE_WHEEL         0x10
#define SDL_XID_SUBTYPE_ARCADE_STICK  0x20
#define SDL_XID_SUBTYPE_DIGITAL_STICK 0x21
#define SDL_XID_SUBTYPE_FLIGHT_STICK  0x30
#define SDL_XID_SUBTYPE_SNOWBOARD     0x40
#define SDL_XID_SUBTYPE_LIGHT_GUN     0x50
#define SDL_XID_SUBTYPE_RADIO_FLIGHT  0x60
#define SDL_XID_SUBTYPE_FISHING_ROD   0x70
#define SDL_XID_SUBTYPE_DANCE_PAD     0x80

// Control requests, each to the XID interface
#define SDL_XID_REQUEST_TYPE_VENDOR_IN 0xC1
#define SDL_XID_REQUEST_GET_DESCRIPTOR 0x06
#define SDL_XID_DESCRIPTOR_VALUE       0x4200
#define SDL_XID_REQUEST_TYPE_CLASS_IN  0xA1
#define SDL_XID_REQUEST_GET_REPORT     0x01
#define SDL_XID_INPUT_REPORT_VALUE     0x0100
#define SDL_XID_REQUEST_TIMEOUT_MS     100

#define SDL_XID_GAMEPAD_REPORT_LENGTH 20
#define SDL_XID_SB_REPORT_LENGTH      26
#define SDL_XID_READ_LENGTH           64 // one transfer: 32-byte endpoints, 64 on the EMS TopGun II
#define SDL_XID_RUMBLE_LENGTH         6
#define SDL_XID_SB_LAMP_EFFECT_LENGTH 22
#define SDL_XID_SB_LAMP_WRITE_LENGTH  32
#define SDL_XID_PRESS_THRESHOLD       0x20 // a face button is down above this pressure

// The values of SDL_JoystickType
#define SDL_XID_JOYSTICK_UNKNOWN      0
#define SDL_XID_JOYSTICK_GAMEPAD      1
#define SDL_XID_JOYSTICK_WHEEL        2
#define SDL_XID_JOYSTICK_ARCADE_STICK 3
#define SDL_XID_JOYSTICK_FLIGHT_STICK 4
#define SDL_XID_JOYSTICK_DANCE_PAD    5

// The values of SDL_HAT_*
#define SDL_XID_HAT_UP    0x01
#define SDL_XID_HAT_RIGHT 0x02
#define SDL_XID_HAT_DOWN  0x04
#define SDL_XID_HAT_LEFT  0x08

#define SDL_XID_KIND_NONE            0 // not served: the DVD remote
#define SDL_XID_KIND_GAMEPAD         1
#define SDL_XID_KIND_STEEL_BATTALION 2

// Per-ID flags, the ones Linux xpad keeps
#define SDL_XID_FLAG_DPAD_BUTTONS    0x01 // the D-pad is four independent arrows
#define SDL_XID_FLAG_TRIGGER_BUTTONS 0x02 // recorded only: the triggers stay axes
#define SDL_XID_FLAG_NULL_STICKS     0x04 // the sticks are not read
#define SDL_XID_FLAG_DANCE_PAD       (SDL_XID_FLAG_DPAD_BUTTONS | SDL_XID_FLAG_TRIGGER_BUTTONS | SDL_XID_FLAG_NULL_STICKS)

// GUID byte 15, which picks the gamepad mapping
#define SDL_XID_GUID_DANCE_PAD       0x80
#define SDL_XID_GUID_STEEL_BATTALION 0xFF

/* The gamepad family: axes 0-5 left X, left Y, right X, right Y, left and
 * right trigger, axes 6-11 the pressure of A, B, X, Y, White and Black.
 * Buttons 0-10 in SDL gamepad order, White the left shoulder and Black the
 * right. A dance pad adds its arrows as buttons 11-14 and has no hat. A light
 * gun adds "light visible" as button 11. */
#define SDL_XID_GAMEPAD_AXES      12
#define SDL_XID_GAMEPAD_BUTTONS   11
#define SDL_XID_BUTTON_EXTRA      11
#define SDL_XID_DANCE_BUTTONS     15
#define SDL_XID_LIGHT_GUN_BUTTONS 12

/* The Steel Battalion: axes aiming X and Y, rotation, sight X and Y, left,
 * middle and right pedal, tuner dial. Buttons 0-38 the report bits, 39-45
 * the gear R, N and 1 to 5. */
#define SDL_XID_SB_AXES        9
#define SDL_XID_SB_BUTTONS     46
#define SDL_XID_SB_GEAR_BUTTON 39

/* Gamepad mappings. Button 5, Guide, is never set, so neither names it. A
 * dance pad's arrows are buttons 11-14. The Steel Battalion has no mapping
 * and stays a joystick. */
#define SDL_XID_MAPPING_GAMEPAD   "a:b0,b:b1,back:b4,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b9,leftstick:b7,lefttrigger:a4,leftx:a0,lefty:a1,rightshoulder:b10,rightstick:b8,righttrigger:a5,rightx:a2,righty:a3,start:b6,x:b2,y:b3,"
#define SDL_XID_MAPPING_DANCE_PAD "a:b0,b:b1,back:b4,dpdown:b12,dpleft:b13,dpright:b14,dpup:b11,leftshoulder:b9,leftstick:b7,lefttrigger:a4,leftx:a0,lefty:a1,rightshoulder:b10,rightstick:b8,righttrigger:a5,rightx:a2,righty:a3,start:b6,x:b2,y:b3,"

typedef struct SDL_XIDDescriptor
{
    uint8_t type;
    uint8_t subtype;
    uint8_t max_input;
    uint8_t max_output;
} SDL_XIDDescriptor;

typedef struct SDL_XIDKnownDevice
{
    uint16_t vendor;
    uint16_t product;
    uint8_t flags;
    const char *name;
} SDL_XIDKnownDevice;

typedef struct SDL_XIDIdentity
{
    int kind;
    int joystick_type;
    bool dance;       // arrows as buttons 11-14, no hat
    bool light_gun;   // button 11 is "light visible"
    bool null_sticks; // the sticks read centered
    uint8_t guid_byte;
    const char *name;
    int naxes;
    int nbuttons;
    int nhats;
} SDL_XIDIdentity;

typedef struct SDL_XIDGamepadState
{
    uint16_t buttons;
    int16_t axes[SDL_XID_GAMEPAD_AXES];
    uint8_t hat;
} SDL_XIDGamepadState;

typedef struct SDL_XIDSteelBattalionState
{
    uint64_t buttons;
    int16_t axes[SDL_XID_SB_AXES];
} SDL_XIDSteelBattalionState;

/* Whether the XID driver serves an enumerated interface. The enumeration has
 * already required the two interrupt endpoints. */
extern bool SDL_XID_IsSupportedInterface(int interface_class, int interface_subclass, int interface_protocol);

/* The Steel Battalion controller's own ID, 0A7B:D000. Its hint governs this
 * ID. The descriptor decides the decoder on any ID. */
extern bool SDL_XID_IsSteelBattalionID(uint16_t vendor, uint16_t product);

/* The XID descriptor, valid only with at least 8 bytes, byte 1 0x42 and a
 * bLength of 8 or more that fits in what arrived. */
extern bool SDL_XID_ParseDescriptor(const uint8_t *data, size_t length, SDL_XIDDescriptor *out);

// The identity table: pads, wheels, dance pads and light guns by ID
extern const SDL_XIDKnownDevice *SDL_XID_FindKnownDevice(uint16_t vendor, uint16_t product);

/* Whether a HIDAPI GUID with this ID can only come from the XID drivers: an
 * ID in the identity table or the Steel Battalion's. No other driver serves
 * these IDs, so the mapping follows from the GUID whether or not the device
 * is connected. */
extern bool SDL_XID_IsKnownID(uint16_t vendor, uint16_t product);

/* What the device is, from its descriptor when one was read, otherwise from
 * the identity table. Returns false for a device that is not served. */
extern bool SDL_XID_Identify(uint16_t vendor, uint16_t product, const SDL_XIDDescriptor *descriptor,
                             const char *product_string, SDL_XIDIdentity *out);

// A 20-byte gamepad report: bytes 0-1 must be 00 14, and only bytes 0-19 are read
extern bool SDL_XID_DecodeGamepad(const SDL_XIDIdentity *identity, const uint8_t *report, size_t length,
                                  SDL_XIDGamepadState *out);

// A 26-byte Steel Battalion report: bytes 0-1 must be 00 1A, and only bytes 0-25 are read
extern bool SDL_XID_DecodeSteelBattalion(const uint8_t *report, size_t length, SDL_XIDSteelBattalionState *out);

// The rumble report: 00 06, then low and high as 16-bit little-endian, low on the left actuator
extern size_t SDL_XID_BuildRumble(uint16_t low, uint16_t high, uint8_t out[SDL_XID_RUMBLE_LENGTH]);

/* The lamp write: a 22-byte effect, one brightness nibble per lamp, becomes
 * a 32-byte write with bytes 0-1 set to 00 16. Any other size is refused. */
extern bool SDL_XID_BuildLamps(const uint8_t *effect, size_t length, uint8_t out[SDL_XID_SB_LAMP_WRITE_LENGTH]);

// The mapping for GUID byte 15, or NULL for the Steel Battalion
extern const char *SDL_XID_GetMapping(uint8_t guid_byte);

/* Where a session sends its requests and posts its state. Every callback is
 * required. */
typedef struct SDL_XIDSink
{
    void *userdata;
    /* One control transfer with a data stage from the device. Returns the
     * bytes received, or a negative value on a stall, a timeout or an error. */
    int (*control_in)(void *userdata, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                      uint8_t *data, uint16_t length, unsigned int timeout_ms);
    /* One interrupt IN transfer, without waiting. Returns the bytes read, 0
     * when nothing arrived, or a negative value when the device is gone. */
    int (*read)(void *userdata, uint8_t *data, size_t length);
    // Queues one report for the interrupt OUT endpoint. Returns false when it was not queued.
    bool (*write)(void *userdata, const uint8_t *data, size_t length);
    // The open joystick, or NULL while the application has not opened it
    void *(*joystick)(void *userdata);
    // The whole state, posted to the open joystick
    void (*gamepad)(void *userdata, void *joystick, const SDL_XIDGamepadState *state);
    void (*steel_battalion)(void *userdata, void *joystick, const SDL_XIDSteelBattalionState *state);
} SDL_XIDSink;

/* One XID device from open to removal. The state follows every report,
 * whether or not a joystick is open. A replugged device is a new session and
 * starts at rest. */
typedef struct SDL_XIDSession
{
    SDL_XIDIdentity identity;
    uint8_t interface_number;
    bool post_pending; // a joystick opened and has not been sent the state
    SDL_XIDGamepadState gamepad;
    SDL_XIDSteelBattalionState steel_battalion;
} SDL_XIDSession;

/* Reads the XID descriptor with one GET_DESCRIPTOR and identifies the device,
 * falling back to the ID tables on a stall, a timeout or a malformed reply.
 * The state starts at rest. The gamepad family then reads its current report
 * with one GET_REPORT, since the device sends a report only on a change.
 * Returns false for a device that is not served, the DVD remote. */
extern bool SDL_XID_Open(SDL_XIDSession *session, uint16_t vendor, uint16_t product, uint8_t interface_number,
                         const char *product_string, const SDL_XIDSink *sink);

// A joystick opened: the next update sends it the whole state
extern void SDL_XID_Start(SDL_XIDSession *session);

/* Applies every transfer waiting and posts the state to the open joystick.
 * Silence changes nothing. Returns false on a read error: the device is
 * gone. */
extern bool SDL_XID_Update(SDL_XIDSession *session, const SDL_XIDSink *sink);

#define SDL_XID_OUTPUT_SENT        0
#define SDL_XID_OUTPUT_UNSUPPORTED 1 // not this kind of device
#define SDL_XID_OUTPUT_INVALID     2 // an effect that is not 22 bytes
#define SDL_XID_OUTPUT_FAILED      3 // the write was not queued

// Rumble, the gamepad family only
extern int SDL_XID_Rumble(SDL_XIDSession *session, uint16_t low, uint16_t high, const SDL_XIDSink *sink);

// The Steel Battalion lamps: exactly 22 bytes
extern int SDL_XID_SendEffect(SDL_XIDSession *session, const uint8_t *effect, size_t length, const SDL_XIDSink *sink);

#endif // SDL_hidapi_xid_proto_h_
