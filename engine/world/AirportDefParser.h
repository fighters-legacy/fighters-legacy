// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/AirportDef.h"

#include <string_view>

namespace fl {

// Parse an airport definition from TOML text (a content-pack airports/<name>.toml asset). Mirrors
// EntityDefParser: throws std::runtime_error on a malformed def (missing id/name, neither nor both
// of lat/lon and world_x/world_z, a non-positive runway dimension, an unknown surface string). The
// engine and the validate tooling share this one function so they cannot drift.
//
// Angles are authored in DEGREES and stored in RADIANS (latRad/lonRad); dimensions are metres.
[[nodiscard]] AirportDef parseAirportDef(std::string_view toml);

} // namespace fl
