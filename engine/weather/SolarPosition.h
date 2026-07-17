// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Geographic solar position (#481, part of the spherical-Earth epic #468). The old sky put the sun
// at the same place at the same clock time everywhere on Earth (a single sinusoid of time-of-day).
// For a planet simulation the sun's azimuth/elevation must depend on the OBSERVER's latitude and
// longitude and the shared UTC date — then the day/night terminator sweeps across longitudes for
// free, sunrise azimuth shifts with latitude and season, and two players far apart see different
// local suns.
//
// This is the NOAA solar-position algorithm (the widely-used Astronomical-Almanac-derived
// spreadsheet method; accurate to ~0.1°, far better than any gameplay needs). Header-only, pure
// double math, no link deps — unit-tested headless.
//
// Frame convention matches engine/flight/LocalFrame.h: azimuth is a compass bearing from true North,
// positive clockwise (East = +90°); elevation is the angle above the local horizon. `sunDirectionEnu`
// returns the unit vector TOWARD the sun in the local East/North/Up tangent basis; callers turn that
// into a world-frame direction with `enuBasis(observerPos, R)` (LocalFrame.h).

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/vec3.hpp>

namespace fl {

inline constexpr double kPiD = std::numbers::pi_v<double>;

// Solar angles at an observer, radians. azimuth from true North, clockwise (E=+π/2); elevation above
// the local horizon (negative at night).
struct SolarAngles {
    double elevationRad;
    double azimuthRad;
};

// Julian Day (days since −4712 Jan 1, 12:00 UTC) from a Unix timestamp (seconds since 1970-01-01
// 00:00 UTC). The Unix epoch is JD 2440587.5.
[[nodiscard]] inline double julianDayFromUnixSeconds(double unixSeconds) noexcept {
    return unixSeconds / 86400.0 + 2440587.5;
}

// Julian Day from a UTC calendar date + fractional hour (Fliegel–Van Flandern, Gregorian).
[[nodiscard]] inline double julianDay(int year, int month, int day, double hourUTC) noexcept {
    const int a = (14 - month) / 12;
    const int y = year + 4800 - a;
    const int m = month + 12 * a - 3;
    const long jdn = day + (153 * m + 2) / 5 + 365L * y + y / 4 - y / 100 + y / 400 - 32045;
    return static_cast<double>(jdn) - 0.5 + hourUTC / 24.0;
}

// Solar declination (rad) for a Julian Day — the sub-solar latitude, ±23.44° across the year.
[[nodiscard]] inline double solarDeclinationRad(double jd) noexcept {
    const double t = (jd - 2451545.0) / 36525.0; // Julian centuries since J2000.0
    const double deg2rad = 0.017453292519943295;
    const double L0 = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
    const double M = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    const double Mr = M * deg2rad;
    const double C = std::sin(Mr) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                     std::sin(2 * Mr) * (0.019993 - 0.000101 * t) + std::sin(3 * Mr) * 0.000289;
    const double trueLong = L0 + C;
    const double lambda = (trueLong - 0.00569 - 0.00478 * std::sin((125.04 - 1934.136 * t) * deg2rad)) * deg2rad;
    const double eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    const double eps = (eps0 + 0.00256 * std::cos((125.04 - 1934.136 * t) * deg2rad)) * deg2rad;
    return std::asin(std::sin(eps) * std::sin(lambda));
}

// Equation of time (minutes): apparent solar time minus mean solar time.
[[nodiscard]] inline double equationOfTimeMinutes(double jd) noexcept {
    const double t = (jd - 2451545.0) / 36525.0;
    const double deg2rad = 0.017453292519943295;
    const double L0 = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
    const double M = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    const double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    const double eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    const double eps = (eps0 + 0.00256 * std::cos((125.04 - 1934.136 * t) * deg2rad)) * deg2rad;
    const double y = std::tan(eps / 2.0) * std::tan(eps / 2.0);
    const double L0r = L0 * deg2rad;
    const double Mr = M * deg2rad;
    const double etRad = y * std::sin(2 * L0r) - 2 * e * std::sin(Mr) + 4 * e * y * std::sin(Mr) * std::cos(2 * L0r) -
                         0.5 * y * y * std::sin(4 * L0r) - 1.25 * e * e * std::sin(2 * Mr);
    return 4.0 * etRad / deg2rad; // radians → minutes of arc-time (4 min per degree)
}

// Solar azimuth/elevation at an observer for a Julian Day (UTC baked into jd). NOAA method.
[[nodiscard]] inline SolarAngles solarAngles(double latRad, double lonRad, double jd) noexcept {
    const double decl = solarDeclinationRad(jd);
    const double eot = equationOfTimeMinutes(jd);

    // Fractional UTC minutes of the day from the Julian Day (JD midnight is at .5).
    const double dayFrac = jd + 0.5 - std::floor(jd + 0.5); // [0,1), 0 = 00:00 UTC
    const double minutesUTC = dayFrac * 1440.0;
    const double lonDeg = lonRad * 57.29577951308232;

    // True solar time (minutes): mean solar time corrected by the equation of time and longitude
    // (4 min per degree east).
    double tst = std::fmod(minutesUTC + eot + 4.0 * lonDeg, 1440.0);
    if (tst < 0.0)
        tst += 1440.0;

    // Hour angle: 0 at local solar noon, negative before noon, +180°..−180°.
    double haDeg = tst / 4.0 - 180.0;
    const double ha = haDeg * 0.017453292519943295;

    const double sinEl = std::sin(latRad) * std::sin(decl) + std::cos(latRad) * std::cos(decl) * std::cos(ha);
    const double el = std::asin(std::clamp(sinEl, -1.0, 1.0));

    // Azimuth from North, clockwise. Derived from the standard zenith-triangle solution.
    const double cosZen = sinEl;
    const double zen = std::acos(std::clamp(cosZen, -1.0, 1.0));
    double az;
    const double denom = std::cos(latRad) * std::sin(zen);
    if (std::fabs(denom) < 1e-9) {
        az = (decl < latRad) ? kPiD : 0.0; // sun at zenith/nadir — bearing degenerate
    } else {
        double cosAz = (std::sin(decl) - std::sin(latRad) * cosZen) / denom;
        cosAz = std::clamp(cosAz, -1.0, 1.0);
        az = std::acos(cosAz); // 0..π measured from North
        if (ha > 0.0)          // afternoon → sun in the western half
            az = 2.0 * kPiD - az;
    }
    return {el, az};
}

// Unit vector TOWARD the sun in the local East/North/Up basis at the observer. Combine with
// enuBasis(observerPos, R) (LocalFrame.h) to get a world-frame direction.
[[nodiscard]] inline glm::vec3 sunDirectionEnu(SolarAngles a) noexcept {
    const double ce = std::cos(a.elevationRad);
    return glm::vec3(static_cast<float>(ce * std::sin(a.azimuthRad)), // East
                     static_cast<float>(ce * std::cos(a.azimuthRad)), // North
                     static_cast<float>(std::sin(a.elevationRad)));   // Up
}

} // namespace fl
