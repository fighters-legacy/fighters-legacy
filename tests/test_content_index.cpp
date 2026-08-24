// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for ContentIndex (#810): the id -> asset-name map that lets a def cross-reference another
// def without ever touching the filesystem.
//
// The bug this class exists to kill: an entity's `sensors = ["fl-base:apq159"]` was fed straight to
// AssetManager, which builds "sensors/fl-base:apq159.toml" -- a path that cannot exist, and is not a
// legal Windows filename. Every aircraft in every pack silently flew with no radar.

#include "mock_log.h"
#include <ILogger.h>
#include <content/AssetManager.h>
#include <content/ContentIndex.h>
#include <content/IContentPack.h>
#include <mock_content.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace fl;

namespace {

// Serves def TOML blobs keyed by asset name, for both def asset types.
struct DefPack : public NullContentPack {
    std::string packId{"test-pack"};
    std::string ns{"test-pack"};
    int prio{0};
    std::map<std::string, std::string> entities; // assetName -> TOML
    std::map<std::string, std::string> sensors;

    const char* id() const override {
        return packId.c_str();
    }
    const char* namespaceId() const override {
        return ns.c_str();
    }
    int priority() const override {
        return prio;
    }
    bool hasAsset(const char* n, AssetType t) const override {
        const auto& m = (t == AssetType::SensorDef) ? sensors : entities;
        return (t == AssetType::SensorDef || t == AssetType::EntityDef) && m.count(n) != 0;
    }
    std::optional<EntityDefData> loadEntityDef(const char* n) override {
        auto it = entities.find(n);
        if (it == entities.end())
            return std::nullopt;
        EntityDefData d;
        d.name = n;
        d.bytes.assign(it->second.begin(), it->second.end());
        return d;
    }
    std::optional<SensorDefData> loadSensorDef(const char* n) override {
        auto it = sensors.find(n);
        if (it == sensors.end())
            return std::nullopt;
        SensorDefData d;
        d.name = n;
        d.bytes.assign(it->second.begin(), it->second.end());
        return d;
    }
    std::vector<std::string> listAssets(AssetType t) const override {
        std::vector<std::string> out;
        const auto& m = (t == AssetType::SensorDef) ? sensors : entities;
        if (t == AssetType::SensorDef || t == AssetType::EntityDef)
            for (const auto& [k, v] : m)
                out.push_back(k);
        return out;
    }
};

std::string sensorToml(const std::string& id) {
    return "[sensor]\nid = \"" + id + "\"\nname = \"Radar\"\ntype = \"radar\"\n";
}
std::string entityToml(const std::string& id) {
    return "[entity]\nid = \"" + id + "\"\nname = \"Jet\"\ncategory = \"air_vehicle\"\nmax_hp = 100.0\n";
}

constexpr AssetType kDefTypes[] = {AssetType::EntityDef, AssetType::SensorDef};

// Builds an AssetManager over the given packs, in the order supplied (index 0 = highest priority,
// matching how ModLoader hands them over).
std::unique_ptr<AssetManager> managerOf(std::vector<DefPack> packs, ILogger& log) {
    std::vector<std::unique_ptr<IContentPack>> v;
    for (auto& p : packs)
        v.push_back(std::make_unique<DefPack>(std::move(p)));
    auto am = std::make_unique<AssetManager>(std::move(v), log);
    am->initialize(nullptr);
    return am;
}

} // namespace

TEST_CASE("ContentIndex resolves a namespaced id to its asset name", "[content_index]") {
    DefPack pack;
    pack.ns = "fl-base";
    pack.sensors["apq159"] = sensorToml("fl-base:apq159");
    pack.entities["f5e"] = entityToml("fl-base:f5e");

    RecordingLogger log;
    auto assets = managerOf({std::move(pack)}, log);

    ContentIndex index;
    index.build(*assets, kDefTypes, log);

    const std::string* sensor = index.assetNameFor(AssetType::SensorDef, "fl-base:apq159");
    REQUIRE(sensor != nullptr);
    CHECK(*sensor == "apq159");

    const std::string* entity = index.assetNameFor(AssetType::EntityDef, "fl-base:f5e");
    REQUIRE(entity != nullptr);
    CHECK(*entity == "f5e");

    // The id and the asset name are different strings, which is the whole point.
    CHECK(*sensor != "fl-base:apq159");
}

TEST_CASE("ContentIndex returns nullptr for an unknown id", "[content_index]") {
    DefPack pack;
    pack.ns = "fl-base";
    pack.sensors["apq159"] = sensorToml("fl-base:apq159");

    RecordingLogger log;
    auto assets = managerOf({std::move(pack)}, log);
    ContentIndex index;
    index.build(*assets, kDefTypes, log);

    CHECK(index.assetNameFor(AssetType::SensorDef, "fl-base:nonexistent") == nullptr);
    // Right id, wrong type: the two namespaces are indexed separately.
    CHECK(index.assetNameFor(AssetType::EntityDef, "fl-base:apq159") == nullptr);
}

TEST_CASE("ContentIndex id lookup is case-insensitive", "[content_index]") {
    // AssetManager::cacheKey lowercases every name before it reaches a pack, so a case-sensitive id
    // lookup would resolve through the index and then miss the file.
    DefPack pack;
    pack.ns = "fl-base";
    pack.sensors["apq159"] = sensorToml("FL-Base:APQ159");

    RecordingLogger log;
    auto assets = managerOf({std::move(pack)}, log);
    ContentIndex index;
    index.build(*assets, kDefTypes, log);

    REQUIRE(index.assetNameFor(AssetType::SensorDef, "fl-base:apq159") != nullptr);
    REQUIRE(index.assetNameFor(AssetType::SensorDef, "FL-BASE:APQ159") != nullptr);
}

