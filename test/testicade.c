/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_ICADE_JoystickDriver, the iCade driver of hifihedgehog/SDL#33
   Part 16, inside a static SDL against a fake Win32 system. No keyboard is
   opened, hid.dll is not loaded, and nothing registers for real Raw Input.
   The fake answers every call the driver makes to list, name, open and
   identify keyboards, to load hid.dll, to find its own module, to register
   for Raw Input and for device notifications, and to read WM_INPUT records.
   The test sends WM_INPUT and device change messages to the driver's real
   message-only window, and calls the host feed through testicadehost.c,
   which sees only SDL's public headers. Each scenario starts SDL, reaches
   the device through SDL's joystick and gamepad API, and quits SDL. The
   scenarios follow the part's replay tests 1 to 9, then the rules for the
   keyboard registration, the window class, hid.dll and logging.

   The driver in this file calls the fake, not Windows. Each name through
   which the driver reaches a device, hid.dll, its module or the Raw Input
   registration is defined below before the driver source is included. This
   object defines SDL_ICADE_JoystickDriver and the host feed, so SDL's driver
   table binds to this copy and SDL's own copy of the driver is never linked,
   as in the BLE GATT driver's test. Before the link,
   test/icade-driver/CheckSystem.cmake reads this object's symbols and fails
   the build when it still imports a Win32 function that reaches a device,
   loads a library or registers, or uses SDL's shared hid.dll state.

   Arguments: optionally the letters of the scenarios to run. */

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"
#include "core/windows/SDL_windows.h"
#include "core/windows/SDL_hid.h"
#include <dbt.h>

/* The fake system, defined below */
static BOOL WINAPI Fake_RegisterRawInputDevices(PCRAWINPUTDEVICE devices, UINT count, UINT size);
static UINT WINAPI Fake_GetRegisteredRawInputDevices(PRAWINPUTDEVICE devices, PUINT count, UINT size);
static UINT WINAPI Fake_GetRawInputDeviceList(PRAWINPUTDEVICELIST list, PUINT count, UINT size);
static UINT WINAPI Fake_GetRawInputDeviceInfoW(HANDLE device, UINT command, LPVOID data, PUINT size);
static UINT WINAPI Fake_GetRawInputData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size);
static LRESULT WINAPI Fake_DefWindowProcW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static HANDLE WINAPI Fake_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                      DWORD disposition, DWORD flags, HANDLE template_file);
static BOOL WINAPI Fake_CloseHandle(HANDLE handle);
static HMODULE WINAPI Fake_LoadLibraryW(LPCWSTR name);
static FARPROC WINAPI Fake_GetProcAddress(HMODULE module, LPCSTR name);
static BOOL WINAPI Fake_FreeLibrary(HMODULE module);
static BOOL WINAPI Fake_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *module);
static HDEVNOTIFY WINAPI Fake_RegisterDeviceNotificationW(HANDLE recipient, LPVOID filter, DWORD flags);
static BOOL WINAPI Fake_UnregisterDeviceNotification(HDEVNOTIFY notify);

#define ICADE_RegisterRawInputDevices      Fake_RegisterRawInputDevices
#define ICADE_GetRegisteredRawInputDevices Fake_GetRegisteredRawInputDevices
#define ICADE_GetRawInputDeviceList        Fake_GetRawInputDeviceList
#define ICADE_GetRawInputDeviceInfoW       Fake_GetRawInputDeviceInfoW
#define ICADE_GetRawInputData              Fake_GetRawInputData
#define ICADE_DefWindowProcW               Fake_DefWindowProcW
#define ICADE_CreateFileW                  Fake_CreateFileW
#define ICADE_CloseHandle                  Fake_CloseHandle
#define ICADE_LoadLibraryW                 Fake_LoadLibraryW
#define ICADE_GetProcAddress               Fake_GetProcAddress
#define ICADE_FreeLibrary                  Fake_FreeLibrary
#define ICADE_GetModuleHandleExW           Fake_GetModuleHandleExW
#define ICADE_RegisterDeviceNotificationW  Fake_RegisterDeviceNotificationW
#define ICADE_UnregisterDeviceNotification Fake_UnregisterDeviceNotification

#include "../src/joystick/windows/SDL_icadejoystick.c"

#include <process.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* testicadehost.c */
extern bool TestICade_HostFeed(void *device_handle, Uint16 make_code, Uint16 flags);

#define DT_WAIT_MS           3000  /* Longest wait for anything the driver does */
#define DT_QUIT_LIMIT_MS     5000  /* SDL_Quit in any scenario */
#define DT_SCENARIO_LIMIT_MS 60000 /* The watchdog ends a scenario that hangs */

#define DT_HINT_ICADE    "SDL_JOYSTICK_ICADE"
#define DT_HINT_RAWINPUT "SDL_JOYSTICK_ICADE_RAWINPUT"
#define DT_HINT_DEVICES  "SDL_JOYSTICK_ICADE_DEVICES"

#define DT_COUNT(array) ((int)SDL_arraysize(array))

/* Every other joystick driver off, so only the driver under test adds a
   joystick, whatever the environment holds */
static const char *const dt_other_drivers[][2] = {
    { "SDL_JOYSTICK_HIDAPI", "0" },
    { "SDL_JOYSTICK_RAWINPUT", "0" },
    { "SDL_JOYSTICK_DIRECTINPUT", "0" },
    { "SDL_XINPUT_ENABLED", "0" },
    { "SDL_JOYSTICK_WGI", "0" },
    { "SDL_JOYSTICK_GAMEINPUT", "0" },
    { "SDL_JOYSTICK_GAMEINPUT_RAW", "0" },
    { "SDL_JOYSTICK_BLE", "0" },
    { "SDL_JOYSTICK_BLE_SWITCH2", "0" },
    { "SDL_JOYSTICK_SERIAL", "" },
    { "SDL_JOYSTICK_SERIAL_AUTO", "0" },
    { "SDL_JOYSTICK_DJI_REMOTE_TCP_HOSTS", "" },
    { "SDL_JOYSTICK_RFCOMM", "0" },
    { "SDL_JOYSTICK_WINMM", "0" },
    { "SDL_JOYSTICK_THREAD", "0" },
    { "SDL_JOYSTICK_ROG_CHAKRAM", "0" },
    { SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1" }
};

/* The part's table, built here from it and not from the module: state bit,
   press make code, release make code, and the press letter's US virtual key */
static const struct
{
    Uint16 bit;
    Uint16 press;
    Uint16 release;
    Uint16 vkey;
} dt_controls[12] = {
    { 0x001, 0x11, 0x12, 'W' }, /* Stick up */
    { 0x002, 0x20, 0x2E, 'D' }, /* Stick right */
    { 0x004, 0x2D, 0x2C, 'X' }, /* Stick down */
    { 0x008, 0x1E, 0x10, 'A' }, /* Stick left */
    { 0x010, 0x15, 0x14, 'Y' }, /* Button A */
    { 0x020, 0x23, 0x13, 'H' }, /* Button B */
    { 0x040, 0x16, 0x21, 'U' }, /* Button C */
    { 0x080, 0x24, 0x31, 'J' }, /* Button D */
    { 0x100, 0x17, 0x32, 'I' }, /* Button E */
    { 0x200, 0x25, 0x19, 'K' }, /* Button F */
    { 0x400, 0x18, 0x22, 'O' }, /* Button G */
    { 0x800, 0x26, 0x2F, 'L' }, /* Button H */
};

/* Checks */

static int dt_checks;
static int dt_failures;
static const char *volatile dt_scenario = "-";

static bool DT_Check(bool condition, int line, const char *format, ...)
{
    va_list args;

    ++dt_checks;
    if (condition) {
        return true;
    }
    ++dt_failures;
    printf("FAIL (%s) line %d: ", dt_scenario, line);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    return false;
}

#define DT_CHECK(condition, ...) DT_Check((condition) ? true : false, __LINE__, __VA_ARGS__)

/* The fake system */

#define FAKE_MAX_DEVICES    24
#define FAKE_MAX_FILES      256
#define FAKE_MAX_RECORDS    8192
#define FAKE_MAX_REGISTERED 8
#define FAKE_NAME_CHARS     400
#define FAKE_RECORD_BASE    0x40000
#define FAKE_FILE_BASE      0x70000
#define FAKE_NOTIFY         ((HDEVNOTIFY)(uintptr_t)0xD00D)
#define FAKE_HID_MODULE     ((HMODULE)(uintptr_t)0x4D1D0000)

typedef struct FakeDevice
{
    HANDLE handle;
    DWORD type; /* RIM_TYPE* */
    WCHAR name[FAKE_NAME_CHARS];
    bool present;
    bool openable;
    bool identifiable;
    USHORT vendor;
    USHORT product;
    USHORT version;
    int name_calls;
    int attempts; /* Opens asked for, whether or not they worked */
    int opens;
} FakeDevice;

typedef struct FakeFile
{
    HANDLE handle;
    int device;
    bool open;
} FakeFile;

typedef struct FakeRecord
{
    UINT size;
    bool truncated; /* Only reported bytes count, and the rest are stale */
    UINT reported;
    RAWINPUT raw;
} FakeRecord;

typedef struct FakeSystem
{
    SDL_Mutex *mutex;
    FakeDevice devices[FAKE_MAX_DEVICES];
    int ndevices;
    int list_calls;
    int list_short;   /* A NULL list call reports one device fewer, as when one arrives between the calls */
    int list_fail;    /* List calls that fail with ERROR_ACCESS_DENIED */
    int bad_list_calls;
    int name_calls;
    int bad_name_calls;
    FakeFile files[FAKE_MAX_FILES];
    int nfiles;
    int opens;
    int closes;
    int bad_opens;     /* Access asked for, another share mode or disposition, or not a keyboard */
    int bad_closes;
    int attribute_calls;
    int bad_attribute_calls;
    RAWINPUTDEVICE registered[FAKE_MAX_REGISTERED];
    int nregistered;
    int register_calls; /* Every RegisterRawInputDevices call */
    int add_calls;      /* The entries that register a class, not remove it */
    int remove_calls;
    /* A keyboard registration with RIDEV_DEVNOTIFY sends GIDC_ARRIVAL for
       every keyboard present to the window before it returns, the case that
       nests the most. Before that, the device at leave_on_register - 1
       leaves and the one at arrive_on_register - 1 arrives, once. */
    bool register_reports;
    int leave_on_register;
    int arrive_on_register;
    int bad_register_calls;
    RAWINPUTDEVICE last_register;
    DWORD register_thread;
    int registered_calls;
    bool registered_fail; /* GetRegisteredRawInputDevices fails */
    int hid_loads;        /* LoadLibraryW of hid.dll */
    int hid_frees;
    int hid_lookups;
    int bad_hid_calls;    /* Another library, another function, a module not loaded */
    DWORD hid_thread;
    bool hid_missing;     /* LoadLibraryW fails */
    bool hid_no_function; /* hid.dll has no HidD_GetAttributes */
    int module_lookups;
    DWORD module_flags;
    const void *module_address;
    HWND notify_window;
    int notify_calls;
    int unnotify_calls;
    int bad_notify_calls;
    DWORD notify_thread;
    FakeRecord records[FAKE_MAX_RECORDS];
    int nrecords;
    int data_calls;
    int bad_data_calls;
    int foreground_cleanups; /* WM_INPUT with RIM_INPUT through DefWindowProc */
    int background_cleanups; /* WM_INPUT with RIM_INPUTSINK through DefWindowProc */
    bool quit_returned;
    int calls_after_quit;
} FakeSystem;

static FakeSystem fake;

/* The driver's thread, from its first call to the fake */
static volatile LONG dt_driver_thread;

/* GUID_DEVINTERFACE_HID */
static const GUID fake_hid_guid = { 0x4D1E55B2L, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

static void Fake_Setup(void)
{
    fake.mutex = SDL_CreateMutex();
}

static void Fake_Reset(void)
{
    SDL_Mutex *mutex = fake.mutex;

    SDL_LockMutex(mutex);
    SDL_zero(fake);
    fake.mutex = mutex;
    SDL_UnlockMutex(mutex);
}

/* The caller holds the mutex */
static void Fake_Called(void)
{
    if (fake.quit_returned) {
        ++fake.calls_after_quit;
    }
}

static int Fake_DeviceByHandle(HANDLE handle)
{
    int i;

    for (i = 0; i < fake.ndevices; ++i) {
        if (fake.devices[i].present && fake.devices[i].handle == handle) {
            return i;
        }
    }
    return -1;
}

static int Fake_FileByHandle(HANDLE handle)
{
    int i;

    for (i = 0; i < fake.nfiles; ++i) {
        if (fake.files[i].handle == handle) {
            return i;
        }
    }
    return -1;
}

static BOOL WINAPI Fake_RegisterRawInputDevices(PCRAWINPUTDEVICE devices, UINT count, UINT size)
{
    HANDLE reports[FAKE_MAX_DEVICES];
    HWND report_window = NULL;
    int nreports = 0;
    BOOL result = TRUE;
    UINT i;
    int j;

    SDL_LockMutex(fake.mutex);
    Fake_Called();
    ++fake.register_calls;
    fake.register_thread = GetCurrentThreadId();
    if (!devices || count == 0 || size != sizeof(RAWINPUTDEVICE)) {
        ++fake.bad_register_calls;
        result = FALSE;
    }
    for (i = 0; result && i < count; ++i) {
        const RAWINPUTDEVICE *device = &devices[i];

        fake.last_register = *device;
        for (j = 0; j < fake.nregistered; ++j) {
            if (fake.registered[j].usUsagePage == device->usUsagePage && fake.registered[j].usUsage == device->usUsage) {
                break;
            }
        }
        if (device->dwFlags & RIDEV_REMOVE) {
            ++fake.remove_calls;
            /* RIDEV_REMOVE with a window fails (RAWINPUTDEVICE remarks) */
            if (device->hwndTarget) {
                ++fake.bad_register_calls;
                result = FALSE;
                break;
            }
            if (j < fake.nregistered) {
                SDL_memmove(&fake.registered[j], &fake.registered[j + 1], (size_t)(fake.nregistered - j - 1) * sizeof(fake.registered[0]));
                --fake.nregistered;
            }
        } else {
            /* RIDEV_INPUTSINK needs a window (RAWINPUTDEVICE dwFlags) */
            if ((device->dwFlags & RIDEV_INPUTSINK) && !device->hwndTarget) {
                ++fake.bad_register_calls;
                result = FALSE;
                break;
            }
            if (j == fake.nregistered) {
                if (fake.nregistered == FAKE_MAX_REGISTERED) {
                    result = FALSE;
                    break;
                }
                ++fake.nregistered;
            }
            /* The window of the last call receives the class */
            fake.registered[j] = *device;
            ++fake.add_calls;
            if (fake.register_reports && device->usUsagePage == 1 && device->usUsage == 6 &&
                (device->dwFlags & RIDEV_DEVNOTIFY) && device->hwndTarget) {
                if (fake.leave_on_register > 0) {
                    fake.devices[fake.leave_on_register - 1].present = false;
                    fake.leave_on_register = 0;
                }
                if (fake.arrive_on_register > 0) {
                    fake.devices[fake.arrive_on_register - 1].present = true;
                    fake.arrive_on_register = 0;
                }
                report_window = device->hwndTarget;
                for (j = 0; j < fake.ndevices; ++j) {
                    if (fake.devices[j].present && fake.devices[j].type == RIM_TYPEKEYBOARD) {
                        reports[nreports++] = fake.devices[j].handle;
                    }
                }
            }
        }
    }
    SDL_UnlockMutex(fake.mutex);
    if (!result) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return result;
    }
    /* Sent from the window's own thread, so each one runs the window
       procedure before this call returns */
    for (j = 0; j < nreports; ++j) {
        (void)SendMessageW(report_window, WM_INPUT_DEVICE_CHANGE, GIDC_ARRIVAL, (LPARAM)reports[j]);
    }
    return result;
}

static UINT WINAPI Fake_GetRegisteredRawInputDevices(PRAWINPUTDEVICE devices, PUINT count, UINT size)
{
    UINT result;

    SDL_LockMutex(fake.mutex);
    Fake_Called();
    ++fake.registered_calls;
    if (!count || size != sizeof(RAWINPUTDEVICE)) {
        SDL_UnlockMutex(fake.mutex);
        SetLastError(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }
    if (fake.registered_fail) {
        SDL_UnlockMutex(fake.mutex);
        SetLastError(ERROR_ACCESS_DENIED);
        return (UINT)-1;
    }
    /* A buffer that is NULL or too small (GetRegisteredRawInputDevices) */
    if (!devices || *count < (UINT)fake.nregistered) {
        *count = (UINT)fake.nregistered;
        SDL_UnlockMutex(fake.mutex);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return (UINT)-1;
    }
    SDL_memcpy(devices, fake.registered, (size_t)fake.nregistered * sizeof(fake.registered[0]));
    result = (UINT)fake.nregistered;
    SDL_UnlockMutex(fake.mutex);
    return result;
}

static UINT WINAPI Fake_GetRawInputDeviceList(PRAWINPUTDEVICELIST list, PUINT count, UINT size)
{
    UINT present = 0, reported, n = 0;
    int i;

    SDL_LockMutex(fake.mutex);
    Fake_Called();
    ++fake.list_calls;
    if (!count || size != sizeof(RAWINPUTDEVICELIST)) {
        ++fake.bad_list_calls;
        SDL_UnlockMutex(fake.mutex);
        SetLastError(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }
    if (fake.list_fail > 0) {
        --fake.list_fail;
        SDL_UnlockMutex(fake.mutex);
        SetLastError(ERROR_ACCESS_DENIED);
        return (UINT)-1;
    }
    for (i = 0; i < fake.ndevices; ++i) {
        if (fake.devices[i].present) {
            ++present;
        }
    }
    if (!list) {
        reported = present;
        if (fake.list_short > 0 && present > 0) {
            --fake.list_short;
            --reported;
        }
        *count = reported;
        SDL_UnlockMutex(fake.mutex);
        return 0;
    }
    /* Too small: the count back and ERROR_INSUFFICIENT_BUFFER
       (GetRawInputDeviceList puiNumDevices) */
    if (*count < present) {
        *count = present;
        SDL_UnlockMutex(fake.mutex);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return (UINT)-1;
    }
    for (i = 0; i < fake.ndevices; ++i) {
        if (fake.devices[i].present) {
            list[n].hDevice = fake.devices[i].handle;
            list[n].dwType = fake.devices[i].type;
            ++n;
        }
    }
    SDL_UnlockMutex(fake.mutex);
    return n;
}

/* For RIDI_DEVICENAME the size is in characters, and a NULL buffer returns
   0 with the size needed (GetRawInputDeviceInfoW) */
static UINT WINAPI Fake_GetRawInputDeviceInfoW(HANDLE device, UINT command, LPVOID data, PUINT size)
{
    UINT chars;
    int d;

    SDL_LockMutex(fake.mutex);
    Fake_Called();
    ++fake.name_calls;
    d = Fake_DeviceByHandle(device);
    if (command != RIDI_DEVICENAME || !size) {
        ++fake.bad_name_calls;
        SDL_UnlockMutex(fake.mutex);
        SetLastError(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }
    if (d < 0) {
        SDL_UnlockMutex(fake.mutex);
        SetLastError(ERROR_INVALID_HANDLE);
        return (UINT)-1;
    }
    ++fake.devices[d].name_calls;
    chars = (UINT)SDL_wcslen(fake.devices[d].name) + 1;
    if (!data) {
        *size = chars;
        SDL_UnlockMutex(fake.mutex);
        return 0;
    }
    if (*size < chars) {
        *size = chars;
        SDL_UnlockMutex(fake.mutex);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return (UINT)-1;
    }
    SDL_memcpy(data, fake.devices[d].name, chars * sizeof(WCHAR));
    SDL_UnlockMutex(fake.mutex);
    return chars;
}

static UINT WINAPI Fake_GetRawInputData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size)
{
    const intptr_t index = (intptr_t)(uintptr_t)input - FAKE_RECORD_BASE;
    FakeRecord *record;
    UINT result;

    SDL_LockMutex(fake.mutex);
    Fake_Called();
    ++fake.data_calls;
    if (command != RID_INPUT || !size || header_size != sizeof(RAWINPUTHEADER)) {
        ++fake.bad_data_calls;
        SDL_UnlockMutex(fake.mutex);
        SetLastError(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }
    if (index < 0 || index >= fake.nrecords) {
        ++fake.bad_data_calls;
        SDL_UnlockMutex(fake.mutex);
        SetLastError(ERROR_INVALID_HANDLE);
        return (UINT)-1;
    }
    record = &fake.records[index];
    if (!data) {
        *size = record->size;
        SDL_UnlockMutex(fake.mutex);
        return 0;
    }
    if (*size < record->size) {
        *size = record->size;
        SDL_UnlockMutex(fake.mutex);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return (UINT)-1;
    }
    /* A truncated record still fills the whole buffer, so the bytes past the
       count look like a real record to a reader that ignores the count */
    SDL_memcpy(data, &record->raw, record->size);
    result = record->truncated ? record->reported : record->size;
    SDL_UnlockMutex(fake.mutex);
    return result;
}

static LRESULT WINAPI Fake_DefWindowProcW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_INPUT) {
        SDL_LockMutex(fake.mutex);
        if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT) {
            ++fake.foreground_cleanups;
        } else {
            ++fake.background_cleanups;
        }
        SDL_UnlockMutex(fake.mutex);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HANDLE WINAPI Fake_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                      DWORD disposition, DWORD flags, HANDLE template_file)
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    int i, d = -1;

    (void)flags;
    SDL_LockMutex(fake.mutex);
    Fake_Called();
    /* The keyboard collections are Windows's to read, so only a zero-access
       open works on them (hid.c:1407-1412) */
    if (access != 0 || share != (FILE_SHARE_READ | FILE_SHARE_WRITE) || disposition != OPEN_EXISTING || security ||
        template_file) {
        ++fake.bad_opens;
    }
    for (i = 0; name && i < fake.ndevices; ++i) {
        if (fake.devices[i].present && SDL_wcscmp(fake.devices[i].name, name) == 0) {
            d = i;
            break;
        }
    }
    if (d >= 0 && fake.devices[d].type != RIM_TYPEKEYBOARD) {
        ++fake.bad_opens;
    }
    if (d >= 0) {
        ++fake.devices[d].attempts;
    }
    if (d >= 0 && fake.devices[d].openable && fake.nfiles < FAKE_MAX_FILES) {
        FakeFile *file = &fake.files[fake.nfiles];

        file->handle = (HANDLE)(uintptr_t)(FAKE_FILE_BASE + fake.nfiles);
        file->device = d;
        file->open = true;
        ++fake.nfiles;
        ++fake.opens;
        ++fake.devices[d].opens;
        handle = file->handle;
    }
    SDL_UnlockMutex(fake.mutex);
    if (handle == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_FILE_NOT_FOUND);
    }
    return handle;
}

