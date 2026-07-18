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
// fire; the Fly seat always flies via its IEntityController regardless). `missionSeed` seeds the
// per-instance skill roll (0 until the mission runtime provides one, #976).
[[nodiscard]] inline std::unique_ptr<ISeatController>
makeSeatController(const SeatDef& seat, uint8_t /*seatIdx*/, const EntityManager& em, uint64_t missionSeed = 0) {
    const std::string& spec = seat.botSpec;
    const bool wantsGunner =
        spec == "gunner" || spec == "builtin:gunner" ||
        (spec.empty() && hasCapability(seat.capabilities, CrewCapability::Fire) && !seat.turret.empty());
    if (wantsGunner) {
        const float skill = seat.defaultSkill; // a mission range overrides this in #976
        return std::make_unique<TurretGunnerController>(em, skill, skill, /*engageRangeM=*/1500.f,
                                                        /*muzzleVelMps=*/1000.f, /*lethalRadiusM=*/12.f, missionSeed);
    }
    return nullptr;
}

} // namespace fl::ai
