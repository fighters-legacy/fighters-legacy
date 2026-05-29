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
    }
    return "unknown";
}

} // namespace fl
