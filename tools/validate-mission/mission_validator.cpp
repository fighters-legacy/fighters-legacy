// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission_validator.h"

#include "mission/MissionParser.h"

namespace fl {

// The schema lives in the engine's own parser (engine-mission), so a mission this tool passes is a
// mission the engine loads — the same anti-drift rule as validate-weapon/-sensor/-entity. This tool
// currently adds no checks of its own beyond the shared schema; plausibility warnings and a --pack
// asset cross-check (the pattern validate-entity established) can be layered on top later.
MissionValidationResult validateMission(std::string_view yamlContent) {
    MissionParseResult parsed = parseMission(yamlContent);

    MissionValidationResult r;
    r.ok = parsed.ok;
    r.errors = std::move(parsed.errors);
    r.warnings = std::move(parsed.warnings);
    return r;
}

} // namespace fl
