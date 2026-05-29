// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/FlightModelData.h"

#include <string_view>

namespace fl {

// Parses raw TOML bytes into a FlightModelData struct.
// Throws std::runtime_error on any validation failure.
FlightModelData parseFlightModel(std::string_view toml_src);

} // namespace fl
