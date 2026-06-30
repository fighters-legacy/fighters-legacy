// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Per-client adaptive send-rate / congestion response (#518). Pure AIMD policy, isolated from
// WorldBroadcaster the same way SnapshotScheduler / SnapshotCodec / JitterBuffer / AuthTracker are —
// no glm, no engine-entity deps, fully unit-testable in isolation.
//
// Each connected peer owns one controller. The broadcaster feeds it a CongestionSample (ENet packet
// loss + RTT + reliable backlog) on the AIMD eval cadence; the controller maintains a single
// throttle in [throttleFloor, 1] that drives TWO levers under congestion:
//   * send-rate decimation — sendIntervalTicks() rises (60 Hz -> down to ~10 Hz at the floor), and
//   * byte-budget scaling — effectiveBudget() shrinks the per-client snapshot budget (the #516
//     scheduler then defers more low-priority entities; no new encode-loop logic needed).
// A healthy peer (or any peer when disabled / all-zero samples) holds throttle == 1, i.e. the exact
// current behaviour: a snapshot every tick at the full configured budget.
//
// Signal note (anti-feedback): the delay term reads ENet RTT, NOT the application's snapshot one-way
// delay (PeerInputState::ewmaDelayTicks). That metric inflates when we decimate — a staler "last
// received tick" looks like more delay — which would drive more decimation: a self-reinforcing
// collapse. ENet keeps RTT fresh via periodic reliable pings, independent of our snapshot cadence.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fl {

// Operator-facing knobs (the first four are wired to [world] config) plus internal AIMD constants.
struct CongestionParams {
    bool enabled{true};                    // false => controller is a no-op (throttle pinned to 1)
    float throttleFloor{1.0f / 6.0f};      // min throttle; 1/6 => 10 Hz floor at a 60 Hz sim
    uint32_t maxIntervalTicks{6};          // hard cap on decimation (=> 10 Hz floor at 60 Hz)
    uint32_t budgetFloorBytes{400};        // never scale a set byte budget below this
    float lossThreshold{0.02f};            // ENet mean loss fraction above which a peer is congested
    uint32_t delayMarginMs{40};            // ENet-RTT rise over the running baseline => congested
    uint32_t backlogThresholdBytes{65536}; // reliableBytesInFlight above which a peer is congested
    float increaseStep{0.125f};            // additive throttle ramp-up per eval when healthy
    float decreaseFactor{0.7f};            // multiplicative throttle back-off per eval when congested
    uint32_t evalIntervalTicks{6};         // AIMD step cadence in ticks (hysteresis between steps)
};

// One per-peer congestion observation, assembled by WorldBroadcaster from INetwork::getPeerLinkStats.
struct CongestionSample {
    float packetLoss{0.f};             // 0..1 ENet mean loss — primary congestion trigger
    uint32_t rttMs{0};                 // ENet mean RTT — cadence-independent delay-gradient source
    uint32_t reliableBytesInFlight{0}; // ENet reliable data awaiting ack — backlog trigger
};

// Per-peer AIMD controller. Sim-thread only (no internal synchronization).
class CongestionController {
  public:
    // Cheap value copy of the params; safe to call every tick (drives reload_config hot-reload).
    void configure(const CongestionParams& params) noexcept;

    // Steps the AIMD controller, but only when at least evalIntervalTicks have elapsed since the last
    // step (built-in hysteresis). Between steps the throttle — and therefore the send interval and
    // effective budget — is held constant.
    void update(uint64_t tick, const CongestionSample& sample) noexcept;

    // Ticks between snapshots for this peer: 1 (full 60 Hz) when healthy or disabled, rising to
    // maxIntervalTicks at the throttle floor. Always >= 1.
    uint32_t sendIntervalTicks() const noexcept;

    // The per-snapshot byte budget after congestion scaling. staticBudget == 0 (unlimited) stays 0
    // (only the send-rate lever applies); disabled returns staticBudget unchanged. Otherwise the budget
    // is scaled by throttle and clamped up to min(staticBudget, budgetFloorBytes) — so a healthy peer
    // (throttle 1) gets staticBudget exactly, and a budget already below the floor is never raised.
    uint32_t effectiveBudget(uint32_t staticBudget) const noexcept;

    // Current throttle in [throttleFloor, 1] — diagnostics / observability (PeerInfo, peers command).
    float throttle() const noexcept {
        return m_throttle;
    }

  private:
    CongestionParams m_params{};
    float m_throttle{1.f};      // [throttleFloor, 1]; 1 = full rate + full budget
    float m_baselineRttMs{0.f}; // running minimum RTT (the uncongested reference)
    bool m_baselineSeeded{false};
    bool m_haveEval{false}; // false until the first AIMD step (so the cadence gate starts clean)
    uint64_t m_lastEvalTick{0};
};

// Build CongestionParams from the operator-facing config scalars. minSendHz (the floor send rate
// under congestion) is the inverse of the controller's interval math: throttleFloor = minSendHz/simHz
// and maxIntervalTicks = round(simHz/minSendHz). The remaining AIMD constants keep their defaults.
inline CongestionParams makeCongestionParams(bool enabled, float minSendHz, float lossThreshold,
                                             uint32_t budgetFloorBytes, float simHz = 60.f) noexcept {
    CongestionParams p;
    p.enabled = enabled;
    minSendHz = std::clamp(minSendHz, 1.f, simHz);
    p.throttleFloor = minSendHz / simHz;
    const long interval = std::lround(simHz / minSendHz);
    p.maxIntervalTicks = interval < 1 ? 1u : static_cast<uint32_t>(interval);
    p.lossThreshold = lossThreshold;
    p.budgetFloorBytes = budgetFloorBytes;
    return p;
}

} // namespace fl
