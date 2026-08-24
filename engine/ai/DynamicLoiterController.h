// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ai/Guidance.h" // LoiterDir + OrbitParams + orbitSteer -- the shared orbit body (#1265)
#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/IEntityController.h"

namespace fl::ai {

// Orbits a MOVING entity: re-centers the loiter circle on the target's live position each tick,
// then flies the same tangent-follow geometry as LoiterController. This is the escort primitive the
// fixed-center LoiterController cannot provide (#464) — the `escort` factory template orbits the
// escorted asset's spawn-time position and drifts off it the moment the asset moves.
//
// The target is read directly via EntityManager::get (the station-keeping pattern shared with
// FormationController), NOT through the sensor contact table: you know where the friendly asset you
// are protecting is. Altitude tracks the target's own altitude, so the escort orbits at the height
// of the thing it is covering.
//
// Returns a neutral ControlInput when the target is dead or invalid (same contract as
// PursuitController).
class DynamicLoiterController : public fl::IEntityController {
  public:
    DynamicLoiterController(const fl::EntityManager& entityManager, fl::EntityId targetId, float radiusM = 3000.f,
                            float throttle = 0.65f, LoiterDir dir = LoiterDir::Clockwise);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
                            const fl::AiTickContext& /*ctx*/ = {}) override;

    void setTarget(fl::EntityId id) noexcept {
        m_targetId = id;
    }

  private:
    const fl::EntityManager& m_entityManager;
    fl::EntityId m_targetId;
    float m_radiusM;
    float m_throttle;       // trim throttle; the speed hold trims around it (#1141)
    float m_targetSpeedMps; // airspeed the orbit is flyable at, from radius + bank limit
    LoiterDir m_dir;
    PitchRateEstimator m_pitchRate; // pitch differentiated for inner-loop damping (#1141)
};

} // namespace fl::ai
