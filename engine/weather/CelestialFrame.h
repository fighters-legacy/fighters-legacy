// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Celestial reference frame utilities (#484, spherical-Earth epic #468). Sidereal time + the
// equatorial (right ascension / declination) -> local horizon transform, so the star field and the
// Moon are oriented to the OBSERVER's latitude/longitude and the shared UTC clock — the celestial
// sphere turns about the pole through the night, the pole sits at an altitude equal to the latitude,
// and two players far apart see a different sky. Header-only, pure double math, no link deps.
//
// Companion to SolarPosition.h (the Sun) and LunarPosition.h (the Moon). Frame convention matches
// engine/flight/LocalFrame.h: ENU = (East, North, Up); a world-frame direction is the ENU vector
// mapped through enuBasis(observerPos, R).

#include "flight/LocalFrame.h" // enuBasis

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

namespace fl {

// Greenwich Mean Sidereal Time (radians, [0, 2pi)) for a UT1~=UTC Julian Day (IAU 1982 series).
[[nodiscard]] inline double greenwichMeanSiderealTimeRad(double jd) noexcept {
    const double t = (jd - 2451545.0) / 36525.0;
    // GMST in seconds of time (IAU 1982): the 876600h term is Earth's sidereal rotation.
    double gmstSec = 67310.54841 + (876600.0 * 3600.0 + 8640184.812866) * t + 0.093104 * t * t - 6.2e-6 * t * t * t;
    double hours = std::fmod(gmstSec / 3600.0, 24.0);
    if (hours < 0.0)
        hours += 24.0;
    return hours * (std::numbers::pi / 12.0); // hours -> radians
}

// Local Mean Sidereal Time (radians) at east-positive longitude lonRad.
[[nodiscard]] inline double localSiderealTimeRad(double jd, double lonRad) noexcept {
    double lst = std::fmod(greenwichMeanSiderealTimeRad(jd) + lonRad, 2.0 * std::numbers::pi);
    if (lst < 0.0)
        lst += 2.0 * std::numbers::pi;
    return lst;
}

// Equatorial (right ascension, declination) -> local ENU unit vector (East, North, Up) for an
// observer at geodetic latitude latRad and local sidereal time lstRad. Standard horizontal-coordinate
// transform (azimuth from North, East positive); the result is already unit length.
[[nodiscard]] inline glm::vec3 equatorialToEnu(double raRad, double decRad, double latRad, double lstRad) noexcept {
    const double H = lstRad - raRad; // hour angle
    const double sinDec = std::sin(decRad), cosDec = std::cos(decRad);
    const double sinLat = std::sin(latRad), cosLat = std::cos(latRad);
    const double cosH = std::cos(H), sinH = std::sin(H);
    const double east = -cosDec * sinH;
    const double north = sinDec * cosLat - cosDec * sinLat * cosH;
    const double up = sinDec * sinLat + cosDec * cosLat * cosH;
    return glm::vec3(static_cast<float>(east), static_cast<float>(north), static_cast<float>(up));
}

// World-frame unit direction toward an equatorial point (RA, Dec) for this observer.
[[nodiscard]] inline glm::vec3 equatorialToWorld(double raRad, double decRad, double latRad, double lstRad,
                                                 glm::dvec3 observerPos, double R) noexcept {
    return glm::normalize(enuBasis(observerPos, R) * equatorialToEnu(raRad, decRad, latRad, lstRad));
}

// Rotation mapping a WORLD direction into the equatorial (celestial) Cartesian frame — the basis in
// which stars are fixed (x toward RA=0/Dec=0, y toward RA=90/Dec=0, z toward the north celestial
// pole). The star shader multiplies a view ray by this to look up a fixed procedural star pattern, so
// the whole field rotates correctly about the pole as sidereal time advances. Orthonormal ->
// its transpose is celestial->world.
[[nodiscard]] inline glm::mat3 worldToCelestial(double latRad, double lstRad, glm::dvec3 observerPos,
                                                double R) noexcept {
    const glm::vec3 cx = equatorialToWorld(0.0, 0.0, latRad, lstRad, observerPos, R);
    const glm::vec3 cy = equatorialToWorld(std::numbers::pi / 2.0, 0.0, latRad, lstRad, observerPos, R);
    const glm::vec3 cz = equatorialToWorld(0.0, std::numbers::pi / 2.0, latRad, lstRad, observerPos, R);
    // Columns are the world directions of the celestial axes -> celestial->world; transpose inverts it.
    return glm::transpose(glm::mat3(cx, cy, cz));
}

} // namespace fl