static BOOL WINAPI Fake_CloseHandle(HANDLE handle)
{
    BOOL result = FALSE;
    int f;

    SDL_LockMutex(fake.mutex);
    Fake_Called();
    f = Fake_FileByHandle(handle);
    if (f >= 0 && fake.files[f].open) {
        fake.files[f].open = false;
        ++fake.closes;
        result = TRUE;
    } else {
        ++fake.bad_closes;
    }
    SDL_UnlockMutex(fake.mutex);
    return result;
}

static BOOLEAN WINAPI Fake_HidD_GetAttributes(HANDLE handle, PHIDD_ATTRIBUTES attributes)
{
    BOOLEAN result = FALSE;
    int f;

    SDL_LockMutex(fake.mutex);
    Fake_Called();
    ++fake.attribute_calls;
    f = Fake_FileByHandle(handle);
    /* Reached only through the pointer the driver took from its own hid.dll,
       while it holds that library */
    if (f < 0 || !fake.files[f].open || !attributes || attributes->Size != sizeof(HIDD_ATTRIBUTES) ||
        fake.hid_loads <= fake.hid_frees) {
        ++fake.bad_attribute_calls;
    } else {
        const FakeDevice *device = &fake.devices[fake.files[f].device];

        /* A failed call leaves the IDs in the structure too, which the
           driver must not take */
        attributes->VendorID = device->vendor;
        attributes->ProductID = device->product;
        attributes->VersionNumber = device->version;
        result = device->identifiable ? TRUE : FALSE;
    }
    SDL_UnlockMutex(fake.mutex);
    return result;
}

/* hid.dll, which the driver loads for itself. Its one function is the fake
   HidD_GetAttributes above. */
static HMODULE WINAPI Fake_LoadLibraryW(LPCWSTR name)
{
    HMODULE result = NULL;

    SDL_LockMutex(fake.mutex);
    Fake_Called();
    if (!name || SDL_wcscmp(name, L"hid.dll") != 0) {
        ++fake.bad_hid_calls;
    } else if (!fake.hid_missing) {
        ++fake.hid_loads;
        fake.hid_thread = GetCurrentThreadId();
        result = FAKE_HID_MODULE;
    }
    SDL_UnlockMutex(fake.mutex);
    if (!result) {
        SetLastError(ERROR_MOD_NOT_FOUND);
    }
    return result;
}

static FARPROC WINAPI Fake_GetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC result = NULL;

    SDL_LockMutex(fake.mutex);
    Fake_Called();
    ++fake.hid_lookups;
    if (module != FAKE_HID_MODULE || fake.hid_loads <= fake.hid_frees || !name || SDL_strcmp(name, "HidD_GetAttributes") != 0) {
        ++fake.bad_hid_calls;
    } else if (!fake.hid_no_function) {
        result = (FARPROC)Fake_HidD_GetAttributes;
    }
    SDL_UnlockMutex(fake.mutex);
    if (!result) {
        SetLastError(ERROR_PROC_NOT_FOUND);
    }
    return result;
}

static BOOL WINAPI Fake_FreeLibrary(HMODULE module)
{
    BOOL result = FALSE;

    SDL_LockMutex(fake.mutex);
    Fake_Called();
    if (module != FAKE_HID_MODULE || fake.hid_loads <= fake.hid_frees) {
        ++fake.bad_hid_calls;
    } else {
        ++fake.hid_frees;
        result = TRUE;
    }
    SDL_UnlockMutex(fake.mutex);
    return result;
}

/* The driver thread's first call. It answers with kernel32.dll, a module
   other than this program's, so the test sees which module the window
   class is registered under. */
static BOOL WINAPI Fake_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *module)
{
    (void)InterlockedExchange(&dt_driver_thread, (LONG)GetCurrentThreadId());
    SDL_LockMutex(fake.mutex);
    Fake_Called();
    ++fake.module_lookups;
    fake.module_flags = flags;
    fake.module_address = (const void *)name;
    SDL_UnlockMutex(fake.mutex);
    if (!module) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *module = GetModuleHandleW(L"kernel32.dll");
    return *module ? TRUE : FALSE;
}

static HDEVNOTIFY WINAPI Fake_RegisterDeviceNotificationW(HANDLE recipient, LPVOID filter, DWORD flags)
{
    const DEV_BROADCAST_DEVICEINTERFACE_W *interface_filter = (const DEV_BROADCAST_DEVICEINTERFACE_W *)filter;

    SDL_LockMutex(fake.mutex);
    Fake_Called();
    ++fake.notify_calls;
    fake.notify_thread = GetCurrentThreadId();
    if (!recipient || !interface_filter || interface_filter->dbcc_size != sizeof(*interface_filter) ||
        interface_filter->dbcc_devicetype != DBT_DEVTYP_DEVICEINTERFACE ||
        SDL_memcmp(&interface_filter->dbcc_classguid, &fake_hid_guid, sizeof(GUID)) != 0 || flags != DEVICE_NOTIFY_WINDOW_HANDLE) {
        ++fake.bad_notify_calls;
    }
    fake.notify_window = (HWND)recipient;
    SDL_UnlockMutex(fake.mutex);
    return FAKE_NOTIFY;
}

static BOOL WINAPI Fake_UnregisterDeviceNotification(HDEVNOTIFY notify)
{
    SDL_LockMutex(fake.mutex);
    Fake_Called();
    ++fake.unnotify_calls;
    if (notify != FAKE_NOTIFY) {
        ++fake.bad_notify_calls;
    }
    SDL_UnlockMutex(fake.mutex);
    return TRUE;
}

/* What the test sets and reads in the fake */

#define DT_CABINET_NAME "\\\\?\\HID#{00001124-0000-1000-8000-00805f9b34fb}_VID&000215e4_PID&0132&Col01#8&2f1a0b3c&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}"
#define DT_KEYBOARD_NAME "\\\\?\\HID#VID_046D&PID_C31C&MI_00#7&1a2b3c4d&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}"
#define DT_PAD_NAME "\\\\?\\HID#{00001124-0000-1000-8000-00805f9b34fb}_VID&00022dc8_PID&9021&Col01#8&11223344&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}"
#define DT_MOUSE_NAME "\\\\?\\HID#VID_046D&PID_C077#7&0a0b0c0d&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}"
#define DT_GAMEPAD_NAME "\\\\?\\HID#VID_045E&PID_028E&IG_00#7&0e0f1011&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"

#define DT_CABINET  ((HANDLE)(uintptr_t)0x1001)
#define DT_KEYBOARD ((HANDLE)(uintptr_t)0x1002)
#define DT_PAD      ((HANDLE)(uintptr_t)0x1003)
#define DT_MOUSE    ((HANDLE)(uintptr_t)0x2001)
#define DT_GAMEPAD  ((HANDLE)(uintptr_t)0x3001)

#define DT_CABINET_VERSION 0x0101

static int DT_AddDevice(HANDLE handle, DWORD type, const char *name, USHORT vendor, USHORT product, USHORT version)
{
    FakeDevice *device;
    int index, i;

    SDL_LockMutex(fake.mutex);
    index = fake.ndevices++;
    device = &fake.devices[index];
    SDL_zerop(device);
    device->handle = handle;
    device->type = type;
    for (i = 0; name[i] && i < FAKE_NAME_CHARS - 1; ++i) {
        device->name[i] = (WCHAR)(unsigned char)name[i];
    }
    device->name[i] = 0;
    device->present = true;
    device->openable = true;
    device->identifiable = true;
    device->vendor = vendor;
    device->product = product;
    device->version = version;
    SDL_UnlockMutex(fake.mutex);
    return index;
}

