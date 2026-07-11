// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/AeroForces.h" // ControlInput — the shared control currency
#include "flight/Geodetic.h"   // kEarthRadiusM — default planet radius for local-level guidance

#include <cstdint>

namespace fl {

struct EntityState; // engine/entity/EntityState.h
class SpatialIndex; // engine/spatial/SpatialIndex.h — pointer only; no include needed here

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
    // step duration in seconds. si is the spatial index rebuilt at the start of this tick by
    // WorldBroadcaster — non-null when called from the broadcaster, nullptr in tests and other
    // contexts. Called on the sim thread inside WorldBroadcaster::onTick.
    virtual ControlInput sample(const EntityState& state, uint64_t tick, double dt,
                                const SpatialIndex* si = nullptr) = 0;

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
