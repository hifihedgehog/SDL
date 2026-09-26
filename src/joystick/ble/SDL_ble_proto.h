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

/* The module contract of the Bluetooth LE GATT joystick driver
 * (hifihedgehog/SDL#33 Part 12). A module turns one device's characteristic
 * values into controls, sensor samples and the writes the device needs.
 * Pure C99: no SDL runtime and no I/O. The session (SDL_ble_session.h) feeds
 * it values with their receive time, runs its timers, sends the writes it
 * queues one at a time and publishes the joystick once the module is ready.
 *
 * Every module state begins with an SDL_BLEBase. Rules for every module:
 * - Decode a value only when it has every byte the layout needs. Bytes past
 *   the layout are ignored.
 * - All timing comes from the clock arguments: milliseconds for timers,
 *   the receive time in nanoseconds for values.
 * - A gamepad-shaped device reports its buttons and axes at the
 *   SDL_GamepadButton and SDL_GamepadAxis indices. A trigger rests at -32768,
 *   as the joystick axis behind SDL's full-axis trigger binding does.
 */

#ifndef SDL_ble_proto_h_
#define SDL_ble_proto_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDL_BLE_MAX_AXES     6
#define SDL_BLE_MAX_BUTTONS  32
#define SDL_BLE_MAX_WRITE    80 /* bytes in one write, the Zwift key exchange the longest at 72 */
#define SDL_BLE_MAX_WRITES   16 /* writes a module can queue */
#define SDL_BLE_MAX_KEYS     4  /* discovery keys of one family */
#define SDL_BLE_MAX_SERVICES 4  /* services of one family */
#define SDL_BLE_MAX_CHARS    8  /* characteristics of one family */
#define SDL_BLE_NAME_LENGTH  64

/* SDL_GamepadButton and SDL_GamepadAxis values */
#define SDL_BLE_BUTTON_SOUTH          0
#define SDL_BLE_BUTTON_EAST           1
#define SDL_BLE_BUTTON_WEST           2
#define SDL_BLE_BUTTON_NORTH          3
#define SDL_BLE_BUTTON_BACK           4
#define SDL_BLE_BUTTON_GUIDE          5
#define SDL_BLE_BUTTON_START          6
#define SDL_BLE_BUTTON_LEFT_STICK     7
#define SDL_BLE_BUTTON_RIGHT_STICK    8
#define SDL_BLE_BUTTON_LEFT_SHOULDER  9
#define SDL_BLE_BUTTON_RIGHT_SHOULDER 10
#define SDL_BLE_BUTTON_DPAD_UP        11
#define SDL_BLE_BUTTON_DPAD_DOWN      12
#define SDL_BLE_BUTTON_DPAD_LEFT      13
#define SDL_BLE_BUTTON_DPAD_RIGHT     14

#define SDL_BLE_AXIS_LEFTX         0
#define SDL_BLE_AXIS_LEFTY         1
#define SDL_BLE_AXIS_RIGHTX        2
#define SDL_BLE_AXIS_RIGHTY        3
#define SDL_BLE_AXIS_LEFT_TRIGGER  4
#define SDL_BLE_AXIS_RIGHT_TRIGGER 5

/* SDL_JoystickType values */
#define SDL_BLE_TYPE_UNKNOWN 0
#define SDL_BLE_TYPE_GAMEPAD 1
#define SDL_BLE_TYPE_GUITAR  6

/* Sensor kinds */
#define SDL_BLE_SENSOR_ACCEL 1 /* meters per second squared */
#define SDL_BLE_SENSOR_GYRO  2 /* radians per second */

#define SDL_BLE_STANDARD_GRAVITY 9.80665f

/* GATT characteristic properties, numbered as the Bluetooth Core
   specification and Windows' GattCharacteristicProperties number them */
#define SDL_BLE_PROPERTY_READ                   0x02
#define SDL_BLE_PROPERTY_WRITE_WITHOUT_RESPONSE 0x04
#define SDL_BLE_PROPERTY_WRITE                  0x08
#define SDL_BLE_PROPERTY_NOTIFY                 0x10
#define SDL_BLE_PROPERTY_INDICATE               0x20

/* Characteristic flags */
#define SDL_BLE_CHAR_SUBSCRIBE 0x01 /* Notify or indicate, whichever the properties allow */
#define SDL_BLE_CHAR_READ      0x02 /* Read once after the subscriptions, delivered as a value */
#define SDL_BLE_CHAR_OPTIONAL  0x04 /* When missing, or when it cannot be subscribed, it is skipped */

/* A UUID in the order it is written, 0000fe55-0000-1000-8000-00805f9b34fb
   as 00 00 fe 55 00 00 10 00 80 00 00 80 5f 9b 34 fb */
typedef struct SDL_BLEUUID
{
    uint8_t bytes[16];
} SDL_BLEUUID;

