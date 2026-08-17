// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/WorldState.h"
#include "net/WorldStateJson.h"
#include "world/FactionRegistry.h"

#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "world/FormationRegistry.h"

#include <ILogger.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace fl;

namespace {

struct NullLog : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

EntityDef makeDef(const char* id, ObjectCategory cat) {
    EntityDef def;
    def.id = id;
    def.name = id;
    def.category = cat;
    def.maxHp = 100.0f;
    return def;
}

EntityTransform xform(double x, double y, double z) {
    EntityTransform t;
    t.pos[0] = x;
    t.pos[1] = y;
    t.pos[2] = z;
    return t;
}

} // namespace

TEST_CASE("WorldState: entities are aggregated in ascending pool order (deterministic)", "[world_state]") {
    NullLog log;
    EntityTypeRegistry registry;
    registry.registerType(makeDef("air", ObjectCategory::AirVehicle));
    registry.registerType(makeDef("ground", ObjectCategory::GroundVehicle));
    EntityManager em(log, registry);

    const EntityId a = em.spawn("air", xform(100.0, 200.0, 300.0), /*ownerId=*/7);
    const EntityId b = em.spawn("ground", xform(-50.0, 0.0, 25.0), /*ownerId=*/0);
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    // Give the entities distinct faction / hp so the copy is observable.
    em.get(a)->factionIndex = 1;
    em.get(a)->hp = 50.0f; // half
    em.get(a)->playerOwned = true;
    em.get(b)->factionIndex = 2;
    em.get(b)->hp = 100.0f;

    const WorldStateSnapshot snap =
        buildWorldStateSnapshot(42, em, registry, /*formations=*/nullptr, /*factions=*/nullptr, {},
                                WorldStateEnvironment{/*weatherPreset=*/3, /*timeOfDayHours=*/14.5f, 0.f, 0.f},
                                /*mission=*/nullptr);

    CHECK(snap.tick == 42);
    CHECK(snap.weatherPreset == 3);
    CHECK(snap.timeOfDayHours == Catch::Approx(14.5f));
    REQUIRE(snap.entities.size() == 2u);

    // Ascending pool index (a spawned first -> lower index).
    CHECK(snap.entities[0].entityIdx < snap.entities[1].entityIdx);

    const WorldStateEntity& ea = snap.entities[0];
    CHECK(ea.entityIdx == a.index);
    CHECK(ea.gen == static_cast<uint16_t>(a.generation));
    CHECK(ea.factionIndex == 1);
    CHECK(ea.ownerPeerId == 7);
    CHECK(ea.category == static_cast<uint8_t>(ObjectCategory::AirVehicle));
    CHECK(ea.hpFrac == Catch::Approx(0.5f));
    CHECK((ea.flags & kWorldStatePlayerOwned) != 0);
    CHECK(ea.pos[0] == Catch::Approx(100.0));
    CHECK(ea.pos[2] == Catch::Approx(300.0));

    const WorldStateEntity& eb = snap.entities[1];
    CHECK(eb.category == static_cast<uint8_t>(ObjectCategory::GroundVehicle));
    CHECK(eb.factionIndex == 2);
    CHECK(eb.hpFrac == Catch::Approx(1.0f));
    CHECK((eb.flags & kWorldStatePlayerOwned) == 0);

    // Determinism: same entity set -> byte-for-byte identical entity list.
    const WorldStateSnapshot snap2 = buildWorldStateSnapshot(42, em, registry, nullptr, nullptr, {},
                                                             WorldStateEnvironment{3, 14.5f, 0.f, 0.f}, nullptr);
    REQUIRE(snap2.entities.size() == snap.entities.size());
    for (std::size_t i = 0; i < snap.entities.size(); ++i) {
        CHECK(snap2.entities[i].entityIdx == snap.entities[i].entityIdx);
        CHECK(snap2.entities[i].category == snap.entities[i].category);
        CHECK(snap2.entities[i].pos[0] == snap.entities[i].pos[0]);
    }
}

