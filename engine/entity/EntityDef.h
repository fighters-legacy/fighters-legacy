// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/DamageDef.h"
#include "entity/ObjectCategory.h"

#include <optional>
#include <string>

namespace fl {

// Immutable definition for one entity type, loaded from a content pack TOML file and
// registered with EntityTypeRegistry. Shared by all live instances of the same type.
struct EntityDef {
    std::string id; // content-pack-scoped, e.g. "fl-base:f15c"
    std::string name;
    ObjectCategory category{ObjectCategory::AirVehicle};
    float maxHp{100.f};
    std::optional<DamageDef> damage; // absent = binary death (no progressive damage)
    std::string mesh;                // asset name for primary geometry
    std::string classicDamageMesh;   // JumpToDamage geometry variant; empty if none
    std::string flightModelId;       // flight-model asset id; empty = builtin UFO model (server-side only)
};

} // namespace fl
