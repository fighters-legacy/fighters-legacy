// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <IClock.h>

#include <chrono>

namespace fl {

// How long this server has been running -- and the ONE place that answer comes from (#1048).
//
// It used to come from two. The `status` command subtracted a `steady_clock::time_point` carried in
// the admin command context; the REST `/health` route subtracted one the HTTP server captured for
// itself. Two sources means two answers, and that is what shipped: the context's field was still
// value-initialised (the clock's epoch) when every command handler copied the context, so `status`
// computed `now() - epoch` -- on Linux, where steady_clock is CLOCK_MONOTONIC, the MACHINE's uptime.
// It advanced at the right rate and was plausible enough to be believed.
//
// Two properties are load-bearing here, and both exist because of that bug:
//   * it CANNOT hold the epoch. The start instant is captured at construction, so there is no
//     default-then-assign window for a reader to fall into; and
//   * one instance is constructed per process and handed to every frontend, so no two of them can
//     disagree about when the server started.
//
// Copyable by value on purpose: a copy carries the same start instant, so passing one around cannot
// fork the answer the way two independently captured instants did.
class ServerUptime {
  public:
    // Starts the clock now, on the real clock. The production case: one at the top of main().
    ServerUptime() noexcept = default;

    // Starts the clock now, on an injected clock -- a test advances a ManualClock instead of sleeping.
    explicit ServerUptime(const IClock& clock) noexcept : m_clock(&clock), m_startedAt(clock.now()) {}

    // An explicit start instant, for a test that wants a server which has already been up a while.
    ServerUptime(std::chrono::steady_clock::time_point startedAt, const IClock& clock) noexcept
        : m_clock(&clock), m_startedAt(startedAt) {}

    [[nodiscard]] std::chrono::steady_clock::time_point startedAt() const noexcept {
        return m_startedAt;
    }

    [[nodiscard]] std::chrono::seconds elapsed() const noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(m_clock->now() - m_startedAt);
    }

    // Whole seconds, for the text and JSON surfaces that carry a plain integer.
    [[nodiscard]] long long seconds() const noexcept {
        return static_cast<long long>(elapsed().count());
    }

  private:
    const IClock* m_clock{&SystemClock::instance()};
    std::chrono::steady_clock::time_point m_startedAt{SystemClock::instance().now()};
};

} // namespace fl
