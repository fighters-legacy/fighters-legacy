// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The campaign frontline raster (#635) — an 8-bit grayscale control field over a theater's geographic
// bounds (docs/modding/formats.md "Frontline Raster"). It is the deterministic system of record for
// who controls the ground: a state a story beat sets and dynamic attrition advances, decoupled from
// terrain tiling. Pixel (0,0) is the NORTH-WEST corner (max_lat, min_lon); rows increase southward,
// columns eastward. Pixel values: 0 unclaimed, 1–127 side A (brightness = strength), 128–254 side B,
// 255 contested.
//
// This type owns only the decoded 8-bit pixels + the bounds — PNG decoding is the caller's concern
// (fl-server decodes via stb_image; tests build rasters from raw bytes), so engine-campaign needs no
// image library and stays trivially unit-testable.

#include "entity/Ejection.h" // TerritoryControl — the ejection landing-zone consequence (#672)
#include "flight/Geodetic.h" // LatLonAlt, worldToGeodetic, kEarthRadiusM

#include <cstdint>
#include <vector>

namespace fl {

// Geographic bounding box, radians. minLon > maxLon denotes an antimeridian-spanning theater.
struct GeoBounds {
    double minLat{0.0};
    double minLon{0.0};
    double maxLat{0.0};
    double maxLon{0.0};
};

enum class FrontlineControl : uint8_t { Unclaimed = 0, SideA = 1, SideB = 2, Contested = 3 };

// Decode one 8-bit pixel value into {control, strength}. strength is 0 for Unclaimed/Contested, and
// 1..127 for a held cell (how firmly the owning side holds it).
struct CellControl {
    FrontlineControl control{FrontlineControl::Unclaimed};
    uint8_t strength{0};
};
[[nodiscard]] CellControl decodeFrontlinePixel(uint8_t value) noexcept;

class Frontline {
  public:
    Frontline() = default;
    Frontline(int cols, int rows, GeoBounds bounds);

    // Replace the raster wholesale (the set_frontline semantics — instant, between missions). Returns
    // false and leaves the raster unchanged if `pixels.size() != cols*rows`.
    [[nodiscard]] bool setPixels(std::vector<uint8_t> pixels);

    [[nodiscard]] bool valid() const noexcept {
        return m_cols > 0 && m_rows > 0 && static_cast<int>(m_pixels.size()) == m_cols * m_rows;
    }
    [[nodiscard]] int cols() const noexcept {
        return m_cols;
    }
    [[nodiscard]] int rows() const noexcept {
        return m_rows;
    }
    [[nodiscard]] const GeoBounds& bounds() const noexcept {
        return m_bounds;
    }

    // Control of a cell by grid index (col east, row south). Out-of-range → Unclaimed.
    [[nodiscard]] CellControl at(int col, int row) const noexcept;

    // Geographic centre (radians) of a cell. Longitude wrap for antimeridian theaters is handled.
    void cellCenterLatLon(int col, int row, double& latRad, double& lonRad) const noexcept;

    // Map a geographic position to a cell index. Returns false if outside the bounds.
    [[nodiscard]] bool geoToCell(double latRad, double lonRad, int& col, int& row) const noexcept;

    // Control of the ground under a world position (via worldToGeodetic). Outside the bounds → Unclaimed.
    [[nodiscard]] FrontlineControl controlAtWorld(double x, double y, double z,
                                                  double planetRadiusM = kEarthRadiusM) const noexcept;

    // Fraction of CLAIMED cells (excludes Unclaimed/Contested) held by side 0 (A) or 1 (B), in [0,1].
    // 0 when nothing is claimed. The campaign uses this for frontline-progress readouts and triggers.
    [[nodiscard]] float sideFraction(int side) const noexcept;

    // Count of cells in each control class, for tests / telemetry.
    void counts(int& unclaimed, int& sideA, int& sideB, int& contested) const noexcept;

    // Resolve the ejection territory (#672) for a pilot of `pilotSideIndex` (0 = side A, 1 = side B)
    // whose parachute lands at a world position: friendly when the pilot's side controls the ground
    // there, hostile when the enemy does, neutral over unclaimed/contested/off-map. This is what
    // WorldBroadcaster's territory-query seam wires to, turning a survived ejection into Rescued /
    // Captured / MIA.
    [[nodiscard]] TerritoryControl territoryAtWorld(double x, double y, double z, int pilotSideIndex,
                                                    double planetRadiusM = kEarthRadiusM) const noexcept;

  private:
    int m_cols{0};
    int m_rows{0};
    GeoBounds m_bounds{};
    std::vector<uint8_t> m_pixels;
};

} // namespace fl
