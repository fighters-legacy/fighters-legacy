// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Server-wide graceful tick-overrun governor (#514, Epic A). Pure AIMD policy, isolated from
// WorldBroadcaster the same way CongestionController / SnapshotScheduler / SnapshotCodec / JitterBuffer
// are — no glm, no engine-entity deps, fully unit-testable in isolation.
//
// When the authoritative 60 Hz tick can no longer complete inside its fixed-step budget (~16.667 ms at
// 60 Hz), the server must shed work and degrade gracefully rather than spiral or silently dilate time.
// The governor maintains a single loadFactor in [floor, 1] (1 = no degradation, floor = maximum shed)
// driven by an EWMA of the measured per-tick wall-time vs the budget, and exposes FOUR composing
// levers the broadcaster folds in on top of the per-client CongestionController:
//   * snapshotIntervalTicks() — server-wide send-rate decimation (max(perPeer, governor));
//   * effectiveBudget()       — shrink the per-client snapshot byte budget (the #516 scheduler then
//                               defers more low-priority entities; no new encode-loop logic needed);
//   * aiSampleStride()        — decimate AI sample() for non-player entities (the only lever that cuts
//                               the AI phase; the integrate pass is never decimated — fixed-dt stability);
//   * interestScale()         — shrink each peer's effective interest radius toward a configured floor
//                               (#726; ladder item 3 of the #572 spike). Unlike the budget lever, which
//                               trims the encoded output AFTER ranking the full visible set, this cuts
//                               the clients x visible-entities cost at its source (interest query +
//                               scheduler ranking + encode all shrink together).
// A healthy server (or a disabled governor, or one that never overruns) holds loadFactor == 1, i.e. the
// exact pre-#514 behaviour: a snapshot every tick at the full budget, every entity sampled every tick.
//
// Signal note: the control signal is an EWMA of the per-tick total wall-time — NOT the TickProfiler's
// 60 s p99, which is far too laggy to recover from. The EWMA reacts in ~10 ticks and recovers
// symmetrically (same rationale as CongestionController's RTT baseline). p99 stays an operator-facing
// display metric only.
//
// loadFactor float math is NOT claimed bit-identical across compilers/ISAs; the governor is
// server-authoritative policy/telemetry and is disabled in the serial-equivalence determinism tests.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fl {

// Operator-facing knobs (the first seven map to [world] config) plus internal AIMD constants.
struct TickGovernorParams {
    bool enabled{true};                   // false => no-op (loadFactor pinned to 1)
    float floor{0.25f};                   // min loadFactor (max shed)
    float highWatermark{0.90f};           // ewmaTickMs > budget*high => overrun (shed)
    float lowWatermark{0.60f};            // ewmaTickMs < budget*low  => healthy (recover)
    float ewmaAlpha{0.1f};                // EWMA smoothing of per-tick wall-ms (~10-tick window)
    float decreaseFactor{0.7f};           // multiplicative loadFactor back-off per eval when overrun
    float increaseStep{0.125f};           // additive loadFactor ramp-up per eval when healthy
    uint32_t evalIntervalTicks{6};        // AIMD step cadence in ticks (hysteresis between steps)
    uint32_t maxSnapshotIntervalTicks{4}; // hard cap on send-rate decimation (=> 15 Hz floor at 60 Hz)
    uint32_t maxAiStride{4};              // hard cap on AI-sample decimation
    uint32_t budgetFloorBytes{400};       // never scale a set snapshot budget below this
    float minInterestFraction{0.5f};      // floor on interestScale(); 1 = interest lever disabled (#726)
};

// Server-wide AIMD overrun controller. Sim-thread only (no internal synchronization).
class TickGovernor {
  public:
    // Cheap value copy of the params; safe to call every tick (drives reload_config hot-reload).
    void configure(const TickGovernorParams& params) noexcept;

    // Steps the controller from the previous tick's measured wall-time (tickMs) against the fixed-step
    // budget (budgetMs). The EWMA updates every call; the AIMD loadFactor steps only every
    // evalIntervalTicks (built-in hysteresis). Between steps loadFactor — and therefore all three
    // levers — is held constant.
    void update(uint64_t tick, double tickMs, double budgetMs) noexcept;

