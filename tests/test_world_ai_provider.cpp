// SPDX-License-Identifier: GPL-3.0-or-later
#include "mock_hal.h"
#include "world/NullAiProvider.h"
#include "world/WorldAiProviderHost.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace fl;

// The AI provider seam (#163).
//
// Two things under test, and the second is where the risk lives. NullAiProvider is a contract check.
// applyWorldEvolution takes a delta produced by a MODEL — something that may be confused, out of
// date, or being steered by the player chat it was shown — so every case here is a way that delta
// could be wrong, and the assertion is that the world does not change and the caller is told.

namespace {

MockLogger& testLogger() {
    static MockLogger log;
    return log;
}

// Sinks that record what actually reached the world, so a test can assert on the difference between
// "rejected" and "applied but ineffective".
struct RecordingSinks {
    std::vector<std::pair<uint16_t, AlertLevel>> alerts;
    std::vector<std::string> zones;
    std::vector<std::string> spawns;
    int relationships{0};
    uint16_t factions{3};

    [[nodiscard]] WorldEvolutionSinks build() {
        WorldEvolutionSinks s;
        s.factionCount = [this] { return factions; };
        s.setAlertLevel = [this](uint16_t idx, AlertLevel l) {
            alerts.emplace_back(idx, l);
            return true;
        };
        s.setRelationship = [this](uint16_t, uint16_t, FactionRelation) {
            ++relationships;
            return true;
        };
        s.setZoneOwner = [this](const std::string& id, uint16_t) {
            zones.push_back(id);
            return id != "nonexistent";
        };
        s.spawn = [this](const SpawnEvent& ev) {
            spawns.push_back(ev.entityTypeId);
            return true;
        };
        return s;
    }
};

const std::vector<std::string> kVocab{"builtin:debug-entity", "fl-base:mig21"};

} // namespace

// ── NullAiProvider contract ─────────────────────────────────────────────────────────────────────

TEST_CASE("null provider: supports nothing, and says so", "[worldai]") {
    NullAiProvider p;
    REQUIRE(p.init(testLogger()));
    // A caller degrades by ASKING, not by discovering — so every capability must answer false rather
    // than the caller finding out from an empty result three seconds later.
    for (uint8_t i = 0; i < static_cast<uint8_t>(WorldAiCapability::Count); ++i)
        CHECK_FALSE(p.supports(static_cast<WorldAiCapability>(i)));
    CHECK(p.getLastError() != nullptr);
    p.shutdown();
}

TEST_CASE("null provider: every request is refused at the call, not completed empty", "[worldai]") {
    NullAiProvider p;
    REQUIRE(p.init(testLogger()));

    bool anyCallbackFired = false;
    auto strCb = [&](std::string, std::string) { anyCallbackFired = true; };

    WorldAiContext ctx;
    CHECK(p.requestMission(ctx, strCb) == 0);
    CHECK(p.requestNarrative(NarrativeType::Briefing, ctx, strCb) == 0);
    CHECK(p.requestIntent(ctx, strCb) == 0);
    CHECK(p.requestWorldEvolution(ctx, [&](WorldEvolutionDelta, std::string) { anyCallbackFired = true; }) == 0);
    CHECK(p.requestFactionDecision("red", ctx, [&](FactionDecision, std::string) { anyCallbackFired = true; }) == 0);

    // "I cannot do this" reaches the caller synchronously. A completion that arrives later with an
    // empty result would be indistinguishable from a working provider behind a bad model.
    p.service();
    CHECK_FALSE(anyCallbackFired);
    p.shutdown();
}

TEST_CASE("null provider: service and shutdown are safe in any order", "[worldai]") {
    NullAiProvider p;
    p.service();  // before init
    p.shutdown(); // before init
    REQUIRE(p.init(testLogger()));
    p.shutdown();
    p.shutdown(); // twice
    p.service();  // after shutdown
    p.cancel(0);
    p.cancel(12345); // an id that was never issued
    SUCCEED("no crash");
}

// ── loading ─────────────────────────────────────────────────────────────────────────────────────

TEST_CASE("provider load: no plugin configured is not a failure", "[worldai]") {
    bool failed = true;
    auto p = loadWorldAiProvider("", testLogger(), failed);
    REQUIRE(p != nullptr);
    // Not configured and failed-to-load are different facts, and only the second deserves an error
    // in the log.
    CHECK_FALSE(failed);
    CHECK_FALSE(p->supports(WorldAiCapability::Mission));
}

