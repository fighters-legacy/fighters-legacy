// SPDX-License-Identifier: GPL-3.0-or-later
#include "content/IContentPackEventHandler.h"
#include "content/ModLoader.h"
#include "content/TrustLevel.h"
#include "mock_hal.h"

#include <catch2/catch_test_macros.hpp>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void addMod(MockFilesystem& fs, const std::string& modId, const std::string& manifest) {
    fs.addDir("mods");
    fs.addDirEntry("mods", modId, true);
    fs.addDir("mods/" + modId);
    fs.addFile("mods/" + modId + "/manifest.toml", manifest);
}

static std::string baseManifest(const std::string& id = "test-mod") {
    return "[mod]\nname = \"Test Mod\"\nid = \"" + id +
           "\"\nversion = \"1.0.0\"\n\"engine-api\" = \"1.0\"\npriority = 10\n";
}

static std::string manifestWithTrust(const std::string& id, const std::string& signedBy,
                                     const std::string& signature = {}) {
    std::string s = baseManifest(id);
    s += "[mod.trust]\n";
    s += "signed-by = \"" + signedBy + "\"\n";
    if (!signature.empty())
        s += "signature = \"" + signature + "\"\n";
    return s;
}

// Minimal event handler that records what fired
struct RecordingHandler : public IContentPackEventHandler {
    std::vector<std::string> untrusted;
    std::vector<std::string> nativeCode;

    void onUntrustedPackLoaded(const IContentPack& pack) override {
        untrusted.push_back(pack.id());
    }
    void onNativeCodePackLoaded(const IContentPack& pack) override {
        nativeCode.push_back(pack.id());
    }
};

// ---------------------------------------------------------------------------
// TrustLevel parsing
// ---------------------------------------------------------------------------

TEST_CASE("TrustLevel: missing mod.trust section defaults to Unsigned") {
    MockFilesystem fs;
    MockLogger logger;
    addMod(fs, "test-mod", baseManifest("test-mod"));

    ModLoader loader(fs, logger);
    auto packs = loader.load();

    REQUIRE(packs.size() == 1);
    CHECK(packs[0]->getTrustLevel() == TrustLevel::Unsigned);
}

TEST_CASE("TrustLevel: signed-by community parses as Community") {
    MockFilesystem fs;
    MockLogger logger;
    addMod(fs, "community-mod", manifestWithTrust("community-mod", "community"));

    ModLoader loader(fs, logger);
    auto packs = loader.load();

    REQUIRE(packs.size() == 1);
    CHECK(packs[0]->getTrustLevel() == TrustLevel::Community);
}

TEST_CASE("TrustLevel: signed-by maintainer parses as Maintainer") {
    MockFilesystem fs;
    MockLogger logger;
    addMod(fs, "official-mod", manifestWithTrust("official-mod", "maintainer"));

    ModLoader loader(fs, logger);
    auto packs = loader.load();

    REQUIRE(packs.size() == 1);
    CHECK(packs[0]->getTrustLevel() == TrustLevel::Maintainer);
}

TEST_CASE("TrustLevel: invalid signed-by value falls back to Unsigned with warning") {
    MockFilesystem fs;
    MockLogger logger;
    addMod(fs, "sketchy-mod", manifestWithTrust("sketchy-mod", "hacker"));

    ModLoader loader(fs, logger);
    auto packs = loader.load();

    REQUIRE(packs.size() == 1);
    CHECK(packs[0]->getTrustLevel() == TrustLevel::Unsigned);
    REQUIRE(logger.hasMessage(LogLevel::Warn, "unknown signed-by value"));
}

TEST_CASE("TrustLevel: signature field present logs not-verified message") {
    MockFilesystem fs;
    MockLogger logger;
    addMod(fs, "signed-mod", manifestWithTrust("signed-mod", "community", "AAAABBBBCCCCDDDD"));

    ModLoader loader(fs, logger);
    auto packs = loader.load();

    REQUIRE(packs.size() == 1);
    REQUIRE(logger.hasMessage(LogLevel::Info, "not verified"));
}

// ---------------------------------------------------------------------------
// Native plugin detection
// ---------------------------------------------------------------------------

TEST_CASE("TrustLevel: manifest without native plugin file returns isNativePlugin false") {
    MockFilesystem fs;
    MockLogger logger;
    addMod(fs, "test-mod", baseManifest("test-mod"));
    // No plugin file registered in fs

    ModLoader loader(fs, logger);
    auto packs = loader.load();

    REQUIRE(packs.size() == 1);
    CHECK_FALSE(packs[0]->isNativePlugin());
}

TEST_CASE("Native plugin detection: detects platform-specific plugin filename") {
    MockFilesystem fs;
    MockLogger logger;
    addMod(fs, "native-mod", baseManifest("native-mod"));

    // Register the platform-specific plugin filename
#if defined(_WIN32)
    fs.addFile("mods/native-mod/native-mod.dll", "placeholder");
#elif defined(__APPLE__)
    fs.addFile("mods/native-mod/libnative-mod.dylib", "placeholder");
#else
    fs.addFile("mods/native-mod/libnative-mod.so", "placeholder");
#endif

    // assetsAbsoluteRoot is empty — detection fires but loading is skipped
    ModLoader loader(fs, logger);
    auto packs = loader.load();

    REQUIRE(packs.size() == 1);
    CHECK(packs[0]->isNativePlugin());
}

// ---------------------------------------------------------------------------
// Event handler callbacks
// ---------------------------------------------------------------------------

TEST_CASE("IContentPackEventHandler: onNativeCodePackLoaded fires for plugin pack") {
    MockFilesystem fs;
    MockLogger logger;
    addMod(fs, "native-mod", baseManifest("native-mod"));
#if defined(_WIN32)
    fs.addFile("mods/native-mod/native-mod.dll", "placeholder");
#elif defined(__APPLE__)
    fs.addFile("mods/native-mod/libnative-mod.dylib", "placeholder");
#else
    fs.addFile("mods/native-mod/libnative-mod.so", "placeholder");
#endif

    RecordingHandler handler;
    ModLoader loader(fs, logger);
    loader.load(&handler);

    REQUIRE(handler.nativeCode.size() == 1);
    CHECK(handler.nativeCode[0] == "native-mod");
}

TEST_CASE("IContentPackEventHandler: onUntrustedPackLoaded fires for Unsigned pack") {
    MockFilesystem fs;
    MockLogger logger;
    addMod(fs, "test-mod", baseManifest("test-mod")); // no trust section → Unsigned

    RecordingHandler handler;
    ModLoader loader(fs, logger);
    loader.load(&handler);

    REQUIRE(handler.untrusted.size() == 1);
    CHECK(handler.untrusted[0] == "test-mod");
}

TEST_CASE("IContentPackEventHandler: onUntrustedPackLoaded does not fire for Community pack") {
    MockFilesystem fs;
    MockLogger logger;
    addMod(fs, "community-mod", manifestWithTrust("community-mod", "community"));

    RecordingHandler handler;
    ModLoader loader(fs, logger);
    loader.load(&handler);

    CHECK(handler.untrusted.empty());
}
