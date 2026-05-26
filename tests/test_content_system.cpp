// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "IFilesystem.h"
#include "IFilesystemWatcher.h"
#include "ILogger.h"
#include "content/AssetManager.h"
#include "content/AssetTypes.h"
#include "content/FolderContentPack.h"
#include "content/IContentPack.h"
#include "content/ModLoader.h"

#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Mock types (inline — no separate header until a second test file needs them)
// ---------------------------------------------------------------------------

struct MockLogger : public ILogger {
    struct Entry {
        LogLevel level;
        std::string message;
    };
    std::vector<Entry> entries;

    void log(LogLevel level, const char*, int, const char* message) override {
        entries.push_back({level, message});
    }
    void setMinLevel(LogLevel) override {}
    void flush() override {}

    bool hasMessage(LogLevel level, const std::string& substr) const {
        for (auto& e : entries)
            if (e.level == level && e.message.find(substr) != std::string::npos)
                return true;
        return false;
    }
};

// In-memory filesystem: directories stored as sets of Entry, files as byte vectors.
struct MockFilesystem : public IFilesystem {
    // path → file bytes  (PathDomain ignored for simplicity in tests)
    std::map<std::string, std::vector<uint8_t>> files;
    // paths that are directories
    std::map<std::string, std::vector<Entry>> dirs;

    void addFile(const std::string& path, const std::string& content) {
        files[path] = std::vector<uint8_t>(content.begin(), content.end());
    }
    void addDir(const std::string& path) {
        if (dirs.find(path) == dirs.end())
            dirs[path] = {};
    }
    void addDirEntry(const std::string& parentDir, const std::string& name, bool isDirectory) {
        dirs[parentDir].push_back({name, isDirectory});
    }

    int openFile(PathDomain, const char* path, bool) override {
        auto it = files.find(path);
        if (it == files.end())
            return -1;
        openHandles[nextHandle] = path;
        return nextHandle++;
    }
    void closeFile(int handle) override {
        openHandles.erase(handle);
    }

    std::size_t readFile(int handle, void* buffer, std::size_t size) override {
        auto hit = openHandles.find(handle);
        if (hit == openHandles.end())
            return 0;
        auto& data = files[hit->second];
        std::size_t n = std::min(size, data.size());
        std::memcpy(buffer, data.data(), n);
        return n;
    }
    std::size_t writeFile(int, const void*, std::size_t) override {
        return 0;
    }
    bool seek(int, std::size_t, SeekOrigin) override {
        return false;
    }
    std::size_t getFileSize(int handle) const override {
        auto hit = openHandles.find(handle);
        if (hit == openHandles.end())
            return 0;
        auto fit = files.find(hit->second);
        return (fit != files.end()) ? fit->second.size() : 0;
    }
    bool fileExists(PathDomain, const char* path) const override {
        return files.find(path) != files.end();
    }
    bool createDirectory(PathDomain, const char*) override {
        return true;
    }
    bool renameFile(PathDomain, const char*, const char*) override {
        return false;
    }
    std::vector<Entry> scanDirectory(PathDomain, const char* path) const override {
        auto it = dirs.find(path);
        if (it == dirs.end())
            return {};
        return it->second;
    }

  private:
    int nextHandle = 1;
    std::map<int, std::string> openHandles;
};

struct MockFilesystemWatcher : public IFilesystemWatcher {
    struct WatchCall {
        std::string path;
        bool recursive;
    };
    std::vector<WatchCall> watchCalls;
    std::vector<std::string> unwatchCalls;
    std::vector<Event> pendingEvents;

    bool watch(PathDomain, const char* path, bool recursive) override {
        watchCalls.push_back({path, recursive});
        return true;
    }
    void unwatch(PathDomain, const char* path) override {
        unwatchCalls.push_back(path);
    }
    std::vector<Event> pollEvents() override {
        return std::exchange(pendingEvents, {});
    }
};

struct MockContentPack : public IContentPack {
    std::string packName = "mock";
    std::string packId = "mock-id";
    std::string packVersion = "1.0.0";
    int packPriority = 0;
    const char* packRootDir = nullptr;
    IContentPack::Status initStatus = IContentPack::Status::Ready;
    bool configureResult = true;

    // Loaded asset name → bytes to return
    std::map<std::pair<std::string, AssetType>, std::vector<uint8_t>> assets;

    const char* name() const override {
        return packName.c_str();
    }
    const char* version() const override {
        return packVersion.c_str();
    }
    const char* id() const override {
        return packId.c_str();
    }
    int priority() const override {
        return packPriority;
    }
    const char* rootDirectory() const override {
        return packRootDir;
    }

    IContentPack::Status init() override {
        return initStatus;
    }
    bool configure(IWindow*) override {
        return configureResult;
    }

    bool hasAsset(const char* n, AssetType t) const override {
        return assets.find({n, t}) != assets.end();
    }

    template <typename T> std::optional<T> loadByType(const char* n, AssetType t) {
        auto it = assets.find({n, t});
        if (it == assets.end())
            return std::nullopt;
        T result;
        result.name = n;
        result.bytes = it->second;
        return result;
    }

