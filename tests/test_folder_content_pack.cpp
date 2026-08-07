// SPDX-License-Identifier: GPL-3.0-or-later
//
// FolderContentPack: the on-disk pack layout and its path sanitizer (#1145).
//
// This is the class a mod author's directory actually goes through. Two things about it are worth
// pinning down. First, the layout table in AssetPaths.cpp is only correct if something pastes it
// into a real path for every asset type — a wrong subdir or extension is invisible to any test that
// uses a mock pack. Second, loadPackFile() takes a path out of a MANIFEST, which is untrusted
// content, so its sanitizer is a security boundary and is tested as one.

#include <catch2/catch_test_macros.hpp>

#include "content/AssetPaths.h"
#include "content/FolderContentPack.h"
#include "mock_hal.h"

#include <algorithm>
#include <set>
#include <string>

using namespace fl;

namespace {

// A filesystem whose open() can fail for a file that exists — the "it is there but I cannot read it"
// case (permissions, a vanished file, a bad handle) that the plain mock cannot express because it
// answers both questions from one map.
struct UnreadableFilesystem final : MockFilesystem {
    std::set<std::string> unopenable;
    int openFile(PathDomain d, const char* path, bool write) override {
        if (unopenable.count(path ? path : ""))
            return -1;
        return MockFilesystem::openFile(d, path, write);
    }
};

constexpr const char* kRoot = "mods/testpack";

FolderContentPack::Manifest manifestOf() {
    FolderContentPack::Manifest m;
    m.name = "Test Pack";
    m.id = "test";
    m.version = "1.0.0";
    return m;
}

std::string pathFor(AssetType t, std::string_view name, bool fallback = false) {
    const AssetPathInfo& info = assetPathInfo(t);
    return std::string(kRoot) + "/" + info.subdir + "/" + std::string(name) + (fallback ? info.extFallback : info.ext);
}

} // namespace

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

TEST_CASE("FolderContentPack: namespaceId falls back to id (#1145)", "[content][pack]") {
    MockFilesystem fs;
    MockLogger log;
    FolderContentPack::Manifest m = manifestOf();
    FolderContentPack pack(fs, log, kRoot, m);

    CHECK(std::string(pack.id()) == "test");
    CHECK(std::string(pack.namespaceId()) == "test"); // omitted in the manifest: it IS the id
    CHECK(std::string(pack.rootDirectory()) == kRoot);
    CHECK(pack.init() == IContentPack::Status::Ready);
    CHECK(pack.configure(nullptr));

    m.namespaceId = "other";
    FolderContentPack renamed(fs, log, kRoot, m);
    CHECK(std::string(renamed.namespaceId()) == "other");
}

// ---------------------------------------------------------------------------
// The layout table, exercised through every loader
// ---------------------------------------------------------------------------

TEST_CASE("FolderContentPack: every asset type resolves at its documented path (#1145)", "[content][pack]") {
    // The point of going through all sixteen: docs/modding/formats.md tells authors where to put
    // each kind of file, and this is the only test that checks the engine agrees for all of them.
    MockFilesystem fs;
    MockLogger log;
    for (uint8_t t = 0; t < static_cast<uint8_t>(AssetType::Count); ++t)
        fs.addFile(pathFor(static_cast<AssetType>(t), "thing"), "payload");

    FolderContentPack pack(fs, log, kRoot, manifestOf());

    CHECK(pack.loadMesh("thing").has_value());
    CHECK(pack.loadTexture("thing").has_value());
    CHECK(pack.loadAudio("thing").has_value());
    CHECK(pack.loadFlightModel("thing").has_value());
    CHECK(pack.loadMission("thing").has_value());
    CHECK(pack.loadTerrain("thing").has_value());
    CHECK(pack.loadAIScript("thing").has_value());
    CHECK(pack.loadEntityDef("thing").has_value());
    CHECK(pack.loadSensorDef("thing").has_value());
    CHECK(pack.loadWeaponDef("thing").has_value());
    CHECK(pack.loadManualProse("thing").has_value());
    CHECK(pack.loadLivery("thing").has_value());
    CHECK(pack.loadAirportDef("thing").has_value());
    CHECK(pack.loadGameMode("thing").has_value());
    CHECK(pack.loadTheater("thing").has_value());
    CHECK(pack.loadZonePolicy("thing").has_value());
}

