// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ILogger.h"
#include "content/AssetManager.h"
#include "match/BuiltinGameModes.h"
#include "match/GameModeParser.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace fl {

// Split a `[rotation]` item "mission[@mode]" into its mission ref and optional mode ref (#521). Mission
// and mode refs both contain ':' (pack:id), so '@' is the unambiguous separator (mirroring the
// "id@version" convention in required-pack specs). An item with no '@' has an empty mode ref, meaning
// "use the server's [match] mode default".
inline std::pair<std::string, std::string> splitRotationItem(std::string_view item) {
    const std::size_t at = item.find('@');
    if (at == std::string_view::npos)
        return {std::string(item), std::string{}};
    return {std::string(item.substr(0, at)), std::string(item.substr(at + 1))};
}

// Resolve a game-mode ref to a GameModeDef, in precedence order:
//   1. a "builtin:" id (free-flight, tdm) — the builtin namespace cannot collide with a pack asset;
//   2. a mounted content-pack `modes/<ref>.toml` asset, parsed through parseGameModeToml;
//   3. builtin:free-flight as the always-available fallback (the server always boots into a mode).
// An empty ref resolves the server default. A malformed pack mode logs and falls back, never fails.
[[nodiscard]] inline GameModeDef resolveGameMode(const std::string& ref, AssetManager* assets, ILogger& log) {
    if (ref.empty())
        return defaultGameMode();

    if (auto builtin = builtinGameMode(ref))
        return *builtin;

    if (assets) {
        if (auto asset = assets->loadGameMode(ref.c_str()); asset && !asset->bytes.empty()) {
            GameModeParseResult res = parseGameModeToml(
                std::string_view(reinterpret_cast<const char*>(asset->bytes.data()), asset->bytes.size()));
            for (const std::string& w : res.warnings)
                log.log(LogLevel::Warn, __FILE__, __LINE__, w.c_str());
            if (res.ok)
                return res.mode;
            char buf[320];
            std::snprintf(buf, sizeof(buf), "game mode '%.80s' failed to parse (%.100s); using builtin:free-flight",
                          ref.c_str(), res.error.c_str());
            log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
            return defaultGameMode();
        }
    }

    char buf[192];
    std::snprintf(buf, sizeof(buf), "game mode '%.120s' not found; using builtin:free-flight", ref.c_str());
    log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
    return defaultGameMode();
}

} // namespace fl
