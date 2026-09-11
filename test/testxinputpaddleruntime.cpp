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

// Offline tests include the real private policy and parser. The explicit --live
// mode only reads system/service/file metadata through the production functions.
#include "../src/joystick/windows/SDL_xinput_paddle_runtime.cpp"
#include <cstdio>
#include <functional>

using namespace sdl_paddles;
using namespace sdl_paddles::runtime_detail;
#define CHECK(x) do { if (!(x)) { throw std::runtime_error(#x); } } while (0)

struct FakeSource : Source {
    Service service{50, SERVICE_RUNNING, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
        L"C:\\Windows\\System32\\GameInputSvc.exe", L"LocalSystem", 15};
    std::vector<Process> processes{{50, 4, 0, 1000, L"GameInputSvc.exe"},
        {60, 50, 1, 2000, L"GameInputSvc.exe"}, {70, 42, 1, 3000, L"test.exe"}};
    std::array<FileStamp, Profile.size()> stamps{};
    std::function<void(FakeSource&)> afterHashes;
    std::function<void(FakeSource&)> secondProcesses;
    std::function<void(FakeSource&)> secondService;
    int openFailure = -1, hashFailure = -1, wrongHash = -1, changedPath = -1;
    unsigned serviceReads = 0, processReads = 0, opens = 0, hashes = 0, checks = 0;
    FakeSource()
    {
        for (std::size_t i = 0; i < Profile.size(); ++i) {
            stamps[i] = {1, {}, Profile[i].size, 900, 4000, FILE_ATTRIBUTE_ARCHIVE};
            stamps[i].id[0] = static_cast<std::uint8_t>(i + 1);
        }
    }
    std::wstring SystemDirectory() const override { return L"C:\\Windows\\System32"; }
    DWORD SelfPid() const override { return 70; }
    Service ReadService() override
    {
        if (++serviceReads == 2 && secondService) { secondService(*this); }
        return service;
    }
    std::vector<Process> ReadProcesses() override
    {
        if (++processReads == 2 && secondProcesses) { secondProcesses(*this); }
        return processes;
    }
    FileStamp OpenFile(std::size_t i) override
    {
        CHECK(hashes == 0); ++opens;
        if (static_cast<int>(i) == openFailure) { throw std::runtime_error("unreadable file"); }
        return stamps[i];
    }
    std::string HashFile(std::size_t i, std::int64_t expectedSize) override
    {
        CHECK(opens == Profile.size() && expectedSize == Profile[i].size);
        ++hashes;
        if (static_cast<int>(i) == hashFailure) { throw std::runtime_error("short read"); }
        std::string digest = Profile[i].sha256;
        if (static_cast<int>(i) == wrongHash) { digest[0] = digest[0] == '0' ? '1' : '0'; }
        if (hashes == Profile.size() && afterHashes) { afterHashes(*this); }
        return digest;
    }
    FileStamp HandleStamp(std::size_t i) override { CHECK(hashes == Profile.size()); ++checks; return stamps[i]; }
    FileStamp PathStamp(std::size_t i) override
    {
        CHECK(hashes == Profile.size()); ++checks;
        auto value = stamps[i];
        if (static_cast<int>(i) == changedPath) { ++value.id[0]; }
        return value;
    }
};

static void Reject(FakeSource& source, DWORD peer = 60)
{
    std::string error;
    CHECK(!Qualify(source, peer, true, error)); CHECK(!error.empty());
}

