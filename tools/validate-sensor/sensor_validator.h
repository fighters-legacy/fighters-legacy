// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ValidatorCli.h"

#include <string>
#include <string_view>
#include <vector>

namespace fl {

// The shared three-field shape (#1277); the name stays for every caller.
using SensorValidationResult = ToolValidationResult;

// Validates one sensor TOML file.
//
// Schema validity is delegated to the RUNTIME parser (fl::sensor::parseSensorDef), for the same
// reason validate-weapon delegates to parseWeaponDef: the tool's job is to answer "will the engine
// load this?", and the only honest way to answer it is to ask the thing that loads it. Validator and
// engine cannot drift.
//
// On top of that, this adds what a parser deliberately does not do: PLAUSIBILITY warnings. A sensor
// can be schema-perfect and still be nonsense — an infrared sensor that emits, a track lobe wider
// than the search lobe that feeds it, a radar that can never take a lock because it never radiates.
// None of those are parse errors (a pack is allowed to be strange on purpose), and all of them are
// almost certainly authoring mistakes.
SensorValidationResult validateSensor(std::string_view tomlContent);

} // namespace fl
