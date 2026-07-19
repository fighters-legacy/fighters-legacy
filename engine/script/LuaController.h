// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/IEntityController.h"

#include <memory>
#include <string>
#include <string_view>

namespace fl {

class EntityManager;
struct WorldApi;
namespace atc {
class AtcService; // engine/atc/AtcService.h — the atc.* Lua module target (#705)
} // namespace atc

// IEntityController backed by a sandboxed Lua 5.5 script.
//
// The script defines ONE of two control-flow entry points:
//   function compute_control(state, tick, dt) → table         (the function-call model)
//   function ai_main()                                        (the coroutine model, #412)
//
// Function-call model: the engine calls compute_control() each sim tick (60 Hz) and maps the
// returned table fields to ControlInput. Missing or non-numeric fields default to 0/false. If the
// function is missing or throws, a neutral ControlInput{} is returned and the error is logged to
// stderr at most once per 60 ticks.
//
// Coroutine model (#412): if the script defines `ai_main`, it is driven as a Lua coroutine resumed
// once per tick, so authors can write sequential state machines. The engine resumes ai_main with
// (state, tick, dt); the coroutine returns a control table for the tick via `coroutine.yield(ctrl)`,
// which also suspends it until the next tick (yield's return values are the next (state, tick, dt)).
// A yield with no value → neutral that tick. When ai_main returns (or errors), the behavior is
// finished and every subsequent tick is neutral. ai_main takes precedence if both are defined.
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
//
//   atc.* module (#705; requires an AtcService, else every call is nil/false):
//     atc.clearance()                        → clearance-state string for THIS entity ("none" if unknown)
//     atc.request_takeoff([airport_id])      → true; queue this entity for departure (nearest field)
//     atc.request_landing([airport_id])      → true; sequence this entity for arrival
//     atc.inbound([airport_id])              → true; declare inbound (radio acknowledgement)
//     atc.scramble(airport_id, type, count)  → bool; launch `count` departures from a named field
//     atc.hold(airport_id, on)               → true; freeze/unfreeze that field's departures
class LuaController : public IEntityController {
  public:
    // scriptSource: Lua source text (never bytecode — rejected by LuaSandbox)
    // packRootDir: passed to LuaSandbox to restrict require() to ai/<module>.lua
    // entityManager: optional; enables get_entity() Lua binding (sim-thread-only)
    // worldApi: optional host seam for the world.* module (#413); null = world.* engine-integration
    //   calls (spawn/despawn/set_relationship/set_music_state/mission_*) are safe no-ops. The pointee
    //   must outlive the controller (the host owns one WorldApi for the sim's lifetime).
    // atcService: optional; enables the atc.* module (#705). Thread-safe (the service takes its own
    //   lock), so a script calling atc.* from the parallel AI pass is safe. Must outlive the controller.
    LuaController(std::string_view scriptSource, std::string packRootDir, const EntityManager* entityManager = nullptr,
                  const WorldApi* worldApi = nullptr, atc::AtcService* atcService = nullptr);
    ~LuaController();

    ControlInput sample(const EntityState& state, uint64_t tick, double dt, const AiTickContext& ctx = {}) override;

    // False if LuaSandbox::create() or loadScript() failed at construction.
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] const std::string& lastError() const;

    // Opaque implementation — accessible to C closure callbacks that hold
    // a lightuserdata pointer to it.
    struct Impl;

  private:
    // Coroutine (#412) tick driver: resume ai_main, returning its yielded control table. Assumes the
    // ctx/tick/dt have already been stashed on Impl for the C bindings (sample() does this).
    ControlInput sampleCoroutine(const EntityState& state, uint64_t tick, double dt);

    std::unique_ptr<Impl> m_impl;
};

} // namespace fl
