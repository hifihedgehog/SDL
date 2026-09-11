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
#pragma once

#include "SDL_xinput_paddle_reader.h"

namespace sdl_paddles {

struct ServiceDevice {
    std::uint64_t nativeId = 0;
    std::uint64_t viewGeneration = 0;
    std::uint16_t vendor = 0, product = 0;
    std::array<std::uint8_t, 32> sectionId{};
};

struct ServicePacket {
    std::uint64_t nativeId = 0, viewGeneration = 0;
    RawReading reading;
    bool hasReading = false, retired = false, gap = false, bootstrap = false;
    std::string reason;
};

struct ServiceQueryDiagnostics {
    bool attempted = false;
    std::int32_t status = 0;
    std::uint32_t returnedBytes = 0, serverSession = 0, serverPid = 0;
};

struct ServiceViewDiagnostics {
    std::uint64_t received = 0, lastBytes = 0;
    std::uint32_t lastFlags = 0, flagsOr = 0;
};

/* One process broker owns this client. Every call, including destruction, runs
 * on its single owner worker. The class creates no threads and calls no SDL API.
 * The caller qualifies the installed format before Connect and applies runtime
 * peer qualification and exact device association before Select. ServerPid and
 * ServerQuery return connection metadata, not resident-image attestation. Process
 * inspection can be denied. The caller keeps an unqualified route inactive.
 */
class Client final {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool Connect(std::string& error); /* Metadata only, no policy write or Poll. */
    std::uint32_t ServerPid() const noexcept;
    ServiceQueryDiagnostics ServerQuery() const noexcept; /* Last attempt, retained after disconnect. */
    ServiceViewDiagnostics ViewDiagnostics() const noexcept;
    std::vector<ServiceDevice> Devices() const; /* Owned catalog copy. */

    /* Select requires a unique native ID and its exact current view generation.
     * Bootstrap drains available history before policy activation and returns
     * only the latest sample of each report ID, ordered by reader generation.
     * Collapse bootstrap into one decoder state, without executing old actions.
     * Deselect emits retirement here and drops the logical reader. Policy stays
     * zero until Disconnect because a restoration protocol is not established.
     * Selecting an already selected generation succeeds with an empty batch.
     */
    bool Select(std::uint64_t nativeId, std::uint64_t viewGeneration, bool enabled,
                std::vector<ServicePacket>& bootstrapPackets, std::string& error);

    /* Immediate receives are bounded at 1024. Each selected reader returns all
     * new owned edges in ascending generation order. Gap events precede data.
     * A fatal reader retires only that view. Peer/transport failure returns false
     * with selected retirements. On any false result the broker must clear all
     * attached sources even if allocation failure prevented retirement output.
     * Unselected and removed mappings remain bounded and owned until replacement
     * or Disconnect. No normal input, descriptors, or commands qualify paddles.
     */
    bool Pump(std::vector<ServicePacket>& packets, std::string& error);
    void Disconnect() noexcept; /* The broker retires its routes before calling. */
    // Counts failed cleanup API calls and incomplete drains. It does not identify
    // a leak or establish the cause of a failure. Peer loss has no assumed status.
    std::uint32_t CleanupFailures() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
#ifdef SDL_XINPUT_PADDLE_SERVICE_TESTING
    friend struct ServiceTestAccess;
#endif
};

} // namespace sdl_paddles
