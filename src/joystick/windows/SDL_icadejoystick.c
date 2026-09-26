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

/*
 * PadForge fork: the ION iCade arcade cabinet, and pads in iCade mode. They
 * pair as Bluetooth HID keyboards and type one letter when a control is
 * pressed and another when it is released. See hifihedgehog/SDL#33 Part 16.
 *
 * The driver runs a thread with a message-only window of its own, whose
 * class belongs to the module that holds this code. The thread lists the
 * keyboards with GetRawInputDeviceList, opens each one's interface path with
 * no access for HidD_GetAttributes, taken from the driver's own reference to
 * hid.dll, and marks those with the cabinet's IDs or a pair from
 * SDL_HINT_JOYSTICK_ICADE_DEVICES. It lists them again after each HID
 * interface notification on its window. The pure decoder in
 * SDL_icade_proto.c turns their records into state. With
 * SDL_HINT_JOYSTICK_ICADE_RAWINPUT on, the window registers for the keyboard
 * class with RIDEV_INPUTSINK and RIDEV_DEVNOTIFY while at least one iCade is
 * marked and no other window of the process holds the class, reads WM_INPUT,
 * and removes the registration when the last one leaves, since only one
 * window per device class receives raw input in a process. With the hint
 * off, SDL registers nothing and the host passes the keyboard records it
 * already receives to SDL_ICadeProcessRawKeyboard. Keyboard traffic takes
 * only this driver's lock, and as in the RFCOMM driver the joystick thread
 * takes presence and state changes from it. The driver thread never logs:
 * the joystick thread holds SDL's joystick lock while it waits for it, and
 * an application's log callback can need that lock. Raw Input consumes no
 * keystroke, so the letters still reach the window with the keyboard focus.
 */

#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_ICADE

#include "../SDL_sysjoystick.h"
#include "../usb_ids.h"
#include "../../core/windows/SDL_windows.h"
#include "../../core/windows/SDL_hid.h"
#include "SDL_icade_proto.h"

#include <dbt.h>

#ifndef RIDEV_DEVNOTIFY
#define RIDEV_DEVNOTIFY 0x00002000
#endif
#ifndef WM_INPUT_DEVICE_CHANGE
#define WM_INPUT_DEVICE_CHANGE 0x00FE
#endif
#ifndef GIDC_ARRIVAL
#define GIDC_ARRIVAL 1
#define GIDC_REMOVAL 2
#endif
#ifndef KEYBOARD_OVERRUN_MAKE_CODE
#define KEYBOARD_OVERRUN_MAKE_CODE 0xFF
#endif
#ifndef DEVICE_NOTIFY_WINDOW_HANDLE
#define DEVICE_NOTIFY_WINDOW_HANDLE 0x00000000
#endif

/* The Win32 calls that reach a device, the process's Raw Input
   registration, hid.dll or the module that holds this code.
   test/testicade.c defines each name before it includes this file, so the
   driver runs against a fake system there, as the BLE GATT driver's test
   does with its transport. */
#ifndef ICADE_RegisterRawInputDevices
#define ICADE_RegisterRawInputDevices RegisterRawInputDevices
#endif
#ifndef ICADE_GetRegisteredRawInputDevices
#define ICADE_GetRegisteredRawInputDevices GetRegisteredRawInputDevices
#endif
#ifndef ICADE_GetRawInputDeviceList
#define ICADE_GetRawInputDeviceList GetRawInputDeviceList
#endif
#ifndef ICADE_GetRawInputDeviceInfoW
#define ICADE_GetRawInputDeviceInfoW GetRawInputDeviceInfoW
#endif
#ifndef ICADE_GetRawInputData
#define ICADE_GetRawInputData GetRawInputData
#endif
#ifndef ICADE_DefWindowProcW
#define ICADE_DefWindowProcW DefWindowProcW
#endif
#ifndef ICADE_CreateFileW
#define ICADE_CreateFileW CreateFileW
#endif
#ifndef ICADE_CloseHandle
#define ICADE_CloseHandle CloseHandle
#endif
#ifndef ICADE_LoadLibraryW
#define ICADE_LoadLibraryW LoadLibraryW
#endif
#ifndef ICADE_GetProcAddress
#define ICADE_GetProcAddress GetProcAddress
#endif
#ifndef ICADE_FreeLibrary
#define ICADE_FreeLibrary FreeLibrary
#endif
#ifndef ICADE_GetModuleHandleExW
#define ICADE_GetModuleHandleExW GetModuleHandleExW
#endif
#ifndef ICADE_RegisterDeviceNotificationW
#define ICADE_RegisterDeviceNotificationW RegisterDeviceNotificationW
#endif
#ifndef ICADE_UnregisterDeviceNotification
#define ICADE_UnregisterDeviceNotification UnregisterDeviceNotification
#endif

/* The pure layer carries the values of SDL and Windows */
SDL_COMPILE_TIME_ASSERT(icade_vendor, SDL_ICADE_VENDOR == USB_VENDOR_ION);
SDL_COMPILE_TIME_ASSERT(icade_product, SDL_ICADE_PRODUCT == USB_PRODUCT_ION_ICADE);
SDL_COMPILE_TIME_ASSERT(icade_hat_centered, SDL_ICADE_HAT_CENTERED == SDL_HAT_CENTERED);
SDL_COMPILE_TIME_ASSERT(icade_hat_up, SDL_ICADE_HAT_UP == SDL_HAT_UP);
SDL_COMPILE_TIME_ASSERT(icade_hat_right, SDL_ICADE_HAT_RIGHT == SDL_HAT_RIGHT);
SDL_COMPILE_TIME_ASSERT(icade_hat_down, SDL_ICADE_HAT_DOWN == SDL_HAT_DOWN);
SDL_COMPILE_TIME_ASSERT(icade_hat_left, SDL_ICADE_HAT_LEFT == SDL_HAT_LEFT);
SDL_COMPILE_TIME_ASSERT(icade_key_break, SDL_ICADE_KEY_BREAK == RI_KEY_BREAK);
SDL_COMPILE_TIME_ASSERT(icade_key_e0, SDL_ICADE_KEY_E0 == RI_KEY_E0);
SDL_COMPILE_TIME_ASSERT(icade_key_e1, SDL_ICADE_KEY_E1 == RI_KEY_E1);
SDL_COMPILE_TIME_ASSERT(icade_overrun, SDL_ICADE_OVERRUN == KEYBOARD_OVERRUN_MAKE_CODE);