    // Ticks between snapshots, server-wide: 1 (full 60 Hz) when healthy or disabled, rising to
    // maxSnapshotIntervalTicks at the floor. Always >= 1.
    uint32_t snapshotIntervalTicks() const noexcept;

    // The per-snapshot byte budget after overrun scaling. staticBudget == 0 (unlimited) stays 0 (only
    // the send-rate lever applies); disabled returns staticBudget unchanged. Otherwise scaled by
    // loadFactor and clamped up to min(staticBudget, budgetFloorBytes) — a healthy server (loadFactor 1)
    // gets staticBudget exactly, and a budget already below the floor is never raised.
    uint32_t effectiveBudget(uint32_t staticBudget) const noexcept;

    // AI sample() decimation stride for non-player entities: 1 (every tick) when healthy or disabled,
    // rising to maxAiStride at the floor. Always >= 1. A non-peer entity with pool index `idx` samples
    // on tick T when (T + idx) % stride == 0 — a pure function of (idx, T, stride) so it is
    // serial-equivalent across worker counts.
    uint32_t aiSampleStride() const noexcept;

    // Interest-radius scale in [minInterestFraction, 1] (#726): 1 (full radius) when healthy or
    // disabled, tracking loadFactor down until it clamps at minInterestFraction. The broadcaster
    // multiplies each peer's interest radius (draw distance) by this — BOTH the SpatialIndex
    // queryRadius bound and the exact XYZ distance gate — shrinking the visible set at its source.
    // A pure function of loadFactor, uniform across peers, frozen once per tick before the parallel
    // peer pass, so it preserves serial-equivalence by construction. Entities leaving the shrunk
    // radius are ordinary interest-out (client retention + kSnapshotRetentionTicks force-full handle
    // re-entry) — never despawned, no wire change. minInterestFraction == 1 pins the lever off.
    float interestScale() const noexcept;

    // Current loadFactor in [floor, 1] — diagnostics / observability (OverrunStatus, status command).
    float loadFactor() const noexcept {
        return m_loadFactor;
    }

    // True when the governor is actively shedding (loadFactor < 1).
    bool degraded() const noexcept {
        return m_loadFactor < 1.f;
    }

  private:
    TickGovernorParams m_params{};
    float m_loadFactor{1.f}; // [floor, 1]; 1 = full rate + full budget + every-tick AI
    float m_ewmaMs{0.f};     // EWMA of measured per-tick wall-time (ms)
    bool m_ewmaSeeded{false};
    bool m_haveEval{false}; // false until the first AIMD step (so the cadence gate starts clean)
    uint64_t m_lastEvalTick{0};
};

// Build TickGovernorParams from the operator-facing config scalars. minSnapshotHz (the floor send rate
// under overrun) maps to the loadFactor floor and the send-rate cap the same way CongestionController's
// minSendHz does: floor = minSnapshotHz/simHz and maxSnapshotIntervalTicks = round(simHz/minSnapshotHz),
// so at the loadFactor floor the send-rate lever bottoms out at exactly minSnapshotHz.
// minInterestFraction is the interest-radius lever floor (#726; 1 = lever disabled). The remaining
// AIMD constants keep their defaults. Shared by startup config and the reload_config path.
inline TickGovernorParams makeTickGovernorParams(bool enabled, float highWatermark, float lowWatermark,
                                                 float minSnapshotHz, uint32_t maxAiStride, uint32_t budgetFloorBytes,
                                                 float minInterestFraction = 0.5f, float simHz = 60.f) noexcept {
    TickGovernorParams p;
    p.enabled = enabled;
    p.highWatermark = highWatermark;
    p.lowWatermark = lowWatermark;
    minSnapshotHz = std::clamp(minSnapshotHz, 1.f, simHz);
    p.floor = minSnapshotHz / simHz;
    const long interval = std::lround(simHz / minSnapshotHz);
    p.maxSnapshotIntervalTicks = interval < 1 ? 1u : static_cast<uint32_t>(interval);
    p.maxAiStride = maxAiStride < 1u ? 1u : maxAiStride;
    p.budgetFloorBytes = budgetFloorBytes;
    p.minInterestFraction = std::clamp(minInterestFraction, 0.f, 1.f);
    return p;
}

} // namespace fl
