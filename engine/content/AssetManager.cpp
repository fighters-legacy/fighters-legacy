// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/AssetManager.h"

#include "content/AssetPaths.h"

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

std::shared_ptr<AirportDefData> AssetManager::loadAirportDef(const char* name) {
    return loadAsset<AirportDefData>(AssetType::Airport, name, &IContentPack::loadAirportDef);
}

std::shared_ptr<GameModeData> AssetManager::loadGameMode(const char* name) {
    return loadAsset<GameModeData>(AssetType::GameMode, name, &IContentPack::loadGameMode);
}

std::shared_ptr<TheaterDefData> AssetManager::loadTheater(const char* name) {
    return loadAsset<TheaterDefData>(AssetType::Theater, name, &IContentPack::loadTheater);
}

std::optional<std::vector<uint8_t>> AssetManager::loadPackFile(const char* relPath) {
    for (auto& pack : m_packs)
        if (auto bytes = pack->loadPackFile(relPath))
            return bytes;
    return std::nullopt;
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
    // Watch each pack's asset SUBDIRS, not the pack root (#152): the bundled base-terrain pack's root
    // holds a huge terrain/ tile tree that would make every poll a full rescan, and terrain streams
    // outside the asset cache anyway. Dedup repeated subdirs (aircraft/ serves Mesh + FlightModel);
    // skip Terrain.
    for (auto& pack : m_packs) {
        const char* root = pack->rootDirectory();
        if (!root)
            continue;
        std::vector<std::string> seen;
        for (uint8_t t = 0; t < static_cast<uint8_t>(AssetType::Count); ++t) {
            const auto type = static_cast<AssetType>(t);
            if (type == AssetType::Terrain)
                continue;
            const char* subdir = assetPathInfo(type).subdir;
            std::string dir = std::string(root) + "/" + subdir;
            if (std::find(seen.begin(), seen.end(), dir) != seen.end())
                continue;
            seen.push_back(dir);
            watcher.watch(PathDomain::Assets, dir.c_str(), /*recursive=*/true);
        }
    }
}

std::vector<ChangedAsset> AssetManager::mapEventsToAssets(std::span<const IFilesystemWatcher::Event> events) const {
    std::vector<ChangedAsset> out;
    for (const auto& ev : events) {
        // Longest-prefix match against each pack root, then reverse-map the remainder. Event paths are
        // relative to PathDomain::Assets (the same space as pack roots), forward-slashed.
        const IContentPack* bestPack = nullptr;
        size_t bestLen = 0;
        for (auto& pack : m_packs) {
            const char* root = pack->rootDirectory();
            if (!root || !*root)
                continue;
            std::string prefix = std::string(root) + "/";
            if (ev.path.size() > prefix.size() && ev.path.compare(0, prefix.size(), prefix) == 0 &&
                prefix.size() > bestLen) {
                bestPack = pack.get();
                bestLen = prefix.size();
            }
        }
        if (!bestPack)
            continue; // not under any pack — caller sees it in HotReloadReport::unmatched
        std::string rel = ev.path.substr(bestLen);
        if (auto mapped = assetFromPackRelativePath(rel)) {
            // Dedup (multiple events for the same asset in one poll).
            const ChangedAsset ca{mapped->first, mapped->second};
            const bool dup = std::any_of(out.begin(), out.end(),
                                         [&](const ChangedAsset& c) { return c.type == ca.type && c.name == ca.name; });
            if (!dup)
                out.push_back(ca);
        }
    }
    return out;
}

void AssetManager::evict(std::span<const ChangedAsset> assets) {
    for (const auto& a : assets)
        m_cache.erase(cacheKey(a.type, a.name.c_str()));
    if (!assets.empty())
        ++m_cacheGeneration;
}

void AssetManager::evictAll() {
    m_cache.clear();
    ++m_cacheGeneration;
}

HotReloadReport AssetManager::processHotReload() {
    HotReloadReport report;
    if (!m_watcher)
        return report;
    auto events = m_watcher->pollEvents();
    if (events.empty())
        return report;

    report.changed = mapEventsToAssets(events);
    // Record paths that mapped to no asset (locale files, editor temp files) so the caller can route
    // them (Localization::reload) or ignore them.
    for (const auto& ev : events) {
        bool underPack = false;
        for (auto& pack : m_packs) {
            const char* root = pack->rootDirectory();
            if (root && *root) {
                std::string prefix = std::string(root) + "/";
                if (ev.path.size() > prefix.size() && ev.path.compare(0, prefix.size(), prefix) == 0) {
                    underPack = true;
                    break;
                }
            }
        }
        if (!underPack)
            report.unmatched.push_back(ev.path);
    }

    if (!report.changed.empty()) {
        evict(report.changed);
        m_logger.log(
            LogLevel::Debug, __FILE__, __LINE__,
            (std::string("hot-reload: evicted ") + std::to_string(report.changed.size()) + " asset(s)").c_str());
    }
    return report;
}

} // namespace fl
