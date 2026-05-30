// SPDX-License-Identifier: GPL-3.0-or-later
#include "playlist_validator.h"

#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

PlaylistValidationResult validatePlaylist(const std::string& tomlPath, const std::string& packDir) {
    PlaylistValidationResult result;

    // Read the file.
    std::ifstream file(tomlPath);
    if (!file) {
        result.errors.push_back("cannot open file: " + tomlPath);
        return result;
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    std::string content = oss.str();

    // Parse TOML.
    toml::table tbl;
    try {
        tbl = toml::parse(content);
    } catch (const toml::parse_error& e) {
        result.errors.push_back(std::string("TOML parse error: ") + e.what());
        return result;
    }

    // Validate [crossfade].
    if (auto v = tbl["crossfade"]["duration_s"].value<double>()) {
        if (*v <= 0.0)
            result.errors.push_back("crossfade.duration_s must be positive");
    }

    // Validate [[states]] array.
    auto* statesArr = tbl["states"].as_array();
    if (!statesArr || statesArr->empty()) {
        result.errors.push_back("no [[states]] entries found");
        return result;
    }

    std::set<std::string> seenIds;
    int idx = 0;
    for (const auto& elem : *statesArr) {
        const auto* st = elem.as_table();
        if (!st) {
            result.errors.push_back("states[" + std::to_string(idx) + "] is not a table");
            ++idx;
            continue;
        }

        std::string id;
        if (auto v = (*st)["id"].value<std::string>()) {
            id = *v;
        } else {
            result.errors.push_back("states[" + std::to_string(idx) + "] missing required 'id'");
            ++idx;
            continue;
        }

        if (seenIds.count(id))
            result.errors.push_back("duplicate state id: '" + id + "'");
        seenIds.insert(id);

        const auto* tracksArr = (*st)["tracks"].as_array();
        if (!tracksArr || tracksArr->empty()) {
            result.warnings.push_back("state '" + id + "' has no tracks");
        } else {
            for (const auto& t : *tracksArr) {
                auto trackName = t.value<std::string>();
                if (!trackName) {
                    result.errors.push_back("state '" + id + "': track entry is not a string");
                    continue;
                }
                ++result.totalTracks;

                if (!packDir.empty()) {
                    // Track asset name "music/foo" → file at <packDir>/audio/music/foo.ogg
                    fs::path trackPath = fs::path(packDir) / "audio" / (*trackName + ".ogg");
                    if (!fs::exists(trackPath))
                        result.errors.push_back("state '" + id + "': track file not found: " + trackPath.string());
                }
            }
        }

        ++result.stateCount;
        ++idx;
    }

    return result;
}
