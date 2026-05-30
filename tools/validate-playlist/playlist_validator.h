// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

struct PlaylistValidationResult {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    int stateCount{0};
    int totalTracks{0};

    bool ok() const {
        return errors.empty();
    }
};

// Validates a playlist TOML file.
// If packDir is non-empty, also checks that each referenced OGG track exists on disk at
// <packDir>/audio/<track-name>.ogg.
PlaylistValidationResult validatePlaylist(const std::string& tomlPath, const std::string& packDir = "");
