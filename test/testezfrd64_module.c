/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* A stand-in for the USB Vibration driver's EZFRD64.DLL, for
   hifihedgehog/SDL#47. Like a module built with an older C runtime, its
   entry point makes a heap through its kernel32 HeapCreate import and
   destroys it on unload through HeapDestroy. test/ezfrd64 builds it as
   EZFRD64.dll, 811EZFRD64.DLL and NOTEZFRD64.dll, a name SDL leaves alone,
   and testezfrd64.c loads them. It links nothing of SDL. */

#include <windows.h>

static HANDLE module_heap;
static void *module_block;
static volatile LONG module_attached;
static volatile LONG *detach_flag;

__declspec(dllexport) HANDLE ModuleHeap(void)
{
    return module_heap;
}

__declspec(dllexport) void *ModuleBlock(void)
{
    return module_block;
}

__declspec(dllexport) LONG ModuleAttached(void)
{
    return module_attached;
}

// The entry point sets *flag to 1 when it runs for the unload
__declspec(dllexport) void SetDetachFlag(volatile LONG *flag)
{
    detach_flag = flag;
}

__declspec(dllexport) HANDLE CreateModuleHeap(DWORD options, SIZE_T initial, SIZE_T maximum)
{
    return HeapCreate(options, initial, maximum);
}

__declspec(dllexport) BOOL DestroyModuleHeap(HANDLE heap)
{
    return HeapDestroy(heap);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        module_heap = HeapCreate(0, 0x1000, 0);
        module_block = module_heap ? HeapAlloc(module_heap, 0, 64) : NULL;
        InterlockedExchange(&module_attached, 1);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (module_heap) {
            HeapDestroy(module_heap);
        }
        if (detach_flag) {
            InterlockedExchange(detach_flag, 1);
        }
    }
    return TRUE;
}