TEST_CASE("WorldState: dead entities are skipped", "[world_state]") {
    NullLog log;
    EntityTypeRegistry registry;
    registry.registerType(makeDef("air", ObjectCategory::AirVehicle));
    EntityManager em(log, registry);

    const EntityId a = em.spawn("air", xform(0, 0, 0));
    const EntityId b = em.spawn("air", xform(1, 1, 1));
    REQUIRE(a.valid());
    REQUIRE(b.valid());

    em.get(a)->dead = true; // mark dead without a full kill/reap cycle
    const WorldStateSnapshot snap =
        buildWorldStateSnapshot(1, em, registry, nullptr, nullptr, {}, WorldStateEnvironment{}, nullptr);
    REQUIRE(snap.entities.size() == 1u);
    CHECK(snap.entities[0].entityIdx == b.index);
}

TEST_CASE("WorldState: formationId is resolved from the formation registry", "[world_state]") {
    NullLog log;
    EntityTypeRegistry registry;
    registry.registerType(makeDef("air", ObjectCategory::AirVehicle));
    EntityManager em(log, registry);

    const EntityId lead = em.spawn("air", xform(0, 0, 0));
    const EntityId wing = em.spawn("air", xform(10, 0, 0));
    REQUIRE(lead.valid());
    REQUIRE(wing.valid());

    FormationRegistry formations;
    const FormationId fid = formations.create("Viper", lead, kNoPeer);
    REQUIRE(fid != kNoFormation);
    FormationMember member;
    member.id = wing;
    REQUIRE(formations.addMember(fid, member));

    const WorldStateSnapshot snap =
        buildWorldStateSnapshot(1, em, registry, &formations, nullptr, {}, WorldStateEnvironment{}, nullptr);
    REQUIRE(snap.entities.size() == 2u);
    // The wingman is in the formation; a bare entity with no formation reports kNoFormation.
    bool sawWingInFormation = false;
    for (const auto& e : snap.entities) {
        if (e.entityIdx == wing.index) {
            CHECK(e.formationId == fid);
            sawWingInFormation = true;
        }
    }
    CHECK(sawWingInFormation);
}

TEST_CASE("WorldState: peers are sorted ascending by peerId", "[world_state]") {
    NullLog log;
    EntityTypeRegistry registry;
    EntityManager em(log, registry);

    std::vector<WorldStatePeer> peers;
    peers.push_back(WorldStatePeer{5, 1, 3, 0});
    peers.push_back(WorldStatePeer{2, 2, 4, 1});
    peers.push_back(WorldStatePeer{9, 0, 0, 0});

    const WorldStateSnapshot snap =
        buildWorldStateSnapshot(1, em, registry, nullptr, nullptr, std::move(peers), WorldStateEnvironment{}, nullptr);
    REQUIRE(snap.peers.size() == 3u);
    CHECK(snap.peers[0].peerId == 2);
    CHECK(snap.peers[1].peerId == 5);
    CHECK(snap.peers[2].peerId == 9);
    CHECK(snap.peers[0].delayTicks == 4);
    CHECK(snap.peers[0].role == 1); // Observer ordinal preserved
}

// ── enrichment + JSON (#600) ────────────────────────────────────────────────────────────────────

