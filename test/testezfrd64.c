/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Tests the EZFRD64.DLL loader patch of hifihedgehog/SDL#47
   (src/core/windows/SDL_ezfrd64.c) against stand-in modules that
   test/ezfrd64 builds from testezfrd64_module.c. No device opens and no
   joystick or haptic subsystem starts: the test calls the patch's functions
   in a static SDL, and SDL_Quit at the end.

   1. Measured with the test's own loader notification, before SDL
      registers one: the load notification comes after the loader bound the
      module's HeapCreate import and before its entry point ran, and the
      unload notification after its entry point ran for the unload and
      while its image is still mapped. Without the patch the module's heap
      lies above 4 GB, the positive control for every check below 4 GB.
   2. With the patch, EZFRD64.dll and 811EZFRD64.DLL get their entry point's
      heap below 4 GB. Its blocks come from its 16 MB region, a 512 KB block
      too when it comes, and a block larger than the region fails, since the
      heap does not grow. A larger maximum size gets a larger region, and
      destroying a heap releases its region. NOTEZFRD64.dll keeps its heap
      above 4 GB.
   3. A module loaded before the patch gets its later heaps below 4 GB.
   4. Removing the patch puts the module's imports back and removes the
      notification. kernel32's HeapDestroy then leaves the region of a heap
      the patch made, which is why the patch's HeapDestroy releases it. A
      module that stays loaded while the patch goes and comes back destroys
      its heap through the patch again, and the region is released.
   5. The patch keeps the 16 newest heaps it made, and a heap whose entry it
      dropped keeps its region.
   6. With the hint SDL_PATCH_BROKEN_EZFRD64 off, which stands in for a
      notification that cannot be registered, nothing is patched and
      WIN_HasBrokenEZFRD64DLL answers as before the patch.
   7. Two threads load and unload the modules while the patch comes and
      goes ten times, with no deadlock and a heap for every load.
   8. SDL_Quit removes the patch. */

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"
#include "core/windows/SDL_windows.h"

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

#define FOUR_GB     ((ULONG_PTR)1 << 32)
#define REGION_SIZE ((SIZE_T)16 * 1024 * 1024)

static bool Low(const void *p)
{
    return p && (ULONG_PTR)p < FOUR_GB;
}

static bool High(const void *p)
{
    return p && (ULONG_PTR)p >= FOUR_GB;
}

static bool Released(const void *p)
{
    MEMORY_BASIC_INFORMATION region;

    return VirtualQuery(p, &region, sizeof(region)) == sizeof(region) && region.State == MEM_FREE;
}

// The size of the allocation that holds p
static SIZE_T AllocationSize(const void *p)
{
    MEMORY_BASIC_INFORMATION region;
    const BYTE *base, *address;
    SIZE_T size = 0;

    if (VirtualQuery(p, &region, sizeof(region)) != sizeof(region) || region.State == MEM_FREE) {
        return 0;
    }
    base = (const BYTE *)region.AllocationBase;
    for (address = base;; address += region.RegionSize) {
        if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region) || region.State == MEM_FREE ||
            region.AllocationBase != base) {
            break;
        }
        size += region.RegionSize;
    }
    return size;
}

// Releases the region a heap the patch made keeps after kernel32's HeapDestroy
static bool ReleaseRegion(const void *p)
{
    MEMORY_BASIC_INFORMATION region;

    return VirtualQuery(p, &region, sizeof(region)) == sizeof(region) && region.State != MEM_FREE &&
           VirtualFree(region.AllocationBase, 0, MEM_RELEASE);
}

typedef struct Module
{
    HMODULE handle;
    HANDLE (*heap)(void);
    void *(*block)(void);
    void (*set_detach_flag)(volatile LONG *flag);
    HANDLE (*create_heap)(DWORD options, SIZE_T initial, SIZE_T maximum);
    BOOL (*destroy_heap)(HANDLE heap);
} Module;

static bool OpenModule(const wchar_t *name, Module *module)
{
    SDL_zerop(module);
    module->handle = LoadLibraryW(name);
    if (!module->handle) {
        CHECK(false, "%ls did not load: error %lu", name, GetLastError());
        return false;
    }
    module->heap = (HANDLE(*)(void))GetProcAddress(module->handle, "ModuleHeap");
    module->block = (void *(*)(void))GetProcAddress(module->handle, "ModuleBlock");
    module->set_detach_flag = (void (*)(volatile LONG *))GetProcAddress(module->handle, "SetDetachFlag");
    module->create_heap = (HANDLE(*)(DWORD, SIZE_T, SIZE_T))GetProcAddress(module->handle, "CreateModuleHeap");
    module->destroy_heap = (BOOL(*)(HANDLE))GetProcAddress(module->handle, "DestroyModuleHeap");
    if (!module->heap || !module->block || !module->set_detach_flag || !module->create_heap || !module->destroy_heap) {
        CHECK(false, "%ls lacks its exports", name);
        FreeLibrary(module->handle);
        SDL_zerop(module);
        return false;
    }
    return true;
}

