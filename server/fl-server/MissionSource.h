// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ILogger.h"
#include "content/AssetManager.h"
#include "mission/BuiltinMissions.h"

#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace fl {

// Resolve a `--mission` / `[rotation]` name to mission YAML, in precedence order:
//   1. a builtin mission id ("builtin:sandbox", "builtin:shape-gallery") — the "builtin:"
//      namespace cannot collide with a file or pack name, so it always wins;
//   2. a readable `.yaml`/`.yml` FILE path — the authoring loop: iterate a mission from disk
//      without mounting a pack or rebuilding (lint it with validate-mission first);
//   3. a Mission asset from a mounted content pack, resolved by stem via AssetManager.
// Returns nullopt when nothing resolves (the caller warns and starts an empty world).
[[nodiscard]] inline std::optional<std::string> loadMissionYaml(const std::string& name, AssetManager* assets,
                                                                ILogger& log) {
    if (std::string_view b = builtinMissionYaml(name); !b.empty())
        return std::string(b);

    const bool looksLikeFile = name.ends_with(".yaml") || name.ends_with(".yml");
    if (looksLikeFile) {
        std::ifstream in(name, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            return std::move(ss).str();
        }
        // A name shaped like a file that cannot be read is almost certainly a typo'd path, not a
        // pack asset stem (asset names carry no extension) — say so, then still try the asset path.
        char buf[192];
        std::snprintf(buf, sizeof(buf), "mission file '%.128s' is not readable — trying pack assets", name.c_str());
        log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
    }

    if (assets) {
        if (auto missionAsset = assets->loadMission(name.c_str()))
            return std::string(missionAsset->bytes.begin(), missionAsset->bytes.end());
    }
    return std::nullopt;
}

} // namespace fl
