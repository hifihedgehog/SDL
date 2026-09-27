/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs SDL_SERIAL_JoystickDriver, the serial joystick driver of
   hifihedgehog/SDL#33 Part 5, inside a static SDL against scripted COM
   ports, with the I-Force haptic driver, for hifihedgehog/SDL#35. No COM
   port opens, no library loads, and the registry is not read. The fake
   answers every call through which the driver opens, configures, reads,
   writes and closes a port, and three scripted devices sit behind it:

   COM250  an I-Force wheel that answers every query as the Boeder wheel
   COM251  the same wheel, except that N goes unanswered
   COM252  a Gravis Stinger
   COM253  an I-Force device whose IDs, 1234:5678, no table lists

   Each device answers only at its line settings, 38400 baud 8N1 for the
   I-Force and 1200 baud 8N1 for the Stinger. A read that waits completes
   with the first byte that arrives, as a COM port read does under the
   engine's timeouts. Cases 1 to 5 are the issue's:

   1. The Boeder wheel's joystick carries the answers to N, B, M and P as the
      four I-Force properties, and its GUID's vendor stays 0.
   2. SDL_IsJoystickHaptic is true for it, and for the device no table lists,
      since Linux gives any I-Force device force feedback once N reports
      effects. It is false for the wheel whose N went unanswered.
   3. Opening the haptic device writes the frames for 40 03 00, 40 04 01 and
      42 04 first, and closing it writes the frame for 42 01 last. A second
      open of the same joystick's haptic device returns the first.
   4. Uploading and running a constant effect writes the commands the same
      haptic driver sends a USB I-Force device, each in a serial frame, in
      order. A joystick record whose effects this test records stands in for
      the USB device, as in test/testiforcedriver.c. The commands are also
      the byte forms test/testiforce.c takes from Linux's iforce-ff.c.
   5. The Stinger's joystick has no haptic device, at SDL's haptic API and in
      the HIDAPI haptic layer, which SDL's gamepad check keeps a Stinger from
      reaching.
   6. The HIDAPI haptic layer serves a serial joystick only through a driver
      that declares it, and no joystick of another driver. Joystick records
      check it: a Guillemot wheel with I-Force properties on another driver,
      and a Logitech G29 on the serial driver, which the Logitech driver
      would take, are refused, and each is served on the HIDAPI driver.

   The driver in this file calls the fake, not Windows. Each name through
   which the driver reaches a COM port, the configuration manager, the
   registry or a library is defined below before the driver source is
   included. This object defines SDL_SERIAL_JoystickDriver, so SDL's driver
   table and the haptic layer bind to this copy and SDL's own copy of the
   driver is never linked, as in the iCade driver's test. Before the link,
   test/serial-driver/CheckSystem.cmake reads this object's symbols and fails
   the build when it still imports a Win32 function that reaches a device,
   the registry or a library. */

#define SDL_MAIN_HANDLED
#include "SDL_internal.h"
#include "core/windows/SDL_windows.h"

/* The fake port, defined below */
static BOOL WINAPI Fake_CancelIo(HANDLE file);
static BOOL WINAPI Fake_CloseHandle(HANDLE handle);
static HANDLE WINAPI Fake_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                      DWORD disposition, DWORD flags, HANDLE template_file);
static BOOL WINAPI Fake_EscapeCommFunction(HANDLE file, DWORD function);
static BOOL WINAPI Fake_FlushFileBuffers(HANDLE file);
static BOOL WINAPI Fake_FreeLibrary(HMODULE module);
static BOOL WINAPI Fake_GetCommState(HANDLE file, LPDCB dcb);
static BOOL WINAPI Fake_GetOverlappedResult(HANDLE file, LPOVERLAPPED overlapped, LPDWORD transferred, BOOL wait);
static FARPROC WINAPI Fake_GetProcAddress(HMODULE module, LPCSTR name);
static HMODULE WINAPI Fake_LoadLibraryW(LPCWSTR name);
static BOOL WINAPI Fake_PurgeComm(HANDLE file, DWORD flags);
static BOOL WINAPI Fake_ReadFile(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped);
static LSTATUS WINAPI Fake_RegCloseKey(HKEY key);
static LSTATUS WINAPI Fake_RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size);
static BOOL WINAPI Fake_SetCommState(HANDLE file, LPDCB dcb);
static BOOL WINAPI Fake_SetCommTimeouts(HANDLE file, LPCOMMTIMEOUTS timeouts);
static BOOL WINAPI Fake_WriteFile(HANDLE file, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED overlapped);

#define SERIAL_CancelIo            Fake_CancelIo
#define SERIAL_CloseHandle         Fake_CloseHandle
#define SERIAL_CreateFileW         Fake_CreateFileW
#define SERIAL_EscapeCommFunction  Fake_EscapeCommFunction
#define SERIAL_FlushFileBuffers    Fake_FlushFileBuffers
#define SERIAL_FreeLibrary         Fake_FreeLibrary
#define SERIAL_GetCommState        Fake_GetCommState
#define SERIAL_GetOverlappedResult Fake_GetOverlappedResult
#define SERIAL_GetProcAddress      Fake_GetProcAddress
#define SERIAL_LoadLibraryW        Fake_LoadLibraryW
#define SERIAL_PurgeComm           Fake_PurgeComm
#define SERIAL_ReadFile            Fake_ReadFile
#define SERIAL_RegCloseKey         Fake_RegCloseKey
#define SERIAL_RegQueryValueExW    Fake_RegQueryValueExW
#define SERIAL_SetCommState        Fake_SetCommState
#define SERIAL_SetCommTimeouts     Fake_SetCommTimeouts
#define SERIAL_WriteFile           Fake_WriteFile

#include "../src/joystick/windows/SDL_serialjoystick.c"

#include "haptic/hidapi/SDL_hidapihaptic_c.h"
#include "haptic/hidapi/SDL_hidapihaptic.h"
#include "joystick/SDL_iforce_proto.h"

#include <stdio.h>
#include <wchar.h>

#define TEST_PRESENT_MS 5000 /* The wheel without N comes up a second after the others */
#define TEST_WAIT_MS    3000 /* Longest wait for a frame */
#define TEST_QUIET_MS   100  /* Nothing more may arrive in this long */

static int checks;
static int failures;

#define CHECK(condition, ...)                     \
    do {                                          \
        ++checks;                                 \
        if (!(condition)) {                       \
            ++failures;                           \
            printf("FAILED line %d: ", __LINE__); \
            printf(__VA_ARGS__);                  \
            printf("\n");                         \
        }                                         \
    } while (0)

/* The wheel's answers, as test/testserialiforce.c builds them from the
   protocol document's format: M 05EF and P 8886, the Boeder wheel, B 200 and
   N 10 */
