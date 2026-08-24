// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/GunsEmploymentController.h"

#include "ai/Guidance.h"
#include "ai/GunLead.h" // the shared gun lead preamble (#1265)
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

    const GunLeadSolution sol = leadSolution(state, tv, m_muzzleVelMps, m_planetRadiusM);

    // Steer at the lead point (fall back to the target itself when no solution exists — closing
    // back into parameters IS the maneuver).
    const glm::dvec3 steerAt = sol.lead.valid ? sol.lead.aimPoint : sol.tgtPos;
    const double steerArr[3] = {steerAt.x, steerAt.y, steerAt.z};
    ctrl.throttle = m_throttle;
    // 80 deg: tracking for guns is the hardest turn a controller makes.
    steerTowardPoint(ctrl, state.transform.quat, state.transform.pos, state.transform.vel, steerArr, m_planetRadiusM,
                     kCombatBankRad);

    // Trigger discipline: predicted miss = the angle between the nose and the lead direction,
    // scaled by the round's travel to the target range. Fire only when that miss is inside the
    // lethal radius and the target is inside the gun's reach.
    if (sol.lead.valid) {
        const float rangeM = sol.rangeM();
        if (rangeM > 1.f && rangeM <= m_maxRangeM) {
            const glm::vec3 nose = bodyForward(state.transform.quat);
            const glm::vec3 want = glm::normalize(glm::vec3(sol.lead.aimPoint - sol.ownPos));
            ctrl.trigger = fl::withinLethalMiss(nose, want, rangeM, m_lethalRadiusM);
        }
    }

    return ctrl;
}

} // namespace fl::ai
