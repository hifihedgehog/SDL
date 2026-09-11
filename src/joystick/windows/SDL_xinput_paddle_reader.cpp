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
#include "SDL_xinput_paddle_reader.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <exception>
#include <intrin.h>
#include <limits>
#include <utility>

#if !defined(_MSC_VER) || !defined(_M_X64) || defined(_M_ARM64EC)
#error This reader requires MSVC targeting native x64.
#endif

namespace sdl_paddles {
#ifdef SDL_XINPUT_PADDLE_READER_TESTING
namespace reader_testing {
void Observe(const char* stage, void* base, std::uint64_t id);
}
#endif

namespace {
constexpr std::size_t SharedHeaderSize = 0x24;
constexpr std::size_t MetadataPrefixSize = 0x148;
constexpr std::size_t InputHeaderSize = 0x2C;
constexpr std::size_t ReadingLayoutSize = 0xA8;
constexpr std::size_t DescriptorSize = 0x18;
constexpr std::size_t ReportItemSize = 0x58;
constexpr std::size_t MaxSlots = 512;
constexpr unsigned MaxPinAttempts = 64;
constexpr std::uint64_t HistoryBit = 1ULL << 29;
constexpr std::uint64_t PreviousBit = 1ULL << 30;
constexpr std::uint64_t NextBit = 1ULL << 31;
constexpr std::uint64_t MaxGeneration = (1ULL << 48) - 1;

bool Fits(std::size_t offset, std::size_t length, std::size_t limit) {
    return offset <= limit && length <= limit - offset;
}

template<class T> T Read(const std::uint8_t* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

// /volatile:ms supplies compiler acquire/release ordering. These aligned x64
// loads and stores do not issue a read-modify-write on a publication cell.
std::uint64_t Load64(const std::uint8_t* address) {
    return std::bit_cast<std::uint64_t>(
        *reinterpret_cast<const volatile __int64*>(address));
}

void Store64(std::uint8_t* address, std::uint64_t value) {
    *reinterpret_cast<volatile __int64*>(address) = std::bit_cast<__int64>(value);
}

void Observe(const char* stage, void* base, std::uint64_t id) {
#ifdef SDL_XINPUT_PADDLE_READER_TESTING
    reader_testing::Observe(stage, base, id);
#else
    (void)stage;
    (void)base;
    (void)id;
#endif
}

struct Range {
    std::size_t offset;
    std::size_t length;
};

bool Separate(Range a, Range b) {
    return a.offset <= b.offset ? a.length <= b.offset - a.offset
                               : b.length <= a.offset - b.offset;
}

bool Fail(std::string& error, const char* message) {
    error = message;
    return false;
}
} // namespace

struct SharedReader::Impl {
    std::uint8_t* base = nullptr;
    std::size_t metadata = 0;
    std::size_t input = 0;
    std::size_t layout = 0;
    std::size_t latest = 0;
    std::size_t bitmap = 0;
    std::size_t entries = 0;
    std::size_t data = 0;
    std::size_t slotCount = 0;
    std::size_t stride = 0;
    std::size_t rawHeader = 0;
    std::size_t rawPayload = 0;
    std::size_t rawCapacity = 0;
    std::size_t rawSequence = 0;
    std::size_t inputPolicy = 0;
    std::uint64_t lastId = 0;
    bool releaseBroken = false;
    DeviceInfo info;
    ReaderDiagnostics diagnostics;
    std::array<std::uint8_t, SharedHeaderSize> headerCopy{};
    std::array<std::uint8_t, InputHeaderSize> inputCopy{};
    std::array<std::uint8_t, ReadingLayoutSize> layoutCopy{};
    std::vector<std::uint8_t> metadataCopy;

