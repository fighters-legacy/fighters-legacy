// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/MeshTextureResolver.h"

#include "content/AssetManager.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

std::string textureAssetNameFromUri(std::string_view uri) {
    constexpr std::string_view kDir = "textures/";
    if (auto pos = uri.rfind(kDir); pos != std::string_view::npos)
        uri.remove_prefix(pos + kDir.size());
    else if (auto slash = uri.find_last_of("/\\"); slash != std::string_view::npos)
        uri.remove_prefix(slash + 1);
    if (auto dot = uri.find_last_of('.'); dot != std::string_view::npos)
        uri = uri.substr(0, dot);
    return std::string(uri);
}

std::string liveryKeyFromBaseAsset(std::string_view baseAsset) {
    static constexpr std::string_view kMaps[] = {"diffuse", "orm", "normal"};
    for (std::string_view m : kMaps) {
        if (baseAsset.size() > m.size() + 1 && baseAsset.substr(baseAsset.size() - m.size()) == m &&
            baseAsset[baseAsset.size() - m.size() - 1] == '_') {
            const std::string_view slot = baseAsset.substr(0, baseAsset.size() - m.size() - 1);
            return std::string(slot) + "." + std::string(m);
        }
    }
    return {};
}

std::function<std::vector<uint8_t>(std::string_view)>
makeMeshTextureResolver(AssetManager& assets, const std::unordered_map<std::string, std::string>& overrides) {
    return [&assets, &overrides](std::string_view uri) -> std::vector<uint8_t> {
        const std::string baseAsset = textureAssetNameFromUri(uri);
        std::string chosen = baseAsset;
        if (!overrides.empty()) {
            const std::string key = liveryKeyFromBaseAsset(baseAsset);
            if (!key.empty()) {
                if (auto ov = overrides.find(key); ov != overrides.end())
                    chosen = ov->second;
            }
        }
        auto tex = assets.loadTexture(chosen.c_str());
        if ((!tex || tex->bytes.empty()) && chosen != baseAsset)
            tex = assets.loadTexture(baseAsset.c_str()); // livery override missing/broken -> base
        if (!tex || tex->bytes.empty())
            return {};
        return tex->bytes;
    };
}

} // namespace fl
