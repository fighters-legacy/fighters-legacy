// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/LoiterController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

#include <algorithm>
#include <cmath>

namespace fl::ai {

LoiterController::LoiterController(glm::dvec3 center, float radiusM, float altitudeM, float throttle, LoiterDir dir)
    : m_center(center), m_radiusM(radiusM), m_altitudeM(altitudeM), m_throttle(throttle),
      // The orbit is only flyable below the speed its bank limit can turn this radius at; hold a
      // margin under it so the turn is not permanently saturated (#1141).
      m_targetSpeedMps(std::clamp(0.85f * turnSpeedForRadius(radiusM, kMaxBankRad), 60.f, 300.f)), m_dir(dir) {}

fl::ControlInput LoiterController::sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
                                          const fl::AiTickContext& /*ctx*/) {
    fl::ControlInput ctrl{};
    const glm::dvec3 ownWorld(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]);
    // Speed hold around the trim throttle (#1141). A FIXED throttle let the aircraft accelerate
    // 150 -> 226 m/s while descending, past the speed its own bank limit can turn a 3 km circle at
    // (v = sqrt(r g tan(bank))) — at which point the orbit can only be flown by trading altitude,
    // and it does, all the way to the ground. Holding the flyable speed removes the energy half of
    // the problem; the flight-path altitude loop below removes the other half.
    const float speed =
        std::sqrt(state.transform.vel[0] * state.transform.vel[0] + state.transform.vel[1] * state.transform.vel[1] +
                  state.transform.vel[2] * state.transform.vel[2]);
    ctrl.throttle = throttleForSpeed(speed, m_targetSpeedMps, m_throttle);

    // Vector from entity to center in XZ plane.
    float tx = static_cast<float>(m_center.x - state.transform.pos[0]);
    float tz = static_cast<float>(m_center.z - state.transform.pos[2]);
    float tLen = std::sqrt(tx * tx + tz * tz);

    if (tLen < 1.f)
        return ctrl; // at center: hold throttle, neutral surfaces

    float nx = tx / tLen;
    float nz = tz / tLen;

    // Tangent direction for orbit:
    //   Clockwise (right turns from +Y view):   tangent = (nz, -nx) in XZ
    //   CounterClockwise (left turns):           tangent = (-nz, nx) in XZ
    float tanX, tanZ;
    if (m_dir == LoiterDir::Clockwise) {
        tanX = nz;
        tanZ = -nx;
    } else {
        tanX = -nz;
        tanZ = nx;
    }

    // Lookahead point along the tangent from current position.
    // Scale with the orbit radius so larger circles get a proportionally farther target.
    // Y component is irrelevant to horizontalHeadingError (projected into the local tangent plane).
    double lookahead[3] = {
        state.transform.pos[0] + static_cast<double>(m_radiusM) * static_cast<double>(tanX),
        m_center.y,
        state.transform.pos[2] + static_cast<double>(m_radiusM) * static_cast<double>(tanZ),
    };

    float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, lookahead, m_planetRadiusM);
    // Bank-ANGLE command closed on the current bank (#1141). An orbit never runs out of heading
    // error — the target bearing keeps moving around the circle — so the rate-only form held the
    // aileron deflected and rolled the aircraft steadily past vertical to inverted, after which
    // "pull up" flies it into the ground.
    ctrl.aileron = bankToTurnAileron(state.transform.quat, state.transform.pos, headErr, m_planetRadiusM, kMaxBankRad);
    // Rudder nulls the SIDESLIP, not the aileron (#1141): in a steady turn the aileron is near zero,
    // so the aileron-proportional form commands nothing and the nose never follows the flight path.
    ctrl.rudder = rudderToCoordinate(sideslipOf(state.transform.quat, state.transform.vel));

    // Altitude hold closed on CLIMB RATE, not pitch attitude (#1141). The attitude form left the
    // aircraft sitting nose-up 30 deg with a -2.8 deg flight path — descending 11 m/s while the
    // loop, satisfied the nose was where it asked for, commanded a neutral elevator into the ground.
    // Pitch rate is differentiated here because EntityState carries no body rates; the first sample
    // has no previous pitch, so it damps on nothing rather than on garbage.
    const float curPitch = fl::pitchOf(state.transform.quat, ownWorld, m_planetRadiusM);
    float pitchRate = 0.f;
    if (m_havePrevPitch && dt > 1e-6)
        pitchRate = static_cast<float>((curPitch - m_prevPitchRad) / dt);
    m_prevPitchRad = curPitch;
    m_havePrevPitch = true;
    ctrl.elevator = elevatorForAltitudeHold(state.transform.quat, state.transform.pos, state.transform.vel, m_altitudeM,
                                            m_planetRadiusM, pitchRate);

    return ctrl;
}

} // namespace fl::ai
