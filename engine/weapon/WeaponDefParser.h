// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "weapon/WeaponDef.h"

#include <string_view>

namespace fl {

// Parses raw weapon TOML bytes into a WeaponDef.
// Throws std::runtime_error on any validation failure (same posture as parseFlightModel /
// parseEntityDef: a content-pack definition is either well-formed or it is not loaded).
// Authored aviation units (nm / kts / lb / ft) are converted to SI here.
WeaponDef parseWeaponDef(std::string_view toml_src);

} // namespace fl