/* The usual system: the cabinet, a desk keyboard, a mouse and a gamepad */
static int DT_AddUsualDevices(void)
{
    const int cabinet = DT_AddDevice(DT_CABINET, RIM_TYPEKEYBOARD, DT_CABINET_NAME, 0x15E4, 0x0132, DT_CABINET_VERSION);

    (void)DT_AddDevice(DT_KEYBOARD, RIM_TYPEKEYBOARD, DT_KEYBOARD_NAME, 0x046D, 0xC31C, 0x6400);
    (void)DT_AddDevice(DT_MOUSE, RIM_TYPEMOUSE, DT_MOUSE_NAME, 0x046D, 0xC077, 0x7200);
    (void)DT_AddDevice(DT_GAMEPAD, RIM_TYPEHID, DT_GAMEPAD_NAME, 0x045E, 0x028E, 0x0114);
    return cabinet;
}

static void DT_SetPresent(int device, bool present)
{
    SDL_LockMutex(fake.mutex);
    fake.devices[device].present = present;
    SDL_UnlockMutex(fake.mutex);
}

static int DT_Fake(const int *counter)
{
    int value;

    SDL_LockMutex(fake.mutex);
    value = *counter;
    SDL_UnlockMutex(fake.mutex);
    return value;
}

static HWND DT_Window(void)
{
    HWND window;

    SDL_LockMutex(fake.mutex);
    window = fake.notify_window;
    SDL_UnlockMutex(fake.mutex);
    return window;
}

/* Whether the driver's window class is registered under kernel32.dll, the
   module the fake's module lookup answers with */
static bool DT_ClassRegistered(WNDCLASSEXW *out)
{
    WNDCLASSEXW wc;

    SDL_zero(wc);
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(GetModuleHandleW(L"kernel32.dll"), L"SDL_ICade", &wc)) {
        return false;
    }
    if (out) {
        *out = wc;
    }
    return true;
}

/* The keyboard registration the fake holds for the process, false when none */
static bool DT_KeyboardRegistration(RAWINPUTDEVICE *out)
{
    bool found = false;
    int i;

    SDL_LockMutex(fake.mutex);
    for (i = 0; i < fake.nregistered; ++i) {
        if (fake.registered[i].usUsagePage == 1 && fake.registered[i].usUsage == 6) {
            if (out) {
                *out = fake.registered[i];
            }
            found = true;
        }
    }
    SDL_UnlockMutex(fake.mutex);
    return found;
}

static HRAWINPUT DT_Record(HANDLE device, DWORD type, USHORT make_code, USHORT flags, USHORT vkey)
{
    FakeRecord *record;
    int index;

    SDL_LockMutex(fake.mutex);
    if (fake.nrecords == FAKE_MAX_RECORDS) {
        SDL_UnlockMutex(fake.mutex);
        DT_CHECK(false, "the fake holds no more records");
        return NULL;
    }
    index = fake.nrecords++;
    record = &fake.records[index];
    SDL_zerop(record);
    record->raw.header.dwType = type;
    record->raw.header.hDevice = device;
    if (type == RIM_TYPEKEYBOARD) {
        record->size = (UINT)(sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD));
        record->raw.data.keyboard.MakeCode = make_code;
        record->raw.data.keyboard.Flags = flags;
        record->raw.data.keyboard.VKey = vkey;
        record->raw.data.keyboard.Message = (flags & RI_KEY_BREAK) ? WM_KEYUP : WM_KEYDOWN;
    } else {
        /* Read as a RAWKEYBOARD, its first field would be Y's make code
           with no flags, a press of A */
        record->size = (UINT)(sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE));
        record->raw.data.mouse.usFlags = 0x15;
        record->raw.data.mouse.lLastX = 3;
    }
    record->raw.header.dwSize = record->size;
    SDL_UnlockMutex(fake.mutex);
    return (HRAWINPUT)(uintptr_t)(FAKE_RECORD_BASE + index);
}

/* Sends a message to the driver's window and waits until its window
   procedure has returned. False when the window did not answer. */
static bool DT_SendMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    const HWND window = DT_Window();
    DWORD_PTR result = 0;

    if (!window) {
        return false;
    }
    return SendMessageTimeoutW(window, msg, wParam, lParam, SMTO_BLOCK, DT_WAIT_MS, &result) != 0;
}

/* One WM_INPUT record, processed when this returns */
static bool DT_Input(HANDLE device, USHORT make_code, USHORT flags, USHORT vkey, WPARAM code)
{
    const HRAWINPUT record = DT_Record(device, RIM_TYPEKEYBOARD, make_code, flags, vkey);

    return record && DT_SendMessage(WM_INPUT, code, (LPARAM)record);
}

static bool DT_DeviceChange(WPARAM event)
{
    DEV_BROADCAST_DEVICEINTERFACE_W broadcast;

    SDL_zero(broadcast);
    broadcast.dbcc_size = sizeof(broadcast);
    broadcast.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    broadcast.dbcc_classguid = fake_hid_guid;
    return DT_SendMessage(WM_DEVICECHANGE, event, (LPARAM)&broadcast);
}

/* SDL */

static bool DT_Expired(Uint64 start, int timeout_ms)
{
    return SDL_GetTicks() - start >= (Uint64)timeout_ms;
}

/* One pass of SDL's joystick update, which runs every driver's Detect and
   the opened joysticks' Update */
static void DT_Pump(void)
{
    SDL_UpdateJoysticks();
    SDL_Delay(1);
}

static void DT_PumpFor(int ms)
{
    const Uint64 start = SDL_GetTicks();

    while (!DT_Expired(start, ms)) {
        DT_Pump();
    }
}

static int DT_Joysticks(SDL_JoystickID *ids, int max)
{
    SDL_JoystickID *list;
    int count = 0, i;

    list = SDL_GetJoysticks(&count);
    for (i = 0; ids && list && i < count && i < max; ++i) {
        ids[i] = list[i];
    }
    SDL_free(list);
    return count;
}

/* The joystick whose name is name, 0 when none */
static SDL_JoystickID DT_FindJoystick(const char *name)
{
    SDL_JoystickID ids[16];
    const int count = DT_Joysticks(ids, DT_COUNT(ids));
    int i;

    for (i = 0; i < count && i < DT_COUNT(ids); ++i) {
        const char *got = SDL_GetJoystickNameForID(ids[i]);

        if (got && SDL_strcmp(got, name) == 0) {
            return ids[i];
        }
    }
    return 0;
}

static bool DT_WaitJoysticks(int count, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (DT_Joysticks(NULL, 0) == count) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

static bool DT_WaitCounter(const int *counter, int value, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (DT_Fake(counter) >= value) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

static bool DT_WaitRegistration(bool registered, int timeout_ms)
{
    const Uint64 start = SDL_GetTicks();

    for (;;) {
        if (DT_KeyboardRegistration(NULL) == registered) {
            return true;
        }
        if (DT_Expired(start, timeout_ms)) {
            return false;
        }
        DT_Pump();
    }
}

/* The driver's slot for a Raw Input handle, -1 when none */
static int DT_Slot(HANDLE handle)
{
    SDL_Mutex *lock = ICADE_Lock();
    int slot = -1, i;

    SDL_LockMutex(lock);
    for (i = 0; i < ICADE_MAX_DEVICES; ++i) {
        if (icade_devices[i].generation && icade_devices[i].handle == handle) {
            slot = i;
            break;
        }
    }
    SDL_UnlockMutex(lock);
    return slot;
}

/* The last event the driver queued for the handle's device */
static Uint64 DT_Sequence(HANDLE handle)
{
    SDL_Mutex *lock = ICADE_Lock();
    const int slot = DT_Slot(handle);
    Uint64 sequence = 0;

    if (slot >= 0) {
        SDL_LockMutex(lock);
        sequence = icade_devices[slot].queue.sequence;
        SDL_UnlockMutex(lock);
    }
    return sequence;
}

/* The hat the part's rule gives, computed here: opposing directions cancel */
static Uint8 DT_ExpectedHat(Uint16 state)
{
    const bool up = (state & 0x001) != 0, right = (state & 0x002) != 0;
    const bool down = (state & 0x004) != 0, left = (state & 0x008) != 0;
    Uint8 hat = SDL_HAT_CENTERED;

    if (up && !down) {
        hat |= SDL_HAT_UP;
    } else if (down && !up) {
        hat |= SDL_HAT_DOWN;
    }
    if (right && !left) {
        hat |= SDL_HAT_RIGHT;
    } else if (left && !right) {
        hat |= SDL_HAT_LEFT;
    }
    return hat;
}

/* The joystick shows the state: hat from bits 0 to 3, buttons A to H from
   bits 4 to 11 */
static bool DT_CheckState(int line, SDL_Joystick *joystick, Uint16 state, const char *what)
{
    const Uint8 hat = SDL_GetJoystickHat(joystick, 0);
    bool ok = DT_Check(hat == DT_ExpectedHat(state), line, "%s: hat 0x%02X, expected 0x%02X", what, hat, DT_ExpectedHat(state));
    int i;

    for (i = 0; i < 8; ++i) {
        const bool down = SDL_GetJoystickButton(joystick, i);
        const bool expected = ((state >> (4 + i)) & 1) != 0;

        ok = DT_Check(down == expected, line, "%s: button %d is %d, expected %d", what, i, down ? 1 : 0, expected ? 1 : 0) && ok;
    }
    return ok;
}

static void DT_SetHint(const char *name, const char *value)
{
    SDL_SetHintWithPriority(name, value, SDL_HINT_OVERRIDE);
}

/* The log. Every line still reaches SDL's default output. The driver thread
   must never log, so a line logged there counts against it. */
#define DT_LOG_LINES  256
#define DT_LOG_LENGTH 200

static SDL_Mutex *dt_log_lock;
static int dt_log_skipped;
static int dt_log_on_driver_thread;
static char dt_log_lines[DT_LOG_LINES][DT_LOG_LENGTH];
static int dt_log_count;

static void SDLCALL DT_LogOutput(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    SDL_LogOutputFunction output = SDL_GetDefaultLogOutputFunction();
    const DWORD driver = (DWORD)InterlockedCompareExchange(&dt_driver_thread, 0, 0);

    (void)userdata;
    SDL_LockMutex(dt_log_lock);
    if (driver && GetCurrentThreadId() == driver) {
        ++dt_log_on_driver_thread;
    }
    if (message && SDL_strstr(message, "SDL_JOYSTICK_ICADE_DEVICES entry")) {
        ++dt_log_skipped;
    }
    if (message && dt_log_count < DT_LOG_LINES) {
        SDL_strlcpy(dt_log_lines[dt_log_count++], message, DT_LOG_LENGTH);
    }
    SDL_UnlockMutex(dt_log_lock);
    if (output) {
        output(NULL, category, priority, message);
    }
}

/* The lines logged in this scenario that contain text */
static int DT_LogCount(const char *text)
{
    int count = 0, i;

    SDL_LockMutex(dt_log_lock);
    for (i = 0; i < dt_log_count; ++i) {
        if (SDL_strstr(dt_log_lines[i], text)) {
            ++count;
        }
    }
    SDL_UnlockMutex(dt_log_lock);
    return count;
}

/* SDL_Init with every other driver off. A NULL value leaves a hint unset. */
static bool DT_Start(const char *icade, const char *rawinput, const char *devices)
{
    size_t i;

    for (i = 0; i < SDL_arraysize(dt_other_drivers); ++i) {
        DT_SetHint(dt_other_drivers[i][0], dt_other_drivers[i][1]);
    }
    SDL_ResetHint(DT_HINT_ICADE);
    SDL_ResetHint(DT_HINT_RAWINPUT);
    SDL_ResetHint(DT_HINT_DEVICES);
    if (icade) {
        DT_SetHint(DT_HINT_ICADE, icade);
    }
    if (rawinput) {
        DT_SetHint(DT_HINT_RAWINPUT, rawinput);
    }
    if (devices) {
        DT_SetHint(DT_HINT_DEVICES, devices);
    }
    SDL_SetLogPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_DEBUG);
    SDL_SetLogOutputFunction(DT_LogOutput, NULL);
    return DT_CHECK(SDL_Init(SDL_INIT_GAMEPAD), "SDL_Init failed: %s", SDL_GetError());
}

/* SDL_Quit, then what every scenario must leave behind */
static void DT_Stop(void)
{
    Uint64 start, end;
    int elapsed;

    start = SDL_GetPerformanceCounter();
    SDL_Quit();
    end = SDL_GetPerformanceCounter();
    elapsed = (int)((end - start) * 1000 / SDL_GetPerformanceFrequency());

    SDL_LockMutex(fake.mutex);
    fake.quit_returned = true;
    DT_CHECK(fake.opens == fake.closes, "%d keyboards opened and %d closed", fake.opens, fake.closes);
    DT_CHECK(fake.bad_opens == 0, "%d opens asked for access or reached no keyboard", fake.bad_opens);
    DT_CHECK(fake.bad_closes == 0, "%d closes of no open file", fake.bad_closes);
    DT_CHECK(fake.bad_attribute_calls == 0, "%d HidD_GetAttributes calls without an open file or with a wrong Size", fake.bad_attribute_calls);
    DT_CHECK(fake.bad_name_calls == 0, "%d GetRawInputDeviceInfoW calls asked for something else", fake.bad_name_calls);
    DT_CHECK(fake.bad_list_calls == 0, "%d malformed GetRawInputDeviceList calls", fake.bad_list_calls);
    DT_CHECK(fake.bad_data_calls == 0, "%d malformed GetRawInputData calls", fake.bad_data_calls);
    DT_CHECK(fake.bad_register_calls == 0, "%d malformed RegisterRawInputDevices calls", fake.bad_register_calls);
    DT_CHECK(fake.bad_notify_calls == 0, "%d malformed device notification calls", fake.bad_notify_calls);
    DT_CHECK(fake.notify_calls == fake.unnotify_calls, "%d device notifications registered and %d unregistered",
             fake.notify_calls, fake.unnotify_calls);
    /* The driver's own hid.dll reference, taken and released on SDL's
       thread, never SDL_hid.c's shared one */
    DT_CHECK(fake.hid_loads == fake.hid_frees && fake.bad_hid_calls == 0, "hid.dll loaded %d times and freed %d times, %d bad calls",
             fake.hid_loads, fake.hid_frees, fake.bad_hid_calls);
    DT_CHECK(fake.hid_loads == 0 || fake.hid_thread == GetCurrentThreadId(), "hid.dll was loaded on another thread");
    /* The window class under the module that holds the window procedure */
    DT_CHECK(fake.module_lookups == 0 ||
                 (fake.module_flags == (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) &&
                  fake.module_address == (const void *)(uintptr_t)ICADE_WindowProc),
             "the module lookup had flags 0x%lX and another address", fake.module_flags);
    SDL_UnlockMutex(fake.mutex);

    DT_CHECK(!icade_thread && !icade_initialized && !icade_hid && !icade_get_attributes, "the driver's thread or state outlived SDL_Quit");
    DT_CHECK(!DT_ClassRegistered(NULL), "the window class outlived SDL_Quit");
    DT_CHECK(elapsed < DT_QUIT_LIMIT_MS, "SDL_Quit took %d ms", elapsed);
    SDL_Delay(100);
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.calls_after_quit == 0, "the driver made %d calls after SDL_Quit returned", fake.calls_after_quit);
    SDL_UnlockMutex(fake.mutex);
    SDL_LockMutex(dt_log_lock);
    DT_CHECK(dt_log_on_driver_thread == 0, "the driver thread logged %d lines", dt_log_on_driver_thread);
    SDL_UnlockMutex(dt_log_lock);
    (void)InterlockedExchange(&dt_driver_thread, 0);
}

/* The watchdog ends a scenario that hangs, so the log names it */

static volatile LONG64 dt_deadline;

