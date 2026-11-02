/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* The scripted XInput slots and GameInput that the XInput firmware test runs
   SDL's XInput driver and its firmware lookup against, for
   hifihedgehog/SDL#44. testxinputfirmware.c includes the driver source and
   testxinputfirmware_gameinput.cpp includes SDL_xinput_firmware.cpp, each
   after naming its own fakes for every call that would reach XInput,
   GameInput, the configuration manager or SDL's joystick list. */

#ifndef testxinputfirmware_h_
#define testxinputfirmware_h_

#include "SDL_internal.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int xf_checks;
extern int xf_failures;
extern const char *xf_scenario;

#define CHECK(condition, ...)                                        \
    do {                                                             \
        ++xf_checks;                                                 \
        if (!(condition)) {                                          \
            ++xf_failures;                                           \
            printf("FAILED line %d (%s): ", __LINE__, xf_scenario);  \
            printf(__VA_ARGS__);                                     \
            printf("\n");                                            \
        }                                                            \
    } while (0)

/* The scripted GameInput, in testxinputfirmware_gameinput.cpp. A device's
   connection and removal are queued, and FakeGI_Deliver runs the queue on a
   thread of its own, as GameInput runs its callbacks, and waits for it.
   Registering and unregistering wait for any running callback, as GameInput
   4.0.302 does. A registration lasts until unregistered, across resets. */
void FakeGI_Reset(bool available, bool register_fails);
bool FakeGI_WaitRegistered(int milliseconds);
bool FakeGI_WaitRegistrations(int count, int milliseconds);
/* Waits until the lookup's registering thread has finished */
bool FakeGI_WaitModuleSettled(int milliseconds);
/* Another client's callback, running in GameInput's dispatch and waiting on
   the joystick lock, as SDL's GameInput driver's device callback does */
void FakeGI_StartForeign(void);
void FakeGI_WaitForeign(void);
int FakeGI_AddDevice(const char *path, Uint16 major, Uint16 minor, Uint16 build, Uint16 revision);
void FakeGI_Disconnect(int device);
void FakeGI_Deliver(void);
/* Delivers count events for one device, alternating removal and connection,
   on a thread that runs while the caller goes on */
void FakeGI_StartFlood(int device, int count);
void FakeGI_WaitFlood(void);
/* The device instance the configuration manager gives for a path */
void FakeGI_SetInstance(const char *path, const char *instance);
int FakeGI_Inits(void);
int FakeGI_Quits(void);
int FakeGI_Registrations(void);
int FakeGI_Unregistrations(void);
bool FakeGI_Registered(void);
long FakeGI_References(int device); /* References the code under test holds */
int FakeGI_Unexpected(void);        /* Calls the firmware lookup has no use for */
int FakeGI_InstanceLookups(void);
int FakeGI_PlatformStringLookups(void);

#ifdef __cplusplus
}
#endif

#endif /* testxinputfirmware_h_ */
