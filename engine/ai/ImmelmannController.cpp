// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/ImmelmannController.h"

#include "ai/Guidance.h" // terrainFloorRecovery — the deck outranks the manoeuvre (#1352)
#include "entity/EntityState.h"

namespace fl::ai {

ImmelmannController::ImmelmannController(float pullDurationS, float rollDurationS)
    : m_pullDuration(pullDurationS), m_rollDuration(rollDurationS) {}

fl::ControlInput ImmelmannController::sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
                                             const fl::AiTickContext& ctx) {
    fl::ControlInput ctrl{};

    m_timer += static_cast<float>(dt);
    if (m_phase == Phase::Pull && m_timer >= m_pullDuration) {
        m_phase = Phase::Roll;
        m_timer = 0.f;
    }
    if (m_phase == Phase::Roll && m_timer >= m_rollDuration) {
        m_phase = Phase::Done;
        m_timer = 0.f;
    }

    // Terrain does not negotiate (#1352). Checked AFTER the phase clock and BEFORE the surfaces:
    // the manoeuvre still finishes on schedule, so the state machine sequencing it transitions
    // out normally, but every tick below the deck flies the recovery instead of the manoeuvre.
    // For a Split-S that is the whole point — the manoeuvre IS a dive, and at the deck it is a
    // way to hit the ground pointing the right way.
    if (terrainFloorRecovery(ctrl, state.transform.quat, state.transform.pos, state.transform.vel, ctx, kCombatDeckAglM,
                             m_planetRadiusM))
        return ctrl;

    if (m_phase == Phase::Pull) {
        ctrl.elevator = 1.f;
        ctrl.throttle = 1.f;
        ctrl.afterburner = true;
    } else if (m_phase == Phase::Roll) {
        ctrl.aileron = 1.f;
        ctrl.rudder = 0.3f;
        ctrl.throttle = 1.f;
        ctrl.afterburner = true;
    }
    // Phase::Done: ctrl remains zero-initialized (neutral).

    return ctrl;
}

} // namespace fl::ai