static unsigned __stdcall DT_Watchdog(void *unused)
{
    (void)unused;
    for (;;) {
        const LONG64 deadline = InterlockedCompareExchange64(&dt_deadline, 0, 0);

        if (deadline && (LONG64)GetTickCount64() > deadline) {
            printf("FAIL (%s): the scenario did not finish within %d s\n", dt_scenario, DT_SCENARIO_LIMIT_MS / 1000);
            fflush(stdout);
            _Exit(1);
        }
        Sleep(100);
    }
}

static int dt_scenario_failures;

static void DT_Begin(const char *id, const char *title)
{
    dt_scenario = id;
    dt_scenario_failures = dt_failures;
    printf("scenario (%s) %s\n", id, title);
    Fake_Reset();
    (void)InterlockedExchange(&dt_driver_thread, 0);
    SDL_LockMutex(dt_log_lock);
    dt_log_skipped = 0;
    dt_log_on_driver_thread = 0;
    dt_log_count = 0;
    SDL_UnlockMutex(dt_log_lock);
    InterlockedExchange64(&dt_deadline, (LONG64)GetTickCount64() + DT_SCENARIO_LIMIT_MS);
}

static void DT_End(void)
{
    InterlockedExchange64(&dt_deadline, 0);
    printf("scenario (%s) %s\n", dt_scenario, (dt_failures == dt_scenario_failures) ? "passed" : "FAILED");
}

static bool DT_IsMappingMetaField(const char *field)
{
    static const char *const prefixes[] = { "platform:", "crc:", "type:", "face:", "hint:", "sdk>=:", "sdk<=:" };
    size_t i;

    for (i = 0; i < SDL_arraysize(prefixes); ++i) {
        if (SDL_strncmp(field, prefixes[i], SDL_strlen(prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

/* The gamepad's mapping binds exactly these entries, in any order, past the
   GUID and the name */
static void DT_CheckMapping(int line, SDL_Gamepad *gamepad, const char *const *expected, int nexpected)
{
    char *mapping = SDL_GetGamepadMapping(gamepad);
    char *copy, *field, *next;
    bool matched[32];
    int index = 0, i;

    if (!DT_Check(mapping != NULL, line, "the gamepad has no mapping: %s", SDL_GetError())) {
        return;
    }
    copy = SDL_strdup(mapping);
    SDL_zeroa(matched);
    for (field = copy; field; field = next, ++index) {
        next = SDL_strchr(field, ',');
        if (next) {
            *next++ = '\0';
        }
        if (index < 2 || !*field || DT_IsMappingMetaField(field)) {
            continue;
        }
        for (i = 0; i < nexpected && SDL_strcmp(field, expected[i]) != 0; ++i) {
        }
        if (i == nexpected || matched[i]) {
            DT_Check(false, line, "mapping entry %s is not expected in %s", field, mapping);
        } else {
            matched[i] = true;
        }
    }
    for (i = 0; i < nexpected; ++i) {
        DT_Check(matched[i], line, "mapping lacks %s: %s", expected[i], mapping);
    }
    SDL_free(copy);
    SDL_free(mapping);
}

static void DT_CheckGUID(int line, SDL_JoystickID id, Uint16 vendor, Uint16 product, Uint16 version, const char *name,
                         SDL_JoystickType type)
{
    const SDL_GUID guid = SDL_GetJoystickGUIDForID(id);
    const SDL_GUID expected = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_BLUETOOTH, vendor, product, version, NULL, name, 'i', (Uint8)type);
    char got_text[33], want_text[33];

    SDL_GUIDToString(guid, got_text, sizeof(got_text));
    SDL_GUIDToString(expected, want_text, sizeof(want_text));
    DT_Check(guid.data[0] == SDL_HARDWARE_BUS_BLUETOOTH && guid.data[1] == 0, line, "GUID %s is not on the Bluetooth bus", got_text);
    DT_Check(guid.data[14] == 'i', line, "GUID %s has signature 0x%02X, not 'i'", got_text, guid.data[14]);
    DT_Check(guid.data[15] == (Uint8)type, line, "GUID %s carries type %d, not %d", got_text, guid.data[15], (int)type);
    DT_Check(SDL_memcmp(guid.data, expected.data, sizeof(guid.data)) == 0, line, "GUID %s, expected %s", got_text, want_text);
    DT_Check(SDL_GetJoystickTypeForID(id) == type, line, "joystick type %d, expected %d", (int)SDL_GetJoystickTypeForID(id), (int)type);
}

/* The replay tests, through the window or through the host */

typedef enum DT_Path
{
    DT_WINDOW,
    DT_HOST
} DT_Path;

typedef struct DT_Context
{
    DT_Path path;
    HANDLE device;          /* The decoded keyboard */
    SDL_Joystick *joystick; /* Its joystick, open */
    Uint16 state;           /* What the joystick must show */
} DT_Context;

/* One record, done when this returns. Through the host, the return value
   says whether the handle is a decoded keyboard. */
static void DT_Send(DT_Context *ctx, HANDLE device, USHORT make_code, USHORT flags, USHORT vkey)
{
    if (ctx->path == DT_WINDOW) {
        DT_CHECK(DT_Input(device, make_code, flags, vkey, RIM_INPUTSINK), "the window did not take a WM_INPUT");
    } else {
        const bool known = TestICade_HostFeed(device, make_code, flags);

        DT_CHECK(known == (device == ctx->device), "the host feed said %d for handle %p", known ? 1 : 0, device);
    }
}

/* Update takes what the driver queued, then the joystick must show state */
static void DT_Expect(DT_Context *ctx, int line, const char *what)
{
    DT_Pump();
    DT_CheckState(line, ctx->joystick, ctx->state, what);
}

/* 1: each control's press letter and its key-up hold the control, its
   release letter and key-up release it, and nothing else changes */
static void DT_Test1(DT_Context *ctx)
{
    int i;

    for (i = 0; i < DT_COUNT(dt_controls); ++i) {
        char what[64];

        (void)SDL_snprintf(what, sizeof(what), "test 1, control %d", i);
        DT_Send(ctx, ctx->device, dt_controls[i].press, 0, dt_controls[i].vkey);
        ctx->state |= dt_controls[i].bit;
        DT_Expect(ctx, __LINE__, what);
        DT_Send(ctx, ctx->device, dt_controls[i].press, RI_KEY_BREAK, dt_controls[i].vkey);
        DT_Expect(ctx, __LINE__, what);
        DT_Send(ctx, ctx->device, dt_controls[i].release, 0, 0);
        ctx->state = (Uint16)(ctx->state & ~dt_controls[i].bit);
        DT_Expect(ctx, __LINE__, what);
        DT_Send(ctx, ctx->device, dt_controls[i].release, RI_KEY_BREAK, 0);
        DT_Expect(ctx, __LINE__, what);
    }
}

/* 2: up and right held, hat up-right, then right alone */
static void DT_Test2(DT_Context *ctx)
{
    DT_Send(ctx, ctx->device, 0x11, 0, 'W');
    DT_Send(ctx, ctx->device, 0x11, RI_KEY_BREAK, 'W');
    DT_Send(ctx, ctx->device, 0x20, 0, 'D');
    DT_Send(ctx, ctx->device, 0x20, RI_KEY_BREAK, 'D');
    ctx->state = 0x003;
    DT_Expect(ctx, __LINE__, "test 2, up and right");
    DT_CHECK(SDL_GetJoystickHat(ctx->joystick, 0) == SDL_HAT_RIGHTUP, "test 2: hat 0x%02X, not up-right", SDL_GetJoystickHat(ctx->joystick, 0));
    DT_Send(ctx, ctx->device, 0x12, 0, 'E');
    DT_Send(ctx, ctx->device, 0x12, RI_KEY_BREAK, 'E');
    ctx->state = 0x002;
    DT_Expect(ctx, __LINE__, "test 2, right");
    DT_CHECK(SDL_GetJoystickHat(ctx->joystick, 0) == SDL_HAT_RIGHT, "test 2: hat 0x%02X, not right", SDL_GetJoystickHat(ctx->joystick, 0));
    DT_Send(ctx, ctx->device, 0x2E, 0, 'C');
    ctx->state = 0;
    DT_Expect(ctx, __LINE__, "test 2, released");
}

/* 3: a press letter alone holds, again keeps it, the release letter
   releases, again changes nothing, and a repeat queues nothing */
static void DT_Test3(DT_Context *ctx)
{
    Uint64 sequence;

    DT_Send(ctx, ctx->device, 0x15, 0, 'Y');
    ctx->state = 0x010;
    DT_Expect(ctx, __LINE__, "test 3, A held");
    sequence = DT_Sequence(ctx->device);
    DT_Send(ctx, ctx->device, 0x15, 0, 'Y');
    DT_Expect(ctx, __LINE__, "test 3, A still held");
    DT_CHECK(DT_Sequence(ctx->device) == sequence, "test 3: a repeated press queued an event");
    DT_Send(ctx, ctx->device, 0x14, 0, 'T');
    ctx->state = 0;
    DT_Expect(ctx, __LINE__, "test 3, A released");
    sequence = DT_Sequence(ctx->device);
    DT_Send(ctx, ctx->device, 0x14, 0, 'T');
    DT_Expect(ctx, __LINE__, "test 3, A still released");
    DT_CHECK(DT_Sequence(ctx->device) == sequence, "test 3: a repeated release queued an event");
}

/* 4: B, S, the digit row, an E0 letter and the overrun code change
   nothing, with controls held */
static void DT_Test4(DT_Context *ctx)
{
    Uint64 sequence;
    USHORT code;

    DT_Send(ctx, ctx->device, 0x11, 0, 'W');
    DT_Send(ctx, ctx->device, 0x16, 0, 'U');
    ctx->state = 0x041;
    DT_Expect(ctx, __LINE__, "test 4, up and C");
    sequence = DT_Sequence(ctx->device);
    DT_Send(ctx, ctx->device, 0x30, 0, 'B');
    DT_Send(ctx, ctx->device, 0x1F, 0, 'S');
    for (code = 0x02; code <= 0x0B; ++code) {
        DT_Send(ctx, ctx->device, code, 0, (USHORT)(code == 0x0B ? '0' : '1' + code - 0x02));
    }
    DT_Send(ctx, ctx->device, 0x11, RI_KEY_E0, VK_UP);
    DT_Send(ctx, ctx->device, 0x12, RI_KEY_E0, 0);
    DT_Send(ctx, ctx->device, 0x16, RI_KEY_E1, 0);
    DT_Send(ctx, ctx->device, 0xFF, 0, 0xFF);
    DT_Expect(ctx, __LINE__, "test 4, unchanged");
    DT_CHECK(DT_Sequence(ctx->device) == sequence, "test 4: an ignored record queued an event");
    DT_Send(ctx, ctx->device, 0x12, 0, 'E');
    DT_Send(ctx, ctx->device, 0x21, 0, 'F');
    ctx->state = 0;
    DT_Expect(ctx, __LINE__, "test 4, released");
}

/* 5: the same events from another keyboard change nothing, nor do an
   unknown handle and a mouse record */
static void DT_Test5(DT_Context *ctx, HANDLE other)
{
    const HANDLE unknown = (HANDLE)(uintptr_t)0x5555;
    Uint64 sequence;
    int i;

    DT_Send(ctx, ctx->device, 0x23, 0, 'H');
    ctx->state = 0x020;
    DT_Expect(ctx, __LINE__, "test 5, B held");
    sequence = DT_Sequence(ctx->device);
    for (i = 0; i < DT_COUNT(dt_controls); ++i) {
        DT_Send(ctx, other, dt_controls[i].press, 0, dt_controls[i].vkey);
        DT_Send(ctx, other, dt_controls[i].press, RI_KEY_BREAK, dt_controls[i].vkey);
        DT_Send(ctx, unknown, dt_controls[i].press, 0, dt_controls[i].vkey);
    }
    DT_Send(ctx, other, 0x13, 0, 'R');
    DT_Send(ctx, unknown, 0x13, 0, 'R');
    if (ctx->path == DT_WINDOW) {
        const HRAWINPUT record = DT_Record(ctx->device, RIM_TYPEMOUSE, 0, 0, 0);

        DT_CHECK(record && DT_SendMessage(WM_INPUT, RIM_INPUTSINK, (LPARAM)record), "the window did not take a mouse record");
    } else {
        DT_CHECK(!TestICade_HostFeed(NULL, 0x13, 0), "the host feed took a NULL handle");
    }
    DT_Expect(ctx, __LINE__, "test 5, unchanged");
    DT_CHECK(DT_Sequence(ctx->device) == sequence, "test 5: another device queued an event");
    DT_Send(ctx, ctx->device, 0x13, 0, 'R');
    ctx->state = 0;
    DT_Expect(ctx, __LINE__, "test 5, released");
}

/* 6: the make code decides whatever the layout. On a French AZERTY layout
   Q's code 10 types A and A's code 1E types Q. */
static void DT_Test6(DT_Context *ctx)
{
    DT_Send(ctx, ctx->device, 0x1E, 0, 'Q');
    ctx->state = 0x008;
    DT_Expect(ctx, __LINE__, "test 6, left held");
    DT_Send(ctx, ctx->device, 0x10, 0, 'A');
    ctx->state = 0;
    DT_Expect(ctx, __LINE__, "test 6, left released");
}

/* Every truncation of a keyboard record, from 0 bytes to one short of a
   whole RAWINPUTHEADER and RAWKEYBOARD, changes nothing, though the bytes
   past the count hold a press of A. The whole record then presses A. */
static void DT_TestTruncated(DT_Context *ctx)
{
    const UINT whole = (UINT)(sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD));
    Uint64 sequence;
    UINT n;

    DT_Expect(ctx, __LINE__, "truncated records, nothing held");
    sequence = DT_Sequence(ctx->device);
    for (n = 0; n < whole; ++n) {
        const HRAWINPUT record = DT_Record(ctx->device, RIM_TYPEKEYBOARD, 0x15, 0, 'Y');

        if (record) {
            FakeRecord *entry;

            SDL_LockMutex(fake.mutex);
            entry = &fake.records[(intptr_t)(uintptr_t)record - FAKE_RECORD_BASE];
            entry->truncated = true;
            entry->reported = n;
            SDL_UnlockMutex(fake.mutex);
        }
        DT_CHECK(record && DT_SendMessage(WM_INPUT, RIM_INPUTSINK, (LPARAM)record), "the window did not take a record of %u bytes", n);
    }
    DT_Expect(ctx, __LINE__, "truncated records, unchanged");
    DT_CHECK(DT_Sequence(ctx->device) == sequence, "a truncated record queued an event");
    DT_Send(ctx, ctx->device, 0x15, 0, 'Y');
    ctx->state = 0x010;
    DT_Expect(ctx, __LINE__, "the whole record, A held");
    DT_Send(ctx, ctx->device, 0x14, 0, 'T');
    ctx->state = 0;
    DT_Expect(ctx, __LINE__, "the whole record, A released");
}

/* Every event SDL queued for the joystick since the last flush: a button up
   for each button of mask, the hat centered, then the removal */
static void DT_CheckRemovalEvents(int line, SDL_JoystickID id, Uint8 buttons)
{
    SDL_Event events[64];
    Uint8 released = 0;
    bool centered = false, removed = false, order = true;
    int count, i;

    count = SDL_PeepEvents(events, DT_COUNT(events), SDL_PEEKEVENT, SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_JOYSTICK_REMOVED);
    for (i = 0; i < count; ++i) {
        switch (events[i].type) {
        case SDL_EVENT_JOYSTICK_BUTTON_UP:
            if (events[i].jbutton.which == id && events[i].jbutton.button < 8) {
                released |= (Uint8)(1 << events[i].jbutton.button);
                order = order && !removed;
            }
            break;
        case SDL_EVENT_JOYSTICK_HAT_MOTION:
            if (events[i].jhat.which == id && events[i].jhat.value == SDL_HAT_CENTERED) {
                centered = true;
                order = order && !removed;
            }
            break;
        case SDL_EVENT_JOYSTICK_REMOVED:
            if (events[i].jdevice.which == id) {
                removed = true;
            }
            break;
        default:
            break;
        }
    }
    DT_Check(removed, line, "no removal event for joystick %u", (unsigned)id);
    DT_Check(released == buttons, line, "buttons released on removal 0x%02X, expected 0x%02X", released, buttons);
    DT_Check(centered, line, "the hat was not centered on removal");
    DT_Check(order, line, "a release came after the removal");
}

/* 7: the device leaves with every control released, and comes back with
   nothing held. gidc uses WM_INPUT_DEVICE_CHANGE, else the device list
   read after WM_DEVICECHANGE. Returns the new joystick's ID. */
static SDL_JoystickID DT_Test7(DT_Context *ctx, int fake_device, bool gidc)
{
    const SDL_JoystickID old_id = SDL_GetJoystickID(ctx->joystick);
    SDL_JoystickID new_id;
    char name[64];

    SDL_strlcpy(name, SDL_GetJoystickName(ctx->joystick), sizeof(name));
    DT_Send(ctx, ctx->device, 0x15, 0, 'Y');
    DT_Send(ctx, ctx->device, 0x11, 0, 'W');
    DT_Send(ctx, ctx->device, 0x24, 0, 'J');
    ctx->state = 0x091;
    DT_Expect(ctx, __LINE__, "test 7, held before removal");
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

    DT_SetPresent(fake_device, false);
    if (gidc) {
        DT_CHECK(DT_SendMessage(WM_INPUT_DEVICE_CHANGE, GIDC_REMOVAL, (LPARAM)ctx->device), "the window did not take GIDC_REMOVAL");
    } else {
        DT_CHECK(DT_DeviceChange(DBT_DEVICEREMOVECOMPLETE), "the window did not take WM_DEVICECHANGE");
    }
    if (!DT_CHECK(DT_WaitJoysticks(DT_Joysticks(NULL, 0) - 1, DT_WAIT_MS), "test 7: the joystick stayed after removal")) {
        return 0;
    }
    /* A is button 0 and D button 3 */
    DT_CheckRemovalEvents(__LINE__, old_id, 0x09);
    DT_CHECK(DT_Slot(ctx->device) < 0, "test 7: the driver still decodes the removed device");
    DT_CHECK(!TestICade_HostFeed(ctx->device, 0x15, 0), "test 7: the host feed took the removed device");
    SDL_CloseJoystick(ctx->joystick);
    ctx->joystick = NULL;

    DT_SetPresent(fake_device, true);
    if (gidc) {
        DT_CHECK(DT_SendMessage(WM_INPUT_DEVICE_CHANGE, GIDC_ARRIVAL, (LPARAM)ctx->device), "the window did not take GIDC_ARRIVAL");
    } else {
        DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    }
    if (!DT_CHECK(DT_WaitJoysticks(DT_Joysticks(NULL, 0) + 1, DT_WAIT_MS), "test 7: no joystick after arrival")) {
        return 0;
    }
    new_id = DT_FindJoystick(name);
    DT_CHECK(new_id != 0 && new_id != old_id, "test 7: the returning device has ID %u, the old one %u", (unsigned)new_id, (unsigned)old_id);
    ctx->joystick = SDL_OpenJoystick(new_id);
    if (!DT_CHECK(ctx->joystick != NULL, "test 7: SDL_OpenJoystick failed: %s", SDL_GetError())) {
        return 0;
    }
    ctx->state = 0;
    DT_Expect(ctx, __LINE__, "test 7, nothing held after arrival");
    DT_Send(ctx, ctx->device, 0x23, 0, 'H');
    ctx->state = 0x020;
    DT_Expect(ctx, __LINE__, "test 7, B held after arrival");
    DT_Send(ctx, ctx->device, 0x13, 0, 'R');
    ctx->state = 0;
    DT_Expect(ctx, __LINE__, "test 7, B released after arrival");
    return new_id;
}

/* The window is among the message-only windows of the driver's class, which
   FindWindowEx lists for HWND_MESSAGE across every process */
static bool DT_IsMessageOnly(HWND window)
{
    HWND found = NULL;

    while ((found = FindWindowExW(HWND_MESSAGE, found, L"SDL_ICade", NULL)) != NULL) {
        if (found == window) {
            return true;
        }
    }
    return false;
}

/* The cabinet as SDL shows it */
static void DT_CheckCabinet(int line, SDL_JoystickID id)
{
    static const char *const mapping[] = {
        "a:b1", "b:b3", "x:b0", "y:b2", "leftshoulder:b6", "rightshoulder:b4", "lefttrigger:b7", "righttrigger:b5",
        "dpup:h0.1", "dpdown:h0.4", "dpleft:h0.8", "dpright:h0.2"
    };
    const char *path = SDL_GetJoystickPathForID(id);
    SDL_Gamepad *gamepad;

    DT_Check(id != 0, line, "no cabinet joystick");
    DT_CheckGUID(line, id, 0x15E4, 0x0132, DT_CABINET_VERSION, "ION iCade", SDL_JOYSTICK_TYPE_ARCADE_STICK);
    DT_Check(path && SDL_strcmp(path, DT_CABINET_NAME) == 0, line, "path \"%s\"", path ? path : "(null)");
    DT_Check(SDL_IsGamepad(id), line, "the cabinet is not a gamepad");
    gamepad = SDL_OpenGamepad(id);
    if (DT_Check(gamepad != NULL, line, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        SDL_Joystick *joystick = SDL_GetGamepadJoystick(gamepad);

        DT_CheckMapping(line, gamepad, mapping, DT_COUNT(mapping));
        DT_Check(SDL_GetNumJoystickAxes(joystick) == 0 && SDL_GetNumJoystickHats(joystick) == 1 &&
                     SDL_GetNumJoystickButtons(joystick) == 8,
                 line, "%d axes, %d hats and %d buttons", SDL_GetNumJoystickAxes(joystick), SDL_GetNumJoystickHats(joystick),
                 SDL_GetNumJoystickButtons(joystick));
        DT_Check(SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRELESS, line, "not wireless");
        SDL_CloseGamepad(gamepad);
    }
}

/* Scenario (a): start-up in Raw Input mode */
static void ScenarioStartup(void)
{
    SDL_JoystickID ids[8];
    RAWINPUTDEVICE registration;
    WNDCLASSEXW wc, own;
    SDL_Joystick *joystick;
    HWND window;
    Uint64 sequence;
    int cabinet, lists, late;

    DT_Begin("a", "start-up and identity with the Raw Input hint on");
    cabinet = DT_AddUsualDevices();
    if (!DT_Start(NULL, NULL, NULL)) {
        goto done;
    }
    /* The first list is done before SDL_Init returns, and the messages of
       the driver thread are logged on this thread before it returns */
    DT_CHECK(DT_Joysticks(ids, DT_COUNT(ids)) == 1, "%d joysticks right after SDL_Init, expected 1", DT_Joysticks(NULL, 0));
    DT_CHECK(DT_LogCount("iCade: reading keyboard Raw Input") == 1, "the registration was logged %d times before SDL_Init returned",
             DT_LogCount("iCade: reading keyboard Raw Input"));
    DT_CheckCabinet(__LINE__, DT_FindJoystick("ION iCade"));

    /* The driver's own hid.dll reference, taken on this thread, and the
       window class under the module that holds the window procedure, not
       under this program's */
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.hid_loads == 1 && fake.hid_frees == 0 && fake.hid_lookups == 1 && fake.hid_thread == GetCurrentThreadId(),
             "hid.dll loaded %d times and freed %d times", fake.hid_loads, fake.hid_frees);
    DT_CHECK(fake.module_lookups == 1, "%d module lookups", fake.module_lookups);
    SDL_UnlockMutex(fake.mutex);
    DT_CHECK(DT_ClassRegistered(&wc) && wc.lpfnWndProc == ICADE_WindowProc, "the class is not under the window procedure's module");
    SDL_zero(own);
    own.cbSize = sizeof(own);
    DT_CHECK(!GetClassInfoExW(GetModuleHandleW(NULL), L"SDL_ICade", &own), "the class is under this program's module");

    /* Make code 0 with RI_KEY_BREAK changes no control, so a host can ask
       whether a handle is decoded */
    sequence = DT_Sequence(DT_CABINET);
    DT_CHECK(TestICade_HostFeed(DT_CABINET, 0, RI_KEY_BREAK), "the break probe did not find the cabinet");
    DT_CHECK(!TestICade_HostFeed(DT_KEYBOARD, 0, RI_KEY_BREAK), "the break probe found the desk keyboard");
    DT_CHECK(DT_Sequence(DT_CABINET) == sequence, "the break probe changed the cabinet");

    /* Opened while C is held, the joystick shows C at once, and B's press
       and release, queued before the open, are not replayed */
    DT_CHECK(DT_Input(DT_CABINET, 0x23, 0, 'H', RIM_INPUTSINK) && DT_Input(DT_CABINET, 0x13, 0, 'R', RIM_INPUTSINK) &&
                 DT_Input(DT_CABINET, 0x16, 0, 'U', RIM_INPUTSINK),
             "the window did not take a WM_INPUT");
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    joystick = SDL_OpenJoystick(DT_FindJoystick("ION iCade"));
    if (DT_CHECK(joystick != NULL, "SDL_OpenJoystick failed: %s", SDL_GetError())) {
        SDL_Event events[32];
        int count, i;
        bool replayed = false;

        DT_Pump();
        DT_CheckState(__LINE__, joystick, 0x040, "opened with C held");
        count = SDL_PeepEvents(events, DT_COUNT(events), SDL_PEEKEVENT, SDL_EVENT_JOYSTICK_BUTTON_DOWN, SDL_EVENT_JOYSTICK_BUTTON_DOWN);
        for (i = 0; i < count; ++i) {
            replayed = replayed || events[i].jbutton.button == 1;
        }
        DT_CHECK(!replayed, "B's press, queued before the open, reached the joystick");
        DT_CHECK(DT_Input(DT_CABINET, 0x21, 0, 'F', RIM_INPUTSINK), "the window did not take a WM_INPUT");
        DT_Pump();
        DT_CheckState(__LINE__, joystick, 0x000, "C released");
        SDL_CloseJoystick(joystick);
    }

    window = DT_Window();
    DT_CHECK(window != NULL, "no device notification window");
    DT_CHECK(DT_KeyboardRegistration(&registration), "no keyboard Raw Input registration");
    DT_CHECK(registration.dwFlags == (RIDEV_INPUTSINK | RIDEV_DEVNOTIFY) && registration.dwFlags == 0x2100,
             "registration flags 0x%08lX", registration.dwFlags);
    DT_CHECK(registration.hwndTarget == window, "the registration names another window");
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.register_calls == 1, "%d RegisterRawInputDevices calls", fake.register_calls);
    DT_CHECK(fake.register_thread != GetCurrentThreadId() && fake.register_thread == fake.notify_thread,
             "the registration was not made on the driver's thread");
    /* Keyboards only, each named and opened once */
    DT_CHECK(fake.devices[cabinet].opens == 1 && fake.devices[1].opens == 1, "keyboards opened %d and %d times",
             fake.devices[cabinet].opens, fake.devices[1].opens);
    DT_CHECK(fake.devices[2].opens == 0 && fake.devices[3].opens == 0 && fake.devices[2].name_calls == 0 &&
                 fake.devices[3].name_calls == 0,
             "the driver named or opened a mouse or a HID device");
    SDL_UnlockMutex(fake.mutex);
    DT_CHECK(GetWindowThreadProcessId(window, NULL) != GetCurrentThreadId(), "the window belongs to the main thread");
    DT_CHECK(DT_IsMessageOnly(window), "the window is not message-only");

    /* A device change lists again, and a keyboard seen before is not opened again */
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitCounter(&fake.list_calls, 4, DT_WAIT_MS), "no list after WM_DEVICECHANGE");
    DT_PumpFor(50);
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.devices[cabinet].opens == 1, "the cabinet was opened %d times", fake.devices[cabinet].opens);
    SDL_UnlockMutex(fake.mutex);
    DT_CHECK(DT_Joysticks(NULL, 0) == 1, "%d joysticks after a list", DT_Joysticks(NULL, 0));

    /* GIDC_ARRIVAL lists again only for a keyboard not listed yet, since
       registering reports every keyboard already there */
    lists = DT_Fake(&fake.list_calls);
    DT_CHECK(DT_SendMessage(WM_INPUT_DEVICE_CHANGE, GIDC_ARRIVAL, (LPARAM)DT_KEYBOARD), "the window did not take GIDC_ARRIVAL");
    DT_CHECK(DT_Fake(&fake.list_calls) == lists, "GIDC_ARRIVAL of a listed keyboard listed again");
    late = DT_AddDevice((HANDLE)(uintptr_t)0x1004, RIM_TYPEKEYBOARD, "\\\\?\\HID#VID_04D9&PID_0169#late", 0x04D9, 0x0169, 1);
    DT_CHECK(DT_SendMessage(WM_INPUT_DEVICE_CHANGE, GIDC_ARRIVAL, (LPARAM)(uintptr_t)0x1004), "the window did not take GIDC_ARRIVAL");
    DT_CHECK(DT_Fake(&fake.list_calls) > lists, "GIDC_ARRIVAL of a new keyboard did not list again");
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.devices[late].opens == 1, "the new keyboard was opened %d times", fake.devices[late].opens);
    SDL_UnlockMutex(fake.mutex);

