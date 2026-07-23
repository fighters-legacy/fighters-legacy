// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/WorldState.h"

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
        buildWorldStateSnapshot(42, em, registry, /*formations=*/nullptr, {}, /*weatherPreset=*/3,
                                /*timeOfDayHours=*/14.5f);

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
    const WorldStateSnapshot snap2 = buildWorldStateSnapshot(42, em, registry, nullptr, {}, 3, 14.5f);
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
    const WorldStateSnapshot snap = buildWorldStateSnapshot(1, em, registry, nullptr, {}, 0, 12.f);
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

    const WorldStateSnapshot snap = buildWorldStateSnapshot(1, em, registry, &formations, {}, 0, 12.f);
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

    const WorldStateSnapshot snap = buildWorldStateSnapshot(1, em, registry, nullptr, std::move(peers), 0, 12.f);
    REQUIRE(snap.peers.size() == 3u);
    CHECK(snap.peers[0].peerId == 2);
    CHECK(snap.peers[1].peerId == 5);
    CHECK(snap.peers[2].peerId == 9);
    CHECK(snap.peers[0].delayTicks == 4);
    CHECK(snap.peers[0].role == 1); // Observer ordinal preserved
}
