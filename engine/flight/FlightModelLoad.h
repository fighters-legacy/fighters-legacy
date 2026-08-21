// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

namespace fl {

class AssetManager;
struct FlightModelData;

// The one load-and-parse step behind both flight-model resolvers (#1232). The client resolver and
// the server spawn path keep their own caches, keying and fallback policy, but the step that turns
// an asset name into a parsed model must refuse malformed content the same way on both sides: a
// missing or malformed model is a null pointer plus a reason — never an exception escaping into a
// spawn path.
struct FlightModelLoadResult {
    std::shared_ptr<const FlightModelData> model; // null when the asset is missing, empty or malformed
    std::string error;                            // human-readable reason whenever model is null
};

FlightModelLoadResult loadAndParseFlightModel(AssetManager& assets, const char* assetName);

} // namespace fl
