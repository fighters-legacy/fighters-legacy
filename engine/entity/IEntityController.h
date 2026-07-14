// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/AiTickContext.h" // AiTickContext — the bundled per-tick world view
#include "entity/EntityState.h"   // EntityTransform — the SpawnRequest's spawn state (#355)
#include "flight/AeroForces.h"    // ControlInput — the shared control currency
#include "flight/Geodetic.h"      // kEarthRadiusM — default planet radius for local-level guidance

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fl {

struct IEntityController;

// A controller's request to bring a NEW entity into the world (#355 — MIRV bus deploying RVs).
// Controllers are sampled in a data-parallel, read-only pass and can never spawn directly:
// EntityManager::spawn mutates the pool and fires handlers. So a controller RAISES this intent and
// WorldBroadcaster drains it serially in the weapons phase — the same discipline as over-G damage.
struct SpawnRequest {
    std::string typeId;        // entity type to spawn; EMPTY = "the same type as the requester"
    EntityTransform transform; // world spawn state (position, velocity, orientation)
    // Controller for the child; REQUIRED — a spawned vehicle with no controller would never be
    // integrated. Called on the sim thread at execution time.
    std::function<std::unique_ptr<IEntityController>()> makeController;
};

// Source of per-tick control inputs for a single simulated entity. Decouples the flight sim from the
// network-peer assumption: WorldBroadcaster keeps an EntityId-keyed registry of controllers and steps
// every one each tick, with no special-casing for who (or what) is flying. A connected player is a
// PeerController (wraps the latest MsgClientInput); a future server-side AiController (issue #350) or
// scripted LuaController (#357) registers exactly the same way with zero onTick changes. The integrator
// (and thus MsgWorldSnapshot serialisation) already treats every entity uniformly, so AI/scripted
// entities broadcast to clients for free.
struct IEntityController {
    virtual ~IEntityController() = default;

    // Produce this tick's control inputs for the given entity. tick is the sim tick index; dt is the
    // step duration in seconds. ctx is everything the controller is allowed to know about the world
    // this tick (spatial index, detected contacts, environment, difficulty) — see AiTickContext.h;
    // each of its fields may be null, meaning "not evaluated in the context you were called from",
    // and a default-constructed context is the behavior every controller had before the bundle
    // existed. Called on the sim thread inside WorldBroadcaster::onTick.
    virtual ControlInput sample(const EntityState& state, uint64_t tick, double dt, const AiTickContext& ctx = {}) = 0;

    // Spawn intents raised since the last drain (#355). WorldBroadcaster drains this SERIALLY in
    // the weapons phase — never from the parallel AI pass — and executes each request through the
    // ordinary spawn + controller-registration path with the requester's ownership chained onto
    // the child (a MIRV kill credits whoever launched the bus). Default: nothing to say.
    virtual std::vector<SpawnRequest> drainSpawnRequests() {
        return {};
    }

    // Planet radius (m) for local-level (tangent-plane) guidance math. Defaults to Earth so the
    // controllers and unit tests behave correctly near the world origin without any wiring.
    // WorldBroadcaster sets this from its configured planet radius when it takes ownership of a
    // controller (see addControlledEntity), so AI flies correctly far from the origin on any
    // planet. Composite controllers (e.g. StateMachineController) forward it to their children.
    void setPlanetRadius(double radiusM) noexcept {
        m_planetRadiusM = radiusM;
    }
    [[nodiscard]] double planetRadiusM() const noexcept {
        return m_planetRadiusM;
    }

  protected:
    double m_planetRadiusM{kEarthRadiusM};
};

} // namespace fl
