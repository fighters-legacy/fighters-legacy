// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

// Schema validation for a mission document, in the engine rather than in tools/ (#601).
//
// It started in tools/validate-mission, which was the only caller for as long as the only way to
// check a mission was to run the linter. #601's `submit_mission` tool has to validate a mission an
// agent just generated, in-process, before anything loads it — and `server/` is added to the build
// before `tools/`, so fl-server naming validate-mission-lib would silently degrade into a raw link
// flag rather than a dependency.
//
// So the schema-only check lives here, next to parseMission — the single schema owner it delegates
// to. validate-mission keeps its --pack crew cross-check on top of this, and every existing caller
// keeps including "mission_validator.h" unchanged.

namespace fl {

struct MissionValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Validates a YAML mission against the schema in docs/modding/missions.md. All errors are
// accumulated before returning — never fail-fast, because an author fixing one error at a time is
// how a linter wastes someone's afternoon.
[[nodiscard]] MissionValidationResult validateMission(std::string_view yamlContent);

} // namespace fl