#define ICADE_MAX_DEVICES    8   /* iCade devices at once */
#define ICADE_PATH_LENGTH    512 /* An interface path in UTF-8 */
#define ICADE_NAME_LENGTH    48
#define ICADE_UPDATE_BATCH   32  /* Events Update takes under one lock */
#define ICADE_LIST_ATTEMPTS  4   /* A device can arrive between the two list calls */
#define ICADE_UPDATE_PASSES  4   /* Registration passes when calls nest */
#define ICADE_MAX_NAME_CHARS 32768
#define ICADE_NOTE_ENTRIES   8   /* Driver thread messages kept for the joystick thread */
#define ICADE_NOTE_LENGTH    160
#define ICADE_RESCAN_TIMER_1 1
#define ICADE_RESCAN_TIMER_2 2
#define ICADE_WM_APPLY       (WM_APP + 1) /* The joystick thread has new hint values */
#define ICADE_CLASS_NAME     L"SDL_ICade"

/* GUID_DEVINTERFACE_HID, as SDL_windowsjoystick.c spells it */
static const GUID icade_hid_interface = { 0x4D1E55B2L, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

/* A keyboard from the last list, the driver thread's */
typedef struct ICADE_Keyboard
{
    HANDLE handle;
    bool identified; /* HidD_GetAttributes answered */
    Uint16 vendor;
    Uint16 product;
    Uint16 version;
    char path[ICADE_PATH_LENGTH];
} ICADE_Keyboard;

typedef struct ICADE_Device
{
    /* Under the lock */
    HANDLE handle;
    Uint32 generation; /* 0 while no device holds the slot */
    SDL_ICadeLayout layout;
    Uint16 vendor;
    Uint16 product;
    Uint16 version;
    char path[ICADE_PATH_LENGTH];
    Uint16 state;
    SDL_ICadeQueue queue;

    /* The joystick thread's */
    Uint32 registered; /* The generation it has a joystick for, 0 when none */
    SDL_JoystickID instance_id;
    SDL_GUID guid;
    SDL_ICadeLayout joystick_layout;
    char joystick_name[ICADE_NAME_LENGTH];
    char joystick_path[ICADE_PATH_LENGTH];
} ICADE_Device;

struct joystick_hwdata
{
    int slot;
    Uint32 generation;
    Uint64 sequence;   /* The last event sent */
    bool send_initial; /* The state Open took is not sent yet */
    Uint64 initial_stamp;
    Uint16 initial;
};

/* Who holds the process's keyboard Raw Input registration */
typedef enum ICADE_Owner
{
    ICADE_OWNER_NONE,   /* No window */
    ICADE_OWNER_WINDOW, /* This driver's window */
    ICADE_OWNER_OTHER,  /* Another window, or the keyboard focus */
    ICADE_OWNER_UNKNOWN /* The registrations could not be read */
} ICADE_Owner;

/* The SDL_Mutex, atomic. Created once and never destroyed, since the host
   can call SDL_ICadeProcessRawKeyboard at any time. */
static void *icade_lock;
/* Created with the lock and never destroyed. The thread signals it once its
   first list is done. */
static SDL_Semaphore *icade_ready;
static bool icade_ready_ok; /* Written by the thread before it signals */

/* The driver thread's messages, logged by the joystick thread. Under their
   own lock, created with the driver lock and never destroyed. */
static SDL_Mutex *icade_notes_lock;
static char icade_notes[ICADE_NOTE_ENTRIES][ICADE_NOTE_LENGTH];
static int icade_note_count;
static Uint32 icade_notes_dropped;

/* Under the lock */
static ICADE_Device icade_devices[ICADE_MAX_DEVICES];
static bool icade_decoding;             /* While the thread runs */
static bool icade_want_rawinput;        /* SDL_HINT_JOYSTICK_ICADE_RAWINPUT */
static SDL_ICadeDeviceList icade_pairs; /* SDL_HINT_JOYSTICK_ICADE_DEVICES */
static Uint32 icade_next_generation;

/* The joystick thread's. The driver thread reads icade_get_attributes, set
   before it starts and cleared after it is joined. */
static bool icade_initialized;
static HMODULE icade_hid; /* This driver's own reference to hid.dll */
static HidD_GetAttributes_t icade_get_attributes;
static SDL_Thread *icade_thread;
static HWND icade_window; /* Set by the thread before it reports ready */
static SDL_AtomicInt icade_hints_changed;

/* Set by the thread, taken by the joystick thread */
static SDL_AtomicInt icade_presence_changed;

/* The driver thread's */
static ICADE_Keyboard *icade_keyboards;
static int icade_nkeyboards;
static bool icade_rawinput;     /* The window holds the keyboard registration it made */
static bool icade_updating;     /* ICADE_UpdateRegistration is running */
static bool icade_update_again; /* It was called again meanwhile */
static HDEVNOTIFY icade_notify;
static bool icade_logged_full;

static SDL_Mutex *ICADE_Lock(void)
{
    return (SDL_Mutex *)SDL_GetAtomicPointer(&icade_lock);
}

/* A message from the driver thread. SDL_LockJoysticks locks SDL_event_lock,
   which an application's log callback also takes when it pushes an event,
   and the joystick thread holds it while it waits for this thread at start
   and at quit. So this thread keeps its messages here and the joystick
   thread logs them. */
static void ICADE_Note(const char *format, ...)
{
    va_list ap;

    SDL_LockMutex(icade_notes_lock);
    if (icade_note_count < ICADE_NOTE_ENTRIES) {
        va_start(ap, format);
        (void)SDL_vsnprintf(icade_notes[icade_note_count], ICADE_NOTE_LENGTH, format, ap);
        va_end(ap);
        ++icade_note_count;
    } else {
        ++icade_notes_dropped;
    }
    SDL_UnlockMutex(icade_notes_lock);
}

/* On the joystick thread: logs the driver thread's messages with none of
   the driver's locks held */
static void ICADE_FlushNotes(void)
{
    char notes[ICADE_NOTE_ENTRIES][ICADE_NOTE_LENGTH];
    Uint32 dropped;
    int count, i;

    SDL_LockMutex(icade_notes_lock);
    count = icade_note_count;
    for (i = 0; i < count; ++i) {
        SDL_memcpy(notes[i], icade_notes[i], ICADE_NOTE_LENGTH);
    }
    dropped = icade_notes_dropped;
    icade_note_count = 0;
    icade_notes_dropped = 0;
    SDL_UnlockMutex(icade_notes_lock);

    for (i = 0; i < count; ++i) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "%s", notes[i]);
    }
    if (dropped) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "iCade: %u more messages dropped", (unsigned int)dropped);
    }
}

