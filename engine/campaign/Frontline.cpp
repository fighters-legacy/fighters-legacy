// SPDX-License-Identifier: GPL-3.0-or-later
#include "campaign/Frontline.h"

#include <cmath>
#include <numbers>

namespace fl {

namespace {
constexpr double kTwoPi = 2.0 * std::numbers::pi;

// Longitude span of a bounds, handling an antimeridian-spanning box (minLon > maxLon).
double lonSpan(const GeoBounds& b) noexcept {
    double s = b.maxLon - b.minLon;
    if (s < 0.0)
        s += kTwoPi; // wraps the antimeridian
    return s;
}
} // namespace

CellControl decodeFrontlinePixel(uint8_t v) noexcept {
    if (v == 0)
        return {FrontlineControl::Unclaimed, 0};
    if (v == 255)
        return {FrontlineControl::Contested, 0};
    if (v <= 127)
        return {FrontlineControl::SideA, v};
    return {FrontlineControl::SideB, static_cast<uint8_t>(v - 127)}; // 128..254 -> strength 1..127
}

Frontline::Frontline(int cols, int rows, GeoBounds bounds)
    : m_cols(cols > 0 ? cols : 0), m_rows(rows > 0 ? rows : 0), m_bounds(bounds) {
    if (m_cols > 0 && m_rows > 0)
        m_pixels.assign(static_cast<std::size_t>(m_cols) * m_rows, 0);
}

bool Frontline::setPixels(std::vector<uint8_t> pixels) {
    if (static_cast<int>(pixels.size()) != m_cols * m_rows)
        return false;
    m_pixels = std::move(pixels);
    return true;
}

CellControl Frontline::at(int col, int row) const noexcept {
    if (col < 0 || col >= m_cols || row < 0 || row >= m_rows)
        return {};
    return decodeFrontlinePixel(m_pixels[static_cast<std::size_t>(row) * m_cols + col]);
}

void Frontline::cellCenterLatLon(int col, int row, double& latRad, double& lonRad) const noexcept {
    const double span = lonSpan(m_bounds);
    lonRad = m_bounds.minLon + (col + 0.5) * span / (m_cols > 0 ? m_cols : 1);
    if (lonRad > std::numbers::pi)
        lonRad -= kTwoPi; // normalise back into (-pi, pi]
    latRad = m_bounds.maxLat - (row + 0.5) * (m_bounds.maxLat - m_bounds.minLat) / (m_rows > 0 ? m_rows : 1);
}

bool Frontline::geoToCell(double latRad, double lonRad, int& col, int& row) const noexcept {
    if (m_cols <= 0 || m_rows <= 0)
        return false;
    const double latSpan = m_bounds.maxLat - m_bounds.minLat;
    const double lSpan = lonSpan(m_bounds);
    if (latSpan <= 0.0 || lSpan <= 0.0)
        return false;

    double dLon = lonRad - m_bounds.minLon;
    while (dLon < 0.0)
        dLon += kTwoPi;
    while (dLon >= kTwoPi)
        dLon -= kTwoPi;
    if (dLon > lSpan)
        return false; // east of the eastern edge (outside)

    const double fCol = dLon / lSpan;
    const double fRow = (m_bounds.maxLat - latRad) / latSpan;
    if (fRow < 0.0 || fRow >= 1.0 || fCol < 0.0 || fCol >= 1.0)
        return false;

    col = static_cast<int>(fCol * m_cols);
    row = static_cast<int>(fRow * m_rows);
    if (col >= m_cols)
        col = m_cols - 1;
    if (row >= m_rows)
        row = m_rows - 1;
    return true;
}

FrontlineControl Frontline::controlAtWorld(double x, double y, double z, double planetRadiusM) const noexcept {
    const LatLonAlt lla = worldToGeodetic(x, y, z, planetRadiusM);
    int col = 0;
    int row = 0;
    if (!geoToCell(lla.lat_rad, lla.lon_rad, col, row))
        return FrontlineControl::Unclaimed;
    return at(col, row).control;
}

float Frontline::sideFraction(int side) const noexcept {
    if (!valid())
        return 0.f;
    long long a = 0;
    long long b = 0;
    for (uint8_t v : m_pixels) {
        const CellControl c = decodeFrontlinePixel(v);
        if (c.control == FrontlineControl::SideA)
            ++a;
        else if (c.control == FrontlineControl::SideB)
            ++b;
    }
    const long long claimed = a + b;
    if (claimed == 0)
        return 0.f;
    const long long held = (side == 0) ? a : b;
    return static_cast<float>(static_cast<double>(held) / static_cast<double>(claimed));
}

void Frontline::counts(int& unclaimed, int& sideA, int& sideB, int& contested) const noexcept {
    unclaimed = sideA = sideB = contested = 0;
    for (uint8_t v : m_pixels) {
        switch (decodeFrontlinePixel(v).control) {
        case FrontlineControl::Unclaimed:
            ++unclaimed;
            break;
        case FrontlineControl::SideA:
            ++sideA;
            break;
        case FrontlineControl::SideB:
            ++sideB;
            break;
        case FrontlineControl::Contested:
            ++contested;
            break;
        }
    }
}

} // namespace fl
