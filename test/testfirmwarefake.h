/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* The scripted controller the firmware version tests run a HIDAPI driver
   against, for hifihedgehog/SDL#42. A test includes this header, then the
   driver source, so every name through which the driver reads, writes,
   queues a write on the rumble thread, adds a joystick, looks one up, reads
   the clock or sleeps names the fake in testfirmwarefake.c. No HID device
   opens, the rumble thread never starts, and SDL_Delay returns at once with
   the fake clock moved on.

   Each test defines Firmware_Respond, which the fake calls with every
   report the driver writes, synchronously or through the rumble thread. It
   queues the controller's answers with Fake_Queue. */

#ifndef testfirmwarefake_h_
#define testfirmwarefake_h_

#include "SDL_internal.h"
#include "joystick/SDL_sysjoystick.h"
#include "joystick/hidapi/SDL_hidapijoystick_c.h"

#include <stdio.h>

extern int fake_checks;
extern int fake_failures;
extern const char *fake_scenario;

#define CHECK(condition, ...)                                          \
    do {                                                               \
        ++fake_checks;                                                 \
        if (!(condition)) {                                            \
            ++fake_failures;                                           \
            printf("FAILED line %d (%s): ", __LINE__, fake_scenario);  \
            printf(__VA_ARGS__);                                       \
            printf("\n");                                              \
        }                                                              \
    } while (0)

#define FAKE_MAX_REPORTS 64
#define FAKE_REPORT_SIZE 512

/* The device the driver runs on, its joystick record, and what the
   controller did */
extern SDL_HIDAPI_Device fake_device;
extern SDL_Joystick *fake_joystick;
extern int fake_writes;       /* Reports the driver wrote */
extern int fake_unexpected;   /* Requests the controller cannot answer */
extern Uint64 fake_now;       /* The clock SDL_GetTicks reads, in ms */

/* Each test defines this. async is true for a report the driver queued on
   the rumble thread. */
extern void Firmware_Respond(const Uint8 *data, int size, bool async);

extern SDL_hid_device *Fake_Handle(void);
extern void Fake_Reset(void);
extern void Fake_Queue(const Uint8 *report, int length);
extern void Fake_Unexpected(const char *what);

/* As the HIDAPI layer and SDL_OpenJoystick run a driver, under the joystick
   lock */
extern bool Fake_InitDevice(void);
extern void Fake_UpdateDevice(void);
extern bool Fake_OpenRecord(void);
extern void Fake_CloseRecord(void);
extern void Fake_FreeDevice(void);
extern int Fake_Finish(const char *name);

/* The names the driver calls through */
extern int Fake_hid_read_timeout(SDL_hid_device *dev, unsigned char *data, size_t length, int milliseconds);
extern int Fake_hid_write(SDL_hid_device *dev, const unsigned char *data, size_t length);
extern int Fake_hid_get_feature_report(SDL_hid_device *dev, unsigned char *data, size_t length);
extern int Fake_hid_get_report_descriptor(SDL_hid_device *dev, unsigned char *buf, size_t buf_size);
extern bool Fake_LockRumble(void);
extern void Fake_UnlockRumble(void);
extern int Fake_SendRumbleAndUnlock(SDL_HIDAPI_Device *device, const Uint8 *data, int size);
/* SDL_HIDAPI_RumbleSentCallback, spelled out, since SDL_hidapi_rumble.h
   comes only with the driver */
extern int Fake_SendRumbleWithCallbackAndUnlock(SDL_HIDAPI_Device *device, const Uint8 *data, int size, void (*callback)(void *userdata), void *userdata);
extern int Fake_SendRumble(SDL_HIDAPI_Device *device, const Uint8 *data, int size);
extern bool Fake_JoystickConnected(SDL_HIDAPI_Device *device, SDL_JoystickID *pJoystickID);
extern void Fake_JoystickDisconnected(SDL_HIDAPI_Device *device, SDL_JoystickID joystickID);
extern SDL_Joystick *Fake_GetJoystickFromID(SDL_JoystickID instance_id);
extern Uint64 Fake_GetTicks(void);
extern void Fake_Delay(Uint32 ms);

#ifndef TESTFIRMWAREFAKE_IMPLEMENTATION
/* SDL_hidapi_rumble.h has no include guard, so only the driver includes it,
   after these names: its declarations then name the fake. */
#undef SDL_hid_read_timeout
#define SDL_hid_read_timeout Fake_hid_read_timeout
#undef SDL_hid_write
#define SDL_hid_write Fake_hid_write
#undef SDL_hid_get_feature_report
#define SDL_hid_get_feature_report Fake_hid_get_feature_report
#undef SDL_hid_get_report_descriptor
#define SDL_hid_get_report_descriptor Fake_hid_get_report_descriptor
#define SDL_HIDAPI_LockRumble Fake_LockRumble
#define SDL_HIDAPI_UnlockRumble Fake_UnlockRumble
#define SDL_HIDAPI_SendRumbleAndUnlock Fake_SendRumbleAndUnlock
#define SDL_HIDAPI_SendRumbleWithCallbackAndUnlock Fake_SendRumbleWithCallbackAndUnlock
#define SDL_HIDAPI_SendRumble Fake_SendRumble
#define HIDAPI_JoystickConnected Fake_JoystickConnected
#define HIDAPI_JoystickDisconnected Fake_JoystickDisconnected
#undef SDL_GetJoystickFromID
#define SDL_GetJoystickFromID Fake_GetJoystickFromID
#undef SDL_GetTicks
#define SDL_GetTicks Fake_GetTicks
#undef SDL_Delay
#define SDL_Delay Fake_Delay
#endif

#endif /* testfirmwarefake_h_ */