    std::optional<MeshData> loadMesh(const char* n) override {
        return loadByType<MeshData>(n, AssetType::Mesh);
    }
    std::optional<TextureData> loadTexture(const char* n) override {
        return loadByType<TextureData>(n, AssetType::Texture);
    }
    std::optional<AudioBuffer> loadAudio(const char* n) override {
        return loadByType<AudioBuffer>(n, AssetType::Audio);
    }
    std::optional<FlightModel> loadFlightModel(const char* n) override {
        return loadByType<FlightModel>(n, AssetType::FlightModel);
    }
    std::optional<MissionData> loadMission(const char* n) override {
        return loadByType<MissionData>(n, AssetType::Mission);
    }
    std::optional<TerrainData> loadTerrain(const char* n) override {
        return loadByType<TerrainData>(n, AssetType::Terrain);
    }
    std::optional<AIScript> loadAIScript(const char* n) override {
        return loadByType<AIScript>(n, AssetType::AIScript);
    }
    std::vector<std::string> listAssets(AssetType) const override {
        return {};
    }
};

// Helper: build a valid manifest TOML string
static std::string makeManifest(const std::string& name = "Test Mod", const std::string& id = "test-mod", int prio = 10,
                                const std::string& api = "1.0") {
    return "[mod]\nname = \"" + name + "\"\nid = \"" + id + "\"\nversion = \"1.0.0\"\n\"engine-api\" = \"" + api +
           "\"\npriority = " + std::to_string(prio) + "\ndepends = []\n";
}

// ---------------------------------------------------------------------------
// ModLoader tests
// ---------------------------------------------------------------------------

TEST_CASE("ModLoader returns empty stack when mods directory is absent") {
    MockFilesystem fs;
    MockLogger logger;
    ModLoader loader(fs, logger);

    auto packs = loader.load();

    REQUIRE(packs.empty());
    REQUIRE(logger.hasMessage(LogLevel::Info, "absent or empty"));
}

TEST_CASE("ModLoader skips subdirectory without manifest.toml") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "empty-mod", true);
    fs.addDir("mods/empty-mod");

    ModLoader loader(fs, logger);
    auto packs = loader.load();

    REQUIRE(packs.empty());
    REQUIRE(logger.hasMessage(LogLevel::Debug, "no manifest.toml"));
}

TEST_CASE("ModLoader parses valid manifest and constructs one FolderContentPack") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "test-mod", true);
    fs.addDir("mods/test-mod");
    fs.addFile("mods/test-mod/manifest.toml", makeManifest("Test Mod", "test-mod", 10));

    ModLoader loader(fs, logger);
    auto packs = loader.load();

    REQUIRE(packs.size() == 1);
    CHECK(std::string(packs[0]->name()) == "Test Mod");
    CHECK(std::string(packs[0]->id()) == "test-mod");
    CHECK(packs[0]->priority() == 10);
}

TEST_CASE("ModLoader skips pack with mismatched engine-api major version") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "old-mod", true);
    fs.addDir("mods/old-mod");
    fs.addFile("mods/old-mod/manifest.toml", makeManifest("Old Mod", "old-mod", 5, "2.0"));

    ModLoader loader(fs, logger);
    auto packs = loader.load();

    REQUIRE(packs.empty());
    REQUIRE(logger.hasMessage(LogLevel::Error, "incompatible"));
}

TEST_CASE("ModLoader sorts packs by priority descending") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "low-mod", true);
    fs.addDirEntry("mods", "high-mod", true);
    fs.addDir("mods/low-mod");
    fs.addDir("mods/high-mod");
    fs.addFile("mods/low-mod/manifest.toml", makeManifest("Low", "low-mod", 5));
    fs.addFile("mods/high-mod/manifest.toml", makeManifest("High", "high-mod", 100));

    ModLoader loader(fs, logger);
    auto packs = loader.load();

    REQUIRE(packs.size() == 2);
    CHECK(packs[0]->priority() == 100);
    CHECK(packs[1]->priority() == 5);
}

TEST_CASE("ModLoader logs warning for declared dependency that is not present") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "dependent-mod", true);
    fs.addDir("mods/dependent-mod");
    fs.addFile("mods/dependent-mod/manifest.toml",
               "[mod]\nname = \"Dep Mod\"\nid = \"dep-mod\"\nversion = \"1.0.0\"\n"
               "\"engine-api\" = \"1.0\"\npriority = 1\ndepends = [\"missing-base\"]\n");

    ModLoader loader(fs, logger);
    auto packs = loader.load();

    REQUIRE(packs.size() == 1);
    REQUIRE(logger.hasMessage(LogLevel::Warn, "missing-base"));
}

// ---------------------------------------------------------------------------
// AssetManager tests
// ---------------------------------------------------------------------------

