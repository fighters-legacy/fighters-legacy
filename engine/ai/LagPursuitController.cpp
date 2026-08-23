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

    double aimPos[3];
    pursuitOffsetPoint(aimPos, state.transform.pos, state.transform.vel, tv.pos, tv.vel, -std::max(m_lagFraction, 0.f));

    ctrl.throttle = m_throttle;
    ctrl.afterburner = m_useAfterburner;
    // 80 deg, as PursuitController.
    steerTowardPoint(ctrl, state.transform.quat, state.transform.pos, state.transform.vel, aimPos, m_planetRadiusM,
                     kCombatBankRad);

    return ctrl;
}

} // namespace fl::ai
