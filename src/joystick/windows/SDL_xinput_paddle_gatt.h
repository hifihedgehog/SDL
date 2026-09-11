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
#ifndef SDL_xinput_paddle_gatt_h_
#define SDL_xinput_paddle_gatt_h_

#include <array>
#include <atomic>
#include <cstdint>
#include <guiddef.h>
#include <memory>
#include <vector>

namespace sdl_paddles {

struct GattPaddlePacket {
    std::uint64_t attachmentGeneration = 0;
    std::array<std::uint8_t, 17> bytes{};
    std::int64_t receivedQpc = 0;
    // Decode bytes only when hasPayload is true. Byte 16 remains opaque.
    bool hasPayload = false;
    // Consume preceding packets, release cached edges, then consume this
    // snapshot if hasPayload. A gap without a payload still requires release.
    bool gap = false;
    // A retired marker has no payload. Its bytes and receivedQpc are zero.
    bool retired = false;
};

enum class GattPaddlePhase {
    Idle, Discover, Open, Verify, Services, ServicesUncached,
    Characteristics, CharacteristicsUncached,
    ReadOriginal, WriteNotify, Streaming, ReadRestore, WriteRestore, Done
};

enum class GattPaddleErrorCode {
    None, InvalidIdentity, WrongThread, WrongApartment, InstanceLimit,
    AlreadyStarted, IdentityUnavailable, IdentityMismatch, AmbiguousIdentity,
    NotConnected, Unsupported, OperationFailed, Timeout, CleanupFailed,
    OutOfMemory
};

struct GattPaddleError {
    GattPaddleErrorCode code = GattPaddleErrorCode::None;
    GattPaddlePhase phase = GattPaddlePhase::Idle;
    std::int32_t hresult = 0;
    // Cleanup diagnostics never replace the initiating failure above.
    GattPaddleErrorCode cleanupCode = GattPaddleErrorCode::None;
    GattPaddlePhase cleanupPhase = GattPaddlePhase::Idle;
    std::int32_t cleanupHresult = 0;
    bool cleanupPending = false;
};

// All methods, including destruction and PumpRetired, belong to one broker MTA
// thread. Initialize that apartment externally. No SDL objects are used here.
// Pin the containing module before Start: Windows can retain delegates after
// revocation. Constructors allocate nothing and perform no device operations.
// Retire on another thread only closes the atomic delivery gate and requests
// owner cleanup. Wrong-thread destruction transfers the state to that owner.
// Object lifetime and all other method calls must still be serialized.
class GattPaddleClient final {
public:
    GattPaddleClient(GUID windowsContainerId, std::uint64_t attachmentGeneration) noexcept;
    ~GattPaddleClient();
    GattPaddleClient(const GattPaddleClient&) = delete;
    GattPaddleClient& operator=(const GattPaddleClient&) = delete;
    GattPaddleClient(GattPaddleClient&&) = delete;
    GattPaddleClient& operator=(GattPaddleClient&&) = delete;

    // Start admits one attempt. Pump starts/polls at most one async phase per
    // call and never waits for completion. False reports an error or retirement,
    // but Pump must continue until Finished. Errors remain available on Pump.
    // A Status query error retires delivery immediately while retaining the
    // operation for terminal polling. Retirement discards all queued packets
    // and delivers a release-only marker, even for edges received earlier.
    bool Start(GattPaddleError& error) noexcept;
    bool Pump(std::vector<GattPaddlePacket>& packets, GattPaddleError& error) noexcept;
    void Retire() noexcept;
    bool Finished() const noexcept;
    std::uint64_t BluetoothAddress() const noexcept;

    // Prefer Retire + Pump until Finished before destruction. Early destruction
    // transfers cleanup to a bounded orphan slot. Call this on the SAME owner
    // MTA thread until it returns zero, even after all public clients are gone.
    // Stuck operations stay retained. The process admits at most 16 live gates,
    // including orphan states and delegates that Windows has not released.
    // The broker's owner thread must persist until process exit. Check error
    // as well as the count: an apartment error returns zero without pumping.
    // If several orphans report errors, error contains the last one visited.
    static unsigned PumpRetired(GattPaddleError& error) noexcept;

private:
#ifdef SDL_XINPUT_PADDLE_GATT_TESTING
    friend struct GattPaddleTestAccess;
#endif
    struct Impl;
    GUID container_{};
    std::uint64_t generation_ = 0;
    unsigned long ownerThread_ = 0;
    bool attempted_ = false;
    std::atomic<bool> retired_{false};
    std::unique_ptr<Impl> impl_;
    // Only this published address is read by a foreign retirement request.
    // Destruction remains serialized with every method call.
    std::atomic<Impl*> published_{nullptr};
};

} // namespace sdl_paddles
#endif
