// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace fl {

// The per-peer 1-second rate-limit window, in one place (#1264).
//
// Eight channels hand-rolled this: wingman orders, radio commands, the unauthenticated admin path,
// chat, seat requests, heartbeats, voice frames and the ClientInput flood detector. Every copy was
// the same three statements -- roll the window over when a second has elapsed, count the packet,
// compare against the channel's limit -- and each copy was a place the rollover could be forgotten.
//
// What the copies did NOT share is what happens once the budget is spent, and that difference is
// deliberate per channel. Three shapes exist and all three are expressible here:
//
//   silent drop          if (!w.allow(now, limit)) return;
//   warn/ack once        if (!w.allow(now, limit)) { if (!w.warned) { w.warned = true; reply(); } return; }
//   account, suppress    if (w.allow(now, limit)) reply();      // the packet still counts either way
//
// `warned` is cleared by the rollover, which is what makes "once per window" mean once per window
// rather than once per peer. Channels that never warn simply leave it alone.
//
// Not a cooldown. The team-switch limiter (PeerInputState::lastTeamRequest) is deliberately "how
// often may a player change teams", not "how many requests per second", and must NOT adopt this.
struct RateWindow {
    std::chrono::steady_clock::time_point start{};
    uint32_t count{0};
    bool warned{false}; // one reply per window (chat notice, wingman RateLimited ack)

    // Count one request and report whether it fits the budget. Rolls the window over first, so the
    // request that opens a new window is counted in that window rather than the expired one.
    // Returns true when the request is WITHIN `limit` -- callers that drop over the limit negate it.
    [[nodiscard]] bool allow(std::chrono::steady_clock::time_point now, uint32_t limit) noexcept {
        if (now - start >= std::chrono::seconds(1)) {
            start = now;
            count = 0;
            warned = false;
        }
        ++count;
        return count <= limit;
    }
};

} // namespace fl