done:
    DT_Stop();
    DT_CHECK(!DT_KeyboardRegistration(NULL), "the keyboard registration outlived SDL_Quit");
    DT_End();
}

/* Scenarios (b) and (c): tests 1 to 7 through the window or the host */
static void ScenarioReplay(const char *id, DT_Path path)
{
    DT_Context ctx;
    int cabinet;

    DT_Begin(id, path == DT_WINDOW ? "tests 1 to 7 through WM_INPUT" : "test 8, tests 1 to 7 through the host feed");
    SDL_zero(ctx);
    ctx.path = path;
    ctx.device = DT_CABINET;
    cabinet = DT_AddUsualDevices();
    if (!DT_Start(NULL, path == DT_WINDOW ? "1" : "0", NULL)) {
        goto done;
    }
    ctx.joystick = SDL_OpenJoystick(DT_FindJoystick("ION iCade"));
    if (!DT_CHECK(ctx.joystick != NULL, "no cabinet to open: %s", SDL_GetError())) {
        goto done;
    }
    DT_Expect(&ctx, __LINE__, "nothing held at start");
    DT_Test1(&ctx);
    DT_Test2(&ctx);
    DT_Test3(&ctx);
    DT_Test4(&ctx);
    DT_Test5(&ctx, DT_KEYBOARD);
    DT_Test6(&ctx);
    if (path == DT_WINDOW) {
        DT_TestTruncated(&ctx);
        (void)DT_Test7(&ctx, cabinet, true);
    }
    if (ctx.joystick) {
        (void)DT_Test7(&ctx, cabinet, false);
    }
    if (path == DT_HOST) {
        /* 8: no Raw Input registration, ever */
        DT_CHECK(DT_Fake(&fake.register_calls) == 0, "RegisterRawInputDevices ran %d times with the hint off", DT_Fake(&fake.register_calls));
        DT_CHECK(DT_Fake(&fake.data_calls) == 0, "GetRawInputData ran with the hint off");
    }

done:
    if (ctx.joystick) {
        SDL_CloseJoystick(ctx.joystick);
    }
    DT_Stop();
    if (path == DT_HOST) {
        DT_CHECK(DT_Fake(&fake.register_calls) == 0, "RegisterRawInputDevices ran at quit with the hint off");
        DT_CHECK(DT_Fake(&fake.registered_calls) == 0, "GetRegisteredRawInputDevices ran with the hint off");
    }
    DT_End();
}

