// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/LandingController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"
#include "flight/LocalFrame.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl::ai {

namespace {

// Horizontal (tangent-plane) distance from `from` to `to` on a planet of radius R.
[[nodiscard]] double horizDistance(glm::dvec3 from, glm::dvec3 to, double R) {
    const glm::dvec3 up(fl::radialUp(from, R));
    const glm::dvec3 d = to - from;
    const glm::dvec3 horiz = d - glm::dot(d, up) * up;
    return glm::length(horiz);
}

} // namespace

LandingController::LandingController(glm::dvec3 threshold, float headingDeg, float runwayElevM, float glideslopeDeg,
                                     float approachSpeedMps)
    : m_threshold(threshold), m_headingDeg(headingDeg), m_runwayElevM(runwayElevM),
      m_glideslopeRad(glideslopeDeg * std::numbers::pi_v<float> / 180.f), m_approachSpeedMps(approachSpeedMps) {}

fl::ControlInput LandingController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                           const fl::AiTickContext& /*ctx*/) {
    const glm::dvec3 ownPos(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]);
    const float gs = fl::horizontalGroundSpeed(
        state.transform.vel, glm::dvec3(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]),
        m_planetRadiusM);
    const float agl = static_cast<float>(fl::localAltitude(ownPos, m_planetRadiusM)) - m_runwayElevM;

    // Steer toward the threshold (the extended centreline runs through it), so lateral error nulls
    // onto the runway heading naturally.
    const double thrArr[3] = {m_threshold.x, m_threshold.y, m_threshold.z};
    const float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, thrArr, m_planetRadiusM);

    // Phase advance.
    constexpr float kFlareAglM = 12.f; // begin the flare below this AGL
    constexpr float kTouchdownAglM = 0.6f;
    switch (m_phase) {
    case Phase::Final:
        if (agl < kFlareAglM)
            m_phase = Phase::Flare;
        break;
    case Phase::Flare:
        if (agl <= kTouchdownAglM)
            m_phase = Phase::Rollout;
        break;
    case Phase::Rollout:
        if (gs < 2.f)
            m_phase = Phase::Done;
        break;
    case Phase::Done:
        break;
    }

    fl::ControlInput ctrl{};
    switch (m_phase) {
    case Phase::Final: {
        // Track the glidepath: the target altitude falls as the aircraft nears the threshold, so
        // holding to it produces the descent. pitchErrorFromAlt is sign-correct — above the path
        // (targetAlt < curAlt) commands nose-down, below it commands nose-up.
        const double dist = horizDistance(ownPos, m_threshold, m_planetRadiusM);
        const float targetAlt = m_runwayElevM + static_cast<float>(dist) * std::tan(m_glideslopeRad);
        const float altErr = targetAlt - static_cast<float>(fl::localAltitude(ownPos, m_planetRadiusM));
        ctrl.elevator = elevatorFromPitchError(
            pitchErrorFromAlt(state.transform.quat, state.transform.pos, altErr, m_planetRadiusM));
        // Bank-ANGLE command closed on the current bank, and a rudder that nulls the SIDESLIP
        // (#1143). 25 deg: an approach is flown gently, and a wing down near the runway is how you
        // arrive on one wingtip. The rate-only law had no ceiling at all here.
        ctrl.aileron =
            bankToTurnAileron(state.transform.quat, state.transform.pos, headErr, m_planetRadiusM, kApproachBankRad);
        ctrl.rudder = rudderToCoordinate(sideslipOf(state.transform.quat, state.transform.vel));
        // Hold the approach speed.
        ctrl.throttle = std::clamp(0.5f + 0.02f * (m_approachSpeedMps - gs), 0.f, 1.f);
        break;
    }
    case Phase::Flare: {
        // Arrest the sink just above the deck: idle power, a gentle nose-up to cushion touchdown.
        ctrl.throttle = 0.f;
        ctrl.elevator = 0.25f;
        ctrl.aileron =
            bankToTurnAileron(state.transform.quat, state.transform.pos, headErr, m_planetRadiusM, kApproachBankRad);
        ctrl.rudder = rudderToCoordinate(sideslipOf(state.transform.quat, state.transform.vel));
        break;
    }
    case Phase::Rollout: {
        // On the ground: full brakes, nosewheel steering (rudder) holds the centreline. Steer along the
        // runway HEADING, not toward the threshold — the threshold is now behind the aircraft, so a
        // threshold-relative error would command a wild turn-around during the rollout.
        const glm::dvec3 dir = fl::worldDirFromHeading(ownPos, m_headingDeg, m_planetRadiusM);
        const glm::dvec3 aim = ownPos + dir * 2000.0;
        const double aimArr[3] = {aim.x, aim.y, aim.z};
        const float rollHeadErr =
            horizontalHeadingError(state.transform.quat, state.transform.pos, aimArr, m_planetRadiusM);
        ctrl.throttle = 0.f;
        ctrl.wheelBrake = 1.f;
        ctrl.rudder = std::clamp(rollHeadErr * 2.f, -1.f, 1.f);
        break;
    }
    case Phase::Done:
        break; // stopped: neutral, idle — the outer state machine transitions away
    }
    return ctrl;
}

} // namespace fl::ai
