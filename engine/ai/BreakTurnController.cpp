// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/BreakTurnController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

namespace fl::ai {

BreakTurnController::BreakTurnController(const fl::EntityManager& entityManager, fl::EntityId threatId,
                                         float rollPhaseDurationS, float maxElevator)
    : m_entityManager(entityManager), m_threatId(threatId), m_rollPhaseDuration(rollPhaseDurationS),
      m_maxElevator(maxElevator) {}

fl::ControlInput BreakTurnController::sample(const fl::EntityState& state, uint64_t /*tick*/, double dt) {
    fl::ControlInput ctrl{};

    const fl::EntityState* threat = m_entityManager.get(m_threatId);
    if (!threat || threat->dead)
        return ctrl;

    if (m_phase == Phase::Roll) {
        m_rollTimer += static_cast<float>(dt);
        if (m_rollTimer >= m_rollPhaseDuration)
            m_phase = Phase::Pull;
    }

    if (m_phase == Phase::Roll) {
        const double threatPos[3] = {
            threat->transform.pos[0],
            threat->transform.pos[1],
            threat->transform.pos[2],
        };
        // Bank toward the threat to orient the lift vector for the maximum-G pull.
        float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, threatPos);
        ctrl.aileron = bankToTurnAileron(headErr);
        ctrl.rudder = coordinatedRudder(ctrl.aileron);
        ctrl.throttle = 1.f;
        ctrl.afterburner = true;
    } else {
        // Pull phase: maximum-G elevator, hold current bank.
        ctrl.elevator = m_maxElevator;
        ctrl.throttle = 1.f;
        ctrl.afterburner = true;
    }

    return ctrl;
}

} // namespace fl::ai
