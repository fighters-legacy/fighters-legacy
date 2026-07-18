// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/SurfaceType.h"
#include "world/AirportDef.h"

namespace fl {

// Total bridge from the RunwaySurface authoring vocabulary (engine/world/AirportDef.h — what a
// runway is PAVED with) to the terrain SurfaceType vocabulary (engine/render/SurfaceType.h — the
// gameplay/physics surface). Header-only and switch-exhaustive: adding a RunwaySurface value without
// a mapping is a -Wswitch build error (both enums are total). Lives on the render side because it is
// the only place both enums are legitimately visible.
[[nodiscard]] constexpr SurfaceType surfaceTypeForRunway(RunwaySurface s) noexcept {
    switch (s) {
    case RunwaySurface::Concrete:
        return SurfaceType::Concrete;
    case RunwaySurface::Asphalt:
        return SurfaceType::Asphalt;
    case RunwaySurface::Grass:
        return SurfaceType::Grass;
    case RunwaySurface::Gravel:
        return SurfaceType::Gravel;
    case RunwaySurface::Water:
        return SurfaceType::Water;
    case RunwaySurface::Deck:
        return SurfaceType::Deck;
    }
    return SurfaceType::Unknown;
}

} // namespace fl
