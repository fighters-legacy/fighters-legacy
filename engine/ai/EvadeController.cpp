// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/EvadeController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

namespace fl::ai {

EvadeController::EvadeController(const fl::EntityManager& entityManager, fl::EntityId threatId, float throttle,
                                 bool useAfterburner)
    : m_entityManager(entityManager), m_threatId(threatId), m_throttle(throttle), m_useAfterburner(useAfterburner) {}

fl::ControlInput EvadeController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                         const fl::SpatialIndex* /*si*/) {
    fl::ControlInput ctrl{};

    const fl::EntityState* threat = m_entityManager.get(m_threatId);
    if (!threat || threat->dead)
        return ctrl;

    const double threatPos[3] = {
        threat->transform.pos[0],
        threat->transform.pos[1],
        threat->transform.pos[2],
    };

    // Negate heading error to bank away from the threat.
    float headErr = -horizontalHeadingError(state.transform.quat, state.transform.pos, threatPos);

    ctrl.throttle = m_throttle;
    ctrl.afterburner = m_useAfterburner;
    ctrl.aileron = bankToTurnAileron(headErr);
    ctrl.rudder = coordinatedRudder(ctrl.aileron);
    // Neutral elevator: prioritize horizontal escape over altitude change.

    return ctrl;
}

} // namespace fl::ai