/* Under the lock. Every control is released, as when the device leaves. */
static void ICADE_FreeDevice(ICADE_Device *device)
{
    device->handle = NULL;
    device->generation = 0;
    device->layout = SDL_ICADE_LAYOUT_NONE;
    device->path[0] = '\0';
    device->state = 0;
    SDL_ICade_ClearQueue(&device->queue);
}

/* One keyboard record from WM_INPUT or the host. Takes only this driver's
   lock. True when the handle is a device the driver decodes. */
static bool ICADE_Key(HANDLE handle, Uint16 make_code, Uint16 flags)
{
    SDL_Mutex *lock = ICADE_Lock();
    bool found = false;
    int i;

    if (!lock || !handle) {
        return false;
    }
    SDL_LockMutex(lock);
    if (icade_decoding) {
        for (i = 0; i < ICADE_MAX_DEVICES; ++i) {
            ICADE_Device *device = &icade_devices[i];

            if (device->generation && device->handle == handle) {
                found = true;
                if (SDL_ICade_ApplyKey(&device->state, make_code, flags)) {
                    SDL_ICade_PushQueue(&device->queue, device->state, SDL_GetTicksNS());
                }
                break;
            }
        }
    }
    SDL_UnlockMutex(lock);
    return found;
}

/* The keyboard's interface path, or NULL. The caller frees it. For
   RIDI_DEVICENAME the size is a count of characters, and a call without a
   buffer returns 0 once it has set the size. */
static WCHAR *ICADE_ReadName(HANDLE handle)
{
    UINT chars = 0, size;
    WCHAR *name;

    if (ICADE_GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, NULL, &chars) != 0 || chars == 0 || chars > ICADE_MAX_NAME_CHARS) {
        return NULL;
    }
    size = chars + 1;
    name = (WCHAR *)SDL_calloc(size, sizeof(WCHAR));
    if (!name) {
        return NULL;
    }
    if (ICADE_GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, name, &chars) == (UINT)-1) {
        SDL_free(name);
        return NULL;
    }
    name[size - 1] = 0;
    return name;
}

/* The IDs through a handle opened with no access, since Windows opens
   keyboard collections for its exclusive use. SDL opens keyboards this way
   for their names (SDL_windowsevents.c:910-918), as hidapi does for system
   devices (hid.c:1407-1412). */
