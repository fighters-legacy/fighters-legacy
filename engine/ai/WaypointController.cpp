// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/WaypointController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

#include <cmath>

namespace fl::ai {

WaypointController::WaypointController(std::vector<glm::dvec3> waypoints, float captureRadiusM, float throttle,
                                       bool loop)
    : m_waypoints(std::move(waypoints)), m_captureRadiusM(captureRadiusM), m_throttle(throttle), m_loop(loop) {}

fl::ControlInput WaypointController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                            const fl::SpatialIndex* /*si*/) {
    fl::ControlInput ctrl{};

    // Exhausted (or empty list): return neutral.
    if (m_currentIdx >= static_cast<int>(m_waypoints.size()))
        return ctrl;

    // Check capture: advance before steering so the turn begins this tick.
    const glm::dvec3& wp = m_waypoints[m_currentIdx];
    double dx = wp.x - state.transform.pos[0];
    double dy = wp.y - state.transform.pos[1];
    double dz = wp.z - state.transform.pos[2];
    float dist3d = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));

    if (dist3d < m_captureRadiusM) {
        ++m_currentIdx;
        if (m_loop && m_currentIdx >= static_cast<int>(m_waypoints.size()))
            m_currentIdx = 0;
        // Re-check: if now exhausted, return neutral.
        if (m_currentIdx >= static_cast<int>(m_waypoints.size()))
            return ctrl;
    }

    const glm::dvec3& target = m_waypoints[m_currentIdx];
    double tgtPos[3] = {target.x, target.y, target.z};

    float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, tgtPos);
    float altErr = static_cast<float>(target.y - state.transform.pos[1]);
    float pitchErr = pitchErrorFromAlt(state.transform.quat, altErr);

    ctrl.throttle = m_throttle;
    ctrl.aileron = bankToTurnAileron(headErr);
    ctrl.rudder = coordinatedRudder(ctrl.aileron);
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    return ctrl;
}

} // namespace fl::ai
