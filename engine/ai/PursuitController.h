// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/IEntityController.h"

namespace fl::ai {

// Pursues a target entity using pure pursuit: steers toward the target's
// current position each tick. Returns neutral ControlInput when the target
// is dead or invalid.
class PursuitController : public fl::IEntityController {
  public:
    PursuitController(const fl::EntityManager& entityManager, fl::EntityId targetId, float throttle = 0.85f,
                      bool useAfterburner = false);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                            const fl::SpatialIndex* /*si*/ = nullptr) override;

    void setTarget(fl::EntityId id) noexcept {
        m_targetId = id;
    }

  private:
    const fl::EntityManager& m_entityManager;
    fl::EntityId m_targetId;
    float m_throttle;
    bool m_useAfterburner;
};

} // namespace fl::ai
