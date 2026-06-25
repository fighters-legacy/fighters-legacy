// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/LeadPursuitController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

#include <algorithm>
#include <cmath>

namespace fl::ai {

LeadPursuitController::LeadPursuitController(const fl::EntityManager& entityManager, fl::EntityId targetId,
                                             float navGain, float throttle, bool useAfterburner)
    : m_entityManager(entityManager), m_targetId(targetId), m_navGain(navGain), m_throttle(throttle),
      m_useAfterburner(useAfterburner) {}

fl::ControlInput LeadPursuitController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
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

    // Start with target's current position; shift forward if navGain > 0.
    double leadPosX = target->transform.pos[0];
    double leadPosY = target->transform.pos[1];
    double leadPosZ = target->transform.pos[2];

    if (m_navGain > 0.f && dist > 0.1f) {
        // Closing speed = -d/dt(|r|) = -dot(r_hat, relVel).
        float rvx = target->transform.vel[0] - state.transform.vel[0];
        float rvy = target->transform.vel[1] - state.transform.vel[1];
        float rvz = target->transform.vel[2] - state.transform.vel[2];
        float closingSpeed = -(dx * rvx + dy * rvy + dz * rvz) / dist;
        // Floor closing speed to avoid infinite / negative TTC; cap TTC to 30 s.
        float ttoIntercept = std::min(dist / std::max(closingSpeed, 10.f), 30.f);
        leadPosX += static_cast<double>(target->transform.vel[0]) * ttoIntercept * m_navGain;
        leadPosY += static_cast<double>(target->transform.vel[1]) * ttoIntercept * m_navGain;
        leadPosZ += static_cast<double>(target->transform.vel[2]) * ttoIntercept * m_navGain;
    }

    const double leadPos[3] = {leadPosX, leadPosY, leadPosZ};
    float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, leadPos);
    float altErr = static_cast<float>(leadPosY - state.transform.pos[1]);
    float pitchErr = pitchErrorFromAlt(state.transform.quat, altErr);

    ctrl.throttle = m_throttle;
    ctrl.afterburner = m_useAfterburner;
    ctrl.aileron = bankToTurnAileron(headErr);
    ctrl.rudder = coordinatedRudder(ctrl.aileron);
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    return ctrl;
}

} // namespace fl::ai