TEST_CASE("provider load: a configured plugin that cannot be loaded reports failure", "[worldai]") {
    bool failed = false;
    auto p = loadWorldAiProvider("/nonexistent/definitely-not-a-real-plugin.so", testLogger(), failed);
    // Still a usable provider — ONE degradation path — but the caller can now say so loudly instead
    // of running scripted content for a week because a path was mistyped.
    REQUIRE(p != nullptr);
    CHECK(failed);
    CHECK_FALSE(p->supports(WorldAiCapability::Mission));
}

// ── delta application: the happy path ───────────────────────────────────────────────────────────

TEST_CASE("delta: an empty delta is a valid answer, not an error", "[worldai]") {
    RecordingSinks rec;
    const auto sinks = rec.build();
    const auto r = applyWorldEvolution(WorldEvolutionDelta{}, kVocab, sinks);
    // "Nothing changed today" is a thing a world-evolution model should be able to say.
    CHECK(r.applied == 0);
    CHECK(r.rejected == 0);
    CHECK(r.rejections.empty());
}

TEST_CASE("delta: valid changes reach the world", "[worldai]") {
    RecordingSinks rec;
    const auto sinks = rec.build();

    WorldEvolutionDelta d;
    d.alertChanges.push_back({1, AlertLevel::Conflict});
    d.relationshipChanges.push_back({1, 2, FactionRelation::Hostile});
    d.zoneChanges.push_back({"north-adiz", 1});
    d.spawnEvents.push_back({"fl-base:mig21", 2, {100.0, 500.0, 200.0}, 90.0});

    const auto r = applyWorldEvolution(d, kVocab, sinks);
    CHECK(r.applied == 4);
    CHECK(r.rejected == 0);
    REQUIRE(rec.alerts.size() == 1);
    CHECK(rec.alerts[0].first == 1);
    CHECK(rec.alerts[0].second == AlertLevel::Conflict);
    CHECK(rec.relationships == 1);
    CHECK(rec.spawns == std::vector<std::string>{"fl-base:mig21"});
}

// ── delta application: the ways a model gets it wrong ───────────────────────────────────────────

TEST_CASE("delta: an out-of-range faction index is rejected everywhere it can appear", "[worldai]") {
    RecordingSinks rec;
    rec.factions = 3; // valid indices are 0..2
    const auto sinks = rec.build();

    WorldEvolutionDelta d;
    d.alertChanges.push_back({7, AlertLevel::Conflict});
    d.relationshipChanges.push_back({0, 9, FactionRelation::Hostile});
    d.zoneChanges.push_back({"north-adiz", 42});
    d.spawnEvents.push_back({"fl-base:mig21", 300, {0, 0, 0}, 0});

    const auto r = applyWorldEvolution(d, kVocab, sinks);
    // A model counting coalitions out of a prompt has no reason to land on the registry's indices.
    CHECK(r.applied == 0);
    CHECK(r.rejected == 4);
    CHECK(rec.alerts.empty());
    CHECK(rec.relationships == 0);
    CHECK(rec.zones.empty());
    CHECK(rec.spawns.empty());
}

TEST_CASE("delta: with no faction registry, every faction-indexed change is rejected", "[worldai]") {
    RecordingSinks rec;
    rec.factions = 0;
    const auto sinks = rec.build();

    WorldEvolutionDelta d;
    d.alertChanges.push_back({0, AlertLevel::Elevated});
    const auto r = applyWorldEvolution(d, kVocab, sinks);
    // Index 0 is in range of nothing. Applying it against an absent registry would be a change
    // nobody can observe.
    CHECK(r.applied == 0);
    CHECK(r.rejected == 1);
}

TEST_CASE("delta: a faction cannot be set hostile to itself", "[worldai]") {
    RecordingSinks rec;
    const auto sinks = rec.build();

    WorldEvolutionDelta d;
    d.relationshipChanges.push_back({1, 1, FactionRelation::Hostile});
    const auto r = applyWorldEvolution(d, kVocab, sinks);
    // The registry seeds the diagonal Friendly. Letting a model rewrite it is a way to make a team
    // hostile to itself, which reads in play as the AI attacking its own side for no reason.
    CHECK(r.applied == 0);
    CHECK(r.rejected == 1);
    CHECK(r.rejections[0].find("itself") != std::string::npos);
}

TEST_CASE("delta: a spawn outside the advertised vocabulary is rejected", "[worldai]") {
    RecordingSinks rec;
    const auto sinks = rec.build();

    WorldEvolutionDelta d;
    d.spawnEvents.push_back({"fl-base:f22-raptor-invented-by-the-model", 1, {0, 100, 0}, 0});
    const auto r = applyWorldEvolution(d, kVocab, sinks);
    // The vocabulary IS what the context advertised. A model must not get to probe what else the
    // server happens to have registered.
    CHECK(r.applied == 0);
    CHECK(r.rejected == 1);
    CHECK(rec.spawns.empty());
}

