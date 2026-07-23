// SPDX-License-Identifier: GPL-3.0-or-later
#include "Autopilot.h"

#include "ai/Guidance.h" // pitchErrorFromAlt / elevatorFromPitchError / bankToTurnAileron / coordinatedRudder
#include "flight/FlightIntegrator.h" // FlightState
#include "flight/Geodetic.h"         // geodeticAltitude
#include "flight/LocalFrame.h"       // headingOf / bankOf

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl {

namespace {
// Disengage threshold on raw elevator/aileron: a deliberate stick input drops the attitude holds.
constexpr float kStickOverride = 0.25f;
// Damping gains: subtract a fraction of the measured rate so the P-controller does not porpoise/roll
// oscillate at the 60 Hz shaping rate.
constexpr float kPitchRateDamp = 0.8f;                          // s, on the pitch rate omega[2]
constexpr float kRollRateDamp = 0.25f;                          // s, on the roll rate omega[0]
constexpr float kWingLevelP = 2.0f / std::numbers::pi_v<float>; // bank angle -> aileron
constexpr float kSpdP = 0.02f;                                  // throttle per m/s of speed error

float wrapPi(float a) {
    constexpr float kPi = std::numbers::pi_v<float>;
    while (a > kPi)
        a -= 2.0f * kPi;
    while (a < -kPi)
        a += 2.0f * kPi;
    return a;
}

float ownAltitude(const FlightState& s, double R) {
    return static_cast<float>(geodeticAltitude(s.pos_world[0], s.pos_world[1], s.pos_world[2], R));
}
} // namespace

void Autopilot::toggleAltHold(const FlightState& s, double planetRadiusM) {
    if (m_modes & AltHold) {
        m_modes &= ~AltHold;
    } else {
        m_modes |= AltHold;
        m_targetAltM = ownAltitude(s, planetRadiusM);
    }
}

void Autopilot::toggleHdgHold(const FlightState& s, double planetRadiusM) {
    if (m_modes & HdgHold) {
        m_modes &= ~HdgHold;
    } else {
        m_modes |= HdgHold;
        const glm::dvec3 pos(s.pos_world[0], s.pos_world[1], s.pos_world[2]);
        m_targetHeadingRad = headingOf(s.quat, pos, planetRadiusM);
    }
}

void Autopilot::toggleSpdHold(const FlightState& s) {
    if (m_modes & SpdHold) {
        m_modes &= ~SpdHold;
    } else {
        m_modes |= SpdHold;
        m_targetSpeedMps = static_cast<float>(s.vel_body[0]); // body-x speed ~= IAS proxy
    }
}

void Autopilot::notePlayerInput(float elevator, float aileron, float rudder, bool throttleTouched) noexcept {
    (void)rudder; // rudder never disengages a hold
    if (std::abs(elevator) > kStickOverride || std::abs(aileron) > kStickOverride)
        m_modes &= ~(AltHold | HdgHold);
    if (throttleTouched)
        m_modes &= ~SpdHold;
}

AutopilotCommand Autopilot::compute(const FlightState& s, float /*dt*/, double R) const {
    AutopilotCommand cmd;
    if (m_modes == 0)
        return cmd;

    const glm::dvec3 pos(s.pos_world[0], s.pos_world[1], s.pos_world[2]);

    // Altitude hold -> elevator (with pitch-rate damping; omega[2] = pitch rate, +nose-up).
    if (m_modes & AltHold) {
        const float altErr = m_targetAltM - ownAltitude(s, R);
        const float pitchErr = ai::pitchErrorFromAlt(s.quat, s.pos_world, altErr, R);
        cmd.elevator = std::clamp(ai::elevatorFromPitchError(pitchErr) - kPitchRateDamp * s.omega[2], -1.0f, 1.0f);
        cmd.hasPitch = true;
    }

    // Heading hold -> aileron + coordinated rudder (roll-rate damped; omega[0] = roll rate).
    if (m_modes & HdgHold) {
        const float hdgErr = wrapPi(m_targetHeadingRad - headingOf(s.quat, pos, R));
        cmd.aileron = std::clamp(ai::bankToTurnAileron(hdgErr) - kRollRateDamp * s.omega[0], -1.0f, 1.0f);
        cmd.rudder = ai::coordinatedRudder(cmd.aileron);
        cmd.hasRoll = true;
    } else if (m_modes & AltHold) {
        // Altitude hold with no heading hold still needs wings level, or the bank bleeds altitude.
        cmd.aileron = std::clamp(-kWingLevelP * bankOf(s.quat, pos, R) - kRollRateDamp * s.omega[0], -1.0f, 1.0f);
        cmd.rudder = 0.0f;
        cmd.hasRoll = true;
    }

    // Speed hold -> throttle (P about the lagged actual throttle, a windup-free pseudo-integrator).
    if (m_modes & SpdHold) {
        cmd.throttle =
            std::clamp(s.throttle_actual + kSpdP * (m_targetSpeedMps - static_cast<float>(s.vel_body[0])), 0.0f, 1.0f);
        cmd.hasThrottle = true;
    }

    return cmd;
}

} // namespace fl
