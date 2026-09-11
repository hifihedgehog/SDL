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
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sdl_paddles {

struct ReportDescriptor {
    std::uint32_t kind = 0;
    std::uint32_t id = 0;
    std::uint32_t maxSize = 0;
};

struct DeviceInfo {
    std::uint16_t vendor = 0;
    std::uint16_t product = 0;
    std::array<std::uint8_t, 32> id{};
    std::array<std::uint8_t, 32> root{};
    std::uint32_t supportedInput = 0;
    std::vector<ReportDescriptor> reports;
};

struct RawReading {
    std::uint64_t generation = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t rawSequence = 0;
    std::uint32_t reportId = 0;
    std::vector<std::uint8_t> bytes;
};

struct ReaderDiagnostics {
    std::uint64_t polls = 0, empty = 0, unchanged = 0, nonraw = 0, raw = 0;
    std::uint64_t latest = 0, cursor = 0, sampleId = 0, referenceWord = 0;
    std::uint64_t flags = 0, timestamp = 0, rawSequence = 0;
    std::uint32_t readingKinds = 0, latestPin = 0;
};

// The owner supplies a writable view of the recovered x64 format. All methods,
// including destruction, run on that owner's thread. The owner keeps the view
// mapped through Poll and calls Retire before unmapping it. Metadata and layout
// are immutable for that view's lifetime. Reading data is immutable while pinned.
class SharedReader final {
public:
    static std::unique_ptr<SharedReader> Create(
        void* base, std::size_t logicalBytes, std::string& error);
    const DeviceInfo& Info() const;
    const ReaderDiagnostics& Diagnostics() const;

    // The native client publishes policy zero after accepting a device view.
    // This changes only this client's input policy. The owner signals its
    // ClientSignal event when changed is true, after this method returns.
    bool UseDefaultInputPolicy(std::uint32_t& previous, bool& changed, std::string& error);
    bool ReadInputPolicy(std::uint32_t& value, std::string& error) const;

    // Replaces out with owned raw payloads in ascending generation order.
    // true + empty error: the observed interval is contiguous (possibly empty).
    // true + "gap: ...": available readings were returned, with missing history.
    // false: out is empty, error is set, and the cursor has not advanced.
    // false + "retry: ...": a transient latest-ID or retention race, retry later.
    // Every other false result is fatal. No references survive a call.
    // Non-raw generations advance the cursor but do not emit repeated raw state.
    bool Poll(std::vector<RawReading>& out, std::string& error);
    void Retire();
    ~SharedReader();

    SharedReader(const SharedReader&) = delete;
    SharedReader& operator=(const SharedReader&) = delete;

private:
    struct Impl;
    explicit SharedReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace sdl_paddles
