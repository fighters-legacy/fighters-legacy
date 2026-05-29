// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityDef.h"

#include <stdexcept>
#include <string_view>

namespace fl {

// Parse an entity definition from TOML source.
// Throws std::runtime_error with a descriptive message on any validation failure.
// The returned EntityDef is fully populated; optional fields absent in TOML are nullopt/empty.
EntityDef parseEntityDef(std::string_view toml_src);

} // namespace fl
