// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/AirportRegistry.h"

#include "flight/LocalFrame.h" // enuBasis

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <numbers>

namespace fl {

namespace {

// Runway flatten footprint (#486). The CORE is the runway plus an apron/shoulder margin, held flat at
// the field elevation; the BLEND annulus extends to kBlendFactor x the core half-extents, smoothly
// returning to the raw terrain.
constexpr double kApronM = 100.0;    // extra half-length beyond L/2 in the flat core
constexpr double kShoulderM = 30.0;  // extra half-width beyond W/2 in the flat core
constexpr double kBlendFactor = 2.0; // blend annulus reaches this multiple of the core half-extents

// Range (m) around a query used to gather flatten candidates from the grid before the precise
// per-airport footprint test. Generously larger than any real runway's footprint radius.
constexpr double kFlattenQueryRangeM = 12'000.0;

[[nodiscard]] double smoothstep01(double t) noexcept {
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return t * t * (3.0 - 2.0 * t);
}

// World position of a point at (x, z) on the sphere's near side at radial altitude alt above datum.
[[nodiscard]] glm::dvec3 nearSideWorld(double x, double z, double alt, double R) noexcept {
    const double rr = (R + alt) * (R + alt) - x * x - z * z;
    const double y = -R + std::sqrt(rr > 0.0 ? rr : 0.0);
    return {x, y, z};
}

[[nodiscard]] glm::dvec3 airportWorldPos(const AirportDef& def, double alt, double R) noexcept {
    if (def.useWorldXZ)
        return nearSideWorld(def.worldX, def.worldZ, alt, R);
    double x = 0.0, y = 0.0, z = 0.0;
    geodeticToWorld(LatLonAlt{def.latRad, def.lonRad, alt}, x, y, z, R);
    return {x, y, z};
}

// Core half-extents of a runway's flat footprint.
[[nodiscard]] double coreHalfL(const ResolvedRunway& rw) noexcept {
    return 0.5 * static_cast<double>(rw.lengthM) + kApronM;
}
[[nodiscard]] double coreHalfW(const ResolvedRunway& rw) noexcept {
    return 0.5 * static_cast<double>(rw.widthM) + kShoulderM;
}

// (along, cross) of `worldPos` relative to a runway centred at `centre` with tangent `up`.
struct AlongCross {
    double along;
    double cross;
};
[[nodiscard]] AlongCross alongCross(const ResolvedRunway& rw, glm::dvec3 centre, glm::dvec3 up,
                                    glm::dvec3 worldPos) noexcept {
    const glm::dvec3 d = worldPos - centre;
    const glm::dvec3 crossDir = glm::normalize(glm::cross(up, rw.centerlineDir));
    return {std::abs(glm::dot(d, rw.centerlineDir)), std::abs(glm::dot(d, crossDir))};
}

// Flatten weight [0,1]: 1 in the core, smoothstep in the blend annulus, 0 beyond.
[[nodiscard]] double runwayFlattenWeight(const ResolvedRunway& rw, glm::dvec3 centre, glm::dvec3 up,
                                         glm::dvec3 worldPos) noexcept {
    const AlongCross ac = alongCross(rw, centre, up, worldPos);
    const double halfL = coreHalfL(rw);
    const double halfW = coreHalfW(rw);
    if (ac.along <= halfL && ac.cross <= halfW)
        return 1.0;
    if (ac.along <= kBlendFactor * halfL && ac.cross <= kBlendFactor * halfW) {
        const double tA = ac.along <= halfL ? 0.0 : (ac.along - halfL) / ((kBlendFactor - 1.0) * halfL);
        const double tC = ac.cross <= halfW ? 0.0 : (ac.cross - halfW) / ((kBlendFactor - 1.0) * halfW);
        return 1.0 - smoothstep01(std::max(tA, tC));
    }
    return 0.0;
}

// Great-circle distance (m) between two geodetic points on a sphere of radius R.
[[nodiscard]] double greatCircleM(double lat0, double lon0, double lat1, double lon1, double R) noexcept {
    const double dLat = lat1 - lat0;
    const double dLon = lon1 - lon0;
    const double s = std::sin(dLat * 0.5);
    const double t = std::sin(dLon * 0.5);
    const double a = s * s + std::cos(lat0) * std::cos(lat1) * t * t;
    return 2.0 * R * std::asin(std::min(1.0, std::sqrt(a)));
}

[[nodiscard]] int latBucket(double latRad) noexcept {
    const double deg = latRad * (180.0 / std::numbers::pi); // [-90, 90]
    int b = static_cast<int>(std::floor(deg + 90.0));
    return std::clamp(b, 0, AirportRegistry::kGridLat - 1);
}
[[nodiscard]] int lonBucket(double lonRad) noexcept {
    const double deg = lonRad * (180.0 / std::numbers::pi); // [-180, 180)
    int b = static_cast<int>(std::floor(deg + 180.0));
    b %= AirportRegistry::kGridLon;
    if (b < 0)
        b += AirportRegistry::kGridLon;
    return b;
}

} // namespace

void AirportRegistry::load(std::vector<AirportDef> defs, double planetRadiusM, const HeightFn& heightFn) {
    m_planetRadiusM = planetRadiusM > 0.0 ? planetRadiusM : kEarthRadiusM;
    m_airports.clear();
    m_index.clear();
    m_airports.reserve(defs.size());

    for (auto& def : defs) {
        double elevationM = def.elevationM;
        if (elevationM < 0.0) {
            elevationM = 0.0;
            if (heightFn)
                elevationM = heightFn(airportWorldPos(def, 0.0, m_planetRadiusM));
        }

        ResolvedAirport resolved;
        resolved.worldPos = airportWorldPos(def, elevationM, m_planetRadiusM);
        resolved.elevationM = elevationM;

        const glm::mat3 enu = enuBasis(resolved.worldPos, m_planetRadiusM);
        const glm::dvec3 east{enu[0]};
        const glm::dvec3 north{enu[1]};

        double footprintRadius = 0.0;
        resolved.runways.reserve(def.runways.size());
        for (const RunwayDef& rw : def.runways) {
            const double hdg = static_cast<double>(rw.headingDeg) * (std::numbers::pi / 180.0);
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
            const double hL = kBlendFactor * coreHalfL(rr);
            const double hW = kBlendFactor * coreHalfW(rr);
            footprintRadius = std::max(footprintRadius, std::sqrt(hL * hL + hW * hW));
        }
        resolved.footprintRadiusM = footprintRadius;

        resolved.def = std::move(def);
        const std::string id = resolved.def.id;
        const auto idx = static_cast<uint32_t>(m_airports.size());
        if (m_index.emplace(id, idx).second)
            m_airports.push_back(std::move(resolved));
    }

    // Build the CSR lat/lon bucket grid (counting sort by cell) over the resolved airports.
    const int cells = kGridLat * kGridLon;
    m_cellStart.assign(static_cast<std::size_t>(cells) + 1, 0);
    auto cellOf = [&](const ResolvedAirport& a) -> int {
        const LatLonAlt lla = worldToGeodetic(a.worldPos.x, a.worldPos.y, a.worldPos.z, m_planetRadiusM);
        return latBucket(lla.lat_rad) * kGridLon + lonBucket(lla.lon_rad);
    };
    for (const auto& a : m_airports)
        ++m_cellStart[static_cast<std::size_t>(cellOf(a)) + 1];
    for (int c = 0; c < cells; ++c)
        m_cellStart[static_cast<std::size_t>(c) + 1] += m_cellStart[static_cast<std::size_t>(c)];
    m_cellAirports.assign(m_airports.size(), 0);
    std::vector<uint32_t> cursor(m_cellStart.begin(), m_cellStart.end() - 1);
    for (uint32_t i = 0; i < m_airports.size(); ++i) {
        const int c = cellOf(m_airports[i]);
        m_cellAirports[cursor[static_cast<std::size_t>(c)]++] = i;
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

void AirportRegistry::forEachCandidate(double latRad, double lonRad, double rangeM,
                                       const std::function<void(uint32_t)>& fn) const {
    if (m_cellStart.empty())
        return;
    // Cells span 1 degree; one degree of latitude is ~ pi*R/180 metres.
    const double metrePerLatDeg = std::numbers::pi * m_planetRadiusM / 180.0;
    const int latSpan = std::max(1, static_cast<int>(std::ceil(rangeM / metrePerLatDeg)));

    // Longitude span must use the cos(lat) of the POLEWARD edge of the range, not the query latitude:
    // meridians converge toward the pole, so a point closer to the pole within range spans MORE
    // longitude. If the range reaches the pole, longitudes wrap entirely and the whole ring is in
    // range (over-the-pole great circles connect all longitudes) — scan the full ring then.
    const double angRange = rangeM / m_planetRadiusM; // range as a central angle
    const double latEdge = std::abs(latRad) + angRange;
    const double kHalfPi = std::numbers::pi / 2.0;
    bool fullRing = latEdge >= kHalfPi - 1e-9;
    int lonSpan = kGridLon;
    if (!fullRing) {
        const double metrePerLonDeg = metrePerLatDeg * std::cos(latEdge);
        lonSpan = static_cast<int>(std::ceil(rangeM / metrePerLonDeg)) + 1;
        if (2 * lonSpan + 1 >= kGridLon)
            fullRing = true;
    }

    const int lat0 = latBucket(latRad);
    const int lon0 = lonBucket(lonRad);
    auto visitCell = [&](int lat, int lon) {
        const int c = lat * kGridLon + lon;
        for (uint32_t k = m_cellStart[static_cast<std::size_t>(c)]; k < m_cellStart[static_cast<std::size_t>(c) + 1];
             ++k)
            fn(m_cellAirports[k]);
    };
    for (int dLat = -latSpan; dLat <= latSpan; ++dLat) {
        const int lat = lat0 + dLat;
        if (lat < 0 || lat >= kGridLat)
            continue;
        if (fullRing) {
            for (int lon = 0; lon < kGridLon; ++lon) // each cell exactly once
                visitCell(lat, lon);
        } else {
            for (int dLon = -lonSpan; dLon <= lonSpan; ++dLon) {
                int lon = (lon0 + dLon) % kGridLon;
                if (lon < 0)
                    lon += kGridLon;
                visitCell(lat, lon);
            }
        }
    }
}

void AirportRegistry::airportsNear(LatLonAlt centre, double radiusKm, std::vector<const ResolvedAirport*>& out) const {
    const double rangeM = radiusKm * 1000.0;
    forEachCandidate(centre.lat_rad, centre.lon_rad, rangeM, [&](uint32_t idx) {
        const ResolvedAirport& a = m_airports[idx];
        const LatLonAlt lla = worldToGeodetic(a.worldPos.x, a.worldPos.y, a.worldPos.z, m_planetRadiusM);
        if (greatCircleM(centre.lat_rad, centre.lon_rad, lla.lat_rad, lla.lon_rad, m_planetRadiusM) <= rangeM)
            out.push_back(&a);
    });
    // Deterministic: ascending airport index. m_airports is contiguous, so pointer order == index
    // order, giving a stable contract regardless of the cell-visit order.
    std::sort(out.begin(), out.end());
}

double AirportRegistry::flattenedHeight(glm::dvec3 worldPos, double rawHeight) const noexcept {
    if (m_airports.empty())
        return rawHeight;
    const LatLonAlt lla = worldToGeodetic(worldPos.x, worldPos.y, worldPos.z, m_planetRadiusM);
    double bestWeight = 0.0;
    double bestElev = rawHeight;
    forEachCandidate(lla.lat_rad, lla.lon_rad, kFlattenQueryRangeM, [&](uint32_t idx) {
        const ResolvedAirport& a = m_airports[idx];
        if (glm::length(worldPos - a.worldPos) > a.footprintRadiusM)
            return; // beyond this field's footprint sphere
        const glm::dvec3 up = radialUp(a.worldPos, m_planetRadiusM);
        for (const auto& rw : a.runways) {
            const double w = runwayFlattenWeight(rw, a.worldPos, up, worldPos);
            if (w > bestWeight) {
                bestWeight = w;
                bestElev = a.elevationM;
            }
        }
    });
    return bestWeight > 0.0 ? rawHeight + (bestElev - rawHeight) * bestWeight : rawHeight;
}

std::optional<RunwaySurface> AirportRegistry::runwaySurfaceAt(glm::dvec3 worldPos) const noexcept {
    if (m_airports.empty())
        return std::nullopt;
    const LatLonAlt lla = worldToGeodetic(worldPos.x, worldPos.y, worldPos.z, m_planetRadiusM);
    std::optional<RunwaySurface> result;
    forEachCandidate(lla.lat_rad, lla.lon_rad, kFlattenQueryRangeM, [&](uint32_t idx) {
        if (result)
            return;
        const ResolvedAirport& a = m_airports[idx];
        if (glm::length(worldPos - a.worldPos) > a.footprintRadiusM)
            return;
        const glm::dvec3 up = radialUp(a.worldPos, m_planetRadiusM);
        for (const auto& rw : a.runways) {
            const AlongCross ac = alongCross(rw, a.worldPos, up, worldPos);
            if (ac.along <= coreHalfL(rw) && ac.cross <= coreHalfW(rw)) {
                result = rw.surface;
                return;
            }
        }
    });
    return result;
}

bool AirportRegistry::regionHasRunway(glm::dvec3 tileCentre, double tileRadiusM) const noexcept {
    if (m_airports.empty())
        return false;
    const LatLonAlt lla = worldToGeodetic(tileCentre.x, tileCentre.y, tileCentre.z, m_planetRadiusM);
    bool hit = false;
    forEachCandidate(lla.lat_rad, lla.lon_rad, tileRadiusM + kFlattenQueryRangeM, [&](uint32_t idx) {
        if (hit)
            return;
        const ResolvedAirport& a = m_airports[idx];
        if (glm::length(tileCentre - a.worldPos) <= tileRadiusM + a.footprintRadiusM)
            hit = true;
    });
    return hit;
}

void AirportRegistry::forEach(const std::function<void(const ResolvedAirport&)>& fn) const {
    for (const auto& a : m_airports)
        fn(a);
}

} // namespace fl