TEST_CASE("ContentIndex duplicate id across packs: the higher-priority pack wins", "[content_index]") {
    DefPack high;
    high.packId = "theater";
    high.ns = "fl-base"; // a theater pack legitimately re-tunes a base-pack radar
    high.sensors["apq159_tuned"] = sensorToml("fl-base:apq159");

    DefPack low;
    low.packId = "base";
    low.ns = "fl-base";
    low.sensors["apq159"] = sensorToml("fl-base:apq159");

    RecordingLogger log;
    auto assets = managerOf({std::move(high), std::move(low)}, log); // index 0 = highest priority
    ContentIndex index;
    index.build(*assets, kDefTypes, log);

    const std::string* name = index.assetNameFor(AssetType::SensorDef, "fl-base:apq159");
    REQUIRE(name != nullptr);
    CHECK(*name == "apq159_tuned");
    CHECK(log.hasMessage(LogLevel::Warn, "duplicate def id"));
}

TEST_CASE("ContentIndex refuses to index an id containing a path separator", "[content_index]") {
    // An id feeds a path builder the moment it resolves, so this is a traversal boundary.
    DefPack pack;
    pack.ns = "fl-base";
    pack.sensors["evil"] = sensorToml("fl-base:../../../etc/passwd");
    pack.sensors["evil2"] = sensorToml("fl-base:..\\..\\windows\\system32");

    RecordingLogger log;
    auto assets = managerOf({std::move(pack)}, log);
    ContentIndex index;
    index.build(*assets, kDefTypes, log);

    CHECK(index.assetNameFor(AssetType::SensorDef, "fl-base:../../../etc/passwd") == nullptr);
    CHECK(index.assetNameFor(AssetType::SensorDef, "fl-base:..\\..\\windows\\system32") == nullptr);
    CHECK(index.idsOfType(AssetType::SensorDef).empty());
    CHECK(log.hasMessage(LogLevel::Error, "path separator"));
}

TEST_CASE("ContentIndex warns when a def id does not match its pack's namespace", "[content_index]") {
    DefPack pack;
    pack.packId = "fl-base-pack";
    pack.ns = "fl-base";
    pack.sensors["apq159"] = sensorToml("some-other-pack:apq159");

    RecordingLogger log;
    auto assets = managerOf({std::move(pack)}, log);
    ContentIndex index;
    index.build(*assets, kDefTypes, log);

    // Still indexed -- a mis-prefixed def resolves; it just says so out loud.
    CHECK(index.assetNameFor(AssetType::SensorDef, "some-other-pack:apq159") != nullptr);
    CHECK(log.hasMessage(LogLevel::Warn, "does not match its pack's declared namespace"));
}

TEST_CASE("ContentIndex warns on an un-namespaced def id", "[content_index]") {
    DefPack pack;
    pack.ns = "fl-base";
    pack.sensors["apq159"] = sensorToml("apq159"); // no "ns:" prefix

    RecordingLogger log;
    auto assets = managerOf({std::move(pack)}, log);
    ContentIndex index;
    index.build(*assets, kDefTypes, log);

    CHECK(index.assetNameFor(AssetType::SensorDef, "apq159") != nullptr); // indexed anyway
    CHECK(log.hasMessage(LogLevel::Warn, "is not namespaced"));
}

TEST_CASE("ContentIndex skips a def with no id and unparseable TOML", "[content_index]") {
    DefPack pack;
    pack.ns = "fl-base";
    pack.sensors["noid"] = "[sensor]\nname = \"Nameless\"\n";
    pack.sensors["broken"] = "[sensor\nid = ";

    RecordingLogger log;
    auto assets = managerOf({std::move(pack)}, log);
    ContentIndex index;
    index.build(*assets, kDefTypes, log);

    CHECK(index.idsOfType(AssetType::SensorDef).empty());
    CHECK(log.hasMessage(LogLevel::Warn, "declares no [sensor] id"));
    CHECK(log.hasMessage(LogLevel::Warn, "not parseable TOML"));
}

TEST_CASE("ContentIndex reports an asset type that has no def id", "[content_index]") {
    DefPack pack;
    RecordingLogger log;
    auto assets = managerOf({std::move(pack)}, log);

    ContentIndex index;
    constexpr AssetType kBogus[] = {AssetType::Mesh};
    index.build(*assets, kBogus, log);

    CHECK(log.hasMessage(LogLevel::Error, "cannot be indexed"));
}

TEST_CASE("ContentIndex idsOfType lists only that type's ids", "[content_index]") {
    DefPack pack;
    pack.ns = "fl-base";
    pack.sensors["apq159"] = sensorToml("fl-base:apq159");
    pack.sensors["eyeball"] = sensorToml("fl-base:eyeball");
    pack.entities["f5e"] = entityToml("fl-base:f5e");

    RecordingLogger log;
    auto assets = managerOf({std::move(pack)}, log);
    ContentIndex index;
    index.build(*assets, kDefTypes, log);

    CHECK(index.idsOfType(AssetType::SensorDef).size() == 2);
    CHECK(index.idsOfType(AssetType::EntityDef).size() == 1);
    CHECK(index.idsOfType(AssetType::EntityDef)[0] == "fl-base:f5e");

    index.clear();
    CHECK(index.idsOfType(AssetType::SensorDef).empty());
}
