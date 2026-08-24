// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/TakeoffController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"
#include "flight/LocalFrame.h"

#include <algorithm>
#include <cmath>

namespace fl::ai {

TakeoffController::TakeoffController(glm::dvec3 threshold, float headingDeg, float runwayElevM, float rotateSpeedMps,
                                     float climboutAglM)
    : m_threshold(threshold), m_headingDeg(headingDeg), m_runwayElevM(runwayElevM), m_rotateSpeedMps(rotateSpeedMps),
      m_climboutAglM(climboutAglM) {}

fl::ControlInput TakeoffController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                           const fl::AiTickContext& /*ctx*/) {
    const glm::dvec3 ownPos(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]);
    const float gs = fl::horizontalGroundSpeed(
        state.transform.vel, glm::dvec3(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]),
        m_planetRadiusM);
    const float agl = static_cast<float>(fl::localAltitude(ownPos, m_planetRadiusM)) - m_runwayElevM;

    // Steer to hold the runway centreline: aim at a point far down the runway from the threshold, so
    // lateral drift is corrected back onto the centreline (not merely held parallel to it).
    const glm::dvec3 dir = fl::worldDirFromHeading(ownPos, m_headingDeg, m_planetRadiusM);
    const glm::dvec3 aim = m_threshold + dir * 3000.0;
    const double aimArr[3] = {aim.x, aim.y, aim.z};
    const float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, aimArr, m_planetRadiusM);

    // Phase advance (self-driving off ground speed + AGL).
    switch (m_phase) {
    case Phase::LineUp:
        if (gs > 2.f)
            m_phase = Phase::Roll;
        break;
    case Phase::Roll:
        if (gs >= m_rotateSpeedMps)
            m_phase = Phase::Rotate;
        break;
    case Phase::Rotate:
        if (agl > 3.f)
            m_phase = Phase::Climb;
        break;
    case Phase::Climb:
        if (agl >= m_climboutAglM)
            m_phase = Phase::Done;
        break;
    case Phase::Done:
        break;
    }

    fl::ControlInput ctrl{};
    switch (m_phase) {
    case Phase::LineUp:
    case Phase::Roll: {
        // Full power, wheels on the ground: the nosewheel (rudder) tracks the centreline; elevator
        // stays neutral so the nose does not rotate before Vr.
        ctrl.throttle = 1.f;
        ctrl.rudder = std::clamp(headErr * 2.f, -1.f, 1.f);
        break;
    }
    case Phase::Rotate: {
        // At Vr, rotate the nose up with a firm aft-stick command; keep steering the centreline while
        // the nosewheel still has authority.
        ctrl.throttle = 1.f;
        ctrl.elevator = 0.6f;
        ctrl.rudder = std::clamp(headErr * 2.f, -1.f, 1.f);
        break;
    }
    case Phase::Climb: {
        // Airborne: hold a steady climb pitch and the runway heading with coordinated bank.
        constexpr float kClimbPitchRad = 0.175f; // ~10 deg nose-up
        const float curPitch = fl::pitchOf(state.transform.quat, ownPos, m_planetRadiusM);
        ctrl.throttle = 1.f;
        ctrl.elevator = elevatorFromPitchError(kClimbPitchRad - curPitch);
        // Bank-ANGLE command closed on the current bank, and a rudder that nulls the SIDESLIP
        // (#1143). 25 deg: a climbout holds the runway heading, it does not manoeuvre.
        ctrl.aileron =
            bankToTurnAileron(state.transform.quat, state.transform.pos, headErr, m_planetRadiusM, kApproachBankRad);
        ctrl.rudder = rudderToCoordinate(sideslipOf(state.transform.quat, state.transform.vel));
        break;
    }
    case Phase::Done:
        break; // neutral surfaces, idle — the outer state machine transitions away
    }
    return ctrl;
}

} // namespace fl::ai
