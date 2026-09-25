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

#include "SDL_hidapihaptic_c.h"

#ifdef SDL_HAPTIC_HIDAPI_IFORCE

#include "../../joystick/SDL_iforce_proto.h"

/* I-Force force feedback, hifihedgehog/SDL#33 Part 8. The I-Force HIDAPI
 * joystick driver writes each command this builds to the interrupt OUT
 * endpoint, and its joystick properties carry the effect count and memory
 * end the device's N and B replies gave, so a device without them has no
 * haptic side. Opening sends Linux's start, the centering spring off and
 * force feedback on, and closing stops everything. Effects take the units
 * SDL's Linux haptic backend converts to, and the encoding follows Linux's
 * iforce-ff.c. A parameter block is written at most every 20 ms, the
 * spacing jsmolina's Windows port gives periodic updates on 06F8:0004, and a
 * thread sends the updates that wait. */

typedef struct SDL_HIDAPI_IForceHaptic
{
    SDL_Joystick *joystick;
    SDL_Mutex *lock;
    SDL_Condition *wake;
    SDL_Thread *thread;
    bool stop;          /* Under lock */
    bool wheel;
    SDL_IForceFF ff;    /* Under lock */
    bool created[SDL_IFORCE_EFFECTS_MAX];
} SDL_HIDAPI_IForceHaptic;

static bool IForceHaptic_Send(SDL_HIDAPI_IForceHaptic *ctx, const SDL_IForceCommand *command)
{
    return SDL_SendJoystickEffect(ctx->joystick, command->bytes, command->length);
}

static bool IForceHaptic_SendAll(SDL_HIDAPI_IForceHaptic *ctx, const SDL_IForceCommands *commands)
{
    bool result = true;
    int i;

    for (i = 0; i < commands->count; ++i) {
        if (!IForceHaptic_Send(ctx, &commands->command[i])) {
            result = false;
        }
    }
    return result;
}

static bool SDL_HIDAPI_HapticDriverIForce_JoystickSupported(SDL_Joystick *joystick)
{
    SDL_PropertiesID props;

    if (!SDL_IForce_FindModel(SDL_GetJoystickVendor(joystick), SDL_GetJoystickProduct(joystick), true)) {
        return false;
    }
    props = SDL_GetJoystickProperties(joystick);
    return SDL_GetNumberProperty(props, SDL_IFORCE_PROP_EFFECTS_NUMBER, 0) > 0;
}

/* Sends the updates whose blocks may be written again */
static int SDLCALL IForceHaptic_Thread(void *data)
{
    SDL_HIDAPI_IForceHaptic *ctx = (SDL_HIDAPI_IForceHaptic *)data;

    SDL_LockMutex(ctx->lock);
    while (!ctx->stop) {
        SDL_IForceCommands commands;
        Uint64 deadline;
        const Uint64 now = SDL_GetTicks();

        if (SDL_IForce_NextDeferred(&ctx->ff, now, &commands)) {
            IForceHaptic_SendAll(ctx, &commands);
            continue;
        }
        if (SDL_IForce_DeferredDeadline(&ctx->ff, &deadline)) {
            SDL_WaitConditionTimeout(ctx->wake, ctx->lock, (Sint32)((deadline > now) ? SDL_min(deadline - now, 1000) : 1));
        } else {
            SDL_WaitCondition(ctx->wake, ctx->lock);
        }
    }
    SDL_UnlockMutex(ctx->lock);
    return 0;
}

