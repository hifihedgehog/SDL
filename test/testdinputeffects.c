/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL's DirectInput haptic effect creation and update against a
   scripted DirectInput device, for hifihedgehog/SDL#48. The file includes
   SDL_dinputhaptic.c and gives SDL_DINPUT_HapticNewEffect and
   SDL_DINPUT_HapticUpdateEffect a device whose CreateEffect, and an effect
   whose SetParameters, record the DIEFFECT SDL hands them. DIEFFECT keeps
   its duration, start delay and trigger repeat interval in microseconds, and
   INFINITE is its duration with no end, except for a ramp force, whose
   duration cannot be INFINITE (Microsoft's DIEFFECT, DIRAMPFORCE and Effect
   Playback pages). The haptic subsystem never starts and no device opens.

   1. SDL_HapticLengthToMicroseconds: a length's thousandfold, INFINITE for
      SDL_HAPTIC_INFINITY, and 4294967000 for every length too long for 32
      bits, SDL_HAPTIC_INFINITY included where the effect cannot be infinite.
   2. A constant, periodic, condition and custom effect created with
      SDL_HAPTIC_INFINITY reaches CreateEffect with an INFINITE duration, and
      a ramp with the longest finite one.
   3. Finite lengths, the longest that fits and those past it, for every
      kind of effect.
   4. The trigger repeat interval and the start delay reach DirectInput in
      microseconds.
   5. An update from a finite length to SDL_HAPTIC_INFINITY sends
      DIEP_DURATION with INFINITE, and an update that keeps the infinite
      length and changes the level sends no DIEP_DURATION. */

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"

#include "../src/haptic/windows/SDL_dinputhaptic.c"

#include <stdio.h>

static int checks;
static int failures;
static const char *scenario = "";

#define CHECK(condition, ...)                                    \
    do {                                                         \
        ++checks;                                                \
        if (!(condition)) {                                      \
            ++failures;                                          \
            printf("FAILED line %d (%s): ", __LINE__, scenario); \
            printf(__VA_ARGS__);                                 \
            printf("\n");                                        \
        }                                                        \
    } while (0)

#define LONGEST_US 4294967000UL

// What the scripted device and effect were last handed
static struct
{
    int creates;
    int updates;
    DWORD duration;
    DWORD interval;
    DWORD delay;
    DWORD flags;
} seen;

static HRESULT STDMETHODCALLTYPE Fake_SetParameters(IDirectInputEffect *self, LPCDIEFFECT effect, DWORD flags)
{
    (void)self;
    ++seen.updates;
    seen.duration = effect->dwDuration;
    seen.interval = effect->dwTriggerRepeatInterval;
    seen.delay = effect->dwStartDelay;
    seen.flags = flags;
    return DI_OK;
}

static IDirectInputEffectVtbl effect_vtbl;
static IDirectInputEffect fake_effect;

static HRESULT STDMETHODCALLTYPE Fake_CreateEffect(IDirectInputDevice8 *self, REFGUID guid, LPCDIEFFECT effect,
                                                   LPDIRECTINPUTEFFECT *created, LPUNKNOWN outer)
{
    (void)self;
    (void)guid;
    (void)outer;
    ++seen.creates;
    seen.duration = effect->dwDuration;
    seen.interval = effect->dwTriggerRepeatInterval;
    seen.delay = effect->dwStartDelay;
    *created = &fake_effect;
    return DI_OK;
}

static IDirectInputDevice8Vtbl device_vtbl;
static IDirectInputDevice8 fake_device;
static struct haptic_hwdata hwdata;
static SDL_Haptic haptic;

static Uint16 custom_data[2] = { 0, 1000 };

