// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct GameModeValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Validate a game-mode TOML file (#521). Delegates the schema to the runtime parseGameModeToml (so the
// validator and the engine cannot drift), then layers plausibility checks a mod author wants caught up
// front: duplicate team ids, a team count that cannot fit the player cap, a warmup longer than the
// match clock, an empty-but-not-mission-sides team list. All problems are accumulated (never
// fail-fast); parser warnings are surfaced as warnings here.
GameModeValidationResult validateGameMode(std::string_view tomlContent);

} // namespace fl
