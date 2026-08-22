// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/AirportDefParser.h"

#include "config/TomlRead.h"
#include "math/Angles.h"

#include <toml++/toml.hpp>

#include <stdexcept>
#include <string>

namespace fl {

namespace {

// Every message this parser raises carries the same context, so bind it once.
constexpr const char* kErr = "airport: ";

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
    def.id = req_string(airport["id"], "airport.id", kErr);
    def.name = req_string(airport["name"], "airport.name", kErr);

    const bool hasLatLon = airport["lat"] && airport["lon"];
    const bool hasWorldXZ = airport["world_x"] && airport["world_z"];
    if (hasLatLon && hasWorldXZ)
        throw std::runtime_error("airport: specify EITHER lat/lon OR world_x/world_z, not both");
    if (!hasLatLon && !hasWorldXZ)
        throw std::runtime_error("airport: requires lat/lon or world_x/world_z placement");

    if (hasWorldXZ) {
        def.useWorldXZ = true;
        def.worldX = req_double(airport["world_x"], "airport.world_x", kErr);
        def.worldZ = req_double(airport["world_z"], "airport.world_z", kErr);
    } else {
        def.latRad = req_double(airport["lat"], "airport.lat", kErr) * kDegToRad<double>;
        def.lonRad = req_double(airport["lon"], "airport.lon", kErr) * kDegToRad<double>;
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
            runway.headingDeg = static_cast<float>(req_double(rw["heading_deg"], "runway.heading_deg", kErr));
            runway.lengthM = static_cast<float>(req_double(rw["length_m"], "runway.length_m", kErr));
            runway.widthM = static_cast<float>(req_double(rw["width_m"], "runway.width_m", kErr));
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