static bool ICADE_ReadIDs(const WCHAR *name, Uint16 *vendor, Uint16 *product, Uint16 *version)
{
    HIDD_ATTRIBUTES attributes;
    HANDLE file;
    bool ok;

    file = ICADE_CreateFileW(name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    SDL_zero(attributes);
    attributes.Size = sizeof(attributes);
    ok = icade_get_attributes(file, &attributes) ? true : false;
    ICADE_CloseHandle(file);
    if (ok) {
        *vendor = attributes.VendorID;
        *product = attributes.ProductID;
        *version = attributes.VersionNumber;
    }
    return ok;
}

/* The driver thread's list against the current pairs. A device whose
   keyboard left or whose layout changed goes with every control released,
   and a keyboard newly decoded as an iCade takes a free slot with nothing
   held. */
static void ICADE_Classify(void)
{
    SDL_Mutex *lock = ICADE_Lock();
    bool changed = false;
    int i, j;

    SDL_LockMutex(lock);
    for (i = 0; i < ICADE_MAX_DEVICES; ++i) {
        ICADE_Device *device = &icade_devices[i];
        SDL_ICadeLayout layout = SDL_ICADE_LAYOUT_NONE;

        if (!device->generation) {
            continue;
        }
        for (j = 0; j < icade_nkeyboards; ++j) {
            const ICADE_Keyboard *keyboard = &icade_keyboards[j];

            if (keyboard->handle == device->handle && keyboard->identified &&
                SDL_strcmp(keyboard->path, device->path) == 0) {
                layout = SDL_ICade_Classify(keyboard->vendor, keyboard->product, &icade_pairs);
                break;
            }
        }
        if (layout != device->layout) {
            ICADE_FreeDevice(device);
            changed = true;
        }
    }
    for (j = 0; j < icade_nkeyboards; ++j) {
        const ICADE_Keyboard *keyboard = &icade_keyboards[j];
        SDL_ICadeLayout layout;
        ICADE_Device *slot = NULL;

        if (!keyboard->identified) {
            continue;
        }
        layout = SDL_ICade_Classify(keyboard->vendor, keyboard->product, &icade_pairs);
        if (layout == SDL_ICADE_LAYOUT_NONE) {
            continue;
        }
        for (i = 0; i < ICADE_MAX_DEVICES; ++i) {
            if (icade_devices[i].generation && icade_devices[i].handle == keyboard->handle) {
                break;
            }
            if (!slot && !icade_devices[i].generation) {
                slot = &icade_devices[i];
            }
        }
        if (i < ICADE_MAX_DEVICES) {
            continue; /* Already decoded */
        }
        if (!slot) {
            if (!icade_logged_full) {
                icade_logged_full = true;
                ICADE_Note("iCade: more than %d devices, the rest are left alone", ICADE_MAX_DEVICES);
            }
            continue;
        }
        if (++icade_next_generation == 0) {
            icade_next_generation = 1;
        }
        ICADE_FreeDevice(slot);
        slot->handle = keyboard->handle;
        slot->generation = icade_next_generation;
        slot->layout = layout;
        slot->vendor = keyboard->vendor;
        slot->product = keyboard->product;
        slot->version = keyboard->version;
        SDL_strlcpy(slot->path, keyboard->path, sizeof(slot->path));
        changed = true;
    }
    SDL_UnlockMutex(lock);
    if (changed) {
        SDL_SetAtomicInt(&icade_presence_changed, 1);
    }
}

/* Which window of the process holds the keyboard class, from
   GetRegisteredRawInputDevices. A registration with no window follows the
   keyboard focus, so it counts as another window's. */
static ICADE_Owner ICADE_KeyboardOwner(HWND window)
{
    RAWINPUTDEVICE local[16];
    RAWINPUTDEVICE *registered = local;
    UINT count = SDL_arraysize(local), got, i;
    ICADE_Owner owner = ICADE_OWNER_NONE;

    got = ICADE_GetRegisteredRawInputDevices(registered, &count, sizeof(RAWINPUTDEVICE));
    if (got == (UINT)-1 && GetLastError() == ERROR_INSUFFICIENT_BUFFER && count > 0) {
        registered = (RAWINPUTDEVICE *)SDL_malloc(count * sizeof(*registered));
        if (!registered) {
            return ICADE_OWNER_UNKNOWN;
        }
        got = ICADE_GetRegisteredRawInputDevices(registered, &count, sizeof(RAWINPUTDEVICE));
    }
    if (got == (UINT)-1) {
        owner = ICADE_OWNER_UNKNOWN;
    } else {
        for (i = 0; i < got; ++i) {
            if (registered[i].usUsagePage == USB_USAGEPAGE_GENERIC_DESKTOP && registered[i].usUsage == USB_USAGE_GENERIC_KEYBOARD) {
                owner = (registered[i].hwndTarget == window) ? ICADE_OWNER_WINDOW : ICADE_OWNER_OTHER;
                break;
            }
        }
    }
    if (registered != local) {
        SDL_free(registered);
    }
    return owner;
}

/* Takes the keyboard class only while no window of the process holds it.
   The RegisterRawInputDevices remarks warn that a library's registration can
   interfere with the raw input processing of the application that loads it.
   Refused, or unable to read the registrations, it leaves icade_rawinput
   false so the next update checks again. */
static void ICADE_Register(HWND window)
{
    RAWINPUTDEVICE device;

    switch (ICADE_KeyboardOwner(window)) {
    case ICADE_OWNER_WINDOW:
        icade_rawinput = true; /* Kept by a removal that could not read the registrations */
        return;
    case ICADE_OWNER_OTHER:
        ICADE_Note("iCade: another window holds keyboard Raw Input, so SDL leaves it alone");
        return;
    case ICADE_OWNER_UNKNOWN:
        ICADE_Note("iCade: the Raw Input registrations could not be read, so SDL registers nothing");
        return;
    default:
        break;
    }
    device.usUsagePage = USB_USAGEPAGE_GENERIC_DESKTOP;
    device.usUsage = USB_USAGE_GENERIC_KEYBOARD;
    device.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    device.hwndTarget = window;
    if (ICADE_RegisterRawInputDevices(&device, 1, sizeof(device))) {
        icade_rawinput = true;
        ICADE_Note("iCade: reading keyboard Raw Input");
    } else {
        ICADE_Note("iCade: RegisterRawInputDevices failed with error %lu", GetLastError());
    }
}

/* Removes the keyboard registration only while it still names this window.
   A window registered later in the process has replaced it, and
   RIDEV_REMOVE would take that window's registration away. A registration
   that cannot be read is left alone as well. */
static void ICADE_Unregister(HWND window)
{
    RAWINPUTDEVICE device;

    if (!icade_rawinput) {
        return;
    }
    icade_rawinput = false;
    switch (ICADE_KeyboardOwner(window)) {
    case ICADE_OWNER_WINDOW:
        device.usUsagePage = USB_USAGEPAGE_GENERIC_DESKTOP;
        device.usUsage = USB_USAGE_GENERIC_KEYBOARD;
        device.dwFlags = RIDEV_REMOVE;
        device.hwndTarget = NULL;
        if (!ICADE_RegisterRawInputDevices(&device, 1, sizeof(device))) {
            ICADE_Note("iCade: removing the Raw Input registration failed with error %lu", GetLastError());
        }
        break;
    case ICADE_OWNER_OTHER:
        ICADE_Note("iCade: another window holds the keyboard Raw Input registration");
        break;
    case ICADE_OWNER_UNKNOWN:
        ICADE_Note("iCade: the Raw Input registrations could not be read, so SDL removes nothing");
        break;
    default:
        break;
    }
}

/* Holds the keyboard registration while the hint allows it and at least one
   iCade is marked, and removes it otherwise, so a process with no iCade
   keeps its own keyboard Raw Input. Registering can report the keyboards
   already there to the window, which can list them again and change the
   devices before the registration returns. A call made meanwhile only asks
   for one more pass once this one is done. */
static void ICADE_UpdateRegistration(HWND window)
{
    SDL_Mutex *lock = ICADE_Lock();
    int pass, i;

    if (icade_updating) {
        icade_update_again = true;
        return;
    }
    icade_updating = true;
    for (pass = 0; pass < ICADE_UPDATE_PASSES; ++pass) {
        bool want, present = false;

        icade_update_again = false;
        SDL_LockMutex(lock);
        want = icade_want_rawinput;
        for (i = 0; i < ICADE_MAX_DEVICES; ++i) {
            if (icade_devices[i].generation) {
                present = true;
                break;
            }
        }
        SDL_UnlockMutex(lock);
        if (want && present && !icade_rawinput) {
            ICADE_Register(window);
        } else if ((!want || !present) && icade_rawinput) {
            ICADE_Unregister(window);
        }
        if (!icade_update_again) {
            break;
        }
    }
    icade_updating = false;
}

/* Lists the keyboards again. A keyboard identified before under the same
   handle and path keeps its IDs. Every other keyboard is opened, one that
   did not answer before included, since a device can be listed before every
   API reports it (SDL_windowsjoystick.c:105-107). A failed list changes
   nothing, since it says nothing about which keyboards left. */
static void ICADE_Rescan(HWND window)
{
    RAWINPUTDEVICELIST *list = NULL;
    ICADE_Keyboard *keyboards = NULL;
    UINT count = 0, i;
    DWORD error;
    int attempt, nkeyboards = 0, k;

    for (attempt = 0; attempt < ICADE_LIST_ATTEMPTS; ++attempt) {
        UINT listed;

        if (ICADE_GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST)) != 0) {
            return;
        }
        if (count == 0) {
            break;
        }
        list = (RAWINPUTDEVICELIST *)SDL_malloc(count * sizeof(*list));
        if (!list) {
            return;
        }
        listed = ICADE_GetRawInputDeviceList(list, &count, sizeof(RAWINPUTDEVICELIST));
        if (listed != (UINT)-1) {
            count = listed;
            break;
        }
        error = GetLastError();
        SDL_free(list);
        list = NULL;
        /* A device arrived between the two calls, as Microsoft's example
           for GetRawInputDeviceList handles it */
        if (error != ERROR_INSUFFICIENT_BUFFER) {
            return;
        }
    }
    if (attempt == ICADE_LIST_ATTEMPTS) {
        return;
    }

    if (count > 0) {
        keyboards = (ICADE_Keyboard *)SDL_calloc(count, sizeof(*keyboards));
        if (!keyboards) {
            SDL_free(list);
            return;
        }
    }
    for (i = 0; i < count; ++i) {
        ICADE_Keyboard *keyboard;
        WCHAR *name;
        char *path;

        if (list[i].dwType != RIM_TYPEKEYBOARD || !list[i].hDevice) {
            continue;
        }
        name = ICADE_ReadName(list[i].hDevice);
        if (!name) {
            continue;
        }
        path = WIN_StringToUTF8W(name);
        if (!path) {
            SDL_free(name);
            continue;
        }
        keyboard = &keyboards[nkeyboards++];
        keyboard->handle = list[i].hDevice;
        SDL_strlcpy(keyboard->path, path, sizeof(keyboard->path));
        SDL_free(path);
        for (k = 0; k < icade_nkeyboards; ++k) {
            if (icade_keyboards[k].identified && icade_keyboards[k].handle == keyboard->handle &&
                SDL_strcmp(icade_keyboards[k].path, keyboard->path) == 0) {
                break;
            }
        }
        if (k < icade_nkeyboards) {
            keyboard->identified = true;
            keyboard->vendor = icade_keyboards[k].vendor;
            keyboard->product = icade_keyboards[k].product;
            keyboard->version = icade_keyboards[k].version;
        } else {
            keyboard->identified = ICADE_ReadIDs(name, &keyboard->vendor, &keyboard->product, &keyboard->version);
        }
        SDL_free(name);
    }
    SDL_free(list);

    SDL_free(icade_keyboards);
    icade_keyboards = keyboards;
    icade_nkeyboards = nkeyboards;
    ICADE_Classify();
    ICADE_UpdateRegistration(window);
}

