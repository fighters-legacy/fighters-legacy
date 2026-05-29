// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

struct MissionValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Validates a YAML mission file against the schema in docs/modding/missions.md.
// All errors are accumulated before returning — never fail-fast.
MissionValidationResult validateMission(std::string_view yamlContent);
