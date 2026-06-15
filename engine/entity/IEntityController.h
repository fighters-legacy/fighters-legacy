// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/AeroForces.h" // ControlInput — the shared control currency

#include <cstdint>

namespace fl {

struct EntityState; // engine/entity/EntityState.h

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
    // step duration in seconds. Called on the sim thread inside WorldBroadcaster::onTick.
    virtual ControlInput sample(const EntityState& state, uint64_t tick, double dt) = 0;
};

} // namespace fl
