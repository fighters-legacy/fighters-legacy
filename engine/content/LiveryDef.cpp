// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/LiveryDef.h"

#include "config/TomlRead.h"
#include <toml++/toml.hpp>

#include <stdexcept>
#include <string>

namespace fl {

namespace {

constexpr const char* kErr = "livery def parse error: ";

} // namespace

LiveryDef parseLiveryDef(std::string_view tomlContent) {
    toml::table tbl;
    try {
        tbl = toml::parse(tomlContent);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("livery def parse error: TOML parse error: ") + e.what());
    }

    LiveryDef def;

    // ── [livery] (required) ──────────────────────────────────────────────────
    auto liv = tbl["livery"];
    if (!liv || !liv.as_table())
        throw std::runtime_error("livery def parse error: missing [livery] table");
    def.name = req_string(liv["name"], "livery.name", kErr);
    def.aircraft = req_string(liv["aircraft"], "livery.aircraft", kErr);

    // ── [textures] (optional) ────────────────────────────────────────────────
    // Nested slot -> map -> asset name. A dotted key `f5e_skin.diffuse = "x"` under [textures] is a
    // TOML nested table (textures.f5e_skin.diffuse), so iterating sub-tables handles both the dotted
    // form and an explicit [textures.f5e_skin] section uniformly. Flatten to a "<slot>.<map>" key.
    if (auto tex = tbl["textures"]; tex && tex.as_table()) {
        for (const auto& [slotKey, slotNode] : *tex.as_table()) {
            const auto* slotTbl = slotNode.as_table();
            if (!slotTbl)
                throw std::runtime_error(std::string("livery def parse error: [textures] entry '") +
                                         std::string(slotKey.str()) +
                                         "' must be a slot table of map = \"asset\" pairs (e.g. "
                                         "f5e_skin.diffuse = \"...\")");
            for (const auto& [mapKey, mapNode] : *slotTbl) {
                auto asset = mapNode.value<std::string>();
                if (!asset)
                    throw std::runtime_error(std::string("livery def parse error: texture override '") +
                                             std::string(slotKey.str()) + "." + std::string(mapKey.str()) +
                                             "' must be a texture asset name (string)");
                def.textures.emplace(std::string(slotKey.str()) + "." + std::string(mapKey.str()), std::move(*asset));
            }
        }
    }

    return def;
}

} // namespace fl
