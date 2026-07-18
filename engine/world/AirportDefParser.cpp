// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/AirportDefParser.h"

#include <toml++/toml.hpp>

#include <numbers>
#include <stdexcept>
#include <string>

namespace fl {

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;

[[nodiscard]] std::string req_string(toml::node_view<toml::node> node, const char* field) {
    auto v = node.value<std::string>();
    if (!v)
        throw std::runtime_error(std::string("airport: missing required field: ") + field);
    return std::move(*v);
}

[[nodiscard]] double req_double(toml::node_view<toml::node> node, const char* field) {
    auto v = node.value<double>();
    if (!v)
        throw std::runtime_error(std::string("airport: missing required field: ") + field);
    return *v;
}

} // namespace

AirportDef parseAirportDef(std::string_view toml) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("airport: TOML parse error: ") + e.description().data());
    }

    auto airport = tbl["airport"];
    if (!airport.is_table())
        throw std::runtime_error("airport: missing [airport] table");

    AirportDef def;
    def.id = req_string(airport["id"], "airport.id");
    def.name = req_string(airport["name"], "airport.name");

    const bool hasLatLon = airport["lat"] && airport["lon"];
    const bool hasWorldXZ = airport["world_x"] && airport["world_z"];
    if (hasLatLon && hasWorldXZ)
        throw std::runtime_error("airport: specify EITHER lat/lon OR world_x/world_z, not both");
    if (!hasLatLon && !hasWorldXZ)
        throw std::runtime_error("airport: requires lat/lon or world_x/world_z placement");

    if (hasWorldXZ) {
        def.useWorldXZ = true;
        def.worldX = req_double(airport["world_x"], "airport.world_x");
        def.worldZ = req_double(airport["world_z"], "airport.world_z");
    } else {
        def.latRad = req_double(airport["lat"], "airport.lat") * kDegToRad;
        def.lonRad = req_double(airport["lon"], "airport.lon") * kDegToRad;
    }

    if (auto elev = airport["elevation_m"].value<double>())
        def.elevationM = *elev;
    if (auto accepts = airport["accepts_landings"].value<bool>())
        def.acceptsLandings = *accepts;
    if (auto faction = airport["faction"].value<std::string>())
        def.factionId = std::move(*faction);

    if (auto* runways = tbl["runway"].as_array()) {
        for (auto& node : *runways) {
            auto* rwTbl = node.as_table();
            if (!rwTbl)
                throw std::runtime_error("airport: [[runway]] entries must be tables");
            const toml::node_view<toml::node> rw{*rwTbl};

            RunwayDef runway;
            runway.headingDeg = static_cast<float>(req_double(rw["heading_deg"], "runway.heading_deg"));
            runway.lengthM = static_cast<float>(req_double(rw["length_m"], "runway.length_m"));
            runway.widthM = static_cast<float>(req_double(rw["width_m"], "runway.width_m"));
            if (runway.lengthM <= 0.f || runway.widthM <= 0.f)
                throw std::runtime_error("airport: runway length_m/width_m must be positive");

            if (auto surf = rw["surface"].value<std::string>()) {
                if (!runwaySurfaceFromString(*surf, runway.surface))
                    throw std::runtime_error("airport: unknown runway surface '" + *surf + "'");
            } // absent = the RunwayDef default (asphalt)

            def.runways.push_back(runway);
        }
    }

    return def;
}

} // namespace fl
