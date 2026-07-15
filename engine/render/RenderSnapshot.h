// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/EngineFailFlags.h" // kEngineFail* — the shared vocabulary (#675)

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
    uint8_t throttle{0};        // [0–100]; from wire MsgEntityEntry::throttle
    uint8_t fuelPct{0};         // [0–100]; from wire MsgEntityEntry::fuelPct
    bool abEngaged{false};      // true when afterburner physically lit (FlightState::ab_engaged)
    uint8_t engineFailFlags{0}; // fl::kEngineFail* bitmask
    glm::vec3 omega{};          // body-frame angular rates p,q,r (rad/s); from wire; used by client-side prediction

    // ── own-record loadout (#625) — meaningful only on the receiving peer's own entry ─────────
    bool hasLoadout{false};       // true when the record carried the own-entity loadout block
    uint8_t selectedStation{255}; // 255 = none; drives the HUD weapon line
    uint16_t stationRounds{0};    // rounds on the selected station
    uint8_t weaponFlags{0};       // bit 0 = seeker locked (#628)
    float payloadMassKg{0.f};     // live store mass — ClientPrediction re-resolves from this
    float payloadCd0{0.f};        // live store drag
};

// Full entity-world snapshot published by the sim thread once per tick.
struct RenderSnapshot {
    uint64_t tickIndex{0};
    std::vector<EntityRenderEntry> entries;
};

} // namespace fl
