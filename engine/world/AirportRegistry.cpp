// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/AirportRegistry.h"

#include "flight/LocalFrame.h" // enuBasis

#include <cmath>
#include <glm/geometric.hpp>
#include <numbers>

namespace fl {

namespace {

// World position of a point at (x, z) on the sphere's near side (the +Y hemisphere around the
// origin/pole) at radial altitude alt above the datum. Mirrors fl-server's nearSideSurface helper:
// the sphere is centred at {0, -R, 0}, so y = -R + sqrt((R+alt)^2 - x^2 - z^2).
[[nodiscard]] glm::dvec3 nearSideWorld(double x, double z, double alt, double R) noexcept {
    const double rr = (R + alt) * (R + alt) - x * x - z * z;
    const double y = -R + std::sqrt(rr > 0.0 ? rr : 0.0);
    return {x, y, z};
}

// World reference point of an airport def at the given elevation.
[[nodiscard]] glm::dvec3 airportWorldPos(const AirportDef& def, double alt, double R) noexcept {
    if (def.useWorldXZ)
        return nearSideWorld(def.worldX, def.worldZ, alt, R);
    double x = 0.0, y = 0.0, z = 0.0;
    geodeticToWorld(LatLonAlt{def.latRad, def.lonRad, alt}, x, y, z, R);
    return {x, y, z};
}

} // namespace

void AirportRegistry::load(std::vector<AirportDef> defs, double planetRadiusM, const HeightFn& heightFn) {
    m_planetRadiusM = planetRadiusM > 0.0 ? planetRadiusM : kEarthRadiusM;
    m_airports.clear();
    m_index.clear();
    m_airports.reserve(defs.size());

    for (auto& def : defs) {
        // Resolve elevation: authoritative def value, or the injected terrain height at the field's
        // ground footprint (queried at the near-side surface so the height function sees a real
        // world position). Unresolvable (no heightFn) collapses to 0.
        double elevationM = def.elevationM;
        if (elevationM < 0.0) {
            elevationM = 0.0;
            if (heightFn) {
                const glm::dvec3 groundProbe = airportWorldPos(def, 0.0, m_planetRadiusM);
                elevationM = heightFn(groundProbe);
            }
        }

        ResolvedAirport resolved;
        resolved.worldPos = airportWorldPos(def, elevationM, m_planetRadiusM);
        resolved.elevationM = elevationM;

        const glm::mat3 enu = enuBasis(resolved.worldPos, m_planetRadiusM);
        const glm::dvec3 east{enu[0]};
        const glm::dvec3 north{enu[1]};

        resolved.runways.reserve(def.runways.size());
        for (const RunwayDef& rw : def.runways) {
            const double hdg = static_cast<double>(rw.headingDeg) * (std::numbers::pi / 180.0);
            // 0 deg = north, 90 deg = east (aviation convention, matching LocalFrame::headingTo).
            const glm::dvec3 dir = glm::normalize(std::sin(hdg) * east + std::cos(hdg) * north);
            const double half = 0.5 * static_cast<double>(rw.lengthM);
            ResolvedRunway rr;
            rr.centerlineDir = dir;
            rr.threshold = resolved.worldPos - dir * half;
            rr.oppositeEnd = resolved.worldPos + dir * half;
            rr.lengthM = rw.lengthM;
            rr.widthM = rw.widthM;
            rr.headingDeg = rw.headingDeg;
            rr.surface = rw.surface;
            resolved.runways.push_back(rr);
        }

        resolved.def = std::move(def);
        const std::string id = resolved.def.id;
        const auto idx = static_cast<uint32_t>(m_airports.size());
        // First id wins (builtin, then packs, then CSV — the caller merges in that order); a
        // duplicate id is dropped so a pack airport shadows a CSV one of the same name.
        if (m_index.emplace(id, idx).second)
            m_airports.push_back(std::move(resolved));
    }
}

const ResolvedAirport* AirportRegistry::byId(std::string_view id) const noexcept {
    const auto it = m_index.find(std::string(id));
    return it == m_index.end() ? nullptr : &m_airports[it->second];
}

const ResolvedAirport* AirportRegistry::nearestTo(double x, double z, double maxRangeM) const noexcept {
    const ResolvedAirport* best = nullptr;
    double bestSq = maxRangeM * maxRangeM;
    for (const auto& a : m_airports) {
        const double dx = a.worldPos.x - x;
        const double dz = a.worldPos.z - z;
        const double d2 = dx * dx + dz * dz;
        if (d2 <= bestSq) {
            bestSq = d2;
            best = &a;
        }
    }
    return best;
}

void AirportRegistry::forEach(const std::function<void(const ResolvedAirport&)>& fn) const {
    for (const auto& a : m_airports)
        fn(a);
}

} // namespace fl
