// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/EvadeController.h"

#include "ai/TargetView.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

namespace fl::ai {

EvadeController::EvadeController(const fl::EntityManager& entityManager, fl::EntityId threatId, float throttle,
                                 bool useAfterburner)
    : m_entityManager(entityManager), m_threatId(threatId), m_throttle(throttle), m_useAfterburner(useAfterburner) {}

fl::ControlInput EvadeController::sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
                                         const fl::AiTickContext& ctx) {
    fl::ControlInput ctrl{};

    // Honest targeting (#690): the target must be a CONTACT when sensing ran — a controller does
    // not chase what its entity cannot see. A coasting contact returns LAST-KNOWN state (steering at
    // a memory is what a coast is for); a dropped one is treated exactly like a dead target.
    const TargetView tv = resolveTarget(m_entityManager, ctx, m_threatId);
    if (!tv.valid)
        return ctrl;

    const double threatPos[3] = {
        tv.pos[0],
        tv.pos[1],
        tv.pos[2],
    };

    // Negate heading error to bank away from the threat.
    float headErr = -horizontalHeadingError(state.transform.quat, state.transform.pos, threatPos, m_planetRadiusM);

    ctrl.throttle = m_throttle;
    ctrl.afterburner = m_useAfterburner;
    // Bank-ANGLE command closed on the current bank, and a rudder that nulls the SIDESLIP (#1143).
    // 60 deg: hard enough to turn away decisively, and still a bank the aircraft can hold while the
    // altitude loop keeps it level — 80 deg sustained put the sideslip past 28 deg and the energy
    // somewhere it could not be spent.
    // A threat that keeps repositioning keeps this heading error alive indefinitely — with the
    // rate-only law that wound the aircraft to 179.9 deg of bank and into the ground within 90 s.
    ctrl.aileron =
        bankToTurnAileron(state.transform.quat, state.transform.pos, headErr, m_planetRadiusM, kFormationBankRad);
    ctrl.rudder = rudderToCoordinate(sideslipOf(state.transform.quat, state.transform.vel));

    // Hold the altitude the evasion STARTED at (#1143). The elevator used to be left neutral, on the
    // reasoning that escape is horizontal and altitude does not matter — but an unloaded aircraft in
    // an 80 deg bank does not fly level, it spirals: measured 3000 m to the ground in 25.9 s at
    // 248 m/s while evading a circling threat. Holding the entry altitude keeps the escape
    // horizontal, which is what that reasoning actually wanted; it is the DIVE it did not intend.
    if (!m_haveHoldAlt) {
        m_holdAltM = static_cast<float>(fl::localAltitude(
            glm::dvec3(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]), m_planetRadiusM));
        m_haveHoldAlt = true;
    }
    const float curPitch = fl::pitchOf(
        state.transform.quat, glm::dvec3(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]),
        m_planetRadiusM);
    ctrl.elevator = elevatorForAltitudeHold(state.transform.quat, state.transform.pos, state.transform.vel, m_holdAltM,
                                            m_planetRadiusM, m_pitchRate.step(curPitch, dt));

    return ctrl;
}

} // namespace fl::ai
