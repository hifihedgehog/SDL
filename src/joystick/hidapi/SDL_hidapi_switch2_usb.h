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

/* Platform abstraction for bulk USB transfers needed by the Switch 2 driver.
   On Windows, the Switch 2 is a composite USB device: Interface 0 is HID
   (owned by the Windows HID driver), Interface 1 is vendor-specific bulk
   (bound to WinUSB via MS OS 2.0 descriptors).  Since libusb cannot claim
   Interface 1 on a Windows composite device when another driver owns
   Interface 0, we use WinUSB directly on Windows.  On other platforms,
   libusb works normally via kernel driver detach. */

#ifndef SDL_HIDAPI_SWITCH2_USB_H
#define SDL_HIDAPI_SWITCH2_USB_H

#include "../../misc/SDL_libusb.h"

typedef struct Switch2_BulkUSB Switch2_BulkUSB;

/* Opaque context for bulk USB transfers to the Switch 2's Interface 1. */
struct Switch2_BulkUSB {
    SDL_LibUSBContext *libusb;
    libusb_device_handle *device_handle;
    bool owns_device_handle;
    libusb_context *libusb_ctx;
    bool interface_claimed;
    Uint8 interface_number;
    Uint8 out_endpoint;
    Uint8 in_endpoint;

#ifdef SDL_PLATFORM_WIN32
    void *winusb_file_handle;       /* HANDLE */
    void *winusb_handle;            /* WINUSB_INTERFACE_HANDLE */
    unsigned char winusb_out_pipe;
    unsigned char winusb_in_pipe;
    bool use_winusb;
#endif
};

static bool Switch2_BulkUSB_FindEndpoints(SDL_LibUSBContext *libusb, libusb_device_handle *handle,
                                           Uint8 *iface_num, Uint8 *out_ep, Uint8 *in_ep)
{
    struct libusb_config_descriptor *config;
    int found = 0;

    if (libusb->get_config_descriptor(libusb->get_device(handle), 0, &config) != 0) {
        return false;
    }

    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &config->interface[i];
        for (int j = 0; j < iface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[j];
            if (alt->bInterfaceNumber == 1) {
                for (int k = 0; k < alt->bNumEndpoints; k++) {
                    const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];
                    if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                        *iface_num = alt->bInterfaceNumber;
                        if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) {
                            *out_ep = ep->bEndpointAddress;
                            found |= 1;
                        }
                        if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
                            *in_ep = ep->bEndpointAddress;
                            found |= 2;
                        }
                        if (found == 3) {
                            libusb->free_config_descriptor(config);
                            return true;
                        }
                    }
                }
            }
        }
    }
    libusb->free_config_descriptor(config);
    return false;
}

#ifdef SDL_PLATFORM_WIN32

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <initguid.h>

/* DeviceInterfaceGUID from the Switch 2's MS OS 2.0 descriptors (Interface 1) */
DEFINE_GUID(GUID_DEVINTERFACE_SWITCH2_BULK,
    0x6F13725E, 0xEF0E, 0x4FD3, 0xAE, 0x5F, 0xB2, 0xDE, 0x98, 0x9E, 0xC8, 0x25);

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")

static bool Switch2_BulkUSB_OpenWinUSB(Switch2_BulkUSB *bulk)
{
    HDEVINFO devInfo;
    SP_DEVICE_INTERFACE_DATA ifData;
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = NULL;
    DWORD needed;

    devInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_SWITCH2_BULK, NULL, NULL,
                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return false;
    }

    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &GUID_DEVINTERFACE_SWITCH2_BULK, i, &ifData); i++) {
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &needed, NULL);
        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)SDL_malloc(needed);
        if (!detail) {
            continue;
        }
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, NULL, NULL)) {
            SDL_free(detail);
            continue;
        }

        /* Nintendo VID 057E — the GUID already scopes to Switch 2 devices */
        if (!wcsstr(detail->DevicePath, L"vid_057e") && !wcsstr(detail->DevicePath, L"VID_057E")) {
            SDL_free(detail);
            continue;
        }

        HANDLE fh = CreateFileW(detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            NULL);
        SDL_free(detail);

        if (fh == INVALID_HANDLE_VALUE) {
            continue;
        }

        WINUSB_INTERFACE_HANDLE wh;
        if (!WinUsb_Initialize(fh, &wh)) {
            CloseHandle(fh);
            continue;
        }

        USB_INTERFACE_DESCRIPTOR ifDesc;
        if (!WinUsb_QueryInterfaceSettings(wh, 0, &ifDesc)) {
            WinUsb_Free(wh);
            CloseHandle(fh);
            continue;
        }

        UCHAR out_pipe = 0, in_pipe = 0;
        for (UCHAR ep = 0; ep < ifDesc.bNumEndpoints; ep++) {
            WINUSB_PIPE_INFORMATION pipeInfo;
            if (WinUsb_QueryPipe(wh, 0, ep, &pipeInfo) && pipeInfo.PipeType == UsbdPipeTypeBulk) {
                if (pipeInfo.PipeId & 0x80) {
                    in_pipe = pipeInfo.PipeId;
                } else {
                    out_pipe = pipeInfo.PipeId;
                }
            }
        }

        if (!out_pipe || !in_pipe) {
            WinUsb_Free(wh);
            CloseHandle(fh);
            continue;
        }

        ULONG timeout = 1000;
        WinUsb_SetPipePolicy(wh, out_pipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
        WinUsb_SetPipePolicy(wh, in_pipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);

        bulk->winusb_file_handle = fh;
        bulk->winusb_handle = wh;
        bulk->winusb_out_pipe = out_pipe;
        bulk->winusb_in_pipe = in_pipe;
        bulk->use_winusb = true;

        SetupDiDestroyDeviceInfoList(devInfo);
        return true;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return false;
}