static const Uint8 reply_o[] = { 0x2B, 0xFF, 0x01, 0x4F, 0x9A };
static const Uint8 reply_m[] = { 0x2B, 0xFF, 0x03, 0x4D, 0xEF, 0x05, 0x70 };
static const Uint8 reply_p[] = { 0x2B, 0xFF, 0x03, 0x50, 0x86, 0x88, 0x89 };
static const Uint8 reply_b[] = { 0x2B, 0xFF, 0x03, 0x42, 0xC8, 0x00, 0x5D };
static const Uint8 reply_n[] = { 0x2B, 0xFF, 0x02, 0x4E, 0x0A, 0x92 };
static const Uint8 reply_c[] = { 0x2B, 0xFF, 0x01, 0x43, 0x96 };
static const Uint8 reply_e[] = { 0x2B, 0xFF, 0x03, 0x45, 0x01, 0x00, 0x93 };
static const Uint8 reply_v[] = { 0x2B, 0xFF, 0x04, 0x56, 0x01, 0x02, 0x03, 0x86 };
/* M 1234 and P 5678, which no table lists */
static const Uint8 reply_m_unknown[] = { 0x2B, 0xFF, 0x03, 0x4D, 0x34, 0x12, 0xBC };
static const Uint8 reply_p_unknown[] = { 0x2B, 0xFF, 0x03, 0x50, 0x78, 0x56, 0xA9 };
static const Uint8 query_letters[] = { 'O', 'M', 'P', 'B', 'N', 'C', 'E', 'O', 'V' };

/* The Stinger's enable string and its answer, as Linux's stinger.c and
   inputattach exchange them */
static const Uint8 stinger_enable[] = { ' ', 'E', '5', 'E', '5' };
static const Uint8 stinger_reply[] = { 0x0D, 0x0A, '0', '6', '0', '0', '5', '2', '0', '0', '5', '8', 'C', '2', '7', '2' };

/* The commands, in the byte forms of Linux's iforce-main.c and iforce-ff.c:
   centering spring strength 0, centering spring on, force feedback on, full
   gain, then the constant effect's magnitude, envelope and core as
   test/testiforce.c checks them, its start, and stop all */
static const Uint8 cmd_spring_strength[] = { 0x40, 0x03, 0x00 };
static const Uint8 cmd_spring_on[] = { 0x40, 0x04, 0x01 };
static const Uint8 cmd_enable[] = { 0x42, 0x04 };
static const Uint8 cmd_gain[] = { 0x43, 0x7F };
static const Uint8 cmd_magnitude[] = { 0x03, 0x00, 0x00, 0x40 };
static const Uint8 cmd_envelope[] = { 0x02, 0x02, 0x00, 0x64, 0x00, 0x20, 0xC8, 0x00, 0x10 };
static const Uint8 cmd_core[] = { 0x01, 0x00, 0x00, 0x22, 0xE8, 0x03, 0x40, 0x32, 0x00, 0x00, 0x00, 0x02, 0x00, 0x05, 0x00 };
static const Uint8 cmd_play[] = { 0x41, 0x00, 0x01, 0x01 };
static const Uint8 cmd_stop_all[] = { 0x42, 0x01 };

typedef struct TestBytes
{
    const Uint8 *data;
    size_t length;
} TestBytes;

#define TEST_BYTES(array) { array, sizeof(array) }

/* Every command from the haptic open to the close, in order */
static const TestBytes expected_commands[] = {
    /* The haptic driver's open */
    TEST_BYTES(cmd_spring_strength),
    TEST_BYTES(cmd_spring_on),
    TEST_BYTES(cmd_enable),
    /* SDL_OpenHapticFromJoystick's full gain and centering off */
    TEST_BYTES(cmd_gain),
    TEST_BYTES(cmd_spring_strength),
    TEST_BYTES(cmd_spring_on),
    /* SDL_CreateHapticEffect and SDL_RunHapticEffect */
    TEST_BYTES(cmd_magnitude),
    TEST_BYTES(cmd_envelope),
    TEST_BYTES(cmd_core),
    TEST_BYTES(cmd_play),
    /* SDL_CloseHaptic */
    TEST_BYTES(cmd_stop_all)
};
#define TEST_OPEN_COMMANDS   6
#define TEST_EFFECT_COMMANDS 10
#define TEST_ALL_COMMANDS    ((int)SDL_arraysize(expected_commands))

/* The fake port */
#define FAKE_PORTS            4
#define FAKE_MAX_WRITES       64
#define FAKE_RX_SIZE          256
#define FAKE_STATUS_PENDING   ((ULONG_PTR)0x00000103)
#define FAKE_STATUS_CANCELLED ((ULONG_PTR)0xC0000120)

typedef enum FakeDevice
{
    FAKE_IFORCE,         /* Answers every query as the Boeder wheel */
    FAKE_IFORCE_NO_N,    /* The same wheel, leaving N unanswered */
    FAKE_STINGER,
    FAKE_IFORCE_UNKNOWN  /* The Boeder wheel's answers with M 1234 and P 5678 */
} FakeDevice;

typedef struct FakeWrite
{
    size_t length;
    Uint8 data[SDL_SERIAL_MAX_WRITE];
} FakeWrite;

typedef struct FakePort
{
    const WCHAR *path;
    const char *key; /* As the hint names it */
    FakeDevice device;
    HANDLE handle;   /* While open, a value no real handle has */
    int opens;
    int closes;
    DWORD baud;      /* The line as SetCommState last set it */
    BYTE byte_size;
    BYTE parity;
    BYTE stop_bits;
    Uint8 rx[FAKE_RX_SIZE]; /* Sent by the device and not yet read */
    size_t rx_length;
    OVERLAPPED *read; /* The host's read that waits for a byte */
    Uint8 *read_buffer;
    FakeWrite writes[FAKE_MAX_WRITES]; /* Every write, in order */
    int nwrites;
    int bad_frames; /* I-Force writes that are not one frame with its checksum */
} FakePort;

/* The port thread calls the fake, and the test reads what it recorded */
static SRWLOCK fake_lock = SRWLOCK_INIT;
static FakePort fake_ports[FAKE_PORTS] = {
    { L"\\\\.\\COM250", "COM250", FAKE_IFORCE },
    { L"\\\\.\\COM251", "COM251", FAKE_IFORCE_NO_N },
    { L"\\\\.\\COM252", "COM252", FAKE_STINGER },
    { L"\\\\.\\COM253", "COM253", FAKE_IFORCE_UNKNOWN }
};
static int fake_unexpected; /* Calls the fake does not expect */
static int fake_libraries;  /* LoadLibraryW calls, each for cfgmgr32.dll */

static void Fake_Lock(void)
{
    AcquireSRWLockExclusive(&fake_lock);
}

static void Fake_Unlock(void)
{
    ReleaseSRWLockExclusive(&fake_lock);
}

/* A real handle's value has its low two bits clear */
static HANDLE Fake_HandleValue(int index)
{
    return (HANDLE)(ULONG_PTR)(0x5E510001u + 0x10u * (unsigned int)index);
}

/* The rest of the fake runs with fake_lock held */
static FakePort *Fake_Find(HANDLE handle)
{
    int i;

    for (i = 0; i < FAKE_PORTS; ++i) {
        if (fake_ports[i].handle && fake_ports[i].handle == handle) {
            return &fake_ports[i];
        }
    }
    return NULL;
}

/* The XOR of every byte, which a frame's last byte carries for the ones
   before it, 2B included */
