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
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "../src/joystick/windows/SDL_xinput_paddle_reader.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using sdl_paddles::RawReading;
using sdl_paddles::SharedReader;
constexpr std::size_t M = 0x80;
constexpr std::size_t I = 0x400;
constexpr std::size_t L = I + 0x2C;
constexpr std::size_t Latest = I + 0xD8;
constexpr std::size_t Bitmap = I + 0xE0;
constexpr std::size_t Entries = I + 0x120;
constexpr std::size_t Data = I + 0x2120;
constexpr std::size_t Stride = 0x50;
constexpr std::uint64_t History = 1ULL << 29;
constexpr std::uint64_t Previous = 1ULL << 30;
constexpr std::uint64_t Next = 1ULL << 31;
const std::array<std::uint8_t, 17> Captured{
    0x10, 0, 0, 0, 0, 0, 0x7B, 0xFE, 0xAE, 0xFD, 0x5F, 0xFF, 0x5A, 0xFC, 0x0F, 0, 0};

#define CHECK(expression) do { if (!(expression)) throw std::runtime_error( \
    std::string(__func__) + ": " + #expression + " at line " + std::to_string(__LINE__)); } while (false)

std::function<void(std::string_view, void*, std::uint64_t)> hook;
std::string hookFailure;

void HookCheck(bool condition, const char* message) {
    if (!condition && hookFailure.empty()) hookFailure = message;
}

std::uint64_t Id(std::uint64_t generation, std::size_t slot) {
    return (generation << 16) | slot;
}

struct Fixture {
    std::size_t count;
    std::size_t feedback;
    std::size_t bytes;
    std::vector<std::uint64_t> storage;

    explicit Fixture(std::size_t slots = 4) : count(slots),
        feedback((Data + slots * Stride + 4 + 7) & ~std::size_t{7}),
        bytes(feedback + 0x80), storage(bytes / 8, 0) {
        Put<std::uint32_t>(0, static_cast<std::uint32_t>(bytes));
        Put<std::uint32_t>(4, 0x28);
        Put<std::uint32_t>(8, static_cast<std::uint32_t>(M));
        Put<std::uint32_t>(0xC, 0x280);
        Put<std::uint32_t>(0x10, static_cast<std::uint32_t>(I));
        Put<std::uint32_t>(0x18, static_cast<std::uint32_t>(feedback));
        Put<std::uint32_t>(M, 0x200);
        Put<std::uint16_t>(M + 4, 0x045E);
        Put<std::uint16_t>(M + 6, 0x0B00);
        for (std::size_t n = 0; n < 32; ++n) {
            Put<std::uint8_t>(M + 0x20 + n, static_cast<std::uint8_t>(n + 1));
            Put<std::uint8_t>(M + 0x40 + n, static_cast<std::uint8_t>(0xE0 + n));
        }
        Put<std::uint32_t>(M + 0x68, 1);
        Put<std::uint32_t>(M + 0x70, 2);
        Put<std::uint64_t>(M + 0xA0, 0x148);
        Put<std::uint32_t>(M + 0x148 + 4, 0xF1234567);
        Put<std::uint32_t>(M + 0x148 + 8, 32);
        Put<std::uint32_t>(M + 0x160 + 4, 0x0C);
        Put<std::uint32_t>(M + 0x160 + 8, 17);
        Put<std::uint64_t>(M + 0x128, 0x1E0);
        Put<std::uint64_t>(M + 0x140, 0xA1B2C3D4000001F0ULL);
        Put<std::uint32_t>(I, static_cast<std::uint32_t>(Data + count * Stride + 4 - I));
        Put<std::uint32_t>(I + 4, 0x2C);
        Put<std::uint32_t>(I + 8, 0xD8);
        Put<std::uint32_t>(I + 0xC, 0xE0);
        Put<std::uint32_t>(I + 0x10, 0x120);
        Put<std::uint32_t>(I + 0x18, static_cast<std::uint32_t>(count));
        Put<std::uint32_t>(I + 0x1C, static_cast<std::uint32_t>(Stride));
        Put<std::uint32_t>(I + 0x20, 0x2120);
        Put<std::uint32_t>(I + 0x28, static_cast<std::uint32_t>(Data + count * Stride - I));
        Put<std::uint32_t>(L, static_cast<std::uint32_t>(Stride));
        Put<std::uint32_t>(L + 4, 0x28);
        Put<std::uint16_t>(L + 8, 32);
        Put<std::uint32_t>(L + 0x14, 0x30);
        Put<std::uint8_t>(L + 0x84, 0x20);
        std::memset(Base() + feedback, 0xA5, 0x80);
    }

    std::uint8_t* Base() { return reinterpret_cast<std::uint8_t*>(storage.data()); }

    template<class T> void Put(std::size_t offset, T value) {
        CHECK(offset <= bytes && sizeof(value) <= bytes - offset);
        std::memcpy(Base() + offset, &value, sizeof(value));
    }

    template<class T> T Get(std::size_t offset) {
        CHECK(offset <= bytes && sizeof(T) <= bytes - offset);
        T value{};
        std::memcpy(&value, Base() + offset, sizeof(value));
        return value;
    }

