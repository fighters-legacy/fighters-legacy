// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/ContentBootstrap.h"

#include "ILogger.h"
#include "content/AssetManager.h"
#include "content/ContentIndex.h"
#include "entity/EntityDef.h"
#include "entity/EntityDefParser.h"
#include "entity/EntityTypeRegistry.h"
#include "sensor/BuiltinSensors.h"
#include "sensor/SensorDef.h"
#include "sensor/SensorDefParser.h"
#include "weapon/BuiltinWeapon.h"    // the sandbox loadout (#440)
#include "weapon/ProjectileSystem.h" // projectileTypeId (#625)
#include "weapon/WeaponDef.h"
#include "weapon/WeaponDefParser.h"
#include "weapon/WeaponRegistry.h"

#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fl {

uint32_t registerPackEntityDefs(AssetManager& assets, EntityTypeRegistry& registry, ILogger& log) {
    uint32_t registered = 0;
    for (const auto& name : assets.listAssets(AssetType::EntityDef)) {
        auto raw = assets.loadEntityDef(name.c_str());
        if (!raw || raw->bytes.empty()) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("entity def '") + name + "' could not be loaded; skipping").c_str());
            continue;
        }
        try {
            EntityDef def =
                parseEntityDef(std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size()));
            const std::string id = def.id;
            if (registry.registerType(std::move(def)) == std::numeric_limits<uint32_t>::max())
                log.log(LogLevel::Warn, __FILE__, __LINE__,
                        (std::string("entity def id '") + id + "' already registered; skipping duplicate").c_str());
            else
                ++registered;
        } catch (const std::exception& e) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("entity def '") + name + "' parse error: " + e.what() + "; skipping").c_str());
        }
    }
    return registered;
}

uint32_t registerPackWeaponDefs(AssetManager& assets, WeaponRegistry& registry, ILogger& log) {
    uint32_t registered = 0;
    for (const auto& name : assets.listAssets(AssetType::Weapon)) {
        auto raw = assets.loadWeaponDef(name.c_str());
        if (!raw || raw->bytes.empty()) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("weapon def '") + name + "' could not be loaded; skipping").c_str());
            continue;
        }
        try {
            WeaponDef def =
                parseWeaponDef(std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size()));
            if (def.seeker && def.seeker->usesLegacyLobe())
                log.log(LogLevel::Warn, __FILE__, __LINE__,
                        (std::string("weapon def '") + name +
                         "' uses the deprecated [seeker] fov_deg/acquisition_nm lobe — reference a "
                         "sensor def with sensor_id (2026-07-14 decision record); the legacy form is "
                         "removed after one release")
                            .c_str());
            const std::string id = def.id;
            if (registry.registerWeapon(std::move(def)) == std::numeric_limits<uint32_t>::max())
                log.log(LogLevel::Warn, __FILE__, __LINE__,
                        (std::string("weapon def id '") + id + "' already registered; skipping duplicate").c_str());
            else
                ++registered;
        } catch (const std::exception& e) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("weapon def '") + name + "' parse error: " + e.what() + "; skipping").c_str());
        }
    }
    return registered;
}

