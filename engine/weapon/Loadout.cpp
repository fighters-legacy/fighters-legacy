// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon/Loadout.h"

#include "ILogger.h"
#include "entity/EntityDef.h"
#include "weapon/WeaponDef.h"
#include "weapon/WeaponRegistry.h"

#include <algorithm>
#include <string>

namespace fl {
namespace {

// The two enums are parallel by design -- except for Fuel, which has no weapon counterpart at all
// (see Loadout.h). Returns false for Fuel; callers must have skipped it already.
bool hardpointAccepts(HardpointType station, WeaponType store) noexcept {
    switch (station) {
    case HardpointType::Missile:
        return store == WeaponType::Missile;
    case HardpointType::Bomb:
        return store == WeaponType::Bomb;
    case HardpointType::Rocket:
        return store == WeaponType::Rocket;
    case HardpointType::Gun:
        return store == WeaponType::Gun;
    case HardpointType::Pod:
        return store == WeaponType::Pod;
    case HardpointType::Fuel:
        return false; // no WeaponType::Fuel exists -- a drop tank is not a weapon
    }
    return false;
}

const char* hardpointTypeName(HardpointType t) noexcept {
    switch (t) {
    case HardpointType::Missile:
        return "missile";
    case HardpointType::Bomb:
        return "bomb";
    case HardpointType::Rocket:
        return "rocket";
    case HardpointType::Gun:
        return "gun";
    case HardpointType::Fuel:
        return "fuel";
    case HardpointType::Pod:
        return "pod";
    }
    return "?";
}

} // namespace

PayloadEffect defaultPayload(const EntityDef& def, const WeaponRegistry& weapons, ILogger& log) {
    PayloadEffect payload{};

    for (const Hardpoint& hp : def.hardpoints) {
        const std::string station = "entity '" + def.id + "' hardpoint " + std::to_string(hp.slot);

        // A fuel station carries a drop tank, which is not a weapon and is not in the registry.
        // Skipped silently -- this is the normal case, not a misconfiguration.
        if (hp.type == HardpointType::Fuel)
            continue;

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

        if (!hardpointAccepts(hp.type, store->type)) {
            log.log(LogLevel::Error, __FILE__, __LINE__,
                    (station + " is a '" + hardpointTypeName(hp.type) + "' station but its default store '" +
                     hp.defaultWeapon + "' is not that kind of weapon; the station will be empty")
                        .c_str());
            continue;
        }

        payload.extra_mass_kg += store->load.massKg;
        payload.extra_cd0 += store->load.dragFactor;
    }

    return payload;
}

} // namespace fl