    void Publish(std::size_t slot, std::uint64_t generation, int previous = -1,
        bool raw = true, std::uint32_t descriptor = 1) {
        CHECK(slot < count);
        const auto e = Entries + slot * 16;
        const auto r = Data + slot * Stride;
        std::memset(Base() + r, 0, Stride);
        Put<std::uint64_t>(r, 123456000 + generation);
        Put<std::uint32_t>(r + 0xC, 1);
        Put<std::uint64_t>(r + 0x20, 700 + generation);
        Put<std::uint32_t>(r + 0x28, descriptor);
        Put<std::uint32_t>(r + 0x2C, static_cast<std::uint32_t>(Captured.size()));
        std::memcpy(Base() + r + 0x30, Captured.data(), Captured.size());
        Put<std::uint64_t>(e, Id(generation, 1));
        std::uint64_t flags = History | (raw ? 1ULL : 0ULL);
        if (previous >= 0) {
            const auto prevSlot = static_cast<std::size_t>(previous);
            CHECK(prevSlot < count);
            flags |= Previous | (static_cast<std::uint64_t>(prevSlot) << 32);
            const auto prev = Entries + prevSlot * 16 + 8;
            Put<std::uint64_t>(prev, Get<std::uint64_t>(prev) | Next | (static_cast<std::uint64_t>(slot) << 48));
        }
        Put<std::uint64_t>(e + 8, flags);
        Put<std::uint64_t>(Latest, Id(generation, slot));
    }

    std::unique_ptr<SharedReader> Reader() {
        std::string error;
        auto reader = SharedReader::Create(Base(), bytes, error);
        CHECK(reader && error.empty());
        return reader;
    }

    void Rejected() {
        const auto before = storage;
        std::string error;
        CHECK(!SharedReader::Create(Base(), bytes, error));
        CHECK(!error.empty());
        CHECK(storage == before);
    }

    void ProducerDrops(std::uint64_t id) {
        const auto e = Entries + static_cast<std::size_t>(id & 0xFFFF) * 16;
        auto* words = reinterpret_cast<volatile LONG64*>(Base() + e);
        InterlockedAnd64(words + 1, std::bit_cast<LONG64>(~(History | Previous)));
        const auto prior = static_cast<std::uint64_t>(InterlockedExchangeAdd64(words, -1));
        HookCheck((prior & 0xFFFF) == 2, "producer did not observe its own reference plus the reader reference");
    }
};

void MetadataAndFullRawPayload() {
    Fixture f;
    f.Publish(2, 1);
    const auto before = f.storage;
    auto reader = f.Reader();
    const auto& info = reader->Info();
    CHECK(info.vendor == 0x045E && info.product == 0x0B00 && info.supportedInput == 1);
    CHECK(info.reports.size() == 2 && info.reports[1].kind == 0);
    CHECK(info.reports[1].id == 0x0C && info.reports[1].maxSize == 17);
    for (std::size_t n = 0; n < 32; ++n) {
        CHECK(info.id[n] == n + 1 && info.root[n] == 0xE0 + n);
    }
    bool sawPin = false;
    hook = [&](std::string_view stage, void*, std::uint64_t id) {
        if (stage == "pinned") {
            sawPin = true;
            HookCheck(f.Get<std::uint64_t>(Entries + 32) == Id(1, 2), "CAS did not increment exactly one reference");
            HookCheck(id == Id(1, 2), "slot ID changed while pinning");
        }
    };
    std::vector<RawReading> out;
    std::string error = "old error";
    CHECK(reader->Poll(out, error) && error.empty());
    CHECK(sawPin && hookFailure.empty());
    CHECK(out.size() == 1 && out[0].generation == 1 && out[0].timestamp == 123456001);
    CHECK(out[0].rawSequence == 701 && out[0].reportId == 0x0C);
    CHECK(out[0].bytes == std::vector<std::uint8_t>(Captured.begin(), Captured.end()));
    CHECK(f.storage == before);
    f.Put<std::uint8_t>(Data + 2 * Stride + 0x30, 0xFF);
    CHECK(out[0].bytes[0] == 0x10);
    CHECK(reader->Poll(out, error) && out.empty() && error.empty());
}

void UnknownReportAndGenericIdentity() {
    Fixture f;
    f.Put<std::uint16_t>(M + 4, 0x1234);
    f.Put<std::uint16_t>(M + 6, 0xABCD);
    f.Publish(0, 1, -1, true, 0);
    auto reader = f.Reader();
    CHECK(reader->Info().vendor == 0x1234 && reader->Info().product == 0xABCD);
    std::vector<RawReading> out;
    std::string error;
    CHECK(reader->Poll(out, error) && out.size() == 1 && out[0].reportId == 0xF1234567);
}

void LogicalViewBounds() {
    Fixture f;
    std::string error;
    CHECK(!SharedReader::Create(nullptr, f.bytes, error));
    CHECK(!SharedReader::Create(f.Base(), 0x23, error));
    CHECK(!SharedReader::Create(f.Base() + 1, f.bytes - 1, error));
    CHECK(!SharedReader::Create(f.Base(), f.bytes - 1, error));
    CHECK(!SharedReader::Create(f.Base(), std::numeric_limits<std::size_t>::max(), error));
    f.Put<std::uint32_t>(0, 0x23);
    f.Rejected();
}