static bool ICADE_IsListed(HANDLE handle)
{
    int k;

    for (k = 0; k < icade_nkeyboards; ++k) {
        if (icade_keyboards[k].handle == handle) {
            return true;
        }
    }
    return false;
}

/* GIDC_REMOVAL: the keyboard goes from the list and its device with it */
static void ICADE_Removed(HWND window, HANDLE handle)
{
    int k;

    for (k = 0; k < icade_nkeyboards; ++k) {
        if (icade_keyboards[k].handle == handle) {
            SDL_memmove(&icade_keyboards[k], &icade_keyboards[k + 1], (size_t)(icade_nkeyboards - k - 1) * sizeof(*icade_keyboards));
            --icade_nkeyboards;
            break;
        }
    }
    ICADE_Classify();
    ICADE_UpdateRegistration(window);
}

/* The hints the joystick thread passed, applied on the driver thread */
static void ICADE_ApplyConfig(HWND window)
{
    ICADE_Classify();
    ICADE_UpdateRegistration(window);
}

static void ICADE_ReadInput(HRAWINPUT input)
{
    RAWINPUT raw;
    UINT size = sizeof(raw);
    UINT copied;

    copied = ICADE_GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
    if (copied == (UINT)-1 || copied < sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD) || raw.header.dwType != RIM_TYPEKEYBOARD) {
        return;
    }
    (void)ICADE_Key(raw.header.hDevice, raw.data.keyboard.MakeCode, raw.data.keyboard.Flags);
}

static LRESULT CALLBACK ICADE_WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INPUT:
        ICADE_ReadInput((HRAWINPUT)lParam);
        /* Microsoft asks for DefWindowProc after foreground input, for its
           cleanup */
        if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT) {
            return ICADE_DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return 0;
    case WM_INPUT_DEVICE_CHANGE:
        if (wParam == GIDC_ARRIVAL) {
            /* Registration reports every keyboard already there */
            if (!ICADE_IsListed((HANDLE)lParam)) {
                ICADE_Rescan(hwnd);
            }
        } else if (wParam == GIDC_REMOVAL) {
            ICADE_Removed(hwnd, (HANDLE)lParam);
        }
        return 0;
    case WM_DEVICECHANGE:
        if ((wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) && lParam &&
            ((DEV_BROADCAST_HDR *)lParam)->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
            /* 300 ms and 2 s later, as the Windows joystick driver waits for
               every API to see the change (SDL_windowsjoystick.c:104-108) */
            SetTimer(hwnd, ICADE_RESCAN_TIMER_1, 300, NULL);
            SetTimer(hwnd, ICADE_RESCAN_TIMER_2, 2000, NULL);
        }
        return TRUE;
    case WM_TIMER:
        if (wParam == ICADE_RESCAN_TIMER_1 || wParam == ICADE_RESCAN_TIMER_2) {
            KillTimer(hwnd, wParam);
            ICADE_Rescan(hwnd);
            return 0;
        }
        break;
    case ICADE_WM_APPLY:
        ICADE_ApplyConfig(hwnd);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* The HID interface notifications on the window, as the Windows joystick
   driver registers them (SDL_windowsjoystick.c:176-186). They bring the
   rescans in both modes, and the only ones while the window holds no
   keyboard registration: with the Raw Input hint off, or with no iCade. */
static void ICADE_WatchDevices(HWND window)
{
    DEV_BROADCAST_DEVICEINTERFACE_W filter;

    SDL_zero(filter);
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = icade_hid_interface;
    icade_notify = ICADE_RegisterDeviceNotificationW(window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!icade_notify) {
        ICADE_Note("iCade: RegisterDeviceNotification failed with error %lu", GetLastError());
    }
}

static int SDLCALL ICADE_Thread(void *data)
{
    HMODULE instance = NULL;
    WNDCLASSEXW wincl;
    HWND window;
    MSG msg;

    (void)data;
    /* The class belongs to the module that holds this code, found the way
       SDL's keyboard hook finds its own (SDL_windowswindow.c:1451), so two
       copies of SDL in one process never share a window procedure */
    if (!ICADE_GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCWSTR)(uintptr_t)ICADE_WindowProc, &instance)) {
        ICADE_Note("iCade: GetModuleHandleEx failed with error %lu", GetLastError());
        icade_ready_ok = false;
        SDL_SignalSemaphore(icade_ready);
        return 0;
    }
    SDL_zero(wincl);
    wincl.cbSize = sizeof(wincl);
    wincl.hInstance = instance;
    wincl.lpszClassName = ICADE_CLASS_NAME;
    wincl.lpfnWndProc = ICADE_WindowProc;
    /* A class this module left from an earlier start is used again */
    if (!RegisterClassExW(&wincl) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        ICADE_Note("iCade: RegisterClassEx failed with error %lu", GetLastError());
        icade_ready_ok = false;
        SDL_SignalSemaphore(icade_ready);
        return 0;
    }
    window = CreateWindowExW(0, ICADE_CLASS_NAME, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, instance, NULL);
    if (!window) {
        ICADE_Note("iCade: CreateWindowEx failed with error %lu", GetLastError());
        UnregisterClassW(ICADE_CLASS_NAME, instance);
        icade_ready_ok = false;
        SDL_SignalSemaphore(icade_ready);
        return 0;
    }
    icade_window = window;
    icade_logged_full = false;
    ICADE_WatchDevices(window);
    /* The first list registers for keyboard Raw Input if it finds an iCade */
    ICADE_Rescan(window);
    icade_ready_ok = true;
    SDL_SignalSemaphore(icade_ready);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        DispatchMessageW(&msg);
    }

    ICADE_Unregister(window);
    if (icade_notify) {
        ICADE_UnregisterDeviceNotification(icade_notify);
        icade_notify = NULL;
    }
    DestroyWindow(window);
    UnregisterClassW(ICADE_CLASS_NAME, instance);
    SDL_free(icade_keyboards);
    icade_keyboards = NULL;
    icade_nkeyboards = 0;
    return 0;
}