static Uint8 Xor(const Uint8 *data, size_t length)
{
    Uint8 check = 0;
    size_t i;

    for (i = 0; i < length; ++i) {
        check ^= data[i];
    }
    return check;
}

/* One I-Force frame: 2B, command, data length, data, checksum */
static bool IsFrame(const Uint8 *data, size_t length)
{
    return length >= 4 && data[0] == 0x2B && (size_t)data[2] + 4 == length && Xor(data, length - 1) == data[length - 1];
}

/* A command in its serial frame, built here from the protocol document's
   format and not by SDL */
static size_t Frame(Uint8 *out, const Uint8 *command, size_t length)
{
    out[0] = 0x2B;
    out[1] = command[0];
    out[2] = (Uint8)(length - 1);
    SDL_memcpy(out + 3, command + 1, length - 1);
    out[length + 2] = Xor(out, length + 2);
    return length + 3;
}

static bool Fake_LineIs(const FakePort *port, DWORD baud)
{
    return port->baud == baud && port->byte_size == 8 && port->parity == NOPARITY && port->stop_bits == ONESTOPBIT;
}

static void Fake_Send(FakePort *port, const Uint8 *data, size_t length)
{
    if (length > sizeof(port->rx) - port->rx_length) {
        ++fake_unexpected;
        return;
    }
    SDL_memcpy(port->rx + port->rx_length, data, length);
    port->rx_length += length;
}

/* A read that waits completes once a byte arrives, as a COM port read does
   when its interval timeout and total multiplier are MAXDWORD. It takes that
   byte, and the next read takes the rest at once. */
static void Fake_Deliver(FakePort *port)
{
    OVERLAPPED *read = port->read;

    if (!read || port->rx_length == 0) {
        return;
    }
    port->read_buffer[0] = port->rx[0];
    SDL_memmove(port->rx, port->rx + 1, port->rx_length - 1);
    --port->rx_length;
    port->read = NULL;
    read->Internal = 0;
    read->InternalHigh = 1;
    SetEvent(read->hEvent);
}

/* Ends the read that waits, as a cancel or a purge does */
static bool Fake_CancelRead(FakePort *port)
{
    OVERLAPPED *read = port->read;

    if (!read) {
        return false;
    }
    port->read = NULL;
    read->Internal = FAKE_STATUS_CANCELLED;
    read->InternalHigh = 0;
    SetEvent(read->hEvent);
    return true;
}

/* The wheel's answer to one write, which must be one whole frame */
static void Fake_IForceHears(FakePort *port, const Uint8 *data, size_t length)
{
    const Uint8 *reply;
    size_t reply_length;

    if (!IsFrame(data, length)) {
        ++port->bad_frames;
        return;
    }
    if (data[1] != 0xFF || data[2] != 1 || !Fake_LineIs(port, 38400)) {
        return;
    }
    switch (data[3]) {
    case 'O':
        reply = reply_o;
        reply_length = sizeof(reply_o);
        break;
    case 'M':
        reply = (port->device == FAKE_IFORCE_UNKNOWN) ? reply_m_unknown : reply_m;
        reply_length = sizeof(reply_m);
        break;
    case 'P':
        reply = (port->device == FAKE_IFORCE_UNKNOWN) ? reply_p_unknown : reply_p;
        reply_length = sizeof(reply_p);
        break;
    case 'B':
        reply = reply_b;
        reply_length = sizeof(reply_b);
        break;
    case 'N':
        if (port->device == FAKE_IFORCE_NO_N) {
            return;
        }
        reply = reply_n;
        reply_length = sizeof(reply_n);
        break;
    case 'C':
        reply = reply_c;
        reply_length = sizeof(reply_c);
        break;
    case 'E':
        reply = reply_e;
        reply_length = sizeof(reply_e);
        break;
    case 'V':
        reply = reply_v;
        reply_length = sizeof(reply_v);
        break;
    default:
        ++fake_unexpected;
        return;
    }
    Fake_Send(port, reply, reply_length);
}

static void Fake_StingerHears(FakePort *port, const Uint8 *data, size_t length)
{
    if (length == sizeof(stinger_enable) && SDL_memcmp(data, stinger_enable, length) == 0 && Fake_LineIs(port, 1200)) {
        Fake_Send(port, stinger_reply, sizeof(stinger_reply));
    }
}

static HANDLE WINAPI Fake_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                      DWORD disposition, DWORD flags, HANDLE template_file)
{
    HANDLE result = INVALID_HANDLE_VALUE;
    DWORD error = ERROR_FILE_NOT_FOUND;
    int i;

    Fake_Lock();
    for (i = 0; i < FAKE_PORTS; ++i) {
        FakePort *port = &fake_ports[i];

        if (!name || wcscmp(name, port->path) != 0) {
            continue;
        }
        /* A communications resource opens exclusive and existing, and the
           driver's I/O on it is overlapped */
        if (access != (GENERIC_READ | GENERIC_WRITE) || share != 0 || security || disposition != OPEN_EXISTING ||
            flags != FILE_FLAG_OVERLAPPED || template_file) {
            ++fake_unexpected;
            error = ERROR_INVALID_PARAMETER;
        } else if (port->handle) {
            ++fake_unexpected;
            error = ERROR_ACCESS_DENIED;
        } else {
            port->handle = Fake_HandleValue(i);
            ++port->opens;
            port->baud = 0;
            port->rx_length = 0;
            port->read = NULL;
            result = port->handle;
        }
        break;
    }
    if (i == FAKE_PORTS) {
        ++fake_unexpected;
    }
    Fake_Unlock();
    if (result == INVALID_HANDLE_VALUE) {
        SetLastError(error);
    }
    return result;
}

static BOOL WINAPI Fake_CloseHandle(HANDLE handle)
{
    FakePort *port;

    Fake_Lock();
    port = Fake_Find(handle);
    if (port) {
        /* The driver cancels its read and waits for it before it closes */
        if (port->read) {
            ++fake_unexpected;
            port->read = NULL;
        }
        port->handle = NULL;
        port->rx_length = 0;
        ++port->closes;
    }
    Fake_Unlock();
    /* The driver's events are real */
    return port ? TRUE : CloseHandle(handle);
}

static BOOL WINAPI Fake_GetCommState(HANDLE file, LPDCB dcb)
{
    FakePort *port;

    Fake_Lock();
    port = Fake_Find(file);
    if (port && dcb && dcb->DCBlength == sizeof(*dcb)) {
        SDL_zerop(dcb);
        dcb->DCBlength = sizeof(*dcb);
        dcb->fBinary = TRUE;
        dcb->BaudRate = port->baud ? port->baud : CBR_9600;
        dcb->ByteSize = port->baud ? port->byte_size : 8;
        dcb->Parity = port->baud ? port->parity : NOPARITY;
        dcb->StopBits = port->baud ? port->stop_bits : ONESTOPBIT;
    } else {
        port = NULL;
        ++fake_unexpected;
    }
    Fake_Unlock();
    if (!port) {
        SetLastError(ERROR_INVALID_PARAMETER);
    }
    return port ? TRUE : FALSE;
}

