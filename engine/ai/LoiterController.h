// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

#include <cstdint>
#include <glm/glm.hpp>

namespace fl::ai {

enum class LoiterDir : uint8_t { Clockwise, CounterClockwise };

// Bank limit for an orbit (#1141). 45 deg turns briskly and stays clear of the attitudes where the
// pitch and roll axes fight each other; it also sets the airspeed the orbit is flyable at
// (turnSpeedForRadius). Shared with DynamicLoiterController, which flies the same geometry.
inline constexpr float kMaxBankRad = 0.785f;

// Orbits a fixed center point at a configurable radius and altitude.
// Direction is caller-specified: Clockwise (default) or CounterClockwise.
//
// The three loops are bank-limited turn, climb-rate altitude hold, and a speed hold trimmed around
// `throttle` (#1141). Each replaced a form that had no feedback on the quantity it was controlling —
// aileron with no bank limit wound the roll up to inverted, an altitude-to-pitch-attitude law flew
// nose-up into the ground, and a fixed throttle let the airspeed run past what the orbit radius can
// be turned at. All three had to go for a loitering entity to still be airborne two minutes later.
class LoiterController : public fl::IEntityController {
  public:
    explicit LoiterController(glm::dvec3 center, float radiusM = 3000.f, float altitudeM = 600.f,
                              float throttle = 0.65f, LoiterDir dir = LoiterDir::Clockwise);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
                            const fl::AiTickContext& /*ctx*/ = {}) override;

  private:
    glm::dvec3 m_center;
    float m_radiusM;
    float m_altitudeM;
    float m_throttle;       // trim throttle; the speed hold trims around it (#1141)
    float m_targetSpeedMps; // airspeed the orbit is flyable at, derived from radius + bank limit
    LoiterDir m_dir;
    // Pitch differentiated across sample intervals for the inner-loop damping (#1141): EntityState
    // exposes no body angular rates, and an undamped pitch loop mushes the aircraft into the ground.
    float m_prevPitchRad{0.f};
    bool m_havePrevPitch{false};
};

} // namespace fl::ai
