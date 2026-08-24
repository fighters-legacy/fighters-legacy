// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ValidatorCli.h"

#include <string>
#include <string_view>
#include <vector>

namespace fl {

// The shared three-field shape (#1277); the name stays for every caller.
using CampaignValidationResult = ToolValidationResult;

// Schema-only validation: delegates to the engine's parseCampaign (#847) so the linter and the runtime
// cannot drift, then reports its errors/warnings. All errors accumulated (never fail-fast).
CampaignValidationResult validateCampaign(std::string_view yamlContent);

// As above, plus pack cross-checks: every dynamic.theaters[].id resolves to a theaters/<id>.toml that
// parses with valid bounds; every story[].file / templates[].file resolves in the pack; every
// initial_frontline / set_frontline PNG exists, is 8-bit grayscale, and matches its theater's
// frontline_grid; template headers declare a role + fills from known sources; a materialized template
// leaves no unresolved ${...} placeholder and passes validateMission. packDir empty = schema-only.
CampaignValidationResult validateCampaign(std::string_view yamlContent, const std::string& packDir);

// How a pack YAML file should be validated (#847/#651). Campaigns live under missions/ alongside plain
// missions and template bodies; they are told apart by their top-level keys. Exported so validate-mod
// shares the one discrimination rule.
enum class PackYamlKind { Mission, Campaign, Template };
[[nodiscard]] PackYamlKind classifyPackYaml(std::string_view yamlContent);

} // namespace fl
