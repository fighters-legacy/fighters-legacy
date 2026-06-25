// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/HighYoYoController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

namespace fl::ai {

HighYoYoController::HighYoYoController(const fl::EntityManager& entityManager, fl::EntityId targetId,
                                       float climbDurationS, float reacquireDurationS)
    : m_entityManager(entityManager), m_targetId(targetId), m_climbDuration(climbDurationS),
      m_reacquireDuration(reacquireDurationS) {}

fl::ControlInput HighYoYoController::sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
                                            const fl::SpatialIndex* /*si*/) {
    fl::ControlInput ctrl{};

    const fl::EntityState* target = m_entityManager.get(m_targetId);
    if (!target || target->dead)
        return ctrl;

    m_timer += static_cast<float>(dt);
    if (m_phase == Phase::Climb && m_timer >= m_climbDuration) {
        m_phase = Phase::Reacquire;
        m_timer = 0.f;
    }
    if (m_phase == Phase::Reacquire && m_timer >= m_reacquireDuration) {
        m_phase = Phase::Done;
        m_timer = 0.f;
    }

    if (m_phase == Phase::Climb) {
        const double tgtPos[3] = {target->transform.pos[0], target->transform.pos[1], target->transform.pos[2]};
        float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, tgtPos);
        // Bank away from target at half authority; pull hard up to bleed speed and gain altitude.
        ctrl.aileron = -bankToTurnAileron(headErr) * 0.5f;
        ctrl.rudder = coordinatedRudder(ctrl.aileron);
        ctrl.elevator = 1.f;
        ctrl.throttle = 0.7f;
    } else if (m_phase == Phase::Reacquire) {
        const double tgtPos[3] = {target->transform.pos[0], target->transform.pos[1], target->transform.pos[2]};
        float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, tgtPos);
        // Roll back toward target and ease in with a moderate elevator pull.
        ctrl.aileron = bankToTurnAileron(headErr);
        ctrl.rudder = coordinatedRudder(ctrl.aileron);
        ctrl.elevator = 0.5f;
        ctrl.throttle = 1.f;
        ctrl.afterburner = true;
    }
    // Phase::Done: ctrl remains zero-initialized (neutral).

    return ctrl;
}

} // namespace fl::ai
