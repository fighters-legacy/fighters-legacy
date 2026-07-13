// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/LagPursuitController.h"

#include "ai/TargetView.h"

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
                                              const fl::AiTickContext& ctx) {
    fl::ControlInput ctrl{};

    // Honest targeting (#690): the target must be a CONTACT when sensing ran — a controller does
    // not chase what its entity cannot see. A coasting contact returns LAST-KNOWN state (steering at
    // a memory is what a coast is for); a dropped one is treated exactly like a dead target.
    const TargetView tv = resolveTarget(m_entityManager, ctx, m_targetId);
    if (!tv.valid)
        return ctrl;

    // Relative position (float arithmetic is sufficient for kinematics at ACM scales).
    float dx = static_cast<float>(tv.pos[0] - state.transform.pos[0]);
    float dy = static_cast<float>(tv.pos[1] - state.transform.pos[1]);
    float dz = static_cast<float>(tv.pos[2] - state.transform.pos[2]);
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    // Start with target's current position; shift backward if lagFraction > 0.
    double lagPosX = tv.pos[0];
    double lagPosY = tv.pos[1];
    double lagPosZ = tv.pos[2];

    if (m_lagFraction > 0.f && dist > 0.1f) {
        // Closing speed = -d/dt(|r|) = -dot(r_hat, relVel).
        float rvx = tv.vel[0] - state.transform.vel[0];
        float rvy = tv.vel[1] - state.transform.vel[1];
        float rvz = tv.vel[2] - state.transform.vel[2];
        float closingSpeed = -(dx * rvx + dy * rvy + dz * rvz) / dist;
        // Floor closing speed to avoid infinite / negative TTC; cap TTC to 30 s.
        float ttoIntercept = std::min(dist / std::max(closingSpeed, 10.f), 30.f);
        lagPosX -= static_cast<double>(tv.vel[0]) * ttoIntercept * m_lagFraction;
        lagPosY -= static_cast<double>(tv.vel[1]) * ttoIntercept * m_lagFraction;
        lagPosZ -= static_cast<double>(tv.vel[2]) * ttoIntercept * m_lagFraction;
    }

    const double lagPos[3] = {lagPosX, lagPosY, lagPosZ};
    const glm::dvec3 ownWorld(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]);
    const glm::dvec3 lagWorld(lagPosX, lagPosY, lagPosZ);
    float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, lagPos, m_planetRadiusM);
    float altErr =
        static_cast<float>(fl::localAltitude(lagWorld, m_planetRadiusM) - fl::localAltitude(ownWorld, m_planetRadiusM));
    float pitchErr = pitchErrorFromAlt(state.transform.quat, state.transform.pos, altErr, m_planetRadiusM);

    ctrl.throttle = m_throttle;
    ctrl.afterburner = m_useAfterburner;
    ctrl.aileron = bankToTurnAileron(headErr);
    ctrl.rudder = coordinatedRudder(ctrl.aileron);
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    return ctrl;
}

} // namespace fl::ai