/* 1: the test's own loader notification, Microsoft's documented layout */
typedef struct TestString
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} TestString;

typedef struct TestModuleData
{
    ULONG Flags;
    const TestString *FullDllName;
    const TestString *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} TestModuleData;

typedef VOID(CALLBACK *TestNotification)(ULONG reason, const TestModuleData *data, PVOID context);
typedef LONG(NTAPI *TestRegister)(ULONG flags, TestNotification function, PVOID context, PVOID *cookie);
typedef LONG(NTAPI *TestUnregister)(PVOID cookie);

static struct
{
    int loads;
    int unloads;
    bool bound_at_load;    // The HeapCreate slot held kernel32's HeapCreate
    bool attached_at_load; // The entry point had run
    bool mapped_at_unload;
    bool detached_at_unload;
} timing;
static volatile LONG detached;
static void *kernel32_HeapCreate;

static bool IsTestModule(const TestString *name)
{
    static const wchar_t expected[] = L"EZFRD64.dll";
    const size_t length = SDL_arraysize(expected) - 1;

    return name && name->Buffer && name->Length == length * sizeof(wchar_t) &&
           SDL_wcsncasecmp(name->Buffer, expected, length) == 0;
}

/* The import slot for HeapCreate from kernel32.dll, found by name in the
   import lookup table beside the address table (Microsoft's PE format) */
static void **FindHeapCreateSlot(BYTE *base)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY *directory = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const IMAGE_IMPORT_DESCRIPTOR *descriptor;

    for (descriptor = (const IMAGE_IMPORT_DESCRIPTOR *)(base + directory->VirtualAddress); descriptor->Name; ++descriptor) {
        const IMAGE_THUNK_DATA *names;
        IMAGE_THUNK_DATA *slots;

        if (SDL_strcasecmp((const char *)(base + descriptor->Name), "kernel32.dll") != 0 || !descriptor->OriginalFirstThunk) {
            continue;
        }
        names = (const IMAGE_THUNK_DATA *)(base + descriptor->OriginalFirstThunk);
        slots = (IMAGE_THUNK_DATA *)(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                const IMAGE_IMPORT_BY_NAME *import = (const IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);

                if (SDL_strcmp((const char *)import->Name, "HeapCreate") == 0) {
                    return (void **)&slots->u1.Function;
                }
            }
        }
    }
    return NULL;
}

static VOID CALLBACK TimingNotification(ULONG reason, const TestModuleData *data, PVOID context)
{
    (void)context;
    if (!data || !IsTestModule(data->BaseDllName)) {
        return;
    }
    if (reason == 1) {
        void **slot = FindHeapCreateSlot((BYTE *)data->DllBase);
        LONG (*attached)(void) = (LONG(*)(void))GetProcAddress((HMODULE)data->DllBase, "ModuleAttached");

        ++timing.loads;
        timing.bound_at_load = (slot && *slot == kernel32_HeapCreate);
        timing.attached_at_load = (!attached || attached() != 0);
    } else if (reason == 2) {
        MEMORY_BASIC_INFORMATION region;

        ++timing.unloads;
        timing.mapped_at_unload = (VirtualQuery(data->DllBase, &region, sizeof(region)) == sizeof(region) &&
                                   region.State == MEM_COMMIT && region.Type == MEM_IMAGE);
        timing.detached_at_unload = (detached != 0);
    }
}

