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
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "SDL_xinput_paddle_runtime.h"
#include <windows.h>
#include <winsvc.h>
#include <winver.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sdl_paddles {
namespace runtime_detail {

static_assert(sizeof(void*) == 8, "The qualified runtime is 64-bit");

// The installed files whose private format the reader depends on, qualified
// by version family instead of by exact bytes, so a servicing update inside
// the family keeps the route on. The service EXE loads the System32 redist
// candidate when it exists and its inbox DLL otherwise, so the redist is
// optional and is checked only when present. A file is inside its family
// when the major, minor and build parts of its product version match and
// its revision is at least the floor. For the two GameInput files the floor
// is 8875, the lowest revision whose pool routines were read and matched,
// on the ARM64 build. The x64 read was 9278. For the other two it is the
// lowest build read. The format itself is checked again on every view by
// the reader's layout validation, which is the gate that catches a build
// that changed the format. The product version carries no architecture, so
// x64 and ARM64 share this table.
struct FileFamily { const wchar_t* name; bool required; unsigned major, minor, build, revisionFloor; };
constexpr std::array<FileFamily, 5> Profile{{
    {L"GameInputSvc.exe", true, 0, 2309, 26100, 8875},
    {L"GameInput.dll", true, 0, 2309, 26100, 8875},
    {L"GameInputRedist.dll", false, 3, 0, 0, 0},
    {L"Windows.Gaming.Input.dll", true, 10, 0, 26100, 8737},
    {L"drivers\\xboxgip.sys", true, 10, 0, 26100, 8972},
}};

struct FileVersion {
    unsigned major = 0, minor = 0, build = 0, revision = 0;
    bool operator==(FileVersion const&) const = default;
};

// The redist family pins its major part alone. Its minor and build change
// with every redistributable release, and the reader's layout validation
// decides whether a given release keeps the format.
static bool InFamily(FileVersion const& version, FileFamily const& family)
{
    if (version.major != family.major) { return false; }
    if (!family.required) { return true; }
    return version.minor == family.minor && version.build == family.build && version.revision >= family.revisionFloor;
}

static std::string Describe(FileFamily const& family, FileVersion const& version)
{
    std::string name;
    for (const wchar_t* c = family.name; *c; ++c) { name.push_back(*c < 128 ? static_cast<char>(*c) : '?'); }
    return name + " " + std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
        std::to_string(version.build) + "." + std::to_string(version.revision);
}

static void Require(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

static void WinCheck(BOOL result, const char* message)
{
    if (!result) { throw std::runtime_error(std::string(message) + " (Windows error " + std::to_string(GetLastError()) + ")"); }
}

static bool SameText(std::wstring const& a, std::wstring const& b)
{
    return a.size() == b.size() && a.size() <= 32767 &&
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

struct FileStamp {
    std::uint64_t volume = 0;
    std::array<std::uint8_t, 16> id{};
    std::int64_t size = 0, written = 0, changed = 0;
    DWORD attributes = 0;
    bool operator==(FileStamp const&) const = default;
};

struct Service {
    DWORD pid = 0, state = 0, type = 0, startType = 0;
    std::wstring path, account;
    std::uint64_t registryWritten = 0;
    bool operator==(Service const&) const = default;
};

struct Process {
    DWORD pid = 0, parent = 0, session = 0;
    std::int64_t created = 0;
    std::wstring name;
    bool operator==(Process const&) const = default;
};

// SYSTEM_PROCESS_INFORMATION's fixed prefix through SessionId, from phnt
// ntexapi.h at 53fbbdc5b5d2b08761db1c7b26bfa8c820924356. The private ABI test
// compares every consumed field against that header. No remote memory is read.
// Remaining counters and thread records are skipped with NextEntryOffset.
struct NativeString { USHORT length, maximumLength; PWSTR buffer; };
struct NativeProcessPrefix {
    ULONG nextEntryOffset, numberOfThreads;
    ULONGLONG workingSetPrivateSize;
    ULONG hardFaultCount, numberOfThreadsHighWatermark;
    ULONGLONG cycleTime;
    LARGE_INTEGER createTime, userTime, kernelTime;
    NativeString imageName;
    LONG basePriority;
    HANDLE uniqueProcessId, inheritedFromUniqueProcessId;
    ULONG handleCount, sessionId;
};
// The same on native x64 and native ARM64. A layout change stops the build.
static_assert(sizeof(NativeString) == 0x10 && offsetof(NativeString, buffer) == 8);
static_assert(sizeof(NativeProcessPrefix) == 0x68 && offsetof(NativeProcessPrefix, createTime) == 0x20 &&
    offsetof(NativeProcessPrefix, imageName) == 0x38 && offsetof(NativeProcessPrefix, basePriority) == 0x48 &&
    offsetof(NativeProcessPrefix, uniqueProcessId) == 0x50 &&
    offsetof(NativeProcessPrefix, inheritedFromUniqueProcessId) == 0x58 &&
    offsetof(NativeProcessPrefix, handleCount) == 0x60 && offsetof(NativeProcessPrefix, sessionId) == 0x64);

static std::vector<Process> ParseProcesses(const BYTE* bytes, std::size_t size)
{
    constexpr std::size_t MaxRecords = 65536;
    const auto base = reinterpret_cast<std::uintptr_t>(bytes);
    std::vector<Process> records;
    Require(bytes && size >= sizeof(NativeProcessPrefix), "The process metadata is truncated.");
    for (std::size_t offset = 0;;) {
        Require(records.size() < MaxRecords && size - offset >= sizeof(NativeProcessPrefix) &&
            offset % alignof(void*) == 0, "The process metadata record is invalid.");
        NativeProcessPrefix entry{};
        std::memcpy(&entry, bytes + offset, sizeof(entry));
        Require(entry.nextEntryOffset == 0 ||
            (entry.nextEntryOffset >= sizeof(entry) && entry.nextEntryOffset <= size - offset),
            "The process metadata chain is invalid.");
        auto id = reinterpret_cast<std::uintptr_t>(entry.uniqueProcessId);
        auto parent = reinterpret_cast<std::uintptr_t>(entry.inheritedFromUniqueProcessId);
        Require(id <= MAXDWORD && parent <= MAXDWORD, "The process identifier is invalid.");
        Process record{static_cast<DWORD>(id), static_cast<DWORD>(parent), entry.sessionId, entry.createTime.QuadPart, {}};
        if (entry.imageName.length) {
            const auto pointer = reinterpret_cast<std::uintptr_t>(entry.imageName.buffer);
            Require(entry.imageName.length % sizeof(wchar_t) == 0 &&
                entry.imageName.length <= entry.imageName.maximumLength && pointer >= base &&
                pointer % alignof(wchar_t) == 0 && pointer - base <= size && entry.imageName.length <= size - (pointer - base),
                "The process image name is outside the metadata buffer.");
            record.name.assign(entry.imageName.buffer, entry.imageName.length / sizeof(wchar_t));
            Require(record.name.find(L'\0') == std::wstring::npos, "The process image name contains a null character.");
        }
        records.push_back(std::move(record));
        if (!entry.nextEntryOffset) { return records; }
        offset += entry.nextEntryOffset;
    }
}

struct Source {
    virtual ~Source() = default;
    virtual std::wstring SystemDirectory() const = 0;
    virtual DWORD SelfPid() const = 0;
    virtual Service ReadService() = 0;
    virtual std::vector<Process> ReadProcesses() = 0;
    virtual bool Present(std::size_t index) = 0;
    virtual FileStamp OpenFile(std::size_t index) = 0;
    virtual FileVersion ReadVersion(std::size_t index) = 0;
    virtual FileStamp HandleStamp(std::size_t index) = 0;
    virtual FileStamp PathStamp(std::size_t index) = 0;
};

static Process FindProcess(std::vector<Process> const& records, DWORD pid)
{
    const Process* match = nullptr;
    for (auto const& record : records) {
        if (record.pid == pid) {
            Require(!match, "The process identity is ambiguous.");
            match = &record;
        }
    }
    Require(match && pid != 0, "The process identity is unavailable.");
    return *match;
}

static std::array<Process, 3> CheckPeer(std::vector<Process> const& records, Service const& service, DWORD peer, DWORD self)
{
    Require(service.state == SERVICE_RUNNING && service.pid && peer && peer != self && peer != service.pid,
        "The input server is not a separate running service instance.");
    auto manager = FindProcess(records, service.pid);
    auto server = FindProcess(records, peer);
    auto caller = FindProcess(records, self);
    Require(SameText(manager.name, L"GameInputSvc.exe") && SameText(server.name, L"GameInputSvc.exe"),
        "The input server process family is not supported.");
    Require(manager.session == 0 && server.session == caller.session && server.parent == manager.pid,
        "The input server is not the service manager's instance in this session.");
    Require(manager.created > 0 && server.created >= manager.created && caller.created > 0,
        "The input server creation metadata is invalid or its parent PID was reused.");
    return {std::move(manager), std::move(server), std::move(caller)};
}

static void CheckService(Service const& service, std::wstring const& system)
{
    auto path = service.path;
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') { path = path.substr(1, path.size() - 2); }
    Require(SameText(path, system + L"\\GameInputSvc.exe") && SameText(service.account, L"LocalSystem") &&
        service.type == SERVICE_WIN32_OWN_PROCESS && service.startType != SERVICE_DISABLED,
        "The installed input service configuration is not supported.");
    Require(service.state == SERVICE_RUNNING || service.state == SERVICE_STOPPED,
        "The input service is changing state.");
}

static bool Qualify(Source& source, DWORD peer, bool connected, std::string& error)
{
    error.clear();
    try {
        auto before = source.ReadService();
        CheckService(before, source.SystemDirectory());
        std::array<Process, 3> processes{};
        if (connected) { processes = CheckPeer(source.ReadProcesses(), before, peer, source.SelfPid()); }
        std::array<FileStamp, Profile.size()> stamps{};
        std::array<bool, Profile.size()> present{};
        // Snapshot and retain every shared handle before reading any version.
        for (std::size_t i = 0; i < Profile.size(); ++i) {
            present[i] = Profile[i].required || source.Present(i);
            if (!present[i]) { continue; }
            stamps[i] = source.OpenFile(i);
            Require(stamps[i].size > 0 &&
                !(stamps[i].attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)),
                "The installed USB runtime file profile is not supported.");
        }
        for (std::size_t i = 0; i < Profile.size(); ++i) {
            if (!present[i]) { continue; }
            const auto version = source.ReadVersion(i);
            if (!InFamily(version, Profile[i])) {
                throw std::runtime_error("The installed USB runtime is outside the qualified family: " + Describe(Profile[i], version) + ".");
            }
        }
        // Recheck every file after the LAST read, including a path reopen.
        // This also catches replacement of an earlier file while a later file
        // was being read. Handles permit write/delete sharing throughout.
        for (std::size_t i = 0; i < Profile.size(); ++i) {
            if (!present[i]) { continue; }
            Require(source.HandleStamp(i) == stamps[i] && source.PathStamp(i) == stamps[i],
                "The USB runtime files changed during qualification.");
        }
        if (connected) {
            Require(CheckPeer(source.ReadProcesses(), before, peer, source.SelfPid()) == processes,
                "The input server process identities changed during qualification.");
        }
        Require(source.ReadService() == before, "The input service configuration changed during qualification.");
        return true;
    } catch (std::exception const& failure) {
        error = failure.what();
        return false;
    } catch (...) {
        error = "The USB runtime metadata could not be read.";
        return false;
    }
}

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
};
struct ServiceHandle {
    SC_HANDLE value = nullptr;
    ~ServiceHandle() { if (value) { CloseServiceHandle(value); } }
};
struct RegistryHandle {
    HKEY value = nullptr;
    ~RegistryHandle() { if (value) { RegCloseKey(value); } }
};
class WindowsSource final : public Source {
public:
    WindowsSource()
    {
        std::array<wchar_t, 32768> path{};
        const UINT length = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
        Require(length && length < path.size(), "The Windows system directory is unavailable.");
        system_.assign(path.data(), length);
    }
    std::wstring SystemDirectory() const override { return system_; }
    DWORD SelfPid() const override { return GetCurrentProcessId(); }

