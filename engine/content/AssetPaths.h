// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "content/AssetTypes.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace fl {

// Asset type -> subdirectory, primary extension, fallback extension (empty = no fallback). The single
// source of truth for the pack directory layout: FolderContentPack pastes it into a path for FORWARD
// resolution, AssetManager::enableHotReload registers each subdir with the watcher, and
// assetFromPackRelativePath() runs it in REVERSE to map a changed file back to an asset (#152/#836).
struct AssetPathInfo {
    const char* subdir;
    const char* ext;
    const char* extFallback;
};

// The row for an AssetType. Every row is guaranteed non-null (a compile-time assert in the .cpp).
[[nodiscard]] const AssetPathInfo& assetPathInfo(AssetType type) noexcept;

// Reverse map: a pack-relative path (e.g. "aircraft/f5e/f5e.glb", forward-slashes, relative to the
// pack root) -> {AssetType, asset name}. The name includes its own subdir but never the type dir
// (e.g. "f5e/f5e") and is lowercased to match AssetManager::cacheKey. Returns nullopt when the path is
// under no known subdir, or has an extension that subdir does not serve (so editor droppings like
// `.swp` / `~` / `.tmp` fall out here — the fine-grained eviction's noise filter). The aircraft/ dir
// serves both Mesh (.glb/.gltf) and FlightModel (.toml), disambiguated by extension.
[[nodiscard]] std::optional<std::pair<AssetType, std::string>> assetFromPackRelativePath(std::string_view relPath);

} // namespace fl
