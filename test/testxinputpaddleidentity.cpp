/* Offline tests for the metadata-only physical identity resolver. */
// Include the implementation to inject metadata below its actual parsers and
// ancestry logic. Compile this test alone, without a second identity object.
#include "../src/joystick/windows/SDL_xinput_paddle_identity.cpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>

using namespace sdl_paddles;

namespace {

constexpr GUID ContainerA = {0x11223344, 0x5566, 0x7788, {0x91, 2, 3, 4, 5, 6, 7, 8}};
constexpr GUID ContainerB = {0x99887766, 0x5544, 0x3322, {0x81, 8, 7, 6, 5, 4, 3, 2}};
constexpr std::uint64_t NativeA = 0x123456789abcdef0;
constexpr wchar_t Path[] = L"\\\\?\\hid#vid_045e&pid_02ff&ig_00#fixture#{ec87f1e3-c13b-4100-b5f7-8b84d54260cb}";
size_t checks = 0;
size_t cases = 0;

void Check(bool value, const char *description)
{
    ++checks;
    if (!value) {
        throw std::runtime_error(description);
    }
}

enum Key { Instance, Hardware, Service, Filters, Container };

int PropertyKey(const DEVPROPKEY &key)
{
    const DEVPROPKEY *keys[] = {&DEVPKEY_Device_InstanceId, &DEVPKEY_Device_HardwareIds,
                               &DEVPKEY_Device_Service, &DEVPKEY_Device_UpperFilters,
                               &DEVPKEY_Device_ContainerId};
    for (int i = 0; i < 5; ++i) {
        if (key.pid == keys[i]->pid && IsEqualGUID(key.fmtid, keys[i]->fmtid)) {
            return i;
        }
    }
    return -1;
}

struct Saved {
    ULONG type = DEVPROP_TYPE_STRING;
    std::vector<unsigned char> bytes;
    CONFIGRET status = CR_SUCCESS;
    ULONG reportedBytes = 0;
};

Saved Raw(const void *data, size_t size, ULONG type)
{
    const auto *begin = static_cast<const unsigned char *>(data);
    return {type, {begin, begin + size}, CR_SUCCESS, 0};
}

Saved String(std::wstring value, ULONG type = DEVPROP_TYPE_STRING)
{
    value.push_back(L'\0');
    return Raw(value.data(), value.size() * sizeof(wchar_t), type);
}

Saved List(std::initializer_list<std::wstring_view> values)
{
    std::wstring text;
    for (auto value : values) {
        text.append(value);
        text.push_back(L'\0');
    }
    text.push_back(L'\0');
    if (values.size() == 0) {
        text.push_back(L'\0');
    }
    return Raw(text.data(), text.size() * sizeof(wchar_t), DEVPROP_TYPE_STRING_LIST);
}

Saved Guid(const GUID &guid)
{
    return Raw(&guid, sizeof(guid), DEVPROP_TYPE_GUID);
}

CONFIGRET Copy(const Saved &saved, Property &out)
{
    out.type = saved.type;
    out.bytes = saved.reportedBytes ? saved.reportedBytes : static_cast<ULONG>(saved.bytes.size());
    if (saved.status != CR_SUCCESS) {
        return saved.status;
    }
    if (saved.bytes.size() > sizeof(out.text)) {
        return CR_BUFFER_SMALL;
    }
    if (!saved.bytes.empty()) {
        std::memcpy(out.text.data(), saved.bytes.data(), saved.bytes.size());
    }
    return CR_SUCCESS;
}

struct Fake final : MetadataSource {
    std::map<DEVINST, std::map<int, Saved>> nodes;
    std::map<DEVINST, DEVINST> parents;
    std::map<DEVINST, std::vector<DEVINST>> children;
    std::map<std::pair<DEVINST, int>, size_t> reads;
    std::map<DEVINST, size_t> parentReads;
    Saved interface = String(L"HID\\VID_045E&PID_02FF&IG_00\\8&FIXTURE&0&0000");
    Saved image = String(L"\\SystemRoot\\System32\\drivers\\xboxgip.sys", REG_EXPAND_SZ);
    std::wstring windows = L"C:\\Windows";
    std::wstring system = L"C:\\Windows\\System32";
    DEVINST located = 1;
    size_t calls = 0;
    size_t interfaceReads = 0;
    size_t imageReads = 0;
    bool imageAvailable = true;
    bool directoriesAvailable = true;
    const char *harnessFailure = nullptr;
    std::function<void(Fake &, const char *, DEVINST, int)> before;