TEST_CASE("FolderContentPack: a missing asset is nullopt for every type (#1145)", "[content][pack]") {
    MockFilesystem fs; // empty
    MockLogger log;
    FolderContentPack pack(fs, log, kRoot, manifestOf());

    CHECK_FALSE(pack.loadMesh("absent").has_value());
    CHECK_FALSE(pack.loadTexture("absent").has_value());
    CHECK_FALSE(pack.loadAudio("absent").has_value());
    CHECK_FALSE(pack.loadFlightModel("absent").has_value());
    CHECK_FALSE(pack.loadMission("absent").has_value());
    CHECK_FALSE(pack.loadTerrain("absent").has_value());
    CHECK_FALSE(pack.loadAIScript("absent").has_value());
    CHECK_FALSE(pack.loadEntityDef("absent").has_value());
    CHECK_FALSE(pack.loadSensorDef("absent").has_value());
    CHECK_FALSE(pack.loadWeaponDef("absent").has_value());
    CHECK_FALSE(pack.loadManualProse("absent").has_value());
    CHECK_FALSE(pack.loadLivery("absent").has_value());
    CHECK_FALSE(pack.loadAirportDef("absent").has_value());
    CHECK_FALSE(pack.loadGameMode("absent").has_value());
    CHECK_FALSE(pack.loadTheater("absent").has_value());
    CHECK_FALSE(pack.loadZonePolicy("absent").has_value());
    CHECK_FALSE(pack.hasAsset("absent", AssetType::Mesh));
}

TEST_CASE("FolderContentPack: the fallback extension is tried second, never first (#1145)", "[content][pack]") {
    // Meshes are .glb with a .gltf fallback; textures .ktx2 with .png. An author shipping both must
    // get the primary, or the compressed asset they built is silently ignored.
    MockFilesystem fs;
    MockLogger log;
    fs.addFile(pathFor(AssetType::Mesh, "jet", /*fallback=*/true), "gltf-bytes");
    FolderContentPack pack(fs, log, kRoot, manifestOf());

    REQUIRE(pack.hasAsset("jet", AssetType::Mesh));
    const auto viaFallback = pack.loadMesh("jet");
    REQUIRE(viaFallback.has_value());
    CHECK(viaFallback->bytes.size() == 10);

    fs.addFile(pathFor(AssetType::Mesh, "jet"), "glb");
    const auto viaPrimary = pack.loadMesh("jet");
    REQUIRE(viaPrimary.has_value());
    CHECK(viaPrimary->bytes.size() == 3); // the .glb won
}

TEST_CASE("FolderContentPack: a type with no fallback does not invent one (#1145)", "[content][pack]") {
    MockFilesystem fs;
    MockLogger log;
    fs.addFile(std::string(kRoot) + "/audio/beep.wav", "riff"); // audio is .ogg only
    FolderContentPack pack(fs, log, kRoot, manifestOf());
    CHECK_FALSE(pack.hasAsset("beep", AssetType::Audio));
}

TEST_CASE("FolderContentPack: an asset that exists but will not open is an error, not silence (#1145)",
          "[content][pack]") {
    UnreadableFilesystem fs;
    MockLogger log;
    const std::string path = pathFor(AssetType::EntityDef, "jet");
    fs.addFile(path, "[entity]\n");
    fs.unopenable.insert(path);

    FolderContentPack pack(fs, log, kRoot, manifestOf());
    CHECK(pack.hasAsset("jet", AssetType::EntityDef));  // it is listed...
    CHECK_FALSE(pack.loadEntityDef("jet").has_value()); // ...but unreadable
    CHECK(log.hasMessage(LogLevel::Error, "failed to open"));
}

TEST_CASE("FolderContentPack: a zero-byte asset loads as empty rather than failing (#1145)", "[content][pack]") {
    MockFilesystem fs;
    MockLogger log;
    fs.addFile(pathFor(AssetType::AIScript, "noop"), "");
    FolderContentPack pack(fs, log, kRoot, manifestOf());

    const auto script = pack.loadAIScript("noop");
    REQUIRE(script.has_value());
    CHECK(script->bytes.empty());
    CHECK(script->name == "noop");
}

// ---------------------------------------------------------------------------
// loadPackFile: the sanitizer
// ---------------------------------------------------------------------------

