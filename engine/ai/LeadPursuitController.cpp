// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/LeadPursuitController.h"

#include "ai/TargetView.h"

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

    // Start with target's current position; shift forward if navGain > 0.
    double leadPosX = tv.pos[0];
    double leadPosY = tv.pos[1];
    double leadPosZ = tv.pos[2];

    if (m_navGain > 0.f && dist > 0.1f) {
        // Closing speed = -d/dt(|r|) = -dot(r_hat, relVel).
        float rvx = tv.vel[0] - state.transform.vel[0];
        float rvy = tv.vel[1] - state.transform.vel[1];
        float rvz = tv.vel[2] - state.transform.vel[2];
        float closingSpeed = -(dx * rvx + dy * rvy + dz * rvz) / dist;
        // Floor closing speed to avoid infinite / negative TTC; cap TTC to 30 s.
        float ttoIntercept = std::min(dist / std::max(closingSpeed, 10.f), 30.f);
        leadPosX += static_cast<double>(tv.vel[0]) * ttoIntercept * m_navGain;
        leadPosY += static_cast<double>(tv.vel[1]) * ttoIntercept * m_navGain;
        leadPosZ += static_cast<double>(tv.vel[2]) * ttoIntercept * m_navGain;
    }

    const double leadPos[3] = {leadPosX, leadPosY, leadPosZ};
    const glm::dvec3 ownWorld(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]);
    const glm::dvec3 leadWorld(leadPosX, leadPosY, leadPosZ);
    float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, leadPos, m_planetRadiusM);
    float altErr = static_cast<float>(fl::localAltitude(leadWorld, m_planetRadiusM) -
                                      fl::localAltitude(ownWorld, m_planetRadiusM));
    float pitchErr = pitchErrorFromAlt(state.transform.quat, state.transform.pos, altErr, m_planetRadiusM);

    ctrl.throttle = m_throttle;
    ctrl.afterburner = m_useAfterburner;
    // Bank-ANGLE command closed on the current bank, and a rudder that nulls the SIDESLIP (#1143).
    // 80 deg, as PursuitController.
    // The rate-only aileron law wound this controller to 179.8 deg of bank and 89 deg of sideslip
    // within 90 s of a heading error it could not null, and flew it into the ground.
    ctrl.aileron =
        bankToTurnAileron(state.transform.quat, state.transform.pos, headErr, m_planetRadiusM, kCombatBankRad);
    ctrl.rudder = rudderToCoordinate(sideslipOf(state.transform.quat, state.transform.vel));
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    return ctrl;
}

} // namespace fl::ai
