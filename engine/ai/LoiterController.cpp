// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/LoiterController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

#include <cmath>

namespace fl::ai {

LoiterController::LoiterController(glm::dvec3 center, float radiusM, float altitudeM, float throttle, LoiterDir dir)
    : m_center(center), m_radiusM(radiusM), m_altitudeM(altitudeM), m_throttle(throttle), m_dir(dir) {}

fl::ControlInput LoiterController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                          const fl::SpatialIndex* /*si*/) {
    fl::ControlInput ctrl{};
    ctrl.throttle = m_throttle;

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
    // Y component is irrelevant to horizontalHeadingError (uses only X,Z).
    double lookahead[3] = {
        state.transform.pos[0] + static_cast<double>(m_radiusM) * static_cast<double>(tanX),
        m_center.y,
        state.transform.pos[2] + static_cast<double>(m_radiusM) * static_cast<double>(tanZ),
    };

    float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, lookahead);
    ctrl.aileron = bankToTurnAileron(headErr);
    ctrl.rudder = coordinatedRudder(ctrl.aileron);

    float altErr = m_altitudeM - static_cast<float>(state.transform.pos[1]);
    float pitchErr = pitchErrorFromAlt(state.transform.quat, altErr);
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    return ctrl;
}

} // namespace fl::ai