static std::vector<BYTE> ProcessBuffer()
{
    std::vector<BYTE> bytes(sizeof(NativeProcessPrefix) + 64);
    NativeProcessPrefix value{};
    value.uniqueProcessId = reinterpret_cast<HANDLE>(60);
    value.inheritedFromUniqueProcessId = reinterpret_cast<HANDLE>(50);
    value.sessionId = 1; value.createTime.QuadPart = 2000;
    const wchar_t name[] = L"GameInputSvc.exe";
    value.imageName = {static_cast<USHORT>(sizeof(name) - 2), static_cast<USHORT>(sizeof(name)),
        reinterpret_cast<PWSTR>(bytes.data() + sizeof(value))};
    std::memcpy(bytes.data(), &value, sizeof(value));
    std::memcpy(bytes.data() + sizeof(value), name, sizeof(name));
    return bytes;
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--live") {
        char* end = nullptr; auto parsed = strtoul(argv[2], &end, 10);
        if (!parsed || *end) { return 2; }
        std::string error;
        auto start = GetTickCount64();
        bool available = PaddleUsbRuntimeAvailable(error);
        std::printf("AVAILABLE value=%u error=%s elapsed_ms=%llu\n", available ? 1u : 0u, error.c_str(), GetTickCount64() - start);
        start = GetTickCount64();
        bool server = PaddleValidateServer(parsed, error);
        std::printf("SERVER pid=%lu value=%u error=%s elapsed_ms=%llu\n", parsed, server ? 1u : 0u, error.c_str(), GetTickCount64() - start);
        return available && server ? 0 : 1;
    }
    unsigned passed = 0, failed = 0;
    auto test = [&](const char* name, std::function<void()> body) {
        try { body(); ++passed; std::printf("PASS %s\n", name); }
        catch (std::exception const& e) { ++failed; std::printf("FAIL %s: %s\n", name, e.what()); }
    };
    test("whole known profile and actual peer relationship accepted", [] {
        FakeSource s; std::string error = "stale";
        CHECK(Qualify(s, 60, true, error) && error.empty());
        CHECK(s.hashes == Profile.size() && s.checks == 2 * Profile.size() && s.processReads == 2 && s.serviceReads == 2);
    });
    test("post-start metadata change time is not image proof or rejection", [] {
        FakeSource s; std::string error; s.stamps[2].changed = 5000;
        CHECK(Qualify(s, 60, true, error));
    });
    test("pre-connect installation check permits a stopped enabled service", [] {
        FakeSource s; s.service.state = SERVICE_STOPPED; s.service.pid = 0; s.processes.clear();
        std::string error; CHECK(Qualify(s, 0, false, error)); CHECK(s.processReads == 0);
    });
    test("every manifest component is mandatory including redist", [] {
        for (int i = 0; i < static_cast<int>(Profile.size()); ++i) { FakeSource s; s.wrongHash = i; Reject(s); }
    });
    test("missing files and incomplete reads fail closed", [] {
        for (int i = 0; i < static_cast<int>(Profile.size()); ++i) {
            FakeSource a; a.openFailure = i; Reject(a);
            FakeSource b; b.hashFailure = i; Reject(b);
        }
    });
    test("wrong size directory and reparse point fail", [] {
        FakeSource a; ++a.stamps[0].size; Reject(a);
        FakeSource b; b.stamps[1].attributes |= FILE_ATTRIBUTE_DIRECTORY; Reject(b);
        FakeSource c; c.stamps[2].attributes |= FILE_ATTRIBUTE_REPARSE_POINT; Reject(c);
    });
    test("earlier file change after final digest is detected", [] {
        FakeSource s; s.afterHashes = [](FakeSource& value) { ++value.stamps[0].changed; }; Reject(s);
    });
    test("path replacement detected even when original handle is stable", [] {
        FakeSource s; s.changedPath = 0; Reject(s);
    });
    test("file identity volume and write timestamp changes detected", [] {
        for (int i = 0; i < 3; ++i) {
            FakeSource s; s.afterHashes = [i](FakeSource& v) {
                if (i == 0) { ++v.stamps[1].id[0]; } else if (i == 1) { ++v.stamps[1].volume; } else { ++v.stamps[1].written; }
            }; Reject(s);
        }
    });
    test("wrong service path account type and disabled service rejected", [] {
        FakeSource a; a.service.path = L"C:\\Elsewhere\\GameInputSvc.exe"; Reject(a);
        FakeSource b; b.service.account = L"OtherAccount"; Reject(b);
        FakeSource c; c.service.type = SERVICE_WIN32_SHARE_PROCESS; Reject(c);
        FakeSource d; d.service.startType = SERVICE_DISABLED; Reject(d);
    });
    test("quoted system path and name casing accepted", [] {
        FakeSource s; s.service.path = L"\"c:\\windows\\system32\\gameinputsvc.exe\"";
        s.processes[1].name = L"GAMEINPUTSVC.EXE";
        std::string error; CHECK(Qualify(s, 60, true, error));
    });
    test("unknown zero manager and self peer IDs rejected", [] {
        for (DWORD id : {0u, 50u, 70u, 99u}) { FakeSource s; Reject(s, id); }
    });
    test("wrong parent and wrong session rejected", [] {
        FakeSource a; a.processes[1].parent = 42; Reject(a);
        FakeSource b; b.processes[1].session = 2; Reject(b);
        FakeSource c; c.processes[0].session = 1; Reject(c);
    });
    test("reused manager PID and zero birth times rejected", [] {
        FakeSource a; a.processes[0].created = 2500; Reject(a);
        FakeSource b; b.processes[0].created = 0; Reject(b);
    });
    test("missing duplicate or unexpected process names rejected", [] {
        FakeSource a; a.processes.erase(a.processes.begin()); Reject(a);
        FakeSource b; b.processes.push_back(b.processes[1]); Reject(b);
        FakeSource c; c.processes[1].name = L"Other.exe"; Reject(c);
    });
    test("process identity changed during hashes rejected", [] {
        FakeSource s; s.secondProcesses = [](FakeSource& value) { ++value.processes[1].created; }; Reject(s);
    });
    test("service PID state and registry metadata changes rejected", [] {
        for (int i = 0; i < 3; ++i) {
            FakeSource s; s.secondService = [i](FakeSource& v) {
                if (i == 0) { ++v.service.pid; } else if (i == 1) { v.service.state = SERVICE_STOPPED; } else { ++v.service.registryWritten; }
            }; Reject(s);
        }
    });
    test("connected validation never accepts a stopped service", [] {
        FakeSource s; s.service.state = SERVICE_STOPPED; Reject(s);
    });
    test("native process prefix yields owned identity data", [] {
        auto bytes = ProcessBuffer(); auto records = ParseProcesses(bytes.data(), bytes.size());
        CHECK(records.size() == 1 && records[0].pid == 60 && records[0].parent == 50 && records[0].created == 2000);
        std::fill(bytes.begin(), bytes.end(), BYTE{0}); CHECK(records[0].name == L"GameInputSvc.exe");
    });
    test("truncated chain and out-of-buffer image names rejected", [] {
        for (int kind = 0; kind < 5; ++kind) {
            auto bytes = ProcessBuffer(); NativeProcessPrefix value{}; std::memcpy(&value, bytes.data(), sizeof(value));
            std::size_t size = bytes.size();
            if (kind == 0) { size = sizeof(value) - 1; }
            if (kind == 1) { value.nextEntryOffset = 1; }
            if (kind == 2) { value.nextEntryOffset = static_cast<ULONG>(size + 1); }
            if (kind == 3) { value.imageName.buffer = reinterpret_cast<PWSTR>(1); }
            if (kind == 4) { ++value.imageName.length; }
            std::memcpy(bytes.data(), &value, sizeof(value));
            bool rejected = false;
            try { ParseProcesses(bytes.data(), size); } catch (std::exception const&) { rejected = true; }
            CHECK(rejected);
        }
    });
    std::printf("RESULT %u passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
