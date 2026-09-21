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

// A version inside each family, and a size for each stamp. The redist is the
// bench's 3.3.221.0, and the others are the bench's Windows builds.
static constexpr std::array<FileVersion, Profile.size()> KnownVersions{{
    {0, 2309, 26100, 9278}, {0, 2309, 26100, 9278}, {3, 3, 221, 0}, {10, 0, 26100, 9278}, {10, 0, 26100, 8972}}};
static constexpr std::array<std::int64_t, Profile.size()> KnownSizes{{80336, 424376, 1155456, 864256, 421888}};
static constexpr std::size_t Redist = 2;
static_assert(!Profile[Redist].required && Profile[0].required && Profile[1].required && Profile[3].required && Profile[4].required);

struct FakeSource : Source {
    Service service{50, SERVICE_RUNNING, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
        L"C:\\Windows\\System32\\GameInputSvc.exe", L"LocalSystem", 15};
    std::vector<Process> processes{{50, 4, 0, 1000, L"GameInputSvc.exe"},
        {60, 50, 1, 2000, L"GameInputSvc.exe"}, {70, 42, 1, 3000, L"test.exe"}};
    std::array<FileStamp, Profile.size()> stamps{};
    std::array<FileVersion, Profile.size()> versions = KnownVersions;
    std::array<bool, Profile.size()> present{{true, true, true, true, true}};
    std::function<void(FakeSource&)> afterVersions;
    std::function<void(FakeSource&)> secondProcesses;
    std::function<void(FakeSource&)> secondService;
    int openFailure = -1, versionFailure = -1, changedPath = -1;
    unsigned serviceReads = 0, processReads = 0, opens = 0, reads = 0, checks = 0, presenceQueries = 0;
    FakeSource()
    {
        for (std::size_t i = 0; i < Profile.size(); ++i) {
            stamps[i] = {1, {}, KnownSizes[i], 900, 4000, FILE_ATTRIBUTE_ARCHIVE};
            stamps[i].id[0] = static_cast<std::uint8_t>(i + 1);
        }
    }
    unsigned Expected() const
    {
        unsigned count = 0;
        for (std::size_t i = 0; i < Profile.size(); ++i) { count += Profile[i].required || present[i]; }
        return count;
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
    bool Present(std::size_t i) override
    {
        CHECK(!Profile[i].required); ++presenceQueries;
        return present[i];
    }
    FileStamp OpenFile(std::size_t i) override
    {
        CHECK(reads == 0); ++opens;
        if (static_cast<int>(i) == openFailure) { throw std::runtime_error("unreadable file"); }
        return stamps[i];
    }
    FileVersion ReadVersion(std::size_t i) override
    {
        CHECK(opens == Expected());
        ++reads;
        if (static_cast<int>(i) == versionFailure) { throw std::runtime_error("no version resource"); }
        if (reads == Expected() && afterVersions) { afterVersions(*this); }
        return versions[i];
    }
    FileStamp HandleStamp(std::size_t i) override { CHECK(reads == Expected()); ++checks; return stamps[i]; }
    FileStamp PathStamp(std::size_t i) override
    {
        CHECK(reads == Expected()); ++checks;
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
        CHECK(s.reads == Profile.size() && s.checks == 2 * Profile.size() && s.processReads == 2 && s.serviceReads == 2);
        CHECK(s.presenceQueries == 1);
    });
    test("every later revision inside a family is accepted", [] {
        FakeSource s; std::string error;
        s.versions[0].revision = 9457; s.versions[1].revision = 9457; s.versions[3].revision = 9500; s.versions[4].revision = 9999;
        CHECK(Qualify(s, 60, true, error) && error.empty());
    });
    test("the ARM64 update set is accepted", [] {
        FakeSource s; std::string error;
        s.versions = {{{0, 2309, 26100, 9457}, {0, 2309, 26100, 9457}, {3, 5, 270, 0}, {10, 0, 26100, 9278}, {10, 0, 26100, 8972}}};
        s.stamps[Redist].size = 1921152;
        CHECK(Qualify(s, 60, true, error) && error.empty());
    });
    test("a revision below the floor is rejected with the file and version named", [] {
        FakeSource s; s.versions[1].revision = 7000; std::string error;
        CHECK(!Qualify(s, 60, true, error));
        CHECK(error.find("GameInput.dll 0.2309.26100.7000") != std::string::npos);
        FakeSource a; a.versions[0].revision = 8874; Reject(a);
        FakeSource b; b.versions[3].revision = 8736; Reject(b);
        FakeSource c; c.versions[4].revision = 8971; Reject(c);
        FakeSource d; d.versions[0].revision = 8875; d.versions[1].revision = 8875; d.versions[3].revision = 8737;
        std::string ok; CHECK(Qualify(d, 60, true, ok));
    });
    test("another major minor or build is rejected for a required file", [] {
        FakeSource a; a.versions[0].major = 1; Reject(a);
        FakeSource b; b.versions[1].minor = 2310; Reject(b);
        FakeSource c; c.versions[3].build = 22621; Reject(c);
        FakeSource d; d.versions[4].build = 26200; Reject(d);
    });
    test("the redist is optional and is checked by major part when present", [] {
        FakeSource absent; absent.present[Redist] = false; std::string error;
        CHECK(Qualify(absent, 60, true, error) && error.empty());
        CHECK(absent.opens == Profile.size() - 1 && absent.reads == Profile.size() - 1 && absent.checks == 2 * (Profile.size() - 1));
        FakeSource newer; newer.versions[Redist] = {3, 9, 1, 5}; CHECK(Qualify(newer, 60, true, error));
        FakeSource older; older.versions[Redist] = {2, 9, 0, 0}; Reject(older);
        FakeSource future; future.versions[Redist] = {4, 0, 0, 0}; Reject(future);
        FakeSource unreadable; unreadable.present[Redist] = true; unreadable.openFailure = Redist; Reject(unreadable);
    });
    test("post-start metadata change time is not image proof or rejection", [] {
        FakeSource s; std::string error; s.stamps[2].changed = 5000;
        CHECK(Qualify(s, 60, true, error));
    });
    test("pre-connect installation check permits a stopped enabled service", [] {
        FakeSource s; s.service.state = SERVICE_STOPPED; s.service.pid = 0; s.processes.clear();
        std::string error; CHECK(Qualify(s, 0, false, error)); CHECK(s.processReads == 0);
    });
    test("missing files and unreadable versions fail closed", [] {
        for (int i = 0; i < static_cast<int>(Profile.size()); ++i) {
            FakeSource a; a.openFailure = i; Reject(a);
            FakeSource b; b.versionFailure = i; Reject(b);
        }
    });
    test("empty file directory and reparse point fail", [] {
        FakeSource a; a.stamps[0].size = 0; Reject(a);
        FakeSource b; b.stamps[1].attributes |= FILE_ATTRIBUTE_DIRECTORY; Reject(b);
        FakeSource c; c.stamps[2].attributes |= FILE_ATTRIBUTE_REPARSE_POINT; Reject(c);
    });
    test("earlier file change after final version read is detected", [] {
        FakeSource s; s.afterVersions = [](FakeSource& value) { ++value.stamps[0].changed; }; Reject(s);
    });
    test("path replacement detected even when original handle is stable", [] {
        FakeSource s; s.changedPath = 0; Reject(s);
    });
    test("file identity volume and write timestamp changes detected", [] {
        for (int i = 0; i < 3; ++i) {
            FakeSource s; s.afterVersions = [i](FakeSource& v) {
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
    test("process identity changed during version reads rejected", [] {
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