static void TestTiming(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    TestRegister register_notification = (TestRegister)GetProcAddress(ntdll, "LdrRegisterDllNotification");
    TestUnregister unregister_notification = (TestUnregister)GetProcAddress(ntdll, "LdrUnregisterDllNotification");
    PVOID cookie = NULL;
    Module module;

    scenario = "loader notification timing";
    kernel32_HeapCreate = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapCreate");
    if (!register_notification || !unregister_notification || !kernel32_HeapCreate ||
        register_notification(0, TimingNotification, NULL, &cookie) < 0) {
        CHECK(false, "no loader notification");
        return;
    }
    if (OpenModule(L"EZFRD64.dll", &module)) {
        HANDLE heap = module.heap();

        CHECK(timing.loads == 1 && timing.bound_at_load && !timing.attached_at_load,
              "the load notification: %d loads, HeapCreate bound %d, entry point run %d", timing.loads,
              timing.bound_at_load, timing.attached_at_load);
        CHECK(High(heap) && High(module.block()), "the module's heap at %p without the patch", heap);
        detached = 0;
        module.set_detach_flag(&detached);
        FreeLibrary(module.handle);
        CHECK(timing.unloads == 1 && timing.mapped_at_unload && timing.detached_at_unload,
              "the unload notification: %d unloads, image mapped %d, entry point run %d", timing.unloads,
              timing.mapped_at_unload, timing.detached_at_unload);
    }
    unregister_notification(cookie);
}

/* 2 */
static void TestPatched(void)
{
    static const struct
    {
        const char *scenario;
        const wchar_t *name;
    } patched[] = {
        { "EZFRD64.dll patched", L"EZFRD64.dll" },
        { "811EZFRD64.DLL patched", L"811EZFRD64.DLL" }
    };
    Module module;
    int i;

    scenario = "patch";
    CHECK(WIN_PatchEZFRD64DLL(), "the patch is not in place");
    CHECK(!WIN_HasBrokenEZFRD64DLL(), "EZFRD64.DLL counts as broken with the patch in place");
    for (i = 0; i < (int)SDL_arraysize(patched); ++i) {
        HANDLE heap, larger;
        void *block;
        int n, outside = 0;

        scenario = patched[i].scenario;
        if (!OpenModule(patched[i].name, &module)) {
            continue;
        }
        heap = module.heap();
        block = module.block();
        CHECK(Low(heap) && Low(block), "the entry point's heap at %p, its block at %p", heap, block);
        if (Low(heap)) {
            CHECK(AllocationSize(heap) == REGION_SIZE, "the heap's region is %zu bytes", AllocationSize(heap));
            for (n = 0; n < 1000; ++n) {
                BYTE *p = (BYTE *)HeapAlloc(heap, 0, 1024);

                if (!p || p < (BYTE *)heap || p >= (BYTE *)heap + REGION_SIZE) {
                    ++outside;
                }
            }
            CHECK(outside == 0, "%d of 1000 blocks failed or fell outside the region", outside);
            {
                BYTE *big = (BYTE *)HeapAlloc(heap, 0, 0x80000);

                CHECK(!big || (big >= (BYTE *)heap && big + 0x80000 <= (BYTE *)heap + REGION_SIZE),
                      "a 512 KB block at %p, outside the region", (void *)big);
            }
            CHECK(HeapAlloc(heap, 0, 2 * REGION_SIZE) == NULL, "a block larger than the region came from the heap");
        }
        larger = module.create_heap(0, 0, (SIZE_T)64 * 1024 * 1024);
        CHECK(Low(larger) && AllocationSize(larger) == (SIZE_T)64 * 1024 * 1024, "the heap with a 64 MB maximum at %p, region %zu",
              larger, AllocationSize(larger));
        CHECK(larger && module.destroy_heap(larger) && Released(larger), "the 64 MB region outlived its heap");
        FreeLibrary(module.handle);
        CHECK(heap && Released(heap), "the entry point's region outlived the module");
    }

    scenario = "a module of another name";
    if (OpenModule(L"NOTEZFRD64.dll", &module)) {
        CHECK(High(module.heap()), "NOTEZFRD64.dll's heap at %p", module.heap());
        FreeLibrary(module.handle);
    }
}

