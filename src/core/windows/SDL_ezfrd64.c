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

#if defined(SDL_PLATFORM_WINDOWS)

#include "SDL_windows.h"

/* The 64-bit EZFRD64.DLL of the USB Vibration force feedback driver, and
   its 811EZFRD64.DLL variant, cannot handle a heap above 4 GB, where 64-bit
   Windows places heaps. DirectInput and GameInput load the module when they
   query a device's capabilities, and the process then crashes. Dolphin
   found the cause and keeps the module's heap below 4 GB (Dolphin
   CompatPatches.cpp, since its PR 5582 of June 2017). This file does the
   same, written from Microsoft's documentation:

   - LdrRegisterDllNotification (ntdll) calls back when a module loads. On
     Windows 11 build 26200 the call comes after the loader bound the
     module's imports and before its entry point runs, and the unload call
     comes after its entry point ran and while its image is still mapped
     (test/ezfrd64 measures both).
   - Each import slot of the module that holds kernel32's HeapCreate or
     HeapDestroy then points at the functions below. The import table is
     the PE format's (IMAGE_DIRECTORY_ENTRY_IMPORT), and a slot is matched
     by the address the loader bound into it.
   - HeapCreate makes the heap with RtlCreateHeap (ntdll) in a region this
     file reserves below 4 GB. Such a heap does not grow past the region,
     16 MB unless the caller asks for more, and RtlDestroyHeap leaves the
     region in place, so HeapDestroy releases it.

   A module that loaded before the notification was registered is patched
   too, but its entry point already made its first heap above 4 GB.
   WIN_UnpatchEZFRD64DLL puts every slot back and removes the notification,
   so no module calls into SDL after SDL_Quit (hifihedgehog/SDL#47). */

#if defined(SDL_PLATFORM_WIN32) && defined(_WIN64)

// The documented layout of UNICODE_STRING and of the notification data
typedef struct EZFRD64_String
{
    USHORT Length; // In bytes, without a terminator
    USHORT MaximumLength;
    PWSTR Buffer;
} EZFRD64_String;

typedef struct EZFRD64_ModuleData
{
    ULONG Flags;
    const EZFRD64_String *FullDllName;
    const EZFRD64_String *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} EZFRD64_ModuleData;

typedef union EZFRD64_NotificationData
{
    EZFRD64_ModuleData Loaded;
    EZFRD64_ModuleData Unloaded;
} EZFRD64_NotificationData;

#define EZFRD64_REASON_LOADED   1
#define EZFRD64_REASON_UNLOADED 2

typedef VOID(CALLBACK *EZFRD64_NotificationFunction)(ULONG reason, const EZFRD64_NotificationData *data, PVOID context);
typedef LONG(NTAPI *EZFRD64_RegisterFunction)(ULONG flags, EZFRD64_NotificationFunction function, PVOID context, PVOID *cookie);
typedef LONG(NTAPI *EZFRD64_UnregisterFunction)(PVOID cookie);
typedef PVOID(NTAPI *EZFRD64_CreateHeapFunction)(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit, PVOID lock, PVOID parameters);
typedef LONG(NTAPI *EZFRD64_GetVersionFunction)(RTL_OSVERSIONINFOW *info);
typedef BOOL(WINAPI *EZFRD64_DestroyHeapFunction)(HANDLE heap);

#define EZFRD64_HEAP_SIZE  (16 * 1024 * 1024)
#define EZFRD64_LOW_START  ((ULONG_PTR)0x00200000) // Leaves the first 2 MB alone
#define EZFRD64_LOW_END    ((ULONG_PTR)1 << 32)
#define EZFRD64_MAX_SLOTS  32
#define EZFRD64_MAX_HEAPS  16

typedef struct EZFRD64_Slot
{
    const void *module; // The module's base address
    void **slot;
    void *original;
    void *replacement;
} EZFRD64_Slot;

typedef struct EZFRD64_Heap
{
    HANDLE heap;
    void *reservation;
} EZFRD64_Heap;

/* The install lock covers installing and removing, which call the loader.
   The table lock covers the slots, the heaps and ezfrd64_active, and is
   never held across a call that can wait on the loader, since the
   notification and the module's heap calls take it under the loader's own
   lock. */
