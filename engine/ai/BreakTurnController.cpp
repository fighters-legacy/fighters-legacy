// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/BreakTurnController.h"

#include "ai/TargetView.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

namespace fl::ai {

BreakTurnController::BreakTurnController(const fl::EntityManager& entityManager, fl::EntityId threatId,
                                         float rollPhaseDurationS, float maxElevator)
    : m_entityManager(entityManager), m_threatId(threatId), m_rollPhaseDuration(rollPhaseDurationS),
      m_maxElevator(maxElevator) {}

fl::ControlInput BreakTurnController::sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
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
    const TargetView tv = resolveTarget(m_entityManager, ctx, m_threatId);
    if (!tv.valid)
        return ctrl;

    if (m_phase == Phase::Roll) {
        m_rollTimer += static_cast<float>(dt);
        if (m_rollTimer >= m_rollPhaseDuration)
            m_phase = Phase::Pull;
    }

    if (m_phase == Phase::Roll) {
        const double threatPos[3] = {
            tv.pos[0],
            tv.pos[1],
            tv.pos[2],
        };
        // Bank toward the threat to orient the lift vector for the maximum-G pull.
        float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, threatPos, m_planetRadiusM);
        // Deliberately the RATE-only turn law (#1143): rolling past knife-edge IS this manoeuvre, so an
        // attitude-closed bank limit would be the regression. It is safe here for the reason the loiter
        // controllers were not — this runs for a couple of seconds under a state machine, not
        // indefinitely, so the heading error cannot outlive the manoeuvre. tests/test_ai_turn_law.cpp
        // pins that it still rolls hard.
        ctrl.aileron = bankToTurnAileron(headErr);
        ctrl.rudder = coordinatedRudder(ctrl.aileron);
        ctrl.throttle = 1.f;
        ctrl.afterburner = true;
    } else {
        // Pull phase: maximum-G elevator, hold current bank.
        ctrl.elevator = m_maxElevator;
        ctrl.throttle = 1.f;
        ctrl.afterburner = true;
    }

    return ctrl;
}

} // namespace fl::ai
