// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <unordered_map>

namespace fl {

// A livery: texture-set indirection keyed by material-slot map, with per-map fallback to the base
// aircraft's textures (#845). Liveries are ordinary content-pack assets (`liveries/<id>.toml`), so
// pack priority + the mod loader apply with no new mechanism. A livery re-skins an aircraft WITHOUT
// touching geometry, nodes or UVs — that invariant is what makes a stranger's skin safe to accept in
// multiplayer (two peers can disagree about pixels, never about geometry).
//
// TWO-VOCABULARY RULE (preserved): `aircraft` is a namespaced DEF ID (never a filename). The values
// in `textures` are ASSET NAMES resolved through kAssetPaths (Texture -> textures/*.ktx2). The KEYS
// are material-slot map selectors of the form "<slot>.<map>" (e.g. "f5e_skin.diffuse"); the base
// aircraft's textures follow the parallel "<slot>_<map>" asset-name convention, so the renderer maps
// a base texture asset name back to a slot.map key and applies the override.
struct LiveryDef {
    std::string id;       // asset name (file stem), e.g. "f5e_aggressor_blue"
    std::string name;     // display name, e.g. "Aggressor Blue"
    std::string aircraft; // target aircraft DEF ID, e.g. "fl-base:f5e"
    // Material-slot map -> replacement texture ASSET NAME. Key: "<slot>.<map>" (map in {diffuse,
    // normal, orm}); value: a Texture asset name. Any map omitted -> the base aircraft's texture for
    // that slot (per-map fallback); an absent/broken whole livery -> the base scheme.
    std::unordered_map<std::string, std::string> textures;

    // The replacement asset for a "<slot>.<map>" key, or empty when the livery does not override it
    // (the caller falls back to the base texture).
    [[nodiscard]] std::string textureFor(const std::string& slotDotMap) const {
        auto it = textures.find(slotDotMap);
        return it == textures.end() ? std::string{} : it->second;
    }
};

// Parses a livery TOML. Throws std::runtime_error on a malformed document or a missing required field
// ([livery] name/aircraft). An empty [textures] table is legal (a livery that overrides nothing is a
// no-op that degrades to the base scheme, which is the safe default, not an error).
LiveryDef parseLiveryDef(std::string_view tomlContent);

} // namespace fl
