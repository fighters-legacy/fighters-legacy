// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fl {

class AssetManager;

// Map a glTF image URI to a Texture asset name (#833). The pack convention places textures under the
// `textures/` asset directory, so a mesh references "../../textures/<name>.ktx2" (a bare
// "<name>.ktx2" is also accepted); both yield asset name "<name>", which AssetManager::loadTexture
// resolves back to textures/<name>.ktx2 (.png fallback). Takes the path after the last "textures/"
// segment (or the bare basename) and strips the extension. Exposed for unit testing the convention.
std::string textureAssetNameFromUri(std::string_view uri);

// Map a base Texture asset name (`<slot>_<map>`) to a livery slot-map key (`<slot>.<map>`), or empty
// when the name carries no recognised map suffix (#845). The map vocabulary matches the livery TOML:
// diffuse (baseColor), orm, normal.
std::string liveryKeyFromBaseAsset(std::string_view baseAsset);

// The canonical mesh-texture URI -> file-bytes resolver, shared by SceneRenderer and the authoring
// tools' PreviewScene so the two cannot drift on the URI->asset-name->file mapping (#833/#836). It
// resolves each glTF image URI to its Texture asset name, applies a livery override with per-map
// fallback to the base (#845), and loads the bytes through the content system. A missing key OR a
// broken override both fall back to the base texture, so a partial/broken livery degrades, never
// fails; a total miss returns {} (the material keeps the renderer's default texture).
//
// LIFETIME: `assets` and `overrides` are captured BY REFERENCE and must outlive every call. The
// resolver is invoked synchronously inside IRenderer::createMesh (never stored), so this is safe for
// the SceneRenderer / PreviewScene call sites that pass a member map. Pass an empty `overrides` for
// the no-livery path.
std::function<std::vector<uint8_t>(std::string_view)>
makeMeshTextureResolver(AssetManager& assets, const std::unordered_map<std::string, std::string>& overrides);

} // namespace fl
