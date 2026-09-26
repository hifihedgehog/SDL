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

/*
 * PadForge fork: joysticks that reach Windows only as a COM port, such as
 * RS-232 devices behind a USB-serial adapter. See hifihedgehog/SDL#33 Part 5.
 *
 * The application names each port and its protocol in
 * SDL_HINT_JOYSTICK_SERIAL. Each port gets a thread that owns the handle and
 * runs one protocol module through the engine in src/joystick/serial. The
 * engine and the modules are pure C, tested offline. The thread hands
 * presence and snapshots to the joystick thread under the port's mutex, and
 * never takes the joystick lock.
 */

#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_SERIAL

#include "../SDL_sysjoystick.h"
#include "../../SDL_hints_c.h"
#include "../../core/windows/SDL_windows.h"
#include "../serial/SDL_serial_engine.h"
#include "../serial/SDL_serial_bio2_proto.h"
#include "../serial/SDL_serial_cyberman_proto.h"
#include "../serial/SDL_serial_ibus_proto.h"
#include "../serial/SDL_serial_jvs_proto.h"
#include "../serial/SDL_serial_kettler_proto.h"
#include "../serial/SDL_serial_iforce_proto.h"
#include "../serial/SDL_serial_kfca_proto.h"
#include "../serial/SDL_serial_magellan_proto.h"
#include "../serial/SDL_serial_mastercontroller_proto.h"
#include "../serial/SDL_serial_mdxf_proto.h"
#include "../serial/SDL_serial_panb_proto.h"
#include "../serial/SDL_serial_rvol_proto.h"
#include "../serial/SDL_serial_spaceball_proto.h"
#include "../serial/SDL_serial_spaceorb_proto.h"
#include "../serial/SDL_serial_stinger_proto.h"
#include "../serial/SDL_serial_vrinsight_proto.h"
#include "../serial/SDL_serial_warrior_proto.h"
#include "../serial/SDL_serial_zhenhua_proto.h"
#include "../dji/SDL_dji_remote_proto.h"

/* The pure engine carries the Windows values */
SDL_COMPILE_TIME_ASSERT(serial_noparity, NOPARITY == SDL_SERIAL_NOPARITY);
SDL_COMPILE_TIME_ASSERT(serial_oddparity, ODDPARITY == SDL_SERIAL_ODDPARITY);
SDL_COMPILE_TIME_ASSERT(serial_evenparity, EVENPARITY == SDL_SERIAL_EVENPARITY);
SDL_COMPILE_TIME_ASSERT(serial_onestopbit, ONESTOPBIT == SDL_SERIAL_ONESTOPBIT);
SDL_COMPILE_TIME_ASSERT(serial_twostopbits, TWOSTOPBITS == SDL_SERIAL_TWOSTOPBITS);
SDL_COMPILE_TIME_ASSERT(serial_dtr_disable, DTR_CONTROL_DISABLE == SDL_SERIAL_DTR_CONTROL_DISABLE);
SDL_COMPILE_TIME_ASSERT(serial_dtr_enable, DTR_CONTROL_ENABLE == SDL_SERIAL_DTR_CONTROL_ENABLE);
SDL_COMPILE_TIME_ASSERT(serial_rts_disable, RTS_CONTROL_DISABLE == SDL_SERIAL_RTS_CONTROL_DISABLE);
SDL_COMPILE_TIME_ASSERT(serial_rts_enable, RTS_CONTROL_ENABLE == SDL_SERIAL_RTS_CONTROL_ENABLE);
SDL_COMPILE_TIME_ASSERT(serial_rts_handshake, RTS_CONTROL_HANDSHAKE == SDL_SERIAL_RTS_CONTROL_HANDSHAKE);
SDL_COMPILE_TIME_ASSERT(serial_rts_toggle, RTS_CONTROL_TOGGLE == SDL_SERIAL_RTS_CONTROL_TOGGLE);
SDL_COMPILE_TIME_ASSERT(serial_purge, (PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR) == SDL_SERIAL_PURGE_ALL);
SDL_COMPILE_TIME_ASSERT(serial_setrts, SETRTS == SDL_SERIAL_SETRTS);
SDL_COMPILE_TIME_ASSERT(serial_clrrts, CLRRTS == SDL_SERIAL_CLRRTS);
SDL_COMPILE_TIME_ASSERT(serial_setdtr, SETDTR == SDL_SERIAL_SETDTR);
SDL_COMPILE_TIME_ASSERT(serial_clrdtr, CLRDTR == SDL_SERIAL_CLRDTR);
SDL_COMPILE_TIME_ASSERT(serial_setbreak, SETBREAK == SDL_SERIAL_SETBREAK);
SDL_COMPILE_TIME_ASSERT(serial_clrbreak, CLRBREAK == SDL_SERIAL_CLRBREAK);
SDL_COMPILE_TIME_ASSERT(serial_maxdword, MAXDWORD == SDL_SERIAL_MAXDWORD);
SDL_COMPILE_TIME_ASSERT(serial_hat_up, SDL_HAT_UP == SDL_SERIAL_HAT_UP);
SDL_COMPILE_TIME_ASSERT(serial_hat_right, SDL_HAT_RIGHT == SDL_SERIAL_HAT_RIGHT);
SDL_COMPILE_TIME_ASSERT(serial_hat_down, SDL_HAT_DOWN == SDL_SERIAL_HAT_DOWN);
SDL_COMPILE_TIME_ASSERT(serial_hat_left, SDL_HAT_LEFT == SDL_SERIAL_HAT_LEFT);
SDL_COMPILE_TIME_ASSERT(serial_type_unknown, SDL_JOYSTICK_TYPE_UNKNOWN == SDL_SERIAL_TYPE_UNKNOWN);
SDL_COMPILE_TIME_ASSERT(serial_type_gamepad, SDL_JOYSTICK_TYPE_GAMEPAD == SDL_SERIAL_TYPE_GAMEPAD);
SDL_COMPILE_TIME_ASSERT(serial_type_arcade, SDL_JOYSTICK_TYPE_ARCADE_STICK == SDL_SERIAL_TYPE_ARCADE_STICK);
SDL_COMPILE_TIME_ASSERT(serial_type_flight, SDL_JOYSTICK_TYPE_FLIGHT_STICK == SDL_SERIAL_TYPE_FLIGHT_STICK);
SDL_COMPILE_TIME_ASSERT(serial_type_dance, SDL_JOYSTICK_TYPE_DANCE_PAD == SDL_SERIAL_TYPE_DANCE_PAD);
SDL_COMPILE_TIME_ASSERT(serial_type_drum, SDL_JOYSTICK_TYPE_DRUM_KIT == SDL_SERIAL_TYPE_DRUM_KIT);

/* cfgmgr32, loaded at run time as SDL_hid.c loads it */
#define SERIAL_CR_SUCCESS                           0x00000000
#define SERIAL_CR_BUFFER_SMALL                      0x0000001A
#define SERIAL_CM_GET_DEVICE_INTERFACE_LIST_PRESENT 0x00000000
#define SERIAL_CM_LOCATE_DEVNODE_NORMAL             0x00000000
#define SERIAL_CM_REGISTRY_HARDWARE                 0x00000000
#define SERIAL_REGDISPOSITION_OPENEXISTING          0x00000001
#define SERIAL_DEVPROP_TYPE_STRING                  0x00000012
#define SERIAL_CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE 0
#define SERIAL_CM_NOTIFY_ACTION_ARRIVAL             0
#define SERIAL_CM_NOTIFY_ACTION_REMOVAL             1

typedef DWORD SERIAL_CONFIGRET;
typedef DWORD SERIAL_DEVINST;
typedef void *SERIAL_HCMNOTIFICATION;

typedef struct SERIAL_DEVPROPKEY
{
    GUID fmtid;
    ULONG pid;
} SERIAL_DEVPROPKEY;

typedef struct SERIAL_CM_NOTIFY_FILTER
{
    DWORD cbSize;
    DWORD Flags;
    DWORD FilterType;
    DWORD Reserved;
    union
    {
        struct
        {
            GUID ClassGuid;
        } DeviceInterface;
        struct
        {
            HANDLE hTarget;
        } DeviceHandle;
        struct
        {
            WCHAR InstanceId[200];
        } DeviceInstance;
    } u;
} SERIAL_CM_NOTIFY_FILTER;