static BOOL WINAPI Fake_SetCommState(HANDLE file, LPDCB dcb)
{
    FakePort *port;

    Fake_Lock();
    port = Fake_Find(file);
    if (port && dcb && dcb->DCBlength == sizeof(*dcb)) {
        port->baud = dcb->BaudRate;
        port->byte_size = dcb->ByteSize;
        port->parity = dcb->Parity;
        port->stop_bits = dcb->StopBits;
    } else {
        port = NULL;
        ++fake_unexpected;
    }
    Fake_Unlock();
    if (!port) {
        SetLastError(ERROR_INVALID_PARAMETER);
    }
    return port ? TRUE : FALSE;
}

/* The fake's reads complete as the engine's timeouts make a COM port's
   complete, so any other timeouts are unexpected */
static BOOL WINAPI Fake_SetCommTimeouts(HANDLE file, LPCOMMTIMEOUTS timeouts)
{
    FakePort *port;

    Fake_Lock();
    port = Fake_Find(file);
    if (!port || !timeouts || timeouts->ReadIntervalTimeout != MAXDWORD || timeouts->ReadTotalTimeoutMultiplier != MAXDWORD ||
        timeouts->ReadTotalTimeoutConstant == 0 || timeouts->ReadTotalTimeoutConstant == MAXDWORD) {
        port = NULL;
        ++fake_unexpected;
    }
    Fake_Unlock();
    if (!port) {
        SetLastError(ERROR_INVALID_PARAMETER);
    }
    return port ? TRUE : FALSE;
}

/* PURGE_RXABORT ends a read that waits, and PURGE_RXCLEAR drops what the
   device sent. Writes finish at once here, so the transmit flags change
   nothing. */
static BOOL WINAPI Fake_PurgeComm(HANDLE file, DWORD flags)
{
    FakePort *port;

    Fake_Lock();
    port = Fake_Find(file);
    if (port) {
        if (flags & PURGE_RXABORT) {
            Fake_CancelRead(port);
        }
        if (flags & PURGE_RXCLEAR) {
            port->rx_length = 0;
        }
    } else {
        ++fake_unexpected;
    }
    Fake_Unlock();
    if (!port) {
        SetLastError(ERROR_INVALID_HANDLE);
    }
    return port ? TRUE : FALSE;
}

static BOOL WINAPI Fake_EscapeCommFunction(HANDLE file, DWORD function)
{
    FakePort *port;

    (void)function;
    Fake_Lock();
    port = Fake_Find(file);
    if (!port) {
        ++fake_unexpected;
    }
    Fake_Unlock();
    if (!port) {
        SetLastError(ERROR_INVALID_HANDLE);
    }
    return port ? TRUE : FALSE;
}

static BOOL WINAPI Fake_FlushFileBuffers(HANDLE file)
{
    FakePort *port;

    Fake_Lock();
    port = Fake_Find(file);
    if (!port) {
        ++fake_unexpected;
    }
    Fake_Unlock();
    if (!port) {
        SetLastError(ERROR_INVALID_HANDLE);
    }
    return port ? TRUE : FALSE;
}

/* Every write goes out at once, and the device answers it */
static BOOL WINAPI Fake_WriteFile(HANDLE file, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED overlapped)
{
    FakePort *port;
    BOOL result = FALSE;

    Fake_Lock();
    port = Fake_Find(file);
    if (!port || !overlapped || !buffer || size == 0 || size > SDL_SERIAL_MAX_WRITE || port->nwrites == FAKE_MAX_WRITES) {
        ++fake_unexpected;
    } else {
        FakeWrite *write = &port->writes[port->nwrites++];

        write->length = size;
        SDL_memcpy(write->data, buffer, size);
        if (port->device == FAKE_STINGER) {
            Fake_StingerHears(port, write->data, write->length);
        } else {
            Fake_IForceHears(port, write->data, write->length);
        }
        Fake_Deliver(port);
        overlapped->Internal = 0;
        overlapped->InternalHigh = size;
        if (written) {
            *written = size;
        }
        SetEvent(overlapped->hEvent);
        result = TRUE;
    }
    Fake_Unlock();
    if (!result) {
        SetLastError(ERROR_INVALID_PARAMETER);
    }
    return result;
}

/* Bytes the device has sent come at once. Otherwise the read waits. */
static BOOL WINAPI Fake_ReadFile(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped)
{
    FakePort *port;
    DWORD error = ERROR_INVALID_PARAMETER;
    BOOL result = FALSE;

    Fake_Lock();
    port = Fake_Find(file);
    if (!port || !overlapped || !buffer || size == 0 || port->read) {
        ++fake_unexpected;
    } else if (port->rx_length > 0) {
        const DWORD count = (DWORD)SDL_min((size_t)size, port->rx_length);

        SDL_memcpy(buffer, port->rx, count);
        SDL_memmove(port->rx, port->rx + count, port->rx_length - count);
        port->rx_length -= count;
        overlapped->Internal = 0;
        overlapped->InternalHigh = count;
        if (read) {
            *read = count;
        }
        SetEvent(overlapped->hEvent);
        result = TRUE;
    } else {
        overlapped->Internal = FAKE_STATUS_PENDING;
        overlapped->InternalHigh = 0;
        port->read = overlapped;
        port->read_buffer = (Uint8 *)buffer;
        error = ERROR_IO_PENDING;
    }
    Fake_Unlock();
    if (!result) {
        SetLastError(error);
    }
    return result;
}

static BOOL WINAPI Fake_GetOverlappedResult(HANDLE file, LPOVERLAPPED overlapped, LPDWORD transferred, BOOL wait)
{
    FakePort *port;
    DWORD error = ERROR_INVALID_PARAMETER;
    BOOL result = FALSE;

    Fake_Lock();
    port = Fake_Find(file);
    if (!port || !overlapped || !transferred) {
        ++fake_unexpected;
    } else if (overlapped->Internal == FAKE_STATUS_PENDING) {
        /* The driver waits only after it cancels, which ends every read
           here, and writes finish at once */
        if (wait) {
            ++fake_unexpected;
        }
        error = ERROR_IO_INCOMPLETE;
    } else if (overlapped->Internal == FAKE_STATUS_CANCELLED) {
        *transferred = 0;
        error = ERROR_OPERATION_ABORTED;
    } else {
        *transferred = (DWORD)overlapped->InternalHigh;
        result = TRUE;
    }
    Fake_Unlock();
    if (!result) {
        SetLastError(error);
    }
    return result;
}

/* Returns whether the handle is an open port, and cancels its read */
static bool Fake_Cancel(HANDLE file, bool *canceled)
{
    FakePort *port;

    Fake_Lock();
    port = Fake_Find(file);
    *canceled = port && Fake_CancelRead(port);
    if (!port) {
        ++fake_unexpected;
    }
    Fake_Unlock();
    return port != NULL;
}

