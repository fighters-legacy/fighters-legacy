// SPDX-License-Identifier: GPL-3.0-or-later
//
// Linking exception: content pack implementors (mods, plugins) may link against
// this header without being required to license their work under GPL v3.
// See GOVERNANCE.md for the full exception text.
#pragma once

#include "content/AssetTypes.h"
#include <optional>
#include <string>
#include <vector>

class IWindow; // configure() passes the window for packs that display a config UI

class IContentPack {
  public:
    virtual ~IContentPack() = default;

    enum class Status : uint8_t { Ready, NeedsConfiguration };

    virtual const char* name() const = 0;
    virtual const char* version() const = 0;
    virtual const char* id() const = 0;
    virtual int priority() const = 0;
    // Returns the root directory of this pack relative to PathDomain::Assets
    // (used for hot-reload path registration), or nullptr for packs with no
    // filesystem root (e.g. compiled plugins that load assets from memory).
    virtual const char* rootDirectory() const = 0;

    // init() is called once by AssetManager before any load. Returns Ready when
    // the pack is usable immediately. Returns NeedsConfiguration when configure()
    // must be called first (e.g. a plugin that presents a setup UI).
    virtual Status init() = 0;
    virtual bool configure(IWindow* window) = 0;

    virtual bool hasAsset(const char* name, AssetType type) const = 0;

    virtual std::optional<MeshData> loadMesh(const char* name) = 0;
    virtual std::optional<TextureData> loadTexture(const char* name) = 0;
    virtual std::optional<AudioBuffer> loadAudio(const char* name) = 0;
    virtual std::optional<FlightModel> loadFlightModel(const char* name) = 0;
    virtual std::optional<MissionData> loadMission(const char* name) = 0;
    virtual std::optional<TerrainData> loadTerrain(const char* name) = 0;
    virtual std::optional<AIScript> loadAIScript(const char* name) = 0;
    virtual std::optional<EntityDefData> loadEntityDef(const char* name) = 0;

    virtual std::vector<std::string> listAssets(AssetType type) const = 0;

    // Returns the raw text of "<modDir>/data/<name>", or nullopt if not present.
    // Used for data-driven config files (e.g. difficulty.toml) that mods can override.
    virtual std::optional<std::string> loadConfig(const char* name) const = 0;

    // Returns the path (relative to PathDomain::Assets) of a terrain chunk PNG,
    // or nullopt if this pack does not provide it. Synchronous; called before
    // queuing an async read via IAsyncFilesystem.
    virtual std::optional<std::string> resolveTerrainChunk(const char* terrainId, uint32_t chunkX, uint32_t chunkY,
                                                           uint32_t lod) const = 0;

    // Exported symbol name for compiled content pack shared libraries.
    // A plugin must export a function with this name and signature:
    //   extern "C" IContentPack* fighters_legacy_create_pack();
    static constexpr const char* kFactorySymbol = "fighters_legacy_create_pack";
};
