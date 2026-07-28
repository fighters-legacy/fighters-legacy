// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The schema-only check and its result type moved into engine-mission (#601) so fl-server's MCP
// `submit_mission` tool can validate a generated mission in-process. This header keeps
// validate-mission's façade: it re-exports that entry point and declares the --pack cross-check,
// which needs a content pack on disk and therefore stays a tool concern.
#include "mission/MissionValidator.h"

#include <string>
#include <string_view>

namespace fl {

// As validateMission(yamlContent), plus a --pack cross-check (#976): every object's `crew:` block is
// checked against the referenced entity type's declared [[crew]] seats in `packDir` — a seat index
// out of range, a role the entity does not declare, or a crew: block on a single-seat entity is an
// ERROR; an entity type the pack does not contain is a WARNING (it may be builtin or from another
// pack). packDir empty = the schema-only validateMission above.
MissionValidationResult validateMission(std::string_view yamlContent, const std::string& packDir);

} // namespace fl
