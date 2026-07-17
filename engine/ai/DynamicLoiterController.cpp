// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/DynamicLoiterController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

#include <cmath>

namespace fl::ai {

DynamicLoiterController::DynamicLoiterController(const fl::EntityManager& entityManager, fl::EntityId targetId,
                                                 float radiusM, float throttle, LoiterDir dir)
    : m_entityManager(entityManager), m_targetId(targetId), m_radiusM(radiusM), m_throttle(throttle), m_dir(dir) {}

fl::ControlInput DynamicLoiterController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                                 const fl::AiTickContext& /*ctx*/) {
    const fl::EntityState* target = m_entityManager.get(m_targetId);
    if (!target || target->dead)
        return fl::ControlInput{}; // neutral: nothing to escort (same contract as PursuitController)

    // The orbit centre follows the target's LIVE position — this is the whole point vs LoiterController.
    const glm::dvec3 center(target->transform.pos[0], target->transform.pos[1], target->transform.pos[2]);

    fl::ControlInput ctrl{};
    ctrl.throttle = m_throttle;

    // Vector from entity to centre in the XZ plane.
    float tx = static_cast<float>(center.x - state.transform.pos[0]);
    float tz = static_cast<float>(center.z - state.transform.pos[2]);
    float tLen = std::sqrt(tx * tx + tz * tz);

    if (tLen < 1.f)
        return ctrl; // atop the target: hold throttle, neutral surfaces

    float nx = tx / tLen;
    float nz = tz / tLen;

    // Tangent direction for the orbit (matches LoiterController):
    //   Clockwise:        tangent = (nz, -nx) in XZ
    //   CounterClockwise: tangent = (-nz, nx) in XZ
    float tanX, tanZ;
    if (m_dir == LoiterDir::Clockwise) {
        tanX = nz;
        tanZ = -nx;
    } else {
        tanX = -nz;
        tanZ = nx;
    }

    // Lookahead point along the tangent, scaled by the orbit radius.
    double lookahead[3] = {
        state.transform.pos[0] + static_cast<double>(m_radiusM) * static_cast<double>(tanX),
        center.y,
        state.transform.pos[2] + static_cast<double>(m_radiusM) * static_cast<double>(tanZ),
    };

    float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, lookahead, m_planetRadiusM);
    ctrl.aileron = bankToTurnAileron(headErr);
    ctrl.rudder = coordinatedRudder(ctrl.aileron);

    // Radial (local-up) altitude error toward the target's altitude — escort at the covered asset's height.
    const glm::dvec3 ownWorld(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]);
    float altErr =
        static_cast<float>(fl::localAltitude(center, m_planetRadiusM) - fl::localAltitude(ownWorld, m_planetRadiusM));
    float pitchErr = pitchErrorFromAlt(state.transform.quat, state.transform.pos, altErr, m_planetRadiusM);
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    return ctrl;
}

} // namespace fl::ai
