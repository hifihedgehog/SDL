/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
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

/* Real SDL core bridge for the offline pipeline test. As in
 * testxinputpaddlemapping.c, include the real mapping implementation and link
 * SDL3-static for the publisher, event queue, joystick state, and virtual driver.
 * The virtual fixture supplies its XInput GUID marker. This does not exercise
 * physical device detection or SDL_XINPUT_PaddleUpdate's hardware-query guards.
 */
#define SDL_MAIN_HANDLED
#include "../src/joystick/SDL_gamepad.c"
#include <stdio.h>

typedef struct PipelineFixture {
    SDL_JoystickID id;
    SDL_Joystick *joystick;
    SDL_Gamepad *gamepad;
    unsigned expected_mask;
    bool expected_a;
} PipelineFixture;

static unsigned pipeline_checks, pipeline_failures;
static char pipeline_error[512];

#define CORE_CHECK(expr) do { ++pipeline_checks; if (!(expr)) { ++pipeline_failures; \
    SDL_snprintf(pipeline_error, sizeof(pipeline_error), "line %d: %s (%s)", __LINE__, #expr, SDL_GetError()); \
    return 0; } } while (0)

/* The oracle is physical mask bit order, independent of SDL enum order. */
static const SDL_GamepadButton mapped_by_bit[4] = {
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2
};

