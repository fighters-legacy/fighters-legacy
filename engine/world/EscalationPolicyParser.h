// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/EscalationPolicy.h"

#include <string_view>

namespace fl {

// Parses a content-pack escalation policy (zones/policies/<id>.toml, #162). Throws
// std::runtime_error on a malformed document, a missing required field, an unknown alert-level
// section, or dwell thresholds that are negative or decrease across stages.
//
// This is the single schema owner: validate-mission and the runtime both call it, so the linter and
// the engine cannot drift (the rule shared by parseAirportDef / parseWeaponDef / parseSensorDef).
[[nodiscard]] EscalationPolicy parseEscalationPolicy(std::string_view toml);

} // namespace fl
