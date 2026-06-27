// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/IEntityController.h"

namespace fl::ai {

// Lag pursuit controller.
// Aims behind the target at `target.pos - target.vel * TTC * lagFraction`, placing
// the attacker inside the target's turn circle without overshooting.
//
// lagFraction=0 degenerates to pure pursuit (aims at target.pos).
// lagFraction=1 places the aim point one TTC-step behind the target (standard guns
// employment position). Values > 1 exaggerate the lag for very tight turning targets.
//
// Returns neutral ControlInput when the target is dead or invalid.
class LagPursuitController : public fl::IEntityController {
  public:
    LagPursuitController(const fl::EntityManager& entityManager, fl::EntityId targetId, float lagFraction = 1.0f,
                         float throttle = 0.85f, bool useAfterburner = false);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                            const fl::SpatialIndex* /*si*/ = nullptr) override;

    void setTarget(fl::EntityId id) noexcept {
        m_targetId = id;
    }

  private:
    const fl::EntityManager& m_entityManager;
    fl::EntityId m_targetId;
    float m_lagFraction;
    float m_throttle;
    bool m_useAfterburner;
};

} // namespace fl::ai
