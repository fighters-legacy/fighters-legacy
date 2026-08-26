// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClientFlightModelResolver.h"

#include <ILogger.h>
#include <content/AssetManager.h>
#include <entity/EntityDef.h>
#include <entity/EntityTypeRegistry.h>
#include <flight/BuiltinFlightModel.h>
#include <flight/FlightModelData.h>
#include <flight/FlightModelLoad.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace fl {

ClientPrediction::FlightModelResolver makeFlightModelResolver(const EntityTypeRegistry& registry, AssetManager& assets,
                                                              ILogger& log) {
    auto cache = std::make_shared<std::unordered_map<uint32_t, std::shared_ptr<const FlightModelData>>>();
    auto lastGen = std::make_shared<uint64_t>(assets.cacheGeneration());

    return [&registry, &assets, &log, cache, lastGen](uint32_t typeIndex) -> std::shared_ptr<const FlightModelData> {
        // Hot-reload (#152): if the AssetManager evicted anything since we last resolved, drop the whole
        // per-type cache so a changed flight-model TOML is re-parsed on the next lookup.
        if (const uint64_t gen = assets.cacheGeneration(); gen != *lastGen) {
            *lastGen = gen;
            cache->clear();
        }
        if (auto it = cache->find(typeIndex); it != cache->end())
            return it->second;

        // Every path out of here that is NOT the pack's own model says so, at Error, by name. The
        // client silently flying a different aeroplane from the server is precisely the failure this
        // function exists to make impossible.
        auto fallback = [&](const std::string& why) {
            log.log(LogLevel::Error, __FILE__, __LINE__,
                    (why + " -- falling back to the builtin flight model; client-side prediction will NOT match the "
                           "server for this entity type")
                        .c_str());
            std::shared_ptr<const FlightModelData> builtin = BuiltinFlightModel::get();
            (*cache)[typeIndex] = builtin;
            return builtin;
        };

        const EntityDef* def = registry.byIndex(typeIndex);
        if (!def)
            return fallback("no entity def for type index " + std::to_string(typeIndex));

        if (def->flightModelAsset.empty()) {
            // Not an error: a type with no flight model (the builtin debug entity) is meant to fly
            // the builtin model, and both sides agree on that. Cache it quietly.
            std::shared_ptr<const FlightModelData> builtin = BuiltinFlightModel::get();
            (*cache)[typeIndex] = builtin;
            return builtin;
        }

        // The WHOLE "builtin:" prefix resolves through the same compiled-in authority the server
        // uses (#1335) — the client had NO builtin intercept before, so a piloted builtin:carrier
        // loaded nothing zero-pack and PREDICTED ON THE WRONG MODEL, the exact divergence this
        // function exists to make impossible. An unknown builtin name errors like malformed content.
        if (def->flightModelAsset.rfind("builtin:", 0) == 0) {
            if (auto builtin = builtinFlightModel(def->flightModelAsset)) {
                (*cache)[typeIndex] = builtin;
                return builtin;
            }
            return fallback("entity type '" + def->id + "': unknown builtin flight model '" + def->flightModelAsset +
                            "'");
        }

        // flightModelAsset is an ASSET NAME, not a def id (#810) -- it names a file, so it goes
        // straight to the AssetManager without an index lookup. The load-and-parse step is shared
        // with the server spawn path (#1232) so both sides refuse malformed content the same way.
        auto res = loadAndParseFlightModel(assets, def->flightModelAsset.c_str());
        if (!res.model)
            return fallback("entity type '" + def->id + "': " + res.error);
        (*cache)[typeIndex] = res.model;
        return res.model;
    };
}

} // namespace fl
