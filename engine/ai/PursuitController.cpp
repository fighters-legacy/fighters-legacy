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

    const glm::dvec3 ownWorld(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]);
    const glm::dvec3 tgtWorld(tgtPos[0], tgtPos[1], tgtPos[2]);
    float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, tgtPos, m_planetRadiusM);
    float altErr =
        static_cast<float>(fl::localAltitude(tgtWorld, m_planetRadiusM) - fl::localAltitude(ownWorld, m_planetRadiusM));
    float pitchErr = pitchErrorFromAlt(state.transform.quat, state.transform.pos, altErr, m_planetRadiusM);

    ctrl.throttle = m_throttle;
    ctrl.afterburner = m_useAfterburner;
    // Bank-ANGLE command closed on the current bank, and a rudder that nulls the SIDESLIP (#1143).
    // 80 deg: a fighter tracks hard, but a roll past knife-edge is a departure, not a turn.
    // The rate-only aileron law wound this controller to 179.8 deg of bank and 89 deg of sideslip
    // within 90 s of a heading error it could not null, and flew it into the ground.
    ctrl.aileron =
        bankToTurnAileron(state.transform.quat, state.transform.pos, headErr, m_planetRadiusM, kCombatBankRad);
    ctrl.rudder = rudderToCoordinate(sideslipOf(state.transform.quat, state.transform.vel));
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    return ctrl;
}

} // namespace fl::ai
