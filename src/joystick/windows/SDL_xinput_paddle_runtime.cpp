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
#include <bcrypt.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sdl_paddles {
namespace runtime_detail {

static_assert(sizeof(void*) == 8, "The qualified runtime is native x64");

// One complete measured installation profile. In particular, the service EXE
// tries the System32 redist candidate before its inbox candidate. These hashes
// qualify this compatible family on disk, not the bytes in a running process.
struct ProfileFile { const wchar_t* name; std::int64_t size; const char* sha256; };
constexpr std::array<ProfileFile, 5> Profile{{
    {L"GameInputSvc.exe", 80336, "83ba5166dd6397ade67f4b6fddae6be61a694f487ed88ea83ee204f20be6d075"},
    {L"GameInput.dll", 424376, "f9f5adbde4a2883b9b307bb1848a120094904578d92b21100828bd83f7b77cb6"},
    {L"GameInputRedist.dll", 1155456, "3f9f7e49868639f00ff1942bb3a4d9bc513c6940a8e43e1b5c8c1eba765060ce"},
    {L"Windows.Gaming.Input.dll", 864256, "2d319bd941977a536c91929d5cf46586bf58a58a2c0c33bda279af9798742491"},
    {L"drivers\\xboxgip.sys", 421888, "ea57e827b3b739d89f68cb41460c3466eac0c9d2be128cd9f7eea6654a816db7"},
}};

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
    virtual FileStamp OpenFile(std::size_t index) = 0;
    virtual std::string HashFile(std::size_t index, std::int64_t expectedSize) = 0;
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
        // Snapshot and retain all five shared handles before hashing any file.
        for (std::size_t i = 0; i < Profile.size(); ++i) {
            stamps[i] = source.OpenFile(i);
            Require(stamps[i].size == Profile[i].size &&
                !(stamps[i].attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)),
                "The installed USB runtime file profile is not supported.");
        }
        for (std::size_t i = 0; i < Profile.size(); ++i) {
            Require(source.HashFile(i, stamps[i].size) == Profile[i].sha256,
                "The installed USB runtime does not match a supported complete file profile.");
        }
        // Recheck every file after the LAST digest, including a path reopen.
        // This also catches replacement of an earlier file while a later file
        // was being hashed. Handles permit write/delete sharing throughout.
        for (std::size_t i = 0; i < Profile.size(); ++i) {
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
struct HashHandles {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ~HashHandles()
    {
        if (hash) { BCryptDestroyHash(hash); }
        if (algorithm) { BCryptCloseAlgorithmProvider(algorithm, 0); }
    }
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

    FileStamp OpenFile(std::size_t index) override
    {
        files_[index].value = OpenPath(index, GENERIC_READ);
        return Stamp(files_[index].value);
    }

    std::string HashFile(std::size_t index, std::int64_t expectedSize) override
    {
        HashHandles handles;
        Require(BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
            "Cannot initialize the runtime SHA256 provider.");
        Require(BCryptCreateHash(handles.algorithm, &handles.hash, nullptr, 0, nullptr, 0, 0) >= 0,
            "Cannot create the runtime SHA256 state.");
        std::array<BYTE, 65536> bytes{};
        std::int64_t total = 0;
        for (;;) {
            DWORD read = 0;
            WinCheck(::ReadFile(files_[index].value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr), "Cannot read the runtime file");
            if (!read) { break; }
            Require(read <= bytes.size() && total <= expectedSize && read <= expectedSize - total,
                "The runtime file grew during its digest read.");
            total += read;
            Require(BCryptHashData(handles.hash, bytes.data(), read, 0) >= 0, "Cannot hash the runtime file.");
        }
        Require(total == expectedSize, "The runtime file digest read was incomplete.");
        std::array<BYTE, 32> digest{};
        Require(BCryptFinishHash(handles.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0,
            "Cannot finish the runtime file digest.");
        constexpr char Hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (auto value : digest) { result.push_back(Hex[value >> 4]); result.push_back(Hex[value & 15]); }
        return result;
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

    HANDLE OpenPath(std::size_t index, DWORD access)
    {
        auto path = system_ + L"\\" + Profile[index].name;
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

// Compatibility assumes the trusted loader's resident provider belongs to this
// reviewed family. Stable disk metadata cannot prove historical mapped bytes.
// Birth times reject PID reuse, not ordinary post-start metadata changes.
// The Client still owns layout, attachment, and connection-generation checks.
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