void LayoutBoundsAndAlignment() {
    const std::vector<std::pair<std::size_t, std::uint32_t>> invalid{
        {8, 0}, {8, 0x20}, {0x10, 0}, {0x10, 0xFFFFFFF8}, {0x18, static_cast<std::uint32_t>(I)},
        {M, 0x140}, {M, 0x201}, {I, 0xFFFFFFFF}, {I + 4, 0xFFFFFFF0},
        {I + 8, 0xDA}, {I + 0xC, 0xE4}, {I + 0x10, 0x124},
        {I + 0x18, 0}, {I + 0x18, 1}, {I + 0x18, 513},
        {I + 0x1C, 0x51}, {I + 0x1C, 0xFFFFFFF8}, {I + 0x20, 0xFFFFFFF8},
        {L, 0x51}, {L, 0x10}, {L, 0x48}, {L + 4, 0xFFFFFFFC},
        {L + 4, 0xC}, {L + 4, 0x30}, {L + 0x14, 0xFFFFFFFF}, {L + 0x14, 0x48}};
    for (const auto [offset, value] : invalid) {
        Fixture f;
        f.Put<std::uint32_t>(offset, value);
        f.Rejected();
    }
    Fixture f;
    f.Put<std::uint8_t>(L + 0x84, 0x4C);
    f.Rejected();
}

void OverlapAndContainingLayoutBounds() {
    for (const auto target : {0U, 0x2CU, 0xD8U, 0x2120U}) {
        Fixture f;
        f.Put<std::uint32_t>(I + 0x10, target);
        f.Rejected();
    }
    Fixture f;
    f.Put<std::uint32_t>(I + 0x20, static_cast<std::uint32_t>(f.feedback - I));
    f.Rejected();
    Fixture g;
    g.Put<std::uint32_t>(I + 8, 0x300);
    g.Put<std::uint32_t>(I + 0x24, 0x300);
    g.Rejected();
    Fixture h;
    h.Put<std::uint32_t>(I + 0x14, 0x300);
    h.Put<std::uint32_t>(I + 0x10, 0x300);
    h.Rejected();
    Fixture i;
    i.Put<std::uint32_t>(I + 0x28, 0xD8);
    i.Rejected();
    Fixture j;
    j.Put<std::uint32_t>(0xC, static_cast<std::uint32_t>(M + 0x160));
    j.Rejected();
}

void MetadataBounds() {
    for (const auto offset : {0ULL, 0x140ULL, 0x1F0ULL, 0xFFFFFFFFULL, 0x100000148ULL,
        std::numeric_limits<std::uint64_t>::max()}) {
        Fixture f;
        f.Put<std::uint64_t>(M + 0xA0, offset);
        f.Rejected();
    }
    Fixture f;
    f.Put<std::uint32_t>(M + 0x70, 0xFFFFFFFF);
    f.Rejected();
    Fixture g;
    g.Put<std::uint32_t>(M + 0x148, 1);
    g.Rejected();
    Fixture h;
    h.Put<std::uint32_t>(M + 0x148 + 8, 33);
    h.Rejected();
    Fixture i;
    i.Put<std::uint32_t>(M + 0x148 + 0xC, 1);
    i.Put<std::uint64_t>(M + 0x148 + 0x10, 0x1F0);
    i.Rejected();
    Fixture j;
    j.Put<std::uint32_t>(M + 0x68, 0);
    j.Rejected();
}

void StaleZeroOverflowAndInvalidIds() {
    for (const auto reference : {Id(2, 1), Id(1, 0), Id(1, 0xFFFF)}) {
        Fixture f;
        f.Publish(0, 1);
        auto reader = f.Reader();
        f.Put<std::uint64_t>(Entries, reference);
        const auto before = f.storage;
        std::string error;
        std::vector<RawReading> out(1);
        CHECK(!reader->Poll(out, error) && !error.empty() && out.empty());
        CHECK(error.starts_with("retry:") == (reference != Id(1, 0xFFFF)));
        CHECK(f.storage == before);
        f.Put<std::uint64_t>(Entries, Id(1, 1));
        CHECK(reader->Poll(out, error) && out.size() == 1);
    }
    for (const auto id : {std::uint64_t{1}, Id(1, 4)}) {
        Fixture f;
        f.Put<std::uint64_t>(Latest, id);
        auto reader = f.Reader();
        const auto before = f.storage;
        std::string error;
        std::vector<RawReading> out;
        CHECK(!reader->Poll(out, error) && !error.empty());
        CHECK(f.storage == before);
    }
    Fixture f;
    f.Publish(0, 1);
    f.Put<std::uint64_t>(Entries, Id(1, 0xFFFE));
    auto reader = f.Reader();
    const auto before = f.storage;
    std::string error;
    std::vector<RawReading> out;
    CHECK(reader->Poll(out, error) && out.size() == 1 && f.storage == before);
}

void CasRetriesAgainstChangedReference() {
    Fixture f;
    f.Publish(0, 1);
    auto reader = f.Reader();
    unsigned tries = 0;
    hook = [&](std::string_view stage, void*, std::uint64_t) {
        if (stage == "before-cas" && ++tries == 1) f.Put<std::uint64_t>(Entries, Id(1, 2));
        if (stage == "pinned") HookCheck(f.Get<std::uint64_t>(Entries) == Id(1, 3), "CAS overwrote another reference");
    };
    std::vector<RawReading> out;
    std::string error;
    CHECK(reader->Poll(out, error) && out.size() == 1);
    CHECK(tries == 2 && f.Get<std::uint64_t>(Entries) == Id(1, 2) && hookFailure.empty());
}