TEST_CASE("world state carries the faction table, postures and the relationship matrix", "[world_state]") {
    NullLog log;
    EntityTypeRegistry registry;
    registry.registerType(makeDef("f22", ObjectCategory::AirVehicle));
    EntityManager em(log, registry);

    FactionRegistry factions;
    std::vector<FactionDef> defs;
    defs.push_back(FactionDef{"", "(neutral)"});
    defs.push_back(FactionDef{"nato", "NATO"});
    defs.push_back(FactionDef{"russia", "Russia", AlertLevel::Conflict});
    factions.load(std::move(defs));
    factions.setRelationship(1, 2, FactionRelation::Hostile);

    const WorldStateSnapshot snap =
        buildWorldStateSnapshot(1, em, registry, nullptr, &factions, {}, WorldStateEnvironment{}, nullptr);

    REQUIRE(snap.factions.size() == 3);
    // Ascending index order, so the emitted document is the same on every run.
    CHECK(snap.factions[0].factionIndex == 0);
    CHECK(snap.factions[1].id == "nato");
    CHECK(snap.factions[2].name == "Russia");
    CHECK(snap.factions[2].alertLevel == static_cast<uint8_t>(AlertLevel::Conflict));

    REQUIRE(snap.relationships.size() == 9);
    CHECK(snap.relationship(1, 2) == FactionRelation::Hostile);
    CHECK(snap.relationship(2, 1) == FactionRelation::Hostile); // symmetric
    CHECK(snap.relationship(1, 1) == FactionRelation::Friendly);
    CHECK(snap.relationship(9, 0) == FactionRelation::Neutral); // out of range is not a crash
}

TEST_CASE("world state without a faction registry or mission omits those blocks", "[world_state]") {
    NullLog log;
    EntityTypeRegistry registry;
    EntityManager em(log, registry);

    const WorldStateSnapshot snap =
        buildWorldStateSnapshot(1, em, registry, nullptr, nullptr, {}, WorldStateEnvironment{}, nullptr);
    CHECK(snap.factions.empty());
    CHECK(snap.relationships.empty());
    CHECK_FALSE(snap.mission.active);

    // A sandbox server still produces a valid document rather than a padded one.
    const std::string json = toJson(snap);
    CHECK(json.find("\"factions\": []") != std::string::npos);
    CHECK(json.find("\"relationships\": []") != std::string::npos);
    CHECK(json.find("\"active\": false") != std::string::npos);
}

TEST_CASE("world state carries wind and mission state", "[world_state]") {
    NullLog log;
    EntityTypeRegistry registry;
    EntityManager em(log, registry);

    WorldStateEnvironment env;
    env.weatherPreset = 4;
    env.timeOfDayHours = 6.25f;
    env.windX = -12.5f;
    env.windZ = 3.f;

    WorldStateMission mission;
    mission.active = true;
    mission.name = "Storm Warning";
    mission.outcome = 1;
    mission.triggersFired = 3;
    mission.elapsedSeconds = 42.5;

    const WorldStateSnapshot snap = buildWorldStateSnapshot(7, em, registry, nullptr, nullptr, {}, env, &mission);
    CHECK(snap.windX == -12.5f);
    CHECK(snap.windZ == 3.f);
    CHECK(snap.mission.name == "Storm Warning");
    CHECK(snap.mission.triggersFired == 3);
}

TEST_CASE("WorldState: sweep_deg defaults to zero and survives the JSON round trip (#1195)", "[world_state]") {
    // Wing sweep is the first flight-model quantity on this surface, and it is here because until
    // #1195 the angle was readable NOWHERE outside the integrator — so "does this aircraft's sweep
    // follow its Mach schedule" could only be answered by an in-process C++ test, which is what
    // blocked fl-base-pack#66's own acceptance criterion.
    //
    // buildWorldStateSnapshot cannot fill it: it sees the entity pool, and the integrators belong to
    // WorldBroadcaster, which stamps them on afterwards. So the invariant THIS test owns is that the
    // pure builder leaves it at a defined zero for everything — which is also the honest value for
    // an entity with no [wing_sweep] table, and for a ground vehicle or a missile.
    NullLog log;
    EntityTypeRegistry registry;
    registry.registerType(makeDef("f22", ObjectCategory::AirVehicle));
    EntityManager em(log, registry);
    em.spawn("f22", xform(1.0, 2.0, 3.0));

    WorldStateSnapshot snap =
        buildWorldStateSnapshot(1, em, registry, nullptr, nullptr, {}, WorldStateEnvironment{}, nullptr);
    REQUIRE(snap.entities.size() == 1u);
    CHECK(snap.entities[0].sweepDeg == 0.f);
    CHECK(toJson(snap).find("\"sweep_deg\": 0") != std::string::npos);

    // And a stamped angle reaches the document unrounded to the degree — a reader compares it
    // against the schedule detents the model publishes (the B-1B's are 15/25/55/67.5).
    snap.entities[0].sweepDeg = 67.5f;
    CHECK(toJson(snap).find("\"sweep_deg\": 67.5") != std::string::npos);
}