    Service ReadService() override
    {
        ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
        WinCheck(manager.value != nullptr, "Cannot query the service manager");
        ServiceHandle handle{OpenServiceW(manager.value, L"GameInputSvc", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS)};
        WinCheck(handle.value != nullptr, "Cannot query the input service");
        SERVICE_STATUS_PROCESS status{}; DWORD size = 0;
        WinCheck(QueryServiceStatusEx(handle.value, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &size),
            "Cannot query the input service status");
        DWORD required = 0;
        BOOL queried = QueryServiceConfigW(handle.value, nullptr, 0, &required);
        Require(!queried && GetLastError() == ERROR_INSUFFICIENT_BUFFER && required >= sizeof(QUERY_SERVICE_CONFIGW) && required <= 65536,
            "The input service configuration size is invalid.");
        std::vector<BYTE> storage(required);
        auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
        WinCheck(QueryServiceConfigW(handle.value, config, required, &required), "Cannot read the input service configuration");
        auto text = [&](const wchar_t* value) {
            auto base = reinterpret_cast<std::uintptr_t>(storage.data());
            auto pointer = reinterpret_cast<std::uintptr_t>(value);
            Require(value && pointer >= base && pointer - base < storage.size() && pointer % alignof(wchar_t) == 0,
                "The input service configuration string is invalid.");
            const auto limit = (storage.size() - (pointer - base)) / sizeof(wchar_t);
            const auto length = wcsnlen_s(value, limit);
            Require(length < limit, "The input service configuration string is truncated.");
            return std::wstring(value, length);
        };
        RegistryHandle key;
        Require(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\GameInputSvc", 0, KEY_QUERY_VALUE, &key.value) == ERROR_SUCCESS,
            "Cannot read the input service registry metadata.");
        FILETIME written{};
        Require(RegQueryInfoKeyW(key.value, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &written) == ERROR_SUCCESS,
            "Cannot query the input service registry timestamp.");
        return {status.dwProcessId, status.dwCurrentState, config->dwServiceType, config->dwStartType,
            text(config->lpBinaryPathName), text(config->lpServiceStartName),
            (static_cast<std::uint64_t>(written.dwHighDateTime) << 32) | written.dwLowDateTime};
    }

