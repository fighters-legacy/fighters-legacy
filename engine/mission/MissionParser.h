// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mission/Mission.h"

#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct MissionParseResult {
    bool ok{true};
    Mission mission; // populated on a best-effort basis even when ok == false
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Parses a YAML mission file against the schema in docs/modding/missions.md into the runtime model,
// accumulating ALL errors before returning (never fail-fast). This is the single schema owner:
// validate-mission delegates to it, so the linter a mod author runs and the engine that loads the
// mission cannot disagree. Callers set up the sim only when `ok` is true (see MissionSetup.h).
MissionParseResult parseMission(std::string_view yamlContent);

} // namespace fl
