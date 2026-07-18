// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/Geodetic.h" // kEarthRadiusM
#include "world/AirportDef.h"

#include <cstdint>
#include <functional>
#include <glm/vec3.hpp>
#include <optional>
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
// def said < 0). footprintRadiusM is the max distance from worldPos at which any runway's flatten
// blend region can reach — the per-airport bounding sphere used to cull the flatten/surface queries.
struct ResolvedAirport {
    AirportDef def;
    glm::dvec3 worldPos{0.0};
    double elevationM{0.0};
    double footprintRadiusM{0.0};
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
    // Linear scan — a once-per-frame convenience (the flatten hot path uses the grid instead).
    [[nodiscard]] const ResolvedAirport* nearestTo(double x, double z, double maxRangeM) const noexcept;
    // Appends every airport whose reference point is within radiusKm of `centre` (great-circle) to
    // `out`. Grid-accelerated. Order is by ascending airport index (deterministic).
    void airportsNear(LatLonAlt centre, double radiusKm, std::vector<const ResolvedAirport*>& out) const;
    [[nodiscard]] std::size_t count() const noexcept {
        return m_airports.size();
    }
    void forEach(const std::function<void(const ResolvedAirport&)>& fn) const;

    [[nodiscard]] double planetRadiusM() const noexcept {
        return m_planetRadiusM;
    }

    // ── runway terrain flattening (#486) ─────────────────────────────────────
    // Given a raw terrain height above the datum at `worldPos`, return the flattened height inside a
    // runway footprint (the field elevation in the core, a smoothstep blend out to ~2x the footprint,
    // and `rawHeight` untouched beyond). Order-independent max-weight combine across overlapping
    // runways/airports. This is the pure function wired into TerrainStreamer::setHeightModifier, so
    // the physics floor and the tile mesh flatten identically. Lock-free (immutable after load).
    [[nodiscard]] double flattenedHeight(glm::dvec3 worldPos, double rawHeight) const noexcept;

    // The runway surface at `worldPos` if it lies within a runway's CORE footprint (no blend), else
    // nullopt. Feeds TerrainStreamer::surfaceTypeAt via an injected override (#487).
    [[nodiscard]] std::optional<RunwaySurface> runwaySurfaceAt(glm::dvec3 worldPos) const noexcept;

    // The per-tile early-out predicate for TerrainStreamer's flatten pass: true when any airport's
    // footprint sphere intersects the tile's bounding sphere (centre + radius, world metres).
    [[nodiscard]] bool regionHasRunway(glm::dvec3 tileCentre, double tileRadiusM) const noexcept;

    // Non-copyable: a single-owner registry held by reference (mirrors FactionRegistry).
    AirportRegistry() = default;
    AirportRegistry(const AirportRegistry&) = delete;
    AirportRegistry& operator=(const AirportRegistry&) = delete;

    // 1-degree lat/lon bucket grid: 180 latitude bands x 360 longitude bands.
    static constexpr int kGridLat = 180;
    static constexpr int kGridLon = 360;

  private:
    // Visits every airport index in the lat/lon cells overlapping [centre +/- rangeM] (great-circle),
    // with longitude widening by 1/cos(lat) near the poles. fn receives each candidate airport index.
    void forEachCandidate(double latRad, double lonRad, double rangeM, const std::function<void(uint32_t)>& fn) const;

    std::vector<ResolvedAirport> m_airports;
    std::unordered_map<std::string, uint32_t> m_index; // id -> m_airports index
    // CSR bucket grid over m_airports (rebuilt each load, never serialized): m_cellStart has
    // kGridLat*kGridLon+1 entries, m_cellAirports holds airport indices grouped by cell.
    std::vector<uint32_t> m_cellStart;
    std::vector<uint32_t> m_cellAirports;
    double m_planetRadiusM{kEarthRadiusM};
};

} // namespace fl
