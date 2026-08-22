// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <numbers>

namespace fl {

// Angle constants and wrapping, in one place (#1246). These were re-declared at ~20 sites in five
// spellings — `std::numbers::pi_v<float> / 180.f`, `static_cast<float>(std::numbers::pi) / 180.f`,
// `3.14159265358979323846 / 180.0`, `0.0174532925f`, `0.017453292519943295` — every one of which
// rounds to the identical value at its type, so adopting these changes no number anywhere.
//
// ⚠ The float and double forms are NOT interchangeable and the type is deliberately explicit at
// the call site: `kPi<float>` and `kPi<double>` differ (float pi rounds UP), so a comparison like
// `someFloat > kPi<double>` answers differently from `someFloat > kPi<float>` at the boundary.
// Write the type the surrounding arithmetic already uses, and a bit-exact move stays bit-exact.

template <typename T> inline constexpr T kPi = std::numbers::pi_v<T>;
template <typename T> inline constexpr T kTwoPi = T(2) * kPi<T>;

template <typename T> inline constexpr T kDegToRad = kPi<T> / T(180);
template <typename T> inline constexpr T kRadToDeg = T(180) / kPi<T>;

// Wrap an angle into [-pi, pi]. Both bounds are inclusive: an input of exactly ±pi is returned
// unchanged, which is what three of the four float wrap sites this replaces already did.
//
// Not the convention `engine/weapon/Turret.cpp` uses — that one wraps into (-pi, pi] and keeps its
// own copy, because converting it would move a turret's commanded azimuth at exactly ±pi and that
// is a determinism-gated change, not a refactor.
[[nodiscard]] constexpr float wrapPi(float a) noexcept {
    while (a > kPi<float>)
        a -= kTwoPi<float>;
    while (a < -kPi<float>)
        a += kTwoPi<float>;
    return a;
}

// Wrap an angle into [0, 2pi) — the longitude-difference convention, where "how far east" is never
// negative.
[[nodiscard]] constexpr double wrapTwoPi(double a) noexcept {
    while (a < 0.0)
        a += kTwoPi<double>;
    while (a >= kTwoPi<double>)
        a -= kTwoPi<double>;
    return a;
}

} // namespace fl
