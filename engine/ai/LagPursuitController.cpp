// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/LagPursuitController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

#include <algorithm>
#include <cmath>

namespace fl::ai {

LagPursuitController::LagPursuitController(const fl::EntityManager& entityManager, fl::EntityId targetId,
                                           float lagFraction, float throttle, bool useAfterburner)
    : m_entityManager(entityManager), m_targetId(targetId), m_lagFraction(lagFraction), m_throttle(throttle),
      m_useAfterburner(useAfterburner) {}

fl::ControlInput LagPursuitController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                              const fl::SpatialIndex* /*si*/) {
    fl::ControlInput ctrl{};

    const fl::EntityState* target = m_entityManager.get(m_targetId);
    if (!target || target->dead)
        return ctrl;

    // Relative position (float arithmetic is sufficient for kinematics at ACM scales).
    float dx = static_cast<float>(target->transform.pos[0] - state.transform.pos[0]);
    float dy = static_cast<float>(target->transform.pos[1] - state.transform.pos[1]);
    float dz = static_cast<float>(target->transform.pos[2] - state.transform.pos[2]);
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    // Start with target's current position; shift backward if lagFraction > 0.
    double lagPosX = target->transform.pos[0];
    double lagPosY = target->transform.pos[1];
    double lagPosZ = target->transform.pos[2];

    if (m_lagFraction > 0.f && dist > 0.1f) {
        // Closing speed = -d/dt(|r|) = -dot(r_hat, relVel).
        float rvx = target->transform.vel[0] - state.transform.vel[0];
        float rvy = target->transform.vel[1] - state.transform.vel[1];
        float rvz = target->transform.vel[2] - state.transform.vel[2];
        float closingSpeed = -(dx * rvx + dy * rvy + dz * rvz) / dist;
        // Floor closing speed to avoid infinite / negative TTC; cap TTC to 30 s.
        float ttoIntercept = std::min(dist / std::max(closingSpeed, 10.f), 30.f);
        lagPosX -= static_cast<double>(target->transform.vel[0]) * ttoIntercept * m_lagFraction;
        lagPosY -= static_cast<double>(target->transform.vel[1]) * ttoIntercept * m_lagFraction;
        lagPosZ -= static_cast<double>(target->transform.vel[2]) * ttoIntercept * m_lagFraction;
    }

    const double lagPos[3] = {lagPosX, lagPosY, lagPosZ};
    float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, lagPos);
    float altErr = static_cast<float>(lagPosY - state.transform.pos[1]);
    float pitchErr = pitchErrorFromAlt(state.transform.quat, altErr);

    ctrl.throttle = m_throttle;
    ctrl.afterburner = m_useAfterburner;
    ctrl.aileron = bankToTurnAileron(headErr);
    ctrl.rudder = coordinatedRudder(ctrl.aileron);
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    return ctrl;
}

} // namespace fl::ai