/* Initializers for the tables */
#define SDL_BLE_UUID16_INIT(hi, lo) \
    { { 0x00, 0x00, hi, lo, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } }
#define SDL_BLE_UUID_INIT(b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15) \
    { { b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15 } }

typedef enum SDL_BLEKeyKind
{
    SDL_BLE_KEY_NAME_EQUALS,
    SDL_BLE_KEY_NAME_PREFIX,
    SDL_BLE_KEY_NAME_CONTAINS,
    SDL_BLE_KEY_SERVICE,
    SDL_BLE_KEY_MANUFACTURER
} SDL_BLEKeyKind;

/* One way to recognize a family in an advertisement or scan response */
typedef struct SDL_BLEMatchKey
{
    uint8_t kind;       /* SDL_BLEKeyKind */
    bool corroborating; /* Never matches alone */
    const char *name;   /* Name keys, compared case for case */
    SDL_BLEUUID uuid;   /* Service keys */
    uint16_t company;   /* Manufacturer keys */
    uint8_t ntypes;     /* Manufacturer keys: when not 0, the first data byte must be one of types */
    uint8_t types[4];
} SDL_BLEMatchKey;

typedef struct SDL_BLECharacteristic
{
    uint8_t service; /* Index into the family's services */
    SDL_BLEUUID uuid;
    uint8_t flags;   /* SDL_BLE_CHAR_* */
} SDL_BLECharacteristic;

typedef enum SDL_BLEPairing
{
    SDL_BLE_PAIR_NEVER,      /* Never pairs */
    SDL_BLE_PAIR_NOT_NEEDED, /* Works without a bond */
    SDL_BLE_PAIR_ON_ERROR,   /* Pairs when a subscription fails for want of authentication or encryption */
    SDL_BLE_PAIR_FIRST       /* Pairs before discovery unless Windows holds a bond */
} SDL_BLEPairing;

struct SDL_BLEModule;

/* One device family: how it is found, what is subscribed and who decodes it */
typedef struct SDL_BLEFamily
{
    const char *name; /* For the log */
    int nkeys;
    SDL_BLEMatchKey keys[SDL_BLE_MAX_KEYS];
    int nservices; /* The first is retried while the link comes up, as the Switch 2 driver does */
    SDL_BLEUUID services[SDL_BLE_MAX_SERVICES];
    bool has_alternate; /* The first service may carry this UUID instead, tried first */
    SDL_BLEUUID alternate;
    int ncharacteristics; /* In subscription order */
    SDL_BLECharacteristic characteristics[SDL_BLE_MAX_CHARS];
    uint8_t pairing; /* SDL_BLEPairing */
    const struct SDL_BLEModule *module;
} SDL_BLEFamily;

/* What the joystick is, fixed while it is published */
typedef struct SDL_BLEIdentity
{
    char name[SDL_BLE_NAME_LENGTH];
    uint8_t type;      /* SDL_BLE_TYPE_* */
    uint16_t vendor;   /* 0 when the device reports none */
    uint16_t product;
    uint32_t buttons;  /* The buttons it has, bit n for button n */
    uint8_t axes;      /* The axes it has, bit n for axis n */
    bool gamepad;      /* Buttons and axes sit at the SDL gamepad indices */
    bool touchpad;     /* One touchpad with one finger */
    float accel_rate;  /* Hz, 0 without an accelerometer */
    float gyro_rate;   /* Hz, 0 without a gyroscope */
} SDL_BLEIdentity;

typedef struct SDL_BLEControls
{
    int16_t axes[SDL_BLE_MAX_AXES];
    uint32_t buttons; /* Bit n is button n */
    bool finger;      /* Touchpad 0, finger 0, down */
    float finger_x;   /* 0 to 1 */
    float finger_y;
    int8_t battery;   /* Percent, -1 when unknown */
} SDL_BLEControls;

typedef struct SDL_BLEWrite
{
    uint8_t characteristic; /* Index into the family's characteristics */
    bool response;          /* Write with response when the characteristic allows it */
    uint8_t length;
    uint8_t data[SDL_BLE_MAX_WRITE];
} SDL_BLEWrite;

/* Receives every change a module commits and every sensor sample, in order */
typedef struct SDL_BLESink
{
    void *userdata;
    void (*changed)(void *userdata, const SDL_BLEControls *controls, uint64_t time_ns);
    void (*sensor)(void *userdata, int sensor, uint64_t time_ns, uint64_t sensor_ns, const float *data);
    void (*log)(void *userdata, const char *text);
} SDL_BLESink;

typedef struct SDL_BLEBase
{
    SDL_BLESink sink;
    SDL_BLEIdentity identity;
    SDL_BLEControls controls;
    SDL_BLEWrite writes[SDL_BLE_MAX_WRITES];
    int write_head;
    int write_count;
    bool ready;       /* The start-up is done and the joystick can appear */
    bool failed;      /* The module gives the connection up */
    bool resubscribe; /* Every subscription is to be written again */
    uint32_t changes; /* Counts every committed change */
    uint64_t emitted_ns; /* The time of the last change or sample handed to the sink */
} SDL_BLEBase;

