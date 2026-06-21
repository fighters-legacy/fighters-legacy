// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

#include <glm/glm.hpp>
#include <vector>

namespace fl::ai {

// Flies a sequence of 3D waypoints in order.
// Advances to the next waypoint when within captureRadiusM of the current one.
// When all waypoints are consumed and loop=false, returns a neutral ControlInput
// (throttle=0, all surfaces=0). When loop=true, wraps back to the first waypoint.
class WaypointController : public fl::IEntityController {
  public:
    explicit WaypointController(std::vector<glm::dvec3> waypoints, float captureRadiusM = 500.f, float throttle = 0.7f,
                                bool loop = false);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                            const fl::SpatialIndex* /*si*/ = nullptr) override;

    int currentWaypointIndex() const noexcept {
        return m_currentIdx;
    }

  private:
    std::vector<glm::dvec3> m_waypoints;
    int m_currentIdx{0};
    float m_captureRadiusM;
    float m_throttle;
    bool m_loop;
};

} // namespace fl::ai