/* Scenario (d): test 9, a pad listed in SDL_HINT_JOYSTICK_ICADE_DEVICES */
static void ScenarioPad(void)
{
    static const char *const mapping[] = {
        "a:b5", "b:b6", "x:b7", "y:b4", "back:b0", "start:b2", "leftshoulder:b1", "rightshoulder:b3",
        "dpup:h0.1", "dpdown:h0.4", "dpleft:h0.8", "dpright:h0.2"
    };
    static const char pad_name[] = "iCade Controller (0x2dc8/0x9021)";
    SDL_Gamepad *gamepad = NULL;
    SDL_JoystickID id;
    int skipped;

    DT_Begin("d", "test 9, a pad from the devices hint");
    (void)DT_AddUsualDevices();
    (void)DT_AddDevice(DT_PAD, RIM_TYPEKEYBOARD, DT_PAD_NAME, 0x2DC8, 0x9021, 0x0100);
    if (!DT_Start(NULL, NULL, " 0x2DC8 / 0x9021 , bad, 0x0/0x1")) {
        goto done;
    }
    SDL_LockMutex(dt_log_lock);
    skipped = dt_log_skipped;
    SDL_UnlockMutex(dt_log_lock);
    DT_CHECK(skipped == 2, "%d devices hint entries logged as skipped, expected 2", skipped);
    DT_CHECK(DT_Joysticks(NULL, 0) == 2, "%d joysticks, expected the cabinet and the pad", DT_Joysticks(NULL, 0));
    id = DT_FindJoystick(pad_name);
    if (!DT_CHECK(id != 0, "no joystick named \"%s\"", pad_name)) {
        goto done;
    }
    DT_CheckGUID(__LINE__, id, 0x2DC8, 0x9021, 0x0100, pad_name, SDL_JOYSTICK_TYPE_GAMEPAD);
    gamepad = SDL_OpenGamepad(id);
    if (!DT_CHECK(gamepad != NULL, "SDL_OpenGamepad failed: %s", SDL_GetError())) {
        goto done;
    }
    DT_CheckMapping(__LINE__, gamepad, mapping, DT_COUNT(mapping));
    DT_Pump();

    /* 15/0, A's press letter, presses back */
    DT_CHECK(DT_Input(DT_PAD, 0x15, 0, 'Y', RIM_INPUTSINK), "the window did not take a WM_INPUT");
    DT_Pump();
    DT_CHECK(SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK), "A's letter did not press back");
    DT_CHECK(!SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH), "A's letter pressed south");
    /* 25/0, F's press letter, presses south */
    DT_CHECK(DT_Input(DT_PAD, 0x25, 0, 'K', RIM_INPUTSINK), "the window did not take a WM_INPUT");
    DT_Pump();
    DT_CHECK(SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH), "F's letter did not press south");
    /* The rest of the pad table: C start, E north, G east, H west, B and D the shoulders */
    DT_CHECK(DT_Input(DT_PAD, 0x16, 0, 'U', RIM_INPUTSINK) && DT_Input(DT_PAD, 0x17, 0, 'I', RIM_INPUTSINK) &&
                 DT_Input(DT_PAD, 0x18, 0, 'O', RIM_INPUTSINK) && DT_Input(DT_PAD, 0x26, 0, 'L', RIM_INPUTSINK) &&
                 DT_Input(DT_PAD, 0x23, 0, 'H', RIM_INPUTSINK) && DT_Input(DT_PAD, 0x24, 0, 'J', RIM_INPUTSINK),
             "the window did not take a WM_INPUT");
    DT_Pump();
    DT_CHECK(SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START) && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH) &&
                 SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST) && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST) &&
                 SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) &&
                 SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER),
             "the pad table is not the mapping");
    /* The cabinet's letters leave the pad alone */
    DT_CHECK(DT_Input(DT_CABINET, 0x12, 0, 'E', RIM_INPUTSINK), "the window did not take a WM_INPUT");
    DT_CHECK(DT_Input(DT_PAD, 0x11, 0, 'W', RIM_INPUTSINK), "the window did not take a WM_INPUT");
    DT_Pump();
    DT_CHECK(SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP), "the pad's stick is not the D-pad");

    /* Taken out of the hint, the pad goes. Put back, it comes back. */
    SDL_CloseGamepad(gamepad);
    gamepad = NULL;
    DT_SetHint(DT_HINT_DEVICES, "");
    DT_CHECK(DT_WaitJoysticks(1, DT_WAIT_MS), "the pad stayed after the hint dropped it");
    DT_CHECK(DT_FindJoystick(pad_name) == 0 && DT_FindJoystick("ION iCade") != 0, "the wrong joystick went");
    DT_SetHint(DT_HINT_DEVICES, "0x2dc8/0x9021");
    DT_CHECK(DT_WaitJoysticks(2, DT_WAIT_MS), "the pad did not come back with the hint");
    DT_CHECK(DT_FindJoystick(pad_name) != 0, "the pad came back under another name");
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.devices[4].opens == 1, "a hint change opened the pad again, %d opens", fake.devices[4].opens);
    SDL_UnlockMutex(fake.mutex);
    /* The cabinet's IDs listed in the hint keep the cabinet layout */
    DT_SetHint(DT_HINT_DEVICES, "0x15e4/0x0132,0x2dc8/0x9021");
    DT_PumpFor(100);
    DT_CHECK(DT_Joysticks(NULL, 0) == 2 && DT_FindJoystick("ION iCade") != 0, "listing the cabinet changed it");

done:
    if (gamepad) {
        SDL_CloseGamepad(gamepad);
    }
    DT_Stop();
    DT_End();
}

typedef struct DT_FeedCall
{
    SDL_AtomicInt done;
    bool result;
} DT_FeedCall;

static int SDLCALL DT_FeedThread(void *data)
{
    DT_FeedCall *call = (DT_FeedCall *)data;

    call->result = TestICade_HostFeed(DT_CABINET, 0x20, 0);
    SDL_SetAtomicInt(&call->done, 1);
    return 0;
}

/* Scenario (e): keyboard traffic never waits for the joystick lock */
static void ScenarioLock(void)
{
    SDL_Joystick *joystick = NULL;
    SDL_Thread *thread = NULL;
    DT_FeedCall call;
    Uint64 sequence, start;

    DT_Begin("e", "keyboard traffic while the joystick lock is held");
    (void)DT_AddUsualDevices();
    if (!DT_Start(NULL, NULL, NULL)) {
        goto done;
    }
    joystick = SDL_OpenJoystick(DT_FindJoystick("ION iCade"));
    if (!DT_CHECK(joystick != NULL, "no cabinet to open")) {
        goto done;
    }
    DT_Pump();
    sequence = DT_Sequence(DT_CABINET);

    SDL_LockJoysticks();
    /* The window thread decodes and queues while this thread holds the lock */
    DT_CHECK(DT_Input(DT_CABINET, 0x11, 0, 'W', RIM_INPUTSINK), "the window did not answer while the joystick lock was held");
    DT_CHECK(DT_Input(DT_KEYBOARD, 0x1E, 0, 'A', RIM_INPUTSINK), "the window did not answer while the joystick lock was held");
    DT_CHECK(DT_Sequence(DT_CABINET) == sequence + 1, "the window thread queued %d events under the joystick lock",
             (int)(DT_Sequence(DT_CABINET) - sequence));
    /* So does the host's thread */
    SDL_zero(call);
    thread = SDL_CreateThread(DT_FeedThread, "ICadeHostFeed", &call);
    start = SDL_GetTicks();
    while (thread && !SDL_GetAtomicInt(&call.done) && !DT_Expired(start, DT_WAIT_MS)) {
        SDL_Delay(1);
    }
    DT_CHECK(SDL_GetAtomicInt(&call.done) == 1, "the host feed waited for the joystick lock");
    SDL_UnlockJoysticks();
    if (thread) {
        SDL_WaitThread(thread, NULL);
    }
    DT_CHECK(call.result, "the host feed did not take the cabinet's record");
    DT_CHECK(DT_Sequence(DT_CABINET) == sequence + 2, "the host feed queued no event");
    DT_Pump();
    DT_CheckState(__LINE__, joystick, 0x003, "after the lock");

done:
    if (joystick) {
        SDL_CloseJoystick(joystick);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (f): the registration, foreground records, and the hint
   switched at run time */
static void ScenarioRegistration(void)
{
    const HWND other = (HWND)(uintptr_t)0x4242;
    RAWINPUTDEVICE registration, theirs;
    SDL_Joystick *joystick = NULL;

    DT_Begin("f", "the Raw Input registration");
    (void)DT_AddUsualDevices();
    if (!DT_Start(NULL, NULL, NULL)) {
        goto done;
    }
    joystick = SDL_OpenJoystick(DT_FindJoystick("ION iCade"));
    if (!DT_CHECK(joystick != NULL, "no cabinet to open")) {
        goto done;
    }
    /* Foreground input goes to DefWindowProc for its cleanup, background
       input does not need it */
    DT_CHECK(DT_Input(DT_CABINET, 0x15, 0, 'Y', RIM_INPUT) && DT_Input(DT_CABINET, 0x15, RI_KEY_BREAK, 'Y', RIM_INPUT),
             "the window did not take a WM_INPUT");
    DT_CHECK(DT_Input(DT_KEYBOARD, 0x15, 0, 'Y', RIM_INPUTSINK), "the window did not take a WM_INPUT");
    DT_Pump();
    DT_CheckState(__LINE__, joystick, 0x010, "foreground A");
    DT_CHECK(DT_Fake(&fake.foreground_cleanups) == 2 && DT_Fake(&fake.background_cleanups) == 0,
             "%d foreground and %d background records reached DefWindowProc", DT_Fake(&fake.foreground_cleanups),
             DT_Fake(&fake.background_cleanups));

    /* Off at run time: the registration goes and the host feed carries on */
    DT_SetHint(DT_HINT_RAWINPUT, "0");
    DT_CHECK(DT_WaitRegistration(false, DT_WAIT_MS), "the registration stayed with the hint off");
    DT_CHECK(DT_Fake(&fake.remove_calls) == 1, "%d RIDEV_REMOVE calls", DT_Fake(&fake.remove_calls));
    DT_CHECK(TestICade_HostFeed(DT_CABINET, 0x14, 0), "the host feed did not take the cabinet");
    DT_Pump();
    DT_CheckState(__LINE__, joystick, 0x000, "host feed after the switch");
    /* On again: registered again, on the same window */
    DT_SetHint(DT_HINT_RAWINPUT, "1");
    DT_CHECK(DT_WaitRegistration(true, DT_WAIT_MS), "no registration with the hint back on");
    DT_CHECK(DT_KeyboardRegistration(&registration) && registration.hwndTarget == DT_Window(), "registered on another window");

    /* A window registered later takes the class. Quitting leaves its
       registration alone. */
    SDL_zero(theirs);
    theirs.usUsagePage = 1;
    theirs.usUsage = 6;
    theirs.dwFlags = RIDEV_INPUTSINK;
    theirs.hwndTarget = other;
    DT_CHECK(Fake_RegisterRawInputDevices(&theirs, 1, sizeof(theirs)), "the fake refused the other window");

done:
    if (joystick) {
        SDL_CloseJoystick(joystick);
    }
    DT_Stop();
    DT_CHECK(DT_KeyboardRegistration(&registration) && registration.hwndTarget == other,
             "quitting removed the other window's registration");
    DT_CHECK(DT_Fake(&fake.remove_calls) == 1, "%d RIDEV_REMOVE calls in all", DT_Fake(&fake.remove_calls));
    /* The driver thread's last message, logged by SDL_Quit's thread after
       the driver thread ended */
    DT_CHECK(DT_LogCount("iCade: another window holds the keyboard Raw Input registration") == 1,
             "the other window's registration at quit was logged %d times",
             DT_LogCount("iCade: another window holds the keyboard Raw Input registration"));
    DT_End();
}

/* Scenario (g): the device list */
static void ScenarioLists(void)
{
    char long_name[FAKE_NAME_CHARS];
    SDL_JoystickID id;
    int cabinet, unopenable, anonymous, i, lists, decoded, opens;
    int more[ICADE_MAX_DEVICES];

    DT_Begin("g", "the device list");
    /* A long interface path, one character short of the fake's limit */
    SDL_strlcpy(long_name, DT_CABINET_NAME, sizeof(long_name));
    for (i = (int)SDL_strlen(long_name); i < FAKE_NAME_CHARS - 1; ++i) {
        long_name[i] = (char)('a' + i % 26);
    }
    long_name[FAKE_NAME_CHARS - 1] = '\0';
    cabinet = DT_AddDevice(DT_CABINET, RIM_TYPEKEYBOARD, long_name, 0x15E4, 0x0132, DT_CABINET_VERSION);
    unopenable = DT_AddDevice((HANDLE)(uintptr_t)0x1101, RIM_TYPEKEYBOARD, "\\\\?\\HID#VID_15E4&PID_0132#locked", 0x15E4, 0x0132, 1);
    anonymous = DT_AddDevice((HANDLE)(uintptr_t)0x1102, RIM_TYPEKEYBOARD, "\\\\?\\HID#VID_15E4&PID_0132#anonymous", 0x15E4, 0x0132, 1);
    SDL_LockMutex(fake.mutex);
    fake.devices[unopenable].openable = false;
    fake.devices[anonymous].identifiable = false;
    /* A device arrives between the driver's two list calls */
    fake.list_short = 1;
    SDL_UnlockMutex(fake.mutex);
    if (!DT_Start(NULL, NULL, NULL)) {
        goto done;
    }
    DT_CHECK(DT_Joysticks(NULL, 0) == 1, "%d joysticks, expected only the cabinet", DT_Joysticks(NULL, 0));
    id = DT_FindJoystick("ION iCade");
    DT_CHECK(id != 0 && SDL_strcmp(SDL_GetJoystickPathForID(id), long_name) == 0, "the long path did not survive");
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.list_calls >= 4 && fake.list_short == 0, "the short list was not listed again");
    DT_CHECK(fake.devices[anonymous].opens == 1 && fake.devices[unopenable].attempts == 1 && fake.devices[unopenable].opens == 0,
             "unexpected opens");
    SDL_UnlockMutex(fake.mutex);

    /* A failed list changes nothing. The first timer's list fails. */
    lists = DT_Fake(&fake.list_calls);
    SDL_LockMutex(fake.mutex);
    fake.list_fail = 1;
    SDL_UnlockMutex(fake.mutex);
    DT_SetPresent(cabinet, false);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEREMOVECOMPLETE), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitCounter(&fake.list_calls, lists + 1, DT_WAIT_MS), "no list after WM_DEVICECHANGE");
    DT_PumpFor(50);
    DT_CHECK(DT_Joysticks(NULL, 0) == 1, "a failed list removed the cabinet");
    /* The second timer lists again and sees the cabinet gone */
    DT_CHECK(DT_WaitJoysticks(0, DT_WAIT_MS), "the cabinet stayed after a good list");
    DT_SetPresent(cabinet, true);
    lists = DT_Fake(&fake.list_calls);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitJoysticks(1, DT_WAIT_MS), "the cabinet did not come back");
    /* Both timers list, two calls each */
    DT_CHECK(DT_WaitCounter(&fake.list_calls, lists + 4, DT_WAIT_MS), "the second timer did not list");
    DT_PumpFor(50);

    /* A keyboard that did not answer is opened again on every list, while one
       identified before is not */
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.devices[anonymous].opens > 1 && fake.devices[unopenable].attempts > 1, "a keyboard that did not answer was not tried again");
    DT_CHECK(fake.devices[cabinet].opens == 2, "the cabinet was opened %d times, once per arrival expected", fake.devices[cabinet].opens);
    SDL_UnlockMutex(fake.mutex);

    /* A handle listed again under another path is another device. The
       anonymous keyboard's handle now names a cabinet that answers, and once
       identified it is not opened again. */
    SDL_LockMutex(fake.mutex);
    SDL_wcslcpy(fake.devices[anonymous].name, L"\\\\?\\HID#VID_15E4&PID_0132#another", FAKE_NAME_CHARS);
    fake.devices[anonymous].identifiable = true;
    opens = fake.devices[anonymous].opens;
    SDL_UnlockMutex(fake.mutex);
    lists = DT_Fake(&fake.list_calls);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitJoysticks(2, DT_WAIT_MS), "the reused handle was not identified again");
    DT_CHECK(DT_WaitCounter(&fake.list_calls, lists + 4, DT_WAIT_MS), "the second timer did not list");
    DT_PumpFor(50);
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.devices[anonymous].opens == opens + 1, "the reused handle was opened %d more times, expected once",
             fake.devices[anonymous].opens - opens);
    SDL_UnlockMutex(fake.mutex);

    /* An identified handle listed under another path is opened again too.
       The handle names a plain keyboard, then a cabinet again. */
    SDL_LockMutex(fake.mutex);
    SDL_wcslcpy(fake.devices[anonymous].name, L"\\\\?\\HID#VID_04D9&PID_0169#plain", FAKE_NAME_CHARS);
    fake.devices[anonymous].vendor = 0x04D9;
    fake.devices[anonymous].product = 0x0169;
    opens = fake.devices[anonymous].opens;
    SDL_UnlockMutex(fake.mutex);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitJoysticks(1, DT_WAIT_MS), "the handle that names a plain keyboard kept its joystick");
    SDL_LockMutex(fake.mutex);
    SDL_wcslcpy(fake.devices[anonymous].name, L"\\\\?\\HID#VID_15E4&PID_0132#again", FAKE_NAME_CHARS);
    fake.devices[anonymous].vendor = 0x15E4;
    fake.devices[anonymous].product = 0x0132;
    SDL_UnlockMutex(fake.mutex);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitJoysticks(2, DT_WAIT_MS), "the handle that names a cabinet again was not decoded");
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.devices[anonymous].opens >= opens + 2, "an identified handle under a new path was not opened again");
    SDL_UnlockMutex(fake.mutex);

    /* A cabinet whose first opens failed is decoded once an open works */
    SDL_LockMutex(fake.mutex);
    fake.devices[unopenable].openable = true;
    SDL_UnlockMutex(fake.mutex);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitJoysticks(3, DT_WAIT_MS), "the cabinet that opens now was not decoded");
    DT_CHECK(DT_Slot((HANDLE)(uintptr_t)0x1101) >= 0, "the cabinet that opens now has no slot");
    DT_SetPresent(unopenable, false);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEREMOVECOMPLETE), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitJoysticks(2, DT_WAIT_MS), "the cabinet that left stayed");

    /* Past ICADE_MAX_DEVICES the rest wait for a free slot. With the
       cabinet and the reused handle decoded, six of eight more fit. */
    for (i = 0; i < ICADE_MAX_DEVICES; ++i) {
        char name[64];

        (void)SDL_snprintf(name, sizeof(name), "\\\\?\\HID#VID_15E4&PID_0132#more%d", i);
        more[i] = DT_AddDevice((HANDLE)(uintptr_t)(0x1200 + i), RIM_TYPEKEYBOARD, name, 0x15E4, 0x0132, 1);
    }
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitJoysticks(ICADE_MAX_DEVICES, DT_WAIT_MS), "%d joysticks, expected %d", DT_Joysticks(NULL, 0), ICADE_MAX_DEVICES);
    DT_PumpFor(50);
    DT_CHECK(DT_Joysticks(NULL, 0) == ICADE_MAX_DEVICES, "more than %d joysticks", ICADE_MAX_DEVICES);
    for (i = 0, decoded = 0; i < ICADE_MAX_DEVICES; ++i) {
        decoded += (DT_Slot((HANDLE)(uintptr_t)(0x1200 + i)) >= 0) ? 1 : 0;
    }
    DT_CHECK(decoded == ICADE_MAX_DEVICES - 2, "%d of the more devices decoded, expected %d", decoded, ICADE_MAX_DEVICES - 2);
    /* Kept by the driver thread, logged by this one */
    DT_CHECK(DT_LogCount("iCade: more than 8 devices") == 1, "the device limit was logged %d times",
             DT_LogCount("iCade: more than 8 devices"));
    DT_SetPresent(cabinet, false);
    DT_CHECK(DT_SendMessage(WM_INPUT_DEVICE_CHANGE, GIDC_REMOVAL, (LPARAM)DT_CABINET), "the window did not take GIDC_REMOVAL");
    DT_PumpFor(50);
    for (i = 0, decoded = 0; i < ICADE_MAX_DEVICES; ++i) {
        decoded += (DT_Slot((HANDLE)(uintptr_t)(0x1200 + i)) >= 0) ? 1 : 0;
    }
    DT_CHECK(decoded == ICADE_MAX_DEVICES - 1, "a waiting device took no free slot, %d decoded", decoded);
    DT_CHECK(DT_Joysticks(NULL, 0) == ICADE_MAX_DEVICES, "%d joysticks after a slot freed", DT_Joysticks(NULL, 0));
    SDL_LockMutex(fake.mutex);
    for (i = 0; i < ICADE_MAX_DEVICES; ++i) {
        DT_CHECK(fake.devices[more[i]].opens == 1, "device %d opened %d times", i, fake.devices[more[i]].opens);
    }
    SDL_UnlockMutex(fake.mutex);