static std::vector<std::unique_ptr<IContentPack>> makePacks(MockContentPack* pack) {
    // Wrap in unique_ptr without taking ownership (test manages lifetime)
    struct BorrowedPack : public IContentPack {
        MockContentPack* p;
        explicit BorrowedPack(MockContentPack* p) : p(p) {}
        const char* name() const override {
            return p->name();
        }
        const char* version() const override {
            return p->version();
        }
        const char* id() const override {
            return p->id();
        }
        int priority() const override {
            return p->priority();
        }
        const char* rootDirectory() const override {
            return p->rootDirectory();
        }
        IContentPack::Status init() override {
            return p->init();
        }
        bool configure(IWindow* w) override {
            return p->configure(w);
        }
        bool hasAsset(const char* n, AssetType t) const override {
            return p->hasAsset(n, t);
        }
        std::optional<MeshData> loadMesh(const char* n) override {
            return p->loadMesh(n);
        }
        std::optional<TextureData> loadTexture(const char* n) override {
            return p->loadTexture(n);
        }
        std::optional<AudioBuffer> loadAudio(const char* n) override {
            return p->loadAudio(n);
        }
        std::optional<FlightModel> loadFlightModel(const char* n) override {
            return p->loadFlightModel(n);
        }
        std::optional<MissionData> loadMission(const char* n) override {
            return p->loadMission(n);
        }
        std::optional<TerrainData> loadTerrain(const char* n) override {
            return p->loadTerrain(n);
        }
        std::optional<AIScript> loadAIScript(const char* n) override {
            return p->loadAIScript(n);
        }
        std::vector<std::string> listAssets(AssetType t) const override {
            return p->listAssets(t);
        }
    };
    std::vector<std::unique_ptr<IContentPack>> v;
    v.push_back(std::make_unique<BorrowedPack>(pack));
    return v;
}

TEST_CASE("AssetManager::initialize keeps pack when init() returns Ready") {
    MockContentPack pack;
    MockLogger logger;
    pack.initStatus = IContentPack::Status::Ready;

    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);

    // Pack is active — should find assets
    pack.assets[{"f22", AssetType::Mesh}] = {0x01};
    auto result = am.loadMesh("f22");
    REQUIRE(result != nullptr);
}

TEST_CASE("AssetManager::initialize calls configure() only when NeedsConfiguration") {
    MockContentPack pack;
    MockLogger logger;
    pack.initStatus = IContentPack::Status::NeedsConfiguration;
    pack.configureResult = true;

    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr); // no window → should drop

    pack.assets[{"f22", AssetType::Mesh}] = {0x01};
    auto result = am.loadMesh("f22");
    REQUIRE(result == nullptr);
    REQUIRE(logger.hasMessage(LogLevel::Warn, "NeedsConfiguration but no window"));
}

TEST_CASE("AssetManager::initialize drops pack when configure() returns false") {
    MockContentPack pack;
    MockLogger logger;
    // Simulate a window-like non-null pointer for the test
    IWindow* fakeWindow = reinterpret_cast<IWindow*>(0x1);
    pack.initStatus = IContentPack::Status::NeedsConfiguration;
    pack.configureResult = false;

    AssetManager am(makePacks(&pack), logger);
    am.initialize(fakeWindow);

    REQUIRE(logger.hasMessage(LogLevel::Warn, "configure() returned false"));
    pack.assets[{"f22", AssetType::Mesh}] = {0x01};
    REQUIRE(am.loadMesh("f22") == nullptr);
}

TEST_CASE("AssetManager returns nullptr when no pack has the asset") {
    MockContentPack pack;
    MockLogger logger;

    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);

    auto result = am.loadMesh("nonexistent");
    REQUIRE(result == nullptr);
    REQUIRE(logger.hasMessage(LogLevel::Warn, "asset not found"));
}

TEST_CASE("AssetManager returns asset bytes from highest-priority pack") {
    MockContentPack packA, packB;
    MockLogger logger;
    packA.packId = "pack-a";
    packA.packPriority = 100;
    packB.packId = "pack-b";
    packB.packPriority = 10;

    packA.assets[{"f22", AssetType::Mesh}] = {0xAA};
    packB.assets[{"f22", AssetType::Mesh}] = {0xBB};

    std::vector<std::unique_ptr<IContentPack>> packs;
    // Insert in priority order (highest first, as ModLoader would do)
    packs.push_back(std::make_unique<MockContentPack>(packA));
    packs.push_back(std::make_unique<MockContentPack>(packB));

    AssetManager am(std::move(packs), logger);
    am.initialize(nullptr);

    auto result = am.loadMesh("f22");
    REQUIRE(result != nullptr);
    REQUIRE(result->bytes == std::vector<uint8_t>{0xAA});
}

TEST_CASE("AssetManager returns same shared_ptr on second request (cache hit)") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"f22", AssetType::Mesh}] = {0x01, 0x02};

    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);

    auto first = am.loadMesh("f22");
    auto second = am.loadMesh("f22");
    REQUIRE(first != nullptr);
    REQUIRE(first.get() == second.get());
}

TEST_CASE("AssetManager lookup is case-insensitive") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"f22", AssetType::Mesh}] = {0x42};

    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);

    auto lower = am.loadMesh("f22");
    auto upper = am.loadMesh("F22");
    REQUIRE(lower != nullptr);
    REQUIRE(upper != nullptr);
    REQUIRE(lower.get() == upper.get());
}

TEST_CASE("AssetManager passes normalized lowercase name to IContentPack methods") {
    struct CapturingPack : public MockContentPack {
        std::string lastQueried;
        std::optional<MeshData> loadMesh(const char* n) override {
            lastQueried = n;
            return MockContentPack::loadMesh(n);
        }
    } pack;
    MockLogger logger;
    pack.assets[{"f22", AssetType::Mesh}] = {0x01};

    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);

    am.loadMesh("F22");
    CHECK(pack.lastQueried == "f22");
}