static BOOL WINAPI Fake_CancelIoEx(HANDLE file, LPOVERLAPPED overlapped)
{
    bool canceled = false;
    bool known;

    /* The driver cancels everything on the handle */
    if (overlapped) {
        Fake_Lock();
        ++fake_unexpected;
        Fake_Unlock();
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    known = Fake_Cancel(file, &canceled);
    if (!canceled) {
        SetLastError(known ? ERROR_NOT_FOUND : ERROR_INVALID_HANDLE);
    }
    return canceled ? TRUE : FALSE;
}

static BOOL WINAPI Fake_CancelIo(HANDLE file)
{
    bool canceled = false;
    const bool known = Fake_Cancel(file, &canceled);

    if (!known) {
        SetLastError(ERROR_INVALID_HANDLE);
    }
    return known ? TRUE : FALSE;
}

/* The configuration manager does not load, so the driver opens ports only
   by COM name, and a scan finds nothing */
static HMODULE WINAPI Fake_LoadLibraryW(LPCWSTR name)
{
    Fake_Lock();
    if (name && wcscmp(name, L"cfgmgr32.dll") == 0) {
        ++fake_libraries;
    } else {
        ++fake_unexpected;
    }
    Fake_Unlock();
    SetLastError(ERROR_MOD_NOT_FOUND);
    return NULL;
}

/* The driver asks kernel32 for CancelIoEx, and nothing else, since the
   configuration manager never loads */
static FARPROC WINAPI Fake_GetProcAddress(HMODULE module, LPCSTR name)
{
    if (module && module == GetModuleHandleW(L"kernel32.dll") && name && SDL_strcmp(name, "CancelIoEx") == 0) {
        return (FARPROC)Fake_CancelIoEx;
    }
    Fake_Lock();
    ++fake_unexpected;
    Fake_Unlock();
    SetLastError(ERROR_PROC_NOT_FOUND);
    return NULL;
}

static BOOL WINAPI Fake_FreeLibrary(HMODULE module)
{
    (void)module;
    Fake_Lock();
    ++fake_unexpected;
    Fake_Unlock();
    return FALSE;
}

static LSTATUS WINAPI Fake_RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size)
{
    (void)key;
    (void)name;
    (void)reserved;
    (void)type;
    (void)data;
    (void)size;
    Fake_Lock();
    ++fake_unexpected;
    Fake_Unlock();
    return ERROR_FILE_NOT_FOUND;
}

static LSTATUS WINAPI Fake_RegCloseKey(HKEY key)
{
    (void)key;
    Fake_Lock();
    ++fake_unexpected;
    Fake_Unlock();
    return ERROR_INVALID_HANDLE;
}

/* What the test reads from the fake */

/* The frames the host wrote to an I-Force port other than its queries, in
   order. Returns their count, which may exceed max. */
static int CommandFrames(int index, FakeWrite *out, int max)
{
    const FakePort *port = &fake_ports[index];
    int i, count = 0;

    Fake_Lock();
    for (i = 0; i < port->nwrites; ++i) {
        const FakeWrite *write = &port->writes[i];

        if (write->length >= 2 && write->data[1] != 0xFF) {
            if (out && count < max) {
                out[count] = *write;
            }
            ++count;
        }
    }
    Fake_Unlock();
    return count;
}

static int WaitForCommandFrames(int index, int count)
{
    const Uint64 start = SDL_GetTicks();
    int now = CommandFrames(index, NULL, 0);

    while (now < count && SDL_GetTicks() - start < TEST_WAIT_MS) {
        SDL_Delay(1);
        now = CommandFrames(index, NULL, 0);
    }
    return now;
}

static bool FrameIs(const FakeWrite *write, const Uint8 *command, size_t length)
{
    Uint8 frame[4 + SDL_IFORCE_MAX_COMMAND];
    const size_t frame_length = Frame(frame, command, length);

    return write->length == frame_length && SDL_memcmp(write->data, frame, frame_length) == 0;
}

/* The port's first writes are the nine queries in order, each once, and the
   rest are commands */
static bool AskedEveryQuery(int index)
{
    const FakePort *port = &fake_ports[index];
    bool result;
    int i;

    Fake_Lock();
    result = port->nwrites >= (int)sizeof(query_letters);
    for (i = 0; result && i < port->nwrites; ++i) {
        const FakeWrite *write = &port->writes[i];
        const bool query = IsFrame(write->data, write->length) && write->data[1] == 0xFF;

        if (i < (int)sizeof(query_letters)) {
            result = query && write->length == 5 && write->data[3] == query_letters[i];
        } else {
            result = !query;
        }
    }
    Fake_Unlock();
    return result;
}

/* The Stinger's port got its enable string and nothing else */
static bool OnlyEnabled(int index)
{
    const FakePort *port = &fake_ports[index];
    bool result;
    int i;

    Fake_Lock();
    result = port->nwrites > 0;
    for (i = 0; result && i < port->nwrites; ++i) {
        result = port->writes[i].length == sizeof(stinger_enable) &&
                 SDL_memcmp(port->writes[i].data, stinger_enable, sizeof(stinger_enable)) == 0;
    }
    Fake_Unlock();
    return result;
}

/* The USB side: the same haptic driver on a joystick record whose effects
   this test records, as test/testiforcedriver.c runs it. The I-Force HIDAPI
   driver writes each command it is given to the OUT endpoint unchanged. */
#define TEST_MAX_USB_COMMANDS 32

typedef struct TestCommand
{
    int length;
    Uint8 data[SDL_IFORCE_MAX_COMMAND];
} TestCommand;

static TestCommand usb_commands[TEST_MAX_USB_COMMANDS];
static int usb_count;
static int usb_dropped;
static SDL_JoystickDriver usb_driver;

/* Every send holds the joystick lock */
static bool UsbSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    (void)joystick;
    if (usb_count < TEST_MAX_USB_COMMANDS && size > 0 && size <= SDL_IFORCE_MAX_COMMAND) {
        usb_commands[usb_count].length = size;
        SDL_memcpy(usb_commands[usb_count].data, data, (size_t)size);
        ++usb_count;
    } else {
        ++usb_dropped;
    }
    return true;
}

/* A joystick record of a driver, with a USB GUID, as the HIDAPI haptic
   layer and its drivers see a joystick */
static SDL_Joystick *CreateRecord(SDL_JoystickDriver *driver, Uint16 vendor, Uint16 product, const char *name)
{
    SDL_Joystick *record = (SDL_Joystick *)SDL_calloc(1, sizeof(*record));

    if (!record) {
        return NULL;
    }
    record->driver = driver;
    record->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_USB, vendor, product, 0, NULL, name, 0, 0);
    record->ref_count = 1;
    SDL_SetObjectValid(record, SDL_OBJECT_TYPE_JOYSTICK, true);
    return record;
}

static void DestroyRecord(SDL_Joystick *record)
{
    if (!record) {
        return;
    }
    SDL_SetObjectValid(record, SDL_OBJECT_TYPE_JOYSTICK, false);
    if (record->props) {
        SDL_DestroyProperties(record->props);
    }
    SDL_free(record);
}

/* Runs the haptic driver on a record with the serial joystick's properties,
   calling it as SDL's haptic API calls it for the serial joystick in
   TestFrames */
