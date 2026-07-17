// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/AssetManager.h"

#include "IFilesystemWatcher.h"
#include "ILogger.h"
#include "IWindow.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <span>
#include <string_view>

namespace fl {

AssetManager::AssetManager(std::vector<std::unique_ptr<IContentPack>> packs, ILogger& logger)
    : m_packs(std::move(packs)), m_logger(logger) {}

std::vector<AssetManager::PackManifestInfo> AssetManager::packManifest() const {
    std::vector<PackManifestInfo> out;
    out.reserve(m_packs.size());
    for (const auto& pack : m_packs)
        out.push_back({pack->id() ? pack->id() : "", pack->version() ? pack->version() : ""});
    return out;
}

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
        if (!result.has_value())
            continue;

        // Validate magic bytes and size before caching — covers both directory mods
        // and compiled plugins since validation happens at the manager layer.
        std::span<const uint8_t> header(result->bytes.data(), std::min(result->bytes.size(), std::size_t{16}));
        auto vr = m_validator.validate(type, header, result->bytes.size());
        if (!vr.valid) {
            m_logger.log(LogLevel::Error, __FILE__, __LINE__,
                         (std::string("discarding asset '") + lower + "': " + vr.reason).c_str());
            continue; // try next pack
        }

        auto ptr = std::make_shared<T>(std::move(*result));
        m_cache.emplace(key, ptr);
        return ptr;
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

const IContentPack* AssetManager::findPackForAsset(AssetType type, const char* name) const {
    for (auto& pack : m_packs) {
        if (pack->hasAsset(name, type))
            return pack.get();
    }
    return nullptr;
}

std::string AssetManager::findPackRootForAsset(AssetType type, const char* name) const {
    const IContentPack* pack = findPackForAsset(type, name);
    if (!pack)
        return "";
    const char* root = pack->rootDirectory();
    return root ? root : "";
}
std::shared_ptr<EntityDefData> AssetManager::loadEntityDef(const char* name) {
    return loadAsset<EntityDefData>(AssetType::EntityDef, name, &IContentPack::loadEntityDef);
}

std::shared_ptr<SensorDefData> AssetManager::loadSensorDef(const char* name) {
    return loadAsset<SensorDefData>(AssetType::SensorDef, name, &IContentPack::loadSensorDef);
}

std::shared_ptr<WeaponDefData> AssetManager::loadWeaponDef(const char* name) {
    return loadAsset<WeaponDefData>(AssetType::Weapon, name, &IContentPack::loadWeaponDef);
}

std::shared_ptr<ManualProse> AssetManager::loadManualProse(const char* name) {
    return loadAsset<ManualProse>(AssetType::Manual, name, &IContentPack::loadManualProse);
}

std::shared_ptr<LiveryData> AssetManager::loadLivery(const char* name) {
    return loadAsset<LiveryData>(AssetType::Livery, name, &IContentPack::loadLivery);
}

std::optional<LiveryDef> AssetManager::liveryForAircraft(const char* aircraftDefId) {
    if (!aircraftDefId || !*aircraftDefId)
        return std::nullopt;
    // listAssets is priority-ordered (first-wins), so the first livery whose aircraft matches is the
    // highest-priority one for this aircraft — the "two peers see their own installed livery" rule.
    for (const auto& name : listAssets(AssetType::Livery)) {
        auto data = loadLivery(name.c_str());
        if (!data || data->bytes.empty())
            continue;
        LiveryDef def;
        try {
            def =
                parseLiveryDef(std::string_view(reinterpret_cast<const char*>(data->bytes.data()), data->bytes.size()));
        } catch (const std::exception&) {
            continue; // a broken livery degrades to the base scheme, it never fails the aircraft
        }
        if (def.aircraft == aircraftDefId) {
            def.id = name; // the parser cannot know the asset name; fill it from the file stem
            return def;
        }
    }
    return std::nullopt;
}

std::shared_ptr<AssetBase> AssetManager::loadDefBytes(AssetType type, const char* name) {
    switch (type) {
    case AssetType::EntityDef:
        return loadEntityDef(name);
    case AssetType::SensorDef:
        return loadSensorDef(name);
    case AssetType::Weapon:
        return loadWeaponDef(name);
    default:
        return nullptr; // not a def type
    }
}

std::optional<std::string> AssetManager::loadConfig(const char* name) {
    for (auto& pack : m_packs) {
        if (auto result = pack->loadConfig(name))
            return result;
    }
    return std::nullopt;
}

std::optional<std::string> AssetManager::resolveTilePath(const char* terrainId, uint8_t face, uint8_t level, uint32_t i,
                                                         uint32_t j, TileLayer layer) {
    for (auto& pack : m_packs) {
        if (auto result = pack->resolveTilePath(terrainId, face, level, i, j, layer))
            return result;
    }
    return std::nullopt;
}

bool AssetManager::hasPacks() const {
    return !m_packs.empty();
}

std::vector<std::string> AssetManager::listMissions() const {
    return listAssets(AssetType::Mission);
}

std::vector<std::string> AssetManager::listAssets(AssetType type) const {
    std::vector<std::string> result;
    for (auto& pack : m_packs) {
        for (auto& id : pack->listAssets(type)) {
            bool dup = false;
            for (auto& existing : result)
                if (existing == id) {
                    dup = true;
                    break;
                }
            if (!dup)
                result.push_back(id);
        }
    }
    return result;
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

} // namespace fl