static SRWLOCK ezfrd64_install_lock = SRWLOCK_INIT;
static SRWLOCK ezfrd64_table_lock = SRWLOCK_INIT;
static bool ezfrd64_tried;
static bool ezfrd64_installed;
static PVOID ezfrd64_cookie;
static EZFRD64_UnregisterFunction ezfrd64_unregister;
static bool ezfrd64_active;

// Set before the first patch and unchanged after
static EZFRD64_CreateHeapFunction ezfrd64_create_heap;
static void *ezfrd64_HeapCreate;
static void *ezfrd64_HeapDestroy;
static ULONG_PTR ezfrd64_granularity;

static EZFRD64_Slot ezfrd64_slots[EZFRD64_MAX_SLOTS];
static int ezfrd64_nslots;
static EZFRD64_Heap ezfrd64_heaps[EZFRD64_MAX_HEAPS];
static int ezfrd64_nheaps;

// Reserves size bytes that end below 4 GB, at the lowest free address
static void *EZFRD64_ReserveLow(SIZE_T size)
{
    ULONG_PTR address = EZFRD64_LOW_START;

    while (address < EZFRD64_LOW_END && EZFRD64_LOW_END - address >= size) {
        MEMORY_BASIC_INFORMATION region;
        ULONG_PTR start, end;

        if (VirtualQuery((LPCVOID)address, &region, sizeof(region)) != sizeof(region)) {
            break;
        }
        start = ((ULONG_PTR)region.BaseAddress + ezfrd64_granularity - 1) & ~(ezfrd64_granularity - 1);
        end = (ULONG_PTR)region.BaseAddress + region.RegionSize;
        if (region.State == MEM_FREE && start < end && end - start >= size && start <= EZFRD64_LOW_END - size) {
            void *base = VirtualAlloc((LPVOID)start, size, MEM_RESERVE, PAGE_READWRITE);
            if (base) {
                return base;
            }
        }
        if (end <= address) {
            break;
        }
        address = end;
    }
    return NULL;
}

/* The module's HeapCreate: a heap that does not grow, in a region below
   4 GB. HEAP_NO_SERIALIZE and HEAP_GENERATE_EXCEPTIONS pass through, as
   RtlCreateHeap takes them. The initial size is committed. */
