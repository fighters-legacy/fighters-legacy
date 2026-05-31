// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "content/IContentPack.h"
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class IFilesystemWatcher;
class ILogger;
class IWindow;

// Threading: all methods must be called from the main thread.
class AssetManager {
  public:
    explicit AssetManager(std::vector<std::unique_ptr<IContentPack>> packs, ILogger& logger);

    // Calls init() on every pack. If init() returns Ready, the pack is active.
    // If init() returns NeedsConfiguration, configure(window) is called; packs
    // whose configure() returns false are dropped with a Warn log.
    // Must be called once before any load*() call.
    // window may be nullptr — NeedsConfiguration packs will be dropped (Warn logged).
    void initialize(IWindow* window);

    // Each method walks the priority stack (index 0 = highest priority), calling
    // the corresponding IContentPack method with the normalized lowercase name.
    // Returns the first non-nullopt result as a shared_ptr (cached).
    // Returns nullptr if no pack provides the asset.
    std::shared_ptr<MeshData> loadMesh(const char* name);
    std::shared_ptr<TextureData> loadTexture(const char* name);
    std::shared_ptr<AudioBuffer> loadAudio(const char* name);
    std::shared_ptr<FlightModel> loadFlightModel(const char* name);
    std::shared_ptr<MissionData> loadMission(const char* name);
    std::shared_ptr<TerrainData> loadTerrain(const char* name);
    std::shared_ptr<AIScript> loadAIScript(const char* name);
    std::shared_ptr<EntityDefData> loadEntityDef(const char* name);

    // Walks the priority stack. Returns the raw text of the first pack
    // that returns non-nullopt for loadConfig(name). Not cached.
    std::optional<std::string> loadConfig(const char* name);

    // Walks the priority stack; returns the resolved path from the first pack
    // that provides the given chunk, or nullopt if none do.
    std::optional<std::string> resolveTerrainChunk(const char* terrainId, uint32_t chunkX, uint32_t chunkY,
                                                   uint32_t lod);

    // Hot-reload support (sandbox/editor mode only). Pass the watcher from Platform.
    // Registers each pack's rootDirectory() with the watcher (recursive).
    // processHotReload() must be called once per frame from the game loop.
    void enableHotReload(IFilesystemWatcher& watcher);
    void processHotReload();

  private:
    static std::string cacheKey(AssetType type, const char* name);

    template <typename T>
    std::shared_ptr<T> loadAsset(AssetType type, const char* name,
                                 std::optional<T> (IContentPack::*loader)(const char*));

    std::unordered_map<std::string, std::shared_ptr<void>> m_cache;
    std::vector<std::unique_ptr<IContentPack>> m_packs;
    ILogger& m_logger;
    IFilesystemWatcher* m_watcher = nullptr;
};
