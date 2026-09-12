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
/* Bounded diagnostic records for the private XInput paddle supplement. */
#ifndef SDL_xinput_paddle_trace_h_
#define SDL_xinput_paddle_trace_h_

#include <cstddef>
#include <cstdint>

namespace sdl_paddles::trace {

enum class Kind : std::uint8_t {
    WgiResume, WgiSuspend, WgiMessage, WgiKey, ServicePacket, Decoded, Queued, Drained,
    CommandQueued, CommandDispatch, CommandComplete, Attachment, Retired,
    PairingLost, FocusChanged, CommandRearm
};

struct Record {
    Kind kind{};
    std::uint64_t order = 0, qpc = 0, nativeId = 0, attachment = 0;
    std::uint64_t provider = 0, epoch = 0, view = 0, reading = 0, sequence = 0;
    std::uint64_t sourceTime = 0, event = 0, command = 0;
    std::int32_t status = 0;
    std::uint32_t instance = 0, report = 0, length = 0;
    std::uint8_t messageClass = 0, mask = 0, profile = 0, copied = 0;
    bool truncated = false;
    std::uint8_t bytes[64]{};
};

// Explicit diagnostic opt-in only. These functions do no I/O or SDL work.
// Disable discards pending diagnostics. Partial drains retain the remainder.
// Record order never resets. QPC is captured before queue locking, so its order
// can differ from lock order. Raw lengths use the native UINT32 report bound.
void SetEnabled(bool enabled) noexcept;
bool Enabled() noexcept;
std::uint64_t NextEvent() noexcept;
void Push(Record record) noexcept;
void Raw(Record record, const std::uint8_t *bytes, std::size_t length) noexcept;
std::size_t Drain(Record *records, std::size_t capacity, std::uint64_t &dropped) noexcept;
const char *Name(Kind kind) noexcept;

} // namespace sdl_paddles::trace
#endif