static HANDLE WINAPI EZFRD64_HeapCreate(DWORD options, SIZE_T initial, SIZE_T maximum)
{
    SIZE_T size = EZFRD64_HEAP_SIZE;
    void *base;
    HANDLE heap;

    if (maximum > size) {
        size = (maximum + ezfrd64_granularity - 1) & ~(SIZE_T)(ezfrd64_granularity - 1);
        if (size < maximum) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
    }
    base = EZFRD64_ReserveLow(size);
    if (!base) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    heap = (HANDLE)ezfrd64_create_heap(options & (HEAP_NO_SERIALIZE | HEAP_GENERATE_EXCEPTIONS), base, size, initial, NULL, NULL);
    if (!heap) {
        VirtualFree(base, 0, MEM_RELEASE);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    /* The table keeps the newest heaps. A full table drops its oldest
       entry, whose heap keeps its region if destroyed later, as does a heap
       made while the patch is being removed. */
    AcquireSRWLockExclusive(&ezfrd64_table_lock);
    if (ezfrd64_active) {
        if (ezfrd64_nheaps == EZFRD64_MAX_HEAPS) {
            SDL_memmove(&ezfrd64_heaps[0], &ezfrd64_heaps[1], (EZFRD64_MAX_HEAPS - 1) * sizeof(ezfrd64_heaps[0]));
            --ezfrd64_nheaps;
        }
        ezfrd64_heaps[ezfrd64_nheaps].heap = heap;
        ezfrd64_heaps[ezfrd64_nheaps].reservation = base;
        ++ezfrd64_nheaps;
    }
    ReleaseSRWLockExclusive(&ezfrd64_table_lock);
    return heap;
}

// The module's HeapDestroy, which releases the region of a heap made above
static BOOL WINAPI EZFRD64_HeapDestroy(HANDLE heap)
{
    EZFRD64_Heap entry = { NULL, NULL };
    BOOL result;
    int i;

    AcquireSRWLockExclusive(&ezfrd64_table_lock);
    for (i = 0; i < ezfrd64_nheaps; ++i) {
        if (ezfrd64_heaps[i].heap == heap) {
            entry = ezfrd64_heaps[i];
            SDL_memmove(&ezfrd64_heaps[i], &ezfrd64_heaps[i + 1], (size_t)(ezfrd64_nheaps - i - 1) * sizeof(ezfrd64_heaps[0]));
            --ezfrd64_nheaps;
            break;
        }
    }
    ReleaseSRWLockExclusive(&ezfrd64_table_lock);

    result = ((EZFRD64_DestroyHeapFunction)ezfrd64_HeapDestroy)(heap);
    if (entry.reservation) {
        if (result) {
            VirtualFree(entry.reservation, 0, MEM_RELEASE);
        } else {
            // The heap still exists
            AcquireSRWLockExclusive(&ezfrd64_table_lock);
            if (ezfrd64_active && ezfrd64_nheaps < EZFRD64_MAX_HEAPS) {
                ezfrd64_heaps[ezfrd64_nheaps++] = entry;
            }
            ReleaseSRWLockExclusive(&ezfrd64_table_lock);
        }
    }
    return result;
}

/* Writes value into an import slot that holds expected, making its page
   writable for the write when it is not, then putting the page back */
static bool EZFRD64_WriteSlot(void **slot, void *expected, void *value)
{
    MEMORY_BASIC_INFORMATION region;
    DWORD writable = 0, old;
    bool written;

    if (VirtualQuery(slot, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
        (region.Protect & PAGE_GUARD)) {
        return false;
    }
    switch (region.Protect & 0xFF) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        break;
    case PAGE_READONLY:
        writable = PAGE_READWRITE;
        break;
    case PAGE_EXECUTE_READ:
        writable = PAGE_EXECUTE_READWRITE;
        break;
    default:
        return false;
    }
    if (writable && !VirtualProtect(slot, sizeof(*slot), writable, &old)) {
        return false;
    }
    written = (InterlockedCompareExchangePointer(slot, value, expected) == expected);
    if (writable) {
        VirtualProtect(slot, sizeof(*slot), old, &old);
    }
    return written;
}

/* Points each import slot of a loaded module that holds HeapCreate or
   HeapDestroy at the functions above, and records the slot. Every read
   stays inside the image. Call with the table lock held. */
static void EZFRD64_PatchModuleLocked(BYTE *base, SIZE_T image_size)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_DATA_DIRECTORY *directory;
    SIZE_T offset;

    if (!base || image_size < sizeof(*dos) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        image_size < sizeof(*nt) || (SIZE_T)dos->e_lfanew > image_size - sizeof(*nt)) {
        return;
    }
    nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return;
    }
    directory = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory->VirtualAddress == 0) {
        return;
    }
    /* The table ends with an entry of zeros (Microsoft's PE format, the
       import directory table), and each slot array with a zero slot */
    for (offset = directory->VirtualAddress;
         offset < image_size && image_size - offset >= sizeof(IMAGE_IMPORT_DESCRIPTOR);
         offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        const IMAGE_IMPORT_DESCRIPTOR *descriptor = (const IMAGE_IMPORT_DESCRIPTOR *)(base + offset);
        SIZE_T thunk;

        if (descriptor->Name == 0) {
            break;
        }
        for (thunk = descriptor->FirstThunk; thunk != 0 && thunk <= image_size - sizeof(void *); thunk += sizeof(void *)) {
            void **slot = (void **)(base + thunk);
            void *value = *slot;
            void *replacement;

            if (!value) {
                break;
            }
            if (value == ezfrd64_HeapCreate) {
                replacement = (void *)EZFRD64_HeapCreate;
            } else if (value == ezfrd64_HeapDestroy) {
                replacement = (void *)EZFRD64_HeapDestroy;
            } else {
                continue;
            }
            if (ezfrd64_nslots < EZFRD64_MAX_SLOTS && EZFRD64_WriteSlot(slot, value, replacement)) {
                ezfrd64_slots[ezfrd64_nslots].module = base;
                ezfrd64_slots[ezfrd64_nslots].slot = slot;
                ezfrd64_slots[ezfrd64_nslots].original = value;
                ezfrd64_slots[ezfrd64_nslots].replacement = replacement;
                ++ezfrd64_nslots;
            }
        }
    }
}

