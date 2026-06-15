// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>

namespace fl {

struct LatLonAlt {
    double lat_rad; // geodetic latitude  (radians, positive north)
    double lon_rad; // geodetic longitude (radians, positive east)
    double alt_m;   // MSL altitude (metres, positive up)
};

// Spherical Earth radius (m) and standard gravitational parameter (m³/s²).
constexpr double kEarthRadiusM = 6'371'000.0;
constexpr double kEarthGM = 3.986004418e14;

// World XYZ → geodetic (spherical model).  Planet centre at {0, -R, 0}.
// World +X ≈ east, +Z ≈ north at the origin (lat=0, lon=0).
inline LatLonAlt worldToGeodetic(double x, double y, double z, double R = kEarthRadiusM) {
    const double ypR = y + R; // y - centreY = y - (-R)
    const double r = std::sqrt(x * x + ypR * ypR + z * z);
    return {std::asin(ypR / r), std::atan2(x, z), r - R};
}

// Geodetic → world XYZ.
inline void geodeticToWorld(LatLonAlt lla, double& x, double& y, double& z, double R = kEarthRadiusM) {
    const double r = R + lla.alt_m;
    const double cosLat = std::cos(lla.lat_rad);
    x = r * cosLat * std::sin(lla.lon_rad);
    y = r * std::sin(lla.lat_rad) - R;
    z = r * cosLat * std::cos(lla.lon_rad);
}

// Convenience: geodetic MSL altitude only (avoids full decomposition).
inline double geodeticAltitude(double x, double y, double z, double R = kEarthRadiusM) {
    const double ypR = y + R;
    return std::sqrt(x * x + ypR * ypR + z * z) - R;
}

} // namespace fl
