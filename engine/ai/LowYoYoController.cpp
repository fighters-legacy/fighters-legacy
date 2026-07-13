// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/LowYoYoController.h"

#include "ai/TargetView.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

namespace fl::ai {

LowYoYoController::LowYoYoController(const fl::EntityManager& entityManager, fl::EntityId targetId, float diveDurationS,
                                     float pullDurationS)
    : m_entityManager(entityManager), m_targetId(targetId), m_diveDuration(diveDurationS),
      m_pullDuration(pullDurationS) {}

fl::ControlInput LowYoYoController::sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
                                           const fl::AiTickContext& ctx) {
    fl::ControlInput ctrl{};

    // Honest targeting (#690): the target must be a CONTACT when sensing ran — a controller does
    // not chase what its entity cannot see. A coasting contact returns LAST-KNOWN state (steering at
    // a memory is what a coast is for); a dropped one is treated exactly like a dead target.
    const TargetView tv = resolveTarget(m_entityManager, ctx, m_targetId);
    if (!tv.valid)
        return ctrl;

    m_timer += static_cast<float>(dt);
    if (m_phase == Phase::Dive && m_timer >= m_diveDuration) {
        m_phase = Phase::Pull;
        m_timer = 0.f;
    }
    if (m_phase == Phase::Pull && m_timer >= m_pullDuration) {
        m_phase = Phase::Done;
        m_timer = 0.f;
    }

    if (m_phase == Phase::Dive || m_phase == Phase::Pull) {
        const double tgtPos[3] = {tv.pos[0], tv.pos[1], tv.pos[2]};
        float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, tgtPos, m_planetRadiusM);
        ctrl.aileron = bankToTurnAileron(headErr);
        ctrl.rudder = coordinatedRudder(ctrl.aileron);
        ctrl.throttle = 1.f;
        ctrl.afterburner = true;

        if (m_phase == Phase::Dive) {
            ctrl.elevator = -0.5f; // Unload to descend and cut the corner.
        } else {
            ctrl.elevator = 1.f; // Pull up to re-engage at target altitude.
        }
    }
    // Phase::Done: ctrl remains zero-initialized (neutral).

    return ctrl;
}

} // namespace fl::ai