/* 3 and 4 */
static void TestLoadedAndRemoved(void)
{
    Module module, other;

    scenario = "module loaded before the patch";
    WIN_UnpatchEZFRD64DLL();
    if (OpenModule(L"EZFRD64.dll", &module)) {
        HANDLE later;

        CHECK(High(module.heap()), "the removed patch still made the heap at %p", module.heap());
        CHECK(WIN_PatchEZFRD64DLL(), "the patch is not in place");
        later = module.create_heap(0, 0, 0);
        CHECK(Low(later), "a heap the module made after the patch at %p", later);
        CHECK(later && module.destroy_heap(later) && Released(later), "the later heap's region outlived it");
        CHECK(High(module.heap()), "the entry point's heap moved");
        FreeLibrary(module.handle);
    }

    scenario = "patch removed";
    if (OpenModule(L"EZFRD64.dll", &module)) {
        HANDLE heap = module.heap(), after;

        CHECK(Low(heap), "the entry point's heap at %p", heap);
        WIN_UnpatchEZFRD64DLL();
        after = module.create_heap(0, 0, 0);
        CHECK(High(after), "a heap made after the removal at %p", after);
        if (after) {
            module.destroy_heap(after);
        }
        FreeLibrary(module.handle);
        /* The module destroyed its heap through kernel32's HeapDestroy,
           which leaves the region the patch reserved */
        CHECK(Low(heap) && !Released(heap), "kernel32's HeapDestroy released the region at %p", heap);
        CHECK(ReleaseRegion(heap), "the region at %p did not release", heap);
    }
    if (OpenModule(L"811EZFRD64.DLL", &other)) {
        CHECK(High(other.heap()), "a module loaded after the removal got its heap at %p", other.heap());
        FreeLibrary(other.handle);
    }

    /* A module that stays loaded while the patch goes and comes back, as
       across SDL_Quit and SDL_Init, destroys its heap through the patch
       again, which releases the region */
    scenario = "patch removed and put back";
    CHECK(WIN_PatchEZFRD64DLL(), "the patch is not in place");
    if (OpenModule(L"EZFRD64.dll", &module)) {
        HANDLE heap = module.heap();

        CHECK(Low(heap), "the entry point's heap at %p", heap);
        WIN_UnpatchEZFRD64DLL();
        CHECK(WIN_PatchEZFRD64DLL(), "the patch did not go back in");
        FreeLibrary(module.handle);
        CHECK(heap && Released(heap), "the region of a heap made before the removal outlived the module");
        if (heap && !Released(heap)) {
            ReleaseRegion(heap);
        }
    }
}

/* 5: the heap table keeps the 16 newest heaps the patch made, and a heap whose
   entry the table dropped keeps its region */
static void TestTable(void)
{
    HANDLE flush[16], heaps[4], first;
    Module module;
    int i, released = 0, kept = 0;

    scenario = "heap table";
    CHECK(WIN_PatchEZFRD64DLL(), "the patch is not in place");
    if (!OpenModule(L"EZFRD64.dll", &module)) {
        return;
    }
    first = module.heap();
    // Sixteen heaps fill the table, whatever it held before
    for (i = 0; i < (int)SDL_arraysize(flush); ++i) {
        flush[i] = module.create_heap(0, 0, 0);
    }
    for (i = 0; i < (int)SDL_arraysize(heaps); ++i) {
        heaps[i] = module.create_heap(0, 0, 0);
    }
    for (i = 0; i < (int)SDL_arraysize(heaps); ++i) {
        released += (heaps[i] && module.destroy_heap(heaps[i]) && Released(heaps[i]));
    }
    for (i = 4; i < (int)SDL_arraysize(flush); ++i) {
        released += (flush[i] && module.destroy_heap(flush[i]) && Released(flush[i]));
    }
    for (i = 0; i < 4; ++i) {
        kept += (flush[i] && module.destroy_heap(flush[i]) && Low(flush[i]) && !Released(flush[i]) && ReleaseRegion(flush[i]));
    }
    CHECK(released == 16 && kept == 4, "%d of the 16 newest heaps released their regions, %d of the 4 oldest kept theirs",
          released, kept);
    FreeLibrary(module.handle);
    CHECK(Low(first) && !Released(first) && ReleaseRegion(first), "the entry point's heap, older than the table, at %p", first);
}

// Whether this machine has the USB Vibration driver's module, as SDL looks for it
static bool MachineHasEZFRD64DLL(void)
{
    static const char *directories[] = { "C:/Windows/USB_Vibration", "C:/Windows/USB Vibration" };
    bool found = false;
    int i;

    for (i = 0; i < (int)SDL_arraysize(directories); ++i) {
        int count = 0;
        char **files = SDL_GlobDirectory(directories[i], "*/EZFRD64.DLL", SDL_GLOB_CASEINSENSITIVE, &count);

        found = found || count > 0;
        SDL_free(files);
    }
    return found;
}

