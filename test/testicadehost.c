/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* The host's side of test/testicade.c. It sees only SDL's public headers,
   as an application does, so its call reaches SDL_ICadeProcessRawKeyboard
   through the name SDL exports, which goes through SDL's dynamic API once
   the tree lists the function there. */

#include <SDL3/SDL.h>

bool TestICade_HostFeed(void *device_handle, Uint16 make_code, Uint16 flags)
{
    return SDL_ICadeProcessRawKeyboard(device_handle, make_code, flags);
}