static int ClearEvents(void)
{
    SDL_Event events[64];
    int count, total = 0;
    while ((count = SDL_PeepEvents(events, 64, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) > 0) {
        total += count;
        CORE_CHECK(total < 8192);
    }
    CORE_CHECK(count == 0);
    return 1;
}

static unsigned RawMask(PipelineFixture *fixture)
{
    unsigned mask = 0;
    for (int bit = 0; bit < 4; ++bit) if (SDL_GetJoystickButton(fixture->joystick, 12 + bit)) mask |= 1u << bit;
    return mask;
}

static unsigned MappedMask(PipelineFixture *fixture)
{
    unsigned mask = 0;
    for (int bit = 0; bit < 4; ++bit) if (SDL_GetGamepadButton(fixture->gamepad, mapped_by_bit[bit])) mask |= 1u << bit;
    return mask;
}

static int CheckState(PipelineFixture *fixture, unsigned mask, bool a)
{
    CORE_CHECK(RawMask(fixture) == mask);
    CORE_CHECK(MappedMask(fixture) == mask);
    CORE_CHECK(SDL_GetJoystickButton(fixture->joystick, 0) == a);
    CORE_CHECK(SDL_GetGamepadButton(fixture->gamepad, SDL_GAMEPAD_BUTTON_SOUTH) == a);
    return 1;
}

/* Inspect the actual event stream. Do not pump the virtual driver here. */
static int CheckEvents(PipelineFixture *fixture, unsigned old_mask, unsigned mask,
                       bool old_a, bool a, bool face_only)
{
    Uint8 raw[4];
    SDL_GamepadButton mapped[4];
    bool down[4];
    int changes = 0, seen = 0, count, total = 0;
    unsigned raw_down = 0, raw_up = 0, mapped_down = 0, mapped_up = 0;
    SDL_Event events[64];
    if (face_only) {
        if (old_a != a) {
            raw[0] = 0;
            mapped[0] = SDL_GAMEPAD_BUTTON_SOUTH;
            down[0] = a;
            changes = 1;
        }
    } else {
        for (int bit = 0; bit < 4; ++bit) {
            if ((old_mask ^ mask) & (1u << bit)) {
                raw[changes] = (Uint8)(12 + bit);
                mapped[changes] = mapped_by_bit[bit];
                down[changes] = (mask & (1u << bit)) != 0;
                ++changes;
            }
        }
    }
    while ((count = SDL_PeepEvents(events, 64, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) > 0) {
        total += count;
        CORE_CHECK(total < 8192);
        for (int i = 0; i < count; ++i) {
            const bool is_raw = events[i].type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || events[i].type == SDL_EVENT_JOYSTICK_BUTTON_UP;
            const bool is_mapped = events[i].type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || events[i].type == SDL_EVENT_GAMEPAD_BUTTON_UP;
            CORE_CHECK(events[i].type != SDL_EVENT_GAMEPAD_REMAPPED);
            if (!is_raw && !is_mapped) continue;
            CORE_CHECK(seen < changes * 2);
            /* SDL's mapping watcher emits the mapped event during raw push. */
            CORE_CHECK(is_mapped == ((seen & 1) == 0));
            if (is_mapped) {
                CORE_CHECK(events[i].gbutton.which == fixture->id);
                CORE_CHECK(events[i].gbutton.button == mapped[seen / 2]);
                CORE_CHECK(events[i].gbutton.down == down[seen / 2]);
                CORE_CHECK(events[i].common.timestamp != 0);
                if (!face_only) {
                    const unsigned bit = 1u << (raw[seen / 2] - 12);
                    if (events[i].gbutton.down) mapped_down |= bit;
                    else mapped_up |= bit;
                }
            } else {
                CORE_CHECK(events[i].jbutton.which == fixture->id);
                CORE_CHECK(events[i].jbutton.button == raw[seen / 2]);
                CORE_CHECK(events[i].jbutton.down == down[seen / 2]);
                CORE_CHECK(events[i].common.timestamp != 0);
                if (!face_only) {
                    const unsigned bit = 1u << (raw[seen / 2] - 12);
                    if (events[i].jbutton.down) raw_down |= bit;
                    else raw_up |= bit;
                }
            }
            ++seen;
        }
    }
    CORE_CHECK(count == 0);
    CORE_CHECK(seen == changes * 2);
    if (!face_only) {
        CORE_CHECK(raw_down == (mask & ~old_mask));
        CORE_CHECK(raw_up == (old_mask & ~mask));
        CORE_CHECK(mapped_down == raw_down && mapped_up == raw_up);
    }
    return 1;
}

int PaddlePipelineInitialize(void)
{
    const char *disabled[] = {
        SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_RAWINPUT, SDL_HINT_JOYSTICK_DIRECTINPUT,
        SDL_HINT_XINPUT_ENABLED, SDL_HINT_JOYSTICK_WGI, SDL_HINT_JOYSTICK_GAMEINPUT,
        SDL_HINT_JOYSTICK_GAMEINPUT_RAW, SDL_HINT_JOYSTICK_BLE_SWITCH2,
        SDL_HINT_JOYSTICK_THREAD, "SDL_JOYSTICK_WINMM", "SDL_JOYSTICK_ROG_CHAKRAM"
    };
    int count = -1;
    SDL_JoystickID *ids;
    SDL_SetMainReady();
    for (int i = 0; i < (int)SDL_arraysize(disabled); ++i) CORE_CHECK(SDL_SetHintWithPriority(disabled[i], "0", SDL_HINT_OVERRIDE));
    CORE_CHECK(SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1", SDL_HINT_OVERRIDE));
    CORE_CHECK(SDL_SetHintWithPriority(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0", SDL_HINT_OVERRIDE));
    CORE_CHECK(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_SetJoystickEventsEnabled(true);
    SDL_SetGamepadEventsEnabled(true);
    CORE_CHECK(SDL_EventEnabled(SDL_EVENT_JOYSTICK_BUTTON_DOWN) && SDL_EventEnabled(SDL_EVENT_JOYSTICK_BUTTON_UP));
    CORE_CHECK(SDL_EventEnabled(SDL_EVENT_GAMEPAD_BUTTON_DOWN) && SDL_EventEnabled(SDL_EVENT_GAMEPAD_BUTTON_UP));
    ids = SDL_GetJoysticks(&count);
    CORE_CHECK(ids != NULL);
    SDL_free(ids);
    CORE_CHECK(count == 0);
    return ClearEvents();
}

int PaddlePipelineOpen(void **output)
{
    static const char *base_fields =
        "a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,"
        "leftstick:b8,rightstick:b9,guide:b10,misc1:b11,"
        "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,"
        "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,";
    SDL_VirtualJoystickDesc desc;
    SDL_GUID original, synthetic;
    char guid[33], mapping[1024];
    int binding_count = 0;
    SDL_GamepadBinding **bindings;
    PipelineFixture *fixture = SDL_calloc(1, sizeof(*fixture));
    CORE_CHECK(fixture != NULL);
    *output = fixture; /* The caller closes partial fixtures on failure too. */
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = 6;
    desc.nbuttons = 16;
    desc.nhats = 1;
    desc.vendor_id = 0xF00D;
    desc.product_id = 0x0028;
    desc.name = "Captured paddle pipeline fixture";
    fixture->id = SDL_AttachVirtualJoystick(&desc);
    CORE_CHECK(fixture->id != 0);
    fixture->joystick = SDL_OpenJoystick(fixture->id);
    CORE_CHECK(fixture->joystick != NULL);
    original = SDL_GetJoystickGUIDForID(fixture->id);
    SDL_GUIDToString(original, guid, sizeof(guid));
    SDL_snprintf(mapping, sizeof(mapping), "%s,Captured paddle pipeline fixture,%s", guid, base_fields);
    CORE_CHECK(SDL_SetGamepadMapping(fixture->id, mapping));
    synthetic = original;
    synthetic.data[14] = 'x';
    SDL_LockJoysticks();
    fixture->joystick->guid = synthetic;
    SDL_UnlockJoysticks();
    CORE_CHECK(SDL_IsJoystickXInput(fixture->joystick->guid));
    CORE_CHECK(SDL_SetNumberProperty(SDL_GetJoystickProperties(fixture->joystick), SDL_PROP_JOYSTICK_XINPUT_PADDLE_MASK_NUMBER, 15));
    fixture->gamepad = SDL_OpenGamepad(fixture->id);
    CORE_CHECK(fixture->gamepad != NULL);
    bindings = SDL_GetGamepadBindings(fixture->gamepad, &binding_count);
    CORE_CHECK(bindings != NULL);
    for (int bit = 0; bit < 4; ++bit) {
        int matches = 0, found_raw = -1;
        for (int i = 0; i < binding_count; ++i) {
            if (bindings[i]->output_type == SDL_GAMEPAD_BINDTYPE_BUTTON && bindings[i]->output.button == mapped_by_bit[bit]) {
                ++matches;
                if (bindings[i]->input_type == SDL_GAMEPAD_BINDTYPE_BUTTON) found_raw = bindings[i]->input.button;
            }
        }
        if (matches != 1 || found_raw != 12 + bit) {
            SDL_free(bindings);
            CORE_CHECK(matches == 1 && found_raw == 12 + bit);
        }
        CORE_CHECK(SDL_GamepadHasButton(fixture->gamepad, mapped_by_bit[bit]));
    }
    SDL_free(bindings);
    CORE_CHECK(CheckState(fixture, 0, false));
    return ClearEvents();
}

int PaddlePipelineSetFaceA(void *handle, int down)
{
    PipelineFixture *fixture = handle;
    const bool want = down != 0;
    SDL_LockJoysticks();
    SDL_SendJoystickButton(SDL_GetTicksNS(), fixture->joystick, 0, want);
    SDL_UnlockJoysticks();
    CORE_CHECK(CheckState(fixture, fixture->expected_mask, want));
    CORE_CHECK(CheckEvents(fixture, fixture->expected_mask, fixture->expected_mask, fixture->expected_a, want, true));
    fixture->expected_a = want;
    return 1;
}

int PaddlePipelinePublish(void *handle, unsigned mask, int expected_a, unsigned *raw, unsigned *mapped)
{
    PipelineFixture *fixture = handle;
    const unsigned old = fixture->expected_mask;
    CORE_CHECK(mask <= 15 && fixture->expected_a == (expected_a != 0));
    SDL_LockJoysticks();
    for (Uint8 bit = 0; bit < 4; ++bit) SDL_SendJoystickButton(SDL_GetTicksNS(), fixture->joystick, (Uint8)(12 + bit), (mask & (1u << bit)) != 0);
    SDL_UnlockJoysticks();
    *raw = RawMask(fixture);
    *mapped = MappedMask(fixture);
    CORE_CHECK(CheckState(fixture, mask, expected_a != 0));
    CORE_CHECK(CheckEvents(fixture, old, mask, fixture->expected_a, fixture->expected_a, false));
    fixture->expected_mask = mask;
    return 1;
}

void PaddlePipelineClose(void *handle)
{
    PipelineFixture *fixture = handle;
    if (!fixture) return;
    if (fixture->gamepad) SDL_CloseGamepad(fixture->gamepad);
    if (fixture->joystick) SDL_CloseJoystick(fixture->joystick);
    if (fixture->id && !SDL_DetachVirtualJoystick(fixture->id)) {
        ++pipeline_failures;
        SDL_snprintf(pipeline_error, sizeof(pipeline_error), "Virtual fixture detach failed: %s", SDL_GetError());
    }
    SDL_free(fixture);
    ClearEvents();
}

unsigned PaddlePipelineChecks(void) { return pipeline_checks; }
unsigned PaddlePipelineFailures(void) { return pipeline_failures; }
const char *PaddlePipelineError(void) { return pipeline_error; }
void PaddlePipelineQuit(void) { SDL_Quit(); }