TEST_CASE("delta: a non-finite spawn position is rejected before it reaches the sim", "[worldai]") {
    RecordingSinks rec;
    const auto sinks = rec.build();

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    WorldEvolutionDelta d;
    d.spawnEvents.push_back({"fl-base:mig21", 1, {nan, 0, 0}, 0});
    d.spawnEvents.push_back({"fl-base:mig21", 1, {0, inf, 0}, 0});
    d.spawnEvents.push_back({"fl-base:mig21", 1, {0, 0, 0}, nan}); // heading

    const auto r = applyWorldEvolution(d, kVocab, sinks);
    // A NaN reaching a transform propagates into everything that reads it, and by the time the
    // quantizer clamps it the entity is already lost.
    CHECK(r.applied == 0);
    CHECK(r.rejected == 3);
    CHECK(rec.spawns.empty());
}

TEST_CASE("delta: an unknown zone is rejected by the sink and counted", "[worldai]") {
    RecordingSinks rec;
    const auto sinks = rec.build();

    WorldEvolutionDelta d;
    d.zoneChanges.push_back({"nonexistent", 1});
    d.zoneChanges.push_back({"", 1});

    const auto r = applyWorldEvolution(d, kVocab, sinks);
    CHECK(r.applied == 0);
    CHECK(r.rejected == 2);
}

TEST_CASE("delta: a missing sink is a rejection, not a silent no-op", "[worldai]") {
    // The distinction that matters when reading a result: "the change did not hold" and "nothing was
    // wired up to receive it" must not look the same.
    WorldEvolutionSinks sinks;
    sinks.factionCount = [] { return uint16_t{3}; };

    WorldEvolutionDelta d;
    d.alertChanges.push_back({1, AlertLevel::Conflict});
    d.relationshipChanges.push_back({0, 1, FactionRelation::Hostile});
    d.zoneChanges.push_back({"north-adiz", 1});
    d.spawnEvents.push_back({"fl-base:mig21", 1, {0, 100, 0}, 0});

    const auto r = applyWorldEvolution(d, kVocab, sinks);
    CHECK(r.applied == 0);
    CHECK(r.rejected == 4);
    REQUIRE(r.rejections.size() == 4);
    for (const std::string& why : r.rejections)
        CHECK(why.find("no ") != std::string::npos); // "no alert system configured", etc.
}

TEST_CASE("delta: a sink that refuses is counted as rejected", "[worldai]") {
    WorldEvolutionSinks sinks;
    sinks.factionCount = [] { return uint16_t{3}; };
    sinks.setAlertLevel = [](uint16_t, AlertLevel) { return false; };

    WorldEvolutionDelta d;
    d.alertChanges.push_back({1, AlertLevel::Conflict});
    const auto r = applyWorldEvolution(d, kVocab, sinks);
    CHECK(r.applied == 0);
    CHECK(r.rejected == 1);
}

TEST_CASE("delta: a partly-valid delta applies what holds and reports what did not", "[worldai]") {
    RecordingSinks rec;
    const auto sinks = rec.build();

    WorldEvolutionDelta d;
    d.alertChanges.push_back({1, AlertLevel::Conflict});           // ok
    d.alertChanges.push_back({99, AlertLevel::WarState});          // bad index
    d.spawnEvents.push_back({"fl-base:mig21", 1, {0, 100, 0}, 0}); // ok
    d.spawnEvents.push_back({"not-a-type", 1, {0, 100, 0}, 0});    // bad vocabulary

    const auto r = applyWorldEvolution(d, kVocab, sinks);
    // A delta that silently half-applies is indistinguishable from one that worked, which is why the
    // rejections are returned rather than logged and forgotten.
    CHECK(r.applied == 2);
    CHECK(r.rejected == 2);
    CHECK(r.rejections.size() == 2);
    CHECK(rec.alerts.size() == 1);
    CHECK(rec.spawns.size() == 1);
}

TEST_CASE("delta: capability names round-trip for every ordinal", "[worldai]") {
    for (uint8_t i = 0; i < static_cast<uint8_t>(WorldAiCapability::Count); ++i) {
        CHECK(isWorldAiCapabilityOrdinal(i));
        const std::string name = worldAiCapabilityName(static_cast<WorldAiCapability>(i));
        CHECK_FALSE(name.empty());
        CHECK(name != "unknown");
    }
    CHECK_FALSE(isWorldAiCapabilityOrdinal(static_cast<uint8_t>(WorldAiCapability::Count)));
}