typedef DWORD(CALLBACK *SERIAL_CM_NOTIFY_CALLBACK)(SERIAL_HCMNOTIFICATION notification, PVOID context, DWORD action, PVOID data, DWORD size);
typedef SERIAL_CONFIGRET(WINAPI *SERIAL_CM_Get_Device_Interface_List_SizeW)(PULONG length, LPGUID guid, WCHAR *device, ULONG flags);
typedef SERIAL_CONFIGRET(WINAPI *SERIAL_CM_Get_Device_Interface_ListW)(LPGUID guid, WCHAR *device, WCHAR *buffer, ULONG length, ULONG flags);
typedef SERIAL_CONFIGRET(WINAPI *SERIAL_CM_Get_Device_Interface_PropertyW)(LPCWSTR interface_path, const SERIAL_DEVPROPKEY *key, ULONG *type, PBYTE buffer, PULONG size, ULONG flags);
typedef SERIAL_CONFIGRET(WINAPI *SERIAL_CM_Locate_DevNodeW)(SERIAL_DEVINST *devinst, WCHAR *device, ULONG flags);
typedef SERIAL_CONFIGRET(WINAPI *SERIAL_CM_Open_DevNode_Key)(SERIAL_DEVINST devinst, REGSAM access, ULONG profile, DWORD disposition, PHKEY key, ULONG flags);
typedef SERIAL_CONFIGRET(WINAPI *SERIAL_CM_Register_Notification)(SERIAL_CM_NOTIFY_FILTER *filter, PVOID context, SERIAL_CM_NOTIFY_CALLBACK callback, SERIAL_HCMNOTIFICATION *notification);
typedef SERIAL_CONFIGRET(WINAPI *SERIAL_CM_Unregister_Notification)(SERIAL_HCMNOTIFICATION notification);
typedef BOOL(WINAPI *SERIAL_CancelIoEx)(HANDLE file, LPOVERLAPPED overlapped);

/* GUID_DEVINTERFACE_COMPORT, {86E0D1E0-8089-11D0-9CE4-08003E301F73} */
static GUID serial_guid_comport = { 0x86E0D1E0, 0x8089, 0x11D0, { 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73 } };
/* DEVPKEY_Device_InstanceId */
static const SERIAL_DEVPROPKEY serial_devpkey_instance_id = { { 0x78C34FC8, 0x104A, 0x4ACA, { 0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57 } }, 256 };

static const SDL_SerialModule *const serial_modules[] = {
    &SDL_SerialSpaceballModule,
    &SDL_SerialSpaceOrbModule,
    &SDL_SerialMagellanModule,
    &SDL_SerialStingerModule,
    &SDL_SerialWarriorModule,
    &SDL_SerialCyberManModule,
    &SDL_SerialZhenHuaModule,
    &SDL_SerialIBusModule,
    &SDL_SerialJVSModule,
    &SDL_SerialVRinsightModule,
    &SDL_SerialKettlerModule,
    &SDL_SerialIForceModule,
    &SDL_SerialMasterControllerModule,
    &SDL_DJIRemoteRCN1Module,
    &SDL_DJIRemoteMavicMiniModule,
    &SDL_DJIRemotePhantom3Module,
    &SDL_DJIRemotePhantom2Module,
    &SDL_SerialBIO2Module,
    &SDL_SerialBIO2IIDXModule,
    &SDL_SerialBIO2SDVXModule,
    &SDL_SerialKFCAModule,
    &SDL_SerialPANBModule,
    &SDL_SerialRVOLModule,
    &SDL_SerialMDXFModule
};

/* The Konami ACIO modules, which SDL_HINT_JOYSTICK_KONAMI_ACIO turns off */
static const SDL_SerialModule *const serial_acio_modules[] = {
    &SDL_SerialBIO2Module,
    &SDL_SerialBIO2IIDXModule,
    &SDL_SerialBIO2SDVXModule,
    &SDL_SerialKFCAModule,
    &SDL_SerialPANBModule,
    &SDL_SerialRVOLModule,
    &SDL_SerialMDXFModule
};

/* Ports whose device identifies itself, by device instance ID prefix. The
 * parts that add such devices add rows. The last row is empty. */
static const SDL_SerialAutoRule serial_auto_rules[] = {
    /* The protocol port of the DJI RC-N1 family, interface 2. DJI's VCOM
       driver binds it on PIDs 1020 and 1030, and other RC-N1 PIDs are not
       recorded, so any DJI PID matches (hifihedgehog/SDL#33 Part 6). */
    { "USB\\VID_2CA3&PID_????&MI_02\\", "dji", 0x2CA3, 0 },
    /* The Konami BIO2, a COM port on Windows' own USB serial driver. A row
       matches the device and any interface of it, as bemanitools' search by
       ID does. Token bio2 takes its mode from
       SDL_HINT_JOYSTICK_KONAMI_BIO2_MODE (hifihedgehog/SDL#33 Part 14). */
    { "USB\\VID_1CCF&PID_804C", "bio2", 0x1CCF, 0x804C },
    { "USB\\VID_1CCF&PID_8040", "bio2", 0x1CCF, 0x8040 },
    { NULL, NULL, 0, 0 }
};

typedef struct SERIAL_Sub
{
    /* Written by the port thread under the port's mutex */
    uint32_t generation; /* 0 while absent */
    SDL_SerialIdentity identity;
    SDL_SerialControls latest;
    uint64_t latest_sequence;
    bool output_set;
    SDL_SerialOutput output;

    /* The joystick thread's */
    uint32_t registered; /* The generation it has a joystick for, 0 when none */
    SDL_JoystickID instance_id;
    SDL_GUID guid;
    SDL_SerialIdentity joystick_identity;
} SERIAL_Sub;

typedef struct SERIAL_Port
{
    char key[SDL_SERIAL_KEY_LENGTH]; /* As written in the hint, or an instance ID */
    const SDL_SerialModule *module;
    const SDL_SerialAutoRule *rule; /* For a port that identifies itself */
    uint16_t usb_vendor;            /* For a port that identifies itself */
    uint16_t usb_product;
    bool auto_seen;                 /* Its device was present at the last scan */
    void *module_state;
    SDL_SerialEngine engine;
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    SDL_AtomicInt presence_changed;

    /* The port thread's */
    HANDLE handle;
    HANDLE stop_event;
    HANDLE read_event;
    HANDLE write_event;
    HANDLE output_event;
    HANDLE rescan_event;
    OVERLAPPED read_overlapped;
    OVERLAPPED write_overlapped;
    bool read_pending;
    bool write_pending;
    bool read_idle; /* A read finished at once with nothing */
    DWORD write_length;
    DWORD logged_error;
    uint8_t read_buffer[SDL_SERIAL_READ_SIZE];
    uint8_t write_buffer[SDL_SERIAL_MAX_WRITE];

    /* Under the mutex */
    SDL_SerialSnapshotQueue queue;
    uint64_t sequence;
    SERIAL_Sub subs[SDL_SERIAL_MAX_SUBDEVICES];

    struct SERIAL_Port *next;
} SERIAL_Port;

struct joystick_hwdata
{
    SERIAL_Port *port; /* NULL once the port has stopped */
    int sub;
    uint32_t generation;
    uint64_t sequence; /* The last snapshot sent */
    bool send_initial; /* The state Open took is not sent yet */
    Uint64 initial_stamp;
    SDL_SerialControls initial;
};

static SDL_Mutex *serial_lock; /* The hint string, and the port list for the notification callback */
static char *serial_hint;
static SDL_AtomicInt serial_hint_changed;
static SDL_AtomicInt serial_rescan;
static bool serial_auto;
static bool serial_acio;                             /* SDL_HINT_JOYSTICK_KONAMI_ACIO */
static const SDL_SerialModule *serial_bio2_module;   /* What token bio2 runs, from SDL_HINT_JOYSTICK_KONAMI_BIO2_MODE */
static SERIAL_Port *serial_ports;
static HMODULE serial_cfgmgr32;
static SERIAL_CM_Get_Device_Interface_List_SizeW serial_list_size;
static SERIAL_CM_Get_Device_Interface_ListW serial_list;
static SERIAL_CM_Get_Device_Interface_PropertyW serial_interface_property;
static SERIAL_CM_Locate_DevNodeW serial_locate_devnode;
static SERIAL_CM_Open_DevNode_Key serial_open_devnode_key;
static SERIAL_CM_Register_Notification serial_register_notification;
static SERIAL_CM_Unregister_Notification serial_unregister_notification;
static SERIAL_HCMNOTIFICATION serial_notification;
static SERIAL_CancelIoEx serial_cancel_io_ex;

static void SERIAL_Log(const SERIAL_Port *port, const char *text)
{
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Serial joystick %s (%s): %s", port->key, port->module->token, text);
}

/* A failing call is logged once until the error changes */
static void SERIAL_LogError(SERIAL_Port *port, const char *call, DWORD error)
{
    if (error == port->logged_error) {
        return;
    }
    port->logged_error = error;
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Serial joystick %s (%s): %s failed with error %lu", port->key, port->module->token, call, (unsigned long)error);
}

