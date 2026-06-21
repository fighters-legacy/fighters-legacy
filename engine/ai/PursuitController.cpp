// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/PursuitController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

namespace fl::ai {

PursuitController::PursuitController(const fl::EntityManager& entityManager, fl::EntityId targetId, float throttle,
                                     bool useAfterburner)
    : m_entityManager(entityManager), m_targetId(targetId), m_throttle(throttle), m_useAfterburner(useAfterburner) {}

fl::ControlInput PursuitController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                           const fl::SpatialIndex* /*si*/) {
    fl::ControlInput ctrl{};

    const fl::EntityState* target = m_entityManager.get(m_targetId);
    if (!target || target->dead)
        return ctrl;

    const double tgtPos[3] = {
        target->transform.pos[0],
        target->transform.pos[1],
        target->transform.pos[2],
    };

    float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, tgtPos);
    float altErr = static_cast<float>(tgtPos[1] - state.transform.pos[1]);
    float pitchErr = pitchErrorFromAlt(state.transform.quat, altErr);

    ctrl.throttle = m_throttle;
    ctrl.afterburner = m_useAfterburner;
    ctrl.aileron = bankToTurnAileron(headErr);
    ctrl.rudder = coordinatedRudder(ctrl.aileron);
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    return ctrl;
}

} // namespace fl::ai