    std::vector<Process> ReadProcesses() override
    {
        using Query = LONG (NTAPI*)(ULONG, PVOID, ULONG, PULONG);
        auto module = GetModuleHandleW(L"ntdll.dll");
        Require(module != nullptr, "The system metadata query module is unavailable.");
        auto address = GetProcAddress(module, "NtQuerySystemInformation");
        Query query = nullptr;
        static_assert(sizeof(query) == sizeof(address));
        std::memcpy(&query, &address, sizeof(query));
        Require(query != nullptr, "The system process metadata query is unavailable.");
        constexpr ULONG ProcessInformation = 5; // phnt SYSTEM_INFORMATION_CLASS
        constexpr ULONG Maximum = 16 * 1024 * 1024;
        ULONG capacity = 65536;
        for (unsigned attempt = 0; attempt < 8 && capacity <= Maximum; ++attempt) {
            std::vector<BYTE> bytes(capacity); ULONG returned = 0;
            const LONG result = query(ProcessInformation, bytes.data(), capacity, &returned);
            if (static_cast<ULONG>(result) == 0xc0000004 || static_cast<ULONG>(result) == 0xc0000023) {
                capacity = returned > capacity ? returned : capacity * 2;
                continue;
            }
            Require(result >= 0 && returned && returned <= bytes.size(), "The system process metadata query failed.");
            return ParseProcesses(bytes.data(), returned);
        }
        throw std::runtime_error("The system process metadata exceeded its bounded query budget.");
    }