static DWORD CALLBACK SERIAL_Notification(SERIAL_HCMNOTIFICATION notification, PVOID context, DWORD action, PVOID data, DWORD size)
{
    (void)notification;
    (void)context;
    (void)data;
    (void)size;
    if (action == SERIAL_CM_NOTIFY_ACTION_ARRIVAL || action == SERIAL_CM_NOTIFY_ACTION_REMOVAL) {
        SERIAL_Port *port;

        SDL_SetAtomicInt(&serial_rescan, 1);
        SDL_LockMutex(serial_lock);
        for (port = serial_ports; port; port = port->next) {
            SetEvent(port->rescan_event);
        }
        SDL_UnlockMutex(serial_lock);
    }
    return ERROR_SUCCESS;
}

static void SERIAL_LoadConfigManager(void)
{
    SERIAL_CM_NOTIFY_FILTER filter;

    if (serial_cfgmgr32) {
        return;
    }
    serial_cfgmgr32 = LoadLibraryW(L"cfgmgr32.dll");
    if (!serial_cfgmgr32) {
        return;
    }
    serial_list_size = (SERIAL_CM_Get_Device_Interface_List_SizeW)GetProcAddress(serial_cfgmgr32, "CM_Get_Device_Interface_List_SizeW");
    serial_list = (SERIAL_CM_Get_Device_Interface_ListW)GetProcAddress(serial_cfgmgr32, "CM_Get_Device_Interface_ListW");
    serial_interface_property = (SERIAL_CM_Get_Device_Interface_PropertyW)GetProcAddress(serial_cfgmgr32, "CM_Get_Device_Interface_PropertyW");
    serial_locate_devnode = (SERIAL_CM_Locate_DevNodeW)GetProcAddress(serial_cfgmgr32, "CM_Locate_DevNodeW");
    serial_open_devnode_key = (SERIAL_CM_Open_DevNode_Key)GetProcAddress(serial_cfgmgr32, "CM_Open_DevNode_Key");
    serial_register_notification = (SERIAL_CM_Register_Notification)GetProcAddress(serial_cfgmgr32, "CM_Register_Notification");
    serial_unregister_notification = (SERIAL_CM_Unregister_Notification)GetProcAddress(serial_cfgmgr32, "CM_Unregister_Notification");
    if (serial_register_notification && serial_unregister_notification) {
        SDL_zero(filter);
        filter.cbSize = sizeof(filter);
        filter.FilterType = SERIAL_CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        filter.u.DeviceInterface.ClassGuid = serial_guid_comport;
        if (serial_register_notification(&filter, NULL, SERIAL_Notification, &serial_notification) != SERIAL_CR_SUCCESS) {
            serial_notification = NULL;
        }
    }
}

static void SERIAL_UnloadConfigManager(void)
{
    if (serial_notification && serial_unregister_notification) {
        serial_unregister_notification(serial_notification);
    }
    serial_notification = NULL;
    if (serial_cfgmgr32) {
        FreeLibrary(serial_cfgmgr32);
        serial_cfgmgr32 = NULL;
    }
    serial_list_size = NULL;
    serial_list = NULL;
    serial_interface_property = NULL;
    serial_locate_devnode = NULL;
    serial_open_devnode_key = NULL;
    serial_register_notification = NULL;
    serial_unregister_notification = NULL;
}

/* The present COM port interfaces, a double-NUL-terminated list */
static WCHAR *SERIAL_ListInterfaces(void)
{
    WCHAR *list = NULL;
    ULONG length = 0;
    SERIAL_CONFIGRET cr;

    if (!serial_list_size || !serial_list) {
        return NULL;
    }
    /* The list can grow between the two calls */
    do {
        cr = serial_list_size(&length, &serial_guid_comport, NULL, SERIAL_CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr != SERIAL_CR_SUCCESS) {
            break;
        }
        SDL_free(list);
        list = (WCHAR *)SDL_calloc(length ? length : 1, sizeof(WCHAR));
        if (!list) {
            return NULL;
        }
        cr = serial_list(&serial_guid_comport, NULL, list, length, SERIAL_CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    } while (cr == SERIAL_CR_BUFFER_SMALL);
    if (cr != SERIAL_CR_SUCCESS) {
        SDL_free(list);
        return NULL;
    }
    return list;
}

static char *SERIAL_InterfaceInstanceId(const WCHAR *interface_path)
{
    ULONG size = 0, type = 0;
    BYTE *buffer;
    char *result;

    if (!serial_interface_property ||
        serial_interface_property(interface_path, &serial_devpkey_instance_id, &type, NULL, &size, 0) != SERIAL_CR_BUFFER_SMALL ||
        type != SERIAL_DEVPROP_TYPE_STRING || size < sizeof(WCHAR)) {
        return NULL;
    }
    buffer = (BYTE *)SDL_calloc(1, size + sizeof(WCHAR));
    if (!buffer) {
        return NULL;
    }
    if (serial_interface_property(interface_path, &serial_devpkey_instance_id, &type, buffer, &size, 0) != SERIAL_CR_SUCCESS) {
        SDL_free(buffer);
        return NULL;
    }
    result = WIN_StringToUTF8W((WCHAR *)buffer);
    SDL_free(buffer);
    return result;
}

/* The interface path of the first present COM port whose device instance ID
 * starts with the prefix */
static WCHAR *SERIAL_FindInterface(const char *prefix)
{
    WCHAR *list = SERIAL_ListInterfaces();
    WCHAR *interface_path, *result = NULL;

    if (!list) {
        return NULL;
    }
    for (interface_path = list; *interface_path; interface_path += SDL_wcslen(interface_path) + 1) {
        char *instance_id = SERIAL_InterfaceInstanceId(interface_path);
        const bool match = instance_id && SDL_Serial_InstanceMatches(prefix, instance_id);

        SDL_free(instance_id);
        if (match) {
            result = SDL_wcsdup(interface_path);
            break;
        }
    }
    SDL_free(list);
    return result;
}

/* The COMn name the ports class gave a device, from its hardware key */
static bool SERIAL_PortName(const char *instance_id, unsigned int *number)
{
    WCHAR *wide = WIN_UTF8ToStringW(instance_id);
    SERIAL_DEVINST devinst = 0;
    HKEY key = NULL;
    WCHAR name[32];
    DWORD size = sizeof(name) - sizeof(WCHAR), type = 0;
    char *utf8;
    bool result = false;

    if (!wide || !serial_locate_devnode || !serial_open_devnode_key ||
        serial_locate_devnode(&devinst, wide, SERIAL_CM_LOCATE_DEVNODE_NORMAL) != SERIAL_CR_SUCCESS ||
        serial_open_devnode_key(devinst, KEY_QUERY_VALUE, 0, SERIAL_REGDISPOSITION_OPENEXISTING, &key, SERIAL_CM_REGISTRY_HARDWARE) != SERIAL_CR_SUCCESS) {
        SDL_free(wide);
        return false;
    }
    SDL_free(wide);
    SDL_zeroa(name);
    if (RegQueryValueExW(key, L"PortName", NULL, &type, (LPBYTE)name, &size) == ERROR_SUCCESS && type == REG_SZ) {
        utf8 = WIN_StringToUTF8W(name);
        result = utf8 && SDL_Serial_IsComKey(utf8, number);
        SDL_free(utf8);
    }
    RegCloseKey(key);
    return result;
}

static bool SERIAL_Open(void *userdata)
{
    SERIAL_Port *port = (SERIAL_Port *)userdata;
    WCHAR *path = NULL;
    unsigned int number = 0;

    if (SDL_Serial_IsComKey(port->key, &number)) {
        /* \\.\COMn opens every port number */
        path = (WCHAR *)SDL_malloc(32 * sizeof(WCHAR));
        if (path) {
            SDL_swprintf(path, 32, L"\\\\.\\COM%u", number);
        }
    } else {
        path = SERIAL_FindInterface(port->key);
        if (!path) {
            SERIAL_LogError(port, "Finding a present COM port with this device instance ID", ERROR_FILE_NOT_FOUND);
            return false;
        }
    }
    if (!path) {
        return false;
    }
    /* Share mode 0, OPEN_EXISTING and no template, as a communications
     * resource requires */
    port->handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    SDL_free(path);
    if (port->handle == INVALID_HANDLE_VALUE) {
        SERIAL_LogError(port, "CreateFile", GetLastError());
        return false;
    }
    port->logged_error = 0;
    port->read_pending = false;
    port->write_pending = false;
    port->read_idle = false;
    ResetEvent(port->read_event);
    ResetEvent(port->write_event);
    SERIAL_Log(port, "opened");
    return true;
}

static bool SERIAL_SetLine(void *userdata, const SDL_SerialLineConfig *config)
{
    SERIAL_Port *port = (SERIAL_Port *)userdata;
    DCB dcb;

    SDL_zero(dcb);
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(port->handle, &dcb)) {
        SERIAL_LogError(port, "GetCommState", GetLastError());
        return false;
    }
    dcb.BaudRate = config->BaudRate;
    dcb.ByteSize = config->ByteSize;
    dcb.Parity = config->Parity;
    dcb.StopBits = config->StopBits;
    dcb.fBinary = config->fBinary;
    dcb.fParity = config->fParity;
    dcb.fOutxCtsFlow = config->fOutxCtsFlow;
    dcb.fOutxDsrFlow = config->fOutxDsrFlow;
    dcb.fDtrControl = config->fDtrControl;
    dcb.fDsrSensitivity = config->fDsrSensitivity;
    dcb.fTXContinueOnXoff = config->fTXContinueOnXoff;
    dcb.fOutX = config->fOutX;
    dcb.fInX = config->fInX;
    dcb.fErrorChar = config->fErrorChar;
    dcb.fNull = config->fNull;
    dcb.fRtsControl = config->fRtsControl;
    dcb.fAbortOnError = config->fAbortOnError;
    if (!SetCommState(port->handle, &dcb)) {
        SERIAL_LogError(port, "SetCommState", GetLastError());
        return false;
    }
    return true;
}