static void *SDL_HIDAPI_HapticDriverIForce_Open(SDL_Joystick *joystick)
{
    SDL_HIDAPI_IForceHaptic *ctx;
    SDL_PropertiesID props;
    SDL_IForceCommand spring[2];
    SDL_IForceCommand enable;
    SDL_IForceIdentity identity;
    const SDL_IForceModel *model = SDL_IForce_FindModel(SDL_GetJoystickVendor(joystick), SDL_GetJoystickProduct(joystick), true);

    if (!model || !SDL_HIDAPI_HapticDriverIForce_JoystickSupported(joystick)) {
        SDL_SetError("The device reported no I-Force effects");
        return NULL;
    }
    ctx = (SDL_HIDAPI_IForceHaptic *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->joystick = joystick;
    SDL_IForce_GetIdentity(model, &identity);
    ctx->wheel = identity.wheel;
    props = SDL_GetJoystickProperties(joystick);
    SDL_IForce_InitFF(&ctx->ff, (int)SDL_GetNumberProperty(props, SDL_IFORCE_PROP_EFFECTS_NUMBER, 0),
                      (Uint16)SDL_GetNumberProperty(props, SDL_IFORCE_PROP_MEMORY_NUMBER, SDL_IFORCE_MEMORY_DEFAULT));

    ctx->lock = SDL_CreateMutex();
    ctx->wake = SDL_CreateCondition();
    if (!ctx->lock || !ctx->wake) {
        goto failed;
    }

    /* Linux's start: spring strength 0, spring on, then force feedback on */
    SDL_IForce_BuildAutocenter(spring, 0);
    SDL_IForce_BuildState(&enable, SDL_IFORCE_STATE_ENABLE);
    if (!IForceHaptic_Send(ctx, &spring[0]) || !IForceHaptic_Send(ctx, &spring[1]) || !IForceHaptic_Send(ctx, &enable)) {
        goto failed;
    }

    ctx->thread = SDL_CreateThread(IForceHaptic_Thread, "SDL I-Force haptic", ctx);
    if (!ctx->thread) {
        goto failed;
    }
    return ctx;

failed:
    if (ctx->wake) {
        SDL_DestroyCondition(ctx->wake);
    }
    if (ctx->lock) {
        SDL_DestroyMutex(ctx->lock);
    }
    SDL_free(ctx);
    return NULL;
}

static bool SDL_HIDAPI_HapticDriverIForce_StopEffects(SDL_HIDAPI_HapticDevice *device)
{
    SDL_HIDAPI_IForceHaptic *ctx = (SDL_HIDAPI_IForceHaptic *)device->ctx;
    bool result = true;
    int i;

    SDL_LockMutex(ctx->lock);
    for (i = 0; i < ctx->ff.effects; ++i) {
        if (ctx->created[i]) {
            SDL_IForceCommand stop;

            SDL_IForce_SetPlaying(&ctx->ff, i, false);
            SDL_IForce_BuildPlay(&stop, (Uint8)i, 0);
            if (!IForceHaptic_Send(ctx, &stop)) {
                result = false;
            }
        }
    }
    SDL_UnlockMutex(ctx->lock);
    return result;
}

static void SDL_HIDAPI_HapticDriverIForce_Close(SDL_HIDAPI_HapticDevice *device)
{
    SDL_HIDAPI_IForceHaptic *ctx = (SDL_HIDAPI_IForceHaptic *)device->ctx;
    SDL_IForceCommand stop;

    SDL_LockMutex(ctx->lock);
    ctx->stop = true;
    SDL_SignalCondition(ctx->wake);
    SDL_UnlockMutex(ctx->lock);
    SDL_WaitThread(ctx->thread, NULL);
    ctx->thread = NULL;

    /* Linux's close: stop every effect, force feedback off */
    if (device->joystick) {
        SDL_IForce_BuildState(&stop, SDL_IFORCE_STATE_STOP_ALL);
        IForceHaptic_Send(ctx, &stop);
    }
    SDL_DestroyCondition(ctx->wake);
    SDL_DestroyMutex(ctx->lock);
}

static int SDL_HIDAPI_HapticDriverIForce_NumEffects(SDL_HIDAPI_HapticDevice *device)
{
    SDL_HIDAPI_IForceHaptic *ctx = (SDL_HIDAPI_IForceHaptic *)device->ctx;

    return ctx->ff.effects;
}

/* Linux's I-Force capabilities */
static Uint32 SDL_HIDAPI_HapticDriverIForce_GetFeatures(SDL_HIDAPI_HapticDevice *device)
{
    return SDL_HAPTIC_CONSTANT |
           SDL_HAPTIC_SINE |
           SDL_HAPTIC_SQUARE |
           SDL_HAPTIC_TRIANGLE |
           SDL_HAPTIC_SAWTOOTHUP |
           SDL_HAPTIC_SAWTOOTHDOWN |
           SDL_HAPTIC_SPRING |
           SDL_HAPTIC_DAMPER |
           SDL_HAPTIC_GAIN |
           SDL_HAPTIC_AUTOCENTER;
}

static int SDL_HIDAPI_HapticDriverIForce_NumAxes(SDL_HIDAPI_HapticDevice *device)
{
    SDL_HIDAPI_IForceHaptic *ctx = (SDL_HIDAPI_IForceHaptic *)device->ctx;

    return ctx->wheel ? 1 : 2;
}

static Uint16 IForceHaptic_Clamp(Uint32 value)
{
    return (Uint16)((value > 32767) ? 32767 : value);
}

/* SDL's Linux haptic backend's conversion to Linux's direction units, with
 * its types, so a negative angle converts as it does there */
static bool IForceHaptic_Direction(const SDL_HapticDirection *direction, Uint16 *out)
{
    Uint32 value;

    switch (direction->type) {
    case SDL_HAPTIC_POLAR:
        value = (Uint32)(((direction->dir[0] % 36000) * 0x8000) / 18000);
        break;
    case SDL_HAPTIC_SPHERICAL:
        value = (Uint32)((direction->dir[0] + 9000) % 36000);
        value = (value * 0x8000) / 18000;
        break;
    case SDL_HAPTIC_CARTESIAN:
        if (!direction->dir[1]) {
            value = (direction->dir[0] >= 0) ? 0x4000 : 0xC000;
        } else if (!direction->dir[0]) {
            value = (direction->dir[1] >= 0) ? 0x8000 : 0;
        } else {
            const float f = SDL_atan2f((float)direction->dir[1], (float)direction->dir[0]);

            value = (Uint32)((((Sint32)(f * 18000.0 / SDL_PI_D)) + 45000) % 36000);
            value = (value * 0x8000) / 18000;
        }
        break;
    case SDL_HAPTIC_STEERING_AXIS:
        value = 0x4000;
        break;
    default:
        return SDL_SetError("Haptic: Unsupported direction type.");
    }
    *out = (Uint16)value;
    return true;
}

/* A trigger button, 1 for the device's first, goes in the core's low nibble */
static Uint8 IForceHaptic_Trigger(Uint16 button)
{
    return (Uint8)((button <= 15) ? button : 0);
}

static void IForceHaptic_Envelope(SDL_IForceEnvelope *out, Uint16 attack_length, Uint16 attack_level, Uint16 fade_length, Uint16 fade_level)
{
    out->attack_length = IForceHaptic_Clamp(attack_length);
    out->attack_level = IForceHaptic_Clamp(attack_level);
    out->fade_length = IForceHaptic_Clamp(fade_length);
    out->fade_level = IForceHaptic_Clamp(fade_level);
}

static bool IForceHaptic_Convert(const SDL_HapticEffect *effect, SDL_IForceEffect *out)
{
    SDL_zerop(out);
    switch (effect->type) {
    case SDL_HAPTIC_CONSTANT:
    {
        const SDL_HapticConstant *c = &effect->constant;

        out->type = SDL_IFORCE_EFFECT_CONSTANT;
        out->length = (c->length == SDL_HAPTIC_INFINITY) ? 0 : IForceHaptic_Clamp(c->length);
        out->delay = IForceHaptic_Clamp(c->delay);
        out->trigger_button = IForceHaptic_Trigger(c->button);
        out->trigger_interval = IForceHaptic_Clamp(c->interval);
        out->level = c->level;
        IForceHaptic_Envelope(&out->envelope, c->attack_length, c->attack_level, c->fade_length, c->fade_level);
        return IForceHaptic_Direction(&c->direction, &out->direction);
    }
    case SDL_HAPTIC_SINE:
    case SDL_HAPTIC_SQUARE:
    case SDL_HAPTIC_TRIANGLE:
    case SDL_HAPTIC_SAWTOOTHUP:
    case SDL_HAPTIC_SAWTOOTHDOWN:
    {
        const SDL_HapticPeriodic *p = &effect->periodic;

        out->type = SDL_IFORCE_EFFECT_PERIODIC;
        switch (effect->type) {
        case SDL_HAPTIC_SINE:
            out->waveform = SDL_IFORCE_WAVE_SINE;
            break;
        case SDL_HAPTIC_SQUARE:
            out->waveform = SDL_IFORCE_WAVE_SQUARE;
            break;
        case SDL_HAPTIC_TRIANGLE:
            out->waveform = SDL_IFORCE_WAVE_TRIANGLE;
            break;
        case SDL_HAPTIC_SAWTOOTHUP:
            out->waveform = SDL_IFORCE_WAVE_SAWTOOTH_UP;
            break;
        default:
            out->waveform = SDL_IFORCE_WAVE_SAWTOOTH_DOWN;
            break;
        }
        out->length = (p->length == SDL_HAPTIC_INFINITY) ? 0 : IForceHaptic_Clamp(p->length);
        out->delay = IForceHaptic_Clamp(p->delay);
        out->trigger_button = IForceHaptic_Trigger(p->button);
        out->trigger_interval = IForceHaptic_Clamp(p->interval);
        out->period = IForceHaptic_Clamp(p->period);
        out->magnitude = p->magnitude;
        out->offset = p->offset;
        /* Linux's phase: 0x10000 is 360 degrees */
        out->phase = (Uint16)(((Uint32)p->phase * 0x10000U) / 36000);
        IForceHaptic_Envelope(&out->envelope, p->attack_length, p->attack_level, p->fade_length, p->fade_level);
        return IForceHaptic_Direction(&p->direction, &out->direction);
    }
    case SDL_HAPTIC_SPRING:
    case SDL_HAPTIC_DAMPER:
    {
        const SDL_HapticCondition *c = &effect->condition;
        int i;

        out->type = (effect->type == SDL_HAPTIC_SPRING) ? SDL_IFORCE_EFFECT_SPRING : SDL_IFORCE_EFFECT_DAMPER;
        out->length = (c->length == SDL_HAPTIC_INFINITY) ? 0 : IForceHaptic_Clamp(c->length);
        out->delay = IForceHaptic_Clamp(c->delay);
        out->trigger_button = IForceHaptic_Trigger(c->button);
        out->trigger_interval = IForceHaptic_Clamp(c->interval);
        for (i = 0; i < 2; ++i) {
            out->condition[i].right_saturation = c->right_sat[i];
            out->condition[i].left_saturation = c->left_sat[i];
            out->condition[i].right_coeff = c->right_coeff[i];
            out->condition[i].left_coeff = c->left_coeff[i];
            out->condition[i].deadband = c->deadband[i];
            out->condition[i].center = c->center[i];
        }
        return IForceHaptic_Direction(&c->direction, &out->direction);
    }
    default:
        return SDL_SetError("Unsupported effect");
    }
}

static int IForceHaptic_Upload(SDL_HIDAPI_IForceHaptic *ctx, int id, const SDL_HapticEffect *data)
{
    SDL_IForceEffect effect;
    SDL_IForceCommands commands;
    int result;

    if (!IForceHaptic_Convert(data, &effect)) {
        return SDL_IFORCE_UPLOAD_INVALID;
    }
    result = SDL_IForce_Upload(&ctx->ff, id, &effect, SDL_GetTicks(), &commands);
    if (result == SDL_IFORCE_UPLOAD_DEFERRED) {
        SDL_SignalCondition(ctx->wake);
    } else if (result == SDL_IFORCE_UPLOAD_SENT && !IForceHaptic_SendAll(ctx, &commands)) {
        return SDL_IFORCE_UPLOAD_INVALID;
    } else if (result == SDL_IFORCE_UPLOAD_NO_MEMORY) {
        SDL_SetError("The device has no room for the effect");
    } else if (result == SDL_IFORCE_UPLOAD_INVALID) {
        SDL_SetError("Bad effect parameters");
    }
    return result;
}

static SDL_HapticEffectID SDL_HIDAPI_HapticDriverIForce_CreateEffect(SDL_HIDAPI_HapticDevice *device, const SDL_HapticEffect *data)
{
    SDL_HIDAPI_IForceHaptic *ctx = (SDL_HIDAPI_IForceHaptic *)device->ctx;
    int id;

    if (!(SDL_HIDAPI_HapticDriverIForce_GetFeatures(device) & data->type)) {
        SDL_SetError("Unsupported effect");
        return -1;
    }
    SDL_LockMutex(ctx->lock);
    for (id = 0; id < ctx->ff.effects; ++id) {
        if (!ctx->created[id]) {
            break;
        }
    }
    if (id == ctx->ff.effects) {
        SDL_UnlockMutex(ctx->lock);
        SDL_SetError("All effect slots in use");
        return -1;
    }
    if (IForceHaptic_Upload(ctx, id, data) < 0) {
        SDL_IForce_Erase(&ctx->ff, id);
        SDL_UnlockMutex(ctx->lock);
        return -1;
    }
    ctx->created[id] = true;
    SDL_UnlockMutex(ctx->lock);
    return id;
}

static bool IForceHaptic_Valid(SDL_HIDAPI_IForceHaptic *ctx, SDL_HapticEffectID id)
{
    return id >= 0 && id < ctx->ff.effects && ctx->created[id];
}

static bool SDL_HIDAPI_HapticDriverIForce_UpdateEffect(SDL_HIDAPI_HapticDevice *device, SDL_HapticEffectID id, const SDL_HapticEffect *data)
{
    SDL_HIDAPI_IForceHaptic *ctx = (SDL_HIDAPI_IForceHaptic *)device->ctx;
    bool result;

    SDL_LockMutex(ctx->lock);
    if (!IForceHaptic_Valid(ctx, id)) {
        SDL_UnlockMutex(ctx->lock);
        return SDL_SetError("Bad effect id");
    }
    result = (IForceHaptic_Upload(ctx, id, data) >= 0);
    SDL_UnlockMutex(ctx->lock);
    return result;
}

static bool SDL_HIDAPI_HapticDriverIForce_RunEffect(SDL_HIDAPI_HapticDevice *device, SDL_HapticEffectID id, Uint32 iterations)
{
    SDL_HIDAPI_IForceHaptic *ctx = (SDL_HIDAPI_IForceHaptic *)device->ctx;
    SDL_IForceCommand play;
    bool result;

    SDL_LockMutex(ctx->lock);
    if (!IForceHaptic_Valid(ctx, id)) {
        SDL_UnlockMutex(ctx->lock);
        return SDL_SetError("Bad effect id");
    }
    SDL_IForce_SetPlaying(&ctx->ff, id, iterations > 0);
    SDL_IForce_BuildPlay(&play, (Uint8)id, iterations);
    result = IForceHaptic_Send(ctx, &play);
    SDL_UnlockMutex(ctx->lock);
    return result;
}

static bool SDL_HIDAPI_HapticDriverIForce_StopEffect(SDL_HIDAPI_HapticDevice *device, SDL_HapticEffectID id)
{
    return SDL_HIDAPI_HapticDriverIForce_RunEffect(device, id, 0);
}

static void SDL_HIDAPI_HapticDriverIForce_DestroyEffect(SDL_HIDAPI_HapticDevice *device, SDL_HapticEffectID id)
{
    SDL_HIDAPI_IForceHaptic *ctx = (SDL_HIDAPI_IForceHaptic *)device->ctx;
    SDL_IForceCommand stop;

    SDL_LockMutex(ctx->lock);
    if (IForceHaptic_Valid(ctx, id)) {
        SDL_IForce_BuildPlay(&stop, (Uint8)id, 0);
        IForceHaptic_Send(ctx, &stop);
        SDL_IForce_Erase(&ctx->ff, id);
        ctx->created[id] = false;
    }
    SDL_UnlockMutex(ctx->lock);
}

static bool SDL_HIDAPI_HapticDriverIForce_GetEffectStatus(SDL_HIDAPI_HapticDevice *device, SDL_HapticEffectID id)
{
    return false;
}

static bool SDL_HIDAPI_HapticDriverIForce_SetGain(SDL_HIDAPI_HapticDevice *device, int gain)
{
    SDL_HIDAPI_IForceHaptic *ctx = (SDL_HIDAPI_IForceHaptic *)device->ctx;
    SDL_IForceCommand command;
    bool result;

    gain = SDL_clamp(gain, 0, 100);
    SDL_LockMutex(ctx->lock);
    SDL_IForce_BuildGain(&command, (Uint16)((0xFFFFU * (Uint32)gain) / 100));
    result = IForceHaptic_Send(ctx, &command);
    SDL_UnlockMutex(ctx->lock);
    return result;
}

static bool SDL_HIDAPI_HapticDriverIForce_SetAutocenter(SDL_HIDAPI_HapticDevice *device, int autocenter)
{
    SDL_HIDAPI_IForceHaptic *ctx = (SDL_HIDAPI_IForceHaptic *)device->ctx;
    SDL_IForceCommand commands[2];
    bool result;

    autocenter = SDL_clamp(autocenter, 0, 100);
    SDL_LockMutex(ctx->lock);
    SDL_IForce_BuildAutocenter(commands, (Uint16)((0xFFFFU * (Uint32)autocenter) / 100));
    result = IForceHaptic_Send(ctx, &commands[0]) && IForceHaptic_Send(ctx, &commands[1]);
    SDL_UnlockMutex(ctx->lock);
    return result;
}

static bool SDL_HIDAPI_HapticDriverIForce_Pause(SDL_HIDAPI_HapticDevice *device)
{
    return SDL_Unsupported();
}

static bool SDL_HIDAPI_HapticDriverIForce_Resume(SDL_HIDAPI_HapticDevice *device)
{
    return SDL_Unsupported();
}

SDL_HIDAPI_HapticDriver SDL_HIDAPI_HapticDriverIForce = {
    SDL_HIDAPI_HapticDriverIForce_JoystickSupported,
    SDL_HIDAPI_HapticDriverIForce_Open,
    SDL_HIDAPI_HapticDriverIForce_Close,
    SDL_HIDAPI_HapticDriverIForce_NumEffects,
    SDL_HIDAPI_HapticDriverIForce_NumEffects,
    SDL_HIDAPI_HapticDriverIForce_GetFeatures,
    SDL_HIDAPI_HapticDriverIForce_NumAxes,
    SDL_HIDAPI_HapticDriverIForce_CreateEffect,
    SDL_HIDAPI_HapticDriverIForce_UpdateEffect,
    SDL_HIDAPI_HapticDriverIForce_RunEffect,
    SDL_HIDAPI_HapticDriverIForce_StopEffect,
    SDL_HIDAPI_HapticDriverIForce_DestroyEffect,
    SDL_HIDAPI_HapticDriverIForce_GetEffectStatus,
    SDL_HIDAPI_HapticDriverIForce_SetGain,
    SDL_HIDAPI_HapticDriverIForce_SetAutocenter,
    SDL_HIDAPI_HapticDriverIForce_Pause,
    SDL_HIDAPI_HapticDriverIForce_Resume,
    SDL_HIDAPI_HapticDriverIForce_StopEffects,
};

#endif /* SDL_HAPTIC_HIDAPI_IFORCE */

#endif /* SDL_JOYSTICK_HIDAPI */
