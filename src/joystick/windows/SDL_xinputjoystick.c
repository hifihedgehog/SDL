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

#ifdef SDL_JOYSTICK_XINPUT

#include "SDL_windowsjoystick_c.h"
#include "SDL_xinputjoystick_c.h"
#include "SDL_xinput_paddle_c.h"
#include "SDL_rawinputjoystick_c.h"
#include "../../core/windows/SDL_gameinput.h"
#include "../hidapi/SDL_hidapijoystick_c.h"
#include "../SDL_rb3pro_proto.h"

// Set up for C function definitions, even when using C++
#ifdef __cplusplus
extern "C" {
#endif

// Internal stuff

static bool s_bXInputEnabled = false;

bool SDL_XINPUT_Enabled(void)
{
    return s_bXInputEnabled;
}

bool SDL_XINPUT_JoystickInit(void)
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

static const char *GetXInputName(const Uint8 userid, BYTE SubType)
{
    static char name[32];

    switch (SubType) {
    case XINPUT_DEVSUBTYPE_GAMEPAD:
        (void)SDL_snprintf(name, sizeof(name), "XInput Controller #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_WHEEL:
        (void)SDL_snprintf(name, sizeof(name), "XInput Wheel #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_ARCADE_STICK:
        (void)SDL_snprintf(name, sizeof(name), "XInput ArcadeStick #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_FLIGHT_STICK:
        (void)SDL_snprintf(name, sizeof(name), "XInput FlightStick #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_DANCE_PAD:
        (void)SDL_snprintf(name, sizeof(name), "XInput DancePad #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_GUITAR:
    case XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE:
    case XINPUT_DEVSUBTYPE_GUITAR_BASS:
        (void)SDL_snprintf(name, sizeof(name), "XInput Guitar #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_DRUM_KIT:
        (void)SDL_snprintf(name, sizeof(name), "XInput DrumKit #%d", 1 + userid);
        break;
    case XINPUT_DEVSUBTYPE_ARCADE_PAD:
        (void)SDL_snprintf(name, sizeof(name), "XInput ArcadePad #%d", 1 + userid);
        break;
    case SDL_RB3PRO_XINPUT_SUBTYPE_KEYBOARD:
    case SDL_RB3PRO_XINPUT_SUBTYPE_GUITAR:
        return SDL_RB3Pro_XInputName(SDL_RB3Pro_VariantForXInputSubtype(SubType));
    default:
        (void)SDL_snprintf(name, sizeof(name), "XInput Device #%d", 1 + userid);
        break;
    }
    return name;
}

static bool GetXInputDeviceInfo(Uint8 userid, Uint16 *pVID, Uint16 *pPID, Uint16 *pVersion)
{
    SDL_XINPUT_CAPABILITIES_EX capabilities;

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

int SDL_XINPUT_GetSteamVirtualGamepadSlot(Uint8 userid)
{
    SDL_XINPUT_CAPABILITIES_EX capabilities;

    if (XINPUTGETCAPABILITIESEX &&
        XINPUTGETCAPABILITIESEX(1, userid, 0, &capabilities) == ERROR_SUCCESS &&
        capabilities.VendorId == USB_VENDOR_VALVE &&
        capabilities.ProductId == USB_PRODUCT_STEAM_VIRTUAL_GAMEPAD) {
        return (int)capabilities.unk2;
    }
    return -1;
}

static void AddXInputDevice(Uint8 userid, BYTE SubType, JoyStick_DeviceData **pContext)
{
    const char *name = NULL;
    Uint16 vendor = 0;
    Uint16 product = 0;
    Uint16 version = 0;
    JoyStick_DeviceData *pPrevJoystick = NULL;
    JoyStick_DeviceData *pNewJoystick = *pContext;

#ifdef SDL_JOYSTICK_RAWINPUT
    if (RAWINPUT_IsEnabled()) {
        // The raw input driver handles more than 4 controllers, so prefer that when available
        /* We do this check here rather than at the top of SDL_XINPUT_JoystickDetect() because
           we need to check XInput state before RAWINPUT gets a hold of the device, otherwise
           when a controller is connected via the wireless adapter, it will shut down at the
           first subsequent XInput call. This seems like a driver stack bug?

           Reference: https://github.com/libsdl-org/SDL/issues/3468
         */
        return;
    }
#endif

    if (SubType == XINPUT_DEVSUBTYPE_UNKNOWN) {
        return;
    }

    while (pNewJoystick) {
        if (pNewJoystick->bXInputDevice && (pNewJoystick->XInputUserId == userid) && (pNewJoystick->SubType == SubType)) {
            // if we are replacing the front of the list then update it
            if (pNewJoystick == *pContext) {
                *pContext = pNewJoystick->pNext;
            } else if (pPrevJoystick) {
                pPrevJoystick->pNext = pNewJoystick->pNext;
            }

            pNewJoystick->pNext = SYS_Joystick;
            SYS_Joystick = pNewJoystick;
            return; // already in the list.
        }

        pPrevJoystick = pNewJoystick;
        pNewJoystick = pNewJoystick->pNext;
    }

    name = GetXInputName(userid, SubType);
    GetXInputDeviceInfo(userid, &vendor, &product, &version);
    if (SDL_ShouldIgnoreJoystick(vendor, product, version, name) ||
        SDL_JoystickHandledByAnotherDriver(&SDL_WINDOWS_JoystickDriver, vendor, product, version, name)) {
        return;
    }

    pNewJoystick = (JoyStick_DeviceData *)SDL_calloc(1, sizeof(JoyStick_DeviceData));
    if (!pNewJoystick) {
        return; // better luck next time?
    }

    pNewJoystick->bXInputDevice = true;
    pNewJoystick->joystickname = SDL_CreateJoystickName(vendor, product, NULL, name);
    if (!pNewJoystick->joystickname) {
        SDL_free(pNewJoystick);
        return; // better luck next time?
    }
    (void)SDL_snprintf(pNewJoystick->path, sizeof(pNewJoystick->path), "XInput#%u", userid);
    pNewJoystick->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_USB, vendor, product, version, NULL, name, 'x', SubType);
    pNewJoystick->SubType = SubType;
    pNewJoystick->XInputUserId = userid;

    WINDOWS_AddJoystickDevice(pNewJoystick);
}

void SDL_XINPUT_JoystickDetect(JoyStick_DeviceData **pContext)
{
    int iuserid;

    if (!s_bXInputEnabled) {
        return;
    }

    // iterate in reverse, so these are in the final list in ascending numeric order.
    for (iuserid = XUSER_MAX_COUNT - 1; iuserid >= 0; iuserid--) {
        const Uint8 userid = (Uint8)iuserid;
        XINPUT_CAPABILITIES capabilities;
        if (XINPUTGETCAPABILITIES(userid, XINPUT_FLAG_GAMEPAD, &capabilities) == ERROR_SUCCESS) {
            AddXInputDevice(userid, capabilities.SubType, pContext);
        }
    }
}

bool SDL_XINPUT_JoystickPresent(Uint16 vendor, Uint16 product, Uint16 version)
{
    int iuserid;

    if (!s_bXInputEnabled) {
        return false;
    }

    // iterate in reverse, so these are in the final list in ascending numeric order.
    for (iuserid = 0; iuserid < XUSER_MAX_COUNT; ++iuserid) {
        const Uint8 userid = (Uint8)iuserid;
        Uint16 slot_vendor;
        Uint16 slot_product;
        Uint16 slot_version;
        if (GetXInputDeviceInfo(userid, &slot_vendor, &slot_product, &slot_version)) {
            if (vendor == slot_vendor && product == slot_product && version == slot_version) {
                return true;
            }
        }
    }
    return false;
}

bool SDL_XINPUT_JoystickOpen(SDL_Joystick *joystick, JoyStick_DeviceData *joystickdevice)
{
    const Uint8 userId = joystickdevice->XInputUserId;
    XINPUT_CAPABILITIES capabilities;
    XINPUT_VIBRATION state;

    SDL_assert(s_bXInputEnabled);
    SDL_assert(XINPUTGETCAPABILITIES);
    SDL_assert(XINPUTSETSTATE);
    SDL_assert(userId < XUSER_MAX_COUNT);

    joystick->hwdata->bXInputDevice = true;

    if (XINPUTGETCAPABILITIES(userId, XINPUT_FLAG_GAMEPAD, &capabilities) != ERROR_SUCCESS) {
        SDL_free(joystick->hwdata);
        joystick->hwdata = NULL;
        return SDL_SetError("Failed to obtain XInput device capabilities. Device disconnected?");
    }
    SDL_zero(state);
    joystick->hwdata->bXInputHaptic = (XINPUTSETSTATE(userId, &state) == ERROR_SUCCESS);
    joystick->hwdata->userid = userId;

    joystick->hwdata->rb3pro_variant = SDL_RB3Pro_VariantForXInputSubtype(joystickdevice->SubType);
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

bool SDL_XINPUT_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
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

void SDL_XINPUT_JoystickUpdate(SDL_Joystick *joystick)
{
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
    // only fire events if the data changed from last time
    if (XInputState.dwPacketNumber && XInputState.dwPacketNumber != joystick->hwdata->dwPacketNumber) {
        if (joystick->hwdata->rb3pro_variant) {
            UpdateXInputRB3ProState(joystick, &XInputState, extra, &XBatteryInformation);
        } else {
            UpdateXInputJoystickState(joystick, &XInputState, &XBatteryInformation);
        }
        joystick->hwdata->dwPacketNumber = XInputState.dwPacketNumber;
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

void SDL_XINPUT_JoystickClose(SDL_Joystick *joystick)
{
#ifdef SDL_JOYSTICK_XINPUT_PADDLES
    SDL_XINPUT_PaddleClose(joystick);
#endif
}

void SDL_XINPUT_JoystickQuit(void)
{
    if (s_bXInputEnabled) {
#ifdef SDL_JOYSTICK_XINPUT_PADDLES
        SDL_XINPUT_PaddleQuit();
#endif
        s_bXInputEnabled = false;
        WIN_UnloadXInputDLL();
    }
}

// Ends C function definitions when using C++
#ifdef __cplusplus
}
#endif

#else // !SDL_JOYSTICK_XINPUT

typedef struct JoyStick_DeviceData JoyStick_DeviceData;

bool SDL_XINPUT_Enabled(void)
{
    return false;
}

bool SDL_XINPUT_JoystickInit(void)
{
    return true;
}

void SDL_XINPUT_JoystickDetect(JoyStick_DeviceData **pContext)
{
}

bool SDL_XINPUT_JoystickPresent(Uint16 vendor, Uint16 product, Uint16 version)
{
    return false;
}

bool SDL_XINPUT_JoystickOpen(SDL_Joystick *joystick, JoyStick_DeviceData *joystickdevice)
{
    return SDL_Unsupported();
}

bool SDL_XINPUT_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

void SDL_XINPUT_JoystickUpdate(SDL_Joystick *joystick)
{
}

void SDL_XINPUT_JoystickClose(SDL_Joystick *joystick)
{
}

void SDL_XINPUT_JoystickQuit(void)
{
}

#endif // SDL_JOYSTICK_XINPUT
