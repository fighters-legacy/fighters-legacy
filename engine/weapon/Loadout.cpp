// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon/Loadout.h"

#include "ILogger.h"
#include "entity/EntityDef.h"
#include "weapon/WeaponDef.h"
#include "weapon/WeaponRegistry.h"

#include <algorithm>
#include <string>

namespace fl {
namespace {} // namespace

PayloadEffect defaultPayload(const EntityDef& def, const WeaponRegistry& weapons, ILogger& log) {
    PayloadEffect payload{};

    for (const Hardpoint& hp : def.hardpoints) {
        const std::string station = "entity '" + def.id + "' hardpoint " + std::to_string(hp.slot);

        if (hp.defaultWeapon.empty())
            continue; // an empty station is a legitimate loadout choice

        const WeaponDef* store = weapons.findById(hp.defaultWeapon);
        if (!store) {
            log.log(LogLevel::Error, __FILE__, __LINE__,
                    (station + ": default store '" + hp.defaultWeapon +
                     "' is not a known weapon id; the station will be empty")
                        .c_str());
            continue;
        }

        // `allowed` is what the AIRFRAME says it can carry. A default outside it is a content bug,
        // and the pack validator catches it -- but the engine must not silently honour it either.
        if (std::find(hp.allowed.begin(), hp.allowed.end(), hp.defaultWeapon) == hp.allowed.end()) {
            log.log(LogLevel::Error, __FILE__, __LINE__,
                    (station + ": default store '" + hp.defaultWeapon +
                     "' is not in this station's allowed list; the station will be empty")
                        .c_str());
            continue;
        }

        payload.extra_mass_kg += store->load.massKg;
        payload.extra_cd0 += store->load.dragFactor;
    }

    return payload;
}

} // namespace fl