done:
    DT_Stop();
    DT_End();
}

/* Scenario (h): the driver off, and switched at run time */
static void ScenarioOff(void)
{
    DT_Begin("h", "SDL_JOYSTICK_ICADE off, then on and off at run time");
    (void)DT_AddUsualDevices();
    if (!DT_Start("0", NULL, NULL)) {
        goto done;
    }
    DT_PumpFor(50);
    DT_CHECK(DT_Joysticks(NULL, 0) == 0, "a joystick with the driver off");
    DT_CHECK(!TestICade_HostFeed(DT_CABINET, 0x15, 0), "the host feed took a record with the driver off");
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.notify_calls == 0 && fake.register_calls == 0 && fake.list_calls == 0 && fake.opens == 0,
             "the driver reached the system while off");
    SDL_UnlockMutex(fake.mutex);

    DT_SetHint(DT_HINT_ICADE, "1");
    DT_CHECK(DT_WaitJoysticks(1, DT_WAIT_MS), "no joystick with the driver switched on");
    DT_CHECK(DT_KeyboardRegistration(NULL), "no registration with the driver switched on");
    DT_CHECK(TestICade_HostFeed(DT_CABINET, 0x15, 0), "the host feed did not take the cabinet");
    DT_SetHint(DT_HINT_ICADE, "0");
    DT_CHECK(DT_WaitJoysticks(0, DT_WAIT_MS), "the joystick stayed with the driver switched off");
    DT_CHECK(!DT_KeyboardRegistration(NULL), "the registration stayed with the driver switched off");
    DT_CHECK(DT_Fake(&fake.unnotify_calls) == 1, "the device notification stayed");
    DT_CHECK(!TestICade_HostFeed(DT_CABINET, 0x14, 0), "the host feed took a record after the driver stopped");

done:
    DT_Stop();
    DT_End();
}

/* Scenario (i): more records than the queue holds, between two updates */
static void ScenarioFlood(void)
{
    SDL_Joystick *joystick = NULL;
    SDL_Mutex *lock;
    Uint32 dropped = 0;
    int i, slot;

    DT_Begin("i", "a flood of records between updates");
    (void)DT_AddUsualDevices();
    if (!DT_Start(NULL, "0", NULL)) {
        goto done;
    }
    joystick = SDL_OpenJoystick(DT_FindJoystick("ION iCade"));
    if (!DT_CHECK(joystick != NULL, "no cabinet to open")) {
        goto done;
    }
    DT_Pump();
    for (i = 0; i < 3 * SDL_ICADE_QUEUE_CAPACITY; ++i) {
        (void)TestICade_HostFeed(DT_CABINET, (i & 1) ? 0x14 : 0x15, 0);
    }
    (void)TestICade_HostFeed(DT_CABINET, 0x15, 0);
    (void)TestICade_HostFeed(DT_CABINET, 0x2D, 0);
    lock = ICADE_Lock();
    slot = DT_Slot(DT_CABINET);
    if (slot >= 0) {
        SDL_LockMutex(lock);
        dropped = icade_devices[slot].queue.dropped;
        SDL_UnlockMutex(lock);
    }
    DT_CHECK(dropped > 0, "the queue dropped nothing");
    DT_Pump();
    DT_Pump();
    DT_CheckState(__LINE__, joystick, 0x014, "after the flood");

done:
    if (joystick) {
        SDL_CloseJoystick(joystick);
    }
    DT_Stop();
    DT_End();
}

/* Scenario (j): the keyboard registration exists only while an iCade does,
   since it takes keyboard Raw Input from every other window of the process */
static void ScenarioPresence(void)
{
    RAWINPUTDEVICE registration;
    int cabinet, pad, late, lists;

    DT_Begin("j", "the registration follows the iCades present");
    (void)DT_AddDevice(DT_KEYBOARD, RIM_TYPEKEYBOARD, DT_KEYBOARD_NAME, 0x046D, 0xC31C, 0x6400);
    (void)DT_AddDevice(DT_MOUSE, RIM_TYPEMOUSE, DT_MOUSE_NAME, 0x046D, 0xC077, 0x7200);
    cabinet = DT_AddDevice(DT_CABINET, RIM_TYPEKEYBOARD, DT_CABINET_NAME, 0x15E4, 0x0132, DT_CABINET_VERSION);
    pad = DT_AddDevice(DT_PAD, RIM_TYPEKEYBOARD, DT_PAD_NAME, 0x2DC8, 0x9021, 0x0100);
    late = DT_AddDevice((HANDLE)(uintptr_t)0x1004, RIM_TYPEKEYBOARD, "\\\\?\\HID#VID_04D9&PID_0169#late", 0x04D9, 0x0169, 1);
    DT_SetPresent(cabinet, false);
    DT_SetPresent(pad, false);
    DT_SetPresent(late, false);
    if (!DT_Start(NULL, NULL, "0x2dc8/0x9021")) {
        goto done;
    }

    /* No iCade, the hint on: no registration at start, nor after a list */
    DT_CHECK(DT_Joysticks(NULL, 0) == 0, "%d joysticks with no iCade", DT_Joysticks(NULL, 0));
    lists = DT_Fake(&fake.list_calls);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitCounter(&fake.list_calls, lists + 4, DT_WAIT_MS), "the timers did not list");
    DT_CHECK(DT_Fake(&fake.register_calls) == 0 && !DT_KeyboardRegistration(NULL), "registered for keyboards with no iCade");

    /* The cabinet arrives: one registration, the driver's */
    DT_SetPresent(cabinet, true);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitJoysticks(1, DT_WAIT_MS), "the cabinet did not arrive");
    DT_CHECK(DT_WaitRegistration(true, DT_WAIT_MS), "no registration with the cabinet present");
    DT_CHECK(DT_KeyboardRegistration(&registration) && registration.hwndTarget == DT_Window() &&
                 registration.dwFlags == (RIDEV_INPUTSINK | RIDEV_DEVNOTIFY),
             "the registration is not the driver's");
    DT_CHECK(DT_Fake(&fake.add_calls) == 1 && DT_Fake(&fake.remove_calls) == 0, "%d registrations for the cabinet",
             DT_Fake(&fake.add_calls));

    /* The pad from the devices hint arrives: still one registration. Both
       timers list before the removals below, so only the removals
       themselves can change the registration. */
    DT_SetPresent(pad, true);
    lists = DT_Fake(&fake.list_calls);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitJoysticks(2, DT_WAIT_MS), "the pad did not arrive");
    DT_CHECK(DT_WaitCounter(&fake.list_calls, lists + 4, DT_WAIT_MS), "the timers did not list");
    DT_CHECK(DT_Fake(&fake.add_calls) == 1 && DT_Fake(&fake.remove_calls) == 0, "a second iCade changed the registration");

    /* The cabinet leaves and the pad keeps the registration */
    DT_SetPresent(cabinet, false);
    DT_CHECK(DT_SendMessage(WM_INPUT_DEVICE_CHANGE, GIDC_REMOVAL, (LPARAM)DT_CABINET), "the window did not take GIDC_REMOVAL");
    DT_CHECK(DT_WaitJoysticks(1, DT_WAIT_MS), "the cabinet stayed");
    DT_PumpFor(50);
    DT_CHECK(DT_KeyboardRegistration(NULL) && DT_Fake(&fake.remove_calls) == 0, "the registration went while the pad stayed");

    /* The pad, the last iCade, leaves: the registration goes */
    DT_SetPresent(pad, false);
    DT_CHECK(DT_SendMessage(WM_INPUT_DEVICE_CHANGE, GIDC_REMOVAL, (LPARAM)DT_PAD), "the window did not take GIDC_REMOVAL");
    DT_CHECK(DT_WaitJoysticks(0, DT_WAIT_MS), "the pad stayed");
    DT_CHECK(DT_WaitRegistration(false, DT_WAIT_MS), "the registration stayed with no iCade");
    DT_CHECK(DT_Fake(&fake.remove_calls) == 1, "%d removals after the last iCade left", DT_Fake(&fake.remove_calls));

    /* It comes back with an iCade, the pad alone this time */
    DT_SetPresent(pad, true);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitJoysticks(1, DT_WAIT_MS), "the pad did not come back");
    DT_CHECK(DT_WaitRegistration(true, DT_WAIT_MS), "no registration with the pad back");
    DT_CHECK(DT_Fake(&fake.add_calls) == 2, "%d registrations after the pad came back", DT_Fake(&fake.add_calls));

    /* The pad counts only while the devices hint lists it */
    DT_SetHint(DT_HINT_DEVICES, "");
    DT_CHECK(DT_WaitJoysticks(0, DT_WAIT_MS), "the pad stayed after the hint dropped it");
    DT_CHECK(DT_WaitRegistration(false, DT_WAIT_MS), "the registration stayed after the hint dropped the pad");
    DT_SetHint(DT_HINT_DEVICES, "0x2dc8/0x9021");
    DT_CHECK(DT_WaitJoysticks(1, DT_WAIT_MS), "the pad did not come back with the hint");
    DT_CHECK(DT_WaitRegistration(true, DT_WAIT_MS), "no registration with the pad listed again");
    DT_CHECK(DT_Fake(&fake.add_calls) == 3 && DT_Fake(&fake.remove_calls) == 2, "%d registrations and %d removals",
             DT_Fake(&fake.add_calls), DT_Fake(&fake.remove_calls));

    /* With the Raw Input hint off there is no registration, whatever arrives,
       and the host feed decodes */
    DT_SetHint(DT_HINT_RAWINPUT, "0");
    DT_CHECK(DT_WaitRegistration(false, DT_WAIT_MS), "the registration stayed with the hint off");
    DT_SetPresent(cabinet, true);
    lists = DT_Fake(&fake.list_calls);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitJoysticks(2, DT_WAIT_MS), "the cabinet did not arrive with the hint off");
    DT_CHECK(DT_WaitCounter(&fake.list_calls, lists + 4, DT_WAIT_MS), "the timers did not list");
    DT_CHECK(DT_Fake(&fake.add_calls) == 3 && !DT_KeyboardRegistration(NULL), "registered with the hint off");
    DT_CHECK(TestICade_HostFeed(DT_CABINET, 0x15, 0), "the host feed did not take the cabinet");

    /* On again with both iCades present, then both leave at once */
    DT_SetHint(DT_HINT_RAWINPUT, "1");
    DT_CHECK(DT_WaitRegistration(true, DT_WAIT_MS), "no registration with the hint back on");
    DT_SetPresent(cabinet, false);
    DT_SetPresent(pad, false);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEREMOVECOMPLETE), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitJoysticks(0, DT_WAIT_MS), "the iCades stayed");
    DT_CHECK(DT_WaitRegistration(false, DT_WAIT_MS), "the registration stayed after both left");
    DT_CHECK(DT_Fake(&fake.add_calls) == 4 && DT_Fake(&fake.remove_calls) == 4, "%d registrations and %d removals",
             DT_Fake(&fake.add_calls), DT_Fake(&fake.remove_calls));

    /* The registration reports the keyboards before it returns, the cabinet
       leaves as it is made, and a keyboard never listed arrives, so the
       report lists again inside the registration. No registration may stay
       without an iCade. */
    SDL_LockMutex(fake.mutex);
    fake.register_reports = true;
    fake.leave_on_register = cabinet + 1;
    fake.arrive_on_register = late + 1;
    SDL_UnlockMutex(fake.mutex);
    DT_SetPresent(cabinet, true);
    DT_CHECK(DT_DeviceChange(DBT_DEVICEARRIVAL), "the window did not take WM_DEVICECHANGE");
    DT_CHECK(DT_WaitCounter(&fake.add_calls, 5, DT_WAIT_MS), "the returning cabinet brought no registration");
    /* Well before the 2 s list, so only the list made inside the
       registration can have seen the cabinet leave and the keyboard arrive */
    DT_CHECK(DT_WaitRegistration(false, 1000), "a registration stayed after the cabinet left during it");
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.remove_calls == 5, "%d removals after the cabinet left during the registration", fake.remove_calls);
    DT_CHECK(fake.devices[late].opens == 1, "the keyboard that arrived during the registration was opened %d times",
             fake.devices[late].opens);
    fake.register_reports = false;
    SDL_UnlockMutex(fake.mutex);
    DT_CHECK(DT_WaitJoysticks(0, DT_WAIT_MS), "the cabinet that left during the registration stayed");

