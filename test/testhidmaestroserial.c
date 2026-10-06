/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_HidmaestroOwnsUsbSerial, the HIDMaestro filter's serial number
   lookup in the Windows HID backend, against a scripted device tree, for
   hifihedgehog/SDL#37. The Switch 2 driver asks it whether a serial number
   that libusb read belongs to a HIDMaestro virtual controller.

   This file includes src/hidapi/windows/hid.c as hidapi builds it alone,
   with no SDL. The backend reaches hid.dll and cfgmgr32.dll through function
   pointers it resolves in hid_init(). The test fills those pointers with the
   fake below and marks the backend initialized, so no library is loaded, and
   it defines CreateFileW and CloseHandle for the backend, so no device path
   is opened. test/switch2-driver/CheckSystem.cmake fails the build when this
   object still imports either one. hidapi's second source file, which
   rebuilds a report descriptor, is left out: SDL's copy needs SDL, and the
   lookup reads no report descriptor.

   The tree follows the ancestry hid.c records for a live composite persona:
   the HID interface, the USB interface, the USB device, the root hub, then
   the emulated host controller ROOT\USB\0000, whose hardware IDs carry
   ROOT\HIDMAESTRO_UDE. A real controller hangs off an ordinary host
   controller. A persona's serial is HM and twelve hex digits (HIDMaestro
   DeviceIdentity.cs:106), a real Switch 2 controller's is "00"
   (switch2_controller_research descriptors.md), and a HIDMaestro device
   with no USB side reports HM-CTL- and its index (HIDMaestro
   driver.c:313-336) under a hardware ID HID\HIDMaestro.

   1. A serial is owned when a HID interface the filter classifies has the
      asked vendor and product ID and reports that serial. A real
      controller's serial is not, and its interface is never opened.
   2. The serial is the one hidapi reports for the interface: when the string
      read gives nothing, the USB device's instance ID supplies it.
   3. The classification decides, not the serial's look: without the host
      controller's token the persona's serial is not owned and its interface
      is not opened. The serial of a device marked in its own hardware IDs
      is still owned.
   4. The serial compares in the form libusb gives a string descriptor, with
      '?' for a code unit outside ASCII.
   5. Every interface the lookup opens is closed. */

/* hid.c sets this for its own wcsncpy calls, after the headers below */
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The two calls the backend makes on a device path, defined below */
static HANDLE WINAPI Fake_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE template_file);
static BOOL WINAPI Fake_CloseHandle(HANDLE handle);

#define CreateFileW Fake_CreateFileW
#define CloseHandle Fake_CloseHandle

/* An executable has nothing to export */
#define HID_API_NO_EXPORT_DEFINE
/* hidapi's switch for building the report descriptor source apart */
#define hidapi_winapi_EXPORTS

/* hidapi is built at warning level 3, as SDL builds it */
#ifdef _MSC_VER
#pragma warning(push, 3)
#endif
#include "../src/hidapi/windows/hid.c"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#undef CreateFileW
#undef CloseHandle

static void Fake_Unexpected(const char *what);

/* The entry point of the source left out. Only hid_get_report_descriptor
   calls it, on an opened device. */
int hid_winapi_descriptor_reconstruct_pp_data(void *hidp_preparsed_data, unsigned char *buf, size_t buf_size)
{
    (void)hidp_preparsed_data;
    (void)buf;
    (void)buf_size;
    Fake_Unexpected("report descriptor read");
    return -1;
}

/* cfgmgr32.h */
#define TEST_CR_NO_SUCH_DEVNODE 0x0000000D
#define TEST_CR_NO_SUCH_VALUE   0x00000025

static int checks;
static int failures;
static const char *scenario = "";

