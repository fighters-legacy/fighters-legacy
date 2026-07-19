// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The single campaign-schema owner (#635): parses a campaign YAML file into a CampaignDef, accumulating
// ALL errors (never fail-fast) like parseMission. validate-campaign (#784) delegates here so the linter
// and the engine cannot drift — the same anti-drift contract the mission/weapon/sensor parsers hold.

#include "campaign/CampaignDef.h"

#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct CampaignParseResult {
    bool ok{true};
    CampaignDef campaign;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

[[nodiscard]] CampaignParseResult parseCampaign(std::string_view yamlContent);

} // namespace fl