TEST_CASE("AssetManager::enableHotReload calls watch() for each pack with a rootDirectory") {
    MockContentPack pack;
    MockLogger logger;
    MockFilesystemWatcher watcher;
    const char* rootDir = "mods/test-mod";
    pack.packRootDir = rootDir;

    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    am.enableHotReload(watcher);

    REQUIRE(watcher.watchCalls.size() == 1);
    CHECK(watcher.watchCalls[0].path == rootDir);
    CHECK(watcher.watchCalls[0].recursive == true);
}

TEST_CASE("AssetManager::processHotReload clears cache when watcher reports a Modified event") {
    MockContentPack pack;
    MockLogger logger;
    MockFilesystemWatcher watcher;
    pack.assets[{"f22", AssetType::Mesh}] = {0x01};

    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    am.enableHotReload(watcher);

    // Populate cache
    auto before = am.loadMesh("f22");
    REQUIRE(before != nullptr);

    // Trigger hot-reload event
    watcher.pendingEvents.push_back({"mods/test-mod/aircraft/f22.glb", IFilesystemWatcher::EventType::Modified});
    am.processHotReload();

    // New load must re-query the pack (different shared_ptr instance)
    auto after = am.loadMesh("f22");
    REQUIRE(after != nullptr);
    REQUIRE(before.get() != after.get());
}

// ---------------------------------------------------------------------------
// AssetManager — all asset type load paths
// ---------------------------------------------------------------------------

TEST_CASE("AssetManager::loadTexture returns data from pack") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"sky", AssetType::Texture}] = {0x10, 0x20};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    auto r = am.loadTexture("sky");
    REQUIRE(r != nullptr);
    CHECK(r->bytes == std::vector<uint8_t>{0x10, 0x20});
}

TEST_CASE("AssetManager::loadTexture returns nullptr when missing") {
    MockContentPack pack;
    MockLogger logger;
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    CHECK(am.loadTexture("sky") == nullptr);
}

TEST_CASE("AssetManager::loadAudio returns data from pack") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"gun", AssetType::Audio}] = {0x30};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    REQUIRE(am.loadAudio("gun") != nullptr);
}

TEST_CASE("AssetManager::loadFlightModel returns data from pack") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"f22", AssetType::FlightModel}] = {0x40};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    REQUIRE(am.loadFlightModel("f22") != nullptr);
}

TEST_CASE("AssetManager::loadMission returns data from pack") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"m1", AssetType::Mission}] = {0x50};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    REQUIRE(am.loadMission("m1") != nullptr);
}

TEST_CASE("AssetManager::loadTerrain returns data from pack") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"iraq", AssetType::Terrain}] = {0x60};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    REQUIRE(am.loadTerrain("iraq") != nullptr);
}

TEST_CASE("AssetManager::loadAIScript returns data from pack") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"mig29_ai", AssetType::AIScript}] = {0x70};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    REQUIRE(am.loadAIScript("mig29_ai") != nullptr);
}

TEST_CASE("AssetManager::enableHotReload skips pack with null rootDirectory") {
    MockContentPack pack;
    MockLogger logger;
    MockFilesystemWatcher watcher;
    pack.packRootDir = nullptr;

    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    am.enableHotReload(watcher);

    CHECK(watcher.watchCalls.empty());
}

TEST_CASE("AssetManager::processHotReload with no watcher is a no-op") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"f22", AssetType::Mesh}] = {0x01};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    am.loadMesh("f22");
    REQUIRE_NOTHROW(am.processHotReload());
}

TEST_CASE("AssetManager::processHotReload with empty events does not clear cache") {
    MockContentPack pack;
    MockLogger logger;
    MockFilesystemWatcher watcher;
    pack.assets[{"f22", AssetType::Mesh}] = {0x01};

    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    am.enableHotReload(watcher);

    auto first = am.loadMesh("f22");
    am.processHotReload(); // no events
    auto second = am.loadMesh("f22");
    CHECK(first.get() == second.get());
}

TEST_CASE("AssetManager::initialize with configure() succeeds when window provided") {
    MockContentPack pack;
    MockLogger logger;
    IWindow* fakeWindow = reinterpret_cast<IWindow*>(0x1);
    pack.initStatus = IContentPack::Status::NeedsConfiguration;
    pack.configureResult = true;

    AssetManager am(makePacks(&pack), logger);
    am.initialize(fakeWindow);

    pack.assets[{"f22", AssetType::Mesh}] = {0x01};
    REQUIRE(am.loadMesh("f22") != nullptr);
}

// ---------------------------------------------------------------------------
// ModLoader — additional branch coverage
// ---------------------------------------------------------------------------

TEST_CASE("ModLoader skips file entries (non-directories) in mods directory") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "readme.txt", false); // file, not a directory
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    CHECK(packs.empty());
}

TEST_CASE("ModLoader parseManifest: invalid TOML logs error and returns empty") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "bad-mod", true);
    fs.addDir("mods/bad-mod");
    fs.addFile("mods/bad-mod/manifest.toml", "this is not valid toml }{{{");
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    CHECK(packs.empty());
    CHECK(logger.hasMessage(LogLevel::Error, "failed to parse manifest"));
}

