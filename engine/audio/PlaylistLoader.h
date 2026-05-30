// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

class ILogger;

struct PlaylistState {
    std::string id;
    std::vector<std::string> tracks; // asset names for AssetManager::loadAudio()
    bool loop{true};
    bool shuffle{false};
};

struct PlaylistData {
    float crossfadeDuration{3.0f};
    std::vector<PlaylistState> states;

    const PlaylistState* findState(std::string_view id) const;
};

// Parses the TOML text returned by AssetManager::loadConfig("playlist.toml").
// Called as: PlaylistLoader::parse(assets.loadConfig("playlist.toml").value_or(""), logger)
PlaylistData parsePlaylist(std::string_view tomlText, ILogger& logger);
