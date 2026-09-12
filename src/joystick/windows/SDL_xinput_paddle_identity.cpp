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
#include "SDL_xinput_paddle_identity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>

#include <array>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "advapi32.lib")

namespace sdl_paddles {
namespace {

constexpr size_t MaxPathChars = 1024;
constexpr size_t MaxPropertyChars = 4096;
constexpr size_t MaxParents = 16;
constexpr size_t MaxChildren = 256;
constexpr GUID SystemContainer = {0, 0, 0, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
constexpr std::wstring_view SyntheticFamily = L"USB\\VID_045E&PID_";
constexpr std::wstring_view BleHidId = L"BTHLEDEVICE\\{00001812-0000-1000-8000-00805F9B34FB}";

bool EqualText(std::wstring_view a, std::wstring_view b) noexcept
{
    if (a.size() != b.size() || a.size() > MaxPropertyChars) {
        return false;
    }
    return a.empty() || CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                             b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool StartsWith(std::wstring_view text, std::wstring_view prefix) noexcept
{
    return text.size() >= prefix.size() && EqualText(text.substr(0, prefix.size()), prefix);
}

bool HasMarker(std::wstring_view text) noexcept
{
    constexpr std::wstring_view marker = L"HIDMAESTRO";
    for (size_t i = 0; i + marker.size() <= text.size(); ++i) {
        if (EqualText(text.substr(i, marker.size()), marker)) {
            return true;
        }
    }
    return false;
}

bool ValidContainer(const GUID &value) noexcept
{
    constexpr GUID zero{};
    return !IsEqualGUID(value, zero) && !IsEqualGUID(value, SystemContainer);
}

bool ValidInstance(std::wstring_view value) noexcept
{
    if (value.empty() || value.size() >= MAX_DEVICE_ID_LEN) {
        return false;
    }
    size_t separators = 0;
    bool empty = true;
    for (wchar_t c : value) {
        if (c < 0x21 || c > 0x7e || c == L'/' || c == L',') {
            return false;
        }
        if (c == L'\\') {
            if (empty) {
                return false;
            }
            ++separators;
        }
        empty = c == L'\\';
    }
    return separators == 2 && !empty;
}

// Keep the returned byte count and type separate from the initialized buffer.
// A missing terminator must not be supplied by the buffer's unused tail.
struct Property {
    std::array<wchar_t, MaxPropertyChars> text{};
    ULONG bytes = static_cast<ULONG>(sizeof(text));
    ULONG type = 0;
};

// Tests replace this metadata boundary and execute the production parser,
// ancestry walk, qualification, and recheck without opening a device.
struct MetadataSource {
    virtual ~MetadataSource() = default;
    virtual CONFIGRET Interface(const wchar_t *path, Property &property) = 0;
    virtual CONFIGRET Locate(std::wstring &instance, DEVINST &node) = 0;
    virtual CONFIGRET Read(DEVINST node, const DEVPROPKEY &key, Property &property) = 0;
    virtual CONFIGRET Parent(DEVINST node, DEVINST &parent) = 0;
    virtual CONFIGRET Child(DEVINST node, DEVINST &child) = 0;
    virtual CONFIGRET Sibling(DEVINST node, DEVINST &sibling) = 0;
    virtual bool DriverImage(Property &property) = 0;
    virtual bool Directories(std::wstring &windows, std::wstring &system) = 0;
};

struct WindowsMetadata final : MetadataSource {
    CONFIGRET Interface(const wchar_t *path, Property &p) override
    {
        return CM_Get_Device_Interface_PropertyW(path, &DEVPKEY_Device_InstanceId,
                                                 &p.type, reinterpret_cast<PBYTE>(p.text.data()), &p.bytes, 0);
    }
    CONFIGRET Locate(std::wstring &instance, DEVINST &node) override
    {
        return CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_NORMAL);
    }
    CONFIGRET Read(DEVINST node, const DEVPROPKEY &key, Property &p) override
    {
        return CM_Get_DevNode_PropertyW(node, &key, &p.type,
                                       reinterpret_cast<PBYTE>(p.text.data()), &p.bytes, 0);
    }
    CONFIGRET Parent(DEVINST node, DEVINST &parent) override { return CM_Get_Parent(&parent, node, 0); }
    CONFIGRET Child(DEVINST node, DEVINST &child) override { return CM_Get_Child(&child, node, 0); }
    CONFIGRET Sibling(DEVINST node, DEVINST &sibling) override { return CM_Get_Sibling(&sibling, node, 0); }
    bool DriverImage(Property &p) override
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\xboxgip",
                          0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
            return false;
        }
        const LSTATUS result = RegQueryValueExW(key, L"ImagePath", nullptr, &p.type,
                                               reinterpret_cast<LPBYTE>(p.text.data()), &p.bytes);
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }
    bool Directories(std::wstring &windows, std::wstring &system) override
    {
        std::array<wchar_t, MaxPathChars> buffer{};
        UINT size = GetSystemWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        if (size == 0 || size >= buffer.size()) {
            return false;
        }
        windows.assign(buffer.data(), size);
        size = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        if (size == 0 || size >= buffer.size()) {
            return false;
        }
        system.assign(buffer.data(), size);
        return true;
    }
};

bool ReadString(const Property &p, ULONG type, size_t capacity, std::wstring &value)
{
    if (p.type != type || p.bytes < sizeof(wchar_t) || p.bytes > sizeof(p.text) ||
        p.bytes % sizeof(wchar_t) != 0) {
        return false;
    }
    const size_t count = p.bytes / sizeof(wchar_t);
    if (count > capacity || p.text[count - 1] != L'\0') {
        return false;
    }
    for (size_t i = 0; i + 1 < count; ++i) {
        if (p.text[i] < 0x20 || (p.text[i] >= 0xd800 && p.text[i] <= 0xdfff)) {
            return false;
        }
    }
    value.assign(p.text.data(), count - 1);
    return true;
}

bool ReadList(const Property &p, std::vector<std::wstring> &values)
{
    if (p.type != DEVPROP_TYPE_STRING_LIST || p.bytes < 2 * sizeof(wchar_t) ||
        p.bytes > sizeof(p.text) || p.bytes % sizeof(wchar_t) != 0) {
        return false;
    }
    const size_t count = p.bytes / sizeof(wchar_t);
    if (p.text[count - 1] != L'\0' || p.text[count - 2] != L'\0') {
        return false;
    }
    if (count == 2 && p.text[0] == L'\0') {
        return true;
    }
    size_t begin = 0;
    for (size_t i = 0; i + 1 < count; ++i) {
        const wchar_t c = p.text[i];
        if (c == L'\0') {
            if (i == begin) {
                return false;
            }
            values.emplace_back(p.text.data() + begin, i - begin);
            begin = i + 1;
        } else if (c < 0x20 || (c >= 0xd800 && c <= 0xdfff)) {
            return false;
        }
    }
    return begin == count - 1;
}

struct Node {
    DEVINST id = 0;
    std::wstring instance;
    std::wstring service;
    std::vector<std::wstring> hardware;
    std::vector<std::wstring> filters;
    GUID container{};
    bool hasContainer = false;
};

bool ReadNode(MetadataSource &source, DEVINST id, Node &node)
{
    node.id = id;
    Property p;
    if (source.Read(id, DEVPKEY_Device_InstanceId, p) != CR_SUCCESS ||
        !ReadString(p, DEVPROP_TYPE_STRING, MAX_DEVICE_ID_LEN, node.instance) ||
        !ValidInstance(node.instance) || HasMarker(node.instance)) {
        return false;
    }
    p = {};
    CONFIGRET status = source.Read(id, DEVPKEY_Device_HardwareIds, p);
    if (status != CR_NO_SUCH_VALUE && (status != CR_SUCCESS || !ReadList(p, node.hardware))) {
        return false;
    }
    for (const auto &hardware : node.hardware) {
        if (HasMarker(hardware)) {
            return false;
        }
    }
    p = {};
    status = source.Read(id, DEVPKEY_Device_Service, p);
    if (status != CR_NO_SUCH_VALUE && (status != CR_SUCCESS ||
        !ReadString(p, DEVPROP_TYPE_STRING, MAX_DEVICE_ID_LEN, node.service) || node.service.empty())) {
        return false;
    }
    p = {};
    status = source.Read(id, DEVPKEY_Device_UpperFilters, p);
    if (status != CR_NO_SUCH_VALUE && (status != CR_SUCCESS || !ReadList(p, node.filters))) {
        return false;
    }
    p = {};
    status = source.Read(id, DEVPKEY_Device_ContainerId, p);
    if (status == CR_SUCCESS) {
        if (p.type != DEVPROP_TYPE_GUID || p.bytes != sizeof(GUID)) {
            return false;
        }
        std::memcpy(&node.container, p.text.data(), sizeof(GUID));
        node.hasContainer = true;
    } else if (status != CR_NO_SUCH_VALUE) {
        return false;
    }
    return true;
}

bool InterfaceNode(MetadataSource &source, const wchar_t *path, DEVINST &node, std::wstring &instance)
{
    Property p;
    return source.Interface(path, p) == CR_SUCCESS &&
           ReadString(p, DEVPROP_TYPE_STRING, MAX_DEVICE_ID_LEN, instance) &&
           ValidInstance(instance) && !HasMarker(instance) &&
           source.Locate(instance, node) == CR_SUCCESS;
}

int HexDigit(wchar_t c) noexcept
{
    if (c >= L'0' && c <= L'9') {
        return c - L'0';
    }
    if (c >= L'A' && c <= L'F') {
        return c - L'A' + 10;
    }
    if (c >= L'a' && c <= L'f') {
        return c - L'a' + 10;
    }
    return -1;
}

bool UsbIds(std::wstring_view instance, std::uint16_t &vendor, std::uint16_t &product) noexcept
{
    if (!StartsWith(instance, L"USB\\VID_") || instance.size() < 21 ||
        !EqualText(instance.substr(12, 5), L"&PID_")) return false;
    std::uint16_t parsedVendor = 0, parsedProduct = 0;
    for (size_t i = 0; i < 4; ++i) {
        const int v = HexDigit(instance[8 + i]);
        const int p = HexDigit(instance[17 + i]);
        if (v < 0 || p < 0) return false;
        parsedVendor = static_cast<std::uint16_t>((parsedVendor << 4) | v);
        parsedProduct = static_cast<std::uint16_t>((parsedProduct << 4) | p);
    }
    vendor = parsedVendor;
    product = parsedProduct;
    return true;
}

bool SyntheticNode(std::wstring_view instance) noexcept
{
    return StartsWith(instance, SyntheticFamily) && instance.size() >= 25 &&
           EqualText(instance.substr(21, 4), L"&IG_");
}

bool ParseNative(std::wstring_view instance, std::uint64_t &nativeId,
                 std::uint16_t &vendor, std::uint16_t &product) noexcept
{
    nativeId = 0;
    constexpr size_t prefixLength = 28;
    if (instance.size() != prefixLength + 22 || !UsbIds(instance, vendor, product) ||
        vendor != 0x045e || !EqualText(instance.substr(21, 7), L"&IG_00\\")) {
        return false;
    }
    const auto suffix = instance.substr(prefixLength);
    // xboxgip!gipAddDevice assigns hardware contexts 0-7 and software contexts
    // 8-39. gipHidAddDevice formats host index, GIP subclient, and native ID.
    // The primary gamepad is subclient zero. The host index is not an identity.
    if (suffix[0] != L'0' || suffix[1] < L'0' || suffix[1] > L'7' ||
        suffix.substr(2, 4) != L"&00&") {
        return false;
    }
    std::uint64_t value = 0;
    for (size_t i = 6; i < suffix.size(); ++i) {
        const int digit = HexDigit(suffix[i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | static_cast<std::uint64_t>(digit);
    }
    nativeId = value;
    return value != 0;
}

bool PhysicalUsb(std::wstring_view instance, std::uint16_t &vendor, std::uint16_t &product) noexcept
{
    return instance.size() > 22 && instance[21] == L'\\' && UsbIds(instance, vendor, product);
}

bool WirelessReceiver(std::uint16_t vendor, std::uint16_t product) noexcept
{
    // Microsoft receiver IDs from xone's transport/dongle.c device table.
    return vendor == 0x045e && (product == 0x02e6 || product == 0x02fe ||
                              product == 0x02f9 || product == 0x091e);
}

bool HasExact(const std::vector<std::wstring> &values, std::wstring_view text) noexcept
{
    for (const auto &value : values) {
        if (EqualText(value, text)) {
            return true;
        }
    }
    return false;
}

bool BleHidMetadata(std::wstring_view value) noexcept
{
    return StartsWith(value, BleHidId) && (value.size() == BleHidId.size() ||
           value[BleHidId.size()] == L'_' || value[BleHidId.size()] == L'\\');
}

bool BleNode(const Node &node) noexcept
{
    // Current Windows binds HID over GATT by the 1812 service ID in hidbthle.inf.
    // The HidBthLE function service and BTHLE enumerator also qualify ancestry.
    // No address is extracted from any string.
    if (BleHidMetadata(node.instance) || StartsWith(node.instance, L"BTHLE\\") ||
        EqualText(node.service, L"HidBthLE")) {
        return true;
    }
    for (const auto &hardware : node.hardware) {
        if (BleHidMetadata(hardware)) {
            return true;
        }
    }
    return false;
}

bool QualifiedDriver(MetadataSource &source)
{
    Property p;
    std::wstring image;
    std::wstring windows;
    std::wstring system;
    if (!source.DriverImage(p) || (p.type != REG_SZ && p.type != REG_EXPAND_SZ) ||
        !ReadString(p, p.type, MaxPathChars, image) || image.empty() ||
        !source.Directories(windows, system) || windows.empty() || system.empty() ||
        windows.size() >= MaxPathChars || system.size() >= MaxPathChars) {
        return false;
    }
    // Accept explicit OS path spellings only. Do not expand the process
    // environment, search a directory, resolve relative paths, or infer a file.
    const std::wstring target = system + L"\\drivers\\xboxgip.sys";
    if (StartsWith(image, L"\\SystemRoot\\")) {
        image = windows + image.substr(11);
    } else if (p.type == REG_EXPAND_SZ && StartsWith(image, L"%SystemRoot%\\")) {
        image = windows + image.substr(12);
    } else if (StartsWith(image, L"\\??\\")) {
        image.erase(0, 4);
    }
    return EqualText(image, target);
}

bool EqualList(const std::vector<std::wstring> &a, const std::vector<std::wstring> &b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (!EqualText(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

bool SameNode(const Node &a, const Node &b) noexcept
{
    return a.id == b.id && EqualText(a.instance, b.instance) && EqualText(a.service, b.service) &&
           EqualList(a.hardware, b.hardware) && EqualList(a.filters, b.filters) &&
           a.hasContainer == b.hasContainer && IsEqualGUID(a.container, b.container);
}

bool ReverseEdge(MetadataSource &source, DEVINST child, DEVINST parent)
{
    std::array<DEVINST, MaxChildren> seen{};
    DEVINST current = 0;
    if (source.Child(parent, current) != CR_SUCCESS) {
        return false;
    }
    for (size_t i = 0; i < seen.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (seen[j] == current) {
                return false;
            }
        }
        seen[i] = current;
        if (current == child) {
            DEVINST recheck = 0;
            return source.Parent(child, recheck) == CR_SUCCESS && recheck == parent;
        }
        DEVINST next = 0;
        if (source.Sibling(current, next) != CR_SUCCESS) {
            return false;
        }
        current = next;
    }
    return false;
}

bool ResolveCore(MetadataSource &source, const wchar_t *path, std::uint32_t index,
                 std::uint32_t count, PhysicalIdentity &result, std::string &error)
{
    auto fail = [&error](const char *message) { error = message; return false; };
    if (index != 0 || count != 1) {
        return fail("The interface channel is not qualified.");
    }
    if (!path) {
        return fail("The interface path is missing.");
    }
    size_t length = 0;
    while (length < MaxPathChars && path[length] != L'\0') {
        ++length;
    }
    if (length == 0 || length == MaxPathChars || HasMarker({path, length})) {
        return fail("The interface path is invalid or excluded.");
    }
    // Own the path across the walk, including when it aliases the caller's output.
    const std::wstring interfacePath(path, length);
    std::wstring instance;
    DEVINST current = 0;
    if (!InterfaceNode(source, interfacePath.c_str(), current, instance)) {
        return fail("The interface device instance is unavailable.");
    }
    std::vector<Node> nodes;
    nodes.reserve(MaxParents + 1);
    for (size_t depth = 0; depth <= MaxParents; ++depth) {
        Node node;
        if (!ReadNode(source, current, node)) {
            return fail("Device ancestry metadata is invalid or excluded.");
        }
        for (const auto &seen : nodes) {
            if (seen.id == node.id || EqualText(seen.instance, node.instance)) {
                return fail("Device ancestry contains a cycle.");
            }
        }
        nodes.push_back(std::move(node));
        DEVINST parent = 0;
        const CONFIGRET status = source.Parent(current, parent);
        if (status == CR_NO_SUCH_DEVNODE && EqualText(nodes.back().instance, L"HTREE\\ROOT\\0")) {
            break;
        }
        if (status != CR_SUCCESS || depth == MaxParents) {
            return fail("Device ancestry is incomplete or exceeds its limit.");
        }
        current = parent;
    }
    const Node &leaf = nodes.front();
    if (!EqualText(instance, leaf.instance) || !StartsWith(leaf.instance, L"HID\\") ||
        !leaf.hasContainer || !ValidContainer(leaf.container)) {
        return fail("The HID interface has no qualified device container.");
    }
    size_t synthetic = nodes.size();
    size_t ble = nodes.size();
    std::uint64_t nativeId = 0;
    std::uint16_t controllerVendor = 0, controllerProduct = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (SyntheticNode(nodes[i].instance)) {
            if (synthetic != nodes.size() || i == 0 ||
                !ParseNative(nodes[i].instance, nativeId, controllerVendor, controllerProduct)) {
                return fail("The native GIP ancestor is ambiguous or unqualified.");
            }
            synthetic = i;
        }
        if (BleNode(nodes[i])) {
            ble = i;
        }
    }
    size_t lastPhysical = 0;
    size_t lastContainer = 0;
    std::uint16_t hardwareVendor = 0, hardwareProduct = 0;
    PaddleTransport transport = PaddleTransport::None;
    if (synthetic != nodes.size()) {
        if (ble != nodes.size() || synthetic + 1 >= nodes.size()) {
            return fail("The device transport is ambiguous.");
        }
        lastPhysical = synthetic + 1;
        const Node &physical = nodes[lastPhysical];
        if (!PhysicalUsb(physical.instance, hardwareVendor, hardwareProduct) ||
            !(EqualText(physical.service, L"xboxgip") || HasExact(physical.filters, L"xboxgip")) ||
            !QualifiedDriver(source)) {
            return fail("The physical USB parent has no qualified XboxGIP driver binding.");
        }
        if (WirelessReceiver(hardwareVendor, hardwareProduct)) {
            // Wireless HID PDOs use the controller's Hello VID/PID. The 02FF
            // hardware-ID alias is separate. Read the primary instance ID.
            if (controllerProduct == 0x02ff) return fail("The wireless controller identity is not qualified.");
            hardwareVendor = controllerVendor;
            hardwareProduct = controllerProduct;
            lastContainer = synthetic;
            transport = PaddleTransport::WirelessGip;
        } else {
            // Wired PDOs can use either the 02FF alias or their actual VID/PID,
            // depending on their metadata flags. Both must name this parent.
            if (controllerProduct != 0x02ff &&
                (controllerVendor != hardwareVendor || controllerProduct != hardwareProduct)) {
                return fail("The wired controller identity disagrees with its physical parent.");
            }
            lastContainer = lastPhysical;
            transport = PaddleTransport::UsbGip;
        }
    } else if (ble != nodes.size()) {
        lastPhysical = ble;
        lastContainer = lastPhysical;
        transport = PaddleTransport::Bluetooth;
    } else {
        return fail("The device transport is not qualified.");
    }
    for (size_t i = 0; i <= lastContainer; ++i) {
        if (!nodes[i].hasContainer || !IsEqualGUID(nodes[i].container, leaf.container)) {
            return fail("The physical ancestry crosses device containers.");
        }
    }
    // Recheck every inspected node and direct parent. Only the attachment
    // edges need bounded reverse traversal of their parent's children.
    for (size_t i = 0; i < nodes.size(); ++i) {
        Node check;
        if (!ReadNode(source, nodes[i].id, check) || !SameNode(nodes[i], check)) {
            return fail("Device ancestry metadata changed during resolution.");
        }
        DEVINST parent = 0;
        const CONFIGRET status = source.Parent(check.id, parent);
        if (i + 1 < nodes.size()) {
            if (status != CR_SUCCESS || parent != nodes[i + 1].id ||
                (i < lastPhysical && !ReverseEdge(source, check.id, parent))) {
                return fail("A physical parent edge changed or could not be verified.");
            }
        } else if (status != CR_NO_SUCH_DEVNODE) {
            return fail("The ancestry root changed during resolution.");
        }
    }
    Node finalLeaf;
    if (!InterfaceNode(source, interfacePath.c_str(), current, instance) || current != leaf.id ||
        !EqualText(instance, leaf.instance) || !ReadNode(source, current, finalLeaf) ||
        !SameNode(leaf, finalLeaf) || (IsGipTransport(transport) && !QualifiedDriver(source))) {
        return fail("The interface or driver binding changed during resolution.");
    }
    result.transport = transport;
    result.container = leaf.container;
    result.nativeId = nativeId;
    result.instance = leaf.instance;
    result.vendor = hardwareVendor;
    result.product = hardwareProduct;
    return true;
}

void ClearIdentity(PhysicalIdentity &identity) noexcept
{
    identity.transport = PaddleTransport::None;
    identity.container = {};
    identity.nativeId = 0;
    identity.instance.clear();
    identity.vendor = identity.product = 0;
}

bool ResolveWithSource(MetadataSource &source, const wchar_t *path, std::uint32_t index,
                       std::uint32_t count, PhysicalIdentity &identity, std::string &error) noexcept
{
    error.clear();
    try {
        PhysicalIdentity result;
        if (ResolveCore(source, path, index, count, result, error)) {
            identity = std::move(result);
            return true;
        }
    } catch (...) {
        try {
            error = "Device identity metadata could not be read.";
        } catch (...) {
            error.clear();
        }
    }
    ClearIdentity(identity);
    return false;
}

} // namespace

bool ResolvePaddleIdentity(const wchar_t *interfacePath, std::uint32_t interfaceIndex,
                           std::uint32_t interfaceCount, PhysicalIdentity &identity,
                           std::string &error) noexcept
{
    WindowsMetadata source;
    return ResolveWithSource(source, interfacePath, interfaceIndex, interfaceCount, identity, error);
}

bool SamePhysicalIdentity(const PhysicalIdentity &a, const PhysicalIdentity &b) noexcept
{
    if (a.transport != b.transport || !ValidContainer(a.container) || !ValidContainer(b.container) ||
        !IsEqualGUID(a.container, b.container) || !ValidInstance(a.instance) || !ValidInstance(b.instance) ||
        !StartsWith(a.instance, L"HID\\") || !StartsWith(b.instance, L"HID\\") ||
        HasMarker(a.instance) || HasMarker(b.instance) || !EqualText(a.instance, b.instance) ||
        a.vendor != b.vendor || a.product != b.product) {
        return false;
    }
    if (a.transport == PaddleTransport::Bluetooth) {
        return a.nativeId == 0 && b.nativeId == 0;
    }
    return IsGipTransport(a.transport) && a.nativeId != 0 && a.nativeId == b.nativeId;
}

} // namespace sdl_paddles