/* What the session knows about the device when it connects */
typedef struct SDL_BLEModuleContext
{
    uint8_t variant;    /* The first manufacturer data byte of a manufacturer key match, else 0 */
    const void *crypto; /* A module-defined service, NULL when not built */
} SDL_BLEModuleContext;

/* The entry points. state points to the module's state, which begins with an
 * SDL_BLEBase. */
typedef struct SDL_BLEModule
{
    size_t state_size;
    /* Clear all state and set the identity. Called when the session starts. */
    void (*Reset)(void *state, const SDL_BLESink *sink, const SDL_BLEModuleContext *context);
    /* The subscriptions are in place: queue the start-up writes and set
       timers. The module sets ready with its first input after the start-up,
       and the joystick appears then. */
    void (*Start)(void *state, uint64_t now);
    /* One value of the characteristic with that index */
    void (*Value)(void *state, int characteristic, const uint8_t *data, size_t length, uint64_t time_ns);
    /* The oldest queued write has finished */
    void (*WriteDone)(void *state, bool success, uint64_t now);
    /* Run due timers */
    void (*Tick)(void *state, uint64_t now);
    /* Earliest clock value at which Tick must run. False when none */
    bool (*GetDeadline)(void *state, uint64_t *deadline);
    /* The connection closes on purpose: queue the closing writes */
    void (*Close)(void *state, uint64_t now);
} SDL_BLEModule;

/* A 16-bit UUID on the Bluetooth base UUID */
extern SDL_BLEUUID SDL_BLE_UUID16(uint16_t uuid);
extern bool SDL_BLE_UUIDEqual(const SDL_BLEUUID *a, const SDL_BLEUUID *b);

extern void SDL_BLE_SetIdentity(SDL_BLEIdentity *identity, const char *name, uint8_t type, uint32_t buttons, uint8_t axes, bool gamepad);
/* Controls at rest: no button, no finger, every axis 0 but a gamepad
   trigger, which rests at -32768. The battery is unknown. */
extern void SDL_BLE_RestControls(const SDL_BLEIdentity *identity, SDL_BLEControls *controls);

extern void SDL_BLE_SetButton(SDL_BLEControls *controls, int button, bool pressed);
extern bool SDL_BLE_GetButton(const SDL_BLEControls *controls, int button);
extern bool SDL_BLE_ControlsEqual(const SDL_BLEControls *a, const SDL_BLEControls *b);
extern int32_t SDL_BLE_Clamp(int32_t value, int32_t lo, int32_t hi);

/* Clears everything, sets the sink and the identity and puts the controls at rest */
extern void SDL_BLE_ResetBase(SDL_BLEBase *base, const SDL_BLESink *sink, const SDL_BLEIdentity *identity);
/* Replaces the controls, emitting when anything changed. The sink gets the
   later of time_ns and the time of the last change or sample it got, so
   its times never go back. */
extern void SDL_BLE_Commit(SDL_BLEBase *base, const SDL_BLEControls *controls, uint64_t time_ns);
/* Puts every control at rest, keeping the battery, emitting when anything changed */
extern void SDL_BLE_Release(SDL_BLEBase *base, uint64_t time_ns);
/* One sample, handed to the sink at once, with its time held as a change's
   is. sensor_ns, the device's own time, is passed as it is. */
extern void SDL_BLE_Sensor(SDL_BLEBase *base, int sensor, uint64_t time_ns, uint64_t sensor_ns, float x, float y, float z);

/* Queues a write. False, queuing nothing, when the queue is full or the data
   does not fit. */
extern bool SDL_BLE_QueueWrite(SDL_BLEBase *base, int characteristic, const uint8_t *data, size_t length, bool response);
/* Takes the oldest queued write. False when none. */
extern bool SDL_BLE_NextWrite(SDL_BLEBase *base, SDL_BLEWrite *write);
extern void SDL_BLE_ClearWrites(SDL_BLEBase *base);

extern void SDL_BLE_Log(SDL_BLEBase *base, const char *text);
/* Folds one candidate into a deadline */
extern void SDL_BLE_EarlierDeadline(bool *have, uint64_t *deadline, uint64_t candidate);

/* Entry points for modules without timers or closing writes */
extern void SDL_BLE_NoWriteDone(void *state, bool success, uint64_t now);
extern void SDL_BLE_NoTick(void *state, uint64_t now);
extern bool SDL_BLE_NoDeadline(void *state, uint64_t *deadline);
extern void SDL_BLE_NoClose(void *state, uint64_t now);

#endif /* SDL_ble_proto_h_ */