static int Switch2_BulkUSB_WinUSB_Write(Switch2_BulkUSB *bulk, const Uint8 *data, unsigned size, unsigned timeout_ms)
{
    WINUSB_INTERFACE_HANDLE wh = (WINUSB_INTERFACE_HANDLE)bulk->winusb_handle;
    OVERLAPPED ov;
    ULONG transferred = 0;

    SDL_memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) {
        return -1;
    }

    if (!WinUsb_WritePipe(wh, bulk->winusb_out_pipe, (PUCHAR)data, size, &transferred, &ov)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, timeout_ms) == WAIT_OBJECT_0) {
                WinUsb_GetOverlappedResult(wh, &ov, &transferred, FALSE);
            } else {
                WinUsb_AbortPipe(wh, bulk->winusb_out_pipe);
                WaitForSingleObject(ov.hEvent, 100);
                CloseHandle(ov.hEvent);
                return -7;
            }
        } else {
            CloseHandle(ov.hEvent);
            return -1;
        }
    }
    CloseHandle(ov.hEvent);
    return (int)transferred;
}

static int Switch2_BulkUSB_WinUSB_Read(Switch2_BulkUSB *bulk, Uint8 *data, unsigned size)
{
    WINUSB_INTERFACE_HANDLE wh = (WINUSB_INTERFACE_HANDLE)bulk->winusb_handle;
    ULONG total = 0;

    while (size > 0) {
        unsigned chunk = (size > 64) ? 64 : size;
        OVERLAPPED ov;
        ULONG transferred = 0;

        SDL_memset(&ov, 0, sizeof(ov));
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!ov.hEvent) {
            return (total > 0) ? (int)total : -1;
        }

        if (!WinUsb_ReadPipe(wh, bulk->winusb_in_pipe, data, chunk, &transferred, &ov)) {
            if (GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0) {
                    WinUsb_GetOverlappedResult(wh, &ov, &transferred, FALSE);
                } else {
                    WinUsb_AbortPipe(wh, bulk->winusb_in_pipe);
                    WaitForSingleObject(ov.hEvent, 100);
                    CloseHandle(ov.hEvent);
                    return (total > 0) ? (int)total : -7;
                }
            } else {
                CloseHandle(ov.hEvent);
                return (total > 0) ? (int)total : -1;
            }
        }
        CloseHandle(ov.hEvent);
        total += transferred;
        size -= transferred;
        data += chunk;
        if (transferred < chunk) {
            break;
        }
    }
    return (int)total;
}

static void Switch2_BulkUSB_FlushWinUSB(Switch2_BulkUSB *bulk)
{
    WINUSB_INTERFACE_HANDLE wh = (WINUSB_INTERFACE_HANDLE)bulk->winusb_handle;
    Uint8 buf[64];
    ULONG read;
    ULONG short_timeout = 50;

    WinUsb_ResetPipe(wh, bulk->winusb_out_pipe);
    WinUsb_ResetPipe(wh, bulk->winusb_in_pipe);

    WinUsb_SetPipePolicy(wh, bulk->winusb_in_pipe, PIPE_TRANSFER_TIMEOUT, sizeof(short_timeout), &short_timeout);
    while (WinUsb_ReadPipe(wh, bulk->winusb_in_pipe, buf, sizeof(buf), &read, NULL)) {
        /* drain stale data */
    }
    ULONG normal_timeout = 1000;
    WinUsb_SetPipePolicy(wh, bulk->winusb_in_pipe, PIPE_TRANSFER_TIMEOUT, sizeof(normal_timeout), &normal_timeout);
}

static void Switch2_BulkUSB_CloseWinUSB(Switch2_BulkUSB *bulk)
{
    if (bulk->winusb_handle) {
        WinUsb_Free((WINUSB_INTERFACE_HANDLE)bulk->winusb_handle);
        bulk->winusb_handle = NULL;
    }
    if (bulk->winusb_file_handle && bulk->winusb_file_handle != INVALID_HANDLE_VALUE) {
        CloseHandle((HANDLE)bulk->winusb_file_handle);
        bulk->winusb_file_handle = NULL;
    }
    bulk->use_winusb = false;
}

#endif /* SDL_PLATFORM_WIN32 */

/* --- Public API used by SDL_hidapi_switch2.c --- */

