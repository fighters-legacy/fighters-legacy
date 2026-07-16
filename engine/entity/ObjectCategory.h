// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

enum class ObjectCategory : uint8_t {
    AirVehicle,
    GroundVehicle,
    NavalVehicle,
    Projectile,
    Effect,
    Player,
    Structure, // fixed emplacement/building (#886) — appended so prior ordinals stay stable
};

// The projectile weapon class (#886) — which store type a Projectile-category entity flew off as.
// Only the store types that fly as ENTITIES appear here: guns are hitscan and never spawn one,
// Pod/Fuel are inert. None on a non-projectile category is the normal state; None on a Projectile
// is a code bug (the parser defaults pack projectiles to Missile) and renders the error marker.
enum class ProjectileKind : uint8_t {
    None,
    Missile,
    Bomb,
    Rocket,
};

// Returns a stable ASCII name for the category (e.g. "air_vehicle"). Never returns nullptr.
inline const char* objectCategoryName(ObjectCategory c) noexcept {
    switch (c) {
    case ObjectCategory::AirVehicle:
        return "air_vehicle";
    case ObjectCategory::GroundVehicle:
        return "ground_vehicle";
    case ObjectCategory::NavalVehicle:
        return "naval_vehicle";
    case ObjectCategory::Projectile:
        return "projectile";
    case ObjectCategory::Effect:
        return "effect";
    case ObjectCategory::Player:
        return "player";
    case ObjectCategory::Structure:
        return "structure";
    }
    return "unknown";
}

// Ordinal gates for values arriving off the wire (MsgEntityTypeDef carries both since #886) —
// gate an attacker-supplied byte before casting, same pattern as isWingmanCommandOrdinal.
// They live HERE beside the enums, not in GameProtocol.h: engine-protocol is stdlib-only and
// cannot include entity headers, and only the client-side parser needs to gate.
[[nodiscard]] inline bool isObjectCategoryOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(ObjectCategory::Structure);
}

[[nodiscard]] inline bool isProjectileKindOrdinal(uint8_t v) noexcept {
    return v <= static_cast<uint8_t>(ProjectileKind::Rocket);
}

} // namespace fl
