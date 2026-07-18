// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/Geodetic.h" // kEarthRadiusM
#include "world/AirportDef.h"

#include <cstdint>
#include <functional>
#include <glm/vec3.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fl {

// A runway resolved to world-space geometry on the sphere. threshold/oppositeEnd are the two ends of
// the centerline at the field elevation; centerlineDir is the unit vector threshold -> oppositeEnd.
struct ResolvedRunway {
    glm::dvec3 threshold{0.0};
    glm::dvec3 oppositeEnd{0.0};
    glm::dvec3 centerlineDir{1.0, 0.0, 0.0};
    float lengthM{2500.f};
    float widthM{45.f};
    float headingDeg{90.f};
    RunwaySurface surface{RunwaySurface::Asphalt};
};

// An airport resolved to a world position + resolved runways. worldPos is the field reference point
// at elevationM; elevationM is authoritative (the def value, or the injected terrain height when the
// def said < 0).
struct ResolvedAirport {
    AirportDef def;
    glm::dvec3 worldPos{0.0};
    double elevationM{0.0};
    std::vector<ResolvedRunway> runways;
};

// O(1)-by-id, spatially-queryable airport store. Mirrors the FactionRegistry contract: load() once
// before gameLoop.start(), immutable afterward, lock-free reads from any thread. A ResolvedAirport*
// returned by byId()/nearestTo() stays valid for the session (do not retain across a reload).
//
// #699 provides the definition seam + linear queries for a handful of TOML/builtin airports. #486
// layers the ~80k-airport OurAirports import, a lat/lon bucket grid, and the terrain-flattening
// helper on top; those methods live here from the start so the registry API does not churn.
class AirportRegistry {
  public:
    // Resolves world positions/geometry via geodeticToWorld (or worldX/worldZ for useWorldXZ defs)
    // and elevations via heightFn (only for defs whose elevationM < 0). heightFn returns the raw
    // terrain height above the datum for a world position; it may be null (then unresolved elevations
    // become 0). Replaces all state.
    using HeightFn = std::function<double(glm::dvec3 worldPos)>;
    void load(std::vector<AirportDef> defs, double planetRadiusM, const HeightFn& heightFn);

    [[nodiscard]] const ResolvedAirport* byId(std::string_view id) const noexcept;
    // Nearest airport whose reference point is within maxRangeM of world XZ (x, z), or nullptr.
    // Linear scan in #699; grid-accelerated in #486.
    [[nodiscard]] const ResolvedAirport* nearestTo(double x, double z, double maxRangeM) const noexcept;
    [[nodiscard]] std::size_t count() const noexcept {
        return m_airports.size();
    }
    void forEach(const std::function<void(const ResolvedAirport&)>& fn) const;

    [[nodiscard]] double planetRadiusM() const noexcept {
        return m_planetRadiusM;
    }

    // Non-copyable: a single-owner registry held by reference (mirrors FactionRegistry).
    AirportRegistry() = default;
    AirportRegistry(const AirportRegistry&) = delete;
    AirportRegistry& operator=(const AirportRegistry&) = delete;

  private:
    std::vector<ResolvedAirport> m_airports;
    std::unordered_map<std::string, uint32_t> m_index; // id -> m_airports index
    double m_planetRadiusM{kEarthRadiusM};
};

} // namespace fl
