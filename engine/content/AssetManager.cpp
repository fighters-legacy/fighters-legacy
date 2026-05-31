// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/AssetManager.h"

#include "IFilesystemWatcher.h"
#include "ILogger.h"
#include "IWindow.h"

#include <algorithm>
#include <cctype>

AssetManager::AssetManager(std::vector<std::unique_ptr<IContentPack>> packs, ILogger& logger)
    : m_packs(std::move(packs)), m_logger(logger) {}

void AssetManager::initialize(IWindow* window) {
    std::vector<std::unique_ptr<IContentPack>> active;
    active.reserve(m_packs.size());

    for (auto& pack : m_packs) {
        auto status = pack->init();
        if (status == IContentPack::Status::Ready) {
            active.push_back(std::move(pack));
        } else {
            // NeedsConfiguration
            if (!window) {
                m_logger.log(
                    LogLevel::Warn, __FILE__, __LINE__,
                    (std::string("dropping pack '") + pack->id() + "': NeedsConfiguration but no window available")
                        .c_str());
                continue;
            }
            if (pack->configure(window)) {
                active.push_back(std::move(pack));
            } else {
                m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                             (std::string("dropping pack '") + pack->id() + "': configure() returned false").c_str());
            }
        }
    }

    m_packs = std::move(active);
}

std::string AssetManager::cacheKey(AssetType type, const char* name) {
    std::string key = std::to_string(static_cast<uint8_t>(type)) + ':';
    for (const char* p = name; *p; ++p)
        key += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    return key;
}

template <typename T>
std::shared_ptr<T> AssetManager::loadAsset(AssetType type, const char* name,
                                           std::optional<T> (IContentPack::*loader)(const char*)) {
    std::string key = cacheKey(type, name);

    auto it = m_cache.find(key);
    if (it != m_cache.end())
        return std::static_pointer_cast<T>(it->second);

    // Normalize to lowercase before calling packs (G3)
    std::string lower = key.substr(key.find(':') + 1);

    for (auto& pack : m_packs) {
        auto result = (pack.get()->*loader)(lower.c_str());
        if (result.has_value()) {
            auto ptr = std::make_shared<T>(std::move(*result));
            m_cache.emplace(key, ptr);
            return ptr;
        }
    }

    m_logger.log(LogLevel::Warn, __FILE__, __LINE__, (std::string("asset not found: ") + lower).c_str());
    return nullptr;
}

std::shared_ptr<MeshData> AssetManager::loadMesh(const char* name) {
    return loadAsset<MeshData>(AssetType::Mesh, name, &IContentPack::loadMesh);
}
std::shared_ptr<TextureData> AssetManager::loadTexture(const char* name) {
    return loadAsset<TextureData>(AssetType::Texture, name, &IContentPack::loadTexture);
}
std::shared_ptr<AudioBuffer> AssetManager::loadAudio(const char* name) {
    return loadAsset<AudioBuffer>(AssetType::Audio, name, &IContentPack::loadAudio);
}
std::shared_ptr<FlightModel> AssetManager::loadFlightModel(const char* name) {
    return loadAsset<FlightModel>(AssetType::FlightModel, name, &IContentPack::loadFlightModel);
}
std::shared_ptr<MissionData> AssetManager::loadMission(const char* name) {
    return loadAsset<MissionData>(AssetType::Mission, name, &IContentPack::loadMission);
}
std::shared_ptr<TerrainData> AssetManager::loadTerrain(const char* name) {
    return loadAsset<TerrainData>(AssetType::Terrain, name, &IContentPack::loadTerrain);
}
std::shared_ptr<AIScript> AssetManager::loadAIScript(const char* name) {
    return loadAsset<AIScript>(AssetType::AIScript, name, &IContentPack::loadAIScript);
}
std::shared_ptr<EntityDefData> AssetManager::loadEntityDef(const char* name) {
    return loadAsset<EntityDefData>(AssetType::EntityDef, name, &IContentPack::loadEntityDef);
}

std::optional<std::string> AssetManager::loadConfig(const char* name) {
    for (auto& pack : m_packs) {
        if (auto result = pack->loadConfig(name))
            return result;
    }
    return std::nullopt;
}

std::optional<std::string> AssetManager::resolveTerrainChunk(const char* terrainId, uint32_t chunkX, uint32_t chunkY,
                                                             uint32_t lod) {
    for (auto& pack : m_packs) {
        if (auto result = pack->resolveTerrainChunk(terrainId, chunkX, chunkY, lod))
            return result;
    }
    return std::nullopt;
}

void AssetManager::enableHotReload(IFilesystemWatcher& watcher) {
    m_watcher = &watcher;
    for (auto& pack : m_packs) {
        if (const char* dir = pack->rootDirectory())
            watcher.watch(PathDomain::Assets, dir, true);
    }
}

void AssetManager::processHotReload() {
    if (!m_watcher)
        return;

    auto events = m_watcher->pollEvents();
    if (events.empty())
        return;

    // Phase 1: full cache clear on any filesystem event. Fine-grained eviction
    // (path → asset name mapping) is a future improvement.
    m_logger.log(LogLevel::Debug, __FILE__, __LINE__, "hot-reload: filesystem change detected, clearing asset cache");
    m_cache.clear();
}
