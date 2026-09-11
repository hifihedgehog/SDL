#include "../src/joystick/windows/SDL_xinput_paddle_trace.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace sdl_paddles;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (0)

int main()
{
    try {
        std::array<trace::Record, 128> output;
        std::uint64_t dropped = 0;
        trace::SetEnabled(false);
        CHECK(trace::NextEvent() == 0);
        trace::Push({});
        CHECK(trace::Drain(output.data(), output.size(), dropped) == 0 && dropped == 0);
        trace::SetEnabled(true);
        const auto event = trace::NextEvent();
        CHECK(event != 0 && trace::NextEvent() > event);
        std::array<std::uint8_t, 80> bytes{};
        for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i);
        trace::Record source;
        source.kind = trace::Kind::ServicePacket;
        source.nativeId = 42; source.event = event;
        trace::Raw(source, bytes.data(), bytes.size());
        bytes.fill(0xff);
        CHECK(trace::Drain(output.data(), output.size(), dropped) == 1 && dropped == 0);
        CHECK(output[0].nativeId == 42 && output[0].event == event && output[0].length == 80);
        CHECK(output[0].truncated && output[0].copied == 64 && output[0].bytes[14] == 14 && output[0].bytes[63] == 63);
        source = output[0];
        trace::Raw(source, nullptr, 17);
        CHECK(trace::Drain(output.data(), output.size(), dropped) == 1 && output[0].truncated && output[0].copied == 0);
        for (unsigned i = 0; i < 4100; ++i) { source.reading = i; trace::Push(source); }
        unsigned count = 0;
        std::uint64_t last = 0, losses = 0;
        for (;;) {
            const auto n = trace::Drain(output.data(), output.size(), dropped);
            losses += dropped;
            if (!n) break;
            for (std::size_t i = 0; i < n; ++i) {
                CHECK(output[i].order > last);
                CHECK(output[i].reading == count + 4);
                last = output[i].order; ++count;
            }
        }
        CHECK(count == 4096 && losses == 4);
        std::array<std::thread, 4> writers;
        for (auto& thread : writers) thread = std::thread([] {
            for (unsigned i = 0; i < 500; ++i) { trace::Record record; record.event = trace::NextEvent(); trace::Push(record); }
        });
        for (auto& thread : writers) thread.join();
        count = 0;
        while (const auto n = trace::Drain(output.data(), output.size(), dropped)) {
            CHECK(dropped == 0);
            for (std::size_t i = 0; i < n; ++i) { CHECK(output[i].order > last && output[i].event); last = output[i].order; ++count; }
        }
        CHECK(count == 2000);
        trace::SetEnabled(false);
        std::puts("PASS trace opt-in, owned payload, truncation, overflow accounting, concurrent ordering, and drain.");
        return 0;
    } catch (const std::exception &error) { std::printf("FAIL %s\n", error.what()); return 1; }
}
