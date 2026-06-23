// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

#include <memory>
#include <string>
#include <string_view>

namespace fl {

class EntityManager;
class SpatialIndex;

// IEntityController backed by a sandboxed Lua 5.5 script.
//
// The script must define:
//   function compute_control(state, tick, dt) → table
//
// The engine calls compute_control() each sim tick (60 Hz) and maps the
// returned table fields to ControlInput. Missing or non-numeric fields default
// to 0/false. If the function is missing or throws, a neutral ControlInput{}
// is returned and the error is logged to stderr at most once per 60 ticks.
//
// Globals registered before loadScript() completes:
//   guidance.heading_error(quat, own_pos, target_pos)   → number (radians)
//   guidance.pitch_error_from_alt(quat, alt_error_m)   → number (radians)
//   guidance.bank_to_turn_aileron(heading_error_rad)   → number [-1,1]
//   guidance.coordinated_rudder(aileron)               → number [-1,1]
//   guidance.elevator_from_pitch_error(pitch_error)    → number [-1,1]
//   guidance.body_forward(quat)                        → {x, y, z}
//
//   nearby_entities(cx, cz, radius_m) → array of {idx, pos={x,y,z}}
//       (valid only inside compute_control; returns {} when SpatialIndex unavailable)
//   get_entity(idx)                   → state table or nil
//       (requires entityManager; returns nil when unavailable or entity dead)
class LuaController : public IEntityController {
  public:
    // scriptSource: Lua source text (never bytecode — rejected by LuaSandbox)
    // packRootDir: passed to LuaSandbox to restrict require() to ai/<module>.lua
    // entityManager: optional; enables get_entity() Lua binding (sim-thread-only)
    LuaController(std::string_view scriptSource, std::string packRootDir, const EntityManager* entityManager = nullptr);
    ~LuaController();

    ControlInput sample(const EntityState& state, uint64_t tick, double dt, const SpatialIndex* si = nullptr) override;

    // False if LuaSandbox::create() or loadScript() failed at construction.
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] const std::string& lastError() const;

    // Opaque implementation — accessible to C closure callbacks that hold
    // a lightuserdata pointer to it.
    struct Impl;

  private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
