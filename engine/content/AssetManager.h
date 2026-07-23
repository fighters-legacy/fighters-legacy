// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "content/AssetTypes.h"
#include "content/AssetValidator.h"
#include "content/IContentPack.h"
#include "content/LiveryDef.h"

#include "IFilesystemWatcher.h" // IFilesystemWatcher::Event in mapEventsToAssets (#152)

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

class IFilesystemWatcher;
class ILogger;
class IWindow;

// One asset that a hot-reload run changed (#152): its type + lowercase asset name (may contain '/',
// e.g. "f5e/f5e"). Callers evict the corresponding GPU/engine caches keyed on (type, name).
struct ChangedAsset {
    AssetType type;
    std::string name;
};

// Result of a hot-reload poll: `changed` = the assets whose files changed (already evicted from the
// byte cache); `unmatched` = event paths owned by no pack asset dir (locale files, editor temp files),
// which the caller may route elsewhere (Localization::reload).
struct HotReloadReport {
    std::vector<ChangedAsset> changed;
    std::vector<std::string> unmatched;
};

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
    std::shared_ptr<SensorDefData> loadSensorDef(const char* name);
    std::shared_ptr<WeaponDefData> loadWeaponDef(const char* name);
    std::shared_ptr<ManualProse> loadManualProse(const char* name);
    std::shared_ptr<LiveryData> loadLivery(const char* name);
    std::shared_ptr<AirportDefData> loadAirportDef(const char* name);
    std::shared_ptr<GameModeData> loadGameMode(const char* name);  // #521
    std::shared_ptr<TheaterDefData> loadTheater(const char* name); // #847

    // Raw pack-relative file bytes, first pack in priority order that provides it (#847). Uncached
    // (like loadConfig). nullopt if no pack has it.
    std::optional<std::vector<uint8_t>> loadPackFile(const char* relPath);

    // The highest-priority livery (#845) targeting the given aircraft DEF ID (e.g. "fl-base:f5e"),
    // or nullopt if none is installed. Walks the installed liveries in priority order and returns the
    // first whose `aircraft` matches; a malformed livery is skipped (degrades to base, never fails).
    // Parses TOML, so a per-frame caller should CACHE the result rather than call it every entity.
    std::optional<LiveryDef> liveryForAircraft(const char* aircraftDefId);

    // Type-generic accessor for the def asset types (EntityDef / SensorDef / ...), returning the
    // same cached bytes the typed loader above would. ContentIndex uses it to shallow-parse an id
    // out of every def without a switch over asset types; returns nullptr for a non-def type.
    std::shared_ptr<AssetBase> loadDefBytes(AssetType type, const char* name);

    // Walks the priority stack. Returns the raw text of the first pack
    // that returns non-nullopt for loadConfig(name). Not cached.
    std::optional<std::string> loadConfig(const char* name);

    // Walks the priority stack; returns the resolved path from the first pack
    // that provides the given chunk, or nullopt if none do.
    std::optional<std::string> resolveTilePath(const char* terrainId, uint8_t face, uint8_t level, uint32_t i,
                                               uint32_t j, TileLayer layer);

    // Returns the highest-priority content pack that owns the named asset, or nullptr.
    // ContentIndex uses it to attribute a def id to the pack that declared it, so a def whose id
    // prefix disagrees with its pack's declared namespace can be reported.
    const IContentPack* findPackForAsset(AssetType type, const char* name) const;

    // Returns the root directory of the first content pack that owns the named asset.
    // Used by LuaController to configure require() to the correct pack ai/ directory.
    // Returns "" if no pack has the asset or the owning pack has no filesystem root.
    std::string findPackRootForAsset(AssetType type, const char* name) const;

    // Returns true if at least one active content pack is loaded.
    bool hasPacks() const;

    // Returns the union of mission asset IDs across all active packs (first-wins dedup).
    std::vector<std::string> listMissions() const;

    // Returns the union of asset names of the given type across all active packs (first-wins dedup).
    std::vector<std::string> listAssets(AssetType type) const;

    // One mounted pack's identity, reported to the server at connect for content-consistency negotiation
    // (#872). Content hashing is a later addition (the wire reserves the field).
    struct PackManifestInfo {
        std::string id;
        std::string version;
    };
    // The mounted content packs' {id, version}, in priority order. Used to build the client pack manifest
    // in MsgConnectRequest (#853/#872).
    std::vector<PackManifestInfo> packManifest() const;

    // Hot-reload support (sandbox/editor mode only, #152). Pass the watcher from Platform. Registers
    // each pack's asset SUBDIRS (aircraft/, textures/, entities/, ... — NOT the pack root, and NOT
    // terrain/, whose tile trees are huge and stream outside the asset cache) with the watcher
    // recursively. processHotReload() must be called once per frame from the game loop.
    void enableHotReload(IFilesystemWatcher& watcher);

    // Poll the watcher, map changed files to assets, evict them from the byte cache, and report both
    // the changed assets and any unmatched paths. No implicit full clears (that thrashed on every
    // editor temp-file write). Returns an empty report when no watcher or no events.
    HotReloadReport processHotReload();

    // Map watcher events to assets WITHOUT mutating the cache (pure w.r.t. the cache; reads only the
    // immutable pack list, so it is safe to call from any thread — fl-server maps on the main thread
    // and evicts on the sim thread).
    [[nodiscard]] std::vector<ChangedAsset> mapEventsToAssets(std::span<const IFilesystemWatcher::Event> events) const;

    // Evict specific assets / everything from the byte cache; bumps cacheGeneration().
    void evict(std::span<const ChangedAsset> assets);
    void evictAll();

    // Monotonic counter bumped by every eviction. Downstream caches that cannot subscribe to the
    // fine-grained report (the flight-model resolver) self-invalidate by watching this.
    [[nodiscard]] uint64_t cacheGeneration() const noexcept {
        return m_cacheGeneration;
    }

  private:
    static std::string cacheKey(AssetType type, const char* name);

    template <typename T>
    std::shared_ptr<T> loadAsset(AssetType type, const char* name,
                                 std::optional<T> (IContentPack::*loader)(const char*));

    std::unordered_map<std::string, std::shared_ptr<void>> m_cache;
    std::vector<std::unique_ptr<IContentPack>> m_packs;
    ILogger& m_logger;
    AssetValidator m_validator;
    IFilesystemWatcher* m_watcher = nullptr;
    uint64_t m_cacheGeneration = 0; // bumped by evict/evictAll (#152)
};

} // namespace fl