    bool Initialize(void* address, std::size_t logicalBytes, std::string& error) {
        const auto integerBase = reinterpret_cast<std::uintptr_t>(address);
        if (!address || (integerBase & 7) != 0 || logicalBytes < SharedHeaderSize ||
            logicalBytes > std::numeric_limits<std::uintptr_t>::max() - integerBase) {
            return Fail(error, "invalid view: null, unaligned, short, or address overflow");
        }
        base = static_cast<std::uint8_t*>(address);
        std::memcpy(headerCopy.data(), base, headerCopy.size());
        const auto* const header = headerCopy.data();
        const std::size_t total = Read<std::uint32_t>(header, 0);
        if (total < SharedHeaderSize || total > logicalBytes) {
            return Fail(error, "invalid layout: total size exceeds the logical view");
        }

        // Other top-level layouts bound the two regions this reader follows.
        // Their contents remain opaque, including every feedback field.
        const std::array<std::size_t, 5> starts{
            Read<std::uint32_t>(header, 4), Read<std::uint32_t>(header, 8),
            Read<std::uint32_t>(header, 0xC), Read<std::uint32_t>(header, 0x10),
            Read<std::uint32_t>(header, 0x18)};
        for (std::size_t i = 0; i < starts.size(); ++i) {
            if (!starts[i]) continue;
            if (starts[i] < SharedHeaderSize || !Fits(starts[i], 4, total)) {
                return Fail(error, "invalid layout: top-level offset is outside its layout");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (starts[i] == starts[j]) {
                    return Fail(error, "invalid layout: top-level regions overlap");
                }
            }
        }
        metadata = starts[1];
        input = starts[3];
        if (!metadata || !input) return Fail(error, "invalid layout: metadata or input is absent");
        const auto containingBytes = [&](std::size_t start) {
            std::size_t end = total;
            for (const auto next : starts) if (next > start) end = (std::min)(end, next);
            return end - start;
        };
        const auto metadataLimit = containingBytes(metadata);
        const auto inputLimit = containingBytes(input);
        if (metadataLimit < MetadataPrefixSize || inputLimit < InputHeaderSize) {
            return Fail(error, "invalid layout: a containing region is too short");
        }
        const auto* const m = base + metadata;
        const std::size_t metadataSize = Read<std::uint32_t>(m, 0);
        if (metadataSize < MetadataPrefixSize || metadataSize > metadataLimit) {
            return Fail(error, "invalid metadata: infoSize does not fit its containing region");
        }
        metadataCopy.assign(m, m + metadataSize);
        Observe("metadata-copied", base, 0);
        const auto* const copy = metadataCopy.data();
        if (Read<std::uint32_t>(copy, 0) != metadataSize) {
            return Fail(error, "shared mutation invariant: infoSize changed during creation");
        }
        info.vendor = Read<std::uint16_t>(copy, 4);
        info.product = Read<std::uint16_t>(copy, 6);
        std::memcpy(info.id.data(), copy + 0x20, info.id.size());
        std::memcpy(info.root.data(), copy + 0x40, info.root.size());
        info.supportedInput = Read<std::uint32_t>(copy, 0x68);
        const std::size_t count = Read<std::uint32_t>(copy, 0x70);
        const auto descriptorOffset = Read<std::uint64_t>(copy, 0xA0);
        if ((count != 0 && descriptorOffset < MetadataPrefixSize) ||
            descriptorOffset > metadataSize ||
            count > (metadataSize - static_cast<std::size_t>(descriptorOffset)) / DescriptorSize) {
            return Fail(error, "invalid metadata: input descriptor array is outside infoSize");
        }
        if (((info.supportedInput & 1) != 0) != (count != 0)) {
            return Fail(error, "invalid metadata: raw support and input descriptor count disagree");
        }
        info.reports.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto offset = static_cast<std::size_t>(descriptorOffset) + i * DescriptorSize;
            ReportDescriptor descriptor{Read<std::uint32_t>(copy, offset),
                Read<std::uint32_t>(copy, offset + 4), Read<std::uint32_t>(copy, offset + 8)};
            const std::size_t itemCount = Read<std::uint32_t>(copy, offset + 0xC);
            const auto items = Read<std::uint64_t>(copy, offset + 0x10);
            if (descriptor.kind != 0 || (itemCount != 0 && items < MetadataPrefixSize) ||
                items > metadataSize ||
                itemCount > (metadataSize - static_cast<std::size_t>(items)) / ReportItemSize) {
                return Fail(error, "invalid metadata: input kind or item-array bounds");
            }
            info.reports.push_back(descriptor);
        }