/* 6 */
static void TestRefused(void)
{
    Module module;

    scenario = "patch refused";
    WIN_UnpatchEZFRD64DLL();
    SDL_SetHint("SDL_PATCH_BROKEN_EZFRD64", "0");
    CHECK(!WIN_PatchEZFRD64DLL(), "the patch went in with its hint off");
    CHECK(WIN_HasBrokenEZFRD64DLL() == MachineHasEZFRD64DLL(), "the answer differs from the one before the patch");
    if (OpenModule(L"EZFRD64.dll", &module)) {
        CHECK(High(module.heap()), "the refused patch made the heap at %p", module.heap());
        FreeLibrary(module.handle);
    }
    // One try until the patch is removed, as at SDL_Quit
    SDL_ResetHint("SDL_PATCH_BROKEN_EZFRD64");
    CHECK(!WIN_PatchEZFRD64DLL(), "the patch went in on a second try after the refusal");
    WIN_UnpatchEZFRD64DLL();
    CHECK(WIN_PatchEZFRD64DLL(), "the patch did not go in after the hint was reset");
}

/* 7 */
static SDL_AtomicInt stop;
static SDL_AtomicInt loads;
static SDL_AtomicInt low_heaps;
static SDL_AtomicInt high_heaps;
static SDL_AtomicInt missing;

static int SDLCALL LoadLoop(void *data)
{
    const wchar_t *name = (const wchar_t *)data;

    while (!SDL_GetAtomicInt(&stop)) {
        HMODULE handle = LoadLibraryW(name);
        HANDLE (*heap)(void);
        HANDLE value;

        if (!handle) {
            SDL_AddAtomicInt(&missing, 1);
            continue;
        }
        heap = (HANDLE(*)(void))GetProcAddress(handle, "ModuleHeap");
        value = heap ? heap() : NULL;
        if (Low(value)) {
            SDL_AddAtomicInt(&low_heaps, 1);
        } else if (High(value)) {
            SDL_AddAtomicInt(&high_heaps, 1);
        } else {
            SDL_AddAtomicInt(&missing, 1);
        }
        FreeLibrary(handle);
        SDL_AddAtomicInt(&loads, 1);
    }
    return 0;
}

static void TestThreads(void)
{
    SDL_Thread *threads[2];
    Module module;
    int i;

    scenario = "threads";
    SDL_SetAtomicInt(&stop, 0);
    threads[0] = SDL_CreateThread(LoadLoop, "EZFRD64 loads", (void *)L"EZFRD64.dll");
    threads[1] = SDL_CreateThread(LoadLoop, "811EZFRD64 loads", (void *)L"811EZFRD64.DLL");
    CHECK(threads[0] && threads[1], "the threads did not start");
    for (i = 0; i < 10; ++i) {
        CHECK(WIN_PatchEZFRD64DLL(), "the patch did not go in, pass %d", i);
        SDL_Delay(20);
        WIN_UnpatchEZFRD64DLL();
        SDL_Delay(20);
    }
    SDL_SetAtomicInt(&stop, 1);
    for (i = 0; i < 2; ++i) {
        if (threads[i]) {
            SDL_WaitThread(threads[i], NULL);
        }
    }
    CHECK(SDL_GetAtomicInt(&loads) > 0 && SDL_GetAtomicInt(&missing) == 0,
          "%d loads, %d without a heap", SDL_GetAtomicInt(&loads), SDL_GetAtomicInt(&missing));
    CHECK(SDL_GetAtomicInt(&low_heaps) > 0 && SDL_GetAtomicInt(&high_heaps) > 0,
          "%d heaps below 4 GB and %d above", SDL_GetAtomicInt(&low_heaps), SDL_GetAtomicInt(&high_heaps));
    // The patch works as before
    CHECK(WIN_PatchEZFRD64DLL(), "the patch did not go in after the threads");
    if (OpenModule(L"EZFRD64.dll", &module)) {
        HANDLE heap = module.heap();

        CHECK(Low(heap), "the heap after the threads at %p", heap);
        FreeLibrary(module.handle);
        CHECK(Released(heap), "the region after the threads outlived the module");
    }
}

/* 8 */
static void TestQuit(void)
{
    Module module;

    scenario = "SDL_Quit";
    CHECK(WIN_PatchEZFRD64DLL(), "the patch is not in place");
    if (OpenModule(L"EZFRD64.dll", &module)) {
        HANDLE heap = module.heap(), after;

        CHECK(Low(heap), "the entry point's heap at %p", heap);
        SDL_Quit();
        after = module.create_heap(0, 0, 0);
        CHECK(High(after), "a heap made after SDL_Quit at %p", after);
        if (after) {
            module.destroy_heap(after);
        }
        FreeLibrary(module.handle);
        CHECK(ReleaseRegion(heap), "the region at %p did not release", heap);
    } else {
        SDL_Quit();
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    TestTiming();
    TestPatched();
    TestLoadedAndRemoved();
    TestTable();
    TestRefused();
    TestThreads();
    TestQuit();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