// An effect of the type, steering on one axis, with the length, interval and delay
static SDL_HapticEffect Effect(Uint16 type, Uint32 length, Uint16 interval, Uint16 delay)
{
    SDL_HapticEffect effect;

    SDL_zero(effect);
    effect.type = type;
    switch (type) {
    case SDL_HAPTIC_CONSTANT:
        effect.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
        effect.constant.length = length;
        effect.constant.interval = interval;
        effect.constant.delay = delay;
        effect.constant.level = 0x2000;
        break;
    case SDL_HAPTIC_SINE:
        effect.periodic.direction.type = SDL_HAPTIC_STEERING_AXIS;
        effect.periodic.length = length;
        effect.periodic.interval = interval;
        effect.periodic.delay = delay;
        effect.periodic.period = 100;
        effect.periodic.magnitude = 0x2000;
        break;
    case SDL_HAPTIC_SPRING:
        effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
        effect.condition.length = length;
        effect.condition.interval = interval;
        effect.condition.delay = delay;
        effect.condition.right_sat[0] = 0xFFFF;
        effect.condition.left_sat[0] = 0xFFFF;
        effect.condition.right_coeff[0] = 0x2000;
        effect.condition.left_coeff[0] = 0x2000;
        break;
    case SDL_HAPTIC_RAMP:
        effect.ramp.direction.type = SDL_HAPTIC_STEERING_AXIS;
        effect.ramp.length = length;
        effect.ramp.interval = interval;
        effect.ramp.delay = delay;
        effect.ramp.start = 0x1000;
        effect.ramp.end = 0x2000;
        break;
    case SDL_HAPTIC_CUSTOM:
        effect.custom.direction.type = SDL_HAPTIC_STEERING_AXIS;
        effect.custom.length = length;
        effect.custom.interval = interval;
        effect.custom.delay = delay;
        effect.custom.channels = 1;
        effect.custom.period = 10;
        effect.custom.samples = 2;
        effect.custom.data = custom_data;
        break;
    default:
        break;
    }
    return effect;
}

/* Creates an effect through SDL_DINPUT_HapticNewEffect, and returns whether
   CreateEffect took it. The caller frees it with Free. */
static bool Create(struct haptic_effect *effect, struct haptic_hweffect *hweffect, const SDL_HapticEffect *data)
{
    SDL_zerop(effect);
    SDL_zerop(hweffect);
    effect->hweffect = hweffect;
    SDL_zero(seen);
    return SDL_DINPUT_HapticNewEffect(&haptic, effect, data) && seen.creates == 1;
}

static void Free(struct haptic_effect *effect, Uint16 type)
{
    SDL_SYS_HapticFreeDIEFFECT(&effect->hweffect->effect, type);
}

/* 1 */
static void TestConversion(void)
{
    scenario = "length to microseconds";
    CHECK(SDL_HapticLengthToMicroseconds(0, INFINITE) == 0, "0 ms");
    CHECK(SDL_HapticLengthToMicroseconds(1, INFINITE) == 1000, "1 ms");
    CHECK(SDL_HapticLengthToMicroseconds(32767, INFINITE) == 32767000, "32767 ms");
    CHECK(SDL_HapticLengthToMicroseconds(4294967, INFINITE) == LONGEST_US, "4294967 ms, the longest that fits");
    CHECK(SDL_HapticLengthToMicroseconds(4294968, INFINITE) == LONGEST_US, "4294968 ms");
    CHECK(SDL_HapticLengthToMicroseconds(SDL_HAPTIC_INFINITY - 1, INFINITE) == LONGEST_US, "SDL_HAPTIC_INFINITY - 1");
    CHECK(SDL_HapticLengthToMicroseconds(SDL_HAPTIC_INFINITY, INFINITE) == INFINITE, "SDL_HAPTIC_INFINITY");
    CHECK(SDL_HapticLengthToMicroseconds(SDL_HAPTIC_INFINITY, 0) == LONGEST_US, "SDL_HAPTIC_INFINITY with no infinite value");
    CHECK(SDL_HapticLengthToMicroseconds(1000, 0) == 1000000, "1000 ms with no infinite value");
}