static bool RunUsbSide(SDL_Joystick *serial_joystick, const SDL_HapticEffect *effect)
{
    static const char *const names[] = {
        SDL_IFORCE_PROP_EFFECTS_NUMBER, SDL_IFORCE_PROP_MEMORY_NUMBER,
        SDL_IFORCE_PROP_VENDOR_NUMBER, SDL_IFORCE_PROP_PRODUCT_NUMBER
    };
    SDL_Joystick *record = CreateRecord(&usb_driver, 0, 0, "I-Force USB side");
    SDL_PropertiesID from, to;
    SDL_HIDAPI_HapticDevice device;
    SDL_HapticEffectID id;
    bool opened;
    void *ctx;
    int i;

    if (!record) {
        return false;
    }
    usb_driver.SendEffect = UsbSendEffect;
    from = SDL_GetJoystickProperties(serial_joystick);
    to = SDL_GetJoystickProperties(record);
    for (i = 0; i < (int)SDL_arraysize(names); ++i) {
        SDL_SetNumberProperty(to, names[i], SDL_GetNumberProperty(from, names[i], 0));
    }

    /* SDL_OpenHapticFromJoystick opens it under the joystick lock */
    SDL_LockJoysticks();
    ctx = SDL_HIDAPI_HapticDriverIForce.Open(record);
    SDL_UnlockJoysticks();
    opened = (ctx != NULL);
    if (opened) {
        SDL_zero(device);
        device.joystick = record;
        device.driver = &SDL_HIDAPI_HapticDriverIForce;
        device.ctx = ctx;
        device.driver->SetGain(&device, 100);
        device.driver->SetAutocenter(&device, 0);
        id = device.driver->CreateEffect(&device, effect);
        if (id >= 0) {
            device.driver->RunEffect(&device, id, 1);
        }
        device.driver->Close(&device);
        SDL_free(ctx);
    }
    DestroyRecord(record);
    return opened;
}

/* The constant effect whose commands test/testiforce.c checks */
static void ConstantEffect(SDL_HapticEffect *effect)
{
    SDL_zerop(effect);
    effect->type = SDL_HAPTIC_CONSTANT;
    effect->constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect->constant.length = 1000;
    effect->constant.delay = 5;
    effect->constant.button = 2;
    effect->constant.interval = 50;
    effect->constant.level = 0x4000;
    effect->constant.attack_length = 100;
    effect->constant.attack_level = 0x2000;
    effect->constant.fade_length = 200;
    effect->constant.fade_level = 0x1000;
}

/* SDL */

/* Every other joystick driver off, so only the driver under test adds a
   joystick, whatever the environment holds */
static const char *const test_hints[][2] = {
    { "SDL_JOYSTICK_HIDAPI", "0" },
    { "SDL_JOYSTICK_RAWINPUT", "0" },
    { "SDL_JOYSTICK_DIRECTINPUT", "0" },
    { "SDL_XINPUT_ENABLED", "0" },
    { "SDL_JOYSTICK_WGI", "0" },
    { "SDL_JOYSTICK_GAMEINPUT", "0" },
    { "SDL_JOYSTICK_GAMEINPUT_RAW", "0" },
    { "SDL_JOYSTICK_BLE", "0" },
    { "SDL_JOYSTICK_BLE_SWITCH2", "0" },
    { "SDL_JOYSTICK_SERIAL_AUTO", "0" },
    { "SDL_JOYSTICK_DJI_REMOTE_TCP_HOSTS", "" },
    { "SDL_JOYSTICK_RFCOMM", "0" },
    { "SDL_JOYSTICK_ICADE", "0" },
    { "SDL_JOYSTICK_WINMM", "0" },
    { "SDL_JOYSTICK_THREAD", "0" },
    { "SDL_JOYSTICK_ROG_CHAKRAM", "0" },
    /* The ports under test */
    { "SDL_JOYSTICK_SERIAL", "COM250=iforce,COM251=iforce,COM252=stinger,COM253=iforce" }
};

static SDL_JoystickID FindJoystick(const char *key)
{
    SDL_JoystickID *ids;
    SDL_JoystickID result = 0;
    int count = 0, i;

    ids = SDL_GetJoysticks(&count);
    for (i = 0; ids && i < count; ++i) {
        const char *path = SDL_GetJoystickPathForID(ids[i]);

        if (path && SDL_strcmp(path, key) == 0) {
            result = ids[i];
            break;
        }
    }
    SDL_free(ids);
    return result;
}

static SDL_Joystick *OpenPort(const char *key)
{
    const Uint64 start = SDL_GetTicks();
    SDL_JoystickID id = FindJoystick(key);

    while (!id && SDL_GetTicks() - start < TEST_PRESENT_MS) {
        SDL_Delay(5);
        SDL_UpdateJoysticks();
        id = FindJoystick(key);
    }
    CHECK(id != 0, "no joystick on %s within %d ms", key, TEST_PRESENT_MS);
    return id ? SDL_OpenJoystick(id) : NULL;
}

static const char *NameOf(SDL_Joystick *joystick)
{
    const char *name = SDL_GetJoystickName(joystick);

    return name ? name : "(none)";
}

/* 1 */
static void TestProperties(SDL_Joystick *wheel)
{
    const SDL_PropertiesID props = SDL_GetJoystickProperties(wheel);
    const SDL_GUID guid = SDL_GetJoystickGUID(wheel);
    Uint16 vendor = 0xFFFF, product = 0xFFFF;

    CHECK(SDL_strcmp(NameOf(wheel), "Boeder Force Feedback Wheel") == 0, "COM250 is %s", NameOf(wheel));
    CHECK(SDL_GetNumberProperty(props, SDL_IFORCE_PROP_EFFECTS_NUMBER, -1) == 10, "effects %d",
          (int)SDL_GetNumberProperty(props, SDL_IFORCE_PROP_EFFECTS_NUMBER, -1));
    CHECK(SDL_GetNumberProperty(props, SDL_IFORCE_PROP_MEMORY_NUMBER, -1) == 200, "memory end %d",
          (int)SDL_GetNumberProperty(props, SDL_IFORCE_PROP_MEMORY_NUMBER, -1));
    CHECK(SDL_GetNumberProperty(props, SDL_IFORCE_PROP_VENDOR_NUMBER, -1) == 0x05EF, "vendor property %x",
          (unsigned int)SDL_GetNumberProperty(props, SDL_IFORCE_PROP_VENDOR_NUMBER, -1));
    CHECK(SDL_GetNumberProperty(props, SDL_IFORCE_PROP_PRODUCT_NUMBER, -1) == 0x8886, "product property %x",
          (unsigned int)SDL_GetNumberProperty(props, SDL_IFORCE_PROP_PRODUCT_NUMBER, -1));

    /* A port the hint names has no IDs SDL can trust */
    SDL_GetJoystickGUIDInfo(guid, &vendor, &product, NULL, NULL);
    CHECK((guid.data[0] | (guid.data[1] << 8)) == SDL_HARDWARE_BUS_SERIAL, "the GUID's bus is %02x%02x", guid.data[1], guid.data[0]);
    CHECK(vendor == 0 && product == 0, "the GUID carries %04x:%04x", vendor, product);
    CHECK(SDL_GetJoystickVendor(wheel) == 0 && SDL_GetJoystickProduct(wheel) == 0, "the joystick reports %04x:%04x",
          SDL_GetJoystickVendor(wheel), SDL_GetJoystickProduct(wheel));
}

