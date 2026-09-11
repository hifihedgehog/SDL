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
#include "SDL_xinput_paddle_trace.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>

namespace sdl_paddles::trace {
namespace {
SRWLOCK lock = SRWLOCK_INIT;
std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> nextEvent{0};
std::array<Record, 4096> records{};
std::size_t begin = 0, count = 0;
std::uint64_t order = 0, droppedCount = 0;
}

void SetEnabled(bool value) noexcept
{
    enabled.store(value, std::memory_order_release);
    if (!value) {
        AcquireSRWLockExclusive(&lock);
        begin = count = 0;
        droppedCount = 0;
        ReleaseSRWLockExclusive(&lock);
    }
}

bool Enabled() noexcept { return enabled.load(std::memory_order_acquire); }

std::uint64_t NextEvent() noexcept
{
    if (!Enabled()) return 0;
    auto current = nextEvent.load(std::memory_order_relaxed);
    while (current != UINT64_MAX) {
        if (nextEvent.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) return current + 1;
    }
    return 0;
}

void Push(Record record) noexcept
{
    if (!Enabled()) return;
    if (!record.qpc) {
        LARGE_INTEGER value{};
        if (QueryPerformanceCounter(&value)) record.qpc = static_cast<std::uint64_t>(value.QuadPart);
    }
    AcquireSRWLockExclusive(&lock);
    if (Enabled()) {
        if (count == records.size()) {
            begin = (begin + 1) % records.size();
            --count;
            if (droppedCount != UINT64_MAX) ++droppedCount;
        }
        if (order != UINT64_MAX) ++order;
        record.order = order;
        records[(begin + count) % records.size()] = record;
        ++count;
    }
    ReleaseSRWLockExclusive(&lock);
}

void Raw(Record record, const std::uint8_t *bytes, std::size_t length) noexcept
{
    if (!Enabled()) return;
    record.copied = 0;
    std::memset(record.bytes, 0, sizeof(record.bytes));
    record.length = length > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(length);
    const auto copy = length < sizeof(record.bytes) ? length : sizeof(record.bytes);
    record.truncated = copy != length || (!bytes && length);
    if (bytes && copy) {
        std::memcpy(record.bytes, bytes, copy);
        record.copied = static_cast<std::uint8_t>(copy);
    }
    Push(record);
}

std::size_t Drain(Record *output, std::size_t capacity, std::uint64_t &dropped) noexcept
{
    dropped = 0;
    if (!output || !capacity) return 0;
    AcquireSRWLockExclusive(&lock);
    dropped = droppedCount;
    droppedCount = 0;
    std::size_t copied = 0;
    while (copied < capacity && count) {
        output[copied++] = records[begin];
        begin = (begin + 1) % records.size();
        --count;
    }
    ReleaseSRWLockExclusive(&lock);
    return copied;
}

const char *Name(Kind kind) noexcept
{
    switch (kind) {
    case Kind::WgiResume: return "wgi-resume";
    case Kind::WgiSuspend: return "wgi-suspend";
    case Kind::WgiMessage: return "wgi-raw";
    case Kind::WgiKey: return "wgi-key";
    case Kind::ServicePacket: return "service-raw";
    case Kind::Decoded: return "decoded";
    case Kind::Queued: return "queued";
    case Kind::Drained: return "drained";
    case Kind::CommandQueued: return "command-queued";
    case Kind::CommandDispatch: return "command-dispatch";
    case Kind::CommandComplete: return "command-complete";
    case Kind::Attachment: return "attachment";
    case Kind::Retired: return "retired";
    }
    return "unknown";
}
} // namespace sdl_paddles::trace