#define CHECK(condition, ...)                                 \
    do {                                                      \
        ++checks;                                             \
        if (!(condition)) {                                   \
            ++failures;                                       \
            printf("FAILED line %d (%s): ", __LINE__, scenario); \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

/* The scripted device tree. A string list is strings one after another,
   closed by an empty one. */
typedef struct Node
{
    const wchar_t *instance_id;
    int parent; /* Its parent's index, -1 at the root */
    const wchar_t *hardware_ids;
    const wchar_t *compatible_ids;
} Node;

enum
{
    NODE_ROOT,
    NODE_EMULATED_CONTROLLER,
    NODE_EMULATED_HUB,
    NODE_PERSONA_USB,
    NODE_PERSONA_USB_INTERFACE,
    NODE_PERSONA_HID,
    NODE_HOST_CONTROLLER,
    NODE_HUB,
    NODE_REAL_USB,
    NODE_REAL_USB_INTERFACE,
    NODE_REAL_HID,
    NODE_DUALSENSE_USB,
    NODE_DUALSENSE_USB_INTERFACE,
    NODE_DUALSENSE_HID,
    NODE_VIRTUAL_HID,
    NODE_COUNT
};

#define TOKEN_IDS    L"ROOT\\USBIP_WIN2\\UDE\0ROOT\\HIDMAESTRO_UDE\0"
#define NO_TOKEN_IDS L"ROOT\\USBIP_WIN2\\UDE\0"

static Node nodes[NODE_COUNT] = {
    { L"HTREE\\ROOT\\0", -1, NULL, NULL },
    { L"ROOT\\USB\\0000", NODE_ROOT, TOKEN_IDS, NULL },
    { L"USB\\ROOT_HUB30\\5&2C6F5E1&0&0", NODE_EMULATED_CONTROLLER, L"USB\\ROOT_HUB30&VID1D6B&PID0003&REV0100\0USB\\ROOT_HUB30\0", NULL },
    { L"USB\\VID_057E&PID_2069\\HM0123456789AB", NODE_EMULATED_HUB,
      L"USB\\VID_057E&PID_2069&REV_0101\0USB\\VID_057E&PID_2069\0", L"USB\\COMPOSITE\0" },
    { L"USB\\VID_057E&PID_2069&MI_00\\6&3A1B2C4D&0&0000", NODE_PERSONA_USB,
      L"USB\\VID_057E&PID_2069&REV_0101&MI_00\0USB\\VID_057E&PID_2069&MI_00\0", L"USB\\Class_03&SubClass_00&Prot_00\0USB\\Class_03\0" },
    { L"HID\\VID_057E&PID_2069&MI_00\\7&1F2E3D4C&0&0000", NODE_PERSONA_USB_INTERFACE,
      L"HID\\VID_057E&PID_2069&REV_0101&MI_00\0HID\\VID_057E&PID_2069&MI_00\0HID_DEVICE\0", NULL },
    { L"PCI\\VEN_8086&DEV_A0ED&SUBSYS_72708086&REV_20\\3&11583659&0&A0", NODE_ROOT, L"PCI\\VEN_8086&DEV_A0ED&SUBSYS_72708086&REV_20\0", NULL },
    { L"USB\\ROOT_HUB30\\4&1A2B3C4D&0&0", NODE_HOST_CONTROLLER, L"USB\\ROOT_HUB30&VID8086&PIDA0ED&REV0020\0USB\\ROOT_HUB30\0", NULL },
    { L"USB\\VID_057E&PID_2069\\00", NODE_HUB,
      L"USB\\VID_057E&PID_2069&REV_0101\0USB\\VID_057E&PID_2069\0", L"USB\\COMPOSITE\0" },
    { L"USB\\VID_057E&PID_2069&MI_00\\7&2B3C4D5E&0&0000", NODE_REAL_USB,
      L"USB\\VID_057E&PID_2069&REV_0101&MI_00\0USB\\VID_057E&PID_2069&MI_00\0", L"USB\\Class_03&SubClass_00&Prot_00\0USB\\Class_03\0" },
    { L"HID\\VID_057E&PID_2069&MI_00\\8&3C4D5E6F&0&0000", NODE_REAL_USB_INTERFACE,
      L"HID\\VID_057E&PID_2069&REV_0101&MI_00\0HID\\VID_057E&PID_2069&MI_00\0HID_DEVICE\0", NULL },
    { L"USB\\VID_054C&PID_0CE6\\HM0123456789CD", NODE_EMULATED_HUB,
      L"USB\\VID_054C&PID_0CE6&REV_0100\0USB\\VID_054C&PID_0CE6\0", L"USB\\COMPOSITE\0" },
    { L"USB\\VID_054C&PID_0CE6&MI_03\\6&4D5E6F70&0&0003", NODE_DUALSENSE_USB,
      L"USB\\VID_054C&PID_0CE6&REV_0100&MI_03\0USB\\VID_054C&PID_0CE6&MI_03\0", L"USB\\Class_03&SubClass_00&Prot_00\0USB\\Class_03\0" },
    { L"HID\\VID_054C&PID_0CE6&MI_03\\7&5E6F7081&0&0000", NODE_DUALSENSE_USB_INTERFACE,
      L"HID\\VID_054C&PID_0CE6&REV_0100&MI_03\0HID\\VID_054C&PID_0CE6&MI_03\0HID_DEVICE\0", NULL },
    { L"HID\\VID_057E&PID_2069\\1&2D595CA7&0&0000", NODE_ROOT,
      L"HID\\VID_057E&PID_2069&REV_0101\0HID\\VID_057E&PID_2069\0HID\\HIDMaestro\0", NULL }
};

typedef struct Interface
{
    const wchar_t *path;
    int node;
    USHORT vendor_id;
    USHORT product_id;
    const wchar_t *serial; /* What HidD_GetSerialNumberString gives, NULL when the read fails */
    bool present;
    bool open_fails;
    int opens;
    int closes;
} Interface;

enum
{
    INTERFACE_REAL,
    INTERFACE_PERSONA,
    INTERFACE_DUALSENSE,
    INTERFACE_VIRTUAL,
    INTERFACE_COUNT
};

#define HID_CLASS L"#{4d1e55b2-f16f-11cf-88cb-001111000030}"

static Interface interfaces[INTERFACE_COUNT];

static struct
{
    CONFIGRET list_size_result; /* What the list size call returns */
    int small_replies;          /* How many times the list call asks for a larger buffer first */
    int list_size_calls;
    int unexpected;
} fake;

static void Fake_Unexpected(const char *what)
{
    ++fake.unexpected;
    printf("fake: unexpected %s\n", what);
}

/* The tree as scenario 1 finds it: every interface listed and readable */
static void ResetTree(void)
{
    static const Interface initial[INTERFACE_COUNT] = {
        { L"\\\\?\\HID#VID_057E&PID_2069&MI_00#8&3c4d5e6f&0&0000" HID_CLASS, NODE_REAL_HID, 0x057E, 0x2069, L"00", true, false, 0, 0 },
        { L"\\\\?\\HID#VID_057E&PID_2069&MI_00#7&1f2e3d4c&0&0000" HID_CLASS, NODE_PERSONA_HID, 0x057E, 0x2069, L"HM0123456789AB", true, false, 0, 0 },
        { L"\\\\?\\HID#VID_054C&PID_0CE6&MI_03#7&5e6f7081&0&0000" HID_CLASS, NODE_DUALSENSE_HID, 0x054C, 0x0CE6, L"HM0123456789CD", true, false, 0, 0 },
        { L"\\\\?\\HID#VID_057E&PID_2069#1&2d595ca7&0&0000" HID_CLASS, NODE_VIRTUAL_HID, 0x057E, 0x2069, L"HM-CTL-0001", true, false, 0, 0 }
    };

    memcpy(interfaces, initial, sizeof(interfaces));
    nodes[NODE_EMULATED_CONTROLLER].hardware_ids = TOKEN_IDS;
    memset(&fake, 0, sizeof(fake));
    /* The filter remembers each path's answer, and each scenario has its own tree */
    hm_cache_size = 0;
}

static size_t StringListBytes(const wchar_t *list)
{
    const wchar_t *end = list;

    while (*end) {
        end += wcslen(end) + 1;
    }
    return (size_t)(end + 1 - list) * sizeof(wchar_t);
}

/* A property read, as CM_Get_DevNode_PropertyW and its interface twin answer:
   CR_BUFFER_SMALL with the size while the buffer cannot hold the value */
static CONFIGRET PropertyReply(const void *value, size_t bytes, DEVPROPTYPE value_type, DEVPROPTYPE *type, PBYTE buffer, PULONG size)
{
    if (!type || !size) {
        Fake_Unexpected("property read without a type or a size");
        return CR_FAILURE;
    }
    *type = value_type;
    if (!buffer || *size < bytes) {
        *size = (ULONG)bytes;
        return CR_BUFFER_SMALL;
    }
    memcpy(buffer, value, bytes);
    *size = (ULONG)bytes;
    return CR_SUCCESS;
}

static bool IsKey(const DEVPROPKEY *key, const DEVPROPKEY *known)
{
    return memcmp(key, known, sizeof(*key)) == 0;
}

static Interface *InterfaceOfHandle(HANDLE handle)
{
    int i;

    for (i = 0; i < INTERFACE_COUNT; ++i) {
        if (handle == (HANDLE)&interfaces[i]) {
            return &interfaces[i];
        }
    }
    return NULL;
}

static HANDLE WINAPI Fake_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE template_file)
{
    int i;

    (void)share;
    (void)security;
    (void)flags;
    (void)template_file;
    /* The lookup asks for no access, as an enumeration does */
    if (access != 0 || disposition != OPEN_EXISTING) {
        Fake_Unexpected("open with access to the device");
    }
    for (i = 0; i < INTERFACE_COUNT; ++i) {
        if (interfaces[i].present && wcscmp(path, interfaces[i].path) == 0) {
            if (interfaces[i].open_fails) {
                SetLastError(ERROR_ACCESS_DENIED);
                return INVALID_HANDLE_VALUE;
            }
            ++interfaces[i].opens;
            return (HANDLE)&interfaces[i];
        }
    }
    Fake_Unexpected("open of a path that is not listed");
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
}

static BOOL WINAPI Fake_CloseHandle(HANDLE handle)
{
    Interface *iface = InterfaceOfHandle(handle);

    if (!iface || iface->closes >= iface->opens) {
        Fake_Unexpected("close of a handle that is not open");
        return FALSE;
    }
    ++iface->closes;
    return TRUE;
}

static void __stdcall Fake_GetHidGuid(LPGUID guid)
{
    static const GUID hid_class = { 0x4d1e55b2, 0xf16f, 0x11cf, { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

    *guid = hid_class;
}

static BOOLEAN __stdcall Fake_GetAttributes(HANDLE handle, PHIDD_ATTRIBUTES attrib)
{
    const Interface *iface = InterfaceOfHandle(handle);

    if (!iface || attrib->Size != sizeof(*attrib)) {
        Fake_Unexpected("attribute read");
        return FALSE;
    }
    attrib->VendorID = iface->vendor_id;
    attrib->ProductID = iface->product_id;
    attrib->VersionNumber = 0x0101;
    return TRUE;
}

/* buffer_len counts bytes */
static BOOLEAN StringReply(const wchar_t *string, PVOID buffer, ULONG buffer_len)
{
    const size_t bytes = (wcslen(string) + 1) * sizeof(wchar_t);

    if (!buffer || buffer_len < bytes) {
        return FALSE;
    }
    memcpy(buffer, string, bytes);
    return TRUE;
}

static BOOLEAN __stdcall Fake_GetSerialNumberString(HANDLE handle, PVOID buffer, ULONG buffer_len)
{
    const Interface *iface = InterfaceOfHandle(handle);

    if (!iface) {
        Fake_Unexpected("serial number read");
        return FALSE;
    }
    if (!iface->serial) {
        return FALSE;
    }
    return StringReply(iface->serial, buffer, buffer_len);
}

static BOOLEAN __stdcall Fake_GetManufacturerString(HANDLE handle, PVOID buffer, ULONG buffer_len)
{
    (void)handle;
    return StringReply(L"Nintendo", buffer, buffer_len);
}

static BOOLEAN __stdcall Fake_GetProductString(HANDLE handle, PVOID buffer, ULONG buffer_len)
{
    (void)handle;
    return StringReply(L"Switch 2 Pro Controller", buffer, buffer_len);
}

/* The record needs no usage, so the report descriptor is not read */
static BOOLEAN __stdcall Fake_GetPreparsedData(HANDLE handle, PHIDP_PREPARSED_DATA *preparsed_data)
{
    (void)handle;
    (void)preparsed_data;
    return FALSE;
}

static CONFIGRET __stdcall Fake_Locate_DevNodeW(PDEVINST devinst, DEVINSTID_W device_id, ULONG flags)
{
    int i;

    (void)flags;
    for (i = 0; i < NODE_COUNT; ++i) {
        if (_wcsicmp(device_id, nodes[i].instance_id) == 0) {
            *devinst = (DEVINST)(i + 1);
            return CR_SUCCESS;
        }
    }
    return TEST_CR_NO_SUCH_DEVNODE;
}

static CONFIGRET __stdcall Fake_Get_Parent(PDEVINST parent, DEVINST devinst, ULONG flags)
{
    (void)flags;
    if (devinst < 1 || devinst > NODE_COUNT || nodes[devinst - 1].parent < 0) {
        return TEST_CR_NO_SUCH_DEVNODE;
    }
    *parent = (DEVINST)(nodes[devinst - 1].parent + 1);
    return CR_SUCCESS;
}

static CONFIGRET __stdcall Fake_Get_DevNode_PropertyW(DEVINST devinst, CONST DEVPROPKEY *key, DEVPROPTYPE *type, PBYTE buffer, PULONG size, ULONG flags)
{
    const Node *node;

    (void)flags;
    if (devinst < 1 || devinst > NODE_COUNT) {
        Fake_Unexpected("property of a node that is not in the tree");
        return TEST_CR_NO_SUCH_DEVNODE;
    }
    node = &nodes[devinst - 1];
    if (IsKey(key, &DEVPKEY_Device_InstanceId)) {
        return PropertyReply(node->instance_id, (wcslen(node->instance_id) + 1) * sizeof(wchar_t), DEVPROP_TYPE_STRING, type, buffer, size);
    }
    if (IsKey(key, &DEVPKEY_Device_HardwareIds) && node->hardware_ids) {
        return PropertyReply(node->hardware_ids, StringListBytes(node->hardware_ids), DEVPROP_TYPE_STRING_LIST, type, buffer, size);
    }
    if (IsKey(key, &DEVPKEY_Device_CompatibleIds) && node->compatible_ids) {
        return PropertyReply(node->compatible_ids, StringListBytes(node->compatible_ids), DEVPROP_TYPE_STRING_LIST, type, buffer, size);
    }
    return TEST_CR_NO_SUCH_VALUE;
}

static CONFIGRET __stdcall Fake_Get_Device_Interface_PropertyW(LPCWSTR path, CONST DEVPROPKEY *key, DEVPROPTYPE *type, PBYTE buffer, PULONG size, ULONG flags)
{
    int i;

    (void)flags;
    if (!IsKey(key, &DEVPKEY_Device_InstanceId)) {
        return TEST_CR_NO_SUCH_VALUE;
    }
    for (i = 0; i < INTERFACE_COUNT; ++i) {
        if (interfaces[i].present && _wcsicmp(path, interfaces[i].path) == 0) {
            const wchar_t *instance_id = nodes[interfaces[i].node].instance_id;

            return PropertyReply(instance_id, (wcslen(instance_id) + 1) * sizeof(wchar_t), DEVPROP_TYPE_STRING, type, buffer, size);
        }
    }
    return TEST_CR_NO_SUCH_VALUE;
}

/* The listed paths, one after another, closed by an empty one */
static ULONG InterfaceListLength(void)
{
    ULONG length = 1;
    int i;

    for (i = 0; i < INTERFACE_COUNT; ++i) {
        if (interfaces[i].present) {
            length += (ULONG)wcslen(interfaces[i].path) + 1;
        }
    }
    return length;
}

static CONFIGRET __stdcall Fake_Get_Device_Interface_List_SizeW(PULONG length, LPGUID guid, DEVINSTID_W device_id, ULONG flags)
{
    (void)guid;
    (void)device_id;
    (void)flags;
    ++fake.list_size_calls;
    if (fake.list_size_result != CR_SUCCESS) {
        return fake.list_size_result;
    }
    *length = InterfaceListLength();
    return CR_SUCCESS;
}

static CONFIGRET __stdcall Fake_Get_Device_Interface_ListW(LPGUID guid, DEVINSTID_W device_id, WCHAR *buffer, ULONG buffer_length, ULONG flags)
{
    WCHAR *out = buffer;
    int i;

    (void)guid;
    (void)device_id;
    (void)flags;
    /* The list grew between the two calls */
    if (fake.small_replies > 0) {
        --fake.small_replies;
        return CR_BUFFER_SMALL;
    }
    if (buffer_length < InterfaceListLength()) {
        return CR_BUFFER_SMALL;
    }
    for (i = 0; i < INTERFACE_COUNT; ++i) {
        if (interfaces[i].present) {
            const size_t length = wcslen(interfaces[i].path) + 1;

            memcpy(out, interfaces[i].path, length * sizeof(WCHAR));
            out += length;
        }
    }
    *out = L'\0';
    return CR_SUCCESS;
}

/* After each lookup: every opened interface is closed, and the fake saw no
   request the backend should not make */
static void CheckClosed(const char *what)
{
    int i;

    for (i = 0; i < INTERFACE_COUNT; ++i) {
        CHECK(interfaces[i].opens == interfaces[i].closes, "%s: interface %d opens %d closes %d", what, i, interfaces[i].opens, interfaces[i].closes);
    }
    CHECK(fake.unexpected == 0, "%s: %d requests the backend should not make", what, fake.unexpected);
}

static int Owns(unsigned short vendor_id, unsigned short product_id, const char *serial)
{
    return SDL_HidmaestroOwnsUsbSerial(vendor_id, product_id, serial);
}

static void TestOwnership(void)
{
    char ansi_path[512];

    scenario = "a persona beside a real controller";
    ResetTree();
    CHECK(Owns(0x057E, 0x2069, "HM0123456789AB") == 1, "the persona's serial is not owned");
    CHECK(interfaces[INTERFACE_PERSONA].opens == 1, "the persona's interface opened %d times", interfaces[INTERFACE_PERSONA].opens);
    CHECK(interfaces[INTERFACE_REAL].opens == 0, "the real controller's interface opened %d times", interfaces[INTERFACE_REAL].opens);
    CHECK(interfaces[INTERFACE_DUALSENSE].opens == 0 && interfaces[INTERFACE_VIRTUAL].opens == 0,
          "the interfaces listed after the match opened %d and %d times", interfaces[INTERFACE_DUALSENSE].opens, interfaces[INTERFACE_VIRTUAL].opens);
    CheckClosed("the persona's serial");

    /* No match ends the walk, so every HIDMaestro interface is opened for its
       IDs, and the real controller's is still left alone */
    ResetTree();
    CHECK(Owns(0x057E, 0x2069, "00") == 0, "a real controller's serial is owned");
    CHECK(interfaces[INTERFACE_REAL].opens == 0, "the real controller's interface opened %d times", interfaces[INTERFACE_REAL].opens);
    CHECK(interfaces[INTERFACE_PERSONA].opens == 1 && interfaces[INTERFACE_DUALSENSE].opens == 1 && interfaces[INTERFACE_VIRTUAL].opens == 1,
          "the HIDMaestro interfaces opened %d, %d and %d times", interfaces[INTERFACE_PERSONA].opens, interfaces[INTERFACE_DUALSENSE].opens, interfaces[INTERFACE_VIRTUAL].opens);
    CheckClosed("a real controller's serial");

    scenario = "a persona with other IDs";
    ResetTree();
    CHECK(Owns(0x057E, 0x2069, "HM0123456789CD") == 0, "a serial is owned under another persona's IDs");
    CHECK(Owns(0x054C, 0x0CE6, "HM0123456789CD") == 1, "the persona's serial is not owned under its own IDs");
    CHECK(Owns(0x054C, 0x0CE6, "HM0123456789AB") == 0, "a serial is owned under another persona's IDs");
    CHECK(Owns(0x057E, 0x2066, "HM0123456789AB") == 0, "a serial is owned under another product ID");
    CHECK(Owns(0x054C, 0x2069, "HM0123456789AB") == 0, "a serial is owned under another vendor ID");
    CheckClosed("other IDs");

    scenario = "a HIDMaestro device marked in its own hardware IDs";
    ResetTree();
    CHECK(Owns(0x057E, 0x2069, "HM-CTL-0001") == 1, "its serial is not owned");
    CheckClosed("the marked device");

    scenario = "serials that are not a persona's";
    ResetTree();
    CHECK(Owns(0x057E, 0x2069, "HM0123456789A") == 0, "a shorter serial is owned");
    CHECK(Owns(0x057E, 0x2069, "HM0123456789ABC") == 0, "a longer serial is owned");
    CHECK(Owns(0x057E, 0x2069, "hm0123456789ab") == 0, "a serial in another case is owned");
    CheckClosed("other serials");
    CHECK(Owns(0x057E, 0x2069, NULL) == 0 && Owns(0x057E, 0x2069, "") == 0, "no serial is owned");
    CHECK(fake.list_size_calls == 3, "the interface list was read %d times, 3 expected", fake.list_size_calls);

    /* The filter's own answers for the two paths, at the depth every caller walks */
    scenario = "the classification";
    ResetTree();
    CHECK(hid_internal_is_hidmaestro_device(interfaces[INTERFACE_PERSONA].path, HM_FILTER_MAX_DEPTH) == 1, "the persona is not classified");
    CHECK(hid_internal_is_hidmaestro_device(interfaces[INTERFACE_REAL].path, HM_FILTER_MAX_DEPTH) == 0, "the real controller is classified");
    ResetTree();
    CHECK(hid_internal_is_hidmaestro_device(interfaces[INTERFACE_PERSONA].path, HM_FILTER_MAX_DEPTH - 1) == 0, "the token was found one node short of the host controller");
    CHECK(WideCharToMultiByte(CP_ACP, 0, interfaces[INTERFACE_PERSONA].path, -1, ansi_path, (int)sizeof(ansi_path), NULL, NULL) > 0, "path conversion");
    CHECK(SDL_HidmaestroIsAnsiHidPathHm(ansi_path) == 1, "the persona's path is not classified through the ANSI entry point");
    CheckClosed("the classification");
}

static void TestSerialSources(void)
{
    scenario = "the persona's serial string cannot be read";
    ResetTree();
    interfaces[INTERFACE_PERSONA].serial = NULL;
    CHECK(Owns(0x057E, 0x2069, "HM0123456789AB") == 1, "the USB device's instance ID did not supply the serial");
    CheckClosed("an unreadable serial string");

    scenario = "the persona's serial string is empty";
    ResetTree();
    interfaces[INTERFACE_PERSONA].serial = L"";
    CHECK(Owns(0x057E, 0x2069, "HM0123456789AB") == 1, "the USB device's instance ID did not supply the serial");
    CheckClosed("an empty serial string");

    scenario = "the persona's interface cannot be opened";
    ResetTree();
    interfaces[INTERFACE_PERSONA].open_fails = true;
    CHECK(Owns(0x057E, 0x2069, "HM0123456789AB") == 0, "a serial is owned with its interface closed to the lookup");
    CheckClosed("an interface that cannot be opened");

    scenario = "no persona is attached";
    ResetTree();
    interfaces[INTERFACE_PERSONA].present = false;
    CHECK(Owns(0x057E, 0x2069, "HM0123456789AB") == 0, "a serial is owned with no persona attached");
    CHECK(interfaces[INTERFACE_REAL].opens == 0, "the real controller's interface opened %d times", interfaces[INTERFACE_REAL].opens);
    CheckClosed("no persona");

    /* A plain usbip-win2 host controller: the same devices, HIDMaestro's none */
    scenario = "the host controller carries no HIDMaestro token";
    ResetTree();
    nodes[NODE_EMULATED_CONTROLLER].hardware_ids = NO_TOKEN_IDS;
    CHECK(Owns(0x057E, 0x2069, "HM0123456789AB") == 0, "a serial is owned by its look alone");
    CHECK(interfaces[INTERFACE_PERSONA].opens == 0, "an interface nothing classifies opened %d times", interfaces[INTERFACE_PERSONA].opens);
    CHECK(Owns(0x057E, 0x2069, "HM-CTL-0001") == 1, "the device marked in its own hardware IDs is not owned");
    CheckClosed("no token");

    scenario = "a serial outside ASCII";
    ResetTree();
    interfaces[INTERFACE_PERSONA].serial = L"HM\x00C9" L"0001";
    CHECK(Owns(0x057E, 0x2069, "HM?0001") == 1, "the serial is not owned in the form libusb gives it");
    CHECK(Owns(0x057E, 0x2069, "HME0001") == 0, "a plain serial is owned by one outside ASCII");
    CheckClosed("a serial outside ASCII");
}

static void TestInterfaceList(void)
{
    scenario = "the interface list cannot be read";
    ResetTree();
    fake.list_size_result = CR_FAILURE;
    CHECK(Owns(0x057E, 0x2069, "HM0123456789AB") == 0, "a serial is owned with no interface list");
    CheckClosed("no interface list");

    scenario = "the interface list grows while it is read";
    ResetTree();
    fake.small_replies = 2;
    CHECK(Owns(0x057E, 0x2069, "HM0123456789AB") == 1, "the persona's serial is not owned");
    CHECK(fake.list_size_calls == 3, "the list size was read %d times, 3 expected", fake.list_size_calls);
    CheckClosed("a growing interface list");
}

int main(void)
{
    /* What hid_init() would resolve from hid.dll and cfgmgr32.dll. Every
       other pointer stays NULL. */
    HidD_GetHidGuid = Fake_GetHidGuid;
    HidD_GetAttributes = Fake_GetAttributes;
    HidD_GetSerialNumberString = Fake_GetSerialNumberString;
    HidD_GetManufacturerString = Fake_GetManufacturerString;
    HidD_GetProductString = Fake_GetProductString;
    HidD_GetPreparsedData = Fake_GetPreparsedData;
    CM_Locate_DevNodeW = Fake_Locate_DevNodeW;
    CM_Get_Parent = Fake_Get_Parent;
    CM_Get_DevNode_PropertyW = Fake_Get_DevNode_PropertyW;
    CM_Get_Device_Interface_PropertyW = Fake_Get_Device_Interface_PropertyW;
    CM_Get_Device_Interface_List_SizeW = Fake_Get_Device_Interface_List_SizeW;
    CM_Get_Device_Interface_ListW = Fake_Get_Device_Interface_ListW;
    hidapi_initialized = TRUE;

    TestOwnership();
    TestSerialSources();
    TestInterfaceList();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
