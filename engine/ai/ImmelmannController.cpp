// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/ImmelmannController.h"

#include "entity/EntityState.h"

namespace fl::ai {

ImmelmannController::ImmelmannController(float pullDurationS, float rollDurationS)
    : m_pullDuration(pullDurationS), m_rollDuration(rollDurationS) {}

fl::ControlInput ImmelmannController::sample(const fl::EntityState& /*state*/, uint64_t /*tick*/, double dt,
                                             const fl::SpatialIndex* /*si*/) {
    fl::ControlInput ctrl{};

    m_timer += static_cast<float>(dt);
    if (m_phase == Phase::Pull && m_timer >= m_pullDuration) {
        m_phase = Phase::Roll;
        m_timer = 0.f;
    }
    if (m_phase == Phase::Roll && m_timer >= m_rollDuration) {
        m_phase = Phase::Done;
        m_timer = 0.f;
    }

    if (m_phase == Phase::Pull) {
        ctrl.elevator = 1.f;
        ctrl.throttle = 1.f;
        ctrl.afterburner = true;
    } else if (m_phase == Phase::Roll) {
        ctrl.aileron = 1.f;
        ctrl.rudder = 0.3f;
        ctrl.throttle = 1.f;
        ctrl.afterburner = true;
    }
    // Phase::Done: ctrl remains zero-initialized (neutral).

    return ctrl;
}

} // namespace fl::ai
