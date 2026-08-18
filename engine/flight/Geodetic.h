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

// Earth's sidereal rotation rate (rad/s). The world frame is Earth-fixed rotating about world +Y
// (the polar axis; north pole = origin), so ω_world = (0, kEarthRotationRate, 0) — used by the
// Coriolis/centrifugal terms (#482) and the geographic sun (#481).
constexpr double kEarthRotationRate = 7.2921150e-5;

// World XYZ → geodetic (spherical model).  Planet centre at {0, -R, 0}.
//
// THE WORLD ORIGIN IS THE NORTH POLE, not lat 0 / lon 0: world +Y is the polar axis, so
// worldToGeodetic(0, 0, 0) returns lat = pi/2 with an undefined longitude, and the ENU basis there
// is degenerate (it rotates ~74 degrees over the 4 km to the sandbox airfield). That is why content
// is anchored at a real latitude — see world/SandboxHome.h — rather than authored at the origin.
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

// Offset a geodetic position by local ENU metres: `eastM` along the local east, `northM` along the
// local north, at MSL altitude `altM`. This is how content places things relative to an anchor
// without writing seven-digit world coordinates (#1211).
//
// It offsets LATITUDE AND LONGITUDE rather than stepping along the tangent plane, so the result sits
// exactly on the sphere at `altM` — a tangent-plane step drops ~70 m below the datum at 30 km, which
// would put a "sea level" object underground. The small-offset approximation that remains is in the
// bearing, and it is under a metre at theatre scale.
//
// Near a pole the longitude step diverges (cos(lat) -> 0); the caller is expected to anchor content
// somewhere a compass means something, which is the whole point of having an anchor.
inline LatLonAlt geodeticOffset(LatLonAlt anchor, double eastM, double northM, double altM, double R = kEarthRadiusM) {
    const double lat = anchor.lat_rad + northM / R;
    const double cosLat = std::cos(lat);
    const double lon = anchor.lon_rad + (std::abs(cosLat) > 1e-12 ? eastM / (R * cosLat) : 0.0);
    return {lat, lon, altM};
}

// Move a world position to MSL altitude `altM` along its own radial, keeping its ground track. This
// is what "set this thing's altitude" means on a sphere; near the origin (the pole) it reduces to
// writing the altitude into world Y, which is the shorthand content used to rely on (#1211).
inline void worldAtAltitude(double x, double y, double z, double altM, double& ox, double& oy, double& oz,
                            double R = kEarthRadiusM) {
    const double cy = -R; // planet centre {0, -R, 0}
    const double dx = x, dy = y - cy, dz = z;
    const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-9) { // degenerate: dead centre of the planet, nothing to scale
        ox = x;
        oy = y;
        oz = z;
        return;
    }
    const double k = (R + altM) / len;
    ox = dx * k;
    oy = cy + dy * k;
    oz = dz * k;
}

// The same offset, resolved straight to world XYZ — the form mission/config coordinate resolution
// wants.
inline void localOffsetToWorld(LatLonAlt anchor, double eastM, double northM, double altM, double& x, double& y,
                               double& z, double R = kEarthRadiusM) {
    geodeticToWorld(geodeticOffset(anchor, eastM, northM, altM, R), x, y, z, R);
}

} // namespace fl
