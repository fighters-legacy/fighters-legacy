// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/IEntityController.h"

namespace fl::ai {

// Flies away from a threat entity (inverse of PursuitController).
// Negates the heading error so the aircraft banks in the opposite direction.
// Returns neutral ControlInput when the threat is dead or invalid.
class EvadeController : public fl::IEntityController {
  public:
    EvadeController(const fl::EntityManager& entityManager, fl::EntityId threatId, float throttle = 1.f,
                    bool useAfterburner = true);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                            const fl::SpatialIndex* /*si*/ = nullptr) override;

    void setThreat(fl::EntityId id) noexcept {
        m_threatId = id;
    }

  private:
    const fl::EntityManager& m_entityManager;
    fl::EntityId m_threatId;
    float m_throttle;
    bool m_useAfterburner;
};

} // namespace fl::ai
