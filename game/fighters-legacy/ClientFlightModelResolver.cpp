// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClientFlightModelResolver.h"

#include <ILogger.h>
#include <content/AssetManager.h>
#include <entity/EntityDef.h>
#include <entity/EntityTypeRegistry.h>
#include <flight/BuiltinFlightModel.h>
#include <flight/FlightModelData.h>
#include <flight/FlightModelParser.h>

#include <exception>
#include <memory>
#include <string>
#include <string_view>
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

        // flightModelAsset is an ASSET NAME, not a def id (#810) -- it names a file, so it goes
        // straight to the AssetManager without an index lookup.
        auto raw = assets.loadFlightModel(def->flightModelAsset.c_str());
        if (!raw || raw->bytes.empty())
            return fallback("entity type '" + def->id + "' names flight model '" + def->flightModelAsset +
                            "' but no loaded content pack provides it");

        try {
            auto model = std::make_shared<const FlightModelData>(fl::parseFlightModel(
                std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size())));
            (*cache)[typeIndex] = model;
            return model;
        } catch (const std::exception& e) {
            return fallback("flight model '" + def->flightModelAsset + "' for entity type '" + def->id +
                            "' failed to parse: " + e.what());
        }
    };
}

} // namespace fl
