// SPDX-License-Identifier: GPL-3.0-or-later
#include "ContentBootstrap.h"

#include <ILogger.h>
#include <content/AssetManager.h>
#include <content/ContentIndex.h>
#include <entity/EntityDef.h>
#include <entity/EntityDefParser.h>
#include <entity/EntityTypeRegistry.h>
#include <sensor/SensorDef.h>
#include <sensor/SensorDefParser.h>
#include <weapon/WeaponDef.h>
#include <weapon/WeaponDefParser.h>
#include <weapon/WeaponRegistry.h>

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

} // namespace fl
