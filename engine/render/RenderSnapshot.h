// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace fl {

// Per-entity data captured at the end of each sim tick and shipped to the render thread.
// Uses primitive types only to avoid introducing a header dependency on engine-entity.
struct EntityRenderEntry {
    uint32_t entityIdx{0};                                     // EntityId::index — pool slot number
    uint32_t entityGen{0};                                     // EntityId::generation — stale-handle discriminator
    uint32_t typeIndex{0};                                     // EntityState::typeIndex — index into EntityTypeRegistry
    glm::dvec3 position{};                                     // world position (m) — double for planet-scale precision
    glm::quat orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // world orientation (identity)
    glm::vec3 velocity{};                                      // world velocity (m/s) — used for sub-tick extrapolation
    uint8_t damageLevel{0};                                    // cast from DamageLevel; 0 = Intact
    bool playerOwned{false};
    uint8_t throttle{0}; // [0–100]; from wire MsgEntityEntry::throttle
    uint8_t fuelPct{0};  // [0–100]; from wire MsgEntityEntry::fuelPct
};

// Full entity-world snapshot published by the sim thread once per tick.
struct RenderSnapshot {
    uint64_t tickIndex{0};
    std::vector<EntityRenderEntry> entries;
};

} // namespace fl
