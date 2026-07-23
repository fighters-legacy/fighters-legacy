// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/PreviewScene.h"

#include <memory>
#include <string>

namespace fl {

class AssetManager;
class ContentIndex;
class IFilesystem;
class ILogger;

// Owns a viewer's content stack (filesystem + mod packs + AssetManager + ContentIndex), the headless
// "packs without a session" bootstrap (mirrors fl-server). Kept alive for the viewer's lifetime so a
// PreviewScene can re-read textures on hot-reload (#152/#838); a snapshot run holds it until exit.
class ViewerContent {
  public:
    // Load the mod stack rooted at `assetsRoot` (empty -> FL_ASSETS_ROOT env, then cwd). Never fails
    // to construct: a root with no mods/ yields an empty (but usable) stack. `logger` must outlive it.
    static std::unique_ptr<ViewerContent> load(const std::string& assetsRoot, ILogger& logger);
    ~ViewerContent();

    AssetManager& assets() noexcept {
        return *m_assets;
    }
    const std::string& resolvedRoot() const noexcept {
        return m_root;
    }

    // Build a ModelDesc for a "pack:id" entity def (mesh + damage mesh + livery overrides), mirroring
    // the game's resolver. Returns false + `error` if the id resolves to no pack/def or has no mesh.
    bool resolveEntity(const std::string& entityId, PreviewScene::ModelDesc& out, std::string& error);

  private:
    ViewerContent() = default;

    std::string m_root;
    std::unique_ptr<IFilesystem> m_fs;
    std::unique_ptr<AssetManager> m_assets;
    std::unique_ptr<ContentIndex> m_index;
};

// Load a bare .glb from disk into a ModelDesc (bytes + parent dir for relative texture URIs). Needs no
// content pack. Returns false + `error` on a read failure.
[[nodiscard]] bool loadBareGlb(const std::string& path, PreviewScene::ModelDesc& out, std::string& error);

} // namespace fl