/* 2, 3 and 4 */
static void TestCreate(void)
{
    static const struct
    {
        const char *name;
        Uint16 type;
        DWORD infinite;
    } kinds[] = {
        { "constant", SDL_HAPTIC_CONSTANT, INFINITE },
        { "sine", SDL_HAPTIC_SINE, INFINITE },
        { "spring", SDL_HAPTIC_SPRING, INFINITE },
        { "ramp", SDL_HAPTIC_RAMP, LONGEST_US },
        { "custom", SDL_HAPTIC_CUSTOM, INFINITE }
    };
    static const struct
    {
        Uint32 length;
        DWORD duration; // 0 for the kind's infinite duration
    } lengths[] = {
        { SDL_HAPTIC_INFINITY, 0 },
        { 0, 0 },
        { 1000, 1000000 },
        { 4294967, LONGEST_US },
        { 4294968, LONGEST_US },
        { SDL_HAPTIC_INFINITY - 1, LONGEST_US }
    };
    struct haptic_effect effect;
    struct haptic_hweffect hweffect;
    int k, l;

    for (k = 0; k < (int)SDL_arraysize(kinds); ++k) {
        scenario = kinds[k].name;
        for (l = 0; l < (int)SDL_arraysize(lengths); ++l) {
            const SDL_HapticEffect data = Effect(kinds[k].type, lengths[l].length, 250, 100);
            const DWORD expected = (l == 0) ? kinds[k].infinite : lengths[l].duration;

            if (!Create(&effect, &hweffect, &data)) {
                CHECK(false, "length %u was not created: %s", (unsigned int)lengths[l].length, SDL_GetError());
                continue;
            }
            CHECK(seen.duration == expected, "length %u reached DirectInput as %lu us, not %lu", (unsigned int)lengths[l].length,
                  seen.duration, expected);
            CHECK(seen.interval == 250000 && seen.delay == 100000, "length %u: interval %lu us and delay %lu us, not 250000 and 100000",
                  (unsigned int)lengths[l].length, seen.interval, seen.delay);
            CHECK(hweffect.effect.dwDuration == expected, "the kept DIEFFECT holds %lu us", hweffect.effect.dwDuration);
            Free(&effect, kinds[k].type);
        }
    }
    /* The largest interval and delay fit in microseconds */
    scenario = "largest interval and delay";
    {
        const SDL_HapticEffect data = Effect(SDL_HAPTIC_CONSTANT, 1000, 65535, 65535);

        if (Create(&effect, &hweffect, &data)) {
            CHECK(seen.interval == 65535000 && seen.delay == 65535000, "interval %lu us and delay %lu us", seen.interval, seen.delay);
            Free(&effect, SDL_HAPTIC_CONSTANT);
        } else {
            CHECK(false, "not created: %s", SDL_GetError());
        }
    }
}

/* 5 */
static void TestUpdate(void)
{
    struct haptic_effect effect;
    struct haptic_hweffect hweffect;
    SDL_HapticEffect data = Effect(SDL_HAPTIC_CONSTANT, 1000, 0, 0);

    scenario = "update to an infinite length";
    if (!Create(&effect, &hweffect, &data)) {
        CHECK(false, "not created: %s", SDL_GetError());
        return;
    }
    CHECK(seen.duration == 1000000, "created with %lu us", seen.duration);
    data.constant.length = SDL_HAPTIC_INFINITY;
    CHECK(SDL_DINPUT_HapticUpdateEffect(&haptic, &effect, &data), "the update failed: %s", SDL_GetError());
    CHECK(seen.updates == 1 && (seen.flags & DIEP_DURATION) && seen.duration == INFINITE,
          "%d updates, flags %08lx, duration %lu us", seen.updates, seen.flags, seen.duration);
    CHECK(hweffect.effect.dwDuration == INFINITE, "the kept DIEFFECT holds %lu us", hweffect.effect.dwDuration);

    scenario = "update of the level only";
    data.constant.level = 0x3000;
    CHECK(SDL_DINPUT_HapticUpdateEffect(&haptic, &effect, &data), "the update failed: %s", SDL_GetError());
    CHECK(seen.updates == 2 && !(seen.flags & DIEP_DURATION) && (seen.flags & DIEP_TYPESPECIFICPARAMS),
          "%d updates, flags %08lx", seen.updates, seen.flags);
    Free(&effect, SDL_HAPTIC_CONSTANT);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    effect_vtbl.SetParameters = Fake_SetParameters;
    fake_effect.lpVtbl = &effect_vtbl;
    device_vtbl.CreateEffect = Fake_CreateEffect;
    fake_device.lpVtbl = &device_vtbl;
    hwdata.device = &fake_device;
    hwdata.axes[0] = DIJOFS_X;
    haptic.hwdata = &hwdata;
    haptic.naxes = 1;

    TestConversion();
    TestCreate();
    TestUpdate();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