static bool Switch2_BulkUSB_Open(Switch2_BulkUSB *bulk, SDL_HIDAPI_Device *device)
{
    int res;

#ifdef SDL_PLATFORM_WIN32
    if (Switch2_BulkUSB_OpenWinUSB(bulk)) {
        Switch2_BulkUSB_FlushWinUSB(bulk);
        return true;
    }
    /* WinUSB failed (e.g. Steam has the interface locked), try libusb */
#endif

    if (!SDL_InitLibUSB(&bulk->libusb)) {
        return SDL_SetError("Couldn't initialize libusb");
    }

    bulk->device_handle = (libusb_device_handle *)SDL_GetPointerProperty(
        SDL_hid_get_properties(device->dev), SDL_PROP_HIDAPI_LIBUSB_DEVICE_HANDLE_POINTER, NULL);

    if (!bulk->device_handle) {
        /* Platform HID backend — no shared libusb handle available.
           Open our own libusb connection to claim Interface 1. */
        libusb_context *usb_ctx = NULL;
        if (bulk->libusb->init(&usb_ctx) == 0) {
            libusb_device **devs = NULL;
            ssize_t n = bulk->libusb->get_device_list(usb_ctx, &devs);
            for (ssize_t i = 0; i < n; i++) {
                struct libusb_device_descriptor desc;
                if (bulk->libusb->get_device_descriptor(devs[i], &desc) != 0) {
                    continue;
                }
                if (desc.idVendor == device->vendor_id && desc.idProduct == device->product_id) {
                    if (bulk->libusb->open(devs[i], &bulk->device_handle) == 0) {
                        bulk->owns_device_handle = true;
                        bulk->libusb_ctx = usb_ctx;
                        break;
                    }
                }
            }
            if (devs) {
                bulk->libusb->free_device_list(devs, 1);
            }
            if (!bulk->device_handle) {
                bulk->libusb->exit(usb_ctx);
            }
        }
        if (!bulk->device_handle) {
            SDL_QuitLibUSB();
            bulk->libusb = NULL;
            return SDL_SetError("Couldn't get libusb device handle");
        }
    }

    if (!Switch2_BulkUSB_FindEndpoints(bulk->libusb, bulk->device_handle,
                                        &bulk->interface_number, &bulk->out_endpoint, &bulk->in_endpoint)) {
        return SDL_SetError("Couldn't find bulk endpoints");
    }

    bulk->libusb->set_auto_detach_kernel_driver(bulk->device_handle, true);
    res = bulk->libusb->claim_interface(bulk->device_handle, bulk->interface_number);
    if (res < 0) {
        return SDL_SetError("Couldn't claim interface %d: %d\n", bulk->interface_number, res);
    }
    bulk->interface_claimed = true;
    return true;
}

static int Switch2_BulkUSB_Write(Switch2_BulkUSB *bulk, const Uint8 *data, unsigned size, unsigned timeout_ms)
{
#ifdef SDL_PLATFORM_WIN32
    if (bulk->use_winusb) {
        return Switch2_BulkUSB_WinUSB_Write(bulk, data, size, timeout_ms);
    }
#endif
    int transferred;
    int res = bulk->libusb->bulk_transfer(bulk->device_handle,
                bulk->out_endpoint, (Uint8 *)data, size, &transferred, timeout_ms);
    return (res < 0) ? res : transferred;
}

static int Switch2_BulkUSB_Read(Switch2_BulkUSB *bulk, Uint8 *data, unsigned size)
{
#ifdef SDL_PLATFORM_WIN32
    if (bulk->use_winusb) {
        return Switch2_BulkUSB_WinUSB_Read(bulk, data, size);
    }
#endif
    int transferred, total = 0, res;

    while (size > 0) {
        unsigned chunk = (size > 64) ? 64 : size;
        res = bulk->libusb->bulk_transfer(bulk->device_handle,
                    bulk->in_endpoint, data, chunk, &transferred, 500);
        if (res < 0) {
            return res;
        }
        total += transferred;
        size -= transferred;
        data += chunk;
        if ((unsigned)transferred < chunk) {
            break;
        }
    }
    return total;
}

static void Switch2_BulkUSB_Close(Switch2_BulkUSB *bulk)
{
#ifdef SDL_PLATFORM_WIN32
    if (bulk->use_winusb) {
        Switch2_BulkUSB_CloseWinUSB(bulk);
    }
#endif
    if (bulk->interface_claimed && bulk->libusb) {
        bulk->libusb->release_interface(bulk->device_handle, bulk->interface_number);
        bulk->interface_claimed = false;
    }
    if (bulk->owns_device_handle && bulk->device_handle && bulk->libusb) {
        bulk->libusb->close(bulk->device_handle);
        bulk->device_handle = NULL;
    }
    if (bulk->libusb_ctx && bulk->libusb) {
        bulk->libusb->exit(bulk->libusb_ctx);
        bulk->libusb_ctx = NULL;
    }
    if (bulk->libusb) {
        SDL_QuitLibUSB();
        bulk->libusb = NULL;
    }
}

#endif /* SDL_HIDAPI_SWITCH2_USB_H */
