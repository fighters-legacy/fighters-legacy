// SPDX-License-Identifier: GPL-3.0-or-later
#include "CongestionController.h"

#include <algorithm>
#include <cmath>

namespace fl {

void CongestionController::configure(const CongestionParams& params) noexcept {
    m_params = params;
}

void CongestionController::update(uint64_t tick, const CongestionSample& sample) noexcept {
    if (!m_params.enabled) {
        m_throttle = 1.f; // pinned to full rate + full budget while disabled
        return;
    }

    // Track the uncongested RTT baseline. Seed on the first sample, then take the running minimum so
    // the gradient (rttMs - baseline) measures the rise above the path's best observed latency. A slow
    // upward decay lets the baseline follow genuine path changes (e.g. a route change) rather than
    // pinning to an early lucky minimum forever.
    const float rtt = static_cast<float>(sample.rttMs);
    if (!m_baselineSeeded) {
        m_baselineRttMs = rtt;
        m_baselineSeeded = true;
    } else if (rtt < m_baselineRttMs) {
        m_baselineRttMs = rtt;
    } else {
        m_baselineRttMs += 0.01f * (rtt - m_baselineRttMs); // gentle drift toward current
    }

    // AIMD cadence gate (hysteresis): only step every evalIntervalTicks. The first call always steps.
    const uint32_t evalInterval = m_params.evalIntervalTicks == 0u ? 1u : m_params.evalIntervalTicks;
    if (m_haveEval && tick - m_lastEvalTick < evalInterval)
        return;
    m_haveEval = true;
    m_lastEvalTick = tick;

    // Guard the unsigned subtraction: only a positive RTT rise counts (no underflow-to-huge-value).
    const bool rttCongested =
        rtt > m_baselineRttMs && (sample.rttMs - static_cast<uint32_t>(m_baselineRttMs)) > m_params.delayMarginMs;
    const bool congested = sample.packetLoss > m_params.lossThreshold || rttCongested ||
                           sample.reliableBytesInFlight > m_params.backlogThresholdBytes;

    if (congested)
        m_throttle = std::max(m_params.throttleFloor, m_throttle * m_params.decreaseFactor);
    else
        m_throttle = std::min(1.f, m_throttle + m_params.increaseStep);
}

uint32_t CongestionController::sendIntervalTicks() const noexcept {
    if (!m_params.enabled)
        return 1u;
    const uint32_t maxInterval = m_params.maxIntervalTicks == 0u ? 1u : m_params.maxIntervalTicks;
    // throttle is clamped to [throttleFloor, 1] with throttleFloor > 0, so 1/throttle is finite; clamp
    // the float result to [1, maxInterval] BEFORE the cast so no inf/NaN ever reaches an int (UBSan).
    const float interval = std::clamp(std::ceil(1.f / m_throttle), 1.f, static_cast<float>(maxInterval));
    return static_cast<uint32_t>(interval);
}

uint32_t CongestionController::effectiveBudget(uint32_t staticBudget) const noexcept {
    if (!m_params.enabled || staticBudget == 0u)
        return staticBudget; // disabled => unchanged; 0 (unlimited) => still unlimited (rate lever only)
    // Scale down by throttle (throttle <= 1, so the scaled value never exceeds the static budget), then
    // clamp up to the floor. The floor is itself capped at the static budget so a budget already below
    // the floor is never RAISED — at full throttle this returns the static budget unchanged.
    const uint32_t scaled = static_cast<uint32_t>(std::round(static_cast<float>(staticBudget) * m_throttle));
    const uint32_t floor = std::min(staticBudget, m_params.budgetFloorBytes);
    return std::max(floor, scaled);
}

} // namespace fl
