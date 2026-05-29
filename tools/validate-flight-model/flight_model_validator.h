// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

struct FlightModelValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Validates a TOML flight model file against the schema in docs/modding/flight-model.md.
// All errors are accumulated before returning — never fail-fast.
FlightModelValidationResult validateFlightModel(std::string_view tomlContent);