        std::memcpy(inputCopy.data(), base + input, inputCopy.size());
        const auto* const in = inputCopy.data();
        const std::size_t inputSize = Read<std::uint32_t>(in, 0);
        if (inputSize < InputHeaderSize || inputSize > inputLimit) {
            return Fail(error, "invalid layout: input size exceeds its containing region");
        }
        const std::size_t relativeLayout = Read<std::uint32_t>(in, 4);
        const std::size_t relativeLatest = Read<std::uint32_t>(in, 8);
        const std::size_t relativeBitmap = Read<std::uint32_t>(in, 0xC);
        const std::size_t relativeEntries = Read<std::uint32_t>(in, 0x10);
        slotCount = Read<std::uint32_t>(in, 0x18);
        stride = Read<std::uint32_t>(in, 0x1C);
        const std::size_t relativeData = Read<std::uint32_t>(in, 0x20);
        if (slotCount < 2 || slotCount > MaxSlots || stride < 0x20 || (stride & 7) != 0 ||
            slotCount > inputSize / stride) {
            return Fail(error, "invalid layout: slot count, stride, or slot-array size");
        }
        std::vector<Range> ranges{{0, InputHeaderSize}, {relativeLayout, ReadingLayoutSize},
            {relativeLatest, 8}, {relativeBitmap, ((slotCount + 63) / 64) * 8},
            {relativeEntries, slotCount * 16}, {relativeData, slotCount * stride}};
        // Reserve the optional regions to prevent an atomic target aliasing them.
        const std::array<Range, 3> optional{
            Range{Read<std::uint32_t>(in, 0x14), 0xC08},
            Range{Read<std::uint32_t>(in, 0x24), 0x810},
            Range{Read<std::uint32_t>(in, 0x28), 4}};
        for (const auto region : optional) if (region.offset) ranges.push_back(region);
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            if ((i != 0 && ranges[i].offset < InputHeaderSize) ||
                !Fits(ranges[i].offset, ranges[i].length, inputSize)) {
                return Fail(error, "invalid layout: input span is outside its containing layout");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (!Separate(ranges[i], ranges[j])) {
                    return Fail(error, "invalid layout: input regions overlap");
                }
            }
        }
        // Widen before adding DWORD offsets. Every atomic target is now bounded.
        layout = input + relativeLayout;
        latest = input + relativeLatest;
        bitmap = input + relativeBitmap;
        entries = input + relativeEntries;
        data = input + relativeData;
        const auto relativePolicy = Read<std::uint32_t>(in, 0x28);
        if (relativePolicy) {
            inputPolicy = input + relativePolicy;
            if ((inputPolicy & 3) != 0) return Fail(error, "invalid layout: input policy is not aligned");
        }
        if (((latest | bitmap | entries | data) & 7) != 0) {
            return Fail(error, "invalid layout: pool fields are not eight-byte aligned");
        }
        std::memcpy(layoutCopy.data(), base + layout, layoutCopy.size());
        Observe("layout-copied", base, 0);
        const auto* const l = layoutCopy.data();
        const std::size_t readingBytes = Read<std::uint32_t>(l, 0);
        rawHeader = Read<std::uint32_t>(l, 4);
        rawCapacity = Read<std::uint16_t>(l, 8);
        rawPayload = Read<std::uint32_t>(l, 0x14);
        rawSequence = Read<std::uint8_t>(l, 0x84);
        if (readingBytes < 0x20 || readingBytes > stride || stride - readingBytes > 7) {
            return Fail(error, "invalid layout: reading size does not fit its slot stride");
        }
        if ((info.supportedInput & 1) != 0) {
            if (rawHeader < 0x20 || rawPayload < 0x20 ||
                !Fits(rawHeader, 8, readingBytes) || !Fits(rawPayload, rawCapacity, readingBytes) ||
                !Separate({rawHeader, 8}, {rawPayload, rawCapacity}) ||
                (rawSequence != 0 && (rawSequence < 0x20 ||
                    !Fits(rawSequence, 8, readingBytes) ||
                    !Separate({rawSequence, 8}, {rawHeader, 8}) ||
                    !Separate({rawSequence, 8}, {rawPayload, rawCapacity})))) {
                return Fail(error, "invalid layout: raw reading spans are invalid or overlapping");
            }
            for (const auto& descriptor : info.reports) {
                if (descriptor.maxSize > rawCapacity) {
                    return Fail(error, "invalid metadata: report maximum exceeds raw capacity");
                }
            }
        } else if (rawHeader || rawPayload || rawCapacity || rawSequence) {
            return Fail(error, "invalid layout: raw fields exist without raw input support");
        }
        Observe("created", base, 0);
        return Immutable(error);
    }