static bool SERIAL_SetTimeouts(void *userdata, const SDL_SerialTimeouts *timeouts)
{
    SERIAL_Port *port = (SERIAL_Port *)userdata;
    COMMTIMEOUTS commtimeouts;

    SDL_zero(commtimeouts);
    commtimeouts.ReadIntervalTimeout = timeouts->ReadIntervalTimeout;
    commtimeouts.ReadTotalTimeoutMultiplier = timeouts->ReadTotalTimeoutMultiplier;
    commtimeouts.ReadTotalTimeoutConstant = timeouts->ReadTotalTimeoutConstant;
    commtimeouts.WriteTotalTimeoutMultiplier = timeouts->WriteTotalTimeoutMultiplier;
    commtimeouts.WriteTotalTimeoutConstant = timeouts->WriteTotalTimeoutConstant;
    if (!SetCommTimeouts(port->handle, &commtimeouts)) {
        SERIAL_LogError(port, "SetCommTimeouts", GetLastError());
        return false;
    }
    return true;
}

static bool SERIAL_Purge(void *userdata, uint32_t flags)
{
    SERIAL_Port *port = (SERIAL_Port *)userdata;

    if (!PurgeComm(port->handle, flags)) {
        SERIAL_LogError(port, "PurgeComm", GetLastError());
        return false;
    }
    return true;
}

static bool SERIAL_Escape(void *userdata, uint32_t function)
{
    SERIAL_Port *port = (SERIAL_Port *)userdata;

    if (!EscapeCommFunction(port->handle, function)) {
        SERIAL_LogError(port, "EscapeCommFunction", GetLastError());
        return false;
    }
    return true;
}

static SDL_SerialIO SERIAL_Write(void *userdata, const uint8_t *data, size_t length)
{
    SERIAL_Port *port = (SERIAL_Port *)userdata;
    DWORD written = 0;

    if (length > sizeof(port->write_buffer)) {
        return SDL_SERIAL_IO_FAILED;
    }
    SDL_memcpy(port->write_buffer, data, length);
    port->write_length = (DWORD)length;
    SDL_zero(port->write_overlapped);
    port->write_overlapped.hEvent = port->write_event;
    ResetEvent(port->write_event);
    if (WriteFile(port->handle, port->write_buffer, port->write_length, &written, &port->write_overlapped)) {
        /* Done at once: the completion goes through the same path */
        port->write_pending = true;
        SetEvent(port->write_event);
        return SDL_SERIAL_IO_PENDING;
    }
    if (GetLastError() == ERROR_IO_PENDING) {
        port->write_pending = true;
        return SDL_SERIAL_IO_PENDING;
    }
    SERIAL_LogError(port, "WriteFile", GetLastError());
    return SDL_SERIAL_IO_FAILED;
}

static bool SERIAL_Drain(void *userdata)
{
    SERIAL_Port *port = (SERIAL_Port *)userdata;

    /* On a communications handle this returns once the written bytes are sent */
    if (!FlushFileBuffers(port->handle)) {
        SERIAL_LogError(port, "FlushFileBuffers", GetLastError());
        return false;
    }
    return true;
}

static void SERIAL_Close(void *userdata)
{
    SERIAL_Port *port = (SERIAL_Port *)userdata;
    DWORD transferred = 0;

    if (port->handle == INVALID_HANDLE_VALUE) {
        return;
    }
    /* Cancel, then wait for each pending operation before its buffer is reused */
    if (serial_cancel_io_ex) {
        serial_cancel_io_ex(port->handle, NULL);
    } else {
        CancelIo(port->handle);
    }
    if (port->read_pending) {
        GetOverlappedResult(port->handle, &port->read_overlapped, &transferred, TRUE);
        port->read_pending = false;
    }
    if (port->write_pending) {
        GetOverlappedResult(port->handle, &port->write_overlapped, &transferred, TRUE);
        port->write_pending = false;
    }
    CloseHandle(port->handle);
    port->handle = INVALID_HANDLE_VALUE;
    SERIAL_Log(port, "closed");
}

static void SERIAL_Presence(void *userdata, int sub, uint32_t generation, const SDL_SerialIdentity *identity)
{
    SERIAL_Port *port = (SERIAL_Port *)userdata;
    SERIAL_Sub *s = &port->subs[sub];

    SDL_LockMutex(port->mutex);
    s->generation = generation;
    if (identity) {
        s->identity = *identity;
    }
    SDL_zero(s->latest);
    SDL_UnlockMutex(port->mutex);
    SDL_SetAtomicInt(&port->presence_changed, 1);
}

static void SERIAL_Publish(void *userdata, const SDL_SerialQueueEntry *entry)
{
    SERIAL_Port *port = (SERIAL_Port *)userdata;
    SDL_SerialQueueEntry numbered = *entry;
    SERIAL_Sub *s = &port->subs[entry->sub];

    SDL_LockMutex(port->mutex);
    numbered.sequence = ++port->sequence;
    SDL_Serial_PushQueue(&port->queue, &numbered);
    s->latest = entry->controls;
    s->latest.ball[0] = 0;
    s->latest.ball[1] = 0;
    s->latest_sequence = numbered.sequence;
    SDL_UnlockMutex(port->mutex);
}

static void SERIAL_ModuleLog(void *userdata, const char *text)
{
    SERIAL_Log((SERIAL_Port *)userdata, text);
}

static const SDL_SerialPortOps serial_ops = {
    SERIAL_Open,
    SERIAL_SetLine,
    SERIAL_SetTimeouts,
    SERIAL_Purge,
    SERIAL_Escape,
    SERIAL_Write,
    SERIAL_Drain,
    SERIAL_Close,
    SERIAL_Presence,
    SERIAL_Publish,
    SERIAL_ModuleLog
};

/* Keeps one read posted while the engine reads. Reads that finish at once
 * are fed here. */
static void SERIAL_ReadMore(SERIAL_Port *port)
{
    int rounds;

    port->read_idle = false;
    for (rounds = 0; rounds < 16; ++rounds) {
        DWORD transferred = 0;

        if (port->read_pending || port->handle == INVALID_HANDLE_VALUE || !SDL_SerialEngine_IsReading(&port->engine)) {
            return;
        }
        SDL_zero(port->read_overlapped);
        port->read_overlapped.hEvent = port->read_event;
        ResetEvent(port->read_event);
        if (ReadFile(port->handle, port->read_buffer, sizeof(port->read_buffer), &transferred, &port->read_overlapped)) {
            if (transferred == 0) {
                port->read_idle = true;
                return;
            }
            SDL_SerialEngine_Received(&port->engine, port->read_buffer, transferred, SDL_GetTicksNS());
            continue;
        }
        if (GetLastError() == ERROR_IO_PENDING) {
            port->read_pending = true;
            return;
        }
        SERIAL_LogError(port, "ReadFile", GetLastError());
        SDL_SerialEngine_Lost(&port->engine, SDL_GetTicksNS());
        return;
    }
}

static void SERIAL_ReadDone(SERIAL_Port *port)
{
    DWORD transferred = 0;

    if (!GetOverlappedResult(port->handle, &port->read_overlapped, &transferred, FALSE)) {
        const DWORD error = GetLastError();

        if (error == ERROR_IO_INCOMPLETE) {
            return;
        }
        port->read_pending = false;
        SERIAL_LogError(port, "ReadFile", error);
        SDL_SerialEngine_Lost(&port->engine, SDL_GetTicksNS());
        return;
    }
    port->read_pending = false;
    /* 0 bytes is the 1000 ms read timeout. The next read goes out. */
    if (transferred > 0) {
        SDL_SerialEngine_Received(&port->engine, port->read_buffer, transferred, SDL_GetTicksNS());
    }
}