void CasRetryRejectsGenerationChangeAndIsBounded() {
    Fixture f;
    f.Publish(0, 1);
    auto reader = f.Reader();
    hook = [&](std::string_view stage, void*, std::uint64_t) {
        if (stage == "before-cas") f.Put<std::uint64_t>(Entries, Id(2, 1));
    };
    std::vector<RawReading> out;
    std::string error;
    CHECK(!reader->Poll(out, error) && out.empty() && error.starts_with("retry:"));
    CHECK(f.Get<std::uint64_t>(Entries) == Id(2, 1));
    f.Put<std::uint64_t>(Entries, Id(1, 1));
    unsigned tries = 0;
    hook = [&](std::string_view stage, void*, std::uint64_t) {
        if (stage == "before-cas") {
            ++tries;
            f.Put<std::uint64_t>(Entries, Id(1, 1 + tries));
        }
    };
    CHECK(!reader->Poll(out, error) && error.find("contention") != std::string::npos);
    CHECK(error.starts_with("retry:"));
    CHECK(tries > 1 && tries <= 512 && f.Get<std::uint64_t>(Entries) == Id(1, 1 + tries));
}

void LastReferenceReleaseAndBitmap() {
    Fixture f(512);
    f.Publish(130, 1);
    const auto e = Entries + 130 * 16;
    const auto word = Bitmap + 2 * 8;
    f.Put<std::uint64_t>(word, 1ULL << 9);
    auto reader = f.Reader();
    std::vector<std::string> stages;
    hook = [&](std::string_view stage, void*, std::uint64_t id) {
        if (stage == "before-release") f.ProducerDrops(id);
        if (stage == "cleared-links") {
            stages.emplace_back(stage);
            HookCheck(f.Get<std::uint64_t>(e + 8) == 0, "links were not cleared first");
            HookCheck(f.Get<std::uint64_t>(e) == Id(1, 0), "generation was cleared before the first fence");
            HookCheck(f.Get<std::uint64_t>(word) == (1ULL << 9), "free bit became visible too soon");
        }
        if (stage == "cleared-reference") {
            stages.emplace_back(stage);
            HookCheck(f.Get<std::uint64_t>(e) == 0, "reference was not cleared");
            HookCheck(f.Get<std::uint64_t>(word) == (1ULL << 9), "free bit became visible before the second fence");
        }
        if (stage == "freed") stages.emplace_back(stage);
    };
    std::vector<RawReading> out;
    std::string error;
    CHECK(reader->Poll(out, error) && out.size() == 1 && error.empty());
    CHECK(hookFailure.empty());
    CHECK(stages == std::vector<std::string>({"cleared-links", "cleared-reference", "freed"}));
    CHECK(f.Get<std::uint64_t>(e) == 0 && f.Get<std::uint64_t>(e + 8) == 0);
    CHECK(f.Get<std::uint64_t>(word) == ((1ULL << 9) | (1ULL << 2)));
    CHECK(f.Get<std::uint64_t>(Bitmap) == 0 && f.Get<std::uint64_t>(Bitmap + 8) == 0);
}

void HistoryChronologyAndTwoPins() {
    Fixture f;
    f.Publish(3, 1);
    f.Publish(1, 2, 3);
    f.Publish(2, 3, 1);
    auto reader = f.Reader();
    bool overlap = false;
    unsigned maximum = 0;
    hook = [&](std::string_view stage, void*, std::uint64_t id) {
        if (stage != "pinned") return;
        unsigned pins = 0;
        for (const auto slot : {1, 2, 3}) {
            if ((f.Get<std::uint64_t>(Entries + static_cast<std::size_t>(slot) * 16) & 0xFFFF) == 2) ++pins;
        }
        maximum = (std::max)(maximum, pins);
        if ((id >> 16) < 3) overlap = overlap || pins == 2;
    };
    const auto before = f.storage;
    std::vector<RawReading> out;
    std::string error;
    CHECK(reader->Poll(out, error) && error.empty() && out.size() == 3);
    CHECK(overlap && maximum == 2 && f.storage == before);
    for (std::size_t n = 0; n < out.size(); ++n) CHECK(out[n].generation == n + 1);
    CHECK(reader->Poll(out, error) && out.empty());
    f.Publish(0, 4, 2);
    CHECK(reader->Poll(out, error) && out.size() == 1 && out[0].generation == 4);
}

void RetainedHistoryGapAndContinuation() {
    Fixture f;
    f.Publish(0, 1);
    auto reader = f.Reader();
    std::vector<RawReading> out;
    std::string error;
    CHECK(reader->Poll(out, error) && out.size() == 1);
    f.Publish(1, 5);
    f.Publish(2, 6, 1);
    CHECK(reader->Poll(out, error) && out.size() == 2);
    CHECK(error.find("generations 2..4") != std::string::npos);
    CHECK(out[0].generation == 5 && out[1].generation == 6);
    CHECK(reader->Poll(out, error) && out.empty() && error.empty());
    f.Publish(3, 7, 2);
    CHECK(reader->Poll(out, error) && out.size() == 1 && out[0].generation == 7 && error.empty());
}