SensorDefResolver makeSensorDefResolver(AssetManager& assets, const ContentIndex& index, ILogger& log) {
    // Shared so the cache survives the copy into WorldBroadcaster's std::function.
    auto cache = std::make_shared<std::unordered_map<std::string, std::shared_ptr<const sensor::SensorDef>>>();

    return [&assets, &index, &log, cache](const std::string& id) -> std::shared_ptr<const sensor::SensorDef> {
        if (auto it = cache->find(id); it != cache->end())
            return it->second;

        // Builtin ids resolve to the compiled-in defs — no pack, no file, no error (#440/#627).
        // Checked BEFORE the index, deliberately: "builtin:" is engine vocabulary, and a pack that
        // shadowed it could make the sandbox seeker mean something different per server. Non-owning
        // pointers via the aliasing constructor — the statics outlive everything.
        for (const sensor::SensorDef* builtin :
             {&sensor::BuiltinSensors::eyeball(), &sensor::BuiltinSensors::irSeeker(),
              &sensor::BuiltinSensors::radarSeeker()}) {
            if (id == builtin->id) {
                std::shared_ptr<const sensor::SensorDef> def(std::shared_ptr<const sensor::SensorDef>{}, builtin);
                (*cache)[id] = def;
                return def;
            }
        }

        std::shared_ptr<const sensor::SensorDef> def;
        const std::string* assetName = index.assetNameFor(AssetType::SensorDef, id);

        if (!assetName) {
            log.log(LogLevel::Error, __FILE__, __LINE__,
                    ("unknown sensor def id '" + id +
                     "': no loaded pack declares it -- the entity will fly without this sensor")
                        .c_str());
        } else if (auto raw = assets.loadSensorDef(assetName->c_str()); raw && !raw->bytes.empty()) {
            try {
                def = std::make_shared<const sensor::SensorDef>(sensor::parseSensorDef(
                    std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size())));
            } catch (const std::exception& e) {
                log.log(LogLevel::Error, __FILE__, __LINE__,
                        ("sensor def '" + id + "' (asset '" + *assetName + "') failed to parse: " + e.what()).c_str());
            }
        } else {
            log.log(LogLevel::Error, __FILE__, __LINE__,
                    ("sensor def '" + id + "' is indexed as asset '" + *assetName + "' but the asset failed to load")
                        .c_str());
        }

        (*cache)[id] = def; // cache misses too, so a bad id isn't re-reported on every spawn
        return def;
    };
}

uint32_t registerProjectileEntityDefs(const WeaponRegistry& weapons, EntityTypeRegistry& registry, ILogger& log) {
    uint32_t registered = 0;
    for (uint32_t i = 0;; ++i) {
        const WeaponDef* w = weapons.byIndex(i);
        if (!w)
            break;
        if (w->type == WeaponType::Gun || w->type == WeaponType::Pod)
            continue; // hitscan / non-flying stores never become entities

        EntityDef def;
        def.id = projectileTypeId(*w);
        def.name = w->name;
        def.category = ObjectCategory::Projectile;
        def.maxHp = 1.f;
        def.mesh = w->mesh; // ASSET NAME; empty = the builtin placeholder
        // A missile is a hard radar target to SEE: small RCS, hot IR while the motor burns (the
        // static signature approximates the burn — per-phase signatures can come with #529).
        def.signatures.rcs = 0.1f;
        def.signatures.visual = 0.3f;
        def.signatures.ir = 2.0f;

        if (registry.registerType(std::move(def)) == std::numeric_limits<uint32_t>::max()) {
            log.log(LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("projectile type for weapon '") + w->id + "' already registered; skipping").c_str());
        } else {
            ++registered;
        }
    }
    return registered;
}

uint32_t registerBuiltinWeapons(WeaponRegistry& registry) {
    uint32_t registered = 0;
    for (const WeaponDef* w : {&BuiltinWeapon::cannon(), &BuiltinWeapon::irMissile(), &BuiltinWeapon::radarMissile()}) {
        if (registry.registerWeapon(*w) != std::numeric_limits<uint32_t>::max())
            ++registered;
    }
    return registered;
}

EntityDef builtinDebugEntityDef() {
    EntityDef def;
    def.id = "builtin:debug-entity";
    def.name = "Debug Entity";
    def.category = ObjectCategory::AirVehicle;
    def.maxHp = 100.0f;

    // Armed (#440): one cannon, two IR rails, two radar rails — every sandbox/debug peer spawns
    // able to exercise the whole fire path with zero content mounted.
    auto hp = [](int slot, HardpointType type, const char* weapon) {
        Hardpoint h;
        h.slot = slot;
        h.type = type;
        h.allowed = {weapon};
        h.defaultWeapon = weapon;
        return h;
    };
    def.hardpoints = {
        hp(0, HardpointType::Gun, BuiltinWeapon::cannon().id.c_str()),
        hp(1, HardpointType::Missile, BuiltinWeapon::irMissile().id.c_str()),
        hp(2, HardpointType::Missile, BuiltinWeapon::irMissile().id.c_str()),
        hp(3, HardpointType::Missile, BuiltinWeapon::radarMissile().id.c_str()),
        hp(4, HardpointType::Missile, BuiltinWeapon::radarMissile().id.c_str()),
    };
    return def;
}

} // namespace fl