/* 2 */
static void TestHapticSupport(SDL_Joystick *wheel, SDL_Joystick *silent, SDL_Joystick *unknown)
{
    const SDL_PropertiesID props = SDL_GetJoystickProperties(silent);
    const SDL_PropertiesID unknown_props = SDL_GetJoystickProperties(unknown);
    SDL_Haptic *haptic;

    CHECK(SDL_IsJoystickHaptic(wheel), "the Boeder wheel has no haptic device: %s", SDL_GetError());
    CHECK(SDL_strcmp(NameOf(unknown), "Unknown I-Force Device [1234:5678]") == 0, "COM253 is %s", NameOf(unknown));
    CHECK(SDL_GetNumberProperty(unknown_props, SDL_IFORCE_PROP_VENDOR_NUMBER, -1) == 0x1234 &&
              SDL_GetNumberProperty(unknown_props, SDL_IFORCE_PROP_PRODUCT_NUMBER, -1) == 0x5678 &&
              SDL_GetNumberProperty(unknown_props, SDL_IFORCE_PROP_EFFECTS_NUMBER, -1) == 10,
          "the device no table lists lacks its IDs or effects");
    CHECK(SDL_IsJoystickHaptic(unknown), "the device no table lists has no haptic device: %s", SDL_GetError());
    CHECK(SDL_strcmp(NameOf(silent), "Boeder Force Feedback Wheel") == 0, "COM251 is %s", NameOf(silent));
    CHECK(SDL_GetNumberProperty(props, SDL_IFORCE_PROP_VENDOR_NUMBER, -1) == 0x05EF &&
              SDL_GetNumberProperty(props, SDL_IFORCE_PROP_PRODUCT_NUMBER, -1) == 0x8886,
          "the wheel without N lacks its IDs");
    CHECK(!SDL_HasProperty(props, SDL_IFORCE_PROP_EFFECTS_NUMBER) && !SDL_HasProperty(props, SDL_IFORCE_PROP_MEMORY_NUMBER),
          "the wheel without N reports effects");
    CHECK(!SDL_IsJoystickHaptic(silent), "the wheel without N has a haptic device");
    haptic = SDL_OpenHapticFromJoystick(silent);
    CHECK(haptic == NULL, "the wheel without N opened a haptic device");
    if (haptic) {
        SDL_CloseHaptic(haptic);
    }

    SDL_LockJoysticks();
    CHECK(SDL_HIDAPI_JoystickIsHaptic(wheel), "the haptic layer does not serve the Boeder wheel");
    CHECK(SDL_HIDAPI_JoystickIsHaptic(unknown), "the haptic layer does not serve the device no table lists");
    CHECK(!SDL_HIDAPI_JoystickIsHaptic(silent), "the haptic layer serves the wheel without N");
    SDL_UnlockJoysticks();
}

static void CheckFrames(const FakeWrite *frames, int first, int count, const char *what)
{
    int i;

    for (i = first; i < first + count; ++i) {
        CHECK(FrameIs(&frames[i], expected_commands[i].data, expected_commands[i].length),
              "%s: frame %d is not command %02x in its frame", what, i, expected_commands[i].data[0]);
    }
}

/* 3 and 4 */
static void TestFrames(SDL_Joystick *wheel)
{
    FakeWrite frames[TEST_ALL_COMMANDS + 4];
    SDL_HapticEffect effect;
    SDL_HapticEffectID id;
    SDL_Haptic *haptic, *again;
    int count, i;

    ConstantEffect(&effect);
    CHECK(CommandFrames(0, NULL, 0) == 0, "commands went out before the haptic device opened");

    /* The USB side, and the Linux byte forms */
    CHECK(RunUsbSide(wheel, &effect), "the haptic driver did not open on the joystick record: %s", SDL_GetError());
    CHECK(usb_count == TEST_ALL_COMMANDS && usb_dropped == 0, "%d commands on the USB side, %d dropped", usb_count, usb_dropped);
    for (i = 0; i < usb_count && i < TEST_ALL_COMMANDS; ++i) {
        CHECK(usb_commands[i].length == (int)expected_commands[i].length &&
                  SDL_memcmp(usb_commands[i].data, expected_commands[i].data, expected_commands[i].length) == 0,
              "USB command %d is not command %02x", i, expected_commands[i].data[0]);
    }

    /* 3: Linux's start comes first */
    haptic = SDL_OpenHapticFromJoystick(wheel);
    CHECK(haptic != NULL, "the Boeder wheel's haptic device did not open: %s", SDL_GetError());
    if (!haptic) {
        return;
    }
    count = WaitForCommandFrames(0, TEST_OPEN_COMMANDS);
    CHECK(count == TEST_OPEN_COMMANDS, "%d frames after the open", count);
    CommandFrames(0, frames, (int)SDL_arraysize(frames));
    CHECK(FrameIs(&frames[0], cmd_spring_strength, sizeof(cmd_spring_strength)) &&
              FrameIs(&frames[1], cmd_spring_on, sizeof(cmd_spring_on)) &&
              FrameIs(&frames[2], cmd_enable, sizeof(cmd_enable)),
          "the open did not start with 40 03 00, 40 04 01 and 42 04");
    CheckFrames(frames, 0, SDL_min(count, TEST_OPEN_COMMANDS), "open");
    CHECK(SDL_GetMaxHapticEffects(haptic) == 10, "%d effects", SDL_GetMaxHapticEffects(haptic));
    /* The joystick's haptic device again, which sends nothing */
    again = SDL_OpenHapticFromJoystick(wheel);
    CHECK(again == haptic, "a second open made another haptic device");
    if (again) {
        SDL_CloseHaptic(again);
    }

    /* 4 */
    id = SDL_CreateHapticEffect(haptic, &effect);
    CHECK(id >= 0, "no effect: %s", SDL_GetError());
    CHECK(id >= 0 && SDL_RunHapticEffect(haptic, id, 1), "the effect did not run: %s", SDL_GetError());
    count = WaitForCommandFrames(0, TEST_EFFECT_COMMANDS);
    CHECK(count == TEST_EFFECT_COMMANDS, "%d frames after the effect", count);
    CommandFrames(0, frames, (int)SDL_arraysize(frames));
    CheckFrames(frames, TEST_OPEN_COMMANDS, SDL_max(0, SDL_min(count, TEST_EFFECT_COMMANDS) - TEST_OPEN_COMMANDS), "effect");

    /* 3: stop all comes last */
    SDL_CloseHaptic(haptic);
    (void)WaitForCommandFrames(0, TEST_ALL_COMMANDS);
    SDL_Delay(TEST_QUIET_MS);
    count = CommandFrames(0, frames, (int)SDL_arraysize(frames));
    CHECK(count == TEST_ALL_COMMANDS, "%d frames after the close", count);
    CHECK(count == TEST_ALL_COMMANDS && FrameIs(&frames[count - 1], cmd_stop_all, sizeof(cmd_stop_all)),
          "the close did not end with 42 01");

    /* Each command the USB side got, in its frame, in order */
    for (i = 0; i < count && i < usb_count; ++i) {
        CHECK(FrameIs(&frames[i], usb_commands[i].data, (size_t)usb_commands[i].length),
              "frame %d is not USB command %d in its frame", i, i);
    }
}