void MissingPredecessorAndPostPinHistoryFlag() {
    for (const unsigned mode : {0U, 1U, 2U}) {
        Fixture f;
        f.Publish(1, 4);
        f.Publish(2, 5, 1);
        auto reader = f.Reader();
        if (mode == 0) f.Put<std::uint64_t>(Entries + 16, Id(3, 1));
        if (mode == 1) f.Put<std::uint64_t>(Entries + 16, Id(4, 0));
        if (mode == 2) {
            hook = [&](std::string_view stage, void*, std::uint64_t id) {
                if (stage == "pinned" && (id >> 16) == 4) f.ProducerDrops(id);
            };
        }
        std::vector<RawReading> out;
        std::string error;
        CHECK(reader->Poll(out, error) && out.size() == 1 && out[0].generation == 5);
        CHECK(error.find("generations 1..4") != std::string::npos);
        CHECK(f.Get<std::uint64_t>(Entries + 32) == Id(5, 1));
        if (mode == 2) {
            CHECK(f.Get<std::uint64_t>(Entries + 16) == 0);
            CHECK((f.Get<std::uint64_t>(Bitmap) & 2) != 0 && hookFailure.empty());
        }
        hook = {};
    }
}

void LatestReclaimedAfterPin() {
    Fixture f;
    f.Publish(0, 1);
    auto reader = f.Reader();
    hook = [&](std::string_view stage, void*, std::uint64_t id) {
        if (stage == "pinned") f.ProducerDrops(id);
    };
    std::vector<RawReading> out;
    std::string error;
    CHECK(!reader->Poll(out, error) && out.empty() && error.find("left retained history") != std::string::npos);
    CHECK(error.starts_with("retry:"));
    CHECK(f.Get<std::uint64_t>(Entries) == 0 && f.Get<std::uint64_t>(Entries + 8) == 0);
    CHECK(f.Get<std::uint64_t>(Bitmap) == 1 && hookFailure.empty());
}

void ErrorDiscardsPartialAndReleasesReferences() {
    for (const unsigned mode : {0U, 1U, 2U, 3U}) {
        Fixture f;
        f.Publish(0, 1);
        f.Publish(1, 2, 0);
        auto reader = f.Reader();
        if (mode == 0) f.Put<std::uint32_t>(Data + 0x28, 2);
        if (mode == 1) f.Put<std::uint32_t>(Data + 0x2C, 18);
        if (mode == 2) f.Put<std::uint32_t>(Data + 0x2C, 0xFFFFFFFF);
        if (mode == 3) f.Put<std::uint32_t>(Data + 0xC, 0);
        const auto before = f.storage;
        std::vector<RawReading> out(1);
        std::string error;
        CHECK(!reader->Poll(out, error) && out.empty() && !error.empty());
        CHECK(!error.starts_with("retry:"));
        CHECK(f.storage == before);
        f.Put<std::uint32_t>(Data + 0x28, 1);
        f.Put<std::uint32_t>(Data + 0x2C, 17);
        f.Put<std::uint32_t>(Data + 0xC, 1);
        CHECK(reader->Poll(out, error) && out.size() == 2 && out[0].generation == 1);
    }
}

void PreviousOverflowReleasesCurrent() {
    Fixture f;
    f.Publish(0, 1);
    f.Publish(1, 2, 0);
    f.Put<std::uint64_t>(Entries, Id(1, 0xFFFF));
    auto reader = f.Reader();
    const auto before = f.storage;
    std::vector<RawReading> out;
    std::string error;
    CHECK(!reader->Poll(out, error) && out.empty() && error.find("overflow") != std::string::npos);
    CHECK(f.storage == before);
}

void ExceptionReleasesBothPins() {
    Fixture f;
    f.Publish(0, 1);
    f.Publish(1, 2, 0);
    auto reader = f.Reader();
    const auto before = f.storage;
    hook = [&](std::string_view stage, void*, std::uint64_t id) {
        if (stage == "pinned" && (id >> 16) == 1) throw std::bad_alloc();
    };
    std::vector<RawReading> out;
    std::string error;
    CHECK(!reader->Poll(out, error) && out.empty() && !error.empty());
    CHECK(f.storage == before);
    hook = {};
    CHECK(reader->Poll(out, error) && out.size() == 2);
}

void ImmutableMutationBeforeAndDuringPoll() {
    for (const auto offset : {M + 4, M + 0x140, M + 0x1F8, I + 0x10, L + 0x14, std::size_t{8}}) {
        Fixture f;
        f.Publish(0, 1);
        auto reader = f.Reader();
        const auto original = f.Get<std::uint32_t>(offset);
        f.Put<std::uint32_t>(offset, original ^ 1U);
        const auto before = f.storage;
        std::vector<RawReading> out;
        std::string error;
        CHECK(!reader->Poll(out, error) && out.empty() && error.find("mutation invariant") != std::string::npos);
        CHECK(f.storage == before);
    }
    Fixture f;
    f.Publish(0, 1);
    auto reader = f.Reader();
    hook = [&](std::string_view stage, void*, std::uint64_t) {
        if (stage == "pinned") f.Put<std::uint32_t>(I + 0x10, 0xFFFFFFF8);
    };
    std::vector<RawReading> out;
    std::string error;
    CHECK(!reader->Poll(out, error) && out.empty());
    CHECK(f.Get<std::uint64_t>(Entries) == Id(1, 1));
}

void ImmutableMutationDuringCreation() {
    for (const auto offset : {std::size_t{8}, I + 0x10, L + 0x14, M + 0x140}) {
        Fixture f;
        hook = [&](std::string_view stage, void*, std::uint64_t) {
            if ((offset == 8 && stage == "metadata-copied") ||
                (offset != 8 && stage == "layout-copied")) {
                f.Put<std::uint32_t>(offset, f.Get<std::uint32_t>(offset) ^ 8U);
            }
        };
        std::string error;
        CHECK(!SharedReader::Create(f.Base(), f.bytes, error));
        CHECK(error.starts_with("shared mutation invariant:"));
        hook = {};
    }
}

