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

/* Captured bytes -> real DataRoute/Shared -> real SDL publisher/mapping/events.
 * No Owner, service connection, identity resolver, or GATT client is started.
 * The C bridge replaces the hardware-dependent SDL_XINPUT_PaddleUpdate adapter,
 * not SDL_SendJoystickButton or its mapping/event behavior. Its bit-to-button
 * boundary is checked against explicit raw indices and actual binding objects.
 * This proves downstream handling of saved payloads, not current acquisition,
 * hardware GUID detection, WGI readiness, concurrent queues, or live payloads.
 */
#define SDL_MAIN_HANDLED
#define SDL_XINPUT_PADDLE_BROKER_TESTING
#include "../src/joystick/windows/SDL_xinput_paddle.cpp"
#include <cstdio>
#include <stdexcept>

extern "C" {
int PaddlePipelineInitialize(void);
int PaddlePipelineOpen(void **output);
int PaddlePipelineSetFaceA(void *handle, int down);
int PaddlePipelinePublish(void *handle, unsigned mask, int expected_a, unsigned *raw, unsigned *mapped);
void PaddlePipelineClose(void *handle);
unsigned PaddlePipelineChecks(void);
unsigned PaddlePipelineFailures(void);
const char *PaddlePipelineError(void);
void PaddlePipelineQuit(void);
}

using namespace sdl_paddles;
using namespace sdl_paddles::broker_detail;
static unsigned checks;
static const char *stage = "startup";
static unsigned source_line;
#define CHECK(expr) do { ++checks; if (!(expr)) throw std::runtime_error(std::string(stage) + ": " + #expr + ", source line " + std::to_string(source_line)); } while (false)
#define CORE(expr) do { ++checks; if (!(expr)) throw std::runtime_error(std::string(stage) + ": " + PaddlePipelineError()); } while (false)

struct CapturedCase {
    bool bluetooth;
    unsigned line;
    std::uint16_t word;
    std::uint8_t mask;
    const char *hex;
};

/* Exact cases from elite-paddle-corpus.json, SHA256
 * 35390220c5f165659c73895cc565672f39643cc6738cdaf53c700891f24328d2.
 * Bluetooth source: vendor-notify-3.log, SHA256
 * 7ad419411ca291d60fe505d03dac0d6084ece27511a3a556dda24ff5a80ded7e.
 * USB source: wgi-usb-background-68772-165963968.log, SHA256
 * 85239d1b827d22ba62ec44997a6b0ba58f78c85229b511c9b6564c35f636e735.
 * Lines are source-log lines. All profiles are zero. No physical paddle-name
 * experiment is inferred from the corpus. The report ID is outside the bytes.
 */
static const CapturedCase cases[] = {
    {true, 4, 0x0000, 0x00, "0000220100002902a7f93afcd307000000"},
    {true, 351, 0x0000, 0x01, "00000000000012fafb0194fb2afb010000"},
    {true, 401, 0x0000, 0x02, "00000000000012fafb0194fb2afb020000"},
    {true, 326, 0x0000, 0x04, "00000000000012fafb0194fb2afb040000"},
    {true, 375, 0x0000, 0x08, "00000000000012fafb0194fb2afb080000"},
    {true, 428, 0x0000, 0x0F, "00000000000012fafb0194fb2afb0f0000"},
    {true, 469, 0x0010, 0x0F, "10000000000012fafb0194fb2afb0f0000"},
    {false, 41, 0x0000, 0x00, "0000000000001cfdbcfd86f766fd000000"},
    {false, 49, 0x0000, 0x01, "0000000000001cfdbcfd86f766fd010000"},
    {false, 57, 0x0000, 0x02, "0000000000001cfdbcfd86f766fd020000"},
    {false, 45, 0x0000, 0x04, "0000000000001cfdbcfd86f766fd040000"},
    {false, 53, 0x0000, 0x08, "0000000000001cfdbcfd86f766fd080000"},
    {false, 69, 0x0000, 0x0F, "0000000000001cfdbcfd86f766fd0f0000"},
    {false, 67, 0x0010, 0x0F, "1000000000001cfdbcfd86f766fd0f0000"}
};

static unsigned Digit(char c)
{
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    CHECK(c >= 'a' && c <= 'f');
    return static_cast<unsigned>(c - 'a' + 10);
}

static std::array<std::uint8_t, 17> Bytes(const CapturedCase &c)
{
    std::array<std::uint8_t, 17> result{};
    CHECK(std::strlen(c.hex) == result.size() * 2);
    for (size_t i = 0; i < result.size(); ++i) result[i] = static_cast<std::uint8_t>((Digit(c.hex[2 * i]) << 4) | Digit(c.hex[2 * i + 1]));
    CHECK(result[14] == c.mask && result[15] == 0);
    CHECK((result[0] | (static_cast<unsigned>(result[1]) << 8)) == c.word);
    return result;
}