TEST_CASE("ModLoader parseManifest: missing [mod] table logs error") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "no-mod-table", true);
    fs.addDir("mods/no-mod-table");
    fs.addFile("mods/no-mod-table/manifest.toml", "[other]\nkey = \"value\"\n");
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    CHECK(packs.empty());
    CHECK(logger.hasMessage(LogLevel::Error, "missing [mod] table"));
}

TEST_CASE("ModLoader parseManifest: missing required fields logs error") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "incomplete-mod", true);
    fs.addDir("mods/incomplete-mod");
    // Missing priority field
    fs.addFile("mods/incomplete-mod/manifest.toml",
               "[mod]\nname = \"Test\"\nid = \"test\"\nversion = \"1.0\"\n\"engine-api\" = \"1.0\"\n");
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    CHECK(packs.empty());
    CHECK(logger.hasMessage(LogLevel::Error, "missing required field"));
}

TEST_CASE("ModLoader validateEngineApi: engine-api version without dot is compared as-is") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "nodot-mod", true);
    fs.addDir("mods/nodot-mod");
    // engine-api without a dot — "1" matches kEngineApiMajor "1"
    fs.addFile("mods/nodot-mod/manifest.toml", "[mod]\nname = \"NoDot\"\nid = \"nodot\"\nversion = \"1.0\"\n"
                                               "\"engine-api\" = \"1\"\npriority = 1\ndepends = []\n");
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    REQUIRE(packs.size() == 1);
    CHECK(std::string(packs[0]->id()) == "nodot");
}

TEST_CASE("ModLoader two mods with met dependency - no warning") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "base-mod", true);
    fs.addDirEntry("mods", "dep-mod", true);
    fs.addDir("mods/base-mod");
    fs.addDir("mods/dep-mod");
    fs.addFile("mods/base-mod/manifest.toml", makeManifest("Base", "base-mod", 10));
    fs.addFile("mods/dep-mod/manifest.toml", "[mod]\nname = \"Dep\"\nid = \"dep-mod\"\nversion = \"1.0.0\"\n"
                                             "\"engine-api\" = \"1.0\"\npriority = 5\ndepends = [\"base-mod\"]\n");
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    REQUIRE(packs.size() == 2);
    // No warning about missing dependency
    CHECK(!logger.hasMessage(LogLevel::Warn, "not found"));
}

// ---------------------------------------------------------------------------
// FolderContentPack tests
// ---------------------------------------------------------------------------

// MockFilesystem subclass that fails to open a specific path (for testing
// the loadBytes openFile-failure branch).
struct OpenFailFilesystem : MockFilesystem {
    std::string failPath;
    int openFile(PathDomain d, const char* path, bool w) override {
        if (failPath == path)
            return -1;
        return MockFilesystem::openFile(d, path, w);
    }
};

static FolderContentPack::Manifest makePackManifest(const char* name = "Test", const char* id = "test",
                                                    const char* ver = "1.0", int prio = 1) {
    FolderContentPack::Manifest m;
    m.name = name;
    m.id = id;
    m.version = ver;
    m.priority = prio;
    return m;
}

TEST_CASE("FolderContentPack::init returns Ready") {
    MockFilesystem fs;
    MockLogger logger;
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(pack.init() == IContentPack::Status::Ready);
}

TEST_CASE("FolderContentPack::configure returns true") {
    MockFilesystem fs;
    MockLogger logger;
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(pack.configure(nullptr));
}

TEST_CASE("FolderContentPack accessors reflect manifest values") {
    MockFilesystem fs;
    MockLogger logger;
    auto m = makePackManifest("My Mod", "my-mod", "2.5", 42);
    FolderContentPack pack(fs, logger, "mods/my-mod", m);
    CHECK(std::string(pack.name()) == "My Mod");
    CHECK(std::string(pack.id()) == "my-mod");
    CHECK(std::string(pack.version()) == "2.5");
    CHECK(pack.priority() == 42);
    CHECK(std::string(pack.rootDirectory()) == "mods/my-mod");
}

TEST_CASE("FolderContentPack::hasAsset true when primary extension exists") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/aircraft/f22.glb", "mesh");
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(pack.hasAsset("f22", AssetType::Mesh));
}

TEST_CASE("FolderContentPack::hasAsset true when fallback extension exists") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/aircraft/f22.gltf", "mesh gltf");
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(pack.hasAsset("f22", AssetType::Mesh));
}

TEST_CASE("FolderContentPack::hasAsset false when neither extension exists") {
    MockFilesystem fs;
    MockLogger logger;
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.hasAsset("f22", AssetType::Mesh));
}

TEST_CASE("FolderContentPack::hasAsset with asset type that has no fallback") {
    MockFilesystem fs;
    MockLogger logger;
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.hasAsset("gun", AssetType::Audio));
    fs.addFile("mods/test/audio/gun.ogg", "audio");
    CHECK(pack.hasAsset("gun", AssetType::Audio));
}

TEST_CASE("FolderContentPack::loadMesh success with primary extension") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/aircraft/f22.glb", "mesh bytes");
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    auto r = pack.loadMesh("f22");
    REQUIRE(r.has_value());
    CHECK(r->name == "f22");
    CHECK(r->bytes.size() == 10);
}

