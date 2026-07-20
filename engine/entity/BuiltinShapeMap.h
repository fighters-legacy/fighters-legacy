// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/ObjectCategory.h"
#include "render/BuiltinShape.h"

namespace fl {

// Entity vocabulary → render vocabulary (#886): which builtin placeholder silhouette a mesh-less
// entity type renders as. Lives in engine/entity (which already depends on engine-render), NOT in
// engine-render — the observed render layer must not include entity headers; the game-layer
// MeshNameResolver lambda calls this when it fills SceneRenderer::ResolvedMesh.
//
// Unknown is the ERROR outcome, not a default: a Projectile whose kind is None (the parser defaults
// pack projectiles to Missile, so None here is a code bug) and any unmapped value land on the
// jarring error beacon — a bug must not look like a plausible aircraft (#832's lesson). Effect maps
// to the Parachute canopy: the ejected-pilot chute (#672) is the only Effect entity that replicates
// and renders, so it gets a real silhouette rather than the beacon.
[[nodiscard]] inline BuiltinShape builtinShapeFor(ObjectCategory category, ProjectileKind kind) noexcept {
    switch (category) {
    case ObjectCategory::AirVehicle:
    case ObjectCategory::Player:
        return BuiltinShape::AirVehicle;
    case ObjectCategory::GroundVehicle:
        return BuiltinShape::GroundVehicle;
    case ObjectCategory::NavalVehicle:
        return BuiltinShape::NavalVessel;
    case ObjectCategory::Structure:
        return BuiltinShape::Structure;
    case ObjectCategory::Projectile:
        switch (kind) {
        case ProjectileKind::Missile:
            return BuiltinShape::Missile;
        case ProjectileKind::Bomb:
            return BuiltinShape::Bomb;
        case ProjectileKind::Rocket:
            return BuiltinShape::Rocket;
        case ProjectileKind::None:
            return BuiltinShape::Unknown;
        }
        return BuiltinShape::Unknown;
    case ObjectCategory::Effect:
        return BuiltinShape::Parachute;
    }
    return BuiltinShape::Unknown;
}

} // namespace fl
