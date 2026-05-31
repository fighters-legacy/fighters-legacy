// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "content/IContentPack.h"
#include <string>

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
        std::string version;
        std::string engineApi;
        int priority = 0;
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
    int priority() const override {
        return m_manifest.priority;
    }
    const char* rootDirectory() const override {
        return m_modDir.c_str();
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

    std::vector<std::string> listAssets(AssetType type) const override;

    std::optional<std::string> loadConfig(const char* name) const override;

    std::optional<std::string> resolveTerrainChunk(const char* terrainId, uint32_t chunkX, uint32_t chunkY,
                                                   uint32_t lod) const override;

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
