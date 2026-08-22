// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fl {

// Aviation↔SI conversions and standard gravity, in one place (#1246). The sim is SI throughout;
// these exist for the two boundaries where it is not — content authored in aviation units, and
// readouts a pilot expects in aviation units.
//
// Each conversion has ONE primary and DERIVES its inverse. That is the part worth stating: the
// tree previously carried independently-typed near-inverses that genuinely disagreed —
// `1.94384f` is not `1 / 0.514444f` (the true reciprocal is 0.5144456), and `2.20462f` is not
// `1 / 0.45359237f` — so a speed authored in a def and then shown on the HUD did not come back as
// the number that was written. Deriving costs a change in the sixth significant figure of the
// display-side readouts and buys back the round trip.
//
// As with the angle constants, the type is explicit at the call site: `kMetresPerFoot<float>` and
// `kMetresPerFoot<double>` are different numbers, and a site that reads doubles must say so to
// stay bit-exact.
//
// A site that DIVIDES by a primary should keep dividing (`m / kMetresPerNauticalMile<float>`)
// rather than multiplying by the inverse — the two are not the same float, and the division is
// what is already there.

// ── primaries (authored unit → SI) ───────────────────────────────────────────
template <typename T> inline constexpr T kMetresPerNauticalMile = T(1852);
template <typename T> inline constexpr T kMetresPerFoot = T(0.3048);
template <typename T> inline constexpr T kMpsPerKnot = T(0.514444);
template <typename T> inline constexpr T kKgPerPound = T(0.45359237);

// ── derived (SI → display unit) ──────────────────────────────────────────────
template <typename T> inline constexpr T kNauticalMilesPerMetre = T(1) / kMetresPerNauticalMile<T>;
template <typename T> inline constexpr T kFeetPerMetre = T(1) / kMetresPerFoot<T>;
template <typename T> inline constexpr T kKnotsPerMps = T(1) / kMpsPerKnot<T>;
template <typename T> inline constexpr T kPoundsPerKg = T(1) / kKgPerPound<T>;

// Standard gravity (m/s²), explicit at the call site for the same reason — the flight model reads
// it as float and the ballistics solvers as double.
template <typename T> inline constexpr T kG0 = T(9.80665);

} // namespace fl