    void Require(bool condition, const char *description)
    {
        // Retain fixture failures outside the resolver's exception boundary.
        // Deliberate metadata exceptions do not set this separate failure.
        if (!condition) {
            harnessFailure = description;
            throw std::runtime_error(description);
        }
    }
    void Call(const char *operation, DEVINST node = 0, int key = -1)
    {
        ++calls;
        if (before) {
            before(*this, operation, node, key);
        }
    }
    CONFIGRET Interface(const wchar_t *path, Property &p) override
    {
        ++interfaceReads;
        Call("interface");
        Require(_wcsicmp(path, Path) == 0, "The exact supplied interface reaches CM");
        return Copy(interface, p);
    }
    CONFIGRET Locate(std::wstring &, DEVINST &node) override
    {
        Call("locate");
        node = located;
        return nodes.contains(node) ? CR_SUCCESS : CR_NO_SUCH_DEVNODE;
    }
    CONFIGRET Read(DEVINST node, const DEVPROPKEY &key, Property &p) override
    {
        const int k = PropertyKey(key);
        Require(k >= 0, "Unexpected property query");
        ++reads[{node, k}];
        Call("read", node, k);
        const auto entry = nodes.find(node);
        if (entry == nodes.end()) {
            return CR_NO_SUCH_DEVNODE;
        }
        const auto value = entry->second.find(k);
        return value == entry->second.end() ? CR_NO_SUCH_VALUE : Copy(value->second, p);
    }
    CONFIGRET Parent(DEVINST node, DEVINST &parent) override
    {
        ++parentReads[node];
        Call("parent", node);
        const auto entry = parents.find(node);
        if (entry == parents.end()) {
            return CR_NO_SUCH_DEVNODE;
        }
        parent = entry->second;
        return CR_SUCCESS;
    }
    CONFIGRET Child(DEVINST node, DEVINST &child) override
    {
        Call("child", node);
        const auto entry = children.find(node);
        if (entry == children.end() || entry->second.empty()) {
            return CR_NO_SUCH_DEVNODE;
        }
        child = entry->second.front();
        return CR_SUCCESS;
    }
    CONFIGRET Sibling(DEVINST node, DEVINST &sibling) override
    {
        Call("sibling", node);
        for (const auto &[parent, group] : children) {
            (void)parent;
            const auto entry = std::find(group.begin(), group.end(), node);
            if (entry != group.end() && entry + 1 != group.end()) {
                sibling = *(entry + 1);
                return CR_SUCCESS;
            }
        }
        return CR_NO_SUCH_DEVNODE;
    }
    bool DriverImage(Property &p) override
    {
        ++imageReads;
        Call("image");
        return imageAvailable && Copy(image, p) == CR_SUCCESS;
    }
    bool Directories(std::wstring &w, std::wstring &s) override
    {
        Call("directories");
        w = windows;
        s = system;
        return directoriesAvailable;
    }
    void Edges()
    {
        children.clear();
        for (const auto &[child, parent] : parents) {
            children[parent].push_back(child);
        }
    }
};

Fake Usb()
{
    Fake f;
    f.nodes[1] = {{Instance, f.interface}, {Hardware, List({L"HID\\VID_045E&PID_02FF&IG_00"})},
                  {Container, Guid(ContainerA)}};
    f.nodes[2] = {{Instance, String(L"USB\\VID_045E&PID_02FF&IG_00\\00&00&123456789ABCDEF0")},
                  {Hardware, List({L"USB\\VID_045E&PID_02FF&IG_00"})},
                  {Service, String(L"HidUsb")}, {Container, Guid(ContainerA)}};
    f.nodes[3] = {{Instance, String(L"USB\\VID_045E&PID_0B00\\UNIT_A")},
                  {Hardware, List({L"USB\\VID_045E&PID_0B00"})}, {Service, String(L"dc1-controller")},
                  {Filters, List({L"xboxgip"})}, {Container, Guid(ContainerA)}};
    f.nodes[4] = {{Instance, String(L"USB\\ROOT_HUB30\\HUB_A")}, {Service, String(L"USBHUB3")},
                  {Container, Guid(SystemContainer)}};
    f.nodes[5] = {{Instance, String(L"HTREE\\ROOT\\0")}, {Container, Guid(SystemContainer)}};
    f.parents = {{1, 2}, {2, 3}, {3, 4}, {4, 5}};
    f.Edges();
    return f;
}

Fake Ble()
{
    Fake f = Usb();
    f.nodes[2][Instance] = String(L"BTHLEDEVICE\\{00001812-0000-1000-8000-00805F9B34FB}_DEV_VID&02045E_PID&0B22\\A&FIXTURE&0&0016");
    f.nodes[2][Hardware] = List({L"BTHLEDEVICE\\{00001812-0000-1000-8000-00805F9B34FB}"});
    f.nodes[2][Service] = String(L"mshidumdf");
    f.nodes[3][Instance] = String(L"BTHLE\\DEV_FIXTURE\\A&REMOTE&0&1");
    f.nodes[3][Hardware] = List({L"BTHLE\\DEV_FIXTURE"});
    f.nodes[3][Service] = String(L"BthLEEnum");
    f.nodes[3].erase(Filters);
    return f;
}

Fake Radio(unsigned host = 3, unsigned product = 0x0b00, unsigned receiver = 0x02fe,
           std::uint64_t native = NativeA)
{
    auto f = Usb();
    wchar_t text[128]{};
    swprintf_s(text, L"HID\\VID_045E&PID_%04X&IG_00\\RADIO_%016llX", product, static_cast<unsigned long long>(native));
    f.interface = String(text);
    f.nodes[1][Instance] = f.interface;
    swprintf_s(text, L"HID\\VID_045E&PID_%04X&IG_00", product);
    f.nodes[1][Hardware] = List({text});
    swprintf_s(text, L"USB\\VID_045E&PID_%04X&IG_00\\%02X&00&%016llX", product, host, static_cast<unsigned long long>(native));
    f.nodes[2][Instance] = String(text);
    swprintf_s(text, L"USB\\VID_045E&PID_%04X&IG_00", product);
    f.nodes[2][Hardware] = List({text, L"USB\\VID_045E&PID_02FF&IG_00"});
    swprintf_s(text, L"USB\\VID_045E&PID_%04X\\RECEIVER_A", receiver);
    f.nodes[3][Instance] = String(text);
    swprintf_s(text, L"USB\\VID_045E&PID_%04X", receiver);
    f.nodes[3][Hardware] = List({text});
    f.nodes[3][Service] = String(L"mt7612US");
    f.nodes[3][Container] = Guid(ContainerB);
    return f;
}

PhysicalIdentity Run(Fake &f, bool expected, std::uint32_t index = 0, std::uint32_t count = 1,
                     const wchar_t *path = Path, const char *expectedError = nullptr)
{
    ++cases;
    PhysicalIdentity value{PaddleTransport::UsbGip, ContainerB, 0xff, L"HID\\old\\instance", 0xffff, 0xffff};
    std::string error = "Previous error";
    const bool success = ResolveWithSource(f, path, index, count, value, error);
    Check(!f.harnessFailure, f.harnessFailure ? f.harnessFailure : "No fixture failure");
    Check(success == expected, error.empty() ? "Unexpected successful resolution" : error.c_str());
    if (expected) {
        Check(error.empty(), "Success clears an old error");
        Check(SamePhysicalIdentity(value, value), "A resolved identity compares equal to itself");
        Check(f.interfaceReads == 2, "Resolution rechecks the interface");
        Check(f.reads[{1, Instance}] == 3, "Resolution rechecks the HID node at completion");
    } else {
        constexpr GUID zero{};
        Check(value.transport == PaddleTransport::None && value.nativeId == 0 &&
              value.instance.empty() && value.vendor == 0 && value.product == 0 &&
              IsEqualGUID(value.container, zero), "Failure clears every identity field");
        Check(!error.empty(), "Failure has a diagnostic");
        if (expectedError) {
            Check(error.find(expectedError) != std::string::npos, error.c_str());
        }
    }
    return value;
}

void Reject(Fake &f, const char *expectedError)
{
    Run(f, false, 0, 1, Path, expectedError);
}

void UsbIdentity()
{
    Fake f = Usb();
    auto value = Run(f, true);
    Check(value.transport == PaddleTransport::UsbGip && value.nativeId == NativeA &&
          IsEqualGUID(value.container, ContainerA), "USB identity derives from the connected ancestry");
    Check(value.vendor == 0x045e && value.product == 0x0b00, "USB hardware IDs come from the physical parent, not the synthetic 02FF node");
    auto changedProduct = value;
    changedProduct.product = 0x0b13;
    Check(!SamePhysicalIdentity(value, changedProduct), "A changed physical product retires the identity");
    Check(f.imageReads == 2, "USB checks the registered driver image twice");
    f = Usb();
    f.nodes[3][Service] = String(L"XbOxGiP");
    f.nodes[3].erase(Filters);
    Run(f, true);
    f = Usb();
    f.nodes[2][Instance] = String(L"usb\\vid_045e&pid_02ff&ig_00\\00&00&123456789abcdef0");
    f.nodes[3][Filters] = List({L"another-filter", L"XBOXGIP"});
    f.nodes[1][Instance] = String(L"hid\\vid_045e&pid_02ff&ig_00\\8&fixture&0&0000");
    f.nodes[3][Instance] = String(L"usb\\vid_045e&pid_0b00\\unit_a");
    value = Run(f, true);
    Check(value.vendor == 0x045e && value.product == 0x0b00, "Lowercase physical USB identifiers preserve their values");
    f = Usb();
    f.nodes[3][Instance] = String(L"USB\\VID_1234&PID_ABCD\\DIFFERENT_DEVICE");
    value = Run(f, true);
    Check(value.vendor == 0x1234 && value.product == 0xabcd, "The resolver reports hardware identity without inventing Elite eligibility");
    f = Usb();
    f.nodes[3][Filters] = List({});
    f.nodes[3][Service] = String(L"xboxgip");
    Run(f, true);
    f = Usb();
    f.nodes[6] = {{Instance, String(L"HID\\COLLECTION\\INTERPOSED")}, {Container, Guid(ContainerA)}};
    f.parents[1] = 6;
    f.parents[6] = 2;
    f.Edges();
    Check(Run(f, true).nativeId == NativeA, "An interposed same-container node preserves the exact ancestry");
}

void NativeGrammar()
{
    const std::wstring prefix = L"USB\\VID_045E&PID_02FF&IG_00\\";
    const wchar_t *bad[] = {L"00&00&0000000000000000", L"08&00&123456789ABCDEF0", L"00&01&123456789ABCDEF0",
                           L"AA&BB&123456789ABCDEF0", L"0&00&123456789ABCDEF0", L"000&00&123456789ABCDEF0",
                           L"00&0&123456789ABCDEF0", L"00&00&123456789ABCDEF", L"00&00&123456789ABCDEF01",
                           L"00&00&+123456789ABCDEF", L"00&00&-123456789ABCDEF", L"00&00&0x3456789ABCDEF0",
                           L"00&00&123456789ABCDEFO", L"00&00&123456789ABCDEF0&0"};
    for (const auto *suffix : bad) {
        auto f = Usb();
        f.nodes[2][Instance] = String(prefix + suffix);
        Reject(f, "native GIP ancestor");
    }
    for (size_t i = 0; i < 16; ++i) {
        auto f = Usb();
        std::wstring suffix = L"00&00&123456789ABCDEF0";
        suffix[i + 6] = L'G';
        f.nodes[2][Instance] = String(prefix + suffix);
        Reject(f, "native GIP ancestor");
    }
    for (unsigned bit = 0; bit < 64; ++bit) {
        auto f = Usb();
        wchar_t suffix[24]{};
        const std::uint64_t native = std::uint64_t{1} << bit;
        swprintf_s(suffix, L"00&00&%016llX", static_cast<unsigned long long>(native));
        f.nodes[2][Instance] = String(prefix + suffix);
        Check(Run(f, true).nativeId == native, "Every native ID bit survives parsing");
    }
    auto f = Usb();
    f.nodes[2][Instance] = String(prefix + L"00&00&FFFFFFFFFFFFFFFF");
    Check(Run(f, true).nativeId == UINT64_MAX, "The full unsigned native ID is preserved");
    const std::pair<const wchar_t *, const char *> unqualified[] = {
        {L"USB\\VID_045E&PID_02FF&IG_01\\00&00&123456789ABCDEF0", "native GIP ancestor"},
        {L"USB\\VID_045E&PID_02FF&IG_000\\00&00&123456789ABCDEF0", "native GIP ancestor"},
        {L"USB\\VID_045F&PID_02FF&IG_00\\00&00&123456789ABCDEF0", "transport is not qualified"},
        {L"HID\\VID_045E&PID_02FF&IG_00\\00&00&123456789ABCDEF0", "transport is not qualified"}};
    for (const auto &[instance, reason] : unqualified) {
        f = Usb();
        f.nodes[2][Instance] = String(instance);
        Reject(f, reason);
    }
    f = Usb();
    f.nodes[4][Instance] = String(prefix + L"00&00&FEDCBA9876543210");
    Reject(f, "native GIP ancestor");
    f = Usb();
    f.nodes[3][Instance] = String(L"ROOT\\VID_045E&PID_0B00\\UNIT_A");
    Reject(f, "USB parent has no qualified XboxGIP");
    f = Usb();
    f.nodes[3][Instance] = String(L"USB\\VID_045E&PID_0B00&MI_00\\UNIT_A");
    Reject(f, "USB parent has no qualified XboxGIP");
    f = Usb();
    f.nodes[2][Instance] = String(prefix + L"00&00&123456789ABCDEF0 ");
    Reject(f, "ancestry metadata is invalid");
}

void Containers()
{
    constexpr GUID zero{};
    for (DEVINST n : {1ul, 2ul, 3ul}) {
        for (const auto &guid : {zero, SystemContainer, ContainerB}) {
            auto f = Usb();
            f.nodes[n][Container] = Guid(guid);
            Reject(f, n == 1 && !IsEqualGUID(guid, ContainerB) ?
                      "HID interface has no qualified device container" : "crosses device containers");
        }
        auto f = Usb();
        f.nodes[n].erase(Container);
        Reject(f, n == 1 ? "HID interface has no qualified device container" : "crosses device containers");
        f = Usb();
        f.nodes[n][Container].type = DEVPROP_TYPE_BINARY;
        Reject(f, "ancestry metadata is invalid");
        f = Usb();
        f.nodes[n][Container].bytes.pop_back();
        Reject(f, "ancestry metadata is invalid");
        f = Usb();
        f.nodes[n][Container].bytes.push_back(0);
        Reject(f, "ancestry metadata is invalid");
    }
    auto f = Usb();
    f.nodes[4].erase(Container);
    Run(f, true);
}

void DriverBinding()
{
    for (const auto *filter : {L"", L"xboxgip2", L"prefix-xboxgip", L"xboxgip.sys", L"dc1-controller"}) {
        auto f = Usb();
        f.nodes[3][Filters] = List({filter});
        Reject(f, "USB parent has no qualified XboxGIP");
    }
    auto f = Usb();
    f.nodes[3].erase(Filters);
    f.nodes[2][Service] = String(L"xboxgip");
    Reject(f, "USB parent has no qualified XboxGIP");
    for (const auto *image : {L"C:\\WINDOWS\\SYSTEM32\\DRIVERS\\XBOXGIP.SYS",
                              L"\\??\\C:\\Windows\\System32\\drivers\\xboxgip.sys",
                              L"%SystemRoot%\\System32\\drivers\\xboxgip.sys",
                              L"\\systemroot\\SYSTEM32\\drivers\\xboxgip.sys"}) {
        f = Usb();
        f.image = String(image, REG_EXPAND_SZ);
        Run(f, true);
    }
    for (const auto *image : {L"", L"xboxgip.sys", L"System32\\drivers\\xboxgip.sys",
                              L"%windir%\\System32\\drivers\\xboxgip.sys", L"%OTHER%\\xboxgip.sys",
                              L"C:\\Other\\System32\\drivers\\xboxgip.sys", L"\\SystemRoot\\System32\\drivers\\xboxgip.sys.exe",
                              L"\\SystemRoot\\System32\\drivers\\dc1-controller.sys",
                              L"\\SystemRoot\\System32\\drivers\\xboxgip.sys /arg",
                              L"\"C:\\Windows\\System32\\drivers\\xboxgip.sys\"",
                              L"C:/Windows/System32/drivers/xboxgip.sys",
                              L"C:\\Windows\\System32\\drivers\\..\\drivers\\xboxgip.sys",
                              L"\\\\server\\System32\\drivers\\xboxgip.sys"}) {
        f = Usb();
        f.image = String(image, REG_EXPAND_SZ);
        Reject(f, "USB parent has no qualified XboxGIP");
    }
    f = Usb();
    f.image = String(L"%SystemRoot%\\System32\\drivers\\xboxgip.sys", REG_SZ);
    Reject(f, "USB parent has no qualified XboxGIP");
    f = Usb();
    f.image = String(L"\\SystemRoot\\System32\\drivers\\xboxgip.sys", REG_SZ);
    Run(f, true);
    f = Usb();
    f.image.type = REG_MULTI_SZ;
    Reject(f, "USB parent has no qualified XboxGIP");
    f = Usb();
    f.imageAvailable = false;
    Reject(f, "USB parent has no qualified XboxGIP");
    f = Usb();
    f.directoriesAvailable = false;
    Reject(f, "USB parent has no qualified XboxGIP");
}

void PropertyBounds()
{
    for (int key : {Instance, Hardware, Service, Filters, Container}) {
        for (int defect = 0; defect < 5; ++defect) {
            auto f = Usb();
            auto &p = f.nodes[3][key];
            if (defect == 0) p.type = DEVPROP_TYPE_UINT64;
            if (defect == 1) p.bytes.clear();
            if (defect == 2) p.bytes.pop_back();
            if (defect == 3) p.reportedBytes = static_cast<ULONG>(MaxPropertyChars * 2 + 2);
            if (defect == 4) p.status = CR_ACCESS_DENIED;
            Run(f, false);
        }
    }
    auto f = Usb();
    f.nodes[2][Instance] = String(std::wstring(L"USB\\VID_045E&PID_02FF&IG_00\\00&00&123456789ABCDEF0\0TAIL", 52));
    Run(f, false);
    f = Usb();
    f.interface = String(std::wstring(MAX_DEVICE_ID_LEN, L'x'));
    Run(f, false);
    f = Usb();
    f.nodes[4][Instance] = String(std::wstring(MAX_DEVICE_ID_LEN, L'x'));
    Run(f, false);
    for (const auto *bad : {L"USB\\\\UNIT", L"USB\\ONE\\TWO\\THREE", L"\\USB\\UNIT", L"USB\\ONE\\", L"USB/ONE/UNIT"}) {
        f = Usb();
        f.nodes[3][Instance] = String(bad);
        Run(f, false);
    }
    for (int target = 0; target < 3; ++target) {
        f = Usb();
        Saved *p = target == 0 ? &f.interface : target == 1 ? &f.nodes[3][Service] : &f.image;
        p->bytes.resize(p->bytes.size() - sizeof(wchar_t));
        Run(f, false);
        f = Usb();
        p = target == 0 ? &f.interface : target == 1 ? &f.nodes[3][Service] : &f.image;
        p->bytes[2] = 0;
        p->bytes[3] = 0;
        Run(f, false);
    }
    for (int key : {Hardware, Filters}) {
        for (int defect = 0; defect < 4; ++defect) {
            f = Usb();
            auto &p = f.nodes[3][key];
            p = List({L"xboxgip", L"other"});
            if (defect == 0) p.bytes.resize(p.bytes.size() - sizeof(wchar_t));
            if (defect == 1) p = List({L"xboxgip", L"", L"tail"});
            if (defect == 2) p.bytes.insert(p.bytes.end(), {L'x', 0});
            if (defect == 3) p = List({L"xboxgip", std::wstring(1, wchar_t{0xd800})});
            Run(f, false);
        }
    }
    f = Usb();
    f.nodes[4][Hardware] = List({std::wstring(MaxPropertyChars, L'X')});
    Run(f, false);
    f = Usb();
    f.nodes[4][Hardware] = List({std::wstring(MaxPropertyChars - 2, L'X')});
    Run(f, true);
}

void ExclusionAndChannels()
{
    for (DEVINST n = 1; n <= 5; ++n) {
        auto f = Usb();
        f.nodes[n][Hardware] = List({L"unchanged-first-id", L"ROOT\\hIdMaEsTrO_UDE"});
        Run(f, false);
        f = Usb();
        f.nodes[n][Instance] = String(L"ROOT\\HidMaestro_UDE\\ONE");
        Run(f, false);
    }
    for (auto tuple : {std::pair{0u, 0u}, {1u, 1u}, {0u, 2u}, {1u, 2u}, {0u, UINT32_MAX}, {UINT32_MAX, 1u}}) {
        auto f = Usb();
        Run(f, false, tuple.first, tuple.second);
        Check(f.calls == 0, "Unsupported channels perform no metadata reads");
    }
    for (const auto *path : {static_cast<const wchar_t *>(nullptr), L"", L"\\\\?\\hIdMaEsTrO#virtual"}) {
        auto f = Usb();
        Run(f, false, 0, 1, path);
        Check(f.calls == 0, "Invalid paths perform no metadata reads");
    }
    auto f = Usb();
    const std::wstring longPath(MaxPathChars, L'x');
    Run(f, false, 0, 1, longPath.c_str());
    const std::array<wchar_t, MaxPathChars> unterminated = [] {
        std::array<wchar_t, MaxPathChars> text;
        text.fill(L'x');
        return text;
    }();
    Run(f, false, 0, 1, unterminated.data());
    PhysicalIdentity value;
    value.instance = Path;
    std::string error;
    f = Usb();
    Check(ResolveWithSource(f, value.instance.c_str(), 0, 1, value, error), "Input can alias the output's string");
    Check(value.nativeId == NativeA, "Aliased input keeps the resolved native ID");
}

void AncestryBounds()
{
    auto f = Usb();
    f.parents[4] = 1;
    Run(f, false);
    f = Usb();
    f.parents[2] = 2;
    Run(f, false);
    f = Usb();
    f.nodes[4][Instance] = f.nodes[3][Instance];
    Run(f, false);
    f = Usb();
    f.parents.erase(3);
    Run(f, false);
    f = Usb();
    f.parents[3] = 99;
    Run(f, false);
    for (DEVINST depth : {16ul, 17ul}) {
        f = Usb();
        f.nodes.erase(5);
        f.parents.erase(4);
        for (DEVINST n = 5; n <= depth + 1; ++n) {
            f.nodes[n][Instance] = String(n == depth + 1 ? L"HTREE\\ROOT\\0" : L"ROOT\\BRIDGE\\" + std::to_wstring(n));
            f.parents[n - 1] = n;
        }
        f.Edges();
        Run(f, depth == 16);
        Check(f.reads[{18, Instance}] == 0, "The seventeenth parent is never read");
    }
    f = Usb();
    f.children[2] = {77};
    Run(f, false);
    f = Usb();
    f.children[3] = {77};
    Run(f, false);
    f = Usb();
    f.children[2] = {77, 88, 77, 1};
    Run(f, false);
    for (size_t length : {MaxChildren, MaxChildren + 1}) {
        f = Usb();
        f.children[2].clear();
        for (size_t i = 1; i < length; ++i) {
            f.children[2].push_back(static_cast<DEVINST>(1000 + i));
        }
        f.children[2].push_back(1);
        Run(f, length == MaxChildren);
    }
}

void ChurnAndFailures()
{
    for (int key : {Instance, Hardware, Service, Filters, Container}) {
        auto f = Usb();
        f.before = [key](Fake &s, const char *op, DEVINST n, int k) {
            if (std::strcmp(op, "read") == 0 && n == 3 && k == key && s.reads[{n, k}] == 2) {
                if (k == Instance) s.nodes[n][k] = String(L"USB\\VID_045E&PID_0B00\\REPLACED");
                if (k == Hardware) s.nodes[n][k] = List({L"USB\\OTHER"});
                if (k == Service) s.nodes[n][k] = String(L"replacement-driver");
                if (k == Filters) s.nodes[n][k] = List({L"other", L"xboxgip"});
                if (k == Container) s.nodes[n][k] = Guid(ContainerB);
            }
        };
        Reject(f, "ancestry metadata changed");
    }
    for (DEVINST n : {1ul, 2ul, 3ul, 4ul, 5ul}) {
        auto f = Usb();
        f.before = [n](Fake &s, const char *op, DEVINST id, int) {
            if (std::strcmp(op, "parent") == 0 && n == id && s.parentReads[n] == 2) {
                s.parents[n] = 99;
            }
        };
        Reject(f, n == 5 ? "ancestry root changed" : "physical parent edge changed");
    }
    auto f = Usb();
    f.before = [](Fake &s, const char *op, DEVINST, int) {
        if (std::strcmp(op, "interface") == 0 && s.interfaceReads == 2) {
            s.interface = String(L"HID\\REPLACED\\INSTANCE");
        }
    };
    Reject(f, "interface or driver binding changed");
    f = Usb();
    f.before = [](Fake &s, const char *op, DEVINST, int) {
        if (std::strcmp(op, "interface") == 0 && s.interfaceReads == 2) s.located = 2;
    };
    Reject(f, "interface or driver binding changed");
    f = Usb();
    f.before = [](Fake &s, const char *op, DEVINST n, int k) {
        if (std::strcmp(op, "read") == 0 && n == 1 && k == Container && s.reads[{n, k}] == 3) {
            s.nodes[n][k] = Guid(ContainerB);
        }
    };
    Reject(f, "interface or driver binding changed");
    f = Usb();
    f.before = [](Fake &s, const char *op, DEVINST, int) {
        if (std::strcmp(op, "image") == 0 && s.imageReads == 2) {
            s.image = String(L"C:\\Other\\xboxgip.sys", REG_SZ);
        }
    };
    Reject(f, "interface or driver binding changed");
    f = Usb();
    f.before = [](Fake &s, const char *op, DEVINST n, int k) {
        if (std::strcmp(op, "read") == 0 && n == 4 && k == Hardware && s.reads[{n, k}] == 2) {
            s.nodes[n][k] = List({L"ROOT\\HIDMAESTRO_UDE"});
        }
    };
    Reject(f, "ancestry metadata changed");
    for (const auto *operation : {"interface", "locate", "read", "parent", "child", "image", "directories"}) {
        f = Usb();
        f.before = [operation](Fake &, const char *op, DEVINST, int) {
            if (std::strcmp(op, operation) == 0) throw std::bad_alloc{};
        };
        Reject(f, "identity metadata could not be read");
    }
}

void BluetoothIdentity()
{
    auto f = Ble();
    const auto a = Run(f, true);
    Check(a.transport == PaddleTransport::Bluetooth && a.nativeId == 0 &&
          IsEqualGUID(a.container, ContainerA), "Bluetooth returns a container without a fabricated native ID");
    Check(a.vendor == 0 && a.product == 0, "Bluetooth does not invent physical USB identifiers");
    Check(f.imageReads == 0, "Bluetooth has no XboxGIP registry gate");
    f = Ble();
    f.nodes[1][Service] = String(L"HidBthLE");
    f.nodes[2][Instance] = String(L"ROOT\\LEGACY\\HID");
    f.nodes[2].erase(Hardware);
    f.nodes[3][Instance] = String(L"ROOT\\LEGACY\\REMOTE");
    Check(Run(f, true).transport == PaddleTransport::Bluetooth,
          "The explicitly permitted HidBthLE service can qualify the HID leaf");
    auto other = Ble();
    for (DEVINST n : {1ul, 2ul, 3ul}) other.nodes[n][Container] = Guid(ContainerB);
    const auto b = Run(other, true);
    Check(!SamePhysicalIdentity(a, b), "Two Bluetooth controllers remain distinct");
    for (DEVINST n : {2ul, 3ul}) {
        f = Ble();
        f.nodes[n][Container] = Guid(ContainerB);
        Run(f, false);
    }
    f = Ble();
    f.nodes[2][Instance] = String(L"ROOT\\LEGACY_BLE\\HID");
    f.nodes[2].erase(Hardware);
    f.nodes[2][Service] = String(L"HidBthLE");
    f.nodes[3][Instance] = String(L"ROOT\\LEGACY_BLE\\REMOTE");
    Run(f, true);
    f = Ble();
    f.nodes[2][Instance] = String(L"ROOT\\BLE_HID\\HID");
    f.nodes[3][Instance] = String(L"ROOT\\BLE_HID\\REMOTE");
    Run(f, true);
    f = Ble();
    f.nodes[2][Instance] = String(L"ROOT\\UNKNOWN\\HID");
    f.nodes[2].erase(Hardware);
    Run(f, true);
    f = Usb();
    f.nodes[2][Instance] = String(L"USB\\UNKNOWN\\00&00&123456789ABCDEF0");
    f.nodes[3][Instance] = String(L"ROOT\\BTHLE_DEVICE_FAKE\\MAC_123456789ABC");
    Run(f, false);
    f = Ble();
    f.nodes[2][Instance] = String(L"BTHLEDEVICE\\{00001812-0000-1000-8000-00805F9B34FB}BAD\\ONE");
    f.nodes[2].erase(Hardware);
    f.nodes[3][Instance] = String(L"ROOT\\BTHLE\\ONE");
    Run(f, false);
    f = Ble();
    f.nodes[2][Instance] = String(L"USB\\VID_045E&PID_02FF&IG_00\\00&00&123456789ABCDEF0");
    Run(f, false);
}

void Equality()
{
    auto f = Usb();
    const auto a = Run(f, true);
    auto b = a;
    std::transform(b.instance.begin(), b.instance.end(), b.instance.begin(), [](wchar_t c) {
        return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    });
    Check(SamePhysicalIdentity(a, b), "Instance comparison follows Windows case rules");
    b = a; b.nativeId ^= 1;
    Check(!SamePhysicalIdentity(a, b), "Distinct native IDs stay distinct");
    b = a; b.container = ContainerB;
    Check(!SamePhysicalIdentity(a, b), "Distinct containers stay distinct");
    b = a; b.instance = L"HID\\SAME_CONTAINER\\NEW_INSTANCE";
    Check(!SamePhysicalIdentity(a, b), "A replaced HID instance retires the attachment");
    b = a; b.nativeId = 0;
    Check(!SamePhysicalIdentity(b, b), "A USB identity with zero native ID is invalid");
    b = a; b.container = SystemContainer;
    Check(!SamePhysicalIdentity(b, b), "The system container is invalid");
    b = a; b.container = {};
    Check(!SamePhysicalIdentity(b, b), "The null container is invalid");
    b = a; b.instance = L"HID\\HIDMAESTRO\\VIRTUAL";
    Check(!SamePhysicalIdentity(b, b), "Excluded identities never compare equal");
    b = a; b.transport = static_cast<PaddleTransport>(99);
    Check(!SamePhysicalIdentity(b, b), "Unknown transports never compare equal");
    b = {};
    Check(!SamePhysicalIdentity(b, b), "An empty identity never compares equal");
    f = Ble();
    b = Run(f, true);
    b.nativeId = 1;
    Check(!SamePhysicalIdentity(b, b), "Bluetooth never accepts a native GIP ID");
    PhysicalIdentity out;
    std::string error;
    Check(!ResolvePaddleIdentity(nullptr, 0, 1, out, error), "The public wrapper rejects a null path without PnP reads");
}

void WirelessHardware()
{
    for (unsigned receiver : {0x02e6u, 0x02feu, 0x02f9u, 0x091eu}) {
        for (unsigned host = 0; host < 8; ++host) {
            auto f = Radio(host, 0x0b00, receiver);
            const auto value = Run(f, true);
            Check(value.transport == PaddleTransport::WirelessGip && value.nativeId == NativeA &&
                  value.vendor == 0x045e && value.product == 0x0b00,
                  "Wireless identity comes from the controller, not its receiver");
            Check(IsEqualGUID(value.container, ContainerA), "Wireless identity keeps the controller container");
        }
    }
    for (unsigned host = 0; host < 8; ++host) {
        auto f = Usb();
        wchar_t text[80]{};
        swprintf_s(text, L"USB\\VID_045E&PID_02FF&IG_00\\%02X&00&%016llX", host, static_cast<unsigned long long>(NativeA));
        f.nodes[2][Instance] = String(text);
        Check(Run(f, true).transport == PaddleTransport::UsbGip, "A nonzero host index can be wired");
    }
    auto f = Usb();
    f.nodes[2][Instance] = String(L"USB\\VID_045E&PID_0B00&IG_00\\07&00&123456789ABCDEF0");
    Check(Run(f, true).transport == PaddleTransport::UsbGip, "Wired metadata can expose the actual controller ID");
    f.nodes[2][Instance] = String(L"USB\\VID_045E&PID_02E3&IG_00\\07&00&123456789ABCDEF0");
    Reject(f, "disagrees");
    for (unsigned host : {8u, 15u, 39u, 255u}) { f = Radio(host); Reject(f, "native GIP ancestor"); }
    f = Radio(0, 0x02ff); Reject(f, "wireless controller identity");
    f = Radio(0, 0x0b00, 0xffff); Reject(f, "disagrees");
    f = Radio(); f.nodes[2][Container] = Guid(ContainerB); Reject(f, "crosses device containers");
    f = Radio(); f.nodes[3].erase(Filters); Reject(f, "XboxGIP driver binding");
    f = Radio(); f.nodes[3][Hardware] = List({L"ROOT\\HIDMAESTRO_UDE"}); Reject(f, "excluded");
    f = Radio(); f.nodes[2][Instance] = String(L"USB\\VID_045E&PID_0B00&IG_00\\03&01&123456789ABCDEF0");
    Reject(f, "native GIP ancestor");

    auto first = Radio(0);
    auto second = Radio(1, 0x0b00, 0x02fe, NativeA + 1);
    const auto a = Run(first, true), b = Run(second, true);
    Check(!SamePhysicalIdentity(a, b), "Two same-product controllers behind one receiver stay distinct");
    auto standard = Radio(2, 0x0b13, 0x02fe, NativeA + 2);
    Check(Run(standard, true).product == 0x0b13, "A standard controller keeps its own product ID");
    auto replacement = Radio(0, 0x0b00, 0x02fe, NativeA + 3);
    Check(!SamePhysicalIdentity(a, Run(replacement, true)), "Reusing a host slot does not reuse controller identity");
    auto moved = Radio(7, 0x0b00, 0x02e6, NativeA);
    moved.interface = String(L"HID\\VID_045E&PID_0B00&IG_00\\NEW_INSTANCE");
    moved.nodes[1][Instance] = moved.interface;
    Check(!SamePhysicalIdentity(a, Run(moved, true)), "A new HID attachment retires the old source");
    auto wired = Usb();
    Check(!SamePhysicalIdentity(a, Run(wired, true)), "Wired and wireless attachments have separate lifetimes");
}

} // namespace

int main()
{
    const std::pair<const char *, void (*)()> groups[] = {
        {"wireless hardware", WirelessHardware},
        {"USB identity", UsbIdentity}, {"native grammar", NativeGrammar}, {"containers", Containers},
        {"driver binding", DriverBinding}, {"property bounds", PropertyBounds},
        {"exclusion and channels", ExclusionAndChannels}, {"ancestry bounds", AncestryBounds},
        {"churn and failures", ChurnAndFailures}, {"Bluetooth identity", BluetoothIdentity}, {"equality", Equality}};
    for (const auto &[name, run] : groups) {
        try {
            run();
            std::printf("PASS %s\n", name);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "FAIL %s: %s (cases=%zu checks=%zu)\n", name, error.what(), cases, checks);
            return 1;
        }
    }
    std::printf("PASS %zu groups, %zu resolver cases, %zu checks\n", std::size(groups), cases, checks);
    return 0;
}
