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
#ifndef SDL_xinput_paddle_identity_h_
#define SDL_xinput_paddle_identity_h_

#include <cstdint>
#include <guiddef.h>
#include <string>

namespace sdl_paddles {

enum class PaddleTransport { None, Bluetooth, UsbGip, WirelessGip };

constexpr bool IsGipTransport(PaddleTransport transport) noexcept
{
    return transport == PaddleTransport::UsbGip || transport == PaddleTransport::WirelessGip;
}

constexpr bool IsGipPaddleHardware(std::uint16_t vendor, std::uint16_t product) noexcept
{
    // GIP Hello identities for Elite Series 1 and 2. The 0B05 and 0B22 IDs
    // describe Bluetooth presentations and do not qualify a GIP initializer.
    return vendor == 0x045e && (product == 0x02e3 || product == 0x0b00);
}

struct PhysicalIdentity {
    PaddleTransport transport = PaddleTransport::None;
    GUID container{};
    std::uint64_t nativeId = 0;
    std::wstring instance;
    // Wired identity comes from the physical controller parent. Wireless GIP
    // uses the controller child, never the receiver's VID/PID. XUSB can expose
    // Microsoft's generic 02FF product in its public slot metadata.
    std::uint16_t vendor = 0, product = 0;
};

// The caller supplies the authoritative OpenXInput interface and channel tuple.
// The path must be a readable, terminated string of fewer than 1024 characters.
// Read PnP and registry metadata only. Failure clears the identity. The error
// is best effort if memory allocation fails. Bluetooth has no native GIP ID.
// The caller owns attachment-generation checks and runtime qualification.
bool ResolvePaddleIdentity(const wchar_t *interfacePath,
                           std::uint32_t interfaceIndex,
                           std::uint32_t interfaceCount,
                           PhysicalIdentity &identity,
                           std::string &error) noexcept;

// Compare qualified snapshots, including the original HID device instance.
// A changed instance retires the attachment even if its container is reused.
bool SamePhysicalIdentity(const PhysicalIdentity &a,
                          const PhysicalIdentity &b) noexcept;

} // namespace sdl_paddles

#endif // SDL_xinput_paddle_identity_h_
