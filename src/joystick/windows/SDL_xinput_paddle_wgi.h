/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
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

#include <Windows.h>
#include <cstdint>

namespace sdl_paddles {

enum class GipEnableStatus : std::uint8_t {
    Pending, Success, Failed, TimedOut, Retired, Exhausted
};

struct GipEnableResult {
    GipEnableStatus status = GipEnableStatus::Failed;
    HRESULT hresult = E_INVALIDARG;
    std::uint64_t provider = 0, epoch = 0, attempt = 0;
    bool dispatched = false;
};

struct GipInputState {
    std::uint64_t provider = 0, epoch = 0;
    std::uint64_t resumeTime = 0, normalTime = 0, splitTime = 0;
    bool resumed = false, normalSeen = false, splitSeen = false, busy = false;
    // This provider has been Ready() at least once in its current input epoch.
    // A suspend keeps it and a resume clears it. A re-send needs only this:
    // WGI accepts the command from a suspended or background provider, and
    // the driver's focus quiesce is what the re-send undoes.
    bool everReady = false;
    HRESULT error = S_OK;
    bool Ready() const noexcept { return provider && epoch && resumed && normalSeen && SUCCEEDED(error); }
    bool ResendReady() const noexcept { return provider && epoch && everReady && SUCCEEDED(error); }
};

// The caller must qualify the WGI/service images and exact physical association
// before starting observation. This starts the bounded worker pool and factory.
// State comes from that provider's own callbacks. False means startup failure
// or lock contention. A missing provider leaves provider=0. Native callback
// timestamps are diagnostic source times. This API publishes no SDL input.
bool QueryGipInputState(std::uint64_t nativeId, GipInputState &state) noexcept;

// Private x64 contract. nativeId is the nonzero 64-bit GIP device ID rendered
// by public GameControllerProviderInfo.GetProviderId as GIP: plus 16 hex digits.
// Before submission, the caller must qualify the WGI/service images, prove the
// exact PnP-slot/SIPC/WGI association in that ID namespace, and obtain Ready()
// from the command provider's own input epoch. A service report alone is not
// this readiness observation. An absent, suspended, changed, or busy provider
// cannot admit a new command. Submission and dispatch recheck the epoch.
// The private aggregation Initialize ABI still requires the qualified WGI image.
// Generations must be nonzero, unique across attachments, and never reused or
// submitted after retirement. This module does not establish paddle capability.
//
// Starting the broker pins this module for process lifetime, even if submission
// or registration fails afterward. One factory, 4 workers, 16 catalog cells, and
// 20 request cells are retained on the heap. At most 16 requests can be queued.
// A 10-second deadline starts at submission. Timeout/retirement cannot cancel
// a native call. Its worker, buffers, and provider lease remain until return.
// No replacement workers are created. Initialization can consume a worker too.
//
// A dispatched terminal result ends this (attachment, provider, input epoch,
// attempt) request. Repeated submissions of the same key return its retained
// token. An undispatched timeout or exhausted request can be admitted again
// once its worker is free. Do not resubmit a dispatched key after token
// eviction. A new resume permits attempt 1 again after its own normal input.
// The caller numbers attempts from 1 within one (provider, input epoch) pair.
// A later attempt in the same pair is a deliberate re-send: the driver quiesces
// every controller on a foreground process change and the Elite then stops its
// separate paddle report. An elevated client gets no suspend or resume for
// that switch. A background client can be suspended by it. A re-send is
// admitted, dispatched, and completed while suspended, as long as the provider
// was Ready() once in its current input epoch. A resume starts a new epoch,
// retires queued re-sends, and requires attempt 1 after its own normal frame
// before any further re-send.
// In-flight calls retain their buffers and provider lease, and prevent another
// dispatch to that provider until return. A successful HRESULT does not
// establish that split input was received.
//
// Zero means no request was admitted. Readiness changes, contention, invalid
// input, startup failure, and a full queue can produce this result.
std::uint64_t SubmitGipEnable(std::uint64_t nativeId,
                              std::uint64_t attachmentGeneration,
                              std::uint64_t expectedProvider,
                              std::uint64_t expectedEpoch,
                              std::uint64_t attempt) noexcept;

// Nonconsuming lookup. Completed cells can be reused by later submissions.
// False means invalid/expired token or brief lock contention. Retry a known
// outstanding token. Terminal cells with unfinished calls cannot be reused.
bool PollGipEnable(std::uint64_t token, GipEnableResult& result) noexcept;

// Invalidates this attachment's queued work and completions. Catalog discovery
// remains registered. Missing WGI removal notifications can retain all 16
// catalog cells and block later admission. No command, callback, or worker is
// joined, and retirement does not release an unfinished call's provider lease.
void RetireGipEnable(std::uint64_t attachmentGeneration) noexcept;

} // namespace sdl_paddles
