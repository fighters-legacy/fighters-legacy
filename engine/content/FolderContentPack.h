// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "content/IContentPack.h"
#include "content/TrustLevel.h"
#include <optional>
#include <string>
#include <vector>

namespace fl {

class IFilesystem;
class ILogger;

// IContentPack implementation for directory-based mods (Lua + assets).
// Reads assets as raw bytes from a mod directory via IFilesystem.
// Compiled plugin loading is out of scope for Phase 1.
class FolderContentPack final : public IContentPack {
  public:
    struct Manifest {
        std::string name;
        std::string id;
        std::string namespaceId; // def-id prefix; defaults to `id` when the manifest omits it
        std::string version;
        std::string engineApi;
        int priority = 0;
        TrustLevel trustLevel = TrustLevel::Unsigned;
        bool nativePlugin = false;
    };

    FolderContentPack(IFilesystem& fs, ILogger& logger, std::string modDir, Manifest manifest);

    const char* name() const override {
        return m_manifest.name.c_str();
    }
    const char* version() const override {
        return m_manifest.version.c_str();
    }
    const char* id() const override {
        return m_manifest.id.c_str();
    }
    const char* namespaceId() const override {
        return m_manifest.namespaceId.empty() ? m_manifest.id.c_str() : m_manifest.namespaceId.c_str();
    }
    int priority() const override {
        return m_manifest.priority;
    }
    const char* rootDirectory() const override {
        return m_modDir.c_str();
    }
    TrustLevel getTrustLevel() const override {
        return m_manifest.trustLevel;
    }
    bool isNativePlugin() const override {
        return m_manifest.nativePlugin;
    }

    Status init() override;
    bool configure(IWindow* window) override;

    bool hasAsset(const char* name, AssetType type) const override;

    std::optional<MeshData> loadMesh(const char* name) override;
    std::optional<TextureData> loadTexture(const char* name) override;
    std::optional<AudioBuffer> loadAudio(const char* name) override;
    std::optional<FlightModel> loadFlightModel(const char* name) override;
    std::optional<MissionData> loadMission(const char* name) override;
    std::optional<TerrainData> loadTerrain(const char* name) override;
    std::optional<AIScript> loadAIScript(const char* name) override;
    std::optional<EntityDefData> loadEntityDef(const char* name) override;
    std::optional<SensorDefData> loadSensorDef(const char* name) override;
    std::optional<WeaponDefData> loadWeaponDef(const char* name) override;
    std::optional<ManualProse> loadManualProse(const char* name) override;
    std::optional<LiveryData> loadLivery(const char* name) override;
    std::optional<AirportDefData> loadAirportDef(const char* name) override;
    std::optional<GameModeData> loadGameMode(const char* name) override;
    std::optional<TheaterDefData> loadTheater(const char* name) override;

    std::vector<std::string> listAssets(AssetType type) const override;

    std::optional<std::string> loadConfig(const char* name) const override;
    std::optional<std::vector<uint8_t>> loadPackFile(const char* relPath) const override;

    std::optional<std::string> resolveTilePath(const char* terrainId, uint8_t face, uint8_t level, uint32_t i,
                                               uint32_t j, TileLayer layer) const override;

  private:
    // Returns the asset file path for the given name and type, trying the primary
    // extension first. Returns an empty string if neither extension exists.
    std::string resolveAssetPath(const char* name, AssetType type) const;

    // Reads a file at the given path (relative to PathDomain::Assets) into bytes.
    // Returns nullopt on open failure.
    template <typename T> std::optional<T> loadBytes(const char* assetName, AssetType type) const;

    IFilesystem& m_fs;
    ILogger& m_logger;
    std::string m_modDir;
    Manifest m_manifest;
};

} // namespace fl
