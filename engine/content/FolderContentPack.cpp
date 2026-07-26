// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/FolderContentPack.h"

#include "content/AssetPaths.h" // the kAssetPaths table (single source of truth, #152/#836)

#include "IFilesystem.h"
#include "ILogger.h"

namespace fl {

FolderContentPack::FolderContentPack(IFilesystem& fs, ILogger& logger, std::string modDir, Manifest manifest)
    : m_fs(fs), m_logger(logger), m_modDir(std::move(modDir)), m_manifest(std::move(manifest)) {}

IContentPack::Status FolderContentPack::init() {
    return Status::Ready;
}

bool FolderContentPack::configure(IWindow* /*window*/) {
    return true;
}

std::string FolderContentPack::resolveAssetPath(const char* name, AssetType type) const {
    const AssetPathInfo& info = assetPathInfo(type);
    std::string primary = m_modDir + "/" + info.subdir + "/" + name + info.ext;
    if (m_fs.fileExists(PathDomain::Assets, primary.c_str()))
        return primary;
    if (info.extFallback[0] != '\0') {
        std::string fallback = m_modDir + "/" + info.subdir + "/" + name + info.extFallback;
        if (m_fs.fileExists(PathDomain::Assets, fallback.c_str()))
            return fallback;
    }
    return {};
}

bool FolderContentPack::hasAsset(const char* name, AssetType type) const {
    return !resolveAssetPath(name, type).empty();
}

template <typename T> std::optional<T> FolderContentPack::loadBytes(const char* assetName, AssetType type) const {
    std::string path = resolveAssetPath(assetName, type);
    if (path.empty())
        return std::nullopt;

    int handle = m_fs.openFile(PathDomain::Assets, path.c_str(), false);
    if (handle < 0) {
        m_logger.log(LogLevel::Error, __FILE__, __LINE__, ("failed to open: " + path).c_str());
        return std::nullopt;
    }

    std::size_t size = m_fs.getFileSize(handle);
    T result;
    result.name = assetName;
    result.bytes.resize(size);
    m_fs.readFile(handle, result.bytes.data(), size);
    m_fs.closeFile(handle);
    return result;
}

std::optional<MeshData> FolderContentPack::loadMesh(const char* name) {
    return loadBytes<MeshData>(name, AssetType::Mesh);
}
std::optional<TextureData> FolderContentPack::loadTexture(const char* name) {
    return loadBytes<TextureData>(name, AssetType::Texture);
}
std::optional<AudioBuffer> FolderContentPack::loadAudio(const char* name) {
    return loadBytes<AudioBuffer>(name, AssetType::Audio);
}
std::optional<FlightModel> FolderContentPack::loadFlightModel(const char* name) {
    return loadBytes<FlightModel>(name, AssetType::FlightModel);
}
std::optional<MissionData> FolderContentPack::loadMission(const char* name) {
    return loadBytes<MissionData>(name, AssetType::Mission);
}
std::optional<TerrainData> FolderContentPack::loadTerrain(const char* name) {
    return loadBytes<TerrainData>(name, AssetType::Terrain);
}
std::optional<AIScript> FolderContentPack::loadAIScript(const char* name) {
    return loadBytes<AIScript>(name, AssetType::AIScript);
}
std::optional<EntityDefData> FolderContentPack::loadEntityDef(const char* name) {
    return loadBytes<EntityDefData>(name, AssetType::EntityDef);
}
std::optional<SensorDefData> FolderContentPack::loadSensorDef(const char* name) {
    return loadBytes<SensorDefData>(name, AssetType::SensorDef);
}
std::optional<WeaponDefData> FolderContentPack::loadWeaponDef(const char* name) {
    return loadBytes<WeaponDefData>(name, AssetType::Weapon);
}
std::optional<ManualProse> FolderContentPack::loadManualProse(const char* name) {
    return loadBytes<ManualProse>(name, AssetType::Manual);
}
std::optional<LiveryData> FolderContentPack::loadLivery(const char* name) {
    return loadBytes<LiveryData>(name, AssetType::Livery);
}
std::optional<AirportDefData> FolderContentPack::loadAirportDef(const char* name) {
    return loadBytes<AirportDefData>(name, AssetType::Airport);
}
std::optional<GameModeData> FolderContentPack::loadGameMode(const char* name) {
    return loadBytes<GameModeData>(name, AssetType::GameMode);
}

std::optional<TheaterDefData> FolderContentPack::loadTheater(const char* name) {
    return loadBytes<TheaterDefData>(name, AssetType::Theater);
}

std::optional<ZonePolicyData> FolderContentPack::loadZonePolicy(const char* name) {
    return loadBytes<ZonePolicyData>(name, AssetType::ZonePolicy);
}

std::optional<std::vector<uint8_t>> FolderContentPack::loadPackFile(const char* relPath) const {
    if (!relPath || !*relPath)
        return std::nullopt;
    // Sanitize: a pack-relative path only. Reject absolute paths, drive letters, backslashes, and any
    // parent-directory component so a manifest/campaign cannot read outside its pack.
    std::string_view p(relPath);
    if (p.front() == '/' || p.find('\\') != std::string_view::npos)
        return std::nullopt;
    if (p.size() >= 2 && p[1] == ':') // Windows drive letter
        return std::nullopt;
    for (size_t i = 0; i < p.size();) {
        size_t slash = p.find('/', i);
        std::string_view seg = p.substr(i, slash == std::string_view::npos ? std::string_view::npos : slash - i);
        if (seg == "..")
            return std::nullopt;
        if (slash == std::string_view::npos)
            break;
        i = slash + 1;
    }
    std::string path = m_modDir + "/" + std::string(p);
    if (!m_fs.fileExists(PathDomain::Assets, path.c_str()))
        return std::nullopt;
    int handle = m_fs.openFile(PathDomain::Assets, path.c_str(), false);
    if (handle < 0)
        return std::nullopt;
    std::size_t size = m_fs.getFileSize(handle);
    std::vector<uint8_t> bytes(size);
    if (size > 0)
        m_fs.readFile(handle, bytes.data(), size);
    m_fs.closeFile(handle);
    return bytes;
}

std::optional<std::string> FolderContentPack::loadConfig(const char* name) const {
    std::string path = m_modDir + "/data/" + name;
    if (!m_fs.fileExists(PathDomain::Assets, path.c_str()))
        return std::nullopt;
    int handle = m_fs.openFile(PathDomain::Assets, path.c_str(), false);
    if (handle < 0)
        return std::nullopt;
    std::size_t size = m_fs.getFileSize(handle);
    std::string content(size, '\0');
    m_fs.readFile(handle, content.data(), size);
    m_fs.closeFile(handle);
    return content;
}

std::optional<std::string> FolderContentPack::resolveTilePath(const char* terrainId, uint8_t face, uint8_t level,
                                                              uint32_t i, uint32_t j, TileLayer layer) const {
    const char* suffix = ".png";
    switch (layer) {
    case TileLayer::Height:
        suffix = ".png";
        break;
    case TileLayer::LandCover:
        suffix = "_lc.png";
        break;
    case TileLayer::Satellite:
        suffix = "_sat.ktx2";
        break;
    }
    std::string path = m_modDir + "/terrain/" + terrainId + "/f" + std::to_string(static_cast<unsigned>(face)) + "/l" +
                       std::to_string(static_cast<unsigned>(level)) + "/tile_" + std::to_string(i) + "_" +
                       std::to_string(j) + suffix;
    if (!m_fs.fileExists(PathDomain::Assets, path.c_str()))
        return std::nullopt;
    return path;
}

std::vector<std::string> FolderContentPack::listAssets(AssetType type) const {
    const AssetPathInfo& info = assetPathInfo(type);
    std::string dir = m_modDir + "/" + info.subdir;

    auto entries = m_fs.scanDirectory(PathDomain::Assets, dir.c_str());
    std::vector<std::string> names;
    for (auto& entry : entries) {
        if (entry.isDirectory)
            continue;
        // Strip primary extension
        auto stripExt = [&](const std::string& ext) -> std::string {
            if (entry.name.size() > ext.size() &&
                entry.name.compare(entry.name.size() - ext.size(), ext.size(), ext) == 0)
                return entry.name.substr(0, entry.name.size() - ext.size());
            return {};
        };
        std::string base = stripExt(info.ext);
        if (base.empty() && info.extFallback[0] != '\0')
            base = stripExt(info.extFallback);
        if (!base.empty())
            names.push_back(std::move(base));
    }
    return names;
}

} // namespace fl
