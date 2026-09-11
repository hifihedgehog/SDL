/* Adapted declarations from phnt/ntlpcapi.h, revision
 * 53fbbdc5b5d2b08761db1c7b26bfa8c820924356. Names are private to this client.
 *
 * MIT License
 *
 * Copyright (c) 2022 Winsider Seminars & Solutions, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winternl.h>
#include <cstddef>

#if !defined(_MSC_VER) || !defined(_M_X64) || defined(_M_ARM64EC)
#error This service format requires MSVC targeting native x64.
#endif

namespace sdl_paddles::nt {
#pragma pack(push, 8)
struct PortMessage {
    USHORT dataLength, totalLength, type, dataInfoOffset;
    struct { HANDLE process, thread; } clientId;
    ULONG messageId;
    SIZE_T clientViewSize;
};
struct PortAttributes {
    ULONG flags;
    SECURITY_QUALITY_OF_SERVICE securityQos;
    SIZE_T maxMessageLength, memoryBandwidth, maxPoolUsage;
    SIZE_T maxSectionSize, maxViewSize, maxTotalSectionSize;
    ULONG dupObjectTypes, reserved;
};
struct MessageAttributes { ULONG allocated, valid; };
struct ContextAttribute {
    PVOID portContext, messageContext;
    ULONG sequence, messageId, callbackId;
};
struct ViewAttribute {
    ULONG flags;
    HANDLE sectionHandle;
    PVOID base;
    SIZE_T size;
};
struct ServerSessionInformation { ULONG sessionId, processId; };
#pragma pack(pop)
static_assert(sizeof(PortMessage) == 0x28 && offsetof(PortMessage, clientViewSize) == 0x20);
static_assert(sizeof(PortAttributes) == 0x48 && offsetof(PortAttributes, maxMessageLength) == 0x10);
static_assert(sizeof(MessageAttributes) == 8 && sizeof(ViewAttribute) == 0x20);
static_assert(sizeof(ContextAttribute) == 0x20 && sizeof(ServerSessionInformation) == 8);

constexpr ULONG View = 0x40000000, Context = 0x20000000;
constexpr ULONG ViewAutoRelease = 0x20000;
constexpr USHORT Datagram = 3, LostReply = 4, PortClosed = 5, ClientDied = 6;
constexpr USHORT Continuation = 0x2000, KernelMessage = 0x8000;
constexpr LONG Success = 0, Timeout = 0x102;
constexpr LONG Unsuccessful = static_cast<LONG>(0xC0000001UL);
constexpr LONG PortDisconnected = static_cast<LONG>(0xC0000037UL);

using ConnectPort = LONG (NTAPI*)(HANDLE*, const UNICODE_STRING*, OBJECT_ATTRIBUTES*,
    PortAttributes*, ULONG, PSID, PortMessage*, SIZE_T*, MessageAttributes*, MessageAttributes*, LARGE_INTEGER*);
using DisconnectPort = LONG (NTAPI*)(HANDLE, ULONG);
using QueryInformation = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
using DeleteSectionView = LONG (NTAPI*)(HANDLE, ULONG, void*);
using SendWaitReceivePort = LONG (NTAPI*)(HANDLE, ULONG, PortMessage*, MessageAttributes*,
    PortMessage*, SIZE_T*, MessageAttributes*, LARGE_INTEGER*);
using CancelMessage = LONG (NTAPI*)(HANDLE, ULONG, ContextAttribute*);
using InitializeMessageAttribute = LONG (NTAPI*)(ULONG, MessageAttributes*, SIZE_T, SIZE_T*);
using GetMessageAttribute = void* (NTAPI*)(MessageAttributes*, ULONG);
} // namespace sdl_paddles::nt
