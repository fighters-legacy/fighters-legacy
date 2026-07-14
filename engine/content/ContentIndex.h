// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "content/AssetTypes.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fl {

class AssetManager;
class ILogger;

// Maps a namespaced def id ("fl-base:apq159") to the ASSET NAME that holds it ("apq159").
//
// THE RULE THIS CLASS EXISTS TO ENFORCE. A content field naming a FILE (`mesh`, `flight_model`,
// `ai_script`, `damage_mesh`, `cockpit`) is an ASSET NAME — a bare filename stem. A field naming a
// DEF (`entity.sensors`, `hardpoints.allowed`, `hardpoints.default`) is a NAMESPACED ID. Ids never
// touch the filesystem: FolderContentPack pastes an asset name straight into a path, so feeding it
// an id builds "sensors/fl-base:apq159.toml" — a file that cannot exist, and is not even a legal
// filename on Windows. Every id-based cross-reference in the engine was silently missing for
// exactly this reason (#810). This index is the ONLY place the two vocabularies are allowed to meet.
//
// Threading: build() and every accessor are main-thread-only, like AssetManager. Build once at
// bootstrap, before GameLoop::start(); the index is read-only during simulation.
class ContentIndex {
  public:
    // Shallow-parses the `id` key out of every def file listed by AssetManager::listAssets(type),
    // for each type in `types`. Only def types have an id — passing a raw-bytes type (Mesh, Audio,
    // Texture, ...) logs an Error and skips it.
    //
    // Duplicate id across packs: first pack wins, matching the priority-stack semantics of every
    // other lookup in the content system (listAssets returns names in pack-priority order).
    void build(AssetManager& assets, std::span<const AssetType> types, ILogger& log);

    // Returns the asset name that def `id` lives in, or nullptr if no pack declares it.
    // Case-insensitive: AssetManager::cacheKey lowercases every name before it reaches a pack, so
    // an id lookup that was case-sensitive would resolve through the index and then miss the file.
    [[nodiscard]] const std::string* assetNameFor(AssetType type, std::string_view id) const noexcept;

    [[nodiscard]] std::vector<std::string> idsOfType(AssetType type) const;

    void clear() noexcept;

  private:
    // key = "<typeOrdinal>:<lowercased id>" — the same shape as AssetManager::cacheKey.
    std::unordered_map<std::string, std::string> m_idToAsset;
};

} // namespace fl
