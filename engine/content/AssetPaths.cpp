// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/AssetPaths.h"

#include <array>
#include <cctype>

namespace fl {

static constexpr std::array<AssetPathInfo, static_cast<size_t>(AssetType::Count)> kAssetPaths = {{
    {"aircraft", ".glb", ".gltf"}, // Mesh
    {"textures", ".ktx2", ".png"}, // Texture
    {"audio", ".ogg", ""},         // Audio
    {"aircraft", ".toml", ""},     // FlightModel
    {"missions", ".yaml", ""},     // Mission
    {"terrain", ".json", ""},      // Terrain
    {"ai", ".lua", ""},            // AIScript
    {"entities", ".toml", ""},     // EntityDef
    {"sensors", ".toml", ""},      // SensorDef
    {"weapons", ".toml", ""},      // Weapon
    {"manual", ".md", ""},         // Manual (prose only — the numbers are generated, never authored)
    {"liveries", ".toml", ""},     // Livery (#845 — texture-set indirection by material slot)
    {"airports", ".toml", ""},     // Airport (#699 — airport/runway definitions)
    {"modes", ".toml", ""},        // GameMode (#521 — multiplayer game-mode definitions)
}};

// THE SIZE ASSERT BELOW IS TAUTOLOGICAL AND CANNOT FAIL. The array's length is *defined* as
// AssetType::Count, so adding an enumerator without adding a row here does not shorten the array --
// it leaves the new row VALUE-INITIALIZED, i.e. `subdir == nullptr`. The first listAssets()/path
// resolution for that type then does `<dir> + "/" + nullptr` and segfaults. That is exactly what
// happened when AssetType::Weapon was added: every unit test passed (they use mock packs) and
// fl-server crashed on the first real content pack. It is kept only for the case where someone
// changes the array's declared length. The check that actually protects you is the next one.
static_assert(kAssetPaths.size() == static_cast<size_t>(AssetType::Count),
              "kAssetPaths out of sync with AssetType enum");

// THIS is the guard that works: every row must name a directory and an extension. Add an AssetType
// and forget the row, and the build stops here instead of the server dying on a content pack.
static_assert(
    [] {
        for (const auto& info : kAssetPaths)
            if (info.subdir == nullptr || info.ext == nullptr || info.extFallback == nullptr)
                return false;
        return true;
    }(),
    "kAssetPaths has a value-initialized row: an AssetType was added without a path entry");

const AssetPathInfo& assetPathInfo(AssetType type) noexcept {
    return kAssetPaths[static_cast<uint8_t>(type)];
}

namespace {
bool ciEndsWith(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size())
        return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b)
            return false;
    }
    return true;
}
std::string lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

std::optional<std::pair<AssetType, std::string>> assetFromPackRelativePath(std::string_view relPath) {
    // Split off the first path segment (the asset-type subdir) and the remainder (name+ext).
    const size_t slash = relPath.find('/');
    if (slash == std::string_view::npos)
        return std::nullopt; // a bare file at the pack root is not a typed asset
    const std::string_view subdir = relPath.substr(0, slash);
    const std::string_view rest = relPath.substr(slash + 1);
    if (rest.empty())
        return std::nullopt;

    // Find every type whose subdir matches, then disambiguate by extension (aircraft/ = Mesh + FlightModel).
    for (uint8_t t = 0; t < static_cast<uint8_t>(AssetType::Count); ++t) {
        const AssetPathInfo& info = kAssetPaths[t];
        if (subdir != info.subdir)
            continue;
        std::string_view ext;
        if (ciEndsWith(rest, info.ext))
            ext = info.ext;
        else if (info.extFallback[0] != '\0' && ciEndsWith(rest, info.extFallback))
            ext = info.extFallback;
        else
            continue; // wrong extension for this subdir — try the next matching type
        std::string name = lowered(rest.substr(0, rest.size() - ext.size()));
        return std::make_pair(static_cast<AssetType>(t), std::move(name));
    }
    return std::nullopt; // unknown subdir, or a subdir with an extension it does not serve
}

} // namespace fl
