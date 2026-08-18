// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/Geodetic.h" // kEarthRadiusM — the default sphere coordinates resolve against
#include "mission/Mission.h"

#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct MissionParseResult {
    bool ok{true};
    Mission mission; // populated on a best-effort basis even when ok == false
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Parses a YAML mission file against the schema in docs/modding/missions.md into the runtime model,
// accumulating ALL errors before returning (never fail-fast). This is the single schema owner:
// validate-mission delegates to it, so the linter a mod author runs and the engine that loads the
// mission cannot disagree. Callers set up the sim only when `ok` is true (see MissionSetup.h).
//
// COORDINATES COME OUT IN WORLD XYZ (#1211). The file's frame is an AUTHORING frame: `pos[1]` (or
// `alt:`) is MSL altitude, and with an `anchor:` the x/z components are metres east and north of it,
// so a mission reads in human numbers wherever on the planet it is set. This function is the one
// place that conversion happens — spawns, routes and camera shots downstream see world coordinates
// exactly as they always did.
//
// `planetRadiusM` is the sphere the conversion resolves against; the caller passes the radius its
// world actually uses (fl-server: `[world] planet_radius_m`), and the default is Earth.
MissionParseResult parseMission(std::string_view yamlContent, double planetRadiusM = kEarthRadiusM);

} // namespace fl
