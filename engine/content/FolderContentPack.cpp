// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/FolderContentPack.h"

#include "IFilesystem.h"
#include "ILogger.h"

#include <algorithm>
#include <array>

// Asset type → subdirectory, primary extension, fallback extension (empty = no fallback)
struct AssetPathInfo {
    const char* subdir;
    const char* ext;
    const char* extFallback;
};

static constexpr std::array<AssetPathInfo, static_cast<size_t>(AssetType::Count)> kAssetPaths = {{
    {"aircraft", ".glb", ".gltf"}, // Mesh
    {"textures", ".ktx2", ".png"}, // Texture
    {"audio", ".ogg", ""},         // Audio
    {"aircraft", ".toml", ""},     // FlightModel
    {"missions", ".yaml", ""},     // Mission
    {"terrain", ".json", ""},      // Terrain
    {"ai", ".lua", ""},            // AIScript
    {"entities", ".toml", ""},     // EntityDef
}};
static_assert(kAssetPaths.size() == static_cast<size_t>(AssetType::Count),
              "kAssetPaths out of sync with AssetType enum");

FolderContentPack::FolderContentPack(IFilesystem& fs, ILogger& logger, std::string modDir, Manifest manifest)
    : m_fs(fs), m_logger(logger), m_modDir(std::move(modDir)), m_manifest(std::move(manifest)) {}

IContentPack::Status FolderContentPack::init() {
    return Status::Ready;
}

bool FolderContentPack::configure(IWindow* /*window*/) {
    return true;
}

std::string FolderContentPack::resolveAssetPath(const char* name, AssetType type) const {
    const auto& info = kAssetPaths[static_cast<uint8_t>(type)];
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

std::optional<std::string> FolderContentPack::resolveTerrainChunk(const char* terrainId, uint32_t chunkX,
                                                                  uint32_t chunkY, uint32_t lod) const {
    auto pad4 = [](uint32_t v) -> std::string {
        std::string s = std::to_string(v);
        return s.size() < 4 ? std::string(4 - s.size(), '0') + s : s;
    };
    std::string path = m_modDir + "/terrain/" + terrainId + "/lod" + std::to_string(lod) + "/chunk_" + pad4(chunkX) +
                       "_" + pad4(chunkY) + ".png";
    if (!m_fs.fileExists(PathDomain::Assets, path.c_str()))
        return std::nullopt;
    return path;
}

std::vector<std::string> FolderContentPack::listAssets(AssetType type) const {
    const auto& info = kAssetPaths[static_cast<uint8_t>(type)];
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