    bool Immutable(std::string& error) const {
        if (std::memcmp(base, headerCopy.data(), headerCopy.size()) != 0 ||
            std::memcmp(base + input, inputCopy.data(), inputCopy.size()) != 0 ||
            std::memcmp(base + layout, layoutCopy.data(), layoutCopy.size()) != 0 ||
            std::memcmp(base + metadata, metadataCopy.data(), metadataCopy.size()) != 0) {
            return Fail(error, "shared mutation invariant: layout or metadata changed");
        }
        return true;
    }

    std::uint8_t* Entry(std::uint64_t id) const {
        return base + entries + static_cast<std::size_t>(id & 0xFFFF) * 16;
    }

    bool Release(std::uint64_t id) noexcept {
        auto* const entry = Entry(id);
        Observe("before-release", base, id);
        const auto check = Load64(entry);
        if ((check >> 16) != (id >> 16) || (check & 0xFFFF) == 0) {
            releaseBroken = true;
            return false;
        }
        const auto before = std::bit_cast<std::uint64_t>(
            _InterlockedExchangeAdd64(reinterpret_cast<volatile __int64*>(entry), -1));
        if ((before >> 16) != (id >> 16) || (before & 0xFFFF) == 0) {
            releaseBroken = true;
            return false;
        }
        if ((before & 0xFFFF) == 1) {
            Store64(entry + 8, 0);
            _mm_mfence();
            Observe("cleared-links", base, id);
            Store64(entry, 0);
            _mm_mfence();
            Observe("cleared-reference", base, id);
            const std::size_t slot = static_cast<std::size_t>(id & 0xFFFF);
            _InterlockedOr64(reinterpret_cast<volatile __int64*>(base + bitmap + (slot / 64) * 8),
                std::bit_cast<__int64>(1ULL << (slot % 64)));
            Observe("freed", base, id);
        }
        return true;
    }

    struct Reference {
        Impl& pool;
        std::uint64_t id = 0;
        std::uint64_t flags = 0;
        explicit Reference(Impl& owner) : pool(owner) {}
        ~Reference() { Reset(); }
        Reference(const Reference&) = delete;
        Reference& operator=(const Reference&) = delete;
        void Reset() noexcept {
            if (id) pool.Release(std::exchange(id, 0));
        }
        void Take(Reference& other) noexcept {
            id = std::exchange(other.id, 0);
            flags = other.flags;
        }
    };

    enum class PinResult { Pinned, Stale, Zero, Overflow, Contention };

    PinResult Pin(std::uint64_t id, Reference& ref) {
        auto* const entry = Entry(id);
        auto value = Load64(entry);
        for (unsigned attempt = 0; attempt < MaxPinAttempts; ++attempt) {
            diagnostics.referenceWord = value;
            if ((value >> 16) != (id >> 16)) return PinResult::Stale;
            const auto count = value & 0xFFFF;
            if (!count) return PinResult::Zero;
            if (count == 0xFFFF) return PinResult::Overflow;
            Observe("before-cas", base, id);
            const auto actual = std::bit_cast<std::uint64_t>(_InterlockedCompareExchange64(
                reinterpret_cast<volatile __int64*>(entry),
                std::bit_cast<__int64>(value + 1), std::bit_cast<__int64>(value)));
            if (actual == value) {
                ref.id = id;
                Observe("pinned", base, id);
                ref.flags = Load64(entry + 8);
                return PinResult::Pinned;
            }
            value = actual;
        }
        return PinResult::Contention;
    }