struct CoreFixture {
    void *handle = nullptr;
    CoreFixture() { if (!PaddlePipelineOpen(&handle)) { PaddlePipelineClose(handle); throw std::runtime_error(PaddlePipelineError()); } }
    ~CoreFixture() { PaddlePipelineClose(handle); }
};

struct Expected {
    const CapturedCase *payload;
    bool release;
    bool hold_a;
};

struct TraceCapture {
    TraceCapture() { trace::SetEnabled(false); trace::SetEnabled(true); }
    ~TraceCapture() { trace::SetEnabled(false); }
};

static void Replay(bool gatt, PaddleTransport gipTransport = PaddleTransport::UsbGip)
{
    stage = gatt ? "GATT captured payload path" : gipTransport == PaddleTransport::WirelessGip ?
        "Wireless GIP shared payload replay" : "ServicePacket captured payload path";
    TraceCapture capture;
    CHECK(trace::Enabled());
    auto shared = std::make_unique<Shared>();
    shared->frequency = 10000000;
    auto attachment = std::make_shared<Attachment>();
    attachment->token = shared->NewToken();
    attachment->generation = 11;
    attachment->instanceId = 1;
    attachment->user = 0;
    attachment->count = 1;
    attachment->trace = true;
    attachment->physical.transport = gatt ? PaddleTransport::Bluetooth : gipTransport;
    attachment->physical.nativeId = gatt ? 0 : 0x1234;
    attachment->physical.container.Data1 = 1;
    attachment->physical.instance = L"offline captured pipeline fixture";
    const auto slot = shared->Attach(attachment);
    CHECK(slot < RouteLimit && shared->Active(slot, attachment->token));
    DataRoute route;
    route.Start(attachment->physical.transport, attachment->token, 19);
    std::vector<Expected> expected;
    std::uint64_t generation = 0;
    for (const auto &c : cases) {
        if (gatt && !c.bluetooth) continue;
        for (unsigned release = 0; release < 2; ++release) {
            const auto &payload = release ? cases[c.bluetooth ? 0 : 7] : c;
            source_line = payload.line;
            const auto original = Bytes(payload);
            ++generation;
            if (gatt) {
                GattPaddlePacket packet;
                packet.attachmentGeneration = attachment->token;
                packet.bytes = original;
                packet.receivedQpc = static_cast<std::int64_t>(generation * 10000);
                packet.hasPayload = true;
                std::vector<GattPaddlePacket> batch{packet};
                route.Gatt(*shared, slot, *attachment, batch, generation * 10000);
                CHECK(batch[0].bytes == original);
                batch[0].bytes.fill(0xA5);
            } else {
                // Bluetooth-origin bytes are reused without edits on the common
                // 17-byte service payload contract. This is not a claim that
                // Bluetooth acquisition uses SIPC. Its real tag is tested above.
                ServicePacket packet;
                packet.nativeId = attachment->physical.nativeId;
                packet.viewGeneration = 19;
                packet.hasReading = true;
                packet.reading.generation = generation;
                packet.reading.timestamp = generation * 1000;
                packet.reading.rawSequence = generation;
                packet.reading.reportId = 0x0C;
                packet.reading.bytes.assign(original.begin(), original.end());
                std::vector<ServicePacket> batch{packet};
                route.Service(*shared, slot, *attachment, batch, generation * 10000);
                CHECK(std::equal(batch[0].reading.bytes.begin(), batch[0].reading.bytes.end(), original.begin(), original.end()));
                batch[0].reading.bytes.assign(17, 0xA5);
            }
            CHECK(route.decoder.paddles.raw_mask == payload.mask);
            CHECK(route.decoder.paddles.physical_mask == payload.mask && route.latest == payload.mask);
            CHECK(route.decoder.paddles.button_word == payload.word && route.decoder.paddles.profile == 0);
            CHECK(route.haveState && route.diagnostics.states == generation);
            CHECK(route.diagnostics.malformed == 0 && route.diagnostics.gaps == 0);
            expected.push_back({&payload, release != 0, (c.word & 0x10) != 0});
        }
    }
    CHECK(expected.size() == (gatt ? 14u : 28u));
    CHECK(expected.size() < EventLimit);
    CHECK(expected.size() * 3 < TraceLimit);
    CHECK((gatt ? route.diagnostics.gatt : route.diagnostics.separate) == expected.size());
    // The queue must own earlier masks after all input buffers and decoder state
    // have changed. No private queue layout or replacement publisher is used.
    route.Start(attachment->physical.transport, attachment->token, 19);
    std::array<Edge, EventLimit + 2> edges{};
    Diagnostics diagnostics;
    CHECK(shared->Drain(slot, attachment->token, edges.data(), edges.size(), diagnostics, false) == 0);
    shared->Publish(slot, attachment->token + 1, {10000000, 15, false});
    std::vector<TraceRecord> queued_trace(TraceLimit);
    std::uint64_t lost = 0;
    const auto queued_records = shared->DrainTrace(queued_trace.data(), queued_trace.size(), lost);
    CHECK(lost == 0 && queued_records == expected.size() * 3);
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto original = Bytes(*expected[i].payload);
        const auto &raw = queued_trace[3 * i];
        const auto &decoded = queued_trace[3 * i + 1];
        const auto &queued = queued_trace[3 * i + 2];
        CHECK(raw.stage == TraceStage::Raw && decoded.stage == TraceStage::Decoded && queued.stage == TraceStage::Queued);
        CHECK(raw.id != 0 && decoded.id == raw.id && queued.id == raw.id);
        CHECK(raw.attachment == attachment->token && decoded.attachment == attachment->token && queued.attachment == attachment->token);
        CHECK(raw.size == original.size() && raw.copied == original.size());
        CHECK(std::equal(original.begin(), original.end(), raw.bytes.begin()));
        CHECK(raw.gatt == gatt && raw.report == (gatt ? 0u : 0x0Cu));
        CHECK(decoded.flags & SDL_XINPUT_PADDLE_STATE_READY);
        CHECK(raw.bytes[14] == expected[i].payload->mask && decoded.rawMask == raw.bytes[14]);
        CHECK(decoded.mask == expected[i].payload->mask && queued.mask == decoded.mask);
    }
    const auto count = shared->Drain(slot, attachment->token, edges.data(), edges.size(), diagnostics, true);
    CHECK(count == expected.size() && diagnostics.overflows == 0 && diagnostics.states == expected.size());
    CHECK(shared->Drain(slot, attachment->token, edges.data() + count, edges.size() - count, diagnostics, true) == 0);
    std::vector<TraceRecord> drained_trace(TraceLimit);
    const auto drained_records = shared->DrainTrace(drained_trace.data(), drained_trace.size(), lost);
    CHECK(lost == 0 && drained_records == count);

    CoreFixture core;
    std::uint64_t previous_qpc = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto &want = expected[i];
        source_line = want.payload->line;
        CHECK(edges[i].mask == want.payload->mask && !edges[i].release);
        CHECK(edges[i].traceId == queued_trace[3 * i].id);
        CHECK(drained_trace[i].stage == TraceStage::Drained && drained_trace[i].id == edges[i].traceId);
        CHECK(drained_trace[i].mask == edges[i].mask && queued_trace[3 * i + 2].mask == edges[i].mask);
        CHECK(edges[i].qpc >= previous_qpc);
        previous_qpc = edges[i].qpc;
        if (!want.release) CORE(PaddlePipelineSetFaceA(core.handle, want.hold_a));
        unsigned raw = 0, mapped = 0;
        CORE(PaddlePipelinePublish(core.handle, edges[i].mask, want.hold_a, &raw, &mapped));
        CHECK(raw == want.payload->mask && mapped == want.payload->mask);
        std::printf("FRAME path=%s origin=%s line=%u release=%u b14=%x decoded=%x queued=%x drained=%x raw=%x mapped=%x faceA=%u\n",
            gatt ? "gatt" : "service", want.payload->bluetooth ? "bluetooth" : "usb", source_line,
            unsigned(want.release), unsigned(queued_trace[3 * i].bytes[14]), unsigned(queued_trace[3 * i + 1].mask),
            unsigned(queued_trace[3 * i + 2].mask), unsigned(drained_trace[i].mask), raw, mapped, unsigned(want.hold_a));
        if (want.release) CORE(PaddlePipelineSetFaceA(core.handle, 0));
    }
    CHECK(count && edges[count - 1].mask == 0);
    shared->Detach(slot, attachment->token);
    shared->Publish(slot, attachment->token, {20000000, 15, false});
    CHECK(shared->Drain(slot, attachment->token, edges.data(), edges.size(), diagnostics, true) == 0);
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        CORE(PaddlePipelineInitialize());
        CHECK(std::size(cases) == 14);
        Replay(false);
        Replay(true);
        Replay(false, PaddleTransport::WirelessGip);
        CORE(PaddlePipelineFailures() == 0);
        PaddlePipelineQuit();
        std::printf("PASS USB and wireless GIP replay of 14 saved cases plus releases, 7 GATT cases plus releases, checks=%u core_checks=%u\n",
            checks, PaddlePipelineChecks());
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        PaddlePipelineQuit();
        return 1;
    }
}
