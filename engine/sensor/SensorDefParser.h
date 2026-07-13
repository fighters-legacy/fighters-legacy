// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sensor/SensorDef.h"

#include <string_view>

namespace fl::sensor {

// Parses a sensor definition TOML file (sensors/*.toml in a content pack).
//
// Throws std::runtime_error with the offending field name on any schema violation. A malformed
// sensor is NOT loaded: a sensor that silently fell back to defaults would be an aircraft whose
// radar quietly became an eyeball, which is worse than a load error.
//
// Schema: docs/modding/formats.md#sensor-definition-toml
[[nodiscard]] SensorDef parseSensorDef(std::string_view toml_src);

} // namespace fl::sensor