done:
    DT_Stop();
    DT_CHECK(!DT_KeyboardRegistration(NULL), "the registration outlived SDL_Quit");
    DT_End();
}

/* Registers a class of usage page 1 for another window of the process, or
   with no window, which follows the keyboard focus. RIDEV_REMOVE takes it
   away. Usage 6 is the keyboard, usage 2 the mouse. */
static bool DT_OtherRegistration(HWND window, USHORT usage, DWORD flags)
{
    RAWINPUTDEVICE theirs;

    SDL_zero(theirs);
    theirs.usUsagePage = 1;
    theirs.usUsage = usage;
    theirs.dwFlags = flags;
    theirs.hwndTarget = (flags & RIDEV_REMOVE) ? NULL : window;
    return Fake_RegisterRawInputDevices(&theirs, 1, sizeof(theirs)) ? true : false;
}

/* WM_DEVICECHANGE, then both of its lists */
static bool DT_ListTwice(DWORD event)
{
    const int lists = DT_Fake(&fake.list_calls);

    return DT_DeviceChange(event) && DT_WaitCounter(&fake.list_calls, lists + 4, DT_WAIT_MS);
}

/* Scenario (k): SDL leaves the keyboard class to a window that holds it,
   and to a registration list it cannot read */
static void ScenarioOwner(void)
{
    const HWND other = (HWND)(uintptr_t)0x4242;
    RAWINPUTDEVICE registration;
    int cabinet, adds, removes, i;

    DT_Begin("k", "another window's keyboard registration, and an unreadable list");
    (void)DT_AddDevice(DT_KEYBOARD, RIM_TYPEKEYBOARD, DT_KEYBOARD_NAME, 0x046D, 0xC31C, 0x6400);
    cabinet = DT_AddDevice(DT_CABINET, RIM_TYPEKEYBOARD, DT_CABINET_NAME, 0x15E4, 0x0132, DT_CABINET_VERSION);
    DT_SetPresent(cabinet, false);
    /* Another window of the process holds the mouse and keyboard classes
       before SDL starts */
    DT_CHECK(DT_OtherRegistration(other, 2, RIDEV_INPUTSINK) && DT_OtherRegistration(other, 6, RIDEV_INPUTSINK),
             "the fake refused the other window");
    adds = DT_Fake(&fake.add_calls);
    if (!DT_Start(NULL, NULL, NULL)) {
        goto done;
    }

    /* The cabinet arrives and is decoded, but SDL does not take the class */
    DT_SetPresent(cabinet, true);
    DT_CHECK(DT_ListTwice(DBT_DEVICEARRIVAL), "the timers did not list");
    DT_CHECK(DT_WaitJoysticks(1, DT_WAIT_MS), "the cabinet did not arrive");
    DT_CHECK(DT_Fake(&fake.add_calls) == adds, "SDL registered while another window held the keyboard class");
    DT_CHECK(DT_KeyboardRegistration(&registration) && registration.hwndTarget == other, "the other window lost the keyboard class");
    DT_CHECK(DT_LogCount("iCade: another window holds keyboard Raw Input") >= 1, "the refusal was not logged");
    DT_CHECK(TestICade_HostFeed(DT_CABINET, 0x15, 0), "the host feed did not take the cabinet");

    /* Ten refusals with no update between them: the driver thread keeps
       eight messages and counts the rest. The second timer's message is
       logged first. */
    DT_PumpFor(50);
    for (i = 0; i < 10; ++i) {
        DT_CHECK(DT_SendMessage(WM_INPUT_DEVICE_CHANGE, GIDC_ARRIVAL, (LPARAM)(uintptr_t)0x9999), "the window did not take GIDC_ARRIVAL");
    }
    DT_Pump();
    DT_CHECK(DT_LogCount("iCade: 2 more messages dropped") == 1, "the dropped messages were not counted");

    /* The other window lets go of the keyboard and keeps the mouse. Nothing
       registers until the next update. */
    DT_CHECK(DT_OtherRegistration(NULL, 6, RIDEV_REMOVE), "the fake refused the removal");
    DT_PumpFor(100);
    DT_CHECK(DT_Fake(&fake.add_calls) == adds && !DT_KeyboardRegistration(NULL), "SDL registered with no update");
    DT_CHECK(DT_ListTwice(DBT_DEVICEARRIVAL), "the timers did not list");
    DT_CHECK(DT_KeyboardRegistration(&registration) && registration.hwndTarget == DT_Window(), "no registration after the other window let go");
    DT_CHECK(DT_Fake(&fake.add_calls) == adds + 1, "%d registrations after the other window let go", DT_Fake(&fake.add_calls) - adds);
    removes = DT_Fake(&fake.remove_calls);

    /* The registrations cannot be read: the cabinet leaves and nothing is
       removed, since SDL cannot see whose registration it is */
    SDL_LockMutex(fake.mutex);
    fake.registered_fail = true;
    SDL_UnlockMutex(fake.mutex);
    DT_SetPresent(cabinet, false);
    DT_CHECK(DT_SendMessage(WM_INPUT_DEVICE_CHANGE, GIDC_REMOVAL, (LPARAM)DT_CABINET), "the window did not take GIDC_REMOVAL");
    DT_CHECK(DT_WaitJoysticks(0, DT_WAIT_MS), "the cabinet stayed");
    DT_CHECK(DT_Fake(&fake.remove_calls) == removes && DT_KeyboardRegistration(NULL), "SDL removed a registration it could not read");
    DT_CHECK(DT_LogCount("could not be read, so SDL removes nothing") == 1, "the unreadable removal was not logged");
    /* The cabinet comes back and nothing is registered */
    DT_SetPresent(cabinet, true);
    DT_CHECK(DT_ListTwice(DBT_DEVICEARRIVAL), "the timers did not list");
    DT_CHECK(DT_WaitJoysticks(1, DT_WAIT_MS), "the cabinet did not come back");
    DT_CHECK(DT_Fake(&fake.add_calls) == adds + 1, "SDL registered while the registrations could not be read");
    DT_CHECK(DT_LogCount("could not be read, so SDL registers nothing") >= 1, "the unreadable registration was not logged");

    /* Readable again, the next update finds the registration this window
       kept and takes it back without registering, and the cabinet leaving
       removes it */
    SDL_LockMutex(fake.mutex);
    fake.registered_fail = false;
    SDL_UnlockMutex(fake.mutex);
    DT_CHECK(DT_ListTwice(DBT_DEVICEARRIVAL), "the timers did not list");
    DT_CHECK(DT_Fake(&fake.add_calls) == adds + 1, "SDL registered a class its window already held");
    DT_SetPresent(cabinet, false);
    DT_CHECK(DT_SendMessage(WM_INPUT_DEVICE_CHANGE, GIDC_REMOVAL, (LPARAM)DT_CABINET), "the window did not take GIDC_REMOVAL");
    DT_CHECK(DT_WaitJoysticks(0, DT_WAIT_MS), "the cabinet stayed");
    DT_CHECK(!DT_KeyboardRegistration(NULL) && DT_Fake(&fake.remove_calls) == removes + 1, "the kept registration was not removed");

    /* A registration with no window follows the keyboard focus, and SDL
       leaves it alone as another window's */
    DT_CHECK(DT_OtherRegistration(NULL, 6, 0), "the fake refused the focus registration");
    adds = DT_Fake(&fake.add_calls);
    DT_SetPresent(cabinet, true);
    DT_CHECK(DT_ListTwice(DBT_DEVICEARRIVAL), "the timers did not list");
    DT_CHECK(DT_WaitJoysticks(1, DT_WAIT_MS), "the cabinet did not come back");
    DT_CHECK(DT_Fake(&fake.add_calls) == adds, "SDL took the class from a registration that follows the focus");

done:
    DT_Stop();
    DT_CHECK(DT_KeyboardRegistration(&registration) && registration.hwndTarget == NULL, "quitting removed the focus registration");
    DT_End();
}

/* Scenario (l): no hid.dll, so no thread and no joystick */
static void ScenarioNoHid(void)
{
    DT_Begin("l", "hid.dll missing");
    (void)DT_AddUsualDevices();
    SDL_LockMutex(fake.mutex);
    fake.hid_missing = true;
    SDL_UnlockMutex(fake.mutex);
    if (!DT_Start(NULL, NULL, NULL)) {
        goto done;
    }
    DT_PumpFor(50);
    DT_CHECK(DT_Joysticks(NULL, 0) == 0, "a joystick without hid.dll");
    DT_CHECK(DT_Fake(&fake.module_lookups) == 0 && DT_Fake(&fake.notify_calls) == 0 && DT_Fake(&fake.register_calls) == 0,
             "the driver thread ran without hid.dll");
    DT_CHECK(DT_LogCount("iCade: hid.dll could not be loaded") == 1, "the missing hid.dll was not logged");
    DT_CHECK(!TestICade_HostFeed(DT_CABINET, 0, RI_KEY_BREAK), "the host feed found the cabinet without hid.dll");

done:
    DT_Stop();
    DT_End();
}

/* Scenario (m): hid.dll without HidD_GetAttributes is released at once */
static void ScenarioNoFunction(void)
{
    DT_Begin("m", "hid.dll without HidD_GetAttributes");
    (void)DT_AddUsualDevices();
    SDL_LockMutex(fake.mutex);
    fake.hid_no_function = true;
    SDL_UnlockMutex(fake.mutex);
    if (!DT_Start(NULL, NULL, NULL)) {
        goto done;
    }
    DT_PumpFor(50);
    DT_CHECK(DT_Joysticks(NULL, 0) == 0, "a joystick without HidD_GetAttributes");
    SDL_LockMutex(fake.mutex);
    DT_CHECK(fake.hid_loads == 1 && fake.hid_frees == 1 && fake.module_lookups == 0, "hid.dll loaded %d times and freed %d times",
             fake.hid_loads, fake.hid_frees);
    SDL_UnlockMutex(fake.mutex);
    DT_CHECK(!icade_hid && !icade_get_attributes, "the driver kept hid.dll");
    DT_CHECK(DT_LogCount("iCade: hid.dll has no HidD_GetAttributes") == 1, "the missing function was not logged");

done:
    DT_Stop();
    DT_End();
}

static bool DT_Wanted(const char *only, char id)
{
    return !only || SDL_strchr(only, id) != NULL;
}

int main(int argc, char *argv[])
{
    const char *only = (argc > 1) ? argv[1] : NULL;
    HANDLE watchdog;

    SDL_SetMainReady();
    Fake_Setup();
    dt_log_lock = SDL_CreateMutex();
    watchdog = (HANDLE)_beginthreadex(NULL, 0, DT_Watchdog, NULL, 0, NULL);
    DT_CHECK(watchdog != NULL, "no watchdog");

    if (DT_Wanted(only, 'a')) {
        ScenarioStartup();
    }
    if (DT_Wanted(only, 'b')) {
        ScenarioReplay("b", DT_WINDOW);
    }
    if (DT_Wanted(only, 'c')) {
        ScenarioReplay("c", DT_HOST);
    }
    if (DT_Wanted(only, 'd')) {
        ScenarioPad();
    }
    if (DT_Wanted(only, 'e')) {
        ScenarioLock();
    }
    if (DT_Wanted(only, 'f')) {
        ScenarioRegistration();
    }
    if (DT_Wanted(only, 'g')) {
        ScenarioLists();
    }
    if (DT_Wanted(only, 'h')) {
        ScenarioOff();
    }
    if (DT_Wanted(only, 'i')) {
        ScenarioFlood();
    }
    if (DT_Wanted(only, 'j')) {
        ScenarioPresence();
    }
    if (DT_Wanted(only, 'k')) {
        ScenarioOwner();
    }
    if (DT_Wanted(only, 'l')) {
        ScenarioNoHid();
    }
    if (DT_Wanted(only, 'm')) {
        ScenarioNoFunction();
    }

    printf("%s: %d checks, %d failures\n", dt_failures ? "FAILED" : "PASSED", dt_checks, dt_failures);
    fflush(stdout);
    return dt_failures ? 1 : 0;
}
