// SPDX-License-Identifier: GPL-3.0-or-later
//
// Linking exception: content pack implementors (mods, plugins) may link against
// this header — part of the Vendorable Interface Set, together with AssetTypes.h
// and TrustLevel.h — without being required to license their work under GPL v3.
// See the "Content Pack Linking Exception" section of GOVERNANCE.md for the full,
// authoritative exception text (scope, what "link against" permits, what remains GPL).
#pragma once

#include "content/AssetTypes.h"
#include "content/TrustLevel.h"
#include <optional>
#include <string>
#include <vector>

namespace fl {

class IWindow; // configure() passes the window for packs that display a config UI

// Per-tile data layer selecting which cube-sphere terrain file a pack resolves for a TileKey.
// Path suffix convention (see FolderContentPack::resolveTilePath):
//   Height    → terrain/<id>/f<face>/l<level>/tile_<i>_<j>.png       (16-bit grayscale heightmap)
//   LandCover → terrain/<id>/f<face>/l<level>/tile_<i>_<j>_lc.png    (WorldCover class map)
//   Satellite → terrain/<id>/f<face>/l<level>/tile_<i>_<j>_sat.ktx2  (orthophoto; path reserved)
enum class TileLayer : uint8_t {
    Height,
    LandCover,
    Satellite,
};

class IContentPack {
  public:
    virtual ~IContentPack() = default;

    enum class Status : uint8_t { Ready, NeedsConfiguration };

    virtual const char* name() const = 0;
    virtual const char* version() const = 0;
    virtual const char* id() const = 0;
    // The prefix every def id in this pack is expected to carry: a def id is "<namespace>:<local>",
    // e.g. "fl-base:apq159" in a pack whose namespace is "fl-base". Declared as `[mod] namespace` in
    // manifest.toml and defaulting to id(); it exists because the two are NOT the same string in
    // practice (fl-base-pack's manifest id is "fl-base-pack" while its def ids say "fl-base:"), and
    // nothing checked them against each other before #810. Never contains ':'.
    virtual const char* namespaceId() const = 0;
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
    virtual std::optional<SensorDefData> loadSensorDef(const char* name) = 0;
    virtual std::optional<WeaponDefData> loadWeaponDef(const char* name) = 0;
    virtual std::optional<ManualProse> loadManualProse(const char* name) = 0;

    // Livery TOML (#845), texture-set indirection by material slot. NON-pure with a nullopt default
    // so existing IContentPack implementors (including out-of-tree packs) keep compiling without
    // change — a pack that ships no liveries simply inherits "no livery", the same degrade-to-base
    // path a missing livery already takes. FolderContentPack overrides it to serve liveries/<name>.toml.
    virtual std::optional<LiveryData> loadLivery(const char* name) {
        (void)name;
        return std::nullopt;
    }

    virtual std::vector<std::string> listAssets(AssetType type) const = 0;

    // Returns the raw text of "<modDir>/data/<name>", or nullopt if not present.
    // Used for data-driven config files (e.g. difficulty.toml) that mods can override.
    virtual std::optional<std::string> loadConfig(const char* name) const = 0;

    // Returns the path (relative to PathDomain::Assets) of the cube-sphere terrain tile file for
    // TileKey{face, level, i, j} and the given data layer, or nullopt if this pack does not provide
    // it. Synchronous; called before queuing an async read via IAsyncFilesystem.
    virtual std::optional<std::string> resolveTilePath(const char* terrainId, uint8_t face, uint8_t level, uint32_t i,
                                                       uint32_t j, TileLayer layer) const = 0;

    // Returns the trust tier assigned to this pack based on its manifest signature.
    // GPG verification is Phase 6 work; in Phase 2 only the enum value is stored.
    virtual TrustLevel getTrustLevel() const = 0;

    // Returns true if a native compiled plugin (.dll/.so/.dylib) was found alongside
    // this pack's manifest and loaded to provide it. Always false for directory-only mods.
    virtual bool isNativePlugin() const = 0;

    // Exported symbol name for compiled content pack shared libraries.
    // A plugin must export a function with this name and signature:
    //   extern "C" IContentPack* fighters_legacy_create_pack();
    static constexpr const char* kFactorySymbol = "fighters_legacy_create_pack";
};

} // namespace fl
