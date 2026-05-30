// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/PlaylistLoader.h"

#include "ILogger.h"

#include <toml++/toml.hpp>

const PlaylistState* PlaylistData::findState(std::string_view id) const {
    for (const auto& s : states) {
        if (s.id == id)
            return &s;
    }
    return nullptr;
}

PlaylistData parsePlaylist(std::string_view tomlText, ILogger& logger) {
    PlaylistData result;

    if (tomlText.empty()) {
        logger.log(LogLevel::Warn, __FILE__, __LINE__, "playlist: no playlist.toml found in active content packs");
        return result;
    }

    toml::table tbl;
    try {
        tbl = toml::parse(tomlText);
    } catch (const toml::parse_error& e) {
        logger.log(LogLevel::Warn, __FILE__, __LINE__,
                   (std::string("playlist: failed to parse playlist.toml: ") + e.what()).c_str());
        return result;
    }

    if (auto v = tbl["crossfade"]["duration_s"].value<double>())
        result.crossfadeDuration = static_cast<float>(*v);

    auto* statesArr = tbl["states"].as_array();
    if (!statesArr)
        return result;

    for (const auto& elem : *statesArr) {
        const auto* tbl2 = elem.as_table();
        if (!tbl2)
            continue;

        PlaylistState state;
        if (auto v = (*tbl2)["id"].value<std::string>())
            state.id = *v;
        else
            continue; // id is mandatory

        state.loop = (*tbl2)["loop"].value_or(true);
        state.shuffle = (*tbl2)["shuffle"].value_or(false);

        if (auto* tracksArr = (*tbl2)["tracks"].as_array()) {
            for (const auto& t : *tracksArr) {
                if (auto v = t.value<std::string>())
                    state.tracks.push_back(*v);
            }
        }

        result.states.push_back(std::move(state));
    }

    return result;
}