static void SERIAL_WriteDone(SERIAL_Port *port)
{
    DWORD transferred = 0;

    if (!GetOverlappedResult(port->handle, &port->write_overlapped, &transferred, FALSE)) {
        const DWORD error = GetLastError();

        if (error == ERROR_IO_INCOMPLETE) {
            return;
        }
        port->write_pending = false;
        SERIAL_LogError(port, "WriteFile", error);
        SDL_SerialEngine_Lost(&port->engine, SDL_GetTicksNS());
        return;
    }
    port->write_pending = false;
    /* A write short of its length ran into the write timeout */
    SDL_SerialEngine_WriteDone(&port->engine, transferred == port->write_length, SDL_GetTicksNS());
}

static void SERIAL_TakeOutput(SERIAL_Port *port)
{
    SDL_SerialOutput requests[SDL_SERIAL_MAX_SUBDEVICES];
    bool set[SDL_SERIAL_MAX_SUBDEVICES];
    int sub;

    SDL_LockMutex(port->mutex);
    for (sub = 0; sub < SDL_SERIAL_MAX_SUBDEVICES; ++sub) {
        set[sub] = port->subs[sub].output_set;
        requests[sub] = port->subs[sub].output;
        port->subs[sub].output_set = false;
    }
    SDL_UnlockMutex(port->mutex);
    for (sub = 0; sub < SDL_SERIAL_MAX_SUBDEVICES; ++sub) {
        if (set[sub]) {
            SDL_SerialEngine_Output(&port->engine, &requests[sub], SDL_GetTicksNS());
        }
    }
}

typedef enum SERIAL_Wake
{
    SERIAL_WAKE_STOP,
    SERIAL_WAKE_OUTPUT,
    SERIAL_WAKE_RESCAN,
    SERIAL_WAKE_READ,
    SERIAL_WAKE_WRITE
} SERIAL_Wake;

/* Only this thread touches the handle and the module state. A module whose
 * device must hear from the host before the port closes, such as a
 * streaming PANB, keeps the thread through its close sequence, which
 * SDL_SERIAL_CLOSE_MS bounds. */
static int SDLCALL SERIAL_PortThread(void *data)
{
    SERIAL_Port *port = (SERIAL_Port *)data;
    bool stopping = false;

    SDL_SerialEngine_Init(&port->engine, &serial_ops, port, port->module, port->module_state, SDL_GetTicksNS());
    for (;;) {
        HANDLE handles[5];
        SERIAL_Wake wakes[5];
        DWORD count = 0, timeout = INFINITE, result;
        uint64_t deadline;

        SDL_SerialEngine_Run(&port->engine, SDL_GetTicksNS());
        SERIAL_ReadMore(port);
        if (stopping && !SDL_SerialEngine_IsStopping(&port->engine)) {
            /* The close sequence ended, or the port was lost during it. A
               read that finished at once can end it, so this follows the
               reads: a closed port has nothing left to wait for. */
            SDL_SerialEngine_Stop(&port->engine);
            return 0;
        }
        if (SDL_SerialEngine_GetDeadline(&port->engine, &deadline)) {
            const Uint64 now = SDL_GetTicks();

            timeout = (deadline <= now) ? 0 : (DWORD)SDL_min(deadline - now, (Uint64)0x7FFFFFFF);
        }
        if (port->read_idle && timeout > 10) {
            timeout = 10;
        }
        if (!stopping) {
            handles[count] = port->stop_event;
            wakes[count++] = SERIAL_WAKE_STOP;
        }
        handles[count] = port->output_event;
        wakes[count++] = SERIAL_WAKE_OUTPUT;
        handles[count] = port->rescan_event;
        wakes[count++] = SERIAL_WAKE_RESCAN;
        if (port->read_pending) {
            handles[count] = port->read_event;
            wakes[count++] = SERIAL_WAKE_READ;
        }
        if (port->write_pending) {
            handles[count] = port->write_event;
            wakes[count++] = SERIAL_WAKE_WRITE;
        }
        result = WaitForMultipleObjects(count, handles, FALSE, timeout);
        if (result == WAIT_TIMEOUT) {
            continue;
        }
        if (result >= WAIT_OBJECT_0 + count) {
            SERIAL_LogError(port, "WaitForMultipleObjects", GetLastError());
            SDL_Delay(100);
            continue;
        }
        switch (wakes[result - WAIT_OBJECT_0]) {
        case SERIAL_WAKE_STOP:
            stopping = true;
            if (!SDL_SerialEngine_BeginStop(&port->engine, SDL_GetTicksNS())) {
                SDL_SerialEngine_Stop(&port->engine);
                return 0;
            }
            break;
        case SERIAL_WAKE_OUTPUT:
            SERIAL_TakeOutput(port);
            break;
        case SERIAL_WAKE_RESCAN:
            SDL_SerialEngine_Rescan(&port->engine, SDL_GetTicksNS());
            break;
        case SERIAL_WAKE_READ:
            SERIAL_ReadDone(port);
            break;
        case SERIAL_WAKE_WRITE:
            SERIAL_WriteDone(port);
            break;
        }
    }
}

static void SERIAL_FreePort(SERIAL_Port *port)
{
    if (port->stop_event) {
        CloseHandle(port->stop_event);
    }
    if (port->read_event) {
        CloseHandle(port->read_event);
    }
    if (port->write_event) {
        CloseHandle(port->write_event);
    }
    if (port->output_event) {
        CloseHandle(port->output_event);
    }
    if (port->rescan_event) {
        CloseHandle(port->rescan_event);
    }
    if (port->mutex) {
        SDL_DestroyMutex(port->mutex);
    }
    SDL_free(port->module_state);
    SDL_free(port);
}

static SERIAL_Port *SERIAL_StartPort(const char *key, const SDL_SerialModule *module, const SDL_SerialAutoRule *rule)
{
    SERIAL_Port *port = (SERIAL_Port *)SDL_calloc(1, sizeof(*port));
    SERIAL_Port **tail;
    char name[SDL_SERIAL_KEY_LENGTH + 16];

    if (!port) {
        return NULL;
    }
    SDL_strlcpy(port->key, key, sizeof(port->key));
    port->module = module;
    port->rule = rule;
    if (rule) {
        port->usb_vendor = rule->vendor_id;
        port->usb_product = rule->product_id;
        if (!port->usb_product) {
            uint16_t vendor, product;

            if (SDL_Serial_ParseUSBIds(key, &vendor, &product)) {
                port->usb_product = product;
            }
        }
    }
    port->handle = INVALID_HANDLE_VALUE;
    port->module_state = SDL_calloc(1, module->state_size);
    port->mutex = SDL_CreateMutex();
    port->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    port->read_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    port->write_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    port->output_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    port->rescan_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!port->module_state || !port->mutex || !port->stop_event || !port->read_event ||
        !port->write_event || !port->output_event || !port->rescan_event) {
        SERIAL_FreePort(port);
        return NULL;
    }
    SDL_Serial_ClearQueue(&port->queue);
    SERIAL_LoadConfigManager();

    SDL_LockMutex(serial_lock);
    for (tail = &serial_ports; *tail; tail = &(*tail)->next) {
    }
    *tail = port;
    SDL_UnlockMutex(serial_lock);

    (void)SDL_snprintf(name, sizeof(name), "SDLSerial %s", key);
    port->thread = SDL_CreateThread(SERIAL_PortThread, name, port);
    if (!port->thread) {
        SDL_LockMutex(serial_lock);
        for (tail = &serial_ports; *tail; tail = &(*tail)->next) {
            if (*tail == port) {
                *tail = port->next;
                break;
            }
        }
        SDL_UnlockMutex(serial_lock);
        SERIAL_FreePort(port);
        return NULL;
    }
    return port;
}

/* Removes the port's joysticks, stops its thread and frees it */
static void SERIAL_StopPort(SERIAL_Port *port, bool notify)
{
    SERIAL_Port **link;
    int sub;

    for (sub = 0; sub < SDL_SERIAL_MAX_SUBDEVICES; ++sub) {
        SERIAL_Sub *s = &port->subs[sub];

        if (s->registered) {
            SDL_Joystick *joystick = SDL_GetJoystickFromID(s->instance_id);

            if (joystick && joystick->hwdata) {
                joystick->hwdata->port = NULL;
            }
            if (notify) {
                SDL_PrivateJoystickRemoved(s->instance_id);
            }
            s->registered = 0;
        }
    }
    SetEvent(port->stop_event);
    SDL_WaitThread(port->thread, NULL);
    port->thread = NULL;

    SDL_LockMutex(serial_lock);
    for (link = &serial_ports; *link; link = &(*link)->next) {
        if (*link == port) {
            *link = port->next;
            break;
        }
    }
    SDL_UnlockMutex(serial_lock);
    SERIAL_FreePort(port);
}

