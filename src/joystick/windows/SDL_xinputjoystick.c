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

#include "../SDL_sysjoystick.h"
#include "SDL_xinputjoystick_c.h"

#ifdef SDL_JOYSTICK_XINPUT

#include "../../core/windows/SDL_xinput.h"
#include "SDL_xinput_paddle_c.h"
#include "SDL_xinputjoystick_hwdata.h"
#include "../../core/windows/SDL_gameinput.h"
#include "../hidapi/SDL_hidapijoystick_c.h"
#include "../SDL_rb3pro_proto.h"

// The XInput slot and subtype, also used as the unique ID for a device
struct SDL_JoystickDeviceData
{
    Uint8 userid;
    BYTE SubType;
};

// local variables
static bool s_bXInputEnabled = false;

bool SDL_XINPUT_Enabled(void)
{
    return s_bXInputEnabled;
}

static const char *GetXInputName(const Uint8 userid, BYTE SubType, char *name, size_t maxlen)
{
    switch (SubType) {
    case XINPUT_DEVSUBTYPE_GAMEPAD:
        (void)SDL_snprintf(name, maxlen, "XInput Controller #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_WHEEL:
        (void)SDL_snprintf(name, maxlen, "XInput Wheel #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_ARCADE_STICK:
        (void)SDL_snprintf(name, maxlen, "XInput ArcadeStick #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_FLIGHT_STICK:
        (void)SDL_snprintf(name, maxlen, "XInput FlightStick #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_DANCE_PAD:
        (void)SDL_snprintf(name, maxlen, "XInput DancePad #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_GUITAR:
    case XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE:
    case XINPUT_DEVSUBTYPE_GUITAR_BASS:
        (void)SDL_snprintf(name, maxlen, "XInput Guitar #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_DRUM_KIT:
        (void)SDL_snprintf(name, maxlen, "XInput DrumKit #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_ARCADE_PAD:
        (void)SDL_snprintf(name, maxlen, "XInput ArcadePad #%d", 1 + userid);
        break;
    case SDL_RB3PRO_XINPUT_SUBTYPE_KEYBOARD:
    case SDL_RB3PRO_XINPUT_SUBTYPE_GUITAR:
        return SDL_RB3Pro_XInputName(SDL_RB3Pro_VariantForXInputSubtype(SubType));
    default:
        (void)SDL_snprintf(name, maxlen, "XInput Device #%d", 1 + userid);
        break;
    }
    return name;
}

static bool GetXInputDeviceInfo(Uint8 userid, Uint16 *pVID, Uint16 *pPID, Uint16 *pVersion, int *pSteamVirtualGamepadSlot)
{
    SDL_XINPUT_CAPABILITIES_EX capabilities;

    if (pSteamVirtualGamepadSlot) {
        *pSteamVirtualGamepadSlot = -1;
    }

    if (!XINPUTGETCAPABILITIESEX || XINPUTGETCAPABILITIESEX(1, userid, 0, &capabilities) != ERROR_SUCCESS) {
        // Use a generic VID/PID representing an XInput controller
        if (pVID) {
            *pVID = USB_VENDOR_MICROSOFT;
        }
        if (pPID) {
            *pPID = USB_PRODUCT_XBOX360_XUSB_CONTROLLER;
        }
        return false;
    }

    if (pSteamVirtualGamepadSlot &&
        capabilities.VendorId == USB_VENDOR_VALVE &&
        capabilities.ProductId == USB_PRODUCT_STEAM_VIRTUAL_GAMEPAD) {
        *pSteamVirtualGamepadSlot = (int)capabilities.unk2;
    }

    // Fixup for Wireless Xbox 360 Controller
    if (capabilities.ProductId == 0 && capabilities.Capabilities.Flags & XINPUT_CAPS_WIRELESS) {
        capabilities.VendorId = USB_VENDOR_MICROSOFT;
        capabilities.ProductId = USB_PRODUCT_XBOX360_XUSB_CONTROLLER;
    }

    if (pVID) {
        *pVID = capabilities.VendorId;
    }
    if (pPID) {
        *pPID = capabilities.ProductId;
    }
    if (pVersion) {
        *pVersion = capabilities.ProductVersion;
    }
    return true;
}

#ifdef SDL_JOYSTICK_XINPUT_PADDLES
static void XINPUT_FreeDevice(SDL_JoystickDevice *device)
{
    // A published device is being removed, so retire the paddles of its open joystick
    if (device->published && !SDL_JoysticksQuitting()) {
        SDL_XINPUT_PaddleRemoved(device->instance_id);
    }
}
#endif

static SDL_JoystickDevice *CreateXInputDevice(Uint8 userid, BYTE SubType)
{
    Uint16 vendor = 0;
    Uint16 product = 0;
    Uint16 version = 0;
    int steam_virtual_gamepad_slot = -1;

    if (SubType == XINPUT_DEVSUBTYPE_UNKNOWN) {
        return NULL;
    }

    char name[32];
    GetXInputName(userid, SubType, name, sizeof(name));
    GetXInputDeviceInfo(userid, &vendor, &product, &version, &steam_virtual_gamepad_slot);

    SDL_JoystickDevice *device = SDL_CreateJoystickDevice(&SDL_XINPUT_JoystickDriver, sizeof(SDL_JoystickDeviceData));
    if (!device) {
        return NULL;
    }

    SDL_JoystickDeviceData *data = (SDL_JoystickDeviceData *)(device + 1);
    data->userid = userid;
    data->SubType = SubType;
    device->internal = data;
    device->unique_id = data;
    device->unique_id_len = sizeof(*data);
#ifdef SDL_JOYSTICK_XINPUT_PADDLES
    device->free = XINPUT_FreeDevice;
#endif

    device->name = SDL_CreateJoystickName(vendor, product, NULL, name);
    if (!device->name) {
        SDL_DestroyJoystickDevice(device);
        return NULL;
    }

    if (SDL_asprintf(&device->path, "XInput#%u", userid) < 0) {
        SDL_DestroyJoystickDevice(device);
        return NULL;
    }

    device->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_USB, vendor, product, version, NULL, name, 'x', SubType);
    device->player_index = userid;
    device->steam_virtual_gamepad_slot = steam_virtual_gamepad_slot;

    SDL_AddJoystickDeviceMatchXInput(device, userid);
    SDL_AddJoystickDeviceMatchVIDPID(device, vendor, product);

    return device;
}

static void UpdateXInputJoystickBatteryInformation(SDL_Joystick *joystick, XINPUT_BATTERY_INFORMATION_EX *pBatteryInformation)
{
    SDL_PowerState state;
    int percent;
    switch (pBatteryInformation->BatteryType) {
    case BATTERY_TYPE_WIRED:
        state = SDL_POWERSTATE_CHARGING;
        break;
    case BATTERY_TYPE_UNKNOWN:
    case BATTERY_TYPE_DISCONNECTED:
        state = SDL_POWERSTATE_UNKNOWN;
        break;
    default:
        state = SDL_POWERSTATE_ON_BATTERY;
        break;
    }
    if (state == SDL_POWERSTATE_ON_BATTERY || state == SDL_POWERSTATE_CHARGING) {
        switch (pBatteryInformation->BatteryLevel) {
        case BATTERY_LEVEL_EMPTY:
            percent = 10;
            break;
        case BATTERY_LEVEL_LOW:
            percent = 40;
            break;
        case BATTERY_LEVEL_MEDIUM:
            percent = 70;
            break;
        default:
        case BATTERY_LEVEL_FULL:
            percent = 100;
            break;
        }
    } else {
        percent = -1;
    }
    SDL_SendJoystickPowerInfo(joystick, state, percent);
}

static void UpdateXInputJoystickState(SDL_Joystick *joystick, XINPUT_STATE *pXInputState, XINPUT_BATTERY_INFORMATION_EX *pBatteryInformation)
{
    static WORD s_XInputButtons[] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_GUIDE
    };
    WORD wButtons = pXInputState->Gamepad.wButtons;
    Uint8 button;
    Uint8 hat = SDL_HAT_CENTERED;
    Uint64 timestamp = SDL_GetTicksNS();

    SDL_SendJoystickAxis(timestamp, joystick, 0, pXInputState->Gamepad.sThumbLX);
    SDL_SendJoystickAxis(timestamp, joystick, 1, ~pXInputState->Gamepad.sThumbLY);
    SDL_SendJoystickAxis(timestamp, joystick, 2, ((int)pXInputState->Gamepad.bLeftTrigger * 257) - 32768);
    SDL_SendJoystickAxis(timestamp, joystick, 3, pXInputState->Gamepad.sThumbRX);
    SDL_SendJoystickAxis(timestamp, joystick, 4, ~pXInputState->Gamepad.sThumbRY);
    SDL_SendJoystickAxis(timestamp, joystick, 5, ((int)pXInputState->Gamepad.bRightTrigger * 257) - 32768);

    for (button = 0; button < (Uint8)SDL_arraysize(s_XInputButtons); ++button) {
        bool down = ((wButtons & s_XInputButtons[button]) != 0);
        SDL_SendJoystickButton(timestamp, joystick, button, down);
    }

    if (wButtons & XINPUT_GAMEPAD_DPAD_UP) {
        hat |= SDL_HAT_UP;
    }
    if (wButtons & XINPUT_GAMEPAD_DPAD_DOWN) {
        hat |= SDL_HAT_DOWN;
    }
    if (wButtons & XINPUT_GAMEPAD_DPAD_LEFT) {
        hat |= SDL_HAT_LEFT;
    }
    if (wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) {
        hat |= SDL_HAT_RIGHT;
    }
    SDL_SendJoystickHat(timestamp, joystick, 0, hat);

    UpdateXInputJoystickBatteryInformation(joystick, pBatteryInformation);
}

/* A Rock Band 3 Pro instrument. XInput passes its bit-packed fields through,
   so SDL_rb3pro_proto.c decodes the raw state, before any inversion.
   extra is the six trailing XUSB bytes, or NULL where only XInput's own
   twelve are known. */
static void UpdateXInputRB3ProState(SDL_Joystick *joystick, const XINPUT_STATE *pXInputState, const BYTE *extra, XINPUT_BATTERY_INFORMATION_EX *pBatteryInformation)
{
    SDL_RB3ProXInputState state;
    SDL_RB3ProOutput output;
    Uint64 timestamp = SDL_GetTicksNS();
    Uint8 i;

    SDL_zero(state);
    state.buttons = pXInputState->Gamepad.wButtons;
    state.left_trigger = pXInputState->Gamepad.bLeftTrigger;
    state.right_trigger = pXInputState->Gamepad.bRightTrigger;
    state.thumb_lx = pXInputState->Gamepad.sThumbLX;
    state.thumb_ly = pXInputState->Gamepad.sThumbLY;
    state.thumb_rx = pXInputState->Gamepad.sThumbRX;
    state.thumb_ry = pXInputState->Gamepad.sThumbRY;
    if (extra) {
        state.has_trailing = true;
        SDL_memcpy(state.trailing, extra, sizeof(state.trailing));
    }

    if (SDL_RB3Pro_DecodeXInput(joystick->hwdata->rb3pro_variant, &state, &output)) {
        for (i = 0; i < 64; ++i) {
            if (output.button_mask & ((Uint64)1 << i)) {
                SDL_SendJoystickButton(timestamp, joystick, i, ((output.buttons >> i) & 1) != 0);
            }
        }
        for (i = 0; i < SDL_RB3PRO_MAX_AXES; ++i) {
            if (output.axis_mask & (1u << i)) {
                // Keys, frets and velocities are data: seed past the anti-jitter gate
                SDL_SeedJoystickDataAxis(joystick, i, output.axes[i]);
                SDL_SendJoystickAxis(timestamp, joystick, i, output.axes[i]);
            }
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, output.hat);
    }

    UpdateXInputJoystickBatteryInformation(joystick, pBatteryInformation);
}

static bool XINPUT_JoystickInit(void)
{
    bool enabled = true;

    if (!SDL_GetHintBoolean(SDL_HINT_XINPUT_ENABLED, true) ||
        SDL_UsingGameInputForXInputControllers()) {
        enabled = false;
    }

    if (enabled && !WIN_LoadXInputDLL()) {
        enabled = false; // oh well.
    }
    s_bXInputEnabled = enabled;

    return true;
}

static SDL_JoystickDevice *XINPUT_JoystickQueryDevices(void)
{
    SDL_JoystickDevice *devices = NULL;

    if (!s_bXInputEnabled) {
        return NULL;
    }

    // iterate in reverse, so these are in the final list in ascending numeric order.
    for (int iuserid = XUSER_MAX_COUNT - 1; iuserid >= 0; iuserid--) {
        const Uint8 userid = (Uint8)iuserid;
        XINPUT_CAPABILITIES capabilities;
        if (XINPUTGETCAPABILITIES(userid, XINPUT_FLAG_GAMEPAD, &capabilities) == ERROR_SUCCESS) {
            SDL_JoystickDevice *device = CreateXInputDevice(userid, capabilities.SubType);
            if (device) {
                device->next = devices;
                devices = device;
            }
        }
    }
    return devices;
}

static void XINPUT_JoystickUpdateSteamVirtualGamepadSlot(SDL_JoystickDevice *device)
{
    const SDL_JoystickDeviceData *data = device->internal;
    Uint16 vendor, product;
    int steam_virtual_gamepad_slot;
    char name[32];

    // Steam can move a different controller into this XInput slot
    if (GetXInputDeviceInfo(data->userid, &vendor, &product, NULL, &steam_virtual_gamepad_slot)) {
        char *joystick_name = SDL_CreateJoystickName(vendor, product, NULL, GetXInputName(data->userid, data->SubType, name, sizeof(name)));
        if (joystick_name) {
            SDL_free(device->name);
            device->name = joystick_name;
        }
    }
    device->steam_virtual_gamepad_slot = steam_virtual_gamepad_slot;
}

static bool XINPUT_JoystickOpen(SDL_Joystick *joystick, SDL_JoystickDevice *device)
{
    // allocate memory for system specific hardware data
    joystick->hwdata = (struct joystick_hwdata *)SDL_calloc(1, sizeof(struct joystick_hwdata));
    if (!joystick->hwdata) {
        return false;
    }

    const Uint8 userId = device->internal->userid;
    XINPUT_CAPABILITIES capabilities;

    if (XINPUTGETCAPABILITIES(userId, XINPUT_FLAG_GAMEPAD, &capabilities) != ERROR_SUCCESS) {
        SDL_free(joystick->hwdata);
        joystick->hwdata = NULL;
        return SDL_SetError("Failed to obtain XInput device capabilities. Device disconnected?");
    }
    joystick->hwdata->userid = userId;
    // The paddle reader reads the enumerated GUID here, before SDL_OpenJoystick sets joystick->guid
    joystick->hwdata->guid = device->guid;

    joystick->hwdata->rb3pro_variant = SDL_RB3Pro_VariantForXInputSubtype(device->SubType);
    if (joystick->hwdata->rb3pro_variant) {
        // The Rock Band 3 Pro instruments take the layout of their PS3 models
        int nbuttons, naxes, nhats;

        SDL_RB3Pro_GetLayout(joystick->hwdata->rb3pro_variant, &nbuttons, &naxes, &nhats);
        joystick->naxes = naxes;
        joystick->nbuttons = nbuttons;
        joystick->nhats = nhats;
    } else {
        // The XInput API has a hard coded button/axis mapping, so we just match it.
        // OpenXInput exposes Share via XInputGetSystemButtons (ordinal 109). When
        // that pointer is non-NULL the controller's Share button lands at index 11.
        joystick->naxes = 6;
        joystick->nbuttons = SDL_XInputGetSystemButtons ? 12 : 11;
        joystick->nhats = 1;
    }

#ifdef SDL_JOYSTICK_XINPUT_PADDLES
    SDL_XINPUT_PaddleOpen(joystick, userId);
#endif

    SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, true);

    return true;
}

static bool XINPUT_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    XINPUT_VIBRATION XVibration;

    if (!XINPUTSETSTATE) {
        return SDL_Unsupported();
    }

    XVibration.wLeftMotorSpeed = low_frequency_rumble;
    XVibration.wRightMotorSpeed = high_frequency_rumble;
    if (XINPUTSETSTATE(joystick->hwdata->userid, &XVibration) != ERROR_SUCCESS) {
        return SDL_SetError("XInputSetState() failed");
    }
    return true;
}

static void XINPUT_JoystickUpdate(SDL_Joystick *joystick)
{
    if (!joystick->hwdata) {
        return;
    }

    DWORD result;
    XINPUT_STATE XInputState;
    XINPUT_BATTERY_INFORMATION_EX XBatteryInformation;
    SDL_XINPUT_STATE_EXTENDED_V1 extended;
    const BYTE *extra = NULL;

    if (!XINPUTGETSTATE) {
        return;
    }

    if (joystick->hwdata->rb3pro_variant && SDL_XInputGetStateExtended) {
        // The same reply, with the six bytes XInputGetState leaves out
        SDL_zero(extended);
        extended.cbSize = sizeof(extended);
        result = SDL_XInputGetStateExtended(joystick->hwdata->userid, &extended);
        XInputState = extended.state;
        if (result == ERROR_SUCCESS && extended.extraByteCount == sizeof(extended.extraBytes)) {
            extra = extended.extraBytes;
        }
    } else {
        result = XINPUTGETSTATE(joystick->hwdata->userid, &XInputState);
    }
    if (result == ERROR_DEVICE_NOT_CONNECTED) {
#ifdef SDL_JOYSTICK_XINPUT_PADDLES
        SDL_XINPUT_PaddleRemoved(joystick->instance_id);
#endif
        SDL_RequestJoystickDeviceUpdate(true);
        return;
    }

    // FIXME: This does end up making a device ioctl() to query data, we shouldn't do this every update.
    SDL_zero(XBatteryInformation);
    if (XINPUTGETBATTERYINFORMATION) {
        result = XINPUTGETBATTERYINFORMATION(joystick->hwdata->userid, BATTERY_DEVTYPE_GAMEPAD, &XBatteryInformation);
    }

#if defined(SDL_PLATFORM_XBOXONE) || defined(SDL_PLATFORM_XBOXSERIES)
    // XInputOnGameInput doesn't ever change dwPacketNumber, so have to just update every frame
    if (joystick->hwdata->rb3pro_variant) {
        UpdateXInputRB3ProState(joystick, &XInputState, extra, &XBatteryInformation);
    } else {
        UpdateXInputJoystickState(joystick, &XInputState, &XBatteryInformation);
    }
#else
    /* Only fire events if the data changed from last time. The Rock Band 3
       Pro instruments also post when only their six trailing bytes changed:
       no source says whether dwPacketNumber counts them. */
    if (XInputState.dwPacketNumber &&
        (XInputState.dwPacketNumber != joystick->hwdata->dwPacketNumber ||
         (extra && SDL_memcmp(extra, joystick->hwdata->rb3pro_extra, sizeof(joystick->hwdata->rb3pro_extra)) != 0))) {
        if (joystick->hwdata->rb3pro_variant) {
            UpdateXInputRB3ProState(joystick, &XInputState, extra, &XBatteryInformation);
        } else {
            UpdateXInputJoystickState(joystick, &XInputState, &XBatteryInformation);
        }
        joystick->hwdata->dwPacketNumber = XInputState.dwPacketNumber;
        if (extra) {
            SDL_memcpy(joystick->hwdata->rb3pro_extra, extra, sizeof(joystick->hwdata->rb3pro_extra));
        }
    }
#endif

    // Poll the OpenXInput Share button channel (ordinal 109) every frame.
    // The API has no packet number, so we read each tick and rely on
    // SDL_SendJoystickButton to deduplicate unchanged state. The Rock Band 3
    // Pro instruments have no Share button: the byte OpenXInput reads for it
    // carries their pedal connection, and button 11 is overdrive or solo.
    if (SDL_XInputGetSystemButtons && !joystick->hwdata->rb3pro_variant) {
        XINPUT_SYSTEM_BUTTONS sys;
        SDL_zero(sys);
        if (SDL_XInputGetSystemButtons(joystick->hwdata->userid, &sys, NULL) == ERROR_SUCCESS) {
            bool share_down = (sys.ExtraSystemButtons & XINPUT_GAMEPAD_EXTRAS_SHARE) != 0;
            SDL_SendJoystickButton(SDL_GetTicksNS(), joystick, 11, share_down);
        }
    }
#ifdef SDL_JOYSTICK_XINPUT_PADDLES
    // The supplemental reports have their own sequence, separate from XInput.
    SDL_XINPUT_PaddleUpdate(joystick);
#endif
}

static void XINPUT_JoystickClose(SDL_Joystick *joystick)
{
#ifdef SDL_JOYSTICK_XINPUT_PADDLES
    SDL_XINPUT_PaddleClose(joystick);
#endif
    SDL_free(joystick->hwdata);
}

static void XINPUT_JoystickQuit(void)
{
    if (s_bXInputEnabled) {
#ifdef SDL_JOYSTICK_XINPUT_PADDLES
        SDL_XINPUT_PaddleQuit();
#endif
        s_bXInputEnabled = false;
        WIN_UnloadXInputDLL();
    }
}

// This file may be compiled as C++, so don't use designated initializers here
SDL_JoystickDriver SDL_XINPUT_JoystickDriver =
{
    XINPUT_JoystickInit,
    NULL, // InitUpdates
    NULL, // QuitUpdates
    XINPUT_JoystickQueryDevices,
    NULL, // DetectDevices
    NULL, // PrepareDevice
    XINPUT_JoystickUpdateSteamVirtualGamepadSlot,
    NULL, // GetGamepadMapping
    XINPUT_JoystickOpen,
    XINPUT_JoystickRumble,
    NULL, // RumbleTriggers
    NULL, // SetLED
    NULL, // SendEffect
    NULL, // SetSensorsEnabled
    NULL, // SetPlayerIndex
    XINPUT_JoystickUpdate,
    XINPUT_JoystickClose,
    XINPUT_JoystickQuit
};

#else // !SDL_JOYSTICK_XINPUT

bool SDL_XINPUT_Enabled(void)
{
    return false;
}

#endif // SDL_JOYSTICK_XINPUT