// Whether a module name is the given ASCII name, ignoring case
static bool EZFRD64_NameIs(const EZFRD64_String *name, const char *expected)
{
    USHORT i, length;

    if (!name || !name->Buffer) {
        return false;
    }
    length = name->Length / sizeof(WCHAR);
    for (i = 0; i < length; ++i) {
        WCHAR c = name->Buffer[i];
        char e = expected[i];

        if (e == '\0') {
            return false;
        }
        if (c >= 'a' && c <= 'z') {
            c = (WCHAR)(c - 'a' + 'A');
        }
        if (e >= 'a' && e <= 'z') {
            e = (char)(e - 'a' + 'A');
        }
        if (c != (WCHAR)(unsigned char)e) {
            return false;
        }
    }
    return expected[length] == '\0';
}

/* The notification. It calls only this module, the memory functions and
   the lock, as Microsoft's documentation asks of it. */
static VOID CALLBACK EZFRD64_Notification(ULONG reason, const EZFRD64_NotificationData *data, PVOID context)
{
    (void)context;
    if (!data) {
        return;
    }
    if (reason == EZFRD64_REASON_LOADED) {
        if (EZFRD64_NameIs(data->Loaded.BaseDllName, "EZFRD64.DLL") ||
            EZFRD64_NameIs(data->Loaded.BaseDllName, "811EZFRD64.DLL")) {
            AcquireSRWLockExclusive(&ezfrd64_table_lock);
            if (ezfrd64_active) {
                EZFRD64_PatchModuleLocked((BYTE *)data->Loaded.DllBase, data->Loaded.SizeOfImage);
            }
            ReleaseSRWLockExclusive(&ezfrd64_table_lock);
        }
    } else if (reason == EZFRD64_REASON_UNLOADED) {
        // The image is still mapped, and its slots go out of the table
        int i = 0;

        AcquireSRWLockExclusive(&ezfrd64_table_lock);
        while (i < ezfrd64_nslots) {
            if (ezfrd64_slots[i].module == data->Unloaded.DllBase) {
                ezfrd64_slots[i] = ezfrd64_slots[--ezfrd64_nslots];
            } else {
                ++i;
            }
        }
        ReleaseSRWLockExclusive(&ezfrd64_table_lock);
    }
}

