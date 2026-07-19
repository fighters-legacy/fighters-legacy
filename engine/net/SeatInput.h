// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/CrewDef.h" // CrewCapabilityMask, CrewCapability, hasCapability

#include <cstdint>

namespace fl {

// ── Seat-scoped client input routing (#966/#972) ─────────────────────────────
//
// A human occupant of a crew seat sends the SAME unmodified 80-byte MsgClientInput every client
// sends — the server does not trust the client to zero the channels its seat does not own. Instead the
// server MASKS the raw input by the seat's capabilities: a gunner's elevator/aileron/throttle are
// ignored (never reach the flight integrator), its viewAxis becomes the commanded turret direction,
// and its selectedStation is clamped to the seat's own station partition. The one-owner-per-channel
// invariant (validateCrewPartition) guarantees these routings never conflict across an aircraft's
// seats, so the masked merge is a straight per-channel pick with no arbitration.
//
// This is PURE routing policy over the capability mask, kept header-only and dependency-light so it is
// unit-tested in isolation and reused verbatim by the server's crew fire/sample passes.

// Which channels of a raw MsgClientInput the occupant of a seat with `caps` (and, for a Fire seat,
// whether it `aimsTurret`) actually drives. Every field defaults false: a seat drives a channel only
// when it owns the capability.
struct SeatInputRouting {
    bool driveFlight{false};          // Fly: elevator/aileron/rudder/throttle/airbrake/AB reach the integrator
    bool driveFire{false};            // Fire: trigger/release + selectedStation reach the seat's fire state
    bool aimTurret{false};            // a Fire seat that aims a turret: viewAxis is the turret command
    bool driveRadar{false};           // Radar: radarMode applies (#526/#587)
    bool driveCountermeasures{false}; // Countermeasures: the CM-dispense button applies (#529)
};

[[nodiscard]] constexpr SeatInputRouting seatInputRouting(CrewCapabilityMask caps, bool aimsTurret) noexcept {
    SeatInputRouting r;
    r.driveFlight = hasCapability(caps, CrewCapability::Fly);
    r.driveFire = hasCapability(caps, CrewCapability::Fire);
    r.aimTurret = r.driveFire && aimsTurret;
    r.driveRadar = hasCapability(caps, CrewCapability::Radar);
    r.driveCountermeasures = hasCapability(caps, CrewCapability::Countermeasures);
    return r;
}

// Clamp a client-requested absolute station selection to a seat's station partition. `selected` is the
// MsgClientInput::selectedStation value (255 = keep); `count` is the number of stations the seat owns.
// Returns 255 (keep) when the request is out of the seat's range, so a gunner cannot select the
// pilot's stores by sending a large index. A seat with no stations rejects every selection.
[[nodiscard]] constexpr uint8_t clampSeatStation(uint8_t selected, std::size_t count) noexcept {
    if (selected == 255u)
        return 255u; // "keep current" always passes through
    return (static_cast<std::size_t>(selected) < count) ? selected : 255u;
}

} // namespace fl
