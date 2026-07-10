// SPDX-License-Identifier: GPL-3.0-or-later
#include "ContentBootstrap.h"

#include <ILogger.h>
#include <content/AssetManager.h>
#include <entity/EntityDef.h>
#include <entity/EntityDefParser.h>
#include <entity/EntityTypeRegistry.h>

#include <exception>
#include <limits>
#include <string>
#include <string_view>

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

} // namespace fl