/* The joystick thread's side */

static int ICADE_GetSlot(int device_index)
{
    int i;

    if (device_index < 0) {
        return -1;
    }
    for (i = 0; i < ICADE_MAX_DEVICES; ++i) {
        if (icade_devices[i].registered && device_index-- == 0) {
            return i;
        }
    }
    return -1;
}

/* Takes the decoded devices as joysticks, or removes those that left */
static void ICADE_CheckPresence(bool notify)
{
    SDL_Mutex *lock = ICADE_Lock();
    int i;

    for (i = 0; i < ICADE_MAX_DEVICES; ++i) {
        ICADE_Device *device = &icade_devices[i];
        SDL_ICadeLayout layout = SDL_ICADE_LAYOUT_NONE;
        Uint16 vendor = 0, product = 0, version = 0;
        Uint32 generation;
        SDL_JoystickType type;

        SDL_LockMutex(lock);
        generation = device->generation;
        if (generation && generation != device->registered) {
            layout = device->layout;
            vendor = device->vendor;
            product = device->product;
            version = device->version;
            SDL_strlcpy(device->joystick_path, device->path, sizeof(device->joystick_path));
        }
        SDL_UnlockMutex(lock);

        if (device->registered && device->registered != generation) {
            /* SDL releases the controls of an open joystick it removes */
            if (notify) {
                SDL_PrivateJoystickRemoved(device->instance_id);
            }
            device->registered = 0;
        }
        if (!device->registered && generation) {
            device->registered = generation;
            device->joystick_layout = layout;
            if (layout == SDL_ICADE_LAYOUT_CABINET) {
                SDL_strlcpy(device->joystick_name, "ION iCade", sizeof(device->joystick_name));
                type = SDL_JOYSTICK_TYPE_ARCADE_STICK;
            } else {
                (void)SDL_snprintf(device->joystick_name, sizeof(device->joystick_name), "iCade Controller (0x%.4x/0x%.4x)", vendor, product);
                type = SDL_JOYSTICK_TYPE_GAMEPAD;
            }
            device->instance_id = SDL_GetNextObjectID();
            /* 'i' is this driver's signature, and the type rides in the last
               byte. The iCade letters come over a Bluetooth keyboard link. */
            device->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, vendor, product, version, NULL, device->joystick_name, 'i', (Uint8)type);
            SDL_PrivateJoystickAdded(device->instance_id);
        }
    }
}

static void ICADE_LogEntry(void *userdata, const char *entry, size_t length, const char *reason)
{
    (void)userdata;
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "SDL_JOYSTICK_ICADE_DEVICES entry \"%.*s\" skipped: %s", (int)length, entry, reason);
}

/* This driver's own reference to hid.dll, loaded and released on the
   joystick thread. WIN_LoadHIDDLL counts its references without a lock
   (SDL_hid.c), and another thread of SDL loads it through that count. */
static void ICADE_ReleaseHID(void)
{
    if (icade_hid) {
        ICADE_FreeLibrary(icade_hid);
        icade_hid = NULL;
    }
    icade_get_attributes = NULL;
}

static void ICADE_StartThread(void)
{
    SDL_Mutex *lock = ICADE_Lock();

    icade_hid = ICADE_LoadLibraryW(L"hid.dll");
    if (!icade_hid) {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "iCade: hid.dll could not be loaded");
        return;
    }
    icade_get_attributes = (HidD_GetAttributes_t)ICADE_GetProcAddress(icade_hid, "HidD_GetAttributes");
    if (!icade_get_attributes) {
        ICADE_ReleaseHID();
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "iCade: hid.dll has no HidD_GetAttributes");
        return;
    }
    icade_ready_ok = false;
    SDL_LockMutex(lock);
    icade_decoding = true;
    SDL_UnlockMutex(lock);
    icade_thread = SDL_CreateThread(ICADE_Thread, "SDLICade", NULL);
    if (icade_thread) {
        /* The first list is done before SDL_Init returns */
        SDL_WaitSemaphore(icade_ready);
    }
    if (!icade_thread || !icade_ready_ok) {
        if (icade_thread) {
            SDL_WaitThread(icade_thread, NULL);
            icade_thread = NULL;
        }
        SDL_LockMutex(lock);
        icade_decoding = false;
        SDL_UnlockMutex(lock);
        ICADE_ReleaseHID();
        ICADE_FlushNotes();
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "iCade: the driver thread could not start");
        return;
    }
    ICADE_FlushNotes();
}

/* Ends the thread, releases every device and removes their joysticks */
static void ICADE_StopThread(bool notify)
{
    SDL_Mutex *lock = ICADE_Lock();
    int i;

    if (icade_thread) {
        PostThreadMessageW((DWORD)SDL_GetThreadID(icade_thread), WM_QUIT, 0, 0);
        SDL_WaitThread(icade_thread, NULL);
        icade_thread = NULL;
    }
    ICADE_FlushNotes();
    icade_window = NULL;
    SDL_LockMutex(lock);
    icade_decoding = false;
    for (i = 0; i < ICADE_MAX_DEVICES; ++i) {
        ICADE_FreeDevice(&icade_devices[i]);
    }
    SDL_UnlockMutex(lock);
    ICADE_ReleaseHID();
    ICADE_CheckPresence(notify);
    SDL_SetAtomicInt(&icade_presence_changed, 0);
}