TEST_CASE("FolderContentPack::loadMesh success with fallback extension") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/aircraft/f22.gltf", "gltf");
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    auto r = pack.loadMesh("f22");
    REQUIRE(r.has_value());
}

TEST_CASE("FolderContentPack::loadMesh returns nullopt when asset is absent") {
    MockFilesystem fs;
    MockLogger logger;
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadMesh("nonexistent").has_value());
}

TEST_CASE("FolderContentPack::loadMesh returns nullopt when openFile fails") {
    OpenFailFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/aircraft/f22.glb", "data");
    fs.failPath = "mods/test/aircraft/f22.glb";
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadMesh("f22").has_value());
    CHECK(logger.hasMessage(LogLevel::Error, "failed to open"));
}

TEST_CASE("FolderContentPack::loadTexture success with primary extension") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/textures/sky.ktx2", "ktx2 data");
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    REQUIRE(pack.loadTexture("sky").has_value());
}

TEST_CASE("FolderContentPack::loadTexture success with fallback .png") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/textures/sky.png", "png data");
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    REQUIRE(pack.loadTexture("sky").has_value());
}

TEST_CASE("FolderContentPack::loadAudio success") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/audio/engine.ogg", "ogg data");
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    REQUIRE(pack.loadAudio("engine").has_value());
}

TEST_CASE("FolderContentPack::loadFlightModel success") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/aircraft/f22.toml", "toml");
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    REQUIRE(pack.loadFlightModel("f22").has_value());
}

TEST_CASE("FolderContentPack::loadMission success") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/missions/op1.yaml", "yaml");
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    REQUIRE(pack.loadMission("op1").has_value());
}

TEST_CASE("FolderContentPack::loadTerrain success") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/terrain/iraq.json", "json");
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    REQUIRE(pack.loadTerrain("iraq").has_value());
}

TEST_CASE("FolderContentPack::loadAIScript success") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/ai/mig_ai.lua", "lua");
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    REQUIRE(pack.loadAIScript("mig_ai").has_value());
}

TEST_CASE("FolderContentPack::listAssets returns names stripped of primary extension") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods/test/aircraft");
    fs.addDirEntry("mods/test/aircraft", "f22.glb", false);
    fs.addDirEntry("mods/test/aircraft", "mig29.glb", false);
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    auto names = pack.listAssets(AssetType::Mesh);
    REQUIRE(names.size() == 2);
    CHECK(std::find(names.begin(), names.end(), "f22") != names.end());
    CHECK(std::find(names.begin(), names.end(), "mig29") != names.end());
}

TEST_CASE("FolderContentPack::listAssets skips subdirectories") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods/test/aircraft");
    fs.addDirEntry("mods/test/aircraft", "f22.glb", false);
    fs.addDirEntry("mods/test/aircraft", "subdir", true);
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    auto names = pack.listAssets(AssetType::Mesh);
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "f22");
}

TEST_CASE("FolderContentPack::listAssets includes files with fallback extension") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods/test/aircraft");
    fs.addDirEntry("mods/test/aircraft", "f22.glb", false);
    fs.addDirEntry("mods/test/aircraft", "mig29.gltf", false);
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    auto names = pack.listAssets(AssetType::Mesh);
    CHECK(names.size() == 2);
}

TEST_CASE("FolderContentPack::listAssets skips files with unrecognised extension") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods/test/aircraft");
    fs.addDirEntry("mods/test/aircraft", "f22.txt", false);
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(pack.listAssets(AssetType::Mesh).empty());
}

TEST_CASE("FolderContentPack::listAssets returns empty for absent directory") {
    MockFilesystem fs;
    MockLogger logger;
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(pack.listAssets(AssetType::Mesh).empty());
}

TEST_CASE("FolderContentPack::listAssets for audio type (no fallback extension)") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods/test/audio");
    fs.addDirEntry("mods/test/audio", "gun.ogg", false);
    fs.addDirEntry("mods/test/audio", "engine.ogg", false);
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    auto names = pack.listAssets(AssetType::Audio);
    CHECK(names.size() == 2);
}

// ---------------------------------------------------------------------------
// FolderContentPack — absent asset tests (covers path.empty() TRUE branch in
// loadBytes for each template instantiation other than Mesh)
// ---------------------------------------------------------------------------

TEST_CASE("FolderContentPack::loadTexture returns nullopt when absent") {
    MockFilesystem fs;
    MockLogger logger;
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadTexture("sky").has_value());
}

TEST_CASE("FolderContentPack::loadAudio returns nullopt when absent") {
    MockFilesystem fs;
    MockLogger logger;
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadAudio("gun").has_value());
}

TEST_CASE("FolderContentPack::loadFlightModel returns nullopt when absent") {
    MockFilesystem fs;
    MockLogger logger;
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadFlightModel("f22").has_value());
}

TEST_CASE("FolderContentPack::loadMission returns nullopt when absent") {
    MockFilesystem fs;
    MockLogger logger;
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadMission("op1").has_value());
}

TEST_CASE("FolderContentPack::loadTerrain returns nullopt when absent") {
    MockFilesystem fs;
    MockLogger logger;
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadTerrain("iraq").has_value());
}

