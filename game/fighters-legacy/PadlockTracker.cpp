// SPDX-License-Identifier: GPL-3.0-or-later
#include "PadlockTracker.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl {

namespace {
constexpr float kMaxSlewDegPerS = 240.0f; // slew rate ceiling
constexpr float kDampGain = 6.0f;         // rate = min(max, gain*errDeg) -> damped approach near centre
constexpr float kPitchClampDeg = 88.0f;   // elevation clamp; the aim never wraps across the vertical
constexpr float kBreakGraceS = 0.4f;      // hysteresis at ridge lines before Locked -> Reacquire
constexpr float kReacquireWindowS = 4.0f; // Reacquire waits this long for the target to reappear

constexpr float kDeg2Rad = std::numbers::pi_v<float> / 180.0f;

glm::vec3 safeNormalize(glm::vec3 v, glm::vec3 fallback) {
    const float len = glm::length(v);
    return (len > 1e-6f) ? v / len : fallback;
}

// Rotate `from` toward `to` by at most a rate-limited, error-damped angular step. Direction-vector
// slerp — no yaw/pitch seam, so an overhead crossing produces continuous aim with no 2π jump.
glm::vec3 slewToward(glm::vec3 from, glm::vec3 to, float dt) {
    const float d = std::clamp(glm::dot(from, to), -1.0f, 1.0f);
    const float ang = std::acos(d); // 0..pi
    if (ang < 1e-5f)
        return to;
    const float rateRadS = std::min(kMaxSlewDegPerS, kDampGain * (ang / kDeg2Rad)) * kDeg2Rad;
    const float stepRad = rateRadS * dt;
    const float t = std::min(1.0f, stepRad / ang);
    glm::vec3 axis = glm::cross(from, to);
    if (glm::length(axis) < 1e-6f) // (anti)parallel: pick any perpendicular
        axis = glm::abs(from.y) < 0.9f ? glm::cross(from, glm::vec3{0, 1, 0}) : glm::cross(from, glm::vec3{1, 0, 0});
    axis = glm::normalize(axis);
    const glm::quat q = glm::angleAxis(t * ang, axis);
    return glm::normalize(q * from);
}

// Clamp the aim's elevation (angle above/below the horizon defined by worldUp) to +/-kPitchClampDeg,
// so it never crosses the vertical (the source of roll flips / 2pi jumps).
glm::vec3 clampElevation(glm::vec3 aim, glm::vec3 worldUp) {
    const float s = std::clamp(glm::dot(aim, worldUp), -1.0f, 1.0f);
    const float el = std::asin(s);
    const float maxEl = kPitchClampDeg * kDeg2Rad;
    if (std::abs(el) <= maxEl)
        return aim;
    glm::vec3 horiz = safeNormalize(aim - worldUp * glm::dot(aim, worldUp), glm::vec3{0, 0, -1});
    const float sign = (el > 0.0f) ? 1.0f : -1.0f;
    return glm::normalize(std::cos(maxEl) * horiz + sign * std::sin(maxEl) * worldUp);
}

// Camera up: world-referenced (level horizon) except near the vertical, where the cross product
// degenerates and we fall back to the (orthogonalized) airframe up.
glm::vec3 computeUp(glm::vec3 forward, glm::vec3 worldUp, const glm::quat& ownOrient) {
    if (std::abs(glm::dot(forward, worldUp)) < 0.985f) {
        const glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));
        return glm::normalize(glm::cross(right, forward));
    }
    glm::vec3 up = ownOrient * glm::vec3{0, 1, 0};
    up = up - forward * glm::dot(up, forward); // orthogonalize against forward
    return safeNormalize(up, worldUp);
}

// Cockpit visibility envelope (#697): body-frame az/el floors — the target is masked below the floor.
// None for |az| <= 90 deg; -15 deg for the beam/quarter (90..150); -5 deg over the tail (>150).
bool envelopeVisible(const glm::quat& ownOrient, const glm::vec3& toTargetWorld) {
    const glm::vec3 body = glm::normalize(glm::conjugate(ownOrient) * toTargetWorld); // x fwd, y up, z right
    const float az = std::atan2(body.z, body.x);
    const float el = std::asin(std::clamp(body.y, -1.0f, 1.0f));
    const float azAbsDeg = std::abs(az) / kDeg2Rad;
    float floorDeg = -1000.0f; // no restriction
    if (azAbsDeg > 150.0f)
        floorDeg = -5.0f;
    else if (azAbsDeg > 90.0f)
        floorDeg = -15.0f;
    return el >= floorDeg * kDeg2Rad;
}
} // namespace

void PadlockTracker::enter(glm::vec3 currentForward, glm::vec3 /*currentUp*/) {
    m_aim = safeNormalize(currentForward, glm::vec3{0, 0, -1});
    m_state = PadlockState::Locked;
    m_breakTimer = 0.0f;
    m_reacquireTimer = 0.0f;
}

PadlockPose PadlockTracker::update(const PadlockInputs& in) {
    PadlockPose pose;
    if (m_state == PadlockState::Off) {
        pose.state = PadlockState::Off;
        pose.forward = m_aim;
        pose.up = computeUp(m_aim, in.worldUp, in.ownOrient);
        return pose;
    }

    const glm::vec3 airframeFwd = glm::normalize(in.ownOrient * glm::vec3{1, 0, 0});
    const glm::dvec3 toTgt = in.targetPos - in.ownPos;
    const glm::vec3 desiredToTarget = (glm::length(toTgt) > 1e-3) ? glm::vec3(glm::normalize(toTgt)) : airframeFwd;

    // Visibility = terrain LOS not blocked (Unknown treated as Clear) AND inside the cockpit envelope.
    const bool losOk = in.terrainLos != LosResult::Blocked;
    const bool visible = losOk && envelopeVisible(in.ownOrient, glm::vec3(toTgt));

    // State machine + choose what the aim slews toward this tick.
    glm::vec3 slewTarget = desiredToTarget;
    switch (m_state) {
    case PadlockState::Locked:
        if (!visible) {
            m_state = PadlockState::Breaking;
            m_breakTimer = 0.0f;
        }
        break;
    case PadlockState::Breaking:
        m_breakTimer += in.dt;
        if (visible) {
            m_state = PadlockState::Locked; // regained within the grace window
        } else if (m_breakTimer >= kBreakGraceS) {
            m_state = PadlockState::Reacquire;
            m_reacquireTimer = 0.0f;
        }
        // keep slewing to the (extrapolated) target during the grace window
        break;
    case PadlockState::Reacquire:
        m_reacquireTimer += in.dt;
        if (visible) {
            m_state = PadlockState::Locked;
        } else {
            slewTarget = airframeFwd; // return the aim toward boresight while waiting
            if (m_reacquireTimer >= kReacquireWindowS) {
                m_state = PadlockState::Off;
                pose.exitToCockpit = true;
            }
        }
        break;
    case PadlockState::Off:
        break;
    }

    m_aim = clampElevation(slewToward(m_aim, slewTarget, in.dt), in.worldUp);
    pose.forward = m_aim;
    pose.up = computeUp(m_aim, in.worldUp, in.ownOrient);
    pose.state = m_state;
    return pose;
}

} // namespace fl
