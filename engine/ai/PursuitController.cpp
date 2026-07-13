// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/PursuitController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

namespace fl::ai {

PursuitController::PursuitController(const fl::EntityManager& entityManager, fl::EntityId targetId, float throttle,
                                     bool useAfterburner)
    : m_entityManager(entityManager), m_targetId(targetId), m_throttle(throttle), m_useAfterburner(useAfterburner) {}

fl::ControlInput PursuitController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                           const fl::AiTickContext& /*ctx*/) {
    fl::ControlInput ctrl{};

    const fl::EntityState* target = m_entityManager.get(m_targetId);
    if (!target || target->dead)
        return ctrl;

    const double tgtPos[3] = {
        target->transform.pos[0],
        target->transform.pos[1],
        target->transform.pos[2],
    };

    const glm::dvec3 ownWorld(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]);
    const glm::dvec3 tgtWorld(tgtPos[0], tgtPos[1], tgtPos[2]);
    float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, tgtPos, m_planetRadiusM);
    float altErr =
        static_cast<float>(fl::localAltitude(tgtWorld, m_planetRadiusM) - fl::localAltitude(ownWorld, m_planetRadiusM));
    float pitchErr = pitchErrorFromAlt(state.transform.quat, state.transform.pos, altErr, m_planetRadiusM);

    ctrl.throttle = m_throttle;
    ctrl.afterburner = m_useAfterburner;
    ctrl.aileron = bankToTurnAileron(headErr);
    ctrl.rudder = coordinatedRudder(ctrl.aileron);
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    return ctrl;
}

} // namespace fl::ai
