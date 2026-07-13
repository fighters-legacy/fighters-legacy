// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/EvadeController.h"

#include "ai/TargetView.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

namespace fl::ai {

EvadeController::EvadeController(const fl::EntityManager& entityManager, fl::EntityId threatId, float throttle,
                                 bool useAfterburner)
    : m_entityManager(entityManager), m_threatId(threatId), m_throttle(throttle), m_useAfterburner(useAfterburner) {}

fl::ControlInput EvadeController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                         const fl::AiTickContext& ctx) {
    fl::ControlInput ctrl{};

    // Honest targeting (#690): the target must be a CONTACT when sensing ran — a controller does
    // not chase what its entity cannot see. A coasting contact returns LAST-KNOWN state (steering at
    // a memory is what a coast is for); a dropped one is treated exactly like a dead target.
    const TargetView tv = resolveTarget(m_entityManager, ctx, m_threatId);
    if (!tv.valid)
        return ctrl;

    const double threatPos[3] = {
        tv.pos[0],
        tv.pos[1],
        tv.pos[2],
    };

    // Negate heading error to bank away from the threat.
    float headErr = -horizontalHeadingError(state.transform.quat, state.transform.pos, threatPos, m_planetRadiusM);

    ctrl.throttle = m_throttle;
    ctrl.afterburner = m_useAfterburner;
    ctrl.aileron = bankToTurnAileron(headErr);
    ctrl.rudder = coordinatedRudder(ctrl.aileron);
    // Neutral elevator: prioritize horizontal escape over altitude change.

    return ctrl;
}

} // namespace fl::ai