    // Only a file that does not exist counts as absent. Any other failure to
    // read its attributes surfaces when the file is opened.
    bool Present(std::size_t index) override
    {
        const auto path = Path(index);
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) { return true; }
        const DWORD error = GetLastError();
        return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
    }

    FileStamp OpenFile(std::size_t index) override
    {
        files_[index].value = OpenPath(index, GENERIC_READ);
        return Stamp(files_[index].value);
    }

    // The product version from the fixed block of the file's version resource.
    // The file version's major and minor parts of the two Windows components
    // differ by architecture (6.2 on x64, 10.0 on ARM64) while their product
    // version is 10.0 on both. The GameInput files carry the same value in both
    // fields. The file stays open through the read, and the stamps around it
    // detect a replacement.
    FileVersion ReadVersion(std::size_t index) override
    {
        const auto path = Path(index);
        DWORD ignored = 0;
        const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &ignored);
        WinCheck(size != 0, "Cannot size the runtime file version resource");
        Require(size <= 1048576, "The runtime file version resource is too large.");
        std::vector<BYTE> block(size);
        WinCheck(GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), 0, size, block.data()), "Cannot read the runtime file version resource");
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT length = 0;
        WinCheck(VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &length), "Cannot query the runtime file version");
        Require(fixed && length >= sizeof(VS_FIXEDFILEINFO) && fixed->dwSignature == 0xFEEF04BD,
            "The runtime file version block is invalid.");
        return {fixed->dwProductVersionMS >> 16, fixed->dwProductVersionMS & 0xFFFF,
            fixed->dwProductVersionLS >> 16, fixed->dwProductVersionLS & 0xFFFF};
    }

    FileStamp HandleStamp(std::size_t index) override { return Stamp(files_[index].value); }
    FileStamp PathStamp(std::size_t index) override
    {
        FileHandle file{OpenPath(index, FILE_READ_ATTRIBUTES)};
        return Stamp(file.value);
    }

