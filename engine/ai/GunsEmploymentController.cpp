// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/GunsEmploymentController.h"

#include "ai/Guidance.h"
#include "ai/TargetView.h"
#include "flight/BallisticLead.h"
#include "flight/LocalFrame.h"

#include <cmath>

namespace fl::ai {

GunsEmploymentController::GunsEmploymentController(const fl::EntityManager& entityManager, fl::EntityId targetId,
                                                   float muzzleVelMps, float lethalRadiusM, float maxRangeM,
                                                   float throttle)
    : m_entityManager(entityManager), m_targetId(targetId), m_muzzleVelMps(muzzleVelMps),
      m_lethalRadiusM(lethalRadiusM), m_maxRangeM(maxRangeM), m_throttle(throttle) {}

fl::ControlInput GunsEmploymentController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                                  const fl::AiTickContext& ctx) {
    fl::ControlInput ctrl{};

    // Honest targeting (#690): the lead is computed from the CONTACT's last-known state.
    const TargetView tv = resolveTarget(m_entityManager, ctx, m_targetId);
    if (!tv.valid)
        return ctrl;

    const glm::dvec3 ownPos{state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]};
    const glm::vec3 ownVel{state.transform.vel[0], state.transform.vel[1], state.transform.vel[2]};
    const glm::dvec3 tgtPos{tv.pos[0], tv.pos[1], tv.pos[2]};
    const glm::vec3 tgtVel{tv.vel[0], tv.vel[1], tv.vel[2]};

    // Local gravity from the planet-radius the broadcaster wired — the drop is along LOCAL down,
    // which is what keeps the pipper honest far from the world origin.
    const glm::vec3 gravity = -fl::radialUp(ownPos, m_planetRadiusM) * 9.80665f;
    const BallisticLeadResult lead = computeBallisticLead(ownPos, ownVel, tgtPos, tgtVel, m_muzzleVelMps, gravity);

    // Steer at the lead point (fall back to the target itself when no solution exists — closing
    // back into parameters IS the maneuver).
    const glm::dvec3 steerAt = lead.valid ? lead.aimPoint : tgtPos;
    const double steerArr[3] = {steerAt.x, steerAt.y, steerAt.z};
    const float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, steerArr, m_planetRadiusM);
    const float altErr =
        static_cast<float>(fl::localAltitude(steerAt, m_planetRadiusM) - fl::localAltitude(ownPos, m_planetRadiusM));
    const float pitchErr = pitchErrorFromAlt(state.transform.quat, state.transform.pos, altErr, m_planetRadiusM);

    ctrl.throttle = m_throttle;
    // Bank-ANGLE command closed on the current bank, and a rudder that nulls the SIDESLIP (#1143).
    // 80 deg: tracking for guns is the hardest turn a controller makes.
    // The rate-only aileron law wound this controller to 179.8 deg of bank and 89 deg of sideslip
    // within 90 s of a heading error it could not null, and flew it into the ground.
    ctrl.aileron =
        bankToTurnAileron(state.transform.quat, state.transform.pos, headErr, m_planetRadiusM, kCombatBankRad);
    ctrl.rudder = rudderToCoordinate(sideslipOf(state.transform.quat, state.transform.vel));
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    // Trigger discipline: predicted miss = the angle between the nose and the lead direction,
    // scaled by the round's travel to the target range. Fire only when that miss is inside the
    // lethal radius and the target is inside the gun's reach.
    if (lead.valid) {
        const float rangeM = static_cast<float>(glm::length(tgtPos - ownPos));
        if (rangeM > 1.f && rangeM <= m_maxRangeM) {
            const glm::vec3 nose = bodyForward(state.transform.quat);
            const glm::vec3 want = glm::normalize(glm::vec3(lead.aimPoint - ownPos));
            const float cosOff = glm::dot(nose, want);
            const float offRad = std::acos(std::min(1.f, std::max(-1.f, cosOff)));
            const float missM = offRad * rangeM; // small-angle arc at the target's range
            ctrl.trigger = missM <= m_lethalRadiusM;
        }
    }

    return ctrl;
}

} // namespace fl::ai
