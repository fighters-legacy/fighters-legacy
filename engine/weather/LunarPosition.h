// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Geocentric Moon position + illuminated fraction (#484, spherical-Earth epic #468). A truncated
// form of Meeus, "Astronomical Algorithms" ch.47 — the dozen largest periodic terms in longitude
// and latitude, good to ~a few arc-minutes, which is far finer than a rendered Moon disc (~0.5 deg)
// needs. Header-only, pure double math, no link deps — unit-tested against known epochs.
//
// Returns the Moon's apparent equatorial coordinates (right ascension / declination), which
// CelestialFrame.h projects into the observer's local sky, plus the illuminated fraction of the disc
// (0 = new, 1 = full). The disc's phase ORIENTATION is reconstructed in the sky shader from the Moon
// and Sun directions, so only the fraction is needed here.

#include <cmath>
#include <numbers>

namespace fl {

struct MoonEquatorial {
    double raRad;               // apparent right ascension
    double decRad;              // apparent declination
    double distanceKm;          // geocentric distance
    double illuminatedFraction; // [0,1]; 0 = new, 1 = full
};

[[nodiscard]] inline MoonEquatorial moonEquatorial(double jd) noexcept {
    const double d2r = std::numbers::pi / 180.0;
    const double T = (jd - 2451545.0) / 36525.0;

    // Mean arguments (degrees), Meeus 47.1-47.5.
    const double Lp = 218.3164477 + 481267.88123421 * T; // mean longitude
    const double D = 297.8501921 + 445267.1114034 * T;   // mean elongation from the Sun
    const double M = 357.5291092 + 35999.0502909 * T;    // Sun mean anomaly
    const double Mp = 134.9633964 + 477198.8675055 * T;  // Moon mean anomaly
    const double F = 93.2720950 + 483202.0175233 * T;    // argument of latitude

    const double Dr = D * d2r, Mr = M * d2r, Mpr = Mp * d2r, Fr = F * d2r;

    // Longitude (degrees) — largest ~dozen periodic terms of Sigma_l / 1e6.
    const double dLon = 6.288774 * std::sin(Mpr) + 1.274027 * std::sin(2 * Dr - Mpr) + 0.658314 * std::sin(2 * Dr) +
                        0.213618 * std::sin(2 * Mpr) - 0.185116 * std::sin(Mr) - 0.114332 * std::sin(2 * Fr) +
                        0.058793 * std::sin(2 * Dr - 2 * Mpr) + 0.057066 * std::sin(2 * Dr - Mr - Mpr) +
                        0.053322 * std::sin(2 * Dr + Mpr) + 0.045758 * std::sin(2 * Dr - Mr) -
                        0.040923 * std::sin(Mr - Mpr) - 0.034720 * std::sin(Dr) - 0.030383 * std::sin(Mr + Mpr);

    // Latitude (degrees) — largest terms of Sigma_b / 1e6.
    const double lat = 5.128122 * std::sin(Fr) + 0.280602 * std::sin(Mpr + Fr) + 0.277693 * std::sin(Mpr - Fr) +
                       0.173237 * std::sin(2 * Dr - Fr) + 0.055413 * std::sin(2 * Dr - Mpr + Fr) +
                       0.046271 * std::sin(2 * Dr - Mpr - Fr) + 0.032573 * std::sin(2 * Dr + Fr);

    // Distance (km) — largest cosine terms of Sigma_r / 1000, base 385000.56 km.
    const double distKm = 385000.56 - 20905.355 * std::cos(Mpr) - 3699.111 * std::cos(2 * Dr - Mpr) -
                          2955.968 * std::cos(2 * Dr) - 569.925 * std::cos(2 * Mpr);

    const double lambda = (Lp + dLon) * d2r; // apparent ecliptic longitude
    const double beta = lat * d2r;           // ecliptic latitude

    // Obliquity of the ecliptic (mean, sufficient here).
    const double eps = (23.439291 - 0.0130042 * T) * d2r;

    const double sinL = std::sin(lambda), cosL = std::cos(lambda);
    const double sinB = std::sin(beta), cosB = std::cos(beta);
    const double sinE = std::sin(eps), cosE = std::cos(eps);

    double ra = std::atan2(sinL * cosE - (sinB / cosB) * sinE, cosL);
    if (ra < 0.0)
        ra += 2.0 * std::numbers::pi;
    const double dec = std::asin(std::clamp(sinB * cosE + cosB * sinE * sinL, -1.0, 1.0));

    // Illuminated fraction from the phase angle. Meeus 48: to good approximation the phase angle is
    // 180 deg - D (elongation), and k = (1 + cos(phase))/2 = (1 - cos D)/2. New moon (D=0) -> 0,
    // full moon (D=180) -> 1.
    double Dnorm = std::fmod(D, 360.0);
    const double k = 0.5 * (1.0 - std::cos(Dnorm * d2r));

    return {ra, dec, distKm, std::clamp(k, 0.0, 1.0)};
}

// Apparent angular radius (radians) of the Moon at a geocentric distance (km). Mean ~0.259 deg.
[[nodiscard]] inline double moonAngularRadiusRad(double distanceKm) noexcept {
    constexpr double kMoonRadiusKm = 1737.4;
    return std::asin(std::clamp(kMoonRadiusKm / distanceKm, 0.0, 1.0));
}

} // namespace fl
