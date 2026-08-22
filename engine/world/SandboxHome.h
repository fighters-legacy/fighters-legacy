// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "math/Angles.h"

#include "flight/Geodetic.h"

namespace fl {

// WHERE THE SANDBOX LIVES (#1211).
//
// The world origin is the NORTH POLE (Geodetic.h: the planet centre is {0, -R, 0} and world +Y is
// the polar axis), and everything shipped used to sit on top of it. That is a defensible frame and a
// bad place to put content, measured:
//
//   - the ENU basis rotates ~74 degrees over the 4 km from the origin to the sandbox airfield, so an
//     aircraft flying dead straight sweeps its compass for no visible reason, and crossing the pole
//     flips north to south;
//   - longitude is degenerate at the origin and hypersensitive beside it;
//   - the sun and moon are true geographic, so at lat 90 the sun holds a constant elevation for all
//     24 hours: a mission's `time:` moved only its azimuth and no sandbox had a sunrise;
//   - `isNight` is a clock test while lighting is geographic, so the two disagreed outright;
//   - Coriolis is maximal at the pole;
//   - real terrain there is Arctic Ocean, under a runway pinned at a fixed 550 m elevation.
//
// So the default home is a real latitude that behaves: 36 degrees north, in the Nevada
// basin-and-range at ~570 m, which is also where this content's fiction already points (aggressor
// training). The elevation barely moves from the old fixed 550 m, so existing `alt:` values and
// every "is it on the deck" threshold keep their range.
//
// The ENGINE STAYS CONTENT-AGNOSTIC. This is a COORDINATE, not a place: the builtin strip remains
// the fictional, unnamed "Sandbox Airfield" (BuiltinAirport.h) that happens to stand here. Naming a
// real airfield, with its real identity and runway designators, is content-pack work — that is where
// real-world likeness belongs, and where the policy for it lives.
constexpr double kSandboxHomeLatDeg = 36.24917;
constexpr double kSandboxHomeLonDeg = -114.99611;
constexpr double kSandboxHomeElevationM = 569.6;

// The home anchor as a geodetic position (radians, MSL metres). Mission coordinates and the
// server's planar spawn coordinates are metres EAST and NORTH of this point.
[[nodiscard]] inline constexpr LatLonAlt sandboxHome() noexcept {
    return LatLonAlt{kSandboxHomeLatDeg * kDegToRad<double>, kSandboxHomeLonDeg * kDegToRad<double>,
                     kSandboxHomeElevationM};
}

} // namespace fl
