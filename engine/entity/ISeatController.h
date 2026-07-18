// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/AiTickContext.h" // the bundled per-tick world view (contacts, difficulty, ...)
#include "entity/CrewDef.h"       // CrewCapabilityMask
#include "entity/EntityState.h"   // the airframe state a seat rides on
#include "flight/Geodetic.h"      // kEarthRadiusM

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>

namespace fl {

// ── Crew seat control (#966/#969) ────────────────────────────────────────────
//
// A crew SEAT that does not fly is driven by an ISeatController, deliberately NARROWER than
// IEntityController: a gunner does not produce a full ControlInput (no elevator/aileron/throttle) —
// only a SeatCommand (where to aim its turret and whether to fire). The Fly seat still uses an
// IEntityController (it flies the airframe); the masked merge in WorldBroadcaster takes flight from
// the Fly seat and each fire channel from its owning seat. This narrowness is why the rejected
// "CrewController : IEntityController facade" could not work — per-seat fire cannot cross a single
// ControlInput return.

// What the seat's turret looks like to its bot, as plain data (no engine-weapon type here, so this
// header stays in engine-entity). A bot aims at a world point; the server turret servo (stepTurret)
// clamps and slews toward it, so the bot only needs the limits to decide whether a target is even
// reachable within its arc.
struct SeatTurretView {
    bool present{false};
    glm::quat mountRest{1.f, 0.f, 0.f, 0.f}; // body-frame rest orientation of the mount
    float azMinRad{-3.14159265f};
    float azMaxRad{3.14159265f};
    float elMinRad{-0.0872665f};
    float elMaxRad{1.4835299f};
    // The turret's CURRENT world-space bore this tick (from the previous slew) — so a bot can hold
    // fire until the servo has actually pointed the gun at its target, not while it is still slewing.
    glm::vec3 boreWorld{1.f, 0.f, 0.f};
};

// The per-seat view handed to an ISeatController each tick.
struct SeatView {
    uint8_t seatIndex{0};
    CrewCapabilityMask capabilities{};
    float skill{0.5f};    // per-instance rolled skill [0,1] (#971) — higher aims tighter, reacts sooner
    float reaction{0.5f}; // per-instance reaction [0,1] — higher is slower
    SeatTurretView turret{};
};

// A seat bot's output for one tick. WeaponControls-shaped fire fields (trigger/release/station) plus
// a turret aim direction in WORLD space. Radar mode / target designation are reserved for the Radar
// seat (#587/#972); a seat contributes only the channels its capabilities own.
struct SeatCommand {
    bool hasAim{false};                   // true when aimDirWorld is meaningful (a turret/Fire seat)
    glm::vec3 aimDirWorld{1.f, 0.f, 0.f}; // where the seat wants its turret to point (world frame)
    bool trigger{false};                  // gun trigger (level)
    bool release{false};                  // fire the selected store (edge-detected downstream)
    uint8_t station{255};                 // absolute selected station within the seat's partition; 255 = keep
};

// Control source for one non-flying crew seat. Sampled in the AI pass inside the entity's worker
// slice (writes stay disjoint per entity); the seat-bot dice hash (entityIdx, seatIdx, tick) keep it
// serial-equivalent. Narrower than IEntityController on purpose (a seat bot does not fly).
struct ISeatController {
    virtual ~ISeatController() = default;

    // Produce this seat's command for `tick`. `airframe` is the parent aircraft's pre-step state;
    // `seat` is this seat's capabilities + turret limits; `ctx` is the shared honest world view (the
    // same contact table the airframe sees — a jammed/blind airframe's gunner sees nothing).
    virtual SeatCommand sample(const EntityState& airframe, const SeatView& seat, uint64_t tick, double dt,
                               const AiTickContext& ctx = {}) = 0;

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
