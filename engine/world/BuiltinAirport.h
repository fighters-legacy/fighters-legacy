// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/AirportDef.h"
#include "world/SandboxHome.h"

namespace fl {

// The compiled-in sandbox airfield (#699), so a runway exists with zero content packs — the airport
// counterpart to builtinDebugEntityDef(). A fictional strip with one 2500 x 45 m asphalt runway
// heading 090, standing AT THE SANDBOX HOME (SandboxHome.h) — 36.25 N in the Nevada basin-and-range,
// which is also the anchor a mission's coordinates are measured from.
//
// It used to be placed in world-XZ 4 km east of the world origin, with a comment explaining that a
// near-origin field cannot be placed in lat/lon because the origin is the NORTH POLE, where
// longitude and the ENU basis are singular. Moving the home off the pole (#1211) removes the reason
// for that workaround: the field is now geodetic like any other airport.
//
// The elevation is FIXED (not terrain-resolved, #486): a fixed value is byte-identical on the server
// and every client without either priming the tile at load, so the runway flatten agrees on both ends
// by construction (the flatten's blend annulus grades the pad into the surrounding terrain). It is
// the home's elevation, ~570 m, which is within 20 m of the 550 m the strip used to be pinned at —
// so authored altitudes and every "is it on the deck" threshold keep their range.
//
// The engine stays content-agnostic: this is geography, not a named base. No real-world airfield is
// hardcoded — a pack that wants a named field ships its own airport def.
constexpr double kBuiltinAirfieldElevationM = kSandboxHomeElevationM;
[[nodiscard]] inline AirportDef builtinAirfield() {
    AirportDef def;
    def.id = "builtin:airfield";
    def.name = "Sandbox Airfield";
    def.useWorldXZ = false;
    def.latRad = sandboxHome().lat_rad;
    def.lonRad = sandboxHome().lon_rad;
    def.elevationM = kBuiltinAirfieldElevationM;
    def.acceptsLandings = true;
    def.runways.push_back(RunwayDef{/*headingDeg=*/90.f, /*lengthM=*/2500.f, /*widthM=*/45.f, RunwaySurface::Asphalt});
    return def;
}

} // namespace fl
