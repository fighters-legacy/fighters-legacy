// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission/MissionValidator.h"

#include "mission/MissionParser.h"

namespace fl {

// The schema lives in the engine's own parser, so a mission this passes is a mission the engine
// loads — the same anti-drift rule as validate-weapon/-sensor/-entity.
MissionValidationResult validateMission(std::string_view yamlContent) {
    MissionParseResult parsed = parseMission(yamlContent);

    MissionValidationResult r;
    r.ok = parsed.ok;
    r.errors = std::move(parsed.errors);
    r.warnings = std::move(parsed.warnings);
    return r;
}

} // namespace fl
