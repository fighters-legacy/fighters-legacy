// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct MissionValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Validates a YAML mission file against the schema in docs/modding/missions.md.
// All errors are accumulated before returning — never fail-fast.
MissionValidationResult validateMission(std::string_view yamlContent);

// As above, plus a --pack cross-check (#976): every object's `crew:` block is checked against the
// referenced entity type's declared [[crew]] seats in `packDir` — a seat index out of range, a role
// the entity does not declare, or a crew: block on a single-seat entity is an ERROR; an entity type the
// pack does not contain is a WARNING (it may be builtin or from another pack). packDir empty = the
// schema-only validateMission above.
MissionValidationResult validateMission(std::string_view yamlContent, const std::string& packDir);

} // namespace fl
