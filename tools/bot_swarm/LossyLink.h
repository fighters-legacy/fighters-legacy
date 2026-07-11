// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// LossyLink — the pure per-datagram drop/delay policy behind the bot_swarm lossy proxy (#714).
//
// A schedule describes one degraded window inside the run: clean until degradeStartS, then
// lossFraction drops + delayMs added one-way delay for degradeDurationS, then clean again. The
// congestion gate uses the clean tail to assert the server's per-peer AIMD controller (#518)
// RECOVERS after it engaged. Pure logic (deterministic seeded RNG, caller-supplied clock) so the
// drop/delay decisions unit-test without sockets; the socket relay lives in LossyProxy.

#include <cstdint>
#include <optional>
#include <random>

namespace fl {

struct LossySchedule {
    double degradeStartS{0.0};    // seconds into the run when the degraded window opens
    double degradeDurationS{0.0}; // window length; <= 0 = never degrade (policy is a no-op)
    float lossFraction{0.f};      // drop probability while degraded [0, 1]
    uint32_t delayMs{0};          // added one-way delay while degraded

    bool enabled() const {
        return degradeDurationS > 0.0 && (lossFraction > 0.f || delayMs > 0);
    }
};

class LossyLink {
  public:
    explicit LossyLink(const LossySchedule& sched, uint32_t seed = 1u) : m_sched(sched), m_rng(seed ? seed : 1u) {}

    bool degradedAt(double nowS) const {
        return m_sched.enabled() && nowS >= m_sched.degradeStartS &&
               nowS < m_sched.degradeStartS + m_sched.degradeDurationS;
    }

    // Per-datagram decision at time nowS: nullopt = drop the datagram; otherwise the added
    // one-way delay in ms (0 outside the degraded window). The RNG advances only for datagrams
    // inside the window, so the clean phases are byte-identical to no proxy at all.
    std::optional<uint32_t> classify(double nowS) {
        if (!degradedAt(nowS))
            return 0u;
        if (m_sched.lossFraction > 0.f) {
            std::uniform_real_distribution<float> dist(0.f, 1.f);
            if (dist(m_rng) < m_sched.lossFraction)
                return std::nullopt;
        }
        return m_sched.delayMs;
    }

    const LossySchedule& schedule() const {
        return m_sched;
    }

  private:
    LossySchedule m_sched;
    std::mt19937 m_rng;
};

} // namespace fl
