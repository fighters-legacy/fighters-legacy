// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ValidatorCli.h"

#include <string>
#include <string_view>
#include <vector>

namespace fl {

// The shared three-field shape (#1277); the name stays for every caller.
using FlightModelValidationResult = ToolValidationResult;

// Validates a TOML flight model file against the schema in docs/modding/flight-model.md.
// All errors are accumulated before returning — never fail-fast.
FlightModelValidationResult validateFlightModel(std::string_view tomlContent);

} // namespace fl