// The size of a loaded module's image, from its headers
static SIZE_T EZFRD64_ImageSize(const BYTE *base)
{
    MEMORY_BASIC_INFORMATION region;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt;

    if (VirtualQuery(base, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
        region.BaseAddress != base || region.RegionSize < sizeof(*dos) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < 0 || region.RegionSize < sizeof(*nt) || (SIZE_T)dos->e_lfanew > region.RegionSize - sizeof(*nt)) {
        return 0;
    }
    nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    return nt->OptionalHeader.SizeOfImage;
}

static bool EZFRD64_Install(void)
{
    static const WCHAR *names[] = { L"EZFRD64.dll", L"811EZFRD64.DLL" };
    EZFRD64_RegisterFunction register_notification;
    EZFRD64_GetVersionFunction get_version;
    RTL_OSVERSIONINFOW version;
    HMODULE ntdll, kernel32;
    SYSTEM_INFO info;
    int i;

    /* The hint is the test hook for a notification that cannot be
       registered: the module is left alone, and WIN_HasBrokenEZFRD64DLL
       answers as before. */
    if (!SDL_GetHintBoolean("SDL_PATCH_BROKEN_EZFRD64", true)) {
        return false;
    }
    ntdll = GetModuleHandleW(L"ntdll.dll");
    kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!ntdll || !kernel32) {
        return false;
    }
    /* The patch is measured on Windows 11 and has run in Dolphin on
       Windows 10, so older Windows keeps the old answer. RtlGetVersion
       reports the running version whatever the program's manifest says,
       which VerifyVersionInfo and so WIN_IsWindows10OrGreater do not. */
    get_version = (EZFRD64_GetVersionFunction)GetProcAddress(ntdll, "RtlGetVersion");
    SDL_zero(version);
    version.dwOSVersionInfoSize = sizeof(version);
    if (!get_version || get_version(&version) < 0 || version.dwMajorVersion < 10) {
        return false;
    }
    register_notification = (EZFRD64_RegisterFunction)GetProcAddress(ntdll, "LdrRegisterDllNotification");
    ezfrd64_unregister = (EZFRD64_UnregisterFunction)GetProcAddress(ntdll, "LdrUnregisterDllNotification");
    ezfrd64_create_heap = (EZFRD64_CreateHeapFunction)GetProcAddress(ntdll, "RtlCreateHeap");
    ezfrd64_HeapCreate = (void *)GetProcAddress(kernel32, "HeapCreate");
    ezfrd64_HeapDestroy = (void *)GetProcAddress(kernel32, "HeapDestroy");
    if (!register_notification || !ezfrd64_unregister || !ezfrd64_create_heap || !ezfrd64_HeapCreate || !ezfrd64_HeapDestroy) {
        return false;
    }
    GetSystemInfo(&info);
    ezfrd64_granularity = info.dwAllocationGranularity ? info.dwAllocationGranularity : 0x10000;

    AcquireSRWLockExclusive(&ezfrd64_table_lock);
    ezfrd64_active = true;
    ReleaseSRWLockExclusive(&ezfrd64_table_lock);
    if (register_notification(0, EZFRD64_Notification, NULL, &ezfrd64_cookie) < 0) {
        AcquireSRWLockExclusive(&ezfrd64_table_lock);
        ezfrd64_active = false;
        ReleaseSRWLockExclusive(&ezfrd64_table_lock);
        ezfrd64_cookie = NULL;
        return false;
    }

    // A module loaded before the notification, held loaded while it changes
    for (i = 0; i < (int)SDL_arraysize(names); ++i) {
        HMODULE module = NULL;

        if (GetModuleHandleExW(0, names[i], &module) && module) {
            const SIZE_T image_size = EZFRD64_ImageSize((const BYTE *)module);

            AcquireSRWLockExclusive(&ezfrd64_table_lock);
            EZFRD64_PatchModuleLocked((BYTE *)module, image_size);
            ReleaseSRWLockExclusive(&ezfrd64_table_lock);
            FreeLibrary(module);
        }
    }
    return true;
}

bool WIN_PatchEZFRD64DLL(void)
{
    bool installed;

    AcquireSRWLockExclusive(&ezfrd64_install_lock);
    if (!ezfrd64_tried) {
        ezfrd64_tried = true;
        ezfrd64_installed = EZFRD64_Install();
    }
    installed = ezfrd64_installed;
    ReleaseSRWLockExclusive(&ezfrd64_install_lock);
    return installed;
}

void WIN_UnpatchEZFRD64DLL(void)
{
    AcquireSRWLockExclusive(&ezfrd64_install_lock);
    if (ezfrd64_installed) {
        int i;

        /* No new patch from here on, and every slot back, while the
           notification still drops the slots of a module that unloads. The
           heaps stay in the table: a module still loaded when the patch goes
           in again destroys its heap through the patch, which releases the
           region then. A heap destroyed in between keeps its region, so no
           new heap can take its handle while its entry waits. */
        AcquireSRWLockExclusive(&ezfrd64_table_lock);
        ezfrd64_active = false;
        for (i = 0; i < ezfrd64_nslots; ++i) {
            EZFRD64_WriteSlot(ezfrd64_slots[i].slot, ezfrd64_slots[i].replacement, ezfrd64_slots[i].original);
        }
        ezfrd64_nslots = 0;
        ReleaseSRWLockExclusive(&ezfrd64_table_lock);
        ezfrd64_unregister(ezfrd64_cookie);
        ezfrd64_cookie = NULL;
        ezfrd64_installed = false;
    }
    ezfrd64_tried = false;
    ReleaseSRWLockExclusive(&ezfrd64_install_lock);
}

#else

bool WIN_PatchEZFRD64DLL(void)
{
    return false;
}

void WIN_UnpatchEZFRD64DLL(void)
{
}

#endif // SDL_PLATFORM_WIN32 && _WIN64

#endif // SDL_PLATFORM_WINDOWS
