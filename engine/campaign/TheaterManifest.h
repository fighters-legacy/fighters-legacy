// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Theater manifest (#847): the geographic definition a campaign theater references by id
// (docs/modding/formats.md "Theater Manifest"). A pack ships `theaters/<id>.toml`; the campaign's
// `dynamic.theaters[].id` resolves to it, supplying the geographic `bounds` the frontline raster maps
// onto. Parsed with the accumulate-all-errors contract of parseCampaign / parseMission, so the engine
// and validate-campaign share one schema.

#include "campaign/Frontline.h" // GeoBounds

#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct TheaterManifest {
    std::string id;
    std::string name;
    std::string layer;            // default weather/lighting layer id (free-form)
    std::string terrain{"world"}; // terrain id this theater rides on (default: the global world)
    double minLatDeg{0.0};
    double minLonDeg{0.0};
    double maxLatDeg{0.0};
    double maxLonDeg{0.0};
};

struct TheaterManifestParseResult {
    bool ok{true};
    TheaterManifest theater;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

[[nodiscard]] TheaterManifestParseResult parseTheaterManifest(std::string_view tomlContent);

// The manifest's degree bounds as engine GeoBounds (radians). minLon > maxLon (antimeridian wrap) is
// preserved.
[[nodiscard]] GeoBounds theaterGeoBounds(const TheaterManifest& m);

} // namespace fl
