// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/DamageDef.h"
#include "entity/EntityId.h"

#include <cstdint>
#include <limits>

namespace fl {

struct EntityTransform {
    float pos[3]{};            // world position (m)
    float vel[3]{};            // world velocity (m/s)
    float quat[4]{0, 0, 0, 1}; // orientation quaternion [x,y,z,w]
};

// Mutable per-entity runtime state stored in EntityPool slots.
// All fields are sim-thread-only except EntityManager::liveCount() which uses a separate atomic.
struct EntityState {
    EntityId id;
    uint32_t typeIndex{std::numeric_limits<uint32_t>::max()}; // index into EntityTypeRegistry
    EntityTransform transform;
    float hp{0.f};
    float maxHp{0.f};
    DamageLevel damageLevel{DamageLevel::Intact};
    bool dead{false};
    bool playerOwned{false};
    uint32_t ownerId{0}; // peer ID; 0 = server / AI
};

} // namespace fl
