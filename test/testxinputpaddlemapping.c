/* Offline mapping tests. The devices are virtual and do not test acquisition.
 * Including the real gamepad implementation exposes its binding pointers. The
 * static archive supplies the rest of SDL without extracting a second copy.
 */
#define SDL_MAIN_HANDLED
#include "../src/joystick/SDL_gamepad.c"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, failures, groups;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    printf("FAIL line=%d expression=%s error=%s\n", __LINE__, #expr, SDL_GetError()); } } while (0)
#define REQUIRE(expr) do { ++checks; if (!(expr)) { ++failures; \
    printf("FAIL line=%d expression=%s error=%s\n", __LINE__, #expr, SDL_GetError()); exit(2); } } while (0)

static SDL_malloc_func base_malloc;
static SDL_calloc_func base_calloc;
static SDL_realloc_func base_realloc;
static SDL_free_func base_free;
static __declspec(thread) SDL_Gamepad *allocation_gamepad;
static __declspec(thread) size_t allocation_size;
static __declspec(thread) int allocation_action;
static __declspec(thread) unsigned allocation_hits;

static void *SDLCALL TargetedRealloc(void *pointer, size_t size)
{
    if (allocation_action && allocation_gamepad &&
        pointer == allocation_gamepad->bindings && size == allocation_size) {
        allocation_action = 0;
        ++allocation_hits;
        return NULL;
    }
    return base_realloc(pointer, size);
}

static const char *base_fields =
    "a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,"
    "leftstick:b8,rightstick:b9,guide:b10,misc1:b11,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,"
    "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,";
static const SDL_GamepadButton paddle_buttons[4] = {
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2
};
static const int paddle_raw[4] = {12, 14, 13, 15};
static const Uint8 paddle_bits[4] = {1, 4, 2, 8};

typedef struct Fixture {
    SDL_JoystickID ids[2];
    SDL_Joystick *joysticks[2];
    SDL_Gamepad *gamepads[2];
    SDL_GUID original_guid;
    SDL_GUID synthetic_guid;
} Fixture;

static void DrainEvents(void)
{
    SDL_Event events[64];
    int count, total = 0;
    while ((count = SDL_PeepEvents(events, 64, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) > 0) {
        total += count;
        REQUIRE(total < 8192);
    }
    REQUIRE(count == 0);
}

static void MappingText(Fixture *fixture, const char *fields, const char *extra, char *output, size_t capacity)
{
    char guid[33];
    SDL_GUIDToString(fixture->original_guid, guid, sizeof(guid));
    SDL_snprintf(output, capacity, "%s,Paddle core fixture,%s%s", guid, fields, extra ? extra : "");
}

static void SetBase(Fixture *fixture, const char *extra)
{
    char mapping[1024];
    MappingText(fixture, base_fields, extra, mapping, sizeof(mapping));
    REQUIRE(SDL_SetGamepadMapping(fixture->ids[0], mapping));
    DrainEvents();
}

static void OpenGamepadAt(Fixture *fixture, int index)
{
    fixture->gamepads[index] = SDL_OpenGamepad(fixture->ids[index]);
    REQUIRE(fixture->gamepads[index] != NULL);
}

static void OpenGamepads(Fixture *fixture)
{
    for (int i = 0; i < 2; ++i) OpenGamepadAt(fixture, i);
}

static Fixture OpenFixture(bool open_gamepads, Uint8 capability_mask)
{
    Fixture fixture;
    SDL_VirtualJoystickDesc desc;
    SDL_zero(fixture);
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = 6;
    desc.nbuttons = 20;
    desc.nhats = 1;
    desc.vendor_id = 0xf00d;
    desc.product_id = 0x0028;
    desc.name = "Paddle core fixture";
    for (int i = 0; i < 2; ++i) {
        fixture.ids[i] = SDL_AttachVirtualJoystick(&desc);
        REQUIRE(fixture.ids[i] != 0);
        fixture.joysticks[i] = SDL_OpenJoystick(fixture.ids[i]);
        REQUIRE(fixture.joysticks[i] != NULL);
    }
    fixture.original_guid = SDL_GetJoystickGUIDForID(fixture.ids[0]);
    {
        SDL_GUID second = SDL_GetJoystickGUIDForID(fixture.ids[1]);
        REQUIRE(SDL_memcmp(&fixture.original_guid, &second, sizeof(second)) == 0);
    }
    SetBase(&fixture, NULL);
    fixture.synthetic_guid = fixture.original_guid;
    fixture.synthetic_guid.data[14] = 'x';
    // Only the opened object is changed to exercise the overlay predicate. The
    // virtual driver retains its GUID and owns every input operation below.
    SDL_LockJoysticks();
    for (int i = 0; i < 2; ++i) fixture.joysticks[i]->guid = fixture.synthetic_guid;
    SDL_UnlockJoysticks();
    REQUIRE(SDL_IsJoystickXInput(fixture.joysticks[0]->guid));
    // Qualification is metadata installed before SDL_OpenGamepad. No input
    // receipt or transport callback is used to publish these mappings.
    REQUIRE(SDL_SetNumberProperty(SDL_GetJoystickProperties(fixture.joysticks[0]),
        SDL_PROP_JOYSTICK_XINPUT_PADDLE_MASK_NUMBER, capability_mask));
    DrainEvents();
    if (open_gamepads) OpenGamepads(&fixture);
    return fixture;
}

static void CloseFixture(Fixture *fixture)
{
    allocation_action = 0;
    allocation_gamepad = NULL;
    for (int i = 0; i < 2; ++i) {
        if (fixture->gamepads[i]) SDL_CloseGamepad(fixture->gamepads[i]);
        SDL_CloseJoystick(fixture->joysticks[i]);
        REQUIRE(SDL_DetachVirtualJoystick(fixture->ids[i]));
    }
    DrainEvents();
}

static void ReloadForAllocationTest(SDL_Gamepad *gamepad)
{
    // Fault injection exercises the actual open/base-map loader. Acquisition
    // state changes do not invoke this loader or publish a remap event.
    SDL_LockJoysticks();
    SDL_PrivateLoadButtonMapping(gamepad, gamepad->mapping);
    SDL_UnlockJoysticks();
}

static int RawBinding(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
    int count = 0, result = -1, matches = 0;
    SDL_GamepadBinding **bindings = SDL_GetGamepadBindings(gamepad, &count);
    REQUIRE(bindings != NULL);
    for (int i = 0; i < count; ++i) {
        if (bindings[i]->output_type == SDL_GAMEPAD_BINDTYPE_BUTTON && bindings[i]->output.button == button) {
            ++matches;
            if (bindings[i]->input_type == SDL_GAMEPAD_BINDTYPE_BUTTON) result = bindings[i]->input.button;
        }
    }
    CHECK(matches <= 1);
    SDL_free(bindings);
    return result;
}

static void CheckDefaultBindings(SDL_Gamepad *gamepad, Uint8 expected_mask)
{
    for (int i = 0; i < 4; ++i) {
        CHECK(RawBinding(gamepad, paddle_buttons[i]) == ((expected_mask & paddle_bits[i]) ? paddle_raw[i] : -1));
        CHECK(SDL_GamepadHasButton(gamepad, paddle_buttons[i]) == ((expected_mask & paddle_bits[i]) != 0));
    }
    CHECK(RawBinding(gamepad, SDL_GAMEPAD_BUTTON_MISC1) == 11);
}

static void CheckStringAgainstBindings(SDL_Gamepad *gamepad, const char *mapping_string)
{
    SDL_Gamepad parsed;
    GamepadMapping_t map;
    int count = 0;
    SDL_GamepadBinding **bindings = SDL_GetGamepadBindings(gamepad, &count);
    const char *fields;
    REQUIRE(bindings != NULL && mapping_string != NULL);
    fields = SDL_strchr(mapping_string, ',');
    REQUIRE(fields != NULL);
    fields = SDL_strchr(fields + 1, ',');
    REQUIRE(fields != NULL);
    SDL_zero(parsed);
    SDL_zero(map);
    map.mapping = (char *)(fields + 1);
    parsed.mapping = &map;
    parsed.joystick = gamepad->joystick;
    // The oracle is SDL's existing parser, not a second mapping implementation.
    SDL_LockJoysticks();
    REQUIRE(SDL_PrivateParseGamepadConfigString(&parsed, fields + 1));
    SDL_UnlockJoysticks();
    CHECK(parsed.num_bindings == count);
    if (parsed.num_bindings == count) {
        for (int i = 0; i < count; ++i) CHECK(SDL_memcmp(&parsed.bindings[i], bindings[i], sizeof(*bindings[i])) == 0);
    }
    SDL_free(parsed.bindings);
    SDL_free(bindings);
}

static void CheckGetters(Fixture *fixture, int index)
{
    char *opened = SDL_GetGamepadMapping(fixture->gamepads[index]);
    char *by_id = SDL_GetGamepadMappingForID(fixture->ids[index]);
    CheckStringAgainstBindings(fixture->gamepads[index], opened);
    CheckStringAgainstBindings(fixture->gamepads[index], by_id);
    SDL_free(opened);
    SDL_free(by_id);
}

static void IsolationAndPolicies(void)
{
    const Uint8 masks[] = {0, 15, 5, 10};
    for (int scenario = 0; scenario < (int)SDL_arraysize(masks); ++scenario) {
        Fixture fixture = OpenFixture(false, masks[scenario]);
        int before_count = 0, after_count = 0;
        char **before = SDL_GetGamepadMappings(&before_count);
        REQUIRE(before != NULL);
        for (int device = 0; device < 2; ++device) {
            OpenGamepadAt(&fixture, device);
            // Inspect each return before opening the sibling or updating input.
            CheckDefaultBindings(fixture.gamepads[device], device == 0 ? masks[scenario] : 0);
            for (int paddle = 0; paddle < 4; ++paddle) {
                CHECK(!SDL_GetJoystickButton(fixture.joysticks[device], paddle_raw[paddle]));
                CHECK(!SDL_GetGamepadButton(fixture.gamepads[device], paddle_buttons[paddle]));
            }
            CheckGetters(&fixture, device);
        }
        CHECK(SDL_GetNumberProperty(SDL_GetJoystickProperties(fixture.joysticks[1]),
            SDL_PROP_JOYSTICK_XINPUT_PADDLE_MASK_NUMBER, 0) == 0);
        {
            char **after = SDL_GetGamepadMappings(&after_count);
            REQUIRE(after != NULL);
            CHECK(before_count == after_count);
            if (before_count == after_count) for (int i = 0; i < before_count; ++i) CHECK(SDL_strcmp(before[i], after[i]) == 0);
            SDL_free(after);
        }
        SDL_free(before);
        CloseFixture(&fixture);
    }
}

static void InputAndEventOrder(void)
{
    Fixture fixture = OpenFixture(true, 15);
    const Uint8 raw[] = {0, 12, 14, 13, 15};
    const SDL_GamepadButton buttons[] = {SDL_GAMEPAD_BUTTON_SOUTH,
        SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
        SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2};
    for (int down = 1; down >= 0; --down) {
        for (int i = 0; i < 5; ++i) REQUIRE(SDL_SetJoystickVirtualButton(fixture.joysticks[0], raw[i], down != 0));
        REQUIRE(SDL_SetJoystickVirtualButton(fixture.joysticks[0], 11, down != 0));
        SDL_UpdateJoysticks();
        for (int i = 0; i < 5; ++i) {
            CHECK(SDL_GetJoystickButton(fixture.joysticks[0], raw[i]) == (down != 0));
            CHECK(SDL_GetGamepadButton(fixture.gamepads[0], buttons[i]) == (down != 0));
            CHECK(!SDL_GetGamepadButton(fixture.gamepads[1], buttons[i]));
        }
        CHECK(SDL_GetGamepadButton(fixture.gamepads[0], SDL_GAMEPAD_BUTTON_MISC1) == (down != 0));
    }
    DrainEvents();
    SDL_LockJoysticks();
    for (int down = 1; down >= 0; --down) {
        for (int i = 0; i < 5; ++i) SDL_SendJoystickButton(SDL_GetTicksNS(), fixture.joysticks[0], raw[i], down != 0);
    }
    SDL_UnlockJoysticks();
    {
        SDL_Event events[64];
        int count = SDL_PeepEvents(events, 64, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST);
        int seen = 0;
        REQUIRE(count >= 0);
        for (int i = 0; i < count; ++i) {
            bool gamepad_event = events[i].type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || events[i].type == SDL_EVENT_GAMEPAD_BUTTON_UP;
            bool raw_event = events[i].type == SDL_EVENT_JOYSTICK_BUTTON_DOWN || events[i].type == SDL_EVENT_JOYSTICK_BUTTON_UP;
            if (!gamepad_event && !raw_event) continue;
            REQUIRE(seen < 20);
            int index = (seen / 2) % 5;
            bool down = seen < 10;
            // SDL's watcher enqueues the derived gamepad event during the raw push.
            CHECK(gamepad_event == ((seen % 2) == 0));
            if (gamepad_event) {
                CHECK(events[i].gbutton.which == fixture.ids[0]);
                CHECK(events[i].gbutton.button == buttons[index] && events[i].gbutton.down == down);
            } else {
                CHECK(events[i].jbutton.which == fixture.ids[0]);
                CHECK(events[i].jbutton.button == raw[index] && events[i].jbutton.down == down);
            }
            ++seen;
        }
        CHECK(seen == 20);
    }
    for (int i = 0; i < 5; ++i) CHECK(!SDL_GetGamepadButton(fixture.gamepads[0], buttons[i]));
    CloseFixture(&fixture);
}

static void NoAcquisitionRemapEvents(void)
{
    SDL_Event events[64];
    int count, total = 0;
    while ((count = SDL_PeepEvents(events, 64, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) > 0) {
        total += count;
        REQUIRE(total < 8192);
        for (int i = 0; i < count; ++i) CHECK(events[i].type != SDL_EVENT_GAMEPAD_REMAPPED);
    }
    REQUIRE(count == 0);
}

static void ZeroInputAndStableCapabilities(void)
{
    Fixture fixture = OpenFixture(true, 15);
    SDL_Gamepad *gamepad = fixture.gamepads[0];
    SDL_Joystick *joystick = fixture.joysticks[0];
    SDL_GamepadBinding *bindings = gamepad->bindings;
    const int binding_count = gamepad->num_bindings;
    char *original_mapping = SDL_GetGamepadMapping(gamepad);
    Sint16 raw_axes[6], gamepad_axes[6];
    Uint8 hat, last_hat;
    REQUIRE(original_mapping != NULL);
    CheckDefaultBindings(gamepad, 15);
    for (int i = 0; i < 4; ++i) CHECK(!SDL_GetGamepadButton(gamepad, paddle_buttons[i]));
    NoAcquisitionRemapEvents();
    // No source frames have been injected. Present controls remain zero.
    for (int update = 0; update < 3; ++update) {
        SDL_UpdateJoysticks();
        for (int i = 0; i < 4; ++i) CHECK(!SDL_GetGamepadButton(gamepad, paddle_buttons[i]));
        CheckDefaultBindings(gamepad, 15);
    }
    REQUIRE(SDL_SetJoystickVirtualAxis(joystick, 4, SDL_JOYSTICK_AXIS_MIN));
    REQUIRE(SDL_SetJoystickVirtualAxis(joystick, 5, SDL_JOYSTICK_AXIS_MIN));
    SDL_UpdateJoysticks();
    REQUIRE(SDL_SetJoystickVirtualAxis(joystick, 0, 12000));
    REQUIRE(SDL_SetJoystickVirtualAxis(joystick, 4, 22000));
    REQUIRE(SDL_SetJoystickVirtualAxis(joystick, 5, 18000));
    REQUIRE(SDL_SetJoystickVirtualHat(joystick, 0, SDL_HAT_UP));
    SDL_UpdateJoysticks();
    CHECK(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0);
    CHECK(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0);
    for (int i = 0; i < 6; ++i) {
        raw_axes[i] = SDL_GetJoystickAxis(joystick, i);
        gamepad_axes[i] = SDL_GetGamepadAxis(gamepad, (SDL_GamepadAxis)i);
    }
    hat = joystick->hats[0];
    last_hat = gamepad->last_hat_mask[0];
    for (int down = 1; down >= 0; --down) {
        // Zero publication models the core input boundary after acquisition
        // loss. It does not execute a transport or a failure handler.
        for (int i = 0; i < 4; ++i) REQUIRE(SDL_SetJoystickVirtualButton(joystick, paddle_raw[i], down != 0));
        SDL_UpdateJoysticks();
        for (int i = 0; i < 4; ++i) CHECK(SDL_GetGamepadButton(gamepad, paddle_buttons[i]) == (down != 0));
        CheckDefaultBindings(gamepad, 15);
        CHECK(gamepad->bindings == bindings && gamepad->num_bindings == binding_count);
        for (int i = 0; i < 6; ++i) {
            CHECK(SDL_GetJoystickAxis(joystick, i) == raw_axes[i]);
            CHECK(SDL_GetGamepadAxis(gamepad, (SDL_GamepadAxis)i) == gamepad_axes[i]);
        }
        CHECK(joystick->hats[0] == hat && gamepad->last_hat_mask[0] == last_hat);
        {
            char *current_mapping = SDL_GetGamepadMapping(gamepad);
            REQUIRE(current_mapping != NULL);
            CHECK(SDL_strcmp(original_mapping, current_mapping) == 0);
            SDL_free(current_mapping);
        }
        NoAcquisitionRemapEvents();
    }
    SDL_free(original_mapping);
    CloseFixture(&fixture);
}

static void ExplicitPrecedence(void)
{
    const char *paddle_fields[] = { " Pa Dd Le1 : b16,", "+paddle1:b16,", "-paddle1:b16," };
    Fixture fixture = OpenFixture(false, 15);
    SetBase(&fixture, "misc2:b12,");
    OpenGamepads(&fixture);
    CheckDefaultBindings(fixture.gamepads[0], 14);
    CHECK(RawBinding(fixture.gamepads[0], SDL_GAMEPAD_BUTTON_MISC2) == 12);
    REQUIRE(SDL_SetJoystickVirtualButton(fixture.joysticks[0], 12, true));
    SDL_UpdateJoysticks();
    CHECK(SDL_GetGamepadButton(fixture.gamepads[0], SDL_GAMEPAD_BUTTON_MISC2));
    CHECK(!SDL_GetGamepadButton(fixture.gamepads[0], SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1));
    CheckGetters(&fixture, 0);
    REQUIRE(SDL_SetJoystickVirtualButton(fixture.joysticks[0], 12, false));
    SDL_UpdateJoysticks();
    for (int i = 0; i < (int)SDL_arraysize(paddle_fields); ++i) {
        REQUIRE(SDL_SetJoystickVirtualButton(fixture.joysticks[0], 12, false));
        REQUIRE(SDL_SetJoystickVirtualButton(fixture.joysticks[0], 16, false));
        SDL_UpdateJoysticks();
        SetBase(&fixture, paddle_fields[i]);
        CHECK(RawBinding(fixture.gamepads[0], SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1) == 16);
        CHECK(fixture.gamepads[0]->xinput_paddle_count == 3);
        REQUIRE(SDL_SetJoystickVirtualButton(fixture.joysticks[0], 12, true));
        SDL_UpdateJoysticks();
        CHECK(!SDL_GetGamepadButton(fixture.gamepads[0], SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1));
        REQUIRE(SDL_SetJoystickVirtualButton(fixture.joysticks[0], 16, true));
        SDL_UpdateJoysticks();
        CHECK(SDL_GetGamepadButton(fixture.gamepads[0], SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1));
        CheckGetters(&fixture, 0);
    }
    CloseFixture(&fixture);
}

static void AllocationFailureAndRetry(void)
{
    Fixture fixture = OpenFixture(true, 15);
    SDL_Gamepad *gamepad = fixture.gamepads[0];
    const int base_count = gamepad->num_bindings - gamepad->xinput_paddle_count;
    allocation_gamepad = gamepad;
    allocation_size = gamepad->num_bindings * sizeof(*gamepad->bindings);
    allocation_action = 1;
    allocation_hits = 0;
    ReloadForAllocationTest(gamepad);
    CHECK(allocation_hits == 1);
    CHECK(gamepad->num_bindings == base_count);
    CHECK(gamepad->xinput_paddle_count == 0);
    CheckDefaultBindings(gamepad, 0);
    CheckGetters(&fixture, 0);
    ReloadForAllocationTest(gamepad);
    CheckDefaultBindings(gamepad, 15);
    CheckGetters(&fixture, 0);
    CloseFixture(&fixture);
}

static void RawOnlyGetter(void)
{
    Fixture fixture = OpenFixture(false, 15);
    char *mapping;
    SetBase(&fixture, "misc2:b12,");
    mapping = SDL_GetGamepadMappingForID(fixture.ids[0]);
    REQUIRE(mapping != NULL);
    CHECK(SDL_strstr(mapping, "paddle1:b12") == NULL);
    fixture.gamepads[0] = SDL_OpenGamepad(fixture.ids[0]);
    REQUIRE(fixture.gamepads[0] != NULL);
    CheckDefaultBindings(fixture.gamepads[0], 14);
    CheckStringAgainstBindings(fixture.gamepads[0], mapping);
    SDL_free(mapping);
    CloseFixture(&fixture);
}

static void GlobalMappingUpdate(void)
{
    Fixture fixture = OpenFixture(true, 15);
    char mapping[1024];
    const char *updated = "a:b1,b:b0,x:b2,y:b3,misc1:b11,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,dpup:h0.1,";
    MappingText(&fixture, updated, NULL, mapping, sizeof(mapping));
    REQUIRE(SDL_AddGamepadMapping(mapping) >= 0);
    CheckDefaultBindings(fixture.gamepads[0], 15);
    CheckDefaultBindings(fixture.gamepads[1], 0);
    for (int i = 0; i < 2; ++i) {
        CHECK(RawBinding(fixture.gamepads[i], SDL_GAMEPAD_BUTTON_SOUTH) == 1);
        REQUIRE(SDL_SetJoystickVirtualButton(fixture.joysticks[i], 1, true));
    }
    SDL_UpdateJoysticks();
    for (int i = 0; i < 2; ++i) {
        CHECK(SDL_GetGamepadButton(fixture.gamepads[i], SDL_GAMEPAD_BUTTON_SOUTH));
        CheckGetters(&fixture, i);
    }
    CloseFixture(&fixture);
}

static void EmptyExplicitField(void)
{
    const char *fields[] = { "paddle1:,", "paddle1:+,", "paddle1:-,", "+paddle1:,", "-paddle1:," };
    for (int i = 0; i < (int)SDL_arraysize(fields); ++i) {
        Fixture fixture = OpenFixture(false, 15);
        SetBase(&fixture, fields[i]);
        OpenGamepads(&fixture);
        CheckDefaultBindings(fixture.gamepads[0], 14);
        CheckGetters(&fixture, 0);
        CloseFixture(&fixture);
    }
}

static void Run(const char *name, void (*test)(void))
{
    unsigned before = failures;
    ++groups;
    printf("RUN %s\n", name);
    test();
    printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
}

int main(int argc, char **argv)
{
    const char *disabled[] = {
        SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_RAWINPUT, SDL_HINT_JOYSTICK_DIRECTINPUT,
        SDL_HINT_XINPUT_ENABLED, SDL_HINT_JOYSTICK_WGI, SDL_HINT_JOYSTICK_GAMEINPUT,
        SDL_HINT_JOYSTICK_GAMEINPUT_RAW, SDL_HINT_JOYSTICK_BLE_SWITCH2,
        SDL_HINT_JOYSTICK_THREAD, "SDL_JOYSTICK_WINMM", "SDL_JOYSTICK_ROG_CHAKRAM"
    };
    int initial_count = -1;
    SDL_JoystickID *initial;
    setvbuf(stdout, NULL, _IONBF, 0);
    SDL_SetMainReady();
    SDL_GetMemoryFunctions(&base_malloc, &base_calloc, &base_realloc, &base_free);
    REQUIRE(SDL_SetMemoryFunctions(base_malloc, base_calloc, TargetedRealloc, base_free));
    for (int i = 0; i < (int)SDL_arraysize(disabled); ++i)
        REQUIRE(SDL_SetHintWithPriority(disabled[i], "0", SDL_HINT_OVERRIDE));
    REQUIRE(SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1", SDL_HINT_OVERRIDE));
    REQUIRE(SDL_SetHintWithPriority(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0", SDL_HINT_OVERRIDE));
    REQUIRE(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_SetJoystickEventsEnabled(true);
    SDL_SetGamepadEventsEnabled(true);
    initial = SDL_GetJoysticks(&initial_count);
    REQUIRE(initial != NULL && initial_count == 0);
    SDL_free(initial);
    puts("SCOPE virtual input only; synthetic XInput predicate; no producer or acquisition coverage");
    if (argc == 2 && SDL_strcmp(argv[1], "--empty-field") == 0) {
        Run("explicit empty paddle field", EmptyExplicitField);
    } else {
        Run("capabilities present at Open and equal GUID isolation", IsolationAndPolicies);
        Run("face, Share, paddles, current values, and event order", InputAndEventOrder);
        Run("zero input and stable capability mappings", ZeroInputAndStableCapabilities);
        Run("raw-button collision, spaces, and case", ExplicitPrecedence);
        Run("base-map append OOM, getters, and loader retry", AllocationFailureAndRetry);
        Run("raw joystick only mapping getter", RawOnlyGetter);
        Run("global base mapping update while qualified", GlobalMappingUpdate);
    }
    SDL_Quit();
    REQUIRE(SDL_SetMemoryFunctions(base_malloc, base_calloc, base_realloc, base_free));
    printf("SUMMARY groups=%u checks=%u failures=%u\n", groups, checks, failures);
    return failures ? 1 : 0;
}