void LastReleaseOnHighBitmapBit() {
    for (const std::size_t slot : {std::size_t{63}, std::size_t{64}, std::size_t{511}}) {
        Fixture f(512);
        f.Publish(slot, 1);
        auto reader = f.Reader();
        hook = [&](std::string_view stage, void*, std::uint64_t id) {
            if (stage == "before-release") f.ProducerDrops(id);
        };
        std::vector<RawReading> out;
        std::string error;
        CHECK(reader->Poll(out, error) && out.size() == 1 && error.empty());
        CHECK(f.Get<std::uint64_t>(Bitmap + (slot / 64) * 8) == (1ULL << (slot % 64)));
        CHECK(f.Get<std::uint64_t>(Entries + slot * 16) == 0);
        CHECK(f.Get<std::uint64_t>(Entries + slot * 16 + 8) == 0);
        CHECK(hookFailure.empty());
        hook = {};
    }
}

void PublicationDuringPollUsesNextCursorInterval() {
    Fixture f;
    f.Publish(0, 1);
    auto reader = f.Reader();
    hook = [&](std::string_view stage, void*, std::uint64_t id) {
        if (stage == "pinned" && (id >> 16) == 1) f.Publish(1, 2, 0);
    };
    std::vector<RawReading> out;
    std::string error;
    CHECK(reader->Poll(out, error) && out.size() == 1 && out[0].generation == 1 && error.empty());
    hook = {};
    CHECK(reader->Poll(out, error) && out.size() == 1 && out[0].generation == 2 && error.empty());
    CHECK(f.Get<std::uint64_t>(Entries) == Id(1, 1));
    CHECK(f.Get<std::uint64_t>(Entries + 16) == Id(2, 1));
}

void ReferenceOwnershipMutationIsFatal() {
    Fixture f;
    f.Publish(0, 1);
    auto reader = f.Reader();
    hook = [&](std::string_view stage, void*, std::uint64_t) {
        if (stage == "before-release") f.Put<std::uint64_t>(Entries, Id(9, 1));
    };
    std::vector<RawReading> out;
    std::string error;
    CHECK(!reader->Poll(out, error) && out.empty());
    CHECK(error == "shared mutation invariant: reference ownership was lost");
    CHECK(f.Get<std::uint64_t>(Entries) == Id(9, 1));
    CHECK(f.Get<std::uint64_t>(Bitmap) == 0);
}

void BadLinksAndRegression() {
    for (const auto flags : {History | 1 | Previous | (4ULL << 32),
        History | 1 | Previous, History | 1 | Next | (4ULL << 48)}) {
        Fixture f;
        f.Publish(0, 2);
        f.Put<std::uint64_t>(Entries + 8, flags);
        auto reader = f.Reader();
        const auto before = f.storage;
        std::vector<RawReading> out;
        std::string error;
        CHECK(!reader->Poll(out, error) && out.empty() && f.storage == before);
    }
    Fixture f;
    f.Publish(0, 1);
    auto reader = f.Reader();
    std::vector<RawReading> out;
    std::string error;
    CHECK(reader->Poll(out, error));
    for (const auto id : {std::uint64_t{0}, Id(1, 1)}) {
        f.Put<std::uint64_t>(Latest, id);
        CHECK(!reader->Poll(out, error) && out.empty());
    }
    f.Publish(1, 2, 0);
    CHECK(reader->Poll(out, error));
    f.Put<std::uint64_t>(Latest, Id(1, 0));
    CHECK(!reader->Poll(out, error) && out.empty());
}

void NonRawGenerationAndNoSequence() {
    Fixture f;
    f.Publish(0, 1);
    f.Publish(1, 2, 0, false);
    auto reader = f.Reader();
    std::vector<RawReading> out;
    std::string error;
    CHECK(reader->Poll(out, error) && out.size() == 1 && out[0].generation == 1 && error.empty());
    CHECK(reader->Poll(out, error) && out.empty());
    Fixture g;
    g.Put<std::uint8_t>(L + 0x84, 0);
    g.Publish(0, 1);
    auto noSequence = g.Reader();
    CHECK(noSequence->Poll(out, error) && out.size() == 1 && out[0].rawSequence == 0);
    Fixture h;
    h.Put<std::uint32_t>(M + 0x68, 0);
    h.Put<std::uint32_t>(M + 0x70, 0);
    h.Put<std::uint64_t>(M + 0xA0, 0);
    h.Put<std::uint32_t>(L, 0x20);
    h.Put<std::uint32_t>(L + 4, 0);
    h.Put<std::uint16_t>(L + 8, 0);
    h.Put<std::uint32_t>(L + 0x14, 0);
    h.Put<std::uint8_t>(L + 0x84, 0);
    h.Put<std::uint32_t>(I + 0x1C, 0x20);
    auto noRaw = h.Reader();
    CHECK(noRaw->Poll(out, error) && out.empty() && error.empty());
}

void EmptyAndRetired() {
    Fixture f;
    auto reader = f.Reader();
    std::vector<RawReading> out(1);
    std::string error = "old";
    CHECK(reader->Poll(out, error) && out.empty() && error.empty());
    f.Publish(0, 1);
    CHECK(reader->Poll(out, error) && out.size() == 1);
    reader->Retire();
    reader->Retire();
    f.storage.clear();
    f.storage.shrink_to_fit();
    CHECK(reader->Info().vendor == 0x045E);
    CHECK(!reader->Poll(out, error) && out.empty() && error == "reader is retired");
    reader.reset();
}

