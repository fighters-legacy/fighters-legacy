// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/ISeatController.h" // ISeatController, SeatCommand, SeatView, SeatTurretView
#include "weapon/FireControl.h"     // FireState — the seat's loadout partition + fire memory
#include "weapon/Turret.h"          // TurretState, TurretLimits — the slew servo

#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace fl {

// ── Crew runtime state (#966/#969) ───────────────────────────────────────────
//
// The server-side runtime that turns an EntityDef's authored [[crew]]/[[turrets]] into a live
// per-seat control frame on a ControlledEntity. Lives on the sim thread; parallel access is confined
// to the owning entity's worker slice, so per-seat writes stay disjoint by construction.
//
// The single-seat fast path keys on CrewState::seats being EMPTY — a plain fighter builds no
// CrewState and runs the existing controller/fire path byte-for-byte. CrewState is populated only for
// a genuinely crewed aircraft (more than one authored seat).
//
// LOADOUT PARTITION, NOT A SHARED LOADOUT. The one-owner-per-channel invariant (validateCrewPartition)
// guarantees each hardpoint station is fired by exactly one seat, so a seat's stations are DISJOINT
// from every other seat's. Each seat therefore owns its own FireState (its slice of the loadout +
// its own ammo/edge/rate memory) with no shared-mutable-ammo hazard — which is why the single-seat
// FireControl path needed no refactor. The airframe's payload cost is the sum over seats.

inline constexpr uint32_t kNoSeatPeer = 0xFFFFFFFFu; // the Formation.h "never use 0" rule for peer ids

// One live turret mount: the slew servo pose + limits + the mount rest orientation (as a quat) and
// the hardpoint stations physically on it.
struct CrewTurret {
    TurretState state{};
    TurretLimits limits{};
    glm::quat mountRest{1.f, 0.f, 0.f, 0.f};
    std::vector<int> stations;
};

// One live crew seat.
struct CrewSeat {
    CrewCapabilityMask capabilities{};
    uint32_t occupantPeer{kNoSeatPeer}; // human peer id (B-wave #972/#974); kNoSeatPeer = bot or empty
    bool botOccupied{false};            // #969: a bot fills this seat (seatBot may still be null if unresolved)
    bool isFlySeat{false};              // the one Fly seat — flies via the ControlledEntity::controller, not seatBot
    std::unique_ptr<ISeatController> seatBot; // a NON-fly seat's bot (null for the fly seat / an empty seat)
    FireState fire{};                         // this seat's loadout partition + fire memory (Fire seats)
    int turretIndex{-1};                      // index into CrewState::turrets; -1 = fires along the airframe nose
    float skill{0.5f};
    float reaction{0.5f};
    SeatCommand lastCommand{}; // last sampled command (reused on a decimated tick)
    bool lastCommandValid{false};
};

// The crew of one aircraft. Empty seats vector = the single-seat fast path.
struct CrewState {
    std::vector<CrewSeat> seats;
    std::vector<CrewTurret> turrets;
    // The payload of hardpoints owned by NO seat (inert stores — a drop tank a pilot never fires).
    // The airframe's total payload = base + sum over seats' current partition payloads, so a store
    // released from a seat shrinks the airframe cost and an unfired tank is still counted.
    float baseMassKg{0.f};
    float baseCd0{0.f};

    [[nodiscard]] bool crewed() const noexcept {
        return !seats.empty();
    }
};

} // namespace fl