TEST_CASE("FolderContentPack: loadPackFile reads a pack-relative file (#1145)", "[content][pack]") {
    MockFilesystem fs;
    MockLogger log;
    fs.addFile(std::string(kRoot) + "/campaigns/desert/brief.md", "hello");
    FolderContentPack pack(fs, log, kRoot, manifestOf());

    const auto bytes = pack.loadPackFile("campaigns/desert/brief.md");
    REQUIRE(bytes.has_value());
    CHECK(std::string(bytes->begin(), bytes->end()) == "hello");
}

TEST_CASE("FolderContentPack: loadPackFile refuses to escape the pack (#1145)", "[content][pack][security]") {
    // The path comes from a manifest, i.e. from downloaded content. Every one of these would read a
    // file the pack was never granted, so each must be refused BEFORE it reaches the filesystem.
    MockFilesystem fs;
    MockLogger log;
    fs.addFile("/etc/passwd", "root:x:0:0");
    fs.addFile(std::string(kRoot) + "/../secret.txt", "secret");
    fs.addFile(std::string(kRoot) + "/ok.txt", "fine");
    FolderContentPack pack(fs, log, kRoot, manifestOf());

    CHECK_FALSE(pack.loadPackFile("/etc/passwd").has_value());   // absolute
    CHECK_FALSE(pack.loadPackFile("../secret.txt").has_value()); // parent, leading
    CHECK_FALSE(pack.loadPackFile("data/../../x").has_value());  // parent, embedded
    CHECK_FALSE(pack.loadPackFile("data/sub/..").has_value());   // parent, trailing
    CHECK_FALSE(pack.loadPackFile("data\\win.txt").has_value()); // backslash separator
    CHECK_FALSE(pack.loadPackFile("C:/Windows/x").has_value());  // drive letter
    CHECK_FALSE(pack.loadPackFile("").has_value());              // empty
    CHECK_FALSE(pack.loadPackFile(nullptr).has_value());         // null
    CHECK(pack.loadPackFile("ok.txt").has_value());              // the control: this one is fine
}

TEST_CASE("FolderContentPack: a dotted name that is not a parent link is allowed (#1145)", "[content][pack]") {
    // "..hidden" and "a..b" contain two dots but are ordinary names. Rejecting them would break
    // legitimate packs, so the check is per path SEGMENT.
    MockFilesystem fs;
    MockLogger log;
    fs.addFile(std::string(kRoot) + "/..hidden/x.txt", "ok");
    fs.addFile(std::string(kRoot) + "/a..b.txt", "ok");
    FolderContentPack pack(fs, log, kRoot, manifestOf());

    CHECK(pack.loadPackFile("..hidden/x.txt").has_value());
    CHECK(pack.loadPackFile("a..b.txt").has_value());
}

TEST_CASE("FolderContentPack: loadPackFile on a missing or unreadable file is nullopt (#1145)", "[content][pack]") {
    UnreadableFilesystem fs;
    MockLogger log;
    const std::string path = std::string(kRoot) + "/locked.bin";
    fs.addFile(path, "x");
    fs.unopenable.insert(path);
    FolderContentPack pack(fs, log, kRoot, manifestOf());

    CHECK_FALSE(pack.loadPackFile("nope.bin").has_value());
    CHECK_FALSE(pack.loadPackFile("locked.bin").has_value());
}

TEST_CASE("FolderContentPack: an empty pack file is an empty buffer, not a failure (#1145)", "[content][pack]") {
    MockFilesystem fs;
    MockLogger log;
    fs.addFile(std::string(kRoot) + "/empty.bin", "");
    FolderContentPack pack(fs, log, kRoot, manifestOf());

    const auto bytes = pack.loadPackFile("empty.bin");
    REQUIRE(bytes.has_value());
    CHECK(bytes->empty());
}

// ---------------------------------------------------------------------------
// Config and terrain tiles
// ---------------------------------------------------------------------------

TEST_CASE("FolderContentPack: loadConfig reads from data/ (#1145)", "[content][pack]") {
    UnreadableFilesystem fs;
    MockLogger log;
    fs.addFile(std::string(kRoot) + "/data/keybinds.toml", "[bindings]\n");
    const std::string locked = std::string(kRoot) + "/data/locked.toml";
    fs.addFile(locked, "x");
    fs.unopenable.insert(locked);
    FolderContentPack pack(fs, log, kRoot, manifestOf());

    const auto cfg = pack.loadConfig("keybinds.toml");
    REQUIRE(cfg.has_value());
    CHECK(*cfg == "[bindings]\n");
    CHECK_FALSE(pack.loadConfig("absent.toml").has_value());
    CHECK_FALSE(pack.loadConfig("locked.toml").has_value());
}