static void ICADE_ApplyHints(void)
{
    SDL_Mutex *lock = ICADE_Lock();
    const bool enabled = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_ICADE, true);
    const bool rawinput = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_ICADE_RAWINPUT, true);
    const char *text = SDL_GetHint(SDL_HINT_JOYSTICK_ICADE_DEVICES);
    SDL_ICadeDeviceList pairs;

    (void)SDL_ICade_ParseDevices(text, text ? SDL_strlen(text) : 0, &pairs, ICADE_LogEntry, NULL);
    SDL_LockMutex(lock);
    icade_want_rawinput = rawinput;
    icade_pairs = pairs;
    SDL_UnlockMutex(lock);

    if (!enabled) {
        if (icade_thread) {
            ICADE_StopThread(true);
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "iCade: stopped");
        }
    } else if (!icade_thread) {
        ICADE_StartThread();
    } else {
        PostMessageW(icade_window, ICADE_WM_APPLY, 0, 0);
    }
}

static void SDLCALL ICADE_HintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    (void)userdata;
    (void)name;
    (void)oldValue;
    (void)hint;
    SDL_SetAtomicInt(&icade_hints_changed, 1);
}

static bool ICADE_JoystickInit(void)
{
    SDL_Mutex *lock = ICADE_Lock();

    if (icade_initialized) {
        return true;
    }
    if (!lock) {
        SDL_Semaphore *ready = SDL_CreateSemaphore(0);
        SDL_Mutex *notes = SDL_CreateMutex();

        lock = SDL_CreateMutex();
        if (!lock || !ready || !notes) {
            if (lock) {
                SDL_DestroyMutex(lock);
            }
            if (ready) {
                SDL_DestroySemaphore(ready);
            }
            if (notes) {
                SDL_DestroyMutex(notes);
            }
            return false;
        }
        icade_ready = ready;
        icade_notes_lock = notes;
        SDL_SetAtomicPointer(&icade_lock, lock);
    }
    icade_initialized = true;
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_ICADE, ICADE_HintChanged, NULL);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_ICADE_RAWINPUT, ICADE_HintChanged, NULL);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_ICADE_DEVICES, ICADE_HintChanged, NULL);
    /* The callbacks ran at once. The hints are read here, so a device
       present now is a joystick when SDL_Init returns. */
    SDL_SetAtomicInt(&icade_hints_changed, 0);
    ICADE_ApplyHints();
    SDL_SetAtomicInt(&icade_presence_changed, 0);
    ICADE_CheckPresence(true);
    return true;
}

static int ICADE_JoystickGetCount(void)
{
    int count = 0, i;

    for (i = 0; i < ICADE_MAX_DEVICES; ++i) {
        if (icade_devices[i].registered) {
            ++count;
        }
    }
    return count;
}

static void ICADE_JoystickDetect(void)
{
    if (!icade_initialized) {
        return;
    }
    if (SDL_GetAtomicInt(&icade_hints_changed)) {
        SDL_SetAtomicInt(&icade_hints_changed, 0);
        ICADE_ApplyHints();
    }
    if (SDL_GetAtomicInt(&icade_presence_changed)) {
        SDL_SetAtomicInt(&icade_presence_changed, 0);
        ICADE_CheckPresence(true);
    }
    ICADE_FlushNotes();
}

/* A keyboard is no other driver's device */
static bool ICADE_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    (void)vendor_id;
    (void)product_id;
    (void)version;
    (void)name;
    return false;
}

static const char *ICADE_JoystickGetDeviceName(int device_index)
{
    const int slot = ICADE_GetSlot(device_index);

    return (slot >= 0) ? icade_devices[slot].joystick_name : NULL;
}

static const char *ICADE_JoystickGetDevicePath(int device_index)
{
    const int slot = ICADE_GetSlot(device_index);

    return (slot >= 0) ? icade_devices[slot].joystick_path : NULL;
}

static int ICADE_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    (void)device_index;
    return -1;
}

static int ICADE_JoystickGetDevicePlayerIndex(int device_index)
{
    (void)device_index;
    return -1;
}

static void ICADE_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
    (void)device_index;
    (void)player_index;
}

static SDL_GUID ICADE_JoystickGetDeviceGUID(int device_index)
{
    const int slot = ICADE_GetSlot(device_index);
    SDL_GUID guid;

    if (slot < 0) {
        SDL_zero(guid);
        return guid;
    }
    return icade_devices[slot].guid;
}

static SDL_JoystickID ICADE_JoystickGetDeviceInstanceID(int device_index)
{
    const int slot = ICADE_GetSlot(device_index);

    return (slot >= 0) ? icade_devices[slot].instance_id : 0;
}

static void ICADE_SendState(SDL_Joystick *joystick, Uint16 state, Uint64 timestamp)
{
    int i;

    SDL_SendJoystickHat(timestamp, joystick, 0, SDL_ICade_GetHat(state));
    for (i = 0; i < SDL_ICADE_BUTTONS; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, SDL_ICade_GetButton(state, i));
    }
}

static bool ICADE_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    SDL_Mutex *lock = ICADE_Lock();
    const int slot = ICADE_GetSlot(device_index);
    struct joystick_hwdata *hwdata;
    ICADE_Device *device;

    if (slot < 0) {
        return SDL_SetError("iCade device index out of range");
    }
    hwdata = (struct joystick_hwdata *)SDL_calloc(1, sizeof(*hwdata));
    if (!hwdata) {
        return false;
    }
    device = &icade_devices[slot];
    hwdata->slot = slot;
    hwdata->generation = device->registered;
    joystick->hwdata = hwdata;
    joystick->naxes = 0;
    joystick->nhats = 1;
    joystick->nbuttons = SDL_ICADE_BUTTONS;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRELESS;

    /* The state so far, sent by the first update, as SDL allocates the
       joystick's hat and buttons only after Open returns. Every event queued
       up to now is skipped. */
    SDL_LockMutex(lock);
    if (device->generation == hwdata->generation) {
        hwdata->initial = device->state;
        hwdata->sequence = device->queue.sequence;
    }
    SDL_UnlockMutex(lock);
    hwdata->initial_stamp = SDL_GetTicksNS();
    hwdata->send_initial = true;
    return true;
}

