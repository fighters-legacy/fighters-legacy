// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity_resolve.h"

#include "content/AssetManager.h"
#include "content/ContentIndex.h"
#include "content/ModLoader.h"
#include "entity/EntityDefParser.h"

#include "ILogger.h"
#include "StdFilesystem.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fl {

std::unique_ptr<ViewerContent> ViewerContent::load(const std::string& assetsRoot, ILogger& logger) {
    namespace fs = std::filesystem;
    fs::path root;
    if (!assetsRoot.empty())
        root = fs::path(assetsRoot);
    else if (const char* ev = std::getenv("FL_ASSETS_ROOT"); ev && *ev)
        root = fs::path(ev);
    else
        root = fs::current_path();

    auto vc = std::unique_ptr<ViewerContent>(new ViewerContent());
    vc->m_root = root.string();
    vc->m_fs = std::make_unique<StdFilesystem>(root, root);

    ModLoader modLoader(*vc->m_fs, logger, root.string());
    auto packs = modLoader.load();
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "content: %zu mod(s) loaded from %s", packs.size(), root.string().c_str());
        logger.log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    vc->m_assets = std::make_unique<AssetManager>(std::move(packs), logger);
    vc->m_assets->initialize(nullptr);
    vc->m_index = std::make_unique<ContentIndex>();
    const std::array<AssetType, 1> types{AssetType::EntityDef};
    vc->m_index->build(*vc->m_assets, types, logger);
    return vc;
}

ViewerContent::~ViewerContent() = default;

bool ViewerContent::resolveEntity(const std::string& entityId, PreviewScene::ModelDesc& out, std::string& error) {
    const std::string* asset = m_index->assetNameFor(AssetType::EntityDef, entityId);
    if (!asset) {
        error = "no content pack declares entity '" + entityId + "'";
        return false;
    }
    auto defBytes = m_assets->loadEntityDef(asset->c_str());
    if (!defBytes || defBytes->bytes.empty()) {
        error = "entity '" + entityId + "' -> asset '" + *asset + "' failed to load";
        return false;
    }
    EntityDef def;
    try {
        def = parseEntityDef(
            std::string_view(reinterpret_cast<const char*>(defBytes->bytes.data()), defBytes->bytes.size()));
    } catch (const std::exception& e) {
        error = "entity '" + entityId + "' failed to parse: " + e.what();
        return false;
    }
    if (def.mesh.empty()) {
        error = "entity '" + entityId + "' declares no mesh";
        return false;
    }
    out = PreviewScene::ModelDesc{};
    out.meshAssetName = def.mesh;
    out.damageMeshAssetName = def.classicDamageMesh;
    out.contentForward = true;
    if (auto livery = m_assets->liveryForAircraft(def.id.c_str())) {
        out.liveryOverrides = std::move(livery->textures);
    }
    return true;
}

bool loadBareGlb(const std::string& path, PreviewScene::ModelDesc& out, std::string& error) {
    namespace fs = std::filesystem;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open '" + path + "'";
        return false;
    }
    out = PreviewScene::ModelDesc{};
    out.glbBytes.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (out.glbBytes.empty()) {
        error = "'" + path + "' is empty";
        return false;
    }
    out.glbDir = fs::path(path).parent_path();
    out.contentForward = true;
    return true;
}

} // namespace fl
