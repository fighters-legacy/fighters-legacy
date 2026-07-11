// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

namespace fl {

class IContentPack;
class IFilesystem;
class ILogger;

// Bundled coarse global base terrain (#474). A zero-user-pack launch should still show a
// recognizable Earth: the engine ships a small coarse cube-sphere base (six face roots to level
// ~5-7, height + optional land cover) that is mounted as the LOWEST-priority content pack, so
// `resolveTilePath` serves its root tiles while `generateProceduralTile` fills finer near-camera
// detail and any user pack overrides specific regions (first-wins priority stack).
//
// The base lives at `<baseDir>/terrain/<terrainId>/f<face>/l<level>/tile_<i>_<j>.png` relative to
// the filesystem's PathDomain::Assets root (the same anchor ModLoader uses for `mods/`), so the
// game (SDL base path) and fl-server (cwd) share one call.
//
// Returns a FolderContentPack serving the base, or nullptr when no base is bundled (detected via
// the +X face root height tile sentinel) — so the common builtin-procedural-only path keeps its
// zero-overhead `hasPacks()==false` fast path. The caller appends the returned pack LAST (lowest
// priority) to the vector handed to AssetManager.
[[nodiscard]] std::unique_ptr<IContentPack>
loadBundledBaseTerrain(IFilesystem& fs, ILogger& logger, const std::string& baseDir, const std::string& terrainId);

} // namespace fl