static void SERIAL_LogHintEntry(void *userdata, const char *entry, size_t length, const char *reason)
{
    (void)userdata;
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "SDL_JOYSTICK_SERIAL entry \"%.*s\" skipped: %s", (int)length, entry, reason);
}

static bool SERIAL_IsACIO(const SDL_SerialModule *module)
{
    size_t i;

    for (i = 0; i < SDL_arraysize(serial_acio_modules); ++i) {
        if (serial_acio_modules[i] == module) {
            return true;
        }
    }
    return false;
}

/* The module a port runs for a hint entry or an auto rule. Token bio2 takes
 * its mode from SDL_HINT_JOYSTICK_KONAMI_BIO2_MODE, and no ACIO module runs
 * while SDL_HINT_JOYSTICK_KONAMI_ACIO is off. NULL leaves the port closed. A
 * module reads no configuration, so a mode is a module of its own, and a
 * mode that changes restarts the port. */
static const SDL_SerialModule *SERIAL_ResolveModule(const SDL_SerialModule *module)
{
    if (!module || (!serial_acio && SERIAL_IsACIO(module))) {
        return NULL;
    }
    if (module == &SDL_SerialBIO2Module && serial_bio2_module) {
        return serial_bio2_module;
    }
    return module;
}

/* Resolves the entries of a parsed hint and drops the ones that run nothing.
 * Returns the number kept. */
static int SERIAL_ResolveEntries(SDL_SerialPortEntry *entries, int count)
{
    int i, kept = 0;

    for (i = 0; i < count; ++i) {
        const SDL_SerialModule *module = SERIAL_ResolveModule(entries[i].module);

        if (!module) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "Serial joystick %s (%s): SDL_JOYSTICK_KONAMI_ACIO is off", entries[i].key, entries[i].module->token);
            continue;
        }
        if (kept != i) {
            entries[kept] = entries[i];
        }
        entries[kept++].module = module;
    }
    return kept;
}

/* Applies the latest SDL_HINT_JOYSTICK_SERIAL on the joystick thread */
static void SERIAL_ApplyHint(void)
{
    SDL_SerialPortEntry *entries = (SDL_SerialPortEntry *)SDL_calloc(2 * SDL_SERIAL_MAX_PORTS, sizeof(SDL_SerialPortEntry));
    SDL_SerialPortEntry *old_entries = entries ? entries + SDL_SERIAL_MAX_PORTS : NULL;
    SERIAL_Port *old_ports[SDL_SERIAL_MAX_PORTS];
    SDL_SerialPortChange old_changes[SDL_SERIAL_MAX_PORTS], new_changes[SDL_SERIAL_MAX_PORTS];
    SERIAL_Port *port;
    char *hint = NULL;
    int nold = 0, nnew, i, j;

    if (!entries) {
        return;
    }
    SDL_LockMutex(serial_lock);
    if (serial_hint) {
        hint = SDL_strdup(serial_hint);
    }
    SDL_UnlockMutex(serial_lock);
    nnew = SDL_Serial_ParseHint(hint, serial_modules, (int)SDL_arraysize(serial_modules), entries, SDL_SERIAL_MAX_PORTS, SERIAL_LogHintEntry, NULL);
    SDL_free(hint);
    nnew = SERIAL_ResolveEntries(entries, nnew);

    for (port = serial_ports; port && nold < SDL_SERIAL_MAX_PORTS; port = port->next) {
        if (!port->rule) {
            SDL_strlcpy(old_entries[nold].key, port->key, sizeof(old_entries[nold].key));
            old_entries[nold].module = port->module;
            old_ports[nold++] = port;
        }
    }
    SDL_Serial_DiffPorts(old_entries, nold, entries, nnew, old_changes, new_changes);
    for (i = 0; i < nold; ++i) {
        if (old_changes[i] != SDL_SERIAL_PORT_KEEP) {
            SERIAL_StopPort(old_ports[i], true);
        }
    }
    for (j = 0; j < nnew; ++j) {
        bool start = (new_changes[j] == SDL_SERIAL_PORT_START);

        for (i = 0; !start && i < nold; ++i) {
            if (old_changes[i] == SDL_SERIAL_PORT_RESTART && SDL_Serial_KeysEqual(old_entries[i].key, entries[j].key)) {
                start = true;
            }
        }
        if (start && !SERIAL_StartPort(entries[j].key, entries[j].module, NULL)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Serial joystick %s could not start", entries[j].key);
        }
    }
    SDL_free(entries);
    /* Ports the hint now claims leave the automatic set */
    SDL_SetAtomicInt(&serial_rescan, 1);
}

static int SERIAL_CountAutoRules(void)
{
    int n = 0;

    while (serial_auto_rules[n].prefix) {
        ++n;
    }
    return n;
}

static const SDL_SerialModule *SERIAL_FindModule(const char *token)
{
    size_t i;

    for (i = 0; i < SDL_arraysize(serial_modules); ++i) {
        if (SDL_strcmp(serial_modules[i]->token, token) == 0) {
            return serial_modules[i];
        }
    }
    return NULL;
}

/* Whether a hint entry names this device, by instance ID prefix or COM name */
static bool SERIAL_HintClaims(const char *instance_id)
{
    SERIAL_Port *port;
    unsigned int number = 0, hint_number = 0;
    bool have_number = false, checked_number = false;

    for (port = serial_ports; port; port = port->next) {
        if (port->rule) {
            continue;
        }
        if (SDL_Serial_IsComKey(port->key, &hint_number)) {
            if (!checked_number) {
                have_number = SERIAL_PortName(instance_id, &number);
                checked_number = true;
            }
            if (have_number && number == hint_number) {
                return true;
            }
        } else if (SDL_Serial_InstanceMatches(port->key, instance_id)) {
            return true;
        }
    }
    return false;
}

/* Starts a port for each present device the table names, and stops the
 * ones that left, or all of them when SDL_HINT_JOYSTICK_SERIAL_AUTO is off */
static void SERIAL_ScanAuto(void)
{
    const int nrules = SERIAL_CountAutoRules();
    SERIAL_Port *port, *next;
    WCHAR *list = NULL, *interface_path;
    bool any = false;

    for (port = serial_ports; port; port = port->next) {
        if (port->rule) {
            port->auto_seen = false;
            any = true;
        }
    }
    if (nrules == 0 && !any) {
        return;
    }
    if (serial_auto && nrules > 0) {
        SERIAL_LoadConfigManager();
        list = SERIAL_ListInterfaces();
    }
    for (interface_path = list; interface_path && *interface_path; interface_path += SDL_wcslen(interface_path) + 1) {
        char *instance_id = SERIAL_InterfaceInstanceId(interface_path);
        const SDL_SerialAutoRule *rule = instance_id ? SDL_Serial_MatchAuto(serial_auto_rules, nrules, instance_id) : NULL;
        const SDL_SerialModule *module = rule ? SERIAL_ResolveModule(SERIAL_FindModule(rule->token)) : NULL;

        if (module && !SERIAL_HintClaims(instance_id)) {
            for (port = serial_ports; port; port = port->next) {
                if (port->rule && SDL_Serial_KeysEqual(port->key, instance_id)) {
                    break;
                }
            }
            if (port && port->module != module) {
                /* A hint picked another module: the port starts over */
                SERIAL_StopPort(port, true);
                port = NULL;
            }
            if (!port) {
                port = SERIAL_StartPort(instance_id, module, rule);
            }
            if (port) {
                port->auto_seen = true;
            }
        }
        SDL_free(instance_id);
    }
    SDL_free(list);

    for (port = serial_ports; port; port = next) {
        next = port->next;
        if (port->rule && !port->auto_seen) {
            SERIAL_StopPort(port, true);
        }
    }
}

static void SERIAL_CheckPresence(SERIAL_Port *port)
{
    int sub;

    for (sub = 0; sub < SDL_SERIAL_MAX_SUBDEVICES; ++sub) {
        SERIAL_Sub *s = &port->subs[sub];
        SDL_SerialIdentity identity;
        uint32_t generation;

        SDL_LockMutex(port->mutex);
        generation = s->generation;
        identity = s->identity;
        SDL_UnlockMutex(port->mutex);

        if (s->registered && s->registered != generation) {
            SDL_Joystick *joystick = SDL_GetJoystickFromID(s->instance_id);

            if (joystick && joystick->hwdata) {
                joystick->hwdata->port = NULL;
            }
            SDL_PrivateJoystickRemoved(s->instance_id);
            s->registered = 0;
        }
        if (!s->registered && generation) {
            s->registered = generation;
            s->joystick_identity = identity;
            s->instance_id = SDL_GetNextObjectID();
            if (port->rule) {
                s->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_USB, port->usb_vendor, port->usb_product, 0, NULL, identity.name, 's', identity.type);
            } else {
                s->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_SERIAL, 0, 0, 0, NULL, identity.name, 's', identity.type);
            }
            SDL_PrivateJoystickAdded(s->instance_id);
        }
    }
}

