// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fl {

enum class ZoneShape : uint8_t { Circle = 0, Polygon = 1 };

// POD descriptor for a restricted-airspace region. Zone tests run in the XZ plane with
// a separate altitude band. Consumed by AlertSystem (#162) and WorldEvolutionDelta (#163);
// mission-YAML parsing is deferred to #162.
struct AirspaceZone {
    std::string id;
    ZoneShape shape{ZoneShape::Circle};

    // Circle (shape == Circle): center + radius in world metres (XZ plane).
    double centerX{0}, centerZ{0}, radiusM{0};

    // Polygon (shape == Polygon): convex XZ vertex pairs.
    std::vector<std::pair<double, double>> vertices;

    double altFloorM{0}, altCeilingM{999'999};

    std::string ownerFactionId; // faction that enforces the zone
    std::string policyId;       // key into the content pack's escalation policies
};

} // namespace fl