TEST_CASE("FolderContentPack::loadAIScript returns nullopt when absent") {
    MockFilesystem fs;
    MockLogger logger;
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadAIScript("mig_ai").has_value());
}

// ---------------------------------------------------------------------------
// FolderContentPack — openFile-fail tests (covers handle<0 TRUE branch for
// each remaining template instantiation)
// ---------------------------------------------------------------------------

TEST_CASE("FolderContentPack::loadTexture returns nullopt when openFile fails") {
    OpenFailFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/textures/sky.ktx2", "data");
    fs.failPath = "mods/test/textures/sky.ktx2";
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadTexture("sky").has_value());
    CHECK(logger.hasMessage(LogLevel::Error, "failed to open"));
}

TEST_CASE("FolderContentPack::loadAudio returns nullopt when openFile fails") {
    OpenFailFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/audio/gun.ogg", "data");
    fs.failPath = "mods/test/audio/gun.ogg";
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadAudio("gun").has_value());
}

TEST_CASE("FolderContentPack::loadFlightModel returns nullopt when openFile fails") {
    OpenFailFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/aircraft/f22.toml", "data");
    fs.failPath = "mods/test/aircraft/f22.toml";
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadFlightModel("f22").has_value());
}

TEST_CASE("FolderContentPack::loadMission returns nullopt when openFile fails") {
    OpenFailFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/missions/op1.yaml", "data");
    fs.failPath = "mods/test/missions/op1.yaml";
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadMission("op1").has_value());
}

TEST_CASE("FolderContentPack::loadAIScript returns nullopt when openFile fails") {
    OpenFailFilesystem fs;
    MockLogger logger;
    fs.addFile("mods/test/ai/mig_ai.lua", "data");
    fs.failPath = "mods/test/ai/mig_ai.lua";
    FolderContentPack pack(fs, logger, "mods/test", makePackManifest());
    CHECK(!pack.loadAIScript("mig_ai").has_value());
}

// ---------------------------------------------------------------------------
// AssetManager — cache hit tests (covers m_cache.find() != end() TRUE branch
// for each template instantiation other than Mesh)
// ---------------------------------------------------------------------------

TEST_CASE("AssetManager::loadTexture cache hit returns same shared_ptr") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"sky", AssetType::Texture}] = {0x10};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    auto first = am.loadTexture("sky");
    auto second = am.loadTexture("sky");
    REQUIRE(first != nullptr);
    CHECK(first.get() == second.get());
}

TEST_CASE("AssetManager::loadAudio cache hit returns same shared_ptr") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"gun", AssetType::Audio}] = {0x20};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    auto first = am.loadAudio("gun");
    auto second = am.loadAudio("gun");
    REQUIRE(first != nullptr);
    CHECK(first.get() == second.get());
}

TEST_CASE("AssetManager::loadFlightModel cache hit returns same shared_ptr") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"f22", AssetType::FlightModel}] = {0x30};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    auto first = am.loadFlightModel("f22");
    auto second = am.loadFlightModel("f22");
    REQUIRE(first != nullptr);
    CHECK(first.get() == second.get());
}

TEST_CASE("AssetManager::loadMission cache hit returns same shared_ptr") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"m1", AssetType::Mission}] = {0x40};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    auto first = am.loadMission("m1");
    auto second = am.loadMission("m1");
    REQUIRE(first != nullptr);
    CHECK(first.get() == second.get());
}

TEST_CASE("AssetManager::loadTerrain cache hit returns same shared_ptr") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"iraq", AssetType::Terrain}] = {0x50};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    auto first = am.loadTerrain("iraq");
    auto second = am.loadTerrain("iraq");
    REQUIRE(first != nullptr);
    CHECK(first.get() == second.get());
}

TEST_CASE("AssetManager::loadAIScript cache hit returns same shared_ptr") {
    MockContentPack pack;
    MockLogger logger;
    pack.assets[{"mig_ai", AssetType::AIScript}] = {0x60};
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    auto first = am.loadAIScript("mig_ai");
    auto second = am.loadAIScript("mig_ai");
    REQUIRE(first != nullptr);
    CHECK(first.get() == second.get());
}

// ---------------------------------------------------------------------------
// AssetManager — missing asset tests (covers result.has_value() FALSE branch
// for each template instantiation other than Texture)
// ---------------------------------------------------------------------------

TEST_CASE("AssetManager::loadAudio returns nullptr when missing") {
    MockContentPack pack;
    MockLogger logger;
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    CHECK(am.loadAudio("gun") == nullptr);
}

TEST_CASE("AssetManager::loadFlightModel returns nullptr when missing") {
    MockContentPack pack;
    MockLogger logger;
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    CHECK(am.loadFlightModel("f22") == nullptr);
}

TEST_CASE("AssetManager::loadMission returns nullptr when missing") {
    MockContentPack pack;
    MockLogger logger;
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    CHECK(am.loadMission("op1") == nullptr);
}

TEST_CASE("AssetManager::loadTerrain returns nullptr when missing") {
    MockContentPack pack;
    MockLogger logger;
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    CHECK(am.loadTerrain("iraq") == nullptr);
}

