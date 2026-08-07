// SPDX-License-Identifier: GPL-3.0-or-later
//
// ModLoader::load — the two-pass scan (#1145).
//
// test_trust_framework.cpp covers trust levels and plugin detection; test_manifest_sanitization.cpp
// covers the id rules. What neither covers is the SCAN: what happens to a directory that is not a
// mod, a mod built against a different engine, a mod whose dependency is not installed, and how the
// surviving packs end up ordered.
//
// Order is the part with teeth. Priority decides which pack wins when two define the same asset, so
// a sort that is wrong silently serves the loser — and the author sees their override do nothing.

#include <catch2/catch_test_macros.hpp>

#include "content/IContentPackEventHandler.h"
#include "content/ModLoader.h"
#include "mock_hal.h"

#include <string>
#include <vector>

using namespace fl;

namespace {

void addMod(MockFilesystem& fs, const std::string& dirName, const std::string& manifest) {
    fs.addDir("mods");
    fs.addDirEntry("mods", dirName, true);
    fs.addFile("mods/" + dirName + "/manifest.toml", manifest);
}

std::string manifestOf(const std::string& id, int priority = 0, const std::string& engineApi = "1.0",
                       const std::string& depends = {}) {
    std::string s = "[mod]\nname = \"" + id + "\"\nid = \"" + id + "\"\nversion = \"1.0.0\"\n\"engine-api\" = \"" +
                    engineApi + "\"\npriority = " + std::to_string(priority) + "\n";
    if (!depends.empty())
        s += "depends = [" + depends + "]\n";
    return s;
}

std::vector<std::string> idsOf(const std::vector<std::unique_ptr<IContentPack>>& packs) {
    std::vector<std::string> out;
    for (const auto& p : packs)
        out.emplace_back(p->id());
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// The scan
// ---------------------------------------------------------------------------

TEST_CASE("ModLoader: an absent or empty mods directory is normal, not an error (#1145)", "[content][modloader]") {
    // A fresh install has no mods at all. This has to be an Info line and an empty list, because
    // anything louder trains an operator to ignore the log.
    MockFilesystem fs;
    MockLogger log;
    ModLoader loader(fs, log);
    CHECK(loader.load().empty());
    CHECK(loader.getLoadErrors().empty());
    CHECK(log.hasMessage(LogLevel::Info, "no content packs loaded"));
}

TEST_CASE("ModLoader: a loose file in mods/ is skipped, not parsed (#1145)", "[content][modloader]") {
    // README.md and .DS_Store live here. Only directories are candidates.
    MockFilesystem fs;
    MockLogger log;
    fs.addDir("mods");
    fs.addDirEntry("mods", "README.md", false);
    fs.addDirEntry("mods", "notes.txt", false);
    ModLoader loader(fs, log);
    CHECK(loader.load().empty());
    CHECK(loader.getLoadErrors().empty()); // a stray file is not a failed mod
}

TEST_CASE("ModLoader: a directory with no manifest is silently skipped (#1145)", "[content][modloader]") {
    // An empty scratch directory under mods/ is a very common thing to find. It is not an error, so
    // it must not land in loadErrors, which is what the UI shows the player.
    MockFilesystem fs;
    MockLogger log;
    fs.addDir("mods");
    fs.addDirEntry("mods", "scratch", true);
    ModLoader loader(fs, log);
    CHECK(loader.load().empty());
    CHECK(loader.getLoadErrors().empty());
    CHECK(log.hasMessage(LogLevel::Debug, "no manifest.toml"));
}

TEST_CASE("ModLoader: an unparseable manifest is a recorded load error (#1145)", "[content][modloader]") {
    // Here the author DID try to ship a mod, so it must appear in loadErrors with the directory
    // named — that is the only breadcrumb they get.
    MockFilesystem fs;
    MockLogger log;
    addMod(fs, "broken", "[mod\nname = ");
    addMod(fs, "good", manifestOf("good"));

    ModLoader loader(fs, log);
    const auto packs = loader.load();

    CHECK(idsOf(packs) == std::vector<std::string>{"good"});
    REQUIRE(loader.getLoadErrors().size() == 1u);
    CHECK(loader.getLoadErrors()[0].path == "mods/broken");
    CHECK(loader.getLoadErrors()[0].modId.empty()); // unparseable: there is no id to report
}

// ---------------------------------------------------------------------------
// engine-api
// ---------------------------------------------------------------------------

TEST_CASE("ModLoader: the engine-api major version must match (#1145)", "[content][modloader]") {
    // Loading a mod built against a different major would give it an API that has since changed
    // shape — a crash deep in the engine instead of a line in the log.
    MockFilesystem fs;
    MockLogger log;
    addMod(fs, "future", manifestOf("future", 0, "2.0"));
    addMod(fs, "ancient", manifestOf("ancient", 0, "0.9"));
    addMod(fs, "current", manifestOf("current", 0, "1.7")); // a later MINOR is compatible

    ModLoader loader(fs, log);
    const auto packs = loader.load();

    CHECK(idsOf(packs) == std::vector<std::string>{"current"});
    CHECK(loader.getLoadErrors().size() == 2u);
    CHECK(log.hasMessage(LogLevel::Error, "incompatible"));
}

TEST_CASE("ModLoader: a bare major version with no dot is accepted (#1145)", "[content][modloader]") {
    MockFilesystem fs;
    MockLogger log;
    addMod(fs, "bare", manifestOf("bare", 0, "1"));
    ModLoader loader(fs, log);
    CHECK(loader.load().size() == 1u);
}

// ---------------------------------------------------------------------------
// Dependencies
// ---------------------------------------------------------------------------

TEST_CASE("ModLoader: a missing dependency warns but still loads (#1145)", "[content][modloader]") {
    // Deliberately a warning, not a refusal: a mod that merely PREFERS another one still works
    // alone, and refusing would make one missing optional pack take a whole load-out down.
    MockFilesystem fs;
    MockLogger log;
    addMod(fs, "dependent", manifestOf("dependent", 0, "1.0", "\"absent-base\""));

    ModLoader loader(fs, log);
    const auto packs = loader.load();

    CHECK(packs.size() == 1u);
    CHECK(loader.getLoadErrors().empty());
    CHECK(log.hasMessage(LogLevel::Warn, "absent-base"));
}

TEST_CASE("ModLoader: a satisfied dependency is silent regardless of scan order (#1145)", "[content][modloader]") {
    // Both passes exist precisely so a mod can depend on one that is scanned after it. If the
    // dependency check ran during pass 1 this would warn, and authors would be told to rename their
    // directories to control alphabetical order.
    MockFilesystem fs;
    MockLogger log;
    addMod(fs, "a-dependent", manifestOf("dependent", 0, "1.0", "\"zbase\""));
    addMod(fs, "z-base", manifestOf("zbase"));

    ModLoader loader(fs, log);
    CHECK(loader.load().size() == 2u);
    CHECK_FALSE(log.hasMessage(LogLevel::Warn, "not found"));
}

// ---------------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------------

TEST_CASE("ModLoader: packs come back highest priority first (#1145)", "[content][modloader]") {
    // Index 0 wins asset lookups. An author raising their priority to override a base asset is
    // relying on exactly this.
    MockFilesystem fs;
    MockLogger log;
    addMod(fs, "base", manifestOf("base", 0));
    addMod(fs, "override", manifestOf("override", 100));
    addMod(fs, "middle", manifestOf("middle", 50));
    addMod(fs, "under", manifestOf("under", -10)); // a negative priority is a deliberate "load last"

    ModLoader loader(fs, log);
    const auto packs = loader.load();
    REQUIRE(packs.size() == 4u);
    CHECK(idsOf(packs) == std::vector<std::string>{"override", "middle", "base", "under"});
}

// ---------------------------------------------------------------------------
// Native plugins
// ---------------------------------------------------------------------------

TEST_CASE("ModLoader: a plugin with no assets root loads as a folder pack and says so (#1145)",
          "[content][modloader]") {
    // Plugin loading needs an ABSOLUTE path so a planted library elsewhere on the search path cannot
    // be picked up. With no root configured the code refuses to dlopen anything — but the mod's
    // data half is still perfectly usable, so it degrades rather than disappearing.
    MockFilesystem fs;
    MockLogger log;
    addMod(fs, "native", manifestOf("native"));
#if defined(_WIN32)
    fs.addFile("mods/native/native.dll", "placeholder");
#elif defined(__APPLE__)
    fs.addFile("mods/native/libnative.dylib", "placeholder");
#else
    fs.addFile("mods/native/libnative.so", "placeholder");
#endif

    struct Handler final : IContentPackEventHandler {
        std::vector<std::string> nativeCode, untrusted;
        void onUntrustedPackLoaded(const IContentPack& p) override {
            untrusted.emplace_back(p.id());
        }
        void onNativeCodePackLoaded(const IContentPack& p) override {
            nativeCode.emplace_back(p.id());
        }
    } handler;

    ModLoader loader(fs, log); // assetsAbsoluteRoot empty
    const auto packs = loader.load(&handler);

    REQUIRE(packs.size() == 1u);
    CHECK(packs[0]->isNativePlugin()); // still FLAGGED as native — the warning depends on it
    CHECK(log.hasMessage(LogLevel::Warn, "plugin loading is disabled"));
    // The security events still fire: the player is told this pack carries native code and is
    // unsigned even though the library itself was not opened.
    CHECK(handler.nativeCode == std::vector<std::string>{"native"});
    CHECK(handler.untrusted == std::vector<std::string>{"native"});
}

TEST_CASE("ModLoader: a plugin that will not open is dropped entirely (#1145)", "[content][modloader]") {
    // Not degraded to a folder pack: the manifest said this mod is code, and serving its data
    // without its code would be a half-loaded mod behaving in ways the author never tested.
    MockFilesystem fs;
    MockLogger log;
    addMod(fs, "native", manifestOf("native"));
#if defined(_WIN32)
    fs.addFile("mods/native/native.dll", "not a real library");
#elif defined(__APPLE__)
    fs.addFile("mods/native/libnative.dylib", "not a real library");
#else
    fs.addFile("mods/native/libnative.so", "not a real library");
#endif
    addMod(fs, "plain", manifestOf("plain"));

    ModLoader loader(fs, log, "/nonexistent/assets/root");
    const auto packs = loader.load();

    CHECK(idsOf(packs) == std::vector<std::string>{"plain"});
    REQUIRE(loader.getLoadErrors().size() == 1u);
    CHECK(loader.getLoadErrors()[0].modId == "native");
    CHECK(loader.getLoadErrors()[0].reason.find("plugin failed to load") != std::string::npos);
}

// ---------------------------------------------------------------------------
// State between loads
// ---------------------------------------------------------------------------

TEST_CASE("ModLoader: loadErrors describes the latest load only (#1145)", "[content][modloader]") {
    // The mod screen can reload. Errors from a run the player has already fixed must not persist.
    MockFilesystem fs;
    MockLogger log;
    addMod(fs, "broken", "[mod\n");

    ModLoader loader(fs, log);
    loader.load();
    REQUIRE(loader.getLoadErrors().size() == 1u);

    fs.files.erase("mods/broken/manifest.toml");
    fs.addFile("mods/broken/manifest.toml", manifestOf("broken"));
    const auto packs = loader.load();
    CHECK(packs.size() == 1u);
    CHECK(loader.getLoadErrors().empty());
}

TEST_CASE("ModLoader: a namespace distinct from the id survives into the pack (#1145)", "[content][modloader]") {
    // The namespace prefixes every def id the pack registers. Two packs sharing one deliberately is
    // how a content bundle is split across directories.
    MockFilesystem fs;
    MockLogger log;
    addMod(fs, "part-one", manifestOf("part-one") + "namespace = \"bundle\"\n");

    ModLoader loader(fs, log);
    const auto packs = loader.load();
    REQUIRE(packs.size() == 1u);
    CHECK(std::string(packs[0]->id()) == "part-one");
    CHECK(std::string(packs[0]->namespaceId()) == "bundle");
}