void GuardPageAndPaddedAllocation() {
    Fixture f;
    f.Publish(0, 1);
    const auto pages = (f.bytes + 4095) / 4096;
    auto* const region = static_cast<std::uint8_t*>(VirtualAlloc(nullptr,
        (pages + 1) * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(region);
    struct Allocation {
        void* address;
        ~Allocation() { VirtualFree(address, 0, MEM_RELEASE); }
    } allocation{region};
    DWORD previous = 0;
    CHECK(VirtualProtect(region + pages * 4096, 4096, PAGE_NOACCESS, &previous));
    auto* const base = region + pages * 4096 - f.bytes;
    std::memcpy(base, f.Base(), f.bytes);
    std::string error;
    auto reader = SharedReader::Create(base, f.bytes, error);
    CHECK(reader);
    std::vector<RawReading> out;
    CHECK(reader->Poll(out, error) && out.size() == 1);
    reader->Retire();
    CHECK(!SharedReader::Create(base, f.bytes - 8, error));
    const std::uint32_t badTotal = static_cast<std::uint32_t>(f.bytes + 8);
    std::memcpy(base, &badTotal, sizeof(badTotal));
    CHECK(!SharedReader::Create(base, f.bytes, error));
}

void StoredSlotCountAndHighGeneration() {
    Fixture f(512);
    const auto generation = (1ULL << 48) - 1;
    f.Publish(511, generation);
    auto reader = f.Reader();
    std::vector<RawReading> out;
    std::string error;
    CHECK(reader->Poll(out, error) && out.size() == 1 && out[0].generation == generation);
    CHECK(error.find("gap:") == 0 && f.Get<std::uint64_t>(Entries + 511 * 16) == Id(generation, 1));
    CHECK(reader->Poll(out, error) && out.empty());
    Fixture g(2);
    g.Publish(0, 9);
    g.Publish(1, 10, 0);
    g.Put<std::uint64_t>(Entries + 8, History | Previous | 1 | (1ULL << 32));
    auto bounded = g.Reader();
    unsigned pins = 0;
    hook = [&](std::string_view stage, void*, std::uint64_t) { if (stage == "pinned") ++pins; };
    CHECK(bounded->Poll(out, error) && out.size() == 2 && error.find("slot-count limit") != std::string::npos);
    CHECK(pins == 2);
}

void ZeroAndMaximumPayloadLengths() {
    for (const std::uint32_t length : {0U, 32U}) {
        Fixture f;
        f.Publish(0, 1, -1, true, 0);
        f.Put<std::uint32_t>(Data + 0x2C, length);
        auto reader = f.Reader();
        std::vector<RawReading> out;
        std::string error;
        CHECK(reader->Poll(out, error) && out.size() == 1 && out[0].bytes.size() == length);
    }
}
void InputPolicyOnlyChangesItsOwnDword() {
    Fixture fixture;
    const auto policy = Data + fixture.count * Stride;
    fixture.Put<std::uint32_t>(policy, 0x15);
    const auto before = fixture.storage;
    std::string error;
    auto reader = SharedReader::Create(fixture.Base(), fixture.bytes, error);
    CHECK(reader != nullptr);
    std::uint32_t value = 0;
    CHECK(reader->ReadInputPolicy(value, error) && value == 0x15);
    CHECK(fixture.storage == before);
    bool changed = false;
    CHECK(reader->UseDefaultInputPolicy(value, changed, error));
    CHECK(value == 0x15 && changed && fixture.Get<std::uint32_t>(policy) == 0);
    const auto* prior = reinterpret_cast<const std::uint8_t*>(before.data());
    for (std::size_t i = 0; i < fixture.bytes; ++i) {
        CHECK(fixture.Base()[i] == (i >= policy && i < policy + 4 ? 0 : prior[i]));
    }
    CHECK(reader->UseDefaultInputPolicy(value, changed, error) && value == 0 && !changed);
    fixture.Publish(0, 1);
    std::vector<RawReading> output;
    CHECK(reader->Poll(output, error) && output.size() == 1 && output[0].bytes.size() == 17);
    reader->Retire();
    CHECK(!reader->UseDefaultInputPolicy(value, changed, error) && !changed);
}

void InputPolicyRejectsMissingUnalignedOrChangedLayouts() {
    {
        Fixture fixture;
        fixture.Put<std::uint32_t>(I + 0x28, 0);
        std::string error;
        auto reader = SharedReader::Create(fixture.Base(), fixture.bytes, error);
        CHECK(reader != nullptr);
        std::uint32_t previous = 0;
        bool changed = true;
        const auto before = fixture.storage;
        CHECK(!reader->UseDefaultInputPolicy(previous, changed, error) && !changed);
        CHECK(fixture.storage == before);
    }
    {
        Fixture fixture;
        fixture.Put<std::uint32_t>(I + 0x28, 0x2001);
        std::string error;
        CHECK(!SharedReader::Create(fixture.Base(), fixture.bytes, error));
    }
    {
        Fixture fixture;
        std::string error;
        auto reader = SharedReader::Create(fixture.Base(), fixture.bytes, error);
        CHECK(reader != nullptr);
        fixture.Put<std::uint32_t>(I + 0x28, 0x2000);
        const auto before = fixture.storage;
        std::uint32_t previous = 0;
        bool changed = true;
        CHECK(!reader->UseDefaultInputPolicy(previous, changed, error) && !changed);
        CHECK(fixture.storage == before);
    }
}

void DiagnosticsDistinguishEmptyUnchangedNonrawAndFailedPins() {
    Fixture fixture;
    std::string error;
    auto reader = SharedReader::Create(fixture.Base(), fixture.bytes, error);
    CHECK(reader != nullptr);
    std::vector<RawReading> out;
    CHECK(reader->Poll(out, error) && out.empty());
    CHECK(reader->Diagnostics().empty == 1 && reader->Diagnostics().latest == 0);
    fixture.Publish(0, 1, -1, false);
    CHECK(reader->Poll(out, error) && out.empty());
    CHECK(reader->Diagnostics().nonraw == 1 && reader->Diagnostics().raw == 0);
    CHECK(reader->Diagnostics().sampleId == Id(1, 0));
    CHECK((reader->Diagnostics().flags & 1) == 0 && reader->Diagnostics().readingKinds == 1);
    CHECK(reader->Diagnostics().cursor == Id(1, 0));
    CHECK(reader->Poll(out, error) && out.empty() && reader->Diagnostics().unchanged == 1);
    fixture.Publish(1, 2, 0);
    CHECK(reader->Poll(out, error) && out.size() == 1);
    CHECK(reader->Diagnostics().raw == 1 && reader->Diagnostics().latestPin == 1);
    CHECK(reader->Diagnostics().cursor == Id(2, 1));
    fixture.Publish(2, 3, 1);
    fixture.Put<std::uint64_t>(Entries + 2 * 16, 3ULL << 16);
    CHECK(!reader->Poll(out, error) && out.empty());
    CHECK(reader->Diagnostics().latestPin == 3 && reader->Diagnostics().latest == Id(3, 2));
    CHECK(reader->Diagnostics().cursor == Id(2, 1));
}
} // namespace

namespace sdl_paddles::reader_testing {
void Observe(const char* stage, void* base, std::uint64_t id) {
    if (hook) hook(stage, base, id);
}
} // namespace sdl_paddles::reader_testing

int main() {
    const std::pair<const char*, void(*)()> tests[]{
        {"metadata and full captured raw17", MetadataAndFullRawPayload},
        {"unknown report IDs and generic identity", UnknownReportAndGenericIdentity},
        {"logical view and address bounds", LogicalViewBounds},
        {"layout bounds and eight-byte alignment", LayoutBoundsAndAlignment},
        {"region overlap and containing bounds", OverlapAndContainingLayoutBounds},
        {"metadata and 64-bit relative offsets", MetadataBounds},
        {"stale zero overflow and invalid ID rejection", StaleZeroOverflowAndInvalidIds},
        {"CAS retries preserve concurrent references", CasRetriesAgainstChangedReference},
        {"CAS generation change and contention bound", CasRetryRejectsGenerationChangeAndIsBounded},
        {"last-reference clearing and bitmap release", LastReferenceReleaseAndBitmap},
        {"chronological history and retain-before-release", HistoryChronologyAndTwoPins},
        {"retained-history gap and cursor continuation", RetainedHistoryGapAndContinuation},
        {"missing predecessor and post-pin history bit", MissingPredecessorAndPostPinHistoryFlag},
        {"latest reclaimed after pin", LatestReclaimedAfterPin},
        {"error discards partial data and releases references", ErrorDiscardsPartialAndReleasesReferences},
        {"previous overflow releases current", PreviousOverflowReleasesCurrent},
        {"exception cleanup with two references", ExceptionReleasesBothPins},
        {"immutable metadata and layout mutations", ImmutableMutationBeforeAndDuringPoll},
        {"immutable snapshots during creation", ImmutableMutationDuringCreation},
        {"last release across bitmap word boundaries", LastReleaseOnHighBitmapBit},
        {"publication during poll preserves cursor intervals", PublicationDuringPollUsesNextCursorInterval},
        {"reference ownership mutation is fatal", ReferenceOwnershipMutationIsFatal},
        {"invalid links and generation regression", BadLinksAndRegression},
        {"non-raw generations and optional sequence", NonRawGenerationAndNoSequence},
        {"empty poll and retirement before unmap", EmptyAndRetired},
        {"guard page and logical versus allocation bounds", GuardPageAndPaddedAllocation},
        {"stored slot count and full 48-bit generation", StoredSlotCountAndHighGeneration},
        {"zero and maximum payload lengths", ZeroAndMaximumPayloadLengths},
        {"input policy writes only its own DWORD", InputPolicyOnlyChangesItsOwnDword},
        {"input policy rejects unsafe layouts and retirement", InputPolicyRejectsMissingUnalignedOrChangedLayouts},
        {"diagnostics distinguish empty nonraw unchanged and failed pin", DiagnosticsDistinguishEmptyUnchangedNonrawAndFailedPins}
    };
    unsigned passed = 0;
    for (const auto& [name, test] : tests) {
        hook = {};
        hookFailure.clear();
        try {
            test();
            CHECK(hookFailure.empty());
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& exception) {
            hook = {};
            std::cout << "FAIL " << name << ": " << exception.what() << '\n';
        }
        hook = {};
    }
    std::cout << "RESULT " << passed << '/' << std::size(tests) << " passed\n";
    return passed == std::size(tests) ? 0 : 1;
}