TEST_CASE("FolderContentPack: each tile layer has its own suffix (#1145)", "[content][pack][terrain]") {
    // Height, land cover and satellite tiles share a directory and differ only by suffix; a
    // collision here would serve a land-cover map as elevation.
    MockFilesystem fs;
    MockLogger log;
    const std::string dir = std::string(kRoot) + "/terrain/earth/f2/l5/";
    fs.addFile(dir + "tile_3_7.png", "h");
    fs.addFile(dir + "tile_3_7_lc.png", "lc");
    fs.addFile(dir + "tile_3_7_sat.ktx2", "sat");
    FolderContentPack pack(fs, log, kRoot, manifestOf());

    const auto height = pack.resolveTilePath("earth", 2, 5, 3, 7, TileLayer::Height);
    const auto cover = pack.resolveTilePath("earth", 2, 5, 3, 7, TileLayer::LandCover);
    const auto sat = pack.resolveTilePath("earth", 2, 5, 3, 7, TileLayer::Satellite);
    REQUIRE(height.has_value());
    REQUIRE(cover.has_value());
    REQUIRE(sat.has_value());
    CHECK(*height == dir + "tile_3_7.png");
    CHECK(*cover == dir + "tile_3_7_lc.png");
    CHECK(*sat == dir + "tile_3_7_sat.ktx2");

    // A tile that was never generated is absent, not a path to a file that is not there.
    CHECK_FALSE(pack.resolveTilePath("earth", 2, 5, 99, 99, TileLayer::Height).has_value());
    CHECK_FALSE(pack.resolveTilePath("mars", 2, 5, 3, 7, TileLayer::Height).has_value());
}

// ---------------------------------------------------------------------------
// listAssets
// ---------------------------------------------------------------------------

TEST_CASE("FolderContentPack: listAssets strips the extension and skips directories (#1145)", "[content][pack]") {
    MockFilesystem fs;
    MockLogger log;
    const std::string dir = std::string(kRoot) + "/entities";
    fs.addDirEntry(dir, "jet.toml", false);
    fs.addDirEntry(dir, "tank.toml", false);
    fs.addDirEntry(dir, "subfolder", true);     // a directory is not an asset
    fs.addDirEntry(dir, "notes.txt", false);    // wrong extension for this subdir
    fs.addDirEntry(dir, ".toml", false);        // extension only: stripping leaves nothing to name
    fs.addDirEntry(dir, "jet.toml.bak", false); // an editor dropping

    FolderContentPack pack(fs, log, kRoot, manifestOf());
    const auto names = pack.listAssets(AssetType::EntityDef);
    CHECK(names.size() == 2);
    CHECK(std::find(names.begin(), names.end(), "jet") != names.end());
    CHECK(std::find(names.begin(), names.end(), "tank") != names.end());
}

TEST_CASE("FolderContentPack: listAssets accepts the fallback extension too (#1145)", "[content][pack]") {
    MockFilesystem fs;
    MockLogger log;
    const std::string dir = std::string(kRoot) + "/aircraft";
    fs.addDirEntry(dir, "f5e.glb", false);
    fs.addDirEntry(dir, "mig.gltf", false); // the fallback spelling still names an asset
    fs.addDirEntry(dir, "f5e.toml", false); // the flight model, a different type in the same dir

    FolderContentPack pack(fs, log, kRoot, manifestOf());
    const auto meshes = pack.listAssets(AssetType::Mesh);
    CHECK(meshes.size() == 2);

    const auto models = pack.listAssets(AssetType::FlightModel);
    REQUIRE(models.size() == 1);
    CHECK(models[0] == "f5e");
}

TEST_CASE("FolderContentPack: listing a subdir the pack does not have is empty (#1145)", "[content][pack]") {
    MockFilesystem fs;
    MockLogger log;
    FolderContentPack pack(fs, log, kRoot, manifestOf());
    CHECK(pack.listAssets(AssetType::Livery).empty());
}
