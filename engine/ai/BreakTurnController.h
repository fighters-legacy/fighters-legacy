// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/IEntityController.h"

namespace fl::ai {

// Two-phase defensive air combat maneuver:
//   Phase 1 (Roll): bank toward the threat bearing for rollPhaseDurationS seconds.
//   Phase 2 (Pull): maximum-G elevator pull with afterburner (runs indefinitely).
// Returns neutral ControlInput when the threat is dead or invalid.
class BreakTurnController : public fl::IEntityController {
  public:
    BreakTurnController(const fl::EntityManager& entityManager, fl::EntityId threatId, float rollPhaseDurationS = 0.5f,
                        float maxElevator = 1.f);

    // dt is used to accumulate the roll-phase timer.
    fl::ControlInput sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
                            const fl::SpatialIndex* /*si*/ = nullptr) override;

  private:
    enum class Phase { Roll, Pull };

    const fl::EntityManager& m_entityManager;
    fl::EntityId m_threatId;
    float m_rollPhaseDuration;
    float m_maxElevator;

    Phase m_phase{Phase::Roll};
    float m_rollTimer{0.f};
};

} // namespace fl::ai
