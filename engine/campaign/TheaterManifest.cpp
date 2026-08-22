// SPDX-License-Identifier: GPL-3.0-or-later
#include "campaign/TheaterManifest.h"

#include "math/Angles.h"

#include <string>
#include <toml++/toml.hpp>

namespace fl {

namespace {}

TheaterManifestParseResult parseTheaterManifest(std::string_view tomlContent) {
    TheaterManifestParseResult r;
    toml::table tbl;
    try {
        tbl = toml::parse(tomlContent);
    } catch (const toml::parse_error& e) {
        r.errors.push_back(std::string("theater: TOML parse error: ") + e.description().data());
        r.ok = false;
        return r;
    }

    auto theater = tbl["theater"];
    if (!theater.is_table()) {
        r.errors.push_back("theater: missing [theater] table");
        r.ok = false;
        return r;
    }

    if (auto id = theater["id"].value<std::string>(); id && !id->empty())
        r.theater.id = *id;
    else
        r.errors.push_back("theater: [theater].id is required");

    if (auto name = theater["name"].value<std::string>())
        r.theater.name = *name;
    if (auto layer = theater["layer"].value<std::string>())
        r.theater.layer = *layer;
    if (auto terrain = theater["terrain"].value<std::string>(); terrain && !terrain->empty())
        r.theater.terrain = *terrain;

    auto bounds = theater["bounds"];
    if (!bounds.is_table()) {
        r.errors.push_back("theater: [theater].bounds table is required (min_lat/min_lon/max_lat/max_lon)");
        r.ok = false;
        return r;
    }
    auto readDeg = [&](const char* key, double& out) {
        if (auto v = bounds[key].value<double>())
            out = *v;
        else
            r.errors.push_back(std::string("theater: bounds.") + key + " is required and must be a number");
    };
    readDeg("min_lat", r.theater.minLatDeg);
    readDeg("min_lon", r.theater.minLonDeg);
    readDeg("max_lat", r.theater.maxLatDeg);
    readDeg("max_lon", r.theater.maxLonDeg);

    // Range + shape checks (only meaningful once the four values parsed).
    if (r.errors.empty()) {
        auto inRange = [&](double v, double lo, double hi, const char* what) {
            if (v < lo || v > hi)
                r.errors.push_back(std::string("theater: bounds.") + what + " out of range [" + std::to_string(lo) +
                                   ", " + std::to_string(hi) + "]");
        };
        inRange(r.theater.minLatDeg, -90.0, 90.0, "min_lat");
        inRange(r.theater.maxLatDeg, -90.0, 90.0, "max_lat");
        inRange(r.theater.minLonDeg, -180.0, 180.0, "min_lon");
        inRange(r.theater.maxLonDeg, -180.0, 180.0, "max_lon");
        if (r.theater.minLatDeg >= r.theater.maxLatDeg)
            r.errors.push_back("theater: bounds.min_lat must be less than max_lat");
        // Longitude span: normal (min<max) or antimeridian-wrapped (min>max => span = max-min+360).
        double lonSpan = r.theater.maxLonDeg - r.theater.minLonDeg;
        if (lonSpan < 0.0)
            lonSpan += 360.0;
        if (lonSpan > 180.0)
            r.errors.push_back("theater: bounds longitude span exceeds 180 deg");
    }

    r.ok = r.errors.empty();
    return r;
}

GeoBounds theaterGeoBounds(const TheaterManifest& m) {
    GeoBounds b;
    b.minLat = m.minLatDeg * kDegToRad<double>;
    b.minLon = m.minLonDeg * kDegToRad<double>;
    b.maxLat = m.maxLatDeg * kDegToRad<double>;
    b.maxLon = m.maxLonDeg * kDegToRad<double>;
    return b;
}

} // namespace fl
