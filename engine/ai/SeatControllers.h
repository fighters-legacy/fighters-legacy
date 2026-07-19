// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ai/TurretGunnerController.h"
#include "entity/CrewDef.h" // SeatDef, CrewCapability
#include "entity/ISeatController.h"

#include <memory>
#include <string>

namespace fl::ai {

// Build a NON-fly crew seat's bot from its authored SeatDef (#971). This is what fl-server injects
// into WorldBroadcaster::setSeatControllerFactory (engine-net must not link engine-ai, so the concrete
// seat bots reach the broadcaster only through that std::function seam).
//
// A `gunner` / `builtin:gunner` spec — or, by default, any Fire seat that aims a turret — becomes a
// TurretGunnerController. An unrecognised spec returns nullptr (the seat spawns empty, contributing no
// fire; the Fly seat always flies via its IEntityController regardless). The bot rolls its per-instance
// skill within [skillMin, skillMax] from `missionSeed` (#971/#976): buildCrew passes the seat's authored
// default (fixed skill, seed 0); a mission `crew:` block passes the configured range + the mission seed.
[[nodiscard]] inline std::unique_ptr<ISeatController> makeSeatController(const SeatDef& seat, uint8_t /*seatIdx*/,
                                                                         const EntityManager& em, float skillMin,
                                                                         float skillMax, uint64_t missionSeed) {
    const std::string& spec = seat.botSpec;
    const bool wantsGunner =
        spec == "gunner" || spec == "builtin:gunner" ||
        (spec.empty() && hasCapability(seat.capabilities, CrewCapability::Fire) && !seat.turret.empty());
    if (wantsGunner)
        return std::make_unique<TurretGunnerController>(em, skillMin, skillMax, /*engageRangeM=*/1500.f,
                                                        /*muzzleVelMps=*/1000.f, /*lethalRadiusM=*/12.f, missionSeed);
    return nullptr;
}

} // namespace fl::ai