TEST_CASE("AssetManager::loadAIScript returns nullptr when missing") {
    MockContentPack pack;
    MockLogger logger;
    AssetManager am(makePacks(&pack), logger);
    am.initialize(nullptr);
    CHECK(am.loadAIScript("mig_ai") == nullptr);
}

// ---------------------------------------------------------------------------
// ModLoader — additional branch coverage
// ---------------------------------------------------------------------------

TEST_CASE("ModLoader loads manifest with no depends key (optional field absent)") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "nodep-mod", true);
    fs.addDir("mods/nodep-mod");
    // No "depends" key at all — as_array() returns nullptr → if(deps) FALSE branch
    fs.addFile("mods/nodep-mod/manifest.toml", "[mod]\nname = \"NoDep\"\nid = \"nodep\"\nversion = \"1.0\"\n"
                                               "\"engine-api\" = \"1.0\"\npriority = 1\n");
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    REQUIRE(packs.size() == 1);
    CHECK(std::string(packs[0]->id()) == "nodep");
}

TEST_CASE("ModLoader skips non-string entries in depends array") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "intdep-mod", true);
    fs.addDir("mods/intdep-mod");
    // depends contains an integer — dep.value<string>() returns nullopt → if(s) FALSE branch
    fs.addFile("mods/intdep-mod/manifest.toml", "[mod]\nname = \"IntDep\"\nid = \"intdep\"\nversion = \"1.0\"\n"
                                                "\"engine-api\" = \"1.0\"\npriority = 1\ndepends = [42]\n");
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    REQUIRE(packs.size() == 1);
}

TEST_CASE("ModLoader handles parseManifest openFile failure gracefully") {
    OpenFailFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "fail-mod", true);
    fs.addDir("mods/fail-mod");
    // File exists in the map but openFile will return -1
    fs.addFile("mods/fail-mod/manifest.toml", "[mod]\nname = \"T\"\n");
    fs.failPath = "mods/fail-mod/manifest.toml";
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    CHECK(packs.empty());
}

// ---------------------------------------------------------------------------
// ModLoader — additional error-path branch coverage
// ---------------------------------------------------------------------------

TEST_CASE("ModLoader skips non-directory file entries in mods directory") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    // Add a plain FILE in mods/ — should be skipped (if(!entry.isDirectory) continue)
    fs.addDirEntry("mods", "README.txt", false);
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    CHECK(packs.empty());
}

TEST_CASE("ModLoader handles manifest with invalid TOML gracefully") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "bad-mod", true);
    fs.addDir("mods/bad-mod");
    // Invalid TOML → parseManifest catch block → returns nullopt
    fs.addFile("mods/bad-mod/manifest.toml", "this is {{{ totally invalid toml");
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    CHECK(packs.empty());
    CHECK(logger.hasMessage(LogLevel::Error, "failed to parse manifest"));
}

TEST_CASE("ModLoader handles manifest without [mod] table") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "nomod-section", true);
    fs.addDir("mods/nomod-section");
    // Valid TOML but no [mod] table → `if (!mod)` TRUE
    fs.addFile("mods/nomod-section/manifest.toml", "[other]\nfoo = \"bar\"\n");
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    CHECK(packs.empty());
    CHECK(logger.hasMessage(LogLevel::Error, "missing [mod] table"));
}

TEST_CASE("ModLoader handles manifest with missing required fields") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "partial-mod", true);
    fs.addDir("mods/partial-mod");
    // Has [mod] but missing id, version, engine-api, priority
    fs.addFile("mods/partial-mod/manifest.toml", "[mod]\nname = \"Partial\"\n");
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    CHECK(packs.empty());
    CHECK(logger.hasMessage(LogLevel::Error, "missing required field"));
}

TEST_CASE("ModLoader validateEngineApi handles engine-api without dot separator") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addDir("mods");
    fs.addDirEntry("mods", "nodot-mod", true);
    fs.addDir("mods/nodot-mod");
    // engine-api = "2" (no dot) → dot == npos → major = "2" != kEngineApiMajor → error
    fs.addFile("mods/nodot-mod/manifest.toml", "[mod]\nname = \"NoDot\"\nid = \"nodot\"\nversion = \"1.0\"\n"
                                               "\"engine-api\" = \"2\"\npriority = 1\n");
    ModLoader loader(fs, logger);
    auto packs = loader.load();
    CHECK(packs.empty());
    CHECK(logger.hasMessage(LogLevel::Error, "incompatible"));
}

// ---------------------------------------------------------------------------
// AssetManager — NeedsConfiguration + window + configure() true
// ---------------------------------------------------------------------------

TEST_CASE("AssetManager::initialize keeps pack when NeedsConfiguration and configure() returns true") {
    MockContentPack pack;
    MockLogger logger;
    IWindow* fakeWindow = reinterpret_cast<IWindow*>(0x1);
    pack.initStatus = IContentPack::Status::NeedsConfiguration;
    pack.configureResult = true;

    AssetManager am(makePacks(&pack), logger);
    am.initialize(fakeWindow);

    // Pack was configured and kept — assets should be accessible
    pack.assets[{"sky", AssetType::Texture}] = {0x11};
    REQUIRE(am.loadTexture("sky") != nullptr);
    REQUIRE_FALSE(logger.hasMessage(LogLevel::Warn, "dropping pack"));
}