/* 5 */
static void TestStinger(SDL_Joystick *stinger)
{
    const SDL_PropertiesID props = SDL_GetJoystickProperties(stinger);
    SDL_Haptic *haptic;

    CHECK(SDL_strcmp(NameOf(stinger), "Gravis Stinger") == 0, "COM252 is %s", NameOf(stinger));
    CHECK(!SDL_HasProperty(props, SDL_IFORCE_PROP_VENDOR_NUMBER) && !SDL_HasProperty(props, SDL_IFORCE_PROP_EFFECTS_NUMBER),
          "the Stinger carries I-Force properties");
    /* A gamepad has no haptic device through SDL_IsJoystickHaptic, so the
       haptic layer is asked too */
    CHECK(SDL_IsGamepad(SDL_GetJoystickID(stinger)), "the Stinger is not a gamepad");
    CHECK(!SDL_IsJoystickHaptic(stinger), "the Stinger has a haptic device");
    haptic = SDL_OpenHapticFromJoystick(stinger);
    CHECK(haptic == NULL, "the Stinger opened a haptic device");
    if (haptic) {
        SDL_CloseHaptic(haptic);
    }
    SDL_LockJoysticks();
    CHECK(!SDL_HIDAPI_JoystickIsHaptic(stinger), "the haptic layer serves the Stinger");
    SDL_UnlockJoysticks();
}

/* 6. Only the driver a joystick record names changes between the pairs. */
static void TestLayerAdmission(void)
{
    SDL_Joystick *guillemot_other = CreateRecord(&usb_driver, USB_VENDOR_GUILLEMOT, USB_PRODUCT_GUILLEMOT_FFB_RACING_WHEEL,
                                                 "Guillemot Force Feedback Racing Wheel");
    SDL_Joystick *guillemot_hidapi = CreateRecord(&SDL_HIDAPI_JoystickDriver, USB_VENDOR_GUILLEMOT, USB_PRODUCT_GUILLEMOT_FFB_RACING_WHEEL,
                                                  "Guillemot Force Feedback Racing Wheel");
    SDL_Joystick *g29_serial = CreateRecord(&SDL_SERIAL_JoystickDriver, 0x046D, 0xC24F, "G29 Driving Force Racing Wheel");
    SDL_Joystick *g29_hidapi = CreateRecord(&SDL_HIDAPI_JoystickDriver, 0x046D, 0xC24F, "G29 Driving Force Racing Wheel");

    CHECK(guillemot_other && guillemot_hidapi && g29_serial && g29_hidapi, "no joystick records");
    if (guillemot_other && guillemot_hidapi && g29_serial && g29_hidapi) {
        /* The properties the I-Force HIDAPI driver sets once N and B are known */
        SDL_SetNumberProperty(SDL_GetJoystickProperties(guillemot_other), SDL_IFORCE_PROP_EFFECTS_NUMBER, 10);
        SDL_SetNumberProperty(SDL_GetJoystickProperties(guillemot_other), SDL_IFORCE_PROP_MEMORY_NUMBER, 200);
        SDL_SetNumberProperty(SDL_GetJoystickProperties(guillemot_hidapi), SDL_IFORCE_PROP_EFFECTS_NUMBER, 10);
        SDL_SetNumberProperty(SDL_GetJoystickProperties(guillemot_hidapi), SDL_IFORCE_PROP_MEMORY_NUMBER, 200);

        SDL_LockJoysticks();
        CHECK(SDL_HIDAPI_JoystickIsHaptic(guillemot_hidapi), "the haptic layer does not serve a HIDAPI I-Force wheel");
        CHECK(!SDL_HIDAPI_JoystickIsHaptic(guillemot_other), "the haptic layer serves another driver's joystick");
        CHECK(SDL_HIDAPI_JoystickIsHaptic(g29_hidapi), "the haptic layer does not serve a HIDAPI G29");
        CHECK(!SDL_HIDAPI_JoystickIsHaptic(g29_serial), "the Logitech driver serves a serial joystick");
        SDL_UnlockJoysticks();
    }
    DestroyRecord(guillemot_other);
    DestroyRecord(guillemot_hidapi);
    DestroyRecord(g29_serial);
    DestroyRecord(g29_hidapi);
}

/* After SDL_Quit */
static void CheckPorts(void)
{
    int i;

    for (i = 0; i < FAKE_PORTS; ++i) {
        const FakePort *port = &fake_ports[i];

        /* A port that was lost would have opened again */
        CHECK(port->opens == 1 && port->closes == 1 && !port->handle, "%s opened %d times and closed %d times",
              port->key, port->opens, port->closes);
        CHECK(port->bad_frames == 0, "%s got %d writes that are not one frame", port->key, port->bad_frames);
    }
    CHECK(AskedEveryQuery(0), "COM250 did not get the nine queries once each, then only commands");
    CHECK(AskedEveryQuery(1) && CommandFrames(1, NULL, 0) == 0, "COM251 got more than the nine queries");
    CHECK(OnlyEnabled(2), "COM252 got more than the Stinger's enable string");
    CHECK(AskedEveryQuery(3) && CommandFrames(3, NULL, 0) == 0, "COM253 got more than the nine queries");
    CHECK(fake_libraries > 0, "the driver's library load never reached the fake");
    CHECK(fake_unexpected == 0, "%d calls the fake did not expect", fake_unexpected);
}

int main(int argc, char *argv[])
{
    SDL_Joystick *wheel, *silent, *stinger, *unknown;
    int count = 0, i;

    (void)argc;
    (void)argv;

    for (i = 0; i < (int)SDL_arraysize(test_hints); ++i) {
        SDL_SetHintWithPriority(test_hints[i][0], test_hints[i][1], SDL_HINT_OVERRIDE);
    }
    /* SDL_SetHapticGain scales by this */
    SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "SDL_HAPTIC_GAIN_MAX");
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    wheel = OpenPort("COM250");
    silent = OpenPort("COM251");
    stinger = OpenPort("COM252");
    unknown = OpenPort("COM253");
    SDL_free(SDL_GetJoysticks(&count));
    CHECK(count == FAKE_PORTS, "%d joysticks, not one per port", count);
    if (wheel) {
        TestProperties(wheel);
    }
    if (wheel && silent && unknown) {
        TestHapticSupport(wheel, silent, unknown);
    }
    if (wheel) {
        TestFrames(wheel);
    }
    if (stinger) {
        TestStinger(stinger);
    }
    TestLayerAdmission();
    if (wheel) {
        SDL_CloseJoystick(wheel);
    }
    if (silent) {
        SDL_CloseJoystick(silent);
    }
    if (stinger) {
        SDL_CloseJoystick(stinger);
    }
    if (unknown) {
        SDL_CloseJoystick(unknown);
    }
    SDL_Quit();
    CheckPorts();

    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}
