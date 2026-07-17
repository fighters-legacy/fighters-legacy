// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/LiveryDef.h"

#include <toml++/toml.hpp>

#include <stdexcept>
#include <string>

namespace fl {

namespace {

[[nodiscard]] std::string reqString(toml::node_view<toml::node> node, const char* field) {
    auto v = node.value<std::string>();
    if (!v)
        throw std::runtime_error(std::string("livery def parse error: missing required field: ") + field);
    return std::move(*v);
}

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
    def.name = reqString(liv["name"], "livery.name");
    def.aircraft = reqString(liv["aircraft"], "livery.aircraft");

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
