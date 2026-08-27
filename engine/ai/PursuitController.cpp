// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/PursuitController.h"

#include "ai/TargetView.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

namespace fl::ai {

PursuitController::PursuitController(const fl::EntityManager& entityManager, fl::EntityId targetId, float throttle,
                                     bool useAfterburner)
    : m_entityManager(entityManager), m_targetId(targetId), m_throttle(throttle), m_useAfterburner(useAfterburner) {}

fl::ControlInput PursuitController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                           const fl::AiTickContext& ctx) {
    fl::ControlInput ctrl{};

    // Terrain does not negotiate (#1352). The deck is checked FIRST and outranks whatever
    // geometry this controller was about to fly; below it the only job is to still be
    // airborne next tick.
    if (terrainFloorRecovery(ctrl, state.transform.quat, state.transform.pos, state.transform.vel, ctx, kCombatDeckAglM,
                             m_planetRadiusM))
        return ctrl;

    // Honest targeting (#690): the target must be a CONTACT when sensing ran — a controller does
    // not chase what its entity cannot see. A coasting contact returns LAST-KNOWN state (steering at
    // a memory is what a coast is for); a dropped one is treated exactly like a dead target.
    const TargetView tv = resolveTarget(m_entityManager, ctx, m_targetId);
    if (!tv.valid)
        return ctrl;

    const double tgtPos[3] = {
        tv.pos[0],
        tv.pos[1],
        tv.pos[2],
    };

    ctrl.throttle = m_throttle;
    ctrl.afterburner = m_useAfterburner;
    // 80 deg: a fighter tracks hard, but a roll past knife-edge is a departure, not a turn.
    steerTowardPoint(ctrl, state.transform.quat, state.transform.pos, state.transform.vel, tgtPos, m_planetRadiusM,
                     kCombatBankRad);

    return ctrl;
}

} // namespace fl::ai
