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

fl::ControlInput TakeoffController::sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
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
        // stays neutral so the nose does not rotate before Vr. gear_down is load-bearing (#1334):
        // ControlInput defaults it false, so without the explicit command the actuator retracted the
        // gear DURING THE ROLL and the 0.55 g belly-scrape brake pinned any airframe that cannot
        // out-thrust it — the old UFO could, which is how this hid.
        ctrl.throttle = 1.f;
        ctrl.gear_down = true;
        ctrl.rudder = std::clamp(headErr * 2.f, -1.f, 1.f);
        break;
    }
    case Phase::Rotate: {
        // At Vr, rotate the nose up with a MEASURED aft-stick command; keep steering the centreline
        // while the nosewheel still has authority. 0.3, not the old 0.6 (#1334): elevator trims
        // ~0.8 rad of alpha per rad of deflection on the builtin trainer, so 0.6 commanded ~27 deg
        // of equilibrium alpha — the rotation left the runway stalled at 38 deg and porpoised back
        // onto it. 0.25 rotates to ~11 deg, keeping even the transient under the 15 deg stall.
        ctrl.throttle = 1.f;
        ctrl.gear_down = true;
        ctrl.elevator = 0.25f;
        ctrl.rudder = std::clamp(headErr * 2.f, -1.f, 1.f);
        break;
    }
    case Phase::Climb: {
        // Airborne: climb-rate-closed altitude cascade toward the climbout gate (#1141, adopted here
        // in #1334) and the runway heading with coordinated bank. The old form held a fixed ~10 deg
        // pitch through elevatorFromPitchError, whose P-only elevator droops on a statically stable
        // airframe — the builtin trainer flew it at barely 1 g, level at best — while this cascade
        // keeps demanding until the aircraft is actually climbing. The +100 m margin keeps it
        // commanding a real climb THROUGH the gate rather than levelling at it; Done fires at the
        // gate regardless.
        const float curPitch = fl::pitchOf(state.transform.quat, ownPos, m_planetRadiusM);
        ctrl.throttle = 1.f;
        // Keep the wheels out until the climb is established (#1334): a low performer lifts off at
        // Vr slower than the guidance AoA bound can hold level flight, so the early climbout skims
        // the deck while it accelerates in ground effect — on its GEAR at 0.02 g, which it powers
        // through, not on its belly at 0.55 g, which is a wall. Retract once genuinely away.
        ctrl.gear_down = (agl < 30.f);
        ctrl.elevator = elevatorForAltitudeHold(state.transform.quat, state.transform.pos, state.transform.vel,
                                                m_runwayElevM + m_climboutAglM + 100.f, m_planetRadiusM,
                                                m_pitchRate.step(curPitch, dt));
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