static bool ICADE_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    (void)joystick;
    (void)low_frequency_rumble;
    (void)high_frequency_rumble;
    return SDL_Unsupported();
}

static bool ICADE_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    (void)joystick;
    (void)left_rumble;
    (void)right_rumble;
    return SDL_Unsupported();
}

static bool ICADE_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)joystick;
    (void)red;
    (void)green;
    (void)blue;
    return SDL_Unsupported();
}

static bool ICADE_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    (void)joystick;
    (void)data;
    (void)size;
    return SDL_Unsupported();
}

static bool ICADE_JoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    (void)joystick;
    (void)enabled;
    return SDL_Unsupported();
}

/* Drains the device's events in order */
static void ICADE_JoystickUpdate(SDL_Joystick *joystick)
{
    SDL_Mutex *lock = ICADE_Lock();
    struct joystick_hwdata *hwdata = joystick->hwdata;
    SDL_ICadeEvent events[ICADE_UPDATE_BATCH];
    ICADE_Device *device;
    int rounds, count, i;

    if (!hwdata) {
        return;
    }
    device = &icade_devices[hwdata->slot];
    if (hwdata->send_initial) {
        hwdata->send_initial = false;
        ICADE_SendState(joystick, hwdata->initial, hwdata->initial_stamp);
    }
    /* A batch per lock, at most one queue's worth per update */
    for (rounds = 0; rounds < SDL_ICADE_QUEUE_CAPACITY / ICADE_UPDATE_BATCH; ++rounds) {
        count = 0;
        SDL_LockMutex(lock);
        if (device->generation == hwdata->generation) {
            while (count < ICADE_UPDATE_BATCH && SDL_ICade_PopQueue(&device->queue, &events[count])) {
                ++count;
            }
        }
        SDL_UnlockMutex(lock);
        for (i = 0; i < count; ++i) {
            if (events[i].sequence <= hwdata->sequence) {
                continue;
            }
            hwdata->sequence = events[i].sequence;
            ICADE_SendState(joystick, events[i].state, events[i].time_ns);
        }
        if (count < ICADE_UPDATE_BATCH) {
            break;
        }
    }
}

static void ICADE_JoystickClose(SDL_Joystick *joystick)
{
    SDL_free(joystick->hwdata);
    joystick->hwdata = NULL;
}

static void ICADE_JoystickQuit(void)
{
    if (!icade_initialized) {
        return;
    }
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_ICADE, ICADE_HintChanged, NULL);
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_ICADE_RAWINPUT, ICADE_HintChanged, NULL);
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_ICADE_DEVICES, ICADE_HintChanged, NULL);
    ICADE_StopThread(false);
    SDL_SetAtomicInt(&icade_hints_changed, 0);
    icade_initialized = false;
}

static void ICADE_MapButton(SDL_InputMapping *mapping, SDL_ICadeLayout layout, SDL_ICadeSlot slot)
{
    const int button = SDL_ICade_GetSlotButton(layout, slot);

    if (button >= 0) {
        mapping->kind = EMappingKind_Button;
        mapping->target = (Uint8)button;
    }
}

/* Hat 0, whose number rides in the high nibble */
static void ICADE_MapHat(SDL_InputMapping *mapping, Uint8 direction)
{
    mapping->kind = EMappingKind_Hat;
    mapping->target = direction;
}

/* The layout's buttons, and the stick's hat on the D-pad */
static bool ICADE_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    const int slot = ICADE_GetSlot(device_index);
    SDL_ICadeLayout layout;

    if (slot < 0) {
        return false;
    }
    layout = icade_devices[slot].joystick_layout;
    SDL_zerop(out);
    ICADE_MapButton(&out->a, layout, SDL_ICADE_SLOT_SOUTH);
    ICADE_MapButton(&out->b, layout, SDL_ICADE_SLOT_EAST);
    ICADE_MapButton(&out->x, layout, SDL_ICADE_SLOT_WEST);
    ICADE_MapButton(&out->y, layout, SDL_ICADE_SLOT_NORTH);
    ICADE_MapButton(&out->back, layout, SDL_ICADE_SLOT_BACK);
    ICADE_MapButton(&out->start, layout, SDL_ICADE_SLOT_START);
    ICADE_MapButton(&out->leftshoulder, layout, SDL_ICADE_SLOT_LEFT_SHOULDER);
    ICADE_MapButton(&out->rightshoulder, layout, SDL_ICADE_SLOT_RIGHT_SHOULDER);
    ICADE_MapButton(&out->lefttrigger, layout, SDL_ICADE_SLOT_LEFT_TRIGGER);
    ICADE_MapButton(&out->righttrigger, layout, SDL_ICADE_SLOT_RIGHT_TRIGGER);
    ICADE_MapHat(&out->dpup, SDL_HAT_UP);
    ICADE_MapHat(&out->dpdown, SDL_HAT_DOWN);
    ICADE_MapHat(&out->dpleft, SDL_HAT_LEFT);
    ICADE_MapHat(&out->dpright, SDL_HAT_RIGHT);
    return true;
}

/* The host's keyboard records, for a host that registers keyboards itself
   and so keeps SDL_HINT_JOYSTICK_ICADE_RAWINPUT off */
bool SDL_ICadeProcessRawKeyboard(void *device_handle, Uint16 make_code, Uint16 flags)
{
    return ICADE_Key((HANDLE)device_handle, make_code, flags);
}

SDL_JoystickDriver SDL_ICADE_JoystickDriver = {
    ICADE_JoystickInit,
    ICADE_JoystickGetCount,
    ICADE_JoystickDetect,
    ICADE_JoystickIsDevicePresent,
    ICADE_JoystickGetDeviceName,
    ICADE_JoystickGetDevicePath,
    ICADE_JoystickGetDeviceSteamVirtualGamepadSlot,
    ICADE_JoystickGetDevicePlayerIndex,
    ICADE_JoystickSetDevicePlayerIndex,
    ICADE_JoystickGetDeviceGUID,
    ICADE_JoystickGetDeviceInstanceID,
    ICADE_JoystickOpen,
    ICADE_JoystickRumble,
    ICADE_JoystickRumbleTriggers,
    ICADE_JoystickSetLED,
    ICADE_JoystickSendEffect,
    ICADE_JoystickSetSensorsEnabled,
    ICADE_JoystickUpdate,
    ICADE_JoystickClose,
    ICADE_JoystickQuit,
    ICADE_JoystickGetGamepadMapping
};

#endif /* SDL_JOYSTICK_ICADE */
