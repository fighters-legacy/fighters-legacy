// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

#include <cstdint>
#include <glm/glm.hpp>

namespace fl::ai {

enum class LoiterDir : uint8_t { Clockwise, CounterClockwise };

// Orbits a fixed center point at a configurable radius and altitude.
// Direction is caller-specified: Clockwise (default) or CounterClockwise.
class LoiterController : public fl::IEntityController {
  public:
    explicit LoiterController(glm::dvec3 center, float radiusM = 3000.f, float altitudeM = 600.f,
                              float throttle = 0.65f, LoiterDir dir = LoiterDir::Clockwise);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                            const fl::SpatialIndex* /*si*/ = nullptr) override;

  private:
    glm::dvec3 m_center;
    float m_radiusM;
    float m_altitudeM;
    float m_throttle;
    LoiterDir m_dir;
};

} // namespace fl::ai
