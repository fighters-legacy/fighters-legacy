// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/AirportDef.h"

namespace fl {

// The compiled-in sandbox airfield (#699), so a runway exists with zero content packs — the airport
// counterpart to builtinDebugEntityDef()/builtinAirfield-less sandboxes before it. A fictional strip
// a few km east of the world-origin spawn (placed in world-XZ because the origin is the north pole,
// where lat/lon is singular), one 2500 x 45 m asphalt runway heading 090, elevation resolved from
// the terrain at load. The engine stays content-agnostic: no real-world airfield is hardcoded.
[[nodiscard]] inline AirportDef builtinAirfield() {
    AirportDef def;
    def.id = "builtin:airfield";
    def.name = "Sandbox Airfield";
    def.useWorldXZ = true;
    def.worldX = 4000.0; // ~4 km east of spawn (origin ENU east = +X at the pole)
    def.worldZ = 0.0;
    def.elevationM = -1.0; // resolve from terrain
    def.acceptsLandings = true;
    def.runways.push_back(RunwayDef{/*headingDeg=*/90.f, /*lengthM=*/2500.f, /*widthM=*/45.f, RunwaySurface::Asphalt});
    return def;
}

} // namespace fl
