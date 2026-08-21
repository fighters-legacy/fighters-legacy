// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/FlightModelLoad.h"

#include <content/AssetManager.h>
#include <flight/FlightModelData.h>
#include <flight/FlightModelParser.h>

#include <exception>
#include <string_view>

namespace fl {

FlightModelLoadResult loadAndParseFlightModel(AssetManager& assets, const char* assetName) {
    auto raw = assets.loadFlightModel(assetName);
    if (!raw || raw->bytes.empty())
        return {nullptr, std::string("no loaded content pack provides flight model '") + assetName + "'"};
    try {
        auto model = std::make_shared<const FlightModelData>(
            parseFlightModel(std::string_view(reinterpret_cast<const char*>(raw->bytes.data()), raw->bytes.size())));
        return {std::move(model), {}};
    } catch (const std::exception& e) {
        return {nullptr, std::string("flight model '") + assetName + "' failed to parse: " + e.what()};
    }
}

} // namespace fl