    bool ValidId(std::uint64_t id, std::string& error) const {
        if ((id >> 16) == 0 || (id & 0xFFFF) >= slotCount) {
            return Fail(error, "shared mutation invariant: invalid generation or slot index");
        }
        return true;
    }

    bool LinksValid(const Reference& ref, std::string& error) const {
        const auto generation = ref.id >> 16;
        const auto slot = ref.id & 0xFFFF;
        const auto previous = (ref.flags >> 32) & 0xFFFF;
        const auto next = ref.flags >> 48;
        if (((ref.flags & PreviousBit) && (generation <= 1 || previous >= slotCount || previous == slot)) ||
            ((ref.flags & NextBit) && (generation == MaxGeneration || next >= slotCount || next == slot))) {
            return Fail(error, "shared mutation invariant: invalid history link");
        }
        return true;
    }

    bool Decode(const Reference& ref, std::vector<RawReading>& readings, std::string& error) {
        const auto* const reading = base + data + static_cast<std::size_t>(ref.id & 0xFFFF) * stride;
        diagnostics.sampleId = ref.id;
        diagnostics.flags = ref.flags;
        diagnostics.readingKinds = Read<std::uint32_t>(reading, 0xC);
        diagnostics.timestamp = Read<std::uint64_t>(reading, 0);
        diagnostics.rawSequence = rawSequence ? Read<std::uint64_t>(reading, rawSequence) : 0;
        if ((ref.flags & 1) == 0) { ++diagnostics.nonraw; return true; }
        ++diagnostics.raw;
        if ((info.supportedInput & 1) == 0 || (Read<std::uint32_t>(reading, 0xC) & 1) == 0) {
            return Fail(error, "shared mutation invariant: published raw kind has no raw data");
        }
        const std::size_t index = Read<std::uint32_t>(reading, rawHeader);
        const std::size_t length = Read<std::uint32_t>(reading, rawHeader + 4);
        if (index >= info.reports.size()) {
            return Fail(error, "shared mutation invariant: raw descriptor index is out of range");
        }
        const auto& descriptor = info.reports[index];
        if (length > descriptor.maxSize || length > rawCapacity) {
            return Fail(error, "shared mutation invariant: raw byte count exceeds its bounds");
        }
        RawReading result;
        result.generation = ref.id >> 16;
        result.timestamp = Read<std::uint64_t>(reading, 0);
        result.rawSequence = rawSequence ? Read<std::uint64_t>(reading, rawSequence) : 0;
        result.reportId = descriptor.id;
        result.bytes.assign(reading + rawPayload, reading + rawPayload + length);
        readings.push_back(std::move(result));
        return true;
    }

