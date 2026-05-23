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
