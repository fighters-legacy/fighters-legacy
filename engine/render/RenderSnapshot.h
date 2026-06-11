// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace fl {

// Bitmask constants for EntityRenderEntry::engineFailFlags and MsgEntityEntry::engineFailFlags.
// kEngineFailGeneric is derived from entity damage state (damageLevel >= 2) in WorldBroadcaster.
// Remaining bits are populated by FlightIntegrator once per-engine failure is modelled (Phase 6+).
constexpr uint8_t kEngineFailGeneric = 0x01; // generic thrust impairment (from damage)
constexpr uint8_t kEngineFailLeft = 0x02;    // left-engine failure (Phase 6+)
constexpr uint8_t kEngineFailRight = 0x04;   // right-engine failure (Phase 6+)
constexpr uint8_t kEngineCompStall = 0x08;   // compressor stall (Phase 6+)
constexpr uint8_t kEngineFlameout = 0x10;    // flameout (Phase 6+)

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
    uint8_t throttle{0};        // [0–100]; from wire MsgEntityEntry::throttle
    uint8_t fuelPct{0};         // [0–100]; from wire MsgEntityEntry::fuelPct
    bool abEngaged{false};      // true when afterburner physically lit (FlightState::ab_engaged)
    uint8_t engineFailFlags{0}; // fl::kEngineFail* bitmask
};

// Full entity-world snapshot published by the sim thread once per tick.
struct RenderSnapshot {
    uint64_t tickIndex{0};
    std::vector<EntityRenderEntry> entries;
};

} // namespace fl