private:
    std::wstring system_;
    std::array<FileHandle, Profile.size()> files_{};

    std::wstring Path(std::size_t index) const { return system_ + L"\\" + Profile[index].name; }

    HANDLE OpenPath(std::size_t index, DWORD access)
    {
        const auto path = Path(index);
        HANDLE result = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        WinCheck(result != INVALID_HANDLE_VALUE, "Cannot open the runtime file");
        return result;
    }

    static FileStamp Stamp(HANDLE file)
    {
        FILE_ID_INFO id{}; FILE_BASIC_INFO basic{}; FILE_STANDARD_INFO standard{};
        WinCheck(GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)), "Cannot read the runtime file identity");
        WinCheck(GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)), "Cannot read the runtime file timestamps");
        WinCheck(GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)), "Cannot read the runtime file size");
        Require(!standard.Directory && standard.EndOfFile.QuadPart >= 0, "The runtime path is not a regular file.");
        FileStamp result{id.VolumeSerialNumber, {}, standard.EndOfFile.QuadPart,
            basic.LastWriteTime.QuadPart, basic.ChangeTime.QuadPart, basic.FileAttributes};
        std::memcpy(result.id.data(), id.FileId.Identifier, result.id.size());
        return result;
    }
};

// Compatibility assumes the trusted loader's resident provider belongs to the
// installed family. Disk metadata cannot prove historical mapped bytes. Birth
// times reject PID reuse, not ordinary post-start metadata changes. The Client
// owns the layout, attachment, and connection-generation checks, and those
// are what reject a build inside the family that changed the format.
static bool Run(DWORD peer, bool connected, std::string& error)
{
    try {
        WindowsSource source;
        return Qualify(source, peer, connected, error);
    } catch (std::exception const& failure) { error = failure.what(); return false; }
    catch (...) { error = "The USB runtime metadata could not be collected."; return false; }
}

} // namespace runtime_detail

bool PaddleUsbRuntimeAvailable(std::string& error) { return runtime_detail::Run(0, false, error); }
bool PaddleValidateServer(std::uint32_t pid, std::string& error) { return runtime_detail::Run(pid, true, error); }

} // namespace sdl_paddles