static bool SERIAL_GetDevice(int device_index, SERIAL_Port **port_out, int *sub_out)
{
    SERIAL_Port *port;
    int sub;

    if (device_index < 0) {
        return false;
    }
    for (port = serial_ports; port; port = port->next) {
        for (sub = 0; sub < SDL_SERIAL_MAX_SUBDEVICES; ++sub) {
            if (port->subs[sub].registered && device_index-- == 0) {
                *port_out = port;
                *sub_out = sub;
                return true;
            }
        }
    }
    return false;
}

static void SDLCALL SERIAL_HintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    (void)userdata;
    (void)name;
    (void)oldValue;
    SDL_LockMutex(serial_lock);
    SDL_free(serial_hint);
    serial_hint = hint ? SDL_strdup(hint) : NULL;
    SDL_UnlockMutex(serial_lock);
    SDL_SetAtomicInt(&serial_hint_changed, 1);
}

static void SDLCALL SERIAL_AutoHintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    (void)userdata;
    (void)name;
    (void)oldValue;
    serial_auto = SDL_GetStringBoolean(hint, true);
    SDL_SetAtomicInt(&serial_rescan, 1);
}

/* The ACIO hints change what hint entries and auto rules run, so both are
   applied again */
static void SDLCALL SERIAL_ACIOHintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    (void)userdata;
    (void)name;
    (void)oldValue;
    serial_acio = SDL_GetStringBoolean(hint, true);
    SDL_SetAtomicInt(&serial_hint_changed, 1);
    SDL_SetAtomicInt(&serial_rescan, 1);
}

static void SDLCALL SERIAL_BIO2HintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    (void)userdata;
    (void)name;
    (void)oldValue;
    if (hint && SDL_strcasecmp(hint, "iidx") == 0) {
        serial_bio2_module = &SDL_SerialBIO2IIDXModule;
    } else if (hint && SDL_strcasecmp(hint, "sdvx") == 0) {
        serial_bio2_module = &SDL_SerialBIO2SDVXModule;
    } else {
        serial_bio2_module = &SDL_SerialBIO2Module;
    }
    SDL_SetAtomicInt(&serial_hint_changed, 1);
    SDL_SetAtomicInt(&serial_rescan, 1);
}

static bool SERIAL_JoystickInit(void)
{
    serial_lock = SDL_CreateMutex();
    if (!serial_lock) {
        return false;
    }
    serial_cancel_io_ex = (SERIAL_CancelIoEx)GetProcAddress(GetModuleHandle(TEXT("kernel32.dll")), "CancelIoEx");
    SDL_SetAtomicInt(&serial_rescan, 1);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_SERIAL_AUTO, SERIAL_AutoHintChanged, NULL);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_KONAMI_ACIO, SERIAL_ACIOHintChanged, NULL);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_KONAMI_BIO2_MODE, SERIAL_BIO2HintChanged, NULL);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_SERIAL, SERIAL_HintChanged, NULL);
    return true;
}

static int SERIAL_JoystickGetCount(void)
{
    SERIAL_Port *port;
    int count = 0, sub;

    for (port = serial_ports; port; port = port->next) {
        for (sub = 0; sub < SDL_SERIAL_MAX_SUBDEVICES; ++sub) {
            if (port->subs[sub].registered) {
                ++count;
            }
        }
    }
    return count;
}

static void SERIAL_JoystickDetect(void)
{
    SERIAL_Port *port;

    if (SDL_GetAtomicInt(&serial_hint_changed)) {
        SDL_SetAtomicInt(&serial_hint_changed, 0);
        SERIAL_ApplyHint();
    }
    if (SDL_GetAtomicInt(&serial_rescan)) {
        SDL_SetAtomicInt(&serial_rescan, 0);
        SERIAL_ScanAuto();
    }
    for (port = serial_ports; port; port = port->next) {
        if (SDL_GetAtomicInt(&port->presence_changed)) {
            SDL_SetAtomicInt(&port->presence_changed, 0);
            SERIAL_CheckPresence(port);
        }
    }
}

/* Connection state is wired and the port holds the device, so no other
 * driver's device is this one */
static bool SERIAL_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    (void)vendor_id;
    (void)product_id;
    (void)version;
    (void)name;
    return false;
}

static const char *SERIAL_JoystickGetDeviceName(int device_index)
{
    SERIAL_Port *port;
    int sub;

    if (!SERIAL_GetDevice(device_index, &port, &sub)) {
        return NULL;
    }
    return port->subs[sub].joystick_identity.name;
}

static const char *SERIAL_JoystickGetDevicePath(int device_index)
{
    SERIAL_Port *port;
    int sub;

    if (!SERIAL_GetDevice(device_index, &port, &sub)) {
        return NULL;
    }
    return port->key;
}

static int SERIAL_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    (void)device_index;
    return -1;
}

static int SERIAL_JoystickGetDevicePlayerIndex(int device_index)
{
    (void)device_index;
    return -1;
}

static void SERIAL_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
    (void)device_index;
    (void)player_index;
}

static SDL_GUID SERIAL_JoystickGetDeviceGUID(int device_index)
{
    SERIAL_Port *port;
    int sub;
    SDL_GUID guid;

    if (!SERIAL_GetDevice(device_index, &port, &sub)) {
        SDL_zero(guid);
        return guid;
    }
    return port->subs[sub].guid;
}

static SDL_JoystickID SERIAL_JoystickGetDeviceInstanceID(int device_index)
{
    SERIAL_Port *port;
    int sub;

    if (!SERIAL_GetDevice(device_index, &port, &sub)) {
        return 0;
    }
    return port->subs[sub].instance_id;
}

static void SERIAL_SendControls(SDL_Joystick *joystick, const SDL_SerialControls *controls, Uint64 timestamp)
{
    int i;

    for (i = 0; i < joystick->naxes; ++i) {
        SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, controls->axes[i]);
    }
    for (i = 0; i < joystick->nbuttons; ++i) {
        SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, SDL_Serial_GetButton(controls, i));
    }
    for (i = 0; i < joystick->nhats; ++i) {
        SDL_SendJoystickHat(timestamp, joystick, (Uint8)i, controls->hats[i]);
    }
    if (joystick->nballs > 0 && (controls->ball[0] || controls->ball[1])) {
        SDL_SendJoystickBall(timestamp, joystick, 0, controls->ball[0], controls->ball[1]);
    }
}

/* The state Open took goes out before anything newer. SDL allocates a
 * joystick's axes and buttons only after Open returns, so Open sends
 * nothing itself. */
static void SERIAL_SendInitial(SDL_Joystick *joystick)
{
    struct joystick_hwdata *hwdata = joystick->hwdata;

    if (hwdata->send_initial) {
        hwdata->send_initial = false;
        SERIAL_SendControls(joystick, &hwdata->initial, hwdata->initial_stamp);
    }
}

static bool SERIAL_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    SERIAL_Port *port;
    SERIAL_Sub *s;
    struct joystick_hwdata *hwdata;
    int sub;

    if (!SERIAL_GetDevice(device_index, &port, &sub)) {
        return SDL_SetError("Serial joystick index out of range");
    }
    s = &port->subs[sub];
    hwdata = (struct joystick_hwdata *)SDL_calloc(1, sizeof(*hwdata));
    if (!hwdata) {
        return false;
    }
    hwdata->port = port;
    hwdata->sub = sub;
    hwdata->generation = s->registered;
    joystick->hwdata = hwdata;
    joystick->naxes = s->joystick_identity.naxes;
    joystick->nbuttons = s->joystick_identity.nbuttons;
    joystick->nhats = s->joystick_identity.nhats;
    joystick->nballs = s->joystick_identity.nballs;
    joystick->connection_state = SDL_JOYSTICK_CONNECTION_WIRED;
    if (port->module->rumble) {
        SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, true);
    }

    /* The state so far, sent by the first update. Queued snapshots up to it
       are skipped. */
    SDL_LockMutex(port->mutex);
    hwdata->initial = s->latest;
    hwdata->sequence = s->latest_sequence;
    SDL_UnlockMutex(port->mutex);
    /* A ball reports motion, and motion from before the open is not state */
    hwdata->initial.ball[0] = 0;
    hwdata->initial.ball[1] = 0;
    hwdata->initial_stamp = SDL_GetTicksNS();
    hwdata->send_initial = true;
    return true;
}

static void SERIAL_PostOutput(SERIAL_Port *port, const SDL_SerialOutput *request)
{
    SDL_LockMutex(port->mutex);
    port->subs[request->sub].output = *request;
    port->subs[request->sub].output_set = true;
    SDL_UnlockMutex(port->mutex);
    SetEvent(port->output_event);
}

