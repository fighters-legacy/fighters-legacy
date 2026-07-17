// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct LiveryValidationResult {
    bool ok{true};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Single-file validation of a livery TOML (#845). Delegates the schema to the engine's own
// parseLiveryDef — a livery this tool passes is a livery the engine loads — then adds plausibility
// warnings a runtime parser must not make (an empty/no-op livery; an override whose map is outside
// the renderer's {diffuse, normal, orm} vocabulary; an `aircraft` that is not a namespaced def id).
// It cannot resolve references (asset names and def ids only mean something against a pack) — that is
// the --pack path.
LiveryValidationResult validateLivery(std::string_view tomlContent);

// Full cross-file validation of every liveries/*.toml in a content pack, the way the engine resolves
// a livery: each texture ASSET NAME must resolve to a Texture file the pack actually has (ERROR — the
// livery ships its own skins), and the `aircraft` DEF ID should resolve to an entity in the pack
// (WARN only — a livery pack legitimately targets an aircraft that lives in a base pack).
LiveryValidationResult validateLiveryPack(const std::string& packDir);

} // namespace fl
