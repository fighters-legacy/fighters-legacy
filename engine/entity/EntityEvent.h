// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/DamageDef.h"
#include "entity/EntityId.h"

#include <cstdint>

namespace fl {

enum class EntityEventType : uint8_t {
    Died,               // entity.dead became true
    DamageLevelChanged, // damage level transitioned (inspect newDamageLevel)
    ScoreAwarded,       // a kill score was credited to instigator's owner
};

struct EntityEvent {
    EntityEventType type;
    EntityId subject;                                // the entity this event is about
    EntityId instigator;                             // who caused it; null() = environment
    DamageLevel newDamageLevel{DamageLevel::Intact}; // meaningful for DamageLevelChanged
    int score{0};                                    // meaningful for ScoreAwarded
};

// Implement this interface and register with EntityManager::addEventHandler() to receive
// entity events. Callbacks fire on the sim thread; do not call HAL methods (except ILogger).
//
// Thread safety: addEventHandler() / removeEventHandler() must be called before
// GameLoop::start(). Mutating the handler list while the sim thread runs is a data race.
class IEntityEventHandler {
  public:
    virtual ~IEntityEventHandler() = default;
    virtual void onEntityEvent(const EntityEvent& event) = 0;
};

} // namespace fl