TEST_CASE("world-state JSON is schema-stable and escapes faction names", "[world_state][json]") {
    NullLog log;
    EntityTypeRegistry registry;
    registry.registerType(makeDef("f22", ObjectCategory::AirVehicle));
    EntityManager em(log, registry);
    em.spawn("f22", xform(100.0, 200.0, 300.0));

    FactionRegistry factions;
    std::vector<FactionDef> defs;
    defs.push_back(FactionDef{"", "(neutral)"});
    // A pack-supplied display name is mod-controlled text; it must not be able to break the document.
    defs.push_back(FactionDef{"nato", "NATO \"North\"", AlertLevel::Elevated});
    factions.load(std::move(defs));

    WorldStateEnvironment env;
    env.weatherPreset = 2;
    env.timeOfDayHours = 14.5f;

    const WorldStateSnapshot snap = buildWorldStateSnapshot(4242, em, registry, nullptr, &factions, {}, env, nullptr);
    const std::string json = toJson(snap);

    // Key presence, not a whole-blob compare: the format is additive, so a new field must not fail
    // this test, but a RENAMED or REMOVED one must.
    for (const char* key : {"\"tick\": 4242", "\"weather_preset\": 2", "\"time_of_day_hours\"", "\"wind\"",
                            "\"mission\"", "\"factions\"", "\"relationships\"", "\"peers\"", "\"entities\"",
                            "\"alert_level\": 1", "\"idx\"", "\"pos\"", "\"hp_frac\"", "\"sweep_deg\""})
        CHECK(json.find(key) != std::string::npos);

    CHECK(json.find("NATO \\\"North\\\"") != std::string::npos);

    // Indented form nests inside a larger document and stays well-formed.
    const std::string nested = toJson(snap, 4);
    CHECK(nested.find("    {") == 0);
}

TEST_CASE("world-state JSON is deterministic for a fixed entity set", "[world_state][json]") {
    // The property the golden-JSON claim rests on: build twice, get byte-identical output.
    auto build = [] {
        NullLog log;
        EntityTypeRegistry registry;
        registry.registerType(makeDef("f22", ObjectCategory::AirVehicle));
        registry.registerType(makeDef("sam", ObjectCategory::GroundVehicle));
        EntityManager em(log, registry);
        em.spawn("f22", xform(1.0, 2.0, 3.0));
        em.spawn("sam", xform(4.0, 5.0, 6.0));
        em.spawn("f22", xform(7.0, 8.0, 9.0));

        FactionRegistry factions;
        std::vector<FactionDef> defs;
        defs.push_back(FactionDef{"", "(neutral)"});
        defs.push_back(FactionDef{"nato", "NATO"});
        defs.push_back(FactionDef{"russia", "Russia"});
        factions.load(std::move(defs));
        factions.setRelationship(1, 2, FactionRelation::Hostile);

        std::vector<WorldStatePeer> peers;
        peers.push_back(WorldStatePeer{9, 2, 5, 0});
        peers.push_back(WorldStatePeer{3, 1, 2, 0});

        return toJson(buildWorldStateSnapshot(99, em, registry, nullptr, &factions, std::move(peers),
                                              WorldStateEnvironment{1, 12.f, 0.5f, -0.5f}, nullptr));
    };

    CHECK(build() == build());
    // And peers really are sorted, so an unordered_map iteration order upstream cannot leak in.
    const std::string json = build();
    CHECK(json.find("\"peer_id\": 3") < json.find("\"peer_id\": 9"));
}