    bool Collect(std::vector<RawReading>& readings, std::uint64_t& observedId, std::string& error) {
        if (!Immutable(error)) return false;
        observedId = Load64(base + latest);
        diagnostics.latest = observedId;
        if (!observedId) {
            ++diagnostics.empty;
            return lastId ? Fail(error, "shared mutation invariant: latest ID became empty") : true;
        }
        if (!ValidId(observedId, error)) return false;
        const auto lastGeneration = lastId >> 16;
        if ((observedId >> 16) < lastGeneration ||
            ((observedId >> 16) == lastGeneration && observedId != lastId)) {
            return Fail(error, "shared mutation invariant: latest generation regressed or changed slots");
        }
        if (observedId == lastId) { ++diagnostics.unchanged; return true; }
        Reference current(*this);
        const auto pinned = Pin(observedId, current);
        diagnostics.latestPin = static_cast<std::uint32_t>(pinned) + 1;
        if (pinned != PinResult::Pinned) {
            if (pinned == PinResult::Overflow) return Fail(error, "reference overflow: latest count is 65535");
            if (pinned == PinResult::Zero) return Fail(error, "retry: latest reference count is zero");
            if (pinned == PinResult::Stale) return Fail(error, "retry: latest generation is stale");
            return Fail(error, "retry: latest reference contention");
        }
        if ((current.flags & HistoryBit) == 0) {
            return Fail(error, "retry: latest reading left retained history");
        }
        std::array<bool, MaxSlots> visited{};
        std::uint64_t oldest = observedId >> 16;
        const char* gapReason = "history is no longer retained";
        for (std::size_t visitedCount = 0; visitedCount < slotCount; ++visitedCount) {
            const auto slot = static_cast<std::size_t>(current.id & 0xFFFF);
            if (visited[slot]) return Fail(error, "shared mutation invariant: history revisited a slot");
            visited[slot] = true;
            if (!LinksValid(current, error) || !Decode(current, readings, error)) return false;
            oldest = current.id >> 16;
            if (oldest == lastGeneration + 1 || (current.flags & PreviousBit) == 0) break;
            if (visitedCount + 1 == slotCount) {
                gapReason = "history traversal reached the slot-count limit";
                break;
            }
            const auto previousId = ((oldest - 1) << 16) | ((current.flags >> 32) & 0xFFFF);
            Reference previous(*this);
            const auto previousResult = Pin(previousId, previous);
            if (previousResult == PinResult::Overflow) {
                return Fail(error, "reference overflow: previous count is 65535");
            }
            if (previousResult != PinResult::Pinned || (previous.flags & HistoryBit) == 0) break;
            current.Reset();
            if (releaseBroken) return false;
            current.Take(previous);
        }
        current.Reset();
        if (releaseBroken || !Immutable(error)) return false;
        if (oldest > lastGeneration + 1) {
            error = "gap: generations " + std::to_string(lastGeneration + 1) + ".." +
                std::to_string(oldest - 1) + " unavailable (" + gapReason + ")";
        }
        std::reverse(readings.begin(), readings.end());
        return true;
    }
};

SharedReader::SharedReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<SharedReader> SharedReader::Create(void* base, std::size_t logicalBytes, std::string& error) {
    error.clear();
    try {
        auto impl = std::make_unique<Impl>();
        if (!impl->Initialize(base, logicalBytes, error)) return nullptr;
        return std::unique_ptr<SharedReader>(new SharedReader(std::move(impl)));
    } catch (const std::exception&) {
        error = "reader creation failed: allocation or copy could not complete";
        return nullptr;
    }
}

const DeviceInfo& SharedReader::Info() const { return impl_->info; }
const ReaderDiagnostics& SharedReader::Diagnostics() const { return impl_->diagnostics; }

bool SharedReader::ReadInputPolicy(std::uint32_t& value, std::string& error) const {
    error.clear();
    value = 0;
    if (!impl_->base) return Fail(error, "reader is retired");
    if (!impl_->inputPolicy) return Fail(error, "input policy is absent");
    if (!impl_->Immutable(error)) return false;
    value = *reinterpret_cast<const volatile std::uint32_t*>(impl_->base + impl_->inputPolicy);
    return true;
}

bool SharedReader::UseDefaultInputPolicy(std::uint32_t& previous, bool& changed, std::string& error) {
    changed = false;
    if (!ReadInputPolicy(previous, error)) return false;
    if (previous != 0) {
        *reinterpret_cast<volatile std::uint32_t*>(impl_->base + impl_->inputPolicy) = 0;
        _mm_mfence();
        changed = true;
    }
    return true;
}

bool SharedReader::Poll(std::vector<RawReading>& out, std::string& error) {
    out.clear();
    error.clear();
    if (!impl_->base) return Fail(error, "reader is retired");
    impl_->releaseBroken = false;
    ++impl_->diagnostics.polls;
    try {
        std::vector<RawReading> readings;
        std::uint64_t observedId = 0;
        const bool success = impl_->Collect(readings, observedId, error);
        if (impl_->releaseBroken) return Fail(error, "shared mutation invariant: reference ownership was lost");
        if (!success) return false;
        impl_->lastId = observedId;
        impl_->diagnostics.cursor = observedId;
        out.swap(readings);
        return true;
    } catch (const std::exception&) {
        return Fail(error, impl_->releaseBroken ? "shared mutation invariant: reference ownership was lost"
                                             : "poll failed: allocation or copy could not complete");
    }
}

void SharedReader::Retire() { impl_->base = nullptr; }
SharedReader::~SharedReader() { Retire(); }

} // namespace sdl_paddles