static bool SERIAL_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    struct joystick_hwdata *hwdata = joystick->hwdata;
    SDL_SerialOutput request;

    if (!hwdata || !hwdata->port) {
        return SDL_SetError("Serial joystick is no longer connected");
    }
    if (!hwdata->port->module->rumble) {
        return SDL_Unsupported();
    }
    SDL_zero(request);
    request.kind = SDL_SERIAL_OUTPUT_RUMBLE;
    request.sub = hwdata->sub;
    request.low_frequency_rumble = low_frequency_rumble;
    request.high_frequency_rumble = high_frequency_rumble;
    SERIAL_PostOutput(hwdata->port, &request);
    return true;
}

static bool SERIAL_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    (void)joystick;
    (void)left_rumble;
    (void)right_rumble;
    return SDL_Unsupported();
}

static bool SERIAL_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)joystick;
    (void)red;
    (void)green;
    (void)blue;
    return SDL_Unsupported();
}

static bool SERIAL_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    struct joystick_hwdata *hwdata = joystick->hwdata;
    const SDL_SerialModule *module;
    SDL_SerialOutput request;

    if (!hwdata || !hwdata->port) {
        return SDL_SetError("Serial joystick is no longer connected");
    }
    module = hwdata->port->module;
    if (module->effect_max == 0) {
        return SDL_Unsupported();
    }
    if (!data || size < (int)module->effect_min || size > (int)module->effect_max) {
        return SDL_SetError("Serial %s effects take %d to %d bytes", module->token, (int)module->effect_min, (int)module->effect_max);
    }
    SDL_zero(request);
    request.kind = SDL_SERIAL_OUTPUT_EFFECT;
    request.sub = hwdata->sub;
    request.length = (size_t)size;
    SDL_memcpy(request.data, data, (size_t)size);
    SERIAL_PostOutput(hwdata->port, &request);
    return true;
}

static bool SERIAL_JoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    (void)joystick;
    (void)enabled;
    return SDL_Unsupported();
}

/* Drains the port's snapshot queue in order to every open joystick of the port */
static void SERIAL_JoystickUpdate(SDL_Joystick *joystick)
{
    struct joystick_hwdata *hwdata = joystick->hwdata;
    SDL_SerialQueueEntry entries[SDL_SERIAL_QUEUE_ENTRIES];
    SERIAL_Port *port;
    int count = 0, i;

    if (!hwdata || !hwdata->port) {
        return;
    }
    SERIAL_SendInitial(joystick);
    port = hwdata->port;
    SDL_LockMutex(port->mutex);
    while (count < SDL_SERIAL_QUEUE_ENTRIES && SDL_Serial_PopQueue(&port->queue, &entries[count])) {
        ++count;
    }
    SDL_UnlockMutex(port->mutex);
    for (i = 0; i < count; ++i) {
        const SDL_SerialQueueEntry *entry = &entries[i];
        SERIAL_Sub *s = &port->subs[entry->sub];
        SDL_Joystick *target;

        if (!s->registered || s->registered != entry->generation) {
            continue;
        }
        target = SDL_GetJoystickFromID(s->instance_id);
        if (!target || !target->hwdata || target->hwdata->port != port || target->hwdata->generation != entry->generation ||
            entry->sequence <= target->hwdata->sequence) {
            continue;
        }
        SERIAL_SendInitial(target);
        target->hwdata->sequence = entry->sequence;
        SERIAL_SendControls(target, &entry->controls, entry->stamp_ns);
    }
}

static void SERIAL_JoystickClose(SDL_Joystick *joystick)
{
    SDL_free(joystick->hwdata);
    joystick->hwdata = NULL;
}

static void SERIAL_JoystickQuit(void)
{
    SERIAL_Port *port;

    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_SERIAL, SERIAL_HintChanged, NULL);
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_KONAMI_BIO2_MODE, SERIAL_BIO2HintChanged, NULL);
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_KONAMI_ACIO, SERIAL_ACIOHintChanged, NULL);
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_SERIAL_AUTO, SERIAL_AutoHintChanged, NULL);
    /* Unregistering waits for a callback in progress, which takes serial_lock */
    if (serial_notification && serial_unregister_notification) {
        serial_unregister_notification(serial_notification);
        serial_notification = NULL;
    }
    /* Every port starts its close at once, so the close sequences of
       several ports run side by side and SDL_Quit waits for the longest
       one, not their sum. The stop event is manual reset, so
       SERIAL_StopPort setting it again changes nothing. */
    SDL_LockMutex(serial_lock);
    for (port = serial_ports; port; port = port->next) {
        SetEvent(port->stop_event);
    }
    SDL_UnlockMutex(serial_lock);
    while (serial_ports) {
        SERIAL_StopPort(serial_ports, false);
    }
    SERIAL_UnloadConfigManager();
    SDL_free(serial_hint);
    serial_hint = NULL;
    SDL_SetAtomicInt(&serial_hint_changed, 0);
    if (serial_lock) {
        SDL_DestroyMutex(serial_lock);
        serial_lock = NULL;
    }
}

static void SERIAL_MapInput(SDL_InputMapping *out, const SDL_SerialMapInput *in)
{
    SDL_zerop(out);
    switch (in->kind) {
    case SDL_SERIAL_MAP_BUTTON:
        out->kind = EMappingKind_Button;
        break;
    case SDL_SERIAL_MAP_AXIS:
        out->kind = EMappingKind_Axis;
        break;
    case SDL_SERIAL_MAP_HAT:
        out->kind = EMappingKind_Hat;
        break;
    case SDL_SERIAL_MAP_AXIS_POSITIVE:
        out->kind = EMappingKind_Axis;
        out->half_axis_positive = true;
        break;
    case SDL_SERIAL_MAP_AXIS_NEGATIVE:
        out->kind = EMappingKind_Axis;
        out->half_axis_negative = true;
        break;
    default:
        out->kind = EMappingKind_None;
        return;
    }
    out->target = in->target;
}

static bool SERIAL_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    SERIAL_Port *port;
    const SDL_SerialGamepadMap *map;
    int sub;

    if (!SERIAL_GetDevice(device_index, &port, &sub) || !port->subs[sub].joystick_identity.has_mapping) {
        return false;
    }
    map = &port->subs[sub].joystick_identity.mapping;
    SDL_zerop(out);
    SERIAL_MapInput(&out->a, &map->a);
    SERIAL_MapInput(&out->b, &map->b);
    SERIAL_MapInput(&out->x, &map->x);
    SERIAL_MapInput(&out->y, &map->y);
    SERIAL_MapInput(&out->back, &map->back);
    SERIAL_MapInput(&out->guide, &map->guide);
    SERIAL_MapInput(&out->start, &map->start);
    SERIAL_MapInput(&out->leftstick, &map->leftstick);
    SERIAL_MapInput(&out->rightstick, &map->rightstick);
    SERIAL_MapInput(&out->leftshoulder, &map->leftshoulder);
    SERIAL_MapInput(&out->rightshoulder, &map->rightshoulder);
    SERIAL_MapInput(&out->dpup, &map->dpup);
    SERIAL_MapInput(&out->dpdown, &map->dpdown);
    SERIAL_MapInput(&out->dpleft, &map->dpleft);
    SERIAL_MapInput(&out->dpright, &map->dpright);
    SERIAL_MapInput(&out->misc1, &map->misc1);
    SERIAL_MapInput(&out->leftx, &map->leftx);
    SERIAL_MapInput(&out->lefty, &map->lefty);
    SERIAL_MapInput(&out->rightx, &map->rightx);
    SERIAL_MapInput(&out->righty, &map->righty);
    SERIAL_MapInput(&out->lefttrigger, &map->lefttrigger);
    SERIAL_MapInput(&out->righttrigger, &map->righttrigger);
    return true;
}

SDL_JoystickDriver SDL_SERIAL_JoystickDriver = {
    SERIAL_JoystickInit,
    SERIAL_JoystickGetCount,
    SERIAL_JoystickDetect,
    SERIAL_JoystickIsDevicePresent,
    SERIAL_JoystickGetDeviceName,
    SERIAL_JoystickGetDevicePath,
    SERIAL_JoystickGetDeviceSteamVirtualGamepadSlot,
    SERIAL_JoystickGetDevicePlayerIndex,
    SERIAL_JoystickSetDevicePlayerIndex,
    SERIAL_JoystickGetDeviceGUID,
    SERIAL_JoystickGetDeviceInstanceID,
    SERIAL_JoystickOpen,
    SERIAL_JoystickRumble,
    SERIAL_JoystickRumbleTriggers,
    SERIAL_JoystickSetLED,
    SERIAL_JoystickSendEffect,
    SERIAL_JoystickSetSensorsEnabled,
    SERIAL_JoystickUpdate,
    SERIAL_JoystickClose,
    SERIAL_JoystickQuit,
    SERIAL_JoystickGetGamepadMapping
};

#endif // SDL_JOYSTICK_SERIAL
